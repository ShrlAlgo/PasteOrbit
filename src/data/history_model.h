#pragma once

#include "history_store.h"

#include <QAbstractListModel>
#include <QAtomicInteger>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QSharedPointer>
#include <QSet>

struct ThumbnailResult {
    QString id;
    QByteArray bytes;
};

class HistoryModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { IdRole = Qt::UserRole + 1, KindRole, PreviewRole, SourceRole, UpdatedRole,
                PinnedRole, SizeRole, TextLengthRole, ThumbnailRole, QuickPasteRole };

    explicit HistoryModel(QString databasePath, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    void setFirstPage(const HistoryPage &page);
    void appendPage(const HistoryPage &page);
    const HistoryItem *itemAt(int row) const;
    int rowForId(const QString &id) const;
    int quickPasteRow(int number) const;
    void setQuickPasteEnabled(bool enabled);
    void setPinned(const QString &id, bool pinned);
    void remove(const QString &id);
    void setVisibleRows(int first, int last);
    void releaseThumbnails();

private:
    QString databasePath_;
    QVector<HistoryItem> items_;
    QHash<QString, QImage> thumbnails_;
    QSet<QString> visibleIds_;
    QSet<QString> loadingIds_;
    QSharedPointer<QAtomicInteger<int>> thumbnailGeneration_ = QSharedPointer<QAtomicInteger<int>>::create(0);
    bool quickPasteEnabled_ = true;
};
