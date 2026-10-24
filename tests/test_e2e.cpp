#include <QtTest>
#include "FaultFileOps.h"
#include <QProcess>
#include <QScopeGuard>
#include <cstdio>
#include "../src/platform/QuickPasteWin.h"
#include "../src/ui/MiniPanel.h"
#include "../src/ui/Theme.h"
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QSettings>
#include <QTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QToolButton>
#include "../src/app/Application.h"
#include "../src/ui/MainWindow.h"
#include "../src/app/IdleTimer.h"
#include "../src/ui/SetupDialog.h"
#include "../src/ui/UnlockDialog.h"
#include "../src/ui/EntryEditDialog.h"
#include "../src/ui/EntryCard.h"
#include "../src/ui/TrayController.h"
#include "../src/ui/SessionFlow.h"
#include "../src/ui/SettingsDialog.h"
#include "../src/platform/ClipboardGuard.h"
#include "../src/platform/QuickPaste.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace tb;

// E2E-путешествия: реальные диалоги, реальная модель/файлы/буфер, без моков.
// Системные файловые диалоги (QFileDialog) автотестом не управляемы — они в ручной матрице приёмки.
namespace {
#ifdef Q_OS_WIN
bool activateTestReceiver(HWND receiver,DWORD expectedPid) {
    DWORD actualPid=0;
    const DWORD receiverThread=GetWindowThreadProcessId(receiver,&actualPid);
    if(!IsWindow(receiver) || !IsWindowVisible(receiver)
        || !receiverThread || actualPid!=expectedPid) return false;
    if(GetForegroundWindow()==receiver) return true;
    // Only fixture setup: permit the known test receiver to become foreground.
    const DWORD currentThread=GetCurrentThreadId();
    const DWORD foregroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
    const bool attached=foregroundThread && foregroundThread!=currentThread
        && AttachThreadInput(currentThread,foregroundThread,TRUE);
    const bool receiverAttached=receiverThread!=currentThread && receiverThread!=foregroundThread
        && AttachThreadInput(currentThread,receiverThread,TRUE);
    const auto detach=qScopeGuard([&] {
        if(receiverAttached) AttachThreadInput(currentThread,receiverThread,FALSE);
        if(attached) AttachThreadInput(currentThread,foregroundThread,FALSE);
    });
    return SetForegroundWindow(receiver);
}
#endif
void typeInto(::EntryEditDialog& dialog, int type, const QString& title, std::function<void(::EntryEditDialog&)> extra = {}) {
    dialog.findChild<QComboBox*>(QStringLiteral("entryType"))->setCurrentIndex(type);
    dialog.findChild<QLineEdit*>(QStringLiteral("entryTitle"))->setText(title);
    if (extra) extra(dialog);
    dialog.accept();
}
}

