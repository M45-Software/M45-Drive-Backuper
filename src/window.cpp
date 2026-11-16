// Native Qt tray/window structure follows DriveWatch (MIT; see LICENSE).
#include "window.h"
#include "exclusions.h"
#include "recoverybrowser.h"
#include "theme.h"
#include "reminders.h"
#include <QCryptographicHash>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QEvent>
#include <QStyleOptionButton>
#include <QComboBox>
#include <QCursor>
#include <QClipboard>
#include <QHeaderView>
#include <QTableWidget>
#include <QTabBar>
#include <QListWidget>
#include <QStyle>
#include <QStackedWidget>
#include <QRegularExpression>
#include <QFontDatabase>
#include <QScrollBar>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
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
#include <QSet>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
// Keep native labels, focus, keyboard behavior and accessibility, but draw a
// visible indicator even when the platform style's dark checkbox has no outline.
class PaletteCheckBox : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    void paintEvent(QPaintEvent *event) override {
        QCheckBox::paintEvent(event);
        QStyleOptionButton option;
        initStyleOption(&option);
        option.palette.setCurrentColorGroup(!isEnabled() ? QPalette::Disabled : isActiveWindow() ? QPalette::Active : QPalette::Inactive);
        const auto indicator = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
        QPainter painter(this);
        painter.fillRect(indicator, option.palette.brush(QPalette::Base));
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(indicator).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool checked = checkState() != Qt::Unchecked;
        painter.setPen(QPen(option.palette.color(QPalette::Text), 1));
        painter.setBrush(option.palette.brush(checked ? QPalette::Highlight : QPalette::Base));
        painter.drawRoundedRect(box, 2, 2);
        if (checked) {
            painter.setPen(QPen(option.palette.color(QPalette::HighlightedText), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const auto point = [&box](qreal x, qreal y) { return QPointF(box.left() + x * box.width(), box.top() + y * box.height()); };
            if (checkState() == Qt::PartiallyChecked) painter.drawLine(point(.25,.5), point(.75,.5));
            else {
                painter.drawLine(point(.22,.5), point(.43,.72));
                painter.drawLine(point(.43,.72), point(.8,.27));
            }
        }
    }
};

QString workspaceStyleSheet() {
    // QSS palette() resolves active brushes, even in :disabled rules. Read the
    // disabled colors explicitly rather than using a border color as text.
    return QString(R"(
        QWidget#workspace { background: palette(base); color: palette(text); }
        QWidget#workspace QLabel, QWidget#workspace QCheckBox, QWidget#workspace QGroupBox { color: palette(text); }
        QWidget#workspace QLabel:disabled, QWidget#workspace QCheckBox:disabled, QWidget#workspace QGroupBox:disabled { color: %2; }
        QFrame#sidebar { background: palette(window); border-right: 1px solid palette(mid); }
        QFrame#sidebar QLabel { color: palette(window-text); }
        QLabel[role="eyebrow"] { color: palette(text); font-size: 10pt; font-weight: 600; }
        QListWidget#navigation { color: palette(window-text); background: transparent; border: none; outline: none; }
        QListWidget#navigation::item { padding: 12px 14px; margin: 3px 0; border-radius: 6px; }
        QListWidget#navigation::item:selected { background: palette(highlight); color: palette(highlighted-text); font-weight: 600; }
        QListWidget#navigation::item:hover:!selected { background: palette(alternate-base); color: palette(text); }
        QListWidget#navigation::item:selected:!active { background: %7; color: %8; }
        QDialog { background: palette(window); color: palette(window-text); }
        QDialog QLabel { color: palette(window-text); }
        QDialog QLabel:disabled { color: %6; }
        QTabWidget::pane { border: none; }
        QScrollArea { border: none; background: transparent; }
        QScrollArea > QWidget > QWidget { background: palette(base); }
        QGroupBox { background: palette(base); border: 1px solid palette(mid); border-radius: 8px; margin-top: 10px; padding: 14px 12px 10px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 16px; padding: 0 6px; }
        QFrame#statusCard { background: palette(alternate-base); border-radius: 8px; border-left: 4px solid palette(highlight); }
        QFrame#statusCard[state="warning"] { border-left-color: #b87917; }
        QFrame#statusCard[state="error"] { border-left-color: #c94d48; }
        QFrame#statusCard[state="ok"] { border-left-color: #338562; }
        QPushButton { padding: 8px 14px; border-radius: 5px; border: 1px solid palette(mid); background: palette(button); color: palette(button-text); }
        QPushButton:hover { border-color: palette(highlight); }
        QPushButton:pressed { background: palette(alternate-base); color: palette(text); }
        QPushButton:disabled { background: %4; color: %1; border-color: %5; }
        QPushButton[primary="true"] { background: palette(highlight); color: palette(highlighted-text); border: 1px solid palette(highlight); font-weight: 600; }
        QPushButton[primary="true"]:disabled { background: %4; color: %1; border-color: %5; }
        QLineEdit, QComboBox, QSpinBox { min-height: 26px; padding: 3px 6px; background: palette(base); color: palette(text); border: 1px solid palette(mid); border-radius: 5px; }
        QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { background: %3; color: %2; border-color: %5; }
        QComboBox QAbstractItemView { background: palette(base); color: palette(text); selection-background-color: palette(highlight); selection-color: palette(highlighted-text); }
        QToolButton { padding: 8px; background: palette(button); color: palette(button-text); border: 1px solid palette(mid); border-radius: 5px; }
        QToolButton:disabled { background: %4; color: %1; border-color: %5; }
        QToolButton:hover:enabled { border-color: palette(highlight); }
        QHeaderView::section { padding: 10px 8px; border: none; background: palette(window); color: palette(window-text); }
        QHeaderView::section:disabled { color: %6; }
        QTableCornerButton::section { background: palette(window); border: none; }
        QTableView, QTreeView, QPlainTextEdit { background: palette(base); color: palette(text); alternate-background-color: palette(alternate-base); selection-background-color: palette(highlight); selection-color: palette(highlighted-text); border: 1px solid palette(mid); border-radius: 5px; }
        QTableView:disabled, QTreeView:disabled, QPlainTextEdit:disabled { background: %3; color: %2; border-color: %5; }
        QTableView::item:selected:!active, QTreeView::item:selected:!active { background: %7; color: %8; }
        QTableView::item, QTreeView::item { padding: 7px; }
        QWidget#editBar { background: palette(window); border-radius: 6px; }
        QWidget#editBar QLabel { color: palette(window-text); }
    )").arg(qApp->palette().color(QPalette::Disabled, QPalette::ButtonText).name(),
             qApp->palette().color(QPalette::Disabled, QPalette::Text).name(),
             qApp->palette().color(QPalette::Disabled, QPalette::Base).name(),
             qApp->palette().color(QPalette::Disabled, QPalette::Button).name(),
             qApp->palette().color(QPalette::Disabled, QPalette::Mid).name(),
             qApp->palette().color(QPalette::Disabled, QPalette::WindowText).name(),
             qApp->palette().color(QPalette::Inactive, QPalette::Highlight).name(),
             qApp->palette().color(QPalette::Inactive, QPalette::HighlightedText).name());
}

