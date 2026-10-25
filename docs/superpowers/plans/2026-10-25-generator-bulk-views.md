# F1–F3: генератор, массовые действия, представления — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (или subagent-driven-development) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** настраиваемый генератор пароля, массовые действия над записями и сохранённые представления в зашифрованной модели CBOR v5.

**Architecture:** генератор — чистая функция в vault/ поверх libsodium, параметры в Preferences. Представления — новое поле модели (`views_`), сериализуемое обоими писателями в корне v5; переход v4→v5 переиспользует согласие и страховочную копию задачи 2 прошлого плана через одну константу версии писателя. Массовые действия — мутации существующего `MainWindow::commit()` над зафиксированными UUID.

**Tech Stack:** C++20, Qt 6.11 Widgets/Test, libsodium, MSVC/vcpkg (сборка в worktree `~/.codex/worktrees/tokenbucket-quick-paste`, VCPKG_MANIFEST_INSTALL=OFF).

**Spec:** [2026-10-25-generator-bulk-views-design.md](../specs/2026-10-25-generator-bulk-views-design.md)

**Status:** исполняется нативно (вопрос об утверждении плана остался без ответа — по мандату владельца взят рекомендованный вариант).

## Global Constraints

- Длина пароля 8–128, по умолчанию 24; группы A–Z, a–z, 0–9, символы `#!%+=-_@$*?.`; «исключать похожие» убирает `0 O 1 l I`; случайность только `randombytes_uniform`.
- Представлений ≤ 50, имя 1–64 печатных символа, уникально без учёта регистра; тегов ≤ 16; поиск ≤ 256 символов; тип −1..4; срок 0..2; сортировка 0..1; раздел 0..2.
- CBOR v5: корень `v`,`nodes`,`entries`,`views` в этом порядке; карта представления `name`,`lib`,`group`,`type`,`exp`,`sort`,`tags`,`q` в этом порядке. Чтение v1–v5; у v1–v4 корень строго 3 ключа.
- Все записи основного файла — через существующий `Application::storeCandidate`; старый файл (< 5) требует согласия и копии `vault.tb.before-upgrade-*`.
- Тексты и комментарии на русском; секреты не попадают в настройки, уведомления, подсказки и журналы.
- Сборка/тесты через `rtk proxy cmd //d //c "tools\\build.cmd"` и `tools\\test.cmd`; одиночный нативный прогон — `build\runtest.cmd <test> [slot]`. Полный прогон — с `TOKENBUCKET_TEST_USER_CLIPBOARD=1`.
- Identity `imtonyromeo`, без AI-атрибуции, без push.

## Review Focus

1. Длина ровно равна числу выбранных групп (например, 8 при 4 группах — допустимо; длина 3 при 4 группах невозможна из-за минимума 8) — каждая группа всё равно представлена: Task 1, тест на 1 000 генераций длины 8.
2. Представление ссылается на удалённую группу или тег — применение не падает и не показывает пустую «группу-призрак»: Task 3.
3. Массовое действие, когда за время диалога одна из записей удалена/изменена или сеанс заблокирован — ничего не меняется: Task 4.
4. Добавление тега к записи с 16 тегами в середине выборки — откат всех записей, сообщение с названием записи: Task 4.
5. Старый v4-файл + сохранение представления — сначала согласие, отказ оставляет файл побайтово прежним: Task 2/3.

---

### Task 1: F1 — параметры генератора

**Files:**
- Create: `src/vault/PasswordGenerator.h`, `src/vault/PasswordGenerator.cpp`
- Modify: `CMakeLists.txt` (источник в tokenbucket_core), `src/app/Preferences.h/.cpp`, `src/ui/EntryEditDialog.cpp`
- Test: `tests/test_crypto.cpp`, `tests/test_preferences.cpp`, `tests/test_ui.cpp`

**Interfaces — Produces:**
```cpp
namespace tb::crypto {
struct GeneratorOptions { int length=24; bool upper=true, lower=true, digits=true, symbols=true, excludeSimilar=true; bool operator==(const GeneratorOptions&) const = default; };
QString generatorError(const GeneratorOptions&); // "" — можно генерировать
QString generatePassword(const GeneratorOptions&); // пусто при ошибке
}
// prefs::Values += crypto::GeneratorOptions generator;  ключи generator/length, generator/groups ("ULDS"-подмножество), generator/excludeSimilar
```

