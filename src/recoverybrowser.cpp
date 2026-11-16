#include "recoverybrowser.h"
#include <QAbstractItemView>
#include <QDesktopServices>
#include <QDir>
#include <QDateTime>
#include <QLocale>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QLabel *textLabel(const QString &text = {}) {
    auto *label = new QLabel(text); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); return label;
}
}

RecoveryBrowser::RecoveryBrowser(const QJsonObject &selection, const QString &archiveName, bool demo, QWidget *parent)
    : QDialog(parent), m_demo(demo) {
    setWindowTitle("Recover files — " + archiveName); resize(1000,760); setMinimumSize(760,600);
    setWindowModality(Qt::WindowModal);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(textLabel("Browse saved folders, select files or folders, then recover them into a new folder. Existing files are kept. Recovery reads only the selected data and cannot overlap a backup or check."));
    auto *navigation = new QHBoxLayout;
    m_root = new QPushButton("Backup root"); m_up = new QPushButton("Up"); m_location = textLabel("Opening saved backup…"); m_location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    navigation->addWidget(m_root); navigation->addWidget(m_up); navigation->addWidget(m_location,1); layout->addLayout(navigation);
    m_search = new QLineEdit; m_search->setPlaceholderText("Filter names in this saved folder…"); m_search->setClearButtonEnabled(true); m_search->setObjectName("recoveryFilter"); layout->addWidget(m_search);
    m_entries = new QTableWidget(0,4); m_entries->setObjectName("recoveryEntries");
    m_entries->setHorizontalHeaderLabels({"Name","Type","Size","Modified"}); m_entries->verticalHeader()->hide();
    m_entries->setEditTriggers(QAbstractItemView::NoEditTriggers); m_entries->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_entries->setSelectionMode(QAbstractItemView::ExtendedSelection); m_entries->setAlternatingRowColors(true); m_entries->setWordWrap(false);
    m_entries->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    for(int column=1;column<4;++column) m_entries->horizontalHeader()->setSectionResizeMode(column,QHeaderView::ResizeToContents);
    layout->addWidget(m_entries,1);
    auto *paging = new QHBoxLayout; m_count=textLabel(); m_previous=new QPushButton("Previous page"); m_next=new QPushButton("Next page");
    paging->addWidget(m_count,1); paging->addWidget(m_previous); paging->addWidget(m_next); layout->addLayout(paging);
    m_destination=new QLineEdit(QDir::home().filePath("Recovered Files")); m_destination->setObjectName("recoveryDestination");
    m_choose=new QPushButton("Choose folder…"); auto *destination=new QHBoxLayout; destination->addWidget(m_destination,1); destination->addWidget(m_choose);
    auto *form=new QFormLayout; form->addRow("Recover into",destination); layout->addLayout(form);
    layout->addWidget(textLabel("Each recovery creates its own subfolder. Files belong to your user account; saved folder structure below the current folder is preserved. Unsafe links and special files may be skipped."));
    m_status=textLabel("Authenticating and reading snapshot metadata…"); m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(m_status);
    auto *buttons=new QHBoxLayout; m_recover=new QPushButton("Recover selected…"); m_recover->setObjectName("recoverSelected"); m_recover->setProperty("primary",true);
    m_open=new QPushButton("Open recovered folder"); m_close=new QPushButton("Close"); buttons->addWidget(m_recover); buttons->addWidget(m_open); buttons->addStretch(); buttons->addWidget(m_close); layout->addLayout(buttons);
    m_searchDelay=new QTimer(this); m_searchDelay->setSingleShot(true); m_searchDelay->setInterval(300);
    connect(m_search,&QLineEdit::textChanged,this,[this] {m_searchDelay->start();});
    connect(m_searchDelay,&QTimer::timeout,this,[this] {if (!m_busy && m_ready) browse(m_path,0,false);});
    connect(m_root,&QPushButton::clicked,this,[this] {browse({});});
    connect(m_up,&QPushButton::clicked,this,[this] {browse(m_parent);});
    connect(m_previous,&QPushButton::clicked,this,[this] {browse(m_path,qMax(0,m_offset-500),false);});
    connect(m_next,&QPushButton::clicked,this,[this] {browse(m_path,m_offset+500,false);});
    connect(m_entries,&QTableWidget::itemSelectionChanged,this,[this] {setBusy(m_busy);});
    connect(m_entries,&QTableWidget::cellDoubleClicked,this,[this](int row,int) {
        const auto *item=m_entries->item(row,0);
        if (item && item->data(Qt::UserRole+1)=="d") browse(item->data(Qt::UserRole).toString());
    });
    connect(m_choose,&QPushButton::clicked,this,[this] {
        const auto folder=QFileDialog::getExistingDirectory(this,"Choose a recovery destination",QFileInfo(m_destination->text()).isDir() ? m_destination->text() : QDir::homePath());
        if (!folder.isEmpty()) m_destination->setText(folder);
    });
    connect(m_recover,&QPushButton::clicked,this,&RecoveryBrowser::recoverSelected);
    connect(m_open,&QPushButton::clicked,this,[this] {QDesktopServices::openUrl(QUrl::fromLocalFile(m_recovered));});
    connect(m_close,&QPushButton::clicked,this,&RecoveryBrowser::reject);
    setBusy(true);
    if (m_demo) {
        QJsonArray items{QJsonObject{{"name","Documents"},{"kind","d"},{"token","RG9jdW1lbnRz"}},
                         QJsonObject{{"name","notes.txt"},{"kind","f"},{"token","bm90ZXMudHh0"},{"size",2048},{"mtime","2026-11-16T06:02:02+00:00"}}};
        m_ready=true; applyListing(QJsonObject{{"display_path","/home/example"},{"path","aG9tZS9leGFtcGxl"},{"parent","aG9tZQ=="},{"entries",items},{"total",2},{"offset",0}});
        m_status->setText("Preview · Saved contents are sample data. Recovery is disabled."); return;
    }
    m_process=new QProcess(this);
    connect(m_process,&QProcess::readyReadStandardError,this,[this] {m_errors=(m_errors+m_process->readAllStandardError()).right(16000);});
    connect(m_process,&QProcess::readyReadStandardOutput,this,[this] {
        m_output+=m_process->readAllStandardOutput();
        while(m_output.contains('\n')) {
            const int end=m_output.indexOf('\n'); const auto line=m_output.left(end); m_output.remove(0,end+1);
            const auto document=QJsonDocument::fromJson(line);
            if (document.isObject()) acceptEvent(document.object());
        }
    });
    connect(m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
        if (error==QProcess::FailedToStart) {m_ready=false; setBusy(false); m_status->setText("Could not start administrator authentication: "+m_process->errorString());}
    });
    connect(m_process,&QProcess::finished,this,[this](int code,QProcess::ExitStatus) {
        m_ready=false; setBusy(false);
        if (m_closing) {QDialog::reject(); return;}
        if(code) m_status->setText(m_failure.isEmpty() ? "The browser stopped or authentication was cancelled. "+QString::fromUtf8(m_errors).trimmed() : m_failure);
    });
    const auto payload=QString::fromLatin1(QJsonDocument(selection).toJson(QJsonDocument::Compact).toBase64());
    m_process->start("/usr/bin/pkexec",{"/usr/libexec/m45-drive-backuper/backend","recovery-session",payload});
}

