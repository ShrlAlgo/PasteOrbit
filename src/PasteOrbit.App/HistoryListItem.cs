using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media.Imaging;
using PasteOrbit.Core;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;

namespace PasteOrbit.App;

/// <summary>
/// 历史记录卡片的数据适配器，负责延迟加载预览和通知界面刷新。
/// </summary>
public sealed class HistoryListItem : INotifyPropertyChanged, IDisposable
{
    private static readonly SemaphoreSlim ThumbnailLoadGate = new(1, 1);

    private readonly DispatcherQueue _dispatcherQueue;
    private Task? _thumbnailLoadTask;
    private CancellationTokenSource? _thumbnailLoadCancellation;
    private BitmapImage? _thumbnail;
    private string _formatLabel = string.Empty;
    private string _quickPasteLabel = string.Empty;
    private string _metadata;
    private bool _thumbnailRequested;
    private bool _disposed;

    private HistoryListItem(ClipboardHistoryEntry item)
    {
        _dispatcherQueue = DispatcherQueue.GetForCurrentThread()
            ?? throw new InvalidOperationException(AppLocalization.GetString("HistoryDispatcherUnavailable"));
        Item = item;
        Preview = item.PreviewText;
        _metadata = item.Kind switch
        {
            ClipboardContentKind.Text => AppLocalization.Format("CharacterCount", item.SearchTextLength),
            ClipboardContentKind.Image => CreateImageMetadata(item),
            ClipboardContentKind.Files => AppLocalization.GetString("ContentTypeFiles"),
            _ => string.Empty
        };
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public ClipboardHistoryEntry Item { get; }

    public string Preview { get; }

    public string Metadata
    {
        get => _metadata;
        private set
        {
            if (_metadata == value)
            {
                return;
            }

            _metadata = value;
            OnPropertyChanged();
        }
    }

    public string SourceLabel => Item.SourceApplication ?? AppLocalization.GetString("UnknownApplication");

    public string TimeLabel => Item.UpdatedAt.ToLocalTime().ToString("HH:mm");

    public bool IsPinned => Item.IsPinned;

    public Visibility PinnedVisibility => IsPinned ? Visibility.Visible : Visibility.Collapsed;

    public string PinToolTip => IsPinned
        ? AppLocalization.GetString("UnpinItem")
        : AppLocalization.GetString("PinItem");

    public string PinGlyph => IsPinned ? "\uE77A" : "\uE718";

    public Visibility PasteAsFileVisibility => Item.Kind is ClipboardContentKind.Text or ClipboardContentKind.Image
        ? Visibility.Visible
        : Visibility.Collapsed;

    public Visibility PlainTextPasteVisibility => Item.Kind == ClipboardContentKind.Text
        ? Visibility.Visible
        : Visibility.Collapsed;

    public BitmapImage? Thumbnail
    {
        get => _thumbnail;
        private set
        {
            if (ReferenceEquals(_thumbnail, value))
            {
                return;
            }

            _thumbnail = value;
            OnPropertyChanged();
        }
    }

    public Visibility TextContentVisibility => Item.Kind == ClipboardContentKind.Image
        ? Visibility.Collapsed
        : Visibility.Visible;

    public string FormatLabel => _formatLabel;

    public Visibility FormatBadgeVisibility => string.IsNullOrEmpty(_formatLabel)
        ? Visibility.Collapsed
        : Visibility.Visible;

    public string QuickPasteLabel => _quickPasteLabel;

    public Visibility QuickPasteVisibility => string.IsNullOrEmpty(_quickPasteLabel)
        ? Visibility.Collapsed
        : Visibility.Visible;

    public static HistoryListItem From(ClipboardHistoryEntry item)
    {
        ArgumentNullException.ThrowIfNull(item);
        return new HistoryListItem(item);
    }

    public void SetQuickPasteIndex(int? index)
    {
        var label = index is null ? string.Empty : (index.Value + 1).ToString();
        if (_quickPasteLabel == label)
        {
            return;
        }

        _quickPasteLabel = label;
        OnPropertyChanged(nameof(QuickPasteLabel));
        OnPropertyChanged(nameof(QuickPasteVisibility));
    }

    internal void UpdateTextFormatMetadata(ClipboardTextContent content)
    {
        ArgumentNullException.ThrowIfNull(content);
        if (_disposed || Item.Kind != ClipboardContentKind.Text)
        {
            return;
        }

        var hasHtmlFormatting = HasMeaningfulHtmlFormatting(content.Html);
        _formatLabel = (hasHtmlFormatting, content.Rtf is not null) switch
        {
            (true, true) => "HTML · RTF",
            (true, false) => "HTML",
            (false, true) => "RTF",
            _ => string.Empty
        };
        OnPropertyChanged(nameof(FormatLabel));
        OnPropertyChanged(nameof(FormatBadgeVisibility));
    }

    public Task EnsureThumbnailLoadedAsync(Func<Guid, byte[]?> loadThumbnail)
    {
        ArgumentNullException.ThrowIfNull(loadThumbnail);
        if (_disposed || Item.Kind != ClipboardContentKind.Image)
        {
            return Task.CompletedTask;
        }

        _thumbnailRequested = true;
        if (Thumbnail is not null)
        {
            return Task.CompletedTask;
        }

        // 容器复用或快速滚动时，避免同一记录同时读取多次。
        if (_thumbnailLoadTask is null)
        {
            var cancellation = new CancellationTokenSource();
            _thumbnailLoadCancellation = cancellation;
            _thumbnailLoadTask = LoadThumbnailAsync(loadThumbnail, cancellation);
        }

        return _thumbnailLoadTask;
    }

    public void UnloadThumbnail()
    {
        _thumbnailRequested = false;
        _thumbnailLoadCancellation?.Cancel();
        _thumbnailLoadCancellation = null;
        _thumbnailLoadTask = null;
        Thumbnail = null;
    }

    public void Dispose()
    {
        _disposed = true;
        UnloadThumbnail();
    }

    private async Task LoadThumbnailAsync(
        Func<Guid, byte[]?> loadThumbnail,
        CancellationTokenSource cancellation)
    {
        var cancellationToken = cancellation.Token;
        try
        {
            // 图片解码会产生与原图尺寸相关的临时位图，串行处理避免多张大图同时占用内存。
            await ThumbnailLoadGate.WaitAsync(cancellationToken);
            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (_disposed || !_thumbnailRequested)
                {
                    return;
                }

                var content = await Task.Run(() => loadThumbnail(Item.Id), cancellationToken);
                cancellationToken.ThrowIfCancellationRequested();
                if (_disposed || !_thumbnailRequested || content is null || content.Length == 0)
                {
                    return;
                }

                await RunOnUiThreadAsync(() => LoadImagePreviewAsync(content, cancellationToken));
            }
            finally
            {
                ThumbnailLoadGate.Release();
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception)
        {
            await RunOnUiThreadAsync(() =>
            {
                if (!_disposed)
                {
                    Metadata = Item.Kind switch
                    {
                        ClipboardContentKind.Image => AppLocalization.GetString("ImagePreviewUnavailable"),
                        _ => Metadata
                    };
                }

                return Task.CompletedTask;
            });
        }
        finally
        {
            if (ReferenceEquals(_thumbnailLoadCancellation, cancellation))
            {
                _thumbnailLoadCancellation = null;
                _thumbnailLoadTask = null;
            }

            cancellation.Dispose();
        }
    }

    // CF_HTML 常会附带在普通文本后，只在存在实际富格式标记时展示 HTML 标签。
    private static bool HasMeaningfulHtmlFormatting(string? html)
    {
        if (string.IsNullOrWhiteSpace(html))
        {
            return false;
        }

        ReadOnlySpan<string> richMarkers =
        [
            "<a ", "<b>", "<b ", "<strong", "<i>", "<i ", "<em", "<u>", "<u ",
            "<s>", "<s ", "<table", "<img", "<ul", "<ol", "<li", "<h1", "<h2", "<h3",
            " style=", " class="
        ];
        foreach (var marker in richMarkers)
        {
            if (html.Contains(marker, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }

        return false;
    }

    private async Task LoadImagePreviewAsync(byte[] content, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_disposed)
        {
            return;
        }

        if (!_thumbnailRequested)
        {
            return;
        }

        using var thumbnailStream = new MemoryStream(content, writable: false).AsRandomAccessStream();

        cancellationToken.ThrowIfCancellationRequested();
        thumbnailStream.Seek(0);
        var decoder = await BitmapDecoder.CreateAsync(thumbnailStream);
        var decodeScale = Math.Min(
            600d / Math.Max(1u, decoder.PixelWidth),
            112d / Math.Max(1u, decoder.PixelHeight));
        var thumbnail = new BitmapImage();
        if (decodeScale < 1d)
        {
            // 按卡片实际尺寸的 2 倍解码，同时限制长图和宽图的像素驻留。
            thumbnail.DecodePixelWidth = Math.Max(
                1,
                (int)Math.Round(decoder.PixelWidth * decodeScale));
        }

        thumbnailStream.Seek(0);
        await thumbnail.SetSourceAsync(thumbnailStream);
        cancellationToken.ThrowIfCancellationRequested();
        if (_disposed || !_thumbnailRequested)
        {
            return;
        }

        Thumbnail = thumbnail;
    }

    private static string CreateImageMetadata(ClipboardHistoryEntry item)
    {
        var size = $"{Math.Max(1, item.ContentSize / 1024d):0.#} KB";
        return size;
    }

    private Task RunOnUiThreadAsync(Func<Task> action)
    {
        if (_dispatcherQueue.HasThreadAccess)
        {
            return action();
        }

        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_dispatcherQueue.TryEnqueue(async () =>
            {
                try
                {
                    await action();
                    completion.SetResult();
                }
                catch (Exception exception)
                {
                    completion.SetException(exception);
                }
            }))
        {
            completion.SetException(new InvalidOperationException(AppLocalization.GetString("PreviewDispatchUnavailable")));
        }

        return completion.Task;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
