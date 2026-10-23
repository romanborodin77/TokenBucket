#pragma once
#include "../model/Types.h"
#include <QString>
#ifdef Q_OS_WIN
#include <windows.h>
#include <QKeySequence>
#include <optional>
#endif
namespace tb::paste {
enum class Field { Secret, Login };
inline const QString* fieldValue(const model::Entry& e, Field field) {
    if (e.type == model::EntryType::Credentials)
        return field == Field::Login ? &e.login : &e.password;
    if (e.type == model::EntryType::ApiToken && field == Field::Secret) return &e.token;
    return nullptr;
}
inline bool eligibleForAutoPaste(const model::Entry& e, Field field = Field::Secret) {
    const auto* s = fieldValue(e, field);
    return s && !s->isEmpty() && !s->contains(QLatin1Char('\n'))
        && !s->contains(QLatin1Char('\r')) && !s->contains(QChar::Null);
}
#ifdef Q_OS_WIN
// Получатель вставки: фиксируется до переключения окна и перепроверяется ПОСЛЕ —
// между подтверждением и Ctrl+V активное окно могло смениться (устранение TOCTOU).
struct Hotkey { UINT modifiers; UINT key; };
std::optional<Hotkey> parseHotkey(const QKeySequence& sequence);
struct Recipient { void* hwnd = nullptr; QString processName, windowTitle; };
Recipient captureRecipient();
bool recipientStillValid(const Recipient& r);
bool sendCtrlV();
#endif
}
