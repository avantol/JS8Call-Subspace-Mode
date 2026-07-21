/**
 * @file SoundInput.cpp
 * @brief miniaudio-based capture (Build 140, replaces QAudioSource)
 */
#include "SoundInput.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <cstring>

#include "moc_SoundInput.cpp"

Q_DECLARE_LOGGING_CATEGORY(soundin_js8)

namespace {

// Resolve an AudioDeviceInfo to a concrete ma_device_id by enumerating
// capture devices and matching on either the hex-encoded id (preferred,
// available once Configuration is on miniaudio in Build 142) or the
// human-readable description (the Build 140 bridge while Configuration
// still uses QMediaDevices). Returns true on match; on miss the caller
// should fall back to miniaudio's default capture device.
bool
resolveCaptureDevice(AudioDeviceInfo const & info,
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
        QByteArray const wantedId = info.id;
        QString    const wantedName = info.description;

        for (ma_uint32 i = 0; i < capCount; ++i) {
            QByteArray const thisId
                = QByteArray(reinterpret_cast<char const *>(&capInfos[i].id),
                             sizeof(ma_device_id)).toHex();
            QString const thisName = QString::fromUtf8(capInfos[i].name);

            bool const idMatch   = !wantedId.isEmpty() && wantedId == thisId;
            bool const nameMatch = !wantedName.isEmpty() && wantedName == thisName;

            if (idMatch || nameMatch) {
                outId = capInfos[i].id;
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
 * @brief miniaudio capture callback. Forwards to the SoundInput
 * instance whose ma_device this is. Runs on miniaudio's worker thread,
 * NOT on the GUI or audio QThread.
 */
void
SoundInput::s_dataCallback(ma_device * device,
                           void * /* pOutput */,
                           void const * pInput,
                           ma_uint32    frameCount)
{
    if (auto * self = static_cast<SoundInput *>(device->pUserData)) {
        self->onCapture(pInput, frameCount);
    }
}

void
SoundInput::onCapture(void const * pInput, ma_uint32 frameCount)
{
    // [TODO #108 keep-warm] Monitor-off discards samples here; the
    // device itself keeps running (see m_discarding in the header).
    if (m_discarding.load(std::memory_order_relaxed)) return;
    if (!m_sink || frameCount == 0) return;

    qint64 const bytes = static_cast<qint64>(frameCount) * m_sink->bytesPerFrame();
    m_sink->write(static_cast<char const *>(pInput), bytes);
}

/**
 * @brief Starts audio input from the specified device.
 */
void
SoundInput::start(AudioDeviceInfo const & device,
                  int                    framesPerBuffer,
                  AudioDevice *          sink,
                  AudioDevice::Channel   channel)
{
    Q_ASSERT(sink);

    // Idempotent guard: skip rebuild if the same device/channel/sink/buffer
    // is requested again and the stream is already running. Prevents the
    // destroy-and-rebuild churn that happens when the GUI emits
    // startAudioInputStream more than once during init or settings reload.
    if (m_deviceInitialized
        && m_sink == sink
        && m_lastDevice == device
        && m_lastChannel == channel
        && m_lastFramesPerBuffer == framesPerBuffer
        && ma_device_is_started(&m_device)) {
        return;
    }

    stop();

    m_sink = sink;
    m_lastDevice = device;
    m_lastChannel = channel;
    m_lastFramesPerBuffer = framesPerBuffer;

    if (!sink->initialize(QIODevice::WriteOnly, channel)) {
        Q_EMIT error(tr("Failed to initialize audio sink device"));
        return;
    }

    ma_device_id deviceId;
    bool const haveDevice = resolveCaptureDevice(device, deviceId);

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID = haveDevice ? &deviceId : nullptr;  // null → default
    cfg.capture.format    = ma_format_s16;
    cfg.capture.channels  = (channel == AudioDevice::Mono) ? 1 : 2;
    cfg.sampleRate        = 48000;
    cfg.periodSizeInFrames = static_cast<ma_uint32>(framesPerBuffer);
    cfg.dataCallback      = &SoundInput::s_dataCallback;
    cfg.pUserData         = this;

    if (ma_device_init(nullptr, &cfg, &m_device) != MA_SUCCESS) {
        Q_EMIT error(tr("Failed to initialize audio input device."));
        return;
    }
    m_deviceInitialized = true;

    if (ma_device_start(&m_device) != MA_SUCCESS) {
        Q_EMIT error(tr("Failed to start audio input device."));
        ma_device_uninit(&m_device);
        m_deviceInitialized = false;
        return;
    }

    Q_EMIT status(tr("Receiving"));
}

/**
 * @brief "Suspends" audio input — [TODO #108 keep-warm 2026-07-21]
 * the device is NOT stopped anymore. Stopping it (the old
 * ma_device_stop) meant every monitor-off/on cycle cold-started the
 * capture device; the original design intent was full-time input
 * audio with monitor as a display/decode gate only. Suspend now just
 * discards captured samples in onCapture — identical decode
 * semantics (the sink sees nothing), warm device.
 */
void
SoundInput::suspend()
{
    m_discarding.store(true, std::memory_order_relaxed);
    Q_EMIT status(tr("Suspended"));
}

void
SoundInput::resume()
{
    if (m_sink) m_sink->reset();

    m_discarding.store(false, std::memory_order_relaxed);
    // Safety: if some path left the device stopped, restart it.
    if (m_deviceInitialized && !ma_device_is_started(&m_device)) {
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            Q_EMIT error(tr("Failed to resume audio input device."));
            return;
        }
    }
    Q_EMIT status(tr("Receiving"));
}

/**
 * @brief Stops audio input and releases the device. ma_device_uninit
 * is synchronous: it stops the device, joins the worker thread, and
 * frees backend resources before returning. No busywait or
 * deferred-delete drain is needed.
 */
void
SoundInput::stop()
{
    if (m_deviceInitialized) {
        ma_device_uninit(&m_device);
        m_deviceInitialized = false;
    }
    if (m_sink) m_sink->close();
}

/**
 * @brief Destructs the SoundInput object. Just stops; ma_device_uninit
 * gives us a clean teardown without the closingDown() leak path that
 * Build 128 needed to bypass Qt6Multimedia's destructor cascade.
 */
SoundInput::~SoundInput()
{
    stop();
}

Q_LOGGING_CATEGORY(soundin_js8, "soundin.js8", QtWarningMsg)
