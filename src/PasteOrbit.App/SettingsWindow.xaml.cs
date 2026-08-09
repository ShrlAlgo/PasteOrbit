using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security;
using Microsoft.Win32;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using WinRT.Interop;
using Windows.Graphics;

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
    private bool _isInitialized;
    private bool _isLoadingSettings;
    private bool _configured;
    private AppWindow? _appWindow;
    // 设置页使用轻量页面历史栈，让标题栏返回与 NavigationView 的页面切换保持一致。
    private readonly Stack<string> _navigationHistory = [];
    private string? _currentPageTag;
    private bool _isNavigatingBack;

    public SettingsWindow(AppSettings settings, AppSettingsStore store)
    {
        InitializeComponent();
        _store = store;
        _currentPageTag = "General";
        _isInitialized = true;
        SettingsRoot.ActualThemeChanged += SettingsRoot_ActualThemeChanged;
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
        ApplyNativeTitleBarTheme(SettingsRoot.ActualTheme == ElementTheme.Dark);
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
        HotKeyTextBox.Text = GlobalHotKey.TryNormalizeShortcut(settings.GlobalHotKey, out var normalizedShortcut)
            ? normalizedShortcut
            : new AppSettings().GlobalHotKey;
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

        ApplyThemePalette(SettingsRoot.ActualTheme == ElementTheme.Dark);
    }

    private void SettingsRoot_ActualThemeChanged(FrameworkElement sender, object args)
    {
        ApplyThemePalette(sender.ActualTheme == ElementTheme.Dark);
    }

    private void ApplyThemePalette(bool isDark)
    {
        SetBrushColor("PageBackgroundBrush", isDark ? (32, 32, 32) : (247, 249, 252));
        SetBrushColor("SurfaceBrush", isDark ? (43, 43, 43) : (255, 255, 255));
        SetBrushColor("SurfaceAltBrush", isDark ? (37, 37, 37) : (251, 252, 254));
        SetBrushColor("TextPrimaryBrush", isDark ? (243, 243, 243) : (29, 41, 57));
        SetBrushColor("TextSecondaryBrush", isDark ? (185, 185, 185) : (102, 112, 133));
        SetBrushColor("TextMutedBrush", isDark ? (214, 214, 214) : (52, 64, 84));
        SetBrushColor("BorderBrush", isDark ? (69, 69, 69) : (217, 225, 234));
        SetBrushColor("AccentBrush", 15, 108, 189);
        SetBrushColor("AccentForegroundBrush", 255, 255, 255);
        SettingsRoot.Background = GetBrush("PageBackgroundBrush");
        ApplyNativeTitleBarTheme(isDark);
    }

    // 原生标题栏不继承设置页内容区的主题，需要单独同步颜色。
    private void ApplyNativeTitleBarTheme(bool isDark)
    {
        if (_appWindow is null)
        {
            return;
        }

        var titleBar = _appWindow.TitleBar;
        var background = isDark
            ? ColorHelper.FromArgb(255, 32, 32, 32)
            : ColorHelper.FromArgb(255, 247, 249, 252);
        var foreground = isDark
            ? ColorHelper.FromArgb(255, 243, 243, 243)
            : ColorHelper.FromArgb(255, 29, 41, 57);
        var inactiveForeground = isDark
            ? ColorHelper.FromArgb(255, 185, 185, 185)
            : ColorHelper.FromArgb(255, 102, 112, 133);
        var hoverBackground = isDark
            ? ColorHelper.FromArgb(255, 52, 52, 52)
            : ColorHelper.FromArgb(255, 242, 244, 247);
        var pressedBackground = isDark
            ? ColorHelper.FromArgb(255, 61, 61, 61)
            : ColorHelper.FromArgb(255, 228, 231, 236);

        titleBar.BackgroundColor = background;
        titleBar.InactiveBackgroundColor = background;
        titleBar.ForegroundColor = foreground;
        titleBar.InactiveForegroundColor = inactiveForeground;
        titleBar.ButtonBackgroundColor = background;
        titleBar.ButtonInactiveBackgroundColor = background;
        titleBar.ButtonForegroundColor = foreground;
        titleBar.ButtonInactiveForegroundColor = inactiveForeground;
        titleBar.ButtonHoverBackgroundColor = hoverBackground;
        titleBar.ButtonHoverForegroundColor = foreground;
        titleBar.ButtonPressedBackgroundColor = pressedBackground;
        titleBar.ButtonPressedForegroundColor = foreground;
    }

    private void SetBrushColor(string key, (int Red, int Green, int Blue) color)
    {
        SetBrushColor(key, color.Red, color.Green, color.Blue);
    }

    private void SetBrushColor(string key, int red, int green, int blue)
    {
        if (SettingsRoot.Resources[key] is SolidColorBrush brush)
        {
            brush.Color = ColorHelper.FromArgb(255, (byte)red, (byte)green, (byte)blue);
        }
    }

    private SolidColorBrush GetBrush(string key)
    {
        return (SolidColorBrush)SettingsRoot.Resources[key];
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
    }

    private void SettingToggleSwitch_Changed(object sender, RoutedEventArgs e)
    {
        ApplyCurrentSettings();
    }

    private void SettingComboBox_Changed(object sender, SelectionChangedEventArgs e)
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
            GlobalHotKey = GetCurrentHotKey(),
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
