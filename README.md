# SWGB Modern

A compatibility and enhancement mod for **Star Wars: Galactic Battlegrounds** (base game and Clone Campaigns, GOG / Steam / retail). It runs on modern Windows with modern graphics and audio, without modifying any original game file.

> Not affiliated with Lucasfilm, LucasArts, Disney, Ensemble Studios or GOG. You need to own the game.

## Features

**Graphics**
- A complete DirectDraw replacement that renders the game through **Direct3D 11, Direct3D 12, Direct3D 10.1, Vulkan, OpenGL 3.3 or a software (CPU) renderer**. It falls back automatically if one isn't available. Direct3D 12 and Vulkan are marked experimental: with some AMD drivers they use a lot of CPU in the menus. The software renderer is slow at 4K.
- **Borderless fullscreen or windowed** (Alt+Enter), with no display mode switches, no palette corruption, and no crashes when you Alt-Tab.
- **True widescreen in game** (for example 1920×1080). The game's own UI layout is rebuilt for the new width: the command panel stretches like Expanding Fronts' (ornaments, buttons and emblems keep their shape; the frame, background and Tech Level bar get longer), or you can keep the original panel centred. Menus remain 800×600 and are scaled.
- **UI scaling**: choose your native resolution and a scale factor. The launcher shows which scales are pixel-perfect and which one is closest to the original interface size. Per-monitor DPI aware.
- **Upscaling filters**: Nearest (lossless), Sharp (crisp pixels, blended edges at non-integer scales) and Bilinear (soft).
- **Mouse-wheel zoom** (optional, Display tab): zoom the battlefield out to 85/70/60% or in up to 125%, like newer RTS games. The command panel keeps its size. The original sprites are scaled, not redrawn, and zooming out costs more CPU because the game draws a larger battlefield.
- **Smooth camera**: scrolling moves the view a little every frame instead of jumping half a tile 20 times a second, at the same speed.
- **Smoother rendering**: units move and the battlefield is redrawn up to 60 times per second instead of 20 (the same world-update change Expanding Fronts makes), and the minimap 10 times per second instead of 3. **Game speed is unchanged**, because units move by elapsed time (verified with the in-game clock).
- Frame limiter tied to the monitor's refresh rate, plus VSync with tearing support. Unchanged frames are not re-presented, so the mod adds little CPU load.

**Optional enhancements** (all off in the *Faithful* profile, on in *Enhanced*)
- **Smooth text**: interface text is drawn with anti-aliased fonts (like Expanding Fronts), blended into the game's own frame. Line breaks, spacing and alignment stay as in the original.
- **Smooth fog of war**: fogged terrain and objects keep all their detail and are evenly darkened instead of drawn as a black checkerboard.

The mod ships no replacement art: everything on screen comes from your copy of the game.

**Audio**
- A DirectSound replacement on WASAPI: 32-bit float mixing at the device's native rate, band-limited **windowed-sinc resampling** of the 11/22 kHz game audio, a soft limiter, and following the Windows default device.
- The in-game music and sound sliders **no longer change your Windows volume**.

**Fixes**
- The original game keeps one CPU core at 100% in menus; with the mod it idles.
- Edit boxes (player names, chat) are positioned and scaled correctly.
- 4 GB (Large Address Aware) launch option, applied to a copy so the original exe is untouched.
- Plays the intro videos through the mod's renderer (any key skips one, Esc skips all). If Windows lacks the old Indeo codec, the launcher converts them with FFmpeg.
- Local crash dumps (`Game\CrashDumps`), never uploaded.
- Works with the **Expanding Fronts** community mod as a renderer and audio layer. EF keeps its own engine patches. See [docs/ExpandingFronts.md](docs/ExpandingFronts.md).

## Install

1. Download `SWGBModern.exe` from [Releases](../../releases).
2. Put it anywhere (the game folder is convenient) and run it. It finds GOG, Steam and retail installations, or you can choose the folder.
3. Pick your settings and press **PLAY**. The mod installs itself on first launch.

**Uninstall:** use the launcher's *Uninstall* button. It removes the mod's files and restores anything it replaced. **Update:** the launcher checks GitHub Releases and updates itself.

The mod is just two DLLs (`ddraw.dll`, `dsound.dll`) and `SWGBModern.ini` next to the game executable, so it also works when the game is started from GOG Galaxy or Steam.

