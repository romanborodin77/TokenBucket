#include <QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include <QWidget>
#include "../src/app/Application.h"
#include "../src/app/IdleTimer.h"
#include "../src/platform/SessionWatcher.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <wtsapi32.h>
#endif
class TestLock : public QObject {
    Q_OBJECT
private slots:
    void idleTracksInputAndStopsWhileLocked() {
        tb::IdleTimer timer(100); QSignalSpy timeout(&timer, &tb::IdleTimer::idleTimeout);
        QWidget widget;
        QTest::qWait(60); QEvent paint(QEvent::Paint); QApplication::sendEvent(&widget, &paint);
        QTRY_COMPARE(timeout.count(), 1);
        timer.poke(); QTest::qWait(60);
        QKeyEvent input(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier); QApplication::sendEvent(&widget, &input);
        QTest::qWait(60); QCOMPARE(timeout.count(), 1); QTRY_COMPARE(timeout.count(), 2);
        timer.setActive(false); timer.poke(); QTest::qWait(150); QCOMPARE(timeout.count(), 2);
        timer.setActive(true); QTRY_COMPARE(timeout.count(), 3);
    }
    void timeoutClearsVault() {
        QTemporaryDir dir; tb::Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("master-password-test", &error));
        QVERIFY(app.model()->addNode(QUuid::createUuid(), {}, "Project").isEmpty());
        const auto generation = app.generation(); tb::IdleTimer timer(20);
        connect(&timer, &tb::IdleTimer::idleTimeout, &app, &tb::Application::lock);
        QTRY_COMPARE(app.gate(), tb::Application::Gate::NeedUnlock);
        QCOMPARE(app.generation(), generation + 1); QVERIFY(app.model()->nodes().isEmpty());
    }
#ifdef Q_OS_WIN
    void nativeSessionAndSuspendMessages() {
        tb::platform::SessionWatcher watcher; QVERIFY2(watcher.error().isEmpty(), qPrintable(watcher.error()));
        QWidget* receiver = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) if (widget->objectName() == QStringLiteral("sessionEventWindow")) receiver = widget;
        QVERIFY(receiver);
        const auto hwnd = reinterpret_cast<HWND>(receiver->winId()); QSignalSpy locked(&watcher, &tb::platform::SessionWatcher::lockNow);
        SendMessageW(hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0); QCOMPARE(locked.count(), 0);
        SendMessageW(hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0); QCOMPARE(locked.count(), 1);
        SendMessageW(hwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0); QCOMPARE(locked.count(), 2);
    }
#endif
};
QTEST_MAIN(TestLock)
#include "test_lock.moc"
