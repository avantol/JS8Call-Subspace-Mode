/**
 * @file chunkedArqHooks.cpp
 * @brief UI_Constructor slots that bridge ChunkedArq::Manager into
 *        the rest of JS8Call (TX enqueue path and conversation-panel
 *        display).
 *
 * The Manager itself lives in JS8_Main/ChunkedArq.{cpp,h} and owns
 * all per-peer state. The hooks here only translate between
 * Manager signals/slots and JS8Call's existing send-message and
 * display-text functions.
 *
 * Per the approved plan
 * (/home/john/.claude/plans/functional-swimming-avalanche.md):
 *   - wantToTransmit → enqueueMessage(PriorityHigh, ...) — same path
 *     the TCP API's TX.SEND_MESSAGE uses; inherits queue + autoreply
 *     + rig-control plumbing.
 *   - chunkAdded → displayTextForFreq with "<body> (CC/TT)" formatted
 *     line and NO diamond — progressive display per chunk.
 *   - messageDelivered → displayTextForFreq with assembled body + ♢
 *     end-of-message marker — single clean summary line on completion.
 *   - sendProgress / sendComplete / sendFailed are TCP API push
 *     events (TODO: emit via MessageServer once those API push
 *     formats are wired in networkMessage.cpp).
 */

#include "JS8_UI/mainwindow.h"

#include <QDateTime>
#include <QMessageBox>

#include "JS8_Main/ChunkedArq.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Main/FileTransfer.h"

#include <QDir>
#include <QStandardPaths>

namespace {

// JS8 end-of-message marker character (U+2666 BLACK DIAMOND SUIT).
// Reserved here for the final assembled-message summary; per-chunk
// progressive lines deliberately omit it so the operator sees the
// "more coming" signal.
QChar const kJs8DiamondMarker(0x2666);

}  // namespace

void UI_Constructor::onChunkedWantToTransmit(QString const &text) {
    // ChunkedArq has a chunk/ACK/NACK ready to TX. We do the minimum
    // directly here and bypass enqueueMessage / processTxQueue:
    //
    //   - For chunk 1 (user clicked Send) the start button is already
    //     CHECKED, so processTxQueue's toggleTx(true) was a no-op and
    //     never re-entered startTx — m_nextFreeTextMsg would have
    //     stayed empty without an explicit prepareNextMessageFrame.
    //
    //   - For chunks 2..N (driven by ACK → sendNextChunk), the start
    //     button is UNCHECKED (chunk 1's stopTx → resetMessage cleared
    //     it). Now toggleTx(true) inside processTxQueue calls
    //     setChecked(true), which fires on_startTxButton_toggled(true)
    //     → INNER startTx() → prepareNextMessageFrame succeeds and
    //     sets m_nextFreeTextMsg. Then control returns to us and our
    //     own explicit prepareNextMessageFrame runs against an empty
    //     m_txFrameQueue (already drained by the inner call), pops an
    //     empty frame, and CLEARS m_nextFreeTextMsg as the
    //     "no frame to send" exit. Chunk 2 dies on the spot.
    //
    // Direct path avoids both quirks: write the chunk into the widget,
    // run prepareNextMessageFrame ONCE, open the m_auto gate, and
    // ensure the start button reflects the in-flight state without
    // re-entering startTx (QSignalBlocker on setChecked).
    if (!ui->extFreeTextMsgEdit) {
        return;
    }
    addMessageText(text, /*clear=*/true);
    if (!ui->startTxButton->isChecked()) {
        QSignalBlocker const block(ui->startTxButton);
        ui->startTxButton->setChecked(true);
    }
    if (!prepareNextMessageFrame()) {
        qCWarning(chunkedarq_js8)
            << "[ARQ] onChunkedWantToTransmit: prepareNextMessageFrame "
               "returned false; chunk stranded:"
            << text.left(40);
        return;
    }
    // Open the guiUpdate gate (mainwindow.cpp:2987 — `m_transmitting
    // || m_auto || m_tune`). Without this prepareSending is never
    // called and PTT never fires.
    if (!m_auto) {
        auto_tx_mode(true);
    }
}

