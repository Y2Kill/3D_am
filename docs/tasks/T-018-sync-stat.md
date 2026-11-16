# T-018. Сверка библиотеки по `stat` и `am library sync` (M2, P0.5 — часть 3)

**Статус:** done (принята 2026-11-16, `bda5c19`) · **Веха:** M2 · **Основание:** ТЗ v1.8 §4.9 п. 1–3 [СОГЛАСОВАНО] (быстрая сверка по размеру и времени модификации, распознавание добавленных, изменённых и отсутствующих ZIP, полный анализ при каждом старте не требуется); ADR-0007 (индекс перестраиваемый); ADR-0031 п. 5 (недоступный носитель — ассеты не считаются удалёнными); `schemas/sql/index.sql` (`health … missing`); приёмка T-017, замечания 1 и 3

## Цель

1. **`sync`** — инкрементальная сверка `index.sqlite` с ZIP в `assets/` и `trash/`. Перечитываются только файлы, у которых `stat` отличается от индекса: путь, размер или время модификации. Добавленные ZIP индексируются. Отсутствующие и ставшие нечитаемыми ZIP помечаются, строки не удаляются.
2. **CLI** `am library sync <dir> [--full] [--json] [--verbose]`.
3. **Исправление замечания 1 приёмки T-017:** ассет более новой minor (V1 `passed`, V2 `not_applicable_newer_minor`) индексируется с `health = warn`.
4. **Замечание 3 приёмки T-017:** явные `#include` в `index_store.hpp`.

**Наблюдение за ФС (efsw) в эту задачу не входит.** Механизмы, события и периодичность — открытый п. 4 §4.9 ТЗ. Кроме того, наблюдатель полезен только в долгоживущем процессе: GUI или служба, а не короткие команды CLI. `sync` — обязательная основа: по ТЗ §4.9 п. 3 наблюдение не может быть единственным способом обнаружения изменений. Наблюдатель будет вызывать ту же сверку.

## Решения управляющего (не пересматривать в задаче)

1. **Уровни (ADR-0024)** — как в T-017.
   - `services`: `catalog::Indexer::sync(SyncOptions)` и `Catalog::sync` (под тем же mutex, что и наблюдатель фиксаций).
   - `storage`: новые операции `IndexStore`: выборка состояния сверки и пометка строки. SQL — только здесь.
   - `application`: фасад `sync_library`; CLI — только через фасад.
2. **Перечень кандидатов** — та же функция, что у `rebuild`: общий код, а не копия.
   - Те же правила `ignored` (имя не `<UUIDv4>.zip`, не обычный файл, символическая ссылка, посторонние каталоги).
   - Тот же порядок: сортировка по относительному пути.
   - То же правило дубликатов: побеждает первый по сортировке (`assets/…` раньше `trash/…`), остальные идут в `skipped` с `duplicate_id`.
   - Проверка `id_mismatch` — та же.
3. **Ключ «без изменений»:** у строки индекса с тем же `asset_id` совпадают `rel_path`, `zip_size` и `zip_mtime_ns`, и `health ≠ missing`. Такой ZIP **не открывается**.
   - Во всех остальных случаях файл читается и отображается функцией `Indexer::map` (те же правила `health`, что в T-017 и в п. 5 ниже), строка заменяется (`IndexStore::replace`: прежние дочерние строки и FTS удаляются).
   - Это относится и к переходу `assets/` ↔ `trash/`: меняется `rel_path`.
   - `--full` (`SyncOptions::full`) перечитывает все кандидаты независимо от `stat`; это способ поймать изменение, не затронувшее размер и mtime.
