#ifndef FT2_ONSET_DETECTOR_H
#define FT2_ONSET_DETECTOR_H

/**
 * @file FT2OnsetDetector.h
 * @brief Scans a ring buffer for FT2 signal onset to position the decode window.
 */

#ifdef JS8_ENABLE_FT2

#include <cstdint>

namespace JS8 {

struct FT2OnsetResult {
    int startOffset;    // sample offset in linearized buffer for 45000-sample extraction
    bool signalFound;   // true if onset detected
    float peakPowerDb;  // peak in-band power above noise floor (dB)
    int onsetBin;       // step index where onset was detected
};

/**
 * Scan linearized ring buffer for FT2 signal onset.
 * Uses short FFTs to find where in-band power (around nfqso) rises above noise.
 * Returns the optimal start offset for a 45000-sample extraction window.
 *
 * @param samples    Linearized ring buffer
 * @param nsamples   Number of valid samples (up to 90000)
 * @param nfqso      Expected signal frequency in Hz
 * @return           Onset result with optimal window start offset
 */
FT2OnsetResult ft2FindOnset(const std::int16_t *samples, int nsamples, int nfqso);

} // namespace JS8

#endif // JS8_ENABLE_FT2
#endif // FT2_ONSET_DETECTOR_H
