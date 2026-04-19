/**
 * @file SoundOutput.cpp
 * @brief Checks for audio errors and emits appropriate error messages.
 * @return true if no error, false otherwise
 */

#include "SoundOutput.h"

#include <QAudioOutput>
#include <QDateTime>
#include <QLoggingCategory>
#include <QSysInfo>
#include <qmath.h>

#include "moc_SoundOutput.cpp"

Q_DECLARE_LOGGING_CATEGORY(soundout_js8)

bool SoundOutput::checkStream() const {
    bool result{false};

    Q_ASSERT_X(m_stream, "SoundOutput", "programming error");
    if (m_stream) {
        switch (m_stream->error()) {
        case QAudio::OpenError:
            Q_EMIT error(
                tr("An error opening the audio output device has occurred."));
            break;

        case QAudio::IOError:
            Q_EMIT error(tr(
                "An error occurred during write to the audio output device."));
            break;

        case QAudio::UnderrunError:
            // Non-fatal: recover by resuming the stream instead of erroring out.
            qWarning() << "SoundOutput: underrun detected, recovering...";
            if (m_stream && (m_stream->state() == QAudio::IdleState
                          || m_stream->state() == QAudio::SuspendedState)) {
                m_stream->resume();
            }
            result = true;  // non-fatal, keep going
            break;

        case QAudio::FatalError:
            Q_EMIT error(tr("Non-recoverable error, audio output device not "
                            "usable at this time."));
            break;

        case QAudio::NoError:
            result = true;
            break;
        }
    }
    return result;
}

/**
 * @brief Sets the audio format based on the device and channel count.
 * @param device The QAudioDevice to use.
 * @param channels The number of audio channels (1 for mono, 2 for stereo).
 * @param msBuffered The buffer size in milliseconds.
 */
void SoundOutput::setFormat(QAudioDevice const &device, unsigned channels,
                            unsigned msBuffered) {

    Q_ASSERT(0 < channels && channels < 3);

    QAudioFormat format(device.preferredFormat());

    format.setChannelCount(channels);
    format.setSampleRate(48000);
    format.setSampleFormat(QAudioFormat::Int16);

    setDeviceFormat(device, format, msBuffered);
}

/**
 * @brief Sets the audio device and format.
 * @param device The QAudioDevice to use.
 * @param format The QAudioFormat to set.
 * @param msBuffered The buffer size in milliseconds.
 */
void SoundOutput::setDeviceFormat(QAudioDevice const &device,
                                  QAudioFormat const &format,
                                  unsigned msBuffered) {
    if (!format.isValid()) {
        Q_EMIT error(tr("Requested output audio format is not valid."));
        return;
    }
    if (!device.isFormatSupported(format)) {
        Q_EMIT error(
            tr("Requested output audio format is not supported on device."));
        return;
    }

    // Race guard: if device/format/buffer are unchanged and we already
    // have a healthy sink, skip the destroy-and-rebuild. NotificationAudio
    // calls this on every play, and rapid plays (e.g. user mashing the
    // test button in Configuration > Notifications) destroy a still-active
    // QAudioSink mid-stream, crashing Qt6Multimedia at offset 0xea4f.
    bool sameParams = (m_device == device && m_format == format &&
                       m_msBuffered == msBuffered && m_stream);
    if (sameParams) {
        return;
    }

    m_device = device;
    m_format = format;
    m_msBuffered = msBuffered;

    // Pre-create the QAudioSink so audio subsystem initialization happens
    // at startup, not on the first TX frame. Without this, the first audio
    // chunk can be dropped due to WASAPI (Windows) or PipeWire (Linux)
    // initialization latency when the device is cold.
    if (!m_device.isNull()) {
        if (m_stream) {
            m_stream->disconnect(this);
            m_stream->reset();
            m_stream->stop();
        }
        m_stream.reset(new QAudioSink(m_device, m_format));
        checkStream();
        m_error = false;
        connect(m_stream.data(), &QAudioSink::stateChanged, this,
                &SoundOutput::handleStateChanged);
        qWarning() << "[FT2-TX] SoundOutput: pre-initializing audio sink";
    }
}

/**
 * @brief Restarts audio output with the specified source.
 * @param source The QIODevice to read audio data from.
 */
