#ifndef MODULATOR_HPP__
#define MODULATOR_HPP__

#include "JS8_Audio/AudioDevice.h"

#include <QPointer>
#include <QVector>
#include <atomic>

class SoundOutput;

/**
 * Audio device that generates PCM audio frames that encode a message.
 *
 * Output can be muted while underway, preserving waveform timing when
 * transmission is resumed.
 *
 * This is intended to run in a thread different from the GUI thread.
 * It is **not** generally thread-safe, see remarks below.
 */
class Modulator final : public AudioDevice {
    Q_OBJECT;

  public:
    enum class State { Synchronizing, Active, KeepAlive, Idle };

    // Constructor

    explicit Modulator(QObject *parent = nullptr) : AudioDevice{parent} {}

    // Inline accessors

    /**
     * Whether the device is idle.
     *
     * This method is thread-safe, i.e., can be called from a different thread.
     */
    bool isIdle() const {
        auto s = m_state.load();
        return s == State::Idle || s == State::KeepAlive;
    }
    bool isFT2WaveformDone() const {
        auto s = m_state.load();
        return s == State::Idle || s == State::KeepAlive
            || (m_ft2Mode && m_ic >= static_cast<unsigned>(m_ft2WaveLen));
    }

    // [AUDIO-CADENCE PROBE 2026-06-09] Wall-clock timestamps captured
    // INSIDE readData() at the moment we WRITE the first waveform sample
    // into the audio-device buffer and the moment we WRITE the last one.
    // These tell us when the Modulator handed samples to the device,
    // not when the device actually played them on the wire — that's
    // typically later by ~one buffer fill (suspected ~1 s for our setup).
    // Sentinel -1 = not yet captured for this cycle. Cleared by start().
    // Both stores are once-per-cycle on transitions; no per-sample cost.
    qint64 audioStartedMs() const { return m_audioStartedMs.load(); }
    qint64 audioEndedMs() const { return m_audioEndedMs.load(); }

    // [ARQ TX TIMING ASYNC-FINISH STALE-SENTINEL FIX 2026-06-10 (build 231)]
    // Called synchronously by mainwindow.cpp at PTT-up to clear the audio
    // sentinels BEFORE the queued Modulator::start() runs. Without this
    // synchronous reset, the guiUpdate poll could see the previous
    // frame's stale audioStartedMs value (>0) and fire stopTx instantly,
    // producing the "transmit, skip, transmit, skip" pattern observed in
    // build 230. Always safe to call (idempotent); does not affect audio.
    void resetCadenceCapture() {
        m_audioStartedMs.store(-1);
        m_audioEndedMs.store(-1);
    }

    // Manipulators

    void close() override;

    /**
     * Sets the audio frequency.
     *
     * This is **not** by itself thread-safe, but ok if fed
     * via the Qt signalling mechanism.
     */
    Q_SLOT void setAudioFrequency(double const audioFrequency) {
        m_audioFrequency = audioFrequency;
    }

    // Slots

    Q_SLOT void start(double audioFrequency, int submode, double tx_delay,
                      SoundOutput *stream, Channel channel);
    Q_SLOT void stop(bool quick = false);
    Q_SLOT void tune(bool state = true);

    // Set by main thread (UI_Constructor) when ARQ messaging is in
    // flight. Modulator's silent-frames decision branch checks this
    // flag and bypasses the period-aligned WAIT-NEXT-PERIOD pad —
    // start immediately, no boundary wait, no SV1UY-style silent TX
    // (the bypass is bounded because m_arqRelax only goes true when
    // ARQ is in flight, and ARQ only runs in FT2 / Subspace where
    // continuous-frame TX makes the relaxed start safe). Atomic so
    // the audio-thread read is well-defined.
    void setArqRelax(bool relax) { m_arqRelax.store(relax); }

