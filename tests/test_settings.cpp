#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QStackedWidget>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QMessageBox>
#include "../src/app/Application.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/SettingsDialog.h"
#include "../src/ui/MiniPanel.h"
#include "../src/ui/SessionFlow.h"
#include "../src/ui/Theme.h"
#include "../src/ui/EnvExportDialog.h"
#include "../src/platform/EnvOps.h"
#include "../src/ui/UnlockDialog.h"
#include "FaultFileOps.h"
using namespace tb;
class TestSettings : public QObject {
    Q_OBJECT
    static QByteArray readFile(const QString& path) {
        QFile file(path); if(!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
    }
#ifdef Q_OS_WIN
    static QKeySequence freeHotkey(const QKeySequence& excluded={}) {
        for(int key=Qt::Key_F6;key<=Qt::Key_F11;++key) {
            const QKeySequence sequence(Qt::CTRL|Qt::ALT|Qt::SHIFT|Qt::Key(key));
            if(sequence==excluded) continue;
            const auto parsed=paste::parseHotkey(sequence);
            if(parsed && RegisterHotKey(nullptr,0x4f18,parsed->modifiers,parsed->key)) {
                UnregisterHotKey(nullptr,0x4f18); return sequence;
            }
        }
        return {};
    }
#endif
private slots:
    void backupDialogFlow_data() {
        QTest::addColumn<int>("action");
        QTest::newRow("wrong-password")<<0; QTest::newRow("decline-confirmation")<<1; QTest::newRow("cancel-auth")<<2;
        QTest::newRow("lock-at-auth")<<3; QTest::newRow("lock-at-confirmation")<<4;
        QTest::newRow("repeat-click-and-commit")<<5; QTest::newRow("safety-write-failure")<<6;
        QTest::newRow("close-settings-during-header")<<7; QTest::newRow("close-settings-during-prepare")<<8;
    }
    void backupDialogFlow() {
        QFETCH(int,action);
        QTemporaryDir dir; FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,platform::hello::request,ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Original").isEmpty()); QVERIFY(app.save(&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Newer").isEmpty()); QVERIFY(app.save(&error));
        const auto before=readFile(app.storePathForTest()),backup=readFile(app.storePathForTest()+".bak");
        QVERIFY(!before.isEmpty()); QVERIFY(!backup.isEmpty());
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&preferences,&run,[] {return true;});
        QPointer<ui::SettingsDialog> settings; int authVisits=0,confirmVisits=0,resultVisits=0;
        int busyCount=0;
        connect(&app,&Application::busyChanged,&window,[&](bool busy) {
            if(busy && settings && ++busyCount==(action==7?1:2) && (action==7 || action==8)) settings->reject();
        });
        bool returned=false; QTimer driver; driver.setInterval(10);
        connect(&driver,&QTimer::timeout,&window,[&] {
            if(auto* auth=qobject_cast<UnlockDialog*>(QApplication::activeModalWidget())) {
                ++authVisits;
                if(action==2) {auth->reject(); return;}
                if(action==3) {app.lock(); return;}
                if(action==5 && settings) settings->findChild<QPushButton*>("restoreBackup")->click();
                auth->findChild<QLineEdit*>("unlockPassword")->setText(action==0?"wrong-password-789":"valid-password");
                auth->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            } else if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                QCOMPARE(box->textFormat(),Qt::PlainText);
                if(box->button(QMessageBox::Yes)) {
                    QVERIFY(box->text().contains(app.backupInfo().modified.toLocalTime().toString(Qt::ISODate)));
                    ++confirmVisits; QCOMPARE(box->defaultButton(),qobject_cast<QPushButton*>(box->button(QMessageBox::No)));
                    if(action==4) {app.lock(); return;}
                    if(action==6) {ops=FaultOps{}; ops.failWriteCall=1;}
                    box->button(action==1?QMessageBox::No:QMessageBox::Yes)->click();
                } else if(auto* ok=box->button(QMessageBox::Ok)) {++resultVisits; ok->click();}
            }
        });
        QTimer::singleShot(0,&window,[&] {
            settings=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(settings);
            auto* restore=settings->findChild<QPushButton*>("restoreBackup"); QVERIFY(restore); QVERIFY(restore->isEnabled());
            driver.start(); restore->click(); returned=true;
            QCOMPARE(authVisits,action==7?0:1);
            if(action==0 || action==6) {QVERIFY(settings->isVisible()); QVERIFY(!settings->findChild<QLabel*>("settingsError")->text().isEmpty());}
            if(settings && settings->isVisible()) settings->reject();
        });
        QTimer::singleShot(5000,&window,[] {if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();});
        window.showSettings(); driver.stop(); QVERIFY(returned); QCOMPARE(authVisits,action==7?0:1);
        QCOMPARE(confirmVisits,action==1 || action==4 || action==5 || action==6?1:0);
        QCOMPARE(resultVisits,action==5?1:0);
        QCOMPARE(readFile(app.storePathForTest()),action==5?backup:before);
        QCOMPARE(readFile(app.storePathForTest()+".bak"),backup);
        QCOMPARE(app.gate(),action>=3 && action<=5?Application::Gate::NeedUnlock:Application::Gate::Ready);
        QVERIFY(!app.restorePreparedBackup().committed);
        if(action==5) {QVERIFY(app.unlock("valid-password",&error)); QCOMPARE(app.model()->nodes().size(),1);}
    }
