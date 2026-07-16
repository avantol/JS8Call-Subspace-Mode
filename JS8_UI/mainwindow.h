// -*- Mode: C++ -*-
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <atomic>
#include <cstdint>

#include "JS8_Audio/AudioDevice.h"
#include "JS8_Audio/NotificationAudio.h"
#include "JS8_Audio/SoundInput.h"
#include "JS8_Audio/SoundOutput.h"
#include "JS8_Include/EventFilter.h"
#include "JS8_Include/commons.h"
#include "JS8_Include/qpriorityqueue.h"
#include "JS8_JSC/JSC_checker.h"
#include "JS8_Logbook/LogBook.h"
#include "JS8_Main/APRSISClient.h"
#include "JS8_Main/AprsInboundRelay.h"
#include "JS8_Main/Bands.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Main/FrequencyList.h"
#include "JS8_Main/Geodesic.h"
#include "JS8_Main/ChunkedArq.h"
#include "JS8_Main/HelpTextWindow.h"
#include "JS8_Main/Inbox.h"
#include "JS8_Main/JS8MessageBox.h"
#include "JS8_Main/MessageClient.h"
#include "JS8_Main/MessageServer.h"
#include "JS8_Main/Modes.h"
#include "JS8_Main/MultiSettings.h"
#include "JS8_Main/ProcessThread.h"
#include "JS8_Main/Radio.h"
#include "JS8_Main/SelfDestructMessageBox.h"
#include "JS8_Main/SignalMeter.h"
#include "JS8_Main/StationList.h"
#include "JS8_Main/TxLoop.h"
#include "JS8_Main/Varicode.h"
#include "JS8_Main/qt_helpers.h"
#include "JS8_Main/revision_utils.h"
#include "JS8_Mode/DecodedText.h"
#include "JS8_Mode/Decoder.h"
#include "JS8_Mode/Detector.h"
#include "JS8_Mode/JS8.h"
#include "JS8_Mode/JS8Submode.h"
#include "JS8_Mode/Modulator.h"
#include "JS8_Network/NetworkAccessManager.h"
#include "JS8_Network/PSKReporter.h"
#include "JS8_Network/SpotClient.h"
#include "JS8_Network/TCPClient.h"
#include "JS8_Transceiver/Transceiver.h"
#include "JS8_Transceiver/TransceiverFactory.h"
#include "JS8_UDP/WSJTXMessageClient.h"
#include "JS8_UDP/WSJTXMessageMapper.h"
#include "JS8_UI/About.h"
#include "JS8_UI/Configuration.h"
#include "JS8_UI/WideGraph.h"
#include "JS8_UI/MessagePanel.h"
#include "LogQSO.h"
#include "SpotMapWindow.h"
#include "MessageReplyDialog.h"
#include "ui_mainwindow.h"

#include <QAbstractSocket>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QByteArrayView>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMdiSubWindow>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScopedPointer>
#include <QScrollBar>
#include <QSet>
#include <QStandardPaths>
#include <QStringBuilder>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
#include <QTime>
#include <QTimeZone>
#include <QTimer>
#include <QToolTip>
#include <QUdpSocket>
#include <QUrl>
#include <QVariant>
#include <QVector>
#include <QVersionNumber>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGui>
#include <boost/crc.hpp>
#include <fftw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(mainwindow_js8)

#ifdef JS8_ENABLE_FT2
extern int volatile itone[FT2_NUM_SYMBOLS]; // Max of JS8 (79) and FT2 (103)
extern float ft2_txwave[FT2_NWAVE];         // Pre-computed FT2 GFSK waveform
extern int ft2_txwave_len;                   // Actual waveform length
#else
extern int volatile itone[JS8_NUM_SYMBOLS]; // Audio tones for all Tx symbols
#endif

//--------------------------------------------------------------- mainwindow
// How often to poll the UI, in MS.
// Some things may depend on this being a divisor of 1000.
constexpr quint32 UI_POLL_INTERVAL_MS = 100;

namespace {
namespace Default {
constexpr Radio::Frequency DIAL_FREQUENCY = 14078000;
constexpr auto FREQUENCY = 1500;
constexpr auto SUBMODE = Varicode::JS8CallFT2;
} // namespace Default

namespace State {
constexpr auto RX = 1;
constexpr auto TX = 2;
} // namespace State
} // namespace

namespace Ui {
class UI_Constructor;
}

class QSettings;
class QLineEdit;
class QFont;
class QHostInfo;
class WideGraph;
class LogQSO;
class Transceiver;
class MessageClient;
class QTime;
class HelpTextWindow;
class SoundOutput;
class Modulator;
class SoundInput;
class Detector;
class AprsInboundRelay;
class MultiSettings;
class DecodedText;
class JSCChecker;
class MessagePanel;

using namespace std;
typedef std::function<void()> Callback;

class WSJTXMessageMapper; // Forward declaration

class UI_Constructor : public QMainWindow {
    Q_OBJECT;
    friend class WSJTXMessageMapper; // Allow WSJTXMessageMapper to access
                                     // private members

    struct CallDetail;
    struct CommandDetail;

  public:
    using Frequency = Radio::Frequency;
    using FrequencyDelta = Radio::FrequencyDelta;
    using Mode = Modes::Mode;

    explicit UI_Constructor(QString const &program_info,
                            QDir const &temp_directory, bool multiple,
                            MultiSettings *settings, QWidget *parent = nullptr);
    ~UI_Constructor();

  private:
    struct SortByReverse {
        QString by;
        bool reverse;
    };

    /**
     * Sometimes, buttons are triggered by code and this refers
     * to a momentary situation only.  Sometimes, the trigger
     * pertains to the long term processing, that is, it should
     * influence (shut off, mostly) the automatic transmit loops
     * for HB and CQ.
     *
     * In an ugly klugde, we try to distinguish these cases
     * via these three *IsLongterm variables.
     */
    bool m_stopTxButtonIsLongterm;
    bool m_hbButtonIsLongterm;
    bool m_cqButtonIsLongterm;

  public slots:
    void showSoundInError(const QString &errorMsg);
    void showSoundOutError(const QString &errorMsg);
    void showStatusMessage(const QString &statusMsg);
    void dataSink(qint64 frames);
    /**
     * The name `guiUpdate` suggests updating of the views from the models
     * (in MVC terms, but we don't do MVC in this project), animations and
     * stuff. While it indeed does that, this also contains controller code.
     */
    void guiUpdate();
    void setXIT(int n);
    void qsy(int hzDelta);
    void onDriftChanged(qint64 new_drift_ms);
    void setFreqOffsetForRestore(int freq, bool shouldRestore);
    bool tryRestoreFreqOffset();
    void changeFreq(int);

