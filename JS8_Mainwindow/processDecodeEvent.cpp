

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

                qWarning() << "[DECODE-EVENT] processDecodeEvent received:"
                           << "snr=" << e.snr << "freq=" << e.frequency
                           << "mode=" << e.mode << "data=" << QString::fromStdString(e.data);

                DecodedText decodedtext(e);
                FrameCacheKey dedupeKey(decodedtext.submode(),
                                        decodedtext.frame());

                qWarning() << "[DECODE-EVENT] DecodedText: submode=" << decodedtext.submode()
                           << "frame=" << decodedtext.frame()
                           << "msg=" << decodedtext.message();

                if (auto const it = m_messageDupeCache.find(dedupeKey);
                    it != m_messageDupeCache.end()) {
                    auto ageSecs = it->second.secsTo(QDateTime::currentDateTimeUtc());
                    // FT2 L2: 90K buffer holds ~7.5s, same frame re-decodes for ~5s.
                    // Use 6s dedup window for FT2, half-period for other modes.
                    auto window = (decodedtext.submode() == Varicode::JS8CallFT2)
                        ? 6.0
                        : 0.5 * JS8::Submode::period(decodedtext.submode());
                    if (ageSecs < window) {
                        qWarning() << "[DECODE-EVENT] DUPLICATE, skipping frame=" << decodedtext.frame()
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
                        // TODO: incremental display if it's "to" me.
                    }

                    m_rxActivityQueue.append(d);
                    m_bandActivity[offset].append(d);
                    while (m_bandActivity[offset].count() > 10) {
                        m_bandActivity[offset].removeFirst();
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

                            // since this is no longer processed here we omit
                            // logging it here. if we change this behavior, we'd
                            // change this back to logging here.
                            // logCallActivity(cd, true);

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
