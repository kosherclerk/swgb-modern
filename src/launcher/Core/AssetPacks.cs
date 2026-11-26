using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;

namespace SWGBModern.Core;

/// <summary>
/// Optional content made once by the maintainers and served from the GitHub
/// release "assets-1" of the assets repository: AI-upscaled menu backgrounds
/// (WebP) and intro videos (H.264), and H.264 copies of the intros for PCs
/// without the Indeo codec. Each pack is verified by SHA-256 and extracted into
/// Game\SWGBModern. The assets repository is private for now; downloads then need
/// a GitHub token with access to it in SWGBM_GITHUB_TOKEN.
/// </summary>
public static class AssetPacks
{
    public sealed record Pack(string Id, string FileName, long Size, string Sha256, string Description);

    public const string Repository = "kosherclerk/swgb-modern-assets";
    public const string ReleaseTag = "assets-1";

    public static readonly Pack Hd = new("hd", "swgbm-hd-pack.zip", 28343742,
        "14846e7313b700e2b2c3614989a02b81537bc58ae699d6889eca9996ce6c5197",
        "HD menu backgrounds and intro videos");

    public static readonly Pack Intros = new("intros", "swgbm-intros.zip", 4688975,
        "9c2e2c06fa8aaca6c35da784f6fae92d2db610dc6922b3c673e36e29c117cb43",
        "intro videos without the Indeo codec");

    // SWGBM_ASSETS_BASE overrides the download location (testing a pack before it is published)
    public static string Url(Pack p) =>
        (Environment.GetEnvironmentVariable("SWGBM_ASSETS_BASE") is { Length: > 0 } b ? b.TrimEnd('/')
            : $"https://github.com/{Repository}/releases/download/{ReleaseTag}") + "/" + p.FileName;

    private static string? Token => Environment.GetEnvironmentVariable("SWGBM_GITHUB_TOKEN") is { Length: > 0 } t ? t : null;

    /// <summary>Private repository: resolve the asset through the API with the token.</summary>
    private static async Task<HttpRequestMessage> Request(HttpClient http, Pack p, CancellationToken ct)
    {
        if (Token is not { } token || Environment.GetEnvironmentVariable("SWGBM_ASSETS_BASE") is { Length: > 0 })
            return new HttpRequestMessage(HttpMethod.Get, Url(p));
        using var rel = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/{Repository}/releases/tags/{ReleaseTag}");
        rel.Headers.Authorization = new("Bearer", token);
        using var r = await http.SendAsync(rel, ct);
        r.EnsureSuccessStatusCode();
        using var doc = System.Text.Json.JsonDocument.Parse(await r.Content.ReadAsStringAsync(ct));
        var asset = doc.RootElement.GetProperty("assets").EnumerateArray().First(a => a.GetProperty("name").GetString() == p.FileName);
        var req = new HttpRequestMessage(HttpMethod.Get, asset.GetProperty("url").GetString());
        req.Headers.Authorization = new("Bearer", token);
        req.Headers.Accept.ParseAdd("application/octet-stream");
        return req;
    }
    public static string SizeText(Pack p) => $"{p.Size / 1048576.0:0} MB";

    private static string ModDir(string game) => Path.Combine(game, "SWGBModern");
    private static string Marker(string game, Pack p) => Path.Combine(ModDir(game), $".pack-{p.Id}-{p.Sha256[..12]}");
    public static bool Installed(string game, Pack p) => File.Exists(Marker(game, p));

    public static bool IndeoInstalled() =>
        File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.SystemX86), "ir50_32.dll"));

    public static int MenuCount(string game)
    {
        var d = Path.Combine(ModDir(game), "hd", "menu");
        return Directory.Exists(d) ? Directory.GetFiles(d).Count(f => f.EndsWith(".webp") || f.EndsWith(".jpg") || f.EndsWith(".png")) : 0;
    }

    public static async Task InstallAsync(string game, Pack p, IProgress<string> log, CancellationToken ct)
    {
        var tmp = Path.Combine(Path.GetTempPath(), $"swgbm-{p.Id}-{Guid.NewGuid():N}.zip");
        try
        {
            using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) })
            {
                http.DefaultRequestHeaders.UserAgent.ParseAdd("SWGBModern-Launcher");
                using var req = await Request(http, p, ct);
                using var resp = await http.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct);
                if (resp.StatusCode == System.Net.HttpStatusCode.NotFound)
                    throw new InvalidOperationException("this download is not publicly available yet.");
                resp.EnsureSuccessStatusCode();
                long total = resp.Content.Headers.ContentLength ?? p.Size, done = 0;
                await using var src = await resp.Content.ReadAsStreamAsync(ct);
                await using var dst = File.Create(tmp);
                var buf = new byte[1 << 16];
                int n, last = -1;
                while ((n = await src.ReadAsync(buf, ct)) > 0)
                {
                    await dst.WriteAsync(buf.AsMemory(0, n), ct);
                    done += n;
                    int pct = (int)(done * 100 / Math.Max(1, total));
                    if (pct != last) { last = pct; log.Report($"Downloading {p.Description}... {pct}% of {SizeText(p)}"); }
                }
            }
            log.Report("Verifying...");
            string hash;
            await using (var fs = File.OpenRead(tmp)) hash = Convert.ToHexString(await SHA256.HashDataAsync(fs, ct)).ToLowerInvariant();
            if (hash != p.Sha256) throw new InvalidDataException("The downloaded file is damaged (checksum mismatch). Please try again.");
            log.Report("Installing...");
            var root = Path.GetFullPath(ModDir(game)) + Path.DirectorySeparatorChar;
            await Task.Run(() =>
            {
                using var z = ZipFile.OpenRead(tmp);
                foreach (var e in z.Entries)
                {
                    if (string.IsNullOrEmpty(e.Name)) continue;
                    var outPath = Path.GetFullPath(Path.Combine(root, e.FullName));
                    if (!outPath.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;
                    Directory.CreateDirectory(Path.GetDirectoryName(outPath)!);
                    e.ExtractToFile(outPath, overwrite: true);
                }
            }, ct);
            foreach (var old in Directory.GetFiles(ModDir(game), $".pack-{p.Id}-*")) File.Delete(old);
            File.WriteAllText(Marker(game, p), p.Sha256);
            log.Report($"Installed: {p.Description}.");
        }
        finally { try { File.Delete(tmp); } catch { } }
    }
}
