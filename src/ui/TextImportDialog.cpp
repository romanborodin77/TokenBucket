#include "TextImportDialog.h"
#include "TextLinesModel.h"
#include "EntryLabels.h"
#include "VerifyDialog.h"
#include "../app/Application.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QTableView>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
namespace tb::ui {
namespace {
QString conflictLabel(io::ConflictAction action) {
    return action==io::ConflictAction::Skip?QStringLiteral("Пропустить"):action==io::ConflictAction::Replace?QStringLiteral("Заменить"):QStringLiteral("Новая версия");
}
}
class ImportRowsModel:public QAbstractTableModel {
public:
    ImportRowsModel(io::TextImportPreview& p,std::function<bool()> valid,QObject* parent):QAbstractTableModel(parent),preview(p),valid_(std::move(valid)) {}
    QList<io::RegistrySelection> choices; QSet<int> enabled; int revealed=-1;
    io::TextImportPreview& preview;
    void reset() {beginResetModel(); revealed=-1; endResetModel();}
    void changed(int row) {if(row>=0 && row<rowCount()) emit dataChanged(index(row,0),index(row,5));}
    void hide() {const int old=revealed; revealed=-1; changed(old);}
    int rowCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:int(choices.size());}
    int columnCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:6;}
    Qt::ItemFlags flags(const QModelIndex& index) const override {return index.isValid()?Qt::ItemIsEnabled|Qt::ItemIsSelectable|(index.column()==0?Qt::ItemIsUserCheckable:Qt::NoItemFlags):Qt::NoItemFlags;}
    bool setData(const QModelIndex& index,const QVariant& value,int role) override {
        if(!valid_() || role!=Qt::CheckStateRole || index.column()!=0 || index.row()<0 || index.row()>=rowCount()) return false;
        if(value.toInt()==Qt::Checked) enabled.insert(index.row()); else enabled.remove(index.row()); changed(index.row()); return true;
    }
    QVariant headerData(int section,Qt::Orientation orientation,int role) const override {
        if(orientation!=Qt::Horizontal || role!=Qt::DisplayRole) return {};
        static const QStringList headings={QStringLiteral("Импорт"),QStringLiteral("Строка"),QStringLiteral("Название"),QStringLiteral("Значение"),QStringLiteral("Тип"),QStringLiteral("Конфликт")}; return headings.value(section);
    }
    QVariant data(const QModelIndex& index,int role=Qt::DisplayRole) const override {
        if(!valid_() || !index.isValid() || index.row()>=rowCount()) return {};
        if(role==Qt::CheckStateRole && index.column()==0) return enabled.contains(index.row())?Qt::Checked:Qt::Unchecked;
        if(role!=Qt::DisplayRole && role!=Qt::ToolTipRole && role!=Qt::AccessibleTextRole) return {};
        const auto& c=choices[index.row()]; const auto& p=preview.parameters[c.parameter];
        switch(index.column()) {
        case 0:return {};
        case 1:return p.line+1;
        case 2:return c.title;
        case 3:return index.row()==revealed || (c.type==model::EntryType::PlainValue && !preview.source.reverify)
            ?p.value.isEmpty()?QStringLiteral("Пусто"):p.value.left(120):QStringLiteral("••••••••");
        case 4:return entryTypeLabel(c.type);
        case 5:return conflictLabel(c.conflict);
        default:return {};
        }
    }
