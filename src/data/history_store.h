#pragma once

#include "clipboard_content.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QPair>
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
    int groupId = 0;
    qint64 contentSize = 0;
    QString formatLabel;
    // 仅用于当前会话的卡片提示，不写入数据库。
    bool unavailable = false;
};

struct HistoryGroup {
    int id = 0;
    QString name;
    QString icon;
};

struct HistoryCursor {
    qint64 storageId = 0;
    qint64 updatedAt = 0;
    bool pinned = false;
};

struct HistoryPage {
    QVector<HistoryItem> items;
    std::optional<HistoryCursor> next;
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
    // 自定义分组筛选复用 kind 参数，固定类型仍使用 -1 到 3。
    static constexpr int GroupKindBase = 1000;

    explicit HistoryStore(QString path);

    QString initialize() const;
    static bool isCurrentSchema(const QString &path, bool allowUpgrade = false);
    HistoryPage search(const QString &text, int kind, const std::optional<HistoryCursor> &cursor,
                       int pageSize = 30, const std::optional<QString> &source = std::nullopt) const;
    QPair<QStringList, QString> sourceApplications() const;
    QVector<HistoryGroup> groups() const;
    QString saveGroup(HistoryGroup &group) const;
    QString removeGroup(int id) const;
    QString setGroup(const QString &id, int groupId) const;
    QString save(const ClipboardCapture &capture, HistoryItem *saved = nullptr) const;
    // 仅替换文本内容；记录标识、时间、来源和收藏状态保持不变。
    QString replaceText(const QString &id, const QByteArray &content, const QString &text) const;
    QByteArray content(const QString &id, bool preferPreview = false) const;
    QByteArray thumbnail(const QString &id) const;
    // 旧记录按需补算格式和打开目标；仅在内容未变化时回写。
    ClipboardContent::ContentMetadata contentMetadata(const QString &id) const;
    QString setPinned(const QString &id, bool pinned) const;
    QString touch(const QString &id) const;
    QString remove(const QString &id) const;
    int removeMatching(const QString &text, int kind, QString *error = nullptr,
                       const std::optional<QString> &source = std::nullopt) const;
    int count() const;
    QString cleanup(int retentionDays, int maxEntries) const;
    QString compact(bool full = false) const;

    const QString &path() const { return path_; }

private:
    QString path_;
};
