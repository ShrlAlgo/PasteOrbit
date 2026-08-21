namespace PasteOrbit.Core;

/// <summary>
/// 剪贴板历史记录的轻量元数据。完整搜索文本和实际内容保存在仓储中按需读取。
/// </summary>
public sealed record ClipboardHistoryEntry(
    long StorageId,
    Guid Id,
    ClipboardContentKind Kind,
    string PreviewText,
    int SearchTextLength,
    string? SourceApplication,
    DateTimeOffset CreatedAt,
    DateTimeOffset UpdatedAt,
    bool IsPinned = false,
    string? OcrPreview = null,
    int OcrTextLength = 0,
    long ContentSize = 0);
