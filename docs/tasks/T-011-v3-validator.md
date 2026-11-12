# T-011. Валидатор V3 (семантика манифеста 1.0) и замечания приёмки T-010

**Статус:** ready (В-1 решён управляющим 2026-11-12, продолжать в той же ветке) · **Веха:** M1 (`am_domain`, `am_storage`; P0.4) · **Основание:** ТЗ §19.4.3–§19.4.7, §19.4.10–§19.4.16, §19.4.19; ADR-0024, ADR-0032, ADR-0035 п. 7, ADR-0036, ADR-0037, ADR-0038; [спецификация формата 1.0](../specifications/container_manifest_v1.md) §§4–12; [свод R-01…R-09](../tz/R01-R09_approved_handoff.md) (R-05, §7.2 M-01…M-10); [журнал решений v0.11](../tz/RC1_decisions_after_e2472e9_v0.11.md) (O-01, O-02, O-12, O-14, O-18, X-01, X-02); [`fs_support.md`](../specifications/fs_support.md) v0.2; T-006 и T-010 вместе с разделами «Приёмка»

## Цель

1. Реализовать **полную семантическую проверку V3** манифеста формата 1.0 на уровне `domain`. Это функция над `ManifestDocument`: она не читает ZIP и не знает о библиотеке.
2. Подключить её к `AssetRepository` (T-010) через существующий интерфейс `SemanticValidator`, чтобы:
   - отчёт проверки перестал быть `v3: partial` из-за неполноты правил;
   - фиксация выполнялась только при пройденной V3 (ADR-0035 п. 7).
3. Устранить четыре замечания приёмки T-010.

Пользовательские команды записи (CLI) в эту задачу **не входят**: это следующая задача. После T-011 для их открытия больше нет препятствий со стороны проверки.

## Решения управляющего (не пересматривать в задаче)

1. **Размещение.**
   - V3 — в `include/am/domain/manifest/semantics.hpp`, `src/domain/manifest/semantics.cpp`. Допускается разбить на несколько `.cpp` в `src/domain/manifest/`: значения и типы, SPDX, время, история.
   - Зависимости — только `foundation`, `contracts` и уже подключённые к `am_domain` библиотеки. Новых внешних зависимостей нет.
   - Правила, которым нужен контекст хранилища, остаются в `storage::repository`: совпадение `asset.id` со ссылкой и именем ZIP, пути записей ZIP, `operation_id` последней записи равен `op_id` журнала, незарегистрированные потоки, повтор `op_id` (п. 12).

2. **API.** Сигнатуры — на усмотрение исполнителя, состав обязателен:
   - `validate_semantics(const ManifestDocument&, const SemanticOptions&) -> Result<SemanticReport>`; `Result` с ошибкой — только при внутренней ошибке или нехватке памяти, нарушения возвращаются в отчёте;
   - `SemanticReport`:
     - `complete` — все правила каталога выполнены;
     - `checked_rules` — идентификаторы выполненных правил;
     - `deferred_rules` — отложенные правила с причиной (п. 9);
     - `findings`;
     - признак наличия plugin-данных;
   - `Finding`:
     - `rule` — идентификатор из каталога;
     - `severity`: `error`, `warning` или `unverifiable`;
     - `pointer` — JSON Pointer места, только для диагностики; в формат он не попадает;
     - `message_id` — `am.manifest.<rule>`;
     - `detail` — английский технический текст для журнала, не для пользователя (ADR-0032);
   - `SemanticOptions` — лимиты (O-17, значения по умолчанию помечены `hypothesis`) и флаг прекращения проверки после N находок.

3. **Свод уровня V3.**
   - Есть находка `error` → `failed`.
   - Есть `unverifiable` и нет `error` → `partial`.
   - Только `warning` или ничего → `passed`.
   - `complete = false` всегда даёт не выше `partial`.

   В `VerificationReport` (T-010) добавить:
   - `v3_findings` вместо плоского `violations` либо рядом с ним (решает исполнитель, но в JSON отчёта находки идут с `severity`, `rule` и `pointer`);
   - `v3_complete`;
   - `v3_deferred_rules`.

4. **Правило фиксации** (ADR-0035 п. 7). Перед заменой ZIP `AssetRepository` требует одновременно:
   - V3 без находок `error`;
   - `complete = true`.

   `unverifiable` и `warning` фиксацию не блокируют, но попадают в отчёт. Это та же логика, что у V4: `verified_crc_size_only` — не ошибка. Отказ возвращает `message_id` первой находки `error` (`Errc::corrupt_data`) и фактический отчёт через выходной параметр.

5. **Значения по умолчанию.**
   - `RepositoryOptions::semantic_validator` по умолчанию становится полным валидатором: V3 уровня domain плюс правила хранилища.
   - `repository_invariants()` остаётся для тестов и не заменяет полный валидатор.
   - Правила T-010 уровня манифеста (уникальность ID, пути и входы, покрытие истории) переезжают в domain; повторно в хранилище не проверяются, каждое нарушение сообщается один раз.

6. **Plugin-данные** (R-09.4, R-09.7, M-08):
   - значения plugin-типов, `data` расширений, `constraints`/`default` plugin-типов V3 не интерпретирует;
   - при наличии plugin-данных `plugin_validation = not_performed`, и это не находка V3;
   - V3 проверяет вокруг них только нормативное: разрешимость FieldID и `dependencies`, область применения.

7. **Числа.**
   - Сравнения `integer`/`decimal` с границами — точные, по десятичной строке, без `double`. `1.500 == 1.5`, `-0 == 0`.
   - `int64` вне диапазона — `error`.
   - Хранимые строки V3 не изменяет: функция только читает документ.

8. **Время.**
   - `timestamp` (RFC 3339 с `Z`) и `date` (`YYYY-MM-DD`) проверяются на календарную допустимость: високосные годы, длина месяца, часы 00–23, минуты 00–59, секунды 00–60 (RFC 3339 допускает 60).
   - Правило применяется ко всем полям этих типов в манифесте: `created_at`, `modified_at`, `imported_at`, `retrieved_at`, `published_at`, `review.at`, `claims[].created_at`, `history.entries[].timestamp`, `dates.*`, значения и границы полей `date`/`datetime`.
   - Упорядоченность времён **не** проверяется: авторитетный порядок — ревизия (§4).

