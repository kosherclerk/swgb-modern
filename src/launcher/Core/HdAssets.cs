using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace SWGBModern.Core;

/// <summary>
/// Generates the optional HD assets on the player's PC: menu backgrounds and
/// intro videos are upscaled once with Real-ESRGAN (x4plus model). FFmpeg is used
/// for video decode/encode, and also to convert the Indeo intros to H.264 when
/// the Indeo codec is not installed.
/// </summary>
public static class HdAssets
{
    public const string FfmpegUrl = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";
    public const string EsrganUrl = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
    public const string Model = "realesrgan-x4plus";
    public static readonly string[] Intros = { "xlogo1", "xintro" };

    public static string ToolsDir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SWGBModern", "tools");
    public static string FfmpegExe => Path.Combine(ToolsDir, "ffmpeg", "ffmpeg.exe");
    public static string EsrganExe => Path.Combine(ToolsDir, "esrgan", "realesrgan-ncnn-vulkan.exe");
    public static bool HaveFfmpeg => File.Exists(FfmpegExe);
    public static bool HaveEsrgan => File.Exists(EsrganExe) && File.Exists(Path.Combine(ToolsDir, "esrgan", "models", Model + ".bin"));

    public static string MenuDir(string game) => Path.Combine(game, "SWGBModern", "hd", "menu");
    public static string HdVideoDir(string game) => Path.Combine(game, "SWGBModern", "hd", "video");
    public static string ConvertedVideoDir(string game) => Path.Combine(game, "SWGBModern", "video");

