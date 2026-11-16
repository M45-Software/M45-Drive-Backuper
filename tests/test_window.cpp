#include "window.h"
#include "reminders.h"
#include <QStandardPaths>
#include "exclusions.h"
#include "recoverybrowser.h"
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
#include <QStyleOptionButton>
#include <QStyle>
#include <QTimer>
#include <QDir>
#include <QMessageBox>
#include <QAbstractItemView>
#include <QMenu>
#include <QAction>

class WindowTests : public QObject {
    Q_OBJECT
private slots:
    void idlePolicyDefaultsToSixHoursAndDisablesUnsupportedControls() {
        Window window(true);
        QCOMPARE(window.m_idleMax->currentData().toInt(),6);
        QVERIFY(!window.m_idleBackup->isChecked()); QVERIFY(!window.m_idleCheck->isChecked());
        window.m_snapshot["idle_policy"]=QJsonObject{{"backup",false},{"check",true},{"max_hours",12},{"available",true},{"revision","policy-revision"},{"message","Busy"}};
        window.m_idleLoaded=false; window.render();
        QCOMPARE(window.m_idleMax->currentData().toInt(),12); QVERIFY(window.m_idleCheck->isChecked());
        window.m_idleMax->setCurrentIndex(window.m_idleMax->findData(24));
        window.render(); QCOMPARE(window.m_idleMax->currentData().toInt(),24);
        QVERIFY(window.m_idleStatus->text().contains("Unsaved"));
        window.m_snapshot["idle_policy"]=QJsonObject{{"backup",false},{"check",false},{"max_hours",6},{"available",false},{"message","Idle reporting unavailable; idle waiting disabled."}};
        window.m_idleLoaded=false; window.render();
        QVERIFY(!window.m_idleBackup->isEnabled()); QVERIFY(!window.m_idleCheck->isEnabled());
        QVERIFY(!window.m_idleMax->isEnabled()); QVERIFY(!window.m_idleSave->isEnabled());
        QVERIFY(window.m_idleStatus->text().contains("unavailable"));
        window.m_snapshot["job_control"]=QJsonObject{{"backup",QJsonObject{{"mode","idle_wait"},{"until",1800000000}}}};
        window.render();
        QCOMPARE(window.m_pauseBackup->text(),QString("Run now"));
        QVERIFY(window.m_backupControlStatus->text().contains("Waiting for idle"));
        QVERIFY(window.m_backupControlStatus->text().contains("Starts by:"));
        QCOMPARE(window.findChild<QAction *>("backup-resume0")->text(),QString("Run waiting job now"));
        QVERIFY(window.m_schedule->text().contains("Idle wait"));
    }

