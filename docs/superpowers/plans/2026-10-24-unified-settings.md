# TokenBucket: Unified Settings Implementation Plan

> **Исполнение:** последовательно в текущем чате, обязательный навык executing-plans. После обоих блоков — один свежий независимый обзор всей ветки. Пункты ниже отмечаются только по фактическому результату.

**Goal:** единое рабочее окно предпочтений с проверяемым сохранением, настоящими таймерами, полем вставки по умолчанию и поведением закрытия F5.

**Architecture:** существующие владельцы поведения сохраняются; SettingsDialog редактирует черновик, MainWindow сохраняет и применяет изменения. Главный цикл выделяется из main в вызываемый SessionFlow, чтобы проверить скрытое заблокированное состояние реальными диалогами. Восстановление F4 — отдельный связанный [план](2026-10-24-backup-recovery.md), который выполняется после задачи A6 и до итоговой задачи A7.

**Tech Stack:** C++20, установленный Qt 6.11.2 Widgets/Concurrent/Test, QSettings, существующие FileOps/libsodium, MSVC/vcpkg; без новых зависимостей.

**Spec:** [утверждённая спецификация](../specs/2026-10-24-unified-settings-design.md), база реализации `146f393` поверх main `2328280`.

**Статус:** исполнение подтверждено владельцем 2026-10-24 командой «продолжай план»; работа ведётся последовательно в текущем чате. Общая готовность объявляется после A1–A7 и B1–B3, не после появления диалога.

## Global Constraints

- Пять разделов: «Общие», «Безопасность», «Быстрая вставка», «Выдача секретов», «Резервные копии»; F1–F3 не входят.
- Буфер: 10/20/30/60 секунд, по умолчанию 20; показ: 5/10/15/30 секунд, по умолчанию 15; автоблокировка: 1–60 минут, по умолчанию 5.
- Новые ключи: `closeBehavior`, `secretRevealSeconds`, `clipboardClearSeconds`, `quickPasteDefaultField`; существующие ключи и значения сохраняются. Автозапуск остаётся в HKCU Run.
- Проверки получателя, повторная проверка, блокировка сеанса, запрет автоматического копирования при отказе и ограничения SendInput не выключаются.
- Отмена до сохранения не меняет состояние; частичный результат сохранения называется явно. Отдельные опасные действия требуют чистого черновика и собственного подтверждения.
- Реальные настройки, автозапуск, vault и чужие переменные владельца не меняются проверками. Буфер — только с уже данным разрешением на вымышленные данные в выделенных окнах.
- UTF-8/LF; .cmd/.bat — CRLF. Команды Git/build/test/search через RTK; исходные проверки не ослабляются.
- Поставка: `D:/Sources/gitverse/TokenBucket/dist/TokenBucket/`; ZIP `D:/Sources/gitverse/TokenBucket/dist/TokenBucket-windows-x64.zip`; проверенная предыдущая сборка сохраняется.
- Локальная ветка `codex/unified-settings`, существующий worktree. Push и операции с сертификатами не входят.

## Review Focus

1. Ошибка записи и повтор после её устранения: не применять закэшированный черновик позднее и не сообщать успех по старому QSettings::status — тест A1/A4.
2. Неизвестное состояние автозапуска и неизменённый флажок: не подменять ошибку значением false и не перезаписывать HKCU Run — A1/A4.
3. Ручной выбор поля и обновление поиска той же записи: не сбрасывать выбор и не подставлять пароль вместо пустого логина — A2.
4. Lock-hide/--tray, затем активация и выход из трея: не открывать unlock самопроизвольно и не оставлять недоступный процесс без трея — A5/A6.
5. Изменение/блокировка во время восстановления: не принять старую модель, не потерять исходный main и не объявить совершённую замену отменённой — B1/B2/B3.

## Файлы и обязанности

