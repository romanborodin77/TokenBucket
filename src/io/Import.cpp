#include "Import.h"
#include "../model/VaultModel.h"
#include <QDateTime>
#include <QRegularExpression>
#include <QHash>
#include <QXmlStreamReader>
namespace tb::io {
namespace {
// Пропуск записи с причиной: копия пароля не нужна модели, счётчик важнее падения.
void skip(ImportResult& result, const QString& reason) {
    ++result.skipped; result.lastSkipReason = reason;
}
bool secretTooBig(const model::Entry& e) {
    return e.token.toUtf8().size() > model::VaultModel::MaxSecretBytes
        || e.password.toUtf8().size() > model::VaultModel::MaxSecretBytes;
}
QString normalized(const QString& raw) {
    static const QHash<QString, QString> map = {
        {QStringLiteral("title"), QStringLiteral("title")}, {QStringLiteral("название"), QStringLiteral("title")}, {QStringLiteral("запись"), QStringLiteral("title")},
        {QStringLiteral("username"), QStringLiteral("login")}, {QStringLiteral("login"), QStringLiteral("login")}, {QStringLiteral("логин"), QStringLiteral("login")},
        {QStringLiteral("password"), QStringLiteral("password")}, {QStringLiteral("пароль"), QStringLiteral("password")},
        {QStringLiteral("url"), QStringLiteral("url")}, {QStringLiteral("адрес"), QStringLiteral("url")},
        {QStringLiteral("notes"), QStringLiteral("note")}, {QStringLiteral("заметки"), QStringLiteral("note")}, {QStringLiteral("заметка"), QStringLiteral("note")},
        {QStringLiteral("group"), QStringLiteral("group")}, {QStringLiteral("группа"), QStringLiteral("group")}, {QStringLiteral("папка"), QStringLiteral("group")},
    };
    return map.value(raw.trimmed().toLower());
}
// Разбор CSV-строки с кавычками; sep определяется по заголовочной строке.
QStringList splitCsvLine(const QString& line, QChar sep) {
    QStringList fields;
    QString field;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (quoted) {
            if (ch == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) { field += ch; ++i; } // экранированная кавычка
                else quoted = false;
            } else field += ch;
        } else if (ch == QLatin1Char('"')) quoted = true;
        else if (ch == sep) { fields << field; field.clear(); }
        else field += ch;
    }
    fields << field;
    return fields;
}
}

ImportResult parseCsv(const QByteArray& bytes) {
    ImportResult result;
    QString text = QString::fromUtf8(bytes);
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1); // UTF-8 BOM
    if (text.isEmpty()) { skip(result, QStringLiteral("Файл пуст.")); return result; }
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    if (lines.isEmpty()) { skip(result, QStringLiteral("Нет строк.")); return result; }
    // Разделитель: чего больше в заголовке — запятых или точек с запятой.
    const QChar sep = lines.first().count(QLatin1Char(';')) > lines.first().count(QLatin1Char(','))
        ? QLatin1Char(';') : QLatin1Char(',');
    const QStringList header = splitCsvLine(lines.first(), sep);
    QHash<QString, int> columns;
    for (int i = 0; i < header.size(); ++i) {
        const QString key = normalized(header.at(i));
        if (!key.isEmpty() && !columns.contains(key)) columns.insert(key, i);
    }
    if (!columns.contains(QStringLiteral("title"))) { skip(result, QStringLiteral("Не найдена колонка названия (title/название).")); return result; }
    if (!columns.contains(QStringLiteral("password"))) { skip(result, QStringLiteral("Не найдена колонка пароля (password/пароль).")); return result; }
    for (int row = 1; row < lines.size(); ++row) {
        const QStringList fields = splitCsvLine(lines.at(row), sep);
        const auto value = [&fields, &columns](const char* key) {
            const int column = columns.value(QLatin1String(key), -1);
            return column >= 0 && column < fields.size() ? fields.at(column).trimmed() : QString();
        };
        ImportedEntry imported;
        model::Entry& e = imported.entry;
        e.title = value("title");
        e.login = value("login");
        e.password = value("password");
        e.url = value("url");
        e.note = value("note");
        // Есть логин — учётные данные, иначе строка ведёт себя как токен:
        // значение переносим в поле token (у ApiToken оно смысловое).
        if (e.login.isEmpty()) { e.type = model::EntryType::ApiToken; e.token = e.password; e.password.clear(); }
        else e.type = model::EntryType::Credentials;
        const QString group = value("group");
        if (!group.isEmpty())
            for (const QString& part : group.split(QRegularExpression(QStringLiteral("[/\\\\>]")), Qt::SkipEmptyParts))
                imported.groupPath << part.trimmed();
        if (e.title.isEmpty()) { skip(result, QStringLiteral("Пустое название (строка %1).").arg(row + 1)); continue; }
        // Проверка после возможного переноса пароля в token: интересна любая ценность.
        if (e.password.isEmpty() && e.token.isEmpty() && e.login.isEmpty()) { skip(result, QStringLiteral("Нет ни пароля, ни логина (строка %1).").arg(row + 1)); continue; }
        if (secretTooBig(e)) { skip(result, QStringLiteral("Секрет больше 1 МиБ (строка %1).").arg(row + 1)); continue; }
        result.entries << imported;
    }
    return result;
}

