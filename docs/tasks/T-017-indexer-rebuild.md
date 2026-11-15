# T-017. Индексатор и перестройка индекса библиотеки (M2, P0.5 — часть 2)

**Статус:** ready · **Веха:** M2 · **Основание:** ADR-0007 (индекс перестраиваемый, `userdata` — нет), ADR-0024 (уровни), ADR-0025 (`AssetRepository` — единственный писатель ZIP, событие после фиксации), ADR-0037 п. 10 / O-16 (точные `uint64` в индексе), `schemas/sql/index.sql`, спецификация формата 1.0 §4–§9; `docs/plan/roadmap.md` M2 («`Indexer`, `rebuild` (без потери `userdata`) … шина событий после фиксации»); замечание 1 приёмки T-016

## Цель

1. **Индексатор** заполняет `index.sqlite` из манифестов ZIP-ассетов библиотеки по `schemas/sql/index.sql`.
2. **`rebuild`** — полная перестройка индекса из `assets/` и `trash/`; `userdata.sqlite` и ZIP не меняются; `rebuild_required` снимается.
3. **Обновление после фиксации**: каждая успешная фиксация `AssetRepository` в открытой библиотеке обновляет строку ассета в индексе (событие `on_indexed`).
4. **Тонкая обёртка SQLite** (замечание 1 приёмки T-016; `libraries.md`: «собственная тонкая RAII-обёртка») — первый настоящий потребитель.
5. **CLI** `am library rebuild` и `am library list`.

Сверка по `stat` и наблюдение за ФС (efsw), корзина, `add-ready`, конфликты ID — следующие задачи.

## Решения управляющего (не пересматривать в задаче)

1. **Разделение по уровням (ADR-0024).**
   - `storage`: обёртка SQLite и `IndexStore` — запись готовых записей в `index.sqlite` (SQL только здесь). Заголовки `include/am/storage/db/sqlite.hpp`, `include/am/storage/db/index_store.hpp`.
   - `services`: `catalog::Indexer` — отображение манифеста в запись индекса и сценарии `rebuild`/`update`/`remove` (знание формата, без SQL и без `sqlite3.h`). `include/am/services/catalog/indexer.hpp`, `src/services/catalog/*.cpp`; `src/services/CMakeLists.txt` — добавить исходники, зависимость уже разрешена.
   - `application`: фасад `library_service` — `rebuild`, `list`; CLI — только через фасад (правило `arch_check`).