| Файлы | Обязанность |
| --- | --- |
| новые `src/app/Preferences.h/.cpp` | Типизированный снимок, нормализация/валидация, проверенная запись одного предпочтения |
| новые `src/ui/SettingsDialog.h/.cpp` | Пять страниц, черновик, ошибки, доступность отдельных действий |
| новые `src/ui/SessionFlow.h/.cpp` | Перенесённый реальный цикл Setup/Unlock/Ready и ожидание открытия из трея |
| `src/ui/MainWindow.h/.cpp`, `src/main.cpp` | Применение предпочтений, маршрутизация настроек/закрытия, связывание владельцев |
| `src/ui/EntryCard.h/.cpp`, `src/platform/ClipboardGuard.h/.cpp` | Действующие сроки показа/очистки |
| `src/ui/MiniPanel.h/.cpp`, `src/platform/QuickPasteWin.h`, `src/platform/quickpaste_win.cpp` | Начальное поле и временная приостановка выдачи |
| `src/ui/TrayController.h/.cpp`, `src/ui/Theme.h/.cpp`, `src/platform/EnvOps.h`, `src/platform/envops_win.cpp`, `src/ui/EnvExportDialog.h/.cpp` | Общий источник предпочтений и проверенные системные операции |
| `src/app/Application.h/.cpp` | Несекретная информация о копии; восстановление расширяется планом B |
| новые `tests/test_preferences.cpp`, `tests/test_settings.cpp`, `tests/test_session.cpp` | Настройки, реальное окно и жизненный цикл |
| существующие `tests/test_ui.cpp`, `test_clipboard.cpp`, `test_paste_win.cpp`, `test_env_win.cpp`, `test_e2e.cpp` | Регрессии прежних владельцев; временные настройки вместо общего пользовательского профиля |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | Новые исходники и тестовые цели, без ослабления платформ/порогов |

## Общий цикл проверки задач

В каждой задаче сначала записать тест, построить цель и подтвердить RED по ожидаемой причине; после минимального изменения повторить и получить GREEN. Ошибка окружения/сборки не считается пройденным поведенческим тестом. Новые декларации могут потребоваться для сборки теста нового API; его поведенческий RED всё равно фиксируется до завершения реализации. Журналы сохранять в игнорируемом `.superpowers/unified-settings/`.

```powershell
rtk proxy cmd /d /c "tools\build.cmd"
rtk proxy cmd /d /c "tools\test.cmd -R test_preferences --output-on-failure"
```

Для других целей ниже заменить только имя после `-R`. Не соединять регулярные выражения через `|` внутри cmd. Тестовые отчёты: `build/tests/test_*.txt`; отсутствие failed/skipped проверяется отдельно от кода выхода CTest. Существующий локальный cache с `VCPKG_MANIFEST_INSTALL=OFF` сохраняется, зависимости повторно не устанавливаются.

В test_settings.cpp модальные проверки используют локальный helper, чтобы assert внутри callback не оставлял окно открытым навсегда:

```cpp
static void withSettingsDialog(ui::MainWindow& window,
                               const std::function<void(QDialog&)>& action) {
    QTimer::singleShot(0, &window, [action] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QTimer::singleShot(3000, dialog, &QDialog::reject);
        action(*dialog);
    });
    window.showSettings();
}
```

Каждая fixture создаёт QTemporaryDir, Application с `valid-password`, непустой узел/запись с вымышленными значениями и два INI-объекта preferences/run. Типы и методы принадлежат настоящему продукту; тестовый helper только заполняет UI и ограничивает ожидание.

### Task 1: A1. Снимок предпочтений и проверяемая запись

**Files:** создать `src/app/Preferences.h/.cpp`, `tests/test_preferences.cpp`; изменить `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/platform/EnvOps.h`, `src/platform/envops_win.cpp`, `src/ui/Theme.h/.cpp`, `src/ui/TrayController.h/.cpp`, `tests/test_ui.cpp`, `tests/test_env_win.cpp`.

**Interfaces:**

```cpp
namespace tb::prefs {
enum class CloseBehavior { Hide, LockHide, Exit };
struct Values {
    QString theme = QStringLiteral("system");
    bool notifications = true, clearEnvironmentOnExit = false;
    int idleMinutes = 5, revealSeconds = 15, clipboardSeconds = 20;
    QKeySequence hotkey = QKeySequence(QStringLiteral("Ctrl+Alt+Space"));
    paste::Field defaultField = paste::Field::Secret;
    CloseBehavior closeBehavior = CloseBehavior::Hide;
    std::optional<bool> autostart;
    bool operator==(const Values&) const = default;
};
struct ApplyResult { QStringList appliedKeys; QString failedKey, error; };
Values read(QSettings& settings);
QString validate(const Values& values);
bool writeValue(QSettings& settings, const QString& key,
                const QVariant& value, QString* error);
}
```

`read` не читает системный Run и не записывает значения: autostart заполняет вызывающий код. В EnvOps добавить перегрузки `std::optional<bool> autostart::enabled(QSettings& run, QString* error)` и `bool autostart::setEnabled(bool on, QSettings& run, QString* error)`. Прежние функции остаются входами для настоящего Run и делегируют тем же операциям. Новый Qt-адаптер использует значение `TokenBucket`, прежний commandForTest и проверку sync/status; тесты передают INI, а не настоящий Run.

