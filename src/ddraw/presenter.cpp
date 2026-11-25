#include "presenter.h"
#include "ddraw_impl.h"
#include "window.h"
#include <algorithm>
#include <math.h>

namespace swgbm {
void LogThreadReport();

Presenter& Presenter::Get() {
    static Presenter* p = [] {
        auto* x = new Presenter();
        InitializeCriticalSection(&x->vpLock_);
        return x;
    }();
    return *p;
}

static Filter ParseFilter(const std::string& s) {
    if (s == "sharp" || s == "sharpbilinear") return Filter::Sharp;
    if (s == "bilinear" || s == "linear") return Filter::Bilinear;
    if (s == "xbr") return Filter::XBR;
    if (s == "smooth" || s == "smooth4x") return Filter::Smooth4x;
    return Filter::Nearest;
}

static ScaleMode ParseScale(const std::string& s) {
    if (s == "integer") return ScaleMode::Integer;
    if (s == "stretch") return ScaleMode::Stretch;
    return ScaleMode::Aspect;
}

void Presenter::Start(HWND hwnd) {
    State();
    if (InterlockedCompareExchange(&running_, 1, 0) != 0) return;
    hwnd_ = hwnd;
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    consumedEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
}

void Presenter::Stop() {
    if (!InterlockedExchange(&running_, 0)) return;
    SetEvent(frameEvent_);
    WaitForSingleObject(thread_, 2000);
}

void Presenter::FrameReady(bool flip) {
    if (!running_) return;
    if (flip) {
        Window::NotifyFlip();
        InterlockedExchange(&pendingFlip_, 1);
        ResetEvent(consumedEvent_);
        SetEvent(frameEvent_);
        // Back-pressure: wait until the presenter has copied this frame so the
        // game never runs more than one frame ahead of the display.
        WaitForSingleObject(consumedEvent_, 100);
    } else {
        SetEvent(frameEvent_);
    }
}

Viewport Presenter::CurrentViewport() {
    // Computed on demand from the live window size and game mode so input
    // mapping is correct even before the first frame of a new mode is shown.
    HWND h = State().hwnd;
    if (!h) return vp_;
    RECT rc;
    GetClientRect(h, &rc);
    return ComputeViewport(rc.right, rc.bottom, State().modeW, State().modeH);
}

void Presenter::GameSize(int& w, int& h) {
    w = State().modeW;
    h = State().modeH;
}

const char* Presenter::BackendName() { return backendName_; }

Viewport Presenter::ComputeViewport(int cw, int ch, int gw, int gh) {
    static ScaleMode mode = ParseScale(CfgStr("Display", "ScaleMode", "aspect"));
    if (cw <= 0 || ch <= 0 || gw <= 0 || gh <= 0) return {0, 0, std::max(cw, 1), std::max(ch, 1)};
    if (mode == ScaleMode::Stretch) return {0, 0, cw, ch};
    double s = std::min((double)cw / gw, (double)ch / gh);
    if (mode == ScaleMode::Integer && s >= 1.0) s = floor(s);
    int w = (int)lround(gw * s), h = (int)lround(gh * s);
    return {(cw - w) / 2, (ch - h) / 2, w, h};
}

std::unique_ptr<Renderer> Presenter::CreateBackend() {
    std::string want = CfgStr("Display", "Renderer", "auto");
    std::vector<std::string> order;
    if (want != "auto") order.push_back(want);
    for (const char* b : {"d3d11", "opengl", "d3d12", "vulkan", "d3d10", "software"})
        if (want != b) order.push_back(b);
    for (auto& name : order) {
        std::unique_ptr<Renderer> r;
        if (name == "d3d11") r = CreateD3D11Renderer();
        else if (name == "d3d10") r = CreateD3D10Renderer();
        else if (name == "d3d12") r = CreateD3D12Renderer();
        else if (name == "opengl") r = CreateOpenGLRenderer();
        else if (name == "vulkan") r = CreateVulkanRenderer();
        else if (name == "software" || name == "gdi") r = CreateSoftwareRenderer();
        if (!r) continue;
        if (r->Init(hwnd_)) {
            LOG("Renderer: using %s%s", r->Name(), name == want ? "" : " (fallback)");
            return r;
        }
        LOG("Renderer: %s failed to initialise", r->Name());
    }
    return nullptr;
}

static void DumpFrameBMP(const Frame& f) {
    std::wstring path = GameDir() + L"\\SWGBModern_frame.bmp";
    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) return;
    int w = f.width, h = f.height, stride = w * 3 + ((4 - (w * 3) % 4) % 4);
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + stride * h;
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, fp);
    fwrite(&ih, sizeof(ih), 1, fp);
    std::vector<uint8_t> row(stride, 0);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t c = f.palette[f.pixels[(size_t)y * w + x]];
            row[x * 3] = c & 0xFF; row[x * 3 + 1] = (c >> 8) & 0xFF; row[x * 3 + 2] = (c >> 16) & 0xFF;
        }
        fwrite(row.data(), 1, stride, fp);
    }
    fclose(fp);
}

