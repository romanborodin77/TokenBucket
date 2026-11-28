#pragma once

#include "SecureBytes.h"
#include <QByteArrayView>
#include <QString>
#include <functional>

namespace tb::collection {
inline constexpr char FileName[] = "agent-collection.tbkc";
// Пустая строка означает успех. При ошибке выходной секрет пуст.
QString issue(const QString& dir, const SecureKey& owner, SecureBytes& token);
QString revoke(const QString& dir, const SecureKey& owner);
QString ownerCreate(const QString& dir, const SecureKey& owner,
                    const QString& name, QByteArrayView value);
QString execute(const QString& dir, QByteArrayView token, const QString& op,
                const QString& name, QByteArrayView value, SecureBytes& result);
QString rebindOwner(const QString& dir, const SecureKey& oldOwner, const SecureKey& newOwner,
                    QByteArrayView previousVault, QByteArrayView nextVault,
                    const std::function<void()>& commitVault);
}
