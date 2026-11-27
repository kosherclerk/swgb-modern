// Debug diagnostics: log per-thread CPU time and owning module of thread start.
#include "../common/common.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <mmsystem.h>
#include <map>
#include <vector>
#include <string>
#include <algorithm>

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

// Sampling profiler ([Debug] Profile=1): every millisecond, the instruction
// pointer of each thread of the game is sampled; every 10 s the hottest
// locations (module+offset, per thread) are logged. Offsets in ddraw.dll can
// be looked up in build/out/ddraw.map.
namespace swgbm {
namespace {
struct ProfThread { DWORD id; HANDLE h; std::map<uintptr_t, int> hits; int samples = 0; ULONGLONG lastCpu = 0; bool busy = false; };
ULONGLONG CpuOf(HANDLE h) { FILETIME c, e, k, u; GetThreadTimes(h, &c, &e, &k, &u); return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime); }

DWORD WINAPI ProfilerMain(LPVOID) {
    std::map<DWORD, ProfThread> threads;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId(), lastList = 0, lastDump = GetTickCount();
    for (;;) {
        if (GetTickCount() - lastList > 1000) {
            lastList = GetTickCount();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {sizeof(te)};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || threads.count(te.th32ThreadID)) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (h) threads[te.th32ThreadID] = ProfThread{te.th32ThreadID, h};
            }
            CloseHandle(snap);
            for (auto& kv : threads) { ULONGLONG cpu = CpuOf(kv.second.h); kv.second.busy = cpu - kv.second.lastCpu > 10 * 10000; kv.second.lastCpu = cpu; }   // > 1% of a core
        }
        for (auto& kv : threads) {
            ProfThread& t = kv.second;
            if (!t.busy) continue;
            if (SuspendThread(t.h) == (DWORD)-1) continue;
            CONTEXT c = {}; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(t.h, &c)) { t.hits[c.Eip & ~(uintptr_t)63]++; t.samples++; }
            ResumeThread(t.h);
        }
        if (GetTickCount() - lastDump > 10000) {
            lastDump = GetTickCount();
            for (auto& kv : threads) {
                ProfThread& t = kv.second;
                if (t.samples < 200) { t.hits.clear(); t.samples = 0; continue; }
                // idle = sampled in a wait (ntdll/kernel32/kernelbase/win32u)
                std::vector<std::pair<int, uintptr_t>> v;
                for (auto& h : t.hits) v.push_back({h.second, h.first});
                std::sort(v.rbegin(), v.rend());
                std::string line;
                std::map<std::string, int> mods;
                for (auto& e : v) {
                    HMODULE m = nullptr; char name[64] = "?";
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)e.second, &m))
                        GetModuleBaseNameA(GetCurrentProcess(), m, name, sizeof(name));
                    mods[name] += e.first;
                }
                for (auto& m : mods) { char b[96]; snprintf(b, sizeof(b), " %s %.1f%%", m.first.c_str(), m.second * 100.0 / t.samples); line += b; }
                LOG("profile thread %lu: %d samples;%s", t.id, t.samples, line.c_str());
                line.clear();
                int shown = 0;
                for (auto& e : v) {
                    HMODULE m = nullptr; char name[64] = "?";
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)e.second, &m))
                        GetModuleBaseNameA(GetCurrentProcess(), m, name, sizeof(name));
                    
                    char b[96]; snprintf(b, sizeof(b), " %s+%lx:%.1f%%", name, (unsigned long)(e.second - (uintptr_t)m), e.first * 100.0 / t.samples);
                    line += b;
                    if (++shown >= 60) break;
                }
                LOG("profile thread %lu top:%s", t.id, line.c_str());
                t.hits.clear(); t.samples = 0;
            }
        }
        Sleep(1);
    }
}
} // namespace

void StartProfiler() {
    if (!CfgBool("Debug", "Profile", false)) return;
    CloseHandle(CreateThread(nullptr, 0, ProfilerMain, nullptr, 0, nullptr));
    LOG("profiler: sampling every 1 ms");
}
} // namespace swgbm
