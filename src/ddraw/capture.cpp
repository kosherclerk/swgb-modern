// Diagnostics: frame recorder, hotkey capture and per-second stats (see capture.h).
// The Compression API (XPRESS) needs the Windows 8+ declarations.
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0602
#include "capture.h"
#include "ddraw_impl.h"
#include <compressapi.h>
#include <wincodec.h>
#include <psapi.h>
#include <oleauto.h>
#include <share.h>
#include <deque>
#include <string>
#include <algorithm>
#include <atomic>

namespace swgbm {
namespace {

bool g_on = false;
UINT g_vk = VK_F12;
bool g_ctrl = true, g_shift = false, g_alt = false;
double g_pre = 3, g_post = 3;   // seconds before / after the key press
int g_fps = 30;

double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

// ---------------------------------------------------------------- recorder
struct Raw { double t = 0; int w = 0, h = 0; uint32_t pal[256]; std::vector<uint8_t> px; };
struct Rec { double t = 0; int w = 0, h = 0; bool key = false; uint32_t pal[256]; std::vector<uint8_t> data; };

CRITICAL_SECTION g_qLock;
std::deque<Raw*> g_queue, g_free;           // presenter -> recorder, recycled buffers
HANDLE g_qEvent = nullptr;
std::deque<Rec> g_ring;                      // recorder thread only
std::atomic<double> g_trigger{0};            // key press time (0 = none)
Raw* g_shot = nullptr;                       // frame shown at the key press (owned by the save job)
double g_lastQueued = 0;

struct Job { double t0; Raw* shot; std::vector<Rec> recs; std::vector<std::string> stats; };
DWORD WINAPI SaveThread(LPVOID);

// stats ring for the summary (presenter thread writes, save thread copies under lock)
CRITICAL_SECTION g_statLock;
std::deque<std::string> g_stats;

void Compress(COMPRESSOR_HANDLE c, const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    SIZE_T need = 0;
    out.resize(n + n / 8 + 1024);
    if (!::Compress(c, (PVOID)src, n, out.data(), out.size(), &need)) { out.assign(src, src + n); out.push_back(0xFF); return; }
    out.resize(need);
    out.push_back(0x00);   // trailer: 0 = compressed, 0xFF = stored
    std::vector<uint8_t>(out).swap(out);   // keep only what is used (the scratch size was ~n)
}
bool Decompress(DECOMPRESSOR_HANDLE d, const std::vector<uint8_t>& in, uint8_t* dst, size_t n) {
    if (in.empty()) return false;
    if (in.back() == 0xFF) { if (in.size() - 1 != n) return false; memcpy(dst, in.data(), n); return true; }
    SIZE_T got = 0;
    return ::Decompress(d, (PVOID)in.data(), in.size() - 1, dst, n, &got) && got == n;
}

DWORD WINAPI RecorderThread(LPVOID) {
    COMPRESSOR_HANDLE comp = nullptr;
    CreateCompressor(COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW, nullptr, &comp);
    std::vector<uint8_t> prev, delta;
    int pw = 0, ph = 0;
    double lastKey = -1e9;
    for (;;) {
        WaitForSingleObject(g_qEvent, 100);
        for (;;) {
            Raw* r = nullptr;
            EnterCriticalSection(&g_qLock);
            if (!g_queue.empty()) { r = g_queue.front(); g_queue.pop_front(); }
            LeaveCriticalSection(&g_qLock);
            if (!r) break;
            Rec rec;
            rec.t = r->t; rec.w = r->w; rec.h = r->h;
            memcpy(rec.pal, r->pal, sizeof(rec.pal));
            size_t n = r->px.size();
            rec.key = r->w != pw || r->h != ph || r->t - lastKey >= 1.0 || prev.size() != n;
            if (rec.key) {
                Compress(comp, r->px.data(), n, rec.data);
                lastKey = r->t;
            } else {
                // unchanged areas become zero runs, which XPRESS packs to almost nothing
                delta.resize(n);
                const uint32_t* a = (const uint32_t*)r->px.data();
                const uint32_t* b = (const uint32_t*)prev.data();
                uint32_t* o = (uint32_t*)delta.data();
                size_t n4 = n / 4;
                for (size_t i = 0; i < n4; i++) o[i] = a[i] ^ b[i];
                for (size_t i = n4 * 4; i < n; i++) delta[i] = r->px[i] ^ prev[i];
                Compress(comp, delta.data(), n, rec.data);
            }
            prev.swap(r->px);
            pw = r->w; ph = r->h;
            r->px.resize(n);
            EnterCriticalSection(&g_qLock);
            g_free.push_back(r);
            LeaveCriticalSection(&g_qLock);
            g_ring.push_back(std::move(rec));
        }
        double now = Now(), trig = g_trigger.load();
        // keep the pre-roll (back to its keyframe); while a capture runs, everything since t0 - pre
        double keepFrom = (trig > 0 ? std::min(trig, now) : now) - g_pre - 1.1;
        // drop everything before the newest keyframe that is still older than keepFrom
        size_t cut = 0;
        for (size_t i = 0; i < g_ring.size() && g_ring[i].t <= keepFrom; i++) if (g_ring[i].key) cut = i;
        g_ring.erase(g_ring.begin(), g_ring.begin() + cut);
        if (trig > 0 && now >= trig + g_post) {
            auto* job = new Job;
            job->t0 = trig;
            EnterCriticalSection(&g_qLock);
            job->shot = g_shot; g_shot = nullptr;
            LeaveCriticalSection(&g_qLock);
            for (auto& r : g_ring) if (r.t <= trig + g_post) job->recs.push_back(std::move(r));
            g_ring.clear();
            lastKey = -1e9;   // the next frame starts a fresh keyframe chain
            EnterCriticalSection(&g_statLock);
            job->stats.assign(g_stats.begin(), g_stats.end());
            LeaveCriticalSection(&g_statLock);
            g_trigger = 0;
            HANDLE th = CreateThread(nullptr, 0, SaveThread, job, CREATE_SUSPENDED, nullptr);
            if (th) { SetThreadPriority(th, THREAD_PRIORITY_LOWEST); ResumeThread(th); CloseHandle(th); }
        }
    }
}

// ---------------------------------------------------------------- saving
struct Wic {
    IWICImagingFactory* f = nullptr;
    Wic() { CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f); }
    ~Wic() { if (f) f->Release(); }
    // BGRA (top-down) -> encoded image in `out` (JPEG or PNG)
    bool Encode(const uint32_t* bgra, int w, int h, bool jpeg, std::vector<uint8_t>& out) {
        if (!f) return false;
        IStream* st = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &st))) return false;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* fr = nullptr;
        IPropertyBag2* props = nullptr;
        bool ok = SUCCEEDED(f->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc)) &&
                  SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &props));
        if (ok && jpeg && props) {
            PROPBAG2 opt = {};
            opt.pstrName = (LPOLESTR)L"ImageQuality";
            VARIANT v; memset(&v, 0, sizeof(v)); v.vt = VT_R4; v.fltVal = 0.88f;
            props->Write(1, &opt, &v);
        }
        WICPixelFormatGUID fmt = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
        std::vector<uint8_t> bgr;
        const BYTE* src = (const BYTE*)bgra;
        UINT stride = w * 4;
        if (jpeg) {   // JPEG wants 24-bit
            bgr.resize((size_t)w * h * 3);
            for (size_t i = 0, n = (size_t)w * h; i < n; i++) { uint32_t c = bgra[i]; bgr[i * 3] = c & 255; bgr[i * 3 + 1] = (c >> 8) & 255; bgr[i * 3 + 2] = (c >> 16) & 255; }
            src = bgr.data(); stride = w * 3;
        }
        ok = ok && SUCCEEDED(fr->Initialize(props)) && SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
             SUCCEEDED(fr->WritePixels(h, stride, stride * h, (BYTE*)src)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        if (ok) {
            HGLOBAL g = nullptr;
            GetHGlobalFromStream(st, &g);
            STATSTG ss = {};
            st->Stat(&ss, STATFLAG_NONAME);
            void* p = GlobalLock(g);
            out.assign((uint8_t*)p, (uint8_t*)p + ss.cbSize.LowPart);
            GlobalUnlock(g);
        }
        if (props) props->Release();
        if (fr) fr->Release();
        if (enc) enc->Release();
        st->Release();
        return ok;
    }
};

