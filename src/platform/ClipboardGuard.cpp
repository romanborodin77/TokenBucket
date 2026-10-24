#include "ClipboardGuard.h"
#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QMimeData>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QThread>
#include <QUuid>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace tb {
ClipboardGuard::ClipboardGuard(QObject* parent) : QObject(parent) {
    timer_.setParent(this); timer_.setObjectName(QStringLiteral("clipboardClearTimer")); timer_.setSingleShot(true); timer_.setInterval(20000);
    connect(&timer_, &QTimer::timeout, this, &ClipboardGuard::clearIfOurs);
#ifdef Q_OS_WIN
    owner_ = CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,nullptr,nullptr);
#endif
}
ClipboardGuard::~ClipboardGuard() {
    destroying_ = true;
#ifdef Q_OS_WIN
    // Чужой процесс может держать буфер обмена доли секунды; секрет не должен
    // переживать выход приложения, поэтому несколько попыток с короткими паузами.
    for (int attempt = 0; attempt < 5 && sequence_ != 0; ++attempt) {
        clearIfOurs();
        if (sequence_ != 0) QThread::msleep(20);
    }
#else
    clearIfOurs();
#endif
#ifdef Q_OS_WIN
    if (owner_) DestroyWindow(static_cast<HWND>(owner_));
#endif
}
#ifdef Q_OS_WIN
namespace {
struct Memory {
    HGLOBAL handle;
    explicit Memory(SIZE_T bytes) : handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes)) {}
    ~Memory() { if (handle) { if (auto* p = GlobalLock(handle)) { SecureZeroMemory(p, GlobalSize(handle)); GlobalUnlock(handle); } GlobalFree(handle); } }
    bool publish(UINT format) { if (!handle || !format || !SetClipboardData(format, handle)) return false; handle = nullptr; return true; }
};
}
bool ClipboardGuard::open(quint64 epoch) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (epoch != epoch_ || !owner_) return false;
        if (OpenClipboard(static_cast<HWND>(owner_))) return true;
        // В деструкторе вложенный цикл событий опасен — только короткий сон;
        // повтор всей очистки делает сам деструктор.
        if (destroying_) { QThread::msleep(20); continue; }
        if (attempt < 2) { QEventLoop loop; QTimer::singleShot(50, &loop, &QEventLoop::quit); loop.exec(); }
    }
    return false;
}
#endif
bool ClipboardGuard::copySecret(const QString& text, QString* err) {
    if (err) err->clear();
    const auto fail = [&](const char* message) { if (err) *err = QString::fromUtf8(message); return false; };
    if (copying_) return fail("Дождитесь завершения копирования.");
    if (text.isEmpty() || text.contains(QChar::Null)) return fail("Пустое значение или нулевой символ не копируется.");
    QScopedValueRollback busy(copying_, true); const auto epoch = epoch_;
#ifdef Q_OS_WIN
    Memory data((text.size() + 1) * sizeof(wchar_t)), history(sizeof(DWORD)), cloud(sizeof(DWORD)), exclude(sizeof(DWORD));
    const UINT historyFormat = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), cloudFormat = RegisterClipboardFormatW(L"CanUploadToCloudClipboard"), excludeFormat = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    if (!data.handle || !history.handle || !cloud.handle || !exclude.handle || !historyFormat || !cloudFormat || !excludeFormat) return fail("Не удалось подготовить защищённое копирование.");
    auto* value = static_cast<wchar_t*>(GlobalLock(data.handle));
    if (!value) return fail("Не удалось подготовить значение для копирования.");
    memcpy(value, text.utf16(), text.size() * sizeof(wchar_t)); value[text.size()] = 0; GlobalUnlock(data.handle);
    if (!open(epoch)) return fail("Буфер обмена занят или копирование отменено.");
    auto close = qScopeGuard([] { CloseClipboard(); });
    if (epoch != epoch_ || !EmptyClipboard()) return fail("Не удалось открыть буфер для записи.");
    sequence_ = 0; clearPending_ = false;
    if (!history.publish(historyFormat) || !cloud.publish(cloudFormat) || !exclude.publish(excludeFormat)
        || epoch != epoch_ || !data.publish(CF_UNICODETEXT) || epoch != epoch_) {
        EmptyClipboard(); timer_.stop(); return fail("Не удалось выполнить защищённое копирование.");
    }
    // Windows finalizes synthesized formats and the sequence when the clipboard closes.
    CloseClipboard(); close.dismiss();
    sequence_ = GetClipboardSequenceNumber();
#else
    token_ = QUuid::createUuid().toByteArray();
    auto* data = new QMimeData; data->setText(text); data->setData("application/x-tokenbucket-owner", token_);
    QApplication::clipboard()->setMimeData(data, QClipboard::Clipboard);
    if (epoch != epoch_) { clearIfOurs(); return fail("Копирование отменено."); }
#endif
    clearPending_ = false; timer_.start(clearSeconds_ * 1000); return true;
}
void ClipboardGuard::setClearSeconds(int seconds) {
    if(seconds!=10 && seconds!=20 && seconds!=30 && seconds!=60) seconds=20;
    if(seconds==clearSeconds_) return;
    clearSeconds_=seconds; clearIfOurs();
}
ClipboardGuard::Snapshot ClipboardGuard::snapshot() const {
#ifdef Q_OS_WIN
    const bool owned = sequence_ != 0 && GetClipboardSequenceNumber() == sequence_
        && GetClipboardOwner() == static_cast<HWND>(owner_);
    return {owned, owned && clearPending_, owned ? timer_.remainingTime() : -1};
#else
    return {};
#endif
}
void ClipboardGuard::clearIfOurs() {
    ++epoch_; timer_.stop(); if (clearing_) return; QScopedValueRollback busy(clearing_, true);
#ifdef Q_OS_WIN
    if (!sequence_) { clearPending_ = false; return; }
    clearPending_ = true;
    if (!open(epoch_)) { if (!destroying_) timer_.start(500); return; }
    auto close = qScopeGuard([] { CloseClipboard(); });
    if (GetClipboardSequenceNumber() != sequence_ || GetClipboardOwner() != static_cast<HWND>(owner_)) { sequence_ = 0; clearPending_ = false; return; }
    if (!EmptyClipboard()) { if (!destroying_) timer_.start(500); return; }
    sequence_ = 0; clearPending_ = false;
#else
    auto* clipboard = QApplication::clipboard();
    const auto* data = clipboard->mimeData(QClipboard::Clipboard);
    if (token_.isEmpty() || !clipboard->ownsClipboard() || !data || data->data("application/x-tokenbucket-owner") != token_) { token_.clear(); return; }
    token_.clear(); clipboard->clear(QClipboard::Clipboard);
#endif
    if (!destroying_) emit cleared();
}
}
