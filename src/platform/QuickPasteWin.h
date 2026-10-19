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
class QuickPaste : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    QuickPaste(Application& app, ClipboardGuard& clip, QWidget* panelParent, QObject* parent = nullptr);
    ~QuickPaste() override;
    bool start(QString* err);
    bool reRegister(QString* err);
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;
signals:
    void conflictDetected(const QString& combo);
private:
    static std::pair<UINT, UINT> splitSequence(const class QKeySequence& ks);
    Application& app_;
    ClipboardGuard& clip_;
    ui::MiniPanel* panel_;
    QWidget* hidden_;
    HWND hotkeyHwnd_ = nullptr;
    int hotkeyId_ = 0;
};
}
}
