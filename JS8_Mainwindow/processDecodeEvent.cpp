

/** \file
 * @brief member function of the UI_Constructor class
 *  process decoded text
 */

#include "JS8_UI/mainwindow.h"

void UI_Constructor::processDecodeEvent(JS8::Event::Variant const &event) {

    std::visit(
        [this](auto &&e) {
            using T = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<T, JS8::Event::DecodeStarted>) {
                if (m_wideGraph->shouldDisplayDecodeAttempts()) {
                    m_wideGraph->drawHorizontalLine(QColor(Qt::yellow), 0, 5);
                }
            } else if constexpr (std::is_same_v<T, JS8::Event::SyncState>) {
                if (m_wideGraph->shouldDisplayDecodeAttempts()) {
                    auto const drawDecodeLine =
                        [this, freq = static_cast<int>(e.frequency),
                         mode = e.mode](QColor const &color) {
                            m_wideGraph->drawDecodeLine(
                                color, freq,
                                freq + JS8::Submode::bandwidth(mode));
                        };

                    if (e.type == JS8::Event::SyncState::Type::DECODED) {
                        drawDecodeLine(Qt::red);
                    } else if (auto const xdtMs = static_cast<int>(e.dt * 1000);
                               std::abs(xdtMs) <= 2000) {
                        if (e.sync.candidate < 10)
                            drawDecodeLine(Qt::darkCyan);
                        else if (e.sync.candidate <= 15)
                            drawDecodeLine(Qt::cyan);
                        else if (e.sync.candidate <= 21)
                            drawDecodeLine(Qt::white);
                    }
                }
            } else if constexpr (std::is_same_v<T,
                                                JS8::Event::DecodeFinished>) {
                qCDebug(decoder_js8) << "decode duration"
                                     << m_decoderBusyStartTime.msecsTo(
                                            QDateTime::currentDateTimeUtc())
                                     << "ms";

                m_bDecoded = e.decoded > 0;
                decodeDone();
            } else if constexpr (std::is_same_v<T, JS8::Event::Decoded>) {
                // A frame is valid if we haven't seen the same frame in the
                // past 1/2 decode period.
                //
                // Note: Success here depends on decodes ordered such that
                // frequencies
                //       near `dec_data.params.nfqso` arrive here first, so it's
                //       key to process the decode candidates in an ordered
                //       manner, likely by sorting the raw take from the initial
                //       selection pass.

                // Use standard decoder's SNR for L2 decodes when available
                auto ev = e;
                if (ev.mode == 16) {
                    int freqKey = static_cast<int>(ev.frequency) / 10;
                    if (!ev.l2) {
                        m_ft2StdSnr[freqKey] = ev.snr;
                    } else {
                        auto it = m_ft2StdSnr.find(freqKey);
                        if (it != m_ft2StdSnr.end()) {
                            ev.snr = it.value();
                        }
                    }
                }

                qCDebug(mainwindow_js8) << "[DECODE-EVENT] received:"
                           << "snr=" << ev.snr << "freq=" << ev.frequency
                           << "mode=" << ev.mode << "l2=" << ev.l2
                           << "data=" << QString::fromStdString(ev.data);

                DecodedText decodedtext(ev);
                // Mode-agnostic dedup: same frame content = same message
                // regardless of which decoder/mode produced it
                FrameCacheKey dedupeKey(0, decodedtext.frame());

                qCDebug(mainwindow_js8) << "[DECODE-EVENT] DecodedText: submode=" << decodedtext.submode()
                           << "frame=" << decodedtext.frame()
                           << "msg=" << decodedtext.message();

                if (auto const it = m_messageDupeCache.find(dedupeKey);
                    it != m_messageDupeCache.end()) {
                    auto ageSecs = it->second.secsTo(QDateTime::currentDateTimeUtc());
                    // FT2 L2: 90K buffer holds ~7.5s, same frame can re-decode
                    // from non-overlapping buffer positions up to 7.5s apart.
                    // Use 8s dedup window for FT2, half-period for other modes.
                    auto window = (decodedtext.submode() == Varicode::JS8CallFT2)
                        ? 8.0
                        : 0.5 * JS8::Submode::period(decodedtext.submode());
                    if (ageSecs < window) {
                        qCDebug(mainwindow_js8) << "[DECODE-EVENT] DUPLICATE, skipping frame=" << decodedtext.frame()
                                   << "age=" << ageSecs << "s, window=" << window << "s";
                        return;
                    }
                }
#if 0
        // frames are valid if they meet our minimum rx threshold for the submode
        bool bValidFrame = decodedtext.snr() >= JS8::Submode::rxSNRThreshold(decodedtext.submode());

        qCDebug(mainwindow_js8) << "valid" << bValidFrame << JS8::Submode::name(decodedtext.submode()) << "decoded text" << decodedtext.message();

        // skip if invalid
        if(!bValidFrame) {
            return;
        }
#else
                qCDebug(mainwindow_js8)
                    << JS8::Submode::name(decodedtext.submode())
                    << "decoded text" << decodedtext.message();
#endif
                // if the frame is valid, cache it!
                m_messageDupeCache.insert_or_assign(
                    dedupeKey, QDateTime::currentDateTimeUtc());

                // log valid frames to ALL.txt (and correct their timestamp
                // format)
                auto freq = dialFrequency();

                // if we changed frequencies, use the old frequency that we
                // started the decode with
                if (m_decoderBusyFreq != freq) {
                    freq = m_decoderBusyFreq;
                }

                auto date = DriftingDateTime::currentDateTimeUtc().toString(
                    "yyyy-MM-dd");
                writeAllTxt(date + " " + decodedtext.string() + " " +
                            decodedtext.message());

                /**
                 * @brief Send decode to WSJT-X protocol
                 *
                 * Converts JS8Call decode events to WSJT-X Decode messages and
                 * sends them to WSJT-X protocol clients. For HeartBeat
                 * messages, ensures the message text includes callsign and grid
                 * so clients can properly associate them with grid plots.
                 */
                if (m_wsjtxMessageMapper && m_config.wsjtx_protocol_enabled()) {
                    // Convert decode time from JS8Call format to QTime
                    auto const hms = decode_time(decodedtext.time());
                    QTime decode_time = QTime(hms.hour, hms.minute, hms.second);

                    // Send decode message
                    // Use "JS8" as the mode string (WSJT-X expects mode names
                    // like "FT8", "FT4", "JT9", etc.)
                    m_wsjtxMessageMapper->sendDecode(
                        true, // is_new - always true for new decodes
                        decode_time, decodedtext.snr(), decodedtext.dt(),
                        static_cast<quint32>(decodedtext.frequencyOffset()),
                        "JS8", // mode string
                        decodedtext.message(), decodedtext.isLowConfidence());
                }

                ActivityDetail d = {};
                CallDetail cd = {};
                CommandDetail cmd = {};
                CallDetail td = {};

            // Parse General Activity
#if 1
                bool shouldParseGeneralActivity = true;
                if (shouldParseGeneralActivity &&
                    !decodedtext.messageWords().isEmpty()) {
                    int offset = decodedtext.frequencyOffset();

                    if (!m_bandActivity.contains(offset)) {
                        int const range =
                            JS8::Submode::rxThreshold(decodedtext.submode());

                        QList<int> offsets =
                            generateOffsets(offset - range, offset + range);

                        bool incomingIsFT2 = decodedtext.submode() == Varicode::JS8CallFT2;
                        foreach (int prevOffset, offsets) {
                            if (!m_bandActivity.contains(prevOffset)) {
                                continue;
                            }
                            // Don't merge FT2/Subspace with standard modes
                            if (!m_bandActivity[prevOffset].isEmpty()) {
                                bool existingIsFT2 = m_bandActivity[prevOffset].last().submode == Varicode::JS8CallFT2;
                                if (existingIsFT2 != incomingIsFT2)
                                    continue;
                            }
                            m_bandActivity[offset] = m_bandActivity[prevOffset];
                            m_bandActivity.remove(prevOffset);
                            break;
                        }
                    }

                    // ActivityDetail d = {};
                    d.isLowConfidence = decodedtext.isLowConfidence();
                    d.isCompound = decodedtext.isCompound();
                    d.isDirected = decodedtext.isDirectedMessage();
                    d.bits = decodedtext.bits();
                    d.dial = freq;
                    d.offset = offset;
                    d.text = decodedtext.message();
                    d.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
                    d.snr = decodedtext.snr();
                    d.isBuffered = false;
                    d.submode = decodedtext.submode();

                    // Detect single-frame-in-buffer condition for SNR accuracy
                    if (d.submode == Varicode::JS8CallFT2) {
                        bool singleFrame = true;
                        if (m_bandActivity.contains(offset) &&
                            !m_bandActivity[offset].isEmpty()) {
                            auto lastTime = m_bandActivity[offset].last().utcTimestamp;
                            auto gapSecs = lastTime.secsTo(d.utcTimestamp);
                            if (gapSecs < 8)
                                singleFrame = false;
                        }
                        d.snrSuspect = singleFrame;
                        if (singleFrame)
                            qWarning() << "[SNR-SUSPECT] single frame in buffer"
                                       << "snr=" << d.snr << "freq=" << offset;
                    }
                    d.tdrift = (d.submode == Varicode::JS8CallFT2)
                                   ? 0.0
                                   : m_wideGraph->shouldAutoSyncSubmode(d.submode)
                                         ? DriftingDateTime::drift() / 1000.0
                                         : decodedtext.dt();

                    // if we have any "first" frame, and a buffer is already
                    // established, clear it...
                    int prevBufferOffset = -1;
                    if (((d.bits & Varicode::JS8CallFirst) ==
                         Varicode::JS8CallFirst) &&
                        hasExistingMessageBuffer(decodedtext.submode(),
                                                 d.offset, true,
                                                 &prevBufferOffset)) {
                        qCDebug(mainwindow_js8) << "first message encountered, "
                                                   "clearing existing buffer"
                                                << prevBufferOffset;
                        m_messageBuffer.remove(d.offset);
                    }

                    // if we have a data frame, and a message buffer has been
                    // established, buffer it...
                    if (hasExistingMessageBuffer(decodedtext.submode(),
                                                 d.offset, true,
                                                 &prevBufferOffset) &&
                        !decodedtext.isCompound() &&
                        !decodedtext.isDirectedMessage()) {
                        qCDebug(mainwindow_js8)
                            << "buffering data" << d.dial << d.offset << d.text;
                        d.isBuffered = true;
                        m_messageBuffer[d.offset].msgs.append(d);

                        // Update the originating call's SNR in the callsign
                        // list on every data frame of a buffered directed
                        // message. The header frame's SNR was stamped when
                        // the buffer was opened; data-body frames often
                        // decode at meaningfully different SNR, so refresh
                        // to the latest value. Attribution comes from
                        // m_messageBuffer[offset].cmd.from, which the header
                        // parse stashed in processCommandActivity's buffered
                        // branch. Grid/ACK/CQ timestamps stay untouched
                        // thanks to the preserve-if-empty logic in
                        // logCallActivity.
                        //
                        // Compound-call fallback: if the directed header's
                        // FROM is still the unresolved "<....>" placeholder
                        // but a FrameCompound arrived earlier at this
                        // offset (e.g. WM8Q/P), borrow that compound call
                        // for attribution. Without this fallback, body
                        // frames of a compound-prefixed message update
                        // nothing — the list entry would only refresh at
                        // message end after processBufferedCompoundMessages
                        // resolves the pair.
                        auto const &buf = m_messageBuffer[d.offset];
                        QString attribFrom = buf.cmd.from;
                        if ((attribFrom.isEmpty() ||
                             attribFrom == "<....>") &&
                            !buf.compound.isEmpty()) {
                            attribFrom = buf.compound.last().call;
                        }
                        if (!attribFrom.isEmpty() &&
                            attribFrom != "<....>") {
                            CallDetail cd{};
                            cd.call = attribFrom;
                            cd.dial = d.dial;
                            cd.offset = d.offset;
                            cd.snr = d.snr;
                            cd.bits = d.bits;
                            cd.utcTimestamp = d.utcTimestamp;
                            cd.tdrift = d.tdrift;
                            cd.submode = d.submode;
                            logCallActivity(cd, false);
                        }
                        // TODO: incremental display if it's "to" me.
                    }

                    m_rxActivityQueue.append(d);
                    m_bandActivity[offset].append(d);
                    // Build 145: cap by submode class. Subspace and
                    // Standard each keep an independent 10-frame
                    // history within an offset bucket. Without this
                    // split, busy traffic in one class would evict
                    // the other's history even though they render as
                    // separate rows (displayBandActivity, Build 145).
                    {
                        bool const incomingIsFT2 =
                            d.submode == Varicode::JS8CallFT2;
                        int sameClassCount = 0;
                        for (auto const & it : m_bandActivity[offset])
                            if ((it.submode == Varicode::JS8CallFT2)
                                == incomingIsFT2)
                                ++sameClassCount;
                        while (sameClassCount > 10) {
                            for (int i = 0;
                                 i < m_bandActivity[offset].size(); ++i) {
                                bool itIsFT2 =
                                    m_bandActivity[offset][i].submode
                                    == Varicode::JS8CallFT2;
                                if (itIsFT2 == incomingIsFT2) {
                                    m_bandActivity[offset].removeAt(i);
                                    --sameClassCount;
                                    break;
                                }
                            }
                        }
                    }


                    // Merge nearby same-mode entries that weren't caught on first decode
                    {
                        int const mergeRange = JS8::Submode::rxThreshold(decodedtext.submode());
                        bool isFT2 = decodedtext.submode() == Varicode::JS8CallFT2;
                        for (int nearby = offset - mergeRange; nearby <= offset + mergeRange; ++nearby) {
                            if (nearby == offset) continue;
                            if (!m_bandActivity.contains(nearby)) continue;
                            if (m_bandActivity[nearby].isEmpty()) continue;
                            bool nearbyIsFT2 = m_bandActivity[nearby].last().submode == Varicode::JS8CallFT2;
                            if (nearbyIsFT2 != isFT2) continue;

                            auto &current = m_bandActivity[offset];
                            auto &other = m_bandActivity[nearby];
                            current.append(other);
                            std::sort(current.begin(), current.end(),
                                      [](const ActivityDetail &a, const ActivityDetail &b) {
                                          return a.utcTimestamp < b.utcTimestamp;
                                      });
                            while (current.count() > 10) {
                                current.removeFirst();
                            }
                            m_bandActivity.remove(nearby);
                            break;
                        }
                    }
                }
#endif

            // Process compound callsign commands (put them in cache)"
#if 1
                qCDebug(mainwindow_js8) << "decoded" << decodedtext.frameType()
                                        << decodedtext.isCompound()
                                        << decodedtext.isDirectedMessage()
                                        << decodedtext.isHeartbeat();
                bool shouldProcessCompound = true;
                if (shouldProcessCompound && decodedtext.isCompound() &&
                    !decodedtext.isDirectedMessage()) {
                    cd.call = decodedtext.compoundCall();
                    if (!cd.call.isEmpty() && cd.call != "<....>") {
                        // Compound frames don't carry an explicit
                        // destination, but heartbeats imply one.
                        // Pre-compute it so this paint goes group-
                        // colored when applicable. The directed branch
                        // (when it fires for the same call at the same
                        // offset later) will repaint with the actual
                        // "to" via the same-call upgrade path in
                        // annotateCall.
                        QString impliedTo;
                        if (decodedtext.isHeartbeat()) {
                            impliedTo =
                                decodedtext.isAlt() ? "@ALLCALL" : "@HB";
                        }
                        // @ALLCALL is too broad to ever flag as
                        // a user-group destination.
                        bool const inMyGroup =
                            !impliedTo.isEmpty() &&
                            impliedTo != "@ALLCALL" &&
                            isGroupCallIncluded(impliedTo);
                        m_wideGraph->setCallsignOverlayEnabled(
                            m_config.show_calls_on_waterfall());
                        m_wideGraph->annotateCall(
                            cd.call, decodedtext.frequencyOffset(),
                            decodedtext.submode(), inMyGroup);
                    }
                    cd.grid = decodedtext.extra(); // compound calls via pings
                                                   // may contain grid...
                    cd.snr = decodedtext.snr();
                    cd.dial = freq;
                    cd.offset = decodedtext.frequencyOffset();
                    cd.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
                    cd.bits = decodedtext.bits();
                    cd.submode = decodedtext.submode();
                    cd.tdrift = (cd.submode == Varicode::JS8CallFT2)
                                    ? 0.0
                                    : m_wideGraph->shouldAutoSyncSubmode(d.submode)
                                          ? DriftingDateTime::drift() / 1000.0
                                          : decodedtext.dt();

                    // Only respond to HEARTBEATS...remember that CQ messages
                    // are "Alt" pings
                    if (decodedtext.isHeartbeat()) {
                        if (decodedtext.isAlt()) {
                            // this is a cq with a standard or compound call,
                            // ala "KN4CRD/P: @ALLCALL CQ CQ CQ"
                            cd.cqTimestamp =
                                DriftingDateTime::currentDateTimeUtc();

                            // convert CQ to a directed command and process...
                            cmd.from = cd.call;
                            cmd.to = "@ALLCALL";
                            cmd.cmd = " CQ";
                            cmd.snr = cd.snr;
                            cmd.bits = cd.bits;
                            cmd.grid = cd.grid;
                            cmd.dial = cd.dial;
                            cmd.offset = cd.offset;
                            cmd.utcTimestamp = cd.utcTimestamp;
                            cmd.tdrift = cd.tdrift;
                            cmd.submode = cd.submode;
                            cmd.text = decodedtext.message();

                            // TODO: check bits so we only auto respond to
                            // "finished" cqs
                            m_rxCommandQueue.append(cmd);

                            // Persist the CQ-bearing CallDetail so cqTimestamp
                            // reaches displayCallActivity (drives the 5-min
                            // green-row + telephone icon). The queued cmd
                            // path doesn't carry cqTimestamp, so without this
                            // call the marker is set on cd here and then lost
                            // when cd falls out of scope. Build 162.
                            logCallActivity(cd, true);

                            // notification for cq
                            tryNotify("cq", cd.submode);

                        } else {
                            // convert HEARTBEAT to a directed command and
                            // process...
                            cmd.from = cd.call;
                            cmd.to = "@HB";
                            cmd.cmd = " HEARTBEAT";
                            cmd.snr = cd.snr;
                            cmd.bits = cd.bits;
                            cmd.grid = cd.grid;
                            cmd.dial = cd.dial;
                            cmd.offset = cd.offset;
                            cmd.utcTimestamp = cd.utcTimestamp;
                            cmd.tdrift = cd.tdrift;
                            cmd.submode = cd.submode;

                            // TODO: check bits so we only auto respond to
                            // "finished" heartbeats
                            m_rxCommandQueue.append(cmd);

                            // notification for hb
                            tryNotify("hb", cd.submode);
                        }

                    } else {
                        qCDebug(mainwindow_js8)
                            << "buffering compound call" << cd.offset << cd.call
                            << cd.bits;

                        hasExistingMessageBuffer(cd.submode, cd.offset, true,
                                                 nullptr);
                        m_messageBuffer[cd.offset].compound.append(cd);
                    }
                }
#endif

            // Parse commands
            // KN4CRD K1JT ?
#if 1
                bool shouldProcessDirected = true;
                if (shouldProcessDirected && decodedtext.isDirectedMessage()) {
                    auto parts = decodedtext.directedMessage();

                    cmd.from = parts.at(0);
                    // Resolve FROM/TO from parts + buffered compound
                    // based on actual observed wire layout (probed
                    // 2026-05-18):
                    //   FrameDirected:         [FROM, TO, CMD, ...]
                    //     — both are real callsigns / basecalls.
                    //     Either may be "<....>" if it was sent in a
                    //     preceding FrameCompound (compound-TO case
                    //     not observed in practice — JS8 prefers
                    //     FrameCompoundDirected for that).
                    //   FrameCompoundDirected: [<....>, TO_compound, CMD, ...]
                    //     — this frame carries the compound TO.
                    //     The FROM is in a preceding FrameCompound
                    //     buffered for this offset.
                    // isUpgrade is true when the compound branch
                    // already painted the FROM (any time we resolve
                    // it from the buffer).
                    QString annotateFrom;
                    QString annotateTo;
                    bool annotateIsUpgrade = false;
                    if (decodedtext.frameType() ==
                        Varicode::FrameCompoundDirected) {
                        annotateTo = (parts.size() > 1)
                                         ? parts.at(1)
                                         : QString();
                        auto const bufIt = m_messageBuffer.find(
                            decodedtext.frequencyOffset());
                        if (bufIt != m_messageBuffer.end() &&
                            !bufIt.value().compound.isEmpty()) {
                            annotateFrom =
                                bufIt.value().compound.last().call;
                            annotateIsUpgrade = true;
                        } else {
                            annotateFrom = "<....>";
                        }
                    } else {
                        annotateFrom = cmd.from;
                        annotateTo = parts.at(1);
                        bool const fromCompound = (annotateFrom == "<....>");
                        bool const toCompound = (annotateTo == "<....>");
                        if (fromCompound || toCompound) {
                            auto const bufIt = m_messageBuffer.find(
                                decodedtext.frequencyOffset());
                            if (bufIt != m_messageBuffer.end() &&
                                !bufIt.value().compound.isEmpty()) {
                                auto const &cmps = bufIt.value().compound;
                                if (fromCompound && toCompound &&
                                    cmps.size() >= 2) {
                                    annotateFrom = cmps.first().call;
                                    annotateTo = cmps.last().call;
                                } else if (fromCompound) {
                                    annotateFrom = cmps.last().call;
                                } else if (toCompound) {
                                    annotateTo = cmps.last().call;
                                }
                                annotateIsUpgrade = fromCompound;
                            }
                        }
                    }
                    if (!annotateFrom.isEmpty() &&
                        annotateFrom != "<....>") {
                        // @ALLCALL is too broad to ever flag as a
                        // user-group destination, regardless of
                        // MyGroups contents.
                        bool const inMyGroup =
                            (annotateTo != "@ALLCALL") &&
                            isGroupCallIncluded(annotateTo);
                        bool const destIsSubspaceGroup =
                            (annotateTo == "@SUBSPACE");
                        // Skip the repaint when the compound branch
                        // already painted the same FROM (isUpgrade)
                        // and the directed branch's color matches.
                        // Compound paints with inMyGroup=false and
                        // destIsSubspaceGroup=false, producing black
                        // bg + (yellow if Subspace else white) text.
                        // Directed matches iff both flags are also
                        // false here.
                        bool const colorsMatchCompound =
                            annotateIsUpgrade && !inMyGroup &&
                            !destIsSubspaceGroup;
                        if (!colorsMatchCompound) {
                            m_wideGraph->setCallsignOverlayEnabled(
                                m_config.show_calls_on_waterfall());
                            m_wideGraph->annotateCall(
                                annotateFrom, decodedtext.frequencyOffset(),
                                decodedtext.submode(), inMyGroup,
                                destIsSubspaceGroup, annotateIsUpgrade);
                        }
                    }
                    cmd.to = parts.at(1);
                    cmd.cmd = parts.at(2);
                    cmd.dial = freq;
                    cmd.offset = decodedtext.frequencyOffset();
                    cmd.snr = decodedtext.snr();
                    cmd.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
                    cmd.bits = decodedtext.bits();
                    cmd.extra =
                        parts.length() > 2 ? parts.mid(3).join(" ") : "";
                    cmd.submode = decodedtext.submode();
                    cmd.tdrift = (cmd.submode == Varicode::JS8CallFT2)
                                     ? 0.0
                                     : m_wideGraph->shouldAutoSyncSubmode(cmd.submode)
                                           ? DriftingDateTime::drift() / 1000.0
                                           : decodedtext.dt();

                    // if the command is a buffered command and its not the last
                    // frame OR we have from or to in a separate message
                    // (compound call)
                    if ((Varicode::isCommandBuffered(cmd.cmd) &&
                         (cmd.bits & Varicode::JS8CallLast) !=
                             Varicode::JS8CallLast) ||
                        cmd.from == "<....>" || cmd.to == "<....>") {
                        qCDebug(mainwindow_js8)
                            << "buffering cmd" << cmd.dial << cmd.offset
                            << cmd.cmd << cmd.from << cmd.to;

                        // log complete buffered callsigns immediately
                        if (cmd.from != "<....>" && cmd.to != "<....>") {
                            CallDetail cmdcd = {};
                            cmdcd.call = cmd.from;
                            cmdcd.bits = cmd.bits;
                            cmdcd.snr = cmd.snr;
                            cmdcd.dial = cmd.dial;
                            cmdcd.offset = cmd.offset;
                            cmdcd.utcTimestamp = cmd.utcTimestamp;
                            cmdcd.ackTimestamp =
                                cmd.to == m_config.my_callsign()
                                    ? cmd.utcTimestamp
                                    : QDateTime{};
                            cmdcd.tdrift = cmd.tdrift;
                            cmdcd.submode = cmd.submode;
                            logCallActivity(cmdcd, false);
                            logHeardGraph(cmd.from, cmd.to);
                        }

                        // merge any existing buffer to this frequency
                        hasExistingMessageBuffer(cmd.submode, cmd.offset, true,
                                                 nullptr);

                        if (cmd.to == m_config.my_callsign()) {
                            d.shouldDisplay = true;
                        }

                        m_messageBuffer[cmd.offset].cmd = cmd;
                        m_messageBuffer[cmd.offset].msgs.clear();
                    } else {
                        m_rxCommandQueue.append(cmd);
                    }

                    // check to see if this is a station we've heard 3rd party
                    bool shouldCaptureThirdPartyCallsigns = false;
                    if (shouldCaptureThirdPartyCallsigns &&
                        Radio::base_callsign(cmd.to) !=
                            Radio::base_callsign(m_config.my_callsign())) {
                        QString relayCall =
                            QString("%1|%2")
                                .arg(Radio::base_callsign(cmd.from))
                                .arg(Radio::base_callsign(cmd.to));
                        int snr = -100;
                        if (parts.length() == 4) {
                            snr = QString(parts.at(3)).toInt();
                        }

                        // CallDetail td = {};
                        td.through = cmd.from;
                        td.call = cmd.to;
                        td.grid = "";
                        td.snr = snr;
                        td.dial = cmd.dial;
                        td.offset = cmd.offset;
                        td.utcTimestamp = cmd.utcTimestamp;
                        td.tdrift = cmd.tdrift;
                        td.submode = cmd.submode;
                        logCallActivity(td, true);
                        logHeardGraph(cmd.from, cmd.to);
                    }
                }
#endif
            }
        },
        event);
}
