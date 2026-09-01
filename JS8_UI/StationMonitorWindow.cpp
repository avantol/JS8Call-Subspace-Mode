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
#include <QRegularExpression>
#include <QTimeZone>
#include <QVBoxLayout>

#include <algorithm>

namespace {
// A member offset stays joinable this long after its last use; a
// station keys the same offset across a conversation, minutes apart.
constexpr qint64 OFFSET_TTL_MS = 10 * 60 * 1000;
// Backfill window: trailing hours of history replayed at open
// (operator 2026-09-01: 4.7 days of tail overgrew the member set).
constexpr int BACKFILL_HOURS = 6;
// And at most this much of each log's tail is even read.
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
    QString const &allTxtPath, QString const &myCall,
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
    backfill(directedTxtPath, allTxtPath, myCall);
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
    // Same/close offset: LIVE traffic only (the band-activity
    // row-migration rule, JS8::Submode::rxThreshold -- one
    // authority). Historic lines neither join by offset nor record
    // one: an offset in the log is whoever owned it THEN (operator
    // field-caught 2026-09-01: WD4KAV's old offset pulled in its
    // later occupants).
    if (!admit && !historical && offset > 0) {
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
    if (!historical && offset > 0) {
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
            .arg(offset > 0 ? QString::number(offset)
                            : QStringLiteral("?"))
            .arg(text));
    updateHeadline();
}

namespace {
struct HistLine {
    QDateTime utc;
    QString from;
    QString text;
    int offset;
};

QList<QByteArray> tailLines(QString const &path) {
    QFile f{path};
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    if (f.size() > BACKFILL_TAIL_BYTES)
        f.seek(f.size() - BACKFILL_TAIL_BYTES);
    QList<QByteArray> lines = f.readAll().split('\n');
    // First line after a mid-file seek is almost surely partial.
    if (f.size() > BACKFILL_TAIL_BYTES && !lines.isEmpty())
        lines.removeFirst();
    return lines;
}

QDateTime parseUtc(QString const &s) {
    QDateTime utc = QDateTime::fromString(
        s, QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    utc.setTimeZone(QTimeZone::utc());
    return utc;
}
} // namespace

// Replay history through the SAME membership engine so history and
// live traffic obey identical rules -- except the offset rule,
// which is live-only (see feed). Sources, merged chronologically,
// bounded to the trailing BACKFILL_HOURS:
//  - DIRECTED.TXT (writeMsgTxt): "utc\tMHz\toffset\tSNR\ttext",
//    text = "FROM: TO ...". No mode column -> "?" indicator.
//  - ALL.TXT "Transmitting" lines: OUR side, per-frame with no
//    offset; consecutive frames <=30 s apart stitch into one
//    assembled message credited to myCall.
void StationMonitorWindow::backfill(QString const &directedTxtPath,
                                    QString const &allTxtPath,
                                    QString const &myCall) {
    QDateTime const cutoff = QDateTime::currentDateTimeUtc().addSecs(
        -BACKFILL_HOURS * 3600);
    QList<HistLine> hist;

    for (QByteArray const &raw : tailLines(directedTxtPath)) {
        QString const line = QString::fromUtf8(raw).trimmed();
        QStringList const cols = line.split(QLatin1Char('\t'));
        if (cols.size() < 5)
            continue;
        QDateTime const utc = parseUtc(cols[0]);
        if (!utc.isValid() || utc < cutoff)
            continue;
        QString const text = cols.mid(4).join(QLatin1Char('\t'));
        // FROM is the text's own "CALL: " prefix; membership also
        // sees every mention, so extraction can stay lax.
        QString from;
        if (int const colon = text.indexOf(QStringLiteral(": "));
            colon > 0)
            from = text.left(colon);
        hist.append({utc, from, text, cols[2].toInt()});
    }

    // Our transmissions: stitch consecutive per-frame lines.
    HistLine tx{QDateTime(), myCall, QString(), 0};
    QDateTime lastFrame;
    auto const flushTx = [&]() {
        if (tx.utc.isValid() && !tx.text.trimmed().isEmpty())
            hist.append(tx);
        tx = {QDateTime(), myCall, QString(), 0};
    };
    static QRegularExpression const txRe{QStringLiteral(
        R"(^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+Transmitting .*JS8:\s(.*)$)")};
    for (QByteArray const &raw : tailLines(allTxtPath)) {
        auto const m =
            txRe.match(QString::fromUtf8(raw).trimmed());
        if (!m.hasMatch())
            continue;
        QDateTime const utc = parseUtc(m.captured(1));
        if (!utc.isValid() || utc < cutoff)
            continue;
        if (!tx.utc.isValid() || lastFrame.secsTo(utc) > 30)
            flushTx();
        if (!tx.utc.isValid())
            tx.utc = utc;
        tx.text += m.captured(2);
        lastFrame = utc;
    }
    flushTx();

    std::stable_sort(hist.begin(), hist.end(),
                     [](HistLine const &a, HistLine const &b) {
                         return a.utc < b.utc;
                     });
    for (HistLine const &h : hist)
        feed(h.from, QString(), QString(), h.text, h.offset, h.utc,
             -1, true);
}

void StationMonitorWindow::updateHeadline() {
    // Count only -- the full list ran to hundreds of calls
    // (operator 2026-09-01: "might be a good idea for later").
    m_headline->setText(
        tr("Following %1 station(s)").arg(m_members.size()));
}
