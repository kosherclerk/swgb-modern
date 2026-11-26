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

enum class Filter { Nearest = 0, Sharp = 1, Bilinear = 2 };
enum class ScaleMode { Aspect = 0, Integer = 1, Stretch = 2 };

struct Viewport { int x, y, w, h; };

struct Frame {
    int width = 0, height = 0;          // game resolution
    std::vector<uint8_t> pixels;        // width*height indices, tightly packed
    uint32_t palette[256];              // 0x00RRGGBB entries
    // Rows [dirtyY0, dirtyY1) changed since the previous Present(); backends
    // upload only those (empty range = indices unchanged). paletteChanged set
    // when the palette differs. Both are "everything" after a size change.
    int dirtyY0 = 0, dirtyY1 = 0;
    bool paletteChanged = true;
    // Optional per-pixel colour multiplier applied after the palette lookup
    // (graphics enhancements: smooth fog, shadows, lighting). value/128, so 128
    // is exactly 1.0. Empty = none (the image stays bit-exact). Uploaded for the
    // same dirty rows as the pixels.
    std::vector<uint8_t> shade;
    bool Dirty() const { return dirtyY1 > dirtyY0 || paletteChanged; }
    void MarkAll() { dirtyY0 = 0; dirtyY1 = height; paletteChanged = true; }
};

struct PresentOptions {
    Filter filter = Filter::Nearest;
    bool vsync = true;
    float sharpness = 0.5f;             // unused (kept for the shader constant layout)
};

// RGBA layer composited above the game image (intro videos, HD text, HD menu
// backgrounds). Pixels are premultiplied BGRA, width*height, top-down.
struct Overlay {
    bool visible = false;
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;
    int dirtyY0 = 0, dirtyY1 = 0;      // rows to upload (like Frame)
    // Destination in game-frame coordinates (scaled with the frame); a
    // fullscreen video uses {0,0,frame w,frame h}.
    float x = 0, y = 0, w = 0, h = 0;
    bool hideFrame = false;            // draw black instead of the game image (videos)
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
    // Optional: block until the swapchain can accept another frame (low
    // latency: the presenter then snapshots the freshest game frame).
    virtual void WaitForNextFrame() {}
    // Whether identical frames may be skipped (some drivers behave better
    // with a steady present cadence).
    virtual bool SkipIdenticalFrames() const { return true; }
    // Optional overlay; drawn after the game image in Present(). Backends that
    // do not override it simply show no overlay.
    virtual void SetOverlay(const Overlay* o) { overlay_ = o; }

protected:
    const Overlay* overlay_ = nullptr;
    // Shade plane bookkeeping shared by the backends: which rows of the shade
    // texture need uploading, and from where (a neutral 128 buffer when the
    // frame has no shade but the texture still holds an old one).
    bool shadeActive_ = false;
    std::vector<uint8_t> neutralShade_;
    bool ShadeUpload(const Frame& f, bool textureNew, int& y0, int& y1, const uint8_t*& data) {
        size_t n = (size_t)f.width * f.height;
        if (!f.shade.empty() && f.shade.size() == n) {
            bool full = textureNew || !shadeActive_;
            shadeActive_ = true;
            y0 = full ? 0 : f.dirtyY0; y1 = full ? f.height : f.dirtyY1;
            data = f.shade.data();
            return y1 > y0;
        }
        if (!shadeActive_ && !textureNew) return false;
        shadeActive_ = false;
        if (neutralShade_.size() != n) neutralShade_.assign(n, 128);
        y0 = 0; y1 = f.height; data = neutralShade_.data();
        return true;
    }
    // Overlay rect in client coordinates for a viewport.
    static void OverlayRect(const Overlay& o, const Frame& f, const Viewport& vp, float& x, float& y, float& w, float& h) {
        float sx = (float)vp.w / f.width, sy = (float)vp.h / f.height;
        x = vp.x + o.x * sx; y = vp.y + o.y * sy; w = o.w * sx; h = o.h * sy;
    }
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
extern const char* g_glslOverlayFrag;

// Constant block shared by all GPU backends for the scale pass (std140 compatible)
struct ScaleConstants {
    float srcSize[4];   // w, h, 1/w, 1/h of game frame
    float dstSize[4];   // viewport w, h, 1/w, 1/h
    int   filter;       // Filter enum
    float sharpness;
    float pad[2];
};

} // namespace swgbm
