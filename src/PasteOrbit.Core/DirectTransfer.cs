using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace PasteOrbit.Core;

public sealed record TransferFile(string Name, byte[] Data)
{
    [System.Text.Json.Serialization.JsonIgnore]
    public string? LocalPath { get; init; }
}

public sealed record TransferMessage
{
    public string Id { get; init; } = Guid.NewGuid().ToString("D");
    public long Time { get; init; } = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
    public string Kind { get; init; } = "text";
    public string Text { get; init; } = string.Empty;
    public TransferFile[] Files { get; init; } = [];
    public string DeviceId { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public string ReplyTo { get; init; } = string.Empty;
}

public sealed record TransferPairing(int Version, string Name, string Endpoint, string Key);

/// <summary>局域网传输协议：配对密钥不经网络发送，消息使用 AES-256-GCM 验证及加密。</summary>
public static partial class DirectTransfer
{
    public const int Port = 48763;
    public const int MaxWireBytes = 16 * 1024 * 1024;
    public static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web)
    {
        // 消息不嵌入 HTML，避免 Base64 的加号及中文被额外转义而放大传输。
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping
    };
    private static readonly byte[] Aad = Encoding.UTF8.GetBytes("PasteOrbit.Direct/1");
    private sealed record Envelope(byte[] Nonce, byte[] Data, byte[] Tag);

    public static TransferPairing ParsePairing(string code)
    {
        var pairing = JsonSerializer.Deserialize<TransferPairing>(code, Json)
            ?? throw new InvalidDataException("配对码为空。");
        if (pairing.Version != 1 || pairing.Name.Length is < 1 or > 80
            || Convert.FromBase64String(pairing.Key).Length != 32)
            throw new InvalidDataException("配对码格式无效。");
        ValidateEndpoint(pairing.Endpoint);
        return pairing;
    }

    public static Uri ValidateEndpoint(string endpoint)
    {
        if (!Uri.TryCreate(endpoint, UriKind.Absolute, out var uri)
            || uri.Scheme != "http" || uri.AbsolutePath != "/" || uri.Query != ""
            || uri.Fragment != "" || uri.UserInfo != ""
            || !IPAddress.TryParse(uri.Host, out var ip) || !IsPrivateAddress(ip))
            throw new InvalidDataException("地址必须是局域网 IPv4，例如 http://192.168.1.10:48763。");
        return uri;
    }

    public static bool IsPrivateAddress(IPAddress ip)
    {
        if (ip.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork) return false;
        var bytes = ip.GetAddressBytes();
        return bytes[0] == 10 || bytes[0] == 127
            || (bytes[0] == 192 && bytes[1] == 168)
            || (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31)
            || (bytes[0] == 169 && bytes[1] == 254);
    }

    public static void ValidateContent(TransferMessage message)
    {
        if (!Guid.TryParse(message.Id, out _) || message.Text is null || message.Files is null
            || message.Files.Length > 32)
            throw new InvalidDataException("内容格式或大小无效。");
        foreach (var file in message.Files)
        {
            if (file is null || string.IsNullOrWhiteSpace(file.Name) || file.Name.Length > 180
                || file.Name is "." or ".." || file.Name.EndsWith('.') || file.Name.EndsWith(' ')
                || file.Name.Any(c => c < 32 || "\\/:*?\"<>|".Contains(c)) || file.Data is null)
                throw new InvalidDataException("文件名无效。");
        }
        if (message.Kind is not ("text" or "image" or "files")
            || (message.Kind == "text" && (message.Text.Length == 0 || message.Files.Length != 0))
            || (message.Kind != "text" && message.Files.Length == 0)
            || (message.Kind == "image" && message.Files.Length != 1))
            throw new InvalidDataException("仅支持文本、图片和文件，每次最多 32 个文件。");
    }

    public static byte[] Seal(TransferMessage message, string key)
    {
        var plain = JsonSerializer.SerializeToUtf8Bytes(message, Json);
        if (plain.Length > 12 * 1024 * 1024) throw new InvalidDataException("消息过大。");
        var nonce = RandomNumberGenerator.GetBytes(12);
        var data = new byte[plain.Length];
        var tag = new byte[16];
        using var aes = new AesGcm(Convert.FromBase64String(key), 16);
        aes.Encrypt(nonce, plain, data, tag, Aad);
        return JsonSerializer.SerializeToUtf8Bytes(new Envelope(nonce, data, tag), Json);
    }

    public static TransferMessage Open(byte[] wire, string key)
    {
        if (wire.Length > MaxWireBytes) throw new InvalidDataException("消息过大。");
        var envelope = JsonSerializer.Deserialize<Envelope>(wire, Json)
            ?? throw new InvalidDataException("消息为空。");
        if (envelope.Nonce.Length != 12 || envelope.Tag.Length != 16)
            throw new InvalidDataException("消息格式错误。");
        var plain = new byte[envelope.Data.Length];
        using var aes = new AesGcm(Convert.FromBase64String(key), 16);
        aes.Decrypt(envelope.Nonce, envelope.Data, envelope.Tag, plain, Aad);
        var message = JsonSerializer.Deserialize<TransferMessage>(plain, Json)
            ?? throw new InvalidDataException("消息为空。");
        if (!Guid.TryParse(message.Id, out _) || Math.Abs((double)message.Time - DateTimeOffset.UtcNow.ToUnixTimeSeconds()) > 300)
            throw new InvalidDataException("消息已过期，请校准两端时间。");
        return message;
    }

    public static async Task<TransferMessage> SendAsync(TransferPairing peer, TransferMessage message, CancellationToken token = default)
    {
        var endpoint = new Uri(ValidateEndpoint(peer.Endpoint), "v1/transfer");
        using var handler = new HttpClientHandler { UseProxy = false, AllowAutoRedirect = false };
        using var client = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(30) };
        using var body = new ByteArrayContent(Seal(message, peer.Key));
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(30));
        token = timeout.Token;
        using var request = new HttpRequestMessage(HttpMethod.Post, endpoint) { Content = body };
        using var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, token);
        response.EnsureSuccessStatusCode();
        await using var stream = await response.Content.ReadAsStreamAsync(token);
        var reply = Open(await ReadLimitedAsync(stream, MaxWireBytes, token), peer.Key);
        if (reply.ReplyTo != message.Id) throw new InvalidDataException("响应与请求不匹配。");
        if (reply.Kind == "error") throw new InvalidDataException(reply.Text);
        return reply;
    }

    public static async Task<byte[]> ReadLimitedAsync(Stream stream, int limit, CancellationToken token = default)
    {
        using var result = new MemoryStream();
        var buffer = new byte[65536];
        int read;
        while ((read = await stream.ReadAsync(buffer, token)) != 0)
        {
            if (result.Length + read > limit) throw new InvalidDataException("内容超过大小限制。");
            result.Write(buffer, 0, read);
        }
        return result.ToArray();
    }
}
