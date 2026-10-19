#pragma once
#include "../platform/FileOps.h"
namespace tb::store {
class VaultStore {
public:
    explicit VaultStore(const QString& dir, FileOps& ops = realFileOps());
    QString mainPath() const;
    QString backupPath() const;
    QString tmpPath() const;
    // Вызывать только после проверки AEAD и разбора модели при разблокировке.
    void acceptVerified(const QByteArray& bytes) { verified_ = bytes; hasVerified_ = true; }
    bool save(const QByteArray& bytes, QString* err);
    enum class Load { Ok, Missing, IoError };
    Load loadBytes(const QString& path, QByteArray* out) const;
private:
    QString dir_;
    FileOps& ops_;
    QByteArray verified_;
    bool hasVerified_ = false;
};
}
