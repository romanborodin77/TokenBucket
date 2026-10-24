#include "TextFileDialog.h"
#include "EntryAccess.h"
#include "TextLinesModel.h"
#include "../app/Application.h"
#include "../model/TextFile.h"
#include "../platform/ClipboardGuard.h"
#include "../platform/FileOps.h"
#include <QAbstractTableModel>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringDecoder>
#include <QTableView>
#include <QVBoxLayout>
#include <QGridLayout>
#include <algorithm>
namespace tb::ui {
TextFileDialog::TextFileDialog(Application& app,model::Id id,ClipboardGuard& clipboard,QWidget* parent)
    :QDialog(parent),app_(app),clipboard_(clipboard),id_(id),generation_(app.generation()) {
    setWindowTitle(QStringLiteral("Текстовый файл — строки")); resize(820,480);
    if(app.gate()==Application::Gate::Ready) if(const auto* e=app.model()->entry(id);e && e->type==model::EntryType::TextFile) snapshot_=*e;
    auto* layout=new QVBoxLayout(this);
    auto* heading=new QLabel(snapshot_?snapshot_->textFile.fileName:QStringLiteral("Документ недоступен"),this);
    heading->setTextFormat(Qt::PlainText); heading->setWordWrap(true); layout->addWidget(heading);
    table_=new QTableView(this); table_->setObjectName("textFileLines"); table_->setAccessibleName(QStringLiteral("Строки документа"));
    lines_=new TextLinesModel([this]() -> const model::Entry* {return valid()?&*snapshot_:nullptr;},this);
    table_->setModel(lines_); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->setDragEnabled(false); table_->setContextMenuPolicy(Qt::NoContextMenu);
    table_->installEventFilter(this); table_->viewport()->installEventFilter(this);
    table_->verticalHeader()->hide(); table_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    table_->setColumnWidth(0,65); table_->setColumnWidth(2,90); table_->setWordWrap(false); layout->addWidget(table_,1);
    auto* actions=new QGridLayout;
    const auto button=[&](const QString& caption,const char* name,int row,int col,auto action) {
        auto* b=new QPushButton(caption,this); b->setObjectName(name); actions->addWidget(b,row,col); connect(b,&QPushButton::clicked,this,action); return b;
    };
    button(QStringLiteral("Показать строку"),"revealTextLine",0,0,[this] {
        const int row=table_->currentIndex().row(); if(!authorize(row)) return;
        lines_->reveal(row); timer_.start();
    });
    button(QStringLiteral("Копировать строку"),"copyTextLine",0,1,[this] {copyLine();});
    button(QStringLiteral("Изменить видимость…"),"textLineVisibility",1,0,[this] {changeVisibility();});
    button(QStringLiteral("Разобрать в реестр…"),"textFileRegistry",1,1,[this] {if(valid()) emit requestRegistryImport(id_);});
    button(QStringLiteral("Копировать весь файл…"),"copyTextFile",2,0,[this] {copyFile();});
    button(QStringLiteral("Сохранить файл…"),"exportTextFile",2,1,[this] {exportFile();});
    layout->addLayout(actions);
    error_=new QLabel(this); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    auto* close=new QDialogButtonBox(QDialogButtonBox::Close,this); connect(close,&QDialogButtonBox::rejected,this,&QDialog::reject); layout->addWidget(close);
    timer_.setParent(this); timer_.setSingleShot(true); timer_.setObjectName("textLineHideTimer"); timer_.setInterval(15000);
    connect(&timer_,&QTimer::timeout,this,&TextFileDialog::hideLine);
    connect(table_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this] {hideLine();});
    connect(&app_,&Application::stateChanged,this,[this] {if(!valid()) {clipboard_.cancelPendingCopy(); clipboard_.clearIfOurs(); reject();}});
    qApp->installEventFilter(this);
}
bool TextFileDialog::valid() const {
    if(!snapshot_ || generation_!=app_.generation() || app_.gate()!=Application::Gate::Ready) return false;
    const auto* current=app_.model()->entry(id_); return current && *current==*snapshot_;
}
void TextFileDialog::setRevealSeconds(int seconds) {
    hideLine(); timer_.setInterval((seconds==5 || seconds==10 || seconds==15 || seconds==30?seconds:15)*1000);
}
bool TextFileDialog::authorize(int row,bool force) {
    if(!valid() || app_.busy() || row<0 || row>=lines_->rowCount()) return false;
    const bool hidden=!model::textfile::isVisible(snapshot_->textFile,row);
    return authorizeEntry(app_,id_,this,force || hidden) && valid() && table_->currentIndex().row()==row;
}
void TextFileDialog::hideLine() {timer_.stop(); if(lines_) lines_->reveal(-1);}
void TextFileDialog::done(int result) {
    for(auto* dialog:findChildren<QDialog*>()) dialog->reject();
    hideLine(); snapshot_.reset(); if(lines_) lines_->reset();
    QDialog::done(result);
}
bool TextFileDialog::eventFilter(QObject* object,QEvent* event) {
    if((object==table_ || object==table_->viewport()) && (event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress)) {
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Cut)) {
            if(event->type()==QEvent::KeyPress && key->matches(QKeySequence::Copy)) copyLine();
            event->accept(); return true;
        }
    }
    if(event->type()==QEvent::ApplicationDeactivate || (object==this && (event->type()==QEvent::WindowDeactivate || event->type()==QEvent::Hide))) hideLine();
    if(event->type()==QEvent::FocusIn) if(auto* w=qobject_cast<QWidget*>(object); w && w!=this && !isAncestorOf(w)) hideLine();
    return QDialog::eventFilter(object,event);
}
bool TextFileDialog::confirm(const QString& text) {
    QMessageBox box(QMessageBox::Warning,QStringLiteral("TokenBucket"),text,QMessageBox::Yes|QMessageBox::No,this);
    box.setTextFormat(Qt::PlainText); box.setDefaultButton(QMessageBox::No); box.setEscapeButton(QMessageBox::No);
    return box.exec()==QMessageBox::Yes && valid() && !app_.busy();
}
void TextFileDialog::copyLine() {
    const int row=table_->currentIndex().row(); if(!authorize(row)) return;
    const auto text=lines_->text(row); if(text.isEmpty()) {error_->setText(QStringLiteral("Пустая строка не копируется.")); return;}
    QString error; if(!clipboard_.copySecret(text,&error)) error_->setText(error);
    if(!valid()) clipboard_.clearIfOurs();
}
bool TextFileDialog::confirmWholeFile() {
    return valid() && !app_.busy() && confirm(QStringLiteral("Весь файл, включая скрытые строки, будет выдан за пределы хранилища без шифрования. Продолжить?"))
        && authorizeEntry(app_,id_,this,true) && valid();
}
void TextFileDialog::copyFile() {
    if(!confirmWholeFile()) return;
    const QString text=QStringDecoder(QStringConverter::Utf8,QStringConverter::Flag::ConvertInitialBom)(snapshot_->textFile.bytes);
    if(text.isEmpty()) {error_->setText(QStringLiteral("Пустой файл не копируется.")); return;}
    QString error; if(!clipboard_.copySecret(text,&error)) error_->setText(error);
    if(!valid()) clipboard_.clearIfOurs();
}
void TextFileDialog::exportFile() {
    if(!confirmWholeFile()) return;
    const QString path=QFileDialog::getSaveFileName(this,QStringLiteral("Сохранить исходные байты"),snapshot_->textFile.fileName,QString(),nullptr,QFileDialog::DontConfirmOverwrite);
    if(path.isEmpty() || !valid() || app_.busy()) return;
    for(QFileInfo info(path);;) {
        if(info.isSymbolicLink() || info.isShortcut() || info.isJunction()) {error_->setText(QStringLiteral("Запись через ссылку запрещена.")); return;}
        const auto parent=info.absolutePath(); if(parent==info.absoluteFilePath()) break; info.setFile(parent);
    }
    auto& ops=store::realFileOps();
    if(QFileInfo::exists(path)) {
        if(!confirm(QStringLiteral("Файл существует. Заменить его точными байтами документа?"))) return;
        const QString temporary=path+QStringLiteral(".tb-new-")+QUuid::createUuid().toString(QUuid::WithoutBraces);
        if(!ops.writeAll(temporary,snapshot_->textFile.bytes)) {error_->setText(QStringLiteral("Не удалось создать закрытый временный файл.")); return;}
        if(!valid() || !ops.renameReplace(temporary,path)) {ops.removeFile(temporary); error_->setText(QStringLiteral("Файл не заменён.")); return;}
    } else if(!ops.writeAll(path,snapshot_->textFile.bytes)) {error_->setText(QStringLiteral("Не удалось создать файл. Проверьте путь и доступ.")); return;}
    error_->setText(QStringLiteral("Файл сохранён."));
}
void TextFileDialog::changeVisibility() {
    const int row=table_->currentIndex().row(); if(!authorize(row,true)) return;
    const bool visible=!model::textfile::isVisible(snapshot_->textFile,row);
    if(visible && !confirm(QStringLiteral("Строка станет видимой после разблокировки и доступной поиску. Продолжить?"))) return;
    if(!valid() || table_->currentIndex().row()!=row) return;
    emit requestVisibilityChange(id_,row,visible);
    if(!snapshot_ || generation_!=app_.generation() || app_.gate()!=Application::Gate::Ready) {reject(); return;}
    const auto* e=app_.model()->entry(id_); if(!e || e->type!=model::EntryType::TextFile) {reject(); return;}
    snapshot_=*e; lines_->reset(); table_->setCurrentIndex(lines_->index(row,1));
}
}
