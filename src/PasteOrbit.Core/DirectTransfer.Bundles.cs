using System.Text;
using System.Text.Json;

namespace PasteOrbit.Core;

public sealed record TransferPart(string Name, long Length);
public sealed record TransferManifest(string Kind, TransferPart[] Parts);

public static partial class DirectTransfer
{
    public const int ChunkBytes = 256 * 1024;

    public static TransferManifest Manifest(TransferMessage message)
    {
        ValidateContent(message);
        return new(message.Kind, message.Kind == "text"
            ? [new("text.txt", Utf8Length(message.Text))]
            : message.Files.Select(f => new TransferPart(f.Name, f.LocalPath is null ? f.Data.LongLength : new FileInfo(f.LocalPath).Length)).ToArray());
    }

    private static long Utf8Length(string text)
    {
        long length = 0;
        for (var position = 0; position < text.Length;)
        {
            var count = Math.Min(16384, text.Length - position);
            if (position + count < text.Length && char.IsHighSurrogate(text[position + count - 1])) count--;
            length = checked(length + Encoding.UTF8.GetByteCount(text.AsSpan(position, count)));
            position += count;
        }
        return length;
    }

    public static TransferManifest ParseManifest(string json)
    {
        if (json.Length > 16384) throw new InvalidDataException("清单元数据过大。");
        var manifest = JsonSerializer.Deserialize<TransferManifest>(json, Json) ?? throw new InvalidDataException("缺少清单。");
        if (manifest.Kind is not ("text" or "image" or "files") || manifest.Parts is null
            || manifest.Parts.Length is < 1 or > 32 || (manifest.Kind != "files" && manifest.Parts.Length != 1))
            throw new InvalidDataException("清单无效。");
        foreach (var part in manifest.Parts)
        {
            if (part is null || part.Length < 0) throw new InvalidDataException("长度无效。");
            ValidateContent(new TransferMessage { Kind = "files", Files = [new(part.Name, [])] });
        }
        _ = manifest.Parts.Aggregate(0L, (total, p) => checked(total + p.Length));
        return manifest;
    }

    // 文字本来就在历史模型中；按字符增量编码，不再创建完整 UTF-8 / Base64 副本。
    public static async Task UploadPartsAsync(TransferMessage message, TransferManifest manifest,
        Func<int, long, byte[], Task> write, CancellationToken token)
    {
        var buffer = new byte[ChunkBytes];
        for (var i = 0; i < manifest.Parts.Length; i++)
        {
            long offset = 0;
            if (message.Kind == "text")
            {
                var encoder = Encoding.UTF8.GetEncoder();
                var position = 0;
                while (position < message.Text.Length)
                {
                    token.ThrowIfCancellationRequested();
                    encoder.Convert(message.Text.AsSpan(position), buffer, true, out var chars, out var bytes, out _);
                    position += chars;
                    // UTF-8 编码器保留字符边界，因此块可以略小于缓冲区。
                    await write(i, offset, buffer.AsSpan(0, bytes).ToArray());
                    offset += bytes;
                }
            }
            else
            {
                var file = message.Files[i];
                await using Stream stream = file.LocalPath is null ? new MemoryStream(file.Data, false) : File.OpenRead(file.LocalPath);
                while (offset < manifest.Parts[i].Length)
                {
                    token.ThrowIfCancellationRequested();
                    var count = (int)Math.Min(buffer.Length, manifest.Parts[i].Length - offset);
                    await stream.ReadExactlyAsync(buffer.AsMemory(0, count), token);
                    await write(i, offset, buffer.AsSpan(0, count).ToArray());
                    offset += count;
                }
                if (stream.ReadByte() != -1) throw new IOException("发送期间源文件大小发生变化。");
            }
            if (offset != manifest.Parts[i].Length) throw new IOException("源内容长度发生变化。");
        }
    }

    public static async Task<TransferMessage> UploadAsync(TransferPairing peer, TransferMessage message,
        CancellationToken token = default, IProgress<long>? progress = null)
    {
        var manifest = Manifest(message);
        var begin = await SendAsync(peer, new TransferMessage { Kind = "begin", Text = JsonSerializer.Serialize(manifest, Json) }, token);
        if (begin.Kind != "ok" || !Guid.TryParseExact(begin.DeviceId, "D", out _)) throw new InvalidDataException("请更新接收端以支持分块传输。");
        long sent = 0;
        try
        {
            await UploadPartsAsync(message, manifest, async (part, offset, bytes) =>
            {
                await SendAsync(peer, new TransferMessage { Kind = "part", DeviceId = begin.DeviceId,
                    Text = $"{part}:{offset}", Files = [new("chunk", bytes)] }, token);
                sent += bytes.Length; progress?.Report(sent);
            }, token);
            return await SendAsync(peer, new TransferMessage { Kind = "commit", DeviceId = begin.DeviceId }, token);
        }
        catch
        {
            using var cleanup = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            try { await SendAsync(peer, new TransferMessage { Kind = "abort", DeviceId = begin.DeviceId }, cleanup.Token); } catch { }
            throw;
        }
    }
}
