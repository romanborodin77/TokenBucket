# TokenBucket v1 — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Цель:** локальное приложение C++/Qt для защищённого хранения API-токенов, учётных данных и SSH-ключей на Windows 11 и Ubuntu 24.04/GNOME.

**Архитектура:** четыре слоя по спецификации — модель (чистые данные, CBOR-сериализация), vault (libsodium: Argon2id + XChaCha20-Poly1305, версионированный контейнер, атомарное сохранение), Qt UI (дерево/список/карточка + мини-панель), платформенные функции (Hello/DPAPI, буфер, автоблокировка, горячие клавиши). Приложение однопроцессное, без сети и плагинов.

**Стек:** C++20, CMake ≥ 3.30, Qt 6 Widgets (vcpkg на Windows / apt на Ubuntu), libsodium, Qt Test + CTest.

**Спецификация:** `docs/superpowers/specs/2026-10-18-tokenbucket-design.md` (утверждена 2026-10-18). План исходит из спеки; исполнители читают оба документа.

## Global Constraints

- C++20, Qt 6 Widgets, CMake, libsodium. Qt DBus — только на Linux. Криптографические примитивы не писать самостоятельно.
- Шифрование содержимого: XChaCha20-Poly1305, ключ данных 32 байта, новый случайный nonce 24 байта на каждую операцию.
- Мастер-пароль: Argon2id v1.3, salt 16 байт, opslimit 3, memlimit 67108864 байт, выход 32 байта. Из недоверенного файла принимаются только эти параметры.
- Пароль: минимум 12, максимум 1024 байта UTF-8, без trim и нормализации, подтверждение при создании. Не хранить пароль или быстрый хеш пароля.
- Лимиты: 10 000 записей; 1 МиБ на секрет/импортируемый ключ; 64 МиБ на расшифрованное содержимое; 80 МиБ на входной файл. Превышение — понятная ошибка без частичного сохранения.
- Группы: вложенность до 8 уровней (проект = уровень 1); перенос без циклов; непустой узел не удаляется.
- Времена: автоматическое скрытие показанного секрета 15 с; очистка своего содержимого буфера 20 с; автоблокировка 5 минут бездействия.
- Windows: горячая клавиша Ctrl+Alt+Space (при конфликте — сообщение и смена); исключение своих данных из истории/синхронизации буфера; Hello через `IUserConsentVerifierInterop`; DPAPI — только локальная оболочка ключа, не переносимый формат.
- Linux: не заполнять PRIMARY selection; Wayland v1 — копирование и ручная вставка; RemoteDesktop-портал не запрашивать.
- Сохранение: временный файл → проверка → атомарная замена; резервная копия предыдущей рабочей версии; без fallback прямой перезаписи основного файла.
- Права: Linux — каталог 0700, файлы 0600; Windows — DACL текущего пользователя и SYSTEM.
- Многострочные секреты и приватные SSH-ключи не выдаются через автоматическую вставку — только копирование или файл.
- Секреты не попадают в логи, аргументы командной строки, диагностические сообщения и crash-отчёты.
- Файлы: UTF-8 без BOM, LF. Git: author `imtonyromeo <246167+imtonyromeo@users.no-reply.gitverse.ru>`, без AI-атрибуции, без push.
- Точные версии инструментов (MSVC, Qt, libsodium, baseline vcpkg) записываются в `docs/build/windows-toolchain.md` в Задаче 1 и не меняются без отдельного решения.

## Review Focus

Пять классов входных данных, которые спека подразумевает, но базовые тесты задач могли бы пропустить. Каждый закреплён тестом в задаче-владельце:

1. Многострочный секрет или SSH-ключ в быстрой вставке — блокируется до SendInput (тест `test_paste_win.cpp: eligible` в Задаче 13).
2. Второй экземпляр приложения при открытом первом — отказ с сообщением, без параллельного доступа к хранилищу (тест `test_app.cpp: secondInstance` в Задаче 6).
3. Unicode-края (эмодзи, RTL, 4-байтные символы) в названиях, логинах, поиске и CBOR-цикле (тест `test_model.cpp: unicodeRoundtrip` в Задаче 4).
4. Пустой файл, файл из одного заголовка, файл больше 80 МиБ на входе — отказ без частичных данных (тесты `test_container.cpp: emptyFile / headerOnly / oversize` в Задаче 3).
5. Смена окна-получателя или его повышение между подтверждением и SendInput — отмена с сохранением ручного копирования (тест `test_paste_win.cpp: recipientChanged` в Задаче 13).

---

## Среды исполнения

- Задачи 1–13 выполняются на Windows-машине разработки (Windows 11 x64, эта машина). Оболочка Git Bash; команды сборки через `tools/build.cmd` и `tools/test.cmd`.
- Задача 14 выполняется на машине/ВМ Ubuntu 24.04 x64 GNOME Wayland (пользователь подтвердил наличие окружения).
- Задача 15 требует обе машины: упаковка Windows локально, `.deb` и матрица приёмки Ubuntu — на Ubuntu-машине.
- Каждая задача завершается коммитом в `main`. Перед коммитом — `tools/test.cmd`, все тесты зелёные.

---

### Task 1: Инструментарий Windows и скелет сборки

**Files:**
- Create: `vcpkg.json`, `CMakeLists.txt`, `CMakePresets.json`, `.gitignore`, `tools/build.cmd`, `tools/test.cmd`, `src/main.cpp`, `tests/CMakeLists.txt`, `tests/test_smoke.cpp`, `docs/build/windows-toolchain.md`

**Interfaces:**
- Consumes: ничего (первая задача).
- Produces: собираемая цель `TokenBucket` (WIN32 GUI) и тестовые цели через CTest; функция `int main(int, char**)` в `src/main.cpp`, которую Задача 6 заменит; пресеты CMake `default` (конфигурация+сборка) и `test`.

- [x] **Step 1: Установить VS BuildTools (MSVC + Windows SDK)**

Проверить место на диске (нужно ≥ 15 ГБ свободных на C:). Установить:

```bash
winget install --id Microsoft.VisualStudio.2026.BuildTools --silent --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Если winget не находит пакет 2026 — взять установщик напрямую: `https://aka.ms/vs/18/release/vs_BuildTools.exe` с теми же флагами (требуются права администратора). Если порт `qtbase` vcpkg не собирается новым набором инструментов, доустановить компонент `Microsoft.VisualStudio.Component.VC.14.4x.x86.x64` (последний стабильный MSVC-набор) через тот же установщик.

