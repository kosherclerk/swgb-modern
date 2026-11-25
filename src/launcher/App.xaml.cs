using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace SWGBModern;

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
        base.OnStartup(e);
    }
}
