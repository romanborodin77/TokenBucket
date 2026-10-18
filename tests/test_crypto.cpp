#include <QtTest>
#include "../src/vault/Crypto.h"
using namespace tb;
class TestCrypto : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(crypto::init()); }
    void deriveIsDeterministicAndSaltSensitive() {
        QByteArray pw("корректный пароль 123");
        unsigned char s1[16] = {1}, s2[16] = {2};
        SecureKey a1, a2, b;
        QVERIFY(crypto::deriveKey(pw, s1, a1));
        QVERIFY(crypto::deriveKey(pw, s1, a2));
        QVERIFY(crypto::deriveKey(pw, s2, b));
        QVERIFY(a1 == a2);       // тот же пароль+salt => тот же ключ
        QVERIFY(!(a1 == b));     // другой salt => другой ключ
    }
    void sealOpenRoundtrip() {
        SecureKey k; unsigned char salt[16] = {7};
        QVERIFY(crypto::deriveKey(QByteArray("пароль-пример-1"), salt, k));
        crypto::Blob b;
        QVERIFY(crypto::seal(k, "aad", "секретные данные", b));
        QByteArray out;
        QVERIFY(crypto::open(k, "aad", b, out));
        QCOMPARE(out, QByteArray("секретные данные"));
    }
    void wrongKeyOrAadFails() {
        SecureKey k1, k2; unsigned char salt[16] = {7};
        QVERIFY(crypto::deriveKey(QByteArray("пароль-пример-1"), salt, k1));
        QVERIFY(crypto::deriveKey(QByteArray("другой-пароль-22"), salt, k2));
        crypto::Blob b;
        QVERIFY(crypto::seal(k1, "aad", "данные", b));
        QByteArray out;
        QVERIFY(!crypto::open(k2, "aad", b, out));
        QVERIFY(!crypto::open(k1, "другой-aad", b, out));
    }
    void tamperedCiphertextFails() {
        SecureKey k; unsigned char salt[16] = {7};
        QVERIFY(crypto::deriveKey(QByteArray("пароль-пример-1"), salt, k));
        crypto::Blob b;
        QVERIFY(crypto::seal(k, "aad", "данные", b));
        b.ct[0] = b.ct[0] ^ 0x01;
        QByteArray out;
        QVERIFY(!crypto::open(k, "aad", b, out));
    }
    void noncesAreUnique() {
        SecureKey k; unsigned char salt[16] = {7};
        QVERIFY(crypto::deriveKey(QByteArray("пароль-пример-1"), salt, k));
        crypto::Blob a, b;
        QVERIFY(crypto::seal(k, "aad", "x", a));
        QVERIFY(crypto::seal(k, "aad", "x", b));
        QVERIFY(memcmp(a.nonce, b.nonce, sizeof(a.nonce)) != 0);
    }
};
QTEST_MAIN(TestCrypto)
#include "test_crypto.moc"
