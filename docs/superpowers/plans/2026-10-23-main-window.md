# TokenBucket — главное окно: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking. Выбран нативный метод выполнения в текущей сессии.

**Goal:** Реализовать утверждённое главное окно этапа 2а с библиотекой, читаемыми записями, защищённой карточкой и достоверными состояниями.

**Architecture:** Сохранить MainWindow, EntryCard, QListWidget, существующие QAction и пути сохранения/выдачи. Добавить только состояние библиотечного представления, пассивное чтение защитных таймеров и сохранение геометрии; не создавать новую модель хранилища или службу настроек.

**Tech Stack:** C++20, Qt Widgets 6.11.2, Fusion/QPalette, QtTest, CMake, Windows; существующие зависимости проекта.

**Spec:** [Утверждённая спецификация](../specs/2026-10-23-main-window-design.md), утверждение пользователя 2026-10-23. Макет: `docs/design/2026-10-22/01-main.png`.

Статус: план утверждён и реализован 2026-10-23; приёмка и её ограничения — docs/testing/2026-10-23-main-window-acceptance.md. Пользователь поручил слить результат в main. Checkout: `C:/Users/bboy2/.codex/worktrees/tokenbucket-quick-paste/TokenBucket`, ветка `codex/main-window-design`, продуктовая основа `a42f2c8`.

## Global Constraints

- «Используются обычные Qt Widgets, системная рамка окна и существующие меню. Новые зависимости, изменения контейнера, криптографии и миграция данных не нужны.»
- «Для большого списка используются стандартные элементы/делегат Qt; отдельный QWidget на каждую из 10 000 записей не создаётся.»
- «Этот тик не вызывает refreshAll/showEntry, не меняет выделение и не скрывает показанный секрет раньше его собственного таймера.»
- «Сохраняются только геометрия окна и состояние разделителей, в новых ключах `ui/mainGeometry` и `ui/mainSplitter`.»
- «Нынешняя карточка-диалог при ширине менее 720 сохраняется как совместимость, без отдельного мобильного интерфейса.»
- Все прежние проверки защиты, сохранения и отката остаются обязательными; изменение стартового представления требует адаптации ожиданий UI-тестов, а не удаления проверок.
- Исходный checkout, установленная программа, контейнер и криптография не меняются; push, упаковка и выпуск сюда не входят. Настройки как предпочтения и F1–F5 остаются следующими этапами.
- Буфер — только вымышленные значения в выделенных тестовых окнах по уже полученному разрешению. Терминал через Computer Use заблокирован политикой; обходов нет.

## Review Focus

1. Секундный индикатор не продлевает автоблокировку и не скрывает раскрытый секрет — задачи 1 и 4, `remainingReadDoesNotRestart`, `statusTickKeepsReveal`.
2. Библиотека без выбранной группы не назначает запись первому проекту молча — задача 2, `libraryRequiresDestination`.
3. Удаление/перемещение выбранного UUID и одинаковые заголовки не оставляют действия над старой записью — задача 2, `librarySelectionFollowsUuid`.
4. Чужой буфер и занятый буфер не изображаются успешно очищенными — задача 1, `snapshotTracksOwnershipAndRetry`.
5. Повреждённые настройки, исчезнувший экран и отказ записи не повреждают хранилище — задача 3, `mainGeometryFallbackAndWriteFailure`.

## Карта файлов и интерфейсов

