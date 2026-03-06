#ifndef DECODE_FT2_H
#define DECODE_FT2_H

/**
 * @file DecodeFT2.h
 * @brief FT2 decoder class that bridges C++ to the Fortran FT2 decoder.
 */

#ifdef JS8_ENABLE_FT2

#include "JS8_Mode/JS8.h"
#include <cstddef>
#include <cstdint>

struct dec_data;

namespace JS8 {

class DecodeFT2 {
  public:
    DecodeFT2() = default;

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