    public static bool IndeoInstalled() =>
        File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.SystemX86), "ir50_32.dll"));

    public static int MenuCount(string game) => Directory.Exists(MenuDir(game)) ? Directory.GetFiles(MenuDir(game), "*.png").Length : 0;
    public static int HdVideoCount(string game) => Intros.Count(n => File.Exists(Path.Combine(HdVideoDir(game), n + ".mp4")));
    public static IEnumerable<string> IntroSources(string game) => Intros.Where(n => File.Exists(Path.Combine(game, n + ".avi")));
    public static bool ConvertedIntrosPresent(string game) =>
        IntroSources(game).All(n => File.Exists(Path.Combine(ConvertedVideoDir(game), n + ".mp4")));

    // ------------------------------------------------------------------ tools
    public static async Task EnsureToolsAsync(bool needEsrgan, IProgress<string> log, CancellationToken ct)
    {
        if (!HaveFfmpeg)
            await DownloadAndExtract(FfmpegUrl, "FFmpeg", Path.Combine(ToolsDir, "ffmpeg"),
                e => e.FullName.EndsWith("/bin/ffmpeg.exe", StringComparison.OrdinalIgnoreCase) ? "ffmpeg.exe" : null, log, ct);
        if (needEsrgan && !HaveEsrgan)
            await DownloadAndExtract(EsrganUrl, "Real-ESRGAN", Path.Combine(ToolsDir, "esrgan"), e =>
            {
                var n = e.FullName.Replace('\\', '/');
                if (n.EndsWith("/")) return null;
                if (n.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) || n.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)) return Path.GetFileName(n);
                if (n.Contains("models/")) return Path.Combine("models", Path.GetFileName(n));
                return null;
            }, log, ct);
        if (!HaveFfmpeg) throw new InvalidOperationException("FFmpeg could not be installed.");
        if (needEsrgan && !HaveEsrgan) throw new InvalidOperationException("Real-ESRGAN could not be installed.");
    }

    private static async Task DownloadAndExtract(string url, string name, string dest, Func<ZipArchiveEntry, string?> map,
                                                 IProgress<string> log, CancellationToken ct)
    {
        var dl = Path.Combine(ToolsDir, "dl");
        Directory.CreateDirectory(dl);
        var zip = Path.Combine(dl, name + ".zip");
        using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) })
        {
            http.DefaultRequestHeaders.UserAgent.ParseAdd("SWGBModern-Launcher");
            using var resp = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct);
            resp.EnsureSuccessStatusCode();
            long total = resp.Content.Headers.ContentLength ?? -1, done = 0;
            await using var src = await resp.Content.ReadAsStreamAsync(ct);
            await using var dst = File.Create(zip);
            var buf = new byte[1 << 16];
            int n, lastPct = -1;
            while ((n = await src.ReadAsync(buf, ct)) > 0)
            {
                await dst.WriteAsync(buf.AsMemory(0, n), ct);
                done += n;
                int pct = total > 0 ? (int)(done * 100 / total) : -1;
                if (pct != lastPct) { lastPct = pct; log.Report(total > 0 ? $"Downloading {name}... {pct}% of {total / 1048576} MB" : $"Downloading {name}... {done / 1048576} MB"); }
            }
        }
        log.Report($"Extracting {name}...");
        await Task.Run(() =>
        {
            Directory.CreateDirectory(dest);
            using var z = ZipFile.OpenRead(zip);
            foreach (var e in z.Entries)
            {
                var rel = map(e);
                if (rel == null) continue;
                var outPath = Path.GetFullPath(Path.Combine(dest, rel));
                if (!outPath.StartsWith(Path.GetFullPath(dest), StringComparison.OrdinalIgnoreCase)) continue;
                Directory.CreateDirectory(Path.GetDirectoryName(outPath)!);
                e.ExtractToFile(outPath, overwrite: true);
            }
        }, ct);
        try { File.Delete(zip); } catch { }
    }

    // ------------------------------------------------------------------ menus
    /// <summary>Extracts every menu background (as listed by the .sin screen
    /// descriptions) to PNG, named by the CRC32 of the SLP the game loads.</summary>
    public static int ExtractMenuBackgrounds(string game, string outDir)
    {
        Directory.CreateDirectory(outDir);
        var archives = new[] { "INTERFAC.DRS", "interfac_x1.drs" }
            .Select(n => Path.Combine(game, "Data", n)).Where(File.Exists).Select(ReadDrs).ToList();
        var all = new Dictionary<(string, int), byte[]>();
        foreach (var a in archives) foreach (var kv in a) all[kv.Key] = kv.Value;
        var done = new HashSet<uint>();
        var seen = new HashSet<uint>();
        foreach (var a in archives)
        {
            foreach (var ((ext, _), data) in a)
            {
                if (ext != "bina") continue;
                var txt = Encoding.Latin1.GetString(data);
                if (!txt.Contains("background1_files")) continue;
                var fields = new Dictionary<string, string[]>();
                foreach (var line in txt.Replace("\r", "").Split('\n'))
                {
                    var p = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                    if (p.Length > 0) fields[p[0]] = p[1..];
                }
                if (!fields.TryGetValue("background1_files", out var bg) || bg.Length < 3 || bg[2] == "-1") continue;
                if (!fields.TryGetValue("palette_file", out var pal) || pal.Length < 2) continue;
                if (!int.TryParse(bg[2], out var slpId) || !int.TryParse(pal[1], out var palId)) continue;
                foreach (var arch in archives)
                {
                    if (!arch.TryGetValue(("slp", slpId), out var sd) || !all.TryGetValue(("bina", palId), out var pd)) continue;
                    uint crc = Crc32(sd);
                    if (!seen.Add(crc)) continue;
                    var (w, h, px) = SlpFrame0(sd);
                    if (w < 320) continue;
                    done.Add(crc);
                    var palette = JascPalette(pd);
                    var bgra = new byte[w * h * 4];
                    for (int i = 0; i < w * h; i++)
                    {
                        int v = px[i];
                        if (v < 0 || v >= palette.Length) continue;
                        var c = palette[v];
                        bgra[i * 4] = c.B; bgra[i * 4 + 1] = c.G; bgra[i * 4 + 2] = c.R; bgra[i * 4 + 3] = 255;
                    }
                    SavePng(Path.Combine(outDir, $"{crc:x8}.png"), w, h, bgra);
                }
            }
        }
        return done.Count;
    }

    private static Dictionary<(string, int), byte[]> ReadDrs(string path)
    {
        var f = File.ReadAllBytes(path);
        int ntab = BitConverter.ToInt32(f, 0x4c);
        var res = new Dictionary<(string, int), byte[]>();
        for (int t = 0; t < ntab; t++)
        {
            int b = 0x54 + t * 12;
            var ext = new string(Encoding.ASCII.GetString(f, b, 4).Reverse().ToArray()).Trim();
            int off = BitConverter.ToInt32(f, b + 4), num = BitConverter.ToInt32(f, b + 8);
            for (int i = 0; i < num; i++)
            {
                int e = off + i * 12;
                int id = BitConverter.ToInt32(f, e), o = BitConverter.ToInt32(f, e + 4), s = BitConverter.ToInt32(f, e + 8);
                if (o < 0 || s < 0 || o + s > f.Length) continue;
                res[(ext, id)] = f.AsSpan(o, s).ToArray();
            }
        }
        return res;
    }

    private static (int w, int h, short[] px) SlpFrame0(byte[] d)
    {
        int cmdo = BitConverter.ToInt32(d, 32), outo = BitConverter.ToInt32(d, 36);
        int w = BitConverter.ToInt32(d, 48), h = BitConverter.ToInt32(d, 52);
        var img = new short[w * h];
        Array.Fill(img, (short)-1);
        for (int y = 0; y < h; y++)
        {
            short l = BitConverter.ToInt16(d, outo + y * 4), r = BitConverter.ToInt16(d, outo + y * 4 + 2);
            if (l == -32768 || r == -32768) continue;
            int p = BitConverter.ToInt32(d, cmdo + y * 4), x = l;
            void Put(int v) { if (x >= 0 && x < w) img[y * w + x] = (short)v; x++; }
            while (p < d.Length)
            {
                int c = d[p++], lo2 = c & 3, lo4 = c & 0xF;
                if (c == 0x0F) break;
                if (lo2 == 0) { int n = c >> 2; for (int i = 0; i < n; i++) Put(d[p++]); }
                else if (lo2 == 1) x += c >> 2;
                else if (lo4 == 2) { int n = ((c & 0xF0) << 4) + d[p++]; for (int i = 0; i < n; i++) Put(d[p++]); }
                else if (lo4 == 3) x += ((c & 0xF0) << 4) + d[p++];
                else if (lo4 is 6 or 7 or 0xA or 0xB)
                {
                    int n = c >> 4;
                    if (n == 0) n = d[p++];
                    if (lo4 == 6) for (int i = 0; i < n; i++) Put(d[p++] + 16);
                    else if (lo4 == 7) { int v = d[p++]; for (int i = 0; i < n; i++) Put(v); }
                    else if (lo4 == 0xA) { int v = d[p++] + 16; for (int i = 0; i < n; i++) Put(v); }
                    else x += n;
                }
                else if (lo4 == 0xE)
                {
                    if (c is 0x4E or 0x6E) x += 1;
                    else if (c is 0x5E or 0x7E) x += d[p++];
                }
                else throw new InvalidDataException($"SLP command {c:x2}");
            }
        }
        return (w, h, img);
    }

    private static Color[] JascPalette(byte[] data)
    {
        var lines = Encoding.Latin1.GetString(data).Replace("\r", "").Split('\n');
        int n = int.Parse(lines[2].Trim());
        var pal = new Color[n];
        for (int i = 0; i < n; i++)
        {
            var p = lines[3 + i].Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            pal[i] = Color.FromRgb(byte.Parse(p[0]), byte.Parse(p[1]), byte.Parse(p[2]));
        }
        return pal;
    }

    private static void SavePng(string path, int w, int h, byte[] bgra)
    {
        var bmp = BitmapSource.Create(w, h, 96, 96, PixelFormats.Bgra32, null, bgra, w * 4);
        var enc = new PngBitmapEncoder();
        enc.Frames.Add(BitmapFrame.Create(bmp));
        using var fs = File.Create(path);
        enc.Save(fs);
    }

    private static uint Crc32(byte[] d)
    {
        var table = new uint[256];
        for (uint i = 0; i < 256; i++)
        {
            uint c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        uint crc = 0xFFFFFFFFu;
        foreach (var b in d) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    // ------------------------------------------------------------------ processes
    private static async Task<string> Run(string exe, string args, CancellationToken ct, Action<string>? onLine = null)
    {
        var psi = new ProcessStartInfo(exe, args)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true,
            WorkingDirectory = Path.GetDirectoryName(exe)!,
        };
        using var p = new Process { StartInfo = psi };
        var sb = new StringBuilder();
        void Line(string? s) { if (s == null) return; lock (sb) sb.AppendLine(s); onLine?.Invoke(s); }
        p.OutputDataReceived += (_, e) => Line(e.Data);
        p.ErrorDataReceived += (_, e) => Line(e.Data);
        p.Start();
        p.BeginErrorReadLine();
        p.BeginOutputReadLine();
        try { await p.WaitForExitAsync(ct); }
        catch (OperationCanceledException) { try { p.Kill(true); } catch { } throw; }
        if (p.ExitCode != 0)
        {
            var tail = sb.ToString().Trim();
            if (tail.Length > 600) tail = tail[^600..];
            throw new InvalidOperationException($"{Path.GetFileName(exe)} failed ({p.ExitCode}):\n{tail}");
        }
        return sb.ToString();
    }

    /// <summary>Upscales every PNG in inDir; reports (done, total) by watching the output folder.</summary>
    private static async Task Upscale(string inDir, string outDir, CancellationToken ct, Action<int, int> progress)
    {
        Directory.CreateDirectory(outDir);
        int total = Directory.GetFiles(inDir, "*.png").Length;
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var watcher = Task.Run(async () =>
        {
            int last = -1;
            while (!stop.IsCancellationRequested)
            {
                int n = Directory.GetFiles(outDir, "*.png").Length;
                if (n != last) { last = n; progress(n, total); }
                try { await Task.Delay(1000, stop.Token); } catch (OperationCanceledException) { break; }
            }
        });
        try { await Run(EsrganExe, $"-i \"{inDir}\" -o \"{outDir}\" -n {Model} -s 4 -f png", ct); }
        finally { stop.Cancel(); await watcher; }
    }

    private static string WorkDir()
    {
        var d = Path.Combine(Path.GetTempPath(), "SWGBModern-hd-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(d);
        return d;
    }

    // ------------------------------------------------------------------ jobs
    public static async Task GenerateMenusAsync(string game, IProgress<string> log, CancellationToken ct)
    {
        var work = WorkDir();
        try
        {
            log.Report("Extracting menu backgrounds...");
            var src = Path.Combine(work, "src");
            int n = await Task.Run(() => ExtractMenuBackgrounds(game, src), ct);
            if (n == 0) throw new InvalidOperationException("No menu backgrounds found in Data\\INTERFAC.DRS.");
            log.Report($"Upscaling {n} menu backgrounds (AI x4)...");
            var hd = Path.Combine(work, "hd");
            await Upscale(src, hd, ct, (d, t) => log.Report($"Upscaling menu backgrounds (AI x4)... {d}/{t}"));
            var dst = MenuDir(game);
            Directory.CreateDirectory(dst);
            foreach (var f in Directory.GetFiles(hd, "*.png")) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), true);
            log.Report($"{n} HD menu backgrounds ready.");
        }
        finally { TryDelete(work); }
    }

    private static double FrameRate(string probeOutput)
    {
        var m = Regex.Match(probeOutput, @"([\d.]+) (fps|tbr)");
        return m.Success && double.TryParse(m.Groups[1].Value, System.Globalization.NumberStyles.Float,
                                            System.Globalization.CultureInfo.InvariantCulture, out var r) && r > 0 ? r : 15;
    }

    private static async Task<string> Probe(string file, CancellationToken ct)
    {
        try { return await Run(FfmpegExe, $"-hide_banner -i \"{file}\"", ct); }
        catch (InvalidOperationException e) { return e.Message; } // ffmpeg -i without output exits 1 after printing the info
    }

    public static async Task GenerateHdVideosAsync(string game, IProgress<string> log, CancellationToken ct)
    {
        foreach (var name in IntroSources(game).ToList())
        {
            var avi = Path.Combine(game, name + ".avi");
            var work = WorkDir();
            try
            {
                var fps = FrameRate(await Probe(avi, ct));
                log.Report($"{name}: extracting frames...");
                var frames = Path.Combine(work, "f");
                Directory.CreateDirectory(frames);
                await Run(FfmpegExe, $"-hide_banner -loglevel error -i \"{avi}\" -fps_mode passthrough \"{Path.Combine(frames, "%06d.png")}\"", ct);
                int total = Directory.GetFiles(frames, "*.png").Length;
                log.Report($"{name}: upscaling {total} frames (AI x4) - this can take a while...");
                var up = Path.Combine(work, "u");
                await Upscale(frames, up, ct, (d, t) => log.Report($"{name}: upscaling frames (AI x4)... {d}/{t}"));
                log.Report($"{name}: encoding...");
                var dstDir = HdVideoDir(game);
                Directory.CreateDirectory(dstDir);
                var tmp = Path.Combine(work, name + ".mp4");
                var rate = fps.ToString(System.Globalization.CultureInfo.InvariantCulture);
                await Run(FfmpegExe, $"-hide_banner -loglevel error -y -framerate {rate} -i \"{Path.Combine(up, "%06d.png")}\" -i \"{avi}\" " +
                                     "-map 0:v -map 1:a? -c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart " +
                                     $"\"{tmp}\"", ct);
                File.Copy(tmp, Path.Combine(dstDir, name + ".mp4"), true);
                log.Report($"{name}: HD video ready.");
            }
            finally { TryDelete(work); }
        }
    }

    /// <summary>Converts the Indeo intros to H.264 at original size (used when the
    /// Indeo codec is missing from Windows).</summary>
    public static async Task ConvertIntrosAsync(string game, IProgress<string> log, CancellationToken ct)
    {
        var dstDir = ConvertedVideoDir(game);
        Directory.CreateDirectory(dstDir);
        foreach (var name in IntroSources(game).ToList())
        {
            log.Report($"{name}: converting (Indeo codec not installed)...");
            var tmp = Path.Combine(dstDir, name + ".tmp.mp4");
            await Run(FfmpegExe, $"-hide_banner -loglevel error -y -i \"{Path.Combine(game, name + ".avi")}\" " +
                                 $"-c:v libx264 -crf 16 -pix_fmt yuv420p -c:a aac -b:a 192k \"{tmp}\"", ct);
            File.Move(tmp, Path.Combine(dstDir, name + ".mp4"), true);
        }
    }

    private static void TryDelete(string dir) { try { Directory.Delete(dir, true); } catch { } }
}
