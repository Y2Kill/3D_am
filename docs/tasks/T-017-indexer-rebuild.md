# T-017. Индексатор и перестройка индекса библиотеки (M2, P0.5 — часть 2)

**Статус:** review (В-1/В-2 выполнены; итоговый отчёт ниже) · **Веха:** M2 · **Основание:** ADR-0007 (индекс перестраиваемый, `userdata` — нет), ADR-0024 (уровни), ADR-0025 (`AssetRepository` — единственный писатель ZIP, событие после фиксации), ADR-0037 п. 10 / O-16 (точные `uint64` в индексе), `schemas/sql/index.sql`, спецификация формата 1.0 §4–§9; `docs/plan/roadmap.md` M2 («`Indexer`, `rebuild` (без потери `userdata`) … шина событий после фиксации»); замечание 1 приёмки T-016

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

Дата: 2026-11-15. Выполнение остановлено при сверке условий задачи с принятым API `Library`; вопрос В-1 ниже. Статус `review` означает передачу вопроса управляющему, а не готовность индексатора к приёмке. Основание — п. 3 `AGENTS.md`: при противоречии задачи коду остановиться и описать вопрос в отчёте.

### Ветка и коммиты

- Ветка `codex/T-017` создана от `main`, исходный коммит `b8bc3ad`.
- Отчёт и статус `review` — завершающий коммит этой ветки (`HEAD`). Слияние, push и переписывание истории не выполнялись.

### Что сделано по критериям приёмки

1. Реально выполнена минимальная сборка `tools\build_min.cmd --ci`: **86/86 PASS**, `req_report` — 0 неверных ID; `arch_check`, `schema_check`, `sql_check` — PASS. Полные CLI, offline CLI и Linux не запускались: после обнаружения противоречия действует правило остановки. Их успех не заявляется.
2. Тесты 1–9 T-017 не добавлены и не запускались: производственная реализация не начата. Примеры `list`/`rebuild` не формировались, команд пока нет.
3. `IndexStore` и отображение манифеста ещё не реализованы; новые поля `index.sql` не заполняются. Схемы, опубликованный golden-корпус и формат 1.0 не менялись.
4. Изменены только этот файл задачи и статус строки T-017 в реестре. Код, зависимости, ADR, спецификации и матрицы не менялись. Предсуществующий незарегистрированный `3D_am_open_questions_e2472e9.zip` оставлен без изменений и не включён в коммиты. Временные файлы и логи находятся в `.cache/`.

### Команды и фактический вывод

```text
tools\build_min.cmd --ci
100% tests passed, 0 tests failed out of 86
Total Test time (real) =  13.63 sec
req_report: 13 IDs, 86 tests, 49 without req, 0 invalid IDs
exit_code: 0
```

Сборка выполнена с доступом к `.cache/pylib`, необходимым для `schema_check` по `AGENTS.md`. Свидетельства: `.cache/t016-t017-preflight-min.log`, `-result.json`, `-junit.xml`, `-req_report.json` (префикс унаследован от использованной служебной обёртки запуска; запущен текущий `tools/build_min.cmd` из `b8bc3ad`). Это проверка исходного состояния проекта, не проверка отсутствующей реализации T-017.

### Отклонения

Реализация остановлена до создания обёртки и индексатора: буквальное требование п. 6 не обеспечивается предписанным способом открытия библиотеки, а менять соответствующий код `Library` вне передачи `on_indexed` задача запрещает. Самостоятельно сужать требование или вводить новый режим открытия баз исполнитель не стал.

### Вопросы

**В-1. Запрет открытия `userdata.sqlite` на запись относится только к самому `Indexer` или ко всему сценарию `am library rebuild`, включая открытие `Library`?**

Пункт 6 требует `rebuild(library)` в `read_write` с владельцем записи из T-016 и одновременно: «`userdata.sqlite` не открывается на запись и не меняется». При этом принятый `Library::open(read_write)`:

- в `src/storage/library/library.cpp:422` открывает `userdata`, `state`, `index` с общими `OpenOptions`;
- в `src/storage/library/library.cpp:277` вычисляет `rw = options.mode == Mode::read_write`;
- в `src/storage/library/library.cpp:289` для `userdata` выбирает `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE`;
- при нулевой версии пустой базы может создавать её схему; следовательно, отсутствие SQL в будущем `Indexer` само по себе не запрещает открытие/изменение `userdata` внутри `Library`.

Пункт 7 и раздел «Файлы» разрешают в `library.hpp/.cpp` **только** добавление `on_indexed` и передачу его в `RepositoryOptions`. Открытие `Library` только для чтения не выполняет другую часть п. 6: не удерживает владельца записи и даёт read-only соединение индекса.

Нужно одно из двух уточнений управляющего/владельца:

1. Запрет относится к `Indexer`: он не открывает `userdata` самостоятельно, не использует её соединение и не меняет её данные; существующее открытие `Library(read_write)` остаётся допустимым. Сохранность данных подтверждается тестом SHA-256 из п. 4.
2. Запрет относится ко всему сценарию: требуется решение о режиме открытия `Library` для перестройки, с доступом к индексу на запись и к `userdata` только для чтения, и явное расширение разрешённых изменений `Library`.

Это вопрос границы требования T-017, не утверждение об ошибке принятой T-016. Продолжать после решения следует в той же ветке `codex/T-017`.

### Решение управляющего по В-1 (2026-11-15)

**Вариант 1: запрет относится к `Indexer`/`rebuild`, поведение `Library` из T-016 не меняется.** Требование п. 6 — гарантия для **данных** `userdata`: перестройка индекса их не читает как источник и не меняет. То, что `Library::open(read_write)` держит соединение с `userdata.sqlite` открытым на запись (и при открытии выполняет `PRAGMA journal_mode`/`synchronous` и проверку версии), — свойство владельца записи библиотеки, а не перестройки; отдельный режим `Library` для `rebuild` не нужен.

