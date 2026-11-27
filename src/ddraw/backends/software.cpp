// Software renderer: palette conversion + scaling on the CPU, shown with GDI.
// Works everywhere (VMs, RDP, broken drivers).
//  - Nearest: exact integer-mapped scaling straight into a viewport-sized DIB;
//    only rows whose source rows changed are recomputed, in parallel.
//  - Sharp: nearest pre-scale to the largest
//    integer factor, then GDI HALFTONE for the small remaining fraction.
//  - Bilinear: game-size conversion, GDI HALFTONE stretch.
#include "../renderer.h"
#include <algorithm>
#include <thread>

namespace swgbm {
namespace {

struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    uint32_t* bits = nullptr;
    int w = 0, h = 0;
    ~Dib() { Free(); }
    void Free() {
        if (dc) DeleteDC(dc);
        if (bmp) DeleteObject(bmp);
        dc = nullptr; bmp = nullptr; bits = nullptr; w = h = 0;
    }
    bool Ensure(int nw, int nh) {
        if (nw == w && nh == h && bits) return false;
        Free();
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = nw;
        bi.bmiHeader.biHeight = -nh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (bmp) SelectObject(dc, bmp);
        w = nw; h = nh;
        return true;
    }
};

template <class F> void ParallelRows(int n, F fn) {
    unsigned hw = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    if (n < 256 || hw == 1) { fn(0, n); return; }
    std::vector<std::thread> t;
    int chunk = (n + (int)hw - 1) / (int)hw;
    for (unsigned i = 0; i < hw; i++) {
        int a = (int)i * chunk, b = std::min(n, a + chunk);
        if (a < b) t.emplace_back([=] { fn(a, b); });
    }
    for (auto& th : t) th.join();
}

class SoftwareRenderer : public Renderer {
public:
    const char* Name() const override { return "Software (GDI)"; }
    bool Init(HWND hwnd) override { hwnd_ = hwnd; return true; }

