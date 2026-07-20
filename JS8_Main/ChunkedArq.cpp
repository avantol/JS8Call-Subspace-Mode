/**
 * @file ChunkedArq.cpp
 * @brief Chunked stop-and-wait ARQ implementation.
 */
#include "JS8_Main/ChunkedArq.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QRegularExpression>

#include "Radio.h"
#include <QRegularExpressionMatch>
#include <QSettings>

// CRCpp is header-only; same setup as Varicode.cpp uses.
#define CRCPP_INCLUDE_ESOTERIC_CRC_DEFINITIONS
#define CRCPP_USE_CPP11
#include <vendor/CRCpp/CRC.h>

Q_LOGGING_CATEGORY(chunkedarq_js8, "chunkedarq.js8", QtWarningMsg)

// [BUILD 339 TODO #103] File-transfer wire-prefix family: "F/V<n>"
// at the start of chunk 1, either the bare token (prefix-only
// chunk-1 truncation) or followed by a space + content.
static QRegularExpression const kFileXferFamilyRe{
    QStringLiteral(R"(^[FL]/V\d+(\s|$))")};  // F=file, L=web link

namespace ChunkedArq {

// --- Wire format ------------------------------------------------------------

// Match "<from>: <to> <body> #NN.CC/TT.HHHH" with optional trailing
// JS8 ♢ end-of-message marker and surrounding whitespace. Body is
// captured non-greedily so a body containing '#' doesn't fool the
// parser into eating part of the marker.
//
// Callsign chars: A-Z 0-9 plus '/' (portable) and '@' (group calls).
//
// Note: QRegularExpression is initialized lazily on first use, so a
// static lives across calls without re-compiling the pattern.
namespace {

QRegularExpression const &dataFrameRegex() {
    static QRegularExpression const re(
        QStringLiteral(
            "^\\s*(?<from>[A-Z0-9/@]+)\\s*:\\s*"
            "(?<to>[A-Z0-9/@]+)\\s+"
            "(?<body>.*?)\\s*"
            "#(?<msg>\\d{2})\\.(?<chunk>\\d{2})/(?<total>\\d{2})\\.(?<crc>[0-9A-F]{4})"));
    return re;
}

}  // namespace

QString bodyCrcHex(QString const &body) {
    // JS8 uppercases all text on the wire, so the receiver always
    // sees uppercase. Hash against the canonical (uppercased) form so
    // both ends produce the same CRC.
    QByteArray const bytes = body.toUpper().toLatin1();
    auto const crc = CRC::Calculate(bytes.data(),
                                    static_cast<size_t>(bytes.size()),
                                    CRC::CRC_16_CCITTFALSE());
    return QStringLiteral("%1").arg(crc, 4, 16, QLatin1Char('0')).toUpper();
}

QString normalizeBody(QString const &body) {
    // Collapse any whitespace run (spaces, tabs, newlines) to a
    // single space, then strip leading/trailing. Matches what JS8's
    // wire form actually carries — keeps the sender's CRC consistent
    // with the body the receiver decodes.
    static QRegularExpression const whitespace(
        QStringLiteral("\\s+"));
    return body.trimmed().replace(whitespace, QStringLiteral(" "));
}

QString encodeChunkedData(QString const &myCall,
                          QString const &peer,
                          QString const &body,
                          int            msgId,
                          int            chunkId,
                          int            total) {
    QString const crc = bodyCrcHex(body);
    return QStringLiteral("%1: %2 %3 #%4.%5/%6.%7")
        .arg(myCall,
             peer,
             body,
             QString::number(msgId).rightJustified(2, QLatin1Char('0')),
             QString::number(chunkId).rightJustified(2, QLatin1Char('0')),
             QString::number(total).rightJustified(2, QLatin1Char('0')),
             crc);
}

bool parseChunkedData(QString const &text, ParsedChunk &out) {
    auto const match = dataFrameRegex().match(text);
    if (!match.hasMatch()) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-DIAG] parseChunkedData REGEX-MISS text=" << text;
        return false;
    }
    bool ok = false;
    int const msgId = match.captured("msg").toInt(&ok);
    if (!ok || msgId < MSG_ID_MIN || msgId > MSG_ID_MAX) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-DIAG] parseChunkedData BAD-MSGID raw=" << match.captured("msg")
            << "parsed=" << msgId << "ok=" << ok << "text=" << text;
        return false;
    }
    int const chunkId = match.captured("chunk").toInt(&ok);
    if (!ok || chunkId < 1) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-DIAG] parseChunkedData BAD-CHUNKID raw=" << match.captured("chunk")
            << "parsed=" << chunkId << "ok=" << ok << "text=" << text;
        return false;
    }
    int const total = match.captured("total").toInt(&ok);
    if (!ok || total < 1 || total > MAX_CHUNKS_ROLLOVER || chunkId > total) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-DIAG] parseChunkedData BAD-TOTAL raw=" << match.captured("total")
            << "parsed=" << total << "chunkId=" << chunkId << "text=" << text;
        return false;
    }
    out.from    = match.captured("from");
    out.to      = match.captured("to");
    out.body    = match.captured("body").trimmed();
    out.msgId   = msgId;
    out.chunkId = chunkId;
    out.total   = total;
    out.crcHex  = match.captured("crc");
    qCWarning(chunkedarq_js8)
        << "[ARQ-DIAG] parseChunkedData OK from=" << out.from << "to=" << out.to
        << "msgId=" << out.msgId << "chunk=" << out.chunkId << "/" << out.total
        << "crc=" << out.crcHex << "bodyLen=" << out.body.length();
    return true;
}

std::optional<QString> tryFormatChunkedDisplay(QString const &fullText) {
    // Display-time helper. The input may include the conversation
    // panel's visual prefix ("⚡ - HH:MM:SS - (1185) - ") prepended by
    // writeMessageTextToUI, so we can't reuse parseChunkedData's
    // start-anchored regex. Search for the wire-format pattern
    // ANYWHERE in the text instead.
    static QRegularExpression const re(
        QStringLiteral(
            R"((?<from>[A-Z0-9/@]+)\s*:\s*(?<to>[A-Z0-9/@]+)\s+)"
            R"((?<body>.*?)\s*#(?<msg>\d{2})\.(?<chunk>\d{2})/)"
            R"((?<total>\d{2})\.(?<crc>[0-9A-Fa-f]{4}))"));
    auto const m = re.match(fullText);
    if (!m.hasMatch()) {
        return std::nullopt;
    }
    bool ok = false;
    int const msgId = m.captured("msg").toInt(&ok);
    if (!ok || msgId < MSG_ID_MIN || msgId > MSG_ID_MAX) {
        return std::nullopt;
    }
    int const chunkId = m.captured("chunk").toInt(&ok);
    if (!ok || chunkId < 1) {
        return std::nullopt;
    }
    int const total = m.captured("total").toInt(&ok);
    if (!ok || total < 1 || total > MAX_CHUNKS_ROLLOVER ||
        chunkId > total) {
        return std::nullopt;
    }
    QString const from = m.captured("from");
    QString const body = m.captured("body").trimmed();
    // NOTE: no CRC check here. At display time the body text may have
    // accumulated frame-join spacing drift (typeahead concatenation,
    // band-activity frame join) that diverges from what the sender
    // computed CRC over. The Manager's onChunkReceived path runs the
    // authoritative CRC check (against the pristine assembled-message
    // text from processCommandActivity) and drives ACK/NACK accordingly.
    // Display is purely cosmetic — rewrite on any pattern match so the
    // operator sees clean text even when CRC is borderline.
    return QStringLiteral("%1: %2 (%3/%4)")
        .arg(from, body)
        .arg(chunkId)
        .arg(total);
}

QList<QString> splitIntoChunks(QString const &body, int maxChunkBody) {
    QString const normalized = normalizeBody(body);
    QList<QString> chunks;
    if (normalized.isEmpty()) {
        chunks << QString();
        return chunks;
    }
    if (normalized.size() <= maxChunkBody) {
        chunks << normalized;
        return chunks;
    }
    QString remaining = normalized;
    while (!remaining.isEmpty()) {
        if (remaining.size() <= maxChunkBody) {
            chunks << remaining;
            break;
        }
        // Prefer a space within the window — keeps words intact.
        int cut = remaining.lastIndexOf(QLatin1Char(' '), maxChunkBody);
        if (cut <= 0) {
            // No space in window — force hard split (long word).
            cut = maxChunkBody;
        }
        chunks << remaining.left(cut);
        // Strip leading spaces (one or more) from the remainder.
        int start = cut;
        while (start < remaining.size() && remaining[start] == QLatin1Char(' ')) {
            ++start;
        }
        remaining = remaining.mid(start);
    }
    return chunks;
}

// --- Manager ---------------------------------------------------------------

// [PERSIST MSG ID 2026-06-12 build 254]
// QSettings key for the next ARQ msg id. Persisted across sender
// restarts so a fresh process doesn't reuse IDs the receiver still
// has in its deliveredMsgs dedup set. Without this, the receiver
// silently drops "new" msgIds that happen to collide with previously-
// delivered ones — diagnosed from 2026-06-12 testing where today's
// msgId=1 hit the dedup cache from yesterday's session and never
// fired messageDelivered.
static constexpr char const *kNextMsgIdSettingsKey = "chunkedArq/nextMsgId";

Manager::Manager(QObject *parent) : QObject(parent) {
    QSettings settings;
    int const stored = settings.value(QString::fromLatin1(kNextMsgIdSettingsKey),
                                      MSG_ID_MIN).toInt();
    m_nextMsgId = (stored >= MSG_ID_MIN && stored <= MSG_ID_MAX)
                      ? stored
                      : MSG_ID_MIN;
    qCWarning(chunkedarq_js8)
        << "[ARQ] persistent msg-id counter restored from settings:"
        << m_nextMsgId;
}

Manager::~Manager() = default;

bool Manager::isActiveSession(QString const &peer) const {
    auto it = m_recv.constFind(peer);
    return it != m_recv.constEnd() && it.value().sessionActive;
}

