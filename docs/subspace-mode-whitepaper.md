# JS8Call Subspace Mode: What It Is, How It Works, and Why It Matters

**JS8Call Subspace Edition v2.6.0.46 — March 2026**
**WM8Q**

---

## The Short Version

Subspace Mode is a new JS8Call operating mode based on the FT2 protocol. It delivers **conversational-speed messaging at weak-signal performance levels that rival JS8 Normal mode** — but in a fraction of the time.

| Mode | Period | SNR Floor (measured) | Bandwidth | Modulation |
|------|--------|---------------------|-----------|------------|
| JS8 Slow | 30s | -28 dB | ~25 Hz | 8-FSK |
| JS8 Normal | 15s | -24 dB | ~50 Hz | 8-FSK |
| JS8 Fast | 10s | -22 dB | ~80 Hz | 8-FSK |
| JS8 Turbo | 6s | -20 dB | ~160 Hz | 8-FSK |
| **Subspace (FT2)** | **3.75s** | **-21 dB** | **~150 Hz** | **4-GFSK + LDPC** |

Read that again: Subspace is **faster than Turbo** (3.75s vs 6s) with **better weak-signal performance** (-21 dB vs -20 dB). It approaches JS8 Normal's sensitivity while running 4x faster.

This is not theoretical. These are measured results from on-air testing at 3 watts over 1+ mile, including decodes with the antenna disconnected and grounded — picking up signal through stray coupling alone.

---

## What Is FT2?

FT2 is a digital protocol designed by **Martino Merola (IU8LMC)** and implemented in his Decodium software. It uses 4-tone Gaussian Frequency-Shift Keying (4-GFSK) with Low-Density Parity-Check (LDPC) forward error correction — the same family of error-correcting codes used in 5G cellular, Wi-Fi 6, and deep-space communication.

### FT2 vs FT8: Different Protocols, Different Lineage

FT2 is **not** a variant of FT8. They share the LDPC(174,91) code and 77-bit message payload, but diverge in every other respect:

| | FT8 / JS8 | FT2 / Subspace |
|---|-----------|----------------|
| Modulation | 8-FSK (8 tones) | 4-GFSK (4 tones, Gaussian-shaped) |
| Symbols | 79 | 103 |
| Symbol rate | 6.25 baud (Normal) | 41.67 baud |
| Sync pattern | 3 Costas arrays (7 tones each) | 4 Costas arrays (8 tones each) |
| Waveform duration | 12.6s (Normal) | 2.52s |
| TX period | 15s (Normal) | 3.75s |

The Gaussian pulse shaping in 4-GFSK produces smoother frequency transitions between symbols, reducing spectral splatter. The 4 Costas sync arrays (vs 3 in FT8) provide more robust synchronization — the decoder can detect and lock onto the signal even in poor conditions.

### "Joe Taylor Said FT2 Was Unstable"

This requires context. K1JT evaluated an early FT2 implementation for possible inclusion in WSJT-X. That evaluation was of a specific codebase at a specific point in time. It says nothing about the protocol itself, and nothing about subsequent implementations.

Consider: if someone built a buggy FT8 decoder in 2017, would that mean FT8 is fundamentally broken? Of course not. The protocol is sound; what matters is the implementation.

Subspace Mode uses a **completely independent decoder architecture** with:

- A two-phase sync-triggered decoder (not the implementation K1JT reviewed)
- LDPC with CRC verification — every accepted decode is mathematically validated
- A continuous sync monitor that detects signals as they arrive
- Progressive LDPC that adapts decoding effort to signal quality

The LDPC CRC check is the final gatekeeper. If the error-correcting code cannot reconstruct a valid codeword, the decode is rejected. There is no "unstable" middle ground — a frame either passes CRC or it doesn't. You will never see a corrupted message on screen.

---

## How the Decoder Works

### The Problem with Timer-Based Decoding