void UI_Constructor::onChunkedWantsResponseTx(QString const &text) {
    // [TODO.md #57 build 268] Before sending an ACK / NACK, snapshot
    // any draft text the operator has typed into the outgoing-msg
    // widget. stopTx() restores it once the response TX completes.
    // If a prior restore is still pending (back-to-back ACK / NACK
    // events), do NOT overwrite the saved text — the existing snapshot
    // is the genuine operator draft we still owe a restore for. The
    // outgoing-box content at this moment is the previous response's
    // wire text, which we'd be re-saving and then over-restoring on
    // the next stopTx, producing a stale ACK / NACK line back in the
    // box.
    if (ui->extFreeTextMsgEdit && !m_arqResponseRestorePending) {
        m_arqResponseSavedText = ui->extFreeTextMsgEdit->toPlainText();
        m_arqResponseRestorePending = true;
        qCWarning(chunkedarq_js8)
            << "[ARQ-RX] outgoing-text save: chars="
            << m_arqResponseSavedText.size()
            << "for response=" << text.left(40);
    }
    // Delegate to the existing chunk-emit path. addMessageText(/*clear=*/true)
    // inside that slot replaces the widget contents with the response
    // wire text; the restore later in stopTx swaps the operator's
    // draft back in.
    onChunkedWantToTransmit(text);
}

void UI_Constructor::onChunkedChunkAdded(QString const &fromCall,
                                         QString const &chunkBody,
                                         int            chunkId,
                                         int            total) {
    // No UI write — per-chunk "(CC/TT)" progressive display was
    // backed out 2026-06-04 (the typeahead path paints body fragments
    // before we ever see the marker, so any "clean" per-chunk line
    // ends up alongside the raw typeahead text rather than replacing
    // it). Operator readability comes from the final " ♦ " summary
    // in messageDelivered. This signal still fires for TCP API push
    // (RX.CHUNKED_PROGRESS etc.); just no longer drives the panel.
    qCDebug(chunkedarq_js8)
        << "[ARQ-RX] chunkAdded from=" << fromCall
        << "chunk=" << chunkId << "/" << total
        << "bodyLen=" << chunkBody.size();
}

void UI_Constructor::onChunkedMessageDelivered(QString const &fromCall,
                                               QString const &toCall,
                                               QString const &assembledBody,
                                               int            msgId) {
    // Multi-chunk message fully assembled. The per-chunk (CC/TT)
    // lines already show the operator what arrived; we still write
    // ONE final " assembled-body ♦" line so the complete message
    // appears as a single coherent block (useful for QSO log scroll-
    // back). Diamond uses U+2666 (black diamond) to distinguish from
    // the standard JS8 ♢ end-of-frame marker.
    QString const line = QStringLiteral("%1: %2 %3")
                             .arg(fromCall, assembledBody)
                             .arg(kJs8DiamondMarker);
    auto const now = DriftingDateTime::currentDateTimeUtc();
    displayTextForFreq(line, freq(), now,
                       /*isTx=*/false,
                       /*isNewLine=*/true,
                       /*isLast=*/true,
                       m_nSubMode);

    // TCP API push: full inbound reliable message delivered.
    sendNetworkMessage("RX.CHUNKED_DELIVERED", assembledBody,
                       {
                           {"FROM",   fromCall},
                           {"TO",     toCall},
                           {"MSG_ID", msgId},
                       });
}

void UI_Constructor::onChunkedSendProgress(QString const &peer, int msgId,
                                           int delivered, int total) {
    // TCP API push: per-ACK outbound progress. Lets external clients
    // (Python prototype, JS8 Spotter, custom bots) render a chunked-
    // send progress bar without having to model the protocol
    // themselves.
    qCDebug(chunkedarq_js8)
        << "[ARQ-TX] sendProgress peer=" << peer << "msgId=" << msgId
        << "delivered=" << delivered << "of" << total;
    sendNetworkMessage("TX.CHUNKED_PROGRESS", "",
                       {
                           {"PEER",      peer},
                           {"MSG_ID",    msgId},
                           {"DELIVERED", delivered},
                           {"TOTAL",     total},
                       });
}

