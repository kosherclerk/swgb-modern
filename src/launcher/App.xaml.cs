using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace SWGBModern;

internal sealed class SyncProgress(Action<string> report) : IProgress<string>
{
    public void Report(string value) => report(value);
}

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // Some GPU drivers fail WPF's hardware path (blank white windows); the
        // launcher UI is simple, so software rendering is always fast enough.
        if (Environment.GetEnvironmentVariable("SWGBM_HWRENDER") != "1")
            RenderOptions.ProcessRenderMode = RenderMode.SoftwareOnly;
        DispatcherUnhandledException += (_, ex) =>
        {
            MessageBox.Show(ex.Exception.ToString(), "SWGB Modern - unexpected error", MessageBoxButton.OK, MessageBoxImage.Error);
            ex.Handled = true;
        };
        // Diagnostics: SWGBModern.exe --extract-menus <game dir> <out dir>
        if (e.Args.Length == 3 && e.Args[0] == "--extract-menus")
        {
            int code = 0;
            try { code = Core.HdAssets.ExtractMenuBackgrounds(e.Args[1], e.Args[2]) > 0 ? 0 : 2; } catch { code = 1; }
            Shutdown(code);
            return;
        }
        // Diagnostics: SWGBModern.exe --generate <game dir> menus|videos|convert <log file>
        if (e.Args.Length == 4 && e.Args[0] == "--generate")
        {
            var game = e.Args[1];
            var logPath = e.Args[3];
            var log = new Action<string>(s => { try { System.IO.File.AppendAllText(logPath, DateTime.Now.ToString("HH:mm:ss ") + s + Environment.NewLine); } catch { } });
            var prog = new SyncProgress(log);
            int code = 0;
            try
            {
                Task.Run(async () =>
                {
                    await Core.HdAssets.EnsureToolsAsync(e.Args[2] != "convert", prog, CancellationToken.None);
                    if (e.Args[2] == "menus") await Core.HdAssets.GenerateMenusAsync(game, prog, CancellationToken.None);
                    else if (e.Args[2] == "videos") await Core.HdAssets.GenerateHdVideosAsync(game, prog, CancellationToken.None);
                    else await Core.HdAssets.ConvertIntrosAsync(game, prog, CancellationToken.None);
                }).GetAwaiter().GetResult();
                log("done");
            }
            catch (Exception ex) { log("failed: " + ex); code = 1; }
            Shutdown(code);
            return;
        }
        // Diagnostics: SWGBModern.exe --install-pack <game dir> hd|intros <log file>
        if (e.Args.Length == 4 && e.Args[0] == "--install-pack")
        {
            var logPath = e.Args[3];
            var prog = new SyncProgress(s => { try { System.IO.File.AppendAllText(logPath, s + Environment.NewLine); } catch { } });
            int code = 0;
            try { Task.Run(() => Core.AssetPacks.InstallAsync(e.Args[1], e.Args[2] == "hd" ? Core.AssetPacks.Hd : Core.AssetPacks.Intros, prog, CancellationToken.None)).GetAwaiter().GetResult(); }
            catch (Exception ex) { prog.Report("failed: " + ex.Message); code = 1; }
            Shutdown(code);
            return;
        }
        base.OnStartup(e);
    }
}
