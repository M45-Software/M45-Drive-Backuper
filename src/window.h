#pragma once
#include <QMainWindow>
#include <QJsonObject>
#include <QSystemTrayIcon>
#include <QSettings>

class QLabel;
class QTabWidget;
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
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    bool m_demo, m_polling = false, m_busy = false, m_loaded = false, m_loading = true;
    QJsonObject m_snapshot;
    QJsonObject m_savedPlan;
    QString m_revision, m_folderSource, m_folderRepository;
    QSettings m_settings;
    QSystemTrayIcon m_tray;
    QMenu *m_menu;
    QAction *m_miniState, *m_miniNext, *m_runAction, *m_mountAction;
    QTabWidget *m_tabs;
    QLabel *m_status, *m_summary, *m_destination, *m_schedule, *m_attempt, *m_space, *m_feedback, *m_contents, *m_storage;
    QPlainTextEdit *m_logs;
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
    void showArchives(const QJsonArray &archives);
    void openAnalyzer();
    void quitMonitor();
    void alert(const QString &key, const QString &title, const QString &message);
    QString driveRepository() const;
};