- [ ] **1. RED:** добавить defaults/legacy/invalid/roundtrip и write-failure/retry. Пример независимых ожиданий:

```cpp
QTemporaryDir dir;
QSettings s(dir.filePath("prefs.ini"), QSettings::IniFormat);
s.setValue("idleTimeoutMinutes", 999);
s.setValue("secretRevealSeconds", 7);
s.setValue("quickPasteDefaultField", "unknown");
const auto value = prefs::read(s);
QCOMPARE(value.idleMinutes, 60);
QCOMPARE(value.revealSeconds, 15);
QCOMPARE(value.defaultField, paste::Field::Secret);
QVERIFY(!value.autostart.has_value());
```

- [ ] **2. Проверить RED:** test_preferences; дополнительно в существующем test_ui установить новый срок через уже имеющийся uiPreferences_ и проверить реальный таймер карточки — до A2 он остаётся 15000 вместо ожидаемого 5000. Этот RED не исправлять изменением ожидания.
- [ ] **3. Реализация:** нормализовать перечисления и сроки по таблице спецификации; недопустимый сохранённый hotkey не заменять другим. `validate` отвергает значения вне разрешённого набора. `writeValue` сохраняет прежний QVariant/наличие ключа, пишет и sync, на отказе восстанавливает прежнее значение/отсутствие в кэше и проверяет откат. Для попытки использовать свежий корневой QSettings с теми же fileName/format/fallbacks; не доверять оставшемуся статусу прежней ошибки и не отключать atomic sync. Вызов не поддерживает произвольные текущие группы: все потребители передают корневой QSettings.

```cpp
QSettings attempt(settings.fileName(), settings.format());
attempt.setFallbacksEnabled(settings.fallbacksEnabled());
const bool existed = attempt.contains(key);
const QVariant before = attempt.value(key);
attempt.setValue(key, value);
attempt.sync();
if (attempt.status() == QSettings::NoError) return true;
if (existed) attempt.setValue(key, before); else attempt.remove(key);
attempt.sync();
// Отдельный свежий reader проверяет результат отката; false возвращается в любом случае.
```

При непроверяемом откате ошибка явно говорит о неизвестном состоянии хранения. Сверить поведение со встроенными исходниками Qt qsettings.cpp: статус хранит первую ошибку, а создание новой попытки не должно оставлять ошибочный черновик на последующий flush. Добавить проверку отказа, снятия препятствия и последующей корректной записи/перечитывания.

- [ ] **4. GREEN:** test_preferences и прежние theme/idle tests. Перевести затронутые функции темы/уведомлений/учёта EnvOps на необязательный `QSettings* preferences = nullptr`; nullptr сохраняет обычный профиль. Для функции записи возвращать bool/ошибку с прежними аргументами по умолчанию, чтобы проверить sync. `exportedNames/rememberExported/forgetExported/clearAllExported` используют один переданный источник; в тестах — временный INI и уникальные синтетические имена. Не очищать список переменных владельца.
- [ ] **5. Commit:** `Add validated preferences and checked persistence` — только перечисленные исходники/тесты, Git identity владельца, без атрибуции.

### Task 2: A2. Подключить сроки и начальное поле к настоящим владельцам

**Files:** EntryCard.*, ClipboardGuard.*, MiniPanel.*, QuickPasteWin.h/quickpaste_win.cpp, MainWindow.*, tests/test_ui.cpp, tests/test_clipboard.cpp, tests/test_paste_win.cpp.

**Interfaces:** `EntryCard::setRevealSeconds(int)`, `ClipboardGuard::setClearSeconds(int)`, `int ClipboardGuard::clearSeconds() const`, `MiniPanel::setDefaultField(paste::Field)`, `QuickPaste::setDefaultField(paste::Field)`, `QuickPaste::setSuspended(bool)`, `bool QuickPaste::suspended() const`. Все новые входы валидируют/нормализуют значения; произвольное число не выключает таймер. QuickPaste вызывает MiniPanel, не создаёт второго владельца буфера.

- [ ] **1. RED:** проверить actual timer/label и поведение копирования, а не только getters. В test_ui расширить сценарий A1:

