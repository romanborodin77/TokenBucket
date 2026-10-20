#include "EnvOps.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <windows.h>
#include <vector>
namespace tb::platform::envops {
namespace {
constexpr wchar_t kEnvironmentKey[] = L"Environment"; // только HKCU: системные (HKLM) не трогаем

// Оповещение системы: без broadcast новые процессы (и проводник) продолжали бы видеть
// старую копию окружения до перезапуска оболочки/входа в систему.
void broadcastEnvironmentChange() {
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
        SMTO_ABORTIFHUNG, 5000, nullptr);
}
void storeExported(const QStringList& names) {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    settings.setValue(QStringLiteral("env-exported"), names); // нативный QStringList — без своего формата
}
}

bool validName(const QString& name) {
    if (name.isEmpty() || name.size() > 64) return false;
    for (int i = 0; i < name.size(); ++i) {
        const ushort u = name.at(i).unicode();
        const bool letter = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
        const bool digit = u >= '0' && u <= '9';
        if (i == 0 ? (!letter && u != '_') : (!letter && !digit && u != '_')) return false;
    }
    return true;
}

bool startWithVariable(const QString& program, const QString& arguments, const QString& name,
                       const QString& value, QString* err) {
    if (err) err->clear();
    if (!validName(name)) { // защита и здесь, как в setUserVariable: пустое имя дало бы запись "=value"
        if (err) *err = QStringLiteral("Недопустимое имя переменной окружения.");
        return false;
    }
    if (program.isEmpty()) {
        if (err) *err = QStringLiteral("Не указана программа для запуска.");
        return false;
    }
    // Окружение ребёнка = строки текущего процесса (одноимённая переменная заменяется, а не
    // дублируется — имена в Windows сравниваются без учёта регистра) + "NAME=value".
    std::vector<wchar_t> block;
    LPWCH original = GetEnvironmentStringsW();
    if (original) {
        for (const wchar_t* p = original; *p; ) {
            const size_t len = wcslen(p);
            const wchar_t* eq = wcschr(p, L'=');
            const bool sameName = eq && QString::fromWCharArray(p, int(eq - p)).compare(name, Qt::CaseInsensitive) == 0;
            if (!sameName) block.insert(block.end(), p, p + len + 1); // строка вместе с её '\0'
            p += len + 1;
        }
        FreeEnvironmentStringsW(original);
    }
    const std::wstring entry = (name + QLatin1Char('=') + value).toStdWString();
    block.insert(block.end(), entry.c_str(), entry.c_str() + entry.size() + 1);
    block.push_back(L'\0'); // завершающий нуль всего блока
    // Квотируем только незаквотированный путь с пробелом/табуляцией: путь, уже введённый в
    // кавычках, повторное квотирование ломает; без пробелов квот не нужен. Параметры —
    // строкой «как есть»: их квотирование остаётся за пользователем.
    const bool needsQuotes = !program.startsWith(QLatin1Char('"'))
        && (program.contains(QLatin1Char(' ')) || program.contains(QLatin1Char('\t')));
    QString commandLine = needsQuotes ? QStringLiteral("\"%1\"").arg(program) : program;
    if (!arguments.trimmed().isEmpty()) commandLine += QLatin1Char(' ') + arguments;
    const std::wstring wideProgram = program.toStdWString();
    std::wstring wideCommandLine = commandLine.toStdWString(); // неконстантный буфер: CreateProcessW может писать в него
    const std::wstring workingDir = QFileInfo(program).absolutePath().toStdWString(); // каталог программы
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CREATE_UNICODE_ENVIRONMENT: lpEnvironment — блок wchar_t, а не ANSI-строк; без флага
    // CreateProcessW интерпретировал бы блок побайтово и переменная до ребёнка не дошла бы.
    if (!CreateProcessW(wideProgram.c_str(), wideCommandLine.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT, block.data(), workingDir.c_str(), &si, &pi)) {
        if (err) *err = QStringLiteral("Не удалось запустить приложение (код %1).").arg(GetLastError());
        return false;
    }
    // Процесс независимый: не ждём завершения, дескрипторы закрываем сразу — иначе утекут.
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

bool setUserVariable(const QString& name, const QString& value, QString* err) {
    if (err) err->clear();
    if (!validName(name)) {
        if (err) *err = QStringLiteral("Недопустимое имя переменной окружения.");
        return false;
    }
    const std::wstring wideName = name.toStdWString(), wideValue = value.toStdWString();
    // REG_SZ, а не EXPAND_SZ: секрет с символами '%' не должен «расширяться» при чтении из реестра.
    const LSTATUS r = RegSetKeyValueW(HKEY_CURRENT_USER, kEnvironmentKey, wideName.c_str(), REG_SZ,
        wideValue.c_str(), DWORD((wideValue.size() + 1) * sizeof(wchar_t)));
    if (r != ERROR_SUCCESS) {
        if (err) *err = QStringLiteral("Не удалось записать переменную в реестр (код %1).").arg(int(r));
        return false;
    }
    broadcastEnvironmentChange();
    return true;
}

bool deleteKnownUserVariable(const QString& name, QString* err) {
    if (err) err->clear();
    const std::wstring wideName = name.toStdWString();
    const LSTATUS r = RegDeleteKeyValueW(HKEY_CURRENT_USER, kEnvironmentKey, wideName.c_str());
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) { // отсутствующая уже «удалена» — идемпотентно
        if (err) *err = QStringLiteral("Не удалось удалить переменную из реестра (код %1).").arg(int(r));
        return false;
    }
    broadcastEnvironmentChange();
    return true;
}

