using ClipVault.Core;

var now = DateTimeOffset.UtcNow;
var item = new ClipboardItem(
    Guid.NewGuid(),
    ClipboardContentKind.Text,
    "hash",
    "示例文本",
    "示例文本"u8.ToArray(),
    "notepad",
    now,
    now);

Assert(item.Kind == ClipboardContentKind.Text, "内容类型应被保留");
Assert(item.SearchText == "示例文本", "检索文本应被保留");
Assert(!item.IsPinned, "新记录默认不置顶");

Console.WriteLine("ClipVault.Core checks passed.");

static void Assert(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}