| Файлы | Ответственность |
|---|---|
| `src/app/IdleTimer.h/.cpp`, `tests/test_lock.cpp` | Только чтение остатка, сохранение действующего механизма активности |
| `src/platform/ClipboardGuard.h/.cpp`, `tests/test_clipboard.cpp` | Снимок метаданных собственной публикации и ожидания очистки |
| `src/ui/MainWindow.h/.cpp`, `tests/test_ui.cpp` | Библиотека, список, компоновка, геометрия, пассивные статусы |
| `src/ui/EntryEditDialog.cpp`, `tests/test_ui.cpp` | Явное назначение группы новой записи |
| `src/ui/EntryCard.h/.cpp`, `src/ui/Theme.cpp`, `tests/test_ui.cpp` | Иерархия карточки, маршрутизация действий, акцент и доступность |
| `src/main.cpp` | Привязка существующего IdleTimer к окну |
| `tests/test_e2e.cpp`, `tests/CMakeLists.txt` | Только при необходимости изолированного предпросмотра; сохранение существующих тестовых целей |
| `docs/testing/2026-10-23-main-window-acceptance.md`, `RESUME.MD` | Фактическая матрица приёмки и следующий шаг |

Новые публичные сигнатуры: `int IdleTimer::remainingMs() const`, `ClipboardGuard::Snapshot ClipboardGuard::snapshot() const`, `void MainWindow::setIdleTimer(IdleTimer*)`. `Snapshot` содержит `bool owned`, `bool clearPending`, `int remainingMs` (по умолчанию false/false/-1). Указатель таймера хранится как QPointer, владение остаётся в main.

Для изолированных INI расширить конструктор совместимо: `explicit MainWindow(Application&, QWidget* parent = nullptr, QSettings* uiPreferences = nullptr)`. Время жизни переданного QSettings больше времени жизни окна; при nullptr окно владеет обычным QSettings. Этот параметр обслуживает только два новых ключа геометрии. Существующие предпочтения сохраняют свои текущие маршруты.

Общий цикл каждой задачи: сначала добавить указанные QtTest-слоты и проверить RED, затем изменить код и проверить GREEN. Команды выполняются из checkout выше через `rtk`. Полный build: `rtk proxy cmd /d /c tools\build.cmd`; выбранные цели: `rtk proxy cmd /d /c tools\test.cmd -R "^(test_lock|test_clipboard)$" --output-on-failure`. QtTest-результаты читать из `build/tests/test_*.txt`, не считать пустой GUI stdout доказательством успеха. Ошибка компиляции на отсутствующей новой сигнатуре допустима как первоначальный RED; после её появления требуются поведенческие проверки.

## Task 1: Пассивные снимки таймеров и буфера

**Files:** Modify `src/app/IdleTimer.h/.cpp`, `src/platform/ClipboardGuard.h/.cpp`; Test `tests/test_lock.cpp`, `tests/test_clipboard.cpp`.

**Interfaces:** Consumes существующие `poke()`, `setActive(bool)`, `setIntervalMs(int)`, `copySecret(const QString&, QString*)`, `clearIfOurs()`. Produces три сигнатуры снимков из карты выше; setIdleTimer появляется только в задаче 4.

- [x] Добавить в TestLock слот ниже. Дополнить проверкой setIntervalMs(200), poke, неактивного состояния и таймаута без видимого окна; чтение после timeout возвращает -1.

```cpp
void remainingReadDoesNotRestart() {
    tb::IdleTimer timer(120);
    QSignalSpy expired(&timer, &tb::IdleTimer::idleTimeout);
    const int before = timer.remainingMs();
    QVERIFY(before > 0);
    QTest::qWait(35);
    QVERIFY(timer.remainingMs() < before);
    for (int i = 0; i < 20; ++i) (void)timer.remainingMs();
    QTRY_COMPARE(expired.count(), 1);
    timer.setActive(false);
    QCOMPARE(timer.remainingMs(), -1);
}
```

- [x] В TestClipboard добавить `snapshotTracksOwnershipAndRetry`: после copySecret("synthetic-owned") snapshot owned=true/pending=false/remaining>0; внешний тестовый процесс публикует "synthetic-foreign", snapshot owned=false и clearIfOurs не уничтожает чужую публикацию. Занятость проверять существующей внешней fixture: после неудачной очистки pending=true, после освобождения и успешного retry owned=false/pending=false. Снимок вызвать, пока внешний процесс держит clipboard открытым: вызов не должен ждать освобождения. Сохранять действующие правила изоляции тестового clipboard.

