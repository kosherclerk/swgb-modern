// HD text.
//
// The engine draws all interface text with GDI TextOutA into its 8-bit
// surfaces at game resolution, so after upscaling it is blocky. (Expanding
// Fronts' "Alternative Text Rendering" swaps the rasterizer for DirectWrite
// but still renders at game resolution.) Here text is redrawn at the output
// resolution:
//   1. TextOutA is hooked. The target surface is diffed before/after the call
//      to find exactly which pixels the text produced (glyphs, and the
//      background box in OPAQUE mode). The string, font, alignment and colour
//      are recorded as an item, and each produced pixel is tagged in the
//      surface's text plane with (item id, position inside the item).
//   2. Blits/flips carry the tags along with the pixels (ddraw_impl.cpp), so
//      wherever the text ends up on screen we know what it is and where its
//      origin is. A tag counts only while its pixel still has the colour the
//      text gave it, so overdrawn text disappears correctly.
//   3. At present time the visible tagged pixels are removed from the image
//      shown (filled from neighbouring non-text pixels) and the strings are
//      drawn with anti-aliased fonts at the canvas resolution into the
//      overlay layer.
#include "textlayer.h"
#include "ddraw_impl.h"
#include "hook.h"
#include <map>
#include <memory>
#include <algorithm>
#include <tuple>
#include <math.h>
#include <climits>
#include <iterator>

