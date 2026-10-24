#pragma once
#include <QAbstractTableModel>
#include <QStringDecoder>
#include <functional>
#include "../model/TextFile.h"
namespace tb::ui {
class TextLinesModel : public QAbstractTableModel {
public:
    TextLinesModel(std::function<const model::Entry*()> current,QObject* parent):QAbstractTableModel(parent),current_(std::move(current)) {reset();}
    void reset() {
        beginResetModel(); ranges_.clear(); revealed_=-1;
        if(const auto* e=current_()) ranges_=model::textfile::lineRanges(e->textFile.bytes);
        endResetModel();
    }
    void reveal(int row) {revealed_=row; if(rowCount()) emit dataChanged(index(0,0),index(rowCount()-1,2));}
    QString text(int row) const {
        const auto* e=current_(); if(!e || row<0 || row>=ranges_.size()) return {};
        const auto r=ranges_[row];
        return QStringDecoder(QStringConverter::Utf8,QStringConverter::Flag::ConvertInitialBom)(QByteArrayView(e->textFile.bytes).sliced(r.offset,r.length));
    }
    int rowCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:int(ranges_.size());}
    int columnCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:3;}
    QVariant headerData(int section,Qt::Orientation orientation,int role) const override {
        if(orientation!=Qt::Horizontal || role!=Qt::DisplayRole) return {};
        return section==0?QStringLiteral("Строка"):section==1?QStringLiteral("Содержимое"):QStringLiteral("Видимость");
    }
    QVariant data(const QModelIndex& index,int role=Qt::DisplayRole) const override {
        const auto* e=current_(); if(!e || !index.isValid() || index.row()>=rowCount()) return {};
        if(role!=Qt::DisplayRole && role!=Qt::ToolTipRole && role!=Qt::AccessibleTextRole) return {};
        const bool visible=model::textfile::isVisible(e->textFile,index.row());
        if(index.column()==0) return index.row()+1;
        if(index.column()==2) return visible?QStringLiteral("Видимая"):QStringLiteral("Скрытая");
        return (!e->reverify && visible) || index.row()==revealed_?text(index.row()):QStringLiteral("••••••••");
    }
private:
    std::function<const model::Entry*()> current_;
    QList<model::textfile::LineRange> ranges_; int revealed_=-1;
};
}
