# Structured Values and Text Files Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking. Исполнение уже выбрано: последовательно в текущем чате, один свежий независимый обзор ветки в конце.

**Goal:** пять работающих типов записей, реестр по ресурсам и безопасное структурирование TXT без автоматического раскрытия прежних секретов.

**Architecture:** существующий Entry и группы расширяются типами PlainValue/TextFile и явной классификацией строк. Исходные байты файла остаются единственным телом документа; реестр хранит отдельные рабочие параметры со ссылками на снимок. Application владеет переходом на CBOR v4 и проверенными записями, UI — черновиком и подтверждениями.

**Tech Stack:** C++20, установленный Qt 6.11.2 Widgets/Concurrent/Test, текущие libsodium/FileOps, MSVC/vcpkg. Без установки сторонних пакетов; QHostAddress берётся из уже установленного QtNetwork, модуль включается в CMake и проверяется в пакете.

**Spec:** [утверждённая спецификация](../specs/2026-10-24-structured-storage-design.md). Утверждена владельцем 2026-10-24. База документа d52db4d; новые типы не реализованы.

**Status:** план утверждён владельцем 2026-10-24; последовательное исполнение после безопасной точки настроек и восстановления.

## Global Constraints

- Типы 0=ApiToken, 1=Credentials, 2=SshKey не меняют номера; 3=PlainValue, 4=TextFile. Внутренняя версия v4, чтение v1–v4, прежний контейнер TBVK/KDF/Hello.
- До 1 МиБ на значение/файл, 50 000 строк файла, 10 000 записей, 8 уровней групп, 64 МиБ открытой модели; прежние лимиты тегов и контейнера сохраняются.
- Все типы зашифрованы. Простое значение видно только в разблокированном сеансе; reverify имеет приоритет над видимостью и исключает поиск содержимого.
- Скрытые/неизвестные строки по умолчанию закрыты; преобразование прежнего секрета в открытое значение требует отдельного подтверждения.
- Исходные UTF-8/BOM/CRLF/LF/CR, пустые строки и комментарии сохраняются. Никакого выполнения shell, переменных и команд из TXT.
- Отмена, блокировка и ошибки до commit не меняют хранилище. Поздний результат не возвращает Ready и не выдаёт выполненную запись за гарантированно отменённую.
- Снимок источника неизменяем; редактирование его копии создаёт новый UUID. Никакой фоновой синхронизации.
- Проверки используют временные vault/INI/Run и вымышленные значения. Буфер — только прежний явный opt-in и согласованные тестовые окна.
- Реальная миграция владельца — отдельное действие в готовом интерфейсе. Прежнее разрешение на импорт не разрешает автоматическую смену видимости.
- Сначала завершить безопасную точку ранее утверждённых настроек с зелёными проверками и собственным журналом. Не смешивать их незавершённые изменения с коммитами модели.
- UTF-8/LF, cmd/bat CRLF. Git/build/search/test через RTK; identity владельца без атрибуции. Push не входит.
- Проверенная поставка: D:/Sources/gitverse/TokenBucket/dist/TokenBucket/ и соседний TokenBucket-windows-x64.zip. Прежняя сборка сохраняется.

## Review Focus

1. Чувствительный текст в комментарии, URL query или повторном имени — не становится видимым от угадывания типа: Task 5.
2. Вставка строки в начало файла — не переносит разрешённую видимость на новый пароль: Task 3/4.
3. Непустые старые неактивные поля у v1–v3 — читаются без очистки; миграция не теряет их ради нового типа: Task 1/6.
4. Lock при создании страховочной копии или финальном rename — запись описана фактически, Ready не возвращается: Task 2/6.
5. Смена пароля/режима/импорт поверх v3, а не только обычный Save — тоже защищены копией до обновления: Task 2/7.

## Файлы и границы

