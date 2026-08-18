using System.Text;
using System.Text.Json;

namespace PasteOrbit.App;

/// <summary>
/// 剪贴板文本的内部载荷，保留纯文本、HTML 和 RTF 三种表示。
/// </summary>
[Serializable]
internal sealed record ClipboardTextContent(string Text, string? Html, string? Rtf)
{
    private const string PayloadPrefix = "PasteOrbit.Text/1\n";
    private static readonly byte[] PayloadPrefixBytes = Encoding.UTF8.GetBytes(PayloadPrefix);

    public byte[] Serialize()
    {
        // 前缀用于区分新格式和历史版本中的纯文本字节。
        var payload = JsonSerializer.SerializeToUtf8Bytes(this);
        var content = GC.AllocateUninitializedArray<byte>(PayloadPrefixBytes.Length + payload.Length);
        PayloadPrefixBytes.CopyTo(content, 0);
        payload.CopyTo(content, PayloadPrefixBytes.Length);
        return content;
    }

    public static ClipboardTextContent Deserialize(byte[] content)
    {
        ArgumentNullException.ThrowIfNull(content);
        if (!content.AsSpan().StartsWith(PayloadPrefixBytes))
        {
            // 兼容没有结构化载荷的旧文本记录。
            return new ClipboardTextContent(Encoding.UTF8.GetString(content), null, null);
        }

        var payload = JsonSerializer.Deserialize<ClipboardTextContent>(content.AsSpan(PayloadPrefixBytes.Length));
        return payload is null || string.IsNullOrEmpty(payload.Text)
            ? throw new InvalidDataException(AppLocalization.GetString("RichTextClipboardInvalid"))
            : payload;
    }
}
