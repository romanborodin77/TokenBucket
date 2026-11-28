#include <QtTest>
#include <QFile>
#include <QLockFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtEndian>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
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

    void ownerCrudPreservesExactBytesAndRejectsConcurrentChanges() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto owner = ownerKey(), wrongOwner = ownerKey();
        const auto path = collectionPath(dir);
        const QByteArray binary = QByteArray::fromHex("61000d0aff20");
        const QByteArray replacement = QByteArray::fromHex("efbbbf0d0a000a");
        QStringList names{"stale"};
        SecureBytes result(QByteArray("stale")), token;

        QCOMPARE(collection::ownerList(dir.path(), owner, names), QString());
        QVERIFY(names.isEmpty());
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(collection::ownerRead(dir.path(), owner, "Entry", result), QString("not-found"));
        QVERIFY(result.isEmpty());
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "Entry", binary), QString());
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "entry", {}), QString());
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "Entry", "duplicate"), QString("already-exists"));
        QCOMPARE(collection::ownerList(dir.path(), owner, names), QString());
        QCOMPARE(names, (QStringList{"Entry", "entry"}));
        QCOMPARE(collection::ownerList(dir.path(), wrongOwner, names), QString("unauthorized"));
        QVERIFY(names.isEmpty());
        QCOMPARE(collection::ownerRead(dir.path(), owner, "Entry", result), QString());
        QCOMPARE(result.bytes(), binary);
        QCOMPARE(collection::ownerRead(dir.path(), wrongOwner, "Entry", result), QString("unauthorized"));
        QVERIFY(result.isEmpty());

        QCOMPARE(collection::issue(dir.path(), owner, token), QString());
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", "Entry", {}, result), QString());
        QCOMPARE(result.bytes(), binary);
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "replace", "Entry", replacement, result), QString());
        const auto prior = storedBytes(path);
        QCOMPARE(collection::ownerUpdate(dir.path(), owner, "Entry", binary, "Entry", "stale"), QString("conflict"));
        QCOMPARE(collection::ownerUpdate(dir.path(), owner, "Entry", binary, "Renamed", "stale"), QString("conflict"));
        QCOMPARE(collection::ownerDelete(dir.path(), owner, "Entry", binary), QString("conflict"));
        QCOMPARE(collection::ownerUpdate(dir.path(), owner, "Entry", replacement, "entry", "collision"),
                 QString("already-exists"));
        QCOMPARE(storedBytes(path), prior);

        QLockFile lock(path + ".lock");
        QVERIFY(lock.tryLock());
        names = QStringList{"stale"};
        result = SecureBytes(QByteArray("stale"));
        QCOMPARE(collection::ownerList(dir.path(), owner, names), QString("busy"));
        QCOMPARE(collection::ownerRead(dir.path(), owner, "Entry", result), QString("busy"));
        QVERIFY(names.isEmpty() && result.isEmpty());
        QCOMPARE(collection::ownerUpdate(dir.path(), owner, "Entry", replacement, "Renamed", binary), QString("busy"));
        QCOMPARE(collection::ownerDelete(dir.path(), owner, "Entry", replacement), QString("busy"));
        QCOMPARE(storedBytes(path), prior);
        lock.unlock();

#ifdef Q_OS_WIN
        // Permit readers while denying replacement of the live ciphertext.
        const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        const auto blocked = collection::ownerUpdate(dir.path(), owner, "Entry", replacement, "Renamed", binary);
        QVERIFY(CloseHandle(handle));
        QCOMPARE(blocked, QString("storage-error"));
        QCOMPARE(storedBytes(path), prior);