```cpp
tb::ClipboardGuard guard; QString error;
QVERIFY2(guard.copySecret(QStringLiteral("synthetic-owned"), &error), qPrintable(error));
const auto state = guard.snapshot();
QVERIFY(state.owned);
QVERIFY(!state.clearPending);
QVERIFY(state.remainingMs > 0);
guard.clearIfOurs();
QVERIFY(!guard.snapshot().owned);
```

- [x] Запустить build и две выбранные цели. До реализации ожидается RED новой сигнатуры/состояния, прежние тесты не менять ради этого RED.
- [x] Реализовать getter IdleTimer и Snapshot. На Windows snapshot проверяет ненулевой сохранённый sequence, GetClipboardSequenceNumber и GetClipboardOwner, не вызывает OpenClipboard и не читает содержимое. remainingMs=-1 при отсутствии своей публикации. Добавить один флаг clearPending_: сбрасывать при успешной публикации, успешной очистке и обнаружении чужого владельца; устанавливать перед попыткой очистки своей публикации и сохранять при retry. Защитные проверки непосредственно в copy/clear не заменять snapshot. Для остальных платформ возвращать консервативное отсутствие доступного снимка до появления нативной проверки метаданных; действующий механизм очистки не менять.

```cpp
int IdleTimer::remainingMs() const {
    return active_ ? timer_.remainingTime() : -1;
}
// В snapshot(), внутри существующего Q_OS_WIN:
const bool owned = sequence_ != 0
    && GetClipboardSequenceNumber() == sequence_
    && GetClipboardOwner() == static_cast<HWND>(owner_);
return {owned, owned && clearPending_, owned ? timer_.remainingTime() : -1};
```

- [x] Повторить build и test_lock/test_clipboard: exit 0, новые слоты PASS, прежние проверки очистки/блокировки PASS, без skipped. Зафиксировать локально: `rtk git add src/app/IdleTimer.h src/app/IdleTimer.cpp src/platform/ClipboardGuard.h src/platform/ClipboardGuard.cpp tests/test_lock.cpp tests/test_clipboard.cpp`, затем `rtk git commit -m "Добавить пассивное чтение защитных таймеров"`.

## Task 2: Библиотека, безопасный список и назначение группы

**Files:** Modify `src/ui/MainWindow.h/.cpp`, `src/ui/EntryEditDialog.cpp`; Test `tests/test_ui.cpp`.

**Interfaces:** Consumes model::VaultModel::entries(), search(const QString&), entriesInGroup(model::Id), entry(model::Id), MainWindow::commitEntry, EntryEditDialog::setGroup(model::Id). Produces private `enum class LibraryView { All, Favorites, Expiring, Group };`, `void setLibraryView(LibraryView)`, `void updateLibraryCounts()`, `QString groupPath(model::Id) const` внутри MainWindow. Снаружи использовать прежние tree/list/search getters и objectName кнопок `libraryAll`, `libraryFavorites`, `libraryExpiring`; библиотека не создаёт model::Id.

- [x] Добавить `libraryRequiresDestination`, `librarySelectionFollowsUuid`, `libraryFiltersAndCounters`: временное Application с двумя проектами, одинаковыми названиями записей, всеми типами, favorite/reverify и сроками без даты/вчера/сегодня/30/31 день. Начальное All содержит обе группы, дерево не выбрано. Заголовок глобального поиска, фильтры типа+AND-тегов+срока и порядок проверять одновременно; глобальные счётчики не меняются от запроса. N — число строк после всех фильтров; M — база после выбора представления и запроса, до ручных type/tags/expiry. В Expiring базовый shortcut входит в фильтр срока, не изменяет исходные данные.

```cpp
QTemporaryDir dir; Application app(dir.path()); QString error;
QVERIFY(app.createPasswordVault("test-password-123", &error));
const auto group = QUuid::createUuid();
QVERIFY(app.model()->addNode(group, {}, "Project").isEmpty());
EntryEditDialog editor(*app.model(), std::nullopt);
editor.setGroup({});
QCOMPARE(editor.findChild<QComboBox*>("entryGroup")->currentIndex(), -1);
ui::MainWindow window(app);
QVERIFY(!window.tree()->currentItem());
QVERIFY(window.findChild<QAbstractButton*>("libraryAll")->isChecked());
```

