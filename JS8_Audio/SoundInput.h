// -*- Mode: C++ -*-
#ifndef SOUNDIN_H__
#define SOUNDIN_H__

#include "JS8_Audio/AudioDevice.h"
#include "JS8_Audio/AudioDeviceInfo.h"
#include "JS8_Audio/miniaudio/miniaudio.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>

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
    // [TODO #113 2026-07-23] The CONFIGURED capture device could not be
    // resolved, so miniaudio was handed a null device id and opened the
    // SYSTEM DEFAULT instead. Previously silent — and the single most
    // expensive silent failure on Linux: after a USB codec is removed
    // and re-added, we quietly end up capturing the built-in mic. The
    // waterfall looks completely normal (room noise) while nothing
    // decodes, with no error anywhere. Deliberately NOT the error()
    // signal: error() also invalidates the configured device, and the
    // operator's device choice should survive a re-plug (dialog-only,
    // operator decision 2026-07-23).
    Q_SIGNAL void deviceFallback(QString message) const;

  private:
    static void s_dataCallback(ma_device * device,
                               void * pOutput,
                               void const * pInput,
                               ma_uint32 frameCount);
    void onCapture(void const * pInput, ma_uint32 frameCount);

    ma_device m_device {};
    bool m_deviceInitialized = false;
    // [TODO #108 keep-warm 2026-07-21] suspend()/resume() gate SAMPLE
    // DELIVERY, not the device: the capture stream runs full-time so
    // the codec never cold-starts (the original full-time-audio plan;
    // ma_device_stop here was the reversion). Written on the audio
    // QThread, read on miniaudio's worker.
    std::atomic<bool> m_discarding{false};
    QPointer<AudioDevice> m_sink;
    AudioDeviceInfo m_lastDevice;
    AudioDevice::Channel m_lastChannel{AudioDevice::Mono};
    int m_lastFramesPerBuffer = 0;
};

#endif
