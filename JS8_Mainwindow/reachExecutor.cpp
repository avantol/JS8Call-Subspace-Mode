/** \file
 * @brief member functions of the UI_Constructor class
 *
 * [reachport2] THE REACHING EXECUTOR — MECHANICAL port of the frozen
 * python (tools/js8reach @ c6e2e7e3), written against the 80-item
 * claims-audit of 2026-08-27 (memory: reference_reachport_audit).
 * The first port was written from a mental model and silently dropped
 * 24 behaviors; this one cites its python twin at every function.
 *
 * ONE OWNER PER FACT: the hearing store + GridDb 24h tier + intel
 * corpus feed a per-attempt route book (snapshot, updated live by YES
 * answers); the assembler feeds the watchers; stopTx's completion
 * branch is the TX-end anchor.
 *
 * DECLARED DEFERRED (and only these): grid targets (#180),
 * multi-hop chains (best_routes Dijkstra; every live success was
 * 1-hop and replay showed hop count changed nothing).
 *
 * DELIBERATE IMPROVEMENTS over the python (kept from build 383):
 * dead-stays-dead watchers; attempt lines cleared at stop AND
 * verdict; [MESSAGE] placeholder selection; REACHED requires the
 * answer addressed to us (via same_station, not prefix).
 */

#include "JS8_UI/mainwindow.h"
#include "JS8_UI/SpotMapWindow.h"   // also provides Geodesic.h
                                    // (which has no include guard)
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Mode/JS8Submode.h"

#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// ---- measured constants, python twins cited --------------------------

// decide.py:89-93 (actions.rtt at Normal)
constexpr double T_SNR = 68.0, T_GRID = 82.0, T_SHOUT = 98.0,
                 T_HEARING = 112.0;
// actions.rtt(2,2,h): cost per chain length (decide.py:93)
constexpr double kTRelay[5] = {97.5, 217.5, 337.5, 457.5, 577.5};
constexpr double T_RELAY1 = 217.5;
// decide.py:162-169
constexpr int kFramesSnr = 1, kFramesGrid = 1, kFramesShout = 4,
              kFramesHearing = 4;
// attempt.py slot_end margin (reference_slot_timing)
constexpr qint64 kEnqueueMarginMs = 700;
// sim.py:53 / livemodel priors
constexpr double kUnseenLink = 0.12;
constexpr double kFwdPrior = 0.43;   // livemodel.py:329
constexpr double kAnsPrior = 0.45;   // livemodel.py:315 formula prior
// decide.py:333
constexpr double FRESH_LINK = 0.28;
// decide.py:628 — session length for the _stale model
constexpr double SESSION_S = 2700.0;
// attempt.py:320-321 — the per-move hard cap
constexpr qint64 kMoveCapMs = (90 + 16 * 15) * 1000;
// livemodel.py:355 geo pool radius; :354 pool limit
constexpr float kGeoKm = 1200.0f;
constexpr int kPoolLimit = 40;
// sim.py:85-88 — measured reciprocity by out-degree ratio
struct RecipRow { double edge, p; };
constexpr RecipRow kReciprocity[] = {
    {0.1, 0.055}, {0.5, 0.199}, {2.0, 0.311}, {10.0, 0.362},
    {1e18, 0.423}};

QString fmtClock(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC)
        .toString(QStringLiteral("HH:mm:ss.zzz"));
}

