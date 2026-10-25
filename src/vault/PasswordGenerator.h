#pragma once
#include <QString>
namespace tb::crypto {
// Параметры генератора пароля редактора записи. Не секрет: хранятся в обычных настройках.
struct GeneratorOptions {
    int length = 24;
    bool upper = true, lower = true, digits = true, symbols = true, excludeSimilar = true;
    bool operator==(const GeneratorOptions&) const = default;
};
// Пустая строка — параметры допустимы; иначе понятная причина отказа.
QString generatorError(const GeneratorOptions&);
// Пароль из выбранных групп: по одному знаку каждой группы гарантирован, остальное — из
// общего алфавита, затем перемешивание. Случайность только из libsodium (crypto::init()
// должен быть вызван). При недопустимых параметрах возвращает пустую строку.
QString generatePassword(const GeneratorOptions&);
}
