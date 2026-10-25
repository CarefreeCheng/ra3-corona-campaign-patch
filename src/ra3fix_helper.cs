// ra3fix_helper.cs --- auto patch helper for Red Alert 3 / Corona campaign play.
// ASCII only on purpose: the file is compiled by csc with whatever console code page,
// and non-ASCII literals kept getting mangled when the file was rewritten by scripts.
//
// What it does:
//   1. waits for the injected copilot HTTP API, turns on 0x1D (extended build page)
//      and turns off 0x1E (which bypasses the tech tree and lets you build T4 for free)
//   2. walks the builder CommandSets and swaps every build button's output type to
//      the "_Enhanced" variant, so new buildings really are the extended versions
//   3. scans the heap for already placed buildings (Thing vtable 0x00BED890) and swaps
//      their type too -- no owner filter, so engineer-captured enemy buildings work too
//   4. stays alive and, when a trigger file shows up, spawns ra3dump_cs.exe
//      (the child inherits the game's elevated token, which is why we can read memory)
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

class Ra3FixHelper
{
    const string API = "http://127.0.0.1:17832";
    const uint THING_VT = 0x00BED890;
    const string TRIGGER = @"D:\一次尝试\copilot\dump_on.txt";

    static string logPath;

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    [StructLayout(LayoutKind.Sequential)]
    struct MBI
    {
        public IntPtr BaseAddress, AllocationBase;
        public uint AllocationProtect;
        public IntPtr RegionSize;
        public uint State, Protect, Type;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr addr, out MBI mbi, IntPtr len);

    static void Log(string s)
    {
        string line = DateTime.Now.ToString("HH:mm:ss.fff") + " " + s;
        Console.WriteLine(line);
        try { File.AppendAllText(logPath, line + "\r\n", Encoding.UTF8); } catch { }
    }

    static string Get(string path)
    {
        try
        {
            var req = (HttpWebRequest)WebRequest.Create(API + path);
            req.Timeout = 8000;
            using (var r = (HttpWebResponse)req.GetResponse())
            using (var sr = new StreamReader(r.GetResponseStream(), Encoding.UTF8))
                return sr.ReadToEnd();
        }
        catch (Exception e) { return "ERR " + e.Message; }
    }

    static string Post(string path)
    {
        try
        {
            var req = (HttpWebRequest)WebRequest.Create(API + path);
            req.Method = "POST";
            req.Timeout = 10000;
            req.ContentLength = 0;
            using (var r = (HttpWebResponse)req.GetResponse())
            using (var sr = new StreamReader(r.GetResponseStream(), Encoding.UTF8))
                return sr.ReadToEnd();
        }
        catch (Exception e) { return "ERR " + e.Message; }
    }

    static IntPtr hProc;

    static byte[] ReadMem(uint addr, int n)
    {
        var o = new byte[n];
        IntPtr got;
        if (hProc == IntPtr.Zero) return null;
        if (!ReadProcessMemory(hProc, (IntPtr)addr, o, (IntPtr)n, out got) || (long)got < n) return null;
        return o;
    }

    static uint U32(byte[] b, int off)
    {
        if (b == null || b.Length < off + 4) return 0;
        return BitConverter.ToUInt32(b, off);
    }

    // ObjectType + 0x1C -> char* name
    static string TypeName(uint tpl)
    {
        if (tpl < 0x10000) return null;
        var b = ReadMem(tpl + 0x1C, 4);
        if (b == null) return null;
        uint p = U32(b, 0);
        if (p < 0x10000) return null;
        var s = ReadMem(p, 96);
        if (s == null) return null;
        int z = Array.IndexOf(s, (byte)0);
        if (z <= 0) z = s.Length;
        var name = Encoding.ASCII.GetString(s, 0, Math.Min(z, s.Length));
        foreach (var ch in name) if (ch < 32 || ch > 126) return null;
        return name;
    }

