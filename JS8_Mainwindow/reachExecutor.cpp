/** \file
 * @brief member functions of the UI_Constructor class
 *
 * [reachport] THE REACHING EXECUTOR — the frozen python attempt.py
 * (tools/js8reach @ c6e2e7e3) ported into the app, where every fact it
 * needs already has ONE owner:
 *   - the hearing store IS the model (bindCallQueryReply files YES
 *     answers as age-graded backdated edges — the whole python
 *     pin/gate/learned patchwork dissolves into that existing path);
 *   - the assembler IS the watcher substrate (per-frame RX.ACTIVITY
 *     bookkeeping becomes reachOnFrame/reachOnDirected hooks);
 *   - stopTx's completion branch IS the TX-end anchor (signal end,
 *     12.64 s into the last frame — never ALL.TXT, whose stamps are
 *     floored AND pre-key).
 *
 * THE PROVEN RULES (every one field-validated on air, 2026-08-26/27):
 *   moves    direct SNR? -> @ALLCALL QUERY CALL (once) -> ranked
 *            1-hop relays; exhausted -> STOP (busy-or-disabled);
 *            NEVER repeat the direct call; retry is manual, from the
 *            top. (Every live success was 1-hop; the replay showed
 *            hop count changed nothing — multi-hop is a later add.)
 *   speed    Normal pinned for the attempt, operator's speed restored
 *            on every exit; refuse to run at an unverifiable speed.
 *   timing   single extending deadline per move, slot-anchored, NO
 *            grace periods — measured constants only (12.64 s decode
 *            instant, 0.7 s enqueue margin, the submode period).
 *   watchers one per started reply TO US: the addressed first frame
 *            admits it at its offset; every frame on a live reply
 *            extends one slot; next-slot-frameless = dead; ALL
 *            done-or-dead = verdict NOW; ceiling = the reply-frames
 *            budget (snr/grid 1, shout/ask_call 4).
 *   success  the target's own transmission addressed to us; the
 *            proven path lands in the outgoing box as
 *            "VIA>DEST [MESSAGE]" (identical to the map relay
 *            builder, placeholder selected).
 *
 * Ledger lines go to the diag log under [REACH] in the exact format
 * the python printed — the operator reads them the same way.
 *
 * NOT YET PORTED (deliberate scope of the first build): grid targets
 * (#180 resolution), multi-hop chains, forward-habit learning from
 * the mined corpus (a flat measured prior stands in).
 */

#include "JS8_UI/mainwindow.h"
#include "JS8_UI/SpotMapWindow.h"   // also provides Geodesic.h
                                    // (which has no include guard)
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Mode/JS8Submode.h"

#include <QTimer>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// Reply-frame budgets (ceilings for the deadline; watchers usually
// close the wait first). Measured: long "YES +NN (AGE)" = 4 Normal
// frames (KD9RJT shout 2026-08-27 01:39Z).
constexpr int kFramesSnr = 1;
constexpr int kFramesShout = 4;

// Enqueue margin: socket write + frame prep before a boundary.
// 0.58 s proven to make the boundary; 0.7 s is the working margin
// (reference_slot_timing).
constexpr qint64 kEnqueueMarginMs = 700;

// Forwarding prior for a stranger relay. Corpus-derived; tonight's
// live tally (3 of 7 distinct relays asked actually forward) is
// consistent with it.
constexpr double kFwdPrior = 0.43;

// Answering prior (does anyone answer a directed call at all).
constexpr double kAnsPrior = 0.45;

// No-evidence link floor (python UNSEEN_LINK).
constexpr double kUnseenLink = 0.12;

QString fmtClock(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC)
        .toString(QStringLiteral("HH:mm:ss.zzz"));
}

} // namespace

void UI_Constructor::reachLog(QString const &line) {
    qCWarning(mainwindow_js8).noquote()
        << QStringLiteral("[REACH]")
        << fmtClock(DriftingDateTime::currentMSecsSinceEpoch())
        << line;
}

