#include "CborWriter.h"
#include <QChar>
#include <cstring>
#include <new>
#include <sodium.h>
namespace tb::cbor {
namespace {
// Длина строки в байтах UTF-8 без выделения памяти: один проход по UTF-16.
// Битые суррогаты заменяются на U+FFFD — ровно как в QString::toUtf8(), чтобы
// побайтовый золотой тест не расходился с эталоном Qt на таких данных.
std::size_t utf8Length(QStringView s) {
    std::size_t n = 0;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        char32_t cp = c.unicode();
        if (c.isHighSurrogate() && i + 1 < s.size() && s.at(i + 1).isLowSurrogate()) {
            cp = QChar::surrogateToUcs4(c, s.at(i + 1));
            ++i; // младший суррогат потреблён парой
        } else if (c.isSurrogate()) {
            cp = 0xFFFD;
        }
        n += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    }
    return n;
}
}
void CborWriter::setOutput(unsigned char* out, std::size_t capacity) {
    out_ = out;
    cap_ = capacity;
    pos_ = 0;
    overflow_ = false;
}
void CborWriter::put(unsigned char b) {
    if (out_) {
        if (pos_ >= cap_) { overflow_ = true; return; }
        out_[pos_] = b;
    }
    ++pos_;
}
void CborWriter::write(const void* p, std::size_t n) {
    if (n == 0) return;
    if (out_) {
        if (pos_ + n > cap_) { overflow_ = true; pos_ += n; return; }
        memcpy(out_ + pos_, p, n);
    }
    pos_ += n;
}
void CborWriter::head(unsigned major, quint64 v) {
    Q_ASSERT(major < 8);
    const unsigned char mt = unsigned char(major << 5);
    if (v < 24) {
        put(unsigned char(mt | v));                       // значение в самом байте типа
    } else if (v <= 0xff) {
        put(unsigned char(mt | 24)); put(unsigned char(v));
    } else if (v <= 0xffff) {
        put(unsigned char(mt | 25)); put(unsigned char(v >> 8)); put(unsigned char(v));
    } else if (v <= 0xffffffffu) {
        put(unsigned char(mt | 26));
        for (int shift = 24; shift >= 0; shift -= 8) put(unsigned char(v >> shift)); // big-endian
    } else {
        put(unsigned char(mt | 27));
        for (int shift = 56; shift >= 0; shift -= 8) put(unsigned char(v >> quint64(shift)));
    }
}
void CborWriter::utf8String(QStringView s, bool asBytes) {
    // definite-строка: сначала заголовок с точной длиной, затем содержимое.
    head(asBytes ? 2 : 3, quint64(utf8Length(s)));
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        char32_t cp = c.unicode();
        if (c.isHighSurrogate() && i + 1 < s.size() && s.at(i + 1).isLowSurrogate()) {
            cp = QChar::surrogateToUcs4(c, s.at(i + 1));
            ++i;
        } else if (c.isSurrogate()) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) {
            put(unsigned char(cp));
        } else if (cp < 0x800) {
            put(unsigned char(0xC0 | (cp >> 6)));
            put(unsigned char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            put(unsigned char(0xE0 | (cp >> 12)));
            put(unsigned char(0x80 | ((cp >> 6) & 0x3F)));
            put(unsigned char(0x80 | (cp & 0x3F)));
        } else {
            put(unsigned char(0xF0 | (cp >> 18)));
            put(unsigned char(0x80 | ((cp >> 12) & 0x3F)));
            put(unsigned char(0x80 | ((cp >> 6) & 0x3F)));
            put(unsigned char(0x80 | (cp & 0x3F)));
        }
    }
}
void CborWriter::text(QStringView s) { utf8String(s, false); }
void CborWriter::bytesUtf8(QStringView s) { utf8String(s, true); }
void CborWriter::bytes(const char* p, std::size_t n) {
    head(2, quint64(n));
    write(p, n);
}

SecureBuffer::SecureBuffer(std::size_t size)
    // sodium_malloc(0) не гарантирует валидный указатель — всегда берём минимум 1 байт
    : p_(static_cast<unsigned char*>(sodium_malloc(size ? size : 1))), n_(size) {
    if (!p_) throw std::bad_alloc();
}
SecureBuffer::~SecureBuffer() { if (p_) sodium_free(p_); }
SecureBuffer::SecureBuffer(SecureBuffer&& o) noexcept : p_(o.p_), n_(o.n_) { o.p_ = nullptr; o.n_ = 0; }
SecureBuffer& SecureBuffer::operator=(SecureBuffer&& o) noexcept {
    if (this != &o) {
        if (p_) sodium_free(p_);
        p_ = o.p_; n_ = o.n_;
        o.p_ = nullptr; o.n_ = 0;
    }
    return *this;
}
}
