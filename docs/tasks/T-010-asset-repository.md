# T-010. `AssetRepository`: снимки, запросы изменений, ревизии и история, проверка перед фиксацией (V1, V2, V4 и инварианты V3)

**Статус:** ready (В-1, В-2 решены управляющим 2026-11-12, продолжать в той же ветке) · **Веха:** M1 (`am_storage`, P0.4) · **Основание:** ТЗ §12.7, §14.2.7–8, §14.5, §19.4.2, §19.4.4, §19.4.7, §19.4.13, §19.4.15, §19.4.16; ADR-0024, ADR-0025 пп. 1–6, ADR-0031 пп. 2–3, ADR-0032, ADR-0033 п. 1, ADR-0035 пп. 2, 7, 8, ADR-0036, ADR-0037, ADR-0038; [спецификация формата 1.0](../specifications/container_manifest_v1.md) §§1–4, 6, 10–12; [`fs_support.md`](../specifications/fs_support.md) v0.1; T-006…T-009 вместе с разделами «Приёмка»

## Цель

Построить над протоколом фиксации T-009 **единственного писателя действующих ZIP** (ADR-0025 п. 1). Он:
1. выдаёт читателям **снимок** ассета: открытый ZIP, ревизия, разобранный манифест; снимок не мешает фиксации и видит свою версию до закрытия;
2. принимает **типизированный запрос изменения** с `expected_rev`. Устаревшая ревизия разрешается правилом коммутирующих операций, конфликт — `Errc::conflict`;
3. строит новый ZIP: манифест первой записью, неизменённые записи — raw-копией, новые — потоком с SHA-256;
4. ведёт `asset.revision`, `modified_at` и `history.entries[]`. Одна фиксация — одна запись истории с `operation_id = op_id` журнала. Запрос без фактических изменений — no-op без новой ревизии;
5. перед заменой выполняет проверку **V1, V2, V4 полностью и V3 в части инвариантов репозитория**. Отчёт о проверке раздельно указывает фактически пройденные уровни;
6. соблюдает `LibraryMode` и `StorageCapabilities` по `fs_support.md`.

Задача также выполняет сценарий приёмки **O-13** из roadmap (M0, S7), часть «транзакция ZIP в `AssetRepository`»: открыть ZIP → V1/V2 → изменить `title` → записать транзакционно → открыть снова → сверить все непрозрачные лексемы и их JSON-пути. Вторая часть O-13 — тесты кодека в CI (T-005).

**Полный V3 сюда не входит.** Claims, группы и превью, `field_definitions`/`custom_values`, единицы, связи `inputs`↔`origin`↔`files`, лицензии, даты, теги — это задача **T-011 «Валидатор V3»**. Она подключается через тот же интерфейс семантической проверки, что и инварианты здесь. До T-011 отчёт проверки помечает V3 как `partial`. Поэтому пользовательские команды записи (CLI/GUI) в T-010 **не открываются**: фиксация доступна только через API и тесты (п. 11 «Решений»).

## Решения управляющего (не пересматривать в задаче)

1. **Размещение (ADR-0024).**
   - `include/am/storage/repository/`, `src/storage/repository/`; собирается только при `AM_WITH_CONTAINER=ON`.
   - Зависимость `am_storage → am_domain` (кодек T-006) разрешена матрицей ADR-0024; `arch_check` должен пройти.
   - Новых библиотек нет: используются zlib, OpenSSL, sqlite3 (через T-009) и jsoncons (через кодек). `libraries.md` сверен управляющим, изменений не требует.
2. **Раскладка библиотеки (ADR-0007).** Действующий ZIP ассета — `<library>/assets/<xx>/<AssetID>.zip`, где `xx` — две последние hex-цифры AssetID. Временные файлы, журнал и блокировки — по T-009. `am-library.toml`, `library.lock` и индекс — M2. `AssetRef{library_id, asset_id}`: `library_id` задаёт вызывающий код при открытии репозитория, в ZIP он не пишется (§19.4.2.5).
3. **Режим и возможности носителя (ADR-0031 пп. 2–3, `fs_support.md`).**
   - `AssetRepository::open(root, RepositoryOptions)` принимает `LibraryMode{read_write, read_only}`.
   - `StorageCapabilities` определяются функцией `detect_storage_capabilities(root)` **по таблице `fs_support.md` v0.1**, а не пробной записью:
     - имя ФС берётся из `GetVolumeInformationW`, сетевой том — по `GetDriveTypeW == DRIVE_REMOTE`;
     - **NTFS на локальном томе** — `posix_rename = true`, `locks_reliable = true`, `durability = assumed`, `max_file_size` без ограничения, режим `full`;
     - **любая другая ФС, а также сетевой том** — режим `read_only_default`, значения «неизвестно», для FAT32 `max_file_size = 4 ГиБ − 1`.
   - Открытие на запись тома с режимом `read_only_default` → `Errc::read_only`, `am.repo.storage_read_only`, действие `open_read_only`. Открытие в `read_only` разрешено всегда.
   - Для тестов `RepositoryOptions` принимает **подставленные** `StorageCapabilities`. Поле подписано в заголовке как тестовое и диагностическое; отметка конфигурации ФС как проверенной — M2.
   - Любая изменяющая операция в `read_only` → `Errc::read_only`, `am.repo.read_only`.
   - `max_file_size` передаётся в `CommitRequest::capabilities` (T-009). Кроме того, он проверяется **до** записи по оценке размера: если оценка больше предела — `Errc::insufficient_space`, `am.repo.file_too_large`, без журнала и временного файла.
