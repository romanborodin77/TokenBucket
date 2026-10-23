#include "QuickPasteWin.h"
#include "QuickPaste.h"
#include "../app/Application.h"
#include "../platform/ClipboardGuard.h"
#include "../ui/MiniPanel.h"
#include <QApplication>
#include <QFileInfo>
#include <QKeySequence>
#include <QSettings>
#include <QScopeGuard>
namespace tb {
namespace paste {
std::optional<Hotkey> parseHotkey(const QKeySequence& sequence) {
    if (sequence.count() != 1) return std::nullopt;
    const auto combination = sequence[0];
    const auto modifiers = combination.keyboardModifiers();
    const auto allowed = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;
    if ((modifiers & ~allowed) || !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
        return std::nullopt;
    UINT flags = MOD_NOREPEAT, key = 0;
    if (modifiers & Qt::ControlModifier) flags |= MOD_CONTROL;
    if (modifiers & Qt::AltModifier) flags |= MOD_ALT;
    if (modifiers & Qt::ShiftModifier) flags |= MOD_SHIFT;
    if (modifiers & Qt::MetaModifier) flags |= MOD_WIN;
    const auto k = combination.key();
    if (k >= Qt::Key_A && k <= Qt::Key_Z) key = UINT('A' + k - Qt::Key_A);
    else if (k >= Qt::Key_0 && k <= Qt::Key_9) key = UINT('0' + k - Qt::Key_0);
    else if (k >= Qt::Key_F1 && k <= Qt::Key_F11) key = UINT(VK_F1 + k - Qt::Key_F1);
    else if (k == Qt::Key_Space) key = VK_SPACE;
    else return std::nullopt;
    return Hotkey{flags, key};
}
Recipient captureRecipient() {
    Recipient r;
    HWND fg = GetForegroundWindow();
    if (!fg) return r;
    r.hwnd = fg;
    wchar_t title[512] = {};
    GetWindowTextW(fg, title, 512);
    r.windowTitle = QString::fromWCharArray(title);
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    GUITHREADINFO gui{sizeof(GUITHREADINFO)};
    if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gui)) r.focusHwnd = gui.hwndFocus;
    r.pid = pid;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        FILETIME exit{}, kernel{}, user{};
        GetProcessTimes(h, &r.created, &exit, &kernel, &user);
        wchar_t path[MAX_PATH] = {};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &sz)) r.processName = QFileInfo(QString::fromWCharArray(path)).fileName();
        CloseHandle(h);
    }
    if (r.processName.isEmpty()) r.processName = QStringLiteral("(нет доступа к процессу — возможно, повышенные права)");
    return r;
}
bool recipientExists(const Recipient& r) {
    if (!r.hwnd || !IsWindow(r.hwnd) || !r.pid) return false;
    DWORD pid = 0; GetWindowThreadProcessId(r.hwnd, &pid);
    if (pid != r.pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    const auto close = qScopeGuard([&] { CloseHandle(process); });
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exit, &kernel, &user)
        || CompareFileTime(&created, &r.created) != 0) return false;
    return !r.focusHwnd || (IsWindow(r.focusHwnd) && GetAncestor(r.focusHwnd, GA_ROOT) == r.hwnd);
}
bool recipientActive(const Recipient& r) {
    if (!recipientExists(r) || GetForegroundWindow() != r.hwnd) return false;
    GUITHREADINFO gui{sizeof(GUITHREADINFO)};
    return !r.focusHwnd || (GetGUIThreadInfo(GetWindowThreadProcessId(r.hwnd, nullptr), &gui)
        && gui.hwndFocus == r.focusHwnd);
}
bool recipientStillValid(const Recipient& r) { return recipientActive(r); }
bool recipientAllowed(const Recipient& r, QString* err) {
    const auto fail = [&] { if (err) *err = QStringLiteral("Не удалось подтвердить права получателя или окно повышено. Автовставка запрещена."); return false; };
    if (!recipientExists(r)) return fail();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid);
    if (!process) return fail();
    const auto closeProcess = qScopeGuard([&] { CloseHandle(process); });
    const auto level = [](HANDLE process, DWORD& integrity, bool& elevated) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
        const auto close = qScopeGuard([&] { CloseHandle(token); });
        TOKEN_ELEVATION elevation{}; DWORD size = 0;
        if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) return false;
        elevated = elevation.TokenIsElevated != 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
        if (!size) return false;
        QByteArray bytes(qsizetype(size), char(0));
        if (!GetTokenInformation(token, TokenIntegrityLevel, bytes.data(), size, &size)) return false;
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(bytes.constData());
        if (!IsValidSid(label->Label.Sid)) return false;
        const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
        if (!count) return false;
        integrity = *GetSidSubAuthority(label->Label.Sid, count - 1);
        return true;
    };
    DWORD own = 0, target = 0; bool ownElevated = false, elevated = false;
    if (!level(GetCurrentProcess(), own, ownElevated) || !level(process, target, elevated)
        || elevated || target > own || target >= SECURITY_MANDATORY_HIGH_RID) return fail();
    return true;
}
bool activateRecipient(const Recipient& r) { return recipientExists(r) && SetForegroundWindow(r.hwnd); }
bool modifiersReleased() {
    for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000) return false;
    return true;
}
UINT sendCtrlV(InputSender sender) {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'V';
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT count = sender(4, in, sizeof(INPUT));
    if (count > 0 && count < 4) {
        INPUT release[2] = {in[2], in[3]};
        if (count == 2) sender(2, release, sizeof(INPUT));
        else sender(1, release + 1, sizeof(INPUT));
    }
    return count;
}
}
namespace platform {
QuickPaste::QuickPaste(Application& app, ClipboardGuard& clip, QWidget* /*panelParent*/, QObject* parent)
    : QObject(parent), app_(app), clip_(clip) {
    // Панель сознательно без родителя-окна: Qt::Tool прячется вместе с родителем,
    // а панель обязана открываться по Ctrl+Alt+Space и из трея при свёрнутом окне.
    // Владение ручное — удаляем панель в деструкторе (скрытое окно-владелец лежит внутри).
    panel_ = new ui::MiniPanel(app, clip, nullptr);
    hidden_ = new QWidget(panel_);
    hidden_->setObjectName(QStringLiteral("hotkeyOwnerWindow"));
}
QuickPaste::~QuickPaste() {
    qApp->removeNativeEventFilter(this);
    if (hotkeyId_) UnregisterHotKey(hotkeyHwnd_, hotkeyId_);
    delete panel_; // панель parentless: скрытое окно-владелец горячей клавиши уходит вместе с ней
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
void QuickPaste::popupPanel() { panel_->popup(); }
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
