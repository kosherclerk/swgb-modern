// Local crash dumps: on an unhandled exception write a minidump to
// <Game>\CrashDumps\ and a line to the log. Nothing is uploaded anywhere.
// Not installed under Expanding Fronts, which ships its own crash reporter.
#include "crash.h"
#include <dbghelp.h>
#include <time.h>

namespace swgbm {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;
volatile LONG g_inCrash = 0;

typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_inCrash, 1) == 0) {
        std::wstring dir = GameDir() + L"\\CrashDumps";
        CreateDirectoryW(dir.c_str(), nullptr);
        wchar_t name[64];
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_s(&tmv, &t);
        wcsftime(name, 64, L"\\swgb-%Y%m%d-%H%M%S.dmp", &tmv);
        std::wstring path = dir + name;
        HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
        auto write = dbg ? (MiniDumpWriteDumpFn)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        BOOL ok = FALSE;
        if (write && f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), ep, FALSE};
            ok = write(GetCurrentProcess(), GetCurrentProcessId(), f,
                       (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs), &mei, nullptr, nullptr);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        LOG("CRASH: exception %08lx at %p - minidump %s", ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress, ok ? "written to CrashDumps" : "could not be written");
    }
    return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// [Debug] FirstChanceLog: log the first access violations as they happen (with
// module, offset and return addresses found on the stack) and write a dump of
// the first one. For crashes that never reach the unhandled-exception filter.
volatile LONG g_fcCount = 0;
void ModuleOf(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    out[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        char path[MAX_PATH];
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* b = strrchr(path, 92);  // backslash
        snprintf(out, n, "%s+%x", b ? b + 1 : path, (unsigned)(a - (uintptr_t)m));
    }
}
LONG CALLBACK FirstChance(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != 0xC0000374) return EXCEPTION_CONTINUE_SEARCH;
    LONG n = InterlockedIncrement(&g_fcCount);
    if (n > 8) return EXCEPTION_CONTINUE_SEARCH;
    char where[160], start[160] = "";
    ModuleOf((uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    typedef LONG(WINAPI* NQIT)(HANDLE, int, PVOID, ULONG, PULONG);
    auto q = (NQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    uintptr_t st = 0;
    if (q) q(GetCurrentThread(), 9, &st, sizeof(st), nullptr);
    if (st) ModuleOf(st, start, sizeof(start));
    LOG("first-chance %08lx at %p (%s) addr %p thread %lu started at %s", code, ep->ExceptionRecord->ExceptionAddress, where,
        ep->ExceptionRecord->NumberParameters > 1 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr, GetCurrentThreadId(), start);
    uintptr_t* sp = (uintptr_t*)ep->ContextRecord->Esp;
    int found = 0;
    for (int i = 0; i < 2048 && found < 16; i++) {
        uintptr_t v;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
        v = sp[i];
        char m[160];
        ModuleOf(v, m, sizeof(m));
        if (m[0] && !strstr(m, "ntdll") && !strstr(m, "KERNEL")) { LOG("  stack[%d] %08x %s", i, (unsigned)v, m); found++; }
    }
    if (n == 1) Filter(ep);   // minidump of the first one
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void Crash::Install() {
    if (CfgBool("Debug", "FirstChanceLog", false)) AddVectoredExceptionHandler(1, FirstChance);
    if (!CfgBool("Debug", "CrashDumps", true) || DetectExe() == GameExe::ExpandingFronts) return;
    g_prev = SetUnhandledExceptionFilter(Filter);
}

} // namespace swgbm
