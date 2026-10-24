#include <QtTest>
#include <QApplication>
#include <QWidget>
#include <QKeySequence>
#include <QSettings>
#include <QTemporaryDir>
#include "../src/platform/QuickPasteWin.h"
#include "../src/platform/ClipboardGuard.h"
#include "../src/app/Application.h"
#include "../src/platform/QuickPaste.h"
#include "../src/ui/MiniPanel.h"
using namespace tb;
#ifdef Q_OS_WIN
namespace {
UINT accepted = 0;
QList<QList<INPUT>> sent;
UINT WINAPI fakeInput(UINT count, LPINPUT input, int) {
    sent.append(QList<INPUT>(input, input + count));
    return accepted < count ? accepted : count;
}
}
#endif
class TestPaste : public QObject {
    Q_OBJECT
private slots:
    void nullIsNotAutoPasted() {
        model::Entry e; e.type = model::EntryType::ApiToken;
        e.token = QStringLiteral("before") + QChar::Null + QStringLiteral("after");
        QVERIFY(!paste::eligibleForAutoPaste(e));
    }
    void chosenField() {
        model::Entry e; e.type = model::EntryType::Credentials;
        e.login = "demo"; e.password = "line1\nline2";
        QVERIFY(paste::eligibleForAutoPaste(e, paste::Field::Login));
        QVERIFY(!paste::eligibleForAutoPaste(e, paste::Field::Secret));
        e.type = model::EntryType::ApiToken; e.token = "demo";
        QVERIFY(!paste::eligibleForAutoPaste(e, paste::Field::Login));
        QVERIFY(paste::eligibleForAutoPaste(e, paste::Field::Secret));
    }
#ifdef Q_OS_WIN
    void suspensionBlocksHotkeyAndManualPopup() {
        QTemporaryDir dir; Application app(dir.path()); ClipboardGuard clip; QString error;
        QVERIFY(app.createPasswordVault("valid-password",&error));
        platform::QuickPaste quick(app,clip,nullptr);
        QVERIFY2(quick.start(QKeySequence("Ctrl+Alt+Shift+F7"),&error),qPrintable(error));
        ui::MiniPanel* panel=nullptr;
        for(auto* widget:QApplication::topLevelWidgets()) if(auto* candidate=qobject_cast<ui::MiniPanel*>(widget)) panel=candidate;
        QVERIFY(panel); auto* owner=panel->findChild<QWidget*>("hotkeyOwnerWindow"); QVERIFY(owner);
        const auto before=GetClipboardSequenceNumber();
        quick.setSuspended(true); QVERIFY(quick.suspended()); quick.popupPanel(); QVERIFY(!panel->isVisible());
        MSG message{}; message.hwnd=reinterpret_cast<HWND>(owner->winId()); message.message=WM_HOTKEY; message.wParam=1;
        qintptr result=-1; QVERIFY(quick.nativeEventFilter("windows_generic_MSG",&message,&result));
        QVERIFY(!panel->isVisible()); QCOMPARE(GetClipboardSequenceNumber(),before);
        quick.setSuspended(false); quick.popupPanel(); QVERIFY(panel->isVisible());
        quick.setSuspended(true); QVERIFY(!panel->isVisible());
    }
    void hotkeys() {
        QTemporaryDir dir; Application app(dir.path()); ClipboardGuard clip;
        platform::QuickPaste quick(app, clip, nullptr);
        QSignalSpy rejected(&quick, &platform::QuickPaste::conflictDetected);
        QString error;
        QVERIFY(!quick.start(QKeySequence("Ctrl+Tab"), &error));
        QCOMPARE(rejected.count(), 1); QVERIFY(!error.isEmpty());
        for (int key = Qt::Key_F1; key <= Qt::Key_F11; ++key) {
            const auto parsed = paste::parseHotkey(QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key(key))));
            QVERIFY(parsed); QCOMPARE(parsed->key, UINT(VK_F1 + key - Qt::Key_F1));
            QCOMPARE(parsed->modifiers, UINT(MOD_CONTROL | MOD_NOREPEAT));
        }
        for (const auto* value : {"Ctrl+A", "Alt+Z", "Ctrl+0", "Meta+9", "Ctrl+Alt+Space"})
            QVERIFY2(paste::parseHotkey(QKeySequence(QString::fromLatin1(value))).has_value(), value);
        for (const auto* value : {"", "A", "Shift+F1", "Ctrl+Tab", "Ctrl+F12", "Ctrl+K, Ctrl+C"})
            QVERIFY(!paste::parseHotkey(QKeySequence(QString::fromLatin1(value))));
    }
