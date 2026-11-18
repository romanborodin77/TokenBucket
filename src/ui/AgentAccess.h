#pragma once
#include <QString>
#include <functional>
#include <memory>

class QWidget;
namespace tb { class Application; }
namespace tb::agent {
class Access {
public:
    using Decision = std::function<bool(QWidget*)>;
    explicit Access(Application& app, const QString& endpointSuffix = {}, Decision consent = {}, bool sshExec = false,
                    const QString& testSshProgram = {}, bool legacyAccess = true);
    ~Access();
    bool start();
    bool sshEnabled() const;
    void setSshEnabled(bool enabled);
    bool sshOffSavePending() const { return sshOffSavePending_; }
    void setSshOffSavePending(bool pending) { sshOffSavePending_ = pending; }
private:
    class Broker;
    std::unique_ptr<Broker> broker_;
    bool sshOffSavePending_ = false;
};
}
