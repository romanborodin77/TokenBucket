#include "FileOps.h"
#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace tb::store {
namespace {
bool containsLink(const QString& path) {
    for (QFileInfo info(path);;) {
        if (info.isSymbolicLink() || info.isShortcut() || info.isJunction()) return true;
        const QString parent = info.absolutePath();
        if (parent == info.absoluteFilePath()) return false;
        info.setFile(parent);
    }
}
}

QString exportFile(const QString& path, const QByteArray& bytes, bool replace, FileOps& ops) {
    if (containsLink(path))
        return QStringLiteral("Нельзя сохранить файл через ссылку. Выберите другой путь.");
    if (!replace)
        return ops.writeAll(path, bytes) ? QString() : QStringLiteral("Не удалось создать файл. Проверьте путь и права доступа.");

    const auto name = QStringLiteral(".tb-new-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString temporary = QFileInfo(path).dir().filePath(name);
    if (!ops.writeAll(temporary, bytes))
        return QStringLiteral("Не удалось создать временный файл для замены.");
    if (ops.renameReplace(temporary, path)) return {};
    if (!ops.removeFile(temporary))
        return QStringLiteral("Не удалось завершить замену. Удалите временный файл с незашифрованными данными: %1").arg(temporary);
    return QStringLiteral("Не удалось завершить замену файла.");
}
}
