#pragma once
#include <QByteArray>
#include <sodium.h>
#include <new>
#include <utility>
namespace tb {
// Ключ шифрования в защищённой памяти libsodium: sodium_malloc даёт guard-страницы
// вокруг региона (обращение за пределы — крах процесса, а не тихая порча) и канарейки,
// проверяемые при освобождении. Копирование запрещено, только перемещение.
// data() отдаёт сырой указатель на 32 байта (XChaCha20-Poly1305 ключ).
class SecureKey {
public:
    static constexpr size_t Size = 32;
    SecureKey() : p_(static_cast<unsigned char*>(sodium_malloc(Size))) { if (!p_) throw std::bad_alloc(); }
    ~SecureKey() { sodium_free(p_); }
    SecureKey(const SecureKey&) = delete;
    SecureKey& operator=(const SecureKey&) = delete;
    SecureKey(SecureKey&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    SecureKey& operator=(SecureKey&& o) noexcept {
        if (this != &o) { sodium_free(p_); p_ = std::exchange(o.p_, nullptr); }
        return *this;
    }
    unsigned char* data() { return p_; }
    const unsigned char* data() const { return p_; }
    // Сравнение ключей — только sodium_memcmp: защита от атак по времени.
    bool operator==(const SecureKey& o) const { return sodium_memcmp(p_, o.p_, Size) == 0; }
private:
    unsigned char* p_;
};
// Обёртка над QByteArray с затиранием: содержимое гарантированно обнуляется
// (sodium_memzero, не оптимизируется компилятором) в clear() и в деструкторе.
// Память лежит в обычной куче — это осознанный компромисс: очистка есть, guard-страниц нет.
// Для сериализации модели без обычной кучи см. cbor::SecureBuffer (CborWriter.h).
// Копирование запрещено; release() передаёт владение байтами наружу (например, в файл).
class SecureBytes {
public:
    SecureBytes() = default;
    explicit SecureBytes(QByteArray b) noexcept : b_(std::move(b)) {}
    SecureBytes(const SecureBytes&) = delete;
    SecureBytes& operator=(const SecureBytes&) = delete;
    SecureBytes(SecureBytes&& o) noexcept : b_(std::move(o.b_)) { o.b_ = QByteArray(); }
    SecureBytes& operator=(SecureBytes&& o) noexcept { clear(); b_ = std::move(o.b_); o.b_ = QByteArray(); return *this; }
    ~SecureBytes() { clear(); }
    void clear() { if (!b_.isEmpty()) { sodium_memzero(b_.data(), size_t(b_.size())); b_.clear(); } }
    bool isEmpty() const { return b_.isEmpty(); }
    qsizetype size() const { return b_.size(); }
    const QByteArray& bytes() const { return b_; }
    QByteArray release() { QByteArray r = std::move(b_); b_ = QByteArray(); return r; }
private:
    QByteArray b_;
};
} // namespace tb
