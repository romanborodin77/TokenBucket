#include <QtTest>
#include <QCborMap>
#include <QCborArray>
#include "../src/model/VaultModel.h"
using namespace tb::model;
class TestModel : public QObject {
    Q_OBJECT
    VaultModel m;
    Id p1{QUuid::createUuid()}, g1{QUuid::createUuid()};
private slots:
    void init() { m = VaultModel{}; QVERIFY(m.addNode(p1, Id(), "Проект").isEmpty()); QVERIFY(m.addNode(g1, p1, "Группа").isEmpty()); }
    void nodeCrudAndDepth() {
        Id a{QUuid::createUuid()};
        QVERIFY(m.addNode(a, g1, "Вложенная").isEmpty());
        QCOMPARE(m.depth(a), 3);
        QVERIFY(m.renameNode(a, "Новое имя").isEmpty());
        QVERIFY(!m.removeNode(g1).isEmpty());
        QVERIFY(m.removeNode(a).isEmpty());
        QVERIFY(m.removeNode(g1).isEmpty());
    }
    void moveRejectsProjectCycleDepth() {
        QVERIFY(!m.moveNode(p1, g1).isEmpty());     // проект нельзя переместить
        Id a{QUuid::createUuid()}, b{QUuid::createUuid()};
        QVERIFY(m.addNode(a, g1, "A").isEmpty());
        QVERIFY(!m.moveNode(g1, a).isEmpty());      // цикл: g1 предок a
        QVERIFY(m.addNode(b, a, "B").isEmpty());
        QVERIFY(m.moveNode(b, p1).isEmpty());       // перенос вверх допустим
    }
    void depthLimitEight() {
        Id cur = p1;
        for (int i = 2; i <= 8; ++i) { Id g{QUuid::createUuid()}; QVERIFY2(m.addNode(g, cur, "уровень").isEmpty(), qPrintable(QString("уровень %1").arg(i))); cur = g; }
        Id tooDeep{QUuid::createUuid()};
        QVERIFY(!m.addNode(tooDeep, cur, "девятый").isEmpty());
    }
    void entryLimits() {
        Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.type = EntryType::ApiToken; e.title = "Токен"; e.token = "x";
        for (int i = 0; i < 3; ++i) { Entry t = e; t.id = QUuid::createUuid(); QVERIFY(m.setEntry(t).isEmpty()); }
        QByteArray big(int(VaultModel::MaxSecretBytes) + 1, 'z');
        Entry t = e; t.id = QUuid::createUuid(); t.token = QString::fromUtf8(big);
        QVERIFY(!m.setEntry(t).isEmpty());      // секрет > 1 МиБ
        QVERIFY(m.setEntry(e).isEmpty());       // add/replace
        QVERIFY(m.removeEntry(e.id).isEmpty());
    }
    void maxEntriesTenThousand() {
        Entry e; e.groupId = p1; e.type = EntryType::ApiToken; e.title = "t"; e.token = "x";
        for (int i = 0; i < VaultModel::MaxEntries; ++i) { Entry t = e; t.id = QUuid::createUuid(); QVERIFY2(m.setEntry(t).isEmpty(), qPrintable(QString::number(i))); }
        Entry over = e; over.id = QUuid::createUuid();
        QVERIFY(!m.setEntry(over).isEmpty());
    }
    void searchSkipsSecretsAndCoversProjectName() {
        Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.type = EntryType::Credentials;
        e.title = "Логин-запись"; e.login = "user@example.com"; e.password = "XyZzY_секрет";
        e.note = "заметка про прод";
        QVERIFY(m.setEntry(e).isEmpty());
        QVERIFY(m.search("логин").contains(e.id));
        QVERIFY(m.search("user@example").contains(e.id));
        QVERIFY(m.search("заметка").contains(e.id));
        QVERIFY(m.search("проект").contains(e.id));  // имя проекта находит записи его поддерева
        QVERIFY(!m.search("XyZzY").contains(e.id));  // секрет не ищется
    }
    void cborRoundtripAndUnicode() {
        Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.type = EntryType::SshKey;
        e.title = "ключ 🚀 عربى"; e.login = "日本語ログイン";
        e.sshPrivate = QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\nabc\r\n-----END-----\n");
        e.sshPublic = QByteArray("ssh-ed25519 AAAA test");
        e.sshPassphrase = "фраза"; e.sshComment = "комментарий"; e.reverify = true; e.favorite = true;
        e.created = QDateTime::currentDateTimeUtc(); e.modified = e.created.addSecs(5);
        QVERIFY(m.setEntry(e).isEmpty());
        QByteArray cbor = m.toCbor();
        VaultModel m2;
        QVERIFY(m2.fromCbor(cbor, nullptr) == VaultModel::ParseStatus::Ok);
        QCOMPARE(m2.entry(e.id)->title, e.title);
        QCOMPARE(m2.entry(e.id)->sshPrivate, e.sshPrivate);
        QCOMPARE(m2.entry(e.id)->sshPublic, e.sshPublic);
        QCOMPARE(m2.entry(e.id)->sshPassphrase, e.sshPassphrase);
        QCOMPARE(m2.entry(e.id)->sshComment, e.sshComment);
        QCOMPARE(m2.entry(e.id)->login, e.login);
        QCOMPARE(m2.entry(e.id)->created, e.created);
        QCOMPARE(m2.entry(e.id)->modified, e.modified);
        QCOMPARE(m2.entry(e.id)->favorite, true);
        QCOMPARE(m2.entry(e.id)->reverify, true);
        QVERIFY(m2.search("🚀").contains(e.id));
    }
    void cborRejectsGarbageAndOversize() {
        VaultModel m2; QString err;
        QVERIFY(m2.fromCbor(QByteArray("мусор"), &err) == VaultModel::ParseStatus::Bad);
        QVERIFY(!err.isEmpty());
        QByteArray big(int(VaultModel::MaxPlainBytesCBOR) + 1, 'q');
        QVERIFY(m2.fromCbor(big, &err) == VaultModel::ParseStatus::TooLarge);
    }
    void clearRemovesEverything() {
        m.clear();
        QVERIFY(m.nodes().isEmpty());
        QVERIFY(m.search("").isEmpty());
    }
    void rejectsInvalidIdentitiesAndTypes() {
        QVERIFY(!m.addNode({}, {}, "Нулевой").isEmpty());
        Entry e; e.id = p1; e.groupId = g1;
        QVERIFY(!m.setEntry(e).isEmpty());
        e.id = {}; QVERIFY(!m.setEntry(e).isEmpty());
        e.id = QUuid::createUuid(); e.type = EntryType(99);
        QVERIFY(!m.setEntry(e).isEmpty());
    }
    void corruptImportIsTransactional() {
        Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.title = "Сохранить";
        QVERIFY(m.setEntry(e).isEmpty());
        const auto valid = QCborValue::fromCbor(m.toCbor()).toMap();
        QList<QByteArray> bad{QByteArray::fromHex("a0"), m.toCbor() + 'x'};
        for (const QString key : {QStringLiteral("nodes"), QStringLiteral("entries")}) {
            auto root = valid; root[key] = false; bad << QCborValue(root).toCbor();
        }
        auto root = valid; auto entries = root.value(QStringLiteral("entries")).toArray();
        entries << entries.first(); root[QStringLiteral("entries")] = entries;
        bad << QCborValue(root).toCbor();
        root = valid; entries = root.value(QStringLiteral("entries")).toArray();
        auto entry = entries.first().toMap(); entry[QStringLiteral("type")] = QStringLiteral("0");
        entries[0] = entry; root[QStringLiteral("entries")] = entries;
        bad << QCborValue(root).toCbor();
        for (const auto& bytes : bad) {
            QString error;
            QVERIFY(m.fromCbor(bytes, &error) != VaultModel::ParseStatus::Ok);
            QVERIFY(!error.isEmpty());
            QVERIFY(m.entry(e.id));
            QCOMPARE(m.entry(e.id)->title, e.title);
            QCOMPARE(m.nodes().size(), 2);
        }
    }
    void importChildBeforeParentAndRejectCycles() {
        auto root = QCborValue::fromCbor(m.toCbor()).toMap();
        QCborArray nodes;
        for (Id id : {g1, p1}) {
            QCborMap n; n[QStringLiteral("id")] = id.toString(QUuid::WithoutBraces);
            n[QStringLiteral("p")] = id == g1 ? p1.toString(QUuid::WithoutBraces) : QString();
            n[QStringLiteral("n")] = QStringLiteral("узел"); nodes << n;
        }
        root[QStringLiteral("nodes")] = nodes;
        VaultModel loaded;
        QCOMPARE(loaded.fromCbor(QCborValue(root).toCbor(), nullptr), VaultModel::ParseStatus::Ok);
        QCOMPARE(loaded.depth(g1), 2);
        auto project = nodes[1].toMap(); project[QStringLiteral("p")] = g1.toString(QUuid::WithoutBraces);
        nodes[1] = project; root[QStringLiteral("nodes")] = nodes;
        QCOMPARE(loaded.fromCbor(QCborValue(root).toCbor(), nullptr), VaultModel::ParseStatus::Bad);
        QCOMPARE(loaded.depth(g1), 2);
    }
    void invalidCborFieldTypes() {
        Entry e; e.id = QUuid::createUuid(); e.groupId = g1;
        QVERIFY(m.setEntry(e).isEmpty());
        const auto valid = QCborValue::fromCbor(m.toCbor()).toMap();
        const auto original = valid.value(QStringLiteral("entries")).toArray().first().toMap();
        for (auto it = original.constBegin(); it != original.constEnd(); ++it) {
            auto brokenEntry = original; brokenEntry[it.key()] = QCborValue(QCborValue::Null);
            auto broken = valid; broken[QStringLiteral("entries")] = QCborArray{brokenEntry};
            QVERIFY(m.fromCbor(QCborValue(broken).toCbor(), nullptr) == VaultModel::ParseStatus::Bad);
            QVERIFY(m.entry(e.id));
        }
    }
    void movingSubtreeRespectsDepth() {
        Id root = g1, tail = g1;
        for (int i = 3; i <= 8; ++i) {
            Id next = QUuid::createUuid();
            QVERIFY(m.addNode(next, tail, "узел").isEmpty()); tail = next;
        }
        Id project = QUuid::createUuid(), group = QUuid::createUuid();
        QVERIFY(m.addNode(project, {}, "проект 2").isEmpty());
        QVERIFY(m.addNode(group, project, "группа 2").isEmpty());
        QVERIFY(!m.moveNode(root, group).isEmpty());
        QCOMPARE(m.depth(tail), 8);
        QVERIFY(m.moveNode(root, project).isEmpty());
        QCOMPARE(m.depth(tail), 8);
    }
};
QTEST_MAIN(TestModel)
#include "test_model.moc"
