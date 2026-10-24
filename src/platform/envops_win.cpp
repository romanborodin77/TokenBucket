#include "EnvOps.h"
#include "../app/Preferences.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QScopeGuard>
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
bool storeExported(const QStringList& names,QSettings* preferences,QString* error) {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    return prefs::writeValue(preferences?*preferences:settings,QStringLiteral("env-exported"),names,error);
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

QStringList exportedNames(QSettings* preferences) {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    return (preferences?*preferences:settings).value(QStringLiteral("env-exported")).toStringList();
}
bool rememberExported(const QString& name,QSettings* preferences,QString* error) {
    QStringList names = exportedNames(preferences);
    if (!names.contains(name)) { names << name; return storeExported(names,preferences,error); }
    if(error) error->clear(); return true;
}
bool forgetExported(const QString& name,QSettings* preferences,QString* error) {
    QStringList names = exportedNames(preferences);
    if (names.removeAll(name)) return storeExported(names,preferences,error);
    if(error) error->clear(); return true;
}
bool clearOnExitEnabled(QSettings* preferences) {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    return prefs::read(preferences?*preferences:settings).clearEnvironmentOnExit;
}
bool setClearOnExitEnabled(bool enabled,QSettings* preferences,QString* error) {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    return prefs::writeValue(preferences?*preferences:settings,QStringLiteral("env-clear-on-exit"),enabled,error);
}
int clearAllExported(QSettings* preferences) {
    int removed = 0;
    for (const QString& name : exportedNames(preferences)) {
        QString error;
        // Идемпотентно: отсутствующая переменная считается стёртой; при реальном отказе
        // имя остаётся в списке, чтобы переменную можно было удалить вручную позже.
        if (deleteKnownUserVariable(name, &error) && forgetExported(name,preferences,&error)) ++removed;
    }
    return removed;
}
}

namespace tb::platform::autostart {
QString commandForTest() {
    // Кавычки обязательны: путь к программе обычно содержит пробелы; --tray просит
    // стартовать свёрнутым в трей, не показывая окно при входе в систему.
    return QStringLiteral("\"%1\" --tray").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
}
std::optional<bool> enabled(QSettings& run,QString* error) {
    if(error) error->clear();
    const auto unknown=[&]() -> std::optional<bool> {
        if(error) *error=QStringLiteral("Не удалось прочитать состояние автозапуска.");
        return std::nullopt;
    };
    if(!run.group().isEmpty()) return unknown();
    if(run.format()==QSettings::NativeFormat || run.format()==QSettings::Registry32Format || run.format()==QSettings::Registry64Format) {
        // Qt NativeFormat не отличает отказ чтения реестра от отсутствующего значения.
        const auto prefix=QStringLiteral("\\HKEY_CURRENT_USER\\"),path=run.fileName();
        if(!path.startsWith(prefix,Qt::CaseInsensitive)) return unknown();
        const auto subkey=path.mid(prefix.size()); HKEY key=nullptr;
        const REGSAM view=run.format()==QSettings::Registry32Format?KEY_WOW64_32KEY
            :run.format()==QSettings::Registry64Format?KEY_WOW64_64KEY:0;
        const auto opened=RegOpenKeyExW(HKEY_CURRENT_USER,reinterpret_cast<LPCWSTR>(subkey.utf16()),0,KEY_QUERY_VALUE|view,&key);
        if(opened==ERROR_FILE_NOT_FOUND || opened==ERROR_PATH_NOT_FOUND) return false;
        if(opened!=ERROR_SUCCESS) return unknown();
        const auto close=qScopeGuard([&] {RegCloseKey(key);}); DWORD size=0;
        const auto queried=RegQueryValueExW(key,L"TokenBucket",nullptr,nullptr,nullptr,&size);
        if(queried==ERROR_SUCCESS || queried==ERROR_MORE_DATA) return true;
        if(queried==ERROR_FILE_NOT_FOUND) return false;
        return unknown();
    }
    QSettings reader(run.fileName(),run.format()); reader.setFallbacksEnabled(run.fallbacksEnabled()); reader.sync();
    if(reader.status()!=QSettings::NoError) return unknown();
    return reader.contains("TokenBucket");
}
bool setEnabled(bool on,QSettings& run,QString* error) {
    if(!prefs::writeValue(run,"TokenBucket",on?QVariant(commandForTest()):QVariant(),error)) return false;
    const auto current=enabled(run,error);
    if(!current || *current!=on || (on && run.value("TokenBucket").toString()!=commandForTest())) {
        if(error) *error=QStringLiteral("Не удалось подтвердить состояние автозапуска.");
        return false;
    }
    return true;
}
bool enabled() {
    QSettings run(QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)"),QSettings::NativeFormat);
    return enabled(run).value_or(false);
}
bool setEnabled(bool on,QString* error) {
    QSettings run(QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)"),QSettings::NativeFormat);
    return setEnabled(on,run,error);
}
}
