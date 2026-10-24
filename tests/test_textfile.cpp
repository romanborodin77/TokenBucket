#include <QtTest>
#include <QCborMap>
#include <QCborArray>
#include "../src/model/VaultModel.h"
#include "../src/model/TextFile.h"
using namespace Qt::StringLiterals;
using namespace tb::model;
class TestTextFile : public QObject {
    Q_OBJECT
private slots:
    void plainValuesAreSearchableUnlessProtected() {
        VaultModel model; const auto group=QUuid::createUuid();
        QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.type=EntryType::PlainValue;
        e.title="Public IP"; e.parameterName="PUBLIC_IP"; e.plainValue="192.0.2.10";
        QVERIFY(model.setEntry(e).isEmpty()); QVERIFY(model.search("192.0.2.10").contains(e.id));
        e.reverify=true; QVERIFY(model.setEntry(e).isEmpty()); QVERIFY(model.search("192.0.2.10").isEmpty());
    }
    void losslessLineRanges_data() {
        QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<int>("count");
        QTest::newRow("empty")<<QByteArray()<<0;
        QTest::newRow("bom-only")<<QByteArray::fromHex("efbbbf")<<0;
        QTest::newRow("crlf-lf-trailing")<<QByteArray("a\r\nb\n")<<3;
        QTest::newRow("cr")<<QByteArray("a\rb")<<2;
        QTest::newRow("empty-lines")<<QByteArray("\n\r\n\r")<<4;
        QTest::newRow("bom-unicode")<<(QByteArray::fromHex("efbbbf")+QStringLiteral("строка\r\n次").toUtf8())<<2;
    }
    void losslessLineRanges() {
        QFETCH(QByteArray,bytes); QFETCH(int,count);
        TextFileData file; file.fileName="demo.txt"; file.bytes=bytes;
        QVERIFY(textfile::validate(file).isEmpty()); const auto ranges=textfile::lineRanges(bytes);
        QCOMPARE(ranges.size(),count);
        QByteArray rebuilt=bytes.startsWith(QByteArray::fromHex("efbbbf"))?bytes.first(3):QByteArray();
        for(int i=0;i<ranges.size();++i) {
            const auto r=ranges[i]; rebuilt+=bytes.mid(r.offset,r.length+r.terminatorLength);
            QCOMPARE(textfile::lineText(file,i),QString::fromUtf8(bytes.constData()+r.offset,r.length));
        }
        QCOMPARE(rebuilt,bytes); QVERIFY(textfile::lineText(file,-1).isEmpty()); QVERIFY(textfile::lineText(file,count).isEmpty());
    }
    void textFileLimitsAndVisibility() {
        TextFileData file; file.fileName="demo.txt"; file.bytes="one\r\ntwo\n"; file.visibleLines={0,2};
        QVERIFY(textfile::validate(file).isEmpty()); QVERIFY(textfile::isVisible(file,0)); QVERIFY(!textfile::isVisible(file,1));
        for(const QList<int>& lines:QList<QList<int>>{{-1},{3},{0,0},{2,0}}) {auto bad=file; bad.visibleLines=lines; QVERIFY(!textfile::validate(bad).isEmpty());}
        for(const QString& name:QStringList{"",".","..","../bad.txt","dir\\bad.txt",QString(256,'x'),QString("bad\nname")}) {
            auto bad=file; bad.fileName=name; QVERIFY(!textfile::validate(bad).isEmpty());
        }
        for(const QByteArray& body:QList<QByteArray>{QByteArray("a\0b",3),QByteArray::fromHex("c0af"),QByteArray(1024*1024+1,'x'),QByteArray(50000,'\n')}) {
            auto bad=file; bad.bytes=body; QVERIFY(!textfile::validate(bad).isEmpty());
        }
        file.bytes=QByteArray(49999,'\n'); file.visibleLines={49999}; QVERIFY(textfile::validate(file).isEmpty());
    }
    void plainFormats_data() {
        QTest::addColumn<int>("format"); QTest::addColumn<QString>("value"); QTest::addColumn<bool>("valid");
        QTest::newRow("text-empty")<<0<<QString()<<true;
        QTest::newRow("ip-empty")<<1<<QString()<<true;
        QTest::newRow("url-empty")<<2<<QString()<<true;
        QTest::newRow("ipv4")<<1<<QString("192.0.2.1")<<true;
        QTest::newRow("ipv6")<<1<<QString("2001:db8::1")<<true;
        QTest::newRow("bad-ip")<<1<<QString("256.0.2.1")<<false;
        QTest::newRow("ip-port")<<1<<QString("192.0.2.1:443")<<false;
        QTest::newRow("url")<<2<<QString("https://example.test/path")<<true;
        QTest::newRow("relative")<<2<<QString("/relative")<<false;
        QTest::newRow("bad-url")<<2<<QString("https://[broken")<<false;
        QTest::newRow("format")<<3<<QString("value")<<false;
        QTest::newRow("cr")<<0<<QString("a\rb")<<false;
        QTest::newRow("lf")<<0<<QString("a\nb")<<false;
        QTest::newRow("nul")<<0<<QString(QChar(0))<<false;
        QTest::newRow("separator")<<0<<QString(QChar(0x2028))<<false;
        QTest::newRow("paragraph")<<0<<QString(QChar(0x2029))<<false;
        QTest::newRow("invalid-utf16")<<0<<QString(QChar(0xd800))<<false;
        QTest::newRow("oversize")<<0<<QString(1024*1024+1,'x')<<false;
    }
    void plainFormats() {
        QFETCH(int,format); QFETCH(QString,value); QFETCH(bool,valid);
        VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.type=EntryType::PlainValue; e.valueFormat=ValueFormat(format); e.plainValue=value;
        QCOMPARE(model.setEntry(e).isEmpty(),valid);
        if(valid) {QCOMPARE(model.entry(e.id)->plainValue,value); e.parameterName=QString(129,'x'); QVERIFY(!model.setEntry(e).isEmpty());}
    }
    void snapshotsLinksAndVisibleSearch() {
        VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        Entry file; file.id=QUuid::createUuid(); file.groupId=group; file.type=EntryType::TextFile;
        file.title="Document"; file.textFile={"demo.txt","public-value\nsynthetic-password",true,{0}};
        QVERIFY(model.setEntry(file).isEmpty()); QVERIFY(model.search("public-value").contains(file.id));
        QVERIFY(model.search("synthetic-password").isEmpty()); file.reverify=true; QVERIFY(model.setEntry(file).isEmpty());
        QVERIFY(model.search("public-value").isEmpty()); file.reverify=false;
        Entry value; value.id=QUuid::createUuid(); value.groupId=group; value.type=EntryType::PlainValue;
        value.sourceEntryId=file.id; value.sourceLine=0; value.plainValue="public-value"; QVERIFY(model.setEntry(value).isEmpty());
        auto invalid=value; invalid.sourceLine=2; QVERIFY(!model.setEntry(invalid).isEmpty());
        invalid=value; invalid.sourceEntryId=value.id; QVERIFY(!model.setEntry(invalid).isEmpty());
        invalid=value; invalid.sourceEntryId=group; QVERIFY(!model.setEntry(invalid).isEmpty());
        invalid=value; invalid.sourceEntryId={}; QVERIFY(!model.setEntry(invalid).isEmpty());
        invalid=file; invalid.textFile.bytes="changed"; QVERIFY(!model.setEntry(invalid).isEmpty());
        invalid=file; invalid.textFile.sourceSnapshot=false; QVERIFY(!model.setEntry(invalid).isEmpty());
        file.title="New metadata"; QVERIFY(model.setEntry(file).isEmpty());
        QVERIFY(model.removeEntry(file.id).isEmpty()); QVERIFY(model.setEntry(value).isEmpty());
        QVERIFY(!model.addNode(file.id,group,"Not a source document").isEmpty());
        invalid={}; invalid.id=file.id; invalid.groupId=group; QVERIFY(!model.setEntry(invalid).isEmpty());
        QVERIFY(model.setEntry(file).isEmpty());
        Entry editable=file; editable.id=QUuid::createUuid(); editable.textFile.sourceSnapshot=false; QVERIFY(model.setEntry(editable).isEmpty());
        editable.textFile.bytes="new secret\nold public"; QVERIFY(!model.setEntry(editable).isEmpty());
        editable.textFile.visibleLines.clear(); QVERIFY(model.setEntry(editable).isEmpty());
        QVERIFY(model.search("new secret").isEmpty());
    }
    void v4GoldenRoundtripAndLegacy() {
        VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        const auto source=QUuid::createUuid();
        for(int type=0;type<5;++type) {
            Entry e; e.id=type==4?source:QUuid::createUuid(); e.groupId=group; e.type=EntryType(type); e.title=QString::number(type);
            if(type==0) {e.token="synthetic-token"; e.password="legacy-unused"; e.sshPrivate="legacy-bytes";}
            if(type==1) {e.login="user"; e.password="synthetic-password";}
            if(type==2) {e.sshPrivate="synthetic-private\r\n"; e.sshPublic="synthetic-public";}
            if(type==3) {e.parameterName="Адрес"; e.plainValue="192.0.2.10"; e.valueFormat=ValueFormat::Ip; e.sourceEntryId=source; e.sourceLine=0;}
            if(type==4) e.textFile={"demo.txt",QByteArray::fromHex("efbbbf")+"IP=192.0.2.10\r\nTOKEN=synthetic\r\n",true,{0}};
            QVERIFY(model.setEntry(e).isEmpty());
        }
        const auto bytes=model.toCbor(); auto secure=model.toCborSecure();
        QCOMPARE(QByteArray::fromRawData(reinterpret_cast<const char*>(secure.data()),qsizetype(secure.size())),bytes);
        VaultModel decoded; QString error; QCOMPARE(decoded.fromCbor(bytes,&error),VaultModel::ParseStatus::Ok); QCOMPARE(decoded.decodedVersion(),4);
        for(const auto& e:model.entries()) {QVERIFY(decoded.entry(e.id)); QCOMPARE(*decoded.entry(e.id),e);}
        auto badLinks=QCborValue::fromCbor(bytes).toMap(); auto linkedEntries=badLinks.value(u"entries"_s).toArray();
        for(int i=0;i<linkedEntries.size();++i) {
            auto item=linkedEntries[i].toMap();
            if(item.value(u"type"_s).toInteger()==4) {item[u"fileSnapshot"_s]=false; linkedEntries[i]=item;}
        }
        badLinks[u"entries"_s]=linkedEntries;
        QCOMPARE(decoded.fromCbor(QCborValue(badLinks).toCbor(),&error),VaultModel::ParseStatus::Bad);
        QCOMPARE(decoded.toCbor(),bytes);
        auto root=QCborValue::fromCbor(bytes).toMap(); QCborArray oldEntries;
        for(auto item:root.value(u"entries"_s).toArray()) if(item.toMap().value(u"type"_s).toInteger()==0) oldEntries.append(item);
        root[u"entries"_s]=oldEntries;
        for(int version=1;version<=3;++version) {
            root[u"v"_s]=version; QCOMPARE(decoded.fromCbor(QCborValue(root).toCbor(),&error),VaultModel::ParseStatus::Ok);
            QCOMPARE(decoded.decodedVersion(),version); const auto e=*decoded.entries().cbegin();
            QCOMPARE(e.password,QString("legacy-unused")); QCOMPARE(e.sshPrivate,QByteArray("legacy-bytes"));
            auto entry=oldEntries.first().toMap(); entry[u"type"_s]=3; root[u"entries"_s]=QCborArray{entry};
            QCOMPARE(decoded.fromCbor(QCborValue(root).toCbor(),&error),VaultModel::ParseStatus::Bad);
            entry=oldEntries.first().toMap(); entry[u"value"_s]=QByteArray("hidden-extra"); root[u"entries"_s]=QCborArray{entry};
            QCOMPARE(decoded.fromCbor(QCborValue(root).toCbor(),&error),VaultModel::ParseStatus::Bad); root[u"entries"_s]=oldEntries;
        }
    }
    void corruptV4IsTransactional() {
        VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        Entry file; file.id=QUuid::createUuid(); file.groupId=group; file.type=EntryType::TextFile;
        file.textFile={"demo.txt","one\ntwo",true,{0}}; QVERIFY(model.setEntry(file).isEmpty());
        const auto before=model.toCbor(); const auto root=QCborValue::fromCbor(before).toMap();
        const QList<QPair<QString,QCborValue>> faults={{"key",7},{"value",QString("bad")},{"format",QString("bad-format")},{"fileName",7},
            {"fileBytes",QString("bad")},{"fileSnapshot",1},{"fileVisible",QCborArray{0,0}},{"fileVisible",QCborArray{-1}},
            {"fileVisible",QCborArray{2}},{"fileVisible",QCborArray{1,0}},{"sourceId",1},{"sourceLine",qint64(50000)},
            {"sourceId",QString("bad-uuid")},{"type",5},{"fileBytes",QByteArray::fromHex("c0af")},{"value",QByteArray("inactive-value")}};
        for(const auto& fault:faults) {
            auto broken=root; auto entry=root.value(u"entries"_s).toArray().first().toMap(); entry[fault.first]=fault.second;
            broken[u"entries"_s]=QCborArray{entry}; QString error;
            QCOMPARE(model.fromCbor(QCborValue(broken).toCbor(),&error),VaultModel::ParseStatus::Bad);
            QVERIFY(!error.isEmpty()); QCOMPARE(model.toCbor(),before); QCOMPARE(model.decodedVersion(),4);
        }
    }
    void initialBomInValuesSurvivesRoundtrip() {
        VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        const auto text=QString(QChar(0xfeff))+QStringLiteral("opaque-value");
        for(int type:{0,1,3}) {
            Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.type=EntryType(type);
            if(type==0) e.token=text; else if(type==1) e.password=text; else e.plainValue=text;
            QVERIFY(model.setEntry(e).isEmpty());
        }
        VaultModel copy; QCOMPARE(copy.fromCbor(model.toCbor(),nullptr),VaultModel::ParseStatus::Ok);
        for(const auto& e:model.entries()) {QVERIFY(copy.entry(e.id)); QCOMPARE(*copy.entry(e.id),e);}
    }
};
QTEST_MAIN(TestTextFile)
#include "test_textfile.moc"