```cpp
uiPreferences_.setValue("secretRevealSeconds", 5);
ui::MainWindow window(app, nullptr, &uiPreferences_);
window.card()->revealButton()->click();
QCOMPARE(window.card()->findChild<QTimer*>("secretHideTimer")->interval(), 5000);
QVERIFY(window.card()->revealButton()->text().contains("5"));
```

Перед примером создать временный парольный vault, группу и выделенную API-запись с `synthetic-secret`; выбор записи должен быть реальным, без вызова приватного метода. Новые UI-слоты используют собственный временный Application.

- [ ] **2. Проверить RED:** test_ui/test_clipboard; для MiniPanel — login по умолчанию, API fallback, пустой логин и сохранение ручного выбора после обновления поиска той же записи. В native test_paste_win добавить ситуацию suspended: реальный trigger не открывает панель и не меняет буфер.
- [ ] **3. Реализация:** оба фиксированных 15000 заменить одним действующим revealSeconds; подпись кнопки формировать из него. Setter скрывает показанный секрет. В ClipboardGuard новый срок используется при новой публикации; смена срока вызывает только clearIfOurs, не EmptyClipboard без проверки. Если срок прежний, setter не очищает буфер. В MainWindow убрать фиксированные «20 секунд» из событий и брать clearSeconds.

```cpp
void EntryCard::setRevealSeconds(int seconds) {
    if (seconds != 5 && seconds != 10 && seconds != 15 && seconds != 30) seconds = 15;
    if (hideTimer_.interval() != seconds * 1000) hideNowForTest();
    hideTimer_.setInterval(seconds * 1000);
    reveal_->setText(QStringLiteral("Показать на %1 секунд").arg(seconds));
}
```

При показе карточки `hideTimer_.start()` использует установленный interval, без второго литерала 15000. В ClipboardGuard добавить отдельное `int clearSeconds_ = 20`: интервал самого QTimer нельзя использовать как настройку, поскольку при повторе очистки он равен 500 мс. Новая публикация запускает `clearSeconds_ * 1000`, повторы сохраняют 500 мс.

MiniPanel хранит начальное поле и последнее выбранное UUID/поле для текущей операции; при смене записи применяет default, при сохранении UUID после refill — ручной выбор. Login у API меняется на Secret; пустой login credentials остаётся видимым и неактивным для отправки. `setSuspended(true)` отменяет открытую панель; и обработчик WM_HOTKEY, и ручной popupPanel идут через одну проверку suspended. Не блокировать повторное нажатие при обычной reverify вне настроек.

- [ ] **4. GREEN:** прежние проверки чужого буфера, отмены, focus/lock/reverify и новые случаи. Для буфера использовать явный тестовый opt-in; при уменьшении/увеличении срока старая собственная публикация очищается, чужая сохраняется, следующий срок действительно работает.
- [ ] **5. Commit:** `Apply configurable secret timers and paste field preference`.

### Task 3: A3. Рабочий диалог черновика и несекретные сведения о копии

**Files:** создать SettingsDialog.h/.cpp и test_settings.cpp, изменить Application.h/.cpp, CMakeLists.txt/tests/CMakeLists.txt, MainWindow.*.

**Interfaces:**

```cpp
// Внутри public Application, после определения Mode:
struct BackupInfo {
    enum class State { Missing, Available, Unavailable };
    State state = State::Missing;
    QString mainPath, backupPath;
    qint64 size = 0;
    QDateTime modified;
};
BackupInfo backupInfo() const;
```

Mode не читать из файла на странице: до строгой подготовки B2 он не нужен. Восстановление выбирает режим через Application::backupMode из B2, а не из QFileInfo в UI. `Available` означает лишь доступный обычный файл допустимого размера, а не проверенную AEAD.

```cpp
SettingsDialog(const prefs::Values& initial,
               const Application::BackupInfo& backup,
               const QString& registeredHotkey, QWidget* parent = nullptr);
prefs::Values draft() const;
void setApplyResult(const prefs::ApplyResult&, const prefs::Values& current);
// signals:
void saveRequested();
void changePasswordRequested();
void changeUnlockModeRequested();
void clearEnvironmentRequested();
void restoreBackupRequested();
```

В этой задаче вкладка копий показывает настоящие метаданные. Кнопка восстановления добавляется и подключается в B3; промежуточная сборка не выдаётся за завершённый F4 и не содержит неработающей кнопки.

- [ ] **1. RED:** в test_settings записать реальное открытие нового пункта меню. До реализации не находится QAction с objectName `openSettings`:

```cpp
ui::MainWindow window(app, nullptr, &preferences);
auto* action = window.findChild<QAction*>("openSettings");
QVERIFY(action);
QTimer::singleShot(0, &window, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    QVERIFY(dialog);
    dialog->findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0);
    dialog->reject();
});
action->trigger();
QCOMPARE(preferences.value("secretRevealSeconds", 15).toInt(), 15);
```

- [ ] **2. Проверить RED:** test_settings. Дополнить draft/cancel/Esc/cross/navigation/unknown-autostart/plaintext/lock и доступными именами. Перед тестом снять байтовый снимок INI; после отмены сравнить его и действующие интервалы/регистрацию. Редактирование не вызывает системные setters.
- [ ] **3. Реализация:** QDialog + QListWidget/QComboBox одной навигации, QStackedWidget страниц, отдельный footer. Ключи контролов совпадают с таблицей предпочтений: `theme`, `autostart`, `trayNotifications`, `closeBehavior`, `idleTimeoutMinutes`, `secretRevealSeconds`, `clipboardClearSeconds`, `hotkey`, `quickPasteDefaultField`, `env-clear-on-exit`; навигация `settingsSections/settingsSectionCombo`, ошибка `settingsError`. Ввод — QComboBox, QSpinBox, QCheckBox и QKeySequenceEdit. Unknown autostart недоступен с объяснением. Save испускает сигнал, а не принимает диалог до фактического результата.

```cpp
connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::saveRequested);
connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
// Размер: min(900, availableWidth-40); высота <= availableHeight-80;
// width < 720: верхний combo вместо бокового списка; footer вне scroll.
```

При dirty отдельные действия недоступны с пояснением; страницы не теряют значения. Метаданные копии доступны через Application::backupInfo без чтения содержимого в UI. Не запускать KDF ради отображения страницы. Переходы к способу разблокировки/паролю используют действующие защищённые сценарии.
- [ ] **4. GREEN:** test_settings, test_ui; обе темы, размеры 900 и 420 логических пикселей, скролл страницы и доступный footer. В settings-demo тестового EXE показывать только временный Application/INI и фиктивные записи.
- [ ] **5. Commit:** `Add unified settings dialog with staged edits`.

### Task 4: A4. Сохранение и интеграция системных предпочтений

**Files:** MainWindow.*, TrayController.*, EnvOps.h/envops_win.cpp, EnvExportDialog.*, Theme.*, test_settings.cpp/test_env_win.cpp/test_ui.cpp/test_e2e.cpp.

**Interfaces:** расширить MainWindow конструктор до `(Application&, QWidget* parent=nullptr, QSettings* preferences=nullptr, QSettings* runPreferences=nullptr)`, затем A5 добавит проверку доступности трея. Действующий корневой источник — preferences_. Поле `prefs::Values appliedPreferences_` хранит подтверждённое состояние. `showSettings()` — публичный slot; приватный `prefs::ApplyResult applyPreferences(const prefs::Values&)` возвращает точный результат. Действующие setters A2 вызываются только после подтверждения записи. Для настоящего Run MainWindow владеет собственным QSettings NativeFormat; тесты передают INI четвёртым аргументом.

- [ ] **1. RED:** через настоящее окно изменить срок, уведомления и сочетание, нажать Save, снова открыть и проверить фактическое состояние. Для fault использовать QSettings с существующим файлом вместо родительского каталога, не защищённые каталоги ОС. Автозапуск проверять через временный run.ini.

```cpp
QTemporaryDir dir;
QSettings preferences(dir.filePath("prefs.ini"), QSettings::IniFormat);
QSettings run(dir.filePath("run.ini"), QSettings::IniFormat);
ui::MainWindow window(app, nullptr, &preferences, &run);
withSettingsDialog(window, [](QDialog& dialog) {
    auto* autostart = dialog.findChild<QCheckBox*>("autostart");
    QVERIFY(autostart);
    autostart->setChecked(true);
    dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
});
QSettings reader(dir.filePath("run.ini"), QSettings::IniFormat);
QCOMPARE(reader.value("TokenBucket").toString(), platform::autostart::commandForTest());
```

- [ ] **2. Проверить RED:** test_settings/test_env_win. Таблица отказов: invalid draft до любого setter; занятый hotkey без остальных записей; отказ Run после успешной смены hotkey; отказ обычного preference после предшествующего успеха; повтор после устранения ошибки; неизменённые значения не пишутся.
- [ ] **3. Реализация:** порядок применения: общая validate → hotkey (если изменён) → autostart (если известен и изменён) → theme/notifications/idle/reveal/clipboard/defaultField/closeBehavior/envClearOnExit. При первом отказе stop; appliedKeys содержит только подтверждённые изменения. QSettings→runtime для каждого обычного поля идёт в таком порядке:

```cpp
if (!prefs::writeValue(*preferences_, key, value, &result.error)) {
    result.failedKey = key;
    return result;
}
result.appliedKeys.append(key);
// Для соответствующего ключа: setter владельца / idleTimeoutChanged / theme::apply.
```

При создании MainWindow прочитать переданный источник, заполнить appliedPreferences_, установить сроки/начальное поле и запустить `hotkey_->start(appliedPreferences_.hotkey, &error)` вместо неявного чтения глобального профиля через start(). Недопустимое или занятое сочетание отражается фактической ошибкой, не заменяется. Валидация на не-Windows не блокирует общие настройки из-за сохранённого неподдерживаемого сочетания; Windows-поля не записываются.

setApplyResult на ошибке перечитывает успешно применённые поля в baseline, сохраняет оставшийся черновик, показывает понятные названия применённых/неудачного параметров и остаётся открытым. Полный успех закрывает. Не показывать один общий «Сохранено» при частичном результате. На время showSettings — QuickPaste suspended с восстановлением прежнего состояния после закрытия, карточка скрывает показ. При lock диалог reject и позднее сохранение не начинается. Недоступные Windows-функции на другой платформе объясняются, не исполняются.

TrayController получает необязательный QSettings* в конструкторе и читает тот же источник в notify. EnvExportDialog получает такой же необязательный источник; checkbox очистки и MainWindow используют общий ключ. Для удаления окружения передавать только список из этого источника; выводить число оставшихся, не значения. Доступ к старым коротким действиям предпочтений заменить основным пунктом Ctrl+,; смену пароля/режима оставить самостоятельными защищёнными действиями.
- [ ] **4. GREEN:** test_settings/test_preferences/test_env_win/test_ui/test_paste_win. Нативный тест проверяет фактическую новую регистрацию и сохранность старой при конфликте. Прежние сценарии reverify/cancel не теряются. Перед закрытием диалога ни выбранная тема, ни автозапуск не меняются без Save.
- [ ] **5. Commit:** `Persist and apply unified settings with explicit failures`.

### Task 5: A5. Поведение закрытия F5

**Files:** MainWindow.*, test_settings.cpp/test_ui.cpp; граница с main задаётся сигналом и используется A6.

**Interfaces:** дополнительный последний аргумент MainWindow `std::function<bool()> trayAvailable = QSystemTrayIcon::isSystemTrayAvailable`; хранить эту проверку и вызывать при закрытии, чтобы учесть исчезновение трея. Новый сигнал `lockedHiddenRequested()`. Конструкторы с прежними аргументами остаются допустимыми.

В MainWindow хранить `std::function<bool()> trayAvailable_`; `<QSystemTrayIcon>` и `<functional>` включить в заголовок для объявленного значения по умолчанию.

- [ ] **1. RED:** data-driven тест трёх closeBehavior и явного requestExit; дополнительно отсутствие трея для каждого режима. Для теста hide передавать `[] { return true; }`, не зависеть от рабочего стола.

```cpp
preferences.setValue("closeBehavior", "lock-hide");
ui::MainWindow w(app, nullptr, &preferences, &run, [] { return true; });
QSignalSpy hidden(&w, &ui::MainWindow::lockedHiddenRequested);
w.show(); w.close();
QCOMPARE(hidden.size(), 1);
QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
QVERIFY(!w.isVisible());
```

- [ ] **2. Проверить RED:** test_settings, прежний closeToTrayAndExit обновить только для явного доступного трея. Нельзя убрать старую проверку сохранности Ready у режима hide.
- [ ] **3. Реализация:** requestExit всегда имеет приоритет. Без доступного трея — lock + обычный closing. Hide — только hide/ignore. LockHide — сначала lockedHiddenRequested (внешний цикл получает намерение до stateChanged), затем hide и lock; не emit closing как выход. Exit — прежний полноценный выход с lock. Выбор поведения берётся из последнего успешно сохранённого снимка.

