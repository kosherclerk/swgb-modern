// Smooth fog of war (see effects.h).
//
// The engine draws fogged sprites and terrain through its masked sprite blitter
// (THIS_COD 0x651bc0 in Clone Campaigns, 0x64ebc0 in the base game) with a
// black checkerboard pattern: pixel = (pixel & mask[row & 3]) | fill[row & 3],
// so half of every fogged pixel pair is simply black. The blitter is wrapped;
// for a fogged draw (pattern enabled, black fill):
//   * a coverage pass draws the sprite with an all-marker pattern into a
//     scratch buffer (through the blitter's row table) to learn exactly
//     which pixels it covers;
//   * the real draw uses a keep-everything pattern, so the sprite or terrain
//     tile comes out with all its pixels;
//   * those pixels are darkened through a palette table (each colour -> the
//     nearest palette colour to it at the fog brightness), the way later Genie
//     games shade fog.
// Everything happens inside the 8-bit frame the engine draws, so partial
// redraws, scrolling and every copy to the screen keep working unchanged.
#include "effects.h"
#include "hook.h"
#include "ddraw_impl.h"
#include <algorithm>

namespace swgbm {
namespace {

bool g_enabled = false;
uint8_t g_shade = 94;                       // fog brightness (x/128)

// engine blitter state (THIS_DAT); offsets from its base (0x7a4000 CC, 0x7a2000 base game)
uint8_t*** g_rowTable = nullptr;            // +0x00: pointer to the destination's row pointers
uint32_t* g_regs = nullptr;                 // +0x60: masks [0..3], fills [4..7]
int32_t* g_clip = nullptr;                  // +0x80: ymin, ymax, xmin, xmax

typedef void(__cdecl* BlitFn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
BlitFn g_blit = nullptr;
const uint8_t kMarker = 48;                 // rare index; only used inside a coverage pass

std::vector<uint8_t> g_cover;           // coverage scratch (marker = covered)
std::vector<uint8_t*> g_scratchRows;    // row table pointing into it

bool FogPattern() {
    bool fillBlack = !g_regs[4] && !g_regs[5] && !g_regs[6] && !g_regs[7];
    bool masked = (g_regs[0] & g_regs[1] & g_regs[2] & g_regs[3]) != 0xFFFFFFFFu;
    return fillBlack && masked && (g_regs[0] | g_regs[1] | g_regs[2] | g_regs[3]);
}

// darkening table for the current palette: index -> nearest palette colour to
// that colour times the fog brightness
uint8_t g_dark[256];
PALETTEENTRY g_darkPal[256];
bool g_darkValid = false;

void UpdateDarkTable() {
    DDPalette* pal = State().primaryPal;
    if (!pal) return;
    if (g_darkValid && memcmp(g_darkPal, pal->entries_, sizeof(g_darkPal)) == 0) return;
    memcpy(g_darkPal, pal->entries_, sizeof(g_darkPal));
    g_darkValid = true;
    for (int i = 0; i < 256; i++) {
        int r = g_darkPal[i].peRed * g_shade / 128, g = g_darkPal[i].peGreen * g_shade / 128, b = g_darkPal[i].peBlue * g_shade / 128;
        int best = i, bestD = INT_MAX;
        for (int j = 0; j < 256; j++) {
            int dr = g_darkPal[j].peRed - r, dg = g_darkPal[j].peGreen - g, db = g_darkPal[j].peBlue - b;
            int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
            if (d < bestD) { bestD = d; best = j; }
        }
        g_dark[i] = (uint8_t)best;
    }
}

void __cdecl BlitHook(uint32_t a1, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t a6, uint32_t a7, uint32_t flags) {
    if (!(flags & 1) || !FogPattern()) { g_blit(a1, x, y, w, h, a6, a7, flags); return; }
    // clipped rectangle (the blitter's own clip: y in [ymin,ymax], x in [xmin,xmax])
    int x0 = std::max<int>((int)x, g_clip[2]), x1 = std::min<int>((int)x + (int)w - 1, g_clip[3]);
    int y0 = std::max<int>((int)y, g_clip[0]), y1 = std::min<int>((int)y + (int)h - 1, g_clip[1]);
    uint8_t** rows = *g_rowTable;
    UpdateDarkTable();
    if (x1 < x0 || y1 < y0 || !rows || !g_darkValid) { g_blit(a1, x, y, w, h, a6, a7, flags); return; }
    // Coverage pass into a scratch buffer: the blitter finds each destination
    // row through the row table (*g_rowTable), so pointing that table at
    // scratch rows makes it draw there. Scratch starts as 0 and the sprite is
    // drawn with an all-marker pattern: marker = a pixel the sprite covers.
    // The real surface is never touched by this pass.
    int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
    g_cover.assign((size_t)rw * rh, 0);
    if ((int)g_scratchRows.size() <= y1) g_scratchRows.resize((size_t)y1 + 1);
    for (int yy = 0; yy < rh; yy++) g_scratchRows[y0 + yy] = g_cover.data() + (size_t)yy * rw - x0;
    uint32_t saved[8];
    memcpy(saved, g_regs, sizeof(saved));
    for (int i = 0; i < 4; i++) { g_regs[i] = 0; g_regs[4 + i] = kMarker * 0x01010101u; }
    *g_rowTable = g_scratchRows.data();
    g_blit(a1, x, y, w, h, a6, a7, flags);
    *g_rowTable = rows;
    // the real draw with all the sprite's pixels, then those pixels darkened
    for (int i = 0; i < 4; i++) { g_regs[i] = 0xFFFFFFFFu; g_regs[4 + i] = 0; }
    g_blit(a1, x, y, w, h, a6, a7, flags);
    memcpy(g_regs, saved, sizeof(saved));
    for (int yy = 0; yy < rh; yy++) {
        uint8_t* row = rows[y0 + yy] + x0;
        const uint8_t* cv = &g_cover[(size_t)yy * rw];
        for (int xx = 0; xx < rw; xx++) if (cv[xx] == kMarker) row[xx] = g_dark[row[xx]];
    }
}

} // namespace

void Effects::InstallEngineHooks(uintptr_t blitter, uintptr_t blitState) {
    g_enabled = CfgBool("Enhancements", "SmoothFog", false);
    if (!g_enabled) return;
    int pct = std::max(30, std::min(100, CfgInt("Enhancements", "FogBrightness", 73)));
    g_shade = (uint8_t)(pct * 128 / 100);
    g_rowTable = (uint8_t***)blitState;
    g_regs = (uint32_t*)(blitState + 0x60);
    g_clip = (int32_t*)(blitState + 0x80);
    static const uint8_t prologue[] = {0x55, 0x8B, 0xEC, 0x53, 0x56};   // push ebp; mov ebp,esp; push ebx; push esi
    bool ok = PatchCheck(blitter, prologue, sizeof(prologue));
    if (ok) {
        auto tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(tramp, prologue, sizeof(prologue));
        tramp[5] = 0xE9;
        int32_t rel = (int32_t)((blitter + 5) - ((uintptr_t)tramp + 10));
        memcpy(tramp + 6, &rel, 4);
        g_blit = (BlitFn)tramp;
        ok = PatchJump(blitter, (void*)&BlitHook, 5);
    }
    LOG("smooth fog: %s (brightness %d%%)", ok ? "sprite blitter hooked" : "engine code not recognised - disabled", pct);
    if (!ok) g_enabled = false;
}

bool Effects::Enabled() { return g_enabled; }

} // namespace swgbm