1. Формулировку п. 6 читать так: «`Indexer` и `rebuild` не обращаются к `userdata.sqlite` (ни чтение, ни запись через его соединение) и не меняют его данные».
2. Тест 4 проверяет это так: SHA-256 **основного файла** `userdata.sqlite` до `Library::open` и после закрытия `Library` совпадает, при условии что база была закрыта корректно (WAL перенесён). Если служебное открытие `Library` меняет байты основного файла (например, контрольной точкой WAL), сравнивать **логическое содержимое**: `PRAGMA user_version`, список таблиц и `SELECT * … ORDER BY` всех таблиц `userdata` до и после — и указать в отчёте, какой способ применён и почему.
3. Код `Library` из-за этого не менять.

Прочие решения задачи не меняются. Продолжать в той же ветке `codex/T-017`, статус — `in_progress`.

## Отчёт исполнителя — продолжение по В-1 и вопрос В-2

Дата: 2026-11-16. Продолжено по решению управляющего В-1 от 2026-11-15. Реализация и проверки обычных сценариев выполнены; дополнительная проверка повреждённого манифеста выявила неописанную проекцию конфликтующих дочерних записей на SQL-схему (В-2 ниже). Работа остановлена по разделу «Файлы» задачи и п. 3 AGENTS.md. Статус review означает передачу вопроса и частичного результата, а не готовность всей T-017 к приёмке.

### Ветка и коммиты

- Ветка `codex/T-017`, исходная база main — `b8bc3ad`.
- `8aaf606` — вопрос В-1; `94bf322` — решение управляющего.
- `8d7ce6d` — SQLite-обёртка, IndexStore, Indexer/Catalog, CLI, сообщения, документация и тесты.
- `69281a8` — устранение предупреждения GCC о неявном преобразовании JSON в optional в новом тесте; проверки и производственный код не менялись.
- `a4de3a6` — явный вывод причины платформенного SKIP для CTest в Linux-ветке CLI-теста.
- Отчёт В-2 и статус review — завершающий коммит этой ветки (HEAD). Слияния, push и переписывания истории не выполнялись.

### Что сделано по критериям приёмки

1. Минимальная сборка — 86/86 PASS; полный Windows CLI — 329/329 PASS, 0 неверных ID. Полный Linux-прогон собран без предупреждений в новых файлах; все 11 новых модульных тестов в нём прошли, но весь прогон сознательно прерван после обнаружения В-2 (exit 130), поэтому PASS полного Linux не заявляется. Полный offline-прогон не запускался. Целевые offline-проверки и их ограничения указаны ниже. Минимальный набор не уменьшился; CLI-набор увеличился на 12 тестов. arch_check, schema_check, sql_check, golden ZIP, каталог сообщений прошли в полном Windows-прогоне. SQL и sqlite3.h добавлены только в storage.
2. Тесты 1–9 реализованы и выполнены на Windows. Их успешность не закрывает В-2: дополнительный существующий отрицательный golden ZIP выявил ошибку rebuild, не включённую в первоначальный набор критериев. Имена тестов:

| Пункт | Проверки |
|---|---|
| 1. SQLite | `Sqlite.BoundValuesPreserveNulUnicodeIntegerRealAndNull`, `Sqlite.TransactionRollbackCommitAndPrepareError`: длины текста с NUL/Unicode, int64, REAL, NULL, перенос владения транзакцией, commit/rollback, ошибочный и пустой prepare |
| 2. Отображение | `Indexer.GoldenFullMapsEverySupportedTableAndNames`, `Indexer.ExactPluginFragmentsAndNumericLexemesArePreserved`: все десять заполняемых таблиц, FTS, исходные байты манифеста, plugin JSON во всех трёх scopes, точные integer/decimal и числовой фильтр; дополнительно `Indexer.VolumesCategorySegmentsUnitsAndHealth` |
| 3. Переполнение | `Indexer.Uint64OverflowAndUnrepresentableNumericFilter`: сумма двух максимальных uint64 → NULL и предупреждение; максимальные размеры сохраняются как TEXT в assets/files/input_entries/input_objects; непредставимый числовой фильтр → NULL, точный txt остаётся |
| 4. Rebuild | `Indexer.RebuildReportsConflictsAndPreservesUserdataAndZips`: четыре разных ассета, дубликат в trash, один ассет только в trash, посторонние файлы, плохой ZIP, несовпадающее имя; indexed=4, три причины skipped; userdata и ZIP неизменны |
| 5. Удалённый индекс | `Indexer.DeletedIndexRebuildRestoresSameRows`: пересоздание индекса через принятую Library, rebuild, равенство выборок |
| 6. Атомарность | `Indexer.RebuildRollbackPreservesRowsFlagAndReaderView`: внедрённый отказ после первой вставки; отдельный читатель видит прежние строки, после отказа прежними остаются строки и rebuild_required; успешный повтор виден читателю после COMMIT |
| 7. После фиксации | `Indexer.CommitObserverUpdatesFtsAndFailureKeepsCommittedZip`: create/change, revision/title и замена FTS, перенос Catalog без потери observer; отказ индексации оставляет ZIP зафиксированным, возвращает предупреждение и ставит rebuild_required=1; remove удаляет строки и FTS |
| 8. Только чтение | `Indexer.ReadOnlyListWithWriterPreservesData`: list через Indexer и фасад при владельце записи в том же процессе; снимок SHA-256 файлов данных совпадает, попытка rebuild отвергается |
| 9. CLI | `cli_library_rebuild_list`: init, предупреждение о необходимости rebuild, пустая перестройка, создание двух ZIP через CLI, inspect ID, копирование по библиотечным путям, rebuild/list/info и текстовый вывод; Windows выполняет весь сценарий, ограничение Linux описано ниже |

