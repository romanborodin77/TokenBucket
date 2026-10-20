#include "SetupDialog.h"
#include "../app/Application.h"
#include "../vault/SecureBytes.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
// Создание нового хранилища: выбор способа разблокировки (пароль или Windows Hello)
// и ввод мастер-пароля с подтверждением. Текст предупреждения Hello-режима — часть
// согласия пользователя с границами DPAPI (ключ не переносим, процесс от того же
// пользователя может обойти проверку — спека §4). Пароли живут в QLineEdit ровно
// до accept: main сразу забирает их в SecureBytes и вызывает clearSecrets().
SetupDialog::SetupDialog(bool helloAvailable, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Создание хранилища TokenBucket"));
    auto* layout = new QVBoxLayout(this);
    pw_ = new QRadioButton(QStringLiteral("&Мастер-пароль"), this);
    hello_ = new QRadioButton(QStringLiteral("Windows &Hello"), this);
    hello_->setObjectName(QStringLiteral("helloChoice"));
    setHelloAvailable(helloAvailable);
    pw_->setChecked(true);
    layout->addWidget(pw_); layout->addWidget(hello_);
#ifndef Q_OS_WIN
    hello_->hide();
#endif
    auto* notice = new QLabel(QStringLiteral("Ключ защищён учётной записью Windows и не переносится на другой компьютер. "
        "Hello добавляет проверку владельца; программа, запущенная от вашей учётной записи, потенциально может её обойти. "
        "Для восстановления понадобится заранее сделанный защищённый экспорт."), this);
    notice->setWordWrap(true); notice->hide(); layout->addWidget(notice);
    auto* form = new QFormLayout;
    pw1_ = new QLineEdit(this); pw2_ = new QLineEdit(this);
    pw1_->setObjectName(QStringLiteral("masterPassword")); pw2_->setObjectName(QStringLiteral("confirmPassword"));
    for (auto* field : {pw1_, pw2_}) {
        field->setEchoMode(QLineEdit::Password); field->setMaxLength(1024);
        field->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    }
    pw1_->setAccessibleName(QStringLiteral("Мастер-пароль")); pw2_->setAccessibleName(QStringLiteral("Повтор пароля"));
    form->addRow(QStringLiteral("&Пароль (12–1024 байта UTF-8):"), pw1_);
    form->addRow(QStringLiteral("По&вторите пароль:"), pw2_);
    layout->addLayout(form);
    error_ = new QLabel(this); error_->setWordWrap(true); error_->setObjectName(QStringLiteral("validationError"));
    layout->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Создать"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &SetupDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SetupDialog::reject);
    connect(hello_, &QRadioButton::toggled, this, [this, notice](bool checked) {
        pw1_->setEnabled(!checked); pw2_->setEnabled(!checked); notice->setVisible(checked);
        error_->clear(); if (checked) clearSecrets();
    });
    pw1_->setFocus();
}
SetupDialog::~SetupDialog() { clearSecrets(); }
void SetupDialog::setHelloAvailable(bool available) {
    hello_->setEnabled(available);
    hello_->setToolTip(available ? QString() : QStringLiteral("Windows Hello недоступен или не настроен."));
}
QByteArray SetupDialog::masterPassword() const { return pw1_->text().toUtf8(); }
bool SetupDialog::helloChosen() const { return hello_->isChecked(); }
void SetupDialog::clearSecrets() { pw1_->clear(); pw2_->clear(); }
void SetupDialog::accept() {
    // Валидация до закрытия: пароль обязан пройти требования спеки и совпасть
    // с подтверждением; ошибки показываются в самом диалоге, а не вторым окном.
    if (helloChosen() && hello_->isEnabled()) { QDialog::accept(); return; }
    tb::SecureBytes password(masterPassword()), confirmation(pw2_->text().toUtf8());
    if (!tb::Application::validPassword(password.bytes())) {
        error_->setText(QStringLiteral("Пароль: минимум 12 и максимум 1024 байта UTF-8.")); pw1_->setFocus(); return;
    }
    if (password.bytes() != confirmation.bytes()) {
        error_->setText(QStringLiteral("Пароли не совпадают.")); pw2_->setFocus(); return;
    }
    QDialog::accept();
}
