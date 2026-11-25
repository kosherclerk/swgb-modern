#include "hook.h"
#include <string.h>

namespace swgbm {

void* HookIAT(HMODULE module, const char* dll, const char* func, void* replacement) {
    auto base = (uint8_t*)(module ? module : GetModuleHandleW(nullptr));
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    void* orig = nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func) != 0) continue;
            DWORD old;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            orig = (void*)addrs->u1.Function;
            addrs->u1.Function = (uintptr_t)replacement;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
        }
    }
    return orig;
}

bool PatchBytes(void* addr, const void* data, size_t len) {
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, data, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return true;
}

bool PatchCheck(uintptr_t addr, const void* expect, size_t len) {
    return memcmp((void*)addr, expect, len) == 0;
}

bool PatchJump(uintptr_t addr, void* target, size_t len, bool call) {
    if (len < 5) return false;
    uint8_t buf[32];
    memset(buf, 0x90, sizeof(buf));
    buf[0] = call ? 0xE8 : 0xE9;
    int32_t rel = (int32_t)((uintptr_t)target - (addr + 5));
    memcpy(buf + 1, &rel, 4);
    return PatchBytes((void*)addr, buf, len);
}

} // namespace swgbm
