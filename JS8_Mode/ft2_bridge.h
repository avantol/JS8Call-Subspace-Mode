#ifndef FT2_BRIDGE_H
#define FT2_BRIDGE_H

/**
 * @file ft2_bridge.h
 * @brief C++ declarations for the Fortran FT2 encoder/decoder bridge.
 *
 * These functions are implemented in lib/ft2/ft2_bridge.f90 using
 * Fortran's bind(C) interoperability. They wrap the OOP ft2_decoder
 * type with a flat C-callable interface.
 */

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback type for decoded FT2 messages.
 *
 * @param sync  Sync power (0.0+)
 * @param snr   SNR estimate in dB
 * @param dt    Time offset in seconds
 * @param freq  Frequency in Hz
 * @param msg   Decoded message (Fortran fixed-length, NOT null-terminated)
 * @param msglen Length of msg (always 37)
 * @param nap   AP type used (0=none, 1-6=AP variants)
 * @param qual  Quality metric (0.0-1.0)
 * @param msgbits77  Raw 77 decoded message bits (after rvec removal)
 * @param ctx   User context pointer passed through from ft2_decode_c
 */
typedef void (*ft2_callback_t)(
    float sync, int snr, float dt, float freq,
    const char *msg, int msglen, int nap, float qual,
    const std::int8_t *msgbits77, void *ctx);

/**
 * Decode FT2 signals from audio samples.
 *
 * @param iwave   Audio samples at 12000 S/s (int16)
 * @param nmax    Number of samples (should be 45000 for FT2)
 * @param nfqso   Expected signal frequency in Hz
 * @param nfa     Low frequency search limit in Hz
 * @param nfb     High frequency search limit in Hz
 * @param ndepth  Decode depth (bits 0-2: 1-3, bit 4: enable averaging)
 * @param callback Function called for each successful decode
 * @param ctx     User context pointer passed to callback
 */
void ft2_decode_c(
    const std::int16_t *iwave, int nmax,
    int nfqso, int nfa, int nfb, int ndepth,
    ft2_callback_t callback, void *ctx);

/**
 * Encode a message to FT2 tones (via pack77).
 *
 * @param msg     Input message (37 chars, space-padded)
 * @param i4tone  Output: 103 tone values (0-3)
 * @param msgsent Output: message as it will be decoded (37 chars)
 */
void ft2_encode_c(
    const char *msg, int *i4tone, char *msgsent);

/**
 * Encode raw 77 message bits to FT2 tones (bypasses pack77).
 *
 * @param msgbits  77-element array of 0/1 values
 * @param i4tone   Output: 103 tone values (0-3)
 */
void ft2_encode_from_bits_c(const std::int8_t *msgbits, int *i4tone);

/**
 * Generate GFSK waveform from tone sequence.
 *
 * @param i4tone  Tone values (0-3)
 * @param nsym    Number of symbols (103)
 * @param nsps    Samples per symbol (1152 at 48kHz, 288 at 12kHz)
 * @param fsample Sample rate in Hz
 * @param f0      Center frequency in Hz
 * @param wave    Output: real waveform samples
 * @param nwave   Number of output samples
 */
void ft2_gen_wave_c(
    const int *i4tone, int nsym, int nsps,
    float fsample, float f0,
    float *wave, int nwave);

/**
 * Level 2 sync-triggered FT2 decoder.
 * Two-phase: Costas scan -> targeted LDPC at confirmed sync positions.
 * Returns raw message77 bits (up to 20 decodes).
 *
 * @param iwave     Audio samples at 12000 S/s (int16), 90000 samples
 * @param nfqso     Expected signal frequency in Hz
 * @param nfa       Low frequency search limit in Hz
 * @param nfb       High frequency search limit in Hz
 * @param ndepth    Decode depth (bits 0-2: 1-3)
 * @param snr_out   Output: SNR for each decode (array of 20)
 * @param dt_out    Output: DT for each decode (array of 20)
 * @param freq_out  Output: frequency for each decode (array of 20)
 * @param sync_out  Output: Costas sync score for each decode (array of 20)
 * @param msgbits_out Output: raw 77 message bits for each decode (77x20)
 * @param ndecoded  Output: number of successful decodes
 */
void ft2_triggered_decode_c(
    const std::int16_t *iwave, int nfqso, int nfa, int nfb, int ndepth,
    int *snr_out, float *dt_out, float *freq_out, float *sync_out,
    std::int8_t *msgbits_out, int *ndecoded,
    const std::int8_t *known_bits, int nknown,
    int nfqso_only);

/**
 * Lightweight sync scanner: downsample + sync2d at a frequency grid.
 * Returns best sync quality across all input frequencies.
 * Used by C++ sync monitor to detect Costas tones before full decode.
 *
 * @param iwave     Audio samples at 12000 S/s (int16), 90000 samples
 * @param nfreqs    Number of frequencies to scan
 * @param freqs     Array of frequencies to scan (Hz)
 * @param sync_out  Output: best sync quality found
 * @param freq_out  Output: frequency of best sync
 * @param ibest_out Output: best time position (downsampled samples)
 * @param idf_out   Output: best frequency offset index
 */
void ft2_sync_scan_c(
    const std::int16_t *iwave, int nfreqs, const float *freqs,
    float *sync_out, float *freq_out, int *ibest_out, int *idf_out);

/** Clear multi-period averaging state (call on band/mode change). */
void ft2_clravg_c(void);

/** Initialize Fortran FFTW patience common block (call once at startup). */
void ft2_init_c(void);

#ifdef __cplusplus
}
#endif

#endif // FT2_BRIDGE_H
