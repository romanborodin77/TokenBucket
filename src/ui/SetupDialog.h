#pragma once
#include <QDialog>
class QRadioButton;
class QLineEdit;
class QLabel;
// Первый запуск: выбор способа разблокировки нового хранилища (см. SetupDialog.cpp).
// Пароль отдаётся как UTF-8 QByteArray — вызыватель оборачивает в SecureBytes и чистит.
class SetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit SetupDialog(bool helloAvailable, QWidget* parent = nullptr);
    ~SetupDialog() override;
    QByteArray masterPassword() const;
    bool helloChosen() const;
    void clearSecrets();
    void setHelloAvailable(bool available);
protected:
    void accept() override;
private:
    QRadioButton *pw_{}, *hello_{};
    QLineEdit *pw1_{}, *pw2_{};
    QLabel* error_{};
};