9. **Открытые поддетали не закрываются допущениями** (CLAUDE.md). Соответствующие места реализуются так:
   - **O-08** (алгоритм поискового ключа тегов): предупреждение о совпадающем поисковом ключе **не реализуется**. Правило `tags_search_key` — в `deferred_rules` с причиной `O-08`. На `complete` оно не влияет, так как даёт только предупреждение. Точный дубль тега проверяет V2 (`uniqueItems`).
   - **UCUM** (X-02, движок открыт): единицы сравниваются только как точные строки. Граница и значение с **одинаковой** строкой единицы сравниваются числом. С **разными** — находка `unverifiable` (`x02_unit_conversion_open`), без пересчёта. Неизвестных для ядра единиц нет: любая непустая строка (V2) сохраняется.
   - **`dimension`** (грамматика открыта): физическая величина — поле `integer`/`decimal` с `dimension`, отличной от `dimensionless`. Отсутствие `dimension` — нефизическая величина.
   - **Физический `default`** (открыт): для физического числового поля `default` проверяется только как представление типа. Сравнение с границами не выполняется: находка `unverifiable` (`x02_physical_default_open`).
   - **Единица значения в claim.** В `claims[]` формата 1.0 нет поля `unit`, поэтому значение `alternative`/`rejected` для физического числового поля нельзя сопоставить с единицей. Проверяется только представление типа, плюс находка `unverifiable` (`claim_value_unit_unexpressible`). Это пробел формата: вынесен владельцу (раздел «Вне объёма»), в задаче не решается.
   - **Словарь `changes[].kind` и plugin-`operation`** (O-18, открыты): цели `changes[]` не разрешаются, неизвестная `operation` — не ошибка.
   - **`included_renamed`/`excluded` для каталогов** (X-01, открыт) и **грамматика `generation.tool`** (O-14, открыт) не проверяются сверх V2.

10. **SPDX.**
    - `license.spdx` проверяется по грамматике SPDX License Expression, SPDX 2.3, Annex D:
      - `idstring = 1*(ALPHA / DIGIT / "-" / ".")`;
      - `license-id` и `license-id+`;
      - `LicenseRef-<idstring>` и `DocumentRef-<idstring>:LicenseRef-<idstring>`;
      - `<simple> WITH <exception-id>`;
      - `AND`, `OR`, скобки;
      - приоритет `WITH` > `AND` > `OR`;
      - операторы — только в верхнем регистре, пробелы между токенами.
    - Принадлежность идентификатора к списку лицензий SPDX **не** проверяется: в проекте нет данных списка и правила их обновления. Это не отложенное правило: спецификация требует только грамматику (O-14 п. 3).
    - Собственный разбор, без библиотеки. Управляющий вносит его в `libraries.md`.

11. **Каталог правил V3.** Каталог ниже **закрыт**: исполнитель реализует ровно его.
    - Каждое правило — отдельный идентификатор `rule` с тестом «нарушение → находка» и проверкой, что эталон `schemas/examples/manifest-1.0.example.json` его проходит.
    - Если правило противоречит схеме или эталону, или в источниках (спецификация §§4–12, R-05/§7.2, журнал v0.11) найдено нормативное правило V3, которого нет в каталоге, — остановка по п. 3 `AGENTS.md`, без самостоятельного расширения.

12. **Замечания приёмки T-010:**
    1. **Коды ошибок нижних слоёв.** `guard` в `repository.cpp` переводит их по таблице. `Errc` сохраняется, исходная ошибка — в `cause`.

       | Исходный `message_id` / условие | Новый `message_id` |
       |---|---|
       | `am.commit.locked` | `am.repo.locked` |
       | `am.commit.insufficient_space` | `am.repo.insufficient_space` |
       | `am.commit.replace_failed` | `am.repo.target_in_use` |
       | `am.commit.journal_error`, `am.commit.journal_version` | `am.repo.journal_error` |
       | `am.commit.out_of_memory`, `am.zip.*` с `Errc::resource_exhausted` | `am.repo.limit_exceeded` |
       | `am.zip.*` с `Errc::corrupt_data` | `am.repo.corrupt_container` |
       | `am.zip.*` с `Errc::unsupported` | `am.repo.unsupported_container` |
       | прочие `am.zip.*` и `am.commit.*`, кроме `base_changed` и `verification_failed` (п. 4 T-010) | `am.repo.io_error` |

    2. **Повтор `op_id`.**
       - V3 domain проверяет уникальность `history.entries[].operation_id` (правило `history_operation_id_unique`).
       - `AssetRepository::change` и `create` до записи отклоняют `op_id`, который уже есть в `history.entries[].operation_id` текущего снимка: `Errc::conflict`, `am.repo.duplicate_operation_id`, без журнала и временного файла. Идемпотентный ответ не вводится.
    3. **Съёмные NTFS-тома.** `detect_storage_capabilities` для `DRIVE_REMOVABLE` возвращает `read_only_default`, `durability = unknown`. Строка внесена в `fs_support.md` v0.2. `full` остаётся только у `DRIVE_FIXED` с NTFS. Испытывался только встроенный диск, а `DRIVE_RAMDISK` никто не испытывал, поэтому он тоже переходит в `read_only_default`.
    4. **`AddFiles`.** Безопасность пути, длина и коллизии (с текущими путями и между файлами запроса) проверяются **до** хеширования источников.

## Каталог правил V3

Идентификатор правила — `rule`; `message_id` = `am.manifest.<rule>`. Если не указано иное, тяжесть — `error`. Ссылки «→» означают «разрешается в».

### A. Идентичность и уникальность

| `rule` | Проверка |
|---|---|
| `unique_ids` | Уникальны в пределах манифеста: FileID в `files ∪ historical_files` (и `files ∩ historical_files = ∅`), GroupID, PreviewItemID, InputID, InputObjectID, SourceEntryID, SourceID, AttachmentID, ClaimID (включая claims источников), FieldID. Уникальны внутри владельца: OptionID в поле; `authors[].id` в одном массиве `authors`; `field_id` в `custom_values` одной `metadata`; `index` томов в объекте |
| `storage_paths` | `files[].storage_path` и `attachments[].path` уникальны по `path_collision_key` (регистр и NFC), безопасны (`paths::check_container_path`), не длиннее 200 байт; путь `content/<X>/…` → `X` — существующий InputID; вложение лежит в `extensions/<свой ExtensionID>/` |
| `history_operation_id_unique` | `history.entries[].operation_id` уникальны |

### B. Файлы и происхождение (§5, §6, X-01)

