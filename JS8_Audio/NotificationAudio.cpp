/**
 * @file NotificationAudio.cpp
 * @brief Play short notification .wav files via QSoundEffect.
 */
#include "NotificationAudio.h"

#include <QFileInfo>
#include <QProcess>
#include <QSoundEffect>
#include <QUrl>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <mmsystem.h>
#endif

// Diagnostic helper used by the QSoundEffect fallback path.
namespace {
char const *statusName(QSoundEffect::Status s) {
    switch (s) {
    case QSoundEffect::Null:    return "Null";
    case QSoundEffect::Loading: return "Loading";
    case QSoundEffect::Ready:   return "Ready";
    case QSoundEffect::Error:   return "Error";
    }
    return "?";
}
} // namespace

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
 * @brief Set the notification audio device. Build 142: stored only
 * for the diag log and the play()-side null guard. The native
 * platform paths (PlaySoundW / paplay / afplay) always route to the
 * system default; the rare QSoundEffect fallback also uses default
 * because Qt6Multimedia coupling is being removed in Build 143.
 * Per-app notification device routing is a separate feature.
 */
void NotificationAudio::setDevice(AudioDeviceInfo const &device,
                                  unsigned const msBuffer) {
    Q_UNUSED(msBuffer);
    m_device = device;
    qWarning() << "[NotificationAudio] setDevice:" << m_device.description
               << "isNull=" << m_device.isNull();
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

    // ---- Build 123: platform-native audio paths ----
    // QSoundEffect (Qt 6) has well-documented multi-platform issues
    // ranging from cosmetic (cut-off / tail-leak between rapid plays
    // on WASAPI) to functional (per-effect session exhaustion under
    // spammed Test clicks with a ~60s OS-reclaim recovery window).
    // The diagnostic build (audio-diag) confirmed the per-effect
    // resource exhaustion empirically. We bypass QSoundEffect on
    // every platform that has a reliable native short-sound API:
    //   - Windows: PlaySoundW from winmm
    //   - macOS:   afplay subprocess (built-in CLI)
    //   - Linux:   paplay subprocess (PulseAudio / pipewire-pulse)
    // Each call cleanly releases its OS-level audio session, so
    // rapid Test clicks no longer accumulate leaked sessions. The
    // QSoundEffect path is kept as a fallback for platforms that
    // don't match any of the above.

#if defined(Q_OS_WIN)
    // Windows: PlaySoundW() handles rapid restart atomically -- a
    // new call automatically interrupts whatever's currently playing
    // with no per-instance state to track. SND_NODEFAULT prevents
    // a fallback ding when the file can't be loaded. SND_ASYNC so
    // we don't block the GUI thread.
    PlaySoundW(reinterpret_cast<LPCWSTR>(filePath.utf16()), nullptr,
               SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    qWarning() << "[NotifAudio-win] PlaySound for" << filePath;
    return;
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    // POSIX: spawn the platform-native short-sound CLI as a fresh
    // subprocess. Each subprocess cleanly releases its audio session
    // on exit. For rapid-restart UX, kill any in-flight subprocess
    // for the SAME file before spawning a new one. Different files
    // are allowed to overlap (matches QSoundEffect cache semantics).
#  if defined(Q_OS_MACOS)
    QString const tool = QStringLiteral("afplay");
#  else
    QString const tool = QStringLiteral("paplay");
#  endif
    {
        if (auto *prev = m_nativeProcs.value(filePath, nullptr)) {
            qWarning() << "[NotifAudio-native] killing prior" << tool
                       << "pid=" << prev->processId() << "for" << filePath;
            prev->disconnect(); // don't fire 'finished' on our kill
            prev->kill();
            prev->waitForFinished(100);
            prev->deleteLater();
            m_nativeProcs.remove(filePath);
        }
        auto *proc = new QProcess(this);
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(
                          &QProcess::finished),
                this, [this, filePath, proc](int /*code*/,
                                             QProcess::ExitStatus /*st*/) {
                    m_nativeProcs.remove(filePath);
                    proc->deleteLater();
                });
        proc->start(tool, QStringList{filePath});
        if (!proc->waitForStarted(500)) {
            qWarning() << "[NotifAudio-native]" << tool
                       << "FAILED to start for" << filePath
                       << "error:" << proc->errorString();
            proc->deleteLater();
            // Fall through to QSoundEffect fallback below.
        } else {
            qWarning() << "[NotifAudio-native]" << tool
                       << "started pid=" << proc->processId()
                       << "for" << filePath;
            m_nativeProcs.insert(filePath, proc);
            return;
        }
    }
    // (fall-through to QSoundEffect only if the native tool failed)
#endif

    auto *effect = m_effects.value(filePath, nullptr);
    bool justCreated = false;
    if (!effect) {
        // Build 142: bind to system default audio device. We no longer
        // hold a QAudioDevice (m_device is now AudioDeviceInfo). Build
        // 143 drops QSoundEffect entirely.
        effect = new QSoundEffect(this);
        effect->setSource(QUrl::fromLocalFile(filePath));
        effect->setLoopCount(1);
        effect->setVolume(1.0f);
        effect->setProperty("plays", 0);
        m_effects.insert(filePath, effect);

        // audio-diag: log every status / playing transition for this
        // effect, so a "dying" effect's terminal state is visible in
        // the diag log alongside the play() call that failed.
        QString const tagPath = filePath;
        QObject::connect(effect, &QSoundEffect::statusChanged, this,
                         [effect, tagPath]() {
                             qWarning() << "[NotifAudio-diag] STATUS"
                                        << statusName(effect->status())
                                        << "file=" << tagPath;
                         });
        QObject::connect(effect, &QSoundEffect::playingChanged, this,
                         [effect, tagPath]() {
                             qWarning() << "[NotifAudio-diag] PLAYING"
                                        << effect->isPlaying()
                                        << "file=" << tagPath;
                         });

        qWarning() << "[NotificationAudio] created QSoundEffect for:"
                   << filePath << "mEffectsCount=" << m_effects.size();
        justCreated = true;
    }

    int const playsBefore = effect->property("plays").toInt();
    effect->setProperty("plays", playsBefore + 1);

    qWarning() << "[NotifAudio-diag] play#" << (playsBefore + 1)
               << "file=" << filePath
               << "preStatus=" << statusName(effect->status())
               << "preIsPlaying=" << effect->isPlaying()
               << "vol=" << effect->volume()
               << "mEffectsCount=" << m_effects.size();

    // QSoundEffect-flakiness mitigation #1 (pre-warm): on first creation,
    // briefly play+stop at zero volume to force the audio backend to
    // load the source and open its session before the real play. Helps
    // the "first play is choppy / silent / late" pattern documented in
    // upstream Qt forums across ALSA, PulseAudio, PipeWire, and WASAPI.
    if (justCreated) {
        effect->setVolume(0.0f);
        effect->play();
        effect->stop();
        effect->setVolume(1.0f);
    }

    // QSoundEffect-flakiness mitigation #3 (stop-before-play): always
    // stop() before play() to force the backend's ring buffer to reset
    // between plays. Defeats the WASAPI buffer-reuse pattern that
    // caused WD4KAV to hear the tail of a previous notification leak
    // into the head of the next one on Windows 11.
    effect->stop();
    effect->play();

    qWarning() << "[NotifAudio-diag] play# done file=" << filePath
               << "postStatus=" << statusName(effect->status())
               << "postIsPlaying=" << effect->isPlaying();
}

/**
 * @brief Stop any currently-playing notification.
 *
 * Stops every cached effect. This is cheap because stop() on an
 * already-stopped effect is a no-op.
 */
void NotificationAudio::stop() {
#if defined(Q_OS_WIN)
    // PlaySoundW(NULL,...) stops any sound currently playing via the
    // PlaySound mechanism for this process.
    PlaySoundW(nullptr, nullptr, 0);
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    for (auto *proc : std::as_const(m_nativeProcs)) {
        if (proc) {
            proc->disconnect();
            proc->kill();
            proc->waitForFinished(100);
            proc->deleteLater();
        }
    }
    m_nativeProcs.clear();
#endif
    // Also stop any QSoundEffect that was created via the fallback path.
    for (auto *effect : std::as_const(m_effects)) {
        effect->stop();
    }
}
