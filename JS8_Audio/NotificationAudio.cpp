/**
 * @file NotificationAudio.cpp
 * @brief Play short notification .wav files via platform-native APIs.
 *
 * Build 143 dropped the QSoundEffect fallback entirely as part of the
 * Qt::Multimedia removal. The native paths (PlaySoundW on Windows,
 * paplay on Linux, afplay on macOS) cover every platform we ship and
 * are individually more reliable than QSoundEffect was even when it
 * worked. If a native tool fails we log and skip the play — better
 * than silent unreliability.
 */
#include "NotificationAudio.h"

#include <QFileInfo>
#include <QProcess>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <mmsystem.h>
#endif

NotificationAudio::NotificationAudio(QObject *parent) : QObject{parent} {}

NotificationAudio::~NotificationAudio() = default;

/**
 * @brief Set the notification audio device. Stored only for the diag
 * log + the play()-side null guard. The native platform paths always
 * route to the system default; per-app notification device routing is
 * a separate feature that no platform we ship currently supports
 * cleanly via a one-line API.
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
 * Each platform shells out to its native short-sound API — bypasses
 * any in-process audio state, so rapid plays do not accumulate leaked
 * sessions. Different files are allowed to overlap; the same file
 * being replayed kills the prior subprocess for that file (rapid-
 * restart UX).
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

#if defined(Q_OS_WIN)
    // Windows: PlaySoundW handles rapid restart atomically. SND_NODEFAULT
    // suppresses the system "ding" if the file can't be loaded; SND_ASYNC
    // keeps the GUI thread responsive.
    PlaySoundW(reinterpret_cast<LPCWSTR>(filePath.utf16()), nullptr,
               SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    qWarning() << "[NotifAudio-win] PlaySound for" << filePath;
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#  if defined(Q_OS_MACOS)
    QStringList const tools{QStringLiteral("afplay")};
#  else
    // Linux: paplay (PulseAudio / pipewire-pulse) is preferred; aplay
    // (ALSA) is a secondary fallback for ALSA-only systems.
    QStringList const tools{QStringLiteral("paplay"), QStringLiteral("aplay")};
#  endif

    if (auto *prev = m_nativeProcs.value(filePath, nullptr)) {
        qWarning() << "[NotifAudio-native] killing prior pid="
                   << prev->processId() << "for" << filePath;
        prev->disconnect();
        prev->kill();
        prev->waitForFinished(100);
        prev->deleteLater();
        m_nativeProcs.remove(filePath);
    }

    for (auto const &tool : tools) {
        auto *proc = new QProcess(this);
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(
                          &QProcess::finished),
                this, [this, filePath, proc](int /*code*/,
                                             QProcess::ExitStatus /*st*/) {
                    m_nativeProcs.remove(filePath);
                    proc->deleteLater();
                });
        proc->start(tool, QStringList{filePath});
        if (proc->waitForStarted(500)) {
            qWarning() << "[NotifAudio-native]" << tool
                       << "started pid=" << proc->processId()
                       << "for" << filePath;
            m_nativeProcs.insert(filePath, proc);
            return;
        }
        qWarning() << "[NotifAudio-native]" << tool << "FAILED to start:"
                   << proc->errorString();
        proc->deleteLater();
    }

    qWarning() << "[NotifAudio-native] no playback tool available for"
               << filePath;
#endif
}

/**
 * @brief Stop any currently-playing notification.
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
}
