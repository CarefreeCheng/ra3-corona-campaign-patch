// xinput1_3.dll 代理（32 位）v1
// 选它的理由：RA3 的导入表里有 xinput1_3.dll，它不是 KnownDLL，
// 而且只有 7 个导出、只用于手柄轮询——即使我们不完美转发，也不会影响键鼠和音频。
// 作用仍然是：等游戏窗口出现后加载 ra3copilot.dll，并自动设好科技开关。
#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <string.h>
#pragma comment(lib, "ws2_32.lib")

typedef DWORD(WINAPI *PFN_Enable)(BOOL);
typedef DWORD(WINAPI *PFN_GetState)(DWORD, void *);
typedef DWORD(WINAPI *PFN_SetState)(DWORD, void *);
typedef DWORD(WINAPI *PFN_GetCaps)(DWORD, DWORD, void *);
typedef DWORD(WINAPI *PFN_GetBattery)(BYTE, DWORD, void *);
typedef DWORD(WINAPI *PFN_GetKeystroke)(DWORD, DWORD, void *);
typedef DWORD(WINAPI *PFN_GetDSoundGuids)(DWORD, GUID *, GUID *);

static HMODULE g_real = NULL;
static HMODULE g_copilot = NULL;
static HANDLE g_ready = NULL;
static char g_dir[MAX_PATH] = {0};
static char g_root[MAX_PATH] = {0};

static void LogLine(const char *fmt, ...)
{
    char path[MAX_PATH];
    lstrcpyA(path, g_dir);
    lstrcatA(path, "\\xinput_proxy.log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    DWORD wrote = 0;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, buf, (DWORD)n, &wrote, NULL);
    WriteFile(h, "\r\n", 2, &wrote, NULL);
    CloseHandle(h);
}

static int HttpPost(const char *path)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(17832);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    int ok = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0)
    {
        char req[512];
        int n = wsprintfA(req,
            "POST %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", path);
        send(s, req, n, 0);
        char buf[1024];
        int r = recv(s, buf, sizeof(buf) - 1, 0);
        if (r > 0) { buf[r] = 0; if (strstr(buf, "ok")) ok = 1; }
    }
    closesocket(s);
    return ok;
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    char title[256] = {0};
    GetWindowTextA(h, title, sizeof(title) - 1);
    if (title[0] == 0) return TRUE;
    *(int *)lp = 1;
    return FALSE;
}

static int HasVisibleWindow(void)
{
    int found = 0;
    EnumWindows(EnumProc, (LPARAM)&found);
    return found;
}

static DWORD WINAPI FlagThread(LPVOID p)
{
    (void)p;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    int done1 = 0, done2 = 0;
    for (int i = 0; i < 240 && !(done1 && done2); i++)
    {
        if (!done1) { done1 = HttpPost("/toggle?f=0x1D&on=1"); if (done1) LogLine("[flags] 0x1D=1 ok"); }
        if (!done2) { done2 = HttpPost("/toggle?f=0x1E&on=0"); if (done2) LogLine("[flags] 0x1E=0 ok"); }
        if (!(done1 && done2)) Sleep(1500);
    }
    LogLine("[flags] done1=%d done2=%d", done1, done2);
    WSACleanup();
    return 0;
}

/* 从 DLL 同目录的 xinput_proxy.cfg 读 delay_ms=N；读不到就用默认 20000 */
static int ReadDelayMs(void)
{
    const char *dirs[2];
    char path[MAX_PATH];
    dirs[0] = g_dir; dirs[1] = g_root;
    for (int i = 0; i < 2; i++)
    {
        if (!dirs[i] || !dirs[i][0]) continue;
        lstrcpyA(path, dirs[i]);
        lstrcatA(path, "\\xinput_proxy.cfg");
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        char buf[512] = {0};
        DWORD got = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(h);
        char *p = strstr(buf, "delay_ms");
        if (p)
        {
            p += 8;
            while (*p == ' ' || *p == '=' || *p == '\t') p++;
            int v = atoi(p);
            if (v >= 0 && v <= 180000) return v;
        }
    }
    return 45000;
}

static DWORD WINAPI SurviveThread(LPVOID p)
{
    (void)p;
    Sleep(6000);
    LogLine("[init] survived 6s after copilot load (render thread seems healthy)");
    return 0;
}

