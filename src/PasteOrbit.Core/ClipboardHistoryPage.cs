namespace PasteOrbit.Core;

/// <summary>
/// 历史记录分页结果。
/// </summary>
public sealed record ClipboardHistoryPage(
    IReadOnlyList<ClipboardHistoryEntry> Items,
    ClipboardHistoryCursor? NextCursor,
    int TotalCount,
    int UnpinnedCount);
