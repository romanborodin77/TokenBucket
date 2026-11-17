#pragma once
#include <QByteArray>
#include <QString>
#include <array>
#include <memory>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace tb::agent {
inline constexpr DWORD MaxRequest = 4096;
inline constexpr DWORD MaxReply = 1400000;
inline constexpr int RequestTimeoutMs = 3000;
inline constexpr int ApprovalTimeoutMs = 60000;
inline constexpr int ReplyTimeoutMs = 3000;
inline constexpr std::array ReadFields{
    "token", "login", "password", "sshPrivate", "sshPublic", "sshPassphrase", "plainValue", "textLine"
};

struct CloseHandleOnExit {
    void operator()(void* handle) const { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleOnExit>;

struct Endpoint {
    QString sid;
    DWORD session = 0;
    QString path(const QString& suffix = {}) const;
    static Endpoint current();
};

HANDLE openServer(const Endpoint& endpoint, const QString& suffix);
HANDLE openClient(const Endpoint& endpoint, const QString& suffix);
QByteArray errorReply(const char* code);
}
