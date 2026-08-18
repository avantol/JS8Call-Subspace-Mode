/**
 * @file ICS213Dialog.cpp
 * @brief See header. Design provenance: operator-approved plan
 * 2026-08-18; standard-layout render mirrors the operator's reference
 * template (ICS-213.txt); Winlink render mirrors the RMS_Express_Form
 * XML observed in a real ICS213 exchange (pat-users thread + K4KDR
 * PAT proof-of-concept).
 */

#include "ICS213Dialog.h"
#include "ui_ICS213Dialog.h"

#include "JS8_Main/DriftingDateTime.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>

#include "JS8_Include/SettingsGroup.h"

namespace {
constexpr int kBodyCap = 500;
QString const kDraftName = QStringLiteral("ics213_draft.json");

QString xmlEscape(QString s) {
    s.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    s.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return s;
}

QString xmlUnescape(QString s) {
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&")); // LAST
    return s;
}

// First line starting with `key` (after `from`) → the rest of it.
QString lineAfter(QString const &text, QString const &key,
                  int const from = 0) {
    int i = text.indexOf(key, from);
    if (i < 0) return {};
    i += key.size();
    int const e = text.indexOf(QLatin1Char('\n'), i);
    return text.mid(i, (e < 0 ? text.size() : e) - i).trimmed();
}
} // namespace

ICS213Dialog::ICS213Dialog(QSettings *settings, QString myCall,
                           QDir saveDir,
                           std::function<double(int)> airtimeSecs,
                           QWidget *parent)
    : QDialog{parent}, ui{new Ui::ICS213Dialog}, m_settings{settings},
      m_myCall{std::move(myCall)}, m_saveDir{std::move(saveDir)},
      m_airtimeSecs{std::move(airtimeSecs)} {
    ui->setupUi(this);

    auto const now = DriftingDateTime::currentDateTimeUtc();
    ui->dateEdit->setText(now.toString(QStringLiteral("yyyy-MM-dd")));
    ui->timeEdit->setText(now.toString(QStringLiteral("HH:mm")));

    {
        SettingsGroup g{m_settings, "ICS213"};
        ui->formatCombo->setCurrentIndex(
            m_settings->value("Format", 0).toInt());
    }

    // Body cap enforced in the change handler (QPlainTextEdit has no
    // maxLength) — truncation at the cap, never silent growth.
    for (auto *e : {ui->incidentEdit, ui->toEdit, ui->fromEdit,
                    ui->subjectEdit, ui->dateEdit, ui->timeEdit,
                    ui->approvedEdit, ui->positionEdit,
                    ui->repliedByEdit, ui->replyPositionEdit,
                    ui->replyDateTimeEdit})
        connect(e, &QLineEdit::textChanged, this,
                &ICS213Dialog::onFieldChanged);
    connect(ui->messageEdit, &QPlainTextEdit::textChanged, this,
            &ICS213Dialog::onFieldChanged);
    connect(ui->replyEdit, &QPlainTextEdit::textChanged, this,
            &ICS213Dialog::onFieldChanged);
    connect(ui->precedenceCombo, &QComboBox::currentIndexChanged, this,
            &ICS213Dialog::onFieldChanged);
    connect(ui->formatCombo, &QComboBox::currentIndexChanged, this,
            &ICS213Dialog::onFieldChanged);

    connect(ui->sendButton, &QPushButton::clicked, this,
            &ICS213Dialog::onSendClicked);
    connect(ui->saveButton, &QPushButton::clicked, this,
            &ICS213Dialog::onSaveClicked);
    connect(ui->clearButton, &QPushButton::clicked, this,
            &ICS213Dialog::onClearClicked);
    // "Save as draft" = reject(); the override below flushes the
    // draft, and Esc/X share the exact same path.
    connect(ui->cancelButton, &QPushButton::clicked, this,
            &QDialog::reject);

    m_draftTimer.setSingleShot(true);
    m_draftTimer.setInterval(1000);
    connect(&m_draftTimer, &QTimer::timeout, this,
            &ICS213Dialog::saveDraft);

    ui->arqBusyLabel->setVisible(false);
    // Serial in the title: peeked, not assigned — the number is only
    // consumed at Send/Save. Single-instance dialog + no other bumper
    // means the peek can't go stale while the form is open.
    setWindowTitle(tr("Send ICS-213 Form — %1")
                       .arg(peekSerial()));
    loadDraft();
    updateFooter();
}

