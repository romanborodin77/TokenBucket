#pragma once
#include "../model/Types.h"
#include <QString>
namespace tb::paste {
inline bool eligibleForAutoPaste(const model::Entry& e) {
    if (e.type == model::EntryType::SshKey) return false;
    const QString s = e.type == model::EntryType::ApiToken ? e.token : e.password;
    return !s.isEmpty() && !s.contains(QLatin1Char('\n')) && !s.contains(QLatin1Char('\r'));
}
#ifdef Q_OS_WIN
struct Recipient { void* hwnd = nullptr; QString processName, windowTitle; };
Recipient captureRecipient();
bool recipientStillValid(const Recipient& r);
void sendCtrlV();
#endif
}