4. **Снимок (ADR-0025 п. 3).** `AssetSnapshot` — перемещаемый объект только для чтения. Он держит:
   - ZIP, открытый `file_source(path, /*share_delete=*/true)` (требование читателей из `fs_support.md` §4);
   - `ZipArchive`, текст `manifest.json`, `ManifestDocument` после V1 и V2, `revision`, `BaseSnapshot` (размер, mtime, SHA-256 манифеста) для `CommitRequest::base`.
   Таблица `snapshots` из `state.sql` нужна только режиму поколений и здесь **не используется**: в POSIX-режиме NTFS старую версию держит открытый дескриптор. Любой код, который в этой задаче читает действующий ZIP, получает его только через снимок.
5. **Конкурентность (ADR-0025 п. 4).**
   - В процессе — мьютекс на `AssetRef`: таблица мьютексов в репозитории, запись в неё удаляется, когда мьютекс никем не держится.
   - Между процессами — блокировка ассета T-009, которую берёт `commit_zip`.
   - Разные ассеты фиксируются параллельно; тест обязателен.
   - Чтение (снимок) мьютекс не берёт.
6. **Запрос изменения (ADR-0025 п. 5).** `ChangeRequest{ref, expected_rev, ops[], op_id?, reason?}`. Если `op_id` не задан, он генерируется (`Uuid::v4`). Типизированные операции T-010:

   | Операция | Действие | Категория `history.operation` | `changes[].kind` |
   |---|---|---|---|
   | `AddFiles{input, files[]}` | Добавляет акт `inputs[]` (JSON-объект от вызывающего кода: репозиторий провенанс не выдумывает) и файлы `content/<InputID>/<path>` с `origin_kind ∈ {imported, user_added}`. Каждый файл: `file_id?` (иначе генерируется), путь внутри акта, источник байтов (путь к файлу), `metadata?` | `files_changed` | `file_add` (target = FileID) |
   | `RemoveFile{file_id}` | Убирает запись из `files[]`, добавляет в `historical_files[]` с `removed_at_revision` = новая ревизия и `last_known_sha256/size_bytes/storage_path`; поток в новый ZIP не попадает | `files_changed` | `file_remove` |
   | `ReplaceFileContent{file_id, source}` | Тот же FileID и `storage_path`, новые байты, `size_bytes` и `sha256`. `derived_from` других файлов **не** переписывается (§6) | `files_changed` | `file_replace{old_sha256, new_sha256}` |
   | `SetMetadata{scope, field, value?}` | Задаёт или удаляет **стандартное** поле `metadata` (§9.1) у `asset` или у файла (`scope = asset` или `file:<FileID>`). Значение — JSON известной части; форму проверяет V2 | `metadata_changed` | `metadata_set{target, field}` |

   - Одна фиксация — **одна категория** `operation`. Смешение категорий в одном запросе → `Errc::invalid_argument`, `am.repo.mixed_operations`. Словарь `changes[].kind` — [ПРЕДЛОЖЕНИЕ, OPEN] O-18; здесь используются его значения из спецификации §11, других не вводить.
   - **Ревизия 1** создаётся отдельным вызовом `create(CreateRequest)`. Ассета ещё нет, `base` пуст, `history` — запись `asset_created` с `details.creation_kind ∈ {import, user}`, `tool.application_version` из `RepositoryOptions` и `changes[]` по добавленным файлам. Задание AssetID вызывающим кодом допускается (тесты, будущий импорт), если файла `<AssetID>.zip` нет; иначе `Errc::conflict`, `am.repo.asset_exists`.
   - Callback-мутатор `commit_update(ref, fn)` (ADR-0025 п. 5) — **внутренний**, не публичный API: на нём реализуются типизированные операции.
7. **`expected_rev` и коммутирующие операции (ADR-0025 п. 5).**
   - `expected_rev == current` → применить.
   - `expected_rev > current` или ассета нет → `Errc::conflict` / `Errc::not_found`.
   - `expected_rev < current` → берутся записи `history.entries` ревизий `expected_rev+1 … current`. Запрос применяется к свежей ревизии **только если каждая** его операция коммутирует со **всеми** этими записями:
     - `SetMetadata(scope, field)` коммутирует с записью, если в её `changes[]` нет `metadata_set` с тем же `target` и `field`, и запись не удаляет файл, к которому относится `scope`;
     - `AddFiles` коммутирует всегда, если на свежей ревизии нет коллизии путей (п. 8) и FileID;
     - `RemoveFile` и `ReplaceFileContent` при устаревшей ревизии **не коммутируют никогда**.
   - **Fail-closed:** если среди этих ревизий есть свёрнутый диапазон `compacted_ranges`, запись с операцией вне базового словаря или запись без разбираемых `changes[]` — `Errc::conflict`.
   - Конфликт — `Errc::conflict`, `am.repo.revision_conflict`, действие `decide`; в `detail` — текущая ревизия.
   - Если `commit_zip` вернул `am.commit.base_changed` (ассет сменил другой процесс между снимком и фиксацией), репозиторий **один раз** берёт новый снимок и повторяет по тем же правилам, затем отдаёт конфликт.
