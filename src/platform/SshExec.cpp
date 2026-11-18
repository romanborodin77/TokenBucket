#include "SshExec.h"
#include "AgentPipe.h"
#include "vault/SecureBytes.h"
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <tlhelp32.h>

namespace tb::agent {
namespace {
constexpr auto AskpassVariable = L"TOKENBUCKET_SSH_ASKPASS";
constexpr auto AskpassPrefix = u".askpass.";
constexpr DWORD MaxPassphrase = 1023; // OpenSSH's ASKPASS pipe buffer, excluding the terminal NUL.

void freeLocal(void* ptr) { LocalFree(ptr); }

QString knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) return {};
    const auto free = qScopeGuard([&] { CoTaskMemFree(path); });
    return QDir::fromNativeSeparators(QString::fromWCharArray(path));
}

QString processImage(HANDLE process) {
    std::array<wchar_t, 32768> path{};
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    return QString::fromWCharArray(path.data(), size);
}

QString systemDirectory() {
    std::array<wchar_t, MAX_PATH> path{};
    const auto size = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    return size && size < path.size() ? QString::fromWCharArray(path.data(), size) : QString{};
}

std::wstring widePath(const QString& path) { return QDir::toNativeSeparators(path).toStdWString(); }

// Encode one Windows argv element for CreateProcessW, including literal quotes
// and runs of backslashes. The command is the last element and goes to SSH verbatim.
std::wstring quote(const QString& value) {
    std::wstring quoted(1, L'"');
    size_t slashes = 0;
    for (const auto ch : value.toStdWString()) {
        if (ch == L'\\') {
            ++slashes;
        } else {
            quoted.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            quoted += ch;
            slashes = 0;
        }
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L'"';
}

bool removeFile(const QString& file) {
    return DeleteFileW(widePath(file).c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool eraseStage(const QString& path) {
    return removeFile(path + "/identity") && removeFile(path + "/diagnostic") &&
        RemoveDirectoryW(widePath(path).c_str());
}

QString stagePrefix(DWORD session) { return QStringLiteral("TokenBucket-ssh-%1-").arg(session); }

void cleanAbandonedStages(const QDir& temp, const Endpoint& endpoint) {
    const auto prefix = stagePrefix(endpoint.session);
    const auto candidates = temp.entryList({prefix + '*'}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const auto& candidate : candidates) {
        const auto suffix = QStringView(candidate).sliced(prefix.size());
        const auto separator = suffix.indexOf('-');
        if (separator < 1 || QUuid(suffix.sliced(separator + 1)).isNull()) continue;
        bool valid = false;
        const DWORD pid = suffix.first(separator).toUInt(&valid);
        if (!valid || !pid) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (process || GetLastError() != ERROR_INVALID_PARAMETER) continue;
        const auto path = temp.filePath(candidate);
        const auto wide = widePath(path);
        const auto attributes = GetFileAttributesW(wide.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const auto status = GetNamedSecurityInfoW(wide.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
            &owner, nullptr, nullptr, nullptr, &descriptor);
        const std::unique_ptr<void, decltype(&freeLocal)> security(descriptor, freeLocal);
        LPWSTR sid = nullptr;
        if (status != ERROR_SUCCESS || !owner || !ConvertSidToStringSidW(owner, &sid)) continue;
        const std::unique_ptr<void, decltype(&freeLocal)> ownerSid(sid, freeLocal);
        if (endpoint.sid == QString::fromWCharArray(sid)) eraseStage(path);
    }
}

// The folder and identity both receive their protected DACL at creation.
class Stage {
public:
    Stage(const Endpoint& endpoint, const QString& local) {
        if (endpoint.sid.isEmpty() || local.isEmpty() || GetDriveTypeW(widePath(local.left(3)).c_str()) != DRIVE_FIXED)
            return;
        const QDir temp(QDir(local).filePath("Temp"));
        if (!temp.exists()) return;
        cleanAbandonedStages(temp, endpoint);
        const auto path = temp.filePath(stagePrefix(endpoint.session) + QString::number(GetCurrentProcessId()) + '-' +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto sddl = QStringLiteral("O:%1D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;%1)").arg(endpoint.sid).toStdWString();
        PSECURITY_DESCRIPTOR raw = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) return;
        std::unique_ptr<void, decltype(&freeLocal)> descriptor(raw, freeLocal);
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
        if (CreateDirectoryW(widePath(path).c_str(), &attributes)) path_ = path;
    }

    ~Stage() { if (!path_.isEmpty()) eraseStage(path_); }
    QString key() const { return path_ + "/identity"; }
    QString log() const { return path_ + "/diagnostic"; }

    bool write(QByteArrayView keyBytes) const {
        if (path_.isEmpty() || keyBytes.isEmpty()) return false;
        Handle file(CreateFileW(widePath(key()).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        DWORD written = 0;
        return file.get() != INVALID_HANDLE_VALUE &&
            WriteFile(file.get(), keyBytes.data(), static_cast<DWORD>(keyBytes.size()), &written, nullptr) &&
            written == keyBytes.size();
    }

    bool clean() {
        if (path_.isEmpty()) return true;
        for (int attempt = 0; attempt != 8; ++attempt) {
            if (eraseStage(path_)) { path_.clear(); return true; }
            Sleep(25);
        }
        return false;
    }

    void redact(QByteArray& data) const {
        data.replace(QDir::toNativeSeparators(path_).toUtf8(), QByteArray("[temporary SSH directory]"));
        data.replace(path_.toUtf8(), QByteArray("[temporary SSH directory]"));
    }

private:
    QString path_;
};

DWORD parentPid(DWORD child) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL found = Process32FirstW(snapshot.get(), &entry); found; found = Process32NextW(snapshot.get(), &entry))
        if (entry.th32ProcessID == child) return entry.th32ParentProcessID;
    return 0;
}

// One-use, SID/session-bound IPC. The peer must be the designated image in a
// live child of the exact SSH process we started. MCP never sees this channel.
class Askpass {
public:
    Askpass(const Endpoint& endpoint, SecureBytes passphrase)
        : endpoint_(endpoint), passphrase_(std::move(passphrase)) {}

    bool start(const QString& image) {
        image_ = image;
        suffix_ = QString(AskpassPrefix) + QUuid::createUuid().toString(QUuid::WithoutBraces);
        pipe_.reset(openServer(endpoint_, suffix_));
        event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (image_.isEmpty() || pipe_.get() == INVALID_HANDLE_VALUE || !event_) return false;
        connection_.hEvent = event_.get();
        const bool ready = ConnectNamedPipe(pipe_.get(), &connection_);
        if (ready) { connected_ = true; return true; }
        const auto status = GetLastError();
        if (status == ERROR_PIPE_CONNECTED) { connected_ = true; return true; }
        pending_ = status == ERROR_IO_PENDING;
        return pending_;
    }

    ~Askpass() {
        if (pending_) {
            CancelIoEx(pipe_.get(), &connection_);
            DWORD ignored = 0;
            GetOverlappedResult(pipe_.get(), &connection_, &ignored, TRUE);
        }
    }

    bool required() const { return !passphrase_.isEmpty(); }
    const QString& suffix() const { return suffix_; }
    bool sent() const { return sent_; }

    void service(HANDLE ssh, DWORD sshPid) {
        if (!required()) return;
        if (pending_) {
            if (WaitForSingleObject(event_.get(), 0) != WAIT_OBJECT_0) return;
            DWORD ignored = 0;
            connected_ = GetOverlappedResult(pipe_.get(), &connection_, &ignored, FALSE);
            pending_ = false;
        }
        if (!connected_) return;
        connected_ = false;
        ULONG pid = 0, session = 0;
        if (!GetNamedPipeClientSessionId(pipe_.get(), &session) || session != endpoint_.session ||
            !GetNamedPipeClientProcessId(pipe_.get(), &pid) || !pid ||
            WaitForSingleObject(ssh, 0) != WAIT_TIMEOUT || parentPid(pid) != sshPid) {
            pipe_.reset();
            passphrase_.clear();
            return;
        }
        Handle peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!peer || processImage(peer.get()).compare(image_, Qt::CaseInsensitive) != 0) {
            pipe_.reset();
            passphrase_.clear();
            return;
        }
        OVERLAPPED write{};
        write.hEvent = event_.get();
        ResetEvent(event_.get());
        DWORD count = 0;
        BOOL complete = WriteFile(pipe_.get(), passphrase_.bytes().constData(),
            static_cast<DWORD>(passphrase_.size()), &count, &write);
        if (!complete && GetLastError() == ERROR_IO_PENDING) {
            // This first, small write fits the pipe buffer. Fail closed if the
            // receiver is no longer present instead of waiting for it to read.
            CancelIoEx(pipe_.get(), &write);
            complete = GetOverlappedResult(pipe_.get(), &write, &count, TRUE);
        }
        sent_ = complete && count == passphrase_.size();
        passphrase_.clear();
        if (!sent_) pipe_.reset();
    }

private:
    Endpoint endpoint_;
    SecureBytes passphrase_;
    QString suffix_, image_;
    Handle pipe_, event_;
    OVERLAPPED connection_{};
    bool pending_ = false, connected_ = false, sent_ = false;
};

bool makeOutput(Handle& read, Handle& write, SECURITY_ATTRIBUTES& inheritable) {
    HANDLE rawRead = nullptr, rawWrite = nullptr;
    if (!CreatePipe(&rawRead, &rawWrite, &inheritable, 0)) return false;
    read.reset(rawRead);
    write.reset(rawWrite);
    return SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0);
}

bool drain(HANDLE pipe, QByteArray& output, qsizetype limit) {
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD available = 0, count = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available) return true;
        if (output.size() + available > limit) return false;
        if (!ReadFile(pipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr) ||
            !count) return true;
        output.append(buffer.data(), count);
    }
}

