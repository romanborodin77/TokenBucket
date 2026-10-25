#include <QtTest>
#include <QCborMap>
#include <QCborArray>
#include <QCborValue>
#include <functional>
#include <limits>
#include "../src/model/VaultModel.h"
#include "../src/vault/CborWriter.h"
using namespace tb::model;
namespace model = tb::model; // тесты тегов из плана используют уточнение model::
namespace cbor = tb::cbor;
using namespace Qt::StringLiterals;
class TestModel : public QObject {
    Q_OBJECT
    VaultModel m;
    Id p1{QUuid::createUuid()}, g1{QUuid::createUuid()};
    bool encodeBroken_ = false; // копится в encodeUnit, проверяется одним QVERIFY2 в конце юнит-слота
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
        root[u"v"_s] = 6; // будущая версия не поддержана
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
        // Берём снимок из ЧИСТОЙ модели первого круга: общая m могла получить записи
        // с невалидными датами от предыдущих слотов (invalidCborFieldTypes Null-ит поля).
        const auto full = QCborValue::fromCbor(fresh.toCbor()).toMap();
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
        root[u"v"_s] = 6;
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
    // ===== Защищённый CBOR-писатель (toCborSecure) =====
    model::Entry richEntry() {
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.type = EntryType::SshKey;
        e.title = QStringLiteral("заголовок 🚀 عربى"); e.note = QStringLiteral("заметка");
        e.url = QStringLiteral("https://пример.рф/путь");
        e.token = QStringLiteral("токен-секрет-ü"); e.login = QStringLiteral("логин-日本");
        e.password = QStringLiteral("пароль-секрет");
        e.sshPrivate = QByteArrayLiteral("\x00\x01\x02\xff--приватный-ключ--");
        e.sshPublic = QByteArrayLiteral("ssh-ed25519 AAAA test");
        e.sshPassphrase = QStringLiteral("фраза"); e.sshComment = QStringLiteral("комментарий");
        e.favorite = true; e.reverify = false;
        e.created = QDateTime::currentDateTimeUtc(); e.modified = e.created.addSecs(7);
        e.expiresAt = QDateTime(QDate(2027, 1, 2), QTime(3, 4, 5, 678), Qt::UTC);
        e.tags = {QStringLiteral("прод 🚀"), QStringLiteral("Сервер")};
        return e;
    }
    void secureCborGoldenByteMatch() {
        // Золотой тест: защищённый энкодер обязан выдавать побайтово те же данные,
        // что и эталонный QCborValue::toCbor() — иначе файлы двух писателей
        // несовместимы. Покрыты все виды значений модели, включая пустые секреты
        // и бессрочные записи.
        model::Entry e = richEntry();
        QVERIFY(m.setEntry(e).isEmpty());
        model::Entry minimal; minimal.id = QUuid::createUuid(); minimal.groupId = p1;
        minimal.type = EntryType::ApiToken; minimal.title = QStringLiteral("минимум"); minimal.token = QStringLiteral("x");
        QVERIFY(m.setEntry(minimal).isEmpty());
        const QByteArray reference = m.toCbor();
        cbor::SecureBuffer produced = m.toCborSecure();
        QCOMPARE(QByteArray::fromRawData(reinterpret_cast<const char*>(produced.data()), qsizetype(produced.size())), reference);
        // Пустая модель тоже должна совпадать (пустые массивы, карта из трёх ключей).
        model::VaultModel empty;
        cbor::SecureBuffer emptyProduced = empty.toCborSecure();
        QCOMPARE(QByteArray::fromRawData(reinterpret_cast<const char*>(emptyProduced.data()), qsizetype(emptyProduced.size())), empty.toCbor());
    }
    void savedViewsRoundtripV5() {
        const auto g = QUuid::createUuid(); QVERIFY(m.addNode(g, {}, QStringLiteral("Demo")).isEmpty());
        model::SavedView v; v.name = QStringLiteral("Истекают в CI"); v.group = g; v.type = 3; v.expiry = 1; v.sort = 1;
        v.tags = {QStringLiteral("ci")}; v.search = QStringLiteral("ip");
        model::SavedView all; all.name = QStringLiteral("Все"); // type=-1 — знаковое кодирование
        QVERIFY(m.setViews({v, all}).isEmpty());
        cbor::SecureBuffer produced = m.toCborSecure();
        QCOMPARE(QByteArray::fromRawData(reinterpret_cast<const char*>(produced.data()), qsizetype(produced.size())), m.toCbor());
        model::VaultModel r; QString err; QCOMPARE(r.fromCbor(m.toCbor(), &err), model::VaultModel::ParseStatus::Ok);
        QCOMPARE(r.views(), (QList<model::SavedView>{v, all})); QCOMPARE(r.decodedVersion(), 5);
        QVERIFY(!m.setViews({v, v}).isEmpty());
        model::SavedView twin = v; twin.name = QStringLiteral("истекают в ci"); QVERIFY(!m.setViews({v, twin}).isEmpty());
        model::SavedView bad = v; bad.name.clear(); QVERIFY(!m.setViews({bad}).isEmpty());
        bad = v; bad.type = 7; QVERIFY(!m.setViews({bad}).isEmpty());
        bad = v; bad.search = QString(257, QLatin1Char('x')); QVERIFY(!m.setViews({bad}).isEmpty());
        bad = v; bad.name = QStringLiteral("a") + QChar(0x0A) + QStringLiteral("b"); QVERIFY(!m.setViews({bad}).isEmpty());
        QList<model::SavedView> many; for (int i = 0; i < 51; ++i) { auto x = all; x.name = QString::number(i); many << x; }
        QVERIFY(!m.setViews(many).isEmpty());
        QCOMPARE(m.views(), (QList<model::SavedView>{v, all}));
    }
    void savedViewsVersionGates() {
        const auto g = QUuid::createUuid(); QVERIFY(m.addNode(g, {}, QStringLiteral("Demo")).isEmpty());
        model::SavedView v; v.name = QStringLiteral("Вид"); QVERIFY(m.setViews({v}).isEmpty());
        const auto good = QCborValue::fromCbor(m.toCbor()).toMap();
        const auto parse = [](const QCborMap& root, model::VaultModel& target) { QString err; return target.fromCbor(QCborValue(root).toCbor(), &err); };
        model::VaultModel target; QCOMPARE(parse(good, target), model::VaultModel::ParseStatus::Ok);
        auto v4 = good; v4[QStringLiteral("v")] = 4; v4.remove(QStringLiteral("views"));
        model::VaultModel old; QCOMPARE(parse(v4, old), model::VaultModel::ParseStatus::Ok); QVERIFY(old.views().isEmpty()); QCOMPARE(old.decodedVersion(), 4);
        auto v4views = good; v4views[QStringLiteral("v")] = 4; QCOMPARE(parse(v4views, target), model::VaultModel::ParseStatus::Bad);
        auto noViews = good; noViews.remove(QStringLiteral("views")); QCOMPARE(parse(noViews, target), model::VaultModel::ParseStatus::Bad);
        auto notMap = good; notMap[QStringLiteral("views")] = QCborArray{1}; QCOMPARE(parse(notMap, target), model::VaultModel::ParseStatus::Bad);
        auto extra = good; { auto a = extra[QStringLiteral("views")].toArray(); auto x = a.at(0).toMap(); x[QStringLiteral("zz")] = 1; a[0] = x; extra[QStringLiteral("views")] = a; }
        QCOMPARE(parse(extra, target), model::VaultModel::ParseStatus::Bad);
        auto wrongType = good; { auto a = wrongType[QStringLiteral("views")].toArray(); auto x = a.at(0).toMap(); x[QStringLiteral("lib")] = QStringLiteral("0"); a[0] = x; wrongType[QStringLiteral("views")] = a; }
        QCOMPARE(parse(wrongType, target), model::VaultModel::ParseStatus::Bad);
        auto dup = good; { auto a = dup[QStringLiteral("views")].toArray(); a.append(a.at(0)); dup[QStringLiteral("views")] = a; }
        QCOMPARE(parse(dup, target), model::VaultModel::ParseStatus::Bad);
        auto v6 = good; v6[QStringLiteral("v")] = 6; QCOMPARE(parse(v6, target), model::VaultModel::ParseStatus::Bad);
        QCOMPARE(target.views().size(), 1); // отказы не меняют прежнюю модель
    }
    void secureCborRoundtrip() {
        model::Entry e = richEntry();
        QVERIFY(m.setEntry(e).isEmpty());
        cbor::SecureBuffer produced = m.toCborSecure();
        // Чтение идёт через QCborValue::fromCbor в обычной куче — задокументированный
        // остаточный риск (docs/security); здесь важно лишь, что защищённый писатель
        // производит корректный читаемый CBOR.
        const QByteArray bytes(reinterpret_cast<const char*>(produced.data()), qsizetype(produced.size()));
        model::VaultModel fresh; QString err;
        QCOMPARE(fresh.fromCbor(bytes, &err), model::VaultModel::ParseStatus::Ok);
        const model::Entry* loaded = fresh.entry(e.id);
        QVERIFY(loaded);
        QCOMPARE(loaded->title, e.title);
        QCOMPARE(loaded->token, e.token);
        QCOMPARE(loaded->password, e.password);
        QCOMPARE(loaded->sshPrivate, e.sshPrivate);
        QCOMPARE(loaded->sshPublic, e.sshPublic);
        QCOMPARE(loaded->expiresAt, e.expiresAt);
        QCOMPARE(loaded->tags, e.tags);
        QCOMPARE(loaded->favorite, true);
        QCOMPARE(loaded->reverify, false);
    }
    void cborWriterEncodingUnits() {
        // Юниты энкодера против эталона Qt: границы длин CBOR (0/23/24/255/256/…
        // /2^32+), bool, строки (ascii, кириллица, эмодзи-суррогаты, пустая),
        // байтовые строки, definite-массивы и карты. Расхождение с Qt здесь
        // означает неверную ручную кодировку ещё до уровня модели.
        // qint64-границы: quint64 > INT64_MAX моделью не пишется, эталона QCborValue для major 0 там нет.
        for (quint64 v : {quint64(0), quint64(1), quint64(23), quint64(24), quint64(25), quint64(255),
                          quint64(256), quint64(65535), quint64(65536), quint64(4294967295u),
                          quint64(4294967296u), quint64(9223372036854775807u)})
            QCOMPARE(encodeUnit([&](cbor::CborWriter& w) { w.uint(v); }), QCborValue(qint64(v)).toCbor());
        QCOMPARE(encodeUnit([](cbor::CborWriter& w) { w.boolean(false); }), QCborValue(false).toCbor());
        for(qint64 value:{qint64(-1),qint64(-24),qint64(-25),std::numeric_limits<qint64>::min(),qint64(0),qint64(50000)})
            QCOMPARE(encodeUnit([&](cbor::CborWriter& w) {w.integer(value);}),QCborValue(value).toCbor());
        QCOMPARE(encodeUnit([](cbor::CborWriter& w) { w.boolean(true); }), QCborValue(true).toCbor());
        for (const QString s : {QStringLiteral(""), QStringLiteral("ascii"),
                                QStringLiteral("кириллица-ü"), QStringLiteral("🚀🎉 пара суррогатов")})
            QCOMPARE(encodeUnit([&](cbor::CborWriter& w) { w.text(s); }), QCborValue(s).toCbor());
        // Битый одинокий суррогат: наш писатель заменяет его на U+FFFD (3 байта),
        // как QString::toUtf8(); Qt-эталон здесь непригоден — QCborValue на такой
        // строке молча выдаёт пустую. В модели битых суррогатов не бывает.
        QCOMPARE(encodeUnit([](cbor::CborWriter& w) { w.text(QString(1, QChar(0xD800))); }),
                 QByteArray::fromHex("63efbfbd"));
        for (const QByteArray b : {QByteArray(), QByteArrayLiteral("\x00\x01\xff"), QByteArray(300, 'z')})
            QCOMPARE(encodeUnit([&](cbor::CborWriter& w) { w.bytes(b.constData(), size_t(b.size())); }),
                     QCborValue(b).toCbor());
        // bytesUtf8: QString как байтовая строка UTF-8 — именно так пишутся токены/пароли.
        const QString secret = QStringLiteral("секрет-🚀");
        QCOMPARE(encodeUnit([&](cbor::CborWriter& w) { w.bytesUtf8(secret); }), QCborValue(secret.toUtf8()).toCbor());
        QCOMPARE(encodeUnit([](cbor::CborWriter& w) { w.beginArray(0); }), QCborValue(QCborArray()).toCbor());
        QCOMPARE(encodeUnit([](cbor::CborWriter& w) { w.beginMap(0); }), QCborValue(QCborMap()).toCbor());
        const QCborArray twoInts = QCborArray{1, 2};
        QCOMPARE(encodeUnit([](cbor::CborWriter& w) { w.beginArray(2); w.uint(1); w.uint(2); }),
                 QCborValue(twoInts).toCbor());
        const QCborMap pairOfTextKeys = QCborMap{{QStringLiteral("b"), 1}, {QStringLiteral("a"), 2}};
        // QCborMap сохраняет порядок вставки (не сортирует): эталон собираем
        // в том же порядке, в котором пишет энкодер — b, затем a.
        QCOMPARE(encodeUnit([](cbor::CborWriter& w) {
                      w.beginMap(2); w.text(u"b"); w.uint(1); w.text(u"a"); w.uint(2);
                  }),
                 QCborValue(pairOfTextKeys).toCbor());
        // Оба прохода каждого кейса обязаны сойтись по размеру и уложиться в буфер.
        QVERIFY2(!encodeBroken_, "расхождение проходов подсчёта и записи");
    }
    // Прогоняет body через оба прохода писателя и возвращает записанные байты.
    // QVERIFY внутри нельзя (метод не void): факт расхождения проходов копится
    // в encodeBroken_ и проверяется одним QVERIFY2 в конце слота.
    QByteArray encodeUnit(const std::function<void(cbor::CborWriter&)>& body) {
        cbor::CborWriter counter;
        body(counter);
        QByteArray out(qsizetype(counter.position()), '\0'); // содержимое целиком перезапишет писатель
        cbor::CborWriter writer;
        writer.setOutput(reinterpret_cast<unsigned char*>(out.data()), size_t(out.size()));
        body(writer);
        encodeBroken_ = encodeBroken_ || writer.overflow() || writer.position() != counter.position();
        return out;
    }
};
QTEST_MAIN(TestModel)
#include "test_model.moc"