| `rule` | Проверка |
|---|---|
| `historical_files` | `removed_at_revision` ∈ [2, `asset.revision`] |
| `derived_from` | `file_id` → `files ∪ historical_files`. Несовпадение `sha256` или исторический FileID — **не** находка: это вычисляемое состояние `stale`/`source_missing` (§6) |
| `input_source_refs` | `inputs[].source_refs[]` → `origin.sources[].id` |
| `input_entry_object` | `entries[].object_id` → объект **того же** Input |
| `input_entry_file` | `file_id` есть только у `entry_kind = file` с `result` ∈ {`included`, `included_renamed`}; у таких записей он обязателен; → `files ∪ historical_files` |
| `input_nesting` | `parent_object_id` и `parent_entry_id` заданы вместе; родительский объект — в том же Input; родительская запись принадлежит родительскому объекту и имеет `entry_kind = file`; циклов вложенности нет; у объекта с `nested_action = exclude` родительская запись не имеет `file_id` |

### C. Группы и превью (§7)

| `rule` | Проверка |
|---|---|
| `group_refs` | `parent_id` → GroupID; циклов нет; `file_refs[]` → текущий FileID (`files`) |
| `preview_refs` | `items[].file_id` → текущий FileID; `group_refs[]` → GroupID; `primary_id` → `items[].id` |

### D. Поля и значения (§8, §9.2, X-02, R-09)

| `rule` | Проверка |
|---|---|
| `field_options` | `enum`/`multi_enum`: OptionID уникальны (дубль внутри поля — здесь, если V2 его не ловит) |
| `field_constraints_unit` | `integer`/`decimal` с физической `dimension`: у каждой границы `minimum`/`maximum` есть `unit` (X-02 п. 7) |
| `field_default` | `default` базового типа проходит те же проверки представления, вариантов и ограничений, что и значение (`value_*`), с учётом п. 9 «Решений» для физического поля |
| `custom_value_field` | `custom_values[].field_id` → `field_definitions` (M-04/M-07); область владельца (`asset`, `group`, `file`) входит в `scopes` поля |
| `value_representation` | Значение соответствует базовому типу: `integer` — строка int64; `decimal` — строка без экспоненты; `boolean` — JSON-логическое; `string`/`text` — строка; `date`, `datetime` — п. 8; `enum` — строка; `multi_enum` — массив строк без повторов; `url` — абсолютный URI (как `$defs/url` схемы); `file_ref` — строка |
| `value_options` | `enum`/`multi_enum` → OptionID поля; `deprecated` не делает значение невалидным |
| `value_file_ref` | `file_ref` → текущий FileID |
| `value_constraints` | `min`/`max` для `integer`/`decimal` (п. 7, единицы — п. 9), `min_length`/`max_length` в кодовых точках без нормализации, `minimum`/`maximum` для `date`/`datetime`, `min_items`/`max_items` по уникальным OptionID |
| `value_unit_required` | `integer`/`decimal` с физической `dimension`: у значения есть `unit` (X-02 п. 4) |
| `value_dependencies_scope` | `custom_values[].dependencies` есть только у значений plugin-типов (§9.2) |
| `x02_unit_conversion_open` | `unverifiable`: граница и значение с разными строками единиц (п. 9) |
| `x02_physical_default_open` | `unverifiable`: `default` физического числового поля не сравнивается с границами (п. 9) |

### E. Метаданные и claims (§9.1, §9.3, R-05, O-01, O-02)

| `rule` | Проверка |
|---|---|
| `claim_target_standard` | `standard`: при `supports_selected` поле присутствует в той же `metadata` (M-01). Для claims источника — в объекте источника |
| `claim_target_item` | `standard_item`: `item_id` → `authors[].id` той же `metadata` (M-05) |
| `claim_target_custom` | `custom`: `field_id` → `field_definitions`, область владельца входит в `scopes`; при `supports_selected` — значение с этим `field_id` есть в той же `metadata` |
| `claim_value_type` | `alternative`/`rejected` с `custom` базового типа: `value` проходит `value_representation` и `value_options` поля. `value_constraints` к claim **не** применяется: отклонённое или альтернативное значение может нарушать текущие границы |
| `claim_value_unit_unexpressible` | `unverifiable`: то же для физического числового поля (п. 9) |
| `claim_evidence` | `evidence[]`: `file_id` → `files ∪ historical_files`; `source_entry_id` → SourceEntryID любого Input; `input_object_id` → InputObjectID; `source_id` → SourceID |
| `license_file_ref` | `license.file_ref` → текущий FileID |
| `license_spdx` | `license.spdx` проходит грамматику п. 10 |
| `tags_search_key` | `warning` — **отложено** (O-08, п. 9) |

Типы `value` для стандартных полей, сочетания `relation`/`value`/`review`/`target.kind` и реестры адресатов проверяет V2 (схема, `$defs.claim`/`sourceClaim`). V3 их не дублирует. Если тест покажет, что какое-то из этих сочетаний V2 пропускает, — остановка по п. 3 `AGENTS.md`.

### F. Расширения и зависимости (§10, O-12)

| `rule` | Проверка |
|---|---|
| `dependency_refs` | Для `extensions.*.dependencies` и `custom_values[].dependencies`: `file` → `files ∪ historical_files` (исторический FileID или несовпадение `sha256` — вычисляемое `stale`/`source_missing`, **не** находка); `group` → GroupID; `field_value{owner_scope, owner_id, field_id}` → существующее значение `custom_values` с этим `field_id` у владельца `owner_scope`/`owner_id` (`asset` — владелец ассета) |

### G. Время и история (§4, §11, O-14, O-18)

| `rule` | Проверка |
|---|---|
| `timestamps` | Календарная допустимость всех `timestamp`/`date` (п. 8) |
| `asset_times` | `asset.created_at` = `timestamp` записи ревизии 1; `asset.modified_at` = `timestamp` последней записи |
| `history_coverage` | Ревизии `1..asset.revision` покрыты ровно один раз записями `entries[]` и диапазонами `compacted_ranges[]` без пропусков и пересечений; оба массива упорядочены по начальной ревизии; ревизия 1 — отдельная запись `asset_created`, последняя — отдельная запись с `revision = asset.revision`; ни одна из них не входит в диапазон. Без развёртывания диапазонов в память (O-18 п. 1) |
| `history_compacted_summary` | `summary.revision_count = to_revision − from_revision + 1`; `operation_counts`, если есть, — неотрицательные счётчики, сумма равна `revision_count`; среди ключей нет защищённых операций (`asset_created`, `format_migrated`, `provenance_changed`, `history_compacted`) |