    // SAGE SuperFastHash, straight port of unit-codes/tools/sage_hash.py
    static uint SageHash(string value)
    {
        var b = Encoding.UTF8.GetBytes(value.ToLowerInvariant());
        int n = b.Length;
        if (n == 0) return 0;
        int blocks = n / 4, extra = n % 4;
        uint h = (uint)n;
        int idy = 0;
        for (int k = 0; k < blocks; k++)
        {
            h += (uint)((b[idy + 1] << 8) | b[idy]);
            uint tmp = (uint)(((b[idy + 3] << 8) | b[idy + 2]) ^ (h << 5));
            h ^= tmp << 11;
            h += h >> 11;
            idy += 4;
        }
        if (extra == 1)
        {
            h += b[idy];
            h = (h << 10) ^ h;
            h += h >> 1;
        }
        else if (extra == 2)
        {
            h += (uint)((b[idy + 1] << 8) | b[idy]);
            h ^= h << 11;
            h += h >> 17;
        }
        else if (extra == 3)
        {
            h += (uint)((b[idy + 1] << 8) | b[idy]);
            h ^= ((h ^ (uint)(b[idy + 2] << 2)) << 16);
            h += h >> 11;
        }
        h ^= h << 3;
        h += h >> 5;
        h ^= h << 2;
        h += h >> 15;
        h ^= h << 10;
        return h;
    }

    static bool TryResolve(uint hash, out uint tpl)
    {
        tpl = 0;
        string r = Get("/resolve?hash=0x" + hash.ToString("X8"));
        if (r.StartsWith("ERR")) return false;
        var m = Regex.Match(r, @"template\s+([0-9A-Fa-f]{8})");
        if (!m.Success) return false;
        uint v;
        if (!uint.TryParse(m.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v)) return false;
        if (v == 0) return false;
        tpl = v;
        return true;
    }

    static List<uint> BuildersCs()
    {
        var list = new List<uint>();
        string be = Get("/buildext");
        if (!be.Contains("cs=")) return list;
        foreach (Match m in Regex.Matches(be, @"cs=([0-9A-Fa-f]{8})"))
        {
            uint v;
            if (uint.TryParse(m.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v)
                && v != 0 && !list.Contains(v))
                list.Add(v);
        }
        return list;
    }

    // swap build button outputs to the _Enhanced variant
    static void PatchBuildButtons()
    {
        var csList = BuildersCs();
        Log("[build] builder command sets = " + csList.Count);

        var names = new List<string>();
        int scanned = 0, patched = 0;

        foreach (var cs in csList)
        {
            uint inner = U32(ReadMem(cs + 4, 4), 0);
            if (inner < 0x10000) continue;
            var hb = ReadMem(inner + 4, 8);
            if (hb == null) continue;
            uint count = U32(hb, 0), basePtr = U32(hb, 4);
            if (count == 0 || count > 64 || basePtr < 0x10000) continue;

            var arr = ReadMem(basePtr, (int)count * 4);
            if (arr == null) continue;

            for (int i = 0; i < count; i++)
            {
                uint def = U32(arr, i * 4);
                if (def < 0x10000) continue;
                uint sub = U32(ReadMem(def + 4, 4), 0);
                if (sub < 0x10000) continue;
                uint tpl = U32(ReadMem(sub + 0x1C, 4), 0);
                if (tpl < 0x10000) continue;

                string name = TypeName(tpl);
                if (name == null || name.EndsWith("_Enhanced")) continue;
                scanned++;
                if (!names.Contains(name)) names.Add(name);

                uint newTpl;
                if (!TryResolve(SageHash(name + "_Enhanced"), out newTpl)) continue;

                string rr = Post("/poke?addr=0x" + (sub + 0x1C).ToString("X") +
                                 "&type=u32&val=0x" + newTpl.ToString("X8"));
                patched++;
                Log(string.Format("  build {0} -> {0}_Enhanced  sub=0x{1:X8}  0x{2:X8}->0x{3:X8}  {4}",
                        name, sub, tpl, newTpl, rr.Trim()));
            }
        }
        Log("[build] scanned " + scanned + " buttons, patched " + patched);
    }

