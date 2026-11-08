#include "exclusions.h"
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

ExclusionEditor::ExclusionEditor(bool preview, QWidget *parent) : QWidget(parent), m_preview(preview) {
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(10);
    auto *intro = new QLabel("Choose what to leave out of future backups. Your original files stay on disk."); intro->setWordWrap(true); layout->addWidget(intro);
    m_sourceLabel = new QLabel; m_sourceLabel->setTextFormat(Qt::PlainText); m_sourceLabel->setWordWrap(true); layout->addWidget(m_sourceLabel);
    m_summary = new QLabel; m_summary->setObjectName("exclusionSummary"); m_summary->setTextFormat(Qt::PlainText); m_summary->setWordWrap(true);
    auto font = m_summary->font(); font.setBold(true); m_summary->setFont(font); layout->addWidget(m_summary);
    auto *buttons = new QHBoxLayout;
    auto *folder = new QPushButton("Skip a folder…"); folder->setObjectName("skipFolder");
    auto *file = new QPushButton("Skip files…");
    auto *remove = new QPushButton("Include selected again"); remove->setObjectName("includeAgain"); remove->setEnabled(false);
    buttons->addWidget(folder); buttons->addWidget(file); buttons->addStretch(); buttons->addWidget(remove); layout->addLayout(buttons);
    m_list = new QTreeWidget; m_list->setObjectName("exclusionList"); m_list->setHeaderLabels({"Skipped item or rule", "Applies to"});
    m_list->setMinimumHeight(180);
    m_list->setRootIsDecorated(false); m_list->setAlternatingRowColors(true); m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->header()->setSectionResizeMode(0,QHeaderView::Stretch); m_list->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    m_filter = new QLineEdit; m_filter->setObjectName("exclusionFilter"); m_filter->setPlaceholderText("Search all exclusion rules…"); m_filter->setClearButtonEnabled(true); layout->addWidget(m_filter);
    m_matches = new QLabel; m_matches->setObjectName("exclusionMatches"); m_matches->setWordWrap(true); layout->addWidget(m_matches);
    layout->addWidget(m_list,1);
    auto *toggle = new QToolButton; toggle->setText("Advanced patterns and comments"); toggle->setCheckable(true); toggle->setArrowType(Qt::RightArrow); toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); layout->addWidget(toggle);
    auto *details = new QWidget; auto *detailLayout = new QVBoxLayout(details); detailLayout->setContentsMargins(0,0,0,0);
    auto *note = new QLabel("Including an item again removes its selected rule. Another matching rule may still skip it. Changes affect future backups; existing snapshots keep their contents. Comments below are preserved and do not count as exclusion rules."); note->setWordWrap(true); detailLayout->addWidget(note);
    m_raw = new QPlainTextEdit; m_raw->setObjectName("exclusionPatterns"); m_raw->setPlaceholderText("One Borg exclusion pattern per line"); m_raw->setMaximumHeight(150); detailLayout->addWidget(m_raw);
    layout->addWidget(details); details->hide();
    connect(toggle,&QToolButton::toggled,this,[details,toggle](bool open) {details->setVisible(open); toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);});
    connect(m_raw,&QPlainTextEdit::textChanged,this,[this] {refresh(); emit changed();});
    connect(m_filter,&QLineEdit::textChanged,this,&ExclusionEditor::refresh);
    connect(m_list,&QTreeWidget::itemSelectionChanged,this,[this,remove] {remove->setEnabled(!m_list->selectedItems().isEmpty());});
    connect(remove,&QPushButton::clicked,this,&ExclusionEditor::removeSelected);
    connect(folder,&QPushButton::clicked,this,[this] {
        auto options = QFileDialog::Options(QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (m_preview) options |= QFileDialog::ReadOnly;
        const auto path = QFileDialog::getExistingDirectory(this,"Choose a folder to skip",m_source,options);
        if (!path.isEmpty() && !addPath(path,true)) QMessageBox::information(this,"Choose a folder inside the source","Choose a folder inside your backup source. The whole source cannot be excluded here. Paths ending in whitespace need an advanced pattern.");
    });
    connect(file,&QPushButton::clicked,this,[this] {
        const auto paths = QFileDialog::getOpenFileNames(this,"Choose files to skip",m_source,QString(),nullptr,m_preview ? QFileDialog::ReadOnly : QFileDialog::Options());
        int invalid = 0; for (const auto &path : paths) if (!addPath(path,false)) ++invalid;
        if (invalid) QMessageBox::information(this,"Some files were not added","Only files inside the backup source can be selected. Paths ending in whitespace need an advanced pattern.");
    });
    refresh();
}
QString ExclusionEditor::text() const {return m_raw->toPlainText();}
void ExclusionEditor::setText(const QString &text) {m_raw->setPlainText(text);}
void ExclusionEditor::setSource(const QString &source) {m_source = QDir::cleanPath(source); m_sourceLabel->setText("Backup source: " + m_source); refresh();}
void ExclusionEditor::setAdditionalSources(const QStringList &sources) {m_additionalSources = sources; refresh();}
QString ExclusionEditor::summary() const {return m_summary->text();}
bool ExclusionEditor::addPath(const QString &input, bool folder) {
    const auto path = QDir::cleanPath(input);
    const auto prefix = m_source == "/" ? "/" : m_source + '/';
    bool inside = path.startsWith(prefix);
    for (const auto &extra : m_additionalSources) inside |= path == extra || path.startsWith(extra + '/');
    if (!QDir::isAbsolutePath(path) || !QDir::isAbsolutePath(m_source) || path == m_source || !inside || path != path.trimmed() || path.contains('\n') || path.contains('\r')) return false;
    const auto pattern = (folder ? "pp:" : "pf:") + path.mid(1);
    for (const auto &line : text().split('\n')) if (line.trimmed() == pattern) return true;
    QString next = text(); if (!next.isEmpty() && !next.endsWith('\n')) next += '\n';
    setText(next + pattern + '\n'); return true;
}
void ExclusionEditor::removeSelected() {
    QSet<int> lines;
    for (const auto *item : m_list->selectedItems()) lines.insert(item->data(0,Qt::UserRole).toInt());
    auto raw = text().split('\n');
    for (int i=raw.size()-1;i>=0;--i) if (lines.contains(i)) raw.removeAt(i);
    setText(raw.join('\n'));
}
void ExclusionEditor::refresh() {
    m_sourceLabel->setText("Source: " + m_source + (m_additionalSources.isEmpty() ? "" : " + system settings and local software"));
    m_sourceLabel->setToolTip("Included sources:\n" + m_source + "\n" + m_additionalSources.join('\n'));
    m_list->clear(); int paths = 0, rules = 0, outside = 0, visible = 0;
    const auto lines = text().split('\n');
    for (int i=0;i<lines.size();++i) {
        const auto rule = lines[i].trimmed(); if (rule.isEmpty() || rule.startsWith('#')) continue;
        QString title = rule, applies = "Pattern · can match many paths";
        if (rule.startsWith("pp:") || rule.startsWith("pf:")) {
            title = '/' + rule.mid(3); if (rule.mid(3).startsWith('/')) title = rule.mid(3);
            applies = rule.startsWith("pp:") ? "Folder and contents" : "This file"; ++paths;
            bool inside = m_source == "/" || title == m_source || title.startsWith(m_source + '/');
            for (const auto &extra : m_additionalSources) inside |= title == extra || title.startsWith(extra + '/');
            if (!m_source.isEmpty() && !inside) {applies += " · outside source"; ++outside;}
        } else {
            ++rules;
            if (rule.endsWith("/**/node_modules")) title = "Downloaded Node.js dependencies (node_modules)";
            else if (rule.endsWith("/**/__pycache__")) title = "Python bytecode caches (__pycache__)";
            else if (rule.endsWith("/**/.cache")) title = "Cache folders (.cache)";
        }
        auto *item = new QTreeWidgetItem(m_list,{title,applies}); item->setData(0,Qt::UserRole,i); item->setToolTip(0,rule); item->setToolTip(1,rule);
        item->setHidden(!m_filter->text().isEmpty() && !title.contains(m_filter->text(),Qt::CaseInsensitive) && !rule.contains(m_filter->text(),Qt::CaseInsensitive));
        if (!item->isHidden()) ++visible;
    }
    m_summary->setText(QString("%1 exclusion %2 · %3 file/folder%4 · %5 pattern%6%7")
        .arg(paths+rules).arg(paths+rules == 1 ? "rule" : "rules").arg(paths).arg(paths == 1 ? "" : "s")
        .arg(rules).arg(rules == 1 ? "" : "s").arg(outside ? QString(" · %1 paths outside the selected source").arg(outside) : QString()));
    m_matches->setText(m_filter->text().isEmpty()
        ? (paths+rules ? QString("All %1 rules are listed below. Folders include their contents; patterns can match many folders.").arg(paths+rules) : "No file or pattern rules. The marked-cache setting below is separate.")
        : QString("Showing %1 of %2 rules. Clear the search to see all exclusions.").arg(visible).arg(paths+rules));
}