bool Manager::hasActiveSession() const {
    if (!m_sends.isEmpty()) {
        return true;
    }
    for (auto it = m_recv.constBegin(); it != m_recv.constEnd(); ++it) {
        if (!it.value().assemblies.isEmpty()) {
            return true;
        }
        // [TODO #107] Binary (V3) reassembly counts as an active
        // session — the UI lock must span native transfers too.
        if (it.value().nativeWin.active ||
            !it.value().binaryAssemblies.isEmpty()) {
            return true;
        }
    }
    return false;
}

void Manager::haltAll() {
    // Operator hit Halt — tear down every in-flight send and the
    // reassembly buffers. Emit sendFailed("halted") for each pending
    // send so UI / TCP API listeners observe the abort. Clears MSG-cmd
    // state automatically (it lives on each SendState).
    qCWarning(chunkedarq_js8)
        << "[ARQ] haltAll: sends=" << m_sends.size()
        << "recv=" << m_recv.size();

    for (auto it = m_sends.begin(); it != m_sends.end(); ++it) {
        if (it.value().ackTimer) {
            it.value().ackTimer->stop();
        }
        emit sendFailed(it.key(), it.value().msgId,
                        it.value().nextIdx,
                        it.value().chunks.size(),
                        it.value().totalRetries,
                        QStringLiteral("halted"));
        // [TODO #51 2026-06-10 build 235] restore original body
        if (!it.value().originalBody.isEmpty()) {
            emit sendRestoreRequested(it.value().originalBody,
                                      QStringLiteral("halted"));
        }
    }
    if (!m_sends.isEmpty()) {
        emit progressEnd();
    }
    m_sends.clear();

    for (auto it = m_recv.begin(); it != m_recv.end(); ++it) {
        if (it.value().quietTimer) {
            it.value().quietTimer->stop();
        }
        for (QTimer *t : it.value().evictTimers) {
            if (t) t->stop();
        }
        // [TODO #107] Native (V3) collect windows / partial binary
        // assemblies die with the halt too.
        clearNativeState(it.value());
    }
    m_recv.clear();
    m_nativeOrphans.clear();

    if (m_txIdlePollTimer && m_txIdlePollTimer->isActive()) {
        m_txIdlePollTimer->stop();
    }
}

// [WIRE-NORMALIZE 2026-06-12 build 257]
// JS8's Varicode/Huffman freetext encoding inserts a space after the
// "MSG TO:" directed-cmd marker even when the wire body lacks one.
// The chunked-ARQ per-chunk CRC is computed by the sender BEFORE the
// wire encoding sees the body; the receiver's CRC (computed over the
// post-encoding body) then mismatches whenever the body contains
// "MSG TO:<non-space>". Pre-normalize that specific pattern here to
// match what the wire will produce.
//
// Build 256 had this too broad — matched ANY ":<non-space>", which
// would modify arbitrary operator-typed text (timestamps like 10:30,
// URLs, sentence "Note:foo", etc.). Build 257 narrows it to the only
// pattern we know JS8 transforms: the "MSG TO:" directed-cmd marker
// followed immediately by an addressee with no space.
//
// Concrete observed case (2026-06-12 ARQ MSG TO: test): sender sent
// "MSG TO:WM8Q ..."; receiver decoded "MSG TO: WM8Q ..."; CRCs
// mismatched every chunk; NACK loop on every retry.
//
// The directed-frame prefix "<myCall>: <peer> " produced by
// encodeChunkedData is unaffected — that `:` is always followed by
// a space anyway.
static QString normalizeForWireEncoding(QString const &body) {
    static QRegularExpression const msgToNoSpaceRe(
        QStringLiteral(R"(\bMSG\s+TO:(?=\S))"),
        QRegularExpression::CaseInsensitiveOption);
    return QString(body).replace(msgToNoSpaceRe, QStringLiteral("MSG TO: "));
}

SendResult Manager::sendChunked(QString const &peer, QString const &inputBody,
                                int submode, int const maxChunks) {
    QString const body = normalizeForWireEncoding(inputBody);
    if (body != inputBody) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-TX] wire-normalize: inserted space(s) after ':'"
            << "origLen=" << inputBody.size() << "normLen=" << body.size();
    }
    SendResult result;
    if (m_sends.contains(peer) && !m_sends[peer].chunks.isEmpty()
        && m_sends[peer].nextIdx < m_sends[peer].chunks.size()) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-TX] sendChunked rejected: send already in flight for"
            << peer;
        emit sendFailed(peer, 0, 0, 0, 0, QStringLiteral("busy"));
        // [TODO #51 2026-06-10 build 235] restore caller's body —
        // even on "busy" reject, the operator's text should reappear.
        emit sendRestoreRequested(body, QStringLiteral("busy"));
        result.error = QStringLiteral("busy");
        return result;
    }
    auto const chunks = splitIntoChunks(body);
    if (chunks.size() > maxChunks) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-TX] sendChunked rejected: message would need"
            << chunks.size() << "chunks; max is" << maxChunks;
        emit sendFailed(peer, 0, 0, chunks.size(), 0,
                        QStringLiteral("too_long"));
        // [TODO #51 2026-06-10 build 235] restore on too_long
        emit sendRestoreRequested(body, QStringLiteral("too_long"));
        result.error = QStringLiteral("too_long");
        result.totalChunks = chunks.size();
        return result;
    }

    auto &state = getOrCreateSend(peer);
    state.msgId        = m_nextMsgId;
    state.chunks       = chunks;
    state.nextIdx      = 0;
    state.retries      = 0;
    state.originalBody = body;
    state.wasMsgCmd    = false;
    state.msgAddressee.clear();
    // [BUILD 331-arqTimeoutLock] Lock the submode the chunks will be
    // transmitted in. Used downstream by armAckTimer to compute the
    // per-chunk ACK timeout, eliminating the race window where an
    // operator mode-switch between sendChunked and the actual TX
    // would have made the timer use the wrong mode's timeout.
    state.txSubmode    = submode;

    // MSG-cmd detection. Match the JS8 directed-cmd words "MSG TO:"
    // (slot 10, store at a station — capture addressee) and the bare
    // "MSG" (slot 9, this is a complete message — no addressee).
    // Case-insensitive to be forgiving of operator-typed input.
    QString const trimmedBody = body.trimmed();
    static QRegularExpression const msgToRe(
        QStringLiteral(R"(^\s*MSG\s+TO\s*:\s*(?<addr>[A-Z0-9/@]+))"),
        QRegularExpression::CaseInsensitiveOption);
    if (auto const m = msgToRe.match(trimmedBody); m.hasMatch()) {
        state.wasMsgCmd    = true;
        state.msgAddressee = m.captured("addr").toUpper();
        qCWarning(chunkedarq_js8)
            << "[ARQ-TX] MSG TO: detected — addressee=" << state.msgAddressee
            << "(will route to inbox on sendComplete)";
    } else if (trimmedBody.startsWith(QStringLiteral("MSG "),
                                       Qt::CaseInsensitive) ||
               trimmedBody.compare(QStringLiteral("MSG"),
                                    Qt::CaseInsensitive) == 0) {
        state.wasMsgCmd = true;
        qCWarning(chunkedarq_js8)
            << "[ARQ-TX] MSG (bare) detected — no addressee"
               "(will route to MY inbox on sendComplete)";
    }

    if (++m_nextMsgId > MSG_ID_MAX) {
        m_nextMsgId = MSG_ID_MIN;
    }
    // [PERSIST MSG ID 2026-06-12 build 254] Persist after every bump so
    // a process restart resumes from the next value, not from 1.
    {
        QSettings settings;
        settings.setValue(QString::fromLatin1(kNextMsgIdSettingsKey),
                          m_nextMsgId);
    }

    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] sendChunked starting: peer=" << peer
        << "msgId=" << state.msgId << "chunks=" << chunks.size()
        << "bodyChars=" << body.size()
        << "wasMsgCmd=" << state.wasMsgCmd;

    sendNextChunk(peer);

    result.ok          = true;
    result.msgId       = state.msgId;
    result.totalChunks = chunks.size();
    return result;
}

// [TODO #107] V3 native-binary send. Mirrors sendChunked's guards and
// bookkeeping exactly; the payload is raw envelope bytes and the FSM's
// `chunks` list becomes a size mirror (empty strings) so every shared
// site (busy guard, completion check, progress, haltAll) works
// untouched.
SendResult Manager::sendChunkedBinary(QString const &peer,
                                      QByteArray const &envelope,
                                      int const submode,
                                      int const chunkBytes) {
    SendResult result;
    if (m_sends.contains(peer) && !m_sends[peer].chunks.isEmpty() &&
        m_sends[peer].nextIdx < m_sends[peer].chunks.size()) {
        qCWarning(chunkedarq_js8)
            << "[V3-TX] sendChunkedBinary rejected: send in flight for"
            << peer;
        emit sendFailed(peer, 0, 0, 0, 0, QStringLiteral("busy"));
        result.error = QStringLiteral("busy");
        return result;
    }
    auto const binChunks =
        NativeBinary::splitIntoBinaryChunks(envelope, chunkBytes);
    if (binChunks.isEmpty() ||
        binChunks.size() > MAX_CHUNKS_ROLLOVER) {
        qCWarning(chunkedarq_js8)
            << "[V3-TX] sendChunkedBinary rejected: envelope needs"
            << binChunks.size() << "chunks; max" << MAX_CHUNKS_ROLLOVER;
        emit sendFailed(peer, 0, 0, binChunks.size(), 0,
                        QStringLiteral("too_long"));
        result.error = QStringLiteral("too_long");
        result.totalChunks = binChunks.size();
        return result;
    }

    auto &state = getOrCreateSend(peer);
    state.msgId = m_nextMsgId;
    state.isBinary = true;
    state.binaryChunks = binChunks;
    state.binaryTotalBytes = envelope.size();
    state.binaryChunkBytesEach = chunkBytes;
    // Size mirror for the shared FSM sites (see struct comment).
    state.chunks = QList<QString>();
    for (int i = 0; i < binChunks.size(); ++i)
        state.chunks.append(QString());
    state.nextIdx = 0;
    state.retries = 0;
    state.originalBody.clear();  // nothing to restore into the box
    state.wasMsgCmd = false;
    state.msgAddressee.clear();
    state.txSubmode = submode;

    if (++m_nextMsgId > MSG_ID_MAX) {
        m_nextMsgId = MSG_ID_MIN;
    }
    {
        QSettings settings;
        settings.setValue(QString::fromLatin1(kNextMsgIdSettingsKey),
                          m_nextMsgId);
    }

    qCWarning(chunkedarq_js8)
        << "[V3-TX] sendChunkedBinary starting: peer=" << peer
        << "msgId=" << state.msgId << "chunks=" << binChunks.size()
        << "envelopeBytes=" << envelope.size()
        << "chunkBytes=" << chunkBytes;

    sendNextChunk(peer);

    result.ok = true;
    result.msgId = state.msgId;
    result.totalChunks = binChunks.size();
    return result;
}

