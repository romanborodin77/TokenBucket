#include <QtTest>
#include <QTemporaryDir>
#include "../src/vault/Container.h"
#include "../src/vault/Crypto.h"
using namespace tb;
class TestContainer : public QObject {
    Q_OBJECT
private:
    QByteArray pack() {
        SecureKey k;
        QByteArray b = container::packPassword("пароль-мастера-1", "cbor-модель", &k);
        return b;
    }
private slots:
    void initTestCase() { QVERIFY(crypto::init()); }
    void independentDataKeys() {
        SecureKey a, b;
        QVERIFY(!container::packPassword("пароль-мастера-1", "x", &a).isEmpty());
        QVERIFY(!container::packPassword("пароль-мастера-1", "x", &b).isEmpty());
        QVERIFY(!(a == b));
    }
    void forgedWrapLength() {
        QByteArray f = pack();
        for (quint32 n : {0u, 47u, 49u, quint32(f.size()), 0xffffffffu}) {
            QByteArray broken = f;
            for (int i = 0; i < 4; ++i) broken[57 + i] = char(n >> (8 * i));
            QCOMPARE(container::openPassword(broken, "пароль-мастера-1").status, container::Status::BadFormat);
        }
    }
    void roundtripPassword() {
        QByteArray f = pack();
        QVERIFY(!f.isEmpty());
        QCOMPARE(container::readMode(f), container::ModePassword);
        auto o = container::openPassword(f, "пароль-мастера-1");
        QCOMPARE(o.status, container::Status::Ok);
        QCOMPARE(o.cbor, QByteArray("cbor-модель"));
    }
    void wrongPassword() {
        auto o = container::openPassword(pack(), "неверный-пароль!!");
        QCOMPARE(o.status, container::Status::WrongKey);
        QVERIFY(o.cbor.isEmpty());
    }
    void emptyFileAndHeaderOnly() {
        QCOMPARE(container::openPassword(QByteArray(), "x").status, container::Status::BadFormat);
        QByteArray f = pack();
        QCOMPARE(container::openPassword(f.left(60), "пароль-мастера-1").status, container::Status::BadFormat);
        QCOMPARE(container::openPassword(f.left(93), "пароль-мастера-1").status, container::Status::BadFormat);
    }
    void truncation() {
        QByteArray f = pack();
        for (qsizetype size = 0; size < f.size(); ++size)
            QCOMPARE(container::openPassword(f.left(size), "пароль-мастера-1").status, container::Status::BadFormat);
        QCOMPARE(container::openPassword(f + 'x', "пароль-мастера-1").status, container::Status::BadFormat);
    }
    void authenticatedHeader() {
        const QByteArray f = pack();
        for (int offset : {6, 7, 13, 17, 33, 61, 109, 133}) {
            QByteArray broken = f; broken[offset] = broken[offset] ^ 1;
            auto result = container::openPassword(broken, "пароль-мастера-1");
            QVERIFY(result.status != container::Status::Ok);
            QVERIFY(result.cbor.isEmpty());
        }
    }
    void passwordBoundaries() {
        for (int size : {0, 11, 1025})
            QVERIFY(container::packPassword(QByteArray(size, 'p'), "x", nullptr).isEmpty());
        for (int size : {12, 1024}) {
            const QByteArray pw(size, 'p');
            auto result = container::openPassword(container::packPassword(pw, {}, nullptr), pw);
            QCOMPARE(result.status, container::Status::Ok);
            QVERIFY(result.cbor.isEmpty());
        }
    }
    void tamperedHeader() {
        QByteArray f = pack();
        f[17] = f[17] ^ 0x01; // бит в salt: ломает и KEK, и AAD
        QVERIFY(container::openPassword(f, "пароль-мастера-1").status != container::Status::Ok);
    }
    void tamperedCiphertext() {
        QByteArray f = pack();
        f[f.size() - 1] = f[f.size() - 1] ^ 0x01;
        QVERIFY(container::openPassword(f, "пароль-мастера-1").status != container::Status::Ok);
    }
    void unsupportedVersionAndParams() {
        QByteArray f = pack();
        f[4] = 2; // version
        QCOMPARE(container::openPassword(f, "пароль-мастера-1").status, container::Status::Unsupported);
        f = pack();
        f[9] = 9; // opslimit
        QCOMPARE(container::openPassword(f, "пароль-мастера-1").status, container::Status::Unsupported);
    }
    void oversizeInput() {
        QByteArray big(int(container::MaxFileBytes) + 1, 'x');
        QCOMPARE(container::openPassword(big, "x").status, container::Status::TooLarge);
    }
    void oversizePlaintext() {
        SecureKey k;
        QByteArray big(int(container::MaxPlainBytes) + 1, 'y');
        QVERIFY(container::packPassword("пароль-мастера-1", big, &k).isEmpty());
    }
    void resealKeepsWrapAndReplacesBody() {
        SecureKey k;
        QByteArray f = container::packPassword("пароль-мастера-1", "первое", &k);
        QByteArray f2 = container::reseal(f, k, "второе");
        QVERIFY(!f2.isEmpty());
        QCOMPARE(f2.left(109), f.left(109)); // заголовок (61) и оболочка ключа (48)
        auto o = container::openPassword(f2, "пароль-мастера-1");
        QCOMPARE(o.status, container::Status::Ok);
        QCOMPARE(o.cbor, QByteArray("второе"));
        QVERIFY(o.dataKey == k);
        QVERIFY(f2.mid(109,24) != f.mid(109,24));
        const auto empty = container::openPassword(container::reseal(f2, k, {}), "пароль-мастера-1");
        QCOMPARE(empty.status, container::Status::Ok);
        QVERIFY(empty.cbor.isEmpty());
        QCOMPARE(container::reseal(QByteArray("мусор"), k, "x"), QByteArray());
    }
#ifdef Q_OS_WIN
    void helloRoundtripViaDpapi() {
        SecureKey k;
        QByteArray f = container::packHello("модель-hello", &k);
        QVERIFY(!f.isEmpty());
        QCOMPARE(container::readMode(f), container::ModeHello);
        auto o = container::openHello(f);
        QCOMPARE(o.status, container::Status::Ok);
        QCOMPARE(o.cbor, QByteArray("модель-hello"));
        // пароль к hello-контейнеру не применим
        QCOMPARE(container::openPassword(f, "что-угодно").status, container::Status::Unsupported);
    }
#endif
};
QTEST_MAIN(TestContainer)
#include "test_container.moc"