void RecoveryBrowser::setBusy(bool busy) {
    m_busy=busy; const bool available=!busy && m_ready;
    m_entries->setEnabled(available); m_search->setEnabled(available); m_destination->setEnabled(available); m_choose->setEnabled(available);
    m_up->setEnabled(available && !m_path.isEmpty()); m_root->setEnabled(available && !m_path.isEmpty());
    m_previous->setEnabled(available && m_offset>0); m_next->setEnabled(available && m_offset+m_entries->rowCount()<m_total);
    m_recover->setEnabled(available && !m_demo && !m_entries->selectedItems().isEmpty());
    m_open->setEnabled(!busy && !m_recovered.isEmpty()); m_close->setEnabled(!busy);
}

void RecoveryBrowser::request(const QJsonObject &command) {
    if(m_demo || !m_process || m_process->state()!=QProcess::Running || m_busy) return;
    m_failure.clear(); setBusy(true); m_status->setText("Reading saved files…");
    m_process->write(QJsonDocument(command).toJson(QJsonDocument::Compact)+'\n');
}

void RecoveryBrowser::browse(const QString &path,int offset,bool clearSearch) {
    m_searchDelay->stop();
    if(clearSearch) {QSignalBlocker blocker(m_search); m_search->clear();}
    request(QJsonObject{{"command","browse"},{"path",path},{"offset",offset},{"search",m_search->text()}});
}

