#pragma once
#include "../common/common.h"

namespace swgbm {
// Replace an import of `module` (the main exe if null) from DLL `dll` named `func`.
// Returns the original function pointer, or null if not imported.
void* HookIAT(HMODULE module, const char* dll, const char* func, void* replacement);
// Write bytes into code/data of the main module (handles page protection).
bool PatchBytes(void* addr, const void* data, size_t len);
bool PatchCheck(uintptr_t addr, const void* expect, size_t len);
// Write a 5-byte relative JMP/CALL at addr to target (pads with NOPs up to len).
bool PatchJump(uintptr_t addr, void* target, size_t len, bool call = false);
// Small executable allocations (stubs, trampolines), packed into shared pages.
uint8_t* ExecAlloc(size_t len);
// Detour a function whose first `len` bytes equal `prologue` (whole
// instructions, >= 5) to `hook`. Returns a callable trampoline to the
// original (prologue + jump back), or null if the bytes do not match.
void* HookFunction(uintptr_t addr, const uint8_t* prologue, size_t len, void* hook);
}
