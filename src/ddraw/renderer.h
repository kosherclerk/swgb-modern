// Renderer backend interface. The game renders 8-bit palettized frames on the CPU
// (emulated DirectDraw surfaces); a backend only has to show such a frame scaled
// into the window. GPU backends upload the indices as an R8 texture plus a 256
// entry palette, resolve the palette in pass 1 (at game resolution, so filtering
// operates on real colors) and scale/filter into the swapchain in pass 2.
#pragma once
#include "../common/common.h"
#include <vector>
#include <memory>

namespace swgbm {

enum class Filter { Nearest = 0, Sharp = 1, Bilinear = 2, XBR = 3, Smooth4x = 4 };
enum class ScaleMode { Aspect = 0, Integer = 1, Stretch = 2 };

struct Viewport { int x, y, w, h; };

struct Frame {
    int width = 0, height = 0;          // game resolution
    std::vector<uint8_t> pixels;        // width*height indices, tightly packed
    uint32_t palette[256];              // 0x00RRGGBB entries
};

struct PresentOptions {
    Filter filter = Filter::Nearest;
    bool vsync = true;
    float sharpness = 0.5f;             // used by xBR / sharp filters
};

class Renderer {
public:
    virtual ~Renderer() {}
    virtual const char* Name() const = 0;
    // Called on the presenter thread. Return false if the API is unavailable so
    // the presenter can fall back to the next backend.
    virtual bool Init(HWND hwnd) = 0;
    // Draw frame into viewport (client coordinates); clear the rest black.
    virtual bool Present(const Frame& f, const Viewport& vp, int clientW, int clientH,
                         const PresentOptions& opt) = 0;
};

std::unique_ptr<Renderer> CreateSoftwareRenderer();
std::unique_ptr<Renderer> CreateD3D11Renderer();
std::unique_ptr<Renderer> CreateD3D10Renderer();
std::unique_ptr<Renderer> CreateD3D12Renderer();
std::unique_ptr<Renderer> CreateOpenGLRenderer();
std::unique_ptr<Renderer> CreateVulkanRenderer();

// Shared shader source (HLSL for D3D10/11/12, GLSL for OpenGL; Vulkan uses
// precompiled SPIR-V generated from the same GLSL at build time).
extern const char* g_hlslSource;
extern const char* g_glslVertex;
extern const char* g_glslPaletteFrag;
extern const char* g_glslScaleFrag;

// Constant block shared by all GPU backends for the scale pass (std140 compatible)
struct ScaleConstants {
    float srcSize[4];   // w, h, 1/w, 1/h of game frame
    float dstSize[4];   // viewport w, h, 1/w, 1/h
    int   filter;       // Filter enum
    float sharpness;
    float pad[2];
};

} // namespace swgbm
