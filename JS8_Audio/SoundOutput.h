// -*- Mode: C++ -*-
#ifndef SOUNDOUT_H__
#define SOUNDOUT_H__

#include "JS8_Audio/AudioDeviceInfo.h"
#include "JS8_Audio/AudioFormat.h"
#include "JS8_Audio/miniaudio/miniaudio.h"

#include <QObject>
#include <QString>

class QIODevice;

// Streams audio samples from a QIODevice source (the Modulator) to the
// system's selected output device.
//
// Build 141 swapped this from QAudioSink (Qt6Multimedia) to miniaudio's
// ma_device. Same rationale as SoundInput (Build 140): Qt 6.9.x WASAPI
// teardown races caused a long crash family. ma_device_uninit() is
// synchronous; the busywait, deferred-delete drain, m_tearingDown gate,
// platform-specific stop-vs-recreate logic, and Build 138 first-only
// warmup are all gone.
class SoundOutput : public QObject {
    Q_OBJECT;

  public:
    // `tag` is the log prefix so TX audio and notification-audio
    // instances are distinguishable. Default matches the historical
    // "[FT2-TX]" for source compatibility.
    explicit SoundOutput(QString const & tag = QStringLiteral("FT2-TX"))
        : m_tag("[" + tag + "]") {}

    ~SoundOutput() override;

    qreal       attenuation() const;
    AudioFormat format() const { return m_format; }
    bool        isStreaming() const;

  public Q_SLOTS:
    void setFormat(AudioDeviceInfo const & device, unsigned channels,
                   unsigned msBuffered = 0u);
    void setDeviceFormat(AudioDeviceInfo const & device,
                         AudioFormat const & format,
                         unsigned msBuffered = 0u);
    void restart(QIODevice *);
    void suspend();
    void resume();
    void reset();
    void stop();
    void setAttenuation(qreal); /* unsigned */
    void resetAttenuation();    /* to zero */

  Q_SIGNALS:
    void error(QString message) const;
    void status(QString message) const;

  private:
    static void s_dataCallback(ma_device * device,
                               void * pOutput,
                               void const * pInput,
                               ma_uint32 frameCount);
    void onPlayback(void * pOutput, ma_uint32 frameCount);

    void teardown();   // ma_device_uninit guard
    bool buildAndStart(QIODevice * source);

    QString         m_tag;
    AudioDeviceInfo m_device;
    AudioFormat     m_format;
    unsigned        m_msBuffered = 0u;
    qreal           m_volume     = 1.0;
    QIODevice *     m_source     = nullptr;
    ma_device       m_device_ma  {};
    bool            m_deviceInitialized = false;
};

#endif
