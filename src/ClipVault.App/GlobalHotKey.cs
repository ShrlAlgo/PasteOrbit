using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace ClipVault.App;

public sealed class GlobalHotKey : IDisposable
{
    private const int HotKeyId = 0x4356;
    private const int WmHotKey = 0x0312;
    private const uint ModControl = 0x0002;
    private const uint ModShift = 0x0004;
    private const uint ModNoRepeat = 0x4000;
    private const uint KeyV = 0x56;
    private HwndSource? _source;

    public event Action<IntPtr>? Pressed;

    public void Start(Window window)
    {
        if (_source is not null)
        {
            throw new InvalidOperationException("全局快捷键已注册。");
        }

        var handle = new WindowInteropHelper(window).Handle;
        _source = HwndSource.FromHwnd(handle)
            ?? throw new InvalidOperationException("无法获取窗口消息源。");
        _source.AddHook(WindowProc);

        if (!RegisterHotKey(handle, HotKeyId, ModControl | ModShift | ModNoRepeat, KeyV))
        {
            _source.RemoveHook(WindowProc);
            _source = null;
            throw new Win32Exception(Marshal.GetLastWin32Error(), "快捷键 Ctrl+Shift+V 已被占用。");
        }
    }

    public void Dispose()
    {
        if (_source is null)
        {
            return;
        }

        UnregisterHotKey(_source.Handle, HotKeyId);
        _source.RemoveHook(WindowProc);
        _source = null;
    }

    private IntPtr WindowProc(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (message == WmHotKey && wParam.ToInt32() == HotKeyId)
        {
            Pressed?.Invoke(GetForegroundWindow());
            handled = true;
        }

        return IntPtr.Zero;
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RegisterHotKey(IntPtr hwnd, int id, uint modifiers, uint virtualKey);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnregisterHotKey(IntPtr hwnd, int id);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();
}
