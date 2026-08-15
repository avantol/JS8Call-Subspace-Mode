/**
 * @file SpotMapWindow.cpp
 * @brief "Spots Map" implementation. See header for the design story.
 */

#include "SpotMapWindow.h"

#include "JS8_Include/SettingsGroup.h"
#include "JS8_Main/Bands.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Main/Geodesic.h"
#include "JS8_Network/MqttClient.h"
#include "JS8_UI/Configuration.h"

#include "SpotMapGeoData.h"

#include <QCloseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QApplication>
#include <QSet>
#include <QSettings>
#include <QLabel>
#include <QToolButton>
#include <QButtonGroup>
#include <QToolTip>
#include <QTimer>

#include <algorithm>
#include <cmath>

Q_DECLARE_LOGGING_CATEGORY(mqttclient_js8)

namespace {
constexpr double DEG2RAD = 0.017453292519943295;
constexpr int TITLE_STRIP_PX = 24;
constexpr int LEGEND_STRIP_PX = 40;
constexpr int MARGIN_PX = 18;
constexpr int DOT_RADIUS_PX = 3;
constexpr float DEFAULT_SCALE_KM = 5000.0f;
constexpr float FLOOR_SCALE_KM = 500.0f;
// Manual-zoom range: floor shared with auto-scale; ceiling just past
// the antipode (~20000 km) so one more "−" from any auto scale always
// shows the whole reachable Earth.
constexpr float MAX_SCALE_KM = 20000.0f;

QString const kMqttHost = QStringLiteral("mqtt.pskreporter.info");
constexpr quint16 kMqttPort = 1883;
} // namespace

SpotMapWindow::SpotMapWindow(QSettings *settings,
                             Configuration const *config, QWidget *parent)
    : QWidget{parent}, m_settings{settings}, m_config{config},
      m_mqtt{new MqttClient{this}} {
    setWindowFlag(Qt::Window);
    setWindowTitle(tr("Spots Map"));
    setMinimumSize(320, 320);
    setMouseTracking(true); // hover tooltips on spots

    {
        SettingsGroup g{m_settings, "SpotMap"};
        restoreGeometry(
            m_settings->value("geometry", saveGeometry()).toByteArray());
        m_restoreVisible = m_settings->value("WindowVisible", false).toBool();
        m_showRings = m_settings->value("ShowRings", false).toBool();
        m_showCallsigns = m_settings->value("ShowCallsigns", false).toBool();
        // [persistui 2026-08-15] Connections overlay, map type, and
        // PSKR toggle persist across sessions AND across hide/show.
        m_showConnections =
            m_settings->value("ShowConnections", false).toBool();
        m_viewAll = m_settings->value("ViewAll", false).toBool();
        m_showPskr = m_settings->value("ShowPskr", true).toBool();
    }

    m_replotTimer.setSingleShot(true);
    m_replotTimer.setInterval(150);
    connect(&m_replotTimer, &QTimer::timeout, this, &SpotMapWindow::redraw);

    m_pruneTimer.setInterval(30 * 1000);
    connect(&m_pruneTimer, &QTimer::timeout, this, &SpotMapWindow::onPruneTick);
    m_pruneTimer.start();

    connect(m_mqtt, &MqttClient::messageReceived,
            this, &SpotMapWindow::onMqttMessage);
    connect(m_mqtt, &MqttClient::stateChanged,
            this, &SpotMapWindow::onMqttState);

    m_mqtt->setServer(kMqttHost, kMqttPort);
    // Station (and therefore topics + start) is seeded by the main
    // window via setStation() right after construction — the client
    // runs from app launch, independent of window visibility.

    // Zoom controls: small vertical stack in the upper-left, below
    // the title strip. Plain child widgets (no layout — the chart is
    // one custom-painted surface), fixed positions.
    auto const makeZoomButton = [this](QString const &text) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setFixedSize(36, 20);
        QFont f = b->font();
        f.setPointSize(8);
        b->setFont(f);
        b->setStyleSheet(QStringLiteral(
            "QToolButton { background-color: rgba(40,40,55,200);"
            " color: rgb(210,210,225); border: 1px solid rgb(70,70,90);"
            " border-radius: 3px; }"
            "QToolButton:hover { background-color: rgba(60,60,80,220); }"
            "QToolButton:checked { background-color: rgba(80,100,150,230);"
            " color: white; border-color: rgb(120,140,190); }"
            "QToolButton:disabled { color: rgb(120,120,140); }"));
        return b;
    };
    m_zoomInBtn = makeZoomButton(QStringLiteral("+"));
    m_zoomAutoBtn = makeZoomButton(tr("Auto"));
    m_zoomOutBtn = makeZoomButton(QStringLiteral("−")); // minus sign
    // +/− glyphs bold and 50% larger (8 → 12 pt) for legibility;
    // "Auto" keeps the small face (operator, 2026-08-03).
    for (QToolButton *b : {m_zoomInBtn, m_zoomOutBtn}) {
        QFont f = b->font();
        f.setPointSize(12);
        f.setBold(true);
        b->setFont(f);
    }
    int const zx = 6;
    int y = TITLE_STRIP_PX + 6;
    for (QToolButton *b : {m_zoomInBtn, m_zoomAutoBtn, m_zoomOutBtn}) {
        b->move(zx, y);
        y += 22;
    }
    m_zoomInBtn->setToolTip(tr("Zoom in"));
    m_zoomAutoBtn->setToolTip(tr("Auto zoom (fit all spots)"));
    m_zoomOutBtn->setToolTip(tr("Zoom out"));

    // [spotwin] Accumulation-window buttons, upper right. Checkable +
    // auto-exclusive (siblings), default 60 min each open (matches
    // the fleet's hourly re-spot cache). Storage always keeps the
    // full hour; these only filter the display.
    m_win5Btn = makeZoomButton(QStringLiteral("5m"));
    m_win15Btn = makeZoomButton(QStringLiteral("15m"));
    m_win30Btn = makeZoomButton(QStringLiteral("30m"));
    m_win60Btn = makeZoomButton(QStringLiteral("60m"));
    struct WinDef { QToolButton *b; int secs; };
    for (WinDef const wd : {WinDef{m_win5Btn, 5 * 60},
                            WinDef{m_win15Btn, 15 * 60},
                            WinDef{m_win30Btn, 30 * 60},
                            WinDef{m_win60Btn, 60 * 60}}) {
        wd.b->setCheckable(true);
        wd.b->setAutoExclusive(true);
        wd.b->setToolTip(tr("Show spots from the last %1 minutes")
                             .arg(wd.secs / 60));
        connect(wd.b, &QToolButton::clicked, this, [this, wd]() {
            m_viewWindowSecs = wd.secs;
            requestReplot();
        });
    }
    m_win60Btn->setChecked(true);

    // [viewall] View-selector buttons, lower-left vertical stack: my
    // call on top (the map as always: who hears ME), "All" below
    // (every JS8 spot on the band, heard sender plotted). Exclusive
    // via QButtonGroup — autoExclusive would merge them into the
    // win15/30/60 sibling group. Session-only: showEvent reverts to
    // my view on every open (sibling convention).
    m_viewMineBtn = makeZoomButton(tr("Me")); // real call set in setStation
    m_viewAllBtn = makeZoomButton(tr("All"));
    auto *viewGroup = new QButtonGroup(this);
    viewGroup->setExclusive(true);
    for (QToolButton *b : {m_viewMineBtn, m_viewAllBtn}) {
        b->setCheckable(true);
        viewGroup->addButton(b);
    }
    (m_viewAll ? m_viewAllBtn : m_viewMineBtn)->setChecked(true); // [persistui]
    m_viewMineBtn->setToolTip(tr("Show stations spotting MY signal"));
    m_viewAllBtn->setToolTip(
        tr("Show all spotted stations on this band (all reporters)"));
    connect(m_viewMineBtn, &QToolButton::clicked, this, [this]() {
        // [relaysel] Choosing my view exits relay-select entirely
        // (mode + button state + selections; operator 2026-08-14).
        if (m_relaySelBtn && m_relaySelBtn->isChecked())
            m_relaySelBtn->setChecked(false); // toggled() clears path
        if (!m_viewAll) return;
        m_viewAll = false;
        requestReplot();
    });
    connect(m_viewAllBtn, &QToolButton::clicked, this, [this]() {
        if (m_viewAll) return;
        m_viewAll = true;
        requestReplot();
    });

    // [connlines] Connections toggle, lower right. Draws 1 px light
    // green lines between stations hearing each other (my view:
    // center to each spotter; All view: heard sender to its
    // reporter). Session-only, off at every open.
    // [pskrtoggle] Internet-sourced spots are OPT-OUT: the button
    // sits below the view stack as its own function group.
    m_pskrBtn = makeZoomButton(tr("Add PSKReporter spots"));
    m_pskrBtn->setCheckable(true);
    m_pskrBtn->setChecked(m_showPskr); // [persistui]
    m_pskrBtn->setAutoExclusive(false);
    m_pskrBtn->setToolTip(
        tr("Add PSKReporter (internet-sourced) spots to your on-air "
           "radio spots"));
    connect(m_pskrBtn, &QToolButton::toggled, this, [this](bool on) {
        m_showPskr = on;
        requestReplot();
    });

    m_connBtn = makeZoomButton(tr("Show connections"));
    m_connBtn->setCheckable(true);
    m_connBtn->setChecked(m_showConnections); // [persistui]
    m_connBtn->setToolTip(
        tr("Draw lines between stations hearing each other"));
    connect(m_connBtn, &QToolButton::toggled, this, [this](bool on) {
        m_showConnections = on;
        requestReplot();
    });
    // [callsbtn] On-map callsign labels, both views (operator
    // 2026-08-15). Sits directly above View connections.
    m_callsBtn = makeZoomButton(tr("Show call signs"));
    m_callsBtn->setCheckable(true);
    m_callsBtn->setChecked(m_showCallsigns);
    m_callsBtn->setAutoExclusive(false);
    m_callsBtn->setToolTip(
        tr("Show each station's call sign next to its dot"));
    connect(m_callsBtn, &QToolButton::toggled, this, [this](bool on) {
        m_showCallsigns = on;
        requestReplot();
    });

    // [relaysel] Relay-path builder rows above Connections:
    //   [Select relay(s)]
    //   [Done] [Undo]
    //   [Connections]
    // Click-to-append while the toggle is on; Done emits the
    // template; Undo pops one hop; untoggling clears everything.
    m_relaySelBtn = makeZoomButton(tr("Select relay(s)"));
    m_relaySelBtn->setCheckable(true);
    m_relaySelBtn->setToolTip(
        tr("Build a relay path: click at least two stations, in "
           "outbound order."));
    m_relayDoneBtn = makeZoomButton(tr("Done"));
    m_relayDoneBtn->setToolTip(
        tr("Put the relay message template in the outgoing box"));
    m_relayUndoBtn = makeZoomButton(tr("Undo"));
    m_relayUndoBtn->setToolTip(tr("Remove the last selected station"));
    connect(m_relaySelBtn, &QToolButton::toggled, this, [this](bool on) {
        m_relaySelect = on;
        if (on && !m_viewAll) {
            // [relaysel] Relay hops are picked from ALL heard
            // stations — selecting flips the map to the All view
            // (operator revision 2026-08-14).
            m_viewAll = true;
            m_viewAllBtn->setChecked(true);
        }
        if (on)
            showToast(
                tr("Click to select relay stations, in outbound order"));
        if (!on) {
            m_relayPath.clear(); // disable = discard selections
            m_relayPathSpots.clear();
        }
        updateRelayButtons();
        requestReplot();
    });
    connect(m_relayUndoBtn, &QToolButton::clicked, this, [this]() {
        if (!m_relayPath.isEmpty()) {
            m_relayPath.removeLast();
            m_relayPathSpots.removeLast(); // [relaykeep]
        }
        showRelayPathToast();
        updateRelayButtons();
        requestReplot();
    });
    connect(m_relayDoneBtn, &QToolButton::clicked, this, [this]() {
        if (m_relayPath.isEmpty())
            return;
        // "HOP1>HOP2>DEST [MESSAGE]" — plain directed relay text, no
        // ARQ wrapping on any hop (operator directive 2026-08-14).
        QString const tpl = m_relayPath.join(QLatin1Char('>')) +
                            QStringLiteral(" [MESSAGE]");
        Q_EMIT relayTemplateReady(tpl);
        showToast(tr("Relay template ready — type your message"));
        m_relaySelBtn->setChecked(false); // clears path via toggled()
    });
    updateRelayButtons();

    positionWindowButtons();
    // Auto is the startup state; the disabled Auto button doubles as
    // the mode indicator (disabled = auto active).
    m_zoomAutoBtn->setEnabled(false);
    connect(m_zoomInBtn, &QToolButton::clicked,
            this, &SpotMapWindow::zoomIn);
    connect(m_zoomAutoBtn, &QToolButton::clicked,
            this, &SpotMapWindow::zoomAuto);
    connect(m_zoomOutBtn, &QToolButton::clicked,
            this, &SpotMapWindow::zoomOut);
}