## Требования

### 1. Domain (`include/am/domain/manifest/semantics.hpp`)

- API по п. 2 «Решений». Функция потокобезопасна для разных документов и не изменяет документ.
- Значения, скрытые кодеком как непрозрачные, V3 не раскрывает и не разбирает. Их наличие и FieldID проверяются по известной части DOM.
- Пределы:
  - число находок ограничено опцией, после неё — одна сводная находка `limit_exceeded` (`Errc::resource_exhausted` не возвращается);
  - глубина групп и вложенности объектов — без рекурсии по стеку либо с ограничением (O-17).
- Сложность — не хуже O(n log n) по числу объектов: индексы ID строятся один раз.

### 2. Storage (`AssetRepository`)

- Полный валидатор по п. 5, правило фиксации по п. 4, свод V3 и поля отчёта по п. 3.
- Отчёт `verify` для ассета, созданного до T-011 и не проходящего полную V3, возвращается без исключения: `v3: failed` с находками. Снимок и чтение такой ассет не блокирует: V3 не входит в условие снимка (В-2 T-010). Запись блокирует.
- Замечания пп. 12.1–12.4.

### 3. Попутно

- `docs/specifications/container_manifest_v1.md` §12 и ADR **не менять**. Каталог правил живёт в этом файле задачи. Расхождение со спецификацией — вопрос по п. 3 `AGENTS.md`.

## Контекст (что прочитать)

- Спецификация §§4–12; эталон `schemas/examples/manifest-1.0.example.json`; схема `schemas/manifest-1.0.draft.schema.json` (`$defs.claim`, `claimCore`, `sourceClaim`, `metadata`, `fieldDefinition`, `dependencies`).
- `include/am/domain/manifest/codec.hpp`: что кодек считает непрозрачным (`custom_values` и `claims[].value` полей plugin-типа или неизвестного поля, `generation.params`, `data` расширений, `default`/`constraints` plugin-полей, `details` и дополнительные ключи `changes[]`).
- `src/storage/repository/repository.cpp`: класс `Invariants`, `check`, `guard`.
- T-010: раздел «Приёмка» (замечания 1–4).

## Тесты

1. **Эталон и созданные репозиторием ассеты.**
   - `manifest-1.0.example.json` проходит V3 без `error` и `unverifiable`: `passed`, `complete`.
   - Ассет, созданный и изменённый всеми операциями T-010, проходит V3 перед фиксацией. Тестовые фикстуры T-010 можно доработать, ожидания проверок T-009/T-010 — не ослаблять.
2. **Каждое правило каталога:** минимальная мутация эталона → ровно ожидаемая находка с верными `rule`, `severity`, `message_id` и `pointer`; без посторонних находок. Сценарии M-01…M-08 из §7.2 свода R-01…R-09 — отдельными именованными тестами.
3. **Числа:** `1.500` против `maximum 1.5` — проходит; `1.5000001` — `error`; int64 на границах ±2⁶³ − 1/−2⁶³ и за ними; `-0`; отрицательные decimal.
4. **Единицы:** физическая величина без `unit` у значения — `error`; граница без `unit` — `error`; одинаковые строки — сравнение; `mm` против `cm` — `unverifiable`, без пересчёта; безразмерная — без требования единицы.
5. **Время:** 29 февраля в високосный и обычный год, 2100 год, `24:00:00`, `:60`, 31 апреля, дробные секунды, сравнение `datetime` с границами.
6. **Строки:** длина в кодовых точках (комбинирующие символы, суррогатные пары, NFC/NFD не нормализуются).
7. **SPDX:** положительные — `MIT`, `GPL-2.0-or-later`, `Apache-2.0 WITH LLVM-exception`, `(MIT OR Apache-2.0) AND BSD-3-Clause`, `LicenseRef-my.custom-1`, `DocumentRef-x:LicenseRef-y`, `GPL-2.0+`; отрицательные — пустая, `mit or apache-2.0`, незакрытая скобка, `WITH` без исключения, двойной оператор, `LicenseRef-` без idstring, недопустимый символ.
8. **История:** пропуск, пересечение, диапазон с ревизией 1 или последней, неверный `revision_count`, сумма `operation_counts`, защищённая операция в счётчиках, повтор `operation_id`; диапазон `1..2⁵³` проверяется без выделения памяти по размеру диапазона.
9. **Plugin:** значение plugin-типа без обработчика — V3 `passed`, `plugin_validation = not_performed` (M-08); неопределённый FieldID — `error`; `dependencies` у значения базового типа — `error`.
10. **Репозиторий:**
    - фиксация с V3 `error` отклоняется, файл не изменён, отчёт с находками получен через выходной параметр;
    - фиксация с только `unverifiable` проходит, отчёт `v3: partial`;
    - `verify` старого ассета, не проходящего V3, — отчёт без исключения;
    - повтор `op_id` — `am.repo.duplicate_operation_id` без журнала и temp.
11. **Коды ошибок (п. 12.1):** `locked` (помощник держит блокировку), нехватка места (подстановка T-009), повреждённый ZIP, неподдерживаемый метод — новые `message_id` при прежнем `Errc` и наличии `cause`.
12. **Возможности ФС (п. 12.3):** классификация по типу диска и имени ФС — модульным тестом чистой функции (тип диска и имя ФС на входе), без реального съёмного носителя.
13. **`AddFiles` (п. 12.4):** недопустимый путь и коллизия отклоняются до чтения источника: источник — несуществующий файл или файл, открытие которого учитывается подстановкой.
14. **Производительность (замер, без норматива, O-17):** V3 манифеста с 100 000 файлов, 10 000 групп и 100 000 claims — время и пик памяти в отчёте.

Пометки `RecordProperty("req", …)`: `R-05`, `M-01`…`M-08`, `O-01`, `O-02`, `O-12`, `O-14`, `O-18`, `X-01`, `X-02` — по смыслу теста.

## Файлы (что можно менять)

- Новые: `include/am/domain/manifest/semantics.hpp`, `src/domain/manifest/*.cpp` (V3), `tests/unit/test_manifest_semantics*.cpp`, фикстуры `tests/fixtures/manifest/` (синтетика).
- Изменяемые: `src/domain/CMakeLists.txt`, `include/am/storage/repository/repository.hpp`, `src/storage/repository/repository.cpp`, `tests/unit/test_asset_repository.cpp`, `tests/integration/asset_repository_crash_helper.cpp`, `tests/CMakeLists.txt`, этот файл (отчёт), строка T-011 в `docs/tasks/README.md` (статус).
- **Не менять:** схему, эталон, спецификацию, ADR, `fs_support.md`, `libraries.md`, матрицы, код T-006…T-009, кодек.

