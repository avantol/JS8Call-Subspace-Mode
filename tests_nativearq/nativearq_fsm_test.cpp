// [TODO #107] Offline FSM harness for the V3 native-binary RX path in
// ChunkedArq::Manager — marker handling, window auto-advance, orphan
// drain, dedup/re-ACK, PCRC NACK, halt cleanup. Timer-EXPIRY scenarios
// (collect watchdog) are NOT covered here — the watchdog interval is
// wire-realistic (≥ ~37 s) and belongs to the wired-audio bench.
// Build: ./build.sh   Run: ./nativearq_fsm_test

#include "JS8_Main/ChunkedArq.h"
#include "JS8_Main/NativeBinary.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <cstdio>

using namespace NativeBinary;

static int fails = 0;
#define CHECK(cond, label)                                              \
    do {                                                                \
        bool const ok_ = (cond);                                        \
        if (!ok_) ++fails;                                              \
        printf("%s  %s\n", ok_ ? "PASS" : "FAIL", label);               \
    } while (0)

static quint64 lcg = 0x9E3779B97F4A7C15ULL;
static quint8 rnd8() {
    lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<quint8>(lcg >> 33);
}

// Test double for one Manager + captured signals.
struct Rig {
    ChunkedArq::Manager mgr;
    QStringList responses;       // ACK/NACK wire texts (wantsResponseTx)
    QByteArray delivered;        // binaryMessageReceived envelope
    QString deliveredFrom;
    int deliveredMsgId{-1};

    Rig() {
        mgr.setMyCall(QStringLiteral("WM8Q"));
        // Shrink the frame slot so post-burst re-ACK delays
        // ((frames+1) slots) resolve inside spin() instead of the
        // wire-realistic 3.75 s per slot.
        mgr.setNativeFrameMs(10);
        QObject::connect(&mgr, &ChunkedArq::Manager::wantsResponseTx,
                         [this](QString const &t) { responses << t; });
        QObject::connect(&mgr, &ChunkedArq::Manager::binaryMessageReceived,
                         [this](QString const &from, QByteArray const &env,
                                int msgId) {
                             deliveredFrom = from;
                             delivered = env;
                             deliveredMsgId = msgId;
                         });
    }
    // ACK/NACK emission rides a 250 ms audio-ramp singleShot — spin
    // the loop so the texts land in `responses`.
    void spin(int ms = 400) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    // Deliver a marker as the RX dispatch would: compose the exact
    // wire text, parse it, hand to onChunkReceived.
    bool marker(QString const &from, int msgId, int cc, int tt,
                QString const &body) {
        QString const wire = ChunkedArq::encodeChunkedData(
            from, QStringLiteral("WM8Q"), body, msgId, cc, tt);
        ChunkedArq::ParsedChunk parsed;
        if (!ChunkedArq::parseChunkedData(wire, parsed)) return false;
        mgr.onChunkReceived(from, parsed);
        return true;
    }
    // Deliver one binary frame through the real codec round trip.
    void frame(int seq, int chunkId, QByteArray const &slice) {
        Frame72 const f = encodeFrame(seq, chunkId, slice);
        int s, c;
        QByteArray p8;
        decodeFrame(f.value, f.rem, &s, &c, &p8);
        mgr.onNativeFrameReceived(s, c, p8, 1500, 0);
    }
    void sendChunkFrames(int chunkId, QByteArray const &chunk) {
        int const n = (chunk.size() + 7) / 8;
        for (int seq = 0; seq < n; ++seq)
            frame(seq, chunkId, chunk.mid(seq * 8, 8));
    }
    // ACK/NACK wire texts are addressed TO THE SENDER: "K9AVT ACK n".
    int ackCount(int cc) const {
        int n = 0;
        for (auto const &r : responses)
            if (r == QStringLiteral("K9AVT ACK %1").arg(
                         ChunkedArq::ackWireSeq(cc)))
                ++n;
        return n;
    }
    int nackCount(int cc) const {
        int n = 0;
        for (auto const &r : responses)
            if (r == QStringLiteral("K9AVT NACK %1").arg(
                         ChunkedArq::ackWireSeq(cc)))
                ++n;
        return n;
    }
};

