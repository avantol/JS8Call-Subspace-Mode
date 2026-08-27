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
 * spotter reported. Rolling ONE-HOUR window (WINDOW_SECS),
 * auto-scaling radius,
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
#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QPointF>
#include <QPolygonF>
#include <QVector>
#include <QVariantMap>
#include <QWidget>

#include "JS8_Main/GridDb.h"

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
    // [visrace2] Call before the shutdown save: makes closeEvent stop
    // treating a window-manager close as "the user closed the map".
    void beginShutdown() { m_shuttingDown = true; }
    bool wasVisibleAtShutdown() const; // for startup restore
    // [visrace] User-intent close (menu toggle): clears the
    // restore-at-startup flag, unlike a shutdown-driven close.
    void userClose();
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
    // mesh (blue lines, drawn over the dark-yellow PSKR ones).
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
    // [#168 mapdump 2026-08-21] Read-only snapshot of what the map
    // knows RIGHT NOW, for the API dump. The live spot feed and the
    // hearing store exist only in RAM and no tool could reach them,
    // so offline planners were blind to the one fact that matters
    // most: who is on the air this minute (operator: "the spots map
    // is pretty much *the point*"). Debug/test surface — structure
    // may change; it is not a compatibility promise.
    QVariantMap dumpState(QString const &band = QString{}) const;

    // [#164] Authority lookup for outside consumers (hearGridFor's
    // fallback chain) — includes everything the persistent store
    // seeded. Returns empty when unknown.
    QString knownGrid(QString const &call) const {
        return m_gridByCall.value(call.toUpper());
    }

    // [reachport] Typed reads over the RAM store for the in-app
    // reaching executor -- the store stays the ONE authority
    // (GridDb.h: "RAM is the single in-session authority"); these are
    // the knownGrid() precedent applied to edges and presence. NO
    // QVariant round-trip: the executor asks the same questions the
    // frozen python asked of dumpState(), in process.
    struct HearerView {
        QString hearer;      // upper-case callsign
        QString grid;
        qint64  whenMs = 0;  // edge sighting time (ms since epoch)
        int     snr = -99;   // third-party: how well hearer copies
        QString source;      // "radio" | "hearing" | "mqtt"
    };
    QVector<HearerView> hearersOf(QString const &band,
                                  QString const &heard) const;
    struct StationView {
        QString call;        // upper-case
        QString grid;
        qint64  lastSeenMs = 0;
        int     snrToMe = -99;   // their report of OUR signal
        bool    hearsMe = false; // heard-map contains us
        QString source;
    };
    QVector<StationView> activeStations(QString const &band) const;

    // heardWhen: optional BACKDATED sighting time for the heard
    // edges ([#161] age-bearing replies) — invalid = now; an edge's
    // `when` only ever moves FORWARD. heardSnr: third-party SNR for
    // the heard edges (-99 = none).
    void addHearingReport(QString const &band, QString const &hearer,
                          QString const &hearerGrid,
                          QStringList const &heardCalls,
                          QStringList const &heardGrids,
                          int reportedToMeSnr = -99,
                          QDateTime const &heardWhen = QDateTime{},
                          int heardSnr = -99,
                          QString const &source = QString{});

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
    void leaveEvent(QEvent *) override;   // [hoverlift] drop the lift
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
    // [oneobs 2026-08-22] TRANSIENT RENDER UNIT ONLY -- never stored.
    // Built per paint from the observation store below. The map used to
    // keep two persistent QVector<Spot> stores alongside the mesh, and
    // every display bug of 2026-08-21/22 was those two disagreeing with
    // it: one spot per station capped the lines at one per station, dot
    // age diverged from line age, heardBy provenance leaked onto
    // stations visible for other reasons, and dots refreshed while
    // their edges froze. Operator: "what's wrong with: for each message
    // (or PSKR spot), draw a colour-coded or empty circle, connect the
    // two with a line?" Nothing -- so the observation is now the only
    // record, and circles and lines are drawn from it together.
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
        // [linecolor] Was the sender->reporter relationship supplied by
        // a PSKR spot? Yellow is PSKR STRICTLY, and heardBy only ever
        // comes from the internet feed -- but a station can be VISIBLE
        // for radio reasons while still carrying a PSKR-sourced
        // heardBy, and the line must not draw then.
        bool heardByPskr = false;
        // [radioage] Last RADIO evidence for this station; `when`
        // may be refreshed by internet reports, so PSKR-off aging
        // must use this clock (dot-without-line gap, 2026-08-15).
        QDateTime radioWhen;
        int snr = -99;          // dB as reported by the spotter;
                                // -99 = NO REPORT sentinel — never
                                // default to a claimable real value
        qint64 freqHz = 0;      // exact RF Hz the spotter logged us at
        float azimuth = 0.0f;   // degrees true, from my grid
        // ALWAYS km (unit conversion at paint). NEGATIVE = UNPLACED,
        // matching HeardEdge/HearingEntry. It defaulted to 0.0f, which
        // the visibility check reads as a valid position zero km away
        // -- so any station whose grid is unknown was drawn ON TOP OF
        // MY STATION (operator, 2026-08-22: "why is AK6OI on the map?
        // no grid, it's plotted over my station"). Zero is a real
        // distance; it must never double as "no distance".
        float distance = -1.0f;
        // [snrwho] WHEN THIS STATION REPORTED HEARING ME -- the age of
        // the X->ME edge, which is NOT the station's last-seen. Other
        // evidence refreshes `when` constantly, so showing that beside
        // "hears me at -10" claimed a 23-minute-old report was current
        // (W3NIC, 2026-08-23: station 51 s, its report of us 1413 s).
        // One fact, one clock.
        QDateTime reportsMeWhen;
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
    // [visrace2 2026-08-21] Set once app shutdown begins. A close
    // arriving after this is teardown, NEVER user intent -- see
    // closeEvent().
    bool m_shuttingDown = false;

    // Toast overlay (lazy-created by showToast()).
    class QLabel *m_toast = nullptr;
    QTimer m_toastTimer;

    // Zoom controls (upper-left, vertical: + / Auto / −). 0 = auto.
    // PERSISTED since [persistui] 2026-08-15 (operator revision
    // superseding the 2026-08-02 "always start in Auto" directive) --
    // the old comment claiming session-only survived the change and
    // contradicted saveSettings() for six days.
    class QToolButton *m_zoomInBtn = nullptr;
    class QToolButton *m_zoomAutoBtn = nullptr;
    class QToolButton *m_zoomOutBtn = nullptr;
    float m_manualScaleKm = 0.0f;
    float m_lastScaleKm = 0.0f; // effective scale of the last redraw
    // [fitdamp] Last AUTO-computed scale — the shrink hysteresis
    // compares only against this (manual scales polluted the damper:
    // Auto after "−" refused to re-fit). 0 = no damping (fresh fit).
    float m_lastAutoScaleKm = 0.0f;

    // [spotwin] View-window buttons (5/15/30/60 min, upper right).
    // PERSISTED ([persistui]); the value is VALIDATED on load against
    // the button set -- an out-of-set value (e.g. left by an
    // experimental build) checked the 60m button while filtering at
    // the stale number, which reads exactly like "the age selection
    // does nothing".
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
    // Persists ([persistui]). NOTE: these two must NOT use
    // autoExclusive — that would join the win5/15/30/60 sibling
    // group (autoExclusive groups by parent) — QButtonGroup instead.
    class QToolButton *m_viewMineBtn = nullptr;
    class QToolButton *m_viewAllBtn = nullptr;
    bool m_viewAll = false;
    // [connlines] "Show connections" toggle (lower right): 1 px
    // lines — dark yellow = PSKR-sourced, blue = on-air mesh.
    // Persists ([persistui]).
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
    // [showfix] Genuine-open detection (first show, or show after
    // close): only the relay builder resets there — everything else
    // persists. Minimize-restore and desktop-switch also fire
    // showEvent and must not reset anything (operator, 2026-08-15,
    // twice).
    bool m_resetOnNextShow = true;

    // [hearlines] On-air heard-mesh storage: band → hearer →
    // (position + per-heard-call edges with their own timestamps).
    // Edges prune on the same 1 h horizon as spots; the view-window
    // filter applies at paint.
    struct HeardEdge {
        QDateTime when;
        // [#170(f)] No az/dist here. Position is derived from the
        // grid by the ONE authority (m_gridByCall) at paint time;
        // storing it meant keeping a second copy in sync, which is
        // what the backfill walk existed to do.
        QString grid;
        // [#161 querycall] THIRD-PARTY snr (hearer's copy of the
        // heard station, e.g. a QUERY CALL "YES +08" reply). Feeds
        // this edge ONLY — never the reportedToMeSnr color
        // authority.
        int snr = -99;
        // [tribblenet] PROVENANCE, and the three differ in KIND:
        //   "radio"   FIRST-HAND -- our own receiver decoded it.
        //   "mqtt"    INTERNET -- PSKReporter said so. Vanishes in an
        //             offline session (#159), so never route on it
        //             when planning for no-internet operation.
        //   "hearing" THIRD-PARTY TESTIMONY, RF-derived: a HEARING?
        //             list, or a QUERY CALL reply -- INCLUDING one
        //             that reached us via a RELAY (operator
        //             2026-08-21). Our radio never copied the station
        //             being described; a remote station claims it,
        //             and the SNR/age are THAT station's copy,
        //             backdated by transit. Valid offline, but not an
        //             observation.
        // Without this, radio and internet edges are
        // indistinguishable -- which silently breaks #159 offline
        // routing and hides whether a relay is reachable by RF at all.
        QString source;
    };
    struct HearingEntry {
        QDateTime lastSeen;   // presence freshness (HBs, any frame)
        // [#170(f)] No az/dist here. Position is derived from the
        // grid by the ONE authority (m_gridByCall) at paint time;
        // storing it meant keeping a second copy in sync, which is
        // what the backfill walk existed to do.
        QString grid;
        int snr = -99;        // SNR this station REPORTED TO US only
        // [tribblenet audit 2026-08-21] PRESENCE provenance, same
        // vocabulary as HeardEdge::source. Needed because the PSKR
        // feed now populates this store: without it the anchor pass
        // synthesized dots for internet-only stations even with "Add
        // PSKReporter spots" OFF, silently breaking that toggle's
        // contract. "radio" once ANY on-air evidence is seen -- a
        // station heard on the air never reverts to internet-only.
        QString source;
        QHash<QString, HeardEdge> heard;
    };
    // [oneobs] THE observation store: band -> hearer -> (position +
    // per-heard-call edges, each with its own time, SNR and source).
    // ONE record per observation; circles AND lines are drawn from it.
    QHash<QString, QHash<QString, HearingEntry>> m_hearingByBand;
    // [oneobs] Facts that belong to a STATION rather than to any one
    // observation -- country and the frequency THEY transmit on. Small,
    // and kept beside the observations rather than inside them so an
    // observation stays a pure relationship.
    struct StationInfo {
        QString country;
        qint64 freqHz = 0;
        bool sawAsSender = false;  // observed transmitting => not rxOnly
    };
    QHash<QString, QHash<QString, StationInfo>> m_infoByBand;
    // [mqttgrid] call -> locator harvested from EVERY MQTT message
    // (sender sc/sl and reporter rc/rl) — fallback grid source for
    // on-air stations the sanctioned text sources haven't placed.
    QHash<QString, QString> m_gridByCall;
    // [#164] Persistent tier of the authority above — seeds it at
    // startup, records every accepted refinement. Never read
    // directly by consumers.
    GridDb m_gridDb;
    // [#168 part 3] Write-behind journal timer. RAM stays the query
    // authority; this only drains the queue into ONE transaction.
    QTimer m_dbFlushTimer;
    // Restore the who-hears-whom mesh (and spots) banked by previous
    // sessions. Without this every restart destroyed the mesh, and a
    // route search could only ever see what refilled since — measured
    // three times mid-route on 2026-08-21.
    void restoreMeshFromDisk();
    // [#168 part 3] Journal one accepted observation as STATION FACTS.
    // ONE place, called from BOTH insertion paths (on-air and PSKR) --
    // patching each site separately is how the two drift apart. The
    // RELATIONSHIP it implies is journalled by addHearingReport as an
    // edge; this records only what is true of the station.
    void journalStation(QString const &band, QString const &call);
    void restoreStationsFromDisk();
    int m_wheelAccum = 0; // [wheelzoom] trackpad delta accumulator
    // [zoomkeepcenter] Auto-fit pan applied at the last redraw —
    // baked into m_panPx when the operator leaves Auto so manual
    // zoom keeps the same center point.
    QPointF m_autoPanPx;
    QToolButton *m_pskrBtn = nullptr; // [pskrtoggle]
    QToolButton *m_callsBtn = nullptr; // [callsbtn]
    // [attemptviz 2026-08-22] Live picture of what we are trying RIGHT
    // NOW. A call we put on the air draws a fat dashed red path along
    // the relay chain; the dash GAP widens as the reply window runs
    // down, so the line visibly "runs out" and then vanishes. A reply
    // from a station we called turns its path fat green for a few
    // seconds. Purely presentational: nothing here feeds routing.
    struct Attempt {
        QStringList path;      // [relay..., target], uppercase
        QDateTime started;
        int waitSecs = 70;     // when the dashes finish opening up
        bool replied = false;  // green, and on its own short timer
        QDateTime repliedAt;
    };
    QVector<Attempt> m_attempts;
    class QTimer *m_attemptTimer = nullptr;
    void tickAttempts();