## Критерии приёмки

1. `tools\build_min.cmd` — PASS, без новых внешних зависимостей минимальной сборки.
2. `tools\build_cli.cmd` и `tools\build_cli.cmd --offline` — PASS, все прежние тесты сохранены, ожидания не ослаблены (кроме перехода `v3: partial` → `passed` там, где теперь работает полный валидатор).
3. Тесты 1–14 выполнены; каждое правило каталога покрыто положительным и отрицательным случаем.
4. `arch_check` PASS: `am_domain` не зависит от `storage`; публичные заголовки без Windows, sqlite, zlib.
5. В отчёте:
   - таблица «правило каталога → тесты»;
   - примеры JSON `VerificationReport` с находками всех трёх тяжестей;
   - замер теста 14;
   - перечень отложенных правил с причинами.
6. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-011`, без слияния и переписывания истории.

## Вне объёма

- CLI `am asset …` и `am library …` — следующая задача.
- Принадлежность к списку лицензий SPDX, движок UCUM и пересчёт единиц, поисковый ключ тегов (O-08), грамматика `dimension`, физический `default`, словарь `changes[].kind`.
- **Единица значения в claim** для физических полей — пробел формата 1.0. Управляющий выносит его владельцу вместе с открытыми вопросами X-02.
- Компактизация истории (вне P0; проверяется только структура).
- Независимая копия (R-07) и транзакционные запреты удаления зависимых объектов, кроме уже реализованных в T-010.

## Отчёт исполнителя

**2026-11-12. Ветка:** `codex/T-011`, создана от `main` на `a7975da` после приёмки T-010. Отчёт и статус — коммит `T-011: report missing uint64 size rule in closed V3 catalog`.

**Реализация остановлена при предварительной сверке каталога со схемой и спецификацией.** Статус review означает передачу вопроса управляющему, а не готовность V3 к приёмке. Исходники, API, CMake, тесты проекта и замечания T-010 пока не менялись.

Основание — п. 11 «Решений» этой задачи: «Если правило противоречит схеме или эталону, или в источниках (…) найдено нормативное правило V3, которого нет в каталоге, — остановка по п. 3 AGENTS.md, без самостоятельного расширения». П. 3 [`AGENTS.md`](../../AGENTS.md) также запрещает самостоятельно выходить за рамки задачи и принимать архитектурные решения.

### Пропущенная проверка диапазона размеров uint64

1. Спецификация `container_manifest_v1.md` §2 «Числа» требует: размеры в байтах — канонические десятичные строки, **не больше 18446744073709551615**. §6 задаёт `files[].size_bytes` как uint64; другие поля размеров ссылаются на тот же тип схемы.
2. `$defs.uint64` исходной схемы проверяет только форму и длину: `^(0|[1-9][0-9]{0,19})$`. Описание прямо указывает: `range 0..18446744073709551615 is checked by V3 (pattern only limits length)`.
3. Строка `18446744073709551616` — 2⁶⁴, то есть первый выход за диапазон. В ней 20 цифр, поэтому она проходит указанную V2-проверку.
4. В закрытом каталоге T-011 нет правила диапазона uint64 для размеров. `value_representation` относится к значениям пользовательских полей базовых типов, включая **int64** для integer; это другой тип и другая область данных. `storage_paths`, `historical_files` и правила inputs также не включают размерные строки.
5. Это проверка над известной частью манифеста, не требующая чтения ZIP; она относится к domain V3. Отложить её на V4 нельзя считать выполнением контракта domain: V4 вообще не входит в validate_semantics. Без дополнения каталога нельзя одновременно реализовать ровно каталог и заявить полную нормативную V3.

Области, использующие `$defs.uint64`:

- `files[].size_bytes`;
- `historical_files[].last_known_size_bytes`;
- `inputs[].objects[].size_bytes`;
- `inputs[].objects[].volumes[].size_bytes`;
- `inputs[].entries[].size_bytes`;
- `extensions.*.attachments[].size_bytes`.

### Воспроизведение на реальном кодеке и V2

Команда: `.cache\t011_contract_proof.cmd`. Диагностика `.cache/t011_contract_proof.cpp` собрана MSVC 14.44 с существующими `build/win-cli/lib/am_domain.lib` и `am_foundation.lib`. Она использует исходные схему и эталон, реальные parse_manifest и SchemaValidator::validate. Мутации выполняются только в памяти; схема и эталон на диске не меняются.

Фактический вывод (`.cache/t011-contract-proof.log`):

```text
v2_example=PASS
v2_uint64_max=PASS
v2_files_size_overflow=PASS
v2_input_object_size_overflow=PASS
v2_input_entry_size_overflow=PASS
v2_historical_size_overflow=PASS
v2_attachment_size_overflow=PASS
uint64_overflow_confirmed=true
```

Проверено пять видов полей размера; область volumes дополнительно установлена по ссылке `$ref` в схеме. Граничное значение uint64 и исходный эталон проходят V2. Выход за диапазон независимо подтверждён `std::from_chars` в `std::uint64_t`: `std::errc::result_out_of_range`. Это диагностика границы ответственности V2/V3, не тест ещё не реализованного validate_semantics.

### Критерии приёмки и фактические проверки

| Критерий | Состояние |
|---|---|
| 1. Минимальная сборка | На исходном коде PASS, 84/84; предупреждений MSVC/CMake нет. |
| 2. CLI и offline | Для T-011 не запускались: реализация V3 ещё не начата. Прогоны приёмки T-010 не выдаются за проверку T-011. |
| 3. Тесты 1–14, каталог | Не реализованы и не запускались. Выполнена только диагностика выше. |
| 4. Архитектура и заголовки | Новых API/исходников нет; исходный arch_check прошёл в минимальной сборке. |
| 5. Таблица правил, VerificationReport, производительность, deferred_rules | Результатов реализации пока нет; требуется решение В-1 по составу каталога. |
| 6. Сдача | Ветка, причина остановки, команды и вопрос приведены здесь; собственные изменения ограничены документацией задачи. |

```text
tools\build_min.cmd
100% tests passed, 0 tests failed out of 84
Total Test time (real) =  14.69 sec
```

Лог — `.cache/t011-min.log`. arch_check, unicode_tables_check, schema_check и sql_check — PASS. Это проверка исходной базы, не выполненного T-011. Зависимости не скачивались.

### Отклонения

Вместо реализации передан вопрос согласно прямому требованию п. 11 задачи и п. 3 AGENTS.md. Закрытый каталог, схема, эталон, спецификации, ADR, fs_support.md, libraries.md, матрицы и код T-006…T-010 не менялись. Замечания приёмки T-010 остаются к выполнению после уточнения задания. В main не сливал, push и переписывание истории не выполнял. Посторонний `3D_am_open_questions_e2472e9.zip` не изменён и не включён в коммит.

### Вопросы

**В-1. Каким правилом закрытого каталога проверять диапазон uint64 у размеров в байтах?**

Нужно дополнить каталог идентификатором/областью проверки либо явно определить другой слой, который гарантированно отклоняет такие данные до объявления V3 complete. Рекомендую отдельное правило (например, `byte_sizes_uint64`) тяжести error с указателем на конкретное поле, для всех перечисленных ссылок на `$defs.uint64`. Положительные границы — `0` и `18446744073709551615`, отрицательная — `18446744073709551616` и другие 20-значные строки выше максимума. Проверка точная, без double и без изменения строк.

Идентификатор и расширение закрытого каталога самостоятельно не назначал. Продолжить можно в той же ветке после решения В-1; сверка каталога остановлена на этом противоречии, полнота оставшейся проверки пока не заявляется.

## Решение управляющего по В-1

**2026-11-12.** Вопрос обоснован, остановка по п. 11 правильная: `$defs.uint64` прямо относит проверку диапазона к V3, а в каталоге правила не было. При ответе я сверил с каталогом **все** описания схемы, которые ссылаются на V3, и нашёл ещё два пропуска того же рода. Они закрываются здесь же, чтобы не останавливаться на них отдельно. Каталог дополняется тремя пунктами и остаётся закрытым.

### 1. Новое правило `byte_sizes_uint64` (группа A)

- **Тяжесть** `error`, `message_id` = `am.manifest.byte_sizes_uint64`, указатель — на конкретное поле.
- **Область:** все поля `$defs.uint64`:
  - `files[].size_bytes`;
  - `historical_files[].last_known_size_bytes`;
  - `inputs[].objects[].size_bytes`;
  - `inputs[].objects[].volumes[].size_bytes`;
  - `inputs[].entries[].size_bytes`;
  - `extensions.*.attachments[].size_bytes`.
- **Проверка:** значение ≤ `18446744073709551615`. Сравнение точное, по строке или `from_chars` в `uint64_t`, без `double` и без изменения строки. Форму (каноничность, длину) проверяет V2.
- **Тесты:** проходят `0` и `18446744073709551615`; находка `error` — `18446744073709551616`, `99999999999999999999` в каждой из шести областей.

### 2. Новое правило `urls` (группа E)

`$defs.url` проверяет в V2 только наличие схемы и отсутствие пробелов; полный разбор по описанию схемы — V3. В каталоге было только `value_representation` для значений `url` пользовательских полей, и то по образцу V2.

- **Тяжесть** `error`, `message_id` = `am.manifest.urls`.
- **Область:** все поля `$defs.url`, то есть `origin.sources[].url`, `publisher.url`, `license.url`, `links[].url`, а также значения, `default` и значения claims пользовательских полей типа `url`. Для последних `value_representation` проверяет только, что это строка, а разбор выполняет это правило.
- **Проверка** — синтаксис RFC 3986 `URI` (§3, Appendix A):
  - `scheme ":" hier-part [ "?" query ] [ "#" fragment ]`;
  - `scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`;
  - authority с userinfo, host (reg-name, IPv4, IP-literal `[…]` с IPv6/IPvFuture) и port;
  - корректные `%HH`;
  - фрагмент разрешён.
- **Одно отступление от RFC 3986** (решение управляющего): байты ≥ 0x80, образующие корректный UTF-8, принимаются там, где допустим `unreserved`. Это совместимо с IRI (RFC 3987 `ucschar`): в описания попадают ссылки с нелатинскими путями, и отвергать их как повреждение манифеста неверно. Управляющие символы, пробелы и некорректный UTF-8 — `error`.
- Сетевых проверок, нормализации и изменения строки нет. Регистр схемы не нормализуется.
- **Тесты:**
  - проходят `https://example.com/a?b=c#d`, `mailto:a@b.c`, `urn:isbn:0451450523`, `http://[::1]:8080/`, `https://пример.рф/путь`, `https://example.com/%D0%BF`;
  - находка `error` — `http://exa mple.com` (если пробел дошёл до V3 через значение поля), `https://ex%ZZ`, `1http://x`, `http://[::1`, `http://host:port/`, строка с `\u0001`, некорректный UTF-8.

