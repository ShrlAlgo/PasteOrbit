using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text.Json;
using PasteOrbit.Core;

namespace PasteOrbit.App;

internal sealed record SharePeer(string Id, string Name, TransferPairing? Pairing)
{
    public override string ToString() => Pairing is null ? $"{Name}（手机，打开应用接收）" : Name;
}

/// <summary>配对信息和收件箱均用 DPAPI 存储；网络监听仅在用户开启时运行。</summary>
internal sealed class DirectShareService : IDisposable
{
    private sealed class State
    {
        public string Key { get; set; } = Convert.ToBase64String(RandomNumberGenerator.GetBytes(32));
        public List<SharePeer> Peers { get; set; } = [];
        public Dictionary<string, long> Seen { get; set; } = [];
    }

    private readonly object _sync = new();
    private readonly string _root;
    private readonly string _statePath;
    private State _state;
    private readonly TransferBundleStore _bundles;
    private DirectTransferListener? _listener;
    public event Action? Changed;
    public bool Listening => _listener is not null;
    public string ReceivedDirectory => Path.Combine(_root, "files");

    public DirectShareService(string root)
    {
        _root = root;
        _statePath = Path.Combine(root, "devices.bin");
        Directory.CreateDirectory(root);
        _bundles = new TransferBundleStore(Path.Combine(root, "bundles"));
        _state = File.Exists(_statePath)
            ? JsonSerializer.Deserialize<State>(UserDataProtector.Unprotect(File.ReadAllBytes(_statePath)), DirectTransfer.Json)
                ?? throw new InvalidDataException("设备配置无效。")
            : new State();
        SaveState();
    }

    public SharePeer[] Peers { get { lock (_sync) return _state.Peers.ToArray(); } }
    public string[] Incoming { get { lock (_sync) return Directory.GetFiles(_root, "in-*.bin"); } }
    public static string[] Addresses => NetworkInterface.GetAllNetworkInterfaces()
        .Where(n => n.OperationalStatus == OperationalStatus.Up)
        .SelectMany(n => n.GetIPProperties().UnicastAddresses)
        .Select(a => a.Address).Where(a => a.AddressFamily == AddressFamily.InterNetwork
            && !IPAddress.IsLoopback(a) && DirectTransfer.IsPrivateAddress(a))
        .Select(a => $"http://{a}:{DirectTransfer.Port}").Distinct().ToArray();

    public string PairingCode(string endpoint)
    {
        DirectTransfer.ValidateEndpoint(endpoint);
        lock (_sync) return JsonSerializer.Serialize(new TransferPairing(1, Environment.MachineName, endpoint, _state.Key), DirectTransfer.Json);
    }

    public void AddPeer(string code)
    {
        var pairing = DirectTransfer.ParsePairing(code);
        lock (_sync)
        {
            if (_state.Peers.Count >= 16 && !_state.Peers.Any(p => p.Pairing?.Endpoint == pairing.Endpoint))
                throw new InvalidOperationException("最多配对 16 台设备。");
            _state.Peers.RemoveAll(p => p.Pairing?.Endpoint == pairing.Endpoint);
            _state.Peers.Add(new SharePeer(Guid.NewGuid().ToString("D"), pairing.Name, pairing));
            SaveState();
        }
        Changed?.Invoke();
    }

    public void SetListening(bool enabled)
    {
        if (enabled && _listener is null) _listener = new DirectTransferListener(_state.Key, ReceiveAsync);
        if (!enabled) { _listener?.Dispose(); _listener = null; }
        Changed?.Invoke();
    }

    public void ResetPairing()
    {
        SetListening(false);
        lock (_sync)
        {
            _state.Key = Convert.ToBase64String(RandomNumberGenerator.GetBytes(32));
            _state.Peers.Clear();
            _state.Seen.Clear();
            // 旧密钥授权的待发件必须一起撤销，只删除本服务命名的队列文件。
            foreach (var file in Directory.GetFiles(_root, "out-*.bin")) DeleteMessage(file);
            SaveState();
        }
        Changed?.Invoke();
    }

    public async Task SendAsync(SharePeer peer, TransferMessage message, CancellationToken token = default, IProgress<long>? progress = null)
    {
        DirectTransfer.ValidateContent(message);
        if (peer.Pairing is { } pairing)
        {
            var reply = await Task.Run(() => DirectTransfer.UploadAsync(pairing, message, token, progress), token);
            if (reply.Kind != "ok") throw new InvalidDataException("设备未确认接收。");
        }
        else
        {
            string id;
            var manifest = await Task.Run(() => DirectTransfer.Manifest(message), token);
            lock (_sync)
            {
                if (!_state.Peers.Any(p => p.Id == peer.Id)) throw new InvalidOperationException("设备已撤销。");
                var path = Path.Combine(_root, $"out-{peer.Id}.bin");
                if (File.Exists(path)) throw new InvalidOperationException("该手机还有待接收内容，请先在手机上接收。");
                id = _bundles.Begin(manifest);
            }
            try
            {
                long sent = 0;
                await Task.Run(() => DirectTransfer.UploadPartsAsync(message, manifest, (part, offset, data) =>
                {
                    lock (_sync) _bundles.Write(id, part, offset, data);
                    sent += data.Length; progress?.Report(sent);
                    return Task.CompletedTask;
                }, token), token);
                lock (_sync)
                {
                    token.ThrowIfCancellationRequested();
                    if (!_state.Peers.Any(p => p.Id == peer.Id)) throw new InvalidOperationException("设备已撤销。");
                    var path = Path.Combine(_root, $"out-{peer.Id}.bin");
                    if (File.Exists(path)) throw new IOException("设备已有待发件。");
                    WriteProtected(path, _bundles.Commit(id));
                }
            }
            catch { lock (_sync) _bundles.Delete(id); throw; }
        }
        Changed?.Invoke();
    }

