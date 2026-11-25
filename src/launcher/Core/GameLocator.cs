using System.IO;
using Microsoft.Win32;

namespace SWGBModern.Core;

public enum GameVariant { CloneCampaigns, BaseGame, ExpandingFronts }

public sealed record GameExecutable(GameVariant Variant, string Name, string ExePath)
{
    public override string ToString() => Name;
}

/// <summary>Finds a Galactic Battlegrounds installation (GOG, Steam, retail CD).</summary>
public static class GameLocator
{
    /// <summary>Returns the "Game" folder (the one holding battlegrounds_x1.exe / Battlegrounds.exe).</summary>
    public static string? Find(string? preferred)
    {
        foreach (var candidate in Candidates(preferred))
        {
            var game = Normalize(candidate);
            if (game != null) return game;
        }
        return null;
    }

    /// <summary>Accepts either the install root or its Game subfolder.</summary>
    public static string? Normalize(string? dir)
    {
        if (string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir)) return null;
        if (IsGameDir(dir)) return Path.GetFullPath(dir);
        var sub = Path.Combine(dir, "Game");
        if (IsGameDir(sub)) return Path.GetFullPath(sub);
        return null;
    }

    public static bool IsGameDir(string dir) =>
        File.Exists(Path.Combine(dir, "battlegrounds_x1.exe")) || File.Exists(Path.Combine(dir, "Battlegrounds.exe"));

    private static IEnumerable<string?> Candidates(string? preferred)
    {
        yield return preferred;
        string self = AppContext.BaseDirectory;
        yield return self;
        yield return Path.GetDirectoryName(self.TrimEnd('\\'));
        foreach (var key in new[]
                 {
                     @"SOFTWARE\WOW6432Node\GOG.com\Games\1421404646",
                     @"SOFTWARE\GOG.com\Games\1421404646",
                 })
        {
            yield return Read(Registry.LocalMachine, key, "path");
        }
        foreach (var key in new[]
                 {
                     @"SOFTWARE\WOW6432Node\LucasArts Entertainment Company LLC\Star Wars Galactic Battlegrounds: Clone Campaigns\1.0",
                     @"SOFTWARE\WOW6432Node\LucasArts Entertainment Company LLC\Star Wars Galactic Battlegrounds\1.0",
                     @"SOFTWARE\LucasArts Entertainment Company LLC\Star Wars Galactic Battlegrounds: Clone Campaigns\1.0",
                     @"SOFTWARE\LucasArts Entertainment Company LLC\Star Wars Galactic Battlegrounds\1.0",
                 })
        {
            var p = Read(Registry.LocalMachine, key, "Install Path");
            yield return p;
            if (p != null) yield return Path.GetDirectoryName(p);
        }
        // Steam (app 356500): library folders
        var steam = Read(Registry.CurrentUser, @"Software\Valve\Steam", "SteamPath");
        if (steam != null)
        {
            var libs = new List<string> { steam };
            var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
            if (File.Exists(vdf))
            {
                foreach (var line in File.ReadAllLines(vdf))
                {
                    var t = line.Trim();
                    if (t.StartsWith("\"path\"", StringComparison.OrdinalIgnoreCase))
                    {
                        var parts = t.Split('"', StringSplitOptions.RemoveEmptyEntries);
                        if (parts.Length >= 3) libs.Add(parts[^1].Replace(@"\\", @"\"));
                    }
                }
            }
            foreach (var lib in libs)
                yield return Path.Combine(lib, "steamapps", "common", "Star Wars Galactic Battlegrounds Saga");
        }
    }

    private static string? Read(RegistryKey root, string key, string value)
    {
        try
        {
            using var k = root.OpenSubKey(key);
            return k?.GetValue(value) as string;
        }
        catch
        {
            return null;
        }
    }

    public static List<GameExecutable> Executables(string gameDir)
    {
        var list = new List<GameExecutable>();
        void Add(GameVariant v, string name, string file)
        {
            var p = Path.Combine(gameDir, file);
            if (File.Exists(p)) list.Add(new GameExecutable(v, name, p));
        }
        Add(GameVariant.CloneCampaigns, "Clone Campaigns", "battlegrounds_x1.exe");
        Add(GameVariant.BaseGame, "Galactic Battlegrounds (base game)", "Battlegrounds.exe");
        Add(GameVariant.ExpandingFronts, "Expanding Fronts (community mod)", "battlegrounds_x2.exe");
        return list;
    }
}
