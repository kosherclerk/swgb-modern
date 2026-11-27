#pragma once
#include "../common/common.h"
#include <vector>
#include <string>

namespace swgbm {
class DDSurface;

// Interface text in its own layer: HD text ([Enhancements] HDText) and text
// over the zoomed battlefield (mouse-wheel zoom).
//
// The game draws all interface text with GDI into its back buffer, which it
// then copies 1:1 to the screen. Each text draw is captured as it happens (the
// pixels it wrote, the pixels underneath, and the lines GDI drew: string,
// position, character advances, font, colour). On the way to the screen those
// pixels are put back to the background, and the presenter draws the texts in
// an overlay at the output resolution instead:
//   - HD text: redrawn with the same font scaled to the output resolution,
//     anti-aliased, at the same place (sharp text at any screen size);
//   - zoom without HD text: the game's own pixels, unscaled, anchored to the
//     nearest corner or edge of the visible battlefield, so zooming does not
//     crop or scale them.
// A text lives while the game's buffer still holds the pixels drawn for it;
// when the game erases or overdraws it, it disappears (pixels drawn over it -
// a cursor, a menu - hide that part of it). The overlay also shows the zoom
// level for a moment after it changes.
namespace FloatText {

struct Line {                 // one ExtTextOut call (surface coordinates)
    int x = 0, y = 0;
    UINT opts = 0;
    RECT clip = {};           // valid with ETO_CLIPPED
    std::wstring text;        // characters, or glyph indices with ETO_GLYPH_INDEX
    std::vector<int> adv;     // advance of every character (game pixels)
};
struct Draw {                 // one TextOut / DrawText call
    LOGFONTW font = {};
    UINT align = 0;           // GetTextAlign of the DC
    int charExtra = 0;
    uint32_t rgb = 0;         // 0xRRGGBB (signature only; shown in the palette colour of `ink`)
    int ink = 0;              // palette index of the text's pixels
    uint32_t blend = 0;       // 0xFF000000 | RGB: shown in this colour instead (a text fading in)
    std::vector<Line> lines;
};

void Configure(bool hd);
bool Wanted(const DDSurface* s);   // record text drawn into this surface?
// text drawn into surface s; box in s's pixels, arrays w*h: cov = pixel was
// written by the text, written = value after, bg = value before
void Record(DDSurface* s, int x, int y, int w, int h, const uint8_t* cov, const uint8_t* written, const uint8_t* bg, Draw&& d);
// 1:1 copy src(s) -> dst(d) between surfaces: texts move along. Begin before
// the copy, End after it.
bool TransferBegin(const DDSurface* src, DDSurface* dst, const RECT& d, const RECT& s, bool keyed, int keyLo, int keyHi);
void TransferEnd(DDSurface* dst);
// Screen blit src(s) -> primary(d), 1:1. Prepare decides which texts are
// visible; Patch returns the source row to copy for display row y (texts
// replaced by their background), or the row itself.
bool Prepare(const DDSurface* src, const RECT& d, const RECT& s, int primaryW, int primaryH);
const uint8_t* Patch(int dy, const uint8_t* row, int dx0, int w);
// presenter: (re)draw the overlay (cw x ch, premultiplied BGRA) covering the
// whole frame; returns false when nothing changed; y0..y1 = changed rows
bool Build(std::vector<uint32_t>& px, int cw, int ch, int frameW, int frameH, const uint32_t* palette, int& y0, int& y1, bool& any);
} // namespace FloatText

} // namespace swgbm
