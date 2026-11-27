// Window mode handling and input translation.
//
// Coordinate model: the game sees a virtual display of GameW x GameH whose
// origin is screen (0,0) and its window covering exactly that. Every user32
// call the game makes that exposes real coordinates is translated through the
// viewport the presenter actually draws the image into.
#include "window.h"
#include "presenter.h"
#include "hook.h"
#include "capture.h"
#include <windowsx.h>
#include <algorithm>

namespace swgbm {
namespace {

HWND g_hwnd = nullptr;
WNDPROC g_gameProc = nullptr;
int g_gw = 800, g_gh = 600;
bool g_windowed = false;
bool g_lockCursor = true;
bool g_active = true;
bool g_idleFix = true;
volatile LONG g_flipCounter = 0;

decltype(&GetCursorPos) pGetCursorPos = GetCursorPos;
decltype(&SetCursorPos) pSetCursorPos = SetCursorPos;
decltype(&ClipCursor) pClipCursor = ClipCursor;
decltype(&GetClientRect) pGetClientRect = GetClientRect;
decltype(&GetWindowRect) pGetWindowRect = GetWindowRect;
decltype(&ClientToScreen) pClientToScreen = ClientToScreen;
decltype(&ScreenToClient) pScreenToClient = ScreenToClient;
decltype(&SetWindowPos) pSetWindowPos = SetWindowPos;
decltype(&MoveWindow) pMoveWindow = MoveWindow;
decltype(&GetSystemMetrics) pGetSystemMetrics = GetSystemMetrics;
decltype(&PeekMessageA) pPeekMessageA = PeekMessageA;
decltype(&WindowFromPoint) pWindowFromPoint = WindowFromPoint;
decltype(&CreateWindowExA) pCreateWindowExA = CreateWindowExA;
decltype(&SendMessageA) pSendMessageA = SendMessageA;
decltype(&SetTimer) pSetTimer = SetTimer;

// The engine redraws the game world from a 50 ms WM_TIMER (timer id 1), i.e.
// 20 frames per second. The simulation itself advances by elapsed time, so a
// shorter interval only adds frames (smoother scrolling/animation) without
// changing game speed.
UINT_PTR WINAPI H_SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn) {
    LOG("SetTimer(hwnd=%p, id=%u, %u ms, proc=%p) from %p", h, (unsigned)id, ms, fn, __builtin_return_address(0));
    if (h && id == 1 && ms == 50 && !fn && DetectExe() != GameExe::ExpandingFronts) {
        int fps = CfgInt("Fixes", "GameFPS", 60);
        if (fps >= 20 && fps <= 240) {
            UINT n = (UINT)(1000 / fps);
            LOG("game redraw timer: %u ms -> %u ms (%d fps)", ms, n, fps);
            ms = n;
        }
    }
    return pSetTimer(h, id, ms, fn);
}

// Fonts given to child controls are scaled with the viewport so native EDIT
// text matches the size of the upscaled image.
HFONT ScaledFont(HFONT f) {
    struct Entry { HFONT src; int scale1000; HFONT dst; };
    static Entry cache[64];
    static int n = 0;
    Viewport v = Presenter::Get().CurrentViewport();
    if (!f || g_gh <= 0 || v.h <= 0) return f;
    int s1000 = (int)((int64_t)v.h * 1000 / g_gh);
    if (s1000 <= 1000) return f;
    for (int i = 0; i < n; i++) if (cache[i].src == f && cache[i].scale1000 == s1000) return cache[i].dst;
    LOGFONTA lf;
    if (!GetObjectA(f, sizeof(lf), &lf)) return f;
    lf.lfHeight = (LONG)((int64_t)lf.lfHeight * s1000 / 1000);
    lf.lfWidth = (LONG)((int64_t)lf.lfWidth * s1000 / 1000);
    lf.lfQuality = CLEARTYPE_QUALITY;
    HFONT d = CreateFontIndirectA(&lf);
    if (!d) return f;
    if (n < 64) cache[n++] = {f, s1000, d};
    return d;
}

bool IsGameChild(HWND h) { return g_hwnd && h && h != g_hwnd && GetParent(h) == g_hwnd && (GetWindowLongW(h, GWL_STYLE) & WS_CHILD); }

// Child controls (EDIT boxes for chat/names) are created in game coordinates;
// place them over the scaled image so clicks and caret land where drawn.
void MapChildRect(int& x, int& y, int& w, int& h) {
    int x2 = x + w, y2 = y + h;
    Window::GameToClient(x, y);
    Window::GameToClient(x2, y2);
    w = x2 - x; h = y2 - y;
}

RECT ViewportScreenRect() {
    Viewport v = Presenter::Get().CurrentViewport();
    POINT p = {v.x, v.y};
    pClientToScreen(g_hwnd, &p);
    return {p.x, p.y, p.x + v.w, p.y + v.h};
}

void ApplyCursorClip() {
    if (!g_hwnd) return;
    if (g_lockCursor && g_active && GetForegroundWindow() == g_hwnd) {
        RECT r = ViewportScreenRect();
        pClipCursor(&r);
    } else {
        pClipCursor(nullptr);
    }
}

void ApplyWindowMode() {
    if (!g_hwnd) return;
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (!g_windowed) {
        SetWindowLongW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowLongW(g_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        RECT r = mi.rcMonitor;
        pSetWindowPos(g_hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        int ww = CfgInt("Display", "WindowWidth", 0), wh = CfgInt("Display", "WindowHeight", 0);
        RECT work = mi.rcWork;
        int mw = work.right - work.left, mh = work.bottom - work.top;
        if (ww <= 0 || wh <= 0) {
            // largest integer multiple of the game size that fits, else ~80% of work area
            int s = std::max(1, std::min((mw - 40) / g_gw, (mh - 80) / g_gh));
            ww = g_gw * s; wh = g_gh * s;
            if (ww > mw || wh > mh || s == 1 && (g_gw * 1.3 < mw * 0.6)) { double f = std::min(mw * 0.8 / g_gw, mh * 0.8 / g_gh); ww = (int)(g_gw * f); wh = (int)(g_gh * f); }
        }
        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN;
        SetWindowLongW(g_hwnd, GWL_STYLE, style);
        SetWindowLongW(g_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        RECT r = {0, 0, ww, wh};
        AdjustWindowRectEx(&r, style, FALSE, WS_EX_APPWINDOW);
        int x = work.left + (mw - (r.right - r.left)) / 2, y = work.top + (mh - (r.bottom - r.top)) / 2;
        pSetWindowPos(g_hwnd, HWND_NOTOPMOST, x, y, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    ApplyCursorClip();
}

// --------------------------------------------------------------- hooks
BOOL WINAPI H_GetCursorPos(LPPOINT p) {
    if (!pGetCursorPos(p)) return FALSE;
    if (g_hwnd) {
        pScreenToClient(g_hwnd, p);
        int x = p->x, y = p->y;
        Window::ClientToGame(x, y);
        p->x = x; p->y = y;
    }
    return TRUE;
}
BOOL WINAPI H_SetCursorPos(int x, int y) {
    if (g_hwnd) {
        Window::GameToClient(x, y);
        POINT p = {x, y};
        pClientToScreen(g_hwnd, &p);
        x = p.x; y = p.y;
    }
    return pSetCursorPos(x, y);
}
BOOL WINAPI H_ClipCursor(const RECT* r) {
    // The game clips to its "screen". We manage clipping ourselves.
    (void)r;
    ApplyCursorClip();
    return TRUE;
}
BOOL WINAPI H_GetClientRect(HWND h, LPRECT r) {
    if (h && h == g_hwnd && r) { *r = {0, 0, g_gw, g_gh}; return TRUE; }
    return pGetClientRect(h, r);
}
BOOL WINAPI H_GetWindowRect(HWND h, LPRECT r) {
    if (h && h == g_hwnd && r) { *r = {0, 0, g_gw, g_gh}; return TRUE; }
    if (r && IsGameChild(h)) {
        if (!pGetWindowRect(h, r)) return FALSE;
        POINT a = {r->left, r->top}, b = {r->right, r->bottom};
        pScreenToClient(g_hwnd, &a); pScreenToClient(g_hwnd, &b);
        int ax = a.x, ay = a.y, bx = b.x, by = b.y;
        Window::ClientToGame(ax, ay); Window::ClientToGame(bx, by);
        *r = {ax, ay, bx, by};
        return TRUE;
    }
    return pGetWindowRect(h, r);
}
BOOL WINAPI H_ClientToScreen(HWND h, LPPOINT p) {
    if (h && h == g_hwnd) return TRUE;
    return pClientToScreen(h, p);
}
BOOL WINAPI H_ScreenToClient(HWND h, LPPOINT p) {
    if (h && h == g_hwnd) return TRUE;
    return pScreenToClient(h, p);
}
BOOL WINAPI H_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f) {
    if (h && h == g_hwnd) return TRUE; // we own the main window geometry
    if (IsGameChild(h)) {
        if ((f & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) {
            RECT r; pGetWindowRect(h, &r);
            POINT p = {r.left, r.top}; pScreenToClient(g_hwnd, &p);
            int gx = x, gy = y, gw = cx, gh = cy;
            MapChildRect(gx, gy, gw, gh);
            if (f & SWP_NOMOVE) { gx = p.x; gy = p.y; }
            if (f & SWP_NOSIZE) { gw = r.right - r.left; gh = r.bottom - r.top; }
            return pSetWindowPos(h, after, gx, gy, gw, gh, f & ~(SWP_NOMOVE | SWP_NOSIZE));
        }
    }
    return pSetWindowPos(h, after, x, y, cx, cy, f);
}
BOOL WINAPI H_MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL rp) {
    if (h && h == g_hwnd) return TRUE;
    if (IsGameChild(h)) MapChildRect(x, y, cx, cy);
    return pMoveWindow(h, x, y, cx, cy, rp);
}
HWND WINAPI H_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                              HMENU menu, HINSTANCE inst, LPVOID param) {
    if (g_hwnd && parent == g_hwnd && (style & WS_CHILD) && x != (int)CW_USEDEFAULT) MapChildRect(x, y, w, h);
    if (!parent && !(style & WS_CHILD)) {
        // A window's DPI awareness is fixed by the creating thread's context.
        // Mods such as Expanding Fronts switch the thread to DPI-unaware, which
        // makes Windows bitmap-scale (and crop) our full-resolution output on
        // scaled displays. Create top-level windows per-monitor aware.
        typedef HANDLE(WINAPI * SetCtx)(HANDLE);
        static auto setCtx = (SetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
        HANDLE prev = setCtx ? setCtx((HANDLE)-4) : nullptr;
        HWND r = pCreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
        if (setCtx && prev) setCtx(prev);
        LOG("top-level window %p created per-monitor DPI aware (previous thread context %p)", r, prev);
        return r;
    }
    return pCreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

LRESULT WINAPI H_SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETFONT && IsGameChild(h)) wp = (WPARAM)ScaledFont((HFONT)wp);
    return pSendMessageA(h, msg, wp, lp);
}

int WINAPI H_GetSystemMetrics(int i) {
    if (g_hwnd) {
        if (i == SM_CXSCREEN || i == SM_CXFULLSCREEN || i == SM_CXVIRTUALSCREEN) return g_gw;
        if (i == SM_CYSCREEN || i == SM_CYFULLSCREEN || i == SM_CYVIRTUALSCREEN) return g_gh;
    }
    return pGetSystemMetrics(i);
}
HWND WINAPI H_WindowFromPoint(POINT p) {
    if (g_hwnd) {
        int x = p.x, y = p.y;
        Window::GameToClient(x, y);
        POINT q = {x, y};
        pClientToScreen(g_hwnd, &q);
        return pWindowFromPoint(q);
    }
    return pWindowFromPoint(p);
}

// Idle fix: the engine spins PeekMessage forever. If the game peeks with an
// empty queue and has not presented a frame since the previous empty peek, it
// has nothing to do: yield until input arrives or 1 ms passes. Gameplay frames
// are paced by Flip() and never hit this path, so the simulation is unaffected.
LONG g_lastFlipSeen = -1;
DWORD g_idleWaitMs = 2;
BOOL WINAPI H_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT rm) {
    BOOL r = pPeekMessageA(m, h, a, b, rm);
    if (!r && g_idleFix) {
        LONG fc = g_flipCounter;
        if (fc == g_lastFlipSeen) MsgWaitForMultipleObjects(0, nullptr, FALSE, g_idleWaitMs, QS_ALLINPUT);
        g_lastFlipSeen = fc;
    }
    return r;
}

LPARAM MapMouseLParam(LPARAM lp) {
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    Window::ClientToGame(x, y);
    return MAKELPARAM((short)x, (short)y);
}

LRESULT CALLBACK SubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (Capture::IsHotkeyMessage(msg, wp)) return 0;   // the recorder polls the key itself
    if (msg == WM_TIMER && wp == 1 && LogEnabled()) {
        static DWORD n = 0, t0 = GetTickCount();
        n++;
        if (GetTickCount() - t0 >= 5000) { LOG("WM_TIMER(1): %.1f/s", n * 1000.0 / (GetTickCount() - t0)); n = 0; t0 = GetTickCount(); }
    }
    if (msg == WM_APP + 0x5157) { // settings changed on disk
        g_windowed = CfgStr("Display", "Mode", "borderless") == "windowed";
        g_lockCursor = CfgBool("Display", "LockCursor", true);
        ApplyWindowMode();
        return 0;
    }
    switch (msg) {
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        lp = MapMouseLParam(lp);
        break;
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        pScreenToClient(h, &p);
        int x = p.x, y = p.y;
        Window::ClientToGame(x, y);
        lp = MAKELPARAM((short)x, (short)y);
        break;
    }
    case WM_SIZE: case WM_MOVE: case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED:
    case WM_GETMINMAXINFO: case WM_NCCALCSIZE: case WM_SIZING:
        // Geometry is ours; don't let the engine react to real window sizes.
        if (msg == WM_WINDOWPOSCHANGED || msg == WM_SIZE) { Presenter::Get().FrameReady(false); ApplyCursorClip(); }
        return DefWindowProcA(h, msg, wp, lp);
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATEAPP:
        g_active = wp != 0;
        ApplyCursorClip();
        break;
    case WM_ACTIVATE:
        ApplyCursorClip();
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { // Alt+Enter toggles windowed/borderless
            g_windowed = !g_windowed;
            ApplyWindowMode();
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MAXIMIZE && g_windowed) { g_windowed = false; ApplyWindowMode(); return 0; }
        break;
    }
    return CallWindowProcA(g_gameProc, h, msg, wp, lp);
}

} // namespace