ICS213Dialog::~ICS213Dialog() { delete ui; }

QDir ICS213Dialog::formDir() const {
    QDir d{m_saveDir};
    d.mkpath(QStringLiteral("ICS213"));
    d.cd(QStringLiteral("ICS213"));
    return d;
}

QString ICS213Dialog::draftPath() const {
    return formDir().filePath(kDraftName);
}

void ICS213Dialog::onFieldChanged() {
    // Enforce the body cap with truncation — on whichever body edit
    // is live in this mode (message in compose, reply text in reply).
    auto *body = m_replyMode ? ui->replyEdit : ui->messageEdit;
    if (auto txt = body->toPlainText(); txt.size() > kBodyCap) {
        auto c = body->textCursor();
        int const pos = std::min(c.position(), kBodyCap);
        body->setPlainText(txt.left(kBodyCap));
        c.setPosition(pos);
        body->setTextCursor(c);
    }
    updateFooter();
    // Reply mode has no draft — the received form is already on disk
    // and the compose draft must not be touched.
    if (!m_replyMode)
        m_draftTimer.start();
}

void ICS213Dialog::updateFooter() {
    auto *body = m_replyMode ? ui->replyEdit : ui->messageEdit;
    int const chars = renderStandard().size();
    QString est;
    if (m_airtimeSecs) {
        double const s = m_airtimeSecs(chars);
        est = (s >= 90.0)
                  ? tr(" · est. airtime ≈ %1 min").arg(qRound(s / 60.0))
                  : tr(" · est. airtime ≈ %1 s").arg(qRound(s));
    }
    ui->footerLabel->setText(
        tr("%1 / %2 message chars · file %3 chars%4")
            .arg(body->toPlainText().size())
            .arg(kBodyCap)
            .arg(chars)
            .arg(est));
}

bool ICS213Dialog::validateRequired() {
    if (m_replyMode) {
        if (ui->replyEdit->toPlainText().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("ICS-213"),
                                 tr("The reply text is required."));
            ui->replyEdit->setFocus();
            return false;
        }
        if (ui->repliedByEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(
                this, tr("ICS-213"),
                tr("\"Replied by (name)\" is required."));
            ui->repliedByEdit->setFocus();
            return false;
        }
        return true;
    }
    struct Req { QLineEdit *e; char const *name; };
    for (auto const &r : {Req{ui->toEdit, "To"},
                          Req{ui->fromEdit, "From"},
                          Req{ui->subjectEdit, "Subject"}}) {
        if (r.e->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("ICS-213"),
                                 tr("\"%1\" is required.")
                                     .arg(QLatin1String(r.name)));
            r.e->setFocus();
            return false;
        }
    }
    if (ui->messageEdit->toPlainText().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("ICS-213"),
                             tr("The message body is required."));
        ui->messageEdit->setFocus();
        return false;
    }
    return true;
}

QString ICS213Dialog::nextSerial() {
    SettingsGroup g{m_settings, "ICS213"};
    int const year =
        DriftingDateTime::currentDateTimeUtc().date().year();
    int next = m_settings->value("SerialNext", 1).toInt();
    if (m_settings->value("SerialYear", year).toInt() != year)
        next = 1; // yearly reset (operator decision 2026-08-18)
    m_settings->setValue("SerialYear", year);
    m_settings->setValue("SerialNext", next + 1);
    return QStringLiteral("%1-%2").arg(m_myCall).arg(
        next, 4, 10, QLatin1Char('0'));
}

