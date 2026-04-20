// -*- Mode: C++ -*-
#ifndef SOUNDOUT_H__
#define SOUNDOUT_H__

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QObject>
#include <QString>

// An instance of this sends audio data to a specified soundcard.

class SoundOutput : public QObject {
    Q_OBJECT;

  public:
    // `tag` is used as the log prefix for all qWarning lines so TX audio
    // and notification-audio instances are distinguishable in diag logs.
    // Default matches the historical "[FT2-TX]" for source compatibility.
    explicit SoundOutput(QString const &tag = QStringLiteral("FT2-TX"))
        : m_tag("[" + tag + "]") {}

    qreal attenuation() const;
    QAudioFormat format() const;
    bool isStreaming() const;

  public Q_SLOTS:
    void setFormat(QAudioDevice const &device, unsigned channels,
                   unsigned msBuffered = 0u);
    void setDeviceFormat(QAudioDevice const &device, QAudioFormat const &format,
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
    bool checkStream() const;

  private Q_SLOTS:
    void handleStateChanged(QAudio::State) const;

  private:
    QString m_tag;
    QAudioDevice m_device;
    QScopedPointer<QAudioSink> m_stream;
    QIODevice *m_source = nullptr;  // current pull-mode source device
    QAudioFormat m_format;
    unsigned m_msBuffered = 0u;
    qreal m_volume = 1.0;
    bool m_error = false;
};

#endif