2. **Обёртка SQLite** (`storage::db`): `Connection` (владеет или заимствует `sqlite3*`), `Statement` (prepare, bind по индексу: `std::string_view` текст, `int64`, `double`, `null`; step; чтение столбцов), `Transaction` (RAII: `BEGIN IMMEDIATE`, `commit()`, откат в деструкторе). Ошибки — `am::Result`/`am::Error` (`am.db.*`), без исключений наружу. `Library::database(kind)` остаётся (сырой указатель для совместимости), обёртка его заимствует. Перевод `state_journal.cpp` и `library.cpp` на обёртку — **не** в этой задаче.
3. **Запись индекса** (`IndexRecord` — простая структура в `storage`, без JSON-библиотеки в интерфейсе, кроме уже существующего `repository::Json` для `manifest_json` как текста): заполняются таблицы `assets`, `files`, `input_entries`, `input_objects`, `sources`, `asset_tags`, `asset_categories`, `asset_fields`, `fts_assets`, `fts_names`. Таблицы признаков (`image_hashes`, `mesh_features`, `asset_minhash`), `fts_docs`, `suggestions` — **не заполняются** (обработка и анализ — позже). Замена ассета — в одной транзакции: удалить все строки `asset_id` (включая FTS), вставить заново.
4. **Отображение полей** (`Indexer`, по манифесту, прочитанному `AssetSnapshot`; известная часть DOM):
   - `assets`: `asset_id`, `revision`, `rel_path` (`assets/xx/<id>.zip` или `trash/<id>.zip`, разделитель `/`), `in_trash`, `zip_size` и `zip_mtime_ns` (stat файла, `zip_size` — десятичная строка), `manifest_sha256` (SHA-256 байтов `manifest.json`), `format_major/minor`, `title`, `description` (из `asset.metadata`), `created_at`, `modified_at`, `incomplete` (1, если хоть один `inputs[].completeness ∈ {incomplete, unknown}`), `primary_preview` (FileID элемента `previews.items[]` с `id = previews.primary_id`), `file_count`, `content_bytes` (точная сумма `files[].size_bytes` в C++ с контролем переполнения; переполнение → `NULL`, `health = warn`, `health_detail` содержит `content_bytes_overflow`), `formats` (JSON-массив различных `files[].type.id`, отсортированный), `manifest_json` (байты `manifest.json` как есть).
   - `health` по проверке **без чтения потоков** (V1–V3, как `validate`): все уровни `passed` → `ok`; `newer_minor`, находки `unverifiable`/`warning` → `warn`; провал V1–V3 → `corrupt`; `Errc::unsupported` при открытии → `unsupported`. В `health_detail` — `message_id` первой находки или ошибки. `missing` — задача сверки, здесь не выставляется.
   - Ассет с `health = corrupt`, у которого манифест не прочитан (V1 failed), в `assets` **не** попадает (обязательные поля неизвестны) — он идёт в отчёт `rebuild` (п. 6).
   - `files`: `file_id`, `storage_path`, `name` (последний компонент `storage_path`), `size` (= `size_bytes`), `sha256`, `origin_kind`, `type_id` (= `type.id`, если есть).
   - `input_entries` / `input_objects`: по `inputs[].entries[]` / `inputs[].objects[]` (с `volume_index` из `volumes[]`, `-1` для не-тома); `src_name`/`original_name` — последний компонент исходного пути в представлении §5.1 спецификации.
   - `sources`: `origin.sources[]` (`source_id`, `kind`, `name`, `url`).
   - `asset_tags`: `asset.metadata.tags[]`. **Поисковый ключ — пока исходная строка без изменений**: алгоритм ключа — открытый вопрос O-08, здесь он не закрывается; функция ключа — одна точка (`tag_search_key`) с комментарием «O-08, identity until decided». Индекс перестраиваемый, смена алгоритма потребует только `rebuild`.
   - `asset_categories`: `asset.metadata.categories[].path` — компактный JSON-массив сегментов (`["Figures","Anime"]`), без склейки через `/` (сегмент может содержать `/`).
   - `asset_fields`: `custom_values[]` уровней `asset`/`group`/`file` (`owner_id` — AssetID/GroupID/FileID); `type_id` — из `field_definitions`; для `integer`/`decimal` — `txt` точная лексема, `num` — `double` для фильтров (если представим, иначе `NULL`); для строк/перечислений — `txt`; для plugin TypeID — `txt` = непрозрачный JSON как есть (байты фрагмента), `num` = `NULL`; `unit` — как в значении.
   - `fts_assets`: `title`, `description`, `authors` (имена через пробел), `tags`, `sources` (имена источников); `fts_names`: по строке на каждое имя из `files.name`, `input_entries.src_name`, `input_objects.original_name` (повторы внутри ассета — один раз).
