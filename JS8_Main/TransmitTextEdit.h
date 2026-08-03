#ifndef TRANSMITTEXTEDIT_H
#define TRANSMITTEXTEDIT_H

#include "qt_helpers.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>

void setTextEditFont(QTextEdit *edit, QFont font);
void setTextEditStyle(QTextEdit *edit, QColor fg, QColor bg, QFont font);
void highlightBlock(QTextBlock block, QFont font, QColor foreground,
                    QColor background);

class TransmitTextEdit : public QTextEdit {
  public:
    TransmitTextEdit(QWidget *parent);

    static QPair<int, int> relativeTextCursorPosition(QTextCursor cursor) {
        auto c = QTextCursor(cursor);
        c.movePosition(QTextCursor::End);
        int last = c.position();

        auto cc = QTextCursor(cursor);
        int relstart = last - qMin(cc.selectionStart(), cc.selectionEnd());
        int relend = last - qMax(cc.selectionStart(), cc.selectionEnd());

        return {relstart, relend};
    }

    int charsSent() const { return m_sent; }
    void setCharsSent(int n);

    QString sentText() const { return m_textSent; }

    QString unsentText() const { return toPlainText().mid(charsSent()); }

    QString toPlainText() const;
    void setPlainText(const QString &text);
    void replaceUnsentText(const QString &text, bool keepCursor);
    void replacePlainText(const QString &text, bool keepCursor);

    void setFont(QFont f);
    void setFont(QFont f, QColor fg, QColor bg);
    void clear();

    bool isProtected() const { return m_protected; }
    void setProtected(bool protect);
    bool cursorShouldBeProtected(QTextCursor c);

    bool isEmpty() const { return toPlainText().isEmpty(); }
    bool isDirty() const { return m_dirty; }
    void setClean() { m_dirty = false; }

    void highlightBase();
    void highlightCharsSent();
    void highlight();

    bool eventFilter(QObject * /*o*/, QEvent *e);

    // [BUILD 354 winban] Placeholder painted by US, not Qt. Qt's
    // native QTextEdit placeholder rendering changed across versions
    // — Qt 6.4 (Linux builds) draws every line, Qt 6.9 (Windows CI)
    // draws a single elided line, which truncated the multi-line
    // "MULTI-PART MSG IN PROGRESS..." banner and the default two-line
    // prompt to their first lines on Windows. These SHADOW the
    // non-virtual QTextEdit members: every call site reaches the
    // widget through a TransmitTextEdit* (ui->extFreeTextMsgEdit), so
    // compile-time dispatch lands here; the native placeholder is
    // kept EMPTY so Qt paints nothing and paintEvent draws all lines
    // itself — identical rendering on every Qt version.
    QString placeholderText() const { return m_placeholder; }
    void setPlaceholderText(QString const &text);

  protected:
    // Prevent crash in Qt's QWidgetTextControl::insertFromMimeData
    // when clipboard contains non-text data (QTBUG in Qt 6.4.x)
    void insertFromMimeData(const QMimeData *source) override;
    void paintEvent(QPaintEvent *e) override;

  public slots:
    void on_selectionChanged();
    void on_textContentsChanged(int pos, int rem, int add);

  private:
    QString m_lastText;
    int m_sent;
    QString m_textSent;
    bool m_protected;
    bool m_dirty;
    QFont m_font;
    QColor m_fg;
    QColor m_bg;
    QString m_placeholder; // see setPlaceholderText shadow above
};

#endif // TRANSMITTEXTEDIT_H