| Файл | Обязанность |
| --- | --- |
| model/Types.h, VaultModel.* | Новые полезные поля, ограничения, связи, поиск и CBOR |
| новые model/TextFile.h/.cpp | Чистая проверка UTF-8 и диапазоны строк без копии всего текста |
| Application.*, VaultStore.* | Состояние версии, согласование обновления, страховочные копии, атомарный commit |
| новые ui/EntryAccess.h/.cpp | Общая действующая проверка владельца с проверкой поколения/записи после диалога |
| EntryEditDialog.*, EntryCard.* | Пять форм, показ PlainValue и маршруты документов |
| новые ui/TextFileDialog.h/.cpp | Виртуальная таблица строк, видимость, просмотр и отдельная выдача файла |
| новые ui/RegistryView.h/.cpp | Таблица ресурсов, безопасные роли данных и сигналы действий |
| новые io/TextImport.h/.cpp | Чистый разбор, предложения, разрешённый кандидат без файловых побочных действий |
| новые ui/TextImportDialog.h/.cpp | Предпросмотр, классификация и конфликты |
| новые io/RegistryMigration.h/.cpp | Предложения преобразования прежних API-записей без автоматического раскрытия |
| MainWindow.*, MiniPanel.*, QuickPaste.h | Соединение владельцев, явные действия и допустимость выдачи |
| tests/test_textfile.cpp, test_text_import.cpp, test_storage_ui.cpp | Новые независимые цели; прежние тесты расширяются, не удаляются |
| CMakeLists.txt/tests/CMakeLists.txt | Источники и QtNetwork, тесты, прежние платформы и ограничения |

Все пути src/ кроме tests/. Исходники Application/VaultStore и SettingsDialog берутся после завершения A1–A6/B1–B3 настроек: их публичные интерфейсы сохраняются.

## Порядок проверки

Каждая задача: записать поведенческий тест → построить цель → увидеть ожидаемый RED → минимальная реализация → GREEN → коммит. Отсутствие декларации не заменяет поведенческий RED; необходимый каркас API не оставлять заглушкой. Полные журналы — в собственном каталоге .superpowers/sdd/2026-10-24-structured-storage/.

```powershell
rtk proxy cmd /d /c "tools\build.cmd"
rtk proxy cmd /d /c "tools\test.cmd -R test_textfile --output-on-failure"
```

Для других задач менять имя цели после -R. Не соединять regex через pipe внутри cmd. Windows Clipboard/Paste/E2E требуют реального нативного прогона с ранее согласованным opt-in; skip явно указывается, а не считается успехом.

### Task 1: Типы, строки, поиск и совместимый CBOR v4

**Files:** Types.h, VaultModel.h/.cpp; создать TextFile.h/.cpp и tests/test_textfile.cpp; расширить tests/test_model.cpp, CMakeLists.txt/tests/CMakeLists.txt.

**Produces:**

```cpp
// namespace tb::model:
enum class EntryType { ApiToken=0, Credentials=1, SshKey=2, PlainValue=3, TextFile=4 };
enum class ValueFormat { Text, Ip, Url };
struct TextFileData {
    QString fileName;
    QByteArray bytes;
    bool sourceSnapshot = false;
    QList<int> visibleLines; // нумерация с нуля; канонический строго возрастающий список
};
// Дополнения Entry:
QString parameterName, plainValue;
ValueFormat valueFormat = ValueFormat::Text;
TextFileData textFile;
Id sourceEntryId;
int sourceLine = -1; // -1 только при отсутствии sourceEntryId
namespace textfile {
struct LineRange { qsizetype offset, length, terminatorLength; };
QString validate(const TextFileData&);
QList<LineRange> lineRanges(const QByteArray&);
QString lineText(const TextFileData&, int line);
bool isVisible(const TextFileData&, int line);
}
// VaultModel:
int decodedVersion() const; // значение после успешного fromCbor; новая модель = 4
```

- [x] **1. RED:** тестировать настоящий setEntry и поиск, а не только номера enum. В локальной fixture создать проект и обычную запись с вымышленным значением:

```cpp
VaultModel m; const auto group=QUuid::createUuid();
QVERIFY(m.addNode(group,{},"Demo").isEmpty());
Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.type=EntryType::PlainValue;
e.title="Public IP"; e.parameterName="PUBLIC_IP"; e.plainValue="192.0.2.10";
QVERIFY(m.setEntry(e).isEmpty()); QVERIFY(m.search("192.0.2.10").contains(e.id));
e.reverify=true; QVERIFY(m.setEntry(e).isEmpty());
QVERIFY(m.search("192.0.2.10").isEmpty());
```