Проверка:

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
```

Expected: непустой путь, например `C:\Program Files\Microsoft Visual Studio\2026\BuildTools`.

- [x] **Step 2: Клонировать vcpkg и зафиксировать baseline**

```bash
git clone https://github.com/microsoft/vcpkg tools/vcpkg
tools/vcpkg/bootstrap-vcpkg.bat -disableMetrics
git -C tools/vcpkg rev-parse HEAD
```

Expected: 40-символьный SHA. Записать его — он подставляется в `vcpkg.json` на следующем шаге вместо строки `BASELINE_HASH` (полученный хэш — это и есть `BASELINE_HASH`).

- [x] **Step 3: Создать `vcpkg.json`**

```json
{
  "name": "tokenbucket",
  "version": "0.1.0",
  "dependencies": [ "qtbase", "libsodium" ],
  "builtin-baseline": "BASELINE_HASH"
}
```

- [x] **Step 4: Создать `.gitignore`**

```
build/
out/
tools/vcpkg/
*.user
```

- [x] **Step 5: Создать `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.30)
project(TokenBucket VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
find_package(unofficial-sodium CONFIG REQUIRED)
qt_standard_project_setup()

qt_add_executable(TokenBucket WIN32 src/main.cpp)
target_link_libraries(TokenBucket PRIVATE Qt6::Widgets unofficial-sodium::sodium)

if(WIN32)
  target_link_libraries(TokenBucket PRIVATE crypt32 user32 wtsapi32 windowsapp)
endif()

enable_testing()
add_subdirectory(tests)
```

- [x] **Step 6: Создать `CMakePresets.json`**

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "default",
      "binaryDir": "${sourceDir}/build",
      "toolchainFile": "${sourceDir}/tools/vcpkg/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" }
    }
  ],
  "buildPresets": [ { "name": "default", "configurePreset": "default" } ],
  "testPresets": [ { "name": "default", "configurePreset": "default", "output": { "outputOnFailure": true } } ]
}
```

- [x] **Step 7: Создать `tools/build.cmd` (окружение MSVC + сборка)**

```bat
@echo off
setlocal
set VCPKG_ROOT=%~dp0..\tools\vcpkg
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --preset default || exit /b 1
cmake --build --preset default || exit /b 1
```

- [x] **Step 8: Создать `tools/test.cmd`**

```bat
@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
ctest --preset default || exit /b 1
```

- [x] **Step 9: Создать `src/main.cpp` (временный каркас)**

```cpp
#include <QApplication>
#include <QLabel>
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QLabel l(u"TokenBucket"_s);
    l.show();
    return app.exec();
}
```

- [x] **Step 10: Создать `tests/CMakeLists.txt` и smoke-тест**

`tests/CMakeLists.txt`:

```cmake
add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE Qt6::Widgets Qt6::Test unofficial-sodium::sodium)
add_test(NAME smoke COMMAND test_smoke)
```

Все тестовые цели этого плана создаются по этому же шаблону (`Qt6::Widgets Qt6::Test unofficial-sodium::sodium`); для headless-запуска (Задача 14) дополнительно задавать `set_tests_properties(<имя> PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")`.

`tests/test_smoke.cpp`:

```cpp
#include <QtTest>
#include <sodium.h>
class TestSmoke : public QObject {
    Q_OBJECT
private slots:
    void libsodiumAndQtLink() {
        QVERIFY(sodium_init() >= 0);
        QCOMPARE(QString("a") + "b", QStringLiteral("ab"));
    }
};
QTEST_MAIN(TestSmoke)
#include "test_smoke.moc"
```

- [x] **Step 11: Собрать и прогнать тест**

```bash
cmd //c tools\\build.cmd
```

Expected: первая конфигурация скачивает и собирает qtbase/libsodium через vcpkg (десятки минут, это единственный долгий шаг); сборка завершается `-- Build finished`. Затем:

```bash
cmd //c tools\\test.cmd
```

Expected: `100% tests passed, 1 tests`.

- [x] **Step 12: Записать версии инструментов в `docs/build/windows-toolchain.md`**

Содержимое (подставить фактические значения из вывода команд; раздел — строки таблицы):

```markdown
# Инструментарий сборки Windows

| Компонент | Версия | Как получено |
|---|---|---|
| MSVC | <вывод cl из vcvars64: `cl` первые 2 строки> | VS BuildTools 2026 |
| Windows SDK | <вывод `vswhere -property catalog_productDisplayVersion`> | VS BuildTools |
| vcpkg baseline | <SHA из Step 2> | git rev-parse HEAD |
| Qt | <вывод `tools\vcpkg\vcpkg list` строка qtbase> | vcpkg, порт qtbase |
| libsodium | <вывод `tools\vcpkg\vcpkg list` строка libsodium> | vcpkg, порт libsodium |
| CMake/Ninja | <вывод `cmake --version`, `ninja --version` из vcvars64> | поставка VS |
```

- [x] **Step 13: Commit**

```bash
git add vcpkg.json CMakeLists.txt CMakePresets.json .gitignore tools/build.cmd tools/test.cmd src/main.cpp tests/ docs/build/windows-toolchain.md
git commit -m "Сборка: инструментарий MSVC/vcpkg и скелет проекта"
```

---

### Task 2: SecureBytes и Crypto (Argon2id + XChaCha20-Poly1305)

**Files:**
- Create: `src/vault/SecureBytes.h`, `src/vault/Crypto.h`, `src/vault/Crypto.cpp`, `tests/test_crypto.cpp`
- Modify: `CMakeLists.txt` (добавить `src/vault/Crypto.cpp` в цель TokenBucket), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: libsodium.
- Produces: `tb::SecureKey` (32 байта, `sodium_malloc`/`sodium_free`, `data()`, `Size`), `tb::SecureBytes` (перемещаемый очищаемый `QByteArray`), `tb::crypto::init()`, `tb::crypto::deriveKey(const QByteArray& passwordUtf8, const unsigned char* salt16, SecureKey& out)`, `tb::crypto::Blob { unsigned char nonce[24]; QByteArray ct; }`, `tb::crypto::seal/open(const SecureKey&, const QByteArray& aad, ...)`.

- [x] **Step 1: Написать failing-тест `tests/test_crypto.cpp`**

```cpp
#include <QtTest>
#include "../src/vault/Crypto.h"
using namespace tb;
class TestCrypto : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(crypto::init()); }
    void deriveIsDeterministicAndSaltSensitive() {
        QByteArray pw("корректный пароль 123");
        unsigned char s1[16] = {1}, s2[16] = {2};
        SecureKey a1, a2, b;
        QVERIFY(crypto::deriveKey(pw, s1, a1));
        QVERIFY(crypto::deriveKey(pw, s1, a2));
        QVERIFY(crypto::deriveKey(pw, s2, b));
        QVERIFY(a1 == a2);       // тот же пароль+salt => тот же ключ
        QVERIFY(!(a1 == b));     // другой salt => другой ключ
    }
    void sealOpenRoundtrip() {
        SecureKey k; unsigned char salt[16] = {7};
        QVERIFY(crypto::deriveKey(QByteArray("пароль-пример-1"), salt, k));
        crypto::Blob b;
        QVERIFY(crypto::seal(k, "aad", "секретные данные", b));
        QByteArray out;
        QVERIFY(crypto::open(k, "aad", b, out));
        QCOMPARE(out, QByteArray("секретные данные"));
    }
    void wrongKeyOrAadFails() {
        SecureKey k1, k2; unsigned char salt[16] = {7};
        QVERIFY(crypto::deriveKey(QByteArray("пароль-пример-1"), salt, k1));
        QVERIFY(crypto::deriveKey(QByteArray("другой-пароль-22"), salt, k2));
        crypto::Blob b;
        QVERIFY(crypto::seal(k1, "aad", "данные", b));
        QByteArray out;
        QVERIFY(!crypto::open(k2, "aad", b, out));
        QVERIFY(!crypto::open(k1, "другой-aad", b, out));
    }
    void tamperedCiphertextFails() {
        SecureKey k; unsigned char salt[16] = {7};
        QVERIFY(crypto::deriveKey(QByteArray("пароль-пример-1"), salt, k));
        crypto::Blob b;
        QVERIFY(crypto::seal(k, "aad", "данные", b));
        b.ct[0] = b.ct[0] ^ 0x01;
        QByteArray out;
        QVERIFY(!crypto::open(k, "aad", b, out));
    }
    void noncesAreUnique() {
        SecureKey k; unsigned char salt[16] = {7};
        QVERIFY(crypto::deriveKey(QByteArray("пароль-пример-1"), salt, k));
        crypto::Blob a, b;
        QVERIFY(crypto::seal(k, "aad", "x", a));
        QVERIFY(crypto::seal(k, "aad", "x", b));
        QVERIFY(memcmp(a.nonce, b.nonce, sizeof(a.nonce)) != 0);
    }
};
QTEST_MAIN(TestCrypto)
#include "test_crypto.moc"
```

- [x] **Step 2: Подключить тест и цель, запустить, убедиться в ошибке компиляции**

`tests/CMakeLists.txt` — добавить:

```cmake
add_executable(test_crypto test_crypto.cpp)
target_link_libraries(test_crypto PRIVATE Qt6::Test unofficial-sodium::sodium)
add_test(NAME crypto COMMAND test_crypto)
```

`CMakeLists.txt`: строку `qt_add_executable(TokenBucket WIN32 src/main.cpp)` дополнить: `qt_add_executable(TokenBucket WIN32 src/main.cpp src/vault/Crypto.cpp)`.

Run: `cmd //c tools\\build.cmd`
Expected: FAIL — `crypto.h: No such file or directory` (файла ещё нет).

- [x] **Step 3: Реализовать `src/vault/SecureBytes.h`**

```cpp
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
```

- [x] **Step 4: Реализовать `src/vault/Crypto.h` и `Crypto.cpp`**

`src/vault/Crypto.h`:

```cpp
#pragma once
#include <QByteArray>
#include "SecureBytes.h"
namespace tb::crypto {
bool init();
bool deriveKey(const QByteArray& passwordUtf8, const unsigned char* salt16, SecureKey& out);
struct Blob { unsigned char nonce[24]; QByteArray ct; };
bool seal(const SecureKey& key, const QByteArray& aad, const QByteArray& plain, Blob& out);
bool open(const SecureKey& key, const QByteArray& aad, const Blob& blob, QByteArray& plain);
}
```

`src/vault/Crypto.cpp`:

```cpp
#include "Crypto.h"
#include <sodium.h>
namespace tb::crypto {
bool init() { return sodium_init() >= 0; }
bool deriveKey(const QByteArray& pw, const unsigned char* salt, SecureKey& out) {
    // Фиксированные параметры спеки: opslimit 3, memlimit 64 МиБ, Argon2id v1.3, выход 32 байта.
    return crypto_pwhash(out.data(), SecureKey::Size, pw.constData(), quint64(pw.size()),
                         salt, 3, 64ull * 1024 * 1024, crypto_pwhash_ARGON2ID_13) == 0;
}
bool seal(const SecureKey& k, const QByteArray& aad, const QByteArray& plain, Blob& out) {
    out.ct.resize(int(plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES));
    unsigned long long clen = 0;
    randombytes_buf(out.nonce, sizeof(out.nonce));
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char*>(out.ct.data()), &clen,
            reinterpret_cast<const unsigned char*>(plain.constData()), quint64(plain.size()),
            reinterpret_cast<const unsigned char*>(aad.constData()), quint64(aad.size()),
            nullptr, out.nonce, k.data()) != 0)
        return false;
    out.ct.resize(int(clen));
    return true;
}
bool open(const SecureKey& k, const QByteArray& aad, const Blob& blob, QByteArray& plain) {
    plain.resize(int(blob.ct.size()));
    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plain.data()), &mlen, nullptr,
            reinterpret_cast<const unsigned char*>(blob.ct.constData()), quint64(blob.ct.size()),
            reinterpret_cast<const unsigned char*>(aad.constData()), quint64(aad.size()),
            blob.nonce, k.data()) != 0) {
        sodium_memzero(plain.data(), size_t(plain.size()));
        plain.clear();
        return false;
    }
    plain.resize(int(mlen));
    return true;
}
} // namespace tb::crypto
```

- [x] **Step 5: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: все тесты PASS (smoke, crypto — 6 тестов).

- [x] **Step 6: Commit**

```bash
git add src/vault/ tests/test_crypto.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "Крипто: Argon2id и XChaCha20-Poly1305 поверх libsodium"
```

---

### Task 3: Контейнер v1

**Files:**
- Create: `src/vault/Container.h`, `src/vault/Container.cpp`, `tests/test_container.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `tb::crypto::*`, `tb::SecureKey`.
- Produces: `tb::container::Status {Ok, BadFormat, Unsupported, WrongKey, TooLarge}`; `struct Opened { Status status; QByteArray cbor; tb::SecureKey dataKey; }`; `uint8_t ModePassword=1, ModeHello=2`; `uint8_t readMode(const QByteArray& fileBytes)` (0 при плохом формате); `QByteArray packPassword(const QByteArray& passwordUtf8, const QByteArray& cbor, SecureKey* outKey)` (пустой QByteArray при ошибке); `QByteArray packHello(const QByteArray& cbor, SecureKey* outKey)` — только Windows; `Opened openPassword(const QByteArray& fileBytes, const QByteArray& passwordUtf8)`; `Opened openHello(const QByteArray& fileBytes)` — только Windows; `QByteArray reseal(const QByteArray& fileBytes, const SecureKey& dataKey, const QByteArray& cbor)`; константы `MaxFileBytes=80MiB`, `MaxPlainBytes=64MiB`.

Байтовая раскладка (все числа little-endian):

```
смещение  размер  поле
0         4       магия "TBVK"
4         2       u16 version = 1
6         1       u8 mode: 1=password, 2=hello
7         2       u16 kdfAlg = 1 (Argon2id)
9         4       u32 opslimit = 3
13        4       u32 memlimitBytes = 67108864
17        16      salt
33        24      wrapNonce (для mode=2 заполнен нулями)
57        4       u32 wrapLen
61        wrapLen wrap: mode=1 — XChaCha(KEK, aad=bytes[0..61), dataKey);
                  mode=2 — DPAPI(dataKey) текущего пользователя
61+W      24      ctNonce
85+W      8       u64 ctLen
93+W      ctLen   ct: XChaCha(dataKey, aad=bytes[0..93+W), cbor)
```

`reseal` сохраняет байты `0..(61+W)` без изменений (включая оболочку ключа и salt) и заменяет `ctNonce/ctLen/ct` новой печатью модели тем же dataKey.

- [x] **Step 1: Failing-тест `tests/test_container.cpp`**

```cpp
#include <QtTest>
#include "../src/vault/Container.h"
#include "../src/vault/Crypto.h"
using namespace tb;
class TestContainer : public QObject {
    Q_OBJECT
    QString dir_;
private slots:
    void initTestCase() { QVERIFY(crypto::init()); dir_ = QTemporaryDir().path() + "/"; }
    QByteArray pack() {
        SecureKey k;
        QByteArray b = container::packPassword("пароль-мастера-1", "cbor-модель", &k);
        return b;
    }
    void roundtripPassword() {
        QByteArray f = pack();
        QVERIFY(!f.isEmpty());
        QCOMPARE(container::readMode(f), container::ModePassword);
        auto o = container::openPassword(f, "пароль-мастера-1");
        QCOMPARE(o.status, container::Status::Ok);
        QCOMPARE(o.cbor, QByteArray("cbor-модель"));
    }
    void wrongPassword() {
        auto o = container::openPassword(pack(), "неверный-пароль!!");
        QCOMPARE(o.status, container::Status::WrongKey);
        QVERIFY(o.cbor.isEmpty());
    }
    void emptyFileAndHeaderOnly() {
        QCOMPARE(container::openPassword(QByteArray(), "x").status, container::Status::BadFormat);
        QByteArray f = pack();
        QCOMPARE(container::openPassword(f.left(60), "пароль-мастера-1").status, container::Status::BadFormat);
        QCOMPARE(container::openPassword(f.left(93), "пароль-мастера-1").status, container::Status::BadFormat);
    }
    void truncation() {
        QByteArray f = pack();
        QCOMPARE(container::openPassword(f.left(f.size() - 1), "пароль-мастера-1").status, container::Status::BadFormat);
    }
    void tamperedHeader() {
        QByteArray f = pack();
        f[17] = f[17] ^ 0x01; // бит в salt: ломает и KEK, и AAD
        QVERIFY(container::openPassword(f, "пароль-мастера-1").status != container::Status::Ok);
    }
    void tamperedCiphertext() {
        QByteArray f = pack();
        f[f.size() - 1] = f[f.size() - 1] ^ 0x01;
        QVERIFY(container::openPassword(f, "пароль-мастера-1").status != container::Status::Ok);
    }
    void unsupportedVersionAndParams() {
        QByteArray f = pack();
        f[4] = 2; // version
        QCOMPARE(container::openPassword(f, "пароль-мастера-1").status, container::Status::Unsupported);
        f = pack();
        f[9] = 9; // opslimit
        QCOMPARE(container::openPassword(f, "пароль-мастера-1").status, container::Status::Unsupported);
    }
    void oversizeInput() {
        QByteArray big(container::MaxFileBytes + 1, 'x');
        QCOMPARE(container::openPassword(big, "x").status, container::Status::TooLarge);
    }
    void oversizePlaintext() {
        SecureKey k;
        QByteArray big(container::MaxPlainBytes + 1, 'y');
        QVERIFY(container::packPassword("пароль-мастера-1", big, &k).isEmpty());
    }
    void resealKeepsWrapAndReplacesBody() {
        SecureKey k;
        QByteArray f = container::packPassword("пароль-мастера-1", "первое", &k);
        QByteArray f2 = container::reseal(f, k, "второе");
        QVERIFY(!f2.isEmpty());
        QCOMPARE(f2.left(93), f.left(93)); // заголовок и оболочка ключа не изменились
        auto o = container::openPassword(f2, "пароль-мастера-1");
        QCOMPARE(o.status, container::Status::Ok);
        QCOMPARE(o.cbor, QByteArray("второе"));
        QCOMPARE(container::reseal(QByteArray("мусор"), k, "x"), QByteArray());
    }
#ifdef Q_OS_WIN
    void helloRoundtripViaDpapi() {
        SecureKey k;
        QByteArray f = container::packHello("модель-hello", &k);
        QVERIFY(!f.isEmpty());
        QCOMPARE(container::readMode(f), container::ModeHello);
        auto o = container::openHello(f);
        QCOMPARE(o.status, container::Status::Ok);
        QCOMPARE(o.cbor, QByteArray("модель-hello"));
        // пароль к hello-контейнеру не применим
        QCOMPARE(container::openPassword(f, "что-угодно").status, container::Status::Unsupported);
    }
#endif
};
QTEST_MAIN(TestContainer)
#include "test_container.moc"
```

- [x] **Step 2: Подключить тест, запустить, убедиться в FAIL**

`tests/CMakeLists.txt` — добавить то же, что для crypto (цель `test_container`). `CMakeLists.txt` — добавить `src/vault/Container.cpp` в цель TokenBucket.
Run: `cmd //c tools\\build.cmd`
Expected: FAIL — `Container.h: No such file or directory`.

- [x] **Step 3: Реализовать `src/vault/Container.h`**

```cpp
#pragma once
#include <QByteArray>
#include "SecureBytes.h"
namespace tb::container {
enum class Status { Ok, BadFormat, Unsupported, WrongKey, TooLarge };
constexpr uint8_t ModePassword = 1, ModeHello = 2;
constexpr qsizetype MaxFileBytes  = 80LL * 1024 * 1024;
constexpr qsizetype MaxPlainBytes = 64LL * 1024 * 1024;
struct Opened {
    Status status = Status::BadFormat;
    QByteArray cbor;
    SecureKey dataKey;
};
uint8_t readMode(const QByteArray& fileBytes);
QByteArray packPassword(const QByteArray& passwordUtf8, const QByteArray& cbor, SecureKey* outKey);
QByteArray reseal(const QByteArray& fileBytes, const SecureKey& dataKey, const QByteArray& cbor);
Opened openPassword(const QByteArray& fileBytes, const QByteArray& passwordUtf8);
#ifdef Q_OS_WIN
QByteArray packHello(const QByteArray& cbor, SecureKey* outKey);
Opened openHello(const QByteArray& fileBytes);
#endif
}
```

- [x] **Step 4: Реализовать `src/vault/Container.cpp`**

```cpp
#include "Container.h"
#include "Crypto.h"
#include <QDataStream>
#include <sodium.h>
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif
namespace tb::container {
namespace {
constexpr char kMagic[4] = {'T','B','V','K'};
constexpr uint16_t kVersion = 1, kKdfArgon2id = 1;
constexpr uint32_t kOpslimit = 3, kMemlimit = 64u * 1024 * 1024;
constexpr int kHead = 61; // всё до wrap

struct Parsed {
    uint8_t mode = 0;
    QByteArray aad1, salt, wrap, aad2;
    crypto::Blob ct;
};

bool parse(const QByteArray& f, Parsed& p) {
    if (f.size() < kHead + 24 + 8 || f.size() > int(MaxFileBytes)) return false;
    QDataStream ds(reinterpret_cast<const uchar*>(f.constData()), int(f.size()));
    ds.setByteOrder(QDataStream::LittleEndian);
    char magic[4]; uint16_t ver, kdf; uint8_t mode; uint32_t ops, mem; quint32 wrapLen;
    ds.readRawData(magic, 4); ds >> ver >> mode >> kdf >> ops >> mem >> wrapLen;
    if (ds.status() != QDataStream::Ok || memcmp(magic, kMagic, 4) != 0) return false;
    if (ver != kVersion || (mode != ModePassword && mode != ModeHello) || kdf != kKdfArgon2id) { p.mode = 255; return true; }
    if (ops != kOpslimit || mem != kMemlimit) { p.mode = 255; return true; }
    p.mode = mode;
    p.salt.resize(16); ds.readRawData(p.salt.data(), 16);
    QByteArray wrapNonce(24, '\0'); ds.readRawData(wrapNonce.data(), 24);
    ds >> wrapLen;
    if (wrapLen > uint32_t(f.size())) return false;
    p.aad1 = QByteArray::fromRawData(f.constData(), kHead); // до wrap
    p.wrap.resize(int(wrapLen)); ds.readRawData(p.wrap.data(), int(wrapLen));
    p.aad2 = QByteArray::fromRawData(f.constData(), kHead + int(wrapLen) + 24 + 8);
    memcpy(p.ct.nonce, f.constData() + kHead + int(wrapLen), 24);
    ds.skipRawData(24); // ctNonce прочитан напрямую из f
    quint64 ctLen; ds >> ctLen;
    if (ctLen != quint64(f.size()) - (kHead + wrapLen + 24 + 8)) return false;
    p.ct.ct = f.mid(kHead + int(wrapLen) + 24 + 8);
    return ds.status() == QDataStream::Ok;
}

QByteArray build(uint8_t mode, const QByteArray& salt, const QByteArray& wrapNonce,
                const QByteArray& wrap, const QByteArray& ctNonce, const QByteArray& ct) {
    QByteArray f;
    QDataStream ds(&f, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.writeRawData(kMagic, 4);
    ds << kVersion << mode << kKdfArgon2id << kOpslimit << kMemlimit;
    ds.writeRawData(salt.constData(), 16);
    ds.writeRawData(wrapNonce.constData(), 24);
    ds << uint32_t(wrap.size());
    ds.writeRawData(wrap.constData(), wrap.size());
    ds.writeRawData(ctNonce.constData(), 24);
    ds << quint64(ct.size());
    ds.writeRawData(ct.constData(), ct.size());
    return f;
}
} // namespace

uint8_t readMode(const QByteArray& f) { Parsed p; if (!parse(f, p)) return 0; return p.mode == 255 ? 0 : p.mode; }

QByteArray packPassword(const QByteArray& pw, const QByteArray& cbor, SecureKey* outKey) {
    if (cbor.size() > int(MaxPlainBytes)) return {};
    SecureKey dataKey, kek;
    QByteArray salt(16, '\0'); randombytes_buf(salt.data(), 16);
    if (!crypto::deriveKey(pw, reinterpret_cast<const unsigned char*>(salt.constData()), kek)) return {};
    QByteArray keyBytes(reinterpret_cast<const char*>(dataKey.data()), SecureKey::Size);
    crypto::Blob wrap;
    if (!crypto::seal(kek, {}, keyBytes, wrap)) return {};
    // aad1 = первые 61 байт итогового файла = константная головка без wrap
    QByteArray head; { QDataStream ds(&head, QIODevice::WriteOnly); ds.setByteOrder(QDataStream::LittleEndian);
        ds.writeRawData(kMagic,4); ds << kVersion << uint8_t(ModePassword) << kKdfArgon2id << kOpslimit << kMemlimit;
        ds.writeRawData(salt.constData(),16); ds.writeRawData(wrap.nonce,24); ds << uint32_t(wrap.ct.size()); }
    crypto::Blob ct;
    if (!crypto::seal(dataKey, head, cbor, ct)) return {};
    if (outKey) memcpy(outKey->data(), dataKey.data(), SecureKey::Size);
    return build(ModePassword, salt, QByteArray(reinterpret_cast<char*>(wrap.nonce), 24), wrap.ct,
                 QByteArray(reinterpret_cast<char*>(ct.nonce), 24), ct.ct);
}

Opened openPassword(const QByteArray& f, const QByteArray& pw) {
    Opened r; Parsed p;
    if (f.size() > int(MaxFileBytes)) { r.status = Status::TooLarge; return r; }
    if (!parse(f, p)) { r.status = Status::BadFormat; return r; }
    if (p.mode == 255) { r.status = Status::Unsupported; return r; }
    if (p.mode != ModePassword) { r.status = Status::Unsupported; return r; }
    SecureKey kek;
    if (!crypto::deriveKey(pw, reinterpret_cast<const unsigned char*>(p.salt.constData()), kek)) { r.status = Status::WrongKey; return r; }
    crypto::Blob wrap; wrap.ct = p.wrap; memcpy(wrap.nonce, p.aad1.constData() + 33, 24);
    QByteArray keyBytes;
    if (!crypto::open(kek, p.aad1, wrap, keyBytes) || keyBytes.size() != SecureKey::Size) { r.status = Status::WrongKey; return r; }
    SecureKey dataKey; memcpy(dataKey.data(), keyBytes.constData(), size_t(keyBytes.size()));
    sodium_memzero(keyBytes.data(), size_t(keyBytes.size()));
    if (!crypto::open(dataKey, p.aad2, p.ct, r.cbor)) { r.status = Status::WrongKey; return r; }
    if (r.cbor.size() > int(MaxPlainBytes)) { r.status = Status::TooLarge; r.cbor.clear(); return r; }
    memcpy(r.dataKey.data(), dataKey.data(), SecureKey::Size);
    r.status = Status::Ok;
    return r;
}

QByteArray reseal(const QByteArray& f, const SecureKey& dataKey, const QByteArray& cbor) {
    if (cbor.size() > int(MaxPlainBytes)) return {};
    Parsed p;
    if (!parse(f, p) || p.mode == 255) return {};
    QByteArray aad2 = QByteArray::fromRawData(f.constData(), kHead + p.wrap.size() + 24 + 8);
    crypto::Blob ct;
    if (!crypto::seal(dataKey, aad2, cbor, ct)) return {};
    QByteArray ctNonce(reinterpret_cast<const char*>(ct.nonce), 24);
    QByteArray head = f.left(kHead + p.wrap.size()); // головка + старая оболочка ключа
    QByteArray tail; { QDataStream ds(&tail, QIODevice::WriteOnly); ds.setByteOrder(QDataStream::LittleEndian);
        ds.writeRawData(ctNonce.constData(), 24); ds << quint64(ct.ct.size()); }
    return head + tail + ct.ct;
}

#ifdef Q_OS_WIN
namespace {
QByteArray dpapiWrap(const SecureKey& key) {
    DATA_BLOB in{ SecureKey::Size, reinterpret_cast<BYTE*>(key.data()) }, out{};
    if (!CryptProtectData(&in, L"TokenBucket", nullptr, nullptr, nullptr, 0, &out)) return {};
    QByteArray r(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return r;
}
}
QByteArray packHello(const QByteArray& cbor, SecureKey* outKey) {
    if (cbor.size() > int(MaxPlainBytes)) return {};
    SecureKey dataKey;
    QByteArray wrap = dpapiWrap(dataKey);
    if (wrap.isEmpty()) return {};
    QByteArray salt(16, '\0'); randombytes_buf(salt.data(), 16);
    QByteArray wrapNonce(24, '\0');
    QByteArray head; { QDataStream ds(&head, QIODevice::WriteOnly); ds.setByteOrder(QDataStream::LittleEndian);
        ds.writeRawData(kMagic,4); ds << kVersion << uint8_t(ModeHello) << kKdfArgon2id << kOpslimit << kMemlimit;
        ds.writeRawData(salt.constData(),16); ds.writeRawData(wrapNonce.constData(),24); ds << uint32_t(wrap.size()); }
    crypto::Blob ct;
    if (!crypto::seal(dataKey, head, cbor, ct)) return {};
    if (outKey) memcpy(outKey->data(), dataKey.data(), SecureKey::Size);
    return build(ModeHello, salt, wrapNonce, wrap, QByteArray(reinterpret_cast<char*>(ct.nonce), 24), ct.ct);
}
Opened openHello(const QByteArray& f) {
    Opened r; Parsed p;
    if (f.size() > int(MaxFileBytes)) { r.status = Status::TooLarge; return r; }
    if (!parse(f, p)) { r.status = Status::BadFormat; return r; }
    if (p.mode == 255 || p.mode != ModeHello) { r.status = Status::Unsupported; return r; }
    DATA_BLOB in{ DWORD(p.wrap.size()), reinterpret_cast<BYTE*>(const_cast<char*>(p.wrap.constData())) }, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) { r.status = Status::WrongKey; return r; }
    if (out.cbData != SecureKey::Size) { LocalFree(out.pbData); r.status = Status::BadFormat; return r; }
    memcpy(r.dataKey.data(), out.pbData, SecureKey::Size);
    LocalFree(out.pbData);
    if (!crypto::open(r.dataKey, p.aad2, p.ct, r.cbor)) { r.status = Status::WrongKey; r.cbor.clear(); return r; }
    if (r.cbor.size() > int(MaxPlainBytes)) { r.status = Status::TooLarge; r.cbor.clear(); return r; }
    r.status = Status::Ok;
    return r;
}
#endif
} // namespace tb::container
```

Примечание к тесту `helloRoundtripViaDpapi`: `openPassword` hello-файла возвращает `Unsupported` — это закреплено тестом; на Linux файл режима 2 так же не открывается (функции `openHello` нет).

- [x] **Step 5: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS — все контейнерные тесты, включая `helloRoundtripViaDpapi` на Windows.

- [x] **Step 6: Commit**

```bash
git add src/vault/Container.h src/vault/Container.cpp tests/test_container.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "Контейнер v1: раскладка, режимы пароль/Hello, reseal"
```

---

### Task 4: VaultModel — дерево, лимиты, поиск, CBOR

**Files:**
- Create: `src/model/Types.h`, `src/model/VaultModel.h`, `src/model/VaultModel.cpp`, `tests/test_model.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: QtCore (QUuid, QCborValue, QDateTime).
- Produces: `tb::model::Id = QUuid`; `enum class EntryType { ApiToken, Credentials, SshKey }`; `struct Entry { Id id, groupId; EntryType type; QString title, note, url, login, password, token, sshPassphrase, sshComment; QByteArray sshPrivate, sshPublic; QDateTime created, modified; bool favorite=false, reverify=false; }`; класс `VaultModel`:

```cpp
QString addNode(Id id, Id parent, const QString& name);   // parent.isNull() => проект
QString renameNode(Id id, const QString& name);
QString moveNode(Id id, Id newParent);                    // циклы и глубина > 8 — ошибка
QString removeNode(Id id);                                // непустой — ошибка
QList<Id> childNodes(Id parent) const;
const QHash<Id, Node>& nodes() const;
QString setEntry(const Entry& e);                         // лимиты 10000/1МиБ, родитель существует
QString removeEntry(Id id);
const Entry* entry(Id id) const;
QList<Id> entriesInGroup(Id groupId) const;
QList<Id> entriesInSubtree(Id groupId) const;               // записи узла и всех потомков
QList<Id> search(const QString& needle) const;            // названия/логины/заметки + имена узлов (поддерево); не секреты
int depth(Id node) const;                                 // проект = 1
QByteArray toCbor() const;
enum class ParseStatus { Ok, Bad, TooLarge };
ParseStatus fromCbor(const QByteArray& cbor, QString* err);
void clear();
```

Все мутаторы возвращают `QString()` при успехе и русский текст ошибки при отказе.

- [x] **Step 1: Failing-тест `tests/test_model.cpp`**

```cpp
#include <QtTest>
#include "../src/model/VaultModel.h"
using namespace tb::model;
class TestModel : public QObject {
    Q_OBJECT
    VaultModel m;
    Id p1{QUuid::createUuid()}, g1{QUuid::createUuid()};
private slots:
    void init() { m = VaultModel{}; QVERIFY(m.addNode(p1, Id(), "Проект").isEmpty()); QVERIFY(m.addNode(g1, p1, "Группа").isEmpty()); }
    void nodeCrudAndDepth() {
        Id a{QUuid::createUuid()};
        QVERIFY(m.addNode(a, g1, "Вложенная").isEmpty());
        QCOMPARE(m.depth(a), 3);
        QVERIFY(m.renameNode(a, "Новое имя").isEmpty());
        QVERIFY(!m.removeNode(g1).isEmpty());
        QVERIFY(m.removeNode(a).isEmpty());
        QVERIFY(m.removeNode(g1).isEmpty());
    }
    void moveRejectsProjectCycleDepth() {
        QVERIFY(!m.moveNode(p1, g1).isEmpty());     // проект нельзя переместить
        Id a{QUuid::createUuid()}, b{QUuid::createUuid()};
        QVERIFY(m.addNode(a, g1, "A").isEmpty());
        QVERIFY(!m.moveNode(g1, a).isEmpty());      // цикл: g1 предок a
        QVERIFY(m.addNode(b, a, "B").isEmpty());
        QVERIFY(m.moveNode(b, p1).isEmpty());       // перенос вверх допустим
    }
    void depthLimitEight() {
        Id cur = p1;
        for (int i = 2; i <= 8; ++i) { Id g{QUuid::createUuid()}; QVERIFY2(m.addNode(g, cur, "уровень").isEmpty(), qPrintable(QString("уровень %1").arg(i))); cur = g; }
        Id tooDeep{QUuid::createUuid()};
        QVERIFY(!m.addNode(tooDeep, cur, "девятый").isEmpty());
    }
    void entryLimits() {
        Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.type = EntryType::ApiToken; e.title = "Токен"; e.token = "x";
        for (int i = 0; i < 3; ++i) { Entry t = e; t.id = QUuid::createUuid(); QVERIFY(m.setEntry(t).isEmpty()); }
        QByteArray big(int(VaultModel::MaxSecretBytes) + 1, 'z');
        Entry t = e; t.id = QUuid::createUuid(); t.token = QString::fromUtf8(big);
        QVERIFY(!m.setEntry(t).isEmpty());      // секрет > 1 МиБ
        QVERIFY(m.setEntry(e).isEmpty());       // add/replace
        QVERIFY(m.removeEntry(e.id).isEmpty());
    }
    void maxEntriesTenThousand() {
        Entry e; e.groupId = p1; e.type = EntryType::ApiToken; e.title = "t"; e.token = "x";
        for (int i = 0; i < VaultModel::MaxEntries; ++i) { Entry t = e; t.id = QUuid::createUuid(); QVERIFY2(m.setEntry(t).isEmpty(), qPrintable(QString::number(i))); }
        Entry over = e; over.id = QUuid::createUuid();
        QVERIFY(!m.setEntry(over).isEmpty());
    }
    void searchSkipsSecretsAndCoversProjectName() {
        Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.type = EntryType::Credentials;
        e.title = "Логин-запись"; e.login = "user@example.com"; e.password = "XyZzY_секрет";
        e.note = "заметка про прод";
        QVERIFY(m.setEntry(e).isEmpty());
        QVERIFY(m.search("логин").contains(e.id));
        QVERIFY(m.search("user@example").contains(e.id));
        QVERIFY(m.search("заметка").contains(e.id));
        QVERIFY(m.search("проект").contains(e.id));  // имя проекта находит записи его поддерева
        QVERIFY(!m.search("XyZzY").contains(e.id));  // секрет не ищется
    }
    void cborRoundtripAndUnicode() {
        Entry e; e.id = QUuid::createUuid(); e.groupId = g1; e.type = EntryType::SshKey;
        e.title = "ключ 🚀 عربى"; e.login = "日本語ログイン";
        e.sshPrivate = QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\nabc\r\n-----END-----\n");
        e.sshPublic = QByteArray("ssh-ed25519 AAAA test");
        e.sshPassphrase = "фраза"; e.sshComment = "комментарий"; e.reverify = true; e.favorite = true;
        e.created = QDateTime::currentDateTimeUtc(); e.modified = e.created.addSecs(5);
        QVERIFY(m.setEntry(e).isEmpty());
        QByteArray cbor = m.toCbor();
        VaultModel m2;
        QVERIFY(m2.fromCbor(cbor, nullptr) == VaultModel::ParseStatus::Ok);
        QCOMPARE(m2.entry(e.id)->title, e.title);
        QCOMPARE(m2.entry(e.id)->sshPrivate, e.sshPrivate);
        QCOMPARE(m2.entry(e.id)->reverify, true);
        QVERIFY(m2.search("🚀").contains(e.id));
    }
    void cborRejectsGarbageAndOversize() {
        VaultModel m2; QString err;
        QVERIFY(m2.fromCbor(QByteArray("мусор"), &err) == VaultModel::ParseStatus::Bad);
        QVERIFY(!err.isEmpty());
        QByteArray big(int(VaultModel::MaxPlainBytesCBOR) + 1, 'q');
        QVERIFY(m2.fromCbor(big, &err) == VaultModel::ParseStatus::TooLarge);
    }
    void clearRemovesEverything() {
        m.clear();
        QVERIFY(m.nodes().isEmpty());
        QVERIFY(m.search("").isEmpty());
    }
};
QTEST_MAIN(TestModel)
#include "test_model.moc"
```

- [x] **Step 2: Подключить цель `test_model`, запустить — FAIL (нет VaultModel.h)**

- [x] **Step 3: Реализовать `src/model/Types.h`**

```cpp
#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUuid>
namespace tb::model {
using Id = QUuid;
enum class EntryType { ApiToken, Credentials, SshKey };
struct Entry {
    Id id, groupId;
    EntryType type = EntryType::ApiToken;
    QString title, note, url, login, password, token, sshPassphrase, sshComment;
    QByteArray sshPrivate, sshPublic;
    QDateTime created, modified;
    bool favorite = false, reverify = false;
};
}
```

- [x] **Step 4: Реализовать `src/model/VaultModel.h/.cpp`**

`src/model/VaultModel.h`:

```cpp
#pragma once
#include <QHash>
#include <QList>
#include "Types.h"
namespace tb::model {
class VaultModel {
public:
    struct Node { Id id, parent; QString name; }; // parent.isNull() => проект
    static constexpr int MaxEntries = 10000;
    static constexpr qsizetype MaxSecretBytes = 1024 * 1024;
    static constexpr qsizetype MaxPlainBytesCBOR = 64LL * 1024 * 1024;
    static constexpr int MaxDepth = 8;

    QString addNode(Id id, Id parent, const QString& name);
    QString renameNode(Id id, const QString& name);
    QString moveNode(Id id, Id newParent);
    QString removeNode(Id id);
    QList<Id> childNodes(Id parent) const;
    const QHash<Id, Node>& nodes() const { return nodes_; }
    int depth(Id node) const;

    QString setEntry(const Entry& e);
    QString removeEntry(Id id);
    const Entry* entry(Id id) const;
    const QHash<Id, Entry>& entries() const { return entries_; }
    QList<Id> entriesInGroup(Id groupId) const;
    QList<Id> entriesInSubtree(Id groupId) const;
    QList<Id> search(const QString& needle) const;

    QByteArray toCbor() const;
    enum class ParseStatus { Ok, Bad, TooLarge };
    ParseStatus fromCbor(const QByteArray& cbor, QString* err);
    void clear();
private:
    bool wouldCycle(Id node, Id newParent) const;
    static bool secretTooBig(const Entry& e);
    QHash<Id, Node> nodes_;
    QHash<Id, Entry> entries_;
};
}
```

`src/model/VaultModel.cpp`:

```cpp
#include "VaultModel.h"
#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
namespace tb::model {
namespace {
QString iso(const QDateTime& t) { return t.toUTC().toString(Qt::ISODateWithMs); }
QDateTime fromIso(const QCborValue& v) { return QDateTime::fromString(v.toString(), Qt::ISODateWithMs); }
Id idFrom(const QCborValue& v) { return QUuid::fromString(v.toString()); }
QString idStr(const Id& id) { return id.toString(QUuid::WithoutBraces); }
}
QString VaultModel::addNode(Id id, Id parent, const QString& name) {
    if (name.isEmpty()) return "Имя не может быть пустым.";
    if (nodes_.contains(id) || entries_.contains(id)) return "Такой идентификатор уже есть.";
    if (!parent.isNull()) {
        if (!nodes_.contains(parent)) return "Родитель не найден.";
        if (depth(parent) >= MaxDepth) return "Превышена вложенность 8 уровней.";
    }
    nodes_.insert(id, {id, parent, name});
    return {};
}
QString VaultModel::renameNode(Id id, const QString& name) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return "Узел не найден.";
    if (name.isEmpty()) return "Имя не может быть пустым.";
    it->name = name;
    return {};
}
int VaultModel::depth(Id node) const {
    int d = 1;
    Id p = nodes_.value(node).parent;
    while (!p.isNull()) { ++d; p = nodes_.value(p).parent; }
    return d;
}
bool VaultModel::wouldCycle(Id node, Id newParent) const {
    Id p = newParent;
    while (!p.isNull()) { if (p == node) return true; p = nodes_.value(p).parent; }
    return false;
}
QString VaultModel::moveNode(Id id, Id newParent) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return "Узел не найден.";
    if (it->parent.isNull()) return "Проект нельзя переместить.";
    if (!nodes_.contains(newParent)) return "Новый родитель не найден.";
    if (wouldCycle(id, newParent)) return "Перенос создал бы цикл.";
    if (id == newParent) return "Нельзя переместить в себя.";
    int subtree = 1;
    std::function<int(Id)> h = [&](Id root) { int m = 0; for (auto&n : nodes_) if (n.parent == root) m = std::max(m, 1 + h(n.id)); return m; };
    subtree = 1 + h(id);
    if (depth(newParent) + subtree > MaxDepth) return "Превышена вложенность 8 уровней.";
    it->parent = newParent;
    return {};
}
QString VaultModel::removeNode(Id id) {
    if (!nodes_.contains(id)) return "Узел не найден.";
    for (auto& n : nodes_) if (n.parent == id) return "Сначала удалите вложенные группы.";
    if (!entriesInGroup(id).isEmpty()) return "Сначала удалите записи из группы.";
    nodes_.remove(id);
    return {};
}
QList<Id> VaultModel::childNodes(Id parent) const {
    QList<Id> r;
    for (auto& n : nodes_) if (n.parent == parent) r << n.id;
    std::sort(r.begin(), r.end(), [&](Id a, Id b){ return nodes_[a].name < nodes_[b].name; });
    return r;
}
bool VaultModel::secretTooBig(const Entry& e) {
    return e.token.toUtf8().size() > MaxSecretBytes || e.password.toUtf8().size() > MaxSecretBytes
        || e.sshPrivate.size() > MaxSecretBytes || e.sshPublic.size() > MaxSecretBytes
        || e.sshPassphrase.toUtf8().size() > MaxSecretBytes;
}
QString VaultModel::setEntry(const Entry& e) {
    if (!nodes_.contains(e.groupId)) return "Проект или группа не найдены.";
    if (!entries_.contains(e.id) && entries_.size() >= MaxEntries) return "Достигнут предел 10 000 записей.";
    if (secretTooBig(e)) return "Секрет превышает 1 МиБ.";
    entries_.insert(e.id, e);
    return {};
}
QString VaultModel::removeEntry(Id id) { return entries_.remove(id) ? QString() : "Запись не найдена."; }
const Entry* VaultModel::entry(Id id) const { return entries_.contains(id) ? &entries_[id] : nullptr; }
QList<Id> VaultModel::entriesInGroup(Id groupId) const {
    QList<Id> r;
    for (auto& e : entries_) if (e.groupId == groupId) r << e.id;
    return r;
}
QList<Id> VaultModel::entriesInSubtree(Id groupId) const {
    QList<Id> r = entriesInGroup(groupId);
    for (auto& n : nodes_) if (n.parent == groupId) r << entriesInSubtree(n.id);
    return r;
}
QList<Id> VaultModel::search(const QString& needle) const {
    QList<Id> r;
    if (needle.isEmpty()) return r;
    for (auto& e : entries_)
        if (e.title.contains(needle, Qt::CaseInsensitive) || e.login.contains(needle, Qt::CaseInsensitive)
            || e.note.contains(needle, Qt::CaseInsensitive))
            r << e.id;
    for (auto& n : nodes_)
        if (n.name.contains(needle, Qt::CaseInsensitive))
            for (auto id : entriesInSubtree(n.id))
                if (!r.contains(id)) r << id;
    return r;
}
QByteArray VaultModel::toCbor() const {
    QCborMap root;
    root[u"v"_s] = 1;
    QCborArray ns;
    for (auto& n : nodes_) { QCborMap m; m[u"id"_s] = idStr(n.id); m[u"p"_s] = n.parent.isNull() ? QString() : idStr(n.parent); m[u"n"_s] = n.name; ns << m; }
    root[u"nodes"_s] = ns;
    QCborArray es;
    for (auto& e : entries_) {
        QCborMap m;
        m[u"id"_s] = idStr(e.id); m[u"g"_s] = idStr(e.groupId);
        m[u"type"_s] = int(e.type);
        m[u"title"_s] = e.title; m[u"note"_s] = e.note; m[u"url"_s] = e.url;
        m[u"cr"_s] = iso(e.created); m[u"mod"_s] = iso(e.modified);
        m[u"fav"_s] = e.favorite; m[u"rv"_s] = e.reverify;
        m[u"tok"_s] = e.token.toUtf8(); m[u"login"_s] = e.login; m[u"pw"_s] = e.password.toUtf8();
        m[u"sshpriv"_s] = e.sshPrivate; m[u"sshpub"_s] = e.sshPublic;
        m[u"sshph"_s] = e.sshPassphrase; m[u"sshc"_s] = e.sshComment;
        es << m;
    }
    root[u"entries"_s] = es;
    return QCborValue(root).toCbor();
}
VaultModel::ParseStatus VaultModel::fromCbor(const QByteArray& cbor, QString* err) {
    if (cbor.size() > MaxPlainBytesCBOR) { if (err) *err = "Содержимое хранилища превышает 64 МиБ."; return ParseStatus::TooLarge; }
    QCborParserError pe;
    QCborValue v = QCborValue::fromCbor(cbor, &pe);
    if (!v.isMap() || pe.error != QCborError::NoError) { if (err) *err = "Хранилище повреждено."; return ParseStatus::Bad; }
    QCborMap root = v.toMap();
    if (root.value(u"v"_s) != 1) { if (err) *err = "Неподдерживаемая версия данных."; return ParseStatus::Bad; }
    VaultModel fresh;
    for (const auto& nv : root.value(u"nodes"_s).toArray()) {
        QCborMap m = nv.toMap();
        Id id = idFrom(m.value(u"id"_s)), parent = idFrom(m.value(u"p"_s));
        if (id.isNull() || fresh.nodes_.contains(id)) { if (err) *err = "Хранилище повреждено (узлы)."; return ParseStatus::Bad; }
        QString e = fresh.addNode(id, parent, m.value(u"n"_s).toString());
        if (!e.isEmpty()) { if (err) *err = e; return ParseStatus::Bad; }
    }
    int count = 0;
    for (const auto& ev : root.value(u"entries"_s).toArray()) {
        if (++count > MaxEntries) { if (err) *err = "Превышен предел 10 000 записей."; return ParseStatus::TooLarge; }
        QCborMap m = ev.toMap();
        Entry e;
        e.id = idFrom(m.value(u"id"_s)); e.groupId = idFrom(m.value(u"g"_s));
        e.type = EntryType(m.value(u"type"_s).toInteger());
        if (e.id.isNull() || !fresh.nodes_.contains(e.groupId) || int(e.type) < 0 || int(e.type) > 2) { if (err) *err = "Хранилище повреждено (записи)."; return ParseStatus::Bad; }
        e.title = m.value(u"title"_s).toString(); e.note = m.value(u"note"_s).toString(); e.url = m.value(u"url"_s).toString();
        e.created = fromIso(m.value(u"cr"_s)); e.modified = fromIso(m.value(u"mod"_s));
        e.favorite = m.value(u"fav"_s).toBool(); e.reverify = m.value(u"rv"_s).toBool();
        e.token = QString::fromUtf8(m.value(u"tok"_s).toByteArray()); e.login = m.value(u"login"_s).toString();
        e.password = QString::fromUtf8(m.value(u"pw"_s).toByteArray());
        e.sshPrivate = m.value(u"sshpriv"_s).toByteArray(); e.sshPublic = m.value(u"sshpub"_s).toByteArray();
        e.sshPassphrase = m.value(u"sshph"_s).toString(); e.sshComment = m.value(u"sshc"_s).toString();
        if (secretTooBig(e)) { if (err) *err = "Секрет превышает 1 МиБ."; return ParseStatus::TooLarge; }
        fresh.entries_.insert(e.id, e);
    }
    *this = std::move(fresh);
    return ParseStatus::Ok;
}
void VaultModel::clear() { nodes_.clear(); entries_.clear(); }
}
```

Примечание: `moveNode` использует `std::function` — включить `#include <functional>` в .cpp.