8. **Пути и коллизии (ADR-0035 п. 8, ADR-0012).**
   - Путь нового файла — `content/<InputID>/<путь>`. Он проходит `paths::check_container_path`, лимит 200 байт UTF-8 на весь путь и проверку коллизии `path_collision_key` против **всех** путей нового ZIP (`files[]`, вложения расширений, перенесённые opaque-записи).
   - Коллизия → `Errc::conflict`, `am.repo.path_collision`. Репозиторий **не переименовывает** файлы сам: суффиксы `~<8hex>` и исходные имена в `inputs[].entries[]` — работа импорта (`ZipPathMapper`, M3a), который готовит запрос.
   - `InputID` нового файла обязан совпадать с `id` акта из того же `AddFiles`.
9. **Порядок записей нового ZIP (ADR-0035 п. 2, ADR-0006).**
   - `manifest.json` пишется **первой** записью. Для этого SHA-256 и размер каждого нового потока вычисляются **предварительным проходом** по источнику до записи манифеста. При записи потока `WrittenEntry::sha256` и размер сверяются с предварительными; расхождение (источник изменился) → `Errc::conflict`, `am.repo.source_changed`, откат T-009.
   - Неизменённые потоки и opaque-записи старого ZIP — `copy_raw` (T-008) с переносом времени DOS. Extra fields и комментарии — только по разрешению (политика T-008); перенос вслепую запрещён.
   - Метод новых потоков — `choose_method` (T-008) по имени и первым 64 КиБ.
   - Порядок записей после манифеста: `files[]` в порядке массива, затем вложения расширений, затем перенесённые opaque-записи в исходном порядке.
   - `estimated_size` для `commit_zip` = сумма несжатых размеров новых потоков + сжатые размеры копируемых записей + размер манифеста + накладные расходы каталога.
10. **Манифест: изменение и сохранность (ADR-0038).**
    - Правки выполняются только в известной части `ManifestDocument::known()`. Непрозрачные корни (`extensions.*.data`, `history.entries[].details`, `files[].generation.params` и др.) типизированными операциями T-010 **не затрагиваются**, поэтому `IntendedChanges` пуст. Исключение — `details` новой записи истории: это новый владелец, он объявляется через `make_opaque` и `IntendedChanges`.
    - Перед фиксацией, в этом порядке: `write_manifest` → `verify_preserved(old_text, new_text, intended)` → V2 по схеме. Отказ любого шага — до записи ZIP, ошибка кодека передаётся в `cause`.
    - **Записывать разрешено только формат 1.0** (`versioning.md`: `write` только для 1.0). Иная `major`/`minor` или неизвестный ключ ядра → `Errc::unsupported`, `am.repo.write_unsupported_version`; чтение и снимок при этом работают.
    - Время — `YYYY-MM-DDTHH:MM:SS.sssZ` ([ПРЕДЛОЖЕНИЕ] §2), источник — подставляемые часы `RepositoryOptions::clock`. `modified_at` = `timestamp` новой записи истории; `created_at` = `timestamp` записи ревизии 1.
    - Схема V2 — `schemas/manifest-1.0.draft.schema.json`, встраивается в сборку при конфигурации, как `state.sql` в T-009 (без копии в исходниках).
11. **Проверка перед фиксацией — через `CommitRequest::verifier` (T-009) над защищённым временным файлом.** Она выполняет:
    - **V1:** один корневой `manifest.json`, разбор `parse_manifest` с лимитами;
    - **V2:** `SchemaValidator::validate`;
    - **V3 — инварианты репозитория** (список закрыт; остальное — T-011):
      1. `format` = 1.0, `asset.id` равен AssetID из имени файла;
      2. уникальность FileID в `files[]` и `historical_files[]`, их пересечение пусто; уникальность InputID и AttachmentID;
      3. уникальность путей нового ZIP по `path_collision_key`; путь `files[]` лежит в `content/<InputID>/` существующего акта либо в `generated/`; путь вложения — в `extensions/<свой ExtensionID>/`;
      4. история: ревизии `1…asset.revision` покрыты ровно один раз записями и диапазонами, без пропусков и пересечений, оба массива упорядочены; ревизия 1 — `asset_created`; последняя запись — `revision = asset.revision` и `timestamp = modified_at`; `timestamp` ревизии 1 = `created_at`; `operation_id` новой записи = `op_id` журнала;
    - **V4 — полностью:** каждая запись `files[]` и `extensions.*.attachments[]` существует в ZIP; несжатый размер, CRC и SHA-256 совпадают; это потоковый проход `verify_entry(..., sha256=true)` с бюджетом. Записей вне реестра нет, кроме `manifest.json` и opaque-записей, перенесённых из старого ZIP. В новом ассете (ревизия 1) opaque-записей нет совсем. Opaque-записи проверяются по CRC и размеру и получают статус `verified_crc_size_only` (ADR-0035 п. 7).
    - **Сводка проверки** `VerificationReport`: уровни раздельно — `v1_v2: passed`, `v3: partial` с перечнем пройденных правил, `v4` со статусом по каждому потоку (`verified_sha256`, `verified_crc_size_only`, `unverifiable`, `unsupported`, `failed`), `plugin_validation: not_performed`, итог `container_verification ∈ {full, partial}` по правилам спецификации §12.
    - Фиксация разрешается, если V1, V2, инварианты V3 и V4 без `failed`/`unsupported`. `v3: partial` фиксацию **не** блокирует только потому, что пользовательские команды записи до T-011 не открываются. После T-011 `partial` по V3 перед фиксацией станет ошибкой — это требование ADR-0035 п. 7.
    - Семантическая проверка подключается интерфейсом `SemanticValidator` (вход — `ManifestDocument` и `ZipArchive`, выход — список нарушений с `message_id` `am.manifest.*` и признак полноты). В T-010 — реализация «инварианты репозитория», T-011 добавит полную.
    - Ошибки проверки из `commit_zip` приходят как `am.commit.verification_failed` с исходной ошибкой в `cause` (замечание 3 приёмки T-009). Репозиторий возвращает вызывающему коду **исходную** ошибку `am.manifest.*` / `am.repo.*` и прикладывает `VerificationReport`.