SpotMapWindow::~SpotMapWindow() = default;

bool SpotMapWindow::wasVisibleAtShutdown() const { return m_restoreVisible; }

void SpotMapWindow::saveSettings() {
    SettingsGroup g{m_settings, "SpotMap"};
    m_settings->setValue("geometry", saveGeometry());
    m_settings->setValue("WindowVisible", isVisible());
    m_settings->setValue("ShowRings", m_showRings);
    m_settings->setValue("ShowCallsigns", m_showCallsigns);
    m_settings->setValue("ShowConnections", m_showConnections); // [persistui]
    m_settings->setValue("ViewAll", m_viewAll);
    m_settings->setValue("ShowPskr", m_showPskr);
}

// -------------------------------------------------------------------------
// Station / band plumbing
// -------------------------------------------------------------------------

void SpotMapWindow::rebuildTopics() {
    if (m_myCall.isEmpty()) {
        m_mqtt->stop();
        return;
    }
    // The broker encodes '/' in callsigns as '.' at the topic level
    // — verified live 2026-07-15 with WM8Q/P: topic level "WM8Q.P",
    // payload sc "WM8Q/P". Subscribe on the EXACT call, dot-encoded;
    // the payload filter then exact-matches sc against m_myCall.
    QString topicCall = m_myCall.toUpper();
    topicCall.replace(QLatin1Char('/'), QLatin1Char('.'));

    // pskr/filter/v2/{band}/{mode}/{sender}/{receiver}/{senderLoc}/...
    // All bands (caches fill in the background), mode JS8 (spotters
    // report our transmissions with the "JS8" mode string).
    // [viewall] Subscribe to ALL JS8 senders (superset of the old
    // exact-call topic) so the "All" view accumulates from launch
    // like the my-view always has. Volume is modest — JS8-mode spots
    // only, latest-per-sender storage — and the my-view dataset is
    // still gated by the exact payload sc match below, unchanged.
    QString topic = QStringLiteral("pskr/filter/v2/+/JS8/#");

    // Documented debug hook: flood-filter override for protocol
    // bring-up without transmitting, e.g.
    //   JS8_SPOTMAP_TOPIC_OVERRIDE=pskr/filter/v2/20m/FT8/#
    if (QString const ov =
            qEnvironmentVariable("JS8_SPOTMAP_TOPIC_OVERRIDE");
        !ov.isEmpty()) {
        qCWarning(mqttclient_js8) << "topic override active:" << ov;
        topic = ov;
    }

    // [spotfmt] Accumulation clock: restarts whenever the
    // subscription (re)starts — the title's "last X of Y min" is
    // honest about how much history can possibly be on screen.
    m_accumStart = DriftingDateTime::currentDateTimeUtc();
    m_mqtt->setClientIdPrefix(QStringLiteral("JS8Call_%1").arg(topicCall));
    m_mqtt->setTopics({topic});
    m_debugDumpsLeft = 20;
    m_mqtt->start();
}

void SpotMapWindow::setStation(QString const &callsign, QString const &grid) {
    if (callsign == m_myCall && grid == m_myGrid)
        return;
    m_myCall = callsign;
    m_myGrid = grid;
    // Azimuth/distance and subscription are both stale — start over.
    qCWarning(mqttclient_js8)
        << "[SPOTMAP] station changed — ALL spot data cleared:"
        << callsign << grid; // [showfix] attribute any real data loss
    m_spotsByBand.clear();
    m_allSpotsByBand.clear(); // [viewall] same staleness
    m_hearingByBand.clear();  // [hearlines] az/dist keyed to my grid
    positionWindowButtons();  // [viewall] my-call button text/width
    rebuildTopics();
    requestReplot();
}

void SpotMapWindow::setDialFrequency(qint64 const hz) {
    m_dialHz = hz;
}

void SpotMapWindow::setBand(QString const &band) {
    if (band == m_currentBand)
        return;
    m_currentBand = band;
    // [relaykeep] A relay path is per-band by nature — band change
    // clears the selections AND the mode/button (operator,
    // 2026-08-14). setChecked(false) routes through toggled(), which
    // clears path + snapshots.
    if (m_relaySelBtn && m_relaySelBtn->isChecked())
        m_relaySelBtn->setChecked(false);
    requestReplot();
}

// -------------------------------------------------------------------------
// MQTT ingest
// -------------------------------------------------------------------------

// [onairspot] See header — my-view spots from on-air evidence.
void SpotMapWindow::addOnAirSpotOfMe(QString const &band,
                                     QString const &call,
                                     QString const &grid, int const snr) {
    if (band.isEmpty() || call.isEmpty() || grid.size() < 4 ||
        m_myGrid.size() < 4)
        return;
    rememberGrid(call, grid); // [gridfine] radio grids feed the
                              // authority too — the 6-char truth can
                              // arrive by RADIO while PSKR only ever
                              // supplies a 4-char registered locator
                              // (KJ7VWV: call-list DN52QT vs rl DN52;
                              // three MQTT-cache-only fixes failed)
    QString const gridF = refinedGrid(call, grid);
    auto const vec = Geodesic::vector(m_myGrid, gridF);
    if (!vec.azimuth().isValid() || !vec.distance().isValid())
        return;
    Spot spot;
    spot.reportsMe = true; // [allsuper]
    spot.when = DriftingDateTime::currentDateTimeUtc();
    spot.receiverCall = call.toUpper();
    spot.receiverGrid = gridF;
    // [hbdots] A frame with no report value must not ERASE a known
    // report: fall back to (1) this call's previous my-view spot,
    // (2) the SNR it stamped into the hearing store (SNR reply that
    // arrived before its position was known — the AC7WY sequence).
    int effSnr = snr;
    if (effSnr <= -99) {
        for (Spot const &old : m_spotsByBand.value(band))
            if (old.receiverCall == spot.receiverCall &&
                !old.monitorOnly) {
                effSnr = old.snr;
                break;
            }
    }
    if (effSnr <= -99) {
        if (auto const &hearers = m_hearingByBand.value(band);
            hearers.contains(spot.receiverCall) &&
            hearers.value(spot.receiverCall).snr > -99)
            effSnr = hearers.value(spot.receiverCall).snr;
    }
    // [snrwho] Keep the -99 sentinel in the store — collapsing it
    // to 0 made "no report" indistinguishable from a real 0 dB
    // (operator 2026-08-15). Paint is safe: monitorOnly renders
    // hollow, never heat-colored.
    spot.snr = effSnr;
    spot.monitorOnly = (effSnr <= -99);
    spot.azimuth = vec.azimuth();
    spot.distance = vec.distance();
    auto &spots = m_spotsByBand[band];
    spots.erase(std::remove_if(spots.begin(), spots.end(),
                               [&](Spot const &s) {
                                   return s.receiverCall ==
                                          spot.receiverCall;
                               }),
                spots.end());
    spots.append(spot);
    pruneBand(band);
    if (band == m_currentBand && isVisible() && !m_viewAll)
        requestReplot();
}

// [hearlines] See header. Latest position per station; edges keyed
// per heard call with their own timestamps, so a conversation with a
// new partner never evicts an older edge (it just ages out).
void SpotMapWindow::addHearingReport(QString const &band,
                                     QString const &hearer,
                                     QString const &hearerGrid,
                                     QStringList const &heardCalls,
                                     QStringList const &heardGrids,
                                     int const reportedToMeSnr) {
    if (band.isEmpty() || hearer.isEmpty())
        return;
    auto const now = DriftingDateTime::currentDateTimeUtc();
    auto const resolve = [&](QString const &grid, float *az, float *dist) {
        *az = 0.0f;
        *dist = -1.0f;
        if (grid.size() < 4 || m_myGrid.size() < 4)
            return;
        if (auto const v = Geodesic::vector(m_myGrid, grid);
            v.azimuth().isValid() && v.distance().isValid()) {
            *az = v.azimuth();
            *dist = v.distance();
        }
    };
    bool const isNew = !m_hearingByBand[band].contains(hearer.toUpper());
    // [gridtrim] Frame grid fields arrive space-prefixed (" EM38",
    // field 2026-08-15 W0MQD) — trim before resolving.
    // [gridfine] Feed then refine: the authority holds the most
    // precise grid seen from ANY source (also serves as the
    // empty-grid fallback).
    rememberGrid(hearer, hearerGrid.trimmed());
    QString const hearerGridT =
        refinedGrid(hearer, hearerGrid.trimmed());
    HearingEntry &e = m_hearingByBand[band][hearer.toUpper()];
    e.lastSeen = now;
    // [placelog] Attribute every FIRST placement of an on-air station
    // (field 2026-08-15: W0MQD appeared at a grid no logged message
    // supplied — source unproven; this line convicts the next one).
    if (isNew && !hearerGridT.isEmpty())
        qCWarning(mqttclient_js8)
            << "[SPOTMAP] on-air station placed:" << hearer
            << "grid=" << hearerGridT << "band=" << band;
    if (!hearerGridT.isEmpty() && (e.grid.isEmpty() || e.dist < 0.0f)) {
        e.grid = hearerGridT;
        resolve(hearerGridT, &e.az, &e.dist);
    }
    if (reportedToMeSnr > -99)
        e.snr = reportedToMeSnr;
    for (int i = 0; i < heardCalls.size(); ++i) {
        QString const call = heardCalls.at(i).toUpper();
        if (call.isEmpty() || call == hearer.toUpper())
            continue;
        HeardEdge &edge = e.heard[call];
        edge.when = now;
        QString const gRaw =
            (i < heardGrids.size() ? heardGrids.at(i) : QString())
                .trimmed();
        rememberGrid(call, gRaw); // [gridfine] feed then refine
        QString const g = refinedGrid(call, gRaw);
        if (!g.isEmpty() || edge.grid.isEmpty()) {
            edge.grid = g;
            resolve(g, &edge.az, &edge.dist);
        }
    }
    if (band == m_currentBand && isVisible() && m_showConnections)
        requestReplot();
}

QString SpotMapWindow::refinedGrid(QString const &call,
                                   QString const &grid) const {
    // [gridfine] Precision cure for stations jumping between views:
    // a 4-char square from call-list/on-air text can sit ~35 km
    // from the 6-char PSKR locator of the SAME square. If the MQTT
    // cache holds a longer grid in the same square, use it — every
    // source then projects to identical coordinates. Different
    // square = station genuinely moved; keep the supplied grid.
    QString const cached = m_gridByCall.value(call.toUpper());
    if (cached.size() > grid.size() &&
        (grid.isEmpty() ||
         cached.left(4).compare(grid.left(4),
                                Qt::CaseInsensitive) == 0))
        return cached;
    return grid;
}

