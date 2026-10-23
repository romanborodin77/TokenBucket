#include <QtTest>
#include <QApplication>
#include <QWidget>
#include <QKeySequence>
#include "../src/app/Application.h"
#include "../src/platform/QuickPaste.h"
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
    void hotkeys() {
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