    void jobControlsAreAvailableInWindowAndTrayAndShowPausedState() {
        Window window(true); window.Show();
        window.m_snapshot["mounted"]=true;
        window.m_snapshot["service"]=QJsonObject{{"ActiveState","activating"},{"InvocationID","abc"}};
        window.m_snapshot["job_control"]=QJsonObject{{"backup",QJsonObject{{"can_pause",true},{"paused",false}}}};
        window.render();
        QVERIFY(!window.m_pauseBackup->isHidden());
        QVERIFY(!window.m_cancelBackup->isHidden());
        QVERIFY(window.findChild<QAction *>("backupTrayPause")->isVisible());
        QVERIFY(window.findChild<QAction *>("backupTrayCancel")->isVisible());
        QVERIFY(window.m_menu->actions().contains(window.m_backupControls->menuAction()));
        QVERIFY(window.m_menu->actions().contains(window.m_checkControls->menuAction()));
        window.m_snapshot["job_control"]=QJsonObject{{"backup",QJsonObject{{"mode","postponed"},{"paused",true},{"can_pause",true},{"until",1800000000}}}};
        window.render();
        QCOMPARE(window.m_pauseBackup->text(),QString("Resume"));
        QCOMPARE(window.findChild<QAction *>("backupTrayPause")->text(),QString("Resume backup"));
        QCOMPARE(window.m_trayState,QString("paused"));
        QVERIFY(!window.m_trayAnimation->isActive());
        const auto captures=qEnvironmentVariable("M45_TEST_CAPTURE_DIR");
        if (!captures.isEmpty()) {QDir().mkpath(captures); window.selectPage(0); QCoreApplication::processEvents(); QVERIFY(window.grab().save(captures+"/paused-backup.png"));}
        QVERIFY(window.m_backupControlStatus->text().contains("Resumes:"));
        QVERIFY(window.m_schedule->text().contains("Postponed"));
        window.m_snapshot["service"]=QJsonObject{{"ActiveState","inactive"}};
        window.m_snapshot["job_control"]=QJsonObject{{"check",QJsonObject{{"mode","postponed"},{"until",1800000000}}}};
        window.render();
        QVERIFY(window.m_pauseBackup->isHidden());
        QVERIFY(!window.findChild<QAction *>("backupTrayPause")->isVisible());
        QVERIFY(window.findChild<QAction *>("check-resume0")->isVisible());
        QVERIFY(window.findChild<QAction *>("check-resume0")->text().contains("now"));
        QVERIFY(!window.m_checkNow->isEnabled());
        window.m_snapshot["verification"]=QJsonObject{{"mode","manual"},{"service",QJsonObject{{"ActiveState","active"}}},{"attempt",QJsonObject{{"outcome","running"}}}};
        window.m_snapshot["job_control"]=QJsonObject{{"check",QJsonObject{{"mode","paused"},{"paused",true},{"can_pause",true}}}};
        window.render();
        QCOMPARE(window.m_pauseCheck->text(),QString("Resume"));
        QVERIFY(!window.m_stopCheck->isHidden());
        QVERIFY(window.m_verificationStatus->text().contains("paused"));
        QVERIFY(window.findChild<QAction *>("checkTrayCancel")->isVisible());
        if (!captures.isEmpty()) {
            window.selectPage(3); QCoreApplication::processEvents(); QVERIFY(window.grab().save(captures+"/paused-check.png"));
            window.m_menu->popup(QPoint(20,20)); QCoreApplication::processEvents(); QVERIFY(window.m_menu->grab().save(captures+"/paused-tray.png")); window.m_menu->hide();
        }
    }

    void remindersSurviveRestartAndDoNotTreatFailuresAsSuccess() {
        QTemporaryDir dir;
        const auto now = QDateTime::fromString("2026-11-16T12:00:00Z",Qt::ISODate);
        const auto path = dir.filePath("reminders.ini");
        {
            QSettings settings(path,QSettings::IniFormat);
            QVERIFY(!Reminders::evaluate(settings,"backup",0,{},false,now).notify);
            QVERIFY(!Reminders::evaluate(settings,"backup",7,{},false,now).notify);
        }
        QSettings settings(path,QSettings::IniFormat);
        auto result = Reminders::evaluate(settings,"backup",7,{},false,now.addDays(8));
        QVERIFY(result.notify);
        QVERIFY(!Reminders::evaluate(settings,"backup",7,{},false,now.addDays(8).addSecs(15)).notify);
        QVERIFY(Reminders::evaluate(settings,"backup",7,{},false,now.addDays(9)).notify);
        // Successful runs reset the interval; unknown/failed attempts pass no success.
        QVERIFY(!Reminders::evaluate(settings,"backup",7,now.addDays(9),false,now.addDays(9)).notify);
        QCOMPARE(Reminders::evaluate(settings,"backup",7,{},false,now.addDays(10)).due,now.addDays(16));
        QVERIFY(!Reminders::evaluate(settings,"backup",7,{},true,now.addDays(17)).notify);
        QVERIFY(Reminders::evaluate(settings,"backup",7,{},false,now.addDays(17)).notify);
        settings.setValue("check/snoozedUntil",now.addDays(4));
        QVERIFY(!Reminders::evaluate(settings,"check",1,now.addDays(-10),false,now).notify);
        QVERIFY(Reminders::evaluate(settings,"check",1,{},false,now.addDays(4)).notify);
        // Moving the clock backwards must recover rather than wait indefinitely.
        QCOMPARE(Reminders::evaluate(settings,"backup",7,{},false,now.addDays(-2)).due,now.addDays(5));
    }

