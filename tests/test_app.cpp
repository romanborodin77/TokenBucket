#include <QtTest>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QSemaphore>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QDir>
#include <QScopeGuard>
#include "../src/app/Application.h"
#include "../src/ui/SetupDialog.h"
#include "../src/ui/UnlockDialog.h"
#include "../src/vault/Container.h"
#include "../src/vault/Crypto.h"
#include "FaultFileOps.h"
using namespace tb;
class TestApp : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    static QByteArray readFile(const QString& path) {
        QFile file(path); if(!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
    }
private slots:
    void backupUsesItsOldPasswordAndSafetyUsesCurrentPassword() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("valid-password",&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Original").isEmpty()); QVERIFY(app.save(&error));
        QVERIFY(app.switchToPassword("new-valid-password",&error));
        const auto current=readFile(app.storePathForTest()); QVERIFY(!current.isEmpty());
        QVERIFY(!app.prepareBackupRestore("new-valid-password",&error));
        QCOMPARE(readFile(app.storePathForTest()),current); QVERIFY(app.checkPassword("new-valid-password",&error));
        QVERIFY(app.prepareBackupRestore("valid-password",&error));
        const auto result=app.restorePreparedBackup(); QVERIFY2(result.committed,qPrintable(result.error));
        QVERIFY(!app.unlock("new-valid-password",&error)); QVERIFY(app.unlock("valid-password",&error));
        const auto safety=readFile(result.safetyPath); QCOMPARE(safety,current);
        QTemporaryDir recovered; QVERIFY(store::ensurePrivateDir(recovered.path()));
        QVERIFY(store::realFileOps().writeAll(recovered.filePath("vault.tb"),safety));
        Application safetyApp(recovered.path()); QVERIFY(safetyApp.unlock("new-valid-password",&error));
        QCOMPARE(safetyApp.model()->nodes().size(),1); QVERIFY(safetyApp.save(&error));
    }
    void backupPreparationFailures_data() {
        QTest::addColumn<int>("fault");
        const char* names[]={"wrong-password","damaged-aead","invalid-cbor","oversize","bad-header","backup-swapped",
            "explicit-cancel","cancel-during-prepare","lock-during-prepare","lock-after-prepare","busy-reentry","main-swapped"};
        for(int i=0;i<int(std::size(names));++i) QTest::newRow(names[i])<<i;
    }
    void backupPreparationFailures() {
        QFETCH(int,fault);
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("valid-password",&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Original").isEmpty()); QVERIFY(app.save(&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Newer").isEmpty()); QVERIFY(app.save(&error));
        const auto path=app.storePathForTest(),backup=path+".bak";
        const auto before=readFile(path),model=app.model()->toCbor(); QVERIFY(!before.isEmpty());
        const auto replace=[&](const QString& file,const QByteArray& bytes) {
            return QFile::remove(file) && store::realFileOps().writeAll(file,bytes);
        };
        if(fault==1) {auto damaged=readFile(backup); damaged[damaged.size()-1]^=char(1); QVERIFY(replace(backup,damaged));}
        if(fault==2) {
            const auto bad=container::packPassword("valid-password",QByteArray(1,char(0xff)),nullptr); QVERIFY(!bad.isEmpty()); QVERIFY(replace(backup,bad));
        }
        if(fault==3) {QFile file(backup); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(container::MaxFileBytes+1));}
        if(fault==4) QVERIFY(replace(backup,"bad-header"));
        if(fault==3 || fault==4) {
            QCOMPARE(app.backupMode(&error),Application::Mode::None); QVERIFY(!error.isEmpty());
        }
        bool timerRan=false;
        if(fault==7 || fault==8 || fault==10) QTimer::singleShot(0,&app,[&] {
            timerRan=true;
            if(fault==7) app.cancelBackupRestore();
            else if(fault==8) app.lock();
            else {
                QString nested; QVERIFY(!app.prepareBackupRestore("valid-password",&nested)); QVERIFY(!nested.isEmpty());
                QCOMPARE(app.backupMode(&nested),Application::Mode::None); QVERIFY(!nested.isEmpty());
                QVERIFY(!app.restorePreparedBackup().committed);
            }
        });
        const bool prepared=app.prepareBackupRestore(fault==0?QByteArray("wrong-password-789"):QByteArray("valid-password"),&error);
        if(fault==7 || fault==8 || fault==10) QVERIFY(timerRan);
        const bool shouldPrepare=fault==5 || fault==6 || fault==9 || fault==10 || fault==11;
        QCOMPARE(prepared,shouldPrepare);
        if(!prepared) QVERIFY(!error.isEmpty());
        if(fault==10) {
            QCOMPARE(app.model()->toCbor(),model); QCOMPARE(readFile(path),before);
            QVERIFY(app.checkPassword("valid-password",&error)); app.cancelBackupRestore();
        }
        if(fault==5) {auto changed=readFile(backup); changed[changed.size()-1]^=char(1); QVERIFY(replace(backup,changed));}
        if(fault==6) app.cancelBackupRestore();
        if(fault==9) app.lock();
        QByteArray expected=before;
        if(fault==11) {expected[expected.size()-1]^=char(1); QVERIFY(replace(path,expected));}
        const auto result=app.restorePreparedBackup(); QVERIFY(!result.committed); QVERIFY(!result.error.isEmpty());
        QCOMPARE(readFile(path),expected); QVERIFY(!app.busy());
        if(fault==8 || fault==9) {
            QCOMPARE(app.gate(),Application::Gate::NeedUnlock); QVERIFY(app.model()->nodes().isEmpty());
            QVERIFY(app.unlock("valid-password",&error));
        } else {QCOMPARE(app.gate(),Application::Gate::Ready); QCOMPARE(app.model()->toCbor(),model);}
        if(fault==0 || fault==6 || fault==7 || fault==8 || fault==9) {
            QVERIFY(app.prepareBackupRestore("valid-password",&error)); QVERIFY(app.restorePreparedBackup().committed);
        }
    }
    void backupCommitFailures_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("safety-write")<<0; QTest::newRow("rename")<<1;
        QTest::newRow("lock-before-candidate")<<2; QTest::newRow("lock-during-rename")<<3;
        QTest::newRow("cancel-during-rename")<<4; QTest::newRow("post-commit-corruption")<<5; QTest::newRow("success")<<6;
    }
    void backupCommitFailures() {
        QFETCH(int,fault);
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
        QVERIFY(app.prepareBackupRestore("valid-password",&error));
        QCOMPARE(readFile(app.storePathForTest()),before); QCOMPARE(app.model()->nodes().size(),2);
        ops=FaultOps{}; bool callbackRan=false,callbackOnWorker=false;
        const auto interrupt=[&] {
            callbackRan=true; callbackOnWorker=QThread::currentThread()!=qApp->thread();
            QVERIFY(callbackOnWorker);
            QVERIFY(QMetaObject::invokeMethod(&app,[&] {if(fault==4) app.cancelBackupRestore(); else app.lock();},Qt::BlockingQueuedConnection));
        };
        if(fault==0) ops.failWriteCall=1;
        if(fault==1) ops.failRenameCall=1;
        if(fault==2) ops.beforeWrite=[&](const QString&) {if(ops.writeCalls==2) interrupt();};
        if(fault==3 || fault==4) ops.beforeRename=[&](const QString&,const QString&) {interrupt();};
        if(fault==5) ops.afterRename=[&](const QString&,const QString& path) {
            QVERIFY(QFile::remove(path)); QVERIFY(store::realFileOps().writeAll(path,"post-commit-corruption"));
        };
        const auto result=app.restorePreparedBackup(); const bool committed=fault>=3;
        QCOMPARE(result.committed,committed); QCOMPARE(result.error.isEmpty(),fault==6); QVERIFY(!app.busy());
        if(fault>=2 && fault<=4) {QVERIFY(callbackRan); QVERIFY(callbackOnWorker);}
        QCOMPARE(readFile(app.storePathForTest()+".bak"),backup);
        QCOMPARE(readFile(app.storePathForTest()),fault==5?QByteArray("post-commit-corruption"):committed?backup:before);
        QCOMPARE(app.gate(),fault>=2?Application::Gate::NeedUnlock:Application::Gate::Ready);
        if(fault>=2) QVERIFY(app.model()->nodes().isEmpty());
        if(!result.safetyPath.isEmpty()) QCOMPARE(readFile(result.safetyPath),before);
        ops=FaultOps{};
        if(fault==0 || fault==1 || fault==2) {
            if(app.gate()==Application::Gate::NeedUnlock) QVERIFY(app.unlock("valid-password",&error));
            QVERIFY(app.prepareBackupRestore("valid-password",&error)); QVERIFY(app.restorePreparedBackup().committed);
        }
        if(fault!=5) {QVERIFY(app.unlock("valid-password",&error)); QCOMPARE(app.model()->nodes().size(),1); QVERIFY(app.save(&error));}
    }
