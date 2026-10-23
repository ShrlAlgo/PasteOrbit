#pragma once

#include <QByteArray>
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
};

QString firstUnavailableFilePath(const QStringList &paths);
QSize boundedSize(QSize size, int maxWidth, int maxHeight, qint64 maxPixels);
ImageContent decodeImageContent(const QByteArray &content);
TextContent decodeTextContent(const QByteArray &content);
QString textFormatLabel(const TextContent &content, bool markdown = false);
bool isMarkdownContent(const TextContent &content);
QPair<QString, QByteArray> textFileContent(const TextContent &content);
}