QString ICS213Dialog::peekSerial() const {
    SettingsGroup g{m_settings, "ICS213"};
    int const year =
        DriftingDateTime::currentDateTimeUtc().date().year();
    int next = m_settings->value("SerialNext", 1).toInt();
    if (m_settings->value("SerialYear", year).toInt() != year)
        next = 1; // mirrors nextSerial()'s yearly reset, sans persist
    return QStringLiteral("%1-%2").arg(m_myCall).arg(
        next, 4, 10, QLatin1Char('0'));
}

QString ICS213Dialog::subjectSlug() const {
    QString s = ui->subjectEdit->text().toLower();
    static QRegularExpression const nonAlnum{
        QStringLiteral("[^a-z0-9]+")};
    s.replace(nonAlnum, QStringLiteral("-"));
    while (s.startsWith(QLatin1Char('-'))) s.remove(0, 1);
    while (s.endsWith(QLatin1Char('-'))) s.chop(1);
    return s.left(24);
}

// Subject with the precedence suffix (Routine = bare) — used by every
// render so the numbered layout stays structurally standard.
static QString subjectWithPrecedence(QString const &subject,
                                     int const precedenceIdx) {
    switch (precedenceIdx) {
    case 1: return subject + QStringLiteral(" [PRIORITY]");
    case 2: return subject + QStringLiteral(" [IMMEDIATE]");
    default: return subject;
    }
}

QString ICS213Dialog::renderStandard() const {
    // Mirrors the operator's reference template (ICS-213.txt): same
    // banner, same numbered sections; blocks 9/10 rendered as empty
    // headers so the recipient sees the complete familiar form
    // awaiting reply.
    QString const bar{50, QLatin1Char('=')};
    QString const thin{50, QLatin1Char('-')};
    QString out;
    out += bar + QLatin1Char('\n');
    out += QStringLiteral("GENERAL MESSAGE (ICS-213)\n");
    out += bar + QLatin1Char('\n');
    out += QStringLiteral("1. Incident Name (Optional): %1\n")
               .arg(ui->incidentEdit->text());
    out += QStringLiteral("2. To (Name and Position): %1\n")
               .arg(ui->toEdit->text());
    out += QStringLiteral("3. From (Name and Position): %1\n")
               .arg(ui->fromEdit->text());
    out += QStringLiteral("4. Subject: %1\n")
               .arg(subjectWithPrecedence(
                   ui->subjectEdit->text(),
                   ui->precedenceCombo->currentIndex()));
    out += QStringLiteral("5. Date: %1\n").arg(ui->dateEdit->text());
    out += QStringLiteral("6. Time: %1\n\n").arg(ui->timeEdit->text());
    out += QStringLiteral("7. Message:\n%1\n\n")
               .arg(ui->messageEdit->toPlainText());
    out += QStringLiteral("8. Approved by: \n");
    out += QStringLiteral("   Name: %1\n").arg(ui->approvedEdit->text());
    out += QStringLiteral("   Position/Title: %1\n\n")
               .arg(ui->positionEdit->text());
    out += QStringLiteral("Sender:  [%1]\n\n")
               .arg(m_replyMode ? m_replySenderCall : m_myCall);
    out += thin + QLatin1Char('\n');
    if (m_replyMode) {
        out += QStringLiteral("9. Reply (By Recipient):\n%1\n\n")
                   .arg(ui->replyEdit->toPlainText());
        out += QStringLiteral("10. Replied by: \n");
        out += QStringLiteral("    Name: %1\n")
                   .arg(ui->repliedByEdit->text());
        out += QStringLiteral("    Position/Title: %1\n")
                   .arg(ui->replyPositionEdit->text());
        out += QStringLiteral("    Date/Time: %1\n")
                   .arg(ui->replyDateTimeEdit->text());
    } else {
        out += QStringLiteral("9. Reply (By Recipient):\n\n");
        out += QStringLiteral("10. Replied by: \n");
        out += QStringLiteral("    Name: \n");
        out += QStringLiteral("    Position/Title: \n");
        out += QStringLiteral("    Date/Time: \n");
    }
    out += bar + QLatin1Char('\n');
    return out;
}

