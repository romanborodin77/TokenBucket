# TokenBucket: Verified Backup Recovery Implementation Plan

> **Исполнение:** executing-plans, последовательно в текущем чате. Связан с [основным планом](2026-10-24-unified-settings.md), выполняется после A6, до общего обзора и поставки A7.

**Goal:** безопасное восстановление своей существующей .bak с проверкой пароля/Hello, зашифрованной страховочной копией и обязательной блокировкой после фиксации.

**Architecture:** VaultStore выполняет проверяемые файловые операции; Application проверяет кандидата, поколение и состояние; SettingsDialog только запускает подтверждённую операцию. UI не получает байты контейнеров или ключ. Никакого вызова обычного save для замены main копией.

**Tech Stack:** существующие FileOps, QtConcurrent, Container, SecureBytes, VaultModel и QtTest; формат TBVK не меняется.

**Spec:** [единые настройки](../specs/2026-10-24-unified-settings-design.md), разделы 7–11. Глобальные ограничения основного плана действуют целиком.

## Global Constraints

- Только собственная .bak в каталоге данных; ссылки, подменённые и недопустимые контейнеры отклоняются.
- Сначала проверка контейнера/KDF/AEAD/модели и подтверждение; затем проверенная страховочная копия и атомарная замена.
- .bak не ротируется при восстановлении. До успеха verified_ не подменяется и новый открытый сеанс не создаётся.
- Неправильный пароль, отмена, изменившиеся файлы и ошибки записи до фиксации сохраняют main. После успеха — NeedUnlock, пустая модель, режим восстановленного контейнера.
- Блокировка во время финальной замены не означает гарантированного отсутствия записи: результат отражается явно, Ready не возвращается.
- Тесты — только временные данные; реальная биометрия/файлы владельца не используются.

## Review Focus

1. Подмена .bak после подготовки — commit обязан отказать, а не восстановить другой файл: B1/B2.
2. Старый пароль .bak после смены текущего пароля — проверяется именно кандидат: B2.
3. Повреждённая AEAD или модель при внешне допустимом заголовке — ни один файл не заменяется: B2.
4. Отказ страховочной записи/проверки или атомарной замены — старый main остаётся доступен: B1.
5. Lock до фиксации и lock, совпавший с успешной заменой — разные результаты без позднего Ready: B1/B2/B3.

## Файлы и интерфейсы

Изменить `src/vault/VaultStore.h/.cpp`, `src/app/Application.h/.cpp`, `src/ui/MainWindow.h/.cpp`, `src/ui/SettingsDialog.h/.cpp`; тесты `tests/test_store.cpp`, `test_app.cpp`, `test_settings.cpp`, `test_e2e.cpp`. В новый тестовый `tests/FaultFileOps.h` вынести существующий FaultOps для использования Store и Application; в продукт этот helper не включается. Нового backend, таймера, формата контейнера и публичного метода записи непроверенной модели не добавлять.

Общий результат в `VaultStore.h`, namespace tb::store:

```cpp
struct RestoreResult {
    bool committed = false;
    QString safetyPath;
    QString error;
};
// Контракт: expectedBackup уже проверен Application; Store проверяет байты файлов и I/O.
RestoreResult VaultStore::restoreBackup(const QByteArray& expectedMain,
    const QByteArray& expectedBackup, const std::function<bool()>& canceled);
```

Application использует тот же RestoreResult, а не параллельный enum успеха. `committed` описывает совершённую замену, даже если после неё ошибка финальной проверки или блокировка. Непустая error никогда не трактуется UI как полный успех.

### B1. Файловая фиксация с сохранением предыдущего main

**Consumes:** FileOps::writeAll/renameReplace/removeFile, ensurePrivateDir, VaultStore::loadBytes/acceptVerified.

**Produces:** описанный restoreBackup. Имена и байты файлов определяются Store; UI не передаёт путь назначения.

- [ ] **1. RED:** расширить существующий FaultOps управляемым отказом конкретной записи/rename и callback перед записью/rename, не добавлять тестовые переключатели в производственный Store. Без инъекции ошибки делегировать realFileOps, чтобы fake не перезаписывал существующий safety-файл и сохранял настоящий контракт закрытых прав. Для коллизии callback создаёт файл по переданному safetyPath до writeAll; UUID-генератор продукта не подменяется. Первый успешный тест использует настоящие временные файлы:

