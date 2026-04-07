#pragma once
#ifdef JS8_ENABLE_FT2

#include "ft2_bridge.h"
#include <complex>
#include <cstdint>
#include <vector>

namespace FT2 {

// FT2 frame constants (mirror ft2_params.f90)
static constexpr int NN   = 103;  // total symbols per frame
static constexpr int NS   = 16;   // sync symbols (4 Costas × 4 tones)
static constexpr int ND   = 87;   // data symbols
static constexpr int NSPS = 288;  // samples/symbol at 12 kHz
static constexpr int NMAX = 90000;// ring buffer size (7.5 s at 12 kHz)
static constexpr int NDOWN = 9;   // downsample factor  12 kHz → 1333⅓ Hz
static constexpr int NP   = NMAX / NDOWN;  // = 10000 downsampled samples
static constexpr int NSS  = NSPS / NDOWN;  // = 32 samples/symbol after DS
static constexpr int NFFT1 = 1152;         // FFT size for candidate search
static constexpr int NH1   = NFFT1 / 2;   // Nyquist bins = 576
static constexpr int NSTEP = NSPS;         // stepping through the buffer
static constexpr float FS  = 12000.0f;

// Costas sync sequences (Fortran icos4a/b/c/d, 0-indexed)
static constexpr int ICOS4A[4] = {0,1,3,2};
static constexpr int ICOS4B[4] = {1,0,2,3};
static constexpr int ICOS4C[4] = {2,3,1,0};
static constexpr int ICOS4D[4] = {3,2,0,1};

// Reverse-scrambling vector (rvec from genft2.f90)
static constexpr int8_t RVEC[77] = {
    0,1,0,0,1,0,1,0,0,1,0,1,1,1,1,0,1,0,0,0,1,0,0,1,1,0,1,1,0,
    1,0,0,1,0,1,1,0,0,0,0,1,0,0,0,1,0,1,0,0,1,1,1,1,0,0,1,0,1,
    0,1,0,1,0,1,1,0,1,1,1,1,1,0,0,0,1,0,1
};

using Cx = std::complex<float>;

// ---------------------------------------------------------------------------
// GFSK waveform generation
// ---------------------------------------------------------------------------
/**
 * Generate a real GFSK waveform for FT2.
 * Mirrors gen_ft2wave.f90 exactly.
 *
 * @param itone   103 tone values (0-3)
 * @param nsym    number of symbols (normally 103)
 * @param nsps    samples per symbol (1152 at 48 kHz, 288 at 12 kHz)
 * @param fsample sample rate (Hz)
 * @param f0      centre frequency (Hz)
 * @param wave    output buffer, length >= (nsym+2)*nsps
 * @param nwave   output buffer size
 */
void genFT2Wave(const int *itone, int nsym, int nsps,
                float fsample, float f0,
                float *wave, int nwave);

// ---------------------------------------------------------------------------
// Encode bits → tones (ft2_encode_from_bits_c)
// ---------------------------------------------------------------------------
/**
 * XOR msgbits with rvec, LDPC-encode, gray-map, and insert Costas arrays.
 * @param msgbits  77 raw JS8 bits (0/1)
 * @param i4tone   output: 103 tones (0-3)
 */
void encodeBitsToTones(const int8_t *msgbits, int *i4tone);

// ---------------------------------------------------------------------------
// Full decode pipeline (ft2_decode_c)
// ---------------------------------------------------------------------------
/**
 * Decode FT2 signals from a 12 kHz int16 buffer.
 * Invokes callback for each valid decode.
 */
void decodeFT2(const int16_t *iwave, int nmax,
               int nfqso, int nfa, int nfb, int ndepth,
               ft2_callback_t callback, void *ctx);

// ---------------------------------------------------------------------------
// Triggered decode (ft2_triggered_decode_c)
// ---------------------------------------------------------------------------
/**
 * Two-phase Costas scan + targeted LDPC decode.
 * Returns up to 20 decoded frame bit arrays.
 */
void triggeredDecodeFT2(const int16_t *iwave, int nfqso, int nfa, int nfb,
                        int ndepth,
                        int *snr_out, float *dt_out, float *freq_out,
                        float *sync_out, int8_t *msgbits_out, int *ndecoded,
                        const int8_t *known_bits, int nknown,
                        int nfqso_only, float sync_score);

// ---------------------------------------------------------------------------
// Sync scan (ft2_sync_scan_c)
// ---------------------------------------------------------------------------
void syncScanFT2(const int16_t *iwave, int nfreqs, const float *freqs,
                 float *sync_out, float *freq_out,
                 int *ibest_out, int *idf_out);

} // namespace FT2
#endif // JS8_ENABLE_FT2
