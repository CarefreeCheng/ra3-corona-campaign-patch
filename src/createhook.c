// createhook.c --- "born as enhanced" patch for RA3 + Corona campaign.
//
// Problem: the command bar of a building is fixed when the building is CREATED.
// Changing the ObjectType of an existing building later does not add buttons.
// Fix: intercept the proto lookup by hash (0x007E4230) and rewrite the lookup key:
//        hash("SovietPowerPlantAdvanced")  ->  hash("SovietPowerPlantAdvanced_Enhanced")
// so the game creates the enhanced variant from the very beginning.
//
// The pair table is built at load time by scanning our own process memory for
// "<name>_Enhanced" strings; no external file and no copilot API needed.
//
// Hook style: entry inline hook, 7 byte prologue (sub esp,8 / push esi / lea esi,[ecx+28h]),
// jmp back to 0x007E4237. The stub only rewrites the single stack argument
// ([esp+4] on entry) when it exactly matches a known base hash - so if anything
// is unexpected it simply behaves like the original function.
//
// ASCII only on purpose (msvc + whatever code page).
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define FN_ENTRY   0x007E4230u
#define FN_BODY    0x007E4237u
#define MAX_PAIRS  512

static unsigned int  g_base[MAX_PAIRS];
static unsigned int  g_enh[MAX_PAIRS];
static int           g_count;
static char          g_dir[MAX_PATH];
static unsigned char *g_stubp;
static int           g_stublen;
static CRITICAL_SECTION g_cs;
static volatile LONG g_hits;

static void Log(const char *fmt, ...)
{
    char path[MAX_PATH + 32];
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    sprintf(path, "%s\\createhook.log", g_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fclose(f);
}

// SAGE SuperFastHash (matches unit-codes/tools/sage_hash.py)
static unsigned int SageHash(const char *s)
{
    unsigned char b[256];
    int n = (int)strlen(s);
    if (n == 0 || n > 255) return 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        b[i] = (unsigned char)c;
    }
    int blocks = n / 4, extra = n % 4, idy = 0;
    unsigned int h = (unsigned int)n;
    for (int k = 0; k < blocks; k++) {
        h += (unsigned int)((b[idy + 1] << 8) | b[idy]);
        unsigned int tmp = (unsigned int)(((b[idy + 3] << 8) | b[idy + 2]) ^ (h << 5));
        h ^= tmp << 11;
        h += h >> 11;
        idy += 4;
    }
    if (extra == 1) { h += b[idy]; h = (h << 10) ^ h; h += h >> 1; }
    else if (extra == 2) { h += (unsigned int)((b[idy + 1] << 8) | b[idy]); h ^= h << 11; h += h >> 17; }
    else if (extra == 3) { h += (unsigned int)((b[idy + 1] << 8) | b[idy]); h ^= ((h ^ (unsigned int)(b[idy + 2] << 2)) << 16); h += h >> 11; }
    h ^= h << 3; h += h >> 5; h ^= h << 2; h += h >> 15; h ^= h << 10;
    return h;
}

static int AddPair(const char *base, const char *enh)
{
    if (g_count >= MAX_PAIRS) return 0;
    unsigned int hb = SageHash(base);
    unsigned int he = SageHash(enh);
    if (!hb || !he) return 0;
    for (int i = 0; i < g_count; i++) if (g_base[i] == hb) return 0;
    g_base[g_count] = hb;
    g_enh[g_count] = he;
    g_count++;
    return 1;
}

// scan committed memory of our own process for identifiers ending in "_Enhanced"
static void BuildPairs(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = (unsigned char *)0x10000;
    const char *suffix = "_Enhanced";
    int slen = 9;
    unsigned char *buf = (unsigned char *)malloc(4 << 20);
    if (!buf) { Log("build: no memory"); return; }

    int found = 0;
    while ((unsigned int)addr < 0x7FFF0000u) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        if (mbi.RegionSize == 0) break;
        int readable = (mbi.State == MEM_COMMIT) && !(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS);
        if (readable) {
            size_t size = mbi.RegionSize, off = 0;
            while (off < size) {
                size_t n = size - off; if (n > (4u << 20)) n = (4u << 20);
                memcpy(buf, addr + off, n);
                for (size_t i = 0; i + slen < n; i++) {
                    if (memcmp(buf + i, suffix, slen) != 0) continue;
                    // buffer ends here (next char not identifier char)
                    unsigned char nx = (i + slen < n) ? buf[i + slen] : 0;
                    if ((nx >= 'a' && nx <= 'z') || (nx >= 'A' && nx <= 'Z') ||
                        (nx >= '0' && nx <= '9') || nx == '_') continue;
                    // walk back over identifier chars
                    size_t s = i;
                    while (s > 0) {
                        unsigned char c = buf[s - 1];
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') s--;
                        else break;
                    }
                    size_t len = (i + slen) - s;
                    if (len < 12 || len > 200) continue;
                    char name[256];
                    memcpy(name, buf + s, len);
                    name[len] = 0;
                    int ok = 1;
                    for (size_t k = 0; k < len; k++) {
                        unsigned char c = (unsigned char)name[k];
                        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) { ok = 0; break; }
                    }
                    if (!ok || strchr(name, '.') || strchr(name, ' ')) continue;
                    strcpy(name + len - slen, "");
                    if (strlen(name) < 3) continue;
                    if (AddPair(name, name)) found++;
                    // AddPair needs full enhanced name; rebuild it
                    g_count--;               // undo, then add properly
                    (void)found;
                }
                off += n;
            }
        }
        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    free(buf);
    Log("build: scanned, pairs=%d", g_count);
}

