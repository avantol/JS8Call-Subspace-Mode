/**
 * @file IntelMiner.cpp
 * @brief See header. Line-for-line port of mine.py (632 lines) +
 *        intel.py schema (216 lines); python citations inline. The
 *        enumeration lives in the port itself: every rule carries its
 *        mine.py line range.
 */
#include "IntelMiner.h"
#include "Radio.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimeZone>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <limits>

Q_LOGGING_CATEGORY(miner_js8, "js8.miner")

namespace {

// mine.py:61 -- ONE character may precede the digit, not two: written
// {2,} this rejected every 1x2/1x3 callsign and silently dropped
// 23.8% of five months of traffic (measured 2026-08-25).
QString const kCallsign =
    QStringLiteral("[A-Z0-9]{1,3}[0-9][A-Z0-9]{1,5}(?:/[A-Z0-9]+)?");

QString rx(QString const &pat) { return pat; }

// intel.py:28-97 -- THE schema, verbatim.
QString const kSchema = QStringLiteral(
    "CREATE TABLE IF NOT EXISTS meta ("
    " key TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE IF NOT EXISTS stations ("
    " call TEXT PRIMARY KEY, first_heard INTEGER, last_heard INTEGER,"
    " heard_count INTEGER DEFAULT 0, snr_n INTEGER DEFAULT 0,"
    " snr_sum INTEGER DEFAULT 0, snr_min INTEGER, snr_max INTEGER,"
    " rev_snr_n INTEGER DEFAULT 0, rev_snr_last INTEGER,"
    " rev_snr_best INTEGER, rev_last INTEGER,"
    " resp_count INTEGER DEFAULT 0, spont_count INTEGER DEFAULT 0,"
    " relay_seen INTEGER DEFAULT 0, relay_asked INTEGER DEFAULT 0,"
    " relay_done INTEGER DEFAULT 0, to_us INTEGER DEFAULT 0,"
    " grid TEXT);"
    "CREATE TABLE IF NOT EXISTS activity ("
    " call TEXT, hour INTEGER, n INTEGER DEFAULT 0,"
    " PRIMARY KEY (call, hour));"
    "CREATE TABLE IF NOT EXISTS edges ("
    " hearer TEXT, heard TEXT, last_when INTEGER,"
    " n INTEGER DEFAULT 0, snr INTEGER, source TEXT,"
    " PRIMARY KEY (hearer, heard));"
    "CREATE TABLE IF NOT EXISTS edge_events ("
    " ts INTEGER, hearer TEXT, heard TEXT, source TEXT);"
    "CREATE TABLE IF NOT EXISTS probes ("
    " ts INTEGER, target TEXT, cmd TEXT,"
    " answered INTEGER DEFAULT 0, latency_s INTEGER,"
    " present INTEGER DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS sightings ("
    " ts INTEGER, call TEXT, snr INTEGER, dial REAL, offset INTEGER);"
    "CREATE INDEX IF NOT EXISTS idx_sight_call_ts ON sightings(call, ts);"
    "CREATE INDEX IF NOT EXISTS idx_sight_ts ON sightings(ts);"
    "CREATE INDEX IF NOT EXISTS idx_edges_heard ON edges(heard);"
    "CREATE INDEX IF NOT EXISTS idx_probes_target ON probes(target, ts);"
    "CREATE INDEX IF NOT EXISTS idx_ee_heard ON edge_events(heard, ts);"
    "CREATE INDEX IF NOT EXISTS idx_ee_pair ON edge_events(hearer, heard, ts);");
// intel.py:104
QString const kSchemaVersion = QStringLiteral("3");

// mine.py:90 -- the app's own QUERY CALL window (kQCallReplyWindowMs).
constexpr qint64 PROBE_WINDOW_S = 300;

// mine.py per-station accumulator (mine.py:182-190)
struct St {
    qint64 firstHeard = -1, lastHeard = -1;
    int heardCount = 0, snrN = 0, snrSum = 0;
    int snrMin = std::numeric_limits<int>::max();
    int snrMax = std::numeric_limits<int>::min();
    int revSnrN = 0;
    int revSnrLast = std::numeric_limits<int>::min();
    int revSnrBest = std::numeric_limits<int>::min();
    qint64 revLast = -1;
    int respCount = 0, spontCount = 0, relaySeen = 0, toUs = 0;
    double relayAsked = 0.0, relayDone = 0.0;
    QString grid;
};
struct Edge {
    qint64 lastWhen = 0;
    int n = 0;
    int snr = std::numeric_limits<int>::min(); // min() = NULL
    QString source;
};
struct EdgeEvent { qint64 ts; QString hearer, heard, source; };
struct Sighting { qint64 ts; QString call; int snr; double dial; int offset; };
struct Probe { qint64 ts; QString target, cmd; int answered;
               qint64 latency; int present; };

qint64 epochUtc(QString const &s) {   // mine.py:118 -- stamps are UTC
    QDateTime dt = QDateTime::fromString(
        s, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    dt.setTimeZone(QTimeZone::utc());
    return dt.toSecsSinceEpoch();
}

} // namespace

IntelMiner::IntelMiner(QObject *parent) : QObject(parent) {
    QString const home = QDir::homePath();
    directedPath = home + QStringLiteral("/.local/share/JS8Call/DIRECTED.TXT");
    allTxtPath = home + QStringLiteral("/.local/share/JS8Call/ALL.TXT");
    gridsDbPath = home + QStringLiteral("/.config/JS8Call-grids.db");
    intelDbPath = home + QStringLiteral("/.config/js8reach-intel.db");
}

IntelMiner::Result IntelMiner::mine(QString const &myCallIn,
                                    QString const &myGridIn, bool force) {
    Result r;
    QElapsedTimer timer;
    timer.start();
    QString const me = myCallIn.trimmed().toUpper();
    if (me.isEmpty())
        return r;
    QString const meBase = me.section(QLatin1Char('/'), 0, 0);
    auto isMe = [&meBase](QString const &c) {   // mine.py:192
        return c.section(QLatin1Char('/'), 0, 0) == meBase;
    };

    // ---- unchanged-logs skip (bookmark; design item 2) ------------
    QFileInfo const dIn{directedPath}, aIn{allTxtPath};
    QString const stamp =
        QStringLiteral("%1:%2:%3:%4")
            .arg(dIn.size()).arg(dIn.lastModified().toSecsSinceEpoch())
            .arg(aIn.size()).arg(aIn.lastModified().toSecsSinceEpoch());
    if (!force && QFileInfo::exists(intelDbPath)) {
        QString const conn = QStringLiteral("intelminer_probe");
        {
            QSqlDatabase db =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(intelDbPath);
            if (db.open()) {
                QSqlQuery q{db};
                if (q.exec(QStringLiteral(
                        "SELECT value FROM meta WHERE key='sources_stamp'"))
                    && q.next() && q.value(0).toString() == stamp) {
                    r.skipped = true;
                    r.ok = true;
                }
            }
        }
        QSqlDatabase::removeDatabase(conn);
        if (r.skipped) {
            qCWarning(miner_js8) << "[MINER] logs unchanged -- skip";
            return r;
        }
    }

    // ---- regexes (mine.py:62-158), compiled once ------------------
    static QRegularExpression const reDirected{rx(
        QStringLiteral("^(?<date>\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2})\\t"
                       "(?<dial>[\\d.]+)\\t(?<offset>-?\\d+)\\t"
                       "(?<snr>[+-]?\\d+)\\t(?<text>.*)$"))};
    static QRegularExpression const reFrom{
        QStringLiteral("^(?<from>%1):\\s+(?<rest>.*)$").arg(kCallsign)};
    static QRegularExpression const reCall{kCallsign};
    static QRegularExpression const reDe{
        QStringLiteral("\\*DE\\*\\s+(?<de>%1)").arg(kCallsign)};
    static QRegularExpression const reRelayAsk{
        QStringLiteral("^(?<s>%1):\\s+(?<a>%1)>\\s*(?<rest>.+)$")
            .arg(kCallsign)};
    static QRegularExpression const reTx{QStringLiteral(
        "^(?<date>\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2})\\s+"
        "Transmitting\\s+(?<dial>[\\d.]+)\\s+MHz\\s+\\w+:\\s+"
        "(?<text>.*)$")};
    // mine.py:80-86 -- lookahead, NOT \b (55 real probes died on \b).
    static QRegularExpression const reProbe{
        QStringLiteral("^(?:(?<me>%1):\\s+)?(?<heads>(?:%1>\\s*)*)"
                       "(?<to>@?[A-Z0-9/]+)\\s+(?<cmd>SNR\\?|GRID\\?|"
                       "HEARING\\?|STATUS\\?|INFO\\?|QUERY CALL|"
                       "QUERY ARQ\\?|QUERY MSGS)(?=\\s|$)")
            .arg(kCallsign)};
    static QRegularExpression const reReplyCmd{QStringLiteral(
        "^(?:HEARTBEAT\\s+SNR|SNR|ACK|YES|NO|GRID|STATUS|INFO|"
        "QUERY\\s+\\w+)(?:\\s|$)")};
    static QRegularExpression const reThirdSnr{
        QStringLiteral("^(?:HEARTBEAT\\s+)?SNR\\s+(?<snr>[+-]?\\d+)")};
    static QRegularExpression const reQueryCmd{QStringLiteral(
        "^(?:SNR\\?|\\?|HEARING\\?|GRID\\?|STATUS\\?|INFO\\?|AGN\\?|"
        "QUERY|HEARTBEAT|HB|CQ|MSG\\b|DIT)(?:\\s|$)")};
    // mine.py:153-158 -- closing paren OPTIONAL (truncation "~~~~~").
    static QRegularExpression const reQcallYes{
        QStringLiteral("^(?<who>%1):\\s+(?<me>%1)\\s+YES\\s+"
                       "(?<snr>[+-]\\d{1,3})\\s*\\((?<age>NOW|\\d+[SMHD])")
            .arg(kCallsign)};
    static QRegularExpression const reQcallTx{
        QStringLiteral("QUERY CALL\\s+(?<target>%1)").arg(kCallsign)};
    static QRegularExpression const reQcallTxTail{
        QStringLiteral("^(?<target>%1)\\?").arg(kCallsign)};
    static QRegularExpression const reHeads{
        QStringLiteral("^(?:(?:%1|@[A-Z0-9]+)>\\s*)+").arg(kCallsign)};
    static QRegularExpression const reHeadOne{
        QStringLiteral("^(?:%1|@[A-Z0-9]+)>").arg(kCallsign)};
    static QRegularExpression const reRevSnr{QStringLiteral(
        "\\b(?:HEARTBEAT\\s+SNR|SNR|ACK)\\s+([+-]\\d{1,2})\\b")};
    static QRegularExpression const reHearing{
        QStringLiteral("\\bHEARING\\b(?<list>.*)$")};
    static QRegularExpression const reGridTail{QStringLiteral(
        "\\bGRID\\s+([A-R]{2}\\d{2}(?:[A-X]{2})?)\\b")};
    // Grid-bank seeding, RELIABLE kinds only (operator ruling): the
    // measured strict scanners, not mine.py (new functionality,
    // design item 5).
    static QRegularExpression const reSeedCall{
        QStringLiteral("\\s([A-Z0-9]{3,10}(?:/[A-Z0-9]+)?):\\s")};
    static QRegularExpression const reSeedHb{QStringLiteral(
        "\\bHEARTBEAT\\s+([A-R]{2}[0-9]{2}(?:[A-Xa-x]{2})?)\\b")};
    static QRegularExpression const reSeedCq{QStringLiteral(
        "\\bCQ(?:\\s+CQ)*\\s+([A-R]{2}[0-9]{2}(?:[A-Xa-x]{2})?)\\s*$")};

    qint64 const now = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    QChar const diamond{0x2666}; // mine.py:305 replaces this with ' '

    // ---- accumulators (mine.py:161-178) ---------------------------
    QHash<QString, St> stations;
    QHash<QString, QHash<int, int>> activity;
    QHash<QString, QHash<QString, Edge>> edges; // hearer -> heard
    QVector<Sighting> sightings;
    QVector<Probe> probes;
    QHash<QString, QVector<qint64>> rxByCall;
    QVector<EdgeEvent> edgeEvents;
    struct RelayAsk { qint64 ts; QString asked, by; };
    QVector<RelayAsk> relayAsks;
    QHash<QString, QVector<QPair<qint64, QString>>> relayFwds;
    QVector<QPair<qint64, QString>> qcallSends, qcallRepliesMeta;
    struct QcallReply { qint64 ts; QString who; int snr; qint64 age; };
    QVector<QcallReply> qcallReplies;
    int skewed = 0;

    auto edgeAdd = [&](QString const &hearer, QString const &heard,
                       qint64 ts, int snr, bool haveSnr,
                       QString const &source) {   // mine.py:271-286
        if (hearer == heard || isMe(heard))
            return;
        Edge &e = edges[hearer][heard];
        if (e.source.isEmpty())
            e.source = source;
        e.n += 1;
        // mine.py:143 EVENT_SOURCES
        if (source == QLatin1String("hearing") ||
            source == QLatin1String("replied") ||
            source == QLatin1String("relayfrom") ||
            source == QLatin1String("querycall"))
            edgeEvents.append({ts, hearer, heard, source});
        if (ts > e.lastWhen) {
            e.lastWhen = ts;
            e.source = source;
            if (haveSnr)
                e.snr = snr;
        }
    };
    auto sighting = [&](QString const &call, qint64 ts, int snr,
                        double dial, int offset) {  // mine.py:195-217
        if (ts > now + 3600) {   // clock-skew guard (field 2026-08-21)
            ++skewed;
            return;
        }
        St &s = stations[call];
        s.heardCount += 1;
        s.firstHeard = s.firstHeard < 0 ? ts : qMin(s.firstHeard, ts);
        s.lastHeard = qMax(s.lastHeard, ts);
        s.snrN += 1;
        s.snrSum += snr;
        s.snrMin = qMin(s.snrMin, snr);
        s.snrMax = qMax(s.snrMax, snr);
        int const hour = QDateTime::fromSecsSinceEpoch(ts, QTimeZone::utc())
                             .time().hour();
        activity[call][hour] += 1;
        sightings.append({ts, call, snr, dial, offset});
        rxByCall[call].append(ts);
    };

    // ---- DIRECTED.TXT (mine.py:298-440) ---------------------------
    {
        QFile f{directedPath};
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in{&f};
            QString line;
            while (in.readLineInto(&line)) {
                auto const m = reDirected.match(line);
                if (!m.hasMatch())
                    continue;
                QString text = m.captured(QStringLiteral("text"));
                text.replace(diamond, QLatin1Char(' '));
                text = text.trimmed();
                auto const fm = reFrom.match(text);
                if (!fm.hasMatch())
                    continue; // continuation frame, no attribution
                QString const sender =
                    fm.captured(QStringLiteral("from")).toUpper();
                QString const rest =
                    fm.captured(QStringLiteral("rest")).trimmed();
                qint64 const ts = epochUtc(m.captured(QStringLiteral("date")));
                r.directedLines += 1;
                // [#178] QUERY CALL YES capture (mine.py:319-328)
                auto const ym = reQcallYes.match(text.toUpper());
                if (ym.hasMatch() &&
                    isMe(ym.captured(QStringLiteral("me")))) {
                    QString const a = ym.captured(QStringLiteral("age"));
                    qint64 secs = 0;
                    if (a != QLatin1String("NOW")) {
                        qint64 const v = a.chopped(1).toLongLong();
                        QChar const u = a.back();
                        secs = v * (u == QLatin1Char('S') ? 1
                                    : u == QLatin1Char('M') ? 60
                                    : u == QLatin1Char('H') ? 3600
                                                            : 86400);
                    }
                    qcallReplies.append(
                        {ts, ym.captured(QStringLiteral("who")).toUpper(),
                         ym.captured(QStringLiteral("snr")).toInt(), secs});
                }
                if (isMe(sender))
                    continue; // our own frames come from ALL.TXT
                sighting(sender, ts,
                         m.captured(QStringLiteral("snr")).toInt(),
                         m.captured(QStringLiteral("dial")).toDouble(),
                         m.captured(QStringLiteral("offset")).toInt());

                auto const de = reDe.match(rest);
                bool const head = reHeadOne.match(rest).hasMatch();
                if (de.hasMatch() || head)
                    stations[sender].relaySeen += 1;
                // Forward vs request (mine.py:339-348)
                if (de.hasMatch()) {
                    relayFwds[sender].append(
                        {ts, de.captured(QStringLiteral("de")).toUpper()});
                } else {
                    auto const ask = reRelayAsk.match(text);
                    if (ask.hasMatch() &&
                        !isMe(ask.captured(QStringLiteral("a")))) {
                        QString const asked =
                            ask.captured(QStringLiteral("a")).toUpper();
                        // 30-day half-life weight (mine.py:222-224)
                        stations[asked].relayAsked +=
                            std::pow(0.5, (now - ts) / (30.0 * 86400.0));
                        relayAsks.append(
                            {ts, asked,
                             ask.captured(QStringLiteral("s")).toUpper()});
                    }
                }
                if (de.hasMatch()) {
                    // overheard relay in flight (mine.py:349-357)
                    QString const src =
                        de.captured(QStringLiteral("de")).toUpper();
                    if (src != sender && !isMe(src))
                        edgeAdd(sender, src, ts, 0, false,
                                QStringLiteral("relayfrom"));
                }
                QString const hearer =
                    de.hasMatch()
                        ? de.captured(QStringLiteral("de")).toUpper()
                        : sender;
                QString body = rest;
                body.remove(reHeads);
                QStringList const parts =
                    body.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (parts.isEmpty())
                    continue;
                QString to = parts.first().toUpper();
                while (to.endsWith(QLatin1Char('>')))
                    to.chop(1);
                QString const tail =
                    QStringList(parts.mid(1)).join(QLatin1Char(' '));
                QString const tu = tail.toUpper();

                // traffic profile (mine.py:371-387)
                if (reReplyCmd.match(tu).hasMatch())
                    stations[sender].respCount += 1;
                else
                    stations[sender].spontCount += 1;

                if (!to.startsWith(QLatin1Char('@')) && !isMe(to)) {
                    // replies make edges; queries never do
                    // (mine.py:389-418, the #167 blind-call lesson)
                    if (reReplyCmd.match(tu).hasMatch()) {
                        auto const ms = reThirdSnr.match(tu);
                        edgeAdd(sender, to, ts,
                                ms.hasMatch()
                                    ? ms.captured(QStringLiteral("snr"))
                                          .toInt()
                                    : 0,
                                ms.hasMatch(), QStringLiteral("replied"));
                    } else if (!tail.trimmed().isEmpty() &&
                               !reQueryCmd.match(tu).hasMatch()) {
                        edgeAdd(sender, to, ts, 0, false,
                                QStringLiteral("freetext"));
                    }
                }
                if (isMe(to)) {
                    stations[sender].toUs += 1;
                    auto const rm = reRevSnr.match(tail);
                    if (rm.hasMatch()) {   // mine.py:288-294
                        St &s = stations[sender];
                        s.revSnrN += 1;
                        s.revSnrLast = rm.captured(1).toInt();
                        s.revSnrBest =
                            qMax(s.revSnrBest, rm.captured(1).toInt());
                        s.revLast = ts;
                    }
                }
                auto const hm = reHearing.match(tail);
                if (hm.hasMatch()) {   // mine.py:427-433
                    QString listing = hm.captured(QStringLiteral("list"));
                    listing.remove(reDe);
                    auto it = reCall.globalMatch(listing.toUpper());
                    while (it.hasNext()) {
                        QString const c = it.next().captured(0);
                        if (c != hearer && !isMe(c))
                            edgeAdd(hearer, c, ts, 0, false,
                                    QStringLiteral("hearing"));
                    }
                }
                auto const gm = reGridTail.match(tail.toUpper());
                if (gm.hasMatch())   // mine.py:435-439
                    stations[hearer].grid = gm.captured(1);
            }
        }
    }

