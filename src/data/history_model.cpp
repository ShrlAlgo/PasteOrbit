#include "history_model.h"
#include "clipboard_content.h"

#include <QCache>
#include <QBuffer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QtConcurrent>
#include <windows.h>
#include <shellapi.h>

#include <utility>

namespace {
QImage fileTypeIcon(const QByteArray &content) {
    const auto paths = QJsonDocument::fromJson(content).array();
    // 多文件保留组合图标；单文件只按扩展名查关联，不访问可能已失效的本地或网络路径。
    if (paths.size() != 1 || !paths.first().isString()) return {};
    auto suffix = QFileInfo(paths.first().toString()).suffix().toLower();
    if (suffix.size() > 32) suffix.clear();
    const auto key = suffix.isEmpty() ? QString{} : QStringLiteral(".") + suffix;
    static QMutex mutex;
    static QCache<QString, QImage> cache(256); // 成本按 KiB 计算，缓存最多约 256 KiB。
    // 只在后台持锁，同类文件并发加载时也只查一次 Shell，避免缓存重复分配。
    QMutexLocker lock(&mutex);
    if (const auto *cached = cache.object(key)) return *cached;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};
    const auto releaseCom = qScopeGuard([&] { if (SUCCEEDED(initialized)) CoUninitialize(); });
    // Shell 查询在工作线程执行，取到的 HICON 转为图像后立即释放。
    const auto query = [](const QString &extension) {
        const auto name = QStringLiteral("PasteOrbit") + extension;
        SHFILEINFOW info{};
        QImage image;
        if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(name.utf16()), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                           SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon)
            image = QImage::fromHICON(info.hIcon);
        if (info.hIcon) DestroyIcon(info.hIcon);
        return image;
    };
    const auto fallback = QStringLiteral(".pasteorbit_unknown");
    auto image = query(key.isEmpty() ? fallback : key);
    if (image.isNull() && !key.isEmpty()) image = query(fallback);
    const int cost = qMax(1, int((image.sizeInBytes() + 1023) / 1024));
    cache.insert(key, new QImage(image), cost);
    return image;
}
}

HistoryModel::HistoryModel(QString databasePath, QObject *parent)
    : QAbstractListModel(parent), databasePath_(std::move(databasePath)) {}

HistoryModel::~HistoryModel() {
    cancelThumbnailLoads();
}

void HistoryModel::cancelThumbnailLoads() {
    for (const auto &cancelled : std::as_const(thumbnailLoads_)) cancelled->storeRelaxed(true);
    thumbnailLoads_.clear();
}

int HistoryModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : items_.size();
}

QVariant HistoryModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size()) return {};
    const auto &item = items_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case PreviewRole: return item.preview;
    case IdRole: return item.id;
    case KindRole: return item.kind;
    case SourceRole: return item.sourceApplication;
    case UpdatedRole: return item.updatedAt;
    case PinnedRole: return item.pinned;
    case SizeRole: return item.contentSize;
    case TextLengthRole: return item.searchTextLength;
    case ThumbnailRole: return thumbnails_.value(item.id);
    case FormatRole: return formats_.value(item.id);
    case QuickPasteRole: {
        // 收藏分类同样支持数字粘贴，编号与当前列表的可见顺序一致。
        if (!quickPasteEnabled_ || index.row() >= 9) return {};
        return index.row() + 1;
    }
    case Qt::SizeHintRole: return QSize(1, 94);
    default: return {};
    }
}

void HistoryModel::setFirstPage(const HistoryPage &page) {
    cancelThumbnailLoads();
    beginResetModel();
    items_ = page.items;
    thumbnails_.clear();
    formats_.clear();
    visibleIds_.clear();
    endResetModel();
}

void HistoryModel::appendPage(const HistoryPage &page) {
    QSet<QString> known;
    known.reserve(items_.size());
    for (const auto &item : items_) known.insert(item.id);
    QVector<HistoryItem> additions;
    additions.reserve(page.items.size());
    for (const auto &item : page.items) if (!known.contains(item.id)) additions.push_back(item);
    if (additions.isEmpty()) return;
    const int first = items_.size();
    beginInsertRows({}, first, first + additions.size() - 1);
    for (auto &item : additions) items_.push_back(std::move(item));
    endInsertRows();
}

const HistoryItem *HistoryModel::itemAt(int row) const {
    return row >= 0 && row < items_.size() ? &items_.at(row) : nullptr;
}

int HistoryModel::rowForId(const QString &id) const {
    for (int i = 0; i < items_.size(); ++i) if (items_.at(i).id == id) return i;
    return -1;
}

int HistoryModel::quickPasteRow(int number) const {
    if (!quickPasteEnabled_ || number < 1 || number > 9 || number > items_.size()) return -1;
    return number - 1;
}

void HistoryModel::setQuickPasteEnabled(bool enabled) {
    if (quickPasteEnabled_ == enabled) return;
    quickPasteEnabled_ = enabled;
    if (!items_.isEmpty()) emit dataChanged(index(0), index(qMin(8, int(items_.size()) - 1)), {QuickPasteRole});
}

void HistoryModel::setPinned(const QString &id, bool pinned) {
    const int row = rowForId(id);
    if (row < 0 || items_[row].pinned == pinned) return;
    items_[row].pinned = pinned;
    // 收藏不再改变排序或数字编号，仅刷新当前卡片。
    emit dataChanged(index(row), index(row), {PinnedRole});
}

