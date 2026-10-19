#include "FileOps.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace tb::store {
namespace {
struct RealOps : FileOps {
    bool writeAll(const QString& path, const QByteArray& bytes) override {
        const auto name = QFile::encodeName(path);
        const int fd = ::open(name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        qsizetype offset = 0; bool ok = ::fchmod(fd, 0600) == 0;
        while (ok && offset < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.constData() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            offset += n;
        }
        if (ok) ok = ::fsync(fd) == 0;
        if (::close(fd) != 0) ok = false;
        return ok;
    }
    bool renameReplace(const QString& from, const QString& to) override {
        const auto parent = QFile::encodeName(QFileInfo(to).absolutePath());
        const int dir = ::open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) return false;
        bool ok = ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
        if (ok) ok = ::fsync(dir) == 0;
        if (::close(dir) != 0) ok = false;
        return ok;
    }
    bool removeFile(const QString& path) override { return ::unlink(QFile::encodeName(path).constData()) == 0; }
};
}
bool ensurePrivateDir(const QString& dir) {
    if (!QDir().mkpath(QFileInfo(dir).absolutePath())) return false;
    const auto name = QFile::encodeName(dir);
    if (::mkdir(name.constData(), 0700) != 0 && errno != EEXIST) return false;
    const int fd = ::open(name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0 && st.st_uid == ::geteuid() && ::fchmod(fd, 0700) == 0;
    if (::close(fd) != 0) ok = false;
    return ok;
}
FileOps& realFileOps() { static RealOps ops; return ops; }
}
