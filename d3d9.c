/*
 * Fable: The Lost Chapters - windowed / borderless proxy d3d9.dll
 *
 * Drop next to Fable.exe. The game loads this instead of the system d3d9.dll.
 *  - Forces every device (CreateDevice/Reset) into D3D windowed mode, so the
 *    game never takes exclusive fullscreen -> Alt+Tab is instant.
 *  - Starts in borderless fullscreen (configurable in d3d9proxy.ini).
 *  - Alt+Enter toggles borderless fullscreen <-> normal window.
 *  - Logs what happens to d3d9proxy.log.
 *
 * Build (32-bit):  see build.bat
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>

/* ---------------------------------------------------------------- logging */

/* Logging is off by default. Build with -DPROXY_LOG to write d3d9proxy.log. */
#ifndef PROXY_LOG
#define Log(...) ((void)0)
#else
static FILE *g_log;
static CRITICAL_SECTION g_logLock;

static void Log(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}
#endif

/* ----------------------------------------------------------------- config */

static wchar_t g_dir[MAX_PATH];
static int g_startBorderless = 1;  /* 1 = borderless fullscreen, 0 = window */
static int g_windowScale = 100;    /* window mode client size, % of backbuffer */

static void LoadConfig(void)
{
    wchar_t ini[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%sd3d9proxy.ini", g_dir);
    g_startBorderless = GetPrivateProfileIntW(L"Window", L"StartBorderless", 1, ini);
    g_windowScale = GetPrivateProfileIntW(L"Window", L"WindowScalePercent", 100, ini);
    if (g_windowScale < 25 || g_windowScale > 400) g_windowScale = 100;
}

/* ------------------------------------------------------------ real d3d9 */

static HMODULE g_real;

static FARPROC RealProc(const char *name)
{
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat(path, L"\\d3d9.dll");
        g_real = LoadLibraryW(path);
        Log("Loaded system d3d9: %p", (void *)g_real);
    }
    return g_real ? GetProcAddress(g_real, name) : NULL;
}

/* ---------------------------------------------------------- window state */

#define WM_PROXY_APPLY (WM_APP + 0x177)

static HWND g_hwnd;
static WNDPROC g_origProc;
static volatile LONG g_borderless;
static UINT g_bbWidth = 1024, g_bbHeight = 768;

static void ApplyWindowMode(void)
{
    HWND h = g_hwnd;
    if (!h) return;

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);

    if (g_borderless) {
        RECT r = mi.rcMonitor;
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongW(h, GWL_EXSTYLE, 0);
        SetWindowPos(h, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        Log("Mode -> borderless fullscreen %ldx%ld", r.right - r.left, r.bottom - r.top);
    } else {
        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        RECT work = mi.rcWork;
        int cw = (int)(g_bbWidth * g_windowScale / 100);
        int ch = (int)(g_bbHeight * g_windowScale / 100);
        RECT r = { 0, 0, cw, ch };
        AdjustWindowRectEx(&r, style, FALSE, 0);
        int w = r.right - r.left, hgt = r.bottom - r.top;
        int x = work.left + ((work.right - work.left) - w) / 2;
        int y = work.top + ((work.bottom - work.top) - hgt) / 2;
        if (y < work.top) y = work.top;
        SetWindowLongW(h, GWL_STYLE, style);
        SetWindowLongW(h, GWL_EXSTYLE, WS_EX_APPWINDOW);
        SetWindowPos(h, HWND_NOTOPMOST, x, y, w, hgt,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        Log("Mode -> window, client %dx%d", cw, ch);
    }
}

static LRESULT CALLBACK ProxyWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SYSKEYDOWN:
        /* Alt+Enter (bit 29 = Alt held, bit 30 = auto-repeat) */
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            if (!(lp & (1 << 30))) {
                InterlockedExchange(&g_borderless, !g_borderless);
                Log("Alt+Enter pressed");
                ApplyWindowMode();
            }
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;  /* no error beep */
        break;
    case WM_PROXY_APPLY:
        ApplyWindowMode();
        return 0;
    }
    return CallWindowProcW(g_origProc, h, msg, wp, lp);
}

static void HookWindow(HWND h)
{
    if (!h || h == g_hwnd) return;
    g_hwnd = h;
    g_origProc = (WNDPROC)SetWindowLongW(h, GWL_WNDPROC, (LONG)ProxyWndProc);
    Log("Subclassed window %p (orig proc %p)", (void *)h, (void *)g_origProc);
}

/* ----------------------------------------------------- present parameters */

static void FixParams(D3DPRESENT_PARAMETERS *pp, const char *who)
{
    Log("%s: game asked %ux%u fmt=%d windowed=%d refresh=%u swap=%d interval=0x%x",
        who, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
        pp->Windowed, pp->FullScreen_RefreshRateInHz, pp->SwapEffect,
        pp->PresentationInterval);
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    if (pp->BackBufferWidth && pp->BackBufferHeight) {
        g_bbWidth = pp->BackBufferWidth;
        g_bbHeight = pp->BackBufferHeight;
    }
}

