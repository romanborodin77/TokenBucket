#pragma once
#include <QByteArray>
#include <QString>
namespace tb::store {
struct FileOps {
    virtual ~FileOps() = default;
    virtual bool writeAll(const QString& path, const QByteArray& bytes) = 0;
    virtual bool renameReplace(const QString& from, const QString& to) = 0;
    virtual bool removeFile(const QString& path) = 0;
};
FileOps& realFileOps();
bool ensurePrivateDir(const QString& dir);
}
