#pragma once
#include "renderer.h"

namespace swgbm {

// Diagnostics ([Debug] Diagnostics=1).
//
// Capture: the presenter hands every shown frame (8-bit indices + palette) to
// a recorder that keeps the last seconds in memory (XOR delta against the
// previous frame, XPRESS-compressed, a full keyframe every second). The
// capture hotkey (CaptureKey, default ctrl+shift+f12) saves, CaptureSeconds before
// and after the key press, into Game\SWGBModern\captures\<date_time>\:
//   screenshot.png   the frame shown when the key was pressed
//   clip.avi         Motion-JPEG video (plays in VLC, browsers, editors)
//   frames\*.png     4 stills per second
//   summary.txt      settings, per-second stats and the log around the press
// Stats: one "diag" log line per second (frame rates, longest frame gap,
// presenter time, CPU, memory) and a "stutter" line when the game's own
// frame loop stalls.
namespace Capture {
void Init();
bool Enabled();
// presenter thread, after every snapshot
void OnFrame(const Frame& f);
// presenter thread: hotkey polling and the per-second stats line
void Tick(HWND hwnd, int presented, int newContent, double presentMs);
// game thread: a full frame was handed to the screen (frame pacing / stutters)
void OnGameFrame();
// window thread: true for the capture hotkey's key messages, which the game
// must not see (e.g. ctrl+f12 is its own "Full Map Print")
bool IsHotkeyMessage(UINT msg, WPARAM wp);
} // namespace Capture

} // namespace swgbm
