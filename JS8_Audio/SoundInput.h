// -*- Mode: C++ -*-
#ifndef SOUNDIN_H__
#define SOUNDIN_H__

#include "JS8_Audio/AudioDevice.h"
#include "JS8_Audio/AudioDeviceInfo.h"
#include "JS8_Audio/miniaudio/miniaudio.h"

#include <QObject>
#include <QPointer>
#include <QString>

// Captures audio samples from the system's selected input device and
// pushes them into an AudioDevice (QIODevice) sink — typically the
// Detector.
//
// Build 140 swapped this from QAudioSource (Qt6Multimedia) to
// miniaudio's ma_device. Reasons documented in the migration plan
// at ~/.claude/plans/functional-swimming-avalanche.md. Short version:
// Qt 6.9.x WASAPI teardown races caused a long crash family
// (Qt6Multimedia.dll+0x5c7f2 etc.). ma_device_uninit() is synchronous
// and joins miniaudio's worker before returning, so the busywait,
// deferred-delete drain, and m_tearingDown gate from Builds 127-133
// are no longer needed.
class SoundInput : public QObject {
    Q_OBJECT;

  public:
    SoundInput(QObject *parent = nullptr) : QObject{parent}, m_sink{nullptr} {}

    ~SoundInput();

    // sink must exist from the start call until the next start call or
    // stop call
    Q_SLOT void start(AudioDeviceInfo const &, int framesPerBuffer,
                      AudioDevice *sink,
                      AudioDevice::Channel = AudioDevice::Mono);
    Q_SLOT void suspend();
    Q_SLOT void resume();
    Q_SLOT void stop();

    Q_SIGNAL void error(QString message) const;
    Q_SIGNAL void status(QString message) const;

  private:
    static void s_dataCallback(ma_device * device,
                               void * pOutput,
                               void const * pInput,
                               ma_uint32 frameCount);
    void onCapture(void const * pInput, ma_uint32 frameCount);

    ma_device m_device {};
    bool m_deviceInitialized = false;
    QPointer<AudioDevice> m_sink;
    AudioDeviceInfo m_lastDevice;
    AudioDevice::Channel m_lastChannel{AudioDevice::Mono};
    int m_lastFramesPerBuffer = 0;
};

#endif
