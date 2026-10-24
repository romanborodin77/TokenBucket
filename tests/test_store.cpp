#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QDir>
#include <QScopeGuard>
#include <memory>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <vector>
#else
#include <sys/stat.h>
#endif
#include "../src/vault/VaultStore.h"
#include "../src/platform/FileOps.h"
#include "../src/vault/Container.h"
#include "FaultFileOps.h"
using namespace tb::store;
class TestStore : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> dir_;
private slots:
    void preservesVerifiedUpgradeCopy() {
        VaultStore store(dir_->path()); QString error; QVERIFY(store.save("legacy-ciphertext",&error));
        const auto copy=store.preserveBeforeUpgrade("legacy-ciphertext",&error); QVERIFY2(!copy.isEmpty(),qPrintable(error));
        QByteArray check; QCOMPARE(store.loadBytes(copy,&check),VaultStore::Load::Ok); QCOMPARE(check,QByteArray("legacy-ciphertext"));
        QVERIFY(copy.contains(".before-upgrade-")); QVERIFY(store.save("new-ciphertext",&error));
        QCOMPARE(store.loadBytes(copy,&check),VaultStore::Load::Ok); QCOMPARE(check,QByteArray("legacy-ciphertext"));
        QVERIFY(store.preserveBeforeUpgrade("legacy-ciphertext",&error).isEmpty());
    }
    void init() { dir_ = std::make_unique<QTemporaryDir>(); QVERIFY(dir_->isValid()); }
    void restorePreservesCurrentAndBackupUntilUnlock() {
        VaultStore store(dir_->path()); QString error;
        QVERIFY(store.save("older",&error)); QVERIFY(store.save("current",&error));
        const auto result=store.restoreBackup("current","older",[] {return false;});
        QVERIFY2(result.committed,qPrintable(result.error)); QVERIFY(result.error.isEmpty());
        QByteArray bytes;
        QCOMPARE(store.loadBytes(store.mainPath(),&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,QByteArray("older"));
        QCOMPARE(store.loadBytes(store.backupPath(),&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,QByteArray("older"));
        QCOMPARE(store.loadBytes(result.safetyPath,&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,QByteArray("current"));
        QVERIFY(!store.save("not-yet-unlocked",&error));
        store.acceptVerified("older"); QVERIFY(store.save("after-unlock",&error));
    }
    void restoreFailures_data() {
        QTest::addColumn<int>("fault");
        const char* names[]={"wrong-main","changed-backup","missing-backup","oversize-backup","cancel-before-write",
            "cancel-after-safety","cancel-before-rename","fail-safety-write","fail-candidate-write","corrupt-safety",
            "corrupt-candidate","fail-rename","safety-collision","candidate-collision","backup-changed-after-write",
            "main-changed-after-write","cancel-during-rename","post-commit-corruption","symlink-backup","unverified-main","missing-main","post-commit-callback-exception"};
        for(int i=0;i<int(std::size(names));++i) QTest::newRow(names[i])<<i;
    }
    void restoreFailures() {
        QFETCH(int,fault);
        VaultStore original(dir_->path()); QString error;
        QVERIFY(original.save("older",&error)); QVERIFY(original.save("current",&error));
        FaultOps ops; VaultStore store(dir_->path(),ops);
        if(fault!=19) store.acceptVerified("current");
        bool canceled=fault==4,renamed=false; QString collided;
        QByteArray expectedMain="current",mainAfter="current",backupAfter="older";
        if(fault==0) expectedMain="different-main";
        if(fault==1) {
            QVERIFY(QFile::remove(store.backupPath())); QVERIFY(realFileOps().writeAll(store.backupPath(),"changed")); backupAfter="changed";
        }
        if(fault==2) QVERIFY(QFile::remove(store.backupPath()));
        if(fault==20) QVERIFY(QFile::remove(store.mainPath()));
        if(fault==3) {
            QFile file(store.backupPath()); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(tb::container::MaxFileBytes+1));
        }
        if(fault==5 || fault==6) ops.beforeWrite=[&](const QString&) {if(ops.writeCalls==(fault==5?1:2)) canceled=true;};
        if(fault==7 || fault==8) ops.failWriteCall=fault==7?1:2;
        if(fault==9 || fault==10) ops.corruptWriteCall=fault==9?1:2;
        if(fault==11) ops.failRenameCall=1;
        if(fault==12 || fault==13) ops.beforeWrite=[&](const QString& path) {
            if(ops.writeCalls==(fault==12?1:2)) {collided=path; QVERIFY(realFileOps().writeAll(path,"foreign-collision"));}
        };
        if(fault==14 || fault==15) {
            ops.beforeWrite=[&](const QString&) {
                if(ops.writeCalls==2) {
                    const auto path=fault==14?store.backupPath():store.mainPath();
                    QVERIFY(QFile::remove(path)); QVERIFY(realFileOps().writeAll(path,"external-change"));
                }
            };
            if(fault==14) backupAfter="external-change"; else mainAfter="external-change";
        }
        if(fault==16) {ops.beforeRename=[&](const QString&,const QString&) {canceled=true;}; mainAfter="older";}
        if(fault==17) {
            ops.afterRename=[&](const QString&,const QString& path) {QVERIFY(QFile::remove(path)); QVERIFY(realFileOps().writeAll(path,"post-commit"));};
            mainAfter="post-commit";
        }
        if(fault==21) {ops.afterRename=[&](const QString&,const QString&) {renamed=true;}; mainAfter="older";}
        if(fault==18) {
            const auto target=dir_->filePath("linked-backup.tb"); QVERIFY(QFile::rename(store.backupPath(),target));
#ifdef Q_OS_WIN
            QVERIFY2(CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(store.backupPath().utf16()),
                reinterpret_cast<LPCWSTR>(target.utf16()),SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE),"File symlink creation requires Windows Developer Mode or symlink privilege");
#else
            QVERIFY(QFile::link(target,store.backupPath()));
#endif
        }
        const auto result=store.restoreBackup(expectedMain,"older",[&] {
            if(fault==21 && renamed) throw std::runtime_error("synthetic post-commit exception");
            return canceled;
        });
        QCOMPARE(result.committed,fault==16 || fault==17 || fault==21); QVERIFY(!result.error.isEmpty());
        QByteArray bytes;
        if(fault==20) QCOMPARE(store.loadBytes(store.mainPath(),&bytes),VaultStore::Load::Missing);
        else {QCOMPARE(store.loadBytes(store.mainPath(),&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,mainAfter);}
        if(fault==2) QCOMPARE(store.loadBytes(store.backupPath(),&bytes),VaultStore::Load::Missing);
        else if(fault==3) QCOMPARE(QFileInfo(store.backupPath()).size(),tb::container::MaxFileBytes+1);
        else if(fault==18) QCOMPARE(store.loadBytes(store.backupPath(),&bytes),VaultStore::Load::IoError);
        else {QCOMPARE(store.loadBytes(store.backupPath(),&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,backupAfter);}
        if(!collided.isEmpty()) {QFile file(collided); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),QByteArray("foreign-collision"));}
        const auto leftovers=QDir(dir_->path()).entryList({"vault.tb.restore-*.tmp"},QDir::Files);
        QCOMPARE(leftovers.size(),fault==13?1:0);
        if(!result.safetyPath.isEmpty()) {
            QCOMPARE(store.loadBytes(result.safetyPath,&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,QByteArray("current"));
        }
        ops=FaultOps{};
        if(!result.committed && fault!=15 && fault!=18 && fault!=19 && fault!=20) QVERIFY2(store.save("after-failure",&error),qPrintable(error));
    }
    void repeatedRestoreRequiresVerificationAndKeepsBothSafetyFiles() {
        VaultStore store(dir_->path()); QString error;
        QVERIFY(store.save("older",&error)); QVERIFY(store.save("current",&error));
        const auto first=store.restoreBackup("current","older",[] {return false;}); QVERIFY(first.committed);
        QVERIFY(!store.restoreBackup("older","older",[] {return false;}).committed);
        store.acceptVerified("older");
        const auto second=store.restoreBackup("older","older",[] {return false;}); QVERIFY(second.committed);
        QVERIFY(first.safetyPath!=second.safetyPath);
        QByteArray bytes; QCOMPARE(store.loadBytes(first.safetyPath,&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,QByteArray("current"));
        QCOMPARE(store.loadBytes(second.safetyPath,&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,QByteArray("older"));
    }
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
    void windowsHeldMainPreventsRestoreWithoutLosingSafety() {
        VaultStore store(dir_->path()); QString error;
        QVERIFY(store.save("older",&error)); QVERIFY(store.save("current",&error));
        HANDLE file=CreateFileW(reinterpret_cast<LPCWSTR>(store.mainPath().utf16()),GENERIC_READ,
            FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        QVERIFY(file!=INVALID_HANDLE_VALUE);
        const auto close=qScopeGuard([&] {CloseHandle(file);});
        const auto result=store.restoreBackup("current","older",[] {return false;});
        QVERIFY(!result.committed); QVERIFY(!result.error.isEmpty()); QVERIFY(!result.safetyPath.isEmpty());
        QByteArray bytes; QCOMPARE(store.loadBytes(store.mainPath(),&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,QByteArray("current"));
        QCOMPARE(store.loadBytes(store.backupPath(),&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,QByteArray("older"));
        QCOMPARE(store.loadBytes(result.safetyPath,&bytes),VaultStore::Load::Ok); QCOMPARE(bytes,QByteArray("current"));
        QVERIFY(QDir(dir_->path()).entryList({"vault.tb.restore-*.tmp"},QDir::Files).isEmpty());
    }
    void windowsProtectedDacl() {
        VaultStore store(dir_->path()); QString err;
        QVERIFY(store.save("first", &err)); QVERIFY(store.save("second", &err));
        const auto restored=store.restoreBackup("second","first",[] {return false;}); QVERIFY2(restored.committed,qPrintable(restored.error));
        store.acceptVerified("first"); const auto upgrade=store.preserveBeforeUpgrade("first",&err); QVERIFY(!upgrade.isEmpty());
        HANDLE token = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
        DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> data(size);
        const bool tokenOk = GetTokenInformation(token, TokenUser, data.data(), size, &size);
        CloseHandle(token); QVERIFY(tokenOk);
        const PSID user = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
        for (const auto& path : {dir_->path(), store.mainPath(), store.backupPath(),restored.safetyPath,upgrade}) {
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