    // swap already placed buildings (Thing + 0x04 = ObjectType). No owner filter on purpose.
    static void PatchPlaced()
    {
        var procs = Process.GetProcessesByName("ra3_1.12.game");
        if (procs.Length == 0) { Log("[placed] game process not found"); return; }
        if (hProc == IntPtr.Zero) { Log("[placed] no process handle"); return; }

        Thread.Sleep(3000);

        var buf = new byte[2 << 20];
        int found = 0, patched = 0, enemies = 0;
        long addr = 0x10000;

        while (addr < 0x7FFF0000)
        {
            MBI mbi;
            if (VirtualQueryEx(hProc, (IntPtr)addr, out mbi, (IntPtr)Marshal.SizeOf(typeof(MBI))) == IntPtr.Zero) break;
            long size = (long)mbi.RegionSize;
            if (size <= 0) break;
            bool readable = mbi.State == 0x1000 && (mbi.Protect & 0x100) == 0 && (mbi.Protect & 0x01) == 0;
            if (readable)
            {
                long off = 0;
                while (off < size)
                {
                    int n = (int)Math.Min((long)buf.Length, size - off);
                    IntPtr got;
                    if (ReadProcessMemory(hProc, (IntPtr)(addr + off), buf, (IntPtr)n, out got) && (long)got > 0)
                    {
                        int len = (int)got;
                        for (int i = 0; i + 4 <= len; i += 4)
                        {
                            if (BitConverter.ToUInt32(buf, i) != THING_VT) continue;
                            uint thing = (uint)(addr + off + i);
                            found++;
                            uint tpl = U32(ReadMem(thing + 4, 4), 0);
                            if (tpl < 0x10000) continue;
                            string nm = TypeName(tpl);
                            if (nm == null || nm.EndsWith("_Enhanced")) continue;

                            uint newTpl;
                            if (!TryResolve(SageHash(nm + "_Enhanced"), out newTpl)) continue;

                            uint owner = U32(ReadMem(thing + 0x418, 4), 0);
                            string rr = Post("/poke?addr=0x" + (thing + 4).ToString("X") +
                                             "&type=u32&val=0x" + newTpl.ToString("X8"));
                            patched++;
                            Log(string.Format("  placed {0} @0x{1:X8} owner=0x{2:X8} -> _Enhanced  {3}",
                                    nm, thing, owner, rr.Trim()));
                        }
                    }
                    else n = Math.Max(n, 1);
                    off += n;
                }
            }
            addr = (long)mbi.BaseAddress + size;
        }
        Log("[placed] things=" + found + " patched=" + patched + " (enemy buildings included on purpose)");
    }

    static void Main(string[] args)
    {
        logPath = Path.Combine(Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().Location),
                               "ra3fix_helper.log");

        bool up = false;
        for (int t = 0; t < 240 && !up; t++)
        {
            if (Get("/ping").StartsWith("pong")) up = true;
            else Thread.Sleep(500);
        }
        if (!up) { Log("[helper] copilot not up, exit"); return; }
        Log("[helper] copilot online");
        Log("  toggle 0x1D: " + Post("/toggle?f=0x1D&on=1").Trim());
        Log("  toggle 0x1E: " + Post("/toggle?f=0x1E&on=0").Trim());

        var ps = Process.GetProcessesByName("ra3_1.12.game");
        if (ps.Length > 0) hProc = OpenProcess(0x0410, false, ps[0].Id);
        if (hProc == IntPtr.Zero) Log("[helper] OpenProcess failed err=" + Marshal.GetLastWin32Error());

        string be = "";
        for (int t = 0; t < 900; t++)
        {
            be = Get("/buildext");
            if (!be.StartsWith("ERR") && be.Contains("cs=")) break;
            Thread.Sleep(1000);
        }
        if (!be.Contains("cs=")) { Log("[helper] no build page, exit"); return; }

        try { PatchBuildButtons(); } catch (Exception e) { Log("[build] " + e.Message); }
        try { PatchPlaced(); } catch (Exception e) { Log("[placed] " + e.Message); }

        Log("[helper] listening for " + TRIGGER);
        for (int t = 0; t < 100000; t++)
        {
            Thread.Sleep(2000);
            try
            {
                if (!File.Exists(TRIGGER)) continue;
                string tag = "";
                try { tag = File.ReadAllText(TRIGGER, Encoding.UTF8).Trim(); } catch { }
                if (tag.Length == 0) tag = "run";
                try { File.Delete(TRIGGER); } catch { }

                int gpid = 0;
                foreach (var p in Process.GetProcessesByName("ra3_1.12.game")) { gpid = p.Id; break; }
                string dir = Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().Location);
                string exe = Path.Combine(dir, "ra3dump_cs.exe");
                Log("[helper] dump trigger tag=" + tag + " pid=" + gpid);
                var psi = new ProcessStartInfo(exe, "\"" + tag + "\" " + gpid);
                psi.UseShellExecute = false;
                psi.WorkingDirectory = dir;
                Process.Start(psi);
            }
            catch (Exception e) { Log("  dump spawn failed " + e.Message); }
        }
    }
}
