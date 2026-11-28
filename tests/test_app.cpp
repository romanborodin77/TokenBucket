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
#include <QCborMap>
#include <QCborArray>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include "../src/app/Application.h"
#include "../src/ui/SetupDialog.h"
#include "../src/ui/UnlockDialog.h"
#include "../src/vault/Container.h"
#include "../src/vault/Crypto.h"
#include "../src/vault/AgentCollection.h"
#include "FaultFileOps.h"
using namespace tb;
class TestApp : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    static QByteArray readFile(const QString& path) {
        QFile file(path); if(!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
    }
    static QByteArray legacyV3() {
        crypto::init(); const auto group=QUuid::createUuid(),id=QUuid::createUuid();
        QCborMap node; node[QStringLiteral("id")]=group.toString(QUuid::WithoutBraces);
        node[QStringLiteral("p")]=QString(); node[QStringLiteral("n")]=QStringLiteral("Legacy");
        QCborMap entry; entry[QStringLiteral("id")]=id.toString(QUuid::WithoutBraces);
        entry[QStringLiteral("g")]=group.toString(QUuid::WithoutBraces); entry[QStringLiteral("type")]=0;
        entry[QStringLiteral("title")]=QStringLiteral("Legacy token"); entry[QStringLiteral("tok")]=QByteArray("synthetic-legacy-token");
        QCborMap root; root[QStringLiteral("v")]=3; root[QStringLiteral("nodes")]=QCborArray{node}; root[QStringLiteral("entries")]=QCborArray{entry};
        return container::packPassword("valid-password",QCborValue(root).toCbor(),nullptr);
    }
private slots:
    void reverseCollectionTransferCopiesMovesAndRejectsCollisions() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Application app(dir.path());
        QString error;
        QVERIFY2(app.createPasswordVault("reverse-test-password",&error),qPrintable(error));
        const auto binary=QByteArray::fromHex("efbbbf00ff0d0a");
        SecureBytes value;
        QCOMPARE(app.copyToCollection("source","shared",binary),QString());
        QCOMPARE(app.copyToCollection("other-source","shared","different"),QString());
        QCOMPARE(app.copyToCollection("source","empty",{}),QString());
        QCOMPARE(app.transferFromCollection("source","shared",binary,{},"Imported",false),QString());
        QCOMPARE(app.model()->entries().size(),1);
        const auto copied=app.model()->entries().cbegin().value();
        const auto destination=copied.groupId;
        QCOMPARE(app.model()->nodes().value(destination).name,QString("Imported"));
        QCOMPARE(copied.type,model::EntryType::ApiToken);
        QVERIFY(copied.reverify && copied.note.contains("Base64"));
        QCOMPARE(QByteArray::fromBase64(copied.token.toLatin1()),binary);
        QCOMPARE(app.readCollection("source","shared",value),QString());
        QCOMPARE(value.bytes(),binary);

        QCOMPARE(app.transferFromCollection("source","shared",binary,{},"Imported",false),QString());
        QCOMPARE(app.model()->nodes().size(),1);
        QCOMPARE(app.model()->entries().size(),1);
        QVERIFY(app.model()->entry(copied.id) && *app.model()->entry(copied.id)==copied);
        const auto beforeCollision=readFile(app.storePathForTest());
        QVERIFY(!app.transferFromCollection("other-source","shared","different",destination,{},true).isEmpty());
        QCOMPARE(readFile(app.storePathForTest()),beforeCollision);
        QCOMPARE(app.readCollection("other-source","shared",value),QString());
        QCOMPARE(value.bytes(),QByteArray("different"));
        QVERIFY(app.model()->entry(copied.id) && *app.model()->entry(copied.id)==copied);