Добавить независимые примеры диапазонов: пусто/BOM→0 строк, a+CRLF+b+LF→3 строки, CR отдельно, последняя пустая строка. Диапазоны не включают исходный BOM; реконструкция с сохранённым BOM и всеми разделителями даёт исходные байты. Для IPv4/IPv6 использовать QHostAddress из установленного QtNetwork; проверить 192.0.2.1, 2001:db8::1, неверные октеты и адрес с портом. URL проверяется QUrl StrictMode, открытие схем не является задачей модели. В тесте файла менять тело и номера видимых строк, проверять отрицательные/дубликаты/выход за границу, NUL, неверный UTF-8, имя и оба лимита.
- [x] **2. Run:** test_textfile/test_model. Expected: новые сценарии не проходят из-за отсутствия типов/поведения; после деклараций зафиксировать поведенческий RED поиска/границ.
- [x] **3. Реализация:** один канонический массив байтов, диапазоны без копирования тела. Разрешённые новые поля проверять в setEntry/fromCbor одинаково; старые типы не очищать. Проверку sourceEntryId выполнять после чтения всех entries. Snapshot с тем же UUID нельзя менять по телу/признаку; отсутствующая удалённая цель допустима.

```cpp
if (entry.type == EntryType::PlainValue && !entry.reverify
    && entry.plainValue.contains(needle, Qt::CaseInsensitive)) matches = true;
if (entry.type == EntryType::TextFile && !entry.reverify)
    for (int line : entry.textFile.visibleLines)
        matches |= textfile::lineText(entry.textFile,line).contains(needle,Qt::CaseInsensitive);
```

Схема v4: сохранить прежние 19 ключей записи; добавить key/value/format, fileName/fileBytes/fileSnapshot/fileVisible, sourceId/sourceLine. value и fileBytes — CBOR bytes, строки метаданных — text, format — text-код text/ip/url, visible — array uint. Оба писателя используют один порядок. У v1–v3 отклонять type>2 и непустые новые полезные поля; v4 проверять тип каждого нового ключа. Неизвестная версия даёт явный отказ.
- [x] **4. GREEN:** золотое побайтовое сравнение Qt/secure writer для пяти типов и richEntry старого теста с заполненными старыми полями; roundtrip с кириллицей/BOM/CRLF, v1–v3 fixtures, транзакционный отказ повреждённых новых данных и неизменность прежней модели. Run test_textfile/test_model/test_container, затем полный набор.
- [x] **5. Commit:** Add structured values and lossless text documents to the vault model.

### Task 2: Согласованный переход на v4 и зашифрованная копия

**Files:** Application.*, VaultStore.*, tests/test_store.cpp/test_app.cpp/test_pwchange.cpp; существующий FaultFileOps.h из плана восстановления.

**Consumes:** decodedVersion(), v4 encoder, strict loadBytes, verified_, FileOps, generation/busy, RestoreResult и runVaultOperation из настроек.

**Produces:**

```cpp
bool Application::schemaUpgradeRequired() const;
bool Application::approveSchemaUpgrade(QString* error); // Ready/!busy, разрешение привязано к generation
QString Application::upgradeSafetyPath() const;
QString VaultStore::preserveBeforeUpgrade(const QByteArray& expectedMain,QString* error);
// private Application: один путь записи для всех производителей шифротекста
bool storeCandidate(const QByteArray& candidate, quint64 generation,
                    bool upgradeApproved, QString* error);
```

- [x] **1. RED:** в test_app создать собственный v3 TBVK вручную: QCborMap root с v=3, узлом Demo и одной ApiToken-записью; packPassword после crypto::init, запись только в QTemporaryDir через realFileOps. Проверить:

```cpp
Application app(dir.path()); QString error;
QVERIFY(app.unlock("valid-password",&error));
QVERIFY(app.schemaUpgradeRequired());
QVERIFY(!app.save(&error)); // без согласования файл остаётся v3
QVERIFY(app.approveSchemaUpgrade(&error)); QVERIFY(app.save(&error));
QVERIFY(!app.upgradeSafetyPath().isEmpty());
app.lock(); QVERIFY(app.unlock("valid-password",&error));
QVERIFY(!app.schemaUpgradeRequired());
```

