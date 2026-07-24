/**
 * @file ChunkedArq.h
 * @brief Chunked stop-and-wait ARQ (automatic repeat request) for
 *        reliable multi-frame message delivery over JS8 Subspace.
 *
 * Port of the validated Python prototype at ~/subspace-arq/ (Phase 1).
 * Wire format and state-machine semantics are bit-for-bit compatible
 * with the prototype so we can interop during transition.
 *
 * Wire form for a DATA chunk:
 *
 *   "<FROM>: <TO> <body> #NN.CC/TT.HHHH"
 *
 *     NN   = message ID, 2 digits (01-99, wraps to 01)
 *     CC   = chunk index, 2 digits, 1-based (01..TT)
 *     TT   = total chunks, 2 digits (01..MAX_CHUNKS_PER_MESSAGE)
 *     HHHH = uppercase hex CRC-16-CCITT-FALSE of the body
 *
 * ACK / NACK reuse existing directed frame format with the chunk ID
 * carried in the wire's 6-bit extra field (encoded via packNum's
 * existing +31 offset, decoded by JS8 to a 1-31 integer in the API
 * EXTRA field).
 *
 * See `/home/john/.claude/plans/functional-swimming-avalanche.md` for
 * the full design rationale.
 */
#ifndef CHUNKEDARQ_H
#define CHUNKEDARQ_H

#include <functional>
#include <optional>

#include <QHash>
#include <QList>
#include <QLoggingCategory>
#include <QObject>
#include <QSet>
#include <QString>

#include "JS8_Main/NativeBinary.h"
#include <QTimer>

Q_DECLARE_LOGGING_CATEGORY(chunkedarq_js8)

