#ifndef ARQ_MONITOR_HPP__
#define ARQ_MONITOR_HPP__

/**
 * @file ArqMonitor.h
 * @brief [TODO #153] Passive bystander assembler for OVERHEARD ARQ
 * file/link transfers between two OTHER stations.
 *
 * Purpose: demonstrate regulatory compliance — our transfers are
 * compressed, not encrypted, and any third party on-frequency CAN
 * decode them. This class proves it by assembling a transfer from the
 * sender's frames alone: decode-only, no ACK/NACK, no keying,
 * invisible to the session peers.
 *
 * ISOLATION (the #149/#143(b)/leadmark family lesson): this class is
 * fully separate from the real ARQ machinery. It holds no reference
 * to ChunkedArq::Manager, cannot reach any TX signal, and never
 * touches RxState/m_recv. It is fed by two additive tee points
 * (processCommandActivity third-party swallow; processDecodeEvent
 * after the Manager calls) and reuses only PURE primitives:
 * ChunkedArq::parseChunkedData, NativeBinary::ChunkCollector /
 * chunkBytesFor / parseMarkerBody, FileTransfer::splitWireBody{,V2,V3}
 * / splitLinkBody.
 *
 * Rules (operator decisions 2026-08-19):
 *  - Session created by EXACTLY one thing: an overheard chunk-1 TEXT
 *    lead marker of the file/link family addressed to a third party
 *    (mirror of the Build 358 leadmark rule). Binary markers alone
 *    never create sessions.
 *  - Listener assumed on-frequency from the start; missed sub-msgs
 *    are acceptable — incomplete transfers are marked, never chased.
 *  - Active ONLY while the ARQ Monitor window is open (setActive);
 *    deactivation drops all sessions.
 *  - Plain-text chunked ARQ (MSG/relay) is excluded — already visible
 *    in the normal display paths.
 */

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include "JS8_Main/FileTransfer.h"
#include "JS8_Main/NativeBinary.h"

class ArqMonitor final : public QObject {
    Q_OBJECT

  public:
    enum class Type { FileV1, FileV2, FileV3, Link };
    enum class Status { Monitoring, Complete, Incomplete, Failed };

    struct Session {
        int id{};
        QString from, to;
        Type type{Type::FileV3};
        Status status{Status::Monitoring};
        int msgId{}, total{}, offset{}, submode{};
        QDateTime started;
        qint64 lastActivityMs{};
        // Text transports (V1/V2/L1): chunk bodies by chunkId.
        QHash<int, QString> textChunks;
        QString payloadBase32; // V1: canonical save payload
        // V3 native: own window, never the Manager's.
        int totalBytes{};
        int chunkBytes{NativeBinary::DEFAULT_CHUNK_BYTES};
        quint16 peerHash{};
        QHash<int, QByteArray> binChunks;
        NativeBinary::ChunkCollector coll;
        int winChunkId{};
        bool winOpen{false};
        QSet<int> suspect; // PCRC mismatch, kept anyway (no NACK)
        // Result.
        FileTransfer::FileHeader header;
        QByteArray fileBytes;
        QString linkUrl;
        QString detail;
        bool saved{false};
        QString savedPath;
    };

    explicit ArqMonitor(QObject *parent = nullptr);

    // Window visibility IS the switch: false drops every session.
    void setActive(bool active);
    bool active() const { return m_active; }

    QList<Session> const &sessions() const { return m_sessions; }
    Session *session(int id);

    // Tee inputs. All no-ops while inactive.
    void onDirectedText(QString const &from, QString const &to,
                        QString const &text, int offset, int submode);
    void onMarkerFrame(NativeBinary::MarkerFrame const &mf, int freq);
    void onDataFrame(int seq, int chk4, QByteArray const &p8, int freq);

  signals:
    void sessionsUpdated();

  private:
    Session *find(QString const &from, int msgId);
    void openWindow(Session &s, int chunkId, quint16 pcrc,
                    bool pcrcValid);
    void chunkDone(Session &s);
    void finalizeV3(Session &s);
    void finalizeText(Session &s);
    void evictSweep();
    void touch(Session &s);

    bool m_active{false};
    int m_nextId{1};
    QList<Session> m_sessions;
    QTimer m_evictTimer;
};

#endif
