#include "AgentCollection.h"

#include "../platform/FileOps.h"
#include "CborWriter.h"
#include "Container.h"
#include "Crypto.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QtEndian>
#include <cstring>
#include <map>

namespace tb::collection {
namespace {
constexpr QByteArrayView Magic("TBKC\x01");
constexpr qsizetype NonceSize = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr qsizetype TagSize = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr qsizetype WrapSize = NonceSize + SecureKey::Size + TagSize;
constexpr qsizetype PrefixSize = Magic.size() + 1;
constexpr qsizetype MaxNameSize = 256;
constexpr qsizetype MaxValueSize = 32 * 1024;
constexpr qsizetype MaxPlainSize = 4 * 1024 * 1024;
constexpr qsizetype MaxFileSize = PrefixSize + 2 * WrapSize + NonceSize + TagSize + MaxPlainSize;
constexpr qsizetype FingerprintSize = 32;
constexpr qsizetype JournalIdsSize = 3 * FingerprintSize;
constexpr qsizetype JournalPrefixSize = JournalIdsSize + FingerprintSize;
constexpr size_t MaxEntries = 512;
constexpr qsizetype RecordHeaderSize = sizeof(quint16) + sizeof(quint32);
constexpr char OwnerContext[] = "TBKOWN01";
constexpr char AgentContext[] = "TBKAGT01";
const QByteArray OwnerAad("TBKC/v1/owner");
const QByteArray AgentAad("TBKC/v1/agent");

const QString Unauthorized = QStringLiteral("unauthorized");
const QString StorageError = QStringLiteral("storage-error");
const QString InvalidRequest = QStringLiteral("invalid-request");
const QString Corrupt = QStringLiteral("corrupt");
const QString NotFound = QStringLiteral("not-found");
const QString LimitExceeded = QStringLiteral("limit-exceeded");

QString readFile(const QString& path, QByteArray& bytes, qsizetype limit = MaxFileSize) {
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction()) return StorageError;
    if (!info.exists()) return {};
    if (!info.isFile()) return StorageError;
    if (info.size() <= 0 || info.size() > limit) return Corrupt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return StorageError;
    bytes = file.read(limit + 1);
    if (file.error() != QFile::NoError || !file.atEnd()) return StorageError;
    return bytes.size() == info.size() ? QString() : StorageError;
}

QString writeAtomic(const QString& path, const QByteArray& next, const QByteArray& previous) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return StorageError;
#ifndef Q_OS_WIN
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return StorageError;
#endif
    if (file.write(next) != next.size()) return StorageError;
    QByteArray current;
    if (!readFile(path, current).isEmpty() || current != previous) return StorageError;
    return file.commit() ? QString() : StorageError;
}

QByteArray journalChecksum(QByteArrayView ids, QByteArrayView next) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(ids);
    hash.addData(next);
    return hash.result();
}

class LockedFile {
public:
    explicit LockedFile(const QString& dir)
        : dir_(dir), path_(QDir(dir).absoluteFilePath(QString::fromLatin1(FileName))),
          journal_(path_ + QStringLiteral(".pending")), lock_(path_ + QStringLiteral(".lock")) {}

    QString open(bool create, const QString& absent) {
        const QFileInfo dirInfo(dir_);
        if (dir_.isEmpty() || dirInfo.isSymLink() || dirInfo.isJunction()) return StorageError;
        if (!create && !dirInfo.exists()) return absent;
        if (!store::ensurePrivateDir(dir_)) return StorageError;
        lock_.setStaleLockTime(0);
        if (!lock_.tryLock())
            return lock_.error() == QLockFile::LockFailedError ? QStringLiteral("busy") : StorageError;
        if (const auto error = readFile(path_, bytes); !error.isEmpty()) return error;
        if (const auto error = recover(); !error.isEmpty()) return error;
        return bytes.isNull() && !create ? absent : QString();
    }

    QByteArray vaultFingerprint() const {
        const auto path = QDir(dir_).absoluteFilePath(QStringLiteral("vault.tb"));
        const QFileInfo info(path);
        if (!info.isFile() || info.isSymLink() || info.isJunction()) return {};
        QFile vault(path);
        if (!vault.open(QIODevice::ReadOnly)) return {};
        return container::keyHeaderFingerprint(vault.read(container::KeyHeaderBytes));
    }

