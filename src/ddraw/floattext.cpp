// Interface text in its own layer (see floattext.h).
#include "floattext.h"
#include "ddraw_impl.h"
#include "zoom.h"
#include <algorithm>
#include <math.h>

namespace swgbm {
namespace {

struct Item {
    const DDSurface* surf = nullptr;
    int x = 0, y = 0, w = 0, h = 0;       // box in the surface
    std::vector<uint8_t> cov, written, bg; // w*h
    FloatText::Draw d;
    uint64_t sig = 0;                     // what is drawn (text, font, colour, place)
    // state from the last screen blit
    bool alive = false, shown = false;
    int offX = 0, offY = 0;               // surface -> frame
    std::vector<uint8_t> occl;            // w*h, 1 = drawn over (empty = nothing)
    uint64_t occlHash = 0;
    int gone = 0;                         // screen blits since it was last seen
    bool stale = false;                   // the picture around it changed: bg no longer valid
    // HD rendering cache
    float sKx = 0, sKy = 0;
    int sw = 0, sh = 0, sPad = 0;
    std::vector<uint8_t> sprite;          // coverage, sw*sh
};

CRITICAL_SECTION g_lock;
bool g_init = false;
bool g_hd = false;
std::vector<Item> g_items;
unsigned g_serial = 1, g_builtSerial = 0;
uint64_t g_shownHash = 0;
std::vector<uint8_t> g_claim;             // scratch
struct RowPatch { int y, x; uint8_t v; };
std::vector<RowPatch> g_patches;             // sorted by row (display coordinates)
std::vector<int> g_rowStart;              // display row -> first patch (+1 past end)
std::vector<uint8_t> g_rowBuf;

void Init() {
    if (!g_init) { InitializeCriticalSection(&g_lock); g_init = true; }
}

bool IsZoomScreen(const DDSurface* s) {
    return s && !s->IsPrimary() && s->w_ == Zoom::InternalW() && s->h_ == Zoom::InternalH();
}

double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

inline uint64_t Mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h; }

uint64_t Signature(const Item& it) {
    uint64_t h = Mix(Mix(Mix(Mix(1, (uint64_t)(uintptr_t)it.surf), (uint64_t)(uint32_t)it.x << 32 | (uint32_t)it.y),
                         (uint64_t)(uint32_t)it.w << 32 | (uint32_t)it.h), it.d.rgb);
    h = Mix(h, it.d.align); h = Mix(h, (uint64_t)(uint32_t)it.d.charExtra); h = Mix(h, (uint64_t)it.d.ink); h = Mix(h, it.d.blend);
    const uint8_t* f = (const uint8_t*)&it.d.font;
    for (size_t i = 0; i < sizeof(LOGFONTW); i++) h = Mix(h, f[i]);
    for (const auto& l : it.d.lines) {
        h = Mix(h, (uint64_t)(uint32_t)l.x << 32 | (uint32_t)l.y);
        h = Mix(h, l.opts);
        for (wchar_t c : l.text) h = Mix(h, c);
        for (int a : l.adv) h = Mix(h, (uint32_t)a);
    }
    return h;
}

// A text in the middle of the battlefield (a dialog, "Game Paused") belongs to
// the zoomed picture: it follows the zoom in place and size.
bool Centred(int fx, int fy, int w, int h) {
    const int bw = Zoom::InternalW(), bh = Zoom::InternalH() - Zoom::PanelH();
    if (fy >= bh) return false;
    float cx = fx + w * 0.5f, cy = fy + h * 0.5f;
    return cx >= bw * 0.35f && cx <= bw * 0.65f && cy >= bh * 0.35f && cy <= bh * 0.65f;
}

// Where a battlefield text goes on the shown (zoomed) battlefield: same
// distance from the nearest edge (left/right/top/bottom) or from the centre.
// Panel texts stay where they are relative to the panel.
void Place(const Item& it, int fx, int fy, int& dx, int& dy) {
    const int bw = Zoom::InternalW(), bh = Zoom::InternalH() - Zoom::PanelH();
    const int dw = Zoom::DisplayW(), dbh = Zoom::DisplayH() - Zoom::PanelH();
    if (fy >= bh) { dx = fx; dy = fy - bh + dbh; return; }
    float cx = fx + it.w * 0.5f, cy = fy + it.h * 0.5f;
    dx = cx < bw * 0.35f ? fx : cx > bw * 0.65f ? fx + (dw - bw) : fx + (dw - bw) / 2;
    dy = cy < bh * 0.35f ? fy : cy > bh * 0.65f ? fy + (dbh - bh) : fy + (dbh - bh) / 2;
}

// ---------------------------------------------------------------- HD drawing
struct Canvas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    uint32_t* bits = nullptr;
    int w = 0, h = 0;
    bool Ensure(int nw, int nh) {
        if (bits && nw <= w && nh <= h) return true;
        if (!dc) dc = CreateCompatibleDC(nullptr);
        if (bmp) { DeleteObject(bmp); bmp = nullptr; bits = nullptr; }
        w = std::max(nw, w); h = std::max(nh, h);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (!bmp) return false;
        SelectObject(dc, bmp);
        return true;
    }
} g_canvas;

