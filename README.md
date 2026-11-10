# AM — каталогизатор и менеджер 3D/2D-ресурсов

Модульная программа на C++20 для каталогизации ресурсов для 3D-печати, ЧПУ и лазерной резки: импорт из архивов в самодостаточные ZIP-ассеты, метаданные и происхождение, поиск дубликатов, просмотр, конвертация.
Техническое задание: [`docs/tz/drafts/TZ_catalog_3D_2D_draft_v1.8.md`](docs/tz/drafts/TZ_catalog_3D_2D_draft_v1.8.md) (исходное описание — [`docs/tz/description.txt`](docs/tz/description.txt); все черновики v0.1–v1.7 — в `docs/tz/drafts/`).

## Состояние: подготовка к реализации завершена (M0, частично)

| Что | Где |
|---|---|
| Навигация по документам | [docs/00_INDEX.md](docs/00_INDEX.md) |
| Отчёт о потерях при эволюции ТЗ | [docs/reports/01_TZ_evolution_loss_report.md](docs/reports/01_TZ_evolution_loss_report.md) |
| Выводы по реальной коллекции | [docs/reports/02_collection_findings.md](docs/reports/02_collection_findings.md) |
| Сверка ТЗ v1.3–v1.5 с подготовкой (принято владельцем) | [docs/reports/03_TZ_v1.3-1.5_vs_prepared.md](docs/reports/03_TZ_v1.3-1.5_vs_prepared.md) |
| Сверка ТЗ v1.7 (НФТ §14) и v1.8 (§19, формат ZIP) с подготовкой (приняты владельцем) | [docs/reports/04_TZ_v1.7_vs_prepared.md](docs/reports/04_TZ_v1.7_vs_prepared.md), [docs/reports/05_TZ_v1.8_format_vs_prepared.md](docs/reports/05_TZ_v1.8_format_vs_prepared.md) |
| Сверка уточнений формата R-01…R-09 (приняты владельцем, ADR-0036) | [docs/reports/06_R01-R09_vs_prepared.md](docs/reports/06_R01-R09_vs_prepared.md) |
| Проверка журнала решений v0.11, 108 принципов (приняты владельцем, ADR-0037) | [docs/reports/07_RC1_v0.11_vs_prepared.md](docs/reports/07_RC1_v0.11_vs_prepared.md) |
| Формат ZIP-ассета 1.0 (рабочая формализация, не опубликован) | [docs/specifications/container_manifest_v1.md](docs/specifications/container_manifest_v1.md) |
| Матрица покрытия сценариев | [docs/plan/coverage_matrix.md](docs/plan/coverage_matrix.md) |
| Руководство разработчика | [docs/developer/README.md](docs/developer/README.md) |
| Исследование библиотек (§17 ТЗ) | [docs/research/libraries.md](docs/research/libraries.md) |
| Архитектурные решения | [docs/decisions/ADR_log.md](docs/decisions/ADR_log.md), [решения по открытым вопросам](docs/decisions/open_questions_resolution.md) |
| План | [docs/plan/roadmap.md](docs/plan/roadmap.md) |

## Сборка (база, без внешних зависимостей)

Нужны Visual Studio 2022 (MSVC 14.4x) и CMake ≥ 3.28. GoogleTest скачивается в `.cache/` внутри проекта.

```bash
tools\build_min.cmd
```

Или вручную, из «x64 Native Tools Command Prompt»:

```bash
cmake --preset win-debug-min && cmake --build --preset win-debug-min && ctest --preset win-debug-min
```

Минимальная сборка включает тесты `arch_check` (проверка уровней ADR-0024), `schema_check` (черновая JSON Schema манифеста, ADR-0036/0037) и `sql_check` (SQL-контракты, точный `uint64` в `index.sql`, ADR-0037); нужен Python 3, для `schema_check` — модуль `jsonschema` (ищется также в `.cache/pylib`). Без модуля тест **проваливается**; явный локальный отказ — `-DAM_ALLOW_PYTHON_SKIP=ON` (тест получает SKIP); в CI `-DAM_REQUIRE_PYTHON_TESTS=ON` делает отсутствие ошибкой конфигурации. Сборка с зависимостями vcpkg (пресет `win-cli`, baseline — релиз vcpkg `2026.10.16`):

```bash
tools\build_cli.cmd
```

Скрипт ожидает клон vcpkg в `.cache/vcpkg` на теге `2026.10.16` (`bootstrap-vcpkg.bat -disableMetrics`) и держит загрузки, бинарный и asset-кэш в `.cache/`. Ключ `--offline` собирает только из кэшей проекта: загрузки из сети запрещены (`x-block-origin`). Пресеты на основе `vcpkg` запускайте **только через скрипт**: сами пресеты кэши не задают, и без скрипта vcpkg положит кэш в профиль пользователя. Пресеты `win-debug`, `win-release`, `win-full-ai`, `linux-cli` ещё не проверялись.

## CLI (диагностика, этап M0)

```bash
am probe <файл>             # тип по сигнатуре + статистика STL
am volumes <папка>          # кандидаты многотомных архивов (вкл. суффикс копии " (N)")
am zip-path <root> <путь>   # как исходный путь ляжет внутрь ассета
am fix-name "ΓÇô"           # починка mojibake CP437 -> UTF-8
am exclude <путь> [файл]    # вердикт правил исключения служебных файлов
```

## Структура (уровни ТЗ §12.6, ADR-0024)

```
src/<level>/, include/am/<level>/   foundation → contracts → domain → jobs / storage / processing
                                    → services / automation / plugin_runtime / ai → application
src/application/   публичный API, cli/, gui/, worker/
sdk/include/       отдельный SDK плагинов (C ABI — кандидат, ADR-0026)
plugins/           форматы и адаптеры, examples/
schemas/           JSON Schema манифеста, SQL-схемы     profiles/   профили импорта (TOML)
tests/             unit, integration, regression, fixtures, performance, plugins, real
cmake/             modules/, toolchains/                packaging/  windows/, portable/
docs/              документация, ТЗ (docs/tz)           tools/      arch_check, аудит ТЗ, сканер коллекции
data/              результаты инвентаризации коллекции (data/inventory)
```
