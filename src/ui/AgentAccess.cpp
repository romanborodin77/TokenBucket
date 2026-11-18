#include "AgentAccess.h"
#include "EntryAccess.h"
#include "app/Application.h"
#include "model/SshTarget.h"
#include "model/TextFile.h"
#include "platform/AgentPipe.h"
#include "platform/SshExec.h"
#include <QApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QScopeGuard>
#include <QTimer>
#include <QWinEventNotifier>
#include <array>

namespace tb::agent {
namespace {
enum class Field { Token, Login, Password, Private, Public, Passphrase, Value, TextLine };
constexpr model::EntryType FieldTypes[] = {
    model::EntryType::ApiToken, model::EntryType::Credentials, model::EntryType::Credentials,
    model::EntryType::SshKey, model::EntryType::SshKey, model::EntryType::SshKey,
    model::EntryType::PlainValue, model::EntryType::TextFile,
};
static_assert(std::size(FieldTypes) == ReadFields.size());
const char* typeName(model::EntryType type) {
    switch (type) {
    case model::EntryType::ApiToken: return "apiToken";
    case model::EntryType::Credentials: return "credentials";
    case model::EntryType::SshKey: return "sshKey";
    case model::EntryType::PlainValue: return "plainValue";
    case model::EntryType::TextFile: return "textFile";
    }
    return "unknown";
}

struct Request {
    QString title, machine, user;
    model::Id id;
    Field field = Field::Token;
    int line = -1, port = 0;
    std::optional<SshCommand> exec;
    bool find() const { return id.isNull(); }
    bool findSsh() const { return !machine.isEmpty(); }
    const char* fieldName() const { return ReadFields[static_cast<size_t>(field)]; }
    model::EntryType fieldType() const { return FieldTypes[static_cast<size_t>(field)]; }
};

std::optional<Request> parseRequest(QByteArrayView bytes) {
    if (!bytes.isValidUtf8()) return std::nullopt;
    const auto doc = QJsonDocument::fromJson(QByteArray::fromRawData(bytes.data(), bytes.size()));
    if (!doc.isObject()) return std::nullopt;
    const auto object = doc.object();
    // QJsonObject folds duplicate keys. Count JSON member separators outside strings
    // before applying the flat schema so an overwritten member is still rejected.
    bool inString = false;
    int members = 0;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const auto ch = bytes[i];
        if (inString && ch == '\\') ++i;
        else if (ch == '"') inString = !inString;
        else if (!inString && ch == ':') ++members;
    }
    if (members != object.size() || object.value("v") != QJsonValue(1)) return std::nullopt;
    Request request;
    const auto op = object.value("op");
    if (op == "ssh_exec") {
        auto args = object;
        args.remove("v");
        args.remove("op");
        request.exec = parseSshCommand(args);
        if (!request.exec) return std::nullopt;
        request.id = request.exec->id;
        return request;
    }
    if (op == "find" && object.size() == 3) {
        request.title = object.value("title").toString();
        if (!request.title.isEmpty() && request.title.isValidUtf16()) return request;
    }
    if (op == "find_ssh") {
        request.machine = object.value("machine").toString();
        request.user = object.value("user").toString();
        request.port = object.value("port").toInt(0);
        const bool hasUser = object.contains("user"), hasPort = object.contains("port");
        if (object.size() == 3 + int(hasUser) + int(hasPort) && (!hasUser || !request.user.isEmpty())
            && (!hasPort || request.port > 0) && model::ssh::validLookup(request.machine, request.user, request.port)) return request;
        return std::nullopt;
    }
    if (op == "read") {
        const auto id = object.value("id").toString();
        request.id = model::Id(id);
        if (request.id.isNull() || request.id.toString(QUuid::WithoutBraces) != id.toLower()) return std::nullopt;
        const auto field = object.value("field");
        for (size_t i = 0; i < ReadFields.size(); ++i) {
            if (field != QJsonValue(ReadFields[i])) continue;
            request.field = static_cast<Field>(i);
            if (request.field != Field::TextLine && object.size() == 4) return request;
            if (request.field == Field::TextLine && object.size() == 5) {
                const int line = object.value("line").toInt(0);
                if (line > 0 && line <= model::textfile::MaxLines) {
                    request.line = line - 1;
                    return request;
                }
            }
        }
    }
    return std::nullopt;
}

constexpr qsizetype InlineTitleLimit = 160;
QString displayTitle(const QString& title, qsizetype limit = InlineTitleLimit) {
    auto prefix = title.left(limit);
    if (title.size() > limit && prefix.back().isHighSurrogate()) prefix.chop(1);
    const auto characters = prefix.toUcs4();
    QString safe = QStringLiteral("\"");
    for (const char32_t ch : characters) {
        if (!QChar::isPrint(ch) || ch == 0x2028 || ch == 0x2029)
            safe += QStringLiteral("\\u{%1}").arg(static_cast<quint32>(ch), 0, 16);
        else if (ch == U'"' || ch == U'\\') safe += QStringLiteral("\\") + QChar(static_cast<ushort>(ch));
        else safe += QString::fromUcs4(&ch, 1);
    }
    if (title.size() > limit) safe += QStringLiteral("…");
    return safe + QStringLiteral("\"");
}

SecureBytes valueReply(QByteArrayView bytes) {
    if (bytes.size() > model::VaultModel::MaxSecretBytes) return SecureBytes(errorReply("too_large"));
    SecureBytes encoded(QByteArray::fromRawData(bytes.data(), bytes.size()).toBase64());
    QByteArray reply;
    reply.reserve(encoded.size() + 48);
    reply += "{\"ok\":true,\"encoding\":\"base64\",\"value\":\"";
    reply += encoded.bytes();
    reply += "\"}\n";
    return SecureBytes(std::move(reply));
}

SecureBytes stringReply(const QString& text) {
    if (text.size() > model::VaultModel::MaxSecretBytes) return SecureBytes(errorReply("too_large"));
    const SecureBytes bytes(text.toUtf8());
    return valueReply(bytes.bytes());
}

SecureBytes readReply(const Request& request, const model::Entry& entry) {
    switch (request.field) {
    case Field::Token: return stringReply(entry.token);
    case Field::Login: return stringReply(entry.login);
    case Field::Password: return stringReply(entry.password);
    case Field::Private: return valueReply(entry.sshPrivate);
    case Field::Public: return valueReply(entry.sshPublic);
    case Field::Passphrase: return stringReply(entry.sshPassphrase);
    case Field::Value: return stringReply(entry.plainValue);
    case Field::TextLine: {
        const auto& file = entry.textFile;
        const auto range = model::textfile::lineRanges(file.bytes).at(request.line);
        return valueReply(QByteArrayView(file.bytes).sliced(range.offset, range.length));
    }
    }
    return SecureBytes(errorReply("invalid_request"));
}

SecureBytes sshLookupReply(const model::VaultModel::SshLookup& result) {
    if (result.tooMany) return SecureBytes(errorReply("too_many"));
    if (result.matches.isEmpty()) return SecureBytes(errorReply("not_found"));
    // Titles predate binding limits. Bound them before JSON serialisation, as
    // already done for the number and field sizes of targets and matching keys.
    QJsonArray entries;
    for (const auto& match : result.matches) {
        if (match.title.size() > 512) return SecureBytes(errorReply("too_large"));
        QJsonArray targets;
        for (const auto& target : match.targets) {
            targets.append(QJsonObject{{"alias", target.alias}, {"host", target.host},
                {"user", target.username}, {"port", target.port}});
        }
        entries.append(QJsonObject{{"id", match.id.toString(QUuid::WithoutBraces)}, {"title", match.title},
            {"type", "sshKey"}, {"targets", targets}});
    }
    auto response = QJsonDocument(QJsonObject{{"ok", true}, {"entries", entries}}).toJson(QJsonDocument::Compact) + '\n';
    if (response.size() > 256 * 1024) return SecureBytes(errorReply("too_large"));
    return SecureBytes(std::move(response));
}

SecureBytes sshExecReply(const SshResult& result) {
    if (result.error) return SecureBytes(errorReply(result.error));
    const QJsonObject fields{{"exitCode", result.exitCode}, {"stdout", QString::fromUtf8(result.out)},
        {"stderr", QString::fromUtf8(result.err)}};
    auto json = QJsonDocument(fields).toJson(QJsonDocument::Compact);
    json.replace(0, 1, result.exitCode == 0 ? "{\"ok\":true," : "{\"ok\":false,\"error\":\"remote_exit\",");
    return SecureBytes(std::move(json) + '\n');
}
}

