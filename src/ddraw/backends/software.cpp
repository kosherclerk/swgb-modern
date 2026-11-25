// Software renderer: palette conversion + scaling on the CPU, shown with GDI.
// Works everywhere (VMs, RDP, broken drivers). Nearest scaling is done by our
// own integer-exact scaler; smooth filters use GDI HALFTONE stretching.
#include "../renderer.h"
#include <algorithm>

namespace swgbm {
namespace {

class SoftwareRenderer : public Renderer {
public:
    const char* Name() const override { return "Software (GDI)"; }
    bool Init(HWND hwnd) override {
        hwnd_ = hwnd;
        return true;
    }
    ~SoftwareRenderer() override {
        if (memDC_) DeleteDC(memDC_);
        if (dib_) DeleteObject(dib_);
    }
    bool Present(const Frame& f, const Viewport& vp, int cw, int ch, const PresentOptions& opt) override {
        bool nearest = opt.filter == Filter::Nearest || opt.filter == Filter::XBR || opt.filter == Filter::Smooth4x;
        // Nearest: scale directly into a viewport-sized DIB (exact pixel mapping).
        // Smooth: convert at game size and let GDI HALFTONE stretch.
        int tw = nearest ? vp.w : f.width, th = nearest ? vp.h : f.height;
        if (tw <= 0 || th <= 0) return true;
        if (tw != dw_ || th != dh_) Recreate(tw, th);
        if (!bits_) return false;
        if (nearest) {
            xmap_.resize(tw);
            for (int x = 0; x < tw; x++) xmap_[x] = (int)((int64_t)x * f.width / tw);
            int prevSy = -1;
            for (int y = 0; y < th; y++) {
                int sy = (int)((int64_t)y * f.height / th);
                uint32_t* d = bits_ + (size_t)y * tw;
                if (sy == prevSy) { memcpy(d, d - tw, tw * 4); continue; }
                const uint8_t* s = f.pixels.data() + (size_t)sy * f.width;
                for (int x = 0; x < tw; x++) d[x] = f.palette[s[xmap_[x]]];
                prevSy = sy;
            }
        } else {
            size_t n = (size_t)f.width * f.height;
            for (size_t i = 0; i < n; i++) bits_[i] = f.palette[f.pixels[i]];
        }
        HDC dc = GetDC(hwnd_);
        if (!dc) return true;
        // black borders
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT r;
        if (vp.y > 0) { r = {0, 0, cw, vp.y}; FillRect(dc, &r, black); }
        if (vp.y + vp.h < ch) { r = {0, vp.y + vp.h, cw, ch}; FillRect(dc, &r, black); }
        if (vp.x > 0) { r = {0, vp.y, vp.x, vp.y + vp.h}; FillRect(dc, &r, black); }
        if (vp.x + vp.w < cw) { r = {vp.x + vp.w, vp.y, cw, vp.y + vp.h}; FillRect(dc, &r, black); }
        if (nearest) {
            BitBlt(dc, vp.x, vp.y, vp.w, vp.h, memDC_, 0, 0, SRCCOPY);
        } else {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, vp.x, vp.y, vp.w, vp.h, memDC_, 0, 0, f.width, f.height, SRCCOPY);
        }
        ReleaseDC(hwnd_, dc);
        if (opt.vsync) {
            static auto flush = (HRESULT(WINAPI*)())GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmFlush");
            if (flush) flush();
        }
        return true;
    }

private:
    void Recreate(int w, int h) {
        if (memDC_) DeleteDC(memDC_);
        if (dib_) DeleteObject(dib_);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        memDC_ = CreateCompatibleDC(nullptr);
        dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, (void**)&bits_, nullptr, 0);
        if (dib_) SelectObject(memDC_, dib_);
        dw_ = w; dh_ = h;
    }
    HWND hwnd_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    uint32_t* bits_ = nullptr;
    int dw_ = 0, dh_ = 0;
    std::vector<int> xmap_;
};

} // namespace

std::unique_ptr<Renderer> CreateSoftwareRenderer() { return std::make_unique<SoftwareRenderer>(); }

} // namespace swgbm
