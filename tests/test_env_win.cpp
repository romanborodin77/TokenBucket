#include <QtTest>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <windows.h>
#include "../src/platform/EnvOps.h"
#include "../src/ui/EnvExportDialog.h"
using namespace tb;

class TestEnv : public QObject {
    Q_OBJECT
private slots:
    void validName() {
        QVERIFY(platform::envops::validName(QStringLiteral("TOKEN")));
        QVERIFY(platform::envops::validName(QStringLiteral("_hidden")));
        QVERIFY(platform::envops::validName(QStringLiteral("a")));
        QVERIFY(platform::envops::validName(QStringLiteral("A_b_9")));
        QVERIFY(platform::envops::validName(QStringLiteral("a").repeated(64)));
        QVERIFY(!platform::envops::validName(QString()));
        QVERIFY(!platform::envops::validName(QStringLiteral("1abc")));
        QVERIFY(!platform::envops::validName(QStringLiteral("a b")));
        QVERIFY(!platform::envops::validName(QStringLiteral("a-b")));
        QVERIFY(!platform::envops::validName(QStringLiteral("a=b")));
        QVERIFY(!platform::envops::validName(QStringLiteral("a.b")));
        QVERIFY(!platform::envops::validName(QStringLiteral("токен")));
        QVERIFY(!platform::envops::validName(QStringLiteral("a").repeated(65)));
        QVERIFY(!platform::envops::validName(QStringLiteral("with\nnewline")));
    }
    void startWithVariable() {
        QTemporaryDir dir;
        const QString out = QDir(dir.path()).filePath(QStringLiteral("env.txt"));
        const QString cmd = qEnvironmentVariable("ComSpec");
        QVERIFY2(!cmd.isEmpty(), "ComSpec не задан");
        QString err;
        // cmd /c echo(%TB_TEST_VAR% > файл: переменная попадает в среду ребёнка и разворачивается echo.
        QVERIFY2(platform::envops::startWithVariable(cmd,
                QStringLiteral("/c echo(%TB_TEST_VAR% > \"%1\"").arg(QDir::toNativeSeparators(out)),
                QStringLiteral("TB_TEST_VAR"), QStringLiteral("ok123"), &err),
            qPrintable(err));
        QString content;
        bool appeared = false;
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 10000; // процесс независимый — ждём файл поллингом
        while (QDateTime::currentMSecsSinceEpoch() < deadline) {
            QFile file(out);
            if (file.open(QIODevice::ReadOnly)) {
                content = QString::fromUtf8(file.readAll());
                if (content.contains(QLatin1String("ok123"))) { appeared = true; break; }
            }
            QTest::qWait(50);
        }
        QVERIFY2(appeared, qPrintable(QStringLiteral("вывод cmd не содержит ok123: «%1»").arg(content)));
        QVERIFY(QFile::remove(out)); // чистим за собой
    }
    void userVariableCycle() {
        const QString name = QStringLiteral("TOKENBUCKET_TEST_ENV");
        platform::envops::forgetExported(name); // хвосты прошлых прогонов недопустимы
        QString err;
        QVERIFY2(platform::envops::setUserVariable(name, QStringLiteral("значение-теста"), &err), qPrintable(err));
        const std::wstring wide = name.toStdWString();
        wchar_t buf[64] = {};
        DWORD size = sizeof(buf);
        QCOMPARE(RegGetValueW(HKEY_CURRENT_USER, L"Environment", wide.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size), LSTATUS(ERROR_SUCCESS));
        QCOMPARE(QString::fromWCharArray(buf), QStringLiteral("значение-теста"));
        platform::envops::rememberExported(name);
        QVERIFY(platform::envops::exportedNames().contains(name));
        QVERIFY2(platform::envops::deleteKnownUserVariable(name, &err), qPrintable(err));
        size = sizeof(buf);
        QCOMPARE(RegGetValueW(HKEY_CURRENT_USER, L"Environment", wide.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size), LSTATUS(ERROR_FILE_NOT_FOUND));
        platform::envops::forgetExported(name);
        QVERIFY(!platform::envops::exportedNames().contains(name));
    }
    void dialogValidation() {
        ui::EnvExportDialog dialog;
        auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("envVarName"));
        auto* program = dialog.findChild<QLineEdit*>(QStringLiteral("envProgram"));
        auto* permanent = dialog.findChild<QRadioButton*>(QStringLiteral("envModePermanent"));
        auto* ok = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        QVERIFY(name && program && permanent && ok);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        ok->click(); // пустое имя — не принимается
        QVERIFY(dialog.result() != QDialog::Accepted);
        QVERIFY(dialog.isVisible());
        name->setText(QStringLiteral("1abc"));
        ok->click(); // некорректное имя — не принимается
        QVERIFY(dialog.result() != QDialog::Accepted);
        QVERIFY(dialog.isVisible());
        name->setText(QStringLiteral("TB_TEST_ENV"));
        ok->click(); // временный режим (по умолчанию) без программы — не принимается
        QVERIFY(dialog.result() != QDialog::Accepted);
        QVERIFY(dialog.isVisible());
        permanent->setChecked(true); // постоянный режим программу не требует
        ok->click();
        QCOMPARE(int(dialog.result()), int(QDialog::Accepted));
        QCOMPARE(dialog.mode(), ui::EnvExportDialog::Mode::Permanent);
        QCOMPARE(dialog.name(), QStringLiteral("TB_TEST_ENV"));
        ui::EnvExportDialog second;
        second.findChild<QLineEdit*>(QStringLiteral("envVarName"))->setText(QStringLiteral("TB_SECOND"));
        second.findChild<QLineEdit*>(QStringLiteral("envProgram"))->setText(QStringLiteral("C:/tools/app.exe"));
        second.findChild<QLineEdit*>(QStringLiteral("envArguments"))->setText(QStringLiteral("--flag \"a b\""));
        second.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(int(second.result()), int(QDialog::Accepted));
        QCOMPARE(second.mode(), ui::EnvExportDialog::Mode::Temporary);
        QCOMPARE(second.program(), QStringLiteral("C:/tools/app.exe"));
        QCOMPARE(second.arguments(), QStringLiteral("--flag \"a b\""));
        QCOMPARE(second.name(), QStringLiteral("TB_SECOND"));
    }
};
QTEST_MAIN(TestEnv)
#include "test_env_win.moc"
