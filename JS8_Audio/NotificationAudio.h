#ifndef NOTIFICATIONAUDIO_H
#define NOTIFICATIONAUDIO_H

#include "JS8_Audio/AudioDeviceInfo.h"

#include <QMap>
#include <QObject>
#include <QString>

class QProcess;

// Plays short notification .wav files via platform-native APIs.
//
// History:
//  - Pre-Build 108: hand-managed QAudioSink via SoundOutput. Crashed
//    repeatedly inside Qt6Multimedia.dll+0xea4f on Windows.
//  - Build 108: QSoundEffect, which solved the immediate crashes but
//    was prone to per-effect WASAPI session exhaustion under rapid
//    plays.
//  - Build 123: bypassed QSoundEffect on every shipped platform with a
//    native short-sound API (PlaySoundW / paplay / afplay) and kept
//    QSoundEffect as a fallback.
//  - Build 143: dropped the QSoundEffect fallback as part of the
//    Qt::Multimedia removal. Native APIs cover every platform we
//    ship; if a native tool fails we log and skip.
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
    // Per-file in-flight subprocess (paplay/aplay on Linux, afplay on
    // macOS) so a new play() can kill the prior one for the SAME file
    // (rapid-restart UX). Different files overlap freely. Empty /
    // unused on Windows where PlaySoundW handles rapid-restart natively.
    QMap<QString, QProcess *> m_nativeProcs;
};

#endif // NOTIFICATIONAUDIO_H
