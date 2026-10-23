#pragma once

#include <QStringList>
#include <QVector>

using TextTable = QVector<QStringList>;

TextTable clipboardTable(const QString &text, const QString &html);
QString tablePreviewHtml(const TextTable &table);
QByteArray delimitedTable(const TextTable &table, QChar separator);
QString formattedStructuredText(const QString &text);
