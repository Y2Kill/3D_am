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
| [T-018](T-018-sync-stat.md) | Сверка библиотеки по `stat` и `am library sync` (M2, P0.5 — часть 3): перечитывание только изменившихся ZIP, пометки `missing`/`corrupt` вместо удаления, одна транзакция, инвариант «sync = rebuild за вычетом пометок», `--full`; индексирование newer_minor с `health = warn` (замечание 1 приёмки T-017); efsw — вне объёма (открытый п. 4 §4.9 ТЗ) | done (принята 2026-11-16, `bda5c19`) | T-017 | M2 |
| [T-019](T-019-trash.md) | Корзина библиотеки (M2, P0.5 — часть 4): `AssetRepository::trash/restore/purge` — переименование без замены и удаление с журналом `pending_ops` и `recover()`, корзина как местоположение (байты и ревизия не меняются), конфликты без замены, индекс через `on_indexed`, размер корзины в `info`; CLI `am library trash/restore/purge` (`purge` только с `--yes`) | done (принята 2026-11-16, `6a7f96d`) | T-018 | M2 |
| [T-020](T-020-add-ready.md) | Импорт готового ассета (M2, P0.5 — часть 5): `AssetRepository::import_ready` — побайтовая копия через общий протокол фиксации (`kind = import_ready`), полный V1–V4 на временной копии, newer_minor без изменения; классы конфликта ID (`identical`, `newer/older_revision`, `same_revision_different`, `current_unreadable`, `in_trash`), замена только явно по SHA-256 увиденного манифеста; CLI `am library add-ready`; независимая копия — T-021 | done (принята 2026-11-16, `18307ef`) | T-019 | M2 |
| [T-021](T-021-independent-copy.md) | Независимая копия ассета (M2, P0.5 — часть 6): `AssetRepository::copy_independent` — новый AssetID, ревизия 1 `asset_created` с `independent_copy` и `source_asset`, новые UUIDv4 всех локальных ID с переназначением ссылок по таблице схемы 1.0, обязательная проверка «старых ID в копии нет», raw-перенос записей; отказы P0 (O-05…O-07: расширения, plugin-значения, `historical_files`, `derived_from` на удалённые, непрозрачные записи); CLI `am library copy` | done (принята 2026-11-17, `b76f829`) | T-020 | M2 |
| [T-022](T-022-open-recovery-verify.md) | Восстановление при открытии и проверка библиотеки (M2, P0.5 — часть 7): `Catalog::open` в режиме записи вызывает `recover()`, переиндексирует затронутые ассеты (`Indexer::refresh`), по отметке незакрытой сессии в `meta` индекса выполняет `sync`; `Catalog::verify` быстрый/полный (§4.7) с прогрессом и отменой, отражение `corrupt` в индексе без удаления ZIP; CLI `am library verify`; тест самодостаточности ZIP (§13.4 п. 4); `purge --all` без `trash/` | done (принята 2026-11-17, `fe218e8`) | T-021 | M2 |
| [T-023](T-023-job-store-library-log.md) | Хранилище заданий и журнал библиотеки (M2, P0.5 — часть 8): контракт `contracts::IJobStore` (состояния ADR-0015/0033, таблица переходов, прогресс, `needs_decision`, порядок, «пакет → подзадания»), реализация `StateJobStore` на опубликованной таблице `jobs`; восстановление заданий при открытии после `pending_ops` (`running` → `queued`/`failed` по `max_attempts`); журнал `op_log` (`OperationLog`) с записями `import_ready`/`copy`/корзины/восстановления/`verify`; CLI `am library log`; исполнитель — M3a | done (принята 2026-11-18, `27cbbb3`) | T-022 | M2 |
| [T-024](T-024-gen-library-k112.md) | Генератор синтетических библиотек и первый замер К-112 (M2, P0.5 — часть 9): детерминированный `am_gen_library` 100…250 000 ассетов только через `Catalog::create` (возобновляемый, без обхода протокола фиксации), `run_scale.py` (время, медиана/P95, пик памяти операций `info/list/sync/verify/rebuild`), результат `tests/performance/results/*.json` без личных данных, проверка «ZIP при подключении не читаются»; поиск и GUI — M4/M5 | done (принята 2026-11-20, `b1170f3`) | T-023 | M2 |
| [T-025](T-025-scale-fixes.md) | Устранение нелинейного масштабирования (M2, P0.5 — часть 10): подтвердить замером и исправить FTS-удаление сканированием (`asset_id UNINDEXED` → удаление по ключу, индекс `user_version` 2 с перестроением) и `integrity_check` при каждом открытии (дешёвое открытие; полная проверка — в `verify`, после незакрытой сессии и при `SQLITE_CORRUPT`); детерминированные тесты независимости от n (VM-шаги SQLite); повторный замер до 100 000 (250 000 — по оценке) | done (принята 2026-11-22, `3a34f09`) | T-024 | M2 |
| [T-026](T-026-job-executor.md) | Исполнитель заданий и ресурсы (M3a, P0.6 — часть 1): `am_jobs` — выборка из `IJobStore`, рабочие потоки, безопасные точки, пауза/отмена/корректная остановка без расхода попытки (`release_attempt`), `needs_decision` с ответом, «пакет → подзадания» (`await_children`, каскад), ограниченные автоповторы `waiting_resources`; `ResourceManager` (CPU/чтение/запись/внешние процессы, лимиты по устройствам, «всё или ничего», резервный интерактивный слот, профили-гипотезы); `detect_media` в `storage`; типы `library.verify` и `dev.probe`; CLI `am jobs list/show/run/pause/resume/cancel/retry/answer/move`, `library verify --enqueue` | review | T-025 | M3a |

**Порядок:** T-003 → T-002 → T-004 (приняты; итог S7 — ADR-0038) → T-006 → T-007 → T-008 → T-009 → T-010 (приняты); → T-011 → T-012 (приняты; веха M1 по CLI закрыта). Команды библиотеки — M2. → T-005 (CI GitHub, принята 2026-11-13) → T-014 (V3 по ADR-0039, принята 2026-11-13) → T-015 (чужие extra fields при изменении, принята 2026-11-13) → T-013 (golden ZIP, трассировка, принята 2026-11-13) → публикация формата 1.0 (выполнена 2026-11-14) → T-016 (основа библиотеки, M2, принята 2026-11-15) → T-017 (индексатор и `rebuild`, принята 2026-11-16) → T-018 (`sync` по `stat`, принята 2026-11-16) → T-019 (корзина, принята 2026-11-16) → T-020 (`add-ready`, принята 2026-11-16) → T-021 (независимая копия, принята 2026-11-17) → T-022 (восстановление при открытии, `library verify`, принята 2026-11-17) → T-023 (`IJobStore`, журнал библиотеки, принята 2026-11-18) → T-024 (`gen_library`, замер К-112, принята 2026-11-20) → T-025 (исправление масштабирования, повторный замер, принята 2026-11-22). Часть P0.5 вехи M2 закрыта. → T-026 (исполнитель заданий и ресурсы, M3a, review). Папка одна, поэтому задачи выполняются строго по одной.

Спайки — эксперименты, а не код продукта: живут в `spikes/<sN>/`, в сборку продукта не входят, итог каждого оформляется отдельной ADR.
