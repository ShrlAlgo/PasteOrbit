namespace PasteOrbit.Core;

/// <summary>
/// 一次剪贴板捕获的完整内容及搜索元数据。
/// </summary>
public sealed record ClipboardCapture(
    ClipboardContentKind Kind,
    string SearchText,
    byte[] Content,
    string? SourceApplication);
