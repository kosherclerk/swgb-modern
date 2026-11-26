#pragma once
#include "renderer.h"
namespace swgbm { namespace HdMenus {
void Init();
// Called by the resource hook for every sprite (SLP) the engine loads.
void OnSlpLoaded(const void* data, size_t size);
// Presenter thread: compose the HD background into `out` for this frame.
// Returns false when no known background is on screen.
bool Compose(const Frame& f, Overlay& out);
void Invalidate();
} }
