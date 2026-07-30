using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media.Imaging;
using ClipVault.Core;

namespace ClipVault.App;

public sealed class ClipboardMonitor : IDisposable
{
    private const int WmClipboardUpdate = 0x031D;
    private const int RetryCount = 3;
    private HwndSource? _source;
    private int _capturePending;

    public event Action<ClipboardCapture>? Captured;

    public event Action<Exception>? CaptureFailed;

    public void Start(Window window)
    {
        if (_source is not null)
        {
            throw new InvalidOperationException("剪切板监听已启动。");
        }

        var handle = new WindowInteropHelper(window).Handle;
        _source = HwndSource.FromHwnd(handle)
            ?? throw new InvalidOperationException("无法获取窗口消息源。");
        _source.AddHook(WindowProc);

        if (!AddClipboardFormatListener(handle))
        {
            _source.RemoveHook(WindowProc);
            _source = null;
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法注册剪切板监听。");
        }
    }

    public void Dispose()
    {
        if (_source is null)
        {
            return;
        }

        RemoveClipboardFormatListener(_source.Handle);
        _source.RemoveHook(WindowProc);
        _source = null;
    }

    private IntPtr WindowProc(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (message == WmClipboardUpdate && Interlocked.Exchange(ref _capturePending, 1) == 0)
        {
            _source?.Dispatcher.BeginInvoke(CaptureWithRetryAsync);
        }

        return IntPtr.Zero;
    }

    private async void CaptureWithRetryAsync()
    {
        try
        {
            for (var attempt = 0; attempt < RetryCount; attempt++)
            {
                try
                {
                    var capture = ReadClipboard();
                    if (capture is not null)
                    {
                        Captured?.Invoke(capture);
                    }

                    return;
                }
                catch (ExternalException) when (attempt < RetryCount - 1)
                {
                    // 剪切板可能被来源程序短暂占用，有限重试避免阻塞界面线程。
                    await Task.Delay(40 * (attempt + 1));
                }
            }
        }
        catch (Exception exception)
        {
            CaptureFailed?.Invoke(exception);
        }
        finally
        {
            Interlocked.Exchange(ref _capturePending, 0);
        }
    }

    private static ClipboardCapture? ReadClipboard()
    {
        var sourceApplication = GetForegroundProcessName();

        if (Clipboard.ContainsFileDropList())
        {
            var paths = Clipboard.GetFileDropList().Cast<string>().ToArray();
            if (paths.Length == 0)
            {
                return null;
            }

            return new ClipboardCapture(
                ClipboardContentKind.Files,
                string.Join(Environment.NewLine, paths.Select(Path.GetFileName)),
                JsonSerializer.SerializeToUtf8Bytes(paths),
                sourceApplication);
        }

        if (Clipboard.ContainsImage())
        {
            var image = Clipboard.GetImage();
            if (image is null)
            {
                return null;
            }

            using var stream = new MemoryStream();
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(image));
            encoder.Save(stream);

            return new ClipboardCapture(
                ClipboardContentKind.Image,
                $"图片 {image.PixelWidth}×{image.PixelHeight}",
                stream.ToArray(),
                sourceApplication);
        }

        if (Clipboard.ContainsText(TextDataFormat.UnicodeText))
        {
            var text = Clipboard.GetText(TextDataFormat.UnicodeText);
            if (!string.IsNullOrEmpty(text))
            {
                return new ClipboardCapture(
                    ClipboardContentKind.Text,
                    text,
                    Encoding.UTF8.GetBytes(text),
                    sourceApplication);
            }
        }

        return null;
    }

    private static string? GetForegroundProcessName()
    {
        var window = GetForegroundWindow();
        if (window == IntPtr.Zero)
        {
            return null;
        }

        GetWindowThreadProcessId(window, out var processId);
        try
        {
            return Process.GetProcessById((int)processId).ProcessName;
        }
        catch (ArgumentException)
        {
            return null;
        }
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AddClipboardFormatListener(IntPtr hwnd);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RemoveClipboardFormatListener(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
}