namespace ChunkedArq {

// --- Protocol constants -----------------------------------------------------
//
// 1-based numbering throughout for operator-readability ("msg 1, chunk
// 2 of 5" beats "msg 0, chunk 1 of 5"). Internal storage uses chunk_id
// directly as a map key, so 1-based works as well as 0-based.

// ARQ protocol level advertised by this build. Increment when on-wire
// semantics change in a way receivers need to know about. Level 1 =
// Phase-1 chunked stop-and-wait + per-chunk CRC + bounded retries +
// auto-enable on RX. Level 2 (planned) will add HARQ Chase combining.
// Replied verbatim as "<from>: <to> YES <level>" to "QUERY ARQ?".
// [BUILD 339 TODO #103+#104] Level 2 = file-transfer wire-format V2
// (F/V2 single-envelope binary header) AND chunk rollover (super-
// messages beyond 31 chunks, up to MAX_CHUNKS_ROLLOVER — the DATA
// header's CC/TT fields were always 2-digit text; only the ACK/NACK
// seq, carried in the directed frame's 1..31 numeric extra, wraps
// modulo-31, unambiguous under stop-and-wait). Both capabilities
// shipped together as one level (Andy 2026-07-17: no fielded
// level-2-without-rollover population exists — dev builds only).
// Advertised in the QUERY ARQ? "YES <level>" reply; senders use V2
// and >31 chunks only for peers whose cached level is >= 2.
// [TODO #107] Level 3 = level 2 (V2 format + 99-chunk rollover) PLUS
// native-binary F/V3 transfers (raw 72-bit Subspace frames, sparse
// markers). Advertised via the existing "YES <level>" reply; V3 TX is
// gated on peer >= 3 AND Subspace mode (silent V2 fallback otherwise).
constexpr int    ARQ_PROTOCOL_LEVEL      = 3;

constexpr int    MSG_ID_MIN              = 1;
constexpr int    MSG_ID_MAX              = 99;
constexpr int    MAX_CHUNKS_PER_MESSAGE  = 31;    // level<3 peers: bounded by wire ACK extra width (1..31)
// [BUILD 339 TODO #104] Level>=3 peers: DATA header CC/TT are 2-digit
// text → 99 is the natural ceiling; ACK seq wraps modulo 31 on the
// wire. ~99 chunks ≈ 26 min best-case air time — the practical bound.
constexpr int    MAX_CHUNKS_ROLLOVER     = 99;
// Wire-side modulus for ACK/NACK seq (packNum numeric-extra range).
constexpr int    ACK_SEQ_MODULUS         = 31;
// Map an absolute 1-based chunk id onto the 1..31 ACK wire range.
constexpr int    ackWireSeq(int const absSeq) {
    return ((absSeq - 1) % ACK_SEQ_MODULUS) + 1;
}

// [BUILD 341 policyGate] Classification of outgoing-box text for ARQ
// eligibility. PURE FUNCTION — text in, class out; no UI, no config,
// no Varicode packing (packing does TX framing only; it has no role
// in classification). Policy encodes Andy's approved chart (TODO
// #105): see the implementation's tables. Covered by the offline
// test matrix in scratchpad/arqgate_test.cpp — extend the matrix
// BEFORE changing the tables.
enum class TextClass {
    FreeText,         // ARQ-eligible
    DirectedCommand,  // ARQ refused (has its own send semantics)
    ArqExempt,        // MSG / MSG TO: / relay — ARQ explicitly wraps
};
TextClass classifyOutgoingText(QString const &boxText);

// [BUILD 341 peerResolve] The INDIVIDUAL-callsign addressee named by
// the text itself (leading token after normalization + FROM-prefix
// strip), or empty. The effective ARQ peer is: selected callsign IF
// it is a valid individual peer, OTHERWISE this — i.e., evaluate the
// final message as interpreted at TX time, where the text's
// addressee wins (a selected @group is irrelevant to a text that
// names its own peer; operator-observed 2026-07-17).
QString leadingPeerOf(QString const &boxText);

// [BUILD 341 sendPeer] THE effective-peer rule, in one place: the
// selected callsign IF it is a valid individual peer, otherwise the
// text's own leading-callsign addressee, otherwise empty. Every
// consumer of "which single station would ACK this?" — the enable
// gate, the file/link resolver, AND the startTx ARQ intercept — must
// call THIS and nothing else. The 2026-07-17 regression chain was
// three hand-rolled copies of this rule drifting apart (the send
// path only fell back on an EMPTY selection, so a selected @group
// silently killed the ARQ wrap at TX time while the menu showed
// enabled).
QString effectivePeer(QString const &selected, QString const &boxText);
constexpr int    MAX_CHUNK_BODY_CHARS    = 60;    // body per chunk; tune for failure rate

// [TURNHOLD 2026-07-21] V3 inter-chunk turnaround hold. The next
// chunk's keyup waits until the peer's ACK/NACK has finished AIRING
// (end-of-air derived from the decode's absPos; the async decoder can
// decode a frame before its own tail airs) plus the peer's 250 ms
// post-roll plus a TX→RX turnaround margin. FT2 frame = 105 symbols ×
// 288 samples = 30240 ring samples @ 12 kHz.
constexpr qint64 TURNAROUND_FRAME_SAMPLES = 30240;
constexpr int    TURNAROUND_TAIL_MS       = 1750;  // 250 post-roll + 1500 margin
constexpr int    CHUNKED_MARKER_LEN      = 14;    // len("#NN.CC/TT.HHHH")

constexpr int    DEFAULT_MAX_RETRIES     = 3;
// Receiver-side mirror of DEFAULT_MAX_RETRIES for the V3 collect
// watchdog: after this many consecutive no-progress expiries the
// window stops NACKing (it stays passively open for late frames; the
// assembly-evict timer does the final cleanup). Bench 2026-07-19:
// unbounded re-NACK kept keying for 4.5 min after the sender halted,
// garbling the next transfer's marker mid-decode.
constexpr int    NATIVE_NACK_GIVEUP      = 3;
constexpr int    DEFAULT_ACK_TIMEOUT_MS  = 12000; // 2x Subspace cycle + ACK decode slack (FT2 fallback)

// Post-TX-done ACK budget per JS8 submode.
//   budget ≈ 2 × cycle_s + decode_slack
//   covers: receiver decode lag + receiver cycle-align + receiver TX
//           frame + sender decode lag.
// Submode IDs from Varicode.h (NOT contiguous — 0,1,2,4,16).
inline int ackTimeoutMsForSubmode(int submode) {
    switch (submode) {
        case 0:  return 36000; // JS8CallNormal — 15 s cycle
        case 1:  return 25000; // JS8CallFast   — 10 s cycle
        case 2:  return 16000; // JS8CallTurbo  —  6 s cycle
        case 4:  return 66000; // JS8CallSlow   — 30 s cycle
        // [BUILD 342.6] Subspace 12 s → 16 s: the V3 deferred-ACK
        // worst case (receiver last-frame decode lag ~2-5 s + 250 ms
        // audio ramp + slot align ≤3.75 s + 3.75 s ACK frame + our
        // decode lag ~2-5 s) brushes 12 s — bench round 2 lost the
        // ACK-vs-timeout race by <1 s on every chunk-2 attempt. The
        // longer budget is FREE in the happy path (an arriving ACK
        // cancels the timer); it only paces real-loss retries.
        case 16: return 16000; // JS8CallFT2 / Subspace — 3.75 s cycle
        default: return 25000; // Unknown — pick a middling value
    }
}

// [BUILD 341 capTimeout] QUERY ARQ? negotiation window per submode —
// i.e. how long we wait before RE-ASKING (and, after one retry, before
// falling back to V1 for that transfer).
//
// Was a flat 20 s — sized for Subspace only (operator 2026-07-17:
// "too short for most speeds. we can even operate at Slow"). The
// negotiation is two ACK-shaped exchanges: our 2-frame query out,
// the peer's 2-frame "YES <level>" back (at OUR submode since
// arqSpeed).
//
// [2026-07-23] Shortened by one period: 2 × ACK budget − TWO periods
// (was − one). Operator: the retry delay "is 4 periods, can be 3" —
// which this makes exact for the legacy speeds. Resulting windows,
// with the retry delay expressed in periods:
//   Subspace 24.50 s (6.5 P)   Turbo 20.00 s (3.3 P)
//   Fast     30.00 s (3.0 P)   Normal 42.00 s (2.8 P)
//   Slow     72.00 s (2.4 P)
// NOTE the earlier comment claimed "Subspace 20.25 s" — stale since
// the Subspace ACK budget went 12 s → 16 s (build 342.6), which had
// silently pushed this window to 28.25 s.
//
// FLOOR: the window can NOT simply be "3 periods" for Subspace. The
// exchange needs our 2-frame query (7.5 s) + peer turnaround + the
// peer's 2-frame reply (7.5 s) + decode lag ≈ 20 s minimum; 3 periods
// (11.25 s) would expire mid-negotiation and force a bogus V1
// fallback every time. 24.5 s keeps ~4.5 s of margin. Subspace reads
// "long" in periods only because its ACK budget is deliberately
// generous relative to its very short 3.75 s period.
inline int capQueryTimeoutMsForSubmode(int submode) {
    int const periodMs = [submode]() {
        switch (submode) {
            case 0:  return 15000; // JS8CallNormal
            case 1:  return 10000; // JS8CallFast
            case 2:  return 6000;  // JS8CallTurbo
            case 4:  return 30000; // JS8CallSlow
            case 16: return 3750;  // JS8CallFT2 / Subspace
            default: return 15000; // Unknown — Normal
        }
    }();
    return 2 * ackTimeoutMsForSubmode(submode) - 2 * periodMs;
}

// TX-idle poll: how often the manager re-checks "has JS8Call finished
// my TX yet?" while a chunk is in flight. 1 s is fine — finer than
// Python's 2 s, since we're already in-process and the cost is just a
// pair of bool reads.
constexpr int    TX_IDLE_POLL_INTERVAL_MS = 1000;

// Safety cap: if JS8Call never reports idle (e.g. user pressed Stop,
// stuck PTT, etc.) arm the ACK timer anyway after this long so the
// chunked-send state machine doesn't wedge.
//
// [DYNAMIC CAP 2026-06-13 build 262]
// Was a fixed 90 000 ms — designed for FT2/Subspace where 7-frame
// chunks take ~26 s. But in slower legacy modes the cap fires before
// TX is actually complete: Normal-mode (15 s cycle) 7-frame chunks
// need ~105 s, Slow-mode (30 s cycle) needs ~210 s. Operator observed
// this 2026-06-13 in a Normal-mode ARQ run where every chunk hit the
// cap at exactly 90 s and the ACK timer started counting down ~15 s
// before TX actually finished. ACKs still landed in time but the
// 36 s ACK budget was burned through more than half before the
// receiver could even decode the chunk.
//
// New per-submode cap: `max(90 s, 8 × cycle_s)`. 8× cycle is the
// upper bound for a wire body that maxes out at MAX_CHUNK_BODY_CHARS
// + envelope + marker (~85 chars) packed into JS8's ~13-char frames
// = ~7 frames, with one extra cycle of cushion. The 90 s floor keeps
// the cap at its original value for fast modes (FT2, Turbo, Fast)
// where 8×cycle is smaller than the original constant.
inline int txIdleMaxWaitMsForSubmode(int submode) {
    int const cycleS = [submode]() {
        switch (submode) {
            case 0:  return 15;  // JS8CallNormal
            case 1:  return 10;  // JS8CallFast
            case 2:  return 6;   // JS8CallTurbo
            case 4:  return 30;  // JS8CallSlow
            case 16: return 4;   // JS8CallFT2 / Subspace — round 3.75 up to 4
            default: return 15;  // Unknown — Normal
        }
    }();
    int const dynamic = 8 * cycleS * 1000;     // ms
    int const floor   = 90000;                  // ms (original constant)
    return dynamic > floor ? dynamic : floor;
}
constexpr int    MIN_NACK_INTERVAL_MS    = 8000;  // per-peer NACK rate limit
constexpr int    SESSION_QUIET_TIMEOUT_MS = 60000; // session-active flag decay
constexpr int    ASSEMBLY_EVICT_TIMEOUT_MS = 300000; // drop incomplete reassemblies
// [ACK-TX-DELAY 2026-06-10 build 237] Brief delay between chunk
// completion and ACK fire. Operator observation: the ACK's leading
// Costas tones appeared abrupt with no ramp-up at the sender's
// receiver, suggesting the audio output device needed more time to
// reach steady state before tone generation began. The Modulator's
// silent_frames pad (100 ms in async mode) alone wasn't enough after
// a long idle gap. 250 ms of additional pre-emit silence gives the
// output device a clean ramp-up window. Applied to ACK only;
// NACK frames are rare and the same logic could be extended there.
constexpr int    ACK_TX_DELAY_MS         = 250;

// --- Wire format helpers ----------------------------------------------------

/**
 * @brief Parsed chunked DATA frame.
 */
struct ParsedChunk {
    QString from;
    QString to;
    QString body;     // marker stripped; ready for CRC validation
    int     msgId;
    int     chunkId;
    int     total;
    QString crcHex;   // 4-char uppercase hex as advertised on wire
};

/**
 * @brief Build the TX text for a single chunked DATA frame.
 *
 * Format: "<myCall>: <peer> <body> #NN.CC/TT.HHHH"
 * The CRC is computed from the uppercased body (since JS8 uppercases
 * everything during Varicode encoding, the receiver sees uppercase).
 */
QString encodeChunkedData(QString const &myCall,
                          QString const &peer,
                          QString const &body,
                          int            msgId,
                          int            chunkId,
                          int            total);

/**
 * @brief Try to parse `text` as a chunked DATA frame. Returns nullopt
 *        when the text doesn't match (caller should treat as vanilla
 *        freetext and route through normal path).
 *
 * Validates field ranges (msg_id in [1, 99], chunk_id in [1, total],
 * total in [1, MAX_CHUNKS_PER_MESSAGE]). Out-of-range fields parse
 * as nullopt rather than the marginal interpretation.
 *
 * The returned `body` has the marker stripped; CRC validation is
 * the caller's responsibility (compare bodyCrcHex(body) to crcHex).
 */
bool parseChunkedData(QString const &text, ParsedChunk &out);

/**
 * @brief Compute 4-char uppercase hex CRC-16-CCITT-FALSE of body.
 *        Uppercases the body first to match JS8's wire form.
 */
QString bodyCrcHex(QString const &body);

/**
 * @brief Display helper for both conversation panel and band activity.
 *
 * If `fullText` matches the chunked-DATA wire form
 *   "<FROM>: <TO> <body> #NN.CC/TT.HHHH [eot]"
 * AND CRC-verifies (bodyCrcHex(body) == HHHH), returns the clean
 * display string "<FROM>: <body> (CC/TT)". Otherwise returns
 * std::nullopt and the caller should render `fullText` as-is.
 *
 * Stateless and side-effect-free — safe to call from any code path.
 * Single source of truth for chunked-aware rewrites; both the
 * conversation window (writeMessageTextToUI/displayTextForFreq) and
 * the band activity panel (displayBandActivity row render) use it.
 */
std::optional<QString> tryFormatChunkedDisplay(QString const &fullText);

/**
 * @brief Collapse all whitespace runs (incl. \\n, \\r, \\t,
 *        multi-space) to a single space, then strip leading/trailing.
 *        Matches what JS8 actually transmits over the wire (no
 *        newlines, single-space inter-word).
 */
QString normalizeBody(QString const &body);

/**
 * @brief Split `body` into chunks of at most `maxChunkBody` chars,
 *        preferring word boundaries. Input is normalized first.
 */
QList<QString> splitIntoChunks(QString const &body,
                               int maxChunkBody = MAX_CHUNK_BODY_CHARS);

// --- State structs ----------------------------------------------------------

/**
 * @brief Outbound chunked-send state per peer.
 *
 * Stop-and-wait at the chunk level: one chunk in flight at a time.
 * Next chunk fires when ACK arrives or the previous chunk fails
 * after MAX_RETRIES.
 */
struct SendState {
    int            msgId{0};
    QList<QString> chunks;           // body chunks (already normalized + split)
    // [TODO #107] Binary (V3) sends: isBinary switches sendNextChunk
    // to the native path — binaryChunks carry the raw bytes, chunks
    // stays EMPTY except as a size mirror (the FSM's nextIdx/size
    // bookkeeping is shared). totalBytes/chunkBytes ride the chunk-1
    // marker.
    bool               isBinary{false};
    QList<QByteArray>  binaryChunks;
    int                binaryTotalBytes{0};
    int                binaryChunkBytesEach{0};
    int            nextIdx{0};       // 0-based index into chunks
    int            retries{0};       // per-chunk retry count (reset on each ACK)
    int            totalRetries{0};  // cumulative across the super-message; never resets
    QTimer        *ackTimer{nullptr}; // ACK-wait timer; nullptr when idle

