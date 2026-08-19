// [TODO #153] Offline harness for the passive ARQ monitor: proves a
// bystander assembles an overheard transfer from the sender's frames
// alone. Build: ./build.sh && ./arqmon_test
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include "JS8_Main/ArqMonitor.h"
#include "JS8_Main/ChunkedArq.h"
#include "JS8_Main/FileTransfer.h"
#include "JS8_Main/NativeBinary.h"

static int g_fail = 0;
#define CHECK(cond, what)                                              \
    do {                                                               \
        if (cond) {                                                    \
            std::printf("PASS %s\n", what);                            \
        } else {                                                       \
            std::printf("FAIL %s\n", what);                            \
            ++g_fail;                                                  \
        }                                                              \
    } while (0)

namespace {
constexpr int kOffset = 1500;
constexpr int kSubmode = 16;

QByteArray makeEnvelope(QString const &dir, QByteArray const &content,
                        FileTransfer::FileHeader &header) {
    QString const path = dir + QStringLiteral("/mon_test.txt");
    QFile f{path};
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(content);
    f.close();
    return FileTransfer::buildSendBodyV3(path, header);
}

// Feed one V3 chunk's data frames (seq'd 8-byte slices), as the
// decoder would deliver them.
void feedChunkFrames(ArqMonitor &mon, int const chunkId,
                     QByteArray const &chunkBytes) {
    for (int off = 0, seq = 0; off < chunkBytes.size();
         off += 8, ++seq) {
        mon.onDataFrame(seq, chunkId & 0xF, chunkBytes.mid(off, 8),
                        kOffset);
    }
}

void feedLeadMarker(ArqMonitor &mon, int const msgId,
                    QByteArray const &envelope, int const total) {
    quint16 const pcrc =
        NativeBinary::payloadCrc16(envelope.left(64));
    QString const body = NativeBinary::composeMarkerBody(
        /*firstChunk=*/true, envelope.size(), pcrc, 64);
    QString const text = ChunkedArq::encodeChunkedData(
        QStringLiteral("K9AVT"), QStringLiteral("WM8Q"), body, msgId,
        1, total);
    mon.onDirectedText(QStringLiteral("K9AVT"), QStringLiteral("WM8Q"),
                       text, kOffset, kSubmode);
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app{argc, argv};
    QTemporaryDir tmp;

    QByteArray content;
    for (int i = 0; i < 300; ++i)
        content += "The quick brown fox #" + QByteArray::number(i) + "\n";

    // ---- 1. V3 happy path -------------------------------------------
    {
        ArqMonitor mon;
        mon.setActive(true);
        FileTransfer::FileHeader hdr;
        QByteArray const env = makeEnvelope(tmp.path(), content, hdr);
        int const total = (env.size() + 63) / 64;
        feedLeadMarker(mon, 7, env, total);
        CHECK(mon.sessions().size() == 1, "v3: session created");
        for (int cc = 1; cc <= total; ++cc)
            feedChunkFrames(mon, cc, env.mid((cc - 1) * 64, 64));
        auto const &s = mon.sessions().first();
        CHECK(s.status == ArqMonitor::Status::Complete, "v3: complete");
        CHECK(s.header.name == hdr.name, "v3: header name matches");
        CHECK(s.fileBytes == content, "v3: bytes identical to original");
    }

    // ---- 2. V3 dropped chunk + marker resync → Incomplete -----------
    {
        ArqMonitor mon;
        mon.setActive(true);
        FileTransfer::FileHeader hdr;
        QByteArray const env = makeEnvelope(tmp.path(), content, hdr);
        int const total = (env.size() + 63) / 64;
        feedLeadMarker(mon, 8, env, total);
        for (int cc = 1; cc <= total; ++cc) {
            if (cc == 2) continue; // bystander missed this one
            if (cc == 3) {
                // sender's periodic/retry binary marker resyncs us
                NativeBinary::MarkerFrame mf;
                mf.msgId = 8;
                mf.chunkId = 3;
                mf.totalBytes = env.size();
                mf.chunkBytes = 64;
                mf.pcrc =
                    NativeBinary::payloadCrc16(env.mid(2 * 64, 64));
                mf.peerHash =
                    NativeBinary::peerHash16(QStringLiteral("K9AVT"));
                mon.onMarkerFrame(mf, kOffset);
            }
            feedChunkFrames(mon, cc, env.mid((cc - 1) * 64, 64));
        }
        auto const &s = mon.sessions().first();
        CHECK(s.status == ArqMonitor::Status::Incomplete,
              "v3 drop: incomplete");
        CHECK(s.detail.contains(QStringLiteral("2")),
              "v3 drop: missing chunk named");
    }

    // ---- 3. V3 duplicate retry burst is benign ----------------------
    {
        ArqMonitor mon;
        mon.setActive(true);
        FileTransfer::FileHeader hdr;
        QByteArray const env = makeEnvelope(tmp.path(), content, hdr);
        int const total = (env.size() + 63) / 64;
        feedLeadMarker(mon, 9, env, total);
        for (int cc = 1; cc <= total; ++cc) {
            feedChunkFrames(mon, cc, env.mid((cc - 1) * 64, 64));
            feedChunkFrames(mon, cc, env.mid((cc - 1) * 64, 64)); // dup
        }
        auto const &s = mon.sessions().first();
        CHECK(s.status == ArqMonitor::Status::Complete,
              "v3 dup: still completes");
        CHECK(s.fileBytes == content, "v3 dup: bytes identical");
    }

    // ---- 4. V2 text transport (single chunk) ------------------------
    {
        ArqMonitor mon;
        mon.setActive(true);
        FileTransfer::FileHeader hdr;
        QString const path = tmp.path() + QStringLiteral("/v2.txt");
        {
            QFile f{path};
            f.open(QIODevice::WriteOnly);
            f.write(content);
        }
        QString const body = FileTransfer::buildSendBodyV2(path, hdr);
        QString const text = ChunkedArq::encodeChunkedData(
            QStringLiteral("K9AVT"), QStringLiteral("WM8Q"), body, 10,
            1, 1);
        mon.onDirectedText(QStringLiteral("K9AVT"),
                           QStringLiteral("WM8Q"), text, kOffset,
                           kSubmode);
        CHECK(mon.sessions().size() == 1, "v2: session created");
        auto const &s = mon.sessions().first();
        CHECK(s.status == ArqMonitor::Status::Complete, "v2: complete");
        CHECK(s.fileBytes == content, "v2: bytes identical");
    }

    // ---- 4b. V2 as the REAL sender chunks it: chunk 1 = the BARE
    // prefix (no trailing space after wire normalization), payload
    // from chunk 2 — the exact field shape that exposed the
    // trailing-space classification miss (2026-08-19).
    {
        ArqMonitor mon;
        mon.setActive(true);
        FileTransfer::FileHeader hdr;
        QString const path = tmp.path() + QStringLiteral("/v2b.txt");
        {
            QFile f{path};
            f.open(QIODevice::WriteOnly);
            f.write(content);
        }
        QString const body = FileTransfer::buildSendBodyV2(path, hdr);
        QString const prefix =
            QString::fromLatin1(FileTransfer::PREFIX_V2).trimmed();
        QString const payload = body.mid(prefix.size()).trimmed();
        int const total = 3;
        QStringList chunks{prefix,
                           payload.left(payload.size() / 2),
                           payload.mid(payload.size() / 2)};
        for (int cc = 1; cc <= total; ++cc) {
            QString const text = ChunkedArq::encodeChunkedData(
                QStringLiteral("K9AVT"), QStringLiteral("WM8Q"),
                chunks[cc - 1], 20, cc, total);
            mon.onDirectedText(QStringLiteral("K9AVT"),
                               QStringLiteral("WM8Q"), text, kOffset,
                               kSubmode);
        }
        CHECK(mon.sessions().size() == 1, "v2 bare-prefix: session");
        auto const &s = mon.sessions().first();
        CHECK(s.status == ArqMonitor::Status::Complete,
              "v2 bare-prefix: complete");
        CHECK(s.fileBytes == content, "v2 bare-prefix: bytes match");
    }

    // ---- 4c. Link with bare "L/V1 BASE32" chunk 1 -------------------
    {
        ArqMonitor mon;
        mon.setActive(true);
        QString const url = QStringLiteral("https://groups.io/g/x/1");
        QString const linkBody = FileTransfer::buildLinkBody(url);
        QString const prefix =
            QString::fromLatin1(FileTransfer::PREFIX_L1).trimmed();
        QString const payload = linkBody.mid(prefix.size()).trimmed();
        QStringList const chunks{prefix, payload};
        for (int cc = 1; cc <= 2; ++cc) {
            QString const text = ChunkedArq::encodeChunkedData(
                QStringLiteral("K9AVT"), QStringLiteral("WM8Q"),
                chunks[cc - 1], 21, cc, 2);
            mon.onDirectedText(QStringLiteral("K9AVT"),
                               QStringLiteral("WM8Q"), text, kOffset,
                               kSubmode);
        }
        CHECK(mon.sessions().size() == 1, "link: session");
        auto const &s = mon.sessions().first();
        CHECK(s.status == ArqMonitor::Status::Complete,
              "link: complete");
        CHECK(s.linkUrl.compare(url, Qt::CaseInsensitive) == 0,
              "link: url recovered");
    }

    // ---- 5. MSG text is excluded, inactive monitor is inert ---------
    {
        ArqMonitor mon;
        mon.setActive(true);
        QString const text = ChunkedArq::encodeChunkedData(
            QStringLiteral("K9AVT"), QStringLiteral("WM8Q"),
            QStringLiteral("MSG TO:WM8Q HELLO THERE"), 11, 1, 2);
        mon.onDirectedText(QStringLiteral("K9AVT"),
                           QStringLiteral("WM8Q"), text, kOffset,
                           kSubmode);
        CHECK(mon.sessions().isEmpty(), "msg: excluded");
        mon.setActive(false);
        FileTransfer::FileHeader hdr;
        QByteArray const env = makeEnvelope(tmp.path(), content, hdr);
        feedLeadMarker(mon, 12, env, (env.size() + 63) / 64);
        CHECK(mon.sessions().isEmpty(), "inactive: inert");
    }

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