void SoundOutput::restart(QIODevice *source) {
    // Race guard: if we're already streaming the same source and the sink
    // is healthy, ignore re-entrant restart() calls. Multiple GUI-thread
    // emits of sendMessage can land back-to-back on the audio thread queue
    // and trigger restart() much more often than there are real TX cycles
    // (WD4KAV captured 13 restarts in ~12 seconds — JS8 should produce
    // 3-4 in that window). Each redundant restart destroys-and-rebuilds
    // (or stop+starts) the QAudioSink, eventually corrupting the Windows
    // audio backend and aborting the process.
    if (m_stream && m_source == source &&
        (m_stream->state() == QAudio::ActiveState ||
         m_stream->state() == QAudio::IdleState)) {
        qWarning() << "[FT2-TX] SoundOutput::restart() skipped (already streaming"
                   << "same source, state=" << (int)m_stream->state() << ")";
        return;
    }

    qWarning() << "[FT2-TX] SoundOutput::restart() source=" << source
               << "isOpen=" << (source ? source->isOpen() : false)
               << "hasStream=" << (m_stream != nullptr);

    if (!m_device.isNull()) {
#ifdef Q_OS_WIN
        // On Windows, reuse the existing QAudioSink to avoid WASAPI
        // initialization latency that drops the first audio chunk.
        // Use stop()+start() to reset the stream without destroying
        // the IAudioClient.
        if (m_stream) {
            m_stream->reset();
            m_stream->stop();
        } else {
            // Fallback: create sink if setDeviceFormat() warmup didn't run
            m_stream.reset(new QAudioSink(m_device, m_format));
            checkStream();
            m_error = false;
            connect(m_stream.data(), &QAudioSink::stateChanged, this,
                    &SoundOutput::handleStateChanged);
        }
#else
        // On Linux, always create a fresh QAudioSink. Reusing via
        // stop()+start() causes Active→Idle oscillation on Qt 6.4 /
        // PipeWire after several cycles.
        if (m_stream) {
            m_stream->disconnect(this);
            m_stream->reset();
            m_stream->stop();
        }
        m_stream.reset(new QAudioSink(m_device, m_format));
        qCDebug(soundout_js8)
            << "SoundOutput::restart Selected audio output format:"
            << m_stream->format();
        checkStream();
        m_error = false;
        connect(m_stream.data(), &QAudioSink::stateChanged, this,
                &SoundOutput::handleStateChanged);
#endif
    }

    if (!m_stream) {
        if (!m_error) {
            m_error = true; // only signal error once
            Q_EMIT error(tr("No audio output device configured."));
        }
        return;
    } else {
        m_error = false;
    }

    m_stream->setVolume(m_volume);

    // we have to set this before every start on the stream because the
    // Windows implementation seems to forget the buffer size after a
    // stop.
    if (m_msBuffered > 0) {
        m_stream->setBufferSize(
            m_stream->format().bytesForDuration(m_msBuffered));
    }

    m_source = source;
    m_stream->start(source);
}

/**
 * @brief Suspends audio output.
 */
void SoundOutput::suspend() {
    if (m_stream && QAudio::ActiveState == m_stream->state()) {
        m_stream->suspend();
        checkStream();
    }
}

/**
 * @brief Resumes audio output.
 */
void SoundOutput::resume() {
    if (m_stream && QAudio::SuspendedState == m_stream->state()) {
        m_stream->resume();
        checkStream();
    }
}

/**
 * @brief Resets the audio output.
 */
void SoundOutput::reset() {
    if (m_stream) {
        m_stream->reset();
        checkStream();
    }
}

/**
 * @brief Stops audio output.
 */
void SoundOutput::stop() {
    if (m_stream) {
        m_stream->reset();
        m_stream->stop();
    }
    // m_stream.reset ();  // XXX in WSJTX, seems like a bug
    // On Windows, the sink is intentionally kept alive (not destroyed)
    // to avoid WASAPI re-initialization on the next start().
}

/**
 * @brief Gets the current attenuation in decibels.
 * @return The attenuation value.
 */
qreal SoundOutput::attenuation() const {
    return -(20. * qLn(m_volume) / qLn(10.));
}

/**
 * @brief Gets the current audio format.
 * @return The QAudioFormat object.
 */
QAudioFormat SoundOutput::format() const { return m_format; }

bool SoundOutput::isStreaming() const {
    return m_stream && m_stream->state() != QAudio::StoppedState;
}

/**
 * @brief Sets the attenuation in decibels.
 * @param a The attenuation value.
 */
void SoundOutput::setAttenuation(qreal a) {
    Q_ASSERT(0. <= a && a <= 999.);
    m_volume = qPow(10.0, -a / 20.0);
    // qCDebug (soundout_js8) << "SoundOut: attn = " << a << ", vol = " <<
    // m_volume;
    if (m_stream) {
        m_stream->setVolume(m_volume);
    }
}

/**
 * @brief Resets the attenuation to zero.
 */
void SoundOutput::resetAttenuation() {
    m_volume = 1.;
    if (m_stream) {
        m_stream->setVolume(m_volume);
    }
}

/**
 * @brief Handles state changes of the audio output.
 * @param newState The new state of the audio output.
 */
void SoundOutput::handleStateChanged(QAudio::State newState) const {
    const char *stateName = "Unknown";
    switch (newState) {
    case QAudio::IdleState:   stateName = "Idle";      break;
    case QAudio::ActiveState: stateName = "Active";    break;
    case QAudio::SuspendedState: stateName = "Suspended"; break;
    case QAudio::StoppedState:   stateName = "Stopped";   break;
    }
    qWarning() << "[FT2-TX] SoundOutput state:" << stateName;
    if (m_stream) {
        auto err = m_stream->error();
        if (err != QAudio::NoError)
            qWarning() << "[FT2-TX] SoundOutput error:" << (int)err;
    }

    switch (newState) {
    case QAudio::IdleState:
        // Wake the sink to prevent permanent underrun (QTBUG-108672).
        // When the Modulator is in KeepAlive, it has silence data available.
        // Emit readyRead() on the source to tell the sink to resume pulling.
        if (m_source)
            QMetaObject::invokeMethod(m_source, "readyRead");
        Q_EMIT status(tr("Idle"));
        break;
    case QAudio::ActiveState:
        Q_EMIT status(tr("Sending"));
        break;
    case QAudio::SuspendedState:
        Q_EMIT status(tr("Suspended"));
        break;
    case QAudio::StoppedState:
        if (!checkStream()) {
            Q_EMIT status(tr("Error"));
        } else {
            Q_EMIT status(tr("Stopped"));
        }
        break;
    }
}

Q_LOGGING_CATEGORY(soundout_js8, "soundout.js8", QtWarningMsg)