void UI_Constructor::onChunkedSendComplete(QString const &peer, int msgId,
                                           int total, int totalRetries) {
    // TCP API push: outbound chunked send finished cleanly.
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] sendComplete peer=" << peer << "msgId=" << msgId
        << "total=" << total << "totalRetries=" << totalRetries;

    // [FILE-XFER build 282] If this msgId matches the file-send we
    // auto-enabled ARQ for, restore ARQ to its prior state.
    if (m_fileSendMsgId != 0 && msgId == m_fileSendMsgId) {
        m_fileSendMsgId = 0;
        if (ui->actionModeReplicatorProtocol &&
            ui->actionModeReplicatorProtocol->isChecked() !=
                m_arqStateBeforeFileSend) {
            ui->actionModeReplicatorProtocol->setChecked(
                m_arqStateBeforeFileSend);
            qCWarning(chunkedarq_js8)
                << "[FT-TX] file send delivered; ARQ restored to"
                << (m_arqStateBeforeFileSend ? "ON" : "OFF");
        }
    }
    // [BUILD 303] ARQ is opt-in per super-message. Disable on EVERY
    // sendComplete regardless of how the message was initiated (menu
    // action, plain Send, TCP API, etc.). The prior build-298 gate on
    // m_arqTextSendMsgId only fired for menu-initiated sends, leaving
    // plain-Send ARQ sessions latched. m_arqTextSendMsgId is still
    // cleared if set so the sentinel doesn't dangle.
    m_arqTextSendMsgId = 0;
    if (ui->actionModeReplicatorProtocol &&
        ui->actionModeReplicatorProtocol->isChecked()) {
        ui->actionModeReplicatorProtocol->setChecked(false);
        qCWarning(chunkedarq_js8)
            << "[ARQ] super-message delivered (msgId="
            << msgId << "); ARQ auto-disabled";
    }
    sendNetworkMessage("TX.CHUNKED_COMPLETE", "",
                       {
                           {"PEER",          peer},
                           {"MSG_ID",        msgId},
                           {"TOTAL",         total},
                           {"TOTAL_RETRIES", totalRetries},
                       });

    // Modeless super-message-success dialog. Parented to the main
    // window so it inherits z-order; WA_DeleteOnClose plus show()
    // (NOT exec()) keeps it non-blocking — operator can keep typing /
    // hitting Send while it's up. All N delivered, so successful =
    // total.
    auto *box = new QMessageBox(QMessageBox::Information,
                                tr("ARQ super-message delivered"),
                                tr("To: %1\n"
                                   "Super-message #%2\n\n"
                                   "Sub-messages: %3 of %3 delivered\n"
                                   "Total retries: %4")
                                    .arg(peer)
                                    .arg(msgId)
                                    .arg(total)
                                    .arg(totalRetries),
                                QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}

