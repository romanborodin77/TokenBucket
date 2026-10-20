#include "QuickPasteWin.h"
#include "QuickPaste.h"
#include "../app/Application.h"
#include "../platform/ClipboardGuard.h"
#include "../ui/MiniPanel.h"
#include <QApplication>
#include <QFileInfo>
#include <QKeySequence>
#include <QSettings>
namespace tb {
namespace paste {
Recipient captureRecipient() {
    Recipient r;
    HWND fg = GetForegroundWindow();
    if (!fg) return r;
    r.hwnd = fg;
    wchar_t title[512] = {};
    GetWindowTextW(fg, title, 512);
    r.windowTitle = QString::fromWCharArray(title);
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t path[MAX_PATH] = {};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &sz)) r.processName = QFileInfo(QString::fromWCharArray(path)).fileName();
        CloseHandle(h);
    }
    if (r.processName.isEmpty()) r.processName = QStringLiteral("(нет доступа к процессу — возможно, повышенные права)");
    return r;
}
bool recipientStillValid(const Recipient& r) {
    return r.hwnd && GetForegroundWindow() == r.hwnd && IsWindow(static_cast<HWND>(r.hwnd));
}
bool sendCtrlV() {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'V';
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    // SendInput атомарен не полностью: отказ мог прийти после части событий —
    // сообщаем неудачу, секрет остаётся защищён 20-секундным таймером буфера.
    return SendInput(4, in, sizeof(INPUT)) == 4;
}
}
namespace platform {
QuickPaste::QuickPaste(Application& app, ClipboardGuard& clip, QWidget* panelParent, QObject* parent)
    : QObject(parent), app_(app), clip_(clip), panel_(new ui::MiniPanel(app, clip, panelParent)), hidden_(new QWidget(panel_)) {
    hidden_->setObjectName(QStringLiteral("hotkeyOwnerWindow"));
}
QuickPaste::~QuickPaste() {
    qApp->removeNativeEventFilter(this);
    if (hotkeyId_) UnregisterHotKey(hotkeyHwnd_, hotkeyId_);
}
bool QuickPaste::start(QString* err) {
    // offscreen/нет оконного HWND: глобальная горячая клавиша не регистрируется
    if (QGuiApplication::platformName() != QLatin1String("windows")) return true;
    qApp->installNativeEventFilter(this);
    return reRegister(err);
}
bool QuickPaste::reRegister(QString* err) {
    if (err) err->clear();
    if (hotkeyId_) { UnregisterHotKey(hotkeyHwnd_, hotkeyId_); hotkeyId_ = 0; }
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    const QKeySequence ks(settings.value(QStringLiteral("hotkey"), QStringLiteral("Ctrl+Alt+Space")).toString());
    const auto [mods, vk] = splitSequence(ks);
    hotkeyHwnd_ = reinterpret_cast<HWND>(hidden_->winId());
    if (!RegisterHotKey(hotkeyHwnd_, 1, mods, vk)) {
        if (err) *err = QStringLiteral("Сочетание %1 занято другим приложением.").arg(ks.toString());
        emit conflictDetected(ks.toString());
        return false;
    }
    hotkeyId_ = 1;
    return true;
}
bool QuickPaste::nativeEventFilter(const QByteArray& type, void* message, qintptr*) {
    if (type != "windows_generic_MSG") return false;
    auto* msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY && msg->hwnd == hotkeyHwnd_ && msg->wParam == 1) panel_->popup();
    return false;
}
std::pair<UINT, UINT> QuickPaste::splitSequence(const QKeySequence& ks) {
    UINT mods = 0, vk = 0;
    if (!ks.count()) return {0, 0};
    const QKeyCombination c = ks[0];
    if (c.keyboardModifiers() & Qt::CTRL) mods |= MOD_CONTROL;
    if (c.keyboardModifiers() & Qt::ALT) mods |= MOD_ALT;
    if (c.keyboardModifiers() & Qt::SHIFT) mods |= MOD_SHIFT;
    if (c.keyboardModifiers() & Qt::META) mods |= MOD_WIN;
    vk = UINT(c.key());
    return {mods, vk};
}
}
}
