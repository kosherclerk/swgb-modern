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
#include "hdmenus.h"
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
int g_W = 0, g_H = 0;
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

// Panel geometry (1280 layout): left of kSplit = unit grid (anchored left),
// right of it = info/minimap/buttons (anchored right); the central info panel
// spanning the split stretches. "center" keeps the 1280 panel centred instead.
struct PanelGeom { bool stretch; int split; int strip; };
PanelGeom Geom() {
    return {CfgStr("Widescreen", "PanelStyle", "stretch") != "center", CfgInt("Widescreen", "PanelSplitX", 436),
            std::max(4, CfgInt("Widescreen", "PanelStripW", 16))};
}

std::string RewriteLayout(const std::string& src, int W, int H) {
    const PanelGeom g = Geom();
    const int extra = W - 1280, dx = extra / 2, dy = H - 1024;
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
                else if (x >= g.split) x += extra;
                else if (x + w > g.split) w += extra;
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

// Widen a 1280x250 in-game panel SLP to W, keeping the original art centred
// and extending both sides by tiling a plain structural strip of the panel.
std::string WidenPanel(const uint8_t* data, size_t size, int W, int H) {
    std::vector<SlpFrame> fr;
    if (!SlpDecode(data, size, fr) || fr.empty() || fr[0].w != 1280) return std::string();
    const SlpFrame& o = fr[0];
    const PanelGeom g = Geom();
    const int extra = W - 1280, dx = extra / 2;
    SlpFrame n;
    n.w = W; n.h = o.h; n.hx = 0; n.hy = -(H - o.h);
    n.px.assign((size_t)n.w * n.h, -1);
    const int s0 = CfgInt("Widescreen", "FillStripX", 236), sw = CfgInt("Widescreen", "FillStripW", 56);
    for (int y = 0; y < n.h; y++) {
        const int16_t* row = &o.px[(size_t)y * 1280];
        for (int x = 0; x < n.w; x++) {
            int16_t v;
            if (g.stretch) {
                if (x < g.split) v = row[x];
                else if (x < g.split + extra) v = row[g.split + ((x - g.split) % g.strip)];
                else v = row[x - extra];
            } else {
                int sx = x - dx;
                if (sx >= 0 && sx < 1280) v = row[sx];
                else v = row[s0 + (sx < 0 ? (sw - 1 - ((-sx - 1) % sw)) : ((sx - 1280) % sw))];
            }
            n.at(x, y) = v;
        }
    }
    return SlpEncode({n});
}

void* __fastcall ResGetHook(uint32_t type, uint32_t id, int* allocFlag, int* size) {
    void* p = g_orig(type, id, allocFlag, size);
    if (!p) return p;
    if (type == 0x736c7020 /* 'slp ' */) HdMenus::OnSlpLoaded(p, (size_t)*size);
    if (type == 0x736c7020 /* 'slp ' */ && g_W > 0 && id >= 51141 && id <= 51148) {
        EnterCriticalSection(&g_lock);
        uint64_t key = ((uint64_t)type << 32) | id;
        auto it = g_cache.find(key);
        if (it == g_cache.end()) {
            std::string w = WidenPanel((const uint8_t*)p, (size_t)*size, g_W, g_H);
            LOG("widescreen: panel slp %u -> %s", id, w.empty() ? "unchanged (decode failed)" : "widened");
            it = g_cache.emplace(key, w).first;
        }
        LeaveCriticalSection(&g_lock);
        if (it->second.empty()) return p;
        if (*allocFlag == 1 && g_gameFree) g_gameFree(p);
        *allocFlag = 0;
        *size = (int)it->second.size();
        return (void*)it->second.data();
    }
    if (type != 0x62696e61 /* 'bina' */) return p;
    for (uint32_t lid : g_layoutIds) {
        if (id != lid) continue;
        EnterCriticalSection(&g_lock);
        uint64_t key = ((uint64_t)type << 32) | id;
        auto it = g_cache.find(key);
        if (it == g_cache.end()) {
            std::string src((const char*)p, (size_t)*size);
            it = g_cache.emplace(key, RewriteLayout(src, g_W, g_H)).first;
        }
        LeaveCriticalSection(&g_lock);
        if (*allocFlag == 1 && g_gameFree) g_gameFree(p);
        *allocFlag = 0;
        *size = (int)it->second.size();
        return (void*)it->second.data();
    }
    return p;
}

} // namespace

bool Resources::Install(uintptr_t getter, uintptr_t gameFree, const uint8_t* prologue, size_t prologueLen, int W, int H,
                        std::initializer_list<uint32_t> layoutIds) {
    InitializeCriticalSection(&g_lock);
    g_W = W; g_H = H;
    g_gameFree = (FreeFn)gameFree;
    g_layoutIds.assign(layoutIds);
    if (!PatchCheck(getter, prologue, prologueLen)) { LOG("resources: getter prologue mismatch at %08x", (unsigned)getter); return false; }
    // trampoline: original prologue + jmp back
    auto tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tramp, prologue, prologueLen);
    tramp[prologueLen] = 0xE9;
    int32_t rel = (int32_t)((getter + prologueLen) - ((uintptr_t)tramp + prologueLen + 5));
    memcpy(tramp + prologueLen + 1, &rel, 4);
    g_orig = (ResGetFn)tramp;
    if (!PatchJump(getter, (void*)&ResGetHook, prologueLen)) return false;
    LOG("resources: getter hooked at %08x", (unsigned)getter);
    return true;
}

} // namespace swgbm
