#include "TrayController.h"
#include "../app/Application.h"
#include <QAction>
#include <QGuiApplication>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
namespace tb::ui {
namespace {
QIcon makeIcon(bool unlocked) {
    // Программная иконка без ресурсов: зелёный кружок — разблокировано, серый — остальное;
    // тёмная обводка отделяет светлый кружок от светлого фона панели задач.
    QPixmap pixmap(16, 16);
    pixmap.fill(unlocked ? QColor(0x2e, 0x7d, 0x32) : QColor(0x61, 0x61, 0x61));
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(0x1b, 0x1b, 0x1b), 1));
    painter.drawEllipse(1, 1, 14, 14);
    return QIcon(pixmap);
}
}
TrayController::TrayController(Application& app, QObject* parent) : QObject(parent), app_(app), tray_(this) {
    menu_ = new QMenu; // без родителя-виджета: удаляем сами
    menu_->addAction(QStringLiteral("Показать окно"), this, &TrayController::showRequested);
    panelAction_ = menu_->addAction(QStringLiteral("Панель быстрой вставки"), this, [this] { if (onPanel_) onPanel_(); });
    lockAction_ = menu_->addAction(QStringLiteral("Заблокировать"), this, &TrayController::lockRequested);
    menu_->addSeparator();
    menu_->addAction(QStringLiteral("Выход"), this, &TrayController::quitRequested);
    connect(&app_, &Application::stateChanged, this, &TrayController::onStateChanged);
    connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick) emit showRequested();
    });
    // Трей не показывается без области уведомлений и на offscreen-платформе (тесты):
    // объект живёт и продолжает следить за состоянием, но меню недоступно — как RegisterHotKey в QuickPaste::start.
    if (QSystemTrayIcon::isSystemTrayAvailable() && QGuiApplication::platformName() == QLatin1String("windows")) {
        tray_.setContextMenu(menu_);
        refresh();
        tray_.show();
    }
}
TrayController::~TrayController() { delete menu_; }
void TrayController::onStateChanged() { refresh(); }
void TrayController::refresh() {
    const bool ready = app_.gate() == Application::Gate::Ready;
    lockAction_->setEnabled(ready);
    panelAction_->setEnabled(ready);
    tray_.setIcon(makeIcon(ready));
    tray_.setToolTip(ready ? QStringLiteral("TokenBucket — разблокировано")
        : app_.gate() == Application::Gate::NeedSetup ? QStringLiteral("TokenBucket — хранилище не создано")
        : QStringLiteral("TokenBucket — заблокировано"));
}
}
