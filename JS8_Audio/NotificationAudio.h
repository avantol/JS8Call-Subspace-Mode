#ifndef NOTIFICATIONAUDIO_H
#define NOTIFICATIONAUDIO_H

#include "JS8_Audio/AudioDeviceInfo.h"

#include <QMap>
#include <QObject>
#include <QString>

class QSoundEffect;
class QProcess;

// Plays short notification .wav files.
//
// Build 108 switched this class from a hand-managed QAudioSink (via
// SoundOutput) to QSoundEffect, which is Qt's purpose-built class for
// low-latency UI chimes. The QAudioSink approach produced repeated
// Qt6Multimedia.dll+0xea4f access violations on Windows, always on the
// second+ play in a session -- no gap length, no sink reuse state, and
// no preError indication made the difference. QSoundEffect internally
// manages its own WASAPI lifecycle across repeated plays and is Qt's
// recommended path for this use case.
class NotificationAudio : public QObject {
    Q_OBJECT

  public:
    explicit NotificationAudio(QObject *parent = nullptr);
    ~NotificationAudio();

  public slots:
    void setDevice(AudioDeviceInfo const &device, unsigned msBuffer = 0);
    void play(QString const &filePath);
    void stop();

  private:
    AudioDeviceInfo m_device;
    QMap<QString, QSoundEffect *> m_effects;
    // Build 123: per-file in-flight subprocess (paplay on Linux,
    // afplay on macOS) so a new play() can kill the prior one for
    // the same file (rapid-restart UX) without going through Qt
    // multimedia. Bypasses QSoundEffect entirely on those platforms.
    // Windows uses PlaySoundW() which handles rapid-restart natively
    // and needs no per-file tracking. Empty / unused on Windows.
    QMap<QString, QProcess *> m_nativeProcs;
};

#endif // NOTIFICATIONAUDIO_H