```cpp
VaultStore store(dir_->path());
QString error;
QVERIFY(store.save("older", &error));
QVERIFY(store.save("current", &error));
const auto result = store.restoreBackup("current", "older", [] { return false; });
QVERIFY2(result.committed, qPrintable(result.error));
QVERIFY(result.error.isEmpty());
QByteArray bytes;
QCOMPARE(store.loadBytes(store.mainPath(), &bytes), VaultStore::Load::Ok);
QCOMPARE(bytes, QByteArray("older"));
QCOMPARE(store.loadBytes(result.safetyPath, &bytes), VaultStore::Load::Ok);
QCOMPARE(bytes, QByteArray("current"));
QCOMPARE(store.loadBytes(store.backupPath(), &bytes), VaultStore::Load::Ok);
QCOMPARE(bytes, QByteArray("older"));
```

Здесь строки моделируют зашифрованные байты на границе Store; аутентификацию настоящего TBVK проверяет B2.

- [ ] **2. Проверить RED:** test_store. Data rows: wrong expectedMain, changed backup, missing/oversize/symlink input, cancel до записи и перед rename, failWriteCall=1/2, corruptWriteCall=1/2, отказ финального rename, два последовательных восстановления и коллизия страховочного имени. Проверить main/.bak после каждого отказа, не только result.committed.
- [ ] **3. Реализация:** ensurePrivateDir; loadBytes обоих файлов и сравнение со снимками; основной файл должен совпасть с уже verified_, его исчезновение — отказ. Уникальный safetyPath `vault.tb.before-restore-<UTC>-<QUuid>` создаётся CREATE_NEW существующим FileOps и проверяется перечитыванием. Временный кандидат тоже создаётся с закрытыми правами и проверяется. Непосредственно перед rename снова сравнить main/.bak и вызвать canceled; по ошибке убрать только собственный временный кандидат. Страховочную копию после проверенной записи не удалять.

```cpp
if (canceled()) return {false, safetyPath, QStringLiteral("Восстановление отменено.")};
if (!ops_.renameReplace(restoreTmp, mainPath()))
    return {false, safetyPath, QStringLiteral("Не удалось заменить файл хранилища.")};
hasVerified_ = false;
verified_.clear();
// Проверка нового main после rename; committed остаётся true даже при её отказе.
return {true, safetyPath, postCommitError};
```

Проверка .bak/main после чтения не является обещанием атомарного сравнения против произвольного внешнего процесса. Сохранить действующий instance lock и проверки FileOps; не заявлять более сильную гарантию, чем есть. Успешная фиксация снимает verified до следующей полной разблокировки; отказ до неё не портит прежний verified.

Обновить прежний комментарий VaultStore о том, что .bak никогда не читается: штатная unlock по-прежнему читает только main, а отдельное восстановление читает .bak лишь как недоверенного кандидата с последующей полной проверкой Application.
- [ ] **4. GREEN:** test_store, включая существующие save/rollback/DACL/reparse tests. Дополнить реальной проверкой DACL страховочной копии тем же способом, что существующий тест main/.bak; Windows replacement занятого файла должен сохранить прежний main.
- [ ] **5. Commit:** `Restore validated backup with an encrypted safety copy`.

### B2. Проверка кандидата и поколение Application

**Consumes:** B1 RestoreResult/restoreBackup; A3 Application::BackupInfo; Container::readMode/openPassword/openHello, VaultModel::fromCbor, SecureBytes и текущий HelloRequest.

**Produces:**

```cpp
// Application public:
Mode backupMode(QString* error);
bool prepareBackupRestore(const QByteArray& backupPassword, QString* error);
#ifdef Q_OS_WIN
bool prepareHelloBackupRestore(void* hwnd, QString* error);
#endif
void cancelBackupRestore();
store::RestoreResult restorePreparedBackup();
// Application private:
struct PreparedBackup {
    QByteArray main, backup;  // только зашифрованные байты
    Mode mode = Mode::None;
    quint64 generation = 0;
};
std::optional<PreparedBackup> preparedBackup_;
```

