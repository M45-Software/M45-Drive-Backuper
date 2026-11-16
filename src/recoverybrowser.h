#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QByteArray>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QProcess;
class QTimer;

class RecoveryBrowser : public QDialog {
    Q_OBJECT
public:
    RecoveryBrowser(const QJsonObject &selection, const QString &archiveName, bool demo, QWidget *parent = nullptr);
protected:
    void reject() override;
private:
    friend class WindowTests;
    bool m_demo, m_busy = true, m_ready = false, m_closing = false;
    QString m_path, m_parent, m_recovered, m_failure;
    int m_offset = 0, m_total = 0;
    QByteArray m_output, m_errors;
    QProcess *m_process = nullptr;
    QLabel *m_status, *m_location, *m_count;
    QLineEdit *m_search, *m_destination;
    QTableWidget *m_entries;
    QPushButton *m_up, *m_root, *m_previous, *m_next, *m_recover, *m_choose, *m_open, *m_close;
    QTimer *m_searchDelay;
    void setBusy(bool busy);
    void request(const QJsonObject &command);
    void browse(const QString &path, int offset = 0, bool clearSearch = true);
    void acceptEvent(const QJsonObject &event);
    void applyListing(const QJsonObject &listing);
    void recoverSelected();
};
