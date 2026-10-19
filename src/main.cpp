#include <QApplication>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include "app/Application.h"
#include "ui/SetupDialog.h"
#include "ui/UnlockDialog.h"
namespace {
bool run(tb::Application& core, const std::function<bool(void*, QString*)>& operation, QString* err) {
    QProgressDialog progress(QStringLiteral("Обработка хранилища…"), QStringLiteral("Отменить"), 0, 0);
    progress.setWindowTitle(QStringLiteral("TokenBucket"));
    progress.setWindowModality(Qt::ApplicationModal); progress.setMinimumDuration(0);
    progress.setAutoClose(false); progress.setAutoReset(false);
    QObject::connect(&progress, &QProgressDialog::canceled, &core, &tb::Application::lock);
    progress.show();
    return operation(reinterpret_cast<void*>(progress.winId()), err);
}
int start() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString error;
    auto instanceLock = tb::Application::acquireInstanceLock(dir, &error);
    if (!instanceLock) { QMessageBox::critical(nullptr, QStringLiteral("TokenBucket"), error); return 1; }
    tb::Application core(dir);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &core, &tb::Application::lock);
    for (;;) {
        if (core.gate() == tb::Application::Gate::NeedSetup) {
            SetupDialog dialog(false);
#ifdef Q_OS_WIN
            QFutureWatcher<tb::platform::hello::Availability> availability;
            QObject::connect(&availability, &QFutureWatcher<tb::platform::hello::Availability>::finished, &dialog, [&] {
                dialog.setHelloAvailable(availability.result() == tb::platform::hello::Availability::Available);
            });
            availability.setFuture(QtConcurrent::run(tb::platform::hello::availability));
#endif
            if (dialog.exec() != QDialog::Accepted) return 0;
            tb::SecureBytes password(dialog.masterPassword()); dialog.clearSecrets();
            const bool ok = run(core, [&](void* hwnd, QString* err) {
#ifdef Q_OS_WIN
                if (dialog.helloChosen()) return core.createHelloVault(err, hwnd);
#endif
                return core.createPasswordVault(password.bytes(), err);
            }, &error);
            password.clear();
            if (!ok) { QMessageBox::warning(nullptr, QStringLiteral("TokenBucket"), error); continue; }
        } else if (core.gate() == tb::Application::Gate::NeedUnlock) {
#ifndef Q_OS_WIN
            if (core.unlockMode() == tb::Application::Mode::Hello) {
                QMessageBox::critical(nullptr, QStringLiteral("TokenBucket"), QStringLiteral("Это хранилище использует Windows Hello. Для Linux нужен защищённый экспорт из Windows."));
                return 1;
            }
#endif
            UnlockDialog dialog(core.unlockMode() == tb::Application::Mode::Hello ? UnlockDialog::Kind::Hello : UnlockDialog::Kind::Password);
            if (dialog.exec() != QDialog::Accepted) return 0;
            tb::SecureBytes password(dialog.password()); dialog.clearSecrets();
            const bool ok = run(core, [&](void* hwnd, QString* err) {
#ifdef Q_OS_WIN
                if (!dialog.passwordMode()) return core.unlockHello(err, hwnd);
#endif
                return core.unlock(password.bytes(), err);
            }, &error);
            password.clear();
            if (!ok) { QMessageBox::warning(nullptr, QStringLiteral("TokenBucket"), error); continue; }
        } else {
            QDialog window; window.setWindowTitle(QStringLiteral("TokenBucket"));
            auto* layout = new QVBoxLayout(&window);
            layout->addWidget(new QLabel(QStringLiteral("Хранилище разблокировано. Интерфейс записей — следующий этап."), &window));
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Close, &window);
            buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Заблокировать"));
            buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
            layout->addWidget(buttons);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &window, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &window, &QDialog::reject);
            const int result = window.exec(); core.lock();
            if (result != QDialog::Accepted) return 0;
        }
    }
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TokenBucket")); app.setOrganizationName(QStringLiteral("TokenBucket"));
    app.setQuitOnLastWindowClosed(false);
    try { return start(); }
    catch (...) { QMessageBox::critical(nullptr, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось запустить приложение.")); return 1; }
}
