#include "BandActivityMessageDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QToolTip>

namespace {
// Bold callsign patterns (CALL:) in HTML for tooltips, with line breaks
QString boldCallsigns(const QString &text) {
    QString html = text.toHtmlEscaped();
    // Match CALLSIGN: — must contain at least one digit (real callsigns always do)
    static const QRegularExpression re(R"((\b(?=[A-Z0-9/]*[0-9])[A-Z0-9/]{3,15}:)(?=\s))");
    html.replace(re, "<br/><b>\\1</b>");
    // Remove leading <br/> if text starts with a callsign
    if (html.startsWith("<br/>"))
        html = html.mid(5);
    // Wrap in a wide container to prevent premature line wrapping
    return QString("<div style='white-space:nowrap;'>%1</div>").arg(html);
}
}

BandActivityMessageDelegate::BandActivityMessageDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {}

QVariantList BandActivityMessageDelegate::getGroups(const QModelIndex &index) {
    return index.data(Qt::UserRole).toList();
}

void BandActivityMessageDelegate::paint(QPainter *painter,
                                        const QStyleOptionViewItem &option,
                                        const QModelIndex &index) const {
    if (!painter || !index.isValid())
        return;

    auto groups = getGroups(index);

    // Single group or no groups: render with bold callsigns but no dividers
    if (groups.size() <= 1) {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QString text = opt.text;
        // [BUILD 331-todo79] Render-side strip of newlines / carriage
        // returns / tabs. Source data is untouched; only paint output
        // is cleaned so the row stays on one line with no column
        // misalignment.
        text.replace(QChar('\n'), QChar(' '))
            .replace(QChar('\r'), QChar(' '))
            .replace(QChar('\t'), QChar(' '));
        opt.text.clear();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

        painter->save();
        QRect textRect = opt.rect.adjusted(4, 0, -4, 0);
        QColor textColor = opt.state & QStyle::State_Selected
            ? opt.palette.highlightedText().color()
            : opt.palette.text().color();
        painter->setPen(textColor);

        // Elide from left (show most recent) for consistency
        QFontMetrics fm(opt.font);
        QString elided = fm.elidedText(text, Qt::ElideLeft, textRect.width());

        // Draw text with bold callsigns (CALL: pattern with at least one digit)
        static const QRegularExpression callRe(R"(\b((?=[A-Z0-9/]*[0-9])[A-Z0-9/]{3,15}: ))");
        QFont boldFont = opt.font;
        boldFont.setBold(true);
        int xPos = textRect.x();
        int remaining = textRect.width();
        auto it = callRe.globalMatch(elided);
        int lastEnd = 0;
        while (it.hasNext() && remaining > 0) {
            auto match = it.next();
            // Draw text before the callsign (normal)
            if (match.capturedStart() > lastEnd) {
                QString before = elided.mid(lastEnd, match.capturedStart() - lastEnd);
                painter->setFont(opt.font);
                painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                                  Qt::AlignLeft | Qt::AlignVCenter, before);
                int w = QFontMetrics(opt.font).horizontalAdvance(before);
                xPos += w;
                remaining -= w;
            }
            // Draw callsign in bold
            QString call = match.captured(0);
            painter->setFont(boldFont);
            painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, call);
            int w = QFontMetrics(boldFont).horizontalAdvance(call);
            xPos += w;
            remaining -= w;
            lastEnd = match.capturedEnd();
        }
        // Draw remaining text (normal)
        if (lastEnd < elided.length() && remaining > 0) {
            painter->setFont(opt.font);
            painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, elided.mid(lastEnd));
        }
        painter->restore();
        return;
    }

    // Draw background (selection highlight, alternating row colors)
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear(); // we'll draw text ourselves
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

    painter->save();

    const QRect rect = opt.rect;
    const int n = groups.size();
    const int regionWidth = rect.width() / n;
    const QFontMetrics fm(opt.font);
    const int textMargin = 4;
    const bool selected = opt.state & QStyle::State_Selected;

    for (int i = 0; i < n; i++) {
        auto map = groups[i].toMap();
        QString text = map["text"].toString();
        // [BUILD 331-todo79] Render-side newline / CR / tab strip,
        // mirror of the single-group path above.
        text.replace(QChar('\n'), QChar(' '))
            .replace(QChar('\r'), QChar(' '))
            .replace(QChar('\t'), QChar(' '));

        QRect regionRect(rect.x() + i * regionWidth, rect.y(),
                         regionWidth, rect.height());

        // Draw vertical divider (except before first region)
        if (i > 0) {
            // Use background color when selected, gray otherwise
            QColor divColor = selected
                ? opt.palette.base().color()
                : QColor(180, 180, 180);
            painter->setPen(QPen(divColor, 1));
            painter->drawLine(regionRect.topLeft(), regionRect.bottomLeft());
        }

        // Draw text with bold callsigns. Left-justify (ElideRight) for
        // key status messages where the start is most important.
        QRect textRect = regionRect.adjusted(textMargin, 0, -textMargin, 0);
        bool leftJustify = text.contains("@HB HEARTBEAT ")
            || text.contains("HEARTBEAT SNR")
            || text.contains("@ALLCALL CQ")
            || (!m_myCall.isEmpty() && (text.contains(m_myCall + " SNR")
                                       || text.contains(m_myCall + " YES")));
        auto elideMode = leftJustify ? Qt::ElideRight : Qt::ElideLeft;
        QString elided = fm.elidedText(text, elideMode, textRect.width());

        // Draw text with bold callsigns only (CALL: with digit)
        QColor textColor = selected
            ? opt.palette.highlightedText().color()
            : opt.palette.text().color();
        painter->setPen(textColor);

        static const QRegularExpression callRe(R"(\b((?=[A-Z0-9/]*[0-9])[A-Z0-9/]{3,15}: ))");
        QFont boldFont = opt.font;
        boldFont.setBold(true);
        int xPos = textRect.x();
        int remaining = textRect.width();
        auto it = callRe.globalMatch(elided);
        int lastEnd = 0;
        while (it.hasNext() && remaining > 0) {
            auto match = it.next();
            if (match.capturedStart() > lastEnd) {
                QString before = elided.mid(lastEnd, match.capturedStart() - lastEnd);
                painter->setFont(opt.font);
                painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                                  Qt::AlignLeft | Qt::AlignVCenter, before);
                int w = QFontMetrics(opt.font).horizontalAdvance(before);
                xPos += w; remaining -= w;
            }
            QString call = match.captured(0);
            painter->setFont(boldFont);
            painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, call);
            int w = QFontMetrics(boldFont).horizontalAdvance(call);
            xPos += w; remaining -= w;
            lastEnd = match.capturedEnd();
        }
        if (lastEnd < elided.length() && remaining > 0) {
            painter->setFont(opt.font);
            painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, elided.mid(lastEnd));
        }
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
            QStyleOptionViewItem opt = option;
            initStyleOption(&opt, index);
            QFontMetrics fm(opt.font);
            int cellWidth = opt.rect.width();
            if (fm.horizontalAdvance(text) > cellWidth) {
                QToolTip::showText(event->globalPos(), boldCallsigns(text), view);
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
    QToolTip::showText(event->globalPos(), boldCallsigns(text), view);
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
