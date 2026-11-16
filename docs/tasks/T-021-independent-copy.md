# T-021. Независимая копия ассета: `am library copy` (M2, P0.5 — часть 6)

**Статус:** ready · **Веха:** M2 · **Основание:** ТЗ v1.8 §4.4 п. 2 [СОГЛАСОВАНО], §19.4.2.2 (новый AssetID у независимой копии); R-07.1–R-07.9 (`docs/tz/R01-R09_approved_handoff.md`, проверки D-05…D-07); ADR-0036 п. 4 (заменяет ADR-0035 п. 11); ADR-0039 п. 11 (O-05…O-07: в P0 — безопасный отказ); SPEC `container_manifest_v1.md` §1 (раскладка `content/<InputID>/`), §10 (словарь `asset_created{creation_kind, source_asset}`), §11.1; ADR-0025 п. 1, 4, 7; T-020 (замечание 3 приёмки)

## Цель

1. `AssetRepository::copy_independent` создаёт из ассета библиотеки новый ассет с новым AssetID. Переносится текущее состояние, все локальные ID получают новые UUIDv4, все нормативные ссылки переназначаются согласованно. Байты полезных файлов не меняются.
2. Если корректность копии нельзя доказать, операция отказывает и ничего не меняет (R-07.6, R-07.7). Альтернатива для пользователя — обычное копирование ZIP с прежними ID.
3. CLI `am library copy`.

## Решения управляющего (не пересматривать в задаче)

1. **Источник и назначение — одна библиотека, источник не меняется.**
   - Вход: `AssetRef` действующего ассета этой библиотеки (не из `trash/`; иначе `not_found`, как у `change`). Копия в другую библиотеку — межбиблиотечные операции P1 (ADR-0025 п. 9).
   - Источник читается снимком (`snapshot`) той ревизии, которая была на момент начала. Источник не блокируется на запись и не переписывается; блокируется новый AssetID.
   - Источник формата newer_minor → отказ `am.repo.write_unsupported_version` (как у `change`: писать неизвестные поля нельзя).
2. **Что получает копия** (R-07.2, R-07.3, SPEC §10):
   - `asset.id` — новый UUIDv4; `asset.revision = 1`; `created_at = modified_at` = время операции.
   - `history.entries` — ровно одна запись: `revision = 1`, новый `operation_id`, `operation = asset_created`, `details = {"creation_kind": "independent_copy", "source_asset": {"asset_id": <исходный>, "revision": <исходная ревизия>}}`, `tool` — как у `create`. `compacted_ranges` нет. Старые записи истории не переносятся.
   - `CreateRequest.creation_kind` по-прежнему принимает только `user`/`import`: `independent_copy` выставляет только этот сервис.
   - Метаданные, поля, группы, превью, `origin`/`inputs`, claims переносятся как текущее состояние с переназначенными ID.
3. **Новые ID — по отображению `old → new`** (R-07.4, R-07.5, ADR-0036 п. 4). Новый UUIDv4 получает каждый экземпляр:

   | Объект | Поле с ID | Ссылки на него, которые переназначаются |
   |---|---|---|
   | ассет | `asset.id` | `dependencyRef.owner_id` при `owner_scope = asset` |
   | файл | `files[].id` | `groups[].file_refs[]`, `previews.items[].file_id`, `files[].derived_from[].file_id`, `inputEntry.file_id`, `metadata.license.file_ref`, значения полей типа `file_ref`, `claims[].evidence[].file_id`, `dependencyRef.file_id`, `owner_id` при `owner_scope = file` |
   | группа | `groups[].id` | `groups[].parent_id`, `previews.items[].group_refs[]`, `dependencyRef.group_id`, `owner_id` при `owner_scope = group` |
   | превью | `previews.items[].id` | `previews.primary_id` |
   | источник | `origin` → `source.id` | `input.source_refs[]`, `claims[].evidence[].source_id` |
   | входной набор | `input.id` | префикс `storage_path` `content/<InputID>/` (п. 5) |
   | объект входа | `inputObject.id` | `inputObject.parent_object_id`, `inputEntry.object_id`, `claims[].evidence[].input_object_id` |
   | запись входа | `inputEntry.id` | `inputObject.parent_entry_id`, `claims[].evidence[].source_entry_id` |
   | автор | `metadata.authors[].id` | `claimTarget.item_id` |
   | claim | `claims[].id` (включая claims источников) | — |
   | операция | `history.entries[].operation_id` | новая запись п. 2 |

   - FieldID, ExtensionID, TypeID и `file.type.id` сохраняются: это идентификаторы контрактов, а не экземпляров (R-07.4).
   - SHA-256 в `derived_from`, `fileRefWithHash`, `dependencyRef.sha256` не пересчитываются: байты те же.
   - Список полей выше составлен по схеме 1.0 (`$defs/*` со ссылкой на `uuid4`). Если в коде или схеме найдётся UUID-поле, которого нет в таблице, — остановиться и описать, а не решать самостоятельно.
