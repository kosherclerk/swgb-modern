// Engine patches applied in memory to known executables only.
#include "patches.h"
#include "hook.h"
#include "resources.h"
#include "effects.h"
#include <vector>
#include <algorithm>

namespace swgbm {
namespace {

// Tiny x86 code emitter for code caves.
struct Cave {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (i * 8))); }
    void raw(std::initializer_list<uint8_t> l) { b.insert(b.end(), l); }
    // jmp rel32 to absolute target, resolved when placed
    std::vector<std::pair<size_t, uintptr_t>> rel;
    void jmp(uintptr_t target) { u8(0xE9); rel.push_back({b.size(), target}); u32(0); }
    void call(uintptr_t target) { u8(0xE8); rel.push_back({b.size(), target}); u32(0); }
    uintptr_t Place() {
        auto mem = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!mem) return 0;
        memcpy(mem, b.data(), b.size());
        for (auto& r : rel) {
            int32_t d = (int32_t)(r.second - ((uintptr_t)mem + r.first + 4));
            memcpy(mem + r.first, &d, 4);
        }
        FlushInstructionCache(GetCurrentProcess(), mem, b.size());
        return (uintptr_t)mem;
    }
};

// Patch `len` bytes at `at` (verified against `expect`) with a jump to the cave.
bool Detour(uintptr_t at, std::initializer_list<uint8_t> expect, Cave& c, const char* what) {
    std::vector<uint8_t> e(expect);
    if (!PatchCheck(at, e.data(), e.size())) { LOG("patch %s: bytes at %08x differ - skipped", what, (unsigned)at); return false; }
    uintptr_t cave = c.Place();
    if (!cave || !PatchJump(at, (void*)cave, e.size())) { LOG("patch %s: failed", what); return false; }
    LOG("patch %s: ok", what);
    return true;
}

// Replace an imm32 inside the instruction at `at` (searched within 12 bytes).
bool PatchImm(uintptr_t at, uint32_t oldv, uint32_t newv) {
    auto p = (uint8_t*)at;
    for (int i = 1; i <= 8; i++) {
        uint32_t v;
        memcpy(&v, p + i, 4);
        if (v == oldv) return PatchBytes(p + i, &newv, 4);
    }
    LOG("imm patch at %08x: value %x not found", (unsigned)at, oldv);
    return false;
}

struct ImmSite { uintptr_t at; uint32_t oldv; char kind; }; // kind 'W' or 'H'

// Per-executable engine addresses. The base game and Clone Campaigns share
// the engine; sites were located in Battlegrounds.exe by masked byte
// signatures of the Clone Campaigns sites (tools/sigfind.py).
struct ExeLayout {
    const char* name;
    uintptr_t startupMode, startupResume;  // registry "Screen Size"==1280 branch
    uintptr_t enterGameRes;                // "enter game resolution" function
    uintptr_t resGetter, gameFree;         // DRS resource getter, engine CRT free()
    uintptr_t gameMalloc;                  // engine CRT malloc()
    uintptr_t viewInterval, mapInterval;   // game screen redraw throttles
    uintptr_t worldStep;                   // world update: "cmp eax,32h" (minimum ms between updates)
    uintptr_t setPattern4, setPattern8, patternRegs; // blitter stipple pattern (fog)
    uint32_t layoutTableId;                // "Resolution Specific Rects" table
    std::vector<ImmSite> sites;            // 1280x1024 mode identification
};

const ExeLayout kCloneCampaigns = {
    "Clone Campaigns", 0x424bb9, 0x424bcb, 0x42c960, 0x4d5060, 0x632cca, 0x632b9d, 0x4f2fd7, 0x4f2f17, 0x61e92a, 0x651780, 0x6517c0, 0x7a4060, 53290,
    {
        {0x45a364, 0x500, 'W'},                          // in-game mode select from config
        {0x45acb9, 0x500, 'W'}, {0x45acbe, 0x400, 'H'},  // options screen apply
        {0x50e4cc, 0x500, 'W'}, {0x50e4d1, 0x400, 'H'},  // options screen apply (game screen)
        {0x50edcb, 0x500, 'W'},                          // options screen current-mode button
        {0x470df9, 0x500, 'W'}, {0x470e01, 0x400, 'H'},  // display mode capability table
        {0x4710d3, 0x500, 'W'},                          // display mode lookup
        {0x46a914, 0x500, 'W'}, {0x4f670b, 0x500, 'W'},  // game screen layout
        {0x5003a7, 0x500, 'W'}, {0x502851, 0x500, 'W'}, {0x42aa3d, 0x500, 'W'},
        {0x5d853e, 0x500, 'W'}, {0x5d8d45, 0x500, 'W'}, {0x5d98e8, 0x500, 'W'},
    }};

const ExeLayout kBaseGame = {
    "Galactic Battlegrounds", 0x424329, 0x42433b, 0x42c0c0, 0x4d4b30, 0x62ef5a, 0x62ee2d, 0x4f1c57, 0x4f1b97, 0x61acaa, 0x64e780, 0x64e7c0, 0x7a2060, 50006,
    {
        {0x459a04, 0x500, 'W'},
        {0x45a359, 0x500, 'W'}, {0x45a35e, 0x400, 'H'},
        {0x50cfac, 0x500, 'W'}, {0x50cfb1, 0x400, 'H'},
        {0x50d8ab, 0x500, 'W'},
        {0x470359, 0x500, 'W'}, {0x470361, 0x400, 'H'},
        {0x470633, 0x500, 'W'},
        {0x469e94, 0x500, 'W'}, {0x4f531b, 0x500, 'W'},
        {0x4fef27, 0x500, 'W'}, {0x5013c1, 0x500, 'W'}, {0x42a18d, 0x500, 'W'},
        {0x5d499e, 0x500, 'W'}, {0x5d51a5, 0x500, 'W'}, {0x5d5d48, 0x500, 'W'},
    }};

