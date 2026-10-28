#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QPair>

// 预览与粘贴共用的历史内容解码，确保格式判断一致。
namespace ClipboardContent {
inline constexpr auto TextPayloadPrefix = "PasteOrbit.Text/1\n";
inline constexpr char MixedImagePayloadPrefix[] = "PasteOrbit.Image/2\n";
struct ImageContent {
    QByteArray bytes;
    QString text;
    QString html;
    QByteArray rtf;
};

struct TextContent {
    QString text;
    QString html;
    QByteArray rtf;
    bool markdown = false;
    QHash<QString, QByteArray> htmlImages;
};

struct ContentMetadata {
    QString formatLabel;
    QStringList openTargets;
};

// 只识别内容，不访问文件或网络；打开目标统一保存为编码后的 URL。
ContentMetadata contentMetadata(const QByteArray &content, int kind);

QString firstUnavailableFilePath(const QStringList &paths);
QSize boundedSize(QSize size, int maxWidth, int maxHeight, qint64 maxPixels);
ImageContent decodeImageContent(const QByteArray &content);
TextContent decodeTextContent(const QByteArray &content, bool includeImages = false);
// 捕获时快照 HTML 引用的本地图片；预览仅解码快照和内嵌图片，不读取外部资源。
QHash<QString, QByteArray> captureHtmlImages(const QString &html);
QHash<QString, QImage> previewHtmlImages(const QString &html, const QHash<QString, QByteArray> &images);
QString textFormatLabel(const TextContent &content, bool markdown = false);
bool isMarkdownContent(const TextContent &content);
QPair<QString, QByteArray> textFileContent(const TextContent &content);
}
