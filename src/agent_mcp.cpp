#include "Version.h"
#include "model/SshTarget.h"
#include "model/TextFile.h"
#include "platform/AgentClient.h"
#include "platform/SshExecRequest.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <list>

namespace tb::agent {
namespace {
constexpr qsizetype MaxFrame = 64 * 1024;
constexpr qsizetype MaxQueuedBytes = 3 * 1024 * 1024;
constexpr size_t MaxQueuedFrames = 32;
constexpr int OutputTimeoutMs = 15000;
constexpr auto ModernVersion = "2026-07-28";
constexpr auto LegacyVersion = "2025-11-25";
constexpr auto ProtocolVersion = "io.modelcontextprotocol/protocolVersion";
constexpr auto ClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
constexpr auto ServerInfo = "io.modelcontextprotocol/serverInfo";
constexpr auto FindTool = "tokenbucket_find";
constexpr auto ReadTool = "tokenbucket_read";
constexpr auto FindSshTool = "tokenbucket_find_ssh";
constexpr auto SshExecTool = "tokenbucket_ssh_exec";
constexpr auto Instructions = "Start TokenBucket with --agent-access in the same Windows session. "
    "Wait for owner consent and verification. Do not retry automatically after a denial.";

QJsonObject serverInfo() { return {{"name", "TokenBucket"}, {"version", TOKENBUCKET_VERSION}}; }
QJsonObject capabilities() { return {{"tools", QJsonObject{}}}; }
QJsonArray supportedVersions() { return {ModernVersion, LegacyVersion}; }

QJsonArray toolList() {
    const QJsonObject text{{"type", "string"}, {"minLength", 1}};
    QJsonArray fields;
    for (const auto* field : ReadFields) fields.append(field);
    const auto tool = [](const char* name, const char* description, QJsonObject properties, QJsonArray required,
                         bool remote = false) {
        return QJsonObject{{"name", name}, {"description", description},
            {"annotations", QJsonObject{{"readOnlyHint", !remote}, {"openWorldHint", remote}, {"idempotentHint", false}}},
            {"inputSchema", QJsonObject{{"type", "object"}, {"additionalProperties", false},
                {"properties", properties}, {"required", required}}}};
    };
    return {
        tool(FindTool, "Find one TokenBucket entry by exact case-sensitive title after owner consent. Returns ID, title and type.",
            {{"title", text}}, {"title"}),
        tool(ReadTool, "Retrieve one field after owner consent and verification. Returns raw bytes in base64. The MCP client or model can retain or forward this value.",
            {{"id", QJsonObject{{"type", "string"}, {"format", "uuid"}}},
             {"field", QJsonObject{{"type", "string"}, {"enum", fields}}},
             {"line", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", model::textfile::MaxLines},
                  {"description", "Required only for textLine. One-based number of a visible line."}}}}, {"id", "field"}),
        tool(FindSshTool, "Find SSH keys for an exact machine alias or host after owner consent. Select a returned key ID before requesting a field.",
            {{"machine", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", model::ssh::MaxHost}}},
             {"user", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", model::ssh::MaxUser}}},
             {"port", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", model::ssh::MaxPort}}}}, {"machine"}),
        tool(SshExecTool, "Run one command on an exact stored SSH binding after owner consent and verification. "
            "Requires the TokenBucket GUI to be started with --agent-access --ssh-exec. "
            "Use the selected ID, machine alias or host, exact user and port. Returns command output and exit status.",
            {{"id", QJsonObject{{"type", "string"}, {"format", "uuid"}}},
             {"machine", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", model::ssh::MaxHost}}},
             {"user", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", model::ssh::MaxUser}}},
             {"port", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", model::ssh::MaxPort}}},
             {"command", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", MaxSshCommandBytes},
                 {"description", "One printable command, at most 1024 UTF-8 bytes."}}},
             {"timeoutSeconds", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", MaxSshSeconds},
                 {"default", DefaultSshSeconds}}}}, {"id", "machine", "user", "port", "command"}, true)
    };
}

bool validId(const QJsonValue& id) {
    if (id.isString()) return id.toString().isValidUtf16();
    return id.isDouble() && id == QJsonValue(id.toInteger());
}

// Qt folds duplicate keys. Count both the original separators and the parsed
// members so a second key cannot silently replace a validated parameter.
int jsonMembers(const QJsonValue& value, int depth = 0) {
    if (depth > 32) return -1;
    int count = 0;
    if (value.isObject()) {
        const auto object = value.toObject();
        count = static_cast<int>(object.size());
        for (const auto& child : object) {
            const int nested = jsonMembers(child, depth + 1);
            if (nested < 0) return -1;
            count += nested;
        }
    } else if (value.isArray()) {
        const auto array = value.toArray();
        for (const auto& child : array) {
            const int nested = jsonMembers(child, depth + 1);
            if (nested < 0) return -1;
            count += nested;
        }
    }
    return count;
}

int jsonSeparators(QByteArrayView text) {
    int count = 0;
    bool inString = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (inString && ch == '\\') ++i;
        else if (ch == '"') inString = !inString;
        else if (!inString && ch == ':') ++count;
    }
    return count;
}

QByteArray brokerRequest(const QJsonObject& params) {
    const auto name = params.value("name");
    const auto args = params.value("arguments").toObject();
    QJsonObject request{{"v", 1}};
    if (name == FindTool) {
        const auto title = args.value("title").toString();
        if (args.size() != 1 || title.isEmpty() || !title.isValidUtf16()) return {};
        request.insert("op", "find");
        request.insert("title", title);
    } else if (name == FindSshTool) {
        const auto machine = args.value("machine").toString();
        const auto user = args.value("user").toString();
        const int port = args.value("port").toInt();
        const bool hasUser = args.contains("user"), hasPort = args.contains("port");
        if (args.size() != 1 + int(hasUser) + int(hasPort) || (hasUser && user.isEmpty()) || (hasPort && port < 1)
            || !model::ssh::validLookup(machine, user, port)) return {};
        request.insert("op", "find_ssh");
        request.insert("machine", machine);
        if (hasUser) request.insert("user", user);
        if (hasPort) request.insert("port", port);
    } else if (name == SshExecTool) {
        if (!parseSshCommand(args)) return {};
        request = args;
        request.insert("v", 1);
        request.insert("op", "ssh_exec");
    } else if (name == ReadTool) {
        const auto id = args.value("id").toString();
        const auto uuid = QUuid(id);
        if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id.toLower()) return {};
        const auto field = args.value("field").toString();
        if (std::none_of(ReadFields.begin(), ReadFields.end(), [&](const char* item) { return field == QLatin1StringView(item); })) return {};
        if (field == "textLine") {
            const int line = args.value("line").toInt();
            if (args.size() != 3 || line < 1 || line > model::textfile::MaxLines) return {};
            request.insert("line", line);
        } else if (args.size() != 2) return {};
        request.insert("op", "read");
        request.insert("id", id);
        request.insert("field", field);
    } else return {};
    const auto encoded = QJsonDocument(request).toJson(QJsonDocument::Compact);
    return encoded.size() <= MaxRequest ? encoded : QByteArray{};
}

class McpServer {
public:
    explicit McpServer(QString suffix) : suffix_(std::move(suffix)) {
        QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] { tick(); });
    }

    bool start() {
        // Inherited stdio handles need not support overlapped I/O. The local
        // pipe's nonblocking mode bounds each write, even when the host stops reading.
        DWORD mode = PIPE_NOWAIT, outbound = 0, inbound = 0;
        if (GetFileType(inputHandle_) != FILE_TYPE_PIPE || GetFileType(outputHandle_) != FILE_TYPE_PIPE ||
            !GetNamedPipeInfo(outputHandle_, nullptr, &outbound, &inbound, nullptr) ||
            !SetNamedPipeHandleState(outputHandle_, &mode, nullptr, nullptr)) return false;
        const auto capacity = outbound && inbound ? std::min(outbound, inbound) : std::max(outbound, inbound);
        outputChunk_ = std::clamp<DWORD>(capacity, 1, 4096);
        timer_.start(10);
        return true;
    }

private:
    enum class Legacy { BeforeInitialize, BeforeInitialized, Ready };
    struct OutputFrame {
        OutputFrame(QByteArray data, QJsonValue requestId, bool tool)
            : bytes(std::move(data)), id(std::move(requestId)), toolResult(tool) {}
        ~OutputFrame() { if (toolResult) SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size())); }
        QByteArray bytes;
        QJsonValue id;
        bool toolResult;
        qsizetype offset = 0;
        QDeadlineTimer deadline{OutputTimeoutMs};
    };

