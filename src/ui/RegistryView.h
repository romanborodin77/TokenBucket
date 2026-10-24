#pragma once
#include <QWidget>
#include "../model/Types.h"
class QTreeView; class QPushButton;
namespace tb {class Application;}
namespace tb::ui {
class RegistryRowsModel;
class RegistryView:public QWidget {
    Q_OBJECT
public:
    RegistryView(Application&,QWidget* parent=nullptr);
    void setGroup(model::Id);
    void setEntries(const QList<model::Id>&);
    void refresh();
signals:
    void entryActivated(model::Id);
    void copyRequested(model::Id);
    void sourceRequested(model::Id);
protected:
    bool eventFilter(QObject*,QEvent*) override;
private:
    model::Id current() const;
    void updateActions();
    Application& app_; model::Id group_; bool limited_=false;
    RegistryRowsModel* rows_{}; QTreeView* tree_{}; QPushButton *copy_{},*source_{};
};
}
