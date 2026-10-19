#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <vector>
#else
#include <sys/stat.h>
#endif
#include "../src/vault/VaultStore.h"
#include "../src/platform/FileOps.h"
using namespace tb::store;
namespace {
struct FaultOps : FileOps {
    bool failWrite = false;
    int failRenameCall = 0;     // 1 = backup.tmp -> bak, 2 = tmp -> main
    int renameCalls = 0;
    int writeCalls = 0, corruptWriteCall = 0, failWriteCall = 0;
    bool writeAll(const QString& p, const QByteArray& b) override {
        ++writeCalls;
        if (failWrite || writeCalls == failWriteCall) return false;
        QFile f(p); if (!f.open(QIODevice::WriteOnly)) return false;
        if (writeCalls == corruptWriteCall) return f.write("partial") == 7 && f.flush();
        return f.write(b) == b.size() && f.flush();
    }
    bool renameReplace(const QString& from, const QString& to) override {
        ++renameCalls;
        if (renameCalls == failRenameCall) return false;
        if (QFile::exists(to)) QFile::remove(to);   // имитация атомарной замены
        return QFile::rename(from, to);
    }
    bool removeFile(const QString& p) override { return QFile::remove(p); }
};
}
class TestStore : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> dir_;
private slots:
    void init() { dir_ = std::make_unique<QTemporaryDir>(); QVERIFY(dir_->isValid()); }
    void saveCreatesMainAndRotatesBackup() {
        VaultStore s(dir_->path());
        QString err;
        QVERIFY(s.save("первый", &err));
        QCOMPARE(QFile(s.mainPath()).exists(), true);
        QVERIFY(s.save("второй", &err));
        QByteArray bak; QCOMPARE(s.loadBytes(s.backupPath(), &bak), VaultStore::Load::Ok);
        QCOMPARE(bak, QByteArray("первый"));
        QByteArray main; QCOMPARE(s.loadBytes(s.mainPath(), &main), VaultStore::Load::Ok);
        QCOMPARE(main, QByteArray("второй"));
    }
    void writeFailureLeavesMainIntact() {
        VaultStore s(dir_->path());
        QString err; QVERIFY(s.save("исходная", &err));
        FaultOps ops; ops.failWrite = true;
        VaultStore s2(dir_->path(), ops); s2.acceptVerified("исходная");
        QVERIFY(!s2.save("новая", &err)); QVERIFY(!err.isEmpty());
        QByteArray main; QCOMPARE(s.loadBytes(s.mainPath(), &main), VaultStore::Load::Ok);
        QCOMPARE(main, QByteArray("исходная"));
    }
    void secondRenameFailureLeavesMainInPlace() {
        VaultStore s(dir_->path());
        QString err; QVERIFY(s.save("исходная", &err)); // main существует
        FaultOps ops; ops.failRenameCall = 2;           // отказ tmp -> main
        VaultStore s2(dir_->path(), ops); s2.acceptVerified("исходная");
        QVERIFY(!s2.save("новая", &err));
        QCOMPARE(ops.renameCalls, 2);
        QByteArray main; QCOMPARE(s.loadBytes(s.mainPath(), &main), VaultStore::Load::Ok);
        QCOMPARE(main, QByteArray("исходная"));
    }
    void firstRenameFailureLeavesMainAndNoBackup() {
        VaultStore s(dir_->path());
        QString err; QVERIFY(s.save("исходная", &err));
        FaultOps ops; ops.failRenameCall = 1;
        VaultStore s2(dir_->path(), ops); s2.acceptVerified("исходная");
        QVERIFY(!s2.save("новая", &err));
        QCOMPARE(ops.renameCalls, 1);
        QByteArray main; QCOMPARE(s.loadBytes(s.mainPath(), &main), VaultStore::Load::Ok);
        QCOMPARE(main, QByteArray("исходная"));
    }
    void loadMissing() {
        VaultStore s(dir_->path() + "/нет");
        QByteArray b;
        QCOMPARE(s.loadBytes(s.mainPath(), &b), VaultStore::Load::Missing);
    }
    void failedWriteOrVerificationPreservesBothFiles() {
        VaultStore original(dir_->path()); QString err;
        QVERIFY(original.save("first", &err)); QVERIFY(original.save("second", &err));
        for (int call : {1, 2}) {
            for (bool corrupt : {false, true}) {
                FaultOps ops;
                if (corrupt) ops.corruptWriteCall = call; else ops.failWriteCall = call;
                VaultStore store(dir_->path(), ops); store.acceptVerified("second");
                QVERIFY(!store.save("third", &err)); QVERIFY(!err.isEmpty());
                QByteArray data;
                QCOMPARE(store.loadBytes(store.mainPath(), &data), VaultStore::Load::Ok); QCOMPARE(data, QByteArray("second"));
                QCOMPARE(store.loadBytes(store.backupPath(), &data), VaultStore::Load::Ok); QCOMPARE(data, QByteArray("first"));
                QVERIFY(!QFile::exists(store.tmpPath()));
                QVERIFY(!QFile::exists(store.backupPath() + ".tmp"));
            }
        }
    }
    void unverifiedOrChangedMainCannotDestroyBackup() {
        VaultStore original(dir_->path()); QString err;
        QVERIFY(original.save("first", &err)); QVERIFY(original.save("second", &err));
        VaultStore unverified(dir_->path());
        QVERIFY(!unverified.save("third", &err));
        QFile file(original.mainPath()); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("corrupt"), 7); file.close();
        QVERIFY(!original.save("third", &err));
        QByteArray data;
        QCOMPARE(original.loadBytes(original.backupPath(), &data), VaultStore::Load::Ok); QCOMPARE(data, QByteArray("first"));
        QCOMPARE(original.loadBytes(original.mainPath(), &data), VaultStore::Load::Ok); QCOMPARE(data, QByteArray("corrupt"));
    }
    void loadRejectsOversizeAndClearsOutput() {
        VaultStore store(dir_->path());
        QFile f(store.mainPath()); QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.resize(80LL * 1024 * 1024 + 1)); f.close();
        QByteArray data("old");
        QCOMPARE(store.loadBytes(store.mainPath(), &data), VaultStore::Load::IoError);
        QVERIFY(data.isEmpty());
        data = "old";
        QCOMPARE(store.loadBytes(store.backupPath(), &data), VaultStore::Load::Missing);
        QVERIFY(data.isEmpty());
    }
