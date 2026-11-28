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
#include <filesystem>
#include <map>
#include <tuple>

namespace tb::collection {
namespace {
constexpr QByteArrayView Magic("TBKC\x01");
constexpr qsizetype NonceSize = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr qsizetype TagSize = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr qsizetype WrapSize = NonceSize + SecureKey::Size + TagSize;
constexpr qsizetype PrefixSize = Magic.size() + 1;
constexpr qsizetype MaxNameSize = 256;
constexpr qsizetype MaxValueSize = 32 * 1024;
constexpr qsizetype EmptyOwnerFileSize = PrefixSize + WrapSize + NonceSize + TagSize;
constexpr qsizetype MaxFileSize = EmptyOwnerFileSize + WrapSize + MaxSnapshotBytes;
constexpr qsizetype FingerprintSize = 32;
constexpr qsizetype JournalIdsSize = 3 * FingerprintSize;
constexpr qsizetype JournalPrefixSize = JournalIdsSize + FingerprintSize;
constexpr qsizetype MaxJournalSize = JournalPrefixSize + EmptyOwnerFileSize + MaxSnapshotBytes;
constexpr size_t MaxEntries = 512;
constexpr qsizetype RecordHeaderSize = sizeof(quint16) + sizeof(quint32);
constexpr qsizetype ProjectHeaderSize = 2 * sizeof(quint16);
constexpr char OwnerContext[] = "TBKOWN01";
constexpr char AgentContext[] = "TBKAGT01";
const QByteArray OwnerAad("TBKC/v1/owner");
const QByteArray AgentAad("TBKC/v1/agent");
const QByteArray AbsentVaultId(FingerprintSize, '\0');

const QString Unauthorized = QStringLiteral("unauthorized");
const QString StorageError = QStringLiteral("storage-error");
const QString InvalidRequest = QStringLiteral("invalid-request");
const QString Corrupt = QStringLiteral("corrupt");
const QString NotFound = QStringLiteral("not-found");
const QString LimitExceeded = QStringLiteral("limit-exceeded");
const QString AlreadyExists = QStringLiteral("already-exists");
const QString Conflict = QStringLiteral("conflict");

enum class PathState { Missing, Regular, Directory, Unavailable };

PathState pathState(const QFileInfo& info) {
    if (info.isSymLink() || info.isJunction()) return PathState::Unavailable;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(info.filePath().toStdU16String(), error);
    if (status.type() == std::filesystem::file_type::not_found
        && (!error || error == std::errc::no_such_file_or_directory)) return PathState::Missing;
    if (error) return PathState::Unavailable;
    if (std::filesystem::is_regular_file(status)) return PathState::Regular;
    return std::filesystem::is_directory(status) ? PathState::Directory : PathState::Unavailable;
}

QString readFile(const QString& path, QByteArray& bytes, qsizetype limit = MaxFileSize) {
    const QFileInfo info(path);
    const auto state = pathState(info);
    if (state == PathState::Missing) return {};
    if (state != PathState::Regular) return StorageError;
    if (info.size() <= 0 || info.size() > limit) return Corrupt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return StorageError;
    bytes = file.read(limit + 1);
    if (file.error() != QFile::NoError || !file.atEnd()) return StorageError;
    return bytes.size() == info.size() ? QString() : StorageError;
}

QString writeAtomic(const QString& path, const QByteArray& next, const QByteArray& previous,
                    qsizetype limit = MaxFileSize) {
    if (next.isEmpty() || next.size() > limit) return StorageError;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return StorageError;
#ifndef Q_OS_WIN
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return StorageError;
#endif
    if (file.write(next) != next.size()) return StorageError;
    QByteArray current;
    if (!readFile(path, current, limit).isEmpty() || current != previous) return StorageError;
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

    QString open(bool create, const QString& absent, bool requireDirectory = false) {
        if (dir_.isEmpty()) return StorageError;
        const auto state = pathState(QFileInfo(dir_));
        if (state == PathState::Missing && !create) return requireDirectory ? StorageError : absent;
        if (state != PathState::Directory && state != PathState::Missing) return StorageError;
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
        const auto state = pathState(QFileInfo(path));
        if (state == PathState::Missing) return AbsentVaultId;
        if (state != PathState::Regular) return {};
        QFile vault(path);
        if (!vault.open(QIODevice::ReadOnly)) return {};
        const auto header = vault.read(container::KeyHeaderBytes);
        return vault.error() == QFile::NoError ? container::keyHeaderFingerprint(header) : QByteArray();
    }

    QString replaceCollection(const QByteArray& oldId, const QByteArray& newId, const QByteArray& next,
                              const std::function<void()>& commitVault) {
        if (oldId.isEmpty() || newId.isEmpty() || oldId == newId || newId == AbsentVaultId
            || vaultFingerprint() != oldId || (oldId == AbsentVaultId && !bytes.isNull()))
            return StorageError;
        const auto ids = oldId + newId + QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        if (const auto error = writeAtomic(journal_, ids + journalChecksum(ids, next) + next, {}, MaxJournalSize);
            !error.isEmpty()) return error;
        // Фиксация vault.tb выбирает снимок. После сбоя его выберет recover().
        commitVault();
        return recover();
    }

    QString recover() {
        QByteArray pending;
        if (const auto error = readFile(journal_, pending, MaxJournalSize); !error.isEmpty())
            return error;
        if (pending.isNull()) return {};
        if (pending.size() < JournalPrefixSize + EmptyOwnerFileSize)
            return Corrupt;
        const auto oldId = QByteArrayView(pending).first(FingerprintSize);
        const auto newId = QByteArrayView(pending).sliced(FingerprintSize, FingerprintSize);
        const auto prior = QByteArrayView(pending).sliced(2 * FingerprintSize, FingerprintSize);
        const auto digest = QByteArrayView(pending).sliced(JournalIdsSize, FingerprintSize);
        const auto next = QByteArrayView(pending).sliced(JournalPrefixSize);
        if (oldId == newId || newId == AbsentVaultId || !next.startsWith(Magic) || next[Magic.size()] != 0
            || (oldId == AbsentVaultId && prior != QCryptographicHash::hash(QByteArrayView{}, QCryptographicHash::Sha256))
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
        } else if (id != oldId || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != prior) {
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
// Запись без проекта: BE16 длина имени, BE32 длина значения, имя, значение.
// Запись с проектом: BE16 ноль, BE16 длина проекта, длины имени и значения,
// проект, имя, значение. Имена и проекты в UTF-8, значения — произвольные байты.
struct EntryKey {
    QByteArray project, name;
    bool operator==(const EntryKey&) const = default;
    bool operator<(const EntryKey& other) const {
        return std::tie(project, name) < std::tie(other.project, other.name);
    }
};
using Entries = std::map<EntryKey, QByteArrayView>;
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

EntryKey encodeKey(const QString& project, const QString& name) {
    auto encodedProject = encodeName(project);
    if (!project.isEmpty() && encodedProject.isEmpty()) return {};
    return {std::move(encodedProject), encodeName(name)};
}

bool readEntries(QByteArrayView plain, Entries& entries) {
    while (!plain.isEmpty()) {
        if (plain.size() < RecordHeaderSize || entries.size() == MaxEntries) return false;
        quint16 projectSize = 0;
        if (qFromBigEndian<quint16>(plain.data()) == 0) {
            if (plain.size() < ProjectHeaderSize + RecordHeaderSize) return false;
            projectSize = qFromBigEndian<quint16>(plain.data() + sizeof(quint16));
            if (projectSize == 0 || projectSize > MaxNameSize) return false;
            plain = plain.sliced(ProjectHeaderSize);
        }
        const auto nameSize = qFromBigEndian<quint16>(plain.data());
        const auto valueSize = qFromBigEndian<quint32>(plain.data() + sizeof(quint16));
        plain = plain.sliced(RecordHeaderSize);
        const auto keySize = qsizetype(projectSize) + nameSize;
        if (nameSize == 0 || nameSize > MaxNameSize || valueSize > MaxValueSize
            || plain.size() < keySize + valueSize) return false;
        const auto project = plain.first(projectSize);
        const auto name = plain.sliced(projectSize, nameSize);
        auto key = encodeKey(QString::fromUtf8(project), QString::fromUtf8(name));
        if (key.project != project || key.name != name
            || !entries.emplace(std::move(key), plain.sliced(keySize, valueSize)).second)
            return false;
        plain = plain.sliced(keySize + valueSize);
    }
    return true;
}

QString openCollection(const QByteArray& file, const unsigned char* source, bool agent, Collection& collection) {
    if (file.size() < PrefixSize || !file.startsWith(Magic)) return Corrupt;
    const char active = file[Magic.size()];
    if (active != 0 && active != 1) return Corrupt;
    const qsizetype headerSize = PrefixSize + (active + 1) * WrapSize;
    if (file.size() < headerSize + NonceSize + TagSize
        || file.size() - headerSize - NonceSize - TagSize > MaxSnapshotBytes) return Corrupt;
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

QByteArray newHeader(const SecureKey& owner, QByteArrayView agent, SecureKey& key) {
    SecureKey kek;
    randombytes_buf(key.data(), SecureKey::Size);
    if (!deriveKek(kek, owner.data(), false)) return {};
    crypto::Blob wrap;
    if (!crypto::seal(kek, OwnerAad, key.data(), SecureKey::Size, wrap)) return {};
    QByteArray header(Magic.data(), Magic.size());
    header.append(char(!agent.isEmpty()));
    header = appendBlob(std::move(header), wrap);
    if (!agent.isEmpty()) {
        if (!deriveKek(kek, reinterpret_cast<const unsigned char*>(agent.data()), true)
            || !crypto::seal(kek, AgentAad, key.data(), SecureKey::Size, wrap)) return {};
        header = appendBlob(std::move(header), wrap);
    }
    return header;
}

QByteArray rotate(const SecureKey& owner, QByteArrayView agent, QByteArrayView plain) {
    SecureKey key;
    const auto header = newHeader(owner, agent, key);
    return header.isEmpty() ? QByteArray() : sealBody(header, key, plain);
}

QString saveRotation(LockedFile& file, const SecureKey& owner, QByteArrayView agent, QByteArrayView plain) {
    const auto bytes = rotate(owner, agent, plain);
    return bytes.isEmpty() ? StorageError : file.save(bytes);
}

QString saveEntries(LockedFile& file, const Collection& collection) {
    qsizetype size = 0;
    for (const auto& [key, value] : collection.entries) {
        size += RecordHeaderSize + key.name.size() + value.size();
        if (!key.project.isEmpty()) size += ProjectHeaderSize + key.project.size();
    }
    if (size > MaxSnapshotBytes) return LimitExceeded;
    cbor::SecureBuffer plain(size);
    auto* out = plain.data();
    for (const auto& [key, value] : collection.entries) {
        if (!key.project.isEmpty()) {
            qToBigEndian<quint16>(0, out);
            qToBigEndian<quint16>(quint16(key.project.size()), out + sizeof(quint16));
            out += ProjectHeaderSize;
        }
        qToBigEndian<quint16>(quint16(key.name.size()), out);
        qToBigEndian<quint32>(quint32(value.size()), out + sizeof(quint16));
        out += RecordHeaderSize;
        if (!key.project.isEmpty()) {
            std::memcpy(out, key.project.constData(), key.project.size());
            out += key.project.size();
        }
        std::memcpy(out, key.name.constData(), key.name.size());
        out += key.name.size();
        if (!value.isEmpty()) std::memcpy(out, value.data(), value.size());
        out += value.size();
    }
    return saveBody(file, collection.header, collection.key,
                    QByteArrayView(reinterpret_cast<const char*>(plain.data()), size));
}

QString addEntry(LockedFile& file, Collection& collection, const EntryKey& key, QByteArrayView value) {
    if (collection.entries.contains(key)) return AlreadyExists;
    if (collection.entries.size() == MaxEntries) return LimitExceeded;
    collection.entries.emplace(key, value);
    return saveEntries(file, collection);
}

QString changeEntry(LockedFile& file, Collection& collection, Entries::iterator entry,
                    const EntryKey* target, QByteArrayView value) {
    if (!target) {
        collection.entries.erase(entry);
    } else if (*target == entry->first) {
        if (value == entry->second) return {};
        entry->second = value;
    } else {
        if (!collection.entries.emplace(*target, value).second) return AlreadyExists;
        collection.entries.erase(entry);
    }
    return saveEntries(file, collection);
}

QString editEntry(const QString& dir, const SecureKey& owner, const EntryKey& key,
                  QByteArrayView original, const EntryKey* target, QByteArrayView value) {
    if (key.name.isEmpty() || (target && target->name.isEmpty()) || value.size() > MaxValueSize)
        return InvalidRequest;
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(false, Conflict); !error.isEmpty()) return error;
    Collection collection;
    if (const auto error = openCollection(file.bytes, owner.data(), false, collection); !error.isEmpty())
        return error;
    const auto entry = collection.entries.find(key);
    if (entry == collection.entries.end() || entry->second != original) return Conflict;
    return changeEntry(file, collection, entry, target, value);
}

void entryNames(const Entries& entries, const QByteArray& project, QStringList& names) {
    for (auto entry = entries.lower_bound({project, {}});
         entry != entries.end() && entry->first.project == project; ++entry)
        names.append(QString::fromUtf8(entry->first.name));
}

QString openAgent(LockedFile& file, QByteArrayView token, Collection& collection) {
    const auto rawToken = crypto::parseAgentToken(token);
    if (rawToken.isEmpty()) return Unauthorized;
    if (const auto error = file.open(false, Unauthorized); !error.isEmpty()) return error;
    return openCollection(file.bytes, reinterpret_cast<const unsigned char*>(rawToken.bytes().constData()),
                          true, collection);
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
    return ownerCreate(dir, owner, {}, name, value);
}

QString ownerCreate(const QString& dir, const SecureKey& owner, const QString& project, const QString& name,
                    QByteArrayView value) {
    const auto key = encodeKey(project, name);
    if (key.name.isEmpty() || value.size() > MaxValueSize) return InvalidRequest;
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(true, {}); !error.isEmpty()) return error;
    Collection collection;
    if (file.bytes.isNull()) {
        collection.header = newHeader(owner, {}, collection.key);
        if (collection.header.isEmpty()) return StorageError;
    } else if (const auto error = openCollection(file.bytes, owner.data(), false, collection); !error.isEmpty()) {
        return error;
    }
    return addEntry(file, collection, key, value);
}

QString ownerList(const QString& dir, const SecureKey& owner, QStringList& names) {
    return ownerList(dir, owner, {}, names);
}

QString ownerList(const QString& dir, const SecureKey& owner, const QString& project, QStringList& names) {
    names.clear();
    const auto encodedProject = encodeName(project);
    if (!project.isEmpty() && encodedProject.isEmpty()) return InvalidRequest;
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(false, NotFound); !error.isEmpty())
        return error == NotFound ? QString() : error;
    Collection collection;
    if (const auto error = openCollection(file.bytes, owner.data(), false, collection); !error.isEmpty())
        return error;
    entryNames(collection.entries, encodedProject, names);
    return {};
}

QString ownerTree(const QString& dir, const SecureKey& owner, QMap<QString, QStringList>& projects) {
    projects.clear();
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(false, NotFound); !error.isEmpty())
        return error == NotFound ? QString() : error;
    Collection collection;
    if (const auto error = openCollection(file.bytes, owner.data(), false, collection); !error.isEmpty())
        return error;
    for (const auto& record : collection.entries) {
        const auto& [project, name] = record.first;
        projects[QString::fromUtf8(project)].append(QString::fromUtf8(name));
    }
    return {};
}

QString ownerRead(const QString& dir, const SecureKey& owner, const QString& name, SecureBytes& value) {
    return ownerRead(dir, owner, {}, name, value);
}

QString ownerRead(const QString& dir, const SecureKey& owner, const QString& project, const QString& name,
                  SecureBytes& value) {
    value.clear();
    const auto key = encodeKey(project, name);
    if (key.name.isEmpty()) return InvalidRequest;
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(false, NotFound); !error.isEmpty()) return error;
    Collection collection;
    if (const auto error = openCollection(file.bytes, owner.data(), false, collection); !error.isEmpty())
        return error;
    const auto entry = collection.entries.find(key);
    if (entry == collection.entries.end()) return NotFound;
    value = SecureBytes(entry->second.toByteArray());
    return {};
}

QString ownerUpdate(const QString& dir, const SecureKey& owner, const QString& name,
                    QByteArrayView original, const QString& nextName, QByteArrayView value) {
    return ownerUpdate(dir, owner, {}, name, original, {}, nextName, value);
}

QString ownerUpdate(const QString& dir, const SecureKey& owner, const QString& project, const QString& name,
                    QByteArrayView original, const QString& nextProject, const QString& nextName, QByteArrayView value) {
    const auto target = encodeKey(nextProject, nextName);
    return editEntry(dir, owner, encodeKey(project, name), original, &target, value);
}

QString ownerDelete(const QString& dir, const SecureKey& owner, const QString& name, QByteArrayView original) {
    return ownerDelete(dir, owner, {}, name, original);
}

QString ownerDelete(const QString& dir, const SecureKey& owner, const QString& project, const QString& name,
                    QByteArrayView original) {
    return editEntry(dir, owner, encodeKey(project, name), original, nullptr, {});
}

QString ownerSnapshot(const QString& dir, const SecureKey& owner, SecureBytes& plain) {
    plain.clear();
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(false, NotFound, true); !error.isEmpty())
        return error == NotFound ? QString() : error;
    Collection collection;
    if (const auto error = openCollection(file.bytes, owner.data(), false, collection); !error.isEmpty())
        return error;
    plain = std::move(collection.plain);
    return {};
}

