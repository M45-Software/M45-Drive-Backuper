// Native Qt tray/window structure follows DriveWatch (MIT; see LICENSE).
#include "window.h"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QIcon statusIcon(const QString &state) {
    QPixmap pix(64, 64); pix.fill(Qt::transparent);
    QPainter p(&pix); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(QColor("#4679ad")); p.drawRoundedRect(7, 8, 46, 49, 7, 7);
    p.setPen(QPen(Qt::white, 4)); p.drawLine(17, 21, 43, 21); p.drawLine(17, 30, 35, 30);
    p.setPen(QPen(qApp->palette().color(QPalette::Window), 3));
    p.setBrush(state == "running" ? QColor("#5ea9ec") : state == "ok" ? QColor("#54b992") : QColor("#efb14f"));
    p.drawEllipse(35, 35, 25, 25);
    p.setPen(QPen(QColor("#172731"), 3));
    if (state == "ok") {p.drawLine(41, 47, 46, 52); p.drawLine(46, 52, 54, 42);}
    else if (state == "running") {p.drawLine(48, 40, 48, 48); p.drawLine(48, 48, 53, 48);}
    else {p.drawLine(48, 40, 48, 49); p.drawPoint(48, 54);}
    return QIcon(pix);
}
QString stamp(const QString &text) { return text.isEmpty() ? "Unavailable" : text; }
QString startupPath() { return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/io.github.m45drivebackuper.desktop"; }
QLabel *label(const QString &text = {}) {
    auto *l = new QLabel(text); l->setTextFormat(Qt::PlainText); l->setWordWrap(true); l->setTextInteractionFlags(Qt::TextSelectableByMouse); return l;
}
QWidget *page(QTabWidget *tabs, const QString &name, QVBoxLayout *&layout) {
    auto *w = new QWidget; layout = new QVBoxLayout(w); layout->setContentsMargins(20,20,20,20); layout->setSpacing(14); tabs->addTab(w, name); return w;
}
}