3. Незаполняемые поля схемы перечислены ниже.
4. Изменения ограничены разрешёнными файлами. В Library изменены только поле on_indexed и его передача в RepositoryOptions. AssetRepository, протокол фиксации, state_journal, SQL-схемы, кодек, V1–V4, опубликованный формат/golden ZIP, зависимости, ADR, спецификации и матрицы не менялись. Предсуществующий незарегистрированный `3D_am_open_questions_e2472e9.zip` не затронут и не включён в коммиты. Коллекция не читалась и не копировалась. Рабочие синтетические данные, логи и пробы — в `.cache/`.

### Реализация и сохранность данных

- `Connection` владеет или заимствует соединение, `Statement` связывает значения параметрами и сохраняет длины текста, `Transaction` откатывается в деструкторе без commit. Ошибки возвращаются как Result с `am.db.*`. Принятый код Library/StateJournal на обёртку не переводился.
- `IndexStore` хранит обычные структуры без JSON-зависимости в интерфейсе. Замена ассета удаляет прежние строки, включая FTS, и вставляет новые в одной транзакции; дочерние обычные строки удаляются через FK CASCADE. Перестройка использует один BEGIN IMMEDIATE на весь сценарий, снимает rebuild_required и записывает rebuilt_at только перед успешным COMMIT.
- Манифест проверяется V1–V3 через verify_container с streams=false. Полезные потоки не распаковываются для индексирования. manifest_json копируется побайтно, SHA-256 считается по тем же байтам. Для plugin-значений берётся исходный диапазон строгого JSON-дерева; DOM-дескрипторы непрозрачных значений не сериализуются в индекс.
- Размеры хранятся строками. Сумма — проверяемое сложение uint64 в C++; CAST/SUM/TOTAL и преобразования размеров в double не применяются. REAL используется только для приблизительных фильтров integer/decimal custom fields, исходный txt сохраняется.
- `tag_search_key` — единственная точка получения ключа тегов, тождественная исходной строке с комментарием O-08. Категории — компактные JSON-массивы сегментов, в том числе с `/` внутри сегмента. Имена для fts_names дедуплицируются внутри ассета. Набор кандидатов сортируется: активные assets обрабатываются перед trash; повторный ID отчётливо пропускается.
- `Catalog` владеет Library и Indexer. Его callback передаётся через разрешённый on_indexed; обёртки create/change возвращают фиксацию и предупреждения. При отказе индексации фиксация не откатывается, ставится rebuild_required. Состояние callback остаётся по стабильному адресу после перемещения Catalog.
- По В-1 применена **побайтовая проверка SHA-256 основного userdata.sqlite**, до открытия и после закрытия Library, на корректно закрытой исходной базе. В тесте добавлены пользовательские строки; SHA-256 совпадает. Переход к сравнению логического содержимого не понадобился. ZIP-файлы сравниваются по SHA-256; тест чтения сравнивает все файлы данных, исключая разрешённые В-1 T-016 SQLite WAL/SHM.

Изложение выше относится к реализованным сценариям. Обработка конфликта дочерних ключей повреждённого манифеста пока не реализована — см. В-2.

### Команды и фактические результаты

```text
tools\build_min.cmd --ci
100% tests passed, 0 tests failed out of 86
Total Test time (real) =  14.35 sec
req_report: 13 IDs, 86 tests, 49 without req, 0 invalid IDs
exit_code: 0

tools\build_cli.cmd --ci
100% tests passed, 0 tests failed out of 329
Total Test time (real) = 640.28 sec
req_report: 45 IDs, 329 tests, 49 without req, 0 invalid IDs
exit_code: 0

tools\build_cli.cmd --offline --ci -R Indexer
100% tests passed, 0 tests failed out of 9
Total Test time (real) =  12.96 sec
req_report: 45 IDs, 329 tests, 51 without req, 0 invalid IDs
exit_code: 0

tools\build_cli.cmd --offline --ci -R cli_library
100% tests passed, 0 tests failed out of 5
Total Test time (real) =   8.72 sec
exit_code: 0

tools/build_linux.sh cli --ci (WSL, прерван по В-2)
req_report: 45 IDs, 326 tests, 48 without req, 0 invalid IDs
exit_code: 130
```

В целевых прогонах req_report перечисляет весь зарегистрированный inventory, а не число реально выполненных тестов: соответственно реально выполнены 9 и 5. Прямой Windows-прогон Sqlite/Indexer до добавления дополнительного теста томов/health — 10/10 PASS. В последнем Linux-прогоне реально прошли `Sqlite.*` (2) и `Indexer.*` (9); общий успех полного набора не заявляется. Полный Windows CLI выполнен на 8d7ce6d; после него изменялись только инициализация значения и вывод причины SKIP в тестах, не их проверки или производственный код.

Промежуточный Linux-прогон остановлен после предупреждения GCC в новом тесте, затем пересобран после 69281a8 без этого предупреждения. Предупреждения в неизменённых старых файлах первого dev-прогона не исправлялись вне объёма. Начальные ошибки переносимости MSVC (`file_clock::to_sys`, deprecated u8path) устранены до успешного полного Windows-прогона. Первый расширенный целевой прогон остановлен, поскольку cmd снял якорь regex; он не подменяет отдельный успешный полный прогон.

Свидетельства: `.cache/t017-final-min-*`, `.cache/t017-final-cli-*`, `.cache/t017-final-targeted-*`, `.cache/t017-dev-cli-*`, `.cache/t017-final-linux-*`, `.cache/t017-linux-initial-warning-*`. В каждом наборе сохранены log, result.json, JUnit и req_report, когда соответствующий инструмент их сформировал. После остановки В-2 тестовых процессов в общей рабочей папке не оставлено.

