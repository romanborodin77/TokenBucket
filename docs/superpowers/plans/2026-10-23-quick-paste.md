# Рабочая быстрая вставка — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Исправить полный маршрут горячая клавиша → панель → выбранное поле записи → исходное поле внешнего Windows-приложения, с безопасной отменой и проверяемой приёмкой.

**Architecture:** Переиспользовать Application, VerifyDialog, ClipboardGuard и MiniPanel. Разделить проверку идентичности и активности получателя, сделать смену hotkey транзакционной и последовательно выполнять подтверждение, возврат фокуса, публикацию и отправку. Не вводить общий фреймворк управления окнами или новые зависимости.

**Tech Stack:** C++20, Qt 6 Widgets/Test/Concurrent, Win32, существующие CMake/vcpkg и RTK. Выполнение — Windows, Release.

**Spec:** [2026-10-23-quick-paste-design.md](../specs/2026-10-23-quick-paste-design.md), утверждена 2026-10-23.

**Status:** План на рассмотрении; способ выполнения ещё не выбран. Утверждённые главное окно, настройки и F1–F5 остаются последующими этапами.

## Global Constraints

- «Сохраняются Qt Widgets, существующие Application/VerifyDialog/ClipboardGuard, формат хранилища и поведение остальных способов выдачи. Миграция данных и новые зависимости не нужны.»
- «Текущий таймер буфера 20 секунд сохраняется.»
- «Восстановление фокуса и отпускание физических Ctrl/Alt/Shift/Win имеют общий бюджет 1500 мс.»
- «Проверка получателя не отключается пользовательской настройкой.»
- «Вставка в повышенное окно запрещена; неизвестная идентичность/права также ведут к отказу.»
- «Enter в получатель не передаётся.»
- «Утверждение этой спецификации не разрешает менять текущий системный буфер.»
- UTF-8 без BOM и LF; tools/test.cmd остаётся CRLF. Автор и коммиттер — настроенный imtonyromeo, без подстановки identity. Только явные пути в git add; push и выпуск пакета не входят в план.
- Не считать пропуск полного маршрута успешной приёмкой; не ослаблять существующие проверки, не удалять тесты и не маскировать ошибки suppressions.

## Review Focus

1. HWND/PID переиспользованы либо дочернее поле исчезло — отказ без выдачи постороннему получателю. Проверки Task 2 и Task 5.
2. У пароля есть переносы, но выбран однострочный логин; присутствует NUL или поле пустое — оценивать выбранное поле. Проверки Task 1.
3. Запись QSettings невозможна после успешной регистрации кандидата — прежнее runtime-сочетание остаётся рабочим, ошибка хранения не скрывается. Проверки Task 3.
4. Вложенный цикл VerifyDialog/ClipboardGuard допускает lock или замену записи — старое значение не выдаётся после возврата. Проверки Task 5.
5. Получатель проигнорировал Ctrl+V, SendInput принял только часть событий либо пользователь скопировал новые данные — не обещать доставку, не повторять секрет и не стирать чужую публикацию. Проверки Task 2 и Task 5.

---

## Карта файлов и проверок

| Файл | Изменение |
| --- | --- |
| src/platform/QuickPaste.h | Выбор поля, проверки значения, снимок получателя и Windows-контракты |
| src/platform/QuickPasteWin.h | Кандидат hotkey, текущая зарегистрированная комбинация и ID |
| src/platform/quickpaste_win.cpp | Win32 identity/права/фокус, VK, регистрация и отправка |
| src/ui/MiniPanel.h/.cpp | Одна операция выдачи, элементы панели, подтверждение и неблокирующее ожидание |
| src/ui/MainWindow.cpp | Только обработчик смены сочетания и потребители изменённых контрактов |
| tests/test_paste_win.cpp | Правила полей, клавиш, получателя и имитируемого результата SendInput |
| tests/test_ui.cpp | Управление панелью и очистка состояния без публикации |
| tests/test_e2e.cpp | Внешний тестовый процесс и полный реальный маршрут |
| tools/test.cmd | Передача аргументов CTest для запуска выбранных целей |
| README.md, docs/build/manual-checks.md, docs/security/assessment-2026-10-20.md, RESUME.MD, SDD ledger | Текущее поведение и только измеренные результаты |

Новые исходные файлы/библиотеки не нужны. Внешним получателем служит специальный режим существующего test_e2e.exe; это не новая производственная функция.

### Команды и среда

В проверенной текущей PowerShell-среде cmake/ctest отсутствуют в PATH. Использовать tools/build.cmd и tools/test.cmd, которые поднимают VS environment. Не делать вывод об отсутствии установленного toolchain только по PATH.

```powershell
rtk proxy cmd /d /c tools\build.cmd
rtk proxy cmd /d /c "tools\test.cmd -N -R ^test_paste_win$"
rtk proxy cmd /d /c "tools\test.cmd -R ^test_paste_win$ --output-on-failure"
```

Для фильтрации добавить `%*` к существующему вызову CTest:

```bat
ctest --preset default %* || exit /b 1
```

Полный `tools/test.cmd` и test_e2e/test_ui нельзя считать заведомо безопасными для системного буфера: старые сценарии уже могут его менять. Перед запуском анализировать выбранные слоты/цели. Тесты используют explicit start и собственные INI QSettings; реальное пользовательское сочетание/реестр не изменяются. До согласия допустимы сборка, `ctest -N`, чистые проверки полей/VK/identity и UI-сценарии без выдачи. Изолированная станция допустима только после подтверждения её фактической изоляции; QSKIP при отсутствии права создать её не является прохождением проверки.

Вывод сборки/тестов сохранять в локальные build-логи; показывать сводку и ошибки. Число тестов и путь EXE брать из CTest/сборки, не из исторических записей.

### Task 1: Поле записи и корректное преобразование горячей клавиши

**Files:** Modify src/platform/QuickPaste.h, src/platform/QuickPasteWin.h, src/platform/quickpaste_win.cpp, tests/test_paste_win.cpp, tools/test.cmd.

**Interfaces:**
- Consumes: model::Entry, EntryType, QKeySequence.
- Produces: `enum class Field { Secret, Login };`, `const QString* fieldValue(const model::Entry&, Field);`, `bool eligibleForAutoPaste(const model::Entry&, Field = Field::Secret);`.
- Windows produces: `struct Hotkey { UINT modifiers; UINT key; };`, `std::optional<Hotkey> parseHotkey(const QKeySequence&);` in tb::paste. Windows-типы/include находятся под Q_OS_WIN.
- Field::Secret означает token для ApiToken и password для Credentials. Login допустим только для Credentials. fieldValue возвращает nullptr для неподдержанного типа/поля; указатель не живёт через цикл событий.

- [ ] **Step 1: Зафиксировать исходное состояние.** Проверить ближайшие AGENTS/CLAUDE, ветку, status, RESUME, spec, plan и ledger. Поднять изоляцию согласно using-git-worktrees перед реализацией; не терять текущие коммиты документов. Проверить путь vswhere/toolchain. Перед широким поиском запросить существующий Engram и подтвердить нужные места в исходниках. Прочитать test-driven-development и source-driven-development на этапе выполнения.
- [ ] **Step 2: Добавить красные проверки выбранного поля и VK.** Существующую eligibility сохранить и дополнить:

```cpp
model::Entry e;
e.type = model::EntryType::Credentials;
e.login = QStringLiteral("demo-user");
e.password = QStringLiteral("line1\nline2");
QVERIFY(paste::eligibleForAutoPaste(e, paste::Field::Login));
QVERIFY(!paste::eligibleForAutoPaste(e, paste::Field::Secret));
e.login = QString(QChar::Null);
QVERIFY(!paste::eligibleForAutoPaste(e, paste::Field::Login));
#ifdef Q_OS_WIN
const auto f = paste::parseHotkey(QKeySequence(QStringLiteral("Ctrl+Alt+F12")));
QVERIFY(f.has_value());
QCOMPARE(f->key, UINT(VK_F12));
QVERIFY(f->modifiers & MOD_NOREPEAT);
QVERIFY(!paste::parseHotkey(QKeySequence(QStringLiteral("Shift+F1"))));
QVERIFY(!paste::parseHotkey(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+C"))));
#endif
```

Дополнить data-таблицей: A/Z/0/9/F1/F12/Space; Ctrl/Alt/Win и Shift в комбинации; голая клавиша, только Shift, неизвестные модификаторы, пустой аккорд, Tab, пустое поле, CR, LF, NUL, SSH и Login у ApiToken.
- [ ] **Step 3: Получить RED.** Сборка должна указать отсутствующие Field/parseHotkey; после объявления контрактов проверки должны падать именно по поведению старого преобразования/выбора. Проверить, что неудача не вызвана toolchain. Добавить `%*` в test.cmd и проверить `-N`, не выполняя тесты.
- [ ] **Step 4: Минимальная реализация.** Для поля вернуть адрес соответствующей существующей QString. Для авто — проверить указатель, непустоту и отсутствие CR/LF/NUL. Старые вызовы без Field сохраняют прежний Secret. VK вычислять явно:

```cpp
if (key >= Qt::Key_A && key <= Qt::Key_Z) vk = UINT('A' + key - Qt::Key_A);
else if (key >= Qt::Key_0 && key <= Qt::Key_9) vk = UINT('0' + key - Qt::Key_0);
else if (key >= Qt::Key_F1 && key <= Qt::Key_F12) vk = UINT(VK_F1 + key - Qt::Key_F1);
else if (key == Qt::Key_Space) vk = VK_SPACE;
else return std::nullopt;
```

Перед этим отклонить число аккордов != 1, неизвестные модификаторы и отсутствие Ctrl/Alt/Win; добавить MOD_NOREPEAT. Не создавать копию значения ради проверки длины/переносов.
- [ ] **Step 5: GREEN и коммит.** Собрать Release, выполнить test_paste_win после подтверждения отсутствия публикации в текущем наборе слотов. Проверить LF/CRLF и staged diff. Commit: `Быстрая вставка: выбор поля и допустимые горячие клавиши`.

