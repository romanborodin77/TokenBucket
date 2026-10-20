#include "SessionWatcher.h"
#include <QWidget>
#include <windows.h>
#include <wtsapi32.h>
namespace tb::platform {
namespace {
// Скрытое окно-приёмник: WTS-уведомления о сеансе доставляются через сообщения
// Windows, нужен HWND. Блокировка seанса (WTS_SESSION_LOCK) и уход в сон
// (PBT_APMSUSPEND) немедленно запирают хранилище — не дожидаясь таймера
// автоблокировки: свернутое спящее устройство не должно держать секреты в памяти.
class Receiver : public QWidget {
public:
    SessionWatcher& watcher;
    explicit Receiver(SessionWatcher& owner) : watcher(owner) {}
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
        const auto* event = static_cast<MSG*>(message);
        if ((event->message == WM_WTSSESSION_CHANGE && event->wParam == WTS_SESSION_LOCK)
            || (event->message == WM_POWERBROADCAST && event->wParam == PBT_APMSUSPEND)) emit watcher.lockNow();
        return QWidget::nativeEvent(type, message, result);
    }
};
}
SessionWatcher::SessionWatcher(QObject* parent) : QObject(parent), receiver_(new Receiver(*this)) {
    receiver_->setObjectName(QStringLiteral("sessionEventWindow"));
    const auto hwnd = reinterpret_cast<HWND>(receiver_->winId());
    if (!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)) error_ = QStringLiteral("Не удалось подключить блокировку сеанса Windows (%1).").arg(GetLastError());
}
SessionWatcher::~SessionWatcher() {
    WTSUnRegisterSessionNotification(reinterpret_cast<HWND>(receiver_->winId())); delete receiver_;
}
}