SshResult runProcess(QString program, const QString& system, const QString& profile, const QString& windows,
                     const Stage& stage, const model::SshTarget& target, const SshCommand& command,
                     Askpass& askpass, std::stop_token stop) {
    const bool protectedKey = askpass.required();
    const auto image = processImage(GetCurrentProcess());
    if (protectedKey && !askpass.start(image)) return {"ssh_askpass_failed"};
    QStringList environment{QStringLiteral("Path=") + system, QStringLiteral("SystemRoot=") + windows,
        QStringLiteral("USERPROFILE=") + QDir::toNativeSeparators(profile),
        protectedKey ? QStringLiteral("SSH_ASKPASS_REQUIRE=force") : QStringLiteral("SSH_ASKPASS_REQUIRE=never")};
    if (protectedKey) {
        environment << QStringLiteral("SSH_ASKPASS=") + image << QStringLiteral("TOKENBUCKET_SSH_ASKPASS=") + askpass.suffix();
    }
    environment.sort(Qt::CaseInsensitive);
    std::wstring env;
    for (const auto& variable : environment) { env += variable.toStdWString(); env += L'\0'; }
    env += L'\0';

    const QStringList args{"-F", "none", "-T", "-E", QDir::toNativeSeparators(stage.log()),
        "-o", "LogLevel=ERROR", "-o", "IdentityFile=none", "-i", QDir::toNativeSeparators(stage.key()),
        "-o", "IdentityAgent=none", "-o", "IdentitiesOnly=yes", "-o", "CertificateFile=none",
        "-o", protectedKey ? "BatchMode=no" : "BatchMode=yes", "-o", "NumberOfPasswordPrompts=1",
        "-o", "PreferredAuthentications=publickey", "-o", "PasswordAuthentication=no",
        "-o", "KbdInteractiveAuthentication=no", "-o", "GSSAPIAuthentication=no",
        "-o", "StrictHostKeyChecking=yes", "-o", "UpdateHostKeys=no", "-o", "GlobalKnownHostsFile=none",
        "-o", "UserKnownHostsFile=~/.ssh/known_hosts", "-o", "ClearAllForwardings=yes",
        "-o", "ConnectTimeout=10", "-o", "ConnectionAttempts=1", "-l", target.username,
        "-p", QString::number(target.port), "--", target.host, command.command};
    auto commandLine = quote(program);
    for (const auto& arg : args) { commandLine += L' '; commandLine += quote(arg); }

    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    Handle outRead, outWrite, errRead, errWrite;
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE || !makeOutput(outRead, outWrite, inheritable) ||
        !makeOutput(errRead, errWrite, inheritable)) return {"ssh_launch_failed"};

    HANDLE standardHandles[] = {input.get(), outWrite.get(), errWrite.get()};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    QByteArray storage(static_cast<qsizetype>(size), Qt::Uninitialized);
    const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!size || !InitializeProcThreadAttributeList(list, 1, 0, &size)) return {"ssh_launch_failed"};
    const auto free = qScopeGuard([&] { DeleteProcThreadAttributeList(list); });
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, standardHandles, sizeof(standardHandles),
        nullptr, nullptr)) return {"ssh_launch_failed"};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = outWrite.get();
    startup.StartupInfo.hStdError = errWrite.get();
    startup.lpAttributeList = list;

    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return {"ssh_launch_failed"};
    if (stop.stop_requested()) return {"cancelled"};
    PROCESS_INFORMATION raw{};
    if (!CreateProcessW(widePath(program).c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        env.data(), system.toStdWString().c_str(), &startup.StartupInfo, &raw)) return {"ssh_launch_failed"};
    Handle process(raw.hProcess), thread(raw.hThread);
    outWrite.reset();
    errWrite.reset();
    if (!AssignProcessToJobObject(job.get(), process.get()) || ResumeThread(thread.get()) == DWORD(-1)) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 2000);
        return {"ssh_launch_failed"};
    }
    const auto terminate = qScopeGuard([&] {
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process.get(), 2000);
    });

    QDeadlineTimer deadline(command.timeoutSeconds * 1000, Qt::PreciseTimer);
    SshResult result;
    for (;;) {
        if (stop.stop_requested()) return {"cancelled"};
        if (deadline.hasExpired()) return {"ssh_timeout"};
        askpass.service(process.get(), raw.dwProcessId);
        const auto ready = WaitForSingleObject(process.get(), 15);
        if (!drain(outRead.get(), result.out, MaxSshOutputBytes - result.err.size()) ||
            !drain(errRead.get(), result.err, MaxSshOutputBytes - result.out.size())) return {"ssh_output_limit"};
        if (ready == WAIT_OBJECT_0) break;
        if (ready != WAIT_TIMEOUT) return {"ssh_launch_failed"};
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.get(), &exitCode)) return {"ssh_launch_failed"};
    if (exitCode == 255 || exitCode > 255) {
        QFile log(stage.log());
        const auto diagnostics = log.open(QIODevice::ReadOnly) ? log.read(16 * 1024) : QByteArray{};
        if (diagnostics.contains("Host key verification failed")) return {"ssh_host_key_failed"};
        if (diagnostics.contains("incorrect passphrase") || diagnostics.contains("bad passphrase"))
            return {"ssh_auth_failed"};
        if (diagnostics.contains("Permission denied") || diagnostics.contains("Load key"))
            return {protectedKey && !askpass.sent() ? "ssh_askpass_failed" : "ssh_auth_failed"};
        return {"ssh_connection_failed"};
    }
    result.exitCode = static_cast<int>(exitCode);
    stage.redact(result.out);
    stage.redact(result.err);
    return result;
}

