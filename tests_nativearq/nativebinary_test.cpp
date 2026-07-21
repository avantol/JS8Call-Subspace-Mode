// [TODO #107] Offline harness for the native-binary (F/V3) pure logic.
// Project process rule: EXTEND THIS FIRST, run green, then build the app.
// Build: ./build.sh   (g++ + Qt6Core, no moc needed — no QObject here)

#include "JS8_Main/FileTransfer.h"
#include "JS8_Main/NativeBinary.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <cstdio>

using namespace NativeBinary;

static int fails = 0;
#define CHECK(cond, label)                                              \
    do {                                                                \
        bool const ok_ = (cond);                                        \
        if (!ok_) ++fails;                                              \
        printf("%s  %s\n", ok_ ? "PASS" : "FAIL", label);               \
    } while (0)

// Deterministic PRNG (no seeds from clock — reproducible runs).
static quint64 lcg = 0x2545F4914F6CDD1DULL;
static quint8 rnd8() {
    lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<quint8>(lcg >> 33);
}

// --- Reference reimplementations (guard the cross-module contracts) ---

// mainwindow.cpp FT2 TX branch: msgbits77[0..71] from (value, rem).
static void refBitsFromValueRem(quint64 value, quint8 rem, int bits[72]) {
    for (int i = 0; i < 64; ++i) bits[i] = (value >> (63 - i)) & 1;
    for (int i = 0; i < 8; ++i) bits[64 + i] = (rem >> (7 - i)) & 1;
}

// The 9 wire bytes as the frame codec defines them (bit 0 = MSB byte 0).
static void refBitsFromBytes(quint8 const b[9], int bits[72]) {
    for (int i = 0; i < 72; ++i)
        bits[i] = (b[i / 8] >> (7 - (i % 8))) & 1;
}