void Manager::sendNextChunk(QString const &peer) {
    auto sendIt = m_sends.find(peer);
    if (sendIt == m_sends.end()) {
        return;
    }
    SendState &state = sendIt.value();
    if (state.nextIdx >= state.chunks.size()) {
        // All chunks delivered. Emit msgDelivered FIRST so the UI hook
        // can route to the inbox while the SendState is still live
        // (preserves originalBody / msgAddressee). Then emit
        // sendComplete and drop the state.
        if (state.wasMsgCmd) {
            qCWarning(chunkedarq_js8)
                << "[ARQ-TX] sendComplete — routing MSG to inbox: peer="
                << peer << "addressee=" << state.msgAddressee;
            emit msgDelivered(peer, state.msgAddressee,
                              state.originalBody, state.msgId);
        }
        emit sendComplete(peer, state.msgId,
                          state.chunks.size(), state.totalRetries);
        emit progressEnd();
        m_sends.remove(peer);
        return;
    }
    int const cc = state.nextIdx + 1;          // 1-based wire chunk_id
    int const tt = state.chunks.size();

    // [TODO #107] V3 binary branch: marker on chunk 1 and every
    // MARKER_INTERVALth chunk (cc % 4 == 1) — chunk 1's marker is
    // load-bearing (TOTAL + KB) and rides EVERY chunk-1 retransmit
    // automatically since the decision is by cc. Periodic markers are
    // advisory (callsign ID + PCRC refresh). The rest of this
    // function's timer/progress machinery is shared verbatim.
    if (state.isBinary) {
        QByteArray const &binBody = state.binaryChunks[state.nextIdx];
        // [BUILD 342.9 lastAck] retries > 0: EVERY retransmit carries
        // the marker, not just the cadence chunks. A markerless retry
        // is anonymous (SEQ + CHK4 only) — if the receiver's window is
        // gone it can never re-ACK. Bench 2026-07-19: blocked final
        // ACK → receiver delivered + closed → 3 retries of chunk 3/3
        // all orphaned → sender failed a COMPLETED transfer (the
        // classic stop-and-wait last-ACK hole; V2 is immune because
        // every V2 chunk is text with full identity). With the marker,
        // handleNativeMarker's deliveredMsgs check re-ACKs.
        bool const wantMarker =
            (cc % NativeBinary::MARKER_INTERVAL) == 1 || cc == 1 ||
            state.retries > 0;
        QString markerText;
        if (wantMarker) {
            quint16 const pcrc = NativeBinary::payloadCrc16(binBody);
            markerText = encodeChunkedData(
                m_myCall, peer,
                NativeBinary::composeMarkerBody(
                    cc == 1, state.binaryTotalBytes, pcrc,
                    state.binaryChunkBytesEach),
                state.msgId, cc, tt);
        }
        if (state.ackTimer) {
            state.ackTimer->stop();
        }
        state.awaitingTxDone = true;
        state.awaitingSinceMs = QDateTime::currentMSecsSinceEpoch();
        qCWarning(chunkedarq_js8)
            << "[V3-TX] sending chunk peer=" << peer
            << "msgId=" << state.msgId << "chunk=" << cc << "/" << tt
            << "bytes=" << binBody.size()
            << "marker=" << (markerText.isEmpty()
                                 ? QStringLiteral("(none)")
                                 : markerText)
            << "retries=" << state.retries;
        emit progressUpdate(cc, tt, state.totalRetries);
        emit wantToTransmitNativeChunk(peer, markerText, cc, tt,
                                       binBody);
        ensureTxIdlePolling();
        return;
    }

    QString const &chunkBody = state.chunks[state.nextIdx];
    QString const text =
        encodeChunkedData(m_myCall, peer, chunkBody, state.msgId, cc, tt);

    // DO NOT arm the ACK timer here — we'd burn ~4–7.5 s of the budget
    // on Subspace (more on slower modes) waiting for our own
    // cycle-aligned TX to even start. Instead, mark the peer as
    // awaiting-TX-done and let the idle poller arm the ACK timer once
    // JS8Call reports the TX has fully drained. Mirrors the Python
    // prototype's _wait_for_tx_done_then_arm_timer().
    if (state.ackTimer) {
        state.ackTimer->stop();   // belt-and-suspenders: prior arming cleared
    }
    state.awaitingTxDone   = true;
    state.awaitingSinceMs  = QDateTime::currentMSecsSinceEpoch();

    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] sending chunk peer=" << peer
        << "msgId=" << state.msgId
        << "chunk=" << cc << "/" << tt
        << "retries=" << state.retries
        << "text=" << text;

    // [STATUS-BAR PROGRESS 2026-06-11 build 251] UI hook overrides
    // the "Tx: <message>" status-bar label with the ARQ progress
    // string until progressEnd fires from a terminal path below.
    // [BUILD 309] TODO #69: pass totalRetries (cumulative across the
    // whole super-message) rather than state.retries (per-chunk,
    // resets to 0 on every ACK). Operator saw the count flicker
    // back to 0 after each successful chunk; they expect a running
    // total of how stuck the entire transfer has been.
    emit progressUpdate(cc, tt, state.totalRetries);

    emit wantToTransmit(text);
    ensureTxIdlePolling();
}

void Manager::ensureTxIdlePolling() {
    if (!m_txIdlePollTimer) {
        m_txIdlePollTimer = new QTimer(this);
        m_txIdlePollTimer->setInterval(TX_IDLE_POLL_INTERVAL_MS);
        connect(m_txIdlePollTimer, &QTimer::timeout,
                this, &Manager::onTxIdlePollTick);
    }
    if (!m_txIdlePollTimer->isActive()) {
        m_txIdlePollTimer->start();
    }
}

void Manager::armAckTimer(QString const &peer, SendState &state) {
    if (!state.ackTimer) {
        state.ackTimer = new QTimer(this);
        state.ackTimer->setSingleShot(true);
        state.ackTimer->setProperty("peer", peer);
        connect(state.ackTimer, &QTimer::timeout,
                this, &Manager::onAckTimerExpired);
    }
    // [BUILD 331-arqTimeoutLock] Pass the chunk's TX-time submode
    // (locked at sendChunked) so the timeout is correct for the mode
    // the chunk actually went out in, regardless of any operator
    // mode-switch between sendChunked and now.
    int const timeoutMs = m_ackTimeoutFn ? m_ackTimeoutFn(state.txSubmode)
                                         : DEFAULT_ACK_TIMEOUT_MS;
    state.ackTimer->start(timeoutMs);
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] ACK timer armed post-TX-done: peer=" << peer
        << "msgId=" << state.msgId
        << "timeoutMs=" << timeoutMs;
}

void Manager::onTxIdlePollTick() {
    // No callback wired → can't tell idle from busy. Fall back to the
    // pre-Fix-B behaviour: arm immediately (we lose the TX-time
    // headroom but the state machine doesn't wedge).
    bool const idleCheckMissing = !m_txIdleCheck;
    bool const idleNow = idleCheckMissing ? true : m_txIdleCheck();
    qint64 const nowMs = QDateTime::currentMSecsSinceEpoch();

    // [DYNAMIC CAP 2026-06-12 build 262] Per-submode safety cap so slow
    // legacy modes (Normal 15s, Slow 30s) get enough TX-wait budget for
    // a multi-frame chunk to finish before we arm the ACK timer. Read
    // via callback so submode is sampled at THIS poll tick (mid-QSO
    // mode switches take effect immediately). Falls back to the original
    // 90 s constant if the host hasn't wired the callback.
    int const dynamicCapMs = m_txIdleCapFn ? m_txIdleCapFn() : 90000;
    bool anyStillAwaiting = false;
    for (auto it = m_sends.begin(); it != m_sends.end(); ++it) {
        SendState &st = it.value();
        if (!st.awaitingTxDone) continue;

        bool const sanityCapHit =
            (nowMs - st.awaitingSinceMs) >= dynamicCapMs;

        if (idleNow || sanityCapHit) {
            if (sanityCapHit && !idleNow) {
                qCWarning(chunkedarq_js8)
                    << "[ARQ-TX] TX-idle safety cap hit: peer=" << it.key()
                    << "ms=" << (nowMs - st.awaitingSinceMs)
                    << "capMs=" << dynamicCapMs
                    << "— arming ACK timer anyway";
            }
            st.awaitingTxDone = false;
            armAckTimer(it.key(), st);
        } else {
            anyStillAwaiting = true;
        }
    }

    if (!anyStillAwaiting && m_txIdlePollTimer) {
        m_txIdlePollTimer->stop();
    }
}

void Manager::onAckReceived(QString const &fromCall, int seq) {
    auto sendIt = m_sends.find(fromCall);
    if (sendIt == m_sends.end()) {
        return;  // no in-flight send for this peer
    }
    SendState &state = sendIt.value();
    int const expectedCc = state.nextIdx + 1;
    // [BUILD 339 TODO #104] Wire seq is modulo-31; compare against
    // the outstanding chunk's wire representation.
    if (seq != ackWireSeq(expectedCc)) {
        qCDebug(chunkedarq_js8)
            << "[ARQ-TX] stale ACK ignored: peer=" << fromCall
            << "seq=" << seq << "expected=" << expectedCc
            << "(wire" << ackWireSeq(expectedCc) << ")";
        return;
    }
    // Chunk delivered. Cancel ACK timer, advance index, fire progress,
    // send next chunk.
    if (state.ackTimer) {
        state.ackTimer->stop();
    }
    state.retries = 0;
    state.nextIdx += 1;
    emit sendProgress(fromCall, state.msgId, state.nextIdx, state.chunks.size());
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] ACK received peer=" << fromCall
        << "chunk=" << seq << "/" << state.chunks.size()
        << "next=" << state.nextIdx;
    sendNextChunk(fromCall);
}

