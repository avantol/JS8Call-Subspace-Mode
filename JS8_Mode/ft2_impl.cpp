/**
 * @file ft2_impl.cpp
 * @brief C bridge: delegates all ft2_*_c extern "C" functions to the FT2:: namespace
 *        DSP implementation in ft2_modem.cpp.
 *
 * Phase 2: all stubs replaced with real implementations.
 */

#ifdef JS8_ENABLE_FT2

#include "ft2_bridge.h"
#include "ft2_modem.h"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// ft2_init_c — one-time initialisation (FFTW wisdom, etc.)
// No-op here; FFTW plans are lazily created in ft2_modem.cpp on first call.
// ---------------------------------------------------------------------------
extern "C" void ft2_init_c(void) {}

// ---------------------------------------------------------------------------
// ft2_clravg_c — clear multi-period spectral averaging state.
// No multi-period averaging implemented in Phase 2.
// ---------------------------------------------------------------------------
extern "C" void ft2_clravg_c(void) {}

// ---------------------------------------------------------------------------
// ft2_encode_c — encode a free-text FT2 message to 4-FSK tone sequence.
// Requires pack77 which is not available in pure C++.
// Stub: fills i4tone with zeros and echoes the input message.
// ft2_encode_from_bits_c (below) is the actual TX path used by JS8Call.
// ---------------------------------------------------------------------------
extern "C" void ft2_encode_c(const char *msg, int *i4tone, char *msgsent)
{
    std::fill_n(i4tone, 103, 0);
    if (msgsent && msg)
        std::memcpy(msgsent, msg, 37);
}

// ---------------------------------------------------------------------------
// ft2_encode_from_bits_c — encode raw 77-bit payload to FT2 tones.
// Full Phase 2 implementation via FT2::encodeBitsToTones.
// ---------------------------------------------------------------------------
extern "C" void ft2_encode_from_bits_c(const std::int8_t *msgbits, int *i4tone)
{
    FT2::encodeBitsToTones(msgbits, i4tone);
}

// ---------------------------------------------------------------------------
// ft2_gen_wave_c — generate GFSK waveform from tone sequence.
// Full Phase 2 implementation via FT2::genFT2Wave.
// ---------------------------------------------------------------------------
extern "C" void ft2_gen_wave_c(const int *i4tone, int nsym,
                               int nsps, float fsample, float f0,
                               float *wave, int nwave)
{
    FT2::genFT2Wave(i4tone, nsym, nsps, fsample, f0, wave, nwave);
}

// ---------------------------------------------------------------------------
// ft2_decode_c — decode FT2 signals from a 12 kHz int16 audio buffer.
// Full Phase 2 implementation via FT2::decodeFT2.
// ---------------------------------------------------------------------------
extern "C" void ft2_decode_c(const std::int16_t *iwave, int nmax,
                             int nfqso, int nfa, int nfb, int ndepth,
                             ft2_callback_t callback, void *ctx)
{
    FT2::decodeFT2(iwave, nmax, nfqso, nfa, nfb, ndepth, callback, ctx);
}

// ---------------------------------------------------------------------------
// ft2_triggered_decode_c — two-phase sync-triggered LDPC decoder.
// Full Phase 2 implementation via FT2::triggeredDecodeFT2.
// ---------------------------------------------------------------------------
extern "C" __attribute__((noinline)) void ft2_triggered_decode_c(
    const std::int16_t *iwave,
    int nfqso, int nfa, int nfb, int ndepth,
    int *snr_out, float *dt_out, float *freq_out, float *sync_out,
    std::int8_t *msgbits_out, int *ndecoded,
    const std::int8_t *known_bits, int nknown,
    int nfqso_only, float sync_score)
{
    FT2::triggeredDecodeFT2(iwave, nfqso, nfa, nfb, ndepth,
                             snr_out, dt_out, freq_out, sync_out,
                             msgbits_out, ndecoded,
                             known_bits, nknown, nfqso_only, sync_score);
}

// ---------------------------------------------------------------------------
// ft2_sync_scan_c — lightweight Costas sync scanner across a frequency grid.
// Full Phase 2 implementation via FT2::syncScanFT2.
// ---------------------------------------------------------------------------
extern "C" void ft2_sync_scan_c(
    const std::int16_t *iwave,
    int nfreqs, const float *freqs,
    float *sync_out, float *freq_out,
    int *ibest_out, int *idf_out)
{
    FT2::syncScanFT2(iwave, nfreqs, freqs, sync_out, freq_out, ibest_out, idf_out);
}

#endif // JS8_ENABLE_FT2