Байты прежнего файла сравнить со страховочной копией; её декодер читает root v3. Не использовать реальное хранилище для подготовки fixture.
- [x] **2. Run:** test_app/test_store/test_pwchange. Expected: save без согласования ещё не запрещён или нет пригодной копии. Данные FaultOps: отказ/порча safety write, изменённый main, reparse, lock до записи и lock в финальном rename. Нужны отдельные строки для save/importContainer/switchToPassword/switchToHello.
- [x] **3. Реализация:** запоминать версию проверенной загрузки в Application отдельно от версии писателя. Новый vault сразу v4. Согласование — только флаг поколения, без файловой записи. Все store_.save вызывающие пути направить через storeCandidate; для существующего v1–v3 main требовать согласование и проверенную уникальную копию vault.tb.before-upgrade-UTC-UUID в закрытом каталоге. Копия создаётся CREATE_NEW и перечитывается. Verified main обязан совпасть со снимком.

```cpp
if (generation()!=expected) return fail(error,"Операция отменена.");
if (legacyCurrent && !upgradeApproved) return fail(error,"Требуется подтверждение обновления формата.");
if (legacyCurrent && safetyPath.isEmpty())
    safetyPath=store_.preserveBeforeUpgrade(currentCiphertext,error);
if (legacyCurrent && safetyPath.isEmpty()) return false;
if (generation()!=expected) return fail(error,"Операция отменена.");
return store_.save(candidate,error);
```

В настоящей реализации legacyCurrent/currentCiphertext/safetyPath — снимки полей владельца, снятые до worker; не читать меняющуюся GUI-модель из worker. Outcome дополнить wroteFile: true только после фактического успешного save, не при unlock. После lock поздний wroteFile сообщает совершённую запись и сохраняет NeedUnlock; обычное чтение unlock не считается commit. Экспорт v4 не меняет локальный файл/версию. Сохранять копию до обновления при последующих сохранениях v4; не ротировать её вместе с .bak.
- [x] **4. GREEN:** все реальные временные v3→v4 маршруты, отмена согласования, повтор после отказа, блокировка до/во время commit, Hello через внедрённый ответ, восстановление v3 и v4 из существующего B2. Проверить DACL safety и отсутствие открытых значений на диске. Run test_store/test_app/test_pwchange и полный набор.
- [x] **5. Commit:** Preserve verified legacy vaults before schema upgrades.

### Task 3: Пять компактных форм и безопасное отображение простых значений

**Files:** EntryEditDialog.*, EntryCard.*, новые EntryAccess.h/.cpp, test_storage_ui.cpp, tests/test_ui.cpp, CMakeLists/tests/CMakeLists.

**Consumes:** поля Task 1, таймеры/сроки A2, действующие VerifyDialog/Hello.

**Produces:** прежние entry()/showEntry()/copySecret() сохраняются; редактор получает пять явных типов и редактор текстовых байтов; общий bool ui::authorizeEntry(Application&, model::Id, QWidget*) для новых выдающих UI. Старые дополнительные проверки selection/action/generation в вызывающих не удалять.

- [x] **1. RED:** в test_storage_ui реальная форма на временном Application с PlainValue и TextFile; поля objectName: entryParameterName, entryPlainValue, entryValueFormat, entryTextFileName, entryTextFileBody. Кнопка Save:

```cpp
EntryEditDialog dialog(*app.model(),std::nullopt);
dialog.findChild<QComboBox*>("entryType")->setCurrentIndex(int(EntryType::PlainValue));
dialog.findChild<QLineEdit*>("entryTitle")->setText("Public IP");
dialog.findChild<QLineEdit*>("entryParameterName")->setText("PUBLIC_IP");
dialog.findChild<QLineEdit*>("entryPlainValue")->setText("192.0.2.10");
dialog.accept(); QVERIFY(dialog.entry());
QCOMPARE(dialog.entry()->plainValue,QStringLiteral("192.0.2.10"));
QVERIFY(dialog.entry()->token.isEmpty());
```

Для snapshot исходника изменение тела создаёт новый UUID через явное Save As, а не setEntry прежнего ID. При метаданных неизменённого TextFile байты сравнить точно.
- [x] **2. Run:** test_storage_ui/test_ui. Expected: отсутствуют нужные контролы/видимый PlainValue. Добавить отмену смены секрет→plain, reverify до показа/редактора, lock во время VerifyDialog и сброс visibility после изменения тела.
- [x] **3. Реализация:** QComboBox хранит enum в itemData, не зависит от индекса фильтра. Поля скрываются вместе с подписями, прежний autosize/рабочая область/скролл сохраняются. PlainValue без reverify показан сразу и без таймера; при reverify — маска и действующая проверка. Общее authorizeEntry проверяет Ready/!busy, снимает generation/modified/type/reverify, вызывает прежний VerifyDialog при необходимости, затем заново проверяет запись; не кэширует успех между действиями.