Для выбора первого запроса пароля/Hello backupMode выполняет строгое ограниченное чтение в worker через действующий waitFor/busy, возвращает None с причиной при ошибке/невалидном заголовке/изменении поколения. Он не пишет файлы и не готовит авторизованного кандидата. BackupInfo остаётся только метаданными A3. Заголовок — подсказка выбора способа, не проверка содержимого. Точная авторизация выполняется prepare*; режим копии может отличаться от текущего. Если файл сменился между подсказкой и prepare, фактическая проверка выбранного режима должна отказать.

- [ ] **1. RED:** настоящий парольный контейнер, две версии модели, затем восстановление:

```cpp
QTemporaryDir dir;
Application app(dir.path());
QString error;
QVERIFY(app.createPasswordVault("valid-password", &error));
const auto group = QUuid::createUuid();
QVERIFY(app.model()->addNode(group, {}, "Original").isEmpty());
QVERIFY(app.save(&error));
QVERIFY(app.model()->addNode(QUuid::createUuid(), {}, "Newer").isEmpty());
QVERIFY(app.save(&error));
QVERIFY(app.prepareBackupRestore("valid-password", &error));
const auto result = app.restorePreparedBackup();
QVERIFY2(result.committed, qPrintable(result.error));
QVERIFY(result.error.isEmpty());
QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
QVERIFY(app.model()->nodes().isEmpty());
QVERIFY(app.unlock("valid-password", &error));
QCOMPARE(app.model()->nodes().size(), 1);
QCOMPARE(app.model()->nodes().value(group).name, QStringLiteral("Original"));
QVERIFY(app.save(&error));
```

- [ ] **2. Проверить RED:** test_app. Дополнить подготовкой с неверным/старым паролем после switchToPassword; валидной AEAD с невалидным CBOR; большим/битым/подменённым кандидатом; canceled prepare; повторным prepare/restore при busy; lock до фиксации и во время финальной замены; подтверждением, что prepare не меняет main/file_/verified/модель. Для гонки финальной замены в Application нужен существующий FileOps seam: добавить необязательный последний аргумент конструктора `store::FileOps& ops = store::realFileOps()` после HelloRequest на Windows, после parent на других платформах. Передать его Store; прежние вызовы совместимы. В тесте — FaultOps из B1 с настоящими временными файлами, а не новый публичный тестовый метод. Callback финального rename из worker вызывает `Application::lock` через BlockingQueuedConnection; GUI обслуживает этот вызов своим waitFor, поэтому гонка воспроизводится до возврата rename. Callback не исполнять с BlockingQueuedConnection на том же GUI-потоке.
- [ ] **3. Реализация:** подготовка проверяет Ready/!busy, сбрасывает прежнего кандидата, снимает generation и строгие снимки; KDF/AEAD/fromCbor выполняются QtConcurrent с прежним локальным ожиданием. Использовать SecureBytes для пароля/открытого CBOR; подготовленный объект хранит только шифротекст/режим/поколение. После ожидания проверяются gate/generation; кандидат принимается только при успехе. Нельзя передать кандидат через Outcome.file текущего execute: тот меняет file_ даже при adopt=false. Использовать отдельный узкий результат подготовки с тем же waitFor/busy и guard сброса busy/пароля на каждом выходе.

```cpp
if (!canStart(Gate::Ready, error)) return false;
cancelBackupRestore();
const auto expected = generation();
// Worker: strict snapshots -> openPassword/openHello -> VaultModel::fromCbor.
// GUI: Ready && generation()==expected -> сохранить только PreparedBackup.
```

