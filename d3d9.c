/*
 * Fable: The Lost Chapters - windowed / borderless proxy d3d9.dll
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

/* mouse capture state (see "mouse" section below) */
static volatile LONG g_inSizeMove;   /* user is dragging/resizing the window */
static volatile LONG g_captured;     /* game currently owns the mouse */
static volatile LONG g_mouseWasExclusive;
static void *g_mouseDev;             /* the game's DirectInput mouse device */

static BOOL MouseReleasedByUser(void)
{
    return !g_borderless && (GetAsyncKeyState(VK_MENU) & 0x8000);
}

static void SetMouseCaptured(BOOL captured);

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
    case WM_ACTIVATEAPP:
        if (!wp) SetMouseCaptured(FALSE);   /* switched to another app */
        break;
    case WM_KILLFOCUS:
        SetMouseCaptured(FALSE);
        break;
    case WM_SYSKEYDOWN:
        /* Alt pressed in window mode: free the mouse right away, even if the
           game isn't polling it at the moment. */
        if (wp == VK_MENU && !g_borderless) SetMouseCaptured(FALSE);
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
    case WM_SYSCOMMAND:
        /* A bare Alt tap would enter the (non-existent) menu loop and freeze
           the game until the next key; Alt is our mouse-release key now. */
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;
        /* Moving/resizing/min/max always goes to Windows, not the game. */
        if (!g_borderless) {
            switch (wp & 0xFFF0) {
            case SC_MOVE: case SC_SIZE: case SC_MINIMIZE: case SC_MAXIMIZE: case SC_RESTORE:
                return DefWindowProcW(h, msg, wp, lp);
            }
        }
        break;
    case WM_NCHITTEST:
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        /* Window frame (title bar, borders): handled by Windows directly. */
        if (!g_borderless) return DefWindowProcW(h, msg, wp, lp);
        break;
    case WM_LBUTTONDOWN:
        /* Alt + drag anywhere inside the window moves it. */
        if (!g_captured && MouseReleasedByUser()) {
            ReleaseCapture();
            SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (!g_captured) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
                return TRUE;
            }
            if (g_mouseWasExclusive) {      /* exclusive mode never showed it */
                SetCursor(NULL);
                return TRUE;
            }
        }
        break;
    case WM_ENTERSIZEMOVE:
        InterlockedExchange(&g_inSizeMove, 1);
        break;
    case WM_EXITSIZEMOVE:
        InterlockedExchange(&g_inSizeMove, 0);
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

static int g_cursorShowCount;   /* ShowCursor(TRUE) calls we owe back */

static void ReleaseCursorToUser(void)
{
    ClipCursor(NULL);
    if (g_cursorShowCount == 0) {
        int n = ShowCursor(TRUE);
        g_cursorShowCount = 1;
        while (n < 0 && g_cursorShowCount < 64) {
            n = ShowCursor(TRUE);
            g_cursorShowCount++;
        }
    }
}

static void ReturnCursorToGame(void)
{
    while (g_cursorShowCount > 0) {
        ShowCursor(FALSE);
        g_cursorShowCount--;
    }
}

static void SetMouseCaptured(BOOL captured)
{
    if (captured == (BOOL)g_captured) return;
    InterlockedExchange(&g_captured, captured);
    if (captured) ReturnCursorToGame(); else ReleaseCursorToUser();
    Log("Mouse %s", captured ? "captured by game" : "released to user");
}

/* Called at the start of every mouse poll. Returns TRUE if the game should
   receive empty mouse data this time. */
static BOOL UpdateMouseCapture(void)
{
    HWND h = g_hwnd;
    BOOL wasCaptured = g_captured;
    BOOL want = FALSE;
    RECT rc = { 0 };

    if (h && GetForegroundWindow() == h && !IsIconic(h) &&
        !g_inSizeMove && !MouseReleasedByUser()) {
        GetClientRect(h, &rc);
        MapWindowPoints(h, NULL, (POINT *)&rc, 2);
        /* Like modern windowed games: take the mouse only once the cursor
           is inside the game area. Coming back with Alt+Tab while the
           cursor is on the title bar or a border leaves it free, so the
           window can be dragged or resized straight away. */
        POINT pt;
        GetCursorPos(&pt);
        want = wasCaptured || PtInRect(&rc, pt);
    }

    SetMouseCaptured(want);
    if (want) ClipCursor(&rc);
    /* Not captured, or just recaptured: discard what queued up meanwhile
       (e.g. the click on the title bar) so the camera doesn't jump. */
    return !want || !wasCaptured;
}

/* IDirectInputDevice8 vtable: 9 GetDeviceState, 10 GetDeviceData,
   13 SetCooperativeLevel. The vtable is shared by keyboard and mouse
   devices, so every hook checks for the mouse first. */
typedef HRESULT (STDMETHODCALLTYPE *GetDeviceStateFn)(void *, DWORD, void *);
typedef HRESULT (STDMETHODCALLTYPE *GetDeviceDataFn)(void *, DWORD, void *, DWORD *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SetCoopLevelFn)(void *, HWND, DWORD);
static GetDeviceStateFn g_origGetDeviceState;
static GetDeviceDataFn g_origGetDeviceData;
static SetCoopLevelFn g_origSetCoopLevel;