- [x] **Step 1: RED.** В `test_crypto.cpp` слот `generatorHonoursOptions`:
```cpp
using tb::crypto::GeneratorOptions;
tb::crypto::init();
for (int i = 0; i < 1000; ++i) {
    const auto p = tb::crypto::generatePassword({8, true, true, true, true, true});
    QCOMPARE(p.size(), 8);
    QVERIFY(std::any_of(p.cbegin(), p.cend(), [](QChar c){ return c.isUpper(); }));
    QVERIFY(std::any_of(p.cbegin(), p.cend(), [](QChar c){ return c.isLower(); }));
    QVERIFY(std::any_of(p.cbegin(), p.cend(), [](QChar c){ return c.isDigit(); }));
    QVERIFY(std::any_of(p.cbegin(), p.cend(), [](QChar c){ return QStringLiteral("#!%+=-_@$*?.").contains(c); }));
    for (QChar c : QStringLiteral("0O1lI")) QVERIFY(!p.contains(c));
}
QCOMPARE(tb::crypto::generatePassword({128, false, false, true, false, false}).size(), 128);
QVERIFY(!tb::crypto::generatorError({24, false, false, false, false, true}).isEmpty());
QVERIFY(tb::crypto::generatePassword({24, false, false, false, false, true}).isEmpty());
QVERIFY(!tb::crypto::generatorError({7, true, true, true, true, true}).isEmpty());
QVERIFY(!tb::crypto::generatorError({129, true, true, true, true, true}).isEmpty());
```
В `test_preferences.cpp` слот `generatorOptionsRoundtripAndFallback`: записать `generator/length=40`, `generator/groups=LD`, `generator/excludeSimilar=false` → `read()` даёт {40,false,true,true,false,false}; `generator/length=500` и `generator/groups=XYZ` → значения по умолчанию.
- [x] **Step 2: Run** `rtk proxy cmd //d //c "tools\\build.cmd"` → ошибка компиляции (нет заголовка) — затем после объявлений поведенческий RED.
- [x] **Step 3: Реализация** `PasswordGenerator.cpp`:
```cpp
namespace tb::crypto {
namespace {
QString filtered(QString set, bool exclude) { if (exclude) for (QChar c : QStringLiteral("0O1lI")) set.remove(c); return set; }
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
    if (o.symbols) groups << QStringLiteral("#!%+=-_@$*?.");
    const QString all = groups.join(QString());
    QString out;
    for (const auto& g : groups) out += pick(g);          // по знаку каждой группы
    while (out.size() < o.length) out += pick(all);
    for (int i = int(out.size()) - 1; i > 0; --i)         // Фишер–Йейтс тем же RNG
        std::swap(out[i], out[int(randombytes_uniform(uint(i + 1)))]);
    return out;
}
}
```
`Preferences`: поле `crypto::GeneratorOptions generator;` в `Values`; `read()` — `length` через `integer()` с проверкой 8..128, `groups` — строка только из букв `U L D S` без повторов и непустая, иначе умолчание; `excludeSimilar` через `boolean()`. `validate()` возвращает `crypto::generatorError(v.generator)`.
`EntryEditDialog`: рядом с `generate` — `QToolButton` `generatorOptions` (popupMode InstantPopup) с меню: `QWidgetAction` со `QSpinBox` `generatorLength` (8..128) и checkable-действиями `generatorUpper/Lower/Digits/Symbols/Similar`. Обработчик `generate` строит `GeneratorOptions` из контролов, при ошибке пишет её через `showError()`, иначе `password_->setText(generatePassword(o))` и сохраняет параметры `prefs::writeValue` в `QSettings` по умолчанию (ключи из Interfaces). Начальные значения — `prefs::read(QSettings())`.
- [x] **Step 4: GREEN** — `build\runtest.cmd test_crypto`, `test_preferences`; в `test_ui` расширить `passwordGeneratorProducesLongDistinctPasswords`: выставить `generatorLength=40`, снять `generatorSymbols`, нажать `generatePassword` → длина 40, нет символов из набора; снять все группы → пароль не изменился, видна ошибка. Полный набор.
- [x] **Step 5: Commit** `Configure password generator length and character groups`.

### Task 2: F3 — модель и CBOR v5

**Files:** Modify `src/model/Types.h`, `src/model/VaultModel.h/.cpp`, `src/app/Application.cpp/.h`, `src/ui/MainWindow.cpp` (тексты «v4»), `docs/structured-storage-format.md`. Test: `tests/test_model.cpp`, `tests/test_app.cpp`.

