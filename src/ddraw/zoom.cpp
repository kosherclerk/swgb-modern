// Mouse-wheel zoom geometry (see zoom.h).
#include "zoom.h"
#include <algorithm>
#include <math.h>

namespace swgbm {
namespace {
bool g_on = false;
int g_dw = 0, g_dh = 0;            // display (what the player sees, in game pixels)
int g_iw = 0, g_ih = 0;            // internal (what the game renders)
int g_bw = 0, g_bh = 0;            // internal battlefield
const int kPanel = 250;            // bottom panel height (1280x250 panel art)
float g_zMin = 1, g_zMax = 1;
float g_step = 1.1f;               // zoom factor per wheel notch
bool g_detent = true;              // stop at 100% on the way through
double g_holdUntil = 0;            // detent: notches before this time are ignored
// The zoom is animated in log space (every notch looks the same size) with
// time-based easing, independent of the frame rate.
volatile float g_z = 1, g_target = 1;
double g_lastAnim = 0;
double g_lastChange = -100;

double Seconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}
} // namespace

void Zoom::Init() {
    g_dw = CfgInt("Display", "GameWidth", 0);
    g_dh = CfgInt("Display", "GameHeight", 0);
    g_iw = g_dw; g_ih = g_dh;
    bool widescreen = CfgBool("Fixes", "Widescreen", true) && g_dw >= 800 && g_dh >= 600 && !(g_dw == 1280 && g_dh == 1024);
    g_on = widescreen && CfgBool("Zoom", "Enabled", false);
    if (!g_on) return;
    g_zMin = std::max(40, std::min(100, CfgInt("Zoom", "MaxOut", 70))) / 100.0f;
    g_zMax = std::max(100, std::min(200, CfgInt("Zoom", "MaxIn", 125))) / 100.0f;
    g_step = 1.0f + std::max(3, std::min(30, CfgInt("Zoom", "StepPct", 10))) / 100.0f;
    g_detent = CfgBool("Zoom", "SnapAt100", true);
    if (g_zMin >= 1.0f && g_zMax <= 1.0f) { g_on = false; return; }
    int bh = g_dh - kPanel;
    g_bw = std::max(g_dw, ((int)(g_dw / g_zMin + 0.5f) + 3) & ~3);
    g_bh = std::max(bh, ((int)(bh / g_zMin + 0.5f) + 1) & ~1);
    g_iw = g_bw;
    g_ih = g_bh + kPanel;
    LOG("zoom: display %dx%d, game renders %dx%d (battlefield %dx%d), zoom %.0f%%-%.0f%%", g_dw, g_dh, g_iw, g_ih, g_bw, g_bh,
        g_zMin * 100, g_zMax * 100);
}

bool Zoom::On() { return g_on; }
int Zoom::DisplayW() { return g_dw; }
int Zoom::DisplayH() { return g_dh; }
int Zoom::InternalW() { return g_iw; }
int Zoom::InternalH() { return g_ih; }
int Zoom::PanelH() { return kPanel; }
float Zoom::Current() { return g_on ? g_z : 1.0f; }

bool Zoom::OnWheel(int notches) {
    if (!g_on || !notches) return false;
    double now = Seconds();
    if (now < g_holdUntil) return true;     // resting at 100%
    float from = g_target;
    float t = from * powf(g_step, (float)notches);
    if (fabsf(t - 1.0f) < (g_step - 1.0f) * 0.4f) t = 1.0f;   // close to 100%: land on it
    // detent: passing through 100% stops there; the next notch (after a short
    // rest) continues
    if (g_detent && from != 1.0f && (from - 1.0f) * (t - 1.0f) < 0) t = 1.0f;
    if (g_detent && t == 1.0f && from != 1.0f) g_holdUntil = now + 0.25;
    float nt = std::max(g_zMin, std::min(g_zMax, t));
    if (nt != g_target || notches) g_lastChange = now;
    g_target = nt;
    return true;
}

