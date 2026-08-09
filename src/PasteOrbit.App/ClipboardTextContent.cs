using System.Text;
using System.Text.Json;

namespace PasteOrbit.App;

[Serializable]
internal sealed record ClipboardTextContent(string Text, string? Html, string? Rtf)
{
    private const string PayloadPrefix = "PasteOrbit.Text/1\n";
    private static readonly byte[] PayloadPrefixBytes = Encoding.UTF8.GetBytes(PayloadPrefix);

    public byte[] Serialize()
    {
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
            return new ClipboardTextContent(Encoding.UTF8.GetString(content), null, null);
        }

        var payload = JsonSerializer.Deserialize<ClipboardTextContent>(content.AsSpan(PayloadPrefixBytes.Length));
        return payload is null || string.IsNullOrEmpty(payload.Text)
            ? throw new InvalidDataException("富文本剪切板内容无效。")
            : payload;
    }
}