SshResult execute(const model::SshTarget& target, const SshCommand& command, SecureBytes privateKey,
                  SecureBytes passphrase, const QString& testProgram, std::stop_token stop) {
    if (passphrase.size() > MaxPassphrase || passphrase.bytes().contains('\0') ||
        passphrase.bytes().contains('\r') || passphrase.bytes().contains('\n')) return {"ssh_passphrase_unsupported"};
    const auto system = systemDirectory();
    const auto profile = knownFolder(FOLDERID_Profile);
    const auto windows = knownFolder(FOLDERID_Windows);
    if (system.isEmpty() || profile.isEmpty() || windows.isEmpty()) return {"ssh_launch_failed"};
    const auto program = testProgram.isEmpty() ? system + QStringLiteral("\\OpenSSH\\ssh.exe") : testProgram;
    const auto endpoint = Endpoint::current();
    Stage stage(endpoint, knownFolder(FOLDERID_LocalAppData));
    if (stop.stop_requested()) return {"cancelled"};
    if (!stage.write(privateKey.bytes())) return stage.clean() ? SshResult{"ssh_key_staging_failed"} :
        SshResult{"ssh_cleanup_failed"};
    privateKey.clear();
    Askpass askpass(endpoint, std::move(passphrase));
    auto result = runProcess(program, system, profile, windows, stage, target, command, askpass, stop);
    return stage.clean() ? std::move(result) : SshResult{"ssh_cleanup_failed"};
}
}

