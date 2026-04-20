/**
 * @file NotificationAudio.cpp
 * @brief Implementation of NotificationAudio class
 */
#include "NotificationAudio.h"
#include "BWFFile.h"
#include "SoundOutput.h"

#include <QLoggingCategory>
#include <QMediaDevices>

Q_DECLARE_LOGGING_CATEGORY(notificationaudio_js8)

/******************************************************************************/
// Public Implementation
/******************************************************************************/
/**
 * @brief Constructs a NotificationAudio object.
 * @param parent The parent QObject.
 * @return None.
 */
NotificationAudio::NotificationAudio(QObject *parent)
    : QObject{parent}, m_stream{new SoundOutput} {
    connect(m_stream.data(), &SoundOutput::status, this,
            &NotificationAudio::status);
    connect(m_stream.data(), &SoundOutput::error, this,
            &NotificationAudio::error);
}

/**
 * @brief Destructs the NotificationAudio object.
 */
NotificationAudio::~NotificationAudio() { stop(); }

/**
 * @brief Handles status messages from the SoundOutput.
 * @param message The status message.
 */
void NotificationAudio::status(QString const message) {
    if (message == "Idle") {
        // Qt6's IdleState fires both on "source fully consumed" AND on
        // transient mid-play underruns when the sink's internal buffer
        // drains briefly. Only stop if the source QBuffer is actually
        // empty -- otherwise we'd cut off playback whenever the sink
        // momentarily outpaces the buffer feed (which is most of the
        // time for short clips, producing the "random play time, often
        // cut off" symptom).
        if (m_buffer.bytesAvailable() <= 0) {
            stop();
            m_playing.store(false);
        }
    }
}

/**
 * @brief Handles error messages from the SoundOutput.
 * @param message The error message.
 */
void NotificationAudio::error(QString const message) {
    qCDebug(notificationaudio_js8) << "notification error:" << message;
}

/**
 * @brief Sets the audio device and buffer size.
 * @param device The QAudioDevice to use.
 * @param msBuffer The buffer size in milliseconds.
 */
void NotificationAudio::setDevice(QAudioDevice const &device,
                                  unsigned const msBuffer) {
    if (device.isNull()) {
        m_device = QMediaDevices::defaultAudioOutput();
        qWarning() << "[NotificationAudio] setDevice: null given,"
                   << "fallback=" << m_device.description()
                   << "nullAfter=" << m_device.isNull();
    } else {
        m_device = device;
        qWarning() << "[NotificationAudio] setDevice:"
                   << m_device.description();
    }
    m_msBuffer = msBuffer;
}

/**
 * @brief Plays an audio file from the specified file path.
 * @param filePath The path to the audio file.
 */