#ifdef Q_OS_WIN
    void lockedMainRejectsNativeReplacement() {
        VaultStore store(dir_->path()); QString err;
        QVERIFY(store.save("first", &err));
        HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(store.mainPath().utf16()), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(file != INVALID_HANDLE_VALUE);
        const bool saved = store.save("second", &err);
        CloseHandle(file);
        QVERIFY(!saved); QVERIFY(!err.isEmpty());
        QByteArray data;
        QCOMPARE(store.loadBytes(store.mainPath(), &data), VaultStore::Load::Ok); QCOMPARE(data, QByteArray("first"));
        QCOMPARE(store.loadBytes(store.backupPath(), &data), VaultStore::Load::Ok); QCOMPARE(data, QByteArray("first"));
        QVERIFY(!QFile::exists(store.tmpPath()));
    }
    void windowsProtectedDacl() {
        VaultStore store(dir_->path()); QString err;
        QVERIFY(store.save("first", &err)); QVERIFY(store.save("second", &err));
        HANDLE token = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
        DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> data(size);
        const bool tokenOk = GetTokenInformation(token, TokenUser, data.data(), size, &size);
        CloseHandle(token); QVERIFY(tokenOk);
        const PSID user = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
        for (const auto& path : {dir_->path(), store.mainPath(), store.backupPath()}) {
            PACL acl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
            QCOMPARE(GetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
                SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &sd), DWORD(ERROR_SUCCESS));
            SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
            const bool protectedAcl = GetSecurityDescriptorControl(sd, &control, &revision) && (control & SE_DACL_PROTECTED);
            bool correct = acl && acl->AceCount == 2; bool hasUser = false, hasSystem = false;
            if (correct) for (DWORD i = 0; i < acl->AceCount; ++i) {
                void* raw = nullptr;
                if (!GetAce(acl, i, &raw)) { correct = false; break; }
                auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
                const PSID sid = &ace->SidStart;
                hasUser = hasUser || EqualSid(sid, user) != FALSE;
                hasSystem = hasSystem || IsWellKnownSid(sid, WinLocalSystemSid) != FALSE;
                correct &= ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Mask == FILE_ALL_ACCESS;
            }
            LocalFree(sd);
            QVERIFY(protectedAcl); QVERIFY(correct); QVERIFY(hasUser); QVERIFY(hasSystem);
        }
    }
#endif
#ifndef Q_OS_WIN
    void posixFileAndDirPermissions() {
        VaultStore s(dir_->path());
        QString err; QVERIFY(s.save("данные", &err));
        QFileInfo fi(s.mainPath());
        struct stat st{};
        QCOMPARE(::stat(QFile::encodeName(s.mainPath()).constData(), &st), 0);
        QCOMPARE(st.st_mode & 0777, 0600u);
        QCOMPARE(::stat(QFile::encodeName(dir_->path()).constData(), &st), 0);
        QCOMPARE(st.st_mode & 0777, 0700u);
    }
#endif
};
QTEST_MAIN(TestStore)
#include "test_store.moc"
