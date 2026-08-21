using PasteOrbit.Core;

// 覆盖历史记录去重、搜索、置顶和 OCR 索引失效场景。
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

var chinese = history.AddOrUpdate(
    new ClipboardCapture(ClipboardContentKind.Text, "剪贴板历史", "pinyin"u8.ToArray(), "notepad"),
    now.AddSeconds(2));
Assert(history.Search("jiantieban").Single().Id == chinese.Id, "搜索应匹配中文全拼");
Assert(history.Search("jtb").Single().Id == chinese.Id, "搜索应匹配拼音首字母");
Assert(history.Search("JIAN").Single().Id == chinese.Id, "拼音搜索应忽略大小写");
Assert(history.Search("不存在").Count == 0, "无关查询不应匹配拼音索引");
var updatedChinese = history.AddOrUpdate(
    new ClipboardCapture(ClipboardContentKind.Text, "轨道交通", "pinyin"u8.ToArray(), "notepad"),
    now.AddSeconds(3));
Assert(updatedChinese.Id == chinese.Id, "重复内容更新时应保留原记录标识");
Assert(history.Search("gdjt").Single().Id == chinese.Id, "记录更新后应重建拼音索引");
Assert(history.Search("jtb").Count == 0, "记录更新后不应保留旧拼音索引");

var image = history.AddOrUpdate(
    new ClipboardCapture(ClipboardContentKind.Image, "图片内容", "image"u8.ToArray(), "snippingtool"),
    now.AddSeconds(4));
Assert(history.SetOcrText(image.Id, "剪切板中的识别文字")?.OcrText == "剪切板中的识别文字", "OCR 文字应更新到历史记录");
Assert(history.Search("识别文字").Single().Id == image.Id, "搜索应匹配 OCR 文字");
Assert(history.Search("sbwz").Single().Id == image.Id, "拼音搜索应匹配 OCR 文字首字母");

var protectedText = UserDataProtector.ProtectText("仅当前用户可读取");
Assert(protectedText.AsSpan().IndexOf("仅当前用户可读取"u8) < 0, "密文不应包含明文");
Assert(UserDataProtector.UnprotectText(protectedText) == "仅当前用户可读取", "DPAPI 应能往返解密");

var testDirectory = Path.Combine(Path.GetTempPath(), "PasteOrbit.Tests", Guid.NewGuid().ToString("N"));
var databasePath = Path.Combine(testDirectory, "history.db");
try
{
    // 使用临时数据库验证加密持久化、迁移、清理和损坏恢复。
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
    Assert(repository.SetOcrText(entry.Id, "encrypted OCR text"), "OCR 文字应能独立更新");
    Assert(repository.LoadEntries().Single().OcrText == "encrypted OCR text", "SQLite 应解密 OCR 文字");
    Assert(File.ReadAllBytes(databasePath).AsSpan().IndexOf("encrypted OCR text"u8) < 0, "数据库不应出现 OCR 明文");
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
    repository.Compact();
    Assert(repository.LoadEntries().Count == 1, "SQLite 应删除清理出的记录");
    AssertThrows<KeyNotFoundException>(
        () => repository.LoadContent(expired.Id),
        "已删除记录的正文不应继续可用");

    var compactEntry = entry with
    {
        Id = Guid.NewGuid(),
        ContentHash = "compact-large",
        UpdatedAt = now.AddMinutes(2)
    };
    repository.Upsert(compactEntry, new byte[1024 * 1024]);
    var databaseSizeBeforeCompact = new FileInfo(databasePath).Length;
    repository.Delete([compactEntry.Id]);
    repository.Compact();
    var databaseSizeAfterCompact = new FileInfo(databasePath).Length;
    Assert(databaseSizeAfterCompact < databaseSizeBeforeCompact, "SQLite VACUUM 应回收已删除内容占用的空间");

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