HFONT MakeFont(const LOGFONTW& base, float ky, float kx, int widthOverride) {
    LOGFONTW lf = base;
    lf.lfHeight = (LONG)lroundf(base.lfHeight * ky);
    if (!lf.lfHeight) lf.lfHeight = base.lfHeight < 0 ? -1 : 1;
    lf.lfWidth = widthOverride ? widthOverride : (LONG)lroundf(base.lfWidth * kx);
    lf.lfQuality = ANTIALIASED_QUALITY;
    return CreateFontIndirectW(&lf);
}

// The text of one item, redrawn at kx x ky into it.sprite (coverage). Each line
// starts where GDI started it and uses the scaled font's own spacing, so it
// reads like text drawn at that size; a line that would come out wider than the
// original scaled up is narrowed to fit (layout and centring stay the same).
void RenderSprite(Item& it, float kx, float ky) {
    it.sKx = kx; it.sKy = ky;
    it.sPad = (int)ceilf(3 * ky);
    it.sw = (int)ceilf(it.w * kx) + 2 * it.sPad;
    it.sh = (int)ceilf(it.h * ky) + 2 * it.sPad;
    it.sprite.assign((size_t)it.sw * it.sh, 0);
    if (!g_canvas.Ensure(it.sw, it.sh)) return;
    HDC dc = g_canvas.dc;
    for (int y = 0; y < it.sh; y++) memset(g_canvas.bits + (size_t)y * g_canvas.w, 0, (size_t)it.sw * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    const UINT align = it.d.align & ~TA_UPDATECP;
    SetTextAlign(dc, align);
    SetTextCharacterExtra(dc, (int)lroundf(it.d.charExtra * kx));
    // 1x metrics of the original font (baseline of top-aligned lines)
    HFONT f1 = CreateFontIndirectW(&it.d.font);
    HGDIOBJ old = SelectObject(dc, f1);
    TEXTMETRICW tm1 = {};
    GetTextMetricsW(dc, &tm1);
    HFONT fh = MakeFont(it.d.font, ky, kx, 0);
    SelectObject(dc, fh);
    TEXTMETRICW tmh = {};
    GetTextMetricsW(dc, &tmh);
    for (const auto& l : it.d.lines) {
        if (l.text.empty()) continue;
        int w1 = 0;
        for (int a : l.adv) w1 += a;
        const bool glyphs = (l.opts & ETO_GLYPH_INDEX) != 0;
        SIZE nat = {};
        if (glyphs) GetTextExtentPointI(dc, (LPWORD)l.text.data(), (int)l.text.size(), &nat);
        else GetTextExtentPoint32W(dc, l.text.data(), (int)l.text.size(), &nat);
        HFONT narrow = nullptr;
        const float target = w1 * kx;
        if (w1 > 0 && nat.cx > target + std::max(1.0f, kx)) {
            int avg = std::max(1, (int)lroundf(tmh.tmAveCharWidth * target / nat.cx));
            narrow = MakeFont(it.d.font, ky, kx, avg);
            SelectObject(dc, narrow);
        }
        int X = (int)lroundf((l.x - it.x) * kx) + it.sPad;
        int Y;
        if (align & (TA_BASELINE | TA_BOTTOM)) Y = (int)lroundf((l.y - it.y) * ky) + it.sPad;
        else Y = (int)lroundf((l.y - it.y + tm1.tmAscent) * ky) - tmh.tmAscent + it.sPad;
        RECT clip, *pc = nullptr;
        UINT opts = l.opts & (ETO_GLYPH_INDEX | ETO_RTLREADING);
        if (l.opts & ETO_CLIPPED) {
            clip = {(LONG)lroundf((l.clip.left - it.x) * kx) + it.sPad, (LONG)lroundf((l.clip.top - it.y) * ky) + it.sPad,
                    (LONG)lroundf((l.clip.right - it.x) * kx) + it.sPad, (LONG)lroundf((l.clip.bottom - it.y) * ky) + it.sPad};
            pc = &clip;
            opts |= ETO_CLIPPED;
        }
        ExtTextOutW(dc, X, Y, opts, pc, l.text.data(), (UINT)l.text.size(), nullptr);
        if (narrow) { SelectObject(dc, fh); DeleteObject(narrow); }
    }
    GdiFlush();
    SelectObject(dc, old);
    DeleteObject(fh);
    DeleteObject(f1);
    for (int y = 0; y < it.sh; y++) {
        const uint32_t* r = g_canvas.bits + (size_t)y * g_canvas.w;
        uint8_t* o = it.sprite.data() + (size_t)y * it.sw;
        for (int x = 0; x < it.sw; x++) o[x] = (uint8_t)((r[x] >> 8) & 0xFF);
    }
}

inline void Blend(uint32_t& d, uint32_t rgb, int a) {   // premultiplied "over"
    if (a <= 0) return;
    if (a >= 255) { d = 0xFF000000u | rgb; return; }
    uint32_t r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    uint32_t dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255, da = d >> 24;
    dr = (r * a + dr * (255 - a)) / 255; dg = (g * a + dg * (255 - a)) / 255; db = (b * a + db * (255 - a)) / 255;
    da = a + da * (255 - a) / 255;
    d = da << 24 | dr << 16 | dg << 8 | db;
}

// zoom level indicator (top right), drawn with GDI once per value and size
struct Label {
    int value = -1, size = 0, w = 0, h = 0;
    std::vector<uint8_t> a;
    void Make(int pct, int px) {
        if (pct == value && px == size) return;
        value = pct; size = px;
        int bw = px * 12, bh = px * 2;
        if (!g_canvas.Ensure(bw, bh)) return;
        HDC dc = g_canvas.dc;
        for (int y = 0; y < bh; y++) memset(g_canvas.bits + (size_t)y * g_canvas.w, 0, (size_t)bw * 4);
        HFONT f = CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Arial");
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        SetTextAlign(dc, TA_TOP | TA_LEFT);
        SetTextCharacterExtra(dc, 0);
        wchar_t t[32];
        swprintf(t, 32, L"Zoom %d%%", pct);
        SIZE sz;
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        TextOutW(dc, 2, 2, t, (int)wcslen(t));
        GdiFlush();
        w = std::min(bw, (int)sz.cx + 6); h = std::min(bh, (int)sz.cy + 6);
        a.assign((size_t)w * h, 0);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) a[(size_t)y * w + x] = (uint8_t)((g_canvas.bits[(size_t)y * g_canvas.w + x] >> 8) & 0xFF);
        SelectObject(dc, of); DeleteObject(f);
    }
} g_label;

} // namespace