void UI_Constructor::onChunkedSendFailed(QString const &peer, int msgId,
                                         int delivered, int total,
                                         int totalRetries,
                                         QString const &reason) {
    // TCP API push: outbound chunked send gave up (timeout, too_long,
    // busy, etc.). Reason is the same short token used in
    // ChunkedArq::Manager::sendFailed.
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] sendFailed peer=" << peer << "msgId=" << msgId
        << "delivered=" << delivered << "of" << total
        << "totalRetries=" << totalRetries << "reason=" << reason;

    // [FILE-XFER build 282] Mirror of the sendComplete restore. The
    // file send didn't make it, but ARQ still needs to go back to
    // its pre-file state so the operator's UI returns to normal.
    if (m_fileSendMsgId != 0 && msgId == m_fileSendMsgId) {
        m_fileSendMsgId = 0;
        if (ui->actionModeReplicatorProtocol &&
            ui->actionModeReplicatorProtocol->isChecked() !=
                m_arqStateBeforeFileSend) {
            ui->actionModeReplicatorProtocol->setChecked(
                m_arqStateBeforeFileSend);
            qCWarning(chunkedarq_js8)
                << "[FT-TX] file send failed; ARQ restored to"
                << (m_arqStateBeforeFileSend ? "ON" : "OFF");
        }
    }
    // [BUILD 303] Mirror of sendComplete. Disable ARQ on EVERY failure
    // regardless of initiation path. ARQ is opt-in per-message.
    m_arqTextSendMsgId = 0;
    if (ui->actionModeReplicatorProtocol &&
        ui->actionModeReplicatorProtocol->isChecked()) {
        ui->actionModeReplicatorProtocol->setChecked(false);
        qCWarning(chunkedarq_js8)
            << "[ARQ] super-message failed (msgId=" << msgId
            << "reason=" << reason << "); ARQ auto-disabled";
    }
    sendNetworkMessage("TX.CHUNKED_FAILED", reason,
                       {
                           {"PEER",          peer},
                           {"MSG_ID",        msgId},
                           {"DELIVERED",     delivered},
                           {"TOTAL",         total},
                           {"TOTAL_RETRIES", totalRetries},
                           {"REASON",        reason},
                       });

    // Modeless super-message-failure dialog. Same pattern as the
    // success path; Warning icon distinguishes at a glance.
    QString body;
    if (total <= 0) {
        // Pre-flight rejection (busy / too_long) — no chunks were
        // ever in flight. Show just the reason.
        body = tr("To: %1\n"
                  "Super-message #%2 rejected before send.\n\n"
                  "Reason: %3")
                   .arg(peer)
                   .arg(msgId)
                   .arg(reason);
    } else {
        body = tr("To: %1\n"
                  "Super-message #%2\n\n"
                  "Sub-messages: %3 of %4 delivered\n"
                  "Total retries: %5\n"
                  "Reason: %6")
                   .arg(peer)
                   .arg(msgId)
                   .arg(delivered)
                   .arg(total)
                   .arg(totalRetries)
                   .arg(reason);
    }
    auto *box = new QMessageBox(QMessageBox::Warning,
                                tr("ARQ super-message failed"),
                                body,
                                QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}

// [TODO #51 2026-06-10 build 235]
// Restore the original outbound body to the outgoing-text widget on
// non-success terminal paths (halted, nack_exhausted, timeout_exhausted,
// too_long, busy). Operator sees their text reappear so they can edit
// and retry. The status-bar tip names the failure reason briefly.
//
// Fires AFTER onChunkedSendFailed in normal Qt signal ordering (both
// signals come from the same Manager call). The failure dialog from
// onChunkedSendFailed and the restored text from this slot do not
// race — they happen in deterministic sequence on the GUI thread.
void UI_Constructor::onChunkedSendRestoreRequested(QString const &body,
                                                   QString const &reason) {
    if (!ui->extFreeTextMsgEdit) {
        return;
    }
    if (body.isEmpty()) {
        return;
    }
    // [FILE-XFER 2026-06-16] Suppress restore for file-transfer
    // super-messages. The body is base32 gibberish ("F/V1 GZIP/
    // BASE32 .ABCD...") with no operator value — pasting it back
    // into the outgoing box just creates a mess for them to clear
    // before composing their next message. Halt / timeout / fail /
    // complete all route through this slot; for files we drop all
    // four restore paths uniformly.
    if (body.startsWith(QString::fromLatin1(FileTransfer::PREFIX_V1))) {
        qCWarning(chunkedarq_js8)
            << "[FT-TX] suppressing outgoing-text restore for file "
               "transfer (reason=" << reason << ")";
        return;
    }
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] restoring outgoing text after fail reason="
        << reason << "chars=" << body.size();
    // Replace whatever is currently in the box (probably a partially-
    // sent chunk's wrapped text) with the original super-msg text.
    ui->extFreeTextMsgEdit->blockSignals(true);
    ui->extFreeTextMsgEdit->setPlainText(body);
    ui->extFreeTextMsgEdit->blockSignals(false);
    // Brief status hint so the operator understands why the text
    // reappeared. Translatable; reason token is for the log only.
    QString const tip = tr("ARQ send failed — text restored for retry");
    statusBar()->showMessage(tip, 5000);
}