static QByteArray randomEnvelope(int size) {
    QByteArray env(size, '\0');
    for (int i = 0; i < size; ++i) env[i] = char(rnd8());
    return env;
}

// Scenario 1+3+9: happy path, 3 chunks (150 B → 64/64/22), sparse
// markers = chunk-1 only (periodic markers deliberately OMITTED to
// prove auto-advance carries markerless chunks AND the final partial
// chunk sizes itself from TOTAL).
static void happyPathSparse() {
    Rig r;
    QByteArray const env = randomEnvelope(150);
    auto const chunks = splitIntoBinaryChunks(env, 64);
    r.marker(QStringLiteral("K9AVT"), 11, 1, 3,
             composeMarkerBody(true, env.size(),
                               payloadCrc16(chunks[0])));
    r.sendChunkFrames(1, chunks[0]);
    r.sendChunkFrames(2, chunks[1]);   // markerless (lost marker = same)
    r.sendChunkFrames(3, chunks[2]);   // final partial, markerless
    r.spin();
    CHECK(r.ackCount(1) == 1 && r.ackCount(2) == 1 && r.ackCount(3) == 1,
          "happy path: one ACK per chunk");
    CHECK(r.delivered == env && r.deliveredMsgId == 11 &&
              r.deliveredFrom == QStringLiteral("K9AVT"),
          "happy path: envelope delivered byte-identical");
    CHECK(!r.mgr.hasActiveSession(),
          "happy path: no active session after delivery");
}

// Scenario 2: chunk-1 marker beaten by its frames (pipeline lag) —
// frames orphan, marker drains them, chunk completes immediately.
static void orphanDrain() {
    Rig r;
    QByteArray const env = randomEnvelope(64);
    r.sendChunkFrames(1, env);   // no window yet -> orphaned
    r.spin(50);
    CHECK(r.responses.isEmpty(), "orphans: no ACK before marker");
    r.marker(QStringLiteral("K9AVT"), 12, 1, 1,
             composeMarkerBody(true, 64, payloadCrc16(env)));
    r.spin();
    CHECK(r.ackCount(1) == 1 && r.delivered == env,
          "orphans: marker drains orphans, completes, ACKs");
}

// Scenario 4: duplicate frames are idempotent (dedup-layer leak sim).
static void duplicates() {
    Rig r;
    QByteArray const env = randomEnvelope(64);
    r.marker(QStringLiteral("K9AVT"), 13, 1, 1,
             composeMarkerBody(true, 64, payloadCrc16(env)));
    for (int pass = 0; pass < 2; ++pass) r.sendChunkFrames(1, env);
    r.spin();
    CHECK(r.ackCount(1) == 1 && r.delivered == env,
          "duplicates: single ACK, single delivery");
}

// Scenario 5+11: ACK lost → sender retransmits marker of a collected
// chunk / of a delivered msg → re-ACK both times, no double delivery.
static void reAckPaths() {
    Rig r;
    QByteArray const env = randomEnvelope(64);
    QString const mk =
        composeMarkerBody(true, 64, payloadCrc16(env));
    r.marker(QStringLiteral("K9AVT"), 14, 1, 1, mk);
    r.sendChunkFrames(1, env);
    r.spin();
    int const before = r.ackCount(1);
    QByteArray const firstDelivery = r.delivered;
    r.marker(QStringLiteral("K9AVT"), 14, 1, 1, mk);  // retransmit
    r.spin();
    CHECK(before == 1 && r.ackCount(1) == 2 &&
              r.delivered == firstDelivery,
          "re-ACK: delivered-msg marker retransmit re-ACKs, no re-delivery");
}

// Scenario 6: PCRC mismatch → NACK; corrected retransmit recovers.
static void pcrcMismatch() {
    Rig r;
    QByteArray const env = randomEnvelope(64);
    // Marker advertises a WRONG pcrc.
    r.marker(QStringLiteral("K9AVT"), 15, 1, 1,
             composeMarkerBody(true, 64,
                               quint16(payloadCrc16(env) ^ 0x5A5A)));
    r.sendChunkFrames(1, env);
    r.spin();
    CHECK(r.nackCount(1) == 1 && r.delivered.isEmpty(),
          "pcrc: mismatch NACKs, no delivery");
    // Sender retransmits with the CORRECT marker + frames.
    r.marker(QStringLiteral("K9AVT"), 15, 1, 1,
             composeMarkerBody(true, 64, payloadCrc16(env)));
    r.sendChunkFrames(1, env);
    r.spin();
    CHECK(r.ackCount(1) == 1 && r.delivered == env,
          "pcrc: corrected retransmit recovers");
}

