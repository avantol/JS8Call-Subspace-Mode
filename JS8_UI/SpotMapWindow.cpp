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
#include <QSettings>
#include <QLabel>
#include <QToolTip>

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
}

SpotMapWindow::~SpotMapWindow() = default;

bool SpotMapWindow::wasVisibleAtShutdown() const { return m_restoreVisible; }

void SpotMapWindow::saveSettings() {
    SettingsGroup g{m_settings, "SpotMap"};
    m_settings->setValue("geometry", saveGeometry());
    m_settings->setValue("WindowVisible", isVisible());
    m_settings->setValue("ShowRings", m_showRings);
    m_settings->setValue("ShowCallsigns", m_showCallsigns);
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
    QString topic =
        QStringLiteral("pskr/filter/v2/+/JS8/%1/#").arg(topicCall);

    // Documented debug hook: flood-filter override for protocol
    // bring-up without transmitting, e.g.
    //   JS8_SPOTMAP_TOPIC_OVERRIDE=pskr/filter/v2/20m/FT8/#
    if (QString const ov =
            qEnvironmentVariable("JS8_SPOTMAP_TOPIC_OVERRIDE");
        !ov.isEmpty()) {
        qCWarning(mqttclient_js8) << "topic override active:" << ov;
        topic = ov;
    }

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
    m_spotsByBand.clear();
    rebuildTopics();
    requestReplot();
}

void SpotMapWindow::setBand(QString const &band) {
    if (band == m_currentBand)
        return;
    m_currentBand = band;
    requestReplot();
}

// -------------------------------------------------------------------------
// MQTT ingest
// -------------------------------------------------------------------------

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
    if (!sender.isEmpty() &&
        sender.compare(m_myCall, Qt::CaseInsensitive) != 0) {
        ++m_skippedSpots;
        return;
    }

    QString const receiverCall = o.value(QStringLiteral("rc")).toString();
    QString const receiverGrid = o.value(QStringLiteral("rl")).toString();
    int const snr = o.value(QStringLiteral("rp")).toInt(-99);
    if (receiverCall.isEmpty() || receiverGrid.size() < 4 || snr == -99) {
        ++m_skippedSpots;
        return;
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

    auto const vec = Geodesic::vector(m_myGrid, receiverGrid);
    if (!vec.azimuth().isValid() || !vec.distance().isValid()) {
        ++m_skippedSpots;
        return;
    }

    Spot spot;
    spot.receiverCall = receiverCall.toUpper();
    spot.receiverGrid = receiverGrid;
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
    }

    // Latest-per-spotter (operator choice): replace any existing spot
    // from the same receiver in this band.
    auto &spots = m_spotsByBand[band];
    spots.erase(std::remove_if(spots.begin(), spots.end(),
                               [&](Spot const &s) {
                                   return s.receiverCall ==
                                          spot.receiverCall;
                               }),
                spots.end());
    spots.append(spot);
    pruneBand(band);

    if (band == m_currentBand && isVisible())
        requestReplot();
}

void SpotMapWindow::pruneBand(QString const &band) {
    auto const cutoff =
        DriftingDateTime::currentDateTimeUtc().addSecs(-WINDOW_SECS);
    auto &spots = m_spotsByBand[band];
    spots.erase(std::remove_if(spots.begin(), spots.end(),
                               [&](Spot const &s) {
                                   return s.when < cutoff;
                               }),
                spots.end());
}

void SpotMapWindow::onPruneTick() {
    bool currentChanged = false;
    for (auto it = m_spotsByBand.begin(); it != m_spotsByBand.end(); ++it) {
        int const before = it.value().size();
        pruneBand(it.key());
        if (it.key() == m_currentBand && it.value().size() != before)
            currentChanged = true;
    }
    if (currentChanged && isVisible())
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
bool gridInUS(QString const &grid) {
    double lat = 0.0, lon = 0.0;
    if (!gridToLatLon(grid, lat, lon))
        return false;
    if (lat >= 24.5 && lat <= 49.4 && lon >= -125.0 && lon <= -66.9)
        return true; // CONUS
    if (lat >= 51.0 && lat <= 72.0 && lon >= -170.0 && lon <= -129.0)
        return true; // Alaska
    if (lat >= 18.5 && lat <= 22.5 && lon >= -160.5 && lon <= -154.5)
        return true; // Hawaii
    return false;
}
} // namespace