### 3. Уточнение `field_constraints_unit` → `field_constraints` (группа D)

`$defs.int64String` относит диапазон int64 к V3. В каталоге он был только у значений (`value_representation`), а не у границ `constraints`.

- Правило переименовано в `field_constraints`; `message_id` — `am.manifest.field_constraints`.
- Дополнительно к требованию единицы у физических границ проверяет **представление** границ по типу поля:
  - `integer` — int64 в диапазоне;
  - `decimal` — без экспоненты;
  - `date`/`datetime` — календарная допустимость, которую сообщает `timestamps` (не дублировать, указатель на границу).
- Противоречивость `minimum > maximum` **не** проверяется: в источниках такого правила нет.

### Остальное

- Сверка описаний схемы с каталогом показала, что прочие отсылки к V3 уже покрыты: `storagePath` → `storage_paths`; `date` → `timestamps`; `asset` → `asset_times`; `volumes.index` → `unique_ids`; `default` → `field_default`; `authors[].id` → `claim_target_item`; `spdx` → `license_spdx`; `tags` → `tags_search_key` (отложено); `custom_values.unit` → `value_unit_required`; `claimTarget` → группа E; `compactedRangeSummary` → `history_compacted_summary`; `history` → `history_coverage`.
- Если при реализации обнаружится ещё одно нормативное правило V3 вне каталога — по-прежнему остановка по п. 11.
- Продолжать в ветке `codex/T-011`. Новый отчёт — разделом «## Отчёт исполнителя (после В-1)» в конце файла, первый отчёт не удалять. Диагностику `.cache/t011_contract_proof.*` в продукт не переносить; правило `byte_sizes_uint64` покрывается обычными модульными тестами.

## Отчёт исполнителя (после В-1)

