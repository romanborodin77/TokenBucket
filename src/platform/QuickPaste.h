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
    if (e.type == model::EntryType::PlainValue && field == Field::Secret) return &e.plainValue;
    return nullptr;
}
inline bool eligibleForAutoPaste(const model::Entry& e, Field field = Field::Secret) {
    const auto* s = fieldValue(e, field);
    return s && !s->isEmpty() && !s->contains(QLatin1Char('\n'))
        && !s->contains(QLatin1Char('\r')) && !s->contains(QChar::Null)
        && !s->contains(QChar(0x2028)) && !s->contains(QChar(0x2029));
}
#ifdef Q_OS_WIN
// Идентичность и foreground проверяются отдельно; SendInput не даёт атомарной адресности.
struct Hotkey { UINT modifiers; UINT key; };
std::optional<Hotkey> parseHotkey(const QKeySequence& sequence);
struct Recipient {
    HWND hwnd = nullptr, focusHwnd = nullptr;
    DWORD pid = 0;
    FILETIME created{};
    QString processName, windowTitle;
};
Recipient captureRecipient();
bool recipientStillValid(const Recipient& r);
bool recipientExists(const Recipient& r);
bool recipientActive(const Recipient& r);
bool recipientAllowed(const Recipient& r, QString* err);
bool activateRecipient(const Recipient& r);
bool modifiersReleased();
using InputSender = decltype(&::SendInput);
UINT sendCtrlV(InputSender sender = ::SendInput);
#endif
}