```cpp
saveUiState();
if (exiting_ || !trayAvailable_() || appliedPreferences_.closeBehavior == prefs::CloseBehavior::Exit) {
    app_.lock(); QMainWindow::closeEvent(event); emit closing(); return;
}
if (appliedPreferences_.closeBehavior == prefs::CloseBehavior::LockHide)
    emit lockedHiddenRequested();
hide(); event->ignore();
if (appliedPreferences_.closeBehavior == prefs::CloseBehavior::LockHide) app_.lock();
```
- [ ] **4. GREEN:** проверить порядок lockedHiddenRequested→stateChanged; при выборе Exit из настроек не завершать процесс до последующего закрытия главного окна. Esc/крестик SettingsDialog не запускает политику MainWindow.
- [ ] **5. Commit:** `Honor window close preference without bypassing lock`.

### Task 6: A6. Проверяемый цикл сеанса и заблокированный трей

**Files:** создать SessionFlow.h/.cpp, test_session.cpp; изменить main.cpp, TrayController.cpp, CMakeLists.txt/tests/CMakeLists.txt и settings-demo в test_e2e.cpp.

**Interfaces:**

```cpp
namespace tb::ui {
int runSession(Application& core, QSettings& preferences, bool startHidden,
               QSettings* runPreferences = nullptr,
               std::function<bool()> trayAvailable = QSystemTrayIcon::isSystemTrayAvailable);
bool runVaultOperation(Application& core,
    const std::function<bool(void*, QString*)>& operation,
    QString* error, QWidget* parent = nullptr);
}
```

Перенести существующий run(progress) под именем runVaultOperation и цикл Setup/Unlock/Ready, IdleTimer/SessionWatcher/TrayController из main без переписывания KDF/Hello. Оба реальных вызывающих — цикл сеанса и восстановление B3 — используют этот progress/cancel. В main остаются CLI, путь данных, instance lock, создание Application/QSettings, стартовые ресурсы и прежняя единая cleanup-точка выхода. Новые скрытые тестовые флаги в производственный EXE не добавлять. В test_session временно выставить quitOnLastWindowClosed=false, как в настоящем приложении, и восстановить прежнее значение guard-ом после теста.

- [ ] **1. RED:** test_session вызывает тот же runSession на временном Application. QTimer с ограниченным watchdog закрывает настоящее MainWindow в режиме lock-hide, проверяет отсутствие видимого UnlockDialog, затем посылает настоящее showRequested тестовому TrayController и вводит `valid-password` в `unlockPassword`. После Ready вызывает requestExit. Наблюдать реальные gate и окна, не отдельную копию машины состояний.

```cpp
bool completedUnlockAndExplicitExit = false;
QTimer watchdog;
watchdog.setSingleShot(true);
watchdog.setInterval(15000);
connect(&watchdog, &QTimer::timeout, &core, [&] {
    if (auto* tray = core.findChild<ui::TrayController*>("sessionTray"))
        emit tray->quitRequested();
});
// Отдельные одноразовые QTimer ведут описанный маршрут; watchdog не заменяет assertions.
const int exitCode = ui::runSession(core, preferences, false, &run, [] { return true; });
QCOMPARE(exitCode, 0);
QVERIFY(completedUnlockAndExplicitExit);
```

- [ ] **2. Проверить RED:** test_session. Дополнительные маршруты: startHidden+NeedUnlock, startHidden+NeedSetup, выход до разблокировки, block сеанса при скрытом Ready, отсутствие трея. Каждый маршрут ограничен таймером, итоговые asserts выполняются после возврата.
- [ ] **3. Реализация:** хранить в реальном цикле `waitForTrayOpen` и `quitting`. LockHide выставляет первый флаг до lock. NeedUnlock при этом запускает локальный event loop ожидания только showRequested/quitRequested, не UnlockDialog; show сбрасывает флаг и переходит к прежней разблокировке. В NeedSetup первоначальный setup остаётся видимым. `--tray` с NeedUnlock использует тот же путь; без трея открывается обычная разблокировка. При quitting никакая следующая итерация не создаёт диалог. TrayController имеет родителя core и objectName sessionTray; на время ожидания callbacks не указывают на уже уничтоженный MainWindow.

```cpp
// waitLoop — QPointer<QEventLoop>; mainWindow — текущий живой MainWindow*.
connect(&tray, &TrayController::showRequested, &core, [&] {
    waitForTrayOpen = false;
    if (waitLoop) waitLoop->quit();
    if (mainWindow) { mainWindow->show(); mainWindow->raise(); mainWindow->activateWindow(); }
});
if (core.gate() == Application::Gate::NeedUnlock && waitForTrayOpen && trayAvailable()) {
    QEventLoop pending;
    waitLoop = &pending;
    pending.exec();
    waitLoop = nullptr;
    if (quitting) return 0;
    continue;
}
```