QString ICS213Dialog::renderCompact() const {
    QString out;
    out += QStringLiteral("FORM: ICS-213/1\n");
    out += QStringLiteral("INCIDENT: %1\n").arg(ui->incidentEdit->text());
    out += QStringLiteral("TO: %1\n").arg(ui->toEdit->text());
    out += QStringLiteral("FROM: %1\n").arg(ui->fromEdit->text());
    out += QStringLiteral("SUBJECT: %1\n")
               .arg(subjectWithPrecedence(
                   ui->subjectEdit->text(),
                   ui->precedenceCombo->currentIndex()));
    out += QStringLiteral("DATE: %1\n").arg(ui->dateEdit->text());
    out += QStringLiteral("TIME: %1\n").arg(ui->timeEdit->text());
    out += QStringLiteral("APPROVED: %1, %2\n")
               .arg(ui->approvedEdit->text(), ui->positionEdit->text());
    out += QStringLiteral("SENDER: %1\n")
               .arg(m_replyMode ? m_replySenderCall : m_myCall);
    out += QStringLiteral("MSG:\n%1\n")
               .arg(ui->messageEdit->toPlainText());
    if (m_replyMode) {
        out += QStringLiteral("REPLY:\n%1\n")
                   .arg(ui->replyEdit->toPlainText());
        out += QStringLiteral("REPLIED-BY: %1, %2\n")
                   .arg(ui->repliedByEdit->text(),
                        ui->replyPositionEdit->text());
        out += QStringLiteral("REPLY-DATETIME: %1\n")
                   .arg(ui->replyDateTimeEdit->text());
    }
    return out;
}

QString ICS213Dialog::renderWinlinkXml(QString const &serial) const {
    // RMS_Express_Form structure per a real ICS213 exchange (tags
    // verified from the pat-users capture, incl. Winlink's own
    // "approved_postitle" spelling). msgseqnum = numeric part of our
    // serial. NOTE: the message-body tag <msg> is best-known but was
    // not visible in the captures — VERIFY against a live Winlink
    // export before relying on far-end rendering (a wrong tag shows
    // an empty body in the Winlink viewer).
    auto const now = DriftingDateTime::currentDateTimeUtc();
    QString const seq = serial.section(QLatin1Char('-'), -1);
    QString out;
    out += QStringLiteral("<?xml version=\"1.0\"?>\n");
    out += QStringLiteral("<RMS_Express_Form>\n");
    out += QStringLiteral("  <form_parameters>\n");
    out += QStringLiteral("    <xml_file_version>1.0</xml_file_version>\n");
    out += QStringLiteral("    <rms_express_version>Subspace</rms_express_version>\n");
    out += QStringLiteral("    <submission_datetime>%1</submission_datetime>\n")
               .arg(now.toString(QStringLiteral("yyyyMMddHHmmss")));
    out += QStringLiteral("    <senders_callsign>%1</senders_callsign>\n")
               .arg(xmlEscape(m_replyMode ? m_replySenderCall
                                          : m_myCall));
    out += QStringLiteral("    <display_form>ICS213_Initial_Viewer.html</display_form>\n");
    out += QStringLiteral("    <reply_template>ICS213_SendReply.0</reply_template>\n");
    out += QStringLiteral("  </form_parameters>\n");
    out += QStringLiteral("  <variables>\n");
    out += QStringLiteral("    <templateversion>ICS 213 v.41.3</templateversion>\n");
    out += QStringLiteral("    <msgsender>%1</msgsender>\n")
               .arg(xmlEscape(m_replyMode ? m_replySenderCall
                                          : m_myCall));
    out += QStringLiteral("    <msgseqnum>%1</msgseqnum>\n").arg(seq);
    out += QStringLiteral("    <inc_name>%1</inc_name>\n")
               .arg(xmlEscape(ui->incidentEdit->text()));
    out += QStringLiteral("    <to_name>%1</to_name>\n")
               .arg(xmlEscape(ui->toEdit->text()));
    out += QStringLiteral("    <fm_name>%1</fm_name>\n")
               .arg(xmlEscape(ui->fromEdit->text()));
    out += QStringLiteral("    <subjectline>%1</subjectline>\n")
               .arg(xmlEscape(subjectWithPrecedence(
                   ui->subjectEdit->text(),
                   ui->precedenceCombo->currentIndex())));
    out += QStringLiteral("    <mdate>%1</mdate>\n")
               .arg(xmlEscape(ui->dateEdit->text()));
    out += QStringLiteral("    <mtime>%1</mtime>\n")
               .arg(xmlEscape(ui->timeEdit->text()));
    out += QStringLiteral("    <msg>%1</msg>\n")
               .arg(xmlEscape(ui->messageEdit->toPlainText()));
    out += QStringLiteral("    <approved_name>%1</approved_name>\n")
               .arg(xmlEscape(ui->approvedEdit->text()));
    out += QStringLiteral("    <approved_postitle>%1</approved_postitle>\n")
               .arg(xmlEscape(ui->positionEdit->text()));
    if (m_replyMode) {
        // Reply tag names UNVERIFIED against a live Winlink reply
        // export (same caveat as <msg> above) — chosen to match the
        // capture's naming style.
        out += QStringLiteral("    <reply>%1</reply>\n")
                   .arg(xmlEscape(ui->replyEdit->toPlainText()));
        out += QStringLiteral("    <replied_by>%1</replied_by>\n")
                   .arg(xmlEscape(ui->repliedByEdit->text()));
        out += QStringLiteral(
                   "    <replied_postitle>%1</replied_postitle>\n")
                   .arg(xmlEscape(ui->replyPositionEdit->text()));
        out += QStringLiteral(
                   "    <replied_datetime>%1</replied_datetime>\n")
                   .arg(xmlEscape(ui->replyDateTimeEdit->text()));
    }
    out += QStringLiteral("  </variables>\n");
    out += QStringLiteral("</RMS_Express_Form>\n");
    return out;
}

