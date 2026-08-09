using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using PasteOrbit.Core;
using Windows.Storage.Streams;

namespace PasteOrbit.App;

public sealed class HistoryListItem : INotifyPropertyChanged, IDisposable
{
    private IRandomAccessStream? _thumbnailStream;
    private readonly DispatcherQueue _dispatcherQueue;
    private Task? _previewLoadTask;
    private BitmapImage? _thumbnail;
    private string? _richTextContent;
    private string _formatLabel = string.Empty;
    private string _metadata;
    private bool _previewRequested;
    private bool _textMetadataLoaded;
    private bool _fileMetadataLoaded;
    private bool _disposed;

    private HistoryListItem(ClipboardHistoryEntry item)
    {
        _dispatcherQueue = DispatcherQueue.GetForCurrentThread()
            ?? throw new InvalidOperationException("无法获取历史列表的 UI 调度队列。");
        Item = item;
        Preview = item.SearchText.ReplaceLineEndings(" ");
        _metadata = item.Kind switch
        {
            ClipboardContentKind.Text => $"{item.SearchText.Length} 个字符",
            ClipboardContentKind.Image => "图片",
            ClipboardContentKind.Files => "文件",
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

    public string SourceLabel => Item.SourceApplication ?? "未知应用";

    public string TimeLabel => Item.UpdatedAt.ToLocalTime().ToString("HH:mm");

    public bool IsPinned => Item.IsPinned;

    public Visibility PinnedVisibility => IsPinned ? Visibility.Visible : Visibility.Collapsed;

    public string PinToolTip => IsPinned ? "取消置顶" : "置顶记录";

    public string PinGlyph => IsPinned ? "\uE77A" : "\uE718";

    public Visibility PasteAsFileVisibility => Item.Kind is ClipboardContentKind.Text or ClipboardContentKind.Image
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

    internal string? RichTextContent => _richTextContent;

    public static HistoryListItem From(ClipboardHistoryEntry item)
    {
        ArgumentNullException.ThrowIfNull(item);
        return new HistoryListItem(item);
    }

    public Task EnsurePreviewLoadedAsync(Func<Guid, byte[]> loadContent)
    {
        ArgumentNullException.ThrowIfNull(loadContent);
        _previewRequested = true;
        if (_disposed)
        {
            return Task.CompletedTask;
        }

        if (Item.Kind == ClipboardContentKind.Text && _textMetadataLoaded)
        {
            return Task.CompletedTask;
        }

        if (Item.Kind == ClipboardContentKind.Image && Thumbnail is not null)
        {
            return Task.CompletedTask;
        }

        if (Item.Kind == ClipboardContentKind.Files && _fileMetadataLoaded)
        {
            return Task.CompletedTask;
        }

        _previewLoadTask ??= LoadPreviewAsync(loadContent);
        return _previewLoadTask;
    }

    public void UnloadPreview()
    {
        _previewRequested = false;
        if (Item.Kind != ClipboardContentKind.Image)
        {
            return;
        }

        Thumbnail = null;
        _thumbnailStream?.Dispose();
        _thumbnailStream = null;
    }

    public void Dispose()
    {
        _disposed = true;
        UnloadPreview();
    }

    private async Task LoadPreviewAsync(Func<Guid, byte[]> loadContent)
    {
        try
        {
            if (Item.Kind == ClipboardContentKind.Text)
            {
                var textContent = await Task.Run(() => ClipboardTextContent.Deserialize(loadContent(Item.Id)));
                await RunOnUiThreadAsync(() =>
                {
                    if (!_disposed)
                    {
                        ApplyTextContent(textContent);
                    }

                    return Task.CompletedTask;
                });
                return;
            }

            if (Item.Kind == ClipboardContentKind.Files)
            {
                var fileMetadata = await Task.Run(() => CreateFilesMetadata(loadContent(Item.Id)));
                await RunOnUiThreadAsync(() =>
                {
                    if (!_disposed)
                    {
                        Metadata = fileMetadata;
                        _fileMetadataLoaded = true;
                    }

                    return Task.CompletedTask;
                });
                return;
            }

            var content = await Task.Run(() => loadContent(Item.Id));
            await RunOnUiThreadAsync(() => LoadImagePreviewAsync(content));
        }
        catch (Exception)
        {
            await RunOnUiThreadAsync(() =>
            {
                if (!_disposed)
                {
                    Metadata = Item.Kind switch
                    {
                        ClipboardContentKind.Image => "图片 · 预览不可用",
                        ClipboardContentKind.Files => "文件 · 信息不可用",
                        _ => Metadata
                    };
                }

                return Task.CompletedTask;
            });
        }
        finally
        {
            _previewLoadTask = null;
        }
    }

    private void ApplyTextContent(ClipboardTextContent content)
    {
        _richTextContent = content.Rtf;
        _formatLabel = (content.Html, content.Rtf) switch
        {
            (not null, not null) => "HTML · RTF",
            (not null, null) => "HTML",
            (null, not null) => "RTF",
            _ => string.Empty
        };
        _textMetadataLoaded = true;
        OnPropertyChanged(nameof(FormatLabel));
        OnPropertyChanged(nameof(FormatBadgeVisibility));
    }

    private async Task LoadImagePreviewAsync(byte[] content)
    {
        if (_disposed)
        {
            return;
        }

        Metadata = $"{Math.Max(1, content.Length / 1024d):0.#} KB";
        if (!_previewRequested)
        {
            return;
        }

        var thumbnailStream = new InMemoryRandomAccessStream();
        using (var output = thumbnailStream.GetOutputStreamAt(0))
        using (var writer = new DataWriter(output))
        {
            writer.WriteBytes(content);
            await writer.StoreAsync();
            await writer.FlushAsync();
        }

        thumbnailStream.Seek(0);
        var thumbnail = new BitmapImage { DecodePixelWidth = 420 };
        await thumbnail.SetSourceAsync(thumbnailStream);
        if (_disposed || !_previewRequested)
        {
            thumbnailStream.Dispose();
            return;
        }

        _thumbnailStream?.Dispose();
        _thumbnailStream = thumbnailStream;
        Thumbnail = thumbnail;
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
            completion.SetException(new InvalidOperationException("无法调度图片预览更新。"));
        }

        return completion.Task;
    }

    private static string CreateFilesMetadata(byte[] content)
    {
        var paths = JsonSerializer.Deserialize<string[]>(content) ?? [];
        var metadata = $"{paths.Length} 个文件";
        var missingCount = paths.Count(path => !File.Exists(path) && !Directory.Exists(path));
        return missingCount > 0 ? $"{metadata} · {missingCount} 项已失效" : metadata;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