```cpp
if (original.type!=EntryType::PlainValue && target==EntryType::PlainValue) {
    if (!confirmVisibleValue(parent)) return;
}
if (textBodyChanged) {
    draft.textFile.bytes=editedUtf8WithLf;
    draft.textFile.visibleLines.clear();
}
```

confirmVisibleValue — локальный QMessageBox с PlainText и Yes/No, по умолчанию No; остальные настройки не подавляют его. editedUtf8WithLf сохраняет прежний BOM. Перед очисткой списка видимых строк показать причину и возможность отмены. Не копировать секрет в note/tooltip/accessibility.
- [x] **4. GREEN:** test_storage_ui/test_ui; все прежние API/credentials/SSH/reverify/cancel случаи, обе темы и 420×480, 100/200% синтетический DPI. Динамические подписи сроков совпадают с A2. Полный набор перед коммитом.
- [x] **5. Commit:** Add compact editors and guarded views for new storage types.

### Task 4: Просмотр строк, выдача файла и простая строка в быстрой вставке

**Files:** новые TextFileDialog.h/.cpp; EntryCard.*, MiniPanel.*, QuickPaste.h, MainWindow.*, tests/test_storage_ui.cpp/test_paste_win.cpp/test_e2e.cpp.

**Produces:** TextFileDialog(Application&, model::Id, ClipboardGuard&, QWidget*); приватная QAbstractTableModel строк. Сигнал requestRegistryImport(model::Id) передаёт только UUID. Получателем копирования остаётся единственный ClipboardGuard MainWindow.

- [x] **1. RED:** открыть настоящий TextFileDialog с публичной строкой 0 и скрытой строкой 1; проверить DisplayRole/ToolTipRole/AccessibleTextRole не содержат секрета, Ctrl+C на строке 0 копирует только её, а на скрытой требует проверку. Предыдущий ввод вне этой строки не меняется:

```cpp
TextFileDialog dialog(app,id,clipboard);
auto* table=dialog.findChild<QTableView*>("textFileLines"); QVERIFY(table);
QVERIFY(!table->model()->data(table->model()->index(1,1)).toString().contains("synthetic-password"));
QVERIFY(!table->model()->data(table->model()->index(1,1),Qt::ToolTipRole).toString().contains("synthetic-password"));
```

- [x] **2. Run:** test_storage_ui/test_paste_win. Expected: нет таблицы строк/PlainValue не допускается. Negative rows: empty, CR/LF/NUL, TextFile целиком, reverify cancel, lock во время показа/копирования, существующий файл экспорта.
- [x] **3. Реализация:** модель вычисляет только запрошенные строки; не создавать 50 000 QWidgets. Из ролей наружу передаются только безопасное отображение и номер строки, не весь document.bytes. Показ скрытой строки имеет один таймер A2 и сбрасывается при уходе/блокировке/смене выбора. Выдача целого файла отдельна, её подтверждение по умолчанию отменено; точные байты в файл — FileOps CREATE_NEW с закрытыми правами. Занятый/существующий путь не обходить.

```cpp
if (e.type==EntryType::PlainValue && field==paste::Field::Secret) return &e.plainValue;
// TextFile не имеет автоматического поля вставки.
if (e.type==EntryType::TextFile || e.type==EntryType::SshKey) return nullptr;
```

В MiniPanel подпись поля PlainValue — «Значение», без ложного login fallback; ручной выбор и suspended из A2 сохраняются. Весь TextFile исключён; отдельная строка выдаётся только явным выбором в документе. Реальные проверки получателя/SendInput остаются общими.
- [x] **4. GREEN:** test_storage_ui/test_paste_win/test_e2e; точные bytes экспорта BOM/CRLF, отмена full-copy, собственная публикация и чужой буфер, отсутствие Enter и fallback копирования. Полный набор.
- [x] **5. Commit:** View document lines and safely deliver plain values.

### Task 5: Чистый TXT-разбор и предпросмотр импорта

**Files:** новые io/TextImport.h/.cpp, ui/TextImportDialog.h/.cpp, test_text_import.cpp; MainWindow.*, CMakeLists/tests/CMakeLists.

**Interfaces:**

