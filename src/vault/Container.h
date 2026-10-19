#pragma once
#include <QByteArray>
#include "SecureBytes.h"
namespace tb::container {
enum class Status { Ok, BadFormat, Unsupported, WrongKey, TooLarge };
constexpr uint8_t ModePassword = 1, ModeHello = 2;
constexpr qsizetype MaxFileBytes  = 80LL * 1024 * 1024;
constexpr qsizetype MaxPlainBytes = 64LL * 1024 * 1024;
struct Opened {
    Opened() = default;
    Opened(Opened&&) = default;
    Status status = Status::BadFormat;
    QByteArray cbor;
    SecureKey dataKey;
};
uint8_t readMode(const QByteArray& fileBytes);
QByteArray packPassword(const QByteArray& passwordUtf8, const QByteArray& cbor, SecureKey* outKey);
QByteArray reseal(const QByteArray& fileBytes, const SecureKey& dataKey, const QByteArray& cbor);
Opened openPassword(const QByteArray& fileBytes, const QByteArray& passwordUtf8);
#ifdef Q_OS_WIN
QByteArray packHello(const QByteArray& cbor, SecureKey* outKey);
Opened openHello(const QByteArray& fileBytes);
#endif
}