    void stop(int exitCode) {
        timer_.stop();
        call_.reset();
        output_.clear();
        QCoreApplication::exit(exitCode);
    }

    void queue(QByteArray bytes, const QJsonValue& id, bool toolResult = false) {
        if (bytes.size() > MaxQueuedBytes - queuedBytes_ || output_.size() >= MaxQueuedFrames) {
            if (toolResult) SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
            stop(1);
            return;
        }
        queuedBytes_ += bytes.size();
        output_.emplace_back(std::move(bytes), id, toolResult);
    }

    void flushOutput() {
        qsizetype allowance = 128 * 1024;
        while (!output_.empty() && allowance > 0) {
            auto& frame = output_.front();
            DWORD written = 0;
            const auto count = static_cast<DWORD>(std::min({frame.bytes.size() - frame.offset, allowance, qsizetype(outputChunk_)}));
            if (!WriteFile(outputHandle_, frame.bytes.constData() + frame.offset, count, &written, nullptr)) {
                stop(1);
                return;
            }
            if (!written) return;
            allowance -= written;
            frame.offset += written;
            if (frame.offset == frame.bytes.size()) {
                queuedBytes_ -= frame.bytes.size();
                output_.pop_front();
            }
        }
    }

    void cancel(const QJsonValue& id) {
        if (!validId(id)) return;
        if (call_ && id == callId_) call_.reset();
        const auto frame = std::find_if(output_.begin(), output_.end(), [&](const auto& item) {
            return item.toolResult && item.id == id;
        });
        if (frame == output_.end()) return;
        // A started response cannot be removed without corrupting the byte stream.
        // Close the stream before sending any more of the cancelled frame.
        if (frame->offset) { stop(0); return; }
        queuedBytes_ -= frame->bytes.size();
        output_.erase(frame);
    }

