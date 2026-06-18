/**
 * @file mainwindow.cpp
 * @brief source file that implements the JS8Call user interface
 *   executes member functions of the UI_Constructor class that provide
 *   all functionality of the JS8call main window
 */

#include "mainwindow.h"
#include "JS8_Widgets/BandActivityMessageDelegate.h"
#include "JS8_Main/FileTransfer.h"

#include <QFileDialog>
#include <QStandardPaths>
#include <QToolButton>

#ifdef JS8_ENABLE_FT2
#include "JS8_Mode/DecodeFT2.h"
#include "JS8_Mode/ft2_bridge.h"
#include <cstring>
#endif

#include "moc_mainwindow.cpp"

// TODO: Move to member:
static char message[29];
static char msgsent[29];
static int msgibits;

// How many milliseconds to wait before releasing PTT at end of transmission.
constexpr int TX_SWITCHOFF_DELAY = 200;

#ifdef JS8_ENABLE_FT2
int volatile itone[FT2_NUM_SYMBOLS]; // Max of JS8 (79) and FT2 (103)
float ft2_txwave[FT2_NWAVE];         // Pre-computed FT2 GFSK waveform
int ft2_txwave_len = 0;              // Actual waveform length
#else
int volatile itone[JS8_NUM_SYMBOLS]; // Audio tones for all Tx symbols
#endif
struct dec_data dec_data;            // for sharing with Fortran
struct specData specData;            // Used by plotter
std::mutex fftw_mutex;

namespace {
int ms_minute_error() {
    auto const now = DriftingDateTime::currentDateTimeLocal();
    auto const time = now.time();
    auto const second = time.second();

    return now.msecsTo(now.addSecs(second > 30 ? 60 - second : -second)) -
           time.msec();
}

namespace State {
constexpr QStringView Ready = u"Ready";
constexpr QStringView Send = u"Send";
constexpr QStringView Sending = u"Sending";
constexpr QStringView Tuning = u"Tuning";

QString timed(QStringView const state, int const delay) {
    auto time = std::div(delay, 60);

    if (time.quot && time.rem)
        return QString("%1 (%2m %3s)").arg(state).arg(time.quot).arg(time.rem);
    else if (time.quot)
        return QString("%1 (%2m)").arg(state).arg(time.quot);
    else
        return QString("%1 (%2s)").arg(state).arg(time.rem);
}
} // namespace State

#if 0
  int round(int numToRound, int multiple)
  {
   if(multiple == 0)
   {
    return numToRound;
   }

   int roundDown = ( (int) (numToRound) / multiple) * multiple;

   if(numToRound - roundDown > multiple/2){
    return roundDown + multiple;
   }

   return roundDown;
  }
#endif

int roundUp(int numToRound, int multiple) {
    if (multiple == 0) {
        return numToRound;
    }

    int roundDown = (numToRound / multiple) * multiple;
    return roundDown + multiple;
}

// Copy at most size bytes into the array, filling any unused size
// with spaces if less than size bytes were available to copy. For
// convenience, return a one past the end iterator, i.e., equal to
// array + size.

auto copyByteData(QByteArrayView const bytes, char *const array,
                  qsizetype const size) {
    return std::fill_n(
        std::copy_n(bytes.begin(), std::min(size, bytes.size()), array),
        size - bytes.size(), ' ');
}

// Copy at most size bytes into the array, padding out the message
// with spaces if less than size bytes were available to copy, and
// null-terminate it. Caller is responsible for ensuring that at
// least (size + 1) bytes of space are available.

void copyMessage(QStringView const string, char *const array,
                 qsizetype const size = 28) {
    *copyByteData(string.toLocal8Bit(), array, size) = '\0';
}

} // namespace

void UI_Constructor(); // explicit member function of the UI_Constructor class

void UI_Constructor::ensureMessageDock()
{
    if (messageDock_) return;

    messagePanel_ = new MessagePanel(inboxPath(), this);

    messageDock_ = new QDockWidget(tr("Message Inbox"), this);
    messageDock_->setObjectName("messageInboxDock"); // important for save/restoreState
    messageDock_->setWidget(messagePanel_);

    // Choose where it can dock:
    messageDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                  Qt::RightDockWidgetArea |
                                  Qt::BottomDockWidgetArea);

    // Choose behavior:
    messageDock_->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);

    // Initial placement:
    addDockWidget(Qt::RightDockWidgetArea, messageDock_);

    // Optional: closing hides (default); ensure no auto-delete:
    messageDock_->setAttribute(Qt::WA_DeleteOnClose, false);

    // Make the menu action reflect visibility automatically:
    ui->actionShow_Message_Inbox->setCheckable(true);
    ui->actionShow_Message_Inbox->setChecked(messageDock_->isVisible());
    connect(messageDock_, &QDockWidget::visibilityChanged, this,
            [this](bool visible) {
                QSignalBlocker b(ui->actionShow_Message_Inbox);
                ui->actionShow_Message_Inbox->setChecked(visible);
            });

    // Handle reply function
    connect(messagePanel_, &MessagePanel::replyMessage, this,
                [this](const QString &text) {
                    addMessageText(text, true, true);
                    refreshInboxCounts();
                    displayCallActivity();
                });

    connect(messagePanel_, &MessagePanel::countsUpdated, this, [this]() {
            refreshInboxCounts();
            displayCallActivity();
        });
}

bool checkVersion(); // JS8_Mainwindow/checkVersion.cpp

void UI_Constructor::checkStartupWarnings() {
    checkVersion(false);  // Always check for updates at startup
    ensureCallsignSet(false);
}

void initializeDummyData(); // JS8_Mainwindow/initializeDummyData.cpp

void initializeGroupMessage(); // JS8_Mainwindow/initializeGroupMessage.cpp

void UI_Constructor::initialize_fonts() {
    set_application_font(m_config.text_font());

    setTextEditFont(ui->textEditRX, m_config.rx_text_font());
    setTextEditFont(ui->extFreeTextMsgEdit, m_config.tx_text_font());

    displayActivity(true);
}

void UI_Constructor::on_the_minute() {
    if (minuteTimer.isSingleShot()) {
        minuteTimer.setSingleShot(false);
        minuteTimer.start(60 * 1000); // run free
    } else {
        auto const &ms_error = ms_minute_error();
        if (qAbs(ms_error) > 1000) // keep drift within +-1s
        {
            minuteTimer.setSingleShot(true);
            minuteTimer.start(ms_error + 60 * 1000);
        }
    }

    if (m_config.watchdog()) {
        incrementIdleTimer();
    } else {
        tx_watchdog(false);
    }
}

void UI_Constructor::tryBandHop() {
    // see if we need to hop bands...
    if (!m_config.auto_switch_bands()) {
        return;
    }

    // make sure we're not transmitting (split/fake-split shifts VFO during TX)
    if (isMessageQueuedForTransmit() || m_transmitting) {
        return;
    }

    // get the current band
    auto dialFreq = dialFrequency();
    if (dialFreq == 0) return;  // rig not yet connected

    auto currentBand = m_config.bands()->find(dialFreq);

    // get the stations list
    auto stations = m_config.stations()->station_list();

    // order stations by (switch_at, switch_until) time tuple
    std::stable_sort(
        stations.begin(), stations.end(),
        [](StationList::Station const &a, StationList::Station const &b) {
            return (a.switch_at_ < b.switch_at_) ||
                   (a.switch_at_ == b.switch_at_ &&
                    a.switch_until_ < b.switch_until_);
        });

    // we just set the date to a known y/m/d to make the comparisons easier
    QDateTime d = DriftingDateTime::currentDateTimeUtc();
    d.setDate(QDate(2000, 1, 1));

    QDateTime startOfDay =
        QDateTime(QDate(2000, 1, 1), QTime(0, 0), QTimeZone::utc());
    QDateTime endOfDay =
        QDateTime(QDate(2000, 1, 1), QTime(23, 59, 59, 999), QTimeZone::utc());

    StationList::Station *hopStation = nullptr;

    // See if we can find a needed band switch...
    // In the case of overlapping windows, choose the latest one
    foreach (auto station, stations) {
        // we can switch to this frequency if we're in the time range, inclusive
        // of switch_at, exclusive of switch_until and if we are switching to a
        // different frequency than the last hop. this allows us to switch bands
        // at that time, but then later we can later switch to a different band
        // if needed without the automatic band switching to take over
        bool inTimeRange =
            ((station.switch_at_ <= d &&
              d <= station.switch_until_) || // <- normal range, 12-16 && 6-8,
                                             // evaluated as 12 <= d <= 16 || 6
                                             // <= d <= 8

             (station.switch_until_ < station.switch_at_ &&
              ( // <- say for a range of 12->2 & 2->12;  12->2,
                  (station.switch_at_ <= d &&
                   d <= endOfDay) || //    should be evaluated as 12 <= d <=
                                     //    23:59 || 00:00 <= d <= 2
                  (startOfDay <= d && d <= station.switch_until_))));

        if (inTimeRange) {
            delete hopStation;
            hopStation = new StationList::Station(station);
        }
    }

    // If we have a candidate station, see if the hop is valid, and if so, do it
    if (hopStation != nullptr) {
        bool noOverride =
            (m_bandHopped ||
             (!m_bandHopped && hopStation->frequency_ != m_bandHoppedFreq));

        // Tolerance: ignore differences under 1 kHz (split TX offset, drift)
        bool freqIsDifferent = (std::abs((qint64)hopStation->frequency_ - (qint64)dialFreq) > 1000);

        bool canSwitch = (noOverride && freqIsDifferent);

        if (canSwitch) {
            qWarning() << "[BAND-HOP] canSwitch: hop=" << hopStation->frequency_
                       << "dial=" << dialFreq << "bandHopped=" << m_bandHopped
                       << "bandHoppedFreq=" << m_bandHoppedFreq;
            Frequency frequency = hopStation->frequency_;

            m_bandHopped = false;
            m_bandHoppedFreq = frequency;

            SelfDestructMessageBox *m = new SelfDestructMessageBox(
                30, "Scheduled Frequency Change",
                QString("A scheduled frequency change has arrived. The rig "
                        "frequency will be changed to %1 MHz in %2 second(s).")
                    .arg(Radio::frequency_MHz_string(frequency)),
                QMessageBox::Information, QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Ok, true, this);

            connect(m, &SelfDestructMessageBox::finished, this,
                    [this, m, frequency]() {
                        if (m->result() == QMessageBox::Ok) {
                            m_bandHopped = true;
                            setRig(frequency);
                        }
                        m->deleteLater();
                    });

            m->show();

#if 0
		  // TODO: jsherer - this is totally a hack because of the signal that gets emitted to clearActivity on band change...
          QTimer *t = new QTimer(this);
          t->setInterval(250);
          t->setSingleShot(true);
          connect(t, &QTimer::timeout, this, [this, frequency, dialFreq](){
              auto message = QString("Scheduled frequency switch from %1 MHz to %2 MHz");
              message = message.arg(Radio::frequency_MHz_string(dialFreq));
              message = message.arg(Radio::frequency_MHz_string(frequency));
              writeNoticeTextToUI(DriftingDateTime::currentDateTimeUtc(), message);
          });
          t->start();
#endif

            return;
        }

        delete hopStation;
    }
}

void UI_Constructor::manualBandHop(const StationList::Station station) {
    // make sure we're not transmitting
    if (isMessageQueuedForTransmit()) {
        return;
    }

    Frequency frequency = station.frequency_;

    m_bandHopped = true;
    m_bandHoppedFreq = frequency;
    setRig(frequency);
}

//--------------------------------------------------- UI_Constructor destructor
UI_Constructor::~UI_Constructor() {
#ifdef JS8_ENABLE_FT2
    m_l2DecodeTimer.stop();
    m_l2DecodeWatcher.waitForFinished();
#endif

    {
        std::lock_guard<std::mutex> lock(fftw_mutex);
        fftwf_export_wisdom_to_filename(wisdomFileName());
    }

    m_networkThread.quit();
    m_networkThread.wait();

    m_audioThread.quit();
    m_audioThread.wait();

    m_notificationAudioThread.quit();
    m_notificationAudioThread.wait();

    m_decoder.quit();

    remove_child_from_event_filter(this);
}

//-------------------------------------------------------- writeSettings()
void UI_Constructor::writeSettings() {
    m_settings->beginGroup("UI_Constructor");
    m_settings->setValue("geometry", saveGeometry());
    m_settings->setValue("geometryNoControls", m_geometryNoControls);
    m_settings->setValue("state", saveState());

    m_settings->setValue("MainSplitter", ui->mainSplitter->saveState());
    m_settings->setValue("TextHorizontalSplitter",
                         ui->textHorizontalSplitter->saveState());
    m_settings->setValue("BandActivityVisible",
                         ui->tableWidgetRXAll->isVisible());
    m_settings->setValue("BandHBActivityVisible",
                         ui->actionShow_Band_Heartbeats_and_ACKs->isChecked());
    m_settings->setValue("TextVerticalSplitter",
                         ui->textVerticalSplitter->saveState());
    m_settings->setValue("TimeDrift", DriftingDateTime::drift());
    m_settings->setValue("ShowTooltips", ui->actionShow_Tooltips->isChecked());
    m_settings->setValue("ShowStatusbar", ui->statusBar->isVisible());
    m_settings->setValue("RXActivity", ui->textEditRX->toHtml());

    m_settings->endGroup();

    m_settings->beginGroup("Common");
    m_settings->setValue("Freq", freq());
    m_settings->setValue("SubMode", Varicode::JS8CallNormal); // always save Normal for JS8Call-Improved compat
    m_settings->setValue("SsSubMode", m_nSubMode);
    m_settings->setValue("SubModeHB", ui->actionModeJS8HB->isChecked());
    m_settings->setValue("SubModeHBAck",
                         ui->actionHeartbeatAcknowledgements->isChecked());
    m_settings->setValue("SubModeMultiDecode",
                         ui->actionModeMultiDecoder->isChecked());
    // [BUILD 304] ARQ state intentionally NOT persisted. With the
    // opt-in-per-super-message + auto-disable design, persistence
    // doesn't make sense and the startup-restore path raced ahead
    // of m_chunkedArq construction (labels showed +ARQ but Manager
    // was off, so the first Send went out as plain non-ARQ).
    m_settings->setValue("DialFreq",
                         QVariant::fromValue(m_lastMonitoredFrequency));
    m_settings->setValue("OutAttenuation", ui->outAttenuation->value());
    m_settings->setValue("pwrBandTxMemory", m_pwrBandTxMemory);
    m_settings->setValue("pwrBandTuneMemory", m_pwrBandTuneMemory);
    m_settings->setValue("SortBy", QVariant(m_sortCache));
    m_settings->setValue("ShowColumns", QVariant(m_showColumnsCache));
    m_settings->setValue("HBInterval", m_hbInterval);
    m_settings->setValue("CQInterval", m_cqInterval);

    // TODO: jsherer - need any other customizations?
    /*m_settings->setValue("PanelLeftGeometry",
    ui->tableWidgetRXAll->geometry());
    m_settings->setValue("PanelRightGeometry",
    ui->tableWidgetCalls->geometry()); m_settings->setValue("PanelTopGeometry",
    ui->extFreeTextMsg->geometry()); m_settings->setValue("PanelBottomGeometry",
    ui->extFreeTextMsgEdit->geometry());
    m_settings->setValue("PanelWaterfallGeometry",
    ui->bandHorizontalWidget->geometry());*/
    // m_settings->setValue("MainSplitter",
    // QVariant::fromValue(ui->mainSplitter->sizes()));

    m_settings->endGroup();

    auto now = DriftingDateTime::currentDateTimeUtc();
    int callsignAging = m_config.callsign_aging();

    m_settings->beginGroup("SsCallActivity");
    m_settings->remove(""); // remove all keys in current group
    foreach (auto cd, m_callActivity.values()) {
        if (cd.call.trimmed().isEmpty()) {
            continue;
        }
        if (callsignAging &&
            cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
            continue;
        }
        m_settings->setValue(
            cd.call.trimmed(),
            QVariantMap{
                {"snr", QVariant(cd.snr)},
                {"grid", QVariant(cd.grid)},
                {"dial", QVariant(cd.dial)},
                {"freq", QVariant(cd.offset)},
                {"tdrift", QVariant(cd.tdrift)},
#if CACHE_CALL_DATETIME_AS_STRINGS
                {"ackTimestamp",
                 QVariant(cd.ackTimestamp.toString("yyyy-MM-dd hh:mm:ss"))},
                {"utcTimestamp",
                 QVariant(cd.utcTimestamp.toString("yyyy-MM-dd hh:mm:ss"))},
#else
                {"ackTimestamp", QVariant(cd.ackTimestamp)},
                {"utcTimestamp", QVariant(cd.utcTimestamp)},
#endif
                {"submode", QVariant(cd.submode)},
            });
    }
    m_settings->endGroup();
}

//---------------------------------------------------------- readSettings()
void UI_Constructor::readSettings() {
    m_settings->beginGroup("UI_Constructor");
    ensureMessageDock();
    setMinimumSize(800, 400);
    restoreGeometry(
        m_settings->value("geometry", saveGeometry()).toByteArray());
    setMinimumSize(800, 400);

    m_geometryNoControls =
        m_settings->value("geometryNoControls", saveGeometry()).toByteArray();
    restoreState(m_settings->value("state", saveState()).toByteArray());

    auto mainSplitterState = m_settings->value("MainSplitter").toByteArray();
    if (!mainSplitterState.isEmpty()) {
        ui->mainSplitter->restoreState(mainSplitterState);
    }
    auto horizontalState =
        m_settings->value("TextHorizontalSplitter").toByteArray();
    if (!horizontalState.isEmpty()) {
        ui->textHorizontalSplitter->restoreState(horizontalState);
        auto hsizes = ui->textHorizontalSplitter->sizes();

        ui->tableWidgetRXAll->setVisible(hsizes.at(0) > 0);
        ui->tableWidgetCalls->setVisible(hsizes.at(2) > 0);
    }

    m_bandActivityWasVisible =
        m_settings->value("BandActivityVisible", true).toBool();
    ui->tableWidgetRXAll->setVisible(m_bandActivityWasVisible);

    auto verticalState =
        m_settings->value("TextVerticalSplitter").toByteArray();
    if (!verticalState.isEmpty()) {
        ui->textVerticalSplitter->restoreState(verticalState);
    }
    {
        int savedDrift = m_settings->value("TimeDrift", 0).toInt();
        setDrift(savedDrift);
    }
    ui->actionShow_Waterfall_Controls->setChecked(
        m_wideGraph->controlsVisible());
    ui->actionShow_Waterfall_Time_Drift_Controls->setChecked(
        m_wideGraph->timeControlsVisible());
    ui->actionShow_Tooltips->setChecked(
        m_settings->value("ShowTooltips", true).toBool());
    ui->actionShow_Statusbar->setChecked(
        m_settings->value("ShowStatusbar", true).toBool());
    ui->statusBar->setVisible(ui->actionShow_Statusbar->isChecked());
    ui->textEditRX->setHtml(
        m_config.reset_activity()
            ? ""
            : m_settings->value("RXActivity", "").toString());
    ui->actionShow_Band_Heartbeats_and_ACKs->setChecked(
        m_settings->value("BandHBActivityVisible", true).toBool());
    m_settings->endGroup();

    m_settings->beginGroup("Common");

    // set the frequency offset
    setFreqOffsetForRestore(
        m_settings->value("Freq", Default::FREQUENCY).toInt(), false); // XXX

    setSubmode(m_settings->value("SsSubMode", Default::SUBMODE).toInt());
    ui->actionModeJS8HB->setChecked(
        m_settings->value("SubModeHB", false).toBool());
    ui->actionHeartbeatAcknowledgements->setChecked(
        m_settings->value("SubModeHBAck", false).toBool());
    ui->actionModeMultiDecoder->setChecked(
        m_settings->value("SubModeMultiDecode", true).toBool());
    // [BUILD 304] ARQ always starts OFF. Opt-in per super-message via
    // the menu action or manual toggle. Not persisted (see save site).
    ui->actionModeReplicatorProtocol->setChecked(false);

    m_lastMonitoredFrequency =
        m_settings
            ->value("DialFreq",
                    QVariant::fromValue<Frequency>(Default::DIAL_FREQUENCY))
            .value<Frequency>();
    setFreq(0); // ensure a change is signaled
    setFreq(m_settings->value("Freq", Default::FREQUENCY).toInt());
    // setup initial value of tx attenuator
    m_block_pwr_tooltip = true;
    ui->outAttenuation->setValue(
        m_settings->value("OutAttenuation", 0).toInt());
    m_block_pwr_tooltip = false;
    m_pwrBandTxMemory = m_settings->value("pwrBandTxMemory").toHash();
    m_pwrBandTuneMemory = m_settings->value("pwrBandTuneMemory").toHash();

    m_sortCache = m_settings->value("SortBy").toMap();
    m_showColumnsCache = m_settings->value("ShowColumns").toMap();
    m_hbInterval = m_settings->value("HBInterval", 0).toInt();
    m_cqInterval = m_settings->value("CQInterval", 0).toInt();

    // TODO: jsherer - any other customizations?
    // ui->mainSplitter->setSizes(m_settings->value("MainSplitter",
    // QVariant::fromValue(ui->mainSplitter->sizes())).value<QList<int> >());
    // ui->tableWidgetRXAll->restoreGeometry(m_settings->value("PanelLeftGeometry",
    // ui->tableWidgetRXAll->saveGeometry()).toByteArray());
    // ui->tableWidgetCalls->restoreGeometry(m_settings->value("PanelRightGeometry",
    // ui->tableWidgetCalls->saveGeometry()).toByteArray());
    // ui->extFreeTextMsg->setGeometry( m_settings->value("PanelTopGeometry",
    // ui->extFreeTextMsg->geometry()).toRect());
    // ui->extFreeTextMsgEdit->setGeometry(
    // m_settings->value("PanelBottomGeometry",
    // ui->extFreeTextMsgEdit->geometry()).toRect());
    // ui->bandHorizontalWidget->setGeometry(
    // m_settings->value("PanelWaterfallGeometry",
    // ui->bandHorizontalWidget->geometry()).toRect()); qCDebug(mainwindow_js8)
    // << m_settings->value("PanelTopGeometry") << ui->extFreeTextMsg;

    setTextEditStyle(ui->textEditRX, m_config.color_rx_foreground(),
                     m_config.color_rx_background(), m_config.rx_text_font());
    setTextEditStyle(
        ui->extFreeTextMsgEdit, m_config.color_compose_foreground(),
        m_config.color_compose_background(), m_config.compose_text_font());
    ui->extFreeTextMsgEdit->setFont(m_config.compose_text_font(),
                                    m_config.color_compose_foreground(),
                                    m_config.color_compose_background());

    m_settings->endGroup();

    // use these initialisation settings to tune the audio o/p buffer
    // size and audio thread priority
    m_settings->beginGroup("Tune");
    m_msAudioOutputBuffered = m_settings->value("Audio/OutputBufferMs").toInt();
    m_framesAudioInputBuffered =
        m_settings->value("Audio/InputBufferFrames", JS8_RX_SAMPLE_RATE / 10)
            .toInt();
    m_audioThreadPriority = static_cast<QThread::Priority>(
        m_settings->value("Audio/ThreadPriority", QThread::TimeCriticalPriority)
            .toInt() %
        8);
    m_notificationAudioThreadPriority = static_cast<QThread::Priority>(
        m_settings
            ->value("Audio/NotificationThreadPriority", QThread::LowPriority)
            .toInt() %
        8);
    m_decoderThreadPriority = static_cast<QThread::Priority>(
        m_settings->value("Audio/DecoderThreadPriority", QThread::HighPriority)
            .toInt() %
        8);
    m_networkThreadPriority = static_cast<QThread::Priority>(
        m_settings->value("Network/NetworkThreadPriority", QThread::LowPriority)
            .toInt() %
        8);
    m_settings->endGroup();

    if (m_config.reset_activity()) {
        // NOOP
    } else {
        // Migrate from old [CallActivity] to [SsCallActivity] if needed,
        // then delete [CallActivity] to prevent JS8Call-Improved crashes.
        QString readGroup = "SsCallActivity";
        if (!m_settings->childGroups().contains("SsCallActivity") &&
            m_settings->childGroups().contains("CallActivity")) {
            readGroup = "CallActivity";  // one-time migration
        }

        m_settings->beginGroup(readGroup);
        foreach (auto call, m_settings->allKeys()) {

            auto values = m_settings->value(call).toMap();

            auto snr = values.value("snr", -64).toInt();
            auto grid = values.value("grid", "").toString();
            auto dial = values.value("dial", 0).toInt();
            auto freq = values.value("freq", 0).toInt();
            auto tdrift = values.value("tdrift", 0).toFloat();

#if CACHE_CALL_DATETIME_AS_STRINGS
            auto ackTimestampStr = values.value("ackTimestamp", "").toString();
            auto ackTimestamp =
                QDateTime::fromString(ackTimestampStr, "yyyy-MM-dd hh:mm:ss");
            ackTimestamp.setUtcOffset(0);

            auto utcTimestampStr = values.value("utcTimestamp", "").toString();
            auto utcTimestamp =
                QDateTime::fromString(utcTimestampStr, "yyyy-MM-dd hh:mm:ss");
            utcTimestamp.setUtcOffset(0);
#else
            auto ackTimestamp = values.value("ackTimestamp").toDateTime();
            auto utcTimestamp = values.value("utcTimestamp").toDateTime();
#endif
            auto submode =
                values.value("submode", Varicode::JS8CallNormal).toInt();

            CallDetail cd = {};
            cd.call = call;
            cd.snr = snr;
            cd.grid = grid;
            cd.dial = dial;
            cd.offset = freq;
            cd.tdrift = tdrift;
            cd.ackTimestamp = ackTimestamp;
            cd.utcTimestamp = utcTimestamp;
            cd.submode = submode;

            logCallActivity(cd, false);
        }
        m_settings->endGroup();

        // Always delete [CallActivity] to prevent JS8Call-Improved crashes
        if (m_settings->childGroups().contains("CallActivity")) {
            m_settings->beginGroup("CallActivity");
            m_settings->remove("");
            m_settings->endGroup();
        }
    }

    QTimer::singleShot(0, this, [this]{
        if (!messageDock_) return;

        // If restoreState made it floating, force Qt to recreate the floating window.
        if (messageDock_->isFloating() && messageDock_->isVisible()) {
            messageDock_->setFloating(false);
            messageDock_->setFloating(true);

            messageDock_->show();
            messageDock_->raise();
        }
    });

    m_settings_read = true;
}

void UI_Constructor::set_application_font(QFont const &font) {
    qApp->setFont(font);
    // set font in the application style sheet as well in case it has
    // been modified in the style sheet which has priority
    qApp->setStyleSheet(qApp->styleSheet() + "* {" + font_as_stylesheet(font) +
                        '}');
    for (auto &widget : qApp->topLevelWidgets()) {
        widget->updateGeometry();
    }
}

void dataSink(); // JS8_Mainwindow/dataSink.cpp

void UI_Constructor::showSoundInError(const QString &errorMsg) {
    JS8MessageBox::critical_message(this, tr("Error in Sound Input"), errorMsg);
}

void UI_Constructor::showSoundOutError(const QString &errorMsg) {
    JS8MessageBox::critical_message(this, tr("Error in Sound Output"),
                                    errorMsg);
}

void UI_Constructor::showStatusMessage(const QString &statusMsg) {
    statusBar()->showMessage(statusMsg, 5000);
}

void UI_Constructor::on_menuModeJS8_aboutToShow() {
    // Two-tier gate, mirrors the periodic-poll logic:
    //   canChangeSpeed — speed-mode actions stay usable BETWEEN
    //     chunks during an ARQ session (operator can adapt to
    //     changing band conditions mid-super-message).
    //   canChangeMode — ARQ toggle action stays locked for the full
    //     session (flipping ARQ mid-protocol breaks the state
    //     machine).
    // [RX-SIDE NO-LOCK 2026-06-10] arqBusy uses hasActiveTxSession()
    // — fires only when WE are sending an ARQ super-msg, NOT when
    // we are receiving one. Receiver-side menus stay usable mid-RX
    // (operator can keep working while chunks arrive).
    bool const arqBusy = (m_chunkedArq && m_chunkedArq->hasActiveTxSession());
    bool const canChangeSpeed =
        !m_transmitting && m_txFrameCount == 0 && m_txFrameQueue.isEmpty();
    bool const canChangeMode = canChangeSpeed && !arqBusy;
    ui->actionModeJS8Normal->setEnabled(canChangeSpeed);
    ui->actionModeJS8Fast->setEnabled(canChangeSpeed);
    ui->actionModeJS8Turbo->setEnabled(canChangeSpeed);
    ui->actionModeJS8Slow->setEnabled(canChangeSpeed);
    ui->actionModeJS8Ultra->setEnabled(canChangeSpeed);
#ifdef JS8_ENABLE_FT2
    ui->actionModeFT2->setEnabled(canChangeSpeed);
#endif
    ui->actionModeReplicatorProtocol->setEnabled(canChangeMode);

    // dynamically replace the autoreply menu item text
    auto autoreplyText = ui->actionModeAutoreply->text();
    if (m_config.autoreply_confirmation() &&
        !autoreplyText.contains(" with Confirmation")) {
        autoreplyText.replace("Autoreply", "Autoreply with Confirmation");
        autoreplyText.replace("&AUTO", "&AUTO+CONF");
        ui->actionModeAutoreply->setText(autoreplyText);
    } else if (!m_config.autoreply_confirmation() &&
               autoreplyText.contains(" with Confirmation")) {
        autoreplyText.replace(" with Confirmation", "");
        autoreplyText.replace("+CONF", "");
        ui->actionModeAutoreply->setText(autoreplyText);
    }
}

void UI_Constructor::on_menuControl_aboutToShow() {
    auto freqMenu = new QMenu(this->menuBar());
    buildFrequencyMenu(freqMenu);
    ui->actionSetFrequency->setMenu(freqMenu);

    auto heartbeatMenu = new QMenu(this->menuBar());
    buildHeartbeatMenu(heartbeatMenu);
    ui->actionHeartbeat->setMenu(heartbeatMenu);

    auto cqMenu = new QMenu(this->menuBar());
    buildCQMenu(cqMenu);
    ui->actionCQ->setMenu(cqMenu);

    ui->actionEnable_Monitor_RX->setChecked(ui->monitorButton->isChecked());
    ui->actionEnable_Transmitter_TX->setChecked(
        ui->monitorTxButton->isChecked());
    ui->actionEnable_Reporting_SPOT->setChecked(ui->spotButton->isChecked());
    ui->actionEnable_Tuning_Tone_TUNE->setChecked(ui->tuneButton->isChecked());
}

void UI_Constructor::on_actionUser_Guide_triggered() {
    QDesktopServices::openUrl(
        QUrl("https://js8call-improved.com/downloads/JS8Call_User_Guide.pdf"));
}

void UI_Constructor::on_actionEnable_Monitor_RX_toggled(bool checked) {
    ui->monitorButton->setChecked(checked);
}

void UI_Constructor::on_actionEnable_Transmitter_TX_toggled(bool checked) {
    ui->monitorTxButton->setChecked(checked);
}

void UI_Constructor::on_actionEnable_Reporting_SPOT_toggled(bool checked) {
    ui->spotButton->setChecked(checked);
}

void UI_Constructor::on_actionEnable_Tuning_Tone_TUNE_toggled(bool checked) {
    ui->tuneButton->setChecked(checked);
    on_tuneButton_clicked(checked);
}

void UI_Constructor::on_menuWindow_aboutToShow() {
    ui->actionShow_Fullscreen->setChecked(
        (windowState() & Qt::WindowFullScreen) == Qt::WindowFullScreen);

    ui->actionShow_Statusbar->setChecked(ui->statusBar &&
                                         ui->statusBar->isVisible());

    auto hsizes = ui->textHorizontalSplitter->sizes();
    ui->actionShow_Band_Activity->setChecked(hsizes.at(0) > 0);
    ui->actionShow_Call_Activity->setChecked(hsizes.at(2) > 0);

    auto vsizes = ui->mainSplitter->sizes();
    ui->actionShow_Frequency_Clock->setChecked(vsizes.first() > 0);
    ui->actionShow_Waterfall->setChecked(vsizes.last() > 0);
    ui->actionShow_Waterfall_Controls->setChecked(
        ui->actionShow_Waterfall->isChecked() &&
        m_wideGraph->controlsVisible());
    ui->actionShow_Waterfall_Time_Drift_Controls->setChecked(
        ui->actionShow_Waterfall->isChecked() &&
        m_wideGraph->timeControlsVisible());

    QMenu *sortBandMenu = new QMenu(this->menuBar()); // ui->menuWindow);
    buildBandActivitySortByMenu(sortBandMenu);
    ui->actionSort_Band_Activity->setMenu(sortBandMenu);
    ui->actionSort_Band_Activity->setEnabled(
        ui->actionShow_Band_Activity->isChecked());

    QMenu *sortCallMenu = new QMenu(this->menuBar()); // ui->menuWindow);
    buildCallActivitySortByMenu(sortCallMenu);
    ui->actionSort_Call_Activity->setMenu(sortCallMenu);
    ui->actionSort_Call_Activity->setEnabled(
        ui->actionShow_Call_Activity->isChecked());

    QMenu *showBandMenu = new QMenu(this->menuBar()); // ui->menuWindow);
    buildShowColumnsMenu(showBandMenu, "band");
    ui->actionShow_Band_Activity_Columns->setMenu(showBandMenu);
    ui->actionShow_Band_Activity_Columns->setEnabled(
        ui->actionShow_Band_Activity->isChecked());

    QMenu *showCallMenu = new QMenu(this->menuBar()); // ui->menuWindow);
    buildShowColumnsMenu(showCallMenu, "call");
    ui->actionShow_Call_Activity_Columns->setMenu(showCallMenu);
    ui->actionShow_Call_Activity_Columns->setEnabled(
        ui->actionShow_Call_Activity->isChecked());

    ui->actionShow_Band_Heartbeats_and_ACKs->setEnabled(
        ui->actionShow_Band_Activity->isChecked());
}

void UI_Constructor::on_actionFocus_Message_Receive_Area_triggered() {
    ui->textEditRX->setFocus();
}

void UI_Constructor::on_actionFocus_Message_Reply_Area_triggered() {
    ui->extFreeTextMsgEdit->setFocus();
}

void UI_Constructor::on_actionFocus_Band_Activity_Table_triggered() {
    ui->tableWidgetRXAll->setFocus();
}

void UI_Constructor::on_actionFocus_Call_Activity_Table_triggered() {
    ui->tableWidgetCalls->setFocus();
}

void UI_Constructor::on_actionClear_All_Activity_triggered() {
    clearActivity();
}

void UI_Constructor::on_actionClear_Band_Activity_triggered() {
    clearBandActivity();
}

void UI_Constructor::on_actionClear_RX_Activity_triggered() {
    clearRXActivity();
}

void UI_Constructor::on_actionClear_Call_Activity_triggered() {
    clearCallActivity();
}

void UI_Constructor::on_actionSetOffset_triggered() {
    bool ok = false;
    auto const currentFreq = freq();
    QString newFreq =
        QInputDialog::getText(this, tr("Set Frequency Offset"),
                              tr("Offset in Hz:"), QLineEdit::Normal,
                              QString("%1").arg(currentFreq), &ok)
            .toUpper()
            .trimmed();
    int offset = newFreq.toInt(&ok);
    if (!ok) {
        return;
    }

    setFreqOffsetForRestore(offset, false);
}

void UI_Constructor::on_actionShow_Fullscreen_triggered(bool checked) {
    auto state = windowState();
    if (checked) {
        state |= Qt::WindowFullScreen;
    } else {
        state &= ~Qt::WindowFullScreen;
    }
    setWindowState(state);
}

void UI_Constructor::on_actionShow_Statusbar_triggered(bool checked) {
    if (!ui->statusBar) {
        return;
    }

    ui->statusBar->setVisible(checked);
}

void UI_Constructor::on_actionShow_Frequency_Clock_triggered(bool checked) {
    auto vsizes = ui->mainSplitter->sizes();
    vsizes[0] = checked ? ui->logHorizontalWidget->minimumHeight() : 0;
    ui->logHorizontalWidget->setVisible(checked);
    ui->mainSplitter->setSizes(vsizes);
}

void UI_Constructor::on_actionShow_Band_Activity_triggered(bool checked) {
    auto hsizes = ui->textHorizontalSplitter->sizes();

    if (m_bandActivityWidth == 0) {
        m_bandActivityWidth = ui->textHorizontalSplitter->width() / 4;
    }

    if (m_callActivityWidth == 0) {
        m_callActivityWidth = ui->textHorizontalSplitter->width() / 4;
    }

    if (m_textActivityWidth == 0) {
        m_textActivityWidth = ui->textHorizontalSplitter->width() / 2;
    }

    if (checked) {
        hsizes[0] = m_bandActivityWidth;
        hsizes[1] = m_textActivityWidth;
        if (hsizes[2])
            hsizes[2] = m_callActivityWidth;

    } else {
        if (hsizes[0])
            m_bandActivityWidth = hsizes[0];
        if (hsizes[1])
            m_textActivityWidth = hsizes[1];
        if (hsizes[2])
            m_callActivityWidth = hsizes[2];
        hsizes[0] = 0;
    }

    ui->textHorizontalSplitter->setSizes(hsizes);
    ui->tableWidgetRXAll->setVisible(checked);
    m_bandActivityWasVisible = checked;
}

void UI_Constructor::on_actionShow_Band_Heartbeats_and_ACKs_triggered(bool) {
    displayBandActivity();
}

void UI_Constructor::on_actionShow_Call_Activity_triggered(bool checked) {
    auto hsizes = ui->textHorizontalSplitter->sizes();

    if (m_bandActivityWidth == 0) {
        m_bandActivityWidth = ui->textHorizontalSplitter->width() / 4;
    }

    if (m_callActivityWidth == 0) {
        m_callActivityWidth = ui->textHorizontalSplitter->width() / 4;
    }

    if (m_textActivityWidth == 0) {
        m_textActivityWidth = ui->textHorizontalSplitter->width() / 2;
    }

    if (checked) {
        if (hsizes[0])
            hsizes[0] = m_bandActivityWidth;
        hsizes[1] = m_textActivityWidth;
        hsizes[2] = m_callActivityWidth;

    } else {
        if (hsizes[0])
            m_bandActivityWidth = hsizes[0];
        if (hsizes[1])
            m_textActivityWidth = hsizes[1];
        if (hsizes[2])
            m_callActivityWidth = hsizes[2];
        hsizes[2] = 0;
    }

    ui->textHorizontalSplitter->setSizes(hsizes);
    ui->tableWidgetCalls->setVisible(checked);
}

void UI_Constructor::on_actionShow_Waterfall_triggered(bool checked) {
    auto vsizes = ui->mainSplitter->sizes();

    if (m_waterfallHeight == 0) {
        m_waterfallHeight = ui->mainSplitter->height() / 4;
    }

    if (checked) {
        vsizes[vsizes.length() - 1] = m_waterfallHeight;

    } else {
        m_waterfallHeight = vsizes[vsizes.length() - 1];
        vsizes[1] += m_waterfallHeight;
        vsizes[vsizes.length() - 1] = 0;
    }

    ui->mainSplitter->setSizes(vsizes);
    ui->bandHorizontalWidget->setVisible(checked);
}

void UI_Constructor::on_actionShow_Waterfall_Controls_triggered(bool checked) {
    m_wideGraph->setControlsVisible(checked);
    if (checked && !ui->bandHorizontalWidget->isVisible()) {
        on_actionShow_Waterfall_triggered(checked);
    }
}

void UI_Constructor::on_actionShow_Waterfall_Time_Drift_Controls_triggered(
    bool checked) {
    m_wideGraph->setTimeControlsVisible(checked);
    if (checked && !ui->bandHorizontalWidget->isVisible()) {
        on_actionShow_Waterfall_triggered(checked);
    }
}

void UI_Constructor::on_actionReset_Window_Sizes_triggered() {
    // auto size = this->centralWidget()->size();

    ui->mainSplitter->setSizes({ui->logHorizontalWidget->minimumHeight(),
                                ui->mainSplitter->height() / 2,
                                ui->macroHorizonalWidget->minimumHeight(),
                                ui->mainSplitter->height() / 4});

    ui->textHorizontalSplitter->setSizes(
        {ui->textHorizontalSplitter->width() / 4,
         ui->textHorizontalSplitter->width() / 2,
         ui->textHorizontalSplitter->width() / 4});

    ui->textVerticalSplitter->setSizes(
        {ui->textVerticalSplitter->height() / 2,
         ui->textVerticalSplitter->height() / 2});
}

void UI_Constructor::on_actionSettings_triggered() { openSettings(); }

void UI_Constructor::openSettings(int tab) {
    m_config.select_tab(tab);

    // things that might change that we need know about
    auto callsign = m_config.my_callsign();
    auto my_grid = m_config.my_grid();
    auto spot_on = m_config.spot_to_reporting_networks();
    if (QDialog::Accepted == m_config.exec()) {
        if (m_config.my_callsign() != callsign) {
            m_baseCall = Radio::base_callsign(m_config.my_callsign());
            // [TODO #50 FIX 2026-06-10 build 235]
            // Refresh ChunkedArqManager's cached callsign so outgoing
            // ARQ super-msg frames carry the NEW callsign in the
            // <FROM>: field. Without this, frames continue carrying
            // the stale (startup-time) callsign until app restart —
            // on-air identity mismatch.
            //
            // FULL callsign (includes /P, /M, /MM etc.) per
            // feedback_callsign_preserve_affixes. NOT base_callsign().
            //
            // Also halt any in-flight ARQ sessions: changing on-air
            // identity mid-transmission is operator-unfriendly and
            // would leave receivers confused (the first N chunks were
            // marked from old callsign; the rest from new). Operator
            // should re-send after settling on a callsign.
            if (m_chunkedArq) {
                if (m_chunkedArq->hasActiveSession()) {
                    qWarning() << "[ARQ] halting in-flight session(s) "
                                  "on callsign change from"
                               << callsign << "to"
                               << m_config.my_callsign();
                    m_chunkedArq->haltAll();
                }
                m_chunkedArq->setMyCall(
                    m_config.my_callsign().trimmed());
            }
        }
        if (m_config.my_callsign() != callsign ||
            m_config.my_grid() != my_grid) {
            statusUpdate();
        }

        enable_DXCC_entity(m_config.DXCC()); // sets text window proportions and
                                             // (re)inits the logbook

        prepareApi();
        prepareSpotting();

        // this will close the connection to PSKReporter if it has been
        // disabled
        if (spot_on && !m_config.spot_to_reporting_networks()) {
            Q_EMIT pskReporterSendReport(true);
        }

        if (m_config.restart_audio_input() &&
            !m_config.audio_input_device().isNull()) {
            Q_EMIT startAudioInputStream(m_config.audio_input_device(),
                                         m_framesAudioInputBuffered, m_detector,
                                         m_config.audio_input_channel());
        }

        if (m_config.restart_audio_output() &&
            !m_config.audio_output_device().isNull()) {
            Q_EMIT initializeAudioOutputStream(
                m_config.audio_output_device(),
                AudioDevice::Mono == m_config.audio_output_channel() ? 1 : 2,
                m_msAudioOutputBuffered);
        }

        if (m_config.restart_notification_audio_output() &&
            !m_config.notification_audio_output_device().isNull()) {
            Q_EMIT initializeNotificationAudioOutputStream(
                m_config.notification_audio_output_device(),
                m_msAudioOutputBuffered);
        }

        displayDialFrequency();
        displayActivity(true);

        setup_status_bar();
        setupJS8();

        m_config.transceiver_online();

        setXIT(freq());

        m_opCall = m_config.opCall();
    }
}

void UI_Constructor::prepareApi() {
    // the udp api is prepared by default (always listening)

    // so, we just need to prepare the tcp api
    bool enabled = m_config.tcpEnabled();
    if (enabled) {
        emit apiSetMaxConnections(m_config.tcp_max_connections());
        emit apiSetServer(m_config.tcp_server_name(),
                          m_config.tcp_server_port());
        emit apiStartServer();
    } else {
        emit apiStopServer();
    }
}

void UI_Constructor::prepareSpotting() {
    bool reportingEnabled = m_config.spot_to_reporting_networks();
    bool aprsEnabled = reportingEnabled &&
                       (m_config.spot_to_aprs() || m_config.spot_to_aprs_relay());

    if (reportingEnabled) {
        spotSetLocal();
        pskSetLocal();
        if (aprsEnabled) {
            aprsSetLocal();
            emit aprsClientSetSkipPercent(0.25);
            emit aprsClientSetServer(m_config.aprs_server_name(),
                                     m_config.aprs_server_port());
        }
        emit aprsClientSetIncomingRelayEnabled(
            aprsEnabled && m_config.spot_to_aprs_relay());
        emit aprsClientSetPaused(!aprsEnabled);
        ui->spotButton->setChecked(true);
    } else {
        emit aprsClientSetPaused(true);
        emit aprsClientSetIncomingRelayEnabled(false);
        ui->spotButton->setChecked(false);
    }
}

void UI_Constructor::on_spotButton_clicked(bool checked) {
    // 1. save setting
    m_config.set_spot_to_reporting_networks(checked);

    // 2. prepare
    prepareApi();
    prepareSpotting();
}

void UI_Constructor::on_monitorButton_clicked(bool checked) {
    if (!m_transmitting) {
        auto prior = m_monitoring;
        monitor(checked);
        if (checked && !prior) {
            if (m_config.monitor_last_used()) {
                // put rig back where it was when last in control
                setRig(m_lastMonitoredFrequency);
                setXIT(freq());
            }
            setFreq(freq()); // ensure FreqCal triggers
        }
        // Get Configuration in/out of strict split and mode checking
        Q_EMIT m_config.sync_transceiver(true, checked);
    } else {
        ui->monitorButton->setChecked(false); // disallow
    }
}

void UI_Constructor::monitor(bool state) {
    ui->monitorButton->setChecked(state);

    // make sure widegraph is running if we are monitoring, otherwise pause it.
    m_wideGraph->setPaused(!state);

    if (state) {
        if (!m_monitoring)
            Q_EMIT resumeAudioInputStream();
    } else {
        Q_EMIT suspendAudioInputStream();
    }
    m_monitoring = state;
}

void UI_Constructor::on_actionAbout_triggered() // Display "About"
{
    CAboutDlg{this}.exec();
}

void UI_Constructor::on_monitorButton_toggled(bool) {
    resetPushButtonToggleText(ui->monitorButton);
}

void UI_Constructor::on_monitorTxButton_toggled(bool checked) {
    resetPushButtonToggleText(ui->monitorTxButton);

    if (!checked) {
        qCDebug(mainwindow_js8)
            << "on_monitorTxButton_toggled(" << checked << ") to stop TX.";
        on_stopTxButton_clicked();
    }
}

void UI_Constructor::on_tuneButton_toggled(bool) {
    resetPushButtonToggleText(ui->tuneButton);
}

void UI_Constructor::on_spotButton_toggled(bool) {
    resetPushButtonToggleText(ui->spotButton);
}

void UI_Constructor::auto_tx_mode(bool state) {
    qCDebug(mainwindow_js8) << "auto_tx_mode(" << state << ")";
    m_auto = state;
    statusUpdate();
    if (state) {
        // Let us not wait until the next polling slot, but prepare transmission
        // now, even though that may waste a few CPU cycles through double work
        // that will be done soon anyway:
        prepareSending(DriftingDateTime::currentMSecsSinceEpoch());
    } else {
        // This function is called recursively from on_stopTxButton_clicked()!
        bool previous_stopTxButtonisLongterm = m_stopTxButtonIsLongterm;
        m_stopTxButtonIsLongterm = false;
        on_stopTxButton_clicked();
        m_stopTxButtonIsLongterm = previous_stopTxButtonisLongterm;
    }
    qCDebug(mainwindow_js8) << "auto_tx_mode(" << state << ") completed.";
}

void UI_Constructor::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Escape:
        on_stopTxButton_clicked();
        stopTx();
        return;
    case Qt::Key_F5:
        on_logQSOButton_clicked();
        return;
    }

    QMainWindow::keyPressEvent(e);
}

void UI_Constructor::f11f12(int const n) {
    if (n == 11)
        setFreq(freq() - 1);
    if (n == 12)
        setFreq(freq() + 1);
}

Radio::Frequency UI_Constructor::dialFrequency() {
    return Frequency{m_rigState.ptt() && m_rigState.split()
                         ? m_rigState.tx_frequency()
                         : m_rigState.frequency()};
}

void UI_Constructor::setSubmode(int submode) {
    // Block mode switch during active TX — stale m_TRperiod causes truncated frames
    if (m_transmitting || m_txFrameCount > 0 || !m_txFrameQueue.isEmpty()) {
        qWarning() << "[UI] setSubmode BLOCKED: submode=" << submode
                    << "m_transmitting=" << m_transmitting
                    << "m_txFrameCount=" << m_txFrameCount
                    << "queueEmpty=" << m_txFrameQueue.isEmpty();
        return;
    }
    m_nSubMode = submode;
    ui->actionModeJS8Normal->setChecked(submode == Varicode::JS8CallNormal);
    ui->actionModeJS8Fast->setChecked(submode == Varicode::JS8CallFast);
    ui->actionModeJS8Turbo->setChecked(submode == Varicode::JS8CallTurbo);
    ui->actionModeJS8Slow->setChecked(submode == Varicode::JS8CallSlow);
    ui->actionModeJS8Ultra->setChecked(submode == Varicode::JS8CallUltra);
#ifdef JS8_ENABLE_FT2
    ui->actionModeFT2->setChecked(submode == Varicode::JS8CallFT2);
#endif

    // Update status-bar mode label. Submode name, with " + ARQ"
    // appended when Auto Repeat Request is active (operator-requested
    // 2026-06-06 — they want the ARQ indicator visible at the bottom
    // of the screen too, not just on the top-right modeButton). Other
    // mode-flag summaries (+MULTI, +AUTO, +HAIL, +HB+ACK) stay on the
    // modeButton — only ARQ surfaces in both places, as the visibility
    // of the reliability mode is most safety-relevant for the operator.
    QString modeText = (submode == Varicode::JS8CallFT2
        ? QString::fromUtf8("\xe2\x9a\xa1 Subspace")
        : JS8::Submode::name(submode));
    if (ui->actionModeReplicatorProtocol &&
        ui->actionModeReplicatorProtocol->isChecked()) {
        modeText += QStringLiteral(" + ARQ");
    }
    mode_label.setText(modeText);

    // Update mode switch buttons — block signals to prevent re-triggering
    if (m_modeBtnNormal) { m_modeBtnNormal->blockSignals(true); m_modeBtnNormal->setChecked(submode == Varicode::JS8CallNormal); m_modeBtnNormal->blockSignals(false); }
    if (m_modeBtnFast)   { m_modeBtnFast->blockSignals(true);   m_modeBtnFast->setChecked(submode == Varicode::JS8CallFast);     m_modeBtnFast->blockSignals(false); }
    if (m_modeBtnTurbo)  { m_modeBtnTurbo->blockSignals(true);  m_modeBtnTurbo->setChecked(submode == Varicode::JS8CallTurbo);   m_modeBtnTurbo->blockSignals(false); }
    if (m_modeBtnSlow)   { m_modeBtnSlow->blockSignals(true);   m_modeBtnSlow->setChecked(submode == Varicode::JS8CallSlow);     m_modeBtnSlow->blockSignals(false); }
    if (m_modeBtnFT2)    { m_modeBtnFT2->blockSignals(true);    m_modeBtnFT2->setChecked(submode == Varicode::JS8CallFT2);       m_modeBtnFT2->blockSignals(false); }

    setupJS8();
    Q_EMIT submodeChanged(Varicode::intToSubmode(submode));
}

// switchSubmode() removed — all mode switches go through setSubmode() which
// calls setupJS8() to update all layers (UI, wideGraph, dec_data.params, period
// timer). Using a "lightweight" switch caused dec_data.params mismatch leading
// to truncated TX frames after mode switch.

void UI_Constructor::updateCurrentBand() {
    QVariant state = ui->readFreq->property("state");
    if (!state.isValid()) {
        return;
    }

    auto dial_frequency = dialFrequency();
    auto const &band_name = m_config.bands()->find(dial_frequency);

    if (m_lastBand == band_name) {
        return;
    }

    cacheActivity(m_lastBand);

    // clear activity on startup if asked or on when the previous band is not
    // empty
    if (m_config.reset_activity() || !m_lastBand.isEmpty()) {
        clearActivity();
    }

    m_wideGraph->setBand(band_name);

    qCDebug(mainwindow_js8) << "setting band" << band_name;

    /**
     * @brief Send WSJT-X Status message on band change
     *
     * When the band changes, send a status update to WSJT-X protocol clients
     * and native JSON API clients (if not conflicting).
     */
    // Send WSJT-X Status message if protocol is enabled (band change triggers
    // status update)
    if (m_wsjtxMessageMapper && m_config.wsjtx_protocol_enabled()) {
        QString dx_call = callsignSelected();
        QString dx_grid = "";
        if (!dx_call.isEmpty() && m_callActivity.contains(dx_call)) {
            dx_grid = m_callActivity[dx_call].grid;
        }
        QString tx_message = m_transmitting ? m_currentMessage : "";

        m_wsjtxMessageMapper->sendStatusUpdate(
            dialFrequency(), freq(),
            "JS8", // mode
            dx_call, m_config.my_callsign(), m_config.my_grid(), dx_grid,
            true, // tx_enabled
            m_transmitting,
            m_decoderBusy || m_monitoring, // decoding
            tx_message);
    }

    // Send native JSON message only if not conflicting with WSJT-X
    bool skip_json = false;
    if (m_config.wsjtx_protocol_enabled() &&
        m_config.wsjtx_server_port() == m_config.udp_server_port() &&
        m_config.wsjtx_server_name() == m_config.udp_server_name()) {
        skip_json = true;
    }

    if (!skip_json) {
        sendNetworkMessage(
            "RIG.FREQ", "",
            {{"_ID", QVariant(-1)},
             {"BAND", QVariant(band_name)},
             {"FREQ", QVariant((quint64)dialFrequency() + freq())},
             {"DIAL", QVariant((quint64)dialFrequency())},
             {"OFFSET", QVariant((quint64)freq())}});
    }
    m_lastBand = band_name;

    clearSelection();
    band_changed();
    restoreActivity(m_lastBand);
}

void UI_Constructor::displayDialFrequency() {
#if 0
    qCDebug(mainwindow_js8) << "rx nominal" << m_freqNominal;
    qCDebug(mainwindow_js8) << "tx nominal" << m_freqTxNominal;
    qCDebug(mainwindow_js8) << "offset set to" << freq() << freq();
#endif

    auto dial_frequency = dialFrequency();
    auto audio_frequency = freq();

    // lookup band
    auto const &band_name = m_config.bands()->find(dial_frequency);

    auto sFreq = Radio::pretty_frequency_MHz_string(dial_frequency);
    ui->currentFreq->setDigitCount(sFreq.length());
    ui->currentFreq->display(sFreq);

    if (m_splitMode && m_transmitting) {
        audio_frequency += m_XIT;
    }
    ui->labDialFreqOffset->setText(QString("%1 Hz").arg(audio_frequency));
}

void UI_Constructor::statusChanged() { statusUpdate(); }

bool UI_Constructor::eventFilter(QObject *object, QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress:
        // fall through
    case QEvent::MouseButtonPress:
        // reset the Tx watchdog
        resetIdleTimer();
        tx_watchdog(false);
        break;

    case QEvent::ChildAdded:
        // ensure our child widgets get added to our event filter
        add_child_to_event_filter(static_cast<QChildEvent *>(event)->child());
        break;

    case QEvent::ChildRemoved:
        // ensure our child widgets get d=removed from our event filter
        remove_child_from_event_filter(
            static_cast<QChildEvent *>(event)->child());
        break;

    case QEvent::MouseButtonDblClick:
        // Handled by EventFilter in UI_Constructor.cpp
        break;

    case QEvent::ToolTip:
        if (!ui->actionShow_Tooltips->isChecked()) {
            return true;
        }

        break;

    default:
        break;
    }
    return QObject::eventFilter(object, event);
}

void UI_Constructor::createStatusBar() // createStatusBar
{
    tx_status_label.setAlignment(Qt::AlignCenter);
    tx_status_label.setMinimumSize(QSize{150, 18});
    tx_status_label.setStyleSheet("QLabel{background-color: #22ff22}");
    tx_status_label.setFrameStyle(QFrame::Panel | QFrame::Sunken);
    statusBar()->addWidget(&tx_status_label);

    config_label.setAlignment(Qt::AlignCenter);
    config_label.setMinimumSize(QSize{80, 18});
    config_label.setFrameStyle(QFrame::Panel | QFrame::Sunken);
    statusBar()->addWidget(&config_label);
    config_label.hide(); // only shown for non-default configuration

    mode_label.setAlignment(Qt::AlignCenter);
    mode_label.setMinimumSize(QSize{160, 18}); // wide enough for "⚡ Subspace"
    mode_label.setStyleSheet("QLabel{background-color: #6699ff}");
    mode_label.setFrameStyle(QFrame::Panel | QFrame::Sunken);
    mode_label.setText("JS8");
    statusBar()->addWidget(&mode_label);

    last_tx_label.setAlignment(Qt::AlignCenter);
    last_tx_label.setMinimumSize(QSize{150, 18});
    last_tx_label.setFrameStyle(QFrame::Panel | QFrame::Sunken);
    statusBar()->addWidget(&last_tx_label);

    statusBar()->addPermanentWidget(&progressBar);
    progressBar.setMinimumSize(QSize{100, 18});
    progressBar.setFormat("%v/%m");

    statusBar()->addPermanentWidget(&wpm_label);
    wpm_label.setMinimumSize(QSize{120, 18});
    wpm_label.setFrameStyle(QFrame::Panel | QFrame::Sunken);
    wpm_label.setAlignment(Qt::AlignCenter);
}

void UI_Constructor::setup_status_bar() { last_tx_label.clear(); }

void UI_Constructor::closeEvent(QCloseEvent *e) {
        if (canSendNetworkMessage()) {
        sendNetworkMessage("STATION.CLOSING", "",
            {{"_ID", QVariant(-1)},
             {"REASON", QVariant("User closed application")}});
    }
    m_valid = false; // suppresses subprocess errors
    m_config.transceiver_offline();
    writeSettings();
    m_guiTimer.stop();
#ifdef JS8_ENABLE_FT2
    m_l2DecodeTimer.stop();
    m_l2DecodeWatcher.waitForFinished();
#endif
    m_prefixes.reset();
    m_shortcuts.reset();
    m_mouseCmnds.reset();
    Q_EMIT finished();

    QMainWindow::closeEvent(e);
}

void UI_Constructor::on_dialFreqUpButton_clicked() {
    setRig(m_freqNominal + 250);
}

void UI_Constructor::on_dialFreqDownButton_clicked() {
    setRig(m_freqNominal - 250);
}

void UI_Constructor::on_actionAdd_Log_Entry_triggered() {
    on_logQSOButton_clicked();
}

void UI_Constructor::on_actionCopyright_Notice_triggered() {
    auto const &message = tr(
        "If you make fair use of any part of this program under terms of the "
        "GNU "
        "General Public License, you must display the following copyright "
        "notice prominently in your derivative work:\n\n"
        "\"The algorithms, source code, look-and-feel of WSJT-X and related "
        "programs, and protocol specifications for the modes FSK441, FT8, JT4, "
        "JT6M, JT9, JT65, JTMS, QRA64, ISCAT, MSK144 are Copyright (C) "
        "2001-2018 by one or more of the following authors: Joseph Taylor, "
        "K1JT; Bill Somerville, G4WJS; Steven Franke, K9AN; Nico Palermo, "
        "IV3NWV; Greg Beam, KI7MT; Michael Black, W9MDB; Edson Pereira, "
        "PY2SDR; "
        "Philip Karn, KA9Q; and other members of the WSJT Development "
        "Group.\n\n"
        "Further, the source code of Subspace Edition contains material Copyright (C) "
        "2018-2019 by Jordan Sherer, KN4CRD.\"");
    JS8MessageBox::warning_message(this, message);
}

/**
 * @brief UI_Constructor::isDecodeReady
 *        determine if decoding is ready for a given submode
 * @param submode - submode to test
 * @param k - current frame count
 * @param k0 - previous frame count
 * @param pCurrentDecodeStart - input pointer to a static integer with the
 * current decode start position
 * @param pNextDecodeStart - input pointer to a static integer with the next
 * decode start position
 * @param pStart - output pointer to the next start position when decode is
 * ready
 * @param pSz - output pointer to the next size when decode is ready
 * @param pCycle - output pointer to the next cycle when decode is ready
 * @return true if decode is ready for this submode, false otherwise
 */
bool UI_Constructor::isDecodeReady(int const submode, qint32 const k,
                                   qint32 const k0, qint32 *pCurrentDecodeStart,
                                   qint32 *pNextDecodeStart, qint32 *pStart,
                                   qint32 *pSz, qint32 *pCycle) {
    if (pCurrentDecodeStart == nullptr || pNextDecodeStart == nullptr) {
        return false;
    }

    qint32 const cycleFrames = JS8::Submode::samplesPerPeriod(submode);
    qint32 const framesNeeded = JS8::Submode::samplesNeeded(submode);
    qint32 const currentCycle = JS8::Submode::computeCycleForDecode(submode, k);
    qint32 const delta = qAbs(k - k0);

    if (delta > cycleFrames) {
        qCDebug(decoder_js8) << "-->" << JS8::Submode::name(submode)
                             << "buffer advance delta" << delta;
    }

    // say, current decode start is 360000 and the next is 540000 (right before
    // we loop) frames needed are 150000 and then we turn off rx until k is
    // 110000 and the cycle frames are 180000 and k0 is a proper 100000 we need
    // to still reset... so, if k is less than the last decode start - cycle
    // frames (in this case 360000-180000, or 180000), then we should reset.
    // but, what if k is now 182000??

    // k=182000
    // k < current (182000 < 360000) true
    // k < max(0, current-cycleframes+framesNeeded) (k < 180000+150000) true

    // k=6000
    // k < current (6000<360000) true
    // k < max(0, 360000-180000+150000) true

    // k=350000
    // k < current (350000<360000) true
    // k < max(0, 360000-350000+150000) false

    // are we in the space between the end of the last decode and the start of
    // the next decode?
    bool const deadAir =
        (k < *pCurrentDecodeStart &&
         k < qMax(0, *pCurrentDecodeStart - cycleFrames + framesNeeded));

    // on buffer loop or init, prepare proper next decode start
    if ((deadAir) || (k < k0) || (delta > cycleFrames) ||
        (*pCurrentDecodeStart == -1) || (*pNextDecodeStart == -1)) {
        *pCurrentDecodeStart = currentCycle * cycleFrames;
        *pNextDecodeStart = *pCurrentDecodeStart + cycleFrames;
    }

    bool const ready = *pCurrentDecodeStart + framesNeeded <= k;

    if (ready) {
        qCDebug(decoder_js8)
            << "-->" << JS8::Submode::name(submode) << "from"
            << *pCurrentDecodeStart << "to"
            << *pCurrentDecodeStart + framesNeeded << "k" << k << "k0" << k0;

        if (pCycle)
            *pCycle = currentCycle;
        if (pStart)
            *pStart = *pCurrentDecodeStart;
        if (pSz)
            *pSz = qMax(framesNeeded, k - (*pCurrentDecodeStart));

        *pCurrentDecodeStart = *pNextDecodeStart;
        *pNextDecodeStart = *pCurrentDecodeStart + cycleFrames;
    }

    return ready;
}

/**
 * @brief UI_Constructor::decode
 *        try decoding
 * @return true if the decoder was activated, false otherwise
 */
bool UI_Constructor::decode(qint32 k) {
    static int k0 = 9999999;
    int kZero = k0;
    k0 = k;
    qCDebug(decoder_js8)
        << "decoder checking if ready..."
        << "k" << k << "k0" << kZero << "busy?" << m_decoderBusy
        << "lock exists?"
        << (QFile{m_config.temp_dir().absoluteFilePath(".lock")}.exists());

    if (k == kZero) {
        qCDebug(decoder_js8) << "--> decoder stream has not advanced";
        return false;
    }

    if (!m_monitoring) {
        qCDebug(decoder_js8) << "--> decoder stream is not active";
        return false;
    }

    bool ready = false;

#if JS8_USE_EXPERIMENTAL_DECODE_TIMING
    ready = decodeEnqueueReady(k, kZero);
    if (ready || !m_decoderQueue.isEmpty()) {
        qCDebug(decoder_js8) << "--> decoder is ready to be run with"
                             << m_decoderQueue.count() << "decode periods";
    }
#else
    ready = decodeEnqueueReadyExperiment(k, kZero);
    if (ready || !m_decoderQueue.isEmpty()) {
        qCDebug(decoder_js8) << "--> decoder is ready to be run with"
                             << m_decoderQueue.count() << "decode periods";
    }
#endif

    //
    // TODO: what follows can likely be pulled out to an async process
    //

    // pause decoder if we are currently transmitting
    if (m_transmitting) {
        // We used to use isMessageQueuedForTransmit, and some form of checking
        // for queued messages but, that just caused problems with missing
        // decodes, so we only pause if we are actually actively transmitting.
        qCDebug(decoder_js8) << "--> decoder paused during transmit";
        return false;
    }

    if (m_decoderBusyStartTime.isValid() &&
        m_decoderBusyStartTime.msecsTo(QDateTime::currentDateTimeUtc()) <
            1000) {
        qCDebug(decoder_js8)
            << "--> decoder paused for 1000 ms after last decode start";
        return false;
    }

    int threshold =
        m_nSubMode == Varicode::JS8CallSlow ? 4000 : 2000; // two seconds
    if (isInDecodeDelayThreshold(threshold)) {
        qCDebug(decoder_js8) << "--> decoder paused for" << threshold
                             << "ms after transmit stop";
        return false;
    }

    // critical section (modifying dec_data)

    qint32 submode = -1;
    if (!decodeProcessQueue(&submode)) {
        return false;
    }

    decodeStart();

    return true;
}

/**
 * @brief UI_Constructor::decodeEnqueueReady
 *        compute the available decoder ranges that can be processed and
 *        place them in the decode queue
 * @param k - the current frame count
 * @param k0 - the previous frame count
 * @return true if decoder ranges were queued, false otherwise
 */
bool UI_Constructor::decodeEnqueueReady(qint32 k, qint32 k0) {
    // compute the next decode for each submode
    // enqueue those decodes that are "ready"
    // on an interval, issue a decode
    int decodes = 0;

    bool couldDecodeA = false;
    qint32 startA = -1;
    qint32 szA = -1;
    qint32 cycleA = -1;

    bool couldDecodeB = false;
    qint32 startB = -1;
    qint32 szB = -1;
    qint32 cycleB = -1;

    bool couldDecodeC = false;
    qint32 startC = -1;
    qint32 szC = -1;
    qint32 cycleC = -1;

    bool couldDecodeE = false;
    qint32 startE = -1;
    qint32 szE = -1;
    qint32 cycleE = -1;

#if JS8_ENABLE_JS8I
    bool couldDecodeI = false;
    qint32 startI = -1;
    qint32 szI = -1;
    qint32 cycleI = -1;
#endif

    static qint32 currentDecodeStartA = -1;
    static qint32 nextDecodeStartA = -1;
    qCDebug(decoder_js8) << "? NORMAL   " << currentDecodeStartA
                         << nextDecodeStartA;
    couldDecodeA =
        isDecodeReady(Varicode::JS8CallNormal, k, k0, &currentDecodeStartA,
                      &nextDecodeStartA, &startA, &szA, &cycleA);

    static qint32 currentDecodeStartB = -1;
    static qint32 nextDecodeStartB = -1;
    qCDebug(decoder_js8) << "? FAST     " << currentDecodeStartB
                         << nextDecodeStartB;
    couldDecodeB =
        isDecodeReady(Varicode::JS8CallFast, k, k0, &currentDecodeStartB,
                      &nextDecodeStartB, &startB, &szB, &cycleB);

    static qint32 currentDecodeStartC = -1;
    static qint32 nextDecodeStartC = -1;
    qCDebug(decoder_js8) << "? TURBO    " << currentDecodeStartC
                         << nextDecodeStartC;
    couldDecodeC =
        isDecodeReady(Varicode::JS8CallTurbo, k, k0, &currentDecodeStartC,
                      &nextDecodeStartC, &startC, &szC, &cycleC);

    static qint32 currentDecodeStartE = -1;
    static qint32 nextDecodeStartE = -1;
    qCDebug(decoder_js8) << "? SLOW     " << currentDecodeStartE
                         << nextDecodeStartE;
    couldDecodeE =
        isDecodeReady(Varicode::JS8CallSlow, k, k0, &currentDecodeStartE,
                      &nextDecodeStartE, &startE, &szE, &cycleE);

#if JS8_ENABLE_JS8I
    static qint32 currentDecodeStartI = -1;
    static qint32 nextDecodeStartI = -1;
    qCDebug(decoder_js8) << "? ULTRA    " << currentDecodeStartI
                         << nextDecodeStartI;
    couldDecodeI =
        isDecodeReady(Varicode::JS8CallUltra, k, k0, &currentDecodeStartI,
                      &nextDecodeStartI, &startI, &szI, &cycleI);
#endif

    if (couldDecodeA) {
        DecodeParams d;
        d.submode = Varicode::JS8CallNormal;
        d.start = startA;
        d.sz = szA;
        m_decoderQueue.append(d);
        decodes++;
    }

    if (couldDecodeB) {
        DecodeParams d;
        d.submode = Varicode::JS8CallFast;
        d.start = startB;
        d.sz = szB;
        m_decoderQueue.append(d);
        decodes++;
    }

    if (couldDecodeC) {
        DecodeParams d;
        d.submode = Varicode::JS8CallTurbo;
        d.start = startC;
        d.sz = szC;
        m_decoderQueue.append(d);
        decodes++;
    }

    if (couldDecodeE) {
        DecodeParams d;
        d.submode = Varicode::JS8CallSlow;
        d.start = startE;
        d.sz = szE;
        m_decoderQueue.append(d);
        decodes++;
    }

#if JS8_ENABLE_JS8I
    if (couldDecodeI) {
        DecodeParams d;
        d.submode = Varicode::JS8CallUltra;
        d.start = startI;
        d.sz = szI;
        m_decoderQueue.append(d);
        decodes++;
    }
#endif

#ifdef JS8_ENABLE_FT2
    // FT2 uses its own cycle calculation (not JS8::Submode which would throw)
    {
        static qint32 currentDecodeStartFT2 = -1;
        static qint32 nextDecodeStartFT2 = -1;
        constexpr qint32 ft2CycleFrames = FT2_TX_PERIOD_MS * JS8_RX_SAMPLE_RATE / 1000;
        constexpr qint32 ft2FramesNeeded = FT2_NMAX;

        // [TODO.md #58 build 268] m_arqMultiModeOverride is the
        // sticky runtime flag that ARQ activation sets. Once true,
        // FT2/Subspace decode runs even when the operator is in a
        // legacy submode and the persisted multi-decoder action is
        // unchecked. The override is set-once for the program run —
        // never cleared — so a single ARQ contact unlocks Subspace
        // RX for the remainder of the session without touching
        // Configuration / QSettings. See setupJS8 entry handler and
        // the auto-enable site in processCommandActivity.cpp.
        if (m_nSubMode == Varicode::JS8CallFT2 ||
            ui->actionModeMultiDecoder->isChecked() ||
            m_arqMultiModeOverride) {
            qint32 currentCycle = (k / ft2CycleFrames) %
                                  (JS8_RX_SAMPLE_SIZE / ft2CycleFrames);
            qint32 delta = qAbs(k - k0);

            if ((k < k0) || (delta > ft2CycleFrames) ||
                (currentDecodeStartFT2 == -1) || (nextDecodeStartFT2 == -1)) {
                currentDecodeStartFT2 = currentCycle * ft2CycleFrames;
                nextDecodeStartFT2 = currentDecodeStartFT2 + ft2CycleFrames;
            }

            if (currentDecodeStartFT2 + ft2FramesNeeded <= k) {
                // Primary decode at cycle boundary
                DecodeParams d;
                d.submode = Varicode::JS8CallFT2;
                d.start = currentDecodeStartFT2;
                if (d.start < 0) d.start += JS8_RX_SAMPLE_SIZE;
                if (d.start >= JS8_RX_SAMPLE_SIZE) d.start -= JS8_RX_SAMPLE_SIZE;
                d.sz = qMax(ft2FramesNeeded, k - currentDecodeStartFT2);
                m_decoderQueue.append(d);
                decodes++;

                qCDebug(decoder_js8) << "[FT2-SYNC] decodeEnqueueReady:"
                    << "primary.start=" << d.start
                    << "k=" << k;

                currentDecodeStartFT2 = nextDecodeStartFT2;
                nextDecodeStartFT2 = currentDecodeStartFT2 + ft2CycleFrames;
            }
        }
    }
#endif

    return decodes > 0;
}

/**
 * @brief UI_Constructor::decodeEnqueueReadyExperiment
 *        compute the available decoder ranges that can be processed and
 *        place them in the decode queue
 *
 *        experiment with decoding on a much shorter interval than usual
 *
 * @param k - the current frame count
 * @param k0 - the previous frame count
 * @return true if decoder ranges were queued, false otherwise
 */
bool UI_Constructor::decodeEnqueueReadyExperiment(qint32 k, qint32 /*k0*/) {
    // TODO: make this non-static field of UI_Constructor?
    // map of last decode positions for each submode
    // static QMap<qint32, qint32> m_lastDecodeStartMap;

    // TODO: make this non-static field of UI_Constructor?
    // map of submodes to decode + optional alternate decode positions
    static QMap<qint32, QList<qint32>> submodes = {
        {Varicode::JS8CallSlow, {0}},
        {Varicode::JS8CallNormal, {0}},
        {Varicode::JS8CallFast, {0}},  // NORMAL: 0, 10, 20    --- ALT: 15, 25
        {Varicode::JS8CallTurbo, {0}}, // NORMAL: 0, 6, 12, 18 --- ALT: 15, 21,
                                       // 27
#if JS8_ENABLE_JS8I
        {Varicode::JS8CallUltra, {0}},
#endif
    };

    static qint32 maxSamples = JS8_RX_SAMPLE_SIZE;
    static qint32 oneSecondSamples = JS8_RX_SAMPLE_RATE;

    int decodes = 0;

    // do we have a better way to check this?
    // [TODO.md #58 build 269] Fold the sticky ARQ multi-mode latch
    // into the `multi` flag so BOTH the legacy-mode dispatcher (uses
    // `multi`) and the FT2 dispatcher below see the same effective
    // gate. Build 268 added the override at the FT2 site only; the
    // legacy-mode/alternate-positions block at line ~2071 also uses
    // this `multi` and was therefore still blind to the override.
    bool multi = ui->actionModeMultiDecoder->isChecked() ||
                 m_arqMultiModeOverride;

    // do we need to process alternate positions?
    bool skipAlt = true;

    foreach (auto submode, submodes.keys()) {
        // do we have a better way to check this?
        bool everySecond = m_wideGraph->shouldAutoSyncSubmode(submode);

        // skip if multi is disabled and this mode is not the current submode
        // and we're not autosyncing this mode
        if (!everySecond && !multi && submode != m_nSubMode) {
            continue;
        }

        // check all alternate decode positions
        foreach (auto alt, submodes.value(submode)) {
            // skip alt decode positions if needed
            if (skipAlt && alt != 0) {
                continue;
            }

            // skip alts if we are decoding every second
            if (everySecond && alt != 0) {
                continue;
            }

            qint32 const cycle = JS8::Submode::computeAltCycleForDecode(
                submode, k, alt * oneSecondSamples);
            qint32 const cycleFrames = JS8::Submode::samplesPerPeriod(submode);
            qint32 const cycleFramesNeeded =
                (submode == Varicode::JS8CallTurbo ||
                 submode == Varicode::JS8CallUltra)
                    ? JS8::Submode::samplesNeeded(submode)
                    : JS8::Submode::samplesForSymbols(submode);
            qint32 cycleFramesReady = k - (cycle * cycleFrames);
            if (cycleFramesReady < 0) {
                cycleFramesReady = k + (maxSamples - (cycle * cycleFrames));
            }

            if (!m_lastDecodeStartMap.contains(submode)) {
                m_lastDecodeStartMap[submode] = cycle * cycleFrames;
            }

            qint32 lastDecodeStart = m_lastDecodeStartMap[submode];
            qint32 incrementedBy = k - lastDecodeStart;
            if (k < lastDecodeStart) {
                incrementedBy = maxSamples - lastDecodeStart + k;
            }
            qCDebug(decoder_js8)
                << JS8::Submode::name(submode) << "alt" << alt << "cycle"
                << cycle << "cycle frames" << cycleFrames << "cycle start"
                << cycle * cycleFrames << "cycle end"
                << (cycle + 1) * cycleFrames << "k" << k << "frames ready"
                << cycleFramesReady << "incremeted by" << incrementedBy;

            if (everySecond && incrementedBy >= oneSecondSamples) {
                DecodeParams d;
                d.submode = submode;
                d.sz = cycleFrames;
                d.start = k - d.sz;
                if (d.start < 0) {
                    d.start += maxSamples;
                }
                m_decoderQueue.append(d);
                decodes++;

                // keep track of last decode position
                m_lastDecodeStartMap[submode] = k;
            } else if ((incrementedBy >= 1.5 * oneSecondSamples &&
                        cycleFramesReady >=
                            cycleFramesNeeded) || // within every 3/2 seconds
                                                  // for normal positions
                       (incrementedBy >= oneSecondSamples &&
                        cycleFramesReady >=
                            cycleFramesNeeded -
                                1.5 * oneSecondSamples) || // within the last
                                                           // 3/2 seconds of a
                                                           // new cycle
                       (incrementedBy >= oneSecondSamples &&
                        cycleFramesReady <
                            1.5 * oneSecondSamples) // within the first 3/2
                                                    // seconds of a new cycle
            ) {
                DecodeParams d;
                d.submode = submode;
                d.start = cycle * cycleFrames;
                d.sz = cycleFramesReady;
                m_decoderQueue.append(d);
                decodes++;

                // keep track of last decode position
                m_lastDecodeStartMap[submode] = k;
            }
        }
    }

#ifdef JS8_ENABLE_FT2
    // FT2 handled separately — uses its own cycle params, not JS8::Submode
    if (m_nSubMode == Varicode::JS8CallFT2 || multi) {
        constexpr qint32 ft2CycleFrames = FT2_TX_PERIOD_MS * JS8_RX_SAMPLE_RATE / 1000;
        constexpr qint32 ft2FramesNeeded = FT2_NMAX;
        qint32 ft2Cycle = (k / ft2CycleFrames) %
                          (maxSamples / ft2CycleFrames);

        if (!m_lastDecodeStartMap.contains(Varicode::JS8CallFT2)) {
            m_lastDecodeStartMap[Varicode::JS8CallFT2] = ft2Cycle * ft2CycleFrames;
        }

        qint32 lastDecodeStart = m_lastDecodeStartMap[Varicode::JS8CallFT2];
        qint32 incrementedBy = k - lastDecodeStart;
        if (k < lastDecodeStart) {
            incrementedBy = maxSamples - lastDecodeStart + k;
        }

        qint32 cycleFramesReady = k - (ft2Cycle * ft2CycleFrames);
        if (cycleFramesReady < 0) {
            cycleFramesReady = k + (maxSamples - (ft2Cycle * ft2CycleFrames));
        }

        if ((incrementedBy >= 1.5 * oneSecondSamples &&
             cycleFramesReady >= ft2FramesNeeded) ||
            (incrementedBy >= oneSecondSamples &&
             cycleFramesReady >= ft2FramesNeeded - 1.5 * oneSecondSamples) ||
            (incrementedBy >= oneSecondSamples &&
             cycleFramesReady < 1.5 * oneSecondSamples)) {
            // Primary decode at cycle boundary
            qint32 primaryStart = ft2Cycle * ft2CycleFrames;
            if (primaryStart < 0) primaryStart += maxSamples;
            if (primaryStart >= maxSamples) primaryStart -= maxSamples;

            DecodeParams d;
            d.submode = Varicode::JS8CallFT2;
            d.start = primaryStart;
            d.sz = cycleFramesReady;
            m_decoderQueue.append(d);
            decodes++;

            qCDebug(decoder_js8) << "[FT2-SYNC] decodeEnqueueReadyExperiment:"
                << "primary.start=" << primaryStart
                << "k=" << k;

            m_lastDecodeStartMap[Varicode::JS8CallFT2] = k;
        }
    }
#endif

    return decodes > 0;
}

/**
 * @brief UI_Constructor::decodeProcessQueue
 *        process the decode queue by merging available decode ranges
 *        into the dec_data shared structure for the decoder to process
 * @param pSubmode - the lowest speed submode in this iteration
 * @return true if the decoder is ready to be run, false otherwise
 */
bool UI_Constructor::decodeProcessQueue(qint32 *pSubmode) {
    // critical section
    QMutexLocker mutex(m_detector->getMutex());

    if (m_decoderBusy) {
        int seconds =
            m_decoderBusyStartTime.secsTo(QDateTime::currentDateTimeUtc());
        if (seconds > 60) {
            qCDebug(decoder_js8) << "--> decoder should be killed!"
                                 << QString("(%1 seconds)").arg(seconds);
        } else if (seconds > 30) {
            qCDebug(decoder_js8) << "--> decoder is hanging!"
                                 << QString("(%1 seconds)").arg(seconds);
        } else {
            qCDebug(decoder_js8) << "--> decoder is busy!";
        }

        return false;
    }

    if (m_decoderQueue.isEmpty()) {
        qCDebug(decoder_js8) << "--> decoder has nothing to process!";
        return false;
    }

    int submode = -1;
    int maxDecodes = 1;

    // [TODO.md #58 build 269] Same OR-with-override as the other
    // `multi` definitions — without it the decoder dispatch picks
    // maxDecodes=1 and the FT2 enqueue gets capped out of the queue
    // when the operator is in a legacy mode with ARQ-driven multi-
    // mode active.
    bool multi = ui->actionModeMultiDecoder->isChecked() ||
                 m_arqMultiModeOverride;
    if (multi) {
        maxDecodes = JS8_ENABLE_JS8I ? 5 : 4;
    }

    int count = m_decoderQueue.count();
    if (count > maxDecodes) {
        qCDebug(decoder_js8) << "--> decoder skipping at least 1 decode cycle"
                             << "count" << count << "max" << maxDecodes;
    }

    // default to no submodes being decoded, then bitwise OR the modes together
    // to decode them all at once
    dec_data.params.nsubmodes = 0;
#ifdef JS8_ENABLE_FT2
    dec_data.params.kszFT2b = 0; // no overlap
#endif

    while (!m_decoderQueue.isEmpty()) {
        auto params = m_decoderQueue.front();
        m_decoderQueue.removeFirst();

        // skip if we are not in multi mode and the submode doesn't equal the
        // global submode
        if (!multi && params.submode != m_nSubMode) {
            continue;
        }

        if (submode == -1 || params.submode < submode) {
            submode = params.submode;
        }

        switch (params.submode) {
        case Varicode::JS8CallNormal:
            dec_data.params.kposA = params.start;
            dec_data.params.kszA = params.sz;
            dec_data.params.nsubmodes |= (params.submode + 1);
            break;
        case Varicode::JS8CallFast:
            dec_data.params.kposB = params.start;
            dec_data.params.kszB = params.sz;
            dec_data.params.nsubmodes |= (params.submode << 1);
            break;
        case Varicode::JS8CallTurbo:
            dec_data.params.kposC = params.start;
            dec_data.params.kszC = params.sz;
            dec_data.params.nsubmodes |= (params.submode << 1);
            break;
        case Varicode::JS8CallSlow:
            dec_data.params.kposE = params.start;
            dec_data.params.kszE = params.sz;
            dec_data.params.nsubmodes |= (params.submode << 1);
            break;
#if JS8_ENABLE_JS8I
        case Varicode::JS8CallUltra:
            dec_data.params.kposI = params.start;
            dec_data.params.kszI = params.sz;
            dec_data.params.nsubmodes |= (params.submode << 1);
            break;
#endif
#ifdef JS8_ENABLE_FT2
        case Varicode::JS8CallFT2:
            dec_data.params.kposFT2 = params.start;
            dec_data.params.kszFT2 = params.sz;
            // Standard FT2 decoder runs alongside L2. With C++ port
            // (no Fortran lock conflict), both can run concurrently.
            // Standard decoder provides accurate SNR via getCandidates.
            dec_data.params.nsubmodes |= 16; // bit 4
            break;
#endif
        }
    }

    if (submode == -1) {
        qCDebug(decoder_js8) << "--> decoder has no segments to decode!";
        return false;
    }

    dec_data.params.syncStats = (m_wideGraph->shouldDisplayDecodeAttempts() ||
                                 m_wideGraph->isAutoSyncEnabled());
    dec_data.params.newdat = 1;

    auto const period_unsigned = JS8::Submode::period(submode);
    // Need to use a signed integer here,
    auto const period_signed = (int)period_unsigned;
    // as (2 - period_unsigned) results in an enourmeous number close to 2**32.
    auto const t =
        DriftingDateTime::currentDateTimeUtc().addSecs(2 - period_signed);
    auto const ihr = t.toString("hh").toInt();
    auto const imin = t.toString("mm").toInt();
    auto const isec = t.toString("ss").toInt();

    dec_data.params.nutc = code_time(ihr, imin, isec - isec % period_unsigned);
    dec_data.params.nfqso = freq();
    dec_data.params.nfa =
        m_wideGraph->filterEnabled() ? m_wideGraph->filterMinimum() : 0;
    dec_data.params.nfb =
        m_wideGraph->filterEnabled() ? m_wideGraph->filterMaximum() : 5000;

    if (dec_data.params.nutc < m_nutc0)
        m_RxLog = 1; // Date and Time to ALL.TXT
    if (dec_data.params.newdat == 1)
        m_nutc0 = dec_data.params.nutc;

    // keep track of the minimum submode
    if (pSubmode)
        *pSubmode = submode;

    return true;
}

/**
 * @brief UI_Constructor::decodeStart
 *        copy the dec_data structure to shared memory and
 *        remove the lock file to start the decoding process
 */
void UI_Constructor::decodeStart() {
    // critical section
    QMutexLocker mutex(m_detector->getMutex());

    if (m_decoderBusy) {
        qCDebug(decoder_js8) << "--> decoder cannot start...busy (busy flag)";
        return;
    }

#ifdef JS8_ENABLE_FT2
    // Standard FT2 decoder is disabled when L2 is active (nsubmodes bit 4
    // not set), so no Fortran state overlap — no need to defer.
#endif

    // Mark the decoder busy; decodeDone is responsible for marking
    // the decode _not_ busy

    decodeBusy(true);
    qCDebug(decoder_js8) << "--> decoder starting"
                         << " --> kin:" << dec_data.params.kin
                         << " --> newdat:" << dec_data.params.newdat
                         << " --> nsubmodes:" << dec_data.params.nsubmodes
                         << " --> A:" << dec_data.params.kposA
                         << dec_data.params.kposA + dec_data.params.kszA
                         << QString("(%1)").arg(dec_data.params.kszA)
                         << " --> B:" << dec_data.params.kposB
                         << dec_data.params.kposB + dec_data.params.kszB
                         << QString("(%1)").arg(dec_data.params.kszB)
                         << " --> C:" << dec_data.params.kposC
                         << dec_data.params.kposC + dec_data.params.kszC
                         << QString("(%1)").arg(dec_data.params.kszC)
                         << " --> E:" << dec_data.params.kposE
                         << dec_data.params.kposE + dec_data.params.kszE
                         << QString("(%1)").arg(dec_data.params.kszE)
                         << " --> I:" << dec_data.params.kposI
                         << dec_data.params.kposI + dec_data.params.kszI
                         << QString("(%1)").arg(dec_data.params.kszI);

    m_decoder.decode();
}

/**
 * @brief UI_Constructor::decodeBusy
 *        mark the decoder as currently busy (to prevent overlapping decodes)
 * @param b - true if busy, false otherwise
 */
void UI_Constructor::decodeBusy(bool b) // decodeBusy()
{
    m_decoderBusy = b;

    if (m_decoderBusy) {
        tx_status_label.setText("Decoding");

        m_decoderBusyStartTime = QDateTime::
            currentDateTimeUtc(); // DriftingDateTime::currentDateTimeUtc();
        m_decoderBusyFreq = dialFrequency();
        m_decoderBusyBand = m_config.bands()->find(m_decoderBusyFreq);
    }
}

/**
 * @brief UI_Constructor::decodeDone
 *        clean up after a decode is finished
 */
void UI_Constructor::decodeDone() {
    // critical section
    QMutexLocker mutex(m_detector->getMutex());

    dec_data.params.newdat = false;
    m_RxLog = 0;

    // cleanup old cached messages (messages > submode period old)

    std::erase_if(m_messageDupeCache, [](auto const &it) {
        return it.second.secsTo(QDateTime::currentDateTimeUtc()) >
               JS8::Submode::period(it.first.submode);
    });

    decodeBusy(false);
}

QDateTime UI_Constructor::nextTransmitCycle() {
    auto timestamp = DriftingDateTime::currentDateTimeUtc();

    // remove milliseconds
    auto t = timestamp.time();
    t.setHMS(t.hour(), t.minute(), t.second());
    timestamp.setTime(t);

    // round to 15 second increment
    int secondsSinceEpoch = (timestamp.toMSecsSinceEpoch() / 1000);
    int delta = roundUp(secondsSinceEpoch, m_TRperiod) + 1 - secondsSinceEpoch;
    timestamp = timestamp.addSecs(delta);

    return timestamp;
}

void processDecodeEvent(); // JS8_Mainwindow/processDecodeEvent.cpp

bool UI_Constructor::hasExistingMessageBufferToMe(int *const pOffset) {
    for (auto const [offset, buffer] : m_messageBuffer.asKeyValueRange()) {
        // if this is a valid buffer and it's to me...
        if (buffer.cmd.utcTimestamp.isValid() &&
            buffer.cmd.to == m_config.my_callsign()) {
            if (pOffset)
                *pOffset = offset;
            return true;
        }
    }

    return false;
}

bool UI_Constructor::hasExistingMessageBuffer(int submode, int offset,
                                              bool drift, int *pPrevOffset) {
    if (m_messageBuffer.contains(offset)) {
        if (pPrevOffset)
            *pPrevOffset = offset;
        return true;
    }

    int const range = JS8::Submode::rxThreshold(submode);

    QList<int> offsets = generateOffsets(offset - range, offset + range);

    foreach (int prevOffset, offsets) {
        if (!m_messageBuffer.contains(prevOffset)) {
            continue;
        }

        if (drift) {
            m_messageBuffer[offset] = m_messageBuffer[prevOffset];
            m_messageBuffer.remove(prevOffset);
        }

        if (pPrevOffset)
            *pPrevOffset = prevOffset;
        return true;
    }

    return false;
}

bool UI_Constructor::hasClosedExistingMessageBuffer(int offset) {
#if 0
    int range = 10;
    if(m_nSubMode == Varicode::JS8CallFast){ range = 16; }
    if(m_nSubMode == Varicode::JS8CallTurbo){ range = 32; }

    return offset - range <= m_lastClosedMessageBufferOffset && m_lastClosedMessageBufferOffset <= offset + range;
#elif 0
    int range = 10;
    if (m_nSubMode == Varicode::JS8CallFast) {
        range = 16;
    }
    if (m_nSubMode == Varicode::JS8CallTurbo) {
        range = 32;
    }

    return m_lastClosedMessageBufferOffset - range <= offset &&
           offset <= m_lastClosedMessageBufferOffset + range;
#else
    Q_UNUSED(offset);
#endif
    return false;
}

void UI_Constructor::logCallActivity(CallDetail d, bool spot) {
    // don't log empty calls
    if (d.call.trimmed().isEmpty()) {
        return;
    }

    // don't log relay calls
    if (d.call.contains(">")) {
        return;
    }

    // [TODO.md #66 build 275] Don't log MYCALL as a remote station.
    // Single-point fix at the choke point — at least 7 upstream sites
    // (processDecodeEvent / processCommandActivity / processRxActivity)
    // pass `cd.call = attribFrom` straight through, so trapping it
    // here covers every path: audio loopback / RX-during-TX, TCP API
    // echo, third-party relay carrying our call, recall/replay.
    //
    // General rule (operator preference, 2026-06-15): compare on the
    // FULL callsign, not base. WM8Q operating from home filters out
    // "WM8Q" only; "WM8Q/P" (the operator's portable rig, same person
    // legally) is a separate logical station and SHOULD appear in
    // Call Activity so the operator can see their own field
    // operations from a stationary console.
    QString const myCall = m_config.my_callsign().trimmed();
    if (!myCall.isEmpty() &&
        d.call.compare(myCall, Qt::CaseInsensitive) == 0) {
        return;
    }

    if (m_callActivity.contains(d.call)) {
        // update (keep grid)
        CallDetail old = m_callActivity[d.call];
        if (d.grid.isEmpty() && !old.grid.isEmpty()) {
            d.grid = old.grid;
        }
        if (!d.ackTimestamp.isValid() && old.ackTimestamp.isValid()) {
            d.ackTimestamp = old.ackTimestamp;
        }
        if (!d.cqTimestamp.isValid() && old.cqTimestamp.isValid()) {
            d.cqTimestamp = old.cqTimestamp;
        }
        // Sub-band-aware offset preservation: a frame in the HB/hailing
        // sub-band (500-1000 Hz) is presence-only traffic, not QSO content.
        // If the stored offset is outside the sub-band, keep it as the
        // last-known QSO operating frequency rather than overwriting with
        // the sub-band number. Non-sub-band updates always win; same-sub-
        // band updates replace normally.
        constexpr int subbandLow = 500;
        constexpr int subbandHigh = 1000;
        bool newInSub = (d.offset >= subbandLow && d.offset <= subbandHigh);
        bool oldOutOfSub = (old.offset < subbandLow || old.offset > subbandHigh);
        if (newInSub && oldOutOfSub) {
            d.offset = old.offset;
        }
        m_callActivity[d.call] = d;
    } else {
        // create
        m_callActivity[d.call] = d;

        // notification of old and new callsigns
        if (m_logBook.hasWorkedBefore(d.call, "")) {
            tryNotify("call_old", d.submode);
        } else {
            tryNotify("call_new", d.submode);
        }
    }

    // enqueue for spotting to psk reporter
    if (spot) {
        m_rxCallQueue.append(d);
    }

    // Mark the call-list model dirty so the next once-per-second tick
    // does a full rebuild. Cheap — plain int increment; wrapping is
    // harmless since we only compare for equality against the last
    // rendered version.
    ++m_callActivityVersion;
}

void UI_Constructor::logHeardGraph(QString from, QString to) {
    auto my_callsign = m_config.my_callsign();

    // hearing
    if (m_heardGraphOutgoing.contains(my_callsign)) {
        m_heardGraphOutgoing[my_callsign].insert(from);
    } else {
        m_heardGraphOutgoing[my_callsign].insert(from);
    }

    // heard by
    if (m_heardGraphIncoming.contains(from)) {
        m_heardGraphIncoming[from].insert(my_callsign);
    } else {
        m_heardGraphIncoming[from] = {my_callsign};
    }

    if (to == "@ALLCALL") {
        return;
    }

    // hearing
    if (m_heardGraphOutgoing.contains(from)) {
        m_heardGraphOutgoing[from].insert(to);
    } else {
        m_heardGraphOutgoing[from] = {to};
    }

    // heard by
    if (m_heardGraphIncoming.contains(to)) {
        m_heardGraphIncoming[to].insert(from);
    } else {
        m_heardGraphIncoming[to] = {from};
    }
}

QString UI_Constructor::lookupCallInCompoundCache(QString const &call) {
    QString myBaseCall = Radio::base_callsign(m_config.my_callsign());
    if (call == myBaseCall) {
        return m_config.my_callsign();
    }
    return m_compoundCallCache.value(call, call);
}

void UI_Constructor::spotReport(int const submode, int const dial,
                                int const offset, int const snr,
                                QString const &callsign, QString const &grid) {
    if (!m_config.spot_to_reporting_networks() ||
        (m_config.spot_blacklist().contains(callsign) ||
         m_config.spot_blacklist().contains(Radio::base_callsign(callsign))))
        return;

    Q_EMIT spotClientEnqueueSpot(callsign, grid, submode, dial, offset, snr);
}

void UI_Constructor::spotCmd(CommandDetail const &cmd) {
    if (!m_config.spot_to_reporting_networks() ||
        (m_config.spot_blacklist().contains(cmd.from) ||
         m_config.spot_blacklist().contains(Radio::base_callsign(cmd.from))))
        return;

    QString cmdStr = cmd.cmd;

    if (!cmdStr.trimmed().isEmpty()) {
        cmdStr = Varicode::lstrip(cmd.cmd);
    }

    Q_EMIT spotClientEnqueueCmd(cmdStr, cmd.from, cmd.to, cmd.relayPath,
                                cmd.text, cmd.grid, cmd.extra, cmd.submode,
                                cmd.dial, cmd.offset, cmd.snr);
}

// KN4CRD: @APRSIS CMD :EMAIL-2  :email@domain.com booya{1
void UI_Constructor::spotAprsCmd(CommandDetail const &cmd) {
    if (!m_config.spot_to_reporting_networks())
        return;
    if (!m_config.spot_to_aprs())
        return;
    if (m_config.spot_blacklist().contains(cmd.from) ||
        m_config.spot_blacklist().contains(Radio::base_callsign(cmd.from)))
        return;

    if (cmd.cmd != " CMD")
        return;

    qCDebug(mainwindow_js8)
        << "APRSISClient Enqueueing Third Party Text" << cmd.from << cmd.text;

    auto by_call = APRSISClient::replaceCallsignSuffixWithSSID(
        m_config.my_callsign(), Radio::base_callsign(m_config.my_callsign()));
    auto from_call = APRSISClient::replaceCallsignSuffixWithSSID(
        cmd.from, Radio::base_callsign(cmd.from));

    // we use a queued signal here so we can process these spots in a network
    // thread to prevent blocking the gui/decoder while waiting on TCP
    emit aprsClientEnqueueThirdParty(by_call, from_call, cmd.text);
}

void UI_Constructor::spotAprsGrid(int dial, int offset, int snr,
                                  QString callsign, QString grid) {
    if (!m_config.spot_to_reporting_networks())
        return;
    if (!m_config.spot_to_aprs())
        return;
    if (m_config.spot_blacklist().contains(callsign) ||
        m_config.spot_blacklist().contains(Radio::base_callsign(callsign)))
        return;
    if (grid.length() < 4)
        return;

    Frequency frequency = dial + offset;

    auto comment = QString("%1MHz %2dB")
                       .arg(Radio::frequency_MHz_string(frequency))
                       .arg(Varicode::formatSNR(snr));
    if (callsign.contains("/")) {
        comment = QString("%1 %2").arg(callsign).arg(comment);
    }

    auto by_call = APRSISClient::replaceCallsignSuffixWithSSID(
        m_config.my_callsign(), Radio::base_callsign(m_config.my_callsign()));
    auto from_call = APRSISClient::replaceCallsignSuffixWithSSID(
        callsign, Radio::base_callsign(callsign));

    // we use a queued signal here so we can process these spots in a network
    // thread to prevent blocking the gui/decoder while waiting on TCP
    emit aprsClientEnqueueSpot(by_call, from_call, grid, comment);
}

void UI_Constructor::pskLogReport(QString const &mode, int const dial,
                                  int const offset, int const snr,
                                  QString const &callsign, QString const &grid,
                                  QDateTime const &utcTimestamp) {
    if (!m_config.spot_to_reporting_networks() ||
        (m_config.spot_blacklist().contains(callsign) ||
         m_config.spot_blacklist().contains(Radio::base_callsign(callsign))))
        return;

    Q_EMIT pskReporterAddRemoteStation(callsign, grid, dial + offset, mode, snr,
                                       utcTimestamp);
}

void UI_Constructor::refuseToSendIn30mWSPRBand() {
    if (m_transmitting or m_auto or m_tune) {
        m_dateTimeLastTX = DriftingDateTime::currentDateTimeLocal();

        // Don't transmit another mode in the 30 m WSPR sub-band
        Frequency onAirFreq = m_freqNominal + freq();

        // qCDebug(mainwindow_js8) << "transmitting on" << onAirFreq;

        if (10139900 <= onAirFreq && onAirFreq <= 10140320) {
            qCWarning(mainwindow_js8)
                << "QRG" << onAirFreq
                << "found to be in WSPR guard band 10139.9 - 10140.32 kHz "
                   "where this programm will not transmit, so  canceling all "
                   "transmissions.";
            m_isTimeToSend = false;
            if (m_auto)
                auto_tx_mode(false);
            if (m_hb_loop->isActive()) {
                qWarning() << "[HAIL-DIAG] loop cancelled: WSPR guard band";
                m_hb_loop->onLoopCancel();
            }
            if (m_cq_loop->isActive()) {
                m_cq_loop->onLoopCancel();
            }
            if (onAirFreq != m_onAirFreq0) {
                m_onAirFreq0 = onAirFreq;
                QTimer::singleShot(0, [this] {
                    JS8MessageBox::warning_message(
                        this, tr("WSPR Guard Band"),
                        tr("Please choose another Tx frequency."
                           " The app will not knowingly transmit another"
                           " mode in the WSPR sub-band on 30m."));
                });
            }
        }
    }
}

void UI_Constructor::prepareSending(qint64 nowMS) {
    // TX Duration in seconds.
    const double tx_duration = JS8::Submode::txDuration(m_nSubMode);
    const unsigned periodMS = JS8::Submode::periodMS(m_nSubMode);
    const double period = periodMS / 1000.0;

    const double seconds_into_the_period = (nowMS % periodMS) / 1000.0;
    const double tx_delay = m_TxDelay;

    const bool time_is_in_tx_delay =
        (period - tx_delay) <= seconds_into_the_period;

    // Are we during the time we might be sending?
    const bool m_timeToSend = ((0 <= seconds_into_the_period) and
                               (seconds_into_the_period < tx_duration)) or
                              time_is_in_tx_delay or m_tune;

    auto const msgLength = QStringView(m_nextFreeTextMsg).trimmed().length();

    // TODO: stop
    if (msgLength == 0 && !m_tune) {
        if (m_nSubMode == Varicode::JS8CallFT2 && m_btxok)
            qWarning() << "[FT2-TX] prepareSending: msgLength=0, stopping TX"
                        << "btxok=" << m_btxok << "iptt=" << m_iptt
                        << "auto=" << m_auto;
        m_stopTxButtonIsLongterm = false;
        this->on_stopTxButton_clicked();
        m_stopTxButtonIsLongterm = true;
    }

    double const fraction_of_tx_slot = seconds_into_the_period / period;

    // Period-aligned TX only: lateThreshold = 0 for every mode, so the
    // start branch below fires only when time_is_in_tx_delay is true
    // (i.e. within the tx_delay window just before the next period
    // boundary). Legacy behavior permitted mid-period TX starts with
    // large lateThreshold values (~0.83 for Normal mode) -- that was
    // always an alignment hazard for synchronous decode, and since
    // Build 88b's WAIT-NEXT-PERIOD silent-frames injection in
    // Modulator::start it became a hard failure: prepareSending would
    // start the TX mid-period, Modulator would queue silence until the
    // next boundary, prepareSending would kill the TX when
    // seconds_into_the_period exceeded tx_duration of the CURRENT
    // period, and the real audio never got a chance to play. SV1UY's
    // 2026-04-21 17:42Z log captured this end-to-end (11.5 s of
    // silent on-air "TX"). Forcing period-aligned start eliminates
    // both the old alignment hazard and the post-88b silence-TX bug.
    // The unused JS8::Submode::computeRatio / ratio value remains
    // available if future logic wants to distinguish modes, but no
    // mode currently needs a non-zero lateThreshold.
    float lateThreshold = 0.0f;
    (void)JS8::Submode::computeRatio(m_nSubMode, m_TRperiod);

    // FT2 per-period diagnostic: log once per period at the TX delay window
    if (m_nSubMode == Varicode::JS8CallFT2 && time_is_in_tx_delay &&
        m_iptt == 0 && !m_tune) {
        qWarning() << "[FT2-TX] prepareSending state:"
                    << "sec=" << seconds_into_the_period
                    << "timeToSend=" << m_timeToSend
                    << "btxok=" << m_btxok
                    << "iptt=" << m_iptt
                    << "auto=" << m_auto
                    << "transmitting=" << m_transmitting
                    << "msgLen=" << msgLength
                    << "msg=" << m_nextFreeTextMsg.left(20);
    }

    // Subspace + ARQ TX-gate FULL relax (authorized 2026-06-05,
    // re-refined 2026-06-05 after operator observed ACK-triggered chunk
    // TX still slipping to the next period boundary): when we're in
    // Subspace mode AND ARQ messaging is in flight, ignore BOTH
    // m_timeToSend (the tx_duration sliver of each cycle) AND the
    // fraction_of_tx_slot/lateThreshold check. ACK-triggered chunks
    // come in at arbitrary moments in the cycle — the previous 0.1×
    // cycle clamp meant any ACK landing past 375 ms into the cycle
    // had to wait for the next boundary (3+ s wasted). For ARQ the
    // back-to-back risk that motivated the clamp is moot — each chunk
    // is separated by a full ACK round-trip anyway. Modulator's own
    // ARQ-RELAX path already inserts only the minimum startDelayMS
    // pad on this side of the boundary, so an arbitrary-time start
    // is air-safe.
    // Gate the relax on an actually-in-flight session, not on the
    // toggle alone (2026-06-08 tightening, per operator request).
    // Earlier code keyed on m_chunkedArq->arqInProgress() — which is
    // really just the menu/button toggle state. With ARQ enabled and
    // no session in flight, even a plain HAIL would TX via the
    // arbitrary-time arqFullRelax path; while that's air-safe per the
    // earlier audit, it widens the window for unwanted interactions
    // with the receiver's dedup layers and (theoretically) leaves no
    // period-aligned-spacing safety net for non-ARQ traffic. Now: the
    // relax only kicks in when there's an actual chunked super-message
    // in flight (m_sends or m_recv.assemblies non-empty). All other
    // FT2 TX (HAILs, autoreplies, plain directed messages) goes the
    // period-aligned path even when ARQ is enabled but idle.
    // [ARQ TX TIMING TEST — conditional compilation 2026-06-09]
    // Defined  = async TX path (PTT fires immediately on ACK arrival,
    //            no period-boundary wait — current default behaviour)
    // Undefined = period-aligned TX path (PTT only fires at period
    //            boundary, like legacy synchronous modes)
    // Toggle here AND in JS8_Mode/Modulator.cpp must match. Recompile
    // required (this is intentional — runtime certainty about which
    // path is in the binary).
    //
// [BUILD 297] Relaxed TX re-enabled. Build 296 confirmed the failure
// mode IS related to relaxed TX, but later analysis showed the
// difference was the pre-silence pad amount (100ms relaxed vs
// 200-300ms non-relaxed), not the off-boundary timing itself. Build
// 297 unifies the pad at 250ms in Modulator.cpp for BOTH modes, so
// relaxed TX now puts the same amount of silence before the Costas
// as non-relaxed. Expected: relaxed mode now decodes reliably too.
// #define ARQ_TX_ASYNC 1  // build 308 (periodTx): forced OFF — period-aligned PTT for A/B vs asyncTx
    //
    // ^^ Comment-out the #define line above for the period-aligned
    //    test build. Uncomment for the async build.
#ifdef ARQ_TX_ASYNC
    bool const arqFullRelax = (m_nSubMode == Varicode::JS8CallFT2) &&
                              m_chunkedArq && m_chunkedArq->hasActiveSession();
#else
    // Period-aligned ARQ TX: arqFullRelax forced false, so the PTT
    // condition falls through to the standard period-aligned path
    // (m_timeToSend && fraction<lateThreshold || time_is_in_tx_delay).
    bool const arqFullRelax = false;
#endif

    // Per-PTT interval gate (2026-06-06, build arq-interval). The
    // earlier 100 ms cooldown produced occasional cut-short cycles
    // (PTT at sec 3.618 → next PTT 2.83 s later, only 312 ms of on-
    // air silence between waveforms). Earlier 750 ms post-stopTx
    // cooldown drove τ to the tx_delay window but per-cycle length
    // still depended on τ via the period-anchored stopTx.
    //
    // Operator spec: "nominal 3.75 s period, no lengthening, no cut-
    // shorts." The clean way to enforce that is to gate on the
    // interval since the LAST PTT, not since the last stopTx. Every
    // cycle becomes ≥ 3.75 s (one Subspace period), regardless of
    // where in the period the previous frame landed. Air silence
    // becomes a consistent ~1.13 s every frame, matching the natural
    // period-aligned non-ARQ cadence the operator confirmed decodes
    // reliably.
    constexpr qint64 MIN_ARQ_PTT_INTERVAL_MS = 3750;  // build 302 (rxStdCycle): back to standard 3.75s to rule out dead-time effect
    bool const arqIntervalOK = !m_lastTxStartTime.isValid() ||
        m_lastTxStartTime.msecsTo(DriftingDateTime::currentDateTimeUtc())
            >= MIN_ARQ_PTT_INTERVAL_MS;

    // Floor-loophole fix (2026-06-09): when arqFullRelax is in effect,
    // the second OR-clause (period-aligned via m_timeToSend +
    // time_is_in_tx_delay) MUST also respect arqIntervalOK. Without
    // this, every other PTT in an ARQ session lands ~60-700ms below
    // the 3750ms floor because the period boundary races ahead of the
    // interval gate. Observed sawtooth pattern 2026-06-09: ~3690ms /
    // ~3795ms alternation, worst case 2997ms (~750ms under floor).
    // Non-ARQ TX (arqFullRelax=false) keeps original period-aligned
    // behaviour unchanged — the AND-with-arqIntervalOK only short-
    // circuits the floor enforcement when ARQ is active anyway.
    if (m_iptt == 0 &&
        (m_tune || (arqFullRelax && msgLength > 0 && arqIntervalOK) ||
         (m_timeToSend &&
          (fraction_of_tx_slot < lateThreshold || time_is_in_tx_delay) &&
          0 < msgLength &&
          (!arqFullRelax || arqIntervalOK)))) {
        // This signals the transmitter to switch to sending.
        // When that has happened, we get a callback from
        // handle_transceiver_update, which will start the audio.
        if (m_nSubMode == Varicode::JS8CallFT2) {
            qint64 const msSinceLastTx =
                m_lastTxStartTime.isValid()
                    ? m_lastTxStartTime.msecsTo(
                        DriftingDateTime::currentDateTimeUtc())
                    : qint64{-1};
            qWarning() << "[FT2-TX] prepareSending: m_iptt 0→1, emitPTT(true)"
                        << "secInPeriod=" << seconds_into_the_period
                        << "txDelay=" << time_is_in_tx_delay
                        << "lateThreshold=" << lateThreshold
                        << "arqFullRelax=" << arqFullRelax
                        << "arqIntervalOK=" << arqIntervalOK
                        << "msSinceLastTx=" << msSinceLastTx;
        }
        m_iptt = 1;
        m_generateAudioWhenPttConfirmedByTX = true;
        // [GATE MISMATCH FIX 2026-06-16 build 289]
        // Synchronize the Modulator's m_arqRelax flag to THIS PTT's
        // actual gate decision (session-active state), not to the ARQ
        // button toggle. Pre-build-289 the flag was wired to the
        // button toggle via setArqRelax(checked), which left non-ARQ
        // TX (hails, autoreplies, plain directed messages) routing
        // through the ARQ-RELAX branch in Modulator::start whenever
        // the ARQ button was on — bypassing period alignment for
        // ostensibly period-aligned TX. The caller (this site) knows
        // the actual decision via `arqFullRelax`; pushing it to the
        // Modulator here means the Modulator picks the correct
        // alignment path per-TX.
        if (m_modulator) {
            m_modulator->setArqRelax(arqFullRelax);
        }
        setRig();
        setXIT(freq());
        emitPTT(true);
        // [ARQ TX TIMING ASYNC-FINISH STALE-SENTINEL FIX 2026-06-10 (build 231)]
        // Reset the Modulator's audio-cadence sentinels SYNCHRONOUSLY at
        // PTT-up so the next guiUpdate poll sees -1 (not started) for the
        // new cycle. Otherwise the previous frame's stale audioStartedMs
        // value (>0) causes stopTx to fire instantly on every-other PTT,
        // producing "transmit, skip period, transmit, skip period" —
        // protocol thinks each skipped frame was sent and advances the
        // queue, so receiver gets every OTHER frame. (Modulator::start()
        // also resets these inside the queued slot — this synchronous
        // call ensures the reset happens BEFORE the poll, not after.)
        if (m_modulator) {
            m_modulator->resetCadenceCapture();
        }
    }

    // Stop transmitting when the time window expires.
    if (!m_timeToSend and !m_tune)
        m_btxok = false;

    // Calculate Tx tones when needed
    if ((m_iptt == 1 && m_iptt0 == 0) || m_restart) {
        //----------------------------------------------------------------------

        copyMessage(m_nextFreeTextMsg, message);

        if (m_lastMessageSent != m_currentMessage ||
            m_lastMessageType != m_currentMessageType) {
            m_lastMessageSent = m_currentMessage;
            m_lastMessageType = m_currentMessageType;
        }

        m_currentMessageType = 0;

        if (m_tune) {
            itone[0] = 0;
        }
#ifdef JS8_ENABLE_FT2
        else if (m_nSubMode == Varicode::JS8CallFT2) {
            // FT2 native 72-bit framing: encode JS8 varicode frame
            // into 77-bit FT2 payload (72 data + 5 metadata bits).
            QString frame = QString::fromLatin1(message, 12);
            quint8 rem = 0;
            quint64 value = Varicode::unpack72bits(frame, &rem);

            // Build 77-bit array: bits 0-71 = JS8 frame, 72-74 = flags
            std::int8_t msgbits77[77] = {};
            for (int i = 0; i < 64; ++i)
                msgbits77[i] = (value >> (63 - i)) & 1;
            for (int i = 0; i < 8; ++i)
                msgbits77[64 + i] = (rem >> (7 - i)) & 1;
            if (m_i3bit & Varicode::JS8CallFirst) msgbits77[72] = 1;
            if (m_i3bit & Varicode::JS8CallLast)  msgbits77[73] = 1;
            if (m_i3bit & Varicode::JS8CallData)   msgbits77[74] = 1;

            ft2_encode_from_bits_c(msgbits77,
                         const_cast<int *>(reinterpret_cast<volatile int *>(itone)));

            // Generate the GFSK waveform at 48kHz
            ft2_gen_wave_c(
                const_cast<int *>(reinterpret_cast<volatile int *>(itone)),
                FT2_NUM_SYMBOLS, FT2_TX_NSPS, 48000.0f,
                static_cast<float>(freq() + m_XIT),
                ft2_txwave, FT2_NWAVE);
            ft2_txwave_len = FT2_NWAVE;

            // Fill msgsent so the status label shows the TX message
            std::fill_n(std::begin(msgsent), 22, ' ');
            std::copy_n(std::begin(message), 12, std::begin(msgsent));
            msgibits = m_i3bit;
            msgsent[22] = 0;
            m_currentMessage = QString::fromLatin1(msgsent).trimmed();
            m_currentMessageBits = msgibits;

            qWarning() << "[FT2-TX] native frame:" << frame
                       << "bits=" << m_i3bit
                       << "itone[0..4]=" << itone[0] << itone[1]
                       << itone[2] << itone[3] << itone[4];

            emitTones();
        }
#endif
        else {
            JS8::encode(m_i3bit,
                        JS8::Costas::array(JS8::Submode::costas(m_nSubMode)),
                        message,
                        const_cast<int *>(reinterpret_cast<volatile int *>(
                            itone))); // XXX ick...

            std::fill_n(std::begin(msgsent), 22, ' ');
            std::copy_n(std::begin(message), 12, std::begin(msgsent));

            if (mainwindow_js8().isDebugEnabled()) {
                qCDebug(mainwindow_js8) << "-> msg:" << message;
                qCDebug(mainwindow_js8) << "-> bit:" << m_i3bit;
                for (int i = 0; i < 7; ++i)
                    qCDebug(mainwindow_js8)
                        << "-> tone" << i << "=" << itone[i];
                for (int i = JS8_NUM_SYMBOLS - 7; i < JS8_NUM_SYMBOLS; ++i)
                    qCDebug(mainwindow_js8)
                        << "-> tone" << i << "=" << itone[i];
            }

            msgibits = m_i3bit;
            msgsent[22] = 0;
            m_currentMessage = QString::fromLatin1(msgsent).trimmed();
            m_currentMessageBits = msgibits;

            emitTones();
        }

        if (m_tune) {
            m_currentMessage = "TUNE";
            m_currentMessageType = -1;
        }
        if (m_restart) {
            write_transmit_entry("ALL.TXT");
        }

        auto msg_parts = m_currentMessage.split(' ', Qt::SkipEmptyParts);
        if (msg_parts.size() > 2) {
            // clean up short code forms
            msg_parts[0].remove(QChar{'<'});
            msg_parts[1].remove(QChar{'>'});
        }

        if ((m_currentMessageType < 6 || 7 == m_currentMessageType) &&
            msg_parts.length() >= 3 &&
            (msg_parts[1] == m_config.my_callsign() ||
             msg_parts[1] == m_baseCall)) {
            int i1;
            bool ok;
            i1 = msg_parts[2].toInt(&ok);
            if (ok and i1 >= -50 and i1 < 50) {
                m_rptSent = msg_parts[2];
            } else {
                if (msg_parts[2].mid(0, 1) == "R") {
                    i1 = msg_parts[2].mid(1).toInt(&ok);
                    if (ok and i1 >= -50 and i1 < 50) {
                        m_rptSent = msg_parts[2].mid(1);
                    }
                }
            }
        }
        m_restart = false;
        //----------------------------------------------------------------------
    } else if (m_nSubMode == Varicode::JS8CallFT2 && m_iptt == 1 && !m_transmitting) {
        // DIAG BUILD 51: log only when tone-gen skipped AND not already transmitting (revert in Build 52)
        qWarning() << "[FT2-TX] TONE-GEN SKIPPED: m_iptt=" << m_iptt
                   << "m_iptt0=" << m_iptt0 << "m_restart=" << m_restart
                   << "m_transmitting=" << m_transmitting;
    }

    if (m_iptt == 1 && m_iptt0 == 0) {
        auto const &current_message = QString::fromLatin1(msgsent);
        if (m_config.watchdog() && current_message != m_msgSent0) {
            // new messages don't reset the idle timer :|
            // tx_watchdog (false);  // in case we are auto sequencing
            m_msgSent0 = current_message;
        }

        if (!m_tune) {
            write_transmit_entry("ALL.TXT");
        }

        // TODO: jsherer - perhaps an on_transmitting signal?
        m_lastTxStartTime = DriftingDateTime::currentDateTimeUtc();
        // Record wall-clock start of first frame for countdown display
        if (m_txFrameQueue.count() == m_txFrameCount - 1) {
            // First dequeue already happened, so queue is count-1
            m_txQueueStartTime = m_lastTxStartTime;
        }

        m_transmitting = true;
        transmitDisplay(true);
        statusUpdate();

        // Start modulator immediately rather than waiting for async PTT
        // callback. The rig may not reliably report PTT off→on transitions
        // between cycles, especially for short-period modes.
        m_generateAudioWhenPttConfirmedByTX = false;
        transmit();
    } else if (m_nSubMode == Varicode::JS8CallFT2 && m_iptt == 1 && !m_transmitting) {
        // DIAG BUILD 51: log only when transmit skipped AND not already transmitting (revert in Build 52)
        qWarning() << "[FT2-TX] TRANSMIT SKIPPED: m_iptt=" << m_iptt
                   << "m_iptt0=" << m_iptt0
                   << "m_transmitting=" << m_transmitting;
    }

    // TODO: stop
    if (!m_btxok && m_btxok0 && m_iptt == 1) {
        if (m_nSubMode == Varicode::JS8CallFT2) {
            // FT2 is async — don't kill the waveform based on period math.
            // The Modulator will emit ft2WaveformDone() when the waveform
            // finishes playing, which triggers stopTx() cleanly.
            // Safety: force stop if waveform hasn't completed in 5 seconds.
            if (m_modulator->isFT2WaveformDone()) {
                qWarning() << "[FT2-TX] btxok edge: waveform already done,"
                            << "triggering stopTx()";
                stopTx();
            } else {
                qWarning() << "[FT2-TX] btxok edge: SKIPPED for FT2"
                            << "(waiting for waveform completion)";
            }
        } else {
            stopTx();
        }
    }

    // FT2: poll for waveform completion every guiUpdate tick.
    // The btxok edge check above is a one-shot — if the waveform wasn't
    // done at that instant, this continuous check catches it.
    //
    // [ARQ TX TIMING ASYNC-FINISH 2026-06-09]
    // Under arqFullRelax (defined ARQ_TX_ASYNC + active ARQ session +
    // Subspace mode), stopTx fires the instant `isFT2WaveformDone()`
    // returns true — WITHOUT waiting for `m_btxok` to flip false.
    //
    // m_btxok is derived from period-aligned `m_timeToSend`, so under
    // truly async TX (PTT-fire bypasses period at mainwindow.cpp:2770),
    // gating stopTx on `!m_btxok` reintroduces a period-aligned wait
    // at the END of every frame. That wait extends PTT-to-stopTx up
    // to a full period (~3.75 s) after Modulator finishes writing the
    // last waveform sample, which then trips the 5 s SAFETY force-stop
    // (see line ~3032 below). Net: a half-async design where TX-start
    // is async but TX-end is period-aligned. Frames extend past their
    // expected window, the next frame's PTT gets delayed, and the
    // receiver sees ill-timed or mid-period TX activity.
    //
    // This makes the async design symmetric: PTT-fire bypasses period
    // AND stopTx bypasses period when arqFullRelax is in effect. The
    // non-arqFullRelax branch (legacy synchronous modes, plain FT2 TX
    // outside ARQ) keeps its original `!m_btxok` gate exactly as
    // before.
    //
    // Conditional-compilation parity with the PTT-fire side: when
    // ARQ_TX_ASYNC is UNDEFINED (period-aligned test build), arqFullRelax
    // is forced false everywhere and this stopTx-side bypass also
    // becomes false. The two sides stay consistent — never one async
    // and the other not.
#ifdef ARQ_TX_ASYNC
    bool const arqFullRelaxStopTx = (m_nSubMode == Varicode::JS8CallFT2) &&
                                    m_chunkedArq && m_chunkedArq->hasActiveSession();
#else
    bool const arqFullRelaxStopTx = false;
#endif
    // [ARQ TX TIMING ASYNC-FINISH RACE FIX 2026-06-10 (build 230)]
    // The original bypass `arqFullRelaxStopTx || !m_btxok` exposed a
    // race: at PTT-up, the Modulator's state is briefly still KeepAlive
    // (the transition to Synchronizing/Active happens inside the
    // queued Modulator::start() call). isFT2WaveformDone() returns
    // true under that state, so the poll fires stopTx 11 ms after
    // PTT-up — instant TX abort. The OLD `!m_btxok` gate accidentally
    // prevented this because m_btxok was just set true.
    //
    // Fix: under arqFullRelaxStopTx, additionally require that the
    // Modulator has actually started emitting waveform samples
    // (audioStartedMs() > 0). The Modulator sets that sentinel inside
    // readData() the moment it writes the first waveform sample. If
    // it's still -1, the waveform hasn't begun yet and isFT2WaveformDone
    // is reading the stale KeepAlive state.
    //
    // Non-arqFullRelax path is unchanged: legacy m_btxok gate applies.
    bool const audioActuallyStarted = m_modulator->audioStartedMs() > 0;
    bool const okToStopTx = arqFullRelaxStopTx
        ? (audioActuallyStarted && m_modulator->isFT2WaveformDone())
        : (!m_btxok && m_modulator->isFT2WaveformDone());
    if (m_nSubMode == Varicode::JS8CallFT2 && m_transmitting && m_iptt == 1
        && !m_tune
        && okToStopTx) {
        qWarning() << "[FT2-TX] waveform poll: done, triggering stopTx()"
                   << "arqFullRelaxStopTx=" << arqFullRelaxStopTx
                   << "audioActuallyStarted=" << audioActuallyStarted;
        stopTx();
    }

    // FT2 safety: force stop if TX has been running too long (stuck Modulator)
    if (m_nSubMode == Varicode::JS8CallFT2 && m_transmitting && m_iptt == 1
        && !m_tune) {
        static qint64 ft2TxStartMs = 0;
        if (m_btxok && !m_btxok0) {
            ft2TxStartMs = DriftingDateTime::currentMSecsSinceEpoch();
        }
        if (ft2TxStartMs > 0) {
            qint64 elapsed = DriftingDateTime::currentMSecsSinceEpoch()
                             - ft2TxStartMs;
            if (elapsed > 5000) {
                qWarning() << "[FT2-TX] SAFETY: TX exceeded 5s, forcing stopTx()"
                            << "elapsed=" << elapsed << "ms";
                ft2TxStartMs = 0;
                stopTx();
            }
        }
    }
}

void UI_Constructor::updateClockUI(const QDateTime &now) {
    qint64 drift = DriftingDateTime::drift();
    QStringList parts;
    parts
        << (now.time().toString() +
            (!drift
                 ? " "
                 : QString(" (%1%2ms)").arg(drift > 0 ? "+" : "").arg(drift)));
    parts << now.date().toString("yyyy MMM dd");
    ui->labUTC->setText(parts.join("\n"));
}

//------------------------------------------------------------- //guiUpdate()
void UI_Constructor::guiUpdate() {
    unsigned period = JS8::Submode::period(m_nSubMode);

    m_TRperiod = period; // Investigate: Does anyone need this?

    // Propagate any tx delay change to m_hb_loop and m_cq_loop.
    double tx_delay_now = m_config.txDelay();
    if (tx_delay_now != m_TxDelay) {
        m_TxDelay = tx_delay_now;
        qint64 tx_delay_ms = std::lround(tx_delay_now * 1000);
        m_hb_loop->onTxDelayChange(tx_delay_ms);
        m_cq_loop->onTxDelayChange(tx_delay_ms);
    }

    const QDateTime now = DriftingDateTime::currentDateTimeUtc();
    const qint64 seconds_since_epoch = now.toSecsSinceEpoch();

    if (m_transmitting or m_auto or m_tune) {
        refuseToSendIn30mWSPRBand();
        prepareSending(now.toMSecsSinceEpoch());
    } else if (m_nSubMode == Varicode::JS8CallFT2) {
        // DIAG BUILD 51: suppressed — fires every second (revert in Build 52)
#if 0
        static qint64 lastLogSec = 0;
        if (seconds_since_epoch != lastLogSec) {
            lastLogSec = seconds_since_epoch;
            qWarning() << "[FT2-TX] guiUpdate: NOT calling prepareSending"
                        << "transmitting=" << m_transmitting
                        << "auto=" << m_auto
                        << "tune=" << m_tune;
        }
#endif
    }

    // Once per second:
    if (seconds_since_epoch != m_sec0) {
        m_sec0 = seconds_since_epoch;

        updateClockUI(now);

        if (m_monitoring or m_transmitting) {
            // We are lucky that TX delay starts well into the second
            // and lasts less than a second. So as long as we
            // do this near the begining of a second, we will never hit
            // the confusing "progress" of tx delay.
            progressBar.setMaximum(period);
            int progress = seconds_since_epoch % period;
            progressBar.setValue(progress);
        } else {
            progressBar.setValue(0);
        }

        if (m_transmitting) {
            tx_status_label.setStyleSheet(
                "QLabel{background-color: #ff2222; color:#000}");
            if (m_tune) {
                tx_status_label.setText("Tx: TUNE");
            } else {
                auto message =
                    DecodedText(msgsent, msgibits, m_nSubMode).message();
                tx_status_label.setText(
                    QString("Tx: %1").arg(message).left(40).trimmed());
            }
            transmitDisplay(true);
        } else if (m_monitoring) {
            if (m_tx_watchdog) {
                tx_status_label.setStyleSheet(
                    "QLabel{background-color: #000; color:#fff}");
                tx_status_label.setText("Idle timeout");
            } else {
                tx_status_label.setStyleSheet(
                    "QLabel{background-color: #22ff22}");
                tx_status_label.setText(m_decoderBusy ? "Decoding"
                                                      : "Receiving");
            }
            transmitDisplay(false);
        } else if (!m_tx_watchdog) {
            tx_status_label.setStyleSheet("");
            tx_status_label.setText("");
        }

        auto callLabel = m_config.my_callsign();
        if (m_config.use_dynamic_grid() && !m_config.my_grid().isEmpty()) {
            callLabel =
                QString("%1 - %2").arg(callLabel).arg(m_config.my_grid());
        }
        ui->labCallsign->setText(callLabel);

        if (!m_monitoring) {
            ui->signal_meter_widget->setValue(0, 0);
        }

        // once per period
        if (seconds_since_epoch % period == 0) {
            tryBandHop();
        }

        // Need to do processing at the end of the period
        // or when there is something in m_rxActivityQueue.
        bool forceDirty = (seconds_since_epoch % period == 0) ||
                          ((seconds_since_epoch - 1) % period == 0) ||
                          !m_rxActivityQueue.isEmpty();

        // update the dial frequency once per second..
        displayDialFrequency();
        updateHBButtonDisplay();
        updateCQButtonDisplay();

        // once per second...but not when we're transmitting, unless it's in the
        // first second...
        if (!m_transmitting || (seconds_since_epoch % period == 0)) {
            // process all received activity...
            processActivity(forceDirty);

            // process outgoing tx queue...
            processTxQueue();

            // once processed, lets update the display...
            displayActivity(forceDirty);
            updateButtonDisplay();
            updateTextDisplay();
        }

        // Refresh the callsign list (right pane) every second regardless
        // of forceDirty / transmit state. displayActivity's forceDirty
        // gate was starving the right pane of redraws — Age/SNR stayed
        // frozen between decodes and during our own TX even though
        // m_callActivity was being updated per-frame. The left pane
        // (band activity) stays on the gated displayActivity path since
        // it's heavier and doesn't need a per-second tick.
        //
        // Build 121: split the refresh into a cheap Age-only tick and a
        // full rebuild. logCallActivity bumps m_callActivityVersion on
        // insert/update; if nothing has changed since the last full
        // render we just walk existing rows and rewrite the Age cell
        // (O(V) setText calls, no sort, no allocations). Any model
        // change falls back to a full displayCallActivity rebuild,
        // which syncs m_callActivityRenderedVersion at the end.
        // Windows users were seeing 4-40% CPU from the unconditional
        // once-per-second rebuild — especially with large callsign
        // lists, where Qt's QTableWidget mutations are meaningfully
        // heavier than on Linux (GDI font metrics, UIA accessibility
        // events, per-insert widget-style polish).
        if (m_callActivityVersion != m_callActivityRenderedVersion) {
            displayCallActivity();
        } else {
            refreshCallActivityAgeOnly();
        }
    } // end of stuff we do once per second.

    displayTransmit();

    m_iptt0 = m_iptt;
    m_btxok0 = m_btxok;

    // Set the time to hit the start of the next UI_POLL_INTERVAL_MS slot.
    // This automatically hits close to the start of each second
    // Update mode button enable state (disable during TX)
    {
        // arqBusy treats an in-flight chunked super-message as one
        // continuous TX for UI purposes. Between chunks m_transmitting
        // drops momentarily — without the arqBusy gate, buttons would
        // flicker enabled→disabled with each chunk. Holding them
        // disabled for the entire session prevents the operator from
        // changing mode / toggling ARQ / typing into the outgoing box
        // mid-protocol. Cleared by sendComplete / sendFailed /
        // messageDelivered / halt.
        // [RX-SIDE NO-LOCK 2026-06-10] hasActiveTxSession() —
        // fires only when WE are sending. Receiver-side buttons
        // stay usable mid-RX.
        bool const arqBusy = (m_chunkedArq && m_chunkedArq->hasActiveTxSession());
        // Two-tier gate (2026-06-08, follow-up):
        //   canChangeSpeed — speed mode buttons (S/N/F/T/⚡). Re-
        //     enabled BETWEEN chunks during an ARQ session so the
        //     operator can adapt to changing band conditions mid-
        //     super-message (genuinely useful — drop to Slow if SNR
        //     tanks, bump to Turbo if it's clean).
        //   canChangeMode — ARQ button + menu action. Stays locked
        //     for the full session: flipping ARQ mid-protocol would
        //     yank the state machine out from under itself, which
        //     IS unsafe.
        bool const canChangeSpeed =
            !m_transmitting && !m_tune &&
            m_txFrameCount == 0 && m_txFrameQueue.isEmpty();
        bool const canChangeMode = canChangeSpeed && !arqBusy;
        if (m_modeBtnNormal && m_modeBtnNormal->isEnabled() != canChangeSpeed) m_modeBtnNormal->setEnabled(canChangeSpeed);
        if (m_modeBtnFast && m_modeBtnFast->isEnabled() != canChangeSpeed) m_modeBtnFast->setEnabled(canChangeSpeed);
        if (m_modeBtnTurbo && m_modeBtnTurbo->isEnabled() != canChangeSpeed) m_modeBtnTurbo->setEnabled(canChangeSpeed);
        if (m_modeBtnSlow && m_modeBtnSlow->isEnabled() != canChangeSpeed) m_modeBtnSlow->setEnabled(canChangeSpeed);
        if (m_modeBtnFT2 && m_modeBtnFT2->isEnabled() != canChangeSpeed) m_modeBtnFT2->setEnabled(canChangeSpeed);

        // [BUILD 298] m_arqButton enable gate REMOVED — button deleted.
        // The canChangeMode rule still applies to the underlying
        // QAction (which governs the ChunkedArq::Manager and
        // Modulator state), so Subspace mode-switch interlock
        // continues to work for the "Send using ARQ" menu action.
        if (ui->actionModeReplicatorProtocol &&
            ui->actionModeReplicatorProtocol->isEnabled() != canChangeMode) {
            ui->actionModeReplicatorProtocol->setEnabled(canChangeMode);
        }
        // Control menu items locked for the full ARQ session too —
        // operator can't fire a HAIL / CQ broadcast through the menu
        // mid-protocol. Natural state for actionHeartbeat is
        // m_hbModeAvailable (set by setHeartbeatEnabled). Natural for
        // actionCQ is always enabled. AND with !arqBusy to add the
        // session-lock without losing the underlying gating.
        if (ui->actionHeartbeat) {
            bool const wantHb = m_hbModeAvailable && !arqBusy;
            if (ui->actionHeartbeat->isEnabled() != wantHb) {
                ui->actionHeartbeat->setEnabled(wantHb);
            }
        }
        if (ui->actionCQ) {
            bool const wantCq = !arqBusy;
            if (ui->actionCQ->isEnabled() != wantCq) {
                ui->actionCQ->setEnabled(wantCq);
            }
        }

        // Outgoing-text widget mirrors the same lock: read-only +
        // "transmitting" visual property for the duration of the ARQ
        // session, so the operator can't edit between chunks (the
        // protocol layer is filling it with chunk bodies). The
        // existing per-frame readOnly toggling at prepareNextMessageFrame
        // already handles single-frame TX; this extends to span
        // chunk-to-chunk gaps inside an ARQ super-message.
        if (ui->extFreeTextMsgEdit) {
            bool const wantReadOnly = arqBusy;
            if (arqBusy && !ui->extFreeTextMsgEdit->isReadOnly()) {
                ui->extFreeTextMsgEdit->setReadOnly(true);
                update_dynamic_property(ui->extFreeTextMsgEdit,
                                        "transmitting", true);
            } else if (!arqBusy && !m_transmitting &&
                       ui->extFreeTextMsgEdit->isReadOnly() &&
                       m_txFrameQueue.isEmpty()) {
                // Only clear when BOTH ARQ session is over AND no
                // normal TX is in flight. Don't fight the per-frame
                // readOnly toggling that startTxNonArq / prepareNextMessageFrame
                // own for plain TXes.
                ui->extFreeTextMsgEdit->setReadOnly(false);
                update_dynamic_property(ui->extFreeTextMsgEdit,
                                        "transmitting", false);
            }
        }
    }

    // and hence close to the start of each transmit period.
    qint64 now_at_end_ms = DriftingDateTime::currentMSecsSinceEpoch();
    qint64 time_into_poll_slot = now_at_end_ms % UI_POLL_INTERVAL_MS;
    qint64 until_start_of_next_poll_slot =
        UI_POLL_INTERVAL_MS - time_into_poll_slot;

    m_guiTimer.start(until_start_of_next_poll_slot);
} // End of guiUpdate

void UI_Constructor::startTx() {
#if IDLE_BLOCKS_TX
    if (m_tx_watchdog) {
        return;
    }
#endif

    auto text = ui->extFreeTextMsgEdit->toPlainText();

    // [QUEUE PROVENANCE 2026-06-10 build 247]
    // If the current edit-box content matches the most recent text
    // queued in by processTxQueue (autoreply, relay forward, TCP API
    // send), the operator is just clicking Send on a system-built
    // reply — NOT typing a fresh ARQ candidate. Route directly to
    // startTxNonArq so the ARQ gate doesn't get a chance to mis-classify
    // a relay-marker reply path ("K9AVT>WM8Q STATUS …") as an explicit
    // ARQ-relay request and wrap it. Mirrors the Build 205 design:
    // ARQ wrapping is reserved for content the operator typed directly.
    if (!m_lastQueueInjectedText.isEmpty() &&
        text == m_lastQueueInjectedText) {
        qWarning() << "[ARQ] startTx: queue-injected text detected"
                   << "→ routing to startTxNonArq (no ARQ wrap)"
                   << "textHead=" << text.left(40);
        m_lastQueueInjectedText.clear();
        startTxNonArq();
        return;
    }

    // ARQ intercept: when ARQ is enabled AND the
    // operator has selected a target callsign, hand the FULL outgoing
    // text to ChunkedArq::Manager::sendChunked instead of the normal
    // TX queue. The Manager splits, tags (#NN.CC/TT.HHHH), and feeds
    // chunked sub-frames back through wantToTransmit →
    // onChunkedWantToTransmit one at a time. Each sub-frame is what
    // the operator sees in the edit widget while it's TXing. Falls
    // through to normal TX if the peer / toggle gates don't line up.
    //
    // Mode-agnostic: ARQ runs in ANY submode (Normal, Fast, Turbo,
    // Slow, Subspace). In Subspace (FT2) the prepareSending +
    // Modulator FT2 bypass paths give async cycle-independent TX; in
    // synchronous modes (Normal/Fast/Turbo/Slow) ARQ falls back to
    // period-aligned cadence — slower per-chunk wall clock, but the
    // protocol is identical and ACK/NACK semantics work the same.
    // Resolve the ARQ peer. Priority order:
    //   1. callsignSelected() — operator's explicit Call Activity pick
    //   2. Leading callsign of the typed widget text — handles the
    //      common case where the operator types "<peer> body..." or
    //      the JS8 self-prefixed form "WM8Q: <peer> body..." without
    //      first double-clicking Call Activity. Skips the leading
    //      "<mycall>:" if present (JS8's typeahead prepends it).
    //
    // Reject @-prefixed targets (ALLCALL, groups, APRSIS) for ARQ —
    // ARQ needs a single peer that can ACK. Group sends fall through
    // to normal directed TX.
    QString arqPeer = callsignSelected().trimmed();
    if (arqPeer.isEmpty()) {
        QString const myCall = m_config.my_callsign().trimmed().toUpper();
        QString probe = text.toUpper().trimmed();
        if (!myCall.isEmpty() && probe.startsWith(myCall + ":")) {
            probe = probe.mid(myCall.length() + 1).trimmed();
        }
        static QRegularExpression const leadingCallRe(
            QStringLiteral(R"(^\s*(@?[A-Z0-9/]+))"));
        auto const m = leadingCallRe.match(probe);
        if (m.hasMatch()) {
            arqPeer = m.captured(1);
        }
    }
    bool const arqHasMgr    = (m_chunkedArq != nullptr);
    bool const arqEnabled   = arqHasMgr && m_chunkedArq->arqInProgress();
    // Validate the resolved peer is an actual amateur callsign.
    // Without this gate, free-text sends that don't parse as directed
    // (e.g. "TESTING MY RIG" with ARQ enabled) extract "TESTING" as
    // the peer and wrap the whole line in chunked-ARQ wire format
    // (#NN.CC/TT.HHHH), going on-air as free text with visible
    // "magic" markers but no ACKable destination. Radio::is_callsign
    // checks the full callsign-shape regex; @-prefixed targets
    // (ALLCALL, groups) are also rejected because they're group calls
    // with no single peer to ACK.
    bool const arqHasPeer   = !arqPeer.isEmpty() &&
                              !arqPeer.startsWith('@') &&
                              Radio::is_callsign(arqPeer);
    bool const arqHasText   = !text.trimmed().isEmpty();

    // Directed-command detection. Structured single-frame JS8 queries
    // and commands (SNR?, INFO?, STATUS?, MSG, MSG TO:, ACK, NACK,
    // AGN?, GRID?, HEARING?, QUERY ..., etc.) MUST go on-air as their
    // native directed-message form, not ARQ-wrapped. Wrapping them
    // would embed the command inside a chunked-DATA payload with the
    // #NN.CC/TT.HHHH marker — the recipient's directed-cmd parser
    // wouldn't recognize it, so the autoreply (SNR reply, INFO reply,
    // etc.) would never fire and the operator's query would silently
    // succeed in delivery but fail in purpose. Per Andy 2026-06-08:
    // source can be user-typed, pasted, or recalled saved message.
    //
    // Detection uses Varicode::packDirectedMessage on the line minus
    // the optional "<mycall>: " self-prefix (JS8 typeahead form).
    // The packer returns non-empty ONLY when the line parses as a
    // valid directed message with a known command AND a valid
    // callsign in the TO field — false positives are essentially
    // impossible on free-text bodies that just happen to start with
    // a callsign-like token (the cmd_pattern requires a recognized
    // keyword from directed_cmds).
    bool arqBodyIsDirectedCmd = false;
    {
        QString const myCallUp = m_config.my_callsign().trimmed().toUpper();
        QString probe = text.trimmed();
        if (!myCallUp.isEmpty() &&
            probe.toUpper().startsWith(myCallUp + ":")) {
            probe = probe.mid(myCallUp.length() + 1).trimmed();
        }
        QString dirTo, dirCmd, dirNum;
        bool dirToCompound = false;
        int dirN = 0;
        QString dirFrame = Varicode::packDirectedMessage(
            probe, myCallUp, &dirTo, &dirToCompound, &dirCmd, &dirNum,
            &dirN);
        // If the bare text doesn't pack (no leading callsign), try
        // again with the selected peer prepended — mirrors JS8's own
        // AUTO_PREPEND_DIRECTED logic in Varicode.cpp:2114. This
        // catches the case where the operator clicks the STATUS /
        // INFO / TYPING (or any other directed-cmd) macro while a
        // peer is selected: the macro writes "STATUS …" alone into
        // the widget, JS8 will auto-prepend the peer at TX time to
        // form "K9AVT STATUS …" — which IS a directed message that
        // must not get ARQ-wrapped. Without this second try the ARQ
        // gate would see the bare "STATUS …" as un-packable, the
        // selected peer as valid, and (wrongly) wrap the whole macro
        // output as a chunked super-message (operator observed
        // 2026-06-08).
        QString triedProbeWithPeer;
        if (dirFrame.isEmpty() && !arqPeer.isEmpty() &&
            !arqPeer.startsWith('@') &&
            Radio::is_callsign(arqPeer)) {
            triedProbeWithPeer = arqPeer + QStringLiteral(" ") + probe;
            dirFrame = Varicode::packDirectedMessage(
                triedProbeWithPeer, myCallUp, &dirTo, &dirToCompound,
                &dirCmd, &dirNum, &dirN);
        }
        // packDirectedMessage returns non-empty for ANY line that
        // parses as a directed frame — INCLUDING the slot-31 "send
        // freetext" marker (literal " " or "  "). That marker is
        // exactly the case ARQ exists to handle: long free-text to a
        // specific peer. Treating it as "directed cmd, skip ARQ"
        // (Build 208 bug, exposed at full strength by Build 215's
        // peer-prepend retry) made every "K9AVT <anything>" body
        // bypass ARQ wrapping and ship as plain freetext. Now: skip
        // ARQ only when the matched cmd is a structured query/
        // command (SNR?, INFO?, STATUS?, MSG, ACK, NACK, AGN?, GRID?,
        // HEARING?, QUERY..., etc. — anything BUT the freetext
        // marker).
        QString const cmdTrimmed = dirCmd.trimmed();
        bool const isFreetextCmd = cmdTrimmed.isEmpty();
        // [MSG-VIA-ARQ 2026-06-10 build 241]
        // MSG and MSG TO: must NOT short-circuit ARQ — they're
        // exactly what ARQ exists to wrap. ChunkedArq.cpp:286-304
        // already detects MSG / MSG TO: bodies on sendChunked() and
        // sets `wasMsgCmd = true` so the assembled body gets routed
        // to the receiver's inbox on sendComplete. Letting the gate
        // flag MSG as "directed cmd → skip ARQ" defeats that path:
        // the MSG goes out as plain freetext, never gets chunked,
        // never gets reliable delivery, and the receiver never
        // deposits it in their inbox. Treat MSG / MSG TO: as
        // ARQ-bound regardless of the directed-cmd match.
        bool const isMsgCmd =
            (cmdTrimmed.compare(QStringLiteral("MSG"),
                                Qt::CaseInsensitive) == 0) ||
            (cmdTrimmed.compare(QStringLiteral("MSG TO:"),
                                Qt::CaseInsensitive) == 0);
        // [RELAY-VIA-ARQ 2026-06-10 build 243]
        // Relay cmd ">" gets the same exemption as MSG: ARQ wraps the
        // super-message and the receiver-side hook injects into
        // m_messageBuffer so the existing ">" handler at
        // processCommandActivity.cpp:561 fires and forwards verbatim.
        bool const isRelayCmd =
            (cmdTrimmed == QStringLiteral(">"));
        arqBodyIsDirectedCmd =
            !dirFrame.isEmpty() && !isFreetextCmd &&
            !isMsgCmd && !isRelayCmd;

        // JS8-specific macros not in directed_cmds. TYPING is the
        // typing-indicator emitted by the TYPING... button — it has
        // no directed_cmds entry so packDirectedMessage can't match
        // it even with peer prepended, but it must not get ARQ-
        // wrapped (Subspace-only, must go on-air as the raw "TYPING..."
        // pattern the peer recognizes).
        if (!arqBodyIsDirectedCmd) {
            QString const upperProbe = probe.toUpper();
            QString bodyAfterPeer = upperProbe;
            if (!arqPeer.isEmpty()) {
                QString const peerPrefix = arqPeer.toUpper() + QStringLiteral(" ");
                if (upperProbe.startsWith(peerPrefix)) {
                    bodyAfterPeer = upperProbe.mid(peerPrefix.length()).trimmed();
                }
            }
            if (bodyAfterPeer.startsWith(QStringLiteral("TYPING"))) {
                arqBodyIsDirectedCmd = true;
            }
        }
    }

    bool const arqGateOpen  = arqEnabled && arqHasPeer && arqHasText &&
                              !arqBodyIsDirectedCmd;
    // [GATE-LOG-VERBOSE 2026-06-10 build 240]
    // Operator reported regular free text being misclassified as a
    // directed cmd → ARQ skipped → TX goes as plain freetext when
    // it should be ARQ-wrapped. The gate log alone doesn't show
    // which packer matched the text or what cmd was extracted, so
    // include the text body and parsed cmd here.
    qWarning() << "[ARQ] startTx gate:"
               << "hasMgr=" << arqHasMgr
               << "enabled=" << arqEnabled
               << "peer=" << arqPeer
               << "validPeer=" << arqHasPeer
               << "hasText=" << arqHasText
               << "isDirectedCmd=" << arqBodyIsDirectedCmd
               << "textLen=" << text.length()
               << "textHead=" << text.left(40)
               << "→ arqGateOpen=" << arqGateOpen;
    if (arqGateOpen) {
        // Strip both forms of leading addressing from the body before
        // handing to ChunkedArq. encodeChunkedData re-prepends
        // "<myCall>: <peer> " so without stripping the wire would
        // double-prefix (the symptom flagged in todo #38). Two layers:
        //   1. Optional "<myCall>:" self-prefix (JS8 typeahead form)
        //   2. Optional leading "<peer> " token
        QString arqBody = text.trimmed();
        QString const myCallUp = m_config.my_callsign().trimmed().toUpper();
        if (!myCallUp.isEmpty() &&
            arqBody.toUpper().startsWith(myCallUp + ":")) {
            arqBody = arqBody.mid(myCallUp.length() + 1).trimmed();
        }
        QString const peerPrefix = arqPeer.toUpper() + " ";
        if (arqBody.toUpper().startsWith(peerPrefix)) {
            arqBody = arqBody.mid(peerPrefix.length()).trimmed();
        }
        // [RELAY-VIA-ARQ checksum compute 2026-06-10 build 243]
        // For checksumed buffered cmds (currently: relay ">"), compute
        // the 16-bit checksum the on-air pipeline would have added and
        // bake it into the body BEFORE chunking. The receiver-side hook
        // (chunkedArqHooks::onChunkedRelayMessageReceived) populates
        // m_messageBuffer; processBufferedActivity then validates the
        // checksum just like a regular on-air cmd. No layer punch-
        // through, no special-case RX checksum-skip — the wire body
        // looks exactly like the freetext-continuation an on-air sender
        // would have produced. Same pattern (checksum16, space-prefix,
        // 3-char hex) as Varicode.cpp:2408 packMessage path.
        // [RELAY REGEX 2026-06-10 build 245] Accept both "<call>> body"
        // and "<call>>body" forms — the on-air ">" cmd's callsign
        // pattern uses type=[> ] where the type is part of the
        // callsign capture, so no space between ">" and the rest is a
        // valid wire form. Without this fix the no-space form skipped
        // the checksum and the receiver couldn't validate.
        static QRegularExpression const relayBodyRe(
            QStringLiteral(R"(^\s*[A-Z0-9/]+>\s*(?<inner>\S.*)$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (auto const m = relayBodyRe.match(arqBody); m.hasMatch()) {
            QString const inner = m.captured("inner").trimmed();
            QString const crc   = Varicode::checksum16(inner);
            arqBody = QString("%1 %2").arg(arqBody.trimmed(), crc);
            qWarning() << "[ARQ] relay-cmd: checksum16 appended"
                       << "inner=" << inner << "crc=" << crc;
        }
        qWarning() << "[ARQ] startTx: routing through ChunkedArq"
                   << "peer=" << arqPeer
                   << "bodyChars=" << arqBody.size()
                   << "(was" << text.size() << "incl prefix)";
        auto const result = m_chunkedArq->sendChunked(arqPeer, arqBody);
        if (!result.ok) {
            qWarning() << "[ARQ] sendChunked rejected:" << result.error;
            // 2026-06-07 operator request: when the super-message is
            // rejected as too long for ARQ, CANCEL the TX entirely
            // rather than falling through to plain (non-ARQ) directed
            // send. Same treatment for "too_long" and any other hard-
            // failure reason — the failure dialog already pops via
            // onChunkedSendFailed (connected to sendFailed signal), so
            // the operator sees the message; we just don't ALSO send
            // the body as a non-chunked directed blast.
            //
            // "busy" is the one fall-through-worthy case: the operator
            // hit Send while a prior ARQ session was still in flight.
            // For now we treat it identically (cancel). If a future use
            // case wants busy → plain-TX fallthrough, branch on result.
            // error here.
            ui->extFreeTextMsgEdit->clear();
            return;
        } else {
            // sendChunked already emitted wantToTransmit for chunk 1,
            // which the onChunkedWantToTransmit slot pre-filled into
            // extFreeTextMsgEdit + enqueued. Leave startTxButton
            // CHECKED — same as normal multi-frame TX. Toggling it
            // off here would re-trigger on_startTxButton_toggled(false)
            // → resetMessage → on_stopTxButton_clicked → haltAll(),
            // killing the ARQ session we just started.
            return;
        }
    }

    // ARQ gate fell through. Dispatch to the shared non-ARQ TX entry,
    // which is also what processTxQueue calls for auto-replies / bot
    // traffic — single chokepoint for the plain TX path.
    startTxNonArq();
}

// Non-ARQ TX entry. Used by:
//   - startTx (when the operator clicks Send and the ARQ gate is closed)
//   - processTxQueue (auto-replies, HB/CQ loop traffic, TCP API sends,
//     relay messages — anything that didn't originate from the operator
//     directly typing into extFreeTextMsgEdit and hitting Send)
//
// This function MUST NOT consult the ARQ gate. The whole point of
// having it separate from startTx is that the auto-reply / queue
// drain path can reach TX without any risk of accidentally wrapping
// a system-built reply in chunked-ARQ wire format. See ARQ provenance
// design note (todo #46, locked 2026-06-08): "ARQ wrapping is a thing
// that can only happen on the Send-button path, by construction."
void UI_Constructor::startTxNonArq() {
#if IDLE_BLOCKS_TX
    if (m_tx_watchdog) {
        return;
    }
#endif

    auto text = ui->extFreeTextMsgEdit->toPlainText();

    if (!ensureCreateMessageReady(text)) {
        return;
    }

    if (!prepareNextMessageFrame()) {
        return;
    }

    m_dateTimeQSOOn = QDateTime{};
    if (m_transmitting)
        m_restart = true;

    if (!m_auto)
        auto_tx_mode(true);

    // disallow editing of the text while transmitting
    // ui->extFreeTextMsgEdit->setReadOnly(true);
    update_dynamic_property(ui->extFreeTextMsgEdit, "transmitting", true);

    // update the tx button display
    updateTxButtonDisplay();
}

void UI_Constructor::transmit() {
    // Guard: don't restart audio if Modulator is already playing.
    // Double-transmit causes stop/restart which triggers USB codec sleep/wake
    // on devices like the IC-7300, producing a ~2 second audio gap.
    if (!m_modulator->isIdle()) {
        qWarning() << "[FT2-TX] transmit(): SKIPPED — Modulator already active"
                    << "freq=" << (freq() + m_XIT) << "submode=" << m_nSubMode
                    << "m_transmitting=" << m_transmitting
                    << "genAudio=" << m_generateAudioWhenPttConfirmedByTX
                    << "m_iptt=" << m_iptt << "m_iptt0=" << m_iptt0;
        return;
    }
    qWarning() << "[FT2-TX] transmit(): emitting sendMessage"
                << "freq=" << (freq() + m_XIT) << "submode=" << m_nSubMode
                << "modIdle=" << m_modulator->isIdle()
                << "m_transmitting=" << m_transmitting
                << "genAudio=" << m_generateAudioWhenPttConfirmedByTX
                << "m_iptt=" << m_iptt << "m_iptt0=" << m_iptt0;
    Q_EMIT sendMessage(freq() + m_XIT, m_nSubMode, m_TxDelay, m_soundOutput,
                       m_config.audio_output_channel());
    ui->signal_meter_widget->setValue(0, 0);
}

void UI_Constructor::stopTx() {
    // [TX-CADENCE 2026-06-09] capture timestamp on every stopTx so
    // we can correlate with the next PTT-up's msSinceLastTx to find
    // where the 1.4 s slack lives in the
    // stopTx → prepareNextMessageFrame → next-PTT chain. Observed
    // single chunk where PTT #3 was 5189 ms after PTT #2 instead of
    // the steady-state 3790 ms — RX missed exactly one decode.
    qint64 const nowMsCadence = QDateTime::currentMSecsSinceEpoch();
    qint64 const msSincePtt = m_lastTxStartTime.isValid()
        ? m_lastTxStartTime.msecsTo(DriftingDateTime::currentDateTimeUtc())
        : qint64{-1};
    qWarning() << "[TX-CADENCE] stopTx entered: msSincePtt=" << msSincePtt
               << "nowMs=" << nowMsCadence;
    qWarning() << "[FT2-TX] stopTx(): m_iptt=" << m_iptt
                << "m_iptt0=" << m_iptt0
                << "m_transmitting=" << m_transmitting
                << "submode=" << m_nSubMode
                << "m_TRperiod=" << m_TRperiod;

    // [AUDIO-CADENCE PROBE 2026-06-09] Read the timestamps captured
    // inside Modulator::readData() for this cycle. Hand-back tells us
    // when the Modulator wrote the first/last waveform sample into the
    // audio device's buffer. msSincePttToAudioStart = how long after
    // PTT the Modulator actually began emitting waveform samples (vs
    // silence). msAudioDuration = how long it took to emit the whole
    // waveform on the Modulator side (should be ~2520ms if not gated
    // by buffer back-pressure from the audio device).
    if (m_modulator && m_nSubMode == Varicode::JS8CallFT2) {
        qint64 const ttsPtt = m_lastTxStartTime.isValid()
            ? m_lastTxStartTime.toMSecsSinceEpoch()
            : qint64{-1};
        qint64 const tAudioStart = m_modulator->audioStartedMs();
        qint64 const tAudioEnd = m_modulator->audioEndedMs();
        qint64 const msPttToAudioStart =
            (ttsPtt > 0 && tAudioStart > 0) ? (tAudioStart - ttsPtt) : -1;
        qint64 const msAudioDuration =
            (tAudioStart > 0 && tAudioEnd > 0) ? (tAudioEnd - tAudioStart) : -1;
        qint64 const msAudioEndToStopTx =
            (tAudioEnd > 0) ? (nowMsCadence - tAudioEnd) : -1;
        qWarning() << "[AUDIO-CADENCE] tPtt=" << ttsPtt
                   << "tAudioStart=" << tAudioStart
                   << "tAudioEnd=" << tAudioEnd
                   << "msPttToAudioStart=" << msPttToAudioStart
                   << "msAudioDuration=" << msAudioDuration
                   << "msAudioEndToStopTx=" << msAudioEndToStopTx;
    }

    auto dt = DecodedText(m_currentMessage.trimmed(), m_currentMessageBits,
                          m_nSubMode);
    // [STATUS-BAR ARQ PROGRESS 2026-06-11 build 253]
    // When an ARQ session is active, last_tx_label is owned by the
    // ARQ progress override ("ARQ: #x/y (z repeats)"). Suppressing the
    // per-frame "Last Tx: <message>" update keeps the progress visible
    // through the cycle gap until the next chunk's progressUpdate
    // refreshes it (or progressEnd restores the pre-ARQ cache). Without
    // this, every chunk-done event would briefly overwrite the
    // progress with "Last Tx: <chunk text>" for ~3.75s before the next
    // progressUpdate clobbered it back.
    if (!m_lastTxLabelCacheValid) {
        last_tx_label.setText("Last Tx: " +
                              dt.message()); // m_currentMessage.trimmed());
    }

    Q_EMIT endTransmitMessage();

    m_btxok = false;
    m_transmitting = false;
    m_iptt = 0;
    m_lastTxStopTime = DriftingDateTime::currentDateTimeUtc();
    if (!m_tx_watchdog) {
        tx_status_label.setStyleSheet("");
        tx_status_label.setText("");
    }

    // [TODO.md #57 build 269] Restore the operator's pre-response
    // outgoing-text draft. The save in onChunkedWantsResponseTx ran
    // just before the ACK / NACK wire text replaced the widget; this
    // is the symmetric un-replace once that response TX has drained.
    //
    // Build 268 set the text synchronously here and produced a blank
    // line above the post-TX strike-through display. JS8's own post-
    // TX styling (the strike-through pass) runs LATER in the event
    // loop, so a synchronous setPlainText() here gets overlaid by
    // that styling. Defer to a singleShot queued back to the GUI
    // thread so the strike-through completes first, then we replace.
    // A 750 ms delay is generous enough to cover the post-stopTx
    // styling cascade without leaving the operator staring at the
    // wire-form ACK / NACK text for long.
    if (m_arqResponseRestorePending && ui->extFreeTextMsgEdit) {
        QString const saved = m_arqResponseSavedText;
        m_arqResponseSavedText.clear();
        m_arqResponseRestorePending = false;
        QPointer<TransmitTextEdit> const widget(ui->extFreeTextMsgEdit);
        QTimer::singleShot(750, this, [widget, saved]() {
            if (!widget) return;
            widget->setPlainText(saved);
            qCWarning(chunkedarq_js8)
                << "[ARQ-RX] outgoing-text restore (deferred 750ms):"
                << "chars=" << saved.size();
        });
    }

#if IDLE_BLOCKS_TX
    bool shouldContinue = !m_tx_watchdog && prepareNextMessageFrame();
#else
    bool shouldContinue = prepareNextMessageFrame();
#endif
    if (m_nSubMode == Varicode::JS8CallFT2)
        qWarning() << "[FT2-TX] stopTx: shouldContinue=" << shouldContinue
                   << "m_auto=" << m_auto << "m_txFrameCount=" << m_txFrameCount;
    if (!shouldContinue) {
        // TODO: jsherer - split this up...
#ifdef JS8_ENABLE_FT2
        // FT2: keep message text so user can re-send; JS8: clear it
        if (m_nSubMode != Varicode::JS8CallFT2)
#endif
        ui->extFreeTextMsgEdit->clear();
        ui->extFreeTextMsgEdit->setReadOnly(false);
        update_dynamic_property(ui->extFreeTextMsgEdit, "transmitting", false);
        bool previous_stopTxButtonIsLongterm = m_stopTxButtonIsLongterm;
        m_stopTxButtonIsLongterm = false;
        on_stopTxButton_clicked();
        m_stopTxButtonIsLongterm = previous_stopTxButtonIsLongterm;
        tryRestoreFreqOffset();

        // Notify API clients that the queued transmission block finished.
        sendNetworkMessage("TX.COMPLETE", dt.message(),
                           {{"_ID", QVariant(-1)},
                            {"SUBMODE", m_nSubMode},
                            {"UTC", QVariant(
                                DriftingDateTime::currentDateTimeUtc()
                                    .toMSecsSinceEpoch())}});
    }

    pttReleaseTimer.start(
        TX_SWITCHOFF_DELAY); // end-of-transmission sequencer delay stopTx2
    monitor(true);
    statusUpdate();
}

/**
 *  stopTx2 is called from stopTx to open the PTT
 */
void UI_Constructor::stopTx2() {
    // GM8JCF: m_txFrameCount is set to the number of frames to be transmitted
    // when the send button is pressed and remains at that count until the last
    // frame is transmitted. So, we keep the PTT ON so long as m_txFrameCount is
    // non-zero

    qCDebug(mainwindow_js8) << "stopTx2 frames left" << m_txFrameCount;

    // If we're holding the PTT and there are more frames to transmit, do not
    // emit the PTT signal
    if (!m_tune && m_config.hold_ptt() && m_txFrameCount > 0) {
        return;
    }

    // Otherwise, emit the PTT signal
    emitPTT(false);
}

void UI_Constructor::TxAgain() { auto_tx_mode(true); }

void UI_Constructor::cacheActivity(QString key) {
    m_callActivityBandCache[key] = m_callActivity;
    m_bandActivityBandCache[key] = m_bandActivity;
    m_rxTextBandCache[key] = ui->textEditRX->toHtml();
    m_heardGraphIncomingBandCache[key] = m_heardGraphIncoming;
    m_heardGraphOutgoingBandCache[key] = m_heardGraphOutgoing;
}

void UI_Constructor::restoreActivity(QString key) {
    if (m_callActivityBandCache.contains(key)) {
        m_callActivity = m_callActivityBandCache[key];
    }

    if (m_bandActivityBandCache.contains(key)) {
        m_bandActivity = m_bandActivityBandCache[key];
    }

    if (m_rxTextBandCache.contains(key)) {
        ui->textEditRX->setHtml(m_rxTextBandCache[key]);
    }

    if (m_heardGraphIncomingBandCache.contains(key)) {
        m_heardGraphIncoming = m_heardGraphIncomingBandCache[key];
    }

    if (m_heardGraphOutgoingBandCache.contains(key)) {
        m_heardGraphOutgoing = m_heardGraphOutgoingBandCache[key];
    }

    displayActivity(true);
}

void UI_Constructor::clearActivity() {
    qCDebug(mainwindow_js8) << "clear activity";

    m_callSeenHeartbeat.clear();
    m_compoundCallCache.clear();
    m_rxCallCache.clear();
    m_rxCallQueue.clear();
    m_rxRecentCache.clear();
    m_rxDirectedCache.clear();
    m_rxCommandQueue.clear();
    m_lastTxMessage.clear();

    refreshInboxCounts();


    clearBandActivity();
    clearRXActivity();
    clearCallActivity();

    displayActivity(true);
}

void UI_Constructor::clearBandActivity() {
    qCDebug(mainwindow_js8) << "clear band activity";
    m_bandActivity.clear();
    ui->tableWidgetRXAll->setRowCount(0);


    displayBandActivity();
}

void UI_Constructor::clearRXActivity() {
    qCDebug(mainwindow_js8) << "clear rx activity";

    m_rxFrameBlockNumbers.clear();
    m_rxActivityQueue.clear();

    ui->textEditRX->clear();

    // make sure to clear the read only and transmitting flags so there's always
    // a "way out"
    ui->extFreeTextMsgEdit->clear();
    ui->extFreeTextMsgEdit->setReadOnly(false);
    update_dynamic_property(ui->extFreeTextMsgEdit, "transmitting", false);
}

void UI_Constructor::clearCallActivity() {
    qCDebug(mainwindow_js8) << "clear call activity";

    m_callActivity.clear();

    m_heardGraphIncoming.clear();
    m_heardGraphOutgoing.clear();

    ui->tableWidgetCalls->setRowCount(0);

    bool showIconColumn = false;
    createGroupCallsignTableRows(ui->tableWidgetCalls, "", showIconColumn);


    displayCallActivity();
}

void UI_Constructor::createGroupCallsignTableRows(QTableWidget *table,
                                                  QString const &selectedCall,
                                                  bool &showIconColumn) {
    int count = 0;
    auto now = DriftingDateTime::currentDateTimeUtc();
    int callsignAging = m_config.callsign_aging();

    int startCol = 1;

    foreach (auto cd, m_callActivity.values()) {
        if (cd.call.trimmed().isEmpty()) {
            continue;
        }
        if (callsignAging &&
            cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
            continue;
        }
        count++;
    }

    table->horizontalHeaderItem(startCol)->setText(
        count == 0 ? columnLabel("Callsigns")
                   : QString(columnLabel("Callsigns (%1)")).arg(count));

    if (!m_config.avoid_allcall()) {
        table->insertRow(table->rowCount());

        auto emptyItem = new QTableWidgetItem("");
        emptyItem->setData(Qt::UserRole, QVariant("@ALLCALL"));
        table->setItem(table->rowCount() - 1, 0, emptyItem);

        auto item = new QTableWidgetItem(QString("@ALLCALL"));
        item->setData(Qt::UserRole, QVariant("@ALLCALL"));

        table->setItem(table->rowCount() - 1, startCol, item);
        table->setSpan(table->rowCount() - 1, startCol, 1,
                       table->columnCount());
        if (selectedCall == "@ALLCALL") {
            table->item(table->rowCount() - 1, 0)->setSelected(true);
            table->item(table->rowCount() - 1, startCol)->setSelected(true);
        }
    }

    auto groups = m_config.my_groups().values();
    std::sort(groups.begin(), groups.end());
    foreach (auto group, groups) {
        int col = 0;
        table->insertRow(table->rowCount());

        bool hasMessage = m_rxInboxCountCache.value(group, 0) > 0;

        auto iconItem = new QTableWidgetItem(hasMessage ? "\u2691" : "");
        iconItem->setData(Qt::UserRole, QVariant(group));
        iconItem->setToolTip(hasMessage ? "Message Available" : "");
        iconItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        table->setItem(table->rowCount() - 1, col++, iconItem);
        if (hasMessage) {
            showIconColumn = true;
        }

        auto item = new QTableWidgetItem(group);
        item->setData(Qt::UserRole, QVariant(group));
        item->setToolTip(generateCallDetail(group));
        table->setItem(table->rowCount() - 1, col, item);
        table->setSpan(table->rowCount() - 1, col, 1, table->columnCount());

        if (selectedCall == group) {
            table->item(table->rowCount() - 1, 0)->setSelected(true);
            table->item(table->rowCount() - 1, col)->setSelected(true);
        }
    }
}

void UI_Constructor::displayTextForFreq(QString text, int freq, QDateTime date,
                                        bool isTx, bool isNewLine,
                                        bool isLast, int submode) {
    // ChunkedArq filter: suppress raw chunked-DATA wire-form text from
    // ever reaching the conversation panel. Multiple code paths feed
    // this function (processCommandActivity, processRxActivity,
    // buffered/incremental display); intercepting at each was leaking
    // raw markers in one corner or another. Filtering at the single
    // chokepoint guarantees suppression. The chunkAdded slot's clean
    // "<peer>: <body> (CC/TT)" line and the messageDelivered slot's
    // final " body ♦" summary have no `#NN.CC/TT.HHHH` marker, so
    // they pass through unaffected.
    //
    // Cheap fast-path check: only run the full regex if the text
    // contains `#` and a digit. Saves regex cost on the >99% of frames
    // that aren't chunked.
    // NOTE: prior versions tried to suppress raw chunked-DATA wire
    // markers ("#NN.CC/TT.HHHH") here. Backed out 2026-06-04: by the
    // time a marker-bearing frame reaches displayTextForFreq, earlier
    // body-fragment frames have already painted via typeahead. Erasing
    // the prior block looked clean per-chunk but stripped legitimate
    // typeahead content. Per operator call: let the in-band markers
    // ride — ham operators are already used to seeing coded protocol
    // traffic on JS8. The clean assembled summary at messageDelivered
    // (the "♦" line) is what matters for post-QSO readback.
    int lowFreq = freq / 10 * 10;
    int highFreq = lowFreq + 10;

    int block = -1;

    if (m_rxFrameBlockNumbers.contains(freq)) {
        block = m_rxFrameBlockNumbers[freq];
    } else if (m_rxFrameBlockNumbers.contains(lowFreq)) {
        block = m_rxFrameBlockNumbers[lowFreq];
        freq = lowFreq;
    } else if (m_rxFrameBlockNumbers.contains(highFreq)) {
        block = m_rxFrameBlockNumbers[highFreq];
        freq = highFreq;
    }

    qCDebug(mainwindow_js8) << "existing block?" << block << freq;

    if (isNewLine) {
        m_rxFrameBlockNumbers.remove(freq);
        m_rxFrameBlockNumbers.remove(lowFreq);
        m_rxFrameBlockNumbers.remove(highFreq);
        block = -1;
    }

    block = writeMessageTextToUI(date, text, freq, isTx, submode, block);

    // never cache tx or last lines
    if (/*isTx || */ isLast) {
        // reset the cache so we're always progressing forward
        m_rxFrameBlockNumbers.clear();
    } else {
        m_rxFrameBlockNumbers.insert(freq, block);
        m_rxFrameBlockNumbers.insert(lowFreq, block);
        m_rxFrameBlockNumbers.insert(highFreq, block);
    }
}

void UI_Constructor::writeNoticeTextToUI(QDateTime date, QString text) {
    auto c = ui->textEditRX->textCursor();
    c.movePosition(QTextCursor::End);
    if (c.block().length() > 1) {
        c.insertBlock();
    }

    text = text.toHtmlEscaped();
    c.insertBlock();
    c.insertHtml(QString("<strong>%1 - %2</strong>")
                     .arg(date.time().toString())
                     .arg(text));

    c.movePosition(QTextCursor::End);

    ui->textEditRX->ensureCursorVisible();
    ui->textEditRX->verticalScrollBar()->setValue(
        ui->textEditRX->verticalScrollBar()->maximum());
}

int UI_Constructor::writeMessageTextToUI(QDateTime date, QString text, int freq,
                                         bool isTx, int submode, int block) {
    auto c = ui->textEditRX->textCursor();

    // find an existing block (that does not contain an EOT marker)
    bool found = false;
    if (block != -1) {
        QTextBlock b = c.document()->findBlockByNumber(block);
        c.setPosition(b.position());
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);

        auto blockText = c.selectedText();
        c.clearSelection();
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        if (!blockText.contains(m_config.eot())) {
            // Check if the existing block's mode matches the incoming submode
            // Don't append FT2 text to a Normal block or vice versa
            bool blockIsFT2 = blockText.startsWith(QString::fromUtf8("\xe2\x9a\xa1"));
            bool incomingIsFT2 = (submode == Varicode::JS8CallFT2);
            if (blockIsFT2 == incomingIsFT2)
                found = true;
        }
    }

    if (!found) {
        c.movePosition(QTextCursor::End);
        if (c.block().length() > 1) {
            c.insertBlock();
        }
    }

    // fixup duplicate acks
    auto tc = c.document()->find(text);
    if (!tc.isNull() && tc.selectedText() == text &&
        (text.contains(" ACK ") || text.contains(" HEARTBEAT SNR "))) {
        tc.select(QTextCursor::BlockUnderCursor);

        if (tc.selectedText().trimmed().startsWith(date.time().toString())) {
            qCDebug(mainwindow_js8)
                << "found" << tc.selectedText() << "so not displaying...";
            return tc.blockNumber();
        }
    }

    // Don't cross TX/RX boundary — each gets its own block
    if (found && isTx != (c.block().userState() == State::TX))
        found = false;

    if (found) {
        c.clearSelection();
        c.insertText(text);
    } else {
        text = text.toHtmlEscaped();
        text = text.replace("\n", "<br/>");
        text = text.replace("  ", "&nbsp;&nbsp;");
        c.insertBlock();
        // Mode indicator: ⚡ for FT2/Subspace, first letter for standard modes
        QString modeInd;
        if (submode == Varicode::JS8CallFT2)
            modeInd = QString::fromUtf8("\xe2\x9a\xa1");
        else if (submode == Varicode::JS8CallNormal)
            modeInd = "N";
        else if (submode == Varicode::JS8CallFast)
            modeInd = "F";
        else if (submode == Varicode::JS8CallTurbo)
            modeInd = "T";
        else if (submode == Varicode::JS8CallSlow)
            modeInd = "S";
        else
            modeInd = "?";
        c.insertHtml(QString("%1 - %2 - (%3) - %4")
                         .arg(modeInd)
                         .arg(date.time().toString())
                         .arg(freq)
                         .arg(text));
    }

    if (isTx) {
        c.block().setUserState(State::TX);
        highlightBlock(c.block(), m_config.tx_text_font(),
                       m_config.color_tx_foreground(), QColor(Qt::transparent));
    } else {
        c.block().setUserState(State::RX);
        highlightBlock(c.block(), m_config.rx_text_font(),
                       m_config.color_rx_foreground(), QColor(Qt::transparent));
    }

    ui->textEditRX->ensureCursorVisible();
    ui->textEditRX->verticalScrollBar()->setValue(
        ui->textEditRX->verticalScrollBar()->maximum());

    return c.blockNumber();
}

bool UI_Constructor::isMessageQueuedForTransmit() {
    return m_transmitting || m_txFrameCount > 0;
}

bool UI_Constructor::isInDecodeDelayThreshold(int ms) {
    if (!m_lastTxStopTime.isValid() || m_lastTxStopTime.isNull()) {
        return false;
    }

    return m_lastTxStopTime.msecsTo(DriftingDateTime::currentDateTimeUtc()) <
           ms;
}

void UI_Constructor::prependMessageText(QString text) {
    // don't add message text if we already have a transmission queued...
    if (isMessageQueuedForTransmit()) {
        return;
    }

    auto c = QTextCursor(ui->extFreeTextMsgEdit->textCursor());
    c.movePosition(QTextCursor::Start);
    c.insertText(text);
}

void UI_Constructor::addMessageText(QString text, bool clear,
                                    bool selectFirstPlaceholder) {
    // don't add message text if we already have a transmission queued...
    if (isMessageQueuedForTransmit()) {
        return;
    }

    if (clear) {
        ui->extFreeTextMsgEdit->clear();
    }

    QTextCursor c = ui->extFreeTextMsgEdit->textCursor();
    if (c.hasSelection()) {
        c.removeSelectedText();
    }

    int pos = c.position();
    c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);

    bool isSpace =
        c.selectedText().isEmpty() || c.selectedText().at(0).isSpace();
    c.clearSelection();

    c.setPosition(pos);

    if (!isSpace) {
        c.insertText(" ");
    }

    c.insertText(text);

    if (selectFirstPlaceholder) {
        auto match = QRegularExpression("(\\[[^\\]]+\\])")
                         .match(ui->extFreeTextMsgEdit->toPlainText());
        if (match.hasMatch()) {
            c.setPosition(match.capturedStart());
            c.setPosition(match.capturedEnd(), QTextCursor::KeepAnchor);
            ui->extFreeTextMsgEdit->setTextCursor(c);
        }
    }

    ui->extFreeTextMsgEdit->setFocus();
}

void UI_Constructor::confirmThenEnqueueMessage(int timeout, int priority,
                                               QString message, int offset,
                                               Callback c) {
    SelfDestructMessageBox *m = new SelfDestructMessageBox(
        timeout, "Autoreply Confirmation Required",
        QString("A transmission is queued for autoreply:\n\n%1\n\nWould you "
                "like to send this transmission?")
            .arg(message),
        QMessageBox::Question, QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No, false, this);

    connect(m, &SelfDestructMessageBox::finished, this,
            [this, m, priority, message, offset, c](int) {
                // make sure we delete the message box later...
                m->deleteLater();

                if (m->result() == QMessageBox::Yes) {
                    enqueueMessage(priority, message, offset, c);
                }
            });

    m->setWindowModality(Qt::NonModal);
    m->show();
}

void UI_Constructor::enqueueMessage(int priority, QString message, int offset,
                                    Callback c) {
    m_txMessageQueue.enqueue(PrioritizedMessage{
        DriftingDateTime::currentDateTimeUtc(), priority, message, offset, c});
}

void UI_Constructor::resetMessage() {
    resetMessageUI();
    resetMessageTransmitQueue();
}

void UI_Constructor::resetMessageUI() {
    m_nextFreeTextMsg.clear();
    ui->extFreeTextMsgEdit->clear();
    ui->extFreeTextMsgEdit->setReadOnly(false);
    // [QUEUE PROVENANCE 2026-06-10 build 247] Edit-box is being
    // cleared canonically; drop the stale snapshot so it can't ghost-
    // match if the operator types the same text by coincidence later.
    m_lastQueueInjectedText.clear();

    update_dynamic_property(ui->extFreeTextMsgEdit, "transmitting", false);

    if (ui->startTxButton->isChecked()) {
        ui->startTxButton->setChecked(false);
    }
}

bool UI_Constructor::ensureCallsignSet(bool alert) {
    if (m_config.my_callsign().trimmed().isEmpty()) {
        if (alert)
            JS8MessageBox::warning_message(
                this, tr("Please enter your callsign in the settings."));
        openSettings();
        return false;
    }

    if (m_config.my_grid().trimmed().isEmpty()) {
        if (alert)
            JS8MessageBox::warning_message(
                this, tr("Please enter your grid locator in the settings."));
        openSettings();
        return false;
    }

    return true;
}

bool UI_Constructor::ensureKeyNotStuck(QString const &text) {
    // be annoying and drop messages with all the same character to reduce
    // spam...
    if (text.length() > 5 &&
        QString(text).replace(text.at(0), "").trimmed().isEmpty()) {
        return false;
    }

    return true;
}

bool UI_Constructor::ensureNotIdle() {
    if (!m_config.watchdog()) {
        return true;
    }

    if (m_idleMinutes < m_config.watchdog()) {
        return true;
    }

    tx_watchdog(true); // disable transmit and auto replies
    return false;
}

bool UI_Constructor::ensureCanTransmit() {
    return ui->monitorTxButton->isChecked();
}

bool UI_Constructor::ensureCreateMessageReady(const QString &text) {
    if (text.isEmpty()) {
        return false;
    }

    if (!ensureCanTransmit()) {
        on_stopTxButton_clicked();
        return false;
    }

    if (!ensureCallsignSet()) {
        on_stopTxButton_clicked();
        return false;
    }

    if (!ensureNotIdle()) {
        on_stopTxButton_clicked();
        return false;
    }

    if (!ensureKeyNotStuck(text)) {
        on_stopTxButton_clicked();

        ui->monitorButton->setChecked(false);
        ui->monitorTxButton->setChecked(false);
        on_monitorButton_clicked(false);
        on_monitorTxButton_toggled(false);

        foreach (auto obj, this->children()) {
            if (obj->isWidgetType()) {
                auto wid = qobject_cast<QWidget *>(obj);
                wid->setEnabled(false);
            }
        }

        return false;
    }

    return true;
}

QString UI_Constructor::createMessage(QString const &text,
                                      bool *pDisableTypeahead) {
    return createMessageTransmitQueue(
        replaceMacros(text, buildMacroValues(), false), true, false,
        pDisableTypeahead);
}

QString UI_Constructor::appendMessage(QString const &text, bool isData,
                                      bool *pDisableTypeahead) {
    return createMessageTransmitQueue(
        replaceMacros(text, buildMacroValues(), false), false, isData,
        pDisableTypeahead);
}

QString UI_Constructor::createMessageTransmitQueue(QString const &text,
                                                   bool reset, bool isData,
                                                   bool *pDisableTypeahead) {
    if (reset) {
        resetMessageTransmitQueue();
    }

    auto frames = buildMessageFrames(text, isData, pDisableTypeahead);

    QStringList lines;
    foreach (auto frame, frames) {
        auto dt = DecodedText(frame.first, frame.second, m_nSubMode);
        lines.append(dt.message());
    }

    m_txFrameQueue.append(frames);
    m_txFrameCount += frames.length();

    // TODO: jsherer - move this outside of create message transmit queue
    // if we're transmitting a message to be displayed, we should bump the
    // repeat buttons... "Bump the repeat buttons" from 2018 probably translates
    // to "stop automatic transmission loops" in 2025: qCDebug(mainwindow_js8)
    // << "Cancel HB and CQ transmit loops in createMessageTransmitQueue";
    // m_cq_loop->onLoopCancel();
    // m_hb_loop->onLoopCancel();
    // But the loops cause this code to be executed as part of their
    // normal operation, when the first transmission is sent.
    // So the cancelation makes it impossible to iterate through the loop a
    // second time.

    // return the text
    return lines.join("");
}

void UI_Constructor::restoreMessage() {
    if (m_lastTxMessage.isEmpty()) {
        return;
    }
    // Normalize non-breaking spaces to regular spaces to prevent
    // accumulating extra spaces on each restore cycle
    auto text = Varicode::rstrip(m_lastTxMessage);
    text.replace(QChar(0xA0), QChar(' '));
    addMessageText(text, true);
}

/**
 * @brief Resets the frame-level transmission state after a message completes.
 *
 * This function clears the frame queue and resets frame counters, preparing
 * the system for the next message transmission. Importantly, it does NOT
 * clear m_txMessageQueue, which holds pending high-level messages (e.g.,
 * queued APRS relay messages) that should be transmitted after the current
 * transmission completes.
 *
 * @note Called via resetMessage() -> on_stopTxButton_clicked() when
 *       transmission ends.
 */
void UI_Constructor::resetMessageTransmitQueue() {
    m_txFrameCount = 0;
    m_txFrameCountSent = 0;
    m_txFrameQueue.clear();
    m_txQueueStartTime = QDateTime();  // invalidate countdown start
    // Note: m_txMessageQueue is intentionally NOT cleared here.
    // It holds pending messages (e.g., APRS relay messages) that should
    // be transmitted after the current transmission completes.

    // reset the total message sent
    m_totalTxMessage.clear();
}

QPair<QString, int> UI_Constructor::popMessageFrame() {
    if (m_txFrameQueue.isEmpty()) {
        return QPair<QString, int>{};
    }
    return m_txFrameQueue.dequeue();
}

void UI_Constructor::currentTextChanged() {
    auto const text = ui->extFreeTextMsgEdit->toPlainText();

    // keep track of dirty flags
    m_txTextDirty = text != m_txTextDirtyLastText;
    m_txTextDirtyLastText = text;

    // immediately update the display
    updateButtonDisplay();
    updateTextDisplay();
}

void UI_Constructor::tableSelectionChanged(QItemSelection const &,
                                           QItemSelection const &) {
    // Selection logic is handled by explicit click handlers.
    // This signal handler only updates text display state.
    // Don't steal focus here — this fires on periodic table rebuilds.
    currentTextChanged();
}

QList<QPair<QString, int>>
UI_Constructor::buildMessageFrames(const QString &text, bool isData,
                                   bool *pDisableTypeahead) {
    // prepare selected callsign for directed message
    QString selectedCall = callsignSelected();

    // prepare compound
    QString mycall = m_config.my_callsign();
    QString mygrid = m_config.my_grid().left(4);

    bool forceIdentify = !m_config.avoid_forced_identify();

    // TODO: might want to be more explicit?
    bool forceData = m_txFrameCountSent > 0 && isData;

    Varicode::MessageInfo info;
    auto frames = Varicode::buildMessageFrames(mycall, mygrid, selectedCall,
                                               text, forceIdentify, forceData,
                                               m_nSubMode, &info);

    if (pDisableTypeahead) {
        // checksummed commands should not allow typeahead
        *pDisableTypeahead = (!info.dirCmd.isEmpty() &&
                              Varicode::isCommandChecksumed(info.dirCmd));
    }

#if 0
    qCDebug(mainwindow_js8) << "frames:";
    foreach(auto frame, frames){
        auto dt = DecodedText(frame.frame, frame.bits);
        qCDebug(mainwindow_js8) << "->" << frame << dt.message() << Varicode::frameTypeString(dt.frameType());
    }
#endif

    return frames;
}

bool UI_Constructor::prepareNextMessageFrame() {
    // check to see if the last i3bit was a last bit
    bool i3bitLast = (m_i3bit & Varicode::JS8CallLast) == Varicode::JS8CallLast;

    // TODO: should this be user configurable?
    bool shouldForceDataForTypeahead = !i3bitLast;

    // reset i3
    m_i3bit = Varicode::JS8Call;

    // typeahead
    bool shouldDisableTypeahead = false;
    if (ui->extFreeTextMsgEdit->isDirty() &&
        !ui->extFreeTextMsgEdit->isEmpty()) {
        // block edit events while computing next frame
        QString newText;
        ui->extFreeTextMsgEdit->setReadOnly(true);
        {
            auto sent = ui->extFreeTextMsgEdit->sentText();
            auto unsent = ui->extFreeTextMsgEdit->unsentText();
            qCDebug(mainwindow_js8) << "text dirty for typeahead\n"
                                    << sent << "\n"
                                    << unsent;
            m_txFrameQueue.clear();
            m_txFrameCount = 0;

            newText = appendMessage(unsent, shouldForceDataForTypeahead,
                                    &shouldDisableTypeahead);

            // if this was the last frame, append a newline
            if (i3bitLast) {
                m_totalTxMessage.append("\n");
                newText.prepend("\n");
            }

            qCDebug(mainwindow_js8) << "unsent replaced to" << "\n" << newText;
        }
        ui->extFreeTextMsgEdit->setReadOnly(shouldDisableTypeahead);
        ui->extFreeTextMsgEdit->replaceUnsentText(newText, true);
        ui->extFreeTextMsgEdit->setClean();
    }

    QPair<QString, int> f = popMessageFrame();
    auto frame = f.first;
    auto bits = f.second;

    // if not the first frame, ensure first bit is not set
    if (m_txFrameCountSent > 0) {
        bits &= ~Varicode::JS8CallFirst;
    }

    // if last frame, ensure the last bit is set
    if (m_txFrameQueue.isEmpty()) {
        bits |= Varicode::JS8CallLast;
    }

    if (frame.isEmpty()) {
        m_nextFreeTextMsg.clear();
        updateTxButtonDisplay();
        return false;
    }

    // append this frame to the total message sent so far
    auto dt = DecodedText(frame, bits, m_nSubMode);
    m_totalTxMessage.append(dt.message());
    ui->extFreeTextMsgEdit->setCharsSent(m_totalTxMessage.length());
    m_txFrameCountSent += 1;
    m_lastTxMessage = m_totalTxMessage;
    qCDebug(mainwindow_js8) << "total sent:" << m_txFrameCountSent << "\n"
                            << m_totalTxMessage;

    // display the frame...
    if (m_txFrameQueue.isEmpty()) {
        displayTextForFreq(
            QString("%1 %2 ").arg(dt.message()).arg(m_config.eot()), freq(),
            DriftingDateTime::currentDateTimeUtc(), true, false, true, m_nSubMode);
    } else {
        displayTextForFreq(dt.message(), freq(),
                           DriftingDateTime::currentDateTimeUtc(), true,
                           m_txFrameCountSent == 1, false, m_nSubMode);
    }

    m_nextFreeTextMsg = frame;
    m_i3bit = bits;

    // [TX-CADENCE 2026-06-09] log when m_nextFreeTextMsg becomes
    // non-empty — this is the moment the PTT gate's
    // "msgLength > 0" condition flips true. Paired with the stopTx
    // entry log and the next [FT2-TX] PTT-up log, we get the full
    // timing chain: stopTx → prepareNextMessageFrame → next PTT.
    qWarning() << "[TX-CADENCE] prepareNextMessageFrame set"
               << "msgLength=" << QStringView(frame).trimmed().length()
               << "nowMs=" << QDateTime::currentMSecsSinceEpoch();

    updateTxButtonDisplay();

    return true;
}

bool UI_Constructor::isFreqOffsetFree(int const f, int const bw) {
    // if this frequency is our current frequency, or it's in our
    // directed cache, it's free.

    if ((freq() == f) || isDirectedOffset(f, nullptr))
        return true;

    // Run through the band activity; if there's no activity for a given
    // offset, or we last received on it more than 30 seconds ago, then
    // it's free. If it's an occupied slot within the bandwidth of where
    // we'd like to transmit, then it's not free.

    auto const now = DriftingDateTime::currentDateTimeUtc();

    for (auto [offset, activity] : m_bandActivity.asKeyValueRange()) {
        if (activity.isEmpty() ||
            activity.last().utcTimestamp.secsTo(now) >= 30)
            continue;

        if (qAbs(offset - f) < bw)
            return false;
    }

    return true;
}

int UI_Constructor::findFreeFreqOffset(int fmin, int fmax, int bw) {
    int nslots = (fmax - fmin) / bw;

    int f = fmin;
    for (int i = 0; i < nslots; i++) {
        f = fmin + bw * (QRandomGenerator::global()->generate() % nslots);
        if (isFreqOffsetFree(f, bw)) {
            return f;
        }
    }

    for (int i = 0; i < nslots; i++) {
        f = fmin + (QRandomGenerator::global()->generate() % (fmax - fmin));
        if (isFreqOffsetFree(f, bw)) {
            return f;
        }
    }

    // return fmin if there's no free offset
    return fmin;
}

#if 0
// schedulePing
void UI_Constructor::scheduleHeartbeat(bool first){
    auto timestamp = DriftingDateTime::currentDateTimeUtc();

    // if we have the heartbeat interval disabled, return early, unless this is a "heartbeat now"
    if(!m_config.heartbeat() && !first){
        heartbeatTimer.stop();
        return;
    }

    // remove milliseconds
    auto t = timestamp.time();
    t.setHMS(t.hour(), t.minute(), t.second());
    timestamp.setTime(t);

    // round to 15 second increment
    int secondsSinceEpoch = (timestamp.toMSecsSinceEpoch()/1000);
    int delta = roundUp(secondsSinceEpoch, 15) + 1 + (first ? 0 : qMax(1, m_config.heartbeat()) * 60) - secondsSinceEpoch;
    timestamp = timestamp.addSecs(delta);

    // 25% of the time, switch intervals
    float prob = (float) QRandomGenerator::global()->generate() / (RAND_MAX);
    if(prob < 0.25){
        timestamp = timestamp.addSecs(15);
    }

    m_nextHeartbeat = timestamp;
    m_nextHeartbeatQueued = false;
    m_nextHeartPaused = false;

    if(!heartbeatTimer.isActive()){
        heartbeatTimer.setInterval(1000);
        heartbeatTimer.start();
    }
}

// pausePing
void UI_Constructor::pauseHeartbeat(){
    m_nextHeartPaused = true;

    if(heartbeatTimer.isActive()){
        heartbeatTimer.stop();
    }
}

// unpausePing
void UI_Constructor::unpauseHeartbeat(){
    scheduleHeartbeat(false);
}

// checkPing
void UI_Constructor::checkHeartbeat(){
    if(m_config.heartbeat() <= 0){
        return;
    }
    auto secondsUntilHeartbeat = DriftingDateTime::currentDateTimeUtc().secsTo(m_nextHeartbeat);
    if(secondsUntilHeartbeat > 5 && m_txHeartbeatQueue.isEmpty()){
        return;
    }
    if(m_nextHeartbeatQueued){
        return;
    }
    if(m_tx_watchdog){
        return;
    }

    // idle heartbeat watchdog!
    if (m_config.watchdog() && m_idleMinutes >= m_config.watchdog ()){
      tx_watchdog (true);       // disable transmit
      return;
    }

    prepareHeartbeat();
}

// preparePing
void UI_Constructor::prepareHeartbeat(){
    QStringList lines;

    QString mycall = m_config.my_callsign();
    QString mygrid = m_config.my_grid().left(4);

    // JS8Call Style
    if(m_txHeartbeatQueue.isEmpty()){
        lines.append(QString("%1: HEARTBEAT %2").arg(mycall).arg(mygrid));
    } else {
        while(!m_txHeartbeatQueue.isEmpty() && lines.length() < 1){
            lines.append(m_txHeartbeatQueue.dequeue());
        }
    }

    // Choose a ping frequency
    auto f = m_config.heartbeat_anywhere() ? -1 : findFreeFreqOffset(500, 1000, 50);

    auto text = lines.join(QChar('\n'));
    if(text.isEmpty()){
        return;
    }

    // Queue the ping
    enqueueMessage(PriorityLow, text, f, [this](){
        m_nextHeartbeatQueued = false;
    });

    m_nextHeartbeatQueued = true;
}
#endif

void UI_Constructor::on_startTxButton_toggled(bool checked) {
    if (checked) {
        startTx();
    } else {
        resetMessage();
        on_stopTxButton_clicked();
        stopTx();
    }
}

void UI_Constructor::toggleTx(bool start) {
    if (start && ui->startTxButton->isChecked()) {
        return;
    }
    if (!start && !ui->startTxButton->isChecked()) {
        return;
    }
    qCDebug(mainwindow_js8)
        << "toggleTx(" << start << ") setting the TX button.";
    ui->startTxButton->setChecked(start);
    ui->extFreeTextMsgEdit->setFocus();
}

void UI_Constructor::on_logQSOButton_clicked() // Log QSO button
{
    QString call = callsignSelected();
    if (m_callSelectedTime.contains(call)) {
        m_dateTimeQSOOn = m_callSelectedTime[call];
    }
    if (!m_dateTimeQSOOn.isValid()) {
        m_dateTimeQSOOn = DriftingDateTime::currentDateTimeUtc();
    }
    auto dateTimeQSOOff = DriftingDateTime::currentDateTimeUtc();
    if (dateTimeQSOOff < m_dateTimeQSOOn)
        dateTimeQSOOff = m_dateTimeQSOOn;

    if (call.startsWith("@")) {
        call = "";
    }
    QString grid = "";
    if (m_callActivity.contains(call)) {
        grid = m_callActivity[call].grid;
    }
    QString opCall = m_opCall;
    if (opCall.isEmpty()) {
        opCall = m_config.my_callsign();
    }

    QString comments = ui->textEditRX->textCursor().selectedText();

    // don't reset the log window if the call hasn't changed.
    if (!m_logDlg->currentCall().isEmpty() &&
        call.trimmed() == m_logDlg->currentCall()) {
        m_logDlg->show();
        return;
    }

    // kj4ctd - hackish but I don't see anywhere else that we set rptSent
    if (m_callActivity.contains(call)) {
        auto cd = m_callActivity[call];
        if (cd.snr > -50) {
            m_rptSent = Varicode::formatSNR(cd.snr);
        }
    }

    m_logDlg->initLogQSO(call.trimmed(), grid.trimmed(), "JS8", m_rptSent,
                         m_rptRcvd, m_dateTimeQSOOn, dateTimeQSOOff,
                         m_freqNominal + freq(), m_config.my_callsign(),
                         m_config.my_grid(), opCall, comments);
}

void UI_Constructor::acceptQSO(
    QDateTime const &QSO_date_off, QString const &call, QString const &grid,
    Frequency dial_freq, QString const &mode, QString const &submode,
    QString const &rpt_sent, QString const &rpt_received,
    QString const &comments, QString const &name, QDateTime const &QSO_date_on,
    QString const &operator_call, QString const &my_call,
    QString const &my_grid, QByteArray const &ADIF,
    QVariantMap const &additionalFields) {
    QString date = QSO_date_on.toString("yyyyMMdd");
    m_logBook.addAsWorked(m_hisCall, m_config.bands()->find(m_freqNominal),
                          mode, submode, grid, date, name, comments);

    qCDebug(mainwindow_js8) << "acceptQSO rptSent (" << m_rptSent << ")";
    qCDebug(mainwindow_js8) << "acceptQSO rptRcvd (" << m_rptRcvd << ")";

    // Log to JS8Call API
    if (canSendNetworkMessage()) {
        sendNetworkMessage(
            "LOG.QSO", QString(ADIF),
            {{"_ID", QVariant(-1)},
             {"UTC.ON", QVariant(QSO_date_on.toMSecsSinceEpoch())},
             {"UTC.OFF", QVariant(QSO_date_off.toMSecsSinceEpoch())},
             {"CALL", QVariant(call)},
             {"GRID", QVariant(grid)},
             {"FREQ", QVariant(dial_freq)},
             {"MODE", QVariant(mode)},
             {"SUBMODE", QVariant(submode)},
             {"RPT.SENT", QVariant(rpt_sent)},
             {"RPT.RECV", QVariant(rpt_received)},
             {"NAME", QVariant(name)},
             {"COMMENTS", QVariant(comments)},
             {"STATION.OP", QVariant(operator_call)},
             {"STATION.CALL", QVariant(my_call)},
             {"STATION.GRID", QVariant(my_grid)},
             {"EXTRA", additionalFields}});
    }

    // Log to N1MM Logger
    if (m_config.broadcast_to_n1mm() && m_config.valid_n1mm_info()) {
        const QHostAddress n1mmhost = QHostAddress(m_config.n1mm_server_name());
        QUdpSocket _sock;
        auto rzult = _sock.writeDatagram(ADIF + " <eor>", n1mmhost,
                                         quint16(m_config.n1mm_server_port()));
        if (rzult == -1) {
            bool hidden = m_logDlg->isHidden();
            m_logDlg->setHidden(true);
            JS8MessageBox::warning_message(
                this, tr("Error sending log to N1MM"),
                tr("Write returned \"%1\"").arg(rzult));
            m_logDlg->setHidden(hidden);
        }
    }

    // Log to N3FJP Logger
    if (m_config.broadcast_to_n3fjp() && m_config.valid_n3fjp_info()) {
        QString data = QString("<CMD>"
                               "<ADDDIRECT>"
                               "<EXCLUDEDUPES>TRUE</EXCLUDEDUPES>"
                               "<STAYOPEN>FALSE</STAYOPEN>"
                               "<fldDateStr>%1</fldDateStr>"
                               "<fldTimeOnStr>%2</fldTimeOnStr>"
                               "<fldCall>%3</fldCall>"
                               "<fldGridR>%4</fldGridR>"
                               "<fldBand>%5</fldBand>"
                               "<fldFrequency>%6</fldFrequency>"
                               "<fldMode>JS8</fldMode>"
                               "<fldOperator>%7</fldOperator>"
                               "<fldNameR>%8</fldNameR>"
                               "<fldComments>%9</fldComments>"
                               "<fldRstS>%10</fldRstS>"
                               "<fldRstR>%11</fldRstR>"
                               "%12"
                               "</CMD>");

        data = data.arg(QSO_date_on.toString("yyyy/MM/dd"));
        data = data.arg(QSO_date_on.toString("H:mm"));
        data = data.arg(call);
        data = data.arg(grid);
        data = data.arg(m_config.bands()->find(dial_freq).replace("m", ""));
        data = data.arg(Radio::frequency_MHz_string(dial_freq));
        data = data.arg(operator_call);
        data = data.arg(name);
        data = data.arg(comments);
        data = data.arg(rpt_sent);
        data = data.arg(rpt_received);

        int other = 0;
        QStringList additional;
        if (!additionalFields.isEmpty()) {
            foreach (auto key, additionalFields.keys()) {
                QString n3key;
                if (N3FJP_ADIF_MAP.contains(key)) {
                    n3key = N3FJP_ADIF_MAP.value(key);
                } else {
                    other++;
                    n3key = N3FJP_ADIF_MAP.value(QString("*%1").arg(other));
                }

                if (n3key.isEmpty()) {
                    break;
                }
                auto value = additionalFields[key].toString();
                additional.append(QString("<%1>%2</%1>").arg(n3key).arg(value));
            }
        }
        data = data.arg(additional.join(""));

        auto host = m_config.n3fjp_server_name();
        auto port = m_config.n3fjp_server_port();

        if (m_n3fjpClient->sendNetworkMessage(host, port, data.toLocal8Bit(),
                                              true, 500)) {
            QTimer::singleShot(300, this, [this, host, port]() {
                m_n3fjpClient->sendNetworkMessage(
                    host, port, "<CMD><CHECKLOG></CMD>", true, 100);
                m_n3fjpClient->sendNetworkMessage(host, port, "\r\n", true,
                                                  100);
            });
        } else {
            bool hidden = m_logDlg->isHidden();
            m_logDlg->setHidden(true);
            JS8MessageBox::warning_message(
                this, tr("Error sending log to N3FJP"),
                tr("Write failed for \"%1:%2\"").arg(host).arg(port));
            m_logDlg->setHidden(hidden);
        }
    }

    /**
     * @brief Log QSO to WSJT-X Protocol
     *
     * Sends QSO logged information to WSJT-X protocol clients. Sends both
     * the QSOLogged message and the LoggedADIF message (type 12) which
     * contains the ADIF formatted record. The ADIF message is what most
     * WSJT-X clients actually use for logging.
     */
    if (m_wsjtxMessageMapper && m_config.wsjtx_protocol_enabled()) {
        m_wsjtxMessageMapper->sendQSOLogged(QSO_date_off, call, grid, dial_freq,
                                            mode, rpt_sent, rpt_received,
                                            my_call, my_grid);
        // Also send ADIF formatted message (this is what clients actually use)
        if (m_wsjtxMessageClient) {
            m_wsjtxMessageClient->logged_ADIF(ADIF);
        }
    }

    // reload the logbook data
    m_logBook.init();

    clearCallsignSelected();

    displayCallActivity();

    m_dateTimeQSOOn = QDateTime{};
}

void UI_Constructor::on_actionModeJS8HB_toggled(bool) {
    // prep hb mode

    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());
    displayActivity(true);

    setupJS8();
}

void UI_Constructor::on_actionHeartbeatAcknowledgements_toggled(bool) {
    // prep hb ack mode

    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());
    displayActivity(true);

    setupJS8();
}

void UI_Constructor::on_actionModeMultiDecoder_toggled(bool checked) {
    Q_UNUSED(checked);

    displayActivity(true);

    setupJS8();
}

void UI_Constructor::on_actionModeReplicatorProtocol_toggled(bool checked) {
    // Single source of truth lives on ChunkedArq::Manager. Modulator
    // mirrors it via setArqRelax so the audio-thread side sees the
    // change atomically. prepareSending reads arqInProgress() directly
    // and combines with mode check.
    if (m_chunkedArq) {
        m_chunkedArq->setArqEnabled(checked);
    }
    // [TODO.md #58 build 268] First time the operator enables ARQ in
    // this program run, latch the multi-mode RX override true. Sticky
    // — never cleared. Unlocking Subspace decode for legacy-mode
    // operators is half the point of turning ARQ on: peers may send
    // chunked-DATA in FT2 / Subspace.
    if (checked && !m_arqMultiModeOverride) {
        m_arqMultiModeOverride = true;
        qWarning() << "[ARQ] multi-mode RX override latched ON via button";
    }
    if (m_modulator) {
        m_modulator->setArqRelax(checked);
    }
    // Refresh the comprehensive mode-flag summary on the top-right
    // modeButton so the "+ARQ" suffix appears / disappears immediately
    // without waiting for the next setSubmode / updateButtonDisplay
    // pass.
    updateModeButtonText();
    // Also refresh the status-bar mode_label so its " + ARQ" suffix
    // toggles in sync (setSubmode is the other write site and only
    // fires on actual submode change).
    QString modeText = (m_nSubMode == Varicode::JS8CallFT2
        ? QString::fromUtf8("\xe2\x9a\xa1 Subspace")
        : JS8::Submode::name(m_nSubMode));
    if (checked) {
        modeText += QStringLiteral(" + ARQ");
    }
    mode_label.setText(modeText);
    // Repaint the ARQ button so its background reflects the new
    // toggle state combined with the current callsign selection
    // (blue when both hold, gray otherwise). updateButtonDisplay is
    // the single source of truth for that two-input style decision.
    updateButtonDisplay();
    qWarning() << "[ARQ] Auto Repeat Request" << (checked ? "ENABLED" : "DISABLED");
}

void UI_Constructor::on_actionModeJS8Normal_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeJS8Fast_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeJS8Turbo_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeJS8Slow_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeJS8Ultra_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeFT2_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeAutoreply_toggled(bool) {
    // update the HB ack option (needs autoreply on)
    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());

    // then update the js8 mode
    setupJS8();
}

bool UI_Constructor::canCurrentModeSendHeartbeat() const {
    return (m_nSubMode == Varicode::JS8CallFast ||
            m_nSubMode == Varicode::JS8CallNormal ||
            m_nSubMode == Varicode::JS8CallSlow ||
            m_nSubMode == Varicode::JS8CallFT2);
}

// HB-ACK is the auto-reply that fires when we *hear* someone else's HB.
// It is intentionally narrower than canCurrentModeSendHeartbeat:
//   - Turbo: doesn't send HB at all, so it never has anything to ack.
//   - FT2/Subspace: HBs are HAILs — presence beacons that don't want
//     a swarm of SNR-report chatter; the user's HB-ACK preference is
//     preserved for when they switch back to Slow/Normal/Fast.
bool UI_Constructor::canCurrentModeAckHeartbeat() const {
    return (m_nSubMode == Varicode::JS8CallFast ||
            m_nSubMode == Varicode::JS8CallNormal ||
            m_nSubMode == Varicode::JS8CallSlow);
}

void UI_Constructor::prepareMonitorControls() {
    // on_monitorButton_toggled(!m_config.monitor_off_at_startup());
    ui->monitorTxButton->setChecked(!m_config.transmit_off_at_startup());
}

void UI_Constructor::prepareHeartbeatMode(bool enabled) {
    // Not all submodes supports HBs.
    m_hbModeAvailable = enabled;
    ui->hbMacroButton->setEnabled(enabled);
    if (!enabled) {
        if (m_hb_loop->isActive())
            qWarning() << "[HAIL-DIAG] loop cancelled: mode not available";
        m_hb_loop->onLoopCancel();
        ui->hbMacroButton->setChecked(false);
    }
    ui->actionHeartbeat->setEnabled(enabled);
    ui->actionModeJS8HB->setEnabled(canCurrentModeSendHeartbeat());
    ui->actionHeartbeatAcknowledgements->setEnabled(
        enabled && ui->actionModeAutoreply->isChecked());

#if 0
    if(enabled){
        m_config.addGroup("@HB");
    } else {
        m_config.removeGroup("@HB");
    }
#endif

#if 0
    //ui->actionCQ->setEnabled(!enabled);
    //ui->actionFocus_Message_Reply_Area->setEnabled(!enabled);

    // default to not displaying the other buttons
    // ui->cqMacroButton->setVisible(!enabled);
    // ui->replyMacroButton->setVisible(!enabled);
    // ui->snrMacroButton->setVisible(!enabled);
    // ui->infoMacroButton->setVisible(!enabled);
    // ui->macrosMacroButton->setVisible(!enabled);
    // ui->queryButton->setVisible(!enabled);
    // ui->extFreeTextMsgEdit->setVisible(!enabled);
    // if(enabled){
    //     ui->extFreeTextMsgEdit->clear();
    // }

    // show heartbeat and acks in hb mode only
    // ui->actionShow_Band_Heartbeats_and_ACKs->setChecked(enabled);
    // ui->actionShow_Band_Heartbeats_and_ACKs->setVisible(true);
    // ui->actionShow_Band_Heartbeats_and_ACKs->setEnabled(false);
#endif

    updateHBButtonDisplay();
    updateButtonDisplay();
}

void UI_Constructor::setupJS8() {
    m_nSubMode = Varicode::JS8CallNormal;

    if (ui->actionModeJS8Normal->isChecked())
        m_nSubMode = Varicode::JS8CallNormal;
    else if (ui->actionModeJS8Fast->isChecked())
        m_nSubMode = Varicode::JS8CallFast;
    else if (ui->actionModeJS8Turbo->isChecked())
        m_nSubMode = Varicode::JS8CallTurbo;
    else if (ui->actionModeJS8Slow->isChecked())
        m_nSubMode = Varicode::JS8CallSlow;
    else if (ui->actionModeJS8Ultra->isChecked())
        m_nSubMode = Varicode::JS8CallUltra;
#ifdef JS8_ENABLE_FT2
    else if (ui->actionModeFT2->isChecked())
        m_nSubMode = Varicode::JS8CallFT2;
#endif

    // Only enable heartbeat/HAIL for modes that support it
    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());

    // Update menu label: HAIL in Subspace, HB in other modes
    ui->actionModeJS8HB->setText(
        m_nSubMode == Varicode::JS8CallFT2
        ? "Enable HAIL Presence Beacon"
        : "Enable Heartbeat Networking (HB)");
    ui->actionHeartbeatAcknowledgements->setVisible(
        m_nSubMode != Varicode::JS8CallFT2);
    ui->actionHeartbeat->setText(
        m_nSubMode == Varicode::JS8CallFT2
        ? "Send Hailing Message..."
        : "Send &Heartbeat...");

    updateModeButtonText();

    m_wideGraph->setSubMode(m_nSubMode);
    m_wideGraph->setFilterMinimumBandwidth(
        JS8::Submode::bandwidth(m_nSubMode) +
        JS8::Submode::rxThreshold(m_nSubMode) * 2);

    enable_DXCC_entity(m_config.DXCC());
    m_config.frequencies()->filter(m_config.region(), Mode::JS8);
    m_FFTSize = JS8_NSPS / 2;
    Q_EMIT FFTSize(m_FFTSize);
    setup_status_bar();
    m_TRperiod = JS8::Submode::period(m_nSubMode);
    m_wideGraph->show();

    Q_ASSERT(JS8_NTMAX == 60);
    m_wideGraph->setPeriod(JS8::Submode::periodMS(m_nSubMode));
    m_detector->setTRPeriod(JS8_NTMAX); // safe: m_period is atomic

    // Update mode switch buttons and status bar label
    // Block signals to prevent setChecked() from re-triggering setSubmode()
    // 2026-06-07 (arq-statusfix): preserve the " + ARQ" suffix the
    // setSubmode + toggle handlers carry. This setupJS8 path was the
    // third mode_label write site and was missing the suffix — it
    // overwrote the indicator at startup and on mode change, so even
    // when ARQ was persisted-on, the status line stayed bare.
    QString modeText = (m_nSubMode == Varicode::JS8CallFT2
        ? QString::fromUtf8("\xe2\x9a\xa1 Subspace")
        : JS8::Submode::name(m_nSubMode));
    if (ui->actionModeReplicatorProtocol &&
        ui->actionModeReplicatorProtocol->isChecked()) {
        modeText += QStringLiteral(" + ARQ");
    }
    mode_label.setText(modeText);
    // Speed-mode buttons use canChangeSpeed (NO arqBusy gate) so the
    // operator can switch speed BETWEEN chunks during an ARQ session
    // to adapt to changing band conditions mid-super-message
    // (operator request 2026-06-08 — drop to Slow if SNR tanks, bump
    // to Turbo if it's clean). The ARQ-toggle stays locked the full
    // session via the periodic poll's canChangeMode; setupJS8 doesn't
    // touch the ARQ button.
    bool const canChangeSpeed = !m_transmitting && !m_tune &&
                                m_txFrameCount == 0 && m_txFrameQueue.isEmpty();
    if (m_modeBtnNormal) { m_modeBtnNormal->blockSignals(true); m_modeBtnNormal->setChecked(m_nSubMode == Varicode::JS8CallNormal); m_modeBtnNormal->blockSignals(false); m_modeBtnNormal->setEnabled(canChangeSpeed); }
    if (m_modeBtnFast)   { m_modeBtnFast->blockSignals(true);   m_modeBtnFast->setChecked(m_nSubMode == Varicode::JS8CallFast);     m_modeBtnFast->blockSignals(false);   m_modeBtnFast->setEnabled(canChangeSpeed); }
    if (m_modeBtnTurbo)  { m_modeBtnTurbo->blockSignals(true);  m_modeBtnTurbo->setChecked(m_nSubMode == Varicode::JS8CallTurbo);   m_modeBtnTurbo->blockSignals(false);  m_modeBtnTurbo->setEnabled(canChangeSpeed); }
    if (m_modeBtnSlow)   { m_modeBtnSlow->blockSignals(true);   m_modeBtnSlow->setChecked(m_nSubMode == Varicode::JS8CallSlow);     m_modeBtnSlow->blockSignals(false);   m_modeBtnSlow->setEnabled(canChangeSpeed); }
    if (m_modeBtnFT2)    { m_modeBtnFT2->blockSignals(true);    m_modeBtnFT2->setChecked(m_nSubMode == Varicode::JS8CallFT2);       m_modeBtnFT2->blockSignals(false);    m_modeBtnFT2->setEnabled(canChangeSpeed); }

    updateTextDisplay();
    refreshTextDisplay();
    statusChanged();
}

void UI_Constructor::setFreq(int const n) {
    m_freq = n;
    m_wideGraph->setFreq(n);
    Q_EMIT transmitFrequency(n + m_XIT);
    statusUpdate();
    updateButtonDisplay();
}

void UI_Constructor::on_actionErase_ALL_TXT_triggered() // Erase ALL.TXT
{
    int ret = JS8MessageBox::query_message(
        this, tr("Confirm Erase"),
        tr("Are you sure you want to erase file ALL.TXT?"));
    if (ret == JS8MessageBox::Yes) {
        QFile f{m_config.writeable_data_dir().absoluteFilePath("ALL.TXT")};
        f.remove();
        m_RxLog = 1;
    }
}

void UI_Constructor::on_actionErase_js8call_log_adi_triggered() {
    int ret = JS8MessageBox::query_message(
        this, tr("Confirm Erase"),
        tr("Are you sure you want to erase file js8call_log.adi?"));
    if (ret == JS8MessageBox::Yes) {
        QFile f{
            m_config.writeable_data_dir().absoluteFilePath("js8call_log.adi")};
        f.remove();

        m_logBook.init();
    }
}

void UI_Constructor::on_actionOpen_log_directory_triggered() {
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(m_config.writeable_data_dir().absolutePath()));
}

void UI_Constructor::band_changed() {
    if (m_config.pwrBandTxMemory() && !m_tune) {
        if (m_pwrBandTxMemory.contains(m_lastBand)) {
            ui->outAttenuation->setValue(m_pwrBandTxMemory[m_lastBand].toInt());
        } else {
            m_pwrBandTxMemory[m_lastBand] = ui->outAttenuation->value();
        }
    }
}

void UI_Constructor::enable_DXCC_entity(bool /*on*/) {
    m_logBook.init(); // re-read the log and cty.dat files
    updateGeometry();
}

void UI_Constructor::buildFrequencyMenu(QMenu *menu) {
    auto custom = menu->addAction("Set a Custom Frequency...");

    connect(custom, &QAction::triggered, this, [this]() {
        bool ok = false;
        auto currentFreq = Radio::frequency_MHz_string(dialFrequency());
        QString newFreq =
            QInputDialog::getText(this, tr("Set a Custom Frequency"),
                                  tr("Frequency in MHz:"), QLineEdit::Normal,
                                  currentFreq, &ok)
                .toUpper()
                .trimmed();
        if (!ok) {
            return;
        }

        setRig(Radio::frequency(newFreq, 6));
    });

    menu->addSeparator();

    auto frequencies = m_config.frequencies()->frequency_list();
    std::sort(frequencies.begin(), frequencies.end(),
              [](FrequencyList_v3::Item &a, FrequencyList_v3::Item &b) {
                  return a.frequency_ < b.frequency_;
              });

    foreach (auto f, frequencies) {
        auto freq = Radio::pretty_frequency_MHz_string(f.frequency_);
        auto const &band = m_config.bands()->find(f.frequency_);

        QString description = (f.description_.isEmpty()) ? ""
            : QString(" - %1").arg(f.description_);

        auto a =
            menu->addAction(QString("%1:%2%2%3 MHz%4")
                                .arg(band)
                                .arg(QString(" ").repeated(5 - band.length()))
                                .arg(freq)
                                .arg(description));
        connect(a, &QAction::triggered, this,
                [this, f]() { setRig(f.frequency_); });
    }
}

void UI_Constructor::buildHeartbeatMenu(QMenu *menu) {
    if (m_hbInterval > 0) {
        bool isHailMode = (m_nSubMode == Varicode::JS8CallFT2);
        auto startStop = menu->addAction(ui->hbMacroButton->isChecked()
                                             ? (isHailMode ? "Stop Hailing Timer" : "Stop Heartbeat Timer")
                                             : (isHailMode ? "Start Hailing Timer" : "Start Heartbeat Timer"));
        connect(startStop, &QAction::triggered, this,
                [this]() { ui->hbMacroButton->toggle(); });
        menu->addSeparator();
    }

    buildRepeatMenu(menu, ui->hbMacroButton, false, &m_hbInterval);

    menu->addSeparator();
    auto now = menu->addAction(
        m_nSubMode == Varicode::JS8CallFT2 ? "Send Hailing Message Now" : "Send Heartbeat Now");
    connect(now, &QAction::triggered, this, &UI_Constructor::sendHB);
}

void UI_Constructor::buildCQMenu(QMenu *menu) {
    if (m_cqInterval > 0) {
        auto startStop =
            menu->addAction(ui->cqMacroButton->isChecked() ? "Stop CQ Timer"
                                                           : "Start CQ Timer");
        connect(startStop, &QAction::triggered, this,
                [this]() { ui->cqMacroButton->toggle(); });
        menu->addSeparator();
    }

    buildRepeatMenu(menu, ui->cqMacroButton, true, &m_cqInterval);

    menu->addSeparator();
    auto now = menu->addAction("Send CQ Now");
    connect(now, &QAction::triggered, this, [this]() { sendCQ(false); });
}

void UI_Constructor::buildRepeatMenu(QMenu *menu, QPushButton *button,
                                     bool isLowInterval, int *interval) {
    QList<QPair<QString, int>> items = {
        {"On demand / do not repeat", 0},
        {"Repeat every 1 minute", 1},
        {"Repeat every 5 minutes", 5},
        {"Repeat every 10 minutes", 10},
        {"Repeat every 15 minutes", 15},
        {"Repeat every 30 minutes", 30},
        {"Repeat every 60 minutes", 60},
        {"Repeat every N minutes (Custom Interval)",
         -1}, // this needs to be last because of isSet bool
    };

    if (isLowInterval) {
        items.removeAt(6); // remove the sixty minute interval
        items.removeAt(5); // remove the thirty minute interval
    } else {
        items.removeAt(2); // remove the five minute interval
        items.removeAt(1); // remove the one minute interval
    }

    auto customFormat = QString("Repeat every %1 minutes (Custom Interval)");

    QActionGroup *group = new QActionGroup(menu);

    bool isSet = false;
    foreach (auto pair, items) {
        int minutes = pair.second;
        bool isMatch = *interval == minutes;
        bool isCustom = (minutes == -1 && isSet == false);
        if (isMatch) {
            isSet = true;
        }

        auto text = pair.first;
        if (isCustom) {
            text = QString(customFormat).arg(*interval);
        }

        QAction *action = menu->addAction(text);
        action->setData(minutes);
        action->setCheckable(true);
        action->setChecked(isMatch || isCustom);
        group->addAction(action);

        connect(
            action, &QAction::toggled, this,
            [this, action, customFormat, minutes, interval,
             button](bool checked) {
                int min = minutes;

                if (checked) {

                    if (minutes == -1) {
                        bool ok = false;
                        min =
                            QInputDialog::getInt(this, "Repeat every N minutes",
                                                 "Minutes", 0, 1, 1440, 1, &ok);
                        if (!ok) {
                            return;
                        }
                        action->setText(QString(customFormat).arg(*interval));
                    }

                    *interval = min;

                    if (min > 0) {
                        // force a re-toggle
                        button->setChecked(false);
                    }
                    button->setChecked(min > 0);
                }
            });
    }
}

void UI_Constructor::sendHB() {
    // Build 122: live-state guard for "Pause HAIL when in a QSO".
    // The cancel-on-select path at selectCallsign() can race a queued
    // TxLoop tick — onLoopCancel() doesn't dequeue an already-queued
    // signal, so one HB/HAIL can still fire after the user selects a
    // callsign. Check the current selection here so the guard is the
    // source of truth even when the cancel arrived too late.
    if (m_config.heartbeat_qso_pause() && !callsignSelected().isEmpty())
        return;

    QString mycall = m_config.my_callsign();
    QString mygrid = m_config.my_grid().left(4);

    QStringList parts;
    parts.append(QString("%1:").arg(mycall));

    bool isHail = (m_nSubMode == Varicode::JS8CallFT2);

    if (isHail) {
        parts.clear();
        if (m_config.hail_single_frame()) {
            // Single-frame HAIL: @ALLCALL ACK — 1 directed frame.
            // Faster (3.75s). Relies on sync normalization for decode.
            parts.append(QString("%1: @ALLCALL ACK").arg(mycall));
        } else {
            // Two-frame HAIL: @ALLCALL + grid — directed + data frame.
            // Slower (7.5s) but includes grid and decodes more reliably.
            parts.append(QString("%1: @ALLCALL %2").arg(mycall).arg(mygrid));
        }
    } else {
#if JS8_CUSTOMIZE_HB
        auto hb = m_config.hb_message();
#else
        auto hb = QString{};
#endif
        if (hb.isEmpty()) {
            parts.append("HEARTBEAT");
            parts.append(mygrid);
        } else {
            parts.append(hb);
        }
    }

    QString message = parts.join(" ").trimmed();

    auto f = isHail
        ? findFreeFreqOffset(500, 850, 50)
        : findFreeFreqOffset(500, 1000, 50);

    if (freq() <= 1000) {
        f = freq();
    } else if (m_config.heartbeat_anywhere()) {
        f = -1;
    }

    qWarning() << "[" << (isHail ? "HAIL" : "HB") << "] sending:" << message << "freq=" << f;

    enqueueMessage(isHail ? PriorityHigh : (PriorityLow + 1), message, f, nullptr);
    processTxQueue();
}

void UI_Constructor::sendHeartbeatAck(QString to, int snr, QString extra) {
#if JS8_HB_ACK_SNR_CONFIGURABLE
    auto message = m_config.heartbeat_ack_snr()
                       ? QString("%1 SNR %2 %3")
                             .arg(to)
                             .arg(Varicode::formatSNR(snr))
                             .arg(extra)
                             .trimmed()
                       : QString("%1 ACK %2").arg(to).arg(extra).trimmed();
#else
    auto message = QString("%1 HEARTBEAT SNR %2 %3")
                       .arg(to)
                       .arg(Varicode::formatSNR(snr))
                       .arg(extra)
                       .trimmed();
#endif

    auto f =
        m_config.heartbeat_anywhere() ? -1 : findFreeFreqOffset(500, 1000, 50);

    if (m_config.autoreply_confirmation()) {
        confirmThenEnqueueMessage(90, PriorityLow + 1, message, f,
                                  [this]() { processTxQueue(); });
    } else {
        enqueueMessage(PriorityLow + 1, message, f, nullptr);
        processTxQueue();
    }
}

void UI_Constructor::on_hbMacroButton_toggled(bool checked) {
    qWarning() << "[HAIL-DIAG] hbMacroButton toggled:" << checked
               << "interval=" << m_hbInterval
               << "loopActive=" << m_hb_loop->isActive();
    if (checked) {
        // only clear callsign if we do not allow hbs while in qso
        if (m_config.heartbeat_qso_pause()) {
            clearCallsignSelected();
        }

        if (m_hbInterval) {
            if (!m_hb_loop->isActive()) {
                qWarning() << "[HAIL-DIAG] starting loop, period="
                           << m_hbInterval << "min";
                m_hb_loop->onTxLoopPeriodChangeStart(m_hbInterval *
                                                     (qint64)60000);
            }
        } else {
            qWarning() << "[HAIL-DIAG] single-shot send (no loop)";
            m_hb_loop->onLoopCancel();
            // Heartbeat, but not in a loop.
            sendHB();

            // make this button emulate a single press button
            ui->hbMacroButton->setChecked(false);
        }
    } else {
        if (m_hb_loop->isActive() && m_hbButtonIsLongterm) {
            qWarning() << "[HAIL-DIAG] loop cancelled: button unchecked (longterm)";
            m_hb_loop->onLoopCancel();
        }
    }
    qCDebug(mainwindow_js8)
        << "updateHBButtonDisplay called via on_hbMacroButton_toggled";
    updateHBButtonDisplay();
}

void UI_Constructor::on_hbMacroButton_clicked() {}

void UI_Constructor::sendCQ(bool repeat) {
    // Build 122: same live-state guard as sendHB. Same queued-tick race.
    if (m_config.heartbeat_qso_pause() && !callsignSelected().isEmpty())
        return;

    if (!repeat && m_cq_loop->isActive()) {
        qCDebug(mainwindow_js8) << "Cancel CQ loop on single-shot CQ";
        m_cq_loop->onLoopCancel();
    }
    if (!repeat && m_hb_loop->isActive()) {
        qCDebug(mainwindow_js8) << "Cancel HB loop on single-shot CQ";
        m_hb_loop->onLoopCancel();
    }

    QString message;
    message = m_config.cq_message();
    if (message.isEmpty())
        message = QString("CQ CQ CQ %1").arg(m_config.my_grid().left(4)).trimmed();

    clearCallsignSelected();

    addMessageText(replaceMacros(message, buildMacroValues(), true));

    if (repeat || m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::on_cqMacroButton_toggled(bool checked) {
    qCDebug(mainwindow_js8) << "on_cqMacroButton_toggled(" << checked << ")";
    if (checked) {
        // Build 122: gate clear-on-enable on the heartbeat_qso_pause
        // setting so CQ button matches HB button policy (HB does the
        // same conditional clear at on_hbMacroButton_toggled). Prior
        // CQ behavior was to clear unconditionally, inconsistent with
        // the setting label.
        if (m_config.heartbeat_qso_pause())
            clearCallsignSelected();

        if (m_cqInterval) {
            qCDebug(mainwindow_js8)
                << "Starting CQ loop from on_cqMacroButton_toggled()";
            m_cq_loop->onTxLoopPeriodChangeStart(m_cqInterval * (qint64)60000);
        } else {
            qCDebug(mainwindow_js8)
                << "Sending single CQ from on_cqMacroButton_toggled()";
            m_cq_loop->onLoopCancel();
            sendCQ(false);

            // make this button emulate a single press button
            ui->cqMacroButton->setChecked(false);
        }
    } else {
        if (m_cq_loop->isActive() && m_cqButtonIsLongterm) {
            qCDebug(mainwindow_js8)
                << "Stopping CQ loop from on_cqMacroButton_toggled()";
            m_cq_loop->onLoopCancel();
        }
    }
    qCDebug(mainwindow_js8)
        << "updateCQButtonDisplay called via on_cqMacroButton_toggled";
    updateCQButtonDisplay();
}

void UI_Constructor::on_cqMacroButton_clicked() {
}

void UI_Constructor::on_replyMacroButton_clicked() {
    QString call = callsignSelected();
    if (call.isEmpty()) {
        return;
    }

    auto message = m_config.reply_message();
    message = replaceMacros(message, buildMacroValues(), true);
    addMessageText(QString("%1 %2").arg(call).arg(message));

    if (m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::on_snrMacroButton_clicked() {
    QString call = callsignSelected();
    if (call.isEmpty()) {
        return;
    }

    auto now = DriftingDateTime::currentDateTimeUtc();
    int callsignAging = m_config.callsign_aging();
    if (!m_callActivity.contains(call)) {
        return;
    }

    auto cd = m_callActivity[call];
    if (callsignAging && cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
        return;
    }

    auto snr = Varicode::formatSNR(cd.snr);

    addMessageText(QString("%1 SNR %2").arg(call).arg(snr));

    if (m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::on_infoMacroButton_clicked() {
    QString info = m_config.my_info();
    if (info.isEmpty()) {
        return;
    }

    addMessageText(
        QString("INFO %1").arg(replaceMacros(info, buildMacroValues(), true)));

    if (m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::on_statusMacroButton_clicked() {
    QString status = m_config.my_status();
    if (status.isEmpty()) {
        return;
    }

    addMessageText(QString("STATUS %1")
                       .arg(replaceMacros(status, buildMacroValues(), true)));

    if (m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::on_typingMacroButton_clicked() {
    // TYPING... should send without callsign prefix
    m_config.set_avoid_forced_identify(true);

    addMessageText(QString("TYPING..."));

    if (m_config.transmit_directed())
        toggleTx(true);
}

// [BUILD 298] "Send using ARQ" menu action handler. Replaces the
// standalone ARQ-toggle button. Each invocation: validate peer, enable
// the internal ARQ-on flag, fire the normal Send path, and arrange for
// ARQ to disable on sendComplete / sendFailed. There is no persistent
// "ARQ-on" state — ARQ is opt-in per-message via this action.
void UI_Constructor::on_sendUsingArqAction_triggered() {
    if (!m_chunkedArq) {
        JS8MessageBox::warning_message(
            this, QStringLiteral("ARQ unavailable"),
            QStringLiteral("Chunked-ARQ manager is not initialized; "
                           "cannot send via ARQ."));
        return;
    }
    if (!ui->actionModeReplicatorProtocol) {
        return;  // shouldn't happen in practice
    }
    // Validate text & peer using the existing gate logic. The menu
    // item enable state mirrors this; the runtime check here is a
    // belt-and-suspenders against keyboard-driven activation when
    // the menu refresh hasn't caught up.
    QString const txText = ui->extFreeTextMsgEdit
        ? ui->extFreeTextMsgEdit->toPlainText() : QString();
    if (txText.trimmed().isEmpty()) {
        JS8MessageBox::information_message(
            this, QStringLiteral("Nothing to send"),
            QStringLiteral("Type a message in the outgoing box first."));
        return;
    }
    ArqGateState const gate = evaluateArqGateForText(txText);
    if (gate != ArqGateState::Armed) {
        QString reason = (gate == ArqGateState::NotArmed_NoPeer)
            ? QStringLiteral("Select a call sign first — ARQ needs a "
                             "single peer to ACK.")
            : QStringLiteral("This message looks like a JS8 directed "
                             "command, which ARQ does not wrap.");
        JS8MessageBox::information_message(
            this, QStringLiteral("Cannot send via ARQ"), reason);
        return;
    }

    // Enable ARQ for this send. The toggle handler at
    // on_actionModeReplicatorProtocol_toggled propagates to
    // ChunkedArq::Manager + Modulator via setArqEnabled / setArqRelax.
    if (!ui->actionModeReplicatorProtocol->isChecked()) {
        ui->actionModeReplicatorProtocol->setChecked(true);
        qCWarning(chunkedarq_js8)
            << "[ARQ-MENU] Send-using-ARQ: enabled ARQ for this send; "
               "will disable on sendComplete / sendFailed";
    }
    // Mark this as a "menu-initiated ARQ text send" so the next
    // chunked send's terminal event (sendComplete or sendFailed)
    // disables ARQ. msgId is captured after the normal Send path
    // dispatches via ChunkedArq's wrapText hook — see
    // onChunkedSendComplete / onChunkedSendFailed for the match.
    // Setting to -1 means "next send from this menu, msgId unknown
    // yet"; the hook captures it on first emit.
    m_arqTextSendMsgId = -1;

    // Fire the normal Send path. The chunked-ARQ wrapping happens
    // automatically inside the TX pipeline because ARQ is now on.
    toggleTx(true);
}

// [FILE-XFER 2026-06-16 build 276] Send-File button — Phase 1 entry
// point for ARQ file transfer. Pick a local file ≤ 30 KB → build the
// wire body (header b32 + payload b32) → hand to ChunkedArq, which
// chunks + ACKs + retries via the existing protocol.
void UI_Constructor::on_sendFileButton_clicked() {
    // Pre-flight gates. [FILE-XFER build 282] ARQ is no longer a
    // pre-flight bail — it auto-enables for the transfer (and
    // restores to its prior state on sendComplete / sendFailed). We
    // still need m_chunkedArq + the action object to exist for the
    // toggle path below.
    if (!m_chunkedArq) {
        JS8MessageBox::warning_message(
            this, QStringLiteral("ARQ unavailable"),
            QStringLiteral("Chunked-ARQ manager is not initialized; "
                           "cannot send file."));
        return;
    }
    QString peer = callsignSelected().trimmed();
    if (peer.isEmpty()) {
        QString const myCall = m_config.my_callsign().trimmed().toUpper();
        QString probe = ui->extFreeTextMsgEdit->toPlainText()
                            .toUpper().trimmed();
        if (!myCall.isEmpty() && probe.startsWith(myCall + ":")) {
            probe = probe.mid(myCall.length() + 1).trimmed();
        }
        static QRegularExpression const leadingCallRe(
            QStringLiteral(R"(^\s*(@?[A-Z0-9/]+))"));
        auto const m = leadingCallRe.match(probe);
        if (m.hasMatch()) peer = m.captured(1);
    }
    if (peer.isEmpty() || peer.startsWith('@') ||
        !Radio::is_callsign(peer)) {
        JS8MessageBox::information_message(
            this,
            QStringLiteral("Select a peer"),
            QStringLiteral("File transfer needs a single peer "
                           "callsign — pick one from Call Activity "
                           "or type it as the first word in the "
                           "outgoing box. Group targets (@ALLCALL, "
                           "@PUBLIC, custom groups) are not "
                           "supported because the protocol needs a "
                           "single station to ACK."));
        return;
    }

    QString const filePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Pick a file to send via ARQ"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStringLiteral("Any file (*)"));
    if (filePath.isEmpty()) return;  // user cancelled

    QFileInfo const fi(filePath);
    if (!fi.exists() || !fi.isReadable()) {
        JS8MessageBox::warning_message(
            this, QStringLiteral("File error"),
            QStringLiteral("Cannot read file:\n%1").arg(filePath));
        return;
    }
    if (fi.size() > FileTransfer::MAX_FILE_BYTES) {
        JS8MessageBox::warning_message(
            this, QStringLiteral("File too large"),
            QStringLiteral(
                "File is %1 bytes; Phase 1 cap is %2 bytes (%3 KB). "
                "Compress externally or wait for Phase 2.")
                .arg(fi.size())
                .arg(FileTransfer::MAX_FILE_BYTES)
                .arg(FileTransfer::MAX_FILE_BYTES / 1024));
        return;
    }

    FileTransfer::FileHeader header;
    QString const body = FileTransfer::buildSendBody(filePath, header);
    if (body.isEmpty()) {
        JS8MessageBox::warning_message(
            this, QStringLiteral("File transfer failed"),
            QStringLiteral("Could not build send body for %1 "
                           "(see log for detail).").arg(filePath));
        return;
    }

    // [FILE-XFER build 282] Capture ARQ state and auto-enable it for
    // this transfer. We restore the prior state when the matching
    // sendComplete / sendFailed fires (gated on msgId so concurrent
    // ARQ traffic for other peers doesn't trip the restore).
    bool const arqWasOn = ui->actionModeReplicatorProtocol &&
                          ui->actionModeReplicatorProtocol->isChecked();
    bool arqAutoEnabled = false;
    if (!arqWasOn && ui->actionModeReplicatorProtocol) {
        ui->actionModeReplicatorProtocol->setChecked(true);
        arqAutoEnabled = true;
        qCWarning(chunkedarq_js8)
            << "[FT-TX] ARQ auto-enabled for file transfer; will "
               "restore to OFF on sendComplete/sendFailed";
    }

    qCWarning(chunkedarq_js8)
        << "[FT-TX] dispatching to ChunkedArq — peer=" << peer
        << "name=" << header.name
        << "bytes=" << header.bytes
        << "wireBodyChars=" << body.size();

    auto const res = m_chunkedArq->sendChunked(peer, body);
    if (res.ok) {
        if (arqAutoEnabled) {
            m_fileSendMsgId           = res.msgId;
            m_arqStateBeforeFileSend  = arqWasOn;
        }
    } else if (arqAutoEnabled) {
        // sendChunked rejected at pre-flight (too_long, busy, etc.).
        // No async sendComplete / sendFailed will fire on our auto-
        // enable, so restore ARQ immediately.
        ui->actionModeReplicatorProtocol->setChecked(arqWasOn);
        qCWarning(chunkedarq_js8)
            << "[FT-TX] sendChunked rejected (" << res.error
            << "); ARQ restored to prior state immediately";
    }
}

void UI_Constructor::setShowColumn(QString tableKey, QString columnKey,
                                   bool value) {
    m_showColumnsCache[tableKey + columnKey] = QVariant(value);
    displayBandActivity();
    displayCallActivity();
}

bool UI_Constructor::showColumn(QString tableKey, QString columnKey,
                                bool default_) {
    return m_showColumnsCache.value(tableKey + columnKey, QVariant(default_))
        .toBool();
}

QString UI_Constructor::columnLabel(QString defaultLabel) {
    bool minimalLabels = showColumn("all", "minimal_labels", false);

    // If we are not rendering minimal labels, return the default
    if (!minimalLabels) {
        return defaultLabel;
    }

    // If there is an entry, send it, if not, return default
    return m_columnLabelMap.value(defaultLabel, defaultLabel);
}

void UI_Constructor::buildShowColumnsMenu(QMenu *menu, QString tableKey) {
    QList<QPair<QString, QString>> columnKeys = {
        {"Frequency Offset", "offset"},
        {"Last heard timestamp", "timestamp"},
        {"SNR", "snr"},
        {"Time Delta", "tdrift"},
        {"Mode Speed", "submode"},
    };

    QMap<QString, bool> defaultOverride = {
        {"submode", false},  {"tdrift", false},  {"grid", false},
        {"distance", false}, {"azimuth", false}, {"minimal_labels", false}};

    if (tableKey == "call") {
        columnKeys.prepend({"Callsign", "callsign"});
        columnKeys.append({
            {"Grid Locator", "grid"},
            {"Distance", "distance"},
            {"Azimuth", "azimuth"},
            {"Worked Before", "log"},
            {"Logged Name", "logName"},
            {"Logged Comment", "logComment"},
        });
    }

    columnKeys.prepend({"Minimal Column Labels", "minimal_labels"});
    columnKeys.prepend({"Show Column Labels", "labels"});

    int columnIndex = 0;
    QString origTableKey = tableKey;
    foreach (auto p, columnKeys) {
        auto columnLabel = p.first;
        auto columnKey = p.second;

        auto a = menu->addAction(columnLabel);
        a->setCheckable(true);

        // Add separator after second item
        // If this is the second item, it is the minimal labels item, so set the
        // table key to all
        if (++columnIndex == 2) {
            tableKey = "all";
            menu->addSeparator();
        }

        bool showByDefault = true;
        if (defaultOverride.contains(columnKey)) {
            showByDefault = defaultOverride[columnKey];
        }
        a->setChecked(showColumn(tableKey, columnKey, showByDefault));

        connect(a, &QAction::triggered, this, [this, a, tableKey, columnKey]() {
            setShowColumn(tableKey, columnKey, a->isChecked());
        });

        // If we have switched to a custom table key in this iteration, reset to
        // the original key
        if (tableKey != origTableKey) {
            tableKey = origTableKey;
        }
    }
}

void UI_Constructor::setSortBy(QString key, QString value) {
    m_sortCache[key] = QVariant(value);
    displayBandActivity();
    displayCallActivity();
}

QString UI_Constructor::getSortBy(QString const &key,
                                  QString const &defaultValue) const {
    return m_sortCache.value(key, QVariant(defaultValue)).toString();
}

UI_Constructor::SortByReverse
UI_Constructor::getSortByReverse(QString const &key,
                                 QString const &defaultValue) const {
    auto const sortBy = getSortBy(key, defaultValue);
    auto const reverse = sortBy.startsWith("-");

    return {reverse ? sortBy.sliced(1) : sortBy, reverse};
}

void UI_Constructor::buildSortByMenu(QMenu *menu, QString key,
                                     QString defaultValue,
                                     QList<QPair<QString, QString>> values) {
    auto currentSortBy = getSortBy(key, defaultValue);

    QActionGroup *g = new QActionGroup(menu);
    g->setExclusive(true);

    foreach (auto p, values) {
        auto k = p.first;
        auto v = p.second;
        auto a = menu->addAction(k);
        a->setCheckable(true);
        a->setChecked(v == currentSortBy);
        a->setActionGroup(g);

        connect(a, &QAction::triggered, this, [this, a, key, v]() {
            if (a->isChecked()) {
                setSortBy(key, v);
            }
        });
    }
}

void UI_Constructor::buildBandActivitySortByMenu(QMenu *menu) {
    buildSortByMenu(menu, "bandActivity", "offset",
                    {{"Frequency offset", "offset"},
                     {"Last heard timestamp (oldest first)", "timestamp"},
                     {"Last heard timestamp (recent first)", "-timestamp"},
                     {"SNR (weakest first)", "snr"},
                     {"SNR (strongest first)", "-snr"},
                     {"Mode Speed (slowest first)", "submode"},
                     {"Mode Speed (fastest first)", "-submode"}});
}

void UI_Constructor::buildCallActivitySortByMenu(QMenu *menu) {
    buildSortByMenu(menu, "callActivity", "callsign",
                    {{"Callsign", "callsign"},
                     {"Callsigns Replied (recent first)", "ackTimestamp"},
                     {"Frequency offset", "offset"},
                     {"Distance (closest first)", "distance"},
                     {"Distance (farthest first)", "-distance"},
                     {"Azimuth", "azimuth"},
                     {"Last heard timestamp (oldest first)", "timestamp"},
                     {"Last heard timestamp (recent first)", "-timestamp"},
                     {"SNR (weakest first)", "snr"},
                     {"SNR (strongest first)", "-snr"},
                     {"Mode Speed (slowest first)", "submode"},
                     {"Mode Speed (fastest first)", "-submode"}});
}

void buildQueryMenu(); // JS8_Mainwindow/buildQueryMenu.cpp

void UI_Constructor::buildRelayMenu(QMenu *menu) {
    auto now = DriftingDateTime::currentDateTimeUtc();
    int callsignAging = m_config.callsign_aging();
    foreach (auto cd, m_callActivity.values()) {
        if (callsignAging &&
            cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
            continue;
        }

        menu->addAction(buildRelayAction(cd.call));
    }
}

QAction *UI_Constructor::buildRelayAction(QString call) {
    QAction *a = new QAction(call, nullptr);
    connect(a, &QAction::triggered, this,
            [this, call]() { prependMessageText(QString("%1>").arg(call)); });
    return a;
}

void UI_Constructor::buildEditMenu(QMenu *menu, QTextEdit *edit) {
    bool hasSelection = !edit->textCursor().selectedText().isEmpty();

    auto cut = menu->addAction("Cu&t");
    cut->setEnabled(hasSelection && !edit->isReadOnly());
    connect(edit, &QTextEdit::copyAvailable, this,
            [edit, cut](bool copyAvailable) {
                cut->setEnabled(copyAvailable && !edit->isReadOnly());
            });
    connect(cut, &QAction::triggered, this, [edit]() {
        edit->copy();
        edit->textCursor().removeSelectedText();
    });

    auto copy = menu->addAction("&Copy");
    copy->setEnabled(hasSelection);
    connect(edit, &QTextEdit::copyAvailable, this,
            [copy](bool copyAvailable) { copy->setEnabled(copyAvailable); });
    connect(copy, &QAction::triggered, edit, &QTextEdit::copy);

    auto paste = menu->addAction("&Paste");
    paste->setEnabled(edit->canPaste());
    connect(paste, &QAction::triggered, edit, &QTextEdit::paste);
}

QMap<QString, QString> UI_Constructor::buildMacroValues() {
    auto lastActive =
        DriftingDateTime::currentDateTimeUtc().addSecs(-m_idleMinutes * 60);
    QString myIdle = since(lastActive).toUpper().replace("NOW", "0M");
    QString myVersion = version().replace("-devel", "").replace("-rc", "");

    QMap<QString, QString> values = {
        {"<MYCALL>", m_config.my_callsign()},
        {"<MYGRID4>", m_config.my_grid().left(4)},
        {"<MYGRID12>", m_config.my_grid().left(12)},
        {"<MYINFO>", m_config.my_info()},
        {"<MYHB>", m_config.hb_message()},
        {"<MYCQ>", m_config.cq_message()},
        {"<MYREPLY>", m_config.reply_message()},
        {"<MYSTATUS>", m_config.my_status()},

        {"<MYVERSION>", myVersion},
        {"<MYIDLE>", myIdle},
    };

    auto selectedCall = callsignSelected();
    if (m_callActivity.contains(selectedCall)) {
        auto cd = m_callActivity[selectedCall];

        values["<CALL>"] = selectedCall;
        values["<TDELTA>"] = QString("%1 ms").arg((int)(1000 * cd.tdrift));

        if (cd.snr > -31) {
            values["<SNR>"] = Varicode::formatSNR(cd.snr);
        }
    }

    // these macros can have recursive macros
    values["<MYINFO>"] = replaceMacros(values["<MYINFO>"], values, false);
    values["<MYSTATUS>"] = replaceMacros(values["<MYSTATUS>"], values, false);
    values["<MYCQ>"] = replaceMacros(values["<MYCQ>"], values, false);
    values["<MYHB>"] = replaceMacros(values["<MYHB>"], values, false);
    values["<MYREPLY>"] = replaceMacros(values["<MYREPLY>"], values, false);

    return values;
}

void UI_Constructor::buildColumnLabelMap() {
    // This is the map of full-length strings to shortened versions
    // Add new minimal labels here as needed
    m_columnLabelMap = {{"Callsigns", "Call"}, {"Callsigns (%1)", "Call(%1)"},
                        {"Offset", "Off"},     {"SNR", "SN"},
                        {"Time Delta", "TD"},  {"Speed", "Sp"},
                        {"Distance", "Dist"},  {"Azimuth", "Az"},
                        {"%1 ms", "%1"},       {"%1 dB", "%1"},
                        {"%1 Hz", "%1"}};

    // Populate original header maps
    int cols = ui->tableWidgetRXAll->columnCount();
    for (int c = 0; c < cols; ++c) {
        QString label = ui->tableWidgetRXAll->horizontalHeaderItem(c)->text();

        m_origRxHeaderLabelMap[c] = label;
    }

    cols = ui->tableWidgetCalls->columnCount();
    for (int c = 0; c < cols; ++c) {
        QString label = ui->tableWidgetCalls->horizontalHeaderItem(c)->text();

        m_origCallActivityHeaderLabelMap[c] = label;
    }
}

void UI_Constructor::buildSuggestionsMenu(QMenu *menu, QTextEdit *edit,
                                          const QPoint &point) {
    if (!m_config.spellcheck()) {
        return;
    }

    bool found = false;

    auto c = edit->cursorForPosition(point);
    if (c.charFormat().underlineStyle() != QTextCharFormat::WaveUnderline) {
        return;
    }

    c.movePosition(QTextCursor::StartOfWord);
    c.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);

    auto word = c.selectedText().toUpper().trimmed();
    if (word.isEmpty()) {
        return;
    }

    QStringList suggestions = JSCChecker::suggestions(word, 5, &found);
    if (suggestions.isEmpty() && !found) {
        return;
    }

    if (suggestions.isEmpty()) {
        auto a = menu->addAction("No Suggestions");
        a->setDisabled(true);
    } else {
        foreach (auto suggestion, suggestions) {
            auto a = menu->addAction(suggestion);

            connect(a, &QAction::triggered, this, [edit, point, suggestion]() {
                auto c = edit->cursorForPosition(point);
                c.select(QTextCursor::WordUnderCursor);
                c.insertText(suggestion);
            });
        }
    }

    menu->addSeparator();
}

void UI_Constructor::buildSavedMessagesMenu(QMenu *menu) {
    auto values = buildMacroValues();

    foreach (QString macro, m_config.macros()->stringList()) {
        QAction *action = menu->addAction(replaceMacros(macro, values, false));
        connect(action, &QAction::triggered, this, [this, macro]() {
            auto values = buildMacroValues();
            addMessageText(replaceMacros(macro, values, true));

            if (m_config.transmit_directed())
                toggleTx(true);
        });
    }

    menu->addSeparator();

    auto editAction = new QAction(QString("&Edit Saved Messages"), menu);
    menu->addAction(editAction);
    connect(editAction, &QAction::triggered, this,
            [this]() { openSettings(5); });

    auto saveAction = new QAction(QString("&Save Current Message"), menu);
    saveAction->setDisabled(ui->extFreeTextMsgEdit->toPlainText().isEmpty());
    menu->addAction(saveAction);
    connect(saveAction, &QAction::triggered, this, [this]() {
        auto macros = m_config.macros();
        if (macros->insertRow(macros->rowCount())) {
            auto index = macros->index(macros->rowCount() - 1);
            macros->setData(index, ui->extFreeTextMsgEdit->toPlainText());
            writeSettings();
        }
    });
}

void UI_Constructor::on_queryButton_pressed() {
    QMenu *menu = ui->queryButton->menu();
    if (!menu) {
        menu = new QMenu(ui->queryButton);
    }
    menu->clear();

    buildQueryMenu(menu, callsignSelected());

    ui->queryButton->setMenu(menu);
    ui->queryButton->showMenu();
}

void UI_Constructor::on_macrosMacroButton_pressed() {
    QMenu *menu = ui->macrosMacroButton->menu();
    if (!menu) {
        menu = new QMenu(ui->macrosMacroButton);
    }
    menu->clear();

    buildSavedMessagesMenu(menu);

    ui->macrosMacroButton->setMenu(menu);
    ui->macrosMacroButton->showMenu();
}

void UI_Constructor::on_deselectButton_pressed() { clearCallsignSelected(); }

void UI_Constructor::on_tableWidgetRXAll_cellClicked(int row, int /*col*/) {
    ui->tableWidgetCalls->blockSignals(true);
    ui->tableWidgetCalls->selectionModel()->select(
        ui->tableWidgetCalls->selectionModel()->selection(),
        QItemSelectionModel::Deselect);
    ui->tableWidgetCalls->blockSignals(false);

    displayCallActivity();

    auto item = ui->tableWidgetRXAll->item(row, 0);
    if (!item) return;

    int offset = item->data(Qt::UserRole).toInt();
    // Read submode from the row's speed column (set by displayBandActivity)
    auto speedItem = ui->tableWidgetRXAll->item(row, 4);
    int rowSubmode = speedItem ? speedItem->data(Qt::UserRole).toInt() : -1;

    qWarning() << "[UI] click band activity: row=" << row << "offset=" << offset
               << "rowSubmode=" << rowSubmode
               << "speedText=" << (speedItem ? speedItem->text() : "null");

    // Find callsign from sub-divided Message(s) column
    QString call;
    int msgCol = ui->tableWidgetRXAll->columnCount() - 1;
    auto msgItem = ui->tableWidgetRXAll->item(row, msgCol);

    if (msgItem) {
        // Try sub-region detection first (grouped callsigns)
        auto groups = msgItem->data(Qt::UserRole).toList();
        if (groups.size() > 1) {
            // Get mouse X relative to the Message(s) cell
            QPoint cursorPos = ui->tableWidgetRXAll->viewport()->mapFromGlobal(QCursor::pos());
            int cellLeft = ui->tableWidgetRXAll->columnViewportPosition(msgCol);
            int cellWidth = ui->tableWidgetRXAll->columnWidth(msgCol);
            int mouseX = cursorPos.x() - cellLeft;
            call = BandActivityMessageDelegate::callsignAtPosition(groups, cellWidth, mouseX);
        }

        // Fallback: parse from concatenated text (single group or no groups)
        if (call.isEmpty()) {
            QString rowText = msgItem->text();
            if (!rowText.isEmpty()) {
                int lastColon = -1;
                for (int i = rowText.length() - 1; i >= 0; --i) {
                    if (rowText[i] == ':') { lastColon = i; break; }
                }
                if (lastColon > 0) {
                    int ws = lastColon - 1;
                    while (ws >= 0 && (rowText[ws].isLetterOrNumber() || rowText[ws] == '/'))
                        --ws;
                    ++ws;
                    QString cand = rowText.mid(ws, lastColon - ws).trimmed();
                    if (cand.length() >= 3 && cand.length() <= 15) {
                        bool hl = false, hd = false;
                        for (auto ch : cand) {
                            if (ch.isLetter()) hl = true;
                            if (ch.isDigit()) hd = true;
                        }
                        if (hl && hd && cand != m_config.my_callsign())
                            call = cand;
                    }
                }
                if (call.isEmpty()) {
                    QString first = rowText.trimmed().split(' ').first();
                    if (first.length() >= 3 && first.length() <= 10) {
                        bool hl = false, hd = false;
                        for (auto ch : first) {
                            if (ch.isLetter()) hl = true;
                            if (ch.isDigit()) hd = true;
                        }
                        if (hl && hd && first != m_config.my_callsign())
                            call = first;
                    }
                }
            }
        }
    }

    // Fallback: m_callActivity by offset (standard JS8Call behavior)
    if (call.isEmpty()) {
        int threshold = rowSubmode >= 0 ? JS8::Submode::rxThreshold(rowSubmode) : 10;
        for (auto const &key : m_callActivity.keys()) {
            auto const &d = m_callActivity[key];
            if (abs(d.offset - offset) <= threshold) {
                call = d.call;
                break;
            }
        }
    }

    if (!call.isEmpty())
        selectCallsign(call, rowSubmode);
    else
        clearSelection();
}

void UI_Constructor::on_tableWidgetRXAll_cellDoubleClicked(int row, int col) {
    on_tableWidgetRXAll_cellClicked(row, col);

    // TODO: jsherer - could also parse the messages for the last callsign?
    auto item = ui->tableWidgetRXAll->item(row, 0);
    int offset = item->text().replace(" Hz", "").toInt();

    // switch to the offset of this row (inhibit if too low — likely noise)
    if (offset > 1000)
        setFreqOffsetForRestore(offset, false);

    // print the history in the main window...
    // Read submode from the row's speed column to filter by mode
    auto speedItem = ui->tableWidgetRXAll->item(row, 4);
    int rowSubmode = speedItem ? speedItem->data(Qt::UserRole).toInt() : -1;
    bool rowIsFT2 = (rowSubmode == Varicode::JS8CallFT2);

    qWarning() << "[UI] dblclick band activity: offset=" << offset
               << "rowSubmode=" << rowSubmode << "rowIsFT2=" << rowIsFT2
               << "entries=" << m_bandActivity[offset].size();

    int activityAging = m_config.activity_aging();
    QDateTime now = DriftingDateTime::currentDateTimeUtc();
    QDateTime firstActivity = now;
    QString activityText;
    bool isLast = false;
    int activitySubmode = rowSubmode;
    int included = 0, excluded = 0;
    foreach (auto d, m_bandActivity[offset]) {
        // Only include entries matching the clicked row's mode
        if ((d.submode == Varicode::JS8CallFT2) != rowIsFT2) {
            excluded++;
            continue;
        }
        included++;
        if (activityAging && d.utcTimestamp.secsTo(now) / 60 >= activityAging) {
            continue;
        }
        if (activityText.isEmpty()) {
            firstActivity = d.utcTimestamp;
        }
        activityText.append(d.text);
        activitySubmode = d.submode;

        isLast = (d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast;
        if (isLast) {
            activityText = QString("%1 %2 ")
                               .arg(Varicode::rstrip(activityText))
                               .arg(m_config.eot());
        }
    }

    qWarning() << "[UI] dblclick result: included=" << included
               << "excluded=" << excluded << "activitySubmode=" << activitySubmode
               << "textLen=" << activityText.length();

    if (!activityText.isEmpty()) {
        displayTextForFreq(activityText, offset, firstActivity, false, true,
                           isLast, activitySubmode);
    }
}

QString UI_Constructor::generateCallDetail(QString selectedCall) {
    if (selectedCall.isEmpty()) {
        return "";
    }

    // heard detail
    QString hearing =
        m_heardGraphOutgoing.value(selectedCall).values().join(", ");
    QString heardby =
        m_heardGraphIncoming.value(selectedCall).values().join(", ");
    QStringList detail = {
        QString("<h1>%1</h1>").arg(selectedCall.toHtmlEscaped()),
        hearing.isEmpty() ? ""
                          : QString("<p><strong>HEARING</strong>: %1</p>")
                                .arg(hearing.toHtmlEscaped()),
        heardby.isEmpty() ? ""
                          : QString("<p><strong>HEARD BY</strong>: %1</p>")
                                .arg(heardby.toHtmlEscaped()),
    };

    return detail.join("\n");
}

void UI_Constructor::on_tableWidgetCalls_cellClicked(int row, int /*col*/) {
    ui->tableWidgetRXAll->blockSignals(true);
    ui->tableWidgetRXAll->selectionModel()->select(
        ui->tableWidgetRXAll->selectionModel()->selection(),
        QItemSelectionModel::Deselect);
    ui->tableWidgetRXAll->blockSignals(false);

    displayBandActivity();

    // Read callsign and submode from the clicked row
    auto item = ui->tableWidgetCalls->item(row, 0);
    if (item) {
        auto call = item->data(Qt::UserRole).toString();
        int submode = -1;
        if (m_callActivity.contains(call))
            submode = m_callActivity[call].submode;
        if (!call.isEmpty())
            selectCallsign(call, submode);
    }
}

void UI_Constructor::on_tableWidgetCalls_cellDoubleClicked(int row, int col) {
    on_tableWidgetCalls_cellClicked(row, col);

    auto call = callsignSelected();
    ui->extFreeTextMsgEdit->clear();
    addMessageText(call);

#if SHOW_MESSAGE_HISTORY_ON_DOUBLECLICK
    if (m_rxInboxCountCache.value(call, 0) > 0) {

        // TODO:
        // CommandDetail d = m_rxCallsignInboxCountCache[call].first();
        // m_rxCallsignInboxCountCache[call].removeFirst();
        //
        // processAlertReplyForCommand(d, d.relayPath, d.cmd);

        Inbox i(inboxPath());
        if (i.open()) {
            QList<Message> msgs;
            foreach (auto pair,
                     i.values("UNREAD", "$.params.FROM", call, 0, 1000)) {
                msgs.append(pair.second);
            }

            auto mp = new MessagePanel(this);
            mp->populateMessages(msgs);
            mp->show();

            ensureMessageDock();

            messageDock_->show();
            messageDock_->raise();

            auto pair = i.firstUnreadFrom(call);
            auto id = pair.first;
            auto msg = pair.second;
            auto params = msg.params();

            CommandDetail d;
            d.cmd = params.value("CMD").toString();
            d.extra = params.value("EXTRA").toString();
            d.freq = params.value("OFFSET").toInt();
            d.from = params.value("FROM").toString();
            d.grid = params.value("GRID").toString();
            d.relayPath = params.value("PATH").toString();
            d.snr = params.value("SNR").toInt();
            d.tdrift = params.value("TDRIFT").toFloat();
            d.text = params.value("TEXT").toString();
            d.to = params.value("TO").toString();
            d.utcTimestamp = QDateTime::fromString(
                params.value("UTC").toString(), "yyyy-MM-dd hh:mm:ss");
            d.utcTimestamp.setUtcOffset(0);

            msg.setType("READ");
            i.set(id, msg);

            m_rxInboxCountCache[call] =
                max(0, m_rxInboxCountCache.value(call) - 1);

            processAlertReplyForCommand(d, d.relayPath, d.cmd);
        }

    } else {
        addMessageText(call);
    }
#endif
}

void UI_Constructor::on_tuneButton_clicked(bool checked) {
    static bool lastChecked = false;
    if (lastChecked == checked)
        return;
    lastChecked = checked;
    if (checked && m_tune == false) { // we're starting tuning so remember Tx
                                      // and change pwr to Tune value
        if (m_config.pwrBandTuneMemory()) {
            m_pwrBandTxMemory[m_lastBand] =
                ui->outAttenuation->value(); // remember our Tx pwr
            m_PwrBandSetOK = false;
            if (m_pwrBandTuneMemory.contains(m_lastBand)) {
                ui->outAttenuation->setValue(
                    m_pwrBandTuneMemory[m_lastBand].toInt()); // set to Tune pwr
            }
            m_PwrBandSetOK = true;
        }
    }
    if (m_tune) {
        tuneButtonTimer.start(250);
    } else {
        itone[0] = 0;
        on_monitorButton_clicked(true);
        m_tune = true;
    }
    Q_EMIT tune(checked);
}

void UI_Constructor::end_tuning() {
    tuneATU_Timer.stop(); // stop tune watchdog when stopping Tune manually
    on_stopTxButton_clicked();
    // we're turning off so remember our Tune pwr setting and reset to Tx pwr
    if (m_config.pwrBandTuneMemory() || m_config.pwrBandTxMemory()) {
        m_pwrBandTuneMemory[m_lastBand] =
            ui->outAttenuation->value(); // remember our Tune pwr
        m_PwrBandSetOK = false;
        ui->outAttenuation->setValue(
            m_pwrBandTxMemory[m_lastBand].toInt()); // set to Tx pwr
        m_PwrBandSetOK = true;
    }
}

void UI_Constructor::stop_tuning() {
    tuneATU_Timer.stop(); // stop tune watchdog when stopping Tune manually
    on_tuneButton_clicked(false);
    ui->tuneButton->setChecked(false);
    m_isTimeToSend = false;
    m_tune = false;
}

void UI_Constructor::stopTuneATU() {
    on_tuneButton_clicked(false);
    m_isTimeToSend = false;
}

void UI_Constructor::resetPushButtonToggleText(QPushButton *btn) {
    bool checked = btn->isChecked();
    auto style = btn->styleSheet();
    if (checked) {
        style = style.replace("font-weight:normal;", "font-weight:bold;");
    } else {
        style = style.replace("font-weight:bold;", "font-weight:normal;");
    }
    btn->setStyleSheet(style);

#if PUSH_BUTTON_CHECKMARK
    auto on = "✓ ";
    auto text = btn->text();
    if (checked) {
        btn->setText(on + text.replace(on, ""));
    } else {
        btn->setText(text.replace(on, ""));
    }
#endif

#if PUSH_BUTTON_MIN_WIDTH
    int width = 0;
    QList<QPushButton *> btns;
    foreach (auto child, ui->buttonGrid->children()) {
        if (!child->isWidgetType()) {
            continue;
        }

        if (!child->objectName().contains("Button")) {
            continue;
        }

        auto b = qobject_cast<QPushButton *>(child);
        width = qMax(width, b->geometry().width());
        btns.append(b);
    }

    foreach (auto child, btns) {
        child->setMinimumWidth(width);
    }
#endif
}

void UI_Constructor::on_stopTxButton_clicked() // Stop Tx
{
    if (m_tune)
        stop_tuning();
    if (m_auto and !m_tuneup)
        auto_tx_mode(false);
    m_btxok = false;

    resetMessage();

    // Operator-initiated halt aborts any in-flight chunked-ARQ
    // session and clears all per-peer state (incl. MSG-cmd flags).
    // Done after resetMessage so the TX queue is already empty when
    // Manager fires its sendFailed("halted") for each pending send.
    //
    // GATED on m_stopTxButtonIsLongterm: this slot is reached by THREE
    // different paths, only one of which is the operator pressing Halt:
    //   (a) Halt button click — m_stopTxButtonIsLongterm = true (default)
    //   (b) stopTx() after a TX frame completes — toggles longterm to
    //       false before calling us (mainwindow.cpp:3278), restores after
    //   (c) auto_tx_mode(false) → us — same toggle pattern (line 1230-33)
    // Paths (b) and (c) are routine inter-chunk cleanup; killing the ARQ
    // session there means chunk 1 finishes, we tear down state, the ACK
    // arrives a few seconds later, and onAckReceived finds no SendState
    // to advance — the message stalls at one chunk forever.
    if (m_chunkedArq && m_stopTxButtonIsLongterm) {
        m_chunkedArq->haltAll();
    }

    if (m_stopTxButtonIsLongterm) {
        if (m_hb_loop->isActive())
            qWarning() << "[HAIL-DIAG] loop cancelled: stop button (longterm)";
        m_hb_loop->onLoopCancel();
        m_cq_loop->onLoopCancel();
    }
}

void UI_Constructor::rigOpen() {
    update_dynamic_property(ui->readFreq, "state", "warning");
    ui->readFreq->setText("CAT");
    ui->readFreq->setEnabled(true);
    m_config.transceiver_online();
    Q_EMIT m_config.sync_transceiver(true, true);
}

void UI_Constructor::on_readFreq_clicked() {
    if (m_transmitting)
        return;

    if (m_config.transceiver_online()) {
        Q_EMIT m_config.sync_transceiver(true, true);
    }
}

void UI_Constructor::setXIT(int audio_freq) {
    if (m_transmitting && !m_config.tx_qsy_allowed()) {
        qCWarning(mainwindow_js8) << "Ignoring change of audio freq to"
                                  << audio_freq << "as currently transmitting.";
        return;
    }

    // m_XIT is the frequency diff that will be added to the audio frequency
    // and subtracted from the radio frequency.
    // The new audio frequency is in the 1500 - 2000 Hz range,
    // the audio actually transmitted is 1500 - 2160 Hz (TURBO has 160 Hz
    // bandwith). This way, the unwanted triple audio frequency possibly
    // generated by audio distortions is safely beyond the TX audio bandwidth of
    // 3 kHz and will not result in transmission. Also, the 1500 - 2160 Hz range
    // should not be distorted or dampened by the TX's audio filter.
    if (m_config.split_mode()) {
        const int next_lower_multiple_of_500 = audio_freq - audio_freq % 500;
        m_XIT = 1500 - next_lower_multiple_of_500;
    } else {
        m_XIT = 0;
    }

    const int new_audio_frequency = audio_freq + m_XIT;

    if ((m_monitoring || m_transmitting) && m_config.is_transceiver_online() &&
        m_config.split_mode()) {
        // All conditions are met, reset the transceiver Tx dial frequency
        m_freqTxNominal = m_freqNominal - m_XIT;
        qCDebug(mainwindow_js8)
            << "For incoming AF" << audio_freq << "setting tx HF to"
            << m_freqTxNominal << "and new AF to" << new_audio_frequency;
        Q_EMIT m_config.transceiver_tx_frequency(m_freqTxNominal);
    }

    // Now set the audio Tx freq
    Q_EMIT transmitFrequency(new_audio_frequency);
}

void UI_Constructor::qsy(int const hzDelta) {
    setRig(m_freqNominal + hzDelta);
    setFreqOffsetForRestore(m_wideGraph->centerFreq(), false);

    // Adjust band activity frequencies.

    BandActivity bandActivity;

    for (auto [key, value] : m_bandActivity.asKeyValueRange()) {
        if (value.isEmpty())
            continue;

        auto const newKey = key - hzDelta;

        bandActivity[newKey] = value;
        bandActivity[newKey].last().offset -= hzDelta;
    }

    m_bandActivity.swap(bandActivity);

    // Adjust call activity frequencies.

    for (auto [key, value] : m_callActivity.asKeyValueRange()) {
        value.offset -= hzDelta;
    }

    displayActivity(true);
}

void UI_Constructor::onDriftChanged(qint64 /*new_drift_ms*/) {
    // here we reset the buffer position without clearing the buffer
    // this makes the detected emit the correct k when drifting time
    qCDebug(mainwindow_js8) << "Processing drift change.";
    m_detector->resetBufferPosition();
}

void UI_Constructor::setFreqOffsetForRestore(int freq, bool shouldRestore) {
    changeFreq(freq);
    if (shouldRestore) {
        m_shouldRestoreFreq = true;
    } else {
        m_previousFreq = 0;
        m_shouldRestoreFreq = false;
    }
}

bool UI_Constructor::tryRestoreFreqOffset() {
    if (!m_shouldRestoreFreq || m_previousFreq == 0) {
        return false;
    }

    setFreqOffsetForRestore(m_previousFreq, false);
    return true;
}

void UI_Constructor::changeFreq(int const newFreq) {
    // Don't allow QSY if we've already queued a transmission,
    // unless we have that functionality enabled.

    if (isMessageQueuedForTransmit() && !m_config.tx_qsy_allowed())
        return;

    // TODO: jsherer - here's where we'd set minimum frequency again (later?)

    m_previousFreq = freq();
    setFreq(std::max(0, newFreq));

    displayDialFrequency();
}

void UI_Constructor::handle_transceiver_update(
    Transceiver::TransceiverState const &new_rig_state) {
    qCDebug(mainwindow_js8)
        << "UI_Constructor::handle_transceiver_update:" << new_rig_state;
    Transceiver::TransceiverState old_state{m_rigState};

    // GM8JCF: in stopTx2 we maintain PTT if there are still untransmitted JS8
    // frames and we are holding the PTT KN4CRD: if we're not holding the PTT we
    // need to check to ensure it's safe to transmit
    if (m_config.hold_ptt() ||
        (new_rig_state.ptt() &&
         !m_rigState
              .ptt())) // safe to start audio (caveat - DX Lab Suite Commander)
    {
        if (m_generateAudioWhenPttConfirmedByTX &&
            m_iptt) // waiting to Tx and still needed
        {
            qWarning() << "[FT2-TX] PTT confirmed, calling transmit()"
                        << "m_iptt=" << m_iptt;
            transmit();
        } else {
            if (m_nSubMode == Varicode::JS8CallFT2)
                qCDebug(mainwindow_js8) << "[FT2-TX] PTT update but NOT transmitting:"
                            << "genAudio=" << m_generateAudioWhenPttConfirmedByTX
                            << "m_iptt=" << m_iptt;
        }
        m_generateAudioWhenPttConfirmedByTX = false;
    } else {
        if (m_nSubMode == Varicode::JS8CallFT2 && new_rig_state.ptt())
            qWarning() << "[FT2-TX] PTT update SKIPPED: hold_ptt="
                        << m_config.hold_ptt()
                        << "newPtt=" << new_rig_state.ptt()
                        << "oldPtt=" << m_rigState.ptt();
    }
    m_rigState = new_rig_state;

    auto old_freqNominal = m_freqNominal;
    if (!old_freqNominal) {
        // always take initial rig frequency to avoid start up problems
        // with bogus Tx frequencies
        m_freqNominal = new_rig_state.frequency();
    }

    if (old_state.online() == false && new_rig_state.online() == true) {
        // initializing
        on_monitorButton_clicked(!m_config.monitor_off_at_startup());
        on_monitorTxButton_toggled(!m_config.transmit_off_at_startup());
    }

    if (new_rig_state.frequency() != old_state.frequency() ||
        new_rig_state.split() != m_splitMode) {
        m_splitMode = new_rig_state.split();
        if (!new_rig_state.ptt()) {
            m_freqNominal = new_rig_state.frequency();
            if (old_freqNominal != m_freqNominal) {
                m_freqTxNominal = m_freqNominal;
            }

            if (m_monitoring) {
                m_lastMonitoredFrequency = m_freqNominal;
            }
            if (m_lastDialFreq != m_freqNominal) {

                m_lastDialFreq = m_freqNominal;
                m_secBandChanged =
                    DriftingDateTime::currentMSecsSinceEpoch() / 1000;

                if (m_freqNominal != m_bandHoppedFreq) {
                    m_bandHopped = false;
                }

                if (new_rig_state.frequency() < 30000000u) {
                    write_frequency_entry("ALL.TXT");
                }

                if (m_config.spot_to_reporting_networks()) {
                    spotSetLocal();
                    pskSetLocal();
                    if (m_config.spot_to_aprs() ||
                        m_config.spot_to_aprs_relay()) {
                        aprsSetLocal();
                    }
                }
                statusChanged();
                m_wideGraph->setDialFreq(m_freqNominal / 1.e6f);
            }
        } else {
            m_freqTxNominal = new_rig_state.split()
                                  ? new_rig_state.tx_frequency()
                                  : new_rig_state.frequency();
        }
    }

    // ensure frequency display is correct
    // setRig();
    updateCurrentBand();
    displayDialFrequency();
    update_dynamic_property(ui->readFreq, "state", "ok");
    ui->readFreq->setEnabled(false);
    ui->readFreq->setText(new_rig_state.split() ? "CAT/S" : "CAT");
}

void UI_Constructor::handle_transceiver_failure(QString const &reason) {
    update_dynamic_property(ui->readFreq, "state", "error");
    ui->readFreq->setEnabled(true);
    on_stopTxButton_clicked();
    rigFailure(reason);
}

void UI_Constructor::rigFailure(QString const &reason) {
    if (m_first_error) {
        // one automatic retry
        QTimer::singleShot(0, this, &UI_Constructor::rigOpen);
        m_first_error = false;
    } else {
        m_rigErrorMessageBox.setDetailedText(reason);

        // don't call slot functions directly to avoid recursion
        m_rigErrorMessageBox.exec();
        auto const clicked_button = m_rigErrorMessageBox.clickedButton();
        if (clicked_button == m_configurations_button) {
            ui->menuConfig->exec(QCursor::pos());
        } else {
            switch (m_rigErrorMessageBox.standardButton(clicked_button)) {
            case JS8MessageBox::Ok:
                m_config.select_tab(1);
                QTimer::singleShot(
                    0, this, &UI_Constructor::on_actionSettings_triggered);
                break;

            case JS8MessageBox::Retry:
                QTimer::singleShot(0, this, &UI_Constructor::rigOpen);
                break;

            case JS8MessageBox::Cancel:
                QTimer::singleShot(0, this, &UI_Constructor::close);
                break;

            default:
                break; // squashing compile warnings
            }
        }
        m_first_error = true; // reset
    }
}

void UI_Constructor::on_outAttenuation_valueChanged(int const a) {
    if (m_PwrBandSetOK) {
        if (!m_tune && m_config.pwrBandTxMemory())
            m_pwrBandTxMemory[m_lastBand] = a; // remember our Tx pwr
        if (m_tune && m_config.pwrBandTuneMemory())
            m_pwrBandTuneMemory[m_lastBand] = a; // remember our Tune pwr
    }

    // Slider interpreted as dB / 100.

    Q_EMIT outAttenuationChanged(a / 10.0);
}

void UI_Constructor::spotSetLocal() {
    Q_EMIT spotClientSetLocalStation(
        m_config.my_callsign(), m_config.my_grid(),
        replaceMacros(m_config.my_info(), buildMacroValues(), true));
}

void UI_Constructor::pskSetLocal() {
    Q_EMIT pskReporterSetLocalStation(
        m_config.my_callsign(), m_config.my_grid(),
        replaceMacros(m_config.my_info(), buildMacroValues(), true));
}

void UI_Constructor::aprsSetLocal() {
    Q_EMIT aprsClientSetLocalStation(
        "APJ8CL", QString::number(APRSISClient::hashCallsign("APJ8CL")));
}

void UI_Constructor::transmitDisplay(bool transmitting) {
    if (transmitting == m_transmitting) {
        if (transmitting) {
            ui->signal_meter_widget->setValue(0, 0);
            // [MONITOR-DURING-TX 2026-06-10 build 236]
            // Subspace (FT2) mode: keep monitor ON during our own TX
            // so the audio capture pipeline doesn't go through a
            // wake-up transient at stopTx → monitor-on transition.
            // Incoming ACKs arriving right after our chunk's TX-end
            // would otherwise lose their leading Costas sync samples
            // during the wake-up window (ARQ-style back-to-back
            // TX→RX timing exposes that gap).
            //
            // Legacy modes (Normal/Fast/Turbo/Slow): keep the
            // original behaviour — pause audio capture during own TX.
            // These modes have 15-30 s period cycles with the next
            // RX expected at the next period boundary; the wake-up
            // window after TX-end is negligible compared to the gap
            // before the next RX. Operator observation 2026-06-10:
            // keeping monitor on during TX is "not necessary or
            // desirable" for legacy modes.
            // [MONITOR-DURING-TX 2026-06-10 build 238]
            // Operator reported legacy-speed TXes "still leaving
            // recv on" after build 236's submode gate was added.
            // Log every TX-start so we can verify in the diag log
            // whether this branch is being reached and what the
            // submode value is at that moment. Two scenarios to
            // distinguish: (a) submode is somehow FT2 even when
            // operator is in a legacy mode (UI/state desync), or
            // (b) monitor pause is firing but something is re-
            // enabling capture elsewhere.
            if (m_nSubMode != Varicode::JS8CallFT2) {
                qWarning() << "[MONITOR] legacy-mode TX: pausing audio"
                              " capture; submode=" << m_nSubMode
                           << "m_monitoring=" << m_monitoring;
                if (m_monitoring)
                    monitor(false);
            } else {
                qWarning() << "[MONITOR] FT2-mode TX: keeping audio"
                              " capture ON; m_monitoring=" << m_monitoring;
            }
            m_btxok = true;
        }
    }

    updateTxButtonDisplay();
}

void UI_Constructor::postDecode(bool is_new, QString const &) {
#if 0
  auto const& decode = message.trimmed ();
  auto const& parts = decode.left (22).split (' ', QString::SkipEmptyParts);
  if (parts.size () >= 5)
  {
      auto has_seconds = parts[0].size () > 4;
      m_messageClient->decode (is_new
                               , QTime::fromString (parts[0], has_seconds ? "hhmmss" : "hhmm")
                               , parts[1].toInt ()
                               , parts[2].toFloat (), parts[3].toUInt (), parts[4]
                               , decode.mid (has_seconds ? 24 : 22, 21)
                               , QChar {'?'} == decode.mid (has_seconds ? 24 + 21 : 22 + 21, 1)
                               , m_diskData);
  }
#endif

    if (is_new) {
        m_rxDirty = true;
    }
}

void UI_Constructor::tryNotify(QString const &key, int submode) {
    // The subspace-only checkbox filters *real decodes* by mode: when it's
    // set, we only play for FT2 submode. The test button in
    // Configuration > Notifications invokes tryNotify(key) with the default
    // submode=-1 ("no specific mode"), which previously made checked rows
    // never play from the test button. Treat submode=-1 as "not a
    // real-decode event" and bypass the filter so the test button always
    // previews the sound regardless of the checkbox.
    if (submode != -1 && m_config.notification_requires_subspace(key) &&
        submode != Varicode::JS8CallFT2) {
        return;
    }
    if (auto const path = m_config.notification_path(key); !path.isEmpty()) {
        emit playNotification(path);
    }
}

void UI_Constructor::displayTransmit() {
    // Transmit Activity
    update_dynamic_property(ui->startTxButton, "transmitting", m_transmitting);
    update_dynamic_property(ui->monitorTxButton, "transmitting",
                            m_transmitting);
}

bool UI_Constructor::presentlyWantHBReplies() {
    return canCurrentModeAckHeartbeat() &&
           ui->actionModeAutoreply->isChecked() &&
           ui->actionHeartbeatAcknowledgements->isChecked() &&
           m_messageBuffer.isEmpty() &&
           (!m_config.heartbeat_qso_pause() ||
            m_selectedCallsign.isEmpty());
}

void UI_Constructor::updateModeButtonText() {
    auto multi = ui->actionModeMultiDecoder->isChecked();
    auto autoreply = ui->actionModeAutoreply->isChecked();
    auto heartbeat =
        ui->actionModeJS8HB->isEnabled() && ui->actionModeJS8HB->isChecked();

    auto modeText = JS8::Submode::name(m_nSubMode);
    if (multi) {
        modeText += QString("+MULTI");
    }

    if (autoreply) {
        if (m_config.autoreply_confirmation()) {
            modeText += QString("+AUTO+CONF");
        } else {
            modeText += QString("+AUTO");
        }
    }

    if (heartbeat) {
        // FT2/Subspace renames the HB cadence to "HAIL" — same wire
        // semantics (presence beacon) but matches the button label
        // the operator already sees in Subspace mode and the
        // "Enable HAIL Presence Beacon" menu wording at line 4665-4668.
        // HAIL is a non-reply protocol (presence broadcast, no ACK
        // expected), so we never decorate it with "+ACK" even when
        // presentlyWantHBReplies() is true — that ACK toggle only
        // affects classic HB cadence in Normal/Fast/Turbo/Slow.
        bool const isHail = (m_nSubMode == Varicode::JS8CallFT2);
        if (isHail) {
            modeText += QStringLiteral("+HAIL");
        } else if (presentlyWantHBReplies()) {
            modeText += QStringLiteral("+HB+ACK");
        } else {
            modeText += QStringLiteral("+HB");
        }
    }

    // Auto Repeat Request (ARQ) status — drives ChunkedArq routing in
    // any submode; visible here so the operator sees it alongside the
    // other mode flags (+MULTI, +AUTO, +HAIL/+HB). The toggle action
    // keeps its legacy internal name `actionModeReplicatorProtocol`
    // to preserve QSettings keys and Qt auto-slot-connection; the
    // operator-visible text is "Enable Auto Repeat Request (ARQ)".
    if (ui->actionModeReplicatorProtocol &&
        ui->actionModeReplicatorProtocol->isChecked()) {
        modeText += QStringLiteral("+ARQ");
    }

    ui->modeButton->setText(modeText);
}

// Helper: only set disabled if state actually changes (avoids flicker)
static void setDisabledIfChanged(QWidget *w, bool disabled) {
    if (w->isEnabled() == disabled)  // enabled == !disabled, so if equal, state differs
        w->setDisabled(disabled);
}

// [TODO.md #67 build 272] Live ARQ-gate evaluator. Mirrors the full
// TX-time gate logic at on_startTxButton_clicked (this file, around
// lines 3485-3629) so the ARQ button's armed visual matches what
// Send will actually do — not just whether a Call Activity row is
// picked. Const because it only reads widget / config / chunked-arq
// state. Called from updateButtonDisplay() and indirectly from the
// 100 ms text-debounce path. Keep this function and the TX-time
// block in sync as a pair; if either drifts, the button visual
// stops predicting actual behavior.
UI_Constructor::ArqGateState
UI_Constructor::evaluateArqGateForText(QString const &text) const {
    QString arqPeer = const_cast<UI_Constructor *>(this)
                          ->callsignSelected().trimmed();
    if (arqPeer.isEmpty()) {
        QString const myCall = m_config.my_callsign().trimmed().toUpper();
        QString probe = text.toUpper().trimmed();
        if (!myCall.isEmpty() && probe.startsWith(myCall + ":")) {
            probe = probe.mid(myCall.length() + 1).trimmed();
        }
        static QRegularExpression const leadingCallRe(
            QStringLiteral(R"(^\s*(@?[A-Z0-9/]+))"));
        auto const m = leadingCallRe.match(probe);
        if (m.hasMatch()) {
            arqPeer = m.captured(1);
        }
    }
    bool const arqHasPeer = !arqPeer.isEmpty() &&
                            !arqPeer.startsWith('@') &&
                            Radio::is_callsign(arqPeer);
    bool const arqHasText = !text.trimmed().isEmpty();
    if (!arqHasPeer) {
        return ArqGateState::NotArmed_NoPeer;
    }
    // [BUILD 287] Empty outgoing box + valid peer = Armed. Earlier
    // builds (272+) required non-empty text here too, which broke
    // "select a call sign and the ARQ button immediately reflects
    // readiness." Empty text is the legitimate starting state for a
    // file send (operator picks a peer, then clicks the chevron's
    // Send file… action — the file body never goes through the
    // outgoing widget) and for any operator who selects a peer
    // BEFORE composing. The downstream directed-cmd check only
    // applies when there's actual text to inspect.
    if (!arqHasText) {
        return ArqGateState::Armed;
    }

    QString const myCallUp = m_config.my_callsign().trimmed().toUpper();
    QString probe = text.trimmed();
    if (!myCallUp.isEmpty() &&
        probe.toUpper().startsWith(myCallUp + ":")) {
        probe = probe.mid(myCallUp.length() + 1).trimmed();
    }
    QString dirTo, dirCmd, dirNum;
    bool dirToCompound = false;
    int dirN = 0;
    QString dirFrame = Varicode::packDirectedMessage(
        probe, myCallUp, &dirTo, &dirToCompound, &dirCmd, &dirNum, &dirN);
    if (dirFrame.isEmpty()) {
        QString const triedProbeWithPeer =
            arqPeer + QStringLiteral(" ") + probe;
        dirFrame = Varicode::packDirectedMessage(
            triedProbeWithPeer, myCallUp, &dirTo, &dirToCompound,
            &dirCmd, &dirNum, &dirN);
    }
    QString const cmdTrimmed = dirCmd.trimmed();
    bool const isFreetextCmd = cmdTrimmed.isEmpty();
    bool const isMsgCmd =
        (cmdTrimmed.compare(QStringLiteral("MSG"),
                            Qt::CaseInsensitive) == 0) ||
        (cmdTrimmed.compare(QStringLiteral("MSG TO:"),
                            Qt::CaseInsensitive) == 0);
    bool const isRelayCmd = (cmdTrimmed == QStringLiteral(">"));
    bool arqBodyIsDirectedCmd =
        !dirFrame.isEmpty() && !isFreetextCmd &&
        !isMsgCmd && !isRelayCmd;

    // TYPING macro — not in directed_cmds so packDirectedMessage can't
    // catch it, but must not arm ARQ.
    if (!arqBodyIsDirectedCmd) {
        QString const upperProbe = probe.toUpper();
        QString bodyAfterPeer = upperProbe;
        QString const peerPrefix = arqPeer.toUpper() + QStringLiteral(" ");
        if (upperProbe.startsWith(peerPrefix)) {
            bodyAfterPeer = upperProbe.mid(peerPrefix.length()).trimmed();
        }
        if (bodyAfterPeer.startsWith(QStringLiteral("TYPING"))) {
            arqBodyIsDirectedCmd = true;
        }
    }

    return arqBodyIsDirectedCmd ? ArqGateState::NotArmed_DirectedCmd
                                : ArqGateState::Armed;
}

void UI_Constructor::updateButtonDisplay() {
    // Treat an in-flight chunked-ARQ super-message as "transmitting"
    // for ALL the macro-button gates below. Between chunks
    // m_transmitting drops momentarily and isMessageQueuedForTransmit
    // returns false, which re-enabled REPLY / SNR / INFO / STATUS /
    // TYPING / HB / CQ / macros / query / deselect after each sub-
    // message (operator observed 2026-06-08, arq-uiLockSetup follow-up).
    // Baking arqBusy into the local isTransmitting flag holds them
    // all disabled for the entire ARQ session in one stroke without
    // touching each setDisabledIfChanged call.
    // [RX-SIDE NO-LOCK 2026-06-10] hasActiveTxSession() — macro
    // buttons stay usable on the receiver side mid-RX (operator can
    // type STATUS / INFO / HB / etc. while chunks arrive from a peer).
    bool isTransmitting = isMessageQueuedForTransmit() ||
                          (m_chunkedArq && m_chunkedArq->hasActiveTxSession());

    auto selectedCallsign = callsignSelected(true);
    bool emptyCallsign = selectedCallsign.isEmpty();
    bool emptyInfo = m_config.my_info().isEmpty();
    bool emptyStatus = m_config.my_status().isEmpty();

    bool previous_hbButtonisLongterm = m_hbButtonIsLongterm;
    m_hbButtonIsLongterm = false;
    setDisabledIfChanged(ui->hbMacroButton, isTransmitting || !m_hbModeAvailable);
    m_hbButtonIsLongterm = previous_hbButtonisLongterm;

    bool previous_cqButtonisLongterm = m_cqButtonIsLongterm;
    m_cqButtonIsLongterm = false;
    setDisabledIfChanged(ui->cqMacroButton, isTransmitting);
    m_cqButtonIsLongterm = previous_cqButtonisLongterm;

    setDisabledIfChanged(ui->replyMacroButton, isTransmitting || emptyCallsign);
    setDisabledIfChanged(ui->snrMacroButton, isTransmitting || emptyCallsign);
    setDisabledIfChanged(ui->infoMacroButton, isTransmitting || emptyInfo);
    setDisabledIfChanged(ui->statusMacroButton, isTransmitting || emptyStatus);

    // [BUILD 298] "Send using ARQ" menu action — live enable/disable.
    // Replaces the prior m_arqButton armed-visual (no more blue
    // styled state; just enabled vs disabled). The action is
    // enabled exactly when an ARQ send would actually succeed: peer
    // resolvable, text non-empty, text not a directed cmd that ARQ
    // can't wrap (MSG / MSG TO: / relay > and freetext are armed;
    // SNR? / GRID? / standard directed cmds are not). Driven by the
    // same evaluateArqGateForText helper that prior arming used.
    if (m_sendArqAction) {
        QString const txText = ui->extFreeTextMsgEdit
            ? ui->extFreeTextMsgEdit->toPlainText() : QString();
        ArqGateState const gate = evaluateArqGateForText(txText);
        // updateButtonDisplay scope doesn't have `canTransmit` —
        // `isTransmitting` + `emptyCallsign` are the relevant local
        // gates. Gate::Armed already requires a valid peer, so
        // emptyCallsign is implicitly covered by the gate result.
        bool const canSendArq = (gate == ArqGateState::Armed) &&
                                !isTransmitting;
        m_sendArqAction->setEnabled(canSendArq);
    }
    {
        // TYPING enabled if: Subspace mode, not transmitting, and either
        // a callsign is selected OR cursor is on the last decoded signal freq
        bool onPartnerFreq = false;
        if (emptyCallsign && m_nSubMode == Varicode::JS8CallFT2) {
            int myOffset = freq();
            int threshold = JS8::Submode::rxThreshold(m_nSubMode);
            for (auto const &cd : m_callActivity) {
                if (abs(cd.offset - myOffset) <= threshold) {
                    onPartnerFreq = true;
                    break;
                }
            }
        }
        setDisabledIfChanged(ui->typingMacroButton,
            isTransmitting || m_nSubMode != Varicode::JS8CallFT2
            || (emptyCallsign && !onPartnerFreq));
    }
    setDisabledIfChanged(ui->macrosMacroButton, isTransmitting);
    setDisabledIfChanged(ui->queryButton, isTransmitting || emptyCallsign);
    setDisabledIfChanged(ui->deselectButton, isTransmitting || emptyCallsign);

    auto directedText = emptyCallsign ? "Directed"
                      : QString("Directed to %1").arg(selectedCallsign);
    if (ui->queryButton->text() != directedText) {
        ui->queryButton->setText(directedText);
        auto fmDir = ui->queryButton->fontMetrics();
        // Extra "m" of padding: the button has a dropdown arrow on the
        // right side whose pixel width isn't counted in
        // horizontalAdvance(text). Without this the arrow gets clipped
        // when in directed mode with a selected callsign.
        ui->queryButton->setMinimumWidth(fmDir.horizontalAdvance(directedText)
                                        + fmDir.horizontalAdvance("m") + 12);
    }

    // update mode button text
    updateModeButtonText();
}

void UI_Constructor::updateHBButtonDisplay() {
    if (m_hb_loop->isActive()) {
        QDateTime now = DriftingDateTime::currentDateTimeUtc();
        QDateTime nextHeartbeat = m_hb_loop->nextActivity();
        long secs = std::lround(now.msecsTo(nextHeartbeat) / 1000.0);

        bool isHail = (m_nSubMode == Varicode::JS8CallFT2);
        QString hbBase = isHail ? "HAIL"
            : (presentlyWantHBReplies() ? "HB + ACK" : "HB");

        if (secs > 0) {
            ui->hbMacroButton->setText(
                QString("%1 (%2)").arg(hbBase).arg(secs));
        } else {
            ui->hbMacroButton->setText(QString("%1 (now)").arg(hbBase));
        }
    } else {
        if (m_nSubMode == Varicode::JS8CallFT2) {
            ui->hbMacroButton->setText("HAIL");
        } else if (presentlyWantHBReplies()) {
            ui->hbMacroButton->setText("HB + ACK");
        } else {
            ui->hbMacroButton->setText("HB");
        }
    }
    // Adjust minimum width to fit current text
    auto fm = ui->hbMacroButton->fontMetrics();
    ui->hbMacroButton->setMinimumWidth(fm.horizontalAdvance(ui->hbMacroButton->text()) + 12);

    ui->hbMacroButton->setToolTip(
        m_nSubMode == Varicode::JS8CallFT2
        ? "Send hailing message (1 or 2 frames, configurable, no reply)"
        : "Send heartbeat with grid square");
}

void UI_Constructor::updateCQButtonDisplay() {
    if (m_cq_loop->isActive()) {
        QDateTime now = DriftingDateTime::currentDateTimeUtc();
        QDateTime nextCQ = m_cq_loop->nextActivity();
        long secs = std::lround(now.msecsTo(nextCQ) / 1000.0);
        // qCDebug(mainwindow_js8)
        //         << "updateCQButtonDisplay, signal due at" << nextCQ
        //         << "so" << secs << "s to go";
        if (secs > 0) {
            ui->cqMacroButton->setText(QString("CQ (%1)").arg(secs));
        } else {
            // Dead code?
            ui->cqMacroButton->setText("CQ (now)");
        }
    } else {
        ui->cqMacroButton->setText("CQ");
        // qCDebug(mainwindow_js8) << "updateCQButtonDisplay while m_cq_loop is
        // off";
    }
}

void UI_Constructor::updateTextDisplay() {
    bool canTransmit = ensureCanTransmit();
    bool isTransmitting = isMessageQueuedForTransmit();
    bool emptyText = ui->extFreeTextMsgEdit->toPlainText().isEmpty();

    // Disable Send when nothing to send (only update if state changed to avoid flash)
    setDisabledIfChanged(ui->startTxButton, !canTransmit || isTransmitting || emptyText);
    // [FILE-XFER build 283] Chevron button stays enabled full-time
    // for discoverability — the operator can always open the menu
    // and see what send-options exist. Gating moves to the menu
    // *action*. We intentionally drop the emptyText / frame-count
    // gates that apply to Send: file send doesn't read from the
    // outgoing widget, so an empty box is a legitimate starting
    // state. The remaining gates (canTransmit, !isTransmitting)
    // cover "callsign selected" + "not currently TXing".
    if (m_sendFileAction) {
        m_sendFileAction->setEnabled(canTransmit && !isTransmitting);
    }
    // Chevron tooltip reflects current selection state so the
    // operator sees, without opening the menu, why file send is or
    // isn't ready. Cheap string compare avoids needless repaint.
    if (m_sendMenuButton) {
        bool const haveCall = !callsignSelected().trimmed().isEmpty();
        QString const tip = haveCall
            ? QStringLiteral("Send options: send using ARQ, send a file")
            : QStringLiteral("Send options: send using ARQ, send a file (select call sign first)");
        if (m_sendMenuButton->toolTip() != tip) {
            m_sendMenuButton->setToolTip(tip);
        }
    }

    if (m_txTextDirty) {
        // debounce frame and word count
        if (m_txTextDirtyDebounce.isActive()) {
            m_txTextDirtyDebounce.stop();
        }
        m_txTextDirtyDebounce.setSingleShot(true);
        m_txTextDirtyDebounce.start(100);
        m_txTextDirty = false;
    }
}

#if __APPLE__
#define USE_SYNC_FRAME_COUNT 0
#else
#define USE_SYNC_FRAME_COUNT 0
#endif

void UI_Constructor::refreshTextDisplay() {
    qCDebug(mainwindow_js8) << "refreshing text display...";
    auto text = ui->extFreeTextMsgEdit->toPlainText();

#if USE_SYNC_FRAME_COUNT
    auto frames = buildMessageFrames(text);

    QStringList textList;
    qCDebug(mainwindow_js8) << "frames:";
    foreach (auto frame, frames) {
        auto dt = DecodedText(frame.first, frame.second);
        qCDebug(mainwindow_js8) << "->" << frame << dt.message()
                                << Varicode::frameTypeString(dt.frameType());
        textList.append(dt.message());
    }

    auto transmitText = textList.join("");
    auto count = frames.length();

    // ugh...i hate these globals
    m_txTextDirtyLastSelectedCall = callsignSelected(true);
    m_txTextDirtyLastText = text;
    m_txFrameCountEstimate = count;
    m_txTextDirty = false;

    updateTextWordCheckerDisplay();
    updateTextStatsDisplay(transmitText, count);
    updateTxButtonDisplay();

#else
    // prepare selected callsign for directed message
    QString selectedCall = callsignSelected();

    // prepare compound
    QString mycall = m_config.my_callsign();
    QString mygrid = m_config.my_grid().left(4);
    bool forceIdentify = !m_config.avoid_forced_identify();
    bool forceData = false;

    BuildMessageFramesThread *t =
        new BuildMessageFramesThread(mycall, mygrid, selectedCall, text,
                                     forceIdentify, forceData, m_nSubMode);

    connect(t, &BuildMessageFramesThread::finished, t, &QObject::deleteLater);
    connect(t, &BuildMessageFramesThread::resultReady, this,
            [this, text](QString transmitText, int frames) {
                // ugh...i hate these globals
                m_txTextDirtyLastSelectedCall = callsignSelected(true);
                m_txTextDirtyLastText = text;
                m_txFrameCountEstimate = frames;
                m_txTextDirty = false;

                updateTextWordCheckerDisplay();
                updateTextStatsDisplay(transmitText, m_txFrameCountEstimate);
                updateTxButtonDisplay();
                // [TODO.md #67 build 272] Re-evaluate the broader UI
                // button state (specifically the ARQ button's
                // armed/not-armed visual) now that text has stabilized.
                // updateButtonDisplay calls evaluateArqGateForText
                // against the current widget contents and restyles the
                // ARQ button only if the resulting style differs from
                // the current one (no per-keystroke flash).
                updateButtonDisplay();
            });
    t->start();
#endif
}

void UI_Constructor::updateTextWordCheckerDisplay() {
    if (!m_config.spellcheck()) {
        return;
    }

    JSCChecker::checkRange(ui->extFreeTextMsgEdit, 0, -1);
}

void UI_Constructor::updateTextStatsDisplay(QString text, int count) {
    // Use precise period in ms to avoid rounding error (3750ms → 4s loses accuracy)
    const double fpm = 60000.0 / JS8::Submode::periodMS(m_nSubMode);
    if (count > 0) {
        auto words = text.split(" ", Qt::SkipEmptyParts).length();
        auto wpm = QString::number(words / (count / fpm), 'f', 1);
        auto cpm = QString::number(text.length() / (count / fpm), 'f', 1);
        wpm_label.setText(QString("%1wpm / %2cpm").arg(wpm).arg(cpm));
        wpm_label.setVisible(true);
    } else {
        wpm_label.setVisible(false);
        wpm_label.clear();
    }
}

void UI_Constructor::updateTxButtonDisplay() {
    // can we transmit at all?
    bool canTransmit = ensureCanTransmit();

    // if we're tuning or have a message queued
    if (m_tune || isMessageQueuedForTransmit()) {
        int count = m_txFrameCount;
        int left = m_txFrameQueue.count();
        int sent = count - left;
        QString buttonText;
        if (m_tune) {
            buttonText = State::Tuning.toString();
        } else if (m_transmitting || sent > 0) {
            // Elapsed-time countdown: total expected - wall-clock elapsed
            int totalExpected = count * m_TRperiod;
            int elapsed = m_txQueueStartTime.isValid()
                ? static_cast<int>(m_txQueueStartTime.secsTo(
                      DriftingDateTime::currentDateTimeUtc()))
                : 0;
            int secs = qMax(0, totalExpected - elapsed);
            qDebug("[TX-BTN] %s: left=%d count=%d sent=%d elapsed=%d total=%d secs=%d",
                   m_transmitting ? "SENDING" : "READY",
                   left, count, sent, elapsed, totalExpected, secs);
            buttonText = State::timed(
                m_transmitting ? State::Sending : State::Ready, secs);
        }
        ui->startTxButton->setText(buttonText);
        ui->startTxButton->setEnabled(false);
        ui->startTxButton->setFlat(true);
        // [FILE-XFER build 282] Chevron stays enabled full-time; the
        // menu action mirrors Send's disabled state (no TX while
        // queued / transmitting).
        if (m_sendFileAction) m_sendFileAction->setEnabled(false);
    } else {
        QString const buttonText =
            m_txFrameCountEstimate > 0
                ? State::timed(State::Send, m_txFrameCountEstimate * m_TRperiod)
                : State::Send.toString();
        ui->startTxButton->setText(buttonText);
        bool const sendEnabled = canTransmit &&
                                 m_txFrameCountEstimate > 0 &&
                                 !ui->extFreeTextMsgEdit->toPlainText().isEmpty();
        ui->startTxButton->setEnabled(sendEnabled);
        ui->startTxButton->setFlat(false);
        // [FILE-XFER build 283] File send doesn't read the outgoing
        // widget, so the action's enable gate is just canTransmit —
        // an empty outgoing box is a valid starting state for "pick
        // a file to send". Chevron button itself stays enabled
        // full-time for discoverability.
        if (m_sendFileAction) m_sendFileAction->setEnabled(canTransmit);
    }
}

QString UI_Constructor::callsignSelected(bool) {
    return m_selectedCallsign;
}


// --- Selection spec: central entry points ---

void UI_Constructor::selectCallsign(QString call, int submode) {
    if (call.isEmpty())
        return;

    // Always switch mode even if same callsign (could be different row/mode)
    if (submode >= 0)
        autoSwitchMode(submode);

    // Pause HB when selecting a callsign (entering QSO)
    if (m_config.heartbeat_qso_pause()) {
        if (ui->hbMacroButton->isChecked()) {
            qWarning() << "[HAIL-DIAG] loop cancelled: callsign selected (QSO pause) call=" << call;
            ui->hbMacroButton->setChecked(false);
            m_hb_loop->onLoopCancel();
            m_hbPaused = true;
        }
        if (ui->cqMacroButton->isChecked()) {
            ui->cqMacroButton->setChecked(false);
            m_cq_loop->onLoopCancel();
        }
    }

    if (call == m_selectedCallsign)
        return;

    m_selectedCallsign = call;

    if (!m_callSelectedTime.contains(call))
        m_callSelectedTime[call] = DriftingDateTime::currentDateTimeUtc();

    // Immediately select in callsign table if present
    ui->tableWidgetCalls->blockSignals(true);
    for (int r = 0; r < ui->tableWidgetCalls->rowCount(); ++r) {
        auto item = ui->tableWidgetCalls->item(r, 0);
        if (item && item->data(Qt::UserRole).toString() == call) {
            ui->tableWidgetCalls->selectRow(r);
            break;
        }
    }
    ui->tableWidgetCalls->blockSignals(false);

    // Clear stale callsign from outgoing box (placed by previous dbl-click on right panel)
    auto outText = ui->extFreeTextMsgEdit->toPlainText().trimmed().toUpper();
    if (!outText.isEmpty() && outText != call.toUpper()) {
        // Check if the outgoing box contains only a callsign from the callsign list
        for (auto const &key : m_callActivity.keys()) {
            if (key.toUpper() == outText) {
                ui->extFreeTextMsgEdit->clear();
                break;
            }
        }
    }

    auto placeholderText =
        QString("Type your outgoing directed message to %1 here.")
            .arg(call).toUpper();
    ui->extFreeTextMsgEdit->setPlaceholderText(placeholderText);

    updateButtonDisplay();
    updateTextDisplay();
    statusChanged();
}

void UI_Constructor::clearSelection() {
    // Restore HB if it was paused for this QSO (Build 122 set m_hbPaused
    // when entering selectCallsign() with heartbeat_qso_pause enabled).
    // The historical restore lived in callsignSelectedChanged(), but that
    // function was never called after Build 58 moved selection to explicit
    // click handlers — the timer was killed and never resumed. (J-IMP
    // build 215124da plumbs the restore through callsignSelectedChanged
    // from tableSelectionChanged; we inline it here instead since both
    // deselect paths funnel through clearSelection().)
    if (m_hbPaused) {
        ui->hbMacroButton->setChecked(true);
        m_hbPaused = false;
    }

    m_callSelectedTime.remove(m_selectedCallsign);
    m_selectedCallsign.clear();

    ui->tableWidgetRXAll->blockSignals(true);
    ui->tableWidgetCalls->blockSignals(true);
    ui->tableWidgetRXAll->clearSelection();
    ui->tableWidgetCalls->clearSelection();
    ui->tableWidgetRXAll->blockSignals(false);
    ui->tableWidgetCalls->blockSignals(false);

    // Clear stale callsign from outgoing box (no call selected now)
    auto outText = ui->extFreeTextMsgEdit->toPlainText().trimmed().toUpper();
    if (!outText.isEmpty()) {
        for (auto const &key : m_callActivity.keys()) {
            if (key.toUpper() == outText) {
                ui->extFreeTextMsgEdit->clear();
                break;
            }
        }
    }

    ui->extFreeTextMsgEdit->setPlaceholderText(
        QString("Type your outgoing messages here.\nType partial call sign to search list.").toUpper());

    updateButtonDisplay();
    updateTextDisplay();
    statusChanged();
}

void UI_Constructor::autoSwitchMode(int submode) {
    if (m_transmitting || m_txFrameCount > 0 || !m_txFrameQueue.isEmpty())
        return;  // don't switch mode during TX
    if (submode == m_nSubMode)
        return;
    if (submode == Varicode::JS8CallFT2) {
        m_prevStandardSubmode = m_nSubMode;
        setSubmode(Varicode::JS8CallFT2);
    } else if (m_nSubMode == Varicode::JS8CallFT2) {
        setSubmode(m_prevStandardSubmode);
    }
}

void UI_Constructor::clearCallsignSelected() { clearSelection(); }

bool UI_Constructor::isRecentOffset(int submode, int offset) {
    if (abs(offset - freq()) <= JS8::Submode::rxThreshold(submode)) {
        return true;
    }
    return (m_rxRecentCache.contains(offset / 10 * 10) &&
            m_rxRecentCache[offset / 10 * 10]->secsTo(
                DriftingDateTime::currentDateTimeUtc()) < 120);
}

void UI_Constructor::markOffsetRecent(int offset) {
    m_rxRecentCache.insert(
        offset / 10 * 10, new QDateTime(DriftingDateTime::currentDateTimeUtc()),
        10);
    m_rxRecentCache.insert(
        offset / 10 * 10 + 10,
        new QDateTime(DriftingDateTime::currentDateTimeUtc()), 10);
}

bool UI_Constructor::isDirectedOffset(int offset, bool *pIsAllCall) {
    bool isDirected = (m_rxDirectedCache.contains(offset / 10 * 10) &&
                       m_rxDirectedCache[offset / 10 * 10]->date.secsTo(
                           DriftingDateTime::currentDateTimeUtc()) < 120);

    if (isDirected && pIsAllCall) {
        *pIsAllCall = m_rxDirectedCache[offset / 10 * 10]->isAllcall;
    }

    return isDirected;
}

void UI_Constructor::markOffsetDirected(int offset, bool isAllCall) {
    CachedDirectedType *d1 = new CachedDirectedType{
        isAllCall, DriftingDateTime::currentDateTimeUtc()};
    CachedDirectedType *d2 = new CachedDirectedType{
        isAllCall, DriftingDateTime::currentDateTimeUtc()};
    m_rxDirectedCache.insert(offset / 10 * 10, d1, 10);
    m_rxDirectedCache.insert(offset / 10 * 10 + 10, d2, 10);
}

void UI_Constructor::clearOffsetDirected(int offset) {
    m_rxDirectedCache.remove(offset / 10 * 10);
    m_rxDirectedCache.remove(offset / 10 * 10 + 10);
}

bool UI_Constructor::isMyCallIncluded(const QString &text) {
    QString myCall = Radio::base_callsign(m_config.my_callsign());

    if (myCall.isEmpty()) {
        return false;
    }

    if (!text.contains(myCall)) {
        return false;
    }

    auto calls = Varicode::parseCallsigns(text);
    return calls.contains(myCall) || calls.contains(m_config.my_callsign());
}

bool UI_Constructor::isAllCallIncluded(const QString &text) {
    return text.contains("@ALLCALL") || text.contains("@HB");
}

bool UI_Constructor::isGroupCallIncluded(const QString &text) {
    return m_config.my_groups().contains(text);
}

void UI_Constructor::processActivity(bool force) {
    if (!m_rxDirty && !force) {
        return;
    }

    // Recent Rx Activity
    processRxActivity();

    // Process Idle Activity
    processIdleActivity();

    // Grouped Compound Activity
    processCompoundActivity();

    // Buffered Activity
    processBufferedActivity();

    // Command Activity
    processCommandActivity();

    // Process PSKReporter Spots
    processSpots();

    m_rxDirty = false;
}


void UI_Constructor::setDrift(int n) { DriftingDateTime::setDrift(n); }

void UI_Constructor::matchCallsignFromInput() {
    QString text = ui->extFreeTextMsgEdit->toPlainText().trimmed();

    // Only match a single word (no spaces)
    if (text.contains(' ')) return;

    // Ignore @ directives (@ALLCALL, @HB, etc.)
    if (text.startsWith('@')) return;

    // Clear previous type-ahead highlight
    for (int r = 0; r < ui->tableWidgetCalls->rowCount(); ++r) {
        for (int c = 0; c < ui->tableWidgetCalls->columnCount(); ++c) {
            auto item = ui->tableWidgetCalls->item(r, c);
            if (item && item->data(Qt::UserRole + 1).toBool()) {
                item->setBackground(QBrush());
                item->setData(Qt::UserRole + 1, false);
            }
        }
    }

    if (text.length() < 2) return;

    // Search callsign table for matching row — highlight and scroll
    for (int r = 0; r < ui->tableWidgetCalls->rowCount(); ++r) {
        auto item = ui->tableWidgetCalls->item(r, 1);  // column 1 = callsign
        if (item) {
            QString call = item->data(Qt::UserRole).toString();
            if (call.startsWith(text, Qt::CaseInsensitive)) {
                // Subtle gray highlight on the matching row
                QColor highlight(0, 0, 0, 20);  // very subtle
                for (int c = 0; c < ui->tableWidgetCalls->columnCount(); ++c) {
                    auto cell = ui->tableWidgetCalls->item(r, c);
                    if (cell) {
                        cell->setBackground(highlight);
                        cell->setData(Qt::UserRole + 1, true);  // mark for clearing
                    }
                }
                ui->tableWidgetCalls->scrollToItem(item);
                return;
            }
        }
    }
}

void UI_Constructor::processIdleActivity() {
    // Don't insert MFI markers for our own outgoing frames during TX
    if (m_transmitting)
        return;

    auto const now = DriftingDateTime::currentDateTimeUtc();

    // if we detect an idle offset, insert an ellipsis into the activity queue
    // and band activity

    for (auto [offset, activity] : m_bandActivity.asKeyValueRange()) {
        if (activity.isEmpty())
            continue;

        auto const last = activity.last();

        if ((last.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast)
            continue;
        if (last.text == m_config.mfi())
            continue;
        if (last.utcTimestamp.msecsTo(now) <
            JS8::Submode::periodMS(last.submode) * 2)
            continue;

        ActivityDetail d = {};
        d.text = m_config.mfi();
        d.utcTimestamp = last.utcTimestamp;
        d.snr = last.snr;
        d.tdrift = last.tdrift;
        d.dial = last.dial;
        d.offset = last.offset;
        d.submode = last.submode;

        if (hasExistingMessageBuffer(d.submode, offset, false, nullptr)) {
            m_messageBuffer[offset].msgs.append(d);
        }

        m_rxActivityQueue.append(d);
        activity.append(d);
    }
}

void processRxActivity(); // JS8_Mainwindow/processRxActivity.cpp

void UI_Constructor::processCompoundActivity() {
    if (m_messageBuffer.isEmpty()) {
        return;
    }

    // group compound callsign and directed commands together.
    foreach (auto freq, m_messageBuffer.keys()) {

        auto &buffer = m_messageBuffer[freq];

        qCDebug(mainwindow_js8) << "-> grouping buffer for freq" << freq;

        if (buffer.compound.isEmpty()) {
            qCDebug(mainwindow_js8) << "-> buffer.compound is empty...skip";
            continue;
        }

        // if we don't have an initialized command, skip...
        int bits = buffer.cmd.bits;
        bool validBits =
            (bits == Varicode::JS8Call ||
             ((bits & Varicode::JS8CallFirst) == Varicode::JS8CallFirst) ||
             ((bits & Varicode::JS8CallLast) == Varicode::JS8CallLast) ||
             ((bits & Varicode::JS8CallData) == Varicode::JS8CallData));
        if (!validBits) {
            qCDebug(mainwindow_js8) << "-> buffer.cmd bits is invalid...skip";
            continue;
        }

        // if we need two compound calls, but less than two have arrived...skip
        if (buffer.cmd.from == "<....>" && buffer.cmd.to == "<....>" &&
            buffer.compound.length() < 2) {
            qCDebug(mainwindow_js8)
                << "-> buffer needs two compound, but has less...skip";
            continue;
        }

        // if we need one compound call, but non have arrived...skip
        if ((buffer.cmd.from == "<....>" || buffer.cmd.to == "<....>") &&
            buffer.compound.length() < 1) {
            qCDebug(mainwindow_js8)
                << "-> buffer needs one compound, but has less...skip";
            continue;
        }

        if (buffer.cmd.from == "<....>") {
            auto d = buffer.compound.dequeue();
            buffer.cmd.from = d.call;
            buffer.cmd.grid = d.grid;
            buffer.cmd.isCompound = true;
            buffer.cmd.utcTimestamp =
                qMin(buffer.cmd.utcTimestamp, d.utcTimestamp);

            if ((d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast) {
                buffer.cmd.bits = d.bits;
            }
        }

        if (buffer.cmd.to == "<....>") {
            auto d = buffer.compound.dequeue();
            buffer.cmd.to = d.call;
            buffer.cmd.isCompound = true;
            buffer.cmd.utcTimestamp =
                qMin(buffer.cmd.utcTimestamp, d.utcTimestamp);

            if ((d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast) {
                buffer.cmd.bits = d.bits;
            }
        }

        if ((buffer.cmd.bits & Varicode::JS8CallLast) !=
            Varicode::JS8CallLast) {
            qCDebug(mainwindow_js8) << "-> still not last message...skip";
            continue;
        }

        // fixup the datetime with the "minimum" dt seen
        // this will allow us to delete the activity lines
        // when the compound buffered command comes in.
        auto dt = buffer.cmd.utcTimestamp;
        foreach (auto c, buffer.compound) {
            dt = qMin(dt, c.utcTimestamp);
        }
        foreach (auto m, buffer.msgs) {
            dt = qMin(dt, m.utcTimestamp);
        }
        buffer.cmd.utcTimestamp = dt;

        qCDebug(mainwindow_js8)
            << "buffered compound command ready" << buffer.cmd.from
            << buffer.cmd.to << buffer.cmd.cmd;

        m_rxCommandQueue.append(buffer.cmd);
        m_messageBuffer.remove(freq);

        // TODO: only if to me?
        m_lastClosedMessageBufferOffset = freq;
    }
}

void processBufferedActivity(); // JS8_Mainwindow/processBufferedActivity.cpp

void processCommandActivity(); // JS8_Mainwindow/processCommandActivity.cpp

QString UI_Constructor::inboxPath() {
    return QDir::toNativeSeparators(
        m_config.writeable_data_dir().absoluteFilePath("inbox.db3"));
}

void UI_Constructor::refreshInboxCounts() {
    auto inbox = Inbox(inboxPath());
    if (inbox.open()) {
        // reset inbox counts
        m_rxInboxCountCache.clear();

        // compute new counts from db
        auto v = inbox.values("UNREAD", "$", "%", 0, 10000);
        foreach (auto pair, v) {
            auto params = pair.second.params();
            auto to = params.value("TO").toString();
            if (to.isEmpty() || to != m_config.my_callsign()) {
                continue;
            }
            auto from = params.value("FROM").toString();
            if (from.isEmpty()) {
                continue;
            }

            m_rxInboxCountCache[from] = m_rxInboxCountCache.value(from, 0) + 1;

            if (!m_callActivity.contains(from)) {
                auto const utc = params.value("UTC").toString();
                auto const snr = params.value("SNR").toInt();
                auto const dial = params.value("DIAL").toInt();
                auto const offset = params.value("OFFSET").toInt();
                auto const tdrift = params.value("TDRIFT").toInt();
                auto const submode = params.value("SUBMODE").toInt();

                CallDetail cd;
                cd.call = from;
                cd.snr = snr;
                cd.dial = dial;
                cd.offset = offset;
                cd.tdrift = tdrift;
                cd.utcTimestamp = QDateTime(
                    QDate::fromString(utc.left(10), "yyyy-MM-dd"),
                    QTime::fromString(utc.mid(11), "hh:mm:ss"),
                    QTimeZone::utc());
                cd.ackTimestamp = cd.utcTimestamp;
                cd.submode = submode;
                logCallActivity(cd, false);
            }
        }

        // Now handle group message counts
        QMap<QString, int> groupMessageCounts = inbox.getGroupMessageCounts();
        foreach (auto key, groupMessageCounts.keys()) {
            m_rxInboxCountCache[key] = groupMessageCounts[key];
        }
    }
}

bool UI_Constructor::hasMessageHistory(QString call) {
    auto inbox = Inbox(inboxPath());
    if (!inbox.open()) {
        return false;
    }

    int store = inbox.count("STORE", "$.params.TO", call);
    int unread = inbox.count("UNREAD", "$.params.FROM", call);
    int read = inbox.count("READ", "$.params.FROM", call);
    return (store + unread + read) > 0;
}

int UI_Constructor::addCommandToMyInbox(CommandDetail d) {
    // local cache for inbox count
    m_rxInboxCountCache[d.from] = m_rxInboxCountCache.value(d.from, 0) + 1;

    // add it to my unread inbox
    return addCommandToStorage("UNREAD", d);
}

int UI_Constructor::addCommandToStorage(QString type, CommandDetail d) {
    // inbox:
    auto inbox = Inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    QVariantMap v = {
        {"UTC", QVariant(d.utcTimestamp.toString("yyyy-MM-dd hh:mm:ss"))},
        {"TO", QVariant(d.to)},
        {"FROM", QVariant(d.from)},
        {"PATH", QVariant(d.relayPath)},
        {"TDRIFT", QVariant(d.tdrift)},
        {"FREQ", QVariant(d.dial + d.offset)},
        {"DIAL", QVariant(d.dial)},
        {"OFFSET", QVariant(d.offset)},
        {"CMD", QVariant(d.cmd)},
        {"SNR", QVariant(d.snr)},
        {"SUBMODE", QVariant(d.submode)},
    };

    if (!d.grid.isEmpty()) {
        v["GRID"] = QVariant(d.grid);
    }

    if (!d.extra.isEmpty()) {
        v["EXTRA"] = QVariant(d.extra);
    }

    if (!d.text.isEmpty()) {
        v["TEXT"] = QVariant(d.text);
    }

    auto m = Message(type, "", v);

    int msgId = inbox.append(m);

    emit messageAdded(msgId);

    return msgId;
}

int UI_Constructor::getNextMessageIdForCallsign(QString callsign) {
    auto inbox = Inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    auto v1 = inbox.values("STORE", "$.params.TO", callsign, 0, 10);
    foreach (auto pair, v1) {
        auto params = pair.second.params();
        auto text = params.value("TEXT").toString().trimmed();
        if (!text.isEmpty()) {
            return pair.first;
        }
    }

    auto v2 = inbox.values("STORE", "$.params.TO",
                           Radio::base_callsign(callsign), 0, 10);
    foreach (auto pair, v2) {
        auto params = pair.second.params();
        auto text = params.value("TEXT").toString().trimmed();
        if (!text.isEmpty()) {
            return pair.first;
        }
    }

    return -1;
}

int UI_Constructor::getLookaheadMessageIdForCallsign(QString callsign,
                                                     int msgId) {
    auto inbox = Inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    int mid = inbox.getLookaheadMessageIdForCallsign(callsign, msgId);

    if (mid == -1) {
        mid = inbox.getLookaheadMessageIdForCallsign(
            Radio::base_callsign(callsign), msgId);
    }

    if (mid != -1) {
        return mid;
    }

    return -1;
}

// Facade for Inbox::getNextGroupMessageIdForCallsign
int UI_Constructor::getNextGroupMessageIdForCallsign(QString group_name,
                                                     QString callsign) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    return inbox.getNextGroupMessageIdForCallsign(group_name, callsign);
}

// Facade for Inbox::getLookaheadGroupMessageIdForCallsign
int UI_Constructor::getLookaheadGroupMessageIdForCallsign(QString group_name,
                                                          QString callsign,
                                                          int afterMsgId) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    int mid = inbox.getLookaheadGroupMessageIdForCallsign(group_name, callsign,
                                                          afterMsgId);

    if (mid == -1) {
        mid = inbox.getLookaheadGroupMessageIdForCallsign(
            group_name, Radio::base_callsign(callsign), afterMsgId);
    }

    if (mid != -1) {
        return mid;
    }

    return -1;
}

// Facade for Inbox::countUnreadForCallsign
int UI_Constructor::countUnreadForCallsign(const QString &callsign) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return 0;
    }

    return inbox.countUnreadForCallsign(callsign);
}

// Facade for Inbox::countGroupUnreadForCallsign
int UI_Constructor::countGroupUnreadForCallsign(const QString &group_name,
                                            const QString &callsign) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return 0;
    }

    return inbox.countGroupUnreadForCallsign(group_name, callsign);
}

// Facade for Inbox::markGroupMsgDeliveredForCallsign
bool UI_Constructor::markGroupMsgDeliveredForCallsign(int msgId,
                                                      QString callsign) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return false;
    }

    return inbox.markGroupMsgDeliveredForCallsign(msgId, callsign);
}

bool UI_Constructor::markMsgDelivered(int mid, Message msg) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return false;
    }

    msg.setType("DELIVERED");
    return inbox.set(mid, msg);
}

QStringList UI_Constructor::parseRelayPathCallsigns(QString from,
                                                    QString text) {
    QStringList calls;
    QString callDePattern = {
        R"(\s([*]DE[*]|VIA)\s(?<callsign>\b(?<prefix>[A-Z0-9]{1,4}\/)?(?<base>([0-9A-Z])?([0-9A-Z])([0-9])([A-Z])?([A-Z])?([A-Z])?)(?<suffix>\/[A-Z0-9]{1,4})?)\b)"};
    QRegularExpression re(callDePattern);
    auto iter = re.globalMatch(text);
    while (iter.hasNext()) {
        auto match = iter.next();
        calls.prepend(match.captured("callsign"));
    }
    calls.prepend(from);
    return calls;
}

void UI_Constructor::processSpots() {
    if (!m_config.spot_to_reporting_networks()) {
        m_rxCallQueue.clear();
        return;
    }

    if (m_rxCallQueue.isEmpty()) {
        return;
    }

    // Is it ok to post spots to PSKReporter?
    int nsec = DriftingDateTime::currentSecsSinceEpoch() - m_secBandChanged;
    bool okToPost = (nsec > (4 * m_TRperiod) / 5);
    if (!okToPost) {
        return;
    }

    while (!m_rxCallQueue.isEmpty()) {
        CallDetail d = m_rxCallQueue.dequeue();
        if (d.call.isEmpty()) {
            continue;
        }

        if (m_config.spot_blacklist().contains(d.call) ||
            m_config.spot_blacklist().contains(Radio::base_callsign(d.call))) {
            continue;
        }

        qCDebug(mainwindow_js8) << "spotting call to reporting networks"
                                << d.call << d.snr << d.dial << d.offset;

        spotReport(d.submode, d.dial, d.offset, d.snr, d.call, d.grid);
        pskLogReport("JS8", d.dial, d.offset, d.snr, d.call, d.grid,
                     d.utcTimestamp);

        if (canSendNetworkMessage()) {
            sendNetworkMessage("RX.SPOT", "",
                               {
                                   {"_ID", QVariant(-1)},
                                   {"FREQ", QVariant(d.dial + d.offset)},
                                   {"DIAL", QVariant(d.dial)},
                                   {"OFFSET", QVariant(d.offset)},
                                   {"CALL", QVariant(d.call)},
                                   {"SNR", QVariant(d.snr)},
                                   {"GRID", QVariant(d.grid)},
                               });
        }
    }
}

/**
 * @brief Processes the outgoing message queue and initiates transmission.
 *
 * This function is called periodically (once per second) to check if there
 * are pending messages in m_txMessageQueue that can be transmitted. It
 * implements several guard conditions to ensure safe transmission:
 *
 * - The frame queue (m_txFrameQueue) must be empty
 * - The message text box must be empty
 * - No active transmission in progress (m_transmitting and m_txFrameCount)
 * - Low priority messages must wait 30 seconds after last transmission
 *
 * When conditions are met, the next message is dequeued, placed in the
 * message text box, and transmission is initiated for high-priority messages.
 *
 * @note This function works in conjunction with resetMessageTransmitQueue()
 *       to support queuing multiple messages (e.g., APRS relay messages)
 *       that are transmitted sequentially.
 */
void UI_Constructor::processTxQueue() {
#if IDLE_BLOCKS_TX
    if (m_tx_watchdog) {
        return;
    }
#endif

    if (m_txMessageQueue.isEmpty()) {
        return;
    }

    // grab the next message...
    auto head = m_txMessageQueue.head();

    // decide if it's ok to transmit...
    int f = head.offset;
    if (f == -1) {
        f = freq();
    }

    // we need a valid frequency...
    if (f <= 0) {
        return;
    }

    // tx frame queue needs to be empty...
    if (!m_txFrameQueue.isEmpty()) {
        return;
    }

    // our message box needs to be empty...
    if (!ui->extFreeTextMsgEdit->toPlainText().isEmpty()) {
        return;
    }

    // don't process if we're currently transmitting...
    if (isMessageQueuedForTransmit()) {
        return;
    }

    // and if we are a low priority message, we need to have not transmitted
    // in the past 30 seconds...
    if (head.priority <= PriorityLow &&
        m_lastTxStartTime.secsTo(DriftingDateTime::currentDateTimeUtc()) <=
            30) {
        return;
    }

    // if so... dequeue the next message from the queue...
    auto message = m_txMessageQueue.dequeue();

    // add the message to the outgoing message text box
    addMessageText(message.message, true);

    // [QUEUE PROVENANCE 2026-06-10 build 247] Snapshot the post-inject
    // edit-box content so startTx can detect a queue-injected manual
    // send and route to startTxNonArq (no ARQ wrapping). Without this,
    // PriorityNormal autoreplies with autoreply OFF sit in the box
    // until the operator clicks Send — and that click hits the ARQ
    // gate, which mis-classifies relay-marker reply paths
    // (e.g. "K9AVT>WM8Q STATUS …") as explicit ARQ-relay requests and
    // wraps them. Snapshot the actual edit-box content (not
    // message.message) because addMessageText may have prefixed a
    // space or otherwise normalized the text.
    m_lastQueueInjectedText = ui->extFreeTextMsgEdit->toPlainText();

    // check to see if this is a high priority message, or if we have
    // autoreply enabled, or if this is a ping and the ping button is
    // enabled
    bool isHB = message.message.contains(" HEARTBEAT ") ||
                message.message.contains(" HB ");
    if (message.priority >= PriorityHigh || isHB ||
        ui->actionModeAutoreply->isChecked()) {
        // then try to set the frequency...
        setFreqOffsetForRestore(f, true);

        // Then prepare to transmit. CRUCIAL: bypass toggleTx(true) so we
        // don't route through on_startTxButton_toggled → startTx → ARQ
        // gate. Auto-replies / queue-drained traffic (autoreply, HB/CQ
        // loops, TCP API, relay) must NEVER get ARQ-wrapped no matter
        // what mode is enabled — that wrapping is reserved for messages
        // the operator places in the outgoing box and Sends manually.
        // The button visual still flips to "checked" so the UI matches
        // the in-flight state; QSignalBlocker prevents the toggle slot
        // (which would call startTx and re-enter the ARQ gate) from
        // firing.
        {
            QSignalBlocker const block(ui->startTxButton);
            ui->startTxButton->setChecked(true);
        }
        startTxNonArq();
    }

    if (message.callback) {
        message.callback();
    }
}

void UI_Constructor::displayActivity(bool force) {
    if (!m_rxDisplayDirty && !force) {
        return;
    }

    // Band Activity
    displayBandActivity();

    // Call Activity
    displayCallActivity();

    m_rxDisplayDirty = false;
}

// updateBandActivity
void displayBandActivity(); // JS8_Mainwindow/displayBandActivity.cpp

// updateCallActivity
void displayCallActivity(); // JS8_Mainwindow/displayCallActivity.cpp

void UI_Constructor::emitPTT(bool on) {
    qCDebug(mainwindow_js8) << "Setting PTT to" << (on ? "on" : "off");

    Q_EMIT m_config.transceiver_ptt(on);

    // emit to network
    sendNetworkMessage(
        "RIG.PTT", on ? "on" : "off",
        {
            {"_ID", QVariant(-1)},
            {"PTT", QVariant(on)},
            {"UTC",
             QVariant(
                 DriftingDateTime::currentDateTimeUtc().toMSecsSinceEpoch())},
        });
}

void UI_Constructor::emitTones() {
    if (!canSendNetworkMessage()) {
        return;
    }

    // emit tone numbers to network
    QVariantList t;
    for (int i = 0; i < JS8_NUM_SYMBOLS; i++) {
        // qCDebug(mainwindow_js8) << "tone" << i << "=" << itone[i];
        t.append(QVariant((int)itone[i]));
    }

    sendNetworkMessage("TX.FRAME", "", {{"_ID", QVariant(-1)}, {"TONES", t}});
}

void UI_Constructor::udpNetworkMessage(Message const &message) {
    if (!m_config.udpEnabled()) {
        return;
    }

    if (!m_config.accept_udp_requests()) {
        return;
    }

    networkMessage(message);
}

void UI_Constructor::tcpNetworkMessage(Message const &message) {
#if 0  // TCP diagnostic logging — enable for JS8 Spotter debugging
    qWarning() << "[TCP-RX] type=" << message.type()
               << "value=" << message.value().left(100)
               << "tcpEnabled=" << m_config.tcpEnabled()
               << "acceptTcp=" << m_config.accept_tcp_requests();
#endif
    if (!m_config.tcpEnabled()) {
        return;
    }

    if (!m_config.accept_tcp_requests()) {
        return;
    }

    networkMessage(message);
}

void networkMessage(); // JS8_Mainwindow/networkMessage.cpp

bool UI_Constructor::canSendNetworkMessage() {
    return m_config.udpEnabled() || m_config.tcpEnabled();
}

void UI_Constructor::sendNetworkMessage(QString const &type,
                                        QString const &message) {
    if (!canSendNetworkMessage()) {
        return;
    }

    auto m = Message(type, message);

    if (m_config.udpEnabled()) {
        m_messageClient->send(m);
    }

    if (m_config.tcpEnabled()) {
        m_messageServer->send(m);
    }
}

void UI_Constructor::sendNetworkMessage(QString const &type,
                                        QString const &message,
                                        QVariantMap const &params) {
    if (!canSendNetworkMessage()) {
        return;
    }

    // Log outgoing RX notifications for TCP API debugging
#if 0  // TCP diagnostic logging — enable for JS8 Spotter debugging
    if (type.startsWith("RX.") || type.startsWith("TX."))
        qWarning() << "[TCP-TX]" << type << "msg=" << message.left(60);
#endif

    auto m = Message(type, message, params);

    if (m_config.udpEnabled()) {
        m_messageClient->send(m);
    }

    if (m_config.tcpEnabled()) {
        m_messageServer->send(m);
    }
}

void UI_Constructor::pskReporterError(QString const &message) {
    qCDebug(mainwindow_js8) << "PSK Reporter Error:" << message;

    showStatusMessage(tr("Spotting to PSK Reporter unavailable"));
}

void UI_Constructor::setRig(Frequency f) {
    if (f) {
        m_freqNominal = f;
        m_freqTxNominal = m_freqNominal - m_XIT;
    }

    if (m_transmitting && !m_config.tx_qsy_allowed())
        return;

    if ((m_monitoring || m_transmitting) && m_config.transceiver_online()) {
        if (m_config.split_mode()) {
            Q_EMIT m_config.transceiver_tx_frequency(m_freqTxNominal);
        }

        Q_EMIT m_config.transceiver_frequency(m_freqNominal);
    }
}

/**
 * @brief Update and send station status
 *
 * Sends station status updates to both WSJT-X protocol clients (if enabled)
 * and native JSON API clients (if not conflicting). When WSJT-X protocol
 * is enabled on the same port/address as the native JSON API, the native
 * JSON messages are skipped to avoid conflicts.
 */
void UI_Constructor::statusUpdate() {
    // Send WSJT-X Status message if protocol is enabled
    if (m_wsjtxMessageMapper && m_config.wsjtx_protocol_enabled()) {
        QString dx_call = callsignSelected();
        QString dx_grid = "";
        if (!dx_call.isEmpty() && m_callActivity.contains(dx_call)) {
            dx_grid = m_callActivity[dx_call].grid;
        }
        QString mode = JS8::Submode::name(m_nSubMode);
        QString tx_message = m_transmitting ? m_currentMessage : "";

        m_wsjtxMessageMapper->sendStatusUpdate(
            dialFrequency(), freq(),
            "JS8", // mode
            dx_call, m_config.my_callsign(), m_config.my_grid(), dx_grid,
            true, // tx_enabled - JS8Call always allows TX when not in
                  // special modes
            m_transmitting,
            m_decoderBusy || m_monitoring, // decoding
            tx_message);
    }

    // Send native JSON message only if not conflicting with WSJT-X
    if (canSendNetworkMessage()) {
        // Don't send JSON if WSJT-X is enabled on the same port/address
        bool skip_json = false;
        if (m_config.wsjtx_protocol_enabled() &&
            m_config.wsjtx_server_port() == m_config.udp_server_port() &&
            m_config.wsjtx_server_name() == m_config.udp_server_name()) {
            skip_json = true;
        }

        if (!skip_json) {
            sendNetworkMessage("STATION.STATUS", "",
                               {
                                   {"FREQ", QVariant(dialFrequency() + freq())},
                                   {"DIAL", QVariant(dialFrequency())},
                                   {"OFFSET", QVariant(freq())},
                                   {"SPEED", QVariant(m_nSubMode)},
                                   {"SELECTED", QVariant(callsignSelected())},
                               });
        }
    }
}

void UI_Constructor::childEvent(QChildEvent *e) {
    if (e->child()->isWidgetType()) {
        switch (e->type()) {
        case QEvent::ChildAdded:
            add_child_to_event_filter(e->child());
            break;
        case QEvent::ChildRemoved:
            remove_child_from_event_filter(e->child());
            break;
        default:
            break;
        }
    }
    QMainWindow::childEvent(e);
}

// add widget and any child widgets to our event filter so that we can
// take action on key press ad mouse press events anywhere in the main
// window
void UI_Constructor::add_child_to_event_filter(QObject *target) {
    if (target && target->isWidgetType()) {
        target->installEventFilter(this);
    }
    auto const &children = target->children();
    for (auto iter = children.begin(); iter != children.end(); ++iter) {
        add_child_to_event_filter(*iter);
    }
}

// recursively remove widget and any child widgets from our event filter
void UI_Constructor::remove_child_from_event_filter(QObject *target) {
    auto const &children = target->children();
    for (auto iter = children.begin(); iter != children.end(); ++iter) {
        remove_child_from_event_filter(*iter);
    }
    if (target && target->isWidgetType()) {
        target->removeEventFilter(this);
    }
}

void UI_Constructor::resetIdleTimer() {
    if (m_idleMinutes) {
        m_idleMinutes = 0;
        qCDebug(mainwindow_js8) << "idle" << m_idleMinutes << "minutes";
    }
}

void UI_Constructor::incrementIdleTimer() {
    m_idleMinutes++;
    qCDebug(mainwindow_js8)
        << "increment idle to" << m_idleMinutes << "minutes";
}

void UI_Constructor::tx_watchdog(bool triggered) {
    auto prior = m_tx_watchdog;
    m_tx_watchdog = triggered;
    if (triggered) {
        m_isTimeToSend = false;
        if (m_tune)
            stop_tuning();
        if (m_auto)
            auto_tx_mode(false);
        stopTx();
        tx_status_label.setStyleSheet(
            "QLabel{background-color: #000000; color:#ffffff; }");
        tx_status_label.setText("Idle timeout");

        // if the watchdog is triggered...we're no longer active
        bool wasAuto = ui->actionModeAutoreply->isChecked();
        bool wasHB = ui->hbMacroButton->isChecked();
        bool wasCQ = ui->cqMacroButton->isChecked();

        // save the button states
        ui->actionModeAutoreply->setChecked(false);
        if (wasHB)
            qWarning() << "[HAIL-DIAG] loop cancelled: idle watchdog after"
                        << m_config.watchdog() << "minutes";
        ui->hbMacroButton->setChecked(false);
        ui->cqMacroButton->setChecked(false);

        // clear the tx queues
        resetMessageTransmitQueue();

        QMessageBox *msgBox = new QMessageBox(this);
        msgBox->setIcon(QMessageBox::Information);
        msgBox->setWindowTitle("Idle Timeout");
        msgBox->setInformativeText(
            QString("You have been idle for more than %1 minutes.")
                .arg(m_config.watchdog()));
        msgBox->addButton(QMessageBox::Ok);

        connect(msgBox, &QMessageBox::finished, this,
                [this, wasAuto, wasHB, wasCQ](int /*result*/) {
                    // restore the button states
                    ui->actionModeAutoreply->setChecked(wasAuto);
                    ui->hbMacroButton->setChecked(wasHB);
                    ui->cqMacroButton->setChecked(wasCQ);

                    this->tx_watchdog(false);
                });
        msgBox->setModal(true);
        msgBox->show();
    }
    if (prior != triggered)
        statusUpdate();
}

void UI_Constructor::write_frequency_entry(QString const &file_name) {
    if (!m_config.write_logs()) {
        return;
    }

    // Write freq changes to ALL.TXT only below 30 MHz.
    QFile f2{m_config.writeable_data_dir().absoluteFilePath(file_name)};
    if (f2.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream out(&f2);
        out << DriftingDateTime::currentDateTimeUtc().toString(
                   "yyyy-MM-dd hh:mm:ss")
            << "  " << qSetRealNumberPrecision(12) << (m_freqNominal / 1.e6)
            << " MHz  "
            << "JS8" << Qt::endl;
        f2.close();
    } else {
        QTimer::singleShot(0, [this,
                               message = tr("Cannot open \"%1\" for append: %2")
                                             .arg(f2.fileName())
                                             .arg(f2.errorString())] {
            JS8MessageBox::warning_message(this, tr("Log File Error"), message);
        });
    }
}

void UI_Constructor::write_transmit_entry(QString const &file_name) {
    if (!m_config.write_logs()) {
        return;
    }

    QFile f{m_config.writeable_data_dir().absoluteFilePath(file_name)};
    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream out(&f);
        auto time = DriftingDateTime::currentDateTimeUtc();
        time = time.addSecs(-(time.time().second() % m_TRperiod));
        auto dt =
            DecodedText(m_currentMessage, m_currentMessageBits, m_nSubMode);
        out << time.toString("yyyy-MM-dd hh:mm:ss") << "  Transmitting "
            << qSetRealNumberPrecision(12) << (m_freqNominal / 1.e6) << " MHz  "
            << "JS8"
            << ":  " << dt.message() << Qt::endl;
        f.close();
    } else {
        QTimer::singleShot(0, [this,
                               message = tr("Cannot open \"%1\" for append: %2")
                                             .arg(f.fileName())
                                             .arg(f.errorString())] {
            JS8MessageBox::warning_message(this, tr("Log File Error"), message);
        });
    }
}

void UI_Constructor::writeAllTxt(QStringView message) {
    if (!m_config.write_logs())
        return;

    // Write decoded text to file "ALL.TXT".

    QFile f{m_config.writeable_data_dir().absoluteFilePath("ALL.TXT")};

    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream out(&f);

        if (m_RxLog == 1) {
            out << DriftingDateTime::currentDateTimeUtc().toString(
                       "yyyy-MM-dd hh:mm:ss")
                << "  " << qSetRealNumberPrecision(12) << (m_freqNominal / 1.e6)
                << " MHz  JS8" << Qt::endl;

            m_RxLog = 0;
        }

        out << message << Qt::endl;

        f.close();
    } else {
        JS8MessageBox::warning_message(this, tr("File Open Error"),
                                       tr("Cannot open \"%1\" for append: %2")
                                           .arg(f.fileName())
                                           .arg(f.errorString()));
    }
}

void UI_Constructor::writeMsgTxt(QStringView message, int snr, int offset) {
    if (!m_config.write_logs())
        return;

    // Write decoded text to file "DIRECTED.TXT".

    QFile f{m_config.writeable_data_dir().absoluteFilePath("DIRECTED.TXT")};

    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream out(&f);
        QString output = DriftingDateTime::currentDateTimeUtc().toString(
                             "yyyy-MM-dd hh:mm:ss") %
                         "\t" % Radio::frequency_MHz_string(m_freqNominal) %
                         "\t" % QString::number(offset) % "\t" %
                         Varicode::formatSNR(snr) % "\t" % message;

        out << output << Qt::endl;

        f.close();
    } else {
        JS8MessageBox::warning_message(this, tr("File Open Error"),
                                       tr("Cannot open \"%1\" for append: %2")
                                           .arg(f.fileName())
                                           .arg(f.errorString()));
    }
}

QByteArray UI_Constructor::wisdomFileName() const {
    return QDir::toNativeSeparators(
               m_config.writeable_data_dir().absoluteFilePath(
                   "js8call_wisdom.dat"))
        .toLocal8Bit();
}


#ifdef JS8_ENABLE_FT2
void UI_Constructor::l2DecodeDone() {
    auto now = QDateTime::currentMSecsSinceEpoch();
    auto delay = m_l2DecodeFinishedMs > 0 ? now - m_l2DecodeFinishedMs : -1;
    if (delay > 500)
        qWarning() << "[FT2-L2] signal delivery delay:" << delay << "ms";
    m_l2Decoding = false;
    // Immediately start next decode — no waiting for timer.
    l2TryDecode("chain");
}

void UI_Constructor::l2TryDecode(char const *source) {
    if (!m_l2Enabled || m_l2Decoding || m_transmitting)
        return;

    std::int64_t pos = m_l2RingPos.load(std::memory_order_acquire);
    if (pos < FT2_NMAX)
        return;  // ring buffer not yet full

    // Acquire Fortran lock via CAS
    bool expected = false;
    if (!JS8::DecodeFT2::fortranLock.compare_exchange_strong(expected, true))
        return;  // standard FT2 has the lock — will retry on next trigger

    // Linearize the ring buffer into a contiguous array. pos is a
    // monotonic 64-bit sample counter, so (pos - validSamples) is the
    // write index of the oldest sample still in the buffer; modulo the
    // ring size gives its physical offset.
    int validSamples =
        (pos < FT2_L2_RINGSIZE) ? static_cast<int>(pos) : FT2_L2_RINGSIZE;
    auto linear = std::make_shared<std::array<std::int16_t, FT2_L2_RINGSIZE>>();
    std::int64_t ringStart = (pos - validSamples) % FT2_L2_RINGSIZE;
    for (int i = 0; i < validSamples; ++i)
        (*linear)[i] = m_l2RingBuf[(ringStart + i) % FT2_L2_RINGSIZE];

    auto buf = linear;

    // DIAG BUILD 51: suppressed — fires every few seconds (revert in Build 52)
    // qWarning() << "[FT2-L2]" << source << ": validSamples=" << validSamples
    //            << "ringPos=" << pos << "nknown=" << m_l2NKnown;

    // Use last decoded frequency for candidate prioritization (not UI cursor)
    int nfqso = m_l2SignalFreq > 0 ? m_l2SignalFreq : dec_data.params.nfqso;
    int nfa = dec_data.params.nfa;
    int nfb = dec_data.params.nfb;
    int utc = dec_data.params.nutc;

    // Snapshot known bits for the async thread
    std::int8_t knownSnap[77 * 20];
    std::memcpy(knownSnap, m_l2KnownBits, sizeof(knownSnap));
    int nknownSnap = m_l2NKnown;

    // [BUILD 294] Capture the snapshot's global base position so the
    // L2 callback can compute the absolute global sample-position for
    // each decoded frame. (pos - validSamples) is the global sample
    // position of buf[0]; adding the per-frame ibest (recovered from
    // the decoded event's xdt) yields an absPos that's identical
    // across decode passes for the same physical frame.
    std::int64_t const snapBasePos = pos - validSamples;
    m_l2Decoding = true;
    m_l2DecodeWatcher.setFuture(QtConcurrent::run(
        [buf, nfqso, nfa, nfb, utc, knownSnap, nknownSnap, snapBasePos, this]() {
        auto t0 = QDateTime::currentMSecsSinceEpoch();

        // --- Sync monitor: scan for Costas tones before full decode ---
        // Build frequency grid: if we have a known signal freq, scan tight
        // around it; otherwise scan the full passband in 50 Hz steps.
        int useNfqsoOnly = 0;
        int scanNfqso = nfqso;
        float syncBest = -99.0f;
        float syncFreq = 0.0f;
        int syncIbest = -1, syncIdf = 0;

        constexpr int MAX_SCAN_FREQS = 40;
        float scanFreqs[MAX_SCAN_FREQS];
        int nScanFreqs = 0;

        if (nfqso > 0 && nfqso >= nfa && nfqso <= nfb) {
            // Known signal frequency — scan ±100 Hz in 25 Hz steps
            for (int f = std::max(nfa, nfqso - 100);
                 f <= std::min(nfb, nfqso + 100) && nScanFreqs < MAX_SCAN_FREQS;
                 f += 25)
                scanFreqs[nScanFreqs++] = static_cast<float>(f);
        } else {
            // No known frequency — scan full passband in 50 Hz steps
            for (int f = nfa; f <= nfb && nScanFreqs < MAX_SCAN_FREQS; f += 50)
                scanFreqs[nScanFreqs++] = static_cast<float>(f);
        }

        if (nScanFreqs > 0) {
            auto tSync = QDateTime::currentMSecsSinceEpoch();
            ft2_sync_scan_c(buf->data(), nScanFreqs, scanFreqs,
                            &syncBest, &syncFreq, &syncIbest, &syncIdf);
            auto syncMs = QDateTime::currentMSecsSinceEpoch() - tSync;

            // [RX-PROBE 2026-06-09] bumped to qWarning to diagnose
            // missed-frame events under chunked-ARQ on wired-loopback
            // audio at +3 dB SNR. Need to see every sync-scan pass'
            // best score to determine if the decoder even SAW the
            // frame's Costas tones, or if sync detection failed.
            qWarning() << "[RX-PROBE] L2 sync scan:" << syncMs << "ms"
                       << "nfreqs=" << nScanFreqs
                       << "sync=" << syncBest << "freq=" << syncFreq
                       << "ibest=" << syncIbest << "idf=" << syncIdf;

            if (syncBest >= 3.00f) {
                // Strong sync well above noise floor (~2.6) — skip getcandidates2
                useNfqsoOnly = 1;
                scanNfqso = static_cast<int>(syncFreq + 0.5f);
            }
        }

        // Show rejected sync-low frames for WM8Q analysis
        JS8::DecodeFT2::showRejected = m_config.my_callsign().startsWith("WM8Q");

        std::int8_t newBits[77 * 20] = {};
        int nNewDecoded = 0;
        float decodedFreq = 0.0f;
        JS8::DecodeFT2::decodeL2(buf->data(), scanNfqso, nfa, nfb, utc,
            [this, snapBasePos](JS8::Event::Variant const &ev) {
                // [BUILD 295] Compute absolute global sample position
                // for Decoded events so processBufferedActivity can
                // sort frames by TX order across multiple sliding-
                // window decode passes.
                //
                // Build 294 had a unit-mismatch bug: snapBasePos is
                // in 12 kHz sample units (matches m_l2RingPos), but
                // ibest computed as (xdt+0.5)*1333.33 is in DECIMATED
                // sample units (1333.33 ≈ 12000/9 = 9-sample bins).
                // Adding mismatched units produced absPos values that
                // varied across decode passes for the same physical
                // frame: pass 1 vs pass 2 differed by ~747 absPos for
                // a 70 ms inter-pass interval (snapBasePos delta of
                // 840 vs ibest delta of only 93). Sort+dedup both
                // failed silently — same frame wasn't recognized as
                // a duplicate, and ordering across passes was wrong.
                //
                // Fix: compute the frame's sample position WITHIN the
                // snapshot in 12 kHz units directly: (xdt + 0.5) *
                // 12000. Then absPos = snapBasePos + that, in
                // consistent 12 kHz sample units. Invariant across
                // passes: snapBasePos increases by the same amount
                // the in-snapshot position decreases.
                JS8::Event::Variant ev2 = ev;
                if (auto *d = std::get_if<JS8::Event::Decoded>(&ev2)) {
                    std::int64_t const samplePosInSnap =
                        static_cast<std::int64_t>((d->xdt + 0.5f) * 12000.0f);
                    d->absPos = snapBasePos + samplePosInSnap;
                }
                QMetaObject::invokeMethod(this, [this, ev2]() {
                    processDecodeEvent(ev2);
                }, Qt::QueuedConnection);
            },
            knownSnap, nknownSnap,
            newBits, &nNewDecoded,
            useNfqsoOnly, &decodedFreq,
            syncBest);
        auto elapsed = QDateTime::currentMSecsSinceEpoch() - t0;
        // [RX-PROBE 2026-06-09] bumped to qWarning — paired with sync
        // scan probe above so we can see which decode passes found
        // a frame vs which missed. Critical: ndecoded=0 with high
        // syncBest means sync detected the frame but LDPC failed.
        qWarning() << "[RX-PROBE] L2 decode took" << elapsed << "ms"
                   << "ndecoded=" << nNewDecoded << "nknown=" << nknownSnap
                   << (useNfqsoOnly ? "SYNC-HIT" : "FULL-SCAN")
                   << "sync=" << syncBest;
        m_l2DecodeFinishedMs = QDateTime::currentMSecsSinceEpoch();
        if (elapsed > 3000) {
            qWarning() << "[FT2-L2] WARNING: decode cycle approaching buffer limit (7500ms)";
        }

        // Expire known frames older than one full buffer (90K samples).
        // m_l2RingPos is now a monotonic 64-bit counter, so the age is
        // just (curPos - knownPos), no wrap adjustments. Prior impl
        // wrapped ringPos at 180000 back to 90000 and the expiration
        // logic only caught half-cycle wraps; entries whose knownPos
        // landed near a wrap boundary could persist indefinitely, which
        // is why WM8Q/P's repeating beacon would go undecodable for
        // minutes at a time under the old code.
        std::int64_t curPos = m_l2RingPos.load(std::memory_order_relaxed);
        int i = 0;
        while (i < m_l2NKnown) {
            std::int64_t age = curPos - m_l2KnownPos[i];
            if (age > FT2_L2_RINGSIZE) {
                // This known frame is old enough to have left the buffer
                qWarning() << "[FT2-L2] expiring known frame" << i
                            << "age=" << age;
                int remaining = m_l2NKnown - i - 1;
                if (remaining > 0) {
                    std::memmove(m_l2KnownBits + i * 77,
                                 m_l2KnownBits + (i + 1) * 77,
                                 remaining * 77);
                    std::memmove(m_l2KnownPos + i,
                                 m_l2KnownPos + i + 1,
                                 remaining * sizeof(int));
                }
                m_l2NKnown--;
            } else {
                i++;
            }
        }

        // Add newly decoded frames to known list
        if (nNewDecoded > 0) {
            for (int d = 0; d < nNewDecoded && m_l2NKnown < 20; d++) {
                std::memcpy(m_l2KnownBits + m_l2NKnown * 77,
                            newBits + d * 77, 77);
                m_l2KnownPos[m_l2NKnown] = curPos;
                m_l2NKnown++;
            }
            // Track signal frequency for candidate prioritization
            if (decodedFreq > 0.0f)
                m_l2SignalFreq = static_cast<int>(decodedFreq + 0.5f);
        }

        JS8::DecodeFT2::fortranLock.store(false);
    }));
}
#endif

Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)
Q_LOGGING_CATEGORY(mainwindow_js8, "mainwindow.js8", QtWarningMsg)