bool Zoom::Animate() {
    if (!g_on) return false;
    double now = Seconds();
    double dt = g_lastAnim > 0 ? std::min(0.1, now - g_lastAnim) : 0.016;
    g_lastAnim = now;
    float z = g_z, t = g_target;
    if (z == t) return false;
    // exponential approach in log space, ~95% of the way in 0.2 s
    float lz = logf(z), lt = logf(t);
    lz += (lt - lz) * (float)(1.0 - exp(-dt / 0.065));
    z = expf(lz);
    if (fabsf(z - t) < 0.0015f) z = t;
    g_z = z;
    return true;
}

double Zoom::LastChange() { return g_lastChange; }
bool Zoom::Animating() { return g_on && g_z != g_target; }

void Zoom::BattlefieldSource(float& x, float& y, float& w, float& h) {
    float z = Current();
    w = g_dw / z;
    h = (g_dh - kPanel) / z;
    x = (g_bw - w) * 0.5f;
    y = (g_bh - h) * 0.5f;
}

void Zoom::Regions(const Viewport& vp, int frameW, int frameH, Filter filter, std::vector<Region>& out) {
    out.clear();
    if (!g_on || vp.w <= 0 || vp.h <= 0 || frameW <= 0 || frameH <= 0) return;
    const float s = (float)vp.h / g_dh;
    const int split = vp.y + (int)((g_dh - kPanel) * s + 0.5f);
    float bx, by, bw, bh;
    BattlefieldSource(bx, by, bw, bh);
    Region field, panel;
    field.x = vp.x; field.y = vp.y; field.w = vp.w; field.h = split - vp.y;
    field.u0 = bx / frameW; field.v0 = by / frameH;
    field.u1 = (bx + bw) / frameW; field.v1 = (by + bh) / frameH;
    // one look at every zoom: the player's filter at exactly 100%, pixel art
    // (the same at integer scales, crisp in between) otherwise
    field.filter = filter == Filter::Bilinear ? Filter::Bilinear : Current() == 1.0f ? filter : Filter::PixelArt;
    panel.x = vp.x; panel.y = split; panel.w = vp.w; panel.h = vp.y + vp.h - split;
    panel.u0 = 0; panel.v0 = (float)(frameH - kPanel) / frameH;
    panel.u1 = (float)g_dw / frameW; panel.v1 = 1;
    panel.filter = filter;
    out.push_back(field);
    out.push_back(panel);
}

void Zoom::GameToDisplay(float gx, float gy, float& dx, float& dy) {
    if (!g_on) { dx = gx; dy = gy; return; }
    if (gy >= g_bh) { dx = gx; dy = gy - g_bh + (g_dh - kPanel); return; }
    float x, y, w, h;
    BattlefieldSource(x, y, w, h);
    float z = Current();
    dx = (gx - x) * z;
    dy = (gy - y) * z;
}

void Zoom::DisplayToGame(float dx, float dy, int& gx, int& gy) {
    if (!g_on) { gx = (int)dx; gy = (int)dy; return; }
    int bh = g_dh - kPanel;
    if (dy >= bh) {   // panel: 1:1, the screen's right and bottom edges are the game's edges
        gx = dx >= g_dw - 1 ? g_iw - 1 : (int)dx;
        gy = (int)(dy - bh) + g_bh;
        if (dy >= g_dh - 1) gy = g_ih - 1;
        gx = std::max(0, std::min(g_iw - 1, gx)); gy = std::max(0, std::min(g_ih - 1, gy));
        return;
    }
    float x, y, w, h;
    BattlefieldSource(x, y, w, h);
    float z = Current();
    gx = (int)floorf(x + dx / z);
    gy = (int)floorf(y + dy / z);
    // the screen's edges are the game's edges (edge scrolling)
    if (dx <= 0) gx = 0;
    if (dx >= g_dw - 1) gx = g_iw - 1;
    if (dy <= 0) gy = 0;
    gx = std::max(0, std::min(g_iw - 1, gx)); gy = std::max(0, std::min(g_ih - 1, gy));
}

} // namespace swgbm
