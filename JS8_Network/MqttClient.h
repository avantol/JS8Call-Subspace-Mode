#ifndef MQTT_CLIENT_HPP__
#define MQTT_CLIENT_HPP__

/**
 * @file MqttClient.h
 * @brief Minimal MQTT 3.1.1 subscriber over QTcpSocket.
 *
 * Purpose-built for consuming the PSK Reporter MQTT feed
 * (mqtt.pskreporter.info) without adding an external dependency:
 * Qt6Mqtt is not packaged on any of our target platforms and
 * libmosquitto would burden all four CI builds. This implements the
 * receive-only subset of MQTT 3.1.1: CONNECT/CONNACK, SUBSCRIBE
 * (QoS 0)/SUBACK, inbound PUBLISH, PINGREQ/PINGRESP keepalive, and
 * DISCONNECT — with automatic reconnect and exponential backoff.
 *
 * Generic by design (host/port/topics are caller-supplied), so it can
 * serve other MQTT feeds later.
 */

#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <QTimer>

class QTcpSocket;

class MqttClient final : public QObject {
    Q_OBJECT

  public:
    explicit MqttClient(QObject *parent = nullptr);

    // Configuration; take effect on the next (re)connect.
    void setServer(QString const &host, quint16 port);
    void setClientIdPrefix(QString const &prefix); // suffixed with random hex
    void setTopics(QStringList const &topicFilters);

    bool isUp() const { return m_state == State::Up; }

  public slots:
    void start(); // begin connecting; idempotent
    void stop();  // clean DISCONNECT + close; no reconnect until start()

  signals:
    void messageReceived(QString const &topic, QByteArray const &payload);
    void stateChanged(QString const &humanReadable);

  private slots:
    void onConnected();
    void onReadyRead();
    void onErrorOccurred(QAbstractSocket::SocketError);
    void onDisconnected();
    void onKeepaliveTick();
    void onBackoffExpired();

  private:
    enum class State {
        Idle,           // not started, or stop() called
        TcpConnecting,  // TCP in progress
        MqttConnecting, // CONNECT sent, awaiting CONNACK
        Subscribing,    // SUBSCRIBE sent, awaiting SUBACK
        Up,             // receiving
        BackoffWait     // failed; timer running to retry
    };

    // MQTT 3.1.1 control packet types (high nibble of the first byte).
    static constexpr quint8 CONNECT = 0x10;
    static constexpr quint8 CONNACK = 0x20;
    static constexpr quint8 PUBLISH = 0x30;
    static constexpr quint8 SUBSCRIBE = 0x82; // includes mandatory flags 0b0010
    static constexpr quint8 SUBACK = 0x90;
    static constexpr quint8 PINGREQ = 0xC0;
    static constexpr quint8 PINGRESP = 0xD0;
    static constexpr quint8 DISCONNECT = 0xE0;

    static constexpr int KEEPALIVE_SECS = 60;   // advertised in CONNECT
    static constexpr int PING_INTERVAL_MS = 30 * 1000;
    static constexpr int DEAD_AFTER_MS = 75 * 1000; // no inbound traffic
    static constexpr int BACKOFF_MIN_MS = 5 * 1000;
    static constexpr int BACKOFF_MAX_MS = 300 * 1000;
    static constexpr int BUFFER_CAP = 1 * 1024 * 1024; // desync guard

    void setState(State state, QString const &detail = {});
    void beginConnect();
    void scheduleReconnect(QString const &reason);
    void teardownSocket();

    void sendConnectPacket();
    void sendSubscribePacket();
    void sendPacket(quint8 typeByte, QByteArray const &body);

    void processBuffer();
    void handlePacket(quint8 firstByte, QByteArray const &body);

    static QByteArray encodeString(QString const &s);

    QTcpSocket *m_socket = nullptr;
    QTimer m_pingTimer;
    QTimer m_backoffTimer;

    QString m_host;
    quint16 m_port = 1883;
    QString m_clientIdPrefix = QStringLiteral("JS8Call");
    QStringList m_topics;

    State m_state = State::Idle;
    bool m_started = false; // start() called and not stop()ed
    QByteArray m_buffer;
    int m_backoffMs = BACKOFF_MIN_MS;
    quint16 m_packetId = 0;
    qint64 m_lastInboundMs = 0;
};

#endif