### Task 2: Идентичность получателя, права и результат отправки

**Files:** Modify src/platform/QuickPaste.h, src/platform/quickpaste_win.cpp, src/ui/MiniPanel.cpp, tests/test_paste_win.cpp, tests/test_e2e.cpp.

**Interfaces:**
- Consumes: Field/eligibleForAutoPaste из Task 1; Win32 HWND/PID/process tokens.
- Produces Windows: `Recipient { HWND hwnd; HWND focusHwnd; DWORD pid; FILETIME created; QString processName, windowTitle; }`, `Recipient captureRecipient();`, `bool recipientExists(const Recipient&);`, `bool recipientActive(const Recipient&);`, `bool recipientAllowed(const Recipient&, QString* err);`, `bool activateRecipient(const Recipient&);`, `bool modifiersReleased();`.
- Existing `recipientStillValid(const Recipient&)` временно остаётся обёрткой recipientActive до обновления всех вызовов; после Task 5 убрать, если потребителей больше нет.
- Produces: `using InputSender = decltype(&::SendInput);`, `UINT sendCtrlV(InputSender sender = ::SendInput);`. Возврат — исходное количество вставленных событий 0…4; partial не превращается в bool success.

- [ ] **Step 1: RED на identity.** С собственным тестовым QWidget захватить снимок и проверить его существование независимо от foreground; изменить pid и created в копии — обе проверки обязаны отказать. Удалить исходное окно — отказ. Проверка child HWND должна отказывать, если оно исчезло или принадлежит другому top-level. Эти проверки не публикуют буфер.

```cpp
QWidget target;
target.show(); target.raise(); target.activateWindow();
QVERIFY(QTest::qWaitForWindowActive(&target));
const auto r = paste::captureRecipient();
QVERIFY(paste::recipientExists(r));
auto replaced = r;
replaced.pid ^= 1;
QVERIFY(!paste::recipientExists(replaced));
replaced = r;
replaced.created.dwLowDateTime ^= 1;
QVERIFY(!paste::recipientExists(replaced));
```

При невозможности активировать окно эту проверку не объявлять успешной; разделить чистую проверку подменённого снимка и фактический focus-test в отчёте.
- [ ] **Step 2: RED на частичную отправку без реального ввода.** В тесте определить `UINT WINAPI fakeInput(UINT count, LPINPUT input, int size)`; он запоминает события и возвращает управляемое количество, не вызывает Win32 SendInput. Прогнать 0/1/2/3/4, проверить возвращённое значение и отсутствие VK_RETURN/повторной передачи V-down. При partial допускаются только завершающие key-up.

```cpp
// fakeInput — локальная функция test_paste_win, объявленная с WINAPI.
for (UINT accepted : {0u, 1u, 2u, 3u, 4u}) {
    fakeAccepted = accepted;
    fakeCalls.clear();
    QCOMPARE(paste::sendCtrlV(&fakeInput), accepted);
    QVERIFY(!fakeCalls.empty());
    QCOMPARE(fakeCalls.front().size(), 4);
    for (const auto& call : fakeCalls)
        for (const auto& event : call)
            QVERIFY(event.ki.wVk != VK_RETURN);
}
```

fakeInput сохраняет каждый массив INPUT и возвращает (fakeAccepted < count ? fakeAccepted : count), а для отдельного случая отказа cleanup возвращает 0 на последующих вызовах. fakeAccepted/fakeCalls определяются в anonymous namespace тестового файла, с восстановлением между случаями; никакого глобального тестового состояния в продукте.
- [ ] **Step 3: Реализовать Win32-проверки.** Через OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)/GetProcessTimes получить pid+created; scope guards закрывают каждый HANDLE. recipientExists проверяет окно, его PID, время создания и принадлежность focusHwnd. recipientActive дополнительно требует актуальный foreground и исходный focusHwnd, когда он был доступен. activateRecipient возвращает результат SetForegroundWindow, без ожиданий и обхода ограничений. recipientAllowed читает TokenIntegrityLevel/TokenElevation своего и целевого процесса; повышенный/недоступный/несовместимый получатель отклоняется. Имя/заголовок никогда не участвуют в доверии. modifiersReleased читает старший бит GetAsyncKeyState для Ctrl/Alt/Shift/LWin/RWin.
- [ ] **Step 4: Реализовать raw count и безопасное завершение.** Передать ровно 4 события Ctrl-down,V-down,V-up,Ctrl-up. При count 1 отправить только Ctrl-up; при 2 — V-up/Ctrl-up; при 3 — Ctrl-up. Не повторять полезный ввод; возвратить исходный count даже при неуспешном cleanup. Существующий MiniPanel сравнивает результат с 4, иначе промежуточная версия ошибочно сочтёт partial успехом. Обновить остальные вызовы и HWND-casts.
- [ ] **Step 5: GREEN и коммит.** Запустить безопасные test_paste_win; native права/focus проверить отдельно, не снимать защиту ради среды. Обновить контрактные комментарии: повторная проверка не устраняет атомарно TOCTOU. Commit: `Быстрая вставка: получатель и результат передачи ввода`.