    // ---- ALL.TXT: probes + qcall sends + grid seeding -------------
    {
        struct Sent { qint64 ts; QString target, cmd; };
        QVector<Sent> sent;
        qint64 qcallAwait = -1;
        QHash<QString, QHash<QString, QPair<int, qint64>>> seed; // call->grid4->(n,latest)
        QFile f{allTxtPath};
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in{&f};
            QString line;
            while (in.readLineInto(&line)) {
                auto const m = reTx.match(line);
                if (!m.hasMatch()) {
                    // Not one of our transmissions: grid-bank seeding
                    // pass over decode lines (design item 5; NOT in
                    // mine.py). Reliable kinds only.
                    auto const cm = reSeedCall.match(line);
                    if (cm.hasMatch()) {
                        QString const tailPart =
                            line.mid(cm.capturedEnd());
                        auto g = reSeedHb.match(tailPart);
                        if (!g.hasMatch())
                            g = reSeedCq.match(tailPart);
                        if (g.hasMatch()) {
                            QString const call = cm.captured(1).toUpper();
                            // The BANK is a targeting authority:
                            // only real amateur shapes may seed it
                            // (the intel db keeps score-don't-filter;
                            // 14 corroborated-garbage calls slipped
                            // through the loose pattern on first
                            // run, e.g. DUGH67, 0000IH8P8).
                            if (!isMe(call) &&
                                Radio::is_amateur_callsign(call)) {
                                QString const g4 =
                                    g.captured(1).left(4).toUpper();
                                // decode-line date is cols 0-18
                                qint64 const ts =
                                    epochUtc(line.left(19));
                                auto &slot = seed[call][g4];
                                slot.first += 1;
                                slot.second = qMax(slot.second, ts);
                            }
                        }
                    }
                    continue;
                }
                QString text = m.captured(QStringLiteral("text"));
                text.replace(diamond, QLatin1Char(' '));
                text = text.trimmed();
                qint64 const tsTx =
                    epochUtc(m.captured(QStringLiteral("date")));
                QString const up = text.toUpper();
                // [#178] two-frame QUERY CALL stitch (mine.py:453-471)
                if (up.contains(QLatin1String("QUERY CALL"))) {
                    auto const qm = reQcallTx.match(up);
                    if (qm.hasMatch()) {
                        qcallSends.append(
                            {tsTx,
                             qm.captured(QStringLiteral("target"))});
                        qcallAwait = -1;
                    } else {
                        qcallAwait = tsTx;
                    }
                } else if (qcallAwait >= 0) {
                    auto const tm = reQcallTxTail.match(up);
                    if (tm.hasMatch())
                        qcallSends.append(
                            {qcallAwait,
                             tm.captured(QStringLiteral("target"))});
                    qcallAwait = -1;
                }
                auto const pm = reProbe.match(text);
                if (!pm.hasMatch())
                    continue;
                if (!pm.captured(QStringLiteral("me")).isEmpty() &&
                    !isMe(pm.captured(QStringLiteral("me"))))
                    continue;
                QString const to =
                    pm.captured(QStringLiteral("to")).toUpper();
                QStringList heads;
                for (QString h :
                     pm.captured(QStringLiteral("heads"))
                         .split(QLatin1Char('>'), Qt::SkipEmptyParts)) {
                    h = h.trimmed();
                    if (!h.isEmpty())
                        heads << h.toUpper();
                }
                QString const target = heads.isEmpty() ? to : heads.first();
                if (target.startsWith(QLatin1Char('@')))
                    continue; // broadcast: no single expected answerer
                sent.append({tsTx, target,
                             pm.captured(QStringLiteral("cmd"))});
            }
        }
        // credit answers + presence (mine.py:492-506)
        for (auto const &s : sent) {
            auto const &stamps = rxByCall.value(s.target);
            qint64 latency = -1;
            bool present = false;
            for (qint64 t : stamps) {
                if (latency < 0 && s.ts < t && t <= s.ts + PROBE_WINDOW_S)
                    latency = t - s.ts;
                if (std::llabs(t - s.ts) <= 600)
                    present = true;
            }
            probes.append({s.ts, s.target, s.cmd, latency > 0 ? 1 : 0,
                           latency, present ? 1 : 0});
        }
        r.probes = sent.size();