void Manager::onNackReceived(QString const &fromCall, int seq) {
    auto sendIt = m_sends.find(fromCall);
    if (sendIt == m_sends.end()) {
        return;
    }
    SendState &state = sendIt.value();
    int const expectedCc = state.nextIdx + 1;
    // [BUILD 342.10 implicitAck] Cumulative semantics: the receiver
    // only opens window k after collecting chunk k-1 (stop-and-wait
    // invariant), so NACK(expected+1) PROVES our in-flight chunk
    // arrived and its ACK was lost in transit. Treat it as an
    // implicit ACK(expected) — the resulting fresh send of k is
    // exactly what the receiver is starving for. Single step only.
    // Bench 2026-07-19 (muted ACK 2): receiver NACK 3'd while we
    // burned 3 retries of a chunk it already had — a 2-minute detour
    // this branch closes at the first NACK.
    if (expectedCc + 1 <= state.chunks.size() &&
        seq == ackWireSeq(expectedCc + 1)) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-TX] NACK for next chunk => implicit ACK: peer="
            << fromCall << "nacked=" << (expectedCc + 1)
            << "implies delivered=" << expectedCc;
        onAckReceived(fromCall, ackWireSeq(expectedCc));
        return;
    }
    // [BUILD 339 TODO #104] Modulo-31 wire compare (see onAckReceived).
    if (seq != ackWireSeq(expectedCc)) {
        qCDebug(chunkedarq_js8)
            << "[ARQ-TX] stale NACK ignored: peer=" << fromCall
            << "seq=" << seq << "expected=" << expectedCc
            << "(wire" << ackWireSeq(expectedCc) << ")";
        return;
    }
    // Receiver explicitly NACKed our in-flight chunk. Retransmit
    // immediately (no need to wait for ACK timeout).
    if (state.ackTimer) {
        state.ackTimer->stop();
    }
    if (state.retries >= DEFAULT_MAX_RETRIES) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-TX] giving up after NACK: peer=" << fromCall
            << "msgId=" << state.msgId
            << "chunk=" << expectedCc << "/" << state.chunks.size();
        emit sendFailed(fromCall, state.msgId, state.nextIdx,
                        state.chunks.size(),
                        state.totalRetries,
                        QStringLiteral("nack_exhausted"));
        emit progressEnd();
        // [TODO #51 2026-06-10 build 235] restore on nack_exhausted
        if (!state.originalBody.isEmpty()) {
            emit sendRestoreRequested(state.originalBody,
                                      QStringLiteral("nack_exhausted"));
        }
        m_sends.remove(fromCall);
        return;
    }
    state.retries += 1;
    state.totalRetries += 1;
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] NACK -> retransmit: peer=" << fromCall
        << "chunk=" << expectedCc << "/" << state.chunks.size()
        << "retry=" << state.retries;
    sendNextChunk(fromCall);
}

void Manager::onAckTimerExpired() {
    auto *timer = qobject_cast<QTimer *>(sender());
    if (!timer) return;
    QString const peer = timer->property("peer").toString();
    auto sendIt = m_sends.find(peer);
    if (sendIt == m_sends.end()) {
        return;
    }
    SendState &state = sendIt.value();
    if (state.retries >= DEFAULT_MAX_RETRIES) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-TX] giving up after timeout: peer=" << peer
            << "msgId=" << state.msgId
            << "chunk=" << (state.nextIdx + 1) << "/" << state.chunks.size();
        emit sendFailed(peer, state.msgId, state.nextIdx,
                        state.chunks.size(),
                        state.totalRetries,
                        QStringLiteral("timeout_exhausted"));
        // [TODO #51 2026-06-10 build 235] restore on timeout_exhausted
        if (!state.originalBody.isEmpty()) {
            emit sendRestoreRequested(state.originalBody,
                                      QStringLiteral("timeout_exhausted"));
        }
        emit progressEnd();
        m_sends.remove(peer);
        return;
    }
    state.retries += 1;
    state.totalRetries += 1;
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] ACK timeout -> retransmit: peer=" << peer
        << "chunk=" << (state.nextIdx + 1) << "/" << state.chunks.size()
        << "retry=" << state.retries;
    sendNextChunk(peer);
}

