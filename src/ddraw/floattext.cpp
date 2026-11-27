// Interface text over the zoomed battlefield (see floattext.h).
#include "floattext.h"
#include "ddraw_impl.h"
#include "zoom.h"
#include <algorithm>
#include <math.h>

namespace swgbm {
namespace {

struct Item {
    int x, y, w, h;                       // in the screen buffer (game pixels)
    std::vector<uint8_t> cov, written, bg;
    uint32_t rgb;                         // 0xRRGGBB
};

CRITICAL_SECTION g_lock;
bool g_init = false;
std::vector<Item> g_items;
unsigned g_serial = 1, g_builtSerial = 0;
std::vector<uint8_t> g_claim;             // scratch: pixels claimed by later texts

void Init() {
    if (!g_init) { InitializeCriticalSection(&g_lock); g_init = true; }
}

bool IsScreen(const DDSurface* s) {
    return s && !s->IsPrimary() && s->w_ == Zoom::InternalW() && s->h_ == Zoom::InternalH();
}

double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

// Where a text of the battlefield goes on the shown battlefield: same distance
// from the nearest edge (left/right/top/bottom) or from the centre.
void Anchor(const Item& it, int& dx, int& dy) {
    const int bw = Zoom::InternalW(), bh = Zoom::InternalH() - Zoom::PanelH();
    const int dw = Zoom::DisplayW(), dbh = Zoom::DisplayH() - Zoom::PanelH();
    float cx = it.x + it.w * 0.5f, cy = it.y + it.h * 0.5f;
    dx = cx < bw * 0.35f ? it.x : cx > bw * 0.65f ? it.x + (dw - bw) : it.x + (dw - bw) / 2;
    dy = cy < bh * 0.35f ? it.y : cy > bh * 0.65f ? it.y + (dbh - bh) : it.y + (dbh - bh) / 2;
}

// zoom level indicator (top right), drawn with GDI once per value
struct Label {
    int value = -1, w = 0, h = 0;
    std::vector<uint8_t> a;               // coverage
    void Make(int pct) {
        if (pct == value) return;
        value = pct;
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = 240; bi.bmiHeader.biHeight = -40; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        uint32_t* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (!bmp) { DeleteDC(dc); return; }
        HGDIOBJ ob = SelectObject(dc, bmp);
        HFONT f = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Arial");
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        wchar_t t[32];
        swprintf(t, 32, L"Zoom %d%%", pct);
        SIZE sz;
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        TextOutW(dc, 2, 2, t, (int)wcslen(t));
        GdiFlush();
        w = std::min(240, (int)sz.cx + 6); h = std::min(40, (int)sz.cy + 6);
        a.assign((size_t)w * h, 0);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) a[(size_t)y * w + x] = (uint8_t)((bits[y * 240 + x] >> 8) & 0xFF);
        SelectObject(dc, of); DeleteObject(f); SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    }
} g_label;

inline void Blend(uint32_t& d, uint32_t rgb, int a) {   // premultiplied "over"
    if (a <= 0) return;
    uint32_t r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    uint32_t dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255, da = d >> 24;
    dr = (r * a + dr * (255 - a)) / 255; dg = (g * a + dg * (255 - a)) / 255; db = (b * a + db * (255 - a)) / 255;
    da = a + da * (255 - a) / 255;
    d = da << 24 | dr << 16 | dg << 8 | db;
}

} // namespace