QIcon statusIcon(const QString &state, int frame = 0) {
    const bool backingUp = state == "backup-running", checking = state == "check-running", recovering = state == "recovery-running";
    const bool working = backingUp || checking || recovering;
    const QColor accent = recovering ? QColor("#3dd6b3") : checking ? QColor("#a675ed") : QColor("#38b6ff");
    QPixmap pix(64, 64); pix.fill(Qt::transparent);
    QPainter p(&pix); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(working ? accent.darker(155) : QColor("#4679ad"));
    p.drawRoundedRect(9, 9, 44, 47, 7, 7);
    p.setPen(QPen(Qt::white, 4)); p.drawLine(18, 22, 42, 22); p.drawLine(18, 31, 34, 31);
    if (working) {
        p.setBrush(Qt::NoBrush); p.setPen(QPen(accent, 5, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(4,4,56,56), -frame * 30 * 16, 250 * 16);
    }
    p.setPen(QPen(qApp->palette().color(QPalette::Window), 2));
    p.setBrush(working ? accent : state == "idle" ? QColor("#aab6c2") : state == "ok" ? QColor("#54b992") : state == "error" ? QColor("#ef716c") : QColor("#efb14f"));
    p.drawEllipse(34, 34, 28, 28);
    p.setPen(QPen(working ? Qt::white : QColor("#172731"), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (state == "paused") {p.drawLine(44,42,44,54); p.drawLine(52,42,52,54);}
    else if (backingUp) {p.drawLine(48,55,48,41); p.drawLine(42,47,48,41); p.drawLine(48,41,54,47);}
    else if (recovering) {p.drawLine(48,40,48,54); p.drawLine(42,48,48,54); p.drawLine(48,54,54,48);}
    else if (checking) {p.setBrush(Qt::NoBrush); p.drawEllipse(QRectF(40,40,11,11)); p.drawLine(50,50,56,56);}
    else if (state == "ok") {p.drawLine(40,47,46,53); p.drawLine(46,53,55,42);}
    else if (state == "idle") p.drawLine(41,48,55,48);
    else {p.drawLine(48,40,48,49); p.drawPoint(48,55);}
    return QIcon(pix);
}

QString stamp(const QString &text) { return text.isEmpty() ? "Unavailable" : text; }
QSet<QString> exclusionRules(const QString &text) {
    QSet<QString> rules;
    for (const auto &line : text.split('\n')) if (!line.trimmed().isEmpty() && !line.trimmed().startsWith('#')) rules.insert(line.trimmed());
    return rules;
}
QString exclusionChanges(const QString &saved, const QString &edited) {
    const auto before = exclusionRules(saved), after = exclusionRules(edited);
    const int added = (after-before).size(), removed = (before-after).size();
    return added || removed ? QString("Exclusions: %1 added, %2 removed.").arg(added).arg(removed) : QString();
}
QString startupPath() { return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/io.github.m45drivebackuper.desktop"; }
QLabel *label(const QString &text = {}) {
    auto *l = new QLabel(text); l->setTextFormat(Qt::PlainText); l->setWordWrap(true); l->setTextInteractionFlags(Qt::TextSelectableByMouse); return l;
}
QWidget *page(QTabWidget *tabs, const QString &name, QVBoxLayout *&layout) {
    auto *w = new QWidget; layout = new QVBoxLayout(w); layout->setContentsMargins(0,0,0,0); layout->setSpacing(14); tabs->addTab(w, name); return w;
}
}

Window::Window(bool demo) : m_demo(demo), m_settings("M45 Drive Backuper", "M45 Drive Backuper"), m_tray(this) {
    m_lastNotification = m_settings.value("lastNotification").toString();
    m_trayAnimation = new QTimer(this); m_trayAnimation->setInterval(160);
    connect(m_trayAnimation,&QTimer::timeout,this,[this] {
        m_trayFrame = (m_trayFrame + 1) % 12;
        m_tray.setIcon(statusIcon(m_trayState,m_trayFrame));
    });
    connect(this,&Window::attentionRequested,this,[this](const QString &title,const QString &message,bool critical) {
        m_notificationPage = 0;
        if (!m_demo) m_tray.showMessage("M45 Drive Backuper — " + title,message,critical ? QSystemTrayIcon::Critical : QSystemTrayIcon::Warning,10000);
    });
    connect(this,&Window::reminderRequested,this,[this](const QString &title,const QString &message,int page) {
        m_notificationPage = page;
        if (!m_demo) m_tray.showMessage("M45 Drive Backuper — " + title,message,QSystemTrayIcon::Information,15000);
    });
    connect(&m_tray,&QSystemTrayIcon::messageClicked,this,[this] {selectPage(m_notificationPage); Show();});
    setWindowTitle("M45 Drive Backuper"); resize(1120, 840); setMinimumSize(940, 700); setWindowIcon(statusIcon("ok"));
    setStyleSheet(workspaceStyleSheet());
    qApp->installEventFilter(this);
    m_backupControls = createJobControls("backup"); m_checkControls = createJobControls("check");
    auto *body = new QWidget; body->setObjectName("workspace");
    auto *shell = new QHBoxLayout(body); shell->setContentsMargins(0,0,0,0); shell->setSpacing(0);
    auto *sidebar = new QFrame; sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(204);
    auto *side = new QVBoxLayout(sidebar); side->setContentsMargins(16,28,16,20); side->setSpacing(12);
    auto *brand = label("M45"); auto font = brand->font(); font.setPointSize(25); font.setBold(true); brand->setFont(font); side->addWidget(brand);
    side->addWidget(label("DRIVE BACKUPER")); side->addSpacing(24);
    auto *navigation = new QListWidget; navigation->setObjectName("navigation"); navigation->setAccessibleName("Main navigation");
    navigation->addItems({"Overview", "Backup plan", "Skip files", "Saved backups", "Activity", "Preferences"});
    side->addWidget(navigation,1);
    auto *trayNote = label("Runs quietly in your tray.\nBackups continue when this window is closed."); side->addWidget(trayNote);
    shell->addWidget(sidebar);
    auto *content = new QWidget; auto *outer = new QVBoxLayout(content); outer->setContentsMargins(28,26,28,18); outer->setSpacing(18); shell->addWidget(content,1);
    auto *header = new QHBoxLayout;
    auto *heading = label("Overview"); font.setPointSize(24); heading->setFont(font); header->addWidget(heading,1);
    if (demo) {auto *badge = label("PREVIEW · Sample data"); badge->setMinimumWidth(160); badge->setToolTip("Machine settings and backups are unchanged."); header->addWidget(badge);}
    outer->addLayout(header);
    m_tabs = new QTabWidget; m_tabs->tabBar()->hide(); outer->addWidget(m_tabs, 1); setCentralWidget(body);
    QVBoxLayout *layout;
    page(m_tabs, "Overview", layout);
    auto *overviewScroll = new QScrollArea; overviewScroll->setWidgetResizable(true); overviewScroll->setFrameShape(QFrame::NoFrame);
    auto *overviewBody = new QWidget; auto *overviewLayout = new QVBoxLayout(overviewBody); overviewLayout->setContentsMargins(0,0,8,0); overviewLayout->setSpacing(14);
    overviewScroll->setWidget(overviewBody); layout->addWidget(overviewScroll); layout = overviewLayout;
    auto *statusCard = new QFrame; statusCard->setObjectName("statusCard");
    auto *statusLayout = new QVBoxLayout(statusCard); statusLayout->setContentsMargins(20,20,20,20); statusLayout->setSpacing(12);
    auto *statusCaption = label("BACKUP STATUS"); statusCaption->setProperty("role","eyebrow"); statusLayout->addWidget(statusCaption);
    m_status = label("Reading backup status…"); font = m_status->font(); font.setPointSize(17); font.setBold(true); m_status->setFont(font); statusLayout->addWidget(m_status);
    m_summary = label(); statusLayout->addWidget(m_summary);
    auto *resolve = new QPushButton("Review backup plan →"); resolve->setObjectName("statusAction");
    statusLayout->addWidget(resolve,0,Qt::AlignLeft);
    connect(resolve,&QPushButton::clicked,this,[this] {
        selectPage(findChild<QPushButton *>("statusAction")->property("targetPage").toInt());
    });
    layout->addWidget(statusCard);
    m_updateReporting = new QPushButton("Enable Borg warning and error reporting…");
    m_updateReporting->setToolTip("Update the saved backup job to retain Borg results. Your sources, exclusions, schedule and retention stay the same. Administrator authentication is required.");
    layout->addWidget(m_updateReporting,0,Qt::AlignLeft);
    connect(m_updateReporting,&QPushButton::clicked,this,[this] {action("upgrade-reporting");});
    m_destination = label(); m_schedule = label(); m_attempt = label(); m_space = label();
    auto *timing = new QHBoxLayout; timing->setSpacing(16);
    auto metric = [timing](const QString &title, QLabel *value) {
        auto *card = new QGroupBox(title); auto *box = new QVBoxLayout(card); box->addWidget(value); timing->addWidget(card,1);
    };
    metric("Next scheduled run",m_schedule); metric("Last attempt",m_attempt); layout->addLayout(timing);
    auto *group = new QGroupBox("Your backup plan"); auto *form = new QFormLayout(group); form->setVerticalSpacing(16); form->setHorizontalSpacing(24);
    form->addRow("Destination", m_destination); form->addRow("Available space", m_space);
    m_contents = label(); m_storage = label();
    form->addRow("Contents", m_contents); form->addRow("Storage", m_storage); layout->addWidget(group);
    auto *buttons = new QHBoxLayout;
    m_run = new QPushButton("Back up now"); m_enable = new QPushButton("Enable schedule"); m_disable = new QPushButton("Disable schedule");
    m_mountDrives = new QPushButton("Mount drives…");
    m_run->setProperty("primary",true); m_run->setMinimumHeight(42); auto runFont = m_run->font(); runFont.setBold(true); m_run->setFont(runFont);
    auto *more = new QToolButton; more->setText("More"); more->setPopupMode(QToolButton::InstantPopup); auto *moreMenu = new QMenu(more); more->setMenu(moreMenu);
    auto *mountNow = moreMenu->addAction("Mount drives now…",m_mountDrives,&QPushButton::click);
    auto *enable = moreMenu->addAction("Enable automatic backups…",m_enable,&QPushButton::click);
    auto *disable = moreMenu->addAction("Pause automatic backups…",m_disable,&QPushButton::click);
    connect(moreMenu,&QMenu::aboutToShow,this,[this,mountNow,enable,disable] {mountNow->setEnabled(m_mountDrives->isEnabled()); enable->setEnabled(m_enable->isEnabled()); disable->setEnabled(m_disable->isEnabled());});
    for (auto *button : {m_mountDrives,m_enable,m_disable}) {button->setParent(body); button->hide();}
    buttons->addWidget(m_run); buttons->addWidget(more); buttons->addStretch();
    auto *refresh = new QPushButton("Refresh"); buttons->addWidget(refresh); layout->addLayout(buttons);
    auto *backupControlsRow = new QHBoxLayout;
    m_pauseBackup = new QPushButton("Pause backup"); m_pauseBackup->setObjectName("pauseBackup");
    m_cancelBackup = new QPushButton("Cancel backup…"); m_cancelBackup->setObjectName("cancelBackup");
    auto *backupLater = new QToolButton; backupLater->setText("Backup run controls"); backupLater->setMenu(m_backupControls); backupLater->setPopupMode(QToolButton::InstantPopup);
    backupControlsRow->addWidget(m_pauseBackup); backupControlsRow->addWidget(m_cancelBackup); backupControlsRow->addWidget(backupLater); backupControlsRow->addStretch(); layout->addLayout(backupControlsRow);
    m_backupControlStatus = label(); layout->addWidget(m_backupControlStatus);
    connect(m_pauseBackup,&QPushButton::clicked,this,[this] {jobControl("backup",m_snapshot.value("job_control").toObject().value("backup").toObject().value("paused").toBool() || !m_snapshot.value("job_control").toObject().value("backup").toObject().value("mode").toString().isEmpty() ? "resume" : "pause");});
    connect(m_cancelBackup,&QPushButton::clicked,this,[this] {jobControl("backup","cancel");});
    auto *plan = new QPushButton("Edit backup plan");
    buttons->insertWidget(2,plan);
    connect(plan,&QPushButton::clicked,this,[this] {selectPage(1);});
    layout->addStretch();
    connect(refresh, &QPushButton::clicked, this, &Window::poll);
    connect(m_run, &QPushButton::clicked, this, [this] {action("start");});
    connect(m_mountDrives, &QPushButton::clicked, this, [this] {action("mount");});
    connect(m_enable, &QPushButton::clicked, this, [this] {action("enable");});
    connect(m_disable, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Disable automatic backups?", "Future scheduled backups will stop. A backup already running will finish.") == QMessageBox::Yes) action("disable");
    });
    page(m_tabs, "Saved backups", layout);
    auto *verificationBox = new QGroupBox("Backup integrity");
    auto *verificationLayout = new QVBoxLayout(verificationBox);
    verificationLayout->addWidget(label("Check the most recent completed snapshot, a selected snapshot below, or the whole repository. Snapshot checks read and validate the data needed to recover that snapshot without restoring files. Shared unchanged data still has to be read; checks can take hours."));
    m_verificationStatus = label("No verification result yet."); m_verificationStatus->setObjectName("verificationStatus");
    verificationLayout->addWidget(m_verificationStatus);
    m_verificationMode = new QComboBox; m_verificationMode->setObjectName("verificationMode"); m_verificationMode->setAccessibleName("When to check backup integrity");
    m_verificationMode->addItem("Manual only (default)", "manual");
    m_verificationMode->addItem("After every completed backup", "after_backup");
    m_verificationMode->addItem("Daily at 03:00", "daily");
    m_verificationMode->addItem("Weekly on Sunday at 03:00", "weekly");
    m_verificationMode->addItem("Monthly on the 1st at 03:00", "monthly");
    m_verificationScope = new QComboBox; m_verificationScope->setObjectName("verificationScope"); m_verificationScope->setAccessibleName("Backup check scope");
    m_verificationScope->addItem("Most recent completed backup only (default)", "latest");
    m_verificationScope->addItem("Whole repository and all backups", "all");
    auto *checkForm = new QFormLayout;
    checkForm->addRow("What to check",m_verificationScope); checkForm->addRow("When to check",m_verificationMode);
    verificationLayout->addLayout(checkForm);
    verificationLayout->addWidget(label("Scheduled times use local time with up to 10 minutes of delay. Missed scheduled checks run when the machine is back on. Backups wait while a check runs."));
    auto *checkButtons = new QHBoxLayout;
    m_checkNow = new QPushButton("Check now…"); m_checkNow->setObjectName("checkNow");
    m_saveVerification = new QPushButton("Save check options…"); m_saveVerification->setObjectName("saveVerification");
    m_checkDetails = new QPushButton("Check details"); m_checkDetails->setObjectName("checkDetails");
    m_stopCheck = new QPushButton("Cancel check…"); m_stopCheck->setObjectName("stopCheck");
    connect(m_stopCheck,&QPushButton::clicked,this,[this] {jobControl("check","cancel");});
    m_pauseCheck = new QPushButton("Pause check"); m_pauseCheck->setObjectName("pauseCheck");
    connect(m_pauseCheck,&QPushButton::clicked,this,[this] {jobControl("check",m_snapshot.value("job_control").toObject().value("check").toObject().value("paused").toBool() || !m_snapshot.value("job_control").toObject().value("check").toObject().value("mode").toString().isEmpty() ? "resume" : "pause");});
    checkButtons->addWidget(m_checkNow); checkButtons->addWidget(m_stopCheck); checkButtons->addWidget(m_saveVerification); checkButtons->addWidget(m_checkDetails); checkButtons->addStretch();
    verificationLayout->addLayout(checkButtons);
    auto *checkControlsRow = new QHBoxLayout;
    auto *checkLater = new QToolButton; checkLater->setText("Check run controls"); checkLater->setMenu(m_checkControls); checkLater->setPopupMode(QToolButton::InstantPopup);
    checkControlsRow->addWidget(m_pauseCheck); checkControlsRow->addWidget(checkLater); checkControlsRow->addStretch(); verificationLayout->addLayout(checkControlsRow);
    m_checkControlStatus = label(); verificationLayout->addWidget(m_checkControlStatus);
    layout->addWidget(verificationBox);
    connect(m_verificationMode,&QComboBox::currentIndexChanged,this,[this] {renderVerification(); setBusy(m_busy);});
    connect(m_verificationScope,&QComboBox::currentIndexChanged,this,[this] {renderVerification(); setBusy(m_busy);});
    connect(m_checkNow,&QPushButton::clicked,this,[this] {startVerification(m_verificationScope->currentData().toString());});
    connect(m_saveVerification,&QPushButton::clicked,this,[this] {
        if (QMessageBox::question(this,"Save verification options?","What to check: " + m_verificationScope->currentText() + "\nWhen to check: " + m_verificationMode->currentText() + "\n\nAutomatic checking is opt-in. Backups wait during checks. Administrator authentication is required.",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes) return;
        const QJsonObject data{{"mode",m_verificationMode->currentData().toString()},{"scope",m_verificationScope->currentData().toString()},{"revision",m_verificationRevision}};
        action("verification-configure",QString::fromLatin1(QJsonDocument(data).toJson(QJsonDocument::Compact).toBase64()));
    });
    connect(m_checkDetails,&QPushButton::clicked,this,[this] {
        const auto attempt = m_snapshot.value("verification").toObject().value("attempt").toObject();
        QMessageBox details(this); details.setWindowTitle("Backup verification"); details.setText(attempt.value("message").toString("No verification result yet."));
        details.setDetailedText(attempt.value("details").toString("No Borg output was recorded.")); details.exec();
    });
    layout->addWidget(label("Snapshots stored on your backup drive. Reading this list requests administrator authentication."));
    m_archiveRefresh = new QPushButton("Refresh backup list…");
    m_checkSelected = new QPushButton("Check selected backup…"); m_checkSelected->setObjectName("checkSelectedBackup"); m_checkSelected->setToolTip("Refresh the list and select a completed snapshot to check its stored data.");
    m_browseBackup = new QPushButton("Browse and recover…"); m_browseBackup->setObjectName("browseBackup");
    auto *archiveButtons = new QHBoxLayout; archiveButtons->addWidget(m_archiveRefresh); archiveButtons->addWidget(m_browseBackup); archiveButtons->addWidget(m_checkSelected); archiveButtons->addStretch(); layout->addLayout(archiveButtons);
    connect(m_browseBackup,&QPushButton::clicked,this,[this] {
        const auto *item=m_archives->selectedItems().isEmpty() ? nullptr : m_archives->item(m_archives->currentRow(),1);
        const QJsonObject selection{{"archive_id",item ? item->data(Qt::UserRole).toString() : QString("latest")},{"revision",m_snapshot.value("config").toObject().value("revision").toString()}};
        auto *browser=new RecoveryBrowser(selection,item ? item->text() : QString("Most recent backup"),m_demo,this); browser->setAttribute(Qt::WA_DeleteOnClose); browser->show();
    });
    connect(m_checkSelected,&QPushButton::clicked,this,[this] {
        const auto *item = m_archives->item(m_archives->currentRow(),1);
        if (item) startVerification("archive",item->data(Qt::UserRole).toString());
    });
    m_archives = new QTableWidget(0,3); m_archives->setObjectName("archiveTable");
    m_archives->setHorizontalHeaderLabels({"Created", "Snapshot", "Status"}); m_archives->verticalHeader()->hide();
    m_archives->setEditTriggers(QAbstractItemView::NoEditTriggers); m_archives->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_archives->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_archives,&QTableWidget::itemSelectionChanged,this,[this] {setBusy(m_busy);});
    m_archives->setAlternatingRowColors(true); m_archives->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    m_archives->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch); m_archives->horizontalHeader()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
    layout->addWidget(m_archives,1); m_archiveHint = label("No catalog loaded yet. Connect the backup drive and refresh. The catalog does not verify backup integrity."); layout->addWidget(m_archiveHint);
    connect(m_archiveRefresh, &QPushButton::clicked, this, [this] {action("archives");});
    page(m_tabs, "Activity", layout);
    layout->addWidget(label("Activity from the backup service. Times use your local timezone; statistics appear when Borg reports them."));
    m_borgHeading = label("Borg warnings and errors for this attempt"); layout->addWidget(m_borgHeading);
    m_borgMessages = new QPlainTextEdit; m_borgMessages->setReadOnly(true); m_borgMessages->setObjectName("borgMessages");
    m_borgMessages->setMaximumHeight(180); layout->addWidget(m_borgMessages);
    auto *logTools = new QHBoxLayout; m_logSearch = new QLineEdit; m_logSearch->setPlaceholderText("Search activity…"); m_logSearch->setClearButtonEnabled(true);
    m_logSearch->setObjectName("activityFilter"); m_logSearch->setAccessibleName("Search activity");
    m_logLevel = new QComboBox; m_logLevel->setObjectName("activityLevel"); m_logLevel->setAccessibleName("Activity severity");
    m_logLevel->addItems({"All activity", "Warnings & errors"});
    logTools->addWidget(m_logSearch,1); logTools->addWidget(m_logLevel); layout->addLayout(logTools);
    auto *viewTools = new QHBoxLayout;
    m_logCount = label(); m_logCount->setObjectName("activityCount"); viewTools->addWidget(m_logCount,1);
    auto *rawLog = new PaletteCheckBox("Plain text"); rawLog->setObjectName("rawActivity");
    auto *copyLog = new QPushButton("Copy visible log"); copyLog->setObjectName("copyActivity");
    m_followLog = new PaletteCheckBox("Follow latest"); m_followLog->setChecked(true);
    viewTools->addWidget(rawLog); viewTools->addWidget(m_followLog); viewTools->addWidget(copyLog); layout->addLayout(viewTools);
    m_logAccess = label(); m_logAccess->setObjectName("activityAccess"); layout->addWidget(m_logAccess);
    m_logViews = new QStackedWidget; layout->addWidget(m_logViews,1);
    m_activity = new QTableWidget(0,4); m_activity->setObjectName("activityTable"); m_activity->setAccessibleName("Backup activity events");
    m_activity->setHorizontalHeaderLabels({"Time", "Level", "Source", "Message"});
    m_activity->setEditTriggers(QAbstractItemView::NoEditTriggers); m_activity->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_activity->setAlternatingRowColors(true); m_activity->setShowGrid(false); m_activity->setWordWrap(true);
    m_activity->verticalHeader()->hide(); m_activity->verticalHeader()->setMinimumSectionSize(48);
    m_activity->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    for (int column=0;column<3;++column) m_activity->horizontalHeader()->setSectionResizeMode(column,QHeaderView::Fixed);
    m_activity->setColumnWidth(0,125); m_activity->setColumnWidth(1,108); m_activity->setColumnWidth(2,110);
    m_activity->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch); m_logViews->addWidget(m_activity);
    m_logs = new QPlainTextEdit; m_logs->setReadOnly(true); m_logs->setObjectName("activityLog");
    m_logs->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); m_logs->setLineWrapMode(QPlainTextEdit::NoWrap); m_logViews->addWidget(m_logs);
    connect(rawLog,&QCheckBox::toggled,this,[this](bool raw) {m_logViews->setCurrentIndex(raw ? 1 : 0);});
    connect(m_logSearch,&QLineEdit::textChanged,this,&Window::renderLog);
    connect(m_logLevel,&QComboBox::currentIndexChanged,this,&Window::renderLog);
    connect(m_followLog,&QCheckBox::toggled,this,[this](bool follow) {
        if (follow) {m_logs->verticalScrollBar()->setValue(m_logs->verticalScrollBar()->maximum()); m_activity->scrollToBottom();}
    });
    connect(copyLog,&QPushButton::clicked,this,[this] {QApplication::clipboard()->setText(m_logs->toPlainText());});
    page(m_tabs, "Backup plan", layout);
    m_planPage = m_tabs->widget(3);
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *settingsBody = new QWidget; auto *settingsLayout = new QVBoxLayout(settingsBody); settingsLayout->setContentsMargins(0,0,8,0); settingsLayout->setSpacing(12);
    scroll->setWidget(settingsBody); layout->addWidget(scroll); layout = settingsLayout;
    layout->addWidget(label("Choose your source, destination and schedule. Use Skip files to leave out downloads, caches or anything else you don’t need."));
    auto *sourceGroup = new QGroupBox("1   What to back up");
    auto *settingsForm = new QFormLayout(sourceGroup); settingsForm->setVerticalSpacing(8); settingsForm->setHorizontalSpacing(18);
    layout->addWidget(sourceGroup);
    m_sourceKind = new QComboBox; m_sourceKind->setObjectName("sourceKind");
    m_sourceKind->addItem("Folder", "folder"); m_sourceKind->addItem("Mounted drive", "drive");
    settingsForm->addRow("Back up", m_sourceKind);
    auto *sourceRow = new QWidget; auto *sourceLayout = new QHBoxLayout(sourceRow); sourceLayout->setContentsMargins(0,0,0,0);
    m_source = new QLineEdit; m_source->setObjectName("sourcePath");
    auto *browse = new QPushButton("Browse…"); sourceLayout->addWidget(m_source,1); sourceLayout->addWidget(browse);
    settingsForm->addRow("Folder", sourceRow);
    connect(browse,&QPushButton::clicked,this,[this] {
        auto options = QFileDialog::Options(QFileDialog::ShowDirsOnly);
        if (m_demo) options |= QFileDialog::ReadOnly;
        const auto path = QFileDialog::getExistingDirectory(this,"Choose a folder to back up",m_source->text(),options);
        if (!path.isEmpty()) m_source->setText(path);
    });
    m_sourceDrive = new QComboBox; m_sourceDrive->setObjectName("sourceDrive");
    settingsForm->addRow("Source drive", m_sourceDrive);
    connect(m_sourceDrive,&QComboBox::currentIndexChanged,this,[this](int index) {if (index >= 0 && m_sourceKind->currentData() == "drive") m_source->setText(m_sourceDrive->itemData(index).toString());});
    connect(m_sourceKind,&QComboBox::currentIndexChanged,this,[this,sourceRow,settingsForm](int) {
        const bool drive = m_sourceKind->currentData() == "drive";
        settingsForm->setRowVisible(sourceRow,!drive); settingsForm->setRowVisible(m_sourceDrive,drive);
        if (drive && m_sourceDrive->currentIndex() >= 0) m_source->setText(m_sourceDrive->currentData().toString());
        if (!drive && !m_loading) m_source->setText(m_folderSource);
    });
    connect(m_source,&QLineEdit::textChanged,this,[this](const QString &source) {if (!m_loading && m_sourceKind->currentData() == "folder") m_folderSource = source;});
    settingsForm->setRowVisible(m_sourceDrive,false);
    m_includeSystem = new PaletteCheckBox("Also include system settings and local software"); m_includeSystem->setObjectName("includeSystem");
    m_includeSystem->setToolTip("Includes /etc, /usr/local, /root, scheduled jobs, package records, network and account settings. Other drives and service databases are not included.");
    settingsForm->addRow("",m_includeSystem);
    m_packageRecovery = new PaletteCheckBox("Include package recovery file in every backup"); m_packageRecovery->setObjectName("packageRecovery");
    m_packageRecovery->setToolTip("Automatically refresh package names, versions and reinstall information, then store the file in the same backup snapshot. Applies to scheduled and manual backups. Save changes to enable or disable it.");
    settingsForm->addRow("",m_packageRecovery);
    auto *destinationGroup = new QGroupBox("2   Where to keep it");
    auto *destinationForm = new QFormLayout(destinationGroup); destinationForm->setVerticalSpacing(8); destinationForm->setHorizontalSpacing(18);
    layout->addWidget(destinationGroup);
    m_destinationKind = new QComboBox; m_destinationKind->setObjectName("destinationKind");
    m_destinationKind->addItem("A folder","folder"); m_destinationKind->addItem("This drive","drive"); destinationForm->addRow("Save backups to",m_destinationKind);
    auto *destinationRow = new QWidget; auto *destinationLayout = new QHBoxLayout(destinationRow); destinationLayout->setContentsMargins(0,0,0,0);
    m_repository = new QLineEdit; m_repository->setObjectName("repositoryPath"); m_repository->setPlaceholderText("Choose an empty folder for your backups");
    auto *destinationBrowse = new QPushButton("Choose / create folder…"); destinationLayout->addWidget(m_repository,1); destinationLayout->addWidget(destinationBrowse);
    destinationForm->addRow("Backup folder",destinationRow);
    connect(destinationBrowse,&QPushButton::clicked,this,[this] {
        QFileDialog::Options options = QFileDialog::ShowDirsOnly;
        if (m_demo) options |= QFileDialog::ReadOnly;
        const auto path = QFileDialog::getExistingDirectory(this,"Choose or create an empty backup folder",m_repository->text(),options);
        if (!path.isEmpty()) m_repository->setText(path);
    });
    m_destinationDrive = new QComboBox; m_destinationDrive->setObjectName("destinationDrive"); destinationForm->addRow("Backup drive",m_destinationDrive); destinationForm->setRowVisible(m_destinationDrive,false);
    auto *destinationHint = label(); destinationHint->setObjectName("destinationHint");
    destinationHint->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum);
    destinationForm->addRow("",destinationHint);
    auto updateDestinationHint = [this,destinationHint] {
        destinationHint->setText(m_destinationKind->currentData() == "drive"
            ? "Backup folder: " + m_repository->text() + "\nOther files on this drive are kept."
            : "Choose an empty folder, or use New Folder in the chooser. You can also type a new folder path.");
    };
    connect(m_repository,&QLineEdit::textChanged,this,updateDestinationHint);
    connect(m_destinationKind,&QComboBox::currentIndexChanged,this,updateDestinationHint);
    connect(m_repository,&QLineEdit::textChanged,this,[this](const QString &path) {if (!m_loading && m_destinationKind->currentData() == "folder") m_folderRepository = path;});
    m_mount = new QLineEdit(this); m_mount->setObjectName("destinationPath"); m_mount->hide();
    connect(m_destinationDrive,&QComboBox::currentIndexChanged,this,[this](int index) {
        if (!m_loading && index >= 0 && m_destinationKind->currentData() == "drive") {m_repository->setText(driveRepository()); m_autoMount->setChecked(true);}
    });
    connect(m_destinationKind,&QComboBox::currentIndexChanged,this,[this,destinationForm,destinationRow](int) {
        const bool drive = m_destinationKind->currentData() == "drive";
        destinationForm->setRowVisible(destinationRow,!drive); destinationForm->setRowVisible(m_destinationDrive,drive);
        if (!m_loading && drive && m_destinationDrive->currentIndex() >= 0) {m_repository->setText(driveRepository()); m_autoMount->setChecked(true);}
        if (!m_loading && !drive) m_repository->setText(m_folderRepository);
    });
    m_interval = new QComboBox;
    for (int h : {1,2,3,4,6,8,12,24}) m_interval->addItem(h == 24 ? "Every day" : h == 1 ? "Every hour" : QString("Every %1 hours").arg(h), h);
    auto *scheduleGroup = new QGroupBox("3   When to back up"); auto *scheduleForm = new QFormLayout(scheduleGroup);
    scheduleForm->addRow("Run automatically", m_interval); layout->addWidget(scheduleGroup);
    auto *simpleActions = new QHBoxLayout;
    auto *chooseSkips = new QPushButton("Choose what to skip…"); chooseSkips->setMinimumHeight(36);
    auto *defaults = new QPushButton("Use recommended settings"); defaults->setObjectName("recommendedDefaults");
    simpleActions->addWidget(chooseSkips); simpleActions->addWidget(defaults); simpleActions->addStretch(); layout->addLayout(simpleActions);
    connect(chooseSkips,&QPushButton::clicked,this,[this] {selectPage(2);});
    defaults->setToolTip("Use fast, space-saving backups and mount configured drives when needed. Review and save to apply.");
    auto *advancedToggle = new QToolButton; advancedToggle->setObjectName("advancedToggle"); advancedToggle->setText("Advanced options"); advancedToggle->setCheckable(true); advancedToggle->setArrowType(Qt::RightArrow); advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    layout->addWidget(advancedToggle,0,Qt::AlignLeft);
    auto *advanced = new QWidget; advanced->setObjectName("advancedOptions"); auto *advancedLayout = new QVBoxLayout(advanced); advancedLayout->setContentsMargins(0,0,0,0);
    auto *advancedForm = new QFormLayout;
    m_backupType = new QComboBox; m_backupType->setObjectName("backupType");
    m_backupType->addItem("Incremental snapshot (recommended)","incremental"); m_backupType->addItem("Full file scan","full_scan");
    advancedForm->addRow("Backup type",m_backupType);
    m_sourceKind->setToolTip("A drive backup saves its files, not a bootable disk image. Nested filesystems are skipped.");
    m_backupType->setToolTip("Incremental scans use cached file metadata to skip unchanged files. Full scans reread all files. Both share duplicate data and each archive represents a complete snapshot.");
    m_compression = new QComboBox; m_compression->setObjectName("compression");
    m_compression->setToolTip("Applies to newly stored data. Existing backup data keeps its previous compression.");
    for (const auto &option : {qMakePair(QString("Balanced (recommended)"),QString("zstd,3")),qMakePair(QString("Fast"),QString("lz4")),qMakePair(QString("Smaller backups"),QString("zstd,6")),qMakePair(QString("Smallest backups (slower)"),QString("zstd,9")),qMakePair(QString("Off"),QString("none"))}) m_compression->addItem(option.first,option.second);
    advancedForm->addRow("Compression",m_compression);
    m_reserve = new QSpinBox; m_reserve->setRange(1,1048576); m_reserve->setSuffix(" GiB"); m_reserve->setObjectName("reserveGiB");
    m_reserve->setToolTip("A backup will not start writing its archive unless more than this space is free after cleanup. This is a preflight check, not a guaranteed reserve throughout the run.");
    advancedForm->addRow("Leave free on backup drive",m_reserve);
    auto *quotaRow = new QWidget; auto *quotaLayout = new QHBoxLayout(quotaRow); quotaLayout->setContentsMargins(0,0,0,0);
    m_quotaMode = new QComboBox; m_quotaMode->setObjectName("quotaMode");
    m_quotaMode->addItem("Keep existing limit",-1); m_quotaMode->addItem("No limit",0); m_quotaMode->addItem("Limit backup storage to",1);
    m_quota = new QSpinBox; m_quota->setRange(1,1048576); m_quota->setValue(100); m_quota->setSuffix(" GiB"); m_quota->setObjectName("quotaGiB");
    quotaLayout->addWidget(m_quotaMode,1); quotaLayout->addWidget(m_quota); m_quota->hide();
    quotaRow->setToolTip("Limits Borg repository data; metadata and temporary space can add overhead. Applied on the next backup.");
    connect(m_quotaMode,&QComboBox::currentIndexChanged,this,[this] {m_quota->setVisible(m_quotaMode->currentData().toInt() == 1);});
    advancedForm->addRow("Backup storage limit",quotaRow);
    m_autoMount = new PaletteCheckBox("Mount configured drives when needed"); m_autoMount->setObjectName("autoMount");
    m_autoMount->setToolTip("Before a backup, try existing /etc/fstab entries for the source and destination. UUIDs are checked before and after mounting. Drives must be connected and unlocked; new mount definitions are not created.");
    advancedForm->addRow("",m_autoMount);

    auto spin = [] {auto *s = new QSpinBox; s->setRange(0,365); return s;};
    m_hourly = spin(); m_daily = spin(); m_weekly = spin(); m_monthly = spin();
    auto *retention = new QWidget; auto *retentionLayout = new QHBoxLayout(retention); retentionLayout->setContentsMargins(0,0,0,0);
    for (auto pair : {qMakePair(QString("Hourly"),m_hourly),qMakePair(QString("Daily"),m_daily),qMakePair(QString("Weekly"),m_weekly),qMakePair(QString("Monthly"),m_monthly)}) {
        retentionLayout->addWidget(label(pair.first)); retentionLayout->addWidget(pair.second);
    }
    advancedForm->addRow("Backups to keep", retention); advancedLayout->addLayout(advancedForm);
    connect(defaults,&QPushButton::clicked,this,[this] {
        m_backupType->setCurrentIndex(m_backupType->findData("incremental")); m_compression->setCurrentIndex(m_compression->findData("zstd,3"));
        m_reserve->setValue(10); m_quotaMode->setCurrentIndex(m_quotaMode->findData(-1)); m_autoMount->setChecked(true);
        m_hourly->setValue(4); m_daily->setValue(7); m_weekly->setValue(4); m_monthly->setValue(6); m_skipCaches->setChecked(true);
        m_includeSystem->setChecked(true); m_packageRecovery->setChecked(true);
        updateEditState();
    });
    advancedLayout->addWidget(label("Older backups beyond these limits are removed during cleanup. Keep at least one value above zero."));

    layout->addWidget(advanced); advanced->hide();
    connect(advancedToggle,&QToolButton::toggled,this,[advanced,advancedToggle](bool expanded) {advanced->setVisible(expanded); advancedToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);});
    layout->addStretch();
    page(m_tabs, "Skip files", layout); m_exclusionsPage = m_tabs->widget(4);
    auto *skipScroll = new QScrollArea; skipScroll->setWidgetResizable(true); skipScroll->setFrameShape(QFrame::NoFrame);
    auto *skipBody = new QWidget; auto *skipLayout = new QVBoxLayout(skipBody); skipLayout->setContentsMargins(0,0,8,0); skipLayout->setSpacing(12);
    skipScroll->setWidget(skipBody); layout->addWidget(skipScroll); layout = skipLayout;
    m_excludes = new ExclusionEditor(m_demo); layout->addWidget(m_excludes,1);
    m_skipCaches = new PaletteCheckBox("Also skip marked cache folders (extra rule)"); m_skipCaches->setToolTip("This adds Borg’s CACHEDIR.TAG cache rule to the exclusion list above. It is separate from the displayed rule count."); layout->addWidget(m_skipCaches);
    auto *analyzer = new QWidget; auto *analyzerLayout = new QHBoxLayout(analyzer); analyzerLayout->setContentsMargins(0,0,0,0);
    auto *analyzerText = label("Find large folders with K4DirStat, then choose what to skip.");
    auto *openAnalyzer = new QPushButton("Open K4DirStat…"); openAnalyzer->setObjectName("openAnalyzer");
    analyzerLayout->addWidget(analyzerText,1); analyzerLayout->addWidget(openAnalyzer); layout->addWidget(analyzer);
    connect(openAnalyzer,&QPushButton::clicked,this,&Window::openAnalyzer);
    connect(m_source,&QLineEdit::textChanged,m_excludes,&ExclusionEditor::setSource);
    connect(m_includeSystem,&QCheckBox::toggled,this,[this](bool enabled) {
        m_excludes->setAdditionalSources(enabled ? QStringList{"/etc","/usr/local","/root","/var/spool/cron","/var/lib/dpkg","/var/lib/apt/extended_states","/var/lib/NetworkManager","/var/lib/bluetooth","/var/lib/AccountsService"} : QStringList{});
        updateEditState();
    });
    page(m_tabs, "Preferences", layout);
    auto *preferencesScroll = new QScrollArea; preferencesScroll->setWidgetResizable(true); preferencesScroll->setFrameShape(QFrame::NoFrame);
    auto *preferencesBody = new QWidget; auto *preferencesLayout = new QVBoxLayout(preferencesBody); preferencesLayout->setContentsMargins(0,0,8,0); preferencesLayout->setSpacing(14);
    preferencesScroll->setWidget(preferencesBody); layout->addWidget(preferencesScroll); layout = preferencesLayout;
    layout->addWidget(label("Desktop monitoring"));
    m_startup = new PaletteCheckBox("Start tray monitor at login"); m_notifications = new PaletteCheckBox("Notify when backup status needs attention");
    bool startup = true; QFile startupFile(startupPath()); if (startupFile.open(QIODevice::ReadOnly)) startup = !startupFile.readAll().contains("Hidden=true");
    m_startup->setChecked(startup); m_notifications->setChecked(m_settings.value("notifications",true).toBool());
    layout->addWidget(m_startup); layout->addWidget(m_notifications);
    auto *loginWindow = new PaletteCheckBox("Open window at login");
    loginWindow->setObjectName("showWindowAtLogin");
    loginWindow->setChecked(m_settings.value("showWindowAtLogin", false).toBool());
    loginWindow->setEnabled(!m_demo && startup);
    layout->addWidget(loginWindow);
    connect(m_startup, &QCheckBox::toggled, loginWindow, [this, loginWindow](bool on) { loginWindow->setEnabled(!m_demo && on); });
    connect(loginWindow, &QCheckBox::toggled, this, [this](bool on) { if (!m_demo) m_settings.setValue("showWindowAtLogin", on); });
    connect(m_startup,&QCheckBox::toggled,this,&Window::setStartup);
    connect(m_notifications,&QCheckBox::toggled,this,[this](bool on) {if (!m_demo) m_settings.setValue("notifications",on);});
    auto *idleBox = new QGroupBox("Wait until idle before automatic runs");
    auto *idleLayout = new QVBoxLayout(idleBox);
    m_idleBackup = new PaletteCheckBox("Wait until idle for automatic backups"); m_idleBackup->setObjectName("idleBackup");
    m_idleCheck = new PaletteCheckBox("Wait until idle for automatic integrity checks"); m_idleCheck->setObjectName("idleCheck");
    idleLayout->addWidget(m_idleBackup); idleLayout->addWidget(m_idleCheck);
    m_idleMax = new QComboBox; m_idleMax->setObjectName("idleMaxHours");
    for (int hours : {6,12,24,48}) m_idleMax->addItem(QString("%1 hours%2").arg(hours).arg(hours == 6 ? " (default)" : ""),hours);
    auto *idleForm = new QFormLayout; idleForm->addRow("Maximum delay",m_idleMax); idleLayout->addLayout(idleForm);
    idleLayout->addWidget(label("Optional and off by default. Automatic jobs wait until user sessions report idle for 10 minutes, or until the maximum delay expires. They then start even if you are still working. If idle reporting is unavailable, this option is disabled and normal schedules apply. Manual runs and explicit postponements start at your chosen time. This does not pause a run when you return."));
    m_idleStatus = label(); m_idleStatus->setObjectName("idlePolicyStatus"); idleLayout->addWidget(m_idleStatus);
    m_idleSave = new QPushButton("Save idle-wait options…"); m_idleSave->setObjectName("saveIdlePolicy"); idleLayout->addWidget(m_idleSave,0,Qt::AlignLeft);
    layout->addWidget(idleBox);
    for (auto *check : {m_idleBackup,m_idleCheck}) connect(check,&QCheckBox::toggled,this,[this] {renderIdlePolicy();});
    connect(m_idleMax,&QComboBox::currentIndexChanged,this,[this] {renderIdlePolicy();});
    connect(m_idleSave,&QPushButton::clicked,this,[this] {
        if (m_demo || m_busy) return;
        const auto options=idleConfiguration();
        const QString summary=QString("Automatic backups: %1\nAutomatic checks: %2\nMaximum idle wait: %3 hours\n\nJobs start after the limit even if the system is still busy. Turning this option off releases any jobs already waiting for idle. Saved schedules stay unchanged. Administrator authentication is required.").arg(m_idleBackup->isChecked() ? "wait for idle" : "no idle wait",m_idleCheck->isChecked() ? "wait for idle" : "no idle wait").arg(m_idleMax->currentData().toInt());
        if (QMessageBox::question(this,"Save idle-wait options?",summary,QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes) return;
        auto data=options; data["revision"]=m_idleRevision;
        action("idle-configure",QString::fromLatin1(QJsonDocument(data).toJson(QJsonDocument::Compact).toBase64()));
    });
    auto *remindersBox = new QGroupBox("Reminders for manual runs");
    auto *remindersLayout = new QVBoxLayout(remindersBox);
    remindersLayout->addWidget(label("Get a desktop reminder without starting a backup or check. Reminders pause while automatic runs are enabled. Set backups to manual by disabling the schedule on Overview; set checks to Manual only in Saved backups."));
    auto *remindersForm = new QFormLayout;
    m_backupReminder = new QComboBox; m_backupReminder->setObjectName("backupReminder");
    m_checkReminder = new QComboBox; m_checkReminder->setObjectName("checkReminder");
    for (auto *combo : {m_backupReminder,m_checkReminder}) {
        combo->addItem("Off (default)",0); combo->addItem("Every day",1);
        combo->addItem("Every week",7); combo->addItem("Every 30 days",30);
    }
    remindersForm->addRow("Remind me to back up",m_backupReminder);
    remindersForm->addRow("Remind me to check integrity",m_checkReminder);
    remindersLayout->addLayout(remindersForm);
    m_reminderStatus = label(); m_reminderStatus->setObjectName("reminderStatus");
    remindersLayout->addWidget(m_reminderStatus);
    remindersLayout->addWidget(label("Saved immediately for this desktop user. The tray monitor must be running and desktop notifications allowed. Missed reminders appear when you return; overdue reminders repeat at most once a day. A successful run resets the interval; failed or incomplete runs do not."));
    layout->addWidget(remindersBox);
    for (const auto &entry : {qMakePair(m_backupReminder,QString("backup")),qMakePair(m_checkReminder,QString("check"))}) {
        auto *combo = entry.first;
        combo->setCurrentIndex(qMax(0,combo->findData(m_demo ? 0 : m_settings.value("reminders/" + entry.second + "Days",0).toInt())));
        connect(combo,&QComboBox::currentIndexChanged,this,[this,combo,kind=entry.second] {
            if (!m_demo) {
                m_settings.setValue("reminders/" + kind + "Days",combo->currentData());
                m_settings.remove(reminderKey(kind));
            }
            updateReminders(QDateTime::currentDateTimeUtc());
        });
    }
    auto *recoveryBox = new QGroupBox("Software recovery"); auto *recoveryLayout = new QVBoxLayout(recoveryBox);
    m_recoveryStatus = label("Reading saved recovery settings…"); m_recoveryStatus->setObjectName("recoveryStatus");
    auto recoveryFont = m_recoveryStatus->font(); recoveryFont.setBold(true); m_recoveryStatus->setFont(recoveryFont); recoveryLayout->addWidget(m_recoveryStatus);
    m_recoveryToggle = new PaletteCheckBox("Include package recovery file in every backup"); m_recoveryToggle->setObjectName("recoveryToggle"); recoveryLayout->addWidget(m_recoveryToggle);
    recoveryLayout->addWidget(label("When enabled, every scheduled or manual backup refreshes the package list and saves it in that snapshot. Uncheck this option and Save changes to turn off automatic recovery creation. Existing backups and exported copies are kept."));
    connect(m_packageRecovery,&QCheckBox::toggled,this,[this](bool checked) {QSignalBlocker blocker(m_recoveryToggle); m_recoveryToggle->setChecked(checked);});
    connect(m_recoveryToggle,&QCheckBox::toggled,m_packageRecovery,&QCheckBox::setChecked);
    recoveryLayout->addWidget(label("Export saves an extra portable copy now. Reinstall previews APT changes on a matching OS release. Snap, Flatpak and service lists are included for reference."));
    auto *recoveryButtons = new QHBoxLayout;
    m_exportRecovery = new QPushButton("Export an extra recovery copy…"); m_exportRecovery->setObjectName("exportRecovery");
    m_restorePackages = new QPushButton("Reinstall from recovery file…"); m_restorePackages->setObjectName("restorePackages");
    recoveryButtons->addWidget(m_exportRecovery); recoveryButtons->addWidget(m_restorePackages); recoveryButtons->addStretch(); recoveryLayout->addLayout(recoveryButtons); layout->addWidget(recoveryBox);
    connect(m_exportRecovery,&QPushButton::clicked,this,[this] {action("recovery-export");});
    connect(m_restorePackages,&QPushButton::clicked,this,[this] {
        const auto path = QFileDialog::getOpenFileName(this,"Choose a software recovery file",QDir::homePath(),"M45 software recovery (*.json)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4*1024*1024) {QMessageBox::warning(this,"Software recovery","Could not read this file, or it is larger than 4 MiB."); return;}
        const auto data = file.readAll(); const auto doc = QJsonDocument::fromJson(data);
        if (!doc.isObject() || doc.object().value("schema") != "m45-software-recovery-v1") {QMessageBox::warning(this,"Software recovery","Choose a valid M45 software recovery JSON file."); return;}
        action("packages-preview",QString::fromUtf8(data));
    });
    layout->addStretch();
    m_tabs->tabBar()->moveTab(3,1); m_tabs->tabBar()->moveTab(4,2);
    connect(navigation,&QListWidget::currentRowChanged,m_tabs,&QTabWidget::setCurrentIndex);
    connect(m_tabs,&QTabWidget::currentChanged,this,[navigation,heading,this](int index) {navigation->setCurrentRow(index); heading->setText(m_tabs->tabText(index));});
    navigation->setCurrentRow(0);
    m_editBar = new QWidget; m_editBar->setObjectName("editBar"); auto *editLayout = new QHBoxLayout(m_editBar); editLayout->setContentsMargins(12,10,12,10);
    m_changes = label(); m_discard = new QPushButton("Discard changes"); m_save = new QPushButton(m_demo ? "Review changes…" : "Save changes…"); m_save->setObjectName("saveSettings"); m_save->setProperty("primary",true);
    m_save->setEnabled(false); m_discard->setEnabled(false);
    editLayout->addWidget(m_changes,1); editLayout->addWidget(m_discard); editLayout->addWidget(m_save); outer->addWidget(m_editBar);
    connect(m_save,&QPushButton::clicked,this,&Window::saveSettings);
    connect(m_discard,&QPushButton::clicked,this,[this] {
        if (QMessageBox::question(this,"Discard edits?","Restore the last loaded backup plan and exclusions?") == QMessageBox::Yes) {
            const auto latest = m_snapshot; m_snapshot["config"] = m_savedPlan; loadSettings(); m_snapshot = latest;
        }
    });
    connect(m_tabs,&QTabWidget::currentChanged,this,&Window::updateEditState);
    for (auto *box : m_planPage->findChildren<QComboBox *>()) connect(box,&QComboBox::currentIndexChanged,this,&Window::updateEditState);
    for (auto *spin : m_planPage->findChildren<QSpinBox *>()) connect(spin,&QSpinBox::valueChanged,this,&Window::updateEditState);
    for (auto *edit : m_planPage->findChildren<QLineEdit *>()) connect(edit,&QLineEdit::textChanged,this,&Window::updateEditState);
    connect(m_excludes,&ExclusionEditor::changed,this,&Window::updateEditState); connect(m_skipCaches,&QCheckBox::toggled,this,&Window::updateEditState);
    connect(m_autoMount,&QCheckBox::toggled,this,&Window::updateEditState);
    connect(m_packageRecovery,&QCheckBox::toggled,this,&Window::updateEditState);
    m_feedback = label(); outer->addWidget(m_feedback);
    m_menu = new QMenu(this); m_miniState = m_menu->addAction("Reading backups…"); m_miniState->setEnabled(false);
    m_menu->addSeparator();
    m_menu->addAction("Open Backuper",this,&Window::Show);
    m_runAction = m_menu->addAction("Back up now…",this,[this] {action("start");});
    for (const auto &kind : {QString("backup"),QString("check")}) {
        auto *pause=m_menu->addAction(kind == "backup" ? "Pause backup" : "Pause integrity check",this,[this,kind] {jobControl(kind,m_snapshot.value("job_control").toObject().value(kind).toObject().value("paused").toBool() || !m_snapshot.value("job_control").toObject().value(kind).toObject().value("mode").toString().isEmpty() ? "resume" : "pause");});
        pause->setObjectName(kind + "TrayPause");
        auto *cancel=m_menu->addAction(kind == "backup" ? "Cancel backup…" : "Cancel integrity check…",this,[this,kind] {jobControl(kind,"cancel");});
        cancel->setObjectName(kind + "TrayCancel");
    }
    m_menu->addMenu(m_backupControls); m_menu->addMenu(m_checkControls);
    m_menu->addSeparator(); m_menu->addAction("Quit monitor",this,&Window::quitMonitor);
    m_tray.setIcon(statusIcon("warning")); m_tray.setContextMenu(m_menu);
    connect(&m_tray,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason why) {
        if (why == QSystemTrayIcon::Trigger) m_menu->popup(m_tray.geometry().isValid() ? m_tray.geometry().center() : QCursor::pos());
        if (why == QSystemTrayIcon::DoubleClick) Show();
    });
    if (!m_demo) m_tray.show();
    setBusy(false);
    if (m_demo) {
        m_snapshot = {{"collected_at",QDateTime::currentSecsSinceEpoch()}, {"mounted",false},
            {"actual_mounts",QJsonArray{"/media/dist/Backup-SSD"}},
            {"config",QJsonObject{{"mountpoint","/media/dist/43b29217-193a-4f61-99ff-01a137b0c8f2"},{"uuid","43b29217-193a-4f61-99ff-01a137b0c8f2"},{"hours",6},{"hourly",4},{"daily",7},{"weekly",4},{"monthly",6},{"include_system",true},{"package_recovery",true},{"excludes","# Caches and regenerable downloads\npp:home/dist/Downloads\npp:home/dist/.cache\nsh:home/dist/**/node_modules"}}},
            {"service",QJsonObject{{"ActiveState","failed"},{"Result","exit-code"},{"ExecMainExitTimestamp","Sun 2026-11-08 06:03:58 MST"}}},
            {"timer",QJsonObject{{"ActiveState","active"},{"NextElapseUSecRealtime","Sun 2026-11-08 12:03:44 MST"}}},
            {"logs","2026-11-08T06:03:44-07:00 INFO · Scheduled backup started.\n2026-11-08T06:03:44-07:00 INFO · Checking source: /home/dist\n2026-11-08T06:03:45-07:00 INFO · Checking backup destination…\n2026-11-08T06:03:58-07:00 ERROR · Backup failed: expected backup filesystem is not mounted.\n2026-11-08T06:03:58-07:00 ERROR · local-system-backup.service: Failed with result 'exit-code'."}};
        loadSettings(); render(); setBusy(false); m_feedback->setText("Preview mode · Explore and review edits without changing your backups.");
        showArchives(QJsonArray{
            QJsonObject{{"name","home-2026-11-07T18:00:00"},{"time","2026-11-07T18:00:00"}},
            QJsonObject{{"name","home-2026-11-08T00:00:00"},{"time","2026-11-08T00:00:00"}},
            QJsonObject{{"name","home-2026-11-08T06:00:00.checkpoint"},{"time","2026-11-08T06:00:00"}}});
        m_archiveHint->setText("Sample catalog · Checkpoints may be incomplete. Entries do not verify backup integrity.");
    } else {
        auto *timer = new QTimer(this); connect(timer,&QTimer::timeout,this,&Window::poll); timer->start(15000); poll();
    }
}