class TestE2E : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
#ifdef Q_OS_WIN
    void quickPanelProtectedCopy_data() {
        QTest::addColumn<int>("interruption");
        QTest::newRow("confirmed") << 0;
        QTest::newRow("rejected") << 1;
        QTest::newRow("locked") << 2;
        QTest::newRow("deleted") << 3;
        QTest::newRow("canceled-and-reopened") << 4;
    }
    void quickPanelProtectedCopy() {
        QFETCH(int, interruption);
        QTemporaryDir dir; Application vault(dir.path()); QString error;
        QVERIFY(vault.createPasswordVault("test-password-123", &error));
        const auto group = QUuid::createUuid();
        QVERIFY(vault.model()->addNode(group, {}, "Demo").isEmpty());
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = group;
        e.title = "protected"; e.token = "synthetic-protected-token"; e.reverify = true;
        QVERIFY(vault.model()->setEntry(e).isEmpty());
        ClipboardGuard clip; ui::MiniPanel panel(vault, clip);
        ClipboardGuard marker;
        QVERIFY2(marker.copySecret("marker",&error),qPrintable(error));
        panel.popup();
        panel.findChild<QLineEdit*>("miniSearch")->setText("protected");
        QString reopenedStatus;
        QTimer answer; answer.setInterval(10);
        connect(&answer, &QTimer::timeout, &panel, [&] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                answer.stop();
                if (interruption == 1) { dialog->reject(); return; }
                if (interruption == 2) { vault.lock(); return; }
                if (interruption == 3) vault.model()->removeEntry(e.id);
                if (interruption == 4) {
                    panel.popup(); panel.popup();
                    panel.findChild<QLineEdit*>("miniSearch")->setText("new-context-missing");
                    reopenedStatus = panel.findChild<QLabel*>("miniStatus")->text();
                }
                dialog->setTextValue("test-password-123"); dialog->accept();
            }
        });
        auto* copy = panel.findChild<QPushButton*>("miniCopy"); QVERIFY(copy->isEnabled());
        answer.start(); copy->click();
        QVERIFY2(!answer.isActive(), qPrintable(panel.findChild<QLabel*>("miniStatus")->text()));
        if (interruption == 0) QVERIFY2(!panel.isVisible(), qPrintable(panel.findChild<QLabel*>("miniStatus")->text()));
        QCOMPARE(QApplication::clipboard()->text(), interruption ? QStringLiteral("marker") : e.token);
        if (interruption == 0 || interruption == 2) QVERIFY(!panel.isVisible());
        else if (interruption != 4) QVERIFY(panel.isVisible());
        if (interruption == 4) {
            QCOMPARE(panel.findChild<QLabel*>("miniStatus")->text(), panel.isVisible() ? reopenedStatus : QString());
            if (panel.isVisible()) QCOMPARE(panel.findChild<QLineEdit*>("miniSearch")->text(), QStringLiteral("new-context-missing"));
        }
        panel.close(); clip.clearIfOurs();
    }
    void quickPanelCancelPublication() {
        QTemporaryDir dir; Application vault(dir.path()); QString error;
        QVERIFY(vault.createPasswordVault("test-password-123", &error));
        const auto group = QUuid::createUuid();
        QVERIFY(vault.model()->addNode(group, {}, "Demo").isEmpty());
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = group;
        e.title = "cancel"; e.token = "must-not-publish";
        QVERIFY(vault.model()->setEntry(e).isEmpty());
        ClipboardGuard clip; ui::MiniPanel panel(vault, clip);
        ClipboardGuard marker;
        QVERIFY2(marker.copySecret("marker",&error),qPrintable(error)); panel.popup();
        panel.findChild<QLineEdit*>("miniSearch")->setText("cancel");
        const auto sequence = GetClipboardSequenceNumber();
        QVERIFY(QTest::qWaitFor([&] { return OpenClipboard(reinterpret_cast<HWND>(panel.winId())); }, 1000));
        QTimer::singleShot(1, &panel, [&] { panel.popup(); });
        QTimer::singleShot(20, &panel, [] { CloseClipboard(); });
        panel.findChild<QPushButton*>("miniCopy")->click();
        CloseClipboard();
        QCOMPARE(GetClipboardSequenceNumber(), sequence);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("marker"));
        QVERIFY(!panel.isVisible());
    }
    void backupRecoveryUsesCopyModeAndReturnsToNormalUnlock_data() {
        QTest::addColumn<bool>("quitOnRename");
        QTest::newRow("normal-unlock")<<false; QTest::newRow("quit-after-committed-rename")<<true;
    }
    void backupRecoveryUsesCopyModeAndReturnsToNormalUnlock() {
        QFETCH(bool,quitOnRename);
        QTemporaryDir dir; FaultOps ops;
        Application core(dir.path(),nullptr,[](void*,const QString&) {return platform::hello::Result::Verified;},ops); QString error;
        QVERIFY(core.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid();
        QVERIFY(core.model()->addNode(group,{},"Original").isEmpty()); QVERIFY(core.save(&error));
        QVERIFY(core.model()->addNode(QUuid::createUuid(),{},"Newer").isEmpty()); QVERIFY(core.switchToHello(&error));
        QCOMPARE(core.mode(),Application::Mode::Hello); QCOMPARE(core.backupMode(&error),Application::Mode::Password);
        const auto readFile=[](const QString& path) {QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();};
        const auto before=readFile(core.storePathForTest()),backup=readFile(core.storePathForTest()+".bak");
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        preferences.setValue("trayNotifications",false);
        QTimer driver,watchdog; driver.setInterval(10); watchdog.setSingleShot(true); watchdog.setInterval(15000);
        int stage=0; bool timedOut=false,completed=false,restoredData=false,copyPasswordPrompt=false,quitSent=false,postQuitDialog=false;
        if(quitOnRename) ops.afterRename=[&](const QString& from,const QString& to) {
            if(to==core.storePathForTest() && from.contains(".restore-")) QMetaObject::invokeMethod(&core,[&] {
                quitSent=true; if(auto* tray=core.findChild<ui::TrayController*>("sessionTray")) emit tray->quitRequested();
            },Qt::BlockingQueuedConnection);
        };
        connect(&watchdog,&QTimer::timeout,&core,[&] {
            timedOut=true; if(auto* tray=core.findChild<ui::TrayController*>("sessionTray")) emit tray->quitRequested();
        });
        connect(&driver,&QTimer::timeout,&core,[&] {
            ui::MainWindow* main=nullptr;
            for(auto* widget:QApplication::topLevelWidgets()) if(auto* window=qobject_cast<ui::MainWindow*>(widget)) main=window;
            auto* modal=QApplication::activeModalWidget();
            if(stage==0 && main && main->isVisible()) {
                stage=1; QTimer::singleShot(0,main,&ui::MainWindow::showSettings);
            } else if(stage==1) {
                if(auto* settings=qobject_cast<ui::SettingsDialog*>(modal)) {
                    auto* restore=settings->findChild<QPushButton*>("restoreBackup");
                    if(restore && restore->isEnabled()) {stage=2; QTimer::singleShot(0,restore,&QPushButton::click);}
                }
            } else if(stage==2 || stage==5) {
                if(auto* auth=qobject_cast<UnlockDialog*>(modal)) {
                    auto* password=auth->findChild<QLineEdit*>("unlockPassword");
                    if(!password) {if(auto* tray=core.findChild<ui::TrayController*>("sessionTray")) emit tray->quitRequested(); return;}
                    if(stage==2) copyPasswordPrompt=auth->windowTitle().contains(QStringLiteral("резервной"));
                    stage=stage==2?3:6; password->setText("valid-password");
                    auth->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                }
            } else if(stage==3) {
                if(auto* confirm=qobject_cast<QMessageBox*>(modal);confirm && confirm->button(QMessageBox::Yes)) {
                    stage=4; confirm->button(QMessageBox::Yes)->click();
                }
            } else if(stage==4) {
                if(auto* outcome=qobject_cast<QMessageBox*>(modal);outcome && outcome->button(QMessageBox::Ok)) {
                    postQuitDialog=quitSent;
                    stage=5; outcome->button(QMessageBox::Ok)->click();
                }
            } else if(stage==6 && main && main->isVisible() && core.gate()==Application::Gate::Ready && !core.busy()) {
                restoredData=core.mode()==Application::Mode::Password && core.model()->nodes().size()==1
                    && core.model()->nodes().value(group).name==QStringLiteral("Original");
                completed=true; stage=7; main->requestExit();
            }
        });
        driver.start(); watchdog.start();
        const int result=ui::runSession(core,preferences,false,&run,[] {return true;});
        driver.stop(); watchdog.stop();
        QCOMPARE(result,0); QVERIFY(!timedOut); QVERIFY(copyPasswordPrompt);
        if(quitOnRename) {
            QVERIFY(quitSent); QVERIFY(!postQuitDialog); QCOMPARE(readFile(core.storePathForTest()),backup);
            const auto copies=QDir(dir.path()).entryList({"vault.tb.before-restore-*"},QDir::Files);
            QCOMPARE(copies.size(),1); QCOMPARE(readFile(dir.filePath(copies.first())),before);
        } else {QVERIFY(completed); QVERIFY(restoredData);}
        QCOMPARE(core.gate(),Application::Gate::NeedUnlock);
    }
    void quickPasteFullRoute() {
        QTemporaryDir dir;
        Application vault(dir.path()); QString error;
        QVERIFY(vault.createPasswordVault("test-password-123", &error));
        const auto project = QUuid::createUuid(), group = QUuid::createUuid();
        QVERIFY(vault.model()->addNode(project, {}, "Demo").isEmpty());
        QVERIFY(vault.model()->addNode(group, project, "Group").isEmpty());
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = group;
        e.type = model::EntryType::Credentials; e.title = "demo"; e.password = "demo-password-777";
        QVERIFY(vault.model()->setEntry(e).isEmpty());
        ClipboardGuard clip;
        platform::QuickPaste quick(vault, clip, nullptr);
        bool registered = false;
        for (const auto* candidate : {"Ctrl+Alt+F8", "Ctrl+Alt+F9", "Ctrl+Alt+F10", "Ctrl+Alt+Space"})
            if (quick.start(QKeySequence(QString::fromLatin1(candidate)), &error)) { registered = true; break; }
        QVERIFY2(registered, qPrintable(error));
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--paste-receiver"});
        QVERIFY(child.waitForStarted());
        const auto cleanup = qScopeGuard([&] { child.terminate(); if (!child.waitForFinished(1000)) { child.kill(); child.waitForFinished(1000); } });
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += child.readAllStandardOutput()).contains('\n'), 5000);
        const auto header = output.left(output.indexOf('\n')).simplified().split(' ');
        QCOMPARE(header.size(), 3); QCOMPARE(header[0], QByteArray("READY"));
        HWND receiver = reinterpret_cast<HWND>(quintptr(header[1].toULongLong()));
        QVERIFY(header[2].toULong() != GetCurrentProcessId());
        QVERIFY(IsWindow(receiver));
        QVERIFY(IsWindowVisible(receiver));
        DWORD actualPid = 0; GetWindowThreadProcessId(receiver, &actualPid);
        QCOMPARE(actualPid, DWORD(header[2].toULong()));
        QVERIFY(activateTestReceiver(receiver,actualPid));
        QTRY_VERIFY(GetForegroundWindow() == receiver);
        const auto parsed = paste::parseHotkey(quick.sequence()); QVERIFY(parsed);
        INPUT trigger[6] = {};
        const WORD key[6] = {VK_CONTROL, VK_MENU, WORD(parsed->key), WORD(parsed->key), VK_MENU, VK_CONTROL};
        for (int i = 0; i < 6; ++i) { trigger[i].type = INPUT_KEYBOARD; trigger[i].ki.wVk = key[i]; trigger[i].ki.dwFlags = i >= 3 ? KEYEVENTF_KEYUP : 0; }
        for (int route = 0; route < 10; ++route) {
            qInfo("Quick-paste route %d", route);
            e.type = route == 2 ? model::EntryType::ApiToken : model::EntryType::Credentials;
            e.login = "synthetic-login"; e.token = "synthetic-token";
            e.reverify = route == 3;
            if (e.reverify) e.password = "synthetic-verified-password";
            QVERIFY(vault.model()->setEntry(e).isEmpty());
            QVERIFY(vault.save(&error));
            QTRY_VERIFY_WITH_TIMEOUT(GetForegroundWindow() == receiver || SetForegroundWindow(receiver), 5000);
            QTRY_VERIFY(GetForegroundWindow() == receiver);
            QTRY_VERIFY(paste::modifiersReleased());
            QTest::qWait(100);
            // Select only the dedicated receiver's old synthetic value.
            INPUT select[4] = {};
            for (int i = 0; i < 4; ++i) { select[i].type = INPUT_KEYBOARD; select[i].ki.wVk = i == 0 || i == 3 ? VK_CONTROL : 'A'; select[i].ki.dwFlags = i >= 2 ? KEYEVENTF_KEYUP : 0; }
            const auto selected = SendInput(4, select, sizeof(INPUT));
            if (selected == 2) SendInput(2, select + 2, sizeof(INPUT));
            else if (selected == 1 || selected == 3) SendInput(1, select + 3, sizeof(INPUT));
            QCOMPARE(selected, UINT(4)); QTest::qWait(50);
            const auto triggered = SendInput(6, trigger, sizeof(INPUT));
            if (triggered && triggered < 6) {
                // Release only down-events not yet balanced by this partial batch.
                INPUT release[3] = {}; UINT count = 0;
                for (int down = 2; down >= 0; --down)
                    if (triggered > UINT(down) && triggered <= UINT(5 - down)) release[count++] = trigger[5 - down];
                if (count) SendInput(count, release, sizeof(INPUT));
            }
            QCOMPARE(triggered, UINT(6));
            ui::MiniPanel* panel = nullptr;
            QTRY_VERIFY(([&] { for (auto* w : QApplication::topLevelWidgets()) if ((panel = qobject_cast<ui::MiniPanel*>(w)) && panel->isVisible()) return true; return false; }()));
            QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(panel->winId()));
            auto* search = panel->findChild<QLineEdit*>("miniSearch"); QVERIFY(search);
            QTest::keyClicks(search, "demo");
            if (route == 1) panel->findChild<QComboBox*>("miniField")->setCurrentIndex(1);
            if (route >= 4) {
                ClipboardGuard marker;
                QVERIFY2(marker.copySecret("negative-route-marker",&error),qPrintable(error));
                const auto sequence = GetClipboardSequenceNumber();
                output += child.readAllStandardOutput(); const auto before = output;
                INPUT shift{}; shift.type = INPUT_KEYBOARD; shift.ki.wVk = VK_SHIFT;
                QCOMPARE(SendInput(1, &shift, sizeof(INPUT)), UINT(1));
                const auto releaseShift = qScopeGuard([&] { shift.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1, &shift, sizeof(INPUT)); });
                QTest::keyClick(search, Qt::Key_Return);
                QLineEdit other; other.setWindowTitle("TokenBucket — другой тестовый получатель");
                if (route == 5) QVERIFY(vault.model()->removeEntry(e.id).isEmpty());
                if (route == 6) { auto changed = *vault.model()->entry(e.id); changed.modified = changed.modified.addSecs(1); QVERIFY(vault.model()->setEntry(changed).isEmpty()); }
                if (route == 7) { other.show(); other.activateWindow(); QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(other.winId())); }
                if (route == 8) vault.lock();
                if (route == 9) { child.terminate(); QVERIFY(child.waitForFinished(1000)); }
                QTest::qWait(1750);
                QCOMPARE(GetClipboardSequenceNumber(), sequence);
                QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("negative-route-marker"));
                output += child.readAllStandardOutput(); QCOMPARE(output, before);
                if (route == 8) { QVERIFY(!panel->isVisible()); QVERIFY(vault.unlock("test-password-123", &error)); }
                panel->close(); other.close();
                if (route != 9) { QVERIFY(SetForegroundWindow(receiver)); QTRY_COMPARE(GetForegroundWindow(), receiver); }
                continue;
            }
            QTimer answer; answer.setInterval(10);
            connect(&answer, &QTimer::timeout, panel, [&] {
                if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    dialog->setTextValue("test-password-123"); dialog->accept(); answer.stop();
                }
            });
            if (e.reverify) answer.start();
            if (route == 1) panel->findChild<QPushButton*>("miniPaste")->click();
            else if (route == 2) QTest::keyClick(panel->findChild<QListWidget*>("miniEntries"), Qt::Key_Return);
            else QTest::keyClick(search, Qt::Key_Return);
            const auto expected = (route == 1 ? e.login : route == 2 ? e.token : e.password).toUtf8().toBase64();
            QTRY_VERIFY2_WITH_TIMEOUT((output += child.readAllStandardOutput()).contains("TEXT " + expected),
                qPrintable(panel->findChild<QLabel*>("miniStatus")->text()),4000);
            QTest::qWait(100); output += child.readAllStandardOutput();
            QVERIFY(!output.contains("RETURN")); QVERIFY(!panel->isVisible());
            clip.clearIfOurs();
        }
    }