```cpp
namespace tb::io {
enum class ConflictAction { Skip, CreateVersion, Replace };
struct ProposedParameter {
    int line=0;
    QString name,value,section;
    model::EntryType suggestedType=model::EntryType::ApiToken;
};
struct TextImportPreview {
    model::Entry source; // TextFile, sourceSnapshot=true, единственное тело
    QList<ProposedParameter> parameters;
    QString error;
};
struct RegistrySelection {
    int parameter=0;
    model::EntryType type=model::EntryType::ApiToken;
    QString title,parameterName;
    QStringList groupPath;
    ConflictAction conflict=ConflictAction::CreateVersion;
    model::Id replaceId;
};
TextImportPreview parseTextFile(const QByteArray&,const QString& fileName);
bool buildRegistryCandidate(const model::VaultModel&,const TextImportPreview&,
    const QList<RegistrySelection>&,model::VaultModel* out,QString* error);
}
```

TextImportDialog выдаёт только утверждённые selections и согласованное разрешение видимых строк снимка; выбор type сам по себе не считается подтверждением раскрытия прежнего секрета.

- [x] **1. RED:** источник с комментариями, public IP, секретом в URL и повторным ключом; байты source совпадают, повтор не теряется:

```cpp
const QByteArray input="# Demo\r\nPUBLIC_IP=192.0.2.10\r\nTOKEN=synthetic\r\nPUBLIC_IP=192.0.2.20\r\n";
const auto parsed=io::parseTextFile(input,"demo.txt");
QVERIFY(parsed.error.isEmpty()); QCOMPARE(parsed.source.textFile.bytes,input);
QCOMPARE(parsed.parameters.size(),3);
QCOMPARE(parsed.parameters[1].suggestedType,EntryType::ApiToken);
QCOMPARE(parsed.parameters[0].line,1); QCOMPARE(parsed.parameters[2].line,3);
```

- [x] **2. Run:** test_text_import. Expected: разбор отсутствует либо теряет повтор/классификацию. Добавить парные кавычки, export, NAME: VALUE, неизвестную строку, пароль в комментарии, DATABASE_URL с userinfo, APP_URL с token query, пустую/слишком большую/битую UTF-8 запись и лимит групп/entries.
- [x] **3. Реализация:** parser только читает bytes; source не меняет visibleLines по эвристике. Не исполнять команды/escape. Известные безопасные значения предлагаются PlainValue, остальные ApiToken; классификацию подтверждает UI. Разные одинаковые имена остаются параметрами с разными line. Перед buildRegistryCandidate заново проверить индексы, target types, ссылки, список видимых строк и ограничения целого кандидата; out присваивается только после полного успеха.

```cpp
auto candidate=base;
for(const auto& choice:selections) {
    // Проверенные parameter/replaceId, реальная группа и отдельный UUID версии.
    if(!applyChoice(candidate,preview,choice,error)) return false;
}
*out=std::move(candidate);
return true;
```

applyChoice — приватная функция TextImport.cpp с сигнатурой bool(VaultModel&,const TextImportPreview&,const RegistrySelection&,QString*), без файлов/сохранения. После отдельного подтверждения UI связывает рабочую запись sourceEntryId/sourceLine. При Skip не менять прежние записи; Replace сохраняет UUID и пользовательские метаданные, CreateVersion создаёт отличимое название. Неоднозначная цель Replace запрещена.
- [x] **4. GREEN:** чистые тесты parser/candidate плюс настоящий диалог cancel/классификация/конфликты на временной модели. QFile-чтение выполняется отдельно с лимитом/обычным файлом и worker+проверкой поколения; parser не открывает пути. Полный набор.
- [x] **5. Commit:** Preview and classify text imports without losing source data.

### Task 6: Реестр ресурсов и подтверждённая миграция прежних записей

**Files:** новые RegistryView.h/.cpp, io/RegistryMigration.h/.cpp; MainWindow.*, test_storage_ui.cpp/test_text_import.cpp/test_app.cpp.

**Interfaces:** RegistryView(Application&,QWidget*) имеет setGroup(model::Id), refresh() и сигналы entryActivated(model::Id), copyRequested(model::Id), sourceRequested(model::Id). io::RegistryMigration.h определяет:

```cpp
namespace tb::io {
struct MigrationProposal {
    quint64 generation=0;
    QList<model::Entry> before,after; // только выбранные записи, одинаковые UUID
    QString error;
};
MigrationProposal proposeRegistryMigration(const model::VaultModel&,
    const QList<model::Id>& ids,quint64 generation);
bool buildMigrationCandidate(const model::VaultModel&,const MigrationProposal&,
    model::VaultModel* out,QString* error);
}
```

