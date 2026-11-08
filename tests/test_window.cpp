#include "window.h"
#include <QtTest>
#include <QCheckBox>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QLineEdit>

class WindowTests : public QObject {
    Q_OBJECT
private slots:
    void previewDoesNotChangeSettings() {
        QTemporaryDir dir;
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,dir.path());
        QSettings settings("M45 Drive Backuper","M45 Drive Backuper");
        settings.setValue("notifications",false); settings.setValue("lastNotification","sentinel"); settings.sync();
        Window window(true);
        window.Show(); QTest::qWait(20);
        auto *tabs = window.findChild<QTabWidget *>();
        QCOMPARE(tabs->count(),4);
        for (auto *button : window.findChildren<QPushButton *>()) {
            if (button->text().contains("Back up now") || button->text().contains("save backup") || button->text().contains("archive catalog"))
                QVERIFY2(!button->isEnabled(),qPrintable(button->text()));
        }
        for (auto *checkbox : window.findChildren<QCheckBox *>()) QVERIFY(!checkbox->isEnabled());
        window.selectPage(3);
        for (auto *button : window.findChildren<QPushButton *>())
            if (button->text().startsWith("Use detected")) QTest::mouseClick(button,Qt::LeftButton);
        QCOMPARE(window.findChild<QLineEdit *>()->text(),QString("/media/dist/Backup-SSD"));
        window.close(); QVERIFY(!window.isVisible());
        settings.sync(); QCOMPARE(settings.value("lastNotification").toString(),QString("sentinel"));
        QCOMPARE(settings.value("notifications").toBool(),false);
    }
};
QTEST_MAIN(WindowTests)
#include "test_window.moc"