void FloatText::Configure(bool hd) { Init(); g_hd = hd; }

bool FloatText::Wanted(const DDSurface* s) {
    if (!s || s->IsPrimary()) return false;
    return g_hd || (Zoom::On() && IsZoomScreen(s));
}

void FloatText::Record(DDSurface* s, int x, int y, int w, int h, const uint8_t* cov, const uint8_t* written, const uint8_t* bg, Draw&& d) {
    if (!Wanted(s) || w <= 0 || h <= 0) return;
    if (!g_hd) {   // zoom only: texts at the battlefield's edges (the panel is not zoomed)
        const int bh = Zoom::InternalH() - Zoom::PanelH();
        if (y >= bh || Centred(x, y, w, std::min(h, bh - y))) return;
        h = std::min(h, bh - y);
    }
    Init();
    Item it;
    it.surf = s; it.x = x; it.y = y; it.w = w; it.h = h;
    it.d = std::move(d);
    it.sig = Signature(it);
    EnterCriticalSection(&g_lock);
    // the game redraws most texts every frame: the same text with the same
    // result needs nothing new
    // (it still moves to the end: the list is in drawing order - a shadow
    // drawn before its text must stay older than it)
    for (size_t k = 0; k < g_items.size(); k++) {
        const Item& o = g_items[k];
        if (o.surf == s && o.sig == it.sig && !memcmp(o.written.data(), written, (size_t)w * h)) {
            std::rotate(g_items.begin() + k, g_items.begin() + k + 1, g_items.end());
            LeaveCriticalSection(&g_lock);
            return;
        }
    }
    it.cov.assign(cov, cov + (size_t)w * h);
    it.written.assign(written, written + (size_t)w * h);
    it.bg.assign(bg, bg + (size_t)w * h);
    // The game redraws texts over themselves without clearing first: a pixel
    // still showing what an earlier text drew inherits that text's background,
    // so every record keeps the real picture underneath.
    std::vector<uint8_t> resolved(it.bg.size(), 0);
    for (int k = (int)g_items.size() - 1; k >= 0; k--) {
        const Item& o = g_items[k];
        if (o.surf != s) continue;
        int x0 = std::max(it.x, o.x), x1 = std::min(it.x + it.w, o.x + o.w);
        int y0 = std::max(it.y, o.y), y1 = std::min(it.y + it.h, o.y + o.h);
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) {
                size_t i = (size_t)(yy - it.y) * it.w + (xx - it.x), j = (size_t)(yy - o.y) * o.w + (xx - o.x);
                if (!resolved[i] && it.cov[i] && o.cov[j] && it.bg[i] == o.written[j]) { it.bg[i] = o.bg[j]; resolved[i] = 1; }
            }
    }
    // a pixel that already had the text's colour and is not an earlier text's
    // is the background's own
    bool anyPx = false;
    for (size_t i = 0; i < it.cov.size(); i++) {
        if (it.cov[i] && !resolved[i] && it.bg[i] == it.written[i]) it.cov[i] = 0;
        anyPx = anyPx || it.cov[i];
    }
    if (!anyPx) { LeaveCriticalSection(&g_lock); return; }
    // the same text drawn again replaces the old record (keeping its HD image);
    // so does any text drawn again with the same box and colour
    for (size_t k = 0; k < g_items.size(); k++) {
        Item& o = g_items[k];
        if (o.surf != s) continue;
        if (o.sig == it.sig) {
            it.sKx = o.sKx; it.sKy = o.sKy; it.sw = o.sw; it.sh = o.sh; it.sPad = o.sPad; it.sprite.swap(o.sprite);
            it.alive = o.alive; it.shown = o.shown; it.offX = o.offX; it.offY = o.offY; it.occlHash = o.occlHash; it.occl.swap(o.occl);
        }
    }
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [&](const Item& o) {
        return o.surf == s && (o.sig == it.sig || (o.x == it.x && o.y == it.y && o.w == it.w && o.h == it.h && o.d.rgb == it.d.rgb)); }), g_items.end());
    // keep the list bounded: drop the oldest texts that are not on screen
    if (g_items.size() > 400) {
        size_t drop = g_items.size() - 400;
        g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [&](const Item& o) { return drop && !o.shown && drop--; }), g_items.end());
        if (g_items.size() > 400) g_items.erase(g_items.begin(), g_items.begin() + (g_items.size() - 400));
    }
    g_items.push_back(std::move(it));
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------- surface to surface
// Some screens draw their text into a surface of their own and copy that into
// the back buffer (the campaign briefing): the texts move along with 1:1 copies.
struct Moving {
    int x, y, w, h;                      // in the destination
    std::vector<uint8_t> cov, ink, bg;   // ink = the text's pixel values
    FloatText::Draw d;
};
std::vector<Moving> g_moving;

