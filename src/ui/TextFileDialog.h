#pragma once
#include <QDialog>
#include <QTimer>
#include <optional>
#include "../model/Types.h"
class QTableView; class QLabel; class QPushButton;
namespace tb {class Application; class ClipboardGuard;}
namespace tb::ui {
class TextLinesModel;
class TextFileDialog : public QDialog {
    Q_OBJECT
public:
    TextFileDialog(Application&,model::Id,ClipboardGuard&,QWidget* parent=nullptr);
    void setRevealSeconds(int);
signals:
    void requestRegistryImport(model::Id);
    void requestVisibilityChange(model::Id,int line,bool visible);
protected:
    bool eventFilter(QObject*,QEvent*) override;
    void done(int) override;
private:
    bool valid() const;
    bool authorize(int row,bool force=false);
    bool confirmWholeFile();
    bool confirm(const QString&);
    void hideLine();
    void copyLine();
    void copyFile();
    void exportFile();
    void changeVisibility();
    Application& app_; ClipboardGuard& clipboard_;
    model::Id id_; quint64 generation_;
    std::optional<model::Entry> snapshot_;
    TextLinesModel* lines_{}; QTableView* table_{}; QLabel* error_{};
    QTimer timer_;
    bool changingVisibility_=false;
};
}
