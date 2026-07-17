/**
 * @file FileTransfer.cpp
 * @brief ARQ file-transfer Phase 1 — implementation.
 *
 * See FileTransfer.h for protocol design notes.
 */

#include "FileTransfer.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(filetransfer_js8, "filetransfer.js8", QtWarningMsg)

namespace FileTransfer {

namespace {

// RFC 4648 base32 alphabet (uppercase). Index = 5-bit value.
char const *const kBase32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

// Reverse lookup: char → 5-bit value, or -1 for non-alphabet. Built on
// first use; case-insensitive on the A-Z half.
int base32Value(QChar c) {
    char const u = c.toUpper().toLatin1();
    if (u >= 'A' && u <= 'Z') return u - 'A';
    if (u >= '2' && u <= '7') return 26 + (u - '2');
    return -1;
}

}  // namespace

QString base32Encode(QByteArray const &bytes) {
    QString out;
    int const n = bytes.size();
    out.reserve((n * 8 + 4) / 5);
    quint32 buffer = 0;
    int bitsInBuffer = 0;
    for (int i = 0; i < n; ++i) {
        buffer = (buffer << 8) | static_cast<quint8>(bytes[i]);
        bitsInBuffer += 8;
        while (bitsInBuffer >= 5) {
            int const idx = (buffer >> (bitsInBuffer - 5)) & 0x1F;
            out.append(QChar::fromLatin1(kBase32Alphabet[idx]));
            bitsInBuffer -= 5;
        }
    }
    if (bitsInBuffer > 0) {
        int const idx = (buffer << (5 - bitsInBuffer)) & 0x1F;
        out.append(QChar::fromLatin1(kBase32Alphabet[idx]));
    }
    return out;
}

QByteArray base32Decode(QString const &text) {
    QByteArray out;
    out.reserve((text.size() * 5) / 8);
    quint32 buffer = 0;
    int bitsInBuffer = 0;
    for (QChar const c : text) {
        if (c == QLatin1Char('=')) continue;  // ignore optional padding
        if (c.isSpace()) continue;            // tolerate whitespace
        int const v = base32Value(c);
        if (v < 0) {
            // Non-alphabet character — treat as decode failure rather
            // than silently dropping. Caller relies on byte-count from
            // header to verify completeness; a corrupted base32 char
            // would corrupt the rest of the stream silently.
            qCWarning(filetransfer_js8)
                << "[FT] base32Decode: invalid char" << c
                << "at offset" << out.size();
            return QByteArray();
        }
        buffer = (buffer << 5) | static_cast<quint32>(v);
        bitsInBuffer += 5;
        if (bitsInBuffer >= 8) {
            int const byte = (buffer >> (bitsInBuffer - 8)) & 0xFF;
            out.append(static_cast<char>(byte));
            bitsInBuffer -= 8;
        }
    }
    return out;
}

QString headerToJson(FileHeader const &h) {
    QJsonObject obj;
    obj["name"] = h.name;
    obj["bytes"] = static_cast<qint64>(h.bytes);
    obj["sha256"] = h.sha256;
    QJsonDocument const doc(obj);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

bool headerFromJson(QString const &json, FileHeader &out) {
    QJsonParseError err;
    QJsonDocument const doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(filetransfer_js8)
            << "[FT] headerFromJson: parse failed:" << err.errorString();
        return false;
    }
    QJsonObject const obj = doc.object();
    out.name = obj.value("name").toString();
    out.bytes = static_cast<qint64>(obj.value("bytes").toDouble());
    out.sha256 = obj.value("sha256").toString();
    return !out.name.isEmpty() && out.bytes >= 0;
}

QString buildSendBody(QString const &filePath, FileHeader &outHeader) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(filetransfer_js8)
            << "[FT] buildSendBody: cannot open" << filePath;
        return QString();
    }
    QByteArray const fileBytes = f.readAll();
    f.close();
    if (fileBytes.size() > MAX_FILE_BYTES) {
        qCWarning(filetransfer_js8)
            << "[FT] buildSendBody: raw file too large"
            << fileBytes.size() << ">" << MAX_FILE_BYTES;
        return QString();
    }

    // [build 277 2026-06-16] Compress before base32. Text payloads
    // (FLMSG, ICS-213, ADIF) compress 3-5×; the protocol's 31-chunk
    // ceiling limits raw base32 payload to ~1 KB, so without
    // compression a 1.6 KB ICS-213 hits too_long. With deflate the
    // same ICS-213 fits comfortably. qCompress is Qt's zlib wrapper
    // (deflate stream + 4-byte length prefix) — both ends use it
    // symmetrically. Level 9 = best compression; this is a one-shot
    // small-file path, not a hot loop, so CPU cost is irrelevant.
    QByteArray const compressed = qCompress(fileBytes, 9);

    // Effective protocol budget for the payload portion of the wire
    // body. Match the chunked-ARQ caps: 31 chunks × 60 chars/chunk
    // minus a fixed header allowance covers the JSON header + space
    // separator. If we exceed this, sendChunked would reject with
    // too_long downstream; catch it here with a clearer error and
    // no chunking work done.
    // [BUILD 339 TODO #104] Builder cap raised to the ROLLOVER
    // ceiling (99 chunks); the PEER-level-aware 31-vs-99 decision
    // happens in the sender's qualification step, which needs the
    // built body to count chunks for the negotiated format.
    constexpr int kMaxWireChars      = 99 * 60;          // 5940
    constexpr int kHeaderAllowance   = 220;              // base32 of JSON header + safety
    constexpr int kMaxPayloadChars   = kMaxWireChars - kHeaderAllowance;
    constexpr int kMaxPayloadBytes   = (kMaxPayloadChars * 5) / 8;
    if (compressed.size() > kMaxPayloadBytes) {
        qCWarning(filetransfer_js8)
            << "[FT] buildSendBody: compressed payload too large"
            << "raw=" << fileBytes.size()
            << "compressed=" << compressed.size()
            << "max=" << kMaxPayloadBytes
            << "(needs better compression OR Phase 2 chunk-count expansion)";
        return QString();
    }

    QFileInfo const fi(filePath);
    outHeader.name = fi.fileName();
    outHeader.bytes = fileBytes.size();     // ORIGINAL size (pre-compress)
    outHeader.sha256 = base32Encode(        // SHA of ORIGINAL bytes
        QCryptographicHash::hash(fileBytes, QCryptographicHash::Sha256));

    // [build 278] Compress the JSON header too so both halves of the
    // wire body actually use the codec declared by the prefix
    // (`F/V1 GZIP/BASE32 `). Regulatory disclosure stays honest, and
    // the header gets a modest wire saving for free (~50-80 chars
    // on a typical 100-char JSON header).
    QString const headerJson  = headerToJson(outHeader);
    QByteArray const headerGz = qCompress(headerJson.toUtf8(), 9);
    QString const headerB32   = base32Encode(headerGz);
    QString const payloadB32  = base32Encode(compressed);   // base32 the COMPRESSED bytes
    // [build 279] Separator between headerB32 and payloadB32 is "."
    // (not a space). ChunkedArq's reassembly path uses
    // `parts.join(' ')` to glue chunks together — that inserts a
    // single space between EVERY chunk pair, not just where the
    // original body had spaces. A space separator here was being
    // mistaken for one of those joiner-inserted spaces by
    // splitWireBody, and the parser took the first 60 chars of the
    // header as the whole header. "." is in JS8's freetext alphabet
    // but NOT in base32's A-Z 2-7 alphabet, so it survives the wire
    // unchanged and can't collide with the encoded data.
    QString const body = QStringLiteral("%1%2.%3")
                             .arg(QString::fromLatin1(PREFIX_V1),
                                  headerB32, payloadB32);
    qCWarning(filetransfer_js8)
        << "[FT-TX] body built: name=" << outHeader.name
        << "rawBytes=" << outHeader.bytes
        << "compressedBytes=" << compressed.size()
        << "compressionRatio=" << QStringLiteral("%1×")
                                      .arg(double(outHeader.bytes) /
                                           double(compressed.size()), 0, 'f', 2)
        << "sha256=" << outHeader.sha256.left(12) + QStringLiteral("...")
        << "wireBodyChars=" << body.size();
    return body;
}