    // True between emit-wantToTransmit and the poll-detected TX-done.
    // Tells the idle poller "this peer's ackTimer should be armed once
    // JS8Call confirms our TX has fully drained." See
    // _wait_for_tx_done_then_arm_timer() in the Python prototype for
    // the design rationale: arming before TX-end burns ~4-7.5 s of the
    // ACK budget on our own cycle-alignment + frame TX.
    bool           awaitingTxDone{false};
    qint64         awaitingSinceMs{0};

    // [TURNHOLD 2026-07-21] A deferred sendNextChunk singleShot is
    // pending (V3 turnaround hold) — guards against double-scheduling.
    bool           sendHoldPending{false};

    // Set in sendChunked() when the body's leading token looks like a
    // JS8 "MSG" directive (either bare "MSG ..." for inbox or
    // "MSG TO: <addr> ..." for relay). On successful sendComplete the
    // UI hook routes the assembled body to the local inbox via
    // addCommandToMyInbox; on sendFailed or Halt the state is dropped.
    bool           wasMsgCmd{false};
    QString        msgAddressee;   // populated only for "MSG TO: <addr>"
    QString        originalBody;   // pre-chunked body, needed for inbox deposit

    // [BUILD 331-arqTimeoutLock] Submode captured at sendChunked() time
    // — the mode the chunk's audio actually goes out in. Used by
    // armAckTimer to compute the per-chunk ACK timeout (passed to
    // AckTimeoutFn). Locking the mode at TX-commit time eliminates the
    // race where the operator changes mode between the timer-arm read
    // and the actual TX (which would have given the chunk a timer for
    // the WRONG mode — too short on slower→faster switches, etc.).
    int            txSubmode{0};
};

/**
 * @brief Inbound reassembly state per peer.
 *
 * Holds in-flight assembly buffers (one per msg_id), recently-delivered
 * msg_ids for dedup, and the per-peer NACK rate-limit timestamp.
 */
struct RxState {
    QHash<int, QHash<int, QString>> assemblies;     // msg_id -> (chunk_id -> body)
    QHash<int, int>                 totals;          // msg_id -> expected total
    QHash<int, QTimer*>             evictTimers;     // msg_id -> stale-evict timer
    QSet<int>                       deliveredMsgs;   // dedup recently-delivered
    // Per-msg_id MSG-cmd detection. When chunk_id==1 arrives and its
    // body starts with "MSG TO: <addr>" (or bare "MSG ..."), we stash
    // the addressee here. On full-assembly completion the manager
    // emits inboxMessageReceived so the UI hook deposits the reassembled
    // body to the local inbox instead of (or in addition to) the normal
    // conversation-panel display.
    QHash<int, QString>             msgCmdAddressee; // msg_id -> addressee (empty for bare MSG)
    QHash<int, bool>                msgCmdDetected;  // msg_id -> first-chunk had a MSG prefix
    // [RELAY-VIA-ARQ 2026-06-10 build 243] Parallel to msgCmdDetected.
    // When chunk 1's body starts with "<callsign>>", flag so the
    // assembly-complete branch emits relayMessageReceived instead of
    // letting the body display as plain freetext.
    QHash<int, bool>                relayCmdDetected;
    // [FILE-XFER 2026-06-16 build 276] Parallel to msgCmdDetected /
    // relayCmdDetected. Chunk 1 body starts with "F/V1 " → flag so
    // the assembly-complete branch emits fileMessageReceived (instead
    // of letting the body display as plain freetext or land in the
    // inbox). The header is unused at assembly-complete time (the
    // chunkedArqHooks slot re-parses from the assembled body), so
    // only a bool flag is needed here.
    QHash<int, bool>                fileXferDetected;
    qint64                          lastNackMonoMs{0};
    QTimer                         *quietTimer{nullptr};
    bool                            sessionActive{false};

