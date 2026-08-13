using System.Security.Cryptography;

namespace PasteOrbit.Core;

public sealed record ClipboardCapture(
    ClipboardContentKind Kind,
    string SearchText,
    byte[] Content,
    string? SourceApplication);

public sealed class ClipboardHistory
{
    private readonly List<ClipboardHistoryEntry> _items = [];
    private readonly Dictionary<Guid, PinyinSearchTerms?> _pinyinSearchCache = [];
    private readonly object _syncRoot = new();

    public ClipboardHistory(IEnumerable<ClipboardHistoryEntry>? items = null)
    {
        if (items is not null)
        {
            _items.AddRange(items.OrderByDescending(item => item.UpdatedAt));
        }
    }

    public int Count
    {
        get
        {
            lock (_syncRoot)
            {
                return _items.Count;
            }
        }
    }

    public IReadOnlyList<ClipboardHistoryEntry> GetSnapshot()
    {
        lock (_syncRoot)
        {
            return OrderItems(_items);
        }
    }

    public IReadOnlyList<ClipboardHistoryEntry> Search(string? query, ClipboardContentKind? kind = null)
    {
        var term = query?.Trim();
        lock (_syncRoot)
        {
            var matches = _items.Where(item => kind is null || item.Kind == kind);
            if (!string.IsNullOrEmpty(term))
            {
                matches = matches.Where(item => MatchesSearch(item, term));
            }

            return OrderItems(matches);
        }
    }

    public ClipboardHistoryEntry AddOrUpdate(ClipboardCapture capture, DateTimeOffset? capturedAt = null)
    {
        ArgumentNullException.ThrowIfNull(capture);
        ArgumentOutOfRangeException.ThrowIfZero(capture.Content.Length);

        var now = capturedAt ?? DateTimeOffset.UtcNow;
        var contentHash = ComputeHash(capture.Kind, capture.Content);
        ClipboardHistoryEntry item;

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
                    SourceApplication = capture.SourceApplication,
                    UpdatedAt = now
                };
                _items.RemoveAt(existingIndex);
                _pinyinSearchCache.Remove(existing.Id);
            }
            else
            {
                item = new ClipboardHistoryEntry(
                    Guid.NewGuid(),
                    capture.Kind,
                    contentHash,
                    capture.SearchText,
                    capture.SourceApplication,
                    now,
                    now);
            }

            _items.Insert(0, item);
        }

        return item;
    }

    public IReadOnlyList<Guid> Cleanup(DateTimeOffset cutoff, int maxEntries)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(maxEntries);
        var removedIds = new List<Guid>();

        lock (_syncRoot)
        {
            var retainedCount = 0;
            _items.RemoveAll(item =>
            {
                if (item.IsPinned)
                {
                    return false;
                }

                var shouldRemove = item.UpdatedAt < cutoff || retainedCount >= maxEntries;
                if (shouldRemove)
                {
                    removedIds.Add(item.Id);
                    return true;
                }

                retainedCount++;
                return false;
            });

            foreach (var removedId in removedIds)
            {
                _pinyinSearchCache.Remove(removedId);
            }
        }

        return removedIds;
    }

    public ClipboardHistoryEntry? SetPinned(Guid id, bool isPinned)
    {
        ClipboardHistoryEntry? updated = null;
        lock (_syncRoot)
        {
            var index = _items.FindIndex(item => item.Id == id);
            if (index >= 0)
            {
                updated = _items[index] with { IsPinned = isPinned };
                _items[index] = updated;
            }
        }

        return updated;
    }

    public ClipboardHistoryEntry? SetOcrText(Guid id, string ocrText)
    {
        ArgumentNullException.ThrowIfNull(ocrText);
        ClipboardHistoryEntry? updated = null;
        lock (_syncRoot)
        {
            var index = _items.FindIndex(item => item.Id == id);
            if (index >= 0)
            {
                updated = _items[index] with { OcrText = ocrText };
                _items[index] = updated;
                // OCR 文字也参与普通搜索和拼音搜索，结果变化后必须丢弃旧索引。
                _pinyinSearchCache.Remove(id);
            }
        }

        return updated;
    }

    public bool Remove(Guid id)
    {
        bool removed;
        lock (_syncRoot)
        {
            removed = _items.RemoveAll(item => item.Id == id) > 0;
            if (removed)
            {
                _pinyinSearchCache.Remove(id);
            }
        }

        return removed;
    }

    public void ReplaceAll(IEnumerable<ClipboardHistoryEntry> items)
    {
        ArgumentNullException.ThrowIfNull(items);
        lock (_syncRoot)
        {
            _items.Clear();
            _items.AddRange(items.OrderByDescending(item => item.UpdatedAt));
            _pinyinSearchCache.Clear();
        }
    }

    private bool MatchesSearch(ClipboardHistoryEntry item, string term)
    {
        if (item.SearchText.Contains(term, StringComparison.CurrentCultureIgnoreCase)
            || item.OcrText?.Contains(term, StringComparison.CurrentCultureIgnoreCase) == true
            || item.SourceApplication?.Contains(term, StringComparison.CurrentCultureIgnoreCase) == true)
        {
            return true;
        }

        if (!_pinyinSearchCache.TryGetValue(item.Id, out var pinyinTerms))
        {
            var searchableText = string.IsNullOrEmpty(item.OcrText)
                ? item.SearchText
                : $"{item.SearchText}\n{item.OcrText}";
            pinyinTerms = PinyinSearchTerms.Create(searchableText);
            _pinyinSearchCache[item.Id] = pinyinTerms;
        }

        return pinyinTerms?.Matches(term) == true;
    }

    private static string ComputeHash(ClipboardContentKind kind, byte[] content)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData([(byte)kind]);
        hash.AppendData(content);
        return Convert.ToHexString(hash.GetHashAndReset());
    }

    private static ClipboardHistoryEntry[] OrderItems(IEnumerable<ClipboardHistoryEntry> items)
    {
        var pinnedItems = new List<ClipboardHistoryEntry>();
        var regularItems = new List<ClipboardHistoryEntry>();
        foreach (var item in items)
        {
            (item.IsPinned ? pinnedItems : regularItems).Add(item);
        }

        return [.. pinnedItems, .. regularItems];
    }
}