bool Window::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Polish) {
        if (auto *widget = qobject_cast<QWidget *>(watched))
            QTimer::singleShot(0,widget,[widget] {applyPlaceholderPalette(widget);});
    }
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        // QSS keeps resolved palette brushes. Re-polish from the new desktop
        // palette so children do not retain light colors after a theme change.
        setStyleSheet({});
        setPalette(qApp->palette());
        setStyleSheet(workspaceStyleSheet());
        for (auto *widget : findChildren<QWidget *>()) applyPlaceholderPalette(widget);
    }
    return QMainWindow::eventFilter(watched, event);
}

void Window::poll() {
    if (m_demo || m_polling || m_busy) return;
    m_polling = true;
    auto *process = new QProcess(this); auto *timeout = new QTimer(process); timeout->setSingleShot(true);
    connect(timeout,&QTimer::timeout,process,&QProcess::kill); timeout->start(45000);
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_polling = false; m_status->setText("Monitoring unavailable");
            m_summary->setText(process->errorString()); m_trayAnimation->stop(); m_trayState = "warning"; m_tray.setIcon(statusIcon("warning"));
            m_tray.setToolTip("M45 Drive Backuper — monitoring unavailable"); m_miniState->setText("Monitoring unavailable");
            alert("monitoring","Backup monitoring unavailable",process->errorString());
            m_snapshot = {}; setBusy(false);
            process->deleteLater();
        }
    });
    connect(process,&QProcess::finished,this,[this,process](int code,QProcess::ExitStatus) {
        m_polling = false;
        if (m_busy) {process->deleteLater(); return;}
        const auto doc = QJsonDocument::fromJson(process->readAllStandardOutput());
        if (code || !doc.isObject() || !doc.object().contains("config")) {
            m_status->setText("Monitoring unavailable");
            m_summary->setText(doc.object().value("error").toString("Could not read backup status. Check that M45 Drive Backuper is installed and the existing backup service is available."));
            m_trayAnimation->stop(); m_trayState = "warning"; m_tray.setIcon(statusIcon("warning")); m_tray.setToolTip("M45 Drive Backuper — monitoring unavailable");
            m_miniState->setText("Monitoring unavailable");
            alert("monitoring","Backup monitoring unavailable",m_summary->text());
            m_snapshot = {}; setBusy(false);
        } else {m_snapshot = doc.object(); if (!m_loaded) loadSettings(); else {
                refreshDrives();
                // A verification hook can change the revision without changing
                // the plan. Preserve pending edits while refreshing that token.
                const auto latest = m_snapshot.value("config").toObject();
                auto before = m_savedPlan, after = latest;
                before.remove("revision"); after.remove("revision");
                for (auto it = after.begin(); it != after.end();) {
                    if (!before.contains(it.key())) it = after.erase(it); else ++it;
                }
                if (before == after) {m_revision = latest.value("revision").toString(); m_savedPlan["revision"] = m_revision;}
            }
            render();}
        process->deleteLater();
    });
    process->start("/usr/bin/python3",{"/usr/libexec/m45-drive-backuper/backend","status"});
}

