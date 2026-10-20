#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUuid>
namespace tb::model {
using Id = QUuid; // идентификаторы узлов и записей; нулевой Id — «нет родителя» (проект верхнего уровня)
enum class EntryType { ApiToken, Credentials, SshKey };
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
};
}
