#pragma once
#include <QDialog>
class QLineEdit;
class UnlockDialog : public QDialog {
    Q_OBJECT
public:
    enum class Kind { Password, Hello };
    explicit UnlockDialog(Kind kind, QWidget* parent = nullptr);
    ~UnlockDialog() override;
    QByteArray password() const;
    bool passwordMode() const { return pw_ != nullptr; }
    void clearSecrets();
private:
    QLineEdit* pw_{};
};