#ifdef Q_OS_WIN
    void partialSettingsDoNotSaveDraftDuringFailedRecovery() {
        QTemporaryDir dir; FaultOps ops;
        Application app(dir.path(),nullptr,platform::hello::request,ops); QString error;
        QVERIFY(app.createPasswordVault("valid-password",&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Original").isEmpty()); QVERIFY(app.save(&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Newer").isEmpty()); QVERIFY(app.save(&error));
        const auto before=readFile(app.storePathForTest());
        const auto parent=dir.filePath("prefs"); QVERIFY(QDir().mkdir(parent));
        QSettings preferences(parent+"/ui.ini",QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        preferences.setValue("secretRevealSeconds",15); preferences.sync();
        ui::MainWindow window(app,nullptr,&preferences,&run,[] {return true;});
        QVERIFY(QFile::remove(preferences.fileName())); QVERIFY(QDir().rmdir(parent));
        QFile blocker(parent); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        int authVisits=0; bool returned=false; QTimer driver; driver.setInterval(10);
        connect(&driver,&QTimer::timeout,&window,[&] {
            if(auto* auth=qobject_cast<UnlockDialog*>(QApplication::activeModalWidget())) {
                ++authVisits; auth->findChild<QLineEdit*>("unlockPassword")->setText("valid-password");
                auth->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            } else if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if(auto* yes=box->button(QMessageBox::Yes)) {ops=FaultOps{}; ops.failWriteCall=1; yes->click();}
                else if(auto* ok=box->button(QMessageBox::Ok)) ok->click();
            }
        });
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            auto* reveal=dialog->findChild<QComboBox*>("secretRevealSeconds");
            dialog->findChild<QCheckBox*>("autostart")->setChecked(true); reveal->setCurrentIndex(0);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            QVERIFY(dialog->isVisible()); QCOMPARE(run.value("TokenBucket").toString(),platform::autostart::commandForTest());
            auto* restore=dialog->findChild<QPushButton*>("restoreBackup"); QVERIFY(!restore->isEnabled());
            driver.start(); QVERIFY(QMetaObject::invokeMethod(dialog,"restoreBackupRequested")); QCOMPARE(authVisits,0);
            reveal->setCurrentIndex(2); QVERIFY(restore->isEnabled());
            restore->click(); QCOMPARE(authVisits,1); QVERIFY(dialog->isVisible());
            QVERIFY(!dialog->findChild<QLabel*>("settingsError")->text().isEmpty());
            QCOMPARE(readFile(app.storePathForTest()),before); QVERIFY(!QFile::exists(preferences.fileName()));
            returned=true; dialog->reject();
        });
        QTimer::singleShot(5000,&window,[] {if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();});
        window.showSettings(); driver.stop(); QVERIFY(returned); QCOMPARE(app.gate(),Application::Gate::Ready);
        QCOMPARE(readFile(app.storePathForTest()),before); QVERIFY(!app.restorePreparedBackup().committed);
    }
