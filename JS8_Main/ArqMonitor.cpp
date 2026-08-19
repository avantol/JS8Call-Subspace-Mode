/**
 * @file ArqMonitor.cpp
 * @brief See header. Every decode primitive here is a pure reuse of
 * what a real recipient runs — the compliance claim is exactly that
 * equivalence.
 */

#include "ArqMonitor.h"

#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStringList>

#include "JS8_Main/ChunkedArq.h"
#include "JS8_Mode/JS8Submode.h"

Q_LOGGING_CATEGORY(arqmonitor_js8, "js8.arqmonitor")

namespace {
// Frames bind to a session only near its offset — same convention as
// the display rule (rxThreshold at the session's submode).
bool nearOffset(int const freq, int const offset, int const submode) {
    return std::abs(freq - offset) <= JS8::Submode::rxThreshold(submode);
}
} // namespace

ArqMonitor::ArqMonitor(QObject *parent) : QObject{parent} {
    m_evictTimer.setInterval(30000);
    connect(&m_evictTimer, &QTimer::timeout, this,
            &ArqMonitor::evictSweep);
}

void ArqMonitor::setActive(bool const active) {
    if (active == m_active) return;
    m_active = active;
    if (active) {
        m_evictTimer.start();
        qCWarning(arqmonitor_js8) << "[ARQMON] monitoring ON";
    } else {
        m_evictTimer.stop();
        m_sessions.clear();
        qCWarning(arqmonitor_js8)
            << "[ARQMON] monitoring OFF - sessions dropped";
    }
    emit sessionsUpdated();
}

ArqMonitor::Session *ArqMonitor::session(int const id) {
    for (auto &s : m_sessions)
        if (s.id == id) return &s;
    return nullptr;
}

ArqMonitor::Session *ArqMonitor::find(QString const &from,
                                      int const msgId) {
    for (auto &s : m_sessions)
        if (s.msgId == msgId && s.status == Status::Monitoring &&
            s.from.compare(from, Qt::CaseInsensitive) == 0)
            return &s;
    return nullptr;
}

void ArqMonitor::touch(Session &s) {
    s.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
}

void ArqMonitor::onDirectedText(QString const &from, QString const &to,
                                QString const &text, int const offset,
                                int const submode) {
    if (!m_active) return;
    ChunkedArq::ParsedChunk parsed;
    if (!ChunkedArq::parseChunkedData(text, parsed)) return;

    Session *s = find(from, parsed.msgId);
    if (!s) {
        // Session creation: chunk-1 lead marker of the file/link
        // family ONLY (leadmark mirror). Anything else overheard
        // mid-transfer is a transfer we joined late — not ours to
        // guess at.
        if (parsed.chunkId != 1) return;
        QString const &body = parsed.body;
        // TRIMMED prefixes: V2 and L1 senders put the BARE prefix in
        // chunk 1 (payload starts at chunk 2), and wire normalization
        // strips the trailing space the PREFIX_* constants carry —
        // the with-space match missed those transfers by one char
        // (field 2026-08-19: chunk-1 bodyLen 16 = "F/V2 GZIP/BASE32",
        // bodyLen 11 = "L/V1 BASE32", zero sessions created).
        static QString const kV3 = QStringLiteral("F/V3 NATIVE/GZIP");
        static QString const kV2 =
            QString::fromLatin1(FileTransfer::PREFIX_V2).trimmed();
        static QString const kV1 =
            QString::fromLatin1(FileTransfer::PREFIX_V1).trimmed();
        static QString const kL1 =
            QString::fromLatin1(FileTransfer::PREFIX_L1).trimmed();
        Type type;
        if (body.startsWith(kV3))
            type = Type::FileV3;
        else if (body.startsWith(kV2))
            type = Type::FileV2;
        else if (body.startsWith(kV1))
            type = Type::FileV1;
        else if (body.startsWith(kL1))
            type = Type::Link;
        else
            return; // MSG/relay/plain — already visible elsewhere

        Session ns;
        ns.id = m_nextId++;
        ns.from = from;
        ns.to = to;
        ns.type = type;
        ns.msgId = parsed.msgId;
        ns.total = parsed.total;
        ns.offset = offset;
        ns.submode = submode;
        ns.started = QDateTime::currentDateTimeUtc();
        ns.peerHash = NativeBinary::peerHash16(from);
        if (type == Type::FileV3) {
            NativeBinary::MarkerInfo mi;
            if (NativeBinary::parseMarkerBody(body, &mi) &&
                mi.isFirstChunkForm) {
                ns.totalBytes = mi.totalBytes;
                ns.chunkBytes = mi.chunkBytes;
            }
        }
        m_sessions.append(ns);
        s = &m_sessions.last();
        qCWarning(arqmonitor_js8)
            << "[ARQMON] session" << s->id << "created:" << from
            << "->" << to << "msgId=" << parsed.msgId
            << "total=" << parsed.total << "type=" << int(type);
        if (type == Type::FileV3 && s->totalBytes > 0)
            openWindow(*s, 1, /*pcrc=*/0, /*pcrcValid=*/false);
    }

    touch(*s);
    if (s->type == Type::FileV3) {
        // Periodic/lead TEXT markers refresh; data rides frames.
        // A marker for an uncollected chunk resyncs the window.
        if (!s->binChunks.contains(parsed.chunkId) &&
            (!s->winOpen || s->winChunkId != parsed.chunkId))
            openWindow(*s, parsed.chunkId, 0, false);
    } else {
        s->textChunks[parsed.chunkId] = parsed.body; // dup = overwrite
        qCDebug(arqmonitor_js8)
            << "[ARQMON] text chunk" << parsed.chunkId << "/"
            << s->total << "session" << s->id;
        if (s->textChunks.size() >= s->total) {
            bool all = true;
            for (int cc = 1; cc <= s->total; ++cc)
                if (!s->textChunks.contains(cc)) { all = false; break; }
            if (all) finalizeText(*s);
        }
    }
    emit sessionsUpdated();
}