class Access::Broker : public QObject {
public:
    Broker(Application& app, QString endpointSuffix, Decision consent, bool sshExec)
        : app_(app), suffix_(std::move(endpointSuffix)), consent_(std::move(consent)), sshExec_(sshExec),
          endpoint_(Endpoint::current()) {
        connect(&notifier_, &QWinEventNotifier::activated, this, [this] { complete(); });
        connect(&timer_, &QTimer::timeout, this, [this] {
            if (phase_ == Phase::Approval) {
                if (!requestAlive()) cancelApproval();
            } else if (phase_ == Phase::Execution) checkExecution();
            else reset();
        });
        connect(&app_, &Application::stateChanged, this, [this] {
            if (phase_ == Phase::Approval && !sessionCurrent()) cancelApproval();
            else if (phase_ == Phase::Execution) checkExecution();
            else if (phase_ == Phase::Reply && !sessionCurrent()) reset();
        });
        connect(&app_, &Application::busyChanged, this, [this](bool busy) {
            if (busy && phase_ == Phase::Execution) checkExecution();
        });
    }

    ~Broker() override { stopIo(); }

    bool start() {
        event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        pipe_.reset(openServer(endpoint_, suffix_));
        if (!event_ || pipe_.get() == INVALID_HANDLE_VALUE) return false;
        notifier_.setHandle(event_.get());
        arm(Phase::Connect);
        return true;
    }

private:
    enum class Phase { Connect, Request, Approval, Execution, Reply, Drain };