bool FloatText::TransferBegin(const DDSurface* src, DDSurface* dst, const RECT& d, const RECT& s, bool keyed, int keyLo, int keyHi) {
    g_moving.clear();
    if (!g_init || src == dst || !Wanted(dst)) return false;
    const int offX = d.left - s.left, offY = d.top - s.top;
    EnterCriticalSection(&g_lock);
    for (const Item& it : g_items) {
        if (it.surf != src) continue;
        int x0 = std::max<int>(it.x, s.left), x1 = std::min<int>(it.x + it.w, s.right);
        int y0 = std::max<int>(it.y, s.top), y1 = std::min<int>(it.y + it.h, s.bottom);
        if (x1 <= x0 || y1 <= y0) continue;
        // only a text still in the source
        int total = 0, match = 0;
        for (int y = y0; y < y1; y++) {
            const uint8_t* row = src->bits_ + (size_t)y * src->pitch_;
            for (int x = x0; x < x1; x++) {
                size_t i = (size_t)(y - it.y) * it.w + (x - it.x);
                if (!it.cov[i]) continue;
                total++; match += row[x] == it.written[i];
            }
        }
        if (!total || match * 4 < total * 3) continue;
        Moving m;
        m.x = x0 + offX; m.y = y0 + offY; m.w = x1 - x0; m.h = y1 - y0;
        if (m.x < 0 || m.y < 0 || m.x + m.w > dst->w_ || m.y + m.h > dst->h_) continue;
        m.cov.resize((size_t)m.w * m.h); m.ink.resize(m.cov.size()); m.bg.resize(m.cov.size());
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                size_t i = (size_t)(y0 - it.y + y) * it.w + (x0 - it.x + x), j = (size_t)y * m.w + x;
                m.cov[j] = it.cov[i]; m.ink[j] = it.written[i];
                // under the text after the copy: what was under it in the
                // source, unless that is transparent (colour key)
                uint8_t sb = it.bg[i];
                bool through = keyed && sb >= keyLo && sb <= keyHi;
                m.bg[j] = through ? dst->bits_[(size_t)(m.y + y) * dst->pitch_ + m.x + x] : sb;
            }
        m.d = it.d;
        for (auto& l : m.d.lines) {
            l.x += offX; l.y += offY;
            OffsetRect(&l.clip, offX, offY);
        }
        g_moving.push_back(std::move(m));
    }
    // A picture made from a surface with texts (a fade: the game blends the
    // text surface into another one and copies that): a text still in its own
    // surface whose letters stand out here from the pixels beside them, in a
    // few colours, moves along in the most common one. What is under its
    // letters is taken from the pixels beside them.
    if (g_moving.empty() && !keyed)
        for (const Item& it : g_items) {
            const DDSurface* home = it.surf;
            if (home == src || home == dst || home->w_ != src->w_ || home->h_ != src->h_) continue;
            int x0 = std::max<int>(it.x, s.left), x1 = std::min<int>(it.x + it.w, s.right);
            int y0 = std::max<int>(it.y, s.top), y1 = std::min<int>(it.y + it.h, s.bottom);
            if (x1 - x0 < 4 || y1 - y0 < 4) continue;
            int total = 0, match = 0, hist[256] = {};
            for (int y = y0; y < y1; y++) {
                const uint8_t* hr = home->bits_ + (size_t)y * home->pitch_;
                const uint8_t* sr = src->bits_ + (size_t)y * src->pitch_;
                for (int x = x0; x < x1; x++) {
                    size_t i = (size_t)(y - it.y) * it.w + (x - it.x);
                    if (!it.cov[i]) continue;
                    total++; match += hr[x] == it.written[i];
                }
            }
            if (total < 8 || match * 4 < total * 3) continue;   // gone from its own surface
            Moving m;
            m.x = x0 + offX; m.y = y0 + offY; m.w = x1 - x0; m.h = y1 - y0;
            if (m.x < 0 || m.y < 0 || m.x + m.w > dst->w_ || m.y + m.h > dst->h_) continue;
            m.cov.assign((size_t)m.w * m.h, 0); m.ink.assign(m.cov.size(), 0); m.bg.assign(m.cov.size(), 0);
            int letters = 0, visible = 0, v = 0;
            for (int y = 0; y < m.h; y++) {
                const uint8_t* sr = src->bits_ + (size_t)(y0 + y) * src->pitch_ + x0;
                for (int x = 0; x < m.w; x++) {
                    size_t i = (size_t)(y0 - it.y + y) * it.w + (x0 - it.x + x), j = (size_t)y * m.w + x;
                    m.ink[j] = sr[x];
                    if (!it.cov[i]) { m.bg[j] = sr[x]; continue; }
                    // nearest pixel beside it that is not a letter
                    uint8_t fill = sr[x];
                    for (int r = 1; r < 8; r++) {
                        if (x - r >= 0 && !it.cov[i - r]) { fill = sr[x - r]; break; }
                        if (x + r < m.w && !it.cov[i + r]) { fill = sr[x + r]; break; }
                    }
                    m.bg[j] = fill;
                    m.cov[j] = 255;
                    letters++;
                    if (sr[x] != fill) { visible++; hist[sr[x]]++; }
                }
            }
            // the letters stand out from what is beside them
            if (letters < 8 || visible * 10 < letters * 7) continue;
            // ... in a few colours (the text blended over a few background
            // shades), not the many of a textured picture
            int top[3] = {0, 0, 0};
            for (int k = 0; k < 256; k++) {
                int c = hist[k];
                if (c > top[0]) { top[2] = top[1]; top[1] = top[0]; top[0] = c; v = k; }
                else if (c > top[1]) { top[2] = top[1]; top[1] = c; }
                else if (c > top[2]) top[2] = c;
            }
            if ((top[0] + top[1] + top[2]) * 100 < visible * 85) continue;
            m.d = it.d;
            m.d.ink = v;
            // shown in the letters' average colour
            const DDPalette* pp = src->pal_ ? src->pal_ : State().primaryPal;
            if (pp) {
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int k = 0; k < 256; k++) {
                    if (!hist[k]) continue;
                    r += pp->entries_[k].peRed * hist[k]; g += pp->entries_[k].peGreen * hist[k]; b += pp->entries_[k].peBlue * hist[k]; n += hist[k];
                }
                if (n) m.d.blend = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
            for (auto& l : m.d.lines) {
                l.x += offX; l.y += offY;
                OffsetRect(&l.clip, offX, offY);
            }
            g_moving.push_back(std::move(m));
        }
    LeaveCriticalSection(&g_lock);
    return !g_moving.empty();
}

