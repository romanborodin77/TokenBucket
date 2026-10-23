#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include "../src/platform/ClipboardGuard.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <cstdio>
#include <memory>
namespace {
bool openTestClipboard(HWND owner) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(10);
    }
    return false;
}
struct ClipboardStation {
    HWINSTA original = GetProcessWindowStation(), station = nullptr;
    HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId()), desktop = nullptr;
    bool ready = false;
    // Станция обязана создаваться ДО QApplication (потоку с окнами нельзя менять
    // desktop), поэтому неготовность фиксируется флагом, а QSKIP выполняется
    // в initTestCase — среда без права CreateWindowStation даёт SKIP, не FAIL.
    ClipboardStation() {
        wchar_t name[80]; swprintf_s(name, L"TokenBucketClipboardTest-%lu", GetCurrentProcessId());
        station = CreateWindowStationW(name, 0, WINSTA_CREATEDESKTOP | WINSTA_ACCESSCLIPBOARD | WINSTA_READATTRIBUTES | WINSTA_WRITEATTRIBUTES | WINSTA_ENUMDESKTOPS | WINSTA_ACCESSGLOBALATOMS, nullptr);
        if (!station) {
            const auto error = GetLastError(); JOBOBJECT_BASIC_UI_RESTRICTIONS limits{};
            const bool queried = QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &limits, sizeof(limits), nullptr);
            fprintf(stderr,"CreateWindowStation: %lu; job query=%d, UI limits=%lu\n",error,queried,limits.UIRestrictionsClass); return;
        }
        if (!SetProcessWindowStation(station)) { fprintf(stderr,"SetProcessWindowStation: %lu\n",GetLastError()); return; }
        desktop = CreateDesktopW(L"Clipboard", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop) { fprintf(stderr,"CreateDesktop: %lu\n",GetLastError()); return; }
        ready = SetThreadDesktop(desktop);
        if (!ready) fprintf(stderr,"SetThreadDesktop: %lu\n",GetLastError());
    }
    ~ClipboardStation() {
        SetThreadDesktop(originalDesktop); SetProcessWindowStation(original);
        if (desktop) CloseDesktop(desktop); if (station) CloseWindowStation(station);
    }
};
bool isolationAvailable = true; // сбрасывается в main при отказе среды
QString clipboardText() {
    if (!openTestClipboard(nullptr)) return QStringLiteral("<clipboard unavailable>");
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    const auto* text = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    QString result = text ? QString::fromWCharArray(text) : QString();
    if (text) GlobalUnlock(handle); CloseClipboard(); return result;
}
bool foreignText(HWND owner, const QString& text) {
    if (!openTestClipboard(owner)) return false;
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    auto* data = memory ? static_cast<wchar_t*>(GlobalLock(memory)) : nullptr;
    bool ok = data != nullptr;
    if (data) { memcpy(data, text.utf16(), text.size() * sizeof(wchar_t)); data[text.size()] = 0; GlobalUnlock(memory); }
    ok = ok && EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    if (!ok && memory) GlobalFree(memory); CloseClipboard(); return ok;
}
}
#else
static QString clipboardText() { return QApplication::clipboard()->text(); }
#endif
class TestClipboard : public QObject {
    Q_OBJECT
#ifdef Q_OS_WIN
    HWND foreign_ = CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,nullptr,nullptr);
