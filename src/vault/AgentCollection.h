#pragma once

#include "SecureBytes.h"
#include <QByteArrayView>
#include <QMap>
#include <QStringList>
#include <functional>

namespace tb::collection {
inline constexpr char FileName[] = "agent-collection.tbkc";
inline constexpr qsizetype MaxSnapshotBytes = 4 * 1024 * 1024;
// Пустая строка означает успех. При ошибке выходной секрет пуст.
QString issue(const QString& dir, const SecureKey& owner, SecureBytes& token);
QString revoke(const QString& dir, const SecureKey& owner);
QString ownerCreate(const QString& dir, const SecureKey& owner,
                    const QString& name, QByteArrayView value);
QString ownerList(const QString& dir, const SecureKey& owner, QStringList& names);
QString ownerRead(const QString& dir, const SecureKey& owner, const QString& name, SecureBytes& value);
QString ownerUpdate(const QString& dir, const SecureKey& owner, const QString& name,
                    QByteArrayView original, const QString& nextName, QByteArrayView value);
QString ownerDelete(const QString& dir, const SecureKey& owner, const QString& name, QByteArrayView original);
// Пустое имя проекта означает группу «Без проекта».
QString ownerTree(const QString& dir, const SecureKey& owner, QMap<QString, QStringList>& projects);
QString ownerList(const QString& dir, const SecureKey& owner, const QString& project, QStringList& names);
QString ownerRead(const QString& dir, const SecureKey& owner, const QString& project, const QString& name,
                  SecureBytes& value);
QString ownerCreate(const QString& dir, const SecureKey& owner, const QString& project, const QString& name,
                    QByteArrayView value);
QString ownerUpdate(const QString& dir, const SecureKey& owner, const QString& project, const QString& name,
                    QByteArrayView original, const QString& nextProject, const QString& nextName, QByteArrayView value);
QString ownerDelete(const QString& dir, const SecureKey& owner, const QString& project, const QString& name,
                    QByteArrayView original);
// Снимок содержит только проверенные записи. Пустое тело — пустая коллекция.
QString ownerSnapshot(const QString& dir, const SecureKey& owner, SecureBytes& plain);
// oldOwner == nullptr допустим только при отсутствии предыдущего хранилища.
QString restoreSnapshot(const QString& dir, const SecureKey* oldOwner, const SecureKey& newOwner,
                        QByteArrayView previousVault, QByteArrayView nextVault, QByteArrayView plain,
                        const std::function<void()>& commitVault);
QString execute(const QString& dir, QByteArrayView token, const QString& op,
                const QString& name, QByteArrayView value, SecureBytes& result, const QString& project = {});
QString agentListProjects(const QString& dir, QByteArrayView token, QStringList& projects);
QString agentList(const QString& dir, QByteArrayView token, const QString& project, QStringList& names);
QString agentMove(const QString& dir, QByteArrayView token, const QString& project, const QString& name,
                  const QString& nextProject, const QString& nextName);
QString rebindOwner(const QString& dir, const SecureKey& oldOwner, const SecureKey& newOwner,
                    QByteArrayView previousVault, QByteArrayView nextVault,
                    const std::function<void()>& commitVault);
}