На Linux производственная классификация носителя остаётся read_only_default. `cli_library_rebuild_list` проверяет начальное чтение и ожидаемый отказ записи, затем явно SKIP; отдельно это подтверждено запуском Python-теста (exit 0, `OK (skipped=1)`, причина видна CTest). Полный положительный CLI-сценарий выполнен на Windows. Сервисные rebuild/observer-тесты Linux используют синтетические возможности, как T-016. Конечный CLI-тест полного Linux-набора из-за остановки ещё не достигнут.

### Какие поля index.sql не заполняются

По п. 3 задачи пока не создаются строки в следующих таблицах:

- `image_hashes`: asset_id, ref, sha256, phash, dhash, w, h; вычисляемые ph0–ph3 соответственно тоже без строк. Требуется обработка изображений.
- `mesh_features`: asset_id, file_id, sha256, triangles, area, volume, ext_sorted, d2_hist, geom_fp, algo_ver. Требуется анализ геометрии.
- `asset_minhash`: asset_id, sig. Расчёт признаков похожести — следующая работа.
- `fts_docs`: asset_id, file_id, body. Извлечение текста документов — последующая обработка/плагины.
- `suggestions`: id, asset_id, owner_scope, owner_id, target_json, value_json, handler, handler_version, basis, confidence, content_key, candidate_key, basis_key, created_at, state. Предложения анализа вне T-017; их решения в userdata не затрагиваются.

В заполняемых таблицах необязательные исходные поля (например, размер/хеш отсутствующего тома, URL источника, unit, primary_preview) остаются NULL, когда их нет в манифесте. `asset_fields.num` не заполняется для plugin-типов и непредставимых чисел; точный txt сохраняется. `assets.content_bytes` — NULL при неизвестном размере или переполнении с диагностикой последнего. Остальные поля таблицы assets и данные десяти назначенных таблиц отображаются по задаче. Статус missing не вычисляется: сверка ФС — T-018.

### Новые message_id

`am.db.bind`, `am.db.error`, `am.db.exec`, `am.db.index`, `am.db.open`, `am.db.out_of_memory`, `am.db.prepare`, `am.db.step`, `am.db.transaction`; `am.catalog.out_of_memory`, `am.catalog.error`, `am.catalog.manifest_invalid`, `am.catalog.read_only`, `am.catalog.id_mismatch`, `am.catalog.duplicate_id`, `am.catalog.index_update_failed`, `am.catalog.rebuild_required`.

Метки представления `am.app.label.indexed_assets`, `assets`, `rebuild_required`, `indexed`, `duration_ms`, `skipped`, `ignored` (все с префиксом am.app.label.). Полноту проверяет message_catalog_check.

### Примеры реального вывода

Получены `.cache/t017_example.py` на обычном синтетическом ассете; источник и результаты сохранены в `.cache/t017-example.json`. Изменено только форматирование JSON.

`am library rebuild <dir> --json`:

```json
{
  "indexed": 1,
  "duration_ms": 509,
  "skipped": [],
  "ignored": [],
  "command": "library rebuild"
}
```

`am library list <dir> --json`:

```json
{
  "assets": [
    {
      "asset_id": "20e6f82c-b520-4120-b6a2-15de1ec39597",
      "revision": 1,
      "title": "T-017 sample",
      "health": "ok",
      "file_count": 1,
      "content_bytes": "18",
      "in_trash": false
    }
  ],
  "rebuild_required": false,
  "command": "library list"
}
```

Следующий `am library info --json` сообщил `indexed_assets = 1`.

### Отклонения

Гарантия userdata реализована по решению В-1: Indexer/rebuild не используют его соединение, поведение Library не изменено. Полный Linux-прогон и полный offline-прогон не завершены вследствие обязательной остановки по В-2. Для конфликтующих записей не применялись INSERT OR IGNORE, произвольный выбор первой/последней записи или изменение SQL-схемы.

### Вопросы

**В-2. Какой состав дочерних строк индексировать у читаемого манифеста с health=corrupt, если его записи нарушают уникальность ключей SQL-схемы?**

Пункт 4 задаёт health=corrupt при провале V1–V3 и отдельно исключает из assets случай, когда манифест не прочитан (V1 failed, обязательные поля неизвестны). Но существующий опубликованный `tests/fixtures/golden/neg-v3-unique_ids/asset.zip` читается, проходит V1 и V2, а два разных элемента files имеют одинаковый FileID. `schemas/sql/index.sql` требует `PRIMARY KEY (asset_id, file_id)`; обе дочерние строки записать нельзя. Схему менять запрещено, и раздел «Файлы» прямо требует: «если отображение не ложится на схему — остановиться и описать».

Воспроизведение на реализации 8d7ce6d (производственный код после этого не менялся):

1. `am library init <новая-библиотека-в-.cache> --json`.
2. Скопировать именно этот golden ZIP в `assets/00/ffa545ed-332d-4889-be8c-43a2d3b8f800.zip` библиотеки, не меняя корпус.
3. `am asset validate <zip> --json` фактически возвращает exit 1, v1=passed, v2=passed, v3=failed, error=am.manifest.unique_ids, pointer=/files/1/id.
4. `am library rebuild <dir> --json` фактически возвращает exit 4:

```json
{"command":"library rebuild","error":{"code":"am.io_error","message_id":"am.db.step","detail":"UNIQUE constraint failed: files.asset_id, files.file_id"}}
```

Вся транзакция откатывается, вместо строки assets с health=corrupt. Это провал дополнительной функциональной проверки, несмотря на PASS первоначальных тестов задачи. Скрипт и исходный вывод: `.cache/t017_corrupt_probe.py`, `.cache/t017-corrupt-probe.json`; фактический validate — `.cache/t017-corrupt-validate.json`.