### Task 3: Транзакционная смена горячей клавиши

**Files:** Modify src/platform/QuickPasteWin.h, src/platform/quickpaste_win.cpp, src/ui/MainWindow.cpp, tests/test_paste_win.cpp.

**Interfaces:**
- Consumes: `parseHotkey` из Task 1; существующий HWND hotkeyOwnerWindow.
- Produces public: `bool QuickPaste::changeSequence(const QKeySequence&, QSettings&, QString* err);`, `QKeySequence QuickPaste::sequence() const;`. `start(QString*)` и popupPanel остаются. Добавить `bool start(const QKeySequence&, QString* err)` для регистрации явно переданной комбинации без чтения/изменения пользовательских настроек; обычный start читает QSettings и делегирует ему. MainWindow создаёт обычный QSettings и передаёт его changeSequence; он больше не записывает hotkey сам.
- State: текущий QKeySequence sequence_, текущий ID hotkeyId_ и hotkeyHwnd_; кандидат использует второй ID. Никакой общей системы транзакций настроек.

- [ ] **Step 1: RED на конфликт/хранение.** С временным INI QSettings зарегистрировать свободное тестовое сочетание; занять другой кандидат вторым тестовым HWND; changeSequence обязан вернуть false, sequence() и старый ID остаться рабочими, settings.value("hotkey") не измениться. Для гарантированного AccessError передать QSettings, путь которого является каталогом, а не файлом. Не полагаться только на read-only attribute.

```cpp
QTemporaryDir dir;
QSettings settings(dir.filePath("hotkey.ini"), QSettings::IniFormat);
const auto before = hotkey.sequence();
QSignalSpy conflict(&hotkey, &platform::QuickPaste::conflictDetected);
QVERIFY(!hotkey.changeSequence(occupied, settings, &error));
QCOMPARE(hotkey.sequence(), before);
QVERIFY(!error.isEmpty());
```

hotkey создаётся из существующих Application/ClipboardGuard и запускается explicit start с тестовым сочетанием, а временный INI содержит прежнее значение; занятие occupied и проверка старой регистрации выполняются в этом же слоте через RegisterHotKey/WM_HOTKEY. При конфликте тестового окружения подобрать кандидат из фиксированного набора Ctrl+Alt+Shift+F8…F12 и явно сообщить невозможность, не выпускать чужие регистрации.
- [ ] **Step 2: Реализовать алгоритм.** Распарсить кандидат; совпадение с sequence_ вернуть как успех без перерегистрации. Иначе candidateId = hotkeyId_ == 1 ? 2 : 1; RegisterHotKey кандидата, старый не снимать. Сохранить прежний QVariant/факт отсутствия ключа; setValue(PortableText), sync, status. При ошибке снять только кандидата, попытаться восстановить прежнее хранимое значение и честно сообщить сохраняющуюся ошибку хранения. При успехе снять старый ID и заменить runtime-state.

```cpp
if (!RegisterHotKey(hotkeyHwnd_, candidateId, parsed->modifiers, parsed->key))
    return reportConflict(candidate);
settings.setValue(QStringLiteral("hotkey"), candidate.toString(QKeySequence::PortableText));
settings.sync();
if (settings.status() != QSettings::NoError) {
    UnregisterHotKey(hotkeyHwnd_, candidateId);
    restorePreviousSetting();
    return reportStorageFailure();
}
if (hotkeyId_) UnregisterHotKey(hotkeyHwnd_, hotkeyId_);
hotkeyId_ = candidateId;
sequence_ = candidate;
```

reportConflict/restorePreviousSetting/reportStorageFailure — локальные лямбды этого метода: первая выставляет err и emit conflictDetected; вторая setValue прежнего QVariant либо remove и sync; третья выставляет err с сообщением об ошибке хранения. Не создавать отдельные сервисы ради этих трёх действий.
- [ ] **Step 3: Старт и доставка.** Обычный start читает существующую строку совместимо и регистрирует её без принудительного переписывания настроек; перегрузка с явной QKeySequence использует только переданное значение. Native event filter устанавливается однократно, в том числе при нескольких неудачных попытках зарегистрировать тестовый кандидат. nativeEventFilter принимает документированные Windows native message types, но обрабатывает только WM_HOTKEY с активным ID/HWND. Дубликат не создаёт вторую операцию; MOD_NOREPEAT подавляет удержание. Деструктор снимает активную регистрацию.
- [ ] **Step 4: GREEN и коммит.** Проверить valid/invalid/same/conflict/AccessError/start failure и сохранность старой комбинации. Проверки не используют системный буфер. Commit: `Быстрая вставка: безопасная смена сочетания`.

### Task 4: Внешний получатель и красная фиксация полного маршрута

