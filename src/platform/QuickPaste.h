#pragma once
#include "../model/Types.h"
#include <QString>
namespace tb::paste {
// Годится ли секрет записи для имитации вставки: одна строка без переводов
// (многострочный пароль нельзя «напечатать» надёжно — пользователь вставит вручную).
inline bool eligibleForAutoPaste(const model::Entry& e) {
    if (e.type == model::EntryType::SshKey) return false;
    const QString s = e.type == model::EntryType::ApiToken ? e.token : e.password;
    return !s.isEmpty() && !s.contains(QLatin1Char('\n')) && !s.contains(QLatin1Char('\r'));
}
#ifdef Q_OS_WIN
// Получатель вставки: фиксируется до переключения окна и перепроверяется ПОСЛЕ —
// между подтверждением и Ctrl+V активное окно могло смениться (устранение TOCTOU).
struct Recipient { void* hwnd = nullptr; QString processName, windowTitle; };
Recipient captureRecipient();
bool recipientStillValid(const Recipient& r);
bool sendCtrlV();
#endif
}
