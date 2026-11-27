// Smooth text (like Expanding Fronts' "Alternative Text Rendering").
//
// The engine draws all interface text with GDI (TextOutA, DrawTextA,
// DrawTextExA) into its 8-bit surfaces. GDI renders 8-bit text without
// anti-aliasing, so letters are hard-edged. Here those calls are replaced: the
// same text, font, position, alignment and clipping are rendered anti-aliased
// (4x supersampled with GDI's own character positions, see below) into a small
// 32-bit coverage mask, and each covered pixel is written into the
// game's surface as the palette colour nearest to the blend of the text colour
// and the pixel underneath. The result is part of the game's own frame, so it is
// covered, scrolled and erased exactly like the original text - nothing is
// tracked afterwards.
#include "textlayer.h"
#include "ddraw_impl.h"
#include "hook.h"
#include "zoom.h"
#include "floattext.h"
#include <vector>
#include <algorithm>
#include <climits>
#include <math.h>

namespace swgbm {
namespace {

bool g_enabled = false;   // hooks installed (smooth text, or zoom: see floattext.h)
bool g_smooth = false;    // [Enhancements] HDText: anti-aliased text
decltype(&TextOutA) pTextOutA = TextOutA;
decltype(&DrawTextA) pDrawTextA = DrawTextA;
decltype(&DrawTextExA) pDrawTextExA = DrawTextExA;

// ---------------------------------------------------------------- palette matching
struct Nearest {
    uint32_t key[256] = {};           // palette the table was built for
    std::vector<uint8_t> map;         // RGB 6:6:6 -> index + 1, lazily filled
    PALETTEENTRY pal[256];
    void Use(const PALETTEENTRY* p) {
        bool same = !map.empty();
        for (int i = 0; i < 256 && same; i++) same = key[i] == *(const uint32_t*)&p[i];
        if (same) return;
        for (int i = 0; i < 256; i++) { key[i] = *(const uint32_t*)&p[i]; pal[i] = p[i]; }
        map.assign(64 * 64 * 64, 0);
    }
    uint8_t Find(int r, int g, int b) {
        size_t k = ((size_t)(r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2);
        if (map[k]) return map[k] - 1;
        // entries 0 and 255 are excluded: the game reserves them (black/white overrides)
        int best = 1, bd = INT_MAX;
        for (int i = 1; i < 255; i++) {
            int dr = r - pal[i].peRed, dg = g - pal[i].peGreen, db = b - pal[i].peBlue;
            int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
            if (d < bd) { bd = d; best = i; }
        }
        map[k] = (uint8_t)(best + 1);
        return (uint8_t)best;
    }
};
Nearest g_near;

const PALETTEENTRY* PaletteOf(DDSurface* s) {
    DDPalette* p = s->pal_ ? s->pal_ : State().primaryPal;
    return p ? p->entries_ : nullptr;
}

// ---------------------------------------------------------------- rendering
struct Mask {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    uint32_t* bits = nullptr;
    int w = 0, h = 0;
    bool Ensure(int nw, int nh) {
        if (bits && nw <= w && nh <= h) return true;
        if (bmp) { DeleteObject(bmp); bmp = nullptr; }
        if (!dc) dc = CreateCompatibleDC(nullptr);
        w = std::max(nw, w); h = std::max(nh, h);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (!bmp) { bits = nullptr; return false; }
        SelectObject(dc, bmp);
        return true;
    }
};
Mask g_mask;

// ---------------------------------------------------------------- 4x supersampling
// GDI anti-aliases small text only horizontally (ClearType) or, for most of
// the game's sizes, not at all (grayscale follows the fonts' "gasp" tables).
// So each line is also drawn at 4x size into a scratch bitmap with every
// character at exactly 4x its normal position (the layout stays GDI's own),
// and the 4x4 blocks are averaged into the mask: smooth in both directions.
// Lines reach us through ExtTextOutW/A, which TextOut and DrawText call
// internally; hooks on them act only while we draw into our own mask.
const int kSS = 4;
typedef BOOL(WINAPI* ExtTextOutWFn)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
typedef BOOL(WINAPI* ExtTextOutAFn)(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
ExtTextOutWFn pExtTextOutW = nullptr;
ExtTextOutAFn pExtTextOutA = nullptr;
bool g_ssActive = false, g_ssBusy = false, g_ssUsed = false;
Mask g_ss;   // kSS x the mask box

HFONT BigFont(HDC maskDc) {
    static std::vector<std::pair<LOGFONTW, HFONT>> cache;
    LOGFONTW lf = {};
    GetObjectW(GetCurrentObject(maskDc, OBJ_FONT), sizeof(lf), &lf);
    lf.lfHeight *= kSS;
    lf.lfWidth *= kSS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    for (auto& e : cache) if (!memcmp(&e.first, &lf, sizeof(lf))) return e.second;
    HFONT f = CreateFontIndirectW(&lf);
    if (cache.size() >= 32) { DeleteObject(cache.front().second); cache.erase(cache.begin()); }
    cache.push_back({lf, f});
    return f;
}

// One line at kSS x, with the advances GDI uses at 1x.
void DrawLineSS(HDC maskDc, int x, int y, UINT opts, const RECT* rc, LPCWSTR s, UINT n, const INT* dx) {
    if (!n || !s) return;
    std::vector<INT> adv(n * ((opts & ETO_PDY) ? 2 : 1));
    if (dx) {
        for (size_t i = 0; i < adv.size(); i++) adv[i] = dx[i] * kSS;
    } else {
        std::vector<INT> part(n);
        SIZE sz;
        BOOL ok = (opts & ETO_GLYPH_INDEX) ? GetTextExtentExPointI(maskDc, (LPWORD)s, (int)n, INT_MAX, nullptr, part.data(), &sz)
                                           : GetTextExtentExPointW(maskDc, s, (int)n, INT_MAX, nullptr, part.data(), &sz);
        if (!ok) return;
        for (UINT i = 0; i < n; i++) adv[i] = (part[i] - (i ? part[i - 1] : 0)) * kSS;
        opts &= ~ETO_PDY;
    }
    HGDIOBJ old = SelectObject(g_ss.dc, BigFont(maskDc));
    SetTextAlign(g_ss.dc, GetTextAlign(maskDc) & ~TA_UPDATECP);
    RECT r4, *pr = nullptr;
    if (rc && (opts & ETO_CLIPPED)) { r4 = {rc->left * kSS, rc->top * kSS, rc->right * kSS, rc->bottom * kSS}; pr = &r4; }
    pExtTextOutW(g_ss.dc, x * kSS, y * kSS, opts & (ETO_CLIPPED | ETO_GLYPH_INDEX | ETO_PDY | ETO_RTLREADING), pr, s, n, adv.data());
    SelectObject(g_ss.dc, old);
    g_ssUsed = true;
}

BOOL WINAPI H_ExtTextOutW(HDC dc, int x, int y, UINT opts, const RECT* rc, LPCWSTR s, UINT n, const INT* dx) {
    if (!g_ssActive || g_ssBusy || dc != g_mask.dc) return pExtTextOutW(dc, x, y, opts, rc, s, n, dx);
    g_ssBusy = true;
    DrawLineSS(dc, x, y, opts, rc, s, n, dx);
    g_ssBusy = false;
    return TRUE;
}

BOOL WINAPI H_ExtTextOutA(HDC dc, int x, int y, UINT opts, const RECT* rc, LPCSTR s, UINT n, const INT* dx) {
    if (!g_ssActive || g_ssBusy || dc != g_mask.dc || (opts & ETO_GLYPH_INDEX)) return pExtTextOutA(dc, x, y, opts, rc, s, n, dx);
    std::vector<wchar_t> w(n + 1);
    int wn = MultiByteToWideChar(CP_ACP, 0, s, (int)n, w.data(), (int)n + 1);
    if (wn != (int)n) return pExtTextOutA(dc, x, y, opts, rc, s, n, dx);   // multi-byte text: plain path
    g_ssBusy = true;
    DrawLineSS(dc, x, y, opts, rc, w.data(), (UINT)wn, dx);
    g_ssBusy = false;
    return TRUE;
}

void InstallSupersampling() {
    static const uint8_t hot[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};   // mov edi,edi; push ebp; mov ebp,esp
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    auto w = g ? (uintptr_t)GetProcAddress(g, "ExtTextOutW") : 0;
    auto a = g ? (uintptr_t)GetProcAddress(g, "ExtTextOutA") : 0;
    if (w) pExtTextOutW = (ExtTextOutWFn)HookFunction(w, hot, sizeof(hot), (void*)H_ExtTextOutW);
    if (a && pExtTextOutW) pExtTextOutA = (ExtTextOutAFn)HookFunction(a, hot, sizeof(hot), (void*)H_ExtTextOutA);
    if (!pExtTextOutA) pExtTextOutA = (ExtTextOutAFn)a;
    LOG("smooth text: 4x supersampling %s", pExtTextOutW ? "on" : "unavailable (ClearType only)");
}

// coverage^0.8: thin anti-aliased strokes keep the weight of the original
// solid 1-pixel text
struct Boost { uint8_t t[256]; Boost() { for (int i = 0; i < 256; i++) t[i] = (uint8_t)(255.0 * pow(i / 255.0, 0.8) + 0.5); } } g_boost;
const uint8_t* kBoost = g_boost.t;

// mask = max(mask, average of each kSS x kSS block of the 4x bitmap)
void ResolveSS(int bw, int bh) {
    const int W4 = g_ss.w;
    for (int y = 0; y < bh; y++) {
        uint32_t* m = g_mask.bits + (size_t)y * g_mask.w;
        for (int x = 0; x < bw; x++) {
            int sum = 0;
            for (int j = 0; j < kSS; j++) {
                const uint32_t* r = g_ss.bits + (size_t)(y * kSS + j) * W4 + x * kSS;
                for (int i = 0; i < kSS; i++) sum += (r[i] >> 8) & 0xFF;   // green = grayscale coverage
            }
            int v = kBoost[sum / (kSS * kSS)];
            if (!v) continue;
            uint32_t old = m[x];
            int oc = ((int)(old & 0xFF) + (int)((old >> 8) & 0xFF) + (int)((old >> 16) & 0xFF)) / 3;
            if (v > oc) m[x] = (uint32_t)(v | v << 8 | v << 16);
        }
    }
}

// Without smooth text (zoom only): the game's own look, never anti-aliased
// (DEFAULT_QUALITY would follow the system's font smoothing).
HFONT PlainFont(HDC dc) {
    static std::vector<std::pair<LOGFONTA, HFONT>> cache;
    LOGFONTA lf = {};
    GetObjectA(GetCurrentObject(dc, OBJ_FONT), sizeof(lf), &lf);
    lf.lfQuality = NONANTIALIASED_QUALITY;
    for (auto& e : cache) if (!memcmp(&e.first, &lf, sizeof(lf))) return e.second;
    HFONT f = CreateFontIndirectA(&lf);
    if (cache.size() >= 32) { DeleteObject(cache.front().second); cache.erase(cache.begin()); }
    cache.push_back({lf, f});
    return f;
}

// Anti-aliased copies of the game's fonts, created once per LOGFONT.
HFONT SmoothFont(HDC dc) {
    static std::vector<std::pair<LOGFONTA, HFONT>> cache;
    LOGFONTA lf = {};
    GetObjectA(GetCurrentObject(dc, OBJ_FONT), sizeof(lf), &lf);
    // ClearType, not ANTIALIASED_QUALITY: grayscale anti-aliasing follows each
    // font's "gasp" table, which turns it off for Arial at 9-16 px - most of
    // the game's text. GDI's ClearType keeps the same advance widths (the
    // game's layout is unchanged); its per-channel coverage is averaged.
    lf.lfQuality = CLEARTYPE_QUALITY;
    for (auto& e : cache) if (!memcmp(&e.first, &lf, sizeof(lf))) return e.second;
    HFONT f = CreateFontIndirectA(&lf);
    if (cache.size() >= 32) { DeleteObject(cache.front().second); cache.erase(cache.begin()); }
    cache.push_back({lf, f});
    return f;
}

// Draws `draw(maskDC, dx, dy)` anti-aliased into the mask (surface box
// bx0..bx1 x by0..by1) and blends it into the surface with the DC's text
// colour, respecting the DC's clip region.
template <class Draw>
void Render(HDC dc, DDSurface* s, int bx0, int by0, int bx1, int by1, Draw draw) {
    bx0 = std::max(0, bx0); by0 = std::max(0, by0);
    bx1 = std::min<int>(s->w_, bx1); by1 = std::min<int>(s->h_, by1);
    if (bx1 <= bx0 || by1 <= by0) return;
    RECT clip;
    int ct = GetClipBox(dc, &clip);
    if (ct == NULLREGION) return;
    if (ct != ERROR) {
        bx0 = std::max<int>(bx0, clip.left); by0 = std::max<int>(by0, clip.top);
        bx1 = std::min<int>(bx1, clip.right); by1 = std::min<int>(by1, clip.bottom);
        if (bx1 <= bx0 || by1 <= by0) return;
    }
    HRGN rgn = nullptr;
    if (ct == COMPLEXREGION) { rgn = CreateRectRgn(0, 0, 0, 0); if (GetClipRgn(dc, rgn) != 1) { DeleteObject(rgn); rgn = nullptr; } }
    int bw = bx1 - bx0, bh = by1 - by0;
    if (!g_mask.Ensure(bw, bh)) { if (rgn) DeleteObject(rgn); return; }
    for (int y = 0; y < bh; y++) memset(g_mask.bits + (size_t)y * g_mask.w, 0, (size_t)bw * 4);
    HGDIOBJ oldFont = SelectObject(g_mask.dc, g_smooth ? SmoothFont(dc) : PlainFont(dc));
    SetBkMode(g_mask.dc, TRANSPARENT);
    SetTextColor(g_mask.dc, RGB(255, 255, 255));
    SetTextAlign(g_mask.dc, GetTextAlign(dc));
    SetTextCharacterExtra(g_mask.dc, GetTextCharacterExtra(dc));
    const bool ss = g_smooth && pExtTextOutW && g_ss.Ensure(bw * kSS, bh * kSS);
    if (ss) {
        for (int y = 0; y < bh * kSS; y++) memset(g_ss.bits + (size_t)y * g_ss.w, 0, (size_t)bw * kSS * 4);
        SetBkMode(g_ss.dc, TRANSPARENT);
        SetTextColor(g_ss.dc, RGB(255, 255, 255));
        g_ssActive = true; g_ssUsed = false;
    }
    draw(g_mask.dc, -bx0, -by0);
    g_ssActive = false;
    GdiFlush();
    if (ss && g_ssUsed) ResolveSS(bw, bh);
    SelectObject(g_mask.dc, oldFont);

    const PALETTEENTRY* pal = PaletteOf(s);
    if (!pal) { if (rgn) DeleteObject(rgn); return; }
    g_near.Use(pal);
    COLORREF tc = GetTextColor(dc);
    int tr, tg, tb;
    if ((tc >> 24) == 1) { const PALETTEENTRY& e = pal[tc & 0xFF]; tr = e.peRed; tg = e.peGreen; tb = e.peBlue; }
    else { tr = GetRValue(tc); tg = GetGValue(tc); tb = GetBValue(tc); }
    // zoom: record what is drawn (coverage, pixels written, pixels underneath)
    static std::vector<uint8_t> rc, rw, rb;
    const bool rec = Zoom::On();
    if (rec) { rc.assign((size_t)bw * bh, 0); rw.assign((size_t)bw * bh, 0); rb.assign((size_t)bw * bh, 0); }
    for (int y = 0; y < bh; y++) {
        const uint32_t* m = g_mask.bits + (size_t)y * g_mask.w;
        uint8_t* row = s->bits_ + (size_t)(by0 + y) * s->pitch_ + bx0;
        for (int x = 0; x < bw; x++) {
            uint32_t v = m[x];
            int cov = ((int)(v & 0xFF) + (int)((v >> 8) & 0xFF) + (int)((v >> 16) & 0xFF) + 1) / 3;
            if (!cov) continue;
            if (rgn && !PtInRegion(rgn, bx0 + x, by0 + y)) continue;
            if (rec) { size_t i = (size_t)y * bw + x; rc[i] = (uint8_t)cov; rb[i] = row[x]; }
            if (cov >= 250) { row[x] = g_near.Find(tr, tg, tb); continue; }
            const PALETTEENTRY& b = pal[row[x]];
            int r = (tr * cov + b.peRed * (255 - cov)) / 255, g = (tg * cov + b.peGreen * (255 - cov)) / 255,
                bl = (tb * cov + b.peBlue * (255 - cov)) / 255;
            row[x] = g_near.Find(r, g, bl);
        }
        if (rec) for (int x = 0; x < bw; x++) rw[(size_t)y * bw + x] = row[x];
    }
    if (rec) FloatText::Record(s, bx0, by0, bw, bh, rc.data(), rw.data(), rb.data(), (uint32_t)(tr << 16 | tg << 8 | tb));
    if (rgn) DeleteObject(rgn);
}

// SetBkMode(OPAQUE): GDI would paint the background box first.
void FillOpaque(HDC dc, const RECT& r) {
    if (GetBkMode(dc) != OPAQUE) return;
    HBRUSH b = CreateSolidBrush(GetBkColor(dc));
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// Which surfaces we draw text into ourselves: all with smooth text; with zoom
// alone only the zoomed screen buffer (for the zoom overlay).
DDSurface* Target(HDC dc) {
    DDSurface* s = g_enabled ? SurfaceFromDC(dc) : nullptr;
    if (s && !g_smooth && !(Zoom::On() && !s->IsPrimary() && s->w_ == Zoom::InternalW() && s->h_ == Zoom::InternalH())) s = nullptr;
    return s;
}

BOOL WINAPI H_TextOutA(HDC dc, int x, int y, LPCSTR str, int len) {
    DDSurface* s = (len > 0 && str) ? Target(dc) : nullptr;
    if (!s) return pTextOutA(dc, x, y, str, len);
    UINT align = GetTextAlign(dc);
    if (align & TA_UPDATECP) return pTextOutA(dc, x, y, str, len);
    SIZE sz = {};
    GetTextExtentPoint32A(dc, str, len, &sz);
    TEXTMETRICA tm = {};
    GetTextMetricsA(dc, &tm);
    int left = x, top = y;
    if ((align & TA_CENTER) == TA_CENTER) left -= sz.cx / 2;
    else if (align & TA_RIGHT) left -= sz.cx;
    if (align & TA_BASELINE) top -= tm.tmAscent;
    else if (align & TA_BOTTOM) top -= sz.cy;
    RECT box = {left, top, left + sz.cx, top + sz.cy};
    FillOpaque(dc, box);
    GdiFlush();   // the blend reads the surface memory directly
    Render(dc, s, left - tm.tmOverhang - 2, top - 1, left + sz.cx + tm.tmOverhang + 3, top + std::max<int>(sz.cy, tm.tmHeight) + 2,
           [&](HDC m, int dx, int dy) { TextOutA(m, x + dx, y + dy, str, len); });
    return TRUE;
}

int DrawTextCommon(HDC dc, LPCSTR str, int len, LPRECT rc, UINT fmt, LPDRAWTEXTPARAMS dtp, bool ex) {
    auto call = [&](HDC d, LPRECT r, UINT f) { return ex ? pDrawTextExA(d, (LPSTR)str, len, r, f, dtp) : pDrawTextA(d, str, len, r, f); };
    DDSurface* s = (str && rc && !(fmt & DT_CALCRECT)) ? Target(dc) : nullptr;
    if (!s) return call(dc, rc, fmt);
    // extent and return value from GDI's own layout
    RECT calc = *rc;
    int height = call(dc, &calc, fmt | DT_CALCRECT);
    RECT box = *rc;
    if (fmt & DT_NOCLIP) {
        box.left = std::min(box.left, calc.left); box.top = std::min(box.top, calc.top);
        box.right = std::max(box.right, calc.right); box.bottom = std::max(box.bottom, calc.bottom);
    }
    FillOpaque(dc, (fmt & DT_NOCLIP) ? calc : *rc);
    GdiFlush();
    RECT r0 = *rc;
    Render(dc, s, box.left - 2, box.top - 1, box.right + 3, box.bottom + 2, [&](HDC m, int dx, int dy) {
        RECT r = {r0.left + dx, r0.top + dy, r0.right + dx, r0.bottom + dy};
        if (ex) pDrawTextExA(m, (LPSTR)str, len, &r, fmt, dtp);
        else pDrawTextA(m, str, len, &r, fmt);
    });
    return height;
}
int WINAPI H_DrawTextA(HDC dc, LPCSTR str, int len, LPRECT rc, UINT fmt) { return DrawTextCommon(dc, str, len, rc, fmt, nullptr, false); }
int WINAPI H_DrawTextExA(HDC dc, LPSTR str, int len, LPRECT rc, UINT fmt, LPDRAWTEXTPARAMS p) { return DrawTextCommon(dc, str, len, rc, fmt, p, true); }

} // namespace

void TextLayer::Install() {
    g_smooth = CfgBool("Enhancements", "HDText", false);
    g_enabled = g_smooth || CfgBool("Zoom", "Enabled", false);   // zoom needs the text draws too
    if (!g_enabled) return;
    void* o = HookIAT(nullptr, "GDI32.dll", "TextOutA", (void*)H_TextOutA);
    if (o) pTextOutA = (decltype(&TextOutA))o;
    if (void* d = HookIAT(nullptr, "USER32.dll", "DrawTextA", (void*)H_DrawTextA)) pDrawTextA = (decltype(&DrawTextA))d;
    if (void* d = HookIAT(nullptr, "USER32.dll", "DrawTextExA", (void*)H_DrawTextExA)) pDrawTextExA = (decltype(&DrawTextExA))d;
    LOG("smooth text: %s", o ? "TextOutA/DrawText hooked" : "TextOutA not imported - disabled");
    if (o && g_smooth) InstallSupersampling();
    if (!o) g_enabled = false;
}

bool TextLayer::Enabled() { return g_smooth; }

} // namespace swgbm
