// Engine patches applied in memory to known executables only.
#include "patches.h"
#include "hook.h"
#include "resources.h"
#include <vector>

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

// Clone Campaigns 1.1 (2002-05-30): places where the 1280x1024 mode is
// identified or requested. Re-targeted to the custom resolution.
const ImmSite kCCWidescreen[] = {
    {0x45a364, 0x500, 'W'},              // in-game mode select from config
    {0x45acb9, 0x500, 'W'}, {0x45acbe, 0x400, 'H'}, // options screen apply
    {0x50e4cc, 0x500, 'W'}, {0x50e4d1, 0x400, 'H'}, // options screen apply (game screen)
    {0x50edcb, 0x500, 'W'},              // options screen current-mode button
    {0x470df9, 0x500, 'W'}, {0x470e01, 0x400, 'H'}, // display mode capability table
    {0x4710d3, 0x500, 'W'},              // display mode lookup
    {0x46a914, 0x500, 'W'},              // game screen layout
    {0x4f670b, 0x500, 'W'},              // game screen layout
    {0x5003a7, 0x500, 'W'},              // game screen layout
    {0x502851, 0x500, 'W'},              // game screen layout
    {0x42aa3d, 0x500, 'W'},              // layout
    {0x5d853e, 0x500, 'W'}, {0x5d8d45, 0x500, 'W'}, {0x5d98e8, 0x500, 'W'},
};

void __stdcall TraceSetRes(void* self, int w, int h) {
    auto o = (uint8_t*)self;
    auto disp = *(uint8_t**)(o + 0x68);
    LOG("trace: change resolution -> %dx%d (cur draw %dx%d, mode flags %d/%d)", w, h,
        disp ? *(int*)(disp + 0x18) : -1, disp ? *(int*)(disp + 0x1c) : -1, disp ? disp[0x3c] : -1, disp ? disp[0x3d] : -1);
}

void ApplyCloneCampaigns() {
    if (CfgBool("Debug", "TraceEngine", false)) {
        // 42ca10: sub esp,20h; push ebx; push ebp  (thiscall, args w,h)
        Cave c;
        c.u8(0x60);                                   // pushad
        c.raw({0xFF, 0x74, 0x24, 0x28});              // push [esp+28h]  (h)
        c.raw({0xFF, 0x74, 0x24, 0x28});              // push [esp+28h]  (w)
        c.u8(0x51);                                   // push ecx (this)
        c.call((uintptr_t)&TraceSetRes);
        c.u8(0x61);                                   // popad
        c.raw({0x83, 0xEC, 0x20, 0x53, 0x55});        // original
        c.jmp(0x42ca15);
        Detour(0x42ca10, {0x83, 0xEC, 0x20, 0x53, 0x55}, c, "trace set-resolution");
    }
    int W = CfgInt("Display", "GameWidth", 0), H = CfgInt("Display", "GameHeight", 0);
    if (CfgBool("Fixes", "Widescreen", true) && W >= 800 && H >= 600 && !(W == 1280 && H == 1024)) {
        // Registry "Screen Size" == 1280 -> custom W x H.
        // 424bb9: mov edx,[ebp+24]; mov [edx+8f4],eax; mov eax,[ebp+24]; mov [eax+8fc],ecx
        Cave c;
        c.raw({0x8B, 0x55, 0x24});                       // mov edx,[ebp+24]
        c.raw({0xC7, 0x82, 0xF4, 0x08, 0, 0}); c.u32(W); // mov [edx+8f4],W
        c.raw({0xC7, 0x82, 0xFC, 0x08, 0, 0}); c.u32(H); // mov [edx+8fc],H
        c.jmp(0x424bcb);
        Detour(0x424bb9, {0x8B, 0x55, 0x24, 0x89, 0x82, 0xF4, 0x08, 0, 0, 0x8B, 0x45, 0x24, 0x89, 0x88, 0xFC, 0x08, 0, 0}, c,
               "widescreen: startup mode");
        // 42c960 "enter game resolution": always use the configured size
        // (the player profile otherwise decides between 800/1024/1280).
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
        Detour(0x42c960, {0x8B, 0x41, 0x24, 0x8B, 0x11}, e, "widescreen: enter game resolution");
        static const uint8_t resPrologue[] = {0x83, 0xEC, 0x08, 0x8D, 0x44, 0x24, 0x0C};
        Resources::Install(0x4d5060, 0x632cca, resPrologue, sizeof(resPrologue), W, H, {53290, 50006});
        int ok = 0;
        for (auto& site : kCCWidescreen) ok += PatchImm(site.at, site.oldv, site.kind == 'W' ? W : H);
        LOG("widescreen: %d/%d immediate sites patched for %dx%d", ok, (int)(sizeof(kCCWidescreen) / sizeof(kCCWidescreen[0])), W, H);
    }
}

} // namespace

namespace Patches {
void Apply(GameExe exe) {
    if (exe == GameExe::CloneCampaigns) ApplyCloneCampaigns();
}
} // namespace Patches
} // namespace swgbm
