#pragma once
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>
#include <QWidget>
#include <windows.h>
namespace tb {
class Application;
class ClipboardGuard;
namespace ui { class MiniPanel; }
namespace platform {
// Быстрая вставка (Ctrl+Alt+Space): регистрирует глобальный hotkey и по нажатию
// показывает панель выбора записи (MiniPanel) поверх активного окна. Hotkey
// регистрируется только на нативной платформе (offscreen в тестах — без HWND, крах).
// conflictDetected сообщает UI, что комбинация занята другим приложением.
class QuickPaste : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    QuickPaste(Application& app, ClipboardGuard& clip, QWidget* panelParent, QObject* parent = nullptr);
    ~QuickPaste() override;
    bool start(QString* err);
    bool reRegister(QString* err);
    void popupPanel();
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;
signals:
    void conflictDetected(const QString& combo);
private:
    static std::pair<UINT, UINT> splitSequence(const class QKeySequence& ks);
    Application& app_;
    ClipboardGuard& clip_;
    ui::MiniPanel* panel_;   // parentless-панель: живёт и при свёрнутом в трей окне; удаляем сами
    QWidget* hidden_;        // невидимое окно-владелец hotkey (сообщения WM_HOTKEY)
    HWND hotkeyHwnd_ = nullptr;
    int hotkeyId_ = 0;
};
}
}
