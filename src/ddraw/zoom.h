#pragma once
#include "renderer.h"

namespace swgbm {

// Mouse-wheel zoom ([Zoom] Enabled, MaxOut, MaxIn).
//
// The engine draws its battlefield at a fixed scale, so zooming out needs a
// larger battlefield: with zoom on, the game runs at an internal size whose
// battlefield is the display's battlefield divided by the maximum zoom-out,
// while the command panel is laid out at the display width in the bottom-left
// corner. The renderer shows two parts of that frame: the battlefield, cropped
// around its centre (where the engine keeps the camera) and scaled to the
// current zoom, and the panel 1:1 below it. Mouse positions are mapped the same
// way (window.cpp), with the screen edges mapped to the internal edges so
// edge-scrolling keeps working. The wheel moves the zoom between MaxOut and
// MaxIn in StepPct steps, animated; with SnapAt100 it rests at 100% on the way
// through. The zoomed battlefield is drawn with a pixel-art filter (crisp
// texels, 1-pixel anti-aliased edges) at every zoom, so sharpness does not
// change while zooming.
namespace Zoom {
void Init();
bool On();
int DisplayW();
int DisplayH();
int InternalW();     // the size the game runs at (== display size when off)
int InternalH();
int PanelH();        // bottom panel rows (same in both)
// current zoom (1 = original scale, < 1 zoomed out); animated towards the target
float Current();
// wheel: +1 per notch towards zooming in, -1 out; returns true if consumed
bool OnWheel(int notches);
// advance the animation; true while it is still moving
bool Animate();
bool Animating();
double LastChange();   // QPC seconds of the last zoom change (indicator)
// game frame <-> display coordinates (display = the DisplayW x DisplayH image)
void GameToDisplay(float gx, float gy, float& dx, float& dy);
void DisplayToGame(float dx, float dy, int& gx, int& gy);
// source rectangle of the battlefield for the current zoom (game pixels)
void BattlefieldSource(float& x, float& y, float& w, float& h);
// the two regions (battlefield, panel) for a viewport showing the display image
void Regions(const Viewport& vp, int frameW, int frameH, Filter filter, std::vector<Region>& out);
} // namespace Zoom

} // namespace swgbm