// Scenario 7: periodic-form marker with no chunk-1 TOTAL → silent drop.
static void midSessionJoin() {
    Rig r;
    r.marker(QStringLiteral("K9AVT"), 16, 5, 9,
             composeMarkerBody(false, 0, 0x1234));
    r.spin();
    CHECK(r.responses.isEmpty() && !r.mgr.hasActiveSession(),
          "mid-session join: periodic marker w/o TOTAL dropped silently");
}

// Scenario 8: haltAll mid-collect clears all native state.
static void haltMidCollect() {
    Rig r;
    QByteArray const env = randomEnvelope(128);
    auto const chunks = splitIntoBinaryChunks(env, 64);
    r.marker(QStringLiteral("K9AVT"), 17, 1, 2,
             composeMarkerBody(true, env.size(),
                               payloadCrc16(chunks[0])));
    r.sendChunkFrames(1, chunks[0]);
    r.spin();
    CHECK(r.mgr.hasActiveSession(), "halt: session active mid-transfer");
    r.mgr.haltAll();
    CHECK(!r.mgr.hasActiveSession(), "halt: native state cleared");
    // Frames for chunk 2 after halt → orphaned, no crash, no ACK.
    int const acksBefore = r.responses.size();
    r.sendChunkFrames(2, chunks[1]);
    r.spin();
    CHECK(r.responses.size() == acksBefore,
          "halt: post-halt frames orphan silently");
}

// Scenario 12: foreign frame with wrong CHK4 doesn't corrupt the
// window (splice guard).
static void foreignFrame() {
    Rig r;
    QByteArray const env = randomEnvelope(64);
    r.marker(QStringLiteral("K9AVT"), 18, 1, 1,
             composeMarkerBody(true, 64, payloadCrc16(env)));
    // Foreign frame: chk4=9 while window expects 1.
    r.frame(0, 9, QByteArray(8, char(0xEE)));
    r.sendChunkFrames(1, env);
    r.spin();
    CHECK(r.ackCount(1) == 1 && r.delivered == env,
          "foreign frame: wrong chk4 ignored, chunk clean");
}

// Scenario 10+K16: NB=128 chunks (K=16) work end to end.
static void k16Chunk() {
    Rig r;
    QByteArray const env = randomEnvelope(200);   // 128 + 72
    auto const chunks = splitIntoBinaryChunks(env, 128);
    r.marker(QStringLiteral("K9AVT"), 19, 1, 2,
             composeMarkerBody(true, env.size(),
                               payloadCrc16(chunks[0]),
                               /*chunkBytes=*/128));
    r.sendChunkFrames(1, chunks[0]);
    r.sendChunkFrames(2, chunks[1]);
    r.spin();
    CHECK(r.delivered == env, "K=16: 128-byte chunks deliver");
}

// Periodic marker arriving for an ALREADY-OPEN auto-advanced window
// must upgrade it (PCRC adopt) without losing collected frames...
// conservative current behavior: reopen resets the collector; frames
// retransmit via NACK/timeout. What must NOT happen: crash, double
// ACK, or mis-delivery. (Documents the accepted trade-off.)
static void periodicMarkerRefresh() {
    Rig r;
    QByteArray const env = randomEnvelope(150);
    auto const chunks = splitIntoBinaryChunks(env, 64);
    r.marker(QStringLiteral("K9AVT"), 21, 1, 3,
             composeMarkerBody(true, env.size(),
                               payloadCrc16(chunks[0])));
    r.sendChunkFrames(1, chunks[0]);
    // Periodic marker for chunk 2 arrives (normal sparse cadence
    // would be chunk 5; interval is sender policy — receiver takes
    // any).
    r.marker(QStringLiteral("K9AVT"), 21, 2, 3,
             composeMarkerBody(false, 0, payloadCrc16(chunks[1])));
    r.sendChunkFrames(2, chunks[1]);
    r.sendChunkFrames(3, chunks[2]);
    r.spin();
    CHECK(r.delivered == env,
          "periodic marker refresh: PCRC adopted, transfer delivers");
}