Требуется определить проекцию повреждённых данных. Например:

1. Для corrupt сохранять assets с полным manifest_json и диагностикой, а дочерние таблицы для такого ассета не заполнять.
2. Сохранять assets и только однозначные дочерние строки; управляющему определить исключение всех конфликтующих строк либо иной явный порядок выбора и дополнительную диагностику.

Выбор влияет на состав поиска и отображения повреждённого ассета; самостоятельно его не назначал. В-1 закрыт. После решения В-2 продолжать в той же ветке codex/T-017, добавить регрессионную проверку этого golden случая и завершить обязательные прогоны.

### Решение управляющего по В-2 (2026-11-16)

Выбран **вариант 1**, с уточнением границы по V2. Пункт 4 читается так:

1. **Строка `assets` пишется, только если V1 и V2 пройдены.** Только схемно-валидный DOM гарантирует наличие и типы обязательных полей `assets` (`created_at`, `modified_at`, `format_*` и др.; см. `neg-v2-required`). Если V1 или V2 провалены, ассет в `assets` не попадает. Он идёт в отчёт `rebuild` (п. 6) с `message_id` первой находки или ошибки, как сейчас для V1. Это уточняет последнюю строку п. 4 («V1 failed» → «V1 или V2 failed»).
2. **Если V1 и V2 пройдены, а V3 провален (`health = corrupt`), пишется только строка `assets`.** Дочерние таблицы для такого ассета **не заполняются**:
   - `files`, `input_entries`, `input_objects`, `sources`, `asset_tags`, `asset_categories`, `asset_fields`;
   - `fts_names`.

   V3 проверяет как раз ссылочные и уникальные инварианты, на которых держатся ключи этих таблиц. Проекция нарушенного графа была бы произвольной, а повреждённый ассет нужно чинить, а не находить по содержимому. Выбор «первой/последней» строки и `INSERT OR IGNORE` по-прежнему запрещены.
3. **Поля строки `assets` у V3-corrupt.** Поля, которые не требуют разрешения ссылок по ID, заполняются из DOM как обычно:
   - `title`, `description`, `created_at`, `modified_at`, `format_*`;
   - `manifest_sha256`, `manifest_json`, `zip_*`, `rel_path`, `in_trash`, `revision`;
   - `file_count` (= длина `files[]`), `content_bytes` (правило переполнения прежнее), `formats`, `incomplete`.

   `primary_preview = NULL`: разрешение `previews.primary_id` по ID у такого ассета ненадёжно. `health_detail` — `message_id` первой находки V3 (для этого случая `am.manifest.unique_ids`). `fts_assets` заполняется: строка ассета видна в списке и в поиске по названию с пометкой `corrupt`.
4. **Ошибка `UNIQUE constraint` при `rebuild` — всегда дефект индексатора, а не данных.** Для ассета, прошедшего V1–V3, ключи уникальны по построению. Для V3-corrupt дочерних строк нет.

**Регрессионные проверки** (добавить к тестам 1–9; в обоих `golden` копируется во временную библиотеку теста, корпус не меняется):

- **Тест 10** (`req: K-3|K-6`): `neg-v3-unique_ids` → `rebuild` успешен, транзакция зафиксирована. Строка `assets` с `health = corrupt`, `health_detail = am.manifest.unique_ids`, `primary_preview IS NULL`, `file_count = 2`. Ноль строк в дочерних таблицах для этого `asset_id`.
- **Тест 11** (`req: K-3|K-6`): весь golden-корпус (79 случаев, каждый под своим UUID из манифеста или подставным именем файла, если манифест не читается) в одной библиотеке → один `rebuild` успешен.
  - Каждый случай либо в `assets` с `health`, выведенным из `expected.json`: v1–v3 passed → `ok` или `warn`; v3 failed → `corrupt`. Уровень v4 не учитывается, потоки не читаются.
  - Либо, если провалены v1/v2 или открытие, случай есть в отчёте `rebuild`.
  - Можно реализовать в Python CLI-тесте поверх `am library rebuild --json`. Случаи с одинаковым AssetID в корпусе (если они есть) разнести по отдельным библиотекам и в отчёте указать, как это сделано.

Продолжать в ветке `codex/T-017`, затем завершить обязательные прогоны (Windows min/CLI, Linux CLI полностью, offline).

## Отчёт исполнителя — завершение по В-1 и В-2

Дата: 2026-11-16. Решения управляющего В-1 (`94bf322`) и В-2 (`8992b58`) выполнены. Предыдущие отчёты выше описывают исторические остановки; окончательная реализация и результаты сдачи приведены здесь.

### Ветка и коммиты

- Ветка `codex/T-017`, исходная база main — `b8bc3ad`.
- `8aaf606` — вопрос В-1; `94bf322` — решение управляющего.
- `8d7ce6d` — SQLite-обёртка, IndexStore, Indexer/Catalog, CLI, сообщения, документация и тесты 1–9.
- `69281a8` — устранение предупреждения GCC в новом тесте; `a4de3a6` — явный вывод причины платформенного CLI SKIP.
- `d3da3e7` — отчёт В-2; `8992b58` — решение управляющего.
- `0e8342e` — В-2: обязательные passed V1/V2, проекция V3-corrupt без дочерних строк, регрессии 10–11 и весь golden-корпус.
- Окончательный отчёт и статус review — завершающий коммит ветки (HEAD). Слияния, push и переписывания истории не выполнялись.

### Критерии приёмки

1. Все четыре обязательных полных прогона завершились с exit 0; фактические итоги ниже. Минимальный набор — 86, CLI — 331 на Windows и 328 на Linux. По сравнению с принятой T-016 добавлено 14 тестов: 2 Sqlite, 11 Indexer, 1 CLI. req_report — 0 неверных ID. arch_check, schema_check, sql_check, проверки golden ZIP и каталога сообщений выполнены в полных прогонах. В производственном коде новые SQL и sqlite3.h находятся только в storage; services вызывает IndexStore без SQL.
2. Тесты 1–11 реализованы и выполнены; имена и соответствие:

