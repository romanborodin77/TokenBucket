#include <QApplication>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include "app/Application.h"
#include "app/Preferences.h"
#include "ui/SessionFlow.h"
#ifdef Q_OS_WIN
#include "platform/EnvOps.h"
#include "platform/CrashPolicy.h"
#include "platform/SshExec.h"
#include "ui/AgentAccess.h"
#endif
#include "ui/Theme.h"
#include "ui/Brand.h"
#include "Version.h"
namespace {
int start() {
    bool startHidden = QCoreApplication::arguments().contains(QStringLiteral("--tray"));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString error;
    auto instanceLock = tb::Application::acquireInstanceLock(dir, &error);
    if(!instanceLock) {QMessageBox::critical(nullptr,QStringLiteral("TokenBucket"),error); return 1;}
#ifdef Q_OS_WIN
    std::jthread sshCleanup(tb::agent::cleanAbandonedSshStages);
#endif
    tb::Application core(dir);
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
#ifdef Q_OS_WIN
    const auto args=QCoreApplication::arguments();
    const bool legacyAccess=args.contains(QStringLiteral("--agent-access"));
    const bool sshEnabled=tb::prefs::read(settings,legacyAccess && args.contains(QStringLiteral("--ssh-exec"))).mcpSshEnabled;
    tb::agent::Access agentAccess(core,{}, {},false,{},legacyAccess);
    if (legacyAccess || sshEnabled) {
        if (agentAccess.start()) agentAccess.setSshEnabled(sshEnabled);
        else {
            QMessageBox::warning(nullptr, QStringLiteral("TokenBucket"),
                QStringLiteral("Локальный интерфейс агента не запущен. Хранилище доступно через обычный интерфейс."));
        }
    }
    const int code=tb::ui::runSession(core,settings,startHidden,nullptr,{},&agentAccess);
    // Очистка выполняется только владельцем instance lock и до его освобождения.
    if(tb::platform::envops::clearOnExitEnabled(&settings)) tb::platform::envops::clearAllExported(&settings);
    return code;
#else
    return tb::ui::runSession(core,settings,startHidden);
#endif
}
}
int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    // Раньше всего: запретить аварийные дампы и системные диалоги ошибок до того,
    // как в памяти появятся секреты (снимок дампа содержит всю кучу процесса).
    tb::platform::crash::disableDumps();
    if (tb::agent::isSshAskpass()) return tb::agent::runSshAskpass();
#endif
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TokenBucket")); app.setOrganizationName(QStringLiteral("TokenBucket"));
    // Счётчик версий из CMake; иконка всех окон — логотип «Щит-жетон».
    app.setApplicationVersion(QString::fromLatin1(TOKENBUCKET_VERSION));
    app.setWindowIcon(QIcon(tb::ui::brand::logo(256)));
    app.setQuitOnLastWindowClosed(false);
    tb::ui::theme::apply(tb::ui::theme::load()); // до создания окон: палитра наследуется всеми окнами
    try { return start(); }
    catch (...) { QMessageBox::critical(nullptr, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось запустить приложение.")); return 1; }
}