    // [TODO #107] Native-binary (V3) receive state. ONE collect window
    // per peer (stop-and-wait ⇒ at most one chunk in flight). The
    // window AUTO-ADVANCES on each completed chunk — markers after
    // chunk 1 are advisory (callsign ID + PCRC refresh); their loss
    // costs nothing because the sender cannot pass chunk CC without
    // our ACK.
    struct NativeWindow {
        bool active{false};
        int  msgId{0};
        int  chunkId{0};   // expected CC
        int  total{0};     // TT (from the marker tail)
        NativeBinary::ChunkCollector collector;
        QTimer *collectTimer{nullptr};  // parented to the Manager
        int  noProgressNacks{0};  // consecutive fruitless collect expiries
    };
    NativeWindow nativeWin;
    // Per-msgId: TOTAL envelope bytes + chunk size (both from the
    // chunk-1 marker, together authoritative for every chunk's byte
    // count) and collected chunk bytes.
    QHash<int, int>                    binaryTotalBytes;
    QHash<int, int>                    binaryChunkBytes;
    QHash<int, QHash<int, QByteArray>> binaryAssemblies;
    // (Orphan frames — binary frames that beat their marker — are
    //  MANAGER-GLOBAL, not per-peer: frames are anonymous until a
    //  window binds them. See Manager::m_nativeOrphans.)

    // NOTE: the Python prototype carried a `next_expected_chunk` field
    // here to feed two extra NACK paths (unparseable freetext from an
    // active peer, and ellipsis "…" markers from JS8Call's typeahead).
    // We intentionally did NOT port either NACK path. Reasons recorded
    // 2026-06-05:
    //   (a) Mid-TX NACK is mostly wasted — the sender's still TXing
    //       when we'd send it; they wouldn't hear us. We'd cost the
    //       channel a NACK frame for nothing.
    //   (b) When ARQ mode is on, an unparseable freetext isn't
    //       necessarily from OUR partner — could be unrelated traffic
    //       to us. No reason to NACK them.
    // The slower recovery (waiting for the sender's ACK-timer to fire)
    // is the explicit accepted trade-off.
};

/**
 * @brief Result of a sendChunked() call.
 *
 * Synchronous diagnostic for TCP API callers — tells them whether the
 * send was accepted and what msg_id / chunk count was assigned, without
 * waiting for the async sendComplete / sendFailed signal.
 */
struct SendResult {
    bool    ok{false};
    QString error;        // populated when ok == false
    int     msgId{0};     // wire msg_id, [MSG_ID_MIN..MSG_ID_MAX]
    int     totalChunks{0};
};

// --- Manager ----------------------------------------------------------------

/**
 * @brief Per-process owner of all chunked-ARQ state.
 *
 * Lives on the main thread (same as UI_Constructor). Interacts with
 * the rest of JS8Call via Qt signals/slots only.
 */
class Manager : public QObject {
    Q_OBJECT

  public:
    explicit Manager(QObject *parent = nullptr);
    ~Manager() override;

    void setMyCall(QString const &myCall) { m_myCall = myCall; }
    QString myCall() const { return m_myCall; }

    /**
     * @brief Predicate returning true when JS8Call's TX path is fully
     *        idle (no in-flight transmission, queue drained, edit
     *        buffer empty). The manager polls this between
     *        wantToTransmit and ACK-timer-arm to mirror the Python
     *        prototype's _wait_for_tx_done_then_arm_timer() — without
     *        it, the ACK budget burns down during our own
     *        cycle-alignment + frame TX (~4–7.5 s on Subspace, more on
     *        slower modes).
     *
     * Pass a lambda that closes over UI_Constructor state, e.g.:
     *     mgr->setTxIdleCheck([this]{
     *         return !m_transmitting && m_txMessageQueue.isEmpty()
     *             && ui->extFreeTextMsgEdit->toPlainText()
     *                .trimmed().isEmpty();
     *     });
     */
    using IdleCheckFn = std::function<bool()>;
    void setTxIdleCheck(IdleCheckFn fn) { m_txIdleCheck = std::move(fn); }

    /**
     * @brief Callback returning the ACK-wait timeout in milliseconds
     *        FOR THE GIVEN SUBMODE. The Manager passes the submode that
     *        was captured at chunk-TX-commit time (state.txSubmode),
     *        guaranteeing the timer matches the mode the chunk actually
     *        went out in — not the mode the operator might have just
     *        switched to. Defaults to DEFAULT_ACK_TIMEOUT_MS if unset.
     *
     * Typical UI wiring:
     *     mgr->setAckTimeoutFn([](int submode){
     *         return ChunkedArq::ackTimeoutMsForSubmode(submode);
     *     });
     */
    using AckTimeoutFn = std::function<int(int submode)>;
    void setAckTimeoutFn(AckTimeoutFn fn) { m_ackTimeoutFn = std::move(fn); }

    /**
     * @brief Callback returning the TX-idle safety-cap timeout in ms,
     *        evaluated each poll tick (every 1 s). See
     *        `txIdleMaxWaitMsForSubmode()` in this header for the per-
     *        submode rationale (legacy Normal/Slow chunks take longer
     *        than 90 s to drain so the fixed cap fired prematurely and
     *        ate into the ACK budget). Defaults to 90 000 ms if unset.
     *
     * Typical UI wiring (mirror of setAckTimeoutFn):
     *     mgr->setTxIdleCapFn([this]{
     *         return ChunkedArq::txIdleMaxWaitMsForSubmode(m_nSubMode);
     *     });
     */
    using TxIdleCapFn = std::function<int()>;
    void setTxIdleCapFn(TxIdleCapFn fn) { m_txIdleCapFn = std::move(fn); }

    /**
     * @brief True if we've recently exchanged ARQ traffic with this
     *        peer (gates blind-NACK paths so unrelated freetext from
     *        random stations doesn't provoke NACKs to them).
     */
    bool isActiveSession(QString const &peer) const;