QString sanitizeFilename(QString const &untrusted) {
    QString const baseOnly = QFileInfo(untrusted).fileName();
    if (baseOnly.isEmpty() ||
        baseOnly == QStringLiteral(".") ||
        baseOnly == QStringLiteral("..") ||
        baseOnly.contains(QLatin1Char('/')) ||
        baseOnly.contains(QLatin1Char('\\')) ||
        baseOnly.startsWith(QLatin1Char('.'))) {
        QString const ts = QDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyyMMdd_hhmmssZ"));
        return QStringLiteral("received_%1.bin").arg(ts);
    }
    return baseOnly;
}

QString assembleReceivedFile(QString const &saveDir,
                             FileHeader const &header,
                             QString const &base32Payload,
                             QString *outErr) {
    auto fail = [&](char const *why) {
        if (outErr) *outErr = QString::fromLatin1(why);
        qCWarning(filetransfer_js8) << "[FT-RX]" << why;
        return QString();
    };

    QByteArray const compressed = base32Decode(base32Payload);
    if (compressed.isEmpty() && header.bytes > 0) {
        return fail("base32 decode failed");
    }

    // [build 277 2026-06-16] Decompress the deflate-compressed payload.
    // qUncompress is symmetric with qCompress (zlib deflate stream
    // prefixed by a 4-byte big-endian length). Returns empty on any
    // corruption — invalid header, bad CRC inside the deflate stream,
    // or length-prefix mismatch. The earlier per-chunk CRC-16 should
    // have caught most corruption upstream; a decompression failure
    // here usually means a still-undetected wire error.
    QByteArray bytes = qUncompress(compressed);
    if (bytes.isEmpty() && header.bytes > 0) {
        return fail("decompression failed");
    }
    if (bytes.size() != header.bytes) {
        qCWarning(filetransfer_js8)
            << "[FT-RX] size mismatch: header.bytes=" << header.bytes
            << "decompressed=" << bytes.size();
        return fail("decompressed size mismatch");
    }

    QString const sha = base32Encode(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
    if (sha.compare(header.sha256, Qt::CaseInsensitive) != 0) {
        qCWarning(filetransfer_js8)
            << "[FT-RX] sha256 mismatch: header=" << header.sha256
            << "computed=" << sha;
        return fail("sha256 mismatch");
    }

    return writeReceivedFile(saveDir, header, bytes, outErr);
}

// [BUILD 339 TODO #103] Write half of the receive pipeline, shared
// by V1 (assembleReceivedFile decodes + verifies, then calls this)
// and V2 (splitWireBodyV2 verified already; the UI slot calls this
// directly).
QString writeReceivedFile(QString const &saveDir,
                          FileHeader const &header,
                          QByteArray const &bytes,
                          QString *outErr) {
    auto fail = [&](char const *why) {
        if (outErr) *outErr = QString::fromLatin1(why);
        qCWarning(filetransfer_js8) << "[FT-RX]" << why;
        return QString();
    };

    QDir dir(saveDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return fail("cannot create save dir");
    }

    QString const safeName = sanitizeFilename(header.name);
    QString outPath = dir.filePath(safeName);

    // Collision-safe: if file exists, append _01, _02, ... before the
    // extension until a free name is found.
    if (QFile::exists(outPath)) {
        QFileInfo const fi(outPath);
        QString const base = fi.completeBaseName();
        QString const suf  = fi.suffix();
        for (int n = 1; n < 100; ++n) {
            QString const candidate = dir.filePath(
                suf.isEmpty()
                    ? QStringLiteral("%1_%2").arg(base).arg(n, 2, 10, QLatin1Char('0'))
                    : QStringLiteral("%1_%2.%3").arg(base).arg(n, 2, 10, QLatin1Char('0')).arg(suf));
            if (!QFile::exists(candidate)) {
                outPath = candidate;
                break;
            }
        }
    }

    QFile w(outPath);
    if (!w.open(QIODevice::WriteOnly)) {
        return fail("cannot open save path for write");
    }
    qint64 const written = w.write(bytes);
    w.close();
    if (written != bytes.size()) {
        QFile::remove(outPath);
        return fail("short write");
    }
    qCWarning(filetransfer_js8)
        << "[FT-RX] saved: path=" << outPath << "bytes=" << bytes.size();
    return outPath;
}

bool splitWireBody(QString const &body,
                   FileHeader &outHeader,
                   QString &outPayloadBase32) {
    QString const prefix = QString::fromLatin1(PREFIX_V1);
    if (!body.startsWith(prefix)) return false;
    // [build 279] Strip ALL whitespace from the post-prefix body
    // before searching for the header/payload boundary. ChunkedArq's
    // reassembly inserts spaces between every chunk pair (line 806:
    // parts.join(' ')), so the body has many incidental spaces that
    // are NOT part of the original encoding. base32Decode tolerates
    // whitespace inside its input, so stripping is safe. The
    // boundary character "." is in JS8's freetext alphabet but NOT
    // in base32, so it's a reliable split point.
    QString rest = body.mid(prefix.size());
    rest.remove(QRegularExpression(QStringLiteral("\\s+")));
    int const sp = rest.indexOf(QLatin1Char('.'));
    if (sp <= 0) return false;
    QString const headerB32 = rest.left(sp);
    outPayloadBase32 = rest.mid(sp + 1);
    // [build 278] Symmetric with the TX-side header compression:
    // base32-decode → qUncompress → JSON parse. Both halves of the
    // wire body use the same codec (the one named by the prefix).
    QByteArray const headerGz = base32Decode(headerB32);
    if (headerGz.isEmpty()) return false;
    QByteArray const headerJsonBytes = qUncompress(headerGz);
    if (headerJsonBytes.isEmpty()) return false;
    QString const headerJson = QString::fromUtf8(headerJsonBytes);
    if (!headerFromJson(headerJson, outHeader)) return false;
    return true;
}

// ------------------------------------------------------------------
// [BUILD 339 TODO #103] Wire-format V2 — single compressed envelope
// with a binary header. See PREFIX_V2 commentary in the header.
// ------------------------------------------------------------------

QString buildSendBodyV2(QString const &filePath, FileHeader &outHeader) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(filetransfer_js8)
            << "[FT] buildSendBodyV2: cannot open" << filePath;
        return QString();
    }
    QByteArray const fileBytes = f.readAll();
    f.close();
    if (fileBytes.size() > MAX_FILE_BYTES) {
        qCWarning(filetransfer_js8)
            << "[FT] buildSendBodyV2: raw file too large"
            << fileBytes.size() << ">" << MAX_FILE_BYTES;
        return QString();
    }

    QFileInfo const fi(filePath);
    QByteArray nameUtf8 = fi.fileName().toUtf8();
    if (nameUtf8.size() > 255) nameUtf8 = nameUtf8.left(255);
    QByteArray const shaFull =
        QCryptographicHash::hash(fileBytes, QCryptographicHash::Sha256);
    QByteArray const sha16 = shaFull.left(V2_HASH_BYTES);

    // Binary header: u8 nameLen + name + u32be size + sha16.
    QByteArray header;
    header.append(static_cast<char>(nameUtf8.size()));
    header.append(nameUtf8);
    quint32 const sz = static_cast<quint32>(fileBytes.size());
    header.append(static_cast<char>((sz >> 24) & 0xFF));
    header.append(static_cast<char>((sz >> 16) & 0xFF));
    header.append(static_cast<char>((sz >> 8) & 0xFF));
    header.append(static_cast<char>(sz & 0xFF));
    header.append(sha16);

    // ONE compression envelope over header ‖ payload — V1's separate
    // header compression was a measured net LOSS (incompressible hash
    // + 12-byte qCompress framing + extra base32 padding boundary).
    QByteArray const envelope = qCompress(header + fileBytes, 9);

    constexpr int kMaxWireChars = 99 * 60;  // rollover ceiling; peer-aware check in sender
    QString const b32 = base32Encode(envelope);
    QString const body = QString::fromLatin1(PREFIX_V2) + b32;
    if (body.size() > kMaxWireChars) {
        qCWarning(filetransfer_js8)
            << "[FT] buildSendBodyV2: wire body too large"
            << body.size() << ">" << kMaxWireChars;
        return QString();
    }

    outHeader.name = fi.fileName();
    outHeader.bytes = fileBytes.size();
    outHeader.sha256 = base32Encode(sha16);

    qCWarning(filetransfer_js8)
        << "[FT-TX] V2 body built: name=" << outHeader.name
        << "rawBytes=" << outHeader.bytes
        << "envelopeBytes=" << envelope.size()
        << "wireBodyChars=" << body.size();
    return body;
}

