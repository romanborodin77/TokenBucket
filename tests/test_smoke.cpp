#include <QtTest>
#include <sodium.h>
class TestSmoke : public QObject {
    Q_OBJECT
private slots:
    void libsodiumAndQtLink() {
        QVERIFY(sodium_init() >= 0);
        QCOMPARE(QString("a") + "b", QStringLiteral("ab"));
    }
};
QTEST_MAIN(TestSmoke)
#include "test_smoke.moc"