void FloatText::TransferEnd(DDSurface* dst) {
    for (Moving& m : g_moving) {
        std::vector<uint8_t> written(m.cov.size());
        bool any = false;
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                size_t j = (size_t)y * m.w + x;
                written[j] = dst->bits_[(size_t)(m.y + y) * dst->pitch_ + m.x + x];
                if (m.cov[j] && written[j] != m.ink[j]) m.cov[j] = 0;   // not copied (colour key)
                any = any || m.cov[j];
            }
        if (any) Record(dst, m.x, m.y, m.w, m.h, m.cov.data(), written.data(), m.bg.data(), std::move(m.d));
    }
    g_moving.clear();
}

bool FloatText::Prepare(const DDSurface* src, const RECT& d, const RECT& s, int pw, int ph) {
    g_patches.clear();
    if (!g_init) return false;
    EnterCriticalSection(&g_lock);
    const bool full = (int64_t)(d.right - d.left) * (d.bottom - d.top) * 2 >= (int64_t)pw * ph;
    bool hasItems = false;
    for (const Item& it : g_items) if (it.surf == src) { hasItems = true; break; }
    if (!hasItems) {
        // a full-screen picture without our texts: nothing of ours is visible
        bool changed = false;
        if (full) for (Item& it : g_items) if (it.shown) { it.shown = false; changed = true; }
        if (changed) { g_shownHash = 0; g_serial++; }
        LeaveCriticalSection(&g_lock);
        return false;
    }
    const int W = src->w_, H = src->h_;
    if (g_claim.size() != (size_t)W * H) g_claim.assign((size_t)W * H, 0);
    const int offX = d.left - s.left, offY = d.top - s.top;
    // newest first: a pixel drawn over by a later text belongs to that text.
    // A text is alive while most of its pixels still hold what it drew;
    // pixels that no longer do (a cursor or menu drawn over it) hide that part.
    for (int k = (int)g_items.size() - 1; k >= 0; k--) {
        Item& it = g_items[k];
        if (it.surf != src) { if (full && it.shown) it.shown = false; continue; }
        int total = 0, match = 0, around = 0, aroundSame = 0;
        bool anyOccl = false;
        if (it.occl.size() != it.cov.size()) it.occl.assign(it.cov.size(), 0);
        uint64_t oh = 1;
        for (int y = 0; y < it.h; y++) {
            int sy = it.y + y;
            if (sy < 0 || sy >= H) continue;
            const uint8_t* row = src->bits_ + (size_t)sy * src->pitch_;
            for (int x = 0; x < it.w; x++) {
                int sx = it.x + x;
                size_t i = (size_t)y * it.w + x;
                if (sx < 0 || sx >= W) { it.occl[i] = 1; continue; }
                // a pixel a later text drew over belongs to that text (not
                // "covered": the later text is drawn on top anyway)
                if (g_claim[(size_t)sy * W + sx]) { it.occl[i] = 0; continue; }
                bool same = row[sx] == it.written[i];
                it.occl[i] = same ? 0 : 1;
                if (!same) { anyOccl = true; oh = Mix(oh, i); }
                if (it.cov[i]) { total++; match += same; }
                else { around++; aroundSame += same; }
            }
        }
        // its letters are still there, and so is what was around them (a
        // text in the background's colour - a black shadow on a screen
        // cleared to black - "matches" after being erased)
        it.alive = total && match * 4 >= total * 3 && (!around || aroundSame * 2 >= around);
        // the picture around the letters is no longer the one recorded (the
        // game redrew it - scrolling - and drew the text again over itself):
        // what was under the letters is unknown, taken from beside them
        it.stale = around && aroundSame * 20 < around * 19;
        // gone for good: covered by newer texts, or not seen for ~1.5 s
        it.gone = it.alive ? 0 : total ? it.gone + 1 : 1000;
        if (!anyOccl) it.occl.clear();
        it.occlHash = it.alive ? oh : 0;
        const bool inside = it.x < s.right && it.x + it.w > s.left && it.y < s.bottom && it.y + it.h > s.top;
        it.shown = it.alive && inside;
        it.offX = offX; it.offY = offY;
        if (it.alive)
            for (int y = 0; y < it.h; y++)
                for (int x = 0; x < it.w; x++) {
                    size_t i = (size_t)y * it.w + x;
                    int sx = it.x + x, sy = it.y + y;
                    if (it.cov[i] && sx >= 0 && sx < W && sy >= 0 && sy < H) g_claim[(size_t)sy * W + sx] = 1;
                }
    }
    // the background back under every visible text (newest first, so older
    // backgrounds win), only where the text's pixels are still there
    for (int k = (int)g_items.size() - 1; k >= 0; k--) {
        Item& it = g_items[k];
        if (it.surf != src) continue;
        for (int y = 0; y < it.h; y++) {
            int sy = it.y + y;
            for (int x = 0; x < it.w; x++) {
                size_t i = (size_t)y * it.w + x;
                int sx = it.x + x;
                if (!it.cov[i] || sx < 0 || sx >= W || sy < 0 || sy >= H) continue;
                g_claim[(size_t)sy * W + sx] = 0;
                if (!it.shown || (!it.occl.empty() && it.occl[i])) continue;
                if (sx < s.left || sx >= s.right || sy < s.top || sy >= s.bottom) continue;
                uint8_t v = it.bg[i];
                if (it.stale) {   // nearest pixel beside it in the row that is not a letter
                    const uint8_t* row = src->bits_ + (size_t)sy * src->pitch_;
                    for (int r = 1; r < 12; r++) {
                        if (x - r >= 0 && !it.cov[i - r]) { v = row[sx - r]; break; }
                        if (x + r < it.w && !it.cov[i + r]) { v = row[sx + r]; break; }
                    }
                }
                g_patches.push_back({sy + offY, sx + offX, v});
            }
        }
    }
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [&](const Item& it) { return it.surf == src && it.gone > 90; }), g_items.end());
    // what is visible now (redraw the overlay only when this changes)
    uint64_t sh = 7;
    for (const Item& it : g_items) if (it.shown) { sh = Mix(sh, it.sig); sh = Mix(sh, it.occlHash); sh = Mix(sh, (uint64_t)(uint32_t)it.offX << 32 | (uint32_t)it.offY); }
    if (sh != g_shownHash) { g_shownHash = sh; g_serial++; }
    LeaveCriticalSection(&g_lock);
    if (g_patches.empty()) return false;
    // group by row, keeping the order within a row (counting sort)
    int maxY = 0;
    for (const RowPatch& p : g_patches) maxY = std::max(maxY, p.y + 1);
    static std::vector<int> count;
    static std::vector<RowPatch> sorted;
    count.assign((size_t)maxY + 1, 0);
    for (const RowPatch& p : g_patches) count[p.y + 1]++;
    for (int y = 0; y < maxY; y++) count[y + 1] += count[y];
    g_rowStart.assign((size_t)maxY + 1, -1);
    for (int y = 0; y < maxY; y++) if (count[y + 1] > count[y]) g_rowStart[y] = count[y];
    sorted.resize(g_patches.size());
    for (const RowPatch& p : g_patches) sorted[count[p.y]++] = p;
    g_patches.swap(sorted);
    return true;
}