    QString stage(const QByteArray& oldId, const QByteArray& newId, const QByteArray& next) {
        const auto ids = oldId + newId + QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        return writeAtomic(journal_, ids + journalChecksum(ids, next) + next, {});
    }

    QString recover() {
        QByteArray pending;
        if (const auto error = readFile(journal_, pending, JournalPrefixSize + MaxFileSize); !error.isEmpty())
            return error;
        if (pending.isNull()) return {};
        if (bytes.isNull() || pending.size() < JournalPrefixSize + PrefixSize + WrapSize + NonceSize + TagSize)
            return Corrupt;
        const auto oldId = QByteArrayView(pending).first(FingerprintSize);
        const auto newId = QByteArrayView(pending).sliced(FingerprintSize, FingerprintSize);
        const auto prior = QByteArrayView(pending).sliced(2 * FingerprintSize, FingerprintSize);
        const auto digest = QByteArrayView(pending).sliced(JournalIdsSize, FingerprintSize);
        const auto next = QByteArrayView(pending).sliced(JournalPrefixSize);
        if (oldId == newId || !next.startsWith(Magic) || next[Magic.size()] != 0
            || journalChecksum(QByteArrayView(pending).first(JournalIdsSize), next) != digest)
            return Corrupt;
        const auto id = vaultFingerprint();
        if (id == newId) {
            if (bytes != next) {
                if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != prior) return StorageError;
                const auto ciphertext = next.toByteArray();
                if (const auto error = save(ciphertext); !error.isEmpty()) return error;
                bytes = ciphertext;
            }
        } else if (id != oldId) {
            return StorageError;
        }
        return QFile::remove(journal_) ? QString() : StorageError;
    }

    QString save(const QByteArray& next) { return writeAtomic(path_, next, bytes); }

    QByteArray bytes;
private:
    QString dir_, path_, journal_;
    QLockFile lock_;
};

// Формат v1: magic | active | owner wrap | [agent wrap] | body.
// Каждый шифротекст содержит nonce и тег. AAD тела охватывает весь заголовок.
// Запись тела: длина имени (BE16), длина значения (BE32), UTF-8 имя, байты значения.
using Entries = std::map<QByteArray, QByteArrayView>;
struct Collection {
    QByteArray header;
    SecureKey key;
    SecureBytes plain;
    Entries entries;
};

QByteArray appendBlob(QByteArray prefix, const crypto::Blob& blob) {
    prefix.append(reinterpret_cast<const char*>(blob.nonce), NonceSize);
    prefix.append(blob.ct);
    return prefix;
}

bool openWrap(const SecureKey& kek, const QByteArray& aad, QByteArrayView wrapped, SecureKey& key) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(wrapped.data());
    return crypto_aead_xchacha20poly1305_ietf_decrypt(
        key.data(), nullptr, nullptr, bytes + NonceSize, SecureKey::Size + TagSize,
        reinterpret_cast<const unsigned char*>(aad.constData()), aad.size(), bytes, kek.data()) == 0;
}

bool deriveKek(SecureKey& out, const unsigned char* source, bool agent) {
    return crypto_kdf_derive_from_key(out.data(), SecureKey::Size, 1,
                                      agent ? AgentContext : OwnerContext, source) == 0;
}

QByteArray encodeName(const QString& name) {
    if (name.size() > MaxNameSize || !name.isValidUtf16()) return {};
    for (const auto c : name.toUcs4()) {
        if (!QChar::isPrint(c) || c == 0xfeff) return {};
    }
    const auto utf8 = name.toUtf8();
    return utf8.size() <= MaxNameSize ? utf8 : QByteArray();
}

bool readEntries(QByteArrayView plain, Entries& entries) {
    while (!plain.isEmpty()) {
        if (plain.size() < RecordHeaderSize || entries.size() == MaxEntries) return false;
        const auto nameSize = qFromBigEndian<quint16>(plain.data());
        const auto valueSize = qFromBigEndian<quint32>(plain.data() + sizeof(quint16));
        plain = plain.sliced(RecordHeaderSize);
        if (nameSize == 0 || nameSize > MaxNameSize || valueSize > MaxValueSize
            || plain.size() < qsizetype(nameSize) + valueSize) return false;
        const auto name = plain.first(nameSize);
        auto key = encodeName(QString::fromUtf8(name));
        if (key != name || !entries.emplace(std::move(key), plain.sliced(nameSize, valueSize)).second)
            return false;
        plain = plain.sliced(nameSize + valueSize);
    }
    return true;
}