void Manager::onChunkReceived(QString const &fromCall, ParsedChunk const &chunk) {
    qCWarning(chunkedarq_js8)
        << "[ARQ-DIAG] onChunkReceived ENTRY from=" << fromCall
        << "msgId=" << chunk.msgId << "chunk=" << chunk.chunkId << "/" << chunk.total
        << "advertisedCRC=" << chunk.crcHex << "bodyLen=" << chunk.body.length();
    markSessionActive(fromCall);

    auto &rx = getOrCreateRx(fromCall);

    // Mid-session-join guard (2026-06-07): if this is the FIRST time
    // we're seeing this peer's msgId AND the chunk is not chunkId == 1,
    // drop silently — we missed the start of the super-message and
    // can't assemble it without chunk 1. Common causes: program restart
    // mid-session, mode switch that flushed our state, or chunk 1 lost
    // to decode failure that we'll never see again. No ACK and no
    // buffering — the sender's ACK timer will expire for the chunk(s)
    // we ignore, retransmits will hit the same drop, and after
    // MAX_RETRIES the sender's `nack_exhausted` cleanup runs cleanly.
    // The peer was never going to deliver this super-message to us
    // anyway; failing fast is the correct behavior.
    //
    // Already-delivered messages take the dedup path below (re-ACK,
    // no re-buffer); this guard fires only for genuinely new sessions.
    // [TODO #107] Binary (V3) sessions live in binaryAssemblies /
    // binaryTotalBytes, not the text `assemblies` map — a periodic V3
    // marker (CC=5,9,…) mid-transfer must not trip this guard.
    bool const haveSession = rx.assemblies.contains(chunk.msgId) ||
                             rx.deliveredMsgs.contains(chunk.msgId) ||
                             rx.binaryTotalBytes.contains(chunk.msgId);
    if (!haveSession && chunk.chunkId != 1) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-RX] mid-session join ignored: peer=" << fromCall
            << "msgId=" << chunk.msgId << "chunk=" << chunk.chunkId
            << "of" << chunk.total
            << "(no chunk 1, dropping silently — sender will retry/give up)";
        return;
    }

    // CRC integrity check — catches silent body corruption that left
    // the marker intact (one of the three RX corruption paths the
    // Python prototype proved load-bearing).
    QString const computed = bodyCrcHex(chunk.body);
    if (computed != chunk.crcHex) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-RX] CRC mismatch peer=" << fromCall
            << "msgId=" << chunk.msgId << "chunk=" << chunk.chunkId
            << "advertised=" << chunk.crcHex << "computed=" << computed
            << "body=" << chunk.body;
        tryNack(fromCall, chunk.chunkId);
        return;
    }

    // [TODO #107] V3 native marker? It opens/refreshes a binary
    // collect window instead of storing text — and the ACK is
    // DEFERRED to collector completion (a V3 chunk isn't received
    // until its binary frames are). Already-collected chunks re-ACK
    // inside the handler (marker retransmit after our lost ACK).
    if (NativeBinary::MarkerInfo mi;
        NativeBinary::parseMarkerBody(chunk.body, &mi)) {
        handleNativeMarker(fromCall, rx, chunk, mi);
        return;
    }

    // ACK the chunk (always — duplicates need re-ACK or sender retries forever).
    sendAck(fromCall, chunk.chunkId);

    // Already-delivered msg? Skip re-buffering (sender retransmitted
    // chunks of a message we already completed). The re-ACK above
    // tells them to stop retrying.
    if (rx.deliveredMsgs.contains(chunk.msgId)) {
        return;
    }

    // Add to assembly buffer.
    auto &asm_ = rx.assemblies[chunk.msgId];
    asm_[chunk.chunkId] = chunk.body;
    rx.totals[chunk.msgId] = chunk.total;

    // MSG-cmd detection at chunk 1. JS8 uppercases all wire text, so
    // match case-insensitively to be safe. Two recognized prefixes:
    //   - "MSG TO: <addr> <rest>"  → capture addressee (route to inbox
    //                                 on full assembly, dest=<addr>)
    //   - bare "MSG <rest>"        → no addressee; treated as a self-
    //                                 directed inbox deposit on
    //                                 completion.
    // Stash both on rx.msgCmdDetected/msgCmdAddressee keyed by msgId so
    // the assembly-complete branch below can emit inboxMessageReceived
    // with the original cmd context. Chunk 1 retransmits don't re-stash
    // (the if-not-contains gate keeps the original detection sticky).
    if (chunk.chunkId == 1 && !rx.msgCmdDetected.contains(chunk.msgId)) {
        static QRegularExpression const msgToRe(
            QStringLiteral(R"(^\s*MSG\s+TO\s*:\s*(?<addr>[A-Z0-9/@]+))"),
            QRegularExpression::CaseInsensitiveOption);
        QString const probe = chunk.body.trimmed();
        if (auto const m = msgToRe.match(probe); m.hasMatch()) {
            rx.msgCmdDetected[chunk.msgId]  = true;
            rx.msgCmdAddressee[chunk.msgId] = m.captured("addr").toUpper();
            qCWarning(chunkedarq_js8)
                << "[ARQ-RX] MSG TO: detected on chunk 1 — peer=" << fromCall
                << "msgId=" << chunk.msgId
                << "addressee=" << rx.msgCmdAddressee[chunk.msgId];
        } else if (probe.startsWith(QStringLiteral("MSG "),
                                     Qt::CaseInsensitive) ||
                   probe.compare(QStringLiteral("MSG"),
                                  Qt::CaseInsensitive) == 0) {
            rx.msgCmdDetected[chunk.msgId]  = true;
            rx.msgCmdAddressee[chunk.msgId].clear();
            qCWarning(chunkedarq_js8)
                << "[ARQ-RX] bare MSG detected on chunk 1 — peer=" << fromCall
                << "msgId=" << chunk.msgId;
        } else {
            // [RELAY-VIA-ARQ RX detect 2026-06-10 build 243]
            // Body starts with "<callsign>>" → this is an ARQ relay
            // super-message. The sender computed and appended the
            // 16-bit checksum just like an on-air ">" cmd; the receiver
            // hook will populate m_messageBuffer and let
            // processBufferedActivity validate it.
            // [RELAY REGEX 2026-06-10 build 245] Allow no-space form
            // "<call>>body" (see mainwindow.cpp send-side regex).
            static QRegularExpression const relayRe(
                QStringLiteral(R"(^\s*(?<call>[A-Z0-9/]+)>\s*\S)"),
                QRegularExpression::CaseInsensitiveOption);
            if (auto const m = relayRe.match(probe); m.hasMatch()) {
                rx.relayCmdDetected[chunk.msgId] = true;
                qCWarning(chunkedarq_js8)
                    << "[ARQ-RX] relay-cmd detected on chunk 1 — peer="
                    << fromCall << "msgId=" << chunk.msgId
                    << "via=" << m.captured("call").toUpper();
            }
            // [FILE-XFER 2026-06-16 build 277] Body starts with the
            // file-transfer magic prefix "F/V1 GZIP/BASE32 " → flag
            // so the assembly-complete branch emits
            // fileMessageReceived. The "GZIP/BASE32" tail is the
            // plain-language regulatory disclosure that the payload
            // is standard zlib gzip wrapped in standard RFC 4648
            // base32, not encryption (see FileTransfer.h PREFIX_V1
            // commentary). Receivers earlier than build 277 had only
            // "F/V1 " as the trigger and will silently fail to
            // detect this; that's a clean failure mode (the
            // super-message just displays as garbled freetext rather
            // than mis-routing). The header is decoded later from
            // the assembled body in the UI slot; we only need a
            // routing flag here.
            // Build 276 had a subtle trailing-space bug here. The
            // splitIntoChunks packer can put JUST the prefix into
            // chunk 1 (the next whitespace-delimited token — the long
            // base32 header — won't fit in the remaining 60-char
            // budget). That left chunk.body == "F/V1 " (5 chars). The
            // probe = chunk.body.trimmed() stripped the trailing space
            // → "F/V1", and the startsWith("F/V1 ") with required
            // trailing space failed to match. The super-message then
            // fell through to plain delivery — confusing display in
            // the conversation panel + no file-receive dialog.
            // Cover both shapes: chunk 1 = ONLY the prefix, OR chunk 1
            // = prefix + start of header content.
            // [BUILD 339 TODO #103] Detect the F/V<n> prefix FAMILY,
            // not just the V1 literal — the chunk-1 string match is
            // the SINGLE point of file-transfer detection, and an
            // unmatched version used to fall through to plain text
            // delivery (base32 gibberish in the conversation panel).
            // Any family member routes to fileMessageReceived; the
            // UI slot does V1 → V2 → polite-unsupported triage. The
            // regex covers both chunk-1 truncation shapes (bare
            // "F/V1" prefix-only chunk vs prefix + header content —
            // the build-276 trailing-space lesson).
            else if (kFileXferFamilyRe.match(probe).hasMatch()) {
                rx.fileXferDetected[chunk.msgId] = true;
                qCWarning(chunkedarq_js8)
                    << "[ARQ-RX] file-transfer detected on chunk 1 — peer="
                    << fromCall << "msgId=" << chunk.msgId
                    << "head=" << probe.left(12);
            }
        }
    }

    // (Re-)arm stale-evict timer for this msg_id.
    QTimer *&evict = rx.evictTimers[chunk.msgId];
    if (!evict) {
        evict = new QTimer(this);
        evict->setSingleShot(true);
        evict->setProperty("peer", fromCall);
        evict->setProperty("msgId", chunk.msgId);
        connect(evict, &QTimer::timeout,
                this, &Manager::onAssemblyEvictTimerExpired);
    }
    evict->start(ASSEMBLY_EVICT_TIMEOUT_MS);

    // UI: progressive display of this chunk.
    emit chunkAdded(fromCall, chunk.body, chunk.chunkId, chunk.total);

    // Complete? Concatenate in chunk-id order, deliver, evict.
    if (asm_.size() == chunk.total) {
        QStringList parts;
        for (int i = 1; i <= chunk.total; ++i) {
            parts << asm_.value(i);
        }
        QString const assembled = parts.join(QLatin1Char(' '));

        rx.deliveredMsgs.insert(chunk.msgId);
        // Cap dedup set to bound memory (arbitrary FIFO — fine for
        // dup suppression).
        if (rx.deliveredMsgs.size() > (MSG_ID_MAX - MSG_ID_MIN + 1)) {
            for (int i = 0; i < (MSG_ID_MAX - MSG_ID_MIN + 1) / 2; ++i) {
                auto first = rx.deliveredMsgs.begin();
                if (first == rx.deliveredMsgs.end()) break;
                rx.deliveredMsgs.erase(first);
            }
        }
        if (evict) {
            evict->stop();
            evict->deleteLater();
        }
        rx.evictTimers.remove(chunk.msgId);
        rx.assemblies.remove(chunk.msgId);
        rx.totals.remove(chunk.msgId);

        qCWarning(chunkedarq_js8)
            << "[ARQ-RX] message delivered peer=" << fromCall
            << "msgId=" << chunk.msgId << "chunks=" << chunk.total
            << "bodyChars=" << assembled.size();

        // Detach the MSG-cmd / relay-cmd stash BEFORE emitting so the
        // slot has room to mutate state safely if needed.
        bool const wasMsg = rx.msgCmdDetected.value(chunk.msgId, false);
        bool const wasRelay = rx.relayCmdDetected.value(chunk.msgId, false);
        bool const wasFile = rx.fileXferDetected.value(chunk.msgId, false);
        QString const addressee = rx.msgCmdAddressee.value(chunk.msgId);
        rx.msgCmdDetected.remove(chunk.msgId);
        rx.msgCmdAddressee.remove(chunk.msgId);
        rx.relayCmdDetected.remove(chunk.msgId);
        rx.fileXferDetected.remove(chunk.msgId);

        // [FILE-XFER 2026-06-16 build 276] File-transfer super-messages
        // skip the standard messageDelivered display path — they have
        // a base32 blob body that's meaningless to render in the
        // conversation panel. Route straight to fileMessageReceived;
        // the UI slot decodes the header, prompts the operator, and
        // writes the file under the configured save dir.
        if (wasFile) {
            qCWarning(chunkedarq_js8)
                << "[ARQ-RX] file-transfer assembled — peer=" << fromCall
                << "msgId=" << chunk.msgId
                << "bodyChars=" << assembled.size();
            emit fileMessageReceived(fromCall, assembled, chunk.msgId);
            return;
        }

        emit messageDelivered(fromCall, m_myCall, assembled, chunk.msgId);

        // If chunk 1's body indicated this super-message is a MSG
        // directive, fire the inbox-deposit signal AFTER
        // messageDelivered so the conversation panel still gets its
        // assembled-body summary line first (then the operator sees
        // the modeless "saved to inbox" dialog).
        if (wasMsg) {
            qCWarning(chunkedarq_js8)
                << "[ARQ-RX] inbox deposit — peer=" << fromCall
                << "msgId=" << chunk.msgId
                << "addressee=" << (addressee.isEmpty() ? "(bare MSG)" : addressee);
            emit inboxMessageReceived(fromCall, addressee, assembled, chunk.msgId);
        }
        // [RELAY-VIA-ARQ assembly emit 2026-06-10 build 243]
        // Relay super-message: hook (chunkedArqHooks) populates
        // m_messageBuffer so processBufferedActivity validates the
        // sender-side-computed checksum and then the existing ">"
        // handler at processCommandActivity.cpp:561 fires.
        if (wasRelay) {
            qCWarning(chunkedarq_js8)
                << "[ARQ-RX] relay-cmd assembled — peer=" << fromCall
                << "msgId=" << chunk.msgId;
            emit relayMessageReceived(fromCall, assembled, chunk.msgId);
        }
    }
}

void Manager::tryNack(QString const &peer, int seq) {
    auto &rx = getOrCreateRx(peer);
    qint64 const nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - rx.lastNackMonoMs < MIN_NACK_INTERVAL_MS) {
        qCDebug(chunkedarq_js8)
            << "[ARQ-RX] NACK rate-limited peer=" << peer << "seq=" << seq;
        return;
    }
    rx.lastNackMonoMs = nowMs;
    // [BUILD 339 TODO #104] seq is the ABSOLUTE chunk id; the wire
    // numeric extra only carries 1..31, so wrap modulo-31 (safe:
    // stop-and-wait means the sender has exactly one chunk
    // outstanding and matches modulo too).
    QString const text = QStringLiteral("%1 NACK %2")
                             .arg(peer).arg(ackWireSeq(seq));
    qCWarning(chunkedarq_js8)
        << "[ARQ-RX] sending NACK peer=" << peer << "seq=" << seq
        << "delayed by" << ACK_TX_DELAY_MS << "ms for output ramp-up";
    // [NACK-TX-DELAY 2026-06-10 build 238]
    // Same 250 ms delay as ACK (see ACK_TX_DELAY_MS commentary in
    // ChunkedArq.h). Operator noted NACK frames are NOT rare in
    // practice, so apply the same audio-output ramp-up window so the
    // NACK's leading Costas tones don't appear abrupt on the wire.
    QTimer::singleShot(ACK_TX_DELAY_MS, this, [this, text]() {
        // [RESPONSE-TX SIGNAL 2026-06-14 build 268] Route via the
        // dedicated wantsResponseTx signal so the host can save/restore
        // outgoing-text widget contents around this transmission (see
        // TODO.md #57).
        emit wantsResponseTx(text);
    });
}

