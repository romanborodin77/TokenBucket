#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QList>
#include <QUuid>
namespace tb::model {
using Id = QUuid; // идентификаторы узлов и записей; нулевой Id — «нет родителя» (проект верхнего уровня)
enum class EntryType { ApiToken=0, Credentials=1, SshKey=2, PlainValue=3, TextFile=4 };
enum class ValueFormat { Text, Ip, Url };
struct TextFileData {
    QString fileName;
    QByteArray bytes;
    bool sourceSnapshot=false;
    QList<int> visibleLines;
    bool operator==(const TextFileData&) const = default;
};
// Запись хранилища — value-тип: копируется при правках (точечный откат в MainWindow::commitEntry
// держит запасную копию), мутация в модели только через VaultModel::setEntry.
// Секретные поля (token/password/sshPrivate/sshPassphrase) живут как обычные Qt-строки:
// защита от снятия памяти процесса вне объёма v1 (спека §4), запись на диск идёт
// через защищённый CBOR-писатель без обычной кучи.
struct Entry {
    Id id, groupId;
    EntryType type = EntryType::ApiToken;
    QString title, note, url, login, password, token, sshPassphrase, sshComment;
    QByteArray sshPrivate, sshPublic; // SSH-ключи — байты (файл как есть), не строки
    QDateTime created, modified;
    QDateTime expiresAt; // срок действия секрета; null — бессрочно (UTC, полночь даты)
    bool favorite = false, reverify = false; // reverify: повторная проверка владельца перед доступом к секрету
    QStringList tags; // теги записи; канонизируются в VaultModel::setEntry: trim, без пустых и дублей (без учёта регистра)
    QString parameterName,plainValue;
    ValueFormat valueFormat=ValueFormat::Text;
    TextFileData textFile;
    Id sourceEntryId;
    int sourceLine=-1;
    bool operator==(const Entry&) const = default;
};
// Сохранённое представление списка (F3): набор фильтров главного окна. Хранится внутри
// зашифрованной модели (CBOR v5), потому что имена тегов, групп и текст поиска —
// содержимое хранилища. Ссылка на удалённую группу или тег допустима: при применении
// такие фильтры просто снимаются.
struct SavedView {
    QString name;
    int library = 0;          // 0 — все, 1 — избранное, 2 — истекают; при непустой group — группа
    Id group;
    int type = -1, expiry = 0, sort = 0; // type −1 — все типы
    QStringList tags;
    QString search;
    bool operator==(const SavedView&) const = default;
};
}