    void arm(Phase phase) {
        phase_ = phase;
        timer_.stop();
        overlapped_ = {};
        overlapped_.hEvent = event_.get();
        ResetEvent(event_.get());
        DWORD count = 0;
        BOOL ready = FALSE;
        switch (phase_) {
        case Phase::Connect: ready = ConnectNamedPipe(pipe_.get(), &overlapped_); break;
        case Phase::Request:
            timer_.start(RequestTimeoutMs);
            ready = ReadFile(pipe_.get(), input_.data(), MaxRequest, &count, &overlapped_); break;
        case Phase::Reply:
            timer_.start(ReplyTimeoutMs);
            ready = WriteFile(pipe_.get(), output_.bytes().constData(), static_cast<DWORD>(output_.size()), &count, &overlapped_); break;
        case Phase::Drain:
            // Keep the instance connected until the client consumes its one response.
            // A blocked client cannot hold it indefinitely.
            timer_.start(ReplyTimeoutMs);
            ready = ReadFile(pipe_.get(), input_.data(), MaxRequest, &count, &overlapped_); break;
        case Phase::Approval:
        case Phase::Execution: return;
        }
        status_ = ready ? ERROR_SUCCESS : GetLastError();
        count_ = count;
        notifier_.setEnabled(true);
        if (status_ != ERROR_IO_PENDING) SetEvent(event_.get());
    }

    void stopIo() {
        notifier_.setEnabled(false);
        timer_.stop();
        if (status_ == ERROR_IO_PENDING) {
            CancelIoEx(pipe_.get(), &overlapped_);
            DWORD ignored = 0;
            GetOverlappedResult(pipe_.get(), &overlapped_, &ignored, TRUE);
            status_ = ERROR_OPERATION_ABORTED;
        }
    }

    void reset() {
        stopIo();
        DisconnectNamedPipe(pipe_.get());
        output_.clear();
        arm(Phase::Connect);
    }

    void complete() {
        notifier_.setEnabled(false);
        if (status_ == ERROR_IO_PENDING) {
            status_ = GetOverlappedResult(pipe_.get(), &overlapped_, &count_, FALSE) ? ERROR_SUCCESS : GetLastError();
        }
        if (phase_ == Phase::Connect && (status_ == ERROR_SUCCESS || status_ == ERROR_PIPE_CONNECTED)) arm(Phase::Request);
        else if (phase_ == Phase::Request && status_ == ERROR_SUCCESS && count_ > 0) {
            phase_ = Phase::Approval;
            deadline_ = QDeadlineTimer(ApprovalTimeoutMs, Qt::PreciseTimer);
            generation_ = app_.generation();
            cancelled_ = false;
            timer_.start(100);
            output_ = handleRequest(QByteArrayView(input_.data(), count_));
            if (phase_ == Phase::Execution) return;
            if (requestAlive()) arm(Phase::Reply);
            else reset();
        } else if (phase_ == Phase::Reply && status_ == ERROR_SUCCESS && count_ == output_.size()) {
            output_.clear();
            arm(Phase::Drain);
        } else reset();
    }

    bool sessionCurrent() const { return app_.gate() == Application::Gate::Ready && app_.generation() == generation_; }

