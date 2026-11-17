#include "platform/AgentClient.h"
#include "platform/CrashPolicy.h"
#include <QCoreApplication>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>

namespace tb::agent { int runMcp(const QString& endpointSuffix); }

namespace {
int writeReply(QByteArrayView reply, int exitCode) {
    if (std::fwrite(reply.data(), 1, size_t(reply.size()), stdout) != size_t(reply.size())) return 1;
    return exitCode;
}

int fail(const char* code) { return writeReply(tb::agent::errorReply(code), 1); }

int runOneShot(const QString& suffix) {
    using namespace tb::agent;
    QByteArray request;
    while (request.size() <= MaxRequest) {
        const int ch = std::fgetc(stdin);
        if (ch == EOF) break;
        if (ch == '\n') {
            if (std::fgetc(stdin) != EOF) return fail("invalid_request");
            break;
        }
        request += static_cast<char>(ch);
    }
    if (request.isEmpty() || request.size() > MaxRequest || std::ferror(stdin)) return fail("invalid_request");
    Exchange call(std::move(request), suffix);
    call.advance(true);
    const auto reply = call.reply();
    if (reply.isEmpty()) return fail("unavailable");
    return writeReply(reply, Exchange::success(reply) ? 0 : 1);
}
}

int main(int argc, char** argv) {
    tb::platform::crash::disableDumps();
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef TOKENBUCKET_TEST_AGENT
    const QString suffix = QStringLiteral(".test");
#else
    const QString suffix;
#endif
    if (argc == 2 && std::strcmp(argv[1], "--mcp") == 0) {
        try {
            QCoreApplication app(argc, argv);
            return tb::agent::runMcp(suffix);
        } catch (...) { return 1; }
    }
    try { return argc == 1 ? runOneShot(suffix) : fail("invalid_request"); }
    catch (...) { return fail("unavailable"); }
}
