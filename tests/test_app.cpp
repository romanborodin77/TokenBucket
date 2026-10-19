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
#include "../src/app/Application.h"
#include "../src/ui/SetupDialog.h"
#include "../src/ui/UnlockDialog.h"
#include "../src/vault/Container.h"
#include "../src/vault/Crypto.h"
using namespace tb;
class TestApp : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
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
#endif
};
QTEST_MAIN(TestApp)
#include "test_app.moc"
