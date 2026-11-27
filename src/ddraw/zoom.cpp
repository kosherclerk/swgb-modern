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
volatile float g_z = 1, g_target = 1;
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
    float t = g_target * powf(1.1f, (float)notches);
    if (fabsf(t - 1.0f) < 0.04f) t = 1.0f;   // snap to the original scale on the way through
    g_target = std::max(g_zMin, std::min(g_zMax, t));
    return true;
}

bool Zoom::Animate() {
    if (!g_on) return false;
    float z = g_z, t = g_target;
    if (z == t) return false;
    z += (t - z) * 0.3f;
    if (fabsf(z - t) < 0.002f) z = t;
    g_z = z;
    return true;
}

void Zoom::BattlefieldSource(float& x, float& y, float& w, float& h) {
    float z = Current();
    w = g_dw / z;
    h = (g_dh - kPanel) / z;
    x = (g_bw - w) * 0.5f;
    y = (g_bh - h) * 0.5f;
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
