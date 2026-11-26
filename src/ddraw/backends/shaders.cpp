// Shaders shared by the GPU backends.
//
// Pass 1 (palette): indices (R8 uint) + palette (256x1 RGBA8) -> RGBA8 at game res.
// Pass 2 (scale):   RGBA8 game frame -> viewport with the selected filter.
//
// Filters: 0 nearest  (lossless; exact for integer scale factors)
//          1 sharp    (sharp-bilinear: nearest to the integer factor, then AA)
//          2 bilinear
#include "../renderer.h"

namespace swgbm {

const char* g_hlslSource = R"HLSL(
Texture2D<uint>   IndexTex   : register(t0);
Texture2D<float4> PaletteTex : register(t1);
Texture2D<float4> FrameTex   : register(t2);
Texture2D<uint>   ShadeTex   : register(t4);   // colour multiplier, 128 = 1.0
SamplerState LinearSamp      : register(s0);

cbuffer ScaleCB : register(b0) {
    float4 srcSize;   // w,h,1/w,1/h
    float4 dstSize;
    int    filterMode;
    float  sharpness;
    float2 pad;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float4 PSPalette(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    uint idx = IndexTex.Load(int3(p, 0));
    float k = ShadeTex.Load(int3(p, 0)) / 128.0;
    return float4(saturate(PaletteTex.Load(int3(idx, 0, 0)).rgb * k), 1);
}

float3 Px(float2 p) { // fetch texel by integer coordinate with clamping
    int2 q = clamp(int2(p), int2(0, 0), int2(srcSize.xy) - 1);
    return FrameTex.Load(int3(q, 0)).rgb;
}

// Overlay layer (videos, HD text, HD menus): premultiplied BGRA, drawn with
// blend ONE / INV_SRC_ALPHA into the overlay's rectangle.
Texture2D<float4> OverlayTex : register(t3);
float4 PSOverlay(VSOut i) : SV_Target {
    return OverlayTex.SampleLevel(LinearSamp, i.uv, 0);
}

float4 PSScale(VSOut i) : SV_Target {
    float2 uv = i.uv;
    if (filterMode == 0) return float4(Px(floor(uv * srcSize.xy)), 1);
    if (filterMode == 2) return float4(FrameTex.SampleLevel(LinearSamp, uv, 0).rgb, 1);
    // sharp bilinear
    float2 texel = uv * srcSize.xy;
    float2 scale = max(floor(dstSize.xy * srcSize.zw), 1.0);
    float2 fl = floor(texel);
    float2 cd = frac(texel) - 0.5;
    float2 rr = 0.5 - 0.5 / scale;
    float2 f = (cd - clamp(cd, -rr, rr)) * scale + 0.5;
    return float4(FrameTex.SampleLevel(LinearSamp, (fl + f) * srcSize.zw, 0).rgb, 1);
}
)HLSL";

// GLSL: shared by OpenGL (#version 330) and Vulkan (#version 450, compiled to
// SPIR-V at build time). SWGBM_VK is defined for the latter.
const char* g_glslVertex = R"GLSL(
#ifdef SWGBM_VK
layout(location = 0) out vec2 vUV;
#define VID gl_VertexIndex
#else
out vec2 vUV;
#define VID gl_VertexID
#endif
void main() {
    vec2 uv = vec2((VID << 1) & 2, VID & 2);
#ifdef SWGBM_VK
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
#else
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
#endif
    vUV = uv;
}
)GLSL";

const char* g_glslPaletteFrag = R"GLSL(
#ifdef SWGBM_VK
layout(set = 0, binding = 0) uniform usampler2D IndexTex;
layout(set = 0, binding = 1) uniform sampler2D PaletteTex;
layout(set = 0, binding = 2) uniform usampler2D ShadeTex;
layout(location = 0) out vec4 outColor;
#define FRAGPOS ivec2(gl_FragCoord.xy)
#else
uniform usampler2D IndexTex;
uniform sampler2D PaletteTex;
uniform usampler2D ShadeTex;
uniform int frameH;
out vec4 outColor;
#define FRAGPOS ivec2(int(gl_FragCoord.x), frameH - 1 - int(gl_FragCoord.y))
#endif
void main() {
    uint idx = texelFetch(IndexTex, FRAGPOS, 0).r;
    float k = float(texelFetch(ShadeTex, FRAGPOS, 0).r) / 128.0;
    outColor = vec4(clamp(texelFetch(PaletteTex, ivec2(int(idx), 0), 0).rgb * k, 0.0, 1.0), 1.0);
}
)GLSL";

const char* g_glslScaleFrag = R"GLSL(
#ifdef SWGBM_VK
layout(set = 0, binding = 0) uniform sampler2D FrameTex;
layout(push_constant) uniform PC { vec4 srcSize; vec4 dstSize; int filterMode; float sharpness; vec2 pad; } pc;
#define srcSize pc.srcSize
#define dstSize pc.dstSize
#define filterMode pc.filterMode
#define sharpness pc.sharpness
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
#else
uniform sampler2D FrameTex;
uniform vec4 srcSize;
uniform vec4 dstSize;
uniform int filterMode;
uniform float sharpness;
in vec2 vUV;
out vec4 outColor;
#endif

vec3 Px(vec2 p) {
    ivec2 q = clamp(ivec2(p), ivec2(0), ivec2(srcSize.xy) - 1);
#ifdef SWGBM_VK
    return texelFetch(FrameTex, q, 0).rgb;
#else
    // GL frame texture is stored bottom-up (rendered by pass 1)
    return texelFetch(FrameTex, ivec2(q.x, int(srcSize.y) - 1 - q.y), 0).rgb;
#endif
}
vec3 Lin(vec2 uv) {
#ifdef SWGBM_VK
    return textureLod(FrameTex, uv, 0.0).rgb;
#else
    return textureLod(FrameTex, vec2(uv.x, 1.0 - uv.y), 0.0).rgb;
#endif
}


void main() {
    vec2 uv = vUV;
    if (filterMode == 0) { outColor = vec4(Px(floor(uv * srcSize.xy)), 1.0); return; }
    if (filterMode == 2) { outColor = vec4(Lin(uv), 1.0); return; }
    vec2 texel = uv * srcSize.xy;
    vec2 scale = max(floor(dstSize.xy * srcSize.zw), vec2(1.0));
    vec2 fl = floor(texel);
    vec2 cd = fract(texel) - 0.5;
    vec2 rr = 0.5 - 0.5 / scale;
    vec2 f = (cd - clamp(cd, -rr, rr)) * scale + 0.5;
    outColor = vec4(Lin((fl + f) * srcSize.zw), 1.0);
}
)GLSL";

const char* g_glslOverlayFrag = R"GLSL(
#ifdef SWGBM_VK
layout(set = 0, binding = 0) uniform sampler2D OverlayTex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
#else
uniform sampler2D OverlayTex;
in vec2 vUV;
out vec4 outColor;
#endif
void main() { outColor = textureLod(OverlayTex, vUV, 0.0); }
)GLSL";

} // namespace swgbm
