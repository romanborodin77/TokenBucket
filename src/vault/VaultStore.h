#pragma once
#include "../platform/FileOps.h"
#include <functional>
namespace tb::store {
struct RestoreResult {
    bool committed=false;
    QString safetyPath,error;
};
// Хранение файлов хранилища (main/.bak/.tmp) в каталоге данных. save() атомарен и
// поверх только ПОДТВЕРЖДЁННОГО содержимого: acceptVerified фиксирует байты,
// прошедшие AEAD и разбор при разблокировке; любое несовпадение на диске — отказ
// сохранения (второй экземпляр/ручная правка). loadBytes — строгая читалка
// (symlink, размер, полнота) для всех трёх ролей файлов.
class VaultStore {
public:
    explicit VaultStore(const QString& dir, FileOps& ops = realFileOps());
    QString mainPath() const;
    QString backupPath() const;
    QString tmpPath() const;
    // Вызывать только после проверки AEAD и разбора модели при разблокировке.
    void acceptVerified(const QByteArray& bytes) { verified_ = bytes; hasVerified_ = true; }
    bool save(const QByteArray& bytes, QString* err);
    RestoreResult restoreBackup(const QByteArray& expectedMain,const QByteArray& expectedBackup,
                                const std::function<bool()>& canceled);
    enum class Load { Ok, Missing, IoError };
    Load loadBytes(const QString& path, QByteArray* out) const;
private:
    QString dir_;
    FileOps& ops_;
    QByteArray verified_;   // последний подтверждённый образ основного файла (шифротекст)
    bool hasVerified_ = false;
};
}
