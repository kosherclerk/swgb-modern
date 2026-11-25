#pragma once
#include "renderer.h"

namespace swgbm {

// Owns the backend and a dedicated presentation thread. The game thread only
// signals that the front buffer changed; Flip() additionally waits until the
// presenter has picked up the frame, which paces the game loop to the display
// (or to MaxFPS) without touching the simulation clock: the Genie engine
// advances the world by elapsed timeGetTime() * game speed, so fewer or more
// frames never change game speed.
class Presenter {
public:
    static Presenter& Get();
    void Start(HWND hwnd);
    void Stop();
    void FrameReady(bool flip);
    Viewport CurrentViewport();      // client-space rect the game image occupies
    void GameSize(int& w, int& h);
    const char* BackendName();
    // Overlay layer (videos, HD text/menus). Thread safe: the pixels are copied.
    // rect is in game-frame coordinates. Rows [y0,y1) changed (-1 = all).
    void SetOverlay(const uint32_t* bgraPremul, int w, int h, float x, float y, float rw, float rh, bool hideFrame,
                    int y0 = -1, int y1 = -1);
    void ClearOverlay();

private:
    static DWORD WINAPI ThreadMain(void*);
    void Run();
    std::unique_ptr<Renderer> CreateBackend();
    Viewport ComputeViewport(int cw, int ch, int gw, int gh);
    void LoadOptions(PresentOptions& opt, int& maxFps);
    volatile LONG scaleMode_ = 0;        // ScaleMode, reloaded live
    std::string rendererName_;

    HWND hwnd_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE frameEvent_ = nullptr;     // game -> presenter: new content
    HANDLE consumedEvent_ = nullptr;  // presenter -> game: snapshot taken
    volatile LONG running_ = 0;
    volatile LONG pendingFlip_ = 0;
    CRITICAL_SECTION vpLock_;
    CRITICAL_SECTION ovLock_;
    Overlay ovShared_;        // written by producers under ovLock_
    bool ovChanged_ = false;
    Overlay ovRender_;        // presenter-thread copy handed to the backend
    Viewport vp_ = {0, 0, 800, 600};
    int gameW_ = 800, gameH_ = 600;
    const char* backendName_ = "none";
    bool limitOnFlip_ = true;
};

} // namespace swgbm