#ifdef Q_OS_WIN
    void canceledHelloBackupCannotRecreateAuthorization_data() {
        QTest::addColumn<bool>("lockSession"); QTest::newRow("cancel-only")<<false; QTest::newRow("lock")<<true;
    }
    void canceledHelloBackupCannotRecreateAuthorization() {
        QFETCH(bool,lockSession);
        QTemporaryDir dir; QSemaphore release;
        std::atomic<bool> block{false},entered{false},onWorker{false};
        Application app(dir.path(),nullptr,[&](void*,const QString&) {
            onWorker=QThread::currentThread()!=qApp->thread();
            if(block) {
                if(!onWorker) return platform::hello::Result::Failed;
                entered=true; release.acquire();
            }
            return platform::hello::Result::Verified;
        });
        QString error; QVERIFY(app.createHelloVault(&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Original").isEmpty()); QVERIFY(app.save(&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Newer").isEmpty()); QVERIFY(app.save(&error));
        const auto before=readFile(app.storePathForTest()); block=true;
        QTimer driver; driver.setInterval(1);
        connect(&driver,&QTimer::timeout,&app,[&] {
            if(entered) {driver.stop(); if(lockSession) app.lock(); else app.cancelBackupRestore(); release.release();}
        });
        driver.start(); QVERIFY(!app.prepareHelloBackupRestore(nullptr,&error)); driver.stop(); block=false;
        QVERIFY(entered); QVERIFY(onWorker); QVERIFY(!error.isEmpty()); QVERIFY(!app.busy());
        QVERIFY(!app.restorePreparedBackup().committed); QCOMPARE(readFile(app.storePathForTest()),before);
        QCOMPARE(app.gate(),lockSession?Application::Gate::NeedUnlock:Application::Gate::Ready);
        if(lockSession) QVERIFY(app.unlockHello(&error));
        QVERIFY(app.prepareHelloBackupRestore(nullptr,&error)); QVERIFY(app.restorePreparedBackup().committed);
        QVERIFY(app.unlockHello(&error)); QCOMPARE(app.model()->nodes().size(),1);
    }
    void backupCanUseDifferentUnlockMode_data() {
        QTest::addColumn<bool>("helloBackup"); QTest::newRow("hello-copy-password-current")<<true; QTest::newRow("password-copy-hello-current")<<false;
    }
    void backupCanUseDifferentUnlockMode() {
        QFETCH(bool,helloBackup);
        QTemporaryDir dir; std::atomic<platform::hello::Result> answer{platform::hello::Result::Verified};
        Application app(dir.path(),nullptr,[&](void*,const QString&) {return answer.load();}); QString error;
        if(helloBackup) QVERIFY(app.createHelloVault(&error)); else QVERIFY(app.createPasswordVault("valid-password",&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Original").isEmpty()); QVERIFY(app.save(&error));
        if(helloBackup) QVERIFY(app.switchToPassword("valid-password",&error)); else QVERIFY(app.switchToHello(&error));
        QCOMPARE(app.backupMode(&error),helloBackup?Application::Mode::Hello:Application::Mode::Password);
        const auto before=readFile(app.storePathForTest());
        if(helloBackup) {
            QVERIFY(!app.prepareBackupRestore("valid-password",&error)); answer=platform::hello::Result::Canceled;
            QVERIFY(!app.prepareHelloBackupRestore(nullptr,&error)); QCOMPARE(readFile(app.storePathForTest()),before);
            answer=platform::hello::Result::Verified; QVERIFY(app.prepareHelloBackupRestore(nullptr,&error));
        } else {QVERIFY(!app.prepareHelloBackupRestore(nullptr,&error)); QVERIFY(app.prepareBackupRestore("valid-password",&error));}
        const auto result=app.restorePreparedBackup(); QVERIFY2(result.committed,qPrintable(result.error)); QVERIFY(result.error.isEmpty());
        QCOMPARE(app.unlockMode(),helloBackup?Application::Mode::Hello:Application::Mode::Password);
        if(helloBackup) QVERIFY(app.unlockHello(&error)); else QVERIFY(app.unlock("valid-password",&error));
        QCOMPARE(app.model()->nodes().size(),1); QVERIFY(app.save(&error));
    }
#endif
    void authenticatedBackupRestoresAndRequiresUnlock() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid();
        QVERIFY(app.model()->addNode(group,{},"Original").isEmpty()); QVERIFY(app.save(&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Newer").isEmpty()); QVERIFY(app.save(&error));
        QCOMPARE(app.backupMode(&error),Application::Mode::Password);
        QVERIFY(!app.restorePreparedBackup().committed);
        QVERIFY2(app.prepareBackupRestore("valid-password",&error),qPrintable(error));
        const auto result=app.restorePreparedBackup();
        QVERIFY2(result.committed,qPrintable(result.error)); QVERIFY(result.error.isEmpty());
        QCOMPARE(app.gate(),Application::Gate::NeedUnlock); QVERIFY(app.model()->nodes().isEmpty());
        QVERIFY(app.unlock("valid-password",&error));
        QCOMPARE(app.model()->nodes().size(),1); QCOMPARE(app.model()->nodes().value(group).name,QStringLiteral("Original"));
        QVERIFY(app.save(&error));
    }
    void initTestCase() {
#ifdef Q_OS_WIN
        qputenv("QT_QPA_FONTDIR", qEnvironmentVariable("WINDIR").toUtf8() + "/Fonts");
#endif
        QVERIFY(crypto::init());
    }
    void passwordFlow() {
        QByteArray pw12("двенадцать-байт"); // >12 байт UTF-8
        QVERIFY(pw12.size() >= 12);
        Application app(dir_.path());
        QCOMPARE(app.gate(), Application::Gate::NeedSetup);
        QVERIFY(!Application::validPassword(QByteArray("short")));
        QVERIFY(!Application::validPassword(QByteArray(1025, 'x')));
        QVERIFY(Application::validPassword(pw12));
        QString err;
        QVERIFY(app.createPasswordVault(pw12, &err));
        QCOMPARE(app.gate(), Application::Gate::Ready);
        QVERIFY(app.model()->addNode(QUuid::createUuid(), QUuid(), "Проект").isEmpty());
        QVERIFY(app.save(&err));
        const auto generation = app.generation(); app.lock();
        QCOMPARE(app.generation(), generation + 1);
        QVERIFY(app.model()->nodes().isEmpty());
        QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
        QVERIFY(!app.unlock(QByteArray("неверный-пароль!"), &err));  // 17 байт, валидная длина
        QVERIFY(!err.isEmpty());
        QVERIFY(app.unlock(pw12, &err));
        QCOMPARE(app.gate(), Application::Gate::Ready);
        QVERIFY(app.model()->nodes().size() == 1);
        QVERIFY(app.save(&err));
    }
    void secondInstanceLock() {
        QString err; const auto path = dir_.path() + "/instance";
        auto first = Application::acquireInstanceLock(path, &err); QVERIFY(first);
        QVERIFY(!Application::acquireInstanceLock(path, &err)); QVERIFY(!err.isEmpty());
        first.reset(); QVERIFY(Application::acquireInstanceLock(path, &err));
    }
    void blockedOperationsAndPasswordBoundaries() {
        Application app(dir_.path() + "/gates"); QString err;
        QVERIFY(!app.unlock("valid-password", &err)); QVERIFY(!app.save(&err));
        QVERIFY(Application::validPassword(QByteArray(12, ' ')));
        QVERIFY(!Application::validPassword(QByteArray::fromHex("ffffffffffffffffffffffff")));
        QVERIFY(app.createPasswordVault("valid-password", &err));
        QVERIFY(!app.createPasswordVault("other-password", &err));
        app.lock(); QVERIFY(!app.save(&err)); QVERIFY(!app.createPasswordVault("other-password", &err));
        QVERIFY(app.unlock("valid-password", &err));
    }
    void lockDuringKdfCannotReopen() {
        Application app(dir_.path() + "/cancel"); QString err;
        QVERIFY(app.createPasswordVault("valid-password", &err)); app.lock();
        bool responsive = false, refusedReentry = false;
        QTimer::singleShot(0, &app, [&] {
            responsive = true; QString nestedError;
            refusedReentry = !app.unlock("valid-password", &nestedError);
            app.lock();
        });
        QVERIFY(!app.unlock("valid-password", &err));
        QVERIFY(responsive); QVERIFY(refusedReentry);
        QCOMPARE(app.gate(), Application::Gate::NeedUnlock); QCOMPARE(app.mode(), Application::Mode::None);
        QVERIFY(app.model()->nodes().isEmpty());
        QVERIFY(app.unlock("valid-password", &err));
    }
    void corruptModelCannotUnlock() {
        const auto path = dir_.path() + "/bad-model";
        store::VaultStore store(path); QString err;
        QVERIFY(store.save(container::packPassword("valid-password", "not-cbor", nullptr), &err));
        Application app(path);
        QVERIFY(!app.unlock("valid-password", &err));
        QCOMPARE(app.gate(), Application::Gate::NeedUnlock); QVERIFY(app.model()->nodes().isEmpty());
    }
    void oversizedModelCannotChangeSavedFile() {
        Application app(dir_.path() + "/oversized-model"); QString err;
        QVERIFY(app.createPasswordVault("valid-password", &err));
        const auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "Проект").isEmpty());
        model::Entry entry; entry.id = QUuid::createUuid(); entry.groupId = group;
        entry.note = QString(int(container::MaxPlainBytes), QChar('x')); QVERIFY(app.model()->setEntry(entry).isEmpty());
        QVERIFY(!app.save(&err)); QVERIFY(err.contains(QStringLiteral("64")));
        app.lock(); QVERIFY(app.unlock("valid-password", &err)); QVERIFY(app.model()->nodes().isEmpty());
    }
    void setupValidatesAndClearsSecrets() {
        SetupDialog dialog(false);
        dialog.show(); dialog.adjustSize();
        QVERIFY(!dialog.grab().isNull());
        QVERIFY(dialog.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/setup-preview.png")));
        auto* first = dialog.findChild<QLineEdit*>(QStringLiteral("masterPassword"));
        auto* second = dialog.findChild<QLineEdit*>(QStringLiteral("confirmPassword"));
        auto* ok = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        QVERIFY(first); QVERIFY(second);
        QCOMPARE(first->echoMode(), QLineEdit::Password);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        first->setText(QStringLiteral("short")); second->setText(QStringLiteral("short")); ok->click();
        QCOMPARE(accepted.size(), 0);
        first->setText(QStringLiteral("пароль-длинный")); second->setText(QStringLiteral("другой-пароль")); ok->click();
        QCOMPARE(accepted.size(), 0);
        second->setText(first->text()); ok->click(); QCOMPARE(accepted.size(), 1);
        QCOMPARE(dialog.masterPassword(), first->text().toUtf8());
        dialog.clearSecrets(); QVERIFY(first->text().isEmpty()); QVERIFY(second->text().isEmpty());
        UnlockDialog unlock(UnlockDialog::Kind::Password);
        auto* field = unlock.findChild<QLineEdit*>(QStringLiteral("unlockPassword")); QVERIFY(field);
        field->setText(QStringLiteral("  пароль-длинный  "));
        QCOMPARE(unlock.password(), QByteArray("  пароль-длинный  "));
        unlock.clearSecrets(); QVERIFY(unlock.password().isEmpty());
    }
    void exportImportRoundtrip() {
        Application src(dir_.path() + "/export-src"); QString err;
        QVERIFY(src.createPasswordVault("пароль-источника", &err));
        QVERIFY(src.model()->addNode(QUuid::createUuid(), {}, "Проект A").isEmpty());
        const QByteArray exported = src.exportContainer("пароль-экспорта-1", &err);
        QVERIFY(!exported.isEmpty()); QVERIFY(err.isEmpty());
        QCOMPARE(container::readMode(exported), container::ModePassword);
        QFile f(dir_.filePath("export.tb")); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(exported), qint64(exported.size())); f.close();
        Application dst(dir_.filePath("dst"));
        QVERIFY(dst.importContainer(exported, "пароль-экспорта-1", "новый-пароль-123", &err));
        QCOMPARE(dst.mode(), Application::Mode::Password);
        QCOMPARE(dst.model()->nodes().size(), 1);
        dst.lock(); QVERIFY(dst.unlock("новый-пароль-123", &err));
        QCOMPARE(dst.model()->nodes().size(), 1);
    }
    void wrongExportPasswordFails() {
        Application src(dir_.path() + "/export-src-2"); QString err;
        QVERIFY(src.createPasswordVault("пароль-источника", &err));
        const QByteArray exported = src.exportContainer("пароль-экспорта-1", &err);
        QVERIFY(!exported.isEmpty());
        Application dst(dir_.filePath("dst2"));
        QVERIFY(!dst.importContainer(exported, "неверный-пароль!!", "новый-пароль-123", &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(dst.gate(), Application::Gate::NeedSetup);
        QVERIFY(!QFile::exists(dst.storePathForTest()));
    }
    void exportRequiresReadyAndValidPassword() {
        Application app(dir_.path() + "/export-gates"); QString err;
        QVERIFY(app.createPasswordVault("valid-password", &err));
        QVERIFY(app.exportContainer("short", &err).isEmpty()); QVERIFY(!err.isEmpty());
        app.lock();
        QVERIFY(app.exportContainer("valid-password", &err).isEmpty()); QVERIFY(!err.isEmpty());
    }
#ifdef Q_OS_WIN
    void helloVaultFlow() {
        std::atomic<platform::hello::Result> result{platform::hello::Result::Verified};
        std::atomic<int> requests{0};
        Application app(dir_.path() + "/h", nullptr, [&](void*, const QString&) { ++requests; return result.load(); });
        QString err;
        QVERIFY(app.createHelloVault(&err));
        QCOMPARE(app.mode(), Application::Mode::Hello);
        app.lock();
        QVERIFY(QFile::exists(app.storePathForTest()));
        result = platform::hello::Result::Canceled;
        QVERIFY(!app.unlockHello(&err)); QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
        result = platform::hello::Result::Failed;
        QVERIFY(!app.unlockHello(&err)); QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
        result = platform::hello::Result::Verified;
        QVERIFY(app.unlockHello(&err)); QVERIFY(app.save(&err));
        QCOMPARE(requests.load(), 4);
    }
    void lateHelloSuccessDoesNotUnlock() {
        QSemaphore release;
        std::atomic<bool> wait{false}, worker{false}, entered{false};
        Application app(dir_.path() + "/late-h", nullptr, [&](void*, const QString&) {
            worker = QThread::currentThread() != qApp->thread();
            if (wait) { entered = true; release.acquire(); }
            return platform::hello::Result::Verified;
        });
        QString err; QVERIFY(app.createHelloVault(&err)); app.lock(); wait = true;
        QTimer timer; timer.setInterval(1);
        connect(&timer, &QTimer::timeout, &app, [&] {
            if (entered) { timer.stop(); app.lock(); release.release(); }
        });
        timer.start();
        QVERIFY(!app.unlockHello(&err)); QVERIFY(worker); QVERIFY(entered);
        QCOMPARE(app.gate(), Application::Gate::NeedUnlock); QCOMPARE(app.mode(), Application::Mode::None);
    }
    void nativeHelloWithoutWindowCannotCreateVault() {
        Application app(dir_.path() + "/no-window"); QString err;
        QVERIFY(!app.createHelloVault(&err)); QVERIFY(!err.isEmpty());
        QVERIFY(!QFile::exists(app.storePathForTest()));
    }
    void verificationDoesNotReplaceOpenState() {
        Application app(dir_.path() + "/verify-password"); QString error;
        QVERIFY(app.createPasswordVault("valid-password", &error));
        QVERIFY(!app.checkPassword("wrong-password", &error)); QVERIFY(app.checkPassword("valid-password", &error));
        QVERIFY(app.save(&error));
        Application hello(dir_.path() + "/verify-hello", nullptr, [](void*, const QString&) { return platform::hello::Result::Verified; });
        QVERIFY(hello.createHelloVault(&error)); QVERIFY(hello.verifyHello(nullptr, &error)); QVERIFY(hello.save(&error));
    }
    void switchPasswordToHelloAndBack() {
        Application app(dir_.path() + "/switch-modes", nullptr, [](void*, const QString&) { return platform::hello::Result::Verified; }); QString err;
        QVERIFY(app.createPasswordVault("пароль-переключения", &err));
        QVERIFY(app.model()->addNode(QUuid::createUuid(), {}, "P").isEmpty());
        QVERIFY(app.switchToHello(&err));
        QCOMPARE(app.mode(), Application::Mode::Hello);
        QCOMPARE(app.model()->nodes().size(), 1);
        QByteArray onDisk;
        store::VaultStore disk(dir_.path() + "/switch-modes");
        QCOMPARE(disk.loadBytes(disk.mainPath(), &onDisk), store::VaultStore::Load::Ok);
        QCOMPARE(container::readMode(onDisk), container::ModeHello);
        app.lock();
        QVERIFY(app.unlockHello(&err));
        QCOMPARE(app.model()->nodes().size(), 1);
        QVERIFY(app.switchToPassword("новый-пароль-123", &err));
        QCOMPARE(app.mode(), Application::Mode::Password);
        QCOMPARE(disk.loadBytes(disk.mainPath(), &onDisk), store::VaultStore::Load::Ok);
        QCOMPARE(container::readMode(onDisk), container::ModePassword);
        app.lock(); QVERIFY(app.unlock("новый-пароль-123", &err));
        QCOMPARE(app.model()->nodes().size(), 1);
    }
    void switchRequiresReady() {
        Application app(dir_.path() + "/switch-gates"); QString err;
        QVERIFY(app.createPasswordVault("valid-password", &err));
        app.lock();
        QVERIFY(!app.switchToPassword("другой-пароль-123", &err)); QVERIFY(!err.isEmpty());
    }
#endif
};
QTEST_MAIN(TestApp)
#include "test_app.moc"
