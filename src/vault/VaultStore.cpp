#include "VaultStore.h"
#include "Container.h"
#include <QFile>
#include <QFileInfo>
namespace tb::store {
VaultStore::VaultStore(const QString& dir, FileOps& ops) : dir_(dir), ops_(ops) {}
QString VaultStore::mainPath() const { return dir_ + "/vault.tb"; }
QString VaultStore::backupPath() const { return dir_ + "/vault.tb.bak"; }
QString VaultStore::tmpPath() const { return dir_ + "/vault.tb.tmp"; }
VaultStore::Load VaultStore::loadBytes(const QString& path, QByteArray* out) const {
    if (!out) return Load::IoError;
    out->clear();
    const QFileInfo info(path);
    if (info.isSymLink()) return Load::IoError;
    if (!info.exists()) return Load::Missing;
    if (!info.isFile() || info.size() > container::MaxFileBytes) return Load::IoError;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Load::IoError;
    QByteArray bytes = file.read(container::MaxFileBytes + 1);
    if (file.error() != QFile::NoError || !file.atEnd() || bytes.size() > container::MaxFileBytes) return Load::IoError;
    *out = std::move(bytes);
    return Load::Ok;
}
bool VaultStore::save(const QByteArray& bytes, QString* err) {
    if (err) err->clear();
    const QString backupTmp = backupPath() + ".tmp";
    const auto fail = [&](const char* message) {
        ops_.removeFile(tmpPath()); ops_.removeFile(backupTmp);
        if (err) *err = QString::fromUtf8(message);
        return false;
    };
    if (bytes.isEmpty() || bytes.size() > container::MaxFileBytes) return fail("Недопустимый размер файла хранилища.");
    if (!ensurePrivateDir(dir_)) return fail("Не удалось ограничить доступ к каталогу.");
    QByteArray previous;
    const Load loaded = loadBytes(mainPath(), &previous);
    if (loaded == Load::IoError) return fail("Не удалось прочитать основной файл.");
    if (loaded == Load::Ok && (!hasVerified_ || previous != verified_))
        return fail("Основной файл изменился или не был проверен. Откройте хранилище заново.");
    if (loaded == Load::Missing && hasVerified_) return fail("Основной файл исчез. Сохранение отменено.");
    ops_.removeFile(tmpPath()); ops_.removeFile(backupTmp);
    if (!ops_.writeAll(tmpPath(), bytes)) return fail("Не удалось записать временный файл.");
    QByteArray check;
    if (loadBytes(tmpPath(), &check) != Load::Ok || check != bytes) return fail("Проверка временного файла не прошла.");
    if (loaded == Load::Ok) {
        if (!ops_.writeAll(backupTmp, previous)
            || loadBytes(backupTmp, &check) != Load::Ok || check != previous
            || !ops_.renameReplace(backupTmp, backupPath())) return fail("Не удалось сохранить резервную копию.");
    }
    // Основной файл не переносится: отказ последней замены не требует отката.
    if (!ops_.renameReplace(tmpPath(), mainPath())) return fail("Не удалось заменить файл хранилища.");
    acceptVerified(bytes);
    return true;
}
}
