#include "UnlockDialog.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
UnlockDialog::UnlockDialog(Kind kind, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Разблокировка TokenBucket"));
    auto* layout = new QVBoxLayout(this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    if (kind == Kind::Password) {
        pw_ = new QLineEdit(this); pw_->setEchoMode(QLineEdit::Password); pw_->setMaxLength(1024);
        pw_->setObjectName(QStringLiteral("unlockPassword")); pw_->setAccessibleName(QStringLiteral("Мастер-пароль"));
        pw_->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
        auto* form = new QFormLayout; form->addRow(QStringLiteral("&Мастер-пароль:"), pw_); layout->addLayout(form);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Разблокировать"));
        connect(pw_, &QLineEdit::returnPressed, this, &QDialog::accept);
    } else buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Проверить через Windows Hello"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
UnlockDialog::~UnlockDialog() { clearSecrets(); }
QByteArray UnlockDialog::password() const { return pw_ ? pw_->text().toUtf8() : QByteArray(); }
void UnlockDialog::clearSecrets() { if (pw_) pw_->clear(); }
