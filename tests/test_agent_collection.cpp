#include <QtTest>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include "vault/AgentCollection.h"
#include "vault/Container.h"
#include "vault/Crypto.h"

using namespace tb;

namespace {
SecureKey ownerKey() {
    SecureKey key;
    randombytes_buf(key.data(), SecureKey::Size);
    return key;
}

QString collectionPath(const QTemporaryDir& dir) {
    return dir.filePath(QString::fromLatin1(collection::FileName));
}

QByteArray storedBytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
}

class TestAgentCollection : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(crypto::init()); }

    void lifecycleAndIsolation() {
        QTemporaryDir dir, other;
        QVERIFY(dir.isValid() && other.isValid());
        const auto owner = ownerKey(), wrongOwner = ownerKey();
        SecureBytes first, second, result;
        const auto exec = [&](const SecureBytes& token, const QString& op, const QString& name,
                              QByteArrayView value = {}) {
            return collection::execute(dir.path(), token.bytes(), op, name, value, result);
        };
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "entry", "x"), QString("not-found"));
        QCOMPARE(collection::issue(dir.path(), owner, first), QString());
        QCOMPARE(collection::issue(other.path(), owner, second), QString());
        QCOMPARE(exec(second, "read", "entry"), QString("unauthorized"));

        const QByteArray secret("test\0\xff entry secret", 19);
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "entry", secret), QString());
        QCOMPARE(collection::ownerCreate(dir.path(), wrongOwner, "blocked", "x"), QString("unauthorized"));
        QCOMPARE(exec(first, "read", "entry"), QString());
        QCOMPARE(result.bytes(), secret);
        QCOMPARE(exec(first, "create", "Entry", ""), QString());
        QCOMPARE(exec(first, "read", "Entry"), QString());
        QVERIFY(result.isEmpty());
        QCOMPARE(exec(first, "create", "entry", "new"), QString("already-exists"));
        QCOMPARE(exec(first, "replace", "missing", "x"), QString("not-found"));

        const auto path = collectionPath(dir);
        const auto snapshot = storedBytes(path);
        QVERIFY(!snapshot.isEmpty());
        QVERIFY(!snapshot.contains(secret));
        QVERIFY(!snapshot.contains(first.bytes()));
        QCOMPARE(collection::issue(dir.path(), wrongOwner, second), QString("unauthorized"));
        QVERIFY(second.isEmpty());
        QCOMPARE(collection::revoke(dir.path(), wrongOwner), QString("unauthorized"));
        QCOMPARE(storedBytes(path), snapshot);

        QCOMPARE(collection::revoke(dir.path(), owner), QString());
        QCOMPARE(exec(first, "read", "entry"), QString("unauthorized"));
        QVERIFY(result.isEmpty());
        const auto revoked = storedBytes(path);
        QVERIFY(revoked != snapshot);
        QCOMPARE(collection::revoke(dir.path(), owner), QString());
        QCOMPARE(storedBytes(path), revoked);
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "new", "late"), QString());
        QCOMPARE(collection::issue(dir.path(), owner, second), QString());
        QCOMPARE(exec(first, "read", "entry"), QString("unauthorized"));
        QCOMPARE(exec(second, "read", "entry"), QString());
        QCOMPARE(result.bytes(), secret);
        QCOMPARE(exec(second, "replace", "entry", "changed"), QString());
        QCOMPARE(exec(second, "read", "entry"), QString());
        QCOMPARE(result.bytes(), QByteArray("changed"));
        QCOMPARE(exec(second, "read", "new"), QString());
        QCOMPARE(result.bytes(), QByteArray("late"));
    }

    void contentionBoundsAndCorruption() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto owner = ownerKey();
        SecureBytes token, result, refused;
        QCOMPARE(collection::issue(dir.path(), owner, token), QString());
        const auto path = collectionPath(dir);
        const auto prior = storedBytes(path);
        QVERIFY(!prior.isEmpty());
        QLockFile lock(path + ".lock");
        QVERIFY(lock.tryLock());
        QCOMPARE(collection::issue(dir.path(), owner, refused), QString("busy"));
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "entry", "x"), QString("busy"));
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", "entry", {}, result), QString("busy"));
        QCOMPARE(storedBytes(path), prior);
        lock.unlock();

        QCOMPARE(collection::ownerCreate(dir.path(), owner, QString(129, QChar(0x00e9)), "x"),
                 QString("invalid-request"));
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "entry", QByteArray(32 * 1024 + 1, 'x')),
                 QString("invalid-request"));
        for (const auto& name : {QString(QChar(0xfeff)) + "leading", QString("control\nname"),
                                 QString::fromUtf8(QByteArray("embedded\0null", 13))}) {
            QCOMPARE(collection::ownerCreate(dir.path(), owner, name, "x"), QString("invalid-request"));
            QCOMPARE(collection::execute(dir.path(), token.bytes(), "create", name, "x", result),
                     QString("invalid-request"));
            QCOMPARE(collection::execute(dir.path(), token.bytes(), "replace", name, "x", result),
                     QString("invalid-request"));
            QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", name, {}, result),
                     QString("invalid-request"));
        }
        QCOMPARE(storedBytes(path), prior);
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", "well-formed", {}, result), QString("not-found"));

        auto corrupt = prior;
        corrupt[corrupt.size() - 1] = char(corrupt.back() ^ 1);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(corrupt), corrupt.size());
        file.close();
        QCOMPARE(collection::issue(dir.path(), owner, refused), QString("corrupt"));
        QCOMPARE(collection::revoke(dir.path(), owner), QString("corrupt"));
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "create", "entry", "x", result), QString("corrupt"));
        QVERIFY(refused.isEmpty() && result.isEmpty());
        QCOMPARE(storedBytes(path), corrupt);

        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.resize(5 * 1024 * 1024));
        file.close();
        QCOMPARE(collection::issue(dir.path(), owner, refused), QString("corrupt"));
        QCOMPARE(file.size(), qint64(5 * 1024 * 1024));
    }

    void interruptedRebindRecoversWithoutLosingWrites() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto owner = ownerKey(), successor = ownerKey();
        const auto mainPath = dir.filePath("vault.tb");
        const auto oldVault = container::packPassword("test-password-before", "synthetic", nullptr);
        const auto newVault = container::packPassword("test-password-after", "synthetic", nullptr);
        QVERIFY(!oldVault.isEmpty() && !newVault.isEmpty() && writeBytes(mainPath, oldVault));
        const auto pending = collectionPath(dir) + ".pending";
        SecureBytes oldToken, newToken, result;
        QCOMPARE(collection::issue(dir.path(), owner, oldToken), QString());
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "shared", "initial"), QString());

        struct Interrupted {};
        try {
            collection::rebindOwner(dir.path(), owner, successor, oldVault, newVault, [] { throw Interrupted{}; });
            QFAIL("Expected an interruption before the main write");
        } catch (const Interrupted&) {}
        QVERIFY(QFileInfo::exists(pending));
        QCOMPARE(collection::execute(dir.path(), oldToken.bytes(), "replace", "shared", "latest", result), QString());
        QVERIFY(!QFileInfo::exists(pending));

        bool primaryChanged = false;
        try {
            collection::rebindOwner(dir.path(), owner, successor, oldVault, newVault, [&] {
                primaryChanged = writeBytes(mainPath, newVault);
                throw Interrupted{};
            });
            QFAIL("Expected an interruption after the main write");
        } catch (const Interrupted&) {}
        QVERIFY(primaryChanged && QFileInfo::exists(pending));
        const auto staged = storedBytes(pending);
        auto damaged = staged;
        damaged[damaged.size() - 1] = char(damaged.back() ^ 1);
        QVERIFY(writeBytes(pending, damaged));
        const auto unpromoted = storedBytes(collectionPath(dir));
        QCOMPARE(collection::issue(dir.path(), successor, newToken), QString("corrupt"));
        QCOMPARE(storedBytes(collectionPath(dir)), unpromoted);
        QVERIFY(writeBytes(pending, staged));
        QCOMPARE(collection::execute(dir.path(), oldToken.bytes(), "read", "shared", {}, result), QString("unauthorized"));
        QVERIFY(!QFileInfo::exists(pending));
        QCOMPARE(collection::issue(dir.path(), owner, newToken), QString("unauthorized"));
        QCOMPARE(collection::issue(dir.path(), successor, newToken), QString());
        QCOMPARE(collection::execute(dir.path(), newToken.bytes(), "read", "shared", {}, result), QString());
        QCOMPARE(result.bytes(), QByteArray("latest"));
        QCOMPARE(collection::execute(dir.path(), newToken.bytes(), "replace", "shared", "after", result), QString());
        const auto latest = storedBytes(collectionPath(dir));
        QVERIFY(writeBytes(pending, staged));
        QCOMPARE(collection::execute(dir.path(), newToken.bytes(), "read", "shared", {}, result), QString("storage-error"));
        QCOMPARE(storedBytes(collectionPath(dir)), latest);
        QVERIFY(QFile::remove(pending));
        QCOMPARE(collection::execute(dir.path(), newToken.bytes(), "read", "shared", {}, result), QString());
        QCOMPARE(result.bytes(), QByteArray("after"));
    }
};

QTEST_GUILESS_MAIN(TestAgentCollection)
#include "test_agent_collection.moc"