    bool hasExistingMessageBufferToMe(int *pOffset);
    bool hasExistingMessageBuffer(int submode, int offset, bool drift,
                                  int *pPrevOffset);
    bool hasClosedExistingMessageBuffer(int offset);
    void logCallActivity(CallDetail d, bool spot = true);
    void logHeardGraph(QString from, QString to);
    QString lookupCallInCompoundCache(QString const &call);
    void cacheActivity(QString key);
    void restoreActivity(QString key);
    void clearActivity();
    void clearBandActivity();
    void clearRXActivity();
    void clearCallActivity();
    void createGroupCallsignTableRows(QTableWidget *table,
                                      const QString &selectedCall,
                                      bool &showIconColumn);
    void displayTextForFreq(QString text, int freq, QDateTime date, bool isTx,
                            bool isNewLine, bool isLast, int submode = -1);
    void writeNoticeTextToUI(QDateTime date, QString text);
    int writeMessageTextToUI(QDateTime date, QString text, int freq, bool isTx,
                             int submode = -1, int block = -1);
    bool isMessageQueuedForTransmit();
    bool isInDecodeDelayThreshold(int seconds);
    void prependMessageText(QString text);
    void addMessageText(QString text, bool clear = false,
                        bool selectFirstPlaceholder = false);
    void confirmThenEnqueueMessage(int timeout, int priority, QString message,
                                   int offset, Callback c);
    void enqueueMessage(int priority, QString message, int offset, Callback c);
    void resetMessage();
    void resetMessageUI();
    void restoreMessage();
    void initializeDummyData();
    void initializeGroupMessage();
    bool ensureCallsignSet(bool alert = true);
    bool ensureKeyNotStuck(QString const &text);
    bool ensureNotIdle();
    bool ensureCanTransmit();
    bool ensureCreateMessageReady(const QString &text);
    QString createMessage(QString const &text, bool *pDisableTypeahead);
    QString appendMessage(QString const &text, bool isData,
                          bool *pDisableTypeahead);
    QString createMessageTransmitQueue(QString const &text, bool reset,
                                       bool isData, bool *pDisableTypeahead);
    void resetMessageTransmitQueue();
    QPair<QString, int> popMessageFrame();
    void tryNotify(const QString &key, int submode = -1);
    void processDecodeEvent(JS8::Event::Variant const &);

    void updateCQButtonDisplay();
    void updateHBButtonDisplay();