#endif

        QCOMPARE(collection::ownerUpdate(dir.path(), owner, "Entry", replacement, "Renamed", binary), QString());
        QCOMPARE(collection::ownerUpdate(dir.path(), owner, "Entry", replacement, "Entry", binary), QString("conflict"));
        QCOMPARE(collection::ownerDelete(dir.path(), owner, "Entry", replacement), QString("conflict"));
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", "Renamed", {}, result), QString());
        QCOMPARE(result.bytes(), binary);
        QCOMPARE(collection::ownerRead(dir.path(), owner, "entry", result), QString());
        QVERIFY(result.isEmpty());
        QCOMPARE(collection::ownerUpdate(dir.path(), owner, "Renamed", binary, "Renamed", {}), QString());
        QCOMPARE(collection::ownerDelete(dir.path(), owner, "Renamed", {}), QString());
        QCOMPARE(collection::ownerDelete(dir.path(), owner, "entry", {}), QString());
        QCOMPARE(collection::ownerList(dir.path(), owner, names), QString());
        QVERIFY(names.isEmpty());
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", "Renamed", {}, result), QString("not-found"));
    }

    void groupedRecordsUseExactIdentityAndAtomicMoves() {
        QTemporaryDir dir, other;
        QVERIFY(dir.isValid() && other.isValid());
        const auto owner = ownerKey();
        const auto binary = QByteArray::fromHex("000d0aff20");
        SecureBytes token, unrelated, result, legacy;
        const auto path = collectionPath(dir);
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "ops/shared", "legacy"), QString());
        QCOMPARE(collection::ownerSnapshot(dir.path(), owner, legacy), QString());
        QCOMPARE(legacy.bytes(), QByteArray::fromHex("000a00000006") + "ops/sharedlegacy");
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "ops", "shared", binary), QString());
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "Ops", "shared", "other"), QString());
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "ops", "shared", "duplicate"), QString("already-exists"));
        QCOMPARE(collection::ownerCreate(dir.path(), owner, QString(129, QChar(0x00e9)), "name", {}), QString("invalid-request"));
        QCOMPARE(collection::ownerSnapshot(dir.path(), owner, result), QString());
        QVERIFY(result.bytes().startsWith(legacy.bytes()));

        QMap<QString, QStringList> tree;
        QCOMPARE(collection::ownerTree(dir.path(), owner, tree), QString());
        QCOMPARE(tree.size(), 3);
        QCOMPARE(tree.value(QString()), QStringList{"ops/shared"});
        QCOMPARE(tree.value("Ops"), QStringList{"shared"});
        QCOMPARE(tree.value("ops"), QStringList{"shared"});
        QCOMPARE(collection::issue(other.path(), owner, unrelated), QString());
        QStringList projects{"stale"}, names;
        QCOMPARE(collection::agentListProjects(dir.path(), unrelated.bytes(), projects), QString("unauthorized"));
        QVERIFY(projects.isEmpty());
        QCOMPARE(collection::issue(dir.path(), owner, token), QString());
        QCOMPARE(collection::agentListProjects(dir.path(), token.bytes(), projects), QString());
        QCOMPARE(projects, (QStringList{QString(), "Ops", "ops"}));
        QCOMPARE(collection::agentList(dir.path(), token.bytes(), "ops", names), QString());
        QCOMPARE(names, QStringList{"shared"});
        QCOMPARE(collection::agentList(dir.path(), token.bytes(), "missing", names), QString());
        QVERIFY(names.isEmpty());
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", "ops/shared", {}, result), QString());
        QCOMPARE(result.bytes(), QByteArray("legacy"));
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", "shared", {}, result, "ops"), QString());
        QCOMPARE(result.bytes(), binary);
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "create", "second", {}, result, "ops"), QString());
        QCOMPARE(collection::ownerList(dir.path(), owner, "ops", names), QString());
        QCOMPARE(names, (QStringList{"second", "shared"}));
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "replace", "shared", "changed", result, "ops"), QString());
        const auto prior = storedBytes(path);
        QCOMPARE(collection::ownerUpdate(dir.path(), owner, "ops", "shared", binary, "new", "renamed", "stale"),
                 QString("conflict"));
        QCOMPARE(collection::ownerDelete(dir.path(), owner, "ops", "shared", binary), QString("conflict"));
        QCOMPARE(collection::ownerUpdate(dir.path(), owner, "ops", "shared", "changed", "Ops", "shared", {}),
                 QString("already-exists"));
        QCOMPARE(collection::agentMove(dir.path(), token.bytes(), "ops", "shared", "Ops", "shared"), QString("already-exists"));
        QCOMPARE(storedBytes(path), prior);

        QLockFile lock(path + ".lock");
        QVERIFY(lock.tryLock());
        QCOMPARE(collection::agentMove(dir.path(), token.bytes(), "ops", "shared", "new", "renamed"), QString("busy"));
        QCOMPARE(collection::ownerTree(dir.path(), owner, tree), QString("busy"));
        QVERIFY(tree.isEmpty());
        lock.unlock();
        QCOMPARE(collection::agentMove(dir.path(), token.bytes(), "ops", "shared", {}, "moved"), QString());
        QCOMPARE(collection::ownerRead(dir.path(), owner, "moved", result), QString());
        QCOMPARE(result.bytes(), QByteArray("changed"));
        QCOMPARE(collection::agentMove(dir.path(), token.bytes(), "ops", "shared", "new", "renamed"), QString("not-found"));
        QCOMPARE(collection::ownerUpdate(dir.path(), owner, {}, "moved", "changed", "new", "renamed", binary), QString());
        QCOMPARE(collection::agentMove(dir.path(), token.bytes(), "new", "renamed", "New", "renamed"), QString());
        QCOMPARE(collection::ownerRead(dir.path(), owner, "New", "renamed", result), QString());
        QCOMPARE(result.bytes(), binary);
        QCOMPARE(collection::ownerDelete(dir.path(), owner, "ops", "second", {}), QString());
        QCOMPARE(collection::agentListProjects(dir.path(), token.bytes(), projects), QString());
        QCOMPARE(projects, (QStringList{QString(), "New", "Ops"}));
        QCOMPARE(collection::revoke(dir.path(), owner), QString());
        QCOMPARE(collection::agentListProjects(dir.path(), token.bytes(), projects), QString("unauthorized"));
        QVERIFY(projects.isEmpty());
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
        QStringList names{"stale"};
        result = SecureBytes(QByteArray("stale"));
        QCOMPARE(collection::ownerList(dir.path(), owner, names), QString("corrupt"));
        QCOMPARE(collection::ownerRead(dir.path(), owner, "entry", result), QString("corrupt"));
        QVERIFY(names.isEmpty());
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

    void snapshotTransferRecoversInterruptedCommit_data() {
        QTest::addColumn<bool>("hasVault");
        QTest::addColumn<bool>("hasCollection");
        QTest::newRow("fresh") << false << false;
        QTest::newRow("vault-only") << true << false;
        QTest::newRow("vault-with-collection") << true << true;
    }

    void snapshotTransferRecoversInterruptedCommit() {
        QFETCH(bool, hasVault);
        QFETCH(bool, hasCollection);
        QTemporaryDir source, target;
        QVERIFY(source.isValid() && target.isValid());
        const auto sourceOwner = ownerKey(), oldOwner = ownerKey(), newOwner = ownerKey();
        const auto* priorOwner = hasVault ? &oldOwner : nullptr;
        const auto primary = target.filePath("vault.tb");
        const auto oldVault = hasVault ? container::packPassword("vault-before-transfer", "before", nullptr) : QByteArray();
        const auto nextVault = container::packPassword("vault-after-transfer", "after", nullptr);
        if (hasVault) QVERIFY(!oldVault.isEmpty() && writeBytes(primary, oldVault));
        QVERIFY(!nextVault.isEmpty());

        SecureBytes sourceToken, oldToken, snapshot, result;
        const auto value = QByteArray::fromHex("000d0aff20");
        QCOMPARE(collection::issue(source.path(), sourceOwner, sourceToken), QString());
        QCOMPARE(collection::ownerCreate(source.path(), sourceOwner, "imported", value), QString());
        QCOMPARE(collection::ownerCreate(source.path(), sourceOwner, "Project", "imported", "grouped"), QString());
        if (hasCollection) {
            QCOMPARE(collection::issue(target.path(), oldOwner, oldToken), QString());
            QCOMPARE(collection::ownerCreate(target.path(), oldOwner, "obsolete", "prior"), QString());
        }
        QCOMPARE(collection::ownerSnapshot(source.path(), sourceOwner, snapshot), QString());
        QVERIFY(!snapshot.isEmpty());
        const auto pending = collectionPath(target) + ".pending";
        const auto liveBefore = storedBytes(collectionPath(target));

        struct Interrupted {};
        try {
            collection::restoreSnapshot(target.path(), priorOwner, newOwner, oldVault, nextVault, snapshot.bytes(), [] {
                throw Interrupted{};
            });
            QFAIL("Expected an interruption before vault replacement");
        } catch (const Interrupted&) {}
        QVERIFY(QFileInfo::exists(pending));
        QCOMPARE(storedBytes(primary), oldVault);
        QStringList names;
        QCOMPARE(collection::ownerList(target.path(), oldOwner, names), QString());
        QCOMPARE(names, hasCollection ? QStringList{"obsolete"} : QStringList{});
        QVERIFY(!QFileInfo::exists(pending));
        QCOMPARE(storedBytes(collectionPath(target)), liveBefore);

        bool mainWritten = false;
        try {
            collection::restoreSnapshot(target.path(), priorOwner, newOwner, oldVault, nextVault, snapshot.bytes(), [&] {
                mainWritten = writeBytes(primary, nextVault);
                throw Interrupted{};
            });
            QFAIL("Expected an interruption after vault replacement");
        } catch (const Interrupted&) {}
        QVERIFY(mainWritten && QFileInfo::exists(pending));
        QCOMPARE(storedBytes(collectionPath(target)), liveBefore);
        QCOMPARE(collection::ownerSnapshot(target.path(), newOwner, result), QString());
        QCOMPARE(result.bytes(), snapshot.bytes());
        QVERIFY(!QFileInfo::exists(pending));
        QCOMPARE(collection::ownerRead(target.path(), newOwner, "imported", result), QString());
        QCOMPARE(result.bytes(), value);
        QCOMPARE(collection::ownerRead(target.path(), newOwner, "Project", "imported", result), QString());
        QCOMPARE(result.bytes(), QByteArray("grouped"));
        QCOMPARE(collection::ownerRead(target.path(), newOwner, "obsolete", result), QString("not-found"));
        QCOMPARE(collection::execute(target.path(), sourceToken.bytes(), "read", "imported", {}, result), QString("unauthorized"));
        if (hasCollection)
            QCOMPARE(collection::execute(target.path(), oldToken.bytes(), "read", "obsolete", {}, result), QString("unauthorized"));
        QCOMPARE(collection::execute(source.path(), sourceToken.bytes(), "read", "imported", {}, result), QString());
        QCOMPARE(result.bytes(), value);
    }

    void snapshotTransferValidatesBeforeChanges() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto owner = ownerKey(), wrongOwner = ownerKey(), successor = ownerKey();
        const auto nextVault = container::packPassword("valid-next-password", "next", nullptr);
        const auto oldVault = container::packPassword("valid-old-password", "old", nullptr);
        QVERIFY(!nextVault.isEmpty() && !oldVault.isEmpty());
        const auto primary = dir.filePath("vault.tb");
        const auto path = collectionPath(dir);
        const auto pending = path + ".pending";
        SecureBytes result(QByteArray("stale"));
        QCOMPARE(collection::ownerSnapshot(dir.filePath("missing-dir"), owner, result), QString("storage-error"));
        QVERIFY(result.isEmpty());
        QVERIFY(writeBytes(dir.filePath("regular-file"), "regular"));
        result = SecureBytes(QByteArray("stale"));
        QCOMPARE(collection::ownerSnapshot(dir.filePath("regular-file"), owner, result), QString("storage-error"));
        QVERIFY(result.isEmpty());
        QCOMPARE(collection::ownerSnapshot(dir.path(), owner, result), QString());
        QVERIFY(result.isEmpty() && !QFileInfo::exists(path));

        QLockFile lock(path + ".lock");
        QVERIFY(lock.tryLock());
        result = SecureBytes(QByteArray("stale"));
        QCOMPARE(collection::ownerSnapshot(dir.path(), owner, result), QString("busy"));
        QVERIFY(result.isEmpty());
        const auto unexpectedCommit = [] { QFAIL("Invalid restore must not invoke commit"); };
        QCOMPARE(collection::restoreSnapshot(dir.path(), nullptr, successor, {}, nextVault, {}, unexpectedCommit), QString("busy"));
        lock.unlock();
        QVERIFY(writeBytes(primary, "not-a-vault"));
        QCOMPARE(collection::restoreSnapshot(dir.path(), nullptr, successor, {}, nextVault, {}, unexpectedCommit), QString("storage-error"));
        QVERIFY(QFile::remove(primary));

        QCOMPARE(collection::ownerCreate(dir.path(), owner, "Project", "original", "secret"), QString());
        const auto live = storedBytes(path);
        // An orphaned collection cannot be replaced by pretending this is a fresh installation.
        QCOMPARE(collection::restoreSnapshot(dir.path(), nullptr, successor, {}, nextVault, {}, unexpectedCommit), QString("storage-error"));
        QVERIFY(writeBytes(primary, oldVault));
        QCOMPARE(collection::restoreSnapshot(dir.path(), &wrongOwner, successor, oldVault, nextVault, {}, unexpectedCommit), QString("unauthorized"));
        SecureBytes valid;
        QCOMPARE(collection::ownerSnapshot(dir.path(), owner, valid), QString());
        const auto truncated = valid.bytes().chopped(1);
        const auto duplicate = valid.bytes() + valid.bytes();
        auto emptyProject = valid.bytes();
        emptyProject[3] = '\0';
        for (const auto& malformed : {truncated, duplicate, emptyProject, QByteArray(collection::MaxSnapshotBytes + 1, 'x')})
            QCOMPARE(collection::restoreSnapshot(dir.path(), &owner, successor, oldVault, nextVault, malformed, unexpectedCommit), QString("corrupt"));
        QVERIFY(!QFileInfo::exists(pending));
        QCOMPARE(storedBytes(primary), oldVault);
        QCOMPARE(storedBytes(path), live);

        bool committed = false;
        QCOMPARE(collection::restoreSnapshot(dir.path(), &owner, successor, oldVault, nextVault, {}, [&] {
            committed = writeBytes(primary, nextVault);
        }), QString());
        QVERIFY(committed && !QFileInfo::exists(pending));
        QCOMPARE(collection::ownerSnapshot(dir.path(), successor, result), QString());
        QVERIFY(result.isEmpty());
        QCOMPARE(collection::ownerSnapshot(dir.path(), owner, result), QString("unauthorized"));
    }