DWORD WINAPI Presenter::ThreadMain(void* p) { ((Presenter*)p)->Run(); return 0; }

void Presenter::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::unique_ptr<Renderer> r = CreateBackend();
    backendName_ = r ? r->Name() : "none";
    PresentOptions opt;
    opt.filter = ParseFilter(CfgStr("Display", "Filter", "nearest"));
    opt.vsync = CfgBool("Display", "VSync", true);
    opt.sharpness = CfgFloat("Display", "Sharpness", 0.5f);
    int maxFps = CfgInt("Display", "MaxFPS", 0);
    if (maxFps <= 0 && opt.vsync) {
        // Drivers can force VSync off ("Wait for vertical refresh: off"); cap
        // at the refresh rate of the monitor the window is on regardless.
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            maxFps = (int)dm.dmDisplayFrequency;
        LOG("frame limiter: %d fps (display refresh)", maxFps);
    }
    int dumpEvery = CfgInt("Debug", "DumpFrameSeconds", 0);
    DWORD lastDump = GetTickCount();

    // High resolution waitable timer for the frame limiter.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    Frame frame;
    DWORD frames = 0, uniqueFrames = 0, fpsTick = GetTickCount();
    uint64_t lastHash = 0;
    while (running_) {
        DWORD wr = WaitForSingleObject(frameEvent_, 200); // periodic refresh keeps resizes smooth
        if (!running_) break;
        bool have = SnapshotPrimary(frame);
        InterlockedExchange(&pendingFlip_, 0);
        SetEvent(consumedEvent_);
        if (!have || !r) continue;

        RECT rc;
        GetClientRect(hwnd_, &rc);
        Viewport vp = ComputeViewport(rc.right, rc.bottom, frame.width, frame.height);
        EnterCriticalSection(&vpLock_);
        vp_ = vp;
        gameW_ = frame.width; gameH_ = frame.height;
        LeaveCriticalSection(&vpLock_);

        if (!r->Present(frame, vp, rc.right, rc.bottom, opt)) {
            LOG("Renderer %s: present failed, recreating", r->Name());
            r.reset();
            r = CreateBackend();
            backendName_ = r ? r->Name() : "none";
        }
        (void)wr;
        frames++;
        if (LogEnabled()) { // count frames whose content actually changed
            uint64_t hsh = 1469598103934665603ull;
            const uint64_t* q = (const uint64_t*)frame.pixels.data();
            for (size_t i = 0, n = frame.pixels.size() / 8; i < n; i += 7) hsh = (hsh ^ q[i]) * 1099511628211ull;
            if (hsh != lastHash) { uniqueFrames++; lastHash = hsh; }
        }
        if (dumpEvery > 0 && GetTickCount() - lastDump > (DWORD)dumpEvery * 1000) {
            DumpFrameBMP(frame);
            lastDump = GetTickCount();
        }
        if (GetTickCount() - fpsTick >= 5000) {
            LOG("present: %.1f fps (%.1f distinct), game %dx%d -> viewport %dx%d @%d,%d (%s)", frames * 1000.0 / (GetTickCount() - fpsTick),
                uniqueFrames * 1000.0 / (GetTickCount() - fpsTick),
                frame.width, frame.height, vp.w, vp.h, vp.x, vp.y, backendName_);
            frames = 0;
            uniqueFrames = 0;
            fpsTick = GetTickCount();
            if (CfgBool("Debug", "ThreadReport", false)) LogThreadReport();
        }
        if (maxFps > 0 && timer) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            int64_t period = freq.QuadPart / maxFps;
            int64_t target = last.QuadPart + period;
            if (target > now.QuadPart) {
                LARGE_INTEGER due;
                due.QuadPart = -((target - now.QuadPart) * 10000000 / freq.QuadPart);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 1000);
                last.QuadPart = target;
            } else {
                last = now;
            }
        }
    }
    r.reset();
    if (timer) CloseHandle(timer);
}

} // namespace swgbm