// slot_end(t, n): the instant we must act to own the boundary after
// t's slot plus n further slots — that boundary minus the enqueue
// margin. Same arithmetic the python proved, parameterized on the
// CURRENT period (Normal while pinned).
qint64 UI_Constructor::reachSlotEndMs(qint64 tMs, int n) const {
    qint64 const p = qMax(1u, JS8::Submode::periodMS(m_nSubMode));
    qint64 const b = ((tMs - 1000) / p + 1) * p;
    return b + qint64(n) * p - kEnqueueMarginMs;
}

// The link curve — livemodel.p_link verbatim: fast decay, diurnal
// bump, scaled by signal margin. Grades a dated edge.
double UI_Constructor::reachPLink(qint64 whenMs, int snr) const {
    if (whenMs <= 0)
        return kUnseenLink;
    double const ageH = qMax<qint64>(
        0, DriftingDateTime::currentMSecsSinceEpoch() - whenMs)
        / 3600000.0;
    double const di = qMax(0.0, std::cos(2.0 * M_PI * ageH / 24.0));
    double const live = 0.120 + 0.100 * std::exp(-ageH / 5.0)
                      + 0.080 * std::exp(-ageH / 192.0) * di;
    double margin = 1.0;
    if (snr > -99)
        margin = qMin(1.0, qMax(0.25, (snr + 24.0) / 18.0));
    return qMin(0.95, live * margin);
}

void UI_Constructor::reachStart(QString const &target, int maxMoves) {
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
    QString const T = target.toUpper().trimmed();  // LITERAL — never base()
    if (!Radio::is_callsign(T)) {
        reachLog(QStringLiteral("%1 is not a callsign -- grid targets "
                                "are not ported yet").arg(T));
        return;
    }
    m_reach = ReachState{};
    m_reach.active = true;
    m_reach.target = T;
    m_reach.maxMoves = qBound(1, maxMoves, 12);
    m_reach.startMs = DriftingDateTime::currentMSecsSinceEpoch();
    m_reach.band = m_config.bands()->find(dialFrequency());

    // Speed pin: first messages to a station are ALWAYS Normal.
    // Refusal aborts — never transmit at the wrong speed (the N9EAT
    // lesson: Subspace-speed asks that no relay could decode).
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

    // The target's own standing in the store, for the operator's
    // pre-flight read (the #173 screen's spirit; full rx-only screen
    // rides with the grid-target port).
    auto const stations = m_spotMapWindow->activeStations(m_reach.band);
    for (auto const &s : stations) {
        if (s.call == T && s.hearsMe)
            reachLog(QStringLiteral("%1 hears us at %2 per the store")
                         .arg(T)
                         .arg(Varicode::formatSNR(s.snrToMe)));
    }
    reachLog(QStringLiteral("target %1 on %2").arg(T, m_reach.band));
    reachNextMove();
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
    if (m_reach.savedSubmode >= 0 && canChangeSpeedNow()) {
        setSubmode(m_reach.savedSubmode);
        reachLog(QStringLiteral("speed restored to %1")
                     .arg(m_reach.savedSubmode));
    } else if (m_reach.savedSubmode >= 0) {
        reachLog(QStringLiteral("speed restore to %1 DEFERRED (tx busy)"
                                " -- restore manually")
                     .arg(m_reach.savedSubmode));
    }
    m_reach.active = false;
}

