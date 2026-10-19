#include "VerifyDialog.h"
#include "../app/Application.h"
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
namespace tb::ui {
bool VerifyDialog::verify(Application& app, QWidget* parent) {
    if (app.gate() != Application::Gate::Ready || app.busy()) return false;
    const auto generation = app.generation(); QString error; bool verified = false;
    if (app.mode() == Application::Mode::Hello) {
#ifdef Q_OS_WIN
        verified = app.verifyHello(parent ? reinterpret_cast<void*>(parent->window()->winId()) : nullptr, &error);
#endif
    } else {
        QInputDialog dialog(parent); dialog.setWindowTitle(QStringLiteral("Повторная проверка"));
        dialog.setLabelText(QStringLiteral("Мастер-пароль:")); dialog.setTextEchoMode(QLineEdit::Password);
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