namespace swgbm {
namespace {

const uint32_t kMaxItems = 4096;
struct Item {
    bool used = false;
    std::string text;
    LOGFONTA lf = {};
    UINT align = 0;
    int ox = 0, oy = 0;          // text origin relative to the item box
    int w = 0, h = 0;            // item box (game pixels)
    int colorIdx = 0;            // palette index the glyphs were drawn with
    bool opaque = false;
    int bkIdx = 0;
    std::vector<int16_t> glyph;  // index each tagged pixel had (-1 = not text)
    uint32_t serial = 0;         // changes when the slot is reused
    std::string key;             // dedup key (see H_TextOutA)
};
Item g_items[kMaxItems];
std::map<std::string, uint32_t> g_byKey;  // identical draws share one item
uint32_t g_next = 1, g_serial = 1;
CRITICAL_SECTION g_lock;
bool g_enabled = false;
decltype(&TextOutA) pTextOutA = TextOutA;

inline uint32_t Pack(uint32_t id, int lx, int ly) { return (id << 20) | ((uint32_t)lx << 8) | (uint32_t)ly; }
inline uint32_t TagId(uint32_t t) { return t >> 20; }
inline int TagX(uint32_t t) { return (t >> 8) & 0xFFF; }
inline int TagY(uint32_t t) { return t & 0xFF; }

BOOL WINAPI H_TextOutA(HDC dc, int x, int y, LPCSTR str, int len) {
    DDSurface* s = (g_enabled && len > 0) ? SurfaceFromDC(dc) : nullptr;
    if (!s) return pTextOutA(dc, x, y, str, len);
    SIZE sz = {};
    GetTextExtentPoint32A(dc, str, len, &sz);
    TEXTMETRICA tm = {};
    GetTextMetricsA(dc, &tm);
    UINT align = GetTextAlign(dc);
    int left = x, top = y;
    if ((align & TA_CENTER) == TA_CENTER) left -= sz.cx / 2;
    else if (align & TA_RIGHT) left -= sz.cx;
    if (align & TA_BASELINE) top -= tm.tmAscent;
    else if (align & TA_BOTTOM) top -= sz.cy;
    // box with margin for overhangs (italic, bold)
    int bx0 = std::max(0, left - 4), by0 = std::max(0, top - 2);
    int bx1 = std::min<int>(s->w_, left + sz.cx + 6), by1 = std::min<int>(s->h_, top + std::max<int>(sz.cy, tm.tmHeight) + 2);
    if (bx1 <= bx0 || by1 <= by0 || bx1 - bx0 > 4095 || by1 - by0 > 255) return pTextOutA(dc, x, y, str, len);
    int bw = bx1 - bx0, bh = by1 - by0;
    std::vector<uint8_t> before((size_t)bw * bh);
    for (int r = 0; r < bh; r++) memcpy(&before[(size_t)r * bw], s->bits_ + (size_t)(by0 + r) * s->pitch_ + bx0, bw);
    BOOL res = pTextOutA(dc, x, y, str, len);
    GdiFlush();
    // which pixels did the text change?
    std::vector<int16_t> glyph((size_t)bw * bh, -1);
    int counts[256] = {};
    int changed = 0;
    for (int r = 0; r < bh; r++) {
        const uint8_t* now = s->bits_ + (size_t)(by0 + r) * s->pitch_ + bx0;
        for (int c = 0; c < bw; c++) {
            if (now[c] != before[(size_t)r * bw + c]) { glyph[(size_t)r * bw + c] = now[c]; counts[now[c]]++; changed++; }
        }
    }
    if (!changed) return res;
    bool opaque = GetBkMode(dc) == OPAQUE;
    // glyph colour = most frequent new colour (OPAQUE: the box colour is most
    // frequent, the glyphs second)
    int c1 = 0, c2 = -1;
    for (int i = 1; i < 256; i++) if (counts[i] > counts[c1]) c1 = i;
    for (int i = 0; i < 256; i++) if (i != c1 && counts[i] && (c2 < 0 || counts[i] > counts[c2])) c2 = i;
    HGDIOBJ font = GetCurrentObject(dc, OBJ_FONT);
    LOGFONTA lf = {};
    GetObjectA(font, sizeof(lf), &lf);
    bool isOpaque = opaque && c2 >= 0;
    int colorIdx = isOpaque ? c2 : c1;
    // The engine redraws the same labels every frame; an identical draw
    // (same string, font, colours, placement inside the box and glyph pixels)
    // reuses the existing item so the HD layer sees no change.
    std::string key(str, len);
    int meta[8] = {(int)align, x - bx0, y - by0, bw, bh, colorIdx, c1, isOpaque};
    key.append((const char*)meta, sizeof(meta));
    key.append((const char*)&lf, sizeof(lf));
    EnterCriticalSection(&g_lock);
    uint32_t id = 0;
    auto found = g_byKey.find(key);
    if (found != g_byKey.end() && g_items[found->second].used && g_items[found->second].glyph == glyph) id = found->second;
    if (!id) {
        id = g_next;
        g_next = g_next + 1 >= kMaxItems ? 1 : g_next + 1;
        Item& it = g_items[id];
        if (it.used && !it.key.empty()) { auto o = g_byKey.find(it.key); if (o != g_byKey.end() && o->second == id) g_byKey.erase(o); }
        it.used = true;
        it.text.assign(str, len);
        it.lf = lf;
        it.align = align;
        it.ox = x - bx0; it.oy = y - by0;
        it.w = bw; it.h = bh;
        it.opaque = isOpaque;
        it.colorIdx = colorIdx;
        it.bkIdx = c1;
        it.glyph = std::move(glyph);
        it.serial = g_serial++;
        it.key = key;
        g_byKey[key] = id;
    }
    Item& it = g_items[id];
    LeaveCriticalSection(&g_lock);
    uint32_t* tp = s->TPlane();
    for (int r = 0; r < bh; r++) {
        uint32_t* row = tp + (size_t)(by0 + r) * s->pitch_ + bx0;
        const int16_t* g = &it.glyph[(size_t)r * bw];
        for (int c = 0; c < bw; c++) if (g[c] >= 0) row[c] = Pack(id, c, r);
    }
    return res;
}

// ---------------------------------------------------------------- presenter side
struct Instance {
    uint32_t id, serial;
    int tx, ty;              // item box top-left on screen (game pixels)
    int cx0, cy0, cx1, cy1;  // visible part (game pixels)
    uint32_t color, bk;
    bool operator<(const Instance& o) const {
        return std::tie(id, serial, tx, ty, cx0, cy0, cx1, cy1, color, bk) < std::tie(o.id, o.serial, o.tx, o.ty, o.cx0, o.cy0, o.cx1, o.cy1, o.color, o.bk);
    }
    bool operator==(const Instance& o) const { return !(*this < o) && !(o < *this); }
};

struct Sprite { int w = 0, h = 0; std::vector<uint32_t> px; };     // premultiplied BGRA
std::map<std::tuple<uint32_t, int, uint32_t, uint32_t>, std::shared_ptr<Sprite>> g_sprites;  // (serial, scale*100, color, bk)

std::shared_ptr<Sprite> RenderItem(const Item& it, float scale, uint32_t color, uint32_t bk) {
    auto key = std::make_tuple(it.serial, (int)(scale * 100), color, bk);
    auto f = g_sprites.find(key);
    if (f != g_sprites.end()) return f->second;
    if (g_sprites.size() > 2048) g_sprites.clear();
    auto sp = std::make_shared<Sprite>();
    sp->w = std::max(1, (int)(it.w * scale + 0.5f));
    sp->h = std::max(1, (int)(it.h * scale + 0.5f));
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = sp->w;
    bi.bmiHeader.biHeight = -sp->h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    uint32_t* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return sp; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)sp->w * sp->h * 4);
    LOGFONTA lf = it.lf;
    lf.lfHeight = (LONG)lroundf(lf.lfHeight * scale);
    lf.lfWidth = (LONG)lroundf(lf.lfWidth * scale);
    lf.lfQuality = ANTIALIASED_QUALITY;
    HFONT font = CreateFontIndirectA(&lf);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SetTextAlign(dc, it.align);
    TextOutA(dc, (int)lroundf(it.ox * scale), (int)lroundf(it.oy * scale), it.text.c_str(), (int)it.text.size());
    GdiFlush();
    sp->px.resize((size_t)sp->w * sp->h);
    uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    uint32_t br = (bk >> 16) & 0xFF, bg = (bk >> 8) & 0xFF, bb = bk & 0xFF;
    for (size_t i = 0; i < sp->px.size(); i++) {
        uint32_t cov = std::max({bits[i] & 0xFF, (bits[i] >> 8) & 0xFF, (bits[i] >> 16) & 0xFF});
        if (it.opaque) {
            uint32_t R = (r * cov + br * (255 - cov)) / 255, G = (g * cov + bg * (255 - cov)) / 255, B = (b * cov + bb * (255 - cov)) / 255;
            sp->px[i] = 0xFF000000u | (R << 16) | (G << 8) | B;
        } else {
            sp->px[i] = (cov << 24) | ((r * cov / 255) << 16) | ((g * cov / 255) << 8) | (b * cov / 255);
        }
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    g_sprites[key] = sp;
    return sp;
}

std::vector<Instance> g_prev;
// Incremental scan state: per game row, the text fragments found on it and
// which pixels are text. Only rows the game changed are rescanned.
struct Frag { uint32_t id, serial; int tx, ty, x0, x1; };
std::vector<std::vector<Frag>> g_rows;
std::vector<uint8_t> g_isText;
int g_sw = 0, g_sh = 0;
bool g_haveDisp = false;
int g_canvasW = 0, g_canvasH = 0;
const Overlay* g_lastBase = nullptr;

void ScanRows(const Frame& f, int y0, int y1) {
    const int W = f.width;
    for (int y = y0; y < y1; y++) {
        auto& frags = g_rows[y];
        frags.clear();
        const uint32_t* tr = f.text.data() + (size_t)y * W;
        const uint8_t* pr = f.pixels.data() + (size_t)y * W;
        uint8_t* is = g_isText.data() + (size_t)y * W;
        memset(is, 0, W);
        for (int x = 0; x < W; x++) {
            uint32_t t = tr[x];
            if (!t) continue;
            uint32_t id = TagId(t);
            const Item& it = g_items[id];
            if (!it.used) continue;
            int lx = TagX(t), ly = TagY(t);
            if (lx >= it.w || ly >= it.h) continue;
            int16_t gv = it.glyph[(size_t)ly * it.w + lx];
            if (gv < 0 || pr[x] != (uint8_t)gv) continue; // overdrawn since
            is[x] = 1;
            int tx = x - lx, ty = y - ly;
            if (!frags.empty() && frags.back().id == id && frags.back().tx == tx && frags.back().ty == ty) frags.back().x1 = x + 1;
            else frags.push_back({id, it.serial, tx, ty, x, x + 1});
        }
    }
}

void Inpaint(const Frame& f, Frame& disp, int y0, int y1) {
    const int W = f.width, H = f.height;
    for (int y = y0; y < y1; y++) {
        uint8_t* d = disp.pixels.data() + (size_t)y * W;
        memcpy(d, f.pixels.data() + (size_t)y * W, W);
        if (g_rows[y].empty()) continue;
        const uint8_t* is = g_isText.data() + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            if (!is[x]) continue;
            int best = -1;
            for (int rad = 1; rad <= 3 && best < 0; rad++)
                for (int dy = -rad; dy <= rad && best < 0; dy++)
                    for (int dx = -rad; dx <= rad; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H || g_isText[(size_t)yy * W + xx]) continue;
                        best = f.pixels[(size_t)yy * W + xx];
                        break;
                    }
            if (best >= 0) d[x] = (uint8_t)best;
        }
    }
}
} // namespace