Window::Window(bool demo) : m_demo(demo), m_settings("M45 Drive Backuper", "M45 Drive Backuper"), m_tray(this) {
    setWindowTitle("M45 Drive Backuper"); resize(900, 740); setWindowIcon(statusIcon("ok"));
    auto *body = new QWidget; auto *outer = new QVBoxLayout(body); outer->setContentsMargins(22,20,22,16); outer->setSpacing(14);
    auto *heading = label("M45 Drive Backuper"); auto font = heading->font(); font.setPointSize(22); font.setBold(true); heading->setFont(font); outer->addWidget(heading);
    outer->addWidget(label(demo ? "PREVIEW · Synthetic data. Machine settings and backups are unchanged." : "Home backups · Borg · KDE Plasma"));
    m_tabs = new QTabWidget; outer->addWidget(m_tabs, 1); setCentralWidget(body);
    QVBoxLayout *layout;
    page(m_tabs, "Overview", layout);
    m_status = label("Reading backup status…"); font = m_status->font(); font.setPointSize(17); font.setBold(true); m_status->setFont(font); layout->addWidget(m_status);
    m_summary = label(); layout->addWidget(m_summary);
    auto *group = new QGroupBox("Current backup job"); auto *form = new QFormLayout(group); form->setVerticalSpacing(16);
    m_destination = label(); m_schedule = label(); m_attempt = label(); m_space = label();
    form->addRow("Destination", m_destination); form->addRow("Next scheduled run", m_schedule); form->addRow("Last attempt", m_attempt); form->addRow("Disk capacity", m_space);
    form->addRow("Backup contents", label("/home/dist · Nested filesystems and configured exclusions are skipped."));
    form->addRow("Storage", label("Deduplicated, Zstandard level 3 · 10 GiB free-space guard")); layout->addWidget(group);
    auto *buttons = new QHBoxLayout;
    m_run = new QPushButton("Back up now"); m_enable = new QPushButton("Enable schedule"); m_disable = new QPushButton("Disable schedule");
    buttons->addWidget(m_run); buttons->addWidget(m_enable); buttons->addWidget(m_disable); buttons->addStretch();
    auto *refresh = new QPushButton("Refresh"); buttons->addWidget(refresh); layout->addLayout(buttons);
    layout->addWidget(label("M45 Drive Backuper uses the existing system service. Settings and backup actions request administrator authentication. Closing this window keeps the tray monitor running.")); layout->addStretch();
    connect(refresh, &QPushButton::clicked, this, &Window::poll);
    connect(m_run, &QPushButton::clicked, this, [this] {action("start");});
    connect(m_enable, &QPushButton::clicked, this, [this] {action("enable");});
    connect(m_disable, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Disable automatic backups?", "Future scheduled backups will stop. A backup already running will finish.") == QMessageBox::Yes) action("disable");
    });
    page(m_tabs, "Archives", layout);
    layout->addWidget(label("Read the repository’s archive catalog to confirm which backups are stored. Authentication is required. This does not run a backup or verify archive contents."));
    m_archiveRefresh = new QPushButton("Read archive catalog…"); layout->addWidget(m_archiveRefresh, 0, Qt::AlignLeft);
    m_archives = new QPlainTextEdit; m_archives->setReadOnly(true); m_archives->setPlainText("Archive catalog has not been read.\n\nA completed service attempt is shown separately from stored archives.\nCheckpoint archives may be incomplete."); layout->addWidget(m_archives);
    connect(m_archiveRefresh, &QPushButton::clicked, this, [this] {action("archives");});
    page(m_tabs, "Journal", layout);
    layout->addWidget(label("Latest 250 service journal entries · refreshed every 15 seconds. Borg currently reports statistics after each run, so there is no live percentage."));
    m_logs = new QPlainTextEdit; m_logs->setReadOnly(true); layout->addWidget(m_logs);
    page(m_tabs, "Settings", layout);
    auto *settingsForm = new QFormLayout; settingsForm->setVerticalSpacing(10);
    m_mount = new QLineEdit; settingsForm->addRow("Backup mount path", m_mount);
    auto *detect = new QPushButton("Use detected path for the existing backup UUID"); settingsForm->addRow("", detect);
    connect(detect, &QPushButton::clicked, this, [this] {
        const auto mounts = m_snapshot.value("actual_mounts").toArray();
        if (mounts.size() == 1) m_mount->setText(mounts[0].toString());
        else QMessageBox::information(this, "Backup drive", "A single matching mounted filesystem could not be found. Connect the backup drive and refresh.");
    });
    m_interval = new QComboBox;
    for (int h : {1,2,3,4,6,8,12,24}) m_interval->addItem(h == 24 ? "Daily at midnight" : QString("Every %1 hour(s)").arg(h), h);
    settingsForm->addRow("Backup schedule", m_interval);
    auto spin = [] {auto *s = new QSpinBox; s->setRange(0,365); return s;};
    m_hourly = spin(); m_daily = spin(); m_weekly = spin(); m_monthly = spin();
    auto *retention = new QWidget; auto *retentionLayout = new QHBoxLayout(retention); retentionLayout->setContentsMargins(0,0,0,0);
    for (auto pair : {qMakePair(QString("Hourly"),m_hourly),qMakePair(QString("Daily"),m_daily),qMakePair(QString("Weekly"),m_weekly),qMakePair(QString("Monthly"),m_monthly)}) {
        retentionLayout->addWidget(label(pair.first)); retentionLayout->addWidget(pair.second);
    }
    settingsForm->addRow("Archives to keep", retention); layout->addLayout(settingsForm);
    layout->addWidget(label("Retention deletes older archives during the next backup’s cleanup. Keep at least one bucket above zero. Calendar schedules include the existing boot trigger and up to five minutes of random delay."));
    layout->addWidget(label("Exclusions · Borg patterns, one per line; # starts a comment. Changes affect future backups."));
    m_excludes = new QPlainTextEdit; m_excludes->setMinimumHeight(120); layout->addWidget(m_excludes, 1);
    m_save = new QPushButton("Review and save backup settings…"); layout->addWidget(m_save,0,Qt::AlignLeft); connect(m_save,&QPushButton::clicked,this,&Window::saveSettings);
    m_startup = new QCheckBox("Start tray monitor at login"); m_notifications = new QCheckBox("Notify when backup status needs attention");
    bool startup = true; QFile startupFile(startupPath()); if (startupFile.open(QIODevice::ReadOnly)) startup = !startupFile.readAll().contains("Hidden=true");
    m_startup->setChecked(startup); m_notifications->setChecked(m_settings.value("notifications",true).toBool());
    layout->addWidget(m_startup); layout->addWidget(m_notifications);
    connect(m_startup,&QCheckBox::toggled,this,&Window::setStartup);
    connect(m_notifications,&QCheckBox::toggled,this,[this](bool on) {if (!m_demo) m_settings.setValue("notifications",on);});
    m_feedback = label(); outer->addWidget(m_feedback);
    m_menu = new QMenu(this); m_miniState = m_menu->addAction("Reading backups…"); m_miniState->setEnabled(false);
    m_miniNext = m_menu->addAction("Waiting for service status"); m_miniNext->setEnabled(false); m_menu->addSeparator();
    m_menu->addAction("Open M45 Drive Backuper",this,&Window::Show);
    m_runAction = m_menu->addAction("Back up now…",this,[this] {action("start");});
    m_menu->addAction("Settings…",this,[this] {selectPage(3); Show();});
    m_menu->addAction("View journal",this,[this] {selectPage(2); Show();});
    m_menu->addSeparator(); m_menu->addAction("Quit monitor",qApp,&QApplication::quit);
    m_tray.setIcon(statusIcon("warning")); m_tray.setContextMenu(m_menu);
    connect(&m_tray,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason why) {
        if (why == QSystemTrayIcon::Trigger) m_menu->popup(m_tray.geometry().isValid() ? m_tray.geometry().center() : QCursor::pos());
        if (why == QSystemTrayIcon::DoubleClick) Show();
    });
    if (!m_demo) m_tray.show();
    if (m_demo) {
        m_snapshot = {{"collected_at",QDateTime::currentSecsSinceEpoch()}, {"mounted",false},
            {"actual_mounts",QJsonArray{"/media/dist/Backup-SSD"}},
            {"config",QJsonObject{{"mountpoint","/media/dist/43b29217-193a-4f61-99ff-01a137b0c8f2"},{"uuid","43b29217-193a-4f61-99ff-01a137b0c8f2"},{"hours",6},{"hourly",4},{"daily",7},{"weekly",4},{"monthly",6},{"excludes","# Caches and regenerable downloads\npp:home/dist/Downloads\npp:home/dist/.cache\nsh:home/dist/**/node_modules"}}},
            {"service",QJsonObject{{"ActiveState","failed"},{"Result","exit-code"},{"ExecMainExitTimestamp","Sun 2026-11-08 06:03:58 MST"}}},
            {"timer",QJsonObject{{"ActiveState","active"},{"NextElapseUSecRealtime","Sun 2026-11-08 12:03:44 MST"}}},
            {"logs","2026-11-08T06:03:58 Backup failed: expected backup filesystem is not mounted.\n2026-11-08T06:03:58 local-system-backup.service: Failed with result 'exit-code'."}};
        loadSettings(); render(); setBusy(false); m_feedback->setText("Preview mode · Backup actions are disabled.");
    } else {
        auto *timer = new QTimer(this); connect(timer,&QTimer::timeout,this,&Window::poll); timer->start(15000); poll();
    }
}