5. **Точные числа.** Размеры — только десятичные строки (CHECK схемы); никаких `CAST`, `SUM`, `double` для `uint64`. Сумма — `std::uint64_t` с проверкой переполнения.
6. **`rebuild(library)`** (`read_write`, владелец записи — `Library` из T-016):
   - перечисление `assets/*/*.zip` и `trash/*.zip` (только имена вида `<uuidv4>.zip`; прочие файлы — в отчёт как `ignored`, не удаляются);
   - каждый ZIP читается через `AssetRepository`-снимок / `verify_container` уровня `validate` (без чтения потоков);
   - `asset.id` ≠ имени файла или повтор `asset.id` → в индекс не попадает, отчёт `id_mismatch`/`duplicate_id` (разрешение конфликтов — отдельная задача);
   - вся перестройка — **одна транзакция** `IndexStore` (читатели видят старый индекс до `COMMIT`); после успеха `meta.rebuild_required = 0`, `meta.rebuilt_at = <UTC>`; ошибка — откат, `rebuild_required` не меняется;
   - `userdata.sqlite` не открывается на запись и не меняется (проверяется тестом по SHA-256 файла);
   - отчёт: `{indexed, skipped:[{path, reason, message_id}], ignored:[path], duration_ms}`; причины: `unreadable`, `id_mismatch`, `duplicate_id`, `unsupported`.
7. **Обновление после фиксации.** `library::OpenOptions` получает `std::function<void(const repository::IndexedEvent&)> on_indexed`, который `Library` передаёт в `RepositoryOptions` (единственное разрешённое изменение `library.hpp/.cpp`). Сервис уровня `services` (`catalog::Catalog` или аналог), владеющий `Library` и `Indexer`, подписывается и при `create`/`change` переиндексирует ассет, при удалении — удаляет строки. Ошибка индексации после фиксации **не** откатывает фиксацию (ADR-0025) — ставит `meta.rebuild_required = 1` и возвращается в результат/журнал как предупреждение.
8. **CLI.**
   - `am library rebuild <dir> [--json]` → 0, если перестройка выполнена, в том числе с непустыми `skipped`/`ignored` (они — в выводе: перестройка индекса не проверка ассетов); 4 — библиотека не открыта (`locked`, `not_found`, `read_only`) или ошибка ввода-вывода/транзакции.
   - `am library list <dir> [--json] [--trash]` — открывает только для чтения; строки: `asset_id`, `revision`, `title`, `health`, `file_count`, `content_bytes`, `in_trash`; при `rebuild_required = 1` — предупреждение в выводе (и поле `rebuild_required: true` в JSON), код 0.
   - `am library info` — дополнить числом проиндексированных ассетов (`indexed_assets`).
   - Новые `message_id` — `en.toml`; `docs/user/cli.md`; `CHANGELOG.md`.

## Тесты

Все — с `RecordProperty("req", …)`.

1. **Обёртка** (`req: ADR-0007`): bind/step/column для текста с NUL и не-ASCII, int64, null; `Transaction` без `commit()` откатывается; ошибка prepare → `Result` с `am.db.*`.
2. **Отображение** (`req: K-3|K-7|ADR-0037`): ассет из golden-корпуса `pos-full`, скопированный тестом по пути `assets/<xx>/<asset.id>.zip` тестовой библиотеки (корпус **не** изменять) → строки во всех таблицах п. 3 с ожидаемыми значениями; `pos-exact-numbers` — точные строки размеров и `txt` в `asset_fields`; plugin-значение — сырой JSON.
3. **Переполнение суммы** (`req: K-3|ADR-0037`): синтетический манифест с двумя файлами по `18446744073709551615` (без реальных байтов: проверка отображения функцией `Indexer` на DOM) → `content_bytes = NULL`, `health = warn`, `health_detail` с `content_bytes_overflow`.
4. **`rebuild`** (`req: K-7|ADR-0007`): библиотека с 3 ассетами, созданными через `AssetRepository` открытой библиотеки (у golden-корпуса всего два различных `asset.id` на 12 положительных случаев — для набора разных ассетов он не годится), + один из них, скопированный в `trash/<id>.zip` (→ `duplicate_id`), + ещё один созданный ассет, перенесённый только в `trash/` (→ `in_trash = 1`), + посторонний файл + ZIP с испорченным манифестом + ZIP, имя которого не совпадает с `asset.id` → `indexed = 4`, `skipped` с `unreadable`, `id_mismatch`, `duplicate_id`, `ignored` с посторонним; `rebuild_required = 0`; SHA-256 `userdata.sqlite` не изменился; ZIP-файлы побайтно не изменились.
5. **Удалённый индекс** (`req: K-7|ADR-0007`): удалить `index.sqlite` → `open` пересоздаёт (T-016) → `rebuild` → те же строки `assets`, что до удаления (сравнение выборок, кроме `rebuilt_at`).
6. **Атомарность `rebuild`** (`req: ADR-0007`): ошибка на середине (внедрение через шов теста) → индекс прежний, `rebuild_required` прежний.
7. **После фиксации** (`req: ADR-0025|K-7`): `create` и `change(SetMetadata title)` через открытую библиотеку → строка `assets` обновлена (`revision`, `title`, FTS находит новое название); внедрённая ошибка индексации → фиксация успешна, `rebuild_required = 1`.
8. **Только чтение** (`req: ADR-0031`): `list` в `read_only` при владельце записи в том же процессе — читает; изменений файлов данных нет (как тест 4 T-016).
9. **CLI** (`tests/cli`, `req: K-7`): `am library init`; `rebuild --json` пустой библиотеки; затем тест создаёт два ассета командой `am asset create`, читает их `asset.id` (`am asset inspect --json`) и копирует по путям `assets/<xx>/<asset.id>.zip` (команды добавления в библиотеку ещё нет — это задача `add-ready`) и проверяет `rebuild --json` (отчёт), `list --json`, `info --json` (`indexed_assets`).