- [x] Проверить создание из библиотеки: без назначения Save оставляет диалог открытым и модель неизменной; после выбора существующего проекта Save сохраняет; Cancel не пишет. Из реальной группы назначение предвыбрано. При пустом дереве доступен +Проект. При отказе сохранения действует прежний откат. При фильтрации/удалении выбранного UUID карточка скрывает старое значение, выбран только UUID видимой строки; одноимённые записи не смешиваются. Проверить move и отсутствие выделенной группы для rename/delete/move group.
- [x] Запустить build и `rtk proxy cmd /d /c tools\test.cmd -R "^test_ui$" --output-on-failure`: новые ожидания дают RED на прежнем стартовом первом проекте и setGroup({}).
- [x] Добавить три отдельные checkable-кнопки над реальным деревом. All/Favorites/Group устанавливают expiry=0; Expiring=1; остальные фильтры сохраняются. Ручная смена expiry при Expiring переводит в All, сохраняя новое значение. Взаимные изменения tree/buttons/expiry оградить QSignalBlocker. refreshAll восстанавливает реальную группу только для Group; если она удалена, переходит в All. При поиске база глобальная, Favorites пересекается с результатом; пустой поиск Group использует entriesInGroup. Групповые операции требуют реального selectedNode.

```cpp
void EntryEditDialog::setGroup(Id id) {
    group_->setCurrentIndex(id.isNull() ? -1 : group_->findData(id));
}
// В addEntry(): убрать отказ при null parent, но сохранить editing/generation guards.
EntryEditDialog dialog(*app_.model(), std::nullopt, this);
dialog.setGroup(selectedNode());
// После Accepted сохранять через существующий commitEntry, не через прямую запись.
```

- [x] Сохранить существующую модельную validation.setEntry: null/исчезнувшая группа — ошибка, не автоматический первый проект. Формировать строку QListWidgetItem из title, type, groupPath, публичных флагов и срока; UUID остаётся UserRole. Никогда не включать token/login/password/private key/passphrase в text/toolTip/statusTip/accessibility. При перестройке сначала скрывать старую карточку, но если итоговый UUID тот же — не вызывать showEntry повторно. Никаких setItemWidget для строк. CtrlK добавляется к CtrlF; Escape остаётся локальным поиску/диалогу.
- [x] Проверить test_ui GREEN; адаптировать прежние exact-title ожидания к многострочным строкам через UUID и отдельные metadata assertions, сохраняя CRUD/откат/защиту. Локальный коммит перечисленных четырёх файлов: `rtk git commit -m "Добавить библиотечные представления и явный выбор группы"` после адресного git add.

## Task 3: Компоновка и сохранение положения окна

**Files:** Modify `src/ui/MainWindow.h/.cpp`; Test `tests/test_ui.cpp`.

**Interfaces:** Consumes библиотеку задачи 2 и стандартные QAction. Produces расширенный конструктор из карты, private `void restoreUiState()`, `void saveUiState()`, QSplitter objectName `mainSplitter`; свойства settings ограничены двумя утверждёнными ключами. `mainHeader`, `listSummary`, `searchScope`, `operationError` — стабильные objectName для проверок.

- [x] Добавить `mainGeometryFallbackAndWriteFailure` с QTemporaryDir и QSettings::IniFormat; окно конструировать `MainWindow(app, nullptr, &prefs)`. Сценарии: пустые настройки, повреждённые QByteArray, geometry за пределами экрана, корректное сохранение/восстановление, INI по пути каталога (AccessError), старые ключи неизменны. Снимок модели до/после ошибки равен; сообщение о несохранённом положении видимо в operationError/notifyRequested. Проверить набор новых ключей:

