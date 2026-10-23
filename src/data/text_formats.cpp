#include "text_formats.h"

#include <QJsonDocument>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextTable>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace {
// Excel 的制表符文本可能包含带引号的换行、制表符及双引号。
TextTable tabularText(const QString &text) {
    if (!text.contains(QLatin1Char('\t'))) return {};
    TextTable rows;
    QStringList row;
    QString cell;
    bool quoted = false, closed = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < text.size() && text.at(i + 1) == c) { cell += c; ++i; }
                else { quoted = false; closed = true; }
            } else cell += c;
        } else if (c == QLatin1Char('"') && cell.isEmpty() && !closed) {
            quoted = true;
        } else if (c == QLatin1Char('\t') || c == QLatin1Char('\r') || c == QLatin1Char('\n')) {
            row.append(cell); cell.clear(); closed = false;
            if (c != QLatin1Char('\t')) {
                if (c == QLatin1Char('\r') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) ++i;
                rows.append(row); row.clear();
            }
        } else {
            if (closed) return {};
            cell += c;
        }
    }
    if (quoted) return {};
    if (!row.isEmpty() || !cell.isEmpty() || closed) { row.append(cell); rows.append(row); }
    if (rows.isEmpty() || rows.first().size() < 2) return {};
    for (const auto &values : rows) if (values.size() != rows.first().size()) return {};
    return rows;
}
}

TextTable clipboardTable(const QString &text, const QString &html) {
    // 优先使用 Excel 提供的单元格文本，避免富文本布局改变空格和换行。
    const auto plain = tabularText(text);
    if (!plain.isEmpty()) return plain;
    if (!html.contains(QStringLiteral("<table"), Qt::CaseInsensitive)) return {};
    class TableDocument final : public QTextDocument {
        QVariant loadResource(int, const QUrl &) override { return {}; }
    } document;
    document.setHtml(html);
    const auto frames = document.rootFrame()->childFrames();
    if (frames.size() != 1) return {};
    const auto *table = qobject_cast<QTextTable *>(frames.first());
    if (!table || !table->childFrames().isEmpty()) return {};
    TextTable rows;
    for (int r = 0; r < table->rows(); ++r) {
        QStringList row;
        for (int c = 0; c < table->columns(); ++c) {
            const auto cell = table->cellAt(r, c);
            QString value;
            if (cell.row() == r && cell.column() == c) {
                auto cursor = cell.firstCursorPosition();
                cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                value = cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'))
                    .replace(QChar::LineSeparator, QLatin1Char('\n'));
            }
            row.append(value);
        }
        rows.append(row);
    }
    return rows;
}

QString tablePreviewHtml(const TextTable &table) {
    QString html = QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"5\">");
    for (const auto &row : table) {
        html += QStringLiteral("<tr>");
        for (const auto &cell : row)
            html += QStringLiteral("<td>") + cell.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))
                + QStringLiteral("</td>");
        html += QStringLiteral("</tr>");
    }
    return html + QStringLiteral("</table>");
}

QByteArray delimitedTable(const TextTable &table, QChar separator) {
    // 带 UTF-8 BOM，便于 Excel 直接打开中文 CSV；保留空单元格和原始文本值。
    QByteArray result("\xEF\xBB\xBF", 3);
    for (const auto &row : table) {
        QStringList values;
        for (QString cell : row) {
            if (cell.contains(separator) || cell.contains(QLatin1Char('"'))
                || cell.contains(QLatin1Char('\n')) || cell.contains(QLatin1Char('\r'))) {
                cell.replace(QStringLiteral("\""), QStringLiteral("\"\""));
                cell = QLatin1Char('"') + cell + QLatin1Char('"');
            }
            values.append(cell);
        }
        result += values.join(separator).toUtf8() + "\r\n";
    }
    return result;
}

QString formattedStructuredText(const QString &text) {
    if (!QJsonDocument::fromJson(text.toUtf8()).isNull()) {
        // 仅重排空白，保留数字字面量、转义和键顺序，避免大整数被浮点序列化改写。
        QString result;
        int depth = 0;
        bool quoted = false, escaped = false;
        const auto newline = [&] { result += QLatin1Char('\n') + QString(depth * 4, QLatin1Char(' ')); };
        for (qsizetype i = 0; i < text.size(); ++i) {
            const QChar c = text.at(i);
            if (quoted) {
                result += c;
                if (escaped) escaped = false;
                else if (c == QLatin1Char('\\')) escaped = true;
                else if (c == QLatin1Char('"')) quoted = false;
                continue;
            }
            if (c.isSpace()) continue;
            if (c == QLatin1Char('"')) { quoted = true; result += c; }
            else if (c == QLatin1Char('{') || c == QLatin1Char('[')) {
                result += c; ++depth;
                qsizetype next = i + 1;
                while (next < text.size() && text.at(next).isSpace()) ++next;
                if (next < text.size() && (text.at(next) == QLatin1Char('}') || text.at(next) == QLatin1Char(']'))) {
                    result += text.at(next); --depth; i = next;
                } else newline();
            } else if (c == QLatin1Char('}') || c == QLatin1Char(']')) { --depth; newline(); result += c; }
            else if (c == QLatin1Char(',')) { result += c; newline(); }
            else if (c == QLatin1Char(':')) result += QStringLiteral(": ");
            else result += c;
        }
        return result;
    }
    if (!text.trimmed().startsWith(QLatin1Char('<'))) return {};
    QXmlStreamReader reader(text);
    QString result;
    QXmlStreamWriter writer(&result);
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(4);
    struct Element { bool child = false; bool text = false; };
    QVector<Element> elements;
    bool root = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) return {};
        if (token == QXmlStreamReader::StartElement) {
            root = true;
            if (!elements.isEmpty()) elements.last().child = true;
            const bool preserve = reader.attributes().value(QStringLiteral("http://www.w3.org/XML/1998/namespace"),
                                                            QStringLiteral("space")) == QLatin1String("preserve");
            if (preserve) return {};
            elements.append(Element{});
        } else if (token == QXmlStreamReader::Characters && !reader.isWhitespace() && !elements.isEmpty()) {
            elements.last().text = true;
        } else if (token == QXmlStreamReader::EndElement) {
            const auto element = elements.takeLast();
            // 混合内容中的缩进可能改变正文，保留原文而不格式化。
            if (element.child && element.text) return {};
        }
        writer.writeCurrentToken(reader);
    }
    return root && !reader.hasError() && !writer.hasError() ? result : QString{};
}