#endif
    void initTestCase() {
        qApp->setQuitOnLastWindowClosed(false);
#ifdef Q_OS_WIN
        qputenv("QT_QPA_FONTDIR", qEnvironmentVariable("WINDIR").toUtf8() + "/Fonts");
#endif
    }

    // Полный день жизни пользователя: создание хранилища через SetupDialog, проекты/группы,
    // все три типа записей через редактор (генератор пароля, теги, SSH из файла), поиск,
    // чип тега + тип-фильтр, избранное, копирование с очисткой буфера, повторная проверка
    // при показе и редактировании, блокировка и разблокировка через UnlockDialog.
    void fullVaultLifecycleJourney() {
        // 1. Экран создания: мастер-пароль дважды.
        SetupDialog setup(false);
        setup.findChild<QLineEdit*>(QStringLiteral("masterPassword"))->setText(QStringLiteral("мастер-пароль-e2e"));
        setup.findChild<QLineEdit*>(QStringLiteral("confirmPassword"))->setText(QStringLiteral("мастер-пароль-e2e"));
        setup.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        const QByteArray masterPassword = setup.masterPassword();
        QVERIFY(Application::validPassword(masterPassword));

        Application app(dir_.path() + "/life");
        QString error;
        QVERIFY(app.createPasswordVault(masterPassword, &error));

        // 2. Структура: проект → группа (модель уровня UI-действий).
        const auto project = QUuid::createUuid(), group = QUuid::createUuid();
        QVERIFY(app.model()->addNode(project, {}, "Прод").isEmpty());
        QVERIFY(app.model()->addNode(group, project, "Сервисы").isEmpty());

        // 3. Три записи через РЕАЛЬНЫЙ редактор: токен с тегами, креды с генератором, SSH из файла.
        QSettings uiPreferences(dir_.filePath("main-window.ini"), QSettings::IniFormat);
        ui::MainWindow w(app, nullptr, &uiPreferences,nullptr,[] {return true;}); w.show();
        {
            ::EntryEditDialog tokenDialog(*app.model(), std::nullopt, &w);
            tokenDialog.setGroup(group);
            typeInto(tokenDialog, 0, QStringLiteral("Токен CI"), [&](auto& d) {
                d.findChild<QLineEdit*>(QStringLiteral("entryToken"))->setText(QStringLiteral("ci-token-abc-123"));
                d.findChild<QLineEdit*>(QStringLiteral("entryTags"))->setText(QStringLiteral(" прод , CI , ci "));
                d.findChild<QCheckBox*>(QStringLiteral("entryReverify"))->setChecked(true);
            });
            QVERIFY(tokenDialog.entry());
            QVERIFY(app.model()->setEntry(*tokenDialog.entry()).isEmpty());
        }
        QString generated;
        {
            ::EntryEditDialog credDialog(*app.model(), std::nullopt, &w);
            credDialog.setGroup(group);
            typeInto(credDialog, 1, QStringLiteral("Логин портала"), [&](auto& d) {
                d.findChild<QLineEdit*>(QStringLiteral("entryLogin"))->setText(QStringLiteral("roman@example.com"));
                d.findChild<QPushButton*>(QStringLiteral("generatePassword"))->click();
                generated = d.findChild<QLineEdit*>(QStringLiteral("entryPassword"))->text();
            });
            QVERIFY(credDialog.entry());
            QVERIFY(app.model()->setEntry(*credDialog.entry()).isEmpty());
        }
        QCOMPARE(generated.size(), 24); // генератор сработал внутри реального редактора
        QString sshPath;
        {
            const QByteArray key = "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\r\n-----END OPENSSH PRIVATE KEY-----\r\n";
            sshPath = dir_.filePath("id_e2e");
            QFile keyFile(sshPath); QVERIFY(keyFile.open(QIODevice::WriteOnly)); keyFile.write(key); keyFile.close();
            ::EntryEditDialog sshDialog(*app.model(), std::nullopt, &w);
            sshDialog.setGroup(group);
            typeInto(sshDialog, 2, QStringLiteral("Ключ сервера"), [&](auto& d) {
                d.findChild<QLineEdit*>(QStringLiteral("sshPrivatePath"))->setText(sshPath);
                d.findChild<QCheckBox*>(QStringLiteral("entryFavorite"))->setChecked(true);
            });
            QVERIFY(sshDialog.entry());
            QVERIFY(app.model()->setEntry(*sshDialog.entry()).isEmpty());
        }
        QVERIFY(app.save(&error));
        w.refreshAll();
        // записи лежат в группе «Сервисы» — выбираем её в дереве (после refreshAll выделен проект).
        for (QTreeWidgetItemIterator it(w.tree()); *it; ++it)
            if ((*it)->text(0) == QStringLiteral("Сервисы")) { w.tree()->setCurrentItem(*it); break; }
        QCOMPARE(w.list()->count(), 3);

        // 4. Поиск находит каждую запись; чип тега «прод» фильтрует вместе с тип-фильтром.
        w.search()->setText(QStringLiteral("портала")); QCOMPARE(w.list()->count(), 1);
        w.search()->setText(QStringLiteral("сервисы"));  QCOMPARE(w.list()->count(), 3); // имя группы находит поддерево
        w.search()->clear();
        auto* tagBar = w.findChild<QWidget*>(QStringLiteral("tagBar"));
        QToolButton* chip = nullptr;
        for (auto* button : tagBar->findChildren<QToolButton*>())
            if (button->text() == QStringLiteral("прод")) chip = button;
        QStringList chipTexts;
        for (auto* button : tagBar->findChildren<QToolButton*>()) chipTexts << button->text();
        const QString firstChip = chipTexts.isEmpty() ? QString() : chipTexts.first();
        QVERIFY(chip);
        chip->click(); QCOMPARE(w.list()->count(), 1); // только токен с тегом
        auto* typeFilter = w.findChild<QComboBox*>(QStringLiteral("typeFilter"));
        typeFilter->setCurrentIndex(1); QCOMPARE(w.list()->count(), 1); // «API-токены» — совпадает
        typeFilter->setCurrentIndex(3); QCOMPARE(w.list()->count(), 0); // «SSH-ключи» — нет
        typeFilter->setCurrentIndex(0); chip->click();

        // 5. Карточка: показ секрета по кнопке (15 с), копирование и очистка буфера.
        const auto tokenId = [app = &app] {
            for (auto id : app->model()->entries().keys())
                if (app->model()->entry(id)->title == QStringLiteral("Токен CI")) return id;
            return model::Id{};
        }();
        w.list()->setCurrentRow(0);
        // выбрать именно токен: через поиск
        w.search()->setText(QStringLiteral("токен ci")); QCOMPARE(w.list()->count(), 1);
        QVERIFY(!w.card()->secretText().contains(QStringLiteral("ci-token-abc-123"))); // до показа скрыт
        {   // показ секрета у reverify-записи просит подтверждение — отвечает реальный драйвер
            QString verifyAnswer = QString::fromUtf8(masterPassword);
            QTimer verifyDriver; verifyDriver.setInterval(20);
            connect(&verifyDriver, &QTimer::timeout, &w, [&] {
                for (auto* top : QApplication::topLevelWidgets())
                    if (auto* input = qobject_cast<QInputDialog*>(top); input && input->isVisible())
                        { input->setTextValue(verifyAnswer); input->accept(); }
            });
            verifyDriver.start();
            w.card()->revealButton()->click();
            verifyDriver.stop();
        }
        QVERIFY(w.card()->secretText().contains(QStringLiteral("ci-token-abc-123")));
        w.card()->hideNowForTest();
        ClipboardGuard guard;
        QVERIFY(guard.copySecret(QStringLiteral("e2e-secret-42"), &error));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("e2e-secret-42"));
        guard.triggerClearNowForTest();
        QVERIFY(QApplication::clipboard()->text().isEmpty()); // свой секрет очищен

        // 6. Reverify-запись: верный пароль в реальном диалоге проверки открывает редактор
        // (отрицательная ветка «неверный пароль — редактор не открывается» покрыта
        // отдельным тестом reverifyAskedBeforeEditorShowsSecrets в test_ui).
        QString answer(QStringLiteral("wrong-password-xx"));
        bool editorSeen = false;
        QTimer driver; driver.setInterval(20);
        connect(&driver, &QTimer::timeout, &w, [&] {
            for (auto* top : QApplication::topLevelWidgets()) {
                if (auto* input = qobject_cast<QInputDialog*>(top); input && input->isVisible())
                    { input->setTextValue(answer); input->accept(); continue; }
                if (auto* editor = qobject_cast<::EntryEditDialog*>(top); editor && editor->isVisible()) {
                    editorSeen = true;
                    if (auto* box = editor->findChild<QDialogButtonBox*>())
                        box->button(QDialogButtonBox::Cancel)->click(); // штатная отмена, как в прочих тестах
                }
            }
        });
        answer = QString::fromUtf8(masterPassword);
        driver.start();
        w.editSelected(); // блокируется в exec() редактора, драйвер фиксирует и закрывает
        driver.stop();
        QVERIFY(editorSeen);
        // 7. Блокировка из UI и разблокировка через РЕАЛЬНЫЙ UnlockDialog.
        QVERIFY(app.save(&error));
        w.close(); // сворачивание в трей не блокирует (окно скрыто, но приложение живёт)
        QVERIFY(w.isHidden());
        QCOMPARE(app.gate(),Application::Gate::Ready);
        app.lock();
        QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
        ::UnlockDialog unlock(::UnlockDialog::Kind::Password);
        unlock.findChild<QLineEdit*>(QStringLiteral("unlockPassword"))->setText(QString::fromUtf8(masterPassword));
        unlock.accept();
        QVERIFY(app.unlock(unlock.password(), &error));
        QCOMPARE(app.gate(), Application::Gate::Ready);
        QCOMPARE(app.model()->entries().size(), 3); // все записи на месте
        const auto* finalToken = app.model()->entry(tokenId);
        QVERIFY2(finalToken, "запись токена не найдена после разблокировки");
        QVERIFY2(finalToken->tags.contains(QStringLiteral("прод")),
                 qPrintable(QStringLiteral("теги после круга: %1").arg(finalToken->tags.join(QLatin1Char(',')))));
    }

    // Миграция «на другую машину»: экспорт в файл, новое приложение в другом каталоге
    // импортирует контейнер с новым мастер-паролем; теги и SSH-байты переживают переезд.
    void exportImportMigrationJourney() {
        Application source(dir_.path() + "/machine-a"); QString error;
        QVERIFY(source.createPasswordVault("пароль-машины-а", &error));
        const auto project = QUuid::createUuid();
        QVERIFY(source.model()->addNode(project, {}, "Проект А").isEmpty());
        model::Entry token; token.id = QUuid::createUuid(); token.groupId = project;
        token.type = model::EntryType::ApiToken; token.title = "Токен переезда";
        token.token = "mig-token"; token.tags = {QStringLiteral("переезд")};
        QVERIFY(source.model()->setEntry(token).isEmpty());
        model::Entry ssh = token; ssh.id = QUuid::createUuid();
        ssh.type = model::EntryType::SshKey; ssh.title = "Ключ переезда"; ssh.token.clear();
        ssh.tags.clear(); ssh.sshPrivate = QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\r\nxyz\r\n-----END-----\r\n");
        QVERIFY(source.model()->setEntry(ssh).isEmpty());
        QVERIFY(source.save(&error));

        const QByteArray exported = source.exportContainer("пароль-переноса-9", &error);
        QVERIFY(!exported.isEmpty());
        const QString exportPath = dir_.filePath("transfer.tb");
        QFile file(exportPath); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(exported), qint64(exported.size())); file.close();

        Application target(dir_.path() + "/machine-b");
        QVERIFY(target.importContainer(exported, "пароль-переноса-9", "пароль-машины-б", &error));
        QCOMPARE(target.model()->entries().size(), 2);
        QVERIFY(target.model()->allTags().contains(QStringLiteral("переезд")));
        target.lock();
        QVERIFY(target.unlock("пароль-машины-б", &error));
        const auto* moved = target.model()->entry(ssh.id);
        QVERIFY(moved && moved->sshPrivate.contains("xyz\r\n")); // байты ключа без изменений
    }

