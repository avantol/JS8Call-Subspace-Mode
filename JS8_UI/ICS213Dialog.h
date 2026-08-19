#ifndef ICS213_DIALOG_HPP__
#define ICS213_DIALOG_HPP__

/**
 * @file ICS213Dialog.h
 * @brief "Send ICS-213 Form" compose dialog (plan approved 2026-08-18).
 *
 * Replaces the file picker of an ordinary ARQ file transfer with a
 * form-entry dialog — NOTHING else differs from "Send file…": the
 * composed form is written as a plain file and handed to the existing
 * transfer path at whatever level the peers negotiate (V1-compatible
 * by design; no new wire format).
 *
 * Behaviors:
 *  - Draft autosave (debounced) to <saveDir>/ICS213/ics213_draft.json;
 *    resumed on reopen; cleared on successful send/save. Cancel keeps
 *    the draft (that's the interrupt case).
 *  - Yearly serial WM8Q-0001 style (QSettings "ICS213" group),
 *    assigned at send/save, never at open.
 *  - Three selectable renders (combo persisted as the default):
 *    Standard numbered form layout / Compact KEY: value / Winlink-
 *    compatible RMS_Express_Form XML.
 *  - Live character count and rough airtime estimate at the current
 *    speed (callback-supplied).
 */

#include <QDialog>
#include <QDir>
#include <QTimer>
#include <functional>

class QSettings;

namespace Ui {
class ICS213Dialog;
}

class ICS213Dialog final : public QDialog {
    Q_OBJECT

  public:
    // airtimeSecs: rough seconds-on-air estimate for N payload chars
    // at the CURRENT speed (supplied by the main window so the dialog
    // stays ignorant of modem details).
    ICS213Dialog(QSettings *settings, QString myCall, QDir saveDir,
                 std::function<double(int)> airtimeSecs,
                 QWidget *parent = nullptr);
    ~ICS213Dialog() override;

    // Path of the file written by the last successful Send/Save.
    QString writtenFilePath() const { return m_writtenPath; }

    // Switch to REPLY mode: re-fill fields 1-8 (read-only) from a
    // RECEIVED form file (well-formed — we authored the renders),
    // show the 9/10 reply blocks, swap the button set to
    // "Send via ARQ" / "Cancel". Returns false (with a warning
    // shown) if the file can't be read. Same machine, new phase —
    // no separate reply dialog class.
    bool enterReplyMode(QString const &receivedPath,
                        QString const &fromCall);

    // ONE authority for "can we interpret this received form?" —
    // returns the detected render format (0 standard / 1 compact /
    // 2 winlink-xml) or -1 if the essential fields aren't
    // recognizable. The receive prompt uses it to gate its Reply
    // button; enterReplyMode uses it to pick the parser.
    static int probeFormat(QByteArray const &bytes);

    // Sparse reply wire file ("FORM: ICS-213-REPLY/1"): the reply
    // sends ONLY the new 9/10 data; the original sender rejoins it
    // with their retained copy of the form (airtime optimization,
    // operator decision 2026-08-18).
    static bool isSparseReply(QByteArray const &bytes);
    // Merge a sparse reply into the ORIGINAL form's content. Returns
    // the completed form, or an empty string if the original doesn't
    // match the expected shape (caller keeps the sparse file).
    static QString mergeReply(QString const &originalContent,
                              QString const &sparseContent);

    // [ARQ level 4] Level-aware WIRE shape. The LOCAL file is always
    // the complete traditional document; a level-4 peer gets a
    // trimmed copy (empty 9/10 section stripped) written to a temp
    // dir under the SAME filename. Returns the temp path, or
    // fullPath unchanged when the section isn't present (Compact/
    // XML/already-trimmed) or the copy can't be written.
    static QString writeTrimmedWireCopy(QString const &fullPath);
    // Sparse wire packet path written by the last reply Send (empty
    // in compose mode) — dispatched to level-4 original senders.
    QString sparseWirePath() const { return m_sparseWirePath; }

    // THE one save-as-draft exit: "Save as draft" button, Esc, and
    // the window X all land here (QDialog::closeEvent → reject).
    // Flushes the draft immediately — the debounce can't eat
    // last-second keystrokes on ANY way out.
    void reject() override;

  public slots:
    // Speed changed on the main screen — recompute the airtime line.
    void refreshEstimate();
    // ARQ send in progress on the main screen: Send disabled, the
    // "(ARQ in progress)" label shown. Cleared on completion.
    void setArqBusy(bool busy);

  signals:
    // Emitted on "Send": files are written; the main window hands
    // them to the ARQ transfer path, choosing the wire shape by the
    // peer's ARQ level. sparsePath is EMPTY for compose sends; for
    // replies it names the sparse packet (level-4 wire), while
    // filePath names the complete filled form (level-3 wire and the
    // local archive either way).
    void sendRequested(QString const &filePath,
                       QString const &sparsePath);

  private slots:
    void onFieldChanged();
    void onSendClicked();
    void onSaveClicked();
    void onClearClicked();

  private:
    QString renderStandard(QString const &serial) const;
    QString renderReplySparse() const;
    void parseStandard(QString const &content);
    void parseCompact(QString const &content);
    void parseWinlinkXml(QString const &content);
    QString renderCompact(QString const &serial) const;
    QString renderWinlinkXml(QString const &serial) const;
    QString nextSerial(); // bumps + persists (yearly reset)
    QString peekSerial() const; // what nextSerial() WILL return
    bool validateRequired();
    bool writeFormFile(QString *outPath); // render + write, serialised
    void saveDraft();
    void loadDraft();
    void clearDraft();
    void updateFooter();
    QString draftPath() const;
    QDir formDir() const; // <saveDir>/ICS213 (created on demand)

    Ui::ICS213Dialog *ui;
    bool m_replyMode{false};
    int m_replyFormat{0};        // format of the RECEIVED form
    QString m_replySerial;       // original form's serial (filename)
    QString m_replySenderCall;   // original sender (parsed / peer)
    QString m_replySourcePath;   // received file (already saved)
    QSettings *m_settings;
    QString m_myCall;
    QDir m_saveDir;
    std::function<double(int)> m_airtimeSecs;
    QTimer m_draftTimer;
    QString m_writtenPath;
    QString m_sparseWirePath; // reply mode: sparse packet (temp dir)
};

#endif
