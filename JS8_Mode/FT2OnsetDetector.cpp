/**
 * @file FT2OnsetDetector.cpp
 * @brief Scans ring buffer for FT2 signal onset using short FFT power measurements.
 *
 * Slides a 1152-sample FFT window across the full 90000-sample ring buffer in
 * steps of 288 samples (one FT2 symbol period).  At each step, measures power
 * in a ±100 Hz band around nfqso.  Finds the onset (power rising above noise)
 * and returns the optimal 45000-sample extraction window start.
 */

#ifdef JS8_ENABLE_FT2

#include "FT2OnsetDetector.h"
#include "JS8_Include/commons.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include <QDebug>
#include <fftw3.h>

namespace JS8 {

// FFT parameters matching ft2_params.f90
static constexpr int ONSET_NFFT  = 1152;          // FFT size (same as NFFT1)
static constexpr int ONSET_NH    = ONSET_NFFT / 2; // Nyquist bin count
static constexpr int ONSET_STEP  = 288;            // step size (one symbol = NSPS)
static constexpr float ONSET_FS  = 12000.0f;       // sample rate
static constexpr float ONSET_DF  = ONSET_FS / ONSET_NFFT; // ~10.42 Hz per bin
static constexpr float ONSET_BW  = 100.0f;         // ±100 Hz search band

FT2OnsetResult ft2FindOnset(const std::int16_t *samples, int nsamples, int nfqso) {
    // Fallback result: most recent FT2_NMAX samples
    FT2OnsetResult fallback{};
    fallback.startOffset = std::max(0, nsamples - FT2_NMAX);
    fallback.signalFound = false;
    fallback.peakPowerDb = 0.0f;
    fallback.onsetBin = -1;

    if (nsamples < FT2_NMAX)
        return fallback;

    // Number of FFT steps across the buffer
    int nsteps = (nsamples - ONSET_NFFT) / ONSET_STEP + 1;
    if (nsteps < 10)
        return fallback;

    // Frequency bin range for nfqso ± BW
    int binLo = std::max(1, static_cast<int>((nfqso - ONSET_BW) / ONSET_DF));
    int binHi = std::min(ONSET_NH - 1, static_cast<int>((nfqso + ONSET_BW) / ONSET_DF));
    if (binLo >= binHi)
        return fallback;

    // FFTW plan (created once, process lifetime).
    // All FFTW work (plan creation AND execution) is done under fftw_mutex
    // because dataSink.cpp may create its plan on the audio thread concurrently,
    // and FFTW execution during plan creation is undefined behavior.
    static fftwf_plan s_plan = nullptr;
    static fftwf_complex *s_cx = nullptr;
    static float *s_re = nullptr;

    std::lock_guard<std::mutex> lock(fftw_mutex);

    if (!s_plan) {
        s_cx = fftwf_alloc_complex(ONSET_NH + 1);
        s_re = fftwf_alloc_real(ONSET_NFFT);
        if (s_cx && s_re)
            s_plan = fftwf_plan_dft_r2c_1d(ONSET_NFFT, s_re, s_cx, FFTW_ESTIMATE);
        if (!s_plan) {
            qWarning() << "[FT2-L2] onset: FFT plan creation failed, using fallback";
            return fallback;
        }
    }

    // Compute in-band power at each step
    std::vector<float> power(nsteps);
    constexpr float scale = 1.0f / 32768.0f;

    for (int step = 0; step < nsteps; ++step) {
        int offset = step * ONSET_STEP;

        // Copy samples into FFT input buffer
        for (int i = 0; i < ONSET_NFFT; ++i)
            s_re[i] = samples[offset + i] * scale;

        fftwf_execute(s_plan);

        // Sum power in the nfqso band
        float pwr = 0.0f;
        for (int k = binLo; k <= binHi; ++k) {
            float re = s_cx[k][0];
            float im = s_cx[k][1];
            pwr += re * re + im * im;
        }
        power[step] = pwr;
    }

    // Estimate noise floor: median of all power measurements
    std::vector<float> sorted = power;
    std::nth_element(sorted.begin(), sorted.begin() + nsteps / 2, sorted.end());
    float noiseFloor = sorted[nsteps / 2];

    if (noiseFloor <= 0.0f)
        return fallback;

    // Threshold: 6x noise floor (~8 dB above noise)
    float threshold = noiseFloor * 6.0f;

    // Find onset: first run of 3 consecutive above-threshold steps
    int onsetStep = -1;
    for (int i = 0; i < nsteps - 2; ++i) {
        if (power[i] > threshold && power[i + 1] > threshold && power[i + 2] > threshold) {
            onsetStep = i;
            break;
        }
    }

    if (onsetStep < 0)
        return fallback;

    // Peak power for diagnostics
    float peakPower = *std::max_element(power.begin(), power.end());
    float peakDb = 10.0f * std::log10(peakPower / noiseFloor);

    // Position window: 24 symbols before onset so the decoder's full
    // ibest search range (-688..+2024 downsampled ≈ -6192..+18216 samples)
    // has room on the negative side.
    int onsetSample = onsetStep * ONSET_STEP;
    int windowStart = std::max(0, onsetSample - 24 * ONSET_STEP);

    // Clamp so window doesn't exceed buffer
    if (windowStart + FT2_NMAX > nsamples)
        windowStart = nsamples - FT2_NMAX;

    return FT2OnsetResult{
        .startOffset = windowStart,
        .signalFound = true,
        .peakPowerDb = peakDb,
        .onsetBin = onsetStep
    };
}

} // namespace JS8

#endif // JS8_ENABLE_FT2
