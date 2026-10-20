#pragma once
#include <QObject>
#include <QString>
class QWidget;
namespace tb::platform {
// Отслеживание событий сеанса ОС для немедленной блокировки: блокировка рабочей
// станции и переход в сон (спека §4). Windows-реализация — WTS/PowerBroadcast через
// скрытое окно; Linux-ветка (sessionlock_linux.cpp) — DBus Screensaver/login1.
// Ошибка подключения критична: приложение без этой защиты не запускается (см. main).
class SessionWatcher : public QObject {
    Q_OBJECT
public:
    explicit SessionWatcher(QObject* parent = nullptr);
    ~SessionWatcher() override;
    QString error() const { return error_; }
signals:
    void lockNow();
private slots:
    void onActiveChanged(bool active) { if (active) emit lockNow(); } // Linux: хранитель экрана стал активен
private:
    QString error_;
#ifdef Q_OS_WIN
    QWidget* receiver_ = nullptr; // скрытое окно-приёмник сообщений WTS/Power
#endif
};
}
