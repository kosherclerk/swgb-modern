// Minimal SLP (Genie sprite) codec, enough for interface art.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace swgbm {

struct SlpFrame {
    int w = 0, h = 0, hx = 0, hy = 0;
    std::vector<int16_t> px; // palette index, -1 transparent
    int16_t& at(int x, int y) { return px[(size_t)y * w + x]; }
};

bool SlpDecode(const uint8_t* data, size_t size, std::vector<SlpFrame>& out);
std::string SlpEncode(const std::vector<SlpFrame>& frames);

} // namespace swgbm
