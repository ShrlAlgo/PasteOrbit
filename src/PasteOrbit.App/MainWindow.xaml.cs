using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Input;
using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

using PasteOrbit.Core;

using Windows.Graphics;
using Windows.Storage.Streams;
using Launcher = Windows.System.Launcher;

using WinRT.Interop;

using VirtualKey = Windows.System.VirtualKey;

namespace PasteOrbit.App;

/// <summary>
/// 剪贴板历史面板主窗口，协调监听、持久化、粘贴、托盘和设置窗口。
/// </summary>
public sealed partial class MainWindow : Window
{
    private const int HistoryPageSize = 50;
    private const int HistoryLoadThreshold = 10;
    private const int SwHide = 0;
    private const int SwShow = 5;
    private const uint SwpNoSize = 0x0001;
    private const uint SwpNoMove = 0x0002;
    private const uint SwpFrameChanged = 0x0020;
    private const uint SwpNoActivate = 0x0010;
    private const uint SwpShowWindow = 0x0040;
    private const int SmCxdrag = 68;
    private const int SmCydrag = 69;
    private const int VkLbutton = 0x01;
    private const int VkRbutton = 0x02;
    private const int VkMbutton = 0x04;
    private const int GwlExstyle = -20;
    private const long WsExToolwindow = 0x00000080L;
    private const long WsExAppwindow = 0x00040000L;
    private const long WsExNoActivate = 0x08000000L;
    private const nint HwndTopmost = -1;
    private const nint HwndNotopmost = -2;
    private const uint SpiGetWorkArea = 0x0030;
    private const uint MonitorDefaultTnearest = 2;
    private const int UiaBoundingRectanglePropertyId = 30001;
    private const int UiaProcessIdPropertyId = 30002;
    private const int UiaControlTypePropertyId = 30003;
    private const int UiaHasKeyboardFocusPropertyId = 30008;
    private const int UiaTextPattern2Id = 10024;
    private static readonly Guid CuiAutomationClsid = new("ff48dba4-60ef-4201-aa87-54103eef594e");
    private static readonly Guid UiAutomationTextPattern2Iid = new("506a921a-fcc9-409f-b23b-37eb74106872");

    private readonly ObservableCollection<HistoryListItem> _displayItems = [];
    private readonly HistoryListItem?[] _quickPasteItems = new HistoryListItem?[9];
    private readonly AppSettingsStore _settingsStore;
    private readonly ClipboardHistory _history;
    private readonly GlobalHotKey _hotKey = new();
    private readonly ClipboardMonitor _monitor = new();
    private readonly ClipboardRepository _repository;
    private readonly ImageOcrService _ocrService;
    private readonly LocalBackupService _backupService;
    private readonly UpdateCheckService _updateCheckService = new();
    private readonly HashSet<string> _excludedApplications = new(StringComparer.OrdinalIgnoreCase);
    private CancellationTokenSource? _historyQueryCancellation;
    private Task _historyLoadTask = Task.CompletedTask;
    private AppSettings _settings;
    private ClipboardContentKind? _selectedKind;
    private ClipboardHistoryQuery _currentHistoryQuery = new();
    private ClipboardHistoryCursor? _nextHistoryCursor;
    private HistoryListItem? _hoveredHistoryItem;
    private HistoryListItem? _previewedHistoryItem;
    private FrameworkElement? _previewedHistoryCard;
    private IntPtr _pasteTarget;
    private MonitorRect _pasteInputBounds;
    private IntPtr _handle;
    private Win32MessageBridge? _messageBridge;
    private NativeTrayIcon? _trayIcon;
    private AppWindow? _appWindow;
    private DispatcherQueue? _dispatcherQueue;
    private DispatcherQueueTimer? _panelMonitorTimer;
    private DispatcherQueueTimer? _listeningPauseTimer;
    private SettingsWindow? _settingsWindow;
    private bool _settingsWindowOpen;
    private bool _automaticUpdateCheckStarted;
    private bool _isCheckingForUpdates;
    private bool _storageAvailable = true;
    private bool _nativeInitialized;
    private bool _hasPasteInputBounds;
    private bool _panelShownWithoutActivation;
    private bool _isTopmost;
    private bool _isExiting;
    private bool _isListeningPaused;
    private bool _isLoadingHistory;
    private int _historyLoadVersion;
    private int _historyTotalCount;
    private int _historyUnpinnedCount;
    private IntPtr _panelTargetWindow;
    private uint? _headerDragPointerId;
    private int _headerDragStartScreenX;
    private int _headerDragStartScreenY;
    private int _headerDragStartWindowX;
    private int _headerDragStartWindowY;
    private bool _headerDragPending;
    private bool _headerDragActive;
    private string? _startupError;

    public MainWindow()
    {
        InitializeComponent();
        HistoryList.ItemsSource = _displayItems;
        SearchBox.KeyDown += Input_KeyDown;
        SearchBox.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler(SearchBox_PointerPressed), true);
        HistoryList.PreviewKeyDown += Input_KeyDown;
        Activated += MainWindow_Activated;
        Closed += MainWindow_Closed;