void NotificationAudio::play(QString const &filePath) {
    qWarning() << "[NotificationAudio] play() called:" << filePath;

    // Startup race: setDevice arrives as a queued event on this thread, and
    // can be delivered *after* the first play() if whoever emitted play()
    // (e.g. startup sound) got queued before setDevice did. Without a
    // device, there's nothing to do -- drop silently so m_playing doesn't
    // latch on a failed sink creation.
    if (m_device.isNull()) {
        qWarning() << "[NotificationAudio] play() dropped (no device yet):"
                   << filePath;
        return;
    }

    // Race guard: if a previous notification is still playing, drop this
    // request. Rapid plays (e.g. user mashing the test button in
    // Configuration > Notifications) destroy a still-active QAudioSink
    // mid-stream via setDeviceFormat, crashing Qt6Multimedia at offset
    // 0xea4f. Cleared from status() when sink reports Idle.
    if (m_playing.exchange(true)) {
        qWarning() << "[NotificationAudio] play() dropped (already playing)";
        return;
    }

    // RAII: if we exit play() without successfully starting playback,
    // clear m_playing so the next call isn't blocked forever.
    struct Guard {
        std::atomic<bool> &flag;
        bool armed = true;
        ~Guard() { if (armed) flag.store(false); }
    } guard{m_playing};

    if (auto const it = m_cache.constFind(filePath); it != m_cache.constEnd()) {
        qWarning() << "[NotificationAudio] play() cache HIT:" << filePath;
        guard.armed = false;  // playback handed off to playEntry
        playEntry(it);
        return;
    }

    BWFFile file(QAudioFormat{}, filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[NotificationAudio] open failed:" << filePath
                   << "err=" << file.errorString();
        return;
    }

    QByteArray data = file.readAll();
    if (data.isEmpty()) {
        qWarning() << "[NotificationAudio] file has no data:" << filePath;
        return;
    }

    QAudioFormat fmt = file.format();
    qWarning() << "[NotificationAudio] loaded:" << filePath
               << "bytes=" << data.size()
               << "sampleFormat=" << (int)fmt.sampleFormat()
               << "channels=" << fmt.channelCount()
               << "rate=" << fmt.sampleRate()
               << "bps=" << file.bitsPerSample();

    // Note: we deliberately do NOT bail on isFormatSupported(fmt) returning
    // false. Qt6's isFormatSupported is conservative and rejects formats
    // (notably 8-bit mono, low sample rates) that the underlying platform
    // actually resamples and plays fine. Let the QAudioSink try; if it
    // genuinely can't handle it, it'll emit an error via our error signal.
    if (!m_device.isFormatSupported(fmt)) {
        qWarning() << "[NotificationAudio] isFormatSupported=false, trying"
                   << "anyway:" << fmt;
    }

    // If source is 8-bit PCM, widen to 16-bit signed -- many devices
    // drop UInt8 support but accept Int16 and the conversion is trivial.
    if (fmt.sampleFormat() == QAudioFormat::UInt8) {
        QByteArray widened(data.size() * 2, Qt::Uninitialized);
        auto const *src = reinterpret_cast<quint8 const *>(data.constData());
        auto *dst = reinterpret_cast<qint16 *>(widened.data());
        for (int i = 0; i < data.size(); ++i) {
            // UInt8 is unsigned 0..255 centered at 128; Int16 is signed
            // -32768..32767 centered at 0. Offset + scale by 256.
            dst[i] = static_cast<qint16>((int(src[i]) - 128) * 256);
        }
        data = std::move(widened);
        fmt.setSampleFormat(QAudioFormat::Int16);
    }

    // If source is 24-bit packed PCM, repack to Int32
    if (file.bitsPerSample() == 24) {
        QByteArray converted = pcm24le_to_int32le(data);
        if (converted.isEmpty()) {
            qWarning() << "[NotificationAudio] 24-bit conversion failed";
            return;
        }
        data = std::move(converted);
        fmt.setSampleFormat(QAudioFormat::Int32);
        // keep sampleRate + channelCount from file.format()
    }

    // Mono -> stereo so mono files play through both speakers
    if (!upmixMonoToStereoInPlace(fmt, data)) {
        qWarning() << "[NotificationAudio] mono upmix failed, fmt=" << fmt;
        return;
    }

    qWarning() << "[NotificationAudio] final fmt=" << fmt
               << "bytes=" << data.size();

    guard.armed = false;  // playback handed off to playEntry
    playEntry(m_cache.emplace(filePath, fmt, data));
}

/**
 * @brief Stops audio playback.
 */
void NotificationAudio::stop() { m_stream->stop(); }

/******************************************************************************/
// Private Implementation
/******************************************************************************/

void NotificationAudio::playEntry(Cache::const_iterator const it) {
    if (m_buffer.isOpen())
        m_buffer.close();

    auto const &[format, data] = *it;

    m_buffer.setData(data);

    if (m_buffer.open(QIODevice::ReadOnly)) {
        m_stream->setDeviceFormat(m_device, format, m_msBuffer);
        m_stream->restart(&m_buffer);
        // If the sink never came up (null device, format rejected, etc.),
        // SoundOutput won't emit status(Idle) on completion -- which means
        // m_playing would latch true forever and kill every subsequent
        // play(). Clear it here so the next play can try again.
        if (!m_stream->isStreaming()) {
            qWarning() << "[NotificationAudio] sink failed to start;"
                       << "clearing m_playing";
            m_playing.store(false);
        }
    } else {
        qWarning() << "[NotificationAudio] m_buffer.open failed;"
                   << "clearing m_playing";
        m_playing.store(false);
    }
}