// --- Sender-side scenarios (sendChunkedBinary) ---

struct TxCapture {
    QString markerText;
    int chunkId{0};
    int total{0};
    QByteArray bytes;
};

// Marker cadence: 9-chunk transfer → markers on 1, 5, 9 only; chunk-1
// marker carries TOTAL/KB; ACK-driven advance; sendComplete at end.
static void senderSparseMarkers() {
    Rig r;
    QList<TxCapture> tx;
    bool complete = false;
    QObject::connect(&r.mgr,
                     &ChunkedArq::Manager::wantToTransmitNativeChunk,
                     [&tx](QString const &, QString const &m, int cc,
                           int tt, QByteArray const &b) {
                         tx.append({m, cc, tt, b});
                     });
    QObject::connect(&r.mgr, &ChunkedArq::Manager::sendComplete,
                     [&complete](QString const &, int, int, int) {
                         complete = true;
                     });
    QByteArray const env = randomEnvelope(550);  // 9 chunks @64
    auto const res = r.mgr.sendChunkedBinary(
        QStringLiteral("K9AVT"), env, 16);
    CHECK(res.ok && res.totalChunks == 9 && !tx.isEmpty() &&
              tx[0].total == 9,
          "sender: accepted, 9 chunks (total rides the TX signal)");
    // ACK each chunk as it appears.
    for (int cc = 1; cc <= 9; ++cc) {
        r.mgr.onAckReceived(QStringLiteral("K9AVT"),
                            ChunkedArq::ackWireSeq(cc));
    }
    CHECK(complete && tx.size() == 9, "sender: 9 chunks, sendComplete");
    bool cadenceOk = true;
    QByteArray rejoin;
    for (auto const &t : tx) {
        bool const wantMarker =
            (t.chunkId % NativeBinary::MARKER_INTERVAL) == 1;
        if (t.markerText.isEmpty() == wantMarker) cadenceOk = false;
        rejoin += t.bytes;
    }
    CHECK(cadenceOk, "sender: markers exactly on chunks 1,5,9");
    CHECK(rejoin == env, "sender: chunk bytes re-join to envelope");
    // Chunk-1 marker parses with TOTAL/KB (strip the ARQ tail via the
    // real parser).
    ChunkedArq::ParsedChunk pc;
    NativeBinary::MarkerInfo mi;
    CHECK(ChunkedArq::parseChunkedData(tx[0].markerText, pc) &&
              NativeBinary::parseMarkerBody(pc.body, &mi) &&
              mi.isFirstChunkForm && mi.totalBytes == env.size() &&
              mi.chunkBytes == 64,
          "sender: chunk-1 marker carries TOTAL + KB");
}

// NACK-driven retransmit resends the SAME chunk; marker present iff a
// marker chunk (chunk-1 retransmit re-sends its load-bearing marker).
static void senderNackRetransmit() {
    Rig r;
    QList<TxCapture> tx;
    QObject::connect(&r.mgr,
                     &ChunkedArq::Manager::wantToTransmitNativeChunk,
                     [&tx](QString const &, QString const &m, int cc,
                           int tt, QByteArray const &b) {
                         tx.append({m, cc, tt, b});
                     });
    QByteArray const env = randomEnvelope(150);  // 3 chunks
    r.mgr.sendChunkedBinary(QStringLiteral("K9AVT"), env, 16);
    r.mgr.onNackReceived(QStringLiteral("K9AVT"),
                         ChunkedArq::ackWireSeq(1));
    CHECK(tx.size() == 2 && tx[1].chunkId == 1 &&
              !tx[1].markerText.isEmpty(),
          "sender: chunk-1 NACK retransmit re-sends marker");
    r.mgr.onAckReceived(QStringLiteral("K9AVT"),
                        ChunkedArq::ackWireSeq(1));
    CHECK(tx.size() == 3 && tx[2].chunkId == 2 &&
              tx[2].markerText.isEmpty(),
          "sender: fresh markerless chunk sends without marker");
    r.mgr.onNackReceived(QStringLiteral("K9AVT"),
                         ChunkedArq::ackWireSeq(2));
    // [BUILD 342.9 lastAck] Retransmits ALWAYS carry the marker so a
    // receiver whose window is gone (delivered / evicted) can identify
    // the frames and re-ACK. The old "stays markerless" expectation
    // was the last-ACK hole.
    CHECK(tx.size() == 4 && tx[3].chunkId == 2 &&
              !tx[3].markerText.isEmpty(),
          "sender: retransmit gains identity marker (lastAck fix)");
    r.mgr.haltAll();
}

