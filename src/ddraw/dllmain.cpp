#include "ddraw_impl.h"
#include "window.h"
#include "presenter.h"
#include "patches.h"

using namespace swgbm;

static bool g_init = false;

static void Init() {
    if (g_init) return;
    g_init = true;
    GameExe exe = DetectExe();
    LOG("SWGB Modern ddraw loaded into %s build", ExeName(exe));
    Window::InstallHooks();
    Patches::Apply(exe);
}

extern "C" {

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // ddraw.dll is imported by the exe, so this runs before any game code:
        // safe to patch the image here. IAT hooks & patches only touch memory.
        Init();
    } else if (reason == DLL_PROCESS_DETACH) {
        // Process is exiting; don't wait on threads here.
    }
    return TRUE;
}

HRESULT WINAPI DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) {
    Init();
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new DDraw();
    LOG("DirectDrawCreate");
    return DD_OK;
}

HRESULT WINAPI DirectDrawCreateEx(GUID*, LPVOID* out, REFIID, IUnknown*) {
    LOG("DirectDrawCreateEx requested - only DirectDraw 1 is emulated");
    if (out) *out = nullptr;
    return DDERR_UNSUPPORTED;
}

HRESULT WINAPI DirectDrawCreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new DDClipper();
    return DD_OK;
}

typedef BOOL(WINAPI* EnumCbA)(GUID*, LPSTR, LPSTR, LPVOID);
typedef BOOL(WINAPI* EnumCbW)(GUID*, LPWSTR, LPWSTR, LPVOID);
typedef BOOL(WINAPI* EnumCbExA)(GUID*, LPSTR, LPSTR, LPVOID, HMONITOR);

HRESULT WINAPI DirectDrawEnumerateA(EnumCbA cb, LPVOID ctx) {
    if (cb) cb(nullptr, (LPSTR) "Primary Display Driver", (LPSTR) "display", ctx);
    return DD_OK;
}
HRESULT WINAPI DirectDrawEnumerateW(EnumCbW cb, LPVOID ctx) {
    if (cb) cb(nullptr, (LPWSTR)L"Primary Display Driver", (LPWSTR)L"display", ctx);
    return DD_OK;
}
HRESULT WINAPI DirectDrawEnumerateExA(EnumCbExA cb, LPVOID ctx, DWORD) {
    if (cb) cb(nullptr, (LPSTR) "Primary Display Driver", (LPSTR) "display", ctx, nullptr);
    return DD_OK;
}
HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID* p) { if (p) *p = nullptr; return CLASS_E_CLASSNOTAVAILABLE; }

} // extern "C"