12. **Отдельный вызов проверки** `verify(ref, VerifyLevel{quick, selective, full}, file_ids?)` над снимком, без записи:
    - `quick` — V1, V2, инварианты V3 и структура ZIP без чтения потоков;
    - `selective` — плюс V4 для перечисленных FileID;
    - `full` — V4 для всех зарегистрированных потоков.
    Отчёт — тот же `VerificationReport`.
13. **Расширения с неизвестными зависимостями (спецификация §10).** `RemoveFile`/`ReplaceFileContent` блокируются, если в манифесте есть расширение, у которого `dependencies` отсутствует или `state = unknown`, либо `declared` со ссылкой `file` на этот FileID. Результат — `Errc::needs_decision`, `am.repo.extension_dependency`, действие `decide`. Данные расширения не удаляются и не правятся.
14. **No-op (§19.4.2.6).** Если после применения операций известная часть манифеста совпадает со старой (сравнение DOM), а набор потоков не изменился, фиксации нет: результат `no_op = true`, ревизия прежняя, журнал и временный каталог не трогаются. Пример: `SetMetadata` с текущим значением, удаление несуществующего поля.
15. **События и индекс.** Индекса и `IEventBus` ещё нет (M2). `RepositoryOptions::on_indexed` — необязательный наблюдатель. Он вызывается после состояния `indexed` с `{ref, revision, op_id, kind}` и не является источником истины (ADR-0025 п. 7); исключения из него перехватываются. `on_committed` для `commit_zip` пуст. `AssetRepository::recover()` вызывает `commit::recover` с тем же журналом и возвращает его отчёт.
16. **Ресурсы.** Память не растёт с размером файлов: все проходы по потокам — буферами фиксированного размера, `ReadBudget` с лимитом из `RepositoryOptions`, по умолчанию — гипотеза O-17 с комментарием.

## Требования

### 1. API (`include/am/storage/repository/`)

- `AssetRef`, `LibraryMode`, `StorageCapabilities` (расширение структуры T-009 по ADR-0031 п. 3 **аддитивно**: новые поля с безопасными значениями по умолчанию), `detect_storage_capabilities`.
- `RepositoryOptions{library_id, mode, capabilities?, application_version, clock, read_budget, zip_limits, codec_limits, lock_timeout, on_indexed, test_hooks}`. `test_hooks` — `commit::FaultHooks` T-009 и хуки репозитория (подмена источника между проходами, подмена манифеста перед проверкой); подписаны в заголовке как тестовые, пусты по умолчанию.
- `AssetRepository::open`, `snapshot(ref)`, `create(CreateRequest)`, `change(ChangeRequest)`, `verify(...)`, `recover()`.
- `ChangeResult{ref, revision, op_id, no_op, rebased_from?, verification, commit?}` (`commit` — `CommitResult` T-009, если была фиксация).
- Публичные заголовки — без `windows.h`, sqlite3, zlib, OpenSSL, jsoncons; `nlohmann/json.hpp` допустим (он уже в публичном API кодека).

### 2. Коды ошибок `am.repo.*`

`read_only`, `storage_read_only`, `file_too_large`, `not_found`, `asset_exists`, `revision_conflict`, `mixed_operations`, `invalid_operation` (неизвестный FileID, поле вне §9.1, `scope` с несуществующим файлом), `path_collision`, `source_changed`, `extension_dependency`, `write_unsupported_version`. Отказы уровня ZIP и журнала — исходными кодами T-007…T-009 в `cause`. Тексты — английские технические (`message`), не UI (ADR-0032).

### 3. Попутно

- `StorageCapabilities` из T-009 расширяется аддитивно (п. 1); поведение `commit_zip` не меняется.
- Тесты T-007…T-009 — без изменения ожиданий.

## Контекст (что прочитать)

- ADR-0025, ADR-0031 (пп. 2–3), ADR-0033 (п. 1), ADR-0035 (пп. 2, 7, 8), ADR-0036, ADR-0037, ADR-0038; спецификация формата §§1–4, 6, 9.1, 10–12; `versioning.md`; `fs_support.md`.
- T-006 (кодек: `parse_manifest`, `write_manifest`, `verify_preserved`, `make_opaque`, `SchemaValidator`), T-007…T-009 целиком, включая разделы «Приёмка».
- `schemas/manifest-1.0.draft.schema.json`, `schemas/examples/manifest-1.0.example.json`.

## Тесты

Теги `RecordProperty("req", …)`: `K-116` (память), `K-117` (заполненный том), `K-122` (kill при открытом читателе), иначе `ADR-0025`/`ADR-0031`/`ADR-0035`/`O-13`. Тестовые библиотеки — в `.cache/repo-tests/`.

