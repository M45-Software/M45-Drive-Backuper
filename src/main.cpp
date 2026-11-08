#include "window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QTimer>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("M45 Drive Backuper");
    app.setOrganizationName("M45 Drive Backuper");
    app.setApplicationVersion(M45_DRIVE_BACKUPER_VERSION);
    app.setDesktopFileName("io.github.m45drivebackuper");
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser args;
    args.setApplicationDescription("Borg backup settings and monitoring for KDE Plasma");
    args.addHelpOption(); args.addVersionOption();
    args.addOption({"show", "Open the backup overview."});
    args.addOption({"demo", "Preview synthetic data without changing the machine."});
    args.addOption({"page", "Open overview, settings, exclusions, archives, journal or preferences.", "page", "overview"});
    args.addOption({"screenshot", "Save a preview screenshot and exit (requires --demo).", "file"});
    args.process(app);
    if (args.isSet("screenshot") && !args.isSet("demo")) args.showHelp(2);
    int page = QStringList{"overview", "settings", "exclusions", "archives", "journal", "preferences"}.indexOf(args.value("page"));
    if (page < 0) args.showHelp(2);
    auto bus = QDBusConnection::sessionBus();
    if (!args.isSet("demo")) {
        if (!bus.isConnected()) qFatal("A desktop D-Bus session is required.");
        if (!bus.registerService("io.github.m45drivebackuper")) {
            QDBusInterface existing("io.github.m45drivebackuper", "/M45DriveBackuper", "io.github.m45drivebackuper", bus);
            existing.call("Show"); return 0;
        }
    }
    Window window(args.isSet("demo"));
    if (!args.isSet("demo")) bus.registerObject("/M45DriveBackuper", &window, QDBusConnection::ExportAllSlots);
    window.selectPage(page);
    if (args.isSet("show") || args.isSet("demo") || !QSystemTrayIcon::isSystemTrayAvailable()) window.Show();
    if (args.isSet("screenshot")) QTimer::singleShot(500, &app, [&] {
        app.exit(window.grab().save(args.value("screenshot")) ? 0 : 1);
    });
    return app.exec();
}