void ArqMonitor::onMarkerFrame(NativeBinary::MarkerFrame const &mf,
                               int const freq) {
    if (!m_active) return;
    for (auto &s : m_sessions) {
        if (s.status != Status::Monitoring || s.type != Type::FileV3 ||
            s.peerHash != mf.peerHash || s.msgId != mf.msgId ||
            !nearOffset(freq, s.offset, s.submode))
            continue;
        touch(s);
        if (s.totalBytes <= 0) { // geometry from the binary form
            s.totalBytes = mf.totalBytes;
            s.chunkBytes = mf.chunkBytes;
            s.total = (mf.totalBytes + mf.chunkBytes - 1) /
                      mf.chunkBytes;
        }
        if (!s.binChunks.contains(mf.chunkId) &&
            (!s.winOpen || s.winChunkId != mf.chunkId))
            openWindow(s, mf.chunkId, mf.pcrc, true);
        emit sessionsUpdated();
        return;
    }
}

void ArqMonitor::onDataFrame(int const seq, int const chk4,
                             QByteArray const &p8, int const freq) {
    if (!m_active) return;
    for (auto &s : m_sessions) {
        if (s.status != Status::Monitoring || s.type != Type::FileV3 ||
            !s.winOpen || !nearOffset(freq, s.offset, s.submode))
            continue;
        if (chk4 != (s.winChunkId & 0xF)) continue;
        if (!s.coll.accept(seq, chk4, p8)) continue;
        touch(s);
        if (s.coll.complete()) chunkDone(s);
        emit sessionsUpdated();
        return;
    }
}

void ArqMonitor::openWindow(Session &s, int const chunkId,
                            quint16 const pcrc, bool const pcrcValid) {
    int const nBytes = NativeBinary::chunkBytesFor(
        chunkId, s.total, s.totalBytes, s.chunkBytes);
    if (nBytes <= 0) {
        s.winOpen = false;
        return;
    }
    s.coll.open(chunkId, nBytes, pcrc, pcrcValid);
    s.winChunkId = chunkId;
    s.winOpen = true;
    qCDebug(arqmonitor_js8)
        << "[ARQMON] window open session" << s.id << "chunk="
        << chunkId << "/" << s.total << "nBytes=" << nBytes;
}

