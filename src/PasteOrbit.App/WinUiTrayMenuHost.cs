using System.Diagnostics;
using System.Runtime.InteropServices;

using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

using Windows.Graphics;
using WinRT.Interop;

namespace PasteOrbit.App;

internal sealed class WinUiTrayMenuHost : IDisposable
{
    private const int GwlStyle = -16;
    private const int SwHide = 0;
    private const int SwShow = 5;
    private const int WindowStylePopup = unchecked((int)0x80000000);
    private const uint WmActivate = 0x0006;
    private const uint WmGetMinMaxInfo = 0x0024;

    private readonly IntPtr _trayOwnerWindow;
    private readonly uint _trayIconId;
    private readonly Window _window;
    private readonly Grid _root;
    private readonly AppWindow _appWindow;
    private readonly Win32MessageBridge _messageBridge;
    private readonly IntPtr _windowHandle;
    private MenuFlyout? _currentMenu;
    private TrayMenuCommand? _pendingCommand;
    private bool _disposed;
    private bool _menuShowing;

    public WinUiTrayMenuHost(IntPtr trayOwnerWindow, uint trayIconId)
    {
        if (trayOwnerWindow == IntPtr.Zero)
        {
            throw new ArgumentException("托盘宿主窗口句柄无效。", nameof(trayOwnerWindow));
        }

        _trayOwnerWindow = trayOwnerWindow;
        _trayIconId = trayIconId;
        _window = new Window();
        _root = new Grid();
        _window.Content = _root;
        _windowHandle = WindowNative.GetWindowHandle(_window);
        _messageBridge = new Win32MessageBridge(_windowHandle);
        _messageBridge.Message += MessageBridge_Message;
        _appWindow = AppWindow.GetFromWindowId(Win32Interop.GetWindowIdFromWindow(_windowHandle));
        _appWindow.IsShownInSwitchers = false;

        SetWindowStyle(_windowHandle, GwlStyle, new IntPtr(WindowStylePopup));
        if (_appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsAlwaysOnTop = true;
            presenter.IsResizable = false;
            presenter.IsMaximizable = false;
            presenter.IsMinimizable = false;
            presenter.SetBorderAndTitleBar(false, false);
        }
    }

    public event Action<TrayMenuCommand>? CommandInvoked;

    public enum TrayMenuCommand
    {
        OpenHistory,
        ToggleListening,
        OpenSettings,
        Exit
    }

    public void SetTheme(ElementTheme theme)
    {
        _root.RequestedTheme = theme;
    }

    public bool TryShow(int screenX, int screenY, bool isListeningPaused)
    {
        if (_disposed)
        {
            return false;
        }

        if (_menuShowing)
        {
            return true;
        }

        _menuShowing = true;
        try
        {
            var menu = CreateMenu(isListeningPaused);
            _currentMenu = menu;
            _root.ContextFlyout = menu;

            var anchorX = screenX;
            var anchorY = screenY;
            var exclusionWidth = 1;
            var exclusionHeight = 1;
            if (TryGetTrayIconRect(out var iconRect))
            {
                anchorX = iconRect.Left;
                anchorY = iconRect.Top;
                exclusionWidth = Math.Max(1, iconRect.Right - iconRect.Left);
                exclusionHeight = Math.Max(1, iconRect.Bottom - iconRect.Top);
            }

            var displayArea = DisplayArea.GetFromPoint(
                new PointInt32(anchorX, anchorY),
                DisplayAreaFallback.Primary);
            _appWindow.MoveAndResize(new RectInt32(anchorX, anchorY, 0, 0), displayArea);
            _window.Activate();
            ShowWindow(_windowHandle, SwShow);
            SetForegroundWindow(_windowHandle);

            var rasterizationScale = _root.XamlRoot?.RasterizationScale ?? 1d;
            menu.ShowAt(_root, new FlyoutShowOptions
            {
                Position = new Windows.Foundation.Point(0, 0),
                ExclusionRect = new Windows.Foundation.Rect(
                    0,
                    0,
                    exclusionWidth / rasterizationScale,
                    exclusionHeight / rasterizationScale)
            });
            return true;
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"WinUI 托盘菜单显示失败：{exception}");
            CloseCurrentMenu(invokeCommand: false);
            return false;
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        CloseCurrentMenu(invokeCommand: false);
        _messageBridge.Message -= MessageBridge_Message;
        _messageBridge.Dispose();
        _window.Close();
        CommandInvoked = null;
    }

