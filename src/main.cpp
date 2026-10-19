#include <QApplication>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include "app/Application.h"
#include "app/IdleTimer.h"
#include "platform/SessionWatcher.h"
#include "ui/SetupDialog.h"
#include "ui/UnlockDialog.h"
#include "ui/MainWindow.h"
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
    tb::IdleTimer idle(5 * 60 * 1000); idle.setActive(false);
    tb::platform::SessionWatcher session;
    if (!session.error().isEmpty()) { QMessageBox::critical(nullptr, QStringLiteral("TokenBucket"), session.error()); return 1; }
    QObject::connect(&idle, &tb::IdleTimer::idleTimeout, &core, &tb::Application::lock);
    QObject::connect(&session, &tb::platform::SessionWatcher::lockNow, &core, &tb::Application::lock);
    QObject::connect(&core, &tb::Application::stateChanged, &idle, [&] { idle.setActive(core.gate() == tb::Application::Gate::Ready); });
    bool quitting = false;
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &core, [&] { quitting = true; core.lock(); });
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
            QEventLoop loop; tb::ui::MainWindow window(core); bool closed = false;
            QObject::connect(&window, &tb::ui::MainWindow::closing, &loop, [&] { closed = true; loop.quit(); });
            QObject::connect(&core, &tb::Application::stateChanged, &loop, [&] {
                if (core.gate() != tb::Application::Gate::Ready) loop.quit();
            });
            window.show(); loop.exec();
            if (closed || quitting) return 0;
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