void Window::poll() {
    if (m_demo || m_polling || m_busy) return;
    m_polling = true;
    auto *process = new QProcess(this); auto *timeout = new QTimer(process); timeout->setSingleShot(true);
    connect(timeout,&QTimer::timeout,process,&QProcess::kill); timeout->start(45000);
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_polling = false; m_status->setText("Monitoring unavailable");
            m_summary->setText(process->errorString()); m_tray.setIcon(statusIcon("warning"));
            m_snapshot = {}; setBusy(false);
            process->deleteLater();
        }
    });
    connect(process,&QProcess::finished,this,[this,process](int code,QProcess::ExitStatus) {
        m_polling = false;
        const auto doc = QJsonDocument::fromJson(process->readAllStandardOutput());
        if (code || !doc.isObject() || !doc.object().contains("config")) {
            m_status->setText("Monitoring unavailable");
            m_summary->setText(doc.object().value("error").toString("Could not read backup status. Check that M45 Drive Backuper is installed and the existing backup service is available."));
            m_tray.setIcon(statusIcon("warning")); m_tray.setToolTip("M45 Drive Backuper — monitoring unavailable");
            m_miniState->setText("Monitoring unavailable"); m_miniNext->setText("Last readings may be out of date");
            m_snapshot = {}; setBusy(false);
        } else {m_snapshot = doc.object(); if (!m_loaded) loadSettings(); render();}
        process->deleteLater();
    });
    process->start("/usr/bin/python3",{"/usr/libexec/m45-drive-backuper/backend","status"});
}