#ifdef Q_OS_WIN
    // Трей: индикатор и подсказка следят за состоянием хранилища; уведомление не падает.
    void trayStatesJourney() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) QSKIP("нативная платформа");
        Application app(dir_.path() + "/tray"); QString error;
        QVERIFY(app.createPasswordVault("tray-password-1", &error));
        ui::TrayController tray(app);
        QVERIFY(!tray.toolTip().isEmpty());
        QVERIFY(tray.toolTip().contains(QStringLiteral("разблокировано")));
        app.lock();
        QVERIFY(tray.toolTip().contains(QStringLiteral("заблокировано")));
        tray.notify(QStringLiteral("проверочное уведомление")); // не выбрасывает секреты — только текст
    }

    // Настоящая вставка: секрет копируется в реальный буфер и Ctrl+V отправляется в живое окно.
    void pasteIntoRealWindowJourney() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) QSKIP("нативная платформа");
        ClipboardGuard guard;
        QVERIFY(guard.copySecret(QStringLiteral("live-paste-777"), nullptr));
        QLineEdit target; target.resize(240, 32); target.show(); target.raise(); target.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&target));
        QVERIFY(activateTestReceiver(reinterpret_cast<HWND>(target.winId()),GetCurrentProcessId()));
        QTest::qWait(150);
        const bool focused = GetForegroundWindow() == reinterpret_cast<HWND>(target.winId());
        if (!focused) QSKIP("окно не получило фокус в этой среде (посторонний topmost)");
        paste::sendCtrlV();
        QTRY_COMPARE(target.text(), QStringLiteral("live-paste-777")); // SendInput дошёл до поля
        guard.triggerClearNowForTest();
    }