// [BUILD 342.9 lastAck] Andy's bench scenario 2026-07-19: final ACK
// blocked → sender retries the last chunk. The retry now carries a
// periodic-form marker; the receiver (delivered, window closed) must
// re-ACK from deliveredMsgs — NOT re-deliver, NOT reopen a window.
static void finalAckLossReAck() {
    Rig r;
    QByteArray const env = randomEnvelope(130);  // 64 + 64 + 2
    auto const chunks = splitIntoBinaryChunks(env, 64);
    r.marker(QStringLiteral("K9AVT"), 15, 1, 3,
             composeMarkerBody(true, env.size(),
                               payloadCrc16(chunks[0])));
    r.sendChunkFrames(1, chunks[0]);
    r.sendChunkFrames(2, chunks[1]);
    r.sendChunkFrames(3, chunks[2]);
    r.spin();
    QByteArray const firstDelivery = r.delivered;
    CHECK(r.ackCount(3) == 1 && firstDelivery == env,
          "lastAck: transfer delivered, final ACK sent once");
    // Sender never heard ACK 3 — its retry arrives as marker + frame.
    r.marker(QStringLiteral("K9AVT"), 15, 3, 3,
             composeMarkerBody(false, env.size(),
                               payloadCrc16(chunks[2])));
    r.sendChunkFrames(3, chunks[2]);
    r.spin();
    CHECK(r.ackCount(3) == 2 && r.delivered == firstDelivery,
          "lastAck: delivered receiver re-ACKs the marked retry");
}

// Scenario: collect-watchdog pacing. Expiries are driven directly via
// nativeCollectTimeout() (the QTimer slot's core) so no wire-realistic
// waits are needed. Fruitless expiries NACK + re-arm up to
// NATIVE_NACK_GIVEUP times, then give up — the window stays passively
// open so late frames still bind (bench 2026-07-19: unbounded re-NACK
// kept keying 4.5 min after the sender halted, garbling the next
// transfer's marker). Any accepted frame resets the give-up count.
static void watchdogNackGiveUp() {
    Rig r;
    QByteArray const env = randomEnvelope(150);  // 3 chunks
    auto const chunks = splitIntoBinaryChunks(env, 64);
    r.marker(QStringLiteral("K9AVT"), 12, 1, 3,
             composeMarkerBody(true, env.size(),
                               payloadCrc16(chunks[0])));
    QString const peer = QStringLiteral("K9AVT");
    CHECK(r.mgr.nativeCollectTimeout(peer) &&
              r.mgr.nativeCollectTimeout(peer) &&
              r.mgr.nativeCollectTimeout(peer),
          "watchdog: expiries 1-3 NACK and re-arm");
    CHECK(!r.mgr.nativeCollectTimeout(peer),
          "watchdog: expiry 4 gives up (mirrors DEFAULT_MAX_RETRIES)");
    r.spin();
    CHECK(r.nackCount(1) >= 1,
          "watchdog: at least one NACK emitted while hunting");
    // Progress resets the count: one frame in → hunting again.
    r.frame(0, 1, chunks[0].mid(0, 8));
    CHECK(r.mgr.nativeCollectTimeout(peer),
          "watchdog: accepted frame resets the give-up count");
    // Window stayed open through the give-up: chunk still completes.
    r.sendChunkFrames(1, chunks[0]);
    r.spin();
    CHECK(r.ackCount(1) == 1,
          "watchdog: passive window still collects and ACKs");
    r.mgr.haltAll();
}

