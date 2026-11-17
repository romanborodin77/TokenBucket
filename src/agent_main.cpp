#include "platform/AgentPipe.h"
#include "platform/CrashPolicy.h"
#include <cstdio>
#include <fcntl.h>
#include <io.h>

namespace {
int writeReply(QByteArrayView reply, int exitCode) {
    if (std::fwrite(reply.data(), 1, size_t(reply.size()), stdout) != size_t(reply.size())) return 1;
    return exitCode;
}

int fail(const char* code) { return writeReply(tb::agent::errorReply(code), 1); }

bool waitForIo(HANDLE pipe, OVERLAPPED& operation, DWORD& size, int timeout) {
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(operation.hEvent, timeout) == WAIT_OBJECT_0)
        return GetOverlappedResult(pipe, &operation, &size, FALSE);
    CancelIoEx(pipe, &operation);
    GetOverlappedResult(pipe, &operation, &size, TRUE);
    return false;
}

int run() {
    using namespace tb::agent;
    QByteArray request;
    while (request.size() <= MaxRequest) {
        const int ch = std::fgetc(stdin);
        if (ch == EOF || ch == '\n') break;
        request += static_cast<char>(ch);
    }
    if (request.isEmpty() || request.size() > MaxRequest || std::ferror(stdin)) return fail("invalid_request");
#ifdef TOKENBUCKET_TEST_AGENT
    const QString suffix = QStringLiteral(".test");
#else
    const QString suffix;
#endif
    Handle pipe(openClient(Endpoint::current(), suffix));
    if (pipe.get() == INVALID_HANDLE_VALUE) return fail("unavailable");
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return fail("unavailable");
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD size = 0;
    const auto requestSize = static_cast<DWORD>(request.size());
    if ((!WriteFile(pipe.get(), request.constData(), requestSize, &size, &operation) &&
        !waitForIo(pipe.get(), operation, size, RequestTimeoutMs)) || size != requestSize) return fail("unavailable");
    QByteArray reply(MaxReply, Qt::Uninitialized);
    ResetEvent(event.get());
    operation = {};
    operation.hEvent = event.get();
    const bool ok = ReadFile(pipe.get(), reply.data(), MaxReply, &size, &operation) ||
        waitForIo(pipe.get(), operation, size, ApprovalTimeoutMs + RequestTimeoutMs + ReplyTimeoutMs);
    pipe.reset();
    const QByteArrayView response(reply.constData(), ok ? size : 0);
    const bool valid = response.startsWith('{') && response.endsWith("}\n");
    const int result = valid ? writeReply(response, response.startsWith("{\"ok\":false,") ? 1 : 0)
        : fail("unavailable");
    SecureZeroMemory(reply.data(), static_cast<size_t>(reply.size()));
    return result;
}
}

int main(int argc, char**) {
    tb::platform::crash::disableDumps();
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try { return argc == 1 ? run() : fail("invalid_request"); }
    catch (...) { return fail("unavailable"); }
}