**Interfaces — Produces:**
```cpp
namespace tb::model {
struct SavedView { QString name; int library=0; Id group; int type=-1, expiry=0, sort=0; QStringList tags; QString search; bool operator==(const SavedView&) const = default; };
}
// VaultModel:
static constexpr int CurrentVersion = 5, MaxViews = 50, MaxViewName = 64, MaxViewSearch = 256;
const QList<SavedView>& views() const;
QString setViews(const QList<SavedView>&); // "" — успех; транзакционно
static QString viewError(const SavedView&);
```
- [x] **Step 1: RED** в `test_model.cpp`:
```cpp
void savedViewsRoundtripV5() {
    VaultModel m; const auto g = QUuid::createUuid(); QVERIFY(m.addNode(g, {}, "Demo").isEmpty());
    SavedView v; v.name = "Истекают в CI"; v.group = g; v.type = 3; v.expiry = 1; v.sort = 1; v.tags = {"ci"}; v.search = "ip";
    QVERIFY(m.setViews({v}).isEmpty());
    QCOMPARE(QByteArray(m.toCborSecure().data(), qsizetype(m.toCborSecure().size())), m.toCbor());
    VaultModel r; QString err; QCOMPARE(r.fromCbor(m.toCbor(), &err), VaultModel::ParseStatus::Ok);
    QCOMPARE(r.views(), QList<SavedView>{v}); QCOMPARE(r.decodedVersion(), 5);
    QVERIFY(!m.setViews({v, v}).isEmpty());                       // дубликат имени
    SavedView bad = v; bad.name.clear(); QVERIFY(!m.setViews({bad}).isEmpty());
    bad = v; bad.type = 7; QVERIFY(!m.setViews({bad}).isEmpty());
    QCOMPARE(m.views(), QList<SavedView>{v});                      // отказ не меняет модель
}
```
Плюс: v4-фикстура (корень из 3 ключей с `v=4`) читается, `views()` пуст, `decodedVersion()==4`; v4 c ключом `views` → Bad; v5 без `views` → Bad; v5 `views` с не-картой/лишним ключом/51 элементом → Bad, прежняя модель не изменена. Обновить существующие проверки `decodedVersion()==4` новой модели на `CurrentVersion`.
В `test_app.cpp`: v4-файл, созданный `packPassword` из `QCborMap{v:4,nodes,entries}` → `schemaUpgradeRequired()`; `save` без согласия — отказ и байты прежние; после `approveSchemaUpgrade` — сохранение, копия равна прежним байтам, повторный `unlock` → `decodedVersion()==5`.
- [x] **Step 2: Run** `build\runtest.cmd test_model` / `test_app` → компиляция, затем RED на `version > 4`.
- [x] **Step 3: Реализация.** `toCbor()`: `root[u"v"_s]=CurrentVersion;` после `entries` — `QCborArray vs; for (const auto& v : views_) { QCborMap m; m[u"name"_s]=v.name; m[u"lib"_s]=v.library; m[u"group"_s]=v.group.isNull()?QString():idStr(v.group); m[u"type"_s]=v.type; m[u"exp"_s]=v.expiry; m[u"sort"_s]=v.sort; QCborArray t; for (auto& s : v.tags) t << s; m[u"tags"_s]=t; m[u"q"_s]=v.search; vs << m; } root[u"views"_s]=vs;`. `encodeTo()`: `beginMap(4)`, `w.uint(CurrentVersion)`, затем `w.text(u"views"); w.beginArray(views_.size());` и для каждой — `beginMap(8)` в том же порядке; `type` пишется `w.integer(v.type)` (знаковый, как sourceLine). `fromCbor()`: `version` в 1..5; `root.size() != (version >= 5 ? 4 : 3)` → bad; для v5 разбор `views` строго (карта ровно из 8 ключей, типы text/uint/text/int/uint/uint/array of text/text, `QUuid` группы валиден или пусто — несуществующая группа допустима), затем `viewError()` и уникальность имён; результат кладётся в `fresh.views_`. `clear()` очищает `views_` и ставит `decodedVersion_=CurrentVersion`. `setViews()`: проверка каждого + уникальность (`compare(..., Qt::CaseInsensitive)`) + `MaxViews`, теги через `normalizeTags`, затем присваивание. `removeNode` не трогает представления (сброс — при применении).
`Application`: заменить `persistedVersion_<4` (2 места) и `persistedVersion_=4` на `model::VaultModel::CurrentVersion`. `MainWindow`: в тексте согласия и экспорта `v4` → `QStringLiteral("v%1").arg(model::VaultModel::CurrentVersion)`. `docs/structured-storage-format.md`: раздел «CBOR v5: представления».
- [x] **Step 4: GREEN** test_model, test_app, test_storage_ui (в `legacyWriterUiKeepsOrUpgradesAtomically`/`legacyExportIsV4…` ожидания версии — `CurrentVersion`), полный набор.
- [x] **Step 5: Commit** `Store saved views inside the encrypted model as CBOR v5`.

