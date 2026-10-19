#pragma once
#include <QObject>
#include <QTimer>
namespace tb {
class IdleTimer : public QObject {
    Q_OBJECT
public:
    explicit IdleTimer(int timeoutMs, QObject* parent = nullptr);
    void poke();
    void setActive(bool active);
signals:
    void idleTimeout();
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    QTimer timer_;
    bool active_ = true;
};
}
