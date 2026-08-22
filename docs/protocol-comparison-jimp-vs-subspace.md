# Protocol comparison: J-IMP JS8-60 (Ultra) vs Subspace FT2

Source verification of the wire-level protocols used by J-IMP's fast mode
(Ultra, also labelled "JS8 60" in their UI) and Subspace Edition's FT2
mode. Captured here so the technical separation is on record — useful
when correspondence with upstream maintainers raises claims about
"interference" between the two populations.

## J-IMP Ultra (JS8-60) protocol

Confirmed from upstream source at `/home/john/shared/JS8Call-improved-master`:

- **Modulation: 8-FSK** (frequency-shift keying with 8 tones — *not* 8-PSK;
  PSK is phase-shift, FSK is frequency-shift, which is what audio HF
  digital modes use).
- **Sync: 3 Costas arrays × 7 tones each** (`NS = 21` sync symbols
  arranged as 3 length-7 sequences). Ultra uses `Costas::Type::MODIFIED`:

  ```cpp
  // J-IMP: JS8_Mode/JS8.h:24-31
  std::array{std::array{0, 6, 2, 3, 5, 4, 1},
             std::array{1, 5, 0, 2, 3, 6, 4},
             std::array{2, 5, 0, 6, 4, 1, 3}}
  ```

- **All 5 J-IMP modes** (Normal, Fast, Turbo, Slow, Ultra) share the same
  8-FSK + length-7 Costas structure. Normal uses `Costas::Type::ORIGINAL`;
  the other four (incl. Ultra) use `MODIFIED`.

References:
- `JS8_Mode/JS8.cpp:187` — `constexpr int NS = 21; // Sync symbols (3 @ Costas 7x7)`
- `JS8_Mode/JS8.h:15-37` — `namespace Costas` with `enum class Type { ORIGINAL, MODIFIED };` and the array literals.
- `JS8_Include/commons.h:45` — `#define JS8I_SYMBOL_SAMPLES 384` (Ultra/JS8-60).

## Subspace FT2 protocol

Confirmed from `/home/john/js8call-improved`:

- **Modulation: 4-GFSK** (Gaussian-filtered frequency-shift keying with
  4 tones — same FSK family as 8-FSK but with Gaussian pulse shaping for
  cleaner spectrum).
- **Sync: 4 Costas arrays × 4 tones each** (`NS = 16` sync symbols
  arranged as 4 length-4 sequences):

  ```cpp
  // Subspace: JS8_Mode/ft2_modem.h:25-29
  static constexpr int ICOS4A[4] = {0, 1, 3, 2};
  static constexpr int ICOS4B[4] = {1, 0, 2, 3};
  static constexpr int ICOS4C[4] = {2, 3, 1, 0};
  static constexpr int ICOS4D[4] = {3, 2, 0, 1};
  ```

References:
- `JS8_Mode/ft2_modem.h:11-29` — frame-constant block including `NS`,
  `NSPS`, and the four Costas sequences.
- `JS8_Include/commons.h:29` — `// FT2 mode constants (different protocol from JS8: 4-GFSK, LDPC(174,91))`.

## Side-by-side

| Property              | J-IMP JS8-60 (Ultra)        | Subspace FT2          |
|-----------------------|-----------------------------|-----------------------|
| Modulation            | 8-FSK                       | 4-GFSK                |
| Tones                 | 8                           | 4                     |
| Sync structure        | 3 Costas arrays             | 4 Costas arrays       |
| Costas length         | 7 each                      | 4 each                |
| Sync symbols total    | 21                          | 16                    |
| Symbol period (NSPS)  | 384 samples (Ultra)         | 288 samples           |
| Forward error correct | LDPC(174,87) + CRC          | LDPC(174,91) + CRC    |

## Bottom line on "interference" claims

Two completely different waveforms. A J-IMP receiver matched-filtering
for 8-FSK length-7 Costas patterns will produce zero-correlation noise
on a Subspace FT2 signal — not even close to threshold. There is no
protocol-level interference. Subspace's matched filter looking for
length-4 Costas sees J-IMP's 8-FSK transmissions the same way: noise.

The only possible "interference" between the two populations is RF
coexistence in the same audio passband, which is just normal HF
spectrum sharing and applies equally to any digital mode using
~50–500 Hz of bandwidth at the same offset. It is not a property of
either protocol; it is a property of using radio.

## Cost of changing Costas patterns

If a future Subspace release wanted to formally separate its sync layer
from any other length-4 Costas FT2 implementation (e.g. KN4CRD's
original Decodium FT2 reference, which Subspace's current Costas
patterns match), the cost would be ~zero on either side: the matched
filter rejects a non-matching Costas at the cheapest possible stage
(candidate scoring), well before any LDPC decode work is attempted.
That stage runs against all audio every period regardless of whether
signals are present, so a "bogus" pattern is identical-cost to scanning
empty band. No CPU penalty.

## File metadata

- **Generated**: 2026-05-06 against Subspace commit `b22fcdd2` (Build
  152, v4.0.0.152) and the J-IMP master branch as cloned to
  `/home/john/shared/JS8Call-improved-master`.
- **Verification commands**: `grep` across `JS8_Mode/`, `JS8_Include/`,
  and the upstream tree as recorded above.