    private void MessageBridge_Message(uint message, IntPtr wParam, IntPtr lParam)
    {
        if (message == WmGetMinMaxInfo && lParam != IntPtr.Zero)
        {
            var minMaxInfo = Marshal.PtrToStructure<NativeMinMaxInfo>(lParam);
            minMaxInfo.MinTrackSize = default;
            Marshal.StructureToPtr(minMaxInfo, lParam, false);
        }
        else if (message == WmActivate && (wParam.ToInt64() & 0xFFFF) == 0)
        {
            _root.DispatcherQueue.TryEnqueue(() => _currentMenu?.Hide());
        }
    }

    private MenuFlyout CreateMenu(bool isListeningPaused)
    {
        var menu = new MenuFlyout
        {
            ShouldConstrainToRootBounds = false
        };
        menu.Items.Add(CreateCommandItem(AppLocalization.GetString("TrayOpenHistory"), TrayMenuCommand.OpenHistory));
        menu.Items.Add(CreateCommandItem(
            isListeningPaused
                ? AppLocalization.GetString("TrayResumeMonitoring")
                : AppLocalization.GetString("TrayPauseMonitoring"),
            TrayMenuCommand.ToggleListening));
        menu.Items.Add(CreateCommandItem(AppLocalization.GetString("TraySettings"), TrayMenuCommand.OpenSettings));
        menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(CreateCommandItem(AppLocalization.GetString("TrayExit"), TrayMenuCommand.Exit));
        menu.Closed += CurrentMenu_Closed;
        return menu;
    }

    private MenuFlyoutItem CreateCommandItem(string text, TrayMenuCommand command)
    {
        var item = new MenuFlyoutItem
        {
            Text = text,
            Tag = command
        };
        item.Click += CommandItem_Click;
        return item;
    }

    private void CommandItem_Click(object sender, RoutedEventArgs e)
    {
        if (sender is MenuFlyoutItem { Tag: TrayMenuCommand command })
        {
            _pendingCommand = command;
        }
    }

    private void CurrentMenu_Closed(object? sender, object e)
    {
        if (sender is MenuFlyout menu)
        {
            menu.Closed -= CurrentMenu_Closed;
        }

        _currentMenu = null;
        ShowWindow(_windowHandle, SwHide);
        _menuShowing = false;
        var command = _pendingCommand;
        _pendingCommand = null;
        if (command is TrayMenuCommand selectedCommand)
        {
            CommandInvoked?.Invoke(selectedCommand);
        }
    }

    private void CloseCurrentMenu(bool invokeCommand)
    {
        var menu = _currentMenu;
        _currentMenu = null;
        if (menu is not null)
        {
            menu.Closed -= CurrentMenu_Closed;
            if (menu.IsOpen)
            {
                menu.Hide();
            }
        }

        ShowWindow(_windowHandle, SwHide);
        _menuShowing = false;
        var command = _pendingCommand;
        _pendingCommand = null;
        if (invokeCommand && command is TrayMenuCommand selectedCommand)
        {
            CommandInvoked?.Invoke(selectedCommand);
        }
    }

    private bool TryGetTrayIconRect(out NativeRect iconRect)
    {
        var identifier = new NotifyIconIdentifier
        {
            Size = (uint)Marshal.SizeOf<NotifyIconIdentifier>(),
            WindowHandle = _trayOwnerWindow,
            Id = _trayIconId
        };
        return ShellNotifyIconGetRect(ref identifier, out iconRect) == 0;
    }

    private static IntPtr SetWindowStyle(IntPtr windowHandle, int index, IntPtr value)
    {
        return IntPtr.Size == 8
            ? SetWindowLongPtr64(windowHandle, index, value)
            : new IntPtr(SetWindowLong32(windowHandle, index, value.ToInt32()));
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeMinMaxInfo
    {
        public NativePoint Reserved;
        public NativePoint MaxSize;
        public NativePoint MaxPosition;
        public NativePoint MinTrackSize;
        public NativePoint MaxTrackSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NotifyIconIdentifier
    {
        public uint Size;
        public IntPtr WindowHandle;
        public uint Id;
        public Guid GuidItem;
    }

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW", SetLastError = true)]
    private static extern IntPtr SetWindowLongPtr64(IntPtr windowHandle, int index, IntPtr value);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongW", SetLastError = true)]
    private static extern int SetWindowLong32(IntPtr windowHandle, int index, int value);

    [DllImport("shell32.dll", EntryPoint = "Shell_NotifyIconGetRect")]
    private static extern int ShellNotifyIconGetRect(
        ref NotifyIconIdentifier identifier,
        out NativeRect iconRect);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindow(IntPtr windowHandle, int command);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr windowHandle);
}
