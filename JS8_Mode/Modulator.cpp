/**
 * @file Modulator.cpp
 * @brief Implementation of Modulator class
 */
#include "Modulator.h"
#include "JS8Submode.h"
#include "JS8_Audio/SoundOutput.h"
#include "JS8_Include/commons.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_UI/mainwindow.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QtMath>

#include <algorithm>
#include <limits>
#include <numbers>

#include "moc_Modulator.cpp"

Q_DECLARE_LOGGING_CATEGORY(modulator_js8)

namespace {
constexpr double TAU = 2 * std::numbers::pi;
constexpr auto FRAME_RATE = 48000;
constexpr auto MS_PER_SEC = 1000;
} // namespace

/**
 * @brief Start the modulation process
 *
 * @param frequency
 * @param submode
 * @param txDelay
 * @param stream
 * @param channel
 */
void Modulator::start(double const frequency, int const submode,
                      double const txDelay, SoundOutput *const stream,
                      Channel const channel) {
    Q_ASSERT(stream);

    ++m_txCycleCount;
    const State current_state = m_state.load();
    qWarning() << "[FT2-TX] Modulator::start() cycle#" << m_txCycleCount
               << "freq=" << frequency
               << "submode=" << submode << "state=" << (int)current_state
               << "tuning=" << m_tuning;
    if (current_state != State::Idle && current_state != State::KeepAlive) {
        qWarning() << "[FT2-TX] Modulator not idle/keepalive, SKIPPING duplicate start"
                    << "cycle#" << m_txCycleCount;
        return;
    }

    m_quickClose = false;
    m_audioFrequency = frequency;
#ifdef JS8_ENABLE_FT2
    if (submode == 16 /* Varicode::JS8CallFT2 */ && !m_tuning) {
        m_ft2Mode = true;
        m_ft2Wave = ft2_txwave;
        m_ft2WaveLen = ft2_txwave_len;
        m_nsps = FT2_TX_NSPS;  // 1152 at 48kHz
        m_toneSpacing = 12000.0 / FT2_NSPS;  // ~41.67 Hz
    } else {
        m_ft2Mode = false;
        m_nsps = JS8::Submode::samplesForOneSymbol(submode);
        m_toneSpacing = JS8::Submode::toneSpacing(submode);
    }
#else
    m_nsps = JS8::Submode::samplesForOneSymbol(submode);
    m_toneSpacing = JS8::Submode::toneSpacing(submode);
#endif
    m_isym0 = std::numeric_limits<unsigned>::max();
    m_amp = std::numeric_limits<qint16>::max();
    m_audioFrequency0 = 0.0;
    m_phi = 0.0;
    m_silentFrames = 0;
    m_ic = 0;

    // If we're not tuning, then we'll need to figure out exactly when we
    // should start transmitting; this will depend on the submode in play.

    if (!m_tuning) {
        // Get the nominal transmit start time for this submode, and determine
        // which millisecond of the current transmit period we're currently at.

        qint64 const nowMS = DriftingDateTime::currentMSecsSinceEpoch();
        unsigned const periodMS = JS8::Submode::periodMS(submode);
        auto const startDelayMS = JS8::Submode::startDelayMS(submode);
        unsigned const periodOffsetMS = nowMS % periodMS;

        // If we haven't yet hit the nominal start time for the period, then we
        // will need to inject some silence into the transmission; determine the
        // number of silent audio samples required to start audio at the correct
        // amount of delay into the period.
        //
        // If we have hit the nominal start time for the period, adjust for late
        // start if we're not exactly at the nominal start time.

        bool const inTxDelayBeforePeriodStart =
            periodMS <= periodOffsetMS + txDelay * MS_PER_SEC;
        if (inTxDelayBeforePeriodStart) {
            unsigned const additionalMSNeededForTxDelay =
                periodMS - periodOffsetMS;
            m_silentFrames = (startDelayMS + additionalMSNeededForTxDelay) *
                             FRAME_RATE / MS_PER_SEC;
            qWarning() << "[FT2-TX] Modulator timing: TX-DELAY path"
                        << "periodOffsetMS=" << periodOffsetMS
                        << "additionalMS=" << additionalMSNeededForTxDelay
                        << "silentFrames=" << m_silentFrames
                        << "silentMS=" << (m_silentFrames * 1000 / FRAME_RATE);
        } else if (startDelayMS > periodOffsetMS) {
            m_silentFrames =
                (startDelayMS - periodOffsetMS) * FRAME_RATE / MS_PER_SEC;
            qWarning() << "[FT2-TX] Modulator timing: EARLY-START path"
                        << "periodOffsetMS=" << periodOffsetMS
                        << "startDelayMS=" << startDelayMS
                        << "silentFrames=" << m_silentFrames
                        << "silentMS=" << (m_silentFrames * 1000 / FRAME_RATE);
        } else {
            // Too late in the current period to start cleanly.
            // Wait for the next period boundary + startDelay instead of
            // cutting away initial symbols (which breaks GFSK sync).
            unsigned const msToNextBoundary = periodMS - periodOffsetMS;
            m_silentFrames = (msToNextBoundary + startDelayMS) *
                             FRAME_RATE / MS_PER_SEC;
            qWarning() << "[FT2-TX] Modulator timing: WAIT-NEXT-PERIOD path"
                        << "periodOffsetMS=" << periodOffsetMS
                        << "startDelayMS=" << startDelayMS
                        << "msToNextBoundary=" << msToNextBoundary
                        << "silentFrames=" << m_silentFrames
                        << "silentMS=" << (m_silentFrames * 1000 / FRAME_RATE);
        }
    } else {
        qCDebug(modulator_js8) << "Modulator finds it is tuning.";
    }


#ifndef Q_OS_WIN
    if (current_state == State::KeepAlive && m_stream && isOpen()
        && m_stream->isStreaming()) {
        // Warm restart (Linux/macOS): stream is still running and pulling.
        // Just switch state — readData() will produce waveform on next call.
        if (0 < m_silentFrames) {
            m_state.store(State::Synchronizing);
        } else {
            m_state.store(State::Active);
        }
        m_stream = stream;
        qWarning() << "[FT2-TX] Modulator::start() warm restart from KeepAlive"
                    << "cycle#" << m_txCycleCount
                    << "ft2Mode=" << m_ft2Mode
                    << "state=" << (int)m_state.load();
    } else
#endif
    {
        // Cold start (always on Windows, fallback on Linux/macOS).
        // On Windows, warm restart is unreliable — the FFmpeg/WASAPI
        // backend can silently stop pulling after Idle→Stopped transitions.
        // Cold restart via m_stream->restart() is cheap (~20ms) because
        // SoundOutput reuses the existing QAudioSink on Windows.
        if (current_state == State::KeepAlive) {
            qWarning() << "[FT2-TX] Modulator::start() cold restart from KeepAlive"
                        << "cycle#" << m_txCycleCount
                        << "streaming=" << (m_stream ? m_stream->isStreaming() : false)
                        << "isOpen=" << isOpen();
            m_state.store(State::Idle);
        }
        // Cold start: initialize device and start stream.
        initialize(QIODevice::ReadOnly, channel);

        if (0 < m_silentFrames) {
            m_state.store(State::Synchronizing);
            qCDebug(modulator_js8)
                << "Symbol transmission to start after"
                << ((float)m_silentFrames) / FRAME_RATE * MS_PER_SEC
                << "ms of silence.";
        } else {
            m_state.store(State::Active);
            qCDebug(modulator_js8) << "Symbol transmission to start immediately.";
        }

        m_stream = stream;
        if (m_stream) {
            qWarning() << "[FT2-TX] Modulator::start() calling m_stream->restart()"
                        << "cycle#" << m_txCycleCount
                        << "ft2Mode=" << m_ft2Mode
                        << "ft2WaveLen=" << m_ft2WaveLen
                        << "state=" << (int)m_state.load();
            m_stream->restart(this);
        } else {
            qWarning() << "[FT2-TX] Modulator::start: NO audio output stream!";
        }
    }
}

