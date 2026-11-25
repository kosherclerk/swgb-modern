// Debug diagnostics: log per-thread CPU time and owning module of thread start.
#include "../common/common.h"
#include <tlhelp32.h>
#include <psapi.h>

namespace swgbm {
void LogThreadReport() {
    typedef LONG(WINAPI * NQIT)(HANDLE, int, PVOID, ULONG, PULONG);
    auto q = (NQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof(te)};
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        void* start = nullptr;
        if (q) q(th, 9, &start, sizeof(start), nullptr);
        FILETIME c, e, k, u;
        GetThreadTimes(th, &c, &e, &k, &u);
        double ms = ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10000.0;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)start, &mod))
            GetModuleBaseNameA(GetCurrentProcess(), mod, name, MAX_PATH);
        LOG("thread %5lu prio %3d cpu %8.0f ms start %p (%s+%lx)", te.th32ThreadID, GetThreadPriority(th), ms, start, name,
            (unsigned long)((uintptr_t)start - (uintptr_t)mod));
        CloseHandle(th);
    }
    CloseHandle(snap);
}
}
