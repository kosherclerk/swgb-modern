// Text capture for the text layer (HD text, zoom).
//
// The engine draws all interface text with GDI (TextOutA, DrawTextA,
// DrawTextExA) into its 8-bit back buffer. These calls are hooked: the game's
// own call runs unchanged (the buffer gets exactly the original pixels), and
// around it we note which pixels it wrote and what was under them, and - via
// hooks on gdi32's ExtTextOutW/A, which TextOut and DrawText use for every
// line - the lines GDI drew: string, start, character advances, clipping, plus
// the DC's font, alignment and colour. FloatText then shows the text in its
// own layer, redrawn at the output resolution (see floattext.h).
#include "textlayer.h"
#include "ddraw_impl.h"
#include "hook.h"
#include "zoom.h"
#include "floattext.h"
#include <vector>
#include <algorithm>
#include <climits>

namespace swgbm {
namespace {

bool g_enabled = false;   // hooks installed
bool g_hd = false;        // [Enhancements] HDText
decltype(&TextOutA) pTextOutA = TextOutA;
decltype(&DrawTextA) pDrawTextA = DrawTextA;
decltype(&DrawTextExA) pDrawTextExA = DrawTextExA;

typedef BOOL(WINAPI* ExtTextOutWFn)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
typedef BOOL(WINAPI* ExtTextOutAFn)(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
ExtTextOutWFn pExtTextOutW = nullptr;
ExtTextOutAFn pExtTextOutA = nullptr;

// lines drawn while a hooked call runs on this DC (game thread only)
struct CaptureState {
    HDC dc = nullptr;
    bool on = false, busy = false, bad = false;
    std::vector<FloatText::Line> lines;
} g_cap;

void AddLine(HDC dc, int x, int y, UINT opts, const RECT* rc, const wchar_t* s, UINT n, const INT* dx) {
    if (!n || !s) return;
    if (opts & ETO_OPAQUE) { g_cap.bad = true; return; }   // background box: left to the game's picture
    FloatText::Line l;
    l.x = x; l.y = y; l.opts = opts & (ETO_CLIPPED | ETO_GLYPH_INDEX | ETO_RTLREADING);
    if (rc && (opts & ETO_CLIPPED)) l.clip = *rc; else l.opts &= ~ETO_CLIPPED;
    l.text.assign(s, s + n);
    l.adv.resize(n);
    if (dx) {
        const int step = (opts & ETO_PDY) ? 2 : 1;
        for (UINT i = 0; i < n; i++) l.adv[i] = dx[i * step];
    } else {
        std::vector<INT> part(n);
        SIZE sz;
        BOOL ok = (opts & ETO_GLYPH_INDEX) ? GetTextExtentExPointI(dc, (LPWORD)s, (int)n, INT_MAX, nullptr, part.data(), &sz)
                                           : GetTextExtentExPointW(dc, s, (int)n, INT_MAX, nullptr, part.data(), &sz);
        if (!ok) { g_cap.bad = true; return; }
        for (UINT i = 0; i < n; i++) l.adv[i] = part[i] - (i ? part[i - 1] : 0);
    }
    g_cap.lines.push_back(std::move(l));
}

BOOL WINAPI H_ExtTextOutW(HDC dc, int x, int y, UINT opts, const RECT* rc, LPCWSTR s, UINT n, const INT* dx) {
    if (g_cap.on && !g_cap.busy && dc == g_cap.dc) {
        g_cap.busy = true;
        AddLine(dc, x, y, opts, rc, s, n, dx);
        BOOL r = pExtTextOutW(dc, x, y, opts, rc, s, n, dx);
        g_cap.busy = false;
        return r;
    }
    return pExtTextOutW(dc, x, y, opts, rc, s, n, dx);
}

BOOL WINAPI H_ExtTextOutA(HDC dc, int x, int y, UINT opts, const RECT* rc, LPCSTR s, UINT n, const INT* dx) {
    if (g_cap.on && !g_cap.busy && dc == g_cap.dc) {
        g_cap.busy = true;
        if (opts & ETO_GLYPH_INDEX) g_cap.bad = true;
        else if (n) {
            std::vector<wchar_t> w(n + 1);
            int wn = MultiByteToWideChar(CP_ACP, 0, s, (int)n, w.data(), (int)n + 1);
            if (wn != (int)n) g_cap.bad = true;   // multi-byte text: advances would not line up
            else AddLine(dc, x, y, opts, rc, w.data(), (UINT)wn, dx);
        }
        BOOL r = pExtTextOutA(dc, x, y, opts, rc, s, n, dx);
        g_cap.busy = false;
        return r;
    }
    return pExtTextOutA(dc, x, y, opts, rc, s, n, dx);
}

void InstallLineHooks() {
    static const uint8_t hot[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};   // mov edi,edi; push ebp; mov ebp,esp
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    auto w = g ? (uintptr_t)GetProcAddress(g, "ExtTextOutW") : 0;
    auto a = g ? (uintptr_t)GetProcAddress(g, "ExtTextOutA") : 0;
    if (w) pExtTextOutW = (ExtTextOutWFn)HookFunction(w, hot, sizeof(hot), (void*)H_ExtTextOutW);
    if (a) pExtTextOutA = (ExtTextOutAFn)HookFunction(a, hot, sizeof(hot), (void*)H_ExtTextOutA);
    if (!pExtTextOutW) pExtTextOutW = (ExtTextOutWFn)w;
    if (!pExtTextOutA) pExtTextOutA = (ExtTextOutAFn)a;
    LOG("text layer: line capture %s", (w && pExtTextOutW != (ExtTextOutWFn)w) ? "on" : "unavailable");
}

const PALETTEENTRY* PaletteOf(DDSurface* s) {
    DDPalette* p = s->pal_ ? s->pal_ : State().primaryPal;
    return p ? p->entries_ : nullptr;
}

// Runs the game's own call (`call`) and records what it drew inside the box
// (surface pixels; clipped to the surface and the DC's clip box).
template <class Call, class Synth>
auto Capture(HDC dc, DDSurface* s, int bx0, int by0, int bx1, int by1, Call call, Synth synth) -> decltype(call()) {
    bx0 = std::max(0, bx0); by0 = std::max(0, by0);
    bx1 = std::min<int>(s->w_, bx1); by1 = std::min<int>(s->h_, by1);
    RECT clip;
    int ct = GetClipBox(dc, &clip);
    if (ct != ERROR && ct != NULLREGION) {
        bx0 = std::max<int>(bx0, clip.left); by0 = std::max<int>(by0, clip.top);
        bx1 = std::min<int>(bx1, clip.right); by1 = std::min<int>(by1, clip.bottom);
    }
    if (ct == NULLREGION || bx1 <= bx0 || by1 <= by0) return call();
    const int bw = bx1 - bx0, bh = by1 - by0;
    static std::vector<uint8_t> before, after, cov;
    before.resize((size_t)bw * bh);
    GdiFlush();
    for (int y = 0; y < bh; y++) memcpy(&before[(size_t)y * bw], s->bits_ + (size_t)(by0 + y) * s->pitch_ + bx0, bw);
    g_cap.dc = dc; g_cap.on = true; g_cap.busy = false; g_cap.bad = false; g_cap.lines.clear();
    auto result = call();
    g_cap.on = false;
    GdiFlush();
    if (!g_cap.bad && g_cap.lines.empty()) synth();   // the line from the call itself
    if (g_cap.bad || g_cap.lines.empty()) return result;   // left as the game drew it
    after.resize(before.size());
    cov.assign(before.size(), 0);
    // GDI writes 8-bit text in one colour: the most common changed value. The
    // text's pixels are all pixels of that colour that it wrote or that already
    // had it (the game draws texts over earlier ones without clearing; the
    // background of those comes from the earlier text's record).
    int hist[256] = {};
    for (int y = 0; y < bh; y++) {
        const uint8_t* r = s->bits_ + (size_t)(by0 + y) * s->pitch_ + bx0;
        memcpy(&after[(size_t)y * bw], r, bw);
        for (int x = 0; x < bw; x++) if (r[x] != before[(size_t)y * bw + x]) hist[r[x]]++;
    }
    int ink = 0;
    for (int v = 1; v < 256; v++) if (hist[v] > hist[ink]) ink = v;
    if (!hist[ink]) return result;   // nothing changed: the same text again, already recorded
    for (size_t i = 0; i < after.size(); i++) if (after[i] == ink) cov[i] = 255;
    FloatText::Draw d;
    d.ink = ink;
    GetObjectW(GetCurrentObject(dc, OBJ_FONT), sizeof(d.font), &d.font);
    d.align = GetTextAlign(dc);
    d.charExtra = GetTextCharacterExtra(dc);
    COLORREF tc = GetTextColor(dc);
    if ((tc >> 24) == 1) {
        const PALETTEENTRY* pal = PaletteOf(s);
        if (!pal) return result;
        const PALETTEENTRY& e = pal[tc & 0xFF];
        d.rgb = (uint32_t)e.peRed << 16 | e.peGreen << 8 | e.peBlue;
    } else {
        d.rgb = (uint32_t)GetRValue(tc) << 16 | GetGValue(tc) << 8 | GetBValue(tc);
    }
    d.lines = std::move(g_cap.lines);
    g_cap.lines = {};
    FloatText::Record(s, bx0, by0, bw, bh, cov.data(), after.data(), before.data(), std::move(d));
    return result;
}

DDSurface* Target(HDC dc) {
    if (!g_enabled) return nullptr;
    DDSurface* s = SurfaceFromDC(dc);
    return FloatText::Wanted(s) ? s : nullptr;
}

BOOL WINAPI H_TextOutA(HDC dc, int x, int y, LPCSTR str, int len) {
    DDSurface* s = (len > 0 && str) ? Target(dc) : nullptr;
    if (!s || GetBkMode(dc) == OPAQUE) return pTextOutA(dc, x, y, str, len);
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
    return Capture(dc, s, left - tm.tmOverhang - 4, top - 2, left + sz.cx + tm.tmOverhang + 5, top + std::max<int>(sz.cy, tm.tmHeight) + 3,
                   [&] { return pTextOutA(dc, x, y, str, len); },
                   [&] {
                       std::vector<wchar_t> w(len + 1);
                       int wn = MultiByteToWideChar(CP_ACP, 0, str, len, w.data(), len + 1);
                       if (wn == len) AddLine(dc, x, y, 0, nullptr, w.data(), (UINT)wn, nullptr);
                       else g_cap.bad = true;
                   });
}

int DrawTextCommon(HDC dc, LPCSTR str, int len, LPRECT rc, UINT fmt, LPDRAWTEXTPARAMS dtp, bool ex) {
    auto call = [&](LPRECT r, UINT f) { return ex ? pDrawTextExA(dc, (LPSTR)str, len, r, f, dtp) : pDrawTextA(dc, str, len, r, f); };
    DDSurface* s = (str && rc && !(fmt & DT_CALCRECT) && GetBkMode(dc) != OPAQUE) ? Target(dc) : nullptr;
    if (!s) return call(rc, fmt);
    RECT calc = *rc;
    call(&calc, fmt | DT_CALCRECT);
    RECT box = *rc;
    if (fmt & DT_NOCLIP) {
        box.left = std::min(box.left, calc.left); box.top = std::min(box.top, calc.top);
        box.right = std::max(box.right, calc.right); box.bottom = std::max(box.bottom, calc.bottom);
    }
    return Capture(dc, s, box.left - 4, box.top - 2, box.right + 5, box.bottom + 3, [&] { return call(rc, fmt); }, [] {});
}
int WINAPI H_DrawTextA(HDC dc, LPCSTR str, int len, LPRECT rc, UINT fmt) { return DrawTextCommon(dc, str, len, rc, fmt, nullptr, false); }
int WINAPI H_DrawTextExA(HDC dc, LPSTR str, int len, LPRECT rc, UINT fmt, LPDRAWTEXTPARAMS p) { return DrawTextCommon(dc, str, len, rc, fmt, p, true); }

} // namespace

void TextLayer::Install() {
    g_hd = CfgBool("Enhancements", "HDText", false);
    FloatText::Configure(g_hd);
    g_enabled = g_hd || CfgBool("Zoom", "Enabled", false);   // zoom needs the text draws too
    if (!g_enabled) return;
    void* o = HookIAT(nullptr, "GDI32.dll", "TextOutA", (void*)H_TextOutA);
    if (o) pTextOutA = (decltype(&TextOutA))o;
    if (void* d = HookIAT(nullptr, "USER32.dll", "DrawTextA", (void*)H_DrawTextA)) pDrawTextA = (decltype(&DrawTextA))d;
    if (void* d = HookIAT(nullptr, "USER32.dll", "DrawTextExA", (void*)H_DrawTextExA)) pDrawTextExA = (decltype(&DrawTextExA))d;
    LOG("text layer (%s): %s", g_hd ? "HD text" : "zoom", o ? "TextOutA/DrawText hooked" : "TextOutA not imported - disabled");
    if (o) InstallLineHooks();
    if (!o) g_enabled = false;
}

bool TextLayer::Enabled() { return g_hd; }

} // namespace swgbm
