#include "AgentPipe.h"
#include <aclapi.h>
#include <sddl.h>

namespace tb::agent {
namespace {
struct FreeLocal {
    void operator()(void* ptr) const { LocalFree(ptr); }
};
}

Endpoint Endpoint::current() {
    Endpoint endpoint;
    HANDLE rawToken = nullptr;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &endpoint.session) ||
        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return endpoint;
    Handle token(rawToken);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (!size) return endpoint;
    QByteArray info(size, Qt::Uninitialized);
    if (!GetTokenInformation(token.get(), TokenUser, info.data(), size, &size)) return endpoint;
    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(info.constData())->User.Sid, &rawSid)) return endpoint;
    std::unique_ptr<wchar_t, FreeLocal> sid(rawSid);
    endpoint.sid = QString::fromWCharArray(sid.get());
    return endpoint;
}

QString Endpoint::path(const QString& suffix) const {
    return QStringLiteral(R"(\\.\pipe\TokenBucket.Agent.v1.%1.%2%3)").arg(sid).arg(session).arg(suffix);
}

HANDLE openServer(const Endpoint& endpoint, const QString& suffix) {
    if (endpoint.sid.isEmpty()) return INVALID_HANDLE_VALUE;
    const auto sddl = QStringLiteral("O:%1D:P(A;;GA;;;%1)").arg(endpoint.sid).toStdWString();
    PSECURITY_DESCRIPTOR rawSecurity = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &rawSecurity, nullptr))
        return INVALID_HANDLE_VALUE;
    std::unique_ptr<void, FreeLocal> security(rawSecurity);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE};
    return CreateNamedPipeW(endpoint.path(suffix).toStdWString().c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, MaxReply, MaxRequest, 0, &attributes);
}

HANDLE openClient(const Endpoint& endpoint, const QString& suffix) {
    if (endpoint.sid.isEmpty()) return INVALID_HANDLE_VALUE;
    Handle pipe(CreateFileW(endpoint.path(suffix).toStdWString().c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    // A predictable endpoint can be pre-created. Trust only a pipe owned by this Windows user.
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR rawSecurity = nullptr;
    const auto status = GetSecurityInfo(pipe.get(), SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &rawSecurity);
    std::unique_ptr<void, FreeLocal> security(rawSecurity);
    LPWSTR rawSid = nullptr;
    if (status != ERROR_SUCCESS || !owner || !ConvertSidToStringSidW(owner, &rawSid)) return INVALID_HANDLE_VALUE;
    std::unique_ptr<wchar_t, FreeLocal> sid(rawSid);
    DWORD session = 0;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (endpoint.sid != QString::fromWCharArray(sid.get()) ||
        !GetNamedPipeServerSessionId(pipe.get(), &session) || session != endpoint.session ||
        !SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) return INVALID_HANDLE_VALUE;
    return pipe.release();
}

QByteArray errorReply(const char* code) {
    return QByteArrayLiteral("{\"ok\":false,\"error\":\"") + code + "\"}\n";
}
}
