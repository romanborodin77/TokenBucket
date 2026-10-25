#include "PasswordGenerator.h"
#include <QStringList>
#include <sodium.h>
#include <utility>
namespace tb::crypto {
namespace {
QString filtered(QString set, bool excludeSimilar) {
    if (excludeSimilar) for (QChar c : QStringLiteral("0O1lI")) set.remove(c);
    return set;
}
QChar pick(const QString& set) { return set.at(int(randombytes_uniform(uint(set.size())))); }
}
QString generatorError(const GeneratorOptions& o) {
    if (o.length < 8 || o.length > 128) return QStringLiteral("Длина пароля: от 8 до 128 знаков.");
    if (!o.upper && !o.lower && !o.digits && !o.symbols) return QStringLiteral("Выберите хотя бы одну группу символов.");
    return {};
}
QString generatePassword(const GeneratorOptions& o) {
    if (!generatorError(o).isEmpty()) return {};
    QStringList groups;
    if (o.upper) groups << filtered(QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ"), o.excludeSimilar);
    if (o.lower) groups << filtered(QStringLiteral("abcdefghijklmnopqrstuvwxyz"), o.excludeSimilar);
    if (o.digits) groups << filtered(QStringLiteral("0123456789"), o.excludeSimilar);
    // Без кавычек, пробела и обратной косой черты — они ломают вставку в shell/конфиги.
    if (o.symbols) groups << QStringLiteral("#!%+=-_@$*?.");
    const QString all = groups.join(QString());
    QString out;
    for (const auto& group : groups) out += pick(group);
    while (out.size() < o.length) out += pick(all);
    // Фишер–Йейтс тем же RNG: гарантированные знаки групп не стоят в начале.
    for (int i = int(out.size()) - 1; i > 0; --i) std::swap(out[i], out[int(randombytes_uniform(uint(i + 1)))]);
    return out;
}
}
