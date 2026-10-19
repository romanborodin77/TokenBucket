#include <QtTest>
#include <QApplication>
#include <QWidget>
#include "../src/app/Application.h"
#include "../src/platform/QuickPaste.h"
using namespace tb;
class TestPaste : public QObject {
    Q_OBJECT
private slots:
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
        if (now.hwnd == (void*)other.winId()) {
            // фокус удалось перевести на другое окно: исходный получатель недействителен
            QVERIFY(!paste::recipientStillValid(r) || r.hwnd == (void*)other.winId());
        } else {
            // стороннее окно удерживает фокус (неинтерактивный запуск): то же правило
            // проверяем на синтетическом получателе, не владеющем фокусом
            paste::Recipient stale; stale.hwnd = (void*)other.winId();
            QVERIFY(!paste::recipientStillValid(stale));
            QVERIFY(r.hwnd != (void*)other.winId());
        }
    }
#endif
};
QTEST_MAIN(TestPaste)
#include "test_paste_win.moc"
