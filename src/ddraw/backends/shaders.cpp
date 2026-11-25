// Shaders shared by the GPU backends.
//
// Pass 1 (palette): indices (R8 uint) + palette (256x1 RGBA8) -> RGBA8 at game res.
// Pass 2 (scale):   RGBA8 game frame -> viewport with the selected filter.
//
// Filters: 0 nearest  (lossless; exact for integer scale factors)
//          1 sharp    (sharp-bilinear: nearest to the integer factor, then AA)
//          2 bilinear
//          3 xBR-lv2  (edge-directed pixel-art upscaler, scale independent;
//                      algorithm by Hyllian, MIT licensed, re-implemented here)
#include "../renderer.h"

namespace swgbm {

const char* g_hlslSource = R"HLSL(
Texture2D<uint>   IndexTex   : register(t0);
Texture2D<float4> PaletteTex : register(t1);
Texture2D<float4> FrameTex   : register(t2);
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
    uint idx = IndexTex.Load(int3(int2(i.pos.xy), 0));
    return float4(PaletteTex.Load(int3(idx, 0, 0)).rgb, 1);
}

float3 Px(float2 p) { // fetch texel by integer coordinate with clamping
    int2 q = clamp(int2(p), int2(0, 0), int2(srcSize.xy) - 1);
    return FrameTex.Load(int3(q, 0)).rgb;
}

static const float3 Yw = float3(14.352, 28.176, 5.472);
float4 df(float4 A, float4 B) { return abs(A - B); }
float  c_df(float3 a, float3 b) { float3 d = abs(a - b); return d.r + d.g + d.b; }
bool4  eq(float4 A, float4 B) { return df(A, B) < 15.0; }
bool4  neq(float4 A, float4 B) { return !eq(A, B); }
float4 wdist(float4 a, float4 b, float4 c, float4 d, float4 e, float4 f, float4 g, float4 h) {
    return df(a,b) + df(a,c) + df(d,e) + df(d,f) + 4.0 * df(g,h);
}
float4 L4(float3 a, float3 b, float3 c, float3 d) { return float4(dot(a,Yw), dot(b,Yw), dot(c,Yw), dot(d,Yw)); }