QString restoreSnapshot(const QString& dir, const SecureKey* oldOwner, const SecureKey& newOwner,
                        QByteArrayView previousVault, QByteArrayView nextVault, QByteArrayView plain,
                        const std::function<void()>& commitVault) {
    Entries records;
    if (plain.size() > MaxSnapshotBytes || !readEntries(plain, records)) return Corrupt;
    if (previousVault.isEmpty() != (oldOwner == nullptr) || !crypto::init()) return StorageError;
    LockedFile file(dir);
    if (const auto error = file.open(true, {}); !error.isEmpty()) return error;
    if (!file.bytes.isNull()) {
        if (!oldOwner) return StorageError;
        Collection collection;
        if (const auto error = openCollection(file.bytes, oldOwner->data(), false, collection); !error.isEmpty())
            return error;
    }
    const auto next = rotate(newOwner, {}, plain);
    if (next.isEmpty()) return StorageError;
    const auto oldId = oldOwner ? container::keyHeaderFingerprint(previousVault) : AbsentVaultId;
    return file.replaceCollection(oldId, container::keyHeaderFingerprint(nextVault), next, commitVault);
}

QString execute(const QString& dir, QByteArrayView token, const QString& op,
                const QString& name, QByteArrayView value, SecureBytes& result, const QString& project) {
    result.clear();
    const bool read = op == QStringLiteral("read");
    const bool create = op == QStringLiteral("create");
    const auto key = encodeKey(project, name);
    if ((!read && !create && op != QStringLiteral("replace")) || key.name.isEmpty()
        || value.size() > MaxValueSize || (read && !value.isEmpty())) return InvalidRequest;
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    Collection collection;
    if (const auto error = openAgent(file, token, collection); !error.isEmpty()) return error;
    if (create) return addEntry(file, collection, key, value);
    const auto entry = collection.entries.find(key);
    if (entry == collection.entries.end()) return NotFound;
    if (read) {
        result = SecureBytes(entry->second.toByteArray());
        return {};
    }
    entry->second = value;
    return saveEntries(file, collection);
}

