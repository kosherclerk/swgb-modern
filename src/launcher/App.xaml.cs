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
    private static Mutex? _single;   // held for the launcher's lifetime

    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr h);
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr h, int cmd);
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern bool IsIconic(IntPtr h);

    /// <summary>
    /// One launcher at a time. A second start brings the running one to the
    /// front and exits. After a self-update/version change the new launcher is
    /// started before the old one has closed, so it waits for it.
    /// </summary>
    private static bool TakeSingleInstance(bool afterUpdate)
    {
        _single = new Mutex(false, @"Local\SWGBModern.Launcher.7d2c41e0");
        try
        {
            if (_single.WaitOne(afterUpdate ? TimeSpan.FromSeconds(15) : TimeSpan.Zero)) return true;
        }
        catch (AbandonedMutexException) { return true; }   // previous launcher ended without releasing it
        try
        {
            var me = System.Diagnostics.Process.GetCurrentProcess();
            foreach (var p in System.Diagnostics.Process.GetProcessesByName(me.ProcessName))
            {
                if (p.Id == me.Id || p.MainWindowHandle == IntPtr.Zero) continue;
                if (IsIconic(p.MainWindowHandle)) ShowWindow(p.MainWindowHandle, 9 /*SW_RESTORE*/);
                SetForegroundWindow(p.MainWindowHandle);
                break;
            }
        }
        catch { }
        return false;
    }

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
        if (!TakeSingleInstance(e.Args.Contains("--updated"))) { Shutdown(0); return; }
        base.OnStartup(e);
    }
}