Traditional JS8Call modes use a timer-based architecture: wait for a fixed period (6-30 seconds), then decode whatever arrived. This creates two inefficiencies:

1. **Idle time** — the decoder waits even when a complete frame has already arrived
2. **Blind searching** — the decoder doesn't know where the signal is in frequency or time, so it searches everything

### Subspace's Continuous Architecture

Subspace Mode replaces the timer with an event-driven architecture:

```
Audio arrives continuously
       |
       v
  Ring buffer (7.5 seconds of audio)
       |
       v
  Costas Sync Monitor (every decode cycle)
       |
       +---> Sync detected (sync > 3.0)?
       |         |
       |     YES: Decode at known frequency (58ms)
       |         |
       |     NO:  Full spectral scan (150-2000ms)
       |
       v
  LDPC decode with CRC verification
       |
       v
  Valid frame -> display
```

**The Sync Monitor** runs a lightweight Costas tone scan across a frequency grid before each decode attempt. The FT2 Costas pattern is a specific sequence of tones that acts like a fingerprint — easy to detect, hard to fake. When the monitor finds strong Costas correlation (sync quality > 3.0, where noise peaks at ~2.6), it tells the decoder exactly where the signal is, skipping the expensive spectral search entirely.

**Progressive LDPC** adapts the decoding effort based on signal quality:
- Strong signals (sync quality > 28/32): try 1 LDPC pass — succeeds nearly every time
- Medium signals (22-28/32): try 2 passes
- Weak signals (< 22/32): try all 5 metric combinations for maximum sensitivity

The result: strong signals decode in **58 milliseconds**. Weak signals take longer (up to 2 seconds) because they need more processing — but they still decode, down to -21 dB SNR.

---

## What -21 dB SNR Actually Means

Signal-to-noise ratio in dB is logarithmic. Every 3 dB represents a halving (or doubling) of power. At -21 dB, the signal power is **less than 1% of the noise power**. The signal is completely invisible on a waterfall display and inaudible to the human ear.

For perspective:

| SNR | Signal vs Noise | Audible? | Visible on waterfall? |
|-----|----------------|----------|----------------------|
| +10 dB | Signal is 10x noise | Yes, clearly | Bold trace |
| 0 dB | Equal | Barely | Faint trace |
| -6 dB | Signal is 25% of noise | No | Maybe, if you know where to look |
| -10 dB | Signal is 10% of noise | No | No |
| **-21 dB** | **Signal is 0.8% of noise** | **No** | **No** |
| -24 dB | Signal is 0.4% of noise | No | No |

The design target for Subspace Mode was -6 dB. We achieved -21 dB — a factor of **30x better** in power terms. This was measured on-air at 3 watts with the receive antenna **disconnected and grounded**, picking up signal solely through stray electromagnetic coupling through the radio chassis.

### How Is This Possible?

LDPC error correction. The transmitted frame contains 174 coded bits encoding 91 information bits — nearly 2x redundancy. The LDPC decoder can reconstruct the original message even when roughly half the received bits are corrupted by noise. Combined with soft-decision decoding (using probability estimates rather than hard 0/1 decisions), this allows reliable communication well below the noise floor.

This is not unique to FT2 — FT8 and all JS8 modes use the same LDPC(174,91) code. What FT2 brings is a faster symbol rate and more sync redundancy (4 Costas arrays vs 3), enabling the same error correction in a shorter transmission.

---

## Comparison with Existing JS8Call Modes

### Speed vs Sensitivity Tradeoff

In traditional JS8 modes, there is a direct tradeoff: faster modes sacrifice sensitivity. Slow mode (30s, -28 dB) is the most sensitive; Turbo (6s, -20 dB) is the fastest. Each step faster costs 2-4 dB of sensitivity.

Subspace breaks this tradeoff. At 3.75 seconds, it's the fastest mode available — yet it matches or exceeds Turbo's sensitivity. How?