- [x] **Step 5: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS. Тест `maxEntriesTenThousand` выполняет Argon2-независимую работу и завершается быстро; если CTest превышает 60 с — проверить, что в тесте не вызывается криптография.

- [x] **Step 6: Commit**

```bash
git add src/model/ tests/test_model.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "Модель: дерево узлов, лимиты, поиск без секретов, CBOR"
```

---

### Task 5: FileOps и VaultStore — атомарное сохранение, резервная копия, права

**Files:**
- Create: `src/platform/FileOps.h`, `src/platform/fileops_win.cpp`, `src/platform/fileops_posix.cpp`, `src/vault/VaultStore.h`, `src/vault/VaultStore.cpp`, `tests/test_store.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: QtCore.
- Produces: `tb::store::FileOps` (виртуальные `writeAll(path, bytes)`, `renameReplace(from, to)`, `removeFile(path)`), `tb::store::realFileOps()`, `tb::store::ensurePrivateDir(dir)`; класс `tb::store::VaultStore { VaultStore(const QString& dir, FileOps& ops = realFileOps()); QString mainPath() const /* dir/vault.tb */; QString backupPath() const /* dir/vault.tb.bak */; QString tmpPath() const; bool save(const QByteArray& bytes, QString* err); enum class Load { Ok, Missing, IoError }; Load loadBytes(const QString& path, QByteArray* out) const; }`.

Актуальное решение при реализации (2026-10-19, заменяет исходные эскизы ниже): `save` создаёт и проверяет tmp, отдельно записывает и проверяет backup.tmp из подтверждённой исправной версии main, атомарно заменяет bak, затем атомарно заменяет main. Основной файл не переносится в bak, откат не нужен. Перед изменением существующего main обязательны `acceptVerified(bytes)` после проверки AEAD и модели и совпадение файла с подтверждённой версией. После успешного save версия подтверждается автоматически. Повреждённый, изменённый или непроверенный main не затирает резервную копию. `loadBytes` ограничен 80 МиБ и очищает out при отказе.

Дополнение интерфейса: `void VaultStore::acceptVerified(const QByteArray& bytes)`. В Задаче 6 вызывать его после успешного `finishOpen` и до публикации состояния Ready для парольного и Hello-открытия. Не подтверждать байты после одного лишь чтения файла. При восстановлении из bak выполнять отдельный сценарий; нельзя подтверждать повреждённый main.

`FileOps::writeAll` создаёт только новый файл (CREATE_NEW / O_EXCL), не перезаписывает существующий и задаёт защищённые права при создании. В Задаче 12 существующий файл выдачи SSH не удалять перед записью; при необходимости подтверждённой замены использовать отдельный защищённый tmp и атомарную замену. На POSIX синхронизируются файл и каталог после переименования. Если fsync каталога отказывает после замены main, предыдущие байты остаются в bak, операция возвращает отказ.

- [x] **Step 1: Failing-тест `tests/test_store.cpp`**

```cpp
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "../src/vault/VaultStore.h"
#include "../src/platform/FileOps.h"
using namespace tb::store;
namespace {
struct FaultOps : FileOps {
    bool failWrite = false;     // отказ записи tmp
    int failRenameCall = 0;     // 1 = первый renameReplace (main->bak), 2 = второй (tmp->main)
    int renameCalls = 0;
    bool writeAll(const QString& p, const QByteArray& b) override {
        if (failWrite) return false;
        QFile f(p); if (!f.open(QIODevice::WriteOnly)) return false;
        return f.write(b) == b.size() && f.flush();
    }
    bool renameReplace(const QString& from, const QString& to) override {
        ++renameCalls;
        if (renameCalls == failRenameCall) return false;
        if (QFile::exists(to)) QFile::remove(to);   // имитация атомарной замены
        return QFile::rename(from, to);
    }
    bool removeFile(const QString& p) override { return QFile::remove(p); }
};
}
class TestStore : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
    void saveCreatesMainAndRotatesBackup() {
        VaultStore s(dir_.path());
        QString err;
        QVERIFY(s.save("первый", &err));
        QCOMPARE(QFile(s.mainPath()).exists(), true);
        QVERIFY(s.save("второй", &err));
        QByteArray bak; QCOMPARE(s.loadBytes(s.backupPath(), &bak), VaultStore::Load::Ok);
        QCOMPARE(bak, QByteArray("первый"));
        QByteArray main; QCOMPARE(s.loadBytes(s.mainPath(), &main), VaultStore::Load::Ok);
        QCOMPARE(main, QByteArray("второй"));
    }
    void writeFailureLeavesMainIntact() {
        VaultStore s(dir_.path());
        QString err; QVERIFY(s.save("исходная", &err));
        FaultOps ops; ops.failWrite = true;
        VaultStore s2(dir_.path(), ops);
        QVERIFY(!s2.save("новая", &err)); QVERIFY(!err.isEmpty());
        QByteArray main; QCOMPARE(s.loadBytes(s.mainPath(), &main), VaultStore::Load::Ok);
        QCOMPARE(main, QByteArray("исходная"));
    }
    void secondRenameFailureRestoresFromBackup() {
        VaultStore s(dir_.path());
        QString err; QVERIFY(s.save("исходная", &err)); // main существует
        FaultOps ops; ops.failRenameCall = 2;           // отказ tmp -> main
        VaultStore s2(dir_.path(), ops);
        QVERIFY(!s2.save("новая", &err));
        QByteArray main; QCOMPARE(s.loadBytes(s.mainPath(), &main), VaultStore::Load::Ok);
        QCOMPARE(main, QByteArray("исходная"));   // main восстановлен из резервной копии
    }
    void firstRenameFailureLeavesMainAndNoBackup() {
        VaultStore s(dir_.path());
        QString err; QVERIFY(s.save("исходная", &err));
        FaultOps ops; ops.failRenameCall = 1;           // отказ main -> bak
        VaultStore s2(dir_.path(), ops);
        QVERIFY(!s2.save("новая", &err));
        QByteArray main; QCOMPARE(s.loadBytes(s.mainPath(), &main), VaultStore::Load::Ok);
        QCOMPARE(main, QByteArray("исходная"));
    }
    void loadMissing() {
        VaultStore s(dir_.path() + "/нет");
        QByteArray b;
        QCOMPARE(s.loadBytes(s.mainPath(), &b), VaultStore::Load::Missing);
    }
#ifndef Q_OS_WIN
    void posixFileAndDirPermissions() {
        VaultStore s(dir_.path());
        QString err; QVERIFY(s.save("данные", &err));
        QFileInfo fi(s.mainPath());
        QCOMPARE(fi.permissions() & QFile::ReadOwner | QFile::WriteOwner, QFile::ReadOwner | QFile::WriteOwner);
        QCOMPARE(int(fi.permissions() & (QFile::ReadGroup | QFile::ReadOther)), 0);
    }
#endif
};
QTEST_MAIN(TestStore)
#include "test_store.moc"
```

- [x] **Step 2: Подключить цель `test_store` (и `tests/CMakeLists.txt`, и корневой `CMakeLists.txt` с файлами платформ), запуск — FAIL**

`CMakeLists.txt` — после `qt_add_executable`:

```cmake
target_sources(TokenBucket PRIVATE
  src/vault/VaultStore.cpp
)
if(WIN32)
  target_sources(TokenBucket PRIVATE src/platform/fileops_win.cpp)
else()
  target_sources(TokenBucket PRIVATE src/platform/fileops_posix.cpp)