QStringList exportedNames() {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    return settings.value(QStringLiteral("env-exported")).toStringList();
}
void rememberExported(const QString& name) {
    QStringList names = exportedNames();
    if (!names.contains(name)) { names << name; storeExported(names); }
}
void forgetExported(const QString& name) {
    QStringList names = exportedNames();
    if (names.removeAll(name)) storeExported(names);
}
bool clearOnExitEnabled() {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    return settings.value(QStringLiteral("env-clear-on-exit"), false).toBool();
}
void setClearOnExitEnabled(bool enabled) {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    settings.setValue(QStringLiteral("env-clear-on-exit"), enabled);
}
int clearAllExported() {
    int removed = 0;
    for (const QString& name : exportedNames()) {
        QString error;
        // Идемпотентно: отсутствующая переменная считается стёртой; при реальном отказе
        // имя остаётся в списке, чтобы переменную можно было удалить вручную позже.
        if (deleteKnownUserVariable(name, &error)) { forgetExported(name); ++removed; }
    }
    return removed;
}
}

namespace tb::platform::autostart {
namespace {
constexpr wchar_t kRunKey[] = L"Software\Microsoft\Windows\CurrentVersion\Run";
constexpr wchar_t kValueName[] = L"TokenBucket";
}
QString commandForTest() {
    // Кавычки обязательны: путь к программе обычно содержит пробелы; --tray просит
    // стартовать свёрнутым в трей, не показывая окно при входе в систему.
    return QStringLiteral("\"%1\" --tray").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
}
bool enabled() {
    wchar_t buffer[1024] = {};
    DWORD size = sizeof(buffer);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS;
}
bool setEnabled(bool on, QString* err) {
    if (err) err->clear();
    if (on) {
        const std::wstring command = commandForTest().toStdWString();
        const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ,
            command.c_str(), DWORD((command.size() + 1) * sizeof(wchar_t)));
        if (status != ERROR_SUCCESS) {
            if (err) *err = QStringLiteral("Не удалось включить автозапуск (код %1).").arg(status);
            return false;
        }
        return true;
    }
    const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        if (err) *err = QStringLiteral("Не удалось выключить автозапуск (код %1).").arg(status);
        return false;
    }
    return true;
}
}
