using System.Threading.Channels;
using Windows.Graphics.Imaging;
using Windows.Media.Ocr;
using Windows.Storage.Streams;

namespace PasteOrbit.App;

/// <summary>
/// 串行识别新复制的图片，避免多张大图同时解码造成瞬时内存峰值。
/// 队列中只保存记录 ID，图片数据在真正识别时才从加密数据库按需读取。
/// </summary>
internal sealed class ImageOcrService : IDisposable
{
    private readonly Func<Guid, byte[]> _loadContent;
    private readonly Channel<Guid> _queue = Channel.CreateUnbounded<Guid>(new UnboundedChannelOptions
    {
        SingleReader = true,
        SingleWriter = false
    });
    private readonly HashSet<Guid> _pendingIds = [];
    private readonly object _pendingSyncRoot = new();
    private readonly CancellationTokenSource _cancellation = new();
    private readonly Task _worker;
    private bool _disposed;

    public ImageOcrService(Func<Guid, byte[]> loadContent)
    {
        _loadContent = loadContent ?? throw new ArgumentNullException(nameof(loadContent));
        _worker = Task.Run(ProcessQueueAsync);
    }

    public event Action<Guid, string>? Recognized;

    public event Action<Exception>? RecognitionFailed;

    public void Enqueue(Guid id)
    {
        lock (_pendingSyncRoot)
        {
            if (_disposed || !_pendingIds.Add(id))
            {
                return;
            }
        }

        if (!_queue.Writer.TryWrite(id))
        {
            lock (_pendingSyncRoot)
            {
                _pendingIds.Remove(id);
            }
        }
    }

    public void Dispose()
    {
        lock (_pendingSyncRoot)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
        }

        _queue.Writer.TryComplete();
        _cancellation.Cancel();
        _cancellation.Dispose();
    }

    private async Task ProcessQueueAsync()
    {
        try
        {
            var engine = OcrEngine.TryCreateFromUserProfileLanguages();
            if (engine is null)
            {
                _queue.Writer.TryComplete();
                lock (_pendingSyncRoot)
                {
                    _pendingIds.Clear();
                }
                RecognitionFailed?.Invoke(new InvalidOperationException(
                    "Windows OCR language support is unavailable for the current user."));
                return;
            }

            await foreach (var id in _queue.Reader.ReadAllAsync(_cancellation.Token))
            {
                try
                {
                    var content = _loadContent(id);
                    var text = await RecognizeAsync(engine, content);
                    Recognized?.Invoke(id, text);
                }
                catch (KeyNotFoundException)
                {
                    // 用户可能在任务排队期间删除了记录。
                }
                catch (OperationCanceledException) when (_cancellation.IsCancellationRequested)
                {
                    return;
                }
                catch (Exception exception)
                {
                    RecognitionFailed?.Invoke(exception);
                }
                finally
                {
                    lock (_pendingSyncRoot)
                    {
                        _pendingIds.Remove(id);
                    }
                }
            }
        }
        catch (OperationCanceledException) when (_cancellation.IsCancellationRequested)
        {
        }
    }

    private static async Task<string> RecognizeAsync(OcrEngine engine, byte[] content)
    {
        using var stream = new InMemoryRandomAccessStream();
        using (var output = stream.GetOutputStreamAt(0))
        using (var writer = new DataWriter(output))
        {
            writer.WriteBytes(content);
            await writer.StoreAsync();
            await writer.FlushAsync();
        }

        stream.Seek(0);
        var decoder = await BitmapDecoder.CreateAsync(stream);
        var transform = CreateDecodeTransform(decoder, OcrEngine.MaxImageDimension);
        using var bitmap = await decoder.GetSoftwareBitmapAsync(
            BitmapPixelFormat.Bgra8,
            BitmapAlphaMode.Premultiplied,
            transform,
            ExifOrientationMode.RespectExifOrientation,
            ColorManagementMode.ColorManageToSRgb);
        var result = await engine.RecognizeAsync(bitmap);
        return BuildTextInVisualOrder(result);
    }

    private static BitmapTransform CreateDecodeTransform(BitmapDecoder decoder, uint maxDimension)
    {
        var transform = new BitmapTransform();
        var largestDimension = Math.Max(decoder.PixelWidth, decoder.PixelHeight);
        if (maxDimension == 0 || largestDimension <= maxDimension)
        {
            return transform;
        }

        var scale = maxDimension / (double)largestDimension;
        transform.ScaledWidth = Math.Max(1u, (uint)Math.Round(decoder.PixelWidth * scale));
        transform.ScaledHeight = Math.Max(1u, (uint)Math.Round(decoder.PixelHeight * scale));
        return transform;
    }

    private static string BuildTextInVisualOrder(OcrResult result)
    {
        var lines = result.Lines
            .Select(line => new
            {
                Line = line,
                Top = line.Words.Count > 0 ? line.Words.Min(word => word.BoundingRect.Y) : double.MaxValue,
                Left = line.Words.Count > 0 ? line.Words.Min(word => word.BoundingRect.X) : double.MaxValue
            })
            // Windows OCR 返回的是识别顺序；这里根据画面坐标固定为先上后下、同高度先左后右。
            .OrderBy(item => item.Top)
            .ThenBy(item => item.Left);
        var orderedText = string.Join(
            Environment.NewLine,
            lines.Select(item => item.Line.Text.Trim()).Where(text => text.Length > 0));
        if (orderedText.Length == 0)
        {
            // 空字符串代表“已识别但没有文字”，可避免重复复制同一图片时反复 OCR。
            return string.Empty;
        }

        return orderedText;
    }
}
