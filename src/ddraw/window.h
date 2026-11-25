#pragma once
#include "../common/common.h"

namespace swgbm {

// Window management + input coordinate translation. The game believes it owns
// a fullscreen display of exactly GameSize; the real window may be any size.
namespace Window {
    void InstallHooks();                // IAT hooks on the game executable
    void Attach(HWND hwnd);             // subclass game window, apply window mode
    void SetGameSize(int w, int h);     // after SetDisplayMode
    void Relayout();                    // apply window mode/size again
    HWND Hwnd();
    void NotifyFlip();
    bool IsActive();
    // Translate between real client coordinates and game coordinates.
    void ClientToGame(int& x, int& y);
    void GameToClient(int& x, int& y);
}

} // namespace swgbm