QString openCollection(const QByteArray& file, const unsigned char* source, bool agent, Collection& collection) {
    if (file.size() < PrefixSize || !file.startsWith(Magic)) return Corrupt;
    const char active = file[Magic.size()];
    if (active != 0 && active != 1) return Corrupt;
    const qsizetype headerSize = PrefixSize + (active + 1) * WrapSize;
    if (file.size() < headerSize + NonceSize + TagSize
        || file.size() - headerSize - NonceSize - TagSize > MaxPlainSize) return Corrupt;
    if (agent && !active) return Unauthorized;

    SecureKey kek;
    if (!deriveKek(kek, source, agent)) return StorageError;
    const auto wrapped = QByteArrayView(file).sliced(PrefixSize + (agent ? WrapSize : 0), WrapSize);
    if (!openWrap(kek, agent ? AgentAad : OwnerAad, wrapped, collection.key)) return Unauthorized;

    collection.header = file.first(headerSize);
    crypto::Blob body;
    std::memcpy(body.nonce, file.constData() + headerSize, NonceSize);
    body.ct = file.sliced(headerSize + NonceSize);
    QByteArray plain;
    if (!crypto::open(collection.key, collection.header, body, plain)) return Corrupt;
    collection.plain = SecureBytes(std::move(plain));
    return readEntries(collection.plain.bytes(), collection.entries) ? QString() : Corrupt;
}

QByteArray sealBody(const QByteArray& header, const SecureKey& key, QByteArrayView plain) {
    crypto::Blob body;
    if (!crypto::seal(key, header, reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), body))
        return {};
    return appendBlob(header, body);
}

QString saveBody(LockedFile& file, const QByteArray& header, const SecureKey& key, QByteArrayView plain) {
    const auto bytes = sealBody(header, key, plain);
    return bytes.isEmpty() ? StorageError : file.save(bytes);
}

QByteArray rotate(const SecureKey& owner, QByteArrayView agent, QByteArrayView plain) {
    SecureKey nextKey, kek;
    randombytes_buf(nextKey.data(), SecureKey::Size);
    if (!deriveKek(kek, owner.data(), false)) return {};
    crypto::Blob wrap;
    if (!crypto::seal(kek, OwnerAad, nextKey.data(), SecureKey::Size, wrap)) return {};
    QByteArray header(Magic.data(), Magic.size());
    header.append(char(!agent.isEmpty()));
    header = appendBlob(std::move(header), wrap);
    if (!agent.isEmpty()) {
        if (!deriveKek(kek, reinterpret_cast<const unsigned char*>(agent.data()), true)
            || !crypto::seal(kek, AgentAad, nextKey.data(), SecureKey::Size, wrap)) return {};
        header = appendBlob(std::move(header), wrap);
    }
    return sealBody(header, nextKey, plain);
}

QString saveRotation(LockedFile& file, const SecureKey& owner, QByteArrayView agent, QByteArrayView plain) {
    const auto bytes = rotate(owner, agent, plain);
    return bytes.isEmpty() ? StorageError : file.save(bytes);
}

QString saveEntries(LockedFile& file, const Collection& collection) {
    qsizetype size = 0;
    for (const auto& [name, value] : collection.entries)
        size += RecordHeaderSize + name.size() + value.size();
    if (size > MaxPlainSize) return LimitExceeded;
    cbor::SecureBuffer plain(size);
    auto* out = plain.data();
    for (const auto& [name, value] : collection.entries) {
        qToBigEndian<quint16>(quint16(name.size()), out);
        qToBigEndian<quint32>(quint32(value.size()), out + sizeof(quint16));
        out += RecordHeaderSize;
        std::memcpy(out, name.constData(), name.size());
        out += name.size();
        if (!value.isEmpty()) std::memcpy(out, value.data(), value.size());
        out += value.size();
    }
    return saveBody(file, collection.header, collection.key,
                    QByteArrayView(reinterpret_cast<const char*>(plain.data()), size));
}

