using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;
using SWGBModern.Core;

namespace SWGBModern;

public sealed record Choice(string Value, string Label)
{
    public override string ToString() => Label;
}

/// <summary>Launcher-only preferences (game location etc.), stored per user.</summary>
public sealed class LauncherPrefs
{
    public string? GameDir { get; set; }
    public string LastGame { get; set; } = "CloneCampaigns";
    public bool LargeAddressAware { get; set; }
    public bool SkipIntro { get; set; } = true;
    public bool CheckUpdates { get; set; } = true;
    public string ResolutionMode { get; set; } = "auto";   // auto | classic | custom
    public int NativeW { get; set; }                        // 0 = detect
    public int NativeH { get; set; }
    public double Scale { get; set; }                       // 0 = closest to original
    public string? Profile { get; set; }                    // faithful | enhanced | custom (null = first run)

    private static string PathOf => System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "SWGBModern", "launcher.json");

    public static LauncherPrefs Load()
    {
        try { if (File.Exists(PathOf)) return JsonSerializer.Deserialize<LauncherPrefs>(File.ReadAllText(PathOf)) ?? new(); }
        catch { }
        return new();
    }

    public void Save()
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(PathOf)!);
        File.WriteAllText(PathOf, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
    }
}

public sealed class MainViewModel : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    private void On([CallerMemberName] string? n = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
    private bool Set<T>(ref T field, T value, [CallerMemberName] string? n = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        On(n);
        if (n is nameof(ResolutionMode) or nameof(SelectedScale) or nameof(CustomWidth) or nameof(CustomHeight)) On(nameof(ResolutionSummary));
        return true;
    }

    public LauncherPrefs Prefs { get; } = LauncherPrefs.Load();

    // ---------------------------------------------------------------- choices
    public List<Choice> Renderers { get; } = new()
    {
        new("auto", "Automatic (recommended)"), new("d3d11", "Direct3D 11"),
        new("d3d12", "Direct3D 12 (experimental: high CPU use in menus with some AMD drivers)"),
        new("d3d10", "Direct3D 10.1"), new("vulkan", "Vulkan (experimental: high CPU use in menus with some AMD drivers)"),
        new("opengl", "OpenGL 3.3"), new("software", "Software (CPU) - slow at 4K"),
    };
    public List<Choice> Modes { get; } = new() { new("borderless", "Borderless fullscreen"), new("windowed", "Windowed") };
    public List<Choice> ScaleModes { get; } = new()
    {
        new("aspect", "Fit, keep aspect ratio"), new("integer", "Integer only (pixel-perfect)"), new("stretch", "Stretch to fill"),
    };
    public List<Choice> Filters { get; } = new()
    {
        new("nearest", "Nearest (lossless pixels)"), new("sharp", "Sharp bilinear"), new("bilinear", "Bilinear (soft)"),
    };
    public List<Choice> ResolutionModes { get; } = new()
    {
        new("auto", "Widescreen: native resolution + scale"), new("custom", "Widescreen: custom size"), new("classic", "Original 4:3 modes (as the game shipped)"),
    };
    public ObservableCollection<Choice> NativeModes { get; } = new();
    private void RebuildNativeModes()
    {
        NativeModes.Clear();
        foreach (var m in SystemInfo.MonitorModes()) NativeModes.Add(new Choice($"{m.W}x{m.H}", $"{m.W} x {m.H}"));
        NativeModes.Add(new Choice("custom", "Other..."));
    }
    public List<Choice> Monitors { get; } = SystemInfo.Monitors()
        .Select(m => new Choice(m.Primary ? "" : m.Device, $"{(m.Primary ? "Primary - " : "")}Monitor {m.Number}: {m.W} x {m.H} @ {m.Hz} Hz")).ToList();
    public bool MultiMonitor => Monitors.Count > 1;
    private string _monitor = "";
    public string Monitor
    {
        get => _monitor;
        set
        {
            if (!Set(ref _monitor, value ?? "")) return;
            SystemInfo.TargetDevice = string.IsNullOrEmpty(_monitor) ? null : _monitor;
            RebuildNativeModes();
            var (mw, mh, _) = SystemInfo.PrimaryMonitor();
            NativeRes = $"{mw}x{mh}";
            RebuildScales();
            On(nameof(ResolutionSummary));
        }
    }
    public List<Choice> ZoomInChoices { get; } = new() { new("100", "No zoom in"), new("125", "Up to 125%"), new("150", "Up to 150%"), new("200", "Up to 200%") };
    private string _zoomIn = "125";
    public string ZoomIn { get => _zoomIn; set => Set(ref _zoomIn, value); }
    private bool _zoomSnap = true;
    public bool ZoomSnap { get => _zoomSnap; set => Set(ref _zoomSnap, value); }
    public bool ZoomEnabled => ZoomOut != "off";
    public ObservableCollection<SystemInfo.ScaleOption> ScaleChoices { get; } = new();
    public List<Choice> PanelStyles { get; } = new() { new("stretch", "Stretched (wide info panel)"), new("center", "Centred original panel") };
    public List<Choice> Resamplers { get; } = new() { new("sinc", "Windowed sinc (best)"), new("cubic", "Cubic"), new("linear", "Linear (classic)") };
    public ObservableCollection<GameExecutable> Games { get; } = new();

    // ---------------------------------------------------------------- state
    private string? _gameDir;
    public string? GameDir { get => _gameDir; set { if (Set(ref _gameDir, value)) Reload(); } }
    private GameExecutable? _selectedGame;
    public GameExecutable? SelectedGame { get => _selectedGame; set { Set(ref _selectedGame, value); On(nameof(IsExpandingFronts)); } }
    public bool IsExpandingFronts => SelectedGame?.Variant == GameVariant.ExpandingFronts;
    private string _status = "";
    public string Status { get => _status; set => Set(ref _status, value); }
    private string _installState = "";
    public string InstallStateText { get => _installState; set => Set(ref _installState, value); }
    public string Version => "v" + Installer.Version;
    public string DirectPlayText => SystemInfo.DirectPlayInstalled() ? "Installed" : "Not installed (needed for LAN/IP multiplayer only)";
    public bool DirectPlayMissing => !SystemInfo.DirectPlayInstalled();

    // display
    private string _renderer = "auto", _mode = "borderless", _scaleMode = "aspect", _filter = "nearest";
    public string Renderer { get => _renderer; set => Set(ref _renderer, value); }
    public string Mode { get => _mode; set => Set(ref _mode, value); }
    public string ScaleMode { get => _scaleMode; set => Set(ref _scaleMode, value); }
    public string Filter { get => _filter; set => Set(ref _filter, value); }
    private bool _vsync = true, _lockCursor = true;
    public bool VSync { get => _vsync; set => Set(ref _vsync, value); }
    public bool LockCursor { get => _lockCursor; set => Set(ref _lockCursor, value); }
    private int _maxFps;
    public int MaxFps { get => _maxFps; set => Set(ref _maxFps, value); }
    private string _resolutionMode = "auto";
    public string ResolutionMode { get => _resolutionMode; set { Set(ref _resolutionMode, value); On(nameof(IsCustomResolution)); On(nameof(IsScaledResolution)); } }
    public bool IsCustomResolution => ResolutionMode == "custom";
    public bool IsScaledResolution => ResolutionMode == "auto";
    private string _nativeRes = "";
    public string NativeRes { get => _nativeRes; set { if (Set(ref _nativeRes, value)) { On(nameof(IsCustomNative)); RebuildScales(); } } }
    public bool IsCustomNative => NativeRes == "custom";
    private int _nativeW = 3840, _nativeH = 2160;
    public int NativeW { get => _nativeW; set { if (Set(ref _nativeW, value)) RebuildScales(); } }
    public int NativeH { get => _nativeH; set { if (Set(ref _nativeH, value)) RebuildScales(); } }
    private SystemInfo.ScaleOption? _scale;
    public SystemInfo.ScaleOption? SelectedScale { get => _scale; set { Set(ref _scale, value); On(nameof(ResolutionSummary)); } }

    private (int W, int H) Native()
    {
        if (NativeRes != "custom" && NativeRes.Split('x') is [var a, var b] && int.TryParse(a, out var w) && int.TryParse(b, out var h)) return (w, h);
        return (Math.Max(1280, NativeW), Math.Max(720, NativeH));
    }

    private void RebuildScales()
    {
        double want = SelectedScale?.Scale ?? Prefs.Scale;
        var (w, h) = Native();
        ScaleChoices.Clear();
        foreach (var o in SystemInfo.ScaleOptions(w, h)) ScaleChoices.Add(o);
        SelectedScale = ScaleChoices.FirstOrDefault(o => o.Scale == want) ?? ScaleChoices.FirstOrDefault(o => o.ClosestToOriginal) ?? ScaleChoices.FirstOrDefault();
    }
    private int _customWidth = 1920, _customHeight = 1080;
    public int CustomWidth { get => _customWidth; set => Set(ref _customWidth, value); }
    public int CustomHeight { get => _customHeight; set => Set(ref _customHeight, value); }
    private string _panelStyle = "stretch";
    public string PanelStyle { get => _panelStyle; set => Set(ref _panelStyle, value); }
    public List<Choice> ZoomChoices { get; } = new() { new("off", "Off"), new("85", "Up to 85% (zoom out a little)"), new("70", "Up to 70%"), new("60", "Up to 60% (most map, most CPU)") };
    private string _zoomOut = "off";
    public string ZoomOut { get => _zoomOut; set { if (Set(ref _zoomOut, value)) On(nameof(ZoomEnabled)); } }

    public (int W, int H) EffectiveResolution()
    {
        if (ResolutionMode == "custom") return (Math.Max(1280, CustomWidth), Math.Max(720, CustomHeight));
        if (SelectedScale is { } o) return (o.W, o.H);
        var r = SystemInfo.ResolutionForScale(0);
        return (r.W, r.H);
    }

    public string ResolutionSummary
    {
        get
        {
            var (mw, mh, hz) = SystemInfo.PrimaryMonitor();
            if (ResolutionMode == "classic") return $"Menus and game use the original 800x600 / 1024x768 / 1280x1024 modes, scaled to your {mw}x{mh} screen.";
            var (w, h) = EffectiveResolution();
            string scale = mw % w == 0 && mh % h == 0 && mw / w == mh / h ? $"exact {mw / w}x pixel scaling" : "non-integer scaling (use the Sharp filter)";
            return $"In-game {w}x{h}, shown on your {mw}x{mh} @ {hz} Hz desktop with {scale}. The interface is drawn at a fixed pixel size, " +
                   "so a lower in-game resolution means a larger interface. Menus are 800x600 (scaled).";
        }
    }

    // fixes
    private int _gameFps = 60, _minimapMs = 100;
    public int GameFps { get => _gameFps; set => Set(ref _gameFps, value); }
    public int MinimapMs { get => _minimapMs; set => Set(ref _minimapMs, value); }
    private bool _idleCpu = true, _isolateVolume = true, _crashDumps = true, _log;
    public bool IdleCpu { get => _idleCpu; set => Set(ref _idleCpu, value); }
    public bool IsolateVolume { get => _isolateVolume; set => Set(ref _isolateVolume, value); }
    public bool CrashDumps { get => _crashDumps; set => Set(ref _crashDumps, value); }
    public bool Log { get => _log; set => Set(ref _log, value); }
    private bool _diagnostics;
    public bool Diagnostics { get => _diagnostics; set { Set(ref _diagnostics, value); if (value) Log = true; } }
    public bool LargeAddressAware { get => Prefs.LargeAddressAware; set { Prefs.LargeAddressAware = value; On(); } }
    public bool CheckUpdates { get => Prefs.CheckUpdates; set { Prefs.CheckUpdates = value; On(); } }

    // ---------------------------------------------------------------- components
    // Core components keep the original look; enhancements change it and are
    // optional (see the Components tab and the Faithful / Enhanced profiles).
    public bool EnhWidescreen
    {
        get => ResolutionMode != "classic";
        set { ResolutionMode = value ? (ResolutionMode == "classic" ? "auto" : ResolutionMode) : "classic"; On(); }
    }
    public bool EnhSmooth { get => GameFps > 20; set { GameFps = value ? 60 : 20; MinimapMs = value ? 100 : 333; On(); } }
    public bool EnhFilter { get => Filter != "nearest"; set { Filter = value ? "sharp" : "nearest"; On(); } }
    private bool _hdText, _playIntro = true, _smoothFog;
    public bool SmoothFog { get => _smoothFog; set => Set(ref _smoothFog, value); }
    public bool HdText { get => _hdText; set => Set(ref _hdText, value); }
    public bool PlayIntro { get => _playIntro; set => Set(ref _playIntro, value); }
    private bool _generating;
    public bool Generating { get => _generating; set { Set(ref _generating, value); On(nameof(NotGenerating)); } }
    public bool NotGenerating => !Generating;

    public void ApplyProfile(string name)
    {
        bool enhanced = name == "enhanced";
        EnhWidescreen = enhanced;
        EnhSmooth = enhanced;
        Filter = enhanced ? "sharp" : "nearest";
        HdText = false;     // optional extras, not part of a profile
        SmoothFog = false;
        PanelStyle = "stretch";
        Prefs.Profile = name;
        foreach (var n in new[] { nameof(EnhWidescreen), nameof(EnhSmooth), nameof(EnhFilter), nameof(ProfileText) }) On(n);
    }
    public string ProfileText => Prefs.Profile switch
    {
        "faithful" => "Faithful: original look, only compatibility fixes",
        "enhanced" => "Enhanced: widescreen, smooth rendering, Sharp filter",
        _ => "Custom",
    };

    // audio
    private bool _audioEnabled = true, _limiter = true;
    public bool AudioEnabled { get => _audioEnabled; set => Set(ref _audioEnabled, value); }
    public bool Limiter { get => _limiter; set => Set(ref _limiter, value); }
    private string _resampler = "sinc";
    public string Resampler { get => _resampler; set => Set(ref _resampler, value); }
    private int _volume = 100, _latency = 20;
    public int Volume { get => _volume; set => Set(ref _volume, value); }
    public int Latency { get => _latency; set => Set(ref _latency, value); }

    public MainViewModel()
    {
        GameDir = GameLocator.Find(Prefs.GameDir);
        RebuildNativeModes();
        ResolutionMode = Prefs.ResolutionMode;
        var (cw, ch, _) = SystemInfo.PrimaryMonitor();
        if (Prefs.NativeW > 0 && Prefs.NativeH > 0)
        {
            var key = $"{Prefs.NativeW}x{Prefs.NativeH}";
            if (NativeModes.Any(m => m.Value == key)) NativeRes = key;
            else { _nativeW = Prefs.NativeW; _nativeH = Prefs.NativeH; NativeRes = "custom"; }
        }
        else NativeRes = $"{cw}x{ch}";
    }

    private string IniPath => Path.Combine(GameDir ?? "", Installer.IniName);

    public void Reload()
    {
        Games.Clear();
        if (GameDir == null) { Status = "Game not found - choose its folder."; InstallStateText = ""; return; }
        foreach (var g in GameLocator.Executables(GameDir)) Games.Add(g);
        SelectedGame = Games.FirstOrDefault(g => g.Variant.ToString() == Prefs.LastGame) ?? Games.FirstOrDefault();
        var ini = new IniFile(IniPath);
        Renderer = Pick(Renderers, ini.GetString("Display", "Renderer", "auto"), "auto");
        Mode = Pick(Modes, ini.GetString("Display", "Mode", "borderless"), "borderless");
        ScaleMode = Pick(ScaleModes, ini.GetString("Display", "ScaleMode", "aspect"), "aspect");
        Filter = Pick(Filters, ini.GetString("Display", "Filter", "nearest"), "nearest");
        VSync = ini.GetBool("Display", "VSync", true);
        MaxFps = ini.GetInt("Display", "MaxFPS", 0);
        LockCursor = ini.GetBool("Display", "LockCursor", true);
        CustomWidth = ini.GetInt("Display", "GameWidth", 1920);
        CustomHeight = ini.GetInt("Display", "GameHeight", 1080);
        PanelStyle = Pick(PanelStyles, ini.GetString("Widescreen", "PanelStyle", "stretch"), "stretch");
        ZoomOut = ini.GetBool("Zoom", "Enabled", false) ? Pick(ZoomChoices, ini.GetString("Zoom", "MaxOut", "70"), "70") : "off";
        ZoomIn = Pick(ZoomInChoices, ini.GetString("Zoom", "MaxIn", "125"), "125");
        ZoomSnap = ini.GetBool("Zoom", "SnapAt100", true);
        var mon = ini.GetString("Display", "Monitor", "");
        Monitor = Monitors.Any(m => m.Value == mon) ? mon : "";
        GameFps = ini.GetInt("Fixes", "GameFPS", 60);
        MinimapMs = ini.GetInt("Fixes", "MinimapRefreshMs", 100);
        IdleCpu = ini.GetBool("Fixes", "IdleCPU", true);
        IsolateVolume = ini.GetBool("Fixes", "IsolateVolume", true);
        Resampler = Pick(Resamplers, ini.GetString("Audio", "Resampler", "sinc"), "sinc");
        Volume = ini.GetInt("Audio", "Volume", 100);
        Latency = ini.GetInt("Audio", "LatencyMs", 20);
        Limiter = ini.GetBool("Audio", "Limiter", true);
        AudioEnabled = ini.GetBool("Audio", "Enabled", true);
        Log = ini.GetBool("Debug", "Log", false);
        Diagnostics = ini.GetBool("Debug", "Diagnostics", false);
        HdText = ini.GetBool("Enhancements", "HDText", false);
        SmoothFog = ini.GetBool("Enhancements", "SmoothFog", false);
        PlayIntro = ini.GetBool("Video", "PlayIntro", true);
        foreach (var n in new[] { nameof(EnhWidescreen), nameof(EnhSmooth), nameof(EnhFilter), nameof(ProfileText) }) On(n);
        CrashDumps = ini.GetBool("Debug", "CrashDumps", true);
        RefreshInstallState();
    }

    private static string Pick(List<Choice> list, string value, string def) =>
        list.Any(c => string.Equals(c.Value, value, StringComparison.OrdinalIgnoreCase)) ? value.ToLowerInvariant() : def;

    public void RefreshInstallState()
    {
        if (GameDir == null) return;
        InstallStateText = new Installer(GameDir).State() switch
        {
            InstallState.Installed => "Installed and up to date",
            InstallState.Outdated => "Installed (an update from this launcher will be applied)",
            InstallState.Damaged => "Installed files were changed - Repair recommended",
            _ => "Not installed yet - installs automatically on Play",
        };
    }

    public void SaveAll()
    {
        Prefs.GameDir = GameDir;
        Prefs.LastGame = SelectedGame?.Variant.ToString() ?? Prefs.LastGame;
        Prefs.ResolutionMode = ResolutionMode;
        var (nw, nh) = Native();
        Prefs.NativeW = nw; Prefs.NativeH = nh;
        Prefs.Scale = SelectedScale?.Scale ?? 0;
        Prefs.Save();
        if (GameDir == null) return;
        var ini = new IniFile(IniPath);
        if (!File.Exists(IniPath) && Installer.Payload(Installer.IniName) is { } def)
            ini = IniFile.FromText(IniPath, System.Text.Encoding.UTF8.GetString(def));
        ini.Set("Display", "Renderer", Renderer);
        ini.Set("Display", "Mode", Mode);
        ini.Set("Display", "ScaleMode", ScaleMode);
        ini.Set("Display", "Filter", Filter);
        ini.Set("Display", "VSync", VSync);
        ini.Set("Display", "MaxFPS", MaxFps);
        ini.Set("Display", "LockCursor", LockCursor);
        bool widescreen = ResolutionMode != "classic";
        var (w, h) = EffectiveResolution();
        ini.Set("Display", "GameWidth", widescreen ? w : 0);
        ini.Set("Display", "GameHeight", widescreen ? h : 0);
        ini.Set("Fixes", "Widescreen", widescreen);
        ini.Set("Widescreen", "PanelStyle", PanelStyle);
        ini.Set("Zoom", "Enabled", ZoomOut != "off");
        if (ZoomOut != "off") ini.Set("Zoom", "MaxOut", ZoomOut);
        ini.Set("Zoom", "MaxIn", ZoomIn);
        ini.Set("Zoom", "SnapAt100", ZoomSnap);
        ini.Set("Display", "Monitor", Monitor);
        ini.Set("Fixes", "GameFPS", GameFps);
        ini.Set("Fixes", "MinimapRefreshMs", MinimapMs);
        ini.Set("Fixes", "IdleCPU", IdleCpu);
        ini.Set("Fixes", "IsolateVolume", IsolateVolume);
        ini.Set("Audio", "Enabled", AudioEnabled);
        ini.Set("Audio", "Resampler", Resampler);
        ini.Set("Audio", "Volume", Volume);
        ini.Set("Audio", "LatencyMs", Latency);
        ini.Set("Audio", "Limiter", Limiter);
        ini.Set("Debug", "Log", Log || Diagnostics);
        ini.Set("Debug", "Diagnostics", Diagnostics);
        ini.Set("Enhancements", "HDText", HdText);
        ini.Set("Enhancements", "SmoothFog", SmoothFog);
        ini.Set("Video", "PlayIntro", PlayIntro);
        ini.Set("Debug", "CrashDumps", CrashDumps);
        ini.Save();
    }
}