    internal Task<TransferMessage> ReceiveAsync(TransferMessage request, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        TransferMessage reply;
        lock (_sync)
        {
            // 块写入由严格递增偏移防重复，读取幂等，不占用短期控制消息重放表。
            token.ThrowIfCancellationRequested();
            if (request.Kind is "part" or "read")
            {
                var fields = request.Text.Split(':');
                if (fields.Length != 2 || !int.TryParse(fields[0], out var part) || !long.TryParse(fields[1], out var position))
                    throw new InvalidDataException("块坐标无效。");
                if (request.Kind == "read")
                    return Task.FromResult(new TransferMessage { Kind = "chunk", Files = [new("chunk", _bundles.Read(request.DeviceId, part, position))] });
                if (request.Files.Length != 1) throw new InvalidDataException("块无效。");
                _bundles.Write(request.DeviceId, part, position, request.Files[0].Data);
                return Task.FromResult(new TransferMessage { Kind = "ok" });
            }
            var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            foreach (var id in _state.Seen.Where(p => p.Value < now - 600).Select(p => p.Key).ToArray()) _state.Seen.Remove(id);
            if (_state.Seen.ContainsKey(request.Id) || _state.Seen.Count >= 2048)
                throw new InvalidDataException("重复请求或请求过多。");
            if (request.Kind == "begin")
            {
                if (Incoming.Length >= 10) throw new IOException("收件箱已满。");
                reply = new TransferMessage { Kind = "ok", DeviceId = _bundles.Begin(DirectTransfer.ParseManifest(request.Text)) };
            }
            else if (request.Kind == "abort")
            {
                _bundles.Abort(request.DeviceId); reply = new TransferMessage { Kind = "ok" };
            }
            else if (request.Kind == "commit")
            {
                if (Incoming.Length >= 10) throw new IOException("收件箱已满。");
                var bundle = _bundles.Commit(request.DeviceId);
                try { WriteProtected(Path.Combine(_root, $"in-{bundle.Id}.bin"), bundle); }
                catch { _bundles.Delete(request.DeviceId); throw; }
                reply = new TransferMessage { Kind = "ok" };
            }
            else if (request.Kind is "hello" or "poll" or "ack")
            {
                if (!Guid.TryParseExact(request.DeviceId, "D", out _)) throw new InvalidDataException("设备标识无效。");
                var peer = _state.Peers.FirstOrDefault(p => p.Id == request.DeviceId && p.Pairing is null);
                if (request.Kind == "hello")
                {
                    if (request.Name.Length is < 1 or > 80) throw new InvalidDataException("设备名称无效。");
                    if (peer is null)
                    {
                        if (_state.Peers.Count >= 16) throw new InvalidOperationException("设备数已满。");
                        _state.Peers.Add(new SharePeer(request.DeviceId, request.Name, null));
                    }
                    reply = new TransferMessage { Kind = "ok" };
                }
                else
                {
                    if (peer is null) throw new InvalidDataException("请先配对。");
                    var path = Path.Combine(_root, $"out-{peer.Id}.bin");
                    if (request.Kind == "poll") reply = File.Exists(path) ? ReadProtected(path) : new TransferMessage { Kind = "empty" };
                    else
                    {
                        if (File.Exists(path) && ReadProtected(path).Id == request.Text) DeleteMessage(path);
                        reply = new TransferMessage { Kind = "ok" };
                    }
                }
            }
            else
            {
                DirectTransfer.ValidateContent(request);
                if (Incoming.Length >= 10) throw new InvalidOperationException("收件箱已满。");
                WriteProtected(Path.Combine(_root, $"in-{Guid.Parse(request.Id):D}.bin"), request);
                reply = new TransferMessage { Kind = "ok" };
            }
            _state.Seen.Add(request.Id, now);
            SaveState();
        }
        Changed?.Invoke();
        return Task.FromResult(reply);
    }

    public TransferMessage ReadIncoming(string path)
    {
        lock (_sync)
        {
            if (!Incoming.Contains(path)) throw new InvalidOperationException("收件不存在。");
            return ReadProtected(path);
        }
    }

    public void DeleteIncoming(string path)
    {
        lock (_sync) { if (Incoming.Contains(path)) DeleteMessage(path); }
        Changed?.Invoke();
    }

    private void DeleteMessage(string path)
    {
        var message = ReadProtected(path);
        File.Delete(path);
        if (message.Kind == "bundle") _bundles.Delete(message.DeviceId);
    }

    public string[] Export(TransferMessage bundle, CancellationToken token = default)
    {
        lock (_sync) return _bundles.Export(bundle, ReceivedDirectory, token);
    }

    private TransferMessage ReadProtected(string path) => JsonSerializer.Deserialize<TransferMessage>(
        UserDataProtector.Unprotect(File.ReadAllBytes(path)), DirectTransfer.Json) ?? throw new InvalidDataException();
    private void SaveState() => WriteProtected(_statePath, _state);
    private static void WriteProtected<T>(string path, T value)
    {
        var bytes = UserDataProtector.Protect(JsonSerializer.SerializeToUtf8Bytes(value, DirectTransfer.Json));
        File.WriteAllBytes(path + ".tmp", bytes);
        File.Move(path + ".tmp", path, true);
    }
    public void Dispose() { _listener?.Dispose(); _listener = null; }
}
