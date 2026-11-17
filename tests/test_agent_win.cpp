#include "app/Application.h"
#include "platform/AgentPipe.h"
#include "ui/AgentAccess.h"
#include <QApplication>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

using namespace tb;

namespace {
constexpr auto Password = "synthetic-master-password";
constexpr auto EndpointSuffix = ".test";

QByteArray find(const QString& title) {
    return QJsonDocument(QJsonObject{{"v", 1}, {"op", "find"}, {"title", title}}).toJson(QJsonDocument::Compact);
}

QByteArray read(const model::Id& id, const QString& field, int line = 0) {
    QJsonObject request{{"v", 1}, {"op", "read"}, {"id", id.toString(QUuid::WithoutBraces)}, {"field", field}};
    if (line) request.insert("line", line);
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

struct Response {
    QByteArray raw, standardError;
    int exitCode;
    QJsonObject json() const { return QJsonDocument::fromJson(raw).object(); }
    QString error() const { return json().value("error").toString(); }
};

QString helperPath() { return QCoreApplication::applicationDirPath() + "/TokenBucket-agent-test.exe"; }

Response query(const QByteArray& request, const std::function<void(QProcess&)>& drive = {}) {
    QProcess client;
    QTimer driver;
    if (drive) {
        QObject::connect(&driver, &QTimer::timeout, &client, [&] { drive(client); });
        driver.start(10);
    }
    client.setProgram(helperPath());
    client.start();
    if (!client.waitForStarted(5000)) return {{}, {}, -1};
    client.write(request + '\n');
    client.closeWriteChannel();
    QEventLoop loop;
    QObject::connect(&client, &QProcess::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    if (client.state() != QProcess::NotRunning) loop.exec();
    if (client.state() != QProcess::NotRunning) {
        client.kill();
        client.waitForFinished(5000);
        return {{}, {}, -1};
    }
    return {client.readAllStandardOutput(), client.readAllStandardError(), client.exitCode()};
}

void sendMcp(QProcess& child, const QString& method, QJsonObject params = {}, QJsonValue id = QJsonValue::Undefined) {
    QJsonObject request{{"jsonrpc", "2.0"}, {"method", method}};
    if (!id.isUndefined()) request.insert("id", id);
    if (!params.isEmpty()) request.insert("params", params);
    child.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

QJsonObject receiveMcp(QProcess& child, const std::function<void()>& drive = {}) {
    QEventLoop loop;
    QTimer driver;
    if (drive) {
        QObject::connect(&driver, &QTimer::timeout, &loop, drive);
        driver.start(10);
    }
    QObject::connect(&child, &QProcess::readyReadStandardOutput, &loop, [&] { if (child.canReadLine()) loop.quit(); });
    QObject::connect(&child, &QProcess::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    if (!child.canReadLine() && child.state() != QProcess::NotRunning) loop.exec();
    return QJsonDocument::fromJson(child.readLine()).object();
}

QJsonObject toolOutput(const QJsonObject& response) {
    const auto content = response.value("result").toObject().value("content").toArray();
    return QJsonDocument::fromJson(content.at(0).toObject().value("text").toString().toUtf8()).object();
}

QJsonObject modernMcpMetadata() {
    return {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
        {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}};
}
}

class TestAgent : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setQuitOnLastWindowClosed(false); }

    void rejectsExtraStdinRecords() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        agent::Access access(app, EndpointSuffix);
        QVERIFY(access.start());
        const auto request = find("first synthetic request");
        QCOMPARE(query(request).error(), QStringLiteral("locked"));
        QTest::qWait(50);
        const auto response = query(request + '\n' + find("second synthetic request"));
        QCOMPARE(response.exitCode, 1);
        QCOMPARE(response.error(), QStringLiteral("invalid_request"));
    }

    void protocolAndOwnerApproval() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password, &error), qPrintable(error));
        const auto group = QUuid::createUuid();
        QVERIFY(app.model()->addNode(group, {}, "synthetic data").isEmpty());
        model::Entry ssh, document, token, credentials, value;
        ssh.groupId = document.groupId = token.groupId = credentials.groupId = value.groupId = group;
        ssh.id = QUuid::createUuid();
        ssh.type = model::EntryType::SshKey;
        ssh.title = QStringLiteral("<b>synthetic-key</b>\n%1‮");
        ssh.sshPrivate = QByteArray(model::VaultModel::MaxSecretBytes, 'x');
        ssh.sshPrivate.replace(0, 11, QByteArray("private\r\n\0\xff", 11));
        ssh.sshPublic = QByteArray("public\r\n\x80\0", 10);
        ssh.sshPassphrase = QStringLiteral("тестовая фраза");
        document.id = QUuid::createUuid();
        document.type = model::EntryType::TextFile;
        document.title = "synthetic document";
        document.textFile = {"sample.txt", QByteArray::fromHex("efbbbf") + QStringLiteral("видно\r\nhidden-secret\n").toUtf8(), false, {0}};
        token.id = QUuid::createUuid();
        token.title = "synthetic token";
        token.token = "synthetic-token-value";
        credentials.id = QUuid::createUuid();
        credentials.type = model::EntryType::Credentials;
        credentials.title = "synthetic credentials";
        credentials.login = "synthetic-login";
        credentials.password = "synthetic-password";
        value.id = QUuid::createUuid();
        value.type = model::EntryType::PlainValue;
        value.title = "synthetic value";
        value.plainValue = QStringLiteral("пример");
        for (const auto& entry : {ssh, document, token, credentials, value}) QVERIFY(app.model()->setEntry(entry).isEmpty());
        QVERIFY2(app.save(&error), qPrintable(error));

        agent::Access access(app, EndpointSuffix);
        QVERIFY(access.start());
        agent::Access collision(app, EndpointSuffix);
        QVERIFY(!collision.start());
        for (const QByteArray& invalid : {QByteArray(R"({"v":1,"op":"find","title":"x","extra":0})"),
                QByteArray(R"({"v":1,"op":"read","id":"all","field":"token"})"),
                QByteArray(R"({"v":1,"\u0076":1,"op":"find","title":"x"})"),
                QByteArray(R"({"v":1,"op":"read","id":"01234567-89ab-4def-8123-456789abcdef","field":"textLine","line":1.5})"),
                QByteArray(R"({"v":2,"op":"find","title":"x"})"),
                QByteArray(R"([{ "v":1,"op":"find","title":"x"}])"),
                QByteArray("  \t"), QByteArray(), QByteArray(agent::MaxRequest + 1, 'a')}) {
            const auto response = query(invalid);
            QCOMPARE(response.exitCode, 1);
            QCOMPARE(response.error(), QStringLiteral("invalid_request"));
            QVERIFY(response.standardError.isEmpty());
            QTest::qWait(30);
        }

        bool consentSeen = false, passwordSeen = false;
        const auto grant = [&](QProcess&) {
            if (auto* password = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                passwordSeen = true;
                password->setTextValue(QString::fromLatin1(Password));
                password->accept();
            } else if (!consentSeen) {
                if (auto* approval = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    consentSeen = true;
                    approval->button(QMessageBox::Yes)->click();
                }
            }
        };
        const auto denied = query(find(ssh.title), [&](QProcess&) {
            if (!consentSeen) if (auto* approval = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                consentSeen = true;
                QCOMPARE(approval->textFormat(), Qt::PlainText);
                QCOMPARE(static_cast<QAbstractButton*>(approval->defaultButton()), approval->button(QMessageBox::No));
                QVERIFY(approval->text().contains("<b>synthetic-key</b>"));
                QVERIFY(approval->text().contains("\\u{a}"));
                QVERIFY(approval->text().contains("\\u{202e}"));
                approval->defaultButton()->click();
            }
        });
        QVERIFY(consentSeen);
        QCOMPARE(denied.error(), QStringLiteral("denied"));
        QTest::qWait(780);
        consentSeen = false;
        const auto found = query(find(ssh.title), grant);
        QCOMPARE(found.exitCode, 0);
        QVERIFY(consentSeen && !passwordSeen);
        QCOMPARE(found.json().value("entry").toObject(), (QJsonObject{{"title", ssh.title},
            {"id", ssh.id.toString(QUuid::WithoutBraces)}, {"type", "sshKey"}}));

        struct FieldCase { model::Id id; QString field; int line; QByteArray value; };
        const FieldCase cases[] = {
            {ssh.id, "sshPrivate", 0, ssh.sshPrivate},
            {ssh.id, "sshPublic", 0, ssh.sshPublic},
            {ssh.id, "sshPassphrase", 0, ssh.sshPassphrase.toUtf8()},
            {token.id, "token", 0, token.token.toUtf8()},
            {credentials.id, "login", 0, credentials.login.toUtf8()},
            {credentials.id, "password", 0, credentials.password.toUtf8()},
            {value.id, "plainValue", 0, value.plainValue.toUtf8()},
            {document.id, "textLine", 1, QStringLiteral("видно").toUtf8()},
        };
        for (const auto& field : cases) {
            consentSeen = passwordSeen = false;
            const auto reply = query(read(field.id, field.field, field.line), grant);
            QCOMPARE(reply.exitCode, 0);
            QVERIFY(consentSeen && passwordSeen);
            QVERIFY(reply.standardError.isEmpty());
            QCOMPARE(reply.json(), (QJsonObject{{"ok", true}, {"encoding", "base64"},
                {"value", QString::fromLatin1(field.value.toBase64())}}));
        }
        QTest::qWait(780);
        consentSeen = passwordSeen = false;
        QCOMPARE(query(read(document.id, "textLine", 2), grant).error(), QStringLiteral("unavailable_field"));
        QVERIFY(consentSeen && !passwordSeen);
        QTest::qWait(780);
        consentSeen = false;
        QCOMPARE(query(read(ssh.id, "token"), grant).error(), QStringLiteral("unavailable_field"));
        QVERIFY(consentSeen && !passwordSeen);

        QTest::qWait(780);
        consentSeen = passwordSeen = false;
        const auto cancelled = query(read(ssh.id, "sshPrivate"), [&](QProcess& process) {
            if (auto* password = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                passwordSeen = true;
                password->reject();
            } else grant(process);
        });
        QVERIFY(consentSeen && passwordSeen);
        QCOMPARE(cancelled.error(), QStringLiteral("denied"));
        QTest::qWait(780);
        consentSeen = passwordSeen = false;
        const auto wrongPassword = query(read(ssh.id, "sshPublic"), [&](QProcess& process) {
            if (auto* password = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                passwordSeen = true;
                password->setTextValue("synthetic-wrong-password");
                password->accept();
            } else if (passwordSeen) {
                if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                    if (auto* ok = message->button(QMessageBox::Ok)) ok->click();
            } else grant(process);
        });
        QVERIFY(consentSeen && passwordSeen);
        QCOMPARE(wrongPassword.error(), QStringLiteral("denied"));
        app.lock();
        QTest::qWait(30);
        QCOMPARE(query(find(ssh.title)).error(), QStringLiteral("locked"));
    }

    void interruptionDiscardsResults() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password, &error), qPrintable(error));
        const auto group = QUuid::createUuid();
        QVERIFY(app.model()->addNode(group, {}, "synthetic data").isEmpty());
        model::Entry token;
        token.groupId = group;
        token.id = QUuid::createUuid();
        token.title = "synthetic token";
        token.token = "first synthetic value";
        QVERIFY(app.model()->setEntry(token).isEmpty());

        std::function<bool(QWidget*)> decision;
        agent::Access access(app, EndpointSuffix, [&](QWidget* widget) { return decision(widget); });
        QVERIFY(access.start());
        QString parallelError, editError;
        decision = [&](QWidget*) {
            parallelError = query(find(token.title)).error();
            token.token = "changed synthetic value";
            editError = app.model()->setEntry(token);
            return true;
        };
        QCOMPARE(query(find(token.title)).error(), QStringLiteral("stale"));
        QCOMPARE(parallelError, QStringLiteral("unavailable"));
        QVERIFY(editError.isEmpty());
        QTest::qWait(780);

        bool inConsent = false;
        decision = [&](QWidget* widget) {
            inConsent = true;
            return static_cast<QMessageBox*>(widget)->exec() == QMessageBox::Yes;
        };
        const auto disconnected = query(find(token.title), [&](QProcess& process) {
            if (inConsent) process.kill();
        });
        QVERIFY(inConsent);
        QVERIFY(disconnected.raw.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!QApplication::activeModalWidget(), 1500);
        QTest::qWait(780);

        auto idleClient = agent::Handle(agent::openClient(agent::Endpoint::current(), EndpointSuffix));
        QVERIFY(idleClient.get() != INVALID_HANDLE_VALUE);
        QTest::qWait(agent::RequestTimeoutMs + 150);
        idleClient.reset();
        decision = [&](QWidget*) { app.lock(); return true; };
        const auto locked = query(find(token.title));
        QVERIFY(locked.raw.isEmpty() || !locked.json().value("ok").toBool());
        QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
    }

    void requiresHelloWhenVaultUsesHello() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        std::atomic<int> verificationCount = 0;
        std::atomic<bool> delayHello = false, finishHello = false;
        Application app(directory.path(), nullptr, [&](void*, const QString&) {
            ++verificationCount;
            if (delayHello) {
                const QDeadlineTimer limit(3000);
                while (!finishHello && !limit.hasExpired()) QThread::msleep(10);
            }
            return platform::hello::Result::Verified;
        });
        QString error;
        QVERIFY2(app.createHelloVault(&error), qPrintable(error));
        QCOMPARE(verificationCount.load(), 1);
        model::Entry token;
        token.groupId = QUuid::createUuid();
        token.id = QUuid::createUuid();
        token.title = "Hello-protected test token";
        token.token = "synthetic-hello-token-value";
        QVERIFY(app.model()->addNode(token.groupId, {}, "synthetic data").isEmpty());
        QVERIFY(app.model()->setEntry(token).isEmpty());
        QVERIFY2(app.save(&error), qPrintable(error));

        agent::Access access(app, EndpointSuffix, [](QWidget*) { return true; });
        QVERIFY(access.start());
        const auto response = query(read(token.id, "token"));
        QCOMPARE(response.exitCode, 0);
        QCOMPARE(verificationCount.load(), 2);
        QCOMPARE(QByteArray::fromBase64(response.json().value("value").toString().toLatin1()), token.token.toUtf8());

        delayHello = true;
        bool killed = false;
        QTest::qWait(780);
        const auto cancelled = query(read(token.id, "token"), [&](QProcess& process) {
            if (verificationCount == 3 && !killed) {
                killed = true;
                process.kill();
                QTimer::singleShot(300, &app, [&] { finishHello = true; });
            }
        });
        QVERIFY(killed);
        QVERIFY(finishHello.load());
        QVERIFY(cancelled.raw.isEmpty());
        delayHello = false;
        QTest::qWait(780);
        QCOMPARE(query(read(token.id, "token")).exitCode, 0);
        QCOMPARE(verificationCount.load(), 4);
    }

    void mcpLegacyDiscoveryAndCalls() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password, &error), qPrintable(error));
        model::Entry ssh;
        ssh.groupId = QUuid::createUuid();
        ssh.id = QUuid::createUuid();
        ssh.type = model::EntryType::SshKey;
        ssh.title = "synthetic deployment SSH";
        ssh.sshPrivate = QByteArray("private\r\n\0\xff", 11);
        ssh.sshPublic = "synthetic-public-key";
        ssh.sshPassphrase = "synthetic-ssh-passphrase";
        QVERIFY(app.model()->addNode(ssh.groupId, {}, "synthetic MCP data").isEmpty());
        QVERIFY(app.model()->setEntry(ssh).isEmpty());
        QVERIFY2(app.save(&error), qPrintable(error));
        agent::Access access(app, EndpointSuffix);
        QVERIFY(access.start());

        QProcess child;
        child.start(helperPath(), {"--mcp"});
        QVERIFY(child.waitForStarted(5000));
        sendMcp(child, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "TokenBucket synthetic client"}, {"version", "1"}}}}, 1);
        const auto initialized = receiveMcp(child);
        QCOMPARE(initialized.value("jsonrpc").toString(), QStringLiteral("2.0"));
        QCOMPARE(initialized.value("id").toInt(), 1);
        const auto server = initialized.value("result").toObject();
        QCOMPARE(server.value("protocolVersion").toString(), QStringLiteral("2025-11-25"));
        QVERIFY(server.value("capabilities").toObject().value("tools").isObject());
        QVERIFY(server.value("serverInfo").toObject().value("name").isString());

        sendMcp(child, "notifications/initialized");
        sendMcp(child, "ping", {}, "ping-id");
        const auto pong = receiveMcp(child);
        QCOMPARE(pong.value("id").toString(), QStringLiteral("ping-id"));
        QVERIFY(pong.value("result").isObject());
        sendMcp(child, "tools/list", {}, 2);
        const auto tools = receiveMcp(child).value("result").toObject().value("tools").toArray();
        QCOMPARE(tools.size(), 2);
        QCOMPARE(tools.at(0).toObject().value("name").toString(), QStringLiteral("tokenbucket_find"));
        QCOMPARE(tools.at(1).toObject().value("name").toString(), QStringLiteral("tokenbucket_read"));
        QVERIFY(tools.at(1).toObject().value("inputSchema").toObject().value("properties").toObject().contains("field"));
        sendMcp(child, "vault/unlock", {}, 3);
        QCOMPARE(receiveMcp(child).value("error").toObject().value("code").toInt(), -32601);
        sendMcp(child, "tools/call", {{"name", "tokenbucket_find"},
            {"arguments", QJsonObject{{"title", ssh.title}, {"op", "read"}}}}, 4);
        QCOMPARE(receiveMcp(child).value("error").toObject().value("code").toInt(), -32602);
        QVERIFY(!QApplication::activeModalWidget());

        int approvals = 0, verifications = 0;
        const auto approve = [&] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                ++verifications;
                dialog->setTextValue(QString::fromLatin1(Password));
                dialog->accept();
            } else if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if (auto* yes = dialog->button(QMessageBox::Yes)) {
                    ++approvals;
                    yes->click();
                }
            }
        };
        sendMcp(child, "tools/call", {{"name", "tokenbucket_find"}, {"arguments", QJsonObject{{"title", ssh.title}}}}, 5);
        const auto found = toolOutput(receiveMcp(child, approve)).value("entry").toObject();
        QCOMPARE(found.value("id").toString(), ssh.id.toString(QUuid::WithoutBraces));
        const QJsonObject readArgs{{"name", "tokenbucket_read"},
            {"arguments", QJsonObject{{"id", found.value("id")}, {"field", "sshPrivate"}}}};
        sendMcp(child, "tools/call", readArgs, 6);
        const auto result = receiveMcp(child, approve);
        QCOMPARE(result.value("id").toInt(), 6);
        QCOMPARE(approvals, 2);
        QCOMPARE(verifications, 1);
        QCOMPARE(toolOutput(result), (QJsonObject{{"ok", true}, {"encoding", "base64"},
            {"value", QString::fromLatin1(ssh.sshPrivate.toBase64())}}));
        app.lock();
        sendMcp(child, "tools/call", readArgs, 7);
        const auto locked = receiveMcp(child);
        QVERIFY(locked.value("result").toObject().value("isError").toBool());
        QCOMPARE(toolOutput(locked).value("error").toString(), QStringLiteral("locked"));
        child.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 3000);
        QVERIFY(child.readAllStandardError().isEmpty());
    }

    void mcpModernCancellationAndEof() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password, &error), qPrintable(error));
        agent::Access access(app, EndpointSuffix);
        QVERIFY(access.start());

        QProcess child;
        child.start(helperPath(), {"--mcp"});
        QVERIFY(child.waitForStarted(5000));
        child.write("{not-json\n");
        const auto invalid = receiveMcp(child);
        QCOMPARE(invalid.value("error").toObject().value("code").toInt(), -32700);
        const auto meta = modernMcpMetadata();
        const QJsonObject params{{"_meta", meta}};
        sendMcp(child, "server/discover", params, 1);
        const auto discovery = receiveMcp(child).value("result").toObject();
        QCOMPARE(discovery.value("resultType").toString(), QStringLiteral("complete"));
        QVERIFY(discovery.value("supportedVersions").toArray().contains("2026-07-28"));
        QVERIFY(discovery.value("_meta").toObject().value("io.modelcontextprotocol/serverInfo").isObject());
        QVERIFY(discovery.value("ttlMs").isDouble());
        QCOMPARE(discovery.value("cacheScope").toString(), QStringLiteral("public"));
        sendMcp(child, "tools/list", {}, 2);
        QCOMPARE(receiveMcp(child).value("error").toObject().value("code").toInt(), -32602);
        sendMcp(child, "tools/list", params, 3);
        const auto catalog = receiveMcp(child).value("result").toObject();
        QCOMPARE(catalog.value("tools").toArray().size(), 2);
        QVERIFY(catalog.value("ttlMs").isDouble());
        QCOMPARE(catalog.value("cacheScope").toString(), QStringLiteral("public"));
        auto unsupported = meta;
        unsupported.insert("io.modelcontextprotocol/protocolVersion", "unknown-version");
        sendMcp(child, "tools/list", {{"_meta", unsupported}}, 4);
        const auto unsupportedError = receiveMcp(child).value("error").toObject();
        QCOMPARE(unsupportedError.value("code").toInt(), -32022);
        QCOMPARE(unsupportedError.value("data").toObject().value("requested").toString(), QStringLiteral("unknown-version"));

        const QJsonObject findArgs{{"_meta", meta}, {"name", "tokenbucket_find"},
            {"arguments", QJsonObject{{"title", "synthetic absent title"}}}};
        sendMcp(child, "tools/call", findArgs, 5);
        bool cancelled = false;
        const auto next = receiveMcp(child, [&] {
            if (!cancelled && qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                cancelled = true;
                sendMcp(child, "notifications/cancelled", {{"requestId", 5}});
                sendMcp(child, "tools/list", params, 6);
            }
        });
        QVERIFY(cancelled);
        QCOMPARE(next.value("id").toInt(), 6);
        QCOMPARE(next.value("result").toObject().value("resultType").toString(), QStringLiteral("complete"));
        QTRY_VERIFY_WITH_TIMEOUT(!QApplication::activeModalWidget(), 1500);
        QTest::qWait(780);
        QVERIFY(!child.canReadLine());

        sendMcp(child, "tools/call", findArgs, 7);
        bool closed = false;
        const auto afterEof = receiveMcp(child, [&] {
            if (!closed && qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                closed = true;
                child.closeWriteChannel();
            }
        });
        QVERIFY(closed);
        QVERIFY(afterEof.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!QApplication::activeModalWidget(), 1500);
        QVERIFY(child.readAllStandardOutput().isEmpty());
        QVERIFY(child.readAllStandardError().isEmpty());
    }

    void mcpProcessesQueuedCancellationBeforeResult() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password, &error), qPrintable(error));
        model::Entry token;
        token.groupId = QUuid::createUuid();
        token.id = QUuid::createUuid();
        token.title = "synthetic cancellable metadata";
        QVERIFY(app.model()->addNode(token.groupId, {}, "synthetic MCP data").isEmpty());
        QVERIFY(app.model()->setEntry(token).isEmpty());
        agent::Access access(app, EndpointSuffix);
        QVERIFY(access.start());

        QProcess child;
        child.start(helperPath(), {"--mcp"});
        QVERIFY(child.waitForStarted(5000));
        const auto meta = modernMcpMetadata();
        sendMcp(child, "tools/call", {{"_meta", meta}, {"name", "tokenbucket_find"},
            {"arguments", QJsonObject{{"title", token.title}}}}, 1);
        bool sent = false;
        const auto response = receiveMcp(child, [&] {
            if (sent) return;
            if (auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                sent = true;
                sendMcp(child, "notifications/unknown", {{"padding", QString(60 * 1024, 'x')}});
                sendMcp(child, "notifications/cancelled", {{"requestId", 1}});
                sendMcp(child, "tools/list", {{"_meta", meta}}, 2);
                QTimer::singleShot(30, prompt, [prompt] { prompt->button(QMessageBox::Yes)->click(); });
            }
        });
        QVERIFY(sent);
        QCOMPARE(response.value("id").toInt(), 2);
        child.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 3000);
        QVERIFY(child.readAllStandardOutput().isEmpty());
    }

    void mcpEofExitsWithFullOutputPipe() {
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE rawInput = nullptr, rawWriter = nullptr, rawReader = nullptr, rawOutput = nullptr;
        QVERIFY(CreatePipe(&rawInput, &rawWriter, &inheritable, 4096));
        agent::Handle input(rawInput), writer(rawWriter);
        QVERIFY(CreatePipe(&rawReader, &rawOutput, &inheritable, 4096));
        agent::Handle reader(rawReader), output(rawOutput);
        QVERIFY(SetHandleInformation(writer.get(), HANDLE_FLAG_INHERIT, 0));
        QVERIFY(SetHandleInformation(reader.get(), HANDLE_FLAG_INHERIT, 0));
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input.get();
        startup.hStdOutput = output.get();
        startup.hStdError = output.get();
        PROCESS_INFORMATION process{};
        const auto program = helperPath().toStdWString();
        auto command = (QStringLiteral("\"") + helperPath() + QStringLiteral("\" --mcp")).toStdWString();
        QVERIFY(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process));
        agent::Handle child(process.hProcess), thread(process.hThread);
        const auto cleanup = qScopeGuard([&] {
            if (WaitForSingleObject(child.get(), 0) == WAIT_TIMEOUT) TerminateProcess(child.get(), 1);
            WaitForSingleObject(child.get(), 3000);
        });
        input.reset();
        output.reset();

        QByteArray requests;
        for (int id = 1; id <= 16; ++id) {
            const QJsonObject list{{"jsonrpc", "2.0"}, {"method", "tools/list"}, {"id", id},
                {"params", QJsonObject{{"_meta", modernMcpMetadata()}}}};
            requests += QJsonDocument(list).toJson(QJsonDocument::Compact) + '\n';
        }
        QVERIFY(requests.size() <= 4096);
        DWORD written = 0;
        QVERIFY(WriteFile(writer.get(), requests.constData(), static_cast<DWORD>(requests.size()), &written, nullptr));
        QCOMPARE(written, static_cast<DWORD>(requests.size()));
        QTest::qWait(250); // The reader deliberately never consumes stdout.
        QCOMPARE(WaitForSingleObject(child.get(), 0), DWORD(WAIT_TIMEOUT));
        writer.reset();
        QCOMPARE(WaitForSingleObject(child.get(), 1500), DWORD(WAIT_OBJECT_0));
    }

    void mcpModernTrafficRejectsLegacyInitialize() {
        QProcess child;
        child.start(helperPath(), {"--mcp"});
        QVERIFY(child.waitForStarted(5000));
        sendMcp(child, "initialize", {{"_meta", modernMcpMetadata()}, {"protocolVersion", "2025-11-25"},
            {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "synthetic"}, {"version", "1"}}}}, 1);
        QCOMPARE(receiveMcp(child).value("error").toObject().value("code").toInt(), -32601);
        child.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 3000);
    }

    void mcpMalformedRequestKeepsUnambiguousId() {
        QProcess child;
        child.start(helperPath(), {"--mcp"});
        QVERIFY(child.waitForStarted(5000));
        child.write("{\"jsonrpc\":\"2.0\",\"id\":\"synthetic-request\",\"method\":17}\n");
        const auto invalid = receiveMcp(child);
        QCOMPARE(invalid.value("error").toObject().value("code").toInt(), -32600);
        QCOMPARE(invalid.value("id").toString(), QStringLiteral("synthetic-request"));
        child.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"id\":2,\"method\":\"tools/list\"}\n");
        const auto duplicate = receiveMcp(child);
        QCOMPARE(duplicate.value("error").toObject().value("code").toInt(), -32600);
        QVERIFY(!duplicate.contains("id"));
        child.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 3000);
    }
};

QTEST_MAIN(TestAgent)
#include "test_agent_win.moc"
