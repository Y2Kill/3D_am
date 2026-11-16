# Задачи на реализацию

Пауза на реализацию снята владельцем 2026-11-10. Роли:
- **Управляющий (Claude Code)** — проектирует, пишет ТЗ задач, принимает результат, сливает ветки, ведёт ADR, спецификацию и планы.
- **Исполнитель (Codex, VS Code, та же папка)** — выполняет задачи по [`AGENTS.md`](../../AGENTS.md).
- **Владелец** — решения по требованиям и по всему, что выходит за принятые ADR.

## Жизненный цикл задачи

`draft` (пишется) → `ready` (можно брать) → `in_progress` (исполнитель работает в ветке `codex/T-NNN`) → `review` (отчёт сдан) → `done` (принято и слито в `main`) или `rework` (замечания в разделе «Приёмка»).

**Приёмка управляющим:**
1. Сверка `git diff main...codex/T-NNN` с разделом «Файлы» и критериями приёмки.
2. Собственный прогон `tools\build_min.cmd`.
3. Слияние в `main` и статус `done` — либо замечания в разделе `## Приёмка` и статус `rework`.

Одна рабочая папка — значит, строгая очерёдность: пока задача `in_progress`, управляющий файлы не меняет.

## Шаблон задачи

```markdown
# T-NNN. Название
**Статус:** ready · **Веха:** M… · **Основание:** ADR-…, спецификация §…

## Цель
## Контекст (что прочитать)
## Требования
## Файлы (что можно менять)
## Критерии приёмки
## Вне объёма
```

## Реестр

