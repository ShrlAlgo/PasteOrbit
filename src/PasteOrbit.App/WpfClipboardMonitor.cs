using System.Collections.Specialized;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

using PasteOrbit.Core;

namespace PasteOrbit.App;

internal sealed class WpfClipboardMonitor : IDisposable
{
    private const uint WmClipboardUpdate = 0x031D;
    private readonly Dispatcher _dispatcher;
    private readonly AppSettings _settings;
    private Win32MessageBridge? _bridge;
    private int _suspended;

    public WpfClipboardMonitor(Dispatcher dispatcher, AppSettings settings)
    {
        _dispatcher = dispatcher;
        _settings = settings;
    }

    public event Action<ClipboardCapture>? Captured;

    public void Start(Win32MessageBridge bridge)
    {
        _bridge = bridge;
        _bridge.Message += OnMessage;
        if (!AddClipboardFormatListener(bridge.Handle))
        {
            throw new InvalidOperationException(AppLocalization.GetString("ClipboardMonitorRegistrationFailed"));
        }
    }

    public void SuspendCapture() => Interlocked.Increment(ref _suspended);

    public void ResumeCapture() => Interlocked.Decrement(ref _suspended);

    public void Dispose()
    {
        if (_bridge is null)
        {
            return;
        }

        RemoveClipboardFormatListener(_bridge.Handle);
        _bridge.Message -= OnMessage;
        _bridge = null;
    }

    private void OnMessage(uint message, IntPtr wParam, IntPtr lParam)
    {
        if (message == WmClipboardUpdate && Volatile.Read(ref _suspended) == 0)
        {
            _dispatcher.BeginInvoke(CaptureClipboard, DispatcherPriority.Background);
        }
    }

    private void CaptureClipboard()
    {
        try
        {
            var capture = ReadClipboard();
            if (capture is not null && !IsExcluded(capture.SourceApplication))
            {
                Captured?.Invoke(capture);
            }
        }
        catch (Exception exception) when (exception is COMException or InvalidOperationException or ExternalException)
        {
            Debug.WriteLine($"读取剪切板失败：{exception}");
        }
    }

    private ClipboardCapture? ReadClipboard()
    {
        var sourceApplication = GetSourceProcessName();
        if (_settings.MonitorFiles && Clipboard.ContainsFileDropList())
        {
            StringCollection files = Clipboard.GetFileDropList();
            var paths = files.Cast<string>().Where(path => !string.IsNullOrWhiteSpace(path)).ToArray();
            if (paths.Length > 0)
            {
                return new ClipboardCapture(ClipboardContentKind.Files, string.Join(Environment.NewLine, paths.Select(Path.GetFileName)),
                    JsonSerializer.SerializeToUtf8Bytes(paths), sourceApplication);
            }
        }

        if (_settings.MonitorImages && Clipboard.ContainsImage())
        {
            var image = Clipboard.GetImage();
            if (image is not null)
            {
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(image));
                using var stream = new MemoryStream();
                encoder.Save(stream);
                return new ClipboardCapture(ClipboardContentKind.Image, AppLocalization.GetString("ImageContent"), stream.ToArray(), sourceApplication);
            }
        }

        if (_settings.MonitorText && Clipboard.ContainsText())
        {
            var text = Clipboard.GetText(TextDataFormat.UnicodeText);
            if (!string.IsNullOrEmpty(text))
            {
                var html = Clipboard.ContainsText(TextDataFormat.Html) ? Clipboard.GetText(TextDataFormat.Html) : null;
                var rtf = Clipboard.ContainsText(TextDataFormat.Rtf) ? Clipboard.GetText(TextDataFormat.Rtf) : null;
                return new ClipboardCapture(ClipboardContentKind.Text, text, new ClipboardTextContent(text, html, rtf).Serialize(), sourceApplication);
            }
        }

        return null;
    }

    private bool IsExcluded(string? processName)
    {
        if (string.IsNullOrWhiteSpace(processName))
        {
            return false;
        }

        return _settings.ExcludedApplications.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Any(item => string.Equals(item, processName, StringComparison.OrdinalIgnoreCase));
    }

    private static string? GetSourceProcessName()
    {
        var owner = GetClipboardOwner();
        if (owner == IntPtr.Zero)
        {
            return null;
        }

        GetWindowThreadProcessId(owner, out var processId);
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
    private static extern bool AddClipboardFormatListener(IntPtr windowHandle);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RemoveClipboardFormatListener(IntPtr windowHandle);

    [DllImport("user32.dll")]
    private static extern IntPtr GetClipboardOwner();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr windowHandle, out uint processId);
}
