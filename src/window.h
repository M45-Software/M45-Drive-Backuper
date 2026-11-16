#pragma once
#include <QMainWindow>
#include <QJsonObject>
#include <QSystemTrayIcon>
#include <QSettings>

class QTimer;
class QDateTime;
class QLabel;
class QTabWidget;
class QStackedWidget;
class QPlainTextEdit;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QAction;
class QMenu;
class QTableWidget;
class ExclusionEditor;

class Window : public QMainWindow {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.m45drivebackuper")
public:
    explicit Window(bool demo = false);
    void poll();
    void selectPage(int page);
public slots:
    void Show();
signals:
    void reminderRequested(const QString &title, const QString &message, int page);
    void attentionRequested(const QString &title, const QString &message, bool critical);
protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    friend class WindowTests;
    bool m_demo, m_polling = false, m_busy = false, m_loaded = false, m_loading = true;
    QJsonObject m_snapshot;
    QJsonObject m_savedPlan;
    QString m_revision, m_folderSource, m_folderRepository;
    QSettings m_settings;
    QString m_lastNotification;
    int m_notificationPage = 0;
    QComboBox *m_backupReminder, *m_checkReminder;
    QCheckBox *m_idleBackup, *m_idleCheck;
    QComboBox *m_idleMax;
    QLabel *m_idleStatus;
    QPushButton *m_idleSave;
    bool m_idleLoaded = false;
    QJsonObject m_idleSaved;
    QString m_idleRevision;
    void renderIdlePolicy();
    QJsonObject idleConfiguration() const;
    QLabel *m_reminderStatus;
    void updateReminders(const QDateTime &now);
    QString reminderKey(const QString &kind) const;
    QTimer *m_trayAnimation;
    QString m_trayState;
    int m_trayFrame = 0;
    QSystemTrayIcon m_tray;
    QMenu *m_menu;
    QMenu *m_backupControls, *m_checkControls;
    QPushButton *m_pauseBackup, *m_cancelBackup, *m_pauseCheck;
    QLabel *m_backupControlStatus, *m_checkControlStatus;
    QMenu *createJobControls(const QString &kind);
    void updateJobControls();
    void jobControl(const QString &kind, const QString &operation, int delay = 0);
    QAction *m_miniState, *m_runAction;
    QTabWidget *m_tabs;
    QLabel *m_status, *m_summary, *m_destination, *m_schedule, *m_attempt, *m_space, *m_feedback, *m_contents, *m_storage;
    QPlainTextEdit *m_logs;
    QTableWidget *m_activity;
    QStackedWidget *m_logViews;
    QComboBox *m_logLevel;
    QLabel *m_logCount, *m_logAccess;
    QPlainTextEdit *m_borgMessages;
    QLabel *m_borgHeading;
    QPushButton *m_updateReporting;
    QTableWidget *m_archives;
    ExclusionEditor *m_excludes;
    QWidget *m_planPage, *m_exclusionsPage, *m_editBar;
    QLabel *m_changes, *m_archiveHint;
    QPushButton *m_discard;
    QLineEdit *m_logSearch;
    QCheckBox *m_followLog;
    QLineEdit *m_mount, *m_source, *m_repository;
    QComboBox *m_interval, *m_sourceKind, *m_sourceDrive, *m_destinationDrive, *m_destinationKind, *m_backupType, *m_compression, *m_quotaMode;
    QSpinBox *m_hourly, *m_daily, *m_weekly, *m_monthly, *m_reserve, *m_quota;
    QCheckBox *m_startup, *m_notifications, *m_skipCaches, *m_autoMount, *m_includeSystem, *m_packageRecovery;
    QCheckBox *m_recoveryToggle;
    QLabel *m_recoveryStatus;
    QPushButton *m_exportRecovery, *m_restorePackages;
    QPushButton *m_save, *m_run, *m_enable, *m_disable, *m_archiveRefresh, *m_mountDrives;
    void render();
    void loadSettings();
    void refreshDrives();
    void action(const QString &name, const QString &payload = {});
    void saveSettings();
    void setStartup(bool enabled);
    void setBusy(bool busy);
    QJsonObject configuration() const;
    void updateEditState();
    void updateRecoveryStatus();
    void renderLog();
    void renderVerification();
    bool m_verificationLoaded = false;
    QString m_verificationRevision, m_verificationSavedMode, m_verificationSavedScope;
    QComboBox *m_verificationMode, *m_verificationScope;
    QLabel *m_verificationStatus;
    QPushButton *m_checkNow, *m_saveVerification, *m_checkDetails, *m_stopCheck, *m_checkSelected, *m_browseBackup;
    void startVerification(const QString &scope, const QString &archiveId = {});
    void showArchives(const QJsonArray &archives);
    void openAnalyzer();
    void quitMonitor();
    void alert(const QString &key, const QString &title, const QString &message, bool critical = false);
    QString driveRepository() const;
};