// Minimal Motion-JPEG AVI writer (RIFF AVI, one video stream, idx1 index).
struct AviWriter {
    FILE* f = nullptr;
    int w = 0, h = 0;
    long moviStart = 0;
    std::vector<std::pair<uint32_t, uint32_t>> index;   // offset (from 'movi'), size
    uint32_t maxChunk = 0;
    void U32(uint32_t v) { fwrite(&v, 4, 1, f); }
    void U16(uint16_t v) { fwrite(&v, 2, 1, f); }
    void Cc(const char* s) { fwrite(s, 4, 1, f); }
    bool Open(const std::wstring& path, int width, int height) {
        f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;
        w = width; h = height;
        Cc("RIFF"); U32(0); Cc("AVI ");
        Cc("LIST"); U32(4 + 8 + 56 + 8 + 4 + 8 + 56 + 8 + 40); Cc("hdrl");
        Cc("avih"); U32(56); for (int i = 0; i < 14; i++) U32(0);          // patched in Close
        Cc("LIST"); U32(4 + 8 + 56 + 8 + 40); Cc("strl");
        Cc("strh"); U32(56); for (int i = 0; i < 14; i++) U32(0);          // patched in Close
        Cc("strf"); U32(40);
        U32(40); U32(w); U32(h); U16(1); U16(24); Cc("MJPG"); U32(w * h * 3); U32(0); U32(0); U32(0); U32(0);
        Cc("LIST"); U32(0); moviStart = ftell(f); Cc("movi");
        return true;
    }
    void Frame(const std::vector<uint8_t>& jpg) {
        uint32_t off = (uint32_t)(ftell(f) - moviStart);
        Cc("00dc"); U32((uint32_t)jpg.size());
        fwrite(jpg.data(), 1, jpg.size(), f);
        if (jpg.size() & 1) fputc(0, f);
        index.push_back({off, (uint32_t)jpg.size()});
        maxChunk = std::max<uint32_t>(maxChunk, (uint32_t)jpg.size());
    }
    void Close(double fps) {
        long moviEnd = ftell(f);
        Cc("idx1"); U32((uint32_t)index.size() * 16);
        for (auto& e : index) { Cc("00dc"); U32(0x10); U32(e.first); U32(e.second); }
        long end = ftell(f);
        uint32_t n = (uint32_t)index.size();
        uint32_t rate = (uint32_t)std::max(1.0, fps * 1000 + 0.5), scale = 1000;
        fseek(f, 4, SEEK_SET); U32((uint32_t)(end - 8));
        // avih
        fseek(f, 12 + 12 + 8, SEEK_SET);
        U32((uint32_t)(1e6 / std::max(1.0, fps))); U32(maxChunk * (uint32_t)fps); U32(0); U32(0x10); U32(n); U32(0); U32(1);
        U32(maxChunk + 8); U32(w); U32(h);
        // strh
        fseek(f, 12 + 12 + 8 + 56 + 12 + 8, SEEK_SET);
        Cc("vids"); Cc("MJPG"); U32(0); U32(0); U32(0); U32(scale); U32(rate); U32(0); U32(n); U32(maxChunk + 8); U32(0xFFFFFFFF); U32(0);
        U16(0); U16(0); U16((uint16_t)w); U16((uint16_t)h);
        fseek(f, moviStart - 4, SEEK_SET); U32((uint32_t)(moviEnd - moviStart));
        fclose(f); f = nullptr;
    }
};

