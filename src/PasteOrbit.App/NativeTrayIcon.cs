using System.Runtime.InteropServices;

using Microsoft.UI.Xaml;

namespace PasteOrbit.App;

/// <summary>
/// 使用 Shell_NotifyIcon 提供不依赖 WPF/WinForms 的 Windows 托盘入口。
/// </summary>
public sealed class NativeTrayIcon : IDisposable
{
    private const uint NifMessage = 0x00000001;
    private const uint NifIcon = 0x00000002;
    private const uint NifTip = 0x00000004;
    private const uint NimAdd = 0x00000000;
    private const uint NimDelete = 0x00000002;
    private const uint NimSetVersion = 0x00000004;
    private const uint WmTrayCallback = 0x8001;
    private const uint WmContextMenu = 0x007B;
    private const uint WmNull = 0x0000;
    private const uint WmLButtonDblClk = 0x0203;
    private const uint WmRButtonUp = 0x0205;
    private const uint NotifyIconVersion4 = 4;
    private const uint TrayIconId = 1;
    private const uint ImageIcon = 1;
    private const uint LrLoadFromFile = 0x00000010;
    private const uint LrDefaultSize = 0x00000040;
    private const uint MfString = 0x00000000;
    private const uint MfSeparator = 0x00000800;
    private const uint TpmRightButton = 0x00000002;
    private const uint TpmReturnCommand = 0x00000100;
    private const uint MenuOpen = 1001;
    private const uint MenuSettings = 1002;
    private const uint MenuExit = 1003;
    private const uint MenuPause = 1004;

    private readonly Win32MessageBridge _bridge;
    private readonly string _iconPath;
    private NotifyIconData _notifyIconData;
    private WinUiTrayMenuHost? _winUiMenuHost;
    private IntPtr _icon;
    private bool _disposed;
    private bool _menuShowing;
    private long _lastContextMenuTick;

    public NativeTrayIcon(Win32MessageBridge bridge, string iconPath)
    {
        _bridge = bridge;
        _iconPath = iconPath;
        _notifyIconData = new NotifyIconData
        {
            Size = (uint)Marshal.SizeOf<NotifyIconData>(),
            WindowHandle = bridge.Handle,
            Id = TrayIconId,
            Flags = NifMessage | NifIcon | NifTip,
            CallbackMessage = WmTrayCallback
        };
        _notifyIconData.SetTip("PasteOrbit · 剪切板历史");
        _icon = LoadImage(IntPtr.Zero, _iconPath, ImageIcon, 0, 0, LrLoadFromFile | LrDefaultSize);
        _notifyIconData.IconHandle = _icon;
        if (_icon == IntPtr.Zero || !ShellNotifyIcon(NimAdd, ref _notifyIconData))
        {
            DestroyIcon(_icon);
            throw new InvalidOperationException("无法创建 PasteOrbit 托盘图标。");
        }

        // 使用新版托盘回调协议，右键通常以 WM_CONTEXTMENU 发送；兼容处理仍保留旧版消息。
        _notifyIconData.VersionOrTimeout = NotifyIconVersion4;
        ShellNotifyIcon(NimSetVersion, ref _notifyIconData);

        try
        {
            _winUiMenuHost = new WinUiTrayMenuHost(bridge.Handle, TrayIconId);
            _winUiMenuHost.CommandInvoked += WinUiMenuHost_CommandInvoked;
        }
        catch (Exception exception)
        {
            System.Diagnostics.Debug.WriteLine($"WinUI 托盘菜单宿主初始化失败：{exception}");
        }

        _bridge.Message += Bridge_Message;
    }

    public event Action? OpenRequested;

    public event Action? SettingsRequested;

    public event Action? PauseRequested;

    public event Action? ExitRequested;

    public event Action<int, int>? ContextMenuRequested;

    public bool IsListeningPaused { get; set; }

    /// <summary>
    /// 显示 WinUI 托盘菜单；宿主不可用时回退到 Windows 原生菜单。
    /// </summary>
    public void ShowContextMenu(int screenX, int screenY)
    {
        if (_disposed || _menuShowing)
        {
            return;
        }

        if (_winUiMenuHost?.TryShow(screenX, screenY, IsListeningPaused) == true)
        {
            return;
        }

        ShowNativeContextMenu(screenX, screenY);
    }

    public void SetTheme(ElementTheme theme)
    {
        _winUiMenuHost?.SetTheme(theme);
    }

