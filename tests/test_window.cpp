#include "window.h"
#include "exclusions.h"
#include <QtTest>
#include <QCheckBox>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QTableWidget>
#include <QLabel>

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
        QCOMPARE(tabs->count(),6);
        QCOMPARE(tabs->tabText(2),QString("Skip files"));
        QVERIFY(!window.findChild<QPushButton *>("saveSettings")->isEnabled());
        for (auto *button : window.findChildren<QPushButton *>()) {
            if (button->text().contains("Back up now") || button->text().contains("Save backup") || button->text().contains("archive catalog"))
                QVERIFY2(!button->isEnabled(),qPrintable(button->text()));
        }
        for (auto *checkbox : window.findChildren<QCheckBox *>())
            if (checkbox->text().contains("login") || checkbox->text().contains("Notify")) QVERIFY(!checkbox->isEnabled());
        window.selectPage(1);
        auto *advanced = window.findChild<QWidget *>("advancedOptions");
        QVERIFY(advanced->isHidden());
        QVERIFY(!window.findChild<QComboBox *>("compression")->isVisible());
        QVERIFY(!window.findChild<QComboBox *>("backupType")->isVisible());
        const auto folderDestination = window.findChild<QLineEdit *>("repositoryPath")->text();
        window.findChild<QComboBox *>("destinationKind")->setCurrentIndex(1);
        auto *dest = window.findChild<QComboBox *>("destinationDrive"); dest->setCurrentIndex(1);
        QCOMPARE(window.findChild<QLineEdit *>("repositoryPath")->text(),QString("/media/dist/Backup-SSD/borg-home-example"));
        dest->setCurrentIndex(2);
        QCOMPARE(window.findChild<QLineEdit *>("repositoryPath")->text(),QString("/media/example/Backup/M45-Backups"));
        QVERIFY(window.findChild<QLabel *>("destinationHint")->text().contains("/media/example/Backup/M45-Backups"));
        window.findChild<QComboBox *>("destinationKind")->setCurrentIndex(0);
        QCOMPARE(window.findChild<QLineEdit *>("repositoryPath")->text(),folderDestination);
        window.findChild<QComboBox *>("destinationKind")->setCurrentIndex(1);
        QCOMPARE(window.findChild<QLineEdit *>("repositoryPath")->text(),QString("/media/example/Backup/M45-Backups"));
        auto *kind = window.findChild<QComboBox *>("sourceKind"); kind->setCurrentIndex(1);
        QCOMPARE(window.findChild<QLineEdit *>("sourcePath")->text(),QString("/media/example/Documents"));
        QVERIFY(window.findChild<QComboBox *>("sourceDrive")->isVisible());
        kind->setCurrentIndex(0);
        QCOMPARE(window.findChild<QLineEdit *>("sourcePath")->text(),QString("/home/dist"));
        window.findChild<QComboBox *>("backupType")->setCurrentIndex(1);
        window.findChild<QComboBox *>("compression")->setCurrentIndex(4);
        window.findChild<QComboBox *>("quotaMode")->setCurrentIndex(2);
        window.findChild<QSpinBox *>("quotaGiB")->setValue(100);
        window.findChild<QCheckBox *>("autoMount")->setChecked(true);
        QVERIFY(window.findChild<QPushButton *>("saveSettings")->isEnabled());
        auto *toggle = window.findChild<QToolButton *>("advancedToggle"); toggle->setChecked(true); QVERIFY(!advanced->isHidden());
        QVERIFY(window.findChild<QComboBox *>("compression")->isVisible());
        toggle->setChecked(false); QVERIFY(advanced->isHidden());
        window.findChild<QPushButton *>("recommendedDefaults")->click();
        QCOMPARE(window.findChild<QComboBox *>("compression")->currentData().toString(),QString("zstd,3"));
        QCOMPARE(window.findChild<QComboBox *>("backupType")->currentData().toString(),QString("incremental"));
        QVERIFY(window.findChild<QCheckBox *>("autoMount")->isChecked());
        QVERIFY(window.findChild<QCheckBox *>("includeSystem")->isChecked());
        QVERIFY(window.findChild<QCheckBox *>("packageRecovery")->isChecked());
        QVERIFY(!window.findChild<QPushButton *>("exportRecovery")->isEnabled());
        QVERIFY(!window.findChild<QPushButton *>("restorePackages")->isEnabled());
        window.selectPage(2);
        auto *excludes = window.findChild<ExclusionEditor *>();
        QVERIFY(excludes->isVisible());
        QVERIFY(excludes->text().contains("sh:home/dist/**/node_modules"));
        QVERIFY(excludes->addPath("/home/dist/Videos",true));
        QVERIFY(excludes->text().contains("pp:home/dist/Videos"));
        QVERIFY(excludes->addPath("/usr/local/games",true));
        window.selectPage(3); QCOMPARE(window.findChild<QTableWidget *>("archiveTable")->rowCount(),3);
        window.selectPage(4);
        auto *filter = window.findChild<QLineEdit *>("activityFilter"); filter->setText("xyz-unmatched");
        QVERIFY(window.findChild<QPlainTextEdit *>("activityLog")->toPlainText().contains("No activity matches"));
        filter->clear(); QVERIFY(window.findChild<QPlainTextEdit *>("activityLog")->toPlainText().contains("failed"));
        window.close(); QVERIFY(!window.isVisible());
        settings.sync(); QCOMPARE(settings.value("lastNotification").toString(),QString("sentinel"));
        QCOMPARE(settings.value("notifications").toBool(),false);
    }
    void exclusionsPreserveExistingRulesAndLiteralPaths() {
        ExclusionEditor editor; editor.setSource("/home/example");
        const QString original = "# Existing rules\n\npp:home/example/Downloads\nsh:home/example/**/node_modules\nre:^home/example/tmp[0-9]+/\n";
        editor.setText(original); QCOMPARE(editor.text(),original);
        QVERIFY(editor.addPath("/home/example/Videos [2026]",true));
        QVERIFY(editor.addPath("/home/example/a*?.txt",false));
        QCOMPARE(editor.text(),original + "pp:home/example/Videos [2026]\npf:home/example/a*?.txt\n");
        QVERIFY(editor.addPath("/home/example/a*?.txt",false));
        QCOMPARE(editor.text().count("pf:home/example/a*?.txt"),1);
        QVERIFY(!editor.addPath("/home/example",true));
        QVERIFY(!editor.addPath("/home/example-other/file",false));
        QVERIFY(!editor.addPath("/home/example/bad\npath",true));
        QVERIFY(!editor.addPath("/home/example/trailing ",true));
        auto *list = editor.findChild<QTreeWidget *>();
        list->topLevelItem(3)->setSelected(true); list->topLevelItem(4)->setSelected(true);
        editor.removeSelected(); QCOMPARE(editor.text(),original);
        editor.setSource("/other"); QVERIFY(editor.summary().contains("outside")); QCOMPARE(editor.text(),original);
    }
};
QTEST_MAIN(WindowTests)
#include "test_window.moc"