#ifdef Q_OS_WIN
    void snapshotTransferBlocksOldTokenUntilCollectionPromotion() {
        QTemporaryDir dir, source;
        QVERIFY(dir.isValid() && source.isValid());
        const auto owner = ownerKey(), successor = ownerKey();
        const auto oldVault = container::packPassword("valid-old-password", "old", nullptr);
        const auto nextVault = container::packPassword("valid-next-password", "next", nullptr);
        const auto primary = dir.filePath("vault.tb");
        const auto path = collectionPath(dir);
        const auto pending = path + ".pending";
        QVERIFY(!oldVault.isEmpty() && !nextVault.isEmpty() && writeBytes(primary, oldVault));
        SecureBytes token, snapshot, result;
        QCOMPARE(collection::issue(dir.path(), owner, token), QString());
        QCOMPARE(collection::ownerCreate(dir.path(), owner, "old", "old secret"), QString());
        QCOMPARE(collection::ownerCreate(source.path(), successor, "incoming", "new secret"), QString());
        QCOMPARE(collection::ownerSnapshot(source.path(), successor, snapshot), QString());
        const auto prior = storedBytes(path);
        {
            const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(handle != INVALID_HANDLE_VALUE);
            const auto close = qScopeGuard([&] { CloseHandle(handle); });
            bool committed = false;
            QCOMPARE(collection::restoreSnapshot(dir.path(), &owner, successor, oldVault, nextVault, snapshot.bytes(), [&] {
                committed = writeBytes(primary, nextVault);
            }), QString("storage-error"));
            QVERIFY(committed && QFileInfo::exists(pending));
            QCOMPARE(storedBytes(primary), nextVault);
            QCOMPARE(storedBytes(path), prior);
            QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", "old", {}, result), QString("storage-error"));
            QVERIFY(result.isEmpty());
        }
        QCOMPARE(collection::execute(dir.path(), token.bytes(), "read", "old", {}, result), QString("unauthorized"));
        QVERIFY(!QFileInfo::exists(pending));
        QStringList names;
        QCOMPARE(collection::ownerList(dir.path(), successor, names), QString());
        QCOMPARE(names, QStringList{"incoming"});
        QCOMPARE(collection::ownerRead(dir.path(), successor, "incoming", result), QString());
        QCOMPARE(result.bytes(), QByteArray("new secret"));
    }
