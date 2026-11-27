#pragma once
#include "../common/common.h"
#include <vector>

namespace swgbm {
class DDSurface;

// Interface text over the zoomed battlefield (mouse-wheel zoom).
//
// The game draws hints, mission text, chat and scores into its screen buffer at
// the edges of its (larger than the screen) battlefield, where zooming would
// crop them. Each such text draw is recorded (coverage, colour, the pixels
// written and the pixels underneath). On the way to the display those pixels
// are put back to the background, and the presenter draws the texts unscaled in
// an overlay, anchored to the nearest corner or edge of the visible
// battlefield. A text lives while the game's buffer still holds exactly the
// pixels drawn for it; when the game erases or overdraws it, it disappears.
// The same overlay shows the zoom level for a moment after it changes.
namespace FloatText {
// text drawn into surface s (TextLayer); box in s's pixels, arrays w*h
void Record(DDSurface* s, int x, int y, int w, int h, const uint8_t* cov, const uint8_t* written, const uint8_t* bg, uint32_t rgb);
// screen buffer -> display copy just made: drop dead texts, put the
// background back under live ones in the display copy; adds touched rows
void Apply(const DDSurface* screen, uint8_t* display, int pitch, std::vector<int>& rows);
// presenter: (re)draw the overlay (DisplayW x DisplayH, premultiplied BGRA);
// returns false when nothing changed since the last call; y0..y1 = changed rows
bool Build(std::vector<uint32_t>& px, int& y0, int& y1, bool& any);
} // namespace FloatText

} // namespace swgbm