void Manager::sendAck(QString const &peer, int seq) {
    // [BUILD 339 TODO #104] Absolute seq → modulo-31 wire seq.
    QString const text = QStringLiteral("%1 ACK %2")
                             .arg(peer).arg(ackWireSeq(seq));
    qCWarning(chunkedarq_js8)
        << "[ARQ-RX] sending ACK peer=" << peer << "seq=" << seq
        << "delayed by" << ACK_TX_DELAY_MS << "ms for output ramp-up";
    // [ACK-TX-DELAY 2026-06-10 build 237]
    // 250 ms delay before emitting wantToTransmit so the audio output
    // device has time to settle into a clean ramp-up window before
    // the Modulator generates the ACK's Costas tones. Without this
    // the leading edge of the ACK looks abrupt at the sender's
    // receiver, suggesting the output device wasn't fully ready.
    // QTimer::singleShot's `this` parented variant cleans up if the
    // Manager is destroyed before the delay expires.
    QTimer::singleShot(ACK_TX_DELAY_MS, this, [this, text]() {
        // [RESPONSE-TX SIGNAL 2026-06-14 build 268] see sendNack above.
        emit wantsResponseTx(text);
    });
}

void Manager::markSessionActive(QString const &peer) {
    auto &rx = getOrCreateRx(peer);
    rx.sessionActive = true;
    if (!rx.quietTimer) {
        rx.quietTimer = new QTimer(this);
        rx.quietTimer->setSingleShot(true);
        rx.quietTimer->setProperty("peer", peer);
        connect(rx.quietTimer, &QTimer::timeout,
                this, &Manager::onQuietTimerExpired);
    }
    rx.quietTimer->start(SESSION_QUIET_TIMEOUT_MS);
}

void Manager::onQuietTimerExpired() {
    auto *timer = qobject_cast<QTimer *>(sender());
    if (!timer) return;
    QString const peer = timer->property("peer").toString();
    auto rxIt = m_recv.find(peer);
    if (rxIt != m_recv.end()) {
        rxIt.value().sessionActive = false;
        qCDebug(chunkedarq_js8)
            << "[ARQ-RX] session quiet timeout peer=" << peer;
    }
}

void Manager::onAssemblyEvictTimerExpired() {
    auto *timer = qobject_cast<QTimer *>(sender());
    if (!timer) return;
    QString const peer = timer->property("peer").toString();
    int const msgId = timer->property("msgId").toInt();
    auto rxIt = m_recv.find(peer);
    if (rxIt == m_recv.end()) return;
    RxState &rx = rxIt.value();
    if (rx.assemblies.contains(msgId)) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-RX] stale assembly evicted peer=" << peer
            << "msgId=" << msgId
            << "haveChunks=" << rx.assemblies[msgId].size()
            << "expected=" << rx.totals.value(msgId);
        rx.assemblies.remove(msgId);
        rx.totals.remove(msgId);
    }
    // [TODO #107] Stale BINARY assembly for this msgId dies too —
    // including its collect window if it's the one in flight.
    if (rx.binaryTotalBytes.contains(msgId) ||
        rx.binaryAssemblies.contains(msgId)) {
        qCWarning(chunkedarq_js8)
            << "[V3-RX] stale binary assembly evicted peer=" << peer
            << "msgId=" << msgId
            << "haveChunks=" << rx.binaryAssemblies.value(msgId).size();
        rx.binaryAssemblies.remove(msgId);
        rx.binaryTotalBytes.remove(msgId);
        rx.binaryChunkBytes.remove(msgId);
        if (rx.nativeWin.active && rx.nativeWin.msgId == msgId) {
            if (rx.nativeWin.collectTimer)
                rx.nativeWin.collectTimer->stop();
            rx.nativeWin.active = false;
        }
    }
    rx.evictTimers.remove(msgId);
    timer->deleteLater();
}

// --- [TODO #107] Native-binary (V3) receive path -----------------------

namespace {
constexpr int    NATIVE_ORPHAN_CAP    = 16;
constexpr qint64 NATIVE_ORPHAN_TTL_MS = 30000;
// [BUILD 342.10 rttSlots] Window-open watchdog headroom for the ACK
// round trip (our ACK TX + sender decode + sender slot-align) — this
// wait dominates and does NOT scale with chunk size, so the plain
// (frames+2) budget starved 1-frame chunks: bench 2026-07-19, window
// 3/3 (frames=1) NACKed at +11 s and exhausted its give-up budget
// before delivery was even possible. Applied at window-open only;
// re-arms after the first accepted frame use (frames+2).
constexpr int    ACK_RTT_SLOTS        = 4;
}  // namespace

void Manager::handleNativeMarker(QString const &peer, RxState &rx,
                                 ParsedChunk const &chunk,
                                 NativeBinary::MarkerInfo const &mi) {
    // [BUILD 342.10 reAckDelay] Both re-ACK paths DELAY the response
    // to land AFTER the sender's retry burst ends: the marker decodes
    // at the HEAD of the burst, with up to 8 binary frames (~30 s)
    // still airing behind it — an immediate re-ACK collides with the
    // sender's own TX (half-duplex) and is lost every time (bench
    // 2026-07-19: three straight re-ACK 2 collisions, 2-minute
    // detour). Delay = (frames-in-chunk + 1) slots; sizes are
    // retained past delivery so the delivered path can size too
    // (conservative 8-frame fallback when unknown, e.g. pre-retention
    // history or post-evict).
    auto const scheduleReAck = [this, peer](RxState &rxs, int msgId,
                                            int cc, int tt) {
        int const totalBytes = rxs.binaryTotalBytes.value(msgId, 0);
        int const kb = rxs.binaryChunkBytes.value(
            msgId, NativeBinary::DEFAULT_CHUNK_BYTES);
        int const nBytes =
            NativeBinary::chunkBytesFor(cc, tt, totalBytes, kb);
        int const frames =
            nBytes > 0
                ? (nBytes + NativeBinary::FRAME_PAYLOAD_BYTES - 1) /
                      NativeBinary::FRAME_PAYLOAD_BYTES
                : 8;
        // (frames - 1) slots, floor 1: the schedule anchor is the
        // MARKER DECODE, which already lags the marker's last frame
        // by ~1-2 slots — (frames + 1) overshot the sender's 16 s
        // post-burst listen window by seconds (bench 2026-07-19,
        // 22:50:05 keyup vs ~22:50:03-06 re-ACK decode). This path is
        // now the FALLBACK for a lost last frame; the frame-triggered
        // re-ACK above handles the common case precisely.
        int const delayMs =
            (frames > 1 ? frames - 1 : 1) * m_nativeFrameMs;
        qCWarning(chunkedarq_js8)
            << "[V3-RX] re-ACK scheduled post-burst: peer=" << peer
            << "msgId=" << msgId << "chunk=" << cc
            << "delayMs=" << delayMs;
        QTimer::singleShot(delayMs, this, [this, peer, cc]() {
            sendAck(peer, cc);
        });
    };
    // Whole message already delivered → the sender lost our final
    // ACK; re-ACK and stop.
    if (rx.deliveredMsgs.contains(chunk.msgId)) {
        scheduleReAck(rx, chunk.msgId, chunk.chunkId, chunk.total);
        return;
    }
    // Live-transfer marker (fresh, retry, or mid-join) — RX-side UI
    // feedback hook.
    emit nativeMarkerSeen(peer, chunk.chunkId, chunk.total);
    // This chunk already collected → marker retransmit after a lost
    // ACK. [BUILD 342.11 frameReAck] NO timer re-ACK here: the retry
    // burst's own last frame triggers the re-ACK precisely
    // (onNativeFrameReceived), and the open window's watchdog NACK
    // advances the sender via implicit-ACK even if every frame is
    // lost — a timer re-ACK on top double-keys and can stomp the
    // sender's NEXT chunk. The marker IS session evidence though:
    // revive the watchdog (it may have given up during the sender's
    // retry cycle) and reset its give-up budget.
    if (rx.binaryAssemblies.value(chunk.msgId).contains(chunk.chunkId)) {
        qCWarning(chunkedarq_js8)
            << "[V3-RX] marker for already-collected chunk —"
            << "frame-trigger/watchdog will answer: peer=" << peer
            << "msgId=" << chunk.msgId << "chunk=" << chunk.chunkId;
        if (rx.nativeWin.active && rx.nativeWin.msgId == chunk.msgId) {
            rx.nativeWin.noProgressNacks = 0;
            if (rx.nativeWin.collectTimer) {
                rx.nativeWin.collectTimer->start(
                    (rx.nativeWin.collector.frameCount() + 2 +
                     ACK_RTT_SLOTS) * m_nativeFrameMs);
            }
        }
        return;
    }
    if (mi.isFirstChunkForm) {
        // Chunk-1 marker carries the authoritative TOTAL envelope
        // byte count AND the chunk size (every chunk's byte count
        // derives from the pair).
        rx.binaryTotalBytes[chunk.msgId] = mi.totalBytes;
        rx.binaryChunkBytes[chunk.msgId] = mi.chunkBytes;
    } else if (!rx.binaryTotalBytes.contains(chunk.msgId)) {
        // Periodic marker for a transfer whose chunk-1 marker we
        // never saw — without TOTAL we can't size any chunk. Drop
        // silently; the mid-session-join philosophy applies (the
        // sender will exhaust retries; we could never assemble this
        // transfer anyway).
        qCWarning(chunkedarq_js8)
            << "[V3-RX] periodic marker without chunk-1 TOTAL — drop"
            << "peer=" << peer << "msgId=" << chunk.msgId
            << "chunk=" << chunk.chunkId;
        return;
    }
    // [BUILD 342.12 evictRearm] (Re-)arm — not arm-once — the
    // assembly evict timer for ANY valid marker of a live transfer
    // (first-form or periodic): the exact mirror of the V2 text
    // path's per-chunk re-arm. Armed once at chunk 1, it evicted a
    // healthy 8-chunk transfer's assembly at the 5-minute line
    // (bench 2026-07-19, haveChunks=6 — unrecoverable afterwards).
    QTimer *&evict = rx.evictTimers[chunk.msgId];
    if (!evict) {
        evict = new QTimer(this);
        evict->setSingleShot(true);
        evict->setProperty("peer", peer);
        evict->setProperty("msgId", chunk.msgId);
        connect(evict, &QTimer::timeout, this,
                &Manager::onAssemblyEvictTimerExpired);
    }
    evict->start(ASSEMBLY_EVICT_TIMEOUT_MS);
    openNativeChunkWindow(peer, rx, chunk.msgId, chunk.chunkId,
                          chunk.total, mi.pcrc, /*pcrcValid=*/true);
}

