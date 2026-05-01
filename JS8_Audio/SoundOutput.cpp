/**
 * @file SoundOutput.cpp
 * @brief miniaudio-based playback (Build 141, replaces QAudioSink)
 */
#include "SoundOutput.h"

#include <QCoreApplication>
#include <QIODevice>
#include <QLoggingCategory>
#include <cmath>
#include <cstring>

#include "moc_SoundOutput.cpp"

Q_DECLARE_LOGGING_CATEGORY(soundout_js8)

namespace {

// Resolve an AudioDeviceInfo to a concrete ma_device_id by enumerating
// playback devices. Same shape as SoundInput's resolver.
bool
resolvePlaybackDevice(AudioDeviceInfo const & info,
                      ma_device_id & outId)
{
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return false;

    ma_device_info * playInfos = nullptr;
    ma_uint32        playCount = 0;
    ma_device_info * capInfos  = nullptr;
    ma_uint32        capCount  = 0;
    bool             matched   = false;

    if (ma_context_get_devices(&ctx, &playInfos, &playCount, &capInfos, &capCount) == MA_SUCCESS) {
        QByteArray const wantedId   = info.id;
        QString    const wantedName = info.description;

        for (ma_uint32 i = 0; i < playCount; ++i) {
            QByteArray const thisId
                = QByteArray(reinterpret_cast<char const *>(&playInfos[i].id),
                             sizeof(ma_device_id)).toHex();
            QString const thisName = QString::fromUtf8(playInfos[i].name);

            bool const idMatch   = !wantedId.isEmpty() && wantedId == thisId;
            bool const nameMatch = !wantedName.isEmpty() && wantedName == thisName;

            if (idMatch || nameMatch) {
                outId = playInfos[i].id;
                matched = true;
                break;
            }
        }
    }

    ma_context_uninit(&ctx);
    return matched;
}

} // namespace

/**
 * @brief miniaudio playback callback. Runs on miniaudio's worker
 * thread, NOT on the GUI or audio QThread. Pulls from the bound
 * QIODevice source, applies attenuation, writes to the output.
 */
void
SoundOutput::s_dataCallback(ma_device * device,
                            void * pOutput,
                            void const * /* pInput */,
                            ma_uint32 frameCount)
{
    if (auto * self = static_cast<SoundOutput *>(device->pUserData)) {
        self->onPlayback(pOutput, frameCount);
    }
}

void
SoundOutput::onPlayback(void * pOutput, ma_uint32 frameCount)
{
    qint64 const totalBytes = m_format.bytesForFrames(frameCount);
    char *       out        = static_cast<char *>(pOutput);

    qint64 written = 0;
    if (m_source) {
        // QIODevice::read may return less than requested when the source
        // is idle / between frames; we pad the rest with silence.
        written = m_source->read(out, totalBytes);
        if (written < 0) written = 0;
    }
    if (written < totalBytes) {
        std::memset(out + written, 0, static_cast<size_t>(totalBytes - written));
    }

    qreal const volume = m_volume;
    if (volume != 1.0) {
        qint16 * samples = reinterpret_cast<qint16 *>(out);
        size_t const n   = static_cast<size_t>(totalBytes / sizeof(qint16));
        for (size_t i = 0; i < n; ++i) {
            samples[i] = static_cast<qint16>(samples[i] * volume);
        }
    }
}

void
SoundOutput::teardown()
{
    if (m_deviceInitialized) {
        ma_device_uninit(&m_device_ma);
        m_deviceInitialized = false;
    }
}

bool
SoundOutput::buildAndStart(QIODevice * source)
{
    teardown();

    if (m_device.isNull()) {
        if (!source) return false;  // nothing to do
        // Fall through anyway; miniaudio will pick the system default
        // when pDeviceID is null.
    }

    ma_device_id deviceId;
    bool const haveDevice = !m_device.isNull()
                          && resolvePlaybackDevice(m_device, deviceId);

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = haveDevice ? &deviceId : nullptr;
    cfg.playback.format    = ma_format_s16;
    cfg.playback.channels  = static_cast<ma_uint32>(m_format.channelCount);
    cfg.sampleRate         = static_cast<ma_uint32>(m_format.sampleRate);
    if (m_msBuffered > 0u) {
        cfg.periodSizeInMilliseconds = m_msBuffered;
    }
    cfg.dataCallback = &SoundOutput::s_dataCallback;
    cfg.pUserData    = this;

    if (ma_device_init(nullptr, &cfg, &m_device_ma) != MA_SUCCESS) {
        Q_EMIT error(tr("Failed to initialize audio output device."));
        return false;
    }
    m_deviceInitialized = true;
    m_source = source;

    if (ma_device_start(&m_device_ma) != MA_SUCCESS) {
        Q_EMIT error(tr("Failed to start audio output device."));
        teardown();
        m_source = nullptr;
        return false;
    }
    Q_EMIT status(tr("Sending"));
    return true;
}