```cpp
QSettings prefs(dir.filePath("ui.ini"), QSettings::IniFormat);
{
    ui::MainWindow window(app, nullptr, &prefs);
    window.show(); window.resize(1000, 700); window.close();
}
prefs.sync();
QCOMPARE(prefs.status(), QSettings::NoError);
QCOMPARE(prefs.allKeys(), QStringList({"ui/mainGeometry", "ui/mainSplitter"}));
```

- [x] Build + test_ui ожидаемо RED на новой сигнатуре/компоновке. Геометрические assertions сравнивают доступный экран и допустимые размеры, а не системную толщину рамки.
- [x] Переставить существующие controls по макету: header с поиском/созданием, библиотека+дерево слева, фильтры/summary/list по центру, card справа. Один QSplitter с setChildrenCollapsible(false), исходные пропорции 250/350/640; начальный размер 1240×800 ограничить availableGeometry. При ширине <960 переносить верхние группы layout без пересоздания QAction; <720 сохранить прежний card-dialog. Проверить клики, табуляцию, контекстные меню, видимый фокус, доступные названия кнопок и отсутствие обрезания сообщений.

```cpp
splitter_->setObjectName(QStringLiteral("mainSplitter"));
splitter_->setChildrenCollapsible(false);
const auto area = screen()->availableGeometry();
resize(QSize(1240, 800).boundedTo(area.size()));
// restoreUiState: restoreGeometry/restoreState с проверкой bool;
// затем ограничить размер area и вернуть окно в availableGeometry при потере экрана.
// saveUiState вызывается перед close/hide при lock и при уничтожении окна.
preferences_->setValue("ui/mainGeometry", saveGeometry());
preferences_->setValue("ui/mainSplitter", splitter_->saveState());
preferences_->sync();
if (preferences_->status() != QSettings::NoError)
    report(QStringLiteral("Положение окна не сохранено."));
```

- [x] Ввести поля default QSettings и необязательный внешний указатель без глобальной смены организации приложения. При сохранении после lock не обращаться к очищенной модели. При teardown не открывать модальные диалоги и не повторять одно сообщение при каждом событии; сохранение не должно запускать новый commit хранилища.
- [x] Build + test_ui GREEN, включая временный INI, отсутствие секретных ключей, узкий диалог и старую семантику закрытия в трей. Адресный git add и локальный коммит `Обновить компоновку и сохранять геометрию главного окна`.

## Task 4: Защищённая карточка, оформление и реальные состояния

**Files:** Modify `src/ui/EntryCard.h/.cpp`, `src/ui/Theme.cpp`, `src/ui/MainWindow.h/.cpp`, `src/main.cpp`; Test `tests/test_ui.cpp`, `tests/test_lock.cpp`.

**Interfaces:** Consumes `IdleTimer::remainingMs() const`, `ClipboardGuard::snapshot() const`, существующие QuickPaste::sequence(), EntryCard::authorize/copyField и MainWindow protected actions. Produces `void MainWindow::setIdleTimer(IdleTimer*)`, private slot `void refreshStatus()`, сигналы `void EntryCard::editRequested()` и `void EntryCard::moreRequested()`; connect к MainWindow::editSelected и существующему listMenu в позиции кнопки. Никакого секретного QString в этих новых сигналах. Labels: `idleStatus`, `clipboardStatus`, `hotkeyStatus`, `vaultState`.

- [ ] Добавить `statusTickKeepsReveal`: выбрать синтетическую непротектированную запись, reveal, вызвать refreshStatus через QMetaObject::invokeMethod, проверить тот же UUID и secretText; дождаться штатных 15 секунд и маски. Проверить tick без idle-binding, разрушение bound timer, busy/lock, смену суток без записи в модель. Второй тест привязывает реальный короткий IdleTimer и ждёт lock при видимом и скрытом окне; paint/status ticks не продлевают срок.

