using System.Security.Cryptography;

namespace PasteOrbit.Core;

/// <summary>
/// 一次剪贴板捕获的内容及其搜索元数据。
/// </summary>
public sealed record ClipboardCapture(
    ClipboardContentKind Kind,
    string SearchText,
    byte[] Content,
    string? SourceApplication);

/// <summary>
/// 管理剪贴板记录的内存集合，并提供线程安全的搜索、排序和生命周期操作。
/// </summary>
public sealed class ClipboardHistory
{
    private const int MaxPinyinCacheEntries = 256;

    private sealed record PinyinCacheEntry(
        PinyinSearchTerms? Terms,
        LinkedListNode<Guid> OrderNode);

    private readonly List<ClipboardHistoryEntry> _items = [];
    private readonly Dictionary<Guid, PinyinCacheEntry> _pinyinSearchCache = [];
    private readonly LinkedList<Guid> _pinyinCacheOrder = [];
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
            // 先按类型筛选，再执行文本和拼音匹配，避免无关记录建立索引。
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
            // 相同内容只更新元数据，保留原记录标识和置顶状态。
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
                RemovePinyinCache(existing.Id);
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
            // 置顶记录始终保留，普通记录同时受保留天数和数量上限约束。
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
                RemovePinyinCache(removedId);
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
                RemovePinyinCache(id);
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
                RemovePinyinCache(id);
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
            ClearPinyinCache();
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

        if (!_pinyinSearchCache.TryGetValue(item.Id, out var cacheEntry))
        {
            var searchableText = string.IsNullOrEmpty(item.OcrText)
                ? item.SearchText
                : $"{item.SearchText}\n{item.OcrText}";
            AddPinyinCache(item.Id, PinyinSearchTerms.Create(searchableText));
            cacheEntry = _pinyinSearchCache[item.Id];
        }
        else
        {
            // 搜索频繁访问的记录保留在缓存中，淘汰长期未命中的旧索引。
            TouchPinyinCache(cacheEntry.OrderNode);
        }

        return cacheEntry.Terms?.Matches(term) == true;
    }

    private void AddPinyinCache(Guid id, PinyinSearchTerms? terms)
    {
        RemovePinyinCache(id);
        while (_pinyinSearchCache.Count >= MaxPinyinCacheEntries
            && _pinyinCacheOrder.First is { } oldest)
        {
            _pinyinCacheOrder.RemoveFirst();
            _pinyinSearchCache.Remove(oldest.Value);
        }

        var node = _pinyinCacheOrder.AddLast(id);
        _pinyinSearchCache[id] = new PinyinCacheEntry(terms, node);
    }

    private void RemovePinyinCache(Guid id)
    {
        if (_pinyinSearchCache.Remove(id, out var entry))
        {
            _pinyinCacheOrder.Remove(entry.OrderNode);
        }
    }

    private void TouchPinyinCache(LinkedListNode<Guid> node)
    {
        _pinyinCacheOrder.Remove(node);
        _pinyinCacheOrder.AddLast(node);
    }

    private void ClearPinyinCache()
    {
        _pinyinSearchCache.Clear();
        _pinyinCacheOrder.Clear();
    }

    private static string ComputeHash(ClipboardContentKind kind, byte[] content)
    {
        // 内容类型参与哈希，避免相同字节在不同剪贴板格式下互相覆盖。
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData([(byte)kind]);
        hash.AppendData(content);
        return Convert.ToHexString(hash.GetHashAndReset());
    }

    private static ClipboardHistoryEntry[] OrderItems(IEnumerable<ClipboardHistoryEntry> items)
    {
        // 保持置顶记录在前，同时保留各组原有的更新时间顺序。
        var pinnedItems = new List<ClipboardHistoryEntry>();
        var regularItems = new List<ClipboardHistoryEntry>();
        foreach (var item in items)
        {
            (item.IsPinned ? pinnedItems : regularItems).Add(item);
        }

        return [.. pinnedItems, .. regularItems];
    }
}
