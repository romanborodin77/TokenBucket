#pragma once
#include "../model/Types.h"
namespace tb::ui {
inline QString entryTypeLabel(model::EntryType type) {
    switch(type) {
    case model::EntryType::ApiToken: return QStringLiteral("API-токен");
    case model::EntryType::Credentials: return QStringLiteral("Учётные данные");
    case model::EntryType::SshKey: return QStringLiteral("SSH-ключ");
    case model::EntryType::PlainValue: return QStringLiteral("Простое значение");
    case model::EntryType::TextFile: return QStringLiteral("Текстовый файл — строки");
    }
    return QStringLiteral("Неизвестный тип");
}
}
