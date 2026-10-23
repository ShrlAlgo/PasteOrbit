#include "clipboard_content.h"

#include <QBuffer>
#include <QFileInfo>
#include <QDir>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtMath>

namespace ClipboardContent {
QString firstUnavailableFilePath(const QStringList &paths) {
    // 仅在用户请求预览或粘贴时由工作线程调用；目录也是可粘贴的有效路径。
    for (const auto &path : paths) {
        if (!QDir::isAbsolutePath(path) || !QFileInfo::exists(path)) return path;
    }
    return {};
}

QSize boundedSize(QSize size, int maxWidth, int maxHeight, qint64 maxPixels) {
    if (!size.isValid()) return {};
    double scale = 1.0;
    if (maxWidth > 0) scale = qMin(scale, maxWidth / static_cast<double>(size.width()));
    if (maxHeight > 0) scale = qMin(scale, maxHeight / static_cast<double>(size.height()));
    const qint64 pixels = static_cast<qint64>(size.width()) * size.height();
    if (pixels > maxPixels) scale = qMin(scale, qSqrt(maxPixels / static_cast<double>(pixels)));
    return QSize(qMax(1, qRound(size.width() * scale)), qMax(1, qRound(size.height() * scale)));
}

// 旧图片记录直接存原图；新图文记录在原图之外保留剪贴板的其他格式。
ImageContent decodeImageContent(const QByteArray &content) {
    if (!content.startsWith(MixedImagePayloadPrefix)) return {content, {}, {}, {}};
    QBuffer buffer;
    buffer.setData(content);
    if (!buffer.open(QIODevice::ReadOnly) || !buffer.seek(sizeof(MixedImagePayloadPrefix) - 1)) return {};
    QDataStream stream(&buffer);
    stream.setVersion(QDataStream::Qt_6_0);
    ImageContent result;
    stream >> result.bytes >> result.text >> result.html >> result.rtf;
    return stream.status() == QDataStream::Ok && buffer.atEnd() ? result : ImageContent{};
}

TextContent decodeTextContent(const QByteArray &content) {
    TextContent result;
    const QByteArray prefix(TextPayloadPrefix);
    if (!content.startsWith(prefix)) {
        result.text = QString::fromUtf8(content);
        return result;
    }
    const auto json = QJsonDocument::fromJson(content.mid(prefix.size())).object();
    result.text = json.value(QStringLiteral("Text")).toString();
    result.html = json.value(QStringLiteral("Html")).toString();
    // RTF 可包含非 UTF-8 字节；新记录使用 Base64，旧记录仍按原格式读取。
    result.rtf = json.contains(QStringLiteral("RtfBase64"))
        ? QByteArray::fromBase64(json.value(QStringLiteral("RtfBase64")).toString().toLatin1())
        : json.value(QStringLiteral("Rtf")).toString().toUtf8();
    result.markdown = json.value(QStringLiteral("Markdown")).toBool();
    return result;
}

// 编辑器常附带 HTML 而不提供 Markdown MIME；导出和预览共用正文识别结果。
QString textFormatLabel(const TextContent &content, bool markdown) {
    if (markdown || content.markdown) return QStringLiteral("Markdown");
    if (!content.html.isEmpty()) return content.rtf.isEmpty() ? QStringLiteral("HTML") : QStringLiteral("HTML · RTF");
    return content.rtf.isEmpty() ? QString{} : QStringLiteral("RTF");
}

bool isMarkdownContent(const TextContent &content) {
    if (content.markdown) return true;
    // 自动链接和 HTML 注释也可出现在 Markdown 开头，仅排除完整标记文档。
    if (content.text.trimmed().startsWith(QLatin1Char('<'))) {
        static const QRegularExpression htmlDocument(QStringLiteral(
            R"(^\s*(?:<!--[\s\S]*?-->\s*)*(?:<!DOCTYPE\s+html\b|<html(?:\s|>)))"),
            QRegularExpression::CaseInsensitiveOption);
        if (htmlDocument.match(content.text).hasMatch()) return false;
        QXmlStreamReader xml(content.text);
        bool hasRoot = false;
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement) hasRoot = true;
        }
        if (!xml.hasError() && hasRoot) return false;
    }
    static const QRegularExpression structure(QStringLiteral(
        R"(^ {0,3}(?:#{1,6}[\t ]+\S|`{3,}|~{3,}|[-+*][\t ]+\[[ xX]\][\t ]+\S)|!?\[[^\]\r\n]+\]\([^\s)\r\n]+\)|(\*\*|__)\S(?:[^\r\n]*?\S)?\1)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression underline(QStringLiteral(
        R"(^[^\r\n]+\r?\n {0,3}(?:={3,}|-{3,})[\t ]*\r?$|^[^\r\n]*\|[^\r\n]+\r?\n[\t ]*\|?[\t ]*:?-{3,}:?[\t ]*(?:\|[\t ]*:?-{3,}:?[\t ]*)+\|?[\t ]*\r?$)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression list(QStringLiteral(R"(^ {0,3}(?:[-+*]|\d+[.)]|>)[\t ]+\S)"),
                                         QRegularExpression::MultilineOption);
    bool marked = structure.match(content.text).hasMatch() || underline.match(content.text).hasMatch();
    if (!marked) {
        auto matches = list.globalMatch(content.text);
        if (matches.hasNext()) { matches.next(); marked = matches.hasNext(); }
    }
    // JSON 字符串中的强调或链接语法不改变其文件类型。
    return marked && QJsonDocument::fromJson(content.text.toUtf8()).isNull();
}

// 先识别正文格式，再使用附带的富文本；内容与扩展名一并确定。
QPair<QString, QByteArray> textFileContent(const TextContent &content) {
    if (isMarkdownContent(content)) return {QStringLiteral("md"), content.text.toUtf8()};
    const QByteArray bytes = content.text.toUtf8();
    if (!QJsonDocument::fromJson(bytes).isNull()) return {QStringLiteral("json"), bytes};
    if (content.text.trimmed().startsWith(QLatin1Char('<'))) {
        QXmlStreamReader xml(bytes);
        QString suffix;
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement && suffix.isEmpty())
                suffix = xml.name().compare(QLatin1String("html"), Qt::CaseInsensitive) == 0
                    ? QStringLiteral("html") : QStringLiteral("xml");
        }
        if (!xml.hasError() && !suffix.isEmpty()) return {suffix, bytes};
    }
    if (!content.html.isEmpty()) return {QStringLiteral("html"), content.html.toUtf8()};
    if (!content.rtf.isEmpty()) return {QStringLiteral("rtf"), content.rtf};
    return {QStringLiteral("txt"), bytes};
}

}