    bool pipeAlive() const {
        DWORD available = 0;
        return PeekNamedPipe(pipe_.get(), nullptr, 0, nullptr, &available, nullptr) && available == 0;
    }
    bool requestAlive() const { return !cancelled_ && !deadline_.hasExpired() && pipeAlive(); }

    void checkExecution() {
        const bool client = pipeAlive();
        const bool current = sessionCurrent();
        const auto* entry = current && executingEntry_ ? app_.model()->entry(executingEntry_->id) : nullptr;
        if (!client || !current || app_.busy() || !entry || *entry != *executingEntry_) {
            if (!executionError_) executionError_ = current ? "stale" : "locked";
            executingEntry_.reset();
            execution_->cancel();
        }
        const auto* result = execution_->result();
        if (!result) return;
        if (client) output_ = executionError_ ? SecureBytes(errorReply(executionError_)) : sshExecReply(*result);
        execution_.reset();
        executingEntry_.reset();
        if (client) arm(Phase::Reply);
        else reset();
    }

    void cancelApproval() {
        cancelled_ = true;
        if (!dialog_) return;
        const auto children = dialog_->findChildren<QDialog*>();
        for (auto* child : children) child->reject();
        dialog_->reject();
    }

    SecureBytes handleRequest(QByteArrayView bytes) {
        const auto fail = [](const char* error) { return SecureBytes(errorReply(error)); };
        DWORD clientSession = 0;
        DWORD pid = 0;
        if (!GetNamedPipeClientSessionId(pipe_.get(), &clientSession) || clientSession != endpoint_.session ||
            !GetNamedPipeClientProcessId(pipe_.get(), &pid)) return fail("denied");
        const auto request = parseRequest(bytes);
        if (!request) return fail("invalid_request");
        if (request->exec && !sshExec_) return fail("ssh_disabled");
        if (!sessionCurrent()) return fail("locked");
        if (app_.busy() || QApplication::activeModalWidget() || !cooldown_.hasExpired()) return fail("busy");
        if (!requestAlive()) return fail("cancelled");

        const auto lookup = [&]() -> std::pair<const model::Entry*, bool> {
            if (!request->find()) return {app_.model()->entry(request->id), false};
            const model::Entry* match = nullptr;
            for (const auto& entry : app_.model()->entries()) {
                if (entry.title != request->title) continue;
                if (match) return {nullptr, true};
                match = &entry;
            }
            return {match, false};
        };
        const auto findSsh = [&] { return app_.model()->sshMatches(request->machine, request->user, request->port); };
        const auto sshBefore = request->findSsh() ? findSsh() : model::VaultModel::SshLookup{};
        const auto [entry, ambiguous] = request->findSsh() ? std::pair<const model::Entry*, bool>{nullptr, false} : lookup();
        const auto before = entry ? std::optional(*entry) : std::nullopt;
        QList<model::SshTarget> targets;
        if (request->exec) {
            const auto& command = *request->exec;
            targets = app_.model()->sshTargetsFor(command.id, command.machine, command.user, command.port);
            if (targets.isEmpty()) return fail("ssh_binding_not_found");
            if (targets.size() != 1) return fail("ssh_binding_ambiguous");
            if (before->sshPrivate.isEmpty()) return fail("ssh_identity_missing");
        }
        const auto consistent = [&] {
            if (!requestAlive() || !sessionCurrent() || app_.busy()) return false;
            if (request->findSsh()) return findSsh() == sshBefore;
            const auto [current, nowAmbiguous] = lookup();
            return nowAmbiguous == ambiguous && (before ? current && *current == *before : !current);
        };

        QString description;
        if (request->exec) {
            const auto& target = targets.front();
            description = QStringLiteral("Выполнить команду SSH из записи %1?\nID записи: %2\nХост: %3\nПользователь: %4\nПорт: %5\n\nКоманда:\n")
                .arg(displayTitle(before->title), request->id.toString(QUuid::WithoutBraces),
                    target.host, target.username, QString::number(target.port));
            description += request->exec->command;
        } else if (request->findSsh()) {
            description = QStringLiteral("Разрешить поиск ключей SSH для машины %1? Будут переданы названия, ID и подходящие привязки.")
                .arg(displayTitle(request->machine));
            if (!request->user.isEmpty()) description += QStringLiteral("\nПользователь: %1").arg(displayTitle(request->user));
            if (request->port) description += QStringLiteral("\nПорт: %1").arg(request->port);
        } else if (request->find()) {
            description = QStringLiteral("Разрешить поиск по точному названию %1? Будут переданы только название, тип и ID.")
                .arg(displayTitle(request->title));
        } else {
            description = QStringLiteral("Передать поле %1 из записи %2?\nID записи: %3\n\nЗначение покинет хранилище. Программа или ИИ-агент могут сохранить его или отправить дальше.")
                .arg(QString::fromLatin1(request->fieldName()), before ? displayTitle(before->title) : QStringLiteral("(недоступна)"),
                    request->id.toString(QUuid::WithoutBraces));
            if (request->field == Field::TextLine) description += QStringLiteral("\nНомер строки: %1").arg(request->line + 1);
        }
        description += QStringLiteral("\n\nЛокальный процесс PID %1. Программа, вызвавшая помощника, не проверена.").arg(pid);
        QMessageBox dialog(QMessageBox::Question, QStringLiteral("TokenBucket — доступ агента"), {}, QMessageBox::Yes | QMessageBox::No);
        dialog.setTextFormat(Qt::PlainText);
        dialog.setText(description);
        const auto queryText = request->findSsh() ? request->machine : request->title;
        if (request->find() && queryText.size() > InlineTitleLimit)
            dialog.setDetailedText(displayTitle(queryText, queryText.size()));
        dialog.setDefaultButton(QMessageBox::No);
        dialog.setEscapeButton(QMessageBox::No);
        dialog.setWindowModality(Qt::ApplicationModal);
        dialog.setWindowFlag(Qt::WindowStaysOnTopHint);
        dialog_ = &dialog;
        bool released = false;
        const auto finishDialog = qScopeGuard([&] {
            dialog_ = nullptr;
            if (!released) cooldown_ = QDeadlineTimer(750);
        });
        const bool approved = consent_ ? consent_(&dialog) : dialog.exec() == QMessageBox::Yes;
        if (!approved) return fail("denied");
        if (!consistent()) return fail("stale");
        if (request->findSsh()) {
            if (!sshBefore.tooMany && !sshBefore.matches.isEmpty()) released = true;
            return sshLookupReply(sshBefore);
        }
        if (ambiguous) return fail("ambiguous");
        if (!before) return fail("not_found");
        if (request->find()) {
            const QJsonObject match{{"id", before->id.toString(QUuid::WithoutBraces)},
                {"title", before->title}, {"type", QString::fromLatin1(typeName(before->type))}};
            released = true;
            return SecureBytes(QJsonDocument(QJsonObject{{"ok", true}, {"entry", match}}).toJson(QJsonDocument::Compact) + '\n');
        }
        if (request->exec) {
            if (!ui::authorizeEntry(app_, request->id, &dialog, true)) return fail("denied");
            if (!consistent()) return fail("stale");
            execution_ = std::make_unique<SshExecution>(targets.front(), *request->exec, before->sshPrivate,
                before->sshPassphrase);
            executingEntry_ = before;
            executionError_ = nullptr;
            phase_ = Phase::Execution;
            released = true;
            timer_.start(25);
            return {};
        }
        if (request->fieldType() != before->type || (request->field == Field::TextLine &&
            !model::textfile::isVisible(before->textFile, request->line))) return fail("unavailable_field");
        if (!ui::authorizeEntry(app_, request->id, &dialog, true)) return fail("denied");
        if (!consistent()) return fail("stale");
        released = true;
        return readReply(*request, *app_.model()->entry(request->id));
    }

    Application& app_;
    QString suffix_;
    Decision consent_;
    bool sshExec_;
    Endpoint endpoint_;
    Handle pipe_, event_;
    QWinEventNotifier notifier_;
    QTimer timer_;
    OVERLAPPED overlapped_{};
    Phase phase_ = Phase::Connect;
    DWORD status_ = ERROR_SUCCESS, count_ = 0;
    std::array<char, MaxRequest> input_{};
    SecureBytes output_;
    QDeadlineTimer deadline_, cooldown_;
    quint64 generation_ = 0;
    bool cancelled_ = false;
    QMessageBox* dialog_ = nullptr;
    std::optional<model::Entry> executingEntry_;
    std::unique_ptr<SshExecution> execution_;
    const char* executionError_ = nullptr;
};

Access::Access(Application& app, const QString& endpointSuffix, Decision consent, bool sshExec)
    : broker_(std::make_unique<Broker>(app, endpointSuffix, std::move(consent), sshExec)) {}
Access::~Access() = default;
bool Access::start() { return broker_->start(); }
}