endif()
```

- [x] **Step 3: Реализовать `src/platform/FileOps.h`**

```cpp
#pragma once
#include <QByteArray>
#include <QString>
namespace tb::store {
struct FileOps {
    virtual ~FileOps() = default;
    virtual bool writeAll(const QString& path, const QByteArray& bytes) = 0;
    virtual bool renameReplace(const QString& from, const QString& to) = 0;
    virtual bool removeFile(const QString& path) = 0;
};
FileOps& realFileOps();
bool ensurePrivateDir(const QString& dir); // 0700 POSIX / DACL пользователя и SYSTEM на Windows
}
```

- [x] **Step 4: Реализовать `src/platform/fileops_win.cpp`**

```cpp
#include "FileOps.h"
#include <QDir>
#include <windows.h>
#include <aclapi.h>
#include <lmapibuf.h>
namespace tb::store {
namespace {
// DACL: полный доступ текущему пользователю и SYSTEM; ошибки DACL — отказ операции.
PSID userSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
    DWORD len = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len);
    PSID r = nullptr;
    if (GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
        auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
        DWORD sz = GetLengthSid(tu->User.Sid);
        r = LocalAlloc(LPTR, sz);
        if (r) CopySid(sz, r, tu->User.Sid);
    }
    CloseHandle(token);
    return r;
}
void freeSid(PSID s) { if (s) LocalFree(s); }
bool setUserSystemDacl(PSECURITY_DESCRIPTOR sd) {
    PSID us = userSid(); if (!us) return false;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID sys = nullptr;
    AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sys);
    EXPLICIT_ACCESSW ea[2] = {};
    ea[0].grfInheritance = NO_INHERITANCE; ea[0].grfAccessPermissions = GENERIC_ALL; ea[0].grfAccessMode = GRANT_ACCESS;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID; ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(us);
    ea[1] = ea[0]; ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sys);
    PACL acl = nullptr;
    BOOL ok = SetEntriesInAclW(2, ea, nullptr, &acl) == ERROR_SUCCESS
           && SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE);
    if (sys) FreeSid(sys); freeSid(us);
    return ok;
}
bool writeFileProtected(const QString& path, const QByteArray& bytes) {
    SECURITY_DESCRIPTOR sd;
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) || !setUserSystemDacl(&sd))
        return false;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), &sd, FALSE };
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE, 0, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = TRUE; DWORD written = 0;
    if (!bytes.isEmpty()) ok = WriteFile(h, bytes.constData(), DWORD(bytes.size()), &written, nullptr) && written == DWORD(bytes.size());
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}
}
bool ensurePrivateDir(const QString& dir) {
    if (!QDir().mkpath(dir)) return false;
    PSID us = userSid(); if (!us) return false;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID sys = nullptr;
    AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sys);
    EXPLICIT_ACCESSW ea[2] = {};
    ea[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT; ea[0].grfAccessPermissions = GENERIC_ALL; ea[0].grfAccessMode = GRANT_ACCESS;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID; ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(us);
    ea[1] = ea[0]; ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sys);
    PACL acl = nullptr;
    DWORD rc = SetEntriesInAclW(2, ea, nullptr, &acl);
    if (rc == ERROR_SUCCESS)
        rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<const wchar_t*>(dir.utf16())),
                                   SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    if (sys) FreeSid(sys); freeSid(us);
    return rc == ERROR_SUCCESS;
}
struct RealOps : FileOps {
    bool writeAll(const QString& p, const QByteArray& b) override { return writeFileProtected(p, b); }
    bool renameReplace(const QString& from, const QString& to) override {
        return MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    bool removeFile(const QString& p) override { return DeleteFileW(reinterpret_cast<LPCWSTR>(p.utf16())) != 0; }
};
FileOps& realFileOps() { static RealOps ops; return ops; }
}
```

Включить `#include <vector>` в начало файла. DACL-оболочка одноразовая на вызов — секретность важнее аллокаций.

- [x] **Step 5: Реализовать `src/platform/fileops_posix.cpp`**

```cpp
#include "FileOps.h"
#include <QDir>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace tb::store {
namespace {
bool writeFile0600(const QString& path, const QByteArray& bytes) {
    int fd = ::open(path.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    const char* p = bytes.constData(); ssize_t left = bytes.size();
    while (left > 0) { ssize_t n = ::write(fd, p, left); if (n <= 0) { ::close(fd); return false; } p += n; left -= n; }
    bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}
}
bool ensurePrivateDir(const QString& dir) {
    if (!QDir().mkpath(dir)) return false;
    return ::chmod(dir.toUtf8().constData(), S_IRWXU) == 0;
}
struct RealOps : FileOps {
    bool writeAll(const QString& p, const QByteArray& b) override { return writeFile0600(p, b); }
    bool renameReplace(const QString& from, const QString& to) override { return ::rename(from.toUtf8().constData(), to.toUtf8().constData()) == 0; }
    bool removeFile(const QString& p) override { return ::unlink(p.toUtf8().constData()) == 0; }
};
FileOps& realFileOps() { static RealOps ops; return ops; }
}
```

- [x] **Step 6: Реализовать `src/vault/VaultStore.h/.cpp`**

`src/vault/VaultStore.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QString>
#include "../platform/FileOps.h"
namespace tb::store {
class VaultStore {
public:
    VaultStore(const QString& dir, FileOps& ops = realFileOps());
    QString mainPath() const;
    QString backupPath() const;
    QString tmpPath() const;
    bool save(const QByteArray& bytes, QString* err);
    enum class Load { Ok, Missing, IoError };
    Load loadBytes(const QString& path, QByteArray* out) const;
private:
    QString dir_;
    FileOps& ops_;
};
}
```

`src/vault/VaultStore.cpp`:

```cpp
#include "VaultStore.h"
#include <QFile>
namespace tb::store {
VaultStore::VaultStore(const QString& dir, FileOps& ops) : dir_(dir), ops_(ops) {}
QString VaultStore::mainPath() const { return dir_ + "/vault.tb"; }
QString VaultStore::backupPath() const { return dir_ + "/vault.tb.bak"; }
QString VaultStore::tmpPath() const { return dir_ + "/vault.tb.tmp"; }
VaultStore::Load VaultStore::loadBytes(const QString& path, QByteArray* out) const {
    QFile f(path);
    if (!f.exists()) return Load::Missing;
    if (!f.open(QIODevice::ReadOnly)) return Load::IoError;
    *out = f.readAll();
    return f.error() == QFile::NoError ? Load::Ok : Load::IoError;
}
bool VaultStore::save(const QByteArray& bytes, QString* err) {
    auto fail = [this, err](const char* m) { ops_.removeFile(tmpPath()); if (err) *err = QString::fromUtf8(m); return false; };
    if (!ops_.writeAll(tmpPath(), bytes)) return fail("Не удалось записать временный файл.");
    QByteArray check;
    if (loadBytes(tmpPath(), &check) != Load::Ok || check != bytes) return fail("Проверка временного файла не прошла.");
    const bool hadMain = QFile::exists(mainPath());
    if (hadMain && !ops_.renameReplace(mainPath(), backupPath())) return fail("Не удалось подготовить резервную копию.");
    if (!ops_.renameReplace(tmpPath(), mainPath())) {
        if (hadMain) ops_.renameReplace(backupPath(), mainPath());
        return fail("Не удалось заменить файл хранилища.");
    }
    return true;
}
}
```

- [x] **Step 7: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS (на Windows posix-тест пропущен по `#ifndef Q_OS_WIN`).

- [x] **Step 8: Commit**

```bash
git add src/platform/ src/vault/VaultStore.h src/vault/VaultStore.cpp tests/test_store.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "Хранилище: атомарная замена, резервная копия, права файлов"
```

---

### Task 6: Application, создание и разблокировка, единственный экземпляр, Hello-обёртка

Актуальные решения реализации (2026-10-19, приоритет над исходными эскизами ниже):

- Hello использует `IAsyncOperation<UserConsentVerificationResult>` и значение Verified; HSTRING передаётся через get_abi. availability/request выполняются из рабочего потока с MTA и парным uninit_apartment. Не использовать Availability как результат проверки.
- Публичного `openHelloContainer` без проверки нет. `createHelloVault(err, hwnd)` и `unlockHello(err, hwnd)` сами запрашивают Hello. HWND передаётся от видимого окна прогресса. Подмена запроса через аргумент конструктора используется только тестами; она не подтверждает реальную биометрию.
- KDF, чтение, запись и Hello выполняются через установленный QtConcurrent. Публичные bool-методы ожидают результат через Qt event loop, сохраняя отзывчивость GUI. `busy()`/`busyChanged(bool)` блокируют повторный запуск. `generation()` атомарен: блокировка отменяет публикацию результата и ещё не начавшуюся запись. Уже выполняющийся системный вызов записи не прерывается, сохраняется его атомарность.
- После успешных AEAD и разбора CBOR вызывается `store_.acceptVerified(bytes)`, затем в GUI-потоке устанавливаются модель, ключ, mode и Ready; stateChanged публикуется после этого. В задаче 7 запрещать редактирование модели при busy=true.
- `Application::acquireInstanceLock(dir, err)` используется и main, и тестом второго экземпляра. `unlockMode()` даёт только подсказку выбора диалога; доступ разрешает полная проверка контейнера.
- SetupDialog/UnlockDialog используют штатные Qt Widgets, маскируют ввод, проверяют длину в байтах без trim, подтверждение и очищают поля. До задачи 7 после открытия показывается временный статус с кнопками блокировки и закрытия.
- В тесте плана слово «коротко» содержит 14 байт UTF-8 и не является коротким по спецификации; отрицательный пример заменён на `short`.

**Files:**
- Create: `src/app/Application.h`, `src/app/Application.cpp`, `src/platform/hello_win.h`, `src/platform/hello_win.cpp`, `src/ui/SetupDialog.h`, `src/ui/SetupDialog.cpp`, `src/ui/UnlockDialog.h`, `src/ui/UnlockDialog.cpp`, `tests/test_app.cpp`
- Modify: `src/main.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `container::*`, `VaultStore`, `VaultModel`, `hello_win`.
- Produces: `tb::Application`:

```cpp
enum class Gate { NeedSetup, NeedUnlock, Ready };
enum class Mode { None, Password, Hello };
Gate gate() const; Mode mode() const;
static bool validPassword(const QByteArray& utf8);            // 12..1024 байт
bool createPasswordVault(const QByteArray& pw, QString* err);
bool unlock(const QByteArray& pw, QString* err);              // режим пароля
bool save(QString* err);                                       // reseal текущей моделью
void lock();                                                   // полная очистка
tb::model::VaultModel* model();
quint64 generation() const;                                    // +1 при каждой блокировке
signals: void stateChanged();
```

Windows-дополнения: `bool createHelloVault(QString* err); bool unlockHello(QString* err); bool switchToPassword(const QByteArray& newPw, QString* err); bool switchToHello(QString* err);` (switch — в Задаче 11, объявить сейчас и реализовать в 11 нельзя; поэтому switch-методы добавляются в Задаче 11).

`tb::platform::hello` (только Windows): `enum class Availability { Available, NotConfigured, Unavailable }; Availability availability(); enum class Result { Verified, Failed, Canceled }; Result request(void* hwnd, const QString& prompt);` — запрос выполняется в отдельном потоке (MTA), окно-владелец передаётся HWND.

- [x] **Step 1: Failing-тест `tests/test_app.cpp`**

```cpp
#include <QtTest>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include "../src/app/Application.h"
using namespace tb;
class TestApp : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
    void passwordFlow() {
        QByteArray pw12("двенадцать-байт"); // >12 байт UTF-8
        QVERIFY(pw12.size() >= 12);
        Application app(dir_.path());
        QCOMPARE(app.gate(), Application::Gate::NeedSetup);
        QVERIFY(!Application::validPassword(QByteArray("коротко")));
        QVERIFY(!Application::validPassword(QByteArray(1025, 'x')));
        QVERIFY(Application::validPassword(pw12));
        QString err;
        QVERIFY(app.createPasswordVault(pw12, &err));
        QCOMPARE(app.gate(), Application::Gate::Ready);
        QVERIFY(app.model()->addNode(QUuid::createUuid(), QUuid(), "Проект").isEmpty());
        QVERIFY(app.save(&err));
        app.lock();
        QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
        QVERIFY(!app.unlock(QByteArray("неверный-пароль!"), &err));  // 17 байт, валидная длина
        QVERIFY(!err.isEmpty());
        QVERIFY(app.unlock(pw12, &err));
        QCOMPARE(app.gate(), Application::Gate::Ready);
        QVERIFY(app.model()->nodes().size() == 1);
    }
    void secondInstanceLock() {
        QString path = dir_.path() + "/instance.lock";
        QLockFile a(path); QVERIFY(a.tryLock(0));
        QLockFile b(path); QVERIFY(!b.tryLock(0));
        a.unlock();
    }
#ifdef Q_OS_WIN
    void helloVaultFlow() {
        Application app(dir_.path() + "/h");
        QString err;
        QVERIFY(app.createHelloVault(&err));
        QCOMPARE(app.mode(), Application::Mode::Hello);
        app.lock();
        // unlockHello без реального устройства Hello в автотесте не проверяем;
        // проверяем, что контейнер создан и имеет режим Hello
        QVERIFY(QFile::exists(app.storePathForTest()));
    }
#endif
};
QTEST_MAIN(TestApp)
#include "test_app.moc"
```

- [x] **Step 2: Подключить цель `test_app`, запуск — FAIL**

`tests/CMakeLists.txt` — цель `test_app` (ссылка на Qt6::Widgets, нужны диалоги позже; пока Test). В корневом `CMakeLists.txt` добавить `src/app/Application.cpp`; на Windows `src/platform/hello_win.cpp` и `target_link_libraries(... windowsapp)` (уже добавлено в Задаче 1).

- [x] **Step 3: Реализовать `src/platform/hello_win.h/.cpp`**

`src/platform/hello_win.h`:

```cpp
#pragma once
#include <QString>
namespace tb::platform::hello {
enum class Availability { Available, NotConfigured, Unavailable };
enum class Result { Verified, Failed, Canceled };
Availability availability();
Result request(void* hwnd, const QString& prompt); // блокирующий вызов; выполнить не из потока GUI
}
```

`src/platform/hello_win.cpp`:

```cpp
#include "hello_win.h"
#include <QFutureInterface>
#include <QtConcurrent/QtConcurrentRun> // отдельная зависимость; проще std::thread:
#include <thread>
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>
#include <UserConsentVerifierInterop.h>
namespace tb::platform::hello {
Availability availability() {
    // Синхронный запрос доступности из MTA-потока.
    auto r = std::async(std::launch::async, [] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            using namespace winrt::Windows::Security::Credentials::UI;
            auto av = UserConsentVerifier::CheckAvailabilityAsync().get();
            if (av == UserConsentVerifierAvailability::Available) return Availability::Available;
            if (av == UserConsentVerifierAvailability::DeviceNotPresent
             || av == UserConsentVerifierAvailability::NotConfiguredForUser) return Availability::NotConfigured;
            return Availability::Unavailable;
        } catch (...) { return Availability::Unavailable; }
    });
    return r.get();
}
Result request(void* hwnd, const QString& prompt) {
    auto r = std::async(std::launch::async, [hwnd, prompt] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            using namespace winrt::Windows::Security::Credentials::UI;
            using namespace winrt::Windows::Foundation;
            auto factory = winrt::get_activation_factory<UserConsentVerifier>();
            auto interop = factory.as<IUserConsentVerifierInterop>();
            winrt::hstring p(prompt.toStdWString());
            IAsyncOperation<UserConsentVerifierAvailability> op{ nullptr };
            interop->RequestVerificationForWindowAsync(static_cast<HWND>(hwnd), p.get(),
                                                       __uuidof(op), winrt::put_abi(op));
            auto res = op.get();
            if (res == UserConsentVerifierAvailability::Available) return Result::Verified;
            return Result::Failed;
        } catch (const winrt::hresult_canceled&) { return Result::Canceled; }
          catch (...) { return Result::Failed; }
    });
    return r.get();
}
}
```

Отмену пользователем отличает `hresult_canceled`; на практике Windows Hello возвращает `DeviceNotPresent`/отказ — итоговые коды сверяются в ручной приёмке №6, автотест проверяет только компиляцию и компоновку (шаг 7).

- [x] **Step 4: Реализовать `src/app/Application.h/.cpp`**

`src/app/Application.h`:

```cpp
#pragma once
#include <QObject>
#include <memory>
#include "../model/VaultModel.h"
#include "../vault/SecureBytes.h"
#include "../vault/VaultStore.h"
namespace tb {
class Application : public QObject {
    Q_OBJECT
public:
    enum class Gate { NeedSetup, NeedUnlock, Ready };
    enum class Mode { None, Password, Hello };
    explicit Application(const QString& dataDir, QObject* parent = nullptr);
    Gate gate() const { return gate_; }
    Mode mode() const { return mode_; }
    static bool validPassword(const QByteArray& utf8);
    bool createPasswordVault(const QByteArray& pw, QString* err);
    bool unlock(const QByteArray& pw, QString* err);
    bool save(QString* err);
    void lock();
    model::VaultModel* model() { return &model_; }
    quint64 generation() const { return generation_; }
    QString storePathForTest() const { return store_.mainPath(); }
#ifdef Q_OS_WIN
    bool createHelloVault(QString* err);
    bool unlockHello(QString* err); // Hello-проверка + DPAPI; вызывается из UI после platform::hello::request
    bool openHelloContainer(QString* err); // DPAPI-открытие без UI-проверки (для unlockHello)
#endif
signals:
    void stateChanged();
private:
    bool finishOpen(container::Opened& o, QString* err);
    store::VaultStore store_;
    model::VaultModel model_;
    std::unique_ptr<SecureKey> dataKey_;
    Gate gate_ = Gate::NeedSetup;
    Mode mode_ = Mode::None;
    quint64 generation_ = 0;
};
}
```

`src/app/Application.cpp`:

```cpp
#include "Application.h"
#include "../vault/Container.h"
#include <QFile>
namespace tb {
Application::Application(const QString& dataDir, QObject* parent)
    : QObject(parent), store_(dataDir) {
    if (QFile::exists(store_.mainPath())) gate_ = Gate::NeedUnlock;
}
bool Application::validPassword(const QByteArray& utf8) { return utf8.size() >= 12 && utf8.size() <= 1024; }
bool Application::createPasswordVault(const QByteArray& pw, QString* err) {
    if (!validPassword(pw)) { if (err) *err = "Пароль: минимум 12 и максимум 1024 байт."; return false; }
    if (!store::ensurePrivateDir(QFileInfo(store_.mainPath()).absolutePath())) { if (err) *err = "Не удалось создать каталог данных."; return false; }
    SecureKey k;
    QByteArray c = container::packPassword(pw, model_.toCbor(), &k);
    if (c.isEmpty() || !store_.save(c, err)) { if (err && err->isEmpty()) *err = "Не удалось сохранить хранилище."; return false; }
    dataKey_ = std::make_unique<SecureKey>(); memcpy(dataKey_->data(), k.data(), SecureKey::Size);
    mode_ = Mode::Password; gate_ = Gate::Ready;
    emit stateChanged();
    return true;
}
bool Application::finishOpen(container::Opened& o, QString* err) {
    if (o.status != container::Status::Ok) {
        if (err) *err = o.status == container::Status::WrongKey ? "Неверный пароль или файл повреждён."
                       : o.status == container::Status::TooLarge ? "Файл превышает допустимый размер."
                   : "Файл хранилища не поддерживается или повреждён.";
        return false;
    }
    QString perr;
    if (model_.fromCbor(o.cbor, &perr) != model::VaultModel::ParseStatus::Ok) { if (err) *err = perr; return false; }
    dataKey_ = std::make_unique<SecureKey>(); memcpy(dataKey_->data(), o.dataKey.data(), SecureKey::Size);
    gate_ = Gate::Ready;
    emit stateChanged();
    return true;
}
bool Application::unlock(const QByteArray& pw, QString* err) {
    QByteArray bytes;
    if (store_.loadBytes(store_.mainPath(), &bytes) != store::VaultStore::Load::Ok) { if (err) *err = "Не удалось прочитать файл хранилища."; return false; }
    auto o = container::openPassword(bytes, pw);
    if (!finishOpen(o, err)) return false;
    mode_ = Mode::Password;
    return true;
}
bool Application::save(QString* err) {
    if (gate_ != Gate::Ready || !dataKey_) { if (err) *err = "Хранилище не разблокировано."; return false; }
    QByteArray bytes;
    if (store_.loadBytes(store_.mainPath(), &bytes) != store::VaultStore::Load::Ok) { if (err) *err = "Не удалось прочитать файл хранилища."; return false; }
    QByteArray nb = container::reseal(bytes, *dataKey_, model_.toCbor());
    if (nb.isEmpty()) { if (err) *err = "Не удалось зашифровать данные."; return false; }
    return store_.save(nb, err);
}
void Application::lock() {
    model_ = model::VaultModel{};
    dataKey_.reset();
    mode_ = Mode::None;
    ++generation_;
    gate_ = QFile::exists(store_.mainPath()) ? Gate::NeedUnlock : Gate::NeedSetup;
    emit stateChanged();
}
#ifdef Q_OS_WIN
bool Application::createHelloVault(QString* err) {
    if (!store::ensurePrivateDir(QFileInfo(store_.mainPath()).absolutePath())) { if (err) *err = "Не удалось создать каталог данных."; return false; }
    SecureKey k;
    QByteArray c = container::packHello(model_.toCbor(), &k);
    if (c.isEmpty() || !store_.save(c, err)) { if (err && err->isEmpty()) *err = "Не удалось сохранить хранилище (DPAPI недоступен?)."; return false; }
    dataKey_ = std::make_unique<SecureKey>(); memcpy(dataKey_->data(), k.data(), SecureKey::Size);
    mode_ = Mode::Hello; gate_ = Gate::Ready;
    emit stateChanged();
    return true;
}
bool Application::openHelloContainer(QString* err) {
    QByteArray bytes;
    if (store_.loadBytes(store_.mainPath(), &bytes) != store::VaultStore::Load::Ok) { if (err) *err = "Не удалось прочитать файл хранилища."; return false; }
    auto o = container::openHello(bytes);
    if (!finishOpen(o, err)) return false;
    mode_ = Mode::Hello;
    return true;
}
bool Application::unlockHello(QString* err) { return openHelloContainer(err); } // Hello-проверку выполняет UI до вызова
#endif
}
```

- [x] **Step 5: Диалоги `src/ui/SetupDialog` и `src/ui/UnlockDialog`**

`src/ui/SetupDialog.h`:

```cpp
#pragma once
#include <QDialog>
class QRadioButton;
class QLineEdit;
class SetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit SetupDialog(bool helloAvailable, QWidget* parent = nullptr);
    QByteArray masterPassword() const;   // UTF-8
    bool helloChosen() const;
private slots:
    void accept() override;