/**
 * @brief Convenience overload that constructs an AudioFormat for the
 * given channel count at JS8Call's standard 48 kHz Int16.
 */
void
SoundOutput::setFormat(AudioDeviceInfo const & device,
                       unsigned channels,
                       unsigned msBuffered)
{
    Q_ASSERT(0 < channels && channels < 3);

    AudioFormat fmt;
    fmt.sampleRate   = 48000;
    fmt.channelCount = static_cast<int>(channels);
    fmt.sample       = AudioFormat::Int16;

    setDeviceFormat(device, fmt, msBuffered);
}

/**
 * @brief Stash the device + format. We do NOT init the ma_device here;
 * that happens in restart(source) once we have a source to bind. This
 * is a behavior change from the QAudioSink era's pre-create + warmup
 * pattern, but miniaudio's WASAPI backend doesn't have the cold-start
 * latency Qt6Multimedia did, so init-on-demand is fine.
 */
void
SoundOutput::setDeviceFormat(AudioDeviceInfo const & device,
                             AudioFormat const & format,
                             unsigned msBuffered)
{
    bool const sameParams = (m_device == device
                          && m_format == format
                          && m_msBuffered == msBuffered);
    if (sameParams && m_deviceInitialized) {
        return;
    }

    teardown();
    m_source     = nullptr;
    m_device     = device;
    m_format     = format;
    m_msBuffered = msBuffered;
}

/**
 * @brief Bind a source and (re)start the playback device.
 */
void
SoundOutput::restart(QIODevice * source)
{
    // Race guard: same source already streaming → no-op. Multiple
    // GUI-thread emits of sendMessage can land back-to-back on the
    // audio thread queue. Same intent as the prior QAudioSink code.
    if (m_deviceInitialized
        && m_source == source
        && ma_device_is_started(&m_device_ma)) {
        return;
    }
    buildAndStart(source);
}

void
SoundOutput::suspend()
{
    if (m_deviceInitialized && ma_device_is_started(&m_device_ma)) {
        ma_device_stop(&m_device_ma);
        Q_EMIT status(tr("Suspended"));
    }
}

void
SoundOutput::resume()
{
    if (m_deviceInitialized && !ma_device_is_started(&m_device_ma)) {
        if (ma_device_start(&m_device_ma) != MA_SUCCESS) {
            Q_EMIT error(tr("Failed to resume audio output device."));
        } else {
            Q_EMIT status(tr("Sending"));
        }
    }
}

/**
 * @brief Reset is treated as stop — the QAudioSink semantic of
 * "discard buffered data and idle the stream" maps cleanly onto
 * ma_device_stop. The next restart() will spin a fresh device.
 */
void
SoundOutput::reset()
{
    if (m_deviceInitialized && ma_device_is_started(&m_device_ma)) {
        ma_device_stop(&m_device_ma);
    }
}

void
SoundOutput::stop()
{
    teardown();
    m_source = nullptr;
    Q_EMIT status(tr("Stopped"));
}

qreal
SoundOutput::attenuation() const
{
    return -(20. * std::log(m_volume) / std::log(10.));
}

bool
SoundOutput::isStreaming() const
{
    return m_deviceInitialized && ma_device_is_started(&m_device_ma);
}

void
SoundOutput::setAttenuation(qreal a)
{
    Q_ASSERT(0. <= a && a <= 999.);
    m_volume = std::pow(10.0, -a / 20.0);
}

void
SoundOutput::resetAttenuation()
{
    m_volume = 1.0;
}

/**
 * @brief Destructor. ma_device_uninit is synchronous (joins the
 * worker), so no closingDown() leak path is needed — Builds 128's
 * "intentionally leak QAudioSink at process exit" workaround is gone.
 */
SoundOutput::~SoundOutput()
{
    teardown();
}

Q_LOGGING_CATEGORY(soundout_js8, "soundout.js8", QtWarningMsg)