4. **Пометки вместо удаления.** `sync` никогда не удаляет строки `assets`; удаляет только `rebuild`, который строит индекс с нуля.
   - **`missing`**: строка есть, файла-победителя с этим `asset_id` среди кандидатов нет. Меняются только `health = 'missing'` и `health_detail = 'am.catalog.asset_missing'`. Остальные поля и дочерние строки остаются: ассет виден в списке и в поиске как отсутствующий (ТЗ §4.9 п. 2), на него продолжают ссылаться данные `userdata`.
   - Если файл вернулся с тем же `stat`, он всё равно перечитывается: `health = missing` никогда не означает «без изменений».
   - **Файл есть, но не индексируется** (не открывается, V1/V2 провалены, `id_mismatch`, отказ `map`), а строка для этого `asset_id` существовала:
     - меняются `health` (`unsupported` для `Errc::unsupported`, иначе `corrupt`), `health_detail` (`message_id`) и `zip_size`/`zip_mtime_ns` на новый `stat`, чтобы следующий `sync` не перечитывал файл;
     - сводные поля и дочерние строки остаются от последней успешной индексации — это сознательно, чтобы пользователь узнал, какой ассет испорчен;
     - путь попадает и в `skipped` отчёта.

     Если строки не было, файл просто попадает в `skipped`, как в `rebuild`. Такой файл перечитывается при каждом `sync` — это допустимо.
   - Проигравший дубликат строку победителя не трогает.
5. **Более новая minor (замечание 1 T-017).** `Indexer::map` принимает V1 `passed` и V2 `passed` **или** V2 = `not_applicable_newer_minor` при `format_support = newer_minor`.
   - Для newer_minor:
     - `health = 'warn'`, `health_detail = 'am.catalog.newer_minor'` (новый `message_id`);
     - обязательные поля строки `assets` читаются с проверкой типа; отсутствие или неверный тип → отказ `am.catalog.manifest_invalid` (идёт в `skipped`), подстановки выдуманных значений нет;
     - дочерние таблицы и `fts_names` **не заполняются**, по тому же основанию, что у V3-corrupt: V3 не выполнялась, уникальность ключей и ссылки не доказаны;
     - `primary_preview = NULL`;
     - `fts_assets` и сводные поля — как у V3-corrupt (п. 3 решения В-2 T-017).
   - Это действует везде, где вызывается `map`: `rebuild`, обновление после фиксации, `sync`.
   - Прочие значения V1/V2 (`failed`, `not_performed` и любые другие) — как сейчас: в `assets` не попадают. **Проверить по перечислению `CheckStatus` в коде, что ни одно значение не осталось без явной ветки**, и перечислить их в отчёте.
6. **Транзакция и согласованность.**
   - `sync` выполняется одной транзакцией. Отказ (I/O, ресурсы, внедрённый сбой) → откат, индекс прежний. Читатели до `COMMIT` видят прежнее состояние.
   - Требуется `read_write`, проверка как у `rebuild`.
   - Восстановление `pending_ops` `sync` не выполняет: он видит только итоговые имена ZIP, а файлы `*.am-bak` и временные не являются кандидатами.
7. **`rebuild_required = 1`** → `sync` отказывает: `am.catalog.rebuild_required`, `Errc::incomplete`, индекс не меняется, CLI возвращает 4. Флаг означает, что индекс мог потерять события (пересоздание при миграции, сбой после фиксации), и инкрементальная сверка его не снимает. `sync` флаг не меняет.
8. **Недоступная библиотека (ADR-0031 п. 5).** Если `assets/` или `trash/` нельзя перечислить (каталог отсутствует, ошибка ввода-вывода), `sync` завершается ошибкой `Errc::io_error` до транзакции и не помечает ни одной строки `missing`. Существующий пустой `assets/` — это не недоступность: все строки получают `missing`.
9. **Инвариант (ядро тестов).** После любого набора изменений на диске `sync` даёт те же строки, что `rebuild` на том же диске (сравнение всех столбцов `assets`, дочерних таблиц и FTS), **за исключением** строк, помеченных по п. 4. Помеченные строки `rebuild` удаляет (`missing`) или не создаёт (нечитаемый файл).
10. **Отчёт** (`SyncReport`, JSON CLI):
    - `added`, `updated`, `unchanged` — числа;
    - `missing` — список `asset_id`, получивших пометку в этом прогоне;
    - `skipped` — путь, причина, `message_id`, как в `rebuild`;
    - `ignored`, `duration_ms`, `full`.

    Строки, уже помеченные `missing` раньше, в `missing` повторно не попадают, но и в `unchanged` не считаются.
