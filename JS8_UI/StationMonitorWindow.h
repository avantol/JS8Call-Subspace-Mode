#ifndef STATION_MONITOR_WINDOW_HPP__
#define STATION_MONITOR_WINDOW_HPP__

/**
 * @file StationMonitorWindow.h
 * @brief [stamon, TODO #210] Debug window following ONE station's
 * conversations: every assembled directed message whose parties
 * connect to the station -- directly, through a relay path, by
 * callsign mention, or by transmitting at/near a member's offset.
 * The member set GROWS as the conversation graph is discovered.
 *
 * Env-gated (JS8_STATION_MONITOR) right-click entry on the call
 * activity list; one window per station; no persistence -- this is
 * a debug instrument, default OFF with zero background state.
 */

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QWidget>

class QLabel;
class QPlainTextEdit;

class StationMonitorWindow final : public QWidget {
    Q_OBJECT

  public:
    // directedTxtPath: DIRECTED.TXT for the open-time backfill.
    StationMonitorWindow(QString const &station,
                         QString const &directedTxtPath,
                         QWidget *parent = nullptr);

    QString station() const { return m_station; }

    // One assembled directed message (the composed "FROM: TO ..."
    // line). historical = a backfilled DIRECTED.TXT line: its mode
    // letter is unknown ("?") but membership math runs identically.
    void feed(QString const &from, QString const &to,
              QString const &relayPath, QString const &text,
              int offset, QDateTime const &utc, int submode,
              bool historical = false);

  private:
    void backfill(QString const &path);
    void updateHeadline();

    QString m_station;
    QSet<QString> m_members;
    // offset -> last-seen ms; an unattributed sender keying within
    // the submode's rx threshold of a member offset joins the set.
    QHash<int, qint64> m_memberOffsets;
    QLabel *m_headline;
    QPlainTextEdit *m_log;
};

#endif
