# T-020. Импорт готового ассета и конфликты ID: `am library add-ready` (M2, P0.5 — часть 5)

**Статус:** ready · **Веха:** M2 · **Основание:** ТЗ v1.8 §6.1 п. 2–3, §6.3 п. 3–5 [СОГЛАСОВАНО], §15 критерий 31, §13.2 сценарий 14; §4.5 п. 4 (неизвестную версию не изменять); ADR-0025 п. 1, 4, 7 (единственный писатель, блокировка ассета, события после `indexed`); ADR-0006 (журнал `pending_ops`, `kind = import_ready` уже есть в `state.sql`); ADR-0008 (классы конфликта ID, уточнены ADR-0036 п. 4); SPEC `container_manifest_v1.md` §10 («импорт готового ассета ревизию не меняет»); T-017…T-019 (индекс, `sync`, корзина)

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
   - Временный файл в `.am/tmp`, хеш SHA-256 считается при копировании. `verify_written` для копии заменяется сравнением: размер и SHA-256 временного файла равны посчитанным при копировании.
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