void Window::loadSettings() {
    m_loading = true;
    const auto c = m_snapshot.value("config").toObject();
    m_revision = c.value("revision").toString();
    m_folderSource = c.value("source_kind").toString("folder") == "folder" ? c.value("source").toString("/home/dist") : QDir::homePath();
    refreshDrives();
    m_destinationDrive->setCurrentIndex(m_destinationDrive->findData(c.value("mountpoint").toString()));
    if (m_sourceDrive->findData(c.value("source").toString()) >= 0) m_sourceDrive->setCurrentIndex(m_sourceDrive->findData(c.value("source").toString()));
    m_sourceKind->setCurrentIndex(m_sourceKind->findData(c.value("source_kind").toString("folder")));
    m_source->setText(c.value("source").toString("/home/dist"));
    m_destinationKind->setCurrentIndex(m_destinationKind->findData(c.value("destination_kind").toString("folder")));
    m_backupType->setCurrentIndex(m_backupType->findData(c.value("scan_mode").toString("incremental")));
    m_compression->setCurrentIndex(m_compression->findData(c.value("compression").toString("zstd,3")));
    m_reserve->setValue(c.value("reserve_gib").toInt(10));
    const int quota = c.value("quota_gib").toInt(-1);
    m_quotaMode->setCurrentIndex(m_quotaMode->findData(quota > 0 ? 1 : quota)); if (quota > 0) m_quota->setValue(quota);
    m_skipCaches->setChecked(c.value("exclude_caches").toBool(true));
    m_autoMount->setChecked(c.value("auto_mount").toBool(false));
    m_includeSystem->setChecked(c.value("include_system").toBool(false));
    m_packageRecovery->setChecked(c.value("package_recovery").toBool(false));
    m_mount->setText(c.value("mountpoint").toString());
    m_repository->setText(c.value("repository").toString(QDir(c.value("mountpoint").toString()).filePath("borg-home-example")));
    m_folderRepository = m_repository->text();
    const int index = m_interval->findData(c.value("hours").toInt()); m_interval->setCurrentIndex(index);
    m_hourly->setValue(c.value("hourly").toInt()); m_daily->setValue(c.value("daily").toInt());
    m_weekly->setValue(c.value("weekly").toInt()); m_monthly->setValue(c.value("monthly").toInt());
    m_excludes->setText(c.value("excludes").toString()); m_loaded = true; m_savedPlan = configuration(); m_loading = false; updateEditState();
}