QString buildLinkBody(QString const &url) {
    QUrl const parsed = QUrl(url.trimmed());
    if (!parsed.isValid() ||
        (parsed.scheme() != QStringLiteral("http") &&
         parsed.scheme() != QStringLiteral("https"))) {
        qCWarning(filetransfer_js8)
            << "[LT-TX] buildLinkBody: not an http(s) URL";
        return QString();
    }
    QString const body =
        QString::fromLatin1(PREFIX_L1) +
        base32Encode(parsed.toString(QUrl::FullyEncoded).toUtf8());
    qCWarning(filetransfer_js8)
        << "[LT-TX] link body built: wireBodyChars=" << body.size();
    return body;
}

bool splitLinkBody(QString const &body, QString &outUrl) {
    QString const prefix = QString::fromLatin1(PREFIX_L1);
    if (!body.startsWith(prefix)) return false;
    QString rest = body.mid(prefix.size());
    rest.remove(QRegularExpression(QStringLiteral("\\s+")));
    QByteArray const bytes = base32Decode(rest);
    if (bytes.isEmpty()) return false;
    QUrl const parsed = QUrl(QString::fromUtf8(bytes));
    if (!parsed.isValid() ||
        (parsed.scheme() != QStringLiteral("http") &&
         parsed.scheme() != QStringLiteral("https"))) {
        qCWarning(filetransfer_js8)
            << "[LT-RX] decoded payload is not an http(s) URL";
        return false;
    }
    outUrl = parsed.toString(QUrl::FullyEncoded);
    return true;
}