void Window::loadSettings() {
    const auto c = m_snapshot.value("config").toObject();
    m_revision = c.value("revision").toString();
    m_mount->setText(c.value("mountpoint").toString());
    const int index = m_interval->findData(c.value("hours").toInt()); m_interval->setCurrentIndex(index);
    m_hourly->setValue(c.value("hourly").toInt()); m_daily->setValue(c.value("daily").toInt());
    m_weekly->setValue(c.value("weekly").toInt()); m_monthly->setValue(c.value("monthly").toInt());
    m_excludes->setPlainText(c.value("excludes").toString()); m_loaded = true;
}

void Window::render() {
    const auto s = m_snapshot.value("service").toObject(), t = m_snapshot.value("timer").toObject(), c = m_snapshot.value("config").toObject();
    const auto active = s.value("ActiveState").toString();
    const bool running = active == "activating" || active == "active", mounted = m_snapshot.value("mounted").toBool();
    const bool enabled = t.value("ActiveState") == "active";
    const bool failed = active == "failed" || (!s.value("Result").toString().isEmpty() && s.value("Result") != "success");
    QString title, explanation, state = "warning";
    if (running) {title = "Backup is running"; explanation = "Follow the Journal tab for Borg output. Closing the monitor does not stop this backup."; state = "running";}
    else if (!mounted) {
        title = "Backup destination needs attention";
        const auto mounts = m_snapshot.value("actual_mounts").toArray();
        explanation = mounts.isEmpty() ? "The expected backup filesystem is not mounted. Connect the drive before the next run." : "The backup drive is mounted at " + mounts[0].toString() + ", but the job checks a different path. Use the detected path in Settings to correct it.";
    } else if (failed) {title = "Last backup attempt failed"; explanation = "The backup destination is available. Review the Journal tab for the failure reason before retrying.";}
    else if (!enabled) {title = "Automatic backups are disabled"; explanation = "Enable the schedule to resume automatic backups.";}
    else if (s.value("ExecMainExitTimestamp").toString().isEmpty()) {title = "Waiting for the first backup result"; explanation = "The schedule is active. Read the archive catalog to inspect stored backups.";}
    else {title = "Last backup attempt completed"; explanation = "The service completed successfully; Borg warnings may still appear in the journal. Read the archive catalog to confirm stored backups."; state = "ok";}
    m_status->setText(title); m_summary->setText(explanation);
    m_destination->setText(c.value("mountpoint").toString() + "\nUUID: " + c.value("uuid").toString());
    m_schedule->setText(enabled ? stamp(t.value("NextElapseUSecRealtime").toString()) : "Schedule disabled");
    m_attempt->setText(stamp(s.value("ExecMainExitTimestamp").toString()) + " · " + (running ? "Running" : stamp(s.value("Result").toString())));
    m_space->setText(mounted ? QString("%1 GiB available / %2 GiB total").arg(m_snapshot.value("free").toDouble()/1073741824.0,0,'f',1).arg(m_snapshot.value("total").toDouble()/1073741824.0,0,'f',1) : "Unavailable at configured mount path");
    const auto logs = m_snapshot.value("logs").toString();
    const auto logError = m_snapshot.value("log_error").toString();
    const auto text = logs + (logError.isEmpty() ? "" : "\nJournal access: " + logError);
    if (m_logs->toPlainText() != text) m_logs->setPlainText(text);
    m_miniState->setText(title); m_miniNext->setText(enabled ? "Next: " + stamp(t.value("NextElapseUSecRealtime").toString()) : "Schedule disabled");
    m_tray.setIcon(statusIcon(state)); m_tray.setToolTip("M45 Drive Backuper — " + title + "\n" + explanation);
    if (!m_demo && state == "warning" && m_notifications->isChecked()) {
        const auto key = title + s.value("ExecMainExitTimestamp").toString();
        if (m_settings.value("lastNotification").toString() != key) {
            m_tray.showMessage("M45 Drive Backuper — " + title, explanation, QSystemTrayIcon::Warning, 10000);
            m_settings.setValue("lastNotification",key);
        }
    }
    setBusy(m_busy);
}

