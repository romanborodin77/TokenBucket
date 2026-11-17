#pragma once
#include <QList>
#include <QString>
#include <QStringView>
#include <algorithm>

namespace tb::model::ssh {
inline constexpr int MaxTargets = 16;
inline constexpr int MaxMatches = 16;
inline constexpr int MaxAlias = 80;
inline constexpr int MaxHost = 253;
inline constexpr int MaxUser = 64;
inline constexpr int MaxPort = 65535;

inline bool printable(QStringView text, qsizetype maxLength) {
    if (text.size() > maxLength || !text.isValidUtf16()) return false;
    const auto characters = text.toString().toUcs4();
    return std::all_of(characters.cbegin(), characters.cend(), [](char32_t ch) {
        return QChar::isPrint(ch) && ch != 0x2028 && ch != 0x2029;
    });
}

inline bool validUser(QStringView user) {
    if (!printable(user, MaxUser)) return false;
    const auto characters = user.toString().toUcs4();
    return std::all_of(characters.cbegin(), characters.cend(), [](char32_t ch) {
        return QChar::isLetterOrNumber(ch) || ch == U'.' || ch == U'_' || ch == U'-' || ch == U'@' || ch == U'\\';
    });
}

// An SSH lookup may name a friendly alias or a literal DNS name/IP address.
// A zero port is internal and means the caller omitted the filter.
inline bool validLookup(QStringView machine, QStringView user, int port) {
    return !machine.isEmpty() && machine == machine.trimmed() && printable(machine, MaxHost)
        && validUser(user) && port >= 0 && port <= MaxPort;
}
}
