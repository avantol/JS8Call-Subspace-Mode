#ifndef RXTEXTEDIT_H
#define RXTEXTEDIT_H

#include <QMimeData>
#include <QTextCursor>
#include <QTextEdit>

// Read-only-side QTextEdit (textEditRX). Overrides
// createMimeDataFromSelection() to publish text/plain only, never
// text/html or text/markdown. Qt's default builder routes through
// QTextMarkdownWriter which can SIGABRT under certain font-cache
// states (QFontDatabase::findFont -> qFatal). The override fires for
// every clipboard / drag path: menu Copy, Ctrl+C, X11/Wayland
// PRIMARY-selection autonomous copy on text-select, and drag-out
// from a text selection. Paste is not relevant -- textEditRX is
// read-only.

class RxTextEdit : public QTextEdit {
public:
    explicit RxTextEdit(QWidget *parent = nullptr) : QTextEdit(parent) {}

protected:
    QMimeData *createMimeDataFromSelection() const override {
        auto *m = new QMimeData;
        auto const c = textCursor();
        if (c.hasSelection()) {
            m->setText(c.selection().toPlainText());
        }
        return m;
    }
};

#endif // RXTEXTEDIT_H
