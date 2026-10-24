#include "SessionFlow.h"
#include "../app/IdleTimer.h"
#include "../platform/SessionWatcher.h"
#include "SetupDialog.h"
#include "UnlockDialog.h"
#include "MainWindow.h"
#include "TrayController.h"
#include <QApplication>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
namespace tb::ui {
bool runVaultOperation(tb::Application& core, const std::function<bool(void*, QString*)>& operation, QString* err,QWidget* parent) {
    QProgressDialog progress(QStringLiteral("Обработка хранилища…"), QStringLiteral("Отменить"), 0, 0,parent);
    progress.setWindowTitle(QStringLiteral("TokenBucket"));
    progress.setWindowModality(Qt::ApplicationModal); progress.setMinimumDuration(0);
    progress.setAutoClose(false); progress.setAutoReset(false);
    QObject::connect(&progress, &QProgressDialog::canceled, &core, &tb::Application::lock);
    progress.show();
    return operation(reinterpret_cast<void*>(progress.winId()), err);
}
int runSession(Application& core,QSettings& settings,bool startHidden,QSettings* runPreferences,std::function<bool()> trayAvailable) {
    QString error;
    // Интервал автоблокировки настраивается в окне; кламп 1–60 минут — тот же, что в диалоге,
    // испорченное значение настроек не должно отключать защиту или блокировать намертво.
    const int idleMinutes = prefs::read(settings).idleMinutes;
    tb::IdleTimer idle(idleMinutes * 60 * 1000); idle.setActive(core.gate()==Application::Gate::Ready);
    tb::platform::SessionWatcher session(&core); session.setObjectName("sessionWatcher");
    if (!session.error().isEmpty()) { QMessageBox::critical(nullptr, QStringLiteral("TokenBucket"), session.error()); return 1; }
    QPointer<MainWindow> mainWindow;
    QPointer<QEventLoop> waitLoop;
    bool waitForTrayOpen=startHidden && core.gate()==Application::Gate::NeedUnlock,quitting=false;
    tb::ui::TrayController tray(core,&core,&settings); tray.setObjectName("sessionTray");
    const auto systemTrayAvailable=std::move(trayAvailable);
    trayAvailable=[&tray,systemTrayAvailable] {return (!systemTrayAvailable || systemTrayAvailable()) && tray.available();};
    QTimer trayWatch; trayWatch.setInterval(1000);
    // У исчезновения собственного значка нет общего сигнала Qt: скрытый сеанс должен остаться доступен.
    QObject::connect(&trayWatch,&QTimer::timeout,&tray,[&] {
        if(quitting || trayAvailable()) return;
        waitForTrayOpen=false;
        if(waitLoop) waitLoop->quit();
        if(mainWindow && core.gate()==Application::Gate::Ready && !mainWindow->isVisible()) mainWindow->show();
    });
    trayWatch.start();
    QObject::connect(&tray, &tb::ui::TrayController::showRequested, &core, [&] {
        if(quitting) return;
        waitForTrayOpen=false; startHidden=false;
        if(waitLoop) waitLoop->quit();
        if (mainWindow && core.gate()==Application::Gate::Ready) { mainWindow->show(); mainWindow->raise(); mainWindow->activateWindow(); }
    });
    tray.setPanelCallback([&] { if (mainWindow) mainWindow->popupQuickPanel(); });
    QObject::connect(&tray, &tb::ui::TrayController::lockRequested, &core, &tb::Application::lock);
    QObject::connect(&tray, &tb::ui::TrayController::quitRequested, &core, [&] {
        quitting=true;
        if(waitLoop) waitLoop->quit();
        if (auto* modal = qobject_cast<QDialog*>(qApp->activeModalWidget())) modal->reject();
        if (mainWindow) mainWindow->requestExit();
        else core.lock();
    });
    QObject::connect(&idle, &tb::IdleTimer::idleTimeout, &core, &tb::Application::lock);
    QObject::connect(&session, &tb::platform::SessionWatcher::lockNow, &core, &tb::Application::lock);
    QObject::connect(&core, &tb::Application::stateChanged, &idle, [&] { idle.setActive(core.gate() == tb::Application::Gate::Ready); });
    QObject::connect(&core,&Application::stateChanged,&tray,[&] {
        // Активный модальный сценарий после блокировки возвращается к видимой разблокировке.
        if(core.gate()==Application::Gate::NeedUnlock && mainWindow && !mainWindow->isVisible()
            && !QApplication::activeModalWidget()) waitForTrayOpen=true;
    });
    // Выход всегда идёт через closeEvent окна → Application::lock(); aboutToQuit здесь не
    // срабатывает, так как app.exec() не используется (событийный цикл ниже — QEventLoop).
    while(!quitting) {
        if (core.gate() == tb::Application::Gate::NeedSetup) {
            startHidden=false;
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
            const bool ok = runVaultOperation(core, [&](void* hwnd, QString* err) {
#ifdef Q_OS_WIN
                if (dialog.helloChosen()) return core.createHelloVault(err, hwnd);
#endif
                return core.createPasswordVault(password.bytes(), err);
            }, &error);
            password.clear();
            if(quitting) return 0;
            if (!ok) { QMessageBox::warning(nullptr, QStringLiteral("TokenBucket"), error); continue; }
        } else if (core.gate() == tb::Application::Gate::NeedUnlock) {
            if(waitForTrayOpen && trayAvailable()) {
                QEventLoop pending; waitLoop=&pending;
                pending.exec(); waitLoop=nullptr;
                if(quitting) return 0;
                continue;
            }
#ifndef Q_OS_WIN
            if (core.unlockMode() == tb::Application::Mode::Hello) {
                QMessageBox::critical(nullptr, QStringLiteral("TokenBucket"), QStringLiteral("Это хранилище использует Windows Hello. Для Linux нужен защищённый экспорт из Windows."));
                return 1;
            }
#endif
            UnlockDialog dialog(core.unlockMode() == tb::Application::Mode::Hello ? UnlockDialog::Kind::Hello : UnlockDialog::Kind::Password);
            if (dialog.exec() != QDialog::Accepted) return 0;
            tb::SecureBytes password(dialog.password()); dialog.clearSecrets();
            const bool ok = runVaultOperation(core, [&](void* hwnd, QString* err) {
#ifdef Q_OS_WIN
                if (!dialog.passwordMode()) return core.unlockHello(err, hwnd);
#endif
                return core.unlock(password.bytes(), err);
            }, &error);
            password.clear();
            if(quitting) return 0;
            if (!ok) { QMessageBox::warning(nullptr, QStringLiteral("TokenBucket"), error); continue; }
        } else {
            QEventLoop loop; tb::ui::MainWindow window(core,nullptr,&settings,runPreferences,trayAvailable); bool closed = false;
            window.setIdleTimer(&idle);
            QObject::connect(&window, &tb::ui::MainWindow::notifyRequested, &tray, &tb::ui::TrayController::notify);
            mainWindow = &window;
            QObject::connect(&window,&MainWindow::lockedHiddenRequested,&loop,[&] {waitForTrayOpen=true;});
            QObject::connect(&window, &tb::ui::MainWindow::closing, &loop, [&] { closed = true; loop.quit(); });
            // Настройка автоблокировки применяется немедленно: окно в области видимости,
            // перезапускаем отсчёт IdleTimer новым интервалом.
            QObject::connect(&window, &tb::ui::MainWindow::idleTimeoutChanged, &idle,
                [&](int m) { idle.setIntervalMs(m * 60 * 1000); });
            QObject::connect(&core, &tb::Application::stateChanged, &loop, [&] {
                if (core.gate() != tb::Application::Gate::Ready) loop.quit();
            });
            // --tray: автозапуск при входе в Windows стартует свёрнутым, окно не показываем.
            if(!startHidden || !trayAvailable()) window.show();
            startHidden=false;
            loop.exec();
            mainWindow = nullptr; // окно разрушается при выходе из области видимости
            if (closed) return 0;
        }
    }
    return 0;
}
}
