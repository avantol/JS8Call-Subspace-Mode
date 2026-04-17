/**
 * @file ft2_modem.cpp
 * @brief Phase 2: Full C++ DSP implementation of the FT2 modem.
 *
 * Ports the following Fortran files to C++:
 *   lib/ft2/gen_ft2wave.f90, gfsk_pulse.f90, genft2.f90
 *   lib/ft2/ft2_downsample.f90, sync2d.f90, getcandidates2.f90
 *   lib/ft2/get_ft2_bitmetrics.f90, ft2_decode.f90
 *   lib/ft2/ft2_triggered_decode.f90, ft2_sync_scan.f90
 *   lib/ft2_deps/normalizebmet.f90, nuttal_window.f90
 *   lib/ft2_deps/twkfreq1.f90, pctile.f90, polyfit.f90, determ.f90
 *   lib/ft2/ft2_baseline.f90
 */

#ifdef JS8_ENABLE_FT2

#include "ft2_modem.h"
#include "ft2_ldpc.h"
#include "JS8_Include/commons.h"   // extern std::mutex fftw_mutex
#include <memory>                  // std::make_unique (heap allocation for large arrays)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <fftw3.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace FT2 {

// ---------------------------------------------------------------------------
// Additional constants not in the header (NH1, NN, ND, etc. come from ft2_modem.h)
// ---------------------------------------------------------------------------
static constexpr int   NFFT2   = NP;                      // 10 000  (c2c backward)
static constexpr int   NHSYM   = (NMAX - NFFT1) / NSTEP; // 308
static constexpr int   NDMAX   = NP;                      // 10 000  downsampled buffer
static constexpr int   MAXCAND = 300;
static constexpr int   MAXHITS = 50;
static constexpr int   MAXDEC  = 20;
static constexpr float TWOPI   = 6.2831853071795864f;
static constexpr float FS_DOWN = FS / NDOWN;              // 1333.33 Hz

// ---------------------------------------------------------------------------
// Static DSP state (initialised once)
// ---------------------------------------------------------------------------
static struct DspState {
    // ---- FFTW plans (all created once at startup) --------------------------
    fftwf_plan plan_r2c_nmax   = nullptr;  // r2c, size NMAX (downsampling big FFT)
    fftwf_plan plan_c2c_nfft2  = nullptr;  // c2c backward, size NFFT2
    fftwf_plan plan_r2c_nfft1  = nullptr;  // r2c, size NFFT1 (getCandidates)
    fftwf_plan plan_c2c_nss    = nullptr;  // c2c forward, size NSS (getBitMetrics)

    // ---- FFTW-aligned buffers ----------------------------------------------
    float          *x_nmax   = nullptr;   // [NMAX] r2c input
    fftwf_complex  *cx_nmax  = nullptr;   // [NMAX/2+1] r2c output
    fftwf_complex  *c1_nfft2 = nullptr;   // [NFFT2] c2c buffer
    float          *x_nfft1  = nullptr;   // [NFFT1] r2c input
    fftwf_complex  *cx_nfft1 = nullptr;   // [NFFT1/2+1] r2c output
    fftwf_complex  *csymb    = nullptr;   // [NSS] c2c input/output for bit metrics

    // ---- Precomputed windows/sequences -------------------------------------
    float  ds_window[NFFT2];             // downsampling lowpass window
    float  nuttal_win[NFFT1];            // Nuttall window for candidate FFT
    Cx     csynca[2*NSS], csyncb[2*NSS], csyncc[2*NSS], csyncd[2*NSS];
    Cx     ctwk2[33][2*NSS];             // freq tweak factors, idf index = idf+16
    bool   have_big_fft = false;         // cx_nmax valid for current x_nmax?
    bool   initialized  = false;
} g;

static std::once_flag g_init_once;

// ---------------------------------------------------------------------------
// Helper: GFSK pulse (ports gfsk_pulse.f90)
// ---------------------------------------------------------------------------
static float gfsk_pulse(float b, float t) {
    // c = pi * sqrt(2 / ln(2))
    static const float c = (float)(M_PI * sqrt(2.0 / log(2.0)));
    return 0.5f * (erff(c * b * (t + 0.5f)) - erff(c * b * (t - 0.5f)));
}

// ---------------------------------------------------------------------------
// Helper: Nuttall window (ports nuttal_window.f90)
// ---------------------------------------------------------------------------
static void nuttal_window(float *win, int n) {
    static const float a0 =  0.3635819f, a1 = -0.4891775f;
    static const float a2 =  0.1365995f, a3 = -0.0106411f;
    for (int i = 0; i < n; i++) {
        float x = TWOPI * i / n;
        win[i] = a0 + a1*cosf(x) + a2*cosf(2*x) + a3*cosf(3*x);
    }
}

// ---------------------------------------------------------------------------
// Helper: normalise bit-metrics (ports normalizebmet.f90)
// ---------------------------------------------------------------------------
static void normalizeBmet(float *bmet, int n) {
    float sum1 = 0.f, sum2 = 0.f;
    for (int i = 0; i < n; i++) { sum1 += bmet[i]; sum2 += bmet[i]*bmet[i]; }
    float av  = sum1 / n;
    float av2 = sum2 / n;
    float var = av2 - av*av;
    float sig = (var > 0.f) ? sqrtf(var) : sqrtf(av2);
    if (sig > 0.f) for (int i = 0; i < n; i++) bmet[i] /= sig;
}

// ---------------------------------------------------------------------------
// Helper: percentile (ports pctile.f90)
// ---------------------------------------------------------------------------
static float pctile(const float *x, int npts, int npct) {
    if (npts < 1 || npct < 0 || npct > 100) return 1.0f;
    std::vector<float> tmp(x, x + npts);
    int j = (int)std::round(npts * 0.01 * npct);
    j = std::max(1, std::min(j, npts));
    std::nth_element(tmp.begin(), tmp.begin() + j - 1, tmp.end());
    return tmp[j - 1];
}