4. **Проверка полноты переназначения — обязательна и в рабочем коде, а не только в тестах.**
   - После построения нового манифеста он сериализуется, и в тексте ищется каждый старый UUID из отображения и старый AssetID.
   - Единственное разрешённое вхождение старого AssetID — `history.entries[0].details.source_asset.asset_id`.
   - Любое другое вхождение (неизвестная встроенная ссылка: строка в `generation.params`, в `metadata`, в непрозрачных данных) → отказ `am.repo.copy_remap_incomplete` с путём JSON первого найденного вхождения. Так закрывается R-07.7 для данных ядра: «доказать безопасное переназначение» = показать, что старых ID в копии не осталось.
5. **Пути хранения** (R-07.8, SPEC §1). У файлов с `storage_path` вида `content/<старый InputID>/<путь>` префикс заменяется на `content/<новый InputID>/`; остаток пути не меняется. Длина пути не меняется (UUID той же длины); ограничение 200 байт проверяется общим кодом. Пути в `generated/` не меняются. Записи ZIP переносятся **raw**: без распаковки и повторного сжатия, CRC-32 и SHA-256 прежние. Если писатель не умеет raw-копирование под другим именем — остановиться и описать.
6. **Отказы P0** (ADR-0039 п. 11, R-07.6–R-07.7). Каждый — `Errc::unsupported`, ничего не создаётся:
   - есть непустой `extensions` → `am.repo.copy_extensions_unsupported` (O-07: контракта clone/remap для плагинов нет);
   - есть определение поля с TypeID `plugin:…` и значения такого поля → `am.repo.copy_plugin_values_unsupported` (значение — JSON по контракту плагина, встроенные ссылки недоказуемы);
   - непустой `historical_files` → `am.repo.copy_history_unsupported` (O-05: удалённые файлы нельзя представить в истории ревизии 1);
   - `files[].derived_from[].file_id` указывает не на текущий файл → `am.repo.copy_history_unsupported` (O-06);
   - в ZIP источника есть незарегистрированные (непрозрачные) записи → `am.repo.copy_opaque_entries` (у новых контейнеров их быть не должно, молча удалять нельзя);
   - п. 4 нашёл старый ID → `am.repo.copy_remap_incomplete`.

   В ответе об отказе — подсказка-факт: «обычное копирование ZIP сохраняет ID» (как текст сообщения в каталоге, не логика).
7. **Запись — общий путь `create`.** Журнал `kind = create`, новый AssetID, база «файла нет». Полный V1–V4 нового ZIP перед фиксацией (ADR-0035 п. 7). Событие `on_indexed` с `kind = create` — тем же порядком, что у `create` (через `on_committed`, до `indexed`); замечание 1 приёмки T-020 здесь не повторять. Существующие точки отказа и kill-тесты create/change — без правок.
8. **`userdata` не копируется** (R-07.9): аннотации, коллекции, решения Dismiss остаются у источника. SHA-256 основного `userdata.sqlite` до и после совпадает.
9. **Уровни (ADR-0024).**
   - `storage`: `AssetRepository::copy_independent(const CopyRequest&)` — `CopyRequest{source: AssetRef, new_asset_id?: string (для тестов и повторяемости), op_id?: string}` → `CopyResult{ref (новый), source_revision, change: ChangeResult}`. Отображение ID строит `storage` (или чистая функция в `domain`, если по матрице ADR-0024 это удобнее; выбор описать).
   - `services`: `Catalog::copy_independent` под общим mutex, предупреждения индекса — как у `create`.
   - `application`: фасад; CLI — только через него.
10. **CLI.** `am library copy <dir> <asset-id> [--json] [--verbose]`.
    - JSON: `source: {asset_id, revision}`, `asset_id` (новый), `revision: 1`, `files`, `error`.
    - Код 0 — копия создана; 4 — отказ (п. 6) или ошибка проверки; 2 — аргументы.
    - Документация `docs/user/cli.md`, `CHANGELOG.md`.
11. **Попутно — замечание 3 приёмки T-020.** Если `import_info` не может прочитать `asset.id`/`revision`/`format.version.minor`, `ImportReadyResult.classification = "invalid"` (сейчас `unknown`). Других изменений `import_ready` нет; тест добавить.
12. **`message_id`:** `am.repo.copy_extensions_unsupported`, `am.repo.copy_plugin_values_unsupported`, `am.repo.copy_history_unsupported`, `am.repo.copy_opaque_entries`, `am.repo.copy_remap_incomplete`; английский текст в `resources/messages/en.toml`, в тексте отказов — что обычное копирование ZIP с прежними ID остаётся доступным.

## Тесты

Юнит-тесты — `RecordProperty("req", "K-3")` (формат 1.0, ADR-0036): отдельного критерия §15 для независимой копии нет. ID проверок R-07 — в имени теста (`D05`, `D06`, `D07`); тест 10 — `K-31`.

