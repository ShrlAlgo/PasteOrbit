using System.Runtime.InteropServices;

namespace PasteOrbit.App;

/// <summary>
/// 在 WinUI 窗口的 HWND 上安装轻量消息桥，供剪切板监听和全局快捷键共用。
/// </summary>
public sealed class Win32MessageBridge : IDisposable
{
    private const int GwlWndProc = -4;
    private const uint WmClose = 0x0010;
    private const uint WmMouseActivate = 0x0021;
    private const int MaNoactivate = 3;
    private readonly IntPtr _handle;
    private readonly WindowProcDelegate _windowProc;
    private IntPtr _previousWndProc;
    private bool _disposed;

    public Win32MessageBridge(IntPtr handle)
    {
        if (handle == IntPtr.Zero)
        {
            throw new ArgumentException("窗口句柄无效。", nameof(handle));
        }
        _handle = handle;
        _windowProc = WindowProc;
        _previousWndProc = SetWindowLongPtr(_handle, GwlWndProc, Marshal.GetFunctionPointerForDelegate(_windowProc));
        if (_previousWndProc == IntPtr.Zero)
        {
            throw new InvalidOperationException("无法安装 WinUI 窗口消息桥。");
        }
    }

    public event Action<uint, IntPtr, IntPtr>? Message;

    public event Func<bool>? CloseRequested;

    public IntPtr Handle => _handle;

    public bool PreventActivation { get; set; }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        SetWindowLongPtr(_handle, GwlWndProc, _previousWndProc);
        Message = null;
        CloseRequested = null;
        GC.KeepAlive(_windowProc);
    }

    private IntPtr WindowProc(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam)
    {
        if (!_disposed)
        {
            try
            {
                // 原生 WndProc 不能承载托管异常；否则任一消息订阅者异常都会直接终止进程。
                if (message == WmClose && CloseRequested?.Invoke() == true)
                {
                    return IntPtr.Zero;
                }

                Message?.Invoke(message, wParam, lParam);
            }
            catch (Exception exception)
            {
                System.Diagnostics.Debug.WriteLine($"Win32 消息处理失败（0x{message:X8}）：{exception}");
            }

            if (message == WmMouseActivate && PreventActivation)
            {
                // 非激活面板仍需接收鼠标消息，但不能夺走原输入框的焦点。
                return new IntPtr(MaNoactivate);
            }
        }

        return CallWindowProc(_previousWndProc, hwnd, message, wParam, lParam);
    }

    private static IntPtr SetWindowLongPtr(IntPtr hwnd, int index, IntPtr value)
    {
        return IntPtr.Size == 8
            ? SetWindowLongPtr64(hwnd, index, value)
            : new IntPtr(SetWindowLong32(hwnd, index, value.ToInt32()));
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate IntPtr WindowProcDelegate(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW", SetLastError = true)]
    private static extern IntPtr SetWindowLongPtr64(IntPtr hwnd, int index, IntPtr value);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongW", SetLastError = true)]
    private static extern int SetWindowLong32(IntPtr hwnd, int index, int value);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr CallWindowProc(IntPtr previousWndProc, IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
}
