#pragma once
#include "renderer.h"

namespace swgbm {

// Graphics enhancements that need colours the 256-colour palette does not have.
// They work through the frame's shade plane (a per-pixel colour multiplier the
// renderers apply after the palette lookup), so the engine keeps drawing its
// usual 8-bit image and nothing changes when they are off.
//
// Smooth fog: the engine darkens fogged areas with a black checkerboard stipple
// (a 4-row AND/OR pattern in its sprite blitter). With [Enhancements] SmoothFog
// the pattern's fill is set to a marker index; the presenter then replaces each
// stippled pixel with its neighbour's colour and darkens the area evenly.
namespace Effects {
void InstallEngineHooks(uintptr_t setPattern4, uintptr_t setPattern8, uintptr_t patternRegs);
bool Enabled();
// in -> out (persistent between calls): pixels with the stipple removed and the
// shade plane filled; out's dirty range covers what changed. Returns false when
// no effect is active (out is then not used).
bool Compose(const Frame& in, Frame& out, bool forceFull);
} // namespace Effects

} // namespace swgbm
