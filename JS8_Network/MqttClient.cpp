/**
 * @file MqttClient.cpp
 * @brief Minimal MQTT 3.1.1 subscriber over QTcpSocket. See header.
 */

#include "MqttClient.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QTcpSocket>

Q_LOGGING_CATEGORY(mqttclient_js8, "mqttclient.js8", QtWarningMsg)

MqttClient::MqttClient(QObject *parent) : QObject{parent} {
    m_pingTimer.setInterval(PING_INTERVAL_MS);
    connect(&m_pingTimer, &QTimer::timeout,
            this, &MqttClient::onKeepaliveTick);

    m_backoffTimer.setSingleShot(true);
    connect(&m_backoffTimer, &QTimer::timeout,
            this, &MqttClient::onBackoffExpired);
}

void MqttClient::setServer(QString const &host, quint16 const port) {
    m_host = host;
    m_port = port;
}

void MqttClient::setClientIdPrefix(QString const &prefix) {
    // MQTT client IDs are restricted in some brokers; keep it tame.
    QString clean;
    for (QChar const c : prefix)
        if (c.isLetterOrNumber() || c == QLatin1Char('_'))
            clean.append(c);
    if (!clean.isEmpty())
        m_clientIdPrefix = clean;
}

void MqttClient::setTopics(QStringList const &topicFilters) {
    m_topics = topicFilters;
    // If we're already up, resubscribe with the new filters via a
    // clean reconnect — simplest correct behavior for a rare event
    // (callsign change).
    if (m_started && m_state != State::Idle) {
        qCWarning(mqttclient_js8) << "[MQTT] topics changed; reconnecting";
        scheduleReconnect(QStringLiteral("topics changed"));
    }
}

void MqttClient::start() {
    // [TODO #159 2026-08-18] No-internet TESTING switch, scoped by
    // operator decision to MQTT only: JS8_NO_MQTT=1 simulates the
    // broker being unreachable. start() is the ONE entry to any
    // connection attempt (beginConnect/reconnect all sit behind
    // m_started), so gating here means no MQTT data enters the app
    // anywhere - exactly a true offline session. Deliberately NOT
    // JS8_DEBUG: that switch gates debug VISUALS, and debugging
    // visuals must not silently dry up map data (one fact, one
    // variable).
    static bool const noMqtt =
        qEnvironmentVariableIsSet("JS8_NO_MQTT");
    if (noMqtt) {
        qCWarning(mqttclient_js8)
            << "[NET] MQTT DISABLED (JS8_NO_MQTT test switch) - "
               "simulating broker unavailable";
        setState(State::Idle, QStringLiteral("off (JS8_NO_MQTT)"));
        return;
    }
    m_started = true;
    if (m_state == State::Idle)
        beginConnect();
}

void MqttClient::stop() {
    m_started = false;
    m_backoffTimer.stop();
    m_pingTimer.stop();
    if (m_socket && m_state == State::Up) {
        // Clean disconnect: DISCONNECT packet then close.
        sendPacket(DISCONNECT, {});
        m_socket->flush();
    }
    teardownSocket();
    setState(State::Idle, QStringLiteral("stopped"));
}

void MqttClient::setState(State const state, QString const &detail) {
    m_state = state;
    QString text;
    switch (state) {
    case State::Idle:
        text = QStringLiteral("offline");
        break;
    case State::TcpConnecting:
        text = QStringLiteral("connecting to %1…").arg(m_host);
        break;
    case State::MqttConnecting:
        text = QStringLiteral("handshaking with %1…").arg(m_host);
        break;
    case State::Subscribing:
        text = QStringLiteral("subscribing…");
        break;
    case State::Up:
        text = QStringLiteral("connected to %1").arg(m_host);
        break;
    case State::BackoffWait:
        text = QStringLiteral("reconnecting to %1 in %2 s")
                   .arg(m_host)
                   .arg((m_backoffMs + 999) / 1000);
        break;
    }
    if (!detail.isEmpty())
        text += QStringLiteral(" (%1)").arg(detail);
    // [mqttstate 2026-08-24] WARNING, not debug. On 2026-08-23 the
    // internet feed died at 21:35:43 and the app ran on for another two
    // and a half hours with radio decodes working normally and the PSKR
    // layer quietly dead. Nineteen hours of diag log contained NOT ONE
    // line from this client -- not even its connect at startup -- so
    // there was no way to tell a dead feed from a quiet band, either in
    // the log or on the map. These transitions are rare by nature
    // (connect, drop, backoff, keepalive timeout), so they cost nothing
    // to record and they are the only evidence that the feed is alive.
    qCWarning(mqttclient_js8) << "[MQTT] state:" << text;
    Q_EMIT stateChanged(text);
}