bool ICS213Dialog::writeFormFile(QString *outPath) {
    if (!validateRequired())
        return false;
    int const fmt =
        m_replyMode ? m_replyFormat : ui->formatCombo->currentIndex();
    if (!m_replyMode) {
        SettingsGroup g{m_settings, "ICS213"};
        m_settings->setValue("Format", fmt);
    }
    // A reply COMPLETES the original form — it keeps the original's
    // serial and consumes none of ours.
    QString const serial =
        m_replyMode ? (m_replySerial.isEmpty()
                           ? QStringLiteral("REPLY")
                           : m_replySerial)
                    : nextSerial();
    QString content;
    QString ext = QStringLiteral("txt");
    switch (fmt) {
    case 1: content = renderCompact(); break;
    case 2:
        content = renderWinlinkXml(serial);
        ext = QStringLiteral("xml");
        break;
    default: content = renderStandard(); break;
    }
    int const year =
        DriftingDateTime::currentDateTimeUtc().date().year();
    QString const name =
        m_replyMode
            ? QStringLiteral("%1_REPLY.%2")
                  .arg(QFileInfo{m_replySourcePath}.completeBaseName())
                  .arg(ext)
            : QStringLiteral("ICS213_%1_%2_%3.%4")
                  .arg(serial)
                  .arg(year)
                  .arg(subjectSlug())
                  .arg(ext);
    QString const path = formDir().filePath(name);
    QFile f{path};
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(
            this, tr("ICS-213"),
            tr("Could not write %1:\n%2").arg(path, f.errorString()));
        return false;
    }
    f.write(content.toUtf8());
    f.close();
    m_writtenPath = path;
    if (outPath)
        *outPath = path;
    return true;
}

void ICS213Dialog::onSendClicked() {
    QString path;
    if (!writeFormFile(&path))
        return;
    if (!m_replyMode)
        clearDraft(); // reply never touches the compose draft
    Q_EMIT sendRequested(path);
    accept();
}

void ICS213Dialog::onSaveClicked() {
    QString path;
    if (!writeFormFile(&path))
        return;
    clearDraft();
    m_draftTimer.stop();
    QMessageBox::information(this, tr("ICS-213"),
                             tr("Form saved:\n%1").arg(path));
    accept(); // "Save as file" exits after showing the file name
}