bool splitWireBodyV2(QString const &body,
                     FileHeader &outHeader,
                     QByteArray &outPayloadBytes) {
    QString const prefix = QString::fromLatin1(PREFIX_V2);
    if (!body.startsWith(prefix)) return false;
    QString rest = body.mid(prefix.size());
    rest.remove(QRegularExpression(QStringLiteral("\\s+")));
    QByteArray const envelope = base32Decode(rest);
    if (envelope.isEmpty()) return false;
    QByteArray const plain = qUncompress(envelope);
    // Minimum: 1 (nameLen) + 0 (name) + 4 (size) + 16 (sha16).
    if (plain.size() < 1 + 4 + V2_HASH_BYTES) return false;

    int const nameLen = static_cast<quint8>(plain.at(0));
    int const hdrLen = 1 + nameLen + 4 + V2_HASH_BYTES;
    if (plain.size() < hdrLen) return false;

    QString const name = QString::fromUtf8(plain.mid(1, nameLen));
    quint32 const sz =
        (static_cast<quint8>(plain.at(1 + nameLen)) << 24) |
        (static_cast<quint8>(plain.at(2 + nameLen)) << 16) |
        (static_cast<quint8>(plain.at(3 + nameLen)) << 8) |
        static_cast<quint8>(plain.at(4 + nameLen));
    QByteArray const sha16 = plain.mid(1 + nameLen + 4, V2_HASH_BYTES);
    QByteArray const payload = plain.mid(hdrLen);

    if (payload.size() != static_cast<int>(sz)) {
        qCWarning(filetransfer_js8)
            << "[FT-RX] V2 size mismatch: header=" << sz
            << "payload=" << payload.size();
        return false;
    }
    QByteArray const computed =
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256)
            .left(V2_HASH_BYTES);
    if (computed != sha16) {
        qCWarning(filetransfer_js8) << "[FT-RX] V2 sha mismatch";
        return false;
    }

    outHeader.name = name;
    outHeader.bytes = sz;
    outHeader.sha256 = base32Encode(sha16);
    outPayloadBytes = payload;
    return true;
}

}  // namespace FileTransfer