/* -------------------------------------------------------- device hooks */

typedef HRESULT (STDMETHODCALLTYPE *ResetFn)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static ResetFn g_origReset;

static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    FixParams(pp, "Reset");
    HRESULT hr = g_origReset(dev, pp);
    if (FAILED(hr) && pp->BackBufferFormat != D3DFMT_UNKNOWN) {
        Log("Reset failed 0x%08lx, retrying with desktop format", hr);
        pp->BackBufferFormat = D3DFMT_UNKNOWN;
        hr = g_origReset(dev, pp);
    }
    Log("Reset -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && g_hwnd) PostMessageW(g_hwnd, WM_PROXY_APPLY, 0, 0);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *CreateDeviceFn)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static CreateDeviceFn g_origCreateDevice;

static void PatchVtable(void **vtbl, int index, void *hook, void **orig)
{
    if (vtbl[index] == hook) return;
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void *), old, &old);
}

static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type,
                                                  HWND focus, DWORD flags,
                                                  D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    FixParams(pp, "CreateDevice");
    HWND h = pp->hDeviceWindow ? pp->hDeviceWindow : focus;

    HRESULT hr = g_origCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (FAILED(hr) && pp->BackBufferFormat != D3DFMT_UNKNOWN) {
        Log("CreateDevice failed 0x%08lx, retrying with desktop format", hr);
        pp->BackBufferFormat = D3DFMT_UNKNOWN;
        hr = g_origCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    }
    Log("CreateDevice -> 0x%08lx", hr);

    if (SUCCEEDED(hr) && out && *out) {
        void **vtbl = *(void ***)*out;
        PatchVtable(vtbl, 16, (void *)HookReset, (void **)&g_origReset);  /* IDirect3DDevice9::Reset */
        HookWindow(h);
        PostMessageW(h, WM_PROXY_APPLY, 0, 0);
    }
    return hr;
}

/* ------------------------------------------------------------- exports */

__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    typedef IDirect3D9 *(WINAPI *Fn)(UINT);
    Fn real = (Fn)RealProc("Direct3DCreate9");
    IDirect3D9 *d3d = real ? real(sdk) : NULL;
    Log("Direct3DCreate9(%u) -> %p", sdk, (void *)d3d);
    if (d3d) {
        void **vtbl = *(void ***)d3d;
        PatchVtable(vtbl, 16, (void *)HookCreateDevice, (void **)&g_origCreateDevice);  /* IDirect3D9::CreateDevice */
    }
    return d3d;
}

__declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n)
{
    typedef int (WINAPI *Fn)(D3DCOLOR, LPCWSTR);
    Fn f = (Fn)RealProc("D3DPERF_BeginEvent");
    return f ? f(c, n) : 0;
}

__declspec(dllexport) int WINAPI D3DPERF_EndEvent(void)
{
    typedef int (WINAPI *Fn)(void);
    Fn f = (Fn)RealProc("D3DPERF_EndEvent");
    return f ? f() : 0;
}

__declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n)
{
    typedef void (WINAPI *Fn)(D3DCOLOR, LPCWSTR);
    Fn f = (Fn)RealProc("D3DPERF_SetMarker");
    if (f) f(c, n);
}

__declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n)
{
    typedef void (WINAPI *Fn)(D3DCOLOR, LPCWSTR);
    Fn f = (Fn)RealProc("D3DPERF_SetRegion");
    if (f) f(c, n);
}

__declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    typedef BOOL (WINAPI *Fn)(void);
    Fn f = (Fn)RealProc("D3DPERF_QueryRepeatFrame");
    return f ? f() : FALSE;
}

__declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD o)
{
    typedef void (WINAPI *Fn)(DWORD);
    Fn f = (Fn)RealProc("D3DPERF_SetOptions");
    if (f) f(o);
}

__declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void)
{
    typedef DWORD (WINAPI *Fn)(void);
    Fn f = (Fn)RealProc("D3DPERF_GetStatus");
    return f ? f() : 0;
}

/* --------------------------------------------------------------- DllMain */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameW(NULL, g_dir, MAX_PATH);
        wchar_t *slash = wcsrchr(g_dir, L'\\');
        if (slash) slash[1] = 0;
#ifdef PROXY_LOG
        InitializeCriticalSection(&g_logLock);
        wchar_t logPath[MAX_PATH];
        swprintf(logPath, MAX_PATH, L"%sd3d9proxy.log", g_dir);
        g_log = _wfopen(logPath, L"w");
#endif
        LoadConfig();
        g_borderless = g_startBorderless;
        Log("d3d9 proxy loaded. StartBorderless=%d WindowScalePercent=%d",
            g_startBorderless, g_windowScale);
    }
    return TRUE;
}