// ---------------------------------------------------------------------------
// Helper: matrix determinant via Gaussian elimination (ports determ.f90)
// Operates on a local copy so the caller's array is not modified.
// ---------------------------------------------------------------------------
static double determ(double arr[10][10], int norder) {
    double result = 1.0;
    for (int k = 0; k < norder; k++) {
        if (arr[k][k] == 0.0) {
            int jfound = -1;
            for (int j = k; j < norder; j++) {
                if (arr[k][j] != 0.0) { jfound = j; break; }
            }
            if (jfound < 0) return 0.0;
            for (int i = k; i < norder; i++)
                std::swap(arr[i][jfound], arr[i][k]);
            result = -result;
        }
        result *= arr[k][k];
        if (k < norder - 1) {
            for (int i = k+1; i < norder; i++)
                for (int j = k+1; j < norder; j++)
                    arr[i][j] -= arr[i][k] * arr[k][j] / arr[k][k];
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Helper: polynomial least-squares fit (ports polyfit.f90, mode=0)
// Fits polynomial of degree nterms-1 to (x,y) data.
// On output a[0..nterms-1] are the coefficients from lowest to highest degree.
// ---------------------------------------------------------------------------
static void polyfit(const double *x, const double *y,
                    int npts, int nterms, double *a) {
    int nmax = 2 * nterms - 1;
    double sumx[10] = {}, sumy[10] = {};
    for (int i = 0; i < npts; i++) {
        double xi = x[i], yi = y[i];
        double xterm = 1.0;
        for (int n = 0; n < nmax; n++) { sumx[n] += xterm; xterm *= xi; }
        double yterm = yi;
        for (int n = 0; n < nterms; n++) { sumy[n] += yterm; yterm *= xi; }
    }
    // Build normal-equation matrix
    double arr_base[10][10] = {};
    for (int j = 0; j < nterms; j++)
        for (int k = 0; k < nterms; k++)
            arr_base[j][k] = sumx[j + k];

    double delta;
    {
        double tmp[10][10];
        memcpy(tmp, arr_base, sizeof(tmp));
        delta = determ(tmp, nterms);
    }
    if (delta == 0.0) { memset(a, 0, sizeof(double) * nterms); return; }

    for (int l = 0; l < nterms; l++) {
        double tmp[10][10];
        memcpy(tmp, arr_base, sizeof(tmp));
        for (int j = 0; j < nterms; j++)
            tmp[j][l] = sumy[j];
        a[l] = determ(tmp, nterms) / delta;
    }
}

// ---------------------------------------------------------------------------
// Helper: spectral baseline fit (ports ft2_baseline.f90)
// Input:  s[0..NH1-1]  power spectrum (linear scale, 1-indexed in Fortran)
//         nfa, nfb    search range (Fortran 1-indexed bin numbers)
// Output: sbase[0..NH1-1]  baseline (linear scale)
// Side effect: s[nfa-1..nfb-1] is converted to dB (like Fortran in-place).
// ---------------------------------------------------------------------------
static void ft2_baseline(float *s, int nfa, int nfb, float *sbase) {
    static const int nseg = 10, npct = 10, nterms = 5;
    const float df = FS / NFFT1;

    int ia = std::max((int)round(200.0f / df), nfa);
    int ib = std::min(NH1, nfb);                       // Fortran 1-indexed, ib inclusive

    // Convert to dB in-place (Fortran s(ia:ib)=10*log10(s(ia:ib)))
    for (int i = ia; i <= ib; i++) {
        float v = s[i - 1];                             // 0-indexed access
        s[i - 1] = (v > 0.f) ? 10.f * log10f(v) : -99.f;
    }

    int i0 = (ia + ib) / 2;                           // midpoint bin
    int nlen = (ib - ia + 1) / nseg;
    if (nlen < 1) nlen = 1;

    // Collect envelope points (lowest npct% per segment)
    std::vector<double> xpts, ypts;
    for (int n = 0; n < nseg; n++) {
        int ja = ia + n * nlen;
        int jb = std::min(ja + nlen - 1, ib);
        if (ja > ib) break;
        // Percentile over this segment
        std::vector<float> seg;
        for (int i = ja; i <= jb; i++) seg.push_back(s[i - 1]);
        float base = pctile(seg.data(), (int)seg.size(), npct);
        for (int i = ja; i <= jb; i++) {
            if (s[i - 1] <= base) {
                xpts.push_back((double)(i - i0));
                ypts.push_back((double)s[i - 1]);
            }
        }
    }

    // Fit polynomial
    double a[5] = {};
    if (!xpts.empty())
        polyfit(xpts.data(), ypts.data(), (int)xpts.size(), nterms, a);

    // Evaluate baseline and convert back to linear scale
    for (int i = 0; i < NH1; i++) sbase[i] = 0.f;
    for (int i = ia; i <= ib; i++) {
        double t = i - i0;
        double val = a[0] + t*(a[1] + t*(a[2] + t*(a[3] + t*a[4]))) + 0.50;
        sbase[i - 1] = (float)pow(10.0, val / 10.0);
    }
}

// ---------------------------------------------------------------------------
// Helper: twkfreq1 (ports twkfreq1.f90)
// Applies a complex frequency shift to ca[0..npts-1] → cb[0..npts-1].
// a[0] is the frequency in Hz; a[1..4] are higher-order polynomial terms (usually 0).
// fsample is the effective sample rate for the frequency shift interpretation.
// ---------------------------------------------------------------------------
static void twkfreq1(const Cx *ca, int npts, float fsample, const float a[5], Cx *cb) {
    float x0 = 0.5f * (npts + 1);
    float s  = 2.0f / npts;
    Cx w(1.f, 0.f);
    for (int i = 1; i <= npts; i++) {
        float x  = s * (i - x0);
        float p2 = 1.5f*x*x - 0.5f;
        float p3 = 2.5f*(x*x*x) - 1.5f*x;
        float p4 = 4.375f*(x*x*x*x) - 3.75f*(x*x) + 0.375f;
        float dphi = (a[0] + x*a[1] + p2*a[2] + p3*a[3] + p4*a[4]) * (TWOPI / fsample);
        Cx wstep(cosf(dphi), sinf(dphi));
        w *= wstep;
        cb[i-1] = w * ca[i-1];
    }
}

// ---------------------------------------------------------------------------
// One-time static initialisation of all FFTW plans and precomputed tables
// ---------------------------------------------------------------------------
static void initDSP() {
    // Allocate FFTW-aligned buffers
    g.x_nmax   = (float*)         fftwf_malloc(sizeof(float)         * NMAX);
    g.cx_nmax  = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * (NMAX/2 + 1));
    g.c1_nfft2 = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NFFT2);
    g.x_nfft1  = (float*)         fftwf_malloc(sizeof(float)         * NFFT1);
    g.cx_nfft1 = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * (NFFT1/2 + 1));
    g.csymb    = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NSS);

    // Create FFTW plans (all under the project mutex)
    {
        std::lock_guard<std::mutex> lock(fftw_mutex);
        g.plan_r2c_nmax  = fftwf_plan_dft_r2c_1d(NMAX,  g.x_nmax,   g.cx_nmax,  FFTW_ESTIMATE);
        g.plan_c2c_nfft2 = fftwf_plan_dft_1d    (NFFT2, g.c1_nfft2, g.c1_nfft2, FFTW_BACKWARD, FFTW_ESTIMATE);
        g.plan_r2c_nfft1 = fftwf_plan_dft_r2c_1d(NFFT1, g.x_nfft1,  g.cx_nfft1, FFTW_ESTIMATE);
        g.plan_c2c_nss   = fftwf_plan_dft_1d    (NSS,   g.csymb,    g.csymb,    FFTW_FORWARD,  FFTW_ESTIMATE);
    }

    // ------------------------------------------------------------------
    // Downsampling window (ports ft2_downsample.f90 first-time init)
    // Window is a Tukey-like lowpass in frequency domain, then circular-shifted.
    // ------------------------------------------------------------------
    {
        const float df   = FS / NMAX;
        const float baud = FS / NSPS;
        const int   iwt  = (int)(0.5f * baud / df);   // 156
        const int   iwf  = (int)(4.0f * baud / df);   // 1250
        const int   iws  = (int)(baud  / df);          // 312

        float win_tmp[NFFT2] = {};
        for (int j = 0; j < iwt; j++)
            win_tmp[j] = 0.5f * (1.f + cosf((float)M_PI * (iwt - 1 - j) / iwt));
        for (int j = iwt; j < iwt + iwf; j++)
            win_tmp[j] = 1.0f;
        for (int j = 0; j < iwt; j++)
            win_tmp[iwt + iwf + j] = 0.5f * (1.f + cosf((float)M_PI * j / iwt));
        // Circular shift left by iws (cshift in Fortran)
        for (int k = 0; k < NFFT2; k++)
            g.ds_window[k] = win_tmp[(k + iws) % NFFT2];
    }

    // ------------------------------------------------------------------
    // Nuttall window for getCandidates
    // ------------------------------------------------------------------
    nuttal_window(g.nuttal_win, NFFT1);

    // ------------------------------------------------------------------
    // Costas reference sequences (ports sync2d.f90 first-time init)
    // Each array is 2*NSS=64 complex samples = 4 Costas symbols × NSS/2 samples each.
    // Phase is CONTINUOUS across symbols (not reset between symbols).
    // ------------------------------------------------------------------
    {
        Cx *seqs[4] = { g.csynca, g.csyncb, g.csyncc, g.csyncd };
        const int  icos[4][4] = {{0,1,3,2},{1,0,2,3},{2,3,1,0},{3,2,0,1}};
        for (int s = 0; s < 4; s++) {
            int k = 0;
            float phi = 0.f;
            for (int sym = 0; sym < 4; sym++) {
                float dphi = 2.f * TWOPI * icos[s][sym] / NSS;
                for (int j = 0; j < NSS/2; j++) {
                    seqs[s][k++] = Cx(cosf(phi), sinf(phi));
                    phi = fmodf(phi + dphi, TWOPI);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Frequency-tweak factors ctwk2[idf+16][sample] (ports twkfreq1 init)
    // Applied in sync2d: csync2 = ctwk2[idf+16] * csync_a
    // ------------------------------------------------------------------
    {
        float fsample_half = FS_DOWN / 2.0f;   // 666.67 Hz  (same as Fortran fs/2)
        Cx ca[2*NSS];
        std::fill(ca, ca + 2*NSS, Cx(1.f, 0.f));
        float a5[5] = {};
        for (int idf = -16; idf <= 16; idf++) {
            a5[0] = (float)idf;
            twkfreq1(ca, 2*NSS, fsample_half, a5, g.ctwk2[idf + 16]);
        }
    }

    g.initialized  = true;
    g.have_big_fft = false;
}

// ---------------------------------------------------------------------------
// Downsample: ports ft2_downsample.f90
// dd[0..NMAX-1]   input real samples at 12 kHz
// newdata         true  → recompute the big r2c FFT (expensive)
// f0              centre frequency (Hz) – lowest tone of the signal
// c[0..NP-1]      output: complex baseband at FS_DOWN = 1333.33 Hz
// ---------------------------------------------------------------------------
static void ft2Downsample(const float *dd, bool newdata, float f0, Cx *c) {
    if (!g.initialized) return;

    float df = FS / NMAX;   // 0.1333 Hz/bin

    if (newdata) {
        memcpy(g.x_nmax, dd, sizeof(float) * NMAX);
        fftwf_execute_dft_r2c(g.plan_r2c_nmax, g.x_nmax, g.cx_nmax);
        g.have_big_fft = true;
    }

    int i0 = (int)roundf(f0 / df);

    // Build c1[0..NFFT2-1] from the r2c output, shifted to f0
    memset(g.c1_nfft2, 0, sizeof(fftwf_complex) * NFFT2);
    int nHalf = NMAX / 2;   // = 45000, max bin index of cx_nmax

    auto setBin = [&](int k, int src) {
        if (src < 0 || src > nHalf) return;
        g.c1_nfft2[k][0] = g.cx_nmax[src][0];
        g.c1_nfft2[k][1] = g.cx_nmax[src][1];
    };

    setBin(0, i0);
    for (int i = 1; i <= NFFT2 / 2; i++) {
        setBin(i,         i0 + i);
        setBin(NFFT2 - i, i0 - i);
    }

    // Apply window and normalise (divide by NFFT2 so IFFT gives unit-amplitude output)
    for (int k = 0; k < NFFT2; k++) {
        float w = g.ds_window[k] / NFFT2;
        g.c1_nfft2[k][0] *= w;
        g.c1_nfft2[k][1] *= w;
    }

    // Backward c2c FFT → time domain complex baseband
    fftwf_execute(g.plan_c2c_nfft2);  // in-place on g.c1_nfft2

    for (int n = 0; n < NP; n++)
        c[n] = Cx(g.c1_nfft2[n][0], g.c1_nfft2[n][1]);
}

// ---------------------------------------------------------------------------
// sync2d: ports sync2d.f90
// cd0[0..NP-1]   complex downsampled signal
// i0             start sample (may be negative)
// ctwk[0..2*NSS-1] frequency tweak factors (ctwk2[:, idf])
// sync           output: Costas sync power (sum of 4 correlation magnitudes)
// ---------------------------------------------------------------------------
static void sync2d(const Cx *cd0, int i0, const Cx *ctwk, float &sync) {
    static const float fac = 1.0f / (2.0f * NSS);

    Cx z1(0,0), z2(0,0), z3(0,0), z4(0,0);

    auto costas_corr = [&](const Cx *csync_ref, int istart, Cx &z) {
        // Multiply reference by frequency tweak
        Cx csync2[2*NSS];
        for (int k = 0; k < 2*NSS; k++) csync2[k] = ctwk[k] * csync_ref[k];

        z = Cx(0,0);
        int iend = istart + 4*NSS - 1;
        if (istart >= 0 && iend <= NP - 1) {
            // Full window, stride 2
            for (int k = 0; k < 2*NSS; k++)
                z += cd0[istart + 2*k] * std::conj(csync2[k]);
        } else if (istart < 0) {
            int npts = (istart + 4*NSS - 1) / 2;   // available points
            if (npts > 16) {
                int offset = 2*NSS - npts;
                for (int k = 0; k < npts; k++)
                    z += cd0[2*k] * std::conj(csync2[offset + k]);
            }
        } else {
            // iend > NP-1
            int npts = (NP - istart) / 2;
            if (npts > 16) {
                for (int k = 0; k < npts; k++)
                    z += cd0[istart + 2*k] * std::conj(csync2[k]);
            }
        }
    };

    costas_corr(g.csynca, i0,               z1);
    costas_corr(g.csyncb, i0 + 33*NSS,      z2);
    costas_corr(g.csyncc, i0 + 66*NSS,      z3);
    costas_corr(g.csyncd, i0 + 99*NSS,      z4);

    // sync = sum of |z * fac|  (same as Fortran p() statement function)
    sync = (std::abs(z1) + std::abs(z2) + std::abs(z3) + std::abs(z4)) * fac;
}

// ---------------------------------------------------------------------------
// getCandidates: ports getcandidates2.f90
// dd[0..NMAX-1]  real 12 kHz samples
// fa, fb         frequency search range (Hz)
// syncmin        minimum peak height
// nfqso          preferred frequency (Hz, integer)
// candidate[0..MAXCAND-1][0..1]  output: [freq, peak_height] pairs
// ncand          output: number of candidates
// savg[0..NH1-1] output: average spectrum (1-indexed bin 1 = DC+1 in Fortran)
// sbase[0..NH1-1] output: baseline (linear)
// ---------------------------------------------------------------------------
static void getCandidates(const float *dd, float fa, float fb,
                          float syncmin, int nfqso, int maxcand,
                          float savg_out[],     // [NH1]
                          float candidate[][2], int &ncand,
                          float sbase[])        // [NH1]
{
    // s[j][i] (j=0..NHSYM-1, i=0..NH1-1): per-step power spectrum
    // savg[i]: average
    // savsm[i]: smoothed average
    // All 0-indexed, but bin i+1 in the FFTW r2c output corresponds to frequency (i+1)*df.
    // To match Fortran 1-indexed arrays, we shift: savg[i] ↔ Fortran savg(i+1).

    float savg[NH1]  = {};
    float savsm[NH1] = {};
    float sbase_loc[NH1] = {};

    float df  = FS / NFFT1;
    float fac = 1.0f / 300.0f;

    // Compute NHSYM symbol spectra
    for (int j = 0; j < NHSYM; j++) {
        int ia = j * NSTEP;
        int ib = ia + NFFT1;
        if (ib > NMAX) break;
        // Copy and window
        for (int k = 0; k < NFFT1; k++)
            g.x_nfft1[k] = fac * dd[ia + k] * g.nuttal_win[k];
        fftwf_execute_dft_r2c(g.plan_r2c_nfft1, g.x_nfft1, g.cx_nfft1);
        // Accumulate power at bins 1..NH1 (skip DC bin 0)
        for (int b = 0; b < NH1; b++) {
            float re = g.cx_nfft1[b+1][0], im = g.cx_nfft1[b+1][1];
            float pw = re*re + im*im;
            savg[b] += pw;
        }
    }
    // Normalise
    for (int b = 0; b < NH1; b++) savg[b] /= NHSYM;
    if (savg_out) memcpy(savg_out, savg, sizeof(float) * NH1);

    // Smooth with 15-tap box filter (Fortran: do i=8,NH1-7 → 0-indexed: 7..NH1-8)
    for (int i = 7; i < NH1 - 7; i++) {
        float sum = 0.f;
        for (int k = -7; k <= 7; k++) sum += savg[i + k];
        savsm[i] = sum / 15.f;
    }

    // Frequency bounds (Fortran 1-indexed bins, so we use the same integer values
    // but remember our 0-indexed savg[b] = Fortran savg(b+1))
    // Fortran: nfa=int(fa/df), loop do i=nfa+1,nfb-1
    // We interpret: loop i=nfa..nfb-2 (0-indexed savg[i] = Fortran savg(i+1))
    int nfa = (int)(fa / df);
    int nfb = (int)(fb / df);
    nfa = std::max(nfa, (int)roundf(200.0f / df));
    nfb = std::min(nfb, (int)roundf(4910.0f / df));
    // Clamp to array bounds (0-indexed: 0..NH1-1)
    nfa = std::max(nfa, 1);   // at least bin 1
    nfb = std::min(nfb, NH1 - 1);

    // Baseline fit (uses Fortran 1-indexed bins, so nfa+1 and nfb+1)
    ft2_baseline(savg, nfa + 1, nfb + 1, sbase_loc);
    if (sbase) memcpy(sbase, sbase_loc, sizeof(float) * NH1);

    // Check baseline is positive
    bool baseline_ok = true;
    for (int i = nfa; i <= nfb; i++) {
        if (sbase_loc[i] <= 0.f) { baseline_ok = false; break; }
    }
    if (!baseline_ok) { ncand = 0; return; }

    // Normalise smoothed spectrum by baseline
    for (int i = nfa; i <= nfb; i++)
        savsm[i] /= sbase_loc[i];

    // Pick spectral peaks
    float f_offset = -1.5f * FS / NSPS;   // -62.5 Hz
    float candidatet[MAXCAND][2] = {};
    int ncand_t = 0;

    // Loop: i=nfa+1..nfb-1 in Fortran (1-indexed) → i=nfa..nfb-2 in 0-indexed
    for (int i = nfa; i <= nfb - 2 && ncand_t < maxcand; i++) {
        if (savsm[i] >= savsm[i-1] && savsm[i] >= savsm[i+1] && savsm[i] >= syncmin) {
            float den = savsm[i-1] - 2*savsm[i] + savsm[i+1];
            float del = (den != 0.f) ? 0.5f*(savsm[i-1] - savsm[i+1]) / den : 0.f;
            // Fortran: fpeak=(i+del)*df+f_offset where i is 1-indexed bin
            // Our i is 0-indexed, Fortran i = our i + 1
            float fpeak = ((i + 1) + del) * df + f_offset;
            if (fpeak < 200.f || fpeak > 4910.f) continue;
            float speak = savsm[i] - 0.25f * (savsm[i-1] - savsm[i+1]) * del;
            candidatet[ncand_t][0] = fpeak;
            candidatet[ncand_t][1] = speak;
            ncand_t++;
        }
    }

    // Partition: candidates near nfqso first (within 20 Hz)
    ncand = 0;
    int n1 = 0;
    for (int i = 0; i < ncand_t; i++) {
        if (fabsf(candidatet[i][0] - nfqso) <= 20.f) n1++;
    }
    int head = 0, tail = n1;
    for (int i = 0; i < ncand_t; i++) {
        if (fabsf(candidatet[i][0] - nfqso) <= 20.f) {
            candidate[head][0] = candidatet[i][0];
            candidate[head][1] = candidatet[i][1];
            head++;
        } else {
            candidate[tail][0] = candidatet[i][0];
            candidate[tail][1] = candidatet[i][1];
            tail++;
        }
    }
    ncand = ncand_t;
}

// ---------------------------------------------------------------------------
// getBitMetrics: ports get_ft2_bitmetrics.f90
// cd[0..NN*NSS-1]       complex downsampled signal, aligned to frame start
// bitmetrics[0..2*NN-1][0..2]  output soft-decision bit metrics
// badsync               output: true if Costas hard-decision fails sanity check
// ---------------------------------------------------------------------------
static void getBitMetrics(const Cx *cd, float bitmetrics[2*NN][3], bool &badsync) {
    static const int graymap[4] = {0, 1, 3, 2};
    static const int icos4a[4]  = {0, 1, 3, 2};
    static const int icos4b[4]  = {1, 0, 2, 3};
    static const int icos4c[4]  = {2, 3, 1, 0};
    static const int icos4d[4]  = {3, 2, 0, 1};

    // Per-symbol FFT results: cs[tone][symbol]
    static Cx  cs[4][NN];
    static float s4[4][NN];   // magnitudes

    // FFT each symbol block
    for (int k = 0; k < NN; k++) {
        int i1 = k * NSS;
        for (int n = 0; n < NSS; n++) {
            g.csymb[n][0] = cd[i1 + n].real();
            g.csymb[n][1] = cd[i1 + n].imag();
        }
        fftwf_execute(g.plan_c2c_nss);   // in-place forward FFT
        // Take tones 0-3 (Fortran 1-indexed csymb(1:4) = 0-indexed bins 0..3)
        for (int t = 0; t < 4; t++) {
            cs[t][k] = Cx(g.csymb[t][0], g.csymb[t][1]);
            s4[t][k] = std::abs(cs[t][k]);
        }
    }

    // Sync quality check: hard-decision on Costas symbols
    badsync = false;
    int is1=0, is2=0, is3=0, is4=0;
    for (int i = 0; i < 4; i++) {
        int pk;
        pk = 0; for (int t=1;t<4;t++) if(s4[t][i]>s4[pk][i]) pk=t;
        if (icos4a[i] == pk) is1++;
        pk = 0; for (int t=1;t<4;t++) if(s4[t][33+i]>s4[pk][33+i]) pk=t;
        if (icos4b[i] == pk) is2++;
        pk = 0; for (int t=1;t<4;t++) if(s4[t][66+i]>s4[pk][66+i]) pk=t;
        if (icos4c[i] == pk) is3++;
        pk = 0; for (int t=1;t<4;t++) if(s4[t][99+i]>s4[pk][99+i]) pk=t;
        if (icos4d[i] == pk) is4++;
    }
    int nsync = is1 + is2 + is3 + is4;
    if (nsync < 4) { badsync = true; return; }

    // Initialise bitmetrics
    memset(bitmetrics, 0, sizeof(float) * 2*NN * 3);

    // Compute soft metrics for nseq=1,2,3 (nsym=1,2,4)
    for (int nseq = 1; nseq <= 3; nseq++) {
        int nsym  = (nseq == 1) ? 1 : (nseq == 2) ? 2 : 4;
        int nt    = 1 << (2 * nsym);   // 4, 16, 256
        int ibmax = (nsym == 1) ? 1 : (nsym == 2) ? 3 : 7;

        std::vector<float> s2(nt);

        int ks_max = NN - nsym + 1;
        for (int ks = 0; ks < ks_max; ks += nsym) {  // 0-indexed ks
            // Build hypothesis metrics
            for (int hyp = 0; hyp < nt; hyp++) {
                int i1 = (hyp >> 6) & 3;   // for nsym=4, bits 6-7
                int i2 = (hyp >> 4) & 3;   // bits 4-5
                int i3 = (hyp >> 2) & 3;   // bits 2-3
                int i4 =  hyp       & 3;   // bits 0-1

                Cx sum_c(0, 0);
                float sum_r = 0.f;

                if (nsym == 1) {
                    sum_r = std::abs(cs[graymap[i4]][ks]);
                } else if (nsym == 2) {
                    sum_c = cs[graymap[i3]][ks] + cs[graymap[i4]][ks + 1];
                    sum_r = std::abs(sum_c);
                } else {
                    sum_c = cs[graymap[i1]][ks  ] + cs[graymap[i2]][ks+1]
                          + cs[graymap[i3]][ks+2] + cs[graymap[i4]][ks+3];
                    sum_r = std::abs(sum_c);
                }
                s2[hyp] = sum_r;
            }

            // Fortran 1-indexed ipt: ipt=1+(ks)*2 (ks is 0-indexed here)
            int ipt = 2 * ks;   // 0-indexed → bitmetrics[ipt+ib][nseq-1]

            for (int ib = 0; ib <= ibmax; ib++) {
                int bitsel = ibmax - ib;  // which bit to extract
                float bm_pos = -1e9f, bm_neg = -1e9f;
                for (int hyp = 0; hyp < nt; hyp++) {
                    bool bit_set = (hyp >> bitsel) & 1;
                    if (bit_set) bm_pos = std::max(bm_pos, s2[hyp]);
                    else         bm_neg = std::max(bm_neg, s2[hyp]);
                }
                int pos = ipt + ib;
                if (pos < 2*NN)
                    bitmetrics[pos][nseq - 1] = bm_pos - bm_neg;
            }
        }
    }

    // Fixup last few positions (mirrors Fortran patch at end of get_ft2_bitmetrics)
    // bitmetrics(205:206,2)=bitmetrics(205:206,1) → 0-indexed: [204..205][1]=[204..205][0]
    // bitmetrics(201:204,3)=bitmetrics(201:204,2) → [200..203][2]=[200..203][1]
    // bitmetrics(205:206,3)=bitmetrics(205:206,1) → [204..205][2]=[204..205][0]
    for (int i = 204; i <= 205; i++) bitmetrics[i][1] = bitmetrics[i][0];
    for (int i = 200; i <= 203; i++) bitmetrics[i][2] = bitmetrics[i][1];
    for (int i = 204; i <= 205; i++) bitmetrics[i][2] = bitmetrics[i][0];

    // Normalise each nseq column
    for (int s = 0; s < 3; s++) {
        float col[2*NN];
        for (int i = 0; i < 2*NN; i++) col[i] = bitmetrics[i][s];
        normalizeBmet(col, 2*NN);
        for (int i = 0; i < 2*NN; i++) bitmetrics[i][s] = col[i];
    }
}

// ---------------------------------------------------------------------------
// Extract LLRs from bitmetrics (common to both decode functions)
// bitmetrics[0..2*NN-1][0..2]
// llra/b/c/d/e[0..2*ND-1]
// ---------------------------------------------------------------------------
static void extractLLR(const float bitmetrics[2*NN][3],
                        float llra[], float llrb[], float llrc[],
                        float llrd[], float llre[]) {
    static const float scalefac = 2.83f;

    // Fortran (1-indexed):
    //   llr(  1: 58)=bitmetrics(  9: 66, nseq)
    //   llr( 59:116)=bitmetrics( 75:132, nseq)
    //   llr(117:174)=bitmetrics(141:198, nseq)
    // 0-indexed: llr[0..57]=bm[8..65], llr[58..115]=bm[74..131], llr[116..173]=bm[140..197]
    auto extract = [&](float *llr, int col) {
        for (int i = 0; i <  58; i++) llr[       i] = bitmetrics[8  + i][col];
        for (int i = 0; i <  58; i++) llr[58  + i] = bitmetrics[74 + i][col];
        for (int i = 0; i <  58; i++) llr[116 + i] = bitmetrics[140+ i][col];
        for (int i = 0; i < 2*ND; i++) llr[i] *= scalefac;
    };
    extract(llra, 0);
    extract(llrb, 1);
    extract(llrc, 2);

    // llrd = best-of (max |value|)
    // llre = average-of-3
    for (int i = 0; i < 2*ND; i++) {
        float va = fabsf(llra[i]), vb = fabsf(llrb[i]), vc = fabsf(llrc[i]);
        if (va >= vb && va >= vc)       llrd[i] = llra[i];
        else if (vb >= vc)              llrd[i] = llrb[i];
        else                            llrd[i] = llrc[i];
        llre[i] = (llra[i] + llrb[i] + llrc[i]) / 3.0f;
    }
}

// ---------------------------------------------------------------------------
// checkSyncQual: verify Costas hard-decision patterns
// Returns nsync_qual (0..32 maximum)
// ---------------------------------------------------------------------------
static int checkSyncQual(const float bitmetrics[2*NN][3]) {
    // Hard decisions on column 0 (nseq=1)
    int8_t hbits[2*NN];
    for (int i = 0; i < 2*NN; i++) hbits[i] = (bitmetrics[i][0] >= 0.f) ? 1 : 0;

    // Expected Costas bit patterns (0-indexed: positions 0..7, 66..73, 132..139, 198..205)
    static const int8_t pat_a[8] = {0,0,0,1,1,0,1,1};  // Costas A: tones 0,1,3,2
    static const int8_t pat_b[8] = {0,1,0,0,1,1,1,0};  // Costas B: tones 1,0,2,3
    static const int8_t pat_c[8] = {1,1,1,0,0,1,0,0};  // Costas C: tones 2,3,1,0
    static const int8_t pat_d[8] = {1,0,1,1,0,0,0,1};  // Costas D: tones 3,2,0,1

    int ns1=0, ns2=0, ns3=0, ns4=0;
    for (int i = 0; i < 8; i++) {
        if (hbits[     i] == pat_a[i]) ns1++;
        if (hbits[66 + i] == pat_b[i]) ns2++;
        if (hbits[132+ i] == pat_c[i]) ns3++;
        if (hbits[198+ i] == pat_d[i]) ns4++;
    }
    return ns1 + ns2 + ns3 + ns4;
}

// ---------------------------------------------------------------------------
// runLDPCPasses: try multiple LLR combinations until decode succeeds
// Returns nharderror (≥0 on success, -1 on failure)
// On success, fills message77[0..76] with decoded bits (after rvec removal).
// ---------------------------------------------------------------------------
static int runLDPCPasses(const float *llra, const float *llrb, const float *llrc,
                         const float *llrd, const float *llre,
                         int npassmax, int8_t *message77) {
    static const int8_t rvec[77] = {
        0,1,0,0,1,0,1,0,0,1,0,1,1,1,1,0,1,0,0,0,1,0,0,1,1,0,1,1,0,
        1,0,0,1,0,1,1,0,0,0,0,1,0,0,0,1,0,1,0,0,1,1,1,1,0,0,1,0,1,
        0,1,0,1,0,1,1,0,1,1,1,1,1,0,0,0,1,0,1
    };

    int nharderror = -1;
    const float *passes[5] = { llrd, llre, llra, llrb, llrc };

    for (int ipass = 0; ipass < npassmax; ipass++) {
        int8_t  message91[91];
        int8_t  cw[2*ND];
        int     nhe;

        bool ok = decode174_91_bp(passes[ipass], message91, cw, &nhe);
        if (ok && nhe >= 0) {
            nharderror = nhe;
            // Copy information bits, un-XOR rvec
            for (int i = 0; i < 77; i++)
                message77[i] = (message91[i] ^ rvec[i]) & 1;
            break;
        }
    }
    return nharderror;
}

// ===========================================================================
// Public interface functions (declared in ft2_modem.h)
// ===========================================================================

// ---------------------------------------------------------------------------
// genFT2Wave — GFSK waveform generation (ports gen_ft2wave.f90)
// ---------------------------------------------------------------------------
void genFT2Wave(const int *itone, int nsym, int nsps,
                float fsample, float f0,
                float *wave, int nwave) {
    static float s_pulse[3456];     // 288*4*3 max
    static int   s_last_nsps    = -1;
    static float s_last_fsample = -1.f;

    if (nsps != s_last_nsps || fsample != s_last_fsample) {
        for (int i = 0; i < 3*nsps; i++) {
            float tt = ((float)i - 1.5f*nsps + 1.f) / nsps;
            s_pulse[i] = gfsk_pulse(1.0f, tt);
        }
        s_last_nsps    = nsps;
        s_last_fsample = fsample;
    }

    int ntot = (nsym + 2) * nsps;
    std::vector<float> dphi(ntot, 0.f);

    float dphi_peak = TWOPI / nsps;   // = 2*pi*hmod/nsps, hmod=1

    for (int j = 0; j < nsym; j++) {
        int ib = j * nsps;
        for (int k = 0; k < 3*nsps && ib+k < ntot; k++)
            dphi[ib + k] += dphi_peak * s_pulse[k] * itone[j];
    }

    float dt = 1.0f / fsample;
    for (int i = 0; i < ntot; i++) dphi[i] += TWOPI * f0 * dt;

    memset(wave, 0, sizeof(float) * nwave);
    float phi = 0.f;
    int n = std::min(ntot, nwave);
    for (int j = 0; j < n; j++) {
        wave[j] = sinf(phi);
        phi = fmodf(phi + dphi[j], TWOPI);
    }

    // Ramp-up: samples 0..nsps-1
    for (int k = 0; k < nsps && k < nwave; k++) {
        float env = (1.0f - cosf(TWOPI * k / (2.f * nsps))) * 0.5f;
        wave[k] *= env;
    }
    // Ramp-down: samples (nsym+1)*nsps .. (nsym+2)*nsps-1
    int k1 = (nsym + 1) * nsps;
    for (int k = 0; k < nsps && k1+k < nwave; k++) {
        float env = (1.0f + cosf(TWOPI * k / (2.f * nsps))) * 0.5f;
        wave[k1 + k] *= env;
    }
}

// ---------------------------------------------------------------------------
// encodeBitsToTones — XOR + LDPC + Gray map + Costas (ports genft2.f90 entry)
// ---------------------------------------------------------------------------
void encodeBitsToTones(const int8_t *msgbits, int *i4tone) {
    static const int8_t rvec[77] = {
        0,1,0,0,1,0,1,0,0,1,0,1,1,1,1,0,1,0,0,0,1,0,0,1,1,0,1,1,0,
        1,0,0,1,0,1,1,0,0,0,0,1,0,0,0,1,0,1,0,0,1,1,1,1,0,0,1,0,1,
        0,1,0,1,0,1,1,0,1,1,1,1,1,0,0,0,1,0,1
    };

    // XOR with rvec
    int8_t msg_scrambled[77];
    for (int i = 0; i < 77; i++)
        msg_scrambled[i] = (msgbits[i] ^ rvec[i]) & 1;

    // LDPC encode → 174-bit codeword
    int8_t codeword[2*ND];
    encode174_91(msg_scrambled, codeword);

    // Gray mapping: is = codeword[2i+1] + 2*codeword[2i]  (0-indexed)
    // is=0→0, is=1→1, is=2→3, is=3→2
    static const int gray2tone[4] = {0, 1, 3, 2};
    int itmp[ND];
    for (int i = 0; i < ND; i++) {
        int is = codeword[2*i + 1] + 2 * codeword[2*i];
        itmp[i] = gray2tone[is];
    }

    // Insert Costas arrays and data tones (0-indexed i4tone[0..102])
    // icos4a at [0..3], data[0..28] at [4..32], icos4b at [33..36], etc.
    for (int k = 0; k < 4;  k++) i4tone[k]      = ICOS4A[k];
    for (int k = 0; k < 29; k++) i4tone[4  + k] = itmp[k];
    for (int k = 0; k < 4;  k++) i4tone[33  + k] = ICOS4B[k];
    for (int k = 0; k < 29; k++) i4tone[37  + k] = itmp[29 + k];
    for (int k = 0; k < 4;  k++) i4tone[66  + k] = ICOS4C[k];
    for (int k = 0; k < 29; k++) i4tone[70  + k] = itmp[58 + k];
    for (int k = 0; k < 4;  k++) i4tone[99  + k] = ICOS4D[k];
}

// ---------------------------------------------------------------------------
// decodeFT2 — main decode pipeline (simplified port of ft2_decode.f90)
// ---------------------------------------------------------------------------
void decodeFT2(const int16_t *iwave, int nmax,
               int nfqso, int nfa, int nfb, int ndepth,
               ft2_callback_t callback, void *ctx) {
    std::call_once(g_init_once, initDSP);
    if (!g.initialized) return;
    if (nmax < 30000) return;

    // Convert to real (heap-allocated: NMAX=90000 floats = 360KB, too large for stack)
    auto dd_buf = std::make_unique<float[]>(NMAX);
    float *dd = dd_buf.get();
    int nsamp = std::min(nmax, NMAX);
    for (int i = 0; i < nsamp; i++) dd[i] = (float)iwave[i];
    for (int i = nsamp; i < NMAX; i++) dd[i] = 0.f;

    int ndepth0 = ndepth & 7;
    float syncmin  = (ndepth0 >= 3) ? 0.80f : (ndepth0 >= 2) ? 0.85f : 0.90f;
    float smaxthresh = (ndepth0 >= 3) ? 0.75f : 0.90f;
    int   nsync_qual_min = (ndepth0 >= 3) ? 12 : 15;
    int   nsp = (ndepth0 == 1) ? 1 : 3;

    float candidate[MAXCAND][2];
    float savg[NH1] = {}, sbase[NH1] = {};

    // Decoded messages (for deduplication)
    static const int MAXDEC_LOC = 100;
    int8_t decodes[MAXDEC_LOC][77] = {};
    int    ndecodes = 0;

    for (int isp = 0; isp < nsp; isp++) {
        if (isp == 1 && ndecodes == 0) break;

        int ncand = 0;
        getCandidates(dd, (float)nfa, (float)nfb, syncmin, nfqso, MAXCAND,
                      savg, candidate, ncand, sbase);
        if (ncand == 0) break;

        bool dobigfft = true;
        for (int icand = 0; icand < ncand; icand++) {
            float f0  = candidate[icand][0];
            float snr = candidate[icand][1] - 1.0f;

            // Downsample (heap-allocated: NP=10000 complex<float> = 80KB, too large for stack)
            auto cd2_buf = std::make_unique<Cx[]>(NP);
            Cx *cd2 = cd2_buf.get();
            ft2Downsample(dd, dobigfft, f0, cd2);
            if (dobigfft) dobigfft = false;

            // Normalise
            float sum2 = 0.f;
            for (int n = 0; n < NP; n++) sum2 += std::norm(cd2[n]);
            sum2 /= (float)NP;
            if (sum2 > 0.f) { float inv = 1.f / sqrtf(sum2); for (int n = 0; n < NP; n++) cd2[n] *= inv; }

            // Sync search in 3 segments
            float smax1 = 0.f;

            for (int iseg = 0; iseg < 3; iseg++) {
                // Coarse search
                int idfmin=-12, idfmax=12, idfstp=3;
                int ibmin, ibmax, ibstp=4;
                int ndmax_seg = NDMAX - NN*NSS + 320;
                if (iseg == 0) { ibmin=216;    ibmax=ndmax_seg/2; }
                else if (iseg == 1) { ibmin=ndmax_seg/2; ibmax=ndmax_seg; }
                else { ibmin=-688; ibmax=216; }

                float smax = -99.f; int ibest=-1, idfbest=0;
                for (int idf = idfmin; idf <= idfmax; idf += idfstp) {
                    const Cx *ctwk = g.ctwk2[idf + 16];
                    for (int istart = ibmin; istart <= ibmax; istart += ibstp) {
                        float sync; sync2d(cd2, istart, ctwk, sync);
                        if (sync > smax) { smax=sync; ibest=istart; idfbest=idf; }
                    }
                }
                if (iseg == 0) smax1 = smax;

                // Fine search
                for (int idf = idfbest-4; idf <= idfbest+4; idf++) {
                    const Cx *ctwk = g.ctwk2[idf + 16];
                    for (int istart = ibest-5; istart <= ibest+5; istart++) {
                        float sync; sync2d(cd2, istart, ctwk, sync);
                        if (sync > smax) { smax=sync; ibest=istart; idfbest=idf; }
                    }
                }

                if (smax < smaxthresh) continue;
                if (iseg > 0 && smax < smax1) continue;

                float f1 = f0 + (float)idfbest;
                if (f1 <= 10.f || f1 >= 4990.f) continue;

                // Re-downsample at corrected frequency (heap-allocated: NP=10000 Cx = 80KB)
                auto cb_buf = std::make_unique<Cx[]>(NP);
                Cx *cb = cb_buf.get();
                ft2Downsample(dd, false, f1, cb);
                float sum_cb = 0.f;
                for (int n = 0; n < NP; n++) sum_cb += std::norm(cb[n]);
                sum_cb /= (float)(NSS * NN);
                if (sum_cb > 0.f) { float inv=1.f/sqrtf(sum_cb); for(int n=0;n<NP;n++) cb[n]*=inv; }

                // Extract signal at ibest (heap-allocated: NN*NSS=3296 Cx = 26KB)
                auto cd_buf = std::make_unique<Cx[]>(NN*NSS);
                Cx *cd = cd_buf.get();
                if (ibest >= 0) {
                    int np = std::min(NDMAX - ibest, NN*NSS);
                    memcpy(cd, cb + ibest, sizeof(Cx) * np);
                } else {
                    int off  = -ibest;
                    int np   = NN*NSS + 2*ibest;
                    if (np > 0) memcpy(cd + off, cb, sizeof(Cx) * std::min(np, NP));
                }

                // Bit metrics
                float bitmetrics[2*NN][3];
                bool badsync;
                getBitMetrics(cd, bitmetrics, badsync);
                if (badsync) continue;

                int nsq = checkSyncQual(bitmetrics);
                if (nsq < nsync_qual_min) continue;

                // Extract and try LLR combinations
                float llra[2*ND], llrb[2*ND], llrc[2*ND], llrd[2*ND], llre[2*ND];
                extractLLR(bitmetrics, llra, llrb, llrc, llrd, llre);

                int8_t message77[77];
                int nharderror = runLDPCPasses(llra, llrb, llrc, llrd, llre, 5, message77);
                if (nharderror < 0) continue;

                // Deduplication
                bool idupe = false;
                for (int d = 0; d < ndecodes; d++) {
                    if (memcmp(decodes[d], message77, 77) == 0) { idupe=true; break; }
                }
                if (idupe) continue;

                if (ndecodes < MAXDEC_LOC)
                    memcpy(decodes[ndecodes++], message77, 77);

                // Compute output metadata
                float xsnr = (snr > 0.f) ? 10.f*log10f(snr) - 13.f : -21.f;
                int   nsnr = (int)roundf(std::max(-21.f, xsnr));
                float xdt  = (float)ibest / 1333.33f - 0.5f;
                float qual = 1.0f - nharderror / 60.0f;

                // Call back with empty text message (caller uses msgbits77)
                char empty[37];
                std::fill_n(empty, 37, ' ');
                callback(smax, nsnr, xdt, f1, empty, 37, 0, qual, message77, ctx);
                break;  // found a decode this segment
            }
        }
    }
}

// ---------------------------------------------------------------------------
// triggeredDecodeFT2 — two-phase sync-triggered decoder (ports ft2_triggered_decode.f90)
// ---------------------------------------------------------------------------
__attribute__((noinline))
void triggeredDecodeFT2(const int16_t *iwave, int nfqso, int nfa, int nfb,
                        int ndepth,
                        int *snr_out, float *dt_out, float *freq_out,
                        float *sync_out, int8_t *msgbits_out, int *ndecoded,
                        const int8_t *known_bits, int nknown,
                        int nfqso_only, float sync_score) {
    std::call_once(g_init_once, initDSP);

    if (ndecoded) *ndecoded = 0;
    if (snr_out)  std::fill_n(snr_out,  MAXDEC, 0);
    if (dt_out)   std::fill_n(dt_out,   MAXDEC, 0.f);
    if (freq_out) std::fill_n(freq_out, MAXDEC, 0.f);
    if (sync_out) std::fill_n(sync_out, MAXDEC, 0.f);

    if (!g.initialized) return;

    // heap-allocated: NMAX=90000 floats = 360KB, too large for stack
    auto dd_buf = std::make_unique<float[]>(NMAX);
    float *dd = dd_buf.get();
    for (int i = 0; i < NMAX; i++) dd[i] = (float)iwave[i];

    int ndepth0 = ndepth & 7;

    // ===================================================================
    // PHASE 1: Fast Costas Scan
    // ===================================================================
    float hit_freq[MAXHITS], hit_snr[MAXHITS];
    int   hit_ibest[MAXHITS], hit_idf[MAXHITS];
    int   nhits = 0;

    float candidate[MAXCAND][2];
    float savg[NH1], sbase[NH1];
    int   ncand = 0;

    if (nfqso_only == 1) {
        ncand = 1;
        candidate[0][0] = (float)nfqso;
        candidate[0][1] = std::max(sync_score, 0.f);
    } else {
        // Lower candidate threshold when no known frames in buffer —
        // single-frame messages (HAIL) have less Costas energy in the
        // 90K-sample buffer, producing weaker sync peaks.
        float cand_syncmin = (nknown == 0) ? 0.40f : 0.60f;
        getCandidates(dd, (float)nfa, (float)nfb, cand_syncmin, nfqso, MAXCAND,
                      savg, candidate, ncand, sbase);
    }

    float syncmin_scan = (nknown == 0) ? 0.35f
                       : (ndepth0 >= 3) ? 0.50f : 0.60f;
    bool dobigfft = true;

    for (int icand = 0; icand < ncand && nhits < MAXHITS; icand++) {
        float f0   = candidate[icand][0];
        float snr0 = candidate[icand][1] - 1.0f;

        // heap-allocated: NP=10000 Cx = 80KB, too large for stack
        auto cd2_buf = std::make_unique<Cx[]>(NP);
        Cx *cd2 = cd2_buf.get();
        ft2Downsample(dd, dobigfft, f0, cd2);
        if (dobigfft) dobigfft = false;

        float sum2 = 0.f;
        for (int n = 0; n < NP; n++) sum2 += std::norm(cd2[n]);
        sum2 /= (float)NP;
        if (sum2 > 0.f) { float inv=1.f/sqrtf(sum2); for(int n=0;n<NP;n++) cd2[n]*=inv; }

        // Coarse scan: DT step 8, freq offset ±12 step 3
        int   ibest_c = -1, idfbest_c = 0;
        float smax_c  = -99.f;
        for (int idf = -12; idf <= 12; idf += 3) {
            const Cx *ctwk = g.ctwk2[idf + 16];
            int ndmax_scan = NDMAX - NN*NSS + 320;
            for (int istart = -688; istart <= ndmax_scan; istart += 8) {
                float sync; sync2d(cd2, istart, ctwk, sync);
                if (sync > smax_c) { smax_c=sync; ibest_c=istart; idfbest_c=idf; }
            }
        }

        if (smax_c >= syncmin_scan) {
            hit_freq[nhits]  = f0;
            hit_ibest[nhits] = ibest_c;
            hit_idf[nhits]   = idfbest_c;
            hit_snr[nhits]   = snr0;
            nhits++;
        }
    }

    // ===================================================================
    // PHASE 2: Targeted Decode at Sync Positions
    // ===================================================================
    int local_ndecoded = 0;
    int8_t prev_bits[MAXDEC][77] = {};

    float smaxthresh = (ndepth0 >= 3) ? 0.65f : 0.80f;
    int   nsync_qual_min = (ndepth0 >= 3) ? 18 : 20;

    for (int ihit = 0; ihit < nhits && local_ndecoded < MAXDEC; ihit++) {
        float f0     = hit_freq[ihit];
        int   ibest0 = hit_ibest[ihit];
        int   idf0   = hit_idf[ihit];
        float snr0   = hit_snr[ihit];

        // Re-downsample (reuse cached FFT) — heap-allocated: NP=10000 Cx = 80KB
        auto cd2_buf2 = std::make_unique<Cx[]>(NP);
        Cx *cd2 = cd2_buf2.get();
        ft2Downsample(dd, false, f0, cd2);
        float sum2 = 0.f;
        for (int n=0;n<NP;n++) sum2+=std::norm(cd2[n]);
        sum2 /= (float)NP;
        if (sum2 > 0.f) { float inv=1.f/sqrtf(sum2); for(int n=0;n<NP;n++) cd2[n]*=inv; }

        // Fine sync around Phase 1 position
        int   ibest = ibest0, idfbest = idf0;
        float smax  = -99.f;
        int idf_lo = std::max(-16, idf0 - 4), idf_hi = std::min(16, idf0 + 4);
        for (int idf = idf_lo; idf <= idf_hi; idf++) {
            const Cx *ctwk = g.ctwk2[idf + 16];
            for (int istart = ibest0 - 8; istart <= ibest0 + 8; istart++) {
                float sync; sync2d(cd2, istart, ctwk, sync);
                if (sync > smax) { smax=sync; ibest=istart; idfbest=idf; }
            }
        }
        if (smax < smaxthresh) continue;

        float f1 = f0 + (float)idfbest;
        if (f1 <= 10.f || f1 >= 4990.f) continue;

        // Final downsample at corrected frequency — heap-allocated: NP=10000 Cx = 80KB
        auto cb_buf = std::make_unique<Cx[]>(NP);
        Cx *cb = cb_buf.get();
        ft2Downsample(dd, false, f1, cb);
        float sum_cb = 0.f;
        for (int n=0;n<NP;n++) sum_cb+=std::norm(cb[n]);
        sum_cb /= (float)NDMAX;
        if (sum_cb > 0.f) { float inv=1.f/sqrtf(sum_cb); for(int n=0;n<NP;n++) cb[n]*=inv; }

        // Extract signal at known position (heap-allocated: NN*NSS=3296 Cx = 26KB)
        auto cd_buf = std::make_unique<Cx[]>(NN*NSS);
        Cx *cd = cd_buf.get();
        if (ibest >= 0) {
            int np = std::min(NDMAX - ibest, NN*NSS);
            if (np > 0) memcpy(cd, cb + ibest, sizeof(Cx) * np);
        } else {
            int off = -ibest;
            int np  = NN*NSS + 2*ibest;
            if (np > 0 && off < NN*NSS) {
                int n_copy = std::min(np, NP);
                memcpy(cd + off, cb, sizeof(Cx) * n_copy);
            }
        }

        // Bit metrics
        float bitmetrics[2*NN][3];
        bool badsync;
        getBitMetrics(cd, bitmetrics, badsync);
        if (badsync) continue;

        // Parabolic DT refinement
        float xibest = (float)ibest;
        if (ibest > 0 && ibest < NDMAX - 1) {
            float sm1, sp1;
            const Cx *ctwk = g.ctwk2[idfbest + 16];
            sync2d(cd2, ibest-1, ctwk, sm1);
            sync2d(cd2, ibest+1, ctwk, sp1);
            float den = sm1 - 2.f*smax + sp1;
            if (fabsf(den) > 1e-6f)
                xibest = (float)ibest + 0.5f * (sm1 - sp1) / den;
        }

        int nsq = checkSyncQual(bitmetrics);
        if (nsq < nsync_qual_min) continue;

        // Extract LLRs
        float llra[2*ND], llrb[2*ND], llrc[2*ND], llrd[2*ND], llre[2*ND];
        extractLLR(bitmetrics, llra, llrb, llrc, llrd, llre);

        // Adaptive pass count by sync quality
        int npassmax = 5;
        if (nsq > 28)         npassmax = 1;
        else if (nsq > 22)    npassmax = 2;

        int8_t message77[77];
        int nharderror = runLDPCPasses(llra, llrb, llrc, llrd, llre, npassmax, message77);
        if (nharderror < 0) continue;

        // Duplicate check vs this-call decodes
        bool idupe = false;
        for (int d = 0; d < local_ndecoded; d++) {
            if (memcmp(prev_bits[d], message77, 77) == 0) { idupe=true; break; }
        }
        // Duplicate check vs known bits from caller
        if (!idupe && known_bits) {
            for (int d = 0; d < std::min(nknown, MAXDEC); d++) {
                if (memcmp(known_bits + d*77, message77, 77) == 0) { idupe=true; break; }
            }
        }
        if (idupe) continue;

        memcpy(prev_bits[local_ndecoded], message77, 77);

        // SNR estimate: use getCandidates speak for accurate spectral SNR.
        // For nfqso_only, getCandidates was skipped pre-decode for speed;
        // run it now only on successful decodes (~40ms cost).
        if (nfqso_only == 1) {
            float snr_savg[NH1], snr_sbase[NH1];
            float snr_cand[MAXCAND][2];
            int snr_ncand = 0;
            getCandidates(dd, (float)nfa, (float)nfb, 0.60f, nfqso, MAXCAND,
                          snr_savg, snr_cand, snr_ncand, snr_sbase);
            // Find the speak value nearest our decoded frequency
            snr0 = 0.f;
            for (int i = 0; i < snr_ncand; i++) {
                if (fabsf(snr_cand[i][0] - f1) <= 20.f) {
                    snr0 = snr_cand[i][1] - 1.0f;
                    break;
                }
            }
        }
        float xsnr;
        if (snr0 > 0.f) {
            xsnr = 10.f * log10f(snr0) - 13.f;
        } else {
            xsnr = -21.f;
        }
        int nsnr = (int)roundf(std::max(-21.f, xsnr));

        // Store results
        if (snr_out)  snr_out[local_ndecoded]  = nsnr;
        if (dt_out)   dt_out[local_ndecoded]   = xibest / 1333.33f - 0.5f;
        if (freq_out) freq_out[local_ndecoded]  = f1;
        if (sync_out) sync_out[local_ndecoded]  = smax;
        if (msgbits_out) memcpy(msgbits_out + local_ndecoded*77, message77, 77);
        local_ndecoded++;
    }

    if (ndecoded) *ndecoded = local_ndecoded;
}

// ---------------------------------------------------------------------------
// syncScanFT2 — lightweight sync scanner (ports ft2_sync_scan.f90)
// ---------------------------------------------------------------------------
void syncScanFT2(const int16_t *iwave, int nfreqs, const float *freqs,
                 float *sync_out, float *freq_out,
                 int *ibest_out, int *idf_out) {
    std::call_once(g_init_once, initDSP);

    if (sync_out)  *sync_out  = -99.f;
    if (freq_out)  *freq_out  = 0.f;
    if (ibest_out) *ibest_out = -1;
    if (idf_out)   *idf_out   = 0;

    if (!g.initialized || nfreqs <= 0) return;

    // heap-allocated: NMAX=90000 floats = 360KB, too large for stack
    auto dd_buf = std::make_unique<float[]>(NMAX);
    float *dd = dd_buf.get();
    for (int i = 0; i < NMAX; i++) dd[i] = (float)iwave[i];

    float best_sync = -99.f;
    float best_freq = 0.f;
    int   best_ibest = -1, best_idf = 0;

    bool dobigfft = true;
    int ndmax_scan = NDMAX - NN*NSS + 320;

    for (int ifreq = 0; ifreq < nfreqs; ifreq++) {
        float f0 = freqs[ifreq];

        // heap-allocated: NP=10000 Cx = 80KB, too large for stack
        auto cd2_buf = std::make_unique<Cx[]>(NP);
        Cx *cd2 = cd2_buf.get();
        ft2Downsample(dd, dobigfft, f0, cd2);
        if (dobigfft) dobigfft = false;

        float sum2 = 0.f;
        for (int n = 0; n < NP; n++) sum2 += std::norm(cd2[n]);
        sum2 /= (float)NP;
        if (sum2 > 0.f) { float inv=1.f/sqrtf(sum2); for(int n=0;n<NP;n++) cd2[n]*=inv; }

        for (int idf = -12; idf <= 12; idf += 3) {
            const Cx *ctwk = g.ctwk2[idf + 16];
            for (int istart = -688; istart <= ndmax_scan; istart += 8) {
                float sync; sync2d(cd2, istart, ctwk, sync);
                if (sync > best_sync) {
                    best_sync  = sync;
                    best_freq  = f0;
                    best_ibest = istart;
                    best_idf   = idf;
                }
            }
        }
    }

    if (sync_out)  *sync_out  = best_sync;
    if (freq_out)  *freq_out  = best_freq;
    if (ibest_out) *ibest_out = best_ibest;
    if (idf_out)   *idf_out   = best_idf;
}

} // namespace FT2

#endif // JS8_ENABLE_FT2