// One move: decide, explain, transmit. The decision order is the
// proven policy; the factor lines mirror the python ledger so the
// operator's reading habits carry over unchanged.
void UI_Constructor::reachNextMove() {
    if (!m_reach.active)
        return;
    if (m_reach.moveNo >= m_reach.maxMoves) {
        reachStop(QStringLiteral("NOT REACHED -- move budget spent; "
                                 "busy or disabled, retry from the top "
                                 "later"));
        return;
    }
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const T = m_reach.target;

    m_reach.kind.clear();
    m_reach.via.clear();
    m_reach.txEndMs = 0;
    m_reach.deadlineMs = 0;
    m_reach.fwdStartedMs = m_reach.fwdDoneMs = m_reach.ansStartedMs = 0;
    m_reach.watchers.clear();

    if (!m_reach.triedDirect) {
        m_reach.triedDirect = true;
        m_reach.kind = QStringLiteral("snr");
        reachSend(QStringLiteral("%1: %2 SNR?").arg(me, T));
        return;
    }
    if (!m_reach.triedShout) {
        // Once per attempt, BY POLICY (operator: "don't repeat QUERY
        // CALL at all").
        m_reach.triedShout = true;
        m_reach.kind = QStringLiteral("shout");
        reachSend(QStringLiteral("%1: @ALLCALL QUERY CALL %2?")
                      .arg(me, T));
        return;
    }

    // Ranked 1-hop relays. Pool = the python's two crude facts, which
    // beat every wider net in replay (6.48% vs 4.40%): stations
    // observed hearing the target, plus stations near the target's
    // grid. Fresh QUERY CALL answers are ALREADY in the store as
    // backdated edges (bindCallQueryReply) — no second learning path.
    QString const tGrid = m_spotMapWindow->knownGrid(T);
    struct Cand {
        QString call, grid;
        double p = 0.0;
        double raise = 0.0, alive = 0.0, link = 0.0, dir = 1.0;
        qint64 linkWhenMs = 0;
        int linkSnr = -99;
    };
    QHash<QString, Cand> cands;
    auto const stations = m_spotMapWindow->activeStations(m_reach.band);
    QHash<QString, SpotMapWindow::StationView> byCall;
    for (auto const &s : stations)
        byCall.insert(s.call, s);

    for (auto const &h : m_spotMapWindow->hearersOf(m_reach.band, T)) {
        if (h.hearer == me || h.hearer == T)
            continue;
        Cand c;
        c.call = h.hearer;
        c.grid = h.grid;
        c.linkWhenMs = h.whenMs;
        c.linkSnr = h.snr;
        cands.insert(c.call, c);
    }
    if (!tGrid.isEmpty()) {
        for (auto const &s : stations) {
            if (s.call == me || s.call == T || cands.contains(s.call))
                continue;
            if (s.grid.isEmpty())
                continue;
            auto const v = Geodesic::vector(tGrid, s.grid);
            if (!v.distance().isValid() ||
                float(v.distance()) > 1200.0f)
                continue;
            Cand c;
            c.call = s.call;
            c.grid = s.grid;
            cands.insert(c.call, c);
        }
    }

    QString const myGrid = m_config.my_grid().left(6);
    Cand best;
    for (auto it = cands.begin(); it != cands.end(); ++it) {
        Cand &c = it.value();
        if (m_reach.triedRelays.contains(c.call))
            continue;
        auto const sv = byCall.value(c.call);
        // can we raise it: their report of our signal, else floor
        c.raise = sv.hearsMe
                      ? qMin(1.0, qMax(0.25, (sv.snrToMe + 24.0) / 18.0))
                      : kUnseenLink;
        // is it on the air: presence recency (half-life 30 min)
        double ageMin = sv.lastSeenMs > 0
                            ? (now - sv.lastSeenMs) / 60000.0 : 1e9;
        c.alive = qMax(0.05, std::exp(-ageMin / 43.0));
        // does the target hear it: the dated edge, graded by the curve
        c.link = reachPLink(c.linkWhenMs, c.linkSnr);
        // direction prior — only for links nobody has observed
        if (c.linkWhenMs == 0 && !tGrid.isEmpty() &&
            !c.grid.isEmpty() && !myGrid.isEmpty()) {
            auto const toT = Geodesic::vector(myGrid, tGrid);
            auto const toC = Geodesic::vector(myGrid, c.grid);
            if (toT.azimuth().isValid() && toC.azimuth().isValid()) {
                double d = std::fabs(double(toT.azimuth())
                                     - double(toC.azimuth()));
                if (d > 180.0)
                    d = 360.0 - d;
                c.dir = qMax(0.3, 1.0 - d / 180.0);
            }
        }
        c.p = c.raise * c.alive * kFwdPrior * c.link * kAnsPrior * c.dir;
        if (c.p > best.p)
            best = c;
    }

    if (best.call.isEmpty()) {
        reachStop(QStringLiteral("nothing left to try -- every option "
                                 "is spent; busy or disabled, retry "
                                 "from the top later"));
        return;
    }
    m_reach.kind = QStringLiteral("relay");
    m_reach.via = best.call;
    m_reach.triedRelays.append(best.call);
    reachLog(QStringLiteral("  we can raise %1 %2 / on air %3 / "
                            "target hears it %4 / toward target %5")
                 .arg(best.call)
                 .arg(best.raise, 0, 'f', 2)
                 .arg(best.alive, 0, 'f', 2)
                 .arg(best.link, 0, 'f', 2)
                 .arg(best.dir, 0, 'f', 2));
    reachSend(QStringLiteral("%1: %2>%3 SNR?").arg(me, best.call, T));
}

