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
    bool m_demo, m_polling = false, m_busy = false, m_loaded = false;
    QJsonObject m_snapshot;
    QString m_revision;
    QSettings m_settings;
    QSystemTrayIcon m_tray;
    QMenu *m_menu;
    QAction *m_miniState, *m_miniNext, *m_runAction;
    QTabWidget *m_tabs;
    QLabel *m_status, *m_summary, *m_destination, *m_schedule, *m_attempt, *m_space, *m_feedback;
    QPlainTextEdit *m_logs, *m_archives, *m_excludes;
    QLineEdit *m_mount;
    QComboBox *m_interval;
    QSpinBox *m_hourly, *m_daily, *m_weekly, *m_monthly;
    QCheckBox *m_startup, *m_notifications;
    QPushButton *m_save, *m_run, *m_enable, *m_disable, *m_archiveRefresh;
    void render();
    void loadSettings();
    void action(const QString &name, const QString &payload = {});
    void saveSettings();
    void setStartup(bool enabled);
    void setBusy(bool busy);
};
