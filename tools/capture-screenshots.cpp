// Capture the actual Qt UI with deterministic demo data, fonts and palettes.
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
QPalette screenshotPalette(bool dark) {
    auto p = QApplication::style()->standardPalette();
    if (dark) {
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
    } else {
        p.setColor(QPalette::Window, QColor("#f0f3f7"));
        p.setColor(QPalette::Base, QColor("#ffffff"));
        p.setColor(QPalette::AlternateBase, QColor("#f4f6f9"));
        p.setColor(QPalette::Text, QColor("#263344"));
        p.setColor(QPalette::WindowText, QColor("#263344"));
        p.setColor(QPalette::Button, QColor("#f5f7fa"));
        p.setColor(QPalette::ButtonText, QColor("#263344"));
        p.setColor(QPalette::Mid, QColor("#a9b4c2"));
        p.setColor(QPalette::PlaceholderText, QColor("#657286"));
    }
    p.setColor(QPalette::Highlight, QColor(dark ? "#347fba" : "#286da8"));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(dark ? "#949dab" : "#737f8f"));
    p.setColor(QPalette::Disabled, QPalette::WindowText, p.color(QPalette::Disabled, QPalette::Text));
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
    if (!QDir().mkpath(output.absolutePath())) return 1;
    bool saved = true;
    const QStringList names{"overview", "settings", "exclusions", "backups", "activity", "preferences"};
    for (bool dark : {false, true}) {
        app.setPalette(screenshotPalette(dark));
        Window window(true);
        window.resize(1200, 960);
        window.show();
        for (int page = 0; page < names.size(); ++page) {
            if (dark && page != 0 && page != 4) continue;
            window.selectPage(page);
            // Let wrapping, scroll areas and content-sized rows finish layout.
            QEventLoop settle;
            QTimer::singleShot(100, &settle, &QEventLoop::quit);
            settle.exec();
            const auto path = output.filePath(names[page] + (dark ? "-dark.png" : ".png"));
            const bool ok = window.grab().save(path);
            std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "Saved" : "Failed", qPrintable(path));
            saved = saved && ok;
        }
    }
    return saved ? 0 : 1;
}