### Task 3: F3 — панель представлений

**Files:** Modify `src/ui/MainWindow.h/.cpp`. Test: `tests/test_storage_ui.cpp`.

**Interfaces — Consumes:** `SavedView`, `VaultModel::views()/setViews()`. **Produces:** `QListWidget` `savedViews`, кнопка `saveView`; публичные для тестов `MainWindow::saveCurrentView(const QString& name)` → bool и `applyView(const QString& name)` → bool.

- [x] **Step 1: RED** `savedViewsCaptureApplyAndSurviveLock`: временное хранилище, группа Demo, записи с тегом `ci` и без; выставить `typeFilter` на PlainValue, чип `ci`, поиск `ip`; `saveCurrentView("CI")` → `app.model()->views().size()==1`; сбросить фильтры; `applyView("CI")` → три фильтра восстановлены, список содержит только подходящие записи; удалить группу и тег, `applyView("CI")` → true, фильтр группы снят, в строке состояния «Группа представления удалена», чип не выбран; `app.lock()` → `savedViews` пуст; разблокировка → представление на месте. Отдельный слот `savedViewOnLegacyFileAsksConsent`: v4-файл, отказ в `schemaUpgradeConfirmation` → `views()` пуст и байты прежние.
- [x] **Step 2: Run** `build\runtest.cmd test_storage_ui savedViewsCaptureApplyAndSurviveLock` → нет методов/виджетов.
- [x] **Step 3: Реализация.** Под кнопками библиотеки: `QLabel("Представления")`, `QListWidget savedViews_` (objectName `savedViews`, клик → `applyView(text)`), `QPushButton saveView_` «Сохранить текущее…» (QInputDialog имени → `saveCurrentView`). Контекстное меню списка: «Переименовать…», «Обновить из текущего», «Удалить» — все через `commit([&](model::VaultModel& m){ auto list = m.views(); …; return m.setViews(list); })`. `captureView()` собирает `SavedView` из `libraryView_` (Group → `group = selectedNode()`, `library = 0`), `typeFilter_->currentData()`, `expiryFilter_`, `sortMode_`, `selectedTags_`, `search_->text()`. `applyView()`: группа существует → выбрать узел и `LibraryView::Group`, иначе `setLibraryView(library)` и сообщение при непустой удалённой группе; `typeFilter_->setCurrentIndex(typeFilter_->findData(type))`; фильтры; `selectedTags_` = пересечение с `allTags()`; `search_->setText`; `refillTagBar(); refillList();`. `refreshAll()` перезаполняет `savedViews_` из `views()` (пусто при не-Ready).
- [x] **Step 4: GREEN** test_storage_ui; снимок 420×480 светлая/тёмная главного окна с двумя представлениями в `registryAndDocumentLayouts`-подобном слоте `savedViewsFitNarrowWindow` (кнопка `saveView` видима в пределах окна). Полный набор.
- [x] **Step 5: Commit** `Save and apply filter views from the library panel`.

### Task 4: F2 — массовые действия

**Files:** Modify `src/ui/MainWindow.h/.cpp`, `src/ui/EntryCard.h/.cpp`. Test: `tests/test_ui.cpp`.

**Interfaces — Produces:** `QList<model::Id> MainWindow::selectedEntries() const`; публичные `bulkMove(model::Id group)`, `bulkAddTag(const QString&)`, `bulkRemoveTag(const QString&)`, `bulkDelete()` → bool; `EntryCard::showSelection(int count)`.