// Varicode::pack72bits / unpack72bits (Varicode.cpp:794-833), verbatim
// math, so we can prove Frame72 survives the 12-char queue round trip
// without linking all of Varicode.
static QString const kAlphabet72 = QStringLiteral(
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?.");
static QString refPack72(quint64 value, quint8 rem) {
    QChar packed[12];
    quint8 const mask4 = 0xF, mask6 = 0x3F;
    quint8 const remHigh = ((value & mask4) << 2) | (rem >> 6);
    quint8 const remLow = rem & mask6;
    value >>= 4;
    packed[11] = kAlphabet72.at(remLow);
    packed[10] = kAlphabet72.at(remHigh);
    for (int i = 0; i < 10; ++i) {
        packed[9 - i] = kAlphabet72.at(value & mask6);
        value >>= 6;
    }
    return QString(packed, 12);
}
static quint64 refUnpack72(QString const &text, quint8 *pRem) {
    quint64 value = 0;
    for (int i = 0; i < 10; ++i)
        value |= static_cast<quint64>(kAlphabet72.indexOf(text.at(i)))
                 << (58 - 6 * i);
    quint8 const remHigh = kAlphabet72.indexOf(text.at(10));
    value |= remHigh >> 2;
    quint8 const remLow = kAlphabet72.indexOf(text.at(11));
    if (pRem) *pRem = ((remHigh & 0x3) << 6) | remLow;
    return value;
}

static void frameCodecTests() {
    // Corner payloads + full round trip incl. the 12-char frame string.
    QList<QByteArray> corners;
    corners << QByteArray(8, '\0') << QByteArray(8, char(0xFF));
    for (int byte = 0; byte < 8; ++byte)
        for (int bit = 0; bit < 8; ++bit) {
            QByteArray p(8, '\0');
            p[byte] = char(1 << bit);
            corners << p;
        }
    int bad = 0;
    for (int seq = 0; seq < 16; ++seq)
        for (int chk = 0; chk < 16; ++chk)
            for (auto const &p : corners) {
                Frame72 const f = encodeFrame(seq, chk, p);
                // through the 12-char queue representation
                quint8 rem2;
                quint64 const v2 = refUnpack72(refPack72(f.value, f.rem), &rem2);
                int s, c;
                QByteArray out;
                decodeFrame(v2, rem2, &s, &c, &out);
                if (s != seq || c != chk || out != p) ++bad;
                // bit-contract: TX loop bits == byte-domain bits
                int bitsA[72], bitsB[72];
                refBitsFromValueRem(f.value, f.rem, bitsA);
                quint8 b[9];
                b[0] = quint8((seq << 4) | chk);
                for (int i = 0; i < 8; ++i) b[1 + i] = quint8(p.at(i));
                refBitsFromBytes(b, bitsB);
                for (int i = 0; i < 72; ++i)
                    if (bitsA[i] != bitsB[i]) ++bad;
            }
    CHECK(bad == 0, "frame codec: corners x seq x chk4, 12-char round trip + bit contract");

    bad = 0;
    for (int n = 0; n < 10000; ++n) {
        QByteArray p(8, '\0');
        for (int i = 0; i < 8; ++i) p[i] = char(rnd8());
        int const seq = rnd8() & 0xF, chk = rnd8() & 0xF;
        Frame72 const f = encodeFrame(seq, chk, p);
        int s, c;
        QByteArray out;
        decodeFrame(f.value, f.rem, &s, &c, &out);
        if (s != seq || c != chk || out != p) ++bad;
    }
    CHECK(bad == 0, "frame codec: 10k random round trips");

    // Short final slice: 3 bytes -> padded to 8 zeros on decode.
    Frame72 const f = encodeFrame(2, 5, QByteArray("\x01\x02\x03", 3));
    int s, c;
    QByteArray out;
    decodeFrame(f.value, f.rem, &s, &c, &out);
    CHECK(s == 2 && c == 5 && out.left(3) == QByteArray("\x01\x02\x03", 3) &&
              out.mid(3) == QByteArray(5, '\0'),
          "frame codec: short final slice zero-padded");
}

static void chunkTests() {
    for (int size : {0, 1, 7, 8, 9, 63, 64, 65, 128, 6336}) {
        QByteArray env(size, '\0');
        for (int i = 0; i < size; ++i) env[i] = char(rnd8());
        auto const chunks = splitIntoBinaryChunks(env, 64);
        QByteArray join;
        for (auto const &ch : chunks) join += ch;
        bool ok = (join == env);
        if (size > 0)
            ok = ok && chunks.size() == (size + 63) / 64;
        else
            ok = ok && chunks.isEmpty();
        char label[64];
        snprintf(label, sizeof label, "split/join size %d", size);
        CHECK(ok, label);
    }

    // CRC-16-CCITT-FALSE standard check value.
    CHECK(payloadCrc16(QByteArray("123456789")) == 0x29B1,
          "payloadCrc16 known answer 0x29B1");

    CHECK(chunkBytesFor(1, 3, 150) == 64 && chunkBytesFor(2, 3, 150) == 64 &&
              chunkBytesFor(3, 3, 150) == 22,
          "chunkBytesFor: 150 B over 3 chunks = 64/64/22");
    CHECK(chunkBytesFor(1, 1, 64) == 64 && chunkBytesFor(1, 1, 1) == 1,
          "chunkBytesFor: single chunk");
    CHECK(chunkBytesFor(0, 3, 150) == 0 && chunkBytesFor(4, 3, 150) == 0 &&
              chunkBytesFor(3, 3, 300) == 0,
          "chunkBytesFor: rejects out-of-range / inconsistent");
}

static void markerTests() {
    quint16 const crc = payloadCrc16(QByteArray("marker-test"));
    QString const first = composeMarkerBody(true, 6336, crc);
    QString const periodic = composeMarkerBody(false, 0, crc);
    CHECK(first == first.toUpper() && periodic == periodic.toUpper(),
          "markers: uppercase-wire invariant");

    MarkerInfo mi;
    CHECK(parseMarkerBody(first, &mi) && mi.isFirstChunkForm &&
              mi.totalBytes == 6336 && mi.chunkBytes == 64 &&
              mi.pcrc == crc,
          "marker: first-chunk form round trip (default KB)");
    CHECK(parseMarkerBody(composeMarkerBody(true, 200, crc, 120), &mi) &&
              mi.chunkBytes == 120,
          "marker: first-chunk form carries explicit KB=120");
    CHECK(parseMarkerBody(periodic, &mi) && !mi.isFirstChunkForm &&
              mi.pcrc == crc,
          "marker: periodic form round trip");

    char const *rejects[] = {
        "F/V3 NATIVE/GZIP 0 64 ABCD",      // TOTAL 0
        "F/V3 NATIVE/GZIP 999999 64 ABCD", // over ceiling
        "F/V3 NATIVE/GZIP 100 64 abcd",    // lowercase hex (wire is upper)
        "F/V3 NATIVE/GZIP 100 64 ABCD X",  // trailing junk
        "F/V3 NATIVE/GZIP 100 63 ABCD",    // KB not a multiple of 8
        "F/V3 NATIVE/GZIP 100 136 ABCD",   // KB over protocol max
        "F/V3 NATIVE/GZIP 100 ABCD",       // missing KB (old form)
        "V3 ABCDE",                        // 5-digit crc
        "V2 ABCD",                         // wrong family
        "F/V2 GZIP/BASE32 AAAA",           // V2 body must not match
        "HELLO OLD FRIEND",                // plain text
        "",
    };
    int bad = 0;
    for (auto const *r : rejects)
        if (parseMarkerBody(QString::fromUtf8(r), nullptr)) ++bad;
    CHECK(bad == 0, "marker: all malformed forms rejected");
}

// [BUILD 344 binMarker] Marker-frame codec: exact round trip across
// field extremes, strict-reject on bad ranges / non-marker SEQ, and
// hash stability (wire contract — a changed hash strands the fleet).
static void markerFrameTests() {
    MarkerFrame m;
    m.msgId = 22; m.chunkId = 33; m.totalBytes = 3078;
    m.chunkBytes = 64; m.pcrc = 0xA998;
    m.peerHash = peerHash16(QStringLiteral("WM8Q"));
    Frame72 const f = encodeMarkerFrame(m);
    MarkerFrame d;
    CHECK(decodeMarkerFrame(f.value, f.rem, &d) &&
              d.msgId == 22 && d.chunkId == 33 &&
              d.totalBytes == 3078 && d.chunkBytes == 64 &&
              d.pcrc == 0xA998 && d.peerHash == m.peerHash,
          "markerFrame: round trip, typical fields");

    MarkerFrame hi;
    hi.msgId = 99; hi.chunkId = 99;
    hi.totalBytes = 99 * MAX_CHUNK_BYTES;  // 11880, 14-bit max case
    hi.chunkBytes = MAX_CHUNK_BYTES; hi.pcrc = 0xFFFF;
    hi.peerHash = 0xFFFF;
    Frame72 const fh = encodeMarkerFrame(hi);
    MarkerFrame dh;
    CHECK(decodeMarkerFrame(fh.value, fh.rem, &dh) &&
              dh.totalBytes == hi.totalBytes &&
              dh.chunkBytes == MAX_CHUNK_BYTES &&
              dh.chunkId == 99,
          "markerFrame: round trip at field maxima");

    // A data frame (SEQ 0..14) must NOT parse as a marker.
    Frame72 const df = encodeFrame(7, 3, QByteArray(8, '\x5A'));
    CHECK(!decodeMarkerFrame(df.value, df.rem, nullptr),
          "markerFrame: data frame rejected (SEQ != 15)");

    // Header chk4 / CC mismatch rejected (corruption guard).
    Frame72 fx = encodeMarkerFrame(m);
    fx.value ^= (quint64(1) << 56);  // flip a header chk4 bit
    CHECK(!decodeMarkerFrame(fx.value, fx.rem, nullptr),
          "markerFrame: chk4/CC mismatch rejected");

    // Hash stability + case/space insensitivity (wire contract).
    CHECK(peerHash16(QStringLiteral("wm8q ")) ==
              peerHash16(QStringLiteral("WM8Q")) &&
          peerHash16(QStringLiteral("WM8Q")) !=
              peerHash16(QStringLiteral("WM8Q/P")),
          "markerFrame: peerHash16 normalized + discriminates");
}

static void collectorTests() {
    QByteArray chunk(64, '\0');
    for (int i = 0; i < 64; ++i) chunk[i] = char(rnd8());
    quint16 const crc = payloadCrc16(chunk);

    auto feed = [&](ChunkCollector &c, int seq) {
        Frame72 const f =
            encodeFrame(seq, 7, chunk.mid(seq * 8, 8));
        int s, k;
        QByteArray p;
        decodeFrame(f.value, f.rem, &s, &k, &p);
        return c.accept(s, k, p);
    };

    ChunkCollector c;
    c.open(7, 64, crc, true);
    for (int seq = 0; seq < 8; ++seq) feed(c, seq);
    CHECK(c.complete() && c.crcOk() && c.bytes() == chunk,
          "collector: in-order complete + crc + bytes");

    c.open(7, 64, crc, true);
    for (int seq = 7; seq >= 0; --seq) feed(c, seq);
    CHECK(c.complete() && c.crcOk() && c.bytes() == chunk,
          "collector: reverse order");

    c.open(7, 64, crc, true);
    for (int seq = 0; seq < 8; ++seq) { feed(c, seq); feed(c, seq); }
    CHECK(c.complete() && c.crcOk(), "collector: duplicates idempotent");

    c.open(7, 64, crc, true);
    CHECK(!c.accept(0, 6, chunk.left(8)), "collector: wrong chk4 rejected");
    CHECK(!c.accept(8, 7, chunk.left(8)), "collector: seq out of range rejected");

    c.open(7, 64, crc, true);
    feed(c, 0); feed(c, 3); feed(c, 7);
    CHECK(!c.complete() &&
              c.missingSeqs() == QList<int>({1, 2, 4, 5, 6}),
          "collector: missingSeqs");

    // Markerless: no pcrc -> crcOk always true once complete.
    c.open(9, 64, 0, false);
    QByteArray chunk9(64, '\0');
    for (int i = 0; i < 64; ++i) chunk9[i] = char(rnd8());
    for (int seq = 0; seq < 8; ++seq) {
        Frame72 const f = encodeFrame(seq, 9, chunk9.mid(seq * 8, 8));
        int s, k; QByteArray p;
        decodeFrame(f.value, f.rem, &s, &k, &p);
        c.accept(s, k, p);
    }
    CHECK(c.complete() && c.crcOk() && c.bytes() == chunk9,
          "collector: markerless chunk (pcrcValid=false)");

    // Corrupted byte -> crc fails (marker chunk).
    c.open(7, 64, crc, true);
    QByteArray evil = chunk; evil[10] = char(evil.at(10) ^ 0x40);
    for (int seq = 0; seq < 8; ++seq) {
        Frame72 const f = encodeFrame(seq, 7, evil.mid(seq * 8, 8));
        int s, k; QByteArray p;
        decodeFrame(f.value, f.rem, &s, &k, &p);
        c.accept(s, k, p);
    }
    CHECK(c.complete() && !c.crcOk(), "collector: corrupted chunk fails pcrc");

    // Final partial chunk: 22 bytes = 3 frames, last carries 6.
    QByteArray tail(22, '\0');
    for (int i = 0; i < 22; ++i) tail[i] = char(rnd8());
    c.open(3, 22, payloadCrc16(tail), true);
    CHECK(c.frameCount() == 3, "collector: 22 B = 3 frames");
    for (int seq = 0; seq < 3; ++seq) {
        Frame72 const f = encodeFrame(seq, 3, tail.mid(seq * 8, 8));
        int s, k; QByteArray p;
        decodeFrame(f.value, f.rem, &s, &k, &p);
        c.accept(s, k, p);
    }
    CHECK(c.complete() && c.crcOk() && c.bytes() == tail,
          "collector: final partial chunk trims padding");
}

static void fileTransferV3Tests() {
    QTemporaryDir dir;
    QString const path = dir.filePath(QStringLiteral("v3test.bin"));
    QByteArray raw(1500, '\0');
    for (int i = 0; i < raw.size(); ++i) raw[i] = char(rnd8());
    {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(raw);
    }

    FileTransfer::FileHeader hdr;
    QByteArray const envelope = FileTransfer::buildSendBodyV3(path, hdr);
    CHECK(!envelope.isEmpty() && hdr.bytes == raw.size(),
          "V3: envelope built");

    // Simulate the full wire: split -> frames -> collect -> reassemble.
    auto const chunks = splitIntoBinaryChunks(envelope, 64);
    int const total = chunks.size();
    QByteArray reassembled;
    bool wireOk = true;
    for (int cc = 1; cc <= total; ++cc) {
        QByteArray const &cb = chunks.at(cc - 1);
        int const nb = chunkBytesFor(cc, total, envelope.size());
        wireOk = wireOk && (nb == cb.size());
        ChunkCollector col;
        col.open(cc, nb, payloadCrc16(cb), true);
        int const frames = col.frameCount();
        for (int seq = 0; seq < frames; ++seq) {
            Frame72 const f = encodeFrame(seq, cc, cb.mid(seq * 8, 8));
            quint8 rem2;
            quint64 const v2 = refUnpack72(refPack72(f.value, f.rem), &rem2);
            int s, k; QByteArray p;
            decodeFrame(v2, rem2, &s, &k, &p);
            col.accept(s, k, p);
        }
        wireOk = wireOk && col.complete() && col.crcOk();
        reassembled += col.bytes();
    }
    CHECK(wireOk, "V3: all chunks collected through simulated wire");

    FileTransfer::FileHeader hdr2;
    QByteArray fileBytes;
    CHECK(FileTransfer::splitWireBodyV3(reassembled, hdr2, fileBytes) &&
              fileBytes == raw && hdr2.name == hdr.name &&
              hdr2.sha256 == hdr.sha256,
          "V3: envelope parse + sha16 verify + byte-identical file");

    // V2 must still round-trip through the SAME shared helpers.
    FileTransfer::FileHeader hv2;
    QString const bodyV2 = FileTransfer::buildSendBodyV2(path, hv2);
    FileTransfer::FileHeader hv2rx;
    QByteArray v2bytes;
    CHECK(!bodyV2.isEmpty() &&
              FileTransfer::splitWireBodyV2(bodyV2, hv2rx, v2bytes) &&
              v2bytes == raw,
          "V2 regression: unchanged through refactored helpers");
}

int main() {
    frameCodecTests();
    chunkTests();
    markerTests();
    markerFrameTests();
    collectorTests();
    fileTransferV3Tests();
    printf("\n%d failures\n", fails);
    return fails ? 1 : 0;
}
