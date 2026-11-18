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
#include <algorithm>

namespace tb::agent {
namespace {
constexpr auto AskpassVariable = L"TOKENBUCKET_SSH_ASKPASS";
constexpr auto AskpassPrefix = u".askpass.";
constexpr DWORD MaxPassphrase = 1023; // OpenSSH's ASKPASS pipe buffer, excluding the terminal NUL.
constexpr DWORD MaxAskpassPrompt = 4096;
constexpr auto StagePrefix = "TokenBucket-ssh-";
constexpr auto Identity = "identity";

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

qulonglong creationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (qulonglong(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

bool privateAcl(HANDLE file, const Endpoint& endpoint) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    const auto status = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &raw);
    const std::unique_ptr<void, decltype(&freeLocal)> descriptor(raw, freeLocal);
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (status != ERROR_SUCCESS || !owner || !dacl || dacl->AceCount != 2 ||
        !GetSecurityDescriptorControl(raw, &control, &revision) || !(control & SE_DACL_PROTECTED)) return false;
    PSID rawExpected = nullptr;
    if (!ConvertStringSidToSidW(endpoint.sid.toStdWString().c_str(), &rawExpected)) return false;
    const std::unique_ptr<void, decltype(&freeLocal)> expected(rawExpected, freeLocal);
    if (!EqualSid(owner, expected.get())) return false;
    bool ownerAllowed = false, systemAllowed = false;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* rawAce = nullptr;
        if (!GetAce(dacl, i, &rawAce)) return false;
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
        auto* sid = const_cast<DWORD*>(&ace->SidStart);
        if (EqualSid(sid, expected.get())) ownerAllowed = true;
        else if (IsWellKnownSid(sid, WinLocalSystemSid)) systemAllowed = true;
        else return false;
    }
    return ownerAllowed && systemAllowed;
}

bool localAclVolume(HANDLE file) {
    std::array<wchar_t, 32768> resolved{};
    const auto length = GetFinalPathNameByHandleW(file, resolved.data(), static_cast<DWORD>(resolved.size()), VOLUME_NAME_DOS);
    DWORD flags = 0;
    if (length < 7 || length >= resolved.size() || resolved[5] != L':' || resolved[6] != L'\\' ||
        !GetVolumeInformationByHandleW(file, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) ||
        !(flags & FILE_PERSISTENT_ACLS)) return false;
    const wchar_t root[]{resolved[4], L':', L'\\', L'\0'};
    return GetDriveTypeW(root) == DRIVE_FIXED;
}

void cleanAbandonedStages(const QDir& temp, const Endpoint& endpoint) {
    const auto prefix = QString::fromLatin1(StagePrefix);
    const auto candidates = temp.entryList({prefix + '*'}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const auto& candidate : candidates) {
        const auto suffix = QStringView(candidate).sliced(prefix.size());
        const auto first = suffix.indexOf('-');
        const auto second = suffix.indexOf('-', first + 1);
        if (first < 1 || second <= first + 1 || QUuid(suffix.sliced(second + 1)).isNull()) continue;
        bool validPid = false, validTime = false;
        const DWORD pid = suffix.first(first).toUInt(&validPid);
        const auto created = suffix.sliced(first + 1, second - first - 1).toULongLong(&validTime);
        if (!validPid || !pid || !validTime || !created) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        if (process) {
            const auto actual = creationTime(process.get());
            if (!actual || (actual == created && WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0)) continue;
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) continue;
        const auto path = temp.filePath(candidate);
        Handle directory(CreateFileW(widePath(path).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (directory.get() == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &info, sizeof(info)) ||
            (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !localAclVolume(directory.get()) ||
            !privateAcl(directory.get(), endpoint)) continue;
        eraseStage(path);
    }
}

// The folder and identity both receive their protected DACL at creation.
class Stage {
public:
    Stage(Endpoint endpoint, const QString& local) : endpoint_(std::move(endpoint)) {
        if (endpoint_.sid.isEmpty() || local.isEmpty()) return;
        const QDir temp(QDir(local).filePath("Temp"));
        const auto created = creationTime(GetCurrentProcess());
        if (!temp.exists() || !created) return;
        const auto path = temp.filePath(QStringLiteral("%1%2-%3-%4").arg(QString::fromLatin1(StagePrefix),
            QString::number(GetCurrentProcessId()), QString::number(created), QUuid::createUuid().toString(QUuid::WithoutBraces)));
        const auto sddl = QStringLiteral("O:%1D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;%1)").arg(endpoint_.sid).toStdWString();
        PSECURITY_DESCRIPTOR raw = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) return;
        descriptor_.reset(raw);
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor_.get(), FALSE};
        if (CreateDirectoryW(widePath(path).c_str(), &attributes)) path_ = path;
    }

    ~Stage() { if (!path_.isEmpty()) eraseStage(path_); }
    const QString& path() const { return path_; }
    QString key() const { return path_ + '/' + Identity; }
    QString log() const { return path_ + "/diagnostic"; }

    bool write(QByteArrayView keyBytes) const {
        if (path_.isEmpty() || keyBytes.isEmpty()) return false;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor_.get(), FALSE};
        Handle directory(CreateFileW(widePath(path_).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (directory.get() == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &info, sizeof(info)) ||
            (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !localAclVolume(directory.get()) ||
            !privateAcl(directory.get(), endpoint_)) return false;
        Handle file(CreateFileW(widePath(key()).c_str(), GENERIC_WRITE | READ_CONTROL | FILE_READ_ATTRIBUTES, 0,
            &attributes, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        DWORD written = 0;
        return file.get() != INVALID_HANDLE_VALUE && localAclVolume(file.get()) && privateAcl(file.get(), endpoint_) &&
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
    Endpoint endpoint_;
    std::unique_ptr<void, decltype(&freeLocal)> descriptor_{nullptr, freeLocal};
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

BOOL finishIo(HANDLE file, OVERLAPPED& operation, DWORD& count, BOOL ready, DWORD timeout) {
    if (ready || GetLastError() != ERROR_IO_PENDING) return ready;
    if (WaitForSingleObject(operation.hEvent, timeout) != WAIT_OBJECT_0) CancelIoEx(file, &operation);
    return GetOverlappedResult(file, &operation, &count, TRUE);
}

// One-use, SID/session-bound IPC. The peer must be the designated image in a
// live child of the exact SSH process we started. MCP never sees this channel.
class Askpass {
public:
    Askpass(const Endpoint& endpoint, SecureBytes passphrase)
        : endpoint_(endpoint), passphrase_(std::move(passphrase)) {}

    bool start(const QString& image, const QString& key) {
        image_ = image;
        key_ = key;
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

    void service(HANDLE ssh, DWORD sshPid, HANDLE job) {
        if (!required()) return;
        if (pending_) {
            if (WaitForSingleObject(event_.get(), 0) != WAIT_OBJECT_0) return;
            DWORD ignored = 0;
            connected_ = GetOverlappedResult(pipe_.get(), &connection_, &ignored, FALSE);
            pending_ = false;
        }
        if (!connected_) return;
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_.get(), nullptr, 0, nullptr, &available, nullptr) || available > MaxAskpassPrompt) {
            reject();
            return;
        }
        if (!available) return;
        ULONG pid = 0, session = 0;
        if (!GetNamedPipeClientSessionId(pipe_.get(), &session) || session != endpoint_.session ||
            !GetNamedPipeClientProcessId(pipe_.get(), &pid) || !pid ||
            WaitForSingleObject(ssh, 0) != WAIT_TIMEOUT) {
            reject();
            return;
        }
        Handle peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        BOOL inJob = FALSE;
        if (!peer || WaitForSingleObject(peer.get(), 0) != WAIT_TIMEOUT ||
            !IsProcessInJob(peer.get(), job, &inJob) || !inJob || parentPid(pid) != sshPid ||
            processImage(peer.get()).compare(image_, Qt::CaseInsensitive) != 0) {
            reject();
            return;
        }
        std::array<char, MaxAskpassPrompt> prompt{};
        OVERLAPPED read{};
        read.hEvent = event_.get();
        ResetEvent(event_.get());
        DWORD count = 0;
        const BOOL ready = ReadFile(pipe_.get(), prompt.data(), static_cast<DWORD>(prompt.size()), &count, &read);
        if (!finishIo(pipe_.get(), read, count, ready, 0) ||
            QDir::fromNativeSeparators(QString::fromUtf8(prompt.data(), count)).compare(key_, Qt::CaseInsensitive) != 0) {
            reject();
            return;
        }
        OVERLAPPED write{};
        write.hEvent = event_.get();
        ResetEvent(event_.get());
        count = 0;
        const BOOL written = WriteFile(pipe_.get(), passphrase_.bytes().constData(),
            static_cast<DWORD>(passphrase_.size()), &count, &write);
        sent_ = finishIo(pipe_.get(), write, count, written, 0) && count == passphrase_.size();
        connected_ = false;
        passphrase_.clear();
        if (!sent_) pipe_.reset();
    }

private:
    void reject() {
        connected_ = false;
        pipe_.reset();
        passphrase_.clear();
    }

    Endpoint endpoint_;
    SecureBytes passphrase_;
    QString suffix_, image_, key_;
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

SshResult runProcess(const QString& program, const QString& system, const QString& profile, const QString& windows,
                     const QString& programData,
                     const QString& testKnownHosts,
                     const Stage& stage, const model::SshTarget& target, const SshCommand& command,
                     Askpass& askpass, std::stop_token stop) {
    const bool protectedKey = askpass.required();
    const auto image = processImage(GetCurrentProcess());
    if (protectedKey && !askpass.start(image, QString::fromLatin1(Identity))) return {"ssh_askpass_failed"};
    QStringList environment{QStringLiteral("Path=") + system, QStringLiteral("SystemRoot=") + windows,
        QStringLiteral("ProgramData=") + QDir::toNativeSeparators(programData),
        QStringLiteral("USERPROFILE=") + QDir::toNativeSeparators(profile),
        protectedKey ? QStringLiteral("SSH_ASKPASS_REQUIRE=force") : QStringLiteral("SSH_ASKPASS_REQUIRE=never")};
    if (protectedKey) {
        environment << QStringLiteral("SSH_ASKPASS=") + image << QStringLiteral("TOKENBUCKET_SSH_ASKPASS=") + askpass.suffix();
    }
    environment.sort(Qt::CaseInsensitive);
    std::wstring env;
    for (const auto& variable : environment) { env += variable.toStdWString(); env += L'\0'; }
    env += L'\0';

    const auto hostsOption = testKnownHosts.isEmpty() ? QStringLiteral("UserKnownHostsFile=~/.ssh/known_hosts") :
        QStringLiteral("UserKnownHostsFile=\"%1\"").arg(QDir::fromNativeSeparators(testKnownHosts));
    // OpenSSH truncates the askpass prompt's key path to 100 bytes. A fixed
    // relative identity in our private working directory also works on long profiles.
    const QStringList args{"-F", "none", "-T", "-E", QDir::toNativeSeparators(stage.log()),
        "-o", "LogLevel=ERROR", "-o", "IdentityFile=none", "-i", Identity,
        "-o", "IdentityAgent=none", "-o", "IdentitiesOnly=yes", "-o", "CertificateFile=none",
        "-o", protectedKey ? "BatchMode=no" : "BatchMode=yes", "-o", "NumberOfPasswordPrompts=1",
        "-o", "PreferredAuthentications=publickey", "-o", "PasswordAuthentication=no",
        "-o", "KbdInteractiveAuthentication=no", "-o", "GSSAPIAuthentication=no",
        "-o", "StrictHostKeyChecking=yes", "-o", "UpdateHostKeys=no", "-o", "GlobalKnownHostsFile=none",
        "-o", hostsOption, "-o", "ClearAllForwardings=yes",
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
        env.data(), widePath(stage.path()).c_str(), &startup.StartupInfo, &raw)) return {"ssh_launch_failed"};
    Handle process(raw.hProcess), thread(raw.hThread);
    outWrite.reset();
    errWrite.reset();
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 2000);
        return {"ssh_launch_failed"};
    }
    const auto terminate = qScopeGuard([&] {
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process.get(), 2000);
    });
    if (stop.stop_requested()) return {"cancelled"};
    if (ResumeThread(thread.get()) == DWORD(-1)) return {"ssh_launch_failed"};

    QDeadlineTimer deadline(command.timeoutSeconds * 1000, Qt::PreciseTimer);
    SshResult result;
    for (;;) {
        if (stop.stop_requested()) return {"cancelled"};
        if (deadline.hasExpired()) return {"ssh_timeout"};
        askpass.service(process.get(), raw.dwProcessId, job.get());
        const auto ready = WaitForSingleObject(process.get(), 15);
        if (!drain(outRead.get(), result.out, MaxSshOutputBytes - result.err.size()) ||
            !drain(errRead.get(), result.err, MaxSshOutputBytes - result.out.size())) return {"ssh_output_limit"};
        if (ready == WAIT_OBJECT_0) break;
        if (ready != WAIT_TIMEOUT) return {"ssh_launch_failed"};
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.get(), &exitCode)) return {"ssh_launch_failed"};
    if (exitCode > 255) return {"ssh_launch_failed"};
    if (exitCode == 255) {
        QFile log(stage.log());
        const auto diagnostics = log.open(QIODevice::ReadOnly) ? log.read(16 * 1024) : QByteArray{};
        if (diagnostics.contains("Host key verification failed")) return {"ssh_host_key_failed"};
        if (diagnostics.contains("incorrect passphrase") || diagnostics.contains("bad passphrase"))
            return {"ssh_auth_failed"};
        if (diagnostics.contains("Permission denied") || diagnostics.contains("Load key"))
            return {protectedKey && !askpass.sent() ? "ssh_askpass_failed" : "ssh_auth_failed"};
        if (!diagnostics.isEmpty()) return {"ssh_connection_failed"};
        result.error = "ssh_status_unknown";
    }
    result.exitCode = static_cast<int>(exitCode);
    stage.redact(result.out);
    stage.redact(result.err);
    return result;
}

SshResult execute(const model::SshTarget& target, const SshCommand& command, SecureBytes privateKey,
                  SecureBytes passphrase, const QString& testProgram, const QString& testKnownHosts, std::stop_token stop) {
    if (passphrase.size() > MaxPassphrase || passphrase.bytes().contains('\0') ||
        passphrase.bytes().contains('\r') || passphrase.bytes().contains('\n')) return {"ssh_passphrase_unsupported"};
    const auto system = systemDirectory();
    const auto profile = knownFolder(FOLDERID_Profile);
    const auto windows = knownFolder(FOLDERID_Windows);
    // Win32 OpenSSH exits before opening its log if ProgramData is absent,
    // even with configuration files disabled. Resolve the OS folder directly.
    const auto programData = knownFolder(FOLDERID_ProgramData);
    if (system.isEmpty() || profile.isEmpty() || windows.isEmpty() || programData.isEmpty()) return {"ssh_launch_failed"};
    const auto program = testProgram.isEmpty() ? system + QStringLiteral("\\OpenSSH\\ssh.exe") : testProgram;
    const auto endpoint = Endpoint::current();
    Stage stage(endpoint, knownFolder(FOLDERID_LocalAppData));
    if (stop.stop_requested()) return {"cancelled"};
    if (!stage.write(privateKey.bytes())) return stage.clean() ? SshResult{"ssh_key_staging_failed"} :
        SshResult{"ssh_cleanup_failed"};
    privateKey.clear();
    Askpass askpass(endpoint, std::move(passphrase));
    auto result = runProcess(program, system, profile, windows, programData, testKnownHosts, stage, target, command, askpass, stop);
    return stage.clean() ? std::move(result) : SshResult{"ssh_cleanup_failed"};
}
}

