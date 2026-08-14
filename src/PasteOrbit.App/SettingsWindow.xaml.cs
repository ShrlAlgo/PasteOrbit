using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security;
using System.Security.Cryptography;

using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
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
    private string _appliedLanguage;
    private bool _isInitialized;
    private bool _isLoadingSettings;
    private bool _isRefreshingLocalization;
    private bool _configured;
    private AppWindow? _appWindow;
    // 设置页使用轻量页面历史栈，让标题栏返回与 NavigationView 的页面切换保持一致。
    private readonly Stack<string> _navigationHistory = [];
    private string? _currentPageTag;
    private bool _isNavigatingBack;

    private sealed record RunningProcessItem(string ProcessName, string WindowTitle)
    {
        public string DisplayName => string.IsNullOrWhiteSpace(WindowTitle)
            ? ProcessName
            : $"{WindowTitle} ({ProcessName})";
    }

    public SettingsWindow(
        AppSettings settings,
        AppSettingsStore store,
        Func<string, Task> exportBackup,
        Func<string, Task> restoreBackup)
    {
        InitializeComponent();
        Title = AppLocalization.GetString("SettingsWindowTitle");
        var shortcutPlaceholder = AppLocalization.GetString("ShortcutPlaceholder");
        HotKeyTextBox.PlaceholderText = shortcutPlaceholder;
        PasteShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        PlainTextPasteShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        PreviewShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        PinShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        DeleteShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        PasteAsFileShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        _store = store;
        _exportBackup = exportBackup;
        _restoreBackup = restoreBackup;
        _appliedLanguage = settings.Language;
        _currentPageTag = "General";
        _isInitialized = true;
        ApplyThemeSettings(settings.ThemeMode);
        _isLoadingSettings = true;
        LoadSettings(settings);
        _isLoadingSettings = false;
        RefreshLocalization();
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
        ImageOcrToggleSwitch.IsOn = settings.EnableImageOcr;
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
        SelectComboItem(LanguageComboBox, settings.Language);
        SelectComboItem(ThemeComboBox, settings.ThemeMode);
        SelectComboItem(DensityComboBox, settings.Density);
        SelectComboItem(RetentionDaysComboBox, settings.RetentionDays.ToString());
        SelectComboItem(MaxEntriesComboBox, settings.MaxHistoryEntries.ToString());
    }

    private void ApplyThemeSettings(string themeMode)
    {
        SettingsRoot.RequestedTheme = themeMode switch
        {
            "Dark" => ElementTheme.Dark,
            "Light" => ElementTheme.Light,
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
            .FirstOrDefault(item => string.Equals(GetComboItemValue(item), value, StringComparison.Ordinal))
            ?? comboBox.Items[0];
    }

    private static string GetSelectedValue(ComboBox comboBox)
    {
        return comboBox.SelectedItem is ComboBoxItem item ? GetComboItemValue(item) : string.Empty;
    }

    private static string GetComboItemValue(ComboBoxItem item)
    {
        return item.Tag?.ToString() ?? item.Content?.ToString() ?? string.Empty;
    }

    // 语言切换后重新绑定选中项，让 ComboBox 的内容呈现器立即使用新的本地化文本。
    private static void RefreshComboBoxSelection(ComboBox comboBox)
    {
        if (comboBox.SelectedItem is not { } selectedItem)
        {
            return;
        }

        comboBox.SelectedItem = null;
        comboBox.SelectedItem = selectedItem;
    }

    private static IReadOnlyList<RunningProcessItem> GetRunningProcesses()
    {
        var processItems = new Dictionary<string, RunningProcessItem>(StringComparer.OrdinalIgnoreCase);
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                if (process.Id == Environment.ProcessId)
                {
                    continue;
                }

                string processName;
                try
                {
                    processName = process.ProcessName;
                }
                catch (ArgumentException)
                {
                    continue;
                }
                catch (InvalidOperationException)
                {
                    continue;
                }
                catch (Win32Exception)
                {
                    continue;
                }

                if (string.IsNullOrWhiteSpace(processName))
                {
                    continue;
                }

                var windowTitle = string.Empty;
                try
                {
                    windowTitle = process.MainWindowTitle;
                }
                catch (ArgumentException)
                {
                }
                catch (InvalidOperationException)
                {
                }
                catch (Win32Exception)
                {
                }

                if (!processItems.TryGetValue(processName, out var existingItem)
                    || (string.IsNullOrWhiteSpace(existingItem.WindowTitle)
                        && !string.IsNullOrWhiteSpace(windowTitle)))
                {
                    processItems[processName] = new RunningProcessItem(processName, windowTitle);
                }
            }
        }

        return processItems.Values
            .OrderByDescending(item => !string.IsNullOrWhiteSpace(item.WindowTitle))
            .ThenBy(item => item.DisplayName, StringComparer.CurrentCultureIgnoreCase)
            .ToArray();
    }

    private static IReadOnlyList<RunningProcessItem> FilterRunningProcesses(
        IReadOnlyList<RunningProcessItem> processItems,
        string? searchText)
    {
        if (string.IsNullOrWhiteSpace(searchText))
        {
            return processItems;
        }

        var query = searchText.Trim();
        return processItems
            .Where(item => item.ProcessName.Contains(query, StringComparison.CurrentCultureIgnoreCase)
                           || item.WindowTitle.Contains(query, StringComparison.CurrentCultureIgnoreCase))
            .ToArray();
    }

    private async void SelectExcludedApplicationButton_Click(object sender, RoutedEventArgs e)
    {
        IReadOnlyList<RunningProcessItem> processItems;
        try
        {
            processItems = await Task.Run(GetRunningProcesses);
        }
        catch (Exception exception) when (exception is Win32Exception or InvalidOperationException or UnauthorizedAccessException)
        {
            await ShowMessageAsync(AppLocalization.GetString("ProcessReadFailed"), exception.Message);
            return;
        }

        if (processItems.Count == 0)
        {
            await ShowMessageAsync(
                AppLocalization.GetString("NoProcessesTitle"),
                AppLocalization.GetString("NoProcessesMessage"));
            return;
        }

        var searchBox = new TextBox
        {
            PlaceholderText = AppLocalization.GetString("ProcessSearchPlaceholder"),
            MinWidth = 460
        };
        var processList = new ListView
        {
            ItemsSource = processItems,
            DisplayMemberPath = nameof(RunningProcessItem.DisplayName),
            SelectionMode = ListViewSelectionMode.Multiple,
            MaxHeight = 420,
            MinWidth = 460
        };
        var emptyResultText = new TextBlock
        {
            Text = AppLocalization.GetString("NoMatchingProcesses"),
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 24, 0, 16),
            Visibility = Visibility.Collapsed
        };
        var content = new StackPanel
        {
            Spacing = 8
        };
        content.Children.Add(searchBox);
        content.Children.Add(processList);
        content.Children.Add(emptyResultText);

        var dialog = new ContentDialog
        {
            Title = AppLocalization.GetString("SelectExcludedAppsTitle"),
            Content = content,
            PrimaryButtonText = AppLocalization.GetString("Add"),
            CloseButtonText = AppLocalization.GetString("Cancel"),
            DefaultButton = ContentDialogButton.Primary,
            IsPrimaryButtonEnabled = false,
            XamlRoot = SettingsRoot.XamlRoot
        };
        var selectedProcessNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var isRestoringSelection = false;

        void UpdatePrimaryButton()
        {
            dialog.IsPrimaryButtonEnabled = selectedProcessNames.Count > 0;
            dialog.PrimaryButtonText = selectedProcessNames.Count == 0
                ? AppLocalization.GetString("Add")
                : AppLocalization.Format("AddSelectedCount", selectedProcessNames.Count);
        }

        processList.SelectionChanged += (_, args) =>
        {
            if (isRestoringSelection)
            {
                return;
            }

            foreach (var item in args.AddedItems.OfType<RunningProcessItem>())
            {
                selectedProcessNames.Add(item.ProcessName);
            }

            foreach (var item in args.RemovedItems.OfType<RunningProcessItem>())
            {
                selectedProcessNames.Remove(item.ProcessName);
            }

            UpdatePrimaryButton();
        };
        searchBox.TextChanged += (_, _) =>
        {
            var filteredItems = FilterRunningProcesses(processItems, searchBox.Text);
            isRestoringSelection = true;
            processList.ItemsSource = filteredItems;
            foreach (var item in filteredItems.Where(item => selectedProcessNames.Contains(item.ProcessName)))
            {
                processList.SelectedItems.Add(item);
            }

            isRestoringSelection = false;
            processList.Visibility = filteredItems.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
            emptyResultText.Visibility = filteredItems.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            UpdatePrimaryButton();
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        AddExcludedApplications(selectedProcessNames);
    }

    private void AddExcludedApplications(IEnumerable<string> processNames)
    {
        var values = new List<string>();
        var knownProcessNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var configuredValues = ExcludedApplicationsTextBox.Text.Split(
            [';', ',', '\r', '\n'],
            StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        foreach (var value in configuredValues.Concat(processNames))
        {
            var processName = NormalizeProcessName(value);
            if (!string.IsNullOrWhiteSpace(processName) && knownProcessNames.Add(processName))
            {
                values.Add(processName);
            }
        }

        ExcludedApplicationsTextBox.Text = string.Join("; ", values);
        ApplyCurrentSettings();
    }

    private static string NormalizeProcessName(string value)
    {
        var processName = value.Trim();
        return processName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
            ? processName[..^4]
            : processName;
    }

    private void AttachSettingHandlers()
    {
        StartWithWindowsToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        AutoHideToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        MonitorTextToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        MonitorImagesToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        ImageOcrToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        MonitorFilesToggleSwitch.Toggled += SettingToggleSwitch_Changed;
        ThemeComboBox.SelectionChanged += SettingComboBox_Changed;
        DensityComboBox.SelectionChanged += SettingComboBox_Changed;
        LanguageComboBox.SelectionChanged += SettingComboBox_Changed;
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
        if (_isRefreshingLocalization)
        {
            return;
        }

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
            EnableImageOcr = ImageOcrToggleSwitch.IsOn,
            MonitorFiles = MonitorFilesToggleSwitch.IsOn,
            ExcludedApplications = ExcludedApplicationsTextBox.Text.Trim(),
            GlobalHotKey = GetCurrentHotKey(),
            PasteShortcut = PasteShortcutTextBox.Text,
            PlainTextPasteShortcut = PlainTextPasteShortcutTextBox.Text,
            PreviewShortcut = PreviewShortcutTextBox.Text,
            PinShortcut = PinShortcutTextBox.Text,
            DeleteShortcut = DeleteShortcutTextBox.Text,
            PasteAsFileShortcut = PasteAsFileShortcutTextBox.Text,
            Language = GetSelectedLanguage(),
            ThemeMode = GetSelectedValue(ThemeComboBox),
            Density = GetSelectedValue(DensityComboBox),
            RetentionDays = int.Parse(GetSelectedValue(RetentionDaysComboBox)),
            MaxHistoryEntries = int.Parse(GetSelectedValue(MaxEntriesComboBox))
        };
    }

    private string GetSelectedLanguage()
    {
        var selectedLanguage = GetSelectedValue(LanguageComboBox);
        return string.Equals(selectedLanguage, "System", StringComparison.Ordinal)
            ? string.Empty
            : selectedLanguage;
    }

    internal void RefreshLocalization()
    {
        var themeValue = GetSelectedValue(ThemeComboBox);
        var densityValue = GetSelectedValue(DensityComboBox);
        var languageValue = GetSelectedValue(LanguageComboBox);

        Title = AppLocalization.GetString("SettingsWindowTitle");
        SettingsTitleBar.Title = AppLocalization.GetString("SettingsTitleBarTitle");
        AutomationProperties.SetName(
            SettingsTitleBar,
            AppLocalization.GetString("SettingsTitleBarAutomationName"));

        GeneralNavigationItem.Content = AppLocalization.GetString("SettingsGeneralNavigationContent");
        HotKeyNavigationItem.Content = AppLocalization.GetString("SettingsHotKeyNavigationContent");
        HistoryNavigationItem.Content = AppLocalization.GetString("SettingsHistoryNavigationContent");
        PrivacyNavigationItem.Content = AppLocalization.GetString("SettingsPrivacyNavigationContent");

        StartupSectionText.Text = AppLocalization.GetString("SettingsStartupSectionText");
        MonitoringSectionText.Text = AppLocalization.GetString("SettingsMonitoringSectionText");
        AppearanceSectionText.Text = AppLocalization.GetString("SettingsAppearanceSectionText");
        HotKeySectionText.Text = AppLocalization.GetString("SettingsHotKeySectionText");
        HistorySectionText.Text = AppLocalization.GetString("SettingsHistorySectionText");
        PrivacySectionText.Text = AppLocalization.GetString("SettingsPrivacySectionText");

        SetCard(StartupCard, "SettingsStartupCardHeader", "SettingsStartupCardDescription");
        SetCard(AutoHideCard, "SettingsAutoHideCardHeader", "SettingsAutoHideCardDescription");
        SetCard(MonitorTextCard, "SettingsMonitorTextCardHeader", "SettingsMonitorTextCardDescription");
        SetCard(MonitorImagesCard, "SettingsMonitorImagesCardHeader", "SettingsMonitorImagesCardDescription");
        SetCard(ImageOcrCard, "SettingsImageOcrCardHeader", "SettingsImageOcrCardDescription");
        SetCard(MonitorFilesCard, "SettingsMonitorFilesCardHeader", "SettingsMonitorFilesCardDescription");
        SetCard(ThemeCard, "SettingsThemeCardHeader", "SettingsThemeCardDescription");
        SetCard(DensityCard, "SettingsDensityCardHeader", "SettingsDensityCardDescription");
        SetCard(LanguageCard, "SettingsLanguageCardHeader", "SettingsLanguageCardDescription");
        SetCard(GlobalHotKeyCard, "SettingsGlobalHotKeyCardHeader", "SettingsGlobalHotKeyCardDescription");
        SetCard(PasteShortcutCard, "SettingsPasteShortcutCardHeader");
        SetCard(PlainTextShortcutCard, "SettingsPlainTextShortcutCardHeader");
        SetCard(PreviewShortcutCard, "SettingsPreviewShortcutCardHeader");
        SetCard(PinShortcutCard, "SettingsPinShortcutCardHeader");
        SetCard(DeleteShortcutCard, "SettingsDeleteShortcutCardHeader");
        SetCard(PasteAsFileShortcutCard, "SettingsPasteAsFileShortcutCardHeader");
        SetCard(RetentionCard, "SettingsRetentionCardHeader", "SettingsRetentionCardDescription");
        SetCard(MaxEntriesCard, "SettingsMaxEntriesCardHeader", "SettingsMaxEntriesCardDescription");
        SetCard(ExcludedAppsCard, "SettingsExcludedAppsCardHeader", "SettingsExcludedAppsCardDescription");
        SetCard(BackupCard, "SettingsBackupCardHeader", "SettingsBackupCardDescription");

        ThemeSystemOption.Content = AppLocalization.GetString("ThemeSystemOptionContent");
        ThemeLightOption.Content = AppLocalization.GetString("ThemeLightOptionContent");
        ThemeDarkOption.Content = AppLocalization.GetString("ThemeDarkOptionContent");
        DensityCompactOption.Content = AppLocalization.GetString("DensityCompactOptionContent");
        DensityComfortableOption.Content = AppLocalization.GetString("DensityComfortableOptionContent");
        LanguageSystemOption.Content = AppLocalization.GetString("LanguageSystemOptionContent");
        LanguageChineseOption.Content = AppLocalization.GetString("LanguageChineseOptionContent");
        LanguageEnglishOption.Content = AppLocalization.GetString("LanguageEnglishOptionContent");

        var shortcutPlaceholder = AppLocalization.GetString("ShortcutPlaceholder");
        HotKeyTextBox.PlaceholderText = shortcutPlaceholder;
        PasteShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        PlainTextPasteShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        PreviewShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        PinShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        DeleteShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        PasteAsFileShortcutTextBox.PlaceholderText = shortcutPlaceholder;
        ShortcutHintText.Text = AppLocalization.GetString("SettingsShortcutHintText");
        PinnedRetentionHintText.Text = AppLocalization.GetString("SettingsPinnedRetentionHintText");
        ExcludedApplicationsTextBox.PlaceholderText = AppLocalization.GetString("ExcludedApplicationsTextBoxPlaceholder");
        SelectProcessButton.Content = AppLocalization.GetString("SelectProcessButtonContent");
        ExportBackupButton.Content = AppLocalization.GetString("ExportBackupButtonContent");
        RestoreBackupButton.Content = AppLocalization.GetString("RestoreBackupButtonContent");
        ProtectionTitleText.Text = AppLocalization.GetString("SettingsProtectionTitleText");
        ProtectionDescriptionText.Text = AppLocalization.GetString("SettingsProtectionDescriptionText");
        RestoreDefaultsButton.Content = AppLocalization.GetString("RestoreDefaultsButtonContent");

        var toggleOnContent = AppLocalization.GetString("ToggleOnContent");
        var toggleOffContent = AppLocalization.GetString("ToggleOffContent");
        SetToggleContent(StartWithWindowsToggleSwitch, toggleOnContent, toggleOffContent);
        SetToggleContent(AutoHideToggleSwitch, toggleOnContent, toggleOffContent);
        SetToggleContent(MonitorTextToggleSwitch, toggleOnContent, toggleOffContent);
        SetToggleContent(MonitorImagesToggleSwitch, toggleOnContent, toggleOffContent);
        SetToggleContent(ImageOcrToggleSwitch, toggleOnContent, toggleOffContent);
        SetToggleContent(MonitorFilesToggleSwitch, toggleOnContent, toggleOffContent);

        _isRefreshingLocalization = true;
        try
        {
            SelectComboItem(ThemeComboBox, themeValue);
            SelectComboItem(DensityComboBox, densityValue);
            SelectComboItem(LanguageComboBox, languageValue);
            RefreshComboBoxSelection(ThemeComboBox);
            RefreshComboBoxSelection(DensityComboBox);
            RefreshComboBoxSelection(LanguageComboBox);
        }
        finally
        {
            _isRefreshingLocalization = false;
        }
    }

    private static void SetCard(SettingsCard card, string headerKey, string? descriptionKey = null)
    {
        card.Header = AppLocalization.GetString(headerKey);
        card.Description = descriptionKey is null
            ? string.Empty
            : AppLocalization.GetString(descriptionKey);
    }

    // 显式设置开关文案，语言切换时同步更新。
    private static void SetToggleContent(ToggleSwitch toggleSwitch, string onContent, string offContent)
    {
        toggleSwitch.OnContent = onContent;
        toggleSwitch.OffContent = offContent;
    }

    private void ApplyCurrentSettings()
    {
        if (_isLoadingSettings)
        {
            return;
        }

        var newSettings = ReadCurrentSettings();
        var languageChanged = !string.Equals(
            newSettings.Language,
            _appliedLanguage,
            StringComparison.Ordinal);
        try
        {
            ApplyStartWithWindows(newSettings.StartWithWindows);
            _store.Save(newSettings);
            if (languageChanged)
            {
                AppLocalization.SetLanguage(newSettings.Language);
            }

            ApplyThemeSettings(newSettings.ThemeMode);
            SettingsChanged?.Invoke(newSettings);
            _appliedLanguage = newSettings.Language;
            if (languageChanged)
            {
                RefreshLocalization();
            }
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
        picker.FileTypeChoices.Add(AppLocalization.GetString("EncryptedBackupFileType"), [".pobackup"]);
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
        var file = await picker.PickSaveFileAsync();
        if (file is null)
        {
            return;
        }

        try
        {
            await _exportBackup(file.Path);
            await ShowMessageAsync(
                AppLocalization.GetString("BackupExportedTitle"),
                AppLocalization.GetString("BackupExportedMessage"));
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or CryptographicException)
        {
            await ShowMessageAsync(AppLocalization.GetString("BackupExportFailed"), exception.Message);
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
            Title = AppLocalization.GetString("RestoreBackupTitle"),
            Content = AppLocalization.GetString("RestoreBackupMessage"),
            PrimaryButtonText = AppLocalization.GetString("Restore"),
            CloseButtonText = AppLocalization.GetString("Cancel"),
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
            await ShowMessageAsync(
                AppLocalization.GetString("RestoreCompletedTitle"),
                AppLocalization.GetString("RestoreCompletedMessage"));
        }
        catch (Exception exception) when (exception is IOException
                                           or UnauthorizedAccessException
                                           or CryptographicException
                                           or InvalidDataException)
        {
            await ShowMessageAsync(AppLocalization.GetString("RestoreFailed"), exception.Message);
        }
    }

    private async Task ShowMessageAsync(string title, string message)
    {
        var dialog = new ContentDialog
        {
            Title = title,
            Content = message,
            CloseButtonText = AppLocalization.GetString("Ok"),
            XamlRoot = SettingsRoot.XamlRoot
        };
        await dialog.ShowAsync();
    }

    private async Task ShowSaveErrorAsync()
    {
        var dialog = new ContentDialog
        {
            Title = "PasteOrbit",
            Content = AppLocalization.GetString("SettingsSaveFailed"),
            CloseButtonText = AppLocalization.GetString("Ok"),
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
            ?? throw new InvalidOperationException(AppLocalization.GetString("StartupRegistryOpenFailed"));
        if (enabled)
        {
            var processPath = Environment.ProcessPath
                ?? throw new InvalidOperationException(AppLocalization.GetString("ApplicationPathUnavailable"));
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
