# Expanding Fronts 1.5.5 vs. stock Clone Campaigns, and what SWGB Modern takes from it

## How EF is built

| Piece | What it is |
|---|---|
| `battlegrounds_x2.exe` | The CC 1.1 executable (same 2002-05-30 link stamp) with about 100 bytes of static code edits, renamed data paths (`*_x2.drs`, `genie_x2.dat`, registry key "Expanding Fronts") and a new `.patch` section that loads `efpatch.dll` at startup. |
| `efpatch.dll` (2025, MSVC 2015+ runtime) | Applies all EF engine changes in memory: resolution patch, 60 FPS, DirectWrite text, keydown hotkeys, fog patterns, large maps, editor unlocks, crash fixes. Settings live in `HKCU\…\Expanding Fronts\Configurator`. |
| `configurator.exe` (3 KB) | A stub that opens efpatch's settings dialog. Holding SHIFT at launch shows a "Select Game Version" prompt (CC or EF). |
| `wndmode.dll` + `wndmode.ini` | A third-party DirectDraw window-mode hook, used when "Window Mode" is enabled. |
| `widescrn_x2.drs` | Interface art regenerated for widescreen on first run (that's why EF needs write access and free disk space). |
| `crashrpt.dll`, `sendrpt.exe`, `dbghelp.dll` | The CrashRpt / Doctor Dump uploader. |
| Data (`*_x2.drs`, `genie_x2.dat`, AI, campaigns) | EF's content: new civs, units, art, sounds, maps, AI. This is EF's own work and isn't reused by SWGB Modern. |

## Measured on this machine (4K monitor at 150%, RX 9070 XT)

| | Stock CC | EF 1.5.5 as shipped | CC + SWGB Modern | EF + SWGB Modern |
|---|---|---|---|---|
| Display | Exclusive 800×600 mode switch | Exclusive mode switch (desktop changes resolution) | Borderless 4K, integer scaled | Borderless 4K, scaled |
| In-game resolution | 1280×1024 max | EF widescreen (1366×768 "Auto" here) | Real 1920×1080, centred stretched panel | EF widescreen |
| CPU in menus | ~100% of one core (busy loop) | ~80% of a core (33 s CPU in ~40 s) | ~3% | ~3% |
| Renderer | DirectDraw 8-bit (palette bugs) | DirectDraw (or wndmode GDI) | D3D10/11/12, OpenGL, Vulkan, Software | same |
| Volume sliders | Change Windows volume | Optional "Delink" (still loud) | Isolated virtual mixer | Isolated virtual mixer* |
| World redraw | 20 fps timer, 10 Hz view, 3 Hz minimap | 60 FPS option | 60 fps timer, 60 Hz view, 10 Hz minimap | EF's 60 FPS option |

\* `efpatch.dll` loads after our DLL and may install its own mixer hooks. Whichever installs last wins; both avoid touching the system volume.

## EF ideas adopted (reimplemented independently, no EF code or assets)

1. **60 FPS rendering.** The engine redraws the world from a 50 ms `WM_TIMER` (`SetTimer(hwnd, 1, 50)` at `0x4251c5`), and on top of that the game screen throttles the main view to 100 ms (`0x4f2fd7`) and the minimap to 333 ms (`0x4f2f17`). SWGB Modern retargets all three (`[Fixes] GameFPS=60`, `MinimapRefreshMs=100`). Measured: distinct frames with units moving went from about 13/s to about 22/s (the rest is limited by the simulation turn rate). The in-game clock still advances at the same 1.5× "Normal" speed at 15, 20 and 60 fps.
2. **Widescreen panel layout.** EF keeps the unit grid anchored left, the minimap and buttons anchored right, and widens the centre info panel. SWGB Modern now does the same at runtime: it rewrites the "Resolution Specific Rects" table (resource 53290) and generates wider panel SLPs in memory. Nothing is written to disk, so there's no "first run" step and no permission problems. `[Widescreen] PanelStyle=stretch|center`.
3. **Auto resolution.** The launcher will default the game resolution from the monitor and the chosen integer scale.
4. **Crash reports.** These are local minidumps in `Game\CrashDumps` instead of uploads, and are disabled under EF, which has its own reporter.
5. **Volume delink.** This is done more thoroughly than EF's option: a virtual mixer that routes the CD-music slider to the ogg player and the wave slider to the new audio engine.

## EF problems that SWGB Modern removes outright

These are from EF's own troubleshooting section:

- palette corruption (red/green pixels)
- flickering text boxes in fullscreen
- fog-of-war "interlace" artifacts from non-integer scaling
- stutter from GDI text on DirectDraw surfaces (our surfaces are system-memory DIBs, so `GetDC` is cheap)
- DPI blur or cropping
- needing compatibility modes
- "Cannot initialize graphics system"

## Running EF with SWGB Modern

- Copy `ddraw.dll`, `dsound.dll` and `SWGBModern.ini` next to `battlegrounds_x2.exe`. In EF's Configurator, leave **Window Mode off**: SWGB Modern provides borderless and windowed display, and `wndmode.dll` would hook DirectDraw a second time. Let EF's resolution patch choose the in-game size.
- SWGB Modern recognises EF by the `.patch` section, so a renamed `battlegrounds_x1.exe` (the GameRanger setup) is still detected. Under EF it applies only the renderer, audio, input, DPI and volume layers. EF's own engine patches stay in charge.
- EF sets its window thread to DPI-unaware, which made Windows magnify and crop the image on scaled displays. SWGB Modern now creates the game window per-monitor DPI aware.

## EF ideas not adopted (yet)

- Keydown object hotkeys, DirectWrite text, the large-maps (640×640) experiment, scenario-editor unlocks and autosave, and EF's content and AI fixes. These are either game-content changes or need deeper engine work with multiplayer-sync risk.
- Raising the simulation turn rate (beyond the render rate). It changes simulation granularity and would need multiplayer validation.
