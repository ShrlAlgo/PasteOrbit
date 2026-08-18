namespace PasteOrbit.Core;

/// <summary>
/// 剪贴板历史记录的元数据。实际内容保存在仓储中按需读取。
/// </summary>
public sealed record ClipboardHistoryEntry(
    Guid Id,
    ClipboardContentKind Kind,
    string ContentHash,
    string SearchText,
    string? SourceApplication,
    DateTimeOffset CreatedAt,
    DateTimeOffset UpdatedAt,
    bool IsPinned = false,
    string? OcrText = null);