void UI_Constructor::reachSend(QString const &wire) {
    m_reach.moveNo += 1;
    m_reach.lastWire = wire;
    reachLog(QStringLiteral("[%1] SEND %2")
                 .arg(m_reach.moveNo).arg(wire));
    // enqueueMessage runs noteAttemptFromText itself — the red line
    // machinery is shared with every other producer, one path.
    enqueueMessage(PriorityHigh, wire, -1, nullptr);
    m_reach.sent += 1;
    processTxQueue();
}

// TX-end anchor: called from stopTx's completion branch (signal end).
void UI_Constructor::reachOnTxComplete() {
    if (!m_reach.active || m_reach.txEndMs != 0)
        return;
    // Only our own move's completion counts; unrelated TX (HB etc.)
    // is possible in principle but the executor holds the queue.
    m_reach.txEndMs = DriftingDateTime::currentMSecsSinceEpoch();
    m_reach.deadlineMs = reachSlotEndMs(m_reach.txEndMs, 1);
    reachLog(QStringLiteral("    TX-END; deadline %1")
                 .arg(fmtClock(m_reach.deadlineMs)));
    reachArmTimer();
}

// Per decoded frame — the watcher substrate.
void UI_Constructor::reachOnFrame(ActivityDetail const &d) {
    if (!m_reach.active || m_reach.txEndMs == 0)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    QString const up = d.text.trimmed().toUpper();
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const T = m_reach.target;

    // forward started: the via keys anything
    if (m_reach.kind == QLatin1String("relay") &&
        m_reach.fwdStartedMs == 0 &&
        up.startsWith(m_reach.via + QLatin1String(":"))) {
        m_reach.fwdStartedMs = now;
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
        return;   // dead stays dead -- no resurrection, no extension
    if (addressed) {
        if (!m_reach.watchers.contains(k))
            reachLog(QStringLiteral("    reply started at %1 Hz: %2")
                         .arg(k).arg(up.left(40)));
        auto &w = m_reach.watchers[k];
        w.lastMs = now;
        if (m_reach.deadlineMs)
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, 1));
        if (m_reach.ansStartedMs == 0) {
            m_reach.ansStartedMs = now;
            int const frames =
                m_reach.kind == QLatin1String("shout") ? kFramesShout
                                                       : kFramesSnr;
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, frames - 1));
            reachLog(QStringLiteral("    answer STARTED +%1s -- "
                                    "deadline extended")
                         .arg((now - m_reach.txEndMs) / 1000));
        }
        reachArmTimer();
    } else if (m_reach.watchers.contains(k) &&
               !m_reach.watchers[k].done) {
        // continuation frame on a watched offset: one more slot
        m_reach.watchers[k].lastMs = now;
        if (m_reach.deadlineMs)
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, 1));
        reachArmTimer();
    }
}

