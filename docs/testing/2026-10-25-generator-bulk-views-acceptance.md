# Приёмка: F1–F3 (генератор, массовые действия, представления)

Дата: 2026-10-25. Коммиты 34f626b…6d9a283 + правка формулировок; влиты в main fast-forward. Спецификация: docs/superpowers/specs/2026-10-25-generator-bulk-views-design.md; план: docs/superpowers/plans/2026-10-25-generator-bulk-views.md.

## Покрытие спецификации

| Раздел | Проверки |
| --- | --- |
| §2 генератор | test_crypto `generatorHonoursOptions` (1 000 паролей длины 8 со всеми группами, «исключать похожие», длина 128, отказ пустого набора и длин 7/129); test_preferences `generatorOptionsRoundtripAndFallback`; test_ui `passwordGeneratorOptionsApplyAndPersist` (меню параметров, сохранение во временные настройки, ошибка без групп) |
| §3 массовые действия | test_ui `bulkActionsAreAtomic` (перемещение, лимит 16 тегов откатывает всё и не меняет файл, снятие тега без учёта регистра, удаление с «Нет»/«Да», карточка без значений), `bulkMenuDialogsUseCapturedSelection` (меню «Записи», блокировка во время диалога), `bulkDeleteAsksOwnerForProtected`, `bulkActionCanceledByLock` |
| §4 представления и CBOR v5 | test_model `savedViewsRoundtripV5` (побайтовое совпадение писателей, лимиты, дубликаты), `savedViewsVersionGates` (v4 без views, отказ views в v4, отсутствующих/лишних/битых полей, v6); test_app `v4VaultUpgradesToV5WithConsent`; test_storage_ui `savedViewsCaptureApplyAndSurviveLock`, `savedViewMenuRenamesUpdatesAndDeletes`, `savedViewOnLegacyFileAsksConsent`, `savedViewsFitNarrowWindow` (светлая/тёмная, 420×480 и 900×600) |

## Прогоны

- Release build exit 0 после каждой задачи; полный CTest после задач 1, 3, 4 — 23/23 (546, 556, 560 passed, 0 failed/0 skipped); после задачи 2 — 22/23 с известным перехватом переднего плана в test_e2e.
- Финальный прогон итогового кода: 22/23, 559 passed. test_e2e `quickPasteFullRoute` не получил передний план: в момент прогона активным был Яндекс Браузер владельца, Windows отклонила SetForegroundWindow тестового получателя. Код вставки после зелёного прогона задачи 4 не менялся (изменены две строки текста); маршрут проходил в трёх полных прогонах этого дня. Для подтверждения нужен прогон без активной работы за машиной.

## Обзор

Агент-рецензент недоступен (провайдер не найден); обзор вручную по Review Focus плана. Critical/Important не найдено. Исправлено: согласование числа («Выбрано записей: N», «Удалить выбранные (N)…»). Компоновка представлений перенесена в строку «Новая запись» после того, как первый вариант сжимал дерево проектов на 420×480 до 24 px.

## Не проверено

Ручной прогон владельцем в живом приложении (меню генератора, массовые действия, представления), физический DPI, Linux.
