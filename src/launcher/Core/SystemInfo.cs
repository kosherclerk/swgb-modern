using System.IO;
using System.Runtime.InteropServices;

namespace SWGBModern.Core;

public static class SystemInfo
{
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
    private delegate bool MonitorEnumProc(IntPtr monitor, IntPtr hdc, IntPtr rect, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc proc, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetMonitorInfoW(IntPtr monitor, ref MONITORINFOEX mi);

    [StructLayout(LayoutKind.Sequential)] private struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MONITORINFOEX
    {
        public int cbSize;
        public RECT rcMonitor, rcWork;
        public int dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
    }

    public sealed record Monitor(string Device, int W, int H, int Hz, bool Primary, int Number);

    /// <summary>Connected monitors (physical resolution), primary first.</summary>
    public static List<Monitor> Monitors()
    {
        var list = new List<Monitor>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (m, _, _, _) =>
        {
            var mi = new MONITORINFOEX { cbSize = Marshal.SizeOf<MONITORINFOEX>() };
            if (!GetMonitorInfoW(m, ref mi)) return true;
            var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf<DEVMODE>() };
            int w = mi.rcMonitor.Right - mi.rcMonitor.Left, h = mi.rcMonitor.Bottom - mi.rcMonitor.Top, hz = 60;
            if (EnumDisplaySettingsW(mi.szDevice, -1, ref dm) && dm.dmPelsWidth > 0) { w = dm.dmPelsWidth; h = dm.dmPelsHeight; hz = dm.dmDisplayFrequency; }
            int num = int.TryParse(new string(mi.szDevice.Where(char.IsDigit).ToArray()), out var n) ? n : list.Count + 1;
            list.Add(new Monitor(mi.szDevice, w, h, hz, (mi.dwFlags & 1) != 0, num));
            return true;
        }, IntPtr.Zero);
        return list.OrderByDescending(m => m.Primary).ThenBy(m => m.Number).ToList();
    }

    /// <summary>The monitor the game runs on (device name; null = primary).</summary>
    public static string? TargetDevice { get; set; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool EnumDisplaySettingsW(string? device, int mode, ref DEVMODE dm);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }

    /// <summary>Physical resolution and refresh rate of the game's monitor (TargetDevice, else the primary).</summary>
    public static (int Width, int Height, int Hz) PrimaryMonitor()
    {
        var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf<DEVMODE>() };
        if (EnumDisplaySettingsW(TargetDevice, -1 /*ENUM_CURRENT_SETTINGS*/, ref dm) && dm.dmPelsWidth > 0)
            return (dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
        return (GetSystemMetrics(0), GetSystemMetrics(1), 60);
    }

    /// <summary>All distinct resolutions the primary monitor supports, largest first.</summary>
    public static List<(int W, int H)> MonitorModes()
    {
        var set = new HashSet<(int, int)>();
        var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf<DEVMODE>() };
        for (int i = 0; EnumDisplaySettingsW(TargetDevice, i, ref dm); i++)
            if (dm.dmPelsWidth >= 1280 && dm.dmPelsHeight >= 720) set.Add((dm.dmPelsWidth, dm.dmPelsHeight));
        var (cw, ch, _) = PrimaryMonitor();
        set.Add((cw, ch));
        return set.OrderByDescending(m => m.Item1 * m.Item2).ThenByDescending(m => m.Item1).ToList();
    }

    public sealed record ScaleOption(double Scale, int W, int H, bool Integer, bool ClosestToOriginal)
    {
        public override string ToString()
        {
            string s = Scale == Math.Floor(Scale) ? $"{Scale:0}x" : $"{Scale:0.##}x";
            string kind = Integer ? "pixel-perfect" : "non-integer - use the Sharp filter";
            string ui = $"{W}x{H}";
            return $"{s}  ->  {ui}  ({kind}){(ClosestToOriginal ? "  - closest to original" : "")}";
        }
    }

    /// <summary>
    /// In-game resolutions for a native resolution. The engine's widescreen
    /// layout needs at least 1280x720. "Closest to original": the game height
    /// nearest 1024 (the original maximum, 1280x1024), which keeps the command
    /// panel at roughly its original share of the screen.
    /// </summary>
    public static List<ScaleOption> ScaleOptions(int nw, int nh)
    {
        var list = new List<ScaleOption>();
        foreach (var s in new[] { 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0 })
        {
            int w = (int)Math.Round(nw / s) & ~1, h = (int)Math.Round(nh / s) & ~1;
            if (w < 1280 || h < 720) continue;
            bool integer = s == Math.Floor(s) && nw % (int)s == 0 && nh % (int)s == 0;
            list.Add(new ScaleOption(s, w, h, integer, false));
        }
        if (list.Count == 0) list.Add(new ScaleOption(1, Math.Max(1280, nw), Math.Max(720, nh), true, false));
        var best = list.OrderBy(o => Math.Abs(o.H - 1024)).ThenByDescending(o => o.Integer).First();
        return list.Select(o => o == best ? o with { ClosestToOriginal = true } : o).ToList();
    }

    /// <summary>
    /// Game resolution for a given UI scale: the monitor divided by an integer
    /// factor, so every game pixel maps to an exact NxN block (crisp UI).
    /// The engine's widescreen layout needs at least 1280x720.
    /// </summary>
    public static (int W, int H, int Scale) ResolutionForScale(int scale)
    {
        var (mw, mh, _) = PrimaryMonitor();
        if (scale <= 0) scale = AutoScale(mw, mh);
        while (scale > 1 && (mw / scale < 1280 || mh / scale < 720)) scale--;
        int w = mw / scale & ~1, h = mh / scale & ~1;
        return (Math.Max(1280, w), Math.Max(720, h), scale);
    }

    public static int AutoScale(int mw, int mh) => Math.Max(1, mh / 1000);

    /// <summary>DirectPlay (needed for LAN/IP multiplayer) is an optional Windows feature.</summary>
    public static bool DirectPlayInstalled()
    {
        var sys = Environment.GetFolderPath(Environment.SpecialFolder.SystemX86);
        return File.Exists(Path.Combine(sys, "dpwsockx.dll"));
    }
}