void SpotMapWindow::rememberGrid(QString const &call,
                                 QString const &grid) {
    // [mqttgrid] Harvest a locator from the MQTT feed and backfill
    // any hearing-store entry/edge stored without a position — the
    // feed runs constantly, so a HEARING-list station we've never
    // copied gets placed the moment ANY reporter mentions it
    // (operator, 2026-08-16).
    if (call.isEmpty() || grid.size() < 4)
        return;
    QString const key = call.toUpper();
    QString const known = m_gridByCall.value(key);
    if (known == grid)
        return; // already known — nothing to backfill
    // [gridfine] Never downgrade precision: a shorter grid in the
    // same square keeps the cached long form.
    if (known.size() > grid.size() &&
        known.left(4).compare(grid.left(4), Qt::CaseInsensitive) == 0)
        return;
    m_gridByCall.insert(key, grid);
    if (m_myGrid.size() < 4)
        return;
    auto const v = Geodesic::vector(m_myGrid, grid);
    if (!v.azimuth().isValid() || !v.distance().isValid())
        return;
    bool touched = false;
    // [gridfine] Upgrade covers unplaced entries AND ones placed on
    // a same-square shorter grid — existing dots snap to the precise
    // position instead of straddling two spots across views.
    auto const upgradable = [&](QString const &g, float dist) {
        if (dist < 0.0f)
            return true;
        return grid.size() > g.size() &&
               grid.left(4).compare(g.left(4),
                                    Qt::CaseInsensitive) == 0;
    };
    for (auto b = m_hearingByBand.begin(); b != m_hearingByBand.end();
         ++b) {
        auto he = b.value().find(key);
        if (he != b.value().end() && upgradable(he->grid, he->dist)) {
            he->grid = grid;
            he->az = v.azimuth();
            he->dist = v.distance();
            touched = true;
        }
        for (auto &entry : b.value()) {
            auto ed = entry.heard.find(key);
            if (ed != entry.heard.end() &&
                upgradable(ed->grid, ed->dist)) {
                ed->grid = grid;
                ed->az = v.azimuth();
                ed->dist = v.distance();
                touched = true;
            }
        }
    }
    // [gridfine] The SPOT datasets hold their birth grid until the
    // station transmits again — upgrade them too, or a my-view dot
    // sits on the 4-char square while the All-view anchor has
    // snapped (field 2026-08-15: KJ7VWV at DN52 vs DN52QT).
    auto const upgradeSpots = [&](QHash<QString, QVector<Spot>> &byBand) {
        for (auto &vec : byBand)
            for (Spot &sp : vec) {
                if (sp.receiverCall == key &&
                    grid.size() > sp.receiverGrid.size() &&
                    grid.left(4).compare(sp.receiverGrid.left(4),
                                         Qt::CaseInsensitive) == 0) {
                    sp.receiverGrid = grid;
                    sp.azimuth = v.azimuth();
                    sp.distance = v.distance();
                    touched = true;
                }
                if (sp.heardBy == key &&
                    (sp.heardByDist < 0.0f ||
                     (grid.size() > sp.heardByGrid.size() &&
                      grid.left(4).compare(sp.heardByGrid.left(4),
                                           Qt::CaseInsensitive) ==
                          0))) {
                    sp.heardByGrid = grid;
                    sp.heardByAz = v.azimuth();
                    sp.heardByDist = v.distance();
                    touched = true;
                }
            }
    };
    upgradeSpots(m_spotsByBand);
    upgradeSpots(m_allSpotsByBand);
    if (touched) {
        qCWarning(mqttclient_js8)
            << "[SPOTMAP] station position refined:"
            << key << "grid=" << grid; // [placelog]
        if (isVisible())
            requestReplot();
    }
}

void SpotMapWindow::onMqttState(QString const &state) {
    m_stateText = state;
    requestReplot();
}

void SpotMapWindow::onMqttMessage(QString const &topic,
                                  QByteArray const &payload) {
    // Runtime schema verification: dump the first N messages after
    // each (re)subscribe so topic-level ordering and field names can
    // be confirmed live before trusting them.
    if (m_debugDumpsLeft > 0) {
        --m_debugDumpsLeft;
        qCDebug(mqttclient_js8) << "spot" << topic << payload;
    }

    QJsonObject const o = QJsonDocument::fromJson(payload).object();
    if (o.isEmpty()) {
        ++m_skippedSpots;
        return;
    }

    // Verify the sender is EXACTLY us. The topic subscribe uses the
    // base callsign level (topic levels can't contain '/'), so the
    // stream is a superset when running with a /P /M suffix — but
    // WM8Q and WM8Q/P are distinct stations operationally, and this
    // map must show only the EXACT configured call's spots (full-
    // callsign-compare rule; no base-call fallback here).
    // NOTE: how the broker encodes a suffixed call at the topic
    // level is unverified — when operating /P, watch the first-N
    // debug dumps (mqttclient.js8) to confirm suffixed spots arrive
    // on the base-level subscription at all.
    QString const sender = o.value(QStringLiteral("sc")).toString();
    // [viewall] The subscription now carries ALL JS8 senders. Spots
    // of MY signal feed the my-view dataset exactly as before (the
    // exact-sc rule above still governs it); every OTHER sender's
    // spot feeds the All-view dataset further below.
    bool const senderIsMe =
        !sender.isEmpty() &&
        sender.compare(m_myCall, Qt::CaseInsensitive) == 0;

    QString const receiverCall = o.value(QStringLiteral("rc")).toString();
    QString const receiverGrid = o.value(QStringLiteral("rl")).toString();
    int const snr = o.value(QStringLiteral("rp")).toInt(-99);
    // [mqttgrid] Harvest locators BEFORE any validity bail-out — a
    // spot we skip as a spot is still a grid sighting.
    rememberGrid(receiverCall, receiverGrid);
    rememberGrid(sender, o.value(QStringLiteral("sl")).toString());
    if (receiverCall.isEmpty() || receiverGrid.size() < 4 || snr == -99) {
        ++m_skippedSpots;
        return;
    }
    // [BUILD 340] Exact RF Hz the spotter logged us at (payload f) —
    // hover shows the audio offset (f − dial) and double-click QSYs
    // to it.
    qint64 const spotFreqHz =
        static_cast<qint64>(o.value(QStringLiteral("f")).toDouble(0));
    // [BUILD 340] Country name for hover, only when the spotter's
    // DXCC differs from OURS — both codes ride the topic
    // (…/{sDXCC}/{rDXCC}); the NAME comes from the injected
    // LogBook/cty.dat lookup by callsign.
    QString country;
    if (QStringList const lv = topic.split(QLatin1Char('/'));
        lv.size() > 10 && m_countryLookup && lv.at(9) != lv.at(10)) {
        country = m_countryLookup(receiverCall);
    }

    // Band: prefer the topic level (pskr/filter/v2/{band}/...), fall
    // back to mapping the reported frequency.
    QString band;
    if (QStringList const levels = topic.split(QLatin1Char('/'));
        levels.size() > 3)
        band = levels.at(3);
    if (band.isEmpty() || m_config->bands()->find(band) < 0) {
        auto const f = static_cast<Radio::Frequency>(
            o.value(QStringLiteral("f")).toDouble(0));
        if (f > 0)
            band = m_config->bands()->find(f);
    }
    if (band.isEmpty()) {
        ++m_skippedSpots;
        return;
    }

    // [viewall] Which station gets PLOTTED depends on the dataset:
    // my-view plots the spotter (who hears me); the All view plots
    // the heard SENDER (payload sl = sender locator), with the
    // reporting station kept for hover detail.
    QString plottedCall = receiverCall.toUpper();
    // [gridfine] Raw payload locators go through the same refinement
    // as every on-air source — a station whose PSKR-REGISTERED
    // locator is 4-char must not re-degrade a precise position on
    // every report it files (field 2026-08-15: KJ7VWV DN52 vs
    // DN52QT, my-view spot re-clobbered per report).
    QString plottedGrid = refinedGrid(plottedCall, receiverGrid);
    QString heardBy;
    if (!senderIsMe) {
        QString const senderGrid = o.value(QStringLiteral("sl")).toString();
        if (senderGrid.size() < 4) {
            ++m_skippedSpots;
            return;
        }
        plottedCall = sender.toUpper();
        plottedGrid = refinedGrid(plottedCall, senderGrid); // [gridfine]
        heardBy = receiverCall.toUpper();
        // Country of the plotted (sender) station for hover.
        country = m_countryLookup ? m_countryLookup(plottedCall)
                                  : QString();
    }
    // [connlines] Reporting station's projection for the Connections
    // overlay (All-view spots; my-view lines just radiate from the
    // chart center, no extra data needed).
    float heardByAz = 0.0f;
    float heardByDist = -1.0f;
    QString const reporterGrid =
        refinedGrid(receiverCall.toUpper(), receiverGrid); // [gridfine]
    if (!heardBy.isEmpty() && reporterGrid.size() >= 4) {
        if (auto const rvec = Geodesic::vector(m_myGrid, reporterGrid);
            rvec.azimuth().isValid() && rvec.distance().isValid()) {
            heardByAz = rvec.azimuth();
            heardByDist = rvec.distance();
        }
    }

    auto const vec = Geodesic::vector(m_myGrid, plottedGrid);
    if (!vec.azimuth().isValid() || !vec.distance().isValid()) {
        ++m_skippedSpots;
        return;
    }

    Spot spot;
    spot.pskr = true;             // [pskrtoggle]
    spot.reportsMe = senderIsMe;  // [allsuper] my-view = reporter of me
    spot.receiverCall = plottedCall;
    spot.receiverGrid = plottedGrid;
    spot.country = country;
    spot.heardBy = heardBy;
    spot.heardByAz = heardByAz;
    spot.heardByDist = heardByDist;
    if (!heardBy.isEmpty())
        spot.heardByGrid = reporterGrid; // [mondots] for the hover
    spot.freqHz = spotFreqHz;
    spot.snr = snr;
    spot.azimuth = vec.azimuth();
    spot.distance = vec.distance(); // km (display conversion at paint)

    // Report time: payload epoch clamped to now; arrival time if absent.
    auto const now = DriftingDateTime::currentDateTimeUtc();
    if (qint64 const t =
            static_cast<qint64>(o.value(QStringLiteral("t")).toDouble(0));
        t > 0) {
        spot.when = QDateTime::fromSecsSinceEpoch(t, Qt::UTC);
        if (spot.when > now)
            spot.when = now;
    } else {
        spot.when = now;
        // [tstamp] Arrival-stamped spot (payload carried no 't').
        // Suspect class for the 2026-08-14 anomaly: stale EU spots of
        // us showing as fresh with no recent TX — a broker (re)connect
        // delivering retained/old messages would stamp them 'now'
        // here. Warn-level so the next occurrence is attributable.
        qCWarning(mqttclient_js8)
            << "[SPOTMAP] spot WITHOUT timestamp, stamped now:"
            << "sender=" << sender << "reporter=" << receiverCall
            << "band=" << band << "state=" << m_stateText;
    }

    // Latest-per-plotted-station (operator choice): replace any
    // existing spot of the same station in this band. My-view keys
    // by spotter; the All view keys by heard sender.
    auto &spots =
        senderIsMe ? m_spotsByBand[band] : m_allSpotsByBand[band];
    // [pskrtoggle] On-air provenance SURVIVES the replace: if the
    // station's existing spot was radio-evidenced, the merged spot
    // stays visible with PSKR display off (field 2026-08-15:
    // KJ7VWV's fresh on-air spot clobbered by his own PSKR report
    // → station vanished from the my-view inside its window).
    for (Spot const &old : spots)
        if (old.receiverCall == spot.receiverCall) {
            if (!old.pskr)
                spot.pskr = false;
            break;
        }
    spots.erase(std::remove_if(spots.begin(), spots.end(),
                               [&](Spot const &s) {
                                   return s.receiverCall ==
                                          spot.receiverCall;
                               }),
                spots.end());
    spots.append(spot);
    pruneBand(band);

    // Repaint only when the affected dataset is the one on screen.
    if (band == m_currentBand && isVisible() && senderIsMe != m_viewAll)
        requestReplot();
}