void Manager::openNativeChunkWindow(QString const &peer, RxState &rx,
                                    int const msgId, int const chunkId,
                                    int const total, quint16 const pcrc,
                                    bool const pcrcValid) {
    int const totalBytes = rx.binaryTotalBytes.value(msgId, 0);
    int const kb = rx.binaryChunkBytes.value(
        msgId, NativeBinary::DEFAULT_CHUNK_BYTES);
    int const nBytes = NativeBinary::chunkBytesFor(chunkId, total,
                                                   totalBytes, kb);
    if (nBytes <= 0) {
        qCWarning(chunkedarq_js8)
            << "[V3-RX] window open failed: inconsistent sizes"
            << "msgId=" << msgId << "chunk=" << chunkId << "/" << total
            << "totalBytes=" << totalBytes;
        return;
    }
    rx.nativeWin.active = true;
    rx.nativeWin.msgId = msgId;
    rx.nativeWin.chunkId = chunkId;
    rx.nativeWin.total = total;
    rx.nativeWin.noProgressNacks = 0;
    rx.nativeWin.collector.open(chunkId, nBytes, pcrc, pcrcValid);

    qCWarning(chunkedarq_js8)
        << "[V3-RX] window open: peer=" << peer << "msgId=" << msgId
        << "chunk=" << chunkId << "/" << total << "nBytes=" << nBytes
        << "frames=" << rx.nativeWin.collector.frameCount()
        << "pcrcValid=" << pcrcValid
        << "orphans=" << m_nativeOrphans.size();

    // Drain orphans that beat the marker through the text pipeline
    // (manager-global — orphan frames are anonymous until a window's
    // CHK4 binds them; harness-caught 2026-07-19: a per-peer store
    // drained the wrong pool when the marker created the peer state).
    qint64 const nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_nativeOrphans.begin();
         it != m_nativeOrphans.end();) {
        if (nowMs - it->monoMs > NATIVE_ORPHAN_TTL_MS) {
            it = m_nativeOrphans.erase(it);
            continue;
        }
        if (rx.nativeWin.collector.accept(it->seq, it->chk4, it->p8)) {
            it = m_nativeOrphans.erase(it);
        } else {
            ++it;
        }
    }

    // Collect watchdog: expected frames' airtime + 2 slots of slack.
    if (!rx.nativeWin.collectTimer) {
        rx.nativeWin.collectTimer = new QTimer(this);
        rx.nativeWin.collectTimer->setSingleShot(true);
        rx.nativeWin.collectTimer->setProperty("peer", peer);
        connect(rx.nativeWin.collectTimer, &QTimer::timeout, this,
                &Manager::onNativeCollectTimerExpired);
    }
    rx.nativeWin.collectTimer->start(
        (rx.nativeWin.collector.frameCount() + 2 + ACK_RTT_SLOTS) *
        m_nativeFrameMs);

    // Orphans may already complete the chunk (deep pipeline lag).
    if (rx.nativeWin.collector.complete()) {
        finishNativeChunk(peer, rx);
    }
}

bool Manager::onNativeFrameReceived(int const seq, int const chk4,
                                    QByteArray const &payload8,
                                    int const freq, qint64 const absPos) {
    Q_UNUSED(absPos);
    // Bind to the (single realistic) active window whose CHK4 tag
    // matches. Stop-and-wait means at most one active window per
    // peer, and in practice one per receiver.
    for (auto it = m_recv.begin(); it != m_recv.end(); ++it) {
        RxState &rx = it.value();
        if (!rx.nativeWin.active) continue;
        // [BUILD 342.11 frameReAck] Frames for the chunk JUST BEHIND
        // the open window, already collected = the definitive
        // stop-and-wait retry signature (our ACK was lost; the sender
        // is re-airing a chunk we have). Swallow them (they polluted
        // the orphan store — 16 orphans in the 2026-07-19 bench) and
        // re-ACK on the burst's LAST frame: the frames themselves
        // mark the burst end exactly, so the re-ACK lands inside the
        // sender's post-burst listen window — the marker-timer
        // fallback path measured from marker-decode kept losing that
        // race by <1 s.
        int const prevCc = rx.nativeWin.chunkId - 1;
        if (prevCc >= 1 && chk4 == (prevCc & 0xF) &&
            rx.binaryAssemblies.value(rx.nativeWin.msgId)
                .contains(prevCc)) {
            int const totalBytes =
                rx.binaryTotalBytes.value(rx.nativeWin.msgId, 0);
            int const kb = rx.binaryChunkBytes.value(
                rx.nativeWin.msgId, NativeBinary::DEFAULT_CHUNK_BYTES);
            int const prevBytes = NativeBinary::chunkBytesFor(
                prevCc, rx.nativeWin.total, totalBytes, kb);
            int const prevFrames =
                prevBytes > 0
                    ? (prevBytes + NativeBinary::FRAME_PAYLOAD_BYTES -
                       1) / NativeBinary::FRAME_PAYLOAD_BYTES
                    : 8;
            if (seq == prevFrames - 1) {
                qCWarning(chunkedarq_js8)
                    << "[V3-RX] retry burst of collected chunk ended"
                    << "— re-ACK: peer=" << it.key()
                    << "msgId=" << rx.nativeWin.msgId
                    << "chunk=" << prevCc;
                sendAck(it.key(), prevCc);
            }
            return true;  // ours; keep out of the orphan store
        }
        if (chk4 != (rx.nativeWin.chunkId & 0xF)) continue;
        if (!rx.nativeWin.collector.accept(seq, chk4, payload8)) {
            qCWarning(chunkedarq_js8)
                << "[V3-RX] frame rejected by collector: seq=" << seq
                << "chk4=" << chk4 << "peer=" << it.key();
            return false;
        }
        markSessionActive(it.key());
        // Progress — reset the give-up count and restart the collect
        // budget (symmetric to the sender deferring its ACK timer
        // until TX-done). A one-shot armed only at window-open expires
        // mid-chunk on auto-advance windows: the window opens before
        // our ACK even airs, so ACK RTT + 8 frames ≈ 45-53 s against
        // a 37.5 s budget (bench 2026-07-19 "NACK too soon").
        rx.nativeWin.noProgressNacks = 0;
        if (rx.nativeWin.collector.complete()) {
            finishNativeChunk(it.key(), rx);
        } else if (rx.nativeWin.collectTimer) {
            rx.nativeWin.collectTimer->start(
                (rx.nativeWin.collector.frameCount() + 2) *
                m_nativeFrameMs);
        }
        return true;
    }
    // No window (frames beat the marker, or stale RF) → the
    // manager-global orphan store; drained by CHK4 match whenever a
    // window opens.
    qint64 const nowMs = QDateTime::currentMSecsSinceEpoch();
    while (m_nativeOrphans.size() >= NATIVE_ORPHAN_CAP) {
        m_nativeOrphans.removeFirst();
    }
    m_nativeOrphans.append({seq, chk4, payload8, nowMs});
    qCWarning(chunkedarq_js8)
        << "[V3-RX] no active window — frame orphaned: seq=" << seq
        << "chk4=" << chk4 << "freq=" << freq
        << "orphans=" << m_nativeOrphans.size();
    return false;
}

void Manager::finishNativeChunk(QString const &peer, RxState &rx) {
    auto &win = rx.nativeWin;
    if (win.collectTimer) {
        win.collectTimer->stop();
    }
    if (!win.collector.crcOk()) {
        qCWarning(chunkedarq_js8)
            << "[V3-RX] chunk complete but PCRC MISMATCH — NACK"
            << "peer=" << peer << "msgId=" << win.msgId
            << "chunk=" << win.chunkId;
        tryNack(peer, win.chunkId);
        win.collector.reset();
        if (win.collectTimer) {
            win.collectTimer->start(
                (win.collector.frameCount() + 2) * m_nativeFrameMs);
        }
        return;
    }

    rx.binaryAssemblies[win.msgId][win.chunkId] = win.collector.bytes();
    // [BUILD 342.12 evictRearm] (Re-)arm the stale-evict timer on
    // every collected chunk — the exact mirror of the V2 text path
    // (onChunkReceived re-arms per chunk). It was armed ONCE at the
    // chunk-1 marker, so any transfer longer than 5 minutes had its
    // assembly evicted MID-TRANSFER (bench 2026-07-19: 8-chunk file
    // + one muted ACK crossed the line at 23:14:42, haveChunks=6 —
    // after which retry frames orphan and retry markers hit the
    // no-TOTAL drop: unrecoverable by design, silently).
    if (auto *evict = rx.evictTimers.value(win.msgId, nullptr)) {
        evict->start(ASSEMBLY_EVICT_TIMEOUT_MS);
    }
    qCWarning(chunkedarq_js8)
        << "[V3-RX] chunk complete crc=OK peer=" << peer
        << "msgId=" << win.msgId << "chunk=" << win.chunkId
        << "/" << win.total
        << "bytes=" << win.collector.bytes().size();
    sendAck(peer, win.chunkId);
    emit nativeChunkCollected(peer, win.chunkId, win.total);

    if (win.chunkId == win.total) {
        // Transfer complete — concatenate in chunk order.
        QByteArray envelope;
        auto const &bins = rx.binaryAssemblies[win.msgId];
        for (int cc = 1; cc <= win.total; ++cc) {
            envelope += bins.value(cc);
        }
        int const msgId = win.msgId;

        rx.deliveredMsgs.insert(msgId);
        if (rx.deliveredMsgs.size() > (MSG_ID_MAX - MSG_ID_MIN + 1)) {
            for (int i = 0; i < (MSG_ID_MAX - MSG_ID_MIN + 1) / 2; ++i) {
                auto first = rx.deliveredMsgs.begin();
                if (first == rx.deliveredMsgs.end()) break;
                // [BUILD 342.10 reAckDelay] Size hashes are retained
                // per delivered msgId (below) — drop them with it.
                rx.binaryTotalBytes.remove(*first);
                rx.binaryChunkBytes.remove(*first);
                rx.deliveredMsgs.erase(first);
            }
        }
        if (auto *evict = rx.evictTimers.value(msgId, nullptr)) {
            evict->stop();
            evict->deleteLater();
        }
        rx.evictTimers.remove(msgId);
        rx.binaryAssemblies.remove(msgId);
        // [BUILD 342.10 reAckDelay] binaryTotalBytes / binaryChunkBytes
        // are KEPT for delivered msgIds (two ints each): the delivered
        // re-ACK path sizes the sender's retry burst from them so the
        // re-ACK lands after the burst. Pruned with deliveredMsgs
        // (above) and in clearNativeState.
        win.active = false;

        qCWarning(chunkedarq_js8)
            << "[V3-RX] transfer complete peer=" << peer
            << "msgId=" << msgId << "chunks=" << win.total
            << "envelopeBytes=" << envelope.size();
        emit binaryMessageReceived(peer, envelope, msgId);
        return;
    }

    // AUTO-ADVANCE: expect the next chunk immediately — markerless
    // chunks bind here; a periodic marker for it merely refreshes
    // the window with a PCRC.
    openNativeChunkWindow(peer, rx, win.msgId, win.chunkId + 1,
                          win.total, /*pcrc=*/0, /*pcrcValid=*/false);
}

