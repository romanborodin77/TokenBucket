#include "Preferences.h"
#include <algorithm>
namespace tb::prefs {
namespace {
int integer(QSettings& s,const QString& key,int fallback) {
    bool ok=false; const int value=s.value(key,fallback).toString().toInt(&ok); return ok?value:fallback;
}
bool boolean(QSettings& s,const QString& key,bool fallback) {
    const auto value=s.value(key,fallback).toString().toLower();
    if(value=="true" || value=="1") return true;
    if(value=="false" || value=="0") return false;
    return fallback;
}
}
Values read(QSettings& s) {
    Values v;
    const auto theme=s.value("theme","system").toString();
    if(theme=="light" || theme=="dark") v.theme=theme;
    v.notifications=boolean(s,"trayNotifications",true);
    v.clearEnvironmentOnExit=boolean(s,"env-clear-on-exit",false);
    v.idleMinutes=qBound(1,integer(s,"idleTimeoutMinutes",5),60);
    const int reveal=integer(s,"secretRevealSeconds",15), clipboard=integer(s,"clipboardClearSeconds",20);
    if(reveal==5 || reveal==10 || reveal==15 || reveal==30) v.revealSeconds=reveal;
    if(clipboard==10 || clipboard==20 || clipboard==30 || clipboard==60) v.clipboardSeconds=clipboard;
    if(s.value("quickPasteDefaultField").toString()=="login") v.defaultField=paste::Field::Login;
    const auto closing=s.value("closeBehavior").toString();
    if(closing=="lock-hide") v.closeBehavior=CloseBehavior::LockHide;
    if(closing=="exit") v.closeBehavior=CloseBehavior::Exit;
    if(s.contains("hotkey")) v.hotkey=QKeySequence::fromString(s.value("hotkey").toString(),QKeySequence::PortableText);
    // Генератор: неверная длина или набор групп заменяются умолчанием целиком — частично
    // применённые параметры могли бы дать неожиданно слабый пароль.
    const int length=integer(s,"generator/length",24); const auto groups=s.value("generator/groups","ULDS").toString();
    const bool groupsValid=!groups.isEmpty() && groups.size()<=4 && std::all_of(groups.cbegin(),groups.cend(),[&](QChar c) {return QStringLiteral("ULDS").contains(c) && groups.count(c)==1;});
    if(length>=8 && length<=128 && groupsValid)
        v.generator={length,groups.contains('U'),groups.contains('L'),groups.contains('D'),groups.contains('S'),boolean(s,"generator/excludeSimilar",true)};
    else v.generator.excludeSimilar=boolean(s,"generator/excludeSimilar",true);
    return v;
}
QString validate(const Values& v) {
    if(v.theme!="system" && v.theme!="light" && v.theme!="dark") return QStringLiteral("Недопустимая тема.");
    if(v.idleMinutes<1 || v.idleMinutes>60) return QStringLiteral("Автоблокировка: от 1 до 60 минут.");
    if(v.revealSeconds!=5 && v.revealSeconds!=10 && v.revealSeconds!=15 && v.revealSeconds!=30)
        return QStringLiteral("Показ секрета: 5, 10, 15 или 30 секунд.");
    if(v.clipboardSeconds!=10 && v.clipboardSeconds!=20 && v.clipboardSeconds!=30 && v.clipboardSeconds!=60)
        return QStringLiteral("Очистка буфера: 10, 20, 30 или 60 секунд.");
    if(v.defaultField!=paste::Field::Secret && v.defaultField!=paste::Field::Login) return QStringLiteral("Недопустимое поле вставки.");
    if(v.closeBehavior!=CloseBehavior::Hide && v.closeBehavior!=CloseBehavior::LockHide && v.closeBehavior!=CloseBehavior::Exit)
        return QStringLiteral("Недопустимое поведение закрытия.");
#ifdef Q_OS_WIN
    if(!paste::parseHotkey(v.hotkey)) return QStringLiteral("Сочетание: Ctrl/Alt/Win и A–Z, 0–9, Space или F1–F11.");
#endif
    return crypto::generatorError(v.generator);
}
bool writeValue(QSettings& settings,const QString& key,const QVariant& value,QString* error) {
    if(error) error->clear();
    if(!settings.group().isEmpty()) {if(error) *error=QStringLiteral("Ожидаются корневые настройки."); return false;}
    const bool existed=settings.contains(key); const auto before=settings.value(key);
    QSettings attempt(settings.fileName(),settings.format());
    attempt.setFallbacksEnabled(settings.fallbacksEnabled());
    if(value.isValid()) attempt.setValue(key,value); else attempt.remove(key);
    attempt.sync();
    if(attempt.status()==QSettings::NoError) return true;
    if(existed) attempt.setValue(key,before); else attempt.remove(key);
    attempt.sync();
    QSettings check(settings.fileName(),settings.format()); check.setFallbacksEnabled(settings.fallbacksEnabled()); check.sync();
    const bool rolledBack=check.status()==QSettings::NoError && check.contains(key)==existed && (!existed || check.value(key)==before);
    if(error) *error=rolledBack?QStringLiteral("Не удалось сохранить настройку. Прежнее значение восстановлено.")
        :QStringLiteral("Не удалось сохранить настройку. Прежнее значение возвращено в памяти; состояние хранения не подтверждено.");
    return false;
}
}
