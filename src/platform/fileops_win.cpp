#include "FileOps.h"
#include <QDir>
#include <QFileInfo>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <vector>
namespace tb::store {
namespace {
struct PrivateSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PrivateSecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
        DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> data(size);
        const bool ok = GetTokenInformation(token, TokenUser, data.data(), size, &size);
        CloseHandle(token);
        if (!ok) return;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid)) return;
        const QString sddl = QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%1)").arg(QString::fromWCharArray(sid));
        LocalFree(sid);
        ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),
            SDDL_REVISION_1, &descriptor, nullptr);
    }
    ~PrivateSecurity() { if (descriptor) LocalFree(descriptor); }
};
bool reparsePoint(const QString& path) {
    const DWORD attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
}
struct RealOps : FileOps {
    bool writeAll(const QString& path, const QByteArray& bytes) override {
        PrivateSecurity security;
        if (!security.descriptor || bytes.size() > MAXDWORD) return false;
        SECURITY_ATTRIBUTES sa{sizeof(sa), security.descriptor, FALSE};
        HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE, 0, &sa,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(file, bytes.constData(), DWORD(bytes.size()), &written, nullptr)
                  && written == DWORD(bytes.size()) && FlushFileBuffers(file);
        if (!CloseHandle(file)) ok = false;
        return ok;
    }
    bool renameReplace(const QString& from, const QString& to) override {
        if (reparsePoint(from) || reparsePoint(to)) return false;
        return MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    bool removeFile(const QString& path) override { return DeleteFileW(reinterpret_cast<LPCWSTR>(path.utf16())); }
};
}
bool ensurePrivateDir(const QString& dir) {
    if (reparsePoint(dir) || !QDir().mkpath(QFileInfo(dir).absolutePath())) return false;
    PrivateSecurity security;
    if (!security.descriptor) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), security.descriptor, FALSE};
    if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(dir.utf16()), &sa) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    if (!QFileInfo(dir).isDir()) return false;
    BOOL present = FALSE, defaulted = FALSE; PACL acl = nullptr;
    if (!GetSecurityDescriptorDacl(security.descriptor, &present, &acl, &defaulted) || !present || !acl) return false;
    return SetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(dir.utf16())), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
}
FileOps& realFileOps() { static RealOps ops; return ops; }
}
