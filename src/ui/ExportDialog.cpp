#include "ExportDialog.h"
#include "../app/Application.h"
#include "../vault/SecureBytes.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
namespace tb::ui {
ExportDialog::ExportDialog(QWidget* parent, const QString& title, const QString& note, const QString& prompt)
    : QDialog(parent) {
    setWindowTitle(title.isEmpty() ? QStringLiteral("TokenBucket — пароль") : title);
    auto* layout = new QVBoxLayout(this);
    if (!note.isEmpty()) {
        auto* explanation = new QLabel(note, this);
        explanation->setWordWrap(true); explanation->setTextFormat(Qt::PlainText);
        layout->addWidget(explanation);
    }
    auto* form = new QFormLayout;
    pw1_ = new QLineEdit(this); pw2_ = new QLineEdit(this);
    pw1_->setObjectName(QStringLiteral("exportPassword")); pw2_->setObjectName(QStringLiteral("confirmExportPassword"));
    for (auto* field : {pw1_, pw2_}) {
        field->setEchoMode(QLineEdit::Password); field->setMaxLength(1024);
        field->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    }
    pw1_->setAccessibleName(prompt.isEmpty() ? QStringLiteral("Пароль") : prompt);
    pw2_->setAccessibleName(QStringLiteral("Повтор пароля"));
    form->addRow((prompt.isEmpty() ? QStringLiteral("&Пароль (12–1024 байта UTF-8):") : prompt), pw1_);
    form->addRow(QStringLiteral("По&вторите пароль:"), pw2_);
    layout->addLayout(form);
    error_ = new QLabel(this); error_->setWordWrap(true); error_->setObjectName(QStringLiteral("validationError"));
    layout->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Продолжить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ExportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ExportDialog::reject);
    pw1_->setFocus();
}
ExportDialog::~ExportDialog() { clearSecrets(); }
QByteArray ExportDialog::password() const { return pw1_->text().toUtf8(); }
void ExportDialog::clearSecrets() { pw1_->clear(); pw2_->clear(); }
void ExportDialog::accept() {
    SecureBytes secret(password()), confirmation(pw2_->text().toUtf8());
    if (!Application::validPassword(secret.bytes())) {
        error_->setText(QStringLiteral("Пароль: минимум 12 и максимум 1024 байта UTF-8.")); pw1_->setFocus(); return;
    }
    if (secret.bytes() != confirmation.bytes()) {
        error_->setText(QStringLiteral("Пароли не совпадают.")); pw2_->setFocus(); return;
    }
    QDialog::accept();
}
}