private:
    std::function<bool()> valid_;
};
TextImportDialog::TextImportDialog(Application& app,io::TextImportPreview preview,Apply apply,QWidget* parent)
    :QDialog(parent),app_(app),generation_(app.generation()),baseline_(*app.model()),preview_(std::move(preview)),apply_(std::move(apply)) {
    setObjectName("textImportPreview"); setWindowTitle(QStringLiteral("TXT — предпросмотр импорта")); resize(900,620);
    auto* layout=new QVBoxLayout(this); summary_=new QLabel(this); summary_->setTextFormat(Qt::PlainText); summary_->setWordWrap(true); layout->addWidget(summary_);
    tabs_=new QTabWidget(this); tabs_->setObjectName("textImportTabs"); layout->addWidget(tabs_,1);
    rows_=new ImportRowsModel(preview_,[this] {return valid();},this); rows_->choices=io::defaultRegistrySelections(baseline_,preview_);
    for(int i=0;i<rows_->choices.size();++i) rows_->enabled.insert(i);
    lines_=new TextLinesModel([this]() -> const model::Entry* {return valid()?&preview_.source:nullptr;},this);
    const auto table=[&](QAbstractItemModel* model,const char* name) {
        auto* view=new QTableView(this); view->setObjectName(name); view->setModel(model); view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->setSelectionBehavior(QAbstractItemView::SelectRows); view->setEditTriggers(QAbstractItemView::NoEditTriggers); view->verticalHeader()->hide();
        view->setDragEnabled(false); view->setContextMenuPolicy(Qt::NoContextMenu); view->setWordWrap(false); view->installEventFilter(this); view->viewport()->installEventFilter(this); return view;
    };
    table_=table(rows_,"textImportParameters"); source_=table(lines_,"textImportSource");
    table_->setAccessibleName(QStringLiteral("Предложения параметров")); source_->setAccessibleName(QStringLiteral("Исходные строки TXT"));
    table_->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch); table_->setColumnWidth(0,65); table_->setColumnWidth(1,60); table_->setColumnWidth(3,170);
    source_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch); source_->setColumnWidth(0,65); source_->setColumnWidth(2,90);
    tabs_->addTab(table_,QStringLiteral("Параметры")); tabs_->addTab(source_,QStringLiteral("Исходные строки"));
    auto* fields=new QWidget(this); auto* form=new QFormLayout(fields); form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    type_=new QComboBox(fields); type_->setObjectName("textImportType"); type_->addItem(entryTypeLabel(model::EntryType::ApiToken),int(model::EntryType::ApiToken)); type_->addItem(entryTypeLabel(model::EntryType::PlainValue),int(model::EntryType::PlainValue));
    title_=new QLineEdit(fields); title_->setObjectName("textImportTitle"); name_=new QLineEdit(fields); name_->setObjectName("textImportName");
    path_=new QLineEdit(fields); path_->setObjectName("textImportGroupPath"); path_->setPlaceholderText(QStringLiteral("Группы внутри выбранного ресурса: через /"));
    conflict_=new QComboBox(fields); conflict_->setObjectName("textImportConflict");
    for(auto c:{io::ConflictAction::Skip,io::ConflictAction::CreateVersion,io::ConflictAction::Replace}) conflict_->addItem(conflictLabel(c),int(c));
    form->addRow(QStringLiteral("Тип:"),type_); form->addRow(QStringLiteral("Название:"),title_); form->addRow(QStringLiteral("Имя параметра:"),name_); form->addRow(QStringLiteral("Группы:"),path_); form->addRow(QStringLiteral("При конфликте:"),conflict_);
    auto* scroll=new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setWidget(fields); scroll->setMaximumHeight(200); layout->addWidget(scroll);
    auto* reveal=new QPushButton(QStringLiteral("Показать выбранное на время…"),this); reveal->setObjectName("textImportReveal"); layout->addWidget(reveal);
    error_=new QLabel(preview_.error,this); error_->setObjectName("textImportError"); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this); buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Проверить и импортировать")); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,this,&TextImportDialog::accept); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(reveal,&QPushButton::clicked,this,&TextImportDialog::revealSelected);
    connect(table_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this] {hideValue(); selectRow();});
    connect(source_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this] {hideValue();});
    connect(rows_,&QAbstractItemModel::dataChanged,this,[this] {updateSummary();});
    connect(tabs_,&QTabWidget::currentChanged,this,[this] {hideValue();});
    connect(type_,&QComboBox::currentIndexChanged,this,[this] {editChoice();}); connect(conflict_,&QComboBox::currentIndexChanged,this,[this] {editChoice();});
    for(auto* edit:{title_,name_,path_}) connect(edit,&QLineEdit::editingFinished,this,&TextImportDialog::editChoice);
    timer_.setParent(this); timer_.setSingleShot(true); timer_.setInterval(15000); timer_.setObjectName("textImportHideTimer"); connect(&timer_,&QTimer::timeout,this,&TextImportDialog::hideValue);
    connect(&app_,&Application::stateChanged,this,[this] {if(generation_!=app_.generation() || app_.gate()!=Application::Gate::Ready) reject();});
    qApp->installEventFilter(this);
    if(rows_->rowCount()) table_->setCurrentIndex(rows_->index(0,2)); selectRow();
}
bool TextImportDialog::valid() const {return !closed_ && app_.gate()==Application::Gate::Ready && generation_==app_.generation() && baseline_==*app_.model();}
void TextImportDialog::setRevealSeconds(int seconds) {hideValue(); timer_.setInterval((seconds==5 || seconds==10 || seconds==15 || seconds==30?seconds:15)*1000);}
void TextImportDialog::hideValue() {timer_.stop(); if(rows_) rows_->hide(); if(lines_) lines_->reveal(-1);}
void TextImportDialog::done(int result) {
    closed_=true; for(auto* dialog:findChildren<QDialog*>()) dialog->reject(); hideValue();
    baseline_.clear(); preview_={}; rows_->choices.clear(); rows_->enabled.clear(); rows_->reset(); lines_->reset();
    for(auto* field:{title_,name_,path_}) field->clear(); QDialog::done(result);
}
bool TextImportDialog::confirm(const QString& text) {
    QMessageBox box(QMessageBox::Warning,QStringLiteral("TXT — классификация"),text,QMessageBox::Yes|QMessageBox::No,this);
    box.setTextFormat(Qt::PlainText); box.setDefaultButton(QMessageBox::No); box.setEscapeButton(QMessageBox::No);
    return box.exec()==QMessageBox::Yes && valid() && !app_.busy();
}
void TextImportDialog::selectRow() {
    const QScopedValueRollback guard(editing_,true); const int row=table_->currentIndex().row(); const bool exists=valid() && row>=0 && row<rows_->choices.size();
    for(QWidget* field:QList<QWidget*>{type_,title_,name_,path_,conflict_}) field->setEnabled(exists);
    if(exists) {
        const auto& c=rows_->choices[row]; type_->setCurrentIndex(type_->findData(int(c.type))); title_->setText(c.title); name_->setText(c.parameterName);
        name_->setEnabled(c.type==model::EntryType::PlainValue); path_->setText(c.groupPath.join('/')); conflict_->setCurrentIndex(conflict_->findData(int(c.conflict)));
    }
    updateSummary();
}
void TextImportDialog::updateSummary() {
    int skip=0,conflicts=0,matches=0;
    for(int i=0;i<rows_->choices.size();++i) {const auto& c=rows_->choices[i]; skip+=!rows_->enabled.contains(i) || c.conflict==io::ConflictAction::Skip; conflicts+=c.matchingEntries>0 && !c.exactMatch; matches+=c.exactMatch;}
    QStringList group; auto id=preview_.source.groupId;
    for(int depth=0;!id.isNull() && depth<model::VaultModel::MaxDepth;++depth) {const auto it=baseline_.nodes().constFind(id); if(it==baseline_.nodes().cend()) break; group.prepend(it->name); id=it->parent;}
    summary_->setText(QStringLiteral("%1 · ресурс: %2\nСтрок: %3 · параметров: %4 · совпадений: %5 · конфликтов: %6 · пропусков: %7\nСнимок сохраняется целиком. Неизвестные строки остаются скрытыми.")
        .arg(preview_.source.textFile.fileName,group.join(" / ")).arg(lines_->rowCount()).arg(rows_->rowCount()).arg(matches).arg(conflicts).arg(skip));
}
void TextImportDialog::editChoice() {
    if(editing_ || !valid() || app_.busy()) return;
    const int row=table_->currentIndex().row(); if(row<0 || row>=rows_->choices.size()) return;
    auto c=rows_->choices[row]; const auto type=model::EntryType(type_->currentData().toInt());
    if(type!=c.type && type==model::EntryType::PlainValue && !confirm(QStringLiteral("Значение и исходная строка станут видимыми после разблокировки и доступными поиску. Продолжить?"))) {selectRow(); return;}
    if(!valid()) return;
    c.type=type; c.title=title_->text(); c.parameterName=type==model::EntryType::PlainValue?name_->text():QString();
    c.groupPath=path_->text().isEmpty()?QStringList():path_->text().split('/',Qt::KeepEmptyParts);
    c.conflict=io::ConflictAction(conflict_->currentData().toInt()); c.replaceId={};
    const auto ids=io::registryMatches(baseline_,preview_,c); c.matchingEntries=int(ids.size());
    c.exactMatch=false;
    for(const auto id:ids) {const auto* e=baseline_.entry(id); if((e->type==model::EntryType::PlainValue?e->plainValue:e->token)==preview_.parameters[c.parameter].value) {c.exactMatch=true; break;}}
    if(c.conflict==io::ConflictAction::Replace && ids.size()==1) c.replaceId=ids.first();
    rows_->choices[row]=std::move(c); rows_->changed(row); selectRow();
}
void TextImportDialog::revealSelected() {
    if(!valid() || app_.busy()) return;
    const bool source=tabs_->currentIndex()==1; const int row=(source?source_:table_)->currentIndex().row();
    if(row<0 || !VerifyDialog::verify(app_,this,QStringLiteral("просмотр строки TXT")) || !valid() || row!=(source?source_:table_)->currentIndex().row()) return;
    hideValue(); if(source) lines_->reveal(row); else {rows_->revealed=row; rows_->changed(row);} timer_.start();
}
bool TextImportDialog::eventFilter(QObject* object,QEvent* event) {
    if(((table_ && (object==table_ || object==table_->viewport())) || (source_ && (object==source_ || object==source_->viewport())))
        && (event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress)) {
        auto* key=static_cast<QKeyEvent*>(event); if(key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Cut)) {event->accept(); return true;}
    }
    if(event->type()==QEvent::ApplicationDeactivate || (object==this && (event->type()==QEvent::WindowDeactivate || event->type()==QEvent::Hide))) hideValue();
    return QDialog::eventFilter(object,event);
}
void TextImportDialog::accept() {
    if(!valid() || app_.busy()) {error_->setText(QStringLiteral("Сеанс или данные изменились. Откройте предпросмотр заново.")); return;}
    editChoice(); if(!valid()) return;
    QList<io::RegistrySelection> choices; auto preview=preview_; int publicValues=0;
    for(int i=0;i<rows_->choices.size();++i) if(rows_->enabled.contains(i)) {
        const auto& c=rows_->choices[i]; choices.append(c); const int line=preview.parameters[c.parameter].line;
        if(c.type==model::EntryType::PlainValue) {++publicValues; if(!preview.source.textFile.visibleLines.contains(line)) preview.source.textFile.visibleLines.append(line);}
        else preview.source.textFile.visibleLines.removeAll(line);
    }
    if(publicValues && !confirm(QStringLiteral("Простых значений: %1. После импорта их значения и выбранные исходные строки будут видимы и доступны поиску, кроме записей с повторной проверкой. Подтверждаете классификацию?").arg(publicValues))) return;
    if(!valid() || app_.busy()) return;
    std::sort(preview.source.textFile.visibleLines.begin(),preview.source.textFile.visibleLines.end());
    QString error; model::VaultModel candidate;
    if(!io::buildRegistryCandidate(baseline_,preview,choices,&candidate,&error)) {error_->setText(error); return;}
    if(!apply_ || !apply_(preview,choices,&error)) {if(!closed_) error_->setText(error.isEmpty()?QStringLiteral("Импорт не сохранён."):error); return;}
    if(!closed_ && generation_==app_.generation() && app_.gate()==Application::Gate::Ready) QDialog::accept();
}
}