  protected:
    void keyPressEvent(QKeyEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void childEvent(QChildEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

  private slots:
    // ChunkedArq integration: TX-side relay + RX-side UI rendering.
    void onChunkedWantToTransmit(QString const &text);
    // [TODO.md #57 / build 268] Per-response wrapper for ACK / NACK.
    // Saves the operator's outgoing-box draft text into
    // m_arqResponseSavedText, then delegates to onChunkedWantToTransmit
    // for the actual TX. stopTx() restores the saved text after the
    // ACK/NACK transmission completes.
    void onChunkedWantsResponseTx(QString const &text);
    void onChunkedChunkAdded(QString const &fromCall,
                             QString const &chunkBody,
                             int            chunkId,
                             int            total);
    void onChunkedMessageDelivered(QString const &fromCall,
                                   QString const &toCall,
                                   QString const &assembledBody,
                                   int            msgId);
    void onChunkedSendProgress(QString const &peer, int msgId,
                               int delivered, int total);
    void onChunkedSendComplete(QString const &peer, int msgId,
                               int total, int totalRetries);
    void onChunkedSendFailed(QString const &peer, int msgId,
                             int delivered, int total,
                             int totalRetries,
                             QString const &reason);
    // [TODO #51 2026-06-10 build 235] Restore original outbound body
    // to the outgoing-text widget on terminal-failure paths so the
    // operator can retry without re-typing. Connected to
    // ChunkedArq::Manager::sendRestoreRequested.
    void onChunkedSendRestoreRequested(QString const &body,
                                       QString const &reason);
    void onChunkedMsgDelivered(QString const &peer,
                               QString const &addressee,
                               QString const &body,
                               int            msgId);
    void onChunkedInboxMessageReceived(QString const &fromCall,
                                       QString const &addressee,
                                       QString const &body,
                                       int            msgId);
    void onChunkedRelayMessageReceived(QString const &fromCall,
                                       QString const &body,
                                       int            msgId);
    // [FILE-XFER 2026-06-16 build 276] RX slot for ARQ file-transfer
    // super-messages. body is the assembled "F/V1 <header-b32> <pay-b32>".
    // Slot pops the accept dialog, verifies SHA-256, writes the file
    // to the configured save dir.
    void onChunkedFileMessageReceived(QString const &fromCall,
                                      QString const &body,
                                      int            msgId);
    // TX-side handler for the "Send File…" button.
    void on_sendFileButton_clicked();
    // [BUILD 298] TX-side handler for the "Send using ARQ" menu action.
    // Enables ARQ for this one send, fires the normal Send path, and
    // arranges for ARQ to auto-disable on sendComplete / sendFailed.
    void on_sendUsingArqAction_triggered();
    // [BUILD 331-visibleHail] TX-side handler for the "Send Visible
    // Hail" menu action. Two-cycle sequence: bolt frame (paints
    // ⚡ silhouette on waterfall as Hellschreiber-style raster), one
    // silent cycle gap, then standard Subspace HAIL message
    // (encoded `<mycall>: @ALLCALL ACK`). Chain step 2 (the HAIL TX)
    // is scheduled in the ft2WaveformDone handler when
    // m_visibleHailPendingHail is set.
    void on_sendVisibleHailAction_triggered();
    void restoreVisibleHailSubmodeIfPending();
    void onChunkedProgressUpdate(int chunkId, int total, int retries);
    void onChunkedProgressEnd();

    void initialize_fonts();
    void on_menuModeJS8_aboutToShow();
    void on_menuControl_aboutToShow();
    void on_actionEnable_Monitor_RX_toggled(bool checked);
    void on_actionEnable_Transmitter_TX_toggled(bool checked);
    void on_actionEnable_Reporting_SPOT_toggled(bool checked);
    void on_actionEnable_Tuning_Tone_TUNE_toggled(bool checked);
    void on_menuWindow_aboutToShow();
    void on_actionFocus_Message_Receive_Area_triggered();
    void on_actionFocus_Message_Reply_Area_triggered();
    void on_actionFocus_Band_Activity_Table_triggered();
    void on_actionFocus_Call_Activity_Table_triggered();
    void on_actionClear_All_Activity_triggered();
    void on_actionClear_Band_Activity_triggered();
    void on_actionClear_RX_Activity_triggered();
    void on_actionClear_Call_Activity_triggered();
    void on_actionSetOffset_triggered();
    void on_actionShow_Fullscreen_triggered(bool checked);
    void on_actionShow_Statusbar_triggered(bool checked);
    void on_actionShow_Frequency_Clock_triggered(bool checked);
    void on_actionShow_Band_Activity_triggered(bool checked);
    void on_actionShow_Band_Heartbeats_and_ACKs_triggered(bool checked);
    void on_actionShow_Call_Activity_triggered(bool checked);
    void on_actionShow_Waterfall_triggered(bool checked);
    void on_actionShow_Spots_Map_triggered(bool checked);
    void on_actionShow_Waterfall_Controls_triggered(bool checked);
    void on_actionShow_Waterfall_Time_Drift_Controls_triggered(bool checked);
    void on_actionReset_Window_Sizes_triggered();
    void on_actionSettings_triggered();
    void openSettings(int tab = 0);
    void prepareApi();
    void prepareSpotting();
    void on_spotButton_clicked(bool checked);
    void on_monitorButton_clicked(bool);
    void on_actionAbout_triggered();
    void resetPushButtonToggleText(QPushButton *btn);
    void on_stopTxButton_clicked();
    void on_dialFreqUpButton_clicked();
    void on_dialFreqDownButton_clicked();
    void on_actionAdd_Log_Entry_triggered();
    void on_actionOpen_log_directory_triggered();
    void on_actionCopyright_Notice_triggered();
    void on_actionUser_Guide_triggered();
    bool decode(qint32 k);
    bool isDecodeReady(int submode, qint32 k, qint32 k0,
                       qint32 *pCurrentDecodeStart, qint32 *pNextDecodeStart,
                       qint32 *pStart, qint32 *pSz, qint32 *pCycle);
    bool decodeEnqueueReady(qint32 k, qint32 k0);
    bool decodeEnqueueReadyExperiment(qint32 k, qint32 k0);
    bool decodeProcessQueue(qint32 *pSubmode);
    void decodeStart();
    void decodeBusy(bool b);
    void decodeDone();
    void on_startTxButton_toggled(bool checked);
    void toggleTx(bool start);
    void on_logQSOButton_clicked();
    void on_actionModeJS8HB_toggled(bool checked);
    void on_actionModeJS8Normal_triggered();
    void on_actionModeJS8Fast_triggered();
    void on_actionModeJS8Turbo_triggered();
    void on_actionModeJS8Slow_triggered();
    void on_actionModeJS8Ultra_triggered();
    void on_actionModeFT2_triggered();
    void on_actionHeartbeatAcknowledgements_toggled(bool checked);
    void on_actionModeMultiDecoder_toggled(bool checked);
    void on_actionModeReplicatorProtocol_toggled(bool checked);
    void on_actionModeAutoreply_toggled(bool checked);
    bool canCurrentModeSendHeartbeat() const;
    bool canCurrentModeAckHeartbeat() const;
    void prepareMonitorControls();
    void prepareHeartbeatMode(bool enabled);
    void f11f12(int n);
    void on_actionErase_ALL_TXT_triggered();
    void on_actionErase_js8call_log_adi_triggered();
    void startTx();
    void startTxNonArq();
    void stopTx();
    void stopTx2();
    void buildFrequencyMenu(QMenu *menu);
    void buildHeartbeatMenu(QMenu *menu);
    void buildCQMenu(QMenu *menu);
    void buildRepeatMenu(QMenu *menu, QPushButton *button, bool isLowInterval,
                         int *interval);
    void sendHB();
    void sendHeartbeatAck(QString to, int snr, QString extra);
    void on_hbMacroButton_toggled(bool checked);
    void on_hbMacroButton_clicked();
    void sendCQ(bool repeat = false);
    void on_cqMacroButton_toggled(bool checked);
    void on_cqMacroButton_clicked();
    void on_replyMacroButton_clicked();
    void on_snrMacroButton_clicked();
    void on_infoMacroButton_clicked();
    void on_statusMacroButton_clicked();
    void on_typingMacroButton_clicked();
    void setShowColumn(QString tableKey, QString columnKey, bool value);
    bool showColumn(QString tableKey, QString columnKey, bool default_ = true);
    void buildShowColumnsMenu(QMenu *menu, QString tableKey);
    void setSortBy(QString key, QString value);
    QString getSortBy(QString const &key, QString const &defaultValue) const;
    SortByReverse getSortByReverse(QString const &key,
                                   QString const &defaultValue) const;
    void buildSortByMenu(QMenu *menu, QString key, QString defaultValue,
                         QList<QPair<QString, QString>> values);
    void buildBandActivitySortByMenu(QMenu *menu);
    void buildCallActivitySortByMenu(QMenu *menu);
    void buildQueryMenu(QMenu *, QString callsign);
    QMap<QString, QString> buildMacroValues();
    void buildColumnLabelMap();
    void buildSuggestionsMenu(QMenu *menu, QTextEdit *edit,
                              const QPoint &point);
    void buildSavedMessagesMenu(QMenu *menu);
    void buildRelayMenu(QMenu *menu);
    QAction *buildRelayAction(QString call);
    void buildEditMenu(QMenu *, QTextEdit *);
    void on_queryButton_pressed();
    void on_macrosMacroButton_pressed();
    void on_deselectButton_pressed();
    void on_tableWidgetRXAll_cellClicked(int row, int col);
    void on_tableWidgetRXAll_cellDoubleClicked(int row, int col);
    QString generateCallDetail(QString selectedCall);
    void on_tableWidgetCalls_cellClicked(int row, int col);
    void on_tableWidgetCalls_cellDoubleClicked(int row, int col);
    QList<QPair<QString, int>> buildMessageFrames(QString const &text,
                                                  bool isData,
                                                  bool *pDisableTypeahead);
    bool prepareNextMessageFrame();
    bool isFreqOffsetFree(int f, int bw);
    int findFreeFreqOffset(int fmin, int fmax, int bw);
    void setDrift(int n);
    void matchCallsignFromInput();
    void on_tuneButton_clicked(bool);
    void acceptQSO(QDateTime const &, QString const &call, QString const &grid,
                   Frequency dial_freq, QString const &mode,
                   QString const &submode, QString const &rpt_sent,
                   QString const &rpt_received, QString const &comments,
                   QString const &name, QDateTime const &QSO_date_on,
                   QString const &operator_call, QString const &my_call,
                   QString const &my_grid, QByteArray const &ADIF,
                   QVariantMap const &additionalFields);
    void on_readFreq_clicked();
    void on_outAttenuation_valueChanged(int);
    void rigOpen();
    void handle_transceiver_update(Transceiver::TransceiverState const &);
    void handle_transceiver_failure(QString const &reason);
    void band_changed();
    void monitor(bool);
    void end_tuning();
    void stop_tuning();
    void stopTuneATU();
    void auto_tx_mode(bool);
    void on_monitorButton_toggled(bool checked);
    void on_monitorTxButton_toggled(bool checked);
    void on_tuneButton_toggled(bool checked);
    void on_spotButton_toggled(bool checked);

    void emitPTT(bool on);
    void emitTones();
    void udpNetworkMessage(Message const &message);
    void tcpNetworkMessage(Message const &message);
    void networkMessage(Message const &message);
    bool canSendNetworkMessage();
    void sendNetworkMessage(QString const &type, QString const &message);
    void sendNetworkMessage(QString const &type, QString const &message,
                            const QVariantMap &params);
    void pskReporterError(QString const &);
    void TxAgain();
    void checkVersion(bool alertOnUpToDate);
    void checkStartupWarnings();
    void selectCallsign(QString call, int submode = -1);
    void clearSelection();
    void autoSwitchMode(int submode);
    void clearCallsignSelected();  // legacy — calls clearSelection()
    void refreshTextDisplay();

    void manualBandHop(const StationList::Station station);

  private:
    Q_SIGNAL void apiSetMaxConnections(int n);
    Q_SIGNAL void apiSetServer(QString host, quint16 port);
    Q_SIGNAL void apiStartServer();
    Q_SIGNAL void apiStopServer();

    Q_SIGNAL void aprsClientEnqueueSpot(QString by_call, QString from_call,
                                        QString grid, QString comment);
    Q_SIGNAL void aprsClientEnqueueThirdParty(QString by_call,
                                              QString from_call, QString text);
    /**
     * @brief Send a standard APRS message ACK frame.
     * @param from_call Source callsign for the ACK.
     * @param to_call Destination callsign being acknowledged.
     * @param messageId APRS message identifier to acknowledge.
     */
    Q_SIGNAL void aprsClientEnqueueAck(QString from_call, QString to_call,
                                       QString messageId);
    Q_SIGNAL void aprsClientSetSkipPercent(float skipPercent);
    Q_SIGNAL void aprsClientSetIncomingRelayEnabled(bool enabled);
    Q_SIGNAL void aprsClientSetServer(QString host, quint16 port);
    Q_SIGNAL void aprsClientSetPaused(bool paused);
    Q_SIGNAL void aprsClientSetLocalStation(QString mycall, QString passcode);
    Q_SIGNAL void aprsClientSendReports();

    Q_SIGNAL void pskReporterSendReport(bool);
    Q_SIGNAL void pskReporterSetLocalStation(QString, QString, QString);
    Q_SIGNAL void pskReporterAddRemoteStation(QString, QString,
                                              Radio::Frequency, QString, int,
                                              QDateTime);

    Q_SIGNAL void spotClientSetLocalStation(QString, QString, QString);
    Q_SIGNAL void spotClientEnqueueCmd(QString, QString, QString, QString,
                                       QString, QString, QString, int, int, int,
                                       int);
    Q_SIGNAL void spotClientEnqueueSpot(QString, QString, int, int, int, int);

    Q_SIGNAL void decodedLineReady(QByteArray t);
    Q_SIGNAL void playNotification(const QString &name);
    Q_SIGNAL void initializeNotificationAudioOutputStream(AudioDeviceInfo const &,
                                                          unsigned) const;
    Q_SIGNAL void initializeAudioOutputStream(AudioDeviceInfo, unsigned channels,
                                              unsigned msBuffered) const;
    Q_SIGNAL void stopAudioOutputStream() const;
    Q_SIGNAL void startAudioInputStream(AudioDeviceInfo const &,
                                        int framesPerBuffer, AudioDevice *sink,
                                        AudioDevice::Channel) const;
    Q_SIGNAL void suspendAudioInputStream() const;
    Q_SIGNAL void resumeAudioInputStream() const;
    Q_SIGNAL void startDetector(AudioDevice::Channel) const;
    Q_SIGNAL void FFTSize(unsigned) const;
    Q_SIGNAL void detectorClose() const;
    Q_SIGNAL void finished() const;
    Q_SIGNAL void transmitFrequency(double) const;
    Q_SIGNAL void endTransmitMessage(bool quick = false) const;
    Q_SIGNAL void tune(bool = true) const;
    Q_SIGNAL void sendMessage(double frequency, int submode, double txDelay,
                              SoundOutput *, AudioDevice::Channel) const;
    Q_SIGNAL void outAttenuationChanged(qreal) const;
    Q_SIGNAL void toggleShorthand() const;
    Q_SIGNAL void submodeChanged(Varicode::SubmodeType) const;

    Q_SIGNAL void messageAdded(int) const;

  private:
    QByteArray wisdomFileName() const;

    void writeAllTxt(QStringView message);
    void writeMsgTxt(QStringView message, int snr, int offset);

    void currentTextChanged();
    void tableSelectionChanged(QItemSelection const &, QItemSelection const &);
    void setupJS8();

    int freq() const { return m_freq; }

    /** Sabotage transmission if frequency would result in WSPR band. */
    void refuseToSendIn30mWSPRBand();

    void prepareSending(qint64 nowMS);

    /** Update the clock shown. */
    void updateClockUI(const QDateTime &);

    void setFreq(int);
    void transmit();

    bool presentlyWantHBReplies();

    QString m_nextFreeTextMsg;

    NetworkAccessManager m_network_manager;
    bool m_valid;
    [[maybe_unused]] bool m_multiple; // Used only in Windows builds
    MultiSettings *m_multi_settings;
    QPushButton *m_configurations_button;
    QPushButton *m_modeBtnNormal{nullptr};
    QPushButton *m_modeBtnFast{nullptr};
    QPushButton *m_modeBtnTurbo{nullptr};
    QPushButton *m_modeBtnSlow{nullptr};
    QPushButton *m_modeBtnFT2{nullptr};
    // [BUILD 298] m_arqButton DELETED. The persistent ARQ-on toggle
    // button is gone; ARQ is now opt-in per-message via the Send
    // options menu's "Send using ARQ" action. The internal
    // ui->actionModeReplicatorProtocol QAction still exists and is
    // still the canonical enable flag — the visible button just no
    // longer mirrors it. The "Send using ARQ" handler at
    // on_sendUsingArqAction_triggered toggles the QAction true for
    // the duration of one send, then sendComplete / sendFailed
    // disables it.
    // [FILE-XFER build 280 2026-06-16] Chevron QToolButton glued to the
    // right edge of ui->startTxButton. InstantPopup mode → click opens
    // a menu with "Send file…" (and any future send-action entries).
    // The standalone QPushButton "File" from build 276 is gone — file
    // send now lives inside this menu. Built programmatically in
    // UI_Constructor since the Send button itself comes from the .ui.
    QToolButton *m_sendMenuButton{nullptr};
    // [FILE-XFER build 282] The "Send file…" action inside
    // m_sendMenuButton's dropdown. Held as a member so the
    // updateTextDisplay / updateTxButtonDisplay paths can toggle its
    // enabled state — the chevron button itself stays enabled full-
    // time for discoverability; gating lives on the action.
    QAction *m_sendFileAction{nullptr};
    // [BUILD 298] "Send using ARQ" menu action — first item in the
    // Send options menu, replaces the standalone ARQ-toggle button.
    // Enable state is updated live from updateButtonDisplay using
    // evaluateArqGateForText.
    QAction *m_sendArqAction{nullptr};
    // [BUILD 331-visibleHail] "Send Visible Hail" menu action — third
    // item. One composite transmission: the encoded Subspace HAIL
    // frame (`<mycall>: @ALLCALL ACK`, 2.52 s) followed back-to-back
    // by two painted ⚡ diag frames (Hellschreiber-style audio
    // raster, 3.0 s each) — ~8.8 s total under a single PTT cycle.
    // The encoded ID + the visual together announce the operator's
    // presence to BOTH software-decoder AND eyeballs-on-waterfall
    // peers.
    QAction *m_sendVisibleHailAction{nullptr};
    // [BUILD 336 TODO #94] Visible Hail is ONE transmission: the
    // encoded HAIL frame and both diag bolts are concatenated into a
    // single composite waveform staged via the Modulator's full-frame
    // override, played under a single PTT key/unkey. The former
    // 3-step state machine (per-frame PTT re-key advanced from
    // stopTx side effects) lost frames when CAT-port contention
    // swallowed a re-key on slow machines. This flag is the sequence
    // lifecycle: set when the hail TX is armed, cleared in stopTx()
    // (or operator Halt). Guards the remote AVHAIL? trigger and the
    // menu-enable gate against double-fire while the TX is pending.
    bool m_visibleHailActive{false};
    // [BUILD 336 TODO #87] Submode to restore after a REMOTE-triggered
    // AVHAIL? completes (the trigger switches the receiver to Subspace
    // to transmit the hail; the operator's original mode speed comes
    // back afterward). -1 = no restore pending (manual menu hails
    // never set this — the operator chose Subspace deliberately).
    int m_visibleHailRestoreSubmode{-1};
    // [BUILD 336] Click-to-call: seed the outgoing box with
    // "<CALL> <standard greeting>" (Configuration::standard_greeting,
    // macros substituted). Shared by the Spots Map dot click and the
    // waterfall callsign-label double-click; callers translate the
    // result into their own feedback (toast / status message).
    // NOTE: plain private members, NOT in a slots section — moc
    // cannot parse an enum declaration there.
    enum class GreetingSeedResult { Seeded, DraftBlocked, InvalidCall };
    GreetingSeedResult trySeedOutgoingGreeting(QString const &call);
    // [BUILD 336 TODO #97] Wall-clock ms of the last accepted AVHAIL?
    // remote trigger — global once-per-hour rate limit / replay guard
    // (the protocol has no anti-replay; a replayed trigger must not
    // chain-key the station). 0 = never triggered this session.
    qint64 m_lastAvhailResponseMs{0};
    // [BUILD 298] When set non-zero, an in-flight chunked-ARQ send
    // was initiated by "Send using ARQ" (text), as opposed to
    // "Send file…" (file transfer). sendComplete / sendFailed on a
    // matching msgId disables ARQ. Independent of m_fileSendMsgId
    // which serves the same role for file sends.
    int  m_arqTextSendMsgId{0};
    // [FILE-XFER build 282] ARQ auto-enable bookkeeping. When the
    // operator picks "Send file…" and ARQ was off, we flip it on for
    // the duration of the transfer and restore the prior state on
    // sendComplete / sendFailed. Match is by msgId — a non-zero
    // m_fileSendMsgId means we're holding ARQ open for that send.
    int  m_fileSendMsgId{0};
    bool m_arqStateBeforeFileSend{false};
    QSettings *m_settings;
    bool m_settings_read;
    QScopedPointer<Ui::UI_Constructor> ui;

    // other windows
    Configuration m_config;
    JS8MessageBox m_rigErrorMessageBox;

    QDockWidget* messageDock_ = nullptr;
    MessagePanel* messagePanel_ = nullptr;
    QDialog* messageFloatDialog_ = nullptr;

    enum class MessageHost { Dock, Dialog };
    MessageHost messageHost_ = MessageHost::Dock;

    QScopedPointer<WideGraph> m_wideGraph;
    QScopedPointer<LogQSO> m_logDlg;
    QScopedPointer<SpotMapWindow> m_spotMapWindow; // "Spots Map" view
    QScopedPointer<HelpTextWindow> m_shortcuts;
    QScopedPointer<HelpTextWindow> m_prefixes;
    QScopedPointer<HelpTextWindow> m_mouseCmnds;

    Transceiver::TransceiverState m_rigState;
    Frequency m_lastDialFreq = 0;
    QString m_lastBand;

    Detector *m_detector;
    unsigned m_FFTSize = 0;
    SoundInput *m_soundInput;
    Modulator *m_modulator;
    SoundOutput *m_soundOutput;
    NotificationAudio *m_notification;

    // Configuration might one day offer to send a txDelayChanged signal.
    // As long as it doesn't, we poll and compare with the previous value.
    double m_TxDelay = 0.0; // in seconds.

    TxLoop *m_cq_loop;
    TxLoop *m_hb_loop;

    QThread m_networkThread;
    QThread m_audioThread;
    QThread m_notificationAudioThread;
    JS8::Decoder m_decoder;

    qint64 m_secBandChanged = 0;

    Frequency m_freqNominal = 0;
    Frequency m_freqTxNominal = 0;

    int m_freq = 0;

    qint32 m_XIT = 0;
    qint32 m_sec0 = 0;
    qint32 m_RxLog = 0;
    qint32 m_nutc0 = 0;
    // The period of the current submode, in seconds. (15 for normal, 10 for
    // fast, ...)
    qint32 m_TRperiod = 15;
    qint32 m_inGain = 0;
    qint32 m_idleMinutes = 0;
    qint32 m_nSubMode = 0;
    qint32 m_prevStandardSubmode = 0;  // saved standard mode for click-to-switch
    bool m_submodeChanging{false}; // guard against recursive mode switch
    FrequencyList_v3::const_iterator m_frequency_list_fcal_iter;
    qint32 m_i3bit = 0;

    bool m_btxok = false; // True if OK to transmit
    bool m_decoderBusy = false;
    QString m_decoderBusyBand;
    QMap<qint32, qint32>
        m_lastDecodeStartMap; // submode, decode k start position
    Radio::Frequency m_decoderBusyFreq = 0;
    QDateTime m_decoderBusyStartTime;
    bool m_auto = false;
    bool m_restart = false;
    bool m_bDecoded = false;
    int m_currentMessageType = 0;
    QString m_currentMessage;
    int m_currentMessageBits = 0;
    int m_lastMessageType = 0;
    QString m_lastMessageSent;
    bool m_tuneup = false;
    bool m_isTimeToSend = false;

    int m_ihsym = 0;
    float m_px = 0.0f;
    float m_pxmax = 0.0f;
    float m_df3 = 0.0f;
    quint32 m_iptt = 0;
    quint32 m_iptt0 = 0;
    bool m_btxok0 = false;
    double m_onAirFreq0 = 0.0;
    bool m_first_error = false;

    char m_msg[100][80];

    // labels in status bar
    QLabel tx_status_label;
    QLabel config_label;
    QLabel mode_label;
    QLabel last_tx_label;
    QLabel auto_tx_label;
    QProgressBar progressBar;
    QLabel wpm_label;

    // QPointer<QProcess> proc_js8;

    QTimer m_guiTimer;
    // Timer to switch off PTT after end of transmission.
    QTimer pttReleaseTimer;
    QTimer logQSOTimer;
    QTimer tuneButtonTimer;
    QTimer tuneATU_Timer;
    QTimer TxAgainTimer;
    QTimer minuteTimer;
    QString m_baseCall;
    QString m_hisCall;
    QString m_hisGrid;
    QString m_appDir;
    QString m_palette;
    QString m_rptSent;
    QString m_rptRcvd;
    QString m_msgSent0;
    QString m_opCall;

    struct CallDetail {
        QString call;
        QString through;
        QString grid;
        int dial;
        int offset;
        QDateTime cqTimestamp;
        QDateTime ackTimestamp;
        QDateTime utcTimestamp;
        int snr;
        int bits;
        float tdrift;
        int submode;
    };

    struct CommandDetail {
        bool isCompound;
        bool isBuffered;
        QString from;
        QString to;
        QString cmd;
        int dial;
        int offset;
        QDateTime utcTimestamp;
        int snr;
        int bits;
        QString grid;
        QString text;
        QString extra;
        float tdrift;
        int submode;
        QString relayPath;
    };

    struct ActivityDetail {
        bool isLowConfidence;
        bool isCompound;
        bool isDirected;
        bool isBuffered;
        int bits;
        int dial;
        int offset;
        QString text;
        QDateTime utcTimestamp;
        int snr;
        bool snrSuspect{false}; // true when single frame in buffer (noisy SNR estimate)
        bool shouldDisplay;
        float tdrift;
        int submode;
        // [BUILD 294] Absolute global sample-buffer position where
        // this frame's Costas was found (Subspace async decoder).
        // Sort key for processBufferedActivity assembly. 0 = not
        // populated (e.g., standard period-aligned decoder doesn't
        // set it). See Event::Decoded::absPos for derivation.
        std::int64_t absPos{0};
    };

    struct MessageBuffer {
        CommandDetail cmd;
        QQueue<CallDetail> compound;
        QList<ActivityDetail> msgs;
    };

    QString m_selectedCallsign;
    int m_bandActivityWidth;
    int m_callActivityWidth;
    int m_textActivityWidth;
    int m_waterfallHeight;
    bool m_bandActivityWasVisible;
    bool m_rxDirty;
    bool m_rxDisplayDirty = false;
    // Counter bumped in logCallActivity on every insert/update. The
    // once-per-second tick compares against m_callActivityRenderedVersion
    // and only does a full displayCallActivity rebuild when they differ —
    // otherwise it just rewrites the Age column in place. Keeps the
    // right pane ticking without burning Windows CPU on a full sort +
    // QTableWidget repopulate every second.
    int m_callActivityVersion = 0;
    int m_callActivityRenderedVersion = 0;
    int m_txFrameCountEstimate = 0;
    int m_txFrameCount = 0;
    int m_txFrameCountSent = 0;
    QTimer m_txTextDirtyDebounce;
    bool m_txTextDirty;
    QString m_txTextDirtyLastText;
    QString m_txTextDirtyLastSelectedCall;
    QString m_lastTxMessage;
    QString m_totalTxMessage;
    // [QUEUE PROVENANCE 2026-06-10 build 247]
    // Snapshot of extFreeTextMsgEdit's plain-text content RIGHT AFTER
    // processTxQueue called addMessageText to inject a system-built
    // reply (autoreply / relay forward / TCP API send). Used by startTx
    // to decide whether the current Send-button click is on operator-
    // typed text or queue-injected text. Matches the Build 205 design:
    // queue-injected text NEVER gets ARQ-wrapped, regardless of body
    // shape. Cleared at TX completion or once the operator edits the
    // text away from the snapshot.
    QString m_lastQueueInjectedText;
    // [STATUS-BAR ARQ PROGRESS 2026-06-11 build 252]
    // Cache of the real "Last Tx: <message>" text in the leftmost
    // status-bar widget (last_tx_label), captured on the FIRST
    // onChunkedProgressUpdate of an ARQ session so we can restore it
    // when the session ends. Empty when no ARQ session is in flight.
    QString m_lastTxLabelCache;
    bool    m_lastTxLabelCacheValid{false};
    // [TODO.md #57 build 268] Per-response outgoing-text preservation.
    // Saved on the wantsResponseTx slot before the ACK / NACK text
    // gets written into the outgoing-msg widget; restored in stopTx()
    // once the response TX completes. Empty + flag-false when no
    // restore is pending.
    QString m_arqResponseSavedText;
    bool    m_arqResponseRestorePending{false};
    // [TODO #73 build 312] Pre-auto-switch submode, stashed when
    // the arq-modeFollow logic at processCommandActivity.cpp auto-
    // switches the receiver into the sender's mode for an inbound
    // chunk. Restored unconditionally 750 ms after the ACK / NACK
    // transmits (same hook as m_arqResponseSavedText). The speed
    // buttons are disabled during TX so the operator can't race a
    // manual mode change in. -1 = no stash pending.
    int     m_arqPreSwitchSubmode{-1};
    // [TODO.md #58 build 268] Multi-mode RX runtime override.
    // Set true (set-once / sticky for the program run) when the ARQ
    // button is toggled on OR when the first inbound ARQ chunk is
    // auto-detected. Once true, never cleared until program exit.
    // Read by the legacy-decoder dispatch gate (mainwindow.cpp around
    // line 1891) to force FT2/Subspace decoding even when the
    // operator is in a legacy submode and actionModeMultiDecoder is
    // unchecked. Configuration / QSettings is never written.
    bool    m_arqMultiModeOverride{false};

    // moved from mainwindow.cpp, is used in multiple functions
    QString since(QDateTime const &time) {
        auto const delta = time.secsTo(DriftingDateTime::currentDateTimeUtc());

        if (delta >= 60 * 60 * 24)
            return QString("%1d").arg(delta / (60 * 60 * 24));
        else if (delta >= 60 * 60)
            return QString("%1h").arg(delta / (60 * 60));
        else if (delta >= 60)
            return QString("%1m").arg(delta / (60));
        else if (delta >= 15)
            return QString("%1s").arg(delta - (delta % 15));
        else
            return QString("now");
    }

    QDateTime m_lastTxStartTime;
    QDateTime m_lastTxStopTime;
    QDateTime m_txQueueStartTime;  // wall-clock when first frame TX began (for countdown)
    // moved from mainwindow.cpp, is used in multiple functions
    auto replaceMacros(QString const &text,
                       QMap<QString, QString> const &values, bool const prune) {
        QString output = text;

        for (auto const [key, value] : values.asKeyValueRange()) {
            output = output.replace(key, value.toUpper());
        }

        return prune ? output.replace(QRegularExpression("[<](?:[^>]+)[>]"), "")
                     : output;
    }

    QList<int> generateOffsets(int minOffset, int maxOffset) {
        QList<int> offsets;

        // TODO: these offsets aren't ordered correctly...

        for (int i = minOffset; i <= maxOffset; i++) {
            offsets.append(i);
        }
        return offsets;
    }

    enum Priority {
        PriorityLow = 10,
        PriorityNormal = 100,
        PriorityHigh = 1000
    };

    struct PrioritizedMessage {
        QDateTime date;
        int priority;
        QString message;
        int offset;
        Callback callback;

        friend bool operator<(PrioritizedMessage const &a,
                              PrioritizedMessage const &b) {
            if (a.priority < b.priority) {
                return true;
            }
            return a.date < b.date;
        }
    };

    struct CachedDirectedType {
        bool isAllcall;
        QDateTime date;
    };

    struct DecodeParams {
        int submode;
        int start;
        int sz;
    };

    struct FrameCacheKey {
        int submode;
        QString frame;

        FrameCacheKey(int submode, QString frame)
            : submode(submode), frame(std::move(frame)) {}

        bool operator==(FrameCacheKey const &) const noexcept = default;

        struct Hash {
            std::size_t operator()(FrameCacheKey const &key) const noexcept {
                std::size_t const h1 = std::hash<int>{}(key.submode);
                std::size_t const h2 = qHash(key.frame);
                return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
            }
        };
    };

    // [POS-DEDUP 2026-07-14 TODO #72] Dedup identity is the monotonic
    // L2 buffer position (absPos), per the design intent of the
    // monotonic ring counter. Each cache entry keeps the last few
    // occurrences of a frame's bits: a new decode is a duplicate IFF
    // its absPos falls within one frame-length of a stored occurrence
    // (same physical transmission re-decoded from the ring). Same
    // bits at a distant position = a genuinely new transmission (ARQ
    // chunks all share a bit-identical first frame: "K9AVT: WM8Q " is
    // exactly one frame) and must pass regardless of arrival time.
    // Frames without absPos (standard period decoder) fall back to
    // the legacy time-window check via `when`.
    struct FrameOccurrence {
        QDateTime    when;
        std::int64_t absPos;   // 0 = unknown (standard decoder)
    };
    struct FrameCacheEntry {
        static constexpr int MAX_OCC = 3;
        FrameOccurrence occ[MAX_OCC];
        int n = 0;
        void add(FrameOccurrence const &o) {
            // keep the newest MAX_OCC occurrences (shift-down ring)
            for (int i = std::min(n, MAX_OCC - 1); i > 0; --i)
                occ[i] = occ[i - 1];
            occ[0] = o;
            n = std::min(n + 1, MAX_OCC);
        }
    };
    using FrameCache =
        std::unordered_map<FrameCacheKey, FrameCacheEntry,
                           FrameCacheKey::Hash>;
    using BandActivity = QMap<int, QList<ActivityDetail>>;

    QQueue<DecodeParams> m_decoderQueue;
    FrameCache m_messageDupeCache;  // submode, frame -> date seen
    QVariantMap m_showColumnsCache; // table column:key -> show boolean
    QVariantMap m_sortCache;        // table key -> sort by
    QPriorityQueue<PrioritizedMessage> m_txMessageQueue; // messages to be sent
    QQueue<QPair<QString, int>> m_txFrameQueue;          // frames to be sent
    QQueue<ActivityDetail> m_rxActivityQueue; // all rx activity queue
    QQueue<CommandDetail>
        m_rxCommandQueue; // command queue for processing commands
    QQueue<CallDetail>
        m_rxCallQueue; // call detail queue for spots to pskreporter
    QMap<QString, QString>
        m_compoundCallCache; // base callsign -> compound callsign
    QCache<QString, QDateTime> m_txAllcallCommandCache; // callsign -> last tx
    QCache<int, QDateTime> m_rxRecentCache;             // freq -> last rx
    QCache<int, CachedDirectedType>
        m_rxDirectedCache;                // freq -> last directed rx
    QCache<QString, int> m_rxCallCache;   // call -> last freq seen
    QMap<int, int> m_rxFrameBlockNumbers; // freq -> block
    BandActivity m_bandActivity; // freq -> [(text, last timestamp), ...]
    QMap<int, MessageBuffer> m_messageBuffer; // freq -> (cmd, [frames, ...])
    int m_lastClosedMessageBufferOffset;
    QMap<QString, CallDetail>
        m_callActivity; // call -> (last freq, last timestamp)

    QMap<int, QString> m_origRxHeaderLabelMap;           // colIndex, label
    QMap<int, QString> m_origCallActivityHeaderLabelMap; // colIndex, label
    QMap<QString, QString> m_columnLabelMap;             // full, minimal

    QMap<QString, QSet<QString>>
        m_heardGraphOutgoing; // callsign -> [stations who've this callsign has
                              // heard]
    QMap<QString, QSet<QString>>
        m_heardGraphIncoming; // callsign -> [stations who've heard this
                              // callsign]

    QMap<QString, int> m_rxInboxCountCache; // call -> count


#ifdef JS8_ENABLE_FT2
    // L2 async decode infrastructure
    QTimer m_l2DecodeTimer;                     // fires every 750ms
    QFutureWatcher<void> m_l2DecodeWatcher;     // monitors async decode
    std::int16_t m_l2RingBuf[FT2_L2_RINGSIZE] = {};  // 7.5s ring buffer (90000 samples, 2 periods)
    // Write position as a monotonic 64-bit sample counter. Indexing into
    // the ring uses (pos % FT2_L2_RINGSIZE). Prior impl wrapped at 180000
    // back to 90000, which made known-frame expiration unreliable: if the
    // wrap happened between two expiration checks (every ~380 ms), the
    // next check saw a small positive delta and concluded the entry was
    // fresh -- losing track of how many full cycles had elapsed. A
    // monotonic counter makes subtraction unambiguous and expiration
    // correct at any sampling cadence.
    std::atomic<std::int64_t> m_l2RingPos{0};   // monotonic sample count (audio thread writes, main thread reads)
    bool m_l2Decoding = false;                  // decode in progress
    bool m_l2Enabled = false;                   // L2 decode active
    qint64 m_l2DecodeFinishedMs = 0;            // timestamp when decode thread finished
    void l2DecodeDone();                        // called when async decode finishes
    void l2TryDecode(char const *source);       // attempt to start an L2 decode

    // L2 known-frame suppression: pass last decoded frame's raw bits to
    // the decoder so it skips re-decoding the same content
    std::int8_t m_l2KnownBits[77 * 20] = {};   // known frames' raw bits
    std::int64_t m_l2KnownPos[20] = {};         // monotonic pos when each frame was added
    int m_l2NKnown = 0;                         // number of known frames
    int m_l2SignalFreq = 0;                     // last decoded signal freq (Hz), 0=unknown
    QMap<int, int> m_ft2StdSnr;                  // standard decoder SNR cache: freq/10 → SNR
    int m_l2EmptyCycles = 0;                    // consecutive empty decode cycles

    // L2 deduplication (5s window, best SNR wins)
    struct L2DedupeEntry { int snr; qint64 msec; };
    QMap<QString, L2DedupeEntry> m_l2Dedup;
    qint64 m_l2DedupLastPurge = 0;
#endif

    QMap<QString, QMap<QString, CallDetail>>
        m_callActivityBandCache; // band -> call activity
    QMap<QString, QMap<int, QList<ActivityDetail>>>
        m_bandActivityBandCache;              // band -> band activity
    QMap<QString, QString> m_rxTextBandCache; // band -> rx text
    QMap<QString, QMap<QString, QSet<QString>>>
        m_heardGraphOutgoingBandCache; // band -> heard in
    QMap<QString, QMap<QString, QSet<QString>>>
        m_heardGraphIncomingBandCache; // band -> heard out

    QMap<QString, QDateTime>
        m_callSelectedTime; // call -> timestamp when callsign was last selected
    /**
     * Cache of recently seen APRS relay keys used to suppress duplicates.
     * Key format: "TO|TEXT|SENDER" (uppercased), value is last seen UTC.
     */
    QHash<QString, QDateTime> m_aprsRelayDedupCache;
    QSet<QString> m_callSeenHeartbeat; // call
    int m_previousFreq;
    bool m_shouldRestoreFreq = false;
    bool m_bandHopped = false;
    Frequency m_bandHoppedFreq = 0;

    /** Repeat period of HBs, in seconds. */
    int m_hbInterval;
    /** Repeat period of CQ calls, in seconds. */
    int m_cqInterval;

    /** Whether to resume HBs at the next opportunity. */
    bool m_hbPaused;
    /** Whether the current mode supports heartbeats. */
    bool m_hbModeAvailable{true};

    QDateTime m_dateTimeQSOOn;
    QDateTime m_dateTimeLastTX;

    LogBook m_logBook;
    unsigned m_msAudioOutputBuffered;
    unsigned m_framesAudioInputBuffered = 0;
    QThread::Priority m_audioThreadPriority = QThread::HighPriority;
    QThread::Priority m_notificationAudioThreadPriority = QThread::LowPriority;
    QThread::Priority m_decoderThreadPriority = QThread::HighPriority;
    QThread::Priority m_networkThreadPriority = QThread::LowPriority;
    bool m_splitMode = false;
    bool m_monitoring = false;
    bool m_generateAudioWhenPttConfirmedByTX = false;
    bool m_transmitting = false;
    bool m_tune = false;
    bool m_tx_watchdog = false; // true when watchdog triggered
    bool m_block_pwr_tooltip = false;
    bool m_PwrBandSetOK = true;
    Frequency m_lastMonitoredFrequency = 0;
    MessageClient *m_messageClient;
    MessageServer *m_messageServer;
    ChunkedArq::Manager *m_chunkedArq{nullptr};
    WSJTXMessageClient *m_wsjtxMessageClient;
    WSJTXMessageMapper *m_wsjtxMessageMapper;
    TCPClient *m_n3fjpClient;
    PSKReporter *m_pskReporter;
    SpotClient *m_spotClient;
    APRSISClient *m_aprsClient;
    AprsInboundRelay *m_aprsInboundRelay;
    QVariantHash m_pwrBandTxMemory; // Remembers power level by band
    QVariantHash
        m_pwrBandTuneMemory; // Remembers power level by band for tuning
    QByteArray m_geometryNoControls;

    //---------------------------------------------------- private functions
    void readSettings();
    void set_application_font(QFont const &);
    void writeSettings();
    void createStatusBar();
    void statusChanged();
    void rigFailure(QString const &reason);
    void spotSetLocal();
    void pskSetLocal();
    void aprsSetLocal();
    void spotReport(int submode, int dial, int offset, int snr,
                    QString const &callsign, QString const &grid);
    void spotCmd(CommandDetail const &cmd);
    void spotAprsCmd(CommandDetail const &cmd);
    void pskLogReport(QString const &mode, int dial, int offset, int snr,
                      QString const &callsign, QString const &grid,
                      QDateTime const &utcTimestamp);
    void spotAprsGrid(int dial, int offset, int snr, QString callsign,
                      QString grid);
    Radio::Frequency dialFrequency();
    void setSubmode(int submode);     // full reconfiguration (radio, FFT, tables)
    // switchSubmode() removed — use setSubmode() for all mode changes
    void updateCurrentBand();
    void displayDialFrequency();
    void transmitDisplay(bool);
    void postDecode(bool is_new, QString const &message);
    void displayTransmit();
    void updateModeButtonText();
    void updateButtonDisplay();
    // [TODO.md #67 build 272] Tri-state for the ARQ button's live
    // armed/not-armed visual, mirroring the full TX-time gate logic
    // at on_startTxButton_clicked (mainwindow.cpp ~3485-3629).
    enum class ArqGateState {
        NotArmed_NoPeer,       // no valid peer from selection or text
        NotArmed_DirectedCmd,  // packs as directed cmd (excl MSG/MSG TO:/relay)
        Armed                  // would go via ARQ on the next Send
    };
    ArqGateState evaluateArqGateForText(QString const &text) const;
    void updateTextDisplay();
    void updateTextWordCheckerDisplay();
    void updateTextStatsDisplay(QString text, int count);
    void updateTxButtonDisplay();
    bool isMyCallIncluded(QString const &text);
    bool isAllCallIncluded(QString const &text);
    bool isGroupCallIncluded(const QString &text);
    QString callsignSelected(bool useInputText = false);
    bool isRecentOffset(int submode, int offset);
    void markOffsetRecent(int offset);
    bool isDirectedOffset(int offset, bool *pIsAllCall);
    void markOffsetDirected(int offset, bool isAllCall);
    void clearOffsetDirected(int offset);
    void processActivity(bool force = false);
    void processRxActivity();
    void processIdleActivity();
    void processCompoundActivity();
    void processBufferedActivity();
    void processCommandActivity();
    QString inboxPath();
    void refreshInboxCounts();
    bool hasMessageHistory(QString call);
    int addCommandToMyInbox(CommandDetail d);
    int addCommandToStorage(QString type, CommandDetail d);
    int getNextMessageIdForCallsign(QString callsign);
    int getLookaheadMessageIdForCallsign(QString callsign, int afterMsgId);
    int getNextGroupMessageIdForCallsign(QString group_name, QString callsign);
    int getLookaheadGroupMessageIdForCallsign(QString group_name,
                                              QString callsign, int afterMsgId);
    int countUnreadForCallsign(const QString &callsign);
    int countGroupUnreadForCallsign(const QString &group_name,
                                    const QString &callsign);
    bool markGroupMsgDeliveredForCallsign(int msgId, QString callsign);
    bool markMsgDelivered(int mid, Message msg);
    QStringList parseRelayPathCallsigns(QString from, QString text);
    void processSpots();
    void processTxQueue();
    void displayActivity(bool force = false);
    void displayBandActivity();
    void displayCallActivity();
    // Cheap per-second Age-only refresh of the callsign list. Walks
    // existing rows and rewrites the Age cell in place — no sort, no
    // allocations, no table rebuild. Used when m_callActivityVersion
    // has NOT advanced since the last full render.
    void refreshCallActivityAgeOnly();
    void enable_DXCC_entity(bool on);
    void setRig(Frequency = 0); // zero frequency means no change
    QDateTime nextTransmitCycle();
    void statusUpdate();
    void on_the_minute();
    void tryBandHop();
    void add_child_to_event_filter(QObject *);
    void remove_child_from_event_filter(QObject *);
    void setup_status_bar();
    QString columnLabel(QString defaultLabel);
    void ensureMessageDock();

    void resetIdleTimer();
    void incrementIdleTimer();
    void tx_watchdog(bool triggered);
    void write_frequency_entry(QString const &file_name);
    void write_transmit_entry(QString const &file_name);
};

#endif // MAINWINDOW_H