| Пункт | Проверки |
|---|---|
| 1. SQLite | `Sqlite.BoundValuesPreserveNulUnicodeIntegerRealAndNull`, `Sqlite.TransactionRollbackCommitAndPrepareError`: длины текста с NUL/Unicode, int64, REAL, NULL, перенос владения транзакцией, commit/rollback, ошибочный и пустой prepare |
| 2. Отображение | `Indexer.GoldenFullMapsEverySupportedTableAndNames`, `Indexer.ExactPluginFragmentsAndNumericLexemesArePreserved`: все десять заполняемых таблиц, FTS, исходные байты манифеста, plugin JSON во всех трёх scopes, точные integer/decimal и числовой фильтр; дополнительно `Indexer.VolumesCategorySegmentsUnitsAndHealth` |
| 3. Переполнение | `Indexer.Uint64OverflowAndUnrepresentableNumericFilter`: сумма двух максимальных uint64 → NULL и предупреждение; максимальные размеры сохраняются как TEXT в assets/files/input_entries/input_objects; непредставимый числовой фильтр → NULL, точный txt остаётся |
| 4. Rebuild | `Indexer.RebuildReportsConflictsAndPreservesUserdataAndZips`: четыре разных ассета, дубликат в trash, один ассет только в trash, посторонние файлы, плохой ZIP, несовпадающее имя; indexed=4, три причины skipped; userdata и ZIP неизменны |
| 5. Удалённый индекс | `Indexer.DeletedIndexRebuildRestoresSameRows`: пересоздание индекса через принятую Library, rebuild, равенство выборок; дополнительно `.cache/t017_v2_full_row_probe.py` проверил равенство всех 21 столбца SELECT * FROM assets и SHA-256 userdata/ZIP |
| 6. Атомарность | `Indexer.RebuildRollbackPreservesRowsFlagAndReaderView`: внедрённый отказ после первой вставки; отдельный читатель видит прежние строки, после отказа прежними остаются строки и rebuild_required; успешный повтор виден читателю после COMMIT |
| 7. После фиксации | `Indexer.CommitObserverUpdatesFtsAndFailureKeepsCommittedZip`: create/change, revision/title и замена FTS, перенос Catalog без потери observer; отказ индексации оставляет ZIP зафиксированным, возвращает предупреждение и ставит rebuild_required=1; remove удаляет строки и FTS |
| 8. Только чтение | `Indexer.ReadOnlyListWithWriterPreservesData`: list через Indexer и фасад при владельце записи в том же процессе; снимок SHA-256 файлов данных совпадает, попытка rebuild отвергается |
| 9. CLI | `cli_library_rebuild_list`: init, предупреждение о необходимости rebuild, пустая перестройка, создание двух ZIP через CLI, inspect ID, копирование по библиотечным путям, rebuild/list/info и текстовый вывод; Windows выполняет весь сценарий, ограничение Linux описано ниже |
| 10. V3-corrupt | `Indexer.V3CorruptKeepsSummaryAndAssetFtsWithoutChildRows`: замена ранее проиндексированного pos-full на neg-v3-unique_ids; успешный COMMIT, health/detail, NULL preview, фактическая длина files, 0 дочерних строк, FTS по названию, повторный update, сохранность ZIP; другой исправный ассет остаётся в индексе |
| 11. Весь корпус | `Indexer.EntireGoldenCorpusRebuildsUsingExpectedValidationLevels`: все 79 опубликованных случаев, каждый в отдельной временной библиотеке из-за повторяющихся AssetID; состояние или пропуск по validate-уровням expected.json; V4 не учитывается, исходные ZIP не меняются |

3. Незаполняемые поля перечислены отдельно ниже, включая политику В-2.
4. Изменения ограничены разделом «Файлы» T-017. В Library изменены только on_indexed и его передача в RepositoryOptions. AssetRepository, протокол фиксации, state_journal, SQL-схемы, кодек, V1–V4, опубликованный формат/golden ZIP, зависимости, ADR, спецификации, матрицы и roadmap не менялись. Предсуществующий незарегистрированный `3D_am_open_questions_e2472e9.zip` оставлен без изменений и не включён в коммиты. Коллекция не читалась и не копировалась. Все временные файлы, логи, пробы и синтетические рабочие библиотеки — в `.cache/`.

### Реализация и сохранность данных

- `Connection` владеет или заимствует соединение, `Statement` связывает значения параметрами и сохраняет длины текста, `Transaction` откатывается в деструкторе без commit. Ошибки возвращаются как Result с `am.db.*`. Принятый код Library/StateJournal на обёртку не переводился.
- `IndexStore` хранит обычные структуры без JSON-зависимости в интерфейсе. Замена ассета удаляет прежние строки, включая FTS, и вставляет новые в одной транзакции; дочерние обычные строки удаляются через FK CASCADE. Перестройка использует один BEGIN IMMEDIATE на весь сценарий, снимает rebuild_required и записывает rebuilt_at только перед успешным COMMIT.
- Манифест проверяется V1–V3 через verify_container с streams=false. Полезные потоки не распаковываются для индексирования. manifest_json копируется побайтно, SHA-256 считается по тем же байтам. Для plugin-значений берётся исходный диапазон строгого JSON-дерева; DOM-дескрипторы непрозрачных значений не сериализуются в индекс.
- Размеры хранятся строками. Сумма — проверяемое сложение uint64 в C++; CAST/SUM/TOTAL и преобразования размеров в double не применяются. REAL используется только для приблизительных фильтров integer/decimal custom fields, исходный txt сохраняется.
- `tag_search_key` — единственная точка получения ключа тегов, тождественная исходной строке с комментарием O-08. Категории — компактные JSON-массивы сегментов, в том числе с `/` внутри сегмента. Имена для fts_names дедуплицируются внутри ассета. Набор кандидатов сортируется: активные assets обрабатываются перед trash; повторный ID отчётливо пропускается.
- `Catalog` владеет Library и Indexer. Его callback передаётся через разрешённый on_indexed; обёртки create/change возвращают фиксацию и предупреждения. При отказе индексации фиксация не откатывается, ставится rebuild_required. Состояние callback остаётся по стабильному адресу после перемещения Catalog.
- По В-1 применена **побайтовая проверка SHA-256 основного userdata.sqlite**, до открытия и после закрытия Library, на корректно закрытой исходной базе. В тесте добавлены пользовательские строки; SHA-256 совпадает. Переход к сравнению логического содержимого не понадобился. ZIP-файлы сравниваются по SHA-256; тест чтения сравнивает все файлы данных, исключая разрешённые В-1 T-016 SQLite WAL/SHM.

