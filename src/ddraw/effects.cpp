// Smooth fog (see effects.h).
//
// Engine side: two tiny routines in the THIS_COD blitter section load the
// stipple pattern registers: four row masks and four row fills (pixel =
// (pixel & mask) | fill, row = y & 3). setPattern4(m02, f02, m13, f13) and
// setPattern8(const uint32_t[8]). They are replaced by equivalents that also put
// a marker index into the fill wherever the mask clears a byte and the fill is 0
// (the black stipple used for fog). The marker is a palette entry that is black
// too, so the image is unchanged where the presenter does not process it.
//
// Presenter side: a marker pixel whose diagonal neighbours are markers as well
// is stipple. It takes the colour of a non-stippled 4-neighbour, and the
// stippled area is darkened through the shade plane. A 50% black stipple looks
// like ~73% brightness (it averages in linear light), so that is the default.
#include "effects.h"
#include "hook.h"
#include <algorithm>

namespace swgbm {
namespace {

const uint8_t kMarker = 48;            // palette 50500: black, rare in the art
bool g_enabled = false;
uint32_t* g_regs = nullptr;            // pattern registers: masks [0..3], fills [4..7]
uint8_t g_shade = 94;                  // fog multiplier (x/128)

void FixFills() {
    for (int r = 0; r < 4; r++) {
        uint32_t m = g_regs[r], f = g_regs[4 + r];
        if (f == 0 && m != 0xFFFFFFFFu) g_regs[4 + r] = ~m & (kMarker * 0x01010101u);
    }
}

// mov [60],a; [68],a; [70],b; [78],b; [64],c; [6c],c; [74],d; [7c],d
void __cdecl SetPattern4(uint32_t m02, uint32_t f02, uint32_t m13, uint32_t f13) {
    g_regs[0] = g_regs[2] = m02;
    g_regs[4] = g_regs[6] = f02;
    g_regs[1] = g_regs[3] = m13;
    g_regs[5] = g_regs[7] = f13;
    FixFills();
}
void __cdecl SetPattern8(const uint32_t* p) {
    for (int i = 0; i < 8; i++) g_regs[i] = p[i];
    FixFills();
}

std::vector<uint16_t> g_rowStipple;    // stippled pixels per row (to drop the plane when none)
long g_total = 0;

inline bool M(const uint8_t* px, int w, int h, int x, int y) {
    return x >= 0 && y >= 0 && x < w && y < h && px[(size_t)y * w + x] == kMarker;
}

} // namespace

void Effects::InstallEngineHooks(uintptr_t setPattern4, uintptr_t setPattern8, uintptr_t patternRegs) {
    g_enabled = CfgBool("Enhancements", "SmoothFog", false);
    if (!g_enabled) return;
    int pct = std::max(30, std::min(100, CfgInt("Enhancements", "FogBrightness", 73)));
    g_shade = (uint8_t)(pct * 128 / 100);
    g_regs = (uint32_t*)patternRegs;
    // verify both routines before replacing them
    static const uint8_t p4[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0xA3};
    static const uint8_t p8[] = {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0x75, 0x08};
    bool ok = PatchCheck(setPattern4, p4, sizeof(p4)) && PatchCheck(setPattern8, p8, sizeof(p8)) &&
              PatchJump(setPattern4, (void*)&SetPattern4, 5) && PatchJump(setPattern8, (void*)&SetPattern8, 5);
    LOG("smooth fog: %s (brightness %d%%)", ok ? "pattern hooks installed" : "engine code not recognised - disabled", pct);
    if (!ok) g_enabled = false;
}

bool Effects::Enabled() { return g_enabled; }

bool Effects::Compose(const Frame& in, Frame& out, bool forceFull) {
    if (!g_enabled) return false;
    const int W = in.width, H = in.height;
    bool full = forceFull || out.width != W || out.height != H || out.pixels.size() != in.pixels.size();
    if (full) {
        out.width = W; out.height = H;
        out.pixels = in.pixels;
        out.shade.assign((size_t)W * H, 128);
        g_rowStipple.assign(H, 0);
        g_total = 0;
    }
    memcpy(out.palette, in.palette, sizeof(out.palette));
    out.paletteChanged = in.paletteChanged || full;
    // rows to redo: changed rows plus one row around them (neighbour tests)
    int y0 = full ? 0 : std::max(0, in.dirtyY0 - 1), y1 = full ? H : std::min(H, in.dirtyY1 + 1);
    if (in.dirtyY1 <= in.dirtyY0 && !full) { y0 = y1 = 0; }
    const uint8_t* px = in.pixels.data();
    for (int y = y0; y < y1; y++) {
        const uint8_t* s = px + (size_t)y * W;
        uint8_t* d = out.pixels.data() + (size_t)y * W;
        uint8_t* k = out.shade.data() + (size_t)y * W;
        memcpy(d, s, W);
        memset(k, 128, W);
        int n = 0;
        for (int x = 0; x < W; x++) {
            if (s[x] != kMarker) continue;
            if (!(M(px, W, H, x - 1, y - 1) || M(px, W, H, x + 1, y - 1) || M(px, W, H, x - 1, y + 1) || M(px, W, H, x + 1, y + 1))) continue;
            n++;
            // colour from a non-stippled 4-neighbour
            int c = -1;
            if (x > 0 && s[x - 1] != kMarker) c = s[x - 1];
            else if (x + 1 < W && s[x + 1] != kMarker) c = s[x + 1];
            else if (y > 0 && px[(size_t)(y - 1) * W + x] != kMarker) c = px[(size_t)(y - 1) * W + x];
            else if (y + 1 < H && px[(size_t)(y + 1) * W + x] != kMarker) c = px[(size_t)(y + 1) * W + x];
            d[x] = c >= 0 ? (uint8_t)c : 0;
            k[x] = g_shade;
            if (x > 0) k[x - 1] = g_shade;
            if (x + 1 < W) k[x + 1] = g_shade;
        }
        g_total += n - g_rowStipple[y];
        g_rowStipple[y] = (uint16_t)n;
    }
    // vertical neighbours of stipple are fogged too (rows above/below a changed row)
    for (int y = y0; y < y1; y++) {
        uint8_t* k = out.shade.data() + (size_t)y * W;
        const uint8_t* s = px + (size_t)y * W;
        for (int dy = -1; dy <= 1; dy += 2) {
            int yy = y + dy;
            if (yy < 0 || yy >= H || !g_rowStipple[yy]) continue;
            const uint8_t* ss = px + (size_t)yy * W;
            for (int x = 0; x < W; x++)
                if (ss[x] == kMarker && s[x] != kMarker) k[x] = g_shade;
        }
    }
    out.dirtyY0 = y1 > y0 ? y0 : 0;
    out.dirtyY1 = y1 > y0 ? y1 : 0;
    if (full) out.dirtyY1 = H;
    out.text.clear();
    return true;
}

} // namespace swgbm
