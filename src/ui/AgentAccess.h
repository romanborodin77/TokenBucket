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
                    const QString& testSshProgram = {});
    ~Access();
    bool start();
private:
    class Broker;
    std::unique_ptr<Broker> broker_;
};
}