void Window::refreshDrives() {
    const auto c = m_snapshot.value("config").toObject();
    {
        const QString selected = m_destinationDrive->currentData().toString();
        QSignalBlocker blocker(m_destinationDrive); m_destinationDrive->clear();
        const QString current = c.value("mountpoint").toString();
        m_destinationDrive->addItem(m_snapshot.value("mounted").toBool() ? "Backup drive — " + QFileInfo(current).fileName() : "Configured backup drive (unavailable here)",current);
        for (const auto mount : m_snapshot.value("actual_mounts").toArray())
            if (m_destinationDrive->findData(mount.toString()) < 0) m_destinationDrive->addItem(QFileInfo(mount.toString()).fileName() + " — " + mount.toString(),mount.toString());
        if (m_demo) m_destinationDrive->addItem("Example external drive — /media/example/Backup","/media/example/Backup");
        else for (const auto &volume : QStorageInfo::mountedVolumes()) {
            if (volume.isValid() && volume.isReady() && !volume.isReadOnly() && volume.device().startsWith("/dev/") && m_destinationDrive->findData(volume.rootPath()) < 0)
                m_destinationDrive->addItem((volume.displayName().isEmpty() ? volume.rootPath() : volume.displayName()) + " — " + volume.rootPath(),volume.rootPath());
        }
        if (!selected.isEmpty() && m_destinationDrive->findData(selected) < 0) m_destinationDrive->addItem("Unavailable drive — " + selected,selected);
        m_destinationDrive->setCurrentIndex(selected.isEmpty() ? 0 : m_destinationDrive->findData(selected));
    }
    {
        const QString selected = m_sourceDrive->currentData().toString();
        QSignalBlocker blocker(m_sourceDrive); m_sourceDrive->clear();
        if (m_demo) {m_sourceDrive->addItem("Example drive — /media/example/Documents","/media/example/Documents");}
        else for (const auto &volume : QStorageInfo::mountedVolumes()) {
            if (!volume.isValid() || !volume.isReady() || !volume.device().startsWith("/dev/")) continue;
            m_sourceDrive->addItem((volume.displayName().isEmpty() ? volume.rootPath() : volume.displayName()) + " — " + volume.rootPath(),volume.rootPath());
        }
        const QString source = c.value("source").toString("/home/dist");
        if (c.value("source_kind").toString() == "drive" && m_sourceDrive->findData(source) < 0) m_sourceDrive->addItem("Configured drive — " + source,source);
        if (!selected.isEmpty() && m_sourceDrive->findData(selected) < 0) m_sourceDrive->addItem("Unavailable drive — " + selected,selected);
        if (!selected.isEmpty()) m_sourceDrive->setCurrentIndex(m_sourceDrive->findData(selected));
        else if (m_sourceDrive->findData(source) >= 0) m_sourceDrive->setCurrentIndex(m_sourceDrive->findData(source));
    }
}

void Window::render() {
    const auto s = m_snapshot.value("service").toObject(), t = m_snapshot.value("timer").toObject(), c = m_snapshot.value("config").toObject();
    const auto borg = m_snapshot.value("borg_report").toObject();
    const auto outcome = borg.value("outcome").toString();
    const auto active = s.value("ActiveState").toString();
    const bool running = active == "activating" || active == "active", mounted = m_snapshot.value("mounted").toBool();
    const bool enabled = t.value("ActiveState") == "active";
    const bool reportedWarning = outcome == "warning" && (s.value("Result") == "success" ||
        (s.value("Result") == "exit-code" && s.value("ExecMainCode").toString() == "1" && s.value("ExecMainStatus").toString() == "1"));
    const bool failed = outcome == "error" || (!reportedWarning && (active == "failed" || (!s.value("Result").toString().isEmpty() && s.value("Result") != "success")));
    const auto verification = m_snapshot.value("verification").toObject();
    const auto checkActive = verification.value("service").toObject().value("ActiveState").toString();
    const bool checking = checkActive == "active" || checkActive == "activating" || checkActive == "deactivating";
    const auto checkOutcome = verification.value("attempt").toObject().value("outcome").toString();
    const auto recovery = m_snapshot.value("file_recovery").toObject();
    const bool recovering = recovery.value("active").toBool();
    const auto controls = m_snapshot.value("job_control").toObject();
    const bool backupPaused = controls.value("backup").toObject().value("paused").toBool();
    const bool checkPaused = controls.value("check").toObject().value("paused").toBool();
    const bool held = !controls.value("backup").toObject().value("mode").toString().isEmpty() || !controls.value("check").toObject().value("mode").toString().isEmpty();
    const bool checkWorking = checkActive == "active" && checkOutcome == "running";
    QString title, explanation, state = "warning";
    int statusPage = 1;
    if (recovering) {title = recovery.value("operation") == "browse" ? "Reading saved backup contents" : "Recovering saved files"; explanation = running ? "A backup is waiting while saved files are being read. Review progress in the recovery browser." : "Review progress in the recovery browser. Live files are kept separate from recovery output."; state = "running"; statusPage = 3;}
    else if (checkPaused || backupPaused) {title = checkPaused ? "Integrity check paused" : "Backup paused"; explanation = "Resume or cancel from the tray or run controls. Keep the backup drive connected; the paused job retains its lock."; state = "paused"; statusPage = checkPaused ? 3 : 0;}
    else if (checkWorking) {title = "Checking backup integrity"; explanation = running ? "A backup is waiting while Borg checks stored data. Review check progress under Saved backups." : "Borg is reading and validating stored backup data. Review progress under Saved backups."; state = "running"; statusPage = 3;}
    else if (running) {title = "Backup is running"; explanation = "Follow the Activity tab for Borg output. Closing the monitor does not stop this backup."; state = "running"; statusPage = 4;}
    else if (held) {statusPage = !controls.value("backup").toObject().value("mode").toString().isEmpty() ? 0 : 3; title = controls.value("backup").toObject().value("mode") == "idle_wait" || controls.value("check").toObject().value("mode") == "idle_wait" ? "Waiting for the system to be idle" : "Backup jobs paused or postponed"; explanation = "Review the run controls on Overview or Saved backups, or in the tray menu."; state = "paused";}
    else if (!mounted) {
        title = "Backup destination needs attention";
        const auto mounts = m_snapshot.value("actual_mounts").toArray();
        explanation = mounts.isEmpty() ? "The expected backup filesystem is not mounted. Connect the drive before the next run." : "The backup drive is mounted at " + mounts[0].toString() + ", but the job checks a different path. Choose the detected location in Backup plan to correct it.";
        if (c.value("auto_mount").toBool()) explanation += " Automatic mounting will be attempted when the saved backup job starts.";
    } else if (!m_snapshot.value("source_ready").toBool(true)) {title = "Backup source is unavailable"; explanation = "Connect the source drive and check the selected folder. Automatic mounting can use existing filesystem entries when enabled.";}
    else if (!controls.value("backup").toObject().value("cancelled_invocation").toString().isEmpty() && controls.value("backup").toObject().value("cancelled_invocation") == s.value("InvocationID") && outcome != "success") {title="Backup cancelled"; explanation="The run was cancelled. Partial output is not a completed backup. The normal schedule is unchanged."; state="idle";}
    else if (failed) {title = "Last backup attempt failed"; explanation = "Review the Borg errors and job output in Activity before retrying."; state = "error"; statusPage = 4;}
    else if (checking) {title = "Backup check is queued"; explanation = "Verification is starting or waiting to retry. Review its status under Saved backups."; state = "idle"; statusPage = 3;}
    else if (checkOutcome == "error" || checkOutcome == "warning" || checkOutcome == "interrupted" || checkOutcome == "running") {title = "Backup verification needs attention"; explanation = checkOutcome == "running" ? "The previous check was interrupted. Full verification did not finish; review Saved backups." : verification.value("attempt").toObject().value("message").toString("The check did not finish. Review Saved backups."); state = checkOutcome == "error" ? "error" : "warning"; statusPage = 3;}
    else if (reportedWarning) {title = "Backup completed with warnings"; explanation = "Borg reported problems; some files may have been skipped or changed during the backup. Review the warnings in Activity."; statusPage = 4;}
    else if (outcome == "skipped") {title = "Backup attempt was skipped"; explanation = "This attempt did not create a backup. Another job may have held the backup lock. Check Activity and the saved backups."; statusPage = 4;}
    else if (m_snapshot.contains("free") && m_snapshot.value("free").toDouble() <= c.value("reserve_gib").toInt(10)*1073741824.0) {title = "Backup drive is low on space"; explanation = "Available space is below the configured free-space threshold. Cleanup must reclaim enough space before a new archive can be written.";}
    else if (!enabled) {title = "Automatic backups are disabled"; explanation = "Back up manually, enable the schedule, or set a reminder in Preferences.";}
    else if (s.value("ExecMainExitTimestamp").toString().isEmpty()) {title = "Waiting for a backup result"; explanation = "The schedule is active. No previous attempt details are available. Read the archive catalog to inspect stored backups."; state = "idle";}
    else {title = "Last backup attempt completed"; explanation = outcome == "success" ? "Borg completed without warnings. Read the archive catalog to inspect stored backups." : "The service completed successfully. Detailed Borg results are unavailable for this attempt; inspect Activity and the saved backups."; state = "ok";}
    if (!running && (reportedWarning || failed)) {
        const auto issues = borg.value("issues").toArray();
        for (const auto item : issues) {
            const auto issue = item.toObject();
            if (failed && issue.value("level") == "WARNING") continue;
            explanation += "\n" + issue.value("message").toString().left(500); break;
        }
    }
    m_status->setText(title); m_summary->setText(explanation);
    auto *card = findChild<QFrame *>("statusCard"); card->setProperty("state",state); card->style()->unpolish(card); card->style()->polish(card);
    auto *resolve = findChild<QPushButton *>("statusAction");
    const bool showActivity = statusPage == 4;
    resolve->setProperty("targetPage",statusPage);
    resolve->setText(state == "paused" ? "Review run controls →" : showActivity ? "Review activity →" : statusPage == 3 ? "Review saved backups →" : "Review backup plan →");
    resolve->setVisible(state != "ok");
    m_destination->setText(c.value("repository").toString(c.value("mountpoint").toString())); m_destination->setToolTip("Filesystem UUID: " + c.value("uuid").toString());
    m_contents->setText(c.value("source").toString("/home/dist") + (c.value("include_system").toBool() ? " + system settings and local software" : "") + (c.value("package_recovery").toBool() ? " + software recovery list" : "") + " · Nested filesystems are skipped");
    m_storage->setText(QString("%1 · %2 · Leave %3 GiB free before starting").arg(c.value("scan_mode").toString() == "full_scan" ? "Full file scan" : "Incremental snapshots",c.value("compression").toString("zstd,3")).arg(c.value("reserve_gib").toInt(10)));
    m_schedule->setText(enabled ? stamp(t.value("NextElapseUSecRealtime").toString()) : "Schedule disabled");
    m_attempt->setText(stamp(s.value("ExecMainExitTimestamp").toString()) + " · " + (backupPaused ? "Paused" : running ? "Running" : reportedWarning ? "Completed with warnings" : failed ? "Failed" : outcome == "skipped" ? "Skipped" : stamp(s.value("Result").toString())));
    m_space->setText(mounted ? QString("%1 GiB available / %2 GiB total").arg(m_snapshot.value("free").toDouble()/1073741824.0,0,'f',1).arg(m_snapshot.value("total").toDouble()/1073741824.0,0,'f',1) : "Unavailable at configured mount path");
    renderVerification();
    renderLog();
    m_miniState->setText(title);
    m_trayState = recovering ? "recovery-running" : backupPaused || checkPaused || (!running && !checking && held) ? "paused" : checkWorking ? "check-running" : running ? "backup-running" : state;
    m_tray.setIcon(statusIcon(m_trayState,m_trayFrame));
    if (recovering || ((checkWorking || running) && !backupPaused && !checkPaused)) {if (!m_trayAnimation->isActive()) m_trayAnimation->start();}
    else {m_trayAnimation->stop(); m_trayFrame = 0;}
    m_tray.setToolTip("M45 Drive Backuper — " + title + "\n" + explanation + "\n" + (enabled ? "Next backup: " + stamp(t.value("NextElapseUSecRealtime").toString()) : "Backup schedule disabled"));
    if (state == "warning" || state == "error") alert(title + s.value("InvocationID").toString() + s.value("ExecMainExitTimestamp").toString(),title,explanation,state == "error");
    else {m_lastNotification.clear(); if (!m_demo) m_settings.remove("lastNotification");}
    updateReminders(QDateTime::currentDateTimeUtc());
    setBusy(m_busy);
}