// fixed version: derive both names explicitly
static void BuildPairs2(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = (unsigned char *)0x10000;
    const char *suffix = "_Enhanced";
    int slen = 9;
    unsigned char *buf = (unsigned char *)malloc(4 << 20);
    if (!buf) return;
    g_count = 0;

    while ((unsigned int)addr < 0x7FFF0000u) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        if (mbi.RegionSize == 0) break;
        int readable = (mbi.State == MEM_COMMIT) && !(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS);
        if (readable) {
            size_t size = mbi.RegionSize, off = 0;
            while (off < size) {
                size_t n = size - off; if (n > (4u << 20)) n = (4u << 20);
                memcpy(buf, addr + off, n);
                for (size_t i = 0; i + slen < n; i++) {
                    if (memcmp(buf + i, suffix, slen) != 0) continue;
                    unsigned char nx = (i + slen < n) ? buf[i + slen] : 0;
                    if ((nx >= 'a' && nx <= 'z') || (nx >= 'A' && nx <= 'Z') || (nx >= '0' && nx <= '9') || nx == '_') continue;
                    size_t s = i;
                    while (s > 0) {
                        unsigned char c = buf[s - 1];
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') s--;
                        else break;
                    }
                    size_t len = (i + slen) - s;
                    if (len < 12 || len > 200) continue;
                    char enh[256];
                    memcpy(enh, buf + s, len);
                    enh[len] = 0;
                    int ok = 1;
                    for (size_t k = 0; k < len; k++) {
                        unsigned char c = (unsigned char)enh[k];
                        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) { ok = 0; break; }
                    }
                    if (!ok) continue;
                    char base[256];
                    memcpy(base, enh, len - slen);
                    base[len - slen] = 0;
                    if (strlen(base) < 3) continue;
                    AddPair(base, enh);
                }
                off += n;
            }
        }
        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    free(buf);
    Log("build: pairs=%d", g_count);
    for (int i = 0; i < g_count && i < 12; i++) Log("  pair %08X -> %08X", g_base[i], g_enh[i]);
}

// called from the stub: returns possibly rewritten key
static unsigned int __cdecl PatchKey(unsigned int key)
{
    for (int i = 0; i < g_count; i++) {
        if (g_base[i] == key) {
            if (InterlockedIncrement(&g_hits) <= 40)
                Log("key %08X -> %08X", key, g_enh[i]);
            return g_enh[i];
        }
    }
    return key;
}

static void StubTarget(void) { }

static void BuildStub(void)
{
    g_stubp = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stubp) { Log("stub alloc failed"); return; }
    unsigned char *p = g_stubp;
    unsigned int stubAddr = (unsigned int)(DWORD_PTR)StubTarget;

    *p++ = 0x60;                                   // pushad
    *p++ = 0x9C;                                   // pushfd
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x28;   // mov eax,[esp+0x28]
    *p++ = 0x50;                                   // push eax
    *p++ = 0xE8;                                   // call PatchKey
    {
        unsigned int rel = (unsigned int)(DWORD_PTR)&PatchKey - ((unsigned int)(DWORD_PTR)p + 4);
        memcpy(p, &rel, 4); p += 4;
    }
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;         // add esp,4
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x28;   // mov [esp+0x28],eax
    *p++ = 0x9D;                                   // popfd
    *p++ = 0x61;                                   // popad
    *p++ = 0x83; *p++ = 0xEC; *p++ = 0x08;         // sub esp,8
    *p++ = 0x56;                                   // push esi
    *p++ = 0x8D; *p++ = 0x71; *p++ = 0x28;         // lea esi,[ecx+28h]
    *p++ = 0xE9;                                   // jmp FN_BODY
    {
        unsigned int rel = FN_BODY - ((unsigned int)p + 4);
        memcpy(p, &rel, 4); p += 4;
    }
    g_stublen = (int)(p - g_stubp);
    (void)stubAddr;
}

static DWORD WINAPI Worker(LPVOID param)
{
    (void)param;
    InitializeCriticalSection(&g_cs);
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, '\\');
    if (slash) *slash = 0;

    Log("createhook attach, dir=%s", g_dir);
    // mod data is loaded progressively: keep rescanning until the _Enhanced
    // identifiers show up (up to 5 minutes), then hook once.
    for (int round = 0; round < 150; round++) {
        BuildPairs2();
        if (g_count > 0) break;
        Sleep(2000);
    }
    if (g_count == 0) { Log("no pairs after wait, hook skipped"); return 0; }
    Log("pairs ready after wait: %d", g_count);

    BuildStub();
    unsigned char *target = (unsigned char *)FN_ENTRY;
    DWORD old;
    if (!VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &old)) {
        Log("VirtualProtect failed %lu", GetLastError());
        return 0;
    }
    unsigned char patch[7];
    patch[0] = 0xE9;
    unsigned int rel = (unsigned int)(DWORD_PTR)g_stubp - (FN_ENTRY + 5);
    memcpy(patch + 1, &rel, 4);
    patch[5] = 0x90; patch[6] = 0x90;
    memcpy(target, patch, 7);
    VirtualProtect(target, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 16);
    Log("hooked %08X -> stub %08X (stub len %d)", FN_ENTRY, (unsigned int)(DWORD_PTR)g_stubp, g_stublen);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
