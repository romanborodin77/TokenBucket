#include <QtTest>
#include "../src/vault/Crypto.h"
#include "../src/vault/PasswordGenerator.h"
#include <algorithm>
using namespace tb;
class TestCrypto : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(crypto::init()); }
    void generatorHonoursOptions() {
        const QString symbols = QStringLiteral("#!%+=-_@$*?.");
        for (int i = 0; i < 1000; ++i) {
            const auto p = crypto::generatePassword({8, true, true, true, true, true});
            QCOMPARE(p.size(), 8);
            QVERIFY(std::any_of(p.cbegin(), p.cend(), [](QChar c) { return c.isUpper(); }));
            QVERIFY(std::any_of(p.cbegin(), p.cend(), [](QChar c) { return c.isLower(); }));
            QVERIFY(std::any_of(p.cbegin(), p.cend(), [](QChar c) { return c.isDigit(); }));
            QVERIFY(std::any_of(p.cbegin(), p.cend(), [&](QChar c) { return symbols.contains(c); }));
            for (QChar c : QStringLiteral("0O1lI")) QVERIFY(!p.contains(c));
        }
        const auto digits = crypto::generatePassword({128, false, false, true, false, false});
        QCOMPARE(digits.size(), 128); QVERIFY(std::all_of(digits.cbegin(), digits.cend(), [](QChar c) { return c.isDigit(); }));
        QVERIFY(crypto::generatePassword({24, true, false, false, false, false}) != crypto::generatePassword({24, true, false, false, false, false}));
        QVERIFY(!crypto::generatorError({24, false, false, false, false, true}).isEmpty());
        QVERIFY(crypto::generatePassword({24, false, false, false, false, true}).isEmpty());
        QVERIFY(!crypto::generatorError({7, true, true, true, true, true}).isEmpty());
        QVERIFY(!crypto::generatorError({129, true, true, true, true, true}).isEmpty());
        QVERIFY(crypto::generatorError({8, true, true, true, true, false}).isEmpty());
    }
    void secureKeyMove() {
        SecureKey first; randombytes_buf(first.data(), SecureKey::Size);
        const QByteArray expected(reinterpret_cast<const char*>(first.data()), SecureKey::Size);
        SecureKey second(std::move(first));
        QCOMPARE(first.data(), nullptr);
        SecureKey third; third = std::move(second);
        QCOMPARE(second.data(), nullptr);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(third.data()), SecureKey::Size), expected);
    }
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