**Files:** Modify tests/test_e2e.cpp, tests/test_paste_win.cpp, tests/test_ui.cpp, tests/CMakeLists.txt только при необходимости аргументов/меток теста.

**Interfaces:**
- Consumes: контракты Tasks 1–3 и существующая MiniPanel до исправления и реальные QWidget/Win32/ClipboardGuard.
- Produces test-only child mode: `test_e2e.exe --paste-receiver`; QApplication с QLineEdit, stdout-протокол `READY <hwnd> <pid>` и `TEXT <base64-utf8>`. Секрет не передаётся аргументом; значения только вымышленные тестовые. Родитель запускает QCoreApplication::applicationFilePath() через QProcess.
- Helper functions в anonymous namespace: `int runPasteReceiver(QApplication&);`, `bool waitForReceiver(QProcess&, HWND*, DWORD*);`, `QByteArray receiverText(QProcess&);`. Таймауты не более 5 с; child закрывается terminate/waitForFinished и kill только своего дочернего процесса при отказе.

- [ ] **Step 1: Режим тестового получателя до исправления Task 5.** Заменить QTEST_MAIN собственным main без изменения обычного QTest::qExec. Режим --paste-receiver проверяется после создания QApplication и до запуска тестового класса:

```cpp
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--paste-receiver")))
        return runPasteReceiver(app);
    TestE2E test;
    return QTest::qExec(&test, argc, argv);
}
```

runPasteReceiver создаёт видимый QLineEdit, устанавливает заголовок тестового получателя и выводит READY после winId(); textChanged выводит TEXT с toUtf8().toBase64() и flush. waitForReceiver аккумулирует stdout до целой строки, проверяет разбор HWND/PID и состояние QProcess. receiverText читает завершённые TEXT-строки, декодирует последнюю; ранний выход/таймаут — явный fail.
- [ ] **Step 2: Полный RED до изменения панели.** В новом слоте `TestE2E::quickPasteFullRoute()` создать QuickPaste на временном Application/ClipboardGuard и explicit start зарегистрировать свободное сочетание из Ctrl+Alt+F8…F12/Space, без чтения или записи реальных QSettings; запустить receiver; привести его в foreground и проверить PID; отправить реальное зарегистрированное сочетание через SendInput (6 событий Ctrl/Alt/выбранная клавиша down/up). Не заменять этот шаг popupPanel/PostMessage: они не проверяют регистрацию. Дождаться miniPanel и её QLineEdit, набрать поиск QtTest и нажать Enter; сравнить receiverText с точным demo-значением. Новое переключение поля проверяется после Task 5, чтобы исходный RED не зависел от ещё не созданного UI.

```cpp
QTemporaryDir dataDir;
Application vault(dataDir.path());
QString error;
QVERIFY(vault.createPasswordVault("test-password-123", &error));
const auto project = QUuid::createUuid(), group = QUuid::createUuid();
QVERIFY(vault.model()->addNode(project, {}, "Demo").isEmpty());
QVERIFY(vault.model()->addNode(group, project, "Demo group").isEmpty());
model::Entry demo;
demo.id = QUuid::createUuid(); demo.groupId = group;
demo.type = model::EntryType::Credentials;
demo.title = "demo"; demo.password = "demo-password-777";
QVERIFY(vault.model()->setEntry(demo).isEmpty());
ClipboardGuard clipboard;
platform::QuickPaste hotkey(vault, clipboard, nullptr);
bool registered = false;
for (const auto* candidate : {"Ctrl+Alt+F8", "Ctrl+Alt+F9", "Ctrl+Alt+F10", "Ctrl+Alt+F11", "Ctrl+Alt+F12", "Ctrl+Alt+Space"}) {
    if (hotkey.start(QKeySequence(QString::fromLatin1(candidate)), &error)) {
        registered = true;
        break;
    }
}
QVERIFY2(registered, qPrintable(error));
QProcess receiver;
receiver.start(QCoreApplication::applicationFilePath(), {"--paste-receiver"});
QVERIFY(receiver.waitForStarted());
HWND target = nullptr; DWORD receiverPid = 0;
QVERIFY(waitForReceiver(receiver, &target, &receiverPid));
QVERIFY(receiverPid != GetCurrentProcessId());
QVERIFY(SetForegroundWindow(target));
QTRY_VERIFY(GetForegroundWindow() == target);
INPUT trigger[6] = {};
const auto parsed = paste::parseHotkey(hotkey.sequence());
QVERIFY(parsed.has_value());
const WORD vk = WORD(parsed->key);
const WORD keys[6] = {VK_CONTROL, VK_MENU, vk, vk, VK_MENU, VK_CONTROL};
for (int i = 0; i < 6; ++i) {
    trigger[i].type = INPUT_KEYBOARD;
    trigger[i].ki.wVk = keys[i];
    trigger[i].ki.dwFlags = i >= 3 ? KEYEVENTF_KEYUP : 0;
}
QCOMPARE(SendInput(6, trigger, sizeof(INPUT)), UINT(6));
ui::MiniPanel* panel = nullptr;
QTRY_VERIFY(([&] {
    for (auto* window : QApplication::topLevelWidgets())
        if (window->objectName() == QStringLiteral("miniPanel")) {
            panel = qobject_cast<ui::MiniPanel*>(window);
            return panel && panel->isVisible();
        }
    return false;
}()));
auto* search = panel->findChild<QLineEdit*>();
QVERIFY(search);
QTest::keyClicks(search, "demo");
QTest::keyClick(search, Qt::Key_Return);
QTRY_COMPARE(receiverText(receiver), QByteArray("demo-password-777"));
```

