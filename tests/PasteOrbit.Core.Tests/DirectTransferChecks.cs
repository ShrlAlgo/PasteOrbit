using System.Security.Cryptography;
using PasteOrbit.Core;

internal static class DirectTransferChecks
{
    public static async Task RunAsync()
    {
        var key = Convert.ToBase64String(RandomNumberGenerator.GetBytes(32));
        var message = new TransferMessage { Text = "中文剪贴板 🪐" };
        var wire = DirectTransfer.Seal(message, key);
        Check(DirectTransfer.Open(wire, key).Text == message.Text, "加密往返");
        var envelope = System.Text.Json.Nodes.JsonNode.Parse(wire)!;
        var tag = Convert.FromBase64String(envelope["tag"]!.GetValue<string>()); tag[0] ^= 1;
        envelope["tag"] = Convert.ToBase64String(tag);
        Reject(() => DirectTransfer.Open(System.Text.Encoding.UTF8.GetBytes(envelope.ToJsonString()), key), "篡改认证标签");
        var full = new TransferMessage { Kind = "files", Files = [new("random.bin", RandomNumberGenerator.GetBytes(8 * 1024 * 1024))] };
        DirectTransfer.ValidateContent(full);
        Check(DirectTransfer.Seal(full, key).Length < DirectTransfer.MaxWireBytes, "8 MiB 内容应符合传输上限");
        Reject(() => DirectTransfer.Open(wire, Convert.ToBase64String(new byte[32])), "错误密钥");
        Reject(() => DirectTransfer.Open(DirectTransfer.Seal(message with { Time = 1 }, key), key), "过期消息");
        foreach (var endpoint in new[] { "http://8.8.8.8", "http://localhost", "http://192.168.1.1/other", "http://[::ffff:192.168.1.1]", "https://192.168.1.1" })
            Reject(() => DirectTransfer.ValidateEndpoint(endpoint), "非法地址");
        DirectTransfer.ValidateEndpoint("http://192.168.1.10:48763");
        Reject(() => DirectTransfer.ValidateContent(new TransferMessage { Kind = "files", Files = [new("../escape", [1])] }), "路径穿越");
        DirectTransfer.ValidateContent(new TransferMessage { Kind = "files", Files = [new("large.bin", new byte[9 * 1024 * 1024])] });
        await using (var stream = new MemoryStream(new byte[10]))
        {
            try { await DirectTransfer.ReadLimitedAsync(stream, 9); throw new Exception("未限制流长度"); }
            catch (InvalidDataException) { }
        }
        // 使用系统分配的回环端口，验证真实 HTTP 收发而不占用用户监听端口。
        using var listener = new DirectTransferListener(key, (incoming, _) =>
        {
            Check(incoming.Text == message.Text, "监听器解密");
            return Task.FromResult(new TransferMessage { Kind = "ok" });
        }, 0);
        var peer = new TransferPairing(1, "test", $"http://127.0.0.1:{listener.Port}", key);
        var reply = await DirectTransfer.SendAsync(peer, message);
        Check(reply.Kind == "ok" && reply.ReplyTo == message.Id, "请求响应关联");
        listener.Dispose();
        await listener.Completion;
        await VerifyQueueAsync();
        await VerifyStreamingAsync();
        Console.WriteLine("Direct transfer checks passed.");
    }