void Window::InstallHooks() {
    // Sharp output on scaled (125%/150%) desktops: opt into per-monitor DPI.
    if (auto fn = (BOOL(WINAPI*)(HANDLE))GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        fn((HANDLE)-4 /*PER_MONITOR_AWARE_V2*/);
    g_windowed = CfgStr("Display", "Mode", "borderless") == "windowed";
    g_lockCursor = CfgBool("Display", "LockCursor", true);
    g_idleFix = CfgBool("Fixes", "IdleCPU", true);
    g_idleWaitMs = (DWORD)std::max(1, std::min(16, CfgInt("Fixes", "IdleWaitMs", 2)));
    struct H { const char* name; void* fn; void** orig; } hooks[] = {
        {"GetCursorPos", (void*)H_GetCursorPos, (void**)&pGetCursorPos},
        {"SetCursorPos", (void*)H_SetCursorPos, (void**)&pSetCursorPos},
        {"ClipCursor", (void*)H_ClipCursor, (void**)&pClipCursor},
        {"GetClientRect", (void*)H_GetClientRect, (void**)&pGetClientRect},
        {"GetWindowRect", (void*)H_GetWindowRect, (void**)&pGetWindowRect},
        {"ClientToScreen", (void*)H_ClientToScreen, (void**)&pClientToScreen},
        {"ScreenToClient", (void*)H_ScreenToClient, (void**)&pScreenToClient},
        {"SetWindowPos", (void*)H_SetWindowPos, (void**)&pSetWindowPos},
        {"MoveWindow", (void*)H_MoveWindow, (void**)&pMoveWindow},
        {"GetSystemMetrics", (void*)H_GetSystemMetrics, (void**)&pGetSystemMetrics},
        {"PeekMessageA", (void*)H_PeekMessageA, (void**)&pPeekMessageA},
        {"WindowFromPoint", (void*)H_WindowFromPoint, (void**)&pWindowFromPoint},
        {"CreateWindowExA", (void*)H_CreateWindowExA, (void**)&pCreateWindowExA},
        {"SendMessageA", (void*)H_SendMessageA, (void**)&pSendMessageA},
        {"SetTimer", (void*)H_SetTimer, (void**)&pSetTimer},
    };
    for (auto& hk : hooks) {
        void* o = HookIAT(nullptr, "USER32.dll", hk.name, hk.fn);
        if (o) *hk.orig = o;
        LOG("hook %s: %s", hk.name, o ? "ok" : "not imported");
    }
}

void Window::Attach(HWND hwnd) {
    if (g_hwnd == hwnd) return;
    g_hwnd = hwnd;
    g_gameProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
    ApplyWindowMode();
}

void Window::SetGameSize(int w, int h) {
    g_gw = w; g_gh = h;
    ApplyWindowMode();
}

void Window::Relayout() { ApplyWindowMode(); }
HWND Window::Hwnd() { return g_hwnd; }
bool Window::IsActive() { return g_active; }

void Window::ClientToGame(int& x, int& y) {
    Viewport v = Presenter::Get().CurrentViewport();
    if (v.w <= 0 || v.h <= 0) return;
    x = (int)((int64_t)(x - v.x) * g_gw / v.w);
    y = (int)((int64_t)(y - v.y) * g_gh / v.h);
    x = std::max(0, std::min(g_gw - 1, x));
    y = std::max(0, std::min(g_gh - 1, y));
}

void Window::GameToClient(int& x, int& y) {
    Viewport v = Presenter::Get().CurrentViewport();
    if (g_gw <= 0 || g_gh <= 0) return;
    x = v.x + (int)(((int64_t)x * v.w + v.w / 2) / g_gw);
    y = v.y + (int)(((int64_t)y * v.h + v.h / 2) / g_gh);
}

void Window::NotifyFlip() { InterlockedIncrement(&g_flipCounter); }

} // namespace swgbm