```cpp
tb::IdleTimer idle(180); window.setIdleTimer(&idle);
QSignalSpy expired(&idle, &tb::IdleTimer::idleTimeout);
QVERIFY(QMetaObject::invokeMethod(&window, "refreshStatus", Qt::DirectConnection));
QVERIFY(window.findChild<QLabel*>("idleStatus"));
window.hide();
QTRY_COMPARE(expired.count(), 1);
```

- [x] Добавить карточные проверки: отдельные title/type/path/flags, постоянная маска при разной длине секрета, защищённый login не в декорациях; все строки трактуются plain text даже с HTML. Copy token/password/private key остаются в authorize/copyField; SSH не получает reveal. Ошибка/отмена reverify, lock во время подтверждения, переключение записи, клик снаружи, Edit/More, доступность клавиатуры проверяются через действующие guarded actions. Тесты themeModesApplyAndLoad сохраняют native-light regression и проверяют зелёный Highlight в обеих темах.
- [x] Build + test_ui/test_lock/native-theme: новые слоты RED; прежние проверки выдачи не ослаблять.
- [x] Разнести публичные metadata и действия карточки по существующим layouts; reuse названия и обработчики, добавить edit/more сигналы. В showEntry сохранить generation/selection guards и маску; не хранить указатель на Entry через диалог/асинхронную операцию. Зелёный QPalette::Highlight и соответствующий читаемый HighlightedText применять после формирования каждой палитры; warning/disabled/focus различать не только цветом.

```cpp
void MainWindow::setIdleTimer(IdleTimer* timer) {
    idleTimer_ = timer;
    refreshStatus();
}
// main.cpp, после создания MainWindow и до show():
window.setIdleTimer(&idle);
// Один дочерний UI QTimer:
statusTimer_->setInterval(1000);
connect(statusTimer_, &QTimer::timeout, this, &MainWindow::refreshStatus);
statusTimer_->start();
```

- [x] refreshStatus только читает gate/busy/remaining/snapshot/sequence и меняет тексты. Нет poke, рестарта idle, OpenClipboard, showEntry, refreshAll. Для pending показывать ожидание очистки, для !owned «Нет своей публикации», для unbound/inactive — отсутствие данных/неактивен, для пустого sequence — «Не зарегистрировано». После успешной/неудачной смены hotkey обновлять из фактического объекта, не из настроек. На не-Windows явно недоступный индикатор регистрации. После lock metadata очищены.
- [x] Смена UTC-даты обновляет library counters и список только если меняет отображаемые сроки/результат; сохранённый UUID не вызывает повторного showEntry. Выделить сравнение даты в refreshStatus, не добавлять второй частый таймер. Использовать QPointer для idle, чтобы разрушение чужого QObject не оставляло dangling pointer.
- [x] Build и выбранные цели GREEN; затем полный test_ui с 15s timeout проверки. Адресный git add и локальный коммит `Обновить карточку и показывать фактические состояния защиты`.

Проверки Task 4 выполнены, кроме искусственного перехода UTC-даты: ветка просмотрена в исходнике, системные часы не изменялись. Пункт выше оставлен частично открытым; функциональность реализована, ограничение приёмки записано явно.

## Task 5: Полная проверка, нативная приёмка и отчёт

**Files:** Test existing `tests/test_ui.cpp`, `tests/test_e2e.cpp`, `tests/CMakeLists.txt`; Create `docs/testing/2026-10-23-main-window-acceptance.md`; Modify `RESUME.MD`.

**Interfaces:** Consumes завершённые задачи 1–4, existing tools/build.cmd и tools/test.cmd. Produces документ с командами, exit codes, QtTest totals, путями screenshots и честными ограничениями; новых продуктовых API нет.

- [x] Перед полной проверкой добавить интеграционный `libraryRowsDoNotExposeSecrets`: создать 10 000 синтетических записей без дискового commit каждой строки, открыть окно, проверить count, UUID, отсутствие indexWidget и секретов в text/toolTip/statusTip/WhatsThis/accessibility. Секретные значения генерировать отдельно от title/path. Проверить одновременно Credentials.login и SSH bytes. Синтетический корпус не помещать в пользовательское хранилище.

