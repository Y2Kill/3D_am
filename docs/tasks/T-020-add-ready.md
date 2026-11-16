# T-020. Импорт готового ассета и конфликты ID: `am library add-ready` (M2, P0.5 — часть 5)

**Статус:** review · **Веха:** M2 · **Основание:** ТЗ v1.8 §6.1 п. 2–3, §6.3 п. 3–5 [СОГЛАСОВАНО], §15 критерий 31, §13.2 сценарий 14; §4.5 п. 4 (неизвестную версию не изменять); ADR-0025 п. 1, 4, 7 (единственный писатель, блокировка ассета, события после `indexed`); ADR-0006 (журнал `pending_ops`, `kind = import_ready` уже есть в `state.sql`); ADR-0008 (классы конфликта ID, уточнены ADR-0036 п. 4); SPEC `container_manifest_v1.md` §10 («импорт готового ассета ревизию не меняет»); T-017…T-019 (индекс, `sync`, корзина)

## Цель

1. `AssetRepository::import_ready` размещает ZIP собственного формата в библиотеке **побайтно без изменений**: ID, ревизия, история и метаданные сохраняются (§6.1 п. 2, критерий 31).
2. Совпадение ID с уже имеющимся ассетом классифицируется по §6.3 п. 4. Замена выполняется только по явному решению пользователя для конкретной увиденной версии.
3. CLI `am library add-ready`, индекс обновляется через `on_indexed`.

## Решения управляющего (не пересматривать в задаче)

1. **Без изменения байтов.** В библиотеку попадает точная копия исходного файла: `assets/<xx>/<uuid>.zip`, где `<uuid>` = `asset.id` из манифеста. Имя исходного файла роли не играет.
   - ZIP не перезаписывается через `ZipWriter`, `revision` не растёт, записи `history` нет (SPEC §10).
   - Запись в «журнал библиотеки» из SPEC §10 здесь **не создаётся**: таблицы для него нет, она появится с заданиями (`IJobStore`). В результате операции возвращается `op_id` журнала фиксации.
2. **Протокол фиксации общий с `create`/`change`** (ADR-0006, `kind = import_ready`).
   - `commit_zip` получает второй источник содержимого: побайтовое копирование из файла вместо `producer`. Ровно один из двух источников обязателен.
   - Временный файл в `.am/.am-tmp` (ADR-0007; см. решение по В-1), хеш SHA-256 считается при копировании. `verify_written` для копии заменяется сравнением: размер и SHA-256 временного файла равны посчитанным при копировании.
   - Дальше без изменений: probe `own`, верификатор (п. 3), хеш манифеста, `prepared → verified` (с доказательством), повторная проверка базы, замена, `committed → indexed`. Восстановление `recover()` — существующая ветка для create/update, без нового кода восстановления.
   - После копирования источник проверяется повторно (размер и mtime на момент открытия). Изменился → `Errc::conflict` (`am.repo.import_source_changed`), временный файл и строка журнала удаляются.
   - Свободное место — прежняя проверка `commit_zip` с `estimated_size` = размер источника.
   - Существующие точки отказа и kill-тесты create/change должны пройти без правок.
3. **Проверка перед регистрацией — на временной копии, не на источнике.** Полный V1–V4 через `verify_container`/тот же верификатор, что и для `create`. Принимается, если:
   - probe = `own`. Иначе: `foreign_zip`/`not_zip`/`invalid_zip` → классификация `not_own` (сценарий «создание из исходных материалов» — отдельная команда `am asset create`); `own_unsupported_version` → `am.repo.import_unsupported_version`; `own_corrupt` → `am.repo.import_invalid`;
   - V1 `passed`; V2 `passed` или `not_applicable_newer_minor`;
   - V3 `passed` или `partial` (непроверенная семантика плагинов — не ошибка, R-09); для newer_minor V3 `not_performed` допустим;
   - V4: **каждый** зарегистрированный поток `verified_sha256`. `unsupported`/`unverifiable`/`failed` → отказ `am.repo.import_invalid` с отчётом проверки.
   - **newer_minor** принимается без изменения (§4.5 п. 4 запрещает изменение, а не хранение) с предупреждением `am.repo.import_newer_minor`; индекс уже умеет такие ассеты (T-018).
