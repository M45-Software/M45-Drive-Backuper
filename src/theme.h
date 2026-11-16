#pragma once
#include <QApplication>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>

// Qt 6.4's stylesheet engine synthesizes placeholder text from Text whenever
// a color rule is applied. Keep the desktop's actual PlaceholderText brushes;
// leave every other role unresolved so it continues to inherit theme changes.
inline void applyPlaceholderPalette(QWidget *widget) {
    if (!qobject_cast<QLineEdit *>(widget) && !qobject_cast<QPlainTextEdit *>(widget) && !qobject_cast<QTextEdit *>(widget)) return;
    QPalette palette;
    for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
        palette.setBrush(group,QPalette::PlaceholderText,qApp->palette().brush(group,QPalette::PlaceholderText));
    widget->setPalette(palette);
}
