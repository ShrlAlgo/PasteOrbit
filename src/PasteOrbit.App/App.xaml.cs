using System.Threading;
using System.Windows;
using System.Windows.Forms;

using PasteOrbit.Core;

namespace PasteOrbit.App;

public partial class App : System.Windows.Application
{
    private const string MutexName = "Local\\PasteOrbit.SingleInstance";
    private Mutex? _instanceMutex;
    private NotifyIcon? _trayIcon;
    private ClipboardRepository? _repository;
    private MainWindow? _mainWindow;
    private SettingsWindow? _settingsWindow;

    public App()
    {
        Startup += OnStartup;
        Exit += OnExit;
        DispatcherUnhandledException += (_, args) =>
        {
            MessageBox.Show(args.Exception.Message, "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };
    }

    public AppSettings Settings { get; private set; } = new();
    public AppSettingsStore SettingsStore { get; private set; } = null!;
    public string DataDirectory { get; private set; } = string.Empty;
    public ClipboardRepository Repository => _repository!;

    public void ShowMainWindow(IntPtr targetWindow = default) => _mainWindow?.ShowPanel(targetWindow);

    public void ShowSettings()
    {
        if (_settingsWindow is null)
        {
            _settingsWindow = new SettingsWindow(this);
            _settingsWindow.Closed += (_, _) => _settingsWindow = null;
        }

        _settingsWindow.Show();
        _settingsWindow.Activate();
    }

    public void ExitApplication()
    {
        _settingsWindow?.Close();
        _mainWindow?.CloseForExit();
        Shutdown();
    }

    public void ApplyTheme(string themeMode)
    {
        ThemeMode = themeMode switch
        {
            "Light" => System.Windows.ThemeMode.Light,
            "Dark" => System.Windows.ThemeMode.Dark,
            _ => System.Windows.ThemeMode.System
        };
    }

    private void OnStartup(object sender, StartupEventArgs e)
    {
        _instanceMutex = new Mutex(true, MutexName, out var isFirstInstance);
        if (!isFirstInstance)
        {
            Shutdown();
            return;
        }

        DataDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PasteOrbit");
        Directory.CreateDirectory(DataDirectory);
        SettingsStore = new AppSettingsStore(Path.Combine(DataDirectory, "settings.json"));
        Settings = SettingsStore.Load();
        AppLocalization.SetLanguage(Settings.Language);
        ApplyTheme(Settings.ThemeMode);

        _repository = new ClipboardRepository(Path.Combine(DataDirectory, "history.db"));
        _repository.Initialize();
        _mainWindow = new MainWindow(this, new ClipboardHistory(_repository));
        MainWindow = _mainWindow;
        CreateTrayIcon();
        _mainWindow.ShowPanel();
    }

    private void CreateTrayIcon()
    {
        var menu = new ContextMenuStrip();
        menu.Items.Add(AppLocalization.GetString("TrayOpenHistory"), null, (_, _) => ShowMainWindow());
        menu.Items.Add(AppLocalization.GetString("TraySettings"), null, (_, _) => ShowSettings());
        menu.Items.Add(AppLocalization.GetString("TrayCheckForUpdates"), null, async (_, _) => await CheckForUpdatesAsync());
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(AppLocalization.GetString("TrayExit"), null, (_, _) => ExitApplication());
        _trayIcon = new NotifyIcon
        {
            Icon = new System.Drawing.Icon(Path.Combine(AppContext.BaseDirectory, "Assets", "PasteOrbit.ico")),
            Text = "PasteOrbit",
            ContextMenuStrip = menu,
            Visible = true
        };
        _trayIcon.DoubleClick += (_, _) => ShowMainWindow();
    }

    private async Task CheckForUpdatesAsync()
    {
        try
        {
            using var service = new UpdateCheckService();
            var update = await service.CheckAsync();
            var message = update is { IsUpdateAvailable: true }
                ? $"{AppLocalization.GetString("UpdateAvailableTitle")} {update.LatestVersion}"
                : AppLocalization.GetString("UpdateNoUpdateMessage");
            MessageBox.Show(message, "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception exception)
        {
            MessageBox.Show(exception.Message, "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void OnExit(object sender, ExitEventArgs e)
    {
        if (_trayIcon is not null)
        {
            _trayIcon.Visible = false;
            _trayIcon.Dispose();
        }

        _repository?.Compact();
        if (_instanceMutex is null)
        {
            return;
        }

        try
        {
            _instanceMutex.ReleaseMutex();
        }
        catch (ApplicationException)
        {
        }

        _instanceMutex.Dispose();
    }

}