/**
 * @brief Convert 24-bit PCM to 32-bit PCM
 * @param PCM data byte array
 */
QByteArray NotificationAudio::pcm24le_to_int32le(const QByteArray& in)
{
    if ((in.size() % 3) != 0) return {};

    const int samples = in.size() / 3;
    QByteArray out;
    out.resize(samples * 4);

    const uint8_t* src = reinterpret_cast<const uint8_t*>(in.constData());
    int32_t* dst = reinterpret_cast<int32_t*>(out.data());

    for (int i = 0; i < samples; ++i) {
        int32_t v = (int32_t(src[0])      ) |
                    (int32_t(src[1]) <<  8) |
                    (int32_t(src[2]) << 16);

        // sign extend 24 -> 32
        if (v & 0x00800000) v |= 0xFF000000;

        // scale to full 32-bit range (optional but common)
        dst[i] = v << 8;

        src += 3;
    }
    return out;
}

/**
 * @brief Convert mono PCM data to stereo
 * @param fmt The audio format
 * @param data The audio data byte array
 * @return true if upmix was successful, false otherwise
 */
bool NotificationAudio::upmixMonoToStereoInPlace(QAudioFormat& fmt, QByteArray& data)
{
    if (fmt.channelCount() != 1)
        return true; // nothing to do

    switch (fmt.sampleFormat()) {
    case QAudioFormat::UInt8: {
        // 1 byte/sample -> 2 bytes/frame (L,R)
        QByteArray out;
        out.resize(data.size() * 2);

        const uint8_t* src = reinterpret_cast<const uint8_t*>(data.constData());
        uint8_t* dst = reinterpret_cast<uint8_t*>(out.data());

        for (int i = 0; i < data.size(); ++i) {
            const uint8_t s = src[i];
            *dst++ = s; // L
            *dst++ = s; // R
        }

        data = std::move(out);
        fmt.setChannelCount(2);
        return true;
    }

    case QAudioFormat::Int16: {
        if ((data.size() % 2) != 0) return false;
        const int samples = data.size() / 2;

        QByteArray out;
        out.resize(samples * 4);

        const int16_t* src = reinterpret_cast<const int16_t*>(data.constData());
        int16_t* dst = reinterpret_cast<int16_t*>(out.data());

        for (int i = 0; i < samples; ++i) {
            const int16_t s = src[i];
            *dst++ = s; // L
            *dst++ = s; // R
        }

        data = std::move(out);
        fmt.setChannelCount(2);
        return true;
    }

    case QAudioFormat::Int32: {
        if ((data.size() % 4) != 0) return false;
        const int samples = data.size() / 4;

        QByteArray out;
        out.resize(samples * 8);

        const int32_t* src = reinterpret_cast<const int32_t*>(data.constData());
        int32_t* dst = reinterpret_cast<int32_t*>(out.data());

        for (int i = 0; i < samples; ++i) {
            const int32_t s = src[i];
            *dst++ = s; // L
            *dst++ = s; // R
        }

        data = std::move(out);
        fmt.setChannelCount(2);
        return true;
    }

    case QAudioFormat::Float: {
        // Qt float samples are 32-bit
        if ((data.size() % 4) != 0) return false;
        const int samples = data.size() / 4;

        QByteArray out;
        out.resize(samples * 8); // mono float32 -> stereo float32

        const float* src = reinterpret_cast<const float*>(data.constData());
        float* dst = reinterpret_cast<float*>(out.data());

        for (int i = 0; i < samples; ++i) {
            const float s = src[i];
            *dst++ = s; // L
            *dst++ = s; // R
        }

        data = std::move(out);
        fmt.setChannelCount(2);
        return true;
    }

    default:
        return false; // unsupported here
    }
}

/******************************************************************************/

Q_LOGGING_CATEGORY(notificationaudio_js8, "notificationaudio.js8", QtWarningMsg)