    /**
     * @brief True when the operator has enabled the ARQ menu item
     *        AND we're in a context where the relaxed TX gate should
     *        apply. Currently driven entirely from the menu checkbox
     *        via setArqEnabled(); future work may also gate on actual
     *        in-flight send/recv state.
     *
     * Used by:
     *   - prepareSending (mainwindow.cpp): when (FT2 mode AND
     *     arqInProgress()) the "arqFullRelax" OR-clause enables
     *     "TX anytime" — PTT fires whenever a message is ready,
     *     bypassing m_timeToSend / fraction_of_tx_slot / lateThreshold
     *     entirely. Spacing is still floor-clamped by the separate
     *     arqIntervalOK gate (MIN_ARQ_PTT_INTERVAL_MS = 3750ms PTT-
     *     to-PTT). (Stale comment fixed 2026-06-08: earlier docstring
     *     said "raises lateThreshold to 0.1" — that was the pre-
     *     Build-88 design; lateThreshold is now unconditionally 0.0
     *     and the relax is via a dedicated OR-clause instead.)
     *   - UI_Constructor: pushes the same flag to Modulator via
     *     setArqRelax() on every toggle.
     */
    bool arqInProgress() const { return m_arqEnabled; }

    /**
     * @brief True when at least one chunked-ARQ super-message is
     *        actively being sent OR received. UI uses this to disable
     *        the ARQ enable/disable controls (button + menu action)
     *        for the duration of an in-flight session so the operator
     *        can't yank the protocol out from under itself mid-
     *        transfer. Cleared by sendComplete / sendFailed / final
     *        chunk on RX / haltAll / assembly-evict timer.
     *
     * "Active" = any entry in m_sends (any TX in flight to any peer)
     * OR any non-empty per-peer reassembly map (m_recv[p].assemblies).
     * An m_recv entry with sessionActive=true but no live assemblies
     * (i.e. trailing quiet-timer window after the last delivery) does
     * NOT count as active — the operator may legitimately want to
     * disable ARQ once the data has been handed off.
     */
    bool hasActiveSession() const;

    /**
     * @brief True if WE are actively transmitting a chunked super-msg
     *        (any entry in m_sends). Distinct from hasActiveSession():
     *        does NOT return true when we are only RECEIVING chunks
     *        from someone else. Used by the UI-lock paths so the
     *        operator's buttons / menus stay USABLE when we are the
     *        receiver, even mid-session. UI lock fires only when WE
     *        are the originator of an in-flight super-msg.
     *
     *        (TX-side timing paths — prepareSending's arqFullRelax,
     *        the stopTx async-finish bypass — still use
     *        hasActiveSession(), so the ACKs we send during RX still
     *        get the async-timing treatment.)
     */
    bool hasActiveTxSession() const {
        return !m_sends.isEmpty() || m_negotiating;
    }

    /**
     * @brief [2026-07-23 negophase] Capability negotiation is the
     *        OPENING PHASE OF A TX SESSION, not something that happens
     *        before one.
     *
     * From the operator's point of view the session begins at the Send
     * click: we are transmitting on their behalf (QUERY ARQ? goes out
     * over the air) from that moment until the transfer completes or
     * aborts. m_sends only fills once the format is known and the first
     * chunk exists, so for the 20-130 s negotiation window the Manager
     * used to report "no TX session" — and EVERY UI lock, banner and
     * busy-API flag asks the Manager. Result: the whole control surface
     * stayed live during negotiation, and an operator who pressed a
     * macro button keyed over the QUERY ARQ? and killed the transfer
     * (field report 2026-07-23).
     *
     * Two sites (guiUpdate's arqBusy, the compose-box lock) had the
     * pending-transfer condition HAND-COPIED in from the UI layer to
     * paper over this; those copies are deleted now that the predicate
     * itself is right. Everything else — Speed/Mode lock, the CQ..Saved
     * macro row, Send-file, AV HAIL, TX_QUEUE_DEPTH / BUSY /
     * ARQ_TX_ACTIVE — inherits the lock with no call-site change.
     *
     * COVERS: every consumer of hasActiveTxSession(), plus AV HAIL
     *   (which reads hasActiveSession() and is OR'd explicitly).
     * DOES NOT COVER (deliberate): hasActiveSession(). Two of its
     *   consumers are TX-TIMING paths — arqFullRelax in guiUpdate and
     *   arqFullRelaxStopTx in stopTx — which switch PTT between
     *   period-aligned and async. QUERY ARQ? is an ordinary directed
     *   message on the normal enqueue path and must keep the ordinary
     *   period-aligned timing; folding negotiation in there would
     *   silently change TX timing, which is not what this fixes. Its
     *   other consumers (inbound chime + BELL suppression) key on
     *   inbound chunk traffic, which negotiation has none of.
     *
     * INVARIANT: outside the negotiation window m_negotiating is false,
     * so every predicate above returns exactly what it returned before
     * this change. The behavioural delta is confined to the window.
     *
     * Terminals mirror a session's exactly: reply→resume (sendComplete),
     * timeout→V1 fallback (sendFailed), Halt (haltAll), callsign change
     * (abort). endNegotiation() is idempotent.
     */
    void beginNegotiation(QString const &peer) {
        m_negotiating = true;
        m_negotiatingPeer = peer.toUpper();
    }
    void endNegotiation() {
        m_negotiating = false;
        m_negotiatingPeer.clear();
    }
    bool isNegotiating() const { return m_negotiating; }
    QString negotiatingPeer() const { return m_negotiatingPeer; }

    /**
     * @brief [BUILD 343.3 rxLock] True while a V3 native RECEIVE is
     *        actively in progress: a collect window is open AND its
     *        watchdog is still running (hunting frames or between
     *        chunks). Goes false at delivery, at watchdog give-up
     *        (window turns passive), and after evict — so UI locks
     *        keyed on this release promptly when a sender vanishes,
     *        instead of waiting out the 5-minute assembly evict the
     *        way binaryAssemblies-based checks would.
     */
    /**
     * @brief [TODO #112 2026-07-23] True while ANY chunked ARQ transfer
     *        is inbound to us — V1/V2 text assemblies (file transfers,
     *        web links AND plain ARQ super-messages) or a V3 native
     *        collect window / partial binary assembly.
     *
     *        Deliberately broader than hasActiveRxWindow() (V3 only):
     *        the collision physics are identical whatever the payload —
     *        stop-and-wait, half-duplex, we go deaf while transmitting,
     *        and any auto-reply we key mid-transfer stomps our own ACK
     *        and costs a full retry cycle. The reply dispatch already
     *        suppressed auto-replies while a single multi-frame message
     *        was assembling to us (hasExistingMessageBufferToMe); this
     *        is that same rule at transfer scale, which is the more
     *        justified case, not the less.
     *
     *        Release: text assemblies clear on completion or the 5 min
     *        assembly evict; the native window clears at delivery or
     *        watchdog give-up. So a vanished sender releases the
     *        suppression at evict rather than instantly — bounded, and
     *        the same bound the assemblies themselves live under.
     */
    bool hasActiveRxTransfer() const {
        for (auto it = m_recv.constBegin(); it != m_recv.constEnd();
             ++it) {
            if (!it.value().assemblies.isEmpty() ||
                it.value().nativeWin.active ||
                !it.value().binaryAssemblies.isEmpty()) {
                return true;
            }
        }
        return false;
    }

