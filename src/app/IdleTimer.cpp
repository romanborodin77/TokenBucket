#include "IdleTimer.h"
#include <QApplication>
#include <QEvent>
namespace tb {
IdleTimer::IdleTimer(int timeoutMs, QObject* parent) : QObject(parent) {
    timer_.setParent(this); timer_.setSingleShot(true); timer_.setInterval(timeoutMs);
    connect(&timer_, &QTimer::timeout, this, &IdleTimer::idleTimeout);
    qApp->installEventFilter(this); poke();
}
void IdleTimer::poke() { if (active_) timer_.start(); }
void IdleTimer::setActive(bool active) {
    if (active_ == active) return;
    active_ = active; if (active) poke(); else timer_.stop();
}
bool IdleTimer::eventFilter(QObject* object, QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease: case QEvent::MouseMove:
    case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::Wheel:
    case QEvent::TouchBegin: case QEvent::TouchUpdate: case QEvent::InputMethod:
    case QEvent::TabletMove: case QEvent::TabletPress: case QEvent::TabletRelease:
        poke(); break;
    default: break;
    }
    return QObject::eventFilter(object, event);
}
}
