# Windows x64: единые настройки и восстановление, 2026-10-24

Исходный commit продукта: ade83a0. Версия 1.0.0, MSVC/Qt 6.11.2/libsodium из установленного toolchain. Изменение локально включено в main; удалённой публикации нет.

## Проверки

- Release tools/build.cmd: exit 0.
- Полный tools/test.cmd --output-on-failure с тестовым opt-in буфера: 20/20, 81,44 с; все свежие журналы QtTest 0 failed/0 skipped. E2E 15, Session 15, Settings 45, App 44, Store 37, Env 9 pass.
- Независимый обзор 2328280..7fe1f1b: 0 Critical, 3 Important, 0 Minor. Три Important исправлены в ade83a0 с наблюдаемыми RED→GREEN. Нативный отказ доступа проверен только на уникальном временном ключе HKCU, его DACL восстановлен и ключ удалён. Причина недостаточности QSettings подтверждена в исходнике Qt 6.11.2: https://github.com/qt/qtbase/blob/v6.11.2/src/corelib/io/qsettings_win.cpp#L418 .
- tools/package-windows.cmd: exit 0. Перед внутренним rmdir проверен абсолютный каталог worktree/dist/TokenBucket и отсутствие reparse point; пользовательские процессы не завершались.
- ZIP распакован в отдельный временный каталог; все 69 файлов совпали с пакетом по SHA-256. С очищенным PATH и только DLL/плагинами извлечённого пакета прошли синтетические тесты: session 15, settings navigation/recovery 15, restore/quit 4, native registry 3 pass; все 0 failed/0 skipped. Тестовые EXE добавлялись только в распакованную проверочную копию, в поставке их нет.

Проверка штатного production EXE с данными владельца не запускалась. Синтетические тесты вызывают тот же runSession и UI на временных vault/INI/Run.

## Поставка

Постоянный каталог: D:/Sources/gitverse/TokenBucket/dist/TokenBucket/ . Все 69 конечных файлов сравнены с проверенным пакетом; полный манифест: 2026-10-24-settings-sha256.txt.

ZIP: D:/Sources/gitverse/TokenBucket/dist/TokenBucket-windows-x64.zip . SHA-256: 5E6E03E645F452B98CB64B5D14B01A99086ECF88CFF588E2D5489A20EFDCA432 .

TokenBucket.exe SHA-256: BC94D0625A45155DEA9AC219B2C70C6704A928685442748B95577E8A05FA6E57 .

Предыдущая сборка сохранена в D:/Sources/gitverse/TokenBucket/dist/TokenBucket.previous-20261024-063821Z/ ; все 69 прежних файлов сверены до переноса новой версии. Прежнего ZIP в постоянном каталоге не было; никаких старых ZIP не удаляли. Проверенный новый ZIP создан и сверён после копирования.

## Ограничения

EXE без цифровой подписи. Реальные Hello, физическое изменение DPI и Linux не проверены. 200% проверены масштабом Qt, не настройками Windows. Системное окно гонки SendInput после последней проверки фокуса сохраняется. Новые типы хранения/v4 и реальная миграция данных не входят в этот пакет; их план утверждён отдельно.
