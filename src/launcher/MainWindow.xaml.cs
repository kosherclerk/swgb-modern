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
            else
            {
                // let the window paint before the first-run question appears
                if (_vm.GameDir != null)
                    await Dispatcher.InvokeAsync(AskProfileIfFirstRun, System.Windows.Threading.DispatcherPriority.ContextIdle);
                if (_vm.CheckUpdates) await CheckForUpdates(interactive: false);
            }
        };
        Closing += (_, _) => { try { _vm.SaveAll(); } catch { } };
    }

    private CancellationTokenSource? _convCts;

    private void ProfileFaithful_Click(object sender, RoutedEventArgs e) { _vm.ApplyProfile("faithful"); _vm.Status = "Faithful profile selected - press Save or Play."; }
    private void ProfileEnhanced_Click(object sender, RoutedEventArgs e) { _vm.ApplyProfile("enhanced"); _vm.Status = "Enhanced profile selected - press Save or Play."; }

    /// <summary>First start: let the player choose between the original look and all enhancements.</summary>
    private void AskProfileIfFirstRun()
    {
        if (_vm.Prefs.Profile != null) return;
        var r = MessageBox.Show(this,
            "How would you like to play?\n\n" +
            "YES - Enhanced: widescreen, smooth 60 fps rendering and the Sharp upscaling filter.\n\n" +
            "NO - Faithful: the original look (4:3, 20 fps redraw, plain pixels). Only compatibility fixes " +
            "(modern renderer, audio, crash and CPU fixes) are applied.\n\nYou can change every option later on the Components tab.",
            "SWGB Modern - choose a profile", MessageBoxButton.YesNo, MessageBoxImage.Question);
        _vm.ApplyProfile(r == MessageBoxResult.Yes ? "enhanced" : "faithful");
        try { _vm.SaveAll(); } catch { }
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

    private async void Play_Click(object sender, RoutedEventArgs e)
    {
        if (_vm.SelectedGame == null) { MessageBox.Show(this, "No game executable found. Choose the game folder.", "SWGB Modern"); return; }
        if (!TryInstall(silent: true)) return;
        await ConvertIntrosIfNeeded();
        try
        {
            string exe = _vm.SelectedGame.ExePath;
            if (_vm.LargeAddressAware) exe = new Installer(_vm.GameDir!).LargeAddressAwareCopy(exe);
            var psi = new ProcessStartInfo(exe)
            {
                WorkingDirectory = Path.GetDirectoryName(exe)!,
                UseShellExecute = false,
                // The game's own intro player does not work on modern Windows;
                // the mod plays the intros itself ([Video] PlayIntro).
                Arguments = "NOSTARTUP",
            };
            Process.Start(psi);
            _vm.Status = $"Started {_vm.SelectedGame.Name}.";
            if (_vm.CloseOnPlay) Close();   // settings are saved on closing
            else WindowState = WindowState.Minimized;
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
    private void OpenCaptures_Click(object sender, RoutedEventArgs e)
    {
        if (_vm.GameDir == null) return;
        var d = Path.Combine(_vm.GameDir, "SWGBModern", "captures");
        if (Directory.Exists(d)) Shell(d);
        else MessageBox.Show(this, "No captures yet. Turn on Diagnostics, play, and press Ctrl+Shift+F12.", "SWGB Modern");
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

    /// <summary>Install any published version (rollback or update).</summary>
    private async void Versions_Click(object sender, RoutedEventArgs e)
    {
        List<ReleaseInfo> all;
        try
        {
            _vm.Status = "Loading the list of versions...";
            all = await Updater.AllAsync();
            _vm.Status = "";
        }
        catch (Exception ex) { _vm.Status = "Could not load the versions."; Error("Could not load the list of versions", ex); return; }
        if (all.Count == 0) { MessageBox.Show(this, "No versions found.", "SWGB Modern"); return; }

        var list = new System.Windows.Controls.ListBox { Height = 260, Margin = new Thickness(0, 8, 0, 8) };
        foreach (var r in all)
        {
            string label = $"{r.Tag}   {(r.Published == default ? "" : r.Published.ToLocalTime().ToString("yyyy-MM-dd"))}" +
                           (Updater.SameVersion(r) ? "   (installed)" : Updater.IsNewer(r) ? "   (newer)" : "");
            list.Items.Add(new System.Windows.Controls.ListBoxItem { Content = label, Tag = r });
        }
        list.SelectedIndex = 0;
        var field = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(0x16, 0x1B, 0x24));
        list.Background = field; list.Foreground = Foreground;
        var notes = new System.Windows.Controls.TextBox
        {
            Height = 150, IsReadOnly = true, TextWrapping = TextWrapping.Wrap,
            VerticalScrollBarVisibility = System.Windows.Controls.ScrollBarVisibility.Auto,
        };
        notes.Background = field; notes.Foreground = Foreground;
        list.SelectionChanged += (_, _) => notes.Text = (list.SelectedItem as System.Windows.Controls.ListBoxItem)?.Tag is ReleaseInfo r ? r.Notes : "";
        notes.Text = all[0].Notes;
        var install = new System.Windows.Controls.Button { Content = "Install selected version", IsDefault = true, Margin = new Thickness(0, 0, 8, 0) };
        var cancel = new System.Windows.Controls.Button { Content = "Cancel", IsCancel = true };
        var buttons = new System.Windows.Controls.StackPanel { Orientation = System.Windows.Controls.Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
        buttons.Children.Add(install); buttons.Children.Add(cancel);
        var panel = new System.Windows.Controls.StackPanel { Margin = new Thickness(16) };
        panel.Children.Add(new System.Windows.Controls.TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Text = $"You have {_vm.Version}. Choose a version to install: the launcher replaces itself, restarts and installs that version's files into the game. Your settings are kept.",
        });
        panel.Children.Add(list);
        panel.Children.Add(notes);
        panel.Children.Add(new System.Windows.Controls.Border { Height = 10 });
        panel.Children.Add(buttons);
        var dlg = new Window
        {
            Title = "SWGB Modern - choose version", Owner = this, Content = panel, Width = 560, SizeToContent = SizeToContent.Height,
            WindowStartupLocation = WindowStartupLocation.CenterOwner, ResizeMode = ResizeMode.NoResize,
            Background = Background, Foreground = Foreground,
        };
        ReleaseInfo? pick = null;
        install.Click += (_, _) => { pick = (list.SelectedItem as System.Windows.Controls.ListBoxItem)?.Tag as ReleaseInfo; dlg.DialogResult = pick != null; };
        if (dlg.ShowDialog() != true || pick == null) return;
        if (Updater.SameVersion(pick)) { _vm.Status = $"{pick.Tag} is already installed."; return; }
        if (!Updater.IsNewer(pick))
        {
            var ok = MessageBox.Show(this,
                $"Install the older version {pick.Tag}?\n\nAutomatic update checks will be turned off so it does not offer to update again at every start. " +
                "You can turn them back on (Mod tab), or use Check for updates / Choose version at any time.",
                "Install an older version", MessageBoxButton.OKCancel, MessageBoxImage.Question);
            if (ok != MessageBoxResult.OK) return;
            _vm.CheckUpdates = false;
        }
        try
        {
            _vm.SaveAll();
            _vm.Status = $"Downloading {pick.Tag}...";
            await Updater.DownloadAndRestartAsync(pick, new Progress<double>(p => _vm.Status = $"Downloading {pick.Tag}... {p:P0}"));
        }
        catch (Exception ex) { _vm.Status = "Install failed."; Error($"Could not install {pick.Tag}", ex); }
    }
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

    /// <summary>Before launch: without the Indeo codec the intros need an H.264 copy, made once with FFmpeg.</summary>
    private async Task ConvertIntrosIfNeeded()
    {
        var game = _vm.GameDir;
        if (game == null || !_vm.PlayIntro || _vm.Generating || !Intros.NeedConversion(game)) return;
        if (!Intros.HaveFfmpeg)
        {
            var r = MessageBox.Show(this, "The intro videos use the old Indeo codec, which Windows no longer includes.\n\n" +
                                          "Download FFmpeg (about 100 MB, kept in your user folder) and convert them now? It takes a few seconds. " +
                                          "Choose No to start without intro videos.", "SWGB Modern", MessageBoxButton.YesNo, MessageBoxImage.Question);
            if (r != MessageBoxResult.Yes) return;
        }
        _vm.Generating = true;
        _convCts = new CancellationTokenSource();
        var log = new Progress<string>(t => _vm.Status = t);
        try
        {
            await Intros.EnsureFfmpegAsync(log, _convCts.Token);
            await Intros.ConvertAsync(game, log, _convCts.Token);
            _vm.Status = "Intro videos converted.";
        }
        catch (OperationCanceledException) { _vm.Status = "Cancelled."; }
        catch (Exception ex) { _vm.Status = "Intro conversion failed: " + ex.Message; }
        finally { _vm.Generating = false; }
    }

    private static string Trim(string s) => s.Length > 600 ? s[..600] + "..." : s;
    private static void Shell(string target) => Process.Start(new ProcessStartInfo(target) { UseShellExecute = true });
    private void Error(string what, Exception ex) =>
        MessageBox.Show(this, $"{what}:\n{ex.Message}", "SWGB Modern", MessageBoxButton.OK, MessageBoxImage.Error);
}
