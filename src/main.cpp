#include <QApplication>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include "app/Application.h"
#include "app/IdleTimer.h"
#include "platform/SessionWatcher.h"
#ifdef Q_OS_WIN
#include "platform/EnvOps.h"
#include "platform/CrashPolicy.h"
#endif
#include "ui/SetupDialog.h"
#include "ui/UnlockDialog.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Brand.h"
#include "Version.h"
#include "ui/TrayController.h"
namespace {
int runApp(); // тело главного цикла; start() добавляет очистку на выходе
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
    const int code = runApp();
#ifdef Q_OS_WIN
    // Единая точка выхода: по опции стираем установленные нами пользовательские
    // переменные окружения, чтобы секреты не оставались в реестре после программы.
    if (tb::platform::envops::clearOnExitEnabled()) tb::platform::envops::clearAllExported();
#endif
    return code;
}
int runApp() {
    bool startHidden = QCoreApplication::arguments().contains(QStringLiteral("--tray"));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString error;
    auto instanceLock = tb::Application::acquireInstanceLock(dir, &error);
    tb::Application core(dir);
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    // Интервал автоблокировки настраивается в окне; кламп 1–60 минут — тот же, что в диалоге,
    // испорченное значение настроек не должно отключать защиту или блокировать намертво.
    const int idleMinutes = qBound(1, settings.value(QStringLiteral("idleTimeoutMinutes"), 5).toInt(), 60);
    tb::IdleTimer idle(idleMinutes * 60 * 1000); idle.setActive(false);
    tb::platform::SessionWatcher session;
    if (!session.error().isEmpty()) { QMessageBox::critical(nullptr, QStringLiteral("TokenBucket"), session.error()); return 1; }
    tb::ui::TrayController tray(core); // после SessionWatcher: живёт всё приложение
    tb::ui::MainWindow* mainWindow = nullptr; // текущее окно цикла Ready для команд из трея
    // Показ из трея: поднять живое окно; диалоги модальны и видны сами по себе.
    QObject::connect(&tray, &tb::ui::TrayController::showRequested, &core, [&] {
        if (mainWindow) { mainWindow->show(); mainWindow->raise(); mainWindow->activateWindow(); }
    });
    tray.setPanelCallback([&] { if (mainWindow) mainWindow->popupQuickPanel(); });
    QObject::connect(&tray, &tb::ui::TrayController::lockRequested, &core, &tb::Application::lock);
    // Настоящий выход из трея: модальный диалог завершается reject'ом (exec() != Accepted
    // в циклах ниже — это return 0), живое окно закрывается через requestExit, а между
    // циклами (ни окна, ни диалога) остаётся только QCoreApplication::exit — app.exec() не используется.
    QObject::connect(&tray, &tb::ui::TrayController::quitRequested, &core, [&] {
        if (auto* modal = qobject_cast<QDialog*>(qApp->activeModalWidget())) modal->reject();
        else if (mainWindow) mainWindow->requestExit();
        else QCoreApplication::exit(0);
    });
    QObject::connect(&idle, &tb::IdleTimer::idleTimeout, &core, &tb::Application::lock);
    QObject::connect(&session, &tb::platform::SessionWatcher::lockNow, &core, &tb::Application::lock);
    QObject::connect(&core, &tb::Application::stateChanged, &idle, [&] { idle.setActive(core.gate() == tb::Application::Gate::Ready); });
    // Выход всегда идёт через closeEvent окна → Application::lock(); aboutToQuit здесь не
    // срабатывает, так как app.exec() не используется (событийный цикл ниже — QEventLoop).
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
            window.setIdleTimer(&idle);
            QObject::connect(&window, &tb::ui::MainWindow::notifyRequested, &tray, &tb::ui::TrayController::notify);
            mainWindow = &window;
            QObject::connect(&window, &tb::ui::MainWindow::closing, &loop, [&] { closed = true; loop.quit(); });
            // Настройка автоблокировки применяется немедленно: окно в области видимости,
            // перезапускаем отсчёт IdleTimer новым интервалом.
            QObject::connect(&window, &tb::ui::MainWindow::idleTimeoutChanged, &idle,
                [&](int m) { idle.setIntervalMs(m * 60 * 1000); });
            QObject::connect(&core, &tb::Application::stateChanged, &loop, [&] {
                if (core.gate() != tb::Application::Gate::Ready) loop.quit();
            });
            // --tray: автозапуск при входе в Windows стартует свёрнутым, окно не показываем.
            if (startHidden) startHidden = false; else { window.show(); }
            loop.exec();
            mainWindow = nullptr; // окно разрушается при выходе из области видимости
            if (closed) return 0;
        }
    }
}
}
int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    // Раньше всего: запретить аварийные дампы и системные диалоги ошибок до того,
    // как в памяти появятся секреты (снимок дампа содержит всю кучу процесса).
    tb::platform::crash::disableDumps();
#endif
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TokenBucket")); app.setOrganizationName(QStringLiteral("TokenBucket"));
    // Счётчик версий из CMake; иконка всех окон — логотип «Щит-жетон».
    app.setApplicationVersion(QString::fromLatin1(TOKENBUCKET_VERSION));
    app.setWindowIcon(QIcon(tb::ui::brand::logo(256)));
    app.setQuitOnLastWindowClosed(false);
    tb::ui::theme::apply(tb::ui::theme::load()); // до создания окон: палитра наследуется всеми окнами
    try { return start(); }
    catch (...) { QMessageBox::critical(nullptr, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось запустить приложение.")); return 1; }
}
