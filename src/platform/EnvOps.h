#pragma once
#include <QString>
#include <QStringList>
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
QStringList exportedNames();
void rememberExported(const QString& name);
void forgetExported(const QString& name);
}
