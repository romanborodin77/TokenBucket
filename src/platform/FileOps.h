#pragma once
#include <QByteArray>
#include <QString>
namespace tb::store {
// Файловые операции хранилища. Абстракция ради тестов (test_store подставляет свою
// реализацию в памяти); контракт реальной Windows-версии:
//  - writeAll всегда создаёт файл ЗАНОВО (CREATE_NEW) с DACL «только текущий
//    пользователь и SYSTEM» — временные/резервные копии не существуют ни секунды
//    с унаследованными правами;
//  - renameReplace отказывается работать через reparse point (symlink/junction) —
//    замена не уйдёт мимо каталога данных.
struct FileOps {
    virtual ~FileOps() = default;
    virtual bool writeAll(const QString& path, const QByteArray& bytes) = 0;
    virtual bool renameReplace(const QString& from, const QString& to) = 0;
    virtual bool removeFile(const QString& path) = 0;
};
FileOps& realFileOps();
bool ensurePrivateDir(const QString& dir);
}