/**
 * @brief Set tuning mode
 *
 * @param tuning
 */
void Modulator::tune(bool const tuning) {
    m_tuning = tuning;
    if (!m_tuning)
        stop(true);
}

/**
 * @brief Stop the modulation process
 *
 * @param quickClose
 */
void Modulator::stop(bool const quickClose) {
    qWarning() << "[FT2-TX] Modulator::stop() quickClose=" << quickClose
               << "cycle#" << m_txCycleCount
               << "state=" << (int)m_state.load();
    if (quickClose) {
        // Hard stop: kill the stream immediately (tune cancel, etc.)
        m_quickClose = true;
        close();  // sets Idle, stops stream
        return;
    }
    auto s = m_state.load();
    if (s == State::Active || s == State::Synchronizing) {
        // Soft stop from active TX: transition to KeepAlive.
        // Stream stays alive, readData() feeds silence.
        m_state.store(State::KeepAlive);
    }
    // If already Idle or KeepAlive, nothing to do.
}

/**
 * @brief Close the modulator
 *
 */
void Modulator::close() {
    if (m_stream) {
        if (m_quickClose)
            m_stream->reset();
        else
            m_stream->stop();
    }

    m_state.store(State::Idle);
    AudioDevice::close();
}

/**
 * @brief Read data from the modulator
 *
 * @param data
 * @param maxSize
 * @return qint64
 */