    // [BUILD 328] Full-frame ⚡ Hellschreiber bolt-beacon mode.
    // Pivot from the per-TX pre-roll preamble (builds 324-327, dead
    // code below). When the GUI passes a non-empty bolt waveform here,
    // the NEXT start() invocation uses that waveform as the FT2 data
    // for the cycle — instead of the encoded text bits — and PLAYS
    // THE BOLT. One-shot: the flag clears inside start() so the cycle
    // after the bolt reverts to normal FT2 modulation. Caller must
    // ensure Modulator is Idle when calling (no mid-stream swap).
    void setFullFrameBoltWaveform(QVector<float> waveform) {
        m_boltWaveform = std::move(waveform);
        m_useFullFrameBolt.store(!m_boltWaveform.isEmpty());
    }

    // [DEAD: builds 324-327 preamble code below, kept compiling but
    // setPaintBoltPreamble is no longer called by mainwindow.]
    void setPaintBoltPreamble(bool paint) { m_paintBolt.store(paint); }

  signals:
    /// Emitted when FT2 waveform finishes playing (transitions to KeepAlive).
    void ft2WaveformDone();

  protected:
    // QIODevice protocol

    qint64 readData(char *, qint64) override;
    qint64 writeData(char const *, qint64) override {
        return -1; // we don't consume data
    }

    // In current Qt versions, bytesAvailable() must return a size
    // that exceeds some threshold in order for the AudioSink to go
    // into Active state and start pulling data. This behavior began
    // on Windows with the 6.4 release, on Mac with 6.8, and on Linux
    // with 6.9.
    //
    // See: https://bugreports.qt.io/browse/QTBUG-108672

    qint64 bytesAvailable() const override { return 8000; }

  private:
    // Data members

    QPointer<SoundOutput> m_stream;
    std::atomic<State> m_state = State::Idle;
    bool m_quickClose = false;
    bool m_tuning = false;
    double m_audioFrequency;
    double m_audioFrequency0;
    double m_toneSpacing;
    double m_phi;
    double m_dphi;
    double m_amp;
    double m_nsps;
    qint64 m_silentFrames;
    std::atomic<bool> m_arqRelax{false};

    // [BUILD 324] ⚡ preamble plumbing — DEAD code, retained for the
    // dead setPaintBoltPreamble setter above. Build 328 pivoted to
    // full-frame bolt (see m_boltWaveform / m_useFullFrameBolt below).
    std::atomic<bool> m_paintBolt{false};
    QVector<qint16> m_boltAudio;
    int m_boltPos{0};

    // [BUILD 328] Full-frame ⚡ bolt plumbing.
    // m_useFullFrameBolt: GUI sets via setFullFrameBoltWaveform()
    //   before triggering TX; start() reads, overrides m_ft2Wave to
    //   point at m_boltWaveform's data, then clears the flag (one-shot).
    // m_boltWaveform: float audio samples (~144000 at 48 kHz = 3 s).
    //   Stays alive for the duration of the bolt TX cycle since
    //   start() points m_ft2Wave INTO this buffer's storage.
    std::atomic<bool> m_useFullFrameBolt{false};
    QVector<float> m_boltWaveform;
    unsigned m_ic;
    unsigned m_isym0;
#ifdef JS8_ENABLE_FT2
    bool m_ft2Mode = false;        // True when transmitting FT2 GFSK waveform
    float *m_ft2Wave = nullptr;    // Pointer to pre-generated waveform
    int m_ft2WaveLen = 0;          // Length of pre-generated waveform
#endif
    unsigned m_txCycleCount = 0;   // TX cycle counter for diagnostics

    // [AUDIO-CADENCE PROBE 2026-06-09] see audioStartedMs/audioEndedMs
    // public accessors above. Atomics because written from audio thread
    // (readData) and read from GUI thread (stopTx). Sentinel -1 = unset.
    std::atomic<qint64> m_audioStartedMs{-1};
    std::atomic<qint64> m_audioEndedMs{-1};
};

#endif