QString addEntry(LockedFile& file, Collection& collection, const QByteArray& name, QByteArrayView value) {
    if (collection.entries.contains(name)) return QStringLiteral("already-exists");
    if (collection.entries.size() == MaxEntries) return LimitExceeded;
    collection.entries.emplace(name, value);
    return saveEntries(file, collection);
}
} // namespace

QString issue(const QString& dir, const SecureKey& owner, SecureBytes& token) {
    token.clear();
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(true, {}); !error.isEmpty()) return error;
    Collection collection;
    if (!file.bytes.isNull()) {
        if (const auto error = openCollection(file.bytes, owner.data(), false, collection); !error.isEmpty())
            return error;
    }
    auto next = crypto::generateAgentToken();
    const auto agent = crypto::parseAgentToken(next.bytes());
    if (agent.isEmpty()) return StorageError;
    if (const auto error = saveRotation(file, owner, agent.bytes(), collection.plain.bytes()); !error.isEmpty())
        return error;
    token = std::move(next);
    return {};
}

QString revoke(const QString& dir, const SecureKey& owner) {
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(false, NotFound); !error.isEmpty()) return error;
    Collection collection;
    if (const auto error = openCollection(file.bytes, owner.data(), false, collection); !error.isEmpty())
        return error;
    if (collection.header[Magic.size()] == 0) return {};
    return saveRotation(file, owner, {}, collection.plain.bytes());
}

QString ownerCreate(const QString& dir, const SecureKey& owner, const QString& name, QByteArrayView value) {
    const auto encodedName = encodeName(name);
    if (encodedName.isEmpty() || value.size() > MaxValueSize) return InvalidRequest;
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(false, NotFound); !error.isEmpty()) return error;
    Collection collection;
    if (const auto error = openCollection(file.bytes, owner.data(), false, collection); !error.isEmpty())
        return error;
    return addEntry(file, collection, encodedName, value);
}

QString execute(const QString& dir, QByteArrayView token, const QString& op,
                const QString& name, QByteArrayView value, SecureBytes& result) {
    result.clear();
    const bool read = op == QStringLiteral("read");
    const bool create = op == QStringLiteral("create");
    const auto encodedName = encodeName(name);
    if ((!read && !create && op != QStringLiteral("replace")) || encodedName.isEmpty()
        || value.size() > MaxValueSize || (read && !value.isEmpty())) return InvalidRequest;
    const auto rawToken = crypto::parseAgentToken(token);
    if (rawToken.isEmpty()) return Unauthorized;
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(false, Unauthorized); !error.isEmpty()) return error;
    Collection collection;
    if (const auto error = openCollection(file.bytes,
            reinterpret_cast<const unsigned char*>(rawToken.bytes().constData()), true, collection); !error.isEmpty())
        return error;
    if (create) return addEntry(file, collection, encodedName, value);
    const auto entry = collection.entries.find(encodedName);
    if (entry == collection.entries.end()) return NotFound;
    if (read) {
        result = SecureBytes(entry->second.toByteArray());
        return {};
    }
    entry->second = value;
    return saveEntries(file, collection);
}

QString rebindOwner(const QString& dir, const SecureKey& oldOwner, const SecureKey& newOwner,
                    QByteArrayView previousVault, QByteArrayView nextVault,
                    const std::function<void()>& commitVault) {
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(true, {}); !error.isEmpty()) return error;
    if (file.bytes.isNull() || oldOwner == newOwner) {
        commitVault();
        return {};
    }
    const auto oldId = container::keyHeaderFingerprint(previousVault);
    const auto newId = container::keyHeaderFingerprint(nextVault);
    if (oldId.isEmpty() || newId.isEmpty() || oldId == newId || file.vaultFingerprint() != oldId)
        return StorageError;

    Collection collection;
    if (const auto error = openCollection(file.bytes, oldOwner.data(), false, collection); !error.isEmpty())
        return error;
    const auto next = rotate(newOwner, {}, collection.plain.bytes());
    if (next.isEmpty()) return StorageError;
    if (const auto error = file.stage(oldId, newId, next); !error.isEmpty()) return error;
    // Фиксация vault.tb выбирает снимок. В случае сбоя следующий вызов завершит
    // перенос или отменит его под той же блокировкой.
    commitVault();
    return file.recover();
}
} // namespace tb::collection
