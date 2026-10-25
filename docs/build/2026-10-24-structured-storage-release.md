# Локальный релиз: структурированное хранение

Дата: 2026-10-25. Исходный коммит: d1478e8 (main). Сборка: worktree codex/structured-storage, tools\package-windows.cmd exit 0.

- Постоянный каталог: D:/Sources/gitverse/TokenBucket/dist/TokenBucket/ — 69 файлов, SHA-256 каждого совпадает с пакетом сборки.
- ZIP: dist/TokenBucket-windows-x64.zip, 61 444 702 байта, SHA-256 f86451460282a8d71db6d7c538723748ec1499228e08ee3f78c901f0fc41b165.
- TokenBucket.exe SHA-256 ee0d13813b66bed325296ec2fc285b2652aa1c832e9b52378a079692fc78978f.
- Прежняя сборка сохранена: dist/TokenBucket.previous-20261025-180503Z и dist/TokenBucket.previous-20261025-180503Z.zip.
- Зависимости: dumpbin /dependents по EXE и всем DLL пакета — все несистемные библиотеки в пакете, включая Qt6Network.dll; вне пакета только системные DLL Windows.
- EXE без цифровой подписи. Пакет не запускался из агентской сессии; реальный vault владельца не открывался и не мигрировался. Push не выполнялся.

Внимание владельцу: первое сохранение старого хранилища (v1–v3) попросит согласие на формат v4 и создаст рядом vault.tb.before-upgrade-*; прежний EXE v4 не откроет.
