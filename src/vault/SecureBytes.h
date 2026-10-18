#pragma once
#include <QByteArray>
#include <sodium.h>
namespace tb {
class SecureKey {
public:
    static constexpr size_t Size = 32;
    SecureKey() : p_(static_cast<unsigned char*>(sodium_malloc(Size))) {}
    ~SecureKey() { sodium_free(p_); }
    SecureKey(const SecureKey&) = delete;
    SecureKey& operator=(const SecureKey&) = delete;
    unsigned char* data() { return p_; }
    const unsigned char* data() const { return p_; }
    bool operator==(const SecureKey& o) const { return sodium_memcmp(p_, o.p_, Size) == 0; }
private:
    unsigned char* p_;
};
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
