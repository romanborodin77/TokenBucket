#include "RegistryView.h"
#include "EntryLabels.h"
#include "../app/Application.h"
#include <QAbstractTableModel>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QHBoxLayout>
namespace tb::ui {
class RegistryRowsModel:public QAbstractTableModel {
public:
    RegistryRowsModel(Application& app,QObject* parent):QAbstractTableModel(parent),app_(app) {}
    QList<model::Id> ids;
    void reset(QList<model::Id> next) {beginResetModel(); ids=std::move(next); endResetModel();}
    int rowCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:int(ids.size());}
    int columnCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:5;}
    QVariant headerData(int column,Qt::Orientation orientation,int role) const override {
        if(orientation!=Qt::Horizontal || role!=Qt::DisplayRole) return {};
        static const QStringList labels={QStringLiteral("Параметр / запись"),QStringLiteral("Значение"),QStringLiteral("Тип"),QStringLiteral("Ресурс"),QStringLiteral("Источник")}; return labels.value(column);
    }
    QVariant data(const QModelIndex& index,int role=Qt::DisplayRole) const override {
        if(app_.gate()!=Application::Gate::Ready || !index.isValid() || index.row()>=ids.size()) return {};
        const auto* e=app_.model()->entry(ids[index.row()]); if(!e) return {};
        if(role==Qt::UserRole) return e->id;
        if(role!=Qt::DisplayRole && role!=Qt::ToolTipRole && role!=Qt::AccessibleTextRole) return {};
        if(index.column()==0) return e->parameterName.isEmpty() || e->parameterName==e->title?e->title:e->parameterName+QStringLiteral(" · ")+e->title;
        if(index.column()==1) {
            if(e->type==model::EntryType::PlainValue && !e->reverify) return e->plainValue.isEmpty()?QStringLiteral("Пусто"):e->plainValue.left(96);
            if(e->type==model::EntryType::TextFile) return QStringLiteral("Документ: %1").arg(e->textFile.fileName);
            return QStringLiteral("••••••••");
        }
        if(index.column()==2) return entryTypeLabel(e->type)+(e->reverify?QStringLiteral(" · проверка"):QString());
        if(index.column()==3) {
            QStringList path; auto id=e->groupId;
            for(int depth=0;!id.isNull() && depth<model::VaultModel::MaxDepth;++depth) {const auto it=app_.model()->nodes().constFind(id); if(it==app_.model()->nodes().cend()) break; path.prepend(it->name); id=it->parent;}
            return path.join(" / ");
        }
        if(index.column()==4 && !e->sourceEntryId.isNull()) {
            const auto* source=app_.model()->entry(e->sourceEntryId);
            return source?QStringLiteral("%1 · строка %2").arg(source->textFile.fileName).arg(e->sourceLine+1):QStringLiteral("Источник недоступен");
        }
        return {};
    }
private:Application& app_;
};
RegistryView::RegistryView(Application& app,QWidget* parent):QWidget(parent),app_(app) {
    setObjectName("resourceRegistry"); auto* layout=new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0);
    rows_=new RegistryRowsModel(app,this); tree_=new QTreeView(this); tree_->setObjectName("registryEntries"); tree_->setAccessibleName(QStringLiteral("Реестр ресурсов")); tree_->setModel(rows_);
    tree_->setRootIsDecorated(false); tree_->setSelectionMode(QAbstractItemView::SingleSelection); tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers); tree_->setDragEnabled(false); tree_->setContextMenuPolicy(Qt::NoContextMenu); tree_->setUniformRowHeights(true);
    tree_->header()->setSectionResizeMode(0,QHeaderView::Stretch); tree_->setColumnWidth(1,180); tree_->setColumnWidth(2,120); layout->addWidget(tree_,1);
    auto* actions=new QHBoxLayout; copy_=new QPushButton(QStringLiteral("Копировать значение"),this); copy_->setObjectName("registryCopy"); source_=new QPushButton(QStringLiteral("Источник…"),this); source_->setObjectName("registrySource");
    actions->addWidget(copy_); actions->addWidget(source_); layout->addLayout(actions);
    connect(tree_,&QTreeView::clicked,this,[this] {if(!current().isNull()) emit entryActivated(current());});
    connect(tree_,&QTreeView::activated,this,[this] {if(!current().isNull()) emit entryActivated(current());});
    connect(copy_,&QPushButton::clicked,this,[this] {if(copy_->isEnabled()) emit copyRequested(current());});
    connect(source_,&QPushButton::clicked,this,[this] {const auto* e=app_.model()->entry(current()); if(source_->isEnabled() && e) emit sourceRequested(e->sourceEntryId);});
    connect(tree_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this] {updateActions();});
    connect(&app_,&Application::stateChanged,this,&RegistryView::refresh); connect(&app_,&Application::busyChanged,this,&RegistryView::updateActions);
    tree_->installEventFilter(this); tree_->viewport()->installEventFilter(this); refresh();
}
model::Id RegistryView::current() const {return tree_->currentIndex().data(Qt::UserRole).value<model::Id>();}
void RegistryView::updateActions() {
    const auto* e=app_.gate()==Application::Gate::Ready && !app_.busy()?app_.model()->entry(current()):nullptr;
    copy_->setEnabled(e && e->type!=model::EntryType::TextFile); source_->setEnabled(e && !e->sourceEntryId.isNull() && app_.model()->entry(e->sourceEntryId));
}
void RegistryView::setGroup(model::Id group) {group_=group; limited_=false; refresh();}
void RegistryView::setEntries(const QList<model::Id>& ids) {limited_=true; rows_->reset(app_.gate()==Application::Gate::Ready?ids:QList<model::Id>{}); updateActions();}
void RegistryView::refresh() {
    if(app_.gate()!=Application::Gate::Ready) rows_->reset({});
    else if(limited_) rows_->reset(rows_->ids);
    else rows_->reset(group_.isNull()?app_.model()->entries().keys():app_.model()->entriesInSubtree(group_));
    updateActions();
}
bool RegistryView::eventFilter(QObject* object,QEvent* event) {
    if((object==tree_ || object==tree_->viewport()) && (event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress)) {
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Cut)) {
            if(event->type()==QEvent::KeyPress && key->matches(QKeySequence::Copy) && copy_->isEnabled()) emit copyRequested(current());
            event->accept(); return true;
        }
    }
    return QWidget::eventFilter(object,event);
}
}
