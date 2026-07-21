#ifndef NATIVEBINARY_H
#define NATIVEBINARY_H

/**
 * @file NativeBinary.h
 * @brief [TODO #107, Build 343+] Native-layer (no-varicode) ARQ payload
 *        codec — pure logic for the "F/V3 NATIVE/GZIP" wire format.
 *
 * A Subspace frame carries 72 payload bits (9 bytes). V3 file transfer
 * puts RAW BYTES in those bits — no base32, no varicode — with a 1-byte
 * in-frame header that guarantees per-frame wire uniqueness (both RX
 * dedup layers key on the 77 bits with ~7.5–8 s windows; identical
 * frames would be silently dropped) and provides intra-chunk sequencing:
 *
 *   byte 0: [7..4]=SEQ (frame index in chunk, 0..15)
 *           [3..0]=CHK4 (chunkId & 0xF, binding sanity tag)
 *   bytes 1..8: payload (final frame zero-padded)
 *
 * On the wire the frame sets Data(bit74) + the NEW bit 75 discriminator
 * (Varicode::JS8CallNativeBinary): fielded receivers drop bit-75 frames
 * silently via their reserved-bits garbage filter — no aging clock
 * needed; only negotiated (ARQ level >= 3) peers ever receive V3.
 *
 * Chunk structure (SPARSE MARKERS, sender policy MARKER_INTERVAL=4):
 *   chunk 1:  text marker "F/V3 NATIVE/GZIP <TOTAL> <PCRC>" + K frames
 *   others:   K frames only; every 4th chunk gets a "V3 <PCRC>" marker
 *             (callsign ID cadence + integrity refresh — ADVISORY: the
 *             receiver window auto-advances on each ACK under
 *             stop-and-wait, so a lost periodic marker costs nothing)
 * TOTAL (chunk-1 marker) is authoritative for every chunk's byte count;
 * the envelope's internal u32be size remains the file-level check.
 *
 * Everything here is PURE — no timers, no radio, no UI — and covered by
 * the offline harness in tests_nativearq/ (extend it FIRST when
 * changing anything in this file; project process rule).
 */

#include <QByteArray>
#include <QList>
#include <QString>

