#ifndef SPOT_MAP_WINDOW_HPP__
#define SPOT_MAP_WINDOW_HPP__

/**
 * @file SpotMapWindow.h
 * @brief "Spots Map" — live polar heat map of where my signal is spotted.
 *
 * Subscribes to the PSK Reporter MQTT feed (mqtt.pskreporter.info) for
 * spots where MY callsign is the sender, and renders each spotter on an
 * azimuthal chart centered on my grid: position from
 * Geodesic::vector(myGrid, spotterGrid), heat color from the SNR the
 * spotter reported. Rolling 15-minute window, auto-scaling radius,
 * current band displayed with per-band caches retained across band
 * changes.
 *
 * The MQTT client starts with the app (constructor), NOT with the
 * window: history accumulates in the background from launch so opening
 * the map shows the last 15 minutes immediately. The window is purely a
 * viewport; closing it hides it and rendering stops, but the client and
 * caches keep running until app exit.
 */

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QPointF>
#include <QPolygonF>
#include <QVector>
#include <QWidget>

#include <functional>

class Configuration;
class MqttClient;
class QSettings;

class SpotMapWindow final : public QWidget {
    Q_OBJECT

  public:
    SpotMapWindow(QSettings *settings, Configuration const *config,
                  QWidget *parent = nullptr);
    ~SpotMapWindow() override;

    void saveSettings();
    bool wasVisibleAtShutdown() const; // for startup restore
    // Transient toast overlay (bottom-center, auto-hides ~2.5 s) —
    // feedback for click actions, e.g. "Copied K9AVT to outgoing
    // message".
    void showToast(QString const &text);

  public slots:
    void setBand(QString const &band);
    void setStation(QString const &callsign, QString const &grid);
    // [BUILD 340] Dial frequency (Hz) — lets hover show each spot's
    // AUDIO offset (spot RF f − dial) and gates double-click QSY.
    void setDialFrequency(qint64 hz);

  signals:
    void closed();
    // [BUILD 336 TODO #96 first slice] Left-click on a spot dot.
    // Main window decides what to do with it (currently: seed the
    // outgoing text box when it's empty and no call is selected).
    void spotClicked(QString const &callsign);
    // [BUILD 340] Double-click on a spot: QSY to the DX station's
    // audio offset (only emitted when it's above 1000 Hz).
    void qsyToOffset(int audioHz);

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void showEvent(QShowEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void changeEvent(QEvent *) override; // activation → hint toast

  private slots:
    void onMqttMessage(QString const &topic, QByteArray const &payload);
    void onMqttState(QString const &state);
    void onPruneTick();
    void redraw();

  private:
    struct Spot {
        QDateTime when;         // clamped-to-now report time
        QString receiverCall;
        QString receiverGrid;
        QString country;        // spotter's country (empty if ours/unknown)
        int snr = 0;            // dB as reported by the spotter
        qint64 freqHz = 0;      // exact RF Hz the spotter logged us at
        float azimuth = 0.0f;   // degrees true, from my grid
        float distance = 0.0f;  // km or miles per Configuration::miles()
    };

    static constexpr int WINDOW_SECS = 15 * 60;
    static constexpr int SNR_COLD = -25; // dB → blue
    static constexpr int SNR_HOT = 10;   // dB → red

    void rebuildTopics();
    void requestReplot();
    void pruneBand(QString const &band);
    void rebuildMapCache(QPointF const &center, float R, float scaleKm);
    static float niceCeil(float value); // next 1/2/5 x 10^n
    static QColor snrColor(int snr, float alphaScale);

    QSettings *m_settings;
    Configuration const *m_config;
    MqttClient *m_mqtt;
    qint64 m_dialHz = 0;
    // Country-name lookup by callsign (LogBook/cty.dat, injected by
    // the main window — this widget has no logbook dependency).
    std::function<QString(QString const &)> m_countryLookup;

  public:
    void setCountryLookup(std::function<QString(QString const &)> fn) {
        m_countryLookup = std::move(fn);
    }

  private:

    QHash<QString, QVector<Spot>> m_spotsByBand;
    QString m_currentBand;
    QString m_myCall;
    QString m_myGrid;
    QString m_stateText;
    int m_skippedSpots = 0;
    int m_debugDumpsLeft = 0; // first-N payload dumps after connect

    QPixmap m_pixmap;
    QTimer m_replotTimer; // debounce
    QTimer m_pruneTimer;
    bool m_restoreVisible = false;

    // Toast overlay (lazy-created by showToast()).
    class QLabel *m_toast = nullptr;
    QTimer m_toastTimer;

    // On-map display options (defaults per operator, 2026-07-14;
    // on-map toggle UI to follow — details TBD).
    bool m_showRings = false;
    bool m_showCallsigns = false;

    // Projected world-outline cache (azimuthal equidistant around my
    // grid, clipped to the current scale) — rebuilt only when the
    // center/scale/geometry changes.
    QVector<QPolygonF> m_mapCache;
    QString m_mapCacheGrid;
    float m_mapCacheScale = -1.0f;
    float m_mapCacheR = -1.0f;
    QPointF m_mapCacheCenter;

    // Screen positions of the current band's spots as of the last
    // redraw, for hover lookup.
    struct ScreenSpot {
        QPointF pos;
        Spot spot;
    };
    QVector<ScreenSpot> m_screenSpots;
    // Nearest spot within the hit radius, or nullptr.
    ScreenSpot const *hitTest(QPointF const &pos) const;
};

#endif
