/**
 * @file NotificationAudio.cpp
 * @brief Play short notification .wav files via QSoundEffect.
 */
#include "NotificationAudio.h"

#include <QFileInfo>
#include <QSoundEffect>
#include <QUrl>

/**
 * @brief Construct a NotificationAudio.
 * @param parent The parent QObject.
 */
NotificationAudio::NotificationAudio(QObject *parent) : QObject{parent} {}

/**
 * @brief Destructor.
 */
NotificationAudio::~NotificationAudio() = default;

/**
 * @brief Set the audio output device for all notification effects.
 *
 * The `msBuffer` parameter is accepted for API compatibility with the
 * previous SoundOutput-based implementation but is ignored -- QSoundEffect
 * manages its own buffering internally.
 *
 * @param device The QAudioDevice to use.
 * @param msBuffer Unused.
 */
void NotificationAudio::setDevice(QAudioDevice const &device,
                                  unsigned const msBuffer) {
    Q_UNUSED(msBuffer);
    m_device = device;
    qWarning() << "[NotificationAudio] setDevice:" << m_device.description()
               << "isNull=" << m_device.isNull();

    // Push the new device to every cached effect.
    for (auto *effect : std::as_const(m_effects)) {
        effect->setAudioDevice(m_device);
    }
}

/**
 * @brief Play the notification sound at `filePath`.
 *
 * A QSoundEffect is lazily created per file path and cached for the life
 * of the object; subsequent plays of the same file reuse the loaded
 * effect, which is what QSoundEffect is designed for.
 *
 * @param filePath Absolute path to a .wav file.
 */
void NotificationAudio::play(QString const &filePath) {
    qWarning() << "[NotificationAudio] play() called:" << filePath;

    // Startup race: setDevice arrives as a queued event and can land
    // after the first play(). Without a device there is nothing to do.
    if (m_device.isNull()) {
        qWarning() << "[NotificationAudio] play() dropped (no device yet):"
                   << filePath;
        return;
    }

    if (!QFileInfo::exists(filePath)) {
        qWarning() << "[NotificationAudio] play() dropped (file missing):"
                   << filePath;
        return;
    }

    auto *effect = m_effects.value(filePath, nullptr);
    if (!effect) {
        effect = new QSoundEffect(m_device, this);
        effect->setSource(QUrl::fromLocalFile(filePath));
        effect->setLoopCount(1);
        effect->setVolume(1.0f);
        m_effects.insert(filePath, effect);
        qWarning() << "[NotificationAudio] created QSoundEffect for:"
                   << filePath;
    }

    // QSoundEffect::play() on an already-playing effect restarts it;
    // that's the desired behavior for notification chimes.
    effect->play();
}

/**
 * @brief Stop any currently-playing notification.
 *
 * Stops every cached effect. This is cheap because stop() on an
 * already-stopped effect is a no-op.
 */
void NotificationAudio::stop() {
    for (auto *effect : std::as_const(m_effects)) {
        effect->stop();
    }
}
