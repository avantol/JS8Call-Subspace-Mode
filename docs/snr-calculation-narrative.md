# How SNR is Calculated for Subspace (FT2 L2) Decodes

## The Journey of Audio Data

### Stage 1: Audio Capture → Ring Buffer

The sound card delivers 12,000 audio samples per second (12 kHz sample rate) as
16-bit signed integers (-32768 to +32767). These flow continuously into a circular
ring buffer of 90,000 samples — exactly 7.5 seconds of audio.

The ring buffer is written by the audio input thread and read by the decoder thread.
There is no synchronization to any clock or period boundary — the buffer simply
accumulates whatever the microphone delivers, wrapping around when it reaches the
end.

### Stage 2: Ring Buffer → Linear Array

Every ~600 milliseconds, the L2 decoder thread wakes up and copies the ring buffer
into a fresh, contiguous array of 90,000 samples. This copy is a snapshot — the
ring buffer continues to advance while the decoder works on the snapshot.

The copy starts from the oldest sample in the buffer and proceeds forward, so the
linear array contains the most recent 7.5 seconds of audio in chronological order.
The 16-bit integer samples are passed directly to the Fortran routines.

### Stage 3: Integer → Float Conversion

Inside the Fortran decoder, the first thing that happens is the integer samples
are assigned to a floating-point array: `dd = iwave`. This is a simple type
conversion — no scaling, no normalization. A sample that was -16384 as an integer
becomes -16384.0 as a float. The absolute magnitudes are preserved.

This matters: the SNR calculation works with these raw magnitudes. There is no
"0 dB reference" or calibrated power level. Everything is relative.

## The SNR Calculation: Two Paths

At this point, the decoder decides how to find candidate signals. There are two
paths, and they produce SNR estimates very differently.

### Path A: Full Spectral Scan (FULL-SCAN)

This path runs when the Costas sync monitor hasn't found a strong signal
(sync score < 3.0). It's the expensive but accurate path.

#### Step A1: Build the Spectrogram

The routine `getcandidates2` takes the 90,000-sample float array and computes
a spectrogram — a two-dimensional picture of power vs. frequency vs. time.

It does this by sliding a 1,152-sample window across the data, advancing 288
samples (one symbol period) at a time. At each position, it computes an FFT
(Fast Fourier Transform) that converts the time-domain samples into a frequency
spectrum. Each FFT produces 576 frequency bins, spaced 10.42 Hz apart
(12000 / 1152).

There are 308 window positions across the 90,000 samples. Each produces one
frequency spectrum. The result is a 576 × 308 grid of power values — power at
each frequency for each time slice.

#### Step A2: Average the Spectrogram

All 308 spectra are summed and divided by 308 to produce a single **average
spectrum**: one power value per frequency bin, representing the average power at
that frequency across the entire 7.5-second buffer.

This is a critical step. The averaging treats every time slice equally. A signal
that exists in 100 of the 308 time slices contributes only 100/308 = 32% of its
full power to the average. A signal present in 200 time slices contributes 65%.

**This is the source of the first-frame SNR inaccuracy.** One FT2 frame occupies
about 30,000 samples ≈ 104 time slices out of 308. Two consecutive frames occupy
about 208 time slices. The signal's contribution to the average doubles when two
frames are present, producing a ~3 dB higher apparent power.

#### Step A3: Smooth the Average

The average spectrum is smoothed by a 15-bin running average (±7 bins around each
point). This creates a gently varying version of the spectrum that follows broad
trends but blurs out narrow peaks.

#### Step A4: Fit the Noise Baseline

The smoothed average spectrum is passed to a baseline fitting routine. This routine
needs to determine the noise floor — the background power level without any signals.

It works as follows:

1. Convert the spectrum to decibel (dB) scale: power → 10 × log10(power)

2. Divide the frequency range into 10 equal segments.

3. In each segment, find the lowest 10% of values. These represent the "quiet"
   parts of the spectrum — frequencies with no signal, only noise.

4. Collect all these low-percentile points across all segments. These are the
   noise floor samples.

5. Fit a 5th-order polynomial to these noise floor samples. The polynomial
   captures the gradual shape of the noise floor across frequency (which may
   slope due to receiver characteristics, filtering, or atmospheric noise).

6. Convert the polynomial back from dB to linear power. This is the noise
   baseline — an estimate of what the spectrum would look like with no signals
   present.

The quality of this baseline depends on having enough noise-only frequency bins.
If the band is very crowded with signals, the "lowest 10%" may still include
some signal energy, and the baseline will be too high — underestimating SNR.

#### Step A5: Normalize and Find Peaks

The smoothed average spectrum is divided by the noise baseline, bin by bin. The
result is a normalized spectrum where noise is approximately 1.0 and signals
appear as peaks above 1.0.

