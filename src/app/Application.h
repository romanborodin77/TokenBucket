#pragma once
#include <QObject>
#include <QLockFile>
#include <functional>
#include <memory>
#include <atomic>
#include <optional>
#include "../model/VaultModel.h"
#include "../vault/SecureBytes.h"
#include "../vault/VaultStore.h"
#ifdef Q_OS_WIN
#include "../platform/hello_win.h"
#endif
namespace tb {
// Ядро приложения: владеет моделью, ключом данных и файлом хранилища; переводит
// интерфейс между состояниями-шлюзами. Долгие операции (создание/разблокировка/
// сохранение/экспорт/смена режима — Argon2id занимает секунды) выполняются через
// execute() в QtConcurrent; на время операции выставляется busy_, а «поколение»
// generation() аннулирует результаты операций, завершившихся после блокировки:
// поздний ответ уже сброшенного хранилища не применяется (защита от гонки
// «пользователь нажал Блокировку, пока Argon2id считался»).
class Application : public QObject {
    Q_OBJECT
public:
    enum class Gate { NeedSetup, NeedUnlock, Ready };
    enum class Mode { None, Password, Hello };
    struct BackupInfo {
        enum class State { Missing, Available, Unavailable };
        State state=State::Missing;
        QString mainPath,backupPath;
        qint64 size=0;
        QDateTime modified;
    };
    BackupInfo backupInfo() const;
    Mode backupMode(QString* error);
    bool prepareBackupRestore(const QByteArray& backupPassword,QString* error);
#ifdef Q_OS_WIN
    bool prepareHelloBackupRestore(void* hwnd,QString* error);
#endif
    void cancelBackupRestore();
    store::RestoreResult restorePreparedBackup();
#ifdef Q_OS_WIN
    using HelloRequest = std::function<platform::hello::Result(void*, const QString&)>;
    explicit Application(const QString& dataDir, QObject* parent = nullptr,
                         HelloRequest hello = platform::hello::request,
                         store::FileOps& ops=store::realFileOps());
#else
    explicit Application(const QString& dataDir, QObject* parent = nullptr,store::FileOps& ops=store::realFileOps());
#endif
    Gate gate() const { return gate_; }
    Mode mode() const { return mode_; }
    Mode unlockMode() const { return hint_; }               // подсказка для экрана разблокировки (режим прошлого раза)
    bool busy() const { return busy_; }
    static std::unique_ptr<QLockFile> acquireInstanceLock(const QString& dir, QString* err); // единственный экземпляр
    static bool validPassword(const QByteArray& utf8);      // 12–1024 байта UTF-8 (спека)
    bool createPasswordVault(const QByteArray& pw, QString* err);
    bool unlock(const QByteArray& pw, QString* err);
    bool save(QString* err);
    bool schemaUpgradeRequired() const;
    bool schemaUpgradeApproved() const {return gate_==Gate::Ready && upgradeApproval_ && *upgradeApproval_==generation();}
    bool approveSchemaUpgrade(QString* error);
    QString upgradeSafetyPath() const { return upgradeSafetyPath_; }
    QString preserveBeforeRegistryMigration(QString* error);
    bool checkPassword(const QByteArray& pw, QString* err);
    void lock();
    QByteArray exportContainer(const QByteArray& exportPw, QString* err);
    bool importContainer(const QByteArray& bytes, const QByteArray& importPw, const QByteArray& newLocalPw, QString* err);
    bool switchToPassword(const QByteArray& newPw, QString* err);
#ifdef Q_OS_WIN
    bool switchToHello(QString* err);
#endif
    model::VaultModel* model() { return &model_; }
    quint64 generation() const { return generation_.load(); } // растёт на каждой lock(): сверка для асинхронных проверок
    QString storePathForTest() const { return store_.mainPath(); }
#ifdef Q_OS_WIN
    bool createHelloVault(QString* err, void* hwnd = nullptr);
    bool unlockHello(QString* err, void* hwnd = nullptr);
    bool verifyHello(void* hwnd, QString* err);
#endif
signals:
    void stateChanged();
    void busyChanged(bool busy);
private:
    struct Outcome;
    struct PreparedBackup {
        QByteArray main,backup;
        Mode mode=Mode::None;
        quint64 generation=0,request=0;
    };
    std::optional<PreparedBackup> backupHint_,preparedBackup_;
    std::atomic<quint64> backupRequest_{0};
    bool prepareBackup(Mode mode,const QByteArray& password,void* hwnd,QString* error);
    // Запуск job в QtConcurrent с модальным прогрессом и применением результата в
    // GUI-потоке: adopt=true — операция переводит хранилище в Ready (создание/
    // разблокировка/импорт/смена режима), false — только сохранение поверх текущего.
    // Поколение сверяется до применения: результат отменённой операции выбрасывается.
    bool execute(std::function<void(Outcome&,quint64)> job, Mode mode, bool adopt, QString* err);
    bool storeCandidate(Outcome& result,quint64 expected);
    bool canStart(Gate expected, QString* err) const;
    store::VaultStore store_;
    model::VaultModel model_;
    std::unique_ptr<SecureKey> dataKey_;
    QByteArray file_;
    int persistedVersion_=4;
    std::optional<quint64> upgradeApproval_;
    QString upgradeSafetyPath_;
    Gate gate_ = Gate::NeedSetup;
    Mode mode_ = Mode::None, hint_ = Mode::None;
    std::atomic<quint64> generation_{0};
    bool busy_ = false, hasVault_ = false;
#ifdef Q_OS_WIN
    HelloRequest hello_;
#endif
};
}
