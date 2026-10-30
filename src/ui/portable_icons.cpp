#include "portable_icons.h"
#include <QPainter>
#include <QPainterPath>
#include <cmath>

void drawPortableIcon(QPainter *painter, const QRectF &rect, QChar glyph, const QColor &color) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const qreal side = qMin(rect.width(), rect.height());
    painter->translate(rect.center() - QPointF(side / 2, side / 2));
    painter->scale(side / 24, side / 24);
    painter->setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    switch (glyph.unicode()) {
    case 0xE713: // 设置
        painter->drawEllipse(QPointF(12, 12), 7, 7);
        painter->drawEllipse(QPointF(12, 12), 2.5, 2.5);
        for (int i = 0; i < 8; ++i) {
            painter->save(); painter->translate(12, 12); painter->rotate(i * 45);
            painter->drawLine(QPointF(0, -7), QPointF(0, -10)); painter->restore();
        }
        break;
    case 0xE734: case 0xE735: { // 收藏
        QPainterPath star;
        for (int i = 0; i < 10; ++i) {
            const qreal angle = (i * 36 - 90) * 3.141592653589793 / 180;
            const qreal radius = i % 2 ? 4 : 9;
            const QPointF point(12 + radius * std::cos(angle), 12 + radius * std::sin(angle));
            if (i) star.lineTo(point); else star.moveTo(point);
        }
        star.closeSubpath();
        if (glyph.unicode() == 0xE735) painter->setBrush(color);
        painter->drawPath(star);
        break;
    }
    case 0xE890: case 0xED1A: { // 预览
        QPainterPath eye;
        eye.moveTo(2, 12); eye.quadTo(12, 0, 22, 12); eye.quadTo(12, 24, 2, 12);
        painter->drawPath(eye); painter->drawEllipse(QPointF(12, 12), 3, 3);
        if (glyph.unicode() == 0xED1A) painter->drawLine(3, 3, 21, 21);
        break;
    }
    case 0xE712:
        painter->setBrush(color); painter->setPen(Qt::NoPen);
        for (int x : {5, 12, 19}) painter->drawEllipse(QPointF(x, 12), 1.4, 1.4);
        break;
    case 0xE718: case 0xE77A:
        painter->drawLine(8, 4, 16, 4); painter->drawLine(9, 4, 9, 10);
        painter->drawLine(15, 4, 15, 10); painter->drawLine(9, 10, 6, 14);
        painter->drawLine(15, 10, 18, 14); painter->drawLine(6, 14, 18, 14);
        painter->drawLine(12, 14, 12, 21);
        if (glyph.unicode() == 0xE77A) painter->drawLine(3, 3, 21, 21);
        break;
    case 0xE75C:
        painter->drawLine(4, 6, 20, 6); painter->drawLine(9, 3, 15, 3);
        painter->drawRoundedRect(QRectF(6, 6, 12, 15), 2, 2);
        painter->drawLine(10, 10, 10, 17); painter->drawLine(14, 10, 14, 17);
        break;
    case 0xE71C:
        painter->drawPolyline(QPolygonF{QPointF(3, 4), QPointF(21, 4), QPointF(14, 12),
                                       QPointF(14, 20), QPointF(10, 18), QPointF(10, 12), QPointF(3, 4)});
        break;
    case 0xE8B9:
        painter->drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
        painter->drawEllipse(QPointF(8, 9), 1.5, 1.5);
        painter->drawPolyline(QPolygonF{QPointF(4, 18), QPointF(10, 12), QPointF(14, 16), QPointF(18, 11), QPointF(21, 15)});
        break;
    case 0xE8A9:
        for (int y : {5, 11, 17}) painter->drawRoundedRect(QRectF(4, y, 16, 3), 1, 1);
        break;
    case 0xE8B7:
        painter->drawPolyline(QPolygonF{QPointF(3, 20), QPointF(3, 5), QPointF(10, 5), QPointF(12, 8), QPointF(21, 8), QPointF(21, 20), QPointF(3, 20)});
        break;
    default: // 文本与文件
        painter->drawRoundedRect(QRectF(5, 3, 14, 18), 1, 1);
        for (int y : {8, 12, 16}) painter->drawLine(8, y, 16, y);
        break;
    }
    painter->restore();
}
