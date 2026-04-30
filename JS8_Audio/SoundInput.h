// -*- Mode: C++ -*-
#ifndef SOUNDIN_H__
#define SOUNDIN_H__

#include "JS8_Audio/AudioDevice.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QScopedPointer>
#include <QString>

// Gets audio data from sound sample source and passes it to a sink device
class SoundInput : public QObject {
    Q_OBJECT;

  public:
    SoundInput(QObject *parent = nullptr) : QObject{parent}, m_sink{nullptr} {}

    ~SoundInput();

    // sink must exist from the start call until the next start call or
    // stop call
    Q_SLOT void start(QAudioDevice const &, int framesPerBuffer,
                      AudioDevice *sink,
                      AudioDevice::Channel = AudioDevice::Mono);
    Q_SLOT void suspend();
    Q_SLOT void resume();
    Q_SLOT void stop();

    Q_SIGNAL void error(QString message) const;
    Q_SIGNAL void status(QString message) const;

  private:
    // used internally
    Q_SLOT void handleStateChanged(QAudio::State) const;

    bool audioError() const;

    QScopedPointer<QAudioSource> m_stream;
    QPointer<AudioDevice> m_sink;
    QAudioDevice m_lastDevice;
    AudioDevice::Channel m_lastChannel{AudioDevice::Mono};

    // Build 130: set true while start()/stop() is mid-teardown and the
    // QAudioSource is being torn down or replaced. handleStateChanged()
    // early-returns when set, ignoring already-queued stateChanged
    // signals from the prior source that arrive after our disconnect()
    // but before the new source is fully wired. Without this, those
    // queued events triggered audioError() -> m_stream->error() while
    // m_stream was either null or pointing at a not-yet-initialized new
    // source, faulting inside Qt6Multimedia at +0x5c7f2.
    bool m_tearingDown = false;
};

#endif
