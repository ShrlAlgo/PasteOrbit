using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.UI.Dispatching;
using PasteOrbit.Core;
using Windows.ApplicationModel.DataTransfer;
using Windows.Graphics.Imaging;
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
    private const uint ThumbnailMaxWidth = 320;
    private const uint ThumbnailMaxHeight = 180;
    private Win32MessageBridge? _bridge;
    private DispatcherQueue? _dispatcherQueue;
    private Timer? _pollTimer;
    private uint _clipboardSequence;
    private int _capturePending;
    private int _captureSuspended;
    private uint _retrySequence;
    private long _retryAfter;
    private int _retryDelayMilliseconds;
    private bool _captureFiltered;

    public Func<ClipboardContentKind, bool>? IsKindEnabled { get; set; }
    public Func<string?, bool>? IsSourceExcluded { get; set; }

    public event Action<ClipboardCapture>? Captured;

    public event Action<Exception>? CaptureFailed;

    public void SuspendCapture()
    {
        // 使用计数而不是布尔值，允许粘贴和文件保存流程嵌套暂停监听。
        Interlocked.Increment(ref _captureSuspended);
    }

    public void ResumeCapture(bool capturePending = false)
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
                var sequence = GetClipboardSequenceNumber();
                // 粘贴期间只跳过本程序写回的内容，用户新复制的内容需要补采集。
                if (!capturePending || sequence == ClipboardPlayback.LastWrittenSequence)
                {
                    Volatile.Write(ref _clipboardSequence, sequence);
                }
                else
                {
                    RequestCapture();
                }
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
        uint sequence = 0;
        try
        {
            while (_bridge is not null && Volatile.Read(ref _captureSuspended) == 0)
            {
                sequence = GetClipboardSequenceNumber();
                // 同一版本失败后逐步退避，新复制的版本不受旧重试期限影响。
                if (sequence == _retrySequence && Environment.TickCount64 < _retryAfter)
                {
                    checkForNewerContent = false;
                    return;
                }
                if (sequence == 0 || sequence == Volatile.Read(ref _clipboardSequence))
                {
                    return;
                }

                ClipboardCapture? capture = null;
                for (var attempt = 0; ; attempt++)
                {
                    try
                    {
                        _captureFiltered = false;
                        capture = await ReadClipboardAsync();
                        if (_captureFiltered || capture is not null || attempt >= RetryCount - 1)
                        {
                            break;
                        }

                        // 延迟提供的文本或截图可能暂时没有可读内容，空结果也需要重试。
                        await Task.Delay(40 * (attempt + 1));
                    }
                    catch (Exception exception) when (
                        exception is COMException or InvalidOperationException or IOException
                        && attempt < RetryCount - 1)
                    {
                        // 剪切板可能被来源程序短暂占用，有限重试避免阻塞 UI 线程。
                        await Task.Delay(40 * (attempt + 1));
                    }
                }

                if (_bridge is null || Volatile.Read(ref _captureSuspended) != 0)
                {
                    return;
                }

                if (_captureFiltered)
                {
                    // 用户明确排除的内容不需要重试，后续剪贴板变化仍照常处理。
                    Volatile.Write(ref _clipboardSequence, sequence);
                    continue;
                }

                if (capture is null)
                {
                    DelayCaptureRetry(sequence);
                    // 不消费空结果的序列号，由轮询继续检查，避免立即递归重试。
                    checkForNewerContent = false;
                    return;
                }

                // 只在成功读取后提交序列号，读取期间的新变化会在下一轮继续捕获。
                Captured?.Invoke(capture);
                Volatile.Write(ref _clipboardSequence, sequence);
                _retryDelayMilliseconds = 0;
                _retrySequence = 0;
            }
        }
        catch (Exception exception)
        {
            checkForNewerContent = false;
            DelayCaptureRetry(sequence);
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

    private void DelayCaptureRetry(uint sequence)
    {
        // 保留重试以兼容延迟渲染，持续不可读时最多每 5 秒尝试一次。
        _retryDelayMilliseconds = sequence == _retrySequence
            ? Math.Min(5000, Math.Max(500, _retryDelayMilliseconds * 2))
            : 500;
        _retrySequence = sequence;
        _retryAfter = Environment.TickCount64 + _retryDelayMilliseconds;
    }

    private async Task<ClipboardCapture?> ReadClipboardAsync()
    {
        // 截图工具可能同时提供位图和临时文件；优先保存位图，文件列表仅作后备。
        var sourceApplication = GetSourceProcessName();
        if (IsSourceExcluded?.Invoke(sourceApplication) == true)
        {
            _captureFiltered = true;
            return null;
        }
        var hasBitmap = false;
        try
        {
            var data = Clipboard.GetContent();

            hasBitmap = data.Contains(StandardDataFormats.Bitmap);
            if (hasBitmap)
            {
                if (SkipDisabledKind(ClipboardContentKind.Image)) return null;
                var bitmapReference = await data.GetBitmapAsync();
                using var stream = await bitmapReference.OpenReadAsync();
                byte[]? thumbnail = null;
                try
                {
                    thumbnail = await CreateThumbnailAsync(stream);
                }
                catch (Exception exception) when (exception is COMException or ArgumentException)
                {
                    // 损坏或不受支持的位图仍保存原图，卡片仅缺少缩略图。
                }

                var content = await ReadBytesAsync(stream);
                if (content.Length > 0)
                {
                    return new ClipboardCapture(
                        ClipboardContentKind.Image,
                        AppLocalization.GetString("ImageContent"),
                        content,
                        sourceApplication,
                        thumbnail);
                }

                throw new IOException("剪贴板图片尚未就绪。");
            }

            if (data.Contains(StandardDataFormats.StorageItems))
            {
                if (SkipDisabledKind(ClipboardContentKind.Files)) return null;
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

            if (data.Contains(StandardDataFormats.Text))
            {
                if (SkipDisabledKind(ClipboardContentKind.Text)) return null;
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
            // 位图读取失败保留序列号，交给重试与轮询；不能降级成文本后标记成功。
            if (hasBitmap)
            {
                throw;
            }

            if (SkipDisabledKind(ClipboardContentKind.Text)) return null;
            var fallbackText = TryReadUnicodeText();
            if (string.IsNullOrEmpty(fallbackText))
            {
                throw;
            }

            return new ClipboardCapture(
                ClipboardContentKind.Text,
                fallbackText,
                new ClipboardTextContent(fallbackText, null, null).Serialize(),
                sourceApplication);
        }

        if (SkipDisabledKind(ClipboardContentKind.Text)) return null;
        var nativeText = TryReadUnicodeText();
        return string.IsNullOrEmpty(nativeText)
            ? null
            : new ClipboardCapture(
                ClipboardContentKind.Text,
                nativeText,
                new ClipboardTextContent(nativeText, null, null).Serialize(),
                sourceApplication);
    }

    private bool SkipDisabledKind(ClipboardContentKind kind)
    {
        // 在读取完整内容和生成缩略图之前应用类型设置。
        _captureFiltered = IsKindEnabled?.Invoke(kind) == false;
        return _captureFiltered;
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

    private static async Task<byte[]> CreateThumbnailAsync(IRandomAccessStream source)
    {
        source.Seek(0);
        var decoder = await BitmapDecoder.CreateAsync(source);
        var scale = Math.Min(
            1d,
            Math.Min(
                ThumbnailMaxWidth / (double)Math.Max(1u, decoder.PixelWidth),
                ThumbnailMaxHeight / (double)Math.Max(1u, decoder.PixelHeight)));
        var transform = new BitmapTransform
        {
            ScaledWidth = Math.Max(1u, (uint)Math.Round(decoder.PixelWidth * scale)),
            ScaledHeight = Math.Max(1u, (uint)Math.Round(decoder.PixelHeight * scale))
        };
        using var bitmap = await decoder.GetSoftwareBitmapAsync(
            BitmapPixelFormat.Bgra8,
            BitmapAlphaMode.Premultiplied,
            transform,
            ExifOrientationMode.RespectExifOrientation,
            ColorManagementMode.ColorManageToSRgb);
        using var thumbnailStream = new InMemoryRandomAccessStream();
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, thumbnailStream);
        encoder.SetSoftwareBitmap(bitmap);
        await encoder.FlushAsync();
        return await ReadBytesAsync(thumbnailStream);
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