Group изменения выбираются существующим TextImportDialog; before/after сохраняют исходные метаданные. Безвредная структура не означает согласия раскрыть содержимое.

- [x] **1. RED:** прежняя ApiToken PUBLIC_IP и ApiToken TOKEN, richEntry с несколькими непустыми полями, SSH и снимок исходника:

```cpp
const auto plan=io::proposeRegistryMigration(model,{ip.id,token.id,ssh.id},generation);
QCOMPARE(model.entry(ip.id)->type,EntryType::ApiToken); // предложение не меняет базу
VaultModel candidate;
QVERIFY(io::buildMigrationCandidate(model,plan,&candidate,&error));
QCOMPARE(candidate.entry(ip.id)->id,ip.id);
QCOMPARE(candidate.entry(ip.id)->created,ip.created);
QCOMPARE(candidate.entry(ssh.id)->sshPrivate,ssh.sshPrivate);
```

Фактический переход ip в PlainValue разрешён только после UI-подтверждения соответствующего after; до него защищённый тип остаётся. Тест изменённого before запрещает применить старое предложение.
- [x] **2. Run:** test_text_import/test_storage_ui/test_app. Expected: отсутствует реестр/миграция или меняется база до подтверждения. Проверить deleted source, отсутствующую/неоднозначную запись, изменённый payload без корректировки modified, lock/generation, невыбранные проекты, наследование reverify и все метаданные.
- [x] **3. Реализация:** RegistryView использует QAbstractItemModel/QTreeView по имеющимся группам; роли показывают только безопасные значения. UserRole содержит UUID, не секрет; копирование направляется прежнему владельцу. Открытие ресурса не создаёт группы и не меняет записи.

```cpp
if(app.generation()!=proposal.generation || app.gate()!=Application::Gate::Ready)
    return fail(error,"Сеанс изменился. Повторите предпросмотр.");
// После точного сравнения before с базой:
auto previous=*app.model();
*app.model()=candidate;
if(!app.save(error)) {
    if(app.generation()==proposal.generation) *app.model()=std::move(previous);
    return false;
}
```

Перед этим создать проверенную уникальную страховочную копию для миграции даже при уже v4; добавить QString Application::preserveBeforeRegistryMigration(QString*) с Ready/!busy, strict expected ciphertext и CREATE_NEW. В VaultStore переиспользовать внутреннюю проверенную запись копии Task 2, передав назначение before-registry-migration вместо before-upgrade; не дублировать проверку путей, прав и readback. Пустой результат — отказ до изменения модели. Обычный save не заменяет этот шаг. Назначение/подписи снимка — прежние FileOps-права и чтение; тела снимков не копируются в заметки. После commit перечитать/проверить сохранённую модель через обычную блокировку и разблокировку; пользователь вводит пароль локально, существующий тест использует вымышленный.
- [x] **4. GREEN:** точность значений и SSH, before/after metadata, неправильные признаки snapshot, отказ safety/save, повтор, отмена, lock во время apply и восстановление страховочной копии. Проверить отсутствие невыбранных преобразований и секретов в DisplayRole/tooltip/search. Полный набор.
- [x] **5. Commit:** Organize resource registries with reviewed record migration.

### Task 7: Интеграция подтверждений, копий и старых путей приложения

**Files:** MainWindow.*, SessionFlow.*, ExportDialog.*, SettingsDialog.*, test_app/test_storage_ui/test_e2e; docs формата/README.

**Produces:** общий приватный MainWindow::ensureSchemaUpgradeApproved() использует schemaUpgradeRequired/approveSchemaUpgrade из Task 2; перед каждым пользовательским действием записи вызывается до начала worker. Обычное открытие/поиск/просмотр/экспорт не обновляют локальный файл. ExportDialog сообщает о формате v4.

