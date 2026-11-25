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
    public int UiScale { get; set; }                        // 0 = auto

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
        if (n is nameof(ResolutionMode) or nameof(UiScale) or nameof(CustomWidth) or nameof(CustomHeight)) On(nameof(ResolutionSummary));
        return true;
    }

    public LauncherPrefs Prefs { get; } = LauncherPrefs.Load();

    // ---------------------------------------------------------------- choices
    public List<Choice> Renderers { get; } = new()
    {
        new("auto", "Automatic (recommended)"), new("d3d11", "Direct3D 11"), new("d3d12", "Direct3D 12"),
        new("d3d10", "Direct3D 10.1"), new("vulkan", "Vulkan"), new("opengl", "OpenGL 3.3"), new("software", "Software (CPU)"),
    };
    public List<Choice> Modes { get; } = new() { new("borderless", "Borderless fullscreen"), new("windowed", "Windowed") };
    public List<Choice> ScaleModes { get; } = new()
    {
        new("aspect", "Fit, keep aspect ratio"), new("integer", "Integer only (pixel-perfect)"), new("stretch", "Stretch to fill"),
    };
    public List<Choice> Filters { get; } = new()
    {
        new("nearest", "Nearest (lossless pixels)"), new("sharp", "Sharp bilinear"), new("xbr", "xBR (smooth pixel-art upscaler)"),
        new("bilinear", "Bilinear (soft)"),
    };
    public List<Choice> ResolutionModes { get; } = new()
    {
        new("auto", "Widescreen, scaled to monitor"), new("custom", "Widescreen, custom size"), new("classic", "Original (4:3, as the game shipped)"),
    };
    public List<Choice> UiScales { get; } = new() { new("0", "Auto"), new("1", "1x"), new("2", "2x"), new("3", "3x"), new("4", "4x") };
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
    public string Filter { get => _filter; set { Set(ref _filter, value); On(nameof(ShowSharpness)); } }
    public bool ShowSharpness => Filter == "xbr";
    private double _sharpness = 0.5;
    public double Sharpness { get => _sharpness; set => Set(ref _sharpness, value); }
    private bool _vsync = true, _lockCursor = true;
    public bool VSync { get => _vsync; set => Set(ref _vsync, value); }
    public bool LockCursor { get => _lockCursor; set => Set(ref _lockCursor, value); }
    private int _maxFps;
    public int MaxFps { get => _maxFps; set => Set(ref _maxFps, value); }
    private string _resolutionMode = "auto";
    public string ResolutionMode { get => _resolutionMode; set { Set(ref _resolutionMode, value); On(nameof(IsCustomResolution)); On(nameof(IsScaledResolution)); } }
    public bool IsCustomResolution => ResolutionMode == "custom";
    public bool IsScaledResolution => ResolutionMode == "auto";
    private string _uiScale = "0";
    public string UiScale { get => _uiScale; set => Set(ref _uiScale, value); }
    private int _customWidth = 1920, _customHeight = 1080;
    public int CustomWidth { get => _customWidth; set => Set(ref _customWidth, value); }
    public int CustomHeight { get => _customHeight; set => Set(ref _customHeight, value); }
    private string _panelStyle = "stretch";
    public string PanelStyle { get => _panelStyle; set => Set(ref _panelStyle, value); }

    public (int W, int H) EffectiveResolution()
    {
        if (ResolutionMode == "custom") return (Math.Max(1280, CustomWidth), Math.Max(720, CustomHeight));
        var r = SystemInfo.ResolutionForScale(int.TryParse(UiScale, out var s) ? s : 0);
        return (r.W, r.H);
    }

    public string ResolutionSummary
    {
        get
        {
            var (mw, mh, hz) = SystemInfo.PrimaryMonitor();
            if (ResolutionMode == "classic") return $"Menus and game use the original 800x600 / 1024x768 / 1280x1024 modes, scaled to your {mw}x{mh} screen.";
            var (w, h) = EffectiveResolution();
            string scale = mw % w == 0 && mh % h == 0 ? $"exact {mw / w}x pixel scaling" : "non-integer scaling (use Sharp or xBR filter)";
            return $"In-game resolution {w}x{h} on your {mw}x{mh} @ {hz} Hz monitor: {scale}. Menus stay 800x600 (scaled).";
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
    public bool LargeAddressAware { get => Prefs.LargeAddressAware; set { Prefs.LargeAddressAware = value; On(); } }
    public bool SkipIntro { get => Prefs.SkipIntro; set { Prefs.SkipIntro = value; On(); } }
    public bool CheckUpdates { get => Prefs.CheckUpdates; set { Prefs.CheckUpdates = value; On(); } }

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
        ResolutionMode = Prefs.ResolutionMode;
        UiScale = Prefs.UiScale.ToString();
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
        Sharpness = ini.GetDouble("Display", "Sharpness", 0.5);
        VSync = ini.GetBool("Display", "VSync", true);
        MaxFps = ini.GetInt("Display", "MaxFPS", 0);
        LockCursor = ini.GetBool("Display", "LockCursor", true);
        CustomWidth = ini.GetInt("Display", "GameWidth", 1920);
        CustomHeight = ini.GetInt("Display", "GameHeight", 1080);
        PanelStyle = Pick(PanelStyles, ini.GetString("Widescreen", "PanelStyle", "stretch"), "stretch");
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
        Prefs.UiScale = int.TryParse(UiScale, out var s) ? s : 0;
        Prefs.Save();
        if (GameDir == null) return;
        var ini = new IniFile(IniPath);
        if (!File.Exists(IniPath) && Installer.Payload(Installer.IniName) is { } def)
            ini = IniFile.FromText(IniPath, System.Text.Encoding.UTF8.GetString(def));
        ini.Set("Display", "Renderer", Renderer);
        ini.Set("Display", "Mode", Mode);
        ini.Set("Display", "ScaleMode", ScaleMode);
        ini.Set("Display", "Filter", Filter);
        ini.Set("Display", "Sharpness", Sharpness);
        ini.Set("Display", "VSync", VSync);
        ini.Set("Display", "MaxFPS", MaxFps);
        ini.Set("Display", "LockCursor", LockCursor);
        bool widescreen = ResolutionMode != "classic";
        var (w, h) = EffectiveResolution();
        ini.Set("Display", "GameWidth", widescreen ? w : 0);
        ini.Set("Display", "GameHeight", widescreen ? h : 0);
        ini.Set("Fixes", "Widescreen", widescreen);
        ini.Set("Widescreen", "PanelStyle", PanelStyle);
        ini.Set("Fixes", "GameFPS", GameFps);
        ini.Set("Fixes", "MinimapRefreshMs", MinimapMs);
        ini.Set("Fixes", "IdleCPU", IdleCpu);
        ini.Set("Fixes", "IsolateVolume", IsolateVolume);
        ini.Set("Audio", "Enabled", AudioEnabled);
        ini.Set("Audio", "Resampler", Resampler);
        ini.Set("Audio", "Volume", Volume);
        ini.Set("Audio", "LatencyMs", Latency);
        ini.Set("Audio", "Limiter", Limiter);
        ini.Set("Debug", "Log", Log);
        ini.Set("Debug", "CrashDumps", CrashDumps);
        ini.Save();
    }
}