4. **Источник внутри библиотеки запрещён:** путь источника после нормализации внутри корня библиотеки → `Errc::invalid_argument` (`am.repo.import_source_inside_library`). Файлы, положенные в `assets/` вручную, подхватывает `sync`.
5. **Классификация по ID** (§6.3 п. 4; сравнение с **фактическим ZIP**, а не с индексом):

   | Класс | Условие | Действие по умолчанию |
   |---|---|---|
   | `new` | ни `assets/<xx>/<uuid>.zip`, ни `trash/<uuid>.zip` нет | импорт → `imported` |
   | `identical` | SHA-256 манифеста входящего = SHA-256 манифеста текущего (манифест содержит SHA-256 всех потоков, V4 входящего пройден) | ничего не пишется → `identical`; в ответе `zip_bytes_identical: true/false` |
   | `newer_revision` / `older_revision` | ревизии различаются | `conflict`, файл не тронут |
   | `same_revision_different` | ревизии равны, манифесты различаются | `conflict` |
   | `current_unreadable` | текущий ZIP есть, манифест не читается | `conflict`; замена недоступна (п. 6) — сначала `trash`/`purge` |
   | `in_trash` | ID лежит в `trash/` | `conflict`; сначала `restore` или `purge` |

   Более высокая ревизия сама по себе права на замену не даёт (§6.3 п. 4). Классификация и чтение текущего ZIP выполняются под блокировкой ассета.
6. **Замена — только явная и только увиденной версии** (§6.3 п. 3, ADR-0008):
   - `ImportReadyRequest.replace_expected = <manifest_sha256 текущего>`; значение берётся из ответа о конфликте (`current.manifest_sha256`).
   - Под блокировкой: текущий манифест не равен ожидаемому → `Errc::conflict` (`am.repo.import_current_changed`), ничего не меняется.
   - База фиксации = снимок текущего (`size`, `mtime_ns`, `manifest_sha256`), как у `change`: внешнее изменение между проверкой и заменой → `am.commit.base_changed`.
   - Замена допустима для `newer_revision`, `older_revision`, `same_revision_different`. Для `older_revision` — предупреждение `am.repo.import_older_revision`. Для `identical`, `new`, `current_unreadable`, `in_trash` параметр замены → `Errc::invalid_argument` (`am.repo.import_replace_not_applicable`).
   - `userdata` сохраняется: ID тот же.
7. **Независимая копия (ADR-0036 п. 4) — не в этой задаче.** Она требует отображения всех ID и ссылок `old→new` и отказа по O-05…O-07; это задача T-021. В ответе о конфликте действия `copy` нет.
8. **Уровни (ADR-0024).**
   - `storage`: `AssetRepository::import_ready(const ImportReadyRequest&)` → `ImportReadyResult{classification, status, ref, op_id?, incoming, current?, warnings}`; `incoming`/`current` — `{revision, manifest_sha256, zip_size, format_minor}`, для `current_unreadable` — только `zip_size`.
   - `services`: `Catalog::import_ready` под тем же mutex, предупреждения индекса — как у `create`.
   - `application`: фасад; CLI только через него.
9. **События и индекс.** `IndexedEvent.kind = import_ready`, `revision` — фактическая ревизия из манифеста. Наблюдатель `Catalog` читает ассет по месту `assets/` (существующая ветка `update`). Сбой индекса — как в T-017: ZIP остаётся зарегистрированным, предупреждение, `rebuild_required = 1`.
10. **Несколько источников за один вызов CLI** обрабатываются **по очереди**, каждый — отдельной операцией. Два файла с одним ID: второй классифицируется относительно уже импортированного первого (`identical` или конфликт). Автоматический пропуск достоверно идентичных — это правило §6.3 п. 5, конфликты откладываются (сообщаются), а не разрешаются.
11. **CLI.**
    - `am library add-ready <dir> <zip>... [--json] [--verbose]`.
    - `am library add-ready <dir> <zip> --replace <manifest-sha256> [--json]` — ровно один источник; иначе код 2.
    - JSON: `results[]` с `source`, `asset_id` (`null`, если не свой формат), `classification`, `status` (`imported` / `identical` / `replaced` / `conflict` / `failed`), `incoming`, `current`, `zip_bytes_identical`, `error`. Отчёт проверки — при `--verbose` и при отказе проверки.
    - Код 0 — все `imported`/`identical`/`replaced`; 4 — хотя бы один `conflict`/`failed`; 2 — ошибка аргументов.
    - Каталоги, рекурсивный обход и маски не поддерживаются: только явные пути к файлам.
    - Документация `docs/user/cli.md`, `CHANGELOG.md`.