// callsign.py:88 — amateur-call shape, at least one letter in prefix
bool isAmateurCall(QString const &call) {
    QString const b = Radio::base_callsign(call).toUpper();
    static QRegularExpression const re(
        QStringLiteral("^[A-Z0-9]{1,3}[0-9][A-Z]{1,4}$"));
    if (b.isEmpty() || !re.match(b).hasMatch())
        return false;
    for (int i = 0; i < b.size() - 1; ++i)
        if (b.at(i).isLetter())
            return true;
    return false;
}
// callsign.py:105 — hyphen suffix marks a receive-only node
bool isReceiveOnly(QString const &call) {
    return call.contains(QLatin1Char('-'));
}
// callsign.py:116 is_routable
bool isRoutable(QString const &call) {
    return isAmateurCall(call) && !isReceiveOnly(call);
}
// gridtarget.py:44 GRID_RE
bool isGridSquare(QString const &s) {
    static QRegularExpression const re(
        QStringLiteral("^[A-R]{2}[0-9]{2}([A-X]{2})?$"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(s.trimmed()).hasMatch();
}

// ---- the per-attempt route book --------------------------------------
// Snapshot at reachStart from THREE sources, freshest edge wins:
// RAM hearing store (this hour), GridDb persistent tier (24 h — the
// python's GRAPH_SECS horizon, livemodel.py:55), intel corpus
// (~/.config/js8reach-intel.db, livemodel.py:150-157). Live YES
// answers update it during the attempt (told_us — decide.py:565-571).
struct BookEdge {
    qint64 whenMs = 0;
    int snr = -99;
    QString source;
};
struct BookStation {
    QString grid;
    qint64 lastSeenMs = 0;
    int snrToMe = -99;
    bool hearsMe = false;
    bool txAlive = false;
    QString source;
    double ansHabit = -1.0;   // -1 = not looked up yet
    double fwdHabit = -1.0;
};
struct LearnedEdge {
    QString hearer, heard, source;
    qint64 whenMs;
    int snr;
};
struct Book {
    QHash<QString, QHash<QString, BookEdge>> edges;  // hearer -> heard
    QHash<QString, BookStation> stations;
    QSet<QString> transmitters;   // seen keying (phantom-edge filter)
    QStringList pool;             // ordered, limited
    QVector<LearnedEdge> learned; // live YES overlays, re-applied on
                                  // every refresh
    QString intelPath;
    bool intelOpen = false;
};
Book g_book;

// [gatetrust 2026-08-27] BAND FORWARDING TRUST -- Andy: "teach the
// gate that repeated booked-relay silence devalues the book" and it
// must NOT require a restart. This is not a cached negative about any
// station (that invariant stands): it is a measured SESSION RATE --
// booked-relay asks vs observed forwards on this band, each event
// recency-weighted (half-life SESSION_S), shaped like the p_fwd habit
// formula with the 0.43 prior counting as two asks. Every relay-silent
// verdict drags it down, every forward restores it, time heals it.
// Applied as a multiplier on every relay's forward factor, so ranking,
// expected time, and the shout gate all inherit the skepticism at the
// NEXT move -- mid-attempt, and across retries in the session.
struct RelayOutcome { qint64 ms; bool forwarded; };
QHash<QString, QVector<RelayOutcome>> g_relayOutcomes;  // per band

double bandTrust(QString const &band, qint64 nowMs) {
    double n = 0.0, a = 0.0;
    for (auto const &o : g_relayOutcomes.value(band)) {
        double const w = std::pow(
            0.5, (nowMs - o.ms) / (SESSION_S * 1000.0));
        n += w;
        a += w * (o.forwarded ? 1.0 : 0.0);
    }
    return qMin(0.95, qMax(0.05,
        (kFwdPrior * 2.0 + a) / (2.0 + n)));
}

void bookAddEdge(QString const &hearer, QString const &heard,
                 qint64 whenMs, int snr, QString const &source) {
    auto &e = g_book.edges[hearer][heard];
    if (whenMs > e.whenMs) {       // freshest wins (livemodel.py:156)
        e.whenMs = whenMs;
        e.snr = snr;
        e.source = source;
    } else if (whenMs == e.whenMs && snr > e.snr) {
        e.snr = snr;
    }
}

BookEdge bookEdge(QString const &hearer, QString const &heard) {
    return g_book.edges.value(hearer).value(heard);
}

int bookOutDegree(QString const &call) {          // livemodel.py:194
    return g_book.edges.value(call).size();
}

// sim.py:90-102 — order-sensitive: sender then receiver
double reciprocityFor(int degSender, int degReceiver) {
    double const a = qMax(degSender, 1);
    double const b = qMax(degReceiver, 1);
    double const ratio = b / a;
    for (auto const &r : kReciprocity)
        if (ratio <= r.edge)
            return r.p;
    return kReciprocity[4].p;
}

double pLinkRaw(qint64 whenMs, int snr, qint64 nowMs);

} // namespace

void UI_Constructor::reachLog(QString const &line) {
    qCWarning(mainwindow_js8).noquote()
        << QStringLiteral("[REACH]")
        << fmtClock(DriftingDateTime::currentMSecsSinceEpoch())
        << line;
}

// [exactreply 2026-08-27, operator directive: "be sure to use the
// correct expected reply message when calculating the ultimate wait
// time"] Frame counts come from the PACKER fed the composed expected
// reply -- one owner for "how many frames does this message take",
// per query kind, checksum and callsign packing included. SNR rides
// as a fixed-width 3-char literal, so any value gives the same count.
int UI_Constructor::reachReplyFrames(QString const &from,
                                     QString const &text) const {
    Varicode::MessageInfo info;
    auto const frames = Varicode::buildMessageFrames(
        from, QString{}, QString{}, text,
        /*forceIdentify=*/false, /*forceData=*/false, m_nSubMode,
        &info);
    return qMax(1, int(frames.size()));
}

// attempt.py:291-294
qint64 UI_Constructor::reachSlotEndMs(qint64 tMs, int n) const {
    qint64 const p = qMax(1u, JS8::Submode::periodMS(m_nSubMode));
    qint64 const b = ((tMs - 1000) / p + 1) * p;
    return b + qint64(n) * p - kEnqueueMarginMs;
}

// livemodel.py:175-192 p_link, VERBATIM including the -99 sentinel:
// an edge with no measured SNR gets margin 0.25, never 1.0 (audit
// item 26 — the first port had this inverted). A frame-1 YES pin
// (source "yes-frame1") returns FRESH_LINK flat (decide.py:333).
double UI_Constructor::reachPLink(qint64 whenMs, int snr) const {
    return pLinkRaw(whenMs, snr,
                    DriftingDateTime::currentMSecsSinceEpoch());
}

namespace {

double pLinkRaw(qint64 whenMs, int snr, qint64 nowMs) {
    if (whenMs <= 0)
        return kUnseenLink;
    double const ageH = qMax<qint64>(0, nowMs - whenMs) / 3600000.0;
    double const di = qMax(0.0, std::cos(2.0 * M_PI * ageH / 24.0));
    double const live = 0.120 + 0.100 * std::exp(-ageH / 5.0)
                      + 0.080 * std::exp(-ageH / 192.0) * di;
    double const margin =
        (snr > -99) ? qMin(1.0, qMax(0.25, (snr + 24.0) / 18.0))
                    : 0.25;
    return qMin(0.95, live * margin);
}

double pLinkEdge(UI_Constructor const *, QString const &hearer,
                 QString const &heard) {
    auto const e = bookEdge(hearer, heard);
    if (e.source == QLatin1String("yes-frame1"))
        return FRESH_LINK;
    return pLinkRaw(e.whenMs, e.snr,
                    DriftingDateTime::currentMSecsSinceEpoch());
}

// livemodel.py:248-266 p_copy: 0.95 plateau to 900 s, then decay
// with SESSION_S, floor 0.05; never heard at all -> 0.02.
double pCopy(QString const &call, qint64 nowMs) {
    auto const s = g_book.stations.value(call);
    if (s.lastSeenMs <= 0)
        return 0.02;
    double const age = (nowMs - s.lastSeenMs) / 1000.0;
    return age <= 900.0
               ? 0.95
               : qMax(0.05, 0.95 * std::exp(-(age - 900.0) / SESSION_S));
}

// livemodel.py:268-276 p_hears_us: fresh report of us (<= 1 h) is a
// flat 0.92; otherwise the standing edge through the decay curve.
double pHearsUs(UI_Constructor const *w, QString const &call,
                QString const &me, qint64 nowMs) {
    auto const s = g_book.stations.value(call);
    if (s.hearsMe) {
        auto const e = bookEdge(call, me);
        if (e.whenMs > 0 && nowMs - e.whenMs <= 3600000)
            return 0.92;
    }
    return pLinkEdge(w, call, me);
}

// livemodel.py:214-244 p_reverse: does b hear a, GIVEN a hears b.
double pReverse(QString const &a, QString const &b) {
    double const base = reciprocityFor(bookOutDegree(a),
                                       bookOutDegree(b));
    auto const e = bookEdge(a, b);
    if (e.whenMs > 0 && e.snr > -99) {
        double const margin =
            qMin(1.0, qMax(0.25, (e.snr + 24.0) / 18.0));
        return qMin(0.95, base * (0.45 + 0.85 * margin));
    }
    return base * 0.7;   // known link, unknown quality
}

// decide.py:491-545 delivers_to: chance DEST hears STATION. Forward
// edge where reported; reciprocity from the reverse where only that
// exists; the HIGHER wins. (Audit item 2: the first port used the
// reverse edge directly AS delivery — the exact defect this function
// was written to fix on 2026-08-24, reintroduced. Not this time.)
double deliversTo(UI_Constructor const *w, QString const &station,
                  QString const &dest) {
    double const fwd = pLinkEdge(w, dest, station);
    auto const revEdge = bookEdge(station, dest);
    double const rev =
        revEdge.whenMs > 0 ? pReverse(station, dest) : 0.0;
    return qMax(fwd, rev);
}

// livemodel.py:294-317 p_ans — recency-weighted habit from the intel
// probes table; weight halves every 30 days; prior counts as three
// fresh probes. livemodel.py:319-340 p_fwd — asked/done tiers, else
// seen-forwarding, else the 0.43 prior. Both keyed on BASE call
// (the corpus's own keying).
double intelAns(QString const &call, qint64 nowMs) {
    if (!g_book.intelOpen)
        return kAnsPrior;
    QSqlQuery q(QSqlDatabase::database(QStringLiteral("reach_intel")));
    q.prepare(QStringLiteral(
        "SELECT ts, answered FROM probes WHERE target=?"));
    q.addBindValue(Radio::base_callsign(call).toUpper());
    if (!q.exec())
        return kAnsPrior;
    double n = 0.0, a = 0.0;
    while (q.next()) {
        double const w = std::pow(
            0.5, (nowMs / 1000.0 - q.value(0).toDouble())
                     / (30.0 * 86400.0));
        n += w;
        a += w * q.value(1).toDouble();
    }
    return qMin(0.95, qMax(0.05, (kAnsPrior * 3.0 + a) / (3.0 + n)));
}

double intelFwd(QString const &call) {
    if (!g_book.intelOpen)
        return kFwdPrior;
    QSqlQuery q(QSqlDatabase::database(QStringLiteral("reach_intel")));
    q.prepare(QStringLiteral(
        "SELECT relay_asked, relay_done, relay_seen FROM stations "
        "WHERE call=?"));
    q.addBindValue(Radio::base_callsign(call).toUpper());
    if (!q.exec() || !q.next())
        return kFwdPrior;
    int const asked = q.value(0).toInt();
    int const done = q.value(1).toInt();
    int const seen = q.value(2).toInt();
    if (asked)
        return qMin(0.95, (kFwdPrior * 2.0 + done) / (2.0 + asked));
    if (seen)
        return qMin(0.85, 0.55 + 0.05 * std::log1p(double(seen)));
    return kFwdPrior;
}

double stAns(QString const &call, qint64 nowMs) {
    auto &s = g_book.stations[call];
    if (s.ansHabit < 0)
        s.ansHabit = intelAns(call, nowMs);
    return s.ansHabit;
}
double stFwd(QString const &call) {
    auto &s = g_book.stations[call];
    if (s.fwdHabit < 0)
        s.fwdHabit = intelFwd(call);
    return s.fwdHabit;
}

// decide.py:295-306 _bearing + :307-324 toward — cosine preference,
// floor 0.45, and 1.0 (with "?" display) on any missing grid.
double bearingDeg(QString const &a, QString const &b) {
    auto const v = Geodesic::vector(a, b);
    return v.azimuth().isValid() ? double(v.azimuth()) : -1.0;
}
double toward(QString const &myGrid, QString const &candGrid,
              QString const &tGrid) {
    if (myGrid.size() < 4 || candGrid.size() < 4 || tGrid.size() < 4)
        return 1.0;
    double const want = bearingDeg(myGrid, tGrid);
    double const got = bearingDeg(myGrid, candGrid);
    if (want < 0 || got < 0)
        return 1.0;
    double off = std::fabs(std::fmod(want - got + 180.0, 360.0) - 180.0);
    return 0.45 + 0.55 * (1.0 + std::cos(off * M_PI / 180.0)) / 2.0;
}

// decide.py:629-632 _stale
double staleWorth(double sinceS) {
    return 1.0 - std::exp(-qMax(0.0, sinceS) / SESSION_S);
}

// decide.py:184-214 waits_for (display-only; control flow is
// slot-anchored) and the COST line of explain().
struct MoveCand {
    QString kind;        // snr | shout | relay | hearing | grid
    QString via;         // relay/hearing/grid subject (chain[0])
    QStringList chain;   // full relay chain, excl. target
    double cost = 0.0;
    double score = 0.0;  // p/cost for attempts, net gain for probes
    double p = 0.0;      // route probability (relay) or direct p
    QStringList factors; // pre-rendered factor lines
};

QString bar(double score, bool unknown) {
    if (unknown)
        return QStringLiteral("??????????");
    if (score >= 9.95)
        return QStringLiteral("**********+");
    return QString(int(std::round(score)), QLatin1Char('*'));
}
QString factorLine(QString const &name, QString const &raw,
                   double score, int width, bool unknown = false) {
    return QStringLiteral("  %1  %2  %3  %4")
        .arg(name, -width)
        .arg(score, 4, 'f', 1)
        .arg(bar(score, unknown), -11)
        .arg(raw);
}

} // namespace

// [livebook 2026-08-27] The book is NOT a per-attempt snapshot: the
// operator watched KI4RXJ hear NO1ZE at -12 DURING a run while the
// model ranked it on aged edges -- the observation reached the store
// at +2 min and the book, built at attempt start, never saw it
// ("a backwards search would reveal that" -- it did exist; it ran on
// stale data). Rebuilt from the store's layers before EVERY move
// decision; the live-learned YES overlays are re-applied on top.
void UI_Constructor::reachRefreshBook() {
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    auto const learned = g_book.learned;
    bool const intelWasOpen = g_book.intelOpen;
    g_book.edges.clear();
    g_book.stations.clear();
    g_book.transmitters.clear();
    g_book.intelOpen = intelWasOpen;
    for (auto const &s : m_spotMapWindow->activeStations(m_reach.band)) {
        BookStation b;
        b.grid = s.grid;
        b.lastSeenMs = s.lastSeenMs;
        b.snrToMe = s.snrToMe;
        b.hearsMe = s.hearsMe;
        b.txAlive = s.txAlive;
        b.source = s.source;
        g_book.stations.insert(s.call, b);
        if (s.txAlive)
            g_book.transmitters.insert(s.call);
    }
    for (auto const &e : m_spotMapWindow->allEdges(m_reach.band))
        bookAddEdge(e.hearer, e.heard, e.whenMs, e.snr, e.source);
    // Persistent tier at the 24 h horizon (livemodel.py:55 GRAPH_SECS
    // — the RAM store's 1 h pruning halves the graph, audit item 25).
    for (auto const &r : m_spotMapWindow->edges24h()) {
        if (r.band != m_reach.band)
            continue;
        bookAddEdge(r.hearer.toUpper(), r.heard.toUpper(),
                    r.when * 1000, r.snr, r.source);
        if (!g_book.stations.contains(r.hearer.toUpper())) {
            BookStation b;
            b.grid = r.hearerGrid;
            g_book.stations.insert(r.hearer.toUpper(), b);
        }
    }
    // Corpus edges (livemodel.py:150-157, :373-376): second source,
    // fresher wins; 8 of 12 candidates once lived only here.
    if (!g_book.intelOpen) {
        QString const path =
            QDir::home().filePath(QStringLiteral(
                ".config/js8reach-intel.db"));
        if (QFile::exists(path)) {
            auto db = QSqlDatabase::addDatabase(
                QStringLiteral("QSQLITE"),
                QStringLiteral("reach_intel"));
            db.setDatabaseName(path);
            db.setConnectOptions(
                QStringLiteral("QSQLITE_OPEN_READONLY"));
            g_book.intelOpen = db.open();
        }
    }
    if (g_book.intelOpen) {
        QSqlQuery q(QSqlDatabase::database(
            QStringLiteral("reach_intel")));
        q.prepare(QStringLiteral(
            "SELECT hearer, heard, last_when, snr, source FROM edges "
            "WHERE last_when > ?"));
        q.addBindValue(now / 1000 - 24 * 3600);
        if (q.exec())
            while (q.next())
                bookAddEdge(q.value(0).toString().toUpper(),
                            q.value(1).toString().toUpper(),
                            q.value(2).toLongLong() * 1000,
                            q.value(3).isNull() ? -99
                                                : q.value(3).toInt(),
                            q.value(4).toString());
    }

    for (auto const &l : learned) {
        bookAddEdge(l.hearer, l.heard, l.whenMs, l.snr, l.source);
        g_book.learned.append(l);
    }
}

void UI_Constructor::reachStart(QString const &target, int maxMoves,
                                QString const &via) {
    if (m_reach.active) {
        reachLog(QStringLiteral("already reaching %1 -- stop it first")
                     .arg(m_reach.target));
        return;
    }
    if (!m_reachTimer) {
        m_reachTimer = new QTimer(this);
        m_reachTimer->setSingleShot(true);
        m_reachTimer->setTimerType(Qt::PreciseTimer);
        connect(m_reachTimer, &QTimer::timeout,
                this, &UI_Constructor::reachTick);
    }
    QString T = target.toUpper().trimmed();        // LITERAL, never base
    if (!Radio::is_callsign(T) && !isGridSquare(T)) {
        reachLog(QStringLiteral("%1 is neither a callsign nor a grid "
                                "square").arg(T));
        return;
    }
    m_reach = ReachState{};
    m_reach.forcedVia = via.toUpper().trimmed();
    m_reach.active = true;
    m_reach.target = T;
    m_reach.maxMoves = qBound(1, maxMoves, 12);
    m_reach.startMs = DriftingDateTime::currentMSecsSinceEpoch();
    m_reach.band = m_config.bands()->find(dialFrequency());

    // Speed pin (attempt.py:102-128): Normal for the whole attempt,
    // refusal aborts, restored on every exit incl. app quit.
    if (m_nSubMode != Varicode::JS8CallNormal) {
        if (!canChangeSpeedNow()) {
            reachLog(QStringLiteral("cannot pin Normal speed (tx busy)"
                                    " -- refusing to start"));
            m_reach = ReachState{};
            return;
        }
        m_reach.savedSubmode = m_nSubMode;
        setSubmode(Varicode::JS8CallNormal);
        reachLog(QStringLiteral("speed pinned to Normal (app was at %1;"
                                " restored on exit)")
                     .arg(m_reach.savedSubmode));
    }

    reachRefreshBook();

    // [#180 gridtarget, ported verbatim from gridtarget.py] A GRID
    // is a first-class target: resolve it to the best reachable
    // occupant, screened (receive-only warns and sinks, never
    // refused), then run the normal loop. A square whose only
    // occupants are monitors that report us is a PARTIAL SUCCESS:
    // delivery-in provable, two-way not.
    if (isGridSquare(m_reach.target)) {
        QString const chosen = reachResolveGrid(m_reach.target);
        if (chosen.isEmpty()) {
            reachStop(QStringLiteral("no reachable station in %1")
                          .arg(m_reach.target));
            return;
        }
        m_reach.target = chosen;
        T = chosen;
    }

    // ---- pool: the python's two crude facts, screened -------------
    // (livemodel.py:343-413 LiveBoard: hearers of the target ordered
    // by freshness, then geography near the target; is_routable on
    // BOTH branches; phantom edges — no snr AND never seen keying —
    // are evidence of nothing, live.py:286-302; limit 40.)
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const tGrid = m_spotMapWindow->knownGrid(T);
    QVector<QPair<qint64, QString>> hearers;
    for (auto it = g_book.edges.constBegin();
         it != g_book.edges.constEnd(); ++it) {
        auto const he = it.value().constFind(T);
        if (he == it.value().constEnd())
            continue;
        if (he.value().snr <= -99 &&
            !g_book.transmitters.contains(it.key()))
            continue;                       // phantom edge
        hearers.append({he.value().whenMs, it.key()});
    }
    std::sort(hearers.begin(), hearers.end(),
              [](auto const &a, auto const &b) {
                  return a.first > b.first;
              });
    QSet<QString> seen;
    for (auto const &[whenMs, h] : hearers) {
        if (h == me || Radio::same_station(h, me) || h == T ||
            seen.contains(h) || !isRoutable(h))
            continue;
        seen.insert(h);
        g_book.pool.append(h);
    }
    if (!tGrid.isEmpty()) {
        for (auto it = g_book.stations.constBegin();
             it != g_book.stations.constEnd(); ++it) {
            QString const c = it.key();
            if (c == me || Radio::same_station(c, me) || c == T ||
                seen.contains(c) || !isRoutable(c))
                continue;
            if (it.value().grid.size() < 4)
                continue;
            auto const v = Geodesic::vector(tGrid, it.value().grid);
            if (!v.distance().isValid() || float(v.distance()) > kGeoKm)
                continue;
            seen.insert(c);
            g_book.pool.append(c);
        }
    }
    while (g_book.pool.size() > kPoolLimit)
        g_book.pool.removeLast();

    // #173 named-target screen (attempt.py:195-210): warn, never
    // refuse -- BOTH halves this time (audit item 37).
    auto const ts = g_book.stations.value(T);
    if (!ts.txAlive && ts.lastSeenMs > 0) {
        QString extra;
        if (ts.hearsMe && ts.snrToMe > -99)
            extra = QStringLiteral(" (it hears us at %1 -- delivery "
                                   "provable)")
                        .arg(Varicode::formatSNR(ts.snrToMe));
        reachLog(QStringLiteral("WARNING: %1 has never been heard "
                                "transmitting -- receive-only monitor;"
                                " expect no reply%2").arg(T, extra));
    } else if (ts.lastSeenMs <= 0) {
        reachLog(QStringLiteral("WARNING: %1 never heard on radio or "
                                "internet this session").arg(T));
    } else if (ts.source == QLatin1String("mqtt")) {
        reachLog(QStringLiteral("WARNING: %1 never heard on radio this"
                                " session -- internet-only evidence")
                     .arg(T));
    }
    reachLog(QStringLiteral("target %1 on %2; %3 candidates")
                 .arg(T, m_reach.band)
                 .arg(g_book.pool.size()));
    reachNextMove();
}

// [#180] gridtarget.py collect/rank/resolve, ported verbatim onto
// the native stores: the book's stations (grid, presence age, radio
// evidence = source "radio", rx_only = never seen as sender,
// reports_me = hears-us with a report) plus the intel corpus for
// stations the live stores aged out. Distance via Geodesic, whose
// 120 km floor on 4-char squares applies naturally. Rank class:
// radio <1h/<6h/<48h/older = 0/1/2/3; internet-only +10; receive-
// only +20 (sinks hardest, never refused). Ledger mirrors the python.
QString UI_Constructor::reachResolveGrid(QString const &square) {
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    struct Cand {
        QString call, grid, warn;
        double km = 0;
        qint64 radioAgeS = -1, anyAgeS = -1;
        bool rxOnly = false, reportsMe = false;
        int snrToMe = -99;
        int cls = 4;
    };
    QHash<QString, Cand> seen;
    for (auto it = g_book.stations.constBegin();
         it != g_book.stations.constEnd(); ++it) {
        auto const &b = it.value();
        if (b.grid.size() < 4)
            continue;
        auto const v = Geodesic::vector(square, b.grid.left(6));
        if (!v.distance().isValid() || float(v.distance()) > 250.0f)
            continue;
        Cand c;
        c.call = it.key();
        c.grid = b.grid;
        c.km = double(v.distance());
        if (b.lastSeenMs > 0) {
            c.anyAgeS = (now - b.lastSeenMs) / 1000;
            if (b.source == QLatin1String("radio"))
                c.radioAgeS = c.anyAgeS;
        }
        c.rxOnly = !b.txAlive;
        c.reportsMe = b.hearsMe && b.snrToMe > -99;
        c.snrToMe = b.snrToMe;
        c.warn = c.rxOnly ? QStringLiteral("receive-only monitor")
                 : c.radioAgeS < 0
                     ? QStringLiteral("never heard on radio")
                     : QString{};
        seen.insert(c.call, c);
    }
    if (g_book.intelOpen) {
        QSqlQuery q(QSqlDatabase::database(
            QStringLiteral("reach_intel")));
        q.prepare(QStringLiteral(
            "SELECT call, grid, last_heard FROM stations WHERE grid "
            "IS NOT NULL AND grid<>''"));
        if (q.exec())
            while (q.next()) {
                QString const call = q.value(0).toString().toUpper();
                if (seen.contains(call))
                    continue;
                QString const gr = q.value(1).toString();
                if (gr.size() < 4)
                    continue;
                auto const v = Geodesic::vector(square, gr.left(6));
                if (!v.distance().isValid() ||
                    float(v.distance()) > 250.0f)
                    continue;
                Cand c;
                c.call = call;
                c.grid = gr;
                c.km = double(v.distance());
                qint64 const lh = q.value(2).toLongLong();
                if (lh > 0)
                    c.radioAgeS = now / 1000 - lh;
                c.warn = QStringLiteral(
                    "corpus only, not seen this session");
                seen.insert(call, c);
            }
    }
    QVector<Cand> cands;
    for (auto &c : seen) {
        int cls = 4;
        if (c.radioAgeS >= 0)
            cls = c.radioAgeS < 3600 ? 0
                  : c.radioAgeS < 6 * 3600 ? 1
                  : c.radioAgeS < 48 * 3600 ? 2 : 3;
        else if (c.anyAgeS >= 0 && c.anyAgeS < 3600)
            cls = 2;
        if (c.radioAgeS < 0 && c.anyAgeS >= 0)
            cls += 10;
        if (c.rxOnly)
            cls += 20;   // cannot answer by construction: sinks hardest
        c.cls = cls;
        cands.append(c);
    }
    std::sort(cands.begin(), cands.end(),
              [](Cand const &a, Cand const &b) {
                  if (a.cls != b.cls)
                      return a.cls < b.cls;
                  return a.km < b.km;
              });
    if (cands.isEmpty()) {
        reachLog(QStringLiteral("  no station known within 250 km of "
                                "%1 on %2")
                     .arg(square, m_reach.band));
        return {};
    }
    reachLog(QStringLiteral("  %1: %2 candidates (120 km floor "
                            "applies to 4-char squares)")
                 .arg(square).arg(cands.size()));
    for (int i = 0; i < qMin(8, int(cands.size())); ++i) {
        auto const &c = cands[i];
        QString age = c.radioAgeS < 0 ? QStringLiteral("radio never")
                      : c.radioAgeS < 7200
                          ? QStringLiteral("radio %1m")
                                .arg(c.radioAgeS / 60)
                          : QStringLiteral("radio %1h")
                                .arg(c.radioAgeS / 3600.0, 0, 'f', 1);
        reachLog(QStringLiteral("    %1 %2 %3 km  %4%5")
                     .arg(c.call, -9)
                     .arg(c.grid, -8)
                     .arg(c.km, 4, 'f', 0)
                     .arg(age)
                     .arg(c.warn.isEmpty()
                              ? QString{}
                              : QStringLiteral("   !! ") + c.warn));
    }
    Cand const *pick = nullptr;
    for (auto const &c : cands)
        if (!c.rxOnly) {
            pick = &c;
            break;
        }
    for (auto const &c : cands)
        if (c.rxOnly && c.reportsMe)
            reachLog(QStringLiteral("  note: %1 (monitor, %2 km) "
                                    "reports us at %3 dB -- delivery "
                                    "into %4 provable even without a "
                                    "contact")
                         .arg(c.call)
                         .arg(c.km, 0, 'f', 0)
                         .arg(Varicode::formatSNR(c.snrToMe))
                         .arg(square));
    if (!pick) {
        reachLog(QStringLiteral("  every known station in %1 is a "
                                "receive-only monitor: nothing can "
                                "answer; partial success at best")
                     .arg(square));
        return {};
    }
    reachLog(QStringLiteral("  -> %1 (%2, %3 km) enters the normal "
                            "loop as the target%4")
                 .arg(pick->call, pick->grid)
                 .arg(pick->km, 0, 'f', 0)
                 .arg(pick->warn.isEmpty()
                          ? QString{}
                          : QStringLiteral("   (WARNED: ") + pick->warn
                                + QStringLiteral(")")));
    return pick->call;
}

void UI_Constructor::reachStop(QString const &reason) {
    if (!m_reach.active)
        return;
    reachLog(QStringLiteral("STOP: %1 (%2 transmissions, %3s total)")
                 .arg(reason)
                 .arg(m_reach.sent)
                 .arg((DriftingDateTime::currentMSecsSinceEpoch()
                       - m_reach.startMs) / 1000));
    if (m_reachTimer)
        m_reachTimer->stop();
    if (m_spotMapWindow)
        m_spotMapWindow->clearAttempts();
    m_reach.active = false;
    reachRestoreSpeed();
}

// Speed restore with retry (audit item 14: the python's atexit fired
// on every exit; the first port's "restore manually" branch leaked).
// Called from reachStop and from the destructor path; retries on a
// timer until the TX queue frees.
void UI_Constructor::reachRestoreSpeed() {
    if (m_reach.savedSubmode < 0)
        return;
    if (canChangeSpeedNow()) {
        setSubmode(m_reach.savedSubmode);
        reachLog(QStringLiteral("speed restored to %1")
                     .arg(m_reach.savedSubmode));
        m_reach.savedSubmode = -1;
        return;
    }
    reachLog(QStringLiteral("speed restore pending (tx busy) -- "
                            "retrying in 5s"));
    QTimer::singleShot(5000, this, &UI_Constructor::reachRestoreSpeed);
}

// decide.py:830-901 choose(), :636-712 attempts(), :716-780 probes()
// — the full decision, ported mechanically. 1-hop chains only
// (declared); probes hearing?/grid? included (audit item 10).
void UI_Constructor::reachNextMove() {
    if (!m_reach.active)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    // Late-forward hold (attempt.py:255-265): leave the air clear —
    // a late relay's reply may be inbound. reachOnDirected still
    // catches a REACHED during the hold.
    if (now < m_reach.holdUntilMs) {
        m_reachTimer->start(m_reach.holdUntilMs - now);
        return;
    }
    if (m_reach.moveNo >= m_reach.maxMoves) {
        reachStop(QStringLiteral("NOT REACHED -- move budget spent; "
                                 "busy or disabled, retry from the top "
                                 "later"));
        return;
    }
    reachRefreshBook();   // [livebook] the store may know more now
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const T = m_reach.target;
    QString const myGrid = m_config.my_grid().left(6);
    QString const tGrid = m_spotMapWindow->knownGrid(T);

    m_reach.kind.clear();
    m_reach.via.clear();
    m_reach.txEndMs = 0;
    m_reach.deadlineMs = 0;
    m_reach.moveCapMs = 0;
    m_reach.fwdStartedMs = m_reach.fwdDoneMs = m_reach.ansStartedMs = 0;
    m_reach.watchers.clear();

    // ---- forced first move (attempt.py:239-246) --------------------
    if (!m_reach.forcedVia.isEmpty() && m_reach.moveNo == 0) {
        QString const via = m_reach.forcedVia.toUpper();
        reachLog(QStringLiteral("[1] FORCED route via %1").arg(via));
        m_reach.kind = QStringLiteral("relay");
        m_reach.via = via;
        m_reach.chain = QStringList{via};
        m_reach.triedAt.insert(QStringLiteral("relay:") + via, now);
        reachSend(QStringLiteral("%1: %2>%3 SNR?").arg(me, via, T));
        return;
    }

    // ---- attempts: direct + every 1-hop route, scored p/cost ------
    struct Ranked { double score; MoveCand mv; };
    QVector<Ranked> ranked;

    double pd = pCopy(T, now) * pHearsUs(this, T, me, now)
                * stAns(T, now);
    qint64 const snrTried =
        m_reach.triedAt.value(QStringLiteral("snr:") + T, -1);
    double staleF = 1.0;
    if (snrTried >= 0) {
        // decide.py:641-650: a repeat only buys the chance they
        // ARRIVED; if they were present and silent, they never left.
        staleF = staleWorth((now - snrTried) / 1000.0);
        pd *= staleF * qMax(0.05, 1.0 - pCopy(T, now));
    }
    {
        MoveCand m;
        m.kind = QStringLiteral("snr");
        m.cost = T_SNR;
        m.p = pd;
        m.score = pd / T_SNR;
        int const w = QStringLiteral("worth calling again yet").size();
        m.factors
            << factorLine(QStringLiteral("they are on the air"),
                          QString::number(pCopy(T, now), 'f', 2),
                          10 * pCopy(T, now), w)
            << factorLine(QStringLiteral("they can hear us"),
                          QString::number(pHearsUs(this, T, me, now),
                                          'f', 2),
                          10 * pHearsUs(this, T, me, now), w)
            << factorLine(QStringLiteral("they answer when called"),
                          QString::number(stAns(T, now), 'f', 2),
                          10 * stAns(T, now), w)
            << factorLine(QStringLiteral("worth calling again yet"),
                          snrTried < 0 ? QStringLiteral("first call")
                                       : QString::number(staleF, 'f', 2),
                          snrTried < 0 ? 10.0 : 10 * staleF, w);
        ranked.append({m.score, m});
    }
    // [dijkstra 2026-08-27] best_routes ported VERBATIM
    // (decide.py:336-445) -- the operator's Parallel Path Tree,
    // restored after being wrongly dropped with the multi-hop scope
    // cut ("the backwards-graph part got dropped too?" -- yes, and
    // here it returns). Backward walk FROM the target over the
    // shortlist, -log(p) weights, chains to 4 hops; the first hop
    // must be a station we can actually raise; intermediate hops are
    // not restricted (we never talk to them directly).
    {
        int const maxHops = 4;
        QHash<QString, double> dist;
        QHash<QString, QStringList> route;
        QSet<QString> seenN;
        dist.insert(T, -std::log(qMax(1e-9, stAns(T, now))));
        route.insert(T, {});
        using QP = QPair<double, QString>;
        QVector<QP> heap;
        heap.append({dist.value(T), T});
        auto heapLess = [](QP const &a, QP const &b) {
            return a.first > b.first;   // min-heap via greater
        };
        std::make_heap(heap.begin(), heap.end(), heapLess);
        while (!heap.isEmpty()) {
            std::pop_heap(heap.begin(), heap.end(), heapLess);
            auto const [cost, node] = heap.takeLast();
            if (seenN.contains(node))
                continue;
            seenN.insert(node);
            if (route.value(node).size() >= maxHops)
                continue;
            for (QString const &cand : g_book.pool) {
                if (seenN.contains(cand) || cand == me ||
                    Radio::same_station(cand, me))
                    continue;
                double const out = deliversTo(this, cand, node);
                double const back = pReverse(node, cand);
                bool const observed =
                    bookEdge(cand, node).whenMs > 0 ||
                    bookEdge(node, cand).whenMs > 0;
                QString const cGrid = g_book.stations.value(cand).grid;
                double const direction =
                    observed ? 1.0 : toward(myGrid, cGrid, tGrid);
                double const trust = bandTrust(m_reach.band, now);
                double const fwdEff = qMin(0.95, qMax(
                    0.02, stFwd(cand) * (trust / kFwdPrior)));
                double const live =
                    pCopy(cand, now) * fwdEff * direction;
                double const step = out * back * live;
                if (step <= 1e-9)
                    continue;
                double const nd = cost - std::log(step);
                if (nd < dist.value(cand,
                                    std::numeric_limits<double>::max())) {
                    dist.insert(cand, nd);
                    QStringList r;
                    r << cand;
                    r += route.value(node);
                    route.insert(cand, r);
                    heap.append({nd, cand});
                    std::push_heap(heap.begin(), heap.end(), heapLess);
                }
            }
            if (seenN.size() > 960)
                break;
        }
        for (auto it = route.constBegin(); it != route.constEnd();
             ++it) {
            QString const &cand = it.key();
            QStringList const &chain = it.value();
            if (cand == T || chain.isEmpty())
                continue;
            if (!g_book.pool.contains(chain.first()))
                continue;   // first hop must be raisable-pool
            double p = std::exp(-dist.value(cand))
                       * pHearsUs(this, chain.first(), me, now);
            qint64 const tried = m_reach.triedAt.value(
                QStringLiteral("relay:") + cand, -1);
            if (tried >= 0)
                p *= staleWorth((now - tried) / 1000.0);
            if (p <= 0.0)
                continue;
            int const hops = chain.size();
            double const cost = kTRelay[qMin(4, hops)];
            QString const first = chain.first();
            QString const lastHop = chain.last();
            double const raise_ = pHearsUs(this, first, me, now);
            double const copy_ = pCopy(first, now);
            double const trust = bandTrust(m_reach.band, now);
            double const fwd_ = qMin(0.95, qMax(
                0.02, stFwd(first) * (trust / kFwdPrior)));
            double const deliver = deliversTo(this, lastHop, T);
            double back = 1.0;
            QString prev = T;
            for (int i = chain.size() - 1; i >= 0; --i) {
                back *= pReverse(prev, chain[i]);
                prev = chain[i];
            }
            QString const cGrid = g_book.stations.value(first).grid;
            bool const linkSeen =
                bookEdge(first, T).whenMs > 0 ||
                bookEdge(T, first).whenMs > 0;
            double const dir =
                linkSeen ? 1.0 : toward(myGrid, cGrid, tGrid);
            double const ans_ = stAns(T, now);
            MoveCand m;
            m.kind = QStringLiteral("relay");
            m.via = first;
            m.chain = chain;
            m.cost = cost;
            m.p = p;
            m.score = p / cost;
            int const w =
                (first + QStringLiteral(" forwards when asked")).size();
            bool const dirUnknown = !linkSeen && cGrid.size() < 4;
            m.factors
                << factorLine(QStringLiteral("we can raise ") + first,
                              QString::number(raise_, 'f', 2),
                              10 * raise_, w)
                << factorLine(first + QStringLiteral(" is on the air"),
                              QString::number(copy_, 'f', 2),
                              10 * copy_, w)
                << factorLine(first
                                  + QStringLiteral(" forwards when asked"),
                              QString::number(fwd_, 'f', 2), 10 * fwd_,
                              w)
                << factorLine(QStringLiteral("target hears ") + lastHop,
                              QString::number(deliver, 'f', 2),
                              10 * deliver, w)
                << factorLine(QStringLiteral("the reply gets back"),
                              QString::number(back, 'f', 2), 10 * back,
                              w)
                << factorLine(QStringLiteral("target answers at all"),
                              QString::number(ans_, 'f', 2), 10 * ans_,
                              w)
                << factorLine(QStringLiteral("toward the target"),
                              dirUnknown ? QStringLiteral("unknown")
                                         : QString::number(dir, 'f', 2),
                              10 * dir, w, dirUnknown)
                << factorLine(QStringLiteral("route length"),
                              QStringLiteral("%1 hop").arg(hops),
                              10.0 / hops, w);
            ranked.append({m.score, m});
        }
    }

    std::sort(ranked.begin(), ranked.end(),
              [](auto const &a, auto const &b) {
                  return a.score > b.score;
              });

    // expected_time (decide.py:705-712)
    auto const expectedTime = [](QVector<Ranked> const &r) {
        double total = 0.0, alive = 1.0;
        for (auto const &x : r) {
            total += alive * x.mv.cost;
            alive *= 1.0 - qMin(0.95, x.score * x.mv.cost);
        }
        return total + alive * 600.0;
    };
    double const nowET = expectedTime(ranked);

    {
        double const trust = bandTrust(m_reach.band, now);
        if (trust < kFwdPrior * 0.9) {
            int n = g_relayOutcomes.value(m_reach.band).size();
            reachLog(QStringLiteral("    band forwarding trust %1 "
                                    "(%2 recent asks) -- booked "
                                    "relays devalued")
                         .arg(trust, 0, 'f', 2)
                         .arg(n));
        }
    }

    // ---- step 1: the direct call, once (decide.py:859-866) --------
    if (snrTried < 0) {
        auto const &m = ranked.isEmpty() || ranked[0].mv.kind
                                != QLatin1String("snr")
            ? [&]() -> MoveCand const & {
                  for (auto const &x : ranked)
                      if (x.mv.kind == QLatin1String("snr"))
                          return x.mv;
                  return ranked[0].mv;
              }()
            : ranked[0].mv;
        m_reach.kind = QStringLiteral("snr");
        m_reach.triedAt.insert(QStringLiteral("snr:") + T, now);
        reachExplain(&m);
        reachSend(QStringLiteral("%1: %2 SNR?").arg(me, T));
        return;
    }

    // ---- step 2: the shout, priced as a COMPOSITE ATTEMPT --------
    // [gateswap 2026-08-27, operator-approved replacement of the
    // python's VOI gate] The ported expected-time-difference formula
    // could never fire at real probability scales (measured: gate
    // score -79 at prior trust, -92 devalued -- the devalue moved it
    // the WRONG way). The shout is an attempt-enabler, so price it in
    // the SAME p/t currency as every attempt: shout (98s) + follow-up
    // ask via whoever answers (218s), with the measured 29% answer
    // rate, the answerer's PROVEN aliveness (it just keyed) and
    // hears-us (it decoded our shout: fresh-report 0.92), delivery
    // pinned at FRESH_LINK, the way home at the comparable-degree
    // reciprocity bucket x unknown-quality (0.311 x 0.7 -- both
    // measured constants from the corpus), and the answerer's forward
    // habit at the BAND TRUST (unknown station, current band). Fires
    // when this composite out-scores the best untried booked relay --
    // so collapsed trust or floor-grade booked routes escalate to
    // asking the band NATURALLY, and a strong fresh booked route
    // keeps the 98 seconds in hand.
    if (!m_reach.triedAt.contains(QStringLiteral("shout"))) {
        double const trust = bandTrust(m_reach.band, now);
        double const qAns = 0.29;              // measured answer rate
        double const backUnknown = 0.311 * 0.7;
        double const pRoute = 0.92 * 0.95 * trust * FRESH_LINK
                              * backUnknown * stAns(T, now);
        double const shoutScore =
            qAns * pRoute / (T_SHOUT + T_RELAY1);
        double bestBooked = 0.0;
        QString bestCall;
        for (auto const &x : ranked) {
            if (x.mv.kind != QLatin1String("relay"))
                continue;
            if (m_reach.triedAt.contains(
                    QStringLiteral("relay:") + x.mv.via))
                continue;
            if (x.score > bestBooked) {
                bestBooked = x.score;
                bestCall = x.mv.via;
            }
        }
        if (shoutScore > bestBooked) {
            MoveCand m;
            m.kind = QStringLiteral("shout");
            m.cost = T_SHOUT;
            int const w = QStringLiteral(
                "route via an answerer, per 1000s").size();
            m.factors
                << factorLine(QStringLiteral("someone answers this"),
                              QString::number(qAns, 'f', 2),
                              10 * qAns, w)
                << factorLine(
                       QStringLiteral("route via an answerer, "
                                      "per 1000s"),
                       QString::number(shoutScore * 1000.0, 'f', 4),
                       10 * qMin(1.0, shoutScore / qMax(1e-9,
                                                        bestBooked)),
                       w)
                << factorLine(
                       QStringLiteral("best booked relay, per 1000s"),
                       bestCall.isEmpty()
                           ? QStringLiteral("none untried")
                           : QStringLiteral("%1 %2")
                                 .arg(bestCall)
                                 .arg(bestBooked * 1000.0, 0, 'f', 4),
                       10 * qMin(1.0, bestBooked / qMax(1e-9,
                                                        shoutScore)),
                       w)
                << factorLine(QStringLiteral("band forwarding trust"),
                              QString::number(trust, 'f', 2),
                              10 * trust / 0.95, w);
            m_reach.kind = QStringLiteral("shout");
            m_reach.triedAt.insert(QStringLiteral("shout"), now);
            reachExplain(&m);
            reachSend(QStringLiteral("%1: @ALLCALL QUERY CALL %2?")
                          .arg(me, T));
            return;
        }
        reachLog(QStringLiteral("    shout waits: composite %1/1000s "
                                "vs booked %2/1000s (%3, trust %4)")
                     .arg(shoutScore * 1000.0, 0, 'f', 4)
                     .arg(bestBooked * 1000.0, 0, 'f', 4)
                     .arg(bestCall.isEmpty()
                              ? QStringLiteral("none")
                              : bestCall)
                     .arg(trust, 0, 'f', 2));
    }

    // ---- step 3: routes, best first (decide.py:884-887) -----------
    for (auto const &x : ranked) {
        if (x.mv.kind != QLatin1String("relay"))
            continue;
        QString const relKey = x.mv.chain.isEmpty()
                                   ? x.mv.via : x.mv.chain.last();
        qint64 const tried = m_reach.triedAt.value(
            QStringLiteral("relay:") + relKey, -1);
        if (tried >= 0)
            continue;   // stale-scored above; fresh candidates first
        m_reach.kind = QStringLiteral("relay");
        m_reach.via = x.mv.via;
        m_reach.chain = x.mv.chain.isEmpty()
                            ? QStringList{x.mv.via} : x.mv.chain;
        m_reach.triedAt.insert(QStringLiteral("relay:") + x.mv.via,
                               now);
        m_reach.pastVias.append(x.mv.via);
        reachExplain(&x.mv);
        reachSend(QStringLiteral("%1: %2>%3 SNR?")
                      .arg(me, m_reach.chain.join(QLatin1Char('>')),
                           T));
        return;
    }

    // ---- step 4: probes (decide.py:762-777, :889-895) -------------
    {
        MoveCand bestProbe;
        double bestScore = 0.0;
        // HEARING? at the least-known raisable station
        QString least;
        double lp = 1e9;
        for (QString const &c : g_book.pool) {
            if (m_reach.askedHearing.contains(c))
                continue;
            double const p = pLinkEdge(this, c, T);
            if (p < lp) {
                least = c;
                lp = p;
            }
        }
        if (!least.isEmpty()) {
            QVector<Ranked> boosted = ranked;
            for (auto &x : boosted)
                if (x.mv.via == least)
                    x.score *= 2.0;
            std::sort(boosted.begin(), boosted.end(),
                      [](auto const &a, auto const &b) {
                          return a.score > b.score;
                      });
            double const score =
                0.35 * (nowET - expectedTime(boosted)) - T_HEARING;
            if (score > bestScore) {
                bestScore = score;
                bestProbe.kind = QStringLiteral("hearing");
                bestProbe.via = least;
                bestProbe.cost = T_HEARING;
            }
        }
        // GRID? at the first unlocated of the top 12
        QString unloc;
        for (int i = 0; i < qMin(12, int(g_book.pool.size())); ++i) {
            QString const c = g_book.pool[i];
            if (!m_reach.askedGrid.contains(c) &&
                g_book.stations.value(c).grid.size() < 4) {
                unloc = c;
                break;
            }
        }
        if (!unloc.isEmpty()) {
            QVector<Ranked> boosted = ranked;
            for (auto &x : boosted)
                if (x.mv.via == unloc)
                    x.score *= 1.4;
            std::sort(boosted.begin(), boosted.end(),
                      [](auto const &a, auto const &b) {
                          return a.score > b.score;
                      });
            double const score =
                0.5 * (nowET - expectedTime(boosted)) - T_GRID;
            if (score > bestScore) {
                bestScore = score;
                bestProbe.kind = QStringLiteral("grid");
                bestProbe.via = unloc;
                bestProbe.cost = T_GRID;
            }
        }
        if (bestScore > 0) {
            m_reach.kind = bestProbe.kind;
            m_reach.via = bestProbe.via;
            if (bestProbe.kind == QLatin1String("hearing")) {
                m_reach.askedHearing.append(bestProbe.via);
                reachSend(QStringLiteral("%1: %2 HEARING?")
                              .arg(me, bestProbe.via));
            } else {
                m_reach.askedGrid.append(bestProbe.via);
                reachSend(QStringLiteral("%1: %2 GRID?")
                              .arg(me, bestProbe.via));
            }
            return;
        }
    }

    // ---- nothing left (decide.py:896-901) -------------------------
    reachStop(QStringLiteral("nothing left to try -- every option is "
                             "spent; busy or disabled, retry from the "
                             "top later"));
}

// explain() (decide.py:263-291): COST line from waits_for
// (decide.py:184-214, display-only) + the factor table.
void UI_Constructor::reachExplain(void const *cand) {
    auto const &m = *static_cast<MoveCand const *>(cand);
    double const period = JS8::Submode::periodMS(m_nSubMode) / 1000.0;
    double const check = period + 3, escalate = period + 6;
    double const abandon =
        (m.kind == QLatin1String("shout") ? 20 : 16) * period;
    reachLog(QStringLiteral("  COST   %1 s   check +%2s / escalate "
                            "+%3s / abandon +%4s%5")
                 .arg(m.cost, 0, 'f', 0)
                 .arg(check, 0, 'f', 1)
                 .arg(escalate, 0, 'f', 1)
                 .arg(abandon, 0, 'f', 1)
                 .arg(m.kind == QLatin1String("relay")
                          ? QStringLiteral("   (the +58s forward is "
                                           "the checkpoint)")
                          : QString{}));
    for (auto const &f : m.factors)
        reachLog(f);
}

void UI_Constructor::reachSend(QString const &wire) {
    m_reach.moveNo += 1;
    m_reach.lastWire = wire;
    m_reach.moveCapMs = DriftingDateTime::currentMSecsSinceEpoch()
                        + kMoveCapMs;   // attempt.py:320-321
    reachLog(QStringLiteral("[%1] SEND %2")
                 .arg(m_reach.moveNo).arg(wire));
    enqueueMessage(PriorityHigh, wire, -1, nullptr);
    m_reach.sent += 1;
    processTxQueue();
}

void UI_Constructor::reachOnTxComplete() {
    if (!m_reach.active || m_reach.txEndMs != 0)
        return;
    m_reach.txEndMs = DriftingDateTime::currentMSecsSinceEpoch();
    m_reach.deadlineMs = reachSlotEndMs(m_reach.txEndMs, 1);
    reachLog(QStringLiteral("    TX-END; deadline %1")
                 .arg(fmtClock(m_reach.deadlineMs)));
    reachArmTimer();
}

void UI_Constructor::reachOnFrame(ActivityDetail const &d) {
    if (!m_reach.active || m_reach.txEndMs == 0)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    QString const up = d.text.trimmed().toUpper();
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const T = m_reach.target;

    if (m_reach.kind == QLatin1String("relay") &&
        m_reach.fwdDoneMs != 0 && m_reach.retFwdMs == 0 &&
        now - m_reach.fwdDoneMs > 5000 &&
        up.startsWith(m_reach.via + QLatin1String(":"))) {
        // the via STARTING TX again after its forward = the answer
        // coming back; give it the PACKED frame count of the exact
        // expected re-forward ("VIA: ME> SNR -NN *DE* TARGET")
        m_reach.retFwdMs = now;
        int const retF = reachReplyFrames(
            m_reach.via,
            me + QStringLiteral("> SNR -15 *DE* ") + T);
        m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                  reachSlotEndMs(now, retF));
        reachLog(QStringLiteral("    return forward STARTED +%1s -- "
                                "deadline extended")
                     .arg((now - m_reach.txEndMs) / 1000));
        reachArmTimer();
    }
    if (m_reach.kind == QLatin1String("relay") &&
        m_reach.fwdStartedMs == 0 &&
        up.startsWith(m_reach.via + QLatin1String(":"))) {
        m_reach.fwdStartedMs = now;
        g_relayOutcomes[m_reach.band].append({now, true});
        m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                  reachSlotEndMs(now, 5));
        reachLog(QStringLiteral("    forward STARTED +%1s -- deadline "
                                "extended")
                     .arg((now - m_reach.txEndMs) / 1000));
        reachArmTimer();
    }

    bool addressed = false;
    if (m_reach.kind == QLatin1String("shout")) {
        static QRegularExpression const re(
            QStringLiteral("^[A-Z0-9/]+:\\s*%1\\b")
                .arg(QRegularExpression::escape(me)));
        addressed = re.match(up).hasMatch();
    } else if (m_reach.kind == QLatin1String("hearing") ||
               m_reach.kind == QLatin1String("grid")) {
        addressed = up.startsWith(m_reach.via + QLatin1String(":"));
    } else {
        addressed = up.startsWith(T + QLatin1String(":"));
    }

    auto keyFor = [this](int off) {
        for (auto it = m_reach.watchers.begin();
             it != m_reach.watchers.end(); ++it)
            if (qAbs(it.key() - off) <= 5)
                return it.key();
        return off;
    };
    int const k = keyFor(d.offset);
    if (m_reach.watchers.contains(k) && m_reach.watchers[k].dead)
        return;   // dead stays dead
    if (addressed) {
        if (!m_reach.watchers.contains(k)) {
            reachLog(QStringLiteral("    reply started at %1 Hz: %2")
                         .arg(k).arg(up.left(40)));
            // Frame-1 YES learning (attempt.py:381-384, the N6GRG
            // lesson): the claim rides in frame 1; a died reply still
            // teaches. Pinned at FRESH_LINK via a marker edge.
            static QRegularExpression const yes(
                QStringLiteral(":\\s*%1\\s+YES\\b")
                    .arg(QRegularExpression::escape(me)));
            if (m_reach.kind == QLatin1String("shout") &&
                yes.match(up).hasMatch()) {
                QString const who =
                    up.section(QLatin1Char(':'), 0, 0).trimmed();
                bookAddEdge(who, T, now, -99,
                            QStringLiteral("yes-frame1"));
                g_book.learned.append(
                    {who, T, QStringLiteral("yes-frame1"), now, -99});
            }
        }
        auto &w = m_reach.watchers[k];
        w.lastMs = now;
        if (m_reach.deadlineMs)
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, 1));
        if (m_reach.ansStartedMs == 0) {
            m_reach.ansStartedMs = now;
            int frames;
            if (m_reach.kind == QLatin1String("shout"))
                frames = kFramesShout;   // variable riders; measured max
            else if (m_reach.kind == QLatin1String("hearing"))
                frames = kFramesHearing; // list length varies
            else if (m_reach.kind == QLatin1String("grid"))
                frames = reachReplyFrames(
                    m_reach.via,
                    me + QStringLiteral(" GRID FN20"));
            else if (m_reach.kind == QLatin1String("relay"))
                frames = reachReplyFrames(
                    T, m_reach.via + QStringLiteral("> ") + me
                           + QStringLiteral(" SNR -15"));
            else
                frames = reachReplyFrames(
                    T, me + QStringLiteral(" SNR -15"));
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, frames - 1));
            reachLog(QStringLiteral("    answer STARTED +%1s -- "
                                    "deadline extended")
                         .arg((now - m_reach.txEndMs) / 1000));
        }
        reachArmTimer();
    } else if (m_reach.watchers.contains(k) &&
               !m_reach.watchers[k].done) {
        m_reach.watchers[k].lastMs = now;
        if (m_reach.deadlineMs)
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, 1));
        reachArmTimer();
    }
}

