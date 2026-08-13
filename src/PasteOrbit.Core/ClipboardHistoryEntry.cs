namespace PasteOrbit.Core;

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
