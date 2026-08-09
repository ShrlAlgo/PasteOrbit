using System.Globalization;
using System.Security.Cryptography;
using Microsoft.Data.Sqlite;

namespace PasteOrbit.Core;

public sealed class ClipboardRepository
{
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

    public IReadOnlyList<ClipboardHistoryEntry> InitializeAndLoadEntries()
    {
        return InitializeAndLoad(LoadEntries, static () => Array.Empty<ClipboardHistoryEntry>());
    }

    private T InitializeAndLoad<T>(Func<T> load, Func<T> createEmpty)
    {
        try
        {
            Initialize();
            return load();
        }
        catch (Exception exception) when (exception is SqliteException or CryptographicException or FormatException)
        {
            // 保留损坏数据库供人工恢复，不在原文件上继续写入。
            if (File.Exists(DatabasePath))
            {
                LastRecoveryPath = $"{DatabasePath}.corrupt-{DateTime.UtcNow:yyyyMMddHHmmss}";
                File.Move(DatabasePath, LastRecoveryPath);
            }

            Initialize();
            return createEmpty();
        }
    }

    public void Initialize()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(DatabasePath)!);

        using var connection = OpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = """
            CREATE TABLE IF NOT EXISTS clipboard_items (
                id TEXT PRIMARY KEY,
                kind INTEGER NOT NULL,
                content_hash TEXT NOT NULL UNIQUE,
                search_text BLOB NOT NULL,
                content BLOB NOT NULL,
                source_application BLOB NULL,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                is_pinned INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS ix_clipboard_items_updated_at
                ON clipboard_items(updated_at DESC);
            """;
        command.ExecuteNonQuery();
    }

    public IReadOnlyList<ClipboardHistoryEntry> LoadEntries()
    {
        using var connection = OpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = """
            SELECT id, kind, content_hash, search_text, source_application,
                   created_at, updated_at, is_pinned
            FROM clipboard_items
            ORDER BY is_pinned DESC, updated_at DESC;
            """;

        using var reader = command.ExecuteReader();
        var entries = new List<ClipboardHistoryEntry>();
        while (reader.Read())
        {
            entries.Add(new ClipboardHistoryEntry(
                Guid.Parse(reader.GetString(0)),
                (ClipboardContentKind)reader.GetInt32(1),
                reader.GetString(2),
                UserDataProtector.UnprotectText((byte[])reader[3]),
                reader.IsDBNull(4) ? null : UserDataProtector.UnprotectText((byte[])reader[4]),
                DateTimeOffset.Parse(reader.GetString(5), CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind),
                DateTimeOffset.Parse(reader.GetString(6), CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind),
                reader.GetBoolean(7)));
        }

        return entries;
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

    public void Upsert(ClipboardHistoryEntry entry, byte[] content)
    {
        ArgumentNullException.ThrowIfNull(entry);
        ArgumentNullException.ThrowIfNull(content);
        ArgumentOutOfRangeException.ThrowIfZero(content.Length);

        UpsertCore(
            entry.Id,
            entry.Kind,
            entry.ContentHash,
            entry.SearchText,
            content,
            entry.SourceApplication,
            entry.CreatedAt,
            entry.UpdatedAt,
            entry.IsPinned);
    }

    private void UpsertCore(
        Guid id,
        ClipboardContentKind kind,
        string contentHash,
        string searchText,
        byte[] content,
        string? sourceApplication,
        DateTimeOffset createdAt,
        DateTimeOffset updatedAt,
        bool isPinned)
    {

        using var connection = OpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = """
            INSERT INTO clipboard_items (
                id, kind, content_hash, search_text, content, source_application,
                created_at, updated_at, is_pinned)
            VALUES (
                $id, $kind, $content_hash, $search_text, $content, $source_application,
                $created_at, $updated_at, $is_pinned)
            ON CONFLICT(content_hash) DO UPDATE SET
                search_text = excluded.search_text,
                content = excluded.content,
                source_application = excluded.source_application,
                updated_at = excluded.updated_at,
                is_pinned = excluded.is_pinned;
            """;

        command.Parameters.AddWithValue("$id", id.ToString("D"));
        command.Parameters.AddWithValue("$kind", (int)kind);
        command.Parameters.AddWithValue("$content_hash", contentHash);
        command.Parameters.Add("$search_text", SqliteType.Blob).Value = UserDataProtector.ProtectText(searchText);
        command.Parameters.Add("$content", SqliteType.Blob).Value = UserDataProtector.Protect(content);
        command.Parameters.Add("$source_application", SqliteType.Blob).Value = sourceApplication is null
            ? DBNull.Value
            : UserDataProtector.ProtectText(sourceApplication);
        command.Parameters.AddWithValue("$created_at", createdAt.ToString("O", CultureInfo.InvariantCulture));
        command.Parameters.AddWithValue("$updated_at", updatedAt.ToString("O", CultureInfo.InvariantCulture));
        command.Parameters.AddWithValue("$is_pinned", isPinned);
        command.ExecuteNonQuery();
    }

    public bool SetPinned(Guid id, bool isPinned)
    {
        using var connection = OpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = "UPDATE clipboard_items SET is_pinned = $is_pinned WHERE id = $id;";
        command.Parameters.AddWithValue("$id", id.ToString("D"));
        command.Parameters.AddWithValue("$is_pinned", isPinned);
        return command.ExecuteNonQuery() > 0;
    }

    public void Delete(IEnumerable<Guid> ids)
    {
        ArgumentNullException.ThrowIfNull(ids);
        using var enumerator = ids.GetEnumerator();
        if (!enumerator.MoveNext())
        {
            return;
        }

        using var connection = OpenConnection();
        using var transaction = connection.BeginTransaction();
        using var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = "DELETE FROM clipboard_items WHERE id = $id;";
        var idParameter = command.Parameters.Add("$id", SqliteType.Text);

        do
        {
            idParameter.Value = enumerator.Current.ToString("D");
            command.ExecuteNonQuery();
        }
        while (enumerator.MoveNext());

        transaction.Commit();
    }

    private SqliteConnection OpenConnection()
    {
        var connection = new SqliteConnection(_connectionString);
        connection.Open();
        return connection;
    }
}
