#include "UnlockDialog.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
// Экран разблокировки. Kind::Password — ввод мастер-пароля (IME-подсказки скрытого
// ввода, чтобы раскладки/облака не запоминали пароль); Kind::Hello — единственная
// кнопка «Проверить через Windows Hello» (сам запрос показывает платформенный слой
// по HWND родителя). Деструктор очищает поле: пароль не остаётся в памяти виджета.
UnlockDialog::UnlockDialog(Kind kind, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Разблокировка TokenBucket"));
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(kind == Kind::Password
        ? QStringLiteral("Введите мастер-пароль этого хранилища. Если пароль забыт, восстановить его невозможно — используйте заранее сделанный защищённый экспорт.")
        : QStringLiteral("Подтвердите вход через Windows Hello. Ключ хранилища защищён этой учётной записью Windows и не переносится на другие компьютеры."), this);
    note->setWordWrap(true); note->setTextFormat(Qt::PlainText);
    layout->addWidget(note);
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
