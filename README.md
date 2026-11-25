# SWGB Modern

A compatibility and enhancement mod for **Star Wars: Galactic Battlegrounds** (base game and Clone Campaigns, GOG / Steam / retail). It runs on modern Windows with modern graphics and audio, without modifying any original game file.

> Not affiliated with Lucasfilm, LucasArts, Disney, Ensemble Studios or GOG. You need to own the game.

## Features

**Graphics**
- A complete DirectDraw replacement that renders the game through **Direct3D 11, Direct3D 12, Direct3D 10.1, Vulkan, OpenGL 3.3 or a software (CPU) renderer**. It falls back automatically if one isn't available.
- **Borderless fullscreen or windowed** (Alt+Enter), with no display mode switches, no palette corruption, and no crashes when you Alt-Tab.
- **True widescreen in game** (for example 1920×1080). The game's own UI layout is rebuilt for the new width: the command panel stretches like a native widescreen UI, or you can keep the original panel centred. Menus remain 800×600 and are scaled.
- **UI scaling**: the in-game resolution is your monitor divided by an integer factor, so the UI is crisp and readable at 1440p, 4K and above. Per-monitor DPI aware.
- **Upscaling filters**: Nearest (lossless), Sharp-bilinear, Bilinear, and **xBR** (an edge-directed pixel-art upscaler).
- **Smoother rendering**: the battlefield is redrawn up to 60 times per second instead of 20, and the minimap 10 times per second instead of 3. **Game speed is unchanged**, because the simulation runs on real time (verified with the in-game clock).
- Frame limiter tied to the monitor's refresh rate, plus VSync with tearing support.

**Audio**
- A DirectSound replacement on WASAPI: 32-bit float mixing at the device's native rate, band-limited **windowed-sinc resampling** of the 11/22 kHz game audio, a soft limiter, and following the Windows default device.
- The in-game music and sound sliders **no longer change your Windows volume**.

**Fixes**
- The original game keeps one CPU core at 100% in menus; with the mod it idles.
- Edit boxes (player names, chat) are positioned and scaled correctly.
- 4 GB (Large Address Aware) launch option, applied to a copy so the original exe is untouched.
- Skips the intro videos, which use the long-removed Indeo codec.
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
| `[Display] Filter` | `nearest`, `sharp`, `bilinear`, `xbr` |
| `[Display] ScaleMode` | `aspect`, `integer`, `stretch` |
| `[Widescreen] PanelStyle` | `stretch`, `center` |
| `[Fixes] GameFPS` | battlefield redraw rate (original 20) |
| `[Audio] Resampler` | `sinc`, `cubic`, `linear` |

## How it works

- `ddraw.dll` emulates the DirectDraw 1 API the Genie engine uses, with 8-bit surfaces in system memory. A presenter thread uploads each frame and its palette to the GPU, resolves the palette, and scales it in a shader. It also hooks the window and input APIs so the game always sees its own resolution.
- Engine patches are applied **in memory** and only to executables identified byte-for-byte: Clone Campaigns 1.1 and the base game. Widescreen works through a hook on the engine's resource loader. It rewrites the interface layout table and generates wider panel art at runtime, so nothing is written to the game's data files.
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

MIT, see [LICENSE](LICENSE). The xBR filter is an independent implementation of Hyllian's algorithm (MIT).