12. **`message_id`:** `am.repo.import_source_changed`, `am.repo.import_unsupported_version`, `am.repo.import_invalid`, `am.repo.import_newer_minor`, `am.repo.import_source_inside_library`, `am.repo.import_current_changed`, `am.repo.import_older_revision`, `am.repo.import_replace_not_applicable`, `am.repo.import_conflict`, `am.repo.not_own_format` (если такого ещё нет — проверить каталог); английский текст в `resources/messages/en.toml`. Повторно использовать существующие ID, где смысл тот же.

## Тесты

Все юнит-тесты — `RecordProperty("req", "K-31")` (или `SC-14`, где проверяется сценарий целиком).

1. **Новый ассет:** ZIP, созданный в другой библиотеке (`create` + изменение до ревизии 3) → `import_ready` → файл в `assets/<xx>/<uuid>.zip`, SHA-256 всего файла равен исходному, ревизия и `history` прежние, строка индекса есть.
2. **Идентичный:** повторный импорт того же файла → `identical`, `zip_bytes_identical = true`, mtime и байты текущего не меняются, журнал пуст. Тот же манифест в ZIP с другим порядком записей (собрать через `zip_builder`) → `identical`, `zip_bytes_identical = false`.
3. **Конфликты:** `newer_revision`, `older_revision`, `same_revision_different`, `current_unreadable`, `in_trash` → `conflict`, оба файла байт-в-байт прежние, `current`/`incoming` в результате верны.
4. **Замена:** с верным `replace_expected` для `newer_revision` → `replaced`, байты = входящие, индекс показывает новую ревизию, SHA-256 основного `userdata.sqlite` прежний. Для `older_revision` — то же плюс предупреждение. Неверное ожидаемое значение → `am.repo.import_current_changed`, ничего не изменилось. Замена для `new`/`identical`/`in_trash` → `am.repo.import_replace_not_applicable`.
5. **Отказы проверки:** чужой ZIP, не-ZIP, `own_corrupt`, мажорная версия 2, неверный SHA-256 потока, ошибка V3 → ни файла в `assets/`, ни временного файла, ни строки журнала; классификация/`message_id` по п. 3.
6. **newer_minor:** ZIP с `minor = 1` (без изменения байтов) → `imported` + предупреждение, индекс `health = warn` (как T-018).
7. **Источник меняется во время копирования** (hook после копирования меняет mtime или размер) → `am.repo.import_source_changed`, следов нет. Источник внутри библиотеки → отказ до любых изменений.
8. **Сбои:** все точки `FaultPoint` для `import_ready` (новый и замена) с последующим `recover()` → либо прежнее состояние, либо полностью зарегистрированный ассет с индексом; kill по образцу T-009 хотя бы для `after_verified_journal` и `after_replace`.
9. **Гонка с появлением цели:** между классификацией `new` и заменой в `assets/` появился файл (hook) → `am.commit.base_changed`/`am.repo.asset_exists`, появившийся файл не тронут.
10. **Индекс:** сбой наблюдателя → ZIP зарегистрирован, предупреждение, `rebuild_required = 1`; после `rebuild` строка есть.
11. **CLI** (`req: K-31`, `SC-14`):
    - init → `asset create` во второй библиотеке → `add-ready` трёх файлов (новый, его дубликат, чужой ZIP) → коды и поэлементные результаты, код 4;
    - `add-ready` более новой ревизии → `conflict` → `--replace <sha из ответа>` → 0, `list` показывает новую ревизию;
    - `--replace` с двумя источниками → 2.

    Ограничения Linux — как у прежних CLI-сценариев записи, с явной причиной SKIP.

## Файлы (что можно менять)

- Изменяемые:
  - `include/am/storage/repository/repository.hpp`, `src/storage/repository/repository.cpp`;
  - `include/am/storage/commit/protocol.hpp`, `src/storage/commit/protocol.cpp` — только побайтовый источник и связанные с ним проверки;
  - `include/am/services/catalog/indexer.hpp`, `src/services/catalog/indexer.cpp` (`Catalog::import_ready`);
  - фасад `library_service`, `src/application/cli/main.cpp`;
  - тесты: новый `tests/unit/test_import_ready.cpp`, дополнения в `tests/cli/test_library_cli.py`, регистрация в `tests/CMakeLists.txt`; вспомогательные функции в `tests/unit/zip_builder.*` — только добавлением;
  - `resources/messages/en.toml`, `docs/user/cli.md`, `CHANGELOG.md`;
  - этот файл (отчёт), строка T-020 в `docs/tasks/README.md`.
