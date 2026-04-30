/**
 * @file soundin.cpp
 * @brief Implementation of SoundInput class
 */
#include "SoundInput.h"
#include "AudioTeardown.h"
#include "JS8_Main/DriftingDateTime.h"

#include <QAudioFormat>
#include <QCoreApplication>
#include <QLoggingCategory>
#include <QSysInfo>

#include "moc_SoundInput.cpp"

Q_DECLARE_LOGGING_CATEGORY(soundin_js8)

/**
 * @brief Checks for audio errors and emits appropriate error messages.
 *
 * @return true
 * @return false
 */
bool SoundInput::audioError() const {
    bool result(true);

    Q_ASSERT_X(m_stream, "SoundInput", "programming error");
    if (m_stream) {
        switch (m_stream->error()) {
        case QAudio::OpenError:
            Q_EMIT error(
                tr("An error opening the audio input device has occurred."));
            break;

        case QAudio::IOError:
            Q_EMIT error(tr(
                "An error occurred during read from the audio input device."));
            break;

        case QAudio::UnderrunError:
            // Non-fatal: input underruns happen on consumer hardware.
            // Logging only — do not emit error (which would kill the stream).
            qWarning() << "SoundInput: underrun detected, continuing...";
            result = false;
            break;

        case QAudio::FatalError:
            Q_EMIT error(tr("Non-recoverable error, audio input device not "
                            "usable at this time."));
            break;

        case QAudio::NoError:
            result = false;
            break;
        }
    }
    return result;
}

/**
 * @brief Starts audio input from the specified device.
 *
 * @param device The QAudioDevice to use for input.
 * @param framesPerBuffer The number of frames per buffer.
 * @param sink The AudioDevice sink to write audio data to.
 * @param channel The audio channel configuration (Mono or Stereo).
 */
void SoundInput::start(QAudioDevice const &device, int framesPerBuffer,
                       AudioDevice *sink, AudioDevice::Channel channel) {
    Q_ASSERT(sink);

    // Idempotent guard: if we're already streaming the same device/channel
    // into the same sink and the stream is healthy, do nothing. Prevents
    // the destroy-and-rebuild churn that happens when the GUI emits
    // startAudioInputStream more than once during init or settings reload.
    if (m_stream && m_sink == sink && m_lastDevice == device &&
        m_lastChannel == channel) {
        auto const s = m_stream->state();
        if (s == QAudio::ActiveState || s == QAudio::IdleState) {
            return;
        }
    }

    stop();

    m_sink = sink;
    m_lastDevice = device;
    m_lastChannel = channel;

    QAudioFormat format(device.preferredFormat());
    //  qCDebug (soundin_js8) << "Preferred audio input format:" << format;
    format.setSampleFormat(QAudioFormat::Int16);
    format.setChannelCount(AudioDevice::Mono == channel ? 1 : 2);
    format.setSampleRate(48000);
    if (!format.isValid()) {
        Q_EMIT error(tr("Requested input audio format is not valid."));
        return;
    }

    if (!device.isFormatSupported(format)) {
        //      qCDebug (soundin_js8) << "Nearest supported audio format:" <<
        //      device.nearestFormat (format);
        Q_EMIT error(
            tr("Requested input audio format is not supported on device."));
        return;
    }
    //  qCDebug (soundin_js8) << "Selected audio input format:" << format;

    if (m_stream) {
        m_stream->disconnect(this);
        m_stream->stop();
        waitForAudioStopped(m_stream.data(), "SoundInput::start");
        // Build 129: deleteLater() rather than in-line delete -- mirrors
        // Qt's QSoundEffect AudioSinkDeleter pattern. Defers the
        // destructor cascade past the next event-loop iteration so the
        // worker-thread teardown completes safely. Avoids the
        // Qt6Multimedia.dll+0x5c7f2 destructor-cascade fault.
        QAudioSource *old = m_stream.take();
        old->deleteLater();
        qWarning() << "[AudioTeardown] SoundInput::start "
                   << "deleteLater queued for previous source";
    }
    m_stream.reset(new QAudioSource{device, format});
    if (audioError()) {
        return;
    }

    connect(m_stream.data(), &QAudioSource::stateChanged, this,
            &SoundInput::handleStateChanged);

    m_stream->setBufferSize(m_stream->format().bytesForFrames(framesPerBuffer));
    if (sink->initialize(QIODevice::WriteOnly, channel)) {
        m_stream->start(sink);
        audioError();
    } else {
        Q_EMIT error(tr("Failed to initialize audio sink device"));
    }
}