void FloatText::Record(DDSurface* s, int x, int y, int w, int h, const uint8_t* cov, const uint8_t* written, const uint8_t* bg, uint32_t rgb) {
    if (!Zoom::On() || !IsScreen(s) || w <= 0 || h <= 0) return;
    const int bh = Zoom::InternalH() - Zoom::PanelH();
    if (y >= bh) return;                                   // the panel is not zoomed
    int hh = std::min(h, bh - y);
    Init();
    Item it;
    it.x = x; it.y = y; it.w = w; it.h = hh; it.rgb = rgb;
    it.cov.assign(cov, cov + (size_t)w * hh);
    it.written.assign(written, written + (size_t)w * hh);
    it.bg.assign(bg, bg + (size_t)w * hh);
    EnterCriticalSection(&g_lock);
    // The game redraws texts over themselves without clearing first: a pixel
    // still showing what an earlier text drew inherits that text's background,
    // so every record keeps the real picture underneath.
    std::vector<uint8_t> resolved(it.bg.size(), 0);
    for (int k = (int)g_items.size() - 1; k >= 0; k--) {
        const Item& o = g_items[k];
        int x0 = std::max(it.x, o.x), x1 = std::min(it.x + it.w, o.x + o.w);
        int y0 = std::max(it.y, o.y), y1 = std::min(it.y + it.h, o.y + o.h);
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) {
                size_t i = (size_t)(yy - it.y) * it.w + (xx - it.x), j = (size_t)(yy - o.y) * o.w + (xx - o.x);
                if (!resolved[i] && o.cov[j] && it.bg[i] == o.written[j]) { it.bg[i] = o.bg[j]; resolved[i] = 1; }
            }
    }
    // the same text drawn again at the same place replaces the old record
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [&](const Item& o) {
        return o.x == it.x && o.y == it.y && o.w == it.w && o.h == it.h && o.rgb == it.rgb; }), g_items.end());
    if (g_items.size() > 200) g_items.erase(g_items.begin());
    g_items.push_back(std::move(it));
    g_serial++;
    LeaveCriticalSection(&g_lock);
}

void FloatText::Apply(const DDSurface* screen, uint8_t* display, int pitch, std::vector<int>& rows) {
    if (!g_init || !IsScreen(screen)) return;
    EnterCriticalSection(&g_lock);
    if (g_items.empty()) { LeaveCriticalSection(&g_lock); return; }
    const int W = screen->w_;
    if (g_claim.size() != (size_t)W * screen->h_) g_claim.assign((size_t)W * screen->h_, 0);
    // newest first: a pixel drawn over by a later text belongs to that text.
    // A text is alive while most of the solid core of its letters is still
    // what was drawn (anti-aliased edges can be re-blended or clipped by the
    // game drawing the next line).
    std::vector<bool> alive(g_items.size(), true);
    for (int k = (int)g_items.size() - 1; k >= 0; k--) {
        const Item& it = g_items[k];
        int total = 0, match = 0, allTotal = 0, allMatch = 0;
        for (int y = 0; y < it.h; y++) {
            const uint8_t* src = screen->bits_ + (size_t)(it.y + y) * screen->pitch_ + it.x;
            for (int x = 0; x < it.w; x++) {
                size_t i = (size_t)y * it.w + x;
                if (!it.cov[i] || it.x + x < 0 || it.x + x >= W || g_claim[(size_t)(it.y + y) * W + it.x + x]) continue;
                bool m = src[x] == it.written[i];
                allTotal++; allMatch += m;
                if (it.cov[i] >= 192) { total++; match += m; }   // the solid core of the letters
            }
        }
        // judged by the core; tiny texts without one by all their pixels
        alive[k] = total ? match * 4 >= total * 3 : allTotal && allMatch * 4 >= allTotal * 3;
        if (alive[k])
            for (int y = 0; y < it.h; y++) {
                for (int x = 0; x < it.w; x++) {
                    size_t i = (size_t)y * it.w + x;
                    if (it.cov[i] && it.x + x >= 0 && it.x + x < W) g_claim[(size_t)(it.y + y) * W + it.x + x] = 1;
                }
            }
    }
    // put the background back under the whole letter area (newest first, so
    // older backgrounds win)
    for (int k = (int)g_items.size() - 1; k >= 0; k--) {
        const Item& it = g_items[k];
        for (int y = 0; y < it.h; y++) {
            uint8_t* d = display + (size_t)(it.y + y) * pitch + it.x;
            for (int x = 0; x < it.w; x++) {
                size_t i = (size_t)y * it.w + x;
                if (!it.cov[i] || it.x + x < 0 || it.x + x >= W) continue;
                g_claim[(size_t)(it.y + y) * W + it.x + x] = 0;
                if (alive[k]) d[x] = it.bg[i];
            }
            if (alive[k]) rows.push_back(it.y + y);
        }
    }
    size_t before = g_items.size();
    std::vector<Item> keep;
    for (size_t k = 0; k < g_items.size(); k++) if (alive[k]) keep.push_back(std::move(g_items[k]));
    g_items.swap(keep);
    if (g_items.size() != before) g_serial++;
    LeaveCriticalSection(&g_lock);
}