void SpotMapWindow::pruneBand(QString const &band) {
    auto const cutoff =
        DriftingDateTime::currentDateTimeUtc().addSecs(-WINDOW_SECS);
    auto const age = [&](QVector<Spot> &spots) {
        spots.erase(std::remove_if(spots.begin(), spots.end(),
                                   [&](Spot const &s) {
                                       return s.when < cutoff;
                                   }),
                    spots.end());
    };
    age(m_spotsByBand[band]);
    age(m_allSpotsByBand[band]); // [viewall]
    // [hearlines] Per-edge aging; empty hearers drop out.
    auto &hearers = m_hearingByBand[band];
    for (auto h = hearers.begin(); h != hearers.end();) {
        auto &heard = h.value().heard;
        for (auto ed = heard.begin(); ed != heard.end();) {
            if (ed.value().when < cutoff)
                ed = heard.erase(ed);
            else
                ++ed;
        }
        if (heard.isEmpty() && h.value().lastSeen < cutoff)
            h = hearers.erase(h);
        else
            ++h;
    }
}

void SpotMapWindow::onPruneTick() {
    for (auto it = m_spotsByBand.begin(); it != m_spotsByBand.end(); ++it)
        pruneBand(it.key());
    // [viewall] Bands that only ever saw other senders' spots.
    for (auto it = m_allSpotsByBand.begin(); it != m_allSpotsByBand.end();
         ++it)
        if (!m_spotsByBand.contains(it.key()))
            pruneBand(it.key());
    // [maptick] Repaint on every 30 s tick while visible (was: only
    // when pruning removed something) — keeps the "last X of Y min"
    // counter, the age fade, and the view-window filter current on a
    // quiet band instead of freezing until the next spot arrives.
    if (isVisible())
        requestReplot();
}

// -------------------------------------------------------------------------
// Geographic background — Natural Earth outlines (SpotMapGeoData),
// projected azimuthal-equidistant around my grid, cached until the
// center, scale, or chart geometry changes.
// -------------------------------------------------------------------------

namespace {
constexpr double EARTH_RADIUS_KM = 6371.0;

// Maidenhead grid (4/6 chars) to lat/lon, center-of-square — matches
// the convention Geodesic uses internally (which isn't exported).
bool gridToLatLon(QString const &grid, double &lat, double &lon) {
    QString const g = grid.toUpper();
    if (g.size() < 4 || !g.at(0).isLetter() || !g.at(1).isLetter() ||
        !g.at(2).isDigit() || !g.at(3).isDigit())
        return false;
    lon = (g.at(0).toLatin1() - 'A') * 20.0 - 180.0 +
          (g.at(2).toLatin1() - '0') * 2.0;
    lat = (g.at(1).toLatin1() - 'A') * 10.0 - 90.0 +
          (g.at(3).toLatin1() - '0') * 1.0;
    if (g.size() >= 6 && g.at(4).isLetter() && g.at(5).isLetter()) {
        lon += (g.at(4).toLatin1() - 'A') * (2.0 / 24.0) + (1.0 / 24.0);
        lat += (g.at(5).toLatin1() - 'A') * (1.0 / 24.0) + (0.5 / 24.0);
    } else {
        lon += 1.0;
        lat += 0.5;
    }
    return true;
}
// True when the grid falls inside US territory (CONUS, Alaska,
// Hawaii, by lat/lon bounding boxes). Drives the map's distance
// units: US stations think in miles, everyone else gets km
// (operator directive 2026-07-14) — independent of the global
// units setting.
} // namespace

void SpotMapWindow::rebuildMapCache(float const R, float const scaleKm,
                                    float const cutKm) {
    if (m_mapCacheGrid == m_myGrid && m_mapCacheScale == scaleKm &&
        m_mapCacheR == R && m_mapCacheCut == cutKm)
        return;
    m_mapCache.clear();
    m_mapCacheGrid = m_myGrid;
    m_mapCacheScale = scaleKm;
    m_mapCacheR = R;
    m_mapCacheCut = cutKm;

    double lat0 = 0.0, lon0 = 0.0;
    if (!gridToLatLon(m_myGrid, lat0, lon0))
        return;
    double const p1 = lat0 * DEG2RAD;
    double const l1 = lon0 * DEG2RAD;
    double const sinP1 = std::sin(p1), cosP1 = std::cos(p1);

    // Segments whose endpoints are beyond the visible extent are
    // dropped — the caller derives cutKm from the panned viewport;
    // the cap in redraw() sidesteps the antipodal blow-up inherent
    // to the azimuthal projection.

    for (int i = 0; i < kGeoPolylineCount; ++i) {
        QPolygonF run;
        for (int j = kGeoPolylineStart[i]; j < kGeoPolylineStart[i + 1];
             ++j) {
            double const p2 = (kGeoPoints[j][0] / 100.0) * DEG2RAD;
            double const l2 = (kGeoPoints[j][1] / 100.0) * DEG2RAD;
            double const dl = l2 - l1;
            double const cosC = std::clamp(
                sinP1 * std::sin(p2) +
                    cosP1 * std::cos(p2) * std::cos(dl),
                -1.0, 1.0);
            double const distKm = EARTH_RADIUS_KM * std::acos(cosC);
            if (distKm > cutKm) {
                if (run.size() > 1)
                    m_mapCache.append(run);
                run.clear();
                continue;
            }
            double const az = std::atan2(
                std::sin(dl) * std::cos(p2),
                cosP1 * std::sin(p2) - sinP1 * std::cos(p2) * std::cos(dl));
            double const r = R * distKm / scaleKm;
            run.append(QPointF{std::sin(az) * r, -std::cos(az) * r});
        }
        if (run.size() > 1)
            m_mapCache.append(run);
    }
}

// -------------------------------------------------------------------------
// Rendering — CPlotter pattern: draw into m_pixmap off the paint path,
// then update(); paintEvent only composites.
// -------------------------------------------------------------------------

void SpotMapWindow::requestReplot() {
    if (!m_replotTimer.isActive())
        m_replotTimer.start();
}

QColor SpotMapWindow::snrColor(int const snr, float const alphaScale) {
    // Fixed range (operator choice): -25 dB deep blue (hue 240) through
    // yellow (~60) at 0 dB to red (hue 0) at +10 dB, clamped.
    float const t =
        std::clamp(static_cast<float>(snr - SNR_COLD) / (SNR_HOT - SNR_COLD),
                   0.0f, 1.0f);
    int const hue = static_cast<int>(240.0f * (1.0f - t));
    QColor c = QColor::fromHsv(hue, 255, 255);
    c.setAlphaF(std::clamp(alphaScale, 0.0f, 1.0f));
    return c;
}

float SpotMapWindow::niceCeil(float const value) {
    // Fine-grained auto-zoom steps (operator feedback 2026-07-14:
    // 1/2/5 rounding left the whole US at half-width — a 2600 km
    // spread jumped to a 5000 km scale).
    if (value <= 0.0f)
        return DEFAULT_SCALE_KM;
    float const mag = std::pow(10.0f, std::floor(std::log10(value)));
    for (float const mult : {1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f,
                             5.0f, 6.0f, 8.0f, 10.0f})
        if (value <= mag * mult)
            return mag * mult;
    return mag * 10.0f;
}

float SpotMapWindow::stepScale(float const scale, int const dir) {
    // One step up (+1) or down (−1) the same ladder niceCeil uses, so
    // manual zoom lands on the same scale values auto-zoom produces.
    static constexpr float mults[] = {1.0f, 1.25f, 1.5f, 2.0f, 2.5f,
                                      3.0f, 4.0f,  5.0f, 6.0f, 8.0f};
    constexpr int n = static_cast<int>(std::size(mults));
    float mag = std::pow(10.0f, std::floor(std::log10(scale)));
    float const ratio = scale / mag; // [1, 10)
    int idx = 0;
    float bestErr = 1e9f;
    for (int i = 0; i < n; ++i) {
        if (float const e = std::fabs(mults[i] - ratio); e < bestErr) {
            bestErr = e;
            idx = i;
        }
    }
    idx += dir;
    if (idx < 0) {
        idx = n - 1;
        mag /= 10.0f;
    } else if (idx >= n) {
        idx = 0;
        mag *= 10.0f;
    }
    return std::clamp(mults[idx] * mag, FLOOR_SCALE_KM, MAX_SCALE_KM);
}

void SpotMapWindow::zoomIn() {
    m_manualScaleKm = stepScale(
        m_lastScaleKm > 0.0f ? m_lastScaleKm : DEFAULT_SCALE_KM, -1);
    m_zoomAutoBtn->setEnabled(true);
    requestReplot();
}

void SpotMapWindow::zoomOut() {
    m_manualScaleKm = stepScale(
        m_lastScaleKm > 0.0f ? m_lastScaleKm : DEFAULT_SCALE_KM, +1);
    m_zoomAutoBtn->setEnabled(true);
    requestReplot();
}

void SpotMapWindow::zoomAuto() {
    m_manualScaleKm = 0.0f;
    m_panPx = QPointF{}; // recenter — Auto = fit all, centered
    m_zoomAutoBtn->setEnabled(false);
    requestReplot();
}