void ArqMonitor::chunkDone(Session &s) {
    if (s.coll.nBytes() > 0 && !s.coll.crcOk())
        s.suspect.insert(s.winChunkId); // kept anyway — SHA decides
    s.binChunks[s.winChunkId] = s.coll.bytes();
    qCWarning(arqmonitor_js8)
        << "[ARQMON] chunk collected session" << s.id << "chunk="
        << s.winChunkId << "/" << s.total
        << "suspect=" << s.suspect.contains(s.winChunkId);
    s.winOpen = false;
    if (s.winChunkId >= s.total) {
        finalizeV3(s);
        return;
    }
    // AUTO-ADVANCE, like the real path — markerless chunks bind here.
    openWindow(s, s.winChunkId + 1, 0, false);
}

void ArqMonitor::finalizeV3(Session &s) {
    QStringList missing;
    QByteArray envelope;
    for (int cc = 1; cc <= s.total; ++cc) {
        if (!s.binChunks.contains(cc))
            missing << QString::number(cc);
        else
            envelope += s.binChunks.value(cc);
    }
    if (!missing.isEmpty()) {
        s.status = Status::Incomplete;
        s.detail = QStringLiteral("missing chunk(s) %1")
                       .arg(missing.join(QStringLiteral(",")));
    } else if (FileTransfer::splitWireBodyV3(envelope, s.header,
                                             s.fileBytes)) {
        s.status = Status::Complete; // gunzip + size + SHA-16 verified
    } else {
        s.status = Status::Failed;
        s.detail = QStringLiteral("envelope verify failed");
    }
    s.binChunks.clear();
    qCWarning(arqmonitor_js8)
        << "[ARQMON] session" << s.id << "final:" << int(s.status)
        << s.detail << "name=" << s.header.name
        << "bytes=" << s.fileBytes.size();
    emit sessionsUpdated();
}

void ArqMonitor::finalizeText(Session &s) {
    QStringList parts;
    for (int cc = 1; cc <= s.total; ++cc)
        parts << s.textChunks.value(cc);
    // Same join the real assembler uses (single space).
    QString const joined = parts.join(QLatin1Char(' '));
    switch (s.type) {
    case Type::FileV1: {
        if (FileTransfer::splitWireBody(joined, s.header,
                                        s.payloadBase32)) {
            // Preview decode; the canonical verify+write happens at
            // Save via assembleReceivedFile (identical to a real
            // recipient).
            QByteArray const gz =
                FileTransfer::base32Decode(s.payloadBase32);
            s.fileBytes = qUncompress(gz);
            s.status = Status::Complete;
        } else {
            s.status = Status::Failed;
            s.detail = QStringLiteral("V1 body parse failed");
        }
        break;
    }
    case Type::FileV2:
        if (FileTransfer::splitWireBodyV2(joined, s.header,
                                          s.fileBytes))
            s.status = Status::Complete; // envelope SHA verified
        else {
            s.status = Status::Failed;
            s.detail = QStringLiteral("V2 envelope verify failed");
        }
        break;
    case Type::Link:
        if (FileTransfer::splitLinkBody(joined, s.linkUrl))
            s.status = Status::Complete;
        else {
            s.status = Status::Failed;
            s.detail = QStringLiteral("link body parse failed");
        }
        break;
    default: break;
    }
    s.textChunks.clear();
    qCWarning(arqmonitor_js8)
        << "[ARQMON] session" << s.id << "final:" << int(s.status)
        << s.detail << "name=" << s.header.name
        << "link=" << s.linkUrl;
    emit sessionsUpdated();
}

void ArqMonitor::evictSweep() {
    qint64 const now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (auto &s : m_sessions) {
        if (s.status != Status::Monitoring) continue;
        if (now - s.lastActivityMs <
            ChunkedArq::ASSEMBLY_EVICT_TIMEOUT_MS)
            continue;
        QStringList missing;
        for (int cc = 1; cc <= s.total; ++cc) {
            bool const have = s.type == Type::FileV3
                                  ? s.binChunks.contains(cc)
                                  : s.textChunks.contains(cc);
            if (!have) missing << QString::number(cc);
        }
        s.status = Status::Incomplete;
        s.detail =
            QStringLiteral("timed out; missing chunk(s) %1")
                .arg(missing.isEmpty() ? QStringLiteral("?")
                                       : missing.join(
                                             QStringLiteral(",")));
        s.binChunks.clear();
        s.textChunks.clear();
        s.winOpen = false;
        changed = true;
        qCWarning(arqmonitor_js8)
            << "[ARQMON] session" << s.id << "evicted:" << s.detail;
    }
    if (changed) emit sessionsUpdated();
}
