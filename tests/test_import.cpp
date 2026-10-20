#include <QtTest>
#include "../src/io/Import.h"
#include "../src/model/VaultModel.h"
using namespace tb;
class TestImport : public QObject {
    Q_OBJECT
private slots:
    void csvWithQuotesSeparatorsAndBom() {
        QByteArray csv;
        csv.append("\xEF\xBB\xBF"); // BOM
        csv.append("Название;Логин;Пароль;Группа;Заметки\r\n");
        csv.append("\"Почта, личная\";roman@ex.ru;pw1;Работа/Почта;важно\r\n");
        csv.append("Токен CI;;ci-token;\"Прод > Сервисы\";\"с ; точкой\"\n");
        csv.append(";нет названия;pw;Группа\n"); // пропуск: пустое название
        csv.append("Без секрета;;;Группа\n"); // пропуск: нет ни пароля, ни логина
        const io::ImportResult result = io::parseCsv(csv);
        QCOMPARE(result.entries.size(), 2);
        QCOMPARE(result.skipped, 2);
        const auto& first = result.entries.at(0);
        QCOMPARE(first.entry.title, QStringLiteral("Почта, личная"));
        QCOMPARE(first.entry.type, model::EntryType::Credentials);
        QCOMPARE(first.entry.password, QStringLiteral("pw1"));
        QCOMPARE(first.groupPath, (QStringList{QStringLiteral("Работа"), QStringLiteral("Почта")}));
        const auto& second = result.entries.at(1);
                QCOMPARE(second.entry.type, model::EntryType::ApiToken); // без логина — токен
        QCOMPARE(second.entry.token, QStringLiteral("ci-token"));
        QVERIFY(second.entry.password.isEmpty());
        QCOMPARE(second.groupPath, (QStringList{QStringLiteral("Прод"), QStringLiteral("Сервисы")}));
        // Добавление в модель: группы создаются лениво, лимиты соблюдаются.
        model::VaultModel m;
        QVERIFY(m.addNode(QUuid::createUuid(), {}, "Импорт").isEmpty());
        const auto importRoot = [m]() { for (auto id : m.nodes().keys()) if (m.nodes().value(id).parent.isNull() && m.nodes().value(id).name == QLatin1String("Импорт")) return id; return model::Id{}; }();
        for (const auto& imported : result.entries) {
            model::Id parent = importRoot;
            for (const QString& part : imported.groupPath) {
                model::Id child;
                for (auto id : m.nodes().keys())
                    if (m.nodes().value(id).parent == parent && m.nodes().value(id).name == part) child = id;
                if (child.isNull() && m.addNode(child = QUuid::createUuid(), parent, part).isEmpty()) {}
                parent = child;
            }
            model::Entry e = imported.entry;
            e.id = QUuid::createUuid(); e.groupId = parent;
            e.created = e.modified = QDateTime::currentDateTimeUtc();
            QVERIFY2(m.setEntry(e).isEmpty(), qPrintable(m.setEntry(e)));
        }
        QCOMPARE(m.entries().size(), 2);
    }
    void csvRejectsHeaderless() {
        io::ImportResult result = io::parseCsv(QByteArray("a,b,c\n1,2,3"));
        QCOMPARE(result.entries.size(), 0);
        QVERIFY(result.skipped > 0);
        QVERIFY(!result.lastSkipReason.isEmpty());
    }
    void keepass2XmlNestedGroupsAndExpiry() {
        const QByteArray xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<KeePassFile><Root><Group><Name>Root</Name>
 <Group><Name>Работа</Name>
  <Group><Name>Сервисы</Name>
   <Entry><String><Key>Title</Key><Value>Панель</Value></String>
          <String><Key>UserName</Key><Value>roman</Value></String>
          <String><Key>Password</Key><Value>super-pw</Value></String>
          <String><Key>URL</Key><Value>https://ex.ru</Value></String></Entry>
   <Entry><String><Key>Title</Key><Value>Срочный токен</Value></String>
          <String><Key>Password</Key><Value>tok</Value></String>
          <Times><Expires>True</Expires><ExpiryTime>2026-11-05T00:00:00Z</ExpiryTime></Times></Entry>
  </Group>
 </Group>
</Group></Root></KeePassFile>)";
        const io::ImportResult result = io::parseKeePassXml(xml);
        QCOMPARE(result.entries.size(), 2);
        QCOMPARE(result.skipped, 0);
        QCOMPARE(result.entries.at(0).groupPath, (QStringList{QStringLiteral("Работа"), QStringLiteral("Сервисы")}));
        QCOMPARE(result.entries.at(0).entry.login, QStringLiteral("roman"));
        QCOMPARE(result.entries.at(0).entry.password, QStringLiteral("super-pw"));
        QCOMPARE(result.entries.at(0).entry.url, QStringLiteral("https://ex.ru"));
        QVERIFY(result.entries.at(1).entry.expiresAt.isValid());
        QCOMPARE(result.entries.at(1).entry.expiresAt.date(), QDate(2026, 11, 5));
        QCOMPARE(result.entries.at(1).entry.type, model::EntryType::Credentials);
    }
    void keepassProtectedValuesAreRejected() {
        const QByteArray xml = "<KeePassFile><Root><Group><Name>Root</Name><Entry>"
            "<String><Key>Title</Key><Value>Защищённая</Value></String>"
            "<String><Key>Password</Key><Value Protected=\"True\">base64==</Value></String>"
            "</Entry></Group></Root></KeePassFile>";
        const io::ImportResult result = io::parseKeePassXml(xml);
        QCOMPARE(result.entries.size(), 0);
        QVERIFY(result.skipped > 0);
        QVERIFY(result.lastSkipReason.contains(QStringLiteral("Protected")));
    }
};
QTEST_MAIN(TestImport)
#include "test_import.moc"
