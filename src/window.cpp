// Native Qt tray/window structure follows DriveWatch (MIT; see LICENSE).
#include "window.h"
#include "exclusions.h"
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
        QWidget#workspace QLabel:disabled, QWidget#workspace QCheckBox:disabled { color: %2; }
        QFrame#sidebar { background: palette(window); border-right: 1px solid palette(mid); }
        QFrame#sidebar QLabel { color: palette(window-text); }
        QLabel[role="eyebrow"] { color: palette(text); font-size: 10pt; font-weight: 600; }
        QListWidget#navigation { color: palette(window-text); background: transparent; border: none; outline: none; }
        QListWidget#navigation::item { padding: 12px 14px; margin: 3px 0; border-radius: 6px; }
        QListWidget#navigation::item:selected { background: palette(highlight); color: palette(highlighted-text); font-weight: 600; }
        QListWidget#navigation::item:hover:!selected { background: palette(alternate-base); color: palette(text); }
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
        QPushButton:disabled { color: %1; }
        QPushButton[primary="true"] { background: palette(highlight); color: palette(highlighted-text); border: 1px solid palette(highlight); font-weight: 600; }
        QPushButton[primary="true"]:disabled { background: palette(button); color: %1; border-color: palette(mid); }
        QLineEdit, QComboBox, QSpinBox { min-height: 26px; padding: 3px 6px; }
        QToolButton { padding: 8px; }
        QHeaderView::section { padding: 10px 8px; border: none; background: palette(window); color: palette(window-text); }
        QTableView, QTreeView, QPlainTextEdit { border: 1px solid palette(mid); border-radius: 5px; }
        QTableView::item, QTreeView::item { padding: 7px; }
        QWidget#editBar { background: palette(window); border-radius: 6px; }
        QWidget#editBar QLabel { color: palette(window-text); }
    )").arg(qApp->palette().color(QPalette::Disabled, QPalette::ButtonText).name(),
             qApp->palette().color(QPalette::Disabled, QPalette::Text).name());
}

