#include "app/Application.h"
#include "platform/AgentPipe.h"
#include "platform/CrashPolicy.h"
#include "platform/SshExec.h"
#include "ui/AgentAccess.h"
#include "ui/MainWindow.h"
#include "ui/SettingsDialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
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
#include <cstring>
#include <tuple>

using namespace tb;

namespace {
constexpr auto Password = "synthetic-master-password";
constexpr auto EndpointSuffix = ".test";
constexpr auto SshSyntheticKey = "synthetic-private-canary";
constexpr auto SshSyntheticPhrase = "synthetic-askpass-canary";

QDir scratchTemp() { return QDir(qEnvironmentVariable("LOCALAPPDATA") + "/Temp"); }
QStringList scratchStages(qint64 pid = GetCurrentProcessId()) {
    return scratchTemp().entryList({QStringLiteral("TokenBucket-ssh-%1-*").arg(pid)}, QDir::Dirs);
}

QString runningStage(const QStringList& stages, const QStringList& baseline = {}) {
    for (const auto& item : stages) {
        if (!baseline.contains(item) && QFile::exists(scratchTemp().filePath(item + "/diagnostic"))) return item;
    }
    return {};
}

int fakeSsh(const QStringList& arguments) {
    const auto identity = arguments.indexOf("-i");
    const auto destination = arguments.indexOf("--");
    if (identity < 0 || arguments.value(identity + 1) != "identity" || destination < 0 ||
        destination + 3 != arguments.size() || arguments.value(destination + 1) != "node.example" ||
        arguments.value(arguments.indexOf("-l") + 1) != "owner" || arguments.value(arguments.indexOf("-p") + 1) != "2222" ||
        !arguments.contains("IdentityFile=none") || !arguments.contains("IdentityAgent=none") ||
        !arguments.contains("StrictHostKeyChecking=yes") || !arguments.contains("GlobalKnownHostsFile=none") ||
        !arguments.contains("ClearAllForwardings=yes") || !arguments.contains("PasswordAuthentication=no") ||
        !arguments.contains("KbdInteractiveAuthentication=no") || !arguments.contains("PreferredAuthentications=publickey") ||
        qEnvironmentVariableIsSet("TOKENBUCKET_TEST_NO_INHERIT")) return 70;
    QFile key("identity");
    if (!key.open(QIODevice::ReadOnly) || key.readAll() != SshSyntheticKey) return 71;
    key.close();
    const auto command = arguments.last();
    if (command == "askpass" || command == "wrong askpass") {
        if (qEnvironmentVariable("SSH_ASKPASS_REQUIRE") != "force" ||
            !arguments.contains("BatchMode=no") || !arguments.contains("NumberOfPasswordPrompts=1")) return 72;
        QProcess child;
        child.start(qEnvironmentVariable("SSH_ASKPASS"), {command == "askpass" ?
            "Enter passphrase for key 'identity': " : "Enter passphrase for key 'other/identity': "});
        if (!child.waitForFinished(4000)) return 73;
        auto bytes = child.readAllStandardOutput();
        const bool correct = bytes == SshSyntheticPhrase;
        SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
        if (command == "askpass" && (!correct || child.exitCode() != 0)) return 74;
        if (command == "wrong askpass" && (!bytes.isEmpty() || child.exitCode() == 0)) return 75;
        if (command == "askpass") {
            child.start(qEnvironmentVariable("SSH_ASKPASS"), {"Enter passphrase for key 'identity': "});
            if (!child.waitForFinished(4000) || child.exitCode() == 0 || !child.readAllStandardOutput().isEmpty()) return 76;
        }
    } else if (qEnvironmentVariableIsSet("SSH_ASKPASS") || qEnvironmentVariableIsSet("TOKENBUCKET_SSH_ASKPASS") ||
        qEnvironmentVariable("SSH_ASKPASS_REQUIRE") != "never" || !arguments.contains("BatchMode=yes")) return 77;
    if (command == "hold" || command == "hold tree") {
        QFile marker("diagnostic");
        if (!marker.open(QIODevice::WriteOnly)) return 78;
        qint64 child = 0;
        if (command == "hold tree" &&
            !QProcess::startDetached(QCoreApplication::applicationFilePath(), {"--ssh-fixture-child"}, {}, &child)) return 79;
        marker.write(QByteArray::number(child));
        marker.close();
        Sleep(12000);
    }
    QFile out, err;
    if (!out.open(stdout, QIODevice::WriteOnly) || !err.open(stderr, QIODevice::WriteOnly)) return 80;
    if (command == "output limit") {
        out.write(QByteArray(agent::MaxSshOutputBytes / 2 + 1, 'a'));
        err.write(QByteArray(agent::MaxSshOutputBytes / 2 + 1, 'b'));
        return 0;
    }
    out.write(command.toUtf8());
    err.write(QDir::toNativeSeparators(QDir::currentPath()).toUtf8());
    if (command == "exit 255") return 255;
    if (command == "exit 7") return 7;
    return 0;
}

QByteArray find(const QString& title) {
    return QJsonDocument(QJsonObject{{"v", 1}, {"op", "find"}, {"title", title}}).toJson(QJsonDocument::Compact);
}

QByteArray findSsh(const QString& machine, const QString& user = {}, int port = 0) {
    QJsonObject request{{"v", 1}, {"op", "find_ssh"}, {"machine", machine}};
    if (!user.isEmpty()) request.insert("user", user);
    if (port) request.insert("port", port);
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
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

QJsonObject sshArguments(model::Id id, const QString& command = "hostname", const QString& machine = "Known",
                         const QString& user = "owner", int port = 2222, int timeoutSeconds = 5) {
    return {{"id", id.toString(QUuid::WithoutBraces)}, {"machine", machine}, {"user", user}, {"port", port},
        {"command", command}, {"timeoutSeconds", timeoutSeconds}};
}

QByteArray execRequest(QJsonObject args) {
    args.insert("v", 1);
    args.insert("op", "ssh_exec");
    return QJsonDocument(args).toJson(QJsonDocument::Compact);
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

    void sshLookupShowsOnlyApprovedMatchingBindings() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password, &error), qPrintable(error));
        model::Entry key;
        key.groupId = QUuid::createUuid(); key.id = QUuid::createUuid(); key.type = model::EntryType::SshKey;
        key.title = "A shared SSH key"; key.sshPrivate = "synthetic private key bytes";
        key.sshTargets = {{"Build", "node.example", "Build", 2222}, {"Ops", "node.example", "ops", 22},
            {"Unrelated", "hidden.example", "root", 22}};
        auto second = key;
        second.id = QUuid::createUuid(); second.title = "B fallback SSH key";
        second.sshTargets = {{"Fallback", "node.example", "ops", 22}};
        QVERIFY(app.model()->addNode(key.groupId, {}, "synthetic machines").isEmpty());
        QVERIFY(app.model()->setEntry(key).isEmpty());
        QVERIFY(app.model()->setEntry(second).isEmpty());
        int decisions = 0;
        bool allow = false, edit = false;
        QString editError;
        agent::Access access(app, EndpointSuffix, [&](QWidget*) {
            ++decisions;
            if (edit) {
                key.sshTargets[1].username = "other-ops";
                editError = app.model()->setEntry(key);
            }
            return allow;
        });
        QVERIFY(access.start());
        QCOMPARE(query(findSsh("node.example", "ops", 22)).error(), QStringLiteral("denied"));
        QCOMPARE(decisions, 1);
        QTest::qWait(780);
        allow = true;
        const auto found = query(findSsh("NODE.EXAMPLE", "ops", 22));
        QCOMPARE(found.exitCode, 0);
        const auto entries = found.json().value("entries").toArray();
        QCOMPARE(entries.size(), 2);
        const QJsonObject matching{{"alias", "Ops"}, {"host", "node.example"}, {"user", "ops"}, {"port", 22}};
        QCOMPARE(entries.at(0).toObject(), (QJsonObject{{"id", key.id.toString(QUuid::WithoutBraces)}, {"title", key.title},
            {"type", "sshKey"}, {"targets", QJsonArray{matching}}}));
        QCOMPARE(entries.at(1).toObject().value("id").toString(), second.id.toString(QUuid::WithoutBraces));
        QVERIFY(!found.raw.contains("hidden.example"));
        QVERIFY(!found.raw.contains(key.sshPrivate));
        QCOMPARE(query(findSsh("node.example", "Build", 22)).error(), QStringLiteral("not_found"));
        QTest::qWait(780);
        edit = true;
        QCOMPARE(query(findSsh("node.example", "ops", 22)).error(), QStringLiteral("stale"));
        QVERIFY(editError.isEmpty());
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

