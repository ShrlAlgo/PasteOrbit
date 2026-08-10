using System.Runtime.InteropServices;
using System.Security;
using System.Security.Cryptography;

using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.Win32;

using Windows.Graphics;
using Windows.Storage.Pickers;

using WinRT.Interop;

namespace PasteOrbit.App;

public sealed partial class SettingsWindow : Window
{
    private const int VirtualKeyControl = 0x11;
    private const int VirtualKeyAlt = 0x12;
    private const int VirtualKeyShift = 0x10;
    private const int VirtualKeyLeftWindows = 0x5B;
    private const int VirtualKeyRightWindows = 0x5C;
    private const string StartupRunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string StartupValueName = "PasteOrbit";
    private readonly AppSettingsStore _store;
    private readonly Func<string, Task> _exportBackup;
    private readonly Func<string, Task> _restoreBackup;
    private bool _isInitialized;
    private bool _isLoadingSettings;
    private bool _configured;
    private AppWindow? _appWindow;
    // 设置页使用轻量页面历史栈，让标题栏返回与 NavigationView 的页面切换保持一致。
    private readonly Stack<string> _navigationHistory = [];
    private string? _currentPageTag;
    private bool _isNavigatingBack;

    public SettingsWindow(
        AppSettings settings,
        AppSettingsStore store,
        Func<string, Task> exportBackup,
        Func<string, Task> restoreBackup)
    {
        InitializeComponent();
        _store = store;
        _exportBackup = exportBackup;
        _restoreBackup = restoreBackup;
        _currentPageTag = "General";
        _isInitialized = true;
        ApplyThemeSettings(settings.ThemeMode);
        _isLoadingSettings = true;
        LoadSettings(settings);
        _isLoadingSettings = false;
        AttachSettingHandlers();
        SettingsNavigation.BackRequested += SettingsNavigation_BackRequested;
        Activated += SettingsWindow_Activated;
    }

    public event Action<AppSettings>? SettingsChanged;

    public void InitializeNative()
    {
        if (_configured)
        {
            return;
        }

        _configured = true;
        var handle = WindowNative.GetWindowHandle(this);
        _appWindow = AppWindow.GetFromWindowId(Win32Interop.GetWindowIdFromWindow(handle));
        var appWindow = _appWindow!;
        var iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "PasteOrbit.ico");
        if (File.Exists(iconPath))
        {
            appWindow.SetIcon(iconPath);
        }
        appWindow.Resize(new SizeInt32(760, 680));
        var presenter = OverlappedPresenter.Create();
        presenter.IsResizable = true;
        presenter.IsMaximizable = false;
        presenter.IsMinimizable = false;
        presenter.PreferredMinimumWidth = 600;
        presenter.SetBorderAndTitleBar(true, true);
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(SettingsTitleBar);
        appWindow.SetPresenter(presenter);
        ApplyNativeTitleBarTheme();
        var workArea = DisplayArea.GetFromWindowId(appWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        appWindow.Move(new PointInt32(
            workArea.X + (workArea.Width - appWindow.Size.Width) / 2,
            workArea.Y + (workArea.Height - appWindow.Size.Height) / 2));
    }

    private void SettingsWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        if (args.WindowActivationState == WindowActivationState.Deactivated)
        {
            return;
        }