QIcon statusIcon(const QString &state) {
    QPixmap pix(64, 64); pix.fill(Qt::transparent);
    QPainter p(&pix); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(QColor("#4679ad")); p.drawRoundedRect(7, 8, 46, 49, 7, 7);
    p.setPen(QPen(Qt::white, 4)); p.drawLine(17, 21, 43, 21); p.drawLine(17, 30, 35, 30);
    p.setPen(QPen(qApp->palette().color(QPalette::Window), 3));
    p.setBrush(state == "running" ? QColor("#5ea9ec") : state == "ok" ? QColor("#54b992") : state == "error" ? QColor("#ef716c") : QColor("#efb14f"));
    p.drawEllipse(35, 35, 25, 25);
    p.setPen(QPen(QColor("#172731"), 3));
    if (state == "ok") {p.drawLine(41, 47, 46, 52); p.drawLine(46, 52, 54, 42);}
    else if (state == "running") {p.drawLine(48, 40, 48, 48); p.drawLine(48, 48, 53, 48);}
    else {p.drawLine(48, 40, 48, 49); p.drawPoint(48, 54);}
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
    connect(this,&Window::attentionRequested,this,[this](const QString &title,const QString &message,bool critical) {
        if (!m_demo) m_tray.showMessage("M45 Drive Backuper — " + title,message,critical ? QSystemTrayIcon::Critical : QSystemTrayIcon::Warning,10000);
    });
    setWindowTitle("M45 Drive Backuper"); resize(1120, 840); setMinimumSize(940, 700); setWindowIcon(statusIcon("ok"));
    setStyleSheet(workspaceStyleSheet());
    qApp->installEventFilter(this);
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
    layout->addWidget(label("Snapshots stored on your backup drive. Reading this list requests administrator authentication."));
    m_archiveRefresh = new QPushButton("Refresh backup list…"); layout->addWidget(m_archiveRefresh, 0, Qt::AlignLeft);
    m_archives = new QTableWidget(0,3); m_archives->setObjectName("archiveTable");
    m_archives->setHorizontalHeaderLabels({"Created", "Snapshot", "Status"}); m_archives->verticalHeader()->hide();
    m_archives->setEditTriggers(QAbstractItemView::NoEditTriggers); m_archives->setSelectionBehavior(QAbstractItemView::SelectRows);
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
    connect(m_startup,&QCheckBox::toggled,this,&Window::setStartup);
    connect(m_notifications,&QCheckBox::toggled,this,[this](bool on) {if (!m_demo) m_settings.setValue("notifications",on);});
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
    m_miniNext = m_menu->addAction("Waiting for service status"); m_miniNext->setEnabled(false); m_menu->addSeparator();
    m_menu->addAction("Open M45 Drive Backuper",this,&Window::Show);
    m_runAction = m_menu->addAction("Back up now…",this,[this] {action("start");});
    m_mountAction = m_menu->addAction("Mount backup drives…",this,[this] {action("mount");});
    m_menu->addAction("Backup plan…",this,[this] {selectPage(1); Show();});
    m_menu->addAction("Choose what to skip…",this,[this] {selectPage(2); Show();});
    m_menu->addAction("View activity",this,[this] {selectPage(4); Show();});
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
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        // QSS keeps resolved palette brushes. Re-polish from the new desktop
        // palette so children do not retain light colors after a theme change.
        setStyleSheet({});
        setPalette(qApp->palette());
        setStyleSheet(workspaceStyleSheet());
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
            m_summary->setText(process->errorString()); m_tray.setIcon(statusIcon("warning"));
            m_tray.setToolTip("M45 Drive Backuper — monitoring unavailable"); m_miniState->setText("Monitoring unavailable"); m_miniNext->setText("Status readings are unavailable");
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
            m_tray.setIcon(statusIcon("warning")); m_tray.setToolTip("M45 Drive Backuper — monitoring unavailable");
            m_miniState->setText("Monitoring unavailable"); m_miniNext->setText("Last readings may be out of date");
            alert("monitoring","Backup monitoring unavailable",m_summary->text());
            m_snapshot = {}; setBusy(false);
        } else {m_snapshot = doc.object(); if (!m_loaded) loadSettings(); else refreshDrives(); render();}
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
    QString title, explanation, state = "warning";
    int statusPage = 1;
    if (running) {title = "Backup is running"; explanation = "Follow the Activity tab for Borg output. Closing the monitor does not stop this backup."; state = "running"; statusPage = 4;}
    else if (!mounted) {
        title = "Backup destination needs attention";
        const auto mounts = m_snapshot.value("actual_mounts").toArray();
        explanation = mounts.isEmpty() ? "The expected backup filesystem is not mounted. Connect the drive before the next run." : "The backup drive is mounted at " + mounts[0].toString() + ", but the job checks a different path. Choose the detected location in Backup plan to correct it.";
        if (c.value("auto_mount").toBool()) explanation += " Automatic mounting will be attempted when the saved backup job starts.";
    } else if (!m_snapshot.value("source_ready").toBool(true)) {title = "Backup source is unavailable"; explanation = "Connect the source drive and check the selected folder. Automatic mounting can use existing filesystem entries when enabled.";}
    else if (failed) {title = "Last backup attempt failed"; explanation = "Review the Borg errors and job output in Activity before retrying."; state = "error"; statusPage = 4;}
    else if (reportedWarning) {title = "Backup completed with warnings"; explanation = "Borg reported problems; some files may have been skipped or changed during the backup. Review the warnings in Activity."; statusPage = 4;}
    else if (outcome == "skipped") {title = "Backup attempt was skipped"; explanation = "This attempt did not create a backup. Another job may have held the backup lock. Check Activity and the saved backups."; statusPage = 4;}
    else if (m_snapshot.contains("free") && m_snapshot.value("free").toDouble() <= c.value("reserve_gib").toInt(10)*1073741824.0) {title = "Backup drive is low on space"; explanation = "Available space is below the configured free-space threshold. Cleanup must reclaim enough space before a new archive can be written.";}
    else if (!enabled) {title = "Automatic backups are disabled"; explanation = "Enable the schedule to resume automatic backups.";}
    else if (s.value("ExecMainExitTimestamp").toString().isEmpty()) {title = "Waiting for the first backup result"; explanation = "The schedule is active. Read the archive catalog to inspect stored backups.";}
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
    resolve->setText(showActivity ? "Review activity →" : "Review backup plan →");
    resolve->setVisible(state != "ok");
    m_destination->setText(c.value("repository").toString(c.value("mountpoint").toString())); m_destination->setToolTip("Filesystem UUID: " + c.value("uuid").toString());
    m_contents->setText(c.value("source").toString("/home/dist") + (c.value("include_system").toBool() ? " + system settings and local software" : "") + (c.value("package_recovery").toBool() ? " + software recovery list" : "") + " · Nested filesystems are skipped");
    m_storage->setText(QString("%1 · %2 · Leave %3 GiB free before starting").arg(c.value("scan_mode").toString() == "full_scan" ? "Full file scan" : "Incremental snapshots",c.value("compression").toString("zstd,3")).arg(c.value("reserve_gib").toInt(10)));
    m_schedule->setText(enabled ? stamp(t.value("NextElapseUSecRealtime").toString()) : "Schedule disabled");
    m_attempt->setText(stamp(s.value("ExecMainExitTimestamp").toString()) + " · " + (running ? "Running" : reportedWarning ? "Completed with warnings" : failed ? "Failed" : outcome == "skipped" ? "Skipped" : stamp(s.value("Result").toString())));
    m_space->setText(mounted ? QString("%1 GiB available / %2 GiB total").arg(m_snapshot.value("free").toDouble()/1073741824.0,0,'f',1).arg(m_snapshot.value("total").toDouble()/1073741824.0,0,'f',1) : "Unavailable at configured mount path");
    renderLog();
    m_miniState->setText(title); m_miniNext->setText(enabled ? "Next: " + stamp(t.value("NextElapseUSecRealtime").toString()) : "Schedule disabled");
    m_tray.setIcon(statusIcon(state)); m_tray.setToolTip("M45 Drive Backuper — " + title + "\n" + explanation);
    if (state == "warning" || state == "error") alert(title + s.value("InvocationID").toString() + s.value("ExecMainExitTimestamp").toString(),title,explanation,state == "error");
    else {m_lastNotification.clear(); if (!m_demo) m_settings.remove("lastNotification");}
    setBusy(m_busy);
}

