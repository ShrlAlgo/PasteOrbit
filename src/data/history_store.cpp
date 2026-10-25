#include "history_store.h"
#include "data_protection.h"
#include "clipboard_content.h"

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QUuid>


namespace {

using DataProtection::protect;
using DataProtection::unprotect;

constexpr int CurrentSchemaVersion = 6;

std::optional<QByteArray> protectTargets(const ClipboardContent::ContentMetadata &metadata) {
    return protect(QJsonDocument(QJsonArray::fromStringList(metadata.openTargets)).toJson(QJsonDocument::Compact));
}

class Connection {
public:
    explicit Connection(const QString &path)
        : name_(QStringLiteral("pasteorbit-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        database_.setDatabaseName(path);
        database_.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=1500"));
        open_ = database_.open();
    }

    ~Connection() {
        database_.close();
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }

    bool isOpen() const { return open_; }
    QSqlDatabase &database() { return database_; }
    QString error() const { return database_.lastError().text(); }

private:
    QString name_;
    QSqlDatabase database_;
    bool open_ = false;
};

bool execute(QSqlDatabase &database, const QString &sql, QString *error = nullptr) {
    QSqlQuery query(database);
    if (query.exec(sql)) return true;
    if (error) *error = query.lastError().text();
    return false;
}

bool hasRequiredColumns(QSqlDatabase &database, int schema = 0) {
    const QStringList required{
        QStringLiteral("storage_id"), QStringLiteral("id"), QStringLiteral("kind"),
        QStringLiteral("content_hash"), QStringLiteral("preview_text"),
        QStringLiteral("search_text_length"), QStringLiteral("content"),
        QStringLiteral("thumbnail"), QStringLiteral("content_size"),
        QStringLiteral("source_application"), QStringLiteral("created_at"),
        QStringLiteral("updated_at"), QStringLiteral("is_pinned")};
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(clipboard_items)"))) return false;
    QStringList found;
    while (query.next()) found.push_back(query.value(1).toString());
    for (const auto &column : required) {
        if (!found.contains(column)) return false;
    }
    return (schema < 5 || found.contains(QStringLiteral("format_label")))
        && (schema < 6 || found.contains(QStringLiteral("open_targets")));
}

QString escapeLike(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    value.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return value;
}

QString searchClause(const QString &text, int kind, const std::optional<HistoryCursor> &cursor,
                     bool includeUnpinnedOnly = false, const std::optional<QString> &source = std::nullopt) {
    QString sql = QStringLiteral(" WHERE 1=1");
    if (source) sql += QStringLiteral(" AND COALESCE(fts.source_application,'')=:source COLLATE NOCASE");
    if (includeUnpinnedOnly) sql += QStringLiteral(" AND item.is_pinned=0");
    // 收藏复用旧置顶标记，不改变记录的文本、图片或文件类型。
    if (kind == 3) sql += QStringLiteral(" AND item.is_pinned=1");
    else if (kind >= 0) sql += QStringLiteral(" AND item.kind=:kind");
    const auto term = text.trimmed();
    if (!term.isEmpty()) {
        if (term.size() >= 3) {
            sql += QStringLiteral(" AND clipboard_items_fts MATCH :match");
        } else {
            sql += QStringLiteral(" AND (fts.search_text LIKE :term ESCAPE '\\' COLLATE NOCASE"
                                  " OR fts.source_application LIKE :term ESCAPE '\\' COLLATE NOCASE)");
        }
    }
    if (cursor) {
        sql += QStringLiteral(" AND (item.updated_at<:updated"
                              " OR (item.updated_at=:updated AND item.storage_id<:storage))");
    }
    return sql;
}

void bindSearch(QSqlQuery &query, const QString &text, int kind,
                const std::optional<HistoryCursor> &cursor, const std::optional<QString> &source = std::nullopt) {
    if (source) query.bindValue(QStringLiteral(":source"), source->isEmpty() ? QStringLiteral("") : *source);
    if (kind >= 0 && kind < 3) query.bindValue(QStringLiteral(":kind"), kind);
    const auto term = text.trimmed();
    if (!term.isEmpty()) {
        if (term.size() >= 3) {
            QString phrase = term;
            phrase.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            query.bindValue(QStringLiteral(":match"), QStringLiteral("{search_text source_application} : \"")
                            + phrase + QStringLiteral("\""));
        } else {
            query.bindValue(QStringLiteral(":term"), QStringLiteral("%") + escapeLike(term) + QStringLiteral("%"));
        }
    }
    if (cursor) {
        query.bindValue(QStringLiteral(":updated"), cursor->updatedAt);
        query.bindValue(QStringLiteral(":storage"), cursor->storageId);
    }
}

} // namespace

HistoryStore::HistoryStore(QString path) : path_(std::move(path)) {}

QString HistoryStore::initialize() const {
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return QStringLiteral("无法创建本地数据目录");
    Connection connection(path_);
    if (!connection.isOpen()) return connection.error();

    int version = 0;
    QSqlQuery versionQuery(connection.database());
    if (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) || !versionQuery.next()) {
        return versionQuery.lastError().text();
    }
    version = versionQuery.value(0).toInt();
    versionQuery.finish();
    if (version != 0 && version != 4 && version != 5 && version != CurrentSchemaVersion) {
        return QStringLiteral("历史数据库版本不兼容，已保留原文件：%1").arg(version);
    }
    QSqlQuery existingQuery(connection.database());
    if (!existingQuery.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='clipboard_items'"))
        || !existingQuery.next()) return existingQuery.lastError().text();
    const bool hasItems = existingQuery.value(0).toInt() != 0;
    // 结束读取游标后才能切换 journal_mode，否则新数据库会被隐式读事务阻塞。
    existingQuery.finish();
    if (hasItems && !hasRequiredColumns(connection.database(), version)) {
        return QStringLiteral("历史数据库结构不兼容，已保留原文件");
    }

