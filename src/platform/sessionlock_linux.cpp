#include "SessionWatcher.h"
#include <QDBusConnection>
namespace tb::platform {
SessionWatcher::SessionWatcher(QObject* parent) : QObject(parent) {
    auto session = QDBusConnection::sessionBus(), system = QDBusConnection::systemBus();
    const bool locked = session.connect(QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("/org/gnome/ScreenSaver"),
        QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("ActiveChanged"), this, SLOT(onActiveChanged(bool)));
    const bool sleeping = system.connect(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("PrepareForSleep"), this, SLOT(onActiveChanged(bool)));
    if (!locked || !sleeping) error_ = QStringLiteral("Не удалось подключить уведомления блокировки GNOME и сна logind.");
}
SessionWatcher::~SessionWatcher() = default;
}
