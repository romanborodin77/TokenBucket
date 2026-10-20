#pragma once
#include <QObject>
#include <QSystemTrayIcon>
#include <functional>
class QMenu;
class QAction;
namespace tb {
class Application;
namespace ui {
// Иконка в области уведомлений: показывает состояние хранилища и управляет окном.
// Живёт всё приложение; сигналами общается с main, где хранится указатель на текущее окно.
class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(Application& app, QObject* parent = nullptr);
    ~TrayController() override;
    // Вызывается для «Панель быстрой вставки»: main дёргает MainWindow::popupQuickPanel,
    // пока окно живо (панель принадлежит окну).
    void setPanelCallback(std::function<void()> callback) { onPanel_ = std::move(callback); }
signals:
    void showRequested();
    void lockRequested();
    void quitRequested();
private slots:
    void onStateChanged();
private:
    void refresh();
    Application& app_;
    QSystemTrayIcon tray_;
    QMenu* menu_;
    QAction* panelAction_ = nullptr;
    QAction* lockAction_ = nullptr;
    std::function<void()> onPanel_;
};
}
}
