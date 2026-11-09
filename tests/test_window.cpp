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
#include <QListWidget>
#include <QClipboard>
#include <QStackedWidget>

class WindowTests : public QObject {
    Q_OBJECT
private slots:
    void activityEventsFilterCopyAndPreserveLiteralMultilineOutput() {
        Window window(true); window.Show(); window.selectPage(4);
        const QString message = "<file>&.txt: Permission denied\nOriginal size:      12.4 GB\nDeduplicated size:   42.0 MB";
        window.m_snapshot["activity"] = QJsonArray{
            QJsonObject{{"timestamp","2026-11-08T13:03:44Z"},{"level","INFO"},{"source","Borg create"},{"message","Started"}},
            QJsonObject{{"timestamp","2026-11-08T13:03:58Z"},{"level","WARNING"},{"source","Borg create"},{"message",message}},
            QJsonObject{{"timestamp",""},{"level","ERROR"},{"source","Service"},{"message","Failed"}}};
        window.m_snapshot["log_error"] = "Permission denied";
        window.m_snapshot["log_truncated"] = true;
        window.renderLog();
        QCOMPARE(window.m_activity->rowCount(),3);
        QCOMPARE(window.m_activity->item(1,3)->text(),message);
        QVERIFY(window.m_activity->item(0,0)->text().contains("2026"));
        QCOMPARE(window.m_activity->item(2,0)->text(),QString("—"));
        window.m_logLevel->setCurrentIndex(1);
        QCOMPARE(window.m_activity->rowCount(),2);
        window.m_logSearch->setText("<file>&");
        QCOMPARE(window.m_activity->rowCount(),1);
        window.findChild<QPushButton *>("copyActivity")->click();
        QVERIFY(QApplication::clipboard()->text().contains(message));
        QVERIFY(!QApplication::clipboard()->text().contains("Started"));
        window.findChild<QCheckBox *>("rawActivity")->setChecked(true);
        QVERIFY(window.m_logs->isVisible()); QVERIFY(!window.m_activity->isVisible());
        window.m_logSearch->setText("no-match");
        QCOMPARE(window.m_activity->rowCount(),0);
        QVERIFY(window.m_logCount->text().contains("No activity matches"));
        QVERIFY(window.m_logCount->text().contains("500"));
        QVERIFY(window.m_logAccess->isVisible());
        QVERIFY(window.m_logs->toPlainText().contains("Journal access: Permission denied"));
    }
    void sidebarPreservesEditsAndStatusActionsFollowVisibleProblem() {
        Window window(true); window.Show();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        QVERIFY(navigation);
        navigation->setCurrentRow(1);
        QCOMPARE(window.m_tabs->currentIndex(),1);
        window.m_source->setText("/home/example/Documents");
        QVERIFY(window.m_save->isEnabled());
        navigation->setFocus();
        QTest::keyClick(navigation,Qt::Key_Down);
        QCOMPARE(window.m_tabs->currentIndex(),2);
        QVERIFY(window.m_editBar->isVisible());
        window.selectPage(0);
        QCOMPARE(navigation->currentRow(),0);
        QVERIFY(window.m_editBar->isVisible());
        QCOMPARE(window.m_source->text(),QString("/home/example/Documents"));

        // Resolve the visible destination problem even if the last run also failed.
        auto *resolve = window.findChild<QPushButton *>("statusAction");
        QVERIFY(resolve->text().contains("plan"));
        resolve->click(); QCOMPARE(window.m_tabs->currentIndex(),1);
        auto snapshot = window.m_snapshot; snapshot["mounted"] = true;
        window.m_snapshot = snapshot; window.render(); window.selectPage(0);
        QVERIFY(resolve->text().contains("activity"));
        resolve->click(); QCOMPARE(window.m_tabs->currentIndex(),4);
        QCOMPARE(navigation->currentRow(),4);
        QCOMPARE(window.m_source->text(),QString("/home/example/Documents"));
    }
    void borgDiagnosticsReachOverviewActivityAndNotifications() {
        Window window(true); window.Show(); window.m_notifications->setChecked(true);
        QSignalSpy notifications(&window,&Window::attentionRequested);
        auto snapshot = window.m_snapshot;
        snapshot["mounted"] = true; snapshot["source_ready"] = true;
        auto service = QJsonObject{{"ActiveState","failed"},{"Result","exit-code"},{"ExecMainCode","1"},
            {"ExecMainStatus","1"},{"InvocationID","warning-run"},{"ExecMainExitTimestamp","warning-time"}};
        snapshot["service"] = service;
        snapshot["borg_report"] = QJsonObject{{"outcome","warning"},{"issues",QJsonArray{
            QJsonObject{{"level","WARNING"},{"operation","create"},{"message","example.txt: Permission denied"}}}}};
        snapshot["logs"] = "WARNING · Borg create: example.txt: Permission denied";
        window.m_snapshot=snapshot; window.render();
        QCOMPARE(window.m_status->text(),QString("Backup completed with warnings"));
        QVERIFY(window.m_summary->text().contains("Permission denied"));
        QVERIFY(window.m_miniState->text().contains("warnings"));
        window.selectPage(4); QVERIFY(window.m_borgMessages->isVisible());
        QVERIFY(window.m_borgMessages->toPlainText().contains("example.txt"));
        QCOMPARE(notifications.count(),1); QCOMPARE(notifications[0][2].toBool(),false);
        window.render(); QCOMPARE(notifications.count(),1);

        service["Result"]="success"; service["ActiveState"]="inactive"; service["ExecMainStatus"]="0";
        service["InvocationID"]="clean-run"; snapshot["service"]=service;
        snapshot["borg_report"]=QJsonObject{{"outcome","success"},{"issues",QJsonArray{}}};
        window.m_snapshot=snapshot; window.render();
        QVERIFY(window.m_status->text().contains("completed")); QVERIFY(window.m_borgMessages->isHidden());
        QVERIFY(window.m_borgMessages->toPlainText().isEmpty());

        service["Result"]="exit-code"; service["ActiveState"]="failed"; service["ExecMainStatus"]="2";
        service["InvocationID"]="error-run"; snapshot["service"]=service;
        snapshot["borg_report"]=QJsonObject{{"outcome","error"},{"issues",QJsonArray{
            QJsonObject{{"level","ERROR"},{"operation","compact"},{"message","No space left"}}}}};
        window.m_snapshot=snapshot; window.render();
        QCOMPARE(window.m_status->text(),QString("Last backup attempt failed"));
        QVERIFY(window.m_borgMessages->toPlainText().contains("Borg compact: No space left"));
        QCOMPARE(notifications.count(),2); QCOMPARE(notifications[1][2].toBool(),true);

        // A failed preflight with exit 1 must never look like a Borg warning.
        service["ExecMainStatus"]="1"; service["InvocationID"]="preflight-run"; snapshot["service"]=service;
        snapshot["borg_report"]=QJsonObject{{"outcome","error"},{"issues",QJsonArray{}}};
        window.m_snapshot=snapshot; window.render();
        QCOMPARE(window.m_status->text(),QString("Last backup attempt failed"));
        QCOMPARE(notifications.count(),3); QCOMPARE(notifications[2][2].toBool(),true);

        // Timeout/signal failure overrides an earlier warning result.
        service["Result"]="timeout"; service["InvocationID"]="timeout-run"; snapshot["service"]=service;
        snapshot["borg_report"]=QJsonObject{{"outcome","warning"},{"issues",QJsonArray{}}};
        window.m_snapshot=snapshot; window.render();
        QCOMPARE(window.m_status->text(),QString("Last backup attempt failed"));
        QCOMPARE(notifications.count(),4); QCOMPARE(notifications[3][2].toBool(),true);
    }
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
        QVERIFY(window.findChild<QCheckBox *>("packageRecovery")->isVisible());
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
    void recoveryControlsDistinguishSavedSettingsFromUnsavedEdits() {
        Window window(true); window.Show(); window.selectPage(5); QTest::qWait(10);
        auto *toggle=window.findChild<QCheckBox *>("recoveryToggle");
        auto *plan=window.findChild<QCheckBox *>("packageRecovery");
        auto *status=window.findChild<QLabel *>("recoveryStatus");
        QVERIFY(toggle->isVisible()); QVERIFY(toggle->isChecked()); QVERIFY(plan->isChecked());
        QVERIFY(status->text().contains("included in every backup"));
        toggle->click(); QVERIFY(!plan->isChecked());
        QVERIFY(status->text().contains("Saved plan: package recovery is included"));
        QVERIFY(status->text().contains("Unsaved change: disable"));
        QVERIFY(window.findChild<QPushButton *>("saveSettings")->isEnabled());
        plan->setChecked(true); QVERIFY(toggle->isChecked()); QVERIFY(!status->text().contains("Unsaved"));
        QVERIFY(!window.findChild<QPushButton *>("saveSettings")->isEnabled());
    }
    void exclusionCountSurvivesFilteringAndTheListStaysUsable() {
        struct FontReset {QFont font; ~FontReset() {qApp->setFont(font);}} reset{qApp->font()};
        auto large=reset.font; large.setPointSize(14); qApp->setFont(large);
        Window window(true); window.resize(1000,790); window.Show(); window.selectPage(2);
        auto *editor=window.findChild<ExclusionEditor *>();
        QString rules; for (int i=0;i<17;++i) rules+=QString("pp:home/dist/folder%1\n").arg(i);
        for (int i=0;i<11;++i) rules+=QString("sh:home/dist/**/cache%1\n").arg(i);
        editor->setText(rules); QTest::qWait(20);
        auto *list=editor->findChild<QTreeWidget *>(); QVERIFY(list->height()>=180);
        QVERIFY2(window.height()<=790,qPrintable(QString("Window grew to %1 pixels").arg(window.height())));
        QVERIFY(editor->summary().contains("28 exclusion rules"));
        auto *search=editor->findChild<QLineEdit *>("exclusionFilter"); search->setText("no-match");
        QVERIFY(editor->summary().contains("28 exclusion rules"));
        QVERIFY(editor->findChild<QLabel *>("exclusionMatches")->text().contains("Showing 0 of 28"));
        QCOMPARE(editor->text(),rules); search->clear();
        QCOMPARE(list->topLevelItemCount(),28);
        QVERIFY(editor->findChild<QLabel *>("exclusionMatches")->text().contains("All 28 rules"));
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