#endif
};
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--main-window-demo"))) {
        QTemporaryDir dir; Application vault(dir.path()); QString error;
        if (!vault.createPasswordVault("test-password-123", &error)) return 2;
        const auto project = QUuid::createUuid(), group = QUuid::createUuid();
        if (!vault.model()->addNode(project, {}, QStringLiteral("Разработка")).isEmpty()
            || !vault.model()->addNode(group, project, QStringLiteral("GitVerse")).isEmpty()) return 3;
        for (int i = 0; i < 6; ++i) {
            model::Entry entry; entry.id = QUuid::createUuid(); entry.groupId = i < 3 ? group : project;
            entry.title = i % 3 == 0 ? QStringLiteral("GitVerse API") : i % 3 == 1 ? QStringLiteral("Облачная консоль") : QStringLiteral("Production deploy");
            entry.type = model::EntryType(i % 3); entry.favorite = i < 2; entry.reverify = i == 0;
            entry.token = "synthetic-demo-token"; entry.login = "synthetic-login"; entry.password = "synthetic-password";
            entry.sshPrivate = QByteArray("synthetic-private-key"); entry.sshPassphrase = "synthetic-phrase";
            entry.tags = {QStringLiteral("DEV"), QStringLiteral("Рабочий")};
            entry.note = QStringLiteral("Вымышленные данные для проверки интерфейса. Реальные секреты не загружены.");
            entry.url = "example.test"; entry.created = entry.modified = QDateTime::currentDateTimeUtc();
            if (i == 1) entry.expiresAt = QDateTime::currentDateTimeUtc().addDays(3);
            if (!vault.model()->setEntry(entry).isEmpty()) return 4;
        }
        ui::theme::apply(app.arguments().contains("--dark") ? ui::theme::Mode::Dark : ui::theme::Mode::Light);
        QSettings preferences(dir.filePath("demo-ui.ini"), QSettings::IniFormat);
        ui::MainWindow window(vault, nullptr, &preferences);
        window.setWindowTitle(QStringLiteral("TokenBucket — вымышленные данные"));
        IdleTimer idle(300000); window.setIdleTimer(&idle);
        QObject::connect(&idle, &IdleTimer::idleTimeout, &vault, &Application::lock);
        QObject::connect(&window, &ui::MainWindow::closing, &app, &QCoreApplication::quit);
        window.resize(app.arguments().contains("--compact") ? QSize(960, 700) : QSize(1240, 800)); window.show();
        const auto captureIndex = app.arguments().indexOf("--capture");
        if (captureIndex >= 0 && captureIndex + 1 < app.arguments().size()) {
            const auto path = app.arguments().at(captureIndex + 1);
            QTimer::singleShot(800, &window, [&window, path] { QCoreApplication::exit(window.grab().save(path) ? 0 : 5); });
        }
        return app.exec();
    }
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--quick-paste-demo"))) {
        QTemporaryDir dir; Application vault(dir.path()); QString error;
        if (!vault.createPasswordVault("test-password-123", &error)) return 2;
        const auto group = QUuid::createUuid();
        vault.model()->addNode(group, {}, "Demo / synthetic data only");
        for (int type = 0; type < 2; ++type) {
            model::Entry e; e.id = QUuid::createUuid(); e.groupId = group;
            e.type = type ? model::EntryType::Credentials : model::EntryType::ApiToken;
            e.title = type ? "Demo login" : "Demo token";
            e.login = "synthetic-login"; e.password = "demo-password-777"; e.token = "synthetic-token";
            if (!vault.model()->setEntry(e).isEmpty()) return 3;
        }
        ui::theme::apply(app.arguments().contains("--dark") ? ui::theme::Mode::Dark : ui::theme::Mode::Light);
        ClipboardGuard clip; platform::QuickPaste quick(vault, clip, nullptr);
        if (!quick.start(QKeySequence("Ctrl+Alt+F10"), &error)) return 4;
        QLabel control(QStringLiteral("Только вымышленные данные. Ctrl+Alt+F10 → Demo.\nЗакройте это окно, чтобы завершить тестовый стенд."));
        control.setWindowTitle(QStringLiteral("TokenBucket — тестовый стенд")); control.resize(550, 100); control.show();
        QTimer search; search.setInterval(100);
        QObject::connect(&search, &QTimer::timeout, &control, [] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* panel = qobject_cast<ui::MiniPanel*>(widget); panel && panel->isVisible())
                    if (auto* field = panel->findChild<QLineEdit*>("miniSearch"); field && field->text().isEmpty()) field->setText("Demo");
        });
        search.start();
        return app.exec();
    }
    if (app.arguments().contains(QStringLiteral("--paste-receiver"))) {
        QLineEdit receiver;
        receiver.setWindowTitle(QStringLiteral("TokenBucket — тестовый получатель"));
        receiver.resize(440, 60); receiver.show(); receiver.raise(); receiver.activateWindow();
        QObject::connect(&receiver, &QLineEdit::textChanged, &receiver, [](const QString& value) {
            std::printf("TEXT %s\n", value.toUtf8().toBase64().constData()); std::fflush(stdout);
        });
        QObject::connect(&receiver, &QLineEdit::returnPressed, &receiver, [] { std::printf("RETURN\n"); std::fflush(stdout); });
        std::printf("READY %llu %lu\n", static_cast<unsigned long long>(receiver.winId()), GetCurrentProcessId()); std::fflush(stdout);
        return app.exec();
    }
#endif
    TestE2E test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_e2e.moc"