The routine scans this normalized spectrum for peaks — local maxima that exceed
a threshold (0.60 by default, lowered to 0.50 for deep searches). Each peak is
a candidate signal.

For each candidate, the peak value is recorded as `speak`. This is the signal's
power ratio relative to the noise baseline. A `speak` of 3.0 means the signal
is 3× above the noise floor in the averaged spectrum.

#### Step A6: Priority Ordering

Candidates near the expected frequency (if one is known from prior decodes) are
placed first in the list. Others follow. This prioritizes re-decoding known
signals over discovering new ones.

### Path B: Sync-Score Proxy (SYNC-HIT)

This path runs when the Costas sync monitor found a strong signal (sync score
≥ 3.0). It skips the entire spectral analysis to save ~100ms of computation.

The Costas sync score is used directly as the SNR proxy. The sync score is a
measure of how well the received signal's Costas tone pattern matches the
expected pattern — a signal-to-noise metric, but measured in the time/correlation
domain rather than the frequency/spectral domain.

The candidate's "SNR" is set to the sync score value. No spectrogram, no baseline
fitting, no peak detection.

## From Candidate SNR to Reported SNR

Regardless of which path produced the candidate, the candidate carries an SNR
estimate (`speak` from Path A, or `sync_score` from Path B). This value undergoes
several transformations before being reported.

### Step 1: Offset by 1.0

The raw value has 1.0 subtracted: `snr0 = speak - 1.0`. This converts from
"ratio above baseline" to "excess above baseline." A signal exactly at the noise
floor has `speak = 1.0`, giving `snr0 = 0.0`.

### Step 2: Convert to Decibels

If `snr0 > 0`: `xsnr = 10 × log10(snr0) - 13.0`

The -13.0 is a calibration constant. It accounts for the difference between the
spectral peak ratio and the actual signal-to-noise ratio as conventionally
measured. This constant was determined empirically by the WSJT-X team to align
the reported SNR with standard measurement conventions (noise in a 2500 Hz
bandwidth).

If `snr0 ≤ 0`: the signal is at or below the noise floor, and the formula would
produce negative infinity or a complex number. The SNR is clamped to -21 dB, which
is the conventional floor for WSJT-X family decoders.

### Step 3: SYNC-HIT Calibration (our addition)

For the SYNC-HIT path only, a linear calibration corrects the sync-score-based
estimate to approximate the spectral estimate:

`xsnr = 0.6818 × xsnr + 12.227`

This was derived from field measurements and clamped to [-16, +24] dB.

### Step 4: Integer Rounding and Floor

The floating-point dB value is rounded to the nearest integer. A final floor of
-21 dB (FULL-SCAN) or -16 dB (SYNC-HIT) is applied. The result is an integer
SNR in dB, stored in the output array.

## Timing and Limitations

### The L2 Decode Cycle

Every ~600ms:
1. Copy ring buffer to linear array (~0 ms)
2. Costas sync scan on frequency grid (~50-90 ms)
3. If sync ≥ 3.0: SYNC-HIT path (skip spectral analysis)
   If sync < 3.0: FULL-SCAN path (run spectral analysis)
4. Phase 1: For each candidate, downsample and coarse sync search (~200 ms)
5. Phase 2: For each hit, fine sync + LDPC decode (~300-500 ms)
6. Report results

Total cycle: ~500-1500 ms depending on number of candidates.

### Limitations

1. **Buffer averaging dilutes single-frame SNR.** The spectrogram averages across
   the entire 7.5-second buffer. A single 3.75-second frame contributes to only
   ~33% of the averaging windows. The remaining 67% contain noise (or other
   signals), diluting the signal peak. This produces an SNR estimate ~5-6 dB lower
   than the true per-frame SNR for isolated frames.

2. **No per-frame spectral measurement.** The spectral analysis is always performed
   on the full buffer, not on individual frames. There is no mechanism to measure
   the SNR of a specific frame in isolation.

3. **Baseline fitting assumes sparse signals.** The noise floor estimate uses the
   lowest 10% of spectral values. On a busy band with many signals, the baseline
   may be biased upward, underestimating all SNR values.

4. **SYNC-HIT path trades accuracy for speed.** The sync score proxy is faster
   but less accurate than spectral analysis. The calibration constants are
   empirical approximations from limited data.

5. **The -13 dB calibration constant is inherited from WSJT-X.** It was tuned for
   FT8/FT4 modulation characteristics. FT2's 4-GFSK at 288 samples/symbol may
   have slightly different spectral characteristics, but the constant has not been
   re-tuned for FT2.

6. **Integer quantization.** The final SNR is rounded to the nearest integer dB.
   Fine differences (< 0.5 dB) are lost.