1. **Создание:** `create` с двумя файлами (`AddFiles`, `creation_kind = user`) → `assets/<xx>/<id>.zip`; ревизия 1; `created_at = modified_at = timestamp`; `history` — одна запись `asset_created` с `operation_id = op_id`; `manifest.json` — первая запись ZIP; `VerificationReport`: V4 `verified_sha256` по обоим файлам, V3 `partial`.
2. **Изменения:** последовательно `SetMetadata(title)`, `AddFiles`, `ReplaceFileContent`, `RemoveFile`. После каждого: ревизия +1, одна новая запись истории правильной категории с `changes[]`, `modified_at` новой записи, неизменённые потоки скопированы raw (сжатые байты совпадают со старыми), удалённый файл — в `historical_files` с `removed_at_revision` и `last_known_*`, его потока в ZIP нет.
3. **O-13 (сценарий приёмки roadmap):** фикстура-ассет с `extensions.<id>.data`, `history.entries[].details` и `files[].generation.params`. Внутри — числа `12345678901234567890.000000000000000001`, целые вне int64/uint64, `1e400`, `-1E-400`, `-0`, `-0.0`, `1.500`, очень длинные дроби и показатели, вложенные объекты и массивы, два расширения. `snapshot` → `SetMetadata(title)` → фиксация → новый `snapshot`: все непрозрачные лексемы и их JSON-пути совпадают со старыми (сверка — через `verify_preserved` **и** независимым сравнением в тесте), `title` новый.
4. **No-op:** `SetMetadata` с текущим значением и удаление отсутствующего поля → `no_op`, ревизия, байты файла и mtime не изменились, журнал и `.am-tmp` пусты.
5. **`expected_rev`:**
   - устаревшая ревизия, разные поля метаданных → переприменение, `rebased_from` заполнено;
   - то же поле → `revision_conflict`;
   - `AddFiles` без коллизии путей → переприменение; с коллизией по `path_collision_key` (регистр, NFC) → `path_collision`;
   - `RemoveFile`/`ReplaceFileContent` при устаревшей ревизии → `revision_conflict`;
   - `expected_rev` больше текущей → `conflict`;
   - в промежутке есть `compacted_ranges` или неизвестная `operation` (фикстура) → `conflict`.
6. **Две одновременные записи одного ассета:**
   - в процессе — два потока; результат — две последовательные ревизии либо одна ревизия и конфликт по правилам п. 7;
   - между процессами — помощник держит блокировку ассета, затем второй процесс фиксирует → второй ждёт до `lock_timeout` или получает `locked`; ревизии не теряются.
   - Разные ассеты в двух потоках фиксируются параллельно (тест с барьером в `verifier` подтверждает перекрытие во времени).
7. **Снимок при фиксации (K-122):** снимок открыт → `change` → старый снимок дочитывает **все** потоки своей ревизии с верными CRC и SHA-256, новый снимок — новую ревизию. Kill помощника после замены при открытом снимке → `recover` → новая ревизия, старый снимок цел.
8. **Kill-тесты через репозиторий:** помощник выполняет `change` и завершается в точках `after_prepared`, `after_verify`, `after_verified_journal`, `after_replace`, `after_committed_journal` (хуки T-009 через `RepositoryOptions` только для тестов). `AssetRepository::recover` → ревизия старая или новая, **никогда** не промежуточная; манифест проходит V1/V2/инварианты V3, V4 `full`. Каждая точка — 3 прогона.
9. **Проверка перед фиксацией отклоняет:**
   - источник файла изменился между предварительным проходом и записью (хук) → `source_changed`;
   - нарушение V2 (поле `metadata` неверного типа через внутренний `commit_update`) → исходная ошибка `am.manifest.*`, цель не изменена;
   - нарушение каждого инварианта V3 из п. 11 (через внутренний `commit_update` или подмену манифеста хуком) → отказ, цель не изменена;
   - порча байта потока во временном файле до проверки (хук приёмника) → V4 `failed`, `Errc::corrupt_data`.
10. **Opaque-записи и расширения:** старый ZIP-фикстура с opaque-записью и расширением с вложением → после `SetMetadata` opaque-запись перенесена raw, статус `verified_crc_size_only`, `container_verification = partial`; вложение — `verified_sha256`. `RemoveFile` при расширении без `dependencies` → `needs_decision`, `extension_dependency`; при `declared` без ссылки на файл → разрешено.
11. **Версии:** фикстура с `minor = 1` или неизвестным ключом ядра → снимок и `verify` работают, `change` → `write_unsupported_version`.
12. **Режимы и носитель:**
    - `read_only` → любая изменяющая операция `read_only`, снимок работает;
    - подставленные возможности `read_only_default` при открытии на запись → `storage_read_only`;
    - `detect_storage_capabilities` на томе тестов (NTFS) → `full`, `durability = assumed`;
    - подставленный `max_file_size` меньше оценки → `file_too_large` до журнала;
    - нехватка места (подставляемая функция T-009) → `insufficient_space`, старый ZIP цел (K-117).
13. **`verify`:** `quick` не читает потоки (счётчик чтений источника либо бюджет), `selective` — только указанные, `full` — все; испорченный поток обнаруживается `full` и `selective` с этим FileID, но не `quick`.
14. **Память (K-116):** ассет с файлом 1 ГиБ (несжимаемые псевдослучайные данные из детерминированного генератора, создаются в тесте и удаляются) → `SetMetadata` → raw-копия и полный V4. Время и пик памяти — в отчёт, порога нет. Отдельно — время предварительного SHA-256-прохода протокола T-009 на этом несжимаемом ZIP (замечание 4 приёмки T-009).

## Файлы (что можно менять)