    void sshExecIsDisabledByDefault() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password, &error), qPrintable(error));
        model::Entry key;
        key.groupId = QUuid::createUuid();
        key.id = QUuid::createUuid();
        key.type = model::EntryType::SshKey;
        key.title = "synthetic command key";
        key.sshPrivate = "synthetic-private-canary";
        key.sshPassphrase = "synthetic-passphrase-canary";
        key.sshTargets = {{"Local", "localhost", "owner", 22}};
        QVERIFY(app.model()->addNode(key.groupId, {}, "synthetic machines").isEmpty());
        QVERIFY(app.model()->setEntry(key).isEmpty());
        int decisions = 0;
        agent::Access access(app, EndpointSuffix, [&](QWidget*) {
            ++decisions;
            return false;
        });
        QVERIFY(access.start());
        const QJsonObject arguments{{"id", key.id.toString(QUuid::WithoutBraces)}, {"machine", "Local"},
            {"user", "owner"}, {"port", 22}, {"command", "hostname"}};
        auto direct = arguments;
        direct.insert("v", 1);
        direct.insert("op", "ssh_exec");
        const auto denied = query(QJsonDocument(direct).toJson(QJsonDocument::Compact));
        QCOMPARE(denied.error(), QStringLiteral("ssh_disabled"));
        QVERIFY(denied.standardError.isEmpty());

        QProcess child;
        child.start(helperPath(), {"--mcp"});
        QVERIFY(child.waitForStarted(5000));
        sendMcp(child, "tools/call", {{"_meta", modernMcpMetadata()}, {"name", "tokenbucket_ssh_exec"},
            {"arguments", arguments}}, 1);
        const auto response = receiveMcp(child);
        QVERIFY(response.value("result").toObject().value("isError").toBool());
        QCOMPARE(toolOutput(response).value("error").toString(), QStringLiteral("ssh_disabled"));
        const auto payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
        QVERIFY(!payload.contains(key.sshPrivate));
        QVERIFY(!payload.contains(key.sshPassphrase.toUtf8()));
        QCOMPARE(decisions, 0);
        child.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 3000);
        QVERIFY(child.readAllStandardError().isEmpty());
    }

    void sshSettingsEnableAndRevokeWithoutLegacyAccess() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password,&error),qPrintable(error));
        model::Entry key;
        key.groupId=QUuid::createUuid(); key.id=QUuid::createUuid(); key.type=model::EntryType::SshKey;
        key.title="synthetic settings key"; key.sshPrivate=SshSyntheticKey;
        key.sshTargets={{"Known","node.example","owner",2222}};
        QVERIFY(app.model()->addNode(key.groupId,{},"synthetic machines").isEmpty());
        QVERIFY(app.model()->setEntry(key).isEmpty());
        QVERIFY2(app.save(&error),qPrintable(error));
        QSettings preferences(directory.filePath("preferences.ini"),QSettings::IniFormat);
        QSettings run(directory.filePath("run.ini"),QSettings::IniFormat);
        agent::Access access(app,EndpointSuffix,{},false,QCoreApplication::applicationFilePath(),false);
        ui::MainWindow window(app,nullptr,&preferences,&run);
        window.setAgentAccess(&access);
        const auto setInSettings=[&](bool on,bool save) {
            bool found=false;
            QTimer::singleShot(0,&window,[&] {
                auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget());
                if(!dialog) return;
                auto* checkbox=dialog->findChild<QCheckBox*>(prefs::McpSshSetting);
                if(checkbox) {
                    found=true;
                    checkbox->setChecked(on);
                    if(save) dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
                }
                if(dialog->isVisible()) dialog->reject();
            });
            window.showSettings();
            return found;
        };
        const auto executionRequest=execRequest(sshArguments(key.id));
        QCOMPARE(query(executionRequest).error(),QStringLiteral("unavailable"));
        QVERIFY(setInSettings(true,false));
        QVERIFY(!access.sshEnabled());
        QVERIFY(!preferences.contains(prefs::McpSshSetting));
        QVERIFY(setInSettings(true,true));
        QVERIFY(access.sshEnabled());
        QVERIFY(prefs::read(preferences).mcpSshEnabled);

        const auto forbidden=read(key.id,"sshPrivate");
        QCOMPARE(query(forbidden).error(),QStringLiteral("agent_access_disabled"));
        QCOMPARE(query(find(key.title)).error(),QStringLiteral("agent_access_disabled"));
        QVERIFY(!QApplication::activeModalWidget());

        int approvals=0,verifications=0;
        const auto accept=[&](QProcess&) {
            if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                ++verifications;
                dialog->setTextValue(QString::fromLatin1(Password));
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            } else if(auto* consent=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if(auto* yes=consent->button(QMessageBox::Yes)) {++approvals; yes->click();}
            }
        };
        QCOMPARE(query(findSsh("known","owner",2222),accept).json().value("entries").toArray().size(),1);
        QCOMPARE(approvals,1); QCOMPARE(verifications,0);
        QCOMPARE(query(executionRequest,accept).json().value("stdout").toString(),QStringLiteral("hostname"));
        QCOMPARE(approvals,2); QCOMPARE(verifications,1);

        bool revokedApproval=false;
        const auto revoked=query(executionRequest,[&](QProcess&) {
            if(!revokedApproval && qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                revokedApproval=true;
                access.setSshEnabled(false);
                access.setSshEnabled(true);
            }
        });
        QVERIFY(revokedApproval);
        QCOMPARE(revoked.error(),QStringLiteral("ssh_disabled"));
        QCOMPARE(verifications,1);
        QTest::qWait(780);

        bool revokedVerification=false;
        const auto revokedVerify=query(executionRequest,[&](QProcess& process) {
            if(!revokedVerification && qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                revokedVerification=true;
                access.setSshEnabled(false);
                access.setSshEnabled(true);
            } else accept(process);
        });
        QVERIFY(revokedVerification);
        QCOMPARE(revokedVerify.error(),QStringLiteral("ssh_disabled"));
        QCOMPARE(verifications,1);
        QTest::qWait(780);

        const auto baseline=scratchStages();
        bool revokedExecution=false;
        const auto revokedRun=query(execRequest(sshArguments(key.id,"hold")),[&](QProcess& process) {
            if(!revokedExecution && !runningStage(scratchStages(),baseline).isEmpty()) {
                revokedExecution=true;
                access.setSshEnabled(false);
                access.setSshEnabled(true);
            } else accept(process);
        });
        QVERIFY(revokedExecution);
        QCOMPARE(revokedRun.error(),QStringLiteral("ssh_disabled"));
        QVERIFY(!revokedRun.json().contains("stdout"));
        QVERIFY(access.sshEnabled());
        QCOMPARE(scratchStages(),baseline);

        bool savedOff=false;
        const auto cancelled=query(execRequest(sshArguments(key.id,"hold")),[&](QProcess& process) {
            if(!savedOff && !runningStage(scratchStages(),baseline).isEmpty()) {
                savedOff=true;
                QVERIFY(setInSettings(false,true));
            } else accept(process);
        });
        QVERIFY(savedOff);
        QCOMPARE(cancelled.error(),QStringLiteral("ssh_disabled"));
        QVERIFY(!cancelled.json().contains("stdout"));
        QVERIFY(!access.sshEnabled());
        QCOMPARE(verifications,3);
        QCOMPARE(scratchStages(),baseline);
        QCOMPARE(query(executionRequest).error(),QStringLiteral("ssh_disabled"));
        QCOMPARE(query(findSsh("Known","owner",2222)).error(),QStringLiteral("ssh_disabled"));
        QCOMPARE(query(forbidden).error(),QStringLiteral("agent_access_disabled"));
        QSettings reopened(preferences.fileName(),QSettings::IniFormat);
        QVERIFY(reopened.contains(prefs::McpSshSetting));
        QVERIFY(!prefs::readMcpSshEnabled(reopened,true));

        QVERIFY(setInSettings(true,true));
        QCOMPARE(query(executionRequest,accept).json().value("stdout").toString(),QStringLiteral("hostname"));
        QCOMPARE(verifications,4);
    }

    void sshExecUsesExactBindingAndKeepsSecretsLocal() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password, &error), qPrintable(error));
        model::Entry key;
        key.groupId = QUuid::createUuid();
        key.id = QUuid::createUuid();
        key.type = model::EntryType::SshKey;
        key.title = "synthetic fixture SSH key";
        key.sshPrivate = SshSyntheticKey;
        key.sshTargets = {{"Known", "node.example", "owner", 2222},
            {"Conflict", "node.example", "owner", 2222}, {"Conflict", "other.example", "owner", 2222}};
        QVERIFY(app.model()->addNode(key.groupId, {}, "synthetic machines").isEmpty());
        QVERIFY(app.model()->setEntry(key).isEmpty());
        int decisions = 0, verifications = 0;
        bool allow = true, edit = false;
        QString prompt;
        agent::Access access(app, EndpointSuffix, [&](QWidget* widget) {
            auto* dialog = qobject_cast<QMessageBox*>(widget);
            if (!dialog || dialog->textFormat() != Qt::PlainText || dialog->defaultButton() != dialog->button(QMessageBox::No))
                return false;
            ++decisions;
            prompt = dialog->text();
            if (edit) {
                key.sshTargets[0].host = "changed.example";
                error = app.model()->setEntry(key);
            }
            return allow;
        }, true, QCoreApplication::applicationFilePath());
        QVERIFY(access.start());
        const auto verify = [&](QProcess&) {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                ++verifications;
                dialog->setTextValue(QString::fromLatin1(Password));
                dialog->accept();
            }
        };
        for (auto malformed : {sshArguments(key.id, "x\ny"), sshArguments(key.id, QStringLiteral("x\u202ey")),
             sshArguments(key.id, QString(512, QChar(0x20ac))), sshArguments(key.id, "hostname", "Known", ""),
             sshArguments(key.id, "hostname", "Known", "owner", 0),
             sshArguments(key.id, "hostname", "Known", "owner", 2222, 31)}) {
            QCOMPARE(query(execRequest(malformed), verify).error(), QStringLiteral("invalid_request"));
        }
        QCOMPARE(decisions, 0);
        QCOMPARE(verifications, 0);
        allow = false;
        QCOMPARE(query(execRequest(sshArguments(key.id, "hostname", "Unmatched"))).error(), QStringLiteral("denied"));
        QCOMPARE(decisions, 1);
        QTest::qWait(780);
        allow = true;
        QCOMPARE(query(execRequest(sshArguments(key.id, "hostname", "Conflict"))).error(), QStringLiteral("ssh_binding_ambiguous"));
        QTest::qWait(780);
        QCOMPARE(query(execRequest(sshArguments(key.id, "hostname", "Known", "Owner"))).error(), QStringLiteral("ssh_binding_not_found"));
        QTest::qWait(780);
        QCOMPARE(verifications, 0);

        const QString command = QStringLiteral("echo \"%1\" < > | & \\\\");
        QProcess child;
        child.start(helperPath(), {"--mcp"});
        QVERIFY(child.waitForStarted(5000));
        const auto call = [&](const QString& text, int id) {
            sendMcp(child, "tools/call", {{"_meta", modernMcpMetadata()}, {"name", "tokenbucket_ssh_exec"},
                {"arguments", sshArguments(key.id, text, "known")}}, id);
            return receiveMcp(child, [&] { verify(child); });
        };
        const auto stagesBefore = scratchStages();
        const auto response = call(command, 1);
        QCOMPARE(response.value("id").toInt(), 1);
        QVERIFY(!response.value("result").toObject().value("isError").toBool());
        const auto payload = toolOutput(response);
        QCOMPARE(payload, (QJsonObject{{"ok", true}, {"exitCode", 0}, {"stdout", command},
            {"stderr", "[temporary SSH directory]"}}));
        QVERIFY(prompt.contains(command));
        QVERIFY(prompt.contains("node.example"));
        QVERIFY(prompt.contains(key.id.toString(QUuid::WithoutBraces)));
        QVERIFY(prompt.contains("owner"));
        QVERIFY(prompt.contains("2222"));
        QVERIFY(!prompt.contains(SshSyntheticKey));
        QCOMPARE(verifications, 1);
        QCOMPARE(scratchStages(), stagesBefore);

        const auto nonzero = call("exit 7", 2);
        QVERIFY(nonzero.value("result").toObject().value("isError").toBool());
        QCOMPARE(toolOutput(nonzero), (QJsonObject{{"ok", false}, {"error", "remote_exit"}, {"exitCode", 7},
            {"stdout", "exit 7"}, {"stderr", "[temporary SSH directory]"}}));
        const auto ambiguous = call("exit 255", 3);
        QVERIFY(ambiguous.value("result").toObject().value("isError").toBool());
        QCOMPARE(toolOutput(ambiguous).value("exitCode").toInt(), 255);
        QCOMPARE(toolOutput(ambiguous).value("error").toString(), QStringLiteral("ssh_status_unknown"));
        QCOMPARE(toolOutput(ambiguous).value("stdout").toString(), QStringLiteral("exit 255"));
        QCOMPARE(verifications, 3);

        key.sshPassphrase = SshSyntheticPhrase;
        QVERIFY(app.model()->setEntry(key).isEmpty());
        const auto encrypted = call("askpass", 4);
        QVERIFY(!encrypted.value("result").toObject().value("isError").toBool());
        QCOMPARE(toolOutput(encrypted).value("stdout").toString(), QStringLiteral("askpass"));
        const auto raw = QJsonDocument(encrypted).toJson(QJsonDocument::Compact);
        QVERIFY(!raw.contains(SshSyntheticKey));
        QVERIFY(!raw.contains(SshSyntheticPhrase));
        QCOMPARE(verifications, 4);

        edit = true;
        const auto stale = call("hostname", 5);
        QCOMPARE(toolOutput(stale).value("error").toString(), QStringLiteral("stale"));
        QVERIFY(error.isEmpty());
        QCOMPARE(verifications, 4);
        child.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 3000);
        QVERIFY(child.readAllStandardError().isEmpty());
        QCOMPARE(scratchStages(), stagesBefore);
    }

    void sshExecNativeBoundaries() {
        const auto stagesBefore = scratchStages();
        const auto program = QCoreApplication::applicationFilePath();
        const auto target = model::SshTarget{"Known", "node.example", "owner", 2222};
        qputenv("TOKENBUCKET_TEST_NO_INHERIT", "fixture");
        const auto resetEnvironment = qScopeGuard([] { qunsetenv("TOKENBUCKET_TEST_NO_INHERIT"); });
        const auto run = [&](const QString& command, const QString& phrase, const QString& executable, int timeout = 5) {
            return std::make_unique<agent::SshExecution>(target, agent::SshCommand{{}, {}, {}, command, 0, timeout},
                QByteArrayView(SshSyntheticKey), phrase, executable);
        };
        auto execution = run("askpass", SshSyntheticPhrase, program);
        QTRY_VERIFY_WITH_TIMEOUT(execution->result(), 6500);
        QVERIFY(!execution->result()->error);
        QCOMPARE(execution->result()->exitCode, 0);
        QCOMPARE(execution->result()->out, QByteArray("askpass"));
        QVERIFY(!execution->result()->err.contains(SshSyntheticPhrase));
        QCOMPARE(scratchStages(), stagesBefore);

        execution = run("wrong askpass", SshSyntheticPhrase, program);
        QTRY_VERIFY_WITH_TIMEOUT(execution->result(), 6500);
        QVERIFY(!execution->result()->error);
        QCOMPARE(execution->result()->exitCode, 0);
        QCOMPARE(scratchStages(), stagesBefore);

        execution = run("hostname", QStringLiteral("invalid\nphrase"), program);
        QTRY_VERIFY(execution->result());
        QCOMPARE(QByteArray(execution->result()->error), QByteArray("ssh_passphrase_unsupported"));
        QCOMPARE(scratchStages(), stagesBefore);
        execution = run("hostname", {}, QStringLiteral("C:/does-not-exist/ssh.exe"));
        QTRY_VERIFY(execution->result());
        QCOMPARE(QByteArray(execution->result()->error), QByteArray("ssh_launch_failed"));
        QCOMPARE(scratchStages(), stagesBefore);

        execution = run("output limit", {}, program);
        QTRY_VERIFY(execution->result());
        QCOMPARE(QByteArray(execution->result()->error), QByteArray("ssh_output_limit"));
        QVERIFY(execution->result()->out.isEmpty() && execution->result()->err.isEmpty());
        QCOMPARE(scratchStages(), stagesBefore);

        execution = run("hold", {}, program, 1);
        QTRY_VERIFY_WITH_TIMEOUT(execution->result(), 3500);
        QCOMPARE(QByteArray(execution->result()->error), QByteArray("ssh_timeout"));
        QCOMPARE(scratchStages(), stagesBefore);

        execution = run("hold tree", {}, program);
        DWORD childPid = 0;
        const auto childIsAlive = [&] {
            const auto stages = scratchStages();
            if (stages.size() != stagesBefore.size() + 1) return false;
            for (const auto& item : stages) {
                if (stagesBefore.contains(item)) continue;
                QFile marker(scratchTemp().filePath(item + "/diagnostic"));
                if (!marker.open(QIODevice::ReadOnly)) return false;
                childPid = marker.readAll().toUInt();
                return childPid != 0;
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(childIsAlive(), 2500);
        agent::Handle child(OpenProcess(SYNCHRONIZE, FALSE, childPid));
        QVERIFY(child);
        QCOMPARE(WaitForSingleObject(child.get(), 0), DWORD(WAIT_TIMEOUT));
        execution->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(execution->result(), 3000);
        QCOMPARE(QByteArray(execution->result()->error), QByteArray("cancelled"));
        QCOMPARE(WaitForSingleObject(child.get(), 2000), DWORD(WAIT_OBJECT_0));
        QCOMPARE(scratchStages(), stagesBefore);
    }

    void sshExecCancelsOnDisconnectAndChanges() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Application app(directory.path());
        QString error;
        QVERIFY2(app.createPasswordVault(Password, &error), qPrintable(error));
        model::Entry key;
        key.groupId = QUuid::createUuid();
        key.id = QUuid::createUuid();
        key.type = model::EntryType::SshKey;
        key.title = "synthetic cancellation key";
        key.sshPrivate = SshSyntheticKey;
        key.sshTargets = {{"Known", "node.example", "owner", 2222}};
        QVERIFY(app.model()->addNode(key.groupId, {}, "synthetic machines").isEmpty());
        QVERIFY(app.model()->setEntry(key).isEmpty());
        agent::Access access(app, EndpointSuffix, [](QWidget*) { return true; }, true,
            QCoreApplication::applicationFilePath());
        QVERIFY(access.start());
        QProcess child;
        QTimer verifier;
        connect(&verifier, &QTimer::timeout, &child, [] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                dialog->setTextValue(QString::fromLatin1(Password));
                dialog->accept();
            }
        });
        verifier.start(10);

        const auto start = [&](int id) {
            sendMcp(child, "tools/call", {{"_meta", modernMcpMetadata()}, {"name", "tokenbucket_ssh_exec"},
                {"arguments", sshArguments(key.id, "hold")}}, id);
        };
        const auto baseline = scratchStages();
        child.start(helperPath(), {"--mcp"});
        QVERIFY(child.waitForStarted(5000));
        start(1);
        QTRY_VERIFY_WITH_TIMEOUT(!runningStage(scratchStages(), baseline).isEmpty(), 3500);
        sendMcp(child, "notifications/cancelled", {{"requestId", 1}});
        QTRY_COMPARE_WITH_TIMEOUT(scratchStages(), baseline, 2500);
        sendMcp(child, "tools/list", {{"_meta", modernMcpMetadata()}}, 2);
        QCOMPARE(receiveMcp(child).value("id").toInt(), 2);
        QVERIFY(!child.canReadLine());

        start(3);
        QTRY_VERIFY_WITH_TIMEOUT(!runningStage(scratchStages(), baseline).isEmpty(), 3500);
        child.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 2500);
        QTRY_COMPARE_WITH_TIMEOUT(scratchStages(), baseline, 2500);
        QVERIFY(child.readAllStandardOutput().isEmpty());

        child.start(helperPath(), {"--mcp"});
        QVERIFY(child.waitForStarted(5000));
        start(4);
        QTRY_VERIFY_WITH_TIMEOUT(!runningStage(scratchStages(), baseline).isEmpty(), 3500);
        key.sshTargets[0].host = "changed.example";
        QVERIFY(app.model()->setEntry(key).isEmpty());
        QCOMPARE(toolOutput(receiveMcp(child)).value("error").toString(), QStringLiteral("stale"));
        QCOMPARE(scratchStages(), baseline);

        key.sshTargets[0].host = "node.example";
        QVERIFY(app.model()->setEntry(key).isEmpty());
        start(5);
        QTRY_VERIFY_WITH_TIMEOUT(!runningStage(scratchStages(), baseline).isEmpty(), 3500);
        app.lock();
        QCOMPARE(toolOutput(receiveMcp(child)).value("error").toString(), QStringLiteral("locked"));
        QCOMPARE(scratchStages(), baseline);
        child.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 2500);
        QVERIFY(child.readAllStandardError().isEmpty());
    }

    void sshExecRecoversOnlyOwnAbandonedStage() {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--ssh-stage-abandon"});
        QVERIFY(child.waitForStarted(5000));
        const auto childPid = child.processId();
        QTRY_VERIFY_WITH_TIMEOUT(!runningStage(scratchStages(childPid)).isEmpty(), 3000);
        const auto stage = runningStage(scratchStages(childPid));
        agent::cleanAbandonedSshStages();
        QVERIFY(scratchStages(childPid).contains(stage));
        child.kill();
        QVERIFY(child.waitForFinished(3000));

        const auto lookalike = QStringLiteral("TokenBucket-ssh-%1-1-%2").arg(GetCurrentProcessId())
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto temp = scratchTemp();
        QVERIFY(temp.mkdir(lookalike));
        QFile unowned(temp.filePath(lookalike + "/identity"));
        const auto cleanup = qScopeGuard([&] { unowned.remove(); temp.rmdir(lookalike); });
        QVERIFY(unowned.open(QIODevice::WriteOnly));
        unowned.write("synthetic unrelated file");
        unowned.close();
        agent::cleanAbandonedSshStages();
        QVERIFY(!scratchStages(childPid).contains(stage));
        QVERIFY(unowned.exists());
    }

    void sshExecRealOpenSsh() {
        const auto keyPath = qEnvironmentVariable("TOKENBUCKET_TEST_SSH_KEY");
        if (keyPath.isEmpty()) QSKIP("Loopback SSH fixture was not supplied.");
        QFile key(keyPath);
        QVERIFY(key.open(QIODevice::ReadOnly));
        SecureBytes keyBytes(key.readAll());
        const auto port = qEnvironmentVariableIntValue("TOKENBUCKET_TEST_SSH_PORT");
        QVERIFY(port > 0 && port < 65536);
        const auto goodPin = qEnvironmentVariable("TOKENBUCKET_TEST_SSH_KNOWN_HOSTS");
        const auto wrongPin = qEnvironmentVariable("TOKENBUCKET_TEST_SSH_WRONG_HOSTS");
        QVERIFY(!goodPin.isEmpty() && !wrongPin.isEmpty());
        const auto stageBaseline = scratchStages();
        const auto target = model::SshTarget{{}, "127.0.0.1", "synthetic-owner", port};
        const auto command = agent::SshCommand{{}, {}, {}, "hostname", 0, 10};
        for (const auto& [phrase, hosts, error] : std::array{
                std::tuple<QString, QString, const char*>{SshSyntheticPhrase, goodPin, nullptr},
                std::tuple<QString, QString, const char*>{"synthetic-wrong-phrase", goodPin, "ssh_auth_failed"},
                std::tuple<QString, QString, const char*>{{}, goodPin, "ssh_auth_failed"},
                std::tuple<QString, QString, const char*>{SshSyntheticPhrase, wrongPin, "ssh_host_key_failed"}}) {
            agent::SshExecution execution(target, command, keyBytes.bytes(), phrase, {}, hosts);
            QTRY_VERIFY_WITH_TIMEOUT(execution.result(), 12500);
            const auto actual = QByteArray(execution.result()->error);
            const auto detail = QByteArray("expected: ") + QByteArray(error) + ", actual: " + actual +
                ", stderr: " + execution.result()->err;
            QVERIFY2(actual == QByteArray(error), detail.constData());
            if (!error) {
                QCOMPARE(execution.result()->exitCode, 0);
                QCOMPARE(execution.result()->out.trimmed(), QByteArray("TOKENBUCKET-LOOPBACK"));
            } else QVERIFY(execution.result()->out.isEmpty() && execution.result()->err.isEmpty());
            QCOMPARE(scratchStages(), stageBaseline);
        }
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
        ssh.sshTargets = {{"Deployment", "node.example", "deploy", 2222}, {"Backup", "other.example", "ops", 22}};
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
        QCOMPARE(tools.size(), 4);
        QCOMPARE(tools.at(0).toObject().value("name").toString(), QStringLiteral("tokenbucket_find"));
        QCOMPARE(tools.at(1).toObject().value("name").toString(), QStringLiteral("tokenbucket_read"));
        QCOMPARE(tools.at(2).toObject().value("name").toString(), QStringLiteral("tokenbucket_find_ssh"));
        const auto exec = tools.at(3).toObject();
        QCOMPARE(exec.value("name").toString(), QStringLiteral("tokenbucket_ssh_exec"));
        QCOMPARE(exec.value("annotations").toObject(), (QJsonObject{
            {"readOnlyHint", false}, {"openWorldHint", true}, {"idempotentHint", false}}));
        sendMcp(child, "tools/call", {{"name", "tokenbucket_ssh_exec"},
            {"arguments", sshArguments(ssh.id, "hostname", "Deployment", "deploy", 2222)}}, 10);
        const auto disabled = receiveMcp(child);
        QVERIFY(disabled.value("result").toObject().value("isError").toBool());
        QCOMPARE(toolOutput(disabled).value("error").toString(), QStringLiteral("ssh_disabled"));
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
        sendMcp(child, "tools/call", {{"name", "tokenbucket_find_ssh"},
            {"arguments", QJsonObject{{"machine", "deployment"}, {"user", "deploy"}, {"port", 2222}}}}, 8);
        const auto candidates = toolOutput(receiveMcp(child, approve)).value("entries").toArray();
        QCOMPARE(candidates.size(), 1);
        const auto selected = candidates.at(0).toObject();
        QCOMPARE(selected.value("id"), found.value("id"));
        QCOMPARE(selected.value("targets").toArray().size(), 1);
        const QJsonObject readArgs{{"name", "tokenbucket_read"},
            {"arguments", QJsonObject{{"id", selected.value("id")}, {"field", "sshPrivate"}}}};
        sendMcp(child, "tools/call", readArgs, 6);
        const auto result = receiveMcp(child, approve);
        QCOMPARE(result.value("id").toInt(), 6);
        QCOMPARE(approvals, 3);
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
        QCOMPARE(catalog.value("tools").toArray().size(), 4);
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

int main(int argc, char** argv) {
    platform::crash::disableDumps();
    if (argc > 1 && std::strcmp(argv[1], "-F") == 0) {
        QCoreApplication app(argc, argv);
        return fakeSsh(app.arguments());
    }
    if (argc == 2 && std::strcmp(argv[1], "--ssh-fixture-child") == 0) {
        Sleep(20000);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--ssh-stage-abandon") == 0) {
        QCoreApplication app(argc, argv);
        const model::SshTarget target{{}, "node.example", "owner", 2222};
        const agent::SshCommand command{{}, {}, {}, "hold", 0, 10};
        agent::SshExecution execution(target, command, QByteArrayView(SshSyntheticKey), {}, app.applicationFilePath());
        Sleep(15000);
        return 0;
    }
    if (agent::isSshAskpass()) return agent::runSshAskpass();
    QApplication app(argc, argv);
    TestAgent test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_agent_win.moc"
