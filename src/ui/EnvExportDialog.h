#pragma once
#include <QDialog>
class QLineEdit;
class QRadioButton;
class QLabel;
namespace tb::ui {
class EnvExportDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Temporary, Permanent };
    explicit EnvExportDialog(QWidget* parent = nullptr);
    QString name() const;
    Mode mode() const;
    QString program() const;
    QString arguments() const;
protected:
    void accept() override;
private:
    QLineEdit *name_{}, *program_{}, *arguments_{};
    QRadioButton *temporary_{}, *permanent_{};
    QLabel* error_{};
};
}