#endif
    void backupActionRequiresCleanDraftAndAvailableFile() {
        Application::BackupInfo backup; backup.state=Application::BackupInfo::State::Available;
        ui::SettingsDialog dialog({},backup,{});
        auto* restore=dialog.findChild<QPushButton*>("restoreBackup"); QVERIFY(restore); QVERIFY(restore->isEnabled());
        dialog.findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0); QVERIFY(!restore->isEnabled());
        dialog.findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(2); QVERIFY(restore->isEnabled());
        for(auto state:{Application::BackupInfo::State::Missing,Application::BackupInfo::State::Unavailable}) {
            backup.state=state; ui::SettingsDialog unavailable({},backup,{});
            QVERIFY(!unavailable.findChild<QPushButton*>("restoreBackup")->isEnabled());
        }
    }
    void savingExitPolicyWaitsForMainWindowClose() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&preferences,&run,[] {return true;});
        QSignalSpy closing(&window,&ui::MainWindow::closing);
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            auto* mode=dialog->findChild<QComboBox*>("closeBehavior"); mode->setCurrentIndex(mode->findData(int(prefs::CloseBehavior::Exit)));
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        });
        window.showSettings(); QCOMPARE(app.gate(),Application::Gate::Ready); QCOMPARE(closing.size(),0);
        QTimer::singleShot(0,&window,[] {
            if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->close();
        });
        window.showSettings(); QCOMPARE(app.gate(),Application::Gate::Ready); QCOMPARE(closing.size(),0);
        window.show(); window.close(); QCOMPARE(app.gate(),Application::Gate::NeedUnlock); QCOMPARE(closing.size(),1);
    }
    void closePolicy_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("tray"); QTest::addColumn<bool>("explicitExit");
        for(const auto* mode:{"hide","lock-hide","exit"}) for(bool tray:{false,true}) for(bool explicitExit:{false,true})
            QTest::newRow(qPrintable(QStringLiteral("%1-tray%2-explicit%3").arg(mode).arg(tray).arg(explicitExit)))
                <<QString::fromLatin1(mode)<<tray<<explicitExit;
    }
    void closePolicy() {
        QFETCH(QString,mode); QFETCH(bool,tray); QFETCH(bool,explicitExit);
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        preferences.setValue("closeBehavior",mode);
        ui::MainWindow window(app,nullptr,&preferences,&run,[tray] {return tray;});
        QStringList order;
        connect(&window,&ui::MainWindow::lockedHiddenRequested,&window,[&] {order<<"hidden";});
        connect(&app,&Application::stateChanged,&window,[&] {order<<"locked";});
        connect(&window,&ui::MainWindow::closing,&window,[&] {order<<"closing";});
        window.show();
        if(explicitExit) window.requestExit(); else window.close();
        const bool exiting=explicitExit || !tray || mode=="exit";
        const bool lockHidden=!exiting && mode=="lock-hide";
        QVERIFY(!window.isVisible());
        QCOMPARE(app.gate(),exiting || lockHidden?Application::Gate::NeedUnlock:Application::Gate::Ready);
        QCOMPARE(order,exiting?QStringList({"locked","closing"}):lockHidden?QStringList({"hidden","locked"}):QStringList());
    }
    void storedExitPreferenceLocksOnClose() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        preferences.setValue("closeBehavior","exit");
        ui::MainWindow window(app,nullptr,&preferences,&run);
        QSignalSpy closing(&window,&ui::MainWindow::closing);
        window.show(); window.close();
        QCOMPARE(app.gate(),Application::Gate::NeedUnlock); QCOMPARE(closing.size(),1);
        QVERIFY(!window.isVisible());
    }