| ID | Задача | Статус | Зависит от | Веха |
|---|---|---|---|---|
| [T-001](T-001-foundation-adr0035.md) | Foundation по ADR-0035: `Uuid::v4`, `Errc::incomplete`, пути `content/<uuid>/`, причины переименования и суффикс коллизии по формату | done | — | M1, шаг 0 |
| [T-002](T-002-unicode-foundation.md) | Unicode в foundation: NFC и simple case folding (Unicode 17.0.0, данные UCD в репозитории); коллизии путей по ADR-0035 п. 8; защита бюджета длины пути от переполнения | done | загрузка UCD разрешена 2026-11-10 | M1, шаг 0 |
| [T-003](T-003-vcpkg-baseline.md) | Базовая линия vcpkg: клон реестра в `.cache/vcpkg`, `builtin-baseline`, сборка `win-cli`, офлайн-повтор с `x-block-origin` | done | загрузка разрешена 2026-11-10 | M0 |
| [T-004](T-004-spike-s7-exact-json-numbers.md) | Спайк S7: точные числа непрозрачного JSON — кандидаты A–E (nlohmann DOM/SAX, RapidJSON, собственный токенизатор, jsoncons), полный цикл ZIP, V2 по схеме 2020-12, замеры (каталог `spikes/s7/`) | done | загрузка портов vcpkg разрешена 2026-11-10 | M0 |
| [T-005](T-005-ci-github.md) | CI на GitHub Actions: `win-debug-min`, `win-cli`, `linux-min`, `linux-cli`; общие скрипты с ключом `--ci` (`AM_REQUIRE_PYTHON_TESTS=ON`, JUnit), vswhere, `tools/build_linux.sh`, первая сборка всего проекта на Linux; actions по SHA; проверка локально + WSL; публикация в Y2Kill/3D_am — управляющий (`tools/publish_public.sh`) | done (2026-11-13; GitHub зелёный) | T-003, T-012 | M0 |
| [T-006](T-006-manifest-codec.md) | Кодек `manifest.json` по ADR-0038: строгий токенизатор в foundation, гибрид D2 в `am_domain`, V2 на jsoncons, защита записи по ключам владельцев; `vcpkg.json`: jsoncons вместо json-schema-validator | done | ADR-0038; загрузка порта jsoncons разрешена | M1 |
| [T-007](T-007-zip-reader.md) | Безопасное чтение ZIP-контейнера в `am_storage`: собственный разбор структуры (EOCD, ZIP64, каталог, сверка локальных заголовков, перекрытия), потоковый inflate zlib с CRC и бюджетом, нормативный профиль 1.0, `probe_container` | done | T-006 | M1 |
| [T-008](T-008-zip-writer.md) | Запись ZIP-контейнера в `am_storage`: собственный писатель (Stored/Deflate, ZIP64 по правилам читателя, без data descriptor), raw-копия с проверкой тех же байтов одним проходом, политика extra fields «не вслепую», `verify_written`, приёмник с внедрением отказов | done | T-007 | M1 |
| [T-009](T-009-commit-protocol.md) | Протокол фиксации ZIP (ADR-0006): журнал `pending_ops` в `state.sqlite`, проверка места, временный файл, проверка до замены, POSIX-переименование с открытым читателем, восстановление, kill-тесты; спайки S5/S5+ в части NTFS | done | T-008 | M1 (+ M0: S5, S5+ NTFS) |
| [T-010](T-010-asset-repository.md) | `AssetRepository`: снимки с `FILE_SHARE_DELETE`, `ChangeRequest` с `expected_rev` и коммутирующими операциями, ревизии и `history`, no-op, манифест первой записью, проверка V1/V2/V4 и инварианты V3, `LibraryMode`/`StorageCapabilities` по `fs_support.md`, сценарий O-13 | done | T-009 | M1 |
| [T-011](T-011-v3-validator.md) | Валидатор V3 в `am_domain` по закрытому каталогу правил (идентичность, файлы и происхождение, группы и превью, поля и значения, claims, зависимости, время и история) с находками `error`/`warning`/`unverifiable`; подключение к `AssetRepository` с правилом фиксации; замечания приёмки T-010 (коды ошибок, повтор `op_id`, съёмные NTFS, ранняя проверка путей) | done | T-010 | M1 |
| [T-012](T-012-cli-asset.md) | CLI `am asset create/inspect/validate/verify/extract` (M1) поверх фасада `am_application`: отдельный ZIP вне библиотеки, `create` через служебную библиотеку и перенос без замены, общая с репозиторием `verify_container`, `--json`, коды выхода 0–4, каталог сообщений `en.toml`, `CHANGELOG.md`; `am library …` — M2 | done (принята 2026-11-12, `f3abc5b`) | T-011 | M1 |
| [T-013](T-013-golden-zip-test-infra.md) | Golden ZIP формата 1.0 (независимый Python-генератор, положительные/отрицательные по V1–V4 и структуре, `expected.json` + `schemas/expected.v1.schema.json`, ctest `golden_zip`/`golden_regen_check`), отчёт трассировки `req_report.py`; не-ZIP → код 1 (замечание T-012) | done (принята 2026-11-13, `959eb6f`) | T-005, T-012, T-014, T-015 | M1 |
| [T-014](T-014-v3-adr0039.md) | Правила V3 по ADR-0039: физический `default` `{value, unit}`, `claims[].unit`, `change_kind_fields`, согласованность каталогов в описи; удаление `x02_physical_default_open` и `claim_value_unit_unexpressible`; тест extra fields писателя | done (принята 2026-11-13, `5728a1b`) | T-005, T-011 | M1 |
| [T-015](T-015-raw-copy-foreign-extra.md) | Изменение ассета с чужими extra fields/комментарием записи: `RawCopyOptions::drop_unlisted`, счётчики отброшенного в `WrittenEntry`, `AssetRepository` не отказывает (ADR-0039 п. 7; находка приёмки T-014) | done (принята 2026-11-13, `a1ff004`) | T-014 | M1 |
| [T-016](T-016-library-foundation.md) | Основа библиотеки (M2, P0.5 — часть 1): раскладка ADR-0007 и `am-library.toml`, владелец записи `.am/library.lock` (ADR-0031), режим «только чтение» без изменения файлов, три базы с `user_version`, каркас миграций с `VACUUM INTO`, пересоздание индекса; `pending_ops_new` в `state.sql`; CLI `am library init/info` | done (принята 2026-11-15, `17eff39`) | T-010, T-013 | M2 |
| [T-017](T-017-indexer-rebuild.md) | Индексатор и перестройка индекса (M2, P0.5 — часть 2): RAII-обёртка SQLite и `IndexStore` в `storage`, `catalog::Indexer` в `services` (манифест → `index.sql`, точные `uint64`, ключ тегов — тождественный до O-08), `rebuild` одной транзакцией без изменения `userdata`/ZIP, обновление после фиксации через `on_indexed`; CLI `am library rebuild/list` | done (принята 2026-11-16, `da7bf68`) | T-016 | M2 |
| [T-018](T-018-sync-stat.md) | Сверка библиотеки по `stat` и `am library sync` (M2, P0.5 — часть 3): перечитывание только изменившихся ZIP, пометки `missing`/`corrupt` вместо удаления, одна транзакция, инвариант «sync = rebuild за вычетом пометок», `--full`; индексирование newer_minor с `health = warn` (замечание 1 приёмки T-017); efsw — вне объёма (открытый п. 4 §4.9 ТЗ) | review | T-017 | M2 |

**Порядок:** T-003 → T-002 → T-004 (приняты; итог S7 — ADR-0038) → T-006 → T-007 → T-008 → T-009 → T-010 (приняты); → T-011 → T-012 (приняты; веха M1 по CLI закрыта). Команды библиотеки — M2. → T-005 (CI GitHub, принята 2026-11-13) → T-014 (V3 по ADR-0039, принята 2026-11-13) → T-015 (чужие extra fields при изменении, принята 2026-11-13) → T-013 (golden ZIP, трассировка, принята 2026-11-13) → публикация формата 1.0 (выполнена 2026-11-14) → T-016 (основа библиотеки, M2, принята 2026-11-15) → T-017 (индексатор и `rebuild`, принята 2026-11-16) → T-018 (`sync` по `stat`). Папка одна, поэтому задачи выполняются строго по одной.

Спайки — эксперименты, а не код продукта: живут в `spikes/<sN>/`, в сборку продукта не входят, итог каждого оформляется отдельной ADR.