private:
    QRadioButton* pw_{}, * hello_{};
    QLineEdit* pw1_{}, * pw2_{};
};
```

`src/ui/SetupDialog.cpp`:

```cpp
#include "SetupDialog.h"
#include "../app/Application.h"
#include <QButtonGroup>
#include <QFormLayout>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>
SetupDialog::SetupDialog(bool helloAvailable, QWidget* parent) : QDialog(parent) {
    setWindowTitle("Создание хранилища TokenBucket");
    auto* lay = new QVBoxLayout(this);
    pw_ = new QRadioButton("Мастер-пароль (переносимо между компьютерами)");
    hello_ = new QRadioButton("Windows Hello (только этот компьютер и эта учётная запись)");
    hello_->setEnabled(helloAvailable);
    hello_->setToolTip("Ключ защищён DPAPI текущего пользователя. Проверка Hello — не криптографическая привязка: процесс от имени той же учётной записи потенциально может обойти UI-проверку.");
    auto* g = new QButtonGroup(this); g->addButton(pw_); g->addButton(hello_);
    pw_->setChecked(true);
    lay->addWidget(pw_); lay->addWidget(hello_);
    auto* form = new QFormLayout;
    pw1_ = new QLineEdit; pw1_->setEchoMode(QLineEdit::Password);
    pw2_ = new QLineEdit; pw2_->setEchoMode(QLineEdit::Password);
    form->addRow("Мастер-пароль (12–1024 байт):", pw1_);
    form->addRow("Повторите пароль:", pw2_);
    lay->addLayout(form);
    auto* ok = new QPushButton("Создать");
    auto* cancel = new QPushButton("Отмена");
    auto* btns = new QHBoxLayout; btns->addWidget(ok); btns->addWidget(cancel);
    lay->addLayout(btns);
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}
QByteArray SetupDialog::masterPassword() const { return pw1_->text().toUtf8(); }
bool SetupDialog::helloChosen() const { return hello_->isChecked(); }
void SetupDialog::accept() {
    if (hello_->isChecked()) { QDialog::accept(); return; }
    QByteArray pw = pw1_->text().toUtf8();
    if (!Application::validPassword(pw)) { QMessageBox::warning(this, "TokenBucket", "Пароль: минимум 12 и максимум 1024 байта."); return; }
    if (pw != pw2_->text().toUtf8()) { QMessageBox::warning(this, "TokenBucket", "Пароли не совпадают."); return; }
    QDialog::accept();
}
```

`src/ui/UnlockDialog.h/.cpp`:

```cpp
#pragma once
#include <QDialog>
class QLineEdit;
class QPushButton;
class UnlockDialog : public QDialog {
    Q_OBJECT
public:
    enum class Kind { Password, Hello };
    explicit UnlockDialog(Kind kind, QWidget* parent = nullptr);
    QByteArray password() const;
signals:
    void helloRequested();   // UI-слой запускает проверку Hello и при успехе вызывает accept()
private:
    QLineEdit* pw_{};
    QPushButton* helloBtn_{};
};
```

```cpp
#include "UnlockDialog.h"
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
UnlockDialog::UnlockDialog(Kind kind, QWidget* parent) : QDialog(parent) {
    setWindowTitle("Разблокировка TokenBucket");
    auto* lay = new QVBoxLayout(this);
    if (kind == Kind::Password) {
        pw_ = new QLineEdit; pw_->setEchoMode(QLineEdit::Password);
        lay->addWidget(pw_);
        auto* ok = new QPushButton("Разблокировать");
        auto* cancel = new QPushButton("Отмена");
        auto* b = new QHBoxLayout; b->addWidget(ok); b->addWidget(cancel); lay->addLayout(b);
        connect(ok, &QPushButton::clicked, this, &QDialog::accept);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(pw_, &QLineEdit::returnPressed, this, &QDialog::accept);
    } else {
        helloBtn_ = new QPushButton("Разблокировать через Windows Hello");
        auto* cancel = new QPushButton("Отмена");
        auto* b = new QHBoxLayout; b->addWidget(helloBtn_); b->addWidget(cancel); lay->addLayout(b);
        connect(helloBtn_, &QPushButton::clicked, this, &UnlockDialog::helloRequested);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    }
}
QByteArray UnlockDialog::password() const { return pw_ ? pw_->text().toUtf8() : QByteArray(); }
```

- [x] **Step 6: Заменить `src/main.cpp`**

```cpp
#include <QApplication>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#ifdef Q_OS_WIN
#include "platform/hello_win.h"
#endif
#include "app/Application.h"
#include "ui/SetupDialog.h"
#include "ui/UnlockDialog.h"
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("TokenBucket");
    QApplication::setOrganizationName("TokenBucket");
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QLockFile instanceLock(dir + "/instance.lock");
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock(0)) {
        QMessageBox::critical(nullptr, "TokenBucket", "Приложение уже запущено. Второй экземпляр закрыт, чтобы не менять хранилище параллельно.");
        return 1;
    }
    tb::Application core(dir);
    for (;;) {
        if (core.gate() == tb::Application::Gate::NeedSetup) {
            bool helloOk = false;
#ifdef Q_OS_WIN
            helloOk = tb::platform::hello::availability() == tb::platform::hello::Availability::Available;
#endif
            SetupDialog d(helloOk);
            if (d.exec() != QDialog::Accepted) return 0;
            QString err;
            bool ok = false;
#ifdef Q_OS_WIN
            if (d.helloChosen()) ok = core.createHelloVault(&err);
            else
#endif
            ok = core.createPasswordVault(d.masterPassword(), &err);
            if (!ok) { QMessageBox::critical(nullptr, "TokenBucket", err); continue; }
        } else if (core.gate() == tb::Application::Gate::NeedUnlock) {
            QFile f(core.storePathForTest());
            f.open(QIODevice::ReadOnly);
            const uint8_t mode = tb::container::readMode(f.readAll());
            UnlockDialog d(mode == tb::container::ModeHello ? UnlockDialog::Kind::Hello
                                                            : UnlockDialog::Kind::Password);
            if (d.exec() != QDialog::Accepted) return 0;
            QString err;
            bool ok = false;
            if (d.passwordMode()) ok = core.unlock(d.password(), &err); // см. правку ниже
            else ok = core.unlockHello(&err);
            if (!ok) { QMessageBox::critical(nullptr, "TokenBucket", err); continue; }
        } else break;
    }
    QMessageBox::information(nullptr, "TokenBucket", "Хранилище разблокировано. Интерфейс добавляется следующими задачами.");
    return app.exec();
}
```

Упрощение: в `UnlockDialog` добавить метод `bool passwordMode() const { return pw_ != nullptr; }`. Вызов `container::readMode` из main.cpp требует включить `vault/Container.h`; переменную `m` удалить (используется только `mode`).

- [x] **Step 7: Собрать и прогнать автотесты**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS. `helloVaultFlow` создаёт DPAPI-контейнер (не требует устройства Hello). Запускать приложение вручную на этом шаге не нужно.

- [x] **Step 8: Commit**

```bash
git add src/app/ src/platform/hello_win.h src/platform/hello_win.cpp src/ui/SetupDialog.h src/ui/SetupDialog.cpp src/ui/UnlockDialog.h src/ui/UnlockDialog.cpp src/main.cpp tests/test_app.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "Приложение: создание/разблокировка, режимы пароль и Hello, единственный экземпляр"
```

---

### Task 7: MainWindow — дерево, список, карточка, CRUD, поиск
Актуальные решения реализации (2026-10-19, приоритет над исходными эскизами ниже):

- MainWindow содержит дерево, список и сведения без секретных значений. Полноценный показ/копирование остаётся Задаче 8. Поиск глобальный; выбранные UUID сохраняются при обновлении. Проекты/группы переименовываются и переносятся через отдельные действия; удаление подтверждается, непустые узлы защищает модель.
- Все мутации проходят общий commit: снимок модели, проверка, save, видимый отказ и откат. При блокировке снимок очищается и не восстанавливается. busy запрещает изменение UI; блокировка закрывает редакторы, очищает поля и скрывает окно. Поддерживается повторный цикл разблокировки в main.
- EntryEditDialog сохраняет прежний приватный ключ, пока явно не выбран новый файл. Комментарий не используется как путь, публичный ключ без правок сохраняется исходными байтами. Тип существующей записи фиксирован: преобразование между типами не согласовано. Неактивные типовые секретные поля не сохраняются.
- Импорт SSH уже проверяет ограничение 1 МиБ и границы контейнера OpenSSH/PEM; чтение выполняется в рабочем потоке, байты не перекодируются. Проверка не является криптографической валидацией. В Задаче 12 использовать существующий импорт, не дублировать его.
- Штатные Qt-формы используют подписи, маскирование, прокрутку и адаптацию к доступному экрану. Секретные поля не обрезаются стандартным пределом QLineEdit в 32767 символов; проверяются лимиты UTF-8 модели. Save явно сообщает превышение 64 МиБ до шифрования.
- Проверки: Release build exit 0; CTest 7/7; TestUi покрывает диалоги всех типов, сохранение/открытие, перенос/удаление, отказ записи и блокировку. Дополнительно выполнен автоматизированный нативный Qt Windows-прогон при QT_SCALE_FACTOR=1 и 2; рендеры основного окна и редактора просмотрены.
- Ручной перезапуск отдельного TokenBucket.exe с вводом владельца ещё не выполнен; автоматизированный GUI-прогон использует временное хранилище и повторное открытие Application. Ручная проверка отдельного процесса остаётся в матрице приёмки и не подменяется автотестом.

**Files:**
- Create: `src/ui/MainWindow.h`, `src/ui/MainWindow.cpp`, `src/ui/EntryEditDialog.h`, `src/ui/EntryEditDialog.cpp`, `tests/test_ui.cpp`
- Modify: `src/main.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `tb::Application`, `tb::model::*`.
- Produces: `tb::ui::MainWindow { explicit MainWindow(tb::Application&, QWidget* parent=nullptr); void refreshAll(); }` — слоты CRUD доступны для тестов как public-методы: `void addProject(); void addGroup(); void addEntry(); void editSelected(); void deleteSelected();` и поле поиска `QLineEdit* search() const;`. `EntryEditDialog { EntryEditDialog(const VaultModel&, std::optional<model::Entry> existing, QWidget*); std::optional<model::Entry> entry() const; }`.

- [x] **Step 1: Failing-тест `tests/test_ui.cpp`**

```cpp
#include <QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include "../src/app/Application.h"
#include "../src/ui/MainWindow.h"
using namespace tb;
class TestUi : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
    void crudThroughModelAndRefresh() {
        Application app(dir_.path());
        QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        ui::MainWindow w(app);
        w.show();
        model::Id p{QUuid::createUuid()};
        QVERIFY(app.model()->addNode(p, model::Id(), "Проект UI").isEmpty());
        w.refreshAll();
        QCOMPARE(w.tree()->topLevelItemCount(), 1);
        QCOMPARE(w.tree()->topLevelItem(0)->text(0), QStringLiteral("Проект UI"));
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = p;
        e.type = model::EntryType::Credentials; e.title = "Логин"; e.login = "u"; e.password = "p";
        QVERIFY(app.model()->setEntry(e).isEmpty());
        w.refreshAll();
        w.tree()->setCurrentItem(w.tree()->topLevelItem(0));
        QCOMPARE(w.list()->count(), 1);
        QCOMPARE(w.list()->item(0)->text(), QStringLiteral("Логин"));
        // поиск
        w.search()->setText("логин");
        QCOMPARE(w.list()->count(), 1);
        w.search()->setText("нет-такой");
        QCOMPARE(w.list()->count(), 0);
        w.search()->clear();
    }
};
QTEST_MAIN(TestUi)
#include "test_ui.moc"
```

В `MainWindow` для тестов открыть `QTreeWidget* tree() const; QListWidget* list() const;` (public).

- [x] **Step 2: Подключить цель `test_ui` (Qt6::Widgets), запуск — FAIL**

- [x] **Step 3: Реализовать `src/ui/EntryEditDialog.h/.cpp`**

```cpp
#pragma once
#include <QDialog>
#include <optional>
#include "../model/VaultModel.h"
class QComboBox; class QLineEdit; class QPlainTextEdit; class QStackedWidget;
class EntryEditDialog : public QDialog {
    Q_OBJECT
public:
    EntryEditDialog(const tb::model::VaultModel& m, std::optional<tb::model::Entry> existing, QWidget* parent = nullptr);
    std::optional<tb::model::Entry> entry() const { return result_; }
private slots:
    void accept() override;
private:
    const tb::model::VaultModel& model_;
    std::optional<tb::model::Entry> result_;
    QComboBox* type_{}, * group_{};
    QLineEdit* title_{}, * url_{}, * login_{}, * token_{}, * sshPass_{};
    QPlainTextEdit* note_{}, * sshPublic_{}, * sshPrivatePath_{};
    QCheckBox* fav_{}, * rv_{};
    QStackedWidget* pages_{};
};
```

```cpp
#include "EntryEditDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
EntryEditDialog::EntryEditDialog(const tb::model::VaultModel& m, std::optional<tb::model::Entry> existing, QWidget* parent)
    : QDialog(parent), model_(m), result_(existing) {
    setWindowTitle(existing ? "Изменение записи" : "Новая запись");
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    type_ = new QComboBox; type_->addItems({"API-токен", "Учётные данные", "SSH-ключ"});
    group_ = new QComboBox;
    for (auto& n : m.nodes()) {
        QString prefix;
        tb::model::Id p = n.parent;
        while (!p.isNull()) { prefix += "    "; p = m.nodes().value(p).parent; }
        group_->addItem(prefix + n.name, n.id.toString());
        if (existing && existing->groupId == n.id) group_->setCurrentIndex(group_->count() - 1);
    }
    title_ = new QLineEdit; url_ = new QLineEdit; note_ = new QPlainTextEdit;
    form->addRow("Тип:", type_); form->addRow("Проект/группа:", group_);
    form->addRow("Название:", title_); form->addRow("Адрес (URL):", url_); form->addRow("Заметка:", note_);
    lay->addLayout(form);
    pages_ = new QStackedWidget;
    token_ = new QLineEdit; token_->setEchoMode(QLineEdit::Password);
    auto* p1 = new QWidget; auto* f1 = new QFormLayout(p1); f1->addRow("Токен:", token_);
    login_ = new QLineEdit; auto* pw = new QLineEdit; pw->setEchoMode(QLineEdit::Password); pw->setObjectName("pw");
    auto* p2 = new QWidget; auto* f2 = new QFormLayout(p2); f2->addRow("Логин:", login_); f2->addRow("Пароль:", pw);
    sshPass_ = new QLineEdit; sshPass_->setEchoMode(QLineEdit::Password);
    sshPublic_ = new QPlainTextEdit; sshPrivatePath_ = new QPlainTextEdit; sshPrivatePath_->setPlaceholderText("Путь к файлу приватного ключа");
    auto* p3 = new QWidget; auto* f3 = new QFormLayout(p3);
    f3->addRow("Файл приватного ключа:", sshPrivatePath_);
    auto* browse = new QPushButton("Выбрать файл...");
    f3->addRow(browse);
    f3->addRow("Публичный ключ (необязательно):", sshPublic_);
    f3->addRow("Парольная фраза ключа (необязательно):", sshPass_);
    pages_->addWidget(p1); pages_->addWidget(p2); pages_->addWidget(p3);
    lay->addWidget(pages_);
    auto* ok = new QPushButton("Сохранить"); auto* cancel = new QPushButton("Отмена");
    auto* b = new QHBoxLayout; b->addWidget(ok); b->addWidget(cancel); lay->addLayout(b);
    connect(type_, qOverload<int>(&QComboBox::currentIndexChanged), pages_, &QStackedWidget::setCurrentIndex);
    fav_ = new QCheckBox("Избранное");
    rv_ = new QCheckBox("Повторная проверка при каждом показе и копировании");
    form->addRow(fav_); form->addRow(rv_);
    connect(browse, &QPushButton::clicked, this, [this] {
        QString f = QFileDialog::getOpenFileName(this, "Приватный ключ SSH");
        if (!f.isEmpty()) sshPrivatePath_->setPlainText(f);
    });
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    if (existing) {
        type_->setCurrentIndex(int(existing->type));
        title_->setText(existing->title); url_->setText(existing->url); note_->setPlainText(existing->note);
        token_->setText(existing->token); login_->setText(existing->login);
        findChild<QLineEdit*>("pw")->setText(existing->password);
        sshPublic_->setPlainText(QString::fromUtf8(existing->sshPublic));
        sshPass_->setText(existing->sshPassphrase);
        sshPrivatePath_->setPlainText(existing->sshComment); // путь не храним; при изменении вводится заново
        fav_->setChecked(existing->favorite); rv_->setChecked(existing->reverify);
    }
    pages_->setCurrentIndex(type_->currentIndex());
}
void EntryEditDialog::accept() {
    tb::model::Entry e = result_.value_or(tb::model::Entry{});
    if (e.id.isNull()) e.id = QUuid::createUuid();
    e.type = tb::model::EntryType(type_->currentIndex());
    e.groupId = QUuid::fromString(group_->currentData().toString());
    e.title = title_->text(); e.url = url_->text(); e.note = note_->toPlainText();
    e.token = token_->text(); e.login = login_->text();
    e.password = findChild<QLineEdit*>("pw")->text();
    e.sshPublic = sshPublic_->toPlainText().toUtf8();
    e.sshPassphrase = sshPass_->text();
    if (e.title.isEmpty()) { QMessageBox::warning(this, "TokenBucket", "Название обязательно."); return; }
    if (e.type == tb::model::EntryType::SshKey) {
        QString path = sshPrivatePath_->toPlainText().trimmed();
        if (path.isEmpty() && e.sshPrivate.isEmpty()) { QMessageBox::warning(this, "TokenBucket", "Укажите файл приватного ключа."); return; }
        if (!path.isEmpty()) {
            QFileInfo fi(path);
            if (fi.size() > tb::model::VaultModel::MaxSecretBytes) { QMessageBox::warning(this, "TokenBucket", "Файл ключа превышает 1 МиБ."); return; }
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) { QMessageBox::warning(this, "TokenBucket", "Не удалось прочитать файл ключа."); return; }
            e.sshPrivate = f.readAll();  // исходные байты, без перекодировки
            e.sshComment.clear();
        }
    }
    e.modified = QDateTime::currentDateTimeUtc();
    if (e.created.isNull()) e.created = e.modified;
    e.favorite = fav_->isChecked(); e.reverify = rv_->isChecked();
    result_ = e;
    QDialog::accept();
}
```

- [x] **Step 4: Реализовать `src/ui/MainWindow.h/.cpp`**

`src/ui/MainWindow.h`:

```cpp
#pragma once
#include <QMainWindow>
#include "../app/Application.h"
class QTreeWidget; class QListWidget; class QLineEdit; class QStackedWidget;
namespace tb::ui {
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Application& app, QWidget* parent = nullptr);
    void refreshAll();
    QTreeWidget* tree() const { return tree_; }
    QListWidget* list() const { return list_; }
    QLineEdit* search() const { return search_; }
public slots:
    void addProject();
    void addGroup();
    void addEntry();
    void editSelected();
    void deleteSelected();
private:
    model::Id selectedNode() const;
    model::Id selectedEntry() const;
    void rebuildTree();
    void refillList();
    Application& app_;
    QTreeWidget* tree_{};
    QListWidget* list_{};
    QLineEdit* search_{};
    QStackedWidget* right_{};
};
}
```

`src/ui/MainWindow.cpp`:

```cpp
#include "MainWindow.h"
#include "../model/VaultModel.h"
#include "EntryCard.h"
#include "EntryEditDialog.h"
#include <QHBoxLayout>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace tb::ui {
MainWindow::MainWindow(Application& app, QWidget* parent) : QMainWindow(parent), app_(app) {
    setWindowTitle("TokenBucket");
    auto* file = menuBar()->addMenu("Хранилище");
    file->addAction("Новый проект", this, &MainWindow::addProject);
    file->addAction("Новая группа", this, &MainWindow::addGroup);
    file->addAction("Новая запись", this, &MainWindow::addEntry);
    file->addSeparator();
    file->addAction("Изменить...", this, &MainWindow::editSelected, QKeySequence("F2"));
    file->addAction("Удалить", this, &MainWindow::deleteSelected, QKeySequence::Delete);
    auto* central = new QWidget(this);
    auto* top = new QVBoxLayout(central);
    search_ = new QLineEdit; search_->setPlaceholderText("Поиск по названиям, логинам, проектам и заметкам");
    top->addWidget(search_);
    auto* split = new QSplitter;
    tree_ = new QTreeWidget; tree_->setHeaderLabel("Проекты и группы");
    list_ = new QListWidget; list_->setMinimumWidth(220);
    right_ = new QStackedWidget;
    right_->addWidget(new QLabel("Выберите запись")); // placeholder до Задачи 8
    split->addWidget(tree_); split->addWidget(list_); split->addWidget(right_);
    top->addWidget(split);
    setCentralWidget(central);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] { refillList(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { refillList(); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { editSelected(); });
    refreshAll();
}
void MainWindow::rebuildTree() {
    tree_->clear();
    std::function<void(model::Id, QTreeWidgetItem*)> add = [&](model::Id parent, QTreeWidgetItem* item) {
        for (auto id : app_.model()->childNodes(parent)) {
            auto* ch = new QTreeWidgetItem(QStringList{app_.model()->nodes().value(id).name});
            ch->setData(0, Qt::UserRole, id.toString());
            (item ? item : tree_->invisibleRootItem())->addChild(ch);
            add(id, ch);
        }
    };
    for (auto id : app_.model()->childNodes(model::Id())) {
        auto* it = new QTreeWidgetItem(QStringList{app_.model()->nodes().value(id).name});
        it->setData(0, Qt::UserRole, id.toString());
        tree_->addTopLevelItem(it);
        add(id, it);
    }
}
void MainWindow::refillList() {
    list_->clear();
    const QString q = search_->text();
    QList<model::Id> ids = q.isEmpty() ? app_.model()->entriesInGroup(selectedNode())
                                       : app_.model()->search(q);
    for (auto id : ids) {
        auto* e = app_.model()->entry(id);
        auto* it = new QListWidgetItem(e->favorite ? "★ " + e->title : e->title);
        it->setData(Qt::UserRole, id.toString());
        list_->addItem(it);
    }
}
model::Id MainWindow::selectedNode() const {
    auto* i = tree_->currentItem();
    return i ? QUuid::fromString(i->data(0, Qt::UserRole).toString()) : model::Id{};
}
model::Id MainWindow::selectedEntry() const {
    auto* i = list_->currentItem();
    return i ? QUuid::fromString(i->data(Qt::UserRole).toString()) : model::Id{};
}
void MainWindow::refreshAll() { rebuildTree(); refillList(); }
void MainWindow::addProject() {
    bool ok = false;
    QString name = QInputDialog::getText(this, "TokenBucket", "Имя проекта:", QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    QString e = app_.model()->addNode(QUuid::createUuid(), model::Id(), name);
    if (!e.isEmpty()) QMessageBox::warning(this, "TokenBucket", e);
    refreshAll(); QString s; app_.save(&s);
}
void MainWindow::addGroup() {
    model::Id parent = selectedNode();
    if (parent.isNull()) { QMessageBox::information(this, "TokenBucket", "Выберите проект или группу."); return; }
    bool ok = false;
    QString name = QInputDialog::getText(this, "TokenBucket", "Имя группы:", QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    QString e = app_.model()->addNode(QUuid::createUuid(), parent, name);
    if (!e.isEmpty()) QMessageBox::warning(this, "TokenBucket", e);
    refreshAll(); QString s; app_.save(&s);
}
void MainWindow::addEntry() {
    model::Id parent = selectedNode();
    if (parent.isNull()) { QMessageBox::information(this, "TokenBucket", "Сначала выберите проект или группу."); return; }
    EntryEditDialog d(*app_.model(), std::nullopt, this);
    if (d.exec() == QDialog::Accepted && d.entry()) {
        QString e = app_.model()->setEntry(*d.entry());
        if (!e.isEmpty()) QMessageBox::warning(this, "TokenBucket", e);
        refreshAll(); QString s; app_.save(&s);
    }
}
void MainWindow::editSelected() {
    model::Id id = selectedEntry();
    const auto* e = app_.model()->entry(id);
    if (!e) { QMessageBox::information(this, "TokenBucket", "Выберите запись в списке."); return; }
    EntryEditDialog d(*app_.model(), *e, this);
    if (d.exec() == QDialog::Accepted && d.entry()) {
        QString err = app_.model()->setEntry(*d.entry());
        if (!err.isEmpty()) QMessageBox::warning(this, "TokenBucket", err);
        refreshAll(); QString s; app_.save(&s);
    }
}
void MainWindow::deleteSelected() {
    model::Id id = selectedEntry();
    if (!id.isNull()) {
        if (QMessageBox::question(this, "TokenBucket", "Удалить запись?") == QMessageBox::Yes) {
            app_.model()->removeEntry(id); refreshAll(); QString s; app_.save(&s);
        }
        return;
    }
    model::Id n = selectedNode();
    if (!n.isNull()) {
        QString e = app_.model()->removeNode(n);
        if (!e.isEmpty()) QMessageBox::warning(this, "TokenBucket", e);
        refreshAll(); QString s; app_.save(&s);
    }
}
}
```

