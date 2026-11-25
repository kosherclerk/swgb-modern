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

} // namespace

void Crash::Install() {
    if (!CfgBool("Debug", "CrashDumps", true) || DetectExe() == GameExe::ExpandingFronts) return;
    g_prev = SetUnhandledExceptionFilter(Filter);
}

} // namespace swgbm
