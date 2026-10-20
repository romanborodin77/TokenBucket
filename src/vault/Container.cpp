#include "Container.h"
#include "Crypto.h"
#include <QDataStream>
#include <QIODevice>
#include <sodium.h>
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif
namespace tb::container {
namespace {
constexpr char kMagic[4] = {'T','B','V','K'};
constexpr uint16_t kVersion = 1, kKdfArgon2id = 1;
constexpr uint32_t kOpslimit = 3, kMemlimit = 64u * 1024 * 1024;
constexpr int kHead = 61; // всё до wrap

struct Parsed {
    uint8_t mode = 0;        // 255 = распознан, но параметры/версия не поддержаны
    QByteArray aad1, salt, wrap, aad2;
    crypto::Blob ct;
};

bool parse(const QByteArray& f, Parsed& p) {
    if (f.size() < kHead + 24 + 8 + 16 || f.size() > int(MaxFileBytes)) return false;
    QDataStream ds(f); // Qt 6: конструктор для чтения из const QByteArray&
    ds.setByteOrder(QDataStream::LittleEndian);
    char magic[4]; uint16_t ver, kdf; uint8_t mode; uint32_t ops, mem; quint32 wrapLen;
    ds.readRawData(magic, 4); ds >> ver >> mode >> kdf >> ops >> mem;
    if (ds.status() != QDataStream::Ok || memcmp(magic, kMagic, 4) != 0) return false;
    if (ver != kVersion || (mode != ModePassword && mode != ModeHello) || kdf != kKdfArgon2id) { p.mode = 255; return true; }
    if (ops != kOpslimit || mem != kMemlimit) { p.mode = 255; return true; }
    p.mode = mode;
    p.salt.resize(16); ds.readRawData(p.salt.data(), 16);
    QByteArray wrapNonce(24, '\0'); ds.readRawData(wrapNonce.data(), 24);
    ds >> wrapLen;
    if (ds.status() != QDataStream::Ok || wrapLen == 0
        || wrapLen > quint64(f.size() - kHead - 24 - 8 - 16)
        || (mode == ModePassword && wrapLen != SecureKey::Size + 16)
        || (mode == ModeHello && wrapNonce != QByteArray(24, '\0'))) return false;
    p.aad1 = QByteArray::fromRawData(f.constData(), kHead); // до wrap
    p.wrap.resize(int(wrapLen)); ds.readRawData(p.wrap.data(), int(wrapLen));
    p.aad2 = QByteArray::fromRawData(f.constData(), kHead + int(wrapLen) + 24 + 8);
    memcpy(p.ct.nonce, f.constData() + kHead + int(wrapLen), 24);
    ds.skipRawData(24); // ctNonce прочитан напрямую из f
    quint64 ctLen; ds >> ctLen;
    if (ds.status() != QDataStream::Ok || ctLen != quint64(f.size()) - (kHead + wrapLen + 24 + 8)) return false;
    p.ct.ct = f.mid(kHead + int(wrapLen) + 24 + 8);
    return true;
}

QByteArray sealBody(QByteArray prefix, const SecureKey& key, const unsigned char* cbor, qsizetype cborLen) {
    unsigned char nonce[24]; randombytes_buf(nonce, sizeof nonce);
    QByteArray tail;
    QDataStream ds(&tail, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.writeRawData(reinterpret_cast<const char*>(nonce), 24);
    ds << quint64(cborLen + 16);
    prefix += tail;
    crypto::Blob ct;
    if (!crypto::seal(key, prefix, cbor, quint64(cborLen), ct, nonce)) return {};
    return prefix + ct.ct;
}
} // namespace

uint8_t readMode(const QByteArray& f) {
    Parsed p;
    if (!parse(f, p)) return 0;
    return p.mode == 255 ? 0 : p.mode;
}

QByteArray packPassword(const QByteArray& pw, const unsigned char* cbor, qsizetype cborLen, SecureKey* outKey) {
    if (pw.size() < 12 || pw.size() > 1024 || cborLen < 0 || cborLen > MaxPlainBytes) return {};
    SecureKey dataKey, kek;
    randombytes_buf(dataKey.data(), SecureKey::Size);
    QByteArray salt(16, '\0'); randombytes_buf(salt.data(), 16);
    if (!crypto::deriveKey(pw, reinterpret_cast<const unsigned char*>(salt.constData()), kek)) return {};
    crypto::Blob wrap;
    randombytes_buf(wrap.nonce, sizeof wrap.nonce);
    // aad1 = первые 61 байт итогового файла = константная головка без wrap
    QByteArray head; { QDataStream ds(&head, QIODevice::WriteOnly); ds.setByteOrder(QDataStream::LittleEndian);
        ds.writeRawData(kMagic,4); ds << kVersion << uint8_t(ModePassword) << kKdfArgon2id << kOpslimit << kMemlimit;
        ds.writeRawData(salt.constData(),16); ds.writeRawData(reinterpret_cast<const char*>(wrap.nonce),24); ds << uint32_t(SecureKey::Size + 16); }
    unsigned char wrapNonce[24]; memcpy(wrapNonce, wrap.nonce, 24);
    // Ключ заворачивается прямо из защищённого буфера SecureKey — без копии в обычной куче.
    const bool wrapped = crypto::seal(kek, head, dataKey.data(), SecureKey::Size, wrap, wrapNonce);
    if (!wrapped) return {};
    QByteArray file = sealBody(head + wrap.ct, dataKey, cbor, cborLen);
    if (!file.isEmpty() && outKey) memcpy(outKey->data(), dataKey.data(), SecureKey::Size);
    return file;
}
QByteArray packPassword(const QByteArray& pw, const QByteArray& cbor, SecureKey* outKey) {
    return packPassword(pw, reinterpret_cast<const unsigned char*>(cbor.constData()), cbor.size(), outKey);
}

Opened openPassword(const QByteArray& f, const QByteArray& pw) {
    Opened r; Parsed p;
    if (f.size() > int(MaxFileBytes)) { r.status = Status::TooLarge; return r; }
    if (!parse(f, p)) { r.status = Status::BadFormat; return r; }
    if (p.mode == 255 || p.mode != ModePassword) { r.status = Status::Unsupported; return r; }
    if (pw.size() < 12 || pw.size() > 1024) { r.status = Status::WrongKey; return r; }
    SecureKey kek;
    if (!crypto::deriveKey(pw, reinterpret_cast<const unsigned char*>(p.salt.constData()), kek)) { r.status = Status::WrongKey; return r; }
    crypto::Blob wrap; wrap.ct = p.wrap; memcpy(wrap.nonce, p.aad1.constData() + 33, 24);
    QByteArray keyBytes;
    if (!crypto::open(kek, p.aad1, wrap, keyBytes) || keyBytes.size() != int(SecureKey::Size)) {
        sodium_memzero(keyBytes.data(), keyBytes.size()); r.status = Status::WrongKey; return r;
    }
    SecureKey dataKey; memcpy(dataKey.data(), keyBytes.constData(), size_t(keyBytes.size()));
    sodium_memzero(keyBytes.data(), size_t(keyBytes.size()));
    if (!crypto::open(dataKey, p.aad2, p.ct, r.cbor)) { r.status = Status::WrongKey; return r; }
    if (r.cbor.size() > int(MaxPlainBytes)) { r.status = Status::TooLarge; sodium_memzero(r.cbor.data(), r.cbor.size()); r.cbor.clear(); return r; }
    memcpy(r.dataKey.data(), dataKey.data(), SecureKey::Size);
    r.status = Status::Ok;
    return r;
}

QByteArray reseal(const QByteArray& f, const SecureKey& dataKey, const unsigned char* cbor, qsizetype cborLen) {
    if (cborLen < 0 || cborLen > MaxPlainBytes) return {};
    Parsed p;
    if (!parse(f, p) || p.mode == 255) return {};
    return sealBody(f.left(kHead + p.wrap.size()), dataKey, cbor, cborLen);
}
QByteArray reseal(const QByteArray& f, const SecureKey& dataKey, const QByteArray& cbor) {
    return reseal(f, dataKey, reinterpret_cast<const unsigned char*>(cbor.constData()), cbor.size());
}

#ifdef Q_OS_WIN
namespace {
QByteArray dpapiWrap(SecureKey& key) {
    DATA_BLOB in{ SecureKey::Size, reinterpret_cast<BYTE*>(key.data()) }, out{};
    if (!CryptProtectData(&in, L"TokenBucket", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    QByteArray r(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return r;
}
}
QByteArray packHello(const unsigned char* cbor, qsizetype cborLen, SecureKey* outKey) {
    if (cborLen < 0 || cborLen > MaxPlainBytes) return {};
    SecureKey dataKey;
    randombytes_buf(dataKey.data(), SecureKey::Size);
    QByteArray wrap = dpapiWrap(dataKey);
    if (wrap.isEmpty()) return {};
    QByteArray salt(16, '\0'); randombytes_buf(salt.data(), 16);
    QByteArray wrapNonce(24, '\0');
    QByteArray head; { QDataStream ds(&head, QIODevice::WriteOnly); ds.setByteOrder(QDataStream::LittleEndian);
        ds.writeRawData(kMagic,4); ds << kVersion << uint8_t(ModeHello) << kKdfArgon2id << kOpslimit << kMemlimit;
        ds.writeRawData(salt.constData(),16); ds.writeRawData(wrapNonce.constData(),24); ds << uint32_t(wrap.size()); }
    QByteArray file = sealBody(head + wrap, dataKey, cbor, cborLen);
    if (!file.isEmpty() && outKey) memcpy(outKey->data(), dataKey.data(), SecureKey::Size);
    return file;
}
QByteArray packHello(const QByteArray& cbor, SecureKey* outKey) {
    return packHello(reinterpret_cast<const unsigned char*>(cbor.constData()), cbor.size(), outKey);
}
Opened openHello(const QByteArray& f) {
    Opened r; Parsed p;
    if (f.size() > int(MaxFileBytes)) { r.status = Status::TooLarge; return r; }
    if (!parse(f, p)) { r.status = Status::BadFormat; return r; }
    if (p.mode == 255 || p.mode != ModeHello) { r.status = Status::Unsupported; return r; }
    DATA_BLOB in{ DWORD(p.wrap.size()), reinterpret_cast<BYTE*>(const_cast<char*>(p.wrap.constData())) }, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) { r.status = Status::WrongKey; return r; }
    SecureKey dataKey;
    const bool valid = out.cbData == SecureKey::Size;
    if (valid) memcpy(dataKey.data(), out.pbData, SecureKey::Size);
    sodium_memzero(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (!valid) return r;
    if (!crypto::open(dataKey, p.aad2, p.ct, r.cbor)) { r.status = Status::WrongKey; r.cbor.clear(); return r; }
    if (r.cbor.size() > int(MaxPlainBytes)) { r.status = Status::TooLarge; sodium_memzero(r.cbor.data(), r.cbor.size()); r.cbor.clear(); return r; }
    r.dataKey = std::move(dataKey);
    r.status = Status::Ok;
    return r;
}
#endif
} // namespace tb::container