void UI_Constructor::onChunkedMsgDelivered(QString const &peer,
                                           QString const &addressee,
                                           QString const &body,
                                           int            msgId) {
    // [SENDER-SIDE INBOX FIX 2026-06-12 build 261]
    // Previously this slot called addCommandToMyInbox on the SENDER,
    // depositing a copy of every successfully-delivered ARQ MSG into
    // the operator's own inbox. That mirrored neither the on-air bare
    // MSG path (no sender-side auto-copy at all) nor the on-air
    // MSG TO: path (stores via addCommandToStorage("STORE") on the
    // RECEIVER's machine, not on the sender's). Per operator 2026-06-12:
    // "when i'm sending a message with the MSG command, a copy goes
    // into my inbox, not the desired action."
    //
    // Drop the local deposit. The operator already sees:
    //   • Standard outbound TX rendering in the conversation panel
    //   • onChunkedSendComplete's success dialog confirming delivery
    //   • Status-bar ARQ progress while in flight
    //
    // The receiver-side hook (onChunkedInboxMessageReceived) still
    // routes the body to the appropriate inbox slot on the peer's
    // machine.
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] msgDelivered: peer=" << peer
        << "addressee=" << addressee << "msgId=" << msgId
        << "bodyChars=" << body.size()
        << "(local-inbox deposit removed Build 261)";
}