        // corroborated grid seeding rows (>= 2 sightings of one grid)
        for (auto it = seed.constBegin(); it != seed.constEnd(); ++it) {
            QString bestGrid;
            int bestN = 0;
            qint64 bestTs = 0;
            for (auto g = it.value().constBegin();
                 g != it.value().constEnd(); ++g) {
                if (g.value().first > bestN ||
                    (g.value().first == bestN &&
                     g.value().second > bestTs)) {
                    bestGrid = g.key();
                    bestN = g.value().first;
                    bestTs = g.value().second;
                }
            }
            if (bestN >= 2)
                r.logGrids.append({it.key(), bestGrid, bestTs, bestN});
        }
    }

    // ---- grid bank (mine.py:511-526) ------------------------------
    {
        QString const conn = QStringLiteral("intelminer_grids");
        {
            QSqlDatabase db =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(gridsDbPath);
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
            if (db.open()) {
                QSqlQuery q{db};
                if (q.exec(QStringLiteral("SELECT call, grid FROM grids")))
                    while (q.next()) {
                        QString const call = q.value(0).toString().toUpper();
                        QString const grid = q.value(1).toString().toUpper();
                        St &s = stations[call];
                        if (s.grid.isEmpty() ||
                            grid.size() >= s.grid.size())
                            s.grid = grid;
                    }
            }
        }
        QSqlDatabase::removeDatabase(conn);
    }

    // ---- harvest + settle (mine.py:229-269) -----------------------
    for (auto const &send : qcallSends)
        for (auto const &rep : qcallReplies)
            if (rep.ts - send.first > 0 &&
                rep.ts - send.first <= PROBE_WINDOW_S)
                edgeAdd(rep.who, send.second, rep.ts - rep.age, rep.snr,
                        true, QStringLiteral("querycall"));
    for (auto const &ask : relayAsks) {
        double const w =
            std::pow(0.5, (now - ask.ts) / (30.0 * 86400.0));
        for (auto const &fw : relayFwds.value(ask.asked)) {
            if (fw.first - ask.ts >= 0 && fw.first - ask.ts < 900 &&
                fw.second == ask.by) {
                stations[ask.asked].relayDone += w;
                break;
            }
        }
    }

    // ---- write: temp db, then atomic rename (design item 3) -------
    QString const tmpPath = intelDbPath + QStringLiteral(".mining");
    QFile::remove(tmpPath);
    bool wrote = false;
    {
        QString const conn = QStringLiteral("intelminer_out");
        {
            QSqlDatabase db =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(tmpPath);
            if (db.open()) {
                QSqlQuery q{db};
                for (QString const &stmt :
                     kSchema.split(QLatin1Char(';'), Qt::SkipEmptyParts))
                    if (!stmt.trimmed().isEmpty() && !q.exec(stmt))
                        qCWarning(miner_js8) << "[MINER] DDL failed:"
                                             << q.lastError().text();
                db.transaction();
                q.prepare(QStringLiteral(
                    "INSERT INTO stations (call, first_heard, last_heard,"
                    " heard_count, snr_n, snr_sum, snr_min, snr_max,"
                    " rev_snr_n, rev_snr_last, rev_snr_best, rev_last,"
                    " relay_seen, relay_asked, relay_done, to_us, grid,"
                    " resp_count, spont_count)"
                    " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
                auto nullIf = [](qint64 v, qint64 sentinel) {
                    return v == sentinel ? QVariant{}
                                         : QVariant{qlonglong(v)};
                };
                for (auto it = stations.constBegin();
                     it != stations.constEnd(); ++it) {
                    St const &s = it.value();
                    q.addBindValue(it.key());
                    q.addBindValue(nullIf(s.firstHeard, -1));
                    q.addBindValue(nullIf(s.lastHeard, -1));
                    q.addBindValue(s.heardCount);
                    q.addBindValue(s.snrN);
                    q.addBindValue(s.snrSum);
                    q.addBindValue(
                        nullIf(s.snrMin, std::numeric_limits<int>::max()));
                    q.addBindValue(
                        nullIf(s.snrMax, std::numeric_limits<int>::min()));
                    q.addBindValue(s.revSnrN);
                    q.addBindValue(nullIf(s.revSnrLast,
                                          std::numeric_limits<int>::min()));
                    q.addBindValue(nullIf(s.revSnrBest,
                                          std::numeric_limits<int>::min()));
                    q.addBindValue(nullIf(s.revLast, -1));
                    q.addBindValue(s.relaySeen);
                    q.addBindValue(s.relayAsked);
                    q.addBindValue(s.relayDone);
                    q.addBindValue(s.toUs);
                    q.addBindValue(s.grid.isEmpty() ? QVariant{}
                                                    : QVariant{s.grid});
                    q.addBindValue(s.respCount);
                    q.addBindValue(s.spontCount);
                    q.exec();
                }
                q.prepare(QStringLiteral(
                    "INSERT INTO activity (call, hour, n) VALUES (?,?,?)"));
                for (auto it = activity.constBegin();
                     it != activity.constEnd(); ++it)
                    for (auto h = it.value().constBegin();
                         h != it.value().constEnd(); ++h) {
                        q.addBindValue(it.key());
                        q.addBindValue(h.key());
                        q.addBindValue(h.value());
                        q.exec();
                    }
                q.prepare(QStringLiteral(
                    "INSERT INTO edges (hearer, heard, last_when, n, snr,"
                    " source) VALUES (?,?,?,?,?,?)"));
                for (auto it = edges.constBegin(); it != edges.constEnd();
                     ++it)
                    for (auto e = it.value().constBegin();
                         e != it.value().constEnd(); ++e) {
                        q.addBindValue(it.key());
                        q.addBindValue(e.key());
                        q.addBindValue(qlonglong(e.value().lastWhen));
                        q.addBindValue(e.value().n);
                        q.addBindValue(
                            e.value().snr ==
                                    std::numeric_limits<int>::min()
                                ? QVariant{}
                                : QVariant{e.value().snr});
                        q.addBindValue(e.value().source);
                        q.exec();
                        r.edges += 1;
                    }
                q.prepare(QStringLiteral(
                    "INSERT INTO probes (ts, target, cmd, answered,"
                    " latency_s, present) VALUES (?,?,?,?,?,?)"));
                for (auto const &p : probes) {
                    q.addBindValue(qlonglong(p.ts));
                    q.addBindValue(p.target);
                    q.addBindValue(p.cmd);
                    q.addBindValue(p.answered);
                    q.addBindValue(p.latency > 0
                                       ? QVariant{qlonglong(p.latency)}
                                       : QVariant{});
                    q.addBindValue(p.present);
                    q.exec();
                }
                q.prepare(QStringLiteral(
                    "INSERT INTO sightings (ts, call, snr, dial, offset)"
                    " VALUES (?,?,?,?,?)"));
                for (auto const &s : sightings) {
                    q.addBindValue(qlonglong(s.ts));
                    q.addBindValue(s.call);
                    q.addBindValue(s.snr);
                    q.addBindValue(s.dial);
                    q.addBindValue(s.offset);
                    q.exec();
                }
                q.prepare(QStringLiteral(
                    "INSERT INTO edge_events (ts, hearer, heard, source)"
                    " VALUES (?,?,?,?)"));
                for (auto const &e : edgeEvents) {
                    q.addBindValue(qlonglong(e.ts));
                    q.addBindValue(e.hearer);
                    q.addBindValue(e.heard);
                    q.addBindValue(e.source);
                    q.exec();
                }
                auto setMeta = [&q](QString const &k, QString const &v) {
                    q.prepare(QStringLiteral(
                        "INSERT INTO meta (key, value) VALUES (?,?) ON "
                        "CONFLICT(key) DO UPDATE SET value=excluded.value"));
                    q.addBindValue(k);
                    q.addBindValue(v);
                    q.exec();
                };
                setMeta(QStringLiteral("schema_version"), kSchemaVersion);
                setMeta(QStringLiteral("mycall"), me);
                setMeta(QStringLiteral("mygrid"),
                        myGridIn.trimmed().toUpper());
                setMeta(QStringLiteral("mined_at"), QString::number(now));
                setMeta(QStringLiteral("sources_stamp"), stamp);
                db.commit();
                wrote = true;
            } else {
                qCWarning(miner_js8) << "[MINER] temp db open FAILED:"
                                     << db.lastError().text();
            }
        }
        QSqlDatabase::removeDatabase(conn);
    }
    if (wrote) {
        QFile::remove(intelDbPath);
        if (!QFile::rename(tmpPath, intelDbPath)) {
            qCWarning(miner_js8) << "[MINER] rename FAILED";
            wrote = false;
        }
    }

    r.ok = wrote;
    r.stations = stations.size();
    r.sightings = sightings.size();
    r.events = edgeEvents.size();
    r.elapsedMs = timer.elapsed();
    qCWarning(miner_js8).nospace()
        << "[MINER] mined " << r.directedLines << " directed lines, "
        << r.probes << " probes, " << r.stations << " stations, "
        << r.edges << " edges, " << r.sightings << " sightings, "
        << r.events << " events, " << r.logGrids.size()
        << " corroborated log grids, " << skewed << " skewed, in "
        << r.elapsedMs << " ms; ok=" << r.ok;
    return r;
}