1. **D-05:** ассет A доведён до ревизии 12 → копия: новый AssetID, `revision = 1`, одна запись истории с `source_asset = {A, 12}`; A байт-в-байт прежний (SHA-256 файла), индекс содержит оба.
2. **D-06, полный набор ссылок:** синтетический манифест, где **каждая** строка таблицы п. 3 представлена хотя бы одной ссылкой (группы с `parent_id`, превью с `group_refs` и `primary_id`, `derived_from` на текущий файл, поле `file_ref`, `license.file_ref`, `origin`/`inputs` с объектами и записями, claims с evidence всех четырёх видов и `item_id`, зависимости `file`/`group`/`field_value` для трёх `owner_scope`). После копии: множества старых и новых ID не пересекаются, каждая ссылка указывает на образ своего прежнего адресата, полный V1–V4 пройден, SHA-256 всех потоков прежние, пути `content/<новый InputID>/…`.
3. **Полнота (п. 4):** старый FileID, вписанный строкой в `generation.params` или в текстовое поле метаданных → `am.repo.copy_remap_incomplete` с путём JSON; ничего не создано (ни ZIP, ни временного файла, ни строки журнала, ни строки индекса).
4. **D-07 и отказы п. 6:** `extensions`, plugin-поле со значением, `historical_files`, `derived_from` на удалённый файл, непрозрачная запись ZIP → свой `message_id`, источник и библиотека без изменений.
5. **Raw-копирование:** CRC-32 и сжатый размер каждой записи полезного файла в копии равны исходным (переименована только запись).
6. **Повторяемость:** две копии одного источника — разные AssetID, обе корректны; копия копии — `source_asset` указывает на первую копию.
7. **Ограничения источника:** источник в `trash/` → `not_found`; newer_minor → `am.repo.write_unsupported_version`; занятый `new_asset_id` → `am.repo.asset_exists`.
8. **Сбои:** точки `FaultPoint` фиксации копии с последующим `recover()` — либо копии нет, либо она полностью зарегистрирована с индексом; источник не изменён ни в одном исходе.
9. **`userdata`:** у источника есть строка в `userdata` → после копии SHA-256 основного файла прежний, у нового ID записей нет.
10. **T-020, замечание 3:** манифест без читаемой `asset.revision` → `classification = invalid`.
11. **CLI:** init → `asset create` + `add-ready` → `library copy` → 0, новый ID в `list`; копия ассета с `extensions` → 4 с `message_id`. Ограничения Linux — как у прежних CLI-сценариев записи, с явной причиной SKIP.

## Файлы (что можно менять)

- Изменяемые:
  - `include/am/storage/repository/repository.hpp`, `src/storage/repository/repository.cpp`;
  - при выборе п. 9 — новый файл отображения ID в `src/domain/…`/`include/am/domain/…` (только с проверкой `arch_check`);
  - `src/storage/asset_zip/*`, `include/am/storage/asset_zip/*` — только если для raw-копирования под новым именем не хватает существующего API; иначе не трогать;
  - `include/am/services/catalog/indexer.hpp`, `src/services/catalog/indexer.cpp` (`Catalog::copy_independent`);
  - фасад `library_service`, `src/application/cli/main.cpp`;
  - тесты: новый `tests/unit/test_independent_copy.cpp`, дополнения в `tests/unit/test_import_ready.cpp` (тест 10), `tests/cli/test_library_cli.py`, регистрация в `tests/CMakeLists.txt`; `tests/unit/zip_builder.*` — только добавлением;
  - `resources/messages/en.toml`, `docs/user/cli.md`, `CHANGELOG.md`;
  - этот файл (отчёт), строка T-021 в `docs/tasks/README.md`.
- **Не менять:**
  - `schemas/*` (включая `schemas/sql`), golden ZIP, кодек, правила V1–V4;
  - протокол фиксации и журнал (`src/storage/commit/*`, `state_journal.cpp`);
  - поведение `create`/`change`/`trash`/`restore`/`purge`/`import_ready` (кроме п. 11) и их точки отказа;
  - `Library`, ADR, спецификации, матрицы, roadmap, `vcpkg.json`.

## Критерии приёмки

1. `tools\build_min.cmd --ci`, `tools\build_cli.cmd --ci`, `tools\build_cli.cmd --offline --ci` — PASS; `tools/build_linux.sh cli --ci` в WSL — PASS. Число тестов не уменьшилось; `req_report` — 0 неверных ID; `arch_check` PASS.
2. Тесты 1–11 выполнены. В отчёте — имена тестов, таблица п. 3 с указанием, где в коде переназначается каждая строка, пример JSON `library copy` (успех и отказ), где живёт отображение ID и почему.
3. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-021`, без слияния, push и переписывания истории.

## Вне объёма

- Перенос исторической родословной (`historical_files`, `derived_from` на удалённые файлы) — O-05/O-06, P1+, новые необязательные поля минорной версии.
- Клонирование расширений и plugin-значений — O-07, после контракта C ABI (S6, M5P).
- Копия в другую библиотеку и выбор места — межбиблиотечные операции P1.
- Изменение метаданных копии в той же операции (название и т. п.) — последующий `change`.
- Связь «копия ↔ источник» в `userdata`/индексе и показ родословной в GUI.
