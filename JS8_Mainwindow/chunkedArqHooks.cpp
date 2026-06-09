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

void UI_Constructor::onChunkedMsgDelivered(QString const &peer,
                                           QString const &addressee,
                                           QString const &body,
                                           int            msgId) {
    // Route a successfully-delivered ARQ MSG body to the local inbox.
    // Mirrors the construction at processCommandActivity.cpp:641-655
    // for the existing "MSG TO:" handler — same CommandDetail shape,
    // same addCommandToMyInbox call.
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] msgDelivered → inbox: peer=" << peer
        << "addressee=" << addressee << "msgId=" << msgId
        << "bodyChars=" << body.size();

    CommandDetail cd = {};
    cd.cmd  = QStringLiteral(" MSG ");
    cd.from = m_baseCall;                  // we are the sender
    cd.to   = addressee.isEmpty() ? m_baseCall : addressee;
    cd.text = body.trimmed();
    cd.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
    cd.submode      = m_nSubMode;
    cd.offset       = freq();
    cd.dial         = m_freqNominal;
    cd.snr          = 0;
    cd.tdrift       = 0;
    addCommandToMyInbox(cd);
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

    // Addressee resolution: for "MSG TO: <addr>" use <addr>; for bare
    // MSG (no addressee field) default to our own callsign — the
    // sender meant the message for us directly.
    QString const to = addressee.isEmpty() ? m_baseCall : addressee;

    CommandDetail cd = {};
    cd.cmd          = QStringLiteral(" MSG ");
    cd.from         = fromCall;
    cd.to           = to;
    cd.text         = body.trimmed();
    cd.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
    cd.submode      = m_nSubMode;
    cd.offset       = freq();
    cd.dial         = m_freqNominal;
    cd.snr          = 0;
    cd.tdrift       = 0;
    addCommandToMyInbox(cd);

    // Operator-visible notification (matches the existing single-frame
    // MSG-TO handler at processCommandActivity.cpp:660 — `tryNotify(
    // "inbox", ...)`).
    tryNotify(QStringLiteral("inbox"), m_nSubMode);

    auto *box = new QMessageBox(QMessageBox::Information,
                                tr("ARQ super-message saved to inbox"),
                                tr("From: %1\n"
                                   "Addressee: %2\n"
                                   "Super-message #%3\n\n"
                                   "Reassembled %4 chars; saved to inbox.")
                                    .arg(fromCall)
                                    .arg(to)
                                    .arg(msgId)
                                    .arg(body.trimmed().size()),
                                QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}
