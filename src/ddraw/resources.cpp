// Runtime resource overrides. The engine fetches every DRS resource through one
// getter (type tag + id); we detour it and may substitute generated data.
//
// Used for widescreen: the in-game interface layout is a tab-separated table
// ("Resolution Specific Rects") with columns for 800x600, 1024x768 and
// 1280x1024. We rewrite the 1280x1024 column for the configured resolution,
// so the engine lays out its own UI for the wider screen.
#include "resources.h"
#include "hook.h"
#include "slp.h"
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <algorithm>

namespace swgbm {
namespace {

typedef void*(__fastcall* ResGetFn)(uint32_t type, uint32_t id, int* allocFlag, int* size);
ResGetFn g_orig = nullptr;
typedef void(__cdecl* FreeFn)(void*);
FreeFn g_gameFree = nullptr; // the engine's own CRT free()
typedef void*(__cdecl* MallocFn)(size_t);
MallocFn g_gameMalloc = nullptr; // and malloc()
int g_W = 0, g_H = 0;   // game (internal) size
int g_PW = 0;           // panel width (the display width; == g_W unless zooming)
std::vector<uint32_t> g_layoutIds;
CRITICAL_SECTION g_lock;
std::map<uint64_t, std::string> g_cache;

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

// Panel geometry (1280 layout). The in-game panel is three blocks: the unit
// grid (left), the info panel with the Tech Level box on top (centre) and the
// minimap/buttons (right). "stretch" widens the info panel the way Expanding
// Fronts does: rects that start in the right block move right by the extra
// width, rects that reach into the stretch zone widen, the left block stays.
// "center" keeps the 1280 panel centred instead.
struct PanelGeom { bool stretch; int zoneL, zoneR; };
PanelGeom Geom() {
    return {CfgStr("Widescreen", "PanelStyle", "stretch") != "center", CfgInt("Widescreen", "PanelZoneLeft", 436),
            CfgInt("Widescreen", "PanelZoneRight", 690)};
}

std::string RewriteLayout(const std::string& src, int W, int H, int PW) {
    // W x H: the game's size; the panel and its controls span PW (the display
    // width), the battlefield W. They differ only with mouse-wheel zoom on.
    const PanelGeom g = Geom();
    const int extra = PW - 1280, dx = extra / 2, dy = H - 1024;
    std::string out;
    bool inRes = false;
    size_t a = 0;
    int rows = 0;
    while (a < src.size()) {
        size_t b = src.find('\n', a);
        std::string line = src.substr(a, b == std::string::npos ? std::string::npos : b - a + 1);
        a = b == std::string::npos ? src.size() : b + 1;
        std::string eol;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) { eol.insert(eol.begin(), line.back()); line.pop_back(); }
        auto f = Split(line, '\t');
        if (!f.empty() && f[0] == "Item Name") inRes = f.size() > 1 && f[1].find("Resolution Specific") != std::string::npos;
        if (inRes && f.size() >= 13 && !f[0].empty() && f[0] != "Item Name" && isdigit((unsigned char)f[9][0])) {
            int x = atoi(f[9].c_str()), y = atoi(f[10].c_str()), w = atoi(f[11].c_str()), h = atoi(f[12].c_str());
            const std::string& n = f[0];
            if (n == "ToolBox") { w = W; }
            else if (n.rfind("Main Viewport", 0) == 0) { w = W; h += dy; }
            else if (n == "Objectives Changed Text") { x = W / 2; w = W / 2; }
            else {
                y += dy;
                if (!g.stretch) x += dx;
                else if (x >= g.zoneR) x += extra;
                else if (x + w > g.zoneL) w += extra;
            }
            f[9] = std::to_string(x); f[10] = std::to_string(y); f[11] = std::to_string(w); f[12] = std::to_string(h);
            rows++;
            line.clear();
            for (size_t i = 0; i < f.size(); i++) { if (i) line += '\t'; line += f[i]; }
        }
        out += line + eol;
    }
    LOG("widescreen: layout table rewritten (%d rects) for %dx%d, panel %s", rows, W, H, g.stretch ? "stretched" : "centred");
    return out;
}

// Interface palette (for colour distances when choosing the seam); empty if
// it can't be loaded, then pixels only compare equal / different.
std::vector<uint32_t> g_pal;
void LoadPalette();

int PixDist(int16_t a, int16_t b) {
    if (a == b) return 0;
    if (a < 0 || b < 0 || g_pal.size() != 256) return 255;
    uint32_t p = g_pal[a], q = g_pal[b];
    return abs((int)(p >> 16 & 255) - (int)(q >> 16 & 255)) + abs((int)(p >> 8 & 255) - (int)(q >> 8 & 255)) + abs((int)(p & 255) - (int)(q & 255));
}

// Widen a 1280x250 in-game panel SLP to W. "stretch": find the plainest
// connected vertical path through the info panel (one pixel per row, moving at
// most one column between rows, lowest local detail; dynamic programming) and
// repeat that path's pixel on each row. The path runs between ornaments, so
// corner pieces, buttons and emblems keep their shape while plain frame,
// background and the Tech Level box get longer, as in Expanding Fronts.
// "center": the original art centred, both sides filled from a plain strip.
std::string WidenPanel(const uint8_t* data, size_t size, int W, int H) {
    std::vector<SlpFrame> fr;
    if (!SlpDecode(data, size, fr) || fr.empty() || fr[0].w != 1280) return std::string();
    SlpFrame& o = fr[0];
    const PanelGeom g = Geom();
    const int extra = W - 1280, dx = extra / 2;
    SlpFrame n;
    n.w = W; n.h = o.h; n.hx = 0; n.hy = -(H - o.h);
    n.px.assign((size_t)n.w * n.h, -1);
    if (g.stretch) {
        LoadPalette();
        const int lo = std::max(1, CfgInt("Widescreen", "SeamLeft", 300)), hi = std::min(1278, CfgInt("Widescreen", "SeamRight", g.zoneR));
        const int cw = hi - lo, h = o.h;
        std::vector<int> cost((size_t)cw * h), from((size_t)cw * h);
        for (int y = 0; y < h; y++)
            for (int i = 0; i < cw; i++) {
                int x = lo + i;
                int e = PixDist(o.at(x, y), o.at(x - 1, y)) + PixDist(o.at(x, y), o.at(x + 1, y));
                if (y == 0) { cost[i] = e; from[i] = i; continue; }
                int best = INT_MAX, bi = i;
                for (int d = -1; d <= 1; d++) {
                    int j = i + d;
                    if (j < 0 || j >= cw) continue;
                    int c = cost[(size_t)(y - 1) * cw + j] + (d ? 1 : 0);   // small penalty for shearing
                    if (c < best) { best = c; bi = j; }
                }
                cost[(size_t)y * cw + i] = best + e;
                from[(size_t)y * cw + i] = bi;
            }
        int i = 0;
        for (int k = 1; k < cw; k++) if (cost[(size_t)(h - 1) * cw + k] < cost[(size_t)(h - 1) * cw + i]) i = k;
        std::vector<int> seam(h);
        for (int y = h - 1; y >= 0; y--) { seam[y] = lo + i; i = from[(size_t)y * cw + i]; }
        for (int y = 0; y < h; y++) {
            const int16_t* row = &o.px[(size_t)y * 1280];
            int16_t* out = &n.px[(size_t)y * W];
            int s = seam[y];
            memcpy(out, row, (s + 1) * sizeof(int16_t));
            for (int k = 0; k < extra; k++) out[s + 1 + k] = row[s];
            memcpy(out + s + 1 + extra, row + s + 1, (1280 - s - 1) * sizeof(int16_t));
        }
    } else {
        const int s0 = CfgInt("Widescreen", "FillStripX", 236), sw = CfgInt("Widescreen", "FillStripW", 56);
        for (int y = 0; y < n.h; y++) {
            const int16_t* row = &o.px[(size_t)y * 1280];
            for (int x = 0; x < n.w; x++) {
                int sx = x - dx;
                n.at(x, y) = (sx >= 0 && sx < 1280) ? row[sx] : row[s0 + (sx < 0 ? (sw - 1 - ((-sx - 1) % sw)) : ((sx - 1280) % sw))];
            }
        }
    }
    return SlpEncode({n});
}

// Every load gets its own copy, allocated with the engine's own malloc() and
// marked as allocated (allocFlag 1): the engine edits loaded data in place and
// frees resources with its own free() when it leaves a screen. Handing it
// memory from another heap corrupted the engine's heap when a widescreen game
// was quit (the widened command panel sprites are freed then).
void* Fresh(const std::string& data, int* allocFlag, int* size) {
    char* m = g_gameMalloc ? (char*)g_gameMalloc(data.size() + 16) : nullptr;
    if (!m) return nullptr;
    memcpy(m, data.data(), data.size());
    memset(m + data.size(), 0, 16);
    *allocFlag = 1;
    *size = (int)data.size();
    return m;
}

// The interface palette (JASC text, bina 50500) fetched through the engine's
// own getter, once.
void LoadPalette() {
    static bool done = false;
    if (done) return;
    done = true;
    int alloc = 0, size = 0;
    const char* p = (const char*)g_orig(0x62696e61 /* 'bina' */, 50500, &alloc, &size);
    if (!p || size <= 0) return;
    std::istringstream in(std::string(p, (size_t)size));
    std::string magic, ver; int count = 0;
    in >> magic >> ver >> count;
    if (magic == "JASC-PAL" && count == 256) {
        std::vector<uint32_t> pal(256);
        bool ok = true;
        for (int i = 0; i < 256 && ok; i++) { int r, g, b; ok = (bool)(in >> r >> g >> b); pal[i] = (uint32_t)(r << 16 | g << 8 | b); }
        if (ok) g_pal = pal;
    }
    if (alloc == 1 && g_gameFree) g_gameFree((void*)p);
    LOG("widescreen: interface palette %s", g_pal.empty() ? "not found (seam by index)" : "loaded");
}

void* __fastcall ResGetHook(uint32_t type, uint32_t id, int* allocFlag, int* size) {
    // Above 1280-class sizes (a zoomed-out battlefield) the engine asks for a
    // fourth panel set (51151..51158) that the game does not have: serve the
    // 1280 panels (51141..51148), widened like the others.
    if (type == 0x736c7020 /* 'slp ' */ && g_W > 0 && id >= 51151 && id <= 51158) id -= 10;
    void* p = g_orig(type, id, allocFlag, size);
    if (!p) return p;
    if (type == 0x736c7020 /* 'slp ' */ && g_W > 0 && id >= 51141 && id <= 51148) {
        EnterCriticalSection(&g_lock);
        uint64_t key = ((uint64_t)type << 32) | id;
        auto it = g_cache.find(key);
        if (it == g_cache.end()) {
            std::string w = WidenPanel((const uint8_t*)p, (size_t)*size, g_PW, g_H);
            LOG("widescreen: panel slp %u -> %s", id, w.empty() ? "unchanged (decode failed)" : "widened");
            it = g_cache.emplace(key, w).first;
        }
        std::string copy = it->second;
        LeaveCriticalSection(&g_lock);
        if (copy.empty()) return p;
        if (*allocFlag == 1 && g_gameFree) g_gameFree(p);
        return Fresh(copy, allocFlag, size);
    }
    if (type != 0x62696e61 /* 'bina' */) return p;
    for (uint32_t lid : g_layoutIds) {
        if (id != lid) continue;
        EnterCriticalSection(&g_lock);
        uint64_t key = ((uint64_t)type << 32) | id;
        auto it = g_cache.find(key);
        if (it == g_cache.end()) {
            std::string src((const char*)p, (size_t)*size);
            it = g_cache.emplace(key, RewriteLayout(src, g_W, g_H, g_PW)).first;
        }
        std::string copy = it->second;
        LeaveCriticalSection(&g_lock);
        if (*allocFlag == 1 && g_gameFree) g_gameFree(p);
        return Fresh(copy, allocFlag, size);
    }
    return p;
}

} // namespace

bool Resources::Install(uintptr_t getter, uintptr_t gameFree, uintptr_t gameMalloc, const uint8_t* prologue, size_t prologueLen, int W, int H, int PW,
                        std::initializer_list<uint32_t> layoutIds) {
    InitializeCriticalSection(&g_lock);
    g_W = W; g_H = H; g_PW = PW > 0 ? PW : W;
    g_gameFree = (FreeFn)gameFree;
    g_gameMalloc = (MallocFn)gameMalloc;
    g_layoutIds.assign(layoutIds);
    g_orig = (ResGetFn)HookFunction(getter, prologue, prologueLen, (void*)&ResGetHook);
    if (!g_orig) { LOG("resources: getter prologue mismatch at %08x", (unsigned)getter); return false; }
    LOG("resources: getter hooked at %08x", (unsigned)getter);
    return true;
}

} // namespace swgbm
