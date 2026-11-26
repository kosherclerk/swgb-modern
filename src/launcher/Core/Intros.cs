using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Text;

namespace SWGBModern.Core;

/// <summary>
/// The intro videos (xlogo1.avi, xintro.avi) use the Indeo 5 codec, which modern
/// Windows does not include. When it is missing, the launcher converts them once
/// with FFmpeg (downloaded on request into the user's folder) to H.264 at their
/// original size, into Game\SWGBModern\video; the mod plays those instead.
/// </summary>
public static class Intros
{
    public const string FfmpegUrl = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";
    public static readonly string[] Names = { "xlogo1", "xintro" };

    public static string ToolsDir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SWGBModern", "tools");
    public static string FfmpegExe => Path.Combine(ToolsDir, "ffmpeg", "ffmpeg.exe");
    public static bool HaveFfmpeg => File.Exists(FfmpegExe);
    public static string ConvertedDir(string game) => Path.Combine(game, "SWGBModern", "video");

    public static bool IndeoInstalled() =>
        File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.SystemX86), "ir50_32.dll"));
    public static IEnumerable<string> Sources(string game) => Names.Where(n => File.Exists(Path.Combine(game, n + ".avi")));
    public static bool Converted(string game) => Sources(game).All(n => File.Exists(Path.Combine(ConvertedDir(game), n + ".mp4")));
    public static bool NeedConversion(string game) => !IndeoInstalled() && Sources(game).Any() && !Converted(game);

    public static async Task EnsureFfmpegAsync(IProgress<string> log, CancellationToken ct)
    {
        if (HaveFfmpeg) return;
        var dl = Path.Combine(ToolsDir, "dl");
        Directory.CreateDirectory(dl);
        var zip = Path.Combine(dl, "ffmpeg.zip");
        using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) })
        {
            http.DefaultRequestHeaders.UserAgent.ParseAdd("SWGBModern-Launcher");
            using var resp = await http.GetAsync(FfmpegUrl, HttpCompletionOption.ResponseHeadersRead, ct);
            resp.EnsureSuccessStatusCode();
            long total = resp.Content.Headers.ContentLength ?? -1, done = 0;
            await using var src = await resp.Content.ReadAsStreamAsync(ct);
            await using var dst = File.Create(zip);
            var buf = new byte[1 << 16];
            int n, last = -1;
            while ((n = await src.ReadAsync(buf, ct)) > 0)
            {
                await dst.WriteAsync(buf.AsMemory(0, n), ct);
                done += n;
                int pct = total > 0 ? (int)(done * 100 / total) : -1;
                if (pct != last) { last = pct; log.Report(total > 0 ? $"Downloading FFmpeg... {pct}%" : $"Downloading FFmpeg... {done / 1048576} MB"); }
            }
        }
        log.Report("Extracting FFmpeg...");
        await Task.Run(() =>
        {
            var dest = Path.Combine(ToolsDir, "ffmpeg");
            Directory.CreateDirectory(dest);
            using var z = ZipFile.OpenRead(zip);
            var e = z.Entries.FirstOrDefault(x => x.FullName.EndsWith("/bin/ffmpeg.exe", StringComparison.OrdinalIgnoreCase))
                    ?? throw new InvalidDataException("ffmpeg.exe not found in the download.");
            e.ExtractToFile(Path.Combine(dest, "ffmpeg.exe"), overwrite: true);
        }, ct);
        try { File.Delete(zip); } catch { }
        if (!HaveFfmpeg) throw new InvalidOperationException("FFmpeg could not be installed.");
    }

    public static async Task ConvertAsync(string game, IProgress<string> log, CancellationToken ct)
    {
        var dstDir = ConvertedDir(game);
        Directory.CreateDirectory(dstDir);
        foreach (var name in Sources(game).ToList())
        {
            log.Report($"Converting intro video {name}...");
            var tmp = Path.Combine(dstDir, name + ".tmp.mp4");
            await Run(FfmpegExe, $"-hide_banner -loglevel error -y -i \"{Path.Combine(game, name + ".avi")}\" " +
                                 $"-c:v libx264 -crf 18 -pix_fmt yuv420p -c:a aac -b:a 128k \"{tmp}\"", ct);
            File.Move(tmp, Path.Combine(dstDir, name + ".mp4"), true);
        }
    }

    private static async Task Run(string exe, string args, CancellationToken ct)
    {
        var psi = new ProcessStartInfo(exe, args)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true,
            WorkingDirectory = Path.GetDirectoryName(exe)!,
        };
        using var p = new Process { StartInfo = psi };
        var sb = new StringBuilder();
        p.OutputDataReceived += (_, e) => { if (e.Data != null) lock (sb) sb.AppendLine(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (sb) sb.AppendLine(e.Data); };
        p.Start();
        p.BeginErrorReadLine();
        p.BeginOutputReadLine();
        try { await p.WaitForExitAsync(ct); }
        catch (OperationCanceledException) { try { p.Kill(true); } catch { } throw; }
        if (p.ExitCode != 0)
        {
            var tail = sb.ToString().Trim();
            if (tail.Length > 600) tail = tail[^600..];
            throw new InvalidOperationException($"ffmpeg failed ({p.ExitCode}):\n{tail}");
        }
    }
}