void Window::setBusy(bool busy) {
    m_busy = busy;
    const bool available = !busy && !m_demo && !m_snapshot.isEmpty();
    const auto active = m_snapshot.value("service").toObject().value("ActiveState").toString();
    const bool running = active == "active" || active == "activating" || active == "deactivating";
    const bool autoMount = m_snapshot.value("config").toObject().value("auto_mount").toBool();
    const auto checkState = m_snapshot.value("verification").toObject().value("service").toObject().value("ActiveState").toString();
    const bool recovering = m_snapshot.value("file_recovery").toObject().value("active").toBool();
    const bool checking = checkState == "active" || checkState == "activating" || checkState == "deactivating";
    m_stopCheck->setVisible(checking);
    m_stopCheck->setEnabled(available && checking);
    m_checkNow->setEnabled(available && !running && !checking && !recovering && (m_snapshot.value("mounted").toBool() || autoMount));
    m_verificationMode->setEnabled(!busy && !checking && !recovering && !running);
    m_verificationScope->setEnabled(!busy && !checking && !recovering && !running);
    m_saveVerification->setEnabled(available && !running && !checking && !recovering && !m_verificationRevision.isEmpty() && (m_verificationMode->currentData().toString() != m_verificationSavedMode || m_verificationScope->currentData().toString() != m_verificationSavedScope));
    const auto *selected = m_archives->item(m_archives->currentRow(),1);
    m_checkSelected->setEnabled(available && (m_snapshot.value("mounted").toBool() || autoMount) && !running && !checking && !recovering && !m_archives->selectedItems().isEmpty() && selected &&
        QRegularExpression("^[a-f0-9]{64}$").match(selected->data(Qt::UserRole).toString()).hasMatch() && !selected->text().contains(".checkpoint"));
    const bool hasArchiveSelection = !m_archives->selectedItems().isEmpty();
    m_browseBackup->setText(hasArchiveSelection ? "Browse selected backup…" : "Browse latest backup…");
    m_browseBackup->setEnabled(hasArchiveSelection ? m_checkSelected->isEnabled() : available && !running && !checking && !recovering && (m_snapshot.value("mounted").toBool() || autoMount));
    m_checkDetails->setEnabled(!m_snapshot.value("verification").toObject().value("attempt").toObject().isEmpty());
    m_run->setEnabled(available && !checking && !recovering && !running && (m_snapshot.value("mounted").toBool() || autoMount));
    m_mountDrives->setEnabled(available && !checking && !recovering && !running && autoMount);
    m_mountDrives->setToolTip(autoMount ? "Mount the configured source and destination using existing filesystem entries." : "Enable and save automatic mounting in Backup plan first.");
    m_runAction->setEnabled(m_run->isEnabled()); m_archiveRefresh->setEnabled(available && !checking && !recovering && !running && m_snapshot.value("mounted").toBool());
    const bool scheduled = m_snapshot.value("timer").toObject().value("ActiveState") == "active";
    m_enable->setEnabled(available && !scheduled); m_disable->setEnabled(available && scheduled);
    m_run->setToolTip(running ? "A backup is already running." : autoMount ? "Start the saved backup plan, attempting to mount configured drives first." : !m_snapshot.value("mounted").toBool() ? "Connect the backup drive or review its location in Backup plan." : "Start the saved backup plan.");
    m_planPage->setEnabled(m_loaded && !busy); m_exclusionsPage->setEnabled(m_loaded && !busy);
    updateEditState();
    m_startup->setEnabled(!m_demo); m_notifications->setEnabled(!m_demo);
    m_exportRecovery->setEnabled(available); m_restorePackages->setEnabled(available);
    m_recoveryToggle->setEnabled(m_loaded && !busy);
    updateRecoveryStatus();
    renderIdlePolicy();
    updateJobControls();
}

void Window::action(const QString &name, const QString &payload) {
    if (m_demo || m_busy) return;
    if (name == "start" && m_loaded && configuration() != m_savedPlan &&
        QMessageBox::question(this,"Run the saved backup plan?","You have unsaved changes. Back up using the saved settings anyway?",QMessageBox::Yes | QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes) return;
    setBusy(true); m_feedback->setText(name == "recovery-export" || name == "packages-preview" ? "Preparing software recovery information…" : "Authenticating and processing your request…");
    auto *process = new QProcess(this);
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {setBusy(false); m_feedback->setText("Could not start authentication: " + process->errorString()); process->deleteLater();}
    });
    connect(process,&QProcess::finished,this,[this,process,name,payload](int code,QProcess::ExitStatus) {
        const auto output = process->readAllStandardOutput(); const auto doc = QJsonDocument::fromJson(output);
        const auto result = doc.object();
        if (code || !doc.isObject()) {
            m_feedback->setText("Action failed or authentication was cancelled.");
            QString reason = result.value("error").toString(QString::fromUtf8(process->readAllStandardError()).trimmed());
            if (reason.isEmpty()) reason = "The action was not completed. Administrator authentication may have been cancelled.";
            QMessageBox::warning(this,"M45 Drive Backuper",reason);
        } else if (name == "recovery-export") {
            const auto path = QFileDialog::getSaveFileName(this,"Save software recovery file",QDir::homePath()+"/m45-software-recovery.json","M45 software recovery (*.json)");
            if (!path.isEmpty()) {
                QSaveFile file(path); const auto bytes = QJsonDocument(result.value("manifest").toObject()).toJson();
                if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) || file.write(bytes) != bytes.size() || !file.commit()) QMessageBox::warning(this,"Software recovery","Could not save the recovery file.");
                else {
                    m_feedback->setText("Software recovery file saved: " + path);
                    const auto warnings = result.value("manifest").toObject().value("warnings").toArray();
                    if (!warnings.isEmpty()) {QStringList lines; for (const auto item : warnings) lines << item.toString(); QMessageBox::warning(this,"Some inventories are incomplete",lines.join('\n'));}
                }
            } else m_feedback->setText("Export cancelled.");
        } else if (name == "packages-preview") {
            setBusy(false);
            QMessageBox review(QMessageBox::Question,"Review package reinstallation",QString("APT will resolve %1 saved package selections using this machine’s current repositories. Review the detailed plan before installing.\n\nSnap, Flatpak, service and configuration restoration remain separate steps.").arg(result.value("package_count").toInt()),QMessageBox::Yes|QMessageBox::Cancel,this);
            review.setTextFormat(Qt::PlainText); review.setDetailedText(result.value("preview").toString()); review.setDefaultButton(QMessageBox::Cancel);
            review.button(QMessageBox::Yes)->setText("Install packages…");
            const bool confirmed = review.exec() == QMessageBox::Yes; process->deleteLater();
            if (confirmed) action("packages-install",payload); else m_feedback->setText("Package review closed. No packages were installed.");
            return;
        } else if (name == "archives") {
            showArchives(result.value("archives").toArray());
            m_archiveHint->setText(QString("%1 snapshots · Last refreshed %2. Checkpoints may be incomplete; catalog entries do not verify integrity.").arg(m_archives->rowCount()).arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
            m_feedback->setText("Backup list refreshed.");
        } else {if (name == "verification-configure") m_verificationLoaded = false; if (name == "idle-configure") m_idleLoaded = false; m_feedback->setText(result.value("message").toString()); if (name == "apply" || name == "upgrade-reporting" || ((name == "verification-configure" || name == "verification-start") && configuration() == m_savedPlan)) m_loaded = false;}
        setBusy(false); poll(); process->deleteLater();
    });
    const bool manifestInput = name == "packages-preview" || name == "packages-install";
    QStringList args{"/usr/libexec/m45-drive-backuper/backend",name}; if (!payload.isEmpty() && !manifestInput) args << payload;
    process->start(name == "recovery-export" || name == "packages-preview" ? "/usr/bin/python3" : "/usr/bin/pkexec",args);
    if (manifestInput) {process->write(payload.toUtf8()); process->closeWriteChannel();}
}

QJsonObject Window::configuration() const {
    return QJsonObject{{"mountpoint",m_destinationKind->currentData() == "drive" ? m_destinationDrive->currentData().toString() : m_mount->text()}, {"repository",m_repository->text()}, {"destination_kind",m_destinationKind->currentData().toString()}, {"hours",m_interval->currentData().toInt()}, {"revision",m_revision},
        {"hourly",m_hourly->value()},{"daily",m_daily->value()},{"weekly",m_weekly->value()},{"monthly",m_monthly->value()},{"excludes",m_excludes->text()},
        {"source",m_source->text()},{"source_kind",m_sourceKind->currentData().toString()}, {"compression",m_compression->currentData().toString()},
        {"scan_mode",m_backupType->currentData().toString()}, {"reserve_gib",m_reserve->value()}, {"quota_gib",m_quotaMode->currentData().toInt() == 1 ? m_quota->value() : m_quotaMode->currentData().toInt()}, {"exclude_caches",m_skipCaches->isChecked()}, {"auto_mount",m_autoMount->isChecked()}, {"include_system",m_includeSystem->isChecked()}, {"package_recovery",m_packageRecovery->isChecked()}};
}

void Window::saveSettings() {
    if (m_interval->currentIndex() < 0 || m_compression->currentIndex() < 0 || m_backupType->currentIndex() < 0) {QMessageBox::warning(this,"Backup settings","Choose a schedule, backup type and compression before saving."); return;}
    const auto data = configuration();
    QString summary = QString("Back up: %1\nSave to: %2\nSchedule: %3\n\n%4")
        .arg(m_source->text(),m_repository->text(),m_interval->currentText(),m_excludes->summary());
    if (m_includeSystem->isChecked()) summary += "\n\nAlso includes system settings, local software, administrator files, scheduled jobs and package records.";
    if (m_packageRecovery->isChecked()) summary += "\nA software recovery list will be refreshed with each backup.";
    else summary += "\nAutomatic package recovery will be disabled for future backups.";
    const auto exclusionDelta = exclusionChanges(m_savedPlan.value("excludes").toString(),m_excludes->text());
    if (!exclusionDelta.isEmpty()) summary += "\n\n" + exclusionDelta;
    if (data.value("repository") != m_savedPlan.value("repository")) summary += "\n\nExisting Borg backups in this folder will be reused, including the saved cleanup policy. An empty folder will be initialized as an unencrypted repository. Previous backup folders are kept.";
    bool retentionChanged = false;
    for (const auto &key : {"hourly","daily","weekly","monthly"}) retentionChanged |= data.value(key) != m_savedPlan.value(key);
    if (retentionChanged) summary += "\n\nRetention changed. Older backups beyond the new limits will be removed on a future run.";
    const QString details = QString("Type: %1\nCompression: %2\nLeave free: %3 GiB\nMount configured drives: %4\nSkip marked caches: %5\nKeep: %6 hourly, %7 daily, %8 weekly, %9 monthly\nStorage quota: %10 (-1 preserves current, 0 is unlimited)\n\nExclusion rules:\n%11")
        .arg(m_backupType->currentText(),m_compression->currentText()).arg(m_reserve->value()).arg(m_autoMount->isChecked() ? "Yes" : "No",m_skipCaches->isChecked() ? "Yes" : "No")
        .arg(m_hourly->value()).arg(m_daily->value()).arg(m_weekly->value()).arg(m_monthly->value()).arg(data.value("quota_gib").toInt()).arg(m_excludes->text());
    QMessageBox review(QMessageBox::Question,m_demo ? "Preview your backup plan" : "Review backup settings",summary,
                       m_demo ? QMessageBox::Ok : QMessageBox::Save | QMessageBox::Cancel,this);
    review.setTextFormat(Qt::PlainText); review.setDetailedText(details);
    if (review.exec() == QMessageBox::Save && !m_demo)
        action("apply",QString::fromLatin1(QJsonDocument(data).toJson(QJsonDocument::Compact).toBase64()));
}