    void json(QJsonObject object) { queue(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n', object.value("id")); }

    void error(const QJsonValue& id, int code, const char* message, QJsonObject data = {}) {
        QJsonObject body{{"code", code}, {"message", message}};
        if (!data.isEmpty()) body.insert("data", data);
        QJsonObject envelope{{"jsonrpc", "2.0"}, {"error", body}};
        if (validId(id)) envelope.insert("id", id);
        json(envelope);
    }

    void result(const QJsonValue& id, QJsonObject body, bool modern) {
        if (modern) {
            body.insert("resultType", "complete");
            body.insert("_meta", QJsonObject{{ServerInfo, serverInfo()}});
        }
        json({{"jsonrpc", "2.0"}, {"id", id}, {"result", body}});
    }

    void toolResult(const QJsonValue& id, QByteArrayView response, bool modern) {
        // Wrap the single broker reply without creating additional JSON DOM copies of the secret.
        constexpr char Hex[] = "0123456789abcdef";
        const auto identifier = id.toJson(QJsonValue::JsonFormat::Compact);
        QByteArray encoded;
        encoded.reserve(response.size() * 2 + identifier.size() + 256);
        encoded += "{\"jsonrpc\":\"2.0\",\"id\":";
        encoded += identifier;
        encoded += ",\"result\":{";
        if (modern) {
            encoded += "\"resultType\":\"complete\",\"_meta\":";
            encoded += QJsonDocument(QJsonObject{{ServerInfo, serverInfo()}}).toJson(QJsonDocument::Compact);
            encoded += ',';
        }
        encoded += Exchange::success(response) ? "\"isError\":false," : "\"isError\":true,";
        encoded += "\"content\":[{\"type\":\"text\",\"text\":\"";
        for (const unsigned char ch : response.first(response.size() - 1)) {
            if (ch < 32) {
                encoded += "\\u00";
                encoded += Hex[ch >> 4];
                encoded += Hex[ch & 15];
            } else {
                if (ch == '"' || ch == '\\') encoded += '\\';
                encoded += static_cast<char>(ch);
            }
        }
        encoded += "\"}]}}\n";
        queue(std::move(encoded), id, true);
    }

    void dispatch(QByteArrayView frame) {
        QJsonParseError parseError{};
        const auto parsed = QJsonValue::fromJson(frame, &parseError);
        if (parseError.error != QJsonParseError::NoError) { error({}, -32700, "Parse error"); return; }
        const auto request = parsed.toObject();
        const auto id = request.value("id");
        const auto method = request.value("method").toString();
        if (!parsed.isObject() || jsonMembers(parsed) != jsonSeparators(frame)) {
            error({}, -32600, "Invalid request");
            return;
        }
        if (request.value("jsonrpc") != "2.0" || method.isEmpty() || (!id.isUndefined() && !validId(id))) {
            error(id, -32600, "Invalid request");
            return;
        }
        const auto paramsValue = request.value("params");
        if (!paramsValue.isUndefined() && !paramsValue.isObject()) {
            if (!id.isUndefined()) error(id, -32602, "Invalid params");
            return;
        }
        const auto params = paramsValue.toObject();
        if (id.isUndefined()) {
            if (method == "notifications/initialized" && legacy_ == Legacy::BeforeInitialized) legacy_ = Legacy::Ready;
            if (method == "notifications/cancelled") cancel(params.value("requestId"));
            return;
        }
        if ((call_ && id == callId_) || std::any_of(output_.begin(), output_.end(), [&](const auto& frame) { return frame.id == id; })) {
            error({}, -32600, "Request ID already active");
            return;
        }

        const auto metadata = params.value("_meta").toObject();
        const auto version = metadata.value(ProtocolVersion);
        if (method == "initialize" && version.isUndefined()) {
            const auto info = params.value("clientInfo").toObject();
            if (legacy_ != Legacy::BeforeInitialize || !params.value("protocolVersion").isString() ||
                !params.value("capabilities").isObject() || !info.value("name").isString() || !info.value("version").isString()) {
                error(id, -32602, "Invalid initialize params");
                return;
            }
            legacy_ = Legacy::BeforeInitialized;
            result(id, {{"protocolVersion", LegacyVersion}, {"capabilities", capabilities()}, {"serverInfo", serverInfo()},
                {"instructions", Instructions}}, false);
            return;
        }

        const bool modern = method == "server/discover" || !version.isUndefined() || legacy_ != Legacy::Ready;
        if (method == "ping" && version.isUndefined()) { result(id, {}, false); return; }
        if (modern) {
            if (!version.isString() || !metadata.value(ClientCapabilities).isObject()) {
                error(id, -32602, "Missing MCP protocol metadata");
                return;
            }
            if (version != ModernVersion) {
                error(id, -32022, "Unsupported protocol version", {{"requested", version}, {"supported", supportedVersions()}});
                return;
            }
        }
        if (method == "server/discover") {
            result(id, {{"supportedVersions", supportedVersions()}, {"capabilities", capabilities()},
                {"instructions", Instructions}, {"ttlMs", 300000}, {"cacheScope", "public"}}, true);
        } else if (method == "tools/list") {
            if (params.contains("cursor")) { error(id, -32602, "Invalid cursor"); return; }
            QJsonObject body{{"tools", toolList()}};
            if (modern) {
                body.insert("ttlMs", 300000);
                body.insert("cacheScope", "public");
            }
            result(id, body, modern);
        } else if (method == "tools/call") {
            auto encoded = brokerRequest(params);
            if (encoded.isEmpty()) { error(id, -32602, "Unknown tool or invalid arguments"); return; }
            if (call_) { toolResult(id, errorReply("busy"), modern); return; }
            callId_ = id;
            callModern_ = modern;
            call_ = std::make_unique<Exchange>(std::move(encoded), suffix_);
        } else error(id, -32601, "Method not found");
    }

    void tick() {
        if (!output_.empty() && output_.front().deadline.hasExpired()) { stop(1); return; }
        if (!input_.isEmpty() && inputDeadline_.hasExpired()) { stop(1); return; }
        DWORD available = 0;
        if (!PeekNamedPipe(inputHandle_, nullptr, 0, nullptr, &available, nullptr)) { stop(0); return; }

        if (available) {
            char chunk[4096];
            DWORD received = 0;
            if (!ReadFile(inputHandle_, chunk, std::min<DWORD>(available, sizeof(chunk)), &received, nullptr) || !received) {
                stop(0);
                return;
            }
            if (input_.isEmpty()) inputDeadline_ = QDeadlineTimer(RequestTimeoutMs);
            input_.append(chunk, received);
            // Take complete input messages, in order, before publishing a pending response.
            // In particular, an observed cancellation closes the local pipe first.
            for (auto newline = input_.indexOf('\n'); newline >= 0 && newline <= MaxFrame; newline = input_.indexOf('\n')) {
                dispatch(QByteArrayView(input_.constData(), newline));
                if (!timer_.isActive()) return;
                input_.remove(0, newline + 1);
                inputDeadline_ = QDeadlineTimer(RequestTimeoutMs);
            }
            if (input_.size() > MaxFrame) { stop(1); return; }
            if (!PeekNamedPipe(inputHandle_, nullptr, 0, nullptr, &available, nullptr)) { stop(0); return; }
        }
        const bool complete = call_ && call_->advance();
        if (available || !input_.isEmpty()) return;
        if (complete) {
            const auto completed = std::move(call_);
            const auto response = completed->reply();
            if (response.isEmpty() || !response.isValidUtf8()) toolResult(callId_, errorReply("unavailable"), callModern_);
            else toolResult(callId_, response, callModern_);
        }
        flushOutput();
    }

    QString suffix_;
    HANDLE inputHandle_ = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE outputHandle_ = GetStdHandle(STD_OUTPUT_HANDLE);
    QTimer timer_;
    QByteArray input_;
    QDeadlineTimer inputDeadline_;
    std::list<OutputFrame> output_;
    qsizetype queuedBytes_ = 0;
    DWORD outputChunk_ = 0;
    std::unique_ptr<Exchange> call_;
    QJsonValue callId_;
    bool callModern_ = false;
    Legacy legacy_ = Legacy::BeforeInitialize;
};
}

int runMcp(const QString& endpointSuffix) {
    McpServer server(endpointSuffix);
    return server.start() ? QCoreApplication::exec() : 1;
}
}