static DWORD WINAPI InitThread(LPVOID p)
{
    (void)p;
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    lstrcatA(sys, "\\xinput1_3.dll");
    g_real = LoadLibraryA(sys);
    LogLine("[xinput] real -> %p", g_real);
    LogLine("[xinput] thread up, waiting for game window (max 60s)");
    if (g_ready) SetEvent(g_ready);

    /* 尽早挂上只读探针：记录 CommandSet 查表的 key（不改行为） */
    {
        char probe[MAX_PATH];
        lstrcpyA(probe, g_dir);
        lstrcatA(probe, "\\csprobe.dll");
        HMODULE gp = LoadLibraryA(probe);
        LogLine("[init] csprobe(%s) -> %p (err=%lu)", probe, gp, gp ? 0 : GetLastError());
    }

    int waited = 0;
    while (waited < 120 && !HasVisibleWindow()) { Sleep(500); waited++; }
    LogLine("[init] window=%d after %d ms", HasVisibleWindow(), waited * 500);
    int delay = ReadDelayMs();
    LogLine("[init] delay_ms=%d (cfg)", delay);
    {
        int slept = 0;
        while (slept < delay)
        {
            int step = (delay - slept > 5000) ? 5000 : (delay - slept);
            Sleep(step);
            slept += step;
            LogLine("[wait] %d/%d ms", slept, delay);
        }
    }

    char cop[MAX_PATH];
    lstrcpyA(cop, g_dir);
    lstrcatA(cop, "\\ra3copilot.dll");
    LogLine("[init] loading copilot %s", cop);
    g_copilot = LoadLibraryA(cop);
    if (!g_copilot)
    {
        lstrcpyA(cop, g_root);
        lstrcatA(cop, "\\ra3copilot.dll");
        g_copilot = LoadLibraryA(cop);
    }
    LogLine("[init] ra3copilot(%s) -> %p (err=%lu)", cop, g_copilot, g_copilot ? 0 : GetLastError());
    if (g_copilot)
    {
        HANDLE t = CreateThread(NULL, 0, FlagThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        HANDLE t2 = CreateThread(NULL, 0, SurviveThread, NULL, 0, NULL);
        if (t2) CloseHandle(t2);

        /* 拉起补丁助手：把建造按钮的产出类型批量换成 _Enhanced 版（扩展建筑自带的升级按钮） */
        {
            char hp[MAX_PATH];
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            lstrcpyA(hp, g_dir);
            lstrcatA(hp, "\\ra3fix_helper.exe");
            if (GetFileAttributesA(hp) == INVALID_FILE_ATTRIBUTES)
            {
                lstrcpyA(hp, g_root);
                lstrcatA(hp, "\\ra3fix_helper.exe");
            }
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));
            if (CreateProcessA(hp, NULL, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi))
            {
                LogLine("[init] helper spawned: %s", hp);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            else
            {
                LogLine("[init] helper spawn FAILED err=%lu (%s)", GetLastError(), hp);
            }
        }
    }
    return 0;
}

static HMODULE RealModule(void)
{
    if (!g_real && g_ready) WaitForSingleObject(g_ready, 20000);
    return g_real;
}

DWORD WINAPI XInputEnable(BOOL enable)
{
    PFN_Enable f = (PFN_Enable)GetProcAddress(RealModule(), "XInputEnable");
    if (f) return f(enable);
    return 0;
}
DWORD WINAPI XInputGetState(DWORD idx, void *state)
{
    PFN_GetState f = (PFN_GetState)GetProcAddress(RealModule(), "XInputGetState");
    return f ? f(idx, state) : 1167;   // ERROR_DEVICE_NOT_CONNECTED
}
DWORD WINAPI XInputSetState(DWORD idx, void *vib)
{
    PFN_SetState f = (PFN_SetState)GetProcAddress(RealModule(), "XInputSetState");
    return f ? f(idx, vib) : 1167;
}
DWORD WINAPI XInputGetCapabilities(DWORD idx, DWORD flags, void *caps)
{
    PFN_GetCaps f = (PFN_GetCaps)GetProcAddress(RealModule(), "XInputGetCapabilities");
    return f ? f(idx, flags, caps) : 1167;
}
DWORD WINAPI XInputGetBatteryInformation(BYTE idx, BYTE dev, void *b)
{
    PFN_GetBattery f = (PFN_GetBattery)GetProcAddress(RealModule(), "XInputGetBatteryInformation");
    return f ? f(idx, dev, b) : 1167;
}
DWORD WINAPI XInputGetKeystroke(DWORD idx, DWORD reserved, void *ks)
{
    PFN_GetKeystroke f = (PFN_GetKeystroke)GetProcAddress(RealModule(), "XInputGetKeystroke");
    return f ? f(idx, reserved, ks) : 1167;
}
DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD idx, GUID *render, GUID *capture)
{
    PFN_GetDSoundGuids f = (PFN_GetDSoundGuids)GetProcAddress(RealModule(), "XInputGetDSoundAudioDeviceGuids");
    return f ? f(idx, render, capture) : 1167;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        char *slash = strrchr(g_dir, '\\');
        if (slash) *slash = 0;
        lstrcpyA(g_root, g_dir);
        if (!lstrcmpiA(strrchr(g_root, 0) - 4, "Data"))
        {
            slash = strrchr(g_root, '\\');
            if (slash) *slash = 0;
        }
        g_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
