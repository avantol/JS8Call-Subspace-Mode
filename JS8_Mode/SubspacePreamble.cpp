#include "SubspacePreamble.h"

#include <QtMath>
#include <algorithm>

namespace SubspacePreamble {

// =====================================================================
// Vertical ⚡ bitmap — 24 cols (time slices) × 8 rows (freq tones).
// Cols 0-23 are emitted sequentially over ~3 s. On a typical
// waterfall scrolling new-at-top, col 0 ends up at the BOTTOM of the
// visible bolt area and col 23 at the TOP — so the bolt design
// places the bolt's "ground end" at col 0 and "sky end" at col 23.
//
// Classic two-segment zigzag, full vertical extent of the row range:
//
//        cols 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23
// row 0:       .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  #  #  #
// row 1:       .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  #  #  #  .  .
// row 2:       .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  #  #  .  .  .  .
// row 3:       .  .  .  .  .  .  .  .  .  .  .  .  #  #  #  #  #  #  #  .  .  .  .  .
// row 4:       .  .  .  .  #  #  #  #  #  #  #  #  #  .  .  .  .  .  .  .  .  .  .  .
// row 5:       .  .  .  #  #  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .
// row 6:       .  .  #  #  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .
// row 7:       #  #  #  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .
//
// [BUILD 331-diag1] Direct rasterization of ~/Downloads/diag1.png at
// native 40×16. Single thick diagonal stroke from bottom-left
// (rows 13-15 at col 0) to top-right (rows 0-3 at col 39). Each col
// has ~3-6 active rows; the stroke tapers slightly with col index.
static constexpr uint16_t kBoltBitmap[kBoltCols] = {
    0xE000, 0xF000, 0xF000, 0xF000, 0xF800, 0xF800, 0xF800, 0xFC00,
    0x7C00, 0x7E00, 0x3E00, 0x3E00, 0x3F00, 0x1F00, 0x1F00, 0x1F80,
    0x0F80, 0x0FC0, 0x07C0, 0x07C0, 0x07E0, 0x03E0, 0x03E0, 0x03F0,
    0x01F0, 0x01F8, 0x00F8, 0x00F8, 0x00FC, 0x007C, 0x007C, 0x007E,
    0x003E, 0x003F, 0x001F, 0x001F, 0x001F, 0x000F, 0x000F, 0x000F,
};

// ---------------------------------------------------------------------
// Audio generation parameters
// [BUILD 331-visHailEpi7] kPixelMs 85 → 75. Andy 2026-06-20: "end of
// second diag line is cut off early". 40 × 85 = 3400 ms left only
// 100 ms cycle tail; PTT-release delay + guiUpdate-poll lag often
// pushes the back-to-back diag-2 cycle start late, eating into the
// 100 ms tail and truncating bolt audio. Reduced to 40 × 75 = 3000
// ms → 500 ms tail margin, easily absorbs ~200-300 ms of timing slop.
constexpr int kPixelMs = 75;                    // 75 ms per column
constexpr int kPixelSamples =
    kPixelMs * kSampleRate / 1000;              // 3600 samples
// [BUILD 331-visHailEpi6] kEnvelopeSamples 480 → 960 (~20 ms) per
// Andy 2026-06-20 "smooth more". Even slower amplitude transitions
// → narrower spectral sidebands. At 85 ms/col with 20 ms ramp on
// each edge, the flat-amplitude middle is ~45 ms (53% of pixel).
constexpr int kEnvelopeSamples = 960;           // raised-cosine ramp (~20 ms)
// [BUILD 331-bolt1half] kRowSpacingHz 10 → 5 to halve total bolt width
// (Andy 2026-06-19 "cut the width in half"). 16 rows × 5 Hz = 80 Hz
// total. Caveat: at 5 Hz spacing, adjacent rows may blur together in
// the receiver waterfall's FFT (typical bin width ~4-6 Hz). If blur
// becomes a problem, bump back toward 6-7 Hz.
constexpr double kRowSpacingHz = 5.0;           // 5 Hz between row tones
constexpr double kBoltAmplitude = 0.95;         // ~95% of full scale
constexpr double kTwoPi = 2.0 * M_PI;
// [BUILD 329] Removed fixed kRowNormDivisor (was 2.0). Per-column
// normalization in generateFullFrameBolt() now divides amplitude by
// the count of ACTIVE rows in each column — every column ends up at
// the same total energy regardless of bolt-thickness, so diagonal
// (1-row) and chunk (multi-row) segments look equally bright on the
// waterfall.

int durationSamples() {
    return kBoltCols * kPixelSamples;
}

QVector<float> generateFullFrameBolt(double const centerHz) {
    QVector<float> out;
    out.resize(durationSamples());
    std::fill(out.begin(), out.end(), 0.0f);

    // Row 0 → highest freq. Row kBoltRows-1 → lowest freq.
    // Centered vertically around centerHz with kRowSpacingHz spacing.
    auto const rowToToneHz = [centerHz](int row) {
        double const offset =
            (static_cast<double>(kBoltRows - 1) * 0.5 -
             static_cast<double>(row)) *
            kRowSpacingHz;
        return centerHz + offset;
    };

    for (int col = 0; col < kBoltCols; ++col) {
        // [BUILD 331-boltSVG] stroke2 mechanism + thickening REMOVED.
        // The bitmap is now a direct rasterization of the SVG master,
        // so the rendered shape IS the bitmap — no synthetic offset
        // strokes, no row-thickening. Use the bitmap as-is.
        uint16_t const rowMask = kBoltBitmap[col];

        int const colStart = col * kPixelSamples;

        // [BUILD 329] Per-column normalization: divisor = active-row
        // count, so every column outputs the SAME total amplitude.
        // Without this, single-row columns are quieter than multi-row
        // columns (the diagonal stroke fades; only the chunks show).
        int activeRows = 0;
        for (int row = 0; row < kBoltRows; ++row) {
            if (rowMask & (1u << row)) ++activeRows;
        }
        if (activeRows == 0) continue;
        double const divisor = static_cast<double>(activeRows);

        for (int row = 0; row < kBoltRows; ++row) {
            if (!(rowMask & (1u << row))) continue;

            double const omega =
                kTwoPi * rowToToneHz(row) / kSampleRate;
            double phase = 0.0;

            for (int n = 0; n < kPixelSamples; ++n) {
                // Raised-cosine envelope on each pixel to kill clicks
                double env = 1.0;
                if (n < kEnvelopeSamples) {
                    double const x = static_cast<double>(n) /
                                     kEnvelopeSamples;
                    env = 0.5 * (1.0 - qCos(M_PI * x));
                } else if (n >= kPixelSamples - kEnvelopeSamples) {
                    double const x =
                        static_cast<double>(kPixelSamples - 1 - n) /
                        kEnvelopeSamples;
                    env = 0.5 * (1.0 - qCos(M_PI * x));
                }

                double const sample =
                    env * kBoltAmplitude * qSin(phase) / divisor;

                int const idx = colStart + n;
                double accumulated =
                    static_cast<double>(out[idx]) + sample;
                accumulated = std::clamp(accumulated, -1.0, 1.0);
                out[idx] = static_cast<float>(accumulated);

                phase += omega;
                if (phase > kTwoPi) phase -= kTwoPi;
            }
        }
    }

    return out;
}

}  // namespace SubspacePreamble