bool FloatText::Build(std::vector<uint32_t>& px, int& y0, int& y1, bool& any) {
    Init();
    const int dw = Zoom::DisplayW(), dh = Zoom::DisplayH();
    const double since = Now() - Zoom::LastChange();
    const float labelA = Zoom::Animating() || since < 1.5 ? 1.0f : since < 2.0 ? (float)(1.0 - (since - 1.5) / 0.5) : 0.0f;
    static float lastLabelA = -1;
    static int lastPct = -1;
    const int pct = (int)floorf(Zoom::Current() * 100.0f + 0.5f);
    EnterCriticalSection(&g_lock);
    bool changed = g_serial != g_builtSerial || labelA != lastLabelA || (labelA > 0 && pct != lastPct) || px.size() != (size_t)dw * dh;
    if (!changed) { LeaveCriticalSection(&g_lock); return false; }
    g_builtSerial = g_serial; lastLabelA = labelA; lastPct = pct;
    std::vector<uint32_t> old;
    old.swap(px);
    px.assign((size_t)dw * dh, 0);
    const int dbh = dh - Zoom::PanelH();
    for (const Item& it : g_items) {
        int ax, ay;
        Anchor(it, ax, ay);
        for (int y = 0; y < it.h; y++) {
            int ty = ay + y;
            if (ty < 0 || ty >= dbh) continue;
            for (int x = 0; x < it.w; x++) {
                int tx = ax + x;
                if (tx < 0 || tx >= dw) continue;
                Blend(px[(size_t)ty * dw + tx], it.rgb, it.cov[(size_t)y * it.w + x]);
            }
        }
    }
    any = !g_items.empty();
    LeaveCriticalSection(&g_lock);
    if (labelA > 0) {
        g_label.Make(pct);
        int lx = dw - g_label.w - 12, ly = 8;
        for (int pass = 0; pass < 2; pass++)   // dark outline, then white text
            for (int y = 0; y < g_label.h; y++)
                for (int x = 0; x < g_label.w; x++) {
                    int a = g_label.a[(size_t)y * g_label.w + x];
                    if (!a) continue;
                    if (pass == 0) {
                        for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
                            int tx = lx + x + ox, ty = ly + y + oy;
                            if (tx >= 0 && tx < dw && ty >= 0 && ty < dbh) Blend(px[(size_t)ty * dw + tx], 0x000000, (int)(a * labelA * 0.8f));
                        }
                    } else {
                        int tx = lx + x, ty = ly + y;
                        if (tx >= 0 && tx < dw && ty >= 0 && ty < dbh) Blend(px[(size_t)ty * dw + tx], 0xFFFFFF, (int)(a * labelA));
                    }
                }
        any = true;
    }
    // changed rows
    y0 = dh; y1 = 0;
    if (old.size() != px.size()) { y0 = 0; y1 = dh; }
    else
        for (int y = 0; y < dh; y++)
            if (memcmp(&old[(size_t)y * dw], &px[(size_t)y * dw], (size_t)dw * 4)) { y0 = std::min(y0, y); y1 = y + 1; }
    return y1 > y0;
}

} // namespace swgbm
