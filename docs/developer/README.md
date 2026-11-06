# Руководство разработчика

## Сборка и тесты
- Минимальная сборка (без vcpkg, GUI, AI и тяжёлого 3D): `tools\build_min.cmd` — пресет `win-debug-min`. Все временные файлы и кэши направляются в `.cache/`.
- Остальные конфигурации ТЗ §12.6.6: пресеты `win-cli`, `win-debug`, `win-release`, `win-full-ai`, `win-asan`, `linux-cli`, `linux-min` (`CMakePresets.json`).

## Правила уровней (ADR-0024)
- Код уровня `X` лежит в `src/X/` и `include/am/X/`. Зависимости разрешены только по матрице ADR-0024.
- Интерфейс, нужный нескольким уровням, размещается в `contracts/`. Реализация — на своём уровне.
- `foundation` не использует сторонние библиотеки. Внешние библиотеки скрываются за собственными интерфейсами.
- Действующие ZIP меняет только `storage::AssetRepository` (ADR-0025).
- Команды объявляются через `contracts::Command` с уровнем `permission` и регистрируются через `ICommandSink` (ADR-0027).
- Проверка: ctest-тест `arch_check` (`tools/arch_check/arch_check.py`). Нарушение — архитектурный дефект.

## Тесты
- Юнит-тесты: `tests/unit/test_<тема>.cpp`. Требования ТЗ помечаются через `RecordProperty("req", "K-n")` (ADR-0029).
- Синтетические наборы — `tests/fixtures/`, реальные — реестр `tests/real/registry.json` (только чтение).
- После изменения статуса сценария обновляется `docs/plan/coverage_matrix.md`.

## Ошибки
`am::Result<T>` / `am::Error{Errc, message, context}`. Исключения не пересекают границы модулей и C ABI плагинов.