    void reminderNotificationsAreOptInAndNeverStartJobs() {
        QTemporaryDir dir;
        struct SettingsPathReset {
            QString original=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
            ~SettingsPathReset(){QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,original);}
        } reset;
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,dir.path());
        const auto now=QDateTime::fromString("2026-11-16T12:00:00Z",Qt::ISODate);
        Window window(true);
        QCOMPARE(window.m_backupReminder->currentData().toInt(),0);
        QCOMPARE(window.m_checkReminder->currentData().toInt(),0);
        window.m_demo=false;
        window.m_snapshot["timer"]=QJsonObject{{"ActiveState","inactive"}};
        window.m_snapshot["service"]=QJsonObject{{"ActiveState","inactive"},{"Result","exit-code"}};
        window.m_snapshot["borg_report"]=QJsonObject{{"outcome","error"}};
        window.m_snapshot["verification"]=QJsonObject{{"mode","manual"}};
        window.m_backupReminder->setCurrentIndex(window.m_backupReminder->findData(7));
        window.m_checkReminder->setCurrentIndex(window.m_checkReminder->findData(30));
        window.m_settings.setValue(window.reminderKey("backup")+"/enabledAt",now.addDays(-8));
        window.m_settings.setValue(window.reminderKey("check")+"/enabledAt",now.addDays(-31));
        QSignalSpy reminders(&window,&Window::reminderRequested);
        const auto before=window.m_snapshot;
        window.updateReminders(now);
        QCOMPARE(reminders.count(),1);
        QCOMPARE(reminders.at(0).at(2).toInt(),0);
        window.updateReminders(now.addSecs(15));
        QCOMPARE(reminders.count(),2);
        QCOMPARE(reminders.at(1).at(2).toInt(),3);
        window.updateReminders(now.addSecs(30));
        QCOMPARE(reminders.count(),2);
        QCOMPARE(window.m_snapshot,before);
        QVERIFY(!window.m_busy);
        // Pause reminders during automatic jobs and active recovery.
        window.m_snapshot["timer"]=QJsonObject{{"ActiveState","active"}};
        window.m_snapshot["verification"]=QJsonObject{{"mode","monthly"}};
        window.updateReminders(now.addDays(2));
        QCOMPARE(reminders.count(),2);
        QVERIFY(window.m_reminderStatus->text().contains("paused"));
        window.m_snapshot["timer"]=QJsonObject{{"ActiveState","inactive"}};
        window.m_snapshot["verification"]=QJsonObject{{"mode","manual"}};
        window.m_snapshot["file_recovery"]=QJsonObject{{"active",true}};
        window.updateReminders(now.addDays(2));
        QCOMPARE(reminders.count(),2);
        window.m_snapshot.remove("file_recovery");
        window.updateReminders(now.addDays(2));
        QCOMPARE(reminders.count(),3);
        // Repository changes cannot inherit an old repository's due reminder.
        auto config=window.m_snapshot.value("config").toObject(); config["repository"]="/different";
        window.m_snapshot["config"]=config;
        window.updateReminders(now.addDays(2));
        QCOMPARE(reminders.count(),3);
        // A reported successful backup postpones the next reminder, even
        // when a failed run follows it; the monitor remembers that success.
        window.m_snapshot["borg_report"]=QJsonObject{{"outcome","success"}};
        window.m_snapshot["activity"]=QJsonArray{QJsonObject{{"timestamp",now.addDays(2).toString(Qt::ISODate)}}};
        window.updateReminders(now.addDays(2));
        QCOMPARE(window.m_settings.value(window.reminderKey("backup")+"/success").toDateTime(),now.addDays(2));
        window.m_snapshot["borg_report"]=QJsonObject{{"outcome","error"}};
        window.updateReminders(now.addDays(3));
        QCOMPARE(window.m_settings.value(window.reminderKey("backup")+"/success").toDateTime(),now.addDays(2));
        window.m_demo=true;
    }
    void recoveryNavigationClearsSelectionAndKeepsDefaultDestination() {
        RecoveryBrowser browser({},"home-test",true);
        QCOMPARE(browser.m_destination->text(),QDir::home().filePath("Recovered Files"));
        browser.m_demo=false; browser.m_entries->selectRow(1);
        QVERIFY(browser.m_recover->isEnabled());
        const QJsonArray entries{QJsonObject{{"name","new.txt"},{"kind","f"},{"token","bmV3LnR4dA=="}}};
        browser.applyListing(QJsonObject{{"path","ZG9jcw=="},{"parent",""},{"display_path","/docs"},{"entries",entries},{"offset",0},{"total",1}});
        QVERIFY(browser.m_entries->selectedItems().isEmpty());
        QVERIFY(!browser.m_recover->isEnabled());
        QCOMPARE(browser.m_location->text(),QString("/docs"));
        browser.setBusy(true); browser.reject();
        QVERIFY(browser.m_status->text().contains("Wait"));
        browser.setBusy(false); browser.reject();
    }

    void runningBackupsAndChecksHaveDistinctAnimatedTrayIcons() {
        Window window(true);
        auto snapshot=window.m_snapshot;
        snapshot["mounted"]=true; snapshot["source_ready"]=true;
        snapshot["timer"]=QJsonObject{{"ActiveState","active"}};
        snapshot["service"]=QJsonObject{{"ActiveState","activating"},{"Result","success"},{"ExecMainExitTimestamp","today"}};
        snapshot["borg_report"]=QJsonObject{{"outcome","success"}};
        window.m_snapshot=snapshot; window.render();
        QCOMPARE(window.m_trayState,QString("backup-running"));
        QVERIFY(window.m_trayAnimation->isActive());
        auto frame=window.m_tray.icon().pixmap(64,64).toImage();
        QTest::qWait(190);
        QVERIFY(frame!=window.m_tray.icon().pixmap(64,64).toImage());
        snapshot["verification"]=QJsonObject{{"mode","manual"},{"scope","latest"},
            {"service",QJsonObject{{"ActiveState","active"}}},{"attempt",QJsonObject{{"outcome","running"}}}};
        window.m_snapshot=snapshot; window.render();
        QCOMPARE(window.m_trayState,QString("check-running"));
        QVERIFY(window.m_tray.toolTip().contains("A backup is waiting"));
        QVERIFY(frame!=window.m_tray.icon().pixmap(64,64).toImage());
        snapshot["file_recovery"]=QJsonObject{{"active",true},{"operation","recover"}};
        window.m_snapshot=snapshot; window.render();
        QCOMPARE(window.m_trayState,QString("recovery-running"));
        QVERIFY(window.m_tray.toolTip().contains("A backup is waiting"));
        snapshot.remove("file_recovery");
        auto verification=snapshot["verification"].toObject();
        verification["service"]=QJsonObject{{"ActiveState","inactive"}};
        verification["attempt"]=QJsonObject{{"outcome","success"}};
        snapshot["verification"]=verification;
        snapshot["service"]=QJsonObject{{"ActiveState","inactive"},{"Result","success"},{"ExecMainExitTimestamp","today"}};
        window.m_snapshot=snapshot; window.render();
        QVERIFY(!window.m_trayAnimation->isActive());
        const auto key=window.m_tray.icon().cacheKey(); QTest::qWait(190);
        QCOMPARE(window.m_tray.icon().cacheKey(),key);
    }

    void selectingAnArchiveKeepsItsIdWhenRowsAreSorted() {
        Window window(true);
        window.showArchives(QJsonArray{QJsonObject{{"name","home-old"},{"time","2026-11-15"},{"id",QString(64,'a')}},
                                      QJsonObject{{"name","home-new"},{"time","2026-11-16"},{"id",QString(64,'b')}}});
        QCOMPARE(window.m_archives->item(0,1)->text(),QString("home-new"));
        QCOMPARE(window.m_archives->item(0,1)->data(Qt::UserRole).toString(),QString(64,'b'));
    }

    void verificationDefaultsToManualAndTracksFailuresIndependently() {
        Window window(true);
        QCOMPARE(window.m_verificationMode->currentData().toString(),QString("manual"));
        QCOMPARE(window.m_verificationScope->currentData().toString(),QString("latest"));
        QVERIFY(window.m_verificationStatus->text().contains("No successful check"));
        QVERIFY(!window.m_checkNow->isEnabled());
        auto snapshot=window.m_snapshot;
        snapshot["mounted"]=true; snapshot["source_ready"]=true;
        snapshot["timer"]=QJsonObject{{"ActiveState","active"}};
        snapshot["service"]=QJsonObject{{"ActiveState","inactive"},{"Result","success"},{"ExecMainExitTimestamp","today"}};
        snapshot["borg_report"]=QJsonObject{{"outcome","success"}};
        snapshot["verification"]=QJsonObject{{"mode","manual"},{"revision","revision"},
            {"service",QJsonObject{{"ActiveState","inactive"}}},
            {"attempt",QJsonObject{{"outcome","error"},{"message","Read failed"}}},
            {"last_success",QJsonObject{{"finished_at","2026-11-15T12:00:00.000+00:00"}}}};
        window.m_snapshot=snapshot; window.render();
        QCOMPARE(window.m_status->text(),QString("Backup verification needs attention"));
        QVERIFY(window.m_verificationStatus->text().contains("Read failed"));
        QVERIFY(window.m_verificationStatus->text().contains("Last successful check"));
        QCOMPARE(window.m_snapshot.value("borg_report").toObject().value("outcome").toString(),QString("success"));
        auto verification=snapshot["verification"].toObject();
        verification["service"]=QJsonObject{{"ActiveState","active"}};
        verification["attempt"]=QJsonObject{{"outcome","running"}};
        snapshot["verification"]=verification; window.m_snapshot=snapshot; window.render();
        QCOMPARE(window.m_status->text(),QString("Checking backup integrity"));
        verification["service"]=QJsonObject{{"ActiveState","inactive"}};
        verification["attempt"]=QJsonObject{{"outcome","running"}};
        snapshot["verification"]=verification; window.m_snapshot=snapshot; window.render();
        QVERIFY(window.m_verificationStatus->text().contains("interrupted"));
    }

    void missingHistoryIsNeutralWhileRestoredResultsKeepTheirOutcome() {
        Window window(true);
        auto snapshot = window.m_snapshot;
        snapshot["mounted"] = true;
        snapshot["source_ready"] = true;
        snapshot["timer"] = QJsonObject{{"ActiveState", "active"}};
        snapshot["service"] = QJsonObject{{"ActiveState", "inactive"}, {"Result", "success"}};
        snapshot["borg_report"] = QJsonObject{{"outcome", "unknown"}};
        window.m_snapshot = snapshot;
        window.render();
        QCOMPARE(window.m_status->text(), QString("Waiting for a backup result"));
        QVERIFY(window.m_lastNotification.isEmpty());
        auto service = snapshot["service"].toObject();
        service["ExecMainExitTimestamp"] = "Mon 2026-11-09 17:00:00 MST";
        snapshot["service"] = service;
        snapshot["borg_report"] = QJsonObject{{"outcome", "success"}};
        snapshot["previous_attempt"] = true;
        window.m_snapshot = snapshot;
        window.render();
        QCOMPARE(window.m_status->text(), QString("Last backup attempt completed"));
    }
    void loginWindowPreferenceDefaultsToTrayAndPersists() {
        QSettings settings("M45 Drive Backuper", "M45 Drive Backuper");
        settings.remove("showWindowAtLogin"); settings.sync();
        {
            Window window(true); window.m_demo = false;
            auto *checkbox = window.findChild<QCheckBox *>("showWindowAtLogin");
            QVERIFY(checkbox); QVERIFY(!checkbox->isChecked());
            checkbox->setChecked(true);
            QCOMPARE(settings.value("showWindowAtLogin").toBool(), true);
        }
        {
            Window window(true); window.m_demo = false;
            auto *checkbox = window.findChild<QCheckBox *>("showWindowAtLogin");
            QVERIFY(checkbox->isChecked());
            checkbox->setChecked(false);
        }
        QCOMPARE(settings.value("showWindowAtLogin").toBool(), false);
    }

    void desktopPaletteChangesRefreshExistingWidgets() {
        struct PaletteReset {
            QPalette palette = qApp->palette();
            ~PaletteReset() { qApp->setPalette(palette); }
        } reset;
        const auto light = qApp->style()->standardPalette();
        auto dark = light;
        dark.setColor(QPalette::Window, QColor("#252930"));
        dark.setColor(QPalette::WindowText, QColor("#e3e7ed"));
        dark.setColor(QPalette::Base, QColor("#1b1e24"));
        dark.setColor(QPalette::Text, QColor("#e3e7ed"));
        dark.setColor(QPalette::AlternateBase, QColor("#292e36"));
        dark.setColor(QPalette::Button, QColor("#303640"));
        dark.setColor(QPalette::ButtonText, QColor("#e3e7ed"));
        dark.setColor(QPalette::Highlight, QColor("#347fba"));
        dark.setColor(QPalette::HighlightedText, Qt::white);
        dark.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#949dab"));
        qApp->setPalette(light);
        Window window(true); window.Show(); window.selectPage(1);
        auto *checkbox = window.findChild<QCheckBox *>("packageRecovery");
        for (const auto &palette : {dark, light, dark}) {
            qApp->setPalette(palette);
            QCoreApplication::processEvents();
            const auto image = window.grab().toImage();
            QCOMPARE(image.pixelColor(window.width()-5, window.height()/2), palette.color(QPalette::Base));
            QCOMPARE(checkbox->palette().color(QPalette::WindowText), palette.color(QPalette::Text));
            QCOMPARE(window.m_source->palette().color(QPalette::Text), palette.color(QPalette::Text));
            QCOMPARE(window.m_save->palette().color(QPalette::Disabled, QPalette::ButtonText),
                     palette.color(QPalette::Disabled, QPalette::ButtonText));
            checkbox->setFocus();
            checkbox->setChecked(false);
            const auto unchecked = checkbox->grab().toImage();
            QTest::keyClick(checkbox, Qt::Key_Space);
            QVERIFY(checkbox->isChecked());
            const auto checked = checkbox->grab().toImage();
            QVERIFY(checked != unchecked);
            QStyleOptionButton option; option.initFrom(checkbox);
            const auto indicator = checkbox->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, checkbox);
            // An unchecked box must have a visible outline, not disappear into the page.
            bool outline = false;
            for (int y=indicator.top(); y<=indicator.bottom(); ++y)
                for (int x=indicator.left(); x<=indicator.right(); ++x)
                    outline |= unchecked.pixelColor(x,y) == palette.color(QPalette::Text);
            QVERIFY(outline);
        }
    }
    void popupDisabledAndRecoveryControlsFollowThemeChanges() {
        struct PaletteReset {QPalette palette=qApp->palette(); ~PaletteReset(){qApp->setPalette(palette);}} reset;
        auto light=qApp->style()->standardPalette(), dark=light;
        dark.setColor(QPalette::Window,QColor("#252930")); dark.setColor(QPalette::WindowText,QColor("#e3e7ed"));
        dark.setColor(QPalette::Base,QColor("#1b1e24")); dark.setColor(QPalette::Text,QColor("#e3e7ed"));
        dark.setColor(QPalette::Button,QColor("#303640")); dark.setColor(QPalette::ButtonText,QColor("#e3e7ed"));
        dark.setColor(QPalette::PlaceholderText,QColor("#a0a9b6"));
        dark.setColor(QPalette::Disabled,QPalette::Text,QColor("#949dab"));
        dark.setColor(QPalette::Disabled,QPalette::Base,QColor("#20242c"));
        dark.setColor(QPalette::Disabled,QPalette::Button,QColor("#292d35"));
        dark.setColor(QPalette::Disabled,QPalette::ButtonText,QColor("#919bab"));
        dark.setColor(QPalette::Disabled,QPalette::Mid,QColor("#454b56"));
        qApp->setPalette(light);
        Window window(true); window.Show(); window.selectPage(3);
        auto *popup=window.m_verificationScope->view(); popup->ensurePolished();
        RecoveryBrowser browser({},"home-test",true,&window); browser.show();
        browser.m_destination->setEnabled(false);
        QMessageBox message(QMessageBox::Information,"Theme","System theme text",QMessageBox::Ok,&window);
        message.show();
        for(const auto &palette:{dark,light,dark}) {
            qApp->setPalette(palette); QCoreApplication::processEvents();
            QCOMPARE(popup->palette().color(QPalette::Base),palette.color(QPalette::Base));
            QCOMPARE(popup->palette().color(QPalette::Text),palette.color(QPalette::Text));
            QCOMPARE(browser.m_search->palette().color(QPalette::PlaceholderText),palette.color(QPalette::PlaceholderText));
            QCOMPARE(browser.m_destination->palette().color(QPalette::Disabled,QPalette::Text),palette.color(QPalette::Disabled,QPalette::Text));
            QCOMPARE(browser.m_destination->palette().color(QPalette::Disabled,QPalette::Base),palette.color(QPalette::Disabled,QPalette::Base));
            QCOMPARE(browser.m_recover->palette().color(QPalette::Disabled,QPalette::Button),palette.color(QPalette::Disabled,QPalette::Button));
            QCOMPARE(browser.m_entries->viewport()->palette().color(QPalette::Base),palette.color(QPalette::Base));
            QCOMPARE(message.palette().color(QPalette::Window),palette.color(QPalette::Window));
            const auto pixel=browser.m_destination->grab().toImage().pixelColor(browser.m_destination->width()-12,browser.m_destination->height()/2);
            QCOMPARE(pixel,palette.color(QPalette::Disabled,QPalette::Base));
        }
    }

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