Заголовочный `EntryCard.h` здесь ещё не создан — placeholder правой панели оставить `QLabel`, а `#include "EntryCard.h"` убрать до Задачи 8.

Узкое окно (спека §2: «при недостатке места карточка открывается отдельно»): правая панель допускает сворачивание, а при ширине окна меньше 720 px выбор записи открывает карточку отдельным окном. Метод `showCard` и член `card_` задействуют карточку, которая появится в Задаче 8 — в этой задаче реализовать только `resizeEvent` и `narrow_`, а `showCard` добавить в Задаче 8 вместе с карточкой. В `MainWindow` добавить член `bool narrow_ = false;`, переопределение `resizeEvent` и окно карточки:

```cpp
void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    const bool narrow = width() < 720;
    if (narrow == narrow_) return;
    narrow_ = narrow;
    right_->setVisible(!narrow);
}
// вызывается из обработчика выбора записи в списке:
void MainWindow::showCard(model::Id id) {
    if (!narrow_) { card_->showEntry(id); return; }
    QDialog dlg(this);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(card_);            // перемещает карточку в диалог
    card_->showEntry(id);
    dlg.setWindowTitle("TokenBucket — запись");
    dlg.exec();
    right_->layout()->addWidget(card_); // вернуть карточку на место
}
```

- [x] **Step 5: `src/main.cpp` — после разблокировки показывать MainWindow**

Заменить финальный `QMessageBox::information(...)` на:

```cpp
    tb::ui::MainWindow w(core);
    w.show();
    return app.exec();
```

и добавить `#include "ui/MainWindow.h"`. Из `MainWindow` подключить `app.stateChanged` → при `NeedSetup/NeedUnlock` показывать соответствующий диалог (вынести цикл разблокировки в лямбду `relock`, вызвать её по сигналу; минимально: при `stateChanged` в незелёное состояние — скрыть окно и запустить тот же цикл; при Ready — `w.refreshAll(); w.show();`).

- [ ] **Step 6: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS.

Ручная проверка (не автотест): запустить `build/TokenBucket.exe`, создать хранилище с паролем, создать проект/группу/записи всех трёх типов, перезапустить приложение и убедиться, что данные читаются. Результат записать в `docs/build/manual-checks.md` одной строкой на проверку.

- [x] **Step 7: Commit**

```bash
git add src/ui/MainWindow.h src/ui/MainWindow.cpp src/ui/EntryEditDialog.h src/ui/EntryEditDialog.cpp src/main.cpp tests/test_ui.cpp CMakeLists.txt tests/CMakeLists.txt docs/build/manual-checks.md
git commit -m "Интерфейс: дерево/список/карточка, CRUD и поиск"
```

---

### Task 8: EntryCard — показ/скрытие 15 с, повторная проверка, кнопки копирования

**Files:**
- Create: `src/ui/EntryCard.h`, `src/ui/EntryCard.cpp`, `src/ui/VerifyDialog.h`, `src/ui/VerifyDialog.cpp`
- Modify: `src/ui/MainWindow.cpp` (правая панель), `src/app/Application.h/.cpp` (checkPassword), `tests/test_ui.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Application`, модель, `ClipboardGuard`.
- Produces: `tb::ui::EntryCard { EntryCard(Application&, QWidget* parent); void showEntry(model::Id id); QString secretText() const; QPushButton* revealButton() const; void hideNowForTest(); signals: void copyRequested(const QString& text); }`; `tb::ui::VerifyDialog { static bool verify(Application&, QWidget* parent); }` — в режиме Password запрашивает мастер-пароль и сверяет через новый метод `Application::checkPassword(const QByteArray&, QString* err)` (проверяет только оболочку ключа, не открывая модель повторно); в режиме Hello — `platform::hello::request`. Копирование из карточки идёт через сигнал `copyRequested`; `MainWindow` временно соединяет его с прямым `QClipboard::setText` (Задача 9 заменяет на `ClipboardGuard`).

Правило: показ секрета выполняется только после `VerifyDialog::verify`, если `entry->reverify == true`; каждое копирование защищённой записи — новая проверка.

- [x] **Step 1: Failing-тест (добавить в `tests/test_ui.cpp`)**

```cpp
    void cardHidesSecretsUntilReveal() {
        Application app(dir_.path());
        QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        ui::MainWindow w(app); w.show();
        model::Id p{QUuid::createUuid()};
        QVERIFY(app.model()->addNode(p, model::Id(), "P").isEmpty());
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = p;
        e.type = model::EntryType::ApiToken; e.title = "Т"; e.token = "tok-12345"; e.reverify = false;
        QVERIFY(app.model()->setEntry(e).isEmpty());
        w.refreshAll();
        w.list()->setCurrentRow(0);
        auto* card = w.card();
        QVERIFY(card->secretText().contains("••••"));
        QVERIFY(!card->secretText().contains("tok-12345"));
        QTest::mouseClick(card->revealButton(), Qt::LeftButton);
        QVERIFY(card->secretText().contains("tok-12345"));
        // автоскрытие через 15 с эмуулируется коротким таймером теста:
        card->hideNowForTest();
        QVERIFY(!card->secretText().contains("tok-12345"));
    }
```

Открыть в `MainWindow`: `EntryCard* card() const;`. В `EntryCard`: `QString secretText() const; QPushButton* revealButton() const; void hideNowForTest();`.

- [x] **Step 2: Реализовать `VerifyDialog` и `Application::checkPassword`**

`src/ui/VerifyDialog.h/.cpp`:

```cpp
#pragma once
#include <QWidget>
namespace tb { class Application; }
namespace tb::ui {
class VerifyDialog {
public:
    static bool verify(Application& app, QWidget* parent); // true — проверка пройдена
};
}
```

```cpp
#include "VerifyDialog.h"
#include "../app/Application.h"
#ifdef Q_OS_WIN
#include "../platform/hello_win.h"
#endif
#include <QInputDialog>
#include <QMessageBox>
namespace tb::ui {
bool VerifyDialog::verify(Application& app, QWidget* parent) {
    if (app.mode() == Application::Mode::Hello) {
#ifdef Q_OS_WIN
        return tb::platform::hello::request(parent ? parent->winId() : nullptr, "Подтвердите доступ к секрету") == tb::platform::hello::Result::Verified;
#else
        return false;
#endif
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool ok = false;
        QString pw = QInputDialog::getText(parent, "Повторная проверка", "Мастер-пароль:", QLineEdit::Password, QString(), &ok);
        if (!ok) return false;
        QString err;
        if (app.checkPassword(pw.toUtf8(), &err)) return true;
        QMessageBox::warning(parent, "TokenBucket", err.isEmpty() ? "Неверный пароль." : err);
    }
    return false;
}
}
```

В `Application` добавить (и в заголовок):

```cpp
bool Application::checkPassword(const QByteArray& pw, QString* err) {
    QByteArray bytes;
    if (store_.loadBytes(store_.mainPath(), &bytes) != store::VaultStore::Load::Ok) { if (err) *err = "Не удалось прочитать файл хранилища."; return false; }
    return container::openPassword(bytes, pw).status == container::Status::Ok;
}
```

- [x] **Step 3: Реализовать `src/ui/EntryCard.h/.cpp`**

```cpp
#pragma once
#include <QFrame>
#include <QPushButton>
#include "../app/Application.h"
namespace tb::ui {
class EntryCard : public QFrame {
    Q_OBJECT
public:
    EntryCard(Application& app, QWidget* parent = nullptr);
    void showEntry(model::Id id);
    QString secretText() const;
    QPushButton* revealButton() const { return reveal_; }
    void hideNowForTest();
signals:
    void copyRequested(const QString& text);
private:
    void revealField(QLabel* value, const QString& secret);
    void hideField(QLabel* value);
    bool maybeVerify(const model::Entry& e);
    Application& app_;
    model::Id id_;
    QLabel* title_{}, * value_{}, * loginValue_{}, * note_{}, * url_{};
    QPushButton* reveal_{}, * copySecret_{}, * copyLogin_{}, * copyPassword_{}, * copySshPass_{}, * sshToFile_{};
    QTimer* hideTimer_{};
};
}
```

```cpp
#include "EntryCard.h"
#include "../model/VaultModel.h"
#include "VerifyDialog.h"
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
EntryCard::EntryCard(Application& app, QWidget* parent) : QFrame(parent), app_(app) {
    auto* lay = new QVBoxLayout(this);
    title_ = new QLabel; title_->setStyleSheet("font-weight: bold;");
    url_ = new QLabel; note_ = new QLabel; note_->setWordWrap(true);
    value_ = new QLabel; loginValue_ = new QLabel;
    lay->addWidget(title_); lay->addWidget(url_); lay->addWidget(loginValue_); lay->addWidget(value_); lay->addWidget(note_);
    auto* btns = new QHBoxLayout;
    reveal_ = new QPushButton("Показать"); copySecret_ = new QPushButton("Копировать");
    copyLogin_ = new QPushButton("Копировать логин"); copyPassword_ = new QPushButton("Копировать пароль");
    copySshPass_ = new QPushButton("Копировать фразу"); sshToFile_ = new QPushButton("Сохранить ключ в файл...");
    btns->addWidget(reveal_); btns->addWidget(copySecret_); btns->addWidget(copyLogin_);
    btns->addWidget(copyPassword_); btns->addWidget(copySshPass_); btns->addWidget(sshToFile_);
    lay->addLayout(btns);
    hideTimer_ = new QTimer(this); hideTimer_->setSingleShot(true); hideTimer_->setInterval(15000);
    connect(hideTimer_, &QTimer::timeout, this, [this] { hideField(value_); hideField(loginValue_); });
    connect(reveal_, &QPushButton::clicked, this, [this] {
        const auto* e = app_.model()->entry(id_); if (!e) return;
        if (!maybeVerify(*e)) return;
        revealField(value_, e->type == model::EntryType::ApiToken ? e->token : e->password);
        if (e->type == model::EntryType::Credentials) revealField(loginValue_, e->login);
        hideTimer_->start();
    });
    auto copy = [this](const QString& s) { const auto* e = app_.model()->entry(id_);
        if (!e || s.isEmpty()) return; if (!maybeVerify(*e)) return; emit copyRequested(s); };
    connect(copySecret_, &QPushButton::clicked, this, [copy, this] { const auto* e = app_.model()->entry(id_); if (e) copy(e->type == model::EntryType::SshKey ? QString() : (e->type == model::EntryType::ApiToken ? e->token : e->password)); });
    connect(copyLogin_, &QPushButton::clicked, this, [copy, this] { const auto* e = app_.model()->entry(id_); if (e) copy(e->login); });
    connect(copyPassword_, &QPushButton::clicked, this, [copy, this] { const auto* e = app_.model()->entry(id_); if (e) copy(e->password); });
    connect(copySshPass_, &QPushButton::clicked, this, [copy, this] { const auto* e = app_.model()->entry(id_); if (e) copy(e->sshPassphrase); });
    sshToFile_->hide(); // включается в Задаче 12
}
bool EntryCard::maybeVerify(const model::Entry& e) {
    if (!e.reverify) return true;
    return VerifyDialog::verify(app_, this);
}
void EntryCard::revealField(QLabel* l, const QString& s) { l->setText(s.isEmpty() ? "(пусто)" : s); }
void EntryCard::hideField(QLabel* l) { l->setText("••••••••"); }
void EntryCard::showEntry(model::Id id) {
    id_ = id;
    const auto* e = app_.model()->entry(id);
    if (!e) { hideNowForTest(); return; }
    title_->setText(e->title); url_->setText(e->url); note_->setText(e->note);
    hideField(value_); hideField(loginValue_);
    const bool ssh = e->type == model::EntryType::SshKey;
    copySecret_->setVisible(!ssh); copyLogin_->setVisible(e->type == model::EntryType::Credentials);
    copyPassword_->setVisible(e->type == model::EntryType::Credentials);
    copySshPass_->setVisible(ssh && !e->sshPassphrase.isEmpty());
    reveal_->setVisible(!ssh); // приватный SSH-ключ не показывается текстом, только файл (Задача 12)
}
QString EntryCard::secretText() const { return value_->text(); }
void EntryCard::hideNowForTest() { hideTimer_->stop(); hideField(value_); hideField(loginValue_); id_ = model::Id{}; title_->clear(); url_->clear(); note_->clear(); }
```

В `MainWindow`: заменить placeholder правой панели на `EntryCard`, добавить `card()` и `#include "EntryCard.h"`; при выборе записи в списке вызывать `card_->showEntry(id)`; подписать `connect(card_, &EntryCard::copyRequested, ...)` на временный прямой `QClipboard setText` (Задача 9 заменит на `ClipboardGuard`). Также скрытие при потере фокуса: `focusOutEvent` окна вызывает `card_->hideNowForTest()` — подключить через `installEventFilter` в `MainWindow` на себя: при `QEvent::WindowDeactivate` вызвать `card_->hideNowForTest()`.

- [x] **Step 4: Собрать и прогнать (test_ui с новым кейсом)**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS. Тест не требует ручного подтверждения: `reverify=false`, проверка не вызывается.

- [x] **Step 5: Commit**

```bash
git add src/ui/EntryCard.h src/ui/EntryCard.cpp src/ui/VerifyDialog.h src/ui/VerifyDialog.cpp src/app/Application.h src/app/Application.cpp src/ui/MainWindow.h src/ui/MainWindow.cpp tests/test_ui.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "Карточка: скрытие по умолчанию, показ 15 с, повторная проверка"
```

---

### Task 9: ClipboardGuard — 20 секунд, владение, занятость, история Windows, без PRIMARY

**Files:**
- Create: `src/platform/ClipboardGuard.h`, `src/platform/ClipboardGuard.cpp`, `tests/test_clipboard.cpp`
- Modify: `src/ui/MainWindow.cpp` (копирование через guard), `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: QApplication (QClipboard / WinAPI).
- Produces: `tb::ClipboardGuard : QObject { explicit ClipboardGuard(QObject* parent = nullptr); bool copySecret(const QString& text, QString* err); void clearIfOurs(); signals: void cleared(); }`. Поведение: очистка через 20 с после копирования; очистка только если буфер всё ещё наш (Windows: `GetClipboardSequenceNumber()` не изменился; иначе: `QClipboard::ownsClipboard()`); при занятом буфере Windows (OpenClipboard fail) — три попытки с паузой 50 мс, затем ошибка; Linux — только `QClipboard::Clipboard`, никогда `QClipboard::Selection`.

- [x] **Step 1: Failing-тест `tests/test_clipboard.cpp`**

```cpp
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include "../src/platform/ClipboardGuard.h"
using namespace tb;
class TestClipboard : public QObject {
    Q_OBJECT
private slots:
    void copiesAndClearsAfterTimer() {
        ClipboardGuard g;
        QString err;
        QVERIFY(g.copySecret("секрет-123", &err));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("секрет-123"));
        g.triggerClearNowForTest();  // вместо ожидания 20 с
        QVERIFY(QApplication::clipboard()->text().isEmpty());
    }
    void foreignContentNotCleared() {
        ClipboardGuard g;
        QString err;
        QVERIFY(g.copySecret("наш-секрет", &err));
        QApplication::clipboard()->setText("чужое-новее");  // конкурентная запись
        g.triggerClearNowForTest();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("чужое-новее"));
    }
    void emptyNotCopied() {
        ClipboardGuard g; QString err;
        QVERIFY(!g.copySecret("", &err));
    }
    void secondCopyResetsTimer() {
        ClipboardGuard g; QString err;
        QVERIFY(g.copySecret("первый", &err));
        QVERIFY(g.copySecret("второй", &err));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("второй"));
        g.triggerClearNowForTest();
        QVERIFY(QApplication::clipboard()->text().isEmpty());
    }
};
QTEST_MAIN(TestClipboard)
#include "test_clipboard.moc"
```

Тесты используют `triggerClearNowForTest()` — публичный метод, выполняющий логику очистки немедленно (тот же код, что и по таймеру).

- [x] **Step 2: Реализовать `src/platform/ClipboardGuard.h/.cpp`**

`src/platform/ClipboardGuard.h`:

```cpp
#pragma once
#include <QObject>
namespace tb {
class ClipboardGuard : public QObject {
    Q_OBJECT
public:
    explicit ClipboardGuard(QObject* parent = nullptr);
    bool copySecret(const QString& text, QString* err);
    void clearIfOurs();
    void triggerClearNowForTest() { clearIfOurs(); }
signals:
    void cleared();
private:
    class QTimer* timer_ = nullptr;
#ifdef Q_OS_WIN
    unsigned long seq_ = 0;   // GetClipboardSequenceNumber после нашей публикации
#endif
};
}
```

`src/platform/ClipboardGuard.cpp`:

```cpp
#include "ClipboardGuard.h"
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace tb {
static constexpr int kClearMs = 20000;
ClipboardGuard::ClipboardGuard(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &ClipboardGuard::clearIfOurs);
}
bool ClipboardGuard::copySecret(const QString& text, QString* err) {
    if (text.isEmpty()) { if (err) *err = "Пустое значение не копируется."; return false; }
#ifdef Q_OS_WIN
    // Публикуем сами, чтобы приложить форматы исключения из истории/синхронизации:
    // CanIncludeInClipboardHistory=0, ExcludeClipboardContentFromMonitorProcessing=0
    // (см. https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-formats)
    const HWND hwnd = reinterpret_cast<HWND>(qApp->focusWindow() ? qApp->focusWindow()->winId() : nullptr);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
            auto* p = static_cast<wchar_t*>(GlobalLock(g));
            memcpy(p, text.utf16(), size_t(text.size()) * sizeof(wchar_t));
            p[text.size()] = 0;
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
            const UINT histFmt = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
            const UINT exclFmt = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
            DWORD zero = 0;
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            memcpy(GlobalLock(h), &zero, sizeof(DWORD)); GlobalUnlock(h);
            SetClipboardData(histFmt, h);
            HGLOBAL e = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            memcpy(GlobalLock(e), &zero, sizeof(DWORD)); GlobalUnlock(e);
            SetClipboardData(exclFmt, e);
            CloseClipboard();
            seq_ = GetClipboardSequenceNumber();
            timer_->start(kClearMs);
            return true;
        }
        Sleep(50); // буфер занят другим процессом — ограниченная попытка
    }
    if (err) *err = "Буфер обмена занят другим приложением.";
    return false;
#else
    QApplication::clipboard()->setText(text, QClipboard::Clipboard); // Selection не заполняем
    timer_->start(kClearMs);
    return true;
#endif
}
void ClipboardGuard::clearIfOurs() {
    timer_->stop();
#ifdef Q_OS_WIN
    if (seq_ == 0) return;
    if (GetClipboardSequenceNumber() != seq_) return; // чужое более новое содержимое не трогаем
    const HWND hwnd = reinterpret_cast<HWND>(qApp->focusWindow() ? qApp->focusWindow()->winId() : nullptr);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (OpenClipboard(hwnd)) { EmptyClipboard(); CloseClipboard(); seq_ = 0; emit cleared(); return; }
        Sleep(50);
    }
#else
    if (!QApplication::clipboard()->ownsClipboard()) return;
    QApplication::clipboard()->setText(QString(), QClipboard::Clipboard);
#endif
    emit cleared();
}
}
```

Включить `#include <QWindow>` при необходимости. На Windows тест `foreignContentNotCleared` работает, т.к. `QClipboard::setText` тоже меняет sequence number.

- [x] **Step 3: Подключить в MainWindow**

`MainWindow` создаёт `ClipboardGuard guard_;` (член), `copyRequested` → `guard_.copySecret(text, &err)` с `QMessageBox` при отказе. Удалить временный прямой `setText` из Задачи 8.

