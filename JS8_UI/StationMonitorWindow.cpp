/**
 * [stamon, TODO #210] Per-station conversation follower. See header.
 */

#include "StationMonitorWindow.h"

#include "JS8_Main/Radio.h"
#include "JS8_Main/Varicode.h"
#include "JS8_Mode/JS8Submode.h"

#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTimeZone>
#include <QVBoxLayout>

namespace {
// A member offset stays joinable this long after its last use; a
// station keys the same offset across a conversation, minutes apart.
constexpr qint64 OFFSET_TTL_MS = 10 * 60 * 1000;
// Backfill reads at most this much of DIRECTED.TXT's tail.
constexpr qint64 BACKFILL_TAIL_BYTES = 512 * 1024;

// Real single-station callsigns only: no @groups, no empty parts.
QStringList validParties(QString const &joined) {
    QStringList out;
    for (QString const &p : joined.split(QLatin1Char('>'),
                                         Qt::SkipEmptyParts)) {
        QString const c = p.trimmed();
        if (!c.isEmpty() && !c.startsWith(QLatin1Char('@')) &&
            Radio::is_callsign(c))
            out << c;
    }
    return out;
}
} // namespace

StationMonitorWindow::StationMonitorWindow(
    QString const &station, QString const &directedTxtPath,
    QWidget *parent)
    : QWidget{parent}, m_station{station} {
    setWindowFlag(Qt::Window);
    setWindowTitle(tr("Station monitor: %1").arg(station));
    setMinimumSize(560, 360);
    auto *lay = new QVBoxLayout(this);
    m_headline = new QLabel(this);
    lay->addWidget(m_headline);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    QFont mono{QStringLiteral("monospace")};
    mono.setStyleHint(QFont::Monospace);
    m_log->setFont(mono);
    lay->addWidget(m_log);

    m_members.insert(station);
    updateHeadline();
    backfill(directedTxtPath);
}

void StationMonitorWindow::feed(QString const &from, QString const &to,
                                QString const &relayPath,
                                QString const &text, int offset,
                                QDateTime const &utc, int submode,
                                bool historical) {
    // Everyone this message names: sender (which can itself be a
    // relay path "A>B" -- processCommandActivity.cpp:1070), the
    // addressee, the relay path, and any callsign mentioned in the
    // text. Full-callsign identity throughout.
    QStringList parties = validParties(from);
    parties += validParties(to);
    parties += validParties(relayPath);
    for (QString const &c : Varicode::parseCallsigns(text))
        if (!parties.contains(c))
            parties << c;

    bool admit = false;
    for (QString const &c : parties)
        if (m_members.contains(c)) {
            admit = true;
            break;
        }
    if (!admit && offset > 0) {
        // Same/close offset: the band-activity row-migration rule
        // (JS8::Submode::rxThreshold -- one authority).
        int const range = JS8::Submode::rxThreshold(
            submode >= 0 ? submode : Varicode::JS8CallNormal);
        qint64 const nowMs = utc.toMSecsSinceEpoch();
        for (auto it = m_memberOffsets.constBegin();
             it != m_memberOffsets.constEnd(); ++it)
            if (qAbs(it.key() - offset) <= range &&
                nowMs - it.value() <= OFFSET_TTL_MS) {
                admit = true;
                break;
            }
    }
    if (!admit)
        return;

    for (QString const &c : parties)
        m_members.insert(c);
    if (offset > 0) {
        m_memberOffsets[offset] = utc.toMSecsSinceEpoch();
        for (auto it = m_memberOffsets.begin();
             it != m_memberOffsets.end();)
            if (utc.toMSecsSinceEpoch() - it.value() > OFFSET_TTL_MS)
                it = m_memberOffsets.erase(it);
            else
                ++it;
    }

    // The conversation panel's exact preamble (mainwindow.cpp
    // writeMessageTextToUI): mode letter - time - (offset) - text.
    m_log->appendPlainText(
        QStringLiteral("%1 - %2 - (%3) - %4")
            .arg(historical ? QStringLiteral("?")
                            : JS8::Submode::indicator(submode))
            .arg(utc.time().toString())
            .arg(offset)
            .arg(text));
    updateHeadline();
}

// Replay the DIRECTED.TXT tail through the SAME membership engine
// so history and live traffic obey identical rules. Columns
// (writeMsgTxt): "yyyy-MM-dd hh:mm:ss\tMHz\toffset\tSNR\ttext",
// text = "FROM: TO ...". No mode column -> historical lines print
// "?" and use Normal's rx threshold for the offset rule.
void StationMonitorWindow::backfill(QString const &path) {
    QFile f{path};
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    if (f.size() > BACKFILL_TAIL_BYTES)
        f.seek(f.size() - BACKFILL_TAIL_BYTES);
    QList<QByteArray> const lines = f.readAll().split('\n');
    // First line after a mid-file seek is almost surely partial.
    for (int i = (f.size() > BACKFILL_TAIL_BYTES ? 1 : 0);
         i < lines.size(); ++i) {
        QString const line = QString::fromUtf8(lines[i]).trimmed();
        QStringList const cols = line.split(QLatin1Char('\t'));
        if (cols.size() < 5)
            continue;
        QDateTime utc = QDateTime::fromString(
            cols[0], QStringLiteral("yyyy-MM-dd hh:mm:ss"));
        utc.setTimeZone(QTimeZone::utc());
        QString const text = cols.mid(4).join(QLatin1Char('\t'));
        // FROM is the text's own "CALL: " prefix; membership also
        // sees every mention, so from/to extraction can stay lax.
        QString from;
        int const colon = text.indexOf(QStringLiteral(": "));
        if (colon > 0)
            from = text.left(colon);
        feed(from, QString(), QString(), text, cols[2].toInt(), utc,
             -1, true);
    }
}

void StationMonitorWindow::updateHeadline() {
    QStringList calls{m_members.begin(), m_members.end()};
    calls.sort();
    m_headline->setText(
        tr("Following %1 station(s): %2")
            .arg(m_members.size())
            .arg(calls.join(QStringLiteral("  "))));
    m_headline->setWordWrap(true);
}
