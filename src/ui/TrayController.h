#pragma once
#include <QObject>
#include <QSystemTrayIcon>
#include <functional>
class QMenu;
class QAction;
class QSettings;
namespace tb {
class Application;
namespace ui {
// Иконка в области уведомлений: показывает состояние хранилища и управляет окном.
// Живёт всё приложение; сигналами общается с main, где хранится указатель на текущее окно.
class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(Application& app, QObject* parent = nullptr,QSettings* preferences=nullptr);
    ~TrayController() override;
    // Вызывается для «Панель быстрой вставки»: main дёргает MainWindow::popupQuickPanel,
    // пока окно живо (панель принадлежит окну).
    void setPanelCallback(std::function<void()> callback) { onPanel_ = std::move(callback); }
    // Короткое всплывающее уведомление области уведомлений. Секреты сюда не попадают:
    // передаём только факт события («скопировано», «буфер очищен», «заблокировано»).
    void notify(const QString& text);
    static bool notificationsEnabled(QSettings* preferences=nullptr);
    static bool setNotificationsEnabled(bool on,QSettings* preferences=nullptr,QString* error=nullptr);
    // Подсказка текущего состояния — для тестов и диагностики (без секретов).
    QString toolTip() const { return tray_.toolTip(); }
signals:
    void showRequested();
    void lockRequested();
    void quitRequested();
private slots:
    void onStateChanged();
private:
    void refresh();
    Application& app_;
    QSettings* preferences_{};
    QSystemTrayIcon tray_;
    QMenu* menu_;
    QAction* panelAction_ = nullptr;
    QAction* lockAction_ = nullptr;
    std::function<void()> onPanel_;
    bool wasReady_ = false; // прошлое состояние: отличает блокировку от прочих изменений
};
}
}
