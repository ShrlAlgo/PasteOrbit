using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.UI.Dispatching;
using PasteOrbit.Core;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Streams;

namespace PasteOrbit.App;

/// <summary>
/// 监听 Windows 剪贴板变化，并将 WinRT 数据转换为应用内部记录。
/// </summary>
public sealed class ClipboardMonitor : IDisposable
{
    private const uint WmClipboardUpdate = 0x031D;
    private const uint CfUnicodeText = 13;
    private const int RetryCount = 3;
    private Win32MessageBridge? _bridge;
    private DispatcherQueue? _dispatcherQueue;
    private Timer? _pollTimer;
    private uint _clipboardSequence;
    private int _capturePending;
    private int _captureSuspended;

    public event Action<ClipboardCapture>? Captured;

    public event Action<Exception>? CaptureFailed;

    public void SuspendCapture()
    {
        // 使用计数而不是布尔值，允许粘贴和文件保存流程嵌套暂停监听。
        Interlocked.Increment(ref _captureSuspended);
    }

    public void ResumeCapture()
    {
        while (true)
        {
            var suspensionCount = Volatile.Read(ref _captureSuspended);
            if (suspensionCount <= 0)
            {
                return;
            }

            if (Interlocked.CompareExchange(ref _captureSuspended, suspensionCount - 1, suspensionCount) != suspensionCount)
            {
                continue;
            }

            if (suspensionCount == 1)
            {
                Volatile.Write(ref _clipboardSequence, GetClipboardSequenceNumber());
            }

            return;
        }
    }

    public void Start(Win32MessageBridge bridge, DispatcherQueue dispatcherQueue)
    {
        ArgumentNullException.ThrowIfNull(bridge);
        ArgumentNullException.ThrowIfNull(dispatcherQueue);
        if (_bridge is not null)
        {
            throw new InvalidOperationException(AppLocalization.GetString("ClipboardMonitorAlreadyStarted"));
        }

        if (!AddClipboardFormatListener(bridge.Handle))
        {
            throw new Win32Exception(
                Marshal.GetLastWin32Error(),
                AppLocalization.GetString("ClipboardMonitorRegistrationFailed"));
        }

        _bridge = bridge;
        _dispatcherQueue = dispatcherQueue;
        _clipboardSequence = GetClipboardSequenceNumber();
        _bridge.Message += Bridge_Message;
        // 某些 WinUI 3 窗口在隐藏/重新激活后可能漏收 WM_CLIPBOARDUPDATE，序列号轮询用于兜底。
        _pollTimer = new Timer(PollClipboard, null, 250, 250);
    }

    public void Dispose()
    {
        if (_bridge is null)
        {
            return;
        }

        _bridge.Message -= Bridge_Message;
        RemoveClipboardFormatListener(_bridge.Handle);
        _pollTimer?.Dispose();
        _pollTimer = null;
        _dispatcherQueue = null;
        _bridge = null;
    }

    private void Bridge_Message(uint message, IntPtr wParam, IntPtr lParam)
    {
        if (message == WmClipboardUpdate)
        {
            RequestCapture();
        }
    }

    private void PollClipboard(object? state)
    {
        if (Volatile.Read(ref _captureSuspended) != 0)
        {
            return;
        }

        var sequence = GetClipboardSequenceNumber();
        if (sequence == 0 || sequence == Volatile.Read(ref _clipboardSequence))
        {
            return;
        }

        RequestCapture();
    }

    private void RequestCapture()
    {
        // 同一时间只允许一个捕获任务，任务结束后再检查是否有更新遗漏。
        if (Volatile.Read(ref _captureSuspended) != 0
            || Interlocked.Exchange(ref _capturePending, 1) != 0)
        {
            return;
        }

        if (_dispatcherQueue?.HasThreadAccess == true)
        {
            _ = CaptureWithRetryAsync();
            return;
        }

        var enqueued = _dispatcherQueue?.TryEnqueue(() => _ = CaptureWithRetryAsync()) == true;
        if (!enqueued)
        {
            Interlocked.Exchange(ref _capturePending, 0);
        }
    }

    private async Task CaptureWithRetryAsync()
    {
        // 以序列号为边界读取剪贴板，避免读取期间的新内容被旧序列号覆盖。
        var checkForNewerContent = true;
        try
        {
            while (Volatile.Read(ref _captureSuspended) == 0)
            {
                var sequence = GetClipboardSequenceNumber();
                if (sequence == 0 || sequence == Volatile.Read(ref _clipboardSequence))
                {
                    return;
                }

                ClipboardCapture? capture = null;
                for (var attempt = 0; ; attempt++)
                {
                    try
                    {
                        capture = await ReadClipboardAsync();
                        break;
                    }
                    catch (COMException) when (attempt < RetryCount - 1)
                    {
                        // 剪切板可能被来源程序短暂占用，有限重试避免阻塞 UI 线程。
                        await Task.Delay(40 * (attempt + 1));
                    }
                }

                if (Volatile.Read(ref _captureSuspended) != 0)
                {
                    return;
                }

                // 只在读取完成后提交序列号；读取期间发生的新变化会在下一轮继续捕获。
                Volatile.Write(ref _clipboardSequence, sequence);
                if (capture is not null)
                {
                    Captured?.Invoke(capture);
                }
            }
        }
        catch (Exception exception)
        {
            checkForNewerContent = false;
            CaptureFailed?.Invoke(exception);
        }
        finally
        {
            Interlocked.Exchange(ref _capturePending, 0);
            var currentSequence = GetClipboardSequenceNumber();
            if (checkForNewerContent
                && Volatile.Read(ref _captureSuspended) == 0
                && currentSequence != 0
                && currentSequence != Volatile.Read(ref _clipboardSequence))
            {
                RequestCapture();
            }
        }
    }

