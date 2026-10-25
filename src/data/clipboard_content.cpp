#include "clipboard_content.h"

#include <QBuffer>
#include <QFileInfo>
#include <QDir>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QUrl>
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

ContentMetadata contentMetadata(const QByteArray &content, int kind) {
    ContentMetadata result;
    QSet<QString> seen;
    const auto appendUrl = [&result, &seen](const QUrl &url) {
        // 不允许执行自定义协议；file 目标始终由文件管理器处理。
        const auto scheme = url.scheme().toLower();
        if (!url.isValid() || url.host().isEmpty()
            || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))) return;
        const QString encoded = url.toString(QUrl::FullyEncoded);
        if (!seen.contains(encoded)) { seen.insert(encoded); result.openTargets.append(encoded); }
    };
    const auto appendPath = [&result, &seen](QStringView path) {
        path = path.trimmed();
        if (path.size() >= 2 && ((path.front() == QLatin1Char('"') && path.back() == QLatin1Char('"'))
            || (path.front() == QLatin1Char('\'') && path.back() == QLatin1Char('\''))))
            path = path.mid(1, path.size() - 2);
        QString localPath;
        if (path.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
            const QUrl url(path.toString(), QUrl::StrictMode);
            if (!url.isValid() || !url.isLocalFile()) return;
            localPath = url.toLocalFile();
            path = localPath;
        }
        // 仅接受完整绝对路径，避免把普通文本、相对路径或网页地址当成文件。
        static const QRegularExpression absolute(QStringLiteral(R"(^(?:[A-Za-z]:[/\\]|\\\\[^\\/]+[\\/][^\\/]+|/(?!/)))"));
        if (!absolute.matchView(path).hasMatch() || path.contains(QChar::Null)
            || path.contains(QLatin1Char('\n')) || path.contains(QLatin1Char('\r'))) return;
        const QString encoded = QUrl::fromLocalFile(QDir::fromNativeSeparators(path.toString())).toString(QUrl::FullyEncoded);
        if (!seen.contains(encoded)) { seen.insert(encoded); result.openTargets.append(encoded); }
    };
    if (kind == 2) {
        for (const auto &entry : QJsonDocument::fromJson(content).array())
            if (entry.isString()) appendPath(entry.toString());
        return result;
    }
    if (kind != 0) return result;
    const auto text = decodeTextContent(content);
    result.formatLabel = textFormatLabel(text);
    if (result.formatLabel.isEmpty()) result.formatLabel = QStringLiteral("PlainTextFormat");
    static const QRegularExpression links(QStringLiteral(R"(https?://[^\s<>"'\x{3000}-\x{303f}\x{ff00}-\x{ffef}]+)"),
                                         QRegularExpression::CaseInsensitiveOption);
    auto matches = links.globalMatch(text.text);
    while (matches.hasNext()) {
        QString value = matches.next().captured();
        while (!value.isEmpty()) {
            const QChar last = value.back();
            const bool extraBracket = (last == QLatin1Char(')') && value.count(')') > value.count('('))
                || (last == QLatin1Char(']') && value.count(']') > value.count('['))
                || (last == QLatin1Char('}') && value.count('}') > value.count('{'));
            if (!extraBracket && !QStringLiteral(".,;!?").contains(last)) break;
            value.chop(1);
        }
        appendUrl(QUrl(value, QUrl::StrictMode));
    }
    // 一行一个路径，允许空格和引号；不猜测正文中未加引号的路径边界。
    for (const auto line : QStringView(text.text).tokenize(QLatin1Char('\n')))
        appendPath(line);
    if (!text.html.isEmpty()) {
        class LinkDocument final : public QTextDocument {
            QVariant loadResource(int, const QUrl &) override { return {}; }
        } document;
        document.setHtml(text.html);
        for (auto block = document.begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QString href = it.fragment().charFormat().anchorHref();
                const QUrl url(href, QUrl::StrictMode);
                if (url.isLocalFile()) appendPath(href);
                else appendUrl(url);
            }
        }
    }
    return result;
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
