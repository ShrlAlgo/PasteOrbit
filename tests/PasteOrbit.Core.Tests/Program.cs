using Microsoft.Data.Sqlite;

using PasteOrbit.Core;

var protectedText = UserDataProtector.ProtectText("仅当前用户可读取");
Assert(protectedText.AsSpan().IndexOf("仅当前用户可读取"u8) < 0, "DPAPI 密文不应包含明文");
Assert(UserDataProtector.UnprotectText(protectedText) == "仅当前用户可读取", "DPAPI 应能往返解密");

var testDirectory = Path.Combine(Path.GetTempPath(), "PasteOrbit.Tests", Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(testDirectory);
try
{
    VerifyLegacyDatabaseReset(Path.Combine(testDirectory, "legacy.db"));
    VerifyIncompleteDatabaseReset(Path.Combine(testDirectory, "incomplete.db"));
    VerifyHistoryStore(Path.Combine(testDirectory, "history.db"));
    VerifyImagePreviews(Path.Combine(testDirectory, "previews.db"));
    VerifyCorruptDatabaseRecovery(Path.Combine(testDirectory, "corrupt.db"));
}
finally
{
    if (Directory.Exists(testDirectory))
    {
        Directory.Delete(testDirectory, true);
    }
}

Console.WriteLine("PasteOrbit.Core checks passed.");

static void VerifyLegacyDatabaseReset(string databasePath)
{
    var connectionString = new SqliteConnectionStringBuilder { DataSource = databasePath }.ToString();
    using (var connection = new SqliteConnection(connectionString))
    {
        connection.Open();
        using var command = connection.CreateCommand();
        command.CommandText = """
            CREATE TABLE legacy_items (id TEXT PRIMARY KEY);
            INSERT INTO legacy_items (id) VALUES ('old');
            PRAGMA user_version = 1;
            """;
        command.ExecuteNonQuery();
    }

    Assert(!ClipboardRepository.IsCurrentSchema(databasePath), "旧数据库不应通过当前结构校验");
    Assert(File.Exists(databasePath), "结构校验不应删除旧数据库");
    var repository = new ClipboardRepository(databasePath);
    repository.Initialize();
    Assert(ClipboardRepository.IsCurrentSchema(databasePath), "初始化后的数据库应通过当前结构校验");
    Assert(repository.Count() == 0, "旧数据库应直接删除并创建空的新结构");
    using var currentConnection = new SqliteConnection(connectionString);
    currentConnection.Open();
    using var versionCommand = currentConnection.CreateCommand();
    versionCommand.CommandText = "PRAGMA user_version;";
    Assert(Convert.ToInt32(versionCommand.ExecuteScalar()) == 4, "新数据库应写入当前结构版本");
}

static void VerifyHistoryStore(string databasePath)
{
    var repository = new ClipboardRepository(databasePath);
    var history = new ClipboardHistory(repository);
    history.Initialize();
    var now = DateTimeOffset.UtcNow;
    var sameContent = "ENCRYPTED_CONTENT_MARKER"u8.ToArray();

    var first = history.AddOrUpdate(
        new ClipboardCapture(ClipboardContentKind.Text, "first", sameContent, "notepad"),
        now);
    var second = history.AddOrUpdate(
        new ClipboardCapture(ClipboardContentKind.Text, "second", sameContent, "browser"),
        now.AddSeconds(1));
    Assert(history.Count == 1, "相同内容不应产生重复记录");
    Assert(first.Id == second.Id, "重复内容应保留原记录标识");
    Assert(second.PreviewText == "second", "重复内容应更新短预览");
    Assert(second.SourceApplication == "browser", "重复内容应更新来源应用");
    Assert(history.Search(new ClipboardHistoryQuery("SECOND"), null, 50).Items.Single().Id == second.Id, "FTS5 搜索应忽略大小写");
    Assert(history.Search(new ClipboardHistoryQuery("browser"), null, 50).Items.Single().Id == second.Id, "搜索应匹配来源应用");
    Assert(history.Search(new ClipboardHistoryQuery("first"), null, 50).TotalCount == 0, "重复内容更新后不应保留旧索引");

    var longText = string.Concat(Enumerable.Repeat("长篇内容压力验证", 1500)) + "尾部检索标记";
    var longContent = System.Text.Encoding.UTF8.GetBytes(longText);
    var longEntry = history.AddOrUpdate(
        new ClipboardCapture(ClipboardContentKind.Text, longText, longContent, "notepad"),
        now.AddSeconds(2));
    Assert(longEntry.PreviewText.Length == 513, "卡片文本预览应限制为 512 字符并附加省略号");
    Assert(longEntry.SearchTextLength == longText.Length, "轻量记录应保留完整文本长度");
    Assert(repository.LoadContent(longEntry.Id).AsSpan().SequenceEqual(longContent), "万字文本保存后不能截断正文");
    Assert(history.Search(new ClipboardHistoryQuery("尾部检索标记"), null, 50).Items.Single().Id == longEntry.Id, "长文本末尾仍应支持原文检索");

    var chinese = history.AddOrUpdate(
        new ClipboardCapture(ClipboardContentKind.Text, "剪贴板历史", "pinyin"u8.ToArray(), "notepad"),
        now.AddSeconds(3));
    Assert(history.Search(new ClipboardHistoryQuery("剪贴板"), null, 50).Items.Single().Id == chinese.Id, "FTS5 trigram 应匹配中文原文");
    Assert(history.Search(new ClipboardHistoryQuery("历史"), null, 50).Items.Single().Id == chinese.Id, "短查询应匹配两个中文字符");
    Assert(history.Search(new ClipboardHistoryQuery("jiantieban"), null, 50).TotalCount == 0, "不再生成中文全拼索引");
    Assert(history.Search(new ClipboardHistoryQuery("jtb"), null, 50).TotalCount == 0, "不再生成拼音首字母索引");
    using (var legacyConnection = new SqliteConnection($"Data Source={databasePath};Pooling=False"))
    {
        legacyConnection.Open();
        using var legacyCommand = legacyConnection.CreateCommand();
        legacyCommand.CommandText = "UPDATE clipboard_items_fts SET full_pinyin = 'jiantieban', pinyin_initials = 'jtb' WHERE rowid = $storage_id;";
        legacyCommand.Parameters.AddWithValue("$storage_id", chinese.StorageId);
        legacyCommand.ExecuteNonQuery();
    }

    Assert(history.Search(new ClipboardHistoryQuery("jiantieban"), null, 50).TotalCount == 0, "搜索应忽略旧库保留的拼音列");
    Assert(history.Search(new ClipboardHistoryQuery("jt"), null, 50).TotalCount == 0, "短查询也应忽略旧库拼音列");

    var updatedChinese = history.AddOrUpdate(
        new ClipboardCapture(ClipboardContentKind.Text, "轨道交通", "pinyin"u8.ToArray(), "notepad"),
        now.AddSeconds(4));
    Assert(updatedChinese.Id == chinese.Id, "重复内容更新时应保留记录标识");
    Assert(history.Search(new ClipboardHistoryQuery("轨道交通"), null, 50).Items.Single().Id == chinese.Id, "重复内容更新后应重建原文索引");
    Assert(history.Search(new ClipboardHistoryQuery("jtb"), null, 50).TotalCount == 0, "重复内容更新后不应保留旧拼音索引");

    var thumbnail = "thumbnail"u8.ToArray();
    var image = history.AddOrUpdate(
        new ClipboardCapture(
            ClipboardContentKind.Image,
            "图片内容",
            "image"u8.ToArray(),
            "snippingtool",
            thumbnail),
        now.AddSeconds(5));
    Assert(repository.LoadThumbnail(image.Id)?.AsSpan().SequenceEqual(thumbnail) == true, "缩略图应按 ID 解密读取");
    var pinned = history.SetPinned(second.Id, true);
    Assert(pinned?.IsPinned == true, "记录应可置顶");
    var ordered = history.Search(new ClipboardHistoryQuery(), null, 50).Items;
    Assert(ordered[0].Id == second.Id, "置顶记录应排在普通记录之前");

    for (var index = 0; index < 125; index++)
    {
        history.AddOrUpdate(
            new ClipboardCapture(
                ClipboardContentKind.Text,
                $"paged item {index:D3}",
                System.Text.Encoding.UTF8.GetBytes($"paged-content-{index:D3}"),
                "pager"),
            now.AddMinutes(index + 1));
    }

    var allEntries = LoadAll(history, new ClipboardHistoryQuery(), 17);
    Assert(allEntries.Count == history.Count, "游标分页应加载全部记录");
    Assert(allEntries.Select(entry => entry.Id).Distinct().Count() == allEntries.Count, "游标分页不应产生重复记录");
    Assert(allEntries[0].Id == second.Id, "分页排序应始终保持置顶记录在前");
    Assert(
        allEntries.Skip(1).Zip(allEntries.Skip(2), (left, right) => left.UpdatedAt >= right.UpdatedAt).All(result => result),
        "普通记录分页应按更新时间倒序");

    var textPage = history.Search(
        new ClipboardHistoryQuery(null, ClipboardContentKind.Text),
        null,
        20);
    Assert(textPage.Items.All(entry => entry.Kind == ClipboardContentKind.Text), "类型筛选应在数据库查询中执行");

    var filterPinned = history.AddOrUpdate(
        new ClipboardCapture(ClipboardContentKind.Text, "delete group pinned", "delete-pinned"u8.ToArray(), "tests"),
        now.AddHours(3));
    history.SetPinned(filterPinned.Id, true);
    history.AddOrUpdate(
        new ClipboardCapture(ClipboardContentKind.Text, "delete group normal", "delete-normal"u8.ToArray(), "tests"),
        now.AddHours(3).AddSeconds(1));
    var deletedCount = history.DeleteMatching(new ClipboardHistoryQuery("delete group"));
    Assert(deletedCount == 1, "清空筛选结果应只删除未置顶记录");
    Assert(history.Search(new ClipboardHistoryQuery("delete group pinned"), null, 50).Items.Single().Id == filterPinned.Id, "清空筛选结果应保留置顶记录");

    var cleanupPinned = history.AddOrUpdate(
        new ClipboardCapture(ClipboardContentKind.Text, "cleanup pinned", "cleanup-pinned"u8.ToArray(), "tests"),
        now.AddDays(-90));
    history.SetPinned(cleanupPinned.Id, true);
    history.AddOrUpdate(
        new ClipboardCapture(ClipboardContentKind.Text, "cleanup expired", "cleanup-expired"u8.ToArray(), "tests"),
        now.AddDays(-90));
    var removedCount = history.Cleanup(now.AddDays(-30), 500);
    Assert(removedCount >= 1, "自动清理应删除过期普通记录");
    Assert(history.Search(new ClipboardHistoryQuery("cleanup pinned"), null, 50).TotalCount == 1, "自动清理应保留置顶记录");

    Assert(repository.LoadContent(second.Id).AsSpan().SequenceEqual(sameContent), "正文应按 ID 解密读取");
    Assert(File.ReadAllBytes(databasePath).AsSpan().IndexOf(sameContent) < 0, "数据库文件不应包含未加密正文");
    repository.Compact();
    repository.Compact(full: true);
    Assert(history.Remove(longEntry.Id), "记录应能从数据库和搜索索引删除");
    AssertThrows<KeyNotFoundException>(() => repository.LoadContent(longEntry.Id), "删除后正文不应继续可用");
}

// 覆盖旧库兼容、加密存储和预览随历史删除的生命周期。
static void VerifyImagePreviews(string databasePath)
{
    var repository = new ClipboardRepository(databasePath);
    repository.Initialize();
    var original = "original-image-content"u8.ToArray();
    var preview = "bounded-image-preview-content"u8.ToArray();
    var capture = new ClipboardCapture(ClipboardContentKind.Image, "图片", original, null);
    var entry = repository.Upsert(capture);
    Assert(repository.LoadImagePreview(entry.Id).AsSpan().SequenceEqual(original), "无预览的历史应回退原图");

    using var connection = new SqliteConnection(new SqliteConnectionStringBuilder
    {
        DataSource = databasePath,
        Pooling = false
    }.ToString());
    connection.Open();
    using var command = connection.CreateCommand();
    command.CommandText = "DROP TRIGGER delete_clipboard_image_preview; DROP TABLE clipboard_image_previews;";
    command.ExecuteNonQuery();
    repository.Initialize();
    Assert(repository.Count() == 1, "增加可选预览表不能重建或清空旧历史");
    Assert(ClipboardRepository.IsCurrentSchema(databasePath), "预览附表不改变原有结构版本");

    var updated = repository.Upsert(capture with { ImagePreview = preview });
    Assert(updated.Id == entry.Id, "补充预览应保留原记录标识");
    Assert(repository.LoadImagePreview(entry.Id).AsSpan().SequenceEqual(preview), "查看应优先读取受限预览");
    Assert(repository.LoadContent(entry.Id).AsSpan().SequenceEqual(original), "粘贴原图不能被预览替换");
    command.CommandText = "SELECT content FROM clipboard_image_previews;";
    var protectedPreview = (byte[])command.ExecuteScalar()!;
    Assert(protectedPreview.AsSpan().IndexOf(preview) < 0, "预览应加密存储");
    Assert(UserDataProtector.Unprotect(protectedPreview).AsSpan().SequenceEqual(preview), "预览应能正常解密");

    repository.Upsert(capture);
    Assert(repository.LoadImagePreview(entry.Id).AsSpan().SequenceEqual(preview), "重复捕获缺少预览时应保留已有预览");
    repository.Delete(entry.Id);
    command.CommandText = "SELECT COUNT(*) FROM clipboard_image_previews;";
    Assert(Convert.ToInt32(command.ExecuteScalar()) == 0, "删除历史时应同步删除预览");
    AssertThrows<KeyNotFoundException>(() => repository.LoadImagePreview(entry.Id), "删除后预览不可读取");

    repository.Upsert(capture with { ImagePreview = preview }, DateTimeOffset.UtcNow.AddDays(-40));
    repository.Cleanup(DateTimeOffset.UtcNow.AddDays(-30), 500);
    Assert(Convert.ToInt32(command.ExecuteScalar()) == 0, "自动清理历史时应同步删除预览");
    repository.Upsert(capture with { ImagePreview = preview });
    repository.DeleteMatching(new ClipboardHistoryQuery());
    Assert(Convert.ToInt32(command.ExecuteScalar()) == 0, "清空历史时应同步删除预览");
}

static void VerifyIncompleteDatabaseReset(string databasePath)
{
    var connectionString = new SqliteConnectionStringBuilder
    {
        DataSource = databasePath,
        Pooling = false
    }.ToString();
    using (var connection = new SqliteConnection(connectionString))
    {
        connection.Open();
        using var command = connection.CreateCommand();
        command.CommandText = """
            CREATE TABLE clipboard_items (storage_id INTEGER PRIMARY KEY);
            PRAGMA user_version = 2;
            """;
        command.ExecuteNonQuery();
    }

    Assert(!ClipboardRepository.IsCurrentSchema(databasePath), "结构不完整的数据库不应通过校验");
    Assert(File.Exists(databasePath), "结构校验不应删除不完整的数据库");
    var repository = new ClipboardRepository(databasePath);
    repository.Initialize();
    Assert(repository.Count() == 0, "当前版本但结构不完整的数据库应直接重建");
}

static IReadOnlyList<ClipboardHistoryEntry> LoadAll(
    ClipboardHistory history,
    ClipboardHistoryQuery query,
    int pageSize)
{
    var entries = new List<ClipboardHistoryEntry>();
    ClipboardHistoryCursor? cursor = null;
    do
    {
        var page = history.Search(query, cursor, pageSize);
        entries.AddRange(page.Items);
        cursor = page.NextCursor;
    }
    while (cursor is not null);

    return entries;
}

static void VerifyCorruptDatabaseRecovery(string databasePath)
{
    File.WriteAllText(databasePath, "not a sqlite database");
    Assert(!ClipboardRepository.IsCurrentSchema(databasePath), "损坏数据库不应通过结构校验");
    Assert(File.ReadAllText(databasePath) == "not a sqlite database", "结构校验不应修改损坏数据库");
    var repository = new ClipboardRepository(databasePath);
    repository.Initialize();
    Assert(repository.Count() == 0, "损坏数据库应恢复为空库");
    Assert(repository.LastRecoveryPath is not null && File.Exists(repository.LastRecoveryPath), "损坏数据库原文件应被隔离保留");
}

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