// [BUILD 342.10 implicitAck] Muted-ACK bench scenario 2026-07-19:
// receiver collected chunk k-1 (its ACK was lost) and NACKs window k.
// Sender in-flight on k-1 must read NACK(k) as implicit ACK(k-1) and
// advance — not ignore it and burn retries on a chunk the receiver
// already has. Strictly single-step: NACKs further ahead stay ignored.
static void implicitAckViaNack() {
    Rig r;
    QList<TxCapture> tx;
    QObject::connect(&r.mgr,
                     &ChunkedArq::Manager::wantToTransmitNativeChunk,
                     [&tx](QString const &, QString const &m, int cc,
                           int tt, QByteArray const &b) {
                         tx.append({m, cc, tt, b});
                     });
    QByteArray const env = randomEnvelope(150);  // 3 chunks
    r.mgr.sendChunkedBinary(QStringLiteral("K9AVT"), env, 16);
    // In-flight chunk 1; receiver NACKs window 2 → implicit ACK 1,
    // fresh (markerless, retries=0) send of chunk 2.
    r.mgr.onNackReceived(QStringLiteral("K9AVT"),
                         ChunkedArq::ackWireSeq(2));
    CHECK(tx.size() == 2 && tx[1].chunkId == 2 &&
              tx[1].markerText.isEmpty(),
          "implicitAck: NACK(k) advances past in-flight k-1");
    // Two ahead is NOT possible under stop-and-wait → ignored.
    r.mgr.onNackReceived(QStringLiteral("K9AVT"),
                         ChunkedArq::ackWireSeq(4));
    CHECK(tx.size() == 2,
          "implicitAck: NACK two ahead stays ignored");
    r.mgr.haltAll();
}

// [BUILD 342.11 frameReAck] Muted-ACK bench 2026-07-19 round 3: the
// retry burst of an already-collected chunk must be answered by a
// re-ACK triggered off its LAST FRAME (precise burst-end anchor) —
// exactly one re-ACK (the marker no longer schedules a timer ACK on
// top), frames stay out of the orphan store, and the retry marker
// revives a given-up watchdog.
static void frameTriggeredReAck() {
    Rig r;
    QString const peer = QStringLiteral("K9AVT");
    QByteArray const env = randomEnvelope(150);  // 3 chunks
    auto const chunks = splitIntoBinaryChunks(env, 64);
    r.marker(peer, 16, 1, 3,
             composeMarkerBody(true, env.size(),
                               payloadCrc16(chunks[0])));
    r.sendChunkFrames(1, chunks[0]);
    r.spin();
    CHECK(r.ackCount(1) == 1, "frameReAck: chunk 1 ACKed once");
    // ACK 1 lost → sender re-airs chunk 1 (marker + 8 frames).
    r.marker(peer, 16, 1, 3,
             composeMarkerBody(true, env.size(),
                               payloadCrc16(chunks[0])));
    r.sendChunkFrames(1, chunks[0]);
    r.spin();
    CHECK(r.ackCount(1) == 2,
          "frameReAck: retry burst answered by exactly ONE re-ACK");
    // Watchdog revival: starve window 2 to give-up, then a retry
    // marker for collected chunk 1 resets the give-up budget.
    r.mgr.nativeCollectTimeout(peer);
    r.mgr.nativeCollectTimeout(peer);
    r.mgr.nativeCollectTimeout(peer);
    CHECK(!r.mgr.nativeCollectTimeout(peer),
          "frameReAck: window 2 watchdog gave up while starved");
    r.marker(peer, 16, 1, 3,
             composeMarkerBody(true, env.size(),
                               payloadCrc16(chunks[0])));
    CHECK(r.mgr.nativeCollectTimeout(peer),
          "frameReAck: retry marker revives the watchdog");
    // Transfer still completes normally.
    r.sendChunkFrames(2, chunks[1]);
    r.sendChunkFrames(3, chunks[2]);
    r.spin();
    CHECK(r.delivered == env,
          "frameReAck: transfer completes after the detour");
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(
        QStringLiteral("nativearq-test"));
    QCoreApplication::setApplicationName(QStringLiteral("fsm"));

    happyPathSparse();
    orphanDrain();
    duplicates();
    reAckPaths();
    pcrcMismatch();
    midSessionJoin();
    haltMidCollect();
    foreignFrame();
    k16Chunk();
    periodicMarkerRefresh();
    senderSparseMarkers();
    senderNackRetransmit();
    watchdogNackGiveUp();
    finalAckLossReAck();
    implicitAckViaNack();
    frameTriggeredReAck();

    printf("\n%d failures\n", fails);
    return fails ? 1 : 0;
}
