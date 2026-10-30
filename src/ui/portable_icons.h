#pragma once

#include <QChar>
#include <QColor>
#include <QRectF>
class QPainter;

// macOS 不具备 Windows 图标字体，直接绘制实际使用的少量线形图标。
void drawPortableIcon(QPainter *painter, const QRectF &rect, QChar glyph, const QColor &color);
