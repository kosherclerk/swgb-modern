#pragma once
#include "../common/common.h"
namespace swgbm { namespace TextLayer {
// Smooth text: hooks the engine's GDI text calls (textlayer.cpp).
void Install();
bool Enabled();
} }
