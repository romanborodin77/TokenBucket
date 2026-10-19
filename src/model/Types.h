#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUuid>
namespace tb::model {
using Id = QUuid;
enum class EntryType { ApiToken, Credentials, SshKey };
struct Entry {
    Id id, groupId;
    EntryType type = EntryType::ApiToken;
    QString title, note, url, login, password, token, sshPassphrase, sshComment;
    QByteArray sshPrivate, sshPublic;
    QDateTime created, modified;
    bool favorite = false, reverify = false;
};
}