    private static async Task VerifyQueueAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "PasteOrbit.Direct.Tests", Guid.NewGuid().ToString("N"));
        try
        {
            using var service = new PasteOrbit.App.DirectShareService(root);
            var hello = new TransferMessage { Kind = "hello", DeviceId = Guid.NewGuid().ToString("D"), Name = "phone" };
            await service.ReceiveAsync(hello, default);
            var content = new TransferMessage { Text = "待领取的内容" };
            await service.SendAsync(service.Peers.Single(), content);
            var poll = new TransferMessage { Kind = "poll", DeviceId = hello.DeviceId };
            var queued = await service.ReceiveAsync(poll, default);
            Check(queued.Kind == "bundle", "手机领取清单");
            Check((await service.ReceiveAsync(poll with { Id = Guid.NewGuid().ToString("D") }, default)).Id == queued.Id, "未确认前保留内容");
            Check(File.ReadAllText(service.Export(queued).Single()) == content.Text, "队列分块内容");
            using (var reopened = new PasteOrbit.App.DirectShareService(root))
            {
                try { await reopened.ReceiveAsync(poll, default); throw new Exception("重启后未拒绝重复请求"); }
                catch (InvalidDataException) { }
            }
            await service.ReceiveAsync(new TransferMessage { Kind = "ack", DeviceId = hello.DeviceId, Text = queued.Id }, default);
            Check((await service.ReceiveAsync(poll with { Id = Guid.NewGuid().ToString("D") }, default)).Kind == "empty", "确认后移除待发件");
            var incoming = new TransferMessage { Text = "手机发往电脑" };
            await service.ReceiveAsync(incoming, default);
            Check(service.ReadIncoming(service.Incoming.Single()).Text == incoming.Text, "收件持久化");
            service.ResetPairing();
            Check(service.Peers.Length == 0 && service.Incoming.Length == 1 && !service.Listening, "撤销配对保留收件");
        }
        finally
        {
            // 仅清理本测试创建的随机独立目录。
            if (Directory.Exists(root)) Directory.Delete(root, true);
        }
    }

    private sealed class InlineProgress(Action<long> action) : IProgress<long> { public void Report(long value) => action(value); }

    private static async Task VerifyStreamingAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "PasteOrbit.Stream.Tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            using var service = new PasteOrbit.App.DirectShareService(Path.Combine(root, "receiver"));
            var key = DirectTransfer.ParsePairing(service.PairingCode("http://127.0.0.1")).Key;
            using var listener = new DirectTransferListener(key, service.ReceiveAsync, 0);
            var peer = new TransferPairing(1, "stream-test", $"http://127.0.0.1:{listener.Port}", key);
            var source = Path.Combine(root, "source.bin");
            var block = RandomNumberGenerator.GetBytes(DirectTransfer.ChunkBytes);
            using (var stream = File.Create(source))
                for (var i = 0; i < 69; i++) stream.Write(block);
            var file = new TransferMessage { Kind = "files", Files = [new("source.bin", []) { LocalPath = source }, new("empty.bin", [])] };
            Check((await DirectTransfer.UploadAsync(peer, file)).Kind == "ok", "超过 16 MiB 的文件流式上传");
            var incomingPath = service.Incoming.Single();
            var bundle = service.ReadIncoming(incomingPath);
            var exported = service.Export(bundle);
            using (var original = File.OpenRead(source))
            using (var received = File.OpenRead(exported[0]))
                Check(SHA256.HashData(original).SequenceEqual(SHA256.HashData(received)), "大文件 SHA256 一致");
            Check(new FileInfo(exported[1]).Length == 0, "零字节文件");
            var first = await DirectTransfer.SendAsync(peer, new TransferMessage { Kind = "read", DeviceId = bundle.DeviceId, Text = "0:0" });
            Check(first.Kind == "chunk" && first.Files.Single().Data.SequenceEqual(block), "手机逐块下载");
            service.DeleteIncoming(incomingPath);
            Check(!Directory.Exists(Path.Combine(root, "receiver", "bundles", bundle.DeviceId)), "删除收件释放分块文件");

            var text = new string('中', 3_000_000) + "🪐边界";
            var boundaryText = new string('a', 16383) + "🪐";
            Check(DirectTransfer.Manifest(new TransferMessage { Text = boundaryText }).Parts[0].Length == System.Text.Encoding.UTF8.GetByteCount(boundaryText), "UTF-8 长度计数保留代理项边界");
            await DirectTransfer.UploadAsync(peer, new TransferMessage { Text = text });
            incomingPath = service.Incoming.Single();
            Check(File.ReadAllText(service.Export(service.ReadIncoming(incomingPath)).Single()) == text, "超过 8 MiB UTF-8 文字完整往返");
            service.DeleteIncoming(incomingPath);
            using var cancellation = new CancellationTokenSource();
            try
            {
                await DirectTransfer.UploadAsync(peer, file, cancellation.Token, new InlineProgress(_ => cancellation.Cancel()));
                throw new Exception("未响应取消");
            }
            catch (OperationCanceledException) { }
            Check(service.Incoming.Length == 0 && Directory.GetDirectories(Path.Combine(root, "receiver", "bundles")).Length == 0, "取消不生成收件并清理上传");

            var store = new TransferBundleStore(Path.Combine(root, "store"));
            try { TransferBundleStore.CheckSpace(root, long.MaxValue); throw new Exception("未检查磁盘容量"); }
            catch (IOException) { }
            var id = store.Begin(new("image", [new("image.png", 3)]));
            Reject(() => store.Write(id, 0, 1, [1]), "错序块");
            store.Write(id, 0, 0, [1, 2]);
            Reject(() => store.Write(id, 0, 0, [1, 2]), "重复块");
            Reject(() => store.Commit(id), "不完整内容提交");
            store.Write(id, 0, 2, [3]);
            var image = store.Commit(id);
            Check(File.ReadAllBytes(store.Export(image, root).Single()).SequenceEqual(new byte[] { 1, 2, 3 }), "图像原始字节不经解码传输");
            Reject(() => DirectTransfer.ParseManifest("{\"kind\":\"text\",\"parts\":[{\"name\":\"x\",\"length\":-1}]}"), "负长度");
            listener.Dispose(); await listener.Completion;
            Console.WriteLine("Streaming transfer checks passed (17.25 MiB file, >8 MiB text, cancellation, chunk ordering).");
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
    }

    private static void Check(bool value, string name) { if (!value) throw new Exception(name); }
    private static void Reject(Action action, string name)
    {
        try { action(); }
        catch (Exception e) when (e is InvalidDataException or CryptographicException) { return; }
        throw new Exception($"未拒绝：{name}");
    }
}
