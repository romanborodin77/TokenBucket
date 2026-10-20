#include <QtTest>
#include <QCborMap>
#include <QCborArray>
#include <QCborValue>
#include "../src/model/VaultModel.h"
using namespace tb::model;
namespace model = tb::model; // тесты тегов из плана используют уточнение model::
using namespace Qt::StringLiterals;
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
            if (it.key() == QStringLiteral("exp")) continue; // Null для срока = бессрочно (легитимно)
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
    void tagsNormalizeAndLimit() {
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.title = "T";
        e.tags = {QStringLiteral("Прод"), QStringLiteral(" прод "), QStringLiteral("СЕРВЕР"), QStringLiteral("прод")};
        QVERIFY(m.setEntry(e).isEmpty());
        QCOMPARE(m.entry(e.id)->tags, (QStringList{QStringLiteral("Прод"), QStringLiteral("СЕРВЕР")}));
        model::Entry many = e; many.id = QUuid::createUuid(); many.tags.clear();
        for (int i = 0; i < 16; ++i) many.tags << QStringLiteral("тег%1").arg(i);
        QVERIFY(m.setEntry(many).isEmpty());
        model::Entry over = many; over.id = QUuid::createUuid();
        over.tags << QStringLiteral("лишний");
        QVERIFY(!m.setEntry(over).isEmpty());                        // 17 тегов
        model::Entry longTag = e; longTag.id = QUuid::createUuid();
        longTag.tags = {QString(33, QChar('д'))};
        QVERIFY(!m.setEntry(longTag).isEmpty());
        model::Entry border = e; border.id = QUuid::createUuid();
        border.tags = {QString(32, QChar('я'))};
        QVERIFY(m.setEntry(border).isEmpty());
    }
    void tagsCborV2RoundtripAndV1Reads() {
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.title = "Т";
        e.tags = {QStringLiteral("прод 🚀"), QStringLiteral("Сервер")};
        QVERIFY(m.setEntry(e).isEmpty());
        model::VaultModel fresh;
        QVERIFY(fresh.fromCbor(m.toCbor(), nullptr) == model::VaultModel::ParseStatus::Ok);
        QCOMPARE(fresh.entry(e.id)->tags, e.tags);
        const auto uid = [](QUuid id) { return id.toString(QUuid::WithoutBraces); };
        // В ручном файле только узел-проект p1, поэтому запись кладём прямо в него.
        QCborMap node; node[u"id"_s] = uid(p1); node[u"p"_s] = QString(); node[u"n"_s] = QStringLiteral("P");
        QCborMap one; one[u"id"_s] = uid(e.id); one[u"g"_s] = uid(p1); one[u"type"_s] = 0; one[u"title"_s] = QStringLiteral("T");
        QCborMap root; QCborArray ns{node}, es{one};
        root[u"v"_s] = 1; root[u"nodes"_s] = ns; root[u"entries"_s] = es;
        model::VaultModel v1; QString err;
        QCOMPARE(v1.fromCbor(QCborValue(root).toCbor(), &err), model::VaultModel::ParseStatus::Ok);
        QVERIFY(v1.entry(e.id)->tags.isEmpty());
        root[u"v"_s] = 2; one[u"tags"_s] = QStringLiteral("не-массив");
        es[0] = one; root[u"entries"_s] = es;   // QCbor-контейнеры хранят значения: правку one возвращаем в root
        model::VaultModel bad; err.clear();
        QCOMPARE(bad.fromCbor(QCborValue(root).toCbor(), &err), model::VaultModel::ParseStatus::Bad);
        QVERIFY(err.contains(QStringLiteral("теги")));
        root[u"v"_s] = 4; // v4 — будущая, не поддержана
        err.clear();
        QCOMPARE(bad.fromCbor(QCborValue(root).toCbor(), &err), model::VaultModel::ParseStatus::Bad);
        QVERIFY(err.contains(QStringLiteral("Неподдерживаемая версия")));
    }
    void expiryCborV3RoundtripAndOlderVersions() {
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.title = "Со сроком";
        e.expiresAt = QDateTime(QDate(2026, 12, 31), QTime(0, 0), Qt::UTC);
        QVERIFY(m.setEntry(e).isEmpty());
        model::VaultModel fresh;
        QVERIFY(fresh.fromCbor(m.toCbor(), nullptr) == model::VaultModel::ParseStatus::Ok);
        QCOMPARE(fresh.entry(e.id)->expiresAt, e.expiresAt);
        model::Entry noExpiry = e; noExpiry.id = QUuid::createUuid(); noExpiry.expiresAt = QDateTime();
        QVERIFY(m.setEntry(noExpiry).isEmpty());
        model::VaultModel second;
        QVERIFY(second.fromCbor(m.toCbor(), nullptr) == model::VaultModel::ParseStatus::Ok);
        QVERIFY(!second.entry(noExpiry.id)->expiresAt.isValid()); // бессрочно переживает круг
        // v2-файл (без exp) читается, срок пуст; v3 с битым exp — отказ.
        const auto uid = [](QUuid id) { return id.toString(QUuid::WithoutBraces); };
        // Берём РЕАЛЬНУЮ запись из m (полная карта v3), понижаем до v2: v→2, exp→удалён.
        const auto full = QCborValue::fromCbor(m.toCbor()).toMap();
        QCborMap one = full.value(u"entries"_s).toArray().first().toMap();
        one.remove(u"exp"_s);
        QCborMap root; root[u"v"_s] = 2;
        root[u"nodes"_s] = full.value(u"nodes"_s).toArray(); // узлы как в реальном снимке
        root[u"entries"_s] = QCborArray{one};
        model::VaultModel v2reader; QString err;
        QVERIFY2(v2reader.fromCbor(QCborValue(root).toCbor(), &err) == model::VaultModel::ParseStatus::Ok, qPrintable(err));
        QVERIFY(!v2reader.entry(e.id)->expiresAt.isValid());
        root[u"v"_s] = 3; one[u"exp"_s] = 12345; root[u"entries"_s] = QCborArray{one};
        err.clear();
        QCOMPARE(v2reader.fromCbor(QCborValue(root).toCbor(), &err), model::VaultModel::ParseStatus::Bad);
        QVERIFY(err.contains(QStringLiteral("срок")));
        one[u"exp"_s] = QStringLiteral("не-дата"); root[u"entries"_s] = QCborArray{one};
        err.clear();
        QCOMPARE(v2reader.fromCbor(QCborValue(root).toCbor(), &err), model::VaultModel::ParseStatus::Bad);
        QVERIFY(err.contains(QStringLiteral("срок")));
        root[u"v"_s] = 4;
        QCOMPARE(v2reader.fromCbor(QCborValue(root).toCbor(), &err), model::VaultModel::ParseStatus::Bad);
        QVERIFY(err.contains(QStringLiteral("Неподдерживаемая версия")));
    }
    void tagsRenameAndRemove() {
        model::Entry first; first.id = QUuid::createUuid(); first.groupId = g1; first.title = "1";
        first.tags = {QStringLiteral("прод"), QStringLiteral("web")};
        model::Entry second = first; second.id = QUuid::createUuid(); second.title = "2"; second.tags = {QStringLiteral("прод")};
        QVERIFY(m.setEntry(first).isEmpty()); QVERIFY(m.setEntry(second).isEmpty());
        QCOMPARE(m.search(QStringLiteral("ПРОД")).size(), 2);          // только теги; имена узлов «ПРОД» не содержат
        const auto partial = m.search(QStringLiteral("eb"));           // часть тега «web»
        QVERIFY(partial.contains(first.id)); QVERIFY(!partial.contains(second.id));
        QVERIFY(m.renameTag(QStringLiteral("ПРОД"), QStringLiteral("production")).isEmpty());
        QCOMPARE(m.entry(first.id)->tags, (QStringList{QStringLiteral("production"), QStringLiteral("web")}));
        QCOMPARE(m.entry(second.id)->tags, (QStringList{QStringLiteral("production")}));
        QVERIFY(m.renameTag(QStringLiteral("web"), QStringLiteral("Production")).isEmpty());   // слияние
        QCOMPARE(m.entry(first.id)->tags, (QStringList{QStringLiteral("production")}));
        QCOMPARE(m.renameTag(QStringLiteral("нет"), QStringLiteral("x")), QStringLiteral("Тег не найден."));
        QCOMPARE(m.renameTag(QStringLiteral("production"), QStringLiteral("   ")), QStringLiteral("Имя тега пустое."));
        QCOMPARE(m.removeTag(QStringLiteral("нет")), QStringLiteral("Тег не найден."));
        QVERIFY(m.removeTag(QStringLiteral("PRODUCTION")).isEmpty()); // латиница: тег «production» — тоже латиница
        QVERIFY(m.entry(first.id)->tags.isEmpty());
        QVERIFY(m.search(QStringLiteral("production")).isEmpty());
    }
};
QTEST_MAIN(TestModel)
#include "test_model.moc"