- Новые: `include/am/storage/repository/*.hpp`, `src/storage/repository/*.cpp`, `tests/unit/test_asset_repository*.cpp`, помощник kill-тестов (расширить `am_commit_crash_helper` режимом репозитория либо отдельный исполняемый в `tests/integration/`), фикстуры `tests/fixtures/repository/`.
- Изменяемые: `include/am/storage/commit/protocol.hpp` и `src/storage/commit/protocol.cpp` — только аддитивное расширение `StorageCapabilities`; `src/storage/CMakeLists.txt`, `tests/CMakeLists.txt`; `docs/tasks/README.md` (статус), этот файл (отчёт).
- Не менять: схемы (`schemas/**`), `vcpkg.json`, ADR, спецификации, `fs_support.md`, `libraries.md`, матрицы, кодек T-006 и поведение T-007…T-009, материалы владельца.

## Критерии приёмки

1. `tools\build_min.cmd` — PASS без предупреждений; минимальная сборка не тянет новые зависимости.
2. `tools\build_cli.cmd` и `tools\build_cli.cmd --offline` — PASS без предупреждений; все прежние тесты проходят без изменения ожиданий.
3. Все сценарии «Тестов» реализованы и выполнены; таблица «сценарий → тесты»; таблица kill-тестов (точка × исход × прогоны).
4. `arch_check` проходит; публичные заголовки по п. 1 «Требований»; исключения не выходят за границы модуля.
5. Отчёт содержит: перечень правил V3, проверяемых в T-010; пример `VerificationReport` для тестов 1 и 10; замеры теста 14.
6. Отчёт по `AGENTS.md`: ветка, коммиты, выполнение каждого критерия, вывод тестов, отклонения, вопросы.

## Вне объёма

- Полный V3 — T-011. Типизированная предметная модель манифеста (аксессоры всех разделов) — по мере нужды T-011 и M3a.
- CLI `am asset create|inspect|validate|verify|extract` — после T-011.
- Импорт (`inputs[].entries[]`, `ZipPathMapper`, переименования), группы, превью, claims, `field_definitions`, `custom_values`, операции над расширениями.
- Компактизация истории (P0 её не выполняет), независимая копия, миграция формата.
- Индекс, `IEventBus`, `library.lock`, `am-library.toml`, корзина, перемещение между библиотеками, режим поколений, ФС кроме NTFS — M2 и далее.

## Отчёт исполнителя

**2026-11-12. Ветка:** `codex/T-010`, создана от `main` на `d5b400b` после приёмки T-009. Отчёт и статус — коммит `T-010: report opaque-owner and future-version contract questions`.

**Реализация остановлена при сверке контракта с кодеком и схемой.** Статус review означает передачу вопросов управляющему, а не готовность AssetRepository к приёмке. Исходники, публичные API, тесты проекта и CMake не менялись.

Основание — п. 3 [`AGENTS.md`](../../AGENTS.md): «Если для выполнения нужно выйти за рамки или задача противоречит ADR, спецификации или коду, **остановись** и опиши вопрос в отчёте (раздел „Вопросы“). Не принимай архитектурных решений сам».

### Найденные противоречия

**1. Разрешение изменений непрозрачных владельцев задано уже, чем нужно операциям T-010.**

П. 10 «Решений» утверждает, что типизированные операции не затрагивают непрозрачные корни, IntendedChanges пуст, исключение — только details новой записи истории. Фактический контракт T-006/ADR-0038 требует большего:

- Кодек считает непрозрачными все дополнительные поля `history.entries[].changes[]`, кроме kind и target (`src/domain/manifest/codec.cpp`, Analysis). Следовательно, `metadata_set.field`, `file_replace.old_sha256` и `file_replace.new_sha256` — отдельные непрозрачные владельцы. Для нового `field` требуется make_opaque, а для добавления этого владельца — отдельный ключ IntendedChanges. Разрешение только нового details не пропускает даже SetMetadata(title) с требуемой историей.
- RemoveFile удаляет запись files[] целиком. У корректного generated-файла может быть непрозрачный `generation.params`; у файла также могут быть непрозрачные данные метаданных. Удаление такого FileID удаляет владельцев. В historical_files разрешены только id, removed_at_revision и last_known_*; generation туда не входит (спецификация §6, `$defs/historicalFile` схемы). Пустой IntendedChanges отклоняет удаление владельца. Утверждение «не затрагиваются» для такой операции неверно.
- Проверка сохранности в T-006 отклоняет как исчезновение прежнего владельца, так и появление нового неразрешённого. Менять кодек либо ослаблять проверку без решения управляющего нельзя.

**2. Обязательная V2 и инвариант format=1.0 противоречат чтению будущей версии из сценария 11.**

- Пп. 4, 11–12 требуют ManifestDocument после V1/V2, SchemaValidator::validate и инвариант V3 format=1.0; отчёт V1/V2 для обычного пути — passed.
- П. 10 и сценарий 11 требуют, чтобы snapshot и verify работали при minor=1 или неизвестном ключе ядра, а запись отклонялась отдельно.
- Утверждённая схема принимает строго minor=0 и запрещает дополнительные корневые ключи. Реальный SchemaValidator::validate такие документы отклоняет как am.manifest.schema_violation. `versioning.md` отдельно говорит: 1.x читается с частичными V2–V4 и не проходит схему 1.0. Неизвестные ключи ядра формата 1.0 также запрещены §2 спецификации.
- Нужен отдельный явно заданный путь чтения/отчёта для этих случаев. Подменять minor, удалять неизвестные поля, молча пропускать V2 либо объявлять её passed — самостоятельное изменение контракта. Схемы и кодек менять в T-010 запрещено.

### Воспроизведение на фактическом API