```cpp
for (int i = 0; i < window.list()->count(); ++i) {
    auto* item = window.list()->item(i);
    QVERIFY(!item->data(Qt::UserRole).value<QUuid>().isNull());
    QVERIFY(!window.list()->itemWidget(item));
    for (const int role : {Qt::DisplayRole, Qt::ToolTipRole,
                          Qt::StatusTipRole, Qt::WhatsThisRole,
                          Qt::AccessibleTextRole, Qt::AccessibleDescriptionRole})
        QVERIFY(!item->data(role).toString().contains("synthetic-secret-marker"));
}
```

- [x] Запустить интеграционный тест на намеренно испорченной локальной строке с secret marker и убедиться в RED; убрать только временное внесение секрета, получить GREEN. Затем Release build и полный `rtk proxy cmd /d /c tools\test.cmd --output-on-failure`. Ожидание: все прежние 17 целей и добавленные проверки PASS; каждый QtTest log проверен на failed/skipped. Не повторять дорогие проверки без изменения/ошибки, которая оправдывает повтор.
- [x] Для нативного просмотра использовать только временный vault. Если текущий fixture не запускает главное окно изолированно, добавить в существующий test_e2e режим `--main-window-demo` с QTemporaryDir, синтетическими типами/одноимёнными записями и обычным MainWindow; проверить отсутствие доступа к каталогу реальных vault. В CMake сохранить прежние Windows/offscreen environments, тестовые цели и внешнюю receiver fixture. Никакого нового launcher приложения.
- [x] Через разрешённый Computer Use проверить светлую/тёмную темы, 960/1240+ logical px и 100/150/200%, табуляцию/CtrlK/CtrlF/Esc, дерево, библиотеку, фильтры, card/More, reveal, close-to-tray, empty/error states и отсутствие обрезания. Сохранить screenshots и фактические условия в acceptance. Геометрию не называть проверкой смены DPI между мониторами. При недоступной поверхности записать ограничение, не обходить policy.
- [x] Проверить внешний маршрут быстрой вставки на dedicated receiver только синтетическими значениями; следить, чтобы UI automation не отбирала foreground во время отправки. Старый успешный результат a42f2c8 не заменяет текущий. Реальный Hello, повышенный получатель и терминал не заявляются проверенными.
- [x] Выполнить review итогового diff против спецификации: пути выдачи/отката, фильтры, жизненный цикл QObject/QPointer, clipboard retries, settings isolation, доступность. Проверить `rtk git diff --check` и `rtk git ls-files --eol` для изменённых файлов; UTF-8/LF. Проверить фактическую author/committer identity перед каждым коммитом; push не выполнять.
- [x] Записать только подтверждённые итоги в acceptance и актуальную верхнюю секцию RESUME. Адресно закоммитить проверки/документы с сообщением `Зафиксировать приёмку главного окна`. Показать пользователю результат и оставшиеся непроверенные поверхности до перехода к следующему этапу.

## Самопроверка плана перед передачей

- Разделы спецификации 1–4 покрыты задачами 2–3; карточка/тема — задачей 4; состояния — 1+4; геометрия — 3; матрица и ограничения — 5.
- Все пять Review Focus привязаны к именованным проверкам. Единые имена remainingMs/snapshot/setIdleTimer, objectName и INI keys используются во всех задачах.
- Конструктор сохраняет совместимость прежних двухаргументных вызовов. Тестовый QSettings не подменяет глобальные пользовательские предпочтения. Никакие новые параметры idle/clipboard/hotkey не вводятся.
- Graphify запрошен до уточнения зависимостей; граф построен по a42f2c8, актуальный код проверен непосредственно. Pending semantic update относится к новым документам 5cc37a2; полный граф без необходимости не перестраивался.
- Утверждение этого плана — следующий шаг. Метод выполнения уже выбран: нативно, в текущем изолированном checkout.