    bool Present(const Frame& f, const Viewport& vp, int cw, int ch, const PresentOptions& opt) override {
        if (vp.w <= 0 || vp.h <= 0) return true;
        Filter mode = opt.filter == Filter::Bilinear ? Filter::Bilinear : opt.filter == Filter::Nearest ? Filter::Nearest : Filter::Sharp;
        const bool regions = !opt.regions.empty();   // zoom: convert at game size, GDI scales each region
        if (regions) mode = Filter::Bilinear;
        int k = std::max(1, std::min(vp.w / f.width, vp.h / f.height)); // integer prescale (Sharp)
        int tw, th;
        if (mode == Filter::Nearest) { tw = vp.w; th = vp.h; }
        else if (mode == Filter::Sharp) { tw = f.width * k; th = f.height * k; }
        else { tw = f.width; th = f.height; }
        bool full = buf_.Ensure(tw, th) || f.paletteChanged || mode != lastMode_ || f.width != lastFw_ || f.height != lastFh_;
        lastMode_ = mode; lastFw_ = f.width; lastFh_ = f.height;
        if (!buf_.bits) return false;

        // map destination rows/cols to source rows/cols (nearest, exact)
        if ((int)ymap_.size() != th || full) {
            ymap_.resize(th);
            xmap_.resize(tw);
            for (int y = 0; y < th; y++) ymap_[y] = (int)((int64_t)y * f.height / th);
            for (int x = 0; x < tw; x++) xmap_[x] = (int)((int64_t)x * f.width / tw);
        }
        int y0 = full ? 0 : f.dirtyY0, y1 = full ? f.height : f.dirtyY1;
        if (y1 > y0) {
            // destination rows whose source row is in [y0, y1)
            int d0 = (int)(((int64_t)y0 * th + f.height - 1) / f.height), d1 = (int)(((int64_t)y1 * th + f.height - 1) / f.height);
            d0 = std::max(0, std::min(th, d0)); d1 = std::max(d0, std::min(th, d1));
            const uint8_t* px = f.pixels.data();
            const uint32_t* pal = f.palette;
            uint32_t* dst = buf_.bits;
            int fw = f.width;
            ParallelRows(d1 - d0, [&](int a, int b) {
                for (int y = d0 + a; y < d0 + b; y++) {
                    uint32_t* d = dst + (size_t)y * tw;
                    if (y > d0 + a && ymap_[y] == ymap_[y - 1]) { memcpy(d, d - tw, (size_t)tw * 4); continue; }
                    const uint8_t* s = px + (size_t)ymap_[y] * fw;
                    for (int x = 0; x < tw; x++) d[x] = pal[s[xmap_[x]]];
                }
            });
        }

        HDC dc = GetDC(hwnd_);
        if (!dc) return true;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT r;
        if (regions) {
            // the regions cover the viewport: paint only the margins (no flash of black)
            if (vp.y > 0) { r = {0, 0, cw, vp.y}; FillRect(dc, &r, black); }
            if (vp.y + vp.h < ch) { r = {0, vp.y + vp.h, cw, ch}; FillRect(dc, &r, black); }
            if (vp.x > 0) { r = {0, vp.y, vp.x, vp.y + vp.h}; FillRect(dc, &r, black); }
            if (vp.x + vp.w < cw) { r = {vp.x + vp.w, vp.y, cw, vp.y + vp.h}; FillRect(dc, &r, black); }
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            if (!(overlay_ && overlay_->visible && overlay_->hideFrame))
                for (const Region& rg : opt.regions)
                    StretchBlt(dc, rg.x, rg.y, rg.w, rg.h, buf_.dc, (int)(rg.u0 * tw + 0.5f), (int)(rg.v0 * th + 0.5f),
                               (int)((rg.u1 - rg.u0) * tw + 0.5f), (int)((rg.v1 - rg.v0) * th + 0.5f), SRCCOPY);
            DrawOverlay(dc, f, vp);
            ReleaseDC(hwnd_, dc);
            if (opt.vsync) {
                static auto flush2 = (HRESULT(WINAPI*)())GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmFlush");
                if (flush2) flush2();
            }
            return true;
        }
        if (vp.y > 0) { r = {0, 0, cw, vp.y}; FillRect(dc, &r, black); }
        if (vp.y + vp.h < ch) { r = {0, vp.y + vp.h, cw, ch}; FillRect(dc, &r, black); }
        if (vp.x > 0) { r = {0, vp.y, vp.x, vp.y + vp.h}; FillRect(dc, &r, black); }
        if (vp.x + vp.w < cw) { r = {vp.x + vp.w, vp.y, cw, vp.y + vp.h}; FillRect(dc, &r, black); }
        if (overlay_ && overlay_->visible && overlay_->hideFrame) {
            r = {vp.x, vp.y, vp.x + vp.w, vp.y + vp.h};
            FillRect(dc, &r, black);
        } else if (tw == vp.w && th == vp.h) {
            BitBlt(dc, vp.x, vp.y, vp.w, vp.h, buf_.dc, 0, 0, SRCCOPY);
        } else {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, vp.x, vp.y, vp.w, vp.h, buf_.dc, 0, 0, tw, th, SRCCOPY);
        }
        DrawOverlay(dc, f, vp);
        ReleaseDC(hwnd_, dc);
        if (opt.vsync) {
            static auto flush = (HRESULT(WINAPI*)())GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmFlush");
            if (flush) flush();
        }
        return true;
    }

private:
    void DrawOverlay(HDC dc, const Frame& f, const Viewport& vp) {
        const Overlay* o = overlay_;
        if (!o || !o->visible || o->width <= 0 || o->height <= 0) return;
        bool full = ov_.Ensure(o->width, o->height);
        int y0 = full ? 0 : o->dirtyY0, y1 = full ? o->height : o->dirtyY1;
        if (y1 > y0) memcpy(ov_.bits + (size_t)y0 * o->width, o->pixels.data() + (size_t)y0 * o->width, (size_t)(y1 - y0) * o->width * 4);
        float x, y, w, h;
        OverlayRect(*o, f, vp, x, y, w, h);
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        SetStretchBltMode(dc, HALFTONE);
        GdiAlphaBlend(dc, (int)x, (int)y, (int)(w + 0.5f), (int)(h + 0.5f), ov_.dc, 0, 0, o->width, o->height, bf);
    }

    Dib ov_;
    HWND hwnd_ = nullptr;
    Dib buf_;
    std::vector<int> xmap_, ymap_;
    Filter lastMode_ = Filter::Nearest;
    int lastFw_ = 0, lastFh_ = 0;
};

} // namespace

std::unique_ptr<Renderer> CreateSoftwareRenderer() { return std::make_unique<SoftwareRenderer>(); }

} // namespace swgbm