#ifdef Q_OS_WIN
    void orderedFailuresAndRetry_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("invalid-before-any-write")<<0;
        QTest::newRow("hotkey-conflict-before-other-writes")<<1;
        QTest::newRow("run-failure-after-hotkey")<<2;
        QTest::newRow("preference-failure-after-run")<<3;
        QTest::newRow("unchanged-does-not-write")<<4;
    }
    void orderedFailuresAndRetry() {
        QFETCH(int,fault);
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        const auto initial=freeHotkey(); QVERIFY(!initial.isEmpty());
        const auto replacement=freeHotkey(initial); QVERIFY(!replacement.isEmpty());
        const auto prefsParent=dir.filePath("prefs"),runParent=dir.filePath("run");
        QVERIFY(QDir().mkdir(prefsParent)); QVERIFY(QDir().mkdir(runParent));
        QSettings preferences(prefsParent+"/ui.ini",QSettings::IniFormat),run(runParent+"/run.ini",QSettings::IniFormat);
        preferences.setValue("hotkey",initial.toString(QKeySequence::PortableText));
        preferences.setValue("secretRevealSeconds",15); preferences.sync();
        run.sync();
        ui::MainWindow window(app,nullptr,&preferences,&run);
        bool blockedHotkey=false;
        const auto release=qScopeGuard([&] {
            UnregisterHotKey(nullptr,0x4f18);
            if(blockedHotkey) UnregisterHotKey(nullptr,0x4f19);
        });
        if(fault==1) {
            const auto key=paste::parseHotkey(replacement); QVERIFY(key);
            blockedHotkey=RegisterHotKey(nullptr,0x4f19,key->modifiers,key->key); QVERIFY(blockedHotkey);
        }
        const auto block=[&](QSettings& settings,const QString& parent) {
            if(QFile::exists(settings.fileName()) && !QFile::remove(settings.fileName())) return false;
            if(!QDir().rmdir(parent)) return false;
            QFile file(parent); return file.open(QIODevice::WriteOnly);
        };
        if(fault==2 || fault==4) QVERIFY(block(run,runParent));
        if(fault==3 || fault==4) QVERIFY(block(preferences,prefsParent));
        bool visited=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            if(fault!=4) {
                dialog->findChild<QKeySequenceEdit*>("hotkey")->setKeySequence(fault==0?QKeySequence("F12"):fault==3?initial:replacement);
                dialog->findChild<QCheckBox*>("autostart")->setChecked(true);
                dialog->findChild<QCheckBox*>("trayNotifications")->setChecked(false);
                dialog->findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0);
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
                QVERIFY(dialog->isVisible());
                QVERIFY(!dialog->findChild<QLabel*>("settingsError")->text().isEmpty());
                QCOMPARE(window.findChild<QTimer*>("secretHideTimer")->interval(),15000);
                QCOMPARE(dialog->draft().revealSeconds,5);
                if(fault<=1) {
                    QCOMPARE(preferences.value("hotkey").toString(),initial.toString(QKeySequence::PortableText));
                    QVERIFY(!run.contains("TokenBucket")); QVERIFY(!preferences.contains("trayNotifications"));
                } else {
                    const auto keyName=fault==2?QStringLiteral("Сочетание клавиш"):QStringLiteral("Запускать вместе с Windows");
                    QVERIFY(dialog->findChild<QLabel*>("settingsError")->text().contains(keyName));
                    if(fault==2) {
                        QCOMPARE(preferences.value("hotkey").toString(),replacement.toString(QKeySequence::PortableText));
                        QVERIFY(QFile::remove(runParent)); QVERIFY(QDir().mkdir(runParent));
                    } else {
                        QCOMPARE(run.value("TokenBucket").toString(),platform::autostart::commandForTest());
                        QVERIFY(QFile::remove(prefsParent)); QVERIFY(QDir().mkdir(prefsParent));
                    }
                    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
                    QVERIFY(!dialog->isVisible());
                    QCOMPARE(preferences.value("secretRevealSeconds").toInt(),5);
                    QCOMPARE(run.value("TokenBucket").toString(),platform::autostart::commandForTest());
                }
            } else {
                QVERIFY(!dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
                QVERIFY(QMetaObject::invokeMethod(dialog,"saveRequested")); QVERIFY(!dialog->isVisible());
                QVERIFY(!QFile::exists(preferences.fileName())); QVERIFY(!QFile::exists(run.fileName()));
            }
            visited=true; if(dialog->isVisible()) dialog->reject();
        });
        QTimer::singleShot(3000,&window,[] {if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();});
        window.showSettings(); QVERIFY(visited);
        const auto registered=paste::parseHotkey(fault==2?replacement:initial); QVERIFY(registered);
        QVERIFY(!RegisterHotKey(nullptr,0x4f18,registered->modifiers,registered->key));
        if(fault==2) {
            const auto old=paste::parseHotkey(initial); QVERIFY(old);
            QVERIFY(RegisterHotKey(nullptr,0x4f18,old->modifiers,old->key)); UnregisterHotKey(nullptr,0x4f18);
        }
    }
    void environmentOptionUsesInjectedCheckedPreferences() {
        QTemporaryDir dir; const auto parent=dir.filePath("blocked");
        QFile file(parent); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        QSettings preferences(parent+"/env.ini",QSettings::IniFormat);
        ui::EnvExportDialog dialog(nullptr,&preferences);
        auto* clear=dialog.findChild<QCheckBox*>("clearOnExit"); QVERIFY(clear);
        clear->setChecked(true); QVERIFY(!clear->isChecked());
        QVERIFY(!dialog.findChild<QLabel*>("validationError")->text().isEmpty());
        QVERIFY(QFile::remove(parent)); QVERIFY(QDir().mkdir(parent));
        clear->setChecked(true); QVERIFY(clear->isChecked());
        QSettings reader(preferences.fileName(),preferences.format());
        QVERIFY(platform::envops::clearOnExitEnabled(&reader));
    }