void UI_Constructor::onChunkedInboxMessageReceived(QString const &fromCall,
                                                   QString const &addressee,
                                                   QString const &body,
                                                   int            msgId) {
    // We just RECEIVED a fully-assembled ARQ super-message whose first
    // chunk was tagged with a JS8 MSG directive. Mirror the inbox-store
    // pattern at processCommandActivity.cpp:641-655 (the existing
    // single-frame MSG TO: handler) so the operator's inbox shows this
    // exactly like a short-form MSG TO: arrival. Then pop a modeless
    // dialog confirming the inbox deposit — parallels the sender-side
    // success dialog in onChunkedSendComplete.
    qCWarning(chunkedarq_js8)
        << "[ARQ-RX] inboxMessageReceived → inbox: from=" << fromCall
        << "addressee=" << addressee << "msgId=" << msgId
        << "bodyChars=" << body.size();

    // [MSG TO: VIA ARQ 2026-06-12 build 255]
    // Distinguish bare MSG vs MSG TO: <addr> and route accordingly,
    // mirroring the on-air handler at processCommandActivity.cpp:675-
    // 768. Two routing paths and a prefix-strip step:
    //
    //   • Bare MSG (addressee empty) → cd.to = m_baseCall,
    //     addCommandToMyInbox(cd) (our UNREAD slot).
    //   • MSG TO: <addr> where addr == us → same as bare MSG
    //     (we're the addressee, so it lands in our UNREAD).
    //   • MSG TO: <addr> where addr is someone else → cd.to =
    //     base_callsign(addr), addCommandToStorage("STORE", cd) so
    //     the addressee can later QUERY-retrieve. Gated on
    //     !m_config.relay_off() (don't store-and-forward if relaying
    //     is off).
    //
    // Also: strip the "MSG TO: <addr>" or "MSG " prefix from the
    // assembled body before storing — cd.text should contain only the
    // actual message content. The on-air pattern at
    // processCommandActivity.cpp:683-686 strips the addressee from
    // d.text; ARQ side now does the equivalent on the assembled body.
    QString const baseAddr = addressee.isEmpty()
                                 ? QString()
                                 : Radio::base_callsign(addressee);
    bool const addressedToUs =
        baseAddr.isEmpty() || baseAddr == m_baseCall;
    QString const to = addressedToUs ? m_baseCall : baseAddr;

    // Strip the leading "MSG TO: <addr>" or "MSG " prefix from body.
    static QRegularExpression const msgToPrefixRe(
        QStringLiteral(R"(^\s*MSG\s+TO\s*:\s*[A-Z0-9/@]+\s*)"),
        QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression const msgPrefixRe(
        QStringLiteral(R"(^\s*MSG\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    QString cleanText = body.trimmed();
    if (auto const m = msgToPrefixRe.match(cleanText); m.hasMatch()) {
        cleanText = cleanText.mid(m.capturedLength()).trimmed();
    } else if (auto const m2 = msgPrefixRe.match(cleanText); m2.hasMatch()) {
        cleanText = cleanText.mid(m2.capturedLength()).trimmed();
    }

    CommandDetail cd = {};
    cd.cmd          = addressee.isEmpty() ? QStringLiteral(" MSG ")
                                          : QStringLiteral(" MSG TO:");
    cd.from         = fromCall;
    cd.to           = to;
    cd.text         = cleanText;
    cd.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
    cd.submode      = m_nSubMode;
    cd.offset       = freq();
    cd.dial         = m_freqNominal;
    cd.snr          = 0;
    cd.tdrift       = 0;
    // [MSG TO: FIELD COVERAGE 2026-06-12 build 259]
    // Populate the same fields the on-air MSG TO: handler at
    // processCommandActivity.cpp:749-763 sets, so the inbox display
    // has every value the on-air store would have produced. Previously
    // these were left at their default-init values (0/""), which
    // matches the storage-time params for FROM but apparently breaks
    // some downstream display logic per operator's report that the
    // FROM column appears empty for ARQ-deposited entries.
    cd.bits         = Varicode::JS8CallLast;
    cd.relayPath    = fromCall;  // single-hop: relay-path = sender
    // (extra and grid stay empty — they come from on-air wire
    //  metadata that ARQ chunks don't carry forward)

    QString storageType;
    if (addressedToUs) {
        // Land in our own UNREAD inbox.
        addCommandToMyInbox(cd);
        storageType = QStringLiteral("UNREAD");
    } else {
        // Store-and-forward for the third-party addressee, but only if
        // relaying is enabled (matches the on-air gate at
        // processCommandActivity.cpp:742).
        if (m_config.relay_off()) {
            qCWarning(chunkedarq_js8)
                << "[ARQ-RX] MSG TO: addressed to" << baseAddr
                << "but relay_off=true — DISCARDED, not stored";
            return;
        }
        addCommandToStorage(QStringLiteral("STORE"), cd);
        storageType = QStringLiteral("STORE");
    }

    qCWarning(chunkedarq_js8)
        << "[ARQ-RX] MSG-via-ARQ stored: type=" << storageType
        << "cd.from=" << cd.from << "cd.to=" << cd.to
        << "cd.cmd=" << cd.cmd
        << "cd.text(first40)=" << cd.text.left(40)
        << "cleanTextChars=" << cleanText.size();

    // Operator-visible notification (matches the existing single-frame
    // MSG-TO handler at processCommandActivity.cpp:660 — `tryNotify(
    // "inbox", ...)`).
    tryNotify(QStringLiteral("inbox"), m_nSubMode);

    auto *box = new QMessageBox(QMessageBox::Information,
                                tr("ARQ super-message saved to inbox"),
                                tr("From: %1\n"
                                   "Addressee: %2\n"
                                   "Storage: %3\n"
                                   "Super-message #%4\n\n"
                                   "Reassembled %5 chars; saved to %6.")
                                    .arg(fromCall)
                                    .arg(to)
                                    .arg(storageType)
                                    .arg(msgId)
                                    .arg(cleanText.size())
                                    .arg(storageType.toLower()),
                                QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}

void UI_Constructor::onChunkedProgressUpdate(int chunkId, int total,
                                             int retries) {
    // [STATUS-BAR ARQ PROGRESS 2026-06-11 build 252]
    // Override the leftmost status-bar widget (last_tx_label) with
    // "ARQ: #x/y (z repeats)". On the FIRST update of a session, cache
    // whatever was there so onChunkedProgressEnd can restore it.
    // Refreshed on each chunk-send / retransmit.
    if (!m_lastTxLabelCacheValid) {
        m_lastTxLabelCache = last_tx_label.text();
        m_lastTxLabelCacheValid = true;
    }
    last_tx_label.setText(QString("ARQ: #%1/%2 (%3 repeats)")
                              .arg(chunkId)
                              .arg(total)
                              .arg(retries));
}

void UI_Constructor::onChunkedProgressEnd() {
    if (m_lastTxLabelCacheValid) {
        last_tx_label.setText(m_lastTxLabelCache);
        m_lastTxLabelCache.clear();
        m_lastTxLabelCacheValid = false;
    }
}

void UI_Constructor::onChunkedRelayMessageReceived(QString const &fromCall,
                                                   QString const &body,
                                                   int            msgId) {
    // [RELAY-VIA-ARQ RX hook 2026-06-10 build 243]
    // Assembled super-msg body shape: "<targetCall>> <inner text> <CRC>"
    // where <CRC> is the 3-char 16-bit checksum the sender computed
    // over <inner text>. Populate m_messageBuffer to look like a
    // freshly-arrived multi-frame on-air ">" cmd; processBufferedActivity
    // will validate the checksum, then the existing relay handler at
    // processCommandActivity.cpp:561 fires and TXs the forward.
    //
    // We don't strip the checksum here — processBufferedActivity does
    // that (its message.right(3) / message.left(len-4) logic).
    // [RELAY REGEX 2026-06-10 build 245] Allow no-space form
    // "<via>>rest" (matches the send-side regex in mainwindow.cpp).
    static QRegularExpression const re(
        QStringLiteral(R"(^\s*(?<via>[A-Z0-9/]+)>\s*(?<rest>\S.*)$)"),
        QRegularExpression::CaseInsensitiveOption);
    auto const m = re.match(body);
    if (!m.hasMatch()) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-RX] relay body parse failed — discarding:" << body;
        return;
    }

    QString const via  = m.captured("via").toUpper();
    QString const rest = m.captured("rest").trimmed();

    qCWarning(chunkedarq_js8)
        << "[ARQ-RX] relay → injecting into m_messageBuffer: from="
        << fromCall << "via=" << via << "msgId=" << msgId
        << "innerLen=" << rest.size();

    int const offset  = freq();
    auto const nowUtc = DriftingDateTime::currentDateTimeUtc();

    CommandDetail cd = {};
    cd.from         = fromCall;
    cd.to           = via;
    cd.cmd          = QStringLiteral(">");
    cd.bits         = Varicode::JS8CallFirst;
    cd.utcTimestamp = nowUtc;
    cd.submode      = m_nSubMode;
    cd.offset       = offset;
    cd.dial         = m_freqNominal;
    cd.snr          = 0;
    cd.tdrift       = 0;

    ActivityDetail msg = {};
    msg.text         = rest;                  // "N7W STATUS? XYZ"
    msg.bits         = Varicode::JS8CallLast; // closes the buffer
    msg.utcTimestamp = nowUtc;
    msg.submode      = m_nSubMode;
    msg.offset       = offset;
    msg.dial         = m_freqNominal;
    msg.snr          = 0;
    msg.tdrift       = 0;
    msg.shouldDisplay = false;

    auto &buf = m_messageBuffer[offset];
    buf.cmd = cd;
    buf.msgs.clear();
    buf.msgs.append(msg);
    // processBufferedActivity runs on the next RX-activity tick; it
    // concatenates buffer.msgs into "N7W STATUS? XYZ", strips and
    // validates the 3-char CRC, then appends cd to m_rxCommandQueue.
    // processCommandActivity then fires the ">" handler at
    // processCommandActivity.cpp:561, which TXs the forward via the
    // standard TX queue.
}

void UI_Constructor::onChunkedFileMessageReceived(QString const &fromCall,
                                                  QString const &body,
                                                  int            msgId) {
    // [FILE-XFER 2026-06-16 build 276] RX hook for ARQ file-transfer
    // super-messages. body is the fully-assembled
    // "F/V1 <header-b32> <payload-b32>" wire form. Decode header,
    // confirm with operator, verify SHA-256, write to disk.
    FileTransfer::FileHeader header;
    QString payloadBase32;
    if (!FileTransfer::splitWireBody(body, header, payloadBase32)) {
        qCWarning(chunkedarq_js8)
            << "[FT-RX] body parse failed — peer=" << fromCall
            << "msgId=" << msgId << "bodyLen=" << body.size();
        return;
    }

    qCWarning(chunkedarq_js8)
        << "[FT-RX] header parsed — peer=" << fromCall
        << "msgId=" << msgId
        << "name=" << header.name
        << "bytes=" << header.bytes;

    // [build 277 2026-06-16] Modeless accept dialog. The Build 276
    // version used QMessageBox::question() which is blocking-modal —
    // operator missed it when not actively watching the screen, and
    // it tied up the GUI thread. Now we new up a QMessageBox on the
    // heap, parent it to `this` so it survives but doesn't block,
    // and connect to its finished() signal for the save/discard
    // decision. Operator can keep using the rest of JS8Call while
    // the dialog sits; the dialog stays open until clicked.
    auto const msg = QStringLiteral(
        "Incoming file from %1\n\n"
        "    Name:  %2\n"
        "    Size:  %3 bytes\n\n"
        "Save to disk?").arg(fromCall, header.name).arg(header.bytes);
    auto *box = new QMessageBox(this);
    box->setWindowTitle(QStringLiteral("Incoming file"));
    box->setText(msg);
    box->setIcon(QMessageBox::Question);
    box->setStandardButtons(QMessageBox::Save | QMessageBox::Discard);
    box->setDefaultButton(QMessageBox::Save);
    box->setWindowModality(Qt::NonModal);
    box->setAttribute(Qt::WA_DeleteOnClose);

    // Capture by value — payloadBase32 / fromCall / header / msgId
    // need to survive the dialog's lifetime even if the original
    // signal-emit frame is long gone. Save dir resolved at click
    // time so a Settings change between receive and accept is
    // honored (Phase 2 makes the dir operator-configurable).
    QString const payloadCopy = payloadBase32;
    QString const fromCopy    = fromCall;
    FileTransfer::FileHeader const headerCopy = header;
    int const msgIdCopy = msgId;

    connect(box, &QMessageBox::finished, this,
            [this, box, payloadCopy, fromCopy, headerCopy, msgIdCopy]
            (int result) {
        Q_UNUSED(box);  // WA_DeleteOnClose handles cleanup.
        QMessageBox::StandardButton const choice =
            static_cast<QMessageBox::StandardButton>(result);
        if (choice != QMessageBox::Save) {
            qCWarning(chunkedarq_js8)
                << "[FT-RX] operator declined save — peer=" << fromCopy
                << "msgId=" << msgIdCopy
                << "name=" << headerCopy.name;
            return;
        }

        QString const saveDir = QDir::cleanPath(
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
            + QStringLiteral("/JS8Call-FileTransfer"));
        QString err;
        QString const savedPath = FileTransfer::assembleReceivedFile(
            saveDir, headerCopy, payloadCopy, &err);
        if (savedPath.isEmpty()) {
            qCWarning(chunkedarq_js8)
                << "[FT-RX] assembleReceivedFile failed:" << err
                << "peer=" << fromCopy << "msgId=" << msgIdCopy;
            auto *errBox = new QMessageBox(this);
            errBox->setWindowTitle(QStringLiteral("File transfer failed"));
            errBox->setText(
                QStringLiteral("Could not save received file from %1:\n%2")
                    .arg(fromCopy, err));
            errBox->setIcon(QMessageBox::Warning);
            errBox->setStandardButtons(QMessageBox::Ok);
            errBox->setWindowModality(Qt::NonModal);
            errBox->setAttribute(Qt::WA_DeleteOnClose);
            errBox->show();
            return;
        }

        qCWarning(chunkedarq_js8)
            << "[FT-RX] saved file — peer=" << fromCopy
            << "msgId=" << msgIdCopy << "path=" << savedPath;
        // Surface the save in the conversation panel so it survives
        // panel scrollback (the modeless dialog will be dismissed by
        // the operator at some point).
        auto const summary =
            QStringLiteral("%1: [file received: %2 → %3]")
                .arg(fromCopy, headerCopy.name, savedPath);
        auto const now = DriftingDateTime::currentDateTimeUtc();
        displayTextForFreq(summary, freq(), now,
                           /*isTx=*/false,
                           /*isNewLine=*/true,
                           /*isLast=*/true,
                           m_nSubMode);
    });

    box->show();
    box->raise();
    box->activateWindow();
}
