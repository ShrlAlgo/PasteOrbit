using System.Globalization;
using System.Security.Cryptography;
using System.Text;

using Microsoft.Data.Sqlite;

namespace PasteOrbit.Core;

/// <summary>
/// 负责剪贴板历史、加密正文、FTS5 搜索索引和稳定分页。
/// </summary>
public sealed class ClipboardRepository
{
    private const int CurrentSchemaVersion = 2;
    private const int MaxPreviewLength = 512;
    private const int MaxOcrPreviewLength = 256;
    private const int MaxPageSize = 200;
    private readonly string _connectionString;

    public ClipboardRepository(string databasePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(databasePath);
        DatabasePath = Path.GetFullPath(databasePath);
        _connectionString = new SqliteConnectionStringBuilder
        {
            DataSource = DatabasePath,
            Mode = SqliteOpenMode.ReadWriteCreate,
            Pooling = false
        }.ToString();
    }

    public string DatabasePath { get; }

    public string? LastRecoveryPath { get; private set; }

    public void Initialize()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(DatabasePath)!);
        LastRecoveryPath = null;

        try
        {
            if (File.Exists(DatabasePath) && !HasCurrentSchema())
            {
                DeleteDatabaseFiles();
            }

            CreateSchema();
        }
        catch (Exception exception) when (exception is SqliteException or FormatException)
        {
            RecoverCorruptDatabase();
            CreateSchema();
        }
    }

    public int Count()
    {
        return Count(new ClipboardHistoryQuery());
    }

    public int Count(ClipboardHistoryQuery query)
    {
        ArgumentNullException.ThrowIfNull(query);
        using var connection = OpenConnection();
        return CountCore(connection, query);
    }

    public ClipboardHistoryPage Search(
        ClipboardHistoryQuery query,
        ClipboardHistoryCursor? cursor,
        int pageSize)
    {
        ArgumentNullException.ThrowIfNull(query);
        ArgumentOutOfRangeException.ThrowIfLessThan(pageSize, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(pageSize, MaxPageSize);

        using var connection = OpenConnection();
        var totalCount = CountCore(connection, query);
        var unpinnedCount = CountCore(connection, query, onlyUnpinned: true);
        using var command = connection.CreateCommand();
        var hasSearch = TryGetSearchTerm(query, out _);
        var sql = new StringBuilder(BuildSelectSql(hasSearch));
        AppendQueryFilters(sql, command, query, cursor);
        sql.Append(" ORDER BY item.is_pinned DESC, item.updated_at DESC, item.storage_id DESC LIMIT $page_size;");
        command.CommandText = sql.ToString();
        command.Parameters.AddWithValue("$page_size", pageSize + 1);

        using var reader = command.ExecuteReader();
        var entries = new List<ClipboardHistoryEntry>(pageSize + 1);
        while (reader.Read())
        {
            entries.Add(ReadEntry(reader));
        }

        var hasMore = entries.Count > pageSize;
        if (hasMore)
        {
            entries.RemoveAt(entries.Count - 1);
        }

        ClipboardHistoryCursor? nextCursor = null;
        if (hasMore && entries.Count > 0)
        {
            var lastEntry = entries[^1];
            nextCursor = new ClipboardHistoryCursor(
                lastEntry.IsPinned,
                lastEntry.UpdatedAt.ToUnixTimeMilliseconds(),
                lastEntry.StorageId);
        }

        return new ClipboardHistoryPage(entries, nextCursor, totalCount, unpinnedCount);
    }

    public ClipboardHistoryEntry Upsert(
        ClipboardCapture capture,
        DateTimeOffset? capturedAt = null)
    {
        ArgumentNullException.ThrowIfNull(capture);
        ArgumentException.ThrowIfNullOrEmpty(capture.SearchText);
        ArgumentOutOfRangeException.ThrowIfZero(capture.Content.Length);

        var captured = capturedAt ?? DateTimeOffset.UtcNow;
        var capturedAtUnixMilliseconds = captured.ToUnixTimeMilliseconds();
        var contentHash = ComputeHash(capture.Kind, capture.Content);
        var previewText = CreatePreview(capture.SearchText, MaxPreviewLength);
        using var connection = OpenConnection();
        using var transaction = connection.BeginTransaction();

        var existing = LoadExistingByHash(connection, transaction, contentHash);
        long storageId;
        Guid id;
        long createdAtUnixMilliseconds;
        bool isPinned;
        string? ocrText;
        if (existing is null)
        {
            id = Guid.NewGuid();
            createdAtUnixMilliseconds = capturedAtUnixMilliseconds;
            isPinned = false;
            ocrText = null;
            storageId = InsertItem(
                connection,
                transaction,
                id,
                capture,
                contentHash,
                previewText,
                createdAtUnixMilliseconds,
                capturedAtUnixMilliseconds);
        }
        else
        {
            storageId = existing.StorageId;
            id = existing.Id;
            createdAtUnixMilliseconds = existing.CreatedAtUnixMilliseconds;
            isPinned = existing.IsPinned;
            ocrText = existing.OcrText;
            UpdateItem(
                connection,
                transaction,
                storageId,
                capture,
                previewText,
                capturedAtUnixMilliseconds);
        }

        ReplaceSearchIndex(
            connection,
            transaction,
            storageId,
            capture.SearchText,
            ocrText,
            capture.SourceApplication);
        transaction.Commit();

        return new ClipboardHistoryEntry(
            storageId,
            id,
            capture.Kind,
            previewText,
            capture.SearchText.Length,
            capture.SourceApplication,
            DateTimeOffset.FromUnixTimeMilliseconds(createdAtUnixMilliseconds),
            captured,
            isPinned,
            CreateNullablePreview(ocrText, MaxOcrPreviewLength),
            ocrText?.Length ?? 0,
            capture.Content.LongLength);
    }

    public byte[] LoadContent(Guid id)
    {
        using var connection = OpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = "SELECT content FROM clipboard_items WHERE id = $id;";
        command.Parameters.AddWithValue("$id", id.ToString("D"));
        var protectedContent = command.ExecuteScalar() as byte[];
        return protectedContent is null
            ? throw new KeyNotFoundException($"找不到剪切板记录：{id:D}")
            : UserDataProtector.Unprotect(protectedContent);
    }

    public string? LoadOcrText(Guid id)
    {
        using var connection = OpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = """
            SELECT fts.ocr_text
            FROM clipboard_items AS item
            JOIN clipboard_items_fts AS fts ON fts.rowid = item.storage_id
            WHERE item.id = $id;
            """;
        command.Parameters.AddWithValue("$id", id.ToString("D"));
        var value = command.ExecuteScalar() as string;
        return string.IsNullOrEmpty(value) ? null : value;
    }

    public ClipboardHistoryEntry? SetOcrText(Guid id, string ocrText)
    {
        ArgumentNullException.ThrowIfNull(ocrText);
        using var connection = OpenConnection();
        using var transaction = connection.BeginTransaction();
        var stored = LoadStoredEntry(connection, transaction, id);
        if (stored is null)
        {
            return null;
        }

        using (var command = connection.CreateCommand())
        {
            command.Transaction = transaction;
            command.CommandText = """
                UPDATE clipboard_items
                SET ocr_preview = $ocr_preview,
                    ocr_text_length = $ocr_text_length
                WHERE storage_id = $storage_id;
                """;
            command.Parameters.Add("$ocr_preview", SqliteType.Blob).Value = string.IsNullOrEmpty(ocrText)
                ? DBNull.Value
                : UserDataProtector.ProtectText(CreatePreview(ocrText, MaxOcrPreviewLength));
            command.Parameters.AddWithValue("$ocr_text_length", ocrText.Length);
            command.Parameters.AddWithValue("$storage_id", stored.Entry.StorageId);
            command.ExecuteNonQuery();
        }

        ReplaceSearchIndex(
            connection,
            transaction,
            stored.Entry.StorageId,
            stored.SearchText,
            ocrText,
            stored.Entry.SourceApplication);
        transaction.Commit();
        return stored.Entry with
        {
            OcrPreview = CreateNullablePreview(ocrText, MaxOcrPreviewLength),
            OcrTextLength = ocrText.Length
        };
    }

    public ClipboardHistoryEntry? SetPinned(Guid id, bool isPinned)
    {
        using var connection = OpenConnection();
        using var transaction = connection.BeginTransaction();
        using (var command = connection.CreateCommand())
        {
            command.Transaction = transaction;
            command.CommandText = "UPDATE clipboard_items SET is_pinned = $is_pinned WHERE id = $id;";
            command.Parameters.AddWithValue("$id", id.ToString("D"));
            command.Parameters.AddWithValue("$is_pinned", isPinned);
            if (command.ExecuteNonQuery() == 0)
            {
                return null;
            }
        }

        var stored = LoadStoredEntry(connection, transaction, id);
        transaction.Commit();
        return stored?.Entry;
    }

    public bool Delete(Guid id)
    {
        using var connection = OpenConnection();
        using var transaction = connection.BeginTransaction();
        var storageIds = LoadStorageIds(connection, transaction, [id]);
        if (storageIds.Count == 0)
        {
            return false;
        }

        DeleteStorageIds(connection, transaction, storageIds);
        transaction.Commit();
        return true;
    }

    public int DeleteMatching(ClipboardHistoryQuery query)
    {
        ArgumentNullException.ThrowIfNull(query);
        using var connection = OpenConnection();
        using var transaction = connection.BeginTransaction();
        using var command = connection.CreateCommand();
        command.Transaction = transaction;
        var hasSearch = TryGetSearchTerm(query, out _);
        var sql = new StringBuilder("SELECT item.storage_id FROM clipboard_items AS item");
        AppendSearchJoin(sql, hasSearch);
        sql.Append(" WHERE item.is_pinned = 0");
        AppendQueryFilters(sql, command, query, cursor: null);
        command.CommandText = sql.ToString();

        var storageIds = new List<long>();
        using (var reader = command.ExecuteReader())
        {
            while (reader.Read())
            {
                storageIds.Add(reader.GetInt64(0));
            }
        }

        DeleteStorageIds(connection, transaction, storageIds);
        transaction.Commit();
        return storageIds.Count;
    }

    public int Cleanup(DateTimeOffset cutoff, int maxEntries)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(maxEntries);
        using var connection = OpenConnection();
        using var transaction = connection.BeginTransaction();
        using var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = """
            WITH ranked AS (
                SELECT storage_id,
                       updated_at,
                       ROW_NUMBER() OVER (ORDER BY updated_at DESC, storage_id DESC) AS row_number
                FROM clipboard_items
                WHERE is_pinned = 0
            )
            SELECT storage_id
            FROM ranked
            WHERE updated_at < $cutoff OR row_number > $max_entries;
            """;
        command.Parameters.AddWithValue("$cutoff", cutoff.ToUnixTimeMilliseconds());
        command.Parameters.AddWithValue("$max_entries", maxEntries);

        var storageIds = new List<long>();
        using (var reader = command.ExecuteReader())
        {
            while (reader.Read())
            {
                storageIds.Add(reader.GetInt64(0));
            }
        }

        DeleteStorageIds(connection, transaction, storageIds);
        transaction.Commit();
        return storageIds.Count;
    }

    public void Compact(bool full = false)
    {
        using var connection = OpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = full
            ? "VACUUM; PRAGMA optimize;"
            : "PRAGMA incremental_vacuum(200); PRAGMA optimize;";
        command.ExecuteNonQuery();
    }

    private bool HasCurrentSchema()
    {
        using var connection = OpenConnection();
        using (var versionCommand = connection.CreateCommand())
        {
            versionCommand.CommandText = "PRAGMA user_version;";
            if (Convert.ToInt32(versionCommand.ExecuteScalar(), CultureInfo.InvariantCulture) != CurrentSchemaVersion)
            {
                return false;
            }
        }

        ReadOnlySpan<string> expectedColumns =
        [
            "storage_id", "id", "kind", "content_hash", "preview_text", "search_text_length",
            "content", "content_size", "source_application", "created_at", "updated_at",
            "is_pinned", "ocr_preview", "ocr_text_length"
        ];
        var actualColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var schemaCommand = connection.CreateCommand())
        {
            schemaCommand.CommandText = "PRAGMA table_info(clipboard_items);";
            using var reader = schemaCommand.ExecuteReader();
            while (reader.Read())
            {
                actualColumns.Add(reader.GetString(1));
            }
        }

        foreach (var column in expectedColumns)
        {
            if (!actualColumns.Contains(column))
            {
                return false;
            }
        }

        using var searchTableCommand = connection.CreateCommand();
        searchTableCommand.CommandText = """
            SELECT COUNT(*)
            FROM sqlite_master
            WHERE type = 'table' AND name = 'clipboard_items_fts';
            """;
        return Convert.ToInt32(searchTableCommand.ExecuteScalar(), CultureInfo.InvariantCulture) == 1;
    }

    private void CreateSchema()
    {
        using var connection = OpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = $"""
            PRAGMA auto_vacuum = INCREMENTAL;
            CREATE TABLE IF NOT EXISTS clipboard_items (
                storage_id INTEGER PRIMARY KEY AUTOINCREMENT,
                id TEXT NOT NULL UNIQUE,
                kind INTEGER NOT NULL,
                content_hash TEXT NOT NULL UNIQUE,
                preview_text BLOB NOT NULL,
                search_text_length INTEGER NOT NULL,
                content BLOB NOT NULL,
                content_size INTEGER NOT NULL,
                source_application BLOB NULL,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL,
                is_pinned INTEGER NOT NULL,
                ocr_preview BLOB NULL,
                ocr_text_length INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS ix_clipboard_items_order
                ON clipboard_items(is_pinned DESC, updated_at DESC, storage_id DESC);
            CREATE INDEX IF NOT EXISTS ix_clipboard_items_kind_order
                ON clipboard_items(kind, is_pinned DESC, updated_at DESC, storage_id DESC);
            CREATE VIRTUAL TABLE IF NOT EXISTS clipboard_items_fts USING fts5(
                search_text,
                ocr_text,
                source_application,
                full_pinyin,
                pinyin_initials,
                tokenize='trigram'
            );
            PRAGMA user_version = {CurrentSchemaVersion};
            """;
        command.ExecuteNonQuery();
    }

    private ExistingEntry? LoadExistingByHash(
        SqliteConnection connection,
        SqliteTransaction transaction,
        string contentHash)
    {
        using var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = """
            SELECT item.storage_id,
                   item.id,
                   item.created_at,
                   item.is_pinned,
                   fts.ocr_text
            FROM clipboard_items AS item
            LEFT JOIN clipboard_items_fts AS fts ON fts.rowid = item.storage_id
            WHERE item.content_hash = $content_hash;
            """;
        command.Parameters.AddWithValue("$content_hash", contentHash);
        using var reader = command.ExecuteReader();
        if (!reader.Read())
        {
            return null;
        }

        return new ExistingEntry(
            reader.GetInt64(0),
            Guid.Parse(reader.GetString(1)),
            reader.GetInt64(2),
            reader.GetBoolean(3),
            reader.IsDBNull(4) ? null : reader.GetString(4));
    }

    private static long InsertItem(
        SqliteConnection connection,
        SqliteTransaction transaction,
        Guid id,
        ClipboardCapture capture,
        string contentHash,
        string previewText,
        long createdAtUnixMilliseconds,
        long updatedAtUnixMilliseconds)
    {
        using var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = """
            INSERT INTO clipboard_items (
                id, kind, content_hash, preview_text, search_text_length,
                content, content_size, source_application, created_at, updated_at,
                is_pinned, ocr_preview, ocr_text_length)
            VALUES (
                $id, $kind, $content_hash, $preview_text, $search_text_length,
                $content, $content_size, $source_application, $created_at, $updated_at,
                0, NULL, 0);
            SELECT last_insert_rowid();
            """;
        AddCaptureParameters(command, id, capture, contentHash, previewText, updatedAtUnixMilliseconds);
        command.Parameters.AddWithValue("$created_at", createdAtUnixMilliseconds);
        return Convert.ToInt64(command.ExecuteScalar(), CultureInfo.InvariantCulture);
    }

    private static void UpdateItem(
        SqliteConnection connection,
        SqliteTransaction transaction,
        long storageId,
        ClipboardCapture capture,
        string previewText,
        long updatedAtUnixMilliseconds)
    {
        using var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = """
            UPDATE clipboard_items
            SET kind = $kind,
                preview_text = $preview_text,
                search_text_length = $search_text_length,
                content = $content,
                content_size = $content_size,
                source_application = $source_application,
                updated_at = $updated_at
            WHERE storage_id = $storage_id;
            """;
        AddCaptureParameters(command, Guid.Empty, capture, string.Empty, previewText, updatedAtUnixMilliseconds);
        command.Parameters.AddWithValue("$storage_id", storageId);
        command.ExecuteNonQuery();
    }

    private static void AddCaptureParameters(
        SqliteCommand command,
        Guid id,
        ClipboardCapture capture,
        string contentHash,
        string previewText,
        long updatedAtUnixMilliseconds)
    {
        if (id != Guid.Empty)
        {
            command.Parameters.AddWithValue("$id", id.ToString("D"));
            command.Parameters.AddWithValue("$content_hash", contentHash);
        }

        command.Parameters.AddWithValue("$kind", (int)capture.Kind);
        command.Parameters.Add("$preview_text", SqliteType.Blob).Value = UserDataProtector.ProtectText(previewText);
        command.Parameters.AddWithValue("$search_text_length", capture.SearchText.Length);
        command.Parameters.Add("$content", SqliteType.Blob).Value = UserDataProtector.Protect(capture.Content);
        command.Parameters.AddWithValue("$content_size", capture.Content.LongLength);
        command.Parameters.Add("$source_application", SqliteType.Blob).Value = capture.SourceApplication is null
            ? DBNull.Value
            : UserDataProtector.ProtectText(capture.SourceApplication);
        command.Parameters.AddWithValue("$updated_at", updatedAtUnixMilliseconds);
    }

    private static void ReplaceSearchIndex(
        SqliteConnection connection,
        SqliteTransaction transaction,
        long storageId,
        string searchText,
        string? ocrText,
        string? sourceApplication)
    {
        using (var deleteCommand = connection.CreateCommand())
        {
            deleteCommand.Transaction = transaction;
            deleteCommand.CommandText = "DELETE FROM clipboard_items_fts WHERE rowid = $storage_id;";
            deleteCommand.Parameters.AddWithValue("$storage_id", storageId);
            deleteCommand.ExecuteNonQuery();
        }

        var searchableText = string.IsNullOrEmpty(ocrText)
            ? searchText
            : $"{searchText}\n{ocrText}";
        var pinyin = PinyinSearchTerms.Create(searchableText);
        using var insertCommand = connection.CreateCommand();
        insertCommand.Transaction = transaction;
        insertCommand.CommandText = """
            INSERT INTO clipboard_items_fts (
                rowid, search_text, ocr_text, source_application, full_pinyin, pinyin_initials)
            VALUES (
                $storage_id, $search_text, $ocr_text, $source_application, $full_pinyin, $pinyin_initials);
            """;
        insertCommand.Parameters.AddWithValue("$storage_id", storageId);
        insertCommand.Parameters.AddWithValue("$search_text", searchText);
        insertCommand.Parameters.AddWithValue("$ocr_text", ocrText ?? string.Empty);
        insertCommand.Parameters.AddWithValue("$source_application", sourceApplication ?? string.Empty);
        insertCommand.Parameters.AddWithValue("$full_pinyin", pinyin?.FullPinyin ?? string.Empty);
        insertCommand.Parameters.AddWithValue("$pinyin_initials", pinyin?.Initials ?? string.Empty);
        insertCommand.ExecuteNonQuery();
    }

    private StoredEntry? LoadStoredEntry(
        SqliteConnection connection,
        SqliteTransaction transaction,
        Guid id)
    {
        using var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = """
            SELECT item.storage_id,
                   item.id,
                   item.kind,
                   item.preview_text,
                   item.search_text_length,
                   item.source_application,
                   item.created_at,
                   item.updated_at,
                   item.is_pinned,
                   item.ocr_preview,
                   item.ocr_text_length,
                   item.content_size,
                   fts.search_text
            FROM clipboard_items AS item
            JOIN clipboard_items_fts AS fts ON fts.rowid = item.storage_id
            WHERE item.id = $id;
            """;
        command.Parameters.AddWithValue("$id", id.ToString("D"));
        using var reader = command.ExecuteReader();
        return reader.Read()
            ? new StoredEntry(ReadEntry(reader), reader.GetString(12))
            : null;
    }

    private static List<long> LoadStorageIds(
        SqliteConnection connection,
        SqliteTransaction transaction,
        IReadOnlyCollection<Guid> ids)
    {
        var storageIds = new List<long>(ids.Count);
        using var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = "SELECT storage_id FROM clipboard_items WHERE id = $id;";
        var parameter = command.Parameters.Add("$id", SqliteType.Text);
        foreach (var id in ids)
        {
            parameter.Value = id.ToString("D");
            if (command.ExecuteScalar() is long storageId)
            {
                storageIds.Add(storageId);
            }
        }

        return storageIds;
    }

    private static void DeleteStorageIds(
        SqliteConnection connection,
        SqliteTransaction transaction,
        IReadOnlyCollection<long> storageIds)
    {
        if (storageIds.Count == 0)
        {
            return;
        }

        using var deleteSearchCommand = connection.CreateCommand();
        deleteSearchCommand.Transaction = transaction;
        deleteSearchCommand.CommandText = "DELETE FROM clipboard_items_fts WHERE rowid = $storage_id;";
        var searchStorageId = deleteSearchCommand.Parameters.Add("$storage_id", SqliteType.Integer);
        using var deleteItemCommand = connection.CreateCommand();
        deleteItemCommand.Transaction = transaction;
        deleteItemCommand.CommandText = "DELETE FROM clipboard_items WHERE storage_id = $storage_id;";
        var itemStorageId = deleteItemCommand.Parameters.Add("$storage_id", SqliteType.Integer);

        foreach (var storageId in storageIds)
        {
            searchStorageId.Value = storageId;
            deleteSearchCommand.ExecuteNonQuery();
            itemStorageId.Value = storageId;
            deleteItemCommand.ExecuteNonQuery();
        }
    }

    private static int CountCore(
        SqliteConnection connection,
        ClipboardHistoryQuery query,
        bool onlyUnpinned = false)
    {
        using var command = connection.CreateCommand();
        var hasSearch = TryGetSearchTerm(query, out _);
        var sql = new StringBuilder("SELECT COUNT(*) FROM clipboard_items AS item");
        AppendSearchJoin(sql, hasSearch);
        sql.Append(" WHERE 1 = 1");
        if (onlyUnpinned)
        {
            sql.Append(" AND item.is_pinned = 0");
        }

        AppendQueryFilters(sql, command, query, cursor: null);
        command.CommandText = sql.ToString();
        return Convert.ToInt32(command.ExecuteScalar(), CultureInfo.InvariantCulture);
    }

    private static string BuildSelectSql(bool hasSearch)
    {
        var sql = new StringBuilder("""
            SELECT item.storage_id,
                   item.id,
                   item.kind,
                   item.preview_text,
                   item.search_text_length,
                   item.source_application,
                   item.created_at,
                   item.updated_at,
                   item.is_pinned,
                   item.ocr_preview,
                   item.ocr_text_length,
                   item.content_size
            FROM clipboard_items AS item
            """);
        AppendSearchJoin(sql, hasSearch);
        sql.Append(" WHERE 1 = 1");
        return sql.ToString();
    }

    private static void AppendSearchJoin(StringBuilder sql, bool hasSearch)
    {
        if (hasSearch)
        {
            sql.Append(" JOIN clipboard_items_fts AS fts ON fts.rowid = item.storage_id");
        }
    }

    private static void AppendQueryFilters(
        StringBuilder sql,
        SqliteCommand command,
        ClipboardHistoryQuery query,
        ClipboardHistoryCursor? cursor)
    {
        if (query.Kind is ClipboardContentKind kind)
        {
            sql.Append(" AND item.kind = $kind");
            command.Parameters.AddWithValue("$kind", (int)kind);
        }

        if (TryGetSearchTerm(query, out var searchTerm))
        {
            if (searchTerm.Length >= 3)
            {
                sql.Append(" AND clipboard_items_fts MATCH $match_query");
                command.Parameters.AddWithValue("$match_query", CreateFtsPhrase(searchTerm));
            }
            else
            {
                sql.Append("""
                     AND (
                         fts.search_text LIKE $like_query ESCAPE '\' COLLATE NOCASE
                         OR fts.ocr_text LIKE $like_query ESCAPE '\' COLLATE NOCASE
                         OR fts.source_application LIKE $like_query ESCAPE '\' COLLATE NOCASE
                         OR fts.full_pinyin LIKE $like_query ESCAPE '\' COLLATE NOCASE
                         OR fts.pinyin_initials LIKE $like_query ESCAPE '\' COLLATE NOCASE)
                    """);
                command.Parameters.AddWithValue("$like_query", $"%{EscapeLikePattern(searchTerm)}%");
            }
        }

        if (cursor is not null)
        {
            sql.Append("""
                 AND (
                     item.is_pinned < $cursor_pinned
                     OR (item.is_pinned = $cursor_pinned AND item.updated_at < $cursor_updated_at)
                     OR (item.is_pinned = $cursor_pinned
                         AND item.updated_at = $cursor_updated_at
                         AND item.storage_id < $cursor_storage_id))
                """);
            command.Parameters.AddWithValue("$cursor_pinned", cursor.IsPinned);
            command.Parameters.AddWithValue("$cursor_updated_at", cursor.UpdatedAtUnixMilliseconds);
            command.Parameters.AddWithValue("$cursor_storage_id", cursor.StorageId);
        }
    }

    private static bool TryGetSearchTerm(ClipboardHistoryQuery query, out string searchTerm)
    {
        searchTerm = query.SearchText?.Trim() ?? string.Empty;
        return searchTerm.Length > 0;
    }

    private static string CreateFtsPhrase(string searchTerm)
    {
        return $"\"{searchTerm.Replace("\"", "\"\"", StringComparison.Ordinal)}\"";
    }

    private static string EscapeLikePattern(string searchTerm)
    {
        return searchTerm
            .Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("%", "\\%", StringComparison.Ordinal)
            .Replace("_", "\\_", StringComparison.Ordinal);
    }

    private static ClipboardHistoryEntry ReadEntry(SqliteDataReader reader)
    {
        return new ClipboardHistoryEntry(
            reader.GetInt64(0),
            Guid.Parse(reader.GetString(1)),
            (ClipboardContentKind)reader.GetInt32(2),
            UserDataProtector.UnprotectText((byte[])reader[3]),
            reader.GetInt32(4),
            reader.IsDBNull(5) ? null : UserDataProtector.UnprotectText((byte[])reader[5]),
            DateTimeOffset.FromUnixTimeMilliseconds(reader.GetInt64(6)),
            DateTimeOffset.FromUnixTimeMilliseconds(reader.GetInt64(7)),
            reader.GetBoolean(8),
            reader.IsDBNull(9) ? null : UserDataProtector.UnprotectText((byte[])reader[9]),
            reader.GetInt32(10),
            reader.GetInt64(11));
    }

    private static string CreatePreview(string text, int maxLength)
    {
        var length = Math.Min(text.Length, maxLength);
        var preview = text[..length].ReplaceLineEndings(" ");
        return text.Length > maxLength ? preview + "…" : preview;
    }

    private static string? CreateNullablePreview(string? text, int maxLength)
    {
        return string.IsNullOrEmpty(text) ? null : CreatePreview(text, maxLength);
    }

    private static string ComputeHash(ClipboardContentKind kind, byte[] content)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData([(byte)kind]);
        hash.AppendData(content);
        return Convert.ToHexString(hash.GetHashAndReset());
    }

    private SqliteConnection OpenConnection()
    {
        var connection = new SqliteConnection(_connectionString);
        connection.Open();
        return connection;
    }

    private void DeleteDatabaseFiles()
    {
        // Windows 会阻止删除仍由 SQLite 连接池持有的数据库文件。
        SqliteConnection.ClearAllPools();
        foreach (var path in new[] { DatabasePath, $"{DatabasePath}-wal", $"{DatabasePath}-shm" })
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
    }

    private void RecoverCorruptDatabase()
    {
        SqliteConnection.ClearAllPools();
        if (!File.Exists(DatabasePath))
        {
            DeleteDatabaseFiles();
            return;
        }

        LastRecoveryPath = $"{DatabasePath}.corrupt-{DateTime.UtcNow:yyyyMMddHHmmssfff}";
        File.Move(DatabasePath, LastRecoveryPath);
        foreach (var path in new[] { $"{DatabasePath}-wal", $"{DatabasePath}-shm" })
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
    }

    private sealed record ExistingEntry(
        long StorageId,
        Guid Id,
        long CreatedAtUnixMilliseconds,
        bool IsPinned,
        string? OcrText);

    private sealed record StoredEntry(
        ClipboardHistoryEntry Entry,
        string SearchText);
}