void SpotMapWindow::rebuildMapCache(QPointF const &center, float const R,
                                    float const scaleKm) {
    if (m_mapCacheGrid == m_myGrid && m_mapCacheScale == scaleKm &&
        m_mapCacheR == R && m_mapCacheCenter == center)
        return;
    m_mapCache.clear();
    m_mapCacheGrid = m_myGrid;
    m_mapCacheScale = scaleKm;
    m_mapCacheR = R;
    m_mapCacheCenter = center;

    double lat0 = 0.0, lon0 = 0.0;
    if (!gridToLatLon(m_myGrid, lat0, lon0))
        return;
    double const p1 = lat0 * DEG2RAD;
    double const l1 = lon0 * DEG2RAD;
    double const sinP1 = std::sin(p1), cosP1 = std::cos(p1);

    // Segments whose endpoints are beyond the visible scale (with a
    // little headroom) are dropped — this also sidesteps the
    // antipodal blow-up inherent to the azimuthal projection.
    float const cutKm = scaleKm * 1.15f;

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
            run.append(center +
                       QPointF{std::sin(az) * r, -std::cos(az) * r});
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
    QPointF const center{w / 2.0,
                         TITLE_STRIP_PX +
                             (h - TITLE_STRIP_PX - LEGEND_STRIP_PX) / 2.0};
    float const R =
        std::min(w / 2.0, (h - TITLE_STRIP_PX - LEGEND_STRIP_PX) / 2.0) -
        MARGIN_PX;
    if (R < 40.0f)
        return;

    bool const miles = gridInUS(m_myGrid);
    float const unitScale = miles ? 0.621371f : 1.0f;
    QString const unitLabel = miles ? tr("mi") : tr("km");

    auto const &spots = m_spotsByBand.value(m_currentBand);
    auto const now = DriftingDateTime::currentDateTimeUtc();

    // Auto-scale: outer ring just past the farthest live spot (nice
    // 1/2/5 value), floored so a lone close-in spot isn't degenerate.
    float maxDist = 0.0f;
    for (Spot const &s : spots)
        maxDist = std::max(maxDist, s.distance);
    float const scaleKm =
        spots.isEmpty() ? DEFAULT_SCALE_KM
                        : std::max(niceCeil(maxDist), FLOOR_SCALE_KM);

    QFont small = p.font();
    small.setPointSize(8);
    p.setFont(small);

    // Geographic background: country/coast outlines, clipped to the
    // chart circle. Cached; rebuilt only on center/scale changes.
    rebuildMapCache(center, R, scaleKm);
    p.save();
    QPainterPath clip;
    clip.addEllipse(center, R, R);
    p.setClipPath(clip);
    p.setPen(QPen{QColor(70, 100, 80), 1}); // muted land outline
    for (QPolygonF const &poly : m_mapCache)
        p.drawPolyline(poly);
    p.restore();

    // Outer boundary circle (always) + azimuth ticks.
    p.setPen(QPen{QColor(70, 70, 90), 1});
    p.drawEllipse(center, R, R);
    for (int az = 0; az < 360; az += 30) {
        double const rad = az * DEG2RAD;
        QPointF const dir{std::sin(rad), -std::cos(rad)};
        p.drawLine(center + dir * (R * 0.97), center + dir * R);
    }

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
    p.setPen(QColor(140, 140, 160));
    p.drawText(QPointF{center.x() + 4,
                       center.y() - R + p.fontMetrics().ascent() + 1},
               QStringLiteral("%1 %2")
                   .arg(qRound(scaleKm * unitScale))
                   .arg(unitLabel));
    p.setPen(QColor(190, 190, 210));
    auto const cardinal = [&](int az, QString const &txt) {
        double const rad = az * DEG2RAD;
        QPointF const pos = center + QPointF{std::sin(rad), -std::cos(rad)} *
                                         (R + MARGIN_PX / 2.0);
        QRectF box{pos.x() - 10, pos.y() - 8, 20, 16};
        p.drawText(box, Qt::AlignCenter, txt);
    };
    cardinal(0, QStringLiteral("N"));
    cardinal(90, QStringLiteral("E"));
    cardinal(180, QStringLiteral("S"));
    cardinal(270, QStringLiteral("W"));

    // Center marker (my station).
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(230, 230, 240));
    p.drawEllipse(center, 3, 3);

    // Spots: oldest first so the newest draw on top. Heat blobs blend
    // additively (operator choice); age fades alpha 1.0 -> 0.5 across
    // the 15-minute window.
    QVector<Spot> ordered = spots;
    std::sort(ordered.begin(), ordered.end(),
              [](Spot const &a, Spot const &b) { return a.when < b.when; });

    m_screenSpots.clear();
    for (Spot const &s : ordered) {
        float const age =
            std::clamp(static_cast<float>(s.when.secsTo(now)) / WINDOW_SECS,
                       0.0f, 1.0f);
        float const alpha = 1.0f - 0.5f * age;
        float const r = R * std::min(s.distance / scaleKm, 1.0f);
        double const rad = s.azimuth * DEG2RAD;
        QPointF const pos =
            center + QPointF{std::sin(rad), -std::cos(rad)} * r;

        // Tiny solid circle (operator choice), SNR heat color, age
        // fade in the alpha; thin dark outline for contrast on land.
        p.setPen(QPen{QColor(0, 0, 0, 180), 1});
        p.setBrush(snrColor(s.snr, alpha));
        p.drawEllipse(pos, DOT_RADIUS_PX, DOT_RADIUS_PX);

        m_screenSpots.append({pos, s});
    }

    // Callsign labels: on-map option, default off (hover tooltip
    // provides identity; toggle UI to follow).
    if (m_showCallsigns) {
        for (auto const &ss : m_screenSpots) {
            QString const label = ss.spot.receiverCall;
            QRectF box = p.fontMetrics().boundingRect(label);
            box.moveCenter(
                QPointF{ss.pos.x(), ss.pos.y() - DOT_RADIUS_PX - 8});
            box.adjust(-2, -1, 2, 1);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawRect(box);
            p.setPen(Qt::white);
            p.drawText(box, Qt::AlignCenter, label);
        }
    }

    // Title strip.
    p.setPen(QColor(220, 220, 235));
    QFont title = p.font();
    title.setPointSize(9);
    p.setFont(title);
    QString const bandText =
        m_currentBand.isEmpty() ? tr("no band") : m_currentBand;
    p.drawText(QRectF{0, 0, static_cast<qreal>(w), TITLE_STRIP_PX},
               Qt::AlignCenter,
               tr("%1 @ %2 — %3 — %4 spotters / last 15 min — %5")
                   .arg(m_myCall, m_myGrid, bandText)
                   .arg(spots.size())
                   .arg(m_stateText));

    if (spots.isEmpty()) {
        p.setPen(QColor(150, 150, 170));
        p.drawText(QRectF{0, center.y() - 30, static_cast<qreal>(w), 60},
                   Qt::AlignCenter, tr("No new spots yet"));
    }

    // Legend: SNR gradient bar.
    {
        qreal const barW = std::min(280.0, w * 0.6);
        QRectF bar{(w - barW) / 2.0,
                   static_cast<qreal>(h - LEGEND_STRIP_PX + 8), barW, 10};
        QLinearGradient lg{bar.topLeft(), bar.topRight()};
        for (float t = 0.0f; t <= 1.001f; t += 0.1f)
            lg.setColorAt(t, snrColor(SNR_COLD +
                                          static_cast<int>(
                                              t * (SNR_HOT - SNR_COLD)),
                                      1.0f));
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

    update();
}