**2026-11-12. Ветка:** `codex/T-011` (продолжена существующая ветка). Код и тесты — `b0bfc9f`; отчёт и передача в review — отдельный коммит `T-011: report V3 validation and hand off for review`. Первый отчёт и решение управляющего по В-1 сохранены.

### Выполнение критериев приёмки

1. Минимальная сборка: **PASS, 84 теста**, внешние зависимости не добавлены. Новые исходники V3 подключены только при `AM_WITH_CONTAINER`.
2. Полная CLI и offline: **PASS, 262 тестов в каждом прогоне**. Все 189 прежних тестов сохранены. В двух проверках ожидаемый V3 изменён с `partial` на `passed`; в проверке вложений имя перенесённого правила обновлено на `storage_paths`. Фикстура удаления непрозрачных владельцев дополнена определением plugin-поля с областью `file`, её проверки сохранности не ослаблены.
3. Реализованы 38 правил закрытого каталога, включая три дополнения решения В-1; для каждого есть проверка эталона и минимальная мутация с ровно одной находкой, точными `rule`, `severity`, `message_id`, `pointer`. Отдельно выполнены M-01…M-08, числовые границы, единицы, календарь, Unicode, SPDX, URI, история, непрозрачные plugin-фрагменты, лимит находок, параллельные вызовы, замер большого документа и сценарии репозитория.
4. `arch_check`, `schema_check`, `sql_check` — **PASS** в min/CLI/offline. Domain не зависит от storage; новые публичные заголовки не содержат Windows/sqlite/zlib. До итоговой CLI выполнена чистая пересборка всех объектных файлов после изменения API отчёта; новых предупреждений MSVC/CMake нет.
5. Таблица правил, реальные примеры JSON и замер приведены ниже. `tags_search_key` отложено по O-08; это не мешает `complete`. UCUM, физический default и единица claim представлены оговорёнными находками `unverifiable`, а не предположениями о формате.
6. Изменены только разрешённые файлы. Слияние, push и переписывание истории не выполнялись. Статус строки T-011 в реестре — `review`.

### Реализация

`validate_semantics` читает известную часть `ManifestDocument`, не раскрывает binary handles кодека и не меняет DOM. Ошибки данных возвращаются находками; `Result` с ошибкой используется для внутренних ошибок/нехватки памяти. Индексы ссылок строятся до проверок и повторно используются; обход графов итеративный, история проверяется интервалами без развёртывания. Числа сравниваются по десятичным строкам, длина строк — по кодовым точкам. Лимит находок имеет пометку hypothesis; превышение даёт одну `limit_exceeded` и `complete = false` при обоих режимах продолжения.

