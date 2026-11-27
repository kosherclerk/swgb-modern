using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Text.Json;

namespace SWGBModern.Core;

public sealed record ReleaseInfo(string Tag, Version Version, string Notes, string DownloadUrl, string PageUrl, DateTime Published = default);

/// <summary>Checks GitHub Releases and replaces the launcher executable in place.</summary>
public static class Updater
{
    /// <summary>GitHub "owner/repo" the launcher updates from.</summary>
    public const string Repository = "kosherclerk/swgb-modern";
    public const string AssetName = "SWGBModern.exe";

    private static HttpClient Http()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("SWGBModern-Launcher");
        c.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return c;
    }

    public static Version Current => typeof(Updater).Assembly.GetName().Version ?? new Version(0, 0, 0);

    public static async Task<ReleaseInfo?> LatestAsync()
    {
        using var http = Http();
        var json = await http.GetStringAsync($"https://api.github.com/repos/{Repository}/releases/latest");
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        string tag = root.GetProperty("tag_name").GetString() ?? "";
        if (!Version.TryParse(tag.TrimStart('v', 'V'), out var ver)) return null;
        string url = "";
        foreach (var a in root.GetProperty("assets").EnumerateArray())
            if (string.Equals(a.GetProperty("name").GetString(), AssetName, StringComparison.OrdinalIgnoreCase))
                url = a.GetProperty("browser_download_url").GetString() ?? "";
        return new ReleaseInfo(tag, ver, root.TryGetProperty("body", out var b) ? b.GetString() ?? "" : "",
                               url, root.TryGetProperty("html_url", out var h) ? h.GetString() ?? "" : "");
    }

    /// <summary>Every published release that has a launcher download, newest first.</summary>
    public static async Task<List<ReleaseInfo>> AllAsync()
    {
        using var http = Http();
        var json = await http.GetStringAsync($"https://api.github.com/repos/{Repository}/releases?per_page=100");
        using var doc = JsonDocument.Parse(json);
        var list = new List<ReleaseInfo>();
        foreach (var r in doc.RootElement.EnumerateArray())
        {
            if (r.TryGetProperty("draft", out var d) && d.GetBoolean()) continue;
            string tag = r.GetProperty("tag_name").GetString() ?? "";
            if (!Version.TryParse(tag.TrimStart('v', 'V'), out var ver)) continue;
            string url = "";
            foreach (var a in r.GetProperty("assets").EnumerateArray())
                if (string.Equals(a.GetProperty("name").GetString(), AssetName, StringComparison.OrdinalIgnoreCase))
                    url = a.GetProperty("browser_download_url").GetString() ?? "";
            if (url.Length == 0) continue;
            DateTime.TryParse(r.TryGetProperty("published_at", out var p) ? p.GetString() : null, out var when);
            list.Add(new ReleaseInfo(tag, ver, r.TryGetProperty("body", out var b) ? b.GetString() ?? "" : "", url,
                                     r.TryGetProperty("html_url", out var h) ? h.GetString() ?? "" : "", when));
        }
        return list.OrderByDescending(x => x.Version).ToList();
    }

    public static bool SameVersion(ReleaseInfo r) =>
        new Version(r.Version.Major, r.Version.Minor, Math.Max(0, r.Version.Build)) ==
        new Version(Current.Major, Current.Minor, Math.Max(0, Current.Build));

    public static bool IsNewer(ReleaseInfo r) =>
        new Version(r.Version.Major, r.Version.Minor, Math.Max(0, r.Version.Build)) >
        new Version(Current.Major, Current.Minor, Math.Max(0, Current.Build));

    /// <summary>
    /// Downloads the given launcher (newer or older) next to the current one,
    /// swaps the files and restarts. The started launcher then installs its own
    /// DLLs into the game.
    /// </summary>
    public static async Task DownloadAndRestartAsync(ReleaseInfo r, IProgress<double>? progress)
    {
        if (string.IsNullOrEmpty(r.DownloadUrl)) throw new InvalidOperationException("The release has no SWGBModern.exe asset.");
        string self = Environment.ProcessPath ?? throw new InvalidOperationException("Cannot locate the launcher executable.");
        string dir = Path.GetDirectoryName(self)!;
        string tmp = Path.Combine(dir, "SWGBModern.update.tmp");
        using (var http = Http())
        using (var resp = await http.GetAsync(r.DownloadUrl, HttpCompletionOption.ResponseHeadersRead))
        {
            resp.EnsureSuccessStatusCode();
            long total = resp.Content.Headers.ContentLength ?? -1;
            await using var src = await resp.Content.ReadAsStreamAsync();
            await using var dst = File.Create(tmp);
            var buf = new byte[81920];
            long done = 0;
            int n;
            while ((n = await src.ReadAsync(buf)) > 0)
            {
                await dst.WriteAsync(buf.AsMemory(0, n));
                done += n;
                if (total > 0) progress?.Report((double)done / total);
            }
        }
        string old = self + ".old";
        if (File.Exists(old)) File.Delete(old);
        File.Move(self, old);          // a running exe can be renamed, not overwritten
        File.Move(tmp, self);
        Process.Start(new ProcessStartInfo(self, "--updated") { UseShellExecute = false });
        System.Windows.Application.Current.Shutdown();
    }

    /// <summary>Removes the previous launcher left behind by an update.</summary>
    public static void CleanupAfterUpdate()
    {
        try
        {
            var old = (Environment.ProcessPath ?? "") + ".old";
            for (int i = 0; i < 20 && File.Exists(old); i++)
            {
                try { File.Delete(old); } catch { Thread.Sleep(250); }
            }
        }
        catch { }
    }
}
