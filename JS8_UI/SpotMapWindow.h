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
    // [units] Settings changed (distance units etc.) — repaint with
    // the new configuration. Cheap; safe to call on every accept.
    void configRefresh() { requestReplot(); }
    // [hearlines] On-air heard-mesh feed: a station (hearer) was
    // observed hearing one or more others — from a HEARING reply
    // (incl. relayed "... *DE* CALL" form) or from any directed
    // exchange ("A: B ..." = A hears/works B). Grids may be empty
    // when unknown; edges draw only when both ends resolve. Works
    // with no internet — this is the offline complement to the MQTT
    // mesh (blue lines, drawn over the green).
    // [onairspot] A station demonstrably heard MY signal (it sent me
    // a directed frame; an SNR reply even carries their copy's
    // report). The on-air equivalent of a PSK Reporter spot of me —
    // feeds the MY view so the map works identically with no
    // internet. snr -99 = frame carried no report (position-only).
    void addOnAirSpotOfMe(QString const &band, QString const &call,
                          QString const &grid, int snr);
    // heardCalls may be empty = pure PRESENCE (e.g. a heartbeat with
    // its grid): the station gets a hollow dot in the All view.
    // reportedToMeSnr: an SNR value this station REPORTED TO US (its
    // copy of our signal) — the ONLY thing that colors an on-air
    // All-view dot solid (operator rule 2026-08-14); -99 = no change.
    void addHearingReport(QString const &band, QString const &hearer,
                          QString const &hearerGrid,
                          QStringList const &heardCalls,
                          QStringList const &heardGrids,
                          int reportedToMeSnr = -99);

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
    // [relaysel] "Done" pressed in relay-select mode: the composed
    // relay template ("HOP1>HOP2>DEST [MESSAGE]") for the outgoing
    // box; the main window highlights the placeholder for typing.
    // Plain directed text — ARQ is NOT used for any hop (operator
    // directive 2026-08-14).
    void relayTemplateReady(QString const &templateText);

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void showEvent(QShowEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void changeEvent(QEvent *) override; // activation → hint toast

  private slots:
    void onMqttMessage(QString const &topic, QByteArray const &payload);
    void onMqttState(QString const &state);
    void onPruneTick();
    void redraw();
    void zoomIn();
    void zoomOut();
    void zoomAuto();

  private:
    struct Spot {
        QDateTime when;         // clamped-to-now report time
        QString receiverCall;   // the PLOTTED station (spotter in my
                                // view; SENDER in the All view)
        QString receiverGrid;   // plotted station's grid
        QString country;        // spotter's country (empty if ours/unknown)
        // [viewall] Non-empty only for All-view spots: the reporting
        // station that heard the plotted sender (hover detail).
        QString heardBy;
        // [connlines] Reporting station's bearing/distance from my
        // grid (All-view spots only; dist < 0 = unknown) so the
        // Connections overlay can draw sender-to-spotter lines
        // without per-paint geodesic work.
        float heardByAz = 0.0f;
        float heardByDist = -1.0f;
        QString heardByGrid; // reporter's grid (All-view spots)
        // [mondots] monitorOnly = hollow rendering (no valid SNR for
        // this view). rxOnly = genuinely receive-only station (MQTT
        // reporter never heard transmitting) — the ONLY case hover
        // labels "monitor" (heard-by-me senders are not monitors;
        // operator 2026-08-15).
        bool monitorOnly = false;
        bool rxOnly = false;
        bool pskr = false;      // [pskrtoggle] internet-sourced spot
        // [allsuper] Spot's SNR is a report of MY signal (my-view
        // datasets) — exempt from the All-view color override.
        bool reportsMe = false;
        int snr = -99;          // dB as reported by the spotter;
                                // -99 = NO REPORT sentinel — never
                                // default to a claimable real value
        qint64 freqHz = 0;      // exact RF Hz the spotter logged us at
        float azimuth = 0.0f;   // degrees true, from my grid
        float distance = 0.0f;  // km or miles per Configuration::miles()
    };

    // [spotwin] STORAGE horizon: spots are retained in memory per
    // band for a full hour (fleet stations on stock JS8Call re-spot
    // at most hourly per band — the 1-hour reporting cache — so a
    // shorter accumulation misses most of the fleet). The DISPLAY
    // window (15/30/60 buttons, m_viewWindowSecs) filters at paint
    // time; band changes keep each band's accumulated hour.
    static constexpr int WINDOW_SECS = 60 * 60;
    // Default view = the full hour (operator, 2026-08-03: with the
    // fleet's hourly re-spot cache, anything shorter hides most
    // reporting stations by default).
    static constexpr int DEFAULT_VIEW_SECS = 60 * 60;
    static constexpr int SNR_COLD = -25; // dB → blue
    static constexpr int SNR_HOT = 10;   // dB → red

    void rebuildTopics();
    void requestReplot();
    void pruneBand(QString const &band);
    // Outline cache is built CENTER-RELATIVE (origin = chart center)
    // and translated at draw time, so panning never rebuilds it; only
    // grid/R/scale/cut changes do.
    void rebuildMapCache(float R, float scaleKm, float cutKm);
    static float niceCeil(float value); // next 1/2/5 x 10^n
    static float stepScale(float scale, int dir); // ±1 step on the ladder
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

    // Zoom controls (upper-left, vertical: + / Auto / −). Manual scale
    // is SESSION-ONLY state, never persisted — every launch starts in
    // Auto (operator directive 2026-08-02). 0 = auto-scale.
    class QToolButton *m_zoomInBtn = nullptr;
    class QToolButton *m_zoomAutoBtn = nullptr;
    class QToolButton *m_zoomOutBtn = nullptr;
    float m_manualScaleKm = 0.0f;
    float m_lastScaleKm = 0.0f; // effective scale of the last redraw

    // [spotwin] Accumulation-window buttons (15/30/60 min, upper
    // right). Session-only; every open defaults to 60 (no
    // persistence, operator directive 2026-08-03).
    class QToolButton *m_win5Btn = nullptr;
    class QToolButton *m_win15Btn = nullptr;
    class QToolButton *m_win30Btn = nullptr;
    class QToolButton *m_win60Btn = nullptr;
    int m_viewWindowSecs = DEFAULT_VIEW_SECS;
    QDateTime m_accumStart; // [spotfmt] when accumulation (re)started
    void positionWindowButtons();

    // [viewall] View-selector buttons (lower-left, vertical stack:
    // my call over "All"). My view = spots of MY signal (spotters
    // plotted, as always). All view = every JS8 spot on the band
    // (the heard SENDER plotted, hover shows who reported it).
    // Session-only; every open reverts to my view. NOTE: these two
    // must NOT use autoExclusive — that would join the win15/30/60
    // sibling group (autoExclusive groups by parent) — they use a
    // QButtonGroup instead.
    class QToolButton *m_viewMineBtn = nullptr;
    class QToolButton *m_viewAllBtn = nullptr;
    bool m_viewAll = false;
    // [connlines] "Connections" toggle (lower right): draw 1 px light
    // green lines between stations hearing each other. Session-only.
    class QToolButton *m_connBtn = nullptr;
    bool m_showConnections = false;
    // [relaysel] Relay-path builder. "Select relay(s)" toggles click-
    // to-append mode: my station -> first click -> next click -> ...;
    // "Undo" pops the last hop, "Done" emits the template. Session-
    // only; disabling the toggle (or reopening) clears the path.
    class QToolButton *m_relaySelBtn = nullptr;
    class QToolButton *m_relayDoneBtn = nullptr;
    class QToolButton *m_relayUndoBtn = nullptr;
    bool m_relaySelect = false;
    QStringList m_relayPath;
    // [relaykeep] Snapshot of each selected hop's Spot at click time:
    // a hop that ages out of the view window (or the window shrinks)
    // stays drawn from its snapshot while relay-select is active —
    // it leaves the map only via Undo or exiting the mode (operator,
    // 2026-08-14). Parallel to m_relayPath.
    QVector<Spot> m_relayPathSpots;
    void updateRelayButtons();
    // [showfix] The session-only resets (view -> mine, Auto zoom,
    // relay off) apply ONLY to a genuine open (first show, or show
    // after close). Minimize-restore and desktop-switch also fire
    // showEvent and were spontaneously reverting the map (operator,
    // 2026-08-15, twice).
    bool m_resetOnNextShow = true;

    // [hearlines] On-air heard-mesh storage: band → hearer →
    // (position + per-heard-call edges with their own timestamps).
    // Edges prune on the same 1 h horizon as spots; the view-window
    // filter applies at paint.
    struct HeardEdge {
        QDateTime when;
        float az = 0.0f;
        float dist = -1.0f; // < 0 = unresolved grid
        QString grid;
    };
    struct HearingEntry {
        QDateTime lastSeen;   // presence freshness (HBs, any frame)
        float az = 0.0f;
        float dist = -1.0f;
        QString grid;
        int snr = -99;        // SNR this station REPORTED TO US only
        QHash<QString, HeardEdge> heard;
    };
    QHash<QString, QHash<QString, HearingEntry>> m_hearingByBand;
    // [mqttgrid] call -> locator harvested from EVERY MQTT message
    // (sender sc/sl and reporter rc/rl) — fallback grid source for
    // on-air stations the sanctioned text sources haven't placed.
    QHash<QString, QString> m_gridByCall;
    int m_wheelAccum = 0; // [wheelzoom] trackpad delta accumulator
    QToolButton *m_pskrBtn = nullptr; // [pskrtoggle]
    QToolButton *m_callsBtn = nullptr; // [callsbtn]
    bool m_showPskr = true;
    void rememberGrid(QString const &call, QString const &grid);
    QString refinedGrid(QString const &call, QString const &grid) const;
    void showRelayPathToast();
    QHash<QString, QVector<Spot>> m_allSpotsByBand;

    // Drag-to-pan (session-only, like manual zoom; Auto recenters).
    // m_panPx = chart-center offset from the geometric center, in
    // logical px. Spots draw at TRUE radial position (no outer-ring
    // clamp) so a zoomed-in view can pan out to distant clusters.
    QPointF m_panPx;
    bool m_maybeDrag = false;
    bool m_dragging = false;
    QPointF m_pressPos;
    QPointF m_panAtPress;

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
    float m_mapCacheCut = -1.0f;

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