void Window::setBusy(bool busy) {
    m_busy = busy;
    const bool available = !busy && !m_demo && !m_snapshot.isEmpty();
    const auto active = m_snapshot.value("service").toObject().value("ActiveState").toString();
    const bool running = active == "active" || active == "activating" || active == "deactivating";
    const bool autoMount = m_snapshot.value("config").toObject().value("auto_mount").toBool();
    m_run->setEnabled(available && !running && (m_snapshot.value("mounted").toBool() || autoMount));
    m_mountDrives->setEnabled(available && !running && autoMount); m_mountAction->setEnabled(m_mountDrives->isEnabled());
    m_mountDrives->setToolTip(autoMount ? "Mount the configured source and destination using existing filesystem entries." : "Enable and save automatic mounting in Backup plan first.");
    m_runAction->setEnabled(m_run->isEnabled()); m_archiveRefresh->setEnabled(available && !running && m_snapshot.value("mounted").toBool());
    const bool scheduled = m_snapshot.value("timer").toObject().value("ActiveState") == "active";
    m_enable->setEnabled(available && !scheduled); m_disable->setEnabled(available && scheduled);
    m_run->setToolTip(running ? "A backup is already running." : autoMount ? "Start the saved backup plan, attempting to mount configured drives first." : !m_snapshot.value("mounted").toBool() ? "Connect the backup drive or review its location in Backup plan." : "Start the saved backup plan.");
    m_planPage->setEnabled(m_loaded && !busy); m_exclusionsPage->setEnabled(m_loaded && !busy);
    updateEditState();
    m_startup->setEnabled(!m_demo); m_notifications->setEnabled(!m_demo);
    m_exportRecovery->setEnabled(available); m_restorePackages->setEnabled(available);
    m_recoveryToggle->setEnabled(m_loaded && !busy);
    updateRecoveryStatus();
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
        } else {m_feedback->setText(result.value("message").toString()); if (name == "apply" || name == "upgrade-reporting") m_loaded = false;}
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
    const QByteArray text = QByteArray("[Desktop Entry]\nType=Application\nName=M45 Drive Backuper\nExec=/usr/bin/m45-drive-backuper\nIcon=m45-drive-backuper\nTerminal=false\nHidden=") + (enabled ? "false\n" : "true\n");
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
    const bool running = active == "active" || active == "activating" || active == "deactivating";
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
    }
    m_archives->setSortingEnabled(true); m_archives->sortItems(0,Qt::DescendingOrder);
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