- [ ] **1. RED:** реальный маршрут временного v3 Application: открыть MainWindow, начать сохранение, отменить подтверждение обновления, сравнить bytes и schemaUpgradeRequired(). Затем согласовать и пройти save/lock/unlock. Отдельные data rows — смена пароля, Hello mock и импорт поверх существующего main.
- [ ] **2. Run:** test_app/test_storage_ui/test_e2e. Expected: хотя бы один путь обходит подтверждение или не создаёт проверенную v3-копию. Для экспорта сравнить main до/после и декодировать только экспорт как v4.
- [ ] **3. Реализация:** использовать существующий runVaultOperation, QPointer и suspended-guard. Перед отдельной опасной операцией не сохранять незавершённый черновик настроек; ошибки/частичный результат настроек из A4 не превращать в разрешение обновления.

```cpp
if(!app_.schemaUpgradeRequired()) return true;
if(QMessageBox::question(this,"Обновление формата",
    "Будет сохранена зашифрованная копия. Прежняя версия программы не откроет новый формат.",
    QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return false;
return app_.approveSchemaUpgrade(&error);
```

В настоящем диалоге PlainText и закрытые пути без значений записей. Поколение согласования не переносится через lock/unlock, восстановление или замену основного файла. SettingsDialog не владеет сериализацией. Все старые типы, CSV/KeePass, смена пароля/режима и генератор остаются работоспособными. Для нового PlainValue экспорт в окружение проходит прежнюю проверку имени/предупреждение; TextFile не экспортируется одной переменной молча.
- [ ] **4. GREEN:** полный набор после сценариев v1–v4, Hello cancel, частичной ошибки настроек и восстановления старой копии с последующим новым save. Проверить фактическую необходимость старого пароля старой копии, а не текущего. Нет расширения разрешений будущего MCP/API.
- [ ] **5. Commit:** Integrate schema confirmation across all vault write flows.

### Task 8: Приёмка, независимый обзор и локальная поставка

**Files:** README/RESUME, документы формата, docs/testing/2026-10-24-structured-storage-acceptance.md и docs/build/2026-10-24-structured-storage-release.md.

- [ ] **1. Проверить покрытие:** сопоставить 12 разделов спецификации с Task 1–7, исходными тестами и реальными результатами. В отчёт — конкретные проверки, пропуски и неопределённые аппаратные условия; не старые числа тестов.
- [ ] **2. Run:** Release tools/build.cmd и полный tools/test.cmd --output-on-failure с прежним явным opt-in буфера. Expected: все фактические цели проходят, свежие QtTest-журналы не содержат failed/skipped. При появлении skipped назвать сценарий и причину, устранить среду или указать реальную границу.
- [ ] **3. Независимый обзор:** один свежий reviewer всей ветки с утверждённой спецификацией/этим планом и журналом. Особое внимание — маски в ролях/поиске, snapshot/visibleLines после редактирования, все вызовы storeCandidate, сохранность исходников и отмена при lock. Подтверждённые Critical/Important исправить с RED→GREEN и полным повтором.
- [ ] **4. Нативная приёмка:** вымышленные проекты по образцу Oitasc/Orbitrax, пять форм, оба вида реестра/файла, обе темы, 420×480, 100/200%, клавиатура, ошибки и подтверждения. Показанный inline-макет не считается нативным тестом Qt.
- [ ] **5. Поставка:** в рамках прежнего поручения локально влить завершённую ветку в main; удалённого push нет. Перед package-windows.cmd проверить абсолютный dist внутри выбранного worktree, отсутствие reparse и пользовательских занятых файлов; процессы владельца не завершать. После упаковки проверить QtNetwork DLL вместе с остальными зависимостями, извлечённый синтетический запуск, ZIP hash и каждый файл при переносе в постоянные пути. Сохранить и сверить прежний пакет. Настоящую миграцию записей автоматически не запускать.

## Покрытие спецификации

| Раздел | Задачи |
| --- | --- |
| 1–2: объём, пять типов, существующие владельцы | 1, 3, 6 |
| 3–4: видимость, reverify, поиск, PlainValue | 1, 3, 4 |
| 5: точные строки/редактирование/выдача файла | 1, 3, 4 |
| 6–7: разбор, конфликты, происхождение | 5, 6 |
| 8: v4, безопасность всех производителей файла | 1, 2, 7 |
| 9: прежние записи и отдельное согласование | 6, 7 |
| 10: границы и последовательность | 1–7 |
| 11: приёмка/обзор/поставка | 8 |
| 12: исполнение и пределы разрешения | весь план |

После рассмотрения этого плана владелец подтверждает, что он отражает утверждённую спецификацию. Повторный выбор способа исполнения не требуется: сохраняется последовательная работа в текущем чате.