11. **CLI.** `am library sync <dir> [--full] [--json] [--verbose]`.
    - Код 0 при успешной фиксации, даже если есть `skipped`/`missing`. В этом случае текстовый вывод предупреждает о них, а JSON содержит те же поля.
    - Код 4 — блокировка, только чтение, `rebuild_required`, I/O, транзакция.
    - Документация — `docs/user/cli.md` (английский; убрать фразу «filesystem change detection is planned separately» в разделе `list`), `CHANGELOG.md`.
12. **`message_id`:** `am.catalog.asset_missing`, `am.catalog.newer_minor` — новые, в `resources/messages/en.toml`. Остальные — существующие.

## Тесты

Все синтетические ассеты создаются через `AssetRepository`/`Catalog`, как в T-017. Golden ZIP копируются только во временные каталоги теста. `req` — по образцу T-017.

1. **Без изменений** (`req: K-3`): после `rebuild` выполняется `sync` → `unchanged = N`, `added = updated = 0`. Ни один ZIP не открывался (счётчик через per-instance hook в `IndexerHooks`, без глобального состояния).
2. **Добавление, изменение, перемещение** (`req: K-3`):
   - новый ZIP скопирован → `added`;
   - ZIP заменён другой ревизией (через `Library` без `on_indexed` или копированием готового ZIP) → `updated`, новая ревизия и новый FTS;
   - ZIP перемещён в `trash/` → `updated`, `in_trash = 1`.
3. **`missing` и возврат:** ZIP удалён → `missing` в отчёте, у строки `health = missing`, дочерние строки и `title` сохранены. Повторный `sync` — без повторной пометки. Файл возвращён с исходным `stat` → файл перечитан, `health` по результату чтения (`ok`).
4. **Ставший нечитаемым:** ZIP заменён испорченным → строка сохранена, `health = corrupt`, `health_detail` = `message_id`, `zip_size`/`zip_mtime_ns` новые, путь в `skipped`. Повторный `sync` файл не открывает (hook-счётчик).
5. **Дубликаты:** один `asset_id` в `assets/` и в `trash/` → победитель тот же, что у `rebuild`.
6. **`--full`:** ZIP подменён с восстановлением исходных размера и mtime (`fs::last_write_time`; если ФС не позволяет — SKIP с причиной) → обычный `sync` изменения не видит, `--full` — видит.
7. **Инвариант п. 9:** сценарий с изменениями 2–5 → снимок всех таблиц после `sync` совпадает со снимком после `rebuild` на копии библиотеки, за вычетом помеченных строк.
8. **Атомарность:** внедрённый отказ посреди `sync` → индекс и пометки прежние, отдельный читатель видит прежние строки.
9. **Отказы:**
   - `rebuild_required = 1` → `am.catalog.rebuild_required`, индекс не изменён;
   - библиотека только для чтения → отказ;
   - `assets/` удалён → `io_error`, ни одной пометки `missing`;
   - пустой `assets/` → все строки `missing`.
10. **newer_minor** (`req: K-3|K-6`): `pos-newer-minor` → `rebuild` индексирует строку `health = warn`, `health_detail = am.catalog.newer_minor`, без дочерних строк и `fts_names`, с `fts_assets`. Тест 11 T-017 (весь корпус) обновить: ветка V2 `not_applicable_newer_minor` теперь ожидает строку `warn`.
11. **CLI** (`req: K-3`): init → создание → `sync --json` (`unchanged`) → удаление файла → `sync --json` (`missing`) → `list --json` показывает `missing`. Плюс текстовый вывод и код 4 при `rebuild_required`. Ограничения Linux — как у `cli_library_rebuild_list`, с явной причиной SKIP.

## Файлы (что можно менять)

