#pragma once
#include <QDialog>
#include <QTimer>
#include <functional>
#include "../io/TextImport.h"
class QTableView; class QTabWidget; class QComboBox; class QLineEdit; class QLabel;
namespace tb {class Application;}
namespace tb::ui {
class ImportRowsModel; class TextLinesModel;
class TextImportDialog:public QDialog {
    Q_OBJECT
public:
    using Apply=std::function<bool(const io::TextImportPreview&,const QList<io::RegistrySelection>&,QString*)>;
    TextImportDialog(Application&,io::TextImportPreview,Apply,QWidget* parent=nullptr);
    void setRevealSeconds(int);
    void accept() override;
protected:
    void done(int) override;
    bool eventFilter(QObject*,QEvent*) override;
private:
    bool valid() const;
    bool confirm(const QString&);
    void selectRow();
    void updateSummary();
    void editChoice();
    void hideValue();
    void revealSelected();
    Application& app_; quint64 generation_; model::VaultModel baseline_;
    io::TextImportPreview preview_; Apply apply_;
    ImportRowsModel* rows_{}; TextLinesModel* lines_{};
    QTableView *table_{},*source_{}; QTabWidget* tabs_{};
    QComboBox *type_{},*conflict_{}; QLineEdit *title_{},*name_{},*path_{};
    QLabel *error_{},*summary_{}; QTimer timer_; bool editing_=false,closed_=false;
};
}
