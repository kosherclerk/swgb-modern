#include "presenter.h"
#include "ddraw_impl.h"
#include "window.h"
#include "capture.h"
#include "zoom.h"
#include "floattext.h"
#include <algorithm>
#include <math.h>

namespace swgbm {
void LogThreadReport();

Presenter& Presenter::Get() {
    static Presenter* p = [] {
        auto* x = new Presenter();
        InitializeCriticalSection(&x->vpLock_);
        InitializeCriticalSection(&x->ovLock_);
        return x;
    }();
    return *p;
}

static Filter ParseFilter(const std::string& s) {
    if (s == "sharp" || s == "sharpbilinear") return Filter::Sharp;
    if (s == "bilinear" || s == "linear") return Filter::Bilinear;
    if (s == "xbr") return Filter::Sharp;   // xBR was removed in 1.3.1
    if (s == "smooth" || s == "smooth4x") return Filter::Bilinear;
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
    int gw = State().modeW, gh = State().modeH;
    if (Zoom::On() && gw == Zoom::InternalW() && gh == Zoom::InternalH()) { gw = Zoom::DisplayW(); gh = Zoom::DisplayH(); }
    return ComputeViewport(rc.right, rc.bottom, gw, gh);
}

void Presenter::GameSize(int& w, int& h) {
    w = State().modeW;
    h = State().modeH;
}

const char* Presenter::BackendName() { return backendName_; }

void Presenter::SetOverlay(const uint32_t* px, int w, int h, float x, float y, float rw, float rh, bool hideFrame, int y0, int y1) {
    EnterCriticalSection(&ovLock_);
    Overlay& o = ovShared_;
    bool resized = o.width != w || o.height != h;
    if (resized) o.pixels.assign((size_t)w * h, 0);
    if (y0 < 0 || resized) { y0 = 0; y1 = h; }
    if (y1 > y0) memcpy(o.pixels.data() + (size_t)y0 * w, px + (size_t)y0 * w, (size_t)(y1 - y0) * w * 4);
    if (!o.visible || ovChanged_ == false) { o.dirtyY0 = y0; o.dirtyY1 = y1; }
    else { o.dirtyY0 = std::min(o.dirtyY0, y0); o.dirtyY1 = std::max(o.dirtyY1, y1); }
    o.visible = true; o.width = w; o.height = h; o.x = x; o.y = y; o.w = rw; o.h = rh; o.hideFrame = hideFrame;
    ovChanged_ = true;
    LeaveCriticalSection(&ovLock_);
    if (frameEvent_) SetEvent(frameEvent_);
}

void Presenter::ClearOverlay() {
    EnterCriticalSection(&ovLock_);
    if (ovShared_.visible) { ovShared_.visible = false; ovChanged_ = true; }
    LeaveCriticalSection(&ovLock_);
    if (frameEvent_) SetEvent(frameEvent_);
}

Viewport Presenter::ComputeViewport(int cw, int ch, int gw, int gh) {
    ScaleMode mode = (ScaleMode)scaleMode_;
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
    // 8-bit indexed: the exact palette indices the engine drew, plus its palette
    int w = f.width, h = f.height, stride = (w + 3) & ~3;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih) + 256 * 4;
    fh.bfSize = fh.bfOffBits + stride * h;
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 8; ih.biClrUsed = 256;
    fwrite(&fh, sizeof(fh), 1, fp);
    fwrite(&ih, sizeof(ih), 1, fp);
    for (int i = 0; i < 256; i++) { uint32_t c = f.palette[i] & 0xFFFFFF; fwrite(&c, 4, 1, fp); }
    std::vector<uint8_t> row(stride, 0);
    for (int y = h - 1; y >= 0; y--) {
        memcpy(row.data(), f.pixels.data() + (size_t)y * w, w);
        fwrite(row.data(), 1, stride, fp);
    }
    fclose(fp);
}

void Presenter::LoadOptions(PresentOptions& opt, int& maxFps) {
    opt.filter = ParseFilter(CfgStr("Display", "Filter", "nearest"));
    opt.vsync = CfgBool("Display", "VSync", true);
    scaleMode_ = (LONG)ParseScale(CfgStr("Display", "ScaleMode", "aspect"));
    rendererName_ = CfgStr("Display", "Renderer", "auto");
    maxFps = CfgInt("Display", "MaxFPS", 0);
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
    }
}

DWORD WINAPI Presenter::ThreadMain(void* p) { ((Presenter*)p)->Run(); return 0; }

