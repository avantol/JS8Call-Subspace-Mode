/**
 * @file NativeBinary.cpp
 * @brief See NativeBinary.h. PURE logic — offline harness
 *        tests_nativearq/ MUST be extended first for any change here.
 */
#include "JS8_Main/NativeBinary.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>

#include <vendor/CRCpp/CRC.h>

namespace NativeBinary {

Frame72 encodeFrame(int const seq, int const chunkId,
                    QByteArray const &payload8) {
    // 9 wire bytes: header + zero-padded payload.
    quint8 b[9] = {};
    b[0] = static_cast<quint8>(((seq & 0xF) << 4) | (chunkId & 0xF));
    int const n = qMin(payload8.size(),
                       static_cast<qsizetype>(FRAME_PAYLOAD_BYTES));
    for (int i = 0; i < n; ++i)
        b[1 + i] = static_cast<quint8>(payload8.at(i));

    // Wire bit i = msgbits77[i]; bits 0..63 = value MSB-first, bits
    // 64..71 = rem MSB-first (mainwindow.cpp FT2 TX branch). Hence:
    // value = bytes 0..7 big-endian, rem = byte 8.
    Frame72 f;
    for (int i = 0; i < 8; ++i)
        f.value = (f.value << 8) | b[i];
    f.rem = b[8];
    return f;
}

bool decodeFrame(quint64 const value, quint8 const rem,
                 int *seq, int *chk4, QByteArray *payload8) {
    quint8 b[9];
    for (int i = 0; i < 8; ++i)
        b[i] = static_cast<quint8>((value >> (8 * (7 - i))) & 0xFF);
    b[8] = rem;

    if (seq)  *seq  = (b[0] >> 4) & 0xF;
    if (chk4) *chk4 = b[0] & 0xF;
    if (payload8) {
        payload8->resize(FRAME_PAYLOAD_BYTES);
        for (int i = 0; i < FRAME_PAYLOAD_BYTES; ++i)
            (*payload8)[i] = static_cast<char>(b[1 + i]);
    }
    return true;
}

QList<QByteArray> splitIntoBinaryChunks(QByteArray const &envelope,
                                        int const chunkBytes) {
    QList<QByteArray> out;
    if (chunkBytes <= 0 || chunkBytes > MAX_CHUNK_BYTES)
        return out;
    for (qsizetype pos = 0; pos < envelope.size(); pos += chunkBytes)
        out.append(envelope.mid(pos, chunkBytes));
    return out;
}

quint16 payloadCrc16(QByteArray const &bytes) {
    return CRC::Calculate(bytes.constData(),
                          static_cast<size_t>(bytes.size()),
                          CRC::CRC_16_CCITTFALSE());
}

int chunkBytesFor(int const chunkId, int const total, int const totalBytes,
                  int const chunkBytes) {
    if (chunkId < 1 || total < 1 || chunkId > total || totalBytes < 1 ||
        chunkBytes < 1 || chunkBytes > MAX_CHUNK_BYTES)
        return 0;
    if (chunkId < total)
        return chunkBytes;
    int const finalBytes = totalBytes - chunkBytes * (total - 1);
    return (finalBytes >= 1 && finalBytes <= chunkBytes) ? finalBytes : 0;
}

QString composeMarkerBody(bool const firstChunk, int const totalBytes,
                          quint16 const pcrc, int const chunkBytes) {
    QString const crcHex = QStringLiteral("%1")
                               .arg(pcrc, 4, 16, QLatin1Char('0'))
                               .toUpper();
    if (firstChunk) {
        // Chunk size rides the chunk-1 marker EXPLICITLY: with sparse
        // markers the receiver cannot derive it from TOTAL and TT
        // alone (TT=ceil(TOTAL/KB) is consistent with a RANGE of KB
        // values — harness-caught 2026-07-19 on the K=16 case).
        return QStringLiteral("F/V3 NATIVE/GZIP %1 %2 %3")
            .arg(totalBytes)
            .arg(chunkBytes)
            .arg(crcHex);
    }
    return QStringLiteral("V3 %1").arg(crcHex);
}

bool parseMarkerBody(QString const &body, MarkerInfo *out) {
    // Anchored — anything else is ordinary chunk text and must fall
    // through to the existing (V1/V2/plain) handlers.
    static QRegularExpression const kFirstRe(QStringLiteral(
        R"(^F/V3 NATIVE/GZIP (?<total>\d{1,5}) (?<kb>\d{1,3}) (?<crc>[0-9A-F]{4})$)"));
    static QRegularExpression const kPeriodicRe(
        QStringLiteral(R"(^V3 (?<crc>[0-9A-F]{4})$)"));

    QString const b = body.trimmed();
    if (auto const m = kFirstRe.match(b); m.hasMatch()) {
        int const total = m.captured(QStringLiteral("total")).toInt();
        int const kb = m.captured(QStringLiteral("kb")).toInt();
        // Ceilings: 99 chunks x MAX_CHUNK_BYTES; chunk size a
        // positive multiple of the frame payload, <= protocol max.
        if (total < 1 || total > 99 * MAX_CHUNK_BYTES)
            return false;
        if (kb < FRAME_PAYLOAD_BYTES || kb > MAX_CHUNK_BYTES ||
            (kb % FRAME_PAYLOAD_BYTES) != 0)
            return false;
        if (out) {
            out->isFirstChunkForm = true;
            out->totalBytes = total;
            out->chunkBytes = kb;
            out->pcrc = static_cast<quint16>(
                m.captured(QStringLiteral("crc")).toUInt(nullptr, 16));
        }
        return true;
    }
    if (auto const m = kPeriodicRe.match(b); m.hasMatch()) {
        if (out) {
            out->isFirstChunkForm = false;
            out->totalBytes = 0;
            out->chunkBytes = 0;
            out->pcrc = static_cast<quint16>(
                m.captured(QStringLiteral("crc")).toUInt(nullptr, 16));
        }
        return true;
    }
    return false;
}

void ChunkCollector::open(int const chunkId, int const nBytes,
                          quint16 const pcrc, bool const pcrcValid) {
    m_chunkId = chunkId;
    m_nBytes = qBound(0, nBytes, MAX_CHUNK_BYTES);
    m_pcrc = pcrc;
    m_pcrcValid = pcrcValid;
    m_seqMask = 0;
    m_bytes = QByteArray(m_nBytes, '\0');
}

int ChunkCollector::frameCount() const {
    return (m_nBytes + FRAME_PAYLOAD_BYTES - 1) / FRAME_PAYLOAD_BYTES;
}

bool ChunkCollector::accept(int const seq, int const chk4,
                            QByteArray const &payload8) {
    if (m_chunkId < 0 || m_nBytes <= 0)
        return false;
    if (chk4 != (m_chunkId & 0xF))
        return false;
    if (seq < 0 || seq >= frameCount())
        return false;
    int const offset = seq * FRAME_PAYLOAD_BYTES;
    int const n = qMin(FRAME_PAYLOAD_BYTES, m_nBytes - offset);
    for (int i = 0; i < n && i < payload8.size(); ++i)
        m_bytes[offset + i] = payload8.at(i);
    m_seqMask |= (1u << seq);
    return true;
}

bool ChunkCollector::complete() const {
    if (m_chunkId < 0 || m_nBytes <= 0)
        return false;
    quint32 const want = (frameCount() >= 32)
                             ? ~0u
                             : ((1u << frameCount()) - 1);
    return (m_seqMask & want) == want;
}

bool ChunkCollector::crcOk() const {
    if (!m_pcrcValid)
        return true;  // markerless chunk: CRC14/frame + completeness + sha16
    return payloadCrc16(m_bytes) == m_pcrc;
}

QList<int> ChunkCollector::missingSeqs() const {
    QList<int> out;
    for (int s = 0; s < frameCount(); ++s)
        if (!(m_seqMask & (1u << s)))
            out.append(s);
    return out;
}

void ChunkCollector::reset() {
    m_seqMask = 0;
    if (m_nBytes > 0)
        m_bytes.fill('\0', m_nBytes);
}

}  // namespace NativeBinary
