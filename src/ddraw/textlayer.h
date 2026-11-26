#pragma once
#include "renderer.h"
namespace swgbm { namespace TextLayer {
void Install();
bool Enabled();
// Presenter thread. Builds `disp` (frame with low-res glyphs removed) and the
// HD text canvas `out` (on top of `base` when given). Canvas covers the game
// rectangle (rx, ry, rw, rh). Returns false when no tagged text is visible.
bool Compose(const Frame& f, Frame& disp, const Overlay* base, int canvasW, int canvasH, float rx, float ry, float rw, float rh,
             Overlay& out);
} }
