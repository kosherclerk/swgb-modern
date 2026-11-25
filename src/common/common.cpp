#include "common.h"
#include <stdarg.h>
#include <stdlib.h>
#include <share.h>

namespace swgbm {

const std::wstring& GameDir() {
    static std::wstring dir = [] {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
        std::wstring s(buf, n);
        size_t p = s.find_last_of(L"\\/");
        return p == std::wstring::npos ? std::wstring(L".") : s.substr(0, p);
    }();
    return dir;
}

std::wstring ConfigPath() { return GameDir() + L"\\SWGBModern.ini"; }

static std::wstring Widen(const char* s) {
    wchar_t buf[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 512);
    return n > 0 ? std::wstring(buf) : std::wstring();
}

std::string CfgStr(const char* section, const char* key, const char* def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(Widen(section).c_str(), Widen(key).c_str(), Widen(def).c_str(), buf, 512,
                             ConfigPath().c_str());
    // strip inline comments ("value ; comment")
    std::wstring w(buf);
    size_t c = w.find_first_of(L";#");
    if (c != std::wstring::npos) w = w.substr(0, c);
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\t')) w.pop_back();
    char out[512];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, 512, nullptr, nullptr);
    return out;
}

int CfgInt(const char* section, const char* key, int def) {
    std::string s = CfgStr(section, key, "");
    if (s.empty()) return def;
    return atoi(s.c_str());
}

float CfgFloat(const char* section, const char* key, float def) {
    std::string s = CfgStr(section, key, "");
    if (s.empty()) return def;
    return (float)atof(s.c_str());
}

bool CfgBool(const char* section, const char* key, bool def) {
    std::string s = CfgStr(section, key, "");
    if (s.empty()) return def;
    char c = s[0];
    return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y' || (s.size() > 1 && (s[1] == 'n' || s[1] == 'N'));
}

static CRITICAL_SECTION g_logLock;
static FILE* g_log = nullptr;
static int g_logState = -1; // -1 unknown, 0 off, 1 on

bool LogEnabled() {
    if (g_logState < 0) {
        g_logState = CfgBool("Debug", "Log", false) ? 1 : 0;
        if (g_logState) {
            InitializeCriticalSection(&g_logLock);
            std::wstring p = GameDir() + L"\\SWGBModern.log";
            // ddraw.dll starts a fresh log; dsound.dll appends. Shared append-mode
            // handles keep both DLLs' lines intact.
            wchar_t self[MAX_PATH] = L"";
            HMODULE hm = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&LogEnabled, &hm);
            GetModuleFileNameW(hm, self, MAX_PATH);
            if (wcsstr(_wcslwr(self), L"ddraw.dll")) { FILE* t = _wfopen(p.c_str(), L"w"); if (t) fclose(t); }
            g_log = _wfsopen(p.c_str(), L"a", _SH_DENYNO);
            if (!g_log) g_logState = 0;
        }
    }
    return g_logState == 1;
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    fprintf(g_log, "[%8lu] ", GetTickCount());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

GameExe DetectExe() {
    static GameExe cached = [] {
        auto base = (const uint8_t*)GetModuleHandleW(nullptr);
        auto dos = (const IMAGE_DOS_HEADER*)base;
        auto nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
        DWORD ts = nt->FileHeader.TimeDateStamp;
        DWORD soi = nt->OptionalHeader.SizeOfImage;
        DWORD ep = nt->OptionalHeader.AddressOfEntryPoint;
        if (ts == 0x3cf57142 && soi == 0x3b1000 && ep == 0x236601) return GameExe::CloneCampaigns;
        if (ts == 0x3c03fd69 && soi == 0x3af000 && ep == 0x232881) return GameExe::BaseGame;
        // Expanding Fronts: battlegrounds_x2.exe is the CC 1.1 image (same link
        // timestamp) with an extra ".patch" section that loads efpatch.dll.
        // GameRanger users rename it to battlegrounds_x1.exe, so don't rely on
        // the file name.
        std::wstring efpatch = GameDir() + L"\\efpatch.dll";
        bool hasEfPatch = GetFileAttributesW(efpatch.c_str()) != INVALID_FILE_ATTRIBUTES;
        auto sec = IMAGE_FIRST_SECTION(nt);
        bool hasPatchSection = false;
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (memcmp(sec[i].Name, ".patch", 6) == 0) hasPatchSection = true;
        if (ts == 0x3cf57142 && (hasPatchSection || (soi != 0x3b1000 && hasEfPatch))) return GameExe::ExpandingFronts;
        return GameExe::Unknown;
    }();
    return cached;
}

const char* ExeName(GameExe e) {
    switch (e) {
    case GameExe::BaseGame: return "Galactic Battlegrounds";
    case GameExe::CloneCampaigns: return "Clone Campaigns";
    case GameExe::ExpandingFronts: return "Expanding Fronts";
    default: return "Unknown";
    }
}

} // namespace swgbm