void SpotMapWindow::paintEvent(QPaintEvent *) {
    QPainter p{this};
    p.drawPixmap(0, 0, m_pixmap);
}

void SpotMapWindow::resizeEvent(QResizeEvent *) { requestReplot(); }

void SpotMapWindow::mouseMoveEvent(QMouseEvent *event) {
    // Hover identity: nearest spot within a comfortable radius.
    QPointF const m = event->position();
    ScreenSpot const *best = nullptr;
    double bestD2 = 12.0 * 12.0;
    for (auto const &ss : m_screenSpots) {
        double const dx = ss.pos.x() - m.x();
        double const dy = ss.pos.y() - m.y();
        if (double const d2 = dx * dx + dy * dy; d2 < bestD2) {
            bestD2 = d2;
            best = &ss;
        }
    }
    if (best) {
        bool const miles = gridInUS(m_myGrid);
        double const dist = best->spot.distance * (miles ? 0.621371 : 1.0);
        qint64 const ageSecs = best->spot.when.secsTo(
            DriftingDateTime::currentDateTimeUtc());
        QToolTip::showText(
            event->globalPosition().toPoint(),
            tr("%1 (%2)\n%3 dB · %4 %5 · %6 min ago")
                .arg(best->spot.receiverCall, best->spot.receiverGrid)
                .arg(best->spot.snr)
                .arg(qRound(dist))
                .arg(miles ? tr("mi") : tr("km"))
                .arg(ageSecs / 60),
            this);
    } else {
        QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
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

void SpotMapWindow::mousePressEvent(QMouseEvent *event) {
    // [BUILD 336 TODO #96 first slice] Left-click a spot dot → emit
    // its callsign. Same nearest-within-radius hit-test as hover.
    if (event->button() == Qt::LeftButton) {
        QPointF const m = event->position();
        ScreenSpot const *best = nullptr;
        double bestD2 = 12.0 * 12.0;
        for (auto const &ss : m_screenSpots) {
            double const dx = ss.pos.x() - m.x();
            double const dy = ss.pos.y() - m.y();
            if (double const d2 = dx * dx + dy * dy; d2 < bestD2) {
                bestD2 = d2;
                best = &ss;
            }
        }
        if (best) {
            Q_EMIT spotClicked(best->spot.receiverCall);
        }
    }
    QWidget::mousePressEvent(event);
}

void SpotMapWindow::changeEvent(QEvent *event) {
    // Hint toast whenever the map window gains focus (Andy
    // 2026-07-16) — teaches the click-to-copy affordance in place.
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        showToast(tr("Click on a call sign to create an outgoing message"));
    }
    QWidget::changeEvent(event);
}

void SpotMapWindow::showEvent(QShowEvent *) { requestReplot(); }

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
    Q_EMIT closed();
    QWidget::closeEvent(event);
}