namespace NativeBinary {

constexpr int FRAME_PAYLOAD_BYTES  = 8;    // per frame, after the header byte
// [BUILD 344 binMarker] SEQ 15 is the MARKER FRAME discriminator, so
// data frames use SEQ 0..14 → 15 frames → 120-byte chunk ceiling
// (was 16/128; shipped K=8 unaffected).
constexpr int MARKER_FRAME_SEQ     = 0xF;
constexpr int MAX_FRAMES_PER_CHUNK = 15;
constexpr int DEFAULT_CHUNK_BYTES  = 64;   // K=8 (operator decision 2026-07-18)
// [K-FALLBACK 2026-07-21] The single operator-in-loop retry step:
// K=8 → K=4 on a timeout_exhausted failure. No further step.
constexpr int FALLBACK_CHUNK_BYTES = 32;
constexpr int MAX_CHUNK_BYTES      = 120;  // K=15; receivers accept from day one

/// Chunks a given envelope needs at a given chunk size — the ONE
/// stage-2 qualification formula (fit iff result ≤ the peer's chunk
/// ceiling). Parameterized so the K=4 retry offer recomputes fit
/// against the same rule the K=8 send used.
constexpr int chunksNeeded(int envelopeBytes, int chunkBytes) {
    return (envelopeBytes + chunkBytes - 1) / chunkBytes;
}
constexpr int MARKER_INTERVAL      = 4;    // BINARY marker cadence (sender policy)
// Text-form marker cadence: station ID + operator-visible line. One
// text marker per TEXT_ID_INTERVAL chunks ≈ 8.8 min at bench pacing —
// inside the 10-minute ID rule for max-length transfers (chunk 1's
// text marker covers everything shorter).
constexpr int TEXT_ID_INTERVAL     = 16;

/// The exact Varicode::pack72bits domain: `value` = wire bits 71..8
/// (bytes 0..7 big-endian), `rem` = wire bits 7..0 (byte 8). Mirrors
/// the msgbits77 packing loop in mainwindow.cpp's FT2 TX branch.
struct Frame72 {
    quint64 value{0};
    quint8  rem{0};
};

/// Compose one binary frame. payload8 may be shorter than 8 on the
/// final frame of a chunk (zero-padded on the wire; the receiver trims
/// via the chunk byte count).
Frame72 encodeFrame(int seq, int chunkId, QByteArray const &payload8);

/// Decompose a received frame. Returns false on structural violation
/// (never expected — all 72-bit patterns parse; kept for symmetry and
/// future range rules).
bool decodeFrame(quint64 value, quint8 rem,
                 int *seq, int *chk4, QByteArray *payload8);

/**
 * [BUILD 344 binMarker] The BINARY marker frame — one native frame
 * (SEQ=15) carrying everything the text marker carries except
 * callsigns, single-frame robust like the payload (2026-07-20 on-air
 * msg-33: two 100%-decoded payload bursts orphaned because the
 * 5-frame TEXT marker lost one varicode frame each time). Rides
 * chunk-1 bursts (redundant with the text marker), every
 * MARKER_INTERVALth chunk, and every retransmit. Peer binding is a
 * 16-bit callsign hash validated against the receiver's known-peer
 * candidates — no match, silent drop (the text marker remains the
 * only fresh-contact open).
 *
 * Wire layout of the 72 bits (MSB-first after the header byte):
 *   byte 0:    [7..4]=SEQ=0xF  [3..0]=chunkId & 0xF
 *   bits 63..: msgId(7) CC(7) TOTAL(14) KB/8-1(4) PCRC(16) HASH(16)
 */
struct MarkerFrame {
    int     msgId{0};        // 1..99
    int     chunkId{0};      // 1..99 (full value; header chk4 = &0xF)
    int     totalBytes{0};   // envelope bytes, authoritative (<= 14 bits)
    int     chunkBytes{0};   // KB, 8..120 in steps of 8
    quint16 pcrc{0};         // payloadCrc16 of THIS chunk
    quint16 peerHash{0};     // peerHash16(sender callsign)
};
/// 9-wire-byte carrier form (signal-friendly): bytes 0..7 = value
/// big-endian, byte 8 = rem. Empty QByteArray = "no marker frame".
inline QByteArray frameToBytes(Frame72 const &f) {
    QByteArray b(9, '\0');
    for (int i = 0; i < 8; ++i)
        b[i] = static_cast<char>((f.value >> (8 * (7 - i))) & 0xFF);
    b[8] = static_cast<char>(f.rem);
    return b;
}
inline Frame72 frameFromBytes(QByteArray const &b) {
    Frame72 f;
    if (b.size() != 9) return f;
    for (int i = 0; i < 8; ++i)
        f.value = (f.value << 8) | static_cast<quint8>(b.at(i));
    f.rem = static_cast<quint8>(b.at(8));
    return f;
}

Frame72 encodeMarkerFrame(MarkerFrame const &m);
/// Strict parse: false unless SEQ==MARKER_FRAME_SEQ, header chk4
/// matches CC, and every field is in range.
bool decodeMarkerFrame(quint64 value, quint8 rem, MarkerFrame *out);
/// FNV-1a 32 over the trimmed uppercased callsign, low 16 bits.
/// Stable across builds — part of the wire contract.
quint16 peerHash16(QString const &callsign);

/// Slice the compressed envelope into chunk-sized byte blocks.
QList<QByteArray> splitIntoBinaryChunks(QByteArray const &envelope,
                                        int chunkBytes = DEFAULT_CHUNK_BYTES);

/// CRC-16-CCITT-FALSE over raw bytes (byte-domain sibling of
/// ChunkedArq's text bodyCrcHex — same polynomial, no uppercasing).
quint16 payloadCrc16(QByteArray const &bytes);

/// Byte count of chunk `chunkId` (1-based) in a transfer of `total`
/// chunks carrying `totalBytes` — chunkBytes for all but the final
/// chunk, remainder on the final. Returns 0 on inconsistent inputs.
int chunkBytesFor(int chunkId, int total, int totalBytes,
                  int chunkBytes = DEFAULT_CHUNK_BYTES);

/// Marker text bodies. Emit ONLY [A-Z0-9 /] so JS8's wire uppercasing
/// is a no-op (harness-enforced).
///   chunk 1:  "F/V3 NATIVE/GZIP <TOTAL> <KB> <PCRC>"
///   periodic: "V3 <PCRC>"
/// KB (chunk size in bytes) must ride the chunk-1 marker explicitly:
/// with sparse markers the receiver cannot derive it from TOTAL and
/// TT alone (harness-caught ambiguity, 2026-07-19).
QString composeMarkerBody(bool firstChunk, int totalBytes, quint16 pcrc,
                          int chunkBytes = DEFAULT_CHUNK_BYTES);

struct MarkerInfo {
    bool    isFirstChunkForm{false};
    int     totalBytes{0};   // valid on first-chunk form only
    int     chunkBytes{0};   // valid on first-chunk form only
    quint16 pcrc{0};
};
/// Anchored, range-checked parse of a marker body. Returns false for
/// anything that is not exactly a V3 marker (so ordinary chunk text
/// falls through to the existing handlers).
bool parseMarkerBody(QString const &body, MarkerInfo *out);

/// Receive-side collector for ONE chunk. Pure state — the caller owns
/// timers and the ACK/NACK decisions.
struct ChunkCollector {
    void open(int chunkId, int nBytes, quint16 pcrc, bool pcrcValid);
    /// Binding (chk4 + seq range) + idempotent store. Returns true if
    /// the frame was accepted (or was an exact duplicate).
    bool accept(int seq, int chk4, QByteArray const &payload8);
    bool complete() const;   // all ceil(nBytes/8) SEQs present
    bool crcOk() const;      // pcrcValid ? compare : true (markerless)
    QList<int> missingSeqs() const;
    void reset();

    int        chunkId() const { return m_chunkId; }
    int        nBytes() const { return m_nBytes; }
    QByteArray bytes() const { return m_bytes; }
    int        frameCount() const;

  private:
    int        m_chunkId{-1};
    int        m_nBytes{0};
    quint16    m_pcrc{0};
    bool       m_pcrcValid{false};
    quint32    m_seqMask{0};
    QByteArray m_bytes;
};

}  // namespace NativeBinary

#endif  // NATIVEBINARY_H