// Per assembled directed message — answers, forwards, learning.
// (YES answers hit the hearing store via bindCallQueryReply on this
// same pass; the router reads the store live, so there is nothing to
// copy here.)
void UI_Constructor::reachOnDirected(CommandDetail const &d,
                                     QString const &line) {
    if (!m_reach.active || m_reach.txEndMs == 0)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    QString const from = d.from.trimmed().toUpper();
    QString const T = m_reach.target;
    QString const me = m_config.my_callsign().trimmed().toUpper();

    // watcher at this offset is finished
    for (auto it = m_reach.watchers.begin();
         it != m_reach.watchers.end(); ++it) {
        if (qAbs(it.key() - d.offset) <= 5)
            it.value().done = true;
    }

    if (from == T && d.to.startsWith(me)) {
        qint64 const total = (now - m_reach.startMs) / 1000;
        reachLog(QStringLiteral("ANSWER +%1s: %2")
                     .arg((now - m_reach.txEndMs) / 1000)
                     .arg(line.left(70)));
        reachLog(QStringLiteral("REACHED %1 on move %2, %3s total, "
                                "%4 transmissions")
                     .arg(T).arg(m_reach.moveNo).arg(total)
                     .arg(m_reach.sent));
        QStringList path;
        if (m_reach.kind == QLatin1String("relay"))
            path << m_reach.via;
        path << T;
        reachPlaceTemplate(path);
        reachStop(QStringLiteral("REACHED"));
        return;
    }
    if (m_reach.kind == QLatin1String("relay") &&
        from == m_reach.via &&
        line.toUpper().contains(QStringLiteral("*DE* ") + me)) {
        m_reach.fwdDoneMs = now;
        m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                  reachSlotEndMs(now, 3));
        reachLog(QStringLiteral("    forward complete +%1s; target "
                                "has three slots")
                     .arg((now - m_reach.txEndMs) / 1000));
        reachArmTimer();
        return;
    }
    // shout answers, for the ledger (the store already banked them)
    if (m_reach.kind == QLatin1String("shout") &&
        line.toUpper().contains(QStringLiteral(": %1 YES").arg(me)))
        reachLog(QStringLiteral("    learned +%1s: %2")
                     .arg((now - m_reach.txEndMs) / 1000)
                     .arg(line.left(60)));
}

void UI_Constructor::reachArmTimer() {
    if (!m_reach.active || !m_reachTimer || m_reach.deadlineMs == 0)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    qint64 next = m_reach.deadlineMs;
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

    // all-finished shortcut: every started reply assembled or dead
    if (m_reach.ansStartedMs && !m_reach.watchers.isEmpty() &&
        now < m_reach.deadlineMs) {
        bool allSettled = true;
        int done = 0;
        for (auto it = m_reach.watchers.begin();
             it != m_reach.watchers.end(); ++it) {
            auto &w = it.value();
            if (w.done) { ++done; continue; }
            if (!w.dead && now >= reachSlotEndMs(w.lastMs, 1)) {
                w.dead = true;   // permanent from here on
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
                                    "finished (%2 assembled, %3 died) "
                                    "-- shortcutting %4s")
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

    // verdict — name the dead stage
    QString state;
    if (m_reach.kind == QLatin1String("relay") &&
        m_reach.fwdStartedMs == 0)
        state = QStringLiteral("relay silent -- never keyed");
    else if (m_reach.fwdDoneMs != 0)
        state = QStringLiteral("forwarded, but the target never "
                               "answered");
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
                     ? m_reach.via : m_reach.target)
                + QStringLiteral(" is busy or disabled");
    reachLog(QStringLiteral("    VERDICT +%1s: %2 -- next move")
                 .arg((now - m_reach.txEndMs) / 1000).arg(state));
    if (m_spotMapWindow)
        m_spotMapWindow->clearAttempts();
    reachNextMove();
}

// The deliverable: the proven path in the outgoing box, identical to
// the map relay builder's output ([relaysel]): full inventory is
// clear selection + set text + select the placeholder.
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
