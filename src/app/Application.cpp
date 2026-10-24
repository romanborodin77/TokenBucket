#include "Application.h"
#include "../vault/Container.h"
#include "../vault/Crypto.h"
#include <QEventLoop>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QFileInfo>
#include <QScopeGuard>
#include <stdexcept>
namespace tb {
Application::BackupInfo Application::backupInfo() const {
    BackupInfo result; result.mainPath=store_.mainPath(); result.backupPath=store_.backupPath();
    const QFileInfo info(result.backupPath);
    if(info.isSymLink() || (info.exists() && (!info.isFile() || !info.isReadable() || info.size()>container::MaxFileBytes)))
        result.state=BackupInfo::State::Unavailable;
    else if(info.exists()) {
        result.state=BackupInfo::State::Available; result.size=info.size(); result.modified=info.lastModified();
    }
    return result;
}
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
    bool wroteFile=false,legacyCurrent=false,upgradeApproved=false;
    QByteArray previousFile;
    QString safetyPath;
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
Application::Application(const QString& dir, QObject* parent, HelloRequest hello,store::FileOps& ops)
    : QObject(parent), store_(dir,ops), hello_(std::move(hello)) {
#else
Application::Application(const QString& dir, QObject* parent,store::FileOps& ops) : QObject(parent), store_(dir,ops) {
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
Application::Mode Application::backupMode(QString* error) {
    if(!canStart(Gate::Ready,error)) return Mode::None;
    cancelBackupRestore();
    const auto expected=generation(),request=backupRequest_.load(); const auto current=file_;
    struct Header {PreparedBackup snapshot; QString error;};
    auto result=std::make_shared<Header>();
    busy_=true;
    const auto done=qScopeGuard([&] {busy_=false; emit busyChanged(false);});
    emit busyChanged(true);
    try {waitFor(QtConcurrent::run([this,result,current,expected,request] {
        auto& snapshot=result->snapshot;
        if(store_.loadBytes(store_.mainPath(),&snapshot.main)!=store::VaultStore::Load::Ok || snapshot.main!=current) {
            result->error=QStringLiteral("Основной файл изменился или недоступен. Откройте хранилище заново."); return false;
        }
        if(store_.loadBytes(store_.backupPath(),&snapshot.backup)!=store::VaultStore::Load::Ok) {
            result->error=QStringLiteral("Резервная копия недоступна."); return false;
        }
        const auto mode=container::readMode(snapshot.backup);
        if(mode==container::ModePassword) snapshot.mode=Mode::Password;
        else if(mode==container::ModeHello) snapshot.mode=Mode::Hello;
        else result->error=QStringLiteral("Неподдерживаемый или повреждённый заголовок резервной копии.");
        snapshot.generation=expected; snapshot.request=request;
        return snapshot.mode!=Mode::None;
    }));} catch(...) {result->error=QStringLiteral("Не удалось прочитать резервную копию.");}
    if(generation()!=expected || backupRequest_.load()!=request || gate_!=Gate::Ready) result->error=QStringLiteral("Чтение копии отменено.");
    if(error) *error=result->error;
    if(!result->error.isEmpty()) return Mode::None;
    backupHint_=std::move(result->snapshot);
    return backupHint_->mode;
}
bool Application::prepareBackupRestore(const QByteArray& password,QString* error) {
    return prepareBackup(Mode::Password,password,nullptr,error);
}
#ifdef Q_OS_WIN
bool Application::prepareHelloBackupRestore(void* hwnd,QString* error) {
    return prepareBackup(Mode::Hello,{},hwnd,error);
}
#endif
void Application::cancelBackupRestore() {backupHint_.reset(); preparedBackup_.reset(); ++backupRequest_;}
bool Application::prepareBackup(Mode mode,const QByteArray& password,void* hwnd,QString* error) {
    if(!canStart(Gate::Ready,error)) return false;
    auto hint=std::move(backupHint_);
    cancelBackupRestore();
    if(mode==Mode::Password && !validPassword(password)) return fail(error,"Недопустимый пароль резервной копии.");
    const auto expected=generation(),request=backupRequest_.load(); const auto current=file_;
    struct Preparation {PreparedBackup snapshot; QString error; bool ok=false;};
    auto result=std::make_shared<Preparation>(); auto secret=std::make_shared<SecureBytes>(password);
    busy_=true;
    const auto done=qScopeGuard([&] {secret->clear(); busy_=false; emit busyChanged(false);});
    emit busyChanged(true);
    try {waitFor(QtConcurrent::run([this,result,secret,current,expected,request,mode,hwnd,hint=std::move(hint)] {
        const auto canceled=[&] {return generation()!=expected || backupRequest_.load()!=request;};
        auto& snapshot=result->snapshot;
        if(canceled()) return false;
        if(store_.loadBytes(store_.mainPath(),&snapshot.main)!=store::VaultStore::Load::Ok || snapshot.main!=current) {
            result->error=QStringLiteral("Основной файл изменился или недоступен. Откройте хранилище заново."); return false;
        }
        if(store_.loadBytes(store_.backupPath(),&snapshot.backup)!=store::VaultStore::Load::Ok) {
            result->error=QStringLiteral("Резервная копия недоступна."); return false;
        }
        if(hint && (hint->main!=snapshot.main || hint->backup!=snapshot.backup)) {
            result->error=QStringLiteral("Выбранная резервная копия или основной файл изменились. Повторите действие."); return false;
        }
        const auto actual=container::readMode(snapshot.backup);
        if(actual!=(mode==Mode::Password?container::ModePassword:container::ModeHello)) {
            result->error=QStringLiteral("Способ разблокировки копии изменился или её заголовок повреждён. Повторите действие."); return false;
        }
#ifdef Q_OS_WIN
        if(mode==Mode::Hello && hello_(hwnd,QStringLiteral("Подтвердите восстановление резервной копии TokenBucket"))!=platform::hello::Result::Verified) {
            result->error=QStringLiteral("Проверка Windows Hello не выполнена."); return false;
        }
#else
        Q_UNUSED(hwnd);
        if(mode==Mode::Hello) {result->error=QStringLiteral("Копия Windows Hello требует Windows."); return false;}
#endif
        if(canceled()) return false;
        Outcome validation;
        bool opened=false;
        if(mode==Mode::Password) opened=validation.open(container::openPassword(snapshot.backup,secret->bytes()));
#ifdef Q_OS_WIN
        else opened=validation.open(container::openHello(snapshot.backup));
#endif
        secret->clear();
        if(!opened) {result->error=validation.error; return false;}
        snapshot.mode=mode; snapshot.generation=expected; snapshot.request=request;
        result->ok=!canceled(); return result->ok;
    }));} catch(...) {result->error=QStringLiteral("Не удалось проверить резервную копию.");}
    if(generation()!=expected || backupRequest_.load()!=request || gate_!=Gate::Ready)
        result->error=QStringLiteral("Подготовка восстановления отменена.");
    if(!result->ok || !result->error.isEmpty()) {
        if(error) *error=result->error.isEmpty()?QStringLiteral("Подготовка восстановления отменена."):result->error;
        return false;
    }
    preparedBackup_=std::move(result->snapshot);
    if(error) error->clear(); return true;
}
store::RestoreResult Application::restorePreparedBackup() {
    QString error;
    if(!canStart(Gate::Ready,&error)) return {false,{},error};
    if(!preparedBackup_) return {false,{},QStringLiteral("Сначала проверьте резервную копию.")};
    auto prepared=std::move(*preparedBackup_); preparedBackup_.reset();
    if(prepared.generation!=generation() || prepared.request!=backupRequest_.load() || prepared.mode==Mode::None)
        return {false,{},QStringLiteral("Подготовка восстановления устарела.")};
    // Состояние commit живёт вне future и сохраняется при поздней ошибке ожидания.
    auto result=std::make_shared<store::RestoreResult>();
    busy_=true;
    const auto done=qScopeGuard([&] {busy_=false; emit busyChanged(false);});
    emit busyChanged(true);
    try {waitFor(QtConcurrent::run([this,prepared,result] {
        *result=store_.restoreBackup(prepared.main,prepared.backup,[&] {
            return generation()!=prepared.generation || backupRequest_.load()!=prepared.request;
        });
        return true;
    }));} catch(...) {
        result->error=result->committed?QStringLiteral("Файл восстановлен, но завершить операцию не удалось. Сеанс будет заблокирован.")
            :QStringLiteral("Не удалось выполнить восстановление.");
    }
    const bool interrupted=generation()!=prepared.generation || backupRequest_.load()!=prepared.request;
    if(result->committed) {
        hasVault_=true; hint_=prepared.mode; lock();
        if(interrupted && result->error.isEmpty()) result->error=QStringLiteral("Копия восстановлена. Сеанс остаётся заблокированным после отмены.");
    }
    return *result;
}
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
bool Application::schemaUpgradeRequired() const {return gate_==Gate::Ready && persistedVersion_<4;}
bool Application::approveSchemaUpgrade(QString* error) {
    if(!canStart(Gate::Ready,error)) return false;
    if(schemaUpgradeRequired()) upgradeApproval_=generation();
    return true;
}
QString Application::preserveBeforeRegistryMigration(QString* error) {
    if(!canStart(Gate::Ready,error)) return {};
    auto path=std::make_shared<QString>();
    const bool ok=execute([this,path](Outcome& result,quint64) {
        *path=store_.preserveBeforeRegistryMigration(result.previousFile,&result.error); result.ok=!path->isEmpty();
    },mode_,false,error);
    return ok?*path:QString();
}
bool Application::storeCandidate(Outcome& result,quint64 expected) {
    const auto canceled=[&] {return generation()!=expected;};
    if(canceled()) return fail(&result.error,"Запись отменена.");
    if(result.legacyCurrent) {
        if(!result.upgradeApproved) return fail(&result.error,"Требуется подтверждение обновления формата до v4.");
        QByteArray safety;
        if(!result.safetyPath.isEmpty() && (store_.loadBytes(result.safetyPath,&safety)!=store::VaultStore::Load::Ok || safety!=result.previousFile))
            result.safetyPath.clear();
        if(result.safetyPath.isEmpty()) result.safetyPath=store_.preserveBeforeUpgrade(result.previousFile,&result.error);
        if(result.safetyPath.isEmpty()) return false;
    }
    if(canceled()) return fail(&result.error,"Запись отменена; страховочная копия сохранена.");
    result.wroteFile=store_.save(result.file,&result.error,canceled);
    return result.wroteFile;
}
bool Application::execute(std::function<void(Outcome&,quint64)> job, Mode mode, bool adopt, QString* err) {
    const quint64 generation = generation_;
    auto result=std::make_shared<Outcome>();
    result->previousFile=file_; result->legacyCurrent=hasVault_ && persistedVersion_<4;
    result->upgradeApproved=upgradeApproval_ && *upgradeApproval_==generation; result->safetyPath=upgradeSafetyPath_;
    busy_ = true; emit busyChanged(true);
    try { waitFor(QtConcurrent::run([this, generation, job = std::move(job),result] {
        if (generation != generation_) return false;
        job(*result,generation); return true;
    })); }
    catch (...) { result->ok=false; result->error = QStringLiteral("Не удалось выполнить операцию хранилища."); }
    bool ok = result->ok;
    const bool changed = result->wroteFile || (ok && !result->file.isEmpty());
    if(!result->safetyPath.isEmpty()) upgradeSafetyPath_=result->safetyPath;
    if(result->wroteFile) {hasVault_=true; hint_=mode; persistedVersion_=4;}
    if (generation != generation_ || (result->wroteFile && !ok)) {
        ok = false;
        result->error = result->wroteFile?QStringLiteral("Файл записан. Операция была прервана после фиксации; сеанс остаётся заблокированным.")
            :QStringLiteral("Операция отменена блокировкой хранилища.");
        if(result->wroteFile) {mode_=Mode::None; lock();}
        gate_ = hasVault_ ? Gate::NeedUnlock : Gate::NeedSetup;
    } else if (ok) {
        if (!result->file.isEmpty()) {file_ = std::move(result->file); hasVault_=true; hint_=mode;}
        if (adopt) {persistedVersion_=result->wroteFile?4:result->model.decodedVersion(); model_ = std::move(result->model); dataKey_ = std::move(result->key); mode_ = mode; gate_ = Gate::Ready;}
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
    return execute([this, secret](Outcome& result,quint64 expected) {
        result.key = std::make_unique<SecureKey>();
        // Секреты сериализуются сразу в защищённый буфер (без обычной кучи) и
        // оттуда уходят в шифратор контейнера.
        cbor::SecureBuffer cbor(result.model.toCborSecure());
        result.file = cbor.size() > container::MaxPlainBytes
            ? QByteArray() : container::packPassword(secret->bytes(), cbor.data(), cbor.size(), result.key.get());
        secret->clear();
        if (generation() != expected) return;
        result.ok = !result.file.isEmpty() && storeCandidate(result,expected);
        if (!result.ok && result.error.isEmpty()) result.error = QStringLiteral("Не удалось создать хранилище.");

    }, Mode::Password, true, err);
}
bool Application::unlock(const QByteArray& pw, QString* err) {
    if (!canStart(Gate::NeedUnlock, err)) return false;
    if (!validPassword(pw)) return fail(err, "Пароль: минимум 12 и максимум 1024 байта UTF-8.");
    auto secret = std::make_shared<SecureBytes>(pw);
    return execute([this, secret](Outcome& result,quint64) {
        if (store_.loadBytes(store_.mainPath(), &result.file) != store::VaultStore::Load::Ok)
            result.error = QStringLiteral("Не удалось прочитать файл хранилища.");
        else if (result.open(container::openPassword(result.file, secret->bytes()))) store_.acceptVerified(result.file);
        secret->clear();

    }, Mode::Password, true, err);
}
bool Application::save(QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    auto plain = std::make_shared<cbor::SecureBuffer>(model_.toCborSecure());
    if (plain->size() > container::MaxPlainBytes) return fail(err, "Содержимое хранилища превышает 64 МиБ.");
    auto key = std::make_shared<SecureKey>(); memcpy(key->data(), dataKey_->data(), SecureKey::Size);
    const auto file = file_;
    return execute([this, plain, key, file](Outcome& result,quint64 expected) mutable {
        result.file = container::reseal(file, *key, plain->data(), plain->size());
        plain.reset(); // освобождает sodium-буфер немедленно, не дожидаясь конца лямбды
        if (generation() != expected) return;
        result.ok = !result.file.isEmpty() && storeCandidate(result,expected);
        if (!result.ok && result.error.isEmpty()) result.error = QStringLiteral("Не удалось сохранить хранилище.");

    }, mode_, false, err);
}
void Application::lock() {
    cancelBackupRestore();
    upgradeApproval_.reset();
    model_.clear(); dataKey_.reset(); file_.clear(); mode_ = Mode::None;
    ++generation_;
    gate_ = hasVault_ ? Gate::NeedUnlock : Gate::NeedSetup;
    emit stateChanged();
}
QByteArray Application::exportContainer(const QByteArray& exportPw, QString* err) {
    if (!canStart(Gate::Ready, err)) return {};
    if (!validPassword(exportPw)) { fail(err, "Пароль экспорта: минимум 12 и максимум 1024 байта UTF-8."); return {}; }
    auto plain = std::make_shared<cbor::SecureBuffer>(model_.toCborSecure());
    if (plain->size() > container::MaxPlainBytes) { fail(err, "Содержимое хранилища превышает 64 МиБ."); return {}; }
    const quint64 expected = generation_;
    auto secret = std::make_shared<SecureBytes>(exportPw);
    busy_ = true; emit busyChanged(true);
    QString error; QByteArray exported;
    try {
        exported = waitFor(QtConcurrent::run([this, expected, plain, secret]() -> QByteArray {
            if (expected != generation_) return {};
            const QByteArray bytes = container::packPassword(secret->bytes(), plain->data(), plain->size(), nullptr);
            return expected == generation_ ? bytes : QByteArray();
        }));
        if (expected != generation_) { exported.clear(); error = QStringLiteral("Операция отменена блокировкой хранилища."); }
        else if (exported.isEmpty()) error = QStringLiteral("Не удалось создать файл экспорта.");
    }
    catch (...) { exported.clear(); error = QStringLiteral("Не удалось создать файл экспорта."); }
    plain.reset(); secret->clear();
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
    return execute([this, source, fileSecret, localSecret](Outcome& result,quint64 expected) {
        auto opened = container::openPassword(source->bytes(), fileSecret->bytes());
        SecureBytes plain(std::move(opened.cbor));
        if (opened.status != container::Status::Ok) { result.error = QStringLiteral("Неверный пароль файла импорта или файл повреждён."); return; }
        if (result.model.fromCbor(plain.bytes(), &result.error) != model::VaultModel::ParseStatus::Ok) return;
        plain.clear();
        if (generation() != expected) return;
        result.key = std::make_unique<SecureKey>();
        cbor::SecureBuffer cbor(result.model.toCborSecure());
        result.file = cbor.size() > container::MaxPlainBytes
            ? QByteArray() : container::packPassword(localSecret->bytes(), cbor.data(), cbor.size(), result.key.get());
        if (generation() != expected) return;
        result.ok = !result.file.isEmpty() && storeCandidate(result,expected);
        if (!result.ok && result.error.isEmpty()) result.error = QStringLiteral("Не удалось импортировать хранилище.");

    }, Mode::Password, true, err);
}
bool Application::switchToPassword(const QByteArray& newPw, QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    if (!validPassword(newPw)) return fail(err, "Новый мастер-пароль: минимум 12 и максимум 1024 байта UTF-8.");
    auto snapshot = std::make_shared<model::VaultModel>(model_);
    auto secret = std::make_shared<SecureBytes>(newPw);
    return execute([this, snapshot, secret](Outcome& result,quint64 expected) {
        result.model = *snapshot;
        result.key = std::make_unique<SecureKey>();
        cbor::SecureBuffer cbor(result.model.toCborSecure());
        if (cbor.size() > container::MaxPlainBytes) { result.error = QStringLiteral("Содержимое хранилища превышает 64 МиБ."); return; }
        result.file = container::packPassword(secret->bytes(), cbor.data(), cbor.size(), result.key.get());
        if (generation() != expected) return;
        result.ok = !result.file.isEmpty() && storeCandidate(result,expected);
        if (!result.ok && result.error.isEmpty()) result.error = QStringLiteral("Не удалось сменить режим разблокировки.");

    }, Mode::Password, true, err);
}
#ifdef Q_OS_WIN
bool Application::switchToHello(QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    auto snapshot = std::make_shared<model::VaultModel>(model_);
    return execute([this, snapshot](Outcome& result,quint64 expected) {
        result.model = *snapshot;
        result.key = std::make_unique<SecureKey>();
        cbor::SecureBuffer cbor(result.model.toCborSecure());
        if (cbor.size() > container::MaxPlainBytes) { result.error = QStringLiteral("Содержимое хранилища превышает 64 МиБ."); return; }
        result.file = container::packHello(cbor.data(), cbor.size(), result.key.get());
        if (generation() != expected) return;
        result.ok = !result.file.isEmpty() && storeCandidate(result,expected);
        if (!result.ok && result.error.isEmpty()) result.error = QStringLiteral("Не удалось сменить режим разблокировки.");

    }, Mode::Hello, true, err);
}
#endif
bool Application::checkPassword(const QByteArray& pw, QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    if (mode_ != Mode::Password || !validPassword(pw)) return fail(err, "Неверный мастер-пароль.");
    auto secret = std::make_shared<SecureBytes>(pw); const auto file = file_;
    auto key = std::make_shared<SecureKey>(); memcpy(key->data(), dataKey_->data(), SecureKey::Size);
    return execute([this, secret, file, key](Outcome& result,quint64) {
         QByteArray current;
        const auto loaded = store_.loadBytes(store_.mainPath(), &current);
        if (loaded == store::VaultStore::Load::Ok && current != file)
            result.error = QStringLiteral("Файл хранилища изменился вне приложения. Закройте приложение и откройте хранилище заново.");
        else if (loaded == store::VaultStore::Load::Ok) {
            auto opened = container::openPassword(current, secret->bytes()); SecureBytes plain(std::move(opened.cbor));
            result.ok = opened.status == container::Status::Ok && opened.dataKey == *key;
            if (!result.ok) result.error = QStringLiteral("Неверный мастер-пароль.");
        } else result.error = QStringLiteral("Не удалось прочитать файл хранилища. Откройте хранилище заново.");
        secret->clear();
    }, Mode::Password, false, err);
}
#ifdef Q_OS_WIN
bool Application::verifyHello(void* hwnd, QString* err) {
    if (!canStart(Gate::Ready, err)) return false;
    if (mode_ != Mode::Hello) return fail(err, "Хранилище не использует Windows Hello.");
    return execute([this, hwnd](Outcome& result,quint64) {
         result.ok = hello_(hwnd, QStringLiteral("Подтвердите доступ к секрету")) == platform::hello::Result::Verified;
        if (!result.ok) result.error = QStringLiteral("Проверка Windows Hello не выполнена.");
    }, Mode::Hello, false, err);
}
bool Application::createHelloVault(QString* err, void* hwnd) {
    if (!canStart(Gate::NeedSetup, err)) return false;
    return execute([this, hwnd](Outcome& result,quint64 expected) {
        if (hello_(hwnd, QStringLiteral("Создание хранилища TokenBucket")) != platform::hello::Result::Verified) {
            result.error = QStringLiteral("Проверка Windows Hello не выполнена."); return;
        }
        if (generation() != expected) return;
        result.key = std::make_unique<SecureKey>();
        cbor::SecureBuffer cbor(result.model.toCborSecure());
        if (cbor.size() > container::MaxPlainBytes) { result.error = QStringLiteral("Содержимое хранилища превышает 64 МиБ."); return; }
        result.file = container::packHello(cbor.data(), cbor.size(), result.key.get());
        if (generation() != expected) return;
        result.ok = !result.file.isEmpty() && storeCandidate(result,expected);
        if (!result.ok && result.error.isEmpty()) result.error = QStringLiteral("Не удалось создать хранилище Windows Hello.");

    }, Mode::Hello, true, err);
}
bool Application::unlockHello(QString* err, void* hwnd) {
    if (!canStart(Gate::NeedUnlock, err)) return false;
    return execute([this, hwnd](Outcome& result,quint64 expected) {
        if (hello_(hwnd, QStringLiteral("Разблокировка TokenBucket")) != platform::hello::Result::Verified) {
            result.error = QStringLiteral("Проверка Windows Hello не выполнена."); return;
        }
        if (generation() != expected) return;
        if (store_.loadBytes(store_.mainPath(), &result.file) != store::VaultStore::Load::Ok)
            result.error = QStringLiteral("Не удалось прочитать файл хранилища.");
        else if (result.open(container::openHello(result.file))) store_.acceptVerified(result.file);

    }, Mode::Hello, true, err);
}
#endif
}
