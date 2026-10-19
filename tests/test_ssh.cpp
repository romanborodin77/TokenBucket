#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <filesystem>
#include "../src/app/Application.h"
#include "../src/platform/FileOps.h"
#include "../src/ui/EntryCard.h"
using namespace tb;
class TestSsh : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
    void keyBytesSurviveRoundtrip() {
        Application app(dir_.path());
        QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        model::Id p{QUuid::createUuid()};
        QVERIFY(app.model()->addNode(p, model::Id(), "P").isEmpty());
        const QByteArray key = QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjEAAAAA\r\n-----END OPENSSH PRIVATE KEY-----\r\n");
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = p;
        e.type = model::EntryType::SshKey; e.title = "K"; e.sshPrivate = key;
        QVERIFY(app.model()->setEntry(e).isEmpty());
        QVERIFY(app.save(&err));
        app.lock();
        QVERIFY(app.unlock("пароль-теста-123", &err));
        QCOMPARE(app.model()->entry(e.id)->sshPrivate, key);   // байты не изменены после цикла
        const QString out = dir_.filePath("id_test");
        QVERIFY(store::realFileOps().writeAll(out, app.model()->entry(e.id)->sshPrivate));
        QFile f(out); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), key);
        QFileInfo info(out);
        QVERIFY(info.exists() && !info.isSymbolicLink());
    }
    void symlinkRefused() {
        const QString realF = dir_.filePath("real");
        QVERIFY(store::realFileOps().writeAll(realF, "x"));
        const QString link = dir_.filePath("link");
        std::error_code ec;
        std::filesystem::create_symlink(realF.toStdWString(), link.toStdWString(), ec);
        QVERIFY2(!ec, qPrintable(QString::fromStdString(ec.message())));
        QVERIFY(QFileInfo(link).isSymbolicLink());
        QString err;
        QVERIFY(ui::EntryCard::isSafeTarget(link, &err) == false);
        QVERIFY(!err.isEmpty());
        QVERIFY(ui::EntryCard::isSafeTarget(dir_.filePath("свежий-путь"), &err) == true);
    }
};
QTEST_MAIN(TestSsh)
#include "test_ssh.moc"