If the game is in `Program Files`, Windows may block writing to the folder; run the launcher once as administrator. For LAN/IP multiplayer, Windows' optional *DirectPlay* feature is required, and the launcher can enable it.

## Configuration

All settings are in `Game\SWGBModern.ini` (edited by the launcher; manual edits are kept). The main keys:

| Key | Values |
|---|---|
| `[Display] Renderer` | `auto`, `d3d11`, `d3d12`, `d3d10`, `vulkan`, `opengl`, `software` |
| `[Display] Mode` | `borderless`, `windowed` |
| `[Display] GameWidth/GameHeight` | in-game resolution (`0` = original 4:3 modes) |
| `[Display] Filter` | `nearest`, `sharp`, `bilinear` |
| `[Display] ScaleMode` | `aspect`, `integer`, `stretch` |
| `[Widescreen] PanelStyle` | `stretch`, `center` |
| `[Fixes] SmoothScroll / ScrollSpeedPct` | `1` / `80` |
| `[Fixes] GameFPS` | battlefield redraw rate (original 20) |
| `[Audio] Resampler` | `sinc`, `cubic`, `linear` |
| `[Enhancements] HDText / SmoothFog` | `0` / `1` |
| `[Zoom] Enabled / MaxOut / MaxIn` | `0` / `70` / `125` |
| `[Video] PlayIntro` | `0` / `1` |
| `[Debug] Diagnostics / CaptureKey` | `0` / `1`, default key `ctrl+shift+f12` |

Renderer changes apply the next time the game starts. Filter, scaling, window mode, VSync and frame limit apply immediately when the launcher saves.

## Diagnostics

Turn on **Diagnostics** on the launcher's Mod tab. The log (`SWGBModern.log` in the game folder) then gets one line per second with the game's and the display's frame rates, the longest frame gap, CPU and memory, plus a `stutter` line whenever the game's frame loop stalls. Press **Ctrl+Shift+F12** while playing to save the 3 seconds before and after the key press to `Game\SWGBModern\captures\<date_time>\`: `clip.avi` (Motion-JPEG video), `frames\*.png` (4 stills per second), `screenshot.png` and `summary.txt` (stats, settings and the end of the log). Open the folder with **Open captures**.

## How it works

- `ddraw.dll` emulates the DirectDraw 1 API the Genie engine uses, with 8-bit surfaces in system memory. A presenter thread uploads each frame and its palette to the GPU, resolves the palette, and scales it in a shader. It also hooks the window and input APIs so the game always sees its own resolution.
- Engine patches are applied **in memory** and only to executables identified byte-for-byte: Clone Campaigns 1.1 and the base game. Widescreen works through a hook on the engine's resource loader. It rewrites the interface layout table and generates wider panel art at runtime, so nothing is written to the game's data files.
- Smooth text hooks `TextOutA` and `DrawTextA`/`DrawTextExA`: each string is rendered anti-aliased with the same font, blended with the pixels underneath, and written into the 8-bit surface as the nearest palette colours, so it behaves exactly like the original text.
- The stretched panel is widened along the plainest connected vertical path through the info panel (found by dynamic programming over colour differences), so no ornament is repeated or cut.
- Smooth camera: the edge and arrow-key scroll routines rounded the distance down to half a tile and only moved when that much time had passed; the exact distance is used instead. With smooth fog the map view also shifts in 1 px steps instead of the engine's 8 px.
- Smooth fog wraps the engine's sprite blitter: fogged sprites and terrain tiles are drawn with all their pixels and then darkened through a palette lookup table instead of the black checkerboard.
- `dsound.dll` implements DirectSound buffers on top of a WASAPI mixer.
- The launcher is a self-contained .NET 10 WPF app that embeds both DLLs.

## Building

Requirements: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (i686 target), the Vulkan SDK (for SPIR-V shaders; optional), the .NET 10 SDK, and Git Bash.

```bash
./build.sh            # ddraw.dll + dsound.dll -> build/out
./package.sh 1.0.0    # launcher with embedded DLLs -> dist/SWGBModern.exe
```

`tools/sigfind.py` locates engine patch sites in other builds of the game executable using masked byte signatures.

## License

MIT, see [LICENSE](LICENSE).
