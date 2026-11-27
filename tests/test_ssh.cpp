#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <filesystem>
#include "../src/app/Application.h"
#include "../src/platform/FileOps.h"
#include "FaultFileOps.h"
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
        const auto exportError = store::exportFile(out, app.model()->entry(e.id)->sshPrivate, false);
        QVERIFY2(exportError.isEmpty(), qPrintable(exportError));
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
        FaultOps ops;
        const auto linkError = store::exportFile(link, "secret", true, ops);
        QVERIFY(!linkError.isEmpty());
        QCOMPARE(ops.writeCalls, 0);
        QFile original(realF);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("x"));

        const auto realDirectory = dir_.filePath("directory");
        const auto linkedDirectory = dir_.filePath("directory-link");
        QVERIFY(QDir().mkdir(realDirectory));
        std::filesystem::create_directory_symlink(realDirectory.toStdWString(), linkedDirectory.toStdWString(), ec);
        QVERIFY2(!ec, qPrintable(QString::fromStdString(ec.message())));
        QCOMPARE(store::exportFile(QDir(linkedDirectory).filePath("output"), "secret", false, ops), linkError);
        QCOMPARE(ops.writeCalls, 0);
        QVERIFY(!QFileInfo::exists(QDir(realDirectory).filePath("output")));
    }
    void failedExportPreservesOriginal_data() {
        QTest::addColumn<bool>("collision");
        QTest::newRow("rename-failed") << false;
        QTest::newRow("staging-name-in-use") << true;
    }
    void failedExportPreservesOriginal() {
        QFETCH(bool, collision);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("private.key");
        QVERIFY(store::realFileOps().writeAll(path, "original"));
        FaultOps ops;
        ops.failRenameCall = 1;
        QString staged;
        ops.beforeWrite = [&](const QString& name) {
            staged = name;
            QVERIFY(!QFileInfo(name).fileName().startsWith(QFileInfo(path).fileName()));
            if (collision) QVERIFY(store::realFileOps().writeAll(name, "other-file"));
        };
        const auto error = store::exportFile(path, "new-secret", true, ops);
        QVERIFY(!error.isEmpty());
        QVERIFY(!staged.isEmpty() && staged != path);
        QCOMPARE(QFileInfo(staged).absolutePath(), QFileInfo(path).absolutePath());
        QCOMPARE(ops.renameCalls, collision ? 0 : 1);
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("original"));
        if (collision) {
            QFile foreign(staged);
            QVERIFY(foreign.open(QIODevice::ReadOnly));
            QCOMPARE(foreign.readAll(), QByteArray("other-file"));
        } else {
            QVERIFY(!QFileInfo::exists(staged));
        }
    }
};
QTEST_MAIN(TestSsh)
#include "test_ssh.moc"