- Изменяемые:
  - `include/am/storage/db/index_store.hpp`, `src/storage/db/index_store.cpp`;
  - `include/am/services/catalog/indexer.hpp`, `src/services/catalog/indexer.cpp`;
  - `include/am/application/library_service.hpp`, `src/application/common/library_service.cpp`, `src/application/cli/main.cpp`;
  - `tests/unit/test_indexer.cpp` (или новый `tests/unit/test_sync.cpp` с регистрацией в `tests/CMakeLists.txt`), `tests/cli/test_library_cli.py`;
  - `resources/messages/en.toml`, `docs/user/cli.md`, `CHANGELOG.md`;
  - этот файл (отчёт), строка T-018 в `docs/tasks/README.md`.
- **Не менять:**
  - `schemas/sql/*.sql` — если пометки не ложатся на схему, остановиться и описать;
  - формат 1.0 (схема, golden ZIP), кодек, V1–V4, протокол фиксации, `AssetRepository`, `Library`;
  - ADR, спецификации, матрицы, roadmap, `vcpkg.json`.

## Критерии приёмки

1. `tools\build_min.cmd --ci`, `tools\build_cli.cmd --ci`, `tools\build_cli.cmd --offline --ci` — PASS; `tools/build_linux.sh cli --ci` в WSL — PASS. Число тестов не уменьшилось; `req_report` — 0 неверных ID; `arch_check` PASS.
2. Тесты 1–11 выполнены. В отчёте — имена тестов, пример `am library sync --json` и список значений `CheckStatus` с веткой обработки каждого (п. 5).
3. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-018`, без слияния, push и переписывания истории.

## Вне объёма

- Наблюдение efsw, debounce, периодичность, сверка при открытии библиотеки в GUI — открытый п. 4 §4.9 ТЗ; отдельная задача вместе с долгоживущим процессом.
- Статус библиотеки `unavailable` в GUI и возврат носителя (проверка LibraryID, `pending_ops`) — ADR-0031 п. 5; здесь только отказ `sync` без пометок.
- Корзина как операция (`trash`/`restore`/`purge`), `add-ready`, конфликты ID и независимая копия.
- Разрешение конфликтов при внешнем изменении ZIP (открытый п. 4 §4.9 ТЗ): `sync` только отражает состояние диска.
- Замеры К-112 (здесь только `duration_ms`).

## Отчёт исполнителя

### Ветка и коммиты

- Ветка: `codex/T-018`, создана от `main` (`1e7b826`).
- `ca4a103` — сверка по stat, транзакционные операции индекса, CLI, тесты и пользовательская документация.
- `Document T-018 execution and validation` — отдельный коммит этого отчёта и перевода в `review`. Слияние, push и переписывание истории не выполнялись.

### Выполнение критериев

1. Обязательные сборки и фактические итоги приведены ниже. SQL, схема формата, golden ZIP, валидаторы, `Library`, `AssetRepository` и протокол фиксации не изменены.
2. Добавлены 11 тестов `Sync.*` и процессный `cli_library_sync`. Обновлён `Indexer.EntireGoldenCorpusRebuildsUsingExpectedValidationLevels`: newer_minor теперь ожидает индексированную строку `warn`.
3. Изменения находятся только в разрешённых файлах, реализация закоммичена в ветку задачи. Исходный пользовательский `3D_am_open_questions_e2472e9.zip` не изменён и в коммиты не включён.

Соответствие обязательным сценариям (имена ниже — из набора `Sync`):

| № | Тест и проверенный результат |
|---|---|
| 1 | `UnchangedDoesNotOpenZipAndFullRereads`: три неизменённых ZIP, 0 вызовов чтения; полный снимок индекса и meta прежний; `full` читает все три. |
| 2 | `AddsExternalCopyUpdatesRevisionFtsAndTrashPath`: внешняя копия добавлена, raw Library change обновляет revision и FTS, перемещение в trash обновляет путь и `in_trash`. |
| 3 | `MissingRetainsAllOtherColumnsChildrenAndRestoresSameStat`: сохранены title, дочерние строки и FTS; повторная пометка отсутствует; возврат с прежним stat перечитывается и восстанавливает исходный снимок. |
| 4 | `UnreadableRetainsSummaryAndChildrenCachesStatAndRetriesNewFile`: сохранены последние данные, выставлены corrupt/detail и новый размер; повторный sync не читает повреждённый ZIP; новый нечитаемый файл повторно проверяется без создания строки. |
| 5 | `SharedCandidateRulesDuplicatesIgnoredAndIdMismatch`: sync и rebuild выбирают assets перед trash, одинаково сообщают duplicate_id, id_mismatch и ignored. Перечисление кандидатов общее. |
| 6 | `FullDetectsEqualSizeAndRestoredMtime`: обычный sync не видит подмену с равным stat, full видит повреждение. На DrvFS — явный SKIP, если точное восстановление mtime невозможно. |
| 7 | `CompleteSnapshotMatchesRebuildOnCopiedDiskExceptRetainedMarks`: сравниваются все столбцы assets, все дочерние таблицы и логические строки FTS; исключены только сохранённые missing/corrupt ассеты. Включён насыщенный `pos-full`. |
| 8 | `FaultRollsBackEveryChangeAndReadersSeeOldStateUntilCommit`: отказ после добавления и пометки missing откатывает весь индекс и meta; отдельный читатель до COMMIT видит прежний снимок, после успешного COMMIT — новый. |
| 9 | `RefusesRebuildFlagReadOnlyAndUnavailableRootsButEmptyIsMissing`: флаг rebuild_required и read-only запрещают sync; недоступность каждого из assets/trash не меняет индекс; существующий пустой assets приводит к missing. |
| 10 | `NewerMinorSummaryWorksForRebuildUpdateAndSync`: все три пути дают warn/newer_minor, NULL primary_preview, только сводку и fts_assets. `MapExplicitlyHandlesEveryCheckStatusAndRequiredNewerMinorType`: все статусы V1/V2, отсутствие/неверный тип обязательных полей, переполнение суммы размеров без изменения обязательного message_id. |
| 11 | `cli_library_sync`: init → отказ с кодом 4 → создание ZIP → rebuild → unchanged → full → missing в JSON и текстовом предупреждении → list с сохранённым title → возврат → corrupt/skipped. Хеш userdata.sqlite сохранён. На Linux проверен отказ записи с явной причиной SKIP. |

`Catalog::sync` использует тот же mutex, что наблюдатель фиксаций. Сверка выполняется одной транзакцией; перечисление обоих каталогов завершается до её начала. Пометки обновляют только поля, перечисленные в задаче; sync не изменяет meta/rebuild_required. Явные `<vector>` и `<functional>` добавлены в `index_store.hpp`.

Все значения `repository::CheckStatus` имеют явные ветки:

| Значение | V1 | V2 при прошедшем V1 |
|---|---|---|
| `passed` | продолжить проверку V2 | индексировать |
| `not_applicable_newer_minor` | отказ | индексировать только при `format_support = newer_minor` |
| `not_performed` | отказ | отказ |
| `partial` | отказ | отказ |
| `failed` | отказ | отказ |

Неизвестное значение также приводит к отказу. Для newer_minor обязательные типы проверяются до проекции; неподтверждённые ключи и ссылки не попадают в дочерние таблицы.

### Команды и фактические итоги

Минимальная сборка: `tools\build_min.cmd --ci` — PASS, 86/86:

```text
100% tests passed, 0 tests failed out of 86
Total Test time (real) = 18.12 sec
req_report: 13 IDs, 86 tests, 49 without req, 0 invalid IDs
```

Целевые прогоны:

- Windows: `tools\build_cli.cmd --offline --ci -R Sync` — `100% tests passed, 0 tests failed out of 11`; `-R Indexer` — 11/11 PASS; `-R cli_library` — 6/6 PASS.
- WSL: `tools/build_linux.sh cli --ci -R Sync` после исправлений — `100% tests passed, 0 tests failed out of 11`: 10 PASS, 1 SKIP (`Filesystem cannot restore mtime exactly`). `req_report: 45 IDs, 340 tests, 48 without req, 0 invalid IDs`. Пересобранные файлы без предупреждений.
- Полный `tools\build_cli.cmd --ci` — PASS, 343/343, без SKIP:

```text
100% tests passed, 0 tests failed out of 343
Total Test time (real) = 897.13 sec
req_report: 45 IDs, 343 tests, 49 without req, 0 invalid IDs
```

Полный `tools\build_cli.cmd --offline --ci` — PASS, 343/343, без SKIP:

```text
100% tests passed, 0 tests failed out of 343
Total Test time (real) = 824.55 sec
req_report: 45 IDs, 343 tests, 49 without req, 0 invalid IDs
```

Полный `tools/build_linux.sh cli --ci` в WSL — PASS, 340 тестов: 324 PASS, 16 SKIP, 0 FAIL:

```text
100% tests passed, 0 tests failed out of 340
Total Test time (real) = 1864.94 sec
req_report: 45 IDs, 340 tests, 48 without req, 0 invalid IDs
```

SKIP: 14 прежних ограничений Linux/Windows-only тестов и два новых — точное восстановление mtime на DrvFS и производственная запись `cli_library_sync` (неподтверждённый фиксированный NTFS). Проверка sync в сервисном слое выполнена с синтетическими capabilities; сценарий равного stat выполнен на Windows без SKIP.

`arch_check`, `schema_check`, `sql_check` — PASS во всех четырёх обязательных прогонах. Число тестов не уменьшилось: минимальная сборка 86; Windows 331 → 343; Linux 328 → 340. Во всех прогонах `req_report` — 0 неверных ID.

Логи и машинные отчёты — `.cache/t018-*.log`, `*-junit.xml`, `*-result.json`, `*-req_report.json`.

Фактический вызов на синтетической библиотеке:

```text
am library sync .cache/t018-cli-example/library --json
```

```json
{"added":0,"updated":0,"unchanged":1,"missing":[],"skipped":[],"ignored":[],"duration_ms":0,"full":false,"command":"library sync"}
```

После удаления ZIP (код завершения 0):

```json
{"added":0,"updated":0,"unchanged":0,"missing":["1620334a-00a3-4746-8ddd-9ca993946a55"],"skipped":[],"ignored":[],"duration_ms":5,"full":false,"command":"library sync"}
```

### Отклонения и вопросы

- Отклонений от требований и разрешённого состава файлов нет. Новые зависимости не добавлялись; реальная коллекция не использовалась.
- В первом целевом WSL-прогоне выявлено округление mtime при копировании тестового ZIP-дерева. Для теста инварианта заданы одинаковые представимые временные метки до индексации и копирования; проверка всех столбцов, включая `zip_mtime_ns`, сохранена. Отдельный тест точного восстановления mtime пропускается только по предусмотренному задачей условию.
- Проверкой граничного случая подтверждён и исправлен суффикс `content_bytes_overflow` у newer_minor: обязательный `health_detail = am.catalog.newer_minor` теперь сохраняется.
- Генерацию общих interoperability-fixtures `.cache/zip-tests/written` в Windows и WSL разводил по времени, временно задерживая родительский CTest WSL. Wall-time полного WSL-прогона включает ожидание и не является замером NFR.
- **Вопросы:** нет.

## Приёмка

Дата: 2026-11-16. Принята, слияние `bda5c19` (`--no-ff`). Прогон управляющего (все `.cpp` помечены изменёнными): `build_min --ci` 86/86, `build_cli --ci` 343/343, `build_cli --offline --ci` 343/343; req_report — 0 неверных ID. Linux — по отчёту исполнителя (324 PASS, 16 SKIP) и CI после публикации. Ревью: общий перечень кандидатов с `rebuild`, перечисление до транзакции, ключ «без изменений» по п. 3, пометки без удаления, явные ветки всех `CheckStatus`; замечание 1 приёмки T-017 закрыто. Замечаний нет.
