/**
 * @file processBufferedActivity.cpp
 * @brief member function of the UI_Constructor class
 *  processes buffered MSG's in m_messageBuffer
 */

#include "JS8_UI/mainwindow.h"

#include <QRegularExpression>
#include <QSet>
#include <algorithm>

void UI_Constructor::processBufferedActivity() {
    if (m_messageBuffer.isEmpty())
        return;

    foreach (auto freq, m_messageBuffer.keys()) {
        auto buffer = m_messageBuffer[freq];

        // check to make sure we empty old buffers by getting the latest
        // timestamp and checking to see if it's older than one minute.
        auto dt = DriftingDateTime::currentDateTimeUtc().addDays(-1);
        if (buffer.cmd.utcTimestamp.isValid()) {
            dt = qMax(dt, buffer.cmd.utcTimestamp);
        }
        if (!buffer.compound.isEmpty()) {
            dt = qMax(dt, buffer.compound.last().utcTimestamp);
        }
        if (!buffer.msgs.isEmpty()) {
            dt = qMax(dt, buffer.msgs.last().utcTimestamp);
        }

        // if the buffer has messages older than 1 minute, and we still haven't
        // closed it, let's mark it as the last frame
        if (dt.secsTo(DriftingDateTime::currentDateTimeUtc()) > 60 &&
            !buffer.msgs.isEmpty()) {
            buffer.msgs.last().bits |= Varicode::JS8CallLast;
        }

        // but, if the buffer is older than 1.5 minutes, and we still haven't
        // closed it, just remove it and skip
        if (dt.secsTo(DriftingDateTime::currentDateTimeUtc()) > 90) {
            m_messageBuffer.remove(freq);
            continue;
        }

        // if the buffer has no messages, skip
        if (buffer.msgs.isEmpty()) {
            continue;
        }

        // if the buffered message hasn't seen the last message, skip
        if ((buffer.msgs.last().bits & Varicode::JS8CallLast) !=
            Varicode::JS8CallLast) {
            continue;
        }

        // [SUBSPACE FRAME-ASSEMBLY FIX — BUILD 294]
        // Multi-frame Subspace messages decoded via the async path
        // can arrive in `msgs` out of order (async dispatches via
        // QueuedConnection, frame N+1 may land before frame N if
        // decoded in different sliding-window passes) and duplicated
        // (sliding window catches the same physical frame multiple
        // times). Build 292 attempted to sort by tdrift but that was
        // window-relative and produced wrong order across passes.
        //
        // Build 294 sorts by absPos — the absolute global sample-
        // buffer position where each frame's Costas was found,
        // computed at the L2 callback site as
        // (snapshot_base_pos + ibest). Invariant across decode
        // passes: same physical frame produces the same absPos no
        // matter which pass found it. Sort puts frames in
        // transmission order. Entries with absPos == 0 (standard
        // period-aligned decoder doesn't compute it) keep their
        // relative arrival order via stable_sort and land before
        // any L2 entries; for normal text traffic that's the
        // standard decoder's once-per-period firing so still
        // correct.
        //
        // After sort, dedup by absPos > 0 (drops sliding-window
        // double-decodes of the same frame). Fall back to text-
        // equality dedup for absPos == 0 entries so the build-293
        // safety net stays in place for the standard path.
        std::stable_sort(
            buffer.msgs.begin(), buffer.msgs.end(),
            [](ActivityDetail const &a, ActivityDetail const &b) {
                return a.absPos < b.absPos;
            });
        QList<ActivityDetail> deduped;
        deduped.reserve(buffer.msgs.size());
        for (auto const &part : buffer.msgs) {
            bool isDup = false;
            for (auto const &kept : deduped) {
                if (part.absPos > 0 && kept.absPos == part.absPos) {
                    isDup = true;
                    break;
                }
                if (part.absPos == 0 && kept.text == part.text) {
                    isDup = true;
                    break;
                }
            }
            if (!isDup) deduped.append(part);
        }
        if (deduped.size() != buffer.msgs.size()) {
            qWarning() << "[BUFFERED-ASM] dedup: dropped"
                       << (buffer.msgs.size() - deduped.size())
                       << "duplicate frame(s) at offset" << freq
                       << "(kept" << deduped.size() << "of"
                       << buffer.msgs.size() << ")";
        }

        QString message;
        foreach (auto part, deduped) {
            message.append(part.text);
        }
        message = Varicode::rstrip(message);

        QString checksum;

        bool valid = false;

        if (Varicode::isCommandBuffered(buffer.cmd.cmd)) {
            int checksumSize = Varicode::isCommandChecksumed(buffer.cmd.cmd);

            // [ARQ-MARKER RX CHECKSUM SKIP 2026-06-10 build 242]
            // Symmetric partner to the sender-side skip in
            // Varicode.cpp::packMessage (skipArqMarkerChecksum). When a
            // buffered cmd (MSG, MSG TO:, QUERY, etc.) carries an
            // ARQ-chunk payload, the body ends with the chunked-DATA
            // marker "#NN.CC/TT.HHHH" — the trailing 4 hex chars are
            // the per-chunk CRC, NOT a JS8 cmd-level checksum. The
            // sender does not add a cmd checksum in this case, so the
            // receiver must not look for one — otherwise the strip-3
            // / strip-6 logic below shaves off the marker tail and
            // the bogus "checksum" never matches. Bypass the cmd
            // checksum check entirely; the ChunkedArq intercept (run
            // earlier in processCommandActivity) does its own CRC
            // verification on the assembled chunk body.
            static QRegularExpression const arqMarkerRe(
                QStringLiteral("#\\d{2}\\.\\d{2}/\\d{2}\\.[0-9A-F]{4}\\s*$"));
            bool const isArqChunk =
                arqMarkerRe.match(message).hasMatch();

            // [QUERY-ARQ SYMMETRIC RX SKIP 2026-06-11 build 248]
            // Symmetric partner to the TX-side skip added in Build 239
            // (Varicode.cpp:2362). When the sender skips the 16-bit
            // checksum for a 3-char "ARQ" / 4-char "ARQ?" body in a
            // QUERY cmd, the wire arrives without a checksum suffix.
            // The standard strip-last-3-as-checksum logic below would
            // chop "RQ?" off and fail validation against an empty
            // body, silently discarding the capability probe. This
            // branch accepts the bare "ARQ" / "ARQ?" body without
            // checksum (handler at processCommandActivity.cpp:1140
            // doesn't need it — the cmd carries the semantic). Old
            // pre-Build-239 senders that still attach a checksum fall
            // through to the standard branch unchanged.
            QString const trimmedMsg = message.trimmed();
            bool const isQueryArqProbe =
                buffer.cmd.cmd.compare(QStringLiteral(" QUERY"),
                                       Qt::CaseInsensitive) == 0 &&
                (trimmedMsg.compare(QStringLiteral("ARQ"),
                                    Qt::CaseInsensitive) == 0 ||
                 trimmedMsg.compare(QStringLiteral("ARQ?"),
                                    Qt::CaseInsensitive) == 0);

            if (isArqChunk) {
                valid = true;
            } else if (isQueryArqProbe) {
                // Accept without checksum; normalize the message to
                // the trimmed body so the handler's d.text.split(" ")
                // sees a clean leading token.
                message = trimmedMsg;
                valid = true;
            } else if (checksumSize == 32) {
                message = Varicode::lstrip(message);
                checksum = message.right(6);
                message = message.left(message.length() - 7);
                valid = Varicode::checksum32Valid(checksum, message);
            } else if (checksumSize == 16) {
                message = Varicode::lstrip(message);
                checksum = message.right(3);
                message = message.left(message.length() - 4);
                valid = Varicode::checksum16Valid(checksum, message);
            } else if (checksumSize == 0) {
                valid = true;
            }
        } else {
            valid = true;
        }

        if (valid) {
            // [BUFFERED-DIAG 2026-06-10 build 244] confirm relay-cmd
            // validation path so we know whether the chunked-ARQ
            // injection is reaching m_rxCommandQueue.
            if (buffer.cmd.cmd == QStringLiteral(">")) {
                qWarning() << "[BUFFERED] relay-cmd valid → queue:"
                           << "from=" << buffer.cmd.from
                           << "to=" << buffer.cmd.to
                           << "text=" << message
                           << "checksum=" << checksum;
            }
            buffer.cmd.bits |= Varicode::JS8CallLast;
            buffer.cmd.text = message;
            buffer.cmd.isBuffered = true;
            m_rxCommandQueue.append(buffer.cmd);
        } else {
            // Promote to qWarning for the relay-cmd case so the log
            // surfaces it directly during ARQ-relay testing.
            if (buffer.cmd.cmd == QStringLiteral(">")) {
                qWarning() << "[BUFFERED] relay-cmd FAILED checksum:"
                           << "checksum=" << checksum
                           << "message=" << message;
            } else {
                qCDebug(mainwindow_js8)
                    << "Buffered message failed checksum...discarding";
                qCDebug(mainwindow_js8) << "Checksum:" << checksum;
                qCDebug(mainwindow_js8) << "Message:" << message;
            }
        }

        // regardless of valid or not, remove the "complete" buffered message
        // from the buffer cache
        m_messageBuffer.remove(freq);
        m_lastClosedMessageBufferOffset = freq;
    }
}
