# Инструментарий сборки Windows

Зафиксировано: 2026-10-18 (Задача 1 плана `docs/superpowers/plans/2026-10-18-tokenbucket-v1.md`).

| Компонент | Версия | Как получено |
|---|---|---|
| VS BuildTools | 18.11.0 (MSBuild 18.11.0-1.26475.11) | winget `Microsoft.VisualStudio.BuildTools`, workload VCTools |
| MSVC (cl) | 19.51.36260 for x64 | поставка VS BuildTools |
| Windows SDK | входит в VS 18.11.0 (`catalog_productDisplayVersion` 18.11.0) | VS BuildTools |
| CMake | 4.3.1-msvc1 | поставка VS (используется из vcvars64) |
| Ninja | 1.13.2 | поставка VS (генератор — Visual Studio/MSBuild) |
| vcpkg baseline | `664933b8d385dfc01f4e0cdff4c7c74e71f6a740` | `git rev-parse HEAD` клона `tools/vcpkg` |
| Qt | 6.11.2 (порт `qtbase`, features: widgets, testlib, dbus, icu, …) | vcpkg manifest, triplet x64-windows |
| libsodium | 1.0.22#1 (порт `libsodium`) | vcpkg manifest, triplet x64-windows |

Сборка: `cmd /c tools\build.cmd` (конфигурация + Release-сборка), тесты: `cmd /c tools\test.cmd`
(CTest, пресеты CMake `default`, окружение тестов — `QT_QPA_PLATFORM=offscreen`).

Первая конфигурация (сборка qtbase и зависимостей vcpkg) заняла ~84 минуты; повторные сборки — минуты.
