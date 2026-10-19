#pragma once
#include <QObject>
#include <QLockFile>
#include <functional>
#include <memory>
#include <atomic>
#include "../model/VaultModel.h"
#include "../vault/SecureBytes.h"
#include "../vault/VaultStore.h"
#ifdef Q_OS_WIN
#include "../platform/hello_win.h"
#endif
namespace tb {
class Application : public QObject {
    Q_OBJECT
public:
    enum class Gate { NeedSetup, NeedUnlock, Ready };
    enum class Mode { None, Password, Hello };
#ifdef Q_OS_WIN
    using HelloRequest = std::function<platform::hello::Result(void*, const QString&)>;
    explicit Application(const QString& dataDir, QObject* parent = nullptr,
                         HelloRequest hello = platform::hello::request);
#else
    explicit Application(const QString& dataDir, QObject* parent = nullptr);
#endif
    Gate gate() const { return gate_; }
    Mode mode() const { return mode_; }
    Mode unlockMode() const { return hint_; }
    bool busy() const { return busy_; }
    static std::unique_ptr<QLockFile> acquireInstanceLock(const QString& dir, QString* err);
    static bool validPassword(const QByteArray& utf8);
    bool createPasswordVault(const QByteArray& pw, QString* err);
    bool unlock(const QByteArray& pw, QString* err);
    bool save(QString* err);
    bool checkPassword(const QByteArray& pw, QString* err);
    void lock();
    model::VaultModel* model() { return &model_; }
    quint64 generation() const { return generation_.load(); }
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
    bool execute(std::function<std::shared_ptr<Outcome>(quint64)> job, Mode mode, bool adopt, QString* err);
    bool canStart(Gate expected, QString* err) const;
    store::VaultStore store_;
    model::VaultModel model_;
    std::unique_ptr<SecureKey> dataKey_;
    QByteArray file_;
    Gate gate_ = Gate::NeedSetup;
    Mode mode_ = Mode::None, hint_ = Mode::None;
    std::atomic<quint64> generation_{0};
    bool busy_ = false, hasVault_ = false;
#ifdef Q_OS_WIN
    HelloRequest hello_;
#endif
};
}
