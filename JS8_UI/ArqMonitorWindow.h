#ifndef ARQ_MONITOR_WINDOW_HPP__
#define ARQ_MONITOR_WINDOW_HPP__

/**
 * @file ArqMonitorWindow.h
 * @brief [TODO #153] Free-floating window over ArqMonitor: status,
 * progress, and content of OVERHEARD ARQ transfers, with manual Save.
 *
 * The window IS the monitoring switch (operator decision 2026-08-19):
 * showEvent activates the assembler, close deactivates it and drops
 * sessions — default OFF with zero background state. Persistence
 * follows the SpotMapWindow seams exactly: geometry + WindowVisible
 * via the visrace tracked flag (showEvent / userClose() /
 * spontaneous-closeEvent), saved in the main window's writeSettings.
 */

#include <QWidget>

class QSettings;
class QTableWidget;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class ArqMonitor;

class ArqMonitorWindow final : public QWidget {
    Q_OBJECT

  public:
    ArqMonitorWindow(QSettings *settings, ArqMonitor *monitor,
                     QWidget *parent = nullptr);

    bool wasVisibleAtShutdown() const { return m_restoreVisible; }
    void saveSettings();
    // Menu-toggle close = user intent: clear the restore flag, close.
    void userClose();

  signals:
    void closed();

  protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

  private:
    void refresh();
    void showSelected();
    void saveSelected();
    int selectedSessionId() const;

    QSettings *m_settings;
    ArqMonitor *m_monitor;
    QTableWidget *m_table;
    QPlainTextEdit *m_content;
    QPushButton *m_saveBtn;
    QLabel *m_headline;
    bool m_restoreVisible{false};
};

#endif