    QString error;
    const QStringList statements{
        QStringLiteral("PRAGMA auto_vacuum=INCREMENTAL"),
        QStringLiteral("PRAGMA journal_mode=WAL"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS clipboard_items ("
            "storage_id INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT NOT NULL UNIQUE,kind INTEGER NOT NULL,"
            "content_hash TEXT NOT NULL UNIQUE,preview_text BLOB NOT NULL,search_text_length INTEGER NOT NULL,"
            "content BLOB NOT NULL,thumbnail BLOB NULL,content_size INTEGER NOT NULL,source_application BLOB NULL,"
            "created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,is_pinned INTEGER NOT NULL,"
            "format_label TEXT NULL,open_targets BLOB NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS ix_clipboard_items_order ON clipboard_items(is_pinned DESC,updated_at DESC,storage_id DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS ix_clipboard_items_kind_order ON clipboard_items(kind,is_pinned DESC,updated_at DESC,storage_id DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS ix_clipboard_items_recent ON clipboard_items(updated_at DESC,storage_id DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS ix_clipboard_items_kind_recent ON clipboard_items(kind,updated_at DESC,storage_id DESC)"),
        QStringLiteral("CREATE VIRTUAL TABLE IF NOT EXISTS clipboard_items_fts USING fts5(search_text,source_application,tokenize='trigram')"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS clipboard_image_previews(storage_id INTEGER PRIMARY KEY,content BLOB NOT NULL)"),
        QStringLiteral("CREATE TRIGGER IF NOT EXISTS delete_clipboard_image_preview AFTER DELETE ON clipboard_items BEGIN DELETE FROM clipboard_image_previews WHERE storage_id=OLD.storage_id; END")};
    for (const auto &statement : statements) {
        if (!execute(connection.database(), statement, &error)) return error;
    }
    // 只添加可空元数据列，不在启动时读取旧记录正文；版本与结构一起提交。
    auto &database = connection.database();
    if (!database.transaction()) return connection.error();
    if ((!hasRequiredColumns(database, 5)
         && !execute(database, QStringLiteral("ALTER TABLE clipboard_items ADD COLUMN format_label TEXT NULL"), &error))
        || (!hasRequiredColumns(database, 6)
            && !execute(database, QStringLiteral("ALTER TABLE clipboard_items ADD COLUMN open_targets BLOB NULL"), &error))
        || !execute(database, QStringLiteral("PRAGMA user_version=6"), &error)) {
        database.rollback();
        return error;
    }
    if (!database.commit()) return connection.error();
    return {};
}

bool HistoryStore::isCurrentSchema(const QString &path, bool allowUpgrade) {
    if (!QFileInfo::exists(path)) return false;
    Connection connection(path);
    if (!connection.isOpen()) return false;
    QSqlQuery integrity(connection.database());
    if (!integrity.exec(QStringLiteral("PRAGMA quick_check")) || !integrity.next()
        || integrity.value(0).toString() != QStringLiteral("ok")) return false;
    integrity.finish();
    QSqlQuery version(connection.database());
    if (!version.exec(QStringLiteral("PRAGMA user_version")) || !version.next()) return false;
    const int schema = version.value(0).toInt();
    if ((schema != CurrentSchemaVersion && !(allowUpgrade && (schema == 4 || schema == 5)))
        || !hasRequiredColumns(connection.database(), schema)) return false;
    QSqlQuery searchTable(connection.database());
    return searchTable.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='clipboard_items_fts'"))
        && searchTable.next() && searchTable.value(0).toInt() == 1;
}

HistoryPage HistoryStore::search(const QString &text, int kind, const std::optional<HistoryCursor> &cursor,
                                int pageSize, const std::optional<QString> &source) const {
    HistoryPage page;
    Connection connection(path_);
    if (!connection.isOpen()) { page.error = connection.error(); return page; }

    const bool hasSearch = !text.trimmed().isEmpty() || source.has_value();
    const QString from = hasSearch
        ? QStringLiteral(" FROM clipboard_items AS item JOIN clipboard_items_fts AS fts ON fts.rowid=item.storage_id")
        : QStringLiteral(" FROM clipboard_items AS item");
    // 清空按钮只在第一页刷新时需要数量，翻页不重复统计。
    if (!cursor) {
        QSqlQuery unpinnedQuery(connection.database());
        unpinnedQuery.prepare(QStringLiteral("SELECT COUNT(*)") + from + searchClause(text, kind, std::nullopt, true, source));
        bindSearch(unpinnedQuery, text, kind, std::nullopt, source);
        if (!unpinnedQuery.exec() || !unpinnedQuery.next()) { page.error = unpinnedQuery.lastError().text(); return page; }
        page.unpinnedCount = unpinnedQuery.value(0).toInt();
    }

    QSqlQuery query(connection.database());
    const QString sql = QStringLiteral("SELECT item.storage_id,item.id,item.kind,item.preview_text,"
        "item.search_text_length,item.source_application,item.created_at,item.updated_at,item.is_pinned,item.content_size,item.format_label")
        + from + searchClause(text, kind, cursor, false, source)
        + QStringLiteral(" ORDER BY item.updated_at DESC,item.storage_id DESC LIMIT :limit");
    query.prepare(sql);
    bindSearch(query, text, kind, cursor, source);
    query.bindValue(QStringLiteral(":limit"), pageSize + 1);
    if (!query.exec()) { page.error = query.lastError().text(); return page; }
    while (query.next()) {
        const auto decryptedPreview = unprotect(query.value(3).toByteArray());
        if (!decryptedPreview) { page.error = QStringLiteral("无法解密历史记录预览"); page.items.clear(); return page; }
        HistoryItem item;
        item.storageId = query.value(0).toLongLong();
        item.id = query.value(1).toString();
        item.kind = query.value(2).toInt();
        item.preview = QString::fromUtf8(*decryptedPreview);
        item.searchTextLength = query.value(4).toInt();
        if (!query.value(5).isNull()) {
            const auto source = unprotect(query.value(5).toByteArray());
            if (source) item.sourceApplication = QString::fromUtf8(*source);
        }
        item.createdAt = query.value(6).toLongLong();
        item.updatedAt = query.value(7).toLongLong();
        item.pinned = query.value(8).toBool();
        item.contentSize = query.value(9).toLongLong();
        item.formatLabel = query.value(10).toString();
        page.items.push_back(std::move(item));
    }
    if (page.items.size() > pageSize) {
        page.items.removeLast();
        const auto &last = page.items.last();
        page.next = HistoryCursor{last.storageId, last.updatedAt, last.pinned};
    }
    return page;
}

QPair<QStringList, QString> HistoryStore::sourceApplications() const {
    Connection connection(path_);
    if (!connection.isOpen()) return {{}, connection.error()};
    QSqlQuery query(connection.database());
    // 复用搜索索引中的来源，不逐条解密内容；按最近记录排序，进程名忽略大小写去重。
    if (!query.exec(QStringLiteral("SELECT COALESCE(fts.source_application,'') AS source"
        " FROM clipboard_items AS item JOIN clipboard_items_fts AS fts ON fts.rowid=item.storage_id"
        " GROUP BY source COLLATE NOCASE ORDER BY MAX(item.updated_at) DESC,source COLLATE NOCASE")))
        return {{}, query.lastError().text()};
    QStringList sources;
    while (query.next()) sources.append(query.value(0).toString());
    return {sources, {}};
}

QString HistoryStore::save(const ClipboardCapture &capture, HistoryItem *saved) const {
    if (capture.content.isEmpty() || capture.searchText.isEmpty()) return QStringLiteral("剪贴板内容为空");
    const auto metadata = ClipboardContent::contentMetadata(capture.content, capture.kind);
    const QString &format = metadata.formatLabel;
    const auto targets = protectTargets(metadata);
    if (!targets) return QStringLiteral("DPAPI 加密失败");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const char kindByte = static_cast<char>(capture.kind);
    hash.addData(QByteArrayView(&kindByte, 1));
    hash.addData(capture.content);
    const QString contentHash = QString::fromLatin1(hash.result().toHex().toUpper());

    auto preview = capture.searchText.left(160);
    preview.replace(QLatin1Char('\r'), QLatin1Char(' '));
    preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (capture.searchText.size() > 160) preview.append(QChar(0x2026));
    const auto protectedPreview = protect(preview.toUtf8());
    const auto protectedSource = capture.sourceApplication.isEmpty()
        ? std::optional<QByteArray>{} : protect(capture.sourceApplication.toUtf8());
    if (!protectedPreview || (!capture.sourceApplication.isEmpty() && !protectedSource)) {
        return QStringLiteral("DPAPI 加密失败");
    }

    Connection connection(path_);
    if (!connection.isOpen()) return connection.error();
    auto &database = connection.database();
    if (!database.transaction()) return connection.error();
    QSqlQuery lookup(database);
    lookup.prepare(QStringLiteral("SELECT storage_id,id,created_at,is_pinned FROM clipboard_items WHERE content_hash=:hash"));
    lookup.bindValue(QStringLiteral(":hash"), contentHash);
    if (!lookup.exec()) { database.rollback(); return lookup.lastError().text(); }
    const bool exists = lookup.next();
    const qint64 storageId = exists ? lookup.value(0).toLongLong() : 0;
    const QString id = exists ? lookup.value(1).toString()
                              : QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 createdAt = exists ? lookup.value(2).toLongLong() : QDateTime::currentMSecsSinceEpoch();
    const bool pinned = exists && lookup.value(3).toBool();
    lookup.finish();

    QSqlQuery write(database);
    if (exists) {
        write.prepare(QStringLiteral("UPDATE clipboard_items SET preview_text=:preview,search_text_length=:length,"
            "content_size=:size,source_application=:source,updated_at=:now,format_label=:format,"
            "open_targets=:targets WHERE storage_id=:storage"));
        write.bindValue(QStringLiteral(":storage"), storageId);
    } else {
        const auto protectedContent = protect(capture.content);
        const auto protectedThumbnail = capture.thumbnail.isEmpty()
            ? std::optional<QByteArray>{} : protect(capture.thumbnail);
        if (!protectedContent || (!capture.thumbnail.isEmpty() && !protectedThumbnail)) {
            database.rollback();
            return QStringLiteral("DPAPI 加密失败");
        }
        write.prepare(QStringLiteral("INSERT INTO clipboard_items(id,kind,content_hash,preview_text,search_text_length,"
            "content,thumbnail,content_size,source_application,created_at,updated_at,is_pinned,format_label,open_targets) "
            "VALUES(:id,:kind,:hash,:preview,:length,:content,:thumbnail,:size,:source,:created,:now,0,:format,:targets)"));
        write.bindValue(QStringLiteral(":id"), id);
        write.bindValue(QStringLiteral(":kind"), capture.kind);
        write.bindValue(QStringLiteral(":hash"), contentHash);
        write.bindValue(QStringLiteral(":content"), *protectedContent);
        write.bindValue(QStringLiteral(":thumbnail"), protectedThumbnail ? QVariant(*protectedThumbnail) : QVariant());
        write.bindValue(QStringLiteral(":created"), createdAt);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    write.bindValue(QStringLiteral(":format"), format);
    write.bindValue(QStringLiteral(":targets"), *targets);
    write.bindValue(QStringLiteral(":preview"), *protectedPreview);
    write.bindValue(QStringLiteral(":length"), capture.searchText.size());
    write.bindValue(QStringLiteral(":size"), capture.content.size());
    write.bindValue(QStringLiteral(":source"), protectedSource ? QVariant(*protectedSource) : QVariant());
    write.bindValue(QStringLiteral(":now"), now);
    if (!write.exec()) { database.rollback(); return write.lastError().text(); }

    qint64 savedStorageId = storageId;
    if (!exists) {
        QSqlQuery lastId(database);
        if (!lastId.exec(QStringLiteral("SELECT last_insert_rowid()")) || !lastId.next()) {
            database.rollback(); return lastId.lastError().text();
        }
        savedStorageId = lastId.value(0).toLongLong();
    }
    if (capture.kind == 1 && !capture.imagePreview.isEmpty()) {
        const auto protectedImagePreview = protect(capture.imagePreview);
        if (!protectedImagePreview) { database.rollback(); return QStringLiteral("DPAPI 加密失败"); }
        QSqlQuery image(database);
        image.prepare(QStringLiteral("INSERT OR REPLACE INTO clipboard_image_previews(storage_id,content) VALUES(:id,:content)"));
        image.bindValue(QStringLiteral(":id"), savedStorageId);
        image.bindValue(QStringLiteral(":content"), *protectedImagePreview);
        if (!image.exec()) { database.rollback(); return image.lastError().text(); }
    }
    QSqlQuery removeIndex(database);
    removeIndex.prepare(QStringLiteral("DELETE FROM clipboard_items_fts WHERE rowid=:id"));
    removeIndex.bindValue(QStringLiteral(":id"), savedStorageId);
    if (!removeIndex.exec()) { database.rollback(); return removeIndex.lastError().text(); }
    QSqlQuery index(database);
    index.prepare(QStringLiteral("INSERT INTO clipboard_items_fts(rowid,search_text,source_application) VALUES(:id,:text,:source)"));
    index.bindValue(QStringLiteral(":id"), savedStorageId);
    index.bindValue(QStringLiteral(":text"), capture.searchText);
    index.bindValue(QStringLiteral(":source"), capture.sourceApplication);
    if (!index.exec()) { database.rollback(); return index.lastError().text(); }
    if (!database.commit()) return connection.error();

    if (saved) {
        saved->storageId = savedStorageId;
        saved->id = id;
        saved->kind = capture.kind;
        saved->preview = preview;
        saved->searchTextLength = capture.searchText.size();
        saved->sourceApplication = capture.sourceApplication;
        saved->createdAt = createdAt;
        saved->updatedAt = now;
        saved->pinned = pinned;
        saved->contentSize = capture.content.size();
        saved->formatLabel = format;
    }
    return {};
}

QString HistoryStore::replaceText(const QString &id, const QByteArray &content, const QString &text) const {
    if (id.isEmpty() || content.isEmpty() || text.isEmpty()) return QStringLiteral("ReplaceRecordInvalid");
    const auto metadata = ClipboardContent::contentMetadata(content, 0);
    const auto targets = protectTargets(metadata);
    if (!targets) return QStringLiteral("DPAPI 加密失败");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const char kind = 0;
    hash.addData(QByteArrayView(&kind, 1));
    hash.addData(content);
    const QString contentHash = QString::fromLatin1(hash.result().toHex().toUpper());
    auto preview = text.left(160);
    preview.replace(QLatin1Char('\r'), QLatin1Char(' '));
    preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (text.size() > 160) preview.append(QChar(0x2026));
    const auto protectedContent = protect(content);
    const auto protectedPreview = protect(preview.toUtf8());
    if (!protectedContent || !protectedPreview) return QStringLiteral("DPAPI 加密失败");
    Connection connection(path_);
    if (!connection.isOpen()) return connection.error();
    auto &database = connection.database();
    if (!database.transaction()) return connection.error();
    QSqlQuery lookup(database);
    lookup.prepare(QStringLiteral("SELECT storage_id FROM clipboard_items WHERE id=:id AND kind=0"));
    lookup.bindValue(QStringLiteral(":id"), id);
    if (!lookup.exec()) { database.rollback(); return lookup.lastError().text(); }
    if (!lookup.next()) { database.rollback(); return QStringLiteral("ReplaceRecordMissing"); }
    const qint64 storageId = lookup.value(0).toLongLong();
    lookup.finish();
    QSqlQuery duplicate(database);
    duplicate.prepare(QStringLiteral("SELECT 1 FROM clipboard_items WHERE content_hash=:hash AND id<>:id"));
    duplicate.bindValue(QStringLiteral(":hash"), contentHash);
    duplicate.bindValue(QStringLiteral(":id"), id);
    if (!duplicate.exec()) { database.rollback(); return duplicate.lastError().text(); }
    if (duplicate.next()) { database.rollback(); return QStringLiteral("ReplaceRecordDuplicate"); }
    duplicate.finish();
    // 内容、加密摘要和全文索引在同一事务内替换，失败时不留下半更新状态。
    QSqlQuery write(database);
    write.prepare(QStringLiteral("UPDATE clipboard_items SET content=:content,content_hash=:hash,preview_text=:preview,"
        "search_text_length=:length,content_size=:size,format_label=:format,open_targets=:targets WHERE storage_id=:storage"));
    write.bindValue(QStringLiteral(":format"), metadata.formatLabel);
    write.bindValue(QStringLiteral(":targets"), *targets);
    write.bindValue(QStringLiteral(":content"), *protectedContent);
    write.bindValue(QStringLiteral(":hash"), contentHash);
    write.bindValue(QStringLiteral(":preview"), *protectedPreview);
    write.bindValue(QStringLiteral(":length"), text.size());
    write.bindValue(QStringLiteral(":size"), content.size());
    write.bindValue(QStringLiteral(":storage"), storageId);
    if (!write.exec()) { database.rollback(); return write.lastError().text(); }
    QSqlQuery index(database);
    index.prepare(QStringLiteral("UPDATE clipboard_items_fts SET search_text=:text WHERE rowid=:storage"));
    index.bindValue(QStringLiteral(":text"), text);
    index.bindValue(QStringLiteral(":storage"), storageId);
    if (!index.exec()) { database.rollback(); return index.lastError().text(); }
    return database.commit() ? QString{} : connection.error();
}

ClipboardContent::ContentMetadata HistoryStore::contentMetadata(const QString &id) const {
    Connection connection(path_);
    if (!connection.isOpen()) return {};
    auto &database = connection.database();
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT format_label,open_targets,kind,content_hash,"
                                 "CASE WHEN open_targets IS NULL THEN content END FROM clipboard_items WHERE id=:id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec() || !query.next()) return {};
    if (!query.value(1).isNull()) {
        const auto targets = unprotect(query.value(1).toByteArray());
        if (!targets) return {};
        const auto document = QJsonDocument::fromJson(*targets);
        if (!document.isArray()) return {};
        ClipboardContent::ContentMetadata metadata;
        metadata.formatLabel = query.value(0).toString();
        for (const auto &entry : document.array())
            if (entry.isString()) metadata.openTargets.append(entry.toString());
        return metadata;
    }
    const int kind = query.value(2).toInt();
    const QString hash = query.value(3).toString();
    const auto plain = unprotect(query.value(4).toByteArray());
    query.finish();
    if (!plain) return {};
    const auto metadata = ClipboardContent::contentMetadata(*plain, kind);
    const auto targets = protectTargets(metadata);
    if (!targets) return {};
    // 与编辑竞争时不覆盖新版标签，避免把旧正文的识别结果写回新内容。
    QSqlQuery write(database);
    write.prepare(QStringLiteral("UPDATE clipboard_items SET format_label=:format,open_targets=:targets "
                                 "WHERE id=:id AND content_hash=:hash AND open_targets IS NULL"));
    write.bindValue(QStringLiteral(":format"), metadata.formatLabel);
    write.bindValue(QStringLiteral(":targets"), *targets);
    write.bindValue(QStringLiteral(":id"), id);
    write.bindValue(QStringLiteral(":hash"), hash);
    if (!write.exec() || write.numRowsAffected() != 1) return {};
    return metadata;
}

QByteArray HistoryStore::content(const QString &id, bool preferPreview) const {
    Connection connection(path_);
    if (!connection.isOpen()) return {};
    QSqlQuery query(connection.database());
    query.prepare(preferPreview
        ? QStringLiteral("SELECT COALESCE(preview.content,item.content) FROM clipboard_items AS item "
                         "LEFT JOIN clipboard_image_previews AS preview ON preview.storage_id=item.storage_id WHERE item.id=:id")
        : QStringLiteral("SELECT content FROM clipboard_items WHERE id=:id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec() || !query.next()) return {};
    const auto value = unprotect(query.value(0).toByteArray());
    return value ? *value : QByteArray{};
}

QByteArray HistoryStore::thumbnail(const QString &id) const {
    Connection connection(path_);
    if (!connection.isOpen()) return {};
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("SELECT thumbnail FROM clipboard_items WHERE id=:id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec() || !query.next() || query.value(0).isNull()) return {};
    const auto value = unprotect(query.value(0).toByteArray());
    return value ? *value : QByteArray{};
}

QString HistoryStore::setPinned(const QString &id, bool pinned) const {
    Connection connection(path_);
    if (!connection.isOpen()) return connection.error();
    QSqlQuery query(connection.database());
    // 收藏不刷新复制时间，避免在普通历史中改变记录的位置。
    query.prepare(QStringLiteral("UPDATE clipboard_items SET is_pinned=:pinned WHERE id=:id"));
    query.bindValue(QStringLiteral(":pinned"), pinned ? 1 : 0);
    query.bindValue(QStringLiteral(":id"), id);
    return query.exec() ? QString{} : query.lastError().text();
}

QString HistoryStore::touch(const QString &id) const {
    Connection connection(path_);
    if (!connection.isOpen()) return connection.error();
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("UPDATE clipboard_items SET updated_at=:now WHERE id=:id"));
    query.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    query.bindValue(QStringLiteral(":id"), id);
    return query.exec() ? QString{} : query.lastError().text();
}

QString HistoryStore::remove(const QString &id) const {
    Connection connection(path_);
    if (!connection.isOpen()) return connection.error();
    auto &database = connection.database();
    if (!database.transaction()) return connection.error();
    QSqlQuery lookup(database);
    lookup.prepare(QStringLiteral("SELECT storage_id FROM clipboard_items WHERE id=:id"));
    lookup.bindValue(QStringLiteral(":id"), id);
    if (!lookup.exec()) { database.rollback(); return lookup.lastError().text(); }
    if (!lookup.next()) { database.rollback(); return {}; }
    const qint64 storageId = lookup.value(0).toLongLong();
    lookup.finish();
    QSqlQuery index(database);
    index.prepare(QStringLiteral("DELETE FROM clipboard_items_fts WHERE rowid=:storage"));
    index.bindValue(QStringLiteral(":storage"), storageId);
    if (!index.exec()) { database.rollback(); return index.lastError().text(); }
    QSqlQuery item(database);
    item.prepare(QStringLiteral("DELETE FROM clipboard_items WHERE storage_id=:storage"));
    item.bindValue(QStringLiteral(":storage"), storageId);
    if (!item.exec()) { database.rollback(); return item.lastError().text(); }
    if (!database.commit()) return connection.error();
    return {};
}

int HistoryStore::removeMatching(const QString &text, int kind, QString *error,
                                 const std::optional<QString> &source) const {
    if (error) error->clear();
    Connection connection(path_);
    if (!connection.isOpen()) { if (error) *error = connection.error(); return 0; }
    auto &database = connection.database();
    if (!database.transaction()) { if (error) *error = connection.error(); return 0; }
    const bool hasSearch = !text.trimmed().isEmpty() || source.has_value();
    const QString from = hasSearch
        ? QStringLiteral(" FROM clipboard_items AS item JOIN clipboard_items_fts AS fts ON fts.rowid=item.storage_id")
        : QStringLiteral(" FROM clipboard_items AS item");
    QSqlQuery select(database);
    select.prepare(QStringLiteral("SELECT item.storage_id") + from + searchClause(text, kind, std::nullopt, true, source));
    bindSearch(select, text, kind, std::nullopt, source);
    if (!select.exec()) { if (error) *error = select.lastError().text(); database.rollback(); return 0; }
    QVector<qint64> ids;
    while (select.next()) ids.push_back(select.value(0).toLongLong());
    select.finish();

    QSqlQuery index(database);
    index.prepare(QStringLiteral("DELETE FROM clipboard_items_fts WHERE rowid=:storage"));
    QSqlQuery item(database);
    item.prepare(QStringLiteral("DELETE FROM clipboard_items WHERE storage_id=:storage"));
    for (const auto storageId : ids) {
        index.bindValue(QStringLiteral(":storage"), storageId);
        item.bindValue(QStringLiteral(":storage"), storageId);
        if (!index.exec() || !item.exec()) {
            if (error) *error = !index.lastError().isValid() ? item.lastError().text() : index.lastError().text();
            database.rollback();
            return 0;
        }
        index.finish();
        item.finish();
    }
    if (!database.commit()) { if (error) *error = connection.error(); return 0; }
    return ids.size();
}

int HistoryStore::count() const {
    Connection connection(path_);
    if (!connection.isOpen()) return -1;
    QSqlQuery query(connection.database());
    return query.exec(QStringLiteral("SELECT COUNT(*) FROM clipboard_items")) && query.next()
        ? query.value(0).toInt() : -1;
}

QString HistoryStore::cleanup(int retentionDays, int maxEntries) const {
    if (retentionDays <= 0 || maxEntries <= 0) return QStringLiteral("历史清理参数必须大于零");
    Connection connection(path_);
    if (!connection.isOpen()) return connection.error();
    auto &database = connection.database();
    if (!database.transaction()) return connection.error();
    QSqlQuery select(database);
    // 使用排序索引筛选过期和超额记录，避免每次保存都对全表计算窗口排名。
    select.prepare(QStringLiteral("SELECT storage_id FROM clipboard_items WHERE is_pinned=0 AND updated_at<:cutoff "
        "UNION SELECT storage_id FROM (SELECT storage_id FROM clipboard_items WHERE is_pinned=0 "
        "ORDER BY updated_at DESC,storage_id DESC LIMIT -1 OFFSET :maximum)"));
    select.bindValue(QStringLiteral(":cutoff"), QDateTime::currentMSecsSinceEpoch()
                     - static_cast<qint64>(retentionDays) * 24 * 60 * 60 * 1000);
    select.bindValue(QStringLiteral(":maximum"), maxEntries);
    if (!select.exec()) { database.rollback(); return select.lastError().text(); }
    QVector<qint64> ids;
    while (select.next()) ids.push_back(select.value(0).toLongLong());
    select.finish();
    QSqlQuery index(database);
    index.prepare(QStringLiteral("DELETE FROM clipboard_items_fts WHERE rowid=:storage"));
    QSqlQuery item(database);
    item.prepare(QStringLiteral("DELETE FROM clipboard_items WHERE storage_id=:storage"));
    for (const auto storageId : ids) {
        index.bindValue(QStringLiteral(":storage"), storageId);
        item.bindValue(QStringLiteral(":storage"), storageId);
        if (!index.exec() || !item.exec()) {
            const auto message = index.lastError().isValid() ? index.lastError().text() : item.lastError().text();
            database.rollback();
            return message;
        }
        index.finish();
        item.finish();
    }
    if (!database.commit()) return connection.error();
    return {};
}

QString HistoryStore::compact(bool full) const {
    Connection connection(path_);
    if (!connection.isOpen()) return connection.error();
    QString error;
    QSqlQuery checkpoint(connection.database());
    // 日常维护不等待读事务结束；完整压缩才要求截断 WAL。
    if (!checkpoint.exec(full ? QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")
                              : QStringLiteral("PRAGMA wal_checkpoint(PASSIVE)")) || !checkpoint.next())
        return checkpoint.lastError().isValid() ? checkpoint.lastError().text() : QStringLiteral("无法确认数据库检查点状态");
    // SQLite 的 busy 通过结果行返回，exec 成功不代表 WAL 已全部写回主数据库。
    if (full && checkpoint.value(0).toInt() != 0) return QStringLiteral("数据库正在使用中，请稍后重试");
    checkpoint.finish();
    if (full) {
        if (!execute(connection.database(), QStringLiteral("VACUUM"), &error)) return error;
    } else {
        auto &database = connection.database();
        if (!database.transaction()) return connection.error();
        QSqlQuery freePages(database);
        if (!freePages.exec(QStringLiteral("PRAGMA freelist_count")) || !freePages.next()) {
            const QString message = freePages.lastError().text();
            database.rollback();
            return message;
        }
        const int pages = qMin(200, freePages.value(0).toInt());
        freePages.finish();
        // Qt 驱动不会完整遍历该 PRAGMA 的无列结果，逐页执行并合并为一个限量事务。
        for (int i = 0; i < pages; ++i) {
            if (!execute(database, QStringLiteral("PRAGMA incremental_vacuum(1)"), &error)) {
                database.rollback();
                return error;
            }
        }
        if (!database.commit()) return connection.error();
    }
    return execute(connection.database(), QStringLiteral("PRAGMA optimize"), &error) ? QString{} : error;
}