Диагностика `.cache/t010_contract_proof.cpp` собрана MSVC 14.44 с уже имеющимися `build/win-cli/lib/am_domain.lib` и `am_foundation.lib`. Использует реальные parse_manifest, make_opaque, write_manifest, verify_preserved и SchemaValidator::validate с исходной схемой проекта. Новые библиотеки не скачивались; исходники и артефакты диагностики находятся только в .cache.

Команда: `.cache\t010_contract_proof.cmd`. Лог: `.cache/t010-contract-proof.log`. Фактический вывод:

```text
schema_1_0=PASS
schema_minor_1=am.manifest.schema_violation
schema_unknown_core=am.manifest.schema_violation
ordinary_history_field=am.manifest.write_blocked: Binary handles must occupy exactly the opaque roots
new_owner=/history/550e8400-e29b-41d4-a716-446655440002/changes/0/field
new_owner=/history/550e8400-e29b-41d4-a716-446655440002/details
preserve_with_only_details_authorized=am.manifest.preservation_failed
preserve_with_all_new_history_owners_authorized=PASS
changed_v2=PASS
generated_v2=PASS
removed_v2=PASS
remove_file_preservation=am.manifest.preservation_failed
remove_file_preservation_detail=owner=/files/550e8400-e29b-41d4-a716-446655440003/generation/params path=/o:files/a:0/o:generation/o:params
```

Таким образом, добавление корректного field и удаление корректного generated-файла не отвергаются V2 сами по себе: отказ возникает именно в правилах непрозрачных владельцев. Пример с разрешением всех новых владельцев истории — только демонстрация необходимого изменения контракта; это разрешение не добавлено в продукт.

### Критерии приёмки и проверки

| Критерий | Фактическое состояние |
|---|---|
| 1. Минимальная сборка | На исходном коде PASS, 84/84; без предупреждений MSVC/CMake. |
| 2. CLI и offline | Для реализации T-010 не запускались: репозитория ещё нет. Диагностика кодека не заменяет эти прогоны. |
| 3. Сценарии 1–14 и kill | Не реализованы и не запускались. Подтверждена только диагностика контрактов выше. |
| 4. Границы модулей | Новых заголовков/кода нет; исходный arch_check прошёл в минимальном прогоне. |
| 5. Правила V3, VerificationReport, замеры 1 ГиБ | Реализации и результатов нет; статус будущих версий требует решения В-2. |
| 6. Отчёт | Ветка, воспроизведение, команды, отклонения и вопросы приведены здесь. Собственные изменения ограничены документацией задачи. |

```text
tools\build_min.cmd
100% tests passed, 0 tests failed out of 84
Total Test time (real) =  15.34 sec
```

Лог — `.cache/t010-min.log`. arch_check, unicode_tables_check, schema_check и sql_check — PASS. Это проверка исходной базы, не выполненного AssetRepository.

### Отклонения

Вместо реализации переданы вопросы согласно п. 3 AGENTS.md. Схемы, ADR, спецификации, fs_support.md, libraries.md, зависимости и код T-006…T-009 не менялись. В main не сливал, push и переписывание истории не выполнял. Посторонний `3D_am_open_questions_e2472e9.zip` сохранён без изменений и не включён в коммит.

### Вопросы

**В-1. Какой точный набор изменений непрозрачных владельцев разрешён типизированным операциям?**

Нужно уточнить п. 10:

- разрешается ли создавать через make_opaque дополнительные поля changes[] новой записи истории (field, old_sha256, new_sha256) и объявлять их владельцев в IntendedChanges наряду с details;
- для явного RemoveFile разрешается ли объявлять удаление всех владельцев удаляемого FileID в IntendedChanges, либо такую операцию при наличии непрозрачных данных следует блокировать отдельным исходом. Если требуется сохранять эти данные, нужно определить допустимое место/контракт их сохранения: существующая historical_files их не принимает.

Рекомендуемое уточнение для новой истории — разрешить только владельцев её нового operation_id, не затрагивая прежние записи. Политику удаления данных файла должен определить управляющий; самостоятельного разрешения потери владельцев нет.

**В-2. Как работают snapshot/verify и какие статусы возвращаются для minor>0 и неизвестного ключа ядра?**

Нужно согласовать пп. 4, 10–12 и сценарий 11 с versioning.md: какие проверки выполняются вместо неподходящей схемы 1.0, как отражаются непройденная V2 и неприменимый инвариант format=1.0, отдельно ли обрабатывается неизвестный ключ в заявленном 1.0. Либо скорректировать требование успешного snapshot/verify. Схему 1.0 нельзя применять с объявлением passed к документу, который она отклоняет.

Продолжить можно в той же ветке после уточнения В-1 и В-2.

## Решение управляющего по В-1 и В-2

**2026-11-12.** Оба вопроса обоснованы, остановка по п. 3 `AGENTS.md` правильная. П. 10 «Решений» не учёл, что кодек T-006 считает непрозрачными дополнительные ключи `changes[]` и данные внутри `files[]`. Сценарий 11 противоречил схеме 1.0 и `versioning.md`. Ниже — замена соответствующих мест ТЗ.

### В-1. Разрешённые изменения непрозрачных владельцев

Разрешение вычисляется **разностью множеств** `opaque_keys()` старого и нового документа, а не перечнем, заданным заранее. Ключи берутся только из `opaque_keys()`, сами ключи не конструируются.

