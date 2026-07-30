using System.Globalization;
using Microsoft.Data.Sqlite;

namespace ClipVault.Core;

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

    public IReadOnlyList<ClipboardItem> Load()
    {
        using var connection = OpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = """
            SELECT id, kind, content_hash, search_text, content, source_application,
                   created_at, updated_at, is_pinned
            FROM clipboard_items
            ORDER BY is_pinned DESC, updated_at DESC;
            """;

        using var reader = command.ExecuteReader();
        var items = new List<ClipboardItem>();
        while (reader.Read())
        {
            items.Add(new ClipboardItem(
                Guid.Parse(reader.GetString(0)),
                (ClipboardContentKind)reader.GetInt32(1),
                reader.GetString(2),
                UserDataProtector.UnprotectText((byte[])reader[3]),
                UserDataProtector.Unprotect((byte[])reader[4]),
                reader.IsDBNull(5) ? null : UserDataProtector.UnprotectText((byte[])reader[5]),
                DateTimeOffset.Parse(reader.GetString(6), CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind),
                DateTimeOffset.Parse(reader.GetString(7), CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind),
                reader.GetBoolean(8)));
        }

        return items;
    }

    public void Upsert(ClipboardItem item)
    {
        ArgumentNullException.ThrowIfNull(item);

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

        command.Parameters.AddWithValue("$id", item.Id.ToString("D"));
        command.Parameters.AddWithValue("$kind", (int)item.Kind);
        command.Parameters.AddWithValue("$content_hash", item.ContentHash);
        command.Parameters.Add("$search_text", SqliteType.Blob).Value = UserDataProtector.ProtectText(item.SearchText);
        command.Parameters.Add("$content", SqliteType.Blob).Value = UserDataProtector.Protect(item.Content);
        command.Parameters.Add("$source_application", SqliteType.Blob).Value = item.SourceApplication is null
            ? DBNull.Value
            : UserDataProtector.ProtectText(item.SourceApplication);
        command.Parameters.AddWithValue("$created_at", item.CreatedAt.ToString("O", CultureInfo.InvariantCulture));
        command.Parameters.AddWithValue("$updated_at", item.UpdatedAt.ToString("O", CultureInfo.InvariantCulture));
        command.Parameters.AddWithValue("$is_pinned", item.IsPinned);
        command.ExecuteNonQuery();
    }

    private SqliteConnection OpenConnection()
    {
        var connection = new SqliteConnection(_connectionString);
        connection.Open();
        return connection;
    }
}
