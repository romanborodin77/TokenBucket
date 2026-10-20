#pragma once
#include <QDialog>
class QLineEdit;
class QLabel;
namespace tb::ui {
// Параметризуемый диалог «пароль + подтверждение» для экспорта/смены мастер-пароля.
class ExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExportDialog(QWidget* parent = nullptr, const QString& title = {},
                          const QString& note = {}, const QString& prompt = {});
    ~ExportDialog() override;
    QByteArray password() const;
    void clearSecrets();
protected:
    void accept() override;
private:
    QLineEdit *pw1_{}, *pw2_{};
    QLabel* error_{};
};
}
