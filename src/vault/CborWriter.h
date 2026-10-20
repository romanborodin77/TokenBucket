#pragma once
#include <QStringView>
#include <QtGlobal>
#include <cstddef>
namespace tb::cbor {

// Собственный CBOR-энкодер пути СОХРАНЕНИЯ. Раньше каждый save прогонял все секреты
// через QCborValue::toCbor() и промежуточные QByteArray — то есть через обычную кучу,
// где копии могли пережить очистку. Этот писатель работает в два прохода одной и той же
// логикой: первый проход только считает размер, второй пишет напрямую в защищённый
// буфер libsodium (sodium_malloc: guard-страницы, канарейки, защита от повреждения
// при освобождении). Никаких промежуточных копий секретов не возникает.
//
// Поддерживается только то, что пишет VaultModel: беззнаковые целые, bool, text/byte
// strings (definite), массивы и карты фиксированной длины. Формат байтов побайтово
// совпадает с QCborValue::toCbor() для тех же данных — это проверяет золотой тест
// (test_model, secureCborGolden*): расхождение энкодеров означало бы несовместимый
// файл. Чтение остаётся на QCborValue::fromCbor (см. документацию остаточного риска
// в docs/security).
class CborWriter {
public:
    CborWriter() = default;                                 // режим подсчёта: байты не пишутся, растёт только позиция
    void setOutput(unsigned char* out, std::size_t capacity); // режим записи в готовый буфер
    std::size_t position() const { return pos_; }
    bool overflow() const { return overflow_; }             // выход за capacity — ошибка программирования

    void uint(quint64 v) { head(0, v); }
    void boolean(bool v) { put(v ? 0xf5 : 0xf4); }          // false = 0xf4, true = 0xf5, как у Qt
    void beginArray(std::size_t n) { head(4, quint64(n)); }
    void beginMap(std::size_t n) { head(5, quint64(n)); }
    void text(QStringView s);                               // major 3: ручная конвертация UTF-16 → UTF-8 без toUtf8()
    void bytes(const char* p, std::size_t n);               // major 2: готовые байты (ssh-ключи)
    void bytesUtf8(QStringView s);                          // major 2: строка как UTF-8-байты (токены, пароли)
private:
    void head(unsigned major, quint64 v);                   // заголовок: 5 бит типа + минимальная длина
    void put(unsigned char b);
    void write(const void* p, std::size_t n);
    void utf8String(QStringView s, bool asBytes);           // общий путь text/bytesUtf8
    unsigned char* out_ = nullptr;                          // nullptr → режим подсчёта
    std::size_t cap_ = 0, pos_ = 0;
    bool overflow_ = false;
};

// RAII-обёртка над sodium_malloc: guard-страницы вокруг региона, при освобождении
// проверяются канарейки. Перемещение без копирования; копирование запрещено.
class SecureBuffer {
public:
    SecureBuffer() = default;
    explicit SecureBuffer(std::size_t size); // бросает std::bad_alloc при отказе
    ~SecureBuffer();
    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
    SecureBuffer(SecureBuffer&& o) noexcept;
    SecureBuffer& operator=(SecureBuffer&& o) noexcept;
    const unsigned char* data() const { return p_; }
    unsigned char* data() { return p_; }
    std::size_t size() const { return n_; }
    bool isEmpty() const { return n_ == 0; }
private:
    unsigned char* p_ = nullptr;
    std::size_t n_ = 0;
};
}