#endif
private slots:
    void initTestCase() {
#ifdef Q_OS_WIN
        // Среда без права создания изолированной станции буфера (например, сервисный
        // контекст или политика машины) — пропускаем проверку, а не считаем регрессом.
        if (!isolationAvailable) QSKIP("изоляция буфера недоступна в этой среде", SkipAll);
#endif
    }
    void cleanup() {
#ifdef Q_OS_WIN
        QVERIFY(openTestClipboard(foreign_));
        const bool cleared = EmptyClipboard(); CloseClipboard(); QVERIFY(cleared);
#else
        QApplication::clipboard()->clear();
#endif
    }
    void copiesAndTimerClears() {
        tb::ClipboardGuard guard; QString error;
        QVERIFY(guard.copySecret(QStringLiteral("test-secret"), &error)); QCOMPARE(clipboardText(), QStringLiteral("test-secret"));
        auto* timer = guard.findChild<QTimer*>(QStringLiteral("clipboardClearTimer")); QVERIFY(timer); QCOMPARE(timer->interval(), 20000);
        timer->setInterval(1); QTRY_VERIFY(clipboardText().isEmpty());
    }
    void foreignContentIsPreserved() {
        tb::ClipboardGuard guard; QString error; QVERIFY(guard.copySecret(QStringLiteral("ours"), &error));
#ifdef Q_OS_WIN
        QVERIFY(foreign_); QVERIFY(foreignText(foreign_, QStringLiteral("newer")));
#else
        QApplication::clipboard()->setText(QStringLiteral("newer"));
#endif
        guard.clearIfOurs(); QCOMPARE(clipboardText(), QStringLiteral("newer"));
    }
    void invalidAndSecondCopy() {
        tb::ClipboardGuard guard; QString error; QVERIFY(!guard.copySecret({}, &error));
        QVERIFY(!guard.copySecret(QString(QChar::Null), &error));
        QVERIFY(guard.copySecret(QStringLiteral("first"), &error)); QVERIFY(guard.copySecret(QStringLiteral("second"), &error));
        QCOMPARE(clipboardText(), QStringLiteral("second")); guard.clearIfOurs(); QVERIFY(clipboardText().isEmpty());
    }
#ifdef Q_OS_WIN
    void snapshotTracksOwnershipAndRetry() {
        tb::ClipboardGuard guard; QString error;
        QVERIFY(guard.copySecret(QStringLiteral("synthetic-owned"), &error));
        auto state = guard.snapshot();
        QVERIFY(state.owned); QVERIFY(!state.clearPending); QVERIFY(state.remainingMs > 0);
        QVERIFY(openTestClipboard(foreign_));
        QElapsedTimer elapsed; elapsed.start();
        state = guard.snapshot();
        const auto snapshotMs = elapsed.elapsed();
        guard.clearIfOurs();
        state = guard.snapshot();
        CloseClipboard();
        QVERIFY(snapshotMs < 50);
        QVERIFY(state.owned); QVERIFY(state.clearPending);
        QTRY_VERIFY(!guard.snapshot().owned);
        QVERIFY(!guard.snapshot().clearPending);
        QVERIFY(guard.copySecret(QStringLiteral("synthetic-owned"), &error));
        QVERIFY(foreignText(foreign_, QStringLiteral("synthetic-foreign")));
        QVERIFY(!guard.snapshot().owned);
        QCOMPARE(guard.snapshot().remainingMs, -1);
        guard.clearIfOurs();
        QCOMPARE(clipboardText(), QStringLiteral("synthetic-foreign"));
        QVERIFY(!guard.snapshot().clearPending);
    }
    void historyFormatsAndBusyCancellation() {
        tb::ClipboardGuard guard; QString error; QVERIFY(guard.copySecret(QStringLiteral("ours"), &error));
        QVERIFY(openTestClipboard(foreign_));
        bool formats = true;
        for (const auto name : {L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard", L"ExcludeClipboardContentFromMonitorProcessing"}) {
            const auto memory = GetClipboardData(RegisterClipboardFormatW(name));
            const auto* value = memory ? static_cast<const DWORD*>(GlobalLock(memory)) : nullptr;
            formats = formats && value && *value == 0; if (value) GlobalUnlock(memory);
        }
        CloseClipboard(); QVERIFY(formats); guard.clearIfOurs();
        QVERIFY(foreignText(foreign_, QStringLiteral("keep"))); QVERIFY(openTestClipboard(foreign_));
        QTimer::singleShot(1, &guard, &tb::ClipboardGuard::clearIfOurs);
        const bool copied = guard.copySecret(QStringLiteral("canceled"), &error);
        CloseClipboard(); QVERIFY(!copied); QCOMPARE(clipboardText(), QStringLiteral("keep"));
    }
#endif
public:
    ~TestClipboard() override {
#ifdef Q_OS_WIN
        if (foreign_) DestroyWindow(foreign_);
#endif
    }
};
int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    std::unique_ptr<ClipboardStation> station;
    // Explicit opt-in only: this mode replaces and clears the user's clipboard.
    if (qEnvironmentVariable("TOKENBUCKET_TEST_USER_CLIPBOARD") != QStringLiteral("1")) {
        station = std::make_unique<ClipboardStation>();
        if (!station->ready) isolationAvailable = false; // слоты завершатся QSKIP в initTestCase
    }
#endif
    QApplication app(argc, argv); TestClipboard test; return QTest::qExec(&test, argc, argv);
}
#include "test_clipboard.moc"
