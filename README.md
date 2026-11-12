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

## Build and CI

Requirements: CMake ≥ 3.28, Ninja, Python 3.12 and a C++20 compiler.
The test wheel hashes also cover Python 3.14 on Windows for local builds.
Windows scripts find Visual Studio 2022 C++ tools with `vswhere` (Community or Build Tools).
Linux CI uses Ubuntu 24.04 and GCC 13. Minimum builds fetch GoogleTest into `.cache/`;
CLI builds use the exact vcpkg `builtin-baseline` from `vcpkg.json`.

Install the test modules into a project-local directory:

```sh
python -m pip install --require-hashes --no-deps --target .cache/pylib -r tools/ci/requirements-test.txt
```

Windows:

```bat
tools\build_min.cmd
tools\ci\bootstrap_vcpkg.cmd
tools\build_cli.cmd
tools\build_cli.cmd --offline
```

Linux (including WSL):

```sh
tools/build_linux.sh min
tools/ci/bootstrap_vcpkg.sh
tools/build_linux.sh cli
tools/build_linux.sh cli --offline
```

On a minimal Ubuntu installation, install `build-essential cmake ninja-build git curl zip unzip tar pkg-config perl`.
Python needs pip to install the test modules (`python3-pip` on Ubuntu; supplied by `setup-python` in CI).
Bootstrap is idempotent and disables vcpkg metrics. Windows and Linux use separate vcpkg clones,
installed packages and binary caches, and share the content-addressed source cache in `.cache/vcpkg-assets`.
Linux keeps FetchContent build files in `.cache/fetchcontent/linux` so WSL can use the same checkout as Windows.
Python modules with native wheels must also be installed separately when sharing a checkout:
install Linux modules into `.cache/pylib-linux` and set `AM_PYTHON_EXTRA_PATH` to that directory for Linux builds.

Use the scripts for vcpkg presets: they keep temporary files and caches inside the checkout.
`--offline` makes vcpkg caches read-only and blocks origin downloads with `x-block-origin`;
minimum builds instead require an already populated GoogleTest cache. GUI/AI presets remain unverified.
Linux repository writes remain disabled by default under [the filesystem policy](docs/specifications/fs_support.md);
read, validate, verify and extract commands are available.

The [GitHub Actions workflow](.github/workflows/ci.yml) runs `win-debug-min`, `win-cli`, `linux-min` and `linux-cli`
with the same scripts and `--ci`. This flag requires Python checks at configure time
(`AM_REQUIRE_PYTHON_TESTS=ON`) and writes `build/<preset>/junit.xml`.
It combines with `--offline` in either order; further arguments are passed to ctest, for example:

```bat
tools\build_cli.cmd --ci --offline -R cli_
```

CI runs the complete test suite and retains JUnit reports for 14 days; failed jobs also upload `LastTest.log`.
Python checks include architecture, Unicode tables, manifest schemas and SQL contracts; CLI builds also
check the message catalog and command-line interface. Without `--ci`, a missing `jsonschema` still fails
`schema_check`; the explicit local opt-out is `AM_ALLOW_PYTHON_SKIP=ON` in CMake, which CI rejects.

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
