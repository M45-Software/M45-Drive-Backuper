#pragma once
#include <QWidget>

class QPlainTextEdit;
class QTreeWidget;
class QLabel;
class QLineEdit;

// The original text remains authoritative so comments and advanced rules survive.
class ExclusionEditor : public QWidget {
    Q_OBJECT
public:
    explicit ExclusionEditor(bool preview = false, QWidget *parent = nullptr);
    QString text() const;
    void setText(const QString &text);
    void setSource(const QString &source);
    bool addPath(const QString &path, bool folder);
    void removeSelected();
    QString summary() const;
signals:
    void changed();
private:
    QString m_source;
    bool m_preview;
    QPlainTextEdit *m_raw;
    QTreeWidget *m_list;
    QLabel *m_summary, *m_sourceLabel;
    QLineEdit *m_filter;
    void refresh();
};
