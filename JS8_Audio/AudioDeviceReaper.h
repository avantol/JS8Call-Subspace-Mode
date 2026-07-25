// -*- Mode: C++ -*-
#ifndef AUDIO_DEVICE_REAPER_H__
#define AUDIO_DEVICE_REAPER_H__

#include "JS8_Audio/miniaudio/miniaudio.h"

#include <QString>
#include <QtDebug>

#include <chrono>
#include <thread>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

// Retire a heap-allocated, already-initialized ma_device WITHOUT blocking
// the caller.
//
// ma_device_uninit() is synchronous: it stops the device and joins
// miniaudio's backend worker before returning. On a PulseAudio device
// re-enumeration (USB codec removed/re-added; its server index jumps) the
// server tears down our streams but miniaudio's worker keeps polling the
// now-dead descriptors and wedges in pa_mainloop_poll -> ppoll forever, so
// the join never returns. When that uninit ran on the audio QThread
// (SoundInput::stop / SoundOutput::teardown via the closeEvent->finished
// path), the thread hung, ~UI_Constructor's bounded wait() overran, and
// the shutdown watchdog fired std::_Exit(0) — which also killed main()'s
// in-place config-switch restart loop (the "exited but didn't come back"
// symptom, 2026-07-24 Default->IC-7300). See the SHUTDOWN WATCHDOG note in
// JS8_UI/mainwindow.cpp for the captured gdb backtrace.
//
// Detaching the uninit here makes teardown return instantly: the audio
// thread finishes, the destructor's join succeeds, and the process either
// exits cleanly or continues into the config-switch restart. The wedged
// worker (if any) lingers harmlessly on dead fds until the process exits —
// its server-side streams are already gone, so it holds no live hardware
// and a fresh ma_device_init for the next configuration still succeeds.
//
// LOG CONTRACT: "retiring device" is logged on the caller's thread before
// the handoff, "device retired OK in N ms" from the reaper thread after
// uninit returns. A "retiring" line with NO matching "retired OK" is the
// wedge signature — the one place a wedged uninit remains visible now
// that it no longer hangs anything. (The reaper thread is named
// "audio-reaper" so a stuck one also identifies itself in ps -T / gdb.)
//
// OWNERSHIP: `device` (heap-allocated by the caller) transfers to the
// detached worker, which frees it after ma_device_uninit returns —
// whenever that is. The caller MUST null its own pointer immediately so a
// later stop()/destructor never touches memory this thread now owns; that
// is exactly what keeps a wedged, still-outstanding uninit from becoming a
// use-after-free when deleteLater destroys the SoundInput/SoundOutput.
inline void
retireAudioDevice(ma_device * device, QString const & label)
{
    if (!device) return;

    // The capture/playback callbacks null-check pUserData before touching
    // the owning object; clear it so a callback that fires during teardown
    // can't dereference an object that may already be destroyed. Callbacks
    // already PAST that check are handled by the owner's in-flight drain
    // (m_callbackDepth in SoundInput/SoundOutput) — not by this clear.
    device->pUserData = nullptr;

    qWarning().noquote() << label
        << "[AUDIO-REAP] retiring device (uninit on detached reaper thread)";

    std::thread([device, label]() {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "audio-reaper");
#elif defined(__APPLE__)
        pthread_setname_np("audio-reaper");
#endif
        auto const t0 = std::chrono::steady_clock::now();
        ma_device_uninit(device);
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        qWarning().noquote() << label
            << "[AUDIO-REAP] device retired OK in" << ms << "ms";
        delete device;
    }).detach();
}

#endif // AUDIO_DEVICE_REAPER_H__