void SpotMapWindow::redraw() {
    QSize const sz = size() * devicePixelRatio();
    if (sz.isEmpty())
        return;
    m_pixmap = QPixmap{sz};
    m_pixmap.setDevicePixelRatio(devicePixelRatio());
    m_pixmap.fill(QColor(16, 16, 24)); // near-black chart background

    QPainter p{&m_pixmap};
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    int const w = width();
    int const h = height();
    QPointF const center =
        QPointF{w / 2.0,
                TITLE_STRIP_PX +
                    (h - TITLE_STRIP_PX - LEGEND_STRIP_PX) / 2.0} +
        m_panPx;
    float const R =
        std::min(w / 2.0, (h - TITLE_STRIP_PX - LEGEND_STRIP_PX) / 2.0) -
        MARGIN_PX;
    if (R < 40.0f)
        return;

    // [units] Distance units follow the Settings selection (was:
    // miles-if-grid-in-North-America). configRefresh() repaints on
    // settings accept so a units change shows immediately.
    bool const miles = m_config->miles();
    float const unitScale = miles ? 0.621371f : 1.0f;
    QString const unitLabel = miles ? tr("mi") : tr("km");

    auto const now = DriftingDateTime::currentDateTimeUtc();
    // [spotwin] Storage holds the full hour; the 15/30/60 buttons
    // pick how much of it renders. Everything below (auto-scale,
    // counts, dots, hover) operates on the filtered view.
    QVector<Spot> visible;
    {
        auto const cutoff = now.addSecs(-m_viewWindowSecs);
        // [viewall] Dataset per the view buttons: my spotters, or
        // every heard station on the band.
        auto const &source = m_viewAll ? m_allSpotsByBand : m_spotsByBand;
        // [posauth 2026-08-15] The hearing store is the ONE position
        // source: when it has placed a station, its grid/az/dist
        // override the spot's own — the two views can no longer
        // disagree about where a station sits (4 fixes failed while
        // position lived in two models).
        auto const &posAuth = m_hearingByBand.value(m_currentBand);
        for (Spot s : source.value(m_currentBand)) {
            if (s.when < cutoff || !(m_showPskr || !s.pskr))
                continue;
            if (auto const it = posAuth.constFind(s.receiverCall);
                it != posAuth.constEnd() && it->dist >= 0.0f) {
                s.receiverGrid = it->grid;
                s.azimuth = it->az;
                s.distance = it->dist;
            }
            visible.append(s);
        }
        // [allsuper 2026-08-15] The All view is a SUPERSET of the
        // my view by construction: stations known only as reporters
        // of MY signal (PSKR rc / on-air spotters) get their my-view
        // spot appended unless already plotted as senders (field:
        // KN6ZOM, W7YSB/7 on the my map, absent from All).
        if (m_viewAll) {
            QSet<QString> present;
            for (Spot const &s : visible)
                present.insert(s.receiverCall);
            for (Spot s : m_spotsByBand.value(m_currentBand)) {
                if (s.when < cutoff || !(m_showPskr || !s.pskr) ||
                    present.contains(s.receiverCall))
                    continue;
                if (auto const it = posAuth.constFind(s.receiverCall);
                    it != posAuth.constEnd() && it->dist >= 0.0f) {
                    s.receiverGrid = it->grid;
                    s.azimuth = it->az;
                    s.distance = it->dist;
                }
                present.insert(s.receiverCall);
                visible.append(s);
            }
        }
    }
    // [relaykeep] Selected relay hops are pinned to the map while
    // relay-select is active: any hop whose live dot aged out of the
    // view window (or the window selection shrank) is re-added from
    // its click-time snapshot. Removed only by Undo / exiting the
    // mode / band change.
    if (m_relaySelect) {
        QSet<QString> present;
        for (Spot const &s : visible)
            present.insert(s.receiverCall);
        for (Spot const &snap : m_relayPathSpots)
            if (!present.contains(snap.receiverCall)) {
                visible.append(snap);
                present.insert(snap.receiverCall);
            }
    }

    // [mondots] All view: reporters that never transmitted on the
    // band (pure monitors) still deserve a dot — we have their call
    // and grid from the reports naming them (operator, 2026-08-14).
    // Newest report per monitor; skip calls already plotted as
    // senders. Gives every connection line a real anchor and makes
    // monitors hoverable / relay-selectable.
    if (m_viewAll && !visible.isEmpty()) {
        QSet<QString> senderCalls;
        for (Spot const &s : visible)
            senderCalls.insert(s.receiverCall);
        QHash<QString, Spot> monitors;
        for (Spot const &s : visible) {
            if (s.heardBy.isEmpty() || s.heardByDist < 0.0f ||
                senderCalls.contains(s.heardBy))
                continue;
            auto it = monitors.find(s.heardBy);
            if (it == monitors.end() || s.when > it->when) {
                Spot m;
                m.when = s.when;
                m.receiverCall = s.heardBy;
                m.receiverGrid = s.heardByGrid;
                m.azimuth = s.heardByAz;
                m.distance = s.heardByDist;
                m.monitorOnly = true;
                m.rxOnly = true; // genuinely receive-only
                monitors.insert(s.heardBy, m);
            }
        }
        for (Spot const &m : monitors)
            visible.append(m);
    }

    // [selfhop] My own station is the triangle, never a dot: no
    // own-call spot from ANY source (MQTT echoes of my signal,
    // mondots synthesis, on-air) may reach the render or hit-test
    // path — nothing clickable exists at my position by
    // construction.
    visible.erase(std::remove_if(visible.begin(), visible.end(),
                                 [this](Spot const &s) {
                                     return s.receiverCall.compare(
                                                m_myCall,
                                                Qt::CaseInsensitive) == 0;
                                 }),
                  visible.end());

    // Auto-scale: outer ring just past the farthest live spot (nice
    // 1/2/5 value), floored so a lone close-in spot isn't degenerate.
    // Manual zoom (m_manualScaleKm > 0, +/Auto/− buttons) overrides;
    // spots beyond a manual scale clamp to the outer ring below.
    float maxDist = 0.0f;
    for (Spot const &s : visible)
        maxDist = std::max(maxDist, s.distance);
    // [onairscale] On-air stations render as anchors AFTER the scale
    // is fixed, so auto-zoom must count their distances here or they
    // clip off-ring (nomqtt field 2026-08-15).
    {
        auto const cutoffH = now.addSecs(-m_viewWindowSecs);
        auto const &hz = m_hearingByBand.value(m_currentBand);
        QString const myUpS = m_myCall.toUpper();
        for (auto h = hz.constBegin(); h != hz.constEnd(); ++h) {
            // [posauth] MY view fits only stations that hear ME.
            bool const hearsMe =
                h.value().snr > -99 || h.value().heard.contains(myUpS);
            if (!m_viewAll && !hearsMe)
                continue;
            if (h.value().lastSeen.isValid() &&
                h.value().lastSeen >= cutoffH && h.value().dist > 0.0f)
                maxDist = std::max(maxDist, h.value().dist);
            if (!m_viewAll)
                continue; // heard-endpoints are the All view's business
            for (auto ed = h.value().heard.constBegin();
                 ed != h.value().heard.constEnd(); ++ed)
                if (ed.value().when >= cutoffH && ed.value().dist > 0.0f)
                    maxDist = std::max(maxDist, ed.value().dist);
        }
    }
    // [autofit 2026-08-15] Key on maxDist, not the spot dataset:
    // on-air stations (presence/hearing entries) contribute distances
    // without being in `visible`, and Auto must fit ALL of them —
    // with only on-air data the old visible.isEmpty() guard snapped
    // to the 5000 km default and ignored every heard-by-me station.
    float const autoScaleKm =
        maxDist <= 0.0f ? DEFAULT_SCALE_KM
                        : std::max(niceCeil(maxDist), FLOOR_SCALE_KM);
    float const scaleKm =
        m_manualScaleKm > 0.0f ? m_manualScaleKm : autoScaleKm;
    m_lastScaleKm = scaleKm;

    QFont small = p.font();
    small.setPointSize(8);
    p.setFont(small);

    // Geographic background: country/coast outlines across the whole
    // visible viewport (pan can carry any part of the chart into
    // view, so the old clip-to-circle is gone — the boundary ring
    // overlays as the scale reference). The cut extends to the
    // farthest visible corner, quantized to the nice ladder so drags
    // only rebuild the cache when crossing a step; capped short of
    // the antipode where the projection blows up.
    double maxCornerPx = 0.0;
    for (QPointF const &corner :
         {QPointF{0, 0}, QPointF{static_cast<qreal>(w), 0},
          QPointF{0, static_cast<qreal>(h)},
          QPointF{static_cast<qreal>(w), static_cast<qreal>(h)}}) {
        maxCornerPx = std::max(maxCornerPx,
                               std::hypot(corner.x() - center.x(),
                                          corner.y() - center.y()));
    }
    float const cutKm = std::min(
        niceCeil(scaleKm * static_cast<float>(maxCornerPx) / R * 1.05f),
        19000.0f);
    rebuildMapCache(R, scaleKm, cutKm);
    p.save();
    p.translate(center);
    p.setPen(QPen{QColor(70, 100, 80), 1}); // muted land outline
    for (QPolygonF const &poly : m_mapCache)
        p.drawPolyline(poly);
    p.restore();

    // [scalebar 2026-08-15] Outer boundary circle, azimuth ticks and
    // compass dropped (operator) — scale now reads from the bar above
    // the SNR legend.

    // Distance rings: on-map option, default off (operator choice;
    // toggle UI to follow).
    if (m_showRings) {
        p.setPen(QPen{QColor(70, 70, 90), 1});
        for (int i = 1; i <= 3; ++i) {
            float const r = R * i / 4.0f;
            p.drawEllipse(center, r, r);
            float const ringVal = scaleKm * i / 4.0f * unitScale;
            p.setPen(QColor(140, 140, 160));
            p.drawText(QPointF{center.x() + 4,
                               center.y() - r + p.fontMetrics().ascent() + 1},
                       QStringLiteral("%1 %2")
                           .arg(qRound(ringVal))
                           .arg(unitLabel));
            p.setPen(QPen{QColor(70, 70, 90), 1});
        }
    }
    // Outer-ring scale label always shown (the map needs SOME scale cue
    // even with rings off).
    // [units] In miles, label the km zoom ladder with the operator's
    // nice-number equivalents (operator table, 2026-08-14) instead of
    // the raw conversion (500 km -> "300 mi", not "311 mi"). The
    // ladder steps themselves are unchanged. Falls back to the raw
    // conversion for any off-ladder scale.
    auto const scaleLabelValue = [&]() -> int {
        if (!miles)
            return qRound(scaleKm);
        struct KmMi { int km; int mi; };
        static constexpr KmMi table[] = {
            {500, 300},    {600, 375},    {800, 500},   {1000, 625},
            {1250, 775},   {1500, 925},   {2000, 1250}, {2500, 1550},
            {3000, 1850},  {4000, 2500},  {5000, 3100}, {6000, 3700},
            {8000, 5000},  {10000, 6200}, {12500, 7700},
            {15000, 9300}, {20000, 12400}};
        for (auto const &e : table)
            if (std::fabs(scaleKm - e.km) <= e.km * 0.01f)
                return e.mi;
        return qRound(scaleKm * 0.621371f);
    };

    // Center marker (my station).
    // [hometri] My station: small point-up triangle (TODO #145) —
    // visually distinct from spot dots at any zoom, and the anchor
    // the relay path grows from.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(230, 230, 240));
    {
        QPolygonF tri; // smaller (operator, 2026-08-14)
        tri << center + QPointF{0.0, -4.5} << center + QPointF{3.8, 3.0}
            << center + QPointF{-3.8, 3.0};
        p.drawPolygon(tri);
    }

    // Spots: oldest first so the newest draw on top. Heat blobs blend
    // additively (operator choice); age fades alpha 1.0 -> 0.5 across
    // the 15-minute window.
    QVector<Spot> ordered = visible;
    std::sort(ordered.begin(), ordered.end(),
              [](Spot const &a, Spot const &b) { return a.when < b.when; });

    auto const project = [&](float az, float dist) {
        double const rad = az * DEG2RAD;
        return center + QPointF{std::sin(rad), -std::cos(rad)} *
                            (R * (dist / scaleKm));
    };
    // Screen position of every plotted station this frame, by call —
    // shared by the Connections overlay (lines must END on a visible
    // dot; operator 2026-08-14: no lines to nowhere) and the relay
    // path.
    QHash<QString, QPointF> posByCall;
    for (Spot const &s : ordered)
        posByCall.insert(s.receiverCall, project(s.azimuth, s.distance));
    // [selfhop] My own position is always the triangle at center —
    // registered here (not as a spot) so edges from/to me anchor
    // even in a pure-MQTT session with no hearing-store entry.
    if (!m_myCall.isEmpty())
        posByCall.insert(m_myCall, center);

    // [connlines] Connections overlay, drawn UNDER the dots: 1 px
    // light green lines between stations hearing each other. My view:
    // every spotter hears ME, so lines radiate from the chart center.
    // All view: each heard sender connects to its reporter — but ONLY
    // when the reporter is itself a plotted dot (both endpoints
    // visible). Stays visible during relay-select (operator revision
    // 2026-08-14) — the red relay path reads over the green mesh.
    // [hearlines] On-air heard-mesh stations: plotted in the All
    // view ALWAYS (operator 2026-08-14 — not gated on Connections;
    // stations that never report to PSK Reporter appear only here).
    // Heat-colored solid dot when WE have decoded them (our own
    // last-heard SNR); hollow gray only when position is relay-
    // learned and we've never copied them ourselves.
    {
        auto const cutoffH = now.addSecs(-m_viewWindowSecs);
        auto const &hearers = m_hearingByBand.value(m_currentBand);
        // [posauth 2026-08-15] The store renders in BOTH views now:
        // All = everything; MY = stations that hear me (field:
        // KL7UT/W7LPN/KN6OEH heard me on-air yet appeared only in
        // All — their evidence lives solely in the hearing store,
        // whose dot pass was All-gated).
        // [hbdots] On-air station dots. Operator rules 2026-08-14:
        //  - every on-air sender with a resolvable grid (heartbeats
        //    carry theirs in the message) gets a dot — hollow;
        //  - the dot is SOLID heat-colored ONLY from an SNR value the
        //    station reported TO US (its copy of our signal). Our own
        //    decode SNR of them implies nothing here.
        auto const addAnchor = [&](QString const &call, float az,
                                   float dist, QString const &grid,
                                   int snr, QDateTime const &when) {
            // [selfhop] I am the triangle, not a dot — filtered from
            // `visible` above; guard here too so no anchor spot can
            // reintroduce one.
            if (call.compare(m_myCall, Qt::CaseInsensitive) == 0)
                return;
            if (dist < 0.0f || posByCall.contains(call))
                return;
            Spot m;
            m.when = when;
            m.receiverCall = call;
            m.receiverGrid = grid;
            m.azimuth = az;
            m.distance = dist;
            // [snrwho] Keep the -99 no-report sentinel — collapsing
            // to 0 made hover claim "hears me at 0 dB" for stations
            // that never reported (KB7ITU, 2026-08-15). Paint is
            // safe: monitorOnly renders hollow.
            m.snr = snr;
            m.monitorOnly = (snr <= -99);
            ordered.append(m);
            posByCall.insert(call, project(az, dist));
        };
        QString const myUpA = m_myCall.toUpper();
        for (auto h = hearers.constBegin(); h != hearers.constEnd(); ++h) {
            bool const hearsMe = h.value().snr > -99 ||
                                 h.value().heard.contains(myUpA);
            if (!m_viewAll && !hearsMe)
                continue; // MY view: hearers of me only
            if (h.value().lastSeen.isValid() &&
                h.value().lastSeen >= cutoffH)
                addAnchor(h.key(), h.value().az, h.value().dist,
                          h.value().grid, h.value().snr,
                          h.value().lastSeen);
            for (auto ed = h.value().heard.constBegin();
                 ed != h.value().heard.constEnd(); ++ed) {
                if (ed.value().when < cutoffH)
                    continue;
                addAnchor(h.key(), h.value().az, h.value().dist,
                          h.value().grid, h.value().snr,
                          ed.value().when);
                if (m_viewAll) // heard-endpoints: All view only
                    addAnchor(ed.key(), ed.value().az, ed.value().dist,
                              ed.value().grid, -99, ed.value().when);
            }
        }
    }

    if (m_showConnections) {
        // [heararrow] Mid-line arrowhead POINTING AT THE HEARING
        // station. Operator rules 2026-08-15: heads on the ALL map
        // only, and only on lines that connect to ME (either
        // direction), PSKR-sourced included. Head color follows the
        // active pen.
        auto const heardLine = [&p](QPointF const &hearer,
                                    QPointF const &heard) {
            p.drawLine(heard, hearer);
            double const len = QLineF(heard, hearer).length();
            if (len < 24.0)
                return; // too short for a legible head
            double const ang = std::atan2(hearer.y() - heard.y(),
                                          hearer.x() - heard.x());
            QPointF const dir{std::cos(ang), std::sin(ang)};
            double const sz = 8.0; // head length (operator: larger)
            // Tip offset hearer-ward of the midpoint by 3 head
            // lengths: a bidirectional pair (each head drawn by its
            // own edge, each offset toward ITS hearer) lands
            // back-to-back with a 4-head-size space between them
            // (operator 2026-08-15). Short lines center the head.
            double const off = len >= 2.0 * (3.0 * sz) + 6.0
                                   ? 3.0 * sz
                                   : sz * 0.4;
            QPointF const tip = (hearer + heard) / 2.0 + dir * off;
            QPainterPath head(tip);
            head.lineTo(tip - QPointF(std::cos(ang - 0.45),
                                      std::sin(ang - 0.45)) * sz);
            head.lineTo(tip - QPointF(std::cos(ang + 0.45),
                                      std::sin(ang + 0.45)) * sz);
            head.closeSubpath();
            p.save();
            p.setBrush(p.pen().color());
            p.setPen(Qt::NoPen);
            p.drawPath(head);
            p.restore();
        };
        // [pskrline] Dark yellow (operator 2026-08-15; was light
        // green) — distinct from blue on-air mesh and red relay path.
        p.setPen(QPen{QColor(200, 170, 25, 170), 1});
        // [melineall 2026-08-15] "Reports me" is a fact about the
        // STATION, not about whichever spot object owns its dot: a
        // reporter of my signal that also TRANSMITS is plotted from
        // the sender dataset in the All view, and its line to me was
        // lost with the skipped append (field: ND7M/NT5DF/K1KWC
        // lined in my view, bare in All).
        QSet<QString> meReporters;
        if (m_viewAll) {
            auto const cutoffR = now.addSecs(-m_viewWindowSecs);
            for (Spot const &s : m_spotsByBand.value(m_currentBand))
                if (s.when >= cutoffR && (m_showPskr || !s.pskr))
                    meReporters.insert(s.receiverCall);
        }
        for (Spot const &s : ordered) {
            QPointF const from = posByCall.value(s.receiverCall);
            // [connfix 2026-08-14] Branch on the VIEW, not on
            // heardBy-emptiness: synthetic monitor dots have no
            // heardBy and were falling into the my-view branch,
            // drawing phantom center-to-monitor lines ("EU stations
            // with lines to me" — operator report, PSKReporter map
            // confirmed no such links). Monitors draw no line of
            // their own; their edges come from each sender's spot.
            if (!m_viewAll) {
                p.drawLine(center, from); // my view: center = me
                continue;
            }
            if (!s.monitorOnly && posByCall.contains(s.heardBy))
                p.drawLine(from, posByCall.value(s.heardBy));
            // [melineall] Line to me for EVERY station that reported
            // my signal, whatever dataset drew its dot. [heararrow]
            // Connects to me → arrowhead at the hearing reporter.
            if (meReporters.contains(s.receiverCall))
                heardLine(from, center);
        }
        // [hearlines] On-air heard-mesh edges: 1 px BLUE, drawn after
        // (over) the green MQTT mesh — visual priority per operator
        // 2026-08-14. Sources: HEARING replies (incl. relayed
        // "*DE* CALL" form) and every directed exchange.
        // [viewedges 2026-08-15] View-filtered: the MY view shows
        // ONLY edges where the heard station is ME (X heard me →
        // line X to my triangle); third-party edges belong to the
        // All view alone.
        auto const cutoffH = now.addSecs(-m_viewWindowSecs);
        p.setPen(QPen{QColor(90, 160, 255, 210), 1});
        auto const &hearers = m_hearingByBand.value(m_currentBand);
        QString const myUp = m_myCall.toUpper();
        for (auto h = hearers.constBegin(); h != hearers.constEnd(); ++h) {
            for (auto ed = h.value().heard.constBegin();
                 ed != h.value().heard.constEnd(); ++ed) {
                if (ed.value().when < cutoffH)
                    continue;
                if (!m_viewAll) {
                    // MY view: X heard ME → X's dot to my triangle.
                    // [heararrow] No heads here — every line
                    // connects to me by definition.
                    if (ed.key() != myUp ||
                        !posByCall.contains(h.key()))
                        continue;
                    p.drawLine(posByCall.value(h.key()), center);
                } else {
                    if (!posByCall.contains(h.key()) ||
                        !posByCall.contains(ed.key()))
                        continue;
                    // [heararrow] All view: head only when I am an
                    // endpoint; third-party edges stay bare.
                    if (h.key() == myUp || ed.key() == myUp)
                        heardLine(posByCall.value(h.key()),
                                  posByCall.value(ed.key()));
                    else
                        p.drawLine(posByCall.value(h.key()),
                                   posByCall.value(ed.key()));
                }
            }
        }
    }

    // [allhollow] All-view color authority (operator 2026-08-14):
    // a third-party PSKR report's SNR is the sender's signal at the
    // REPORTER's QTH — meaningless here unless WE are the reported
    // sender. In the All view a dot is therefore solid ONLY when the
    // station has REPORTED an SNR TO US (hearing store); everything
    // else renders hollow, position/presence only. (My view keeps
    // reporter SNRs — there, we ARE the reported sender.)
    auto const &colorAuthority = m_hearingByBand.value(m_currentBand);

    m_screenSpots.clear();
    for (Spot const &s : ordered) {
        float const age =
            std::clamp(static_cast<float>(s.when.secsTo(now)) /
                           static_cast<float>(m_viewWindowSecs),
                       0.0f, 1.0f);
        float const alpha = 1.0f - 0.5f * age;
        // TRUE radial position — no outer-ring clamp (pan model:
        // zoomed-in views push far spots off-window, and dragging
        // reaches them at full radial resolution). Painter clips
        // off-pixmap dots; auto scale still covers every spot, so
        // nothing is beyond the ring in Auto anyway.
        float const r = R * (s.distance / scaleKm);
        double const rad = s.azimuth * DEG2RAD;
        QPointF const pos =
            center + QPointF{std::sin(rad), -std::cos(rad)} * r;

        // Tiny solid circle (operator choice), SNR heat color, age
        // fade in the alpha; thin dark outline for contrast on land.
        // [mondots] Receive-only reporters: hollow gray — no SNR of
        // their own to heat-color.
        bool hollow = s.monitorOnly;
        int paintSnr = s.snr;
        if (m_viewAll && !s.reportsMe) {
            // [allhollow] Override per the color-authority rule.
            // [allsuper] reportsMe spots are exempt — their SNR IS
            // a report of my signal, the very thing the authority
            // exists to isolate.
            int const rep =
                colorAuthority.contains(s.receiverCall)
                    ? colorAuthority.value(s.receiverCall).snr
                    : -99;
            hollow = (rep <= -99);
            paintSnr = rep;
        } else if (m_viewAll) {
            hollow = s.monitorOnly || s.snr <= -99;
            paintSnr = s.snr;
        }
        if (hollow) {
            p.setPen(QPen{QColor(170, 170, 185,
                                 static_cast<int>(220 * alpha)), 1});
            p.setBrush(Qt::NoBrush);
        } else {
            p.setPen(QPen{QColor(0, 0, 0, 180), 1});
            p.setBrush(snrColor(paintSnr, alpha));
        }
        p.drawEllipse(pos, DOT_RADIUS_PX, DOT_RADIUS_PX);

        m_screenSpots.append({pos, s});
    }

    // Callsign labels: on-map option, default off (hover tooltip
    // provides identity; toggle UI to follow).
    if (m_showCallsigns) {
        // [callsbtn] White on transparent, to the RIGHT of the dot,
        // clear of it (operator 2026-08-15; both views).
        p.setPen(Qt::white);
        p.setBrush(Qt::NoBrush);
        for (auto const &ss : m_screenSpots) {
            p.drawText(QRectF{ss.pos.x() + DOT_RADIUS_PX + 5.0,
                              ss.pos.y() - 9.0, 140.0, 18.0},
                       Qt::AlignLeft | Qt::AlignVCenter,
                       ss.spot.receiverCall);
        }
    }

    // [relaysel] Relay path on TOP of the dots: my triangle -> hop 1
    // -> hop 2 -> ... Bright green so it reads over the dimmer
    // Connections style; selected stations get a ring. Hops whose
    // station has scrolled out of the visible dataset keep their slot
    // in the path (the template still includes them) but draw no
    // segment.
    if (m_relaySelect && !m_relayPath.isEmpty()) {
        // 2 px red (operator revision 2026-08-14) — distinct from the
        // 1 px green Connections mesh underneath.
        p.setPen(QPen{QColor(235, 60, 60, 230), 2});
        p.setBrush(Qt::NoBrush);
        QPointF prev = center;
        bool prevKnown = true;
        for (QString const &call : m_relayPath) {
            if (!posByCall.contains(call)) {
                prevKnown = false;
                continue;
            }
            QPointF const pos = posByCall.value(call);
            if (prevKnown)
                p.drawLine(prev, pos);
            p.drawEllipse(pos, DOT_RADIUS_PX + 3.0, DOT_RADIUS_PX + 3.0);
            prev = pos;
            prevKnown = true;
        }
    }

    // Title strip (backfilled — pan can slide outlines under it).
    p.fillRect(QRectF{0, 0, static_cast<qreal>(w),
                      static_cast<qreal>(TITLE_STRIP_PX)},
               QColor(16, 16, 24));
    p.setPen(QColor(220, 220, 235));
    QFont title = p.font();
    title.setPointSize(9);
    p.setFont(title);
    QString const bandText =
        m_currentBand.isEmpty() ? tr("no band") : m_currentBand;
    // [spotfmt] "last X of Y min" while accumulation is younger than
    // the selected window; plain "last Y min" once it's full.
    int const viewMin = m_viewWindowSecs / 60;
    qint64 const accumMin =
        m_accumStart.isValid()
            ? std::max<qint64>(1, m_accumStart.secsTo(now) / 60)
            : viewMin;
    QString const windowText =
        accumMin < viewMin
            ? tr("last %1 of %2 min").arg(accumMin).arg(viewMin)
            : tr("last %1 min").arg(viewMin);
    QString headline =
        m_viewAll
            // [viewall] All view counts heard stations, not
            // spotters of me.
            ? tr("All stations — %1 — %2 heard / %3")
                  .arg(bandText)
                  .arg(ordered.size())
                  .arg(windowText)
            : tr("%1 @ %2 — %3 — %4 spotters / %5")
                  .arg(m_myCall, m_myGrid, bandText)
                  .arg(ordered.size()) // [posauth] anchors count too
                  .arg(windowText);
    // [pskrtoggle] The MQTT connection state is only relevant while
    // internet-sourced spots are shown.
    if (m_showPskr && !m_stateText.isEmpty())
        headline += QStringLiteral(" — ") + m_stateText;
    p.drawText(QRectF{0, 0, static_cast<qreal>(w), TITLE_STRIP_PX},
               Qt::AlignCenter, headline);

    if (ordered.isEmpty()) { // [onairscale] anchors count as spots
        p.setPen(QColor(150, 150, 170));
        // One line height below the WINDOW center (not the panned
        // chart center) — dead-center overlapped the own-station
        // marker graphic.
        qreal const drop = p.fontMetrics().height();
        p.drawText(QRectF{0, (center.y() - m_panPx.y()) - 30 + drop,
                          static_cast<qreal>(w), 60},
                   Qt::AlignCenter, tr("No new spots yet"));
    }

    // Legend: SNR gradient bar (strip backfilled, same reason as the
    // title strip).
    {
        p.fillRect(QRectF{0, static_cast<qreal>(h - LEGEND_STRIP_PX),
                          static_cast<qreal>(w),
                          static_cast<qreal>(LEGEND_STRIP_PX)},
                   QColor(16, 16, 24));
        // [scalebar] Bar width = the chart radius in pixels, so the
        // scale legend above it IS the radius length (operator
        // 2026-08-15); SNR bar matches it.
        qreal const barW = static_cast<qreal>(R);
        QRectF bar{(w - barW) / 2.0,
                   static_cast<qreal>(h - LEGEND_STRIP_PX + 8), barW, 10};
        {
            qreal const yLine = h - LEGEND_STRIP_PX - 10.0;
            p.setPen(QPen{QColor(190, 190, 210), 1});
            p.drawLine(QPointF{bar.left(), yLine},
                       QPointF{bar.right(), yLine});
            p.drawLine(QPointF{bar.left(), yLine - 4},
                       QPointF{bar.left(), yLine + 4}); // end ticks
            p.drawLine(QPointF{bar.right(), yLine - 4},
                       QPointF{bar.right(), yLine + 4});
            p.setPen(QColor(190, 190, 210));
            p.drawText(QRectF{bar.left(), yLine - 20.0, barW, 16.0},
                       Qt::AlignHCenter | Qt::AlignBottom,
                       QStringLiteral("%1 %2")
                           .arg(scaleLabelValue())
                           .arg(unitLabel));
        }
        QLinearGradient lg{bar.topLeft(), bar.topRight()};
        for (int i = 0; i <= 10; ++i) {
            float const t = i / 10.0f;
            lg.setColorAt(t, snrColor(SNR_COLD +
                                          static_cast<int>(
                                              t * (SNR_HOT - SNR_COLD)),
                                      1.0f));
        }
        p.fillRect(bar, lg);
        p.setPen(QColor(190, 190, 210));
        p.setFont(small);
        p.drawText(QPointF{bar.left(), bar.bottom() + 14},
                   QStringLiteral("%1 dB").arg(SNR_COLD));
        p.drawText(QPointF{bar.center().x() - 24, bar.bottom() + 14},
                   tr("SNR (dB)"));
        p.drawText(QPointF{bar.right() - 24, bar.bottom() + 14},
                   QStringLiteral("+%1").arg(SNR_HOT));
    }

    // [connlegend] Line-type legend under the Connections button
    // (operator 2026-08-14): blue = on-air heard-mesh, green =
    // PSKReporter MQTT. Solid 1 px swatches, right-aligned; subtle
    // backdrop so the labels read over busy map areas. Shown only
    // while the Connections overlay is on (operator 2026-08-15).
    // [pskrtoggle] Legend shown only with Connections on AND PSKR
    // spots added in (operator 2026-08-15: hide BOTH lines otherwise).
    if (m_showConnections && m_showPskr) {
        QFont lf = p.font();
        lf.setPointSize(8);
        p.setFont(lf);
        struct Row { QColor c; QString label; };
        Row const rows[2] = {
            {QColor(90, 160, 255),
             tr("Heard by %1").arg(m_myCall.isEmpty() ? tr("me")
                                                      : m_myCall)},
            {QColor(200, 170, 25), tr("Heard by PSKR")}};
        int const rowH = 11;
        qreal const ascent = p.fontMetrics().ascent();
        qreal maxW = 0;
        for (Row const &r : rows)
            maxW = std::max(
                maxW, static_cast<qreal>(
                          p.fontMetrics().horizontalAdvance(r.label)));
        qreal const xRight = w - 6.0;
        // One font height lower (operator, 2026-08-14) — the right
        // side of the bottom strip is empty (gradient bar is
        // centered), so the second row may ride into it.
        qreal const y0 = h - LEGEND_STRIP_PX - rowH - 2;
        p.fillRect(QRectF{xRight - maxW - 28, y0 - 2, maxW + 28 + 4,
                          2.0 * rowH + 4},
                   QColor(16, 16, 24, 170));
        for (int i = 0; i < 2; ++i) {
            qreal const y = y0 + i * rowH;
            qreal const xText = xRight - maxW;
            p.setPen(QColor(205, 205, 220));
            p.drawText(QPointF{xText, y + ascent - 1}, rows[i].label);
            p.setPen(QPen{rows[i].c, 1});
            qreal const ymid = y + rowH / 2.0 - 1;
            p.drawLine(QPointF{xText - 24, ymid},
                       QPointF{xText - 6, ymid});
        }
    }

    update();
}

