using PasteOrbit.Core;

var now = DateTimeOffset.UtcNow;
var item = new ClipboardHistoryEntry(
    Guid.NewGuid(),
    ClipboardContentKind.Text,
    "hash",
    "示例文本",
    "notepad",
    now,
    now);

Assert(item.Kind == ClipboardContentKind.Text, "内容类型应被保留");
Assert(item.SearchText == "示例文本", "检索文本应被保留");
Assert(!item.IsPinned, "新记录默认不置顶");

var history = new ClipboardHistory();
var sameContent = "same"u8.ToArray();
var first = history.AddOrUpdate(
    new ClipboardCapture(ClipboardContentKind.Text, "first", sameContent, "notepad"),
    now);
var second = history.AddOrUpdate(
    new ClipboardCapture(ClipboardContentKind.Text, "second", sameContent, "browser"),
    now.AddSeconds(1));

Assert(history.GetSnapshot().Count == 1, "相同内容不应产生重复记录");
Assert(first.Id == second.Id, "重复内容应更新原记录");
Assert(second.SearchText == "second", "重复记录应更新检索文本");
Assert(second.SourceApplication == "browser", "重复记录应更新来源应用");
Assert(second.UpdatedAt > first.UpdatedAt, "重复记录应更新时间");
Assert(history.Search("SECOND").Single().Id == second.Id, "搜索应忽略文本大小写");
Assert(history.Search("browser").Single().Id == second.Id, "搜索应匹配来源应用");
Assert(history.Search(null, ClipboardContentKind.Image).Count == 0, "类型筛选应排除其他内容");

var pinnedResult = history.SetPinned(second.Id, true);
Assert(pinnedResult?.IsPinned == true, "记录应可置顶");
Assert(history.Remove(second.Id), "记录应可删除");
Assert(history.GetSnapshot().Count == 0, "删除后记录不应保留");

var protectedText = UserDataProtector.ProtectText("仅当前用户可读取");
Assert(protectedText.AsSpan().IndexOf("仅当前用户可读取"u8) < 0, "密文不应包含明文");
Assert(UserDataProtector.UnprotectText(protectedText) == "仅当前用户可读取", "DPAPI 应能往返解密");

var testDirectory = Path.Combine(Path.GetTempPath(), "PasteOrbit.Tests", Guid.NewGuid().ToString("N"));
var databasePath = Path.Combine(testDirectory, "history.db");
try
{
    var repository = new ClipboardRepository(databasePath);
    repository.Initialize();
    repository.Upsert(second, sameContent);

    var loaded = repository.LoadEntries();
    Assert(loaded.Count == 1, "SQLite 应加载已保存记录");
    Assert(loaded[0].Id == second.Id, "SQLite 应保留记录标识");
    Assert(loaded[0].SearchText == second.SearchText, "SQLite 应解密检索文本");
    Assert(repository.LoadContent(loaded[0].Id).AsSpan().SequenceEqual(sameContent), "SQLite 应按需解密内容");
    Assert(File.ReadAllBytes(databasePath).AsSpan().IndexOf("second"u8) < 0, "数据库不应出现检索明文");

    var entry = second;
    repository.Upsert(entry, sameContent);
    Assert(repository.LoadEntries().Single() == entry, "元数据查询应保留记录信息");
    Assert(repository.LoadContent(entry.Id).AsSpan().SequenceEqual(sameContent), "正文应能按记录标识加载");
    Assert(repository.SetPinned(entry.Id, true), "置顶状态应能独立更新");
    Assert(repository.LoadEntries().Single().IsPinned, "独立置顶更新应持久化");

    var pinned = entry with { Id = Guid.NewGuid(), ContentHash = "pinned", IsPinned = true, UpdatedAt = now };
    var expired = entry with { Id = Guid.NewGuid(), ContentHash = "expired", UpdatedAt = now.AddDays(-31) };
    var recent = entry with { Id = Guid.NewGuid(), ContentHash = "recent", UpdatedAt = now.AddMinutes(1) };
    var cleanupHistory = new ClipboardHistory([pinned, expired, recent]);
    var removed = cleanupHistory.Cleanup(now.AddDays(-30), 1);
    Assert(removed.Contains(expired.Id), "过期普通记录应被清理");
    Assert(cleanupHistory.GetSnapshot().Any(candidate => candidate.Id == pinned.Id), "置顶记录不应被清理");
    Assert(cleanupHistory.GetSnapshot().Any(candidate => candidate.Id == recent.Id), "限制内的新记录应被保留");

    repository.Upsert(expired, sameContent);
    repository.Delete([expired.Id]);
    Assert(repository.LoadEntries().Count == 1, "SQLite 应删除清理出的记录");
    AssertThrows<KeyNotFoundException>(
        () => repository.LoadContent(expired.Id),
        "已删除记录的正文不应继续可用");

    var corruptPath = Path.Combine(testDirectory, "corrupt.db");
    File.WriteAllText(corruptPath, "not a sqlite database");
    var recoveredRepository = new ClipboardRepository(corruptPath);
    Assert(recoveredRepository.InitializeAndLoadEntries().Count == 0, "损坏数据库应恢复为空库");
    Assert(File.Exists(recoveredRepository.LastRecoveryPath), "损坏数据库原文件应被保留");
}
finally
{
    if (Directory.Exists(testDirectory))
    {
        Directory.Delete(testDirectory, true);
    }
}

Console.WriteLine("PasteOrbit.Core checks passed.");

static void Assert(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}

static void AssertThrows<TException>(Action action, string message)
    where TException : Exception
{
    try
    {
        action();
    }
    catch (TException)
    {
        return;
    }

    throw new InvalidOperationException(message);
}
