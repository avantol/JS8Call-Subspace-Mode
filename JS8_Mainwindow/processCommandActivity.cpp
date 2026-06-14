

/** \file
 * @brief member function of the UI_Constructor class
 *  processes JS8 commands
 */

#include "JS8_UI/mainwindow.h"

#include "JS8_Main/ChunkedArq.h"

void UI_Constructor::processCommandActivity() {
#if 0
    if (!m_txFrameQueue.isEmpty()) {
        return;
    }
#endif

    if (m_rxCommandQueue.isEmpty()) {
        return;
    }

#if 0
    bool processed = false;

    int f = currentFreq();
#endif

    auto now = DriftingDateTime::currentDateTimeUtc();

    while (!m_rxCommandQueue.isEmpty()) {
        auto d = m_rxCommandQueue.dequeue();

        auto selectedCallsign = callsignSelected();
        bool isAllCall = isAllCallIncluded(d.to);
        // @APRSIS is bridging traffic, not user-addressed. Don't
        // route to the conversation panel unless the user has
        // explicitly added @APRSIS to their MyGroups. The actual
        // APRS-IS bridging logic (spotAprsCmd / spotAprsGrid / the
        // MSG TO: relay paths later in this file) runs independently
        // of this flag and continues to function regardless.
        bool isGroupCall = isGroupCallIncluded(d.to);

        qCDebug(mainwindow_js8)
            << "try processing command" << d.from << d.to << d.cmd << d.dial
            << d.offset << d.grid << d.extra << isAllCall << isGroupCall;

        // if we need a compound callsign but never got one...skip
        if (d.from == "<....>" || d.to == "<....>") {
            continue;
        }

        // is this to me? Exact match only — WM8Q/P is a different station than WM8Q
        bool toMe = d.to == m_config.my_callsign().trimmed();

        // log call activity BEFORE the command-allowed gate so even
        // unrecognized directed commands refresh the sender's SNR in the
        // callsign list. Gating was rejecting valid SNR updates for
        // any directed message whose cmd wasn't in the known-commands set.
        CallDetail cd = {};
        cd.call = d.from;
        cd.grid = d.grid;
        cd.snr = d.snr;
        cd.dial = d.dial;
        cd.offset = d.offset;
        cd.bits = d.bits;
        cd.ackTimestamp =
            d.text.contains(": ACK") || toMe ? d.utcTimestamp : QDateTime{};
        cd.utcTimestamp = d.utcTimestamp;
        cd.tdrift = d.tdrift;
        cd.submode = d.submode;
        logCallActivity(cd, true);
        logHeardGraph(d.from, d.to);

        // we're only processing a subset of queries at this point
        if (!Varicode::isCommandAllowed(d.cmd)) {
            continue;
        }

        // Placeholder for callbacks to be passed to confirmThenEnqueueMessage
        // and enqueueMessage
        Callback callback = nullptr;

        // PROCESS BUFFERED HEARING FOR EVERYONE
        if (d.cmd == " HEARING") {
            // 1. parse callsigns
            // 2. log it to the heard graph
            auto calls = Varicode::parseCallsigns(d.text);
            foreach (auto call, calls) {
                logHeardGraph(d.from, call);
            }
        }

        // PROCESS BUFFERED GRID FOR EVERYONE
        if (d.cmd == " GRID") {
            // 1. parse grids
            // 2. log it to our call activity
            auto grids = Varicode::parseGrids(d.text);
            foreach (auto grid, grids) {
                CallDetail cd = {};
                cd.bits = d.bits;
                cd.call = d.from;
                cd.dial = d.dial;
                cd.offset = d.offset;
                cd.grid = grid;
                cd.snr = d.snr;
                cd.utcTimestamp = d.utcTimestamp;
                cd.tdrift = d.tdrift;
                cd.submode = d.submode;

                // PROCESS GRID SPOTS TO APRSIS FOR EVERYONE
                /**
                 * APRS relay: inbound `MSG TO:` stores for our local station.
                 * Dedupe within 60s using TO + TEXT + DE <SENDER>.
                 */
                if (d.to == "@APRSIS") {

                    spotAprsGrid(cd.dial, cd.offset, cd.snr, cd.call, cd.grid);
                }

                logCallActivity(cd, true);
            }
        }

        // PROCESS @JS8NET, @APRSIS, AND OTHER GROUP SPOTS FOR EVERYONE
        if (d.to.startsWith("@")) {
            spotCmd(d);
        }

        // PROCESS @APRSIS CMD SPOTS FOR EVERYONE
        if (d.to == "@APRSIS") {
            spotAprsCmd(d);
        }

        // PREPARE CMD TEXT
        // Build baseText without the FROM prefix: "TO CMD EXTRA TEXT [eot]"
        // This is used for the TCP API where FROM is sent as a separate field.
        // The full text (with FROM prefix) is used for file logging and display.
        //
        // Build 152: when d.cmd is the literal-space "freetext follows"
        // marker (directed_cmds[" "] == 31), `d.to + d.cmd` already ends
        // with a trailing space, and the trailing baseList.join(" ") below
        // would add another separator, producing "WM8Q  THIS IS A MSG"
        // (doubled). For whitespace-only cmds, drop d.cmd from the head and
        // let join provide the single separator. Real cmds (" SNR",
        // " ACK", "?", ">", etc.) keep their existing concat — for "?" and
        // ">" the result is still "WM8Q?" / "WM8Q>" with no separator,
        // matching prior behavior. Same pattern as the directed-frame fix
        // in DecodedText::tryUnpackDirected (Build 147) and the
        // heartbeat-alt fix in tryUnpackHeartbeat (Build 148).
        QStringList baseList;
        if (d.cmd.trimmed().isEmpty()) {
            baseList << d.to;
        } else {
            baseList << QString("%1%2").arg(d.to).arg(d.cmd);
        }

        if (!d.extra.isEmpty()) {
            baseList.append(d.extra);
        }

        if (!d.text.isEmpty()) {
            baseList.append(d.text);
        }

        QString baseText = baseList.join(" ");

        // Append the user-configured eot (end of transmission) character
        // if this is the final frame of the message
        bool isLast = (d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast;
        if (isLast) {
            baseText = QString("%1 %2 ")
                           .arg(Varicode::rstrip(baseText))
                           .arg(m_config.eot());
        }

        // Prepend FROM for file logging and UI display: "FROM: TO CMD EXTRA TEXT [eot]"
        QString text = QString("%1: %2").arg(d.from).arg(baseText);

        // Log to DIRECTED.txt (includes FROM prefix)
        writeMsgTxt(text, d.snr, d.offset);

        // Send to TCP API — use full text with FROM prefix for JS8 Spotter compat
        if (canSendNetworkMessage()) {
            sendNetworkMessage(
                "RX.DIRECTED", text,
                {{"_ID", QVariant(-1)},
                 {"FROM", QVariant(d.from)},
                 {"TO", QVariant(d.to)},
                 {"CMD", QVariant(d.cmd)},
                 {"GRID", QVariant(d.grid)},
                 {"EXTRA", QVariant(d.extra)},
                 {"TEXT", QVariant(text)},
                 {"FREQ", QVariant(d.dial + d.offset)},
                 {"DIAL", QVariant(d.dial)},
                 {"OFFSET", QVariant(d.offset)},
                 {"SNR", QVariant(d.snr)},
                 {"SPEED", QVariant(d.submode)},
                 {"TDRIFT", QVariant(d.tdrift)},
                 {"UTC", QVariant(d.utcTimestamp.toMSecsSinceEpoch())}});
        }

        // ChunkedArq intercept — chunked-DATA frames addressed to us,
        // with body matching the wire format "<from>: <to> <body>
        // #NN.CC/TT.HHHH". Route them to the manager (CRC, reassembly,
        // ACK, progressive display); skip the rest of this iteration
        // so the raw marker doesn't leak AND so the standard directed-
        // cmd handler (e.g. MSG TO: → addCommandToMyInbox on chunk 1)
        // doesn't fire prematurely. The receiver-side MSG detection
        // now happens inside ChunkedArq::onChunkReceived on the full
        // assembled body. NOTE: we used to gate this on d.cmd == " "
        // (freetext) but that missed multi-chunk MSG-TO sends because
        // JS8 parses the leading "MSG TO:" of the first chunk's body
        // as a directed cmd. Now we accept any d.cmd as long as the
        // text matches the chunked-DATA wire format — false positives
        // are vanishingly unlikely (parseChunkedData validates msg_id,
        // chunk_id, total, and CRC field placement). Vanilla traffic
        // that doesn't match falls through unchanged.
        if (toMe && m_chunkedArq) {
            ChunkedArq::ParsedChunk parsed;
            if (ChunkedArq::parseChunkedData(text, parsed)) {
                // Auto-enable ARQ on first sight of any chunked-DATA
                // wire frame addressed to us (2026-06-07, build arq-
                // autoEnable).
                if (ui->actionModeReplicatorProtocol &&
                    !ui->actionModeReplicatorProtocol->isChecked()) {
                    qWarning() << "[ARQ-RX] auto-enabling ARQ on first"
                               << "chunked-DATA frame from" << d.from
                               << "msgId=" << parsed.msgId;
                    ui->actionModeReplicatorProtocol->setChecked(true);
                    // The toggle slot will also latch the multi-mode
                    // override (TODO.md #58). Belt-and-suspenders: in
                    // case the button was already on and we're just
                    // arriving here via the chunk-detection path with
                    // no toggle event, latch directly.
                }
                if (!m_arqMultiModeOverride) {
                    m_arqMultiModeOverride = true;
                    qWarning() << "[ARQ-RX] multi-mode RX override latched ON"
                               << "via inbound chunked-DATA from" << d.from;
                }
                // Auto-match submode to sender per the table approved
                // 2026-06-07 (build arq-modeFollow4):
                //
                //   • First chunk OR Subspace involved on either side
                //     → match sender exactly. Subspace↔legacy is a hard
                //     decoder boundary (multi-decoder doesn't bridge it
                //     in the Subspace direction per operator), so the
                //     receiver MUST match the sender's mode to decode
                //     subsequent chunks AND emit ACKs the sender can
                //     decode.
                //   • Subsequent chunks + both-legacy mismatch
                //     → "at minimum sender's speed": only switch DOWN
                //     to sender's mode if our current mode is slower.
                //     If we're already faster (e.g. operator switched
                //     up Normal→Fast mid-session and the sender stayed
                //     on Normal), keep our faster mode — the sender's
                //     decoder can still copy it via multi-decoder.
                //
                // First-chunk heuristic: parsed.chunkId == 1. This also
                // re-fires the exact-match policy on retransmits of
                // chunk 1, which is harmless (idempotent when we're
                // already matched, correct when we're not).
                bool const isFirstChunk = (parsed.chunkId == 1);
                bool const subspaceInvolved =
                    (m_nSubMode == Varicode::JS8CallFT2) ||
                    (d.submode    == Varicode::JS8CallFT2);

                int targetMode = m_nSubMode;
                if (isFirstChunk || subspaceInvolved) {
                    // Cases 1–4, 7, 8 → exact match
                    targetMode = d.submode;
                } else {
                    // Case 9 → "at minimum sender's speed"
                    unsigned const senderPeriod =
                        JS8::Submode::periodMS(d.submode);
                    unsigned const currentPeriod =
                        JS8::Submode::periodMS(m_nSubMode);
                    if (senderPeriod < currentPeriod) {
                        // sender is faster — we'd be slower, match it
                        targetMode = d.submode;
                    }
                    // else keep current (we're already at-or-faster)
                }

                if (targetMode != m_nSubMode) {
                    qWarning() << "[ARQ-RX] auto-mode:"
                               << (isFirstChunk ? "first" : "subseq")
                               << "chunk; was=" << m_nSubMode
                               << "sender=" << d.submode
                               << "→ now=" << targetMode
                               << "(peer=" << d.from
                               << "msgId=" << parsed.msgId
                               << "chunk=" << parsed.chunkId << ")";
                    setSubmode(targetMode);
                }
                m_chunkedArq->onChunkReceived(d.from, parsed);
                continue;
            }
        }

        // ChunkedArq ACK/NACK dispatch — vanilla manual ACKs (no
        // EXTRA payload) still flow through the existing handler
        // below; ARQ ACK/NACK carry a non-empty EXTRA with the chunk
        // ID and we route those to the manager for sender FSM update.
        if (m_chunkedArq && !d.extra.isEmpty()) {
            bool extraOk = false;
            int const seq = d.extra.toInt(&extraOk);
            if (extraOk) {
                if (d.cmd == " ACK") {
                    m_chunkedArq->onAckReceived(d.from, seq);
                    // Fall through — the existing ACK handler below
                    // does notification (tryNotify) and we want that
                    // to fire as the operator-visible cue too.
                } else if (d.cmd == " NACK") {
                    m_chunkedArq->onNackReceived(d.from, seq);
                    // Fall through — operator-visible cue in the
                    // conversation window matters for NACK too;
                    // without the display we only saw NACKs in the
                    // band-activity panel. The dedicated NACK skip
                    // block (mirroring " ACK" at the bottom of the
                    // autoreply if/else chain) prevents NACK — which
                    // sits in autoreply_cmds at slot 2 — from
                    // triggering an unintended auto-response.
                }
            }
        }

        // we're only responding to allcalls if we are participating in the
        // allcall group but, don't avoid for heartbeats...those are technically
        // allcalls but are processed differently
        if (isAllCall && m_config.avoid_allcall() && d.cmd != " CQ" &&
            d.cmd != " HB" && d.cmd != " HEARTBEAT") {
            continue;
        }

        // we're only responding to allcall, groupcalls, and our callsign at
        // this point, so we'll end after logging the callsigns we've heard
        if (!isAllCall && !toMe && !isGroupCall) {
            continue;
        }

        ActivityDetail ad = {};
        ad.isLowConfidence = false;
        ad.isDirected = true;
        ad.bits = d.bits;
        ad.dial = d.dial;
        ad.offset = d.offset;
        ad.snr = d.snr;
        ad.text = text;
        ad.utcTimestamp = d.utcTimestamp;
        ad.submode = d.submode;

        // we'd be double printing here if were on frequency, so let's be
        // "smart" about this...
        bool shouldDisplay = true;

        // don't display ping allcalls
        if (isAllCall && (d.cmd != " " || ad.text.contains("@HB HEARTBEAT"))) {
            shouldDisplay = false;
        }

        if (shouldDisplay) {
            auto c = ui->textEditRX->textCursor();
            c.movePosition(QTextCursor::End);
            ui->textEditRX->setTextCursor(c);

            // ACKs and SNRs are the most likely source of items to be
            // overwritten (multiple responses at once)... so don't overwrite
            // those (i.e., print each on a new line)
            bool shouldOverwrite =
                (!d.cmd.contains(" ACK") &&
                 !d.cmd.contains(" SNR")); /* && isRecentOffset(d.freq);*/

            if (shouldOverwrite &&
                ui->textEditRX->find(d.utcTimestamp.time().toString(),
                                     QTextDocument::FindBackward)) {
                // ... maybe we could delete the last line that had this message
                // on this frequency...
                c = ui->textEditRX->textCursor();
                c.movePosition(QTextCursor::StartOfBlock);
                c.movePosition(QTextCursor::EndOfBlock,
                               QTextCursor::KeepAnchor);
                qCDebug(mainwindow_js8) << "should display directed message, "
                                           "erasing last rx activity line..."
                                        << c.selectedText().toUpper();
                c.removeSelectedText();
                c.deletePreviousChar();
                c.deletePreviousChar();
                /*
                c.deleteChar();
                c.deleteChar();
                */
            }

            // log it to the display!
            displayTextForFreq(ad.text, ad.offset, ad.utcTimestamp, false, true,
                               false, ad.submode);

            /*
            // and send it to the network in case we want to interact with it
            from an external app... if(canSendNetworkMessage()){
                sendNetworkMessage("RX.DIRECTED.ME", ad.text, {
                    {"_ID", QVariant(-1)},
                    {"FROM", QVariant(d.from)},
                    {"TO", QVariant(d.to)},
                    {"CMD", QVariant(d.cmd)},
                    {"GRID", QVariant(d.grid)},
                    {"EXTRA", QVariant(d.extra)},
                    {"TEXT", QVariant(d.text)},
                    {"FREQ", QVariant(ad.dial+ad.offset)},
                    {"DIAL", QVariant(ad.dial)},
                    {"OFFSET", QVariant(ad.offset)},
                    {"SNR", QVariant(ad.snr)},
                    {"SPEED", QVariant(ad.submode)},
                    {"TDRIFT", QVariant(ad.tdrift)},
                    {"UTC", QVariant(ad.utcTimestamp.toMSecsSinceEpoch())}
                });
            }
            */

            if (!isAllCall) {
                // Only cancel CQ loop if message is directed to us
                if (m_cq_loop->isActive() &&
                    d.to == m_config.my_callsign().trimmed()) {
                    qCDebug(mainwindow_js8)
                        << "Canceling CQ loop: directed message to us";
                    m_cq_loop->onLoopCancel();
                }

                // notification for directed message. @APRSIS is group
                // traffic (anyone gateway-relaying to APRS-IS), not
                // anything directed to this operator, so suppress the
                // "directed-msg" alert there. Spotting / display / any
                // auto-reply logic on @APRSIS is unchanged.
                if (d.to != "@APRSIS") {
                    tryNotify("directed", d.submode);
                }
            }
        }

        // we're only responding to callsigns in our whitelist if we have one
        // defined... make sure the whitelist is empty (no restrictions) or the
        // from callsign or its base callsign is on it
        auto whitelist = m_config.auto_whitelist();
        if (!whitelist.isEmpty() &&
            !(whitelist.contains(d.from) ||
              whitelist.contains(Radio::base_callsign(d.from)))) {
            qCDebug(mainwindow_js8)
                << "skipping command for whitelist" << d.from;
            continue;
        }

        // we'll never reply to a blacklisted callsign or base callsign
        auto blacklist = m_config.auto_blacklist();
        if (!blacklist.isEmpty() &&
            (blacklist.contains(d.from) ||
             blacklist.contains(Radio::base_callsign(d.from)))) {
            qCDebug(mainwindow_js8)
                << "skipping command for blacklist" << d.from;
            continue;
        }

        // if this is an allcall, check to make sure we haven't replied to their
        // allcall recently (in the past ten minutes) that way we never get
        // spammed by allcalls at too high of a frequency
        if (isAllCall && m_txAllcallCommandCache.contains(d.from) &&
            m_txAllcallCommandCache[d.from]->secsTo(now) / 60 < 15) {
            qCDebug(mainwindow_js8)
                << "skipping command for allcall timeout" << d.from;
            continue;
        }

        // don't actually process any automatic message replies while in idle
        if (m_tx_watchdog) {
            qCDebug(mainwindow_js8)
                << "skipping command for idle timeout" << d.from;
            continue;
        }

        // HACK: if this is an autoreply cmd and relay path is populated and cmd
        // is not MSG or MSG TO:, then swap out the relay path
        if (Varicode::isCommandAutoreply(d.cmd) && !d.relayPath.isEmpty() &&
            !d.cmd.startsWith(" MSG") && !d.cmd.startsWith(" QUERY")) {
            d.from = d.relayPath;
        }

        // construct a reply, if needed
        QString reply;
        int priority = PriorityNormal;
        int freq = -1;

        // QUERIED SNR
        if (d.cmd == " SNR?" && !isAllCall) {
            reply = QString("%1 SNR %2")
                        .arg(d.from)
                        .arg(Varicode::formatSNR(d.snr));
        }

        // QUERIED INFO
        else if (d.cmd == " INFO?" && !isAllCall) {
            QString info = m_config.my_info();
            if (info.isEmpty()) {
                continue;
            }

            reply = QString("%1 INFO %2")
                        .arg(d.from)
                        .arg(replaceMacros(info, buildMacroValues(), true));
        }

        // QUERIED ACTIVE
        else if (d.cmd == " STATUS?" && !isAllCall) {
            QString status = m_config.my_status();
            if (status.isEmpty()) {
                continue;
            }

            reply = QString("%1 STATUS %2")
                        .arg(d.from)
                        .arg(replaceMacros(status, buildMacroValues(), true));
        }

        // QUERIED GRID
        else if (d.cmd == " GRID?" && !isAllCall) {
            QString grid = m_config.my_grid();
            if (grid.isEmpty()) {
                continue;
            }

            reply = QString("%1 GRID %2").arg(d.from).arg(grid);
        }

        // QUERIED STATIONS HEARD
        else if (d.cmd == " HEARING?" && !isAllCall) {
            auto calls = m_callActivity.keys();

            std::stable_sort(calls.begin(), calls.end(),
                             [this](QString const &lhs, QString const &rhs) {
                                 return m_callActivity[rhs].utcTimestamp <
                                        m_callActivity[lhs].utcTimestamp;
                             });

            auto const callsignAging = m_config.callsign_aging();
            auto const maxStations = 4;
            auto i = 0;
            QStringList lines;

            for (auto const &call : calls) {
                if (i >= maxStations)
                    break;
                if (call == d.from)
                    continue;

                auto const &cd = m_callActivity[call];

                if (callsignAging &&
                    cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
                    continue;
                }

                lines.append(cd.call);
                i++;
            }

            lines.prepend(QString("%1 HEARING").arg(d.from));
            reply = lines.join(' ');
        }

        // PROCESS RELAY
        else if (d.cmd == ">" && !isAllCall && !m_config.relay_off()) {

            // 1. see if there are any more hops to process
            // 2. if so, forward
            // 3. otherwise, display alert & reply dialog

            QString callToPattern = {
                R"(^(?<callsign>\b(?<prefix>[A-Z0-9]{1,4}\/)?(?<base>([0-9A-Z])?([0-9A-Z])([0-9])([A-Z])?([A-Z])?([A-Z])?)(?<suffix>\/[A-Z0-9]{1,4})?(?<type>[> ]))\b)"};
            QRegularExpression re(callToPattern);
            auto text = d.text;
            auto match = re.match(text);

            // if the text starts with a callsign, and relay is not disabled,
            // and this is not a group callsign, then relay.
            if (match.hasMatch() && !isGroupCall) {
                // replace freetext with relayed free text
                if (match.captured("type") != ">") {
                    text = text.replace(match.capturedStart("type"),
                                        match.capturedLength("type"), ">");
                }
                reply = QString("%1 *DE* %2").arg(text).arg(d.from);

                // otherwise, as long as we're not an ACK...alert the user and
                // either send an ACK or Message
            } else if (!d.text.startsWith("ACK")) {

                // parse out the callsign path
                auto calls = parseRelayPathCallsigns(d.from, d.text);

                // put these third party calls in the heard list
                foreach (auto call, calls) {
                    CallDetail cd = {};
                    cd.call = call;
                    cd.snr = -64;
                    cd.dial = d.dial;
                    cd.offset = d.offset;
                    cd.through = d.from;
                    cd.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
                    cd.tdrift = d.tdrift;
                    cd.submode = d.submode;
                    logCallActivity(cd, false);
                }

                d.relayPath = calls.join('>');

                reply = QString("%1 ACK").arg(d.relayPath);

                // check to see if the relay text contains a command that should
                // be replied to instead of an ack.
                QStringList relayedCmds = d.text.split(" ");
                if (!relayedCmds.isEmpty()) {
                    auto first = relayedCmds.first();

                    auto valid = Varicode::isCommandAllowed(first);
                    if (!valid) {
                        first = " " + first;
                        valid = Varicode::isCommandAllowed(first);
                        if (valid) {
                            relayedCmds.removeFirst();
                        }
                    }

                    // HACK: "MSG TO:" should be supported but contains a space
                    // :(
                    if (!relayedCmds.isEmpty()) {
                        if (first == " MSG") {
                            auto second = relayedCmds.first();
                            if (second == "TO:") {
                                first = " MSG TO:";
                                relayedCmds.removeFirst();
                            } else if (second.startsWith("TO:")) {
                                first = " MSG TO:";
                                relayedCmds.replace(0, second.mid(3));
                            }
                        } else if (first == " QUERY") {
                            auto second = relayedCmds.first();
                            if (second == "MSGS" || second == "MSGS?") {
                                first = " QUERY MSGS";
                                relayedCmds.removeFirst();
                            } else if (second == "CALL") {
                                first = " QUERY CALL";
                                relayedCmds.removeFirst();
                            }
                        }
                    }

                    if (Varicode::isCommandAllowed(first) &&
                        Varicode::isCommandAutoreply(first)) {
                        CommandDetail rd = {};
                        rd.bits = d.bits;
                        rd.cmd = first;
                        rd.dial = d.dial;
                        rd.offset = d.offset;
                        rd.from =
                            d.from; // note, MSG and QUERY commands are not set
                                    // with from as the relay path.
                        rd.relayPath = d.relayPath;
                        rd.text = relayedCmds.join(" "); // d.text;
                        rd.to = d.to;
                        rd.utcTimestamp = d.utcTimestamp;

                        m_rxCommandQueue.insert(0, rd);
                        continue;
                    }
                }

#if STORE_RELAY_MSGS_TO_INBOX
                // if we make it here, this is a message
                addCommandToMyInbox(d);
#endif
            }
        }

        // PROCESS MESSAGE STORAGE
        else if (d.cmd == " MSG TO:" && !isAllCall) {

            // store message
            QStringList segs = d.text.split(" ");
            if (segs.isEmpty()) {
                continue;
            }

            auto to = segs.first();
            segs.removeFirst();

            auto text = segs.join(" ").trimmed();

            /**
             * APRS relay: inbound `MSG TO:` stores for our local station.
             * Dedupe within 60s using TO + TEXT + DE <SENDER>.
             */
            if (d.to == "@APRSIS") {
                if (to != m_config.my_callsign().trimmed()) {
                    continue;
                }

                static QRegularExpression aprsDeRe("\\s+DE\\s+(\\S+)\\s*$");
                auto aprsSender = QString("APRS");
                auto deMatch = aprsDeRe.match(text);
                if (deMatch.hasMatch()) {
                    aprsSender = deMatch.captured(1);
                }
                auto dedupeKey =
                    QString("%1|%2|%3").arg(to, text, aprsSender).toUpper();
                auto now = DriftingDateTime::currentDateTimeUtc();
                auto cutoff = now.addSecs(-60);
                auto it = m_aprsRelayDedupCache.begin();
                while (it != m_aprsRelayDedupCache.end()) {
                    if (it.value() < cutoff) {
                        it = m_aprsRelayDedupCache.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (m_aprsRelayDedupCache.contains(dedupeKey)) {
                    continue;
                }

                CommandDetail cd = {};
                cd.bits = d.bits;
                cd.cmd = " MSG ";
                cd.extra = d.extra;
                cd.dial = d.dial;
                cd.offset = d.offset;
                cd.from = "APRS";
                cd.relayPath = "APRS";
                cd.grid = d.grid;
                cd.snr = d.snr;
                cd.tdrift = d.tdrift;
                cd.text = text;
                cd.to = to;
                cd.utcTimestamp = d.utcTimestamp;
                cd.submode = d.submode;

                addCommandToMyInbox(cd);

                m_aprsRelayDedupCache.insert(dedupeKey, now);
                tryNotify("inbox", d.submode);
                continue;
            }

            if (m_config.relay_off()) {
                continue;
            }

            auto calls = parseRelayPathCallsigns(d.from, text);
            d.relayPath = calls.join(">");

            CommandDetail cd = {};
            cd.bits = d.bits;
            cd.cmd = d.cmd;
            cd.extra = d.extra;
            cd.dial = d.dial;
            cd.offset = d.offset;
            cd.from = d.from;
            cd.grid = d.grid;
            cd.relayPath = d.relayPath;
            cd.snr = d.snr;
            cd.tdrift = d.tdrift;
            cd.text = text;
            cd.to = Radio::base_callsign(to);
            cd.utcTimestamp = d.utcTimestamp;
            cd.submode = d.submode;

            qCDebug(mainwindow_js8)
                << "storing message to" << to << ":" << text;

            // [MSG TO: DIAG 2026-06-12 build 258] Surface storage path
            // visibly so we can confirm what actually lands where when
            // the operator reports inbox-routing surprises.
            qWarning() << "[MSG TO:] on-air store: from=" << d.from
                       << "to=K9AVT-style-relay=" << d.to
                       << "addressee=" << to
                       << "baseAddressee=" << Radio::base_callsign(to)
                       << "text=" << text
                       << "→ addCommandToStorage(STORE)";

            addCommandToStorage("STORE", cd);

            // we haven't replaced the from with the relay path, so we have to
            // use it for the ack if there is one
            reply = QString("%1 ACK").arg(calls.length() > 1 ? d.relayPath
                                                             : d.from);
        }

        // PROCESS AGN
        else if (d.cmd == " AGN?" && !isAllCall && !isGroupCall &&
                 !m_lastTxMessage.isEmpty()) {
            reply = Varicode::rstrip(m_lastTxMessage);
        }

        // PROCESS ACTIVE HEARTBEAT
        // if we have hb mode enabled and auto reply enabled <del>and auto ack
        // enabled and no callsign is selected</del> update: if we're in HB
        // mode, doesn't matter if a callsign is selected.
        else if ((d.cmd == " HB" || d.cmd == " HEARTBEAT") &&
                 canCurrentModeAckHeartbeat() &&
                 ui->actionModeJS8HB->isChecked() &&
                 ui->actionModeAutoreply->isChecked() &&
                 ui->actionHeartbeatAcknowledgements->isChecked()) {
            // do not process HB activity if buffer is not empty, this prevents
            // broken incoming MSG's
            if (!m_messageBuffer.isEmpty()) {
                qCDebug(mainwindow_js8) << "hb paused for messageBuffer";
                continue;
            }

            // check to make sure we aren't pausing HB transmissions (ACKs)
            // while a callsign is selected
            if (m_config.heartbeat_qso_pause() && !selectedCallsign.isEmpty()) {
                qCDebug(mainwindow_js8) << "hb paused during qso";
                continue;
            }

            // check to make sure this callsign isn't blacklisted
            if (m_config.hb_blacklist().contains(d.from) ||
                m_config.hb_blacklist().contains(
                    Radio::base_callsign(d.from))) {
                qCDebug(mainwindow_js8) << "hb blacklist blocking" << d.from;
                continue;
            }

            // check to see if we have a message for a station who is
            // heartbeating
            QString extra;
            auto mid = getNextMessageIdForCallsign(d.from);

            // Get number of unread messages
            int pendingCount = countUnreadForCallsign(d.from);
            if (isGroupCall) {
                pendingCount += countGroupUnreadForCallsign(d.to, d.from);
            }

            // We want to send the number of pending messages IN ADDITION TO
            // the one we are about to send, so decrement the actual count
            pendingCount--;

            if (mid != -1) {
                if (pendingCount > 0) {
                    extra = QString("MSG ID %1 +%2)")
                            .arg(mid)
                            .arg(pendingCount);
                }
                else {
                    extra = QString("MSG ID %1").arg(mid);
                }

            }

            // group messaging - if isGroupCall, check to see if there's a
            // message id for the group and return it if there's not an
            // individual message
            // TODO: include group name in response to differentiate from direct
            // messages?
            else if (isGroupCall) {
                mid = getNextGroupMessageIdForCallsign(d.to, d.from);
                if (mid != -1) {
                    if (pendingCount > 0) {
                        extra = QString("MSG ID %1 +%2)")
                            .arg(mid)
                            .arg(pendingCount);
                    }
                    else {
                        extra = QString("MSG ID %1").arg(mid);
                    }
                }
            }

            // TODO: require confirmation?
            sendHeartbeatAck(d.from, d.snr, extra);

            if (isAllCall) {
                // since all pings are technically @ALLCALL, let's bump the
                // allcall cache here...
                m_txAllcallCommandCache.insert(d.from, new QDateTime(now), 5);
            }

            continue;
        }

        // PROCESS HEARTBEAT SNR
        else if (d.cmd == " HEARTBEAT SNR") {
            qCDebug(mainwindow_js8) << "skipping incoming hb snr" << d.text;
            continue;
        }

        // PROCESS CQ
        else if (d.cmd == " CQ") {
            qCDebug(mainwindow_js8) << "skipping incoming cq" << d.text;
            continue;
        }

        // PROCESS MSG
        else if (d.cmd == " MSG" && !isAllCall) {

            auto text = d.text;

            /**
             * APRS relay: inbound `MSG` where the payload starts with
             * `TO:<DEST>` and includes `DE <SENDER>`.
             * Dedupe within 60s using TO + TEXT + DE <SENDER>.
             */
            if (d.to == "@APRSIS") {
                static QRegularExpression aprsToRe("^TO:\\s*(\\S+)\\s+(.*)$");
                auto match = aprsToRe.match(text);
                if (match.hasMatch()) {
                    auto dest = match.captured(1);
                    auto aprsText = match.captured(2).trimmed();
                    if (dest == m_config.my_callsign().trimmed()) {
                        static QRegularExpression aprsDeRe(
                            "\\s+DE\\s+(\\S+)\\s*$");
                        auto aprsSender = QString("APRS");
                        auto deMatch = aprsDeRe.match(aprsText);
                        if (deMatch.hasMatch()) {
                            aprsSender = deMatch.captured(1);
                        }
                        auto dedupeKey = QString("%1|%2|%3")
                                             .arg(dest, aprsText, aprsSender)
                                             .toUpper();
                        auto now = DriftingDateTime::currentDateTimeUtc();
                        auto cutoff = now.addSecs(-60);
                        auto it = m_aprsRelayDedupCache.begin();
                        while (it != m_aprsRelayDedupCache.end()) {
                            if (it.value() < cutoff) {
                                it = m_aprsRelayDedupCache.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        if (m_aprsRelayDedupCache.contains(dedupeKey)) {
                            continue;
                        }

                        CommandDetail cd = d;
                        cd.cmd = " MSG ";
                        cd.from = "APRS";
                        cd.relayPath = "APRS";
                        cd.to = dest;
                        cd.text = aprsText;

                        addCommandToMyInbox(cd);

                        m_aprsRelayDedupCache.insert(dedupeKey, now);
                        tryNotify("inbox", d.submode);
                    }
                }
                continue;
            }

            qCDebug(mainwindow_js8) << "adding message to inbox" << text;

            auto calls = parseRelayPathCallsigns(d.from, text);

            d.cmd = " MSG ";
            d.relayPath = calls.join(">");
            d.text = text;

            addCommandToMyInbox(d);

            // notification
            tryNotify("inbox", d.submode);

            // we haven't replaced the from with the relay path, so we have to
            // use it for the ack if there is one
            reply = QString("%1 ACK").arg(calls.length() > 1 ? d.relayPath
                                                             : d.from);

#define SHOW_ALERT_FOR_MSG 1
#if SHOW_ALERT_FOR_MSG
            SelfDestructMessageBox *m = new SelfDestructMessageBox(
                300, "New Message Received",
                QString("A new message was received at %1 UTC from %2")
                    .arg(d.utcTimestamp.time().toString())
                    .arg(d.from),
                QMessageBox::Information, QMessageBox::Ok, QMessageBox::Ok,
                false, this);

            m->show();
#endif
        }

        // PROCESS ACKS
        else if (d.cmd == " ACK" && !isAllCall) {
            qCDebug(mainwindow_js8) << "skipping incoming ack" << d.text;

            // notification for ack
            tryNotify("ack", d.submode);

            // make sure this is explicit
            continue;
        }

        // PROCESS NACKS — mirror the ACK block so chunked-ARQ NACKs
        // that fell through the early dispatch (for conversation-
        // window visibility) don't trigger an unintended autoreply.
        // NACK lives at slot 2 in directed_cmds and IS a member of
        // autoreply_cmds, so without this explicit skip the
        // reply-construction chain below could fire.
        else if (d.cmd == " NACK" && !isAllCall) {
            qCDebug(mainwindow_js8) << "skipping incoming nack" << d.text;
            tryNotify("ack", d.submode);
            continue;
        }

        // PROCESS BUFFERED CMD
        else if (d.cmd == " CMD" && !isAllCall) {
            qCDebug(mainwindow_js8) << "skipping incoming command" << d.text;

            // make sure this is explicit
            continue;
        }

        // PROCESS BUFFERED QUERY
        //
        // 2026-06-07 (arq-queryAll): dropped the `!isAllCall` gate so
        // "@ALLCALL QUERY ARQ?" and "@CUSTOM_GROUP QUERY ARQ?" reach
        // this handler. The MSG sub-handler below re-gates on !isAllCall
        // (delivering inbox content to an indeterminate audience makes
        // no sense), but ARQ? capability replies are safe to send to
        // any addressee — the response is identical for every querier.
        // The existing m_txAllcallCommandCache 15-minute per-station
        // rate-limit (line ~411) keeps us from flooding when many
        // stations probe the band with @ALLCALL QUERY ARQ?.
        else if (d.cmd == " QUERY") {
            auto who = d.from; // keep in mind, this is the sender, not the
                               // original requestor if relayed
            auto replyPath = d.from;

            if (d.relayPath.contains(">")) {
                auto path = d.relayPath.split(">");
                who = path.last();
                replyPath = d.relayPath;
            }

            QStringList segs = d.text.split(" ");
            if (segs.isEmpty()) {
                continue;
            }

            auto cmd = segs.first();
            segs.removeFirst();

            if (cmd == "MSG" && !segs.isEmpty() && !isAllCall) {
                auto inbox = Inbox(inboxPath());
                if (!inbox.open()) {
                    continue;
                }

                bool ok = false;
                int mid = QString(segs.first()).toInt(&ok);
                if (!ok) {
                    continue;
                }

                auto msg = inbox.value(mid);
                auto params = msg.params();
                if (params.isEmpty()) {
                    continue;
                }

                auto from = params.value("FROM").toString().trimmed();

                auto to = params.value("TO").toString().trimmed();

                // group messaging - allow any message to a @GROUP to be
                // retrieved by anybody
                bool isGroupMsg = to.startsWith("@");

                if (!isGroupMsg && to != who &&
                    to != Radio::base_callsign(who)) {
                    continue;
                }

                auto text = params.value("TEXT").toString().trimmed();
                if (text.isEmpty()) {
                    continue;
                }

                /*
                 * mark as delivered (so subsequent HBs and QUERY MSGS don't
                 * receive this message)
                 *
                 * Do this in callbacks so that messages are only marked read
                 * after any potential confirmations have been made by the user,
                 * and the message has been processed in the transaction queue.
                 */
                if (!isGroupMsg) {
                    callback = [this, mid, msg]() {
                        this->markMsgDelivered(mid, msg);
                    };
                } else {
                    callback = [this, mid, who]() {
                        this->markGroupMsgDeliveredForCallsign(mid, who);
                    };
                }

                // Get number of unread messages
                int pendingCount = countUnreadForCallsign(who);
                if (isGroupCall) {
                    pendingCount += countGroupUnreadForCallsign(d.to, who);
                }

                // We want to send the number of pending messages IN ADDITION TO
                // the one we are about to send, so decrement the actual count
                // We need to decrement 2 here, because "mark as read" occurs
                // in a callback after successful transmission
                pendingCount-=2;

                auto lookaheadMid = getLookaheadMessageIdForCallsign(who, mid);
                if (lookaheadMid == -1 && isGroupMsg) {
                    lookaheadMid =
                        getLookaheadGroupMessageIdForCallsign(d.to, who, mid);
                }

                // and reply
                if (lookaheadMid != -1) {
                    if (pendingCount > 0) {
                        reply = QString("%1 MSG %2 FROM %3 NEXT MSG ID %4 +%5");
                        reply = reply.arg(replyPath);
                        reply = reply.arg(text);
                        reply = reply.arg(from);
                        reply = reply.arg(lookaheadMid);
                        reply = reply.arg(pendingCount);
                    }
                    else {
                        reply = QString("%1 MSG %2 FROM %3 NEXT MSG ID %4");
                        reply = reply.arg(replyPath);
                        reply = reply.arg(text);
                        reply = reply.arg(from);
                        reply = reply.arg(lookaheadMid);
                    }

                } else {
                    reply = QString("%1 MSG %2 FROM %3");
                    reply = reply.arg(replyPath);
                    reply = reply.arg(text);
                    reply = reply.arg(from);
                }
            }

            // QUERY ARQ? — capability interrogation. Wire form
            // "<peer> QUERY ARQ?" parses as cmd=" QUERY" + body="ARQ?"
            // (the regex's bare-QUERY alternation, with the trailing
            // "ARQ?" landing in d.text). Reply with "<peer> YES <level>"
            // where level is the ARQ_PROTOCOL_LEVEL constant. The peer
            // already knows the response is ARQ-related from sending
            // the query; future builds may bump the level when wire
            // semantics change so they can decide whether to use ARQ
            // with us.
            // [QUERY ARQ HANDLER WIDEN 2026-06-11 build 249]
            // Also match the question-mark-less form. Build 239's TX
            // checksum-skip and Build 248's RX checksum-skip both
            // accept "ARQ" and "ARQ?", but until now this handler
            // matched only "ARQ?" — so a manually-typed
            // "<peer> QUERY ARQ" would pass through buffered
            // validation but never produce a YES reply. Now both
            // forms get the response. Standard menu still sends
            // "QUERY ARQ?" so the existing flow is unchanged.
            else if (cmd == "ARQ?" || cmd == "ARQ") {
                reply = QString("%1 YES %2")
                            .arg(replyPath)
                            .arg(ChunkedArq::ARQ_PROTOCOL_LEVEL);
            }
        }

        // PROCESS BUFFERED QUERY MSGS
        else if (d.cmd == " QUERY MSGS" &&
                 ui->actionModeAutoreply->isChecked()) {
            auto who = d.from; // keep in mind, this is the sender, not the
                               // original requestor if relayed
            auto replyPath = d.from;

            if (d.relayPath.contains(">")) {
                auto path = d.relayPath.split(">");
                who = path.last();
                replyPath = d.relayPath;
            }

            // if this is an allcall or a directed call, check to see if we have
            // a stored message for user. we reply yes if the user would be able
            // to retreive a stored message
            auto mid = getNextMessageIdForCallsign(who);

            // Get number of unread messages
            int pendingCount = countUnreadForCallsign(who);
            if (isGroupCall) {
                pendingCount += countGroupUnreadForCallsign(d.to, d.from);
            }

            // We want to send the number of pending messages IN ADDITION TO
            // the one we are about to send, so decrement the actual count
            pendingCount--;

            if (mid != -1) {
                if (pendingCount > 0) {
                    reply = QString("%1 YES MSG ID %2 +%3")
                                .arg(replyPath)
                                .arg(mid)
                                .arg(pendingCount);
                }
                else {
                    reply = QString("%1 YES MSG ID %2")
                                .arg(replyPath)
                                .arg(mid);
                }
            }

            // Group messaging - if isGroupCall, check to see if there's a
            // message id for the group and return it if there's not an
            // individual message
            // TODO: include group name in response to differentiate from direct
            // messages?
            else if (isGroupCall) {
                mid = getNextGroupMessageIdForCallsign(d.to, d.from);
                if (mid != -1) {
                    if (pendingCount > 0) {
                        reply = QString("%1 YES MSG ID %2 +%3")
                                    .arg(replyPath)
                                    .arg(mid)
                                    .arg(pendingCount);
                    }
                    else {
                        reply = QString("%1 YES MSG ID %2")
                                    .arg(replyPath)
                                    .arg(mid);
                    }
                }
            }

            // if this is not an allcall and we have no messages, reply no.
            if (!isAllCall && reply.isEmpty()) {
                reply = QString("%1 NO").arg(replyPath);
            }
        }

        // PROCESS BUFFERED QUERY CALL
        else if (d.cmd == " QUERY CALL" &&
                 ui->actionModeAutoreply->isChecked()) {
            auto replyPath = d.from;
            if (d.relayPath.contains(">")) {
                replyPath = d.relayPath;
            }

            auto who = d.text;
            if (who.isEmpty()) {
                continue;
            }

            auto callsigns = Varicode::parseCallsigns(who);
            if (callsigns.isEmpty()) {
                continue;
            }

            QStringList replies;
            int callsignAging = m_config.callsign_aging();
            auto baseCall = callsigns.first();
            foreach (auto cd, m_callActivity.values()) {
                if (callsignAging &&
                    cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
                    continue;
                }

                if (baseCall == cd.call ||
                    baseCall == Radio::base_callsign(cd.call)) {
                    auto r = QString("%1 (%2)")
                                 .arg(Varicode::formatSNR(cd.snr))
                                 .arg(since(cd.utcTimestamp))
                                 .trimmed();
                    replies.append(r);
                    break;
                }
            }

            if (!replies.isEmpty()) {
                replies.prepend(QString("%1 YES").arg(replyPath));
            }

            reply = replies.join(" ");

            if (!reply.isEmpty()) {
                if (isAllCall) {
                    m_txAllcallCommandCache.insert(d.from, new QDateTime(now),
                                                   25);
                }
            }
        }

#if 0
        // PROCESS ALERT
        else if (d.cmd == "!" && !isAllCall) {

            // create alert dialog
            processAlertReplyForCommand(d, d.from, " ");

            // make sure this is explicit
            continue;
        }
#endif

        // well, if there's no reply, don't do anything...
        if (reply.isEmpty()) {
            // [REPLY-GATE DIAG 2026-06-10 build 244] track which gate
            // silently drops the relay-cmd forward TX. Only log when
            // we KNOW we'd otherwise have sent something.
            if (d.cmd == QStringLiteral(">")) {
                qWarning() << "[REPLY-GATE] relay: reply is empty"
                           << "from=" << d.from << "to=" << d.to
                           << "text=" << d.text;
            }
            continue;
        }

        if (d.cmd == QStringLiteral(">")) {
            qWarning() << "[REPLY-GATE] relay: reply built"
                       << "reply=" << reply
                       << "from=" << d.from << "to=" << d.to;
        }

        // do not queue @ALLCALL replies if auto-reply is not checked
        if (!ui->actionModeAutoreply->isChecked() && isAllCall) {
            if (d.cmd == QStringLiteral(">")) {
                qWarning() << "[REPLY-GATE] relay: blocked by autoreply-off+allcall";
            }
            continue;
        }

#if 0
        // TODO: jsherer - HB issue here
        // do not queue a reply if it's a HB and HB is not active
        // if((!ui->hbMacroButton->isChecked() || m_hbInterval <= 0) && d.cmd.contains("HB")){
        //     continue;
        // }
#endif

        // do not queue for reply if there's text in the window
        // (exception: relay ">" — the forward is an obligation
        //  initiated by a remote station, not an autoreply that should
        //  defer to the local operator's typing. Queue it; the TX
        //  queue will serialise it after whatever else is pending.)
        bool const isRelayForward = (d.cmd == QStringLiteral(">"));
        if (!ui->extFreeTextMsgEdit->toPlainText().isEmpty() &&
            !isRelayForward) {
            continue;
        }

        // do not queue for reply if there's a buffer open to us
        // (same exception as above)
        int bufferOffset = 0;
        if (hasExistingMessageBufferToMe(&bufferOffset) &&
            !isRelayForward) {
            qCDebug(mainwindow_js8) << "skipping reply due to open buffer"
                                    << bufferOffset << m_messageBuffer.count();
            continue;
        }
        if (isRelayForward) {
            qWarning() << "[REPLY-GATE] relay: enqueuing forward"
                       << "reply=" << reply
                       << "extEditEmpty=" << ui->extFreeTextMsgEdit->toPlainText().isEmpty()
                       << "openBufferToMe=" << hasExistingMessageBufferToMe(nullptr);
        }

        // add @ALLCALLs to the @ALLCALL cache
        if (isAllCall) {
            m_txAllcallCommandCache.insert(d.from, new QDateTime(now), 25);
        }

        // queue the reply here to be sent when a free interval is available on
        // the frequency that was sent unless, this is an allcall, to which we
        // should be responding on a clear frequency offset we always want to
        // make sure that the directed cache has been updated at this point so
        // we have the most information available to make a frequency selection.
        if (m_config.autoreply_confirmation()) {
            confirmThenEnqueueMessage(90, priority, reply, freq, callback);
        } else {
            enqueueMessage(priority, reply, freq, callback);
        }
    }
}