#define PROXY_DISCL_EXCLUSIVE    0x1
#define PROXY_DISCL_NONEXCLUSIVE 0x2
#define PROXY_DISCL_FOREGROUND   0x4
#define PROXY_DISCL_BACKGROUND   0x8

static HRESULT STDMETHODCALLTYPE HookGetDeviceState(void *dev, DWORD size, void *data)
{
    HRESULT hr = g_origGetDeviceState(dev, size, data);
    if (dev == g_mouseDev && UpdateMouseCapture() && SUCCEEDED(hr) && data)
        memset(data, 0, size);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookGetDeviceData(void *dev, DWORD objSize, void *data,
                                                   DWORD *count, DWORD flags)
{
    HRESULT hr = g_origGetDeviceData(dev, objSize, data, count, flags);
    if (dev == g_mouseDev && UpdateMouseCapture() && SUCCEEDED(hr) && count)
        *count = 0;  /* events were read (drained) but the game sees none */
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookSetCoopLevel(void *dev, HWND h, DWORD flags)
{
    if (dev == g_mouseDev && (flags & PROXY_DISCL_EXCLUSIVE)) {
        InterlockedExchange(&g_mouseWasExclusive, 1);
        DWORD fixed = (flags & ~PROXY_DISCL_EXCLUSIVE) | PROXY_DISCL_NONEXCLUSIVE;
        Log("Mouse SetCooperativeLevel 0x%lx -> 0x%lx", flags, fixed);
        flags = fixed;
    }
    return g_origSetCoopLevel(dev, h, flags);
}

/* IDirectInput8::CreateDevice (vtable 3) */
typedef HRESULT (STDMETHODCALLTYPE *DICreateDeviceFn)(void *, const GUID *, void **, void *);
static DICreateDeviceFn g_origDICreateDevice;

static const GUID kGuidSysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

static HRESULT STDMETHODCALLTYPE HookDICreateDevice(void *di, const GUID *guid, void **out, void *outer)
{
    HRESULT hr = g_origDICreateDevice(di, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out && guid && IsEqualGUID(guid, &kGuidSysMouse)) {
        g_mouseDev = *out;
        void **vtbl = *(void ***)*out;
        PatchVtable(vtbl, 9, (void *)HookGetDeviceState, (void **)&g_origGetDeviceState);
        PatchVtable(vtbl, 10, (void *)HookGetDeviceData, (void **)&g_origGetDeviceData);
        PatchVtable(vtbl, 13, (void *)HookSetCoopLevel, (void **)&g_origSetCoopLevel);
        Log("Hooked DirectInput mouse device %p", *out);
    }
    return hr;
}

typedef HRESULT (WINAPI *DirectInput8CreateFn)(HINSTANCE, DWORD, const IID *, void **, void *);
static DirectInput8CreateFn g_origDirectInput8Create;

static HRESULT WINAPI HookDirectInput8Create(HINSTANCE inst, DWORD ver, const IID *iid,
                                             void **out, void *outer)
{
    HRESULT hr = g_origDirectInput8Create(inst, ver, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void **vtbl = *(void ***)*out;
        PatchVtable(vtbl, 3, (void *)HookDICreateDevice, (void **)&g_origDICreateDevice);
        Log("Hooked DirectInput8Create -> %p", *out);
    }
    return hr;
}

typedef BOOL (WINAPI *SetCursorPosFn)(int, int);
static SetCursorPosFn g_origSetCursorPos;

static BOOL WINAPI HookSetCursorPos(int x, int y)
{
    if (!g_captured) return TRUE;  /* don't yank the cursor away from the user */
    return g_origSetCursorPos(x, y);
}

/* Replace one imported function in Fable.exe's import table. */
static void PatchImport(const char *dll, const char *func, void *hook, void **orig)
{
    HMODULE target = GetModuleHandleA(dll);
    void *real = target ? (void *)GetProcAddress(target, func) : NULL;
    if (!real || *orig) return;

    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;

    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
         d->Name; d++) {
        if (lstrcmpiA((const char *)(base + d->Name), dll) != 0) continue;
        for (IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + d->FirstThunk); t->u1.Function; t++) {
            if ((void *)t->u1.Function != real) continue;
            DWORD old;
            VirtualProtect(&t->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            *orig = real;
            t->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&t->u1.Function, sizeof(void *), old, &old);
            Log("Patched import %s!%s", dll, func);
            return;
        }
    }
}

static void InstallInputHooks(void)
{
    PatchImport("dinput8.dll", "DirectInput8Create",
                (void *)HookDirectInput8Create, (void **)&g_origDirectInput8Create);
    PatchImport("user32.dll", "SetCursorPos",
                (void *)HookSetCursorPos, (void **)&g_origSetCursorPos);
}

/* ------------------------------------------------------------- exports */

__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    typedef IDirect3D9 *(WINAPI *Fn)(UINT);
    Fn real = (Fn)RealProc("Direct3DCreate9");
    IDirect3D9 *d3d = real ? real(sdk) : NULL;
    Log("Direct3DCreate9(%u) -> %p", sdk, (void *)d3d);
    InstallInputHooks();  /* no-op if DllMain already did it */
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
        InstallInputHooks();
    }
    return TRUE;
}