    private static async Task<ClipboardCapture?> ReadClipboardAsync()
    {
        // 按文件、图片、富文本、纯文本的优先级读取，保留原始格式信息。
        var sourceApplication = GetSourceProcessName();
        try
        {
            var data = Clipboard.GetContent();

            if (data.Contains(StandardDataFormats.StorageItems))
            {
                var items = await data.GetStorageItemsAsync();
                var paths = items.Select(item => item.Path).Where(path => !string.IsNullOrWhiteSpace(path)).ToArray();
                if (paths.Length > 0)
                {
                    return new ClipboardCapture(
                        ClipboardContentKind.Files,
                        string.Join(Environment.NewLine, paths.Select(Path.GetFileName)),
                        JsonSerializer.SerializeToUtf8Bytes(paths),
                        sourceApplication);
                }
            }

            if (data.Contains(StandardDataFormats.Bitmap))
            {
                var bitmapReference = await data.GetBitmapAsync();
                using var stream = await bitmapReference.OpenReadAsync();
                var content = await ReadBytesAsync(stream);
                if (content.Length > 0)
                {
                    return new ClipboardCapture(
                        ClipboardContentKind.Image,
                        AppLocalization.GetString("ImageContent"),
                        content,
                        sourceApplication);
                }
            }

            if (data.Contains(StandardDataFormats.Text))
            {
                var text = await data.GetTextAsync();
                if (!string.IsNullOrEmpty(text))
                {
                    var html = data.Contains(StandardDataFormats.Html)
                        ? await data.GetHtmlFormatAsync()
                        : null;
                    var rtf = data.Contains(StandardDataFormats.Rtf)
                        ? await data.GetRtfAsync()
                        : null;
                    return new ClipboardCapture(
                        ClipboardContentKind.Text,
                        text,
                        new ClipboardTextContent(text, html, rtf).Serialize(),
                        sourceApplication);
                }
            }
        }
        catch (Exception exception) when (exception is COMException or InvalidOperationException)
        {
            // 非打包 WinUI 桌面进程偶尔无法读取 WinRT DataPackage，继续尝试原生 Unicode 文本格式。
        }

        var nativeText = TryReadUnicodeText();
        return string.IsNullOrEmpty(nativeText)
            ? null
            : new ClipboardCapture(
                ClipboardContentKind.Text,
                nativeText,
                new ClipboardTextContent(nativeText, null, null).Serialize(),
                sourceApplication);
    }

    private static string? TryReadUnicodeText()
    {
        if (!IsClipboardFormatAvailable(CfUnicodeText) || !OpenClipboard(IntPtr.Zero))
        {
            return null;
        }

        try
        {
            var handle = GetClipboardData(CfUnicodeText);
            if (handle == IntPtr.Zero)
            {
                return null;
            }

            var pointer = GlobalLock(handle);
            if (pointer == IntPtr.Zero)
            {
                return null;
            }

            try
            {
                return Marshal.PtrToStringUni(pointer);
            }
            finally
            {
                GlobalUnlock(handle);
            }
        }
        finally
        {
            CloseClipboard();
        }

    }

    private static async Task<byte[]> ReadBytesAsync(IRandomAccessStream stream)
    {
        stream.Seek(0);
        var content = GC.AllocateUninitializedArray<byte>(checked((int)stream.Size));
        using var input = stream.AsStreamForRead();
        await input.ReadExactlyAsync(content.AsMemory());
        return content;
    }

    private static string? GetSourceProcessName()
    {
        return GetProcessName(GetClipboardOwner()) ?? GetProcessName(GetForegroundWindow());
    }

    private static string? GetProcessName(IntPtr window)
    {
        if (window == IntPtr.Zero)
        {
            return null;
        }

        GetWindowThreadProcessId(window, out var processId);
        try
        {
            using var process = Process.GetProcessById((int)processId);
            return process.ProcessName;
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
    private static extern IntPtr GetClipboardOwner();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

    [DllImport("user32.dll")]
    private static extern uint GetClipboardSequenceNumber();

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsClipboardFormatAvailable(uint format);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool OpenClipboard(IntPtr owner);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseClipboard();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetClipboardData(uint format);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GlobalLock(IntPtr handle);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalUnlock(IntPtr handle);
}