public:
    // Called by the app: an outgoing directed call/relay, and a reply
    // from a station we called (HB replies included).
    void noteAttempt(QStringList const &path, int waitSecs);
    void noteReply(QString const &from);
    // We have STOPPED WAITING. The map's countdown is its own estimate
    // of how long an answer might take; the caller knows when it has
    // actually given up, and that is usually sooner. Clearing then
    // rather than letting the dashes run out tells the operator the
    // attempt is over and they can move on (operator, 2026-08-22).
    // Only un-replied attempts go: a green result is a result.
    void clearAttempts();

private:
    bool m_showPskr = true;
    // [hoverlift] Station under the cursor, uppercase, empty when none.
    // When the PSKR lines have been muted for density, this station's
    // lines are drawn at full brightness and on top, so one station can
    // be read out of a crowded field without changing the view.
    QString m_hoverCall;
    // [paintlog] What the last paint actually did, so a slow frame can
    // be attributed instead of guessed at.
    mutable int m_lastNoteCount = 0;
    mutable int m_lastSegCount = 0;
    mutable qint64 m_lastPaintMs = 0;  // drives requestReplot's interval
    mutable qint64 m_lastBuildMs = 0;  // render-set rebuild
    mutable qint64 m_lastGeoMs = 0;    // coastline walk, per frame
    mutable int m_lastGeoPts = 0;      // points in that walk
    mutable int m_lastGeoPolys = 0;
    mutable qint64 m_lastSegMs = 0;    // everything after the lines
    mutable QElapsedTimer m_segTimer;
    void rememberGrid(QString const &call, QString const &grid,
                      QString const &source =
                          QStringLiteral("radio"));
    QString refinedGrid(QString const &call, QString const &grid) const;
    void showRelayPathToast();
    void stepZoom(int dir); // [zoomkeepcenter]
    // [radioage] THE presence/age clock; [snrwho] THE reported-to-me
    // SNR rule — single definitions, every consumer reads these.
    QDateTime effectiveWhen(Spot const &s) const;
    int reportedToMeSnr(Spot const &s) const;

    // Drag-to-pan (persists, [persistui]; the Auto button zeroes it).
    // m_panPx = chart-center offset from the geometric center, in
    // logical px. Spots draw at TRUE radial position (no outer-ring
    // clamp) so a zoomed-in view can pan out to distant clusters.
    QPointF m_panPx;
    bool m_maybeDrag = false;
    bool m_dragging = false;
    QPointF m_pressPos;
    QPointF m_panAtPress;

    bool m_showCallsigns = false; // [callsbtn] persisted toggle

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