- [x] **Step 4: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS (5 тестов буфера; на Windows реально работают sequence numbers).

- [x] **Step 5: Commit**

```bash
git add src/platform/ClipboardGuard.h src/platform/ClipboardGuard.cpp src/ui/MainWindow.h src/ui/MainWindow.cpp tests/test_clipboard.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "Буфер: 20 секунд, владение, занятость, исключение из истории Windows"
```

---

### Task 10: Автоблокировка — 5 минут, сеанс, сон, ручная; полная очистка

**Files:**
- Create: `src/app/IdleTimer.h`, `src/app/IdleTimer.cpp`, `src/platform/SessionWatcher.h`, `src/platform/sessionlock_win.cpp`, `src/platform/sessionlock_linux.cpp`, `tests/test_lock.cpp`
- Modify: `src/main.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Application::lock()`, `ClipboardGuard::clearIfOurs()`.
- Produces: `tb::IdleTimer : QObject { explicit IdleTimer(int timeoutMs, QObject* parent=nullptr); void poke(); signals: void idleTimeout(); }` — сбрасывается любым событием ввода через eventFilter на `qApp`; `tb::platform::SessionWatcher : QObject { signals: void lockNow(); }` — Windows: `WM_WTSSESSION_CHANGE/WTS_SESSION_LOCK` и `WM_POWERBROADCAST/PBT_APMSUSPEND` через `QAbstractNativeEventFilter` и собственное скрытое окно; Linux: сигналы DBus `org.freedesktop.ScreenSaver.ActiveChanged(true)` (session) и `org.freedesktop.login1.Manager.PrepareSleep(true)` (system).

- [x] **Step 1: Failing-тест `tests/test_lock.cpp`**

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include <QApplication>
#include "../src/app/Application.h"
#include "../src/app/IdleTimer.h"
#include "../src/platform/ClipboardGuard.h"
using namespace tb;
class TestLock : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
    void lockClearsEverything() {
        Application app(dir_.path());
        QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        QVERIFY(app.model()->addNode(QUuid::createUuid(), model::Id(), "P").isEmpty());
        ClipboardGuard g;
        QVERIFY(g.copySecret("секрет", &err));
        const auto gen = app.generation();
        app.lock();
        QCOMPARE(app.generation(), gen + 1);         // отменённые async-операции сверяют generation
        QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
        QVERIFY(app.model()->nodes().isEmpty());
        g.clearIfOurs();
        QVERIFY(QApplication::clipboard()->text().isEmpty());
    }
    void idleTimerFiresAfterInactivity() {
        IdleTimer t(200);   // в тесте — 200 мс вместо 5 минут
        QSignalSpy spy(&t, &IdleTimer::idleTimeout);
        t.poke();
        QTest::qWait(50);  // активность сбрасывает
        QCOMPARE(spy.count(), 0);
        QTest::qWait(300);
        QCOMPARE(spy.count(), 1);
    }
};
QTEST_MAIN(TestLock)
#include "test_lock.moc"
```

- [x] **Step 2: Реализовать `IdleTimer` и `SessionWatcher`**

`src/app/IdleTimer.h/.cpp`:

```cpp
#pragma once
#include <QObject>
class QTimer;
namespace tb {
class IdleTimer : public QObject {
    Q_OBJECT
public:
    explicit IdleTimer(int timeoutMs, QObject* parent = nullptr);
    ~IdleTimer() override;
    void poke();
    bool eventFilter(QObject* watched, QEvent* event) override;
signals:
    void idleTimeout();
private:
    QTimer* t_;
};
}
```

```cpp
#include "IdleTimer.h"
#include <QApplication>
#include <QEvent>
#include <QTimer>
namespace tb {
IdleTimer::IdleTimer(int timeoutMs, QObject* parent) : QObject(parent) {
    t_ = new QTimer(this);
    t_->setSingleShot(true);
    t_->setInterval(timeoutMs);
    connect(t_, &QTimer::timeout, this, &IdleTimer::idleTimeout);
    qApp->installEventFilter(this);
    poke();
}
IdleTimer::~IdleTimer() { qApp->removeEventFilter(this); }
void IdleTimer::poke() { t_->start(); }
bool IdleTimer::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
        poke();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
}
```

`src/platform/SessionWatcher.h`:

```cpp
#pragma once
#include <QObject>
namespace tb::platform {
class SessionWatcher : public QObject {
    Q_OBJECT
public:
    explicit SessionWatcher(QObject* parent = nullptr);
signals:
    void lockNow();
};
}
```

`src/platform/sessionlock_win.cpp`:

```cpp
#include "SessionWatcher.h"
#include <QAbstractNativeEventFilter>
#include <QWidget>
#include <windows.h>
#include <wtsapi.h>
namespace tb::platform {
namespace {
struct Filter : QAbstractNativeEventFilter {
    SessionWatcher* w;
    bool nativeEventFilter(const QByteArray& t, void* m, long*) override {
        if (t != "windows_generic_MSG") return false;
        auto* msg = static_cast<MSG*>(m);
        if (msg->message == WM_WTSSESSION_CHANGE && msg->wParam == WTS_SESSION_LOCK) emit w->lockNow();
        if (msg->message == WM_POWERBROADCAST && msg->wParam == PBT_APMSUSPEND) emit w->lockNow();
        return false;
    }
};
}
SessionWatcher::SessionWatcher(QObject* parent) : QObject(parent) {
    static QWidget hidden;       // принимает сообщения сеанса
    static bool inited = false;
    if (!inited) {
        inited = true;
        WTSRegisterSessionNotification(reinterpret_cast<HWND>(hidden.winId()), NOTIFY_FOR_THIS_SESSION);
        static Filter f{this};
        qApp->installNativeEventFilter(&f);
    }
}
}
```

`src/platform/sessionlock_linux.cpp`:

```cpp
#include "SessionWatcher.h"
#include <QDBusConnection>
#include <QDBusError>
SessionWatcher::SessionWatcher(QObject* parent) : QObject(parent) {
    // org.freedesktop.ScreenSaver: блокировка сеанса GNOME; login1: уход в сон
    QDBusConnection::sessionBus().connect("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
        "org.freedesktop.ScreenSaver", "ActiveChanged", this, SLOT(onActiveChanged(bool)));
    QDBusConnection::systemBus().connect("org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "PrepareSleep", this, SLOT(onPrepareSleep(bool)));
}
```

Добавить приватные слоты `onActiveChanged(bool)`/`onPrepareSleep(bool)` в объявление `SessionWatcher` (в заголовке, под `#ifndef Q_OS_WIN` не нужно — объявить оба слота безусловно, на Windows они не подключаются).

- [x] **Step 3: Связать в `src/main.cpp`**

```cpp
    tb::IdleTimer idle(5 * 60 * 1000);
    tb::platform::SessionWatcher session;
    QObject::connect(&idle, &tb::IdleTimer::idleTimeout, &core, &tb::Application::lock);
    QObject::connect(&session, &tb::platform::SessionWatcher::lockNow, &core, &tb::Application::lock);
```

В меню MainWindow добавить действие «Заблокировать» (Ctrl+L) → `app_.lock()`.

- [x] **Step 4: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS (на Windows собирается `sessionlock_win.cpp`; linux-файл в сборке не участвует — `if(WIN32)...else()` уже в CMake из Задачи 5, дополнить его файлами sessionlock).

- [x] **Step 5: Commit**

```bash
git add src/app/IdleTimer.h src/app/IdleTimer.cpp src/platform/SessionWatcher.h src/platform/sessionlock_win.cpp src/platform/sessionlock_linux.cpp src/main.cpp src/ui/MainWindow.h src/ui/MainWindow.cpp tests/test_lock.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "Автоблокировка: простой 5 минут, сеанс и сон, ручная блокировка"
```

---

### Task 11: Смена режима разблокировки; защищённый экспорт и импорт

**Files:**
- Create: `src/ui/ExportDialog.h`, `src/ui/ExportDialog.cpp`
- Modify: `src/app/Application.h/.cpp` (switch-методы), `src/ui/MainWindow.cpp` (меню «Настройки»/«Экспорт»/«Импорт»), `tests/test_app.cpp`

**Interfaces:**
- Consumes: контейнер, store, VerifyDialog.
- Produces: `Application::switchToPassword(const QByteArray& newPw, QString* err)`, `Application::switchToHello(QString* err)` (Windows): сначала `packX` новой моделью во временный контейнер, затем `store_.save` (атомарная замена), успех → смена mode/dataKey; любая ошибка — старое состояние без изменений. `ExportDialog { ExportDialog(QWidget*); QByteArray password() const; }` (ввод пароля экспорта дважды, те же правила 12–1024).

- [x] **Step 1: Failing-тесты (добавить в `tests/test_app.cpp`)**

```cpp
    void exportImportRoundtrip() {
        Application src(dir_.path());
        QString err; QVERIFY(src.createPasswordVault("пароль-источника", &err));
        QVERIFY(src.model()->addNode(QUuid::createUuid(), model::Id(), "Проект A").isEmpty());
        QByteArray exported = src.exportContainer("пароль-экспорта-1", &err);
        QVERIFY(!exported.isEmpty());
        QFile f(dir_.filePath("export.tb")); f.open(QIODevice::WriteOnly); f.write(exported); f.close();
        Application dst(dir_.filePath("dst"));
        QVERIFY(dst.importContainer(exported, "пароль-экспорта-1", "новый-пароль-123", &err));
        QCOMPARE(dst.mode(), Application::Mode::Password);
        QCOMPARE(dst.model()->nodes().size(), 1);
    }
    void wrongExportPasswordFails() {
        Application src(dir_.path());
        QString err; QVERIFY(src.createPasswordVault("пароль-источника", &err));
        QByteArray exported = src.exportContainer("пароль-экспорта-1", &err);
        Application dst(dir_.filePath("dst2"));
        QVERIFY(!dst.importContainer(exported, "неверный-пароль!!", "новый-пароль-123", &err));
        QVERIFY(!err.isEmpty());
    }
#ifdef Q_OS_WIN
    void switchPasswordToHelloAndBack() {
        Application app(dir_.path() + "/sw");
        QString err; QVERIFY(app.createPasswordVault("пароль-переключения", &err));
        QVERIFY(app.model()->addNode(QUuid::createUuid(), model::Id(), "P").isEmpty());
        QVERIFY(app.switchToHello(&err));
        QCOMPARE(app.mode(), Application::Mode::Hello);
        QCOMPARE(app.model()->nodes().size(), 1);   // модель не потеряна
        QVERIFY(app.switchToPassword("новый-пароль-123", &err));
        QCOMPARE(app.mode(), Application::Mode::Password);
    }
#endif
```

- [x] **Step 2: Реализовать в `Application`**

```cpp
QByteArray Application::exportContainer(const QByteArray& exportPw, QString* err) {
    if (gate_ != Gate::Ready) { if (err) *err = "Хранилище не разблокировано."; return {}; }
    if (!validPassword(exportPw)) { if (err) *err = "Пароль экспорта: 12–1024 байт."; return {}; }
    return container::packPassword(exportPw, model_.toCbor(), nullptr);
}
bool Application::importContainer(const QByteArray& bytes, const QByteArray& importPw, const QByteArray& newLocalPw, QString* err) {
    auto o = container::openPassword(bytes, importPw);
    if (o.status != container::Status::Ok) { if (err) *err = "Неверный пароль экспорта или файл повреждён."; return false; }
    model::VaultModel m;
    QString perr;
    if (m.fromCbor(o.cbor, &perr) != model::VaultModel::ParseStatus::Ok) { if (err) *err = perr; return false; }
    // проверено полностью — создаём новое локальное хранилище
    if (!store::ensurePrivateDir(QFileInfo(store_.mainPath()).absolutePath())) { if (err) *err = "Не удалось создать каталог данных."; return false; }
    SecureKey k;
    QByteArray c = container::packPassword(newLocalPw, m.toCbor(), &k);
    if (c.isEmpty() || !store_.save(c, err)) return false;
    model_ = std::move(m);
    dataKey_ = std::make_unique<SecureKey>(); memcpy(dataKey_->data(), k.data(), SecureKey::Size);
    mode_ = Mode::Password; gate_ = Gate::Ready;
    emit stateChanged();
    return true;
}
#ifdef Q_OS_WIN
bool Application::switchToPassword(const QByteArray& newPw, QString* err) {
    if (gate_ != Gate::Ready) { if (err) *err = "Хранилище не разблокировано."; return false; }
    if (!validPassword(newPw)) { if (err) *err = "Новый пароль: 12–1024 байт."; return false; }
    SecureKey k;
    QByteArray c = container::packPassword(newPw, model_.toCbor(), &k);
    if (c.isEmpty() || !store_.save(c, err)) return false;
    dataKey_ = std::make_unique<SecureKey>(); memcpy(dataKey_->data(), k.data(), SecureKey::Size);
    mode_ = Mode::Password;
    emit stateChanged();
    return true;
}
bool Application::switchToHello(QString* err) {
    if (gate_ != Gate::Ready) { if (err) *err = "Хранилище не разблокировано."; return false; }
    SecureKey k;
    QByteArray c = container::packHello(model_.toCbor(), &k);
    if (c.isEmpty() || !store_.save(c, err)) return false;
    dataKey_ = std::make_unique<SecureKey>(); memcpy(dataKey_->data(), k.data(), SecureKey::Size);
    mode_ = Mode::Hello;
    emit stateChanged();
    return true;
}
#endif
```

До вызова switch-методов UI обязан выполнить `VerifyDialog::verify` (текущая проверка из спеки §3); в тестах методы вызываются напрямую из разблокированного состояния.

- [x] **Step 3: UI в MainWindow**

Меню «Хранилище» дополнить:

```cpp
    file->addSeparator();
    file->addAction("Защищённый экспорт...", this, &MainWindow::exportVault);
    file->addAction("Импорт из файла...", this, &MainWindow::importVault);
    auto* settings = menuBar()->addMenu("Настройки");
    settings->addAction((app_.mode() == Application::Mode::Password && helloAvailable_) ? "Перейти на Windows Hello..." : "Перейти на мастер-пароль...", this, &MainWindow::switchUnlockMode);
```

`exportVault()`: `VerifyDialog::verify` (если текущая политика записи защищена — для любого экспорта обязательна проверка: вызвать `VerifyDialog::verify` всегда), `ExportDialog` (пароль дважды), `QFileDialog::getSaveFileName` (фильтр `TokenBucket (*.tb)`), запись через `store::realFileOps().writeAll` (права от создания), сообщение об успехе/ошибке.
`importVault()`: предупреждение «Импорт заменит текущее хранилище новым без слияния. Продолжить?», выбор файла, пароль файла (QInputDialog::Password), `ExportDialog` для нового локального пароля, `core.importContainer(...)`, `refreshAll()`.
`switchUnlockMode()`: `VerifyDialog::verify` → если в Hello: `ExportDialog` для нового пароля → `switchToPassword`; если в Password: `switchToHello`. Сообщения об ошибке — `QMessageBox::critical`. Смена мастер-пароля без смены режима — тот же `switchToPassword` с новым паролем (пункт меню Настройки → «Сменить мастер-пароль...»).
`helloAvailable_` — член bool, заполняется в конструкторе (`#ifdef Q_OS_WIN` → `platform::hello::availability()`).

`ExportDialog` — форма двух `QLineEdit::Password` + валидация `Application::validPassword` в `accept()` (как в `SetupDialog`).

- [x] **Step 4: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add src/app/Application.h src/app/Application.cpp src/ui/ExportDialog.h src/ui/ExportDialog.cpp src/ui/MainWindow.h src/ui/MainWindow.cpp tests/test_app.cpp
git commit -m "Экспорт/импорт защищённым файлом и смена режима разблокировки"
```

---

### Task 12: SSH — точный импорт байтов и защищённая выдача в файл

**Files:**
- Modify: `src/ui/EntryCard.cpp` (кнопка «Сохранить ключ в файл...»), `tests/test_ui.cpp` (или новый `tests/test_ssh.cpp`)
- Create: `tests/test_ssh.cpp`

**Interfaces:**
- Consumes: `EntryCard`, `FileOps::writeAll`, `VerifyDialog`.
- Produces: логика выдачи в `EntryCard::saveSshToFile()` (приватный слот): повторная проверка при `reverify`; предупреждение о выдаче ключа за пределы хранилища; отказ по символической ссылке; перезапись только после явного подтверждения; запись через `store::realFileOps().writeAll` (ограниченные права с момента создания); байты `sshPrivate` записываются без изменений.

- [x] **Step 1: Failing-тест `tests/test_ssh.cpp`**

```cpp
#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include "../src/app/Application.h"
#include "../src/platform/FileOps.h"
using namespace tb;
class TestSsh : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
    void keyBytesSurviveRoundtrip() {
        Application app(dir_.path());
        QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        model::Id p{QUuid::createUuid()};
        QVERIFY(app.model()->addNode(p, model::Id(), "P").isEmpty());
        const QByteArray key = QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjEAAAAA\r\n-----END OPENSSH PRIVATE KEY-----\r\n");
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = p;
        e.type = model::EntryType::SshKey; e.title = "K"; e.sshPrivate = key;
        QVERIFY(app.model()->setEntry(e).isEmpty());
        app.save(&err);
        app.lock();
        QVERIFY(app.unlock("пароль-теста-123", &err));
        QCOMPARE(app.model()->entry(e.id)->sshPrivate, key);   // байты не изменены после цикла
        // выдача в файл
        const QString out = dir_.filePath("id_test");
        QVERIFY(store::realFileOps().writeAll(out, app.model()->entry(e.id)->sshPrivate));
        QFile f(out); f.open(QIODevice::ReadOnly);
        QCOMPARE(f.readAll(), key);
    }
    void symlinkRefused() {
        const QString realF = dir_.filePath("real");
        QVERIFY(store::realFileOps().writeAll(realF, "x"));
        const QString link = dir_.filePath("link");
        QFile::link(realF, link);
        QString err;
        QVERIFY(tb::ui::EntryCard::isSafeTarget(link, &err) == false); // статическая проверка цели
        QVERIFY(!err.isEmpty());
    }
};
QTEST_MAIN(TestSsh)
#include "test_ssh.moc"
```

- [x] **Step 2: Реализовать в `EntryCard`**

Статический метод (добавить в заголовок):

```cpp
static bool isSafeTarget(const QString& path, QString* err);
```

```cpp
bool EntryCard::isSafeTarget(const QString& path, QString* err) {
    QFileInfo fi(path);
    if (fi.isSymbolicLink() || fi.isShortcut()) { if (err) *err = "Запись по символической ссылке запрещена."; return false; }
    return true;
}
```

Слот `saveSshToFile()`:

```cpp
void EntryCard::saveSshToFile() {
    const auto* e = app_.model()->entry(id_);
    if (!e || e->type != model::EntryType::SshKey || e->sshPrivate.isEmpty()) return;
    if (e->reverify && !VerifyDialog::verify(app_, this)) return;
    if (QMessageBox::warning(this, "TokenBucket",
            "Приватный ключ будет сохранён за пределами хранилища в незашифрованном виде. Продолжить?",
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    QString path = QFileDialog::getSaveFileName(this, "Сохранение приватного ключа");
    if (path.isEmpty()) return;
    QString err;
    if (!isSafeTarget(path, &err)) { QMessageBox::critical(this, "TokenBucket", err); return; }
    if (QFile::exists(path)
        && QMessageBox::question(this, "TokenBucket", "Файл существует. Перезаписать?") != QMessageBox::Yes) return;
    if (!store::realFileOps().writeAll(path, e->sshPrivate)) { QMessageBox::critical(this, "TokenBucket", "Не удалось записать файл."); return; }
}
```

Подключить кнопку: `connect(sshToFile_, &QPushButton::clicked, this, &EntryCard::saveSshToFile); sshToFile_->setVisible(ssh);` в `showEntry`.

- [x] **Step 3: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS. На Windows `QFile::link` создаёт .lnk — `isShortcut()` покрывает оба случая.

- [x] **Step 4: Commit**

```bash
git add src/ui/EntryCard.h src/ui/EntryCard.cpp tests/test_ssh.cpp tests/CMakeLists.txt
git commit -m "SSH: точные байты ключа и защищённая выдача в файл"
```

---

### Task 13: Windows — быстрая вставка (Ctrl+Alt+Space, мини-панель, получатель, SendInput)

**Files:**
- Create: `src/platform/QuickPaste.h`, `src/platform/QuickPasteWin.h`, `src/platform/quickpaste_win.cpp`, `src/ui/MiniPanel.h`, `src/ui/MiniPanel.cpp`, `tests/test_paste_win.cpp`
- Modify: `src/main.cpp`, `src/ui/MainWindow.h/.cpp` (создание MiniPanel/QuickPaste), `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Application`, `ClipboardGuard`, `hello`, модель.
- Produces: `namespace tb::paste { bool eligibleForAutoPaste(const model::Entry& e); struct Recipient { void* hwnd = nullptr; QString processName, windowTitle; }; }` (чистая логика + WinAPI-захват в win-файле); `tb::platform::QuickPaste : QObject { QuickPaste(Application&, ClipboardGuard&, QWidget* mainForPanelParent); bool start(QString* err); void showPanel(); signals: void conflictDetected(const QString& combo); }`; `tb::ui::MiniPanel : QWidget { MiniPanel(Application&, ClipboardGuard&, QWidget* parent=nullptr); void popup(); }`.

Правила: горячая клавиша Ctrl+Alt+Space из QSettings (`hotkey`, ключ по умолчанию `Ctrl+Alt+Space`); при `RegisterHotKey` конфликте — сигнал `conflictDetected` и меню Настройки с `QKeySequenceEdit`; в панели показываются имя процесса и заголовок окна-получателя, зафиксированные при открытии; передача — только Ctrl+V без Enter; отмена при смене фокуса или недоступности процесса; многострочные секреты и SSH-ключи к автопоставке не допускаются.

- [ ] **Step 1: Failing-тест `tests/test_paste_win.cpp`**

```cpp
#include <QtTest>
#include "../src/app/Application.h"
#include "../src/platform/QuickPaste.h"
using namespace tb;
class TestPaste : public QObject {
    Q_OBJECT
private slots:
    void eligibility() {
        model::Entry ok; ok.type = model::EntryType::ApiToken; ok.token = "single-line-token";
        QVERIFY(paste::eligibleForAutoPaste(ok));
        model::Entry multi = ok; multi.token = "line1\nline2";
        QVERIFY(!paste::eligibleForAutoPaste(multi));
        model::Entry crlf = ok; crlf.token = "line1\r\nline2";
        QVERIFY(!paste::eligibleForAutoPaste(crlf));
        model::Entry ssh; ssh.type = model::EntryType::SshKey; ssh.token.clear(); ssh.sshPrivate = "key";
        QVERIFY(!paste::eligibleForAutoPaste(ssh));
        model::Entry pass; pass.type = model::EntryType::Credentials; pass.password = "pw";
        QVERIFY(paste::eligibleForAutoPaste(pass)); // однострочный пароль допустим
    }
    void recipientChangedDetected() {
        // наш собственный тестовый HWND как «получатель»; смена фокуса обязана дать false
        QWidget w; w.show();
        QTest::qWaitForWindowExposed(&w);
        auto r = paste::captureRecipient();
        QVERIFY(r.hwnd != nullptr);
        QVERIFY(paste::recipientStillValid(r));
        QWidget other; other.show(); other.activateWindow();
        QTest::qWaitForWindowExposed(&other);
        other.raise(); other.activateWindow();
        QTest::qWait(200);
        // после перевода фокуса на other исходный получатель недействителен
        QVERIFY(!paste::recipientStillValid(r) || r.hwnd == (void*)other.winId());
    }
};
QTEST_MAIN(TestPaste)
#include "test_paste_win.moc"
```

Тест `recipientChangedDetected` использует реальный фокус GUI — на CI без интерактива может быть неустойчив; пометить `QSKIP` не нужно: локальная Windows-сессия интерактивна.

- [ ] **Step 2: Реализовать `src/platform/QuickPaste.h` + `quickpaste_win.cpp`**

`src/platform/QuickPaste.h` (кроссплатформенная часть):

```cpp
#pragma once
#include "../model/Types.h"
#include <QString>
namespace tb::paste {
inline bool eligibleForAutoPaste(const model::Entry& e) {
    if (e.type == model::EntryType::SshKey) return false;
    const QString s = e.type == model::EntryType::ApiToken ? e.token : e.password;
    return !s.isEmpty() && !s.contains('\n') && !s.contains('\r');
}
#ifdef Q_OS_WIN
struct Recipient { void* hwnd = nullptr; QString processName, windowTitle; };
Recipient captureRecipient();
bool recipientStillValid(const Recipient& r);
void sendCtrlV();
#endif
}
```

`src/platform/quickpaste_win.cpp` (только Windows; в CMake под `if(WIN32)`):

```cpp
#include "QuickPaste.h"
#include "QuickPasteWin.h"
#include "../app/Application.h"
#include "../ui/MiniPanel.h"
#include <QAbstractNativeEventFilter>
#include <QFileInfo>
#include <QSettings>
#include <QWidget>
#include <windows.h>
namespace tb {
namespace paste {
Recipient captureRecipient() {
    Recipient r;
    HWND fg = GetForegroundWindow();
    if (!fg) return r;
    r.hwnd = fg;
    wchar_t title[512] = {};
    GetWindowTextW(fg, title, 512);
    r.windowTitle = QString::fromWCharArray(title);
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t path[MAX_PATH] = {};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &sz)) r.processName = QFileInfo(QString::fromWCharArray(path)).fileName();
        CloseHandle(h);
    } else {
        r.processName = "(нет доступа к процессу — возможно, повышенные права)";
    }
    return r;
}
bool recipientStillValid(const Recipient& r) {
    return r.hwnd && GetForegroundWindow() == r.hwnd && IsWindow(r.hwnd);
}
void sendCtrlV() {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'V';
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'V';    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}
}
namespace platform {
class QuickPaste : public QObject, QAbstractNativeEventFilter {
    Q_OBJECT
public:
    QuickPaste(Application& app, ClipboardGuard& clip, QWidget* panelParent)
        : app_(app), clip_(clip), panel_(new ui::MiniPanel(app, clip, panelParent)) {}
    bool start(QString* err) {
        qApp->installNativeEventFilter(this);
        return reRegister(err);
    }
    bool reRegister(QString* err) {
        if (hotkeyId_) { UnregisterHotKey(hotkeyHwnd_, hotkeyId_); hotkeyId_ = 0; }
        const QKeySequence ks = QKeySequence(QSettings().value("hotkey", "Ctrl+Alt+Space").toString());
        const auto [mods, vk] = splitSequence(ks);
        hotkeyHwnd_ = reinterpret_cast<HWND>(hidden_.winId());
        if (!RegisterHotKey(hotkeyHwnd_, 1, mods, vk)) {
            if (err) *err = "Сочетание занято другим приложением.";
            emit conflictDetected(ks.toString());
            return false;
        }
        hotkeyId_ = 1;
        return true;
    }
    bool nativeEventFilter(const QByteArray& t, void* m, long*) override {
        if (t != "windows_generic_MSG") return false;
        auto* msg = static_cast<MSG*>(m);
        if (msg->message == WM_HOTKEY && msg->hwnd == hotkeyHwnd_ && msg->wParam == 1) panel_->popup();
        return false;
    }
signals:
    void conflictDetected(const QString& combo);
private:
    static std::pair<UINT, UINT> splitSequence(const QKeySequence& ks) {
        UINT mods = 0; UINT vk = 0;
        const QKeyCombination c = ks[0];
        if (c.keyboardModifiers() & Qt::CTRL) mods |= MOD_CONTROL;
        if (c.keyboardModifiers() & Qt::ALT) mods |= MOD_ALT;
        if (c.keyboardModifiers() & Qt::SHIFT) mods |= MOD_SHIFT;
        vk = UINT(c.key());
        return {mods, vk};
    }
    Application& app_;
    ClipboardGuard& clip_;
    ui::MiniPanel* panel_;
    QWidget hidden_;              // невидимое окно-владелец горячей клавиши
    HWND hotkeyHwnd_ = nullptr;
    int hotkeyId_ = 0;
};
}
}
```

Класс `QuickPaste` объявить в отдельном заголовке `src/platform/QuickPasteWin.h` (moc требует заголовка для класса с `Q_OBJECT`; подключается в CMake только под `if(WIN32)`); в `quickpaste_win.cpp` — реализация методов. Горячая клавиша регистрируется на скрытый `QWidget hidden_` (его `winId()`); сообщения `WM_HOTKEY` приходят этому окну и перехватываются нативным фильтром по `msg->hwnd`.

- [ ] **Step 3: Реализовать `src/ui/MiniPanel.h/.cpp`**

```cpp
#pragma once
#include <QWidget>
namespace tb { class Application; class ClipboardGuard; }
namespace tb::ui {
class MiniPanel : public QWidget {
    Q_OBJECT
public:
    MiniPanel(Application& app, ClipboardGuard& clip, QWidget* parent = nullptr);
    void popup();
private:
    void send();
    Application& app_;
    ClipboardGuard& clip_;
    class QLineEdit* search_{};
    class QListWidget* list_{};
    class QLabel* recipientLbl_{};
#ifdef Q_OS_WIN
    paste::Recipient recipient_{};   // заполняется в popup()
#endif
};
}
```

```cpp
#include "MiniPanel.h"
#include "../app/Application.h"
#include "../platform/ClipboardGuard.h"
#include "../platform/QuickPaste.h"
#include "../platform/QuickPasteWin.h"
#include "VerifyDialog.h"
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QShortcut>
#include <QVBoxLayout>
namespace tb::ui {
MiniPanel::MiniPanel(Application& app, ClipboardGuard& clip, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint), app_(app), clip_(clip) {
    auto* lay = new QVBoxLayout(this);
    recipientLbl_ = new QLabel;
    search_ = new QLineEdit; search_->setPlaceholderText("Быстрый поиск записи...");
    list_ = new QListWidget;
    lay->addWidget(recipientLbl_); lay->addWidget(search_); lay->addWidget(list_);
    connect(search_, &QLineEdit::textChanged, this, [this] {
        list_->clear();
        for (auto id : app_.model()->search(search_->text())) {
            auto* e = app_.model()->entry(id);
            list_->addItem(new QListWidgetItem(e->title));
            list_->item(list_->count() - 1)->setData(Qt::UserRole, id.toString());
        }
    });
    connect(new QShortcut(Qt::Key_Return, list_), &QShortcut::activated, this, &MiniPanel::send);
    connect(list_, &QListWidget::itemDoubleClicked, this, &MiniPanel::send);
}
void MiniPanel::popup() {
    if (app_.gate() != Application::Gate::Ready) return;
    recipient_ = paste::captureRecipient(); // допустимо: панель ещё не активна, фокус у получателя
    recipientLbl_->setText("Получатель: " + recipient_.processName + " — «" + recipient_.windowTitle + "»");
    search_->clear(); list_->clear();
    show(); raise(); activateWindow();
    search_->setFocus();
}
void MiniPanel::send() {
    auto* i = list_->currentItem();
    if (!i) return;
    const auto* e = app_.model()->entry(QUuid::fromString(i->data(Qt::UserRole).toString()));
    if (!e) return;
    if (!paste::eligibleForAutoPaste(*e)) { QMessageBox::information(this, "TokenBucket", "Этот секрет недоступен для быстрой вставки. Используйте копирование."); return; }
    if (e->reverify && !VerifyDialog::verify(app_, this)) return;
    const auto text = e->type == model::EntryType::ApiToken ? e->token : e->password;
    QString err;
    if (!clip_.copySecret(text, &err)) { QMessageBox::warning(this, "TokenBucket", err); return; }
    if (!paste::recipientStillValid(recipient_)) {
        QMessageBox::information(this, "TokenBucket", "Окно-получатель изменилось. Вставка отменена; секрет скопирован в буфер.");
        return;
    }
    hide();
    if (recipient_.hwnd) SetForegroundWindow(static_cast<HWND>(recipient_.hwnd));
    paste::sendCtrlV(); // только Ctrl+V, без Enter
}
}
```

`captureRecipient` вызвать до `show()` панели — иначе фокус уже у панели. Если `processName` содержит «(нет доступа...» — отказ: повышенное окно, отмена с сообщением (добавить проверку в `send()`).

- [ ] **Step 4: Связать в `main.cpp`/`MainWindow`**

`main.cpp` (Windows): после создания окна — `tb::platform::QuickPaste qp(core, guard, &w); QString qerr; qp.start(&qerr);` при отказе — `QMessageBox` «Сочетание Ctrl+Alt+Space занято» и открытие настроек со сменой (QKeySequenceEdit → `QSettings.setValue("hotkey", ...)` → `qp.reRegister(...)`). Меню «Настройки → Горячая клавиша...» в MainWindow.

- [ ] **Step 5: Собрать и прогнать**

Run: `cmd //c tools\\build.cmd && cmd //c tools\\test.cmd`
Expected: PASS (eligibility + recipient).

