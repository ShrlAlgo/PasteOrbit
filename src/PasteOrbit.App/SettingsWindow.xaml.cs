using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

using Microsoft.Win32;

namespace PasteOrbit.App;

public partial class SettingsWindow : Window
{
    private readonly App _app;
    private readonly LocalBackupService _backup;

    public SettingsWindow(App app)
    {
        _app = app;
        _backup = new LocalBackupService(
            Path.Combine(app.DataDirectory, "history.db"),
            Path.Combine(app.DataDirectory, "settings.json"));
        InitializeComponent();
        LoadSettings(app.Settings);
        VersionText.Text = $"PasteOrbit {Assembly.GetEntryAssembly()?.GetName().Version?.ToString(3)}";
    }

    private void LoadSettings(AppSettings settings)
    {
        LanguageBox.SelectedValue = settings.Language;
        ThemeBox.SelectedValue = settings.ThemeMode;
        AutoHideBox.IsChecked = settings.AutoHideOnDeactivate;
        StartWithWindowsBox.IsChecked = settings.StartWithWindows;
        MonitorTextBox.IsChecked = settings.MonitorText;
        MonitorImagesBox.IsChecked = settings.MonitorImages;
        MonitorFilesBox.IsChecked = settings.MonitorFiles;
        ExcludedAppsBox.Text = settings.ExcludedApplications;
        RetentionDaysBox.Text = settings.RetentionDays.ToString(CultureInfo.InvariantCulture);
        MaxEntriesBox.Text = settings.MaxHistoryEntries.ToString(CultureInfo.InvariantCulture);
        HotKeyBox.Text = settings.GlobalHotKey;
    }

    private void SaveButton_Click(object sender, RoutedEventArgs e)
    {
        if (!int.TryParse(RetentionDaysBox.Text, out var retentionDays) || retentionDays is < 1 or > 3650
            || !int.TryParse(MaxEntriesBox.Text, out var maxEntries) || maxEntries is < 10 or > 100000)
        {
            MessageBox.Show("历史保留范围无效。", "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var settings = _app.Settings;
        settings.Language = LanguageBox.SelectedValue as string ?? string.Empty;
        settings.ThemeMode = ThemeBox.SelectedValue as string ?? "System";
        settings.AutoHideOnDeactivate = AutoHideBox.IsChecked == true;
        settings.StartWithWindows = StartWithWindowsBox.IsChecked == true;
        settings.MonitorText = MonitorTextBox.IsChecked == true;
        settings.MonitorImages = MonitorImagesBox.IsChecked == true;
        settings.MonitorFiles = MonitorFilesBox.IsChecked == true;
        settings.ExcludedApplications = ExcludedAppsBox.Text.Trim();
        settings.RetentionDays = retentionDays;
        settings.MaxHistoryEntries = maxEntries;
        settings.GlobalHotKey = HotKeyBox.Text;
        _app.SettingsStore.Save(settings);
        AppLocalization.SetLanguage(settings.Language);
        _app.ApplyTheme(settings.ThemeMode);
        SetStartup(settings.StartWithWindows);
        (_app.MainWindow as MainWindow)?.ApplySettings();
        Close();
    }

    private void HotKeyBox_PreviewKeyDown(object sender, System.Windows.Input.KeyEventArgs e)
    {
        e.Handled = true;
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or Key.LeftShift or Key.RightShift or Key.LWin or Key.RWin)
        {
            return;
        }

        var virtualKey = (uint)KeyInterop.VirtualKeyFromKey(key);
        if (GlobalHotKey.TryFormatShortcut(
                virtualKey,
                Keyboard.Modifiers.HasFlag(ModifierKeys.Control),
                Keyboard.Modifiers.HasFlag(ModifierKeys.Alt),
                Keyboard.Modifiers.HasFlag(ModifierKeys.Shift),
                Keyboard.IsKeyDown(Key.LWin) || Keyboard.IsKeyDown(Key.RWin),
                out var shortcut))
        {
            HotKeyBox.Text = shortcut;
        }
    }

    private async void ExportButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new SaveFileDialog { Filter = "PasteOrbit Backup (*.pobackup)|*.pobackup", FileName = "PasteOrbit.pobackup" };
        if (dialog.ShowDialog(this) != true)
        {
            return;
        }

        try
        {
            await _backup.ExportAsync(dialog.FileName);
            MessageBox.Show("备份已导出。", "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception exception)
        {
            MessageBox.Show(exception.Message, "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private async void RestoreButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "PasteOrbit Backup (*.pobackup)|*.pobackup" };
        if (dialog.ShowDialog(this) != true
            || MessageBox.Show("恢复会替换当前历史和设置，是否继续？", "PasteOrbit", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
        {
            return;
        }

        try
        {
            await _backup.RestoreAsync(dialog.FileName);
            MessageBox.Show("恢复完成，请重新启动 PasteOrbit。", "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Information);
            _app.ExitApplication();
        }
        catch (Exception exception)
        {
            MessageBox.Show(exception.Message, "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private async void UpdateButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            using var service = new UpdateCheckService();
            var update = await service.CheckAsync();
            MessageBox.Show(update is { IsUpdateAvailable: true } ? $"发现新版本 {update.LatestVersion}" : "当前已是最新版本。",
                "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception exception)
        {
            MessageBox.Show(exception.Message, "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void ResetButton_Click(object sender, RoutedEventArgs e) => LoadSettings(new AppSettings());

    private static void SetStartup(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
        if (enabled)
        {
            key.SetValue("PasteOrbit", $"\"{Environment.ProcessPath}\"");
        }
        else
        {
            key.DeleteValue("PasteOrbit", throwOnMissingValue: false);
        }
    }
}
