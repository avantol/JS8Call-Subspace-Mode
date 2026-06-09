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
constexpr int    ARQ_PROTOCOL_LEVEL      = 1;

constexpr int    MSG_ID_MIN              = 1;
constexpr int    MSG_ID_MAX              = 99;
constexpr int    MAX_CHUNKS_PER_MESSAGE  = 31;    // bounded by wire ACK extra width (1..31)
constexpr int    MAX_CHUNK_BODY_CHARS    = 60;    // body per chunk; tune for failure rate
constexpr int    CHUNKED_MARKER_LEN      = 14;    // len("#NN.CC/TT.HHHH")

constexpr int    DEFAULT_MAX_RETRIES     = 3;
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
        case 16: return 12000; // JS8CallFT2 / Subspace — 3.75 s cycle
        default: return 25000; // Unknown — pick a middling value
    }
}

// TX-idle poll: how often the manager re-checks "has JS8Call finished
// my TX yet?" while a chunk is in flight. 1 s is fine — finer than
// Python's 2 s, since we're already in-process and the cost is just a
// pair of bool reads.
constexpr int    TX_IDLE_POLL_INTERVAL_MS = 1000;

// Safety cap: if JS8Call never reports idle (e.g. user pressed Stop,
// stuck PTT, etc.) arm the ACK timer anyway after this long so the
// chunked-send state machine doesn't wedge. 90 s comfortably covers
// even a Slow-mode cycle.
constexpr int    TX_IDLE_MAX_WAIT_MS      = 90000;
constexpr int    MIN_NACK_INTERVAL_MS    = 8000;  // per-peer NACK rate limit
constexpr int    SESSION_QUIET_TIMEOUT_MS = 60000; // session-active flag decay
constexpr int    ASSEMBLY_EVICT_TIMEOUT_MS = 300000; // drop incomplete reassemblies

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

    // Set in sendChunked() when the body's leading token looks like a
    // JS8 "MSG" directive (either bare "MSG ..." for inbox or
    // "MSG TO: <addr> ..." for relay). On successful sendComplete the
    // UI hook routes the assembled body to the local inbox via
    // addCommandToMyInbox; on sendFailed or Halt the state is dropped.
    bool           wasMsgCmd{false};
    QString        msgAddressee;   // populated only for "MSG TO: <addr>"
    QString        originalBody;   // pre-chunked body, needed for inbox deposit
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
    qint64                          lastNackMonoMs{0};
    QTimer                         *quietTimer{nullptr};
    bool                            sessionActive{false};

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
     * @brief Callback returning the ACK-wait timeout in milliseconds,
     *        evaluated at arm time so a mid-QSO mode switch (e.g.
     *        Subspace → Turbo) takes effect immediately. Defaults to
     *        DEFAULT_ACK_TIMEOUT_MS if unset.
     *
     * Typical UI wiring:
     *     mgr->setAckTimeoutFn([this]{
     *         return ChunkedArq::ackTimeoutMsForSubmode(m_nSubMode);
     *     });
     */
    using AckTimeoutFn = std::function<int()>;
    void setAckTimeoutFn(AckTimeoutFn fn) { m_ackTimeoutFn = std::move(fn); }

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
    SendResult sendChunked(QString const &peer, QString const &body);

    /**
     * @brief Notify the manager that we received a chunked DATA frame.
     *        Called from processCommandActivity's RX hook.
     */
    void onChunkReceived(QString const &fromCall, ParsedChunk const &chunk);

    /**
     * @brief Notify the manager that we received an ACK for chunk `seq`
     *        from `fromCall`. Called from processCommandActivity's ACK
     *        handler.
     */
    void onAckReceived(QString const &fromCall, int seq);

    /**
     * @brief Notify the manager that we received a NACK for chunk `seq`
     *        from `fromCall`. Triggers immediate retransmission of the
     *        in-flight chunk if it matches.
     */
    void onNackReceived(QString const &fromCall, int seq);

  signals:
    /**
     * @brief Manager wants this text TX'd. UI_Constructor wires this
     *        to enqueueMessage(PriorityHigh, text, -1, nullptr).
     */
    void wantToTransmit(QString const &text);

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

  private slots:
    void onAckTimerExpired();
    void onQuietTimerExpired();
    void onAssemblyEvictTimerExpired();
    void onTxIdlePollTick();

  private:
    // Fire the next pending chunk for `peer`, or finish the send if
    // all chunks have been delivered.
    void sendNextChunk(QString const &peer);
    // Send a NACK to `peer` for chunk `seq`, rate-limited per peer.
    void tryNack(QString const &peer, int seq);
    // Send an ACK to `peer` for chunk `seq` (always, unrate-limited).
    void sendAck(QString const &peer, int seq);
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

    QString                     m_myCall;
    int                         m_nextMsgId{MSG_ID_MIN};
    QHash<QString, SendState>   m_sends;
    QHash<QString, RxState>     m_recv;

    IdleCheckFn                 m_txIdleCheck;
    AckTimeoutFn                m_ackTimeoutFn;
    QTimer                     *m_txIdlePollTimer{nullptr};
    bool                        m_arqEnabled{false};
};

}  // namespace ChunkedArq

#endif  // CHUNKEDARQ_H
