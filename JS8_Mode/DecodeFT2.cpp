/**
 * @file DecodeFT2.cpp
 * @brief FT2 decoder bridge: calls Fortran FT2 decoder via C interface.
 */

#ifdef JS8_ENABLE_FT2

#include "DecodeFT2.h"
#include "JS8_Include/commons.h"
#include "ft2_bridge.h"

#include "JS8_Main/Varicode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <string>

#include <QDebug>
#include <QString>

namespace JS8 {

void DecodeFT2::decodeCallback(float /*sync*/, int snr, float dt, float freq,
                               const char * /*msg*/, int /*msglen*/, int nap,
                               float qual, const std::int8_t *msgbits77,
                               void *ctx) {
    auto *cctx = static_cast<CallbackContext *>(ctx);

    // Extract JS8 72-bit frame from bits 0-71 of the 77-bit payload
    quint64 value = 0;
    for (int i = 0; i < 64; ++i)
        value |= (static_cast<quint64>(msgbits77[i] & 1) << (63 - i));
    quint8 rem = 0;
    for (int i = 0; i < 8; ++i)
        rem |= ((msgbits77[64 + i] & 1) << (7 - i));

    // Convert 72 bits to 12-char alphabet72 frame
    QString frame = Varicode::pack72bits(value, rem);

    // Extract metadata from bits 72-76
    int frameBits = 0;
    if (msgbits77[72] & 1) frameBits |= Varicode::JS8CallFirst;
    if (msgbits77[73] & 1) frameBits |= Varicode::JS8CallLast;
    if (msgbits77[74] & 1) frameBits |= Varicode::JS8CallData;

    // Filter garbage decodes: reserved bits 75-76 must be 0, SNR >= -10
    bool garbage = (msgbits77[75] & 1) || (msgbits77[76] & 1) || snr < -10;

    qWarning() << "[FT2-RX] DECODED: snr=" << snr << "dt=" << dt
               << "freq=" << freq << "nap=" << nap
               << "bits=" << frameBits
               << "frame=" << frame
               << "raw72-76=" << int(msgbits77[72]) << int(msgbits77[73])
               << int(msgbits77[74]) << int(msgbits77[75]) << int(msgbits77[76])
               << (garbage ? "FILTERED" : "");

    if (garbage)
        return;

    (*cctx->emitEvent)(Event::Decoded{
        .utc = cctx->utc,
        .snr = snr,
        .xdt = dt,
        .frequency = freq,
        .data = frame.toStdString(),
        .type = frameBits,
        .quality = qual,
        .mode = 16 // Varicode::JS8CallFT2
    });

    ++cctx->count;
}

std::size_t DecodeFT2::operator()(struct dec_data &data, int kposFT2,
                                  int kszFT2, Event::Emitter emitEvent) {
    if (kszFT2 <= 0)
        return 0;

    // The Fortran decoder always reads FT2_NMAX (45000) samples regardless
    // of what we pass. We must provide a full, clean buffer every time.
    // Skip decode if we don't have enough samples for the signal (~2.5s).
    int nsamples = std::min(kszFT2, FT2_NMAX);
    if (nsamples < 30000) {
        // Not enough samples — FT2 signal is ~30240 samples (2.52s)
        return 0;
    }

    // Zero-fill the entire buffer first, then copy available samples.
    // This prevents stale data from previous calls corrupting the decode.
    std::fill_n(data.ft2_d2, FT2_NMAX, int16_t{0});

    int srcStart = kposFT2;
    if (srcStart + nsamples <= JS8_RX_SAMPLE_SIZE) {
        std::copy_n(&data.d2[srcStart], nsamples, data.ft2_d2);
    } else {
        int firstPart = JS8_RX_SAMPLE_SIZE - srcStart;
        std::copy_n(&data.d2[srcStart], firstPart, data.ft2_d2);
        std::copy_n(&data.d2[0], nsamples - firstPart,
                    &data.ft2_d2[firstPart]);
    }

    // Compute signal statistics for diagnostics
    int16_t peakSample = 0;
    double rms = 0.0;
    for (int i = 0; i < nsamples; ++i) {
        int16_t s = data.ft2_d2[i];
        if (std::abs(s) > std::abs(peakSample)) peakSample = s;
        rms += static_cast<double>(s) * s;
    }
    rms = std::sqrt(rms / nsamples);

    qWarning() << "[FT2-RX] decode: kpos=" << kposFT2
               << "ksz=" << kszFT2 << "nsamples=" << nsamples
               << "nfqso=" << data.params.nfqso
               << "nfa=" << data.params.nfa << "nfb=" << data.params.nfb
               << "peak=" << peakSample << "rms=" << rms;

    CallbackContext ctx{.emitEvent = &emitEvent,
                        .utc = data.params.nutc,
                        .count = 0};

    emitEvent(Event::SyncStart{kposFT2, kszFT2});

    // Call the Fortran decoder through the bridge
    // ndepth=3 (deep decode, no averaging). Bit 4 (16) enables spectral
    // averaging across calls which causes stale messages to re-appear.
    ft2_decode_c(data.ft2_d2, nsamples, data.params.nfqso, data.params.nfa,
                 data.params.nfb, 3,
                 &DecodeFT2::decodeCallback, &ctx);

    qWarning() << "[FT2-RX] decode complete: decoded=" << ctx.count;

    return ctx.count;
}

void DecodeFT2::clearAveraging() { ft2_clravg_c(); }

static void selfTestCallback(float sync, int snr, float dt, float freq,
                              const char *msg, int msglen, int /*nap*/,
                              float /*qual*/, const std::int8_t * /*msgbits77*/,
                              void *ctx) {
    auto *count = static_cast<int *>(ctx);
    std::string decoded(msg, static_cast<std::size_t>(msglen));
    auto end = decoded.find_last_not_of(' ');
    if (end != std::string::npos) decoded.resize(end + 1);
    qWarning() << "[FT2-SELFTEST] DECODED: snr=" << snr << "dt=" << dt
               << "freq=" << freq << "sync=" << sync
               << "msg=" << QString::fromLatin1(decoded.c_str());
    ++(*count);
}

void DecodeFT2::selfTest() {
    qWarning() << "[FT2-SELFTEST] Starting encode→decode loopback test";

    // 1a. Quick encode test for CQ K9AVT DN61
    {
        char tmsg[37], tsent[37];
        int ttone[FT2_NUM_SYMBOLS];
        std::fill_n(tmsg, 37, ' ');
        const char *t = "CQ K9AVT DN61";
        std::copy_n(t, std::strlen(t), tmsg);
        ft2_encode_c(tmsg, ttone, tsent);
        qWarning() << "[FT2-SELFTEST] Encode 'CQ K9AVT DN61' → msgsent:"
                   << QString::fromLatin1(tsent, 37).trimmed();
    }

    // 1. Encode a test message
    char msg[37], msgsent[37];
    int i4tone[FT2_NUM_SYMBOLS];
    std::fill_n(msg, 37, ' ');
    const char *testmsg = "CQ K1ABC FN42";
    std::copy_n(testmsg, std::strlen(testmsg), msg);
    ft2_encode_c(msg, i4tone, msgsent);

    qWarning() << "[FT2-SELFTEST] Encoded:" << QString::fromLatin1(msgsent, 37).trimmed()
               << "itone[0..7]=" << i4tone[0] << i4tone[1] << i4tone[2] << i4tone[3]
               << i4tone[4] << i4tone[5] << i4tone[6] << i4tone[7];

    // 2. Generate GFSK waveform at 12kHz (NSPS=288) — what the decoder expects
    constexpr int NSPS_12K = 288;
    constexpr int NWAVE_12K = (FT2_NUM_SYMBOLS + 2) * NSPS_12K; // 30240
    auto wave12k = std::make_unique<float[]>(NWAVE_12K);
    ft2_gen_wave_c(i4tone, FT2_NUM_SYMBOLS, NSPS_12K, 12000.0f,
                   1500.0f, wave12k.get(), NWAVE_12K);

    // Check waveform
    float peak = 0;
    for (int i = 0; i < NWAVE_12K; ++i)
        peak = std::max(peak, std::abs(wave12k[i]));
    qWarning() << "[FT2-SELFTEST] 12kHz waveform: nwave=" << NWAVE_12K
               << "peak=" << peak << "sample[1000]=" << wave12k[1000];

    // 3. Convert to int16 buffer with noise.
    // The FT2 decoder uses spectral baseline estimation that needs a noise floor.
    // Use heap allocation to avoid stack overflow.
    std::mt19937 rng(42);

    // Signal scale matching ft2_gfsk_iwave: 32767/sqrt(2)
    constexpr float sigScale = 32767.0f / 1.4142f; // ~23170

    // Try multiple conditions
    struct TestCase {
        const char *name;
        int offset;
        float noiseAmp;  // 0 = noiseless
    };
    TestCase cases[] = {
        {"noiseless-at-0",   0,    0.0f},
    };

    auto iwave = std::make_unique<std::int16_t[]>(FT2_NMAX);

    for (auto &tc : cases) {
        std::fill_n(iwave.get(), FT2_NMAX, int16_t{0});

        // Add Gaussian noise to entire buffer
        if (tc.noiseAmp > 0) {
            std::normal_distribution<float> noise(0.0f, tc.noiseAmp);
            for (int i = 0; i < FT2_NMAX; ++i)
                iwave[i] = static_cast<int16_t>(
                    std::clamp(noise(rng), -32767.0f, 32767.0f));
        }

        // Add signal
        for (int i = 0; i < NWAVE_12K && (i + tc.offset) < FT2_NMAX; ++i) {
            float s = wave12k[i] * sigScale + iwave[i + tc.offset];
            iwave[i + tc.offset] = static_cast<int16_t>(
                std::clamp(s, -32767.0f, 32767.0f));
        }

        // Compute peak/rms
        int16_t pk = 0;
        double rmsVal = 0;
        for (int i = 0; i < FT2_NMAX; ++i) {
            if (std::abs(iwave[i]) > std::abs(pk)) pk = iwave[i];
            rmsVal += double(iwave[i]) * iwave[i];
        }
        rmsVal = std::sqrt(rmsVal / FT2_NMAX);

        int count = 0;
        qWarning() << "[FT2-SELFTEST]" << tc.name
                   << "offset=" << tc.offset << "noiseAmp=" << tc.noiseAmp
                   << "peak=" << pk << "rms=" << rmsVal;

        ft2_decode_c(iwave.get(), FT2_NMAX, 1500, 200, 4000, 3,
                     selfTestCallback, &count);

        qWarning() << "[FT2-SELFTEST]" << tc.name
                   << "decoded=" << count
                   << (count > 0 ? "SUCCESS" : "FAILED");

        if (count > 0) break;  // Stop on first success
    }
}

} // namespace JS8

#endif // JS8_ENABLE_FT2
