namespace PasteOrbit.Core;

/// <summary>
/// 历史记录的搜索词和内容类型筛选条件。
/// </summary>
public sealed record ClipboardHistoryQuery(
    string? SearchText = null,
    ClipboardContentKind? Kind = null);
