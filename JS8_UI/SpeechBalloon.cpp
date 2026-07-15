#include "SpeechBalloon.h"

#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QWindow>

namespace {

constexpr int  TEXT_MAX_WIDTH_PX = 320;     // wraps wider text
constexpr int  GAP_TO_TARGET_PX  = 2;       // tail tip → target edge (build 319: closer)

QColor balloonFill()   { return QColor("#FFFDDF"); }   // soft cream
QColor balloonStroke() { return QColor("#806A00"); }   // warm brown
QColor balloonText()   { return QColor("#222222"); }   // near-black

} // anonymous namespace

SpeechBalloon::SpeechBalloon(QString const &text, QWidget *target)
    : QWidget(target ? target->window() : nullptr,
              Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint),
      m_text(text), m_target(target) {

    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_DeleteOnClose,         true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);

    // Drop-shadow makes the balloon read as floating above the UI.
    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 3);
    shadow->setColor(QColor(0, 0, 0, 140));
    setGraphicsEffect(shadow);

    // Size the widget to text + padding + tail. Width-wrapped at
    // TEXT_MAX_WIDTH_PX so a long string stays a reasonable shape.
    QFontMetrics const fm(font());
    QRect const textRect = fm.boundingRect(
        QRect(0, 0, TEXT_MAX_WIDTH_PX, 10000),
        Qt::AlignLeft | Qt::TextWordWrap, m_text);

    int const bodyW = textRect.width()  + 2 * m_paddingX;
    int const bodyH = textRect.height() + 2 * m_paddingY;
    int extraW = 0, extraH = 0;
    switch (m_tailSide) {
    case TailSide::Top:
    case TailSide::Bottom: extraH = m_tailHeight; break;
    case TailSide::Left:
    case TailSide::Right:  extraW = m_tailHeight; break;
    }
    resize(bodyW + extraW, bodyH + extraH);
}

void SpeechBalloon::showAtTarget() {
    if (!m_target) {
        deleteLater();
        return;
    }

    // Place the balloon so the tail tip lands GAP_TO_TARGET_PX away
    // from the target's nearest edge, centered on that edge. An
    // override rect (target-local) narrows the anchor to a region of
    // the widget, e.g. a single menu title inside the menu bar.
    QRect const local = m_targetRectOverride.isValid()
                            ? m_targetRectOverride
                            : QRect(QPoint(0, 0), m_target->size());
    QPoint const tgtCenterGlobal =
        m_target->mapToGlobal(local.center());
    QRect const tgtRectGlobal(m_target->mapToGlobal(local.topLeft()),
                              local.size());

    QPoint pos;
    switch (m_tailSide) {
    case TailSide::Top:
        // Balloon sits BELOW the target; tail on top points up.
        pos = QPoint(tgtCenterGlobal.x() - width() / 2,
                     tgtRectGlobal.bottom() + GAP_TO_TARGET_PX);
        break;
    case TailSide::Bottom:
        // Balloon sits ABOVE the target; tail on bottom points down.
        pos = QPoint(tgtCenterGlobal.x() - width() / 2,
                     tgtRectGlobal.top() - height() - GAP_TO_TARGET_PX);
        break;
    case TailSide::Left:
        // Balloon sits to the RIGHT of target; tail on left points left.
        pos = QPoint(tgtRectGlobal.right() + GAP_TO_TARGET_PX,
                     tgtCenterGlobal.y() - height() / 2);
        break;
    case TailSide::Right:
        // Balloon sits to the LEFT of target; tail on right points right.
        pos = QPoint(tgtRectGlobal.left() - width() - GAP_TO_TARGET_PX,
                     tgtCenterGlobal.y() - height() / 2);
        break;
    }

    // Clamp to screen so we don't render off-edge.
    if (QScreen const *screen = QGuiApplication::screenAt(tgtCenterGlobal)) {
        QRect const avail = screen->availableGeometry();
        pos.setX(qBound(avail.left() + 4,
                        pos.x(),
                        avail.right() - width() - 4));
        pos.setY(qBound(avail.top() + 4,
                        pos.y(),
                        avail.bottom() - height() - 4));
    }
    move(pos);

    show();

    if (m_autoDismissMs > 0) {
        QTimer::singleShot(m_autoDismissMs, this, &QWidget::close);
    }
}

void SpeechBalloon::updateShape() {
    // Trigger a repaint; the path is recomputed inside paintEvent.
    update();
}

void SpeechBalloon::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    updateShape();
}

void SpeechBalloon::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Compute the body rectangle and the tail triangle. The widget
    // size already includes room for the tail along the appropriate
    // edge, so the body inset is m_tailHeight on that side.
    QRectF body = rect();
    QPolygonF tail;
    QPointF   textOrigin;
    switch (m_tailSide) {
    case TailSide::Top:
        body.adjust(0, m_tailHeight, 0, 0);
        tail << QPointF(body.center().x() - m_tailWidth / 2.0, body.top())
             << QPointF(body.center().x() + m_tailWidth / 2.0, body.top())
             << QPointF(body.center().x(),                     body.top() - m_tailHeight);
        break;
    case TailSide::Bottom:
        body.adjust(0, 0, 0, -m_tailHeight);
        tail << QPointF(body.center().x() - m_tailWidth / 2.0, body.bottom())
             << QPointF(body.center().x() + m_tailWidth / 2.0, body.bottom())
             << QPointF(body.center().x(),                     body.bottom() + m_tailHeight);
        break;
    case TailSide::Left:
        body.adjust(m_tailHeight, 0, 0, 0);
        tail << QPointF(body.left(), body.center().y() - m_tailWidth / 2.0)
             << QPointF(body.left(), body.center().y() + m_tailWidth / 2.0)
             << QPointF(body.left() - m_tailHeight, body.center().y());
        break;
    case TailSide::Right:
        body.adjust(0, 0, -m_tailHeight, 0);
        tail << QPointF(body.right(), body.center().y() - m_tailWidth / 2.0)
             << QPointF(body.right(), body.center().y() + m_tailWidth / 2.0)
             << QPointF(body.right() + m_tailHeight, body.center().y());
        break;
    }

    // Build a single path = rounded-rect body ∪ triangle tail.
    QPainterPath path;
    path.addRoundedRect(body, m_cornerRadius, m_cornerRadius);
    QPainterPath tailPath;
    tailPath.addPolygon(tail);
    tailPath.closeSubpath();
    path = path.united(tailPath);

    p.setPen(QPen(balloonStroke(), 1.5));
    p.setBrush(balloonFill());
    p.drawPath(path);

    p.setPen(balloonText());
    QRect const textArea = body.toRect().adjusted(
        m_paddingX, m_paddingY, -m_paddingX, -m_paddingY);
    p.drawText(textArea, Qt::AlignLeft | Qt::TextWordWrap, m_text);
}

void SpeechBalloon::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        close();
    }
}