#endif
    void eligibility() {
        model::Entry ok; ok.type = model::EntryType::ApiToken; ok.token = "single-line-token";
        QVERIFY(paste::eligibleForAutoPaste(ok));
        model::Entry multi = ok; multi.token = "line1\nline2";
        QVERIFY(!paste::eligibleForAutoPaste(multi));
        model::Entry crlf = ok; crlf.token = "line1\r\nline2";
        QVERIFY(!paste::eligibleForAutoPaste(crlf));
        model::Entry empty = ok; empty.token.clear();
        QVERIFY(!paste::eligibleForAutoPaste(empty));
        model::Entry ssh; ssh.type = model::EntryType::SshKey; ssh.token.clear(); ssh.sshPrivate = "key";
        QVERIFY(!paste::eligibleForAutoPaste(ssh));
        model::Entry pass; pass.type = model::EntryType::Credentials; pass.password = "pw";
        QVERIFY(paste::eligibleForAutoPaste(pass)); // однострочный пароль допустим
        model::Entry multiPw = pass; multiPw.password = "a\r\nb";
        QVERIFY(!paste::eligibleForAutoPaste(multiPw));
    }
#ifdef Q_OS_WIN
    void sequenceFailureKeepsRegistration() {
        QTemporaryDir dir;
        Application app(dir.path()); ClipboardGuard clip;
        platform::QuickPaste quick(app, clip, nullptr);
        QString error;
        const QKeySequence original(QStringLiteral("Ctrl+Alt+Shift+F8"));
        QVERIFY2(quick.start(original, &error), qPrintable(error));
        QSettings broken(dir.path(), QSettings::IniFormat);
        QVERIFY(!quick.changeSequence(QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F9")), broken, &error));
        QCOMPARE(quick.sequence(), original);
        QSettings good(dir.filePath("settings.ini"), QSettings::IniFormat);
        const QKeySequence next(QStringLiteral("Ctrl+Alt+Shift+F10"));
        QVERIFY2(quick.changeSequence(next, good, &error), qPrintable(error));
        QCOMPARE(quick.sequence(), next);
        QCOMPARE(good.value("hotkey").toString(), next.toString(QKeySequence::PortableText));
        QWidget owner;
        QSignalSpy startupConflict(&quick, &platform::QuickPaste::conflictDetected);
        const auto occupied = paste::parseHotkey(original); QVERIFY(occupied);
        const auto hwnd = reinterpret_cast<HWND>(owner.winId());
        QVERIFY(RegisterHotKey(hwnd, 77, occupied->modifiers, occupied->key));
        const bool changed = quick.changeSequence(original, good, &error);
        UnregisterHotKey(hwnd, 77);
        QVERIFY(!changed); QCOMPARE(quick.sequence(), next);
        QCOMPARE(startupConflict.count(), 0);
        QCOMPARE(good.value("hotkey").toString(), next.toString(QKeySequence::PortableText));
    }
    void inputCounts() {
        for (UINT count = 0; count <= 4; ++count) {
            accepted = count; sent.clear();
            QCOMPARE(paste::sendCtrlV(&fakeInput), count);
            QCOMPARE(sent.first().size(), 4);
            for (qsizetype call = 1; call < sent.size(); ++call)
                for (const auto& input : sent[call])
                    QVERIFY(input.ki.dwFlags & KEYEVENTF_KEYUP);
        }
    }
    void recipientIdentity() {
        QWidget window; window.show(); window.raise(); window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        const auto r = paste::captureRecipient();
        QVERIFY(paste::recipientExists(r));
        auto changed = r; changed.pid ^= 1;
        QVERIFY(!paste::recipientExists(changed));
        changed = r; changed.created.dwLowDateTime ^= 1;
        QVERIFY(!paste::recipientExists(changed));
        QVERIFY(!paste::recipientAllowed(changed, nullptr));
    }
    void recipientChangedDetected() {
        // наши собственные тестовые HWND как «получатель»; смена фокуса обязана дать false
        QWidget w; w.show(); w.raise(); w.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(150);
        auto r = paste::captureRecipient();
        QVERIFY2(r.hwnd != nullptr, "нет активного окна перед захватом получателя");
        QVERIFY(paste::recipientStillValid(r));
        QWidget other; other.show(); other.raise(); other.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&other));
        QTest::qWait(200);
        const auto now = paste::captureRecipient();
        if (now.hwnd == reinterpret_cast<HWND>(other.winId())) {
            // фокус удалось перевести на другое окно: исходный получатель недействителен
            QVERIFY(!paste::recipientStillValid(r) || r.hwnd == reinterpret_cast<HWND>(other.winId()));
        } else {
            // стороннее окно удерживает фокус (неинтерактивный запуск): то же правило
            // проверяем на синтетическом получателе, не владеющем фокусом
            paste::Recipient stale; stale.hwnd = reinterpret_cast<HWND>(other.winId());
            QVERIFY(!paste::recipientStillValid(stale));
            QVERIFY(r.hwnd != reinterpret_cast<HWND>(other.winId()));
        }
    }
#endif
};
QTEST_MAIN(TestPaste)
#include "test_paste_win.moc"