// Assembled directed messages. NO tx-end guard (audit item 12): an
// answer decoded before TX.COMPLETE still counts, python verbatim.
void UI_Constructor::reachOnDirected(CommandDetail const &d,
                                     QString const &line) {
    if (!m_reach.active)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    QString const from = d.from.trimmed().toUpper();
    QString const T = m_reach.target;
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const off =
        m_reach.txEndMs
            ? QStringLiteral("+%1s")
                  .arg((now - m_reach.txEndMs) / 1000)
            : QString{};

    for (auto it = m_reach.watchers.begin();
         it != m_reach.watchers.end(); ++it) {
        if (qAbs(it.key() - d.offset) <= 5)
            it.value().done = true;
    }

    // Late forward from a PAST via (attempt.py:422-428): hold TX two
    // slots for its reply -- the K4GMX +104 s lesson.
    if (m_reach.pastVias.contains(from) &&
        from != m_reach.via &&
        line.toUpper().contains(QStringLiteral("*DE* ") + me)) {
        m_reach.holdUntilMs = now + 30000;
        reachLog(QStringLiteral("    LATE FORWARD from %1: holding TX "
                                "two slots for its reply").arg(from));
    }

    // The relay-returned answer: "VIA: WM8Q> SNR -x *DE* TARGET" --
    // from the via (or a past via), carrying *DE* target, addressed
    // to us. This IS the target's answer arriving; the from==target
    // gate alone missed it (the python had the same gap -- every
    // prior success was a directly-audible target).
    bool const viaReturn =
        (from == m_reach.via || m_reach.pastVias.contains(from)) &&
        line.toUpper().contains(QStringLiteral("*DE* ") + T) &&
        Radio::same_station(d.to.trimmed().toUpper()
                                .section(QLatin1Char('>'), 0, 0), me);
    if (viaReturn ||
        (Radio::same_station(from, T) &&
         Radio::same_station(d.to.trimmed().toUpper()
                                 .section(QLatin1Char('>'), 0, 0),
                             me))) {
        qint64 const total = (now - m_reach.startMs) / 1000;
        reachLog(QStringLiteral("ANSWER %1: %2")
                     .arg(off, line.left(70)));
        reachLog(QStringLiteral("REACHED %1 on move %2, %3s total, "
                                "%4 transmissions")
                     .arg(T).arg(m_reach.moveNo).arg(total)
                     .arg(m_reach.sent));
        QStringList path;
        if (m_reach.kind == QLatin1String("relay"))
            path = m_reach.chain;
        else if (viaReturn)
            path << from;
        path << T;
        reachPlaceTemplate(path);
        reachStop(QStringLiteral("REACHED"));
        return;
    }
    if (m_reach.kind == QLatin1String("relay") &&
        from == m_reach.via &&
        line.toUpper().contains(QStringLiteral("*DE* ") + me)) {
        m_reach.fwdDoneMs = now;
        // [returncheck 2026-08-27, operator's rule verbatim]: "if we
        // don't hear the first ('from') frame by the end of the 3rd
        // frame after the period boundary that immediately follows
        // [the] replying station's end of tx, then we assume no
        // message is coming." The target starts tx at the boundary
        // after the forward's end of tx (deterministic, every
        // measured reply); its answer runs the PACKED frame count
        // ("TARGET: VIA> ME SNR -NN", from the varicode packer); the
        // boundary after its end of tx opens a 3-frame window for
        // the via's return to start. That one window covers BOTH a
        // late-starting target (+1/+2 slots shifts the return within
        // it -- KQ4DNM's +2 lands on its last frame) and via queue
        // displacement, and it sits at the stage we can HEAR.
        QString const me_ =
            m_config.my_callsign().trimmed().toUpper();
        // Chain-aware ([dijkstra]): after chain[0]'s forward, each
        // middle hop re-forwards (packed frames each, starting at the
        // next boundary), the target answers the LAST hop, and the
        // answer retraces the middle hops before chain[0]'s return --
        // whose start gets the operator's 3-frame window. Every count
        // from the packer on the composed message for that leg.
        int middle = 0;
        for (int i = 1; i < m_reach.chain.size(); ++i) {
            QStringList tail = m_reach.chain.mid(i + 1);
            tail << m_reach.target;
            middle += reachReplyFrames(
                m_reach.chain[i],
                tail.join(QLatin1Char('>'))
                    + QStringLiteral(" SNR? *DE* ") + me_);
        }
        QString const lastHop = m_reach.chain.isEmpty()
                                    ? m_reach.via
                                    : m_reach.chain.last();
        int const ansF = reachReplyFrames(
            m_reach.target,
            lastHop + QStringLiteral("> ") + me_
                + QStringLiteral(" SNR -15"));
        int innerRet = 0;
        for (int i = m_reach.chain.size() - 1; i >= 1; --i)
            innerRet += reachReplyFrames(
                m_reach.chain[i],
                me_ + QStringLiteral("> SNR -15 *DE* ")
                    + m_reach.target);
        int const window = middle + ansF + innerRet + 3;
        m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                  reachSlotEndMs(now, window));
        reachLog(QStringLiteral("    forward complete %1; %2-frame "
                                "answer via %3 hop(s); %4's return "
                                "must start within 3 frames after -- "
                                "reject at %5 slots")
                     .arg(off, QString::number(ansF))
                     .arg(m_reach.chain.size())
                     .arg(m_reach.via)
                     .arg(window));
        reachArmTimer();
        return;
    }
    // YES learned: update the ROUTE BOOK live (the python's told_us;
    // the store gets it via bindCallQueryReply on the same pass).
    static QRegularExpression const yesRe(
        QStringLiteral(":\\s*%1\\s+YES(?:\\s+([+-]\\d+)\\s*\\((\\d+)"
                       "([SMHD]))?")
            .arg(QRegularExpression::escape(
                m_config.my_callsign().trimmed().toUpper())));
    auto const ym = yesRe.match(line.toUpper());
    if (m_reach.kind == QLatin1String("shout") && ym.hasMatch()) {
        qint64 whenMs = now;
        int snr = -99;
        if (!ym.captured(1).isEmpty()) {
            snr = ym.captured(1).toInt();
            qint64 const n = ym.captured(2).toLongLong();
            QChar const u = ym.captured(3).isEmpty()
                                ? QLatin1Char('S')
                                : ym.captured(3).at(0);
            qint64 const secs = u == QLatin1Char('D') ? n * 86400
                                : u == QLatin1Char('H') ? n * 3600
                                : u == QLatin1Char('M') ? n * 60 : n;
            whenMs = now - secs * 1000;
        }
        bookAddEdge(from, T, whenMs, snr,
                    QStringLiteral("querycall-live"));
        g_book.learned.append(
            {from, T, QStringLiteral("querycall-live"), whenMs, snr});
        reachLog(QStringLiteral("    learned %1: %2")
                     .arg(off, line.left(60)));
    }
}