const uint8_t* FloatText::Patch(int dy, const uint8_t* row, int dx0, int w) {
    if (g_patches.empty() || dy < 0 || dy + 1 >= (int)g_rowStart.size() || g_rowStart[dy] < 0) return row;
    if ((int)g_rowBuf.size() < w) g_rowBuf.resize(w);
    memcpy(g_rowBuf.data(), row, w);
    for (size_t i = g_rowStart[dy]; i < g_patches.size() && g_patches[i].y == dy; i++) {
        int x = g_patches[i].x - dx0;
        if (x >= 0 && x < w) g_rowBuf[x] = g_patches[i].v;
    }
    return g_rowBuf.data();
}

bool FloatText::Build(std::vector<uint32_t>& px, int cw, int ch, int frameW, int frameH, const uint32_t* palette, int& y0, int& y1, bool& any) {
    Init();
    if (cw <= 0 || ch <= 0 || frameW <= 0 || frameH <= 0) return false;
    const bool zoomed = Zoom::On() && frameW == Zoom::InternalW() && frameH == Zoom::InternalH();
    if (!g_hd && !zoomed) {   // the layer is not used (no memory for it either)
        bool had = !px.empty();
        std::vector<uint32_t>().swap(px);
        y0 = y1 = 0; any = false;
        return had;
    }
    const int dispW = zoomed ? Zoom::DisplayW() : frameW, dispH = zoomed ? Zoom::DisplayH() : frameH;
    const float kx = (float)cw / dispW, ky = (float)ch / dispH;
    double since = zoomed ? Now() - Zoom::LastChange() : 99;
    const float labelA = zoomed && (Zoom::Animating() || since < 1.5) ? 1.0f : since < 2.0 ? (float)(1.0 - (since - 1.5) / 0.5) : 0.0f;
    static float lastLabelA = -1, lastKx = 0, lastKy = 0, lastZoom = 0;
    const float zoomNow = zoomed ? Zoom::Current() : 1.0f;
    static int lastPct = -1;
    static uint32_t lastPal[256];
    const int pct = (int)floorf(Zoom::Current() * 100.0f + 0.5f);
    EnterCriticalSection(&g_lock);
    bool palChanged = memcmp(lastPal, palette, sizeof(lastPal)) != 0;
    bool changed = g_serial != g_builtSerial || labelA != lastLabelA || (labelA > 0 && pct != lastPct) || px.size() != (size_t)cw * ch ||
                   kx != lastKx || ky != lastKy || palChanged || zoomNow != lastZoom;
    if (!changed) { LeaveCriticalSection(&g_lock); return false; }
    g_builtSerial = g_serial; lastLabelA = labelA; lastPct = pct; lastKx = kx; lastKy = ky; lastZoom = zoomNow;
    memcpy(lastPal, palette, sizeof(lastPal));
    // only the rows drawn last time are cleared and reported, with the rows
    // drawn now
    static int lastY0 = 0, lastY1 = 0;
    const bool fresh = px.size() != (size_t)cw * ch;
    if (fresh) { px.assign((size_t)cw * ch, 0); lastY0 = 0; lastY1 = ch; }
    else if (lastY1 > lastY0) memset(px.data() + (size_t)lastY0 * cw, 0, (size_t)(lastY1 - lastY0) * cw * 4);
    int ny0 = ch, ny1 = 0;
    auto rows = [&](int a, int b) { a = std::max(a, 0); b = std::min(b, ch); if (b > a) { ny0 = std::min(ny0, a); ny1 = std::max(ny1, b); } };
    any = false;
    // the zoomed battlefield's texts stay out of the panel and vice versa
    const int panelTop = zoomed ? (int)lroundf((dispH - Zoom::PanelH()) * ky) : ch;
    for (Item& it : g_items) {
        if (!it.shown) continue;
        int fx = it.x + it.offX, fy = it.y + it.offY, dx = fx, dy = fy;
        float z = 1;                               // the text's own scale
        float ox, oy;                              // item origin on the canvas
        if (zoomed && Centred(fx, fy, it.w, it.h)) {
            float gx, gy;
            Zoom::GameToDisplay((float)fx, (float)fy, gx, gy);
            z = Zoom::Current();
            ox = gx * kx; oy = gy * ky;
        } else {
            if (zoomed) Place(it, fx, fy, dx, dy);
            ox = dx * kx; oy = dy * ky;
        }
        const float tkx = kx * z, tky = ky * z;
        const bool inPanel = zoomed && fy >= Zoom::InternalH() - Zoom::PanelH();
        const int yLo = inPanel ? panelTop : 0, yHi = inPanel ? ch : panelTop;
        any = true;
        if (g_hd) {
            const uint32_t ink = it.d.blend ? it.d.blend & 0xFFFFFF : palette[it.d.ink & 255] & 0xFFFFFF;   // the colour the game's text has on screen
            if (it.sKx != tkx || it.sKy != tky || it.sprite.empty()) RenderSprite(it, tkx, tky);
            const int bx = (int)lroundf(ox) - it.sPad, by = (int)lroundf(oy) - it.sPad;
            rows(by, by + it.sh);
            for (int y = 0; y < it.sh; y++) {
                int ty = by + y;
                if (ty < yLo || ty >= yHi || ty >= ch) continue;
                const uint8_t* a = it.sprite.data() + (size_t)y * it.sw;
                uint32_t* d = px.data() + (size_t)ty * cw;
                int gy = (int)floorf((ty - oy) / tky);   // item row under this canvas row
                for (int x = 0; x < it.sw; x++) {
                    if (!a[x]) continue;
                    int tx = bx + x;
                    if (tx < 0 || tx >= cw) continue;
                    if (!it.occl.empty()) {   // a part drawn over in the game stays hidden
                        int gx = (int)floorf((tx - ox) / tkx);
                        if (gx >= 0 && gx < it.w && gy >= 0 && gy < it.h && it.occl[(size_t)gy * it.w + gx]) continue;
                    }
                    Blend(d[tx], ink, a[x]);
                }
            }
        } else {
            // the game's own pixels, unscaled (zoom), each a kx x ky block
            rows((int)floorf(oy), (int)ceilf(oy + it.h * tky) + 1);
            for (int y = 0; y < it.h; y++) {
                int ty0 = (int)lroundf(oy + y * tky), ty1 = (int)lroundf(oy + (y + 1) * tky);
                for (int x = 0; x < it.w; x++) {
                    size_t i = (size_t)y * it.w + x;
                    if (!it.cov[i] || (!it.occl.empty() && it.occl[i])) continue;
                    uint32_t c = 0xFF000000u | (palette[it.written[i]] & 0xFFFFFF);
                    int tx0 = (int)lroundf(ox + x * tkx), tx1 = (int)lroundf(ox + (x + 1) * tkx);
                    for (int ty = std::max(ty0, yLo); ty < std::min({ty1, yHi, ch}); ty++)
                        for (int tx = std::max(tx0, 0); tx < std::min(tx1, cw); tx++) px[(size_t)ty * cw + tx] = c;
                }
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    if (labelA > 0) {
        g_label.Make(pct, std::max(8, (int)lroundf(22 * ky)));
        int lx = cw - g_label.w - (int)lroundf(12 * kx), ly = (int)lroundf(8 * ky);
        rows(ly - 4, ly + g_label.h + 4);
        for (int pass = 0; pass < 2; pass++)   // dark outline, then white text
            for (int y = 0; y < g_label.h; y++)
                for (int x = 0; x < g_label.w; x++) {
                    int a = g_label.a[(size_t)y * g_label.w + x];
                    if (!a) continue;
                    if (pass == 0) {
                        int r = std::max(1, (int)lroundf(ky));
                        for (int oy = -r; oy <= r; oy++) for (int ox = -r; ox <= r; ox++) {
                            int tx = lx + x + ox, ty = ly + y + oy;
                            if (tx >= 0 && tx < cw && ty >= 0 && ty < panelTop) Blend(px[(size_t)ty * cw + tx], 0x000000, (int)(a * labelA * 0.8f));
                        }
                    } else {
                        int tx = lx + x, ty = ly + y;
                        if (tx >= 0 && tx < cw && ty >= 0 && ty < panelTop) Blend(px[(size_t)ty * cw + tx], 0xFFFFFF, (int)(a * labelA));
                    }
                }
        any = true;
    }
    y0 = std::min(lastY0, ny0); y1 = std::max(lastY1, ny1);
    if (fresh) { y0 = 0; y1 = ch; }
    lastY0 = ny0; lastY1 = ny1;
    if (lastY1 <= lastY0) lastY0 = lastY1 = 0;
    return y1 > y0;
}

} // namespace swgbm