// Smooth motion. Three limits keep the original game at 20 fps:
//  - the world update (unit positions, animation frames) runs at most every
//    50 ms: "cmp eax,32h / jae update" in the world update function. Units
//    move by elapsed time, so a shorter step only gives more, smaller steps;
//    game speed is unchanged. (Expanding Fronts ships the same change, 16 ms.)
//  - the game screen redraws its main view at most every 100 ms and the
//    minimap every 333 ms;
//  - the 50 ms WM_TIMER that drives redraws (shortened in window.cpp).
void ApplyFrameRate(const ExeLayout& L) {
    int fps = CfgInt("Fixes", "GameFPS", 60);
    if (fps <= 20 || fps > 240) return;
    uint32_t view = (uint32_t)(1000 / fps);
    uint32_t map = (uint32_t)std::max(50, std::min(333, CfgInt("Fixes", "MinimapRefreshMs", 100)));
    bool a = PatchImm(L.viewInterval, 0x64, view), b = PatchImm(L.mapInterval, 0x14d, map);
    bool c = false;
    const uint8_t cmp50[] = {0x83, 0xF8, 0x32, 0x73};
    if (PatchCheck(L.worldStep, cmp50, sizeof(cmp50))) {
        uint8_t step = (uint8_t)std::max<uint32_t>(10, view);
        c = PatchBytes((void*)(L.worldStep + 2), &step, 1);
    }
    LOG("frame rate: world update 50 -> %u ms (%s), main view redraw 100 -> %u ms (%s), minimap 333 -> %u ms (%s)",
        std::max<uint32_t>(10, view), c ? "ok" : "skipped", view, a ? "ok" : "skipped", map, b ? "ok" : "skipped");
}

void ApplyEngine(const ExeLayout& L) {
    LOG("engine patches for %s", L.name);
    ApplyFrameRate(L);
    Effects::InstallEngineHooks(L.setPattern4, L.setPattern8, L.patternRegs);
    int W = CfgInt("Display", "GameWidth", 0), H = CfgInt("Display", "GameHeight", 0);
    bool widescreen = CfgBool("Fixes", "Widescreen", true) && W >= 800 && H >= 600 && !(W == 1280 && H == 1024);
    static const uint8_t resPrologue[] = {0x83, 0xEC, 0x08, 0x8D, 0x44, 0x24, 0x0C};
    // The resource hook serves widescreen layouts/panels and HD menu detection.
    if (widescreen || CfgBool("Enhancements", "HDMenus", false))
        Resources::Install(L.resGetter, L.gameFree, L.gameMalloc, resPrologue, sizeof(resPrologue), widescreen ? W : 0, widescreen ? H : 0,
                           widescreen ? std::initializer_list<uint32_t>{L.layoutTableId} : std::initializer_list<uint32_t>{});
    if (!widescreen) return;
    // Registry "Screen Size" == 1280 -> custom W x H.
    // mov edx,[ebp+24]; mov [edx+8f4],eax; mov eax,[ebp+24]; mov [eax+8fc],ecx
    Cave c;
    c.raw({0x8B, 0x55, 0x24});                       // mov edx,[ebp+24]
    c.raw({0xC7, 0x82, 0xF4, 0x08, 0, 0}); c.u32(W); // mov [edx+8f4],W
    c.raw({0xC7, 0x82, 0xFC, 0x08, 0, 0}); c.u32(H); // mov [edx+8fc],H
    c.jmp(L.startupResume);
    Detour(L.startupMode, {0x8B, 0x55, 0x24, 0x89, 0x82, 0xF4, 0x08, 0, 0, 0x8B, 0x45, 0x24, 0x89, 0x88, 0xFC, 0x08, 0, 0}, c,
           "widescreen: startup mode");
    // "enter game resolution": always use the configured size (the player
    // profile otherwise decides between 800/1024/1280).
    Cave e;
    e.raw({0x8B, 0x41, 0x24});                         // mov eax,[ecx+24]
    e.raw({0xC7, 0x80, 0xF4, 0x08, 0, 0}); e.u32(W);   // mov [eax+8f4],W
    e.raw({0xC7, 0x80, 0xFC, 0x08, 0, 0}); e.u32(H);   // mov [eax+8fc],H
    e.raw({0x8B, 0x11});                               // mov edx,[ecx]
    e.raw({0xC7, 0x41, 0x5C, 1, 0, 0, 0});             // mov dword [ecx+5c],1
    e.u8(0x68); e.u32(H);                              // push H
    e.u8(0x68); e.u32(W);                              // push W
    e.raw({0xFF, 0x52, 0x68});                         // call [edx+68]
    e.u8(0xC3);                                        // ret
    Detour(L.enterGameRes, {0x8B, 0x41, 0x24, 0x8B, 0x11}, e, "widescreen: enter game resolution");
    int ok = 0;
    for (auto& site : L.sites) ok += PatchImm(site.at, site.oldv, site.kind == 'W' ? W : H);
    LOG("widescreen: %d/%d immediate sites patched for %dx%d", ok, (int)L.sites.size(), W, H);
}

} // namespace

namespace Patches {
void Apply(GameExe exe) {
    if (exe == GameExe::CloneCampaigns) ApplyEngine(kCloneCampaigns);
    else if (exe == GameExe::BaseGame) ApplyEngine(kBaseGame);
}
} // namespace Patches
} // namespace swgbm