void ICS213Dialog::onClearClicked() {
    for (auto *e : {ui->incidentEdit, ui->toEdit, ui->fromEdit,
                    ui->subjectEdit, ui->approvedEdit,
                    ui->positionEdit})
        e->clear();
    ui->messageEdit->clear();
    ui->precedenceCombo->setCurrentIndex(0);
    auto const now = DriftingDateTime::currentDateTimeUtc();
    ui->dateEdit->setText(now.toString(QStringLiteral("yyyy-MM-dd")));
    ui->timeEdit->setText(now.toString(QStringLiteral("HH:mm")));
    clearDraft();
    // The clear() calls above fired textChanged → armed the debounce,
    // which would re-write an EMPTY draft 1 s from now. Disarm it.
    m_draftTimer.stop();
    updateFooter();
}

void ICS213Dialog::reject() {
    m_draftTimer.stop();
    // Reply mode: Cancel discards the reply; the RECEIVED form is
    // already saved on disk and the compose draft is not ours to
    // write.
    if (!m_replyMode)
        saveDraft(); // deletes the draft instead if all-empty
    QDialog::reject();
}

int ICS213Dialog::probeFormat(QByteArray const &bytes) {
    if (bytes.contains("<RMS_Express_Form>") &&
        bytes.contains("<to_name>") && bytes.contains("<msg>"))
        return 2;
    if (bytes.contains("FORM: ICS-213/1") && bytes.contains("\nTO: ") &&
        bytes.contains("\nMSG:\n"))
        return 1;
    if (bytes.contains("GENERAL MESSAGE (ICS-213)") &&
        bytes.contains("2. To (Name and Position): ") &&
        bytes.contains("7. Message:\n"))
        return 0;
    return -1;
}

bool ICS213Dialog::enterReplyMode(QString const &receivedPath,
                                  QString const &fromCall) {
    QFile f{receivedPath};
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(
            this, tr("ICS-213"),
            tr("Could not read %1:\n%2")
                .arg(receivedPath, f.errorString()));
        return false;
    }
    QByteArray const bytes = f.readAll();
    int const fmt = probeFormat(bytes);
    if (fmt < 0) {
        QMessageBox::warning(this, tr("ICS-213"),
                             tr("Unable to interpret the form."));
        return false;
    }

    // Mode flag FIRST — the setText calls below fire onFieldChanged,
    // which must not arm the compose-draft autosave.
    m_replyMode = true;
    m_replyFormat = fmt;
    m_replySourcePath = receivedPath;
    m_replySenderCall = fromCall; // parse may refine from the form

    QString const name = QFileInfo{receivedPath}.fileName();
    m_replySerial = name.startsWith(QStringLiteral("ICS213_"),
                                    Qt::CaseInsensitive)
                        ? name.section(QLatin1Char('_'), 1, 1)
                        : QString();

    QString const content = QString::fromUtf8(bytes);
    ui->precedenceCombo->setCurrentIndex(0); // suffix stays in subject
    switch (fmt) {
    case 2: parseWinlinkXml(content); break;
    case 1: parseCompact(content); break;
    default: parseStandard(content); break;
    }

    // Originals are evidence now — read-only.
    for (auto *e : {ui->incidentEdit, ui->toEdit, ui->fromEdit,
                    ui->subjectEdit, ui->dateEdit, ui->timeEdit,
                    ui->approvedEdit, ui->positionEdit})
        e->setReadOnly(true);
    ui->messageEdit->setReadOnly(true);
    ui->precedenceCombo->setEnabled(false);
    ui->formatCombo->setEnabled(false); // reply goes back in kind

    // Reply blocks visible, date/time pre-filled.
    for (QWidget *w :
         std::initializer_list<QWidget *>{
             ui->replyLabel, ui->replyEdit, ui->repliedByLabel,
             ui->repliedByEdit, ui->replyPositionLabel,
             ui->replyPositionEdit, ui->replyDateTimeLabel,
             ui->replyDateTimeEdit})
        w->setVisible(true);
    ui->replyDateTimeEdit->setText(
        DriftingDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyy-MM-dd HH:mm")));

    // Button set for this phase.
    ui->clearButton->setVisible(false);
    ui->saveButton->setVisible(false);
    ui->sendButton->setToolTip(tr("Send reply immediately"));
    ui->cancelButton->setText(tr("Cancel"));
    ui->cancelButton->setToolTip(tr(
        "Cancel reply, the form is saved for 'Send file' later"));

    setWindowTitle(
        m_replySerial.isEmpty()
            ? tr("Reply to ICS-213 Form")
            : tr("Reply to ICS-213 Form \u2014 %1").arg(m_replySerial));
    ui->replyEdit->setFocus();
    updateFooter();
    return true;
}

