#include <QtTest>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QSettings>
#include <windows.h>
#include <sddl.h>
#include <QScopeGuard>
#include "../src/platform/EnvOps.h"
#include "../src/ui/EnvExportDialog.h"
using namespace tb;

class TestEnv : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir_;
    QSettings preferences_{settingsDir_.filePath("prefs.ini"),QSettings::IniFormat};
    QSettings run_{settingsDir_.filePath("run.ini"),QSettings::IniFormat};
private slots:
    void inaccessibleNativeAutostartIsUnknown() {
        const auto path=QStringLiteral("Software\\TokenBucketTests\\%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        HKEY key=nullptr;
        QCOMPARE(RegCreateKeyExW(HKEY_CURRENT_USER,reinterpret_cast<LPCWSTR>(path.utf16()),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr),LONG(ERROR_SUCCESS));
        QByteArray original; DWORD size=0;
        RegGetKeySecurity(key,DACL_SECURITY_INFORMATION,nullptr,&size); original.resize(size);
        QCOMPARE(RegGetKeySecurity(key,DACL_SECURITY_INFORMATION,reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data()),&size),LONG(ERROR_SUCCESS));
        const auto cleanup=qScopeGuard([&] {
            RegSetKeySecurity(key,DACL_SECURITY_INFORMATION,reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data()));
            RegCloseKey(key); RegDeleteKeyW(HKEY_CURRENT_USER,reinterpret_cast<LPCWSTR>(path.utf16()));
        });
        QSettings run("HKEY_CURRENT_USER\\"+path,QSettings::NativeFormat); QString error;
        QCOMPARE(platform::autostart::enabled(run,&error),std::optional<bool>(false));
        const wchar_t value[]=L"synthetic-command";
        QCOMPARE(RegSetValueExW(key,L"TokenBucket",0,REG_SZ,reinterpret_cast<const BYTE*>(value),sizeof(value)),LONG(ERROR_SUCCESS));
        QCOMPARE(platform::autostart::enabled(run,&error),std::optional<bool>(true));
        PSECURITY_DESCRIPTOR denied=nullptr;
        QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x1;;;WD)(A;;KA;;;WD)",SDDL_REVISION_1,&denied,nullptr));
        const auto freeDescriptor=qScopeGuard([&] {LocalFree(denied);});
        QCOMPARE(RegSetKeySecurity(key,DACL_SECURITY_INFORMATION,denied),LONG(ERROR_SUCCESS));
        HKEY probe=nullptr;
        QCOMPARE(RegOpenKeyExW(HKEY_CURRENT_USER,reinterpret_cast<LPCWSTR>(path.utf16()),0,KEY_QUERY_VALUE,&probe),LONG(ERROR_ACCESS_DENIED));
        QVERIFY(!platform::autostart::enabled(run,&error).has_value()); QVERIFY(!error.isEmpty());
    }
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
        const QString name = QStringLiteral("TOKENBUCKET_TEST_ENV_%1").arg(QCoreApplication::applicationPid());
        platform::envops::forgetExported(name,&preferences_); // хвосты прошлых прогонов недопустимы
        QString err;
        QVERIFY2(platform::envops::setUserVariable(name, QStringLiteral("значение-теста"), &err), qPrintable(err));
        const std::wstring wide = name.toStdWString();
        wchar_t buf[64] = {};
        DWORD size = sizeof(buf);
        QCOMPARE(RegGetValueW(HKEY_CURRENT_USER, L"Environment", wide.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size), LSTATUS(ERROR_SUCCESS));
        QCOMPARE(QString::fromWCharArray(buf), QStringLiteral("значение-теста"));
        platform::envops::rememberExported(name,&preferences_);
        QVERIFY(platform::envops::exportedNames(&preferences_).contains(name));
        QVERIFY2(platform::envops::deleteKnownUserVariable(name, &err), qPrintable(err));
        size = sizeof(buf);
        QCOMPARE(RegGetValueW(HKEY_CURRENT_USER, L"Environment", wide.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size), LSTATUS(ERROR_FILE_NOT_FOUND));
        platform::envops::forgetExported(name,&preferences_);
        QVERIFY(!platform::envops::exportedNames(&preferences_).contains(name));
    }
    void clearAllExportedAndOptionRoundtrip() {
        const QString first = QStringLiteral("TOKENBUCKET_TEST_ENV_%1").arg(QCoreApplication::applicationPid()), second = QStringLiteral("TOKENBUCKET_TEST_ENV_2_%1").arg(QCoreApplication::applicationPid());
        platform::envops::forgetExported(first,&preferences_); platform::envops::forgetExported(second,&preferences_);
        QString err;
        QVERIFY2(platform::envops::setUserVariable(first, QStringLiteral("один"), &err), qPrintable(err));
        QVERIFY2(platform::envops::setUserVariable(second, QStringLiteral("два"), &err), qPrintable(err));
        platform::envops::rememberExported(first,&preferences_); platform::envops::rememberExported(second,&preferences_);
        // опция: выключена по умолчанию в наших тестах не гарантируется — проверяем раундтрип
        platform::envops::setClearOnExitEnabled(true,&preferences_);
        QVERIFY(platform::envops::clearOnExitEnabled(&preferences_));
        platform::envops::setClearOnExitEnabled(false,&preferences_);
        QVERIFY(!platform::envops::clearOnExitEnabled(&preferences_));
        QCOMPARE(platform::envops::clearAllExported(&preferences_), 2);
        const std::wstring w1 = first.toStdWString(), w2 = second.toStdWString();
        wchar_t buf[64] = {}; DWORD size = sizeof(buf);
        QCOMPARE(RegGetValueW(HKEY_CURRENT_USER, L"Environment", w1.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size), LSTATUS(ERROR_FILE_NOT_FOUND));
        size = sizeof(buf);
        QCOMPARE(RegGetValueW(HKEY_CURRENT_USER, L"Environment", w2.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size), LSTATUS(ERROR_FILE_NOT_FOUND));
        QVERIFY(platform::envops::exportedNames(&preferences_).isEmpty());
        // повторный вызов идемпотентен (переменных и имён нет)
        QCOMPARE(platform::envops::clearAllExported(&preferences_), 0);
    }
    void autostartRoundtrip() {
        const bool initial = platform::autostart::enabled(run_).value(); // восстановим состояние машины
        QString err;
        QVERIFY2(platform::autostart::setEnabled(true, run_, &err), qPrintable(err));
        QVERIFY(platform::autostart::enabled(run_).value());
        // Команда всегда указывает на текущий файл программы и просит старт в трее.
        const QString command = platform::autostart::commandForTest();
        QVERIFY(command.startsWith(QLatin1Char('"')));
        QVERIFY(command.endsWith(QStringLiteral("--tray")));
        QVERIFY(command.contains(QStringLiteral("test_env_win"), Qt::CaseInsensitive));
        QVERIFY2(platform::autostart::setEnabled(false, run_, &err), qPrintable(err));
        QVERIFY(!platform::autostart::enabled(run_).value());
        QVERIFY(platform::autostart::setEnabled(false, run_, &err)); // идемпотентно
        if (initial) QVERIFY(platform::autostart::setEnabled(true, run_, &err));
    }
    void dialogValidation() {
        ui::EnvExportDialog dialog(nullptr,&preferences_);
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
        ui::EnvExportDialog second(nullptr,&preferences_);
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
