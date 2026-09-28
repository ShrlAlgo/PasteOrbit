#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

#include <optional>

struct HistoryItem {
    qint64 storageId = 0;
    QString id;
    int kind = 0;
    QString preview;
    int searchTextLength = 0;
    QString sourceApplication;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    bool pinned = false;
    qint64 contentSize = 0;
};

struct HistoryCursor {
    qint64 storageId = 0;
    qint64 updatedAt = 0;
    bool pinned = false;
};

struct HistoryPage {
    QVector<HistoryItem> items;
    std::optional<HistoryCursor> next;
    int totalCount = 0;
    int unpinnedCount = 0;
    QString error;
};

struct ClipboardCapture {
    int kind = 0;
    QString searchText;
    QString sourceApplication;
    QByteArray content;
    QByteArray thumbnail;
    QByteArray imagePreview;
    QString text;
    QString html;
    QByteArray rtf;
};

class HistoryStore {
public:
    explicit HistoryStore(QString path);

    QString initialize() const;
    static bool isCurrentSchema(const QString &path);
    HistoryPage search(const QString &text, int kind, const std::optional<HistoryCursor> &cursor,
                       int pageSize = 30) const;
    QString save(const ClipboardCapture &capture, HistoryItem *saved = nullptr) const;
    QByteArray content(const QString &id, bool preferPreview = false) const;
    QByteArray thumbnail(const QString &id) const;
    QString setPinned(const QString &id, bool pinned) const;
    QString touch(const QString &id) const;
    QString remove(const QString &id) const;
    int removeMatching(const QString &text, int kind, QString *error = nullptr) const;
    int count() const;
    QString cleanup(int retentionDays, int maxEntries) const;
    QString compact(bool full = false) const;

    const QString &path() const { return path_; }

private:
    QString path_;
};
