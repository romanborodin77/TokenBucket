#pragma once
#include <QDialog>
class QLineEdit;
// Экран разблокировки существующего хранилища (Kind задаёт способ из hint прошлого раза).
class UnlockDialog : public QDialog {
    Q_OBJECT
public:
    enum class Kind { Password, Hello };
    explicit UnlockDialog(Kind kind, QWidget* parent = nullptr);
    ~UnlockDialog() override;
    QByteArray password() const;
    bool passwordMode() const { return pw_ != nullptr; }
    void clearSecrets();
    void setPrompt(const QString& text,const QString& acceptText);
private:
    QLineEdit* pw_{};
};