SshExecution::SshExecution(const model::SshTarget& target, const SshCommand& command, QByteArrayView privateKey,
                           const QString& passphrase, const QString& testProgram)
    : worker_([this, target, command, key = SecureBytes(privateKey.toByteArray()),
               password = SecureBytes(passphrase.toUtf8()), testProgram](std::stop_token stop) mutable {
        try { result_ = execute(target, command, std::move(key), std::move(password), testProgram, stop); }
        catch (...) { result_ = {"ssh_launch_failed"}; }
        done_.store(true, std::memory_order_release);
    }) {}

bool isSshAskpass() { return GetEnvironmentVariableW(AskpassVariable, nullptr, 0) != 0; }

int runSshAskpass() {
    std::array<wchar_t, 80> suffix{};
    const auto length = GetEnvironmentVariableW(AskpassVariable, suffix.data(), static_cast<DWORD>(suffix.size()));
    if (!length || length >= suffix.size() || GetEnvironmentVariableW(L"SSH_ASKPASS_PROMPT", nullptr, 0)) return 1;
    const auto channel = QString::fromWCharArray(suffix.data(), length);
    if (!channel.startsWith(AskpassPrefix) || QUuid(QStringView(channel).sliced(QStringView(AskpassPrefix).size())).isNull())
        return 1;
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 1;
    const std::unique_ptr<void, decltype(&freeLocal)> free(args, freeLocal);
    if (count != 2) return 1;
    const auto prompt = QString::fromWCharArray(args[1]);
    if (!prompt.startsWith(QStringLiteral("Enter passphrase for key '")) ||
        !(prompt.endsWith(QStringLiteral("/identity': ")) || prompt.endsWith(QStringLiteral("\\identity': ")))) return 1;
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || GetFileType(output) != FILE_TYPE_PIPE) return 1;
    Handle pipe(openClient(Endpoint::current(), channel));
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE || !event) return 1;
    std::array<char, MaxPassphrase> password{};
    const auto wipe = qScopeGuard([&] { SecureZeroMemory(password.data(), password.size()); });
    OVERLAPPED read{};
    read.hEvent = event.get();
    DWORD bytes = 0;
    BOOL ready = ReadFile(pipe.get(), password.data(), static_cast<DWORD>(password.size()), &bytes, &read);
    if (!ready && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(event.get(), 3000) != WAIT_OBJECT_0) CancelIoEx(pipe.get(), &read);
        ready = GetOverlappedResult(pipe.get(), &read, &bytes, TRUE);
    }
    if (!ready || !bytes) return 1;
    DWORD written = 0;
    return WriteFile(output, password.data(), bytes, &written, nullptr) && written == bytes ? 0 : 1;
}
}