void Manager::onNativeCollectTimerExpired() {
    auto *timer = qobject_cast<QTimer *>(sender());
    if (!timer) return;
    nativeCollectTimeout(timer->property("peer").toString());
}

bool Manager::nativeCollectTimeout(QString const &peer) {
    auto rxIt = m_recv.find(peer);
    if (rxIt == m_recv.end()) return false;
    RxState &rx = rxIt.value();
    if (!rx.nativeWin.active || rx.nativeWin.collector.complete()) {
        return false;
    }
    if (rx.nativeWin.noProgressNacks >= NATIVE_NACK_GIVEUP) {
        // Sender answered N straight NACKs with silence — mirror its
        // DEFAULT_MAX_RETRIES give-up. Window stays passively open
        // (late frames still bind); the assembly-evict timer cleans
        // up. Without this bound the re-NACK loop kept keying for
        // 4.5 min after a sender halt, garbling the peer's NEXT
        // transfer's marker (bench 2026-07-19).
        qCWarning(chunkedarq_js8)
            << "[V3-RX] collect watchdog giving up after"
            << rx.nativeWin.noProgressNacks
            << "no-progress NACKs: peer=" << peer
            << "msgId=" << rx.nativeWin.msgId
            << "chunk=" << rx.nativeWin.chunkId;
        if (rx.nativeWin.collectTimer) {
            rx.nativeWin.collectTimer->stop();
        }
        return false;
    }
    ++rx.nativeWin.noProgressNacks;
    qCWarning(chunkedarq_js8)
        << "[V3-RX] collect timeout: peer=" << peer
        << "msgId=" << rx.nativeWin.msgId
        << "chunk=" << rx.nativeWin.chunkId
        << "missing=" << rx.nativeWin.collector.missingSeqs()
        << "noProgress=" << rx.nativeWin.noProgressNacks;
    tryNack(peer, rx.nativeWin.chunkId);
    // Window stays open; the sender's ACK timeout drives the
    // retransmit. Re-arm so persistent gaps keep re-NACKing (the
    // per-peer NACK rate limit bounds the airtime cost; any accepted
    // frame resets noProgressNacks).
    if (rx.nativeWin.collectTimer) {
        rx.nativeWin.collectTimer->start(
            (rx.nativeWin.collector.frameCount() + 2) * m_nativeFrameMs);
    }
    return true;
}

void Manager::clearNativeState(RxState &rx) {
    if (rx.nativeWin.collectTimer) {
        rx.nativeWin.collectTimer->stop();
        rx.nativeWin.collectTimer->deleteLater();
        rx.nativeWin.collectTimer = nullptr;
    }
    rx.nativeWin.active = false;
    rx.binaryAssemblies.clear();
    rx.binaryTotalBytes.clear();
    rx.binaryChunkBytes.clear();
}

SendState &Manager::getOrCreateSend(QString const &peer) {
    return m_sends[peer];
}

RxState &Manager::getOrCreateRx(QString const &peer) {
    return m_recv[peer];
}


// [BUILD 341 policyGate] See header. ONE normalization pipeline, then
// policy tables. History: eligibility used to ride packDirectedMessage
// plus per-specimen regex patches — two classifiers that could never
// agree (case, unpackable arg shapes, pack-failure fallthrough,
// FROM-prefix paste-backs). Nine dev builds of whack-a-mole
// (2026-07-17) until this rewrite; do not reintroduce packing here.
QString leadingPeerOf(QString const &boxText) {
    QString u = boxText.trimmed().toUpper().simplified();
    static QRegularExpression const kFromPrefixRe(
        QStringLiteral(R"(^[A-Z0-9/]+: )"));
    if (auto const m = kFromPrefixRe.match(u); m.hasMatch()) {
        u = u.mid(m.capturedLength()).trimmed();
    }
    int const sp = u.indexOf(QLatin1Char(' '));
    QString const tok0 = sp > 0 ? u.left(sp) : u;
    if (!tok0.startsWith(QLatin1Char('@')) && Radio::is_callsign(tok0)) {
        return tok0;
    }
    return QString();
}

QString effectivePeer(QString const &selected, QString const &boxText) {
    QString const sel = selected.trimmed().toUpper();
    if (!sel.isEmpty() && !sel.startsWith(QLatin1Char('@')) &&
        Radio::is_callsign(sel)) {
        return sel;
    }
    return leadingPeerOf(boxText);
}

TextClass classifyOutgoingText(QString const &boxText) {
    // 0. The exact TX wire transform: uppercase, whitespace collapse.
    QString u = boxText.trimmed().toUpper().simplified();
    if (u.isEmpty()) {
        return TextClass::FreeText;
    }
    // 1. Pasted display FROM-prefix ("WM8Q: …").
    static QRegularExpression const kFromPrefixRe(
        QStringLiteral(R"(^[A-Z0-9/]+: )"));
    if (auto const m = kFromPrefixRe.match(u); m.hasMatch()) {
        u = u.mid(m.capturedLength()).trimmed();
    }
    // 2. ONE addressee token (@group or callsign). A GROUP addressee
    //    refuses ARQ outright, any body (Andy 2026-07-17: "no ARQ
    //    message can go to a group") — the protocol needs exactly ONE
    //    station to ACK. This subsumes the earlier @group-QUERY rule.
    {
        int const sp = u.indexOf(QLatin1Char(' '));
        QString const tok0 = sp > 0 ? u.left(sp) : u;
        if (tok0.startsWith(QLatin1Char('@'))) {
            return TextClass::DirectedCommand;
        }
        if (Radio::is_callsign(tok0)) {
            u = sp > 0 ? u.mid(sp + 1).trimmed() : QString();
        }
    }
    if (u.isEmpty()) {
        return TextClass::FreeText; // bare addressee = composing state
    }

    // 3. Policy tables (longest token first within each set).
    static QStringList const kArqExempt = {
        QStringLiteral("MSG TO:"), QStringLiteral("MSG"),
        QStringLiteral(">")};
    static QStringList const kCmdAlways = {
        QStringLiteral("QUERY MSGS?"), QStringLiteral("QUERY MSGS"),
        QStringLiteral("QUERY CALL"),  QStringLiteral("QUERY"),
        QStringLiteral("HEARTBEAT SNR"), QStringLiteral("HEARTBEAT"),
        QStringLiteral("HB"), QStringLiteral("HAIL"),
        QStringLiteral("CQ"), QStringLiteral("ACK"),
        QStringLiteral("NACK"), QStringLiteral("CMD"),
        QStringLiteral("INFO"), QStringLiteral("GRID"),
        QStringLiteral("STATUS"), QStringLiteral("HEARING"),
        QStringLiteral("AVHAIL?"), QStringLiteral("TYPING")};
    static QStringList const kCmdIfBare = {
        QStringLiteral("HW CPY?"), QStringLiteral("DIT DIT"),
        QStringLiteral("SNR?"), QStringLiteral("INFO?"),
        QStringLiteral("GRID?"), QStringLiteral("STATUS?"),
        QStringLiteral("HEARING?"), QStringLiteral("AGN?"),
        QStringLiteral("QSL?"), QStringLiteral("QSL"),
        QStringLiteral("YES"), QStringLiteral("NO"),
        QStringLiteral("RR"), QStringLiteral("FB"),
        QStringLiteral("73"), QStringLiteral("SK"),
        QStringLiteral("SNR"), QStringLiteral("?")};

    auto matchTok = [&u](QString const &tok, QString *rest) {
        if (u == tok) {
            rest->clear();
            return true;
        }
        if (u.startsWith(tok + QStringLiteral(" "))) {
            *rest = u.mid(tok.length() + 1).trimmed();
            return true;
        }
        // Colon-terminated tokens ("MSG TO:K1ABC…") and the relay
        // marker (">K1ABC…") glue their argument on with no space.
        if ((tok.endsWith(QLatin1Char(':')) ||
             tok == QStringLiteral(">")) &&
            u.startsWith(tok)) {
            *rest = u.mid(tok.length()).trimmed();
            return true;
        }
        return false;
    };

    QString rest;
    for (QString const &t : kArqExempt) {
        if (matchTok(t, &rest)) {
            return TextClass::ArqExempt;
        }
    }
    for (QString const &t : kCmdAlways) {
        if (matchTok(t, &rest)) {
            return TextClass::DirectedCommand;
        }
    }
    for (QString const &t : kCmdIfBare) {
        if (matchTok(t, &rest)) {
            if (rest.isEmpty()) {
                return TextClass::DirectedCommand;
            }
            if (t == QStringLiteral("SNR")) {
                static QRegularExpression const kSignedNumRe(
                    QStringLiteral(R"(^[+-]?\d+$)"));
                if (kSignedNumRe.match(rest).hasMatch()) {
                    return TextClass::DirectedCommand;
                }
            }
            break; // conversational token + trailing text (#93)
        }
    }
    return TextClass::FreeText;
}

}  // namespace ChunkedArq
