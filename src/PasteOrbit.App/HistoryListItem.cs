using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

using PasteOrbit.Core;

namespace PasteOrbit.App;

public sealed class HistoryListItem : INotifyPropertyChanged
{
    private bool _isPinned;
    private ImageSource? _thumbnail;

    public HistoryListItem(ClipboardHistoryEntry entry)
    {
        Entry = entry;
        _isPinned = entry.IsPinned;
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public ClipboardHistoryEntry Entry { get; private set; }
    public Guid Id => Entry.Id;
    public string PreviewText => Entry.Kind == ClipboardContentKind.Image ? string.Empty : Entry.PreviewText;
    public string SourceApplication => string.IsNullOrWhiteSpace(Entry.SourceApplication) ? "未知应用" : Entry.SourceApplication;
    public string TimeText => Entry.UpdatedAt.ToLocalTime().ToString("HH:mm");
    public Visibility ImageVisibility => Entry.Kind == ClipboardContentKind.Image ? Visibility.Visible : Visibility.Collapsed;
    public string PinGlyph => IsPinned ? "\uE77A" : "\uE718";
    public System.Windows.Media.Brush PinBrush => IsPinned
        ? (System.Windows.Media.Brush)System.Windows.Application.Current.Resources["AccentFillColorDefaultBrush"]
        : (System.Windows.Media.Brush)System.Windows.Application.Current.Resources["TextFillColorSecondaryBrush"];
    public ImageSource? Thumbnail { get => _thumbnail; private set => SetField(ref _thumbnail, value); }
    public string DetailText => Entry.Kind switch
    {
        ClipboardContentKind.Text => $"{Entry.SearchTextLength} 个字符",
        ClipboardContentKind.Files => Entry.PreviewText.Contains('\n') ? $"{Entry.PreviewText.Count(character => character == '\n') + 1} 个文件" : "1 个文件",
        ClipboardContentKind.Image => FormatSize(Entry.ContentSize),
        _ => string.Empty
    };

    public bool IsPinned
    {
        get => _isPinned;
        set
        {
            if (!SetField(ref _isPinned, value))
            {
                return;
            }

            Entry = Entry with { IsPinned = value };
            OnPropertyChanged(nameof(PinGlyph));
            OnPropertyChanged(nameof(PinBrush));
        }
    }

    public void LoadThumbnail(byte[] content)
    {
        if (Entry.Kind != ClipboardContentKind.Image || Thumbnail is not null)
        {
            return;
        }

        using var stream = new MemoryStream(content, writable: false);
        var bitmap = new BitmapImage();
        bitmap.BeginInit();
        bitmap.CacheOption = BitmapCacheOption.OnLoad;
        bitmap.DecodePixelWidth = 360;
        bitmap.StreamSource = stream;
        bitmap.EndInit();
        bitmap.Freeze();
        Thumbnail = bitmap;
    }

    private static string FormatSize(long bytes)
    {
        return bytes >= 1024 * 1024 ? $"{bytes / 1024d / 1024d:F1} MB" : $"{Math.Max(1, bytes / 1024d):F0} KB";
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