void Window::setStartup(bool enabled) {
    if (m_demo) return;
    const auto path = startupPath(); QDir().mkpath(QFileInfo(path).absolutePath()); QSaveFile file(path);
    const QByteArray text = QByteArray("[Desktop Entry]\nType=Application\nName=M45 Drive Backuper\nExec=/usr/bin/m45-drive-backuper --autostart\nIcon=m45-drive-backuper\nTerminal=false\nHidden=") + (enabled ? "false\n" : "true\n");
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit()) {
        m_startup->blockSignals(true); m_startup->setChecked(!enabled); m_startup->blockSignals(false);
        QMessageBox::warning(this,"Login startup","Could not save the autostart setting.");
    }
}
void Window::selectPage(int page) {m_tabs->setCurrentIndex(page);}
void Window::Show() {showNormal(); raise(); activateWindow();}
void Window::closeEvent(QCloseEvent *event) {event->ignore(); if (m_demo || !QSystemTrayIcon::isSystemTrayAvailable()) quitMonitor(); else hide();}

void Window::updateEditState() {
    if (m_loading || !m_loaded) return;
    updateRecoveryStatus();
    const bool dirty = configuration() != m_savedPlan;
    const auto active = m_snapshot.value("service").toObject().value("ActiveState").toString();
    const auto verificationActive = m_snapshot.value("verification").toObject().value("service").toObject().value("ActiveState").toString();
    const bool running = active == "active" || active == "activating" || active == "deactivating" || verificationActive == "active" || verificationActive == "activating" || verificationActive == "deactivating" || m_snapshot.value("file_recovery").toObject().value("active").toBool();
    m_changes->setText(dirty ? (m_demo ? "Preview edits · no changes are written" : "Unsaved changes · backups still use the saved plan") : "Backup plan is up to date");
    const auto exclusionDelta = exclusionChanges(m_savedPlan.value("excludes").toString(),m_excludes->text());
    if (!exclusionDelta.isEmpty()) m_changes->setText(m_changes->text() + "\n" + exclusionDelta);
    m_save->setEnabled(dirty && !m_busy && !running && (m_demo || !m_snapshot.isEmpty()));
    m_updateReporting->setVisible(!m_demo && !m_snapshot.isEmpty() && !m_snapshot.value("config").toObject().value("warning_reporting").toBool());
    m_updateReporting->setEnabled(!m_demo && !dirty && !m_busy && !running && !m_snapshot.isEmpty());
    m_discard->setEnabled(dirty && !m_busy);
    m_editBar->setVisible(dirty || m_tabs->currentIndex() == 1 || m_tabs->currentIndex() == 2);
    setWindowModified(dirty); setWindowTitle("M45 Drive Backuper[*]");
}

void Window::updateRecoveryStatus() {
    const auto config = m_snapshot.value("config").toObject();
    if (config.isEmpty()) {m_recoveryStatus->setText("Saved recovery status is unavailable. Check Overview."); return;}
    const bool saved = config.value("package_recovery").toBool();
    QString text = saved ? "Saved plan: package recovery is included in every backup." : "Saved plan: automatic package recovery is off.";
    if (saved && m_snapshot.value("timer").toObject().value("ActiveState") != "active") text += "\nThe automatic backup schedule is currently paused.";
    if (m_loaded && saved != m_packageRecovery->isChecked()) text += m_packageRecovery->isChecked() ? "\nUnsaved change: enable package recovery. Save changes to apply." : "\nUnsaved change: disable package recovery. Save changes to apply.";
    m_recoveryStatus->setText(text);
}

void Window::renderLog() {
    const auto report = m_snapshot.value("borg_report").toObject();
    QStringList issues;
    for (const auto item : report.value("issues").toArray()) {
        const auto issue = item.toObject();
        issues << issue.value("level").toString() + " · Borg " + issue.value("operation").toString() + ": " + issue.value("message").toString();
    }
    const auto outcome = report.value("outcome").toString();
    if (issues.isEmpty() && (outcome == "warning" || outcome == "error")) issues << "The job reported a problem. Its detailed Borg messages are unavailable in the retained journal; review the job output below.";
    m_borgMessages->setVisible(!issues.isEmpty()); m_borgHeading->setVisible(!issues.isEmpty());
    const auto issueText = issues.join("\n\n");
    if (m_borgMessages->toPlainText() != issueText) m_borgMessages->setPlainText(issueText);
    auto entries = m_snapshot.value("activity").toArray();
    // Older installed helpers return readable text only. Keep that output usable
    // without inventing timestamps or severity for unstructured messages.
    if (!m_snapshot.contains("activity")) {
        static const QRegularExpression prefix("^(?:(\\d{4}-\\d{2}-\\d{2}T\\S+) )?(?:(DEBUG|INFO|WARNING|ERROR|CRITICAL) · )?(.*)$");
        for (const auto &line : m_snapshot.value("logs").toString().split('\n')) {
            if (line.isEmpty()) continue;
            const auto match = prefix.match(line);
            entries.append(QJsonObject{{"timestamp",match.captured(1)}, {"level",match.captured(2)},
                {"source","Service"}, {"message",match.captured(3)}, {"raw",line}});
        }
    }
    const auto filter = m_logSearch->text(); QJsonArray visible; QStringList selected;
    for (const auto value : entries) {
        const auto entry = value.toObject();
        const auto level = entry.value("level").toString();
        const auto timestamp = entry.value("timestamp").toString();
        const auto time = QDateTime::fromString(timestamp,Qt::ISODate).toLocalTime();
        const auto displayTime = time.isValid() ? time.toString("yyyy-MM-dd HH:mm:ss t") : timestamp;
        const auto text = displayTime + " " + level + " · " + entry.value("source").toString() + ": " + entry.value("message").toString();
        const bool problem = level == "WARNING" || level == "ERROR" || level == "CRITICAL";
        if ((m_logLevel->currentIndex() == 0 || problem) && (filter.isEmpty() || text.contains(filter,Qt::CaseInsensitive))) {
            visible.append(entry);
            const auto raw = entry.value("raw").toString();
            if (raw.isEmpty()) selected << text.trimmed();
            else if (timestamp.isEmpty() || !m_snapshot.contains("activity")) selected << raw;
            else selected << displayTime + " " + raw;
        }
    }
    QString count = visible.isEmpty() ? (entries.isEmpty() ? "No backup activity available yet." : "No activity matches this filter.")
        : QString("%1 of %2 entries").arg(visible.size()).arg(entries.size());
    if (m_snapshot.value("log_truncated").toBool()) count += " · Latest 500 journal entries";
    m_logCount->setText(count);
    const auto error = m_snapshot.value("log_error").toString();
    m_logAccess->setText(error.isEmpty() ? QString() : "Journal access: " + error); m_logAccess->setVisible(!error.isEmpty());
    if (!error.isEmpty()) selected << "Journal access: " + error;
    auto result = selected.join('\n');
    if (result.isEmpty()) result = count;
    if (m_logs->toPlainText() != result) {
        const auto position = m_logs->verticalScrollBar()->value(); m_logs->setPlainText(result);
        m_logs->verticalScrollBar()->setValue(m_followLog->isChecked() ? m_logs->verticalScrollBar()->maximum() : position);
    }
    const auto key = QJsonDocument(visible).toJson(QJsonDocument::Compact);
    if (m_activity->property("entries").toByteArray() == key) return;
    m_activity->setProperty("entries",key);
    const auto position = m_activity->verticalScrollBar()->value();
    m_activity->setRowCount(visible.size());
    for (int row=0;row<visible.size();++row) {
        const auto entry = visible[row].toObject();
        const auto timestamp = entry.value("timestamp").toString();
        const auto time = QDateTime::fromString(timestamp,Qt::ISODate).toLocalTime();
        const auto level = entry.value("level").toString();
        const auto levelText = level.isEmpty() ? QString("Output") : level.left(1) + level.mid(1).toLower();
        const QStringList cells{time.isValid() ? time.toString("MMM d, yyyy\nHH:mm:ss") : timestamp.isEmpty() ? "—" : timestamp,
            levelText, entry.value("source").toString(), entry.value("message").toString()};
        for (int column=0;column<cells.size();++column) {
            auto *item = new QTableWidgetItem(cells[column]); item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            item->setToolTip(column == 0 && time.isValid() ? time.toString("yyyy-MM-dd HH:mm:ss t") : cells[column]);
            if (column == 1 && (level == "WARNING" || level == "ERROR" || level == "CRITICAL")) {
                item->setIcon(style()->standardIcon(level == "WARNING" ? QStyle::SP_MessageBoxWarning : QStyle::SP_MessageBoxCritical));
                auto font = item->font(); font.setBold(true); item->setFont(font);
            }
            if (column == 3) item->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            m_activity->setItem(row,column,item);
        }
    }
    if (m_followLog->isChecked()) m_activity->scrollToBottom(); else m_activity->verticalScrollBar()->setValue(position);

}

void Window::showArchives(const QJsonArray &archives) {
    m_archives->setSortingEnabled(false); m_archives->setRowCount(archives.size());
    for (int row=0;row<archives.size();++row) {
        const auto item = archives[row].toObject(); const auto name = item.value("name").toString();
        const QStringList columns{item.value("time").toString(),name,name.contains(".checkpoint") ? "Checkpoint · may be incomplete" : "Snapshot"};
        for (int col=0;col<columns.size();++col) m_archives->setItem(row,col,new QTableWidgetItem(columns[col]));
        m_archives->item(row,1)->setData(Qt::UserRole,item.value("id").toString());
    }
    m_archives->setSortingEnabled(true); m_archives->sortItems(0,Qt::DescendingOrder);
    setBusy(m_busy);
}

void Window::openAnalyzer() {
    if (m_demo) {QMessageBox::information(this,"K4DirStat preview","In normal mode, this opens K4DirStat on your selected source folder. Use its size map to find unwanted folders, then add them with “Skip a folder”."); return;}
    const auto program = QStandardPaths::findExecutable("k4dirstat");
    if (program.isEmpty()) {
        QMessageBox message(QMessageBox::Information,"Find large folders with K4DirStat","K4DirStat can help you see which folders use the most space. Install it from your distribution’s software manager, then return here.\n\nOn Ubuntu/Debian: sudo apt install k4dirstat",QMessageBox::Ok,this);
        auto *copy = message.addButton("Copy install command",QMessageBox::ActionRole);
        message.exec(); if (message.clickedButton() == copy) QApplication::clipboard()->setText("sudo apt install k4dirstat");
        return;
    }
    const auto path = m_source->text();
    if (!QFileInfo(path).isDir()) {QMessageBox::information(this,"Choose a source folder","Choose an available source folder or mounted drive in Backup plan first."); return;}
    if (!QProcess::startDetached(program,{path})) QMessageBox::warning(this,"K4DirStat","Could not open K4DirStat. Try launching it from the Applications menu.");
}