void SpotMapWindow::paintEvent(QPaintEvent *) {
    QPainter p{this};
    p.drawPixmap(0, 0, m_pixmap);
}

void SpotMapWindow::positionWindowButtons() {
    if (!m_win15Btn) return;
    int const x = width() - 36 - 6; // right-aligned, mirrors zoom stack
    int y = TITLE_STRIP_PX + 6;
    for (QToolButton *b :
         {m_win5Btn, m_win15Btn, m_win30Btn, m_win60Btn}) {
        b->move(x, y);
        y += 22;
    }
    // [viewall] View-selector stack, lower-left: my call over "All",
    // bottom edge 8 px clear of the legend strip (which also carries
    // the status message). Width grows to fit the callsign (e.g.
    // "WM8Q/P" outgrows the 36 px zoom-button width); both buttons
    // share the wider size so the stack stays a clean column.
    if (m_viewMineBtn) {
        QString const callText = m_myCall.isEmpty()
            ? tr("Show stations hearing me")
            : tr("Show stations hearing %1").arg(m_myCall);
        QString const allText = tr("Show all stations");
        m_viewMineBtn->setText(callText);
        m_viewAllBtn->setText(allText);
        // Width from the buttons' OWN sizeHint — the style knows
        // its true padding; hand-added fontMetrics constants don't.
        int const wBtn = std::max(
            {36, m_viewMineBtn->sizeHint().width(),
             m_viewAllBtn->sizeHint().width(),
             m_pskrBtn ? m_pskrBtn->sizeHint().width() : 0});
        m_viewMineBtn->setFixedSize(wBtn, 20);
        m_viewAllBtn->setFixedSize(wBtn, 20);
        // One font height (~16 px) above the old position so the
        // bottom-center toast can't overlap the stack (operator,
        // 2026-08-14).
        // [pskrtoggle] Third row: one button height (20 px) of air
        // below the view pair — separate function group.
        int vy = height() - LEGEND_STRIP_PX - 24 - 42 - 42;
        for (QToolButton *b : {m_viewMineBtn, m_viewAllBtn}) {
            b->move(6, vy);
            vy += 22;
        }
        if (m_pskrBtn) {
            m_pskrBtn->setFixedSize(wBtn, 20); // same length as pair
            m_pskrBtn->move(6, vy + 20);
        }
    }
    // [connlines] Lower right, bottom-aligned with the view stack;
    // [relaysel] the relay rows stack directly above it.
    if (m_connBtn) {
        // One shared width for the right-hand column (operator
        // 2026-08-15): Show connections, Show call signs,
        // Select relay(s) — widest sizeHint wins.
        int wConn = std::max(36, m_connBtn->sizeHint().width());
        if (m_callsBtn)
            wConn = std::max(wConn, m_callsBtn->sizeHint().width());
        if (m_relaySelBtn)
            wConn = std::max(wConn, m_relaySelBtn->sizeHint().width());
        int const yConn = height() - LEGEND_STRIP_PX - 24 - 20;
        m_connBtn->setFixedSize(wConn, 20);
        m_connBtn->move(width() - wConn - 6, yConn);
        // [callsbtn] Directly above Show connections.
        if (m_callsBtn) {
            m_callsBtn->setFixedSize(wConn, 20);
            m_callsBtn->move(width() - wConn - 6, yConn - 22);
        }
        if (m_relaySelBtn) {
            // Row 2 (Done | Undo) shares the column width.
            int const half = (wConn - 4) / 2;
            m_relayDoneBtn->setFixedSize(half, 20);
            m_relayUndoBtn->setFixedSize(wConn - 4 - half, 20);
            // Relay rows lifted one button height above the calls
            // button (operator 2026-08-15) — separate function group.
            int const yRow2 = yConn - 66;
            m_relayDoneBtn->move(width() - wConn - 6, yRow2);
            m_relayUndoBtn->move(width() - wConn - 6 + half + 4, yRow2);
            m_relaySelBtn->setFixedSize(wConn, 20);
            m_relaySelBtn->move(width() - wConn - 6, yRow2 - 22);
        }
    }
}

