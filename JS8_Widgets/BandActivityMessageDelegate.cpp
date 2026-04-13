#include "BandActivityMessageDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QToolTip>

BandActivityMessageDelegate::BandActivityMessageDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {}

QVariantList BandActivityMessageDelegate::getGroups(const QModelIndex &index) {
    return index.data(Qt::UserRole).toList();
}

void BandActivityMessageDelegate::paint(QPainter *painter,
                                        const QStyleOptionViewItem &option,
                                        const QModelIndex &index) const {
    auto groups = getGroups(index);

    // Fall back to default rendering if no groups or single group
    if (groups.size() <= 1) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // Draw background (selection highlight, alternating row colors)
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear(); // we'll draw text ourselves
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

    painter->save();

    const QRect rect = option.rect;
    const int n = groups.size();
    const int regionWidth = rect.width() / n;
    const QFontMetrics fm(option.font);
    const int textMargin = 4;
    const bool selected = option.state & QStyle::State_Selected;

    for (int i = 0; i < n; i++) {
        auto map = groups[i].toMap();
        QString text = map["text"].toString();

        QRect regionRect(rect.x() + i * regionWidth, rect.y(),
                         regionWidth, rect.height());

        // Draw vertical divider (except before first region)
        if (i > 0) {
            // Use background color when selected, gray otherwise
            QColor divColor = selected
                ? option.palette.base().color()
                : QColor(180, 180, 180);
            painter->setPen(QPen(divColor, 1));
            painter->drawLine(regionRect.topLeft(), regionRect.bottomLeft());
        }

        // Draw text, elided to fit. Left-justify (ElideRight) for
        // key status messages where the start is most important.
        QRect textRect = regionRect.adjusted(textMargin, 0, -textMargin, 0);
        bool leftJustify = text.contains("@HB HEARTBEAT ")
            || text.contains("HEARTBEAT SNR")
            || text.contains("@ALLCALL CQ")
            || (!m_myCall.isEmpty() && (text.contains(m_myCall + " SNR")
                                       || text.contains(m_myCall + " YES")));
        auto elideMode = leftJustify ? Qt::ElideRight : Qt::ElideLeft;
        QString elided = fm.elidedText(text, elideMode, textRect.width());

        painter->setPen(selected
                            ? option.palette.highlightedText().color()
                            : option.palette.text().color());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
    }

    painter->restore();
}

bool BandActivityMessageDelegate::helpEvent(QHelpEvent *event,
                                            QAbstractItemView *view,
                                            const QStyleOptionViewItem &option,
                                            const QModelIndex &index) {
    if (event->type() != QEvent::ToolTip)
        return QStyledItemDelegate::helpEvent(event, view, option, index);

    auto groups = getGroups(index);

    // Single group or no groups: show full text as tooltip if elided
    if (groups.size() <= 1) {
        QString text = index.data(Qt::DisplayRole).toString();
        if (!text.isEmpty()) {
            QFontMetrics fm(option.font);
            int cellWidth = option.rect.width();
            if (fm.horizontalAdvance(text) > cellWidth) {
                QToolTip::showText(event->globalPos(), text.toHtmlEscaped(), view);
                return true;
            }
        }
        QToolTip::hideText();
        return true;
    }

    // Determine which sub-region the mouse is over
    QRect rect = option.rect;
    int mouseX = event->pos().x() - rect.x();
    int n = groups.size();
    int regionWidth = rect.width() / n;
    int regionIndex = qBound(0, mouseX / regionWidth, n - 1);

    auto map = groups[regionIndex].toMap();
    QString text = map["text"].toString();
    QToolTip::showText(event->globalPos(), text.toHtmlEscaped(), view);
    return true;
}

QString BandActivityMessageDelegate::callsignAtPosition(
    const QVariantList &groups, int cellWidth, int mouseX) {
    if (groups.isEmpty() || cellWidth <= 0)
        return {};
    int n = groups.size();
    int regionWidth = cellWidth / n;
    int regionIndex = qBound(0, mouseX / regionWidth, n - 1);
    return groups[regionIndex].toMap()["call"].toString();
}
