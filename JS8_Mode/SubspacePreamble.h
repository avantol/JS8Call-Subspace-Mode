#ifndef SUBSPACE_PREAMBLE_H
#define SUBSPACE_PREAMBLE_H

// Subspace ⚡ lightning-bolt full-frame beacon.
//
// Pivoted from preamble-in-pre-roll (builds 324-327) to full-frame.
// The bolt is now a STANDALONE transmission that occupies a complete
// FT2 cycle (~3.75 s, ~3 s of bolt audio + cycle alignment) and
// REPLACES the FT2 data waveform for that one TX. Audio stays within
// the FT2 frequency band — no out-of-band emissions — and is sized
// to be unmistakably visible on the waterfall (~3 s tall zigzag
// silhouette).
//
// Triggered manually for now (MVP). Future hookup points: pre-HAIL,
// CQ, ARQ session start.

#include <QString>
#include <QVector>

namespace SubspacePreamble {

// Modulator expects float samples at 48 kHz.
constexpr int kSampleRate = 48000;

// [BUILD 331-boltSVG] Bolt geometry — 40 time-slices × 16 frequency-rows.
// Bitmap is a direct rasterization of artwork/subspace_bolt.svg
// (Subspace ⚡ master) at 40x16, ray-cast from the SVG path polygon
// `M 13 2 L 3 14 L 12 14 L 11 22 L 21 10 L 12 10 Z`. Rows = simultaneous
// tones at different freqs within the FT2 band → appear horizontally on
// the waterfall. Cols are emitted sequentially → appear vertically.
constexpr int kBoltCols = 40;
constexpr int kBoltRows = 16;

// Bolt audio duration, in samples at kSampleRate.
// 24 cols × 125 ms/col = 3000 ms = 144000 samples at 48 kHz.
int durationSamples();

// Generate ~3 s of full-frame ⚡ bolt audio centered at `centerHz`.
// `centerHz` should be the MID of the FT2 4-tone block (not the base
// tone) so the bolt sits entirely within the FT2 emissions envelope:
//
//     centerHz = ft2BaseAudioHz + (3 * FT2_TONE_SPACING_HZ / 2)
//              ≈ ft2BaseAudioHz + 62.5
//
// Returns mono float samples in [-1.0, +1.0], ready to be cast/copied
// into the Modulator's m_ft2Wave float* slot (the existing FT2 TX
// path then plays them like any other waveform).
QVector<float> generateFullFrameBolt(double centerHz);

}  // namespace SubspacePreamble

#endif
