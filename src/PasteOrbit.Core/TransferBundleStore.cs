using System.Text.Json;

namespace PasteOrbit.Core;

/// <summary>内容分块 DPAPI 落盘，内存只保留清单；未提交上传不会进入收件箱。</summary>
public sealed class TransferBundleStore
{
    private sealed record State(TransferManifest Manifest, long[] Received, long[] Counts, bool Complete = false);
    private readonly string _root;
    public TransferBundleStore(string root) { _root = Path.GetFullPath(root); Directory.CreateDirectory(_root); }
    private string Folder(string id) => Path.Combine(_root, Guid.ParseExact(id, "D").ToString("D"));
    private State Load(string id) => JsonSerializer.Deserialize<State>(UserDataProtector.Unprotect(File.ReadAllBytes(Path.Combine(Folder(id), "state"))), DirectTransfer.Json)!;
    private void Save(string id, State state)
    {
        var path = Path.Combine(Folder(id), "state");
        File.WriteAllBytes(path + ".tmp", UserDataProtector.Protect(JsonSerializer.SerializeToUtf8Bytes(state, DirectTransfer.Json)));
        File.Move(path + ".tmp", path, true);
    }
    public static void CheckSpace(string path, long bytes)
    {
        var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(path))!);
        if (bytes < 0 || drive.AvailableFreeSpace - 64L * 1024 * 1024 < bytes)
            throw new IOException("磁盘可用空间不足，内容尚未丢弃。");
    }
    public string Begin(TransferManifest manifest)
    {
        manifest = DirectTransfer.ParseManifest(JsonSerializer.Serialize(manifest, DirectTransfer.Json));
        // 只清理本服务创建且超过一小时未活动的未完成目录。
        var pending = 0;
        foreach (var directory in Directory.GetDirectories(_root))
        {
            if (!Guid.TryParseExact(Path.GetFileName(directory), "D", out _)) continue;
            var statePath = Path.Combine(directory, "state");
            if (!File.Exists(statePath)) { if (Directory.GetLastWriteTimeUtc(directory) < DateTime.UtcNow.AddHours(-1)) Delete(Path.GetFileName(directory)); continue; }
            var state = Load(Path.GetFileName(directory));
            if (state.Complete) continue;
            if (File.GetLastWriteTimeUtc(statePath) < DateTime.UtcNow.AddHours(-1)) Delete(Path.GetFileName(directory));
            else pending++;
        }
        if (pending >= 2) throw new IOException("已有两个上传进行中，请稍后重试。");
        CheckSpace(_root, manifest.Parts.Sum(p => p.Length));
        var id = Guid.NewGuid().ToString("D");
        Directory.CreateDirectory(Folder(id));
        Save(id, new(manifest, new long[manifest.Parts.Length], new long[manifest.Parts.Length]));
        return id;
    }
    public void Write(string id, int part, long offset, byte[] bytes)
    {
        var state = Load(id);
        if (state.Complete || part < 0 || part >= state.Received.Length || offset != state.Received[part]
            || bytes.Length is <= 0 or > DirectTransfer.ChunkBytes || bytes.LongLength > state.Manifest.Parts[part].Length - offset)
            throw new InvalidDataException("块顺序或大小无效。");
        CheckSpace(_root, bytes.Length + 4096);
        var path = Path.Combine(Folder(id), $"{part}-{state.Counts[part]}");
        File.WriteAllBytes(path, UserDataProtector.Protect(bytes));
        state.Received[part] += bytes.Length; state.Counts[part]++;
        Save(id, state);
    }
    public TransferMessage Commit(string id)
    {
        var state = Load(id);
        if (state.Complete || state.Received.Where((size, i) => size != state.Manifest.Parts[i].Length).Any())
            throw new InvalidDataException("内容尚未完整或已提交。");
        Save(id, state with { Complete = true });
        return new TransferMessage { Kind = "bundle", DeviceId = id, Text = JsonSerializer.Serialize(state.Manifest, DirectTransfer.Json) };
    }
    public byte[] Read(string id, int part, long chunk)
    {
        var state = Load(id);
        if (!state.Complete || part < 0 || part >= state.Counts.Length || chunk < 0 || chunk >= state.Counts[part])
            throw new InvalidDataException("块不存在。");
        return UserDataProtector.Unprotect(File.ReadAllBytes(Path.Combine(Folder(id), $"{part}-{chunk}")));
    }
    public void Abort(string id) { if (!Load(id).Complete) Delete(id); }
    public void Delete(string id)
    {
        // ID 必须为 GUID，解析后的目标必定位于本服务专用目录下。
        var directory = Folder(id);
        if (Directory.Exists(directory)) Directory.Delete(directory, true);
    }
    public string[] Export(TransferMessage bundle, string destination, CancellationToken token = default)
    {
        var state = Load(bundle.DeviceId);
        if (!state.Complete) throw new InvalidDataException("内容未完成。");
        CheckSpace(destination, state.Manifest.Parts.Sum(p => p.Length));
        var folder = Path.Combine(destination, Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var paths = new List<string>();
            for (var part = 0; part < state.Manifest.Parts.Length; part++)
            {
                var path = Path.Combine(folder, $"{part + 1}-{state.Manifest.Parts[part].Name}");
                using var output = File.Create(path);
                for (long chunk = 0; chunk < state.Counts[part]; chunk++)
                {
                    token.ThrowIfCancellationRequested();
                    var bytes = Read(bundle.DeviceId, part, chunk);
                    CheckSpace(destination, bytes.Length);
                    output.Write(bytes);
                }
                output.Flush(true); paths.Add(path);
            }
            return paths.ToArray();
        }
        catch { Directory.Delete(folder, true); throw; }
    }
}