        QCOMPARE(app.transferFromCollection("source","shared",binary,destination,{},true),QString());
        QVERIFY(!app.readCollection("source","shared",value).isEmpty());
        QVERIFY(value.isEmpty());
        QCOMPARE(app.transferFromCollection("source","empty",{},destination,{},true),QString());
        QVERIFY(!app.readCollection("source","empty",value).isEmpty());
        QCOMPARE(app.model()->entries().size(),2);
        app.lock();
        QVERIFY2(app.unlock("reverse-test-password",&error),qPrintable(error));
        QVERIFY(app.model()->entry(copied.id) && *app.model()->entry(copied.id)==copied);
        const auto records=app.model()->entries();
        const auto empty=std::find_if(records.cbegin(),records.cend(),[](const auto& entry) {return entry.title=="empty";});
        QVERIFY(empty!=records.cend());
        QCOMPARE(empty->type,model::EntryType::ApiToken);
        QVERIFY(empty->reverify && empty->token.isEmpty() && empty->note.isEmpty());
    }

    void reverseCollectionTransferRetainsSourcesOnInterruptedMoves() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,platform::hello::request,ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error;
        QVERIFY2(app.createPasswordVault("reverse-test-password",&error),qPrintable(error));
        SecureBytes value,token;
        QCOMPARE(app.issueCollectionToken(token),QString());
        QCOMPARE(app.copyToCollection("source","retry","saved"),QString());
        QCOMPARE(app.copyToCollection("source","agent-race","original"),QString());
        QCOMPARE(app.copyToCollection("source","locked","still here"),QString());

        const auto before=readFile(app.storePathForTest());
        ops.failWrite=true;
        QVERIFY(!app.transferFromCollection("source","retry","saved",{},"Imported",true).isEmpty());
        ops.failWrite=false;
        QCOMPARE(readFile(app.storePathForTest()),before);
        QVERIFY(app.model()->entries().isEmpty() && app.model()->nodes().isEmpty());
        QCOMPARE(app.readCollection("source","retry",value),QString());
        QCOMPARE(value.bytes(),QByteArray("saved"));

        QLockFile lock(dir.filePath(QString::fromLatin1(collection::FileName)+".lock"));
        bool held=false;
        const auto blockDelete=connect(&app,&Application::busyChanged,&app,[&](bool busy) {
            if (!busy) held=lock.tryLock();
        });
        const auto partial=app.transferFromCollection("source","retry","saved",{},"Imported",true);
        disconnect(blockDelete);
        QVERIFY(held && !partial.isEmpty());
        lock.unlock();
        QCOMPARE(app.model()->entries().size(),1);
        const auto copyId=app.model()->entries().cbegin().key();
        const auto destination=app.model()->entry(copyId)->groupId;
        QCOMPARE(app.readCollection("source","retry",value),QString());
        QCOMPARE(value.bytes(),QByteArray("saved"));
        QCOMPARE(app.transferFromCollection("source","retry","saved",destination,{},true),QString());
        QCOMPARE(app.model()->entries().size(),1);
        QVERIFY(app.model()->entry(copyId));
        QVERIFY(!app.readCollection("source","retry",value).isEmpty());

        QString changed=QStringLiteral("Agent update was not attempted");
        const auto agentRace=connect(&app,&Application::busyChanged,&app,[&](bool busy) {
            if (!busy) changed=collection::execute(dir.path(),token.bytes(),"replace","agent-race","updated",value,"source");
        });
        const auto race=app.transferFromCollection("source","agent-race","original",destination,{},true);
        disconnect(agentRace);
        QCOMPARE(changed,QString());
        QVERIFY(!race.isEmpty());
        QCOMPARE(app.readCollection("source","agent-race",value),QString());
        QCOMPARE(value.bytes(),QByteArray("updated"));
        QCOMPARE(app.model()->entries().size(),2);
        const auto saved=app.model()->entries();
        const auto raced=std::find_if(saved.cbegin(),saved.cend(),[](const auto& entry) {return entry.title=="agent-race";});
        QVERIFY(raced!=saved.cend());
        QCOMPARE(raced->token,QString("original"));

        const auto session=app.generation();
        const auto lockAfterSave=connect(&app,&Application::busyChanged,&app,[&](bool busy) {
            if (!busy) app.lock();
        });
        const auto locked=app.transferFromCollection("source","locked","still here",destination,{},true);
        disconnect(lockAfterSave);
        QVERIFY(!locked.isEmpty());
        QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
        QVERIFY(app.generation()>session && app.model()->entries().isEmpty());
        QCOMPARE(collection::execute(dir.path(),token.bytes(),"read","locked",{},value,"source"),QString());
        QCOMPARE(value.bytes(),QByteArray("still here"));
        QVERIFY2(app.unlock("reverse-test-password",&error),qPrintable(error));
        QCOMPARE(app.model()->entries().size(),3);
        QCOMPARE(app.transferFromCollection("source","locked","still here",destination,{},true),QString());
        QCOMPARE(app.model()->entries().size(),3);
        QVERIFY(!app.readCollection("source","locked",value).isEmpty());
    }

    void ownerCollectionBeforeFirstToken() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Application app(dir.path());
        QString error;
        QVERIFY2(app.createPasswordVault("first-test-password",&error),qPrintable(error));
        QStringList names{"stale"};
        SecureBytes value,token;
        const auto binary=QByteArray::fromHex("ff000d0a");
        QCOMPARE(app.listCollection(names),QString());
        QVERIFY(names.isEmpty());
        QCOMPARE(app.copyToCollection("first",binary),QString());
        QVERIFY2(app.switchToPassword("second-test-password",&error),qPrintable(error));
        QCOMPARE(app.listCollection(names),QString());
        QCOMPARE(names,QStringList{"first"});
        QCOMPARE(app.readCollection("first",value),QString());
        QCOMPARE(value.bytes(),binary);
        QCOMPARE(app.updateCollection("first",value.bytes(),"renamed",binary),QString());
        app.lock();
        QVERIFY(!app.listCollection(names).isEmpty());
        QVERIFY(!app.readCollection("renamed",value).isEmpty());
        QVERIFY(!app.updateCollection("renamed",binary,"denied",{}).isEmpty());
        QVERIFY(!app.deleteCollection("renamed",binary).isEmpty());
        QVERIFY(names.isEmpty() && value.isEmpty());
        QVERIFY2(app.unlock("second-test-password",&error),qPrintable(error));
        QCOMPARE(app.issueCollectionToken(token),QString());
        QCOMPARE(collection::execute(dir.path(),token.bytes(),"read","renamed",{},value),QString());
        QCOMPARE(value.bytes(),binary);
        QCOMPARE(app.deleteCollection("renamed",value.bytes()),QString());
        QCOMPARE(app.listCollection(names),QString());
        QVERIFY(names.isEmpty());
    }

    void collectionSurvivesLockRestartAndPasswordChange() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SecureBytes token, second, result;
        QString error;
        {
            Application app(dir.path());
            QVERIFY(app.createPasswordVault("first-test-password",&error));
            QVERIFY(app.issueCollectionToken(token).isEmpty());
            QVERIFY(app.copyToCollection("shared","synthetic").isEmpty());
            app.lock();
            QVERIFY(!app.copyToCollection("other","denied").isEmpty());
            QVERIFY(!app.revokeCollectionToken().isEmpty());
            QVERIFY(collection::execute(dir.path(),token.bytes(),"replace","shared","updated",result).isEmpty());
            QVERIFY(app.unlock("first-test-password",&error));
            QVERIFY(app.switchToPassword("second-test-password",&error));
        }
        Application restarted(dir.path());
        QCOMPARE(restarted.gate(),Application::Gate::NeedUnlock);
        QCOMPARE(collection::execute(dir.path(),token.bytes(),"read","shared",{},result),QString("unauthorized"));
        const auto priorVault=readFile(restarted.storePathForTest()+".bak");
        auto prior=container::openPassword(priorVault,"first-test-password");
        QCOMPARE(prior.status,container::Status::Ok);
        SecureBytes priorPlain(std::move(prior.cbor));
        SecureKey oldOwner;
        QCOMPARE(crypto_kdf_derive_from_key(oldOwner.data(), SecureKey::Size, 1,"TBColOwn",prior.dataKey.data()),0);
        QCOMPARE(collection::issue(dir.path(),oldOwner,second),QString("unauthorized"));
        QVERIFY(restarted.unlock("second-test-password",&error));
        QVERIFY(restarted.issueCollectionToken(second).isEmpty());
        QVERIFY(collection::execute(dir.path(),second.bytes(),"read","shared",{},result).isEmpty());
        QVERIFY(result.bytes()=="updated");
        QVERIFY(restarted.prepareBackupRestore("first-test-password",&error));
        QVERIFY(restarted.restorePreparedBackup().committed);
        QCOMPARE(readFile(restarted.storePathForTest()),priorVault);
        QCOMPARE(collection::execute(dir.path(),token.bytes(),"read","shared",{},result),QString("unauthorized"));
        QCOMPARE(collection::execute(dir.path(),second.bytes(),"read","shared",{},result),QString("unauthorized"));
        QVERIFY(restarted.unlock("first-test-password",&error));
        QVERIFY(restarted.issueCollectionToken(token).isEmpty());
        QVERIFY(collection::execute(dir.path(),token.bytes(),"read","shared",{},result).isEmpty());
        QVERIFY(result.bytes()=="updated");
    }
    void registrySafetyCopyPreservesVerifiedCiphertext_data() {
        QTest::addColumn<int>("fault"); for(int i=0;i<5;++i) QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void registrySafetyCopyPreservesVerifiedCiphertext() {
        QFETCH(int,fault); QTemporaryDir dir; FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,platform::hello::request,ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error; QVERIFY(app.createPasswordVault("valid-password",&error)); const auto before=readFile(app.storePathForTest()); const auto generation=app.generation();
        if(fault==1) ops.failWrite=true;
        if(fault==2) ops.corruptWriteCall=ops.writeCalls+1;
        if(fault==3) ops.beforeWrite=[&](const QString&) {QMetaObject::invokeMethod(&app,&Application::lock,Qt::BlockingQueuedConnection);};
        if(fault==4) {QVERIFY(store::realFileOps().removeFile(app.storePathForTest())); QVERIFY(store::realFileOps().writeAll(app.storePathForTest(),"different-synthetic"));}
        const auto path=app.preserveBeforeRegistryMigration(&error);
        if(fault!=4) QCOMPARE(readFile(app.storePathForTest()),before);
        if(fault) {QVERIFY(path.isEmpty()); QVERIFY(!error.isEmpty());}
        else {
            QVERIFY(!path.isEmpty()); QVERIFY(path.contains(".before-registry-migration-")); QCOMPARE(readFile(path),before); QCOMPARE(app.generation(),generation);
            const auto second=app.preserveBeforeRegistryMigration(&error); QVERIFY(!second.isEmpty()); QVERIFY(second!=path); QCOMPARE(readFile(second),before);
            QVERIFY(app.save(&error)); app.lock(); QVERIFY(app.unlock("valid-password",&error));
        }
        if(fault==3) QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
    }
    void v4VaultUpgradesToV5WithConsent() {
        QTemporaryDir dir; QVERIFY(store::ensurePrivateDir(dir.path())); crypto::init();
        model::VaultModel source; const auto group=QUuid::createUuid(); QVERIFY(source.addNode(group,{},"Demo").isEmpty());
        auto root=QCborValue::fromCbor(source.toCbor()).toMap(); root[QStringLiteral("v")]=4; root.remove(QStringLiteral("views"));
        const auto bytes=container::packPassword("valid-password",QCborValue(root).toCbor(),nullptr); const auto path=dir.filePath("vault.tb");
        QVERIFY(store::realFileOps().writeAll(path,bytes));
        Application app(dir.path()); QString error; QVERIFY(app.unlock("valid-password",&error)); QCOMPARE(app.model()->decodedVersion(),4);
        QVERIFY(app.schemaUpgradeRequired()); QVERIFY(!app.save(&error)); QCOMPARE(readFile(path),bytes);
        QVERIFY(app.approveSchemaUpgrade(&error)); QVERIFY(app.save(&error)); QCOMPARE(readFile(app.upgradeSafetyPath()),bytes);
        app.lock(); QVERIFY(app.unlock("valid-password",&error)); QCOMPARE(app.model()->decodedVersion(),model::VaultModel::CurrentVersion); QVERIFY(!app.schemaUpgradeRequired());
    }
    void v5SshVaultUpgradePreservesEncryptedOriginalAndBindings() {
        QTemporaryDir directory;
        QVERIFY(store::ensurePrivateDir(directory.path()));
        crypto::init();
        model::VaultModel source;
        model::Entry key;
        key.groupId=QUuid::createUuid(); key.id=QUuid::createUuid(); key.type=model::EntryType::SshKey;
        key.title="synthetic shared key"; key.sshPrivate=QByteArray("private\0\r\n", 10);
        key.sshPublic="synthetic public key"; key.sshPassphrase="synthetic passphrase";
        QVERIFY(source.addNode(key.groupId, {}, "machines").isEmpty());
        QVERIFY(source.setEntry(key).isEmpty());
        model::SavedView view; view.name="saved SSH view"; view.type=2;
        QVERIFY(source.setViews({view}).isEmpty());
        auto root=QCborValue::fromCbor(source.toCbor()).toMap(); root[QStringLiteral("v")]=5;
        auto entries=root.value(QStringLiteral("entries")).toArray(); auto entry=entries.at(0).toMap();
        entry.remove(QStringLiteral("sshTargets")); entries[0]=entry; root[QStringLiteral("entries")]=entries;
        const auto oldFile=container::packPassword("valid-password",QCborValue(root).toCbor(),nullptr);
        const auto file=directory.filePath("vault.tb");
        QVERIFY(store::realFileOps().writeAll(file,oldFile));
        FaultOps ops;
#ifdef Q_OS_WIN
        Application app(directory.path(),nullptr,platform::hello::request,ops);
#else
        Application app(directory.path(),nullptr,ops);
#endif
        QString error;
        QVERIFY2(app.unlock("valid-password",&error),qPrintable(error));
        QCOMPARE(app.model()->decodedVersion(),5);
        QVERIFY(app.model()->entry(key.id)->sshTargets.isEmpty());
        QVERIFY(app.schemaUpgradeRequired());
        key.sshTargets={{"Build lab","build.example","build",2222},{"Recovery","192.0.2.18","ops",22}};
        QVERIFY(app.model()->setEntry(key).isEmpty());
        QVERIFY(!app.save(&error));
        QCOMPARE(readFile(file),oldFile);
        QVERIFY(app.approveSchemaUpgrade(&error));
        ops.failWrite=true;
        QVERIFY(!app.save(&error));
        QCOMPARE(readFile(file),oldFile);
        ops.failWrite=false;
        QVERIFY2(app.save(&error),qPrintable(error));
        QCOMPARE(readFile(app.upgradeSafetyPath()),oldFile);
        app.lock();
        QVERIFY2(app.unlock("valid-password",&error),qPrintable(error));
        QCOMPARE(app.model()->decodedVersion(),6);
        QCOMPARE(app.model()->entry(key.id)->sshPrivate,key.sshPrivate);
        QCOMPARE(app.model()->entry(key.id)->sshPublic,key.sshPublic);
        QCOMPARE(app.model()->entry(key.id)->sshPassphrase,key.sshPassphrase);
        QVERIFY(app.model()->entry(key.id)->sshTargets==key.sshTargets);
        QVERIFY(app.model()->views()==QList<model::SavedView>{view});
        const auto exported=app.exportContainer("export-password",&error);
        QVERIFY2(!exported.isEmpty(),qPrintable(error));
        QTemporaryDir destination;
        Application restored(destination.path());
        QVERIFY2(restored.importContainer(exported,"export-password","new-local-password",&error),qPrintable(error));
        QVERIFY(*restored.model()->entry(key.id)==*app.model()->entry(key.id));
        QVERIFY(restored.model()->views()==app.model()->views());
    }
    void legacySaveRequiresExplicitConsent() {
        QTemporaryDir dir; QVERIFY(store::ensurePrivateDir(dir.path())); const auto bytes=legacyV3(); QVERIFY(!bytes.isEmpty());
        const auto path=dir.filePath("vault.tb"); QVERIFY(store::realFileOps().writeAll(path,bytes));
        Application app(dir.path()); QString error; QVERIFY(app.unlock("valid-password",&error));
        QCOMPARE(app.model()->decodedVersion(),3);
        QVERIFY(!app.save(&error)); QVERIFY(!error.isEmpty()); QCOMPARE(readFile(path),bytes);
        QVERIFY(app.schemaUpgradeRequired()); QVERIFY(app.approveSchemaUpgrade(&error)); QVERIFY(app.save(&error));
        const auto safety=app.upgradeSafetyPath(); QVERIFY(!safety.isEmpty()); QCOMPARE(readFile(safety),bytes);
        QVERIFY(!readFile(path).contains("synthetic-legacy-token")); QVERIFY(!app.schemaUpgradeRequired());
        QVERIFY(app.save(&error)); QCOMPARE(app.upgradeSafetyPath(),safety); QCOMPARE(readFile(safety),bytes);
        app.lock(); QVERIFY(app.unlock("valid-password",&error)); QVERIFY(!app.schemaUpgradeRequired());
        QCOMPARE(app.model()->decodedVersion(),model::VaultModel::CurrentVersion);
    }
    void legacyWriterRoutes_data() {
        QTest::addColumn<int>("route"); QTest::newRow("save")<<0; QTest::newRow("password")<<1; QTest::newRow("import")<<2;
#ifdef Q_OS_WIN
        QTest::newRow("hello")<<3;
#endif
    }
    void legacyWriterRoutes() {
        QFETCH(int,route); QTemporaryDir dir; QVERIFY(store::ensurePrivateDir(dir.path()));
        const auto bytes=legacyV3(); const auto path=dir.filePath("vault.tb"); QVERIFY(store::realFileOps().writeAll(path,bytes));
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,[](void*,const QString&) {return platform::hello::Result::Verified;});
#else
        Application app(dir.path());
#endif
        QString error; QVERIFY(app.unlock("valid-password",&error));
        const auto write=[&] {
            if(route==1) return app.switchToPassword("new-valid-password",&error);
            if(route==2) return app.importContainer(bytes,"valid-password","new-valid-password",&error);
#ifdef Q_OS_WIN
            if(route==3) return app.switchToHello(&error);
#endif
            return app.save(&error);
        };
        QVERIFY(!write()); QCOMPARE(readFile(path),bytes); QVERIFY(app.approveSchemaUpgrade(&error));
        QVERIFY2(write(),qPrintable(error)); const auto safety=app.upgradeSafetyPath(); QVERIFY(!safety.isEmpty()); QCOMPARE(readFile(safety),bytes);
        QVERIFY(QCborValue::fromCbor(container::openPassword(readFile(safety),"valid-password").cbor).toMap().value(QStringLiteral("v")).toInteger()==3);
        app.lock();
#ifdef Q_OS_WIN
        if(route==3) QVERIFY(app.unlockHello(&error)); else
#endif
        QVERIFY(app.unlock(route==0?"valid-password":"new-valid-password",&error));
        QCOMPARE(app.model()->decodedVersion(),model::VaultModel::CurrentVersion); QCOMPARE(app.model()->entries().size(),1);
        QCOMPARE(app.model()->entries().cbegin()->token,QString("synthetic-legacy-token"));
    }
    void upgradeFailures_data() {
        QTest::addColumn<int>("fault");
        const char* names[]={"safety-write","safety-corrupt","main-changed","lock-in-safety","lock-in-temporary",
            "temporary-write","final-rename","lock-after-final-rename","approval-revoked","cached-copy-removed","main-changed-during-write",
            "symlink-main","lock-inside-final-rename"};
        for(int i=0;i<int(std::size(names));++i) QTest::newRow(names[i])<<i;
    }
    void upgradeFailures() {
        QFETCH(int,fault); QTemporaryDir dir; QVERIFY(store::ensurePrivateDir(dir.path()));
        const auto bytes=legacyV3(); const auto path=dir.filePath("vault.tb"); QVERIFY(store::realFileOps().writeAll(path,bytes)); FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,platform::hello::request,ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error; QVERIFY(app.unlock("valid-password",&error)); QVERIFY(app.approveSchemaUpgrade(&error));
        const auto changed=container::packPassword("valid-password",app.model()->toCbor(),nullptr);
        const auto replace=[&] {return QFile::remove(path) && store::realFileOps().writeAll(path,changed);};
        if(fault==0) ops.failWriteCall=1;
        if(fault==1) ops.corruptWriteCall=1;
        if(fault==2) QVERIFY(replace());
        if(fault==3 || fault==4) ops.beforeWrite=[&](const QString& target) {
            if((fault==3 && target.contains(".before-upgrade-")) || (fault==4 && target.endsWith("vault.tb.tmp")))
                QMetaObject::invokeMethod(&app,&Application::lock,Qt::BlockingQueuedConnection);
        };
        if(fault==5 || fault==9) ops.failWriteCall=2;
        if(fault==6) ops.failRenameCall=2;
        if(fault==7) ops.afterRename=[&](const QString&,const QString& to) {
            if(to==path) QMetaObject::invokeMethod(&app,&Application::lock,Qt::BlockingQueuedConnection);
        };
        if(fault==12) ops.beforeRename=[&](const QString&,const QString& to) {
            if(to==path) QMetaObject::invokeMethod(&app,&Application::lock,Qt::BlockingQueuedConnection);
        };
        if(fault==8) {app.lock(); QVERIFY(app.unlock("valid-password",&error));}
        bool changedDuringWrite=false;
        if(fault==10) ops.beforeWrite=[&](const QString& target) {if(target.endsWith("vault.tb.tmp")) changedDuringWrite=replace();};
        if(fault==11) {
            const auto target=dir.filePath("other.tb"); QVERIFY(store::realFileOps().writeAll(target,bytes)); QVERIFY(QFile::remove(path));
#ifdef Q_OS_WIN
            QVERIFY(CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(path.utf16()),reinterpret_cast<LPCWSTR>(target.utf16()),SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
#else
            QVERIFY(QFile::link(target,path));
#endif
        }
        const bool saved=app.save(&error); QVERIFY(!saved); QVERIFY(!error.isEmpty());
        if(fault==7 || fault==12) {
            QVERIFY(error.contains(QStringLiteral("Файл записан"))); QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
            QVERIFY(readFile(path)!=bytes); QVERIFY(app.unlock("valid-password",&error)); QCOMPARE(app.model()->decodedVersion(),model::VaultModel::CurrentVersion);
        } else QCOMPARE(readFile(path),(fault==2 || fault==10)?changed:bytes);
        if(fault==10) QVERIFY(changedDuringWrite);
        if(fault==11) QVERIFY(app.upgradeSafetyPath().isEmpty());
        if(fault==9) {
            const auto first=app.upgradeSafetyPath(); QVERIFY(!first.isEmpty()); QVERIFY(QFile::remove(first)); ops=FaultOps{};
            QVERIFY(app.save(&error)); QVERIFY(app.upgradeSafetyPath()!=first); QCOMPARE(readFile(app.upgradeSafetyPath()),bytes);
        }
        if(fault==0 || fault==1 || fault==5 || fault==6) {
            ops=FaultOps{}; QVERIFY2(app.save(&error),qPrintable(error)); QCOMPARE(readFile(app.upgradeSafetyPath()),bytes);
        }
    }
    void exportingLegacyDoesNotUpgradeMain() {
        QTemporaryDir dir; QVERIFY(store::ensurePrivateDir(dir.path())); const auto bytes=legacyV3(); const auto path=dir.filePath("vault.tb");
        QVERIFY(store::realFileOps().writeAll(path,bytes)); Application app(dir.path()); QString error;
        QVERIFY(app.unlock("valid-password",&error)); const auto exported=app.exportContainer("export-password",&error); QVERIFY(!exported.isEmpty());
        QCOMPARE(readFile(path),bytes); QVERIFY(app.schemaUpgradeRequired()); QVERIFY(app.upgradeSafetyPath().isEmpty());
        auto opened=container::openTransfer(exported,"export-password");
        QCOMPARE(opened.status,container::Status::Ok);
        SecureBytes plain(std::move(opened.cbor));
        const auto payload=container::transferPayload(plain.bytes()); QVERIFY(payload && payload->collection);
        model::VaultModel decoded;
        QCOMPARE(decoded.fromCbor(QByteArray::fromRawData(payload->model.data(),payload->model.size()),&error),model::VaultModel::ParseStatus::Ok);
        QCOMPARE(decoded.decodedVersion(),model::VaultModel::CurrentVersion); QVERIFY(!app.save(&error));
    }
    void lateModeSwitchKeepsActualHint_data() {
        QTest::addColumn<bool>("hello"); QTest::newRow("password")<<false;
#ifdef Q_OS_WIN
        QTest::newRow("hello")<<true;
#endif
    }
    void lateModeSwitchKeepsActualHint() {
        QFETCH(bool,hello); QTemporaryDir dir; FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,[](void*,const QString&) {return platform::hello::Result::Verified;},ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto path=app.storePathForTest();
        ops.afterRename=[&](const QString&,const QString& to) {if(to==path) QMetaObject::invokeMethod(&app,&Application::lock,Qt::BlockingQueuedConnection);};
#ifdef Q_OS_WIN
        const bool changed=hello?app.switchToHello(&error):app.switchToPassword("new-valid-password",&error);
#else
        const bool changed=app.switchToPassword("new-valid-password",&error);
#endif
        QVERIFY(!changed); QVERIFY(error.contains(QStringLiteral("Файл записан"))); QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
        QCOMPARE(app.unlockMode(),hello?Application::Mode::Hello:Application::Mode::Password);
#ifdef Q_OS_WIN
        if(hello) QVERIFY(app.unlockHello(&error)); else
#endif
        QVERIFY(app.unlock("new-valid-password",&error));
        QCOMPARE(app.model()->decodedVersion(),model::VaultModel::CurrentVersion);
    }
    void lateUnlockDoesNotClaimWrite() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto bytes=readFile(app.storePathForTest()); app.lock();
        QTimer::singleShot(0,&app,&Application::lock); QVERIFY(!app.unlock("valid-password",&error));
        QVERIFY(!error.contains(QStringLiteral("Файл записан"))); QCOMPARE(readFile(app.storePathForTest()),bytes);
        QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
    }
    void restoringLegacyRequiresNewUpgradeApproval() {
        QTemporaryDir dir; QVERIFY(store::ensurePrivateDir(dir.path())); const auto bytes=legacyV3(); const auto path=dir.filePath("vault.tb");
        QVERIFY(store::realFileOps().writeAll(path,bytes)); Application app(dir.path()); QString error;
        QVERIFY(app.unlock("valid-password",&error)); QVERIFY(app.approveSchemaUpgrade(&error)); QVERIFY(app.save(&error));
        QCOMPARE(app.backupMode(&error),Application::Mode::Password); QVERIFY(app.prepareBackupRestore("valid-password",&error));
        QVERIFY(app.restorePreparedBackup().committed); QCOMPARE(readFile(path),bytes);
        QVERIFY(app.unlock("valid-password",&error)); QVERIFY(app.schemaUpgradeRequired()); QVERIFY(!app.save(&error));
        QVERIFY(app.approveSchemaUpgrade(&error)); QVERIFY(app.save(&error));
    }
    void lockDuringTemporaryWritePreservesMain() {
        QTemporaryDir dir; FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,platform::hello::request,ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto before=readFile(app.storePathForTest());
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Not committed").isEmpty());
        bool locked=false;
        ops.beforeWrite=[&](const QString& path) {
            if(path.endsWith("vault.tb.tmp")) QMetaObject::invokeMethod(&app,[&] {locked=true; app.lock();},Qt::BlockingQueuedConnection);
        };
        QVERIFY(!app.save(&error)); QVERIFY(locked); QCOMPARE(readFile(app.storePathForTest()),before);
        QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
    }
    void backupHintBindsCiphertextBeforeAuthentication() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("valid-password",&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Original").isEmpty()); QVERIFY(app.save(&error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(),{},"Newer").isEmpty()); QVERIFY(app.save(&error));
        const auto before=readFile(app.storePathForTest());
        QCOMPARE(app.backupMode(&error),Application::Mode::Password);
        model::VaultModel other; const auto replacement=QUuid::createUuid();
        QVERIFY(other.addNode(replacement,{},"Different backup").isEmpty());
        const auto changed=container::packPassword("valid-password",other.toCbor(),nullptr); QVERIFY(!changed.isEmpty());
        const auto backup=app.storePathForTest()+".bak";
        QVERIFY(QFile::remove(backup)); QVERIFY(store::realFileOps().writeAll(backup,changed));
        QVERIFY(!app.prepareBackupRestore("valid-password",&error)); QVERIFY(!error.isEmpty());
        QVERIFY(!app.restorePreparedBackup().committed); QCOMPARE(readFile(app.storePathForTest()),before);
        QCOMPARE(app.backupMode(&error),Application::Mode::Password);
        QVERIFY(app.prepareBackupRestore("valid-password",&error)); QVERIFY(app.restorePreparedBackup().committed);
        QVERIFY(app.unlock("valid-password",&error)); QCOMPARE(app.model()->nodes().size(),1);
        QCOMPARE(app.model()->nodes().value(replacement).name,QStringLiteral("Different backup"));
    }
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
        const auto binary=QByteArray::fromHex("ff000d0a");
        QCOMPARE(src.copyToCollection("before",binary),QString());
        QCOMPARE(src.updateCollection("before",binary,"binary",binary),QString());
        QCOMPARE(src.copyToCollection("Project A","binary","grouped"),QString());
        QCOMPARE(src.copyToCollection("empty",{}),QString());
        SecureBytes sourceToken, value;
        QCOMPARE(src.issueCollectionToken(sourceToken),QString());
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
        QStringList names;
        QCOMPARE(dst.listCollection(names),QString());
        QCOMPARE(names,(QStringList{"binary","empty"}));
        QCOMPARE(dst.readCollection("binary",value),QString());
        QCOMPARE(value.bytes(),binary);
        QMap<QString,QStringList> groups;
        QCOMPARE(dst.listCollection(groups),QString());
        QCOMPARE(groups.value("Project A"),QStringList{"binary"});
        QCOMPARE(dst.readCollection("Project A","binary",value),QString());
        QCOMPARE(value.bytes(),QByteArray("grouped"));
        QCOMPARE(dst.readCollection("empty",value),QString());
        QVERIFY(value.isEmpty());
        QCOMPARE(collection::execute(dir_.filePath("dst"),sourceToken.bytes(),"read","binary",{},value),QString("unauthorized"));
        QCOMPARE(collection::execute(dir_.filePath("export-src"),sourceToken.bytes(),"read","binary",{},value),QString());
        QCOMPARE(value.bytes(),binary);
    }
    void importCollectionPolicy_data() {
        QTest::addColumn<int>("format");
        QTest::newRow("full")<<0;
        QTest::newRow("full-empty")<<1;
        QTest::newRow("legacy-model-only")<<2;
    }
    void importCollectionPolicy() {
        QFETCH(int,format);
        QTemporaryDir source,destination;
        Application src(source.path()), dst(destination.path()); QString error;
        QVERIFY(src.createPasswordVault("source-password",&error));
        QVERIFY(dst.createPasswordVault("destination-password",&error));
        const auto newNode=QUuid::createUuid(), oldNode=QUuid::createUuid();
        QCOMPARE(src.model()->addNode(newNode,{},"Imported"),QString());
        QCOMPARE(dst.model()->addNode(oldNode,{},"Replaced"),QString());
        if(format==0) QCOMPARE(src.copyToCollection("incoming","new"),QString());
        QCOMPARE(dst.copyToCollection("old","existing"),QString());
        SecureBytes oldToken, value, newToken;
        QCOMPARE(dst.issueCollectionToken(oldToken),QString());
        const auto bytes=format==2 ? container::packPassword("export-password",src.model()->toCbor(),nullptr)
                                   : src.exportContainer("export-password",&error);
        QVERIFY2(!bytes.isEmpty(),qPrintable(error));
        const auto oldSession=dst.generation();
        quint64 refreshedSession=oldSession;
        connect(&dst,&Application::stateChanged,&dst,[&] {refreshedSession=dst.generation();});
        QVERIFY2(dst.importContainer(bytes,"export-password","new-local-password",&error),qPrintable(error));
        QCOMPARE(dst.generation(),oldSession+1);
        QCOMPARE(refreshedSession,dst.generation());
        QVERIFY(dst.model()->nodes().contains(newNode)); QVERIFY(!dst.model()->nodes().contains(oldNode));
        QCOMPARE(collection::execute(destination.path(),oldToken.bytes(),"read","old",{},value),QString("unauthorized"));
        dst.lock(); QVERIFY2(dst.unlock("new-local-password",&error),qPrintable(error));
        QStringList names;
        QCOMPARE(dst.listCollection(names),QString());
        if(format==1) QVERIFY(names.isEmpty());
        else {
            const QString name=format==0 ? "incoming" : "old";
            QCOMPARE(names,QStringList{name});
            QCOMPARE(dst.issueCollectionToken(newToken),QString());
            QCOMPARE(collection::execute(destination.path(),newToken.bytes(),"read",name,{},value),QString());
            QCOMPARE(value.bytes(),format==0 ? QByteArray("new") : QByteArray("existing"));
        }
    }
    void invalidTransferKeepsDestination() {
        QTemporaryDir destination;
        const auto path=destination.filePath("vault.tb");
        const auto original=legacyV3();
        QVERIFY(!original.isEmpty());
        QVERIFY(store::ensurePrivateDir(destination.path()));
        QVERIFY(store::realFileOps().writeAll(path,original));
        Application dst(destination.path()); QString error;
        QVERIFY(dst.unlock("valid-password",&error));
        QVERIFY(dst.approveSchemaUpgrade(&error));
        QCOMPARE(dst.copyToCollection("keep","existing"),QString());
        SecureBytes token, value;
        QCOMPARE(dst.issueCollectionToken(token),QString());
        const auto collectionPath=destination.filePath(collection::FileName);
        const auto originalCollection=readFile(collectionPath);
        model::VaultModel model;
        const auto valid=container::packTransfer("export-password",model.toCbor(),{});
        auto opened=container::openTransfer(valid,"export-password");
        QCOMPARE(opened.status,container::Status::Ok);
        opened.cbor.chop(1); // authenticated envelope with a missing block byte
        SecureBytes truncated(std::move(opened.cbor));
        const auto malformed=container::packPassword("export-password",truncated.bytes(),nullptr);
        const auto invalidEntries=container::packTransfer("export-password",model.toCbor(),"x");
        const auto invalidModel=container::packTransfer("export-password","x",{});
        QVERIFY(!malformed.isEmpty() && !invalidEntries.isEmpty() && !invalidModel.isEmpty());
        QVERIFY(!dst.importContainer(valid,"wrong-password","new-local-password",&error));
        for(const auto& rejected:{malformed,invalidEntries,invalidModel}) {
            QVERIFY(!dst.importContainer(rejected,"export-password","new-local-password",&error));
            QVERIFY(!error.isEmpty());
        }
        QCOMPARE(readFile(path),original);
        QCOMPARE(readFile(collectionPath),originalCollection);
        QCOMPARE(dst.gate(),Application::Gate::Ready);
        QVERIFY(dst.upgradeSafetyPath().isEmpty()); QVERIFY(!QFile::exists(path+".bak"));
        QCOMPARE(collection::execute(destination.path(),token.bytes(),"read","keep",{},value),QString());
        QCOMPARE(value.bytes(),QByteArray("existing"));
    }
    void legacyImportRejectsUnownedCollection() {
        QTemporaryDir destination; QString error;
        auto path=destination.filePath("vault.tb");
        {
            Application app(destination.path());
            QVERIFY(app.createPasswordVault("destination-password",&error));
            QCOMPARE(app.copyToCollection("old","existing"),QString());
        }
        QVERIFY(QFile::remove(path));
        const auto collectionPath=destination.filePath(collection::FileName);
        const auto original=readFile(collectionPath);
        QVERIFY(!original.isEmpty());
        Application dst(destination.path());
        QVERIFY(!dst.importContainer(legacyV3(),"valid-password","new-local-password",&error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(dst.gate(),Application::Gate::NeedSetup);
        QVERIFY(!QFile::exists(path));
        QCOMPARE(readFile(collectionPath),original);
    }
    void transferCapacityExceedsLocalLimit() {
        const QByteArray model(container::MaxPlainBytes+1,'m');
        const QByteArray entries(collection::MaxSnapshotBytes+1,'e');
        QVERIFY(container::packTransfer("export-password",model,{}).isEmpty());
        QVERIFY(container::packTransfer("export-password","m",entries).isEmpty());
        const auto modelBytes=QByteArrayView(model).first(container::MaxPlainBytes);
        const auto collectionBytes=QByteArrayView(entries).first(collection::MaxSnapshotBytes);
        const auto bytes=container::packTransfer("export-password",modelBytes,collectionBytes);
        QCOMPARE(bytes.size(),container::MaxTransferFileBytes);
        QCOMPARE(container::openPassword(bytes,"export-password").status,container::Status::TooLarge);
        auto opened=container::openTransfer(bytes,"export-password");
        QCOMPARE(opened.status,container::Status::Ok);
        SecureBytes plain(std::move(opened.cbor));
        const auto payload=container::transferPayload(plain.bytes()); QVERIFY(payload && payload->collection);
        QVERIFY(payload->model==modelBytes); QVERIFY(*payload->collection==collectionBytes);
    }
    void lockOnExportCompletionCancelsBytes() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("valid-password",&error));
        connect(&app,&Application::busyChanged,&app,[&](bool busy) {if(!busy) app.lock();});
        QVERIFY(app.exportContainer("export-password",&error).isEmpty());
        QVERIFY(!error.isEmpty()); QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
    }
    void interruptedTransfer_data() {
        QTest::addColumn<int>("interruption");
        QTest::newRow("rename-failed")<<0;
        QTest::newRow("locked-before-commit")<<1;
        QTest::newRow("locked-after-commit")<<2;
    }
    void interruptedTransfer() {
        QFETCH(int,interruption);
        QTemporaryDir source,destination;
        Application src(source.path()); FaultOps ops;
#ifdef Q_OS_WIN
        Application dst(destination.path(),nullptr,platform::hello::request,ops);
#else
        Application dst(destination.path(),nullptr,ops);
#endif
        QString error;
        QVERIFY(src.createPasswordVault("source-password",&error));
        QVERIFY(dst.createPasswordVault("destination-password",&error));
        QCOMPARE(src.copyToCollection("incoming","new"),QString());
        QCOMPARE(dst.copyToCollection("old","existing"),QString());
        SecureBytes token,value;
        QCOMPARE(dst.issueCollectionToken(token),QString());
        const auto transfer=src.exportContainer("export-password",&error);
        QVERIFY2(!transfer.isEmpty(),qPrintable(error));
        const auto path=dst.storePathForTest(), collectionPath=destination.filePath(collection::FileName);
        const auto oldVault=readFile(path), oldCollection=readFile(collectionPath);
        if(interruption==0) ops.failRenameCall=ops.renameCalls+2;
        if(interruption==1) ops.beforeWrite=[&](const QString& to) {
            if(to==path+".tmp") QMetaObject::invokeMethod(&dst,&Application::lock,Qt::BlockingQueuedConnection);
        };
        if(interruption==2) ops.afterRename=[&](const QString&,const QString& to) {
            if(to==path) QMetaObject::invokeMethod(&dst,&Application::lock,Qt::BlockingQueuedConnection);
        };
        QVERIFY(!dst.importContainer(transfer,"export-password","new-local-password",&error));
        QVERIFY(!error.isEmpty());
        if(interruption==2) {
            QCOMPARE(dst.gate(),Application::Gate::NeedUnlock);
            QVERIFY(dst.unlock("new-local-password",&error));
            QCOMPARE(dst.readCollection("incoming",value),QString());
            QCOMPARE(value.bytes(),QByteArray("new"));
            QCOMPARE(collection::execute(destination.path(),token.bytes(),"read","old",{},value),QString("unauthorized"));
        } else {
            QCOMPARE(readFile(path),oldVault);
            QCOMPARE(readFile(collectionPath),oldCollection);
            QCOMPARE(collection::execute(destination.path(),token.bytes(),"read","old",{},value),QString());
            QCOMPARE(value.bytes(),QByteArray("existing"));
        }
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