    bool hasActiveRxWindow() const {
        for (auto it = m_recv.constBegin(); it != m_recv.constEnd();
             ++it) {
            if (it.value().nativeWin.active &&
                it.value().nativeWin.collectTimer &&
                it.value().nativeWin.collectTimer->isActive()) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Set by UI_Constructor in response to the operator
     *        toggling the ARQ menu action. The actual gate (mode +
     *        this flag) lives in prepareSending and Modulator; this
     *        is the single source of truth.
     */
    void setArqEnabled(bool enabled) { m_arqEnabled = enabled; }

    /**
     * @brief Abort all in-flight chunked-ARQ activity. Called from
     *        UI_Constructor::on_stopTxButton_clicked() ("Halt") so the
     *        operator's stop-TX request also tears down any in-flight
     *        ARQ session — pending sends emit sendFailed("halted"),
     *        timers stop, reassembly buffers drop. MSG-cmd state on
     *        each SendState goes away with the SendState itself.
     */
    void haltAll();

    /**
     * @brief One collect-watchdog expiry for @a peer's native window
     *        — the core of the onNativeCollectTimerExpired slot,
     *        public so the offline FSM harness can drive expiries
     *        without wire-realistic waits.
     * @return true if the watchdog NACKed and re-armed (still
     *         hunting); false if there was nothing to watch or it
     *         gave up after NATIVE_NACK_GIVEUP fruitless expiries
     *         (window stays passively open for late frames).
     */
    bool nativeCollectTimeout(QString const &peer);

    /**
     * @brief [BUILD 344 binMarker] A decoded BINARY marker frame.
     *        Resolves the sender by peerHash16 against known-peer
     *        candidates (any m_recv entry — active windows, past
     *        deliveries, registered negotiation askers); no match =
     *        silent drop (the text marker is the only fresh-contact
     *        open). On match, routes through handleNativeMarker
     *        exactly like a first-chunk-form text marker (the binary
     *        form always carries TOTAL+KB, so mid-join works too).
     * @return true iff resolved + handled (caller re-latches submode).
     */
    // holdMs: keyup delay for any ACK this marker triggers (it can
    // complete a chunk by draining orphans) — see
    // onNativeFrameReceived's frameHoldMs. [TODO #119]
    bool onNativeMarkerFrameReceived(NativeBinary::MarkerFrame const &m,
                                     int freq,
                                     int holdMs = ACK_TX_DELAY_MS);

    /**
     * @brief [BUILD 344 binMarker] Make `peer` resolvable by the
     *        binary marker's hash before any transfer state exists —
     *        the UI calls this when answering QUERY ARQ?, so a fresh
     *        transfer right after negotiation can bind even if the
     *        chunk-1 TEXT marker is lost (on-air msg-33 failure).
     */
    void registerPeerCandidate(QString const &peer);

    /**
     * @brief Test seam: shrink the V3 frame-slot duration (default
     *        3750 ms, one Subspace T/R period) so the offline FSM
     *        harness can exercise the delayed re-ACK and watchdog
     *        arithmetic without wire-realistic waits. Production code
     *        never calls this.
     */
    void setNativeFrameMs(int ms) { m_nativeFrameMs = ms; }

  public slots:
    /**
     * @brief Start a chunked send of `body` to `peer`. Splits into
     *        chunks, fires the first one via `wantToTransmit` signal,
     *        and drives the rest from incoming ACKs.
     *
     * Returns a SendResult with ok=false (and emits `sendFailed`
     * synchronously) if the body would exceed MAX_CHUNKS_PER_MESSAGE
     * chunks, or if a chunked send to this peer is already in flight
     * (one-at-a-time per peer). On success, populates msgId and
     * totalChunks for the caller's tracking.
     */
    SendResult sendChunked(QString const &peer, QString const &body,
                           int submode,
                           int maxChunks = MAX_CHUNKS_PER_MESSAGE);

    /**
     * @brief [TODO #107] V3 native-binary send: the raw envelope
     *        rides Subspace frames directly (no varicode). Markers on
     *        chunk 1 + every MARKER_INTERVALth chunk; the whole
     *        stop-and-wait FSM (ACK/NACK/timeout/retransmit) is the
     *        text path's, unchanged. Level-3 + Subspace-mode peers
     *        only (caller gates).
     */
    SendResult sendChunkedBinary(QString const &peer,
                                 QByteArray const &envelope, int submode,
                                 int chunkBytes =
                                     NativeBinary::DEFAULT_CHUNK_BYTES);

    /**
     * @brief Notify the manager that we received a chunked DATA frame.
     *        Called from processCommandActivity's RX hook.
     */
    /**
     * [TURNHOLD-ACK 2026-07-23] ackHoldMs delays the ACK/NACK KEYUP (V1/V2
     * text path) so it lands after the SENDER has finished its chunk and
     * switched TX->RX. Without it the receiver keys the ACK ~250 ms after
     * decoding the sender's last frame — which is the same instant the
     * sender is turning around — so the ACK's leading Costas airs in the
     * sender's AGC/relay dead zone and is missed ~half the time, forcing a
     * full retransmit cycle (both logs, 2026-07-23: 4 retries on a 7-chunk
     * transfer at -2 dB, every retry an ACK TIMEOUT on a chunk the receiver
     * HAD ACKed). This is the receiver-side counterpart of the Build 346
     * sender turnhold. Default = ACK_TX_DELAY_MS keeps the old behaviour
     * for callers that do not compute a hold (harness, V3 re-ACKs).
     */
    void onChunkReceived(QString const &fromCall, ParsedChunk const &chunk,
                         int ackHoldMs = ACK_TX_DELAY_MS);

    /**
     * @brief [TODO #107] A native-binary (V3) frame decoded — bit75
     *        discriminated, already deduped by both RX layers. Binds
     *        to the peer whose collect window is active (CHK4 sanity
     *        tag); windowless frames go to the orphan store.
     * @return true iff the frame was ACCEPTED into OUR active collect
     *         window — the caller uses this to re-latch the auto-mode
     *         submode switch mid-transfer (V3's counterpart of the
     *         per-chunk re-latch V2 gets from every inbound chunked-
     *         DATA text). Orphaned / rejected frames return false so
     *         third-party listeners hearing someone else's transfer
     *         never mode-switch (native frames carry no addressing).
     */
    /**
     * @param frameHoldMs [TODO #119 2026-07-24 v3ackhold] Keyup delay
     *        for any ACK/NACK this frame triggers, computed by the
     *        caller from the frame's absPos exactly as the V1/V2 text
     *        path does (see processCommandActivity's ackHoldMs). The
     *        three ACK/NACK sites reachable from here fire the instant
     *        the sender's LAST burst frame decodes, i.e. inside its
     *        TX->RX turnaround; at the old flat 250 ms the reply's
     *        leading Costas aired in the sender's dead zone and was
     *        missed (operator 2026-07-24: "partial ACK or NACK" seen
     *        at the sender). Default preserves the pre-fix timing for
     *        the FSM harness.
     */
    bool onNativeFrameReceived(int seq, int chk4,
                               QByteArray const &payload8,
                               int freq, qint64 absPos,
                               int frameHoldMs = ACK_TX_DELAY_MS);

    /**
     * @brief Notify the manager that we received an ACK for chunk `seq`
     *        from `fromCall`. Called from processCommandActivity's ACK
     *        handler.
     */
    /**
     * [TURNHOLD 2026-07-21] holdMs > 0 defers the NEXT chunk's send
     * (V3 only) so our keyup lands after the peer's radio is back in
     * receive. Bench 2026-07-21 (logs 210707Z/211222Z): the async
     * decoder reads the peer's ACK from the ring BEFORE the ACK's own
     * post-roll finishes airing, and we keyed the next chunk 14 ms
     * later — while the peer was still keyed / in TX→RX turnaround.
     * Result: seq-0 lost on ~40% of unmarked chunks, ~40-50 s retry
     * each. The caller computes holdMs from the ACK's end-of-air
     * (absPos) + TURNAROUND_TAIL_MS. State advances immediately;
     * only the send is deferred.
     */
    void onAckReceived(QString const &fromCall, int seq, int holdMs = 0);

    /**
     * @brief Notify the manager that we received a NACK for chunk `seq`
     *        from `fromCall`. Triggers immediate retransmission of the
     *        in-flight chunk if it matches.
     */
    void onNackReceived(QString const &fromCall, int seq, int holdMs = 0);

  signals:
    /**
     * @brief Manager wants this text TX'd. UI_Constructor wires this
     *        to enqueueMessage(PriorityHigh, text, -1, nullptr).
     */
    void wantToTransmit(QString const &text);

    /**
     * @brief [TODO #107] One V3 chunk is ready: TX the marker text
     *        (EMPTY on markerless chunks) then inject the chunk's raw
     *        binary frames. UI hook: onNativeChunkWantToTransmit.
     *        peer + totalChunks ride along for the per-burst
     *        "[Submsg N of M]" conversation-window feedback line
     *        (rendered with the standard from/to/freq/mode header).
     *        [BUILD 344 binMarker] markerFrame9: the BINARY marker
     *        frame's 9 wire bytes (NativeBinary::frameToBytes),
     *        EMPTY when this burst carries none — inject it AHEAD of
     *        the payload frames. markerText may now be empty on
     *        bursts that still carry a binary marker (cadence and
     *        retries went native; text remains on chunk 1 + every
     *        TEXT_ID_INTERVALth for station ID).
     */
    void wantToTransmitNativeChunk(QString const &peer,
                                   QString const &markerText,
                                   int chunkId, int totalChunks,
                                   QByteArray const &markerFrame9,
                                   QByteArray const &chunkBytes);

    /**
     * @brief [TODO #107] RX side collected one V3 chunk (all frames,
     *        PCRC OK — the ACK for it is staged). UI writes the
     *        per-burst "[Submsg N of M]" conversation-window line.
     */
    void nativeChunkCollected(QString const &peer, int chunkId,
                              int totalChunks);

    /**
     * @brief [TODO #107] A V3 marker for a LIVE (not yet delivered)
     *        transfer decoded — cadence, retry, or mid-join. UI uses
     *        it for receive-side feedback (outgoing-box placeholder
     *        "MULTI-PART MSG IN PROGRESS...").
     */
    void nativeMarkerSeen(QString const &peer, int chunkId,
                          int totalChunks);

    /**
     * @brief Manager wants this RX-side response (ACK / NACK) TX'd.
     *        Same payload semantics as wantToTransmit but the host can
     *        wrap save/restore of the outgoing-text widget around the
     *        TX so a draft the user is typing during reception isn't
     *        clobbered. See TODO.md #57 (per-response outgoing-text
     *        preservation) for design rationale.
     */
    void wantsResponseTx(QString const &text);

    /**
     * @brief A clean chunk arrived from `fromCall`. UI should display
     *        it in the conversation panel as "<body> (CC/TT)" with NO
     *        end-of-message diamond — diamond is reserved for the
     *        final summary on completion.
     */
    void chunkAdded(QString const &fromCall,
                    QString const &chunkBody,
                    int            chunkId,
                    int            total);

    /**
     * @brief A complete message finished assembling from `fromCall`.
     *        UI should write the full `assembledBody` to the
     *        conversation panel followed by the diamond marker as the
     *        "delivered" signal.
     */
    void messageDelivered(QString const &fromCall,
                          QString const &toCall,
                          QString const &assembledBody,
                          int            msgId);

    /**
     * @brief Outbound send progress (TCP API push payload).
     */
    void sendProgress(QString const &peer, int msgId, int delivered, int total);

    /**
     * @brief Outbound send completed successfully (TCP API push payload).
     *
     * Carries enough stats to render an end-of-super-message summary
     * dialog: msgId identifies the super-message, total is the sub-
     * message count, totalRetries is the cumulative retry count over
     * the whole super-message (NACKs + ACK-timer expiries).
     */
    void sendComplete(QString const &peer,
                      int            msgId,
                      int            total,
                      int            totalRetries);

    /**
     * @brief Emitted after sendComplete when the sent body was
     *        detected as a MSG command (bare "MSG ..." or
     *        "MSG TO: <addr> ..."). UI_Constructor's hook turns this
     *        into addCommandToMyInbox so the operator's local inbox
     *        records the successful relay.
     *
     * @param peer       Destination of the chunked send.
     * @param addressee  For "MSG TO: <addr>": the <addr> token.
     *                   For bare "MSG ...":   empty string.
     * @param body       The original (pre-chunk) body, including the
     *                   "MSG" prefix — caller can strip if needed.
     * @param msgId      Wire msg_id of the completed send.
     */
    void msgDelivered(QString const &peer,
                      QString const &addressee,
                      QString const &body,
                      int            msgId);

    /**
     * @brief Emitted when we *received* a fully-assembled super-message
     *        whose first chunk started with a JS8 "MSG" directive
     *        ("MSG TO: <addr> ..." or bare "MSG ..."). The UI hook
     *        deposits the reassembled body to the local inbox via
     *        addCommandToMyInbox and shows a modeless "saved to inbox"
     *        dialog. Distinct from msgDelivered (which is the TX-side
     *        mirror — our own sent MSG being logged locally).
     *
     * @param fromCall  Peer that sent us the chunked super-message.
     * @param addressee For "MSG TO: <addr>": the <addr> token (often
     *                  m_myCall when the message is for us). For bare
     *                  "MSG ...": empty.
     * @param body      Full assembled super-message body, including
     *                  the leading "MSG TO: <addr>" prefix if present
     *                  (mirrors the existing single-frame MSG handler).
     * @param msgId     Wire msg_id of the completed super-message.
     */
    void inboxMessageReceived(QString const &fromCall,
                              QString const &addressee,
                              QString const &body,
                              int            msgId);

    /**
     * @brief Assembled super-message is an ARQ-wrapped relay request.
     *
     * Body shape: "<targetCall>> <inner text> <CRC>" — the sender
     * computed the 16-bit checksum over <inner text> and appended it
     * (same wire shape an on-air ">" cmd would produce). The hook
     * (chunkedArqHooks::onChunkedRelayMessageReceived) populates
     * m_messageBuffer with this so processBufferedActivity validates
     * the checksum, then the existing ">" handler at
     * processCommandActivity.cpp:561 fires and TXs the forward.
     */
    void relayMessageReceived(QString const &fromCall,
                              QString const &body,
                              int            msgId);

    /**
     * @brief Inbound super-message that started with the "F/V1 "
     *        file-transfer magic prefix has finished assembling.
     *        chunkedArqHooks splits the body via
     *        FileTransfer::splitWireBody to recover the header +
     *        base32 payload, then pops the accept dialog and writes
     *        the file via FileTransfer::assembleReceivedFile.
     *        See TODO.md ARQ-file-transfer Phase 1 plan (2026-06-16).
     */
    /**
     * @brief [TODO #107] A V3 native-binary transfer fully collected:
     *        all chunks complete, per-chunk integrity passed. envelope
     *        = the raw compressed envelope for splitWireBodyV3.
     */
    void binaryMessageReceived(QString const &fromCall,
                               QByteArray const &envelope, int msgId);

    void fileMessageReceived(QString const &fromCall,
                             QString const &body,
                             int            msgId);

    /**
     * @brief Outbound TX progress update for status-bar display.
     *
     * Emitted on each sendNextChunk (new chunk OR retransmit). The UI
     * status-bar hook overrides the standard "Tx: <text>" label with
     * "ARQ: #x/y (z repeats)" until progressEnd fires.
     */
    void progressUpdate(int chunkId, int total, int retries);

    /**
     * @brief Outbound ARQ session ended (success or failure).
     *
     * Emitted from sendComplete / sendFailed / haltAll paths. The UI
     * hook clears its progress-override and lets the normal status-bar
     * update logic resume.
     */
    void progressEnd();

    /**
     * @brief Outbound send failed after exhausting retries (TCP API push payload).
     *
     * totalRetries is the cumulative retry count across the whole
     * super-message at the moment of failure (matches sendComplete
     * semantics so the UI summary dialog can render uniformly).
     */
    void sendFailed(QString const &peer,
                    int            msgId,
                    int            chunksDelivered,
                    int            total,
                    int            totalRetries,
                    QString const &reason);

    /**
     * @brief Emitted alongside sendFailed when the original outbound
     *        super-message text should be RESTORED to the operator's
     *        outgoing-text widget so they can retry without re-typing.
     *
     *        Fires on non-success terminal paths:
     *          - reason="halted"            (operator pressed Halt)
     *          - reason="nack_exhausted"    (NACKs exceeded max retries)
     *          - reason="timeout_exhausted" (ACK timer expired N times)
     *          - reason="too_long"          (message would exceed chunk cap)
     *          - reason="busy"              (a send was already in flight)
     *
     *        Does NOT fire on sendComplete — the message was delivered,
     *        there's nothing to restore.
     *
     *        UI hook receives the body and writes it back to
     *        ui->extFreeTextMsgEdit so the operator sees their original
     *        text in the outgoing box and can edit / resend.
     *
     *        TODO #51 (2026-06-10 build 235).
     */
    void sendRestoreRequested(QString const &body,
                              QString const &reason);

  private slots:
    void onAckTimerExpired();
    void onQuietTimerExpired();
    void onAssemblyEvictTimerExpired();
    // [TODO #107] Native collect window watchdog — gaps after the
    // expected frame count's airtime → rate-limited NACK; window
    // stays open (the sender's ACK timeout drives the retry loop).
    void onNativeCollectTimerExpired();
    void onTxIdlePollTick();

  private:
    // Fire the next pending chunk for `peer`, or finish the send if
    // all chunks have been delivered.
    void sendNextChunk(QString const &peer);
    // [TURNHOLD 2026-07-21] sendNextChunk now, or after holdMs (V3
    // sends only — V2's box/typeahead path is naturally slow enough).
    void scheduleSendNextChunk(QString const &peer, int holdMs);
    // Send a NACK to `peer` for chunk `seq`, rate-limited per peer.
    // holdMs: keyup delay (see onChunkReceived's ackHoldMs).
    void tryNack(QString const &peer, int seq,
                 int holdMs = ACK_TX_DELAY_MS);
    // Send an ACK to `peer` for chunk `seq` (always, unrate-limited).
    // holdMs: keyup delay (see onChunkReceived's ackHoldMs).
    void sendAck(QString const &peer, int seq,
                 int holdMs = ACK_TX_DELAY_MS);
    // Touch the session-active flag and (re-)arm the quiet timer.
    void markSessionActive(QString const &peer);
    // Ensure the TX-idle poll timer is running (lazy-init + start).
    void ensureTxIdlePolling();
    // Arm `state.ackTimer` using whatever timeout the AckTimeoutFn
    // currently reports (falls back to DEFAULT_ACK_TIMEOUT_MS).
    void armAckTimer(QString const &peer, SendState &state);
    // Helpers for the per-peer maps.
    SendState &getOrCreateSend(QString const &peer);
    RxState &getOrCreateRx(QString const &peer);

    // [TODO #107] Native-binary (V3) receive helpers.
    // Marker chunk arrived (already text-CRC-verified): open/refresh
    // the collect window, or re-ACK an already-collected chunk.
    // holdMs: [TODO #119] keyup delay for an ACK this marker path may
    // send by completing a chunk from drained orphans.
    void handleNativeMarker(QString const &peer, RxState &rx,
                            ParsedChunk const &chunk,
                            NativeBinary::MarkerInfo const &mi,
                            int holdMs = ACK_TX_DELAY_MS);
    // Open the window for chunk `chunkId` (marker- or auto-advance-
    // driven), drain matching orphans, arm the collect timer. May
    // complete immediately if orphans already fill the chunk.
    void openNativeChunkWindow(QString const &peer, RxState &rx,
                               int msgId, int chunkId, int total,
                               quint16 pcrc, bool pcrcValid,
                               int holdMs = ACK_TX_DELAY_MS);
    // Collector complete: PCRC verdict → store+ACK+advance/deliver,
    // or NACK+reset.
    // holdMs: keyup delay for the ACK (crc OK) or NACK (crc bad) this
    // completion sends — see onNativeFrameReceived's frameHoldMs.
    void finishNativeChunk(QString const &peer, RxState &rx,
                           int holdMs);
    // Drop all V3 state for one peer (halt/evict/deliver cleanup).
    void clearNativeState(RxState &rx);

    QString                     m_myCall;
    int                         m_nextMsgId{MSG_ID_MIN};
    QHash<QString, SendState>   m_sends;
    // [2026-07-23 negophase] Opening phase of a TX session — see
    // beginNegotiation(). Folded into hasActiveTxSession().
    bool                        m_negotiating{false};
    QString                     m_negotiatingPeer;
    QHash<QString, RxState>     m_recv;

    // [TODO #107] Binary frames that arrived with no open collect
    // window (they beat their marker through the buffered-text
    // pipeline, or are stale RF). Manager-global: frames are
    // anonymous until a window's CHK4 binds them. Cap 16, TTL 30 s.
    struct NativeOrphan {
        int        seq{0};
        int        chk4{0};
        QByteArray p8;
        qint64     monoMs{0};
    };
    QList<NativeOrphan>         m_nativeOrphans;

    IdleCheckFn                 m_txIdleCheck;
    AckTimeoutFn                m_ackTimeoutFn;
    // V3 frame-slot duration (ms). One Subspace T/R period; the
    // watchdog budgets and post-burst re-ACK delay derive from it.
    // Overridable via setNativeFrameMs() for the offline harness.
    int                         m_nativeFrameMs{3750};
    TxIdleCapFn                 m_txIdleCapFn;
    QTimer                     *m_txIdlePollTimer{nullptr};
    bool                        m_arqEnabled{false};
};

}  // namespace ChunkedArq

#endif  // CHUNKEDARQ_H
