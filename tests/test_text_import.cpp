#include <QtTest>
#include "../src/io/TextImport.h"
#include "../src/model/TextFile.h"
using namespace tb;
class TestTextImport:public QObject {
    Q_OBJECT
    static model::Id group(model::VaultModel& model) {const auto id=QUuid::createUuid(); if(!model.addNode(id,{},"Demo").isEmpty()) return {}; return id;}
private slots:
    void knownLabelsStayUsefulAndUnknownNamesStayHidden() {
        model::VaultModel base; const auto id=group(base); auto p=io::parseTextFile("PASSWORD=synthetic-password\nsynthetic-sensitive-name=synthetic-value\n","demo.txt"); p.source.groupId=id;
        const auto choices=io::defaultRegistrySelections(base,p); QCOMPARE(choices.first().title,QString("PASSWORD"));
        QVERIFY(!choices.last().title.contains("sensitive-name")); QVERIFY(choices.last().parameterName.isEmpty());
    }
    void sourceAndRepeatedNamesAreLossless() {
        const QByteArray bytes=QByteArray::fromHex("efbbbf")+"# synthetic password in comment\r\nPUBLIC_IP=192.0.2.10\r\nTOKEN=synthetic\r\nPUBLIC_IP=192.0.2.20\r\nunknown line\r\n";
        const auto p=io::parseTextFile(bytes,"demo.txt"); QVERIFY(p.error.isEmpty()); QCOMPARE(p.source.textFile.bytes,bytes);
        QVERIFY(p.source.textFile.sourceSnapshot); QVERIFY(p.source.textFile.visibleLines.isEmpty()); QCOMPARE(p.parameters.size(),3);
        QCOMPARE(p.parameters[0].line,1); QCOMPARE(p.parameters[2].line,3); QCOMPARE(p.parameters[0].name,p.parameters[2].name);
        QCOMPARE(p.parameters[0].suggestedType,model::EntryType::PlainValue); QCOMPARE(p.parameters[1].suggestedType,model::EntryType::ApiToken);
        QVERIFY(!p.source.title.contains("synthetic"));
    }
    void classification_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<QString>("value"); QTest::addColumn<bool>("plain");
        QTest::newRow("ipv4")<<QString("PUBLIC_IP")<<QString("192.0.2.10")<<true;
        QTest::newRow("ipv6")<<QString("IP")<<QString("2001:db8::1")<<true;
        QTest::newRow("bad-ip")<<QString("IP")<<QString("192.999.1.1")<<false;
        QTest::newRow("secret-name-wins")<<QString("TOKEN_IP")<<QString("192.0.2.10")<<false;
        QTest::newRow("unknown-name")<<QString("synthetic-sensitive-name")<<QString("192.0.2.10")<<false;
        QTest::newRow("public-url")<<QString("APP_URL")<<QString("https://example.test/path")<<true;
        QTest::newRow("url-userinfo")<<QString("APP_URL")<<QString("https://demo:synthetic@example.test/")<<false;
        QTest::newRow("url-query")<<QString("APP_URL")<<QString("https://example.test/?token=synthetic")<<false;
        QTest::newRow("url-fragment")<<QString("URL")<<QString("https://example.test/#synthetic")<<false;
        QTest::newRow("database-url")<<QString("DATABASE_URL")<<QString("postgres://demo:synthetic@192.0.2.10/db")<<false;
        QTest::newRow("file-url")<<QString("URL")<<QString("file:///C:/fake.txt")<<false;
        QTest::newRow("project")<<QString("PROJECT_ID")<<QString("a130cfaa-8317-4b60-8d14-6b1700059edf")<<true;
        QTest::newRow("username")<<QString("USERNAME")<<QString::fromUtf8("тестовый")<<true;
        QTest::newRow("credentials-in-username")<<QString("USERNAME")<<QString("demo:synthetic-password")<<false;
        QTest::newRow("db-name")<<QString("DB_NAME")<<QString("sample_db")<<true;
        QTest::newRow("version")<<QString("VERSION")<<QString("16.2")<<true;
        QTest::newRow("shell")<<QString("PROJECT_ID")<<QString("$(do-not-run)")<<false;
    }
    void classification() {QFETCH(QString,name); QFETCH(QString,value); QFETCH(bool,plain); QCOMPARE(io::suggestStorageType(name,value),plain?model::EntryType::PlainValue:model::EntryType::ApiToken);}
    void syntaxIsOnlyText() {
        const auto p=io::parseTextFile("[VM]\r\nexport PUBLIC_IP=\"192.0.2.10\"\r\nPASSWORD: 'a\\nb # literal'\r\nEMPTY=\r\n[unknown synthetic]\r\nID=demo\r\nunparsed sensitive heading\r\nID=other\r\n","demo.txt");
        QVERIFY(p.error.isEmpty()); QCOMPARE(p.parameters.size(),5); QCOMPARE(p.parameters[0].value,QString("192.0.2.10"));
        QCOMPARE(p.parameters[1].value,QString("a\\nb # literal")); QVERIFY(p.parameters[2].value.isEmpty());
        QCOMPARE(p.parameters[0].section,QStringLiteral("Виртуальная машина")); QVERIFY(p.parameters[3].section.isEmpty()); QVERIFY(p.parameters[4].section.isEmpty());
        const auto spaced=io::parseTextFile("PASSWORD=  keep spaces  \n","demo.txt"); QCOMPARE(spaced.parameters.first().value,QString("  keep spaces  "));
    }
    void invalidSources_data() {
        QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<QString>("name");
        QTest::newRow("utf8")<<QByteArray::fromHex("ff")<<QString("demo.txt");
        QTest::newRow("nul")<<QByteArray("a\0b",3)<<QString("demo.txt");
        QTest::newRow("size")<<QByteArray(model::VaultModel::MaxSecretBytes+1,'a')<<QString("demo.txt");
        QTest::newRow("rows")<<QByteArray(50000,'\n')<<QString("demo.txt");
        QTest::newRow("path")<<QByteArray("IP=192.0.2.1")<<QString("../demo.txt");
    }
    void invalidSources() {QFETCH(QByteArray,bytes); QFETCH(QString,name); const auto p=io::parseTextFile(bytes,name); QVERIFY(!p.error.isEmpty()); QVERIFY(p.parameters.isEmpty());}
    void atomicCandidateAndOrigins() {
        model::VaultModel base; const auto id=group(base); auto p=io::parseTextFile("PUBLIC_IP=192.0.2.10\nTOKEN=synthetic\nPUBLIC_IP=192.0.2.20\n# hidden\n","demo.txt"); p.source.groupId=id; p.source.reverify=true;
        auto choices=io::defaultRegistrySelections(base,p); p.source.textFile.visibleLines={0,2}; model::VaultModel out; QString error;
        QVERIFY2(io::buildRegistryCandidate(base,p,choices,&out,&error),qPrintable(error)); QVERIFY(base.entries().isEmpty()); QCOMPARE(out.entries().size(),4);
        QSet<QString> titles; int ips=0;
        for(const auto& e:out.entries()) if(e.id!=p.source.id) {
            QVERIFY(e.reverify); QCOMPARE(e.sourceEntryId,p.source.id); QVERIFY(e.sourceLine>=0); titles.insert(e.title);
            if(e.type==model::EntryType::PlainValue) {++ips; QCOMPARE(e.parameterName,QString("PUBLIC_IP"));}
            else {QCOMPARE(e.token,QString("synthetic")); QVERIFY(e.parameterName.isEmpty()); QVERIFY(!e.title.contains("synthetic"));}
        }
        QCOMPARE(ips,2); QCOMPARE(titles.size(),3); QCOMPARE(out.entry(p.source.id)->textFile.bytes,p.source.textFile.bytes);
        const auto before=out.toCbor(); choices[0].parameter=999; QVERIFY(!io::buildRegistryCandidate(base,p,choices,&out,&error)); QCOMPARE(out.toCbor(),before);
    }
    void defaultConflictsAndExplicitReplacePreserveMetadata() {
        model::VaultModel base; const auto id=group(base); model::Entry previous; previous.id=QUuid::createUuid(); previous.groupId=id; previous.type=model::EntryType::PlainValue;
        previous.title="Owner caption"; previous.parameterName="PUBLIC_IP"; previous.plainValue="192.0.2.10"; previous.note="Owner note"; previous.url="https://example.test";
        previous.created=QDateTime::currentDateTimeUtc().addDays(-2); previous.favorite=true; previous.reverify=true; previous.tags={"demo"}; QVERIFY(base.setEntry(previous).isEmpty());
        auto p=io::parseTextFile("PUBLIC_IP=192.0.2.10\n","demo.txt"); p.source.groupId=id; auto choices=io::defaultRegistrySelections(base,p); QCOMPARE(choices.first().conflict,io::ConflictAction::Skip);
        p=io::parseTextFile("PUBLIC_IP=192.0.2.20\n","demo.txt"); p.source.groupId=id; choices=io::defaultRegistrySelections(base,p); QCOMPARE(choices.first().conflict,io::ConflictAction::CreateVersion);
        model::VaultModel out; QString error; QVERIFY(io::buildRegistryCandidate(base,p,choices,&out,&error)); QCOMPARE(*out.entry(previous.id),previous); QCOMPARE(out.entries().size(),3);
        choices[0].conflict=io::ConflictAction::Replace; choices[0].replaceId=previous.id; QVERIFY(io::buildRegistryCandidate(base,p,choices,&out,&error)); QCOMPARE(out.entries().size(),2);
        auto expected=previous; expected.plainValue="192.0.2.20"; expected.sourceEntryId=p.source.id; expected.sourceLine=0; expected.modified=out.entry(previous.id)->modified;
        QCOMPARE(*out.entry(previous.id),expected);
        auto duplicate=previous; duplicate.id=QUuid::createUuid(); QVERIFY(base.setEntry(duplicate).isEmpty()); QVERIFY(!io::buildRegistryCandidate(base,p,choices,&out,&error));
    }
    void rejectsCorruptedSelectionAndUnreviewedVisibility_data() {
        QTest::addColumn<int>("action"); for(int i=0;i<10;++i) QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void rejectsCorruptedSelectionAndUnreviewedVisibility() {
        QFETCH(int,action); model::VaultModel base; const auto id=group(base); auto p=io::parseTextFile("PUBLIC_IP=192.0.2.10\n# synthetic hidden\n","demo.txt"); p.source.groupId=id;
        auto choices=io::defaultRegistrySelections(base,p);
        if(action==0) choices[0].parameter=-1;
        if(action==1) choices.append(choices.first());
        if(action==2) choices[0].type=model::EntryType::SshKey;
        if(action==3) choices[0].conflict=io::ConflictAction(99);
        if(action==4) p.parameters[0].value="different";
        if(action==5) p.source.textFile.visibleLines={1};
        if(action==6) p.source.textFile.sourceSnapshot=false;
        if(action==7) p.source.groupId=QUuid::createUuid();
        if(action==8) {choices[0].conflict=io::ConflictAction::Replace; choices[0].replaceId=QUuid::createUuid();}
        if(action==9) for(int i=0;i<8;++i) choices[0].groupPath.append(QString::number(i));
        model::VaultModel out=base; QString error; QVERIFY(!io::buildRegistryCandidate(base,p,choices,&out,&error)); QVERIFY(!error.isEmpty()); QVERIFY(out.entries().isEmpty()); QCOMPARE(out.nodes().size(),1);
    }
    void sourceIdentityCannotOverwriteOrReviveAnotherRecord() {
        model::VaultModel base; const auto id=group(base); model::Entry old; old.id=QUuid::createUuid(); old.groupId=id; old.title="Old"; old.token="synthetic-old"; QVERIFY(base.setEntry(old).isEmpty());
        auto p=io::parseTextFile("IP=192.0.2.1","demo.txt"); p.source.groupId=id; p.source.id=old.id; model::VaultModel out; QString error;
        QVERIFY(!io::buildRegistryCandidate(base,p,{},&out,&error)); QCOMPARE(base.entry(old.id)->token,old.token);
        QVERIFY(base.removeEntry(old.id).isEmpty()); old.sourceEntryId=p.source.id; old.id=QUuid::createUuid(); old.sourceLine=0; QVERIFY(base.setEntry(old).isEmpty());
        QVERIFY(!io::buildRegistryCandidate(base,p,{},&out,&error));
    }
    void candidateLimitsBeforeAllocation() {
        model::VaultModel base; const auto id=group(base); model::Entry e; e.groupId=id; e.token=QString(model::VaultModel::MaxSecretBytes,'x');
        for(int i=0;i<64;++i) {e.id=QUuid::createUuid(); QVERIFY(base.setEntry(e).isEmpty());}
        QVERIFY(base.encodedSize()>model::VaultModel::MaxPlainBytesCBOR);
        auto p=io::parseTextFile("IP=192.0.2.1","demo.txt"); p.source.groupId=id; model::VaultModel out; QString error;
        QVERIFY(!io::buildRegistryCandidate(base,p,io::defaultRegistrySelections(base,p),&out,&error)); QVERIFY(out.entries().isEmpty());
        model::VaultModel full; const auto fullGroup=group(full); e.token.clear(); e.groupId=fullGroup;
        for(int i=0;i<model::VaultModel::MaxEntries;++i) {e.id=QUuid::createUuid(); QVERIFY(full.setEntry(e).isEmpty());}
        p.source.groupId=fullGroup; QVERIFY(!io::buildRegistryCandidate(full,p,{},&out,&error)); QVERIFY(out.entries().isEmpty());
    }
    void emptyFileIsAValidSnapshot() {
        model::VaultModel base; const auto id=group(base); auto p=io::parseTextFile({},"empty.txt"); p.source.groupId=id; QVERIFY(p.error.isEmpty()); QVERIFY(p.parameters.isEmpty());
        model::VaultModel out; QString error; QVERIFY(io::buildRegistryCandidate(base,p,{},&out,&error)); QCOMPARE(out.entries().size(),1); QVERIFY(out.entry(p.source.id)->textFile.bytes.isEmpty());
    }
};
QTEST_GUILESS_MAIN(TestTextImport)
#include "test_text_import.moc"