Использовать временное хранилище с одной вымышленной записью demo-password-777 и новым UUID, не существующее пользовательское хранилище. Поиск старой панели выполняется по типу QLineEdit, чтобы RED не сводился к отсутствию новых objectNames. При Enter ожидается точное значение в receiver; старая панель отменяет отправку, тест получает RED. Зафиксировать причину и очистить собственную тестовую публикацию после отказа; не считать зависший диалог причиной зависнуть бесконечно. Таймаут QtTest и завершение тестового receiver обязательны.

- [ ] **Step 3: Зафиксировать harness.** Сборка/запуск receiver без вставки допустимы без изменения буфера. Сам RED полного маршрута — только с разрешением на системный буфер. При отказе SendInput самого trigger отпустить только синтетически нажатые клавиши и остановить тест; не воспроизводить полезный ввод. Сохранить лог красного поведения. Commit: `Тесты: получатель и регрессия панели быстрой вставки`; красная ветка не является кандидатом выпуска.


### Task 5: Мини-панель, последовательная выдача и регрессионная матрица

**Files:** Modify src/ui/MiniPanel.h, src/ui/MiniPanel.cpp, tests/test_ui.cpp, tests/test_e2e.cpp, tests/CMakeLists.txt; update platform callers if required.

**Interfaces:**
- Consumes: Field/fieldValue/eligibleForAutoPaste; recipientExists/Active/Allowed/activateRecipient/modifiersReleased; sendCtrlV count; Application::generation/gate/busy/model; VerifyDialog::verify; ClipboardGuard::copySecret/clearIfOurs.
- Public MiniPanel(Application&, ClipboardGuard&, QWidget* = nullptr) и popup() сохраняются.
- Private produces: `void send(bool copyOnly = false);`, `void continuePaste();`, `void clearContext();`, `bool contextValid() const;`, closeEvent/eventFilter для завершения и наблюдения активации. Один QTimer + QElapsedTimer, без worker/блокирующего processEvents-loop.
- Private state: `model::Id operationId_; paste::Field operationField_; quint64 operationGeneration_; QDateTime operationModified_; model::EntryType operationType_; bool operationReverify_;`, Windows `paste::Recipient recipient_;`, `QTimer restoreTimer_; QElapsedTimer restoreBudget_;` и один enum состояния Closed/Choosing/Verifying/Restoring. Все значения сбрасываются clearContext; никаких секретных QString в снимке.
- Test slots in TestUi: `quickPanelClosesOnLock()` и `quickPanelKeyboardManualMode()`. Для offscreen-платформы MiniPanel не обращается к Win32 foreground и открывается только в ручном режиме.
- UI objectNames: miniSearch, miniEntries, miniField, miniPaste, miniCopy, miniCancel, miniStatus. Поле хранится в itemData как Field, запись — UUID; строки не используются как идентификаторы безопасности.

- [ ] **Step 1: Проверить красную фиксацию.** Прочитать лог Task 4 и подтвердить, что upstream-шаги прошли: зарегистрированное сочетание вызвало панель с внешним получателем и запись найдена. Ошибка отсутствующего toolchain, разрешения/фокуса или селектора не заменяет RED исходного дефекта. Если live-прогон ещё не разрешён, безопасные проверки можно продолжить, но маршрут не объявляется проверенным.
- [ ] **Step 2: RED на клавиатуру/lock без публикации.** Создать Application с временным каталогом и вымышленной записью; открыть панель из своего процесса (автовставка запрещена); проверить objectNames, выбор поля, пустой поиск и недоступность вставки без получателя. app.lock() закрывает панель и очищает поиск/list/recipient; popup после нового unlock не продолжает операцию. Esc и повторный popup закрывают одно окно.

```cpp
QTemporaryDir dataDir;
Application app(dataDir.path());
QString error;
QVERIFY(app.createPasswordVault("test-password-123", &error));
const auto project = QUuid::createUuid(), group = QUuid::createUuid();
QVERIFY(app.model()->addNode(project, {}, "Demo").isEmpty());
QVERIFY(app.model()->addNode(group, project, "Demo group").isEmpty());
model::Entry demo;
demo.id = QUuid::createUuid(); demo.groupId = group;
demo.type = model::EntryType::Credentials;
demo.title = "demo"; demo.password = "demo-password-777";
QVERIFY(app.model()->setEntry(demo).isEmpty());
ClipboardGuard clipboard;
ui::MiniPanel panel(app, clipboard);
panel.popup();
auto* search = panel.findChild<QLineEdit*>("miniSearch");
auto* list = panel.findChild<QListWidget*>("miniEntries");
QVERIFY(search); QVERIFY(list);
app.lock();
QVERIFY(!panel.isVisible());
QVERIFY(search->text().isEmpty());
QCOMPARE(list->count(), 0);
```