void Presenter::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::unique_ptr<Renderer> r = CreateBackend();
    backendName_ = r ? r->Name() : "none";
    PresentOptions opt;
    int maxFps = 0;
    LoadOptions(opt, maxFps);
    FILETIME iniTime = {};
    auto iniStamp = [&]() {
        WIN32_FILE_ATTRIBUTE_DATA a;
        FILETIME t = {};
        if (GetFileAttributesExW(ConfigPath().c_str(), GetFileExInfoStandard, &a)) t = a.ftLastWriteTime;
        return t;
    };
    iniTime = iniStamp();
    DWORD lastIniCheck = GetTickCount();
    LOG("frame limiter: %d fps%s", maxFps, maxFps ? "" : " (off)");
    Capture::Init();
    int totalShown = 0, totalNew = 0;
    double lastPresentMs = 0;
    int dumpEvery = CfgInt("Debug", "DumpFrameSeconds", 0);
    DWORD lastDump = GetTickCount();

    // High resolution waitable timer for the frame limiter.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    Frame frame;
    DWORD frames = 0, uniqueFrames = 0, skipped = 0, fpsTick = GetTickCount();
    int64_t tSnap = 0, tPres = 0;
    bool forceFull = true;
    int lastCw = -1, lastCh = -1;
    while (running_) {
        DWORD wr = WaitForSingleObject(frameEvent_, 200); // periodic wake keeps resizes smooth
        if (!running_) break;
        // Live settings: the launcher's Save applies while the game runs.
        if (GetTickCount() - lastIniCheck > 500) {
            lastIniCheck = GetTickCount();
            FILETIME t = iniStamp();
            if (CompareFileTime(&t, &iniTime) != 0) {
                iniTime = t;
                std::string before = rendererName_;
                LoadOptions(opt, maxFps);
                LOG("settings reloaded: renderer %s, filter %d, vsync %d, max fps %d", rendererName_.c_str(), (int)opt.filter, opt.vsync, maxFps);
                // A window keeps traces of the API that drew it (GL pixel format,
                // DXGI flip-model association), so switching APIs live is not
                // reliable: the renderer applies on the next game start.
                if (rendererName_ != before) LOG("renderer change to '%s' applies on next start", rendererName_.c_str());
                PostMessageW(hwnd_, WM_APP + 0x5157, 0, 0); // window mode / cursor lock, applied on the game thread
                forceFull = true;
                lastCw = -1;
            }
        }
        if (r) r->WaitForNextFrame();
        LARGE_INTEGER tA, tB, tC, tD; QueryPerformanceCounter(&tA);
        bool have = SnapshotPrimary(frame);
        QueryPerformanceCounter(&tB); tSnap += tB.QuadPart - tA.QuadPart;
        InterlockedExchange(&pendingFlip_, 0);
        SetEvent(consumedEvent_);
        if (!have && r) {
            // No primary surface yet (e.g. intro videos during startup): show
            // the overlay on a black frame of the current mode's size.
            EnterCriticalSection(&ovLock_);
            bool ov = ovShared_.visible || ovRender_.visible;
            LeaveCriticalSection(&ovLock_);
            if (ov) {
                int w = std::max(1, State().modeW), h = std::max(1, State().modeH);
                if (frame.width != w || frame.height != h) { frame.width = w; frame.height = h; frame.pixels.assign((size_t)w * h, 0); }
                memset(frame.palette, 0, sizeof(frame.palette));
                frame.MarkAll();
                have = true;
            }
        }
        if (!have || !r) continue;
        bool textDirty = false;
        {   // text layer (HD text, zoom): drawn at the viewport's resolution
            RECT crc;
            GetClientRect(hwnd_, &crc);
            const bool zf = Zoom::On() && frame.width == Zoom::InternalW() && frame.height == Zoom::InternalH();
            Viewport tvp = zf ? ComputeViewport(crc.right, crc.bottom, Zoom::DisplayW(), Zoom::DisplayH())
                              : ComputeViewport(crc.right, crc.bottom, frame.width, frame.height);
            int ty0 = 0, ty1 = 0;
            bool any = false;
            if (FloatText::Build(textOv_.pixels, tvp.w, tvp.h, frame.width, frame.height, frame.palette, ty0, ty1, any)) {
                bool wasVisible = textOv_.visible;
                textOv_.visible = any;
                textOv_.width = tvp.w; textOv_.height = tvp.h;
                textOv_.x = 0; textOv_.y = 0; textOv_.w = (float)frame.width; textOv_.h = (float)frame.height;
                textOv_.hideFrame = false;
                textOv_.dirtyY0 = wasVisible ? ty0 : 0;
                textOv_.dirtyY1 = wasVisible ? ty1 : tvp.h;
                textDirty = wasVisible || any;
            } else {
                textOv_.dirtyY0 = textOv_.dirtyY1 = 0;
            }
        }
        if (Capture::Enabled()) {
            Capture::OnFrame(frame);
            Capture::Tick(hwnd_, totalShown, totalNew, lastPresentMs);
            lastPresentMs = 0;
        }

        RECT rc;
        GetClientRect(hwnd_, &rc);
        bool resized = rc.right != lastCw || rc.bottom != lastCh;
        if (forceFull) frame.MarkAll();
        if (dumpEvery > 0 && GetTickCount() - lastDump > (DWORD)dumpEvery * 1000) {
            DumpFrameBMP(frame);
            lastDump = GetTickCount();
        }
        // Nothing new and nothing moved: keep the last presented image.
        // (The engine re-blits identical frames ~3x per real change.)
        // pick up overlay changes (copied so producers never block on the GPU)
        bool ovDirty = false;
        EnterCriticalSection(&ovLock_);
        if (ovChanged_) {
            Overlay& s = ovShared_;
            bool sizeChange = s.width != ovRender_.width || s.height != ovRender_.height;
            if (sizeChange) ovRender_.pixels = s.pixels;
            else if (s.dirtyY1 > s.dirtyY0)
                memcpy(ovRender_.pixels.data() + (size_t)s.dirtyY0 * s.width, s.pixels.data() + (size_t)s.dirtyY0 * s.width,
                       (size_t)(s.dirtyY1 - s.dirtyY0) * s.width * 4);
            ovRender_.visible = s.visible; ovRender_.width = s.width; ovRender_.height = s.height;
            ovRender_.x = s.x; ovRender_.y = s.y; ovRender_.w = s.w; ovRender_.h = s.h; ovRender_.hideFrame = s.hideFrame;
            ovRender_.dirtyY0 = sizeChange ? 0 : s.dirtyY0;
            ovRender_.dirtyY1 = sizeChange ? s.height : s.dirtyY1;
            s.dirtyY0 = s.dirtyY1 = 0;
            ovChanged_ = false;
            ovDirty = true;
        } else {
            ovRender_.dirtyY0 = ovRender_.dirtyY1 = 0;
        }
        LeaveCriticalSection(&ovLock_);
        if (forceFull && ovRender_.visible) { ovRender_.dirtyY0 = 0; ovRender_.dirtyY1 = ovRender_.height; }
        if (forceFull && textOv_.visible) { textOv_.dirtyY0 = 0; textOv_.dirtyY1 = textOv_.height; }
        // videos (and other producers) first, then the text layer
        const Overlay* use = ovRender_.visible ? &ovRender_ : textOv_.visible ? &textOv_ : nullptr;
        if (use == &textOv_ && textDirty) ovDirty = true;
        if (use != lastOverlay_) ovDirty = true;
        Frame* show = &frame;
        if (use && lastOverlay_ != use) { Overlay* u = (Overlay*)use; u->dirtyY0 = 0; u->dirtyY1 = u->height; }
        lastOverlay_ = use;
        r->SetOverlay(use);
        // zoom: the game frame is larger than what is shown; the battlefield
        // and the panel are drawn as two regions (see zoom.h)
        const bool zoomed = Zoom::On() && frame.width == Zoom::InternalW() && frame.height == Zoom::InternalH();
        const bool zoomMoving = zoomed && Zoom::Animate();
        if (zoomMoving) SetEvent(frameEvent_);   // keep presenting until the zoom settles
        bool skip = !show->Dirty() && !resized && !ovDirty && !zoomMoving && r->SkipIdenticalFrames();
        if (skip) skipped++;
        else {
        lastCw = rc.right; lastCh = rc.bottom;
        Viewport vp = zoomed ? ComputeViewport(rc.right, rc.bottom, Zoom::DisplayW(), Zoom::DisplayH())
                             : ComputeViewport(rc.right, rc.bottom, frame.width, frame.height);
        opt.regions.clear();
        if (zoomed) Zoom::Regions(vp, frame.width, frame.height, opt.filter, opt.regions);
        EnterCriticalSection(&vpLock_);
        vp_ = vp;
        gameW_ = frame.width; gameH_ = frame.height;
        LeaveCriticalSection(&vpLock_);

        if (show->Dirty()) { uniqueFrames++; totalNew++; }
        QueryPerformanceCounter(&tC);
        bool presOk = r->Present(*show, vp, rc.right, rc.bottom, opt);
        QueryPerformanceCounter(&tD); tPres += tD.QuadPart - tC.QuadPart;
        lastPresentMs += (tD.QuadPart - tC.QuadPart) * 1000.0 / freq.QuadPart;
        totalShown++;
        if (!presOk) {
            LOG("Renderer %s: present failed, recreating", r->Name());
            r.reset();
            r = CreateBackend();
            backendName_ = r ? r->Name() : "none";
            forceFull = true;
            lastCw = -1;
            continue;
        }
        forceFull = false;
        (void)wr;
        frames++;
        if (GetTickCount() - fpsTick >= 5000) {
            LOG("present: %.1f fps (%.1f with new content, %.1f/s identical skipped), game %dx%d -> viewport %dx%d @%d,%d (%s)",
                frames * 1000.0 / (GetTickCount() - fpsTick), uniqueFrames * 1000.0 / (GetTickCount() - fpsTick),
                skipped * 1000.0 / (GetTickCount() - fpsTick),
                frame.width, frame.height, vp.w, vp.h, vp.x, vp.y, backendName_);
            if (CfgBool("Debug", "ThreadReport", false)) LOG("present: time per 5s: snapshot %.0f ms, text 0 ms, present %.0f ms", tSnap * 1000.0 / freq.QuadPart, tPres * 1000.0 / freq.QuadPart);
            tSnap = tPres = 0;
            frames = 0;
            uniqueFrames = 0;
            skipped = 0;
            fpsTick = GetTickCount();
            if (CfgBool("Debug", "ThreadReport", false)) LogThreadReport();
        }
        } // !skip
        // Pacing applies to skipped frames too: the game loop waits on this
        // thread (Flip/Blt back-pressure), so it keeps running at display rate.
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
