using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace SWGBModern.Core;

public enum InstallState { NotInstalled, Installed, Outdated, Damaged }

public sealed class Manifest
{
    public string Version { get; set; } = "";
    public DateTime InstalledAt { get; set; }
    /// <summary>Installed file name (relative to Game dir) -> SHA-256.</summary>
    public Dictionary<string, string> Files { get; set; } = new();
    /// <summary>Files that existed before installation and were moved to the backup folder.</summary>
    public List<string> BackedUp { get; set; } = new();
    /// <summary>Generated helper files (e.g. 4 GB executable copies).</summary>
    public List<string> Generated { get; set; } = new();
}

/// <summary>
/// Installs the embedded mod DLLs into the game folder. Nothing of the
/// original game is modified: files the mod replaces (e.g. another ddraw.dll
/// wrapper) are moved to SWGBModern\backup and restored on uninstall.
/// </summary>
public sealed class Installer
{
    public static readonly string[] PayloadFiles = { "ddraw.dll", "dsound.dll" };
    public const string IniName = "SWGBModern.ini";
    public string GameDir { get; }
    private string ModDir => Path.Combine(GameDir, "SWGBModern");
    private string ManifestPath => Path.Combine(ModDir, "manifest.json");
    private string BackupDir => Path.Combine(ModDir, "backup");

    public Installer(string gameDir) { GameDir = gameDir; }

    public static string Version =>
        Assembly.GetExecutingAssembly().GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "0.0.0";

    public static byte[]? Payload(string name)
    {
        using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload/" + name);
        if (s == null) return null;
        using var ms = new MemoryStream();
        s.CopyTo(ms);
        return ms.ToArray();
    }

    private static string Sha(byte[] data) => Convert.ToHexString(SHA256.HashData(data));
    private static string ShaFile(string path) => File.Exists(path) ? Sha(File.ReadAllBytes(path)) : "";

    public Manifest? LoadManifest()
    {
        try { return File.Exists(ManifestPath) ? JsonSerializer.Deserialize<Manifest>(File.ReadAllText(ManifestPath)) : null; }
        catch { return null; }
    }

    private void SaveManifest(Manifest m)
    {
        Directory.CreateDirectory(ModDir);
        File.WriteAllText(ManifestPath, JsonSerializer.Serialize(m, new JsonSerializerOptions { WriteIndented = true }));
    }

    public InstallState State()
    {
        var m = LoadManifest();
        if (m == null) return InstallState.NotInstalled;
        foreach (var (file, hash) in m.Files)
            if (ShaFile(Path.Combine(GameDir, file)) != hash) return InstallState.Damaged;
        foreach (var f in PayloadFiles)
        {
            var p = Payload(f);
            if (p != null && (!m.Files.TryGetValue(f, out var h) || h != Sha(p))) return InstallState.Outdated;
        }
        return InstallState.Installed;
    }

    public void Install()
    {
        var m = LoadManifest() ?? new Manifest();
        Directory.CreateDirectory(BackupDir);
        foreach (var f in PayloadFiles)
        {
            var data = Payload(f) ?? throw new InvalidOperationException($"This launcher build does not contain {f}.");
            var target = Path.Combine(GameDir, f);
            bool ours = m.Files.ContainsKey(f);
            if (File.Exists(target) && !ours && !m.BackedUp.Contains(f))
            {
                File.Copy(target, Path.Combine(BackupDir, f), true);
                m.BackedUp.Add(f);
            }
            WriteWithRetry(target, data);
            m.Files[f] = Sha(data);
        }
        var ini = Path.Combine(GameDir, IniName);
        if (!File.Exists(ini) && Payload(IniName) is { } defIni) File.WriteAllBytes(ini, defIni);
        m.Version = Version;
        m.InstalledAt = DateTime.Now;
        SaveManifest(m);
    }

    public void Uninstall(bool keepSettings)
    {
        var m = LoadManifest();
        if (m != null)
        {
            foreach (var f in m.Files.Keys) TryDelete(Path.Combine(GameDir, f));
            foreach (var g in m.Generated) TryDelete(Path.Combine(GameDir, g));
            foreach (var b in m.BackedUp)
            {
                var src = Path.Combine(BackupDir, b);
                if (File.Exists(src)) File.Copy(src, Path.Combine(GameDir, b), true);
            }
        }
        foreach (var f in new[] { "SWGBModern.log", "SWGBModern_frame.bmp" }) TryDelete(Path.Combine(GameDir, f));
        if (!keepSettings) TryDelete(Path.Combine(GameDir, IniName));
        try { if (Directory.Exists(ModDir)) Directory.Delete(ModDir, true); } catch { }
        try { var cd = Path.Combine(GameDir, "CrashDumps"); if (Directory.Exists(cd) && !keepSettings) Directory.Delete(cd, true); } catch { }
    }

    /// <summary>
    /// Creates "&lt;exe&gt;.4gb.exe": an identical copy with the Large Address Aware
    /// header flag, so the 32-bit game can use 4 GB instead of 2 GB on 64-bit
    /// Windows. The original executable is never modified.
    /// </summary>
    public string LargeAddressAwareCopy(string exePath)
    {
        string copy = Path.Combine(Path.GetDirectoryName(exePath)!, Path.GetFileNameWithoutExtension(exePath) + ".4gb.exe");
        bool stale = !File.Exists(copy) || File.GetLastWriteTimeUtc(copy) < File.GetLastWriteTimeUtc(exePath);
        if (stale)
        {
            var data = File.ReadAllBytes(exePath);
            int pe = BitConverter.ToInt32(data, 0x3c);
            ushort ch = BitConverter.ToUInt16(data, pe + 22);
            ch |= 0x20; // IMAGE_FILE_LARGE_ADDRESS_AWARE
            BitConverter.GetBytes(ch).CopyTo(data, pe + 22);
            WriteWithRetry(copy, data);
        }
        var m = LoadManifest() ?? new Manifest();
        var name = Path.GetFileName(copy);
        if (!m.Generated.Contains(name)) { m.Generated.Add(name); SaveManifest(m); }
        return copy;
    }

    private static void WriteWithRetry(string path, byte[] data)
    {
        for (int i = 0; ; i++)
        {
            try { File.WriteAllBytes(path, data); return; }
            catch (IOException) when (i < 10) { Thread.Sleep(300); }
        }
    }

    private static void TryDelete(string p) { try { if (File.Exists(p)) File.Delete(p); } catch { } }
}
