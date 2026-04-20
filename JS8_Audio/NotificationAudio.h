#ifndef NOTIFICATIONAUDIO_H
#define NOTIFICATIONAUDIO_H

#include <QAudioDevice>
#include <QMap>
#include <QObject>
#include <QString>

class QSoundEffect;

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
    void setDevice(QAudioDevice const &device, unsigned msBuffer = 0);
    void play(QString const &filePath);
    void stop();

  private:
    QAudioDevice m_device;
    QMap<QString, QSoundEffect *> m_effects;
};

#endif // NOTIFICATIONAUDIO_H
