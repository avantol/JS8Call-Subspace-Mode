#ifndef JS8CALL_FILETRANSFER_H
#define JS8CALL_FILETRANSFER_H

/**
 * @file FileTransfer.h
 * @brief ARQ file-transfer Phase 1 — wire helpers layered on chunked-ARQ.
 *
 * Phase 1 design: each file transfer rides as one chunked-ARQ super-
 * message. Chunk 1's body carries a base32-encoded JSON header
 * (filename, byte count, SHA-256). Chunks 2..N carry pure base32-
 * encoded file bytes. The existing chunked-ARQ machinery (per-chunk
 * CRC-16, ACK/NACK, retry, msgId) wraps the whole thing — no new
 * wire protocol.
 *
 * Wire alphabet constraint: JS8 freetext uses a 41-character alphabet
 * (0-9 A-Z + - . / ?) and the encoder uppercases lowercase letters at
 * TX time. Base32 (RFC 4648, alphabet A-Z 2-7) is the natural fit —
 * every character is already in JS8's freetext set so the payload
 * survives the wire byte-for-byte. Padding (=) is omitted by
 * convention; the byte count in the header tells the decoder where
 * to truncate.
 */

#include <QByteArray>
#include <QString>

namespace FileTransfer {

/// Cap on file size for Phase 1. 30 KB → ~30 min TX time in Subspace
/// mode at ~160 bytes/min effective wire throughput. Larger transfers
/// wait for Phase 2 (compression + resume).
constexpr int MAX_FILE_BYTES = 30 * 1024;

/// Magic prefix that marks an ARQ super-message as a file transfer.
/// Receiver detects this on chunk 1; routes to FileTransfer pathway.
///
/// **Regulatory disclosure.** The `GZIP/BASE32` token after the
/// version is a plain-language statement that the payload is
/// standard zlib gzip wrapped in standard RFC 4648 base32 —
/// well-known reversible encodings, NOT encryption. Amateur
/// regulations in most regimes (US Part 97 §97.113(a)(4) and
/// equivalents) prohibit transmissions intended to obscure the
/// meaning of the communication; explicitly naming the codec on
/// the wire demonstrates the absence of any such intent. The
/// version field (V1) stays for protocol-revision use; the codec
/// disclosure is orthogonal.
constexpr char const *PREFIX_V1 = "F/V1 GZIP/BASE32 ";

/// [BUILD 339 TODO #103] Wire-format V2. Same regulatory-disclosure
/// codec token; the difference is INSIDE the base32: ONE compressed
/// envelope containing a BINARY header (u8 nameLen + name utf8 +
/// u32be size + first 16 bytes of SHA-256) immediately followed by
/// the raw file bytes. Eliminates V1's double-encoding tax (the
/// 32-byte hash as a base32 STRING inside JSON, compressed
/// pointlessly, base32'd again): measured 231 → 111 wire chars on a
/// 16-byte file, ~2 sub-messages saved on EVERY transfer. Sender
/// uses V2 only for peers that advertised ARQ_PROTOCOL_LEVEL >= 2
/// via QUERY ARQ? (session-cached); V1 remains the default and is
/// decoded forever.
constexpr char const *PREFIX_V2 = "F/V2 GZIP/BASE32 ";

/// Truncated-hash length for V2. 16 bytes is ample for transfer
/// integrity (corruption check, not an adversarial defense).
constexpr int V2_HASH_BYTES = 16;

/// [BUILD 340] Web-link wire form — NOT a file: no name, no size, no
/// hash (per-chunk CRC-16 covers transport; scheme validation at
/// display covers sanity), no compression (URLs don't compress;
/// qCompress ADDS 12 bytes at these sizes). Base32 exists solely
/// because JS8 uppercases the wire and URLs are case-sensitive.
/// Receiver shows the link directly (clickable, escaped, full URL
/// as text) — no save dialog. Sent only to ARQ level >= 2 peers;
/// level-1 peers get the legacy link.txt file transfer.
constexpr char const *PREFIX_L1 = "L/V1 BASE32 ";

/// [BUILD 341 linkCase] THE link-URL gate both ends use: starts with
/// http:// or https:// (case-insensitive) and contains no whitespace
/// or control characters. Deliberately NOT QUrl-based — QUrl
/// normalizes, and link payloads must stay byte-exact (URL paths,
/// shortener IDs, and signed query tokens are case-sensitive).
bool isValidLinkUrl(QString const &url);

/// Build the wire body for a web link. Empty on invalid input. The
/// URL is transmitted byte-exact (trimmed only, never normalized).
QString buildLinkBody(QString const &url);

/// Decode an L/V1 body. Returns false unless the payload decodes to
/// a well-formed http(s) URL (scheme enforced — a corrupt or hostile
/// payload must not produce a clickable link to who-knows-what).
bool splitLinkBody(QString const &body, QString &outUrl);

/**
 * @brief Per-file header carried in chunk 1 of a file-transfer
 *        super-message. Serialized to JSON, then base32-encoded for
 *        wire safety.
 */
struct FileHeader {
    QString name;        // basename only (no path components)
    qint64  bytes{0};    // size of the original file in bytes
    QString sha256;      // SHA-256 of the original file, base32-encoded
};

/**
 * @brief RFC 4648 base32 encode. Alphabet A-Z 2-7. No padding.
 *        Every output character is already in JS8's freetext set.
 */
QString base32Encode(QByteArray const &bytes);

/**
 * @brief RFC 4648 base32 decode. Alphabet A-Z 2-7, case-insensitive.
 *        Padding (=) characters are tolerated and ignored. Any
 *        non-alphabet character returns an empty QByteArray (decode
 *        failure).
 */
QByteArray base32Decode(QString const &text);

/// Serialize FileHeader → JSON.
QString headerToJson(FileHeader const &h);

/// Parse JSON → FileHeader. Returns false on malformed input.
bool headerFromJson(QString const &json, FileHeader &out);

/**
 * @brief Read a file from disk, compute its SHA-256, fill FileHeader
 *        with name/bytes/sha256, and return the super-message body
 *        ready to hand to ChunkedArq::sendChunked().
 *
 *        Body layout:
 *          "F/V1 " + base32(JSON(header)) + " " + base32(file bytes)
 *
 *        Returns empty QString on error (file unreadable, too big).
 *        On success, populates `outHeader` for status-bar display.
 */
QString buildSendBody(QString const &filePath, FileHeader &outHeader);

/**
 * @brief V2 counterpart of buildSendBody. Body layout:
 *          "F/V2 GZIP/BASE32 " + base32(qCompress(binaryHeader ‖ fileBytes))
 *        outHeader.sha256 carries base32(sha256[:16]) for display.
 */
QString buildSendBodyV2(QString const &filePath, FileHeader &outHeader);

/**
 * @brief Given a fully-assembled super-message body that started with
 *        the PREFIX_V1 marker, decode the base32 payload, verify
 *        SHA-256 against the header, and write to disk under saveDir
 *        with collision-safe naming.
 *
 *        Returns the saved-file path on success, or an empty QString
 *        on failure (decode error, SHA mismatch, write error). The
 *        outErr param (if non-null) gets a human-readable reason.
 */
QString assembleReceivedFile(QString const &saveDir,
                             FileHeader const &header,
                             QString const &base32Payload,
                             QString *outErr = nullptr);

/**
 * @brief Sanitize an untrusted filename for safe write under saveDir.
 *        Strips directory components, rejects "..", absolute paths,
 *        path separators after strip, and dotfiles. Empty result
 *        falls back to "received_<utc-timestamp>.bin".
 */
QString sanitizeFilename(QString const &untrusted);

/**
 * @brief Split a super-message body that starts with PREFIX_V1 into
 *        the header-base32 chunk and the payload-base32 chunk.
 *        Returns true if both parts decoded; on success `outHeader`
 *        and `outPayloadBase32` are populated.
 */
bool splitWireBody(QString const &body,
                   FileHeader &outHeader,
                   QString &outPayloadBase32);

/**
 * @brief V2 counterpart of splitWireBody: decode + decompress the
 *        single envelope, parse the binary header, verify size and
 *        truncated SHA-256. On success outHeader is populated and
 *        outPayloadBytes holds the VERIFIED raw file bytes (no
 *        further decode step needed before writeReceivedFile).
 */
bool splitWireBodyV2(QString const &body,
                     FileHeader &outHeader,
                     QByteArray &outPayloadBytes);

/**
 * @brief Write verified file bytes to disk under saveDir with
 *        sanitized, collision-safe naming. The write half of
 *        assembleReceivedFile, shared by the V1 path (which decodes
 *        and verifies first) and the V2 path (splitWireBodyV2
 *        already verified). Returns saved path, or empty + outErr.
 */
QString writeReceivedFile(QString const &saveDir,
                          FileHeader const &header,
                          QByteArray const &bytes,
                          QString *outErr = nullptr);

}  // namespace FileTransfer

#endif  // JS8CALL_FILETRANSFER_H