void ICS213Dialog::parseStandard(QString const &content) {
    // Only the part above block 9 — block 10's "Name:" line would
    // otherwise shadow block 8's.
    QString const head =
        content.section(QStringLiteral("\n9. Reply"), 0, 0);
    ui->incidentEdit->setText(
        lineAfter(head, QStringLiteral("1. Incident Name (Optional): ")));
    ui->toEdit->setText(
        lineAfter(head, QStringLiteral("2. To (Name and Position): ")));
    ui->fromEdit->setText(
        lineAfter(head, QStringLiteral("3. From (Name and Position): ")));
    ui->subjectEdit->setText(
        lineAfter(head, QStringLiteral("4. Subject: ")));
    ui->dateEdit->setText(lineAfter(head, QStringLiteral("5. Date: ")));
    ui->timeEdit->setText(lineAfter(head, QStringLiteral("6. Time: ")));
    int const m0 = head.indexOf(QStringLiteral("7. Message:\n"));
    int const m1 = head.indexOf(QStringLiteral("\n\n8. Approved by:"));
    if (m0 >= 0 && m1 > m0)
        ui->messageEdit->setPlainText(head.mid(m0 + 12, m1 - (m0 + 12)));
    if (int const a = head.indexOf(QStringLiteral("8. Approved by:"));
        a >= 0) {
        ui->approvedEdit->setText(
            lineAfter(head, QStringLiteral("Name: "), a));
        ui->positionEdit->setText(
            lineAfter(head, QStringLiteral("Position/Title: "), a));
    }
    static QRegularExpression const senderRe{
        QStringLiteral("Sender:\\s*\\[([^\\]]+)\\]")};
    if (auto const m = senderRe.match(head); m.hasMatch())
        m_replySenderCall = m.captured(1);
}

void ICS213Dialog::parseCompact(QString const &content) {
    int const mi = content.indexOf(QStringLiteral("\nMSG:\n"));
    QString const head = mi >= 0 ? content.left(mi + 1) : content;
    ui->incidentEdit->setText(
        lineAfter(head, QStringLiteral("INCIDENT: ")));
    ui->toEdit->setText(lineAfter(head, QStringLiteral("TO: ")));
    ui->fromEdit->setText(lineAfter(head, QStringLiteral("FROM: ")));
    ui->subjectEdit->setText(
        lineAfter(head, QStringLiteral("SUBJECT: ")));
    ui->dateEdit->setText(lineAfter(head, QStringLiteral("DATE: ")));
    ui->timeEdit->setText(lineAfter(head, QStringLiteral("TIME: ")));
    // "APPROVED: name, position" — split on the LAST comma so names
    // containing commas survive better than positions do.
    if (QString const ap = lineAfter(head, QStringLiteral("APPROVED: "));
        !ap.isEmpty()) {
        int const comma = ap.lastIndexOf(QStringLiteral(", "));
        ui->approvedEdit->setText(comma >= 0 ? ap.left(comma) : ap);
        ui->positionEdit->setText(comma >= 0 ? ap.mid(comma + 2)
                                             : QString());
    }
    if (QString const sc = lineAfter(head, QStringLiteral("SENDER: "));
        !sc.isEmpty())
        m_replySenderCall = sc;
    if (mi >= 0) {
        QString msg = content.mid(mi + 6);
        if (msg.endsWith(QLatin1Char('\n')))
            msg.chop(1);
        ui->messageEdit->setPlainText(msg);
    }
}