float3 XBR(float2 uv) {
    float2 tc = uv * srcSize.xy;
    float2 fp = frac(tc);
    float2 e0 = floor(tc);
    float3 A1=Px(e0+float2(-1,-2)), B1=Px(e0+float2(0,-2)), C1=Px(e0+float2(1,-2));
    float3 A0=Px(e0+float2(-2,-1)), A=Px(e0+float2(-1,-1)), B=Px(e0+float2(0,-1)), C=Px(e0+float2(1,-1)), C4=Px(e0+float2(2,-1));
    float3 D0=Px(e0+float2(-2,0)),  D=Px(e0+float2(-1,0)),  E=Px(e0),             F=Px(e0+float2(1,0)),  F4=Px(e0+float2(2,0));
    float3 G0=Px(e0+float2(-2,1)),  G=Px(e0+float2(-1,1)),  H=Px(e0+float2(0,1)),  I=Px(e0+float2(1,1)),  I4=Px(e0+float2(2,1));
    float3 G5=Px(e0+float2(-1,2)), H5=Px(e0+float2(0,2)), I5=Px(e0+float2(1,2));

    float4 b = L4(B,D,H,F), c = L4(C,A,G,I), e = L4(E,E,E,E);
    float4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
    float4 i4 = L4(I4,C1,A0,G5), i5 = L4(I5,C4,A1,G0), h5 = L4(H5,F4,B1,D0);
    float4 f4 = h5.yzwx;

    const float4 Ao = float4(1,-1,-1,1), Bo = float4(1,1,-1,-1), Co = float4(1.5,0.5,-0.5,0.5);
    const float4 Ax = float4(1,-1,-1,1), Bx = float4(0.5,2,-0.5,-2), Cx = float4(1,1,-0.5,0);
    const float4 Ay = float4(1,-1,-1,1), By = float4(2,0.5,-2,-0.5), Cy = float4(2,0,-1,0.5);
    const float4 Ci = float4(0.25,0.25,0.25,0.25);
    float scale = max(dstSize.x * srcSize.z, 1.0);
    float4 delta = float4(1,1,1,1) * clamp(1.0 / scale, 0.08, 0.5) * lerp(1.5, 0.5, sharpness);
    float4 deltaL = float4(0.5, 1.0, 0.5, 1.0) * delta.x;
    float4 deltaU = float4(1.0, 0.5, 1.0, 0.5) * delta.x;

    float4 fx      = Ao * fp.y + Bo * fp.x;
    float4 fx_left = Ax * fp.y + Bx * fp.x;
    float4 fx_up   = Ay * fp.y + By * fp.x;

    bool4 lv0 = neq(e,f) && neq(e,h);
    bool4 lv1 = lv0 && ((neq(f,b) && neq(f,c)) || (neq(h,d) && neq(h,g)) ||
                (eq(e,i) && ((neq(f,f4) && neq(f,i4)) || (neq(h,h5) && neq(h,i5)))) || eq(e,g) || eq(e,c));
    bool4 lv2l = neq(e,g) && neq(d,g);
    bool4 lv2u = neq(e,c) && neq(b,c);

    float4 fx45i = saturate((fx + delta - Co - Ci) / (2 * delta));
    float4 fx45  = saturate((fx + delta - Co) / (2 * delta));
    float4 fx30  = saturate((fx_left + deltaL - Cx) / (2 * deltaL));
    float4 fx60  = saturate((fx_up + deltaU - Cy) / (2 * deltaU));

    float4 wd1 = wdist(e, c, g, i, h5, f4, h, f);
    float4 wd2 = wdist(h, d, i5, f, i4, b, e, i);
    bool4 edri  = (wd1 <= wd2) && lv0;
    bool4 edr   = (wd1 <  wd2) && lv1;
    bool4 edrl  = ((2.0 * df(f,g)) <= df(h,c)) && lv2l && edr;
    bool4 edru  = (df(f,g) >= (2.0 * df(h,c))) && lv2u && edr;

    fx45 *= (float4)edr; fx30 *= (float4)edrl; fx60 *= (float4)edru; fx45i *= (float4)edri;
    bool4 px = df(e,f) <= df(e,h);
    float4 mx = max(max(fx30, fx60), max(fx45, fx45i));

    float3 res1 = E;
    res1 = lerp(res1, px.x ? F : H, mx.x);
    res1 = lerp(res1, px.z ? D : B, mx.z);
    float3 res2 = E;
    res2 = lerp(res2, px.y ? B : F, mx.y);
    res2 = lerp(res2, px.w ? H : D, mx.w);
    return lerp(res1, res2, step(c_df(E, res1), c_df(E, res2)));
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
    if (filterMode == 3) return float4(XBR(uv), 1);
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
layout(location = 0) out vec4 outColor;
#define FRAGPOS ivec2(gl_FragCoord.xy)
#else
uniform usampler2D IndexTex;
uniform sampler2D PaletteTex;
uniform int frameH;
out vec4 outColor;
#define FRAGPOS ivec2(int(gl_FragCoord.x), frameH - 1 - int(gl_FragCoord.y))
#endif
void main() {
    uint idx = texelFetch(IndexTex, FRAGPOS, 0).r;
    outColor = vec4(texelFetch(PaletteTex, ivec2(int(idx), 0), 0).rgb, 1.0);
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

const vec3 Yw = vec3(14.352, 28.176, 5.472);
vec4 df(vec4 A, vec4 B) { return abs(A - B); }
float c_df(vec3 a, vec3 b) { vec3 d = abs(a - b); return d.r + d.g + d.b; }
bvec4 eq(vec4 A, vec4 B) { return lessThan(df(A, B), vec4(15.0)); }
bvec4 neq(vec4 A, vec4 B) { return not(eq(A, B)); }
bvec4 band(bvec4 a, bvec4 b) { return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }
bvec4 bor(bvec4 a, bvec4 b) { return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }
vec4 wdist(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) {
    return df(a,b) + df(a,c) + df(d,e) + df(d,f) + 4.0 * df(g,h);
}
vec4 L4(vec3 a, vec3 b, vec3 c, vec3 d) { return vec4(dot(a,Yw), dot(b,Yw), dot(c,Yw), dot(d,Yw)); }

vec3 XBR(vec2 uv) {
    vec2 tc = uv * srcSize.xy;
    vec2 fp = fract(tc);
    vec2 e0 = floor(tc);
    vec3 A1=Px(e0+vec2(-1,-2)), B1=Px(e0+vec2(0,-2)), C1=Px(e0+vec2(1,-2));
    vec3 A0=Px(e0+vec2(-2,-1)), A=Px(e0+vec2(-1,-1)), B=Px(e0+vec2(0,-1)), C=Px(e0+vec2(1,-1)), C4=Px(e0+vec2(2,-1));
    vec3 D0=Px(e0+vec2(-2,0)),  D=Px(e0+vec2(-1,0)),  E=Px(e0),           F=Px(e0+vec2(1,0)),  F4=Px(e0+vec2(2,0));
    vec3 G0=Px(e0+vec2(-2,1)),  G=Px(e0+vec2(-1,1)),  H=Px(e0+vec2(0,1)),  I=Px(e0+vec2(1,1)),  I4=Px(e0+vec2(2,1));
    vec3 G5=Px(e0+vec2(-1,2)), H5=Px(e0+vec2(0,2)), I5=Px(e0+vec2(1,2));

    vec4 b = L4(B,D,H,F), c = L4(C,A,G,I), e = L4(E,E,E,E);
    vec4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
    vec4 i4 = L4(I4,C1,A0,G5), i5 = L4(I5,C4,A1,G0), h5 = L4(H5,F4,B1,D0);
    vec4 f4 = h5.yzwx;

    const vec4 Ao = vec4(1,-1,-1,1), Bo = vec4(1,1,-1,-1), Co = vec4(1.5,0.5,-0.5,0.5);
    const vec4 Ax = vec4(1,-1,-1,1), Bx = vec4(0.5,2,-0.5,-2), Cx = vec4(1,1,-0.5,0);
    const vec4 Ay = vec4(1,-1,-1,1), By = vec4(2,0.5,-2,-0.5), Cy = vec4(2,0,-1,0.5);
    const vec4 Ci = vec4(0.25);
    float scale = max(dstSize.x * srcSize.z, 1.0);
    vec4 delta = vec4(clamp(1.0 / scale, 0.08, 0.5) * mix(1.5, 0.5, sharpness));
    vec4 deltaL = vec4(0.5, 1.0, 0.5, 1.0) * delta.x;
    vec4 deltaU = vec4(1.0, 0.5, 1.0, 0.5) * delta.x;

    vec4 fx      = Ao * fp.y + Bo * fp.x;
    vec4 fx_left = Ax * fp.y + Bx * fp.x;
    vec4 fx_up   = Ay * fp.y + By * fp.x;

    bvec4 lv0 = band(neq(e,f), neq(e,h));
    bvec4 lv1 = band(lv0, bor(bor(bor(band(neq(f,b), neq(f,c)), band(neq(h,d), neq(h,g))),
                 band(eq(e,i), bor(band(neq(f,f4), neq(f,i4)), band(neq(h,h5), neq(h,i5))))), bor(eq(e,g), eq(e,c))));
    bvec4 lv2l = band(neq(e,g), neq(d,g));
    bvec4 lv2u = band(neq(e,c), neq(b,c));

    vec4 fx45i = clamp((fx + delta - Co - Ci) / (2.0 * delta), 0.0, 1.0);
    vec4 fx45  = clamp((fx + delta - Co) / (2.0 * delta), 0.0, 1.0);
    vec4 fx30  = clamp((fx_left + deltaL - Cx) / (2.0 * deltaL), 0.0, 1.0);
    vec4 fx60  = clamp((fx_up + deltaU - Cy) / (2.0 * deltaU), 0.0, 1.0);

    vec4 wd1 = wdist(e, c, g, i, h5, f4, h, f);
    vec4 wd2 = wdist(h, d, i5, f, i4, b, e, i);
    bvec4 edri = band(lessThanEqual(wd1, wd2), lv0);
    bvec4 edr  = band(lessThan(wd1, wd2), lv1);
    bvec4 edrl = band(band(lessThanEqual(2.0 * df(f,g), df(h,c)), lv2l), edr);
    bvec4 edru = band(band(greaterThanEqual(df(f,g), 2.0 * df(h,c)), lv2u), edr);

    fx45 *= vec4(edr); fx30 *= vec4(edrl); fx60 *= vec4(edru); fx45i *= vec4(edri);
    bvec4 px = lessThanEqual(df(e,f), df(e,h));
    vec4 mx = max(max(fx30, fx60), max(fx45, fx45i));

    vec3 res1 = E;
    res1 = mix(res1, px.x ? F : H, mx.x);
    res1 = mix(res1, px.z ? D : B, mx.z);
    vec3 res2 = E;
    res2 = mix(res2, px.y ? B : F, mx.y);
    res2 = mix(res2, px.w ? H : D, mx.w);
    return mix(res1, res2, step(c_df(E, res1), c_df(E, res2)));
}

void main() {
    vec2 uv = vUV;
    if (filterMode == 0) { outColor = vec4(Px(floor(uv * srcSize.xy)), 1.0); return; }
    if (filterMode == 2) { outColor = vec4(Lin(uv), 1.0); return; }
    if (filterMode == 3) { outColor = vec4(XBR(uv), 1.0); return; }
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
