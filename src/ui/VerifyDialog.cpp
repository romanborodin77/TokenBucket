#include "VerifyDialog.h"
#include "../app/Application.h"
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
namespace tb::ui {
// Повторная проверка владельца для записей с флагом reverify (показ/копирование
// секрета, редактор, выдача). Путь Hello — verifyHello по HWND родителя; путь
// пароля — QInputDialog + Application::checkPassword. Ключевая защита от гонок —
// сверка generation ПОСЛЕ диалога и ПОСЛЕ проверки: пока вводился пароль,
// хранилище могли заблокировать (например, по таймеру) — тогда результат
// проверки выбрасывается. Текст password-поля затирается сразу после accept.
bool VerifyDialog::verify(Application& app, QWidget* parent, const QString& action) {
    if (app.gate() != Application::Gate::Ready || app.busy()) return false;
    const auto generation = app.generation(); QString error; bool verified = false;
    if (app.mode() == Application::Mode::Hello) {
#ifdef Q_OS_WIN
        verified = app.verifyHello(parent ? reinterpret_cast<void*>(parent->window()->winId()) : nullptr, &error);
#endif
    } else {
        QInputDialog dialog(parent);
        dialog.setWindowTitle(action.isEmpty() ? QStringLiteral("Повторная проверка") : QStringLiteral("Повторная проверка — %1").arg(action));
        dialog.setLabelText(QStringLiteral("Текущий мастер-пароль:")); dialog.setTextEchoMode(QLineEdit::Password);
        dialog.setOkButtonText(QStringLiteral("Подтвердить")); dialog.setCancelButtonText(QStringLiteral("Отмена"));
        QObject::connect(&app, &Application::stateChanged, &dialog, [&] { if (app.generation() != generation || app.gate() != Application::Gate::Ready) dialog.reject(); });
        const bool accepted = dialog.exec() == QDialog::Accepted;
        SecureBytes password(dialog.textValue().toUtf8()); dialog.setTextValue({});
        if (!accepted || generation != app.generation()) return false;
        verified = app.checkPassword(password.bytes(), &error); password.clear();
    }
    if (generation != app.generation() || app.gate() != Application::Gate::Ready) return false;
    if (!verified && !error.isEmpty()) QMessageBox::warning(parent, QStringLiteral("TokenBucket"), error);
    return verified;
}
}
