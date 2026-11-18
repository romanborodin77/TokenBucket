#pragma once
#include "model/SshTarget.h"
#include <QJsonObject>
#include <QUuid>
#include <optional>

namespace tb::agent {
inline constexpr int MaxSshCommandBytes = 1024;
inline constexpr int DefaultSshSeconds = 15;
inline constexpr int MaxSshSeconds = 30;
inline constexpr int MaxSshOutputBytes = 64 * 1024;

struct SshCommand {
    QUuid id;
    QString machine, user, command;
    int port, timeoutSeconds;
};

inline std::optional<SshCommand> parseSshCommand(const QJsonObject& args) {
    const auto id = args.value("id").toString();
    const bool hasTimeout = args.contains("timeoutSeconds");
    SshCommand request{QUuid(id), args.value("machine").toString(), args.value("user").toString(),
        args.value("command").toString(), args.value("port").toInt(),
        hasTimeout ? args.value("timeoutSeconds").toInt() : DefaultSshSeconds};
    if (args.size() != 5 + int(hasTimeout) || request.id.isNull() ||
        request.id.toString(QUuid::WithoutBraces) != id.toLower() || request.user.isEmpty() || request.port < 1 ||
        !model::ssh::validLookup(request.machine, request.user, request.port) || request.command.trimmed().isEmpty() ||
        !model::ssh::printable(request.command, MaxSshCommandBytes) || request.command.toUtf8().size() > MaxSshCommandBytes ||
        request.timeoutSeconds < 1 || request.timeoutSeconds > MaxSshSeconds) return std::nullopt;
    return request;
}
}