qint64 Modulator::readData(char *const data, qint64 const maxSize) {
    if (maxSize == 0)
        return 0;

    Q_ASSERT(!(maxSize % qint64(bytesPerFrame()))); // no torn frames
    Q_ASSERT(isOpen());

    qint64 framesGenerated = 0;
    qint64 const maxFrames = maxSize / bytesPerFrame();
    qint16 *samples = reinterpret_cast<qint16 *>(data);
    qint16 const *const samplesEnd =
        samples + maxFrames * (bytesPerFrame() / sizeof(qint16));

    switch (m_state.load()) {
    case State::Synchronizing: {
        if (m_silentFrames) {
            // Send silence up to end of start delay.

            framesGenerated = qMin(m_silentFrames, maxFrames);

            do {
                samples = load(0, samples);
            } while (--m_silentFrames && samples != samplesEnd);

            if (!m_silentFrames) {
                qWarning() << "[FT2-TX] Modulator: Synchronizing→Active"
                            << "cycle#" << m_txCycleCount
                            << "ft2Mode=" << m_ft2Mode;
                m_state.store(State::Active);
            }
        }
    }
        [[fallthrough]];

    case State::Active: {
#ifdef JS8_ENABLE_FT2
        if (m_ft2Mode && m_ft2Wave && m_ft2WaveLen > 0) {
            // FT2: play back pre-generated GFSK waveform
            if (m_ic == 0)
                qWarning() << "[FT2-TX] readData: starting waveform, cycle#"
                           << m_txCycleCount << "len="
                           << m_ft2WaveLen << "first sample="
                           << m_ft2Wave[0] << m_ft2Wave[1] << m_ft2Wave[2]
                           << "maxSize=" << maxSize
                           << "bytesPerFrame=" << bytesPerFrame();
            while (samples != samplesEnd &&
                   m_ic < static_cast<unsigned>(m_ft2WaveLen)) {
                auto sample = static_cast<qint16>(std::clamp(
                    static_cast<double>(m_ft2Wave[m_ic]) * 32767.0,
                    -32767.0, 32767.0));
                samples = load(sample, samples);
                ++framesGenerated;
                ++m_ic;
                if (m_ic == 2000)
                    qWarning() << "[FT2-TX] readData: mid-waveform sample[2000]"
                               << "float=" << m_ft2Wave[2000]
                               << "qint16=" << sample;
            }
            // After waveform ends, stay Active and feed silence until
            // stop() is called by stopTx(). Don't set State::Idle here —
            // that causes readData to return 0, triggering QAudioSink
            // UnderrunError which kills audio after a few cycles.
            // Fall through to silence padding below.
        } else
#endif
        {
        // JS8: Fade out parameters; no fade out during tuning.

        unsigned int const i0 =
            (m_tuning ? 9999 : (JS8_NUM_SYMBOLS - 0.017) * 4.0) * m_nsps;
        unsigned int const i1 =
            (m_tuning ? 9999 : JS8_NUM_SYMBOLS * 4.0) * m_nsps;

        while (samples != samplesEnd && m_ic < i1) {
            unsigned int const isym = m_tuning ? 0 : m_ic / (4.0 * m_nsps);

            if (isym != m_isym0 || m_audioFrequency != m_audioFrequency0) {
                double const toneFrequency =
                    m_audioFrequency + itone[isym] * m_toneSpacing;

                m_dphi = TAU * toneFrequency / FRAME_RATE;
                m_isym0 = isym;
                m_audioFrequency0 = m_audioFrequency;
            }

            m_phi += m_dphi;

            if (m_phi > TAU)
                m_phi -= TAU;
            if (m_ic > i0)
                m_amp = 0.98 * m_amp;
            if (m_ic > i1)
                m_amp = 0.0;

            samples = load(qRound(m_amp * qSin(m_phi)), samples);

            ++framesGenerated;
            ++m_ic;
        }

        if (m_amp == 0.0) {
            m_state.store(State::KeepAlive);
            m_phi = 0.0;
            if (m_ft2Mode)
                emit ft2WaveformDone();
            return framesGenerated * bytesPerFrame();
        }
        } // end JS8 else block

        m_audioFrequency0 = m_audioFrequency;

        // Done for this chunk; continue on the next call. Pad the
        // block with silence.

        while (samples != samplesEnd) {
            samples = load(0, samples);
            ++framesGenerated;
        }

        return framesGenerated * bytesPerFrame();
    }
        [[fallthrough]];

    case State::KeepAlive:
        // Feed near-zero samples to keep USB audio codec awake.
        // Actual zeros cause Qt/PipeWire to treat stream as empty
        // and transition to Idle→Stopped. Amplitude 1 is inaudible
        // (-90 dBFS) but keeps the sink in Active state.
        while (samples != samplesEnd) {
            samples = load(1, samples);
            ++framesGenerated;
        }
        return framesGenerated * bytesPerFrame();

    case State::Idle:
        break;
    }

    Q_ASSERT(isIdle());
    return 0;
}

Q_LOGGING_CATEGORY(modulator_js8, "modulator.js8", QtWarningMsg)