    private void ShowNativeContextMenu(int screenX, int screenY)
    {
        if (_disposed || _menuShowing)
        {
            return;
        }

        // 托盘可能连续投递右键消息，禁止同一时间进入两个 TrackPopupMenuEx 消息循环。
        _menuShowing = true;
        var menu = CreatePopupMenu();
        if (menu == IntPtr.Zero)
        {
            _menuShowing = false;
            return;
        }

        try
        {
            AppendMenu(menu, MfString, MenuOpen, "打开历史");
            AppendMenu(menu, MfString, MenuPause, IsListeningPaused ? "恢复监听" : "暂停 10 分钟");
            AppendMenu(menu, MfString, MenuSettings, "设置");
            AppendMenu(menu, MfSeparator, 0, null);
            AppendMenu(menu, MfString, MenuExit, "退出");

            SetForegroundWindow(_bridge.Handle);
            var command = TrackPopupMenuEx(
                menu,
                TpmReturnCommand | TpmRightButton,
                screenX,
                screenY,
                _bridge.Handle,
                IntPtr.Zero);

            // 按照 TrackPopupMenu 的约定发送 WM_NULL，使菜单在无选择时也能正确收起。
            PostMessage(_bridge.Handle, WmNull, IntPtr.Zero, IntPtr.Zero);
            switch (command)
            {
                case MenuOpen:
                    OpenRequested?.Invoke();
                    break;
                case MenuSettings:
                    SettingsRequested?.Invoke();
                    break;
                case MenuPause:
                    PauseRequested?.Invoke();
                    break;
                case MenuExit:
                    ExitRequested?.Invoke();
                    break;
            }
        }
        finally
        {
            DestroyMenu(menu);
            _menuShowing = false;
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _bridge.Message -= Bridge_Message;
        if (_winUiMenuHost is not null)
        {
            _winUiMenuHost.CommandInvoked -= WinUiMenuHost_CommandInvoked;
            _winUiMenuHost.Dispose();
            _winUiMenuHost = null;
        }

        ShellNotifyIcon(NimDelete, ref _notifyIconData);
        DestroyIcon(_icon);
        _icon = IntPtr.Zero;
    }

    private void Bridge_Message(uint message, IntPtr wParam, IntPtr lParam)
    {
        if (message != WmTrayCallback)
        {
            return;
        }

        // 新版托盘协议把事件码放在 lParam 的低 16 位，高位可能携带附加数据。
        var mouseMessage = unchecked((uint)lParam.ToInt64()) & 0xFFFFu;
        if (mouseMessage == WmLButtonDblClk)
        {
            OpenRequested?.Invoke();
        }
        else if (mouseMessage == WmContextMenu || mouseMessage == WmRButtonUp)
        {
            // 某些 Windows 版本会连续发送 WM_RBUTTONUP 和 WM_CONTEXTMENU，只显示一次菜单。
            var currentTick = Environment.TickCount64;
            if (currentTick - _lastContextMenuTick < 250)
            {
                return;
            }

            _lastContextMenuTick = currentTick;
            if (GetCursorPos(out var point))
            {
                ContextMenuRequested?.Invoke(point.X, point.Y);
            }
        }
    }

    private void WinUiMenuHost_CommandInvoked(WinUiTrayMenuHost.TrayMenuCommand command)
    {
        switch (command)
        {
            case WinUiTrayMenuHost.TrayMenuCommand.OpenHistory:
                OpenRequested?.Invoke();
                break;
            case WinUiTrayMenuHost.TrayMenuCommand.ToggleListening:
                PauseRequested?.Invoke();
                break;
            case WinUiTrayMenuHost.TrayMenuCommand.OpenSettings:
                SettingsRequested?.Invoke();
                break;
            case WinUiTrayMenuHost.TrayMenuCommand.Exit:
                ExitRequested?.Invoke();
                break;
        }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NotifyIconData
    {
        public uint Size;
        public IntPtr WindowHandle;
        public uint Id;
        public uint Flags;
        public uint CallbackMessage;
        public IntPtr IconHandle;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Tip;
        public uint State;
        public uint StateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Info;
        public uint VersionOrTimeout;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string InfoTitle;
        public uint InfoFlags;
        public Guid GuidItem;
        public IntPtr BalloonIcon;

        public void SetTip(string value)
        {
            Tip = value;
            Info = string.Empty;
            InfoTitle = string.Empty;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }

    [DllImport("shell32.dll", EntryPoint = "Shell_NotifyIconW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShellNotifyIcon(uint message, ref NotifyIconData data);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadImage(IntPtr instance, string name, uint imageType, int width, int height, uint loadOptions);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyIcon(IntPtr icon);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCursorPos(out NativePoint point);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr CreatePopupMenu();

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AppendMenu(IntPtr menu, uint flags, uint itemId, string? itemText);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint TrackPopupMenuEx(
        IntPtr menu,
        uint flags,
        int x,
        int y,
        IntPtr ownerWindow,
        IntPtr parameters);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyMenu(IntPtr menu);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr windowHandle);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessage(IntPtr windowHandle, uint message, IntPtr wParam, IntPtr lParam);
}