- [ ] **Step 1: RED** в `test_ui.cpp` слот `bulkActionsAreAtomic`: три записи в группе A, группа B. Выделить две (`entriesList` ExtendedSelection, `setSelected`); карточка `entryDetails` пуста, заголовок «Выбрано: 2 записи». `bulkMove(B)` → обе в B, третья в A, одно сохранение (сравнить файл до/после: изменился один раз — `QSignalSpy` на `Application::stateChanged` не нужен; проверить `groupId`). Одной записи выставить 16 тегов; `bulkAddTag("new")` → false, ни у одной записи нет `new`, файл прежний. `bulkRemoveTag("old")` снимает у обеих. `bulkDelete` с драйвером: QMessageBox «Нет» → обе на месте; «Да» → удалены. Слот `bulkDeleteAsksOwnerForProtected`: одна из выбранных reverify, драйвер отменяет VerifyDialog → ничего не удалено; подтверждает → удалены. Слот `bulkActionCanceledByLock`: драйвер вызывает `app.lock()` в диалоге группы → файл прежний.
- [ ] **Step 2: Run** `build\runtest.cmd test_ui bulkActionsAreAtomic` → нет методов.
- [ ] **Step 3: Реализация.** `list_->setSelectionMode(QAbstractItemView::ExtendedSelection)`. `selectedEntries()` — UUID из `list_->selectedItems()`. Общий шаблон:
```cpp
bool MainWindow::bulkApply(const QList<model::Id>& ids, quint64 generation,
                           const std::function<QString(model::VaultModel&, model::Entry&)>& change) {
    if (!ready() || app_.generation() != generation) return false;
    for (auto id : ids) if (!app_.model()->entry(id)) { report(QStringLiteral("Выбранные записи изменились. Повторите действие.")); return false; }
    return commit([&](model::VaultModel& m) {
        for (auto id : ids) {
            auto e = *m.entry(id); if (const auto error = change(m, e); !error.isEmpty()) return QStringLiteral("%1: %2").arg(e.title, error);
            e.modified = QDateTime::currentDateTimeUtc();
            if (const auto error = m.setEntry(e); !error.isEmpty()) return QStringLiteral("%1: %2").arg(e.title, error);
        }
        return QString();
    });
}
```
`bulkMove`: `e.groupId = group`. `bulkAddTag`: `e.tags << tag` (лимит проверит `setEntry`). `bulkRemoveTag`: удалить без учёта регистра. `bulkDelete`: если есть reverify — `VerifyDialog::verify(app_, this, "массовое удаление")`; затем `QMessageBox::question` «Удалить N записей?» с No по умолчанию; `commit` с `removeEntry` для каждой. Пункты меню «Записи» и контекстного меню списка при `selectedEntries().size() > 1`: «Переместить в группу…» (QInputDialog::getItem по `groupPath()` всех узлов), «Добавить тег…» (getText с автодополнением `allTags()`), «Снять тег…» (getItem по объединению тегов выбранных), «Удалить N записей…». Каждое снимает `ids`/`generation` до диалога. `EntryCard::showSelection(n)`: очищает значение, скрывает все кнопки выдачи, заголовок «Выбрано: %1 записей»; вызывается из `showMetadata` при `selectedItems().size() > 1`. Действия копирования/показа/вставки при множественном выборе неактивны (`canExportEnv`, копирование — проверка `selectedItems().size()==1`).
- [ ] **Step 4: GREEN** test_ui, test_storage_ui, test_e2e; полный набор.
- [ ] **Step 5: Commit** `Move, tag and delete several entries in one atomic save`.

### Task 5: Приёмка, обзор, поставка

**Files:** `docs/testing/2026-10-25-generator-bulk-views-acceptance.md`, `docs/build/2026-10-25-generator-bulk-views-release.md`, `RESUME.MD`, `README.md` (возможности, формат v5).

- [ ] **Step 1:** Сопоставить разделы 2–6 спецификации с тестами задач 1–4.
- [ ] **Step 2:** Release build + полный CTest с `TOKENBUCKET_TEST_USER_CLIPBOARD=1`; при падении — изолированный перепрогон и классификация.
- [ ] **Step 3:** Обзор всей ветки (агент при доступности, иначе вручную по Review Focus); Critical/Important — RED→GREEN.
- [ ] **Step 4:** Пересобрать граф `engram update src --scope all --no-description --no-label --no-cluster`.
- [ ] **Step 5:** `tools\package-windows.cmd` в worktree, dumpbin-проверка зависимостей, перенос в `D:/Sources/gitverse/TokenBucket/dist/` с сохранением прежней сборки и сверкой SHA-256, коммит отчётов в main, worktree — fast-forward до main.

## Покрытие спецификации

| Раздел | Задача |
| --- | --- |
| §2 генератор | 1 |
| §3 массовые действия | 4 |
| §4 представления, v5, согласие | 2, 3 |
| §5 компоненты | 1–4 |
| §6 приёмка | 1–5 |