void Window::quitMonitor() {
    if (!m_demo && m_loaded && configuration() != m_savedPlan && QMessageBox::question(this,"Quit with unsaved changes?","Your backup plan edits have not been saved. Quit and discard them?",QMessageBox::Discard | QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Discard) return;
    hide(); qApp->quit();
}

void Window::alert(const QString &key, const QString &title, const QString &message, bool critical) {
    if (!m_notifications->isChecked() || m_lastNotification == key) return;
    emit attentionRequested(title,message,critical);
    m_lastNotification = key;
    if (!m_demo) m_settings.setValue("lastNotification",key);
}

QString Window::driveRepository() const {
    const QString target = m_destinationDrive->currentData().toString();
    const auto config = m_snapshot.value("config").toObject();
    bool existingDrive = target == config.value("mountpoint").toString();
    for (const auto mount : m_snapshot.value("actual_mounts").toArray()) existingDrive |= target == mount.toString();
    if (existingDrive) {
        const auto original = config.value("repository").toString(QDir(config.value("mountpoint").toString()).filePath("borg-home-example"));
        const auto relative = QDir(config.value("mountpoint").toString()).relativeFilePath(original);
        if (!relative.startsWith("..") && !QDir::isAbsolutePath(relative)) return QDir(target).filePath(relative);
    }
    return QDir(target).filePath("M45-Backups");
}

void Window::renderVerification() {
    const auto verification = m_snapshot.value("verification").toObject();
    const auto mode = verification.value("mode").toString("manual");
    const auto scope = verification.value("scope").toString("latest");
    if (!m_verificationLoaded || (m_verificationMode->currentData().toString() == m_verificationSavedMode && m_verificationScope->currentData().toString() == m_verificationSavedScope)) {
        QSignalBlocker blocker(m_verificationMode), scopeBlocker(m_verificationScope);
        m_verificationScope->setCurrentIndex(m_verificationScope->findData(scope));
        m_verificationSavedScope = scope;
        m_verificationMode->setCurrentIndex(m_verificationMode->findData(mode));
        m_verificationSavedMode = mode;
        m_verificationRevision = verification.value("revision").toString();
        m_verificationLoaded = true;
    }
    const auto attempt = verification.value("attempt").toObject();
    const auto service = verification.value("service").toObject();
    const auto active = service.value("ActiveState").toString();
    const bool checking = active == "active" || active == "activating" || active == "deactivating";
    const auto outcome = attempt.value("outcome").toString();
    QString text;
    if (m_snapshot.value("job_control").toObject().value("check").toObject().value("paused").toBool()) text = "Integrity check paused. Resume or cancel using the run controls.";
    else if (checking) {text = active == "active" && outcome == "running" ? "Backup check is running." : "Backup check is starting or waiting to retry."; if (outcome == "running" && !attempt.value("message").toString().isEmpty()) text += "\n" + attempt.value("message").toString();}
    else if (!m_snapshot.value("job_control").toObject().value("check").toObject().value("cancelled_invocation").toString().isEmpty() && m_snapshot.value("job_control").toObject().value("check").toObject().value("cancelled_invocation") == service.value("InvocationID") && outcome != "success") text = "Integrity check cancelled. The incomplete run is not recorded as successful.";
    else if (outcome == "running") text = "Last check was interrupted. Full verification did not finish.";
    else text = attempt.value("message").toString("No successful check is recorded for this backup.");
    if (!verification.value("error").toString().isEmpty()) text = "Verification monitoring unavailable: " + verification.value("error").toString();
    const auto last = verification.value("last_success").toObject();
    const auto passed = last.value("finished_at").toString();
    const auto full = verification.value("last_full_success").toObject();
    if (!passed.isEmpty()) text += "\nLast successful check: " + QDateTime::fromString(passed,Qt::ISODateWithMs).toLocalTime().toString("yyyy-MM-dd HH:mm:ss") +
        (last.value("scope").toString("all") == "all" ? " · whole repository" : " · " + last.value("archive").toString("snapshot"));
    else if (!attempt.isEmpty() || checking) text += "\nNo successful check is recorded for this backup.";
    if (!full.isEmpty() && full.value("finished_at") != last.value("finished_at")) text += "\nLast whole-repository check: " + QDateTime::fromString(full.value("finished_at").toString(),Qt::ISODateWithMs).toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
    const auto timer = verification.value("timer").toObject();
    if (timer.value("ActiveState") == "active") text += "\nNext check: " + stamp(timer.value("NextElapseUSecRealtime").toString());
    else if (mode == "manual") text += "\nAutomatic verification is off (manual only).";
    else if (mode == "after_backup") text += "\nChecks are queued after each completed backup, including backups with warnings.";
    else text += "\nThe verification schedule is not active.";
    if (m_verificationMode->currentData().toString() != m_verificationSavedMode || m_verificationScope->currentData().toString() != m_verificationSavedScope) text += "\nUnsaved check options. Automatic checks still use the saved options.";
    m_verificationStatus->setText(text);
}

void Window::startVerification(const QString &scope, const QString &archiveId) {
    if (m_demo || m_busy) return;
    QString description;
    if (scope == "all") description = "Verify the whole repository and all saved backups?";
    else if (scope == "latest") description = "Read and validate the most recent completed backup? Data used only by older snapshots is not included.";
    else {
        const auto *item = m_archives->item(m_archives->currentRow(),1);
        if (!item || archiveId.isEmpty()) return;
        description = "Read and validate this saved backup?\n" + item->text();
    }
    description += "\n\nThis can take hours. No files will be restored and no repairs will be made. Backups will wait; closing the app does not stop the check.";
    if (QMessageBox::question(this,"Check backup?",description,QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes) return;
    const QJsonObject data{{"scope",scope},{"archive_id",archiveId},{"revision",m_snapshot.value("config").toObject().value("revision").toString()}};
    action("verification-start",QString::fromLatin1(QJsonDocument(data).toJson(QJsonDocument::Compact).toBase64()));
}

QString Window::reminderKey(const QString &kind) const {
    const auto config = m_snapshot.value("config").toObject();
    const QJsonArray identity{config.value("uuid"),config.value("repository"),config.value("source")};
    return "reminders/state/" + QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()) + "/" + kind + (kind == "check" ? "/" + m_verificationSavedScope : QString());
}

void Window::updateReminders(const QDateTime &now) {
    if (m_demo) {m_reminderStatus->setText("Preview only · Reminders are off and no settings are saved."); return;}
    if (m_snapshot.value("config").toObject().isEmpty()) {m_reminderStatus->setText("Waiting for backup status…"); return;}
    const auto service = m_snapshot.value("service").toObject();
    const auto verification = m_snapshot.value("verification").toObject();
    const auto isRunning = [](const QJsonObject &s) {
        return QStringList{"active","activating","deactivating"}.contains(s.value("ActiveState").toString());
    };
    const bool working = m_busy || isRunning(service) || isRunning(verification.value("service").toObject()) || m_snapshot.value("file_recovery").toObject().value("active").toBool();
    QDateTime backupSuccess;
    if (!isRunning(service) && m_snapshot.value("borg_report").toObject().value("outcome") == "success") {
        // Journal timestamps are ISO UTC, unlike systemd's localized display strings.
        for (const auto &entry : m_snapshot.value("activity").toArray()) {
            const auto time = QDateTime::fromString(entry.toObject().value("timestamp").toString(),Qt::ISODateWithMs);
            if (time.isValid() && (!backupSuccess.isValid() || time > backupSuccess)) backupSuccess = time;
        }
    }
    const auto checkRecord = verification.value(m_verificationSavedScope == "all" ? "last_full_success" : "last_success").toObject();
    const auto checkSuccess = QDateTime::fromString(checkRecord.value("finished_at").toString(),Qt::ISODateWithMs);
    QStringList summary;
    bool sent = false;
    for (const auto &kind : {QString("backup"),QString("check")}) {
        const bool backup = kind == "backup";
        const int days = (backup ? m_backupReminder : m_checkReminder)->currentData().toInt();
        const QString name = backup ? "Backup" : "Integrity check";
        if (!days) {summary << name + " reminder: off."; continue;}
        const bool automatic = backup ? m_snapshot.value("timer").toObject().value("ActiveState") == "active" : verification.value("mode").toString("manual") != "manual" || verification.value("timer").toObject().value("ActiveState") == "active";
        const bool unavailable = !m_snapshot.value("error").toString().isEmpty() || (!backup && !verification.value("error").toString().isEmpty());
        const auto result = Reminders::evaluate(m_settings,reminderKey(kind),days,backup ? backupSuccess : checkSuccess,automatic || working || unavailable || sent,now);
        summary << name + (automatic ? " reminder: paused while automatic runs are enabled." : unavailable ? " reminder: waiting for status." : result.due <= now ? " reminder: due." : " reminder: " + result.due.toLocalTime().toString("yyyy-MM-dd HH:mm") + ".");
        if (result.notify) {
            sent = true;
            emit reminderRequested(backup ? "Time to back up" : "Time to check your backup",backup ? "Your manual backup reminder is due. Open Overview and choose Back up now when ready." : "Your integrity-check reminder is due. Open Saved backups and choose Check now when ready.",backup ? 0 : 3);
        }
    }
    updateJobControls();
    m_reminderStatus->setText(summary.join('\n'));
}

QMenu *Window::createJobControls(const QString &kind) {
    auto *menu = new QMenu(kind == "backup" ? "Backup controls" : "Integrity check controls",this);
    menu->setObjectName(kind + "Controls");
    auto add = [this,menu,kind](const QString &text,const QString &op,int delay=0) {
        auto *item=menu->addAction(text,this,[this,kind,op,delay] {jobControl(kind,op,delay);});
        item->setObjectName(kind + "-" + op + QString::number(delay));
        item->setProperty("operation",op); return item;
    };
    add("Pause", "pause"); add("Resume", "resume");
    add("Postpone for 1 hour…", "postpone",3600);
    add("Postpone for 4 hours…", "postpone",14400);
    add("Postpone for 24 hours…", "postpone",86400);
    menu->addSeparator(); add("Cancel…", "cancel");
    menu->addSeparator(); add("Snooze reminder for 1 day", "snooze");
    return menu;
}

void Window::jobControl(const QString &kind,const QString &operation,int delay) {
    if (m_demo || m_busy) return;
    const bool backup=kind == "backup";
    if (operation == "snooze") {
        m_settings.setValue(reminderKey(kind)+"/snoozedUntil",QDateTime::currentDateTimeUtc().addDays(1));
        updateReminders(QDateTime::currentDateTimeUtc());
        m_feedback->setText("Reminder snoozed for one day."); return;
    }
    const auto service=backup ? m_snapshot.value("service").toObject() : m_snapshot.value("verification").toObject().value("service").toObject();
    const bool active=QStringList{"active","activating","deactivating"}.contains(service.value("ActiveState").toString());
    const QString name=backup ? "backup" : "integrity check";
    QString text;
    if (operation == "cancel") text="Cancel this " + name + " run or pending postponement? An incomplete run does not count as successful. Future scheduled runs remain enabled as before.";
    else if (operation == "postpone") text=active ? QString("Pause the current %1 and automatically resume it in %2 hours? Keep the backup drive connected. The paused job retains its repository lock; other backup work may wait. Closing the monitor does not cancel the postponement.").arg(name).arg(delay/3600) : QString("Start a %1 in %2 hours? Automatic runs of this job will wait until then. This one-time run works even when the monitor is closed. Your normal schedule remains unchanged.").arg(name).arg(delay/3600);
    else if (operation == "pause") text=active ? "Pause the current " + name + "? Keep the backup drive connected. It retains its repository lock, so other backup work may wait. Resume it here or from the tray." : "Pause future " + name + " runs until you choose Resume? Your schedule settings are kept.";
    if (!text.isEmpty() && QMessageBox::question(this,"Manage " + name,text,QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes) return;
    const QJsonObject request{{"kind",kind},{"operation",operation},{"delay",delay},{"revision",m_snapshot.value("config").toObject().value("revision")},{"invocation",active ? service.value("InvocationID").toString() : QString()}};
    action("job-control",QString::fromLatin1(QJsonDocument(request).toJson(QJsonDocument::Compact).toBase64()));
}

void Window::updateJobControls() {
    const auto controls=m_snapshot.value("job_control").toObject();
    for (const auto &kind : {QString("backup"),QString("check")}) {
        const bool backup=kind == "backup";
        auto *menu=backup ? m_backupControls : m_checkControls;
        const auto entry=controls.value(kind).toObject();
        const auto service=backup ? m_snapshot.value("service").toObject() : m_snapshot.value("verification").toObject().value("service").toObject();
        const auto state=service.value("ActiveState").toString();
        const bool active=QStringList{"active","activating","deactivating"}.contains(state);
        const bool paused=entry.value("paused").toBool(), held=!entry.value("mode").toString().isEmpty();
        const bool available=!m_demo && !m_busy && !m_snapshot.value("config").toObject().isEmpty();
        for (auto *item:menu->actions()) {
            const auto op=item->property("operation").toString();
            if (op.isEmpty()) continue;
            item->setEnabled(available && state != "deactivating" && (op == "cancel" || !active || entry.value("can_pause").toBool()));
            if (op == "snooze") {item->setVisible((backup ? m_backupReminder : m_checkReminder)->currentData().toInt()>0); item->setEnabled(available);}
            if (op == "pause") {item->setVisible(!paused && !held); item->setText(active ? "Pause current run…" : "Pause future runs…");}
            if (op == "resume") {item->setVisible(paused || held); item->setText(!active && (entry.value("mode") == "postponed" || entry.value("mode") == "idle_wait") ? (entry.value("mode") == "idle_wait" ? "Run waiting job now" : "Run postponed job now") : "Resume");}
            if (op == "cancel") {item->setVisible(active || held); item->setText(active ? "Cancel current run…" : (entry.value("mode") == "postponed" || entry.value("mode") == "idle_wait") ? (entry.value("mode") == "idle_wait" ? "Cancel waiting run…" : "Cancel postponed run…") : "Clear schedule pause…");}
        }
        auto *pause=backup ? m_pauseBackup : m_pauseCheck;
        pause->setVisible((active || held) && state != "deactivating"); pause->setText(!active && held ? ((entry.value("mode") == "postponed" || entry.value("mode") == "idle_wait") ? "Run now" : "Resume schedule") : paused ? "Resume" : "Pause"); pause->setEnabled(available && (!active || entry.value("can_pause").toBool()));
        auto *trayPause=findChild<QAction *>(kind + "TrayPause");
        auto *trayCancel=findChild<QAction *>(kind + "TrayCancel");
        if (trayPause) {trayPause->setVisible(active || held); trayPause->setEnabled(pause->isEnabled()); trayPause->setText(QString(paused || held ? "Resume " : "Pause ") + (backup ? "backup" : "integrity check"));}
        if (trayCancel) {trayCancel->setVisible(active || held); trayCancel->setEnabled(available && state != "deactivating");}
        auto *cancel=backup ? m_cancelBackup : m_stopCheck;
        cancel->setVisible(active || held); cancel->setText(active ? (backup ? "Cancel backup…" : "Cancel check…") : "Cancel postponement / pause…"); cancel->setEnabled(available && state != "deactivating");
        QString text;
        if (paused) text="Paused · Keep the backup drive connected. Other jobs may wait for the repository lock.";
        else if (entry.value("mode") == "idle_wait") text="Waiting for idle · You can run now or cancel from here or the tray.";
        else if (held) text="Future runs temporarily paused.";
        if (entry.value("until").toDouble() > 0) text += "\n" + QString(entry.value("mode") == "idle_wait" ? "Starts by: " : active ? "Resumes: " : "One-time run: ") + QDateTime::fromSecsSinceEpoch(qint64(entry.value("until").toDouble())).toLocalTime().toString("yyyy-MM-dd HH:mm");
        if (!controls.value("error").toString().isEmpty()) text="Run control status unavailable: " + controls.value("error").toString();
        auto *status=backup ? m_backupControlStatus : m_checkControlStatus;
        status->setText(text); status->setVisible(!text.isEmpty());
        if ((held || paused) && backup) m_schedule->setText(entry.value("until").toDouble()>0 ? QString(entry.value("mode") == "idle_wait" ? "Idle wait, starts by " : "Postponed until ") + QDateTime::fromSecsSinceEpoch(qint64(entry.value("until").toDouble())).toLocalTime().toString("yyyy-MM-dd HH:mm") : "Temporarily paused");
        if (held || paused) {
            if (backup) {m_run->setEnabled(false); m_runAction->setEnabled(false);}
            else {m_checkNow->setEnabled(false); m_checkSelected->setEnabled(false);}
        }
    }
}

QJsonObject Window::idleConfiguration() const {
    return QJsonObject{{"backup",m_idleBackup->isChecked()},{"check",m_idleCheck->isChecked()},{"max_hours",m_idleMax->currentData().toInt()}};
}

void Window::renderIdlePolicy() {
    const auto policy=m_snapshot.value("idle_policy").toObject();
    if (!m_idleLoaded || idleConfiguration() == m_idleSaved) {
        QSignalBlocker backup(m_idleBackup), check(m_idleCheck), hours(m_idleMax);
        m_idleBackup->setChecked(policy.value("backup").toBool());
        m_idleCheck->setChecked(policy.value("check").toBool());
        m_idleMax->setCurrentIndex(qMax(0,m_idleMax->findData(policy.value("max_hours").toInt(6))));
        m_idleSaved=idleConfiguration(); m_idleRevision=policy.value("revision").toString(); m_idleLoaded=true;
    }
    const bool supported=policy.value("available").toBool(m_demo);
    const bool available=!m_demo && !m_busy && supported && !m_idleRevision.isEmpty();
    m_idleBackup->setEnabled(!m_busy && supported); m_idleCheck->setEnabled(!m_busy && supported);
    m_idleMax->setEnabled(!m_busy && supported && (m_idleBackup->isChecked() || m_idleCheck->isChecked()));
    m_idleSave->setEnabled(available && idleConfiguration() != m_idleSaved);
    QString text=m_demo && policy.isEmpty() ? "Preview only · Idle waiting is off; maximum delay defaults to 6 hours." : policy.value("message").toString("Waiting for idle-policy status…");
    if (!policy.value("error").toString().isEmpty()) text="Idle-wait status unavailable: " + policy.value("error").toString();
    if (idleConfiguration() != m_idleSaved) text += "\nUnsaved idle-wait options. Automatic jobs still use the saved options.";
    m_idleStatus->setText(text);
}