        InitializeNative();
    }

    private void LoadSettings(AppSettings settings, bool includeSystemStartup = true)
    {
        StartWithWindowsToggleSwitch.IsOn = settings.StartWithWindows
            || (includeSystemStartup && IsStartWithWindowsEnabled());
        AutoHideToggleSwitch.IsOn = settings.AutoHideOnDeactivate;
        MonitorTextToggleSwitch.IsOn = settings.MonitorText;
        MonitorImagesToggleSwitch.IsOn = settings.MonitorImages;
        MonitorFilesToggleSwitch.IsOn = settings.MonitorFiles;
        ExcludedApplicationsTextBox.Text = settings.ExcludedApplications;
        HotKeyTextBox.Text = GlobalHotKey.TryNormalizeShortcut(settings.GlobalHotKey, out var normalizedShortcut)
            ? normalizedShortcut
            : new AppSettings().GlobalHotKey;
        var defaults = new AppSettings();
        PasteShortcutTextBox.Text = PanelShortcut.NormalizeOrDefault(settings.PasteShortcut, defaults.PasteShortcut);
        PlainTextPasteShortcutTextBox.Text = PanelShortcut.NormalizeOrDefault(settings.PlainTextPasteShortcut, defaults.PlainTextPasteShortcut);
        PreviewShortcutTextBox.Text = PanelShortcut.NormalizeOrDefault(settings.PreviewShortcut, defaults.PreviewShortcut);
        PinShortcutTextBox.Text = PanelShortcut.NormalizeOrDefault(settings.PinShortcut, defaults.PinShortcut);
        DeleteShortcutTextBox.Text = PanelShortcut.NormalizeOrDefault(settings.DeleteShortcut, defaults.DeleteShortcut);
        PasteAsFileShortcutTextBox.Text = PanelShortcut.NormalizeOrDefault(settings.PasteAsFileShortcut, defaults.PasteAsFileShortcut);
        SelectComboItem(ThemeComboBox, settings.ThemeMode);
        SelectComboItem(DensityComboBox, settings.Density);
        SelectComboItem(RetentionDaysComboBox, settings.RetentionDays.ToString());
        SelectComboItem(MaxEntriesComboBox, settings.MaxHistoryEntries.ToString());
    }

    private void ApplyThemeSettings(string themeMode)
    {
        SettingsRoot.RequestedTheme = themeMode switch
        {
            "深色" => ElementTheme.Dark,
            "浅色" => ElementTheme.Light,
            _ => ElementTheme.Default
        };

    }

    // 原生标题栏不继承设置页内容区的主题，需要单独同步颜色。
    private void ApplyNativeTitleBarTheme()
    {
        if (_appWindow is null)
        {
            return;
        }

        var titleBar = _appWindow.TitleBar;
        titleBar.BackgroundColor = null;
        titleBar.InactiveBackgroundColor = null;
        titleBar.ForegroundColor = null;
        titleBar.InactiveForegroundColor = null;
        titleBar.ButtonBackgroundColor = Colors.Transparent;
        titleBar.ButtonInactiveBackgroundColor = Colors.Transparent;
        titleBar.ButtonForegroundColor = null;
        titleBar.ButtonInactiveForegroundColor = null;
        titleBar.ButtonHoverBackgroundColor = null;
        titleBar.ButtonHoverForegroundColor = null;
        titleBar.ButtonPressedBackgroundColor = null;
        titleBar.ButtonPressedForegroundColor = null;
    }

    private static void SelectComboItem(ComboBox comboBox, string value)
    {
        comboBox.SelectedItem = comboBox.Items
            .OfType<ComboBoxItem>()
            .FirstOrDefault(item => string.Equals(item.Content?.ToString(), value, StringComparison.Ordinal))
            ?? comboBox.Items[0];
    }

    private static string GetSelectedText(ComboBox comboBox)
    {
        return (comboBox.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? string.Empty;
    }

    private void AttachSettingHandlers()
    {
        StartWithWindowsToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        AutoHideToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        MonitorTextToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        MonitorImagesToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        MonitorFilesToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        ThemeComboBox.SelectionChanged += SettingComboBox_Changed;
        DensityComboBox.SelectionChanged += SettingComboBox_Changed;
        RetentionDaysComboBox.SelectionChanged += SettingComboBox_Changed;
        MaxEntriesComboBox.SelectionChanged += SettingComboBox_Changed;
        ExcludedApplicationsTextBox.LostFocus += SettingTextBox_LostFocus;
    }

    private void SettingToggleSwitch_Changed(object sender, RoutedEventArgs e)
    {
        ApplyCurrentSettings();
    }

    private void SettingComboBox_Changed(object sender, SelectionChangedEventArgs e)
    {
        ApplyCurrentSettings();
    }

    private void SettingTextBox_LostFocus(object sender, RoutedEventArgs e)
    {
        ApplyCurrentSettings();
    }

    private void HotKeyTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        e.Handled = true;
        if (!GlobalHotKey.TryFormatShortcut(
                (uint)e.Key,
                IsVirtualKeyDown(VirtualKeyControl),
                IsVirtualKeyDown(VirtualKeyAlt),
                IsVirtualKeyDown(VirtualKeyShift),
                IsVirtualKeyDown(VirtualKeyLeftWindows) || IsVirtualKeyDown(VirtualKeyRightWindows),
                out var shortcut))
        {
            return;
        }

        HotKeyTextBox.Text = shortcut;
        ApplyCurrentSettings();
    }

    private string GetCurrentHotKey()
    {
        return GlobalHotKey.TryNormalizeShortcut(HotKeyTextBox.Text, out var normalizedShortcut)
            ? normalizedShortcut
            : new AppSettings().GlobalHotKey;
    }

    private void PanelShortcutTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        e.Handled = true;
        if (sender is not TextBox textBox
            || !PanelShortcut.TryFormat(
                e.Key,
                IsVirtualKeyDown(VirtualKeyControl),
                IsVirtualKeyDown(VirtualKeyAlt),
                IsVirtualKeyDown(VirtualKeyShift),
                out var shortcut))
        {
            return;
        }

        textBox.Text = shortcut;
        ApplyCurrentSettings();
    }

    private static bool IsVirtualKeyDown(int virtualKey)
    {
        return (GetKeyState(virtualKey) & 0x8000) != 0;
    }

    private AppSettings ReadCurrentSettings()
    {
        return new AppSettings
        {
            StartWithWindows = StartWithWindowsToggleSwitch.IsOn,
            AutoHideOnDeactivate = AutoHideToggleSwitch.IsOn,
            MonitorText = MonitorTextToggleSwitch.IsOn,
            MonitorImages = MonitorImagesToggleSwitch.IsOn,
            MonitorFiles = MonitorFilesToggleSwitch.IsOn,
            ExcludedApplications = ExcludedApplicationsTextBox.Text.Trim(),
            GlobalHotKey = GetCurrentHotKey(),
            PasteShortcut = PasteShortcutTextBox.Text,
            PlainTextPasteShortcut = PlainTextPasteShortcutTextBox.Text,
            PreviewShortcut = PreviewShortcutTextBox.Text,
            PinShortcut = PinShortcutTextBox.Text,
            DeleteShortcut = DeleteShortcutTextBox.Text,
            PasteAsFileShortcut = PasteAsFileShortcutTextBox.Text,
            ThemeMode = GetSelectedText(ThemeComboBox),
            Density = GetSelectedText(DensityComboBox),
            RetentionDays = int.Parse(GetSelectedText(RetentionDaysComboBox)),
            MaxHistoryEntries = int.Parse(GetSelectedText(MaxEntriesComboBox))
        };
    }

    private void ApplyCurrentSettings()
    {
        if (_isLoadingSettings)
        {
            return;
        }

        var newSettings = ReadCurrentSettings();
        try
        {
            ApplyStartWithWindows(newSettings.StartWithWindows);
            _store.Save(newSettings);
            ApplyThemeSettings(newSettings.ThemeMode);
            SettingsChanged?.Invoke(newSettings);
        }
        catch (IOException)
        {
            _ = ShowSaveErrorAsync();
        }
        catch (UnauthorizedAccessException)
        {
            _ = ShowSaveErrorAsync();
        }
        catch (SecurityException)
        {
            _ = ShowSaveErrorAsync();
        }
        catch (InvalidOperationException)
        {
            _ = ShowSaveErrorAsync();
        }
    }

    private void SettingsNavigation_SelectionChanged(
        NavigationView sender,
        NavigationViewSelectionChangedEventArgs args)
    {
        if (!_isInitialized
            || args.SelectedItem is not NavigationViewItem item)
        {
            return;
        }

        var selected = item.Tag?.ToString();
        if (!string.IsNullOrEmpty(selected)
            && !_isNavigatingBack
            && _currentPageTag is not null
            && !string.Equals(_currentPageTag, selected, StringComparison.Ordinal))
        {
            _navigationHistory.Push(_currentPageTag);
        }

        _currentPageTag = selected;
        GeneralPanel.Visibility = selected == "General" ? Visibility.Visible : Visibility.Collapsed;
        HotKeyPanel.Visibility = selected == "HotKey" ? Visibility.Visible : Visibility.Collapsed;
        HistoryPanel.Visibility = selected == "History" ? Visibility.Visible : Visibility.Collapsed;
        PrivacyPanel.Visibility = selected == "Privacy" ? Visibility.Visible : Visibility.Collapsed;
    }

    private void RestoreDefaultsButton_Click(object sender, RoutedEventArgs e)
    {
        _isLoadingSettings = true;
        LoadSettings(new AppSettings(), includeSystemStartup: false);
        _isLoadingSettings = false;
        ApplyCurrentSettings();
    }

    private async void ExportBackupButton_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileSavePicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            SuggestedFileName = $"PasteOrbit-{DateTime.Now:yyyyMMdd-HHmmss}"
        };
        picker.FileTypeChoices.Add("PasteOrbit 加密备份", [".pobackup"]);
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
        var file = await picker.PickSaveFileAsync();
        if (file is null)
        {
            return;
        }

        try
        {
            await _exportBackup(file.Path);
            await ShowMessageAsync("备份已导出", "历史记录和设置已写入加密备份文件。");
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or CryptographicException)
        {
            await ShowMessageAsync("导出失败", exception.Message);
        }
    }

    private async void RestoreBackupButton_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary
        };
        picker.FileTypeFilter.Add(".pobackup");
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
        var file = await picker.PickSingleFileAsync();
        if (file is null)
        {
            return;
        }

        var confirmation = new ContentDialog
        {
            Title = "恢复本地备份？",
            Content = "当前历史记录和设置将被备份内容替换，恢复前会自动保留一份本机副本。",
            PrimaryButtonText = "恢复",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close,
            XamlRoot = SettingsRoot.XamlRoot
        };
        if (await confirmation.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        try
        {
            await _restoreBackup(file.Path);
            var restoredSettings = _store.Load();
            ApplyStartWithWindows(restoredSettings.StartWithWindows);
            _isLoadingSettings = true;
            LoadSettings(restoredSettings, includeSystemStartup: false);
            _isLoadingSettings = false;
            ApplyThemeSettings(restoredSettings.ThemeMode);
            await ShowMessageAsync("恢复完成", "历史记录和设置已恢复。");
        }
        catch (Exception exception) when (exception is IOException
                                           or UnauthorizedAccessException
                                           or CryptographicException
                                           or InvalidDataException)
        {
            await ShowMessageAsync("恢复失败", exception.Message);
        }
    }

    private async Task ShowMessageAsync(string title, string message)
    {
        var dialog = new ContentDialog
        {
            Title = title,
            Content = message,
            CloseButtonText = "确定",
            XamlRoot = SettingsRoot.XamlRoot
        };
        await dialog.ShowAsync();
    }

    private async Task ShowSaveErrorAsync()
    {
        var dialog = new ContentDialog
        {
            Title = "PasteOrbit",
            Content = "设置应用失败，请稍后重试。",
            CloseButtonText = "确定",
            XamlRoot = SettingsRoot.XamlRoot
        };
        await dialog.ShowAsync();
    }

    private static bool IsStartWithWindowsEnabled()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(StartupRunKey, writable: false);
            return key?.GetValue(StartupValueName) is string value
                && value.Contains("PasteOrbit", StringComparison.OrdinalIgnoreCase);
        }
        catch (SecurityException)
        {
            return false;
        }
    }

    private static void ApplyStartWithWindows(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(StartupRunKey, writable: true)
            ?? throw new InvalidOperationException("无法打开 Windows 启动项。");
        if (enabled)
        {
            var processPath = Environment.ProcessPath
                ?? throw new InvalidOperationException("无法定位 PasteOrbit 程序路径。");
            key.SetValue(StartupValueName, $"\"{processPath}\"");
        }
        else
        {
            key.DeleteValue(StartupValueName, throwOnMissingValue: false);
        }
    }

    private void SettingsNavigation_BackRequested(NavigationView sender, NavigationViewBackRequestedEventArgs args)
    {
        NavigateBack();
    }

    private void SettingsTitleBar_BackRequested(TitleBar sender, object args)
    {
        NavigateBack();
    }

    private void SettingsTitleBar_PaneToggleRequested(TitleBar sender, object args)
    {
        SettingsNavigation.IsPaneOpen = !SettingsNavigation.IsPaneOpen;
    }

    private void NavigateBack()
    {
        if (_navigationHistory.Count == 0)
        {
            Close();
            return;
        }

        var previousTag = _navigationHistory.Pop();
        var previousItem = SettingsNavigation.MenuItems
            .OfType<NavigationViewItem>()
            .FirstOrDefault(item => string.Equals(item.Tag?.ToString(), previousTag, StringComparison.Ordinal));
        if (previousItem is null)
        {
            Close();
            return;
        }

        _isNavigatingBack = true;
        try
        {
            SettingsNavigation.SelectedItem = previousItem;
        }
        finally
        {
            _isNavigatingBack = false;
        }
    }

    [DllImport("user32.dll")]
    private static extern short GetKeyState(int virtualKey);

}
