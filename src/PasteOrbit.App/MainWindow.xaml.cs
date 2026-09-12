using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Threading;

using PasteOrbit.Core;

namespace PasteOrbit.App;

public partial class MainWindow : Window
{
    private readonly App _app;
    private readonly ClipboardHistory _history;
    private readonly ObservableCollection<HistoryListItem> _items = [];
    private readonly DispatcherTimer _searchTimer;
    private Win32MessageBridge? _messageBridge;
    private WpfClipboardMonitor? _monitor;
    private WpfClipboardPlayback? _playback;
    private GlobalHotKey? _hotKey;
    private ClipboardContentKind? _filterKind;
    private IntPtr _targetWindow;
    private UiAutomationTarget? _automationTarget;
    private bool _allowClose;
    private bool _isWindowPinned;

    public MainWindow(App app, ClipboardHistory history)
    {
        _app = app;
        _history = history;
        InitializeComponent();
        HistoryList.ItemsSource = _items;
        _searchTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(180) };
        _searchTimer.Tick += (_, _) =>
        {
            _searchTimer.Stop();
            ReloadHistory();
        };
        SourceInitialized += OnSourceInitialized;
        Deactivated += (_, _) =>
        {
            if (_app.Settings.AutoHideOnDeactivate && !_isWindowPinned)
            {
                Hide();
            }
        };
        Closing += OnClosing;
    }

    public void ShowPanel(IntPtr targetWindow = default)
    {
        _targetWindow = targetWindow != IntPtr.Zero ? targetWindow : GetForegroundWindow();
        _automationTarget = UiAutomationTarget.Capture(_targetWindow);
        ReloadHistory();
        PositionNearInput(_targetWindow);
        Show();
        Activate();
        SearchBox.Focus();
    }

    public void CloseForExit()
    {
        _allowClose = true;
        Close();
    }

    public void ApplySettings()
    {
        if (_messageBridge is null || _hotKey is null)
        {
            return;
        }

        if (!_hotKey.TryReconfigure(_messageBridge, _app.Settings.GlobalHotKey, out var error))
        {
            MessageBox.Show(error, "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        var handle = new WindowInteropHelper(this).Handle;
        ApplyNativeWindowCorners(handle);
        _messageBridge = new Win32MessageBridge(handle);
        _monitor = new WpfClipboardMonitor(Dispatcher, _app.Settings);
        _monitor.Captured += capture =>
        {
            try
            {
                _history.AddOrUpdate(capture);
                _history.Cleanup(DateTimeOffset.UtcNow.AddDays(-_app.Settings.RetentionDays), _app.Settings.MaxHistoryEntries);
                if (IsVisible)
                {
                    ReloadHistory();
                }
            }
            catch (Exception exception)
            {
                Debug.WriteLine($"保存剪切板历史失败：{exception}");
            }
        };
        _monitor.Start(_messageBridge);
        _playback = new WpfClipboardPlayback(_app.Repository, _monitor);
        _hotKey = new GlobalHotKey();
        _hotKey.Pressed += target => Dispatcher.Invoke(() => ShowPanel(target));
        try
        {
            _hotKey.Start(_messageBridge, _app.Settings.GlobalHotKey);
        }
        catch (Exception exception)
        {
            MessageBox.Show(exception.Message, "PasteOrbit", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void ReloadHistory()
    {
        var selectedId = (HistoryList.SelectedItem as HistoryListItem)?.Id;
        var query = new ClipboardHistoryQuery(SearchBox.Text.Trim(), _filterKind);
        var page = _history.Search(query, null, 120);
        _items.Clear();
        foreach (var entry in page.Items)
        {
            var item = new HistoryListItem(entry);
            _items.Add(item);
        }

        HistoryList.SelectedItem = _items.FirstOrDefault(item => item.Id == selectedId) ?? _items.FirstOrDefault();
        if (HistoryList.Items.Count > 0)
        {
            HistoryList.ScrollIntoView(HistoryList.Items[0]);
        }
    }

    private void TryLoadThumbnail(HistoryListItem item)
    {
        try
        {
            item.LoadThumbnail(_app.Repository.LoadContent(item.Id));
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"加载图片缩略图失败：{exception}");
        }
    }

    private void Thumbnail_Loaded(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is HistoryListItem item
            && item.Entry.Kind == ClipboardContentKind.Image)
        {
            Dispatcher.BeginInvoke(() => TryLoadThumbnail(item), DispatcherPriority.Background);
        }
    }

    private async Task PasteSelectedAsync(bool plainTextOnly = false)
    {
        if (HistoryList.SelectedItem is not HistoryListItem item || _playback is null)
        {
            return;
        }

        Hide();
        await _playback.PasteAsync(
            item.Entry,
            _targetWindow,
            plainTextOnly,
            () => _automationTarget?.TryRestoreFocus() == true);
    }

    private void SearchBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        _searchTimer.Stop();
        _searchTimer.Start();
    }

    private void Filter_Checked(object sender, RoutedEventArgs e)
    {
        if (!IsLoaded)
        {
            return;
        }

        AllFilter.IsChecked = ReferenceEquals(sender, AllFilter);
        TextFilter.IsChecked = ReferenceEquals(sender, TextFilter);
        ImageFilter.IsChecked = ReferenceEquals(sender, ImageFilter);
        FilesFilter.IsChecked = ReferenceEquals(sender, FilesFilter);
        _filterKind = ReferenceEquals(sender, TextFilter) ? ClipboardContentKind.Text
            : ReferenceEquals(sender, ImageFilter) ? ClipboardContentKind.Image
            : ReferenceEquals(sender, FilesFilter) ? ClipboardContentKind.Files
            : null;
        ReloadHistory();
    }

    private async void HistoryList_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (FindAncestor<System.Windows.Controls.Button>(e.OriginalSource as DependencyObject) is not null)
        {
            return;
        }

        var item = FindAncestor<ListBoxItem>(e.OriginalSource as DependencyObject);
        if (item is null)
        {
            return;
        }

        HistoryList.SelectedItem = item.DataContext;
        await PasteSelectedAsync();
    }

    private async void HistoryList_KeyDown(object sender, System.Windows.Input.KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
        {
            e.Handled = true;
            await PasteSelectedAsync(Keyboard.Modifiers.HasFlag(ModifierKeys.Shift));
        }
        else if (e.Key == Key.Delete)
        {
            DeleteSelectedItem();
        }
        else if (e.Key == Key.S && Keyboard.Modifiers == (ModifierKeys.Control | ModifierKeys.Shift))
        {
            e.Handled = true;
            await PasteAsFileAsync(HistoryList.SelectedItem as HistoryListItem);
        }
    }

    private void PinItemButton_Click(object sender, RoutedEventArgs e)
    {
        e.Handled = true;
        if ((sender as FrameworkElement)?.DataContext is not HistoryListItem item)
        {
            return;
        }

        var updated = _history.SetPinned(item.Id, !item.IsPinned);
        if (updated is not null)
        {
            ReloadHistory();
        }
    }

    private void DeleteItemButton_Click(object sender, RoutedEventArgs e)
    {
        e.Handled = true;
        if ((sender as FrameworkElement)?.DataContext is HistoryListItem item && _history.Remove(item.Id))
        {
            ReloadHistory();
        }
    }

    private void DeleteSelectedItem()
    {
        if (HistoryList.SelectedItem is HistoryListItem item && _history.Remove(item.Id))
        {
            ReloadHistory();
        }
    }

    private async void PastePlainTextMenuItem_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is HistoryListItem item)
        {
            HistoryList.SelectedItem = item;
            await PasteSelectedAsync(plainTextOnly: true);
        }
    }

    private async void PasteAsFileMenuItem_Click(object sender, RoutedEventArgs e)
    {
        await PasteAsFileAsync((sender as FrameworkElement)?.DataContext as HistoryListItem);
    }

    private void DeleteMenuItem_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is HistoryListItem item && _history.Remove(item.Id))
        {
            ReloadHistory();
        }
    }

    private async Task PasteAsFileAsync(HistoryListItem? item)
    {
        if (item is null)
        {
            return;
        }

        var content = _app.Repository.LoadContent(item.Id);
        if (await ExplorerFilePaste.TrySaveAsync(item.Entry, content, _targetWindow))
        {
            Hide();
        }
    }

    private void ClearButton_Click(object sender, RoutedEventArgs e)
    {
        var query = new ClipboardHistoryQuery(SearchBox.Text.Trim(), _filterKind);
        if (MessageBox.Show(AppLocalization.GetString("ClearCurrentListMessage"), AppLocalization.GetString("ClearCurrentListTitle"),
                MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
        {
            return;
        }

        _history.DeleteMatching(query);
        ReloadHistory();
    }

    private void SettingsButton_Click(object sender, RoutedEventArgs e) => _app.ShowSettings();

    private void GitHubButton_Click(object sender, RoutedEventArgs e)
    {
        Process.Start(new ProcessStartInfo("https://github.com/ShrlAlgo/PasteOrbit") { UseShellExecute = true });
    }

    private void PinWindowButton_Click(object sender, RoutedEventArgs e)
    {
        _isWindowPinned = !_isWindowPinned;
        Topmost = _isWindowPinned;
        PinWindowButton.Foreground = (System.Windows.Media.Brush)System.Windows.Application.Current.Resources[
            _isWindowPinned ? "AccentFillColorDefaultBrush" : "TextFillColorPrimaryBrush"];
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Hide();

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.LeftButton == MouseButtonState.Pressed)
        {
            DragMove();
        }
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (_allowClose)
        {
            _hotKey?.Dispose();
            _monitor?.Dispose();
            _playback?.Dispose();
            _messageBridge?.Dispose();
            return;
        }

        e.Cancel = true;
        Hide();
    }

    private void PositionNearInput(IntPtr targetWindow)
    {
        var anchorBounds = default(Rect);
        if (_automationTarget is not null)
        {
            var inputBounds = _automationTarget.AnchorBounds;
            anchorBounds = new Rect
            {
                Left = Convert.ToInt32(Math.Round(inputBounds.Left)),
                Top = Convert.ToInt32(Math.Round(inputBounds.Top)),
                Right = Convert.ToInt32(Math.Round(inputBounds.Right)),
                Bottom = Convert.ToInt32(Math.Round(inputBounds.Bottom))
            };
        }
        else if (TryGetCaretBounds(targetWindow, out var caretBounds))
        {
            anchorBounds = caretBounds;
        }
        else if (GetCursorPos(out var cursorPoint))
        {
            anchorBounds = new Rect
            {
                Left = cursorPoint.X,
                Top = cursorPoint.Y,
                Right = cursorPoint.X + 1,
                Bottom = cursorPoint.Y + 20
            };
        }

        MoveWindowNearAnchor(anchorBounds);
    }

    private void MoveWindowNearAnchor(Rect anchorBounds)
    {
        var windowHandle = new WindowInteropHelper(this).Handle;
        if (windowHandle == IntPtr.Zero)
        {
            return;
        }

        var monitor = MonitorFromRect(ref anchorBounds, 2);
        var monitorInfo = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        if (monitor == IntPtr.Zero || !GetMonitorInfo(monitor, ref monitorInfo))
        {
            return;
        }

        GetWindowRect(windowHandle, out var panelBounds);
        var panelWidth = Math.Max(1, panelBounds.Right - panelBounds.Left);
        var panelHeight = Math.Max(1, panelBounds.Bottom - panelBounds.Top);
        var x = Math.Clamp(anchorBounds.Left, monitorInfo.WorkArea.Left, monitorInfo.WorkArea.Right - panelWidth);
        var y = anchorBounds.Bottom + 8;
        if (y + panelHeight > monitorInfo.WorkArea.Bottom)
        {
            y = anchorBounds.Top - panelHeight - 8;
        }

        y = Math.Clamp(y, monitorInfo.WorkArea.Top, monitorInfo.WorkArea.Bottom - panelHeight);
        SetWindowPos(windowHandle, IntPtr.Zero, x, y, 0, 0, 0x0001 | 0x0004 | 0x0010);
    }

    private static bool TryGetCaretBounds(IntPtr targetWindow, out Rect bounds)
    {
        bounds = default;
        if (targetWindow == IntPtr.Zero)
        {
            return false;
        }

        var info = new GuiThreadInfo { Size = Marshal.SizeOf<GuiThreadInfo>() };
        var threadId = GetWindowThreadProcessId(targetWindow, out _);
        if (!GetGUIThreadInfo(threadId, ref info) || info.CaretWindow == IntPtr.Zero)
        {
            return false;
        }

        var topLeft = new Point { X = info.CaretBounds.Left, Y = info.CaretBounds.Top };
        var bottomRight = new Point { X = info.CaretBounds.Right, Y = info.CaretBounds.Bottom };
        if (!ClientToScreen(info.CaretWindow, ref topLeft) || !ClientToScreen(info.CaretWindow, ref bottomRight))
        {
            return false;
        }

        bounds = new Rect { Left = topLeft.X, Top = topLeft.Y, Right = bottomRight.X, Bottom = bottomRight.Y };
        return true;
    }

    private static T? FindAncestor<T>(DependencyObject? source) where T : DependencyObject
    {
        while (source is not null)
        {
            if (source is T match)
            {
                return match;
            }

            source = System.Windows.Media.VisualTreeHelper.GetParent(source);
        }

        return null;
    }

    private static void ApplyNativeWindowCorners(IntPtr windowHandle)
    {
        // 无标题栏窗口仍交给 DWM 裁切边界，避免透明 WPF 窗口与 Fluent 控件混用。
        const int windowCornerPreferenceAttribute = 33;
        var preference = 2;
        DwmSetWindowAttribute(windowHandle, windowCornerPreferenceAttribute, ref preference, sizeof(int));
    }

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(IntPtr windowHandle, out Rect bounds);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr windowHandle, out uint processId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetGUIThreadInfo(uint threadId, ref GuiThreadInfo info);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ClientToScreen(IntPtr windowHandle, ref Point point);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCursorPos(out Point point);

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromRect(ref Rect bounds, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo monitorInfo);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        IntPtr windowHandle,
        IntPtr insertAfter,
        int x,
        int y,
        int width,
        int height,
        uint flags);

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr windowHandle, int attribute, ref int value, int valueSize);

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct GuiThreadInfo
    {
        public int Size;
        public uint Flags;
        public IntPtr ActiveWindow;
        public IntPtr FocusWindow;
        public IntPtr CaptureWindow;
        public IntPtr MenuOwnerWindow;
        public IntPtr MoveSizeWindow;
        public IntPtr CaretWindow;
        public Rect CaretBounds;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    private struct MonitorInfo
    {
        public int Size;
        public Rect MonitorArea;
        public Rect WorkArea;
        public uint Flags;
    }
}