/**
 * @brief Suspends audio input.
 */
void SoundInput::suspend() {
    if (m_stream) {
        // Stop instead of suspend — more reliable on Linux and macOS
        m_stream->stop();
        audioError();
    }
}

/**
 * @brief Resumes audio input.
 */
void SoundInput::resume() {
    if (m_sink) {
        m_sink->reset();
    }

    if (m_stream) {
        // Skip if the stream is already running. Prevents the
        // "QAudioSource::start() called while already started" warning
        // (and the Windows audio backend instability that warning
        // sometimes correlates with) when monitor(true) fires shortly
        // after a fresh start() that is already streaming.
        auto const s = m_stream->state();
        if (s == QAudio::ActiveState || s == QAudio::IdleState) {
            return;
        }
        // Restart instead of resume — more reliable on Linux and macOS
        m_stream->start(m_sink);
        audioError();
    }
}

/**
 * @brief Handles state changes of the audio input.
 * @param newState The new state of the audio input.
 */
void SoundInput::handleStateChanged(QAudio::State newState) const {
    // qCDebug (soundin_js8) << "SoundInput::handleStateChanged: newState:" <<
    // newState;

    switch (newState) {
    case QAudio::IdleState:
        Q_EMIT status(tr("Idle"));
        break;

    case QAudio::ActiveState:
        Q_EMIT status(tr("Receiving"));
        break;

    case QAudio::SuspendedState:
        Q_EMIT status(tr("Suspended"));
        break;

    case QAudio::StoppedState:
        if (audioError()) {
            Q_EMIT status(tr("Error"));
        } else {
            Q_EMIT status(tr("Stopped"));
        }
        break;
    }
}

/**
 * @brief Stops audio input.
 */
void SoundInput::stop() {
    if (m_stream) {
        m_stream->disconnect(this);
        m_stream->stop();
        waitForAudioStopped(m_stream.data(), "SoundInput::stop");
        // Build 129: deleteLater() rather than in-line delete (see
        // SoundInput::start for rationale).
        QAudioSource *old = m_stream.take();
        old->deleteLater();
        qWarning() << "[AudioTeardown] SoundInput::stop "
                   << "deleteLater queued for source";
    }

    if (m_sink) {
        m_sink->close();
    }
}

/**
 * @brief Destructs the SoundInput object.
 *
 * At process exit, intentionally leak the QAudioSource to bypass
 * Qt6Multimedia's destructor cascade. See SoundOutput::~SoundOutput
 * and AudioTeardown.h for the rationale -- two minidumps captured
 * 2026-04-29/30 against Build 127 showed that even after
 * state==Stopped (wait completed cleanly), the QAudioSource
 * destructor itself can AV at Qt6Multimedia.dll+0x5c7f2. Process
 * is exiting; OS reclaims memory. Runtime stop() (called via
 * Configuration changes) keeps the destroy path.
 */
SoundInput::~SoundInput() {
    if (QCoreApplication::closingDown() && m_stream) {
        m_stream->disconnect(this);
        m_stream->stop();
        waitForAudioStopped(m_stream.data(), "SoundInput::~SoundInput");
        qWarning() << "[AudioTeardown] SoundInput::~SoundInput "
                   << "leaking QAudioSource at process exit "
                   << "(bypasses Qt6Multimedia destructor cascade)";
        Q_UNUSED(m_stream.take());
        if (m_sink) m_sink->close();
        return;
    }
    stop();
}

Q_LOGGING_CATEGORY(soundin_js8, "soundin.js8", QtWarningMsg)