void MqttClient::teardownSocket() {
    if (!m_socket)
        return;
    m_socket->disconnect(this);
    m_socket->abort();
    m_socket->deleteLater();
    m_socket = nullptr;
    m_buffer.clear();
}

void MqttClient::beginConnect() {
    teardownSocket();

    if (m_host.isEmpty() || m_topics.isEmpty()) {
        setState(State::Idle, QStringLiteral("not configured"));
        return;
    }

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected,
            this, &MqttClient::onConnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &MqttClient::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred,
            this, &MqttClient::onErrorOccurred);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &MqttClient::onDisconnected);

    setState(State::TcpConnecting);
    m_socket->connectToHost(m_host, m_port);
}

void MqttClient::scheduleReconnect(QString const &reason) {
    m_pingTimer.stop();
    teardownSocket();
    if (!m_started) {
        setState(State::Idle);
        return;
    }
    setState(State::BackoffWait, reason);
    m_backoffTimer.start(m_backoffMs);
    m_backoffMs = qMin(m_backoffMs * 2, BACKOFF_MAX_MS);
}

void MqttClient::onBackoffExpired() {
    if (m_started)
        beginConnect();
}

void MqttClient::onConnected() {
    setState(State::MqttConnecting);
    m_lastInboundMs = QDateTime::currentMSecsSinceEpoch();
    sendConnectPacket();
}

void MqttClient::onErrorOccurred(QAbstractSocket::SocketError) {
    if (m_state == State::Idle)
        return;
    QString const err = m_socket ? m_socket->errorString()
                                 : QStringLiteral("socket error");
    qCDebug(mqttclient_js8) << "socket error:" << err;
    scheduleReconnect(err);
}

void MqttClient::onDisconnected() {
    if (m_state == State::Idle)
        return;
    scheduleReconnect(QStringLiteral("connection closed"));
}

void MqttClient::onKeepaliveTick() {
    if (m_state != State::Up || !m_socket)
        return;
    // [mqttheartbeat 2026-08-24] Say how many spots actually arrived.
    // A socket that stays up while delivering nothing produces no state
    // change, so without this a dead feed reads exactly like a quiet
    // band -- which is what happened on 2026-08-23: spots stopped at
    // 21:35:43 and nothing in nineteen hours of log said so. Once a
    // minute, so it is cheap, and a run of zeroes is the symptom.
    qint64 const nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastHeartbeatMs >= 60000) {
        if (m_lastHeartbeatMs)
            qCWarning(mqttclient_js8).nospace()
                << "[MQTT] " << m_pubCount << " spots in the last "
                << ((nowMs - m_lastHeartbeatMs) / 1000) << "s";
        m_lastHeartbeatMs = nowMs;
        m_pubCount = 0;
    }
    // Dead-connection detection: any inbound traffic counts as life.
    if (QDateTime::currentMSecsSinceEpoch() - m_lastInboundMs
        > DEAD_AFTER_MS) {
        qCWarning(mqttclient_js8) << "[MQTT] keepalive timeout";
        scheduleReconnect(QStringLiteral("keepalive timeout"));
        return;
    }
    sendPacket(PINGREQ, {});
}

// -------------------------------------------------------------------------
// Packet encoding
// -------------------------------------------------------------------------

QByteArray MqttClient::encodeString(QString const &s) {
    QByteArray const utf8 = s.toUtf8();
    QByteArray out;
    out.append(static_cast<char>((utf8.size() >> 8) & 0xFF));
    out.append(static_cast<char>(utf8.size() & 0xFF));
    out.append(utf8);
    return out;
}

void MqttClient::sendPacket(quint8 const typeByte, QByteArray const &body) {
    if (!m_socket)
        return;
    QByteArray packet;
    packet.append(static_cast<char>(typeByte));
    // Remaining Length: 7-bit varint, up to 4 bytes.
    int len = body.size();
    do {
        quint8 digit = len % 128;
        len /= 128;
        if (len > 0)
            digit |= 0x80;
        packet.append(static_cast<char>(digit));
    } while (len > 0);
    packet.append(body);
    m_socket->write(packet);
}

void MqttClient::sendConnectPacket() {
    QByteArray body;
    body.append(encodeString(QStringLiteral("MQTT"))); // protocol name
    body.append(static_cast<char>(0x04));              // level 4 = 3.1.1
    body.append(static_cast<char>(0x02));              // clean session
    body.append(static_cast<char>((KEEPALIVE_SECS >> 8) & 0xFF));
    body.append(static_cast<char>(KEEPALIVE_SECS & 0xFF));

    // Unique client ID — duplicate IDs cause broker-side disconnect
    // loops, so suffix with random hex per process.
    QString const clientId =
        QStringLiteral("%1_%2")
            .arg(m_clientIdPrefix)
            .arg(QRandomGenerator::global()->generate(), 8, 16,
                 QLatin1Char('0'));
    body.append(encodeString(clientId));

    sendPacket(CONNECT, body);
}