## Файлы (что можно менять)

- Новые: `include/am/storage/db/sqlite.hpp`, `include/am/storage/db/index_store.hpp`, `src/storage/db/sqlite.cpp`, `src/storage/db/index_store.cpp`, `include/am/services/catalog/*.hpp`, `src/services/catalog/*.cpp`, `tests/unit/test_sqlite.cpp`, `tests/unit/test_indexer.cpp`, CLI-тесты в `tests/cli/`.
- Изменяемые: `src/storage/CMakeLists.txt`, `src/services/CMakeLists.txt`, `include/am/storage/library/library.hpp` и `src/storage/library/library.cpp` (только `on_indexed` в `OpenOptions` → `RepositoryOptions`), `include/am/application/library_service.hpp`, `src/application/common/library_service.cpp`, `src/application/CMakeLists.txt`, `src/application/cli/main.cpp`, `tests/CMakeLists.txt`, `resources/messages/en.toml`, `docs/user/cli.md`, `CHANGELOG.md`, этот файл (отчёт), строка T-017 в `docs/tasks/README.md`.
- **Не менять:** `schemas/sql/*.sql` (если отображение не ложится на схему — остановиться и описать), формат 1.0 (схема, golden ZIP), кодек, V1–V4, протокол фиксации, `AssetRepository`, `state_journal.cpp`, ADR, спецификации, матрицы, roadmap, `vcpkg.json`.

## Критерии приёмки

1. `tools\build_min.cmd --ci`, `tools\build_cli.cmd --ci`, `tools\build_cli.cmd --offline --ci` — PASS; `tools/build_linux.sh cli --ci` в WSL — PASS. Число тестов не уменьшилось; `req_report` — 0 неверных ID; `arch_check` PASS (SQL и `sqlite3.h` только в `storage`).
2. Тесты 1–9 выполнены; в отчёте — имена тестов и пример `am library list --json` / `rebuild --json`.
3. Отчёт перечисляет поля схемы, которые **не** заполняются, и почему.
4. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-017`, без слияния, push и переписывания истории.

## Вне объёма

- Сверка по `stat`, `sync`, efsw, статус `missing` — T-018.
- Корзина (перемещение в `trash/`), `add-ready`, конфликты ID и независимая копия (ADR-0036 п. 4).
- Поиск (запросы FTS для пользователя), признаки похожести, предложения анализа.
- Генератор синтетических библиотек и замеры К-112 (здесь — только `duration_ms` в отчёте).
- Алгоритм поискового ключа тегов (O-08).

## Отчёт исполнителя

_(заполняет исполнитель)_
