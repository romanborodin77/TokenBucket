#pragma once
#include <QObject>
#include <QTimer>
namespace tb {
// Таймер автоблокировки: глобальный фильтр событий приложения замечает активность
// пользователя (мышь, клавиши, тач, колесо, ввод IME) и перезапускает одноразовый
// отсчёт; без событий дольше интервала — сигнал idleTimeout (main связывает его
// с Application::lock). Активен только в Ready — заблокированное хранилище
// не тикает впустую.
class IdleTimer : public QObject {
    Q_OBJECT
public:
    explicit IdleTimer(int timeoutMs, QObject* parent = nullptr);
    void poke();              // перезапустить отсчёт (любая активность)
    void setActive(bool active);
    void setIntervalMs(int ms);
signals:
    void idleTimeout();
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    QTimer timer_;
    bool active_ = true;
};
}
