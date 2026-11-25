using System.Diagnostics;
using System.IO;
using System.Windows;
using Microsoft.Win32;
using SWGBModern.Core;

namespace SWGBModern;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm = new();

    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    public MainWindow()
    {
        InitializeComponent();
        DataContext = _vm;
        SourceInitialized += (_, _) =>
        {
            int dark = 1; // DWMWA_USE_IMMERSIVE_DARK_MODE
            DwmSetWindowAttribute(new System.Windows.Interop.WindowInteropHelper(this).Handle, 20, ref dark, sizeof(int));
        };
        Loaded += async (_, _) =>
        {
            if (Environment.GetCommandLineArgs().Contains("--updated"))
            {
                Updater.CleanupAfterUpdate();
                TryInstall(silent: true);
                _vm.Status = $"Updated to {_vm.Version}.";
            }
            else if (_vm.CheckUpdates)
            {
                await CheckForUpdates(interactive: false);
            }
        };
        Closing += (_, _) => { try { _vm.SaveAll(); } catch { } };
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        try { _vm.SaveAll(); _vm.Status = "Settings saved."; }
        catch (Exception ex) { Error("Could not save settings", ex); }
    }

    private bool TryInstall(bool silent)
    {
        if (_vm.GameDir == null) { if (!silent) MessageBox.Show(this, "Choose the game folder first.", "SWGB Modern"); return false; }
        try
        {
            var inst = new Installer(_vm.GameDir);
            if (inst.State() != InstallState.Installed) inst.Install();
            _vm.SaveAll();
            _vm.RefreshInstallState();
            if (!silent) _vm.Status = "Mod installed.";
            return true;
        }
        catch (UnauthorizedAccessException)
        {
            MessageBox.Show(this, "Windows denied writing to the game folder. If the game is in Program Files, run the launcher as administrator once, or move the game to a folder you own.",
                            "SWGB Modern", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        catch (IOException ex) when (ex.HResult == unchecked((int)0x80070020))
        {
            MessageBox.Show(this, "The game is running. Close it and try again.", "SWGB Modern");
        }
        catch (Exception ex) { Error("Installation failed", ex); }
        return false;
    }

    private void Install_Click(object sender, RoutedEventArgs e)
    {
        if (_vm.GameDir == null) { TryInstall(false); return; }
        try { new Installer(_vm.GameDir).Install(); _vm.SaveAll(); _vm.RefreshInstallState(); _vm.Status = "Mod installed / repaired."; }
        catch (Exception ex) { Error("Installation failed", ex); }
    }

    private void Uninstall_Click(object sender, RoutedEventArgs e)
    {
        if (_vm.GameDir == null) return;
        var r = MessageBox.Show(this,
            "Remove SWGB Modern from this game?\n\nThe mod's DLLs and generated files are removed and any files it replaced are restored. " +
            "Choose 'Yes' to also delete your SWGB Modern settings, 'No' to keep them.",
            "Uninstall SWGB Modern", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
        if (r == MessageBoxResult.Cancel) return;
        try
        {
            new Installer(_vm.GameDir).Uninstall(keepSettings: r == MessageBoxResult.No);
            _vm.RefreshInstallState();
            _vm.Status = "SWGB Modern was removed. The game is back to its original state.";
        }
        catch (Exception ex) { Error("Uninstall failed", ex); }
    }

    private void Play_Click(object sender, RoutedEventArgs e)
    {
        if (_vm.SelectedGame == null) { MessageBox.Show(this, "No game executable found. Choose the game folder.", "SWGB Modern"); return; }
        if (!TryInstall(silent: true)) return;
        try
        {
            string exe = _vm.SelectedGame.ExePath;
            if (_vm.LargeAddressAware) exe = new Installer(_vm.GameDir!).LargeAddressAwareCopy(exe);
            var psi = new ProcessStartInfo(exe)
            {
                WorkingDirectory = Path.GetDirectoryName(exe)!,
                UseShellExecute = false,
                Arguments = _vm.SkipIntro ? "NOSTARTUP" : "",
            };
            Process.Start(psi);
            _vm.Status = $"Started {_vm.SelectedGame.Name}.";
            WindowState = WindowState.Minimized;
        }
        catch (Exception ex) { Error("Could not start the game", ex); }
    }

    private void Browse_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { Title = "Select the Star Wars Galactic Battlegrounds folder" };
        if (dlg.ShowDialog(this) != true) return;
        var dir = GameLocator.Normalize(dlg.FolderName);
        if (dir == null) { MessageBox.Show(this, "That folder does not contain battlegrounds_x1.exe or Battlegrounds.exe (or a Game subfolder with them).", "SWGB Modern"); return; }
        _vm.GameDir = dir;
        _vm.SaveAll();
    }

    private void OpenFolder_Click(object sender, RoutedEventArgs e) { if (_vm.GameDir != null) Shell(_vm.GameDir); }
    private void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        if (_vm.GameDir == null) return;
        var log = Path.Combine(_vm.GameDir, "SWGBModern.log");
        if (File.Exists(log)) Shell(log);
        else MessageBox.Show(this, "No log yet. Enable 'Write a diagnostic log' on the Fixes tab and run the game.", "SWGB Modern");
    }
    private void OpenDumps_Click(object sender, RoutedEventArgs e)
    {
        if (_vm.GameDir == null) return;
        var d = Path.Combine(_vm.GameDir, "CrashDumps");
        if (Directory.Exists(d)) Shell(d); else MessageBox.Show(this, "No crashes recorded.", "SWGB Modern");
    }
    private void Reset_Click(object sender, RoutedEventArgs e)
    {
        if (_vm.GameDir == null) return;
        if (MessageBox.Show(this, "Reset all SWGB Modern settings to defaults?", "SWGB Modern", MessageBoxButton.YesNo) != MessageBoxResult.Yes) return;
        var ini = Path.Combine(_vm.GameDir, Installer.IniName);
        if (Installer.Payload(Installer.IniName) is { } def) File.WriteAllBytes(ini, def);
        _vm.Reload();
        _vm.Status = "Settings reset.";
    }

    private void DirectPlay_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            var p = Process.Start(new ProcessStartInfo("dism.exe", "/online /enable-feature /featurename:DirectPlay /all /norestart")
            {
                UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Normal,
            });
            p?.WaitForExit();
            _vm.Status = SystemInfo.DirectPlayInstalled() ? "DirectPlay enabled." : "DirectPlay was not enabled.";
        }
        catch (System.ComponentModel.Win32Exception) { _vm.Status = "Cancelled."; }
    }

    private async void Update_Click(object sender, RoutedEventArgs e) => await CheckForUpdates(interactive: true);
    private void Project_Click(object sender, RoutedEventArgs e) => Shell($"https://github.com/{Updater.Repository}");

    private async Task CheckForUpdates(bool interactive)
    {
        try
        {
            _vm.Status = "Checking for updates...";
            var rel = await Updater.LatestAsync();
            if (rel == null || !Updater.IsNewer(rel))
            {
                _vm.Status = $"SWGB Modern {_vm.Version} is up to date.";
                return;
            }
            var r = MessageBox.Show(this, $"SWGB Modern {rel.Tag} is available (you have {_vm.Version}).\n\n{Trim(rel.Notes)}\n\nUpdate now?",
                                    "Update available", MessageBoxButton.YesNo, MessageBoxImage.Information);
            if (r != MessageBoxResult.Yes) { _vm.Status = $"Update {rel.Tag} available."; return; }
            _vm.Status = "Downloading update...";
            await Updater.DownloadAndRestartAsync(rel, new Progress<double>(p => _vm.Status = $"Downloading update... {p:P0}"));
        }
        catch (Exception ex)
        {
            _vm.Status = "Update check failed.";
            if (interactive) Error("Update check failed", ex);
        }
    }

    private static string Trim(string s) => s.Length > 600 ? s[..600] + "..." : s;
    private static void Shell(string target) => Process.Start(new ProcessStartInfo(target) { UseShellExecute = true });
    private void Error(string what, Exception ex) =>
        MessageBox.Show(this, $"{what}:\n{ex.Message}", "SWGB Modern", MessageBoxButton.OK, MessageBoxImage.Error);
}
