# TDD для локального доступа агента

Сценарии выведены из запроса на доступ агента к данным и SSH-ключам TokenBucket. Первичная реализация прямого помощника появилась до явного запроса на ECC TDD Workflow. Тесты MCP добавлены до его реализации.

| Требование | Проверка | RED | GREEN |
| --- | --- | --- | --- |
| `find` и последующий `read` работают подряд | `test_agent_win::protocolAndOwnerApproval` запускает помощник без паузы после успешного поиска и между разрешёнными чтениями | Первое чтение вернуло код выхода `1` вместо `0` | `PASS` |
| Помощник отклоняет два запроса в одном stdin | `test_agent_win::rejectsExtraStdinRecords` передаёт два корректных JSON-запроса и ожидает `invalid_request` | `locked` вместо `invalid_request` | `PASS` |
| MCP-клиент обнаруживает инструменты и читает разрешённое поле SSH | `test_agent_win::mcpLegacyDiscoveryAndCalls` проверяет рукопожатие 2025-11-25, поиск и повторную проверку перед чтением точных байтов | В ответе `initialize` отсутствует `jsonrpc: "2.0"` | `PASS` |
| MCP-клиент обнаруживает функции сервера и отменяет запрос | `test_agent_win::mcpModernCancellationAndEof` проверяет `server/discover` и метаданные 2026-07-28, отмену активного согласия и закрытие stdin | Для некорректного JSON отсутствует код `-32700` | `PASS` |
| Отмена ждёт обработки накопившихся входящих сообщений до выдачи | `test_agent_win::mcpProcessesQueuedCancellationBeforeResult` отправляет отмену за кадром 60 КиБ | Преждевременно получен результат ID `1` вместо следующего ID `2` | `PASS` |
| EOF завершает MCP даже при заполненном stdout | `test_agent_win::mcpEofExitsWithFullOutputPipe` запускает помощник с непрочитанным унаследованным каналом | Процесс оставался активен после 1500 мс | `PASS` |
| Метаданные 2026 исключают устаревший `initialize` | `test_agent_win::mcpModernTrafficRejectsLegacyInitialize` передаёт оба контракта в одном запросе | Вместо `-32601` выполнено legacy-рукопожатие | `PASS` |
| Ошибка сохраняет однозначный ID запроса | `test_agent_win::mcpMalformedRequestKeepsUnambiguousId` проверяет неправильный тип метода и повторяющийся идентификатор | В `-32600` потерян идентификатор `synthetic-request` | `PASS` |

`test_agent_win` использует временное хранилище, реальный локальный канал Windows и отдельно запущенный помощник из того же исходника с изолированным именем канала. Проверка охватывает отказ в реальном диалоге, повторную проверку мастер-паролем, SSH с исходными байтами до 1 МиБ, поля других типов, видимость строк, совпадение снимка записи, блокировку, обрыв соединения и одно активное подтверждение. Путь Windows Hello проверяется с подставным системным верификатором и настоящим хранилищем DPAPI, включая поздний успех после обрыва соединения.

Инструменты проекта — CMake, vcpkg и CTest. Команда фокусной проверки Windows: `tools\test.cmd -R "^test_agent_win$"`. Сборка, тесты и измерение покрытия выполняются на локальном компьютере.

В RED (`d8502b9`) сборка `cmake --build --preset default --target test_agent_win --parallel 12` завершилась успешно. CTest вернул код `1`, а QtTest сообщил `4 passed, 4 failed`. После изменений та же сборка и CTest завершились успешно. QtTest сообщил `8 passed, 0 failed`, CTest прошёл за 15,04 с. Вывод сохранён локально в `build/evidence/test_agent_win-red.txt` и `build/evidence/test_agent_win-green.txt`.

После независимого ревью четыре новых теста были собраны и запущены через `build/Release/test_agent_win.exe` с именами методов из таблицы. Второй RED (`eecf835`) дал `2 passed, 4 failed`. После исправлений полный `test_agent_win` дал `12 passed, 0 failed` за 15,61 с. Вывод сохранён локально в `build/evidence/test_agent_win-hardening-red.txt` и `build/evidence/test_agent_win-hardening-green.txt`.

## Привязки ключей SSH к машинам

Новые тесты проверяют один ключ на нескольких машинах, несколько ключей на машине, фильтрацию пользователя и порта в одной привязке, ограничения размера, проверку согласия, безопасный переход с v5 и передачу результата в MCP. Сборка `cmake --build --preset default --target test_model --parallel 12` дала ожидаемый compile-time RED. MSVC выдал `C2039` для отсутствующих `Entry::sshTargets` и `VaultModel::sshMatches`. Вывод сохранён локально в `build/evidence/ssh-targets-red.txt`.

После реализации локальная сборка целей `test_model`, `test_app` и `test_agent_win` завершилась успешно. CTest выполнил `test_model` и `test_agent_win` с результатами `28 passed` и `13 passed` соответственно. Проверка `test_app.exe v5SshVaultUpgradePreservesEncryptedOriginalAndBindings` прошла отдельно, включая проверенную зашифрованную копию, отказ при ошибке её создания, повторное открытие v6 и экспорт с импортом.

В запуске полного `test_app` единственный отказ пришёлся на существующий `upgradeFailures(symlink-main)`. Windows вернула `FALSE` при создании символической ссылки в тестовом окружении. Этот отказ сохранён в отчёте `build/evidence/ssh-targets-green-test_app.txt`. Вывод остальных проверок находится в локальных `build/evidence/ssh-targets-green-*`.

После явного разрешения владельца включён режим разработчика Windows (`AllowDevelopmentWithoutDevLicense=1`). Повторная изолированная проверка символической ссылки вернула Win32 `0` вместо `1314`. Локальная сборка `test_app` и `test_store` и `tools\test.cmd -R "^(test_app|test_store)$"` завершились с кодом `0`: 75 и 38 проверок соответственно, без ошибок и пропусков. Журналы и сведения об изменении настройки ОС — `build/evidence/developer-mode-20261118-054754/`.

## Таблица SSH-подключений

После выбора макета собрана цель `test_ui`. Запуск `test_ui.exe sshConnectionsTablePersistsAndPreservesKey sshConnectionsTableBoundsAndTypeChange sshCardBindingsRemainMetadataWhileKeyRequiresVerification` дал `2 passed, 3 failed`. Все три сценария остановились при поиске ещё отсутствовавших элементов редактора и карточки. Журнал сохранён в `build/evidence/ssh-ui-red.txt`.

После реализации повторный запуск трёх сценариев и `aboutDialogShowsBrandAndStack` дал `6 passed, 0 failed`. Проверены сохранение и повторное открытие, отмена удаления, сохранность всех полей ключа, переход Tab между ячейками, ограничение в 16 строк, подтверждение смены типа и проверка владельца перед копированием ключа. Журнал — `build/evidence/ssh-ui-green.txt`. Светлая, тёмная и узкая версии таблицы сохранены в `build/evidence/ssh-table-*.png`.