### Выполнение В-2 и дополнительные регрессии

- Indexer проверяет, что V1 и V2 имеют статус passed, до отображения полей. При провале возвращается исходная первая ошибка в skipped; отсутствующие/неверно типизированные обязательные поля не заменяются выдуманными значениями.
- Если V2 не выполнялась для newer_minor, запись также не создаётся: применяется строгая граница «только passed V1/V2» из п. 1 В-2. В rebuild это skipped/unsupported с am.catalog.schema_unverified. Опубликованный `pos-newer-minor` проверен в общем корпусе.
- Для passed V1/V2 и failed V3 остаются assets и fts_assets. Сохраняются исходный манифест/хеш, сведения ZIP, версия, даты, заголовок/описание, incomplete, formats, длина files и проверенная сумма размеров. health=corrupt; health_detail начинается с message_id первой находки V3. primary_preview остаётся NULL.
- Для V3-corrupt не формируются files, input_entries, input_objects, sources, asset_tags, asset_categories, asset_fields и fts_names. Ссылки и конфликтующие ID не разрешаются, первая/последняя дочерняя запись не выбирается, INSERT OR IGNORE не используется. Замена ранее исправного ассета очищает его прежние дочерние строки в той же транзакции.
- Регрессия 10 использует исходный neg-v3-unique_ids и сначала индексирует pos-full под тем же AssetID. Повторный rebuild успешно заменяет проекцию; другой исправный ассет не теряется. Отдельно проверены FTS по названию, NULL preview, отсутствие всех восьми видов дочерних строк и повторный update.
- **Уточнение факта корпуса:** опубликованный neg-v3-unique_ids содержит 3 элемента files[], а не 2 из примера в тексте решения. Код и тест следуют нормативному правилу п. 3 В-2 `file_count = длина files[]`: фактически 3. Сам ZIP не менялся.
- Регрессия 11 копирует каждый из 79 исходных ZIP в отдельную временную библиотеку по его AssetID либо подставному UUIDv4, если ID прочитать нельзя. Случаи намеренно повторяют ID; раздельные библиотеки исключают маскировку ошибок через duplicate_id. Для каждого выполняется rebuild, ожидаемая классификация берётся из validate-части expected.json; потоки V4 не проверяются. Для V3-corrupt дополнительно проверены отсутствие дочерних строк, NULL preview и наличие fts_assets. SHA-256 исходной и рабочей копии ZIP сравниваются после проверки.

### Команды и фактические результаты

```text
tools\build_min.cmd --ci
100% tests passed, 0 tests failed out of 86
Total Test time (real) =  16.32 sec
req_report: 13 IDs, 86 tests, 49 without req, 0 invalid IDs
exit_code: 0
```

Фактически: 86 PASS, 0 SKIP, 0 FAIL из 86 зарегистрированных тестов.

```text
tools\build_cli.cmd --ci
100% tests passed, 0 tests failed out of 331
Total Test time (real) = 809.40 sec
req_report: 45 IDs, 331 tests, 49 without req, 0 invalid IDs
exit_code: 0
```

Фактически: 331 PASS, 0 SKIP, 0 FAIL из 331 зарегистрированных тестов.

```text
tools\build_cli.cmd --offline --ci
100% tests passed, 0 tests failed out of 331
Total Test time (real) = 797.18 sec
req_report: 45 IDs, 331 tests, 49 without req, 0 invalid IDs
exit_code: 0
```

Фактически: 331 PASS, 0 SKIP, 0 FAIL из 331 зарегистрированных тестов.

```text
tools/build_linux.sh cli --ci (WSL)
100% tests passed, 0 tests failed out of 328
Total Test time (real) = 1025.73 sec
req_report: 45 IDs, 328 tests, 48 without req, 0 invalid IDs
exit_code: 0
```

Фактически: 314 PASS, 14 SKIP, 0 FAIL из 328 зарегистрированных тестов.

Пропуски (не считаются PASS):

- `AssetService.CreateRefusalsAndPublishRaceNeverReplaceOutput`;
- `AssetService.PostCreateFailureDeletesOnlyTheNewOutput`;
- `AssetService.ValidationAndExtractionUseSharedV3AndPreserveSuccessfulFiles`;
- `AssetService.StreamScopesMinorAndCorruption`;
- `CommitProtocol.FallbackOnlyForUnsupportedAndChecksTheMovedIdentity`;
- `CommitProtocol.ProtectedTemporaryFileDeniesAnotherWriter`;
- `CommitProtocol.LockHeldByAnotherProcessIsReleasedByKill`;
- `CommitProtocol.KillEveryCheckpointThreeTimesAndRecover`;
- `CommitProtocol.RecoveryUsesWholeFileIdentityNotOnlyManifest`;
- `CommitProtocol.OpenReaderKeepsOldBytesAndNewReadersSeeNewVersion`;
- `CommitProtocol.WindowsAndFilesystemEvidence`;
- `AssetRepository.AnotherProcessLockPreventsLostRevisionAndIsReleasedByKill`;
- `AssetRepository.KillEveryRepositoryCheckpointThreeTimesWithOpenSnapshot`;
- `cli_library_rebuild_list`;

