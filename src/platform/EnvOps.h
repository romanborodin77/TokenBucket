#pragma once
#include <QString>
#include <QStringList>
#include <optional>
class QSettings;
namespace tb::platform::envops {
// Имя переменной окружения: [A-Za-z_][A-Za-z0-9_]*, до 64 символов (ASCII — как принято в Windows).
bool validName(const QString& name);
// Запуск независимого процесса с окружением = копия текущего + name=value (секрет живёт
// только в среде запущенного процесса и его потомков). Процесс не ждём. false + err при отказе.
bool startWithVariable(const QString& program, const QString& arguments, const QString& name,
                       const QString& value, QString* err);
// Пользовательская переменная Windows (HKCU\Environment) + оповещение системы об изменении.
bool setUserVariable(const QString& name, const QString& value, QString* err);
// Удаление ранее установленной нами переменной (список имён — exportedNames). Отсутствующая
// переменная считается удалённой (идемпотентность).
bool deleteKnownUserVariable(const QString& name, QString* err);
// Учёт установленных нами ИМЁН (без значений) в QSettings; значения секретов не храним.
QStringList exportedNames(QSettings* preferences=nullptr);
bool rememberExported(const QString& name,QSettings* preferences=nullptr,QString* error=nullptr);
bool forgetExported(const QString& name,QSettings* preferences=nullptr,QString* error=nullptr);
// Опция «стирать установленные нами переменные при выходе из программы» (QSettings
// env-clear-on-exit, по умолчанию выключена) и само стирание всех учтённых переменных.
// Возвращает число стёртых; имена стёртых (и уже отсутствующих) забываются, при отказе
// удаления имя остаётся в списке — переменную можно будет удалить вручную.
bool clearOnExitEnabled(QSettings* preferences=nullptr);
bool setClearOnExitEnabled(bool enabled,QSettings* preferences=nullptr,QString* error=nullptr);
int clearAllExported(QSettings* preferences=nullptr);
}
namespace tb::platform::autostart {
// Автозапуск при входе в Windows: запись HKCU\...\Run («TokenBucket» → "<exe>" --tray).
// Ключ пользовательский, прав администратора не требует; значение всегда перезаписываем
// актуальным путём, чтобы запись не «протухала» после переноса программы.
bool enabled();
bool setEnabled(bool on, QString* err);
std::optional<bool> enabled(QSettings& run,QString* error=nullptr);
bool setEnabled(bool on,QSettings& run,QString* error=nullptr);
QString commandForTest(); // путь с аргументом, который записывается в реестр
}
