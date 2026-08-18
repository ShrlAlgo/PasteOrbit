using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Windows.Globalization;
using Microsoft.UI.Xaml;

namespace PasteOrbit.App;

/// <summary>
/// 应用生命周期入口，负责语言覆盖、单实例唤醒和主窗口释放。
/// </summary>
public partial class App : Application
{
    private const string SingleInstanceMutexName = @"Local\PasteOrbit.SingleInstance";
    private static readonly IntPtr HwndBroadcast = new(-1);
    internal static readonly uint ShowExistingInstanceMessage = RegisterWindowMessage("PasteOrbit.ShowExistingInstance");
    // 在应用覆盖语言前记录系统首选语言，运行时切回“跟随系统”时仍能恢复到正确资源。
    internal static readonly string SystemLanguage = ResolveSystemLanguage();

    private MainWindow? _mainWindow;
    private Mutex? _singleInstanceMutex;
    private readonly bool _isPrimaryInstance;
    private bool _isExiting;

    public App()
    {
        var settingsPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "PasteOrbit",
            "settings.json");
        var language = new AppSettingsStore(settingsPath).Load().Language;
        if (!string.IsNullOrEmpty(language))
        {
            ApplicationLanguages.PrimaryLanguageOverride = language;
        }

        InitializeComponent();
        _singleInstanceMutex = new Mutex(true, SingleInstanceMutexName, out var createdNew);
        _isPrimaryInstance = createdNew;
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // 非主实例只发送唤醒消息，不重复创建托盘、监听器和窗口。
        if (!_isPrimaryInstance)
        {
            // 第二个实例只负责唤醒首个实例，随后立即退出，不创建窗口和后台服务。
            PostMessage(HwndBroadcast, ShowExistingInstanceMessage, IntPtr.Zero, IntPtr.Zero);
            _singleInstanceMutex?.Dispose();
            _singleInstanceMutex = null;
            Environment.Exit(0);
            return;
        }

        _mainWindow = new MainWindow();
        _mainWindow.Closed += (_, _) => ReleaseSingleInstance();
        // 先完成窗口尺寸、位置和原生句柄配置，再首次显示，避免面板在默认位置闪现。
        _mainWindow.InitializeNative();
        _mainWindow.Activate();
    }

    internal void ExitApplication()
    {
        // 统一关闭主窗口及其后台资源，保证单实例互斥体最终释放。
        if (_isExiting)
        {
            return;
        }

        _isExiting = true;
        _mainWindow?.ExitApplication();
    }

    private void ReleaseSingleInstance()
    {
        if (_singleInstanceMutex is null)
        {
            return;
        }

        try
        {
            _singleInstanceMutex.ReleaseMutex();
        }
        catch (ApplicationException)
        {
        }

        _singleInstanceMutex.Dispose();
        _singleInstanceMutex = null;
    }

    private static string ResolveSystemLanguage()
    {
        // 只映射应用提供的语言，其他系统语言使用中文资源。
        foreach (var language in ApplicationLanguages.Languages)
        {
            if (language.StartsWith("en", StringComparison.OrdinalIgnoreCase))
            {
                return "en-US";
            }

            if (language.StartsWith("zh", StringComparison.OrdinalIgnoreCase))
            {
                return "zh-CN";
            }
        }

        return "zh-CN";
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint RegisterWindowMessage(string message);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessage(IntPtr windowHandle, uint message, IntPtr wParam, IntPtr lParam);

}