void HistoryModel::setText(const QString &id, const QString &text, qint64 contentSize, const QString &format) {
    const int row = rowForId(id);
    if (row < 0 || items_[row].kind != 0) return;
    auto &item = items_[row];
    item.preview = text.left(160).replace(QLatin1Char('\r'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (text.size() > 160) item.preview.append(QChar(0x2026));
    item.searchTextLength = text.size();
    item.contentSize = contentSize;
    if (const auto cancelled = thumbnailLoads_.take(id)) cancelled->storeRelaxed(true);
    formats_.insert(id, format);
    // 只更新当前卡片，不重置列表或改变用户的选择、滚动位置。
    emit dataChanged(index(row), index(row), {Qt::DisplayRole, PreviewRole, TextLengthRole, SizeRole, FormatRole});
}

void HistoryModel::remove(const QString &id) {
    const int row = rowForId(id);
    if (row < 0) return;
    beginRemoveRows({}, row, row);
    thumbnails_.remove(id);
    formats_.remove(id);
    visibleIds_.remove(id);
    if (const auto cancelled = thumbnailLoads_.take(id)) cancelled->storeRelaxed(true);
    items_.removeAt(row);
    endRemoveRows();
}

void HistoryModel::setVisibleRows(int first, int last) {
    QSet<QString> nowVisible;
    if (first >= 0 && last >= first) {
        for (int row = first; row <= last && row < items_.size(); ++row) {
            nowVisible.insert(items_.at(row).id);
        }
    }
    for (const auto &id : std::as_const(visibleIds_)) {
        if (!nowVisible.contains(id)) {
            thumbnails_.remove(id);
            formats_.remove(id);
            if (const auto cancelled = thumbnailLoads_.take(id)) cancelled->storeRelaxed(true);
            const int row = rowForId(id);
            if (row >= 0) emit dataChanged(index(row), index(row), {ThumbnailRole, FormatRole});
        }
    }
    visibleIds_ = std::move(nowVisible);
    // 只取消移出视口的请求，仍可见卡片继续复用正在读取的任务。
    for (const auto &id : std::as_const(visibleIds_)) {
        if (thumbnails_.contains(id) || thumbnailLoads_.contains(id)) continue;
        const auto cancelled = QSharedPointer<QAtomicInteger<bool>>::create(false);
        thumbnailLoads_.insert(id, cancelled);
        const int kind = items_.at(rowForId(id)).kind;
        auto *watcher = new QFutureWatcher<ThumbnailResult>(this);
        connect(watcher, &QFutureWatcher<ThumbnailResult>::finished, this, [this, watcher, cancelled] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (cancelled->loadRelaxed()) return;
            thumbnailLoads_.remove(result.id);
            if (!visibleIds_.contains(result.id)) return;
            // 空图像也记为已尝试，避免可见范围内反复读取和解码失败记录。
            // 离开可见范围或重新打开面板后会清除，允许下次重试。
            thumbnails_.insert(result.id, result.image);
            if (!result.format.isEmpty()) formats_.insert(result.id, result.format);
            const int row = rowForId(result.id);
            if (row >= 0) emit dataChanged(index(row), index(row), {ThumbnailRole, FormatRole});
        });
        const QString path = databasePath_;
        watcher->setFuture(QtConcurrent::run([path, id, cancelled, kind] {
            if (cancelled->loadRelaxed()) return ThumbnailResult{id, {}};
            if (kind == 0) {
                const auto content = HistoryStore(path).content(id);
                if (cancelled->loadRelaxed() || content.isEmpty()) return ThumbnailResult{id, {}};
                const auto text = ClipboardContent::decodeTextContent(content);
                const QString format = ClipboardContent::textFormatLabel(text);
                return ThumbnailResult{id, {}, format.isEmpty() ? QStringLiteral("PlainTextFormat") : format};
            }
            if (kind == 2) {
                const auto content = HistoryStore(path).content(id);
                if (cancelled->loadRelaxed()) return ThumbnailResult{id, {}};
                return ThumbnailResult{id, fileTypeIcon(content)};
            }
            const auto bytes = HistoryStore(path).thumbnail(id);
            if (cancelled->loadRelaxed()) return ThumbnailResult{id, {}};
            // 解码与缩放均在工作线程执行，GUI 只接收卡片所需尺寸的图像。
            QBuffer buffer;
            buffer.setData(bytes);
            if (!buffer.open(QIODevice::ReadOnly)) return ThumbnailResult{id, {}};
            QImageReader reader(&buffer);
            if (reader.size().isValid()) reader.setScaledSize(reader.size().scaled(300, 56, Qt::KeepAspectRatio));
            return ThumbnailResult{id, reader.read()};
        }));
    }
}

void HistoryModel::releaseThumbnails() {
    // 面板隐藏或开始恢复备份时，让尚未执行的缩略图任务直接退出。
    cancelThumbnailLoads();
    visibleIds_.clear();
    if (thumbnails_.isEmpty() && formats_.isEmpty()) return;
    thumbnails_.clear();
    formats_.clear();
    if (!items_.isEmpty()) emit dataChanged(index(0), index(items_.size() - 1), {ThumbnailRole, FormatRole});
}