void TextLayer::Install() {
    InitializeCriticalSection(&g_lock);
    g_enabled = CfgBool("Enhancements", "HDText", false);
    if (!g_enabled) return;
    void* o = HookIAT(nullptr, "GDI32.dll", "TextOutA", (void*)H_TextOutA);
    if (o) pTextOutA = (decltype(&TextOutA))o;
    LOG("hd text: %s", o ? "TextOutA hooked" : "TextOutA not imported - disabled");
    if (!o) g_enabled = false;
}

bool TextLayer::Enabled() { return g_enabled; }

bool TextLayer::Compose(const Frame& f, Frame& disp, const Overlay* base, int canvasW, int canvasH, float rx, float ry, float rw, float rh,
                        Overlay& out) {
    if (!g_enabled || f.text.empty()) {
        g_prev.clear();
        g_haveDisp = false;
        return false;
    }
    const int W = f.width, H = f.height;
    bool baseDirty = base && base->dirtyY1 > base->dirtyY0;
    bool full = !g_haveDisp || g_sw != W || g_sh != H || f.paletteChanged || base != g_lastBase;
    bool canvasSame = out.width == canvasW && out.height == canvasH && g_canvasW == canvasW && g_canvasH == canvasH;
    // Nothing changed since the last call: keep the previous result so the
    // presenter can skip the frame.
    if (!full && !f.Dirty() && !baseDirty && canvasSame) {
        disp.dirtyY0 = disp.dirtyY1 = 0;
        disp.paletteChanged = false;
        out.dirtyY0 = out.dirtyY1 = 0;
        return !g_prev.empty() || base;
    }
    EnterCriticalSection(&g_lock);
    if (g_sw != W || g_sh != H) {
        g_rows.assign(H, {});
        g_isText.assign((size_t)W * H, 0);
        g_sw = W; g_sh = H;
        full = true;
    }
    // 1. rescan the changed rows
    int sy0 = full ? 0 : std::max(0, f.dirtyY0), sy1 = full ? H : std::min(H, f.dirtyY1);
    if (sy1 > sy0) ScanRows(f, sy0, sy1);
    // collect instances from all rows; a reused item slot means stale
    // fragments elsewhere, so rescan everything once
    std::map<std::tuple<uint32_t, int, int>, Instance> inst;
    for (int pass = 0; pass < 2; pass++) {
        bool stale = false;
        inst.clear();
        for (int y = 0; y < H && !stale; y++) {
            for (const Frag& fr : g_rows[y]) {
                const Item& it = g_items[fr.id];
                if (!it.used || it.serial != fr.serial) { stale = true; break; }
                auto key = std::make_tuple(fr.id, fr.tx, fr.ty);
                auto ins = inst.find(key);
                if (ins == inst.end()) {
                    Instance n = {fr.id, fr.serial, fr.tx, fr.ty, fr.x0, y, fr.x1, y + 1, f.palette[it.colorIdx], f.palette[it.bkIdx]};
                    inst.emplace(key, n);
                } else {
                    Instance& n = ins->second;
                    n.cx0 = std::min(n.cx0, fr.x0); n.cx1 = std::max(n.cx1, fr.x1);
                    n.cy0 = std::min(n.cy0, y); n.cy1 = std::max(n.cy1, y + 1);
                }
            }
        }
        if (!stale) break;
        full = true; sy0 = 0; sy1 = H;
        ScanRows(f, 0, H);
    }
    std::vector<Instance> now;
    now.reserve(inst.size());
    for (auto& kv : inst) now.push_back(kv.second);
    std::sort(now.begin(), now.end());
    // 2. hide the low-resolution glyphs in the displayed image (persistent
    // copy; only the changed rows +-3 are rebuilt, the inpaint reaches 3 rows)
    if (disp.width != W || disp.height != H || disp.pixels.size() != (size_t)W * H) {
        disp.width = W; disp.height = H;
        disp.pixels.assign((size_t)W * H, 0);
        full = true; sy0 = 0; sy1 = H;
    }
    memcpy(disp.palette, f.palette, sizeof(disp.palette));
    disp.paletteChanged = f.paletteChanged || full;
    int iy0 = full ? 0 : std::max(0, sy0 - 3), iy1 = full ? H : std::min(H, sy1 + 3);
    if (iy1 > iy0) Inpaint(f, disp, iy0, iy1);
    disp.dirtyY0 = iy1 > iy0 ? iy0 : 0;
    disp.dirtyY1 = iy1 > iy0 ? iy1 : 0;
    g_haveDisp = true;
    g_lastBase = base;
    // 3. draw the text sharply into the canvas
    bool changed = full || now != g_prev || baseDirty || !canvasSame;
    if (!changed) { LeaveCriticalSection(&g_lock); out.dirtyY0 = out.dirtyY1 = 0; return !now.empty() || base; }
    float sx = canvasW / rw, sy = canvasH / rh;
    bool canvasNew = out.width != canvasW || out.height != canvasH || out.pixels.size() != (size_t)canvasW * canvasH;
    if (canvasNew) out.pixels.assign((size_t)canvasW * canvasH, 0);
    out.width = canvasW; out.height = canvasH;
    g_canvasW = canvasW; g_canvasH = canvasH;
    out.x = rx; out.y = ry; out.w = rw; out.h = rh;
    out.visible = true; out.hideFrame = false;
    // rows to rebuild: base changes + instances that appeared, vanished or moved
    int r0 = INT_MAX, r1 = 0;
    auto addRows = [&](int gy0, int gy1) {
        r0 = std::min(r0, std::max(0, (int)((gy0 - ry) * sy) - 2));
        r1 = std::max(r1, std::min(canvasH, (int)((gy1 - ry) * sy) + 3));
    };
    if (full || canvasNew || !canvasSame) { r0 = 0; r1 = canvasH; }
    else {
        std::vector<Instance> diff;
        std::set_symmetric_difference(g_prev.begin(), g_prev.end(), now.begin(), now.end(), std::back_inserter(diff));
        for (auto& i : diff) addRows(i.ty, i.ty + std::max(g_items[i.id].h, i.cy1 - i.ty));
        if (baseDirty) { r0 = std::min(r0, base->dirtyY0); r1 = std::max(r1, base->dirtyY1); }
    }
    if (r1 > r0) {
        for (int y = r0; y < r1; y++) {
            uint32_t* d = out.pixels.data() + (size_t)y * canvasW;
            if (base && base->width == canvasW && base->height == canvasH) memcpy(d, base->pixels.data() + (size_t)y * canvasW, (size_t)canvasW * 4);
            else memset(d, 0, (size_t)canvasW * 4);
        }
        for (auto& i : now) {
            const Item& it = g_items[i.id];
            int ox = (int)lroundf((i.tx - rx) * sx), oy = (int)lroundf((i.ty - ry) * sy);
            // clip to the visible part (with a margin for anti-aliasing)
            int cx0 = (int)((i.cx0 - 3 - rx) * sx), cy0 = std::max(r0, (int)((i.cy0 - 2 - ry) * sy));
            int cx1 = (int)((i.cx1 + 3 - rx) * sx), cy1 = std::min(r1, (int)((i.cy1 + 2 - ry) * sy));
            if (cy1 <= cy0) continue;
            auto sp = RenderItem(it, sy, i.color, i.bk);
            for (int y = std::max(cy0, oy); y < std::min(cy1, oy + sp->h); y++) {
                if (y < 0 || y >= canvasH) continue;
                uint32_t* d = out.pixels.data() + (size_t)y * canvasW;
                const uint32_t* s = sp->px.data() + (size_t)(y - oy) * sp->w;
                for (int x = std::max({cx0, ox, 0}); x < std::min({cx1, ox + sp->w, canvasW}); x++) {
                    uint32_t c = s[x - ox], a = c >> 24;
                    if (!a) continue;
                    if (a == 255) { d[x] = c; continue; }
                    uint32_t dc = d[x], ia = 255 - a;
                    uint32_t A = a + ((dc >> 24) * ia) / 255;
                    uint32_t R = ((c >> 16) & 0xFF) + (((dc >> 16) & 0xFF) * ia) / 255;
                    uint32_t G = ((c >> 8) & 0xFF) + (((dc >> 8) & 0xFF) * ia) / 255;
                    uint32_t B = (c & 0xFF) + ((dc & 0xFF) * ia) / 255;
                    d[x] = (A << 24) | (R << 16) | (G << 8) | B;
                }
            }
        }
    }
    out.dirtyY0 = r1 > r0 ? r0 : 0;
    out.dirtyY1 = r1 > r0 ? r1 : 0;
    g_prev = std::move(now);
    LeaveCriticalSection(&g_lock);
    return true;
}

} // namespace swgbm