#endif

    void snapshotJournalAcceptsMaximumSizedSnapshot() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto nextVault = container::packPassword("valid-next-password", "next", nullptr);
        QVERIFY(!nextVault.isEmpty());
        const auto owner = ownerKey();
        QByteArray body;
        body.reserve(collection::MaxSnapshotBytes);
        for (int i = 0; i < 128; ++i) {
            const auto name = QByteArray::number(i).rightJustified(3, '0');
            char lengths[6];
            const auto size = qMin<qsizetype>(32 * 1024, collection::MaxSnapshotBytes - body.size() - 9);
            qToBigEndian(quint16(name.size()), lengths);
            qToBigEndian(quint32(size), lengths + 2);
            body.append(lengths, sizeof lengths).append(name).append(size, '\0');
        }
        QCOMPARE(body.size(), collection::MaxSnapshotBytes);
        bool committed = false;
        QCOMPARE(collection::restoreSnapshot(dir.path(), nullptr, owner, {}, nextVault, body, [&] {
            committed = writeBytes(dir.filePath("vault.tb"), nextVault);
        }), QString());
        QVERIFY(committed);
        SecureBytes restored;
        QCOMPARE(collection::ownerSnapshot(dir.path(), owner, restored), QString());
        QVERIFY(restored.bytes() == body);
    }
};

QTEST_GUILESS_MAIN(TestAgentCollection)
#include "test_agent_collection.moc"
