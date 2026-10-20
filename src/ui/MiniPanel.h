#pragma once
#include <QWidget>
#include "../platform/QuickPaste.h"
namespace tb { class Application; class ClipboardGuard; }
class QLineEdit; class QListWidget; class QLabel;
namespace tb::ui {
// Мини-панель быстрой вставки (Ctrl+Alt+Space): поиск по записям + отправка секрета
// выбранному получателю. Окно parentless — остаётся доступным при свёрнутом в трей
// главном окне (Qt прячет Tool-окна вместе с родителем). Получатель фиксируется
// в момент показа и ДВАЖДЫ проверяется при отправке (до и после SetForegroundWindow) —
// устранение TOCTOU смены активного окна; для повышенных процессов — отказ.
class MiniPanel : public QWidget {
    Q_OBJECT
public:
    MiniPanel(Application& app, ClipboardGuard& clip, QWidget* parent = nullptr);
    void popup();
private:
    void send();
    void refill();
    Application& app_;
    ClipboardGuard& clip_;
    QLineEdit* search_{};
    QListWidget* list_{};
    QLabel* recipientLbl_{};  // строка «Куда: процесс — заголовок» для явного подтверждения получателя
#ifdef Q_OS_WIN
    paste::Recipient recipient_{};
#endif
};
}
