namespace ClipVault.Core;

public enum ClipboardContentKind
{
    Text,
    Image,
    Files
}

public sealed record ClipboardItem(
    Guid Id,
    ClipboardContentKind Kind,
    string ContentHash,
    string SearchText,
    byte[] Content,
    string? SourceApplication,
    DateTimeOffset CreatedAt,
    DateTimeOffset UpdatedAt,
    bool IsPinned = false);