Все данные этого слота находятся в QTemporaryDir. Никакого чтения реального хранилища; clearIfOurs при нулевой собственной публикации не должен открывать системный буфер.
- [ ] **Step 3: Реализовать UI и очистку.** Добавить QLabel статуса, QComboBox поля, явные кнопки и подсказки; настроить accessibleName/порядок Tab/видимый фокус. Return/Enter из поиска и списка и double-click вызывают один send. Up/Down из поиска двигают строку. close/Esc/повторный вызов вызывают clearContext. Подписаться на stateChanged/busyChanged; не хранить указатель Entry через диалог. Для своего процесса/неизвестного получателя показывать ручной режим, не догадку о последнем окне.
- [ ] **Step 4: Реализовать проверку операции.** Сохранить UUID/Field/modified/type/reverify/generation и recipient; snapshot не содержит сырого секрета. Отключить повторный send и редактирование выбора. VerifyDialog::verify(app, this, "Вставить пароль/логин/токен") только по reverify. После возврата contextValid сверяет snapshot и существующую запись. copyOnly использует отдельное подтверждение и copySecret без отправки, разрешая CR/LF при явном копировании.
- [ ] **Step 5: Восстановить окно и дождаться условий.** Существование/права проверяются до hide; foreground панели здесь допустим. После hide вызвать activateRecipient и стартовать QElapsedTimer/QTimer с шагом 25 мс. continuePaste отменяет по contextValid, посторонней активации или elapsed >= 1500; иначе ждёт recipientActive и modifiersReleased. Нельзя блокировать GUI или бесконечно возвращать фокус. Системное окно Hello допустимо только в границах текущего VerifyDialog; после возврата проверки выполняются заново.

```cpp
// Только внутри continuePaste, после проверок/ожиданий и без вложенного диалога.
const auto* entry = app_.model()->entry(operationId_);
if (!entry || !paste::eligibleForAutoPaste(*entry, operationField_)) return cancelWithReason();
const QString value = *paste::fieldValue(*entry, operationField_);
QString error;
if (!clip_.copySecret(value, &error)) return failPublication(error);
if (!contextValid() || !paste::recipientActive(recipient_)) {
    clip_.clearIfOurs();
    return cancelWithReason();
}
const UINT count = paste::sendCtrlV();
if (count != 4) clip_.clearIfOurs();
finishWithCount(count);
```

cancelWithReason/failPublication/finishWithCount — локальные действия этого пути, не новые внешние контракты: остановить таймер, сбросить снимок, показать безопасный статус; для count 0 — отказ, 1…3 — неопределённый результат/возможные удержанные клавиши, 4 — закрытие без обещания принятия чужим приложением. Не удерживать value через последующие сообщения/циклы: локальную копию освободить до показа диалога. После отказа публикации не обещать сохранность прежнего буфера.
- [ ] **Step 6: GREEN полного маршрута.** В tests/CMakeLists.txt добавить узкий запуск двух безопасных слотов существующего test_ui, сохранив старую цель целиком:

```cmake
add_test(NAME test_quickpaste_ui COMMAND test_ui quickPanelClosesOnLock quickPanelKeyboardManualMode)
set_tests_properties(test_quickpaste_ui PROPERTIES ENVIRONMENT
  "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins")
```

После tools/build.cmd выполнить `rtk proxy cmd /d /c "tools\test.cmd -R ^test_quickpaste_ui$ --output-on-failure"`. Этот запуск не копирует значения; он не заменяет полный test_ui/интерактивную приёмку. Прогнать regression после согласия и показать RED→GREEN. Проверить отсутствие raw-секретов в контексте, повторной выдачи и прежней pre-hide foreground-проверки. Commit: `Быстрая вставка: рабочая панель и безопасная отмена`. Отдельно проверить Return из списка, кнопку, double-click, логин, токен, главное окно в трее и отсутствие Enter в receiver (счётчик returnPressed равен 0).

- [ ] **Step 7: Негативная матрица.** В отдельных слотах закрыть receiver до отправки, активировать другое тестовое окно, вызвать lock в VerifyDialog/в таймере ожидания, удалить/изменить выбранную запись, удержать модификатор за бюджет. До отказа поместить вымышленный clipboard marker; убедиться, что предварительный отказ его не изменил. Для newer foreign publication поменять marker после собственной публикации и проверить, что clearIfOurs его сохраняет. Только с согласия/изоляцией.
- [ ] **Step 8: Ошибки публикации и reentrancy.** В отдельном тестовом процессе удержать OpenClipboard и проверить ограниченный отказ. Пока open() крутит вложенный цикл, запланировать app.lock(); публикация/отправка старой операции не происходят. Для имитации partial использовать InputSender из Task 2, не отправлять реальный неполный секрет в пользовательские окна. Одинаковый текст новой чужой публикации не должен обходить проверку owner/sequence.
- [ ] **Step 9: Запуск и отчёт.** Полный test_e2e запускается только после явного согласия на замену/очистку текущего буфера. Если этого согласия нет, матрица отмечается невыполненной, работа не объявляется полностью завершённой. Пропуск из-за отсутствия foreground в целевой интерактивной среде — незавершённая приёмка, а не зелёный результат. Не удалять старые тесты ради прохождения новых. Commit: `Тесты: полный маршрут быстрой вставки и отказы`.