void UI_Constructor::reachArmTimer() {
    if (!m_reach.active || !m_reachTimer)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    if (m_reach.deadlineMs == 0) {
        // waiting for TX-END; the hard cap still guards the move
        if (m_reach.moveCapMs)
            m_reachTimer->start(
                qMax<qint64>(5, m_reach.moveCapMs - now));
        return;
    }
    qint64 next = qMin(m_reach.deadlineMs,
                       m_reach.moveCapMs ? m_reach.moveCapMs
                                         : m_reach.deadlineMs);
    for (auto const &w : m_reach.watchers) {
        if (w.done || w.dead)
            continue;
        qint64 const death = reachSlotEndMs(w.lastMs, 1);
        if (death > now)
            next = qMin(next, death);
    }
    m_reachTimer->start(qMax<qint64>(5, next - now));
}

void UI_Constructor::reachTick() {
    if (!m_reach.active)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();

    // hold expiry re-enters the move chooser
    if (m_reach.holdUntilMs && now >= m_reach.holdUntilMs &&
        m_reach.txEndMs == 0 && m_reach.kind.isEmpty()) {
        m_reach.holdUntilMs = 0;
        reachNextMove();
        return;
    }

    // per-move hard cap (attempt.py:320-321): the ONLY absolute bound
    bool const capped =
        m_reach.moveCapMs && now >= m_reach.moveCapMs;

    if (!capped) {
        if (m_reach.txEndMs == 0) {
            reachArmTimer();
            return;
        }
        if (m_reach.ansStartedMs && !m_reach.watchers.isEmpty() &&
            now < m_reach.deadlineMs) {
            bool allSettled = true;
            int done = 0;
            for (auto it = m_reach.watchers.begin();
                 it != m_reach.watchers.end(); ++it) {
                auto &w = it.value();
                if (w.done) { ++done; continue; }
                if (!w.dead && now >= reachSlotEndMs(w.lastMs, 1)) {
                    w.dead = true;
                    reachLog(QStringLiteral("    reply at %1 Hz died "
                                            "(missing frame)")
                                 .arg(it.key()));
                }
                if (!w.dead) {
                    allSettled = false;
                    break;
                }
            }
            if (allSettled) {
                reachLog(QStringLiteral("    all %1 started replies "
                                        "finished (%2 assembled, %3 "
                                        "died) -- shortcutting %4s")
                             .arg(m_reach.watchers.size()).arg(done)
                             .arg(m_reach.watchers.size() - done)
                             .arg((m_reach.deadlineMs - now) / 1000));
                m_reach.deadlineMs = now;
            }
        }
        if (now < m_reach.deadlineMs) {
            reachArmTimer();
            return;
        }
    }

    // pre-verdict tally (attempt.py:501-505)
    if (!m_reach.watchers.isEmpty()) {
        int done = 0;
        for (auto const &w : m_reach.watchers)
            if (w.done)
                ++done;
        reachLog(QStringLiteral("    replies: %1 started, %2 "
                                "assembled, %3 died")
                     .arg(m_reach.watchers.size()).arg(done)
                     .arg(m_reach.watchers.size() - done));
    }
    QString state;
    if (m_reach.kind == QLatin1String("relay") &&
        m_reach.fwdStartedMs == 0)
        g_relayOutcomes[m_reach.band].append(
            {DriftingDateTime::currentMSecsSinceEpoch(), false});
    if (capped)
        state = QStringLiteral("move hard cap (330s) reached");
    else if (m_reach.kind == QLatin1String("relay") &&
             m_reach.fwdStartedMs == 0)
        state = QStringLiteral("relay silent -- never keyed");
    else if (m_reach.retFwdMs != 0)
        state = QStringLiteral("return forward started but never "
                               "assembled -- frames lost");
    else if (m_reach.fwdDoneMs != 0)
        state = QStringLiteral("forwarded, but no return started -- "
                               "target silent, or the relaying "
                               "station has nothing to send");
    else if (m_reach.ansStartedMs != 0) {
        bool allDone = !m_reach.watchers.isEmpty();
        for (auto const &w : m_reach.watchers)
            if (!w.done)
                allDone = false;
        state = allDone
            ? QStringLiteral("every started reply assembled -- the "
                             "target itself never answered")
            : QStringLiteral("answer started but never assembled -- "
                             "frames lost");
    } else if (m_reach.kind == QLatin1String("shout"))
        state = QStringLiteral("the whole group is busy or disabled");
    else
        state = (m_reach.kind == QLatin1String("relay")
                     ? m_reach.via
                 : m_reach.kind == QLatin1String("hearing") ||
                         m_reach.kind == QLatin1String("grid")
                     ? m_reach.via
                     : m_reach.target)
                + QStringLiteral(" is busy or disabled");
    reachLog(QStringLiteral("    VERDICT +%1s: %2 -- next move")
                 .arg(m_reach.txEndMs
                          ? (now - m_reach.txEndMs) / 1000
                          : 0)
                 .arg(state));
    if (m_spotMapWindow)
        m_spotMapWindow->clearAttempts();
    reachNextMove();
}

void UI_Constructor::reachPlaceTemplate(QStringList const &path) {
    clearCallsignSelected();
    QString const tpl = path.join(QLatin1Char('>'))
                        + QStringLiteral(" [MESSAGE]");
    ui->extFreeTextMsgEdit->setPlainText(tpl);
    QTextCursor c = ui->extFreeTextMsgEdit->textCursor();
    if (int const at = tpl.indexOf(QStringLiteral("[MESSAGE]"));
        at >= 0) {
        c.setPosition(at);
        c.setPosition(at + 9, QTextCursor::KeepAnchor);
    } else {
        c.movePosition(QTextCursor::End);
    }
    ui->extFreeTextMsgEdit->setTextCursor(c);
    reachLog(QStringLiteral("template ready in the outgoing box: %1")
                 .arg(tpl));
}