void Window::setBusy(bool busy) {
    m_busy = busy;
    const bool available = !busy && !m_demo && !m_snapshot.isEmpty();
    const auto active = m_snapshot.value("service").toObject().value("ActiveState").toString();
    const bool running = active == "active" || active == "activating" || active == "deactivating";
    m_save->setEnabled(available && !running); m_run->setEnabled(available && !running && m_snapshot.value("mounted").toBool());
    m_runAction->setEnabled(m_run->isEnabled()); m_archiveRefresh->setEnabled(available && !running && m_snapshot.value("mounted").toBool());
    m_enable->setEnabled(available); m_disable->setEnabled(available);
    m_startup->setEnabled(!m_demo); m_notifications->setEnabled(!m_demo);
}

void Window::action(const QString &name, const QString &payload) {
    if (m_demo || m_busy) return;
    setBusy(true); m_feedback->setText("Waiting for administrator authentication…");
    auto *process = new QProcess(this);
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {setBusy(false); m_feedback->setText("Could not start authentication: " + process->errorString()); process->deleteLater();}
    });
    connect(process,&QProcess::finished,this,[this,process,name](int code,QProcess::ExitStatus) {
        const auto output = process->readAllStandardOutput(); const auto doc = QJsonDocument::fromJson(output);
        const auto result = doc.object();
        if (code || !doc.isObject()) {
            m_feedback->setText("Action failed or authentication was cancelled.");
            QMessageBox::warning(this,"M45 Drive Backuper",result.value("error").toString(QString::fromUtf8(process->readAllStandardError()).trimmed()));
        } else if (name == "archives") {
            QStringList rows;
            for (const auto value : result.value("archives").toArray()) {
                const auto a = value.toObject(); rows << a.value("name").toString() + "\n    " + a.value("time").toString() + "\n";
            }
            m_archives->setPlainText(rows.isEmpty() ? "No matching home-* archives were found." : rows.join('\n'));
            m_feedback->setText("Archive catalog read at " + QDateTime::currentDateTime().toString("HH:mm:ss"));
        } else {m_feedback->setText(result.value("message").toString()); if (name == "apply") m_loaded = false;}
        setBusy(false); poll(); process->deleteLater();
    });
    QStringList args{"/usr/libexec/m45-drive-backuper/backend",name}; if (!payload.isEmpty()) args << payload;
    process->start("/usr/bin/pkexec",args);
}

void Window::saveSettings() {
    if (m_interval->currentIndex() < 0) {QMessageBox::warning(this,"Schedule","Choose a supported schedule before saving."); return;}
    QJsonObject data{{"mountpoint",m_mount->text()}, {"hours",m_interval->currentData().toInt()}, {"revision",m_revision},
        {"hourly",m_hourly->value()},{"daily",m_daily->value()},{"weekly",m_weekly->value()},{"monthly",m_monthly->value()},{"excludes",m_excludes->toPlainText()}};
    const QString summary = QString("Save these settings?\n\nMount path: %1\nSchedule: %2\nKeep: %3 hourly, %4 daily, %5 weekly, %6 monthly\n\nThe exclusions shown in the editor will replace the current exclusions.\nOlder archives beyond retention will be deleted during a future backup. Recovery copies of the previous settings will be saved.")
        .arg(m_mount->text(),m_interval->currentText()).arg(m_hourly->value()).arg(m_daily->value()).arg(m_weekly->value()).arg(m_monthly->value());
    if (QMessageBox::question(this,"Review backup settings",summary) == QMessageBox::Yes)
        action("apply",QString::fromLatin1(QJsonDocument(data).toJson(QJsonDocument::Compact).toBase64()));
}

void Window::setStartup(bool enabled) {
    if (m_demo) return;
    const auto path = startupPath(); QDir().mkpath(QFileInfo(path).absolutePath()); QSaveFile file(path);
    const QByteArray text = QByteArray("[Desktop Entry]\nType=Application\nName=M45 Drive Backuper\nExec=/usr/bin/m45-drive-backuper\nIcon=m45-drive-backuper\nTerminal=false\nHidden=") + (enabled ? "false\n" : "true\n");
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit()) {
        m_startup->blockSignals(true); m_startup->setChecked(!enabled); m_startup->blockSignals(false);
        QMessageBox::warning(this,"Login startup","Could not save the autostart setting.");
    }
}
void Window::selectPage(int page) {m_tabs->setCurrentIndex(page);}
void Window::Show() {showNormal(); raise(); activateWindow();}
void Window::closeEvent(QCloseEvent *event) {hide(); event->ignore();}
