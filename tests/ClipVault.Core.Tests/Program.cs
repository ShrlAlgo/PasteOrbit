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

var history = new ClipboardHistory();
var first = history.AddOrUpdate(
    new ClipboardCapture(ClipboardContentKind.Text, "first", "same"u8.ToArray(), "notepad"),
    now);
var second = history.AddOrUpdate(
    new ClipboardCapture(ClipboardContentKind.Text, "second", "same"u8.ToArray(), "browser"),
    now.AddSeconds(1));

Assert(history.GetSnapshot().Count == 1, "相同内容不应产生重复记录");
Assert(first.Id == second.Id, "重复内容应更新原记录");
Assert(second.SearchText == "second", "重复记录应更新检索文本");
Assert(second.SourceApplication == "browser", "重复记录应更新来源应用");
Assert(second.UpdatedAt > first.UpdatedAt, "重复记录应更新时间");

var protectedText = UserDataProtector.ProtectText("仅当前用户可读取");
Assert(protectedText.AsSpan().IndexOf("仅当前用户可读取"u8) < 0, "密文不应包含明文");
Assert(UserDataProtector.UnprotectText(protectedText) == "仅当前用户可读取", "DPAPI 应能往返解密");

var testDirectory = Path.Combine(Path.GetTempPath(), "ClipVault.Tests", Guid.NewGuid().ToString("N"));
var databasePath = Path.Combine(testDirectory, "history.db");
try
{
    var repository = new ClipboardRepository(databasePath);
    repository.Initialize();
    repository.Upsert(second);

    var loaded = repository.Load();
    Assert(loaded.Count == 1, "SQLite 应加载已保存记录");
    Assert(loaded[0].Id == second.Id, "SQLite 应保留记录标识");
    Assert(loaded[0].SearchText == second.SearchText, "SQLite 应解密检索文本");
    Assert(loaded[0].Content.AsSpan().SequenceEqual(second.Content), "SQLite 应解密内容");
    Assert(File.ReadAllBytes(databasePath).AsSpan().IndexOf("second"u8) < 0, "数据库不应出现检索明文");

    var pinned = second with { Id = Guid.NewGuid(), ContentHash = "pinned", IsPinned = true, UpdatedAt = now };
    var expired = second with { Id = Guid.NewGuid(), ContentHash = "expired", UpdatedAt = now.AddDays(-31) };
    var recent = second with { Id = Guid.NewGuid(), ContentHash = "recent", UpdatedAt = now.AddMinutes(1) };
    var cleanupHistory = new ClipboardHistory([pinned, expired, recent]);
    var removed = cleanupHistory.Cleanup(now.AddDays(-30), 1);
    Assert(removed.Contains(expired.Id), "过期普通记录应被清理");
    Assert(cleanupHistory.GetSnapshot().Any(candidate => candidate.Id == pinned.Id), "置顶记录不应被清理");
    Assert(cleanupHistory.GetSnapshot().Any(candidate => candidate.Id == recent.Id), "限制内的新记录应被保留");

    repository.Upsert(expired);
    repository.Delete([expired.Id]);
    Assert(repository.Load().Count == 1, "SQLite 应删除清理出的记录");

    var corruptPath = Path.Combine(testDirectory, "corrupt.db");
    File.WriteAllText(corruptPath, "not a sqlite database");
    var recoveredRepository = new ClipboardRepository(corruptPath);
    Assert(recoveredRepository.InitializeAndLoad().Count == 0, "损坏数据库应恢复为空库");
    Assert(File.Exists(recoveredRepository.LastRecoveryPath), "损坏数据库原文件应被保留");
}
finally
{
    if (Directory.Exists(testDirectory))
    {
        Directory.Delete(testDirectory, true);
    }
}

Console.WriteLine("ClipVault.Core checks passed.");

static void Assert(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}
