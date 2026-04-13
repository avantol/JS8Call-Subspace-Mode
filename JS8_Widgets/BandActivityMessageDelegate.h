#ifndef BAND_ACTIVITY_MESSAGE_DELEGATE_H
#define BAND_ACTIVITY_MESSAGE_DELEGATE_H

#include <QStyledItemDelegate>
#include <QVariantList>

/**
 * Custom delegate for the Message(s) column in the band activity table.
 * Renders callsign groups in sub-divided regions with vertical separators.
 * Supports per-sub-region tooltips and click target identification.
 */
class BandActivityMessageDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit BandActivityMessageDelegate(QObject *parent = nullptr);

    void setMyCallsign(const QString &call) { m_myCall = call; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

    /// Given a mouse X position within the cell, return the callsign
    /// for the sub-region at that position. Empty string if no groups.
    static QString callsignAtPosition(const QVariantList &groups,
                                      int cellWidth, int mouseX);

private:
    static QVariantList getGroups(const QModelIndex &index);
    QString m_myCall;
};

#endif // BAND_ACTIVITY_MESSAGE_DELEGATE_H