        var dataDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "PasteOrbit");
        _settingsStore = new AppSettingsStore(Path.Combine(dataDirectory, "settings.json"));
        _settings = _settingsStore.Load();
        if (GlobalHotKey.TryNormalizeShortcut(_settings.GlobalHotKey, out var normalizedHotKey))
        {
            _settings.GlobalHotKey = normalizedHotKey;
        }
        else
        {
            _settings.GlobalHotKey = new AppSettings().GlobalHotKey;
        }

        NormalizePanelShortcuts(_settings);
        UpdateExcludedApplications();

        ApplyThemeSettings();
        _repository = new ClipboardRepository(Path.Combine(dataDirectory, "history.db"));
        _history = new ClipboardHistory(_repository);
        _backupService = new LocalBackupService(_repository.DatabasePath, _settingsStore.Path);
        _ocrService = new ImageOcrService(_repository.LoadContent);
        _ocrService.Recognized += OcrService_Recognized;
        _ocrService.RecognitionFailed += OcrService_RecognitionFailed;

        try
        {
            _history.Initialize();
            CleanupHistory();
        }
        catch (Exception)
        {
            _storageAvailable = false;
            _startupError = AppLocalization.GetString("StorageInitializationFailed");
        }

        _monitor.Captured += Monitor_Captured;
        _monitor.CaptureFailed += Monitor_CaptureFailed;
        _hotKey.Pressed += HotKey_Pressed;
        RefreshHistory();
    }

    public void InitializeNative()
    {
        if (_nativeInitialized)
        {
            return;
        }

        _nativeInitialized = true;
        // 原生句柄和托盘先于首次显示初始化，避免面板短暂出现在默认位置。
        _dispatcherQueue = DispatcherQueue.GetForCurrentThread();
        _panelMonitorTimer = _dispatcherQueue.CreateTimer();
        _panelMonitorTimer.Interval = TimeSpan.FromMilliseconds(100);
        _panelMonitorTimer.Tick += PanelMonitorTimer_Tick;
        _listeningPauseTimer = _dispatcherQueue.CreateTimer();
        _listeningPauseTimer.IsRepeating = false;
        _listeningPauseTimer.Interval = TimeSpan.FromMinutes(10);
        _listeningPauseTimer.Tick += ListeningPauseTimer_Tick;
        _handle = WindowNative.GetWindowHandle(this);
        _messageBridge = new Win32MessageBridge(_handle);
        _messageBridge.Message += MessageBridge_Message;
        _messageBridge.CloseRequested += MessageBridge_CloseRequested;
        _appWindow = AppWindow.GetFromWindowId(Win32Interop.GetWindowIdFromWindow(_handle));
        _appWindow.Closing += AppWindow_Closing;
        ConfigureWindow();

        var iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "PasteOrbit.ico");
        try
        {
            _trayIcon = new NativeTrayIcon(_messageBridge, iconPath);
            _trayIcon.OpenRequested += ShowFromTray;
            _trayIcon.PauseRequested += TrayPauseRequested;
            _trayIcon.SettingsRequested += TraySettingsRequested;
            _trayIcon.CheckForUpdatesRequested += TrayCheckForUpdatesRequested;
            _trayIcon.ExitRequested += TrayExitRequested;
            _trayIcon.ContextMenuRequested += TrayContextMenuRequested;
            _trayIcon.SetTheme(WindowSurface.RequestedTheme);
        }
        catch (Exception exception)
        {
            System.Diagnostics.Debug.WriteLine($"托盘图标初始化失败：{exception}");
        }

        try
        {
            if (_storageAvailable)
            {
                _monitor.Start(_messageBridge, _dispatcherQueue);
                _hotKey.Start(_messageBridge, _settings.GlobalHotKey);
                StatusText.Text = _repository.LastRecoveryPath is null
                    ? AppLocalization.Format("ListeningStatus", _history.Count)
                    : AppLocalization.GetString("DatabaseRecovered");
            }
            else
            {
                StatusText.Text = _startupError;
            }
        }
        catch (Exception exception)
        {
            _storageAvailable = false;
            _monitor.Dispose();
            _hotKey.Dispose();
            StatusText.Text = AppLocalization.Format("InitializationFailed", exception.Message);
        }

        PositionWindow();
    }

    public void ExitApplication()
    {
        if (_isExiting)
        {
            return;
        }

        _isExiting = true;
        _settingsWindow?.Close();
        _trayIcon?.Dispose();
        _monitor.Dispose();
        _ocrService.Dispose();
        _updateCheckService.Dispose();
        _hotKey.Dispose();
        _messageBridge?.Dispose();
        Close();
    }

    private void ConfigureWindow()
    {
        if (_appWindow is null)
        {
            return;
        }

        _appWindow.Resize(new SizeInt32(480, 580));
        try
        {
            _appWindow.IsShownInSwitchers = false;
        }
        catch (NotImplementedException)
        {
            // 当前 Windows App SDK 的 unpackaged 模式未实现该属性，用原生扩展样式隐藏任务栏按钮。
            ApplyToolWindowStyle();
        }
        var presenter = OverlappedPresenter.Create();
        presenter.IsResizable = false;
        presenter.IsMaximizable = false;
        presenter.IsMinimizable = false;
        // 主面板使用无原生标题栏的弹出式窗口，避免系统标题栏按钮残留 hover 状态。
        presenter.SetBorderAndTitleBar(true, false);
        ExtendsContentIntoTitleBar = false;
        _appWindow.SetPresenter(presenter);
        var iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "PasteOrbit.ico");
        if (File.Exists(iconPath))
        {
            _appWindow.SetIcon(iconPath);
        }
    }

    private void ApplyToolWindowStyle()
    {
        var extendedStyle = GetWindowLongPtr(_handle, GwlExstyle).ToInt64();
        extendedStyle = (extendedStyle | WsExToolwindow) & ~WsExAppwindow;
        SetWindowLongPtr(_handle, GwlExstyle, new nint(extendedStyle));
    }

    private void SetPanelActivationMode(bool preventActivation)
    {
        if (_handle == IntPtr.Zero)
        {
            return;
        }

        var extendedStyle = GetWindowLongPtr(_handle, GwlExstyle).ToInt64();
        extendedStyle = preventActivation
            ? extendedStyle | WsExNoActivate
            : extendedStyle & ~WsExNoActivate;
        SetWindowLongPtr(_handle, GwlExstyle, new nint(extendedStyle));
        if (_messageBridge is not null)
        {
            _messageBridge.PreventActivation = preventActivation;
        }

        SetWindowPos(
            _handle,
            _isTopmost ? HwndTopmost : HwndNotopmost,
            0,
            0,
            0,
            0,
            SwpNoMove | SwpNoSize | SwpNoActivate | SwpFrameChanged);
    }

    private void HotKey_Pressed(IntPtr foregroundWindow)
    {
        CapturePasteTarget(foregroundWindow);

        EnqueueOnUi(() =>
        {
            PositionWindow();
            ShowPanel(activatePanel: true);
        });
    }

    private void ShowFromTray()
    {
        CapturePasteTarget(GetForegroundWindow());

        EnqueueOnUi(() =>
        {
            PositionWindow();
            ShowPanel(activatePanel: true);
        });
    }

    private void TraySettingsRequested()
    {
        EnqueueOnUi(OpenSettings);
    }

    private void TrayCheckForUpdatesRequested()
    {
        EnqueueOnUi(() => _ = CheckForUpdatesFromTrayAsync());
    }

    private async Task CheckForUpdatesFromTrayAsync()
    {
        if (_isCheckingForUpdates || _isExiting)
        {
            return;
        }

        _isCheckingForUpdates = true;
        var restorePanelVisibility = _handle != IntPtr.Zero && !IsWindowVisible(_handle);
        try
        {
            // ContentDialog 需要可见的 XamlRoot。托盘打开检查时临时显示主窗口，结束后恢复隐藏状态。
            if (restorePanelVisibility)
            {
                ShowPanel(activatePanel: true);
            }

            var result = await CheckForUpdatesAsync();
            if (result is null)
            {
                await ShowUpdateMessageAsync(
                    AppLocalization.GetString("UpdateCheckFailedTitle"),
                    AppLocalization.GetString("UpdateCheckFailedMessage"));
            }
            else if (!result.IsUpdateAvailable)
            {
                await ShowUpdateMessageAsync(
                    AppLocalization.GetString("UpdateNoUpdateTitle"),
                    AppLocalization.Format("UpdateNoUpdateMessage", result.CurrentVersion));
            }
            else
            {
                await ShowUpdateDialogAsync(result);
            }
        }
        catch (Exception exception)
        {
            System.Diagnostics.Debug.WriteLine($"托盘检查更新失败：{exception}");
            if (!_isExiting)
            {
                try
                {
                    await ShowUpdateMessageAsync(
                        AppLocalization.GetString("UpdateCheckFailedTitle"),
                        AppLocalization.GetString("UpdateCheckFailedMessage"));
                }
                catch (Exception dialogException)
                {
                    System.Diagnostics.Debug.WriteLine($"显示托盘更新失败提示失败：{dialogException}");
                }
            }
        }
        finally
        {
            _isCheckingForUpdates = false;
            if (restorePanelVisibility && !_isExiting)
            {
                HidePanel();
            }
        }
    }

    private async Task ShowUpdateMessageAsync(string title, string message)
    {
        if (RootGrid.XamlRoot is not { } xamlRoot)
        {
            return;
        }

        var dialog = new ContentDialog
        {
            Title = title,
            Content = message,
            CloseButtonText = AppLocalization.GetString("Ok"),
            XamlRoot = xamlRoot
        };
        await dialog.ShowAsync();
    }

    private void TrayPauseRequested()
    {
        EnqueueOnUi(() =>
        {
            if (_isListeningPaused)
            {
                ResumeClipboardListening();
            }
            else
            {
                PauseClipboardListening();
            }
        });
    }

    private void PauseClipboardListening()
    {
        if (_isListeningPaused || !_storageAvailable)
        {
            return;
        }

        _isListeningPaused = true;
        _monitor.SuspendCapture();
        if (_trayIcon is not null)
        {
            _trayIcon.IsListeningPaused = true;
        }

        _listeningPauseTimer?.Start();
        StatusText.Text = AppLocalization.GetString("MonitoringPaused");
    }

    private void ResumeClipboardListening()
    {
        if (!_isListeningPaused)
        {
            return;
        }

        _listeningPauseTimer?.Stop();
        _isListeningPaused = false;
        _monitor.ResumeCapture();
        if (_trayIcon is not null)
        {
            _trayIcon.IsListeningPaused = false;
        }

        StatusText.Text = AppLocalization.Format("ListeningStatus", _history.Count);
    }

    private void ListeningPauseTimer_Tick(DispatcherQueueTimer sender, object args)
    {
        ResumeClipboardListening();
    }

    private void TrayExitRequested()
    {
        EnqueueOnUi(() =>
        {
            if (Application.Current is App app)
            {
                app.ExitApplication();
            }
        });
    }

    private void TrayContextMenuRequested(int screenX, int screenY)
    {
        // 退出托盘回调后再创建 Flyout，避免 XAML 菜单重入 Win32 消息过程。
        _dispatcherQueue?.TryEnqueue(() => _trayIcon?.ShowContextMenu(screenX, screenY));
    }

    private void MainWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        if (args.WindowActivationState != WindowActivationState.Deactivated)
        {
            if (!_automaticUpdateCheckStarted && _nativeInitialized)
            {
                _automaticUpdateCheckStarted = true;
                _ = CheckForUpdatesOnStartupAsync();
            }

            if (_panelShownWithoutActivation)
            {
                _panelShownWithoutActivation = false;
                _panelMonitorTimer?.Stop();
                if (!_isTopmost)
                {
                    SetWindowPos(_handle, HwndNotopmost, 0, 0, 0, 0, SwpNoMove | SwpNoSize | SwpNoActivate);
                }
            }

            return;
        }

        CancelHeaderDrag();

        // 非激活弹窗不会收到稳定的失焦事件，由定时器根据前台窗口变化负责隐藏。
        if (_panelShownWithoutActivation)
        {
            return;
        }

        if (_settings.AutoHideOnDeactivate && !_isTopmost && !_settingsWindowOpen)
        {
            HidePanel();
        }
    }

    private void MainWindow_Closed(object sender, WindowEventArgs args)
    {
        CancelHeaderDrag();
        _historyQueryCancellation?.Cancel();
        _historyQueryCancellation?.Dispose();
        _historyQueryCancellation = null;
        _panelMonitorTimer?.Stop();
        _trayIcon?.Dispose();
        _monitor.Dispose();
        _ocrService.Dispose();
        _hotKey.Dispose();
        _messageBridge?.Dispose();
        DisposeDisplayItems();
    }

    private void AppWindow_Closing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (_isExiting)
        {
            return;
        }

        // 主窗口是托盘应用的常驻面板，点击系统关闭按钮只隐藏面板，不结束监听和托盘进程。
        args.Cancel = true;
        if (_dispatcherQueue is null || !_dispatcherQueue.TryEnqueue(HidePanel))
        {
            HidePanel();
        }
    }

    private bool MessageBridge_CloseRequested()
    {
        if (_isExiting)
        {
            return false;
        }

        HidePanel();
        return true;
    }

    private void MessageBridge_Message(uint message, IntPtr wParam, IntPtr lParam)
    {
        if (message != App.ShowExistingInstanceMessage)
        {
            return;
        }

        CapturePasteTarget(GetForegroundWindow());
        EnqueueOnUi(() =>
        {
            PositionWindow();
            ShowPanel(activatePanel: true);
        });
    }

    private void PositionWindow()
    {
        if (_appWindow is null)
        {
            return;
        }

        var windowSize = _appWindow.Size;
        var targetWindow = _pasteTarget != IntPtr.Zero ? _pasteTarget : GetForegroundWindow();
        if (_hasPasteInputBounds && targetWindow == _pasteTarget)
        {
            MoveNearInputBounds(windowSize, _pasteInputBounds);
            return;
        }

        if (TryGetActiveInputBounds(targetWindow, out var inputBounds))
        {
            MoveNearInputBounds(windowSize, inputBounds);
            return;
        }

        // 没有可读取的插入符或输入控件时，使用当前鼠标位置作为最后一个可用锚点。
        if (GetCursorPos(out var cursorPoint))
        {
            MoveNearInputBounds(windowSize, new MonitorRect
            {
                Left = cursorPoint.X,
                Top = cursorPoint.Y,
                Right = cursorPoint.X + 1,
                Bottom = cursorPoint.Y + 1
            });
            return;
        }

        var workArea = GetWorkArea();
        _appWindow.Move(new PointInt32(
            workArea.X + (workArea.Width - windowSize.Width) / 2,
            workArea.Y + (workArea.Height - windowSize.Height) / 2));
    }

    private void MoveNearInputBounds(SizeInt32 windowSize, MonitorRect inputBounds)
    {
        const int gap = 14;
        var inputPoint = new NativePoint { X = inputBounds.Left, Y = inputBounds.Top };
        var inputWorkArea = GetWorkArea(inputPoint);
        var left = inputBounds.Left;
        var top = inputBounds.Bottom + gap;
        if (left + windowSize.Width > inputWorkArea.X + inputWorkArea.Width)
        {
            left = inputBounds.Right - windowSize.Width;
        }

        if (top + windowSize.Height > inputWorkArea.Y + inputWorkArea.Height)
        {
            top = inputBounds.Top - windowSize.Height - gap;
        }

        _appWindow?.Move(new PointInt32(
            Math.Clamp(left, inputWorkArea.X, inputWorkArea.X + inputWorkArea.Width - windowSize.Width),
             Math.Clamp(top, inputWorkArea.Y, inputWorkArea.Y + inputWorkArea.Height - windowSize.Height)));
    }

    private void CapturePasteTarget(IntPtr foregroundWindow)
    {
        // 热键弹出前保存顶层窗口和具体输入控件，面板激活后按原焦点恢复粘贴。
        if (foregroundWindow == IntPtr.Zero || foregroundWindow == _handle)
        {
            if (_pasteTarget != IntPtr.Zero
                && GetWindowThreadProcessId(_pasteTarget, IntPtr.Zero) != 0)
            {
                return;
            }

            _pasteTarget = IntPtr.Zero;
            _pasteInputBounds = default;
            _hasPasteInputBounds = false;
            return;
        }

        _pasteTarget = foregroundWindow;
        _pasteInputBounds = default;
        _hasPasteInputBounds = TryGetActiveInputBounds(foregroundWindow, out _pasteInputBounds);

    }

    private bool TryGetActiveInputBounds(IntPtr foregroundWindow, out MonitorRect bounds)
    {
        bounds = default;
        if (foregroundWindow == IntPtr.Zero || foregroundWindow == _handle)
        {
            return false;
        }

        var threadId = GetWindowThreadProcessId(foregroundWindow, IntPtr.Zero);
        if (threadId == 0)
        {
            return false;
        }

        GetAutomationInputBounds(foregroundWindow, out var automationCaretBounds, out var automationControlBounds);
        // UI Automation 的 TextPattern2 可以直接返回文本插入符，优先于窗口级的 Win32 插入符。
        if (automationCaretBounds is MonitorRect caretBounds)
        {
            bounds = caretBounds;
            return true;
        }

        var threadInfo = new GuiThreadInfo { Size = (uint)Marshal.SizeOf<GuiThreadInfo>() };
        var hasThreadInfo = GetGUIThreadInfo(threadId, ref threadInfo);
        if (hasThreadInfo
            && TryGetCaretBounds(threadInfo.CaretWindow, threadInfo.CaretRect, out bounds))
        {
            return true;
        }

        // 部分 WinUI、WebView 和浏览器控件没有独立 Win32 插入符，只能退回到控件边界。
        if (automationControlBounds is MonitorRect controlBounds)
        {
            bounds = controlBounds;
            return true;
        }

        if (hasThreadInfo)
        {
            var inputWindow = threadInfo.CaretWindow;
            if (TryGetFocusedChildWindow(threadId, foregroundWindow, out var focusedWindow))
            {
                inputWindow = focusedWindow;
            }

            if (TryGetCaretScreenPoint(threadId, inputWindow, out var caretPoint))
            {
                bounds = new MonitorRect
                {
                    Left = caretPoint.X,
                    Top = caretPoint.Y,
                    Right = caretPoint.X + 1,
                    Bottom = caretPoint.Y + 20
                };
                return true;
            }
        }

        // 没有插入符或输入控件边界时由 PositionWindow 使用鼠标位置兜底。
        bounds = default;
        return false;
    }

    private static void GetAutomationInputBounds(
        IntPtr foregroundWindow,
        out MonitorRect? caretBounds,
        out MonitorRect? controlBounds)
    {
        caretBounds = null;
        controlBounds = null;
        IUiAutomation? automation = null;
        IUiAutomationElement? focusedElement = null;
        IUiAutomationTextPattern2? textPattern = null;
        IUiAutomationTextRange? caretRange = null;

        try
        {
            var targetThreadId = GetWindowThreadProcessId(foregroundWindow, out var targetProcessId);
            if (targetThreadId == 0 || targetProcessId == 0)
            {
                return;
            }

            var automationType = Type.GetTypeFromCLSID(CuiAutomationClsid, throwOnError: false);
            if (automationType is null)
            {
                return;
            }

            automation = Activator.CreateInstance(automationType) as IUiAutomation;
            if (automation is null
                || automation.GetFocusedElement(out focusedElement) < 0
                || focusedElement is null
                || focusedElement.GetCurrentPropertyValue(UiaHasKeyboardFocusPropertyId, out var hasKeyboardFocusValue) < 0
                || !Convert.ToBoolean(hasKeyboardFocusValue)
                || focusedElement.GetCurrentPropertyValue(UiaProcessIdPropertyId, out var processIdValue) < 0
                || Convert.ToUInt32(processIdValue) != targetProcessId)
            {
                return;
            }

            var interfaceId = UiAutomationTextPattern2Iid;
            var patternResult = focusedElement.GetCurrentPatternAs(
                    UiaTextPattern2Id,
                    ref interfaceId,
                    out var patternPointer);
            if (patternResult >= 0 && patternPointer != IntPtr.Zero)
            {
                try
                {
                    textPattern = Marshal.GetTypedObjectForIUnknown(
                        patternPointer,
                        typeof(IUiAutomationTextPattern2)) as IUiAutomationTextPattern2;
                }
                finally
                {
                    Marshal.Release(patternPointer);
                }
            }

            if (textPattern is not null
                && textPattern.GetCaretRange(out var isActive, out caretRange) >= 0
                && isActive
                && caretRange is not null
                && caretRange.GetBoundingRectangles(out var rectangles) >= 0
                && TryGetAutomationRectangle(rectangles, out var automationCaretBounds))
            {
                caretBounds = automationCaretBounds;
            }

            if (focusedElement.GetCurrentPropertyValue(UiaControlTypePropertyId, out var controlTypeValue) < 0
                || !IsInputControlType(Convert.ToInt32(controlTypeValue))
                || focusedElement.GetCurrentPropertyValue(UiaBoundingRectanglePropertyId, out var rectangleValue) < 0
                || rectangleValue is not Array rectangleValues
                || rectangleValues.Length < 4)
            {
                return;
            }

            double[] controlRectangle =
            [
                Convert.ToDouble(rectangleValues.GetValue(0)),
                Convert.ToDouble(rectangleValues.GetValue(1)),
                Convert.ToDouble(rectangleValues.GetValue(2)),
                Convert.ToDouble(rectangleValues.GetValue(3))
            ];
            if (TryGetAutomationRectangle(controlRectangle, out var automationControlBounds))
            {
                controlBounds = automationControlBounds;
            }
        }
        catch (Exception exception) when (exception is COMException
            or InvalidOperationException
            or InvalidCastException
            or ArgumentException
            or FormatException
            or OverflowException)
        {
            return;
        }
        finally
        {
            ReleaseComObject(caretRange);
            ReleaseComObject(textPattern);
            ReleaseComObject(focusedElement);
            ReleaseComObject(automation);
        }
    }

    private static bool TryGetAutomationRectangle(double[]? rectangles, out MonitorRect bounds)
    {
        bounds = default;
        if (rectangles is null || rectangles.Length < 4)
        {
            return false;
        }

        for (var index = 0; index + 3 < rectangles.Length; index += 4)
        {
            var left = rectangles[index];
            var top = rectangles[index + 1];
            var width = rectangles[index + 2];
            var height = rectangles[index + 3];
            if (double.IsNaN(left)
                || double.IsNaN(top)
                || double.IsNaN(width)
                || double.IsNaN(height)
                || double.IsInfinity(left)
                || double.IsInfinity(top)
                || double.IsInfinity(width)
                || double.IsInfinity(height)
                || width <= 0
                || height <= 0)
            {
                continue;
            }

            var leftInt = Convert.ToInt32(Math.Round(left));
            var topInt = Convert.ToInt32(Math.Round(top));
            var widthInt = Math.Max(1, Convert.ToInt32(Math.Round(width)));
            var heightInt = Math.Max(1, Convert.ToInt32(Math.Round(height)));
            bounds = new MonitorRect
            {
                Left = leftInt,
                Top = topInt,
                Right = leftInt + widthInt,
                Bottom = topInt + heightInt
            };
            return true;
        }

        return false;
    }

    private static bool IsInputControlType(int controlType)
    {
        // Java/Swing 控件可能以 Text、Custom 或 Pane 类型暴露输入焦点。
        return controlType is 50003 or 50004 or 50020 or 50025 or 50030 or 50033;
    }

    private static void ReleaseComObject(object? value)
    {
        if (value is not null && Marshal.IsComObject(value))
        {
            Marshal.FinalReleaseComObject(value);
        }
    }

    [ComImport]
    [Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IUiAutomation
    {
        [PreserveSig]
        int CompareElements(IntPtr firstElement, IntPtr secondElement, out int areSame);

        [PreserveSig]
        int CompareRuntimeIds(IntPtr firstRuntimeId, IntPtr secondRuntimeId, out int areSame);

        [PreserveSig]
        int GetRootElement(out IUiAutomationElement rootElement);

        [PreserveSig]
        int ElementFromHandle(IntPtr windowHandle, out IUiAutomationElement element);

        [PreserveSig]
        int ElementFromPoint(NativePoint point, out IUiAutomationElement element);

        [PreserveSig]
        int GetFocusedElement(out IUiAutomationElement element);
    }

    [ComImport]
    [Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IUiAutomationElement
    {
        [PreserveSig]
        int SetFocus();

        [PreserveSig]
        int GetRuntimeId(out IntPtr runtimeId);

        [PreserveSig]
        int FindFirst(int scope, IntPtr condition, out IntPtr foundElement);

        [PreserveSig]
        int FindAll(int scope, IntPtr condition, out IntPtr foundElements);

        [PreserveSig]
        int FindFirstBuildCache(int scope, IntPtr condition, IntPtr cacheRequest, out IntPtr foundElement);

        [PreserveSig]
        int FindAllBuildCache(int scope, IntPtr condition, IntPtr cacheRequest, out IntPtr foundElements);

        [PreserveSig]
        int BuildUpdatedCache(IntPtr cacheRequest, out IntPtr updatedElement);

        [PreserveSig]
        int GetCurrentPropertyValue(
            int propertyId,
            [MarshalAs(UnmanagedType.Struct)] out object? value);

        [PreserveSig]
        int GetCurrentPropertyValueEx(
            int propertyId,
            [MarshalAs(UnmanagedType.Bool)] bool ignoreDefaultValue,
            [MarshalAs(UnmanagedType.Struct)] out object? value);

        [PreserveSig]
        int GetCachedPropertyValue(
            int propertyId,
            [MarshalAs(UnmanagedType.Struct)] out object? value);

        [PreserveSig]
        int GetCachedPropertyValueEx(
            int propertyId,
            [MarshalAs(UnmanagedType.Bool)] bool ignoreDefaultValue,
            [MarshalAs(UnmanagedType.Struct)] out object? value);

        [PreserveSig]
        int GetCurrentPatternAs(
            int patternId,
            [In] ref Guid interfaceId,
            out IntPtr patternObject);
    }

    [ComImport]
    [Guid("506a921a-fcc9-409f-b23b-37eb74106872")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IUiAutomationTextPattern2
    {
        [PreserveSig]
        int RangeFromPoint(NativePoint point, out IUiAutomationTextRange range);

        [PreserveSig]
        int RangeFromChild(IntPtr child, out IUiAutomationTextRange range);

        [PreserveSig]
        int GetSelection(out IntPtr ranges);

        [PreserveSig]
        int GetVisibleRanges(out IntPtr ranges);

        [PreserveSig]
        int GetDocumentRange(out IUiAutomationTextRange range);

        [PreserveSig]
        int GetSupportedTextSelection(out int supportedTextSelection);

        [PreserveSig]
        int RangeFromAnnotation(IntPtr annotation, out IUiAutomationTextRange range);

        [PreserveSig]
        int GetCaretRange(
            [MarshalAs(UnmanagedType.Bool)] out bool isActive,
            out IUiAutomationTextRange range);
    }

    [ComImport]
    [Guid("a543cc6a-f4ae-494b-8239-c814481187a8")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IUiAutomationTextRange
    {
        [PreserveSig]
        int Clone(out IUiAutomationTextRange range);

        [PreserveSig]
        int Compare(IUiAutomationTextRange range, [MarshalAs(UnmanagedType.Bool)] out bool areSame);

        [PreserveSig]
        int CompareEndpoints(int sourceEndpoint, IUiAutomationTextRange range, int targetEndpoint, out int comparison);

        [PreserveSig]
        int ExpandToEnclosingUnit(int textUnit);

        [PreserveSig]
        int FindAttribute(int attributeId, [MarshalAs(UnmanagedType.Struct)] object value, [MarshalAs(UnmanagedType.Bool)] bool backward, out IUiAutomationTextRange range);

        [PreserveSig]
        int FindText([MarshalAs(UnmanagedType.BStr)] string text, [MarshalAs(UnmanagedType.Bool)] bool backward, [MarshalAs(UnmanagedType.Bool)] bool ignoreCase, out IUiAutomationTextRange range);

        [PreserveSig]
        int GetAttributeValue(int attributeId, [MarshalAs(UnmanagedType.Struct)] out object? value);

        [PreserveSig]
        int GetBoundingRectangles([MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_R8)] out double[] rectangles);

        [PreserveSig]
        int GetEnclosingElement(out IUiAutomationElement element);

        [PreserveSig]
        int GetText(int maxLength, [MarshalAs(UnmanagedType.BStr)] out string text);

        [PreserveSig]
        int Move(int textUnit, int count, out int moved);

        [PreserveSig]
        int MoveEndpointByUnit(int endpoint, int textUnit, int count, out int moved);

        [PreserveSig]
        int MoveEndpointByRange(int sourceEndpoint, IUiAutomationTextRange range, int targetEndpoint);

        [PreserveSig]
        int Select();

        [PreserveSig]
        int AddToSelection();

        [PreserveSig]
        int RemoveFromSelection();

        [PreserveSig]
        int ScrollIntoView([MarshalAs(UnmanagedType.Bool)] bool alignToTop);

        [PreserveSig]
        int GetChildren(out IntPtr children);
    }

    private static bool TryGetCaretBounds(IntPtr caretWindow, MonitorRect caretRect, out MonitorRect bounds)
    {
        bounds = default;
        if (caretWindow == IntPtr.Zero
            || caretRect.Right <= caretRect.Left
            || caretRect.Bottom <= caretRect.Top)
        {
            return false;
        }

        var caretPoint = new NativePoint { X = caretRect.Left, Y = caretRect.Top };
        if (!ClientToScreen(caretWindow, ref caretPoint))
        {
            return false;
        }

        bounds = new MonitorRect
        {
            Left = caretPoint.X,
            Top = caretPoint.Y,
            Right = caretPoint.X + Math.Max(1, caretRect.Right - caretRect.Left),
            Bottom = caretPoint.Y + Math.Max(1, caretRect.Bottom - caretRect.Top)
        };
        return true;
    }

    private static bool TryGetCaretScreenPoint(uint targetThreadId, IntPtr inputWindow, out NativePoint caretPoint)
    {
        caretPoint = default;
        if (inputWindow == IntPtr.Zero)
        {
            return false;
        }

        var currentThreadId = GetCurrentThreadId();
        var attached = currentThreadId != targetThreadId
            && AttachThreadInput(currentThreadId, targetThreadId, true);
        try
        {
            return GetCaretPos(out caretPoint) && ClientToScreen(inputWindow, ref caretPoint);
        }
        finally
        {
            if (attached)
            {
                AttachThreadInput(currentThreadId, targetThreadId, false);
            }
        }
    }

    private static bool TryGetFocusedChildWindow(uint targetThreadId, IntPtr targetWindow, out IntPtr focusedWindow)
    {
        focusedWindow = IntPtr.Zero;
        var currentThreadId = GetCurrentThreadId();
        var attached = currentThreadId != targetThreadId
            && AttachThreadInput(currentThreadId, targetThreadId, true);
        try
        {
            focusedWindow = GetFocus();
            return focusedWindow != IntPtr.Zero && focusedWindow != targetWindow;
        }
        finally
        {
            if (attached)
            {
                AttachThreadInput(currentThreadId, targetThreadId, false);
            }
        }
    }

    private RectInt32 GetWorkArea(NativePoint? preferredPoint = null)
    {
        NativePoint anchorPoint;
        if (preferredPoint is NativePoint providedPoint)
        {
            anchorPoint = providedPoint;
        }
        else if (!GetCursorPos(out anchorPoint))
        {
            anchorPoint = default;
        }

        if (anchorPoint.X != 0 || anchorPoint.Y != 0)
        {
            var monitor = MonitorFromPoint(anchorPoint, MonitorDefaultTnearest);
            if (monitor != IntPtr.Zero)
            {
                var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
                if (GetMonitorInfo(monitor, ref info))
                {
                    return new RectInt32(info.Work.Left, info.Work.Top, info.Work.Right - info.Work.Left, info.Work.Bottom - info.Work.Top);
                }
            }
        }

        var workArea = new MonitorRect();
        SystemParametersInfo(SpiGetWorkArea, 0, ref workArea, 0);
        return new RectInt32(workArea.Left, workArea.Top, workArea.Right - workArea.Left, workArea.Bottom - workArea.Top);
    }

    private void Monitor_Captured(ClipboardCapture capture)
    {
        EnqueueOnUi(() =>
        {
            // 剪贴板回调可能来自后台线程，所有历史和界面更新统一切回 UI 队列。
            if (!_storageAvailable
                || !IsCaptureEnabled(capture.Kind)
                || IsApplicationExcluded(capture.SourceApplication))
            {
                return;
            }

            try
            {
                var item = _history.AddOrUpdate(capture);
                CleanupHistory();
                StatusText.Text = AppLocalization.Format("SavedItemCount", _history.Count);
                RefreshHistory();
                if (capture.Kind == ClipboardContentKind.Image
                    && _settings.EnableImageOcr)
                {
                    _ocrService.Enqueue(item.Id);
                }
            }
            catch (Exception)
            {
                // 持久化不可靠时停止接收新内容，避免界面状态与磁盘继续分叉。
                _storageAvailable = false;
                _monitor.Dispose();
                StatusText.Text = AppLocalization.GetString("StorageFailureStoppedMonitoring");
            }
        });
    }

    private void Monitor_CaptureFailed(Exception exception)
    {
        EnqueueOnUi(() => StatusText.Text = AppLocalization.Format("ClipboardReadFailed", exception.Message));
    }

    private void OcrService_Recognized(Guid id, string text)
    {
        EnqueueOnUi(() =>
        {
            try
            {
                if (!_storageAvailable)
                {
                    return;
                }

                if (_history.SetOcrText(id, text) is not null)
                {
                    RefreshHistory();
                }
            }
            catch (Exception exception)
            {
                // OCR 是增强功能，写入失败时保留原图片记录并继续监听。
                System.Diagnostics.Debug.WriteLine($"OCR 结果保存失败：{exception}");
            }
        });
    }

    private static void OcrService_RecognitionFailed(Exception exception)
    {
        // 单张图片识别失败不应影响剪切板监听；调试输出用于排查缺失语言包或损坏图片。
        System.Diagnostics.Debug.WriteLine($"OCR 识别失败：{exception}");
    }

    private bool IsCaptureEnabled(ClipboardContentKind kind)
    {
        return kind switch
        {
            ClipboardContentKind.Text => _settings.MonitorText,
            ClipboardContentKind.Image => _settings.MonitorImages,
            ClipboardContentKind.Files => _settings.MonitorFiles,
            _ => false
        };
    }

    private bool IsApplicationExcluded(string? application)
    {
        return !string.IsNullOrWhiteSpace(application) && _excludedApplications.Contains(application);
    }

    private void UpdateExcludedApplications()
    {
        _excludedApplications.Clear();
        foreach (var value in _settings.ExcludedApplications.Split(
                     [';', ',', '\r', '\n'],
                     StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var processName = value.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
                ? value[..^4]
                : value;
            if (!string.IsNullOrWhiteSpace(processName))
            {
                _excludedApplications.Add(processName);
            }
        }
    }

    private void CleanupHistory()
    {
        var cutoff = DateTimeOffset.UtcNow - TimeSpan.FromDays(_settings.RetentionDays);
        if (_history.Cleanup(cutoff, _settings.MaxHistoryEntries) > 0)
        {
            _repository.Compact();
        }
    }

    private void RefreshHistory(bool recreateDisplayItems = false, bool debounce = false)
    {
        _historyQueryCancellation?.Cancel();
        _historyQueryCancellation?.Dispose();
        _historyQueryCancellation = new CancellationTokenSource();
        var version = ++_historyLoadVersion;
        var query = new ClipboardHistoryQuery(SearchBox?.Text, _selectedKind);
        _historyLoadTask = ReloadHistoryAsync(
            query,
            version,
            recreateDisplayItems,
            debounce,
            _historyQueryCancellation.Token);
    }

    private async Task ReloadHistoryAsync(
        ClipboardHistoryQuery query,
        int version,
        bool recreateDisplayItems,
        bool debounce,
        CancellationToken cancellationToken)
    {
        _isLoadingHistory = true;
        try
        {
            if (debounce)
            {
                await Task.Delay(150, cancellationToken);
            }

            var page = await Task.Run(
                () => _history.Search(query, null, HistoryPageSize),
                cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
            if (version != _historyLoadVersion)
            {
                return;
            }

            CloseContentPreview();
            _hoveredHistoryItem = null;
            var selectedId = (HistoryList.SelectedItem as HistoryListItem)?.Item.Id;
            DisposeDisplayItems();
            _currentHistoryQuery = query;
            _nextHistoryCursor = page.NextCursor;
            _historyTotalCount = page.TotalCount;
            _historyUnpinnedCount = page.UnpinnedCount;
            AppendHistoryItems(page.Items);
            UpdateHistoryListState();

            if (selectedId is Guid id)
            {
                HistoryList.SelectedItem = _displayItems.FirstOrDefault(item => item.Item.Id == id);
            }

            if (_displayItems.Count > 0)
            {
                HistoryList.ScrollIntoView(_displayItems[0], ScrollIntoViewAlignment.Leading);
            }

            if (recreateDisplayItems)
            {
                _dispatcherQueue?.TryEnqueue(RefreshHistoryCardLocalization);
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception exception)
        {
            System.Diagnostics.Debug.WriteLine($"加载历史记录失败：{exception}");
            StatusText.Text = AppLocalization.GetString("StorageInitializationFailed");
        }
        finally
        {
            if (version == _historyLoadVersion)
            {
                _isLoadingHistory = false;
            }
        }
    }

    private async Task LoadNextHistoryPageAsync()
    {
        if (_isLoadingHistory || _nextHistoryCursor is null || _historyQueryCancellation is null)
        {
            return;
        }

        _isLoadingHistory = true;
        var version = _historyLoadVersion;
        var cursor = _nextHistoryCursor;
        var cancellationToken = _historyQueryCancellation.Token;
        try
        {
            var page = await Task.Run(
                () => _history.Search(_currentHistoryQuery, cursor, HistoryPageSize),
                cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
            if (version != _historyLoadVersion)
            {
                return;
            }

            _nextHistoryCursor = page.NextCursor;
            _historyTotalCount = page.TotalCount;
            _historyUnpinnedCount = page.UnpinnedCount;
            AppendHistoryItems(page.Items);
            UpdateHistoryListState();
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception exception)
        {
            System.Diagnostics.Debug.WriteLine($"加载下一页历史记录失败：{exception}");
        }
        finally
        {
            if (version == _historyLoadVersion)
            {
                _isLoadingHistory = false;
            }
        }
    }

    private void AppendHistoryItems(IEnumerable<ClipboardHistoryEntry> entries)
    {
        foreach (var entry in entries)
        {
            _displayItems.Add(HistoryListItem.From(entry));
        }
    }

    private void UpdateHistoryListState()
    {
        // 数字键只映射当前第一页中的未置顶记录，搜索期间不显示快捷编号。
        Array.Clear(_quickPasteItems);
        var quickPasteIndex = 0;
        var showQuickPasteLabels = string.IsNullOrEmpty(_currentHistoryQuery.SearchText);
        foreach (var displayItem in _displayItems)
        {
            if (showQuickPasteLabels
                && !displayItem.Item.IsPinned
                && quickPasteIndex < _quickPasteItems.Length)
            {
                _quickPasteItems[quickPasteIndex] = displayItem;
                displayItem.SetQuickPasteIndex(quickPasteIndex);
                quickPasteIndex++;
            }
            else
            {
                displayItem.SetQuickPasteIndex(null);
            }
        }

        EmptyText.Visibility = _historyTotalCount == 0 ? Visibility.Visible : Visibility.Collapsed;
        ClearHistoryButton.IsEnabled = _historyUnpinnedCount > 0;
        CountText.Text = AppLocalization.Format("ItemCount", _historyTotalCount);
    }

    private void RefreshHistoryCardLocalization()
    {
        foreach (var displayItem in _displayItems)
        {
            if (HistoryList.ContainerFromItem(displayItem) is not ListViewItem container
                || container.ContentTemplateRoot is not FrameworkElement card)
            {
                continue;
            }

            ApplyCardLocalization(card);
        }
    }

    private static void ApplyCardLocalization(FrameworkElement card)
    {
        SetCardPreviewButtonState(card, isExpanded: false);
        if (card.FindName("RecordMoreButton") is Button moreButton)
        {
            var label = AppLocalization.GetString("RecordMoreButtonTooltip");
            ToolTipService.SetToolTip(moreButton, label);
            AutomationProperties.SetName(moreButton, label);
        }

        if (card.FindName("PastePlainTextMenuItem") is MenuFlyoutItem plainTextItem)
        {
            plainTextItem.Text = AppLocalization.GetString("PastePlainTextMenuItemText");
        }

        if (card.FindName("PasteOcrTextMenuItem") is MenuFlyoutItem ocrItem)
        {
            ocrItem.Text = AppLocalization.GetString("PasteOcrTextMenuItemText");
        }

        if (card.FindName("PasteAsFileMenuItem") is MenuFlyoutItem fileItem)
        {
            fileItem.Text = AppLocalization.GetString("PasteAsFileMenuItemText");
        }

        if (card.FindName("DeleteRecordMenuItem") is MenuFlyoutItem deleteItem)
        {
            deleteItem.Text = AppLocalization.GetString("DeleteRecordMenuItemText");
        }
    }

    internal void RefreshLocalization()
    {
        AutomationProperties.SetName(
            AppIconImage,
            AppLocalization.GetString("AppIconAutomationName"));
        AutomationProperties.SetName(
            SettingsButton,
            AppLocalization.GetString("MainSettingsButtonAutomationName"));
        ToolTipService.SetToolTip(
            SettingsButton,
            AppLocalization.GetString("MainSettingsButtonTooltip"));
        AutomationProperties.SetName(
            WindowPinToggle,
            AppLocalization.GetString("MainWindowPinToggleAutomationName"));
        ToolTipService.SetToolTip(
            WindowPinToggle,
            AppLocalization.GetString("MainWindowPinToggleTooltip"));
        AutomationProperties.SetName(
            GitHubButton,
            AppLocalization.GetString("MainGitHubButtonAutomationName"));
        ToolTipService.SetToolTip(
            GitHubButton,
            AppLocalization.GetString("MainGitHubButtonTooltip"));
        AutomationProperties.SetName(
            SearchBox,
            AppLocalization.GetString("MainSearchBoxAutomationName"));
        SearchBox.PlaceholderText = AppLocalization.GetString("MainSearchBoxPlaceholder");
        SetFilterLocalization(AllFilterButton, "FilterAllButton");
        SetFilterLocalization(TextFilterButton, "FilterTextButton");
        SetFilterLocalization(ImageFilterButton, "FilterImageButton");
        SetFilterLocalization(FilesFilterButton, "FilterFilesButton");
        AutomationProperties.SetName(
            ClearHistoryButton,
            AppLocalization.GetString("ClearHistoryButtonAutomationName"));
        ToolTipService.SetToolTip(
            ClearHistoryButton,
            AppLocalization.GetString("ClearHistoryButtonTooltip"));
        AutomationProperties.SetName(
            HistoryList,
            AppLocalization.GetString("HistoryListAutomationName"));
        EmptyText.Text = AppLocalization.GetString("MainEmptyHistoryText");
        CountText.Text = AppLocalization.Format("ItemCount", _historyTotalCount);
        StatusText.Text = !_storageAvailable
            ? AppLocalization.GetString("StorageInitializationFailed")
            : _isListeningPaused
                ? AppLocalization.GetString("MonitoringPaused")
                : AppLocalization.Format("ListeningStatus", _history.Count);
        RefreshHistory(recreateDisplayItems: true);
        _trayIcon?.RefreshLocalization();
    }

    private static void SetFilterLocalization(ToggleButton button, string resourcePrefix)
    {
        AutomationProperties.SetName(
            button,
            AppLocalization.GetString($"{resourcePrefix}AutomationName"));
        ToolTipService.SetToolTip(
            button,
            AppLocalization.GetString($"{resourcePrefix}Tooltip"));
    }

    private void DisposeDisplayItems()
    {
        foreach (var displayItem in _displayItems)
        {
            displayItem.Dispose();
        }

        _displayItems.Clear();
    }

    private async Task PlayAsync(HistoryListItem selected, bool plainTextOnly = false)
    {
        // 先保存目标输入窗口，再隐藏面板，避免面板自身成为粘贴目标。
        var pasteTarget = GetPasteTargetSnapshot();
        _monitor.SuspendCapture();
        try
        {
            HidePanel();
            await Task.Delay(50);
            var content = await Task.Run(() => _repository.LoadContent(selected.Item.Id));
            var pasted = await ClipboardPlayback.PlayAsync(
                selected.Item,
                content,
                pasteTarget.TargetWindow,
                () => TryRestoreAutomationFocus(pasteTarget),
                plainTextOnly);
            if (pasted)
            {
                HidePanel();
            }
            else
            {
                StatusText.Text = AppLocalization.GetString("ContentRestoredManualPaste");
            }
        }
        catch (Exception)
        {
            StatusText.Text = AppLocalization.GetString("ContentRestoreFailed");
        }
        finally
        {
            await Task.Delay(150);
            _monitor.ResumeCapture();
        }
    }

    private async void RecordPasteAsFileButton_Click(object sender, RoutedEventArgs e)
    {
        if (GetHistoryListItem(sender) is not HistoryListItem selected)
        {
            return;
        }

        await PasteAsFileAsync(selected);
    }

    private async Task PasteAsFileAsync(HistoryListItem selected)
    {
        var targetWindow = GetPasteTargetWindow();
        _monitor.SuspendCapture();
        try
        {
            HidePanel();
            await Task.Delay(50);
            var content = await Task.Run(() => _repository.LoadContent(selected.Item.Id));
            var saved = await ClipboardPlayback.SaveAsFileAsync(selected.Item, content, targetWindow);
            StatusText.Text = saved
                ? AppLocalization.GetString("SavedAsFile")
                : AppLocalization.GetString("OpenExplorerFolderFirst");
        }
        catch (Exception)
        {
            StatusText.Text = AppLocalization.GetString("SaveAsFileFailed");
        }
        finally
        {
            await Task.Delay(150);
            _monitor.ResumeCapture();
        }
    }

    private IntPtr GetPasteTargetWindow()
    {
        var targetWindow = _pasteTarget != IntPtr.Zero && _pasteTarget != _handle
            ? _pasteTarget
            : _panelTargetWindow;
        return targetWindow == _handle ? IntPtr.Zero : targetWindow;
    }

    private PasteTargetSnapshot GetPasteTargetSnapshot()
    {
        return new PasteTargetSnapshot(
            GetPasteTargetWindow(),
            _pasteInputBounds,
            _hasPasteInputBounds);
    }

    private static bool TryRestoreAutomationFocus(PasteTargetSnapshot pasteTarget)
    {
        if (!pasteTarget.HasInputBounds)
        {
            return false;
        }

        if (pasteTarget.TargetWindow == IntPtr.Zero)
        {
            return false;
        }

        IUiAutomation? automation = null;
        IUiAutomationElement? inputElement = null;
        try
        {
            GetWindowThreadProcessId(pasteTarget.TargetWindow, out var targetProcessId);
            if (targetProcessId == 0)
            {
                return false;
            }

            var automationType = Type.GetTypeFromCLSID(CuiAutomationClsid, throwOnError: false);
            if (automationType is null)
            {
                return false;
            }

            automation = Activator.CreateInstance(automationType) as IUiAutomation;
            if (automation is null)
            {
                return false;
            }

            var inputBounds = pasteTarget.InputBounds;
            var inputPoint = new NativePoint
            {
                X = inputBounds.Left + Math.Max(1, inputBounds.Right - inputBounds.Left) / 2,
                Y = inputBounds.Top + Math.Max(1, inputBounds.Bottom - inputBounds.Top) / 2
            };
            if (automation.ElementFromPoint(inputPoint, out inputElement) < 0
                || inputElement is null
                || inputElement.GetCurrentPropertyValue(UiaProcessIdPropertyId, out var processIdValue) < 0
                || Convert.ToUInt32(processIdValue) != targetProcessId)
            {
                return false;
            }

            return inputElement.SetFocus() >= 0;
        }
        catch (Exception exception) when (exception is COMException
            or InvalidOperationException
            or InvalidCastException
            or ArgumentException
            or FormatException
            or OverflowException)
        {
            return false;
        }
        finally
        {
            ReleaseComObject(inputElement);
            ReleaseComObject(automation);
        }
    }

    private void SearchBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        RefreshHistory(debounce: true);
    }

    private async void HistoryList_ContainerContentChanging(
        ListViewBase sender,
        ContainerContentChangingEventArgs args)
    {
        if (args.InRecycleQueue
            || _isLoadingHistory
            || args.ItemIndex < Math.Max(0, _displayItems.Count - HistoryLoadThreshold))
        {
            return;
        }

        var loadTask = LoadNextHistoryPageAsync();
        _historyLoadTask = loadTask;
        await loadTask;
    }

    private void SearchBox_PointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (_panelShownWithoutActivation)
        {
            ShowPanel(activatePanel: true);
            SearchBox.Focus(FocusState.Pointer);
        }
    }

    private async void ClearHistoryButton_Click(object sender, RoutedEventArgs e)
    {
        if (_historyUnpinnedCount == 0)
        {
            return;
        }

        var dialog = new ContentDialog
        {
            Title = AppLocalization.GetString("ClearCurrentListTitle"),
            Content = AppLocalization.Format("ClearCurrentListMessage", _historyUnpinnedCount),
            PrimaryButtonText = AppLocalization.GetString("Clear"),
            CloseButtonText = AppLocalization.GetString("Cancel"),
            DefaultButton = ContentDialogButton.Close,
            XamlRoot = RootGrid.XamlRoot
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        _monitor.SuspendCapture();
        try
        {
            // 确认后重新读取筛选条件，清理数据库中的全部匹配项，不受当前已加载页限制。
            var query = new ClipboardHistoryQuery(SearchBox?.Text, _selectedKind);
            var deletedCount = await Task.Run(() => _history.DeleteMatching(query));
            await Task.Run(() => _repository.Compact(full: true));
            RefreshHistory();
            StatusText.Text = AppLocalization.Format("CurrentListCleared", deletedCount);
        }
        catch (Exception)
        {
            StatusText.Text = AppLocalization.GetString("ClearHistoryFailed");
        }
        finally
        {
            _monitor.ResumeCapture();
        }
    }

    private void TypeFilter_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not ToggleButton selected)
        {
            return;
        }

        foreach (var filter in new[] { AllFilterButton, TextFilterButton, ImageFilterButton, FilesFilterButton })
        {
            if (!ReferenceEquals(filter, selected))
            {
                filter.IsChecked = false;
            }
        }

        _selectedKind = selected.Tag?.ToString() switch
        {
            "Text" => ClipboardContentKind.Text,
            "Image" => ClipboardContentKind.Image,
            "Files" => ClipboardContentKind.Files,
            _ => null
        };
        RefreshHistory();
    }

    private async void Input_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Escape)
        {
            HidePanel();
            e.Handled = true;
        }
        else if (ReferenceEquals(sender, HistoryList)
                 && string.IsNullOrEmpty(SearchBox.Text)
                 && !PanelShortcut.HasAnyModifierDown()
                 && TryGetQuickPasteIndex(e.Key, out var quickPasteIndex)
                 && _quickPasteItems[quickPasteIndex] is { } quickPasteItem)
        {
            HistoryList.SelectedItem = quickPasteItem;
            e.Handled = true;
            await PlayAsync(quickPasteItem);
        }
        else if (e.Key == VirtualKey.Down && ReferenceEquals(sender, SearchBox) && _displayItems.Count > 0)
        {
            HistoryList.Focus(FocusState.Programmatic);
            HistoryList.SelectedIndex = 0;
            e.Handled = true;
        }
        else if (ReferenceEquals(sender, HistoryList)
                 && HistoryList.SelectedItem is HistoryListItem selected)
        {
            if (PanelShortcut.Matches(e, _settings.PlainTextPasteShortcut))
            {
                e.Handled = true;
                await WaitForModifierKeysReleasedAsync();
                await PlayAsync(selected, plainTextOnly: true);
            }
            else if (PanelShortcut.Matches(e, _settings.PasteShortcut))
            {
                e.Handled = true;
                await PlayAsync(selected);
            }
            else if (PanelShortcut.Matches(e, _settings.PreviewShortcut))
            {
                e.Handled = true;
                await ToggleContentPreviewAsync(selected);
            }
            else if (PanelShortcut.Matches(e, _settings.PinShortcut))
            {
                e.Handled = true;
                TogglePinned(selected);
            }
            else if (PanelShortcut.Matches(e, _settings.DeleteShortcut))
            {
                e.Handled = true;
                DeleteRecord(selected);
            }
            else if (selected.Item.Kind is ClipboardContentKind.Text or ClipboardContentKind.Image
                     && PanelShortcut.Matches(e, _settings.PasteAsFileShortcut))
            {
                e.Handled = true;
                await PasteAsFileAsync(selected);
            }
        }
    }

    // 等待快捷键修饰键松开，避免模拟 Ctrl+V 时叠加成全局热键。
    private static async Task WaitForModifierKeysReleasedAsync()
    {
        while (PanelShortcut.HasAnyModifierDown())
        {
            await Task.Delay(10);
        }
    }

    private async void HistoryList_ItemClick(object sender, ItemClickEventArgs e)
    {
        if (e.ClickedItem is HistoryListItem selected)
        {
            await PlayAsync(selected);
        }
    }

    // 选中项就是键盘导航的当前项，用卡片底色表达焦点，不显示系统焦点边框。
    private void HistoryList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        foreach (var item in e.RemovedItems)
        {
            SetHistoryCardHighlight(item, ReferenceEquals(item, _hoveredHistoryItem));
        }

        foreach (var item in e.AddedItems)
        {
            SetHistoryCardHighlight(item, true);
        }
    }

    private async void HistoryCard_Loaded(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement card)
        {
            ApplyCardLocalization(card);
            if (card.DataContext is HistoryListItem item)
            {
                card.Tag = item;
                if (item.Item.Kind == ClipboardContentKind.Image)
                {
                    await item.EnsureThumbnailLoadedAsync(_repository.LoadContent);
                }
            }
        }
    }

    private async void HistoryCard_DataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args)
    {
        var card = sender;
        if (card.Tag is HistoryListItem oldItem)
        {
            if (ReferenceEquals(_previewedHistoryItem, oldItem))
            {
                CloseContentPreview();
            }

            oldItem.UnloadThumbnail();
        }

        card.Tag = args.NewValue;
        ApplyCardLocalization(card);
        if (args.NewValue is HistoryListItem { Item.Kind: ClipboardContentKind.Image } item)
        {
            await item.EnsureThumbnailLoadedAsync(_repository.LoadContent);
        }
    }

    private async void RecordPreviewButton_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not HistoryListItem selected)
        {
            return;
        }

        HistoryList.SelectedItem = selected;
        await ToggleContentPreviewAsync(selected);
    }

    private async void RecordPlainTextPasteButton_Click(object sender, RoutedEventArgs e)
    {
        if (GetHistoryListItem(sender) is HistoryListItem selected)
        {
            await PlayAsync(selected, plainTextOnly: true);
        }
    }

    private async void RecordPasteOcrTextButton_Click(object sender, RoutedEventArgs e)
    {
        if (GetHistoryListItem(sender) is not HistoryListItem selected
            || selected.Item.OcrTextLength == 0)
        {
            return;
        }

        var pasteTarget = GetPasteTargetSnapshot();
        _monitor.SuspendCapture();
        try
        {
            var ocrText = await Task.Run(() => _history.LoadOcrText(selected.Item.Id));
            if (string.IsNullOrEmpty(ocrText))
            {
                return;
            }

            HidePanel();
            await Task.Delay(50);
            var textItem = selected.Item with { Kind = ClipboardContentKind.Text };
            var content = new ClipboardTextContent(ocrText, null, null).Serialize();
            var pasted = await ClipboardPlayback.PlayAsync(
                textItem,
                content,
                pasteTarget.TargetWindow,
                () => TryRestoreAutomationFocus(pasteTarget),
                plainTextOnly: true);
            if (!pasted)
            {
                StatusText.Text = AppLocalization.GetString("ContentRestoredManualPaste");
            }
        }
        catch (Exception)
        {
            StatusText.Text = AppLocalization.GetString("ContentRestoreFailed");
        }
        finally
        {
            await Task.Delay(150);
            _monitor.ResumeCapture();
        }
    }

    private async Task ToggleContentPreviewAsync(HistoryListItem selected)
    {
        // 同时只展开一条记录，切换时释放上一条记录的预览控件。
        if (!TryGetCardPreviewElements(selected, out var card, out var preview, out var host))
        {
            return;
        }

        if (ReferenceEquals(_previewedHistoryItem, selected))
        {
            CloseContentPreview();
            return;
        }

        CloseContentPreview();

        try
        {
            object contentControl;
            string? richText = null;
            switch (selected.Item.Kind)
            {
                case ClipboardContentKind.Text:
                {
                    var content = await Task.Run(() =>
                        ClipboardTextContent.Deserialize(_repository.LoadContent(selected.Item.Id)));
                    if (!ReferenceEquals(card.DataContext, selected))
                    {
                        return;
                    }

                    selected.UpdateTextFormatMetadata(content);
                    if (!string.IsNullOrEmpty(content.Rtf))
                    {
                        contentControl = CreateRichTextPreview();
                        richText = content.Rtf;
                    }
                    else
                    {
                        contentControl = CreateTextPreview(content.Text);
                    }

                    break;
                }
                case ClipboardContentKind.Image:
                {
                    var image = await LoadExpandedImageAsync(selected.Item.Id);
                    if (!ReferenceEquals(card.DataContext, selected))
                    {
                        return;
                    }

                    contentControl = new Image
                    {
                        Source = image,
                        Stretch = Stretch.Uniform,
                        HorizontalAlignment = HorizontalAlignment.Stretch,
                        VerticalAlignment = VerticalAlignment.Stretch
                    };
                    break;
                }
                case ClipboardContentKind.Files:
                {
                    var content = await Task.Run(() => _repository.LoadContent(selected.Item.Id));
                    var paths = JsonSerializer.Deserialize<string[]>(content) ?? [];
                    contentControl = CreateTextPreview(string.Join(Environment.NewLine, paths));
                    break;
                }
                default:
                    return;
            }

            if (!ReferenceEquals(card.DataContext, selected))
            {
                return;
            }

            host.Content = contentControl;
            preview.Visibility = Visibility.Visible;
            _previewedHistoryItem = selected;
            _previewedHistoryCard = card;
            SetCardPreviewButtonState(card, isExpanded: true);

            if (richText is not null && contentControl is RichEditBox richTextPreview)
            {
                card.UpdateLayout();
                await LoadRichTextPreviewAsync(richTextPreview, richText);
            }
        }
        catch (Exception)
        {
            CloseContentPreview();
            StatusText.Text = AppLocalization.GetString("PreviewUnavailable");
        }
    }

    private async Task<BitmapImage> LoadExpandedImageAsync(Guid id)
    {
        var content = await Task.Run(() => _repository.LoadContent(id));
        using var stream = new InMemoryRandomAccessStream();
        using (var output = stream.GetOutputStreamAt(0))
        using (var writer = new DataWriter(output))
        {
            writer.WriteBytes(content);
            await writer.StoreAsync();
            await writer.FlushAsync();
        }

        stream.Seek(0);
        var image = new BitmapImage
        {
            // 预览容器高度为 168，保留约 2 倍像素即可避免解码整张原图。
            DecodePixelHeight = 336
        };
        await image.SetSourceAsync(stream);
        return image;
    }

    private static RichEditBox CreateRichTextPreview()
    {
        var preview = new RichEditBox
        {
            Background = new SolidColorBrush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = new Thickness(0),
            IsReadOnly = true,
            IsSpellCheckEnabled = false,
            IsTabStop = false,
            TextWrapping = TextWrapping.Wrap
        };
        ScrollViewer.SetVerticalScrollMode(preview, ScrollMode.Auto);
        ScrollViewer.SetVerticalScrollBarVisibility(preview, ScrollBarVisibility.Auto);
        ScrollViewer.SetHorizontalScrollMode(preview, ScrollMode.Disabled);
        ScrollViewer.SetHorizontalScrollBarVisibility(preview, ScrollBarVisibility.Disabled);
        AutomationProperties.SetName(preview, AppLocalization.GetString("RichTextPreview"));
        return preview;
    }

    private static ScrollViewer CreateTextPreview(string text)
    {
        var previewText = new TextBlock
        {
            Text = text,
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true
        };
        AutomationProperties.SetName(previewText, AppLocalization.GetString("TextPreview"));
        return new ScrollViewer
        {
            VerticalScrollMode = ScrollMode.Auto,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollMode = ScrollMode.Disabled,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            Content = previewText
        };
    }

    private static async Task LoadRichTextPreviewAsync(RichEditBox preview, string richText)
    {
        using var stream = new InMemoryRandomAccessStream();
        using (var output = stream.GetOutputStreamAt(0))
        using (var writer = new DataWriter(output))
        {
            writer.WriteBytes(Encoding.UTF8.GetBytes(richText));
            await writer.StoreAsync();
            writer.DetachStream();
        }

        stream.Seek(0);
        preview.TextDocument.LoadFromStream(TextSetOptions.FormatRtf, stream);
    }

    private void CloseContentPreview()
    {
        if (_previewedHistoryCard is FrameworkElement card
            && TryGetCardPreviewElements(card, out var preview, out var host))
        {
            preview.Visibility = Visibility.Collapsed;
            host.Content = null;
            SetCardPreviewButtonState(card, isExpanded: false);
        }

        _previewedHistoryItem = null;
        _previewedHistoryCard = null;
    }

    private static void SetCardPreviewButtonState(FrameworkElement card, bool isExpanded)
    {
        if (card.FindName("RecordPreviewIcon") is FontIcon icon)
        {
            icon.Glyph = isExpanded ? "\uED1A" : "\uE890";
        }

        if (card.FindName("RecordPreviewButton") is Button button)
        {
            var label = isExpanded
                ? AppLocalization.GetString("CollapsePreview")
                : AppLocalization.GetString("PreviewContent");
            ToolTipService.SetToolTip(button, label);
            AutomationProperties.SetName(button, label);
        }
    }

    private static bool TryGetQuickPasteIndex(VirtualKey key, out int index)
    {
        var keyCode = (int)key;
        if (keyCode is >= 0x31 and <= 0x39)
        {
            index = keyCode - 0x31;
            return true;
        }

        if (keyCode is >= 0x61 and <= 0x69)
        {
            index = keyCode - 0x61;
            return true;
        }

        index = -1;
        return false;
    }

    private bool TryGetCardPreviewElements(
        HistoryListItem item,
        out FrameworkElement card,
        out Border preview,
        out ContentControl host)
    {
        card = null!;
        preview = null!;
        host = null!;

        if (HistoryList.ContainerFromItem(item) is not ListViewItem container
            || container.ContentTemplateRoot is not FrameworkElement cardRoot
            || !ReferenceEquals(cardRoot.DataContext, item)
            || !TryGetCardPreviewElements(cardRoot, out var previewContent, out var previewHost))
        {
            return false;
        }

        card = cardRoot;
        preview = previewContent;
        host = previewHost;
        return true;
    }

    private static bool TryGetCardPreviewElements(
        FrameworkElement card,
        out Border preview,
        out ContentControl host)
    {
        preview = null!;
        host = null!;

        if (card.FindName("CardPreviewContent") is not Border previewContent
            || card.FindName("CardPreviewHost") is not ContentControl previewHost)
        {
            return false;
        }

        preview = previewContent;
        host = previewHost;
        return true;
    }

    private void HistoryCard_PointerEntered(object sender, PointerRoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not HistoryListItem item)
        {
            return;
        }

        _hoveredHistoryItem = item;
        SetHistoryCardHighlight(item, true);
    }

    private void HistoryCard_PointerExited(object sender, PointerRoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not HistoryListItem item)
        {
            return;
        }

        if (ReferenceEquals(_hoveredHistoryItem, item))
        {
            _hoveredHistoryItem = null;
        }

        SetHistoryCardHighlight(item, ReferenceEquals(HistoryList.SelectedItem, item));
    }

    private void SetHistoryCardHighlight(object item, bool highlighted)
    {
        if (HistoryList.ContainerFromItem(item) is not ListViewItem container
            || container.ContentTemplateRoot is not Border card)
        {
            return;
        }

        if (card.FindName("HistoryFocusBackground") is Border focusBackground)
        {
            focusBackground.Opacity = highlighted ? 1 : 0;
        }

        // 选中状态优先于置顶底色，取消选中后恢复置顶层的显示。
        if (card.FindName("PinnedCardBackground") is Border pinnedBackground)
        {
            pinnedBackground.Opacity = highlighted ? 0 : 1;
        }
    }

    private void RecordPinButton_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not HistoryListItem selected)
        {
            return;
        }

        TogglePinned(selected);
    }

    private void TogglePinned(HistoryListItem selected)
    {
        var updated = _history.SetPinned(selected.Item.Id, !selected.Item.IsPinned);
        if (updated is not null)
        {
            StatusText.Text = updated.IsPinned
                ? AppLocalization.GetString("ItemPinned")
                : AppLocalization.GetString("ItemUnpinned");
            RefreshHistory();
        }
    }

    private void RecordDeleteButton_Click(object sender, RoutedEventArgs e)
    {
        if (GetHistoryListItem(sender) is not HistoryListItem selected)
        {
            return;
        }

        DeleteRecord(selected);
    }

    private void DeleteRecord(HistoryListItem selected)
    {
        if (_history.Remove(selected.Item.Id))
        {
            StatusText.Text = AppLocalization.GetString("ItemDeleted");
            RefreshHistory();
        }
    }

    private static HistoryListItem? GetHistoryListItem(object sender)
    {
        return sender is FrameworkElement element
            ? element.DataContext as HistoryListItem ?? element.Tag as HistoryListItem
            : null;
    }

    private void WindowPinToggle_Changed(object sender, RoutedEventArgs e)
    {
        _isTopmost = WindowPinToggle.IsChecked == true;
        SetWindowPos(_handle, _isTopmost ? HwndTopmost : HwndNotopmost, 0, 0, 0, 0, SwpNoMove | SwpNoSize | SwpNoActivate);
    }

    private void HeaderGrid_PointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (_handle == IntPtr.Zero)
        {
            return;
        }

        var pointerPoint = e.GetCurrentPoint(HeaderGrid);
        if (!pointerPoint.Properties.IsLeftButtonPressed)
        {
            return;
        }

        if (e.OriginalSource is not DependencyObject source)
        {
            return;
        }

        if (IsInteractiveHeaderElement(source))
        {
            CancelHeaderDrag();
            return;
        }

        if (_appWindow is null || !GetCursorPos(out var cursorPosition))
        {
            return;
        }

        var windowPosition = _appWindow.Position;
        _headerDragPointerId = pointerPoint.PointerId;
        _headerDragStartScreenX = cursorPosition.X;
        _headerDragStartScreenY = cursorPosition.Y;
        _headerDragStartWindowX = windowPosition.X;
        _headerDragStartWindowY = windowPosition.Y;
        _headerDragActive = false;
        _headerDragPending = HeaderGrid.CapturePointer(e.Pointer);
        if (!_headerDragPending)
        {
            _headerDragPointerId = null;
        }

        e.Handled = _headerDragPending;
    }

    private void HeaderGrid_PointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if ((!_headerDragPending && !_headerDragActive)
            || _headerDragPointerId != e.Pointer.PointerId)
        {
            return;
        }

        var pointerPoint = e.GetCurrentPoint(HeaderGrid);
        if (!pointerPoint.Properties.IsLeftButtonPressed)
        {
            CancelHeaderDrag();
            return;
        }

        if (!GetCursorPos(out var cursorPosition))
        {
            CancelHeaderDrag();
            return;
        }

        var deltaX = cursorPosition.X - _headerDragStartScreenX;
        var deltaY = cursorPosition.Y - _headerDragStartScreenY;
        if (_headerDragPending)
        {
            var horizontalThreshold = Math.Max(1, GetSystemMetrics(SmCxdrag));
            var verticalThreshold = Math.Max(1, GetSystemMetrics(SmCydrag));
            if (Math.Abs(deltaX) < horizontalThreshold
                && Math.Abs(deltaY) < verticalThreshold)
            {
                e.Handled = true;
                return;
            }

            _headerDragPending = false;
            _headerDragActive = true;
            if (_panelShownWithoutActivation)
            {
                ShowPanel(activatePanel: true);
            }
        }

        _appWindow?.Move(new PointInt32(
            _headerDragStartWindowX + deltaX,
            _headerDragStartWindowY + deltaY));
        e.Handled = true;
    }

    private void HeaderGrid_PointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if ((!_headerDragPending && !_headerDragActive)
            || _headerDragPointerId != e.Pointer.PointerId)
        {
            return;
        }

        CancelHeaderDrag(e.Pointer);
        e.Handled = true;
    }

    private void HeaderGrid_PointerCanceled(object sender, PointerRoutedEventArgs e)
    {
        if (_headerDragPointerId == e.Pointer.PointerId)
        {
            CancelHeaderDrag(e.Pointer);
        }
    }

    private void HeaderGrid_PointerCaptureLost(object sender, PointerRoutedEventArgs e)
    {
        if (_headerDragPointerId == e.Pointer.PointerId)
        {
            CancelHeaderDrag();
        }
    }

    private void CancelHeaderDrag(Pointer? pointer = null)
    {
        _headerDragPending = false;
        _headerDragActive = false;
        _headerDragPointerId = null;
        if (pointer is null)
        {
            HeaderGrid.ReleasePointerCaptures();
            return;
        }

        HeaderGrid.ReleasePointerCapture(pointer);
    }

    private static bool IsInteractiveHeaderElement(DependencyObject source)
    {
        var current = source;
        while (current is not null)
        {
            if (current is ButtonBase)
            {
                return true;
            }

            current = VisualTreeHelper.GetParent(current);
        }

        return false;
    }

    private void SettingsButton_Click(object sender, RoutedEventArgs e)
    {
        OpenSettings();
    }

    private async void GitHubButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            await Launcher.LaunchUriAsync(new Uri("https://github.com/ShrlAlgo/PasteOrbit"));
        }
        catch (Exception exception)
        {
            System.Diagnostics.Debug.WriteLine($"打开 GitHub 仓库失败：{exception}");
        }
    }

    private void OpenSettings()
    {
        if (_settingsWindow is not null)
        {
            ActivateSettingsWindow();
            return;
        }

        _settingsWindowOpen = true;
        var settingsWindow = new SettingsWindow(
            _settings,
            _settingsStore,
            ExportBackupAsync,
            RestoreBackupAsync,
            CheckForUpdatesAsync,
            StartUpdateAsync);
        _settingsWindow = settingsWindow;
        settingsWindow.SettingsChanged += SettingsWindow_SettingsChanged;
        settingsWindow.Closed += (_, _) =>
        {
            _settingsWindow = null;
            _settingsWindowOpen = false;
            // 保持主窗口关闭前的可见状态。托盘打开设置时主面板应继续隐藏。
        };
        settingsWindow.InitializeNative();
        ActivateSettingsWindow();
    }

    // 重复点击设置按钮时，将已有设置窗口提升到主面板之上。
    private void ActivateSettingsWindow()
    {
        if (_settingsWindow is not { } settingsWindow)
        {
            return;
        }

        settingsWindow.Activate();
        var settingsHandle = WindowNative.GetWindowHandle(settingsWindow);
        SetWindowPos(
            settingsHandle,
            _isTopmost ? HwndTopmost : HwndNotopmost,
            0,
            0,
            0,
            0,
            SwpNoMove | SwpNoSize);
        SetForegroundWindow(settingsHandle);
    }

    private async Task ExportBackupAsync(string destinationPath)
    {
        _monitor.SuspendCapture();
        try
        {
            await _backupService.ExportAsync(destinationPath);
        }
        finally
        {
            _monitor.ResumeCapture();
        }
    }

    private async Task RestoreBackupAsync(string sourcePath)
    {
        _monitor.SuspendCapture();
        try
        {
            _historyQueryCancellation?.Cancel();
            await _historyLoadTask;
            await _backupService.RestoreAsync(sourcePath);
            _history.Initialize();
            CleanupHistory();
            SettingsWindow_SettingsChanged(_settingsStore.Load());
        }
        finally
        {
            _monitor.ResumeCapture();
        }
    }

    private async Task<UpdateCheckResult?> CheckForUpdatesAsync()
    {
        return await _updateCheckService.CheckAsync();
    }

    private async Task CheckForUpdatesOnStartupAsync()
    {
        try
        {
            // 首次激活后后台检查一次，网络异常不影响剪切板监听。
            var result = await CheckForUpdatesAsync();
            if (result?.IsUpdateAvailable == true
                && !string.Equals(
                    _settings.SkippedUpdateVersion,
                    result.ReleaseTag,
                    StringComparison.OrdinalIgnoreCase))
            {
                await ShowUpdateDialogAsync(result);
            }
        }
        catch (Exception exception)
        {
            // 自动检查失败不影响主窗口和剪切板监听。
            System.Diagnostics.Debug.WriteLine($"自动检查更新失败：{exception}");
        }
    }

    private async Task ShowUpdateDialogAsync(UpdateCheckResult result)
    {
        if (RootGrid.XamlRoot is not { } xamlRoot)
        {
            return;
        }

        var content = new StackPanel
        {
            Spacing = 8
        };
        content.Children.Add(new TextBlock
        {
            Text = AppLocalization.Format(
                "UpdateAvailableMessage",
                result.LatestVersion,
                result.CurrentVersion),
            TextWrapping = TextWrapping.Wrap
        });
        if (!string.IsNullOrWhiteSpace(result.ReleaseNotes))
        {
            content.Children.Add(new TextBlock
            {
                Text = AppLocalization.GetString("UpdateReleaseNotesLabel"),
                Margin = new Thickness(0, 8, 0, 0)
            });
            content.Children.Add(new ScrollViewer
            {
                Content = new TextBlock
                {
                    Text = result.ReleaseNotes,
                    TextWrapping = TextWrapping.Wrap
                },
                MaxHeight = 260,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto
            });
        }

        // 有安装包时优先提供自动更新；没有安装包时允许忽略当前版本提示。
        var dialog = new ContentDialog
        {
            Title = AppLocalization.GetString("UpdateAvailableTitle"),
            Content = content,
            PrimaryButtonText = result.CanAutoUpdate
                ? AppLocalization.GetString("DownloadUpdateButton")
                : AppLocalization.GetString("DoNotRemindUpdateButton"),
            SecondaryButtonText = result.CanAutoUpdate
                ? AppLocalization.GetString("DoNotRemindUpdateButton")
                : string.Empty,
            CloseButtonText = AppLocalization.GetString("Later"),
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = xamlRoot
        };
        var dialogResult = await dialog.ShowAsync();
        if (dialogResult == ContentDialogResult.Primary && result.CanAutoUpdate)
        {
            await StartUpdateAsync(result, xamlRoot);
            return;
        }

        if ((dialogResult == ContentDialogResult.Primary && !result.CanAutoUpdate)
            || dialogResult == ContentDialogResult.Secondary)
        {
            IgnoreUpdateVersion(result);
        }
    }

    private void IgnoreUpdateVersion(UpdateCheckResult result)
    {
        _settings.SkippedUpdateVersion = result.ReleaseTag;
        try
        {
            _settingsStore.Save(_settings);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            System.Diagnostics.Debug.WriteLine($"保存忽略更新版本失败：{exception}");
        }
    }

    private async Task StartUpdateAsync(UpdateCheckResult result, XamlRoot xamlRoot)
    {
        if (!result.CanAutoUpdate)
        {
            IgnoreUpdateVersion(result);
            return;
        }

        var progressBar = new ProgressBar
        {
            IsIndeterminate = true,
            Minimum = 0,
            Maximum = 100,
            Width = 280
        };
        var statusText = new TextBlock
        {
            Text = AppLocalization.GetString("UpdateDownloadingMessage"),
            TextWrapping = TextWrapping.Wrap
        };
        var progressContent = new StackPanel
        {
            Spacing = 12
        };
        progressContent.Children.Add(progressBar);
        progressContent.Children.Add(statusText);

        var progressDialog = new ContentDialog
        {
            Title = AppLocalization.GetString("UpdateDownloadingTitle"),
            Content = progressContent,
            XamlRoot = xamlRoot
        };

        Task<ContentDialogResult>? progressDialogTask = null;
        try
        {
            // 下载期间显示进度，完成后交给独立更新器处理退出和重启。
            progressDialogTask = progressDialog.ShowAsync().AsTask();
            var progress = new Progress<double>(value =>
            {
                progressBar.IsIndeterminate = value < 0;
                if (value >= 0)
                {
                    progressBar.Value = value * 100;
                }
            });
            var installerPath = await _updateCheckService.DownloadInstallerAsync(result, progress);

            progressDialog.Hide();
            await progressDialogTask;
            var applicationPath = Environment.ProcessPath;
            if (string.IsNullOrWhiteSpace(applicationPath)
                || !UpdateInstaller.TryStart(installerPath, Environment.ProcessId, applicationPath))
            {
                throw new InvalidOperationException("更新程序启动失败。");
            }

            // 更新器已接管安装流程，主程序可以安全退出。
            ExitApplication();
        }
        catch (Exception exception)
        {
            // 下载、校验或启动更新器失败时保留当前程序。
            System.Diagnostics.Debug.WriteLine($"启动自动更新失败：{exception}");
            try
            {
                progressDialog.Hide();
                if (progressDialogTask is not null)
                {
                    await progressDialogTask;
                }
            }
            catch (Exception dialogException)
            {
                System.Diagnostics.Debug.WriteLine($"关闭更新进度窗口失败：{dialogException}");
            }

            if (!_isExiting)
            {
                var errorDialog = new ContentDialog
                {
                    Title = AppLocalization.GetString("UpdateDownloadFailedTitle"),
                    Content = AppLocalization.GetString("UpdateDownloadFailedMessage"),
                    CloseButtonText = AppLocalization.GetString("Ok"),
                    XamlRoot = xamlRoot
                };
                try
                {
                    await errorDialog.ShowAsync();
                }
                catch (Exception dialogException)
                {
                    System.Diagnostics.Debug.WriteLine($"显示更新失败提示失败：{dialogException}");
                }
            }
        }
    }

    private void SettingsWindow_SettingsChanged(AppSettings settings)
    {
        var languageChanged = !string.Equals(
            settings.Language,
            _settings.Language,
            StringComparison.Ordinal);
        if (languageChanged)
        {
            AppLocalization.SetLanguage(settings.Language);
        }

        NormalizePanelShortcuts(settings);
        if (_messageBridge is not null
            && _storageAvailable
            && !string.Equals(settings.GlobalHotKey, _settings.GlobalHotKey, StringComparison.Ordinal)
            && !_hotKey.TryReconfigure(_messageBridge, settings.GlobalHotKey, out var hotKeyError))
        {
            try
            {
                _settingsStore.Save(_settings);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }

            StatusText.Text = AppLocalization.Format("HotKeyNotApplied", hotKeyError);
            return;
        }

        _settings = settings;
        UpdateExcludedApplications();
        ApplyThemeSettings();
        CleanupHistory();
        if (languageChanged)
        {
            RefreshLocalization();
        }
        else
        {
            RefreshHistory();
        }

        StatusText.Text = AppLocalization.GetString("SettingsApplied");
    }

    private static void NormalizePanelShortcuts(AppSettings settings)
    {
        var defaults = new AppSettings();
        settings.PasteShortcut = PanelShortcut.NormalizeOrDefault(settings.PasteShortcut, defaults.PasteShortcut);
        settings.PlainTextPasteShortcut = PanelShortcut.NormalizeOrDefault(settings.PlainTextPasteShortcut, defaults.PlainTextPasteShortcut);
        settings.PreviewShortcut = PanelShortcut.NormalizeOrDefault(settings.PreviewShortcut, defaults.PreviewShortcut);
        settings.PinShortcut = PanelShortcut.NormalizeOrDefault(settings.PinShortcut, defaults.PinShortcut);
        settings.DeleteShortcut = PanelShortcut.NormalizeOrDefault(settings.DeleteShortcut, defaults.DeleteShortcut);
        settings.PasteAsFileShortcut = PanelShortcut.NormalizeOrDefault(settings.PasteAsFileShortcut, defaults.PasteAsFileShortcut);
    }

    private void ApplyThemeSettings()
    {
        var theme = _settings.ThemeMode switch
        {
            "Dark" => ElementTheme.Dark,
            "Light" => ElementTheme.Light,
            _ => ElementTheme.Default
        };
        WindowSurface.RequestedTheme = theme;
        _trayIcon?.SetTheme(theme);
    }

    private void ShowPanel(bool activatePanel)
    {
        if (activatePanel)
        {
            _panelShownWithoutActivation = false;
            _panelTargetWindow = IntPtr.Zero;
            _panelMonitorTimer?.Stop();
            SetPanelActivationMode(preventActivation: false);
            SetWindowPos(
                _handle,
                _isTopmost ? HwndTopmost : HwndNotopmost,
                0,
                0,
                0,
                0,
                SwpNoMove | SwpNoSize | SwpNoActivate);
            ShowWindow(_handle, SwShow);
            Activate();
            SetForegroundWindow(_handle);
            ResetHistoryScrollPosition();
            HistoryList.Focus(FocusState.Programmatic);
            return;
        }

        // 快捷键弹出时不抢占目标窗口焦点，临时置顶避免被目标窗口盖住。
        _panelTargetWindow = _pasteTarget != IntPtr.Zero && _pasteTarget != _handle
            ? _pasteTarget
            : GetForegroundWindow();
        _panelShownWithoutActivation = true;
        _panelMonitorTimer?.Start();
        SetPanelActivationMode(preventActivation: true);
        SetWindowPos(_handle, HwndTopmost, 0, 0, 0, 0, SwpNoMove | SwpNoSize | SwpNoActivate | SwpShowWindow);
        ResetHistoryScrollPosition();
    }

    private void ResetHistoryScrollPosition()
    {
        if (_displayItems.Count == 0)
        {
            return;
        }

        // ListView 会复用内部 ScrollViewer，面板重新显示时显式定位到第一条记录。
        _dispatcherQueue?.TryEnqueue(() =>
        {
            if (_displayItems.Count > 0)
            {
                HistoryList.ScrollIntoView(_displayItems[0], ScrollIntoViewAlignment.Leading);
            }

            // 隐藏面板时会释放图片流；再次显示时容器可能不会重新触发 Loaded，因此主动恢复可见卡片的图片预览。
            HistoryList.UpdateLayout();
            foreach (var item in _displayItems)
            {
                if (item.Item.Kind == ClipboardContentKind.Image
                    && HistoryList.ContainerFromItem(item) is not null)
                {
                    _ = item.EnsureThumbnailLoadedAsync(_repository.LoadContent);
                }
            }
        });
    }

    private void HidePanel()
    {
        CloseContentPreview();
        foreach (var item in _displayItems)
        {
            item.UnloadThumbnail();
        }

        if (!string.IsNullOrEmpty(SearchBox.Text))
        {
            SearchBox.Text = string.Empty;
        }

        _panelMonitorTimer?.Stop();
        _panelShownWithoutActivation = false;
        _panelTargetWindow = IntPtr.Zero;
        if (_handle != IntPtr.Zero)
        {
            ShowWindow(_handle, SwHide);
            if (!_isTopmost)
            {
                SetWindowPos(_handle, HwndNotopmost, 0, 0, 0, 0, SwpNoMove | SwpNoSize | SwpNoActivate);
            }
        }
    }

    private void PanelMonitorTimer_Tick(DispatcherQueueTimer sender, object args)
    {
        if (!_panelShownWithoutActivation
            || !_settings.AutoHideOnDeactivate
            || _isTopmost
            || _settingsWindowOpen)
        {
            return;
        }

        var foregroundWindow = GetForegroundWindow();
        if (foregroundWindow != IntPtr.Zero
            && foregroundWindow != _handle
            && foregroundWindow != _panelTargetWindow)
        {
            HidePanel();
            return;
        }

        if (IsMouseButtonDownOutsidePanel())
        {
            HidePanel();
        }
    }

    private bool IsMouseButtonDownOutsidePanel()
    {
        if (!IsMouseButtonDown()
            || !GetCursorPos(out var cursor)
            || !GetWindowRect(_handle, out var panelRect))
        {
            return false;
        }

        return cursor.X < panelRect.Left
            || cursor.X >= panelRect.Right
            || cursor.Y < panelRect.Top
            || cursor.Y >= panelRect.Bottom;
    }

    private static bool IsMouseButtonDown()
    {
        return IsKeyDown(VkLbutton) || IsKeyDown(VkRbutton) || IsKeyDown(VkMbutton);
    }

    private static bool IsKeyDown(int virtualKey)
    {
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    private void EnqueueOnUi(Action action)
    {
        if (_dispatcherQueue?.HasThreadAccess == true)
        {
            action();
            return;
        }

        _dispatcherQueue?.TryEnqueue(() => action());
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    private readonly record struct PasteTargetSnapshot(
        IntPtr TargetWindow,
        MonitorRect InputBounds,
        bool HasInputBounds);

    [StructLayout(LayoutKind.Sequential)]
    private struct GuiThreadInfo
    {
        public uint Size;
        public uint Flags;
        public IntPtr ActiveWindow;
        public IntPtr FocusWindow;
        public IntPtr CaptureWindow;
        public IntPtr MenuOwner;
        public IntPtr MoveSizeWindow;
        public IntPtr CaretWindow;
        public MonitorRect CaretRect;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MonitorInfo
    {
        public int Size;
        public MonitorRect Monitor;
        public MonitorRect Work;
        public uint Flags;
    }

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out NativePoint point);

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int virtualKey);

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromPoint(NativePoint point, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, IntPtr processId);

    [DllImport("user32.dll", EntryPoint = "GetWindowThreadProcessId")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool GetGUIThreadInfo(uint threadId, ref GuiThreadInfo threadInfo);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hwnd, out MonitorRect rect);

    [DllImport("user32.dll")]
    private static extern bool SystemParametersInfo(uint action, uint parameter, ref MonitorRect value, uint flags);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("user32.dll")]
    private static extern bool AttachThreadInput(uint sourceThreadId, uint targetThreadId, bool attach);

    [DllImport("user32.dll")]
    private static extern IntPtr GetFocus();

    [DllImport("user32.dll")]
    private static extern bool GetCaretPos(out NativePoint point);

    [DllImport("user32.dll")]
    private static extern bool ClientToScreen(IntPtr windowHandle, ref NativePoint point);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hwnd, int command);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hwnd, nint insertAfter, int x, int y, int width, int height, uint flags);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW", SetLastError = true)]
    private static extern nint GetWindowLongPtr(IntPtr hwnd, int index);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW", SetLastError = true)]
    private static extern nint SetWindowLongPtr(IntPtr hwnd, int index, nint value);

}