Обработчик quitRequested устанавливает quitting, завершает waitLoop, отвергает активный модальный диалог и при живом mainWindow вызывает requestExit; это не взаимоисключающие ветви. Иначе выход из трея во время настроек лишь закрыл бы диалог. При блокировке скрытого Ready также установить waitForTrayOpen до завершения цикла Ready; открытое восстановление B3, напротив, должно привести к видимой штатной разблокировке.
- [ ] **4. GREEN:** test_session (на Windows — native Qt), test_lock/test_e2e/test_settings. Проверить возвращение после отмены setup/unlock и действующий cleanup выхода. В settings-demo использовать эту же SessionFlow для маршрута трея, а не вторую реализацию.
- [ ] **5. Commit:** `Keep locked sessions hidden until tray activation`.

### Task 7: A7. Восстановление, общий обзор и поставка

**Files:** выполнить B1–B3 из [связанного плана](2026-10-24-backup-recovery.md); затем README.md, RESUME.MD, новый отчёт `docs/testing/2026-10-24-settings-acceptance.md` и `docs/build/2026-10-24-settings-release.md`.

- [ ] **1. Выполнить B1–B3:** готовность настройки не подменяет готовность восстановления; до них весь утверждённый этап не закрыт.
- [ ] **2. Полный GREEN:** Release build и bare `tools/test.cmd --output-on-failure` с тестовым opt-in буфера. После трёх новых целей ожидается 20 CTest-целей; ориентироваться на фактический список, не старые 17. Проверить каждый свежий QtTest-журнал, отдельно назвать failed/skipped. Foreground receiver можно один раз активировать только до начала маршрута; не менять фокус посреди маршрутов.
- [ ] **3. Независимый обзор:** один свежий reviewer всей ветки на наиболее способной доступной модели, с утверждённой спецификацией и обоими планами. Проверить утечки/сохранность копии/ошибки/QSettings/повторный вход/потерю доступности трея. Исправить подтверждённые Important/Critical, добавить регрессию и повторить затронутые проверки; после изменения продукта полный набор снова должен быть зелёным.
- [ ] **4. Нативная приёмка:** settings-demo на вымышленных данных, пять страниц, обе темы, 100/200%, 420×480, клавиатура и ошибки. Проверить save/reopen/cancel/partial failure, восстановление с отдельным подтверждением, lock-hide/--tray/show/quit. Не менять настоящий Run, чужое окружение и vault; реальный Hello не отмечать пройденным по mock-тесту.
- [ ] **5. Поставка:** записать точный исходный commit; локально включить завершённую ветку в main в рамках прежнего поручения. Собрать штатным package-windows.cmd в проверенном каталоге worktree, проверить ZIP/EXE/DLL и извлечённый синтетический запуск; перед заменой сохранить и сверить предыдущую сборку. Перенести пакет в постоянные пути, сверить каждый файл и SHA-256 ZIP. Никаких удалённых публикаций.

```powershell
rtk proxy cmd /d /c "tools\package-windows.cmd"
rtk proxy git diff --check
rtk proxy git log -1 --format='%h %an <%ae> | %cn <%ce>%n%B'
```

Перед командой с рекурсивным удалением в упаковщике проверить полный путь `dist/TokenBucket` внутри выбранного worktree и отсутствие reparse point. Заблокированные файлы не обходить завершением пользовательских процессов. В отчёте разделить свежий build/test/smoke и непроверенные реальные Hello/физический DPI; не приписывать старые 187 проверок новому состоянию.

## Покрытие спецификации и передача к исполнению

| Раздел спецификации | Задачи |
| --- | --- |
| 1–4: объём, совместимость, окно, General | A1, A3, A4, A5 |
| 5: таймеры и защита | A1, A2, A4 |
| 6: вставка | A2, A4 |
| 7: окружение и отдельные действия | A1, A3, A4, B3 |
| 8: backup/F4 | B1, B2, B3 |
| 9: ошибки/частичное сохранение | A1, A3, A4, B1, B2 |
| 10: владельцы/платформы | A1–A6, B1–B3 |
| 11: приёмка/поставка | A7 |

После рассмотрения обоих файлов сохранить выбранное исполнение в текущем чате. Код начинается после подтверждения, что план отражает утверждённую спецификацию; повторный выбор способа исполнения не нужен.
