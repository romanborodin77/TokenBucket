#include "Application.h"
#include "../vault/Container.h"
#include "../vault/Crypto.h"
#include <QEventLoop>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <stdexcept>
namespace tb {
namespace {
template<class T> T waitFor(QFuture<T> future) {
    QEventLoop loop;
    QFutureWatcher<T> watcher;
    QObject::connect(&watcher, &QFutureWatcher<T>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    if (!future.isFinished()) loop.exec();
    return future.result();
}
bool fail(QString* err, const char* text) { if (err) *err = QString::fromUtf8(text); return false; }
}
struct Application::Outcome {
    bool ok = false;
    QString error;
    QByteArray file;
    model::VaultModel model;
    std::unique_ptr<SecureKey> key;
    bool open(container::Opened opened) {
        SecureBytes plain(std::move(opened.cbor));
        if (opened.status != container::Status::Ok) {
            error = QStringLiteral("Неверный способ разблокировки, пароль или повреждённый файл."); return false;
        }
        if (model.fromCbor(plain.bytes(), &error) != model::VaultModel::ParseStatus::Ok) return false;
        key = std::make_unique<SecureKey>(std::move(opened.dataKey));
        return ok = true;
    }
};
#ifdef Q_OS_WIN
Application::Application(const QString& dir, QObject* parent, HelloRequest hello)
    : QObject(parent), store_(dir), hello_(std::move(hello)) {
#else
Application::Application(const QString& dir, QObject* parent) : QObject(parent), store_(dir) {
#endif
    if (!crypto::init()) throw std::runtime_error("Не удалось инициализировать криптографию.");
    const auto initial = waitFor(QtConcurrent::run([this] {
        QByteArray bytes;
        const auto loaded = store_.loadBytes(store_.mainPath(), &bytes);
        return std::pair{loaded != store::VaultStore::Load::Missing, container::readMode(bytes)};
    }));
    hasVault_ = initial.first;
    gate_ = hasVault_ ? Gate::NeedUnlock : Gate::NeedSetup;
    hint_ = initial.second == container::ModeHello ? Mode::Hello : Mode::Password;
}
bool Application::validPassword(const QByteArray& pw) { return pw.size() >= 12 && pw.size() <= 1024 && pw.isValidUtf8(); }
std::unique_ptr<QLockFile> Application::acquireInstanceLock(const QString& dir, QString* err) {
    if (err) err->clear();
    if (!store::ensurePrivateDir(dir)) { fail(err, "Не удалось ограничить доступ к каталогу данных."); return {}; }
    auto lock = std::make_unique<QLockFile>(dir + "/instance.lock");
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) { fail(err, "Приложение уже запущено или файл блокировки недоступен."); return {}; }
    return lock;
}
bool Application::canStart(Gate expected, QString* err) const {
    if (err) err->clear();
    if (busy_) return fail(err, "Дождитесь завершения текущей операции.");
    if (gate_ != expected) return fail(err, "Операция недоступна в текущем состоянии хранилища.");
    return true;
}
bool Application::execute(std::function<std::shared_ptr<Outcome>(quint64)> job, Mode mode, bool adopt, QString* err) {
    const quint64 generation = generation_;
    busy_ = true; emit busyChanged(true);
    std::shared_ptr<Outcome> result;
    try { result = waitFor(QtConcurrent::run([this, generation, job = std::move(job)] {
        if (generation != generation_) return std::make_shared<Outcome>();
        return job(generation);
    })); }
    catch (...) { result = std::make_shared<Outcome>(); result->error = QStringLiteral("Не удалось выполнить операцию хранилища."); }
    bool ok = result->ok;
    const bool changed = ok && !result->file.isEmpty();
    if (ok && !result->file.isEmpty()) { hasVault_ = true; hint_ = mode; }
    if (generation != generation_) {
        ok = false; result->error = QStringLiteral("Операция отменена блокировкой хранилища.");
        gate_ = hasVault_ ? Gate::NeedUnlock : Gate::NeedSetup;
    } else if (ok) {
        if (!result->file.isEmpty()) file_ = std::move(result->file);
        if (adopt) { model_ = std::move(result->model); dataKey_ = std::move(result->key); mode_ = mode; gate_ = Gate::Ready; }
    }
    if (err) *err = ok ? QString() : result->error;
    result.reset();
    busy_ = false; emit busyChanged(false);
    if (changed || generation != generation_) emit stateChanged();
    return ok;
}
bool Application::createPasswordVault(const QByteArray& pw, QString* err) {
    if (!canStart(Gate::NeedSetup, err)) return false;
    if (!validPassword(pw)) return fail(err, "Пароль: минимум 12 и максимум 1024 байта UTF-8.");
    auto secret = std::make_shared<SecureBytes>(pw);
    return execute([this, secret](quint64 expected) {
        auto result = std::make_shared<Outcome>();
        result->key = std::make_unique<SecureKey>();
        result->file = container::packPassword(secret->bytes(), result->model.toCbor(), result->key.get());
        secret->clear();
        if (generation() != expected) return result;
        result->ok = !result->file.isEmpty() && store_.save(result->file, &result->error);
        if (!result->ok && result->error.isEmpty()) result->error = QStringLiteral("Не удалось создать хранилище.");
        return result;
    }, Mode::Password, true, err);
}
bool Application::unlock(const QByteArray& pw, QString* err) {
    if (!canStart(Gate::NeedUnlock, err)) return false;
    if (!validPassword(pw)) return fail(err, "Пароль: минимум 12 и максимум 1024 байта UTF-8.");
    auto secret = std::make_shared<SecureBytes>(pw);
    return execute([this, secret](quint64) {
        auto result = std::make_shared<Outcome>();
        if (store_.loadBytes(store_.mainPath(), &result->file) != store::VaultStore::Load::Ok)
            result->error = QStringLiteral("Не удалось прочитать файл хранилища.");
        else if (result->open(container::openPassword(result->file, secret->bytes()))) store_.acceptVerified(result->file);
        secret->clear();
        return result;
    }, Mode::Password, true, err);
}
bool Application::save(QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    auto plain = std::make_shared<SecureBytes>(model_.toCbor());
    if (plain->size() > container::MaxPlainBytes) return fail(err, "Содержимое хранилища превышает 64 МиБ.");
    auto key = std::make_shared<SecureKey>(); memcpy(key->data(), dataKey_->data(), SecureKey::Size);
    const auto file = file_;
    return execute([this, plain, key, file](quint64 expected) {
        auto result = std::make_shared<Outcome>();
        result->file = container::reseal(file, *key, plain->bytes());
        plain->clear();
        if (generation() != expected) return result;
        result->ok = !result->file.isEmpty() && store_.save(result->file, &result->error);
        if (!result->ok && result->error.isEmpty()) result->error = QStringLiteral("Не удалось сохранить хранилище.");
        return result;
    }, mode_, false, err);
}
void Application::lock() {
    model_.clear(); dataKey_.reset(); file_.clear(); mode_ = Mode::None;
    ++generation_;
    gate_ = hasVault_ ? Gate::NeedUnlock : Gate::NeedSetup;
    emit stateChanged();
}
QByteArray Application::exportContainer(const QByteArray& exportPw, QString* err) {
    if (!canStart(Gate::Ready, err)) return {};
    if (!validPassword(exportPw)) { fail(err, "Пароль экспорта: минимум 12 и максимум 1024 байта UTF-8."); return {}; }
    auto plain = std::make_shared<SecureBytes>(model_.toCbor());
    if (plain->size() > container::MaxPlainBytes) { fail(err, "Содержимое хранилища превышает 64 МиБ."); return {}; }
    const quint64 expected = generation_;
    auto secret = std::make_shared<SecureBytes>(exportPw);
    busy_ = true; emit busyChanged(true);
    QString error; QByteArray exported;
    try {
        exported = waitFor(QtConcurrent::run([this, expected, plain, secret]() -> QByteArray {
            if (expected != generation_) return {};
            const QByteArray bytes = container::packPassword(secret->bytes(), plain->bytes(), nullptr);
            return expected == generation_ ? bytes : QByteArray();
        }));
        if (expected != generation_) { exported.clear(); error = QStringLiteral("Операция отменена блокировкой хранилища."); }
        else if (exported.isEmpty()) error = QStringLiteral("Не удалось создать файл экспорта.");
    }
    catch (...) { exported.clear(); error = QStringLiteral("Не удалось создать файл экспорта."); }
    plain->clear(); secret->clear();
    if (err) *err = error;
    const bool canceled = expected != generation_;
    busy_ = false; emit busyChanged(false);
    if (canceled) emit stateChanged();
    return exported;
}
bool Application::importContainer(const QByteArray& bytes, const QByteArray& importPw, const QByteArray& newLocalPw, QString* err) {
    if (err) err->clear();
    if (busy_) return fail(err, "Дождитесь завершения текущей операции.");
    if (gate_ == Gate::NeedUnlock) return fail(err, "Сначала разблокируйте или закройте хранилище.");
    if (!validPassword(importPw)) return fail(err, "Пароль файла импорта: минимум 12 и максимум 1024 байта UTF-8.");
    if (!validPassword(newLocalPw)) return fail(err, "Новый мастер-пароль: минимум 12 и максимум 1024 байта UTF-8.");
    if (bytes.isEmpty() || bytes.size() > int(container::MaxFileBytes)) return fail(err, "Файл импорта отсутствует, повреждён или слишком велик.");
    auto source = std::make_shared<SecureBytes>(bytes);
    auto fileSecret = std::make_shared<SecureBytes>(importPw);
    auto localSecret = std::make_shared<SecureBytes>(newLocalPw);
    return execute([this, source, fileSecret, localSecret](quint64 expected) {
        auto result = std::make_shared<Outcome>();
        auto opened = container::openPassword(source->bytes(), fileSecret->bytes());
        SecureBytes plain(std::move(opened.cbor));
        if (opened.status != container::Status::Ok) { result->error = QStringLiteral("Неверный пароль файла импорта или файл повреждён."); return result; }
        if (result->model.fromCbor(plain.bytes(), &result->error) != model::VaultModel::ParseStatus::Ok) return result;
        plain.clear();
        if (generation() != expected) return result;
        result->key = std::make_unique<SecureKey>();
        SecureBytes cbor(result->model.toCbor());
        result->file = cbor.size() > container::MaxPlainBytes ? QByteArray() : container::packPassword(localSecret->bytes(), cbor.bytes(), result->key.get());
        cbor.clear();
        if (generation() != expected) return result;
        result->ok = !result->file.isEmpty() && store_.save(result->file, &result->error);
        if (!result->ok && result->error.isEmpty()) result->error = QStringLiteral("Не удалось импортировать хранилище.");
        return result;
    }, Mode::Password, true, err);
}
bool Application::switchToPassword(const QByteArray& newPw, QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    if (!validPassword(newPw)) return fail(err, "Новый мастер-пароль: минимум 12 и максимум 1024 байта UTF-8.");
    auto snapshot = std::make_shared<model::VaultModel>(model_);
    auto secret = std::make_shared<SecureBytes>(newPw);
    return execute([this, snapshot, secret](quint64 expected) {
        auto result = std::make_shared<Outcome>();
        result->model = *snapshot;
        result->key = std::make_unique<SecureKey>();
        SecureBytes cbor(result->model.toCbor());
        if (cbor.size() > container::MaxPlainBytes) { result->error = QStringLiteral("Содержимое хранилища превышает 64 МиБ."); return result; }
        result->file = container::packPassword(secret->bytes(), cbor.bytes(), result->key.get());
        cbor.clear();
        if (generation() != expected) return result;
        result->ok = !result->file.isEmpty() && store_.save(result->file, &result->error);
        if (!result->ok && result->error.isEmpty()) result->error = QStringLiteral("Не удалось сменить режим разблокировки.");
        return result;
    }, Mode::Password, true, err);
}
#ifdef Q_OS_WIN
bool Application::switchToHello(QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    auto snapshot = std::make_shared<model::VaultModel>(model_);
    return execute([this, snapshot](quint64 expected) {
        auto result = std::make_shared<Outcome>();
        result->model = *snapshot;
        result->key = std::make_unique<SecureKey>();
        SecureBytes cbor(result->model.toCbor());
        if (cbor.size() > container::MaxPlainBytes) { result->error = QStringLiteral("Содержимое хранилища превышает 64 МиБ."); return result; }
        result->file = container::packHello(cbor.bytes(), result->key.get());
        cbor.clear();
        if (generation() != expected) return result;
        result->ok = !result->file.isEmpty() && store_.save(result->file, &result->error);
        if (!result->ok && result->error.isEmpty()) result->error = QStringLiteral("Не удалось сменить режим разблокировки.");
        return result;
    }, Mode::Hello, true, err);
}
#endif
bool Application::checkPassword(const QByteArray& pw, QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    if (mode_ != Mode::Password || !validPassword(pw)) return fail(err, "Неверный мастер-пароль.");
    auto secret = std::make_shared<SecureBytes>(pw); const auto file = file_;
    auto key = std::make_shared<SecureKey>(); memcpy(key->data(), dataKey_->data(), SecureKey::Size);
    return execute([this, secret, file, key](quint64) {
        auto result = std::make_shared<Outcome>(); QByteArray current;
        const auto loaded = store_.loadBytes(store_.mainPath(), &current);
        if (loaded == store::VaultStore::Load::Ok && current != file)
            result->error = QStringLiteral("Файл хранилища изменился вне приложения. Закройте приложение и откройте хранилище заново.");
        else if (loaded == store::VaultStore::Load::Ok) {
            auto opened = container::openPassword(current, secret->bytes()); SecureBytes plain(std::move(opened.cbor));
            result->ok = opened.status == container::Status::Ok && opened.dataKey == *key;
            if (!result->ok) result->error = QStringLiteral("Неверный мастер-пароль.");
        } else result->error = QStringLiteral("Не удалось прочитать файл хранилища. Откройте хранилище заново.");
        secret->clear(); return result;
    }, Mode::Password, false, err);
}
#ifdef Q_OS_WIN
bool Application::verifyHello(void* hwnd, QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    if (mode_ != Mode::Hello) return fail(err, "Хранилище не использует Windows Hello.");
    return execute([this, hwnd](quint64) {
        auto result = std::make_shared<Outcome>(); result->ok = hello_(hwnd, QStringLiteral("Подтвердите доступ к секрету")) == platform::hello::Result::Verified;
        if (!result->ok) result->error = QStringLiteral("Проверка Windows Hello не выполнена."); return result;
    }, Mode::Hello, false, err);
}
bool Application::createHelloVault(QString* err, void* hwnd) {
    if (!canStart(Gate::NeedSetup, err)) return false;
    return execute([this, hwnd](quint64 expected) {
        auto result = std::make_shared<Outcome>();
        if (hello_(hwnd, QStringLiteral("Создание хранилища TokenBucket")) != platform::hello::Result::Verified) {
            result->error = QStringLiteral("Проверка Windows Hello не выполнена."); return result;
        }
        if (generation() != expected) return result;
        result->key = std::make_unique<SecureKey>();
        result->file = container::packHello(result->model.toCbor(), result->key.get());
        if (generation() != expected) return result;
        result->ok = !result->file.isEmpty() && store_.save(result->file, &result->error);
        if (!result->ok && result->error.isEmpty()) result->error = QStringLiteral("Не удалось создать хранилище Windows Hello.");
        return result;
    }, Mode::Hello, true, err);
}
bool Application::unlockHello(QString* err, void* hwnd) {
    if (!canStart(Gate::NeedUnlock, err)) return false;
    return execute([this, hwnd](quint64 expected) {
        auto result = std::make_shared<Outcome>();
        if (hello_(hwnd, QStringLiteral("Разблокировка TokenBucket")) != platform::hello::Result::Verified) {
            result->error = QStringLiteral("Проверка Windows Hello не выполнена."); return result;
        }
        if (generation() != expected) return result;
        if (store_.loadBytes(store_.mainPath(), &result->file) != store::VaultStore::Load::Ok)
            result->error = QStringLiteral("Не удалось прочитать файл хранилища.");
        else if (result->open(container::openHello(result->file))) store_.acceptVerified(result->file);
        return result;
    }, Mode::Hello, true, err);
}
#endif
}