SshExecution::SshExecution(const model::SshTarget& target, const SshCommand& command, QByteArrayView privateKey,
                           const QString& passphrase, const QString& testProgram, const QString& testKnownHosts)
    : worker_([this, target, command, key = SecureBytes(privateKey.toByteArray()),
               password = SecureBytes(passphrase.toUtf8()), testProgram, testKnownHosts](std::stop_token stop) mutable {
        try { result_ = execute(target, command, std::move(key), std::move(password), testProgram, testKnownHosts, stop); }
        catch (...) { result_ = {"ssh_launch_failed"}; }
        done_.store(true, std::memory_order_release);
    }) {}

bool isSshAskpass() { return GetEnvironmentVariableW(AskpassVariable, nullptr, 0) != 0; }

void cleanAbandonedSshStages() {
    try {
        const auto local = knownFolder(FOLDERID_LocalAppData);
        const auto endpoint = Endpoint::current();
        if (!local.isEmpty() && !endpoint.sid.isEmpty()) cleanAbandonedStages(QDir(QDir(local).filePath("Temp")), endpoint);
    } catch (...) {
        // Startup recovery is best effort. A failed deletion retains its private ACL.
    }
}

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
    const auto intro = QStringView(u"Enter passphrase for key '");
    const auto end = QStringView(u"': ");
    if (!prompt.startsWith(intro) || !prompt.endsWith(end) || prompt.size() <= intro.size() + end.size()) return 1;
    const auto key = QStringView(prompt).sliced(intro.size(), prompt.size() - intro.size() - end.size()).toUtf8();
    if (key.size() > MaxAskpassPrompt) return 1;
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || GetFileType(output) != FILE_TYPE_PIPE) return 1;
    Handle pipe(openClient(Endpoint::current(), channel));
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE || !event) return 1;
    OVERLAPPED write{};
    write.hEvent = event.get();
    DWORD bytes = 0;
    const BOOL writtenKey = WriteFile(pipe.get(), key.constData(), static_cast<DWORD>(key.size()), &bytes, &write);
    if (!finishIo(pipe.get(), write, bytes, writtenKey, 3000) || bytes != key.size()) return 1;
    std::array<char, MaxPassphrase> password{};
    const auto wipe = qScopeGuard([&] { SecureZeroMemory(password.data(), password.size()); });
    OVERLAPPED read{};
    read.hEvent = event.get();
    ResetEvent(event.get());
    bytes = 0;
    const BOOL ready = ReadFile(pipe.get(), password.data(), static_cast<DWORD>(password.size()), &bytes, &read);
    if (!finishIo(pipe.get(), read, bytes, ready, 3000) || !bytes) return 1;
    DWORD written = 0;
    return WriteFile(output, password.data(), bytes, &written, nullptr) && written == bytes ? 0 : 1;
}
}
