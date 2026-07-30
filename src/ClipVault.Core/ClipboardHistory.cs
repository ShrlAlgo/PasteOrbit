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

    public ClipboardHistory(IEnumerable<ClipboardItem>? items = null)
    {
        if (items is not null)
        {
            _items.AddRange(items.OrderByDescending(item => item.UpdatedAt));
        }
    }

    public event EventHandler? Changed;

    public IReadOnlyList<ClipboardItem> GetSnapshot()
    {
        lock (_syncRoot)
        {
            return OrderItems(_items).ToArray();
        }
    }

    public IReadOnlyList<ClipboardItem> Search(string? query, ClipboardContentKind? kind = null)
    {
        var term = query?.Trim();
        lock (_syncRoot)
        {
            var matches = _items.Where(item => kind is null || item.Kind == kind);
            if (!string.IsNullOrEmpty(term))
            {
                matches = matches.Where(item =>
                    item.SearchText.Contains(term, StringComparison.CurrentCultureIgnoreCase)
                    || item.SourceApplication?.Contains(term, StringComparison.CurrentCultureIgnoreCase) == true);
            }

            return OrderItems(matches).ToArray();
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

    public IReadOnlyList<Guid> Cleanup(DateTimeOffset cutoff, int maxEntries)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(maxEntries);
        List<Guid> removedIds;

        lock (_syncRoot)
        {
            var expired = _items
                .Where(item => !item.IsPinned && item.UpdatedAt < cutoff)
                .Select(item => item.Id)
                .ToHashSet();
            var retained = _items
                .Where(item => !item.IsPinned && !expired.Contains(item.Id))
                .OrderByDescending(item => item.UpdatedAt)
                .Skip(maxEntries)
                .Select(item => item.Id);

            expired.UnionWith(retained);
            removedIds = expired.ToList();
            _items.RemoveAll(item => expired.Contains(item.Id));
        }

        if (removedIds.Count > 0)
        {
            Changed?.Invoke(this, EventArgs.Empty);
        }

        return removedIds;
    }

    public ClipboardItem? SetPinned(Guid id, bool isPinned)
    {
        ClipboardItem? updated = null;
        lock (_syncRoot)
        {
            var index = _items.FindIndex(item => item.Id == id);
            if (index >= 0)
            {
                updated = _items[index] with { IsPinned = isPinned };
                _items[index] = updated;
            }
        }

        if (updated is not null)
        {
            Changed?.Invoke(this, EventArgs.Empty);
        }

        return updated;
    }

    public bool Remove(Guid id)
    {
        bool removed;
        lock (_syncRoot)
        {
            removed = _items.RemoveAll(item => item.Id == id) > 0;
        }

        if (removed)
        {
            Changed?.Invoke(this, EventArgs.Empty);
        }

        return removed;
    }

    private static string ComputeHash(ClipboardContentKind kind, byte[] content)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData([(byte)kind]);
        hash.AppendData(content);
        return Convert.ToHexString(hash.GetHashAndReset());
    }

    private static IOrderedEnumerable<ClipboardItem> OrderItems(IEnumerable<ClipboardItem> items)
    {
        return items
            .OrderByDescending(item => item.IsPinned)
            .ThenByDescending(item => item.UpdatedAt);
    }
}
