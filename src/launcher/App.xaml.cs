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
        // Diagnostics: SWGBModern.exe --convert-intros <game dir> <log file>
        if (e.Args.Length == 3 && e.Args[0] == "--convert-intros")
        {
            var logPath = e.Args[2];
            var prog = new SyncProgress(t => { try { System.IO.File.AppendAllText(logPath, t + Environment.NewLine); } catch { } });
            int code = 0;
            try
            {
                Task.Run(async () =>
                {
                    await Core.Intros.EnsureFfmpegAsync(prog, CancellationToken.None);
                    await Core.Intros.ConvertAsync(e.Args[1], prog, CancellationToken.None);
                }).GetAwaiter().GetResult();
                prog.Report("done");
            }
            catch (Exception ex) { prog.Report("failed: " + ex.Message); code = 1; }
            Shutdown(code);
            return;
        }
        base.OnStartup(e);
    }
}