#endif
    void saveAppliesAndReopens() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat);
        preferences.setValue("secretRevealSeconds",15); preferences.sync();
        QSettings run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&preferences,&run);
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            window.popupQuickPanel();
            for(auto* widget:QApplication::topLevelWidgets())
                if(auto* panel=qobject_cast<ui::MiniPanel*>(widget)) QVERIFY(!panel->isVisible());
            dialog->findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0);
            dialog->findChild<QCheckBox*>("trayNotifications")->setChecked(false);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            if(dialog->isVisible()) dialog->reject();
        });
        window.showSettings();
        QCOMPARE(preferences.value("secretRevealSeconds").toInt(),5);
        QCOMPARE(preferences.value("trayNotifications").toBool(),false);
        QCOMPARE(window.findChild<QTimer*>("secretHideTimer")->interval(),5000);
#ifdef Q_OS_WIN
        window.popupQuickPanel();
        bool popupVisible=false;
        for(auto* widget:QApplication::topLevelWidgets())
            if(auto* panel=qobject_cast<ui::MiniPanel*>(widget)) popupVisible|=panel->isVisible();
        QVERIFY(popupVisible);
#endif
        bool reopened=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            QCOMPARE(dialog->draft().revealSeconds,5); QCOMPARE(dialog->draft().notifications,false);
            reopened=true; dialog->reject();
        });
        window.showSettings(); QVERIFY(reopened);
    }
    void draftNavigationAndCancel_data() {
        QTest::addColumn<int>("dismiss");
        QTest::newRow("cancel")<<0; QTest::newRow("escape")<<1; QTest::newRow("cross")<<2;
    }
    void draftNavigationAndCancel() {
        QFETCH(int,dismiss);
        prefs::Values initial; initial.autostart=std::nullopt;
        ui::SettingsDialog dialog(initial,{},QStringLiteral("Ctrl+Alt+F8"));
        dialog.setUnlockMode(Application::Mode::Password,true);
        auto* autostart=dialog.findChild<QCheckBox*>("autostart");
        QVERIFY(!autostart->isEnabled()); QVERIFY(!dialog.draft().autostart);
        const auto originalPalette=qApp->palette();
        auto* theme=dialog.findChild<QComboBox*>("theme"); theme->setCurrentIndex(theme->findData("dark"));
        auto* reveal=dialog.findChild<QComboBox*>("secretRevealSeconds"); reveal->setCurrentIndex(0);
        auto* sections=dialog.findChild<QListWidget*>("settingsSections");
        sections->setCurrentRow(4); sections->setCurrentRow(1);
        QCOMPARE(dialog.draft().revealSeconds,5); QCOMPARE(dialog.draft().theme,QStringLiteral("dark"));
        QCOMPARE(qApp->palette(),originalPalette);
        QVERIFY(!dialog.findChild<QPushButton*>("changePassword")->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton*>("changeUnlockMode")->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton*>("clearEnvironment")->isEnabled());
        for(const auto* label:dialog.findChildren<QLabel*>()) QCOMPARE(label->textFormat(),Qt::PlainText);
        for(const char* name:{"theme","autostart","hotkey","secretRevealSeconds","clipboardClearSeconds"})
            QVERIFY(!dialog.findChild<QWidget*>(name)->accessibleName().isEmpty());
        QSignalSpy saved(&dialog,&ui::SettingsDialog::saveRequested);
        dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        if(dismiss==0) dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        else if(dismiss==1) QTest::keyClick(&dialog,Qt::Key_Escape);
        else dialog.close();
        QCOMPARE(saved.size(),0); QCOMPARE(dialog.result(),int(QDialog::Rejected)); QVERIFY(!dialog.isVisible());
    }
    void partialApplyKeepsRemainingDraft() {
        prefs::Values initial; initial.autostart=false;
        ui::SettingsDialog dialog(initial,{},QStringLiteral("Ctrl+Alt+F8"));
        auto* theme=dialog.findChild<QComboBox*>("theme"); theme->setCurrentIndex(theme->findData("dark"));
        dialog.findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0);
        auto current=initial; current.theme="dark";
        dialog.setApplyResult({{"theme"},"secretRevealSeconds","<b>Ошибка записи</b>"},current);
        QCOMPARE(dialog.draft().theme,QStringLiteral("dark")); QCOMPARE(dialog.draft().revealSeconds,5);
        auto* error=dialog.findChild<QLabel*>("settingsError");
        QVERIFY(error->text().contains(QStringLiteral("Тема"))); QCOMPARE(error->textFormat(),Qt::PlainText);
        QVERIFY(error->text().contains("<b>"));
        QVERIFY(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
        QSignalSpy requested(&dialog,&ui::SettingsDialog::saveRequested);
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QCOMPARE(requested.size(),1); QCOMPARE(dialog.result(),int(QDialog::Rejected));
        dialog.setApplyResult({},dialog.draft()); QCOMPARE(dialog.result(),int(QDialog::Accepted));
    }
    void navigationAndFooterFit_data() {
        QTest::addColumn<int>("width"); QTest::addColumn<bool>("dark");
        QTest::newRow("wide-light")<<900<<false; QTest::newRow("wide-dark")<<900<<true;
        QTest::newRow("narrow-light")<<420<<false; QTest::newRow("narrow-dark")<<420<<true;
    }
    void navigationAndFooterFit() {
        QFETCH(int,width); QFETCH(bool,dark);
        ui::theme::apply(dark?ui::theme::Mode::Dark:ui::theme::Mode::Light);
        Application::BackupInfo backup;
        backup.state=Application::BackupInfo::State::Available; backup.size=4096;
        backup.mainPath=QStringLiteral("C:/Synthetic/TokenBucket/vault.tb"); backup.backupPath=backup.mainPath+".bak";
        backup.modified=QDateTime::fromString(QStringLiteral("2026-10-24T12:34:56Z"),Qt::ISODate);
        ui::SettingsDialog dialog({},backup,QStringLiteral("Ctrl+Alt+F8"));
        dialog.setUnlockMode(Application::Mode::Password,true);
        dialog.resize(width,480); dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCOMPARE(dialog.width(),width);
        auto* sections=dialog.findChild<QListWidget*>("settingsSections");
        auto* combo=dialog.findChild<QComboBox*>("settingsSectionCombo");
        QCOMPARE(sections->isVisible(),width>=720); QCOMPARE(combo->isVisible(),width<720);
        combo->setCurrentIndex(2); QCOMPARE(sections->currentRow(),2);
        QCOMPARE(dialog.findChild<QStackedWidget*>()->currentIndex(),2);
        auto* cancel=dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel);
        QVERIFY(dialog.rect().contains(QRect(cancel->mapTo(&dialog,QPoint()),cancel->size())));
        const auto shots=qEnvironmentVariable("TOKENBUCKET_SETTINGS_SCREENSHOTS");
        for(int page=0;page<5;++page) {
            combo->setCurrentIndex(page); QCoreApplication::processEvents();
            QCOMPARE(dialog.findChild<QStackedWidget*>()->currentIndex(),page);
            cancel->setFocus(); QTest::keyClick(cancel,Qt::Key_Tab); QVERIFY(dialog.focusWidget());
            QVERIFY(dialog.rect().contains(QRect(cancel->mapTo(&dialog,QPoint()),cancel->size())));
            if(!shots.isEmpty()) {
                QVERIFY(QDir().mkpath(shots));
                QVERIFY(dialog.grab().save(shots+QStringLiteral("/settings-%1-%2-page-%3.png").arg(width).arg(dark?"dark":"light").arg(page)));
            }
        }
    }
    void backupMetadataHasNoAuthenticationSideEffects() {
        QTemporaryDir dir; Application app(dir.path());
        const auto generation=app.generation(); const auto gate=app.gate();
        const auto missing=app.backupInfo(); QCOMPARE(missing.state,Application::BackupInfo::State::Missing);
        QFile file(missing.backupPath); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("invalid container"),qint64(17)); file.close();
        const auto available=app.backupInfo(); QCOMPARE(available.state,Application::BackupInfo::State::Available);
        QCOMPARE(available.size,qint64(17)); QVERIFY(available.modified.isValid());
        QVERIFY(file.remove()); QVERIFY(QDir().mkdir(missing.backupPath));
        QCOMPARE(app.backupInfo().state,Application::BackupInfo::State::Unavailable);
        QCOMPARE(app.generation(),generation); QCOMPARE(app.gate(),gate); QVERIFY(!app.busy());
    }
    void lockClosesWithoutApplyingDraft() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat);
        preferences.setValue("secretRevealSeconds",15); preferences.sync();
        QSettings run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&preferences,&run); bool visited=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            dialog->findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0); visited=true;
            app.lock();
        });
        window.showSettings(); QVERIFY(visited);
        QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
        QCOMPARE(preferences.value("secretRevealSeconds").toInt(),15);
    }
    void menuCancelKeepsPreferences() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat);
        preferences.setValue("secretRevealSeconds",15); preferences.sync();
        QSettings run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&preferences,&run);
        auto* action=window.findChild<QAction*>("openSettings"); QVERIFY(action);
        QFile file(preferences.fileName()); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto before=file.readAll(); file.close();
        bool visited=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            auto* reveal=dialog->findChild<QComboBox*>("secretRevealSeconds"); QVERIFY(reveal);
            reveal->setCurrentIndex(0); visited=true; dialog->reject();
        });
        action->trigger(); QVERIFY(visited);
        preferences.sync(); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(),before);
        QCOMPARE(preferences.value("secretRevealSeconds").toInt(),15);
    }
};
int main(int argc,char** argv) {
    QApplication qt(argc,argv);
    if(qt.arguments().contains("--settings-demo")) {
        qt.setQuitOnLastWindowClosed(false);
        QTemporaryDir dir; Application vault(dir.path()); QString error;
        if(!vault.createPasswordVault("test-password-123",&error)) return 2;
        if(!vault.model()->addNode(QUuid::createUuid(),{},QStringLiteral("Демо: исходная версия")).isEmpty() || !vault.save(&error)) return 3;
        if(!vault.model()->addNode(QUuid::createUuid(),{},QStringLiteral("Демо: новые данные")).isEmpty() || !vault.save(&error)) return 4;
        QSettings preferences(dir.filePath("demo.ini"),QSettings::IniFormat);
        QSettings run(dir.filePath("demo-run.ini"),QSettings::IniFormat);
        ui::theme::apply(qt.arguments().contains("--dark")?ui::theme::Mode::Dark:ui::theme::Mode::Light);
        if(!qt.arguments().contains("--tray")) QTimer::singleShot(0,&vault,[&] {
            for(auto* widget:QApplication::topLevelWidgets()) if(auto* window=qobject_cast<ui::MainWindow*>(widget)) {
                window->setWindowTitle(QStringLiteral("TokenBucket — вымышленные данные"));
                QTimer::singleShot(0,window,[&] {
                    if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                        dialog->setWindowTitle(QStringLiteral("Настройки — вымышленные данные"));
                        if(qt.arguments().contains("--compact")) dialog->resize(420,480);
                    }
                });
                window->showSettings(); break;
            }
        });
        return ui::runSession(vault,preferences,qt.arguments().contains("--tray"),&run);
    }
    TestSettings test; return QTest::qExec(&test,argc,argv);
}
#include "test_settings.moc"