void ToBgra(const uint8_t* px, const uint32_t* pal, size_t n, std::vector<uint32_t>& out) {
    out.resize(n);
    for (size_t i = 0; i < n; i++) out[i] = pal[px[i]] | 0xFF000000u;   // 0x00RRGGBB = BGRA in memory
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& d) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite(d.data(), 1, d.size(), f);
    fclose(f);
    return true;
}

std::string ReadTail(const std::wstring& path, size_t maxBytes) {
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return std::string();
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long from = std::max(0L, sz - (long)maxBytes);
    fseek(f, from, SEEK_SET);
    std::string s((size_t)(sz - from), '\0');
    fread(&s[0], 1, s.size(), f);
    fclose(f);
    if (from > 0) { size_t nl = s.find('\n'); if (nl != std::string::npos) s.erase(0, nl + 1); }
    return s;
}

DWORD WINAPI SaveThread(LPVOID p) {
    Job* job = (Job*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"%04d-%02d-%02d_%02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring dir = GameDir() + L"\\SWGBModern\\captures\\" + name;
    CreateDirectoryW((GameDir() + L"\\SWGBModern").c_str(), nullptr);
    CreateDirectoryW((GameDir() + L"\\SWGBModern\\captures").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\frames").c_str(), nullptr);
    double t0 = job->t0;
    {
        Wic wic;
        std::vector<uint32_t> bgra;
        std::vector<uint8_t> enc;
        if (job->shot) {
            ToBgra(job->shot->px.data(), job->shot->pal, job->shot->px.size(), bgra);
            if (wic.Encode(bgra.data(), job->shot->w, job->shot->h, false, enc)) WriteFileBytes(dir + L"\\screenshot.png", enc);
        }
        DECOMPRESSOR_HANDLE dec = nullptr;
        CreateDecompressor(COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW, nullptr, &dec);
        std::vector<uint8_t> cur, delta;
        AviWriter avi;
        bool aviOpen = false;
        int frames = 0, stills = 0;
        double first = 0, last = 0, nextStill = t0 - g_pre;
        for (auto& r : job->recs) {
            size_t n = (size_t)r.w * r.h;
            if (r.key) { cur.resize(n); if (!Decompress(dec, r.data, cur.data(), n)) continue; }
            else {
                if (cur.size() != n) continue;
                delta.resize(n);
                if (!Decompress(dec, r.data, delta.data(), n)) continue;
                for (size_t i = 0; i < n; i++) cur[i] ^= delta[i];
            }
            if (r.t < t0 - g_pre) continue;
            ToBgra(cur.data(), r.pal, n, bgra);
            if (!aviOpen) { aviOpen = avi.Open(dir + L"\\clip.avi", r.w, r.h); first = r.t; }
            if (aviOpen && r.w == avi.w && r.h == avi.h && wic.Encode(bgra.data(), r.w, r.h, true, enc)) { avi.Frame(enc); frames++; last = r.t; }
            if (r.t >= nextStill) {
                wchar_t fn[64];
                swprintf(fn, 64, L"\\frames\\%+06.2fs.png", r.t - t0);
                if (wic.Encode(bgra.data(), r.w, r.h, false, enc)) { WriteFileBytes(dir + fn, enc); stills++; }
                nextStill += 0.25;
            }
        }
        if (aviOpen) avi.Close(frames > 1 && last > first ? (frames - 1) / (last - first) : g_fps);
        if (dec) CloseDecompressor(dec);
        // summary
        std::string s = "SWGB Modern capture\r\n";
        char line[256];
        snprintf(line, sizeof(line), "key pressed at %04d-%02d-%02d %02d:%02d:%02d; %d video frames (%.1f s), %d stills\r\n\r\n", st.wYear,
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, frames, last - first, stills);
        s += line;
        s += "---- per-second stats (newest last)\r\n";
        for (auto& l : job->stats) s += l + "\r\n";
        s += "\r\n---- settings (SWGBModern.ini)\r\n";
        s += ReadTail(ConfigPath(), 64 * 1024);
        s += "\r\n---- log (end)\r\n";
        s += ReadTail(GameDir() + L"\\SWGBModern.log", 48 * 1024);
        WriteFileBytes(dir + L"\\summary.txt", std::vector<uint8_t>(s.begin(), s.end()));
        char d8[512];
        WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, d8, sizeof(d8), nullptr, nullptr);
        LOG("capture: saved %s (%d video frames, %d stills)", d8, frames, stills);
    }
    if (job->shot) delete job->shot;
    delete job;
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- stats
struct Stats {
    double secStart = 0;
    int lastPresented = 0, lastNew = 0;
    double presentMs = 0;
    int presentCount = 0;
    ULONGLONG lastProcCpu = 0, lastGameCpu = 0;
} g_st;
std::atomic<LONG> g_gameFrames{0};
std::atomic<int> g_gapMaxUs{0};
double g_lastGameFrame = 0;
DWORD g_gameTid = 0;
HANDLE g_gameThread = nullptr;
int g_lastRate = 0;

ULONGLONG Ticks(const FILETIME& a, const FILETIME& b) {
    return (((ULONGLONG)a.dwHighDateTime << 32) | a.dwLowDateTime) + (((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime);
}

UINT ParseKey(const std::string& s0, bool& ctrl, bool& shift, bool& alt) {
    std::string s;
    for (char c : s0) s += (char)tolower((unsigned char)c);
    ctrl = s.find("ctrl+") != std::string::npos; shift = s.find("shift+") != std::string::npos; alt = s.find("alt+") != std::string::npos;
    std::string k = s.substr(s.rfind('+') == std::string::npos ? 0 : s.rfind('+') + 1);
    if (k.size() >= 2 && k[0] == 'f' && isdigit((unsigned char)k[1])) { int n = atoi(k.c_str() + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    if (k == "printscreen" || k == "prtsc") return VK_SNAPSHOT;
    if (k == "pause") return VK_PAUSE;
    if (k == "scrolllock") return VK_SCROLL;
    if (k == "insert") return VK_INSERT;
    if (k == "home") return VK_HOME;
    if (k.size() == 1 && isalnum((unsigned char)k[0])) return (UINT)toupper((unsigned char)k[0]);
    return VK_F12;
}

} // namespace

void Capture::Init() {
    g_on = CfgBool("Debug", "Diagnostics", false);
    if (!g_on) return;
    g_vk = ParseKey(CfgStr("Debug", "CaptureKey", "ctrl+shift+f12"), g_ctrl, g_shift, g_alt);
    g_pre = g_post = std::max(1, std::min(10, CfgInt("Debug", "CaptureSeconds", 3)));
    g_fps = std::max(5, std::min(60, CfgInt("Debug", "CaptureFps", 30)));
    InitializeCriticalSection(&g_qLock);
    InitializeCriticalSection(&g_statLock);
    g_qEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (int i = 0; i < 4; i++) g_free.push_back(new Raw);
    HANDLE t = CreateThread(nullptr, 0, RecorderThread, nullptr, 0, nullptr);
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);
    LOG("diagnostics: on; capture key %s, %g s before/after, %d fps", CfgStr("Debug", "CaptureKey", "ctrl+shift+f12").c_str(), g_pre, g_fps);
}

bool Capture::Enabled() { return g_on; }

bool Capture::IsHotkeyMessage(UINT msg, WPARAM wp) {
    if (!g_on || (UINT)wp != g_vk) return false;
    if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP) return false;
    return (!g_ctrl || (GetKeyState(VK_CONTROL) & 0x8000)) && (!g_shift || (GetKeyState(VK_SHIFT) & 0x8000)) &&
           (!g_alt || (GetKeyState(VK_MENU) & 0x8000));
}

void Capture::OnFrame(const Frame& f) {
    if (!g_on || f.width <= 0 || f.pixels.size() != (size_t)f.width * f.height) return;
    double now = Now();
    if (now - g_lastQueued < 1.0 / g_fps - 0.002) return;
    g_lastQueued = now;
    Raw* r = nullptr;
    EnterCriticalSection(&g_qLock);
    if (!g_free.empty()) { r = g_free.front(); g_free.pop_front(); }
    LeaveCriticalSection(&g_qLock);
    if (!r) return;   // recorder busy: skip this frame
    r->t = now; r->w = f.width; r->h = f.height;
    memcpy(r->pal, f.palette, sizeof(r->pal));
    r->px.assign(f.pixels.begin(), f.pixels.end());
    EnterCriticalSection(&g_qLock);
    g_queue.push_back(r);
    LeaveCriticalSection(&g_qLock);
    SetEvent(g_qEvent);
    // the key was just pressed: keep this frame as the screenshot
    if (g_trigger.load() > 0) {
        EnterCriticalSection(&g_qLock);
        bool need = g_shot == nullptr && g_trigger.load() > 0;
        LeaveCriticalSection(&g_qLock);
        if (need) {
            Raw* s = new Raw(*r);
            EnterCriticalSection(&g_qLock);
            if (!g_shot) g_shot = s; else delete s;
            LeaveCriticalSection(&g_qLock);
        }
    }
}

void Capture::OnGameFrame() {
    if (!g_on) return;
    if (!g_gameTid) { g_gameTid = GetCurrentThreadId(); g_gameThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, g_gameTid); }
    double now = Now();
    if (g_lastGameFrame > 0) {
        double gap = now - g_lastGameFrame;
        int us = (int)(gap * 1e6);
        if (us > g_gapMaxUs.load()) g_gapMaxUs = us;
        if (gap > 0.060 && g_lastRate >= 40) LOG("stutter: game frame took %.0f ms (running at %d fps)", gap * 1000, g_lastRate);
    }
    g_lastGameFrame = now;
    g_gameFrames++;
}

void Capture::Tick(HWND hwnd, int presented, int newContent, double presentMs) {
    if (!g_on) return;
    double now = Now();
    g_st.presentMs += presentMs; g_st.presentCount++;
    // hotkey (edge-triggered, only while the game window is in front)
    static bool down = false;
    bool k = hwnd && GetForegroundWindow() == hwnd && (GetAsyncKeyState(g_vk) & 0x8000) &&
             (!g_ctrl || (GetAsyncKeyState(VK_CONTROL) & 0x8000)) && (!g_shift || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) &&
             (!g_alt || (GetAsyncKeyState(VK_MENU) & 0x8000));
    if (k && !down && g_trigger.load() == 0) {
        g_trigger = now;
        LOG("capture: key pressed, saving %g s before and after", g_pre);
        MessageBeep(MB_OK);
    }
    down = k;
    if (g_st.secStart == 0) { g_st.secStart = now; g_st.lastPresented = presented; g_st.lastNew = newContent; return; }
    double el = now - g_st.secStart;
    if (el < 1.0) return;
    FILETIME c, e, kt, ut;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &kt, &ut);
    ULONGLONG proc = Ticks(kt, ut), game = 0;
    if (g_gameThread) { GetThreadTimes(g_gameThread, &c, &e, &kt, &ut); game = Ticks(kt, ut); }
    PROCESS_MEMORY_COUNTERS_EX pm = {sizeof(pm)};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    int gameFps = (int)(g_gameFrames.exchange(0) / el + 0.5);
    g_lastRate = gameFps;
    char line[256];
    snprintf(line, sizeof(line), "diag: game %d fps (longest gap %.0f ms), shown %.0f fps (%.0f new), presenter %.2f ms, cpu %.1f%% (game thread %.1f%%), memory %u MB",
             gameFps, g_gapMaxUs.exchange(0) / 1000.0, (presented - g_st.lastPresented) / el, (newContent - g_st.lastNew) / el,
             g_st.presentCount ? g_st.presentMs / g_st.presentCount : 0.0, g_st.lastProcCpu ? (proc - g_st.lastProcCpu) / (el * 1e5) : 0.0,
             g_st.lastGameCpu ? (game - g_st.lastGameCpu) / (el * 1e5) : 0.0, (unsigned)(pm.PrivateUsage >> 20));
    LOG("%s", line);
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    char stamped[300];
    snprintf(stamped, sizeof(stamped), "%02d:%02d:%02d %s", lt.wHour, lt.wMinute, lt.wSecond, line + 6);
    EnterCriticalSection(&g_statLock);
    g_stats.push_back(stamped);
    while (g_stats.size() > 60) g_stats.pop_front();
    LeaveCriticalSection(&g_statLock);
    g_st.secStart = now; g_st.lastPresented = presented; g_st.lastNew = newContent;
    g_st.presentMs = 0; g_st.presentCount = 0;
    g_st.lastProcCpu = proc; g_st.lastGameCpu = game;
}

} // namespace swgbm