1. **4-GFSK vs 8-FSK**: With 4 tones instead of 8, each tone is spaced further apart in frequency, making them easier to distinguish in noise. The Gaussian shaping concentrates energy, reducing inter-symbol interference.

2. **More sync symbols**: FT2's 4 Costas arrays (32 sync symbols across 103 total) vs FT8's 3 arrays (21 sync symbols across 79 total) means 31% of the frame is sync, vs 27%. More sync = better time/frequency lock = better decoding at low SNR.

3. **Continuous decoding**: JS8 modes wait for period boundaries. Subspace decodes continuously — if a frame spans two decode cycles, both cycles attempt it. This effective "overlap" provides implicit diversity.

### Throughput

At 3.75 seconds per frame carrying 72 bits of payload (after protocol overhead), Subspace delivers approximately 19 bits/second of user data. Compare:

| Mode | Period | Payload bits | Throughput |
|------|--------|-------------|------------|
| JS8 Slow | 30s | 72 bits | 2.4 bps |
| JS8 Normal | 15s | 72 bits | 4.8 bps |
| JS8 Fast | 10s | 72 bits | 7.2 bps |
| JS8 Turbo | 6s | 72 bits | 12 bps |
| **Subspace** | **3.75s** | **72 bits** | **19.2 bps** |

Subspace is 4x faster than Normal mode and 60% faster than Turbo — while maintaining comparable or better sensitivity.

### What You Give Up

Transparency. Subspace Mode uses a native 72-bit framing scheme rather than the standard pack77 message format used by FT8/JS8. This means:

- **Standard JS8Call stations cannot decode Subspace frames** (and vice versa)
- **Decodium FT2 stations using pack77 cannot decode native Subspace frames** (intentional — different framing layer)
- Both stations must be running the Subspace Edition

This is a deliberate design choice. The native framing preserves the full 72-bit JS8 payload without the constraints of pack77's structured message format, enabling arbitrary text, commands, and data transfer — exactly what JS8Call is designed for.

---

## Development History and Architecture

Subspace Mode was developed over builds 35-46, with each build addressing specific technical challenges:

**Builds 35-40**: Core FT2 encode/decode integration, ring buffer architecture, LDPC tuning. Established the Fortran/C++ bridge that connects the FT2 signal processing (Fortran, derived from Decodium's work) to the JS8Call application (C++/Qt).

**Builds 41-44**: Continuous L2 (Level 2) decoder — replaced the timer-based architecture with an event-driven decode chain. The decoder runs continuously on a background thread, processing the audio ring buffer as fast as it can. Each decode completion immediately triggers the next attempt, eliminating idle time.

**Build 45**: Costas sync monitor — a lightweight pre-scanner that detects FT2 sync tones before attempting a full decode. When sync is detected, the decoder skips the expensive spectral search (308 FFTs + baseline estimation) and decodes at the known frequency. Reduces decode latency from 750-2000ms to 58ms for strong signals.

**Build 46**: Progressive LDPC — adapts the number of LDPC decoding passes to signal quality. Strong signals need only 1 pass; weak signals get the full 5-pass treatment. Every decode is CRC-verified regardless of pass count.

### Key Technical Decisions

Several design decisions are worth noting for those who may build on this work:

- **SNR estimates are unreliable at low SNR**. The LDPC CRC is the only trustworthy validity check. Early builds filtered decodes below -10 dB SNR, inadvertently rejecting valid frames at -12 to -16 dB. The fix: trust the math. If LDPC+CRC says it's valid, it's valid.

- **Spectral averaging across decode cycles causes stale messages**. WSJT-X's FT2 decoder uses spectral averaging (ndepth bit 4) to improve sensitivity by combining energy across consecutive periods. In a continuous decoder, this causes previously-decoded messages to re-appear indefinitely. Disabled in Subspace.

- **The sync threshold must be empirically determined**. The Costas sync metric is a normalized correlation: noise produces values of 1.6-2.6, real signals produce 3.0-4.8. The threshold of 3.0 was determined through on-air testing across multiple sessions. It cleanly separates signal from noise with zero false triggers observed across hundreds of noise-only cycles.

- **getcandidates2 false positives are expensive**. The spectral candidate finder returns 13-21 candidates on pure noise, each requiring ~100ms of Phase 1 sync search. Raising the candidate threshold from 0.50 to 0.60 reduced false positives without affecting real signal detection (real signals produce sync values of 1.8-2.2, well above threshold).

---

## Measured Performance

All measurements from on-air testing, 40-meter and 20-meter bands, 3 watts, 1+ mile path.

### Decode Performance
- **SNR range**: -21 dB to +19 dB (40 dB dynamic range)
- **Design target**: -6 dB (exceeded by 15 dB)
- **Decode latency (strong signal)**: 58ms average (sync monitor + single LDPC pass)
- **Decode latency (weak signal)**: 150-2000ms (full spectral search + multiple LDPC passes)
- **False decode rate**: zero observed (LDPC CRC eliminates false positives)
- **Frame completion**: 10-frame Gettysburg Address decoded with no gaps

### Sync Monitor Accuracy
- **Signal sync range**: 3.0 - 4.8 (normalized Costas correlation)
- **Noise sync ceiling**: ~2.6
- **Threshold**: 3.0
- **False triggers on noise**: 0 out of 676 noise-only cycles
- **True detections**: 103 out of 103 signal-present cycles

---

## Acknowledgments

**Martino Merola (IU8LMC)** — creator of FT2 and Decodium, and now a contributor to JS8Call-Subspace. The FT2 protocol and its Fortran signal processing code are Martino's work. His meticulous documentation of the FT2 protocol, decoder architecture, and signal processing pipeline made this integration possible. The Subspace decoder builds directly on his Fortran FT2 implementation, adapted for continuous operation and native JS8 framing. Upon reviewing the Subspace results, Martino's assessment: *"Great engineering!"*

**Joe Taylor, K1JT** — creator of WSJT-X, FT8, and the LDPC(174,91) code that underpins both FT8 and FT2. The error-correction engine at the heart of every weak-signal digital mode on HF today exists because of his work.

---

## How Engineering Became Research

Subspace Mode is the third attempt. The first two failed — not because the protocol was wrong, not because the code was bad, but because small assumptions that seemed too obvious to question turned out to be wrong. Both times, the project was abandoned after weeks of work that produced a decoder that appeared to function but didn't perform.

The third attempt succeeded because we finally paid attention to the smallest facts.

### What the Textbook Says

FT2 uses LDPC(174,91) error correction — the same code as FT8. The theoretical performance is well-understood. The Fortran signal processing was proven in Decodium. The JS8Call application framework was mature. Wire them together, test, done.

### What Actually Happened — Three Times

**The first two attempts** at Subspace Mode both reached the point of "it compiles, it runs, it sometimes decodes." Both were abandoned. The decoder worked on strong signals but was unreliable on anything marginal. The assumption each time was that the problem was fundamental — maybe FT2 wasn't suited for continuous operation, maybe the protocol had inherent limitations, maybe K1JT was right that it was unstable.

**The third attempt** started the same way. And it hit the same walls. But this time, instead of concluding "it doesn't work," we asked "why doesn't it work right now, specifically?"

The answer, both times, was embarrassingly small.

**Problem one**: the decoder had a sensible-looking SNR filter — reject any decode below -10 dB, because at that SNR the signal is buried in noise and the decode is probably garbage. Reasonable, right? Except LDPC doesn't care what's "reasonable." The error-correcting code was successfully reconstructing valid frames at -12, -13, even -16 dB — and the SNR filter was silently throwing them away. The first frame of every transmission was consistently filtered because the SNR estimator produces unreliable values at ring buffer startup.

The fix was humbling: delete the filter. Trust the math. If LDPC produces a valid codeword with a correct CRC, it's valid — regardless of what the SNR estimate says. The SNR estimate is a *guess*; the CRC is a *proof*. This one change — removing a line of code — unlocked 15 dB of performance that had been there all along, across all three attempts.

**Problem two**: the continuous decoder needed to know where to look in the spectrum. The obvious approach: use the UI cursor frequency — the operator knows where the signal is, just use that. Simple, clean, efficient. Zero decodes. Across 1,137 decode cycles, not one frame decoded.

The UI cursor was at 998 Hz. The actual signal was at 1,390 Hz — 392 Hz away. The decoder was confidently, efficiently, rapidly decoding at exactly the wrong frequency. Every cycle completed in milliseconds. Every cycle found nothing.

The lesson: "we know the frequency" is not the same as "we know the frequency." The cursor shows where the operator *thinks* the signal is. The signal is where the signal is. The solution required building an independent frequency discovery system — the Costas sync monitor — that finds the signal by detecting its physical signature in the audio, regardless of what the UI says.

### The Pattern

Both problems shared a root cause: **a small assumption that seemed too obvious to question**. And both had almost certainly been present in the first two failed attempts — invisible, because the symptoms looked like fundamental limitations rather than implementation bugs.

- "Decodes below -10 dB are garbage" — seemed obvious, was wrong
- "The UI cursor frequency is the signal frequency" — seemed obvious, was wrong
- "Spectral averaging improves sensitivity" — seemed obvious, caused ghost messages in a continuous decoder
- "The Hamlib serial poll is instant" — seemed obvious, caused 1-second waterfall blackouts

Each of these looked like an engineering problem but turned out to be a research finding. The SNR discovery revealed that the LDPC code was 15 dB more capable than anyone expected. The frequency discovery led to the sync monitor architecture, which cut decode latency by 20x. The averaging discovery revealed a fundamental incompatibility between frame-based and continuous decoding that isn't documented anywhere.

None of this was planned. The design target for SNR was -6 dB. The measured result is -21 dB. That 15 dB gap is not clever engineering — it's what was left on the table by two previous attempts that gave up too soon. The math was always right. The implementation was getting in its own way, and the symptoms were subtle enough to look like fundamental limitations.

### What This Means for Amateur Radio

The LDPC(174,91) code that K1JT designed for FT8 is more powerful than most operators realize. Every FT8 and JS8 implementation includes a decoder that is theoretically capable of performance well beyond its published specifications. The limiting factor is not the error-correcting code — it's the assumptions in the surrounding implementation: SNR floors, candidate thresholds, timing architectures, and frequency search strategies.

This suggests that existing modes may have untapped headroom. Not because the code needs to be rewritten, but because the guardrails need to be re-examined. The math has been right all along. The question is whether the implementation is getting out of its way.

---

## For the Skeptics

If you've read this far and you're still skeptical — good. Skepticism is healthy in engineering. Here's what I'd suggest:

1. **Look at the code**. It's open source. The LDPC decoder either produces a valid codeword or it doesn't. There is no ambiguity, no "sometimes correct" — it's mathematically verified.

2. **Look at the test results**. Every claim in this document has a corresponding log file with timestamps, SNR measurements, and decoded frames. The -21 dB figure is not a simulation — it's a measured on-air decode with the receive antenna grounded.

3. **Try it**. The Subspace Edition runs alongside standard JS8Call. You lose nothing by testing it.

The question is not whether FT2 is a sound protocol — the math and the measurements settle that. The question is whether the implementation is reliable. Forty-six builds of iterative testing, tuning, and fixing have produced a decoder that handles everything from +19 dB armchair signals to -21 dB signals buried 125:1 below the noise. Every frame is CRC-verified. Zero false decodes observed.

The protocol works. The implementation works. The on-air results speak for themselves.

---

*JS8Call Subspace Edition is developed by WM8Q. FT2 protocol by IU8LMC. LDPC(174,91) by K1JT.*
*Source: https://github.com/avantol/JS8Call-Subspace-Mode*
