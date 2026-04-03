# SNR First-Frame Inaccuracy Analysis

## Problem

The first decoded frame at a new frequency (or after a pause > 7.5s) reports
SNR approximately 5-6 dB lower than subsequent frames of the same signal.
This affects SNR replies, band activity display, and user perception of
signal quality.

## Observed Data

From diagnostic log (Build 68, snr-diag):
```
Frame 1: snr= -12, sync= 2.53  (FULL-SCAN)
Frame 2: snr= -6,  sync= 4.11  (FULL-SCAN)
Frame 3: snr= -6,  sync= 3.99  (FULL-SCAN)
Frame 4: snr= -6,  sync= 4.10  (FULL-SCAN)
...all subsequent: snr= -6 to -7 (stable)
```

Step function: -12 → -6/-7 on second frame, then flat. Not a ramp.

## Root Cause

`getcandidates2` in `lib/ft2/getcandidates2.f90` computes the **average power
spectrum** across the entire 90000-sample buffer (308 FFT windows of 1152 samples,
stepping by 288 samples):

```fortran
do j=1,NHSYM
   savg=savg + s(1:NH1,j)    ! sum all 308 windows
enddo
savg=savg/NHSYM               ! average
```

The SNR estimate is the peak of `savg` divided by the noise baseline.

### Buffer occupancy:
- One FT2 frame = ~30240 samples = 33% of 90000-sample buffer
- **First frame**: buffer contains 1 frame (~102 signal windows out of 308)
- **Second frame+**: buffer contains 2 frames (~204 signal windows out of 308)

The signal peak in the averaged spectrum is proportional to the number of
signal-containing windows. Two frames → 2× the signal energy in `savg` →
~3 dB higher SNR estimate.

### Theoretical vs observed:
- Theoretical: 10*log10(2) = **3.0 dB** difference
- Observed: **5-6 dB** difference
- Extra 2-3 dB likely from baseline fitting sensitivity (less signal
  energy → noisier baseline fit → additional underestimate)

### Why it's a step function (not a ramp):
- Frame 1: 1 frame in buffer → low SNR
- Frame 2: 2 frames in buffer → higher SNR
- Frame 3+: oldest frame wraps out, new one arrives → still 2 frames → same SNR
- Steady state: always ~2 frames in the 7.5-second buffer

## Key Discoveries

1. **Not a SYNC-HIT vs FULL-SCAN issue** — the log shows ALL decodes at this
   signal level (-6/-7 dB) use FULL-SCAN. The sync scan scores (2.0-2.6) stay
   below the 3.0 threshold. The SYNC-HIT calibration code we added doesn't fire.

2. **Not a Fortran stale variable** — `getcandidates2` has no `save` variables
   affecting noise estimation. The baseline is recomputed fresh every call.

3. **Not a ring buffer positioning issue** — the L2 decoder position relative to
   the frame is random. Both first and subsequent decodes have random alignment.

4. **The frame decodes perfectly** — LDPC+CRC pass, all 174 bits correct. The
   signal isn't partial or corrupted. Only the SNR *estimate* is affected.

5. **Known-frame subtraction is irrelevant** — `known_bits` only prevents
   re-reporting the same decoded message. It doesn't modify the audio or the
   spectral analysis. The sync scan and `getcandidates2` see the same signal
   regardless of `nknown`.

6. **The condition is "gap > 7.5 seconds"** — not just "first frame ever." Any
   pause between frames longer than the buffer cycle (7.5s) causes the same
   effect. Mid-message pauses, inter-message gaps, station going idle then
   returning — all trigger the inaccurate first-frame SNR.

## Current State (Build 68)

Two calibration mechanisms implemented but not fully validated:

### 1. SYNC-HIT path (nfqso_only=1):
- Sync score passed from C++ as `sync_score` parameter
- Used as `candidate(2,1) = max(sync_score, 0.0)` instead of hardcoded 0.0
- Linear calibration applied: `xsnr = 0.6818 * xsnr + 12.227`
- Clamped to [-16, +24]
- **Only fires for strong signals** where sync ≥ 3.0

### 2. FULL-SCAN path (nfqso_only=0):
- Uses `getcandidates2` spectral SNR — no calibration applied
- This is where the first-frame inaccuracy occurs
- **No fix applied yet**

## Approaches Considered

### A. Post-hoc correction (+5 dB for first frame)
- Simple but arbitrary — one data point, may vary with signal strength
- Theoretical basis is +3 dB, observed is +5-6 dB

### B. Smaller spectral window
- Use frame-sized window for `getcandidates2`
- **Problem**: SNR needs both signal AND noise. A frame-only window has no
  noise reference.

### C. Post-decode SNR from frame region
- Use `ibest` (sync position) to extract frame region after decode
- Run spectral analysis on frame + surrounding noise
- **Problem**: `ibest` isn't available when `getcandidates2` runs (it's Phase 1,
  `ibest` comes from Phase 2 sync scan)

### D. Display placeholder "---" for first-frame SNR
- Show "---" in band activity and callsign table when SNR is known inaccurate
- Store raw value internally for protocol replies
- Replace with real SNR when second frame is decoded
- **Least disruptive** — no calculation changes, no Fortran modifications

### E. Detect condition and apply correction
- Check `m_bandActivity[offset].last().utcTimestamp` — if gap > 7.5s, it's
  a first-frame condition
- Apply +5 dB correction (or use theoretical +3 dB)
- Catches both literal first frames and pauses

## Detection Condition

"First frame" = no prior decode at this frequency within 7.5 seconds (one buffer cycle).

Best check: in `processDecodeEvent`, compare current timestamp against
`m_bandActivity[offset].last().utcTimestamp`. If gap > 7.5 seconds (or no
prior entry), flag as first-frame condition.

This catches:
- First frame from a new station
- First frame after a long pause
- First frame after your own TX (buffer overwritten)
- Does NOT false-trigger on consecutive frames from same station

## Impact

- **SNR replies** (`SNR?` command): first-frame reply may be 5-6 dB low
- **Band activity display**: first entry shows lower SNR than reality
- **Callsign table**: SNR column may show inaccurate initial value
- **User perception**: "signal is weaker than I thought" on first contact
- **No decode quality impact**: SNR is metadata only, not used in LDPC

## Files Involved

| File | Role |
|------|------|
| `lib/ft2/getcandidates2.f90` | Spectral SNR estimation (root cause) |
| `lib/ft2/ft2_triggered_decode.f90` | SNR conversion and floor clamp |
| `JS8_Mode/DecodeFT2.cpp` | C++ decode wrapper, passes sync_score |
| `JS8_UI/mainwindow.cpp` | L2 decode loop, known-frame tracking |
| `JS8_Mainwindow/processDecodeEvent.cpp` | Decode event processing, band activity |