restorePreparedBackup потребляет кандидат один раз, проверяет mode/generation/Ready и делает B1 с callback сравнения generation. Успешная замена обновляет hint_ по проверенному режиму и всегда вызывает lock; ключ/модель старого сеанса уничтожаются. При lock, совпавшем с rename, committed сохраняется истинным, gate остаётся NeedUnlock и сообщение описывает совершённую замену без возвращения Ready. Никакое private acceptVerified не вызывается до полноценной последующей unlock. Отказ до фиксации оставляет Ready только если пользователь сам не заблокировал приложение. lock и cancelBackupRestore уничтожают подготовленный снимок.
- [ ] **4. GREEN:** test_app/test_store и прежние password/Hello/lateHello/save tests. Windows Hello — внедрённый HelloRequest и собственный тестовый контейнер; не биометрия владельца. Проверить восстановление копии другого режима и необходимость её старого пароля, повторную попытку после ошибки, работоспособность последующего save и возврат к зашифрованной страховочной копии в новом временном Application.
- [ ] **5. Commit:** `Authenticate backup restoration without reopening a stale session`.

### B3. Подтверждение в настройках и полный маршрут

**Files:** SettingsDialog.*, MainWindow.*, test_settings.cpp/test_e2e.cpp, общий acceptance-отчёт A7.

**Consumes:** SettingsDialog::restoreBackupRequested и dirty-проверка A3/A4; B2 public API; текущий UnlockDialog, `ui::runVaultOperation` и SessionFlow из A6.

**Produces:** действующая кнопка `restoreBackup`, приватный slot `MainWindow::restoreBackup()` и завершённый F4.

- [ ] **1. RED:** открыть реальные настройки на временном vault с .bak, нажать restore, ввести неверный пароль и убедиться, что main прежний и нет успеха. Затем правильный пароль, отмена финального QMessageBox и то же сравнение. Третий маршрут подтверждает замену, наблюдает NeedUnlock и штатный UnlockDialog в SessionFlow.

```cpp
// Снимок файлов делать до кнопки; читать QFile только из QTemporaryDir.
QFile beforeFile(app.storePathForTest());
QVERIFY(beforeFile.open(QIODevice::ReadOnly));
const QByteArray before = beforeFile.readAll();
beforeFile.close();
// QTimer отвечает настоящему password dialog и отменяет финальный QMessageBox.
auto* restoreButton = dialog.findChild<QPushButton*>("restoreBackup");
QVERIFY(restoreButton);
restoreButton->click();
QFile afterFile(app.storePathForTest());
QVERIFY(afterFile.open(QIODevice::ReadOnly));
QCOMPARE(afterFile.readAll(), before);
QCOMPARE(app.gate(), Application::Gate::Ready);
```

Для UI кнопки тест получает SettingsDialog через modalWidget и objectName, как A3; данных контейнера UI не передаёт. Полный прогресс/cancel берётся из `ui::runVaultOperation` задачи A6, без второй реализации в MainWindow.

- [ ] **2. Проверить RED:** test_settings/test_e2e. Обязательны dirty→недоступно, нет копии/недоступный файл→недоступно, prepare error→окно остаётся с причиной, cancel→cancelBackupRestore, lock при password/confirmation→закрытие без позднего commit; повторный клик не начинает второй restore.
- [ ] **3. Реализация:** нажать можно только при чистом черновике, Ready и !busy; сохранить QPointer на диалог, при изменении состояния не обращаться к уничтоженному окну. Режим берётся из проверяемой подсказки B2, пароль запрашивается для копии; после его чтения очистить виджет и SecureBytes. Prepare не является финальным подтверждением: после его успеха отдельный QMessageBox предупреждает о замене и блокировке. Любой выход без commit вызывает cancelBackupRestore через guard. Для подтверждённого действия вызвать restorePreparedBackup, показать фактический committed/error/safetyPath обычным текстом, не общий «отменено» после совершённой записи. После успешной фиксации настройки закрыты, SessionFlow показывает обычную разблокировку. Сочетание всё это время suspended.
- [ ] **4. GREEN:** test_settings/test_e2e/test_app/test_store; просмотреть путь восстановления в settings-demo, используя только временные файлы и вымышленный пароль. Прогонить отказ записи и partial settings вместе: восстановление не должно использовать несохранённый черновик или автоматически его сохранять.
- [ ] **5. Commit:** `Expose confirmed backup recovery in unified settings`.

После B3 перейти к A7: полный свежий набор, независимый обзор, нативная приёмка и поставка. До их завершения текущая сборка владельца не заменяется промежуточным результатом.
