#ifndef AUDIOTEARDOWN_H
#define AUDIOTEARDOWN_H

#include <QAudio>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QThread>

// Synchronously wait for a QAudioSink or QAudioSource to reach
// QAudio::StoppedState before the caller deletes it.
//
// Without this synchronization, a Qt audio worker thread that is
// mid-callback inside the old QAudioSink/QAudioSource can read
// through a freed `this` pointer when QScopedPointer's destructor
// runs delete on the object. The fault surfaces inside Qt6Multimedia
// at offsets 0xea4f (QAudioSink) or 0x5c7f2 (QAudioSource) -- same
// bug class, two distinct instructions, both captured in minidumps
// against Build 125 on 2026-04-29. Qt's stop() is asynchronous in
// the sense that it requests a transition but does not block until
// the worker thread has exited its callback. We have to spin-wait
// for state() == StoppedState ourselves.
//
// 250 ms is a generous upper bound. In practice the worker drains
// within tens of milliseconds. Spinning on the calling thread
// (usually the GUI thread) is acceptable here because teardown is
// rare and the user is already stalled (e.g. exiting the app, or
// switching audio devices).
//
// Apply at every site that destroys a QAudioSink or QAudioSource:
//
//   if (m_stream) {
//       m_stream->disconnect(owner);
//       m_stream->reset();           // flush buffer
//       m_stream->stop();            // request stop (async)
//       waitForAudioStopped(m_stream.data(), "tag");
//   }
//   m_stream.reset(/* new sink, or empty */);

template <typename Stream>
inline void waitForAudioStopped(Stream *stream,
                                char const *tag,
                                int timeoutMs = 250)
{
    if (!stream) return;
    if (stream->state() == QAudio::StoppedState) return;

    QElapsedTimer timer;
    timer.start();
    while (stream->state() != QAudio::StoppedState
           && timer.elapsed() < timeoutMs) {
        QThread::msleep(2);
    }

    auto const elapsed = timer.elapsed();
    auto const finalState = stream->state();
    qWarning() << "[AudioTeardown]" << tag
               << "waited" << elapsed << "ms"
               << (finalState == QAudio::StoppedState ? "OK" : "TIMEOUT")
               << "finalState=" << static_cast<int>(finalState);
}

#endif // AUDIOTEARDOWN_H