void SpotMapWindow::updateRelayButtons() {
    if (!m_relayDoneBtn) return;
    // A relay path needs at least one hop AND a destination — Done
    // stays disabled until two stations are clicked.
    m_relayDoneBtn->setEnabled(m_relaySelect && m_relayPath.size() >= 2);
    m_relayUndoBtn->setEnabled(m_relaySelect && !m_relayPath.isEmpty());
}

void SpotMapWindow::resizeEvent(QResizeEvent *) {
    positionWindowButtons();
    requestReplot();
}

SpotMapWindow::ScreenSpot const *
SpotMapWindow::hitTest(QPointF const &pos) const {
    ScreenSpot const *best = nullptr;
    double bestD2 = 12.0 * 12.0;
    for (auto const &ss : m_screenSpots) {
        double const dx = ss.pos.x() - pos.x();
        double const dy = ss.pos.y() - pos.y();
        if (double const d2 = dx * dx + dy * dy; d2 < bestD2) {
            bestD2 = d2;
            best = &ss;
        }
    }
    return best;
}

void SpotMapWindow::mouseMoveEvent(QMouseEvent *event) {
    // Drag-to-pan: once the press moves past the drag threshold it's
    // a pan, not a click. Direct redraw() (not the debounced
    // requestReplot) so the chart tracks the cursor smoothly; the
    // outline cache is center-relative so panning never re-projects.
    if (m_maybeDrag && (event->buttons() & Qt::LeftButton)) {
        QPointF const d = event->position() - m_pressPos;
        if (!m_dragging &&
            d.manhattanLength() >= QApplication::startDragDistance()) {
            m_dragging = true;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_dragging) {
            m_panPx = m_panAtPress + d;
            m_zoomAutoBtn->setEnabled(true); // Auto now recenters too
            redraw();
            return;
        }
    }
    // Hover identity: nearest spot within a comfortable radius.
    if (ScreenSpot const *best = hitTest(event->position())) {
        bool const miles = m_config->miles(); // [units] per Settings
        double const dist = best->spot.distance * (miles ? 0.621371 : 1.0);
        qint64 const ageSecs = best->spot.when.secsTo(
            DriftingDateTime::currentDateTimeUtc());
        QString tip;
        if (best->spot.rxOnly) {
            // [mondots] Receive-only reporter — no SNR of its own.
            tip = tr("%1 (%2)\nmonitor · %3 %4 · %5 min ago")
                      .arg(best->spot.receiverCall,
                           best->spot.receiverGrid)
                      .arg(qRound(dist))
                      .arg(miles ? tr("mi") : tr("km"))
                      .arg(ageSecs / 60);
        } else {
            // [snrwho] A dB value is shown ONLY when it is a report
            // of MY signal; -99 is the no-report sentinel, a real
            // 0 dB report still shows (operator 2026-08-15). All
            // view: third-party PSKR SNRs never display — only the
            // hearing store's reported-to-me value qualifies. My
            // view: the spot's snr IS their copy of me (sentinel
            // possible on position-only on-air spots).
            int reportedToMe = -99;
            if (m_viewAll) {
                auto const &ca = m_hearingByBand.value(m_currentBand);
                if (auto const it =
                        ca.constFind(best->spot.receiverCall);
                    it != ca.constEnd())
                    reportedToMe = it->snr;
                // [allsuper] Internet reporters of me qualify too.
                if (reportedToMe <= -99 && best->spot.reportsMe)
                    reportedToMe = best->spot.snr;
            } else {
                reportedToMe = best->spot.snr;
            }
            // No-report spots carry no SNR line at all — the
            // hollow circle already tells the story (operator
            // 2026-08-15).
            QString const snrPart =
                reportedToMe > -99
                    ? tr("hears me at %1 dB · ").arg(reportedToMe)
                    : QString();
            tip = tr("%1 (%2)\n%3%4 %5 · %6 min ago")
                      .arg(best->spot.receiverCall,
                           best->spot.receiverGrid, snrPart)
                      .arg(qRound(dist))
                      .arg(miles ? tr("mi") : tr("km"))
                      .arg(ageSecs / 60);
        }
        // [viewall] All-view spots: who reported hearing this station.
        if (!best->spot.heardBy.isEmpty()) {
            tip += tr("\nheard by %1").arg(best->spot.heardBy);
        }
        // [BUILD 340] Audio offset they heard us at (spot RF − dial),
        // when both are known and the result is sane.
        if (best->spot.freqHz > 0 && m_dialHz > 0) {
            qint64 const audio = best->spot.freqHz - m_dialHz;
            if (audio > 0 && audio < 6000) {
                tip += tr("\n%1 Hz").arg(audio);
            }
        }
        // [BUILD 340] Country, when not our own (topic DXCC compare).
        if (!best->spot.country.isEmpty()) {
            tip += QStringLiteral("\n") + best->spot.country;
        }
        QToolTip::showText(event->globalPosition().toPoint(), tip, this);
    } else {
        QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
}

// [BUILD 340] Double-click a spot → QSY to the DX station's audio
// offset — only above 1000 Hz (same convention as the waterfall
// double-click gate: keep the HB sub-band / low region safe).
void SpotMapWindow::mouseDoubleClickEvent(QMouseEvent *event) {
    // [BUILD 340] Double-click a spot: QSY to its audio offset.
    // Suppressed ONLY while relay-select is active (clicks there are
    // hop selection; operator clarification 2026-08-14 — the earlier
    // global disable was over-applied).
    if (event->button() == Qt::LeftButton && !m_relaySelect) {
        if (ScreenSpot const *best = hitTest(event->position())) {
            if (best->spot.freqHz > 0 && m_dialHz > 0) {
                qint64 const audio = best->spot.freqHz - m_dialHz;
                // QSY window: above 1000 Hz (HB sub-band convention)
                // and at most 2500 Hz (Andy 2026-07-17 — stay inside
                // the usable passband).
                if (audio > 1000 && audio <= 2500) {
                    showToast(tr("Moved to %1's frequency (%2 Hz)")
                                  .arg(best->spot.receiverCall)
                                  .arg(audio));
                    Q_EMIT qsyToOffset(static_cast<int>(audio));
                }
            }
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void SpotMapWindow::showToast(QString const &text) {
    if (!m_toast) {
        m_toast = new QLabel(this);
        m_toast->setStyleSheet(
            QStringLiteral("QLabel { background-color: rgba(30,30,30,220);"
                           " color: white; border-radius: 6px;"
                           " padding: 6px 14px; font-weight: bold; }"));
        m_toast->setAlignment(Qt::AlignCenter);
        m_toast->hide();
        m_toastTimer.setSingleShot(true);
        connect(&m_toastTimer, &QTimer::timeout, m_toast, &QLabel::hide);
    }
    m_toast->setText(text);
    m_toast->adjustSize();
    m_toast->move((width() - m_toast->width()) / 2,
                  height() - m_toast->height() - 24);
    m_toast->show();
    m_toast->raise();
    m_toastTimer.start(2500);
}

void SpotMapWindow::showRelayPathToast() {
    // [relaysel] Running summary while building: origin first, then
    // the hops in outbound order.
    QStringList parts{m_myCall};
    parts.append(m_relayPath);
    showToast(tr("Relay path: %1")
                  .arg(parts.join(QStringLiteral(" → "))));
}

void SpotMapWindow::mousePressEvent(QMouseEvent *event) {
    // Arm a potential drag; the click action moved to release so a
    // pan gesture doesn't also fire spotClicked.
    if (event->button() == Qt::LeftButton) {
        m_maybeDrag = true;
        m_dragging = false;
        m_pressPos = event->position();
        m_panAtPress = m_panPx;
    } else if (event->button() == Qt::MiddleButton) {
        // [wheelzoom] Push the scroll wheel to zoom in (operator,
        // 2026-08-16).
        zoomIn();
    }
    QWidget::mousePressEvent(event);
}

void SpotMapWindow::wheelEvent(QWheelEvent *event) {
    // [wheelzoom] Scroll to zoom (operator, 2026-08-16). One ladder
    // step per notch; accumulate fine trackpad deltas to a notch.
    m_wheelAccum += event->angleDelta().y();
    while (m_wheelAccum >= 120) {
        m_wheelAccum -= 120;
        zoomIn();
    }
    while (m_wheelAccum <= -120) {
        m_wheelAccum += 120;
        zoomOut();
    }
    event->accept();
}

void SpotMapWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (m_dragging) {
            m_dragging = false;
            unsetCursor();
        } else if (m_maybeDrag) {
            // [BUILD 336 TODO #96 first slice] Left-click a spot dot
            // → emit its callsign. Same nearest-within-radius
            // hit-test as hover. (Release-time since mapzoom drag.)
            if (ScreenSpot const *best = hitTest(event->position())) {
                // [relaysel] Selecting: clicks append hops instead of
                // seeding the outgoing box.
                if (m_relaySelect) {
                    QString const call = best->spot.receiverCall;
                    // Each station may appear in the path only once —
                    // a hop revisited is a loop, not a route.
                    if (!m_relayPath.contains(call)) {
                        m_relayPath.append(call);
                        m_relayPathSpots.append(best->spot); // [relaykeep]
                        showRelayPathToast();
                        updateRelayButtons();
                        requestReplot();
                    }
                } else {
                    Q_EMIT spotClicked(best->spot.receiverCall);
                }
            }
        }
        m_maybeDrag = false;
    }
    QWidget::mouseReleaseEvent(event);
}

void SpotMapWindow::changeEvent(QEvent *event) {
    // [btnpos] Activation and font settling both arrive AFTER
    // showEvent (screen mapping can rescale fonts) — recompute the
    // overlay-button geometry at the moment the operator actually
    // looks at it (field: width wrong at activate through THREE
    // show-time fixes).
    if (event->type() == QEvent::ActivationChange ||
        event->type() == QEvent::FontChange)
        positionWindowButtons();
    // Hint toast whenever the map window gains focus (Andy
    // 2026-07-16) — teaches the click-to-copy affordance in place.
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        // [relaysel] While selecting relays, the activation hint
        // teaches THAT gesture, not click-to-compose (operator,
        // 2026-08-14).
        showToast(m_relaySelect
                      ? tr("Click to select relay stations, in "
                           "outbound order")
                      : tr("Click on a call sign to create an outgoing "
                           "message. Double-click to QSY. Drag to pan."));
    }
    QWidget::changeEvent(event);
}

void SpotMapWindow::showEvent(QShowEvent *) {
    // [showfix] Session-only resets fire ONLY on a genuine open
    // (first show / show-after-close). A minimize-restore or
    // desktop-switch re-expose also lands here and must preserve the
    // operator's view/zoom/pan/mode (field 2026-08-15: map
    // "spontaneously" reverted to the default view on activation).
    // [btnpos] Position immediately AND once more after the event
    // loop settles: at showEvent the window may still carry its
    // pre-restore default size (geometry restore / WM sizing land
    // after show), which left buttons misplaced until a manual
    // resize (operator 2026-08-15, again 2026-08-16).
    positionWindowButtons();
    QTimer::singleShot(0, this, [this]() { positionWindowButtons(); });
    if (m_resetOnNextShow) {
        m_resetOnNextShow = false;
        // Every (re)open starts in Auto — manual zoom/pan are
        // per-viewing gestures (operator directive 2026-08-02).
        zoomAuto();
        // [persistui 2026-08-15] View type, Connections, and PSKR
        // toggle PERSIST across reopen and sessions — only the
        // relay builder resets (its path is a per-use gesture).
        if (m_relaySelBtn && m_relaySelBtn->isChecked())
            m_relaySelBtn->setChecked(false);
    }
    requestReplot();
}

void SpotMapWindow::closeEvent(QCloseEvent *event) {
    // Hide only — the MQTT client and caches keep running so the map
    // is current the moment it's reopened. Client stops at app exit.
    // Geometry saved here; WindowVisible is recorded only by the main
    // window's shutdown-time saveSettings() call so mid-session closes
    // don't clobber the reopen-at-startup state.
    {
        SettingsGroup g{m_settings, "SpotMap"};
        m_settings->setValue("geometry", saveGeometry());
    }
    m_resetOnNextShow = true; // [showfix] genuine close -> next show resets
    Q_EMIT closed();
    QWidget::closeEvent(event);
}