### Task 6: Проверка всей ветки, документация и передача

**Files:** Modify README.md, docs/build/manual-checks.md, docs/security/assessment-2026-10-20.md, RESUME.MD, существующий .superpowers/sdd/2026-10-18-tokenbucket-v1/progress.md. Локальные Engram-артефакты остаются игнорируемыми.

**Interfaces:** Consumes результаты Tasks 1–5; produces отчёт с явными passed/failed/skipped, состоянием интерактивной приёмки и ограничениями.

- [ ] **Step 1: Проверить diff до широкого прогона.** Прочитать всех потребителей изменённых функций через rg/Engram. Убедиться в отсутствии новых зависимостей, миграции формата, автоматического повышения прав и сырых секретов в логах/аргументах. Проверить подписи UI, клавиатуру и обоих владельцев регистрации. Подключить verification-before-completion и code-review-and-quality на этапе выполнения.
- [ ] **Step 2: Свежая сборка и полный CTest.** После разрешения побочных действий выполнить `rtk proxy cmd /d /c tools\build.cmd`, затем `rtk proxy cmd /d /c tools\test.cmd`. Сохранить логи и список пропусков; не переиспользовать старые 15/15. Если разрешение на буфер отсутствует, выполнить безопасную часть и явно оставить полный прогон невыполненным.
- [ ] **Step 3: Ручная матрица.** Блокнот, редактор кода, браузер: записать приложение/версию, выбранное поле, способ отправки и результат; терминал — отдельная проверка Ctrl+V; повышенное окно — отказ. Проверить реальный Hello при доступности, масштаб 100/150/200%, обе темы, трэй и отмену. Недоступный Hello не записывать как прошедший тест.
- [ ] **Step 4: Обновить документы по фактам.** README: реальное сочетание/поля/ручной режим. manual-checks: матрица, логи и ограничения среды. assessment: убрать заявление об атомарном устранении TOCTOU, описать новый порядок публикации и остаточный риск. RESUME/ledger: завершённые задачи и обязательные оставшиеся проверки. Обновить Engram инкрементально по правилам проекта; новые результаты подтверждать в исходниках.
- [ ] **Step 5: Ревью и финальный коммит.** Ревью выполняется выбранным пользователем способом; найденные ошибки исправляются с целевой повторной проверкой. Проверить `rtk git diff --check`, `rtk git ls-files --eol`, identity и явный staged diff. Commit: `Документация: приёмка и ограничения быстрой вставки`. В финале перечислить новые коммиты, проверенные сценарии, skipped и остаточные риски. Push/package/release не выполнять.

## Самопроверка покрытия

| Раздел спецификации | Задачи |
| --- | --- |
| 1–3: цель, дефект, границы | Task 4, Task 5; общие ограничения |
| 4: UI, поля, клавиатура, lock | Task 1, Task 5 |
| 5: подтверждение, контекст, бюджет 1500 мс | Task 2, Task 5 |
| 6: identity, повышенные права, диагностика | Task 2, Task 5, Task 6 |
| 7: буфер, raw count, partial и copy-only | Task 2, Task 5 |
| 8: допустимые клавиши, сохранение/откат | Task 1, Task 3 |
| 9: границы компонентов и все потребители | Interfaces задач и Task 6 |
| 10: RED→GREEN, отдельный процесс, реальная матрица | Task 1–6; полная приёмка не заменяется skip |
| 11: источники и ограничения | Task 2, Task 6 |

Порядок строго последовательный: Task 1 → Task 2 → Task 3 → Task 4 (receiver и RED) → Task 5 (исправление и GREEN/негативная матрица) → Task 6. Receiver-коммит до исправления не объявляется готовым к выпуску. Нативное выполнение одним исполнителем предпочтительно из-за тесных связей регистрации, фокуса и контекста панели; итоговое отдельное ревью сохраняется.

## Перед началом исполнения

Пользователь рассматривает этот написанный план и выбирает способ: исполнение в текущей сессии с итоговым отдельным ревью либо последовательные задачи и ревью отдельными исполнителями. После выбора применяется соответственно executing-plans или subagent-driven-development. Разрешение на испытания с текущим системным буфером запрашивается отдельно непосредственно перед такими испытаниями; отсутствие разрешения не мешает безопасным подготовительным задачам, но не позволяет объявить полную приёмку пройденной.