void MqttClient::sendSubscribePacket() {
    QByteArray body;
    if (++m_packetId == 0)
        ++m_packetId; // packet ID 0 is illegal
    body.append(static_cast<char>((m_packetId >> 8) & 0xFF));
    body.append(static_cast<char>(m_packetId & 0xFF));
    for (QString const &topic : m_topics) {
        body.append(encodeString(topic));
        body.append(static_cast<char>(0x00)); // requested QoS 0
    }
    sendPacket(SUBSCRIBE, body);
}

// -------------------------------------------------------------------------
// Packet decoding
// -------------------------------------------------------------------------

void MqttClient::onReadyRead() {
    if (!m_socket)
        return;
    m_buffer.append(m_socket->readAll());
    m_lastInboundMs = QDateTime::currentMSecsSinceEpoch();

    if (m_buffer.size() > BUFFER_CAP) {
        qCWarning(mqttclient_js8)
            << "receive buffer exceeded cap — protocol desync?";
        scheduleReconnect(QStringLiteral("desync"));
        return;
    }
    processBuffer();
}

void MqttClient::processBuffer() {
    // MQTT packets routinely straddle TCP segment boundaries; loop
    // until the buffer no longer holds a complete packet.
    for (;;) {
        if (m_buffer.size() < 2)
            return;

        // Decode Remaining Length varint (1-4 bytes after the first).
        int  remaining = 0;
        int  multiplier = 1;
        int  idx = 1;
        bool complete = false;
        while (idx < m_buffer.size() && idx <= 4) {
            quint8 const digit = static_cast<quint8>(m_buffer.at(idx));
            remaining += (digit & 0x7F) * multiplier;
            multiplier *= 128;
            ++idx;
            if (!(digit & 0x80)) {
                complete = true;
                break;
            }
        }
        if (!complete) {
            if (idx > 4) {
                qCWarning(mqttclient_js8) << "malformed Remaining Length";
                scheduleReconnect(QStringLiteral("bad length"));
            }
            return; // need more bytes
        }
        if (m_buffer.size() < idx + remaining)
            return; // need more bytes

        quint8 const firstByte = static_cast<quint8>(m_buffer.at(0));
        QByteArray const body = m_buffer.mid(idx, remaining);
        m_buffer.remove(0, idx + remaining);

        handlePacket(firstByte, body);
        if (!m_socket)
            return; // a handler may have torn the connection down
    }
}

void MqttClient::handlePacket(quint8 const firstByte, QByteArray const &body) {
    switch (firstByte & 0xF0) {
    case CONNACK: {
        quint8 const rc = body.size() >= 2
                              ? static_cast<quint8>(body.at(1))
                              : 0xFF;
        if (rc != 0) {
            qCWarning(mqttclient_js8) << "CONNACK refused, rc =" << rc;
            scheduleReconnect(QStringLiteral("refused rc=%1").arg(rc));
            return;
        }
        setState(State::Subscribing);
        sendSubscribePacket();
        break;
    }

    case SUBACK & 0xF0: {
        bool refused = false;
        for (int i = 2; i < body.size(); ++i)
            if (static_cast<quint8>(body.at(i)) == 0x80)
                refused = true;
        if (refused) {
            qCWarning(mqttclient_js8) << "SUBACK refused a topic filter";
            scheduleReconnect(QStringLiteral("subscribe refused"));
            return;
        }
        m_backoffMs = BACKOFF_MIN_MS; // healthy session: reset backoff
        m_pingTimer.start();
        setState(State::Up);
        break;
    }

    case PUBLISH & 0xF0: {
        if (body.size() < 2)
            return;
        int const topicLen = (static_cast<quint8>(body.at(0)) << 8) |
                             static_cast<quint8>(body.at(1));
        int offset = 2 + topicLen;
        if (offset > body.size())
            return;
        QString const topic = QString::fromUtf8(body.mid(2, topicLen));
        // QoS > 0 would insert a 2-byte packet ID here; we subscribe
        // at QoS 0 only, but be tolerant if a broker misbehaves.
        if (quint8 const qos = (firstByte >> 1) & 0x03; qos > 0)
            offset += 2;
        if (offset > body.size())
            return;
        ++m_pubCount;                       // [mqttheartbeat]
        Q_EMIT messageReceived(topic, body.mid(offset));
        break;
    }

    case PINGRESP & 0xF0:
        break; // m_lastInboundMs already refreshed in onReadyRead

    default:
        qCDebug(mqttclient_js8)
            << "ignoring packet type" << Qt::hex << (firstByte & 0xF0);
        break;
    }
}
