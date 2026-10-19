#pragma once
#include <QDialog>
class QRadioButton;
class QLineEdit;
class QLabel;
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