1. **Новые владельцы — только внутри новой записи истории.** Все ключи `new − old` обязаны лежать в области `history`/`<operation_id новой записи>` (префикс ключа, который кодек выдаёт для этой записи: `details` и дополнительные ключи `changes[i]`, например `field`, `old_sha256`, `new_sha256`). Значения создаются через `make_opaque`. Эти ключи объявляются в `IntendedChanges`. Владелец нового ключа вне этой области — ошибка реализации: `Errc::internal`, `am.repo.unexpected_opaque_change`, фиксации нет. Прежние записи истории не меняются.
2. **Исчезающие владельцы — только данные явно удаляемого файла.** Все ключи `old − new` обязаны лежать в области `files`/`<FileID>` файла из `RemoveFile` этого запроса (`generation/params`, `custom_values/<id>/value`, `claims/<id>/value` и др.). Это данные, описывающие сам файл; явное удаление файла по спецификации §6 их удаляет, а `historical_files` их не хранит. Ключи объявляются в `IntendedChanges`. Любое другое исчезновение — `am.repo.unexpected_opaque_change`.
   - Список удалённых владельцев возвращается в `ChangeResult::removed_opaque_owners`, чтобы вызывающий код или UI могли показать, что удалено вместе с файлом.
   - Правило п. 13 «Решений» (расширения с неизвестными зависимостями блокируют удаление) применяется **до** этого шага и не ослабляется.
3. **Изменённые владельцы** (ключ есть в обоих документах, значение другое) не разрешаются никогда: `verify_preserved` с объявленными выше ключами отвергает их сам.
4. **`ReplaceFileContent` для файла с `origin_kind = generated`** → `Errc::invalid_argument`, `am.repo.invalid_operation`. При новых байтах `generation.params` перестали бы описывать файл, а пересоздание производного файла — дело обработчиков (M5), не репозитория.
5. Дополнительные тесты к разделу «Тесты»:
   - `SetMetadata(title)` и `ReplaceFileContent` дают записи истории с `changes[].field`/`old_sha256`/`new_sha256`, сохранность проходит, ключи в отчёте теста;
   - `RemoveFile` у `generated`-файла с `generation.params` и у файла с plugin-значением `custom_values` — фиксация проходит, `removed_opaque_owners` перечисляет ровно их, остальные непрозрачные данные (O-13) сохранены;
   - хук, который подменяет непрозрачное значение другого владельца перед `write_manifest`, → `unexpected_opaque_change` или `preservation_failed`, цель не изменена;
   - `ReplaceFileContent` у `generated` → `invalid_operation`.

### В-2. Будущие версии и манифест, не прошедший V2

Заменяет пп. 4, 11–12 (в части статусов) и сценарий 11. Основание — матрица `versioning.md`.

1. **Классификация до V2** — по `format` после V1:
   - `major = 1, minor = 0` → `supported`;
   - `major = 1, minor > 0` → `newer_minor`: чтение без записи;
   - `major ≥ 2` или `format.id` не совпадает → снимок и `verify` получают `Errc::unsupported`, `am.repo.unsupported_version` (матрица: `read` — нет).
2. **`supported` (1.0):** V2 обязательна.
   - **Снимок** создаётся только после пройденной V2. Объявленный 1.0 с неизвестным ключом ядра или иным нарушением схемы — повреждённый контейнер: `snapshot` возвращает исходную ошибку `am.manifest.schema_violation` (`Errc::corrupt_data`), `change` — то же.
   - **`verify`** не требует пройденной V2: возвращает отчёт с `v2: failed`, V3-инварианты и V4 выполняются, насколько позволяет структура (`files[]` с полями `id`/`storage_path`/`size_bytes`/`sha256` нужных типов; иначе поток `unverifiable`). Итог — ошибка отчёта, без исключения из API.
3. **`newer_minor`:**
   - Снимок создаётся. `AssetSnapshot::format_support = newer_minor`, `writable = false`.
   - V2: статус `not_applicable_newer_minor`, **не** `passed` и не `failed`; схема 1.0 не применяется.
   - V3-инварианты: статус `not_performed`. Инвариант «`format` = 1.0» — условие **записи**, а не чтения.
   - V4: те же проверки по `files[]` и `attachments[]`, насколько поля читаемы по правилам 1.0; нечитаемые — `unverifiable`.
   - `container_verification = partial`.
   - `change` → `Errc::unsupported`, `am.repo.write_unsupported_version` (миграции нет — лениво и не в T-010).
4. **`VerificationReport`** получает поле `format_support ∈ {supported, newer_minor}`; статусы уровней дополняются `not_applicable_newer_minor` и `not_performed`. Поле `v1_v2` разделяется на `v1` и `v2`.
5. **Сценарий 11 заменяется:**
   - `minor = 1` (с неизвестным ключом ядра) → снимок с `newer_minor` и `writable = false`; `verify(full)` → `v2: not_applicable_newer_minor`, V4 `verified_sha256` по читаемым файлам, итог `partial`; `change` → `write_unsupported_version`; файл не изменён;
   - объявленный 1.0 с неизвестным ключом ядра → `snapshot`/`change` → `am.manifest.schema_violation`; `verify` → отчёт с `v2: failed`;
   - `major = 2` → `unsupported_version` для снимка и `verify`.

Остальные требования, сценарии и критерии — без изменений. Продолжать в ветке `codex/T-010`; новый отчёт — разделом «## Отчёт исполнителя (после В-1, В-2)» в конце файла, первый отчёт не удалять. Диагностику `.cache/t010_contract_proof.*` в продукт не переносить.