Собственные разборы используют [RFC 3986, §3 и Appendix A](https://www.rfc-editor.org/rfc/rfc3986.html) и [официальную грамматику SPDX Annex D](https://spdx.github.io/spdx-spec/v2.3.1/SPDX-license-expressions/); UTF-8 в URI принимается по явному решению управляющего. Список лицензий, сеть и новые зависимости не используются.

Полный валидатор по умолчанию объединяет domain V3 с контекстом хранилища (идентичность, ZIP-пути, последняя операция). Класс `Invariants` больше не выполняет перенесённые правила манифеста и не дублирует их находки; ранние операционные проверки путей и коллизий сохранены. `repository_invariants()` оставлен частичным тестовым валидатором: один он фиксацию не разрешает. Перед заменой ZIP требуется `v3_complete` и отсутствие ошибок. `unverifiable` разрешает фиксацию с V3 `partial`; предупреждение — с `passed`. Неполный результат блокируется. Старый V3-invalid ассет можно читать, `verify` возвращает находки.

Коды нижних слоёв переводятся по всей таблице п. 12.1, с сохранением `Errc` и исходной ошибки в `cause`. Проверены реальная блокировка помощником, подстановка нехватки места, повреждённый ZIP, неподдерживаемый метод потока и остальные строки таблицы на границе commit. Повтор переданного `op_id` отклоняется до preflight/journal/temp как для change, так и для create существующего ассета. Чистая функция классификации ФС разрешает full только для fixed NTFS; съёмные носители, RAM-диск и остальные сочетания остаются read-only по умолчанию. Все AddFiles-пути запроса проверяются до открытия первого источника, в том числе между несколькими AddFiles.

### Правила → тесты

Общий положительный тест для всех строк — `ManifestSemantics.ExamplePassesCompleteCatalog`. В каждой строке `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/<rule>` дополнительно повторяет положительную проверку, затем проверяет единственную ожидаемую находку.

| Правило | Отрицательный тест |
|---|---|
| `unique_ids` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/unique_ids` |
| `storage_paths` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/storage_paths` |
| `history_operation_id_unique` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/history_operation_id_unique` |
| `byte_sizes_uint64` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/byte_sizes_uint64` |
| `historical_files` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/historical_files` |
| `derived_from` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/derived_from` |
| `input_source_refs` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/input_source_refs` |
| `input_entry_object` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/input_entry_object` |
| `input_entry_file` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/input_entry_file` |
| `input_nesting` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/input_nesting` |
| `group_refs` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/group_refs` |
| `preview_refs` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/preview_refs` |
| `field_options` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/field_options` |
| `field_constraints` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/field_constraints` |
| `field_default` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/field_default` |
| `custom_value_field` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/custom_value_field` |
| `value_representation` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/value_representation` |
| `value_options` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/value_options` |
| `value_file_ref` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/value_file_ref` |
| `value_constraints` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/value_constraints` |
| `value_unit_required` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/value_unit_required` |
| `value_dependencies_scope` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/value_dependencies_scope` |
| `x02_unit_conversion_open` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/x02_unit_conversion_open` |
| `x02_physical_default_open` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/x02_physical_default_open` |
| `claim_target_standard` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/claim_target_standard` |
| `claim_target_item` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/claim_target_item` |
| `claim_target_custom` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/claim_target_custom` |
| `claim_value_type` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/claim_value_type` |
| `claim_value_unit_unexpressible` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/claim_value_unit_unexpressible` |
| `claim_evidence` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/claim_evidence` |
| `license_file_ref` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/license_file_ref` |
| `license_spdx` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/license_spdx` |
| `urls` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/urls` |
| `dependency_refs` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/dependency_refs` |
| `timestamps` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/timestamps` |
| `asset_times` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/asset_times` |
| `history_coverage` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/history_coverage` |
| `history_compacted_summary` | `Catalog/ManifestSemanticRule.MinimalMutationProducesOnlyExpectedFinding/history_compacted_summary` |
| `tags_search_key` | Отложено по O-08; причина и отсутствие влияния на complete проверены в `ExamplePassesCompleteCatalog` |

Дополнительные проверки: `M01SelectedFieldMustExist`, `M02SelectedClaimCannotCarryValueV2`, `M03AlternativeAndRejectedRequireValueV2`, `M04CustomClaimRequiresKnownField`, `M05ItemClaimRequiresOwnerAuthor`, `M06ReorderedAuthorsKeepTargets`, `M07KnownFieldTypeMustMatch`, `M08PluginFragmentsRemainOpaqueAndDocumentUnchanged`; `ExactNumericRepresentationsAndBounds`, `PhysicalUnitsDefaultsAndClaimConstraints`, `CalendarAndFractionalDatetimeBounds`, `UnicodeCodepointsEnumsAndDeprecatedOptions`, `SpdxGrammar`, `UriGrammarAndAllTypedLocations`, `Uint64BoundsInAllSixLocations`, `EveryTimestampLocationUsesCalendarValidation`, `UriControlsInvalidUtf8AndLocations`, `HistoryIntervalsAndSummariesWithoutExpansion`, `HistoricalReferencesAreValidAndHashesAreNotCompared`, `ScopedDependenciesAndClaimsResolveWithinOwner`, `SameInputObjectReferencesAndNestedExclusion`, `FindingLimitIsReportedAndCompletenessIsAccurate`.

Проверки storage: `SemanticErrorsReturnFindingsAndProtectExistingBytes`, `UnverifiableSemanticsCanCommitAndRetainPartialReport`, `IncompleteValidatorCannotCommitAndWarningsDoNotBlock`, `PartialRepositoryInvariantsRemainTestOnly`, `OldInvalidV3RemainsReadableAndVerifyReturnsReport`, `DuplicateOperationIdFailsBeforePreflightJournalOrTemp`, `DriveClassifierGrantsFullOnlyForFixedNtfs`, `AllAddPathsCheckedBeforeAnySourceOpen`, `LowerLayerErrorMappingsPreserveCodeAndCause`, `UnsupportedPayloadMethodReturnsMappedReportError`, `RemainingLowerLayerMappingsAtCommitBoundary`. Прежние операции T-010 и 15 аварийных запусков помощника проверены повторно.

### Команды и фактический результат

Перед итоговой CLI: `.cache\t011_clean_build.cmd` → `cmake --build --preset win-cli --clean-first` (MSVC 14.44, 63 шага). Далее полные команды без фильтров:

```text
tools\build_min.cmd
100% tests passed, 0 tests failed out of 84
Total Test time (real) =  84.91 sec
```

```text
tools\build_cli.cmd
100% tests passed, 0 tests failed out of 262
Total Test time (real) = 1024.48 sec
```

```text
tools\build_cli.cmd --offline
100% tests passed, 0 tests failed out of 262
Total Test time (real) = 679.89 sec
```

Диагностика в `.cache/t011-final-{min,cli,offline}.log`, полный stdout CTest в `.cache/t011-final-{cli,offline}-tests.log`. Снимки SHA-256 кэшей до/после offline: 81 файл, изменённых файлов — 0. Новые зависимости не скачивались; offline выполнялся с `x-block-origin` и кэшами read-only.

### Замер теста 14

`ManifestSemantics.LargeGraphAndClaimsMeasurement`: 100 000 файлов, 10 000 групп цепочкой, 100 000 claims с разрешением author ID. Время измерено только вокруг вызова V3; пик памяти относится ко всему отдельному процессу теста, включая построение JSON и кодек. Это измерение, норматив не установлен.

```text
cli: SEMANTICS files=100000 groups=10000 claims=100000 seconds=28.9801 peak_working_set_bytes=2725978112 peak_commit_bytes=2789736448
offline: SEMANTICS files=100000 groups=10000 claims=100000 seconds=20.1606 peak_working_set_bytes=2725720064 peak_commit_bytes=2789412864
```

### JSON отчётов

Ниже реальные поля V3 из отчётов тестов (остальные поля опущены). Warning получен **только тестовой подстановкой** `ReportValidator` для проверки свода и сериализации; `test_warning` не добавлен в каталог V3. Предусмотренное каталогом предупреждение `tags_search_key` отложено по O-08.

Error, фиксация отклонена:

```json
{
  "v3": "failed",
  "v3_complete": true,
  "v3_findings": [
    {
      "rule": "license_spdx",
      "severity": "error",
      "pointer": "/asset/metadata/license/spdx",
      "message_id": "am.manifest.license_spdx",
      "detail": "Invalid SPDX expression"
    }
  ],
  "v3_deferred_rules": [
    {
      "rule": "tags_search_key",
      "reason": "O-08"
    }
  ],
  "plugin_validation": "not_performed"
}
```

Unverifiable, фиксация выполнена:

```json
{
  "v3": "partial",
  "v3_complete": true,
  "v3_findings": [
    {
      "rule": "x02_unit_conversion_open",
      "severity": "unverifiable",
      "pointer": "/asset/metadata/custom_values/0/value",
      "message_id": "am.manifest.x02_unit_conversion_open",
      "detail": "Unit conversion engine is not selected"
    }
  ],
  "v3_deferred_rules": [
    {
      "rule": "tags_search_key",
      "reason": "O-08"
    }
  ],
  "plugin_validation": "not_performed"
}
```

Warning (тестовая подстановка), фиксация выполнена:

```json
{
  "v3": "passed",
  "v3_complete": true,
  "v3_findings": [
    {
      "rule": "test_warning",
      "severity": "warning",
      "pointer": "/asset/metadata",
      "message_id": "am.manifest.test_warning",
      "detail": "Synthetic injected warning for report serialization"
    }
  ],
  "v3_deferred_rules": [
    {
      "rule": "tags_search_key",
      "reason": "O-08"
    }
  ],
  "plugin_validation": "not_performed"
}
```

### Отклонения и вопросы

Отклонений от объёма и решений управляющего нет. Новых открытых вопросов не выявлено. O-08, UCUM/X-02, физический default, единица значения claim, словарь history operation/changes, X-01 для каталогов и грамматика generation.tool остаются в границах решений задачи; самостоятельно они не закрывались.

Все изменения задачи закоммичены. Существовавший до начала работы неотслеживаемый `3D_am_open_questions_e2472e9.zip` не изменялся и в коммиты не включён.
