#include "TrayController.h"
#include "../app/Application.h"
#include "../app/Preferences.h"
#include "Brand.h"
#include <QAction>
#include <QGuiApplication>
#include <QMenu>
#include <QSettings>
namespace tb::ui {
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
void TrayController::onStateChanged() {
    // Переход Ready → заблокировано виден только в трее, если окно свёрнуто, — уведомляем.
    const bool ready = app_.gate() == Application::Gate::Ready;
    if (wasReady_ && !ready) notify(QStringLiteral("Хранилище заблокировано"));
    wasReady_ = ready;
    refresh();
}
bool TrayController::notificationsEnabled(QSettings* preferences) {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    return prefs::read(preferences?*preferences:settings).notifications;
}
bool TrayController::setNotificationsEnabled(bool on,QSettings* preferences,QString* error) {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    return prefs::writeValue(preferences?*preferences:settings,QStringLiteral("trayNotifications"),on,error);
}
void TrayController::notify(const QString& text) {
    if (!notificationsEnabled() || !tray_.isVisible()) return;
    tray_.showMessage(QStringLiteral("TokenBucket"), text, QIcon(brand::icon(32)), 4000);
}
void TrayController::refresh() {
    const bool ready = app_.gate() == Application::Gate::Ready;
    lockAction_->setEnabled(ready);
    panelAction_->setEnabled(ready);
    // Логотип «Щит-жетон»: цветной при разблокированном хранилище, обесцвеченный иначе.
    tray_.setIcon(QIcon(brand::icon(16, ready ? brand::Tone::Normal : brand::Tone::Locked)));
    tray_.setToolTip(ready ? QStringLiteral("TokenBucket — разблокировано")
        : app_.gate() == Application::Gate::NeedSetup ? QStringLiteral("TokenBucket — хранилище не создано")
        : QStringLiteral("TokenBucket — заблокировано"));
}
}
