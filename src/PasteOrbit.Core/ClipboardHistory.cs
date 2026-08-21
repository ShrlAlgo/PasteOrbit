namespace PasteOrbit.Core;

/// <summary>
/// 提供剪贴板历史的查询和生命周期操作，持久状态由 SQLite 仓储统一管理。
/// </summary>
public sealed class ClipboardHistory(ClipboardRepository repository)
{
    private readonly ClipboardRepository _repository = repository
        ?? throw new ArgumentNullException(nameof(repository));

    public int Count => _repository.Count();

    public void Initialize()
    {
        _repository.Initialize();
    }

    public ClipboardHistoryPage Search(
        ClipboardHistoryQuery query,
        ClipboardHistoryCursor? cursor,
        int pageSize)
    {
        return _repository.Search(query, cursor, pageSize);
    }

    public ClipboardHistoryEntry AddOrUpdate(
        ClipboardCapture capture,
        DateTimeOffset? capturedAt = null)
    {
        return _repository.Upsert(capture, capturedAt);
    }

    public int Cleanup(DateTimeOffset cutoff, int maxEntries)
    {
        return _repository.Cleanup(cutoff, maxEntries);
    }

    public ClipboardHistoryEntry? SetPinned(Guid id, bool isPinned)
    {
        return _repository.SetPinned(id, isPinned);
    }

    public ClipboardHistoryEntry? SetOcrText(Guid id, string ocrText)
    {
        return _repository.SetOcrText(id, ocrText);
    }

    public string? LoadOcrText(Guid id)
    {
        return _repository.LoadOcrText(id);
    }

    public bool Remove(Guid id)
    {
        return _repository.Delete(id);
    }

    public int DeleteMatching(ClipboardHistoryQuery query)
    {
        return _repository.DeleteMatching(query);
    }
}
