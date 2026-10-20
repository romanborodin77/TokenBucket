#include "Brand.h"
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QtMath>
namespace tb::ui::brand {
QColor accent() { return QColor(0x2f, 0x9e, 0x6f); }
namespace {
// Отрисовка в логических координатах 240×240; scale приводит к нужному размеру.
void draw(QPainter& p, Tone tone) {
    p.setRenderHint(QPainter::Antialiasing);
    // Щит: трапецеидальный контур с макета; градиент зелёный→синий, у Locked — серые тона.
    QLinearGradient shieldGradient(0, 22, 208, 218);
    if (tone == Tone::Normal) {
        shieldGradient.setColorAt(0.0, QColor(0x35, 0xb5, 0x7f));
        shieldGradient.setColorAt(1.0, QColor(0x1c, 0x7f, 0xae));
    } else {
        shieldGradient.setColorAt(0.0, QColor(0x8a, 0x93, 0x9e));
        shieldGradient.setColorAt(1.0, QColor(0x5c, 0x66, 0x72));
    }
    QPainterPath shield;
    shield.moveTo(120, 22);
    shield.lineTo(208, 52);
    shield.lineTo(208, 122);
    shield.quadTo(208, 192, 120, 218); // условное закругление нижней части по макету
    shield.quadTo(32, 192, 32, 122);
    shield.lineTo(32, 52);
    shield.closeSubpath();
    p.setPen(QPen(QColor(0x0d, 0x3b, 0x57, tone == Tone::Normal ? 63 : 40), 6));
    p.setBrush(shieldGradient);
    p.drawPath(shield);
    // Жетон: белое поле, пунктирное кольцо (отсылка к алгоритму token bucket), литера T.
    const QColor field = tone == Tone::Normal ? QColor(0xf4, 0xf9, 0xf6) : QColor(0xe4, 0xe7, 0xea);
    p.setPen(Qt::NoPen);
    p.setBrush(field);
    p.drawEllipse(QPointF(120, 112), 44, 44);
    QColor ring = tone == Tone::Normal ? QColor(0x1c, 0x7f, 0xae) : QColor(0x5c, 0x66, 0x72);
    QPen ringPen(ring, 5, Qt::CustomDashLine);
    ringPen.setDashPattern({7.0, 9.0}); // пунктир по макету — отсылка к алгоритму token bucket
    p.setBrush(Qt::NoBrush);
    p.setPen(ringPen);
    p.drawEllipse(QPointF(120, 112), 33, 33);
    // T из двух прямоугольников по макету (113,96,14×32 и 104,105,32×13).
    const QColor ink(0x17, 0x32, 0x4a);
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawRect(QRectF(113, 96, 14, 32));
    p.drawRect(QRectF(104, 105, 32, 13));
}
}
QPixmap icon(int size, Tone tone) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.scale(size / 240.0, size / 240.0);
    draw(p, tone);
    return pixmap;
}
QPixmap logo(int height) { return icon(height, Tone::Normal); }
}