void ICS213Dialog::parseWinlinkXml(QString const &content) {
    auto const tag = [&content](char const *t) -> QString {
        QRegularExpression const re{
            QStringLiteral("<%1>(.*)</%1>").arg(QLatin1String{t}),
            QRegularExpression::DotMatchesEverythingOption |
                QRegularExpression::InvertedGreedinessOption};
        auto const m = re.match(content);
        return m.hasMatch() ? xmlUnescape(m.captured(1)) : QString();
    };
    ui->incidentEdit->setText(tag("inc_name"));
    ui->toEdit->setText(tag("to_name"));
    ui->fromEdit->setText(tag("fm_name"));
    ui->subjectEdit->setText(tag("subjectline"));
    ui->dateEdit->setText(tag("mdate"));
    ui->timeEdit->setText(tag("mtime"));
    ui->messageEdit->setPlainText(tag("msg"));
    ui->approvedEdit->setText(tag("approved_name"));
    ui->positionEdit->setText(tag("approved_postitle"));
    if (QString const sc = tag("msgsender"); !sc.isEmpty())
        m_replySenderCall = sc;
}

void ICS213Dialog::saveDraft() {
    // An all-empty form has no draft to keep — delete instead of
    // writing an empty file that would "restore" a blank form.
    bool const empty =
        ui->incidentEdit->text().isEmpty() &&
        ui->toEdit->text().isEmpty() && ui->fromEdit->text().isEmpty() &&
        ui->subjectEdit->text().isEmpty() &&
        ui->messageEdit->toPlainText().isEmpty() &&
        ui->approvedEdit->text().isEmpty() &&
        ui->positionEdit->text().isEmpty();
    if (empty) {
        clearDraft();
        return;
    }
    QJsonObject o;
    o[QStringLiteral("incident")] = ui->incidentEdit->text();
    o[QStringLiteral("to")] = ui->toEdit->text();
    o[QStringLiteral("from")] = ui->fromEdit->text();
    o[QStringLiteral("subject")] = ui->subjectEdit->text();
    o[QStringLiteral("precedence")] =
        ui->precedenceCombo->currentIndex();
    o[QStringLiteral("date")] = ui->dateEdit->text();
    o[QStringLiteral("time")] = ui->timeEdit->text();
    o[QStringLiteral("message")] = ui->messageEdit->toPlainText();
    o[QStringLiteral("approved")] = ui->approvedEdit->text();
    o[QStringLiteral("position")] = ui->positionEdit->text();
    QFile f{draftPath()};
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument{o}.toJson(QJsonDocument::Compact));
}

void ICS213Dialog::loadDraft() {
    QFile f{draftPath()};
    if (!f.open(QIODevice::ReadOnly))
        return;
    auto const o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty())
        return;
    ui->incidentEdit->setText(o[QStringLiteral("incident")].toString());
    ui->toEdit->setText(o[QStringLiteral("to")].toString());
    ui->fromEdit->setText(o[QStringLiteral("from")].toString());
    ui->subjectEdit->setText(o[QStringLiteral("subject")].toString());
    ui->precedenceCombo->setCurrentIndex(
        o[QStringLiteral("precedence")].toInt());
    if (auto const d = o[QStringLiteral("date")].toString();
        !d.isEmpty())
        ui->dateEdit->setText(d);
    if (auto const t = o[QStringLiteral("time")].toString();
        !t.isEmpty())
        ui->timeEdit->setText(t);
    ui->messageEdit->setPlainText(
        o[QStringLiteral("message")].toString());
    ui->approvedEdit->setText(o[QStringLiteral("approved")].toString());
    ui->positionEdit->setText(o[QStringLiteral("position")].toString());
    ui->footerLabel->setText(tr("Draft restored."));
}

void ICS213Dialog::clearDraft() { QFile::remove(draftPath()); }

void ICS213Dialog::refreshEstimate() { updateFooter(); }

void ICS213Dialog::setArqBusy(bool const busy) {
    ui->sendButton->setDisabled(busy);
    ui->arqBusyLabel->setVisible(busy);
}
