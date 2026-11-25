using System.IO;
using System.Runtime.InteropServices;

namespace SWGBModern.Core;

public static class SystemInfo
{
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
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

    /// <summary>Physical resolution and refresh rate of the primary monitor.</summary>
    public static (int Width, int Height, int Hz) PrimaryMonitor()
    {
        var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf<DEVMODE>() };
        if (EnumDisplaySettingsW(null, -1 /*ENUM_CURRENT_SETTINGS*/, ref dm) && dm.dmPelsWidth > 0)
            return (dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
        return (GetSystemMetrics(0), GetSystemMetrics(1), 60);
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