ImportResult parseKeePassXml(const QByteArray& bytes) {
    ImportResult result;
    QXmlStreamReader xml(bytes);
    // Рекурсивный обход Group; Entry строка Key/Value → поля; Times/Expires при валидной дате.
    QVector<ImportedEntry> stackOut; // не нужен: пишем сразу в result
    QStringList groupPath;
    model::Entry current;
    bool inEntry = false, inStringKey = false, inStringValue = false, inValueProtected = false;
    QString stringKey, stringValue;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString().toLower();
            if (name == QLatin1String("group")) {
                // Имя группы придёт вложенным <Name>; поднимаем после чтения.
                groupPath << QString();
            } else if (name == QLatin1String("name") && !groupPath.isEmpty()) {
                groupPath.last() = xml.readElementText().trimmed();
            } else if (name == QLatin1String("entry")) {
                current = model::Entry{};
                current.type = model::EntryType::Credentials;
                inEntry = true;
            } else if (name == QLatin1String("key") && inEntry) {
                inStringKey = true; stringKey.clear();
            } else if (name == QLatin1String("value") && inEntry) {
                inStringValue = true; stringValue.clear();
                inValueProtected = xml.attributes().value(QLatin1String("Protected")).toString().compare(QLatin1String("True"), Qt::CaseInsensitive) == 0;
            } else if (name == QLatin1String("expirytime") && inEntry) {
                const QDateTime expiry = QDateTime::fromString(xml.readElementText().left(19), QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
                if (expiry.isValid()) current.expiresAt = QDateTime(expiry.date(), QTime(0, 0), Qt::UTC);
            }
        } else if (token == QXmlStreamReader::Characters) {
            if (inStringKey) stringKey += xml.text();
            else if (inStringValue) stringValue += xml.text();
        } else if (token == QXmlStreamReader::EndElement) {
            const QString name = xml.name().toString().toLower();
            if (name == QLatin1String("key")) inStringKey = false;
            else if (name == QLatin1String("value")) {
                inStringValue = false;
                const QString key = stringKey.trimmed().toLower();
                // Protected-значения в незашифрованном экспорте уже открыты; шифрованный
                // экспорт не поддерживаем (Base64 без ключа бесполезен) — пропускаем запись.
                if (inValueProtected) { skip(result, QStringLiteral("Экспорт с Protected-значениями: переэкспортируйте KeePass без защиты.")); inEntry = false; }
                else if (key == QLatin1String("title")) current.title = stringValue.trimmed();
                else if (key == QLatin1String("username")) current.login = stringValue;
                else if (key == QLatin1String("password")) current.password = stringValue;
                else if (key == QLatin1String("url")) current.url = stringValue.trimmed();
                else if (key == QLatin1String("notes")) current.note = stringValue.trimmed();
            } else if (name == QLatin1String("entry") && inEntry) {
                inEntry = false;
                ImportedEntry imported;
                imported.entry = current;
                imported.groupPath = groupPath.mid(1); // корень "Root" не переносим
                imported.groupPath.removeAll(QString());
                if (current.title.isEmpty()) { skip(result, QStringLiteral("Запись без названия.")); continue; }
                if (current.password.isEmpty() && current.login.isEmpty()) { skip(result, QStringLiteral("Запись без пароля и логина.")); continue; }
                if (secretTooBig(current)) { skip(result, QStringLiteral("Секрет больше 1 МиБ.")); continue; }
                result.entries << imported;
            } else if (name == QLatin1String("group")) {
                if (!groupPath.isEmpty()) groupPath.removeLast();
            }
        }
    }
    if (xml.hasError() && result.entries.isEmpty()) skip(result, QStringLiteral("XML не разобран: %1").arg(xml.errorString()));
    return result;
}
}