Свидетельства: `.cache/t017-v2-final-min-*`, `.cache/t017-v2-final-cli-*`, `.cache/t017-v2-final-offline-*`, `.cache/t017-v2-final-linux-*` — log, result.json, JUnit, req_report и CMakeCache. Это полные прогоны без фильтрации на окончательном производственном коде 0e8342e. Целевые `.cache/t017-v2-targeted-*` и `.cache/t017-v2-targeted-linux-*` — отдельно 11/11 Indexer PASS на каждой платформе; они не подменяют полные наборы.

Первоначальная ошибка SQL UNIQUE из В-2 больше не воспроизводится: `.cache/t017_v2_corrupt_probe.py` скопировал неизменённый golden ZIP в новую библиотеку; rebuild вернул exit 0, indexed=1, skipped=[] (исходный вывод `.cache/t017-v2-corrupt-probe.json`). Исторические остановленные прогоны и неудачная проба сохранены отдельно, их результаты не подменяют итоговые.

### Какие поля index.sql не заполняются

По п. 3 задачи пока не создаются строки в следующих таблицах:

- `image_hashes`: asset_id, ref, sha256, phash, dhash, w, h; вычисляемые ph0–ph3 соответственно тоже без строк. Требуется обработка изображений.
- `mesh_features`: asset_id, file_id, sha256, triangles, area, volume, ext_sorted, d2_hist, geom_fp, algo_ver. Требуется анализ геометрии.
- `asset_minhash`: asset_id, sig. Расчёт признаков похожести — следующая работа.
- `fts_docs`: asset_id, file_id, body. Извлечение текста документов — последующая обработка/плагины.
- `suggestions`: id, asset_id, owner_scope, owner_id, target_json, value_json, handler, handler_version, basis, confidence, content_key, candidate_key, basis_key, created_at, state. Предложения анализа вне T-017; их решения в userdata не затрагиваются.

В заполняемых таблицах необязательные исходные поля (например, размер/хеш отсутствующего тома, URL источника, unit, primary_preview) остаются NULL, когда их нет в манифесте. `asset_fields.num` не заполняется для plugin-типов и непредставимых чисел; точный txt сохраняется. `assets.content_bytes` — NULL при неизвестном размере или переполнении с диагностикой последнего. Остальные поля таблицы assets и данные десяти назначенных таблиц отображаются по задаче. Статус missing не вычисляется: сверка ФС — T-018.

Дополнительно по В-2 у V3-corrupt не заполняются все столбцы files/input_entries/input_objects/sources/asset_tags/asset_categories/asset_fields/fts_names и assets.primary_preview; assets и fts_assets сохраняются. При отсутствии passed V1/V2 строка assets и её дочерние строки не создаются, случай отражается в skipped.

### Новые message_id

`am.db.bind`, `am.db.error`, `am.db.exec`, `am.db.index`, `am.db.open`, `am.db.out_of_memory`, `am.db.prepare`, `am.db.step`, `am.db.transaction`; `am.catalog.out_of_memory`, `am.catalog.error`, `am.catalog.manifest_invalid`, `am.catalog.schema_unverified`, `am.catalog.read_only`, `am.catalog.id_mismatch`, `am.catalog.duplicate_id`, `am.catalog.index_update_failed`, `am.catalog.rebuild_required`.

Метки представления `am.app.label.indexed_assets`, `assets`, `rebuild_required`, `indexed`, `duration_ms`, `skipped`, `ignored` (все с префиксом am.app.label.). Полноту проверяет message_catalog_check.

### Реальные примеры CLI

Получены `.cache/t017_v2_example.py` на обычном синтетическом ассете; исходные результаты — `.cache/t017-v2-example.json`. Изменено только форматирование JSON.

`am library rebuild <dir> --json`:

```json
{
  "indexed": 1,
  "duration_ms": 288,
  "skipped": [],
  "ignored": [],
  "command": "library rebuild"
}
```

`am library list <dir> --json`:

```json
{
  "assets": [
    {
      "asset_id": "ebbff3ce-674d-49c7-830b-460c22806c5f",
      "revision": 1,
      "title": "T-017 sample",
      "health": "ok",
      "file_count": 1,
      "content_bytes": "18",
      "in_trash": false
    }
  ],
  "rebuild_required": false,
  "command": "library list"
}
```

Следующий info сообщил indexed_assets=1.

### Отклонения и вопросы

В-1 и В-2 закрыты по решениям управляющего. Единственное фактическое расхождение с примером В-2 — число файлов neg-v3-unique_ids: 3 вместо упомянутых 2; применена длина опубликованного files[], без изменения корпуса.

На Linux производственная классификация носителя остаётся read_only_default. CLI-тест проверяет начальное чтение и ожидаемый отказ записи, затем явно SKIP; положительный сценарий CLI выполнен на Windows. Все сервисные регрессии, включая 79 golden ZIP, на Linux выполняются с синтетическими возможностями по принятому подходу T-016. 13 прежних платформенных пропусков и этот один новый SKIP перечислены выше и не считаются PASS. Неподдерживаемая для валидации newer_minor пропускается по уточнённой границе В-2, а не индексируется как прошедшая схему.

Stat/sync, перемещение в корзину, add-ready, разрешение конфликтов ID, пользовательский поиск, признаки/анализ и алгоритм ключа тегов остаются вне задачи. Новых вопросов нет.
