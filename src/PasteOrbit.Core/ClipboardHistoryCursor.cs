namespace PasteOrbit.Core;

/// <summary>
/// 历史记录稳定分页使用的复合游标。
/// </summary>
public sealed record ClipboardHistoryCursor(
    bool IsPinned,
    long UpdatedAtUnixMilliseconds,
    long StorageId);
