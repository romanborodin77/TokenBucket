#include "Crypto.h"
#include <sodium.h>
namespace tb::crypto {
bool init() { return sodium_init() >= 0; }
// Ключ из пароля: Argon2id с фиксированными параметрами спеки. Параметры записаны
// в заголовке контейнера и проверяются при чтении — подменённый файл с ослабленными
// настройками KDF будет отклонён (downgrade невозможен).
bool deriveKey(const QByteArray& pw, const unsigned char* salt, SecureKey& out) {
    // Фиксированные параметры спеки: opslimit 3, memlimit 64 МиБ, Argon2id v1.3, выход 32 байта.
    return crypto_pwhash(out.data(), SecureKey::Size, pw.constData(), quint64(pw.size()),
                         salt, 3, 64ull * 1024 * 1024, crypto_pwhash_ALG_ARGON2ID13) == 0;
}
// Шифрование секрета: XChaCha20-Poly1305-IETF. AAD покрывает весь заголовок контейнера,
// поэтому подмена любого байта до шифротекста обнаружится при расшифровке. Nonce —
// 24 байта CSPRNG на каждую операцию (пространство XChaCha не требует счётчика);
// при seal обёртки ключа nonce фиксирован вызывателем (нулевой в Hello-режиме —
// часть формата), для тела — всегда случайный.
bool seal(const SecureKey& k, const QByteArray& aad, const unsigned char* plain, quint64 plainLen,
          Blob& out, const unsigned char* nonce) {
    out.ct.resize(int(plainLen + crypto_aead_xchacha20poly1305_ietf_ABYTES));
    unsigned long long clen = 0;
    if (nonce) memcpy(out.nonce, nonce, sizeof(out.nonce));
    else randombytes_buf(out.nonce, sizeof(out.nonce));
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char*>(out.ct.data()), &clen,
            plain, plainLen,
            reinterpret_cast<const unsigned char*>(aad.constData()), quint64(aad.size()),
            nullptr, out.nonce, k.data()) != 0)
        return false;
    out.ct.resize(int(clen));
    return true;
}
bool seal(const SecureKey& k, const QByteArray& aad, const QByteArray& plain, Blob& out,
          const unsigned char* nonce) {
    return seal(k, aad, reinterpret_cast<const unsigned char*>(plain.constData()),
                quint64(plain.size()), out, nonce);
}
bool open(const SecureKey& k, const QByteArray& aad, const Blob& blob, QByteArray& plain) {
    plain.resize(int(blob.ct.size()));
    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plain.data()), &mlen, nullptr,
            reinterpret_cast<const unsigned char*>(blob.ct.constData()), quint64(blob.ct.size()),
            reinterpret_cast<const unsigned char*>(aad.constData()), quint64(aad.size()),
            blob.nonce, k.data()) != 0) {
        // Отказ аутентификации: содержимое затирается и не покидает функцию.
        sodium_memzero(plain.data(), size_t(plain.size()));
        plain.clear();
        return false;
    }
    plain.resize(int(mlen));
    return true;
}
} // namespace tb::crypto