void RecoveryBrowser::applyListing(const QJsonObject &listing) {
    m_path=listing.value("path").toString(); m_parent=listing.value("parent").toString(); m_offset=listing.value("offset").toInt(); m_total=listing.value("total").toInt();
    m_location->setText(listing.value("display_path").toString());
    QSignalBlocker blocker(m_entries); const auto entries=listing.value("entries").toArray();
    m_entries->clearSelection(); m_entries->setRowCount(entries.size());
    for(int row=0;row<entries.size();++row) {
        const auto entry=entries[row].toObject(); const auto kind=entry.value("kind").toString();
        const auto modified=QDateTime::fromString(entry.value("mtime").toString(),Qt::ISODateWithMs).toLocalTime();
        const QStringList values{entry.value("name").toString(),kind=="d" ? "Folder" : kind=="f" ? "File" : kind=="l" ? "Link" : "Special file",
            kind=="f" ? QLocale().formattedDataSize(entry.value("size").toDouble()) : QString(),modified.isValid() ? modified.toString("yyyy-MM-dd HH:mm") : QString()};
        for(int col=0;col<4;++col) {
            auto *item=new QTableWidgetItem(values[col]);
            if(kind=="special") item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            m_entries->setItem(row,col,item);
        }
        m_entries->item(row,0)->setData(Qt::UserRole,entry.value("token").toString()); m_entries->item(row,0)->setData(Qt::UserRole+1,kind);
    }
    m_count->setText(entries.isEmpty() ? "No matching items." : QString("%1–%2 of %3 items").arg(m_offset+1).arg(m_offset+entries.size()).arg(m_total));
    setBusy(false);
}

void RecoveryBrowser::acceptEvent(const QJsonObject &event) {
    const auto type=event.value("event").toString();
    if(type=="ready") {setWindowTitle("Recover files — "+event.value("archive").toString()); m_ready=true; m_destination->setText(event.value("destination").toString()); applyListing(event); m_status->setText("Choose saved files or folders to recover.");}
    else if(type=="listing") {applyListing(event); m_status->setText("Choose saved files or folders to recover.");}
    else if(type=="progress") m_status->setText(event.value("message").toString());
    else if(type=="destination") {m_recovered=event.value("path").toString(); m_status->setText("Recovering into: "+m_recovered);}
    else if(type=="result") {
        m_recovered=event.value("path").toString(); setBusy(false);
        const auto message=event.value("message").toString()+"\n"+m_recovered; m_status->setText(message);
        QMessageBox result(event.value("ok").toBool() ? QMessageBox::Information : QMessageBox::Warning,"File recovery",message,QMessageBox::Ok,this);
        result.setTextFormat(Qt::PlainText); QStringList details;
        if(!event.value("error").toString().isEmpty()) details<<event.value("error").toString();
        if(!event.value("details").toString().isEmpty()) details<<event.value("details").toString();
        for(const auto warning:event.value("warnings").toArray()) details<<warning.toString();
        if(!details.isEmpty()) result.setDetailedText(details.join('\n'));
        result.exec();
    } else if(type=="error" || event.contains("error")) {m_failure=event.value("message").toString(event.value("error").toString()); setBusy(false); m_status->setText(m_failure);}
}

void RecoveryBrowser::recoverSelected() {
    if(m_demo || m_busy || !m_ready) return;
    m_searchDelay->stop();
    QSet<int> rows; for(auto *item:m_entries->selectedItems()) rows.insert(item->row());
    if(rows.isEmpty()) return;
    QJsonArray tokens; QStringList names;
    for(int row:rows) {tokens.append(m_entries->item(row,0)->data(Qt::UserRole).toString()); if(names.size()<8) names<<m_entries->item(row,0)->text();}
    const auto message=QString("Recover %1 selected item(s), including folder contents, into a new subfolder under:\n%2\n\n%3\n\nExisting files will not be overwritten. Recovered items belong to your user account. Unsafe links and special files may be skipped.").arg(rows.size()).arg(m_destination->text(),names.join('\n'));
    QMessageBox review(QMessageBox::Question,"Recover selected items?",message,QMessageBox::Yes|QMessageBox::Cancel,this); review.setTextFormat(Qt::PlainText); review.setDefaultButton(QMessageBox::Cancel);
    if(review.exec()!=QMessageBox::Yes) return;
    request(QJsonObject{{"command","recover"},{"path",m_path},{"items",tokens},{"destination",m_destination->text()}});
}

void RecoveryBrowser::reject() {
    if(m_busy) {m_status->setText("Wait for the current operation to finish before closing this browser."); return;}
    if(m_process && m_process->state()!=QProcess::NotRunning) {m_closing=true; setBusy(true); m_status->setText("Closing snapshot browser…"); m_process->closeWriteChannel(); return;}
    QDialog::reject();
}