- **Не менять:**
  - `schemas/sql/*.sql`, `state_journal.cpp` (виды и переходы create/update уже подходят; если нет — остановиться и описать);
  - формат 1.0 (схема, golden ZIP), кодек, V1–V4 и их правила;
  - поведение `create`/`change`/`trash`/`restore`/`purge` и их точки отказа;
  - `Library`, ADR, спецификации, матрицы, roadmap, `vcpkg.json`.

## Критерии приёмки

1. `tools\build_min.cmd --ci`, `tools\build_cli.cmd --ci`, `tools\build_cli.cmd --offline --ci` — PASS; `tools/build_linux.sh cli --ci` в WSL — PASS. Число тестов не уменьшилось; `req_report` — 0 неверных ID; `arch_check` PASS.
2. Тесты 1–11 выполнены. В отчёте — имена тестов, пример JSON `add-ready` (новый, `identical`, `conflict`, `replaced`), описание побайтового источника в `commit_zip`.
3. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-020`, без слияния, push и переписывания истории.

## Вне объёма

- Независимая копия с новым AssetID (ADR-0036 п. 4) — T-021.
- Импорт каталогов и рекурсивный поиск готовых ZIP, распознавание готового ZIP внутри обычного архива, выбор сценария GUI (§6.1 п. 3).
- Сохраняемые политики пакетного импорта (§6.3 п. 5) сверх «пропустить идентичные, сообщить о конфликтах»; задания и прогресс (`IJobStore`).
- Журнал библиотеки об импорте (SPEC §10) — с `IJobStore`.
- Сравнение содержимого для GUI (§6.3 п. 6), совпадения по исходным архивам и составу (§6.3 п. 1–2) — M3a.
- Межбиблиотечное перемещение (ADR-0025 п. 9, P1).

## Отчёт исполнителя

### Состояние работы

Выполнение остановлено при сверке п. 2 задачи с ADR-0007 и действующим протоколом. Основание — п. 3 `AGENTS.md`: при противоречии задачи ADR остановиться и передать вопрос, не назначая архитектурное решение самостоятельно. T-020 не завершена; передача отчёта не означает готовность реализации к приёмке.

### Ветка и коммиты

- Ветка `codex/T-020`, создана от `main` (`5bd3f10`).
- Исходники, схемы, формат и тесты не менялись. Изменения ограничены этим отчётом и статусом строки T-020 в реестре.
- `Report T-020 temporary directory conflict` — коммит этого отчёта и статуса review. Слияние, push и переписывание истории не выполнялись.

### Выполнение критериев приёмки

1. `tools\build_min.cmd --ci` выполнена: 86/86 PASS, включая arch_check/schema_check/sql_check; req_report — 0 неверных ID. Полные CLI/offline/WSL-прогоны до разрешения В-1 не запускались; критерий приёмки целиком не выполнен.
2. Реализация, новые тесты и JSON-примеры не подготовлены: выбор временного каталога затрагивает обязательную совместимость с общим восстановлением.
3. Работа ведётся в ветке задачи; материалы владельца, реальная коллекция и исходный пользовательский `3D_am_open_questions_e2472e9.zip` не изменены. Зависимости не добавлялись.

### Команда и фактический вывод

```text
tools\build_min.cmd --ci
100% tests passed, 0 tests failed out of 86
Total Test time (real) = 14.17 sec
req_report: 13 IDs, 86 tests, 49 without req, 0 invalid IDs
```

Артефакты: `.cache/t020-min.log`, `.cache/t020-min-junit.xml`, `.cache/t020-min-result.json`. Статус review означает передачу В-1 управляющему, а не сдачу готовой реализации.

### Вопросы

**В-1. Какой временный каталог требуется для побайтового источника `commit_zip`?**

- Пункт 2 T-020 прямо указывает: «Временный файл в `.am/tmp`».
- ADR-0007 задаёт `.am/.am-tmp/` для временных файлов операций (`docs/decisions/ADR_log.md`, строка 142); ADR-0025 также называет `.am/.am-tmp/`.
- `commit::layout()` строит временный путь в `.am/.am-tmp` (`src/storage/commit/protocol.cpp`, строка 74). `recover()` использует тот же layout и отвергает строку, если `temp_path` ему не соответствует.
- T-020 одновременно требует общий протокол с прежними операциями, существующую ветку восстановления «без нового кода восстановления» и запрещает менять ADR и поведение прежних операций.

Поэтому буквальное использование `.am/tmp` не совместимо с неизменённым восстановлением, а самостоятельная замена требования на `.am/.am-tmp` нарушила бы правило о решениях управляющего.

Предлагаемая развязка: считать `.am/tmp` в п. 2 опечаткой и использовать существующий `.am/.am-tmp` без изменения layout/recover. Если нужен именно новый каталог `.am/tmp`, управляющему сначала требуется уточнить ADR, раскладку и разрешённый объём изменений восстановления. Вопрос передан владельцу через интерфейс уточнения; ответ пока ожидается.

### Отклонения

Реализация не начата вследствие обязательной остановки по В-1. Архитектурных решений вместо владельца не принимал.

## Решение управляющего по В-1 (2026-11-16)

**Вариант 1: `.am/tmp` в п. 2 — опечатка управляющего.** Используется существующий `.am/.am-tmp` (ADR-0007, ADR-0006), тот же каталог и та же раскладка временного файла `<asset>.<op_id>.zip`, что в `commit_zip`/`recover()`. Протокол и ADR не меняются, новый код восстановления не нужен. П. 2 решений исправлен. Задача снова `ready`: продолжать с реализации, полные прогоны — в отчёте.

## Отчёт исполнителя — завершение по В-1

Предыдущий отчёт фиксирует остановку до решения управляющего. Работа возобновлена после `a450b7e`: используется существующий `.am/.am-tmp/<asset>.<op_id>.zip`. Настоящий отчёт описывает завершённую реализацию.

### Ветка и коммиты

- Ветка `codex/T-020`, исходный `main` — `5bd3f10`.
- `7e4d1f5` — первоначальный вопрос В-1; `a450b7e` — решение управляющего.
- `8a4e2fa` — `Implement ready asset imports and explicit ID conflict replacement` (реализация, тесты, каталог сообщений и пользовательская документация).
- `Complete T-020 execution report and submit for review` — отдельный коммит отчёта и статуса. Слияние, push и переписывание истории не выполнялись.

### Что сделано по критериям приёмки

1. Обязательные четыре сборки и прогона выполнены; фактические итоги приведены ниже. Число тестов относительно принятой T-019 выросло: Windows 357 → 371, Linux 354 → 368; минимальная сборка сохраняет 86 тестов. `arch_check`, `schema_check`, `sql_check`, golden-проверки и каталог сообщений прошли; неверных ID требований нет.
2. Добавлены `AssetRepository::import_ready`, `Catalog::import_ready` под общим mutex, фасад и `am library add-ready`. Источники обрабатываются последовательно; классификация использует фактический ZIP под блокировкой ассета. Проверены все классы, замены по ожидаемому SHA-256, сохранение userdata, полный отказ без следов, newer_minor, V3 partial, гонка с появлением цели и сбой индекса. Ни ревизия, ни история, ни ZIP-байты при импорте не меняются. Реальные JSON-примеры приведены ниже.
3. Изменены только файлы из разрешённого списка. Схемы, ADR, опубликованный формат, golden ZIP, `Library`, журнал состояний, материалы владельца и реальная коллекция не менялись. Зависимости не добавлялись. Пользовательский незарегистрированный ZIP в корне проекта не затронут. При сдаче все изменения задачи закоммичены в её ветку.

### Побайтовый источник и восстановление

В `CommitRequest` добавлен `CopyFileSource` как альтернатива `producer`: одновременно использовать оба источника нельзя. Входной файл копируется блоками с потоковым SHA-256 через прежний sink и точки отказа; после flush/close повторно проверяются размер и mtime источника. Размер и SHA-256 защищённой временной копии сравниваются с результатом копирования вместо `verify_written`. Далее используются прежние probe, полный верификатор, доказательство `NewFileEvidence`, проверка снимка базы, замена и переходы журнала. Ошибка проверки удаляет временный файл и строку журнала. Дубликаты и конфликты также полностью проверяются на временной копии; публикация пропускается, временные следы очищаются, текущий ZIP не меняется.

Callback подготовки под `AssetLock` получает текущий манифест и снимок базы; callback решения о публикации выполняется после проверки копии. Для побайтового источника база дополнительно проверяется после hook `before_replace`, чтобы не затереть появившуюся цель. Ветка `producer`, её точки отказа и порядок прежних операций сохранены.

`commit::recover`, layout, состояния и SQL-переходы не менялись: `import_ready` проходит существующую ветку create/update. На уровне репозитория подключено уведомление `import_ready` с фактической ревизией. Для уже `indexed` строки, которую общая FSM только очищает, репозиторий повторяет уведомление нового вида: это покрывает остановку до обычного post-indexed callback. Уведомления прежних операций не менялись. Сбой наблюдателя не откатывает ZIP; Catalog возвращает предупреждение и выставляет `rebuild_required`.

### Выполненные новые тесты

Все новые юнит-тесты имеют `req=K-31` или `SC-14`:

- `ImportReady.NewRevisionThreeKeepsEveryByteHistoryAndActualIndexRevision` — тест 1 задачи.
- `ImportReady.IdenticalManifestSkipsPublicationAndReportsWholeZipEquality` — тест 2, включая другой порядок ZIP-записей.
- `ImportReady.EveryConflictClassUsesCurrentFileAndPreservesBothInputs` — тест 3.
- `ImportReady.ExplicitExpectedHashReplacesNewerOlderAndDifferentAndKeepsUserdata` и `ReplacementRejectsNewIdenticalUnreadableAndTrash` — тест 4; SHA-256 основного userdata сравнивается при закрытых соединениях, присутствует пользовательская строка.
- `ImportReady.RejectsForeignNotZipCorruptMajorTwoBadShaAndV3WithoutFootprints` — тест 5.
- `ImportReady.NewerMinorAndPartialPluginSemanticsAreStoredUnchanged` — тест 6 и допустимость V3 partial.
- `ImportReady.CopiedDigestRejectsDamagedTemporarySinkBeforePublication` — повреждение копии sink до публикации.
- `ImportReady.SourceChangesAfterCopyAndInsideLibraryAreRejected` — тест 7.
- `ImportReady.EveryFaultPointForNewAndReplacementRecoversAtomicallyWithIndex` — тест 8, все 10 точек × новый/замена, Windows и Linux.
- `ImportReady.KillVerifiedAndReplacedForNewAndReplacementRecoverWithIndex` — тест 8, реальный отдельный процесс и `TerminateProcess`, 2 точки × новый/замена на Windows. Helper собран из того же разрешённого файла теста.
- `ImportReady.NewTargetAppearingBeforeReplacementIsNeverOverwritten` — тест 9.
- `ImportReady.IndexFailureDoesNotUndoPublicationAndRebuildRepairs` — тест 10.
- `cli_library_add_ready` (`req:K-31`, `req:SC-14`) — тест 11: пакет новый/идентичный/чужой, конфликт → замена, индекс ревизии 2, userdata и ошибка аргументов с кодом 2.

### Команды и фактический вывод

```text
tools\build_min.cmd --ci
100% tests passed, 0 tests failed out of 86
Total Test time (real) =  24.75 sec
req_report: 13 IDs, 86 tests, 49 without req, 0 invalid IDs
```
PASS: 86; SKIP: 0; FAIL: 0.

```text
tools\build_cli.cmd --ci
100% tests passed, 0 tests failed out of 371
Total Test time (real) = 970.78 sec
req_report: 47 IDs, 371 tests, 49 without req, 0 invalid IDs
```
PASS: 371; SKIP: 0; FAIL: 0.

```text
tools\build_cli.cmd --offline --ci
100% tests passed, 0 tests failed out of 371
Total Test time (real) = 928.94 sec
req_report: 47 IDs, 371 tests, 49 without req, 0 invalid IDs
```
PASS: 371; SKIP: 0; FAIL: 0.

```text
tools/build_linux.sh cli --ci (WSL)
100% tests passed, 0 tests failed out of 368
Total Test time (real) = 1157.85 sec
req_report: 47 IDs, 368 tests, 48 without req, 0 invalid IDs
```
PASS: 349; SKIP: 19; FAIL: 0.


Пропуски Linux: прежние 17 платформенных ограничений плюс новый Windows kill-тест и production CLI-запись `cli_library_add_ready` (требуется проверенный fixed NTFS). Это явные SKIP с причиной, а не PASS. Матрица отказов и сервисный импорт с внедрёнными capabilities реально выполнены на Linux. Проверка `--replace` с двумя источниками исполняется в CLI-тесте до Linux SKIP.

Артефакты полных прогонов: `.cache/t020-{min-final,windows-full,windows-offline,linux-full}.log`, соответствующие `*-junit.xml`, `*-result.json`, `*-req_report.json`, `*-req_report.md`. Дополнительно: целевой Windows 13/13 PASS, WSL 12 PASS / 1 SKIP; все 8 CLI-сценариев Windows PASS. Полные прогоны включают эти тесты повторно как обязательную проверку всей сборки.

### Фактические JSON-примеры

Синтетический ассет `4ced6285-10ff-40fd-bd61-eafc2748c74e`. Последовательно выполнены новый импорт, повторный импорт, импорт ревизии 2 без замены и импорт ревизии 2 с `--replace 2796ed957968ad847961f836f16880859c902c12011c1af0399edbbd136102be`. Коды: 0, 0, 4, 0. Ниже элементы `results` из фактического вывода; `source` сокращён до имени файла. Полный вывод: `.cache/t020-cli-example/{new,identical,conflict,replaced}.json`.

```json
{
  "results": [
    {
      "source": "ready.zip",
      "asset_id": "4ced6285-10ff-40fd-bd61-eafc2748c74e",
      "classification": "new",
      "status": "imported",
      "incoming": {
        "zip_size": "874",
        "revision": 1,
        "manifest_sha256": "2796ed957968ad847961f836f16880859c902c12011c1af0399edbbd136102be",
        "format_minor": 0
      },
      "current": null,
      "zip_bytes_identical": null,
      "error": null
    }
  ]
}
```

```json
{
  "results": [
    {
      "source": "ready.zip",
      "asset_id": "4ced6285-10ff-40fd-bd61-eafc2748c74e",
      "classification": "identical",
      "status": "identical",
      "incoming": {
        "zip_size": "874",
        "revision": 1,
        "manifest_sha256": "2796ed957968ad847961f836f16880859c902c12011c1af0399edbbd136102be",
        "format_minor": 0
      },
      "current": {
        "zip_size": "874",
        "revision": 1,
        "manifest_sha256": "2796ed957968ad847961f836f16880859c902c12011c1af0399edbbd136102be",
        "format_minor": 0
      },
      "zip_bytes_identical": true,
      "error": null
    }
  ]
}
```

```json
{
  "results": [
    {
      "source": "newer.zip",
      "asset_id": "4ced6285-10ff-40fd-bd61-eafc2748c74e",
      "classification": "newer_revision",
      "status": "conflict",
      "incoming": {
        "zip_size": "1618",
        "revision": 2,
        "manifest_sha256": "9c6d5624cbefe30651bda11e8a7a2c8637f8cd53ae49d7f3f97db60d24bf1492",
        "format_minor": 0
      },
      "current": {
        "zip_size": "874",
        "revision": 1,
        "manifest_sha256": "2796ed957968ad847961f836f16880859c902c12011c1af0399edbbd136102be",
        "format_minor": 0
      },
      "zip_bytes_identical": null,
      "error": {
        "code": "am.conflict",
        "message_id": "am.repo.import_conflict",
        "detail": "newer_revision"
      }
    }
  ]
}
```

```json
{
  "results": [
    {
      "source": "newer.zip",
      "asset_id": "4ced6285-10ff-40fd-bd61-eafc2748c74e",
      "classification": "newer_revision",
      "status": "replaced",
      "incoming": {
        "zip_size": "1618",
        "revision": 2,
        "manifest_sha256": "9c6d5624cbefe30651bda11e8a7a2c8637f8cd53ae49d7f3f97db60d24bf1492",
        "format_minor": 0
      },
      "current": {
        "zip_size": "874",
        "revision": 1,
        "manifest_sha256": "2796ed957968ad847961f836f16880859c902c12011c1af0399edbbd136102be",
        "format_minor": 0
      },
      "zip_bytes_identical": null,
      "error": null
    }
  ]
}
```

### Отклонения и вопросы

- В-1 закрыт явным решением управляющего; используется `.am/.am-tmp` без изменения ADR/layout.
- Новый код переходов/восстановления в общем commit-протоколе не добавлялся; подключение и повтор import-ready уведомления репозитория описаны выше.
- Ограничения Linux соответствуют разрешённой задаче политике прежних CLI-сценариев; реальный kill проверен на Windows.
- Открытых вопросов и иных отклонений нет.
