#include "Crypto.h"
#include <sodium.h>
namespace tb::crypto {
bool init() { return sodium_init() >= 0; }
bool deriveKey(const QByteArray& pw, const unsigned char* salt, SecureKey& out) {
    // Фиксированные параметры спеки: opslimit 3, memlimit 64 МиБ, Argon2id v1.3, выход 32 байта.
    return crypto_pwhash(out.data(), SecureKey::Size, pw.constData(), quint64(pw.size()),
                         salt, 3, 64ull * 1024 * 1024, crypto_pwhash_ALG_ARGON2ID13) == 0;
}
bool seal(const SecureKey& k, const QByteArray& aad, const QByteArray& plain, Blob& out) {
    out.ct.resize(int(plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES));
    unsigned long long clen = 0;
    randombytes_buf(out.nonce, sizeof(out.nonce));
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char*>(out.ct.data()), &clen,
            reinterpret_cast<const unsigned char*>(plain.constData()), quint64(plain.size()),
            reinterpret_cast<const unsigned char*>(aad.constData()), quint64(aad.size()),
            nullptr, out.nonce, k.data()) != 0)
        return false;
    out.ct.resize(int(clen));
    return true;
}
bool open(const SecureKey& k, const QByteArray& aad, const Blob& blob, QByteArray& plain) {
    plain.resize(int(blob.ct.size()));
    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plain.data()), &mlen, nullptr,
            reinterpret_cast<const unsigned char*>(blob.ct.constData()), quint64(blob.ct.size()),
            reinterpret_cast<const unsigned char*>(aad.constData()), quint64(aad.size()),
            blob.nonce, k.data()) != 0) {
        sodium_memzero(plain.data(), size_t(plain.size()));
        plain.clear();
        return false;
    }
    plain.resize(int(mlen));
    return true;
}
} // namespace tb::crypto
