#pragma once
#include "../common/common.h"
#include <initializer_list>
namespace swgbm { namespace Resources {
// Detour the engine's DRS resource getter (fastcall: ecx=type, edx=id, stack: int* allocFlag, int* size).
bool Install(uintptr_t getter, uintptr_t gameFree, uintptr_t gameMalloc, const uint8_t* prologue, size_t prologueLen, int W, int H, int PW,
             std::initializer_list<uint32_t> layoutIds);
} }
