#pragma once
#include <QByteArray>
#include "SecureBytes.h"
namespace tb::crypto {
bool init();
bool deriveKey(const QByteArray& passwordUtf8, const unsigned char* salt16, SecureKey& out);
struct Blob { unsigned char nonce[24]; QByteArray ct; };
bool seal(const SecureKey& key, const QByteArray& aad, const QByteArray& plain, Blob& out,
          const unsigned char* nonce = nullptr);
bool open(const SecureKey& key, const QByteArray& aad, const Blob& blob, QByteArray& plain);
}