QString agentListProjects(const QString& dir, QByteArrayView token, QStringList& projects) {
    projects.clear();
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    Collection collection;
    if (const auto error = openAgent(file, token, collection); !error.isEmpty()) return error;
    QByteArray previous;
    for (const auto& record : collection.entries) {
        const auto& project = record.first.project;
        if (projects.isEmpty() || project != previous) {
            projects.append(QString::fromUtf8(project));
            previous = project;
        }
    }
    return {};
}

QString agentList(const QString& dir, QByteArrayView token, const QString& project, QStringList& names) {
    names.clear();
    const auto encodedProject = encodeName(project);
    if (!project.isEmpty() && encodedProject.isEmpty()) return InvalidRequest;
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    Collection collection;
    if (const auto error = openAgent(file, token, collection); !error.isEmpty()) return error;
    entryNames(collection.entries, encodedProject, names);
    return {};
}

QString agentMove(const QString& dir, QByteArrayView token, const QString& project, const QString& name,
                  const QString& nextProject, const QString& nextName) {
    const auto key = encodeKey(project, name);
    const auto target = encodeKey(nextProject, nextName);
    if (key.name.isEmpty() || target.name.isEmpty()) return InvalidRequest;
    if (!crypto::init()) return StorageError;
    LockedFile file(dir);
    Collection collection;
    if (const auto error = openAgent(file, token, collection); !error.isEmpty()) return error;
    const auto entry = collection.entries.find(key);
    if (entry == collection.entries.end()) return NotFound;
    return changeEntry(file, collection, entry, &target, entry->second);
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
    Collection collection;
    if (const auto error = openCollection(file.bytes, oldOwner.data(), false, collection); !error.isEmpty())
        return error;
    const auto next = rotate(newOwner, {}, collection.plain.bytes());
    if (next.isEmpty()) return StorageError;
    return file.replaceCollection(container::keyHeaderFingerprint(previousVault),
                                  container::keyHeaderFingerprint(nextVault), next, commitVault);
}
} // namespace tb::collection