- [ ] **Step 6: Ручная проверка (записать в `docs/build/manual-checks.md`)**

Неавтоматизируемое: нажатие Ctrl+Alt+Space в Блокноте/браузере/терминале с тестовой строкой `тест-вставки-123` (не секретом); смена фокуса до подтверждения; окно «От имени администратора» → отмена. По одному пункту на строку с результатом.

- [ ] **Step 7: Commit**

```bash
git add src/platform/QuickPaste.h src/platform/quickpaste_win.h src/platform/quickpaste_win.cpp src/ui/MiniPanel.h src/ui/MiniPanel.cpp src/main.cpp src/ui/MainWindow.h src/ui/MainWindow.cpp tests/test_paste_win.cpp CMakeLists.txt tests/CMakeLists.txt docs/build/manual-checks.md
git commit -m "Windows: быстрая вставка с проверкой получателя"
```

---

### Task 14: Linux — сборка на Ubuntu, платформенные части, глобальный вызов панели

**Среда:** машина/ВМ Ubuntu 24.04 x64, GNOME Wayland. Выполняется из каталога проекта (скопировать или git-клонировать).

**Files:**
- Create: `tools/ubuntu-setup.sh`, `src/platform/gshortcut_linux.h`, `src/platform/gshortcut_linux.cpp`
- Modify: `src/main.cpp` (QLocalServer/--show-panel)

**Interfaces:**
- Consumes: apt-пакеты `build-essential cmake ninja-build qt6-base-dev qt6-base-dev-tools libsodium-dev`, `Qt6::DBus`.
- Produces: `tb::platform::PanelIpc { PanelIpc(QObject* parent=nullptr); void listen(); signals: void showRequested(); static void raiseExisting(); }` — QLocalServer «tokenbucket-panel»: первый экземпляр слушает, запуск с `--show-panel` подключается, отправляет "show", завершается. Указание пользователю GNOME: Настройки → Клавиатура → Пользовательские сочетания → команда `tokenbucket --show-panel` (в аргументах секретов нет).

- [ ] **Step 1: Установить зависимости и собрать**

`tools/ubuntu-setup.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build qt6-base-dev qt6-base-dev-tools libsodium-dev
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run: `bash tools/ubuntu-setup.sh`
Expected: сборка успешна, все CTest-тесты (включая posix-тесты прав из Задачи 5) зелёные. Если `test_clipboard` требует дисплей — запускать с `QT_QPA_PLATFORM=offscreen` в CMake env или `xvfb-run` (выбрать offscreen: `set(ENV{QT_QPA_PLATFORM} offscreen)` в тестах через `set_tests_properties(... ENVIRONMENT "QT_QPA_PLATFORM=offscreen")` в `tests/CMakeLists.txt`).

- [ ] **Step 2: Реализовать PanelIpc и включить в main.cpp**

```cpp
// src/platform/gshortcut_linux.h
#pragma once
#include <QObject>
namespace tb::platform {
class PanelIpc : public QObject {
    Q_OBJECT
public:
    explicit PanelIpc(QObject* parent = nullptr);
    void listen();                    // первый экземпляр
    static void raiseExisting();      // второй экземпляр с --show-panel
signals:
    void showRequested();
};
}
```

```cpp
// src/platform/gshortcut_linux.cpp
#include "gshortcut_linux.h"
#include <QLocalServer>
#include <QLocalSocket>
namespace tb::platform {
static const char* kServer = "tokenbucket-panel";
PanelIpc::PanelIpc(QObject* parent) : QObject(parent) {}
void PanelIpc::listen() {
    auto* srv = new QLocalServer(this);
    QLocalServer::removeServer(kServer);
    connect(srv, &QLocalServer::newConnection, this, [srv, this] {
        while (QLocalSocket* c = srv->nextPendingConnection()) {
            connect(c, &QLocalSocket::disconnected, c, &QLocalSocket::deleteLater);
            emit showRequested();
            c->disconnectFromServer();
        }
    });
    srv->listen(kServer);
}
void PanelIpc::raiseExisting() {
    QLocalSocket s;
    s.connectToServer(kServer);
    if (s.waitForConnected(500)) { s.write("show"); s.waitForBytesWritten(500); s.disconnectFromServer(); }
}
}
```

`main.cpp`: под `#ifdef Q_OS_LINUX` — при наличии `--show-panel` вызвать `PanelIpc::raiseExisting(); return 0;`; иначе создать PanelIpc и `listen()`, сигнал `showRequested` → `MiniPanel::popup()` (Linux-версия панели без SendInput: выбор записи копирует в буфер и показывает подсказку «Нажмите Ctrl+V»). MiniPanel компилируется на Linux: спрятать Windows-ветку `send()` под `#ifdef Q_OS_WIN`, Linux-ветка — копирование + `QToolTip`/метка. CMake: подключить `src/platform/gshortcut_linux.cpp` и `Qt6::DBus` (SessionWatcher) под `else()` веткой Linux.

- [ ] **Step 3: SessionWatcher DBus проверить в живой сессии**

Запустить приложение, заблокировать сеанс (Super+L), разблокировать — журнал приложения (qDebug без секретов) должен показать блокировку; сон — `systemctl suspend` в VM. Результаты — `docs/build/manual-checks.md`.

- [ ] **Step 4: Сборка и тесты**

Run: `bash tools/ubuntu-setup.sh`
Expected: PASS. Ручные проверки: копирование в GNOME Terminal/Firefox/gnome-text-editor через Ctrl+V вручную; буфер после блокировки сеанса очищается; PRIMARY не заполняется (выделить текст в другом приложении — наш секрет в PRIMARY не попадает: проверить `xclip -o -selection primary` при наличии xclip, иначе пропустить с пометкой).

- [ ] **Step 5: Commit**

```bash
git add tools/ubuntu-setup.sh src/platform/gshortcut_linux.h src/platform/gshortcut_linux.cpp src/main.cpp src/ui/MiniPanel.h src/ui/MiniPanel.cpp CMakeLists.txt tests/CMakeLists.txt docs/build/manual-checks.md
git commit -m "Linux: IPC панели, сессия/сон через DBus, сборка Ubuntu"
```

---

### Task 15: Упаковка и матрица приёмки

**Files:**
- Create: `tools/package-windows.cmd`, `packaging/debian/DEBIAN/control`, `packaging/debian/usr/share/applications/tokenbucket.desktop`, `docs/acceptance/2026-10-18-windows.md`, `docs/acceptance/2026-10-18-ubuntu.md`

**Interfaces:**
- Consumes: собранные Release-сборки обеих платформ, `docs/superpowers/specs/...` раздел 8.
- Produces: `dist/TokenBucket-windows-x64.zip` (EXE + Qt/libsodium DLL + тексты лицензий Qt LGPL и libsodium ISC), `dist/tokenbucket_0.1.0-1_amd64.deb`; заполненные протоколы приёмки по пунктам 1–10 спеки для обеих платформ (или пометка «непроверено» для недоступного).

- [ ] **Step 1: Windows-пакет**

`tools/package-windows.cmd`:

```bat
@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --build --preset default || exit /b 1
mkdir dist\TokenBucket 2>nul
copy build\TokenBucket.exe dist\TokenBucket\ || exit /b 1
windeployqt --release --no-translations dist\TokenBucket\TokenBucket.exe || exit /b 1
copy tools\vcpkg\installed\x64-windows\bin\libsodium.dll dist\TokenBucket\ 2>nul
mkdir dist\TokenBucket\licenses 2>nul
copy docs\licenses\* dist\TokenBucket\licenses\ 2>nul
powershell -NoProfile -Command "Compress-Archive -Force -Path dist\TokenBucket -DestinationPath dist\TokenBucket-windows-x64.zip"
```

Тексты лицензий положить в `docs/licenses/` (LGPL-3.0 Qt, ISC libsodium, Apache-2.0/лицензия Argon2 из поставки libsodium). Запустить: `cmd //c tools\\package-windows.cmd`. Expected: zip создан.

- [ ] **Step 2: .deb**

`packaging/debian/DEBIAN/control`:

```
Package: tokenbucket
Version: 0.1.0-1
Section: utils
Priority: optional
Architecture: amd64
Depends: libc6, libqt6widgets6, libqt6dbus6, libsodium23
Maintainer: imtonyromeo <246167+imtonyromeo@users.no-reply.gitverse.ru>
Description: Локальное хранилище API-токенов, учётных данных и SSH-ключей
 Зашифрованное локальное хранилище секретов (XChaCha20-Poly1305, Argon2id).
```

`packaging/debian/usr/share/applications/tokenbucket.desktop`:

```
[Desktop Entry]
Type=Application
Name=TokenBucket
Comment=Локальное хранилище секретов
Exec=tokenbucket
Icon=tokenbucket
Categories=Utility;Security;
```

Сборка (на Ubuntu): `mkdir -p dist && cp build/TokenBucket packaging/debian/usr/bin/tokenbucket && chmod 755 packaging/debian/usr/bin/tokenbucket && dpkg-deb --root-owner-group --build packaging/debian dist/tokenbucket_0.1.0-1_amd64.deb`. Установка в чистой среде: `sudo apt install ./dist/tokenbucket_0.1.0-1_amd64.deb && tokenbucket`.

- [ ] **Step 3: Матрица приёмки**

Создать оба протокола по шаблону (каждый пункт спеки §8 1–10): колонки «Пункт / Среда / Шаги / Результат / Доказательство». Пройти доступные пункты; для Hello-проверки с реальным устройством — участие владельца; недоступное помечать «НЕ ПРОВЕРЕНО» с причиной. Итоговый статус («готово»/«готово с ограничениями») записать в RESUME.MD.

- [ ] **Step 4: Commit**

```bash
git add tools/package-windows.cmd packaging/ docs/acceptance/ docs/licenses/ RESUME.MD
git commit -m "Упаковка Windows/.deb и протоколы приёмки"
```

---

## Самопроверка плана (выполнена при составлении)

1. **Покрытие спеки:** §2 интерфейс/данные — Задачи 4, 7, 8; §3 шифрование/разблокировка — 2, 3, 6, 11; §4 границы/память — 2 (SecureBytes), 10 (очистка); §5 сохранение/экспорт — 5, 11, 12; §6 копирование/вставка — 8, 9, 13, 14; §7 компоненты — структура задач; §8 приёмка — 15 (+ ручные проверки 7, 13, 14). Пункты «масштабирование 100–200%», «скринридер», «клавиатурная навигация» покрываются стандартными возможностями Qt Widgets и ручной проверкой в Задаче 15 (явных задач не требуют; отмечено в протоколе).
2. **Плейсхолдеры:** отсутствуют; все шаги содержат код или точные команды.
3. **Типы/сигнатуры:** `container::Opened`/`reseal`, `VaultModel` API, `FileOps`, `Application` — сверены между задачами 3–13.
4. **Review Focus:** каждый из пяти пунктов закреплён тестом в задачах 3, 4, 6, 13.
