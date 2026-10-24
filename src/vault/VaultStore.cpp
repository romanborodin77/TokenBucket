#include "VaultStore.h"
#include "Container.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QUuid>
namespace tb::store {
VaultStore::VaultStore(const QString& dir, FileOps& ops) : dir_(dir), ops_(ops) {}
QString VaultStore::mainPath() const { return dir_ + "/vault.tb"; }
QString VaultStore::backupPath() const { return dir_ + "/vault.tb.bak"; }
QString VaultStore::tmpPath() const { return dir_ + "/vault.tb.tmp"; }
RestoreResult VaultStore::restoreBackup(const QByteArray& expectedMain,const QByteArray& expectedBackup,
                                        const std::function<bool()>& canceled) {
    RestoreResult result; QString temporary; bool temporaryOwned=false;
    const auto fail=[&](const char* message) {
        if(temporaryOwned) ops_.removeFile(temporary);
        result.error=QString::fromUtf8(message); return result;
    };
    const auto isCanceled=[&] {return canceled && canceled();};
    const auto matches=[&](const QString& path,const QByteArray& expected) {
        QByteArray bytes; return loadBytes(path,&bytes)==Load::Ok && bytes==expected;
    };
    if(expectedMain.isEmpty() || expectedMain.size()>container::MaxFileBytes
        || expectedBackup.isEmpty() || expectedBackup.size()>container::MaxFileBytes)
        return fail("Недопустимый размер файла восстановления.");
    if(isCanceled()) return fail("Восстановление отменено.");
    if(!ensurePrivateDir(dir_)) return fail("Не удалось ограничить доступ к каталогу.");
    if(!hasVerified_ || verified_!=expectedMain || !matches(mainPath(),expectedMain))
        return fail("Основной файл изменился или не был проверен. Откройте хранилище заново.");
    if(!matches(backupPath(),expectedBackup)) return fail("Резервная копия изменилась или недоступна.");
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto safety=mainPath()+QStringLiteral(".before-restore-%1-%2")
        .arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmsszzz"),id);
    if(!ops_.writeAll(safety,expectedMain)) return fail("Не удалось создать страховочную копию.");
    if(!matches(safety,expectedMain)) return fail("Проверка страховочной копии не прошла.");
    result.safetyPath=safety;
    if(isCanceled()) return fail("Восстановление отменено; страховочная копия сохранена.");
    temporary=mainPath()+QStringLiteral(".restore-%1.tmp").arg(id);
    // Ошибка CREATE_NEW не подтверждает владение путём: возможную коллизию не удаляем.
    if(!ops_.writeAll(temporary,expectedBackup)) return fail("Не удалось записать временный файл восстановления.");
    temporaryOwned=true;
    if(!matches(temporary,expectedBackup)) return fail("Проверка временного файла восстановления не прошла.");
    if(!matches(mainPath(),expectedMain) || !matches(backupPath(),expectedBackup))
        return fail("Файлы изменились. Восстановление отменено.");
    if(isCanceled()) return fail("Восстановление отменено; страховочная копия сохранена.");
    if(!ops_.renameReplace(temporary,mainPath())) return fail("Не удалось заменить файл хранилища.");
    temporaryOwned=false; result.committed=true;
    verified_.clear(); hasVerified_=false;
    try {
        if(!matches(mainPath(),expectedBackup)) result.error=QStringLiteral("Файл заменён, но итоговая проверка не прошла. Откройте хранилище заново.");
        else if(isCanceled()) result.error=QStringLiteral("Резервная копия восстановлена. Во время замены операция была отменена; сеанс остаётся заблокированным.");
    } catch(...) {result.error=QStringLiteral("Файл заменён, но итоговую проверку завершить не удалось. Откройте хранилище заново.");}
    return result;
}
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
    // Атомарное сохранение поверх проверенного файла (см. acceptVerified): запись во
    // временный файл с правами только владельца → перечитывание и сверка → копия
    // подтверждённой версии в .bak (тоже через tmp) → атомарная замена основного.
    // Инварианты: .bak создаётся только из ПОДТВЕРЖДЁННОГО содержимого;
    // читается отдельным восстановлением с полной проверкой Application; замена основного файла —
    // последний шаг, его отказ не требует отката; если основной файл на диске не
    // совпадает с последним подтверждённым — сохранение отменяется (защита от
    // параллельной правки вторым экземпляром или вручную).
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
