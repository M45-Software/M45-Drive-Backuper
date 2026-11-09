// Capture demo pages: capture-screenshots [output] [dark|light|switch-dark|switch-light].
#include "window.h"
#include <QApplication>
#include <QDir>
#include <QFont>
#include <QPalette>
#include <QStyle>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <cstdio>

namespace {
QPalette screenshotPalette() {
    auto p = QApplication::style()->standardPalette();
    p.setColor(QPalette::Window, QColor("#252930"));
    p.setColor(QPalette::Base, QColor("#1b1e24"));
    p.setColor(QPalette::AlternateBase, QColor("#292e36"));
    p.setColor(QPalette::Text, QColor("#e3e7ed"));
    p.setColor(QPalette::WindowText, QColor("#e3e7ed"));
    p.setColor(QPalette::Button, QColor("#303640"));
    p.setColor(QPalette::ButtonText, QColor("#e3e7ed"));
    p.setColor(QPalette::Mid, QColor("#69717f"));
    p.setColor(QPalette::Light, QColor("#69717f"));
    p.setColor(QPalette::Dark, QColor("#16191e"));
    p.setColor(QPalette::PlaceholderText, QColor("#a0a9b6"));
    p.setColor(QPalette::Highlight, QColor("#347fba"));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor("#949dab"));
    p.setColor(QPalette::Disabled, QPalette::WindowText, p.color(QPalette::Disabled, QPalette::Text));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, p.color(QPalette::Disabled, QPalette::Text));
    return p;
}
}

int main(int argc, char **argv) {
    QTemporaryDir profile;
    if (!profile.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", profile.path().toUtf8());
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setFont(QFont("Noto Sans", 10));
    app.setQuitOnLastWindowClosed(false);
    const auto args = app.arguments();
    const QDir output(args.size() > 1 ? args[1] : "docs/screenshots");
    const QString theme = args.size() > 2 ? args[2] : "dark";
    if (!QStringList{"dark", "light", "switch-dark", "switch-light"}.contains(theme)) return 2;
    const auto light = app.style()->standardPalette();
    const auto dark = screenshotPalette();
    if (!QDir().mkpath(output.absolutePath())) return 1;
    bool saved = true;
    const QStringList names{"overview", "settings", "exclusions", "backups", "activity", "preferences"};
    {
        app.setPalette(theme == "light" || theme == "switch-dark" ? light : dark);
        Window window(true);
        window.resize(1200, 960);
        window.show();
        if (theme.startsWith("switch-")) {
            app.processEvents();
            app.setPalette(theme == "switch-dark" ? dark : light);
            app.processEvents();
        }
        for (int page = 0; page < names.size(); ++page) {
            window.selectPage(page);
            // Let wrapping, scroll areas and content-sized rows finish layout.
            QEventLoop settle;
            QTimer::singleShot(100, &settle, &QEventLoop::quit);
            settle.exec();
            const auto path = output.filePath(names[page] + ".png");
            const bool ok = window.grab().save(path);
            std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "Saved" : "Failed", qPrintable(path));
            saved = saved && ok;
        }
    }
    return saved ? 0 : 1;
}
