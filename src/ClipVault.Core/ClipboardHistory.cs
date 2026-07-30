using System.Security.Cryptography;

namespace ClipVault.Core;

public sealed record ClipboardCapture(
    ClipboardContentKind Kind,
    string SearchText,
    byte[] Content,
    string? SourceApplication);

public sealed class ClipboardHistory
{
    private readonly List<ClipboardItem> _items = [];
    private readonly object _syncRoot = new();

    public event EventHandler? Changed;

    public IReadOnlyList<ClipboardItem> GetSnapshot()
    {
        lock (_syncRoot)
        {
            return _items.ToArray();
        }
    }

    public ClipboardItem AddOrUpdate(ClipboardCapture capture, DateTimeOffset? capturedAt = null)
    {
        ArgumentNullException.ThrowIfNull(capture);
        ArgumentOutOfRangeException.ThrowIfZero(capture.Content.Length);

        var now = capturedAt ?? DateTimeOffset.UtcNow;
        var contentHash = ComputeHash(capture.Kind, capture.Content);
        ClipboardItem item;

        lock (_syncRoot)
        {
            var existingIndex = _items.FindIndex(candidate =>
                string.Equals(candidate.ContentHash, contentHash, StringComparison.Ordinal));

            if (existingIndex >= 0)
            {
                var existing = _items[existingIndex];
                item = existing with
                {
                    SearchText = capture.SearchText,
                    Content = capture.Content,
                    SourceApplication = capture.SourceApplication,
                    UpdatedAt = now
                };
                _items.RemoveAt(existingIndex);
            }
            else
            {
                item = new ClipboardItem(
                    Guid.NewGuid(),
                    capture.Kind,
                    contentHash,
                    capture.SearchText,
                    capture.Content,
                    capture.SourceApplication,
                    now,
                    now);
            }

            _items.Insert(0, item);
        }

        Changed?.Invoke(this, EventArgs.Empty);
        return item;
    }

    private static string ComputeHash(ClipboardContentKind kind, byte[] content)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData([(byte)kind]);
        hash.AppendData(content);
        return Convert.ToHexString(hash.GetHashAndReset());
    }
}
