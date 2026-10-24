#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include "../src/app/Application.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/SessionFlow.h"
#include "../src/ui/TrayController.h"
#include "../src/ui/UnlockDialog.h"
#include "../src/ui/SetupDialog.h"
#include "../src/ui/SettingsDialog.h"
#include "../src/platform/SessionWatcher.h"
using namespace tb;
class TestSession : public QObject {
    Q_OBJECT
private slots:
    void startupAndExitRoutes_data() {
        QTest::addColumn<int>("route");
        QTest::newRow("locked-tray-quit")<<0;
        QTest::newRow("locked-tray-show-unlock")<<1;
        QTest::newRow("new-vault-setup-visible")<<2;
        QTest::newRow("locked-without-tray")<<3;
        QTest::newRow("visible-unlock-cancel")<<4;
        QTest::newRow("session-lock-hidden-ready")<<5;
        QTest::newRow("quit-during-settings")<<6;
        QTest::newRow("hidden-ready-without-tray")<<7;
    }
    void startupAndExitRoutes() {
        QFETCH(int,route);
        const bool previous=qApp->quitOnLastWindowClosed(); qApp->setQuitOnLastWindowClosed(false);
        const auto restore=qScopeGuard([&] {qApp->setQuitOnLastWindowClosed(previous);});
        QTemporaryDir dir; Application core(dir.path()); QString error;
        if(route!=2) {
            QVERIFY(core.createPasswordVault("valid-password",&error));
            if(route!=5 && route!=6 && route!=7) core.lock();
        }
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        preferences.setValue("trayNotifications",false);
        QTimer driver,watchdog; driver.setInterval(10); watchdog.setSingleShot(true); watchdog.setInterval(15000);
        QElapsedTimer wait; wait.start();
        int stage=0; bool timedOut=false,completed=false,hidden=false,prematureUnlock=false,visibleAfterUnlock=false;
        connect(&watchdog,&QTimer::timeout,&core,[&] {
            timedOut=true; if(auto* tray=core.findChild<ui::TrayController*>("sessionTray")) emit tray->quitRequested();
        });
        connect(&driver,&QTimer::timeout,&core,[&] {
            auto* tray=core.findChild<ui::TrayController*>("sessionTray"); if(!tray) return;
            ui::MainWindow* main=nullptr; UnlockDialog* unlock=nullptr; SetupDialog* setup=nullptr; ui::SettingsDialog* settings=nullptr;
            for(auto* widget:QApplication::topLevelWidgets()) {
                if(auto* window=qobject_cast<ui::MainWindow*>(widget)) main=window;
                if(auto* dialog=qobject_cast<UnlockDialog*>(widget);dialog && dialog->isVisible()) unlock=dialog;
                if(auto* dialog=qobject_cast<SetupDialog*>(widget);dialog && dialog->isVisible()) setup=dialog;
                if(auto* dialog=qobject_cast<ui::SettingsDialog*>(widget);dialog && dialog->isVisible()) settings=dialog;
            }
            if(route==0 || route==1) {
                if(stage==0) {
                    if(unlock) {prematureUnlock=true; emit tray->quitRequested(); return;}
                    if(wait.elapsed()>=100 && core.gate()==Application::Gate::NeedUnlock) {
                        hidden=true; stage=1;
                        if(route==0) {completed=true; emit tray->quitRequested();}
                        else emit tray->showRequested();
                    }
                } else if(route==1 && stage==1 && unlock) {
                    stage=2; unlock->findChild<QLineEdit*>("unlockPassword")->setText("valid-password");
                    unlock->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                } else if(route==1 && stage==2 && main && main->isVisible() && core.gate()==Application::Gate::Ready && !core.busy()) {
                    visibleAfterUnlock=true; completed=true; stage=3; main->requestExit();
                }
            } else if(route==2 && setup) {completed=true; setup->reject();}
            else if((route==3 || route==4) && unlock) {completed=true; unlock->reject();}
            else if(route==5) {
                if(stage==0 && main && core.gate()==Application::Gate::Ready) {
                    hidden=!main->isVisible(); stage=1; wait.restart();
                    if(auto* session=core.findChild<platform::SessionWatcher*>("sessionWatcher")) emit session->lockNow();
                } else if(stage==1) {
                    if(unlock) {prematureUnlock=true; emit tray->quitRequested(); return;}
                    if(wait.elapsed()>=100 && core.gate()==Application::Gate::NeedUnlock) {completed=true; stage=2; emit tray->quitRequested();}
                }
            } else if(route==6) {
                if(stage==0 && main && main->isVisible()) {
                    stage=1; QTimer::singleShot(0,main,&ui::MainWindow::showSettings);
                } else if(stage==1 && settings) {completed=true; stage=2; emit tray->quitRequested();}
                else if(stage==2 && unlock) {prematureUnlock=true; emit tray->quitRequested();}
            } else if(route==7 && main && main->isVisible() && core.gate()==Application::Gate::Ready) {
                completed=true; main->requestExit();
            }
        });
        watchdog.start(); driver.start();
        const bool trayPresent=route!=3 && route!=7;
        const int result=ui::runSession(core,preferences,route!=4 && route!=6,&run,[trayPresent] {return trayPresent;});
        driver.stop(); watchdog.stop();
        QCOMPARE(result,0); QVERIFY(!timedOut); QVERIFY(!prematureUnlock); QVERIFY(completed);
        if(route==0 || route==1 || route==5) QVERIFY(hidden);
        if(route==1) QVERIFY(visibleAfterUnlock);
        QCOMPARE(core.gate(),route==2?Application::Gate::NeedSetup:Application::Gate::NeedUnlock);
        QVERIFY(!core.findChild<ui::TrayController*>("sessionTray"));
        QVERIFY(!core.findChild<platform::SessionWatcher*>("sessionWatcher"));
    }
    void lockHideWaitsForTrayBeforeUnlock() {
        const bool previous=qApp->quitOnLastWindowClosed(); qApp->setQuitOnLastWindowClosed(false);
        const auto restore=qScopeGuard([&] {qApp->setQuitOnLastWindowClosed(previous);});
        QTemporaryDir dir; Application core(dir.path()); QString error;
        QVERIFY(core.createPasswordVault("valid-password",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        preferences.setValue("closeBehavior","lock-hide");
        preferences.setValue("trayNotifications",false);
        QTimer driver,watchdog; driver.setInterval(10); watchdog.setSingleShot(true); watchdog.setInterval(15000);
        int stage=0; bool waited=false,unexpectedUnlock=false,completed=false,timedOut=false;
        QElapsedTimer wait;
        connect(&watchdog,&QTimer::timeout,&core,[&] {
            timedOut=true; if(auto* tray=core.findChild<ui::TrayController*>("sessionTray")) emit tray->quitRequested();
        });
        connect(&driver,&QTimer::timeout,&core,[&] {
            auto* tray=core.findChild<ui::TrayController*>("sessionTray"); if(!tray) return;
            ui::MainWindow* main=nullptr; UnlockDialog* unlock=nullptr;
            for(auto* widget:QApplication::topLevelWidgets()) {
                if(auto* window=qobject_cast<ui::MainWindow*>(widget)) main=window;
                if(auto* dialog=qobject_cast<UnlockDialog*>(widget);dialog && dialog->isVisible()) unlock=dialog;
            }
            if(stage==0 && main && main->isVisible() && core.gate()==Application::Gate::Ready) {
                stage=1; wait.start(); main->close();
            } else if(stage==1) {
                if(unlock) {unexpectedUnlock=true; emit tray->quitRequested(); return;}
                if(core.gate()==Application::Gate::NeedUnlock && wait.elapsed()>=100) {
                    waited=true; stage=2; emit tray->showRequested();
                }
            } else if(stage==2 && unlock) {
                stage=3;
                unlock->findChild<QLineEdit*>("unlockPassword")->setText("valid-password");
                unlock->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            } else if(stage==3 && main && main->isVisible() && core.gate()==Application::Gate::Ready && !core.busy()) {
                completed=true; stage=4; main->requestExit();
            }
        });
        watchdog.start(); driver.start();
        const int result=ui::runSession(core,preferences,false,&run,[] {return true;});
        driver.stop(); watchdog.stop();
        QCOMPARE(result,0); QVERIFY(!timedOut); QVERIFY(!unexpectedUnlock); QVERIFY(waited); QVERIFY(completed);
        QCOMPARE(core.gate(),Application::Gate::NeedUnlock);
        QVERIFY(!core.findChild<ui::TrayController*>("sessionTray"));
    }
};
QTEST_MAIN(TestSession)
#include "test_session.moc"
