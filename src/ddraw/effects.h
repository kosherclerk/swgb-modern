#pragma once
#include "renderer.h"

namespace swgbm {

// Smooth fog ([Enhancements] SmoothFog, FogBrightness): fogged sprites and
// terrain are drawn with all their pixels and darkened through a palette table
// instead of the engine's black checkerboard stipple (see effects.cpp).
namespace Effects {
void InstallEngineHooks(uintptr_t blitter, uintptr_t blitState);
bool Enabled();
} // namespace Effects

} // namespace swgbm
