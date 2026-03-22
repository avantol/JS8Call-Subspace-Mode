#ifndef DECODE_FT2_H
#define DECODE_FT2_H

/**
 * @file DecodeFT2.h
 * @brief FT2 decoder class that bridges C++ to the Fortran FT2 decoder.
 */

#ifdef JS8_ENABLE_FT2

#include "JS8_Mode/JS8.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

struct dec_data;

namespace JS8 {

class DecodeFT2 {
  public:
    DecodeFT2() = default;

    /**
     * Atomic gate for Fortran mutual exclusion.
     * FT2 standard decoder and L2 share Fortran save variables
     * (ft2_downsample, getcandidates2, sync2d, decode174_91) and
     * must never execute simultaneously.  Both sides acquire via
     * compare_exchange_strong; the loser skips that tick/cycle.
     */
    static inline std::atomic<bool> fortranLock{false};

    /** Show sync-low rejected frames in UI (for developer analysis) */
    static inline bool showRejected{false};

    /**
     * Run FT2 decode on audio data from dec_data.
     * Emits Decoded events via emitEvent for each successful decode.
     *
     * @param data     Reference to dec_data structure with audio samples
     * @param kposFT2  Starting position in the ring buffer
     * @param kszFT2   Number of samples to decode
     * @param emitEvent Event emitter for decode results
     * @return Number of successful decodes
     */
    std::size_t operator()(struct dec_data &data, int kposFT2, int kszFT2,
                           Event::Emitter emitEvent);

    /**
     * Run L2 sync-triggered decode on a contiguous audio buffer.
     * Called from QtConcurrent::run on the async decode timer.
     *
     * @param samples  Contiguous audio buffer (45000 samples at 12kHz)
     * @param nfqso    Expected signal frequency in Hz
     * @param nfa      Low frequency search limit in Hz
     * @param nfb      High frequency search limit in Hz
     * @param utc      UTC timestamp for decoded events
     * @param emitEvent Event emitter for decode results
     * @return Number of successful decodes
     */
    static std::size_t decodeL2(const std::int16_t *samples,
                                int nfqso, int nfa, int nfb, int utc,
                                Event::Emitter emitEvent,
                                const std::int8_t *known_bits = nullptr,
                                int nknown = 0,
                                std::int8_t *decoded_bits_out = nullptr,
                                int *ndecoded_out = nullptr,
                                int nfqso_only = 0,
                                float *decoded_freq_out = nullptr);

    /** Clear multi-period averaging state (call on band/mode change). */
    void clearAveraging();

    /** Encode→decode loopback self-test (call once at startup for diagnostics). */
    static void selfTest();

  private:
    struct CallbackContext {
        Event::Emitter *emitEvent;
        int utc;
        std::size_t count;
    };

    static void decodeCallback(float sync, int snr, float dt, float freq,
                               const char *msg, int msglen, int nap,
                               float qual, const std::int8_t *msgbits77,
                               void *ctx);
};

} // namespace JS8

#endif // JS8_ENABLE_FT2
#endif // DECODE_FT2_H
