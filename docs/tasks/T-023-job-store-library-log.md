# T-023. Хранилище заданий `IJobStore` и журнал библиотеки `op_log` (M2, P0.5 — часть 8)

**Статус:** done (принята 2026-11-18, `27cbbb3`) · **Веха:** M2 · **Основание:** ТЗ v1.8 §6.7 п. 1, 2, 5–7 [СОГЛАСОВАНО], §10.10 п. 1, 4, 5, §14.4 п. 3, 4, 6; SPEC `container_manifest_v1.md` §10 («импорт готового ассета … запись идёт в журнал библиотеки»); ADR-0015, ADR-0024 (`IJobStore` — уровень `contracts`, реализация — `storage`), ADR-0033 п. 3; `docs/architecture/jobs_and_sessions.md` §1; `schemas/sql/state.sql` (таблицы `jobs`, `op_log` уже опубликованы); T-020 (журнал библиотеки отложен до этой задачи), T-022 (порядок восстановления при открытии)

## Цель

1. Узкий контракт `am::contracts::IJobStore` и его реализация на `state.sqlite` в `storage`: персистентные задания, допустимые переходы состояний, прогресс, вопросы пользователю, порядок очереди, иерархия «пакет → подзадания».
2. Восстановление заданий при открытии библиотеки на запись — после восстановления `pending_ops` (ТЗ §14.4 п. 4, `jobs_and_sessions.md` §1): `running` → `queued` с увеличенным `attempt` либо `failed` при исчерпании попыток.
3. Журнал библиотеки `op_log` (§6.7 п. 5) и запись в него существующих операций библиотеки, в том числе импорта готового ассета (SPEC §10). CLI `am library log`.

Исполнителя заданий, пулов, `ResourceManager`, `am jobs …` здесь нет — это M3a. Задача даёт им хранилище, которое уже проверено.

## Решения управляющего (не пересматривать в задаче)

1. **Контракт — `include/am/contracts/job_store.hpp`, только заголовок, зависимость только от `foundation`** (ADR-0024).

   ```cpp
   namespace am::contracts {
   enum class JobState { queued, running, paused, needs_decision, waiting_resources, resource_error,
                         cannot_continue_safely, succeeded, failed, cancelled };
   std::string_view job_state_name(JobState) noexcept;              // строки как в state.sql
   std::optional<JobState> parse_job_state(std::string_view) noexcept;
   constexpr bool job_transition_allowed(JobState from, JobState to) noexcept;  // таблица п. 2
   constexpr bool job_terminal(JobState) noexcept;                  // succeeded | cancelled
   struct JobProgress { std::optional<std::uint64_t> done, total; std::string unit, stage; };
   struct JobSpec {
     std::string type;                      // "import.asset", "verify", … (грамматика п. 3)
     std::string params_json = "{}";
     std::optional<std::string> parent_id, job_id;  // job_id: UUIDv4, по умолчанию новый
     int priority = 0;
   };
   struct JobRecord {
     std::string job_id, type, params_json; std::optional<std::string> parent_id;
     JobState state = JobState::queued; int priority = 0; std::int64_t position = 0; int attempt = 0;
     JobProgress progress;
     std::optional<std::string> decision_json, result_json, error_json;
     std::string created_at; std::optional<std::string> started_at, finished_at;
   };
   struct JobPatch {                        // применяется вместе с переходом
     std::optional<std::string> decision_json, result_json, error_json;
     bool clear_decision = false;
   };
   struct JobFilter { std::vector<JobState> states; std::optional<std::string> parent_id; bool top_level_only = false; };
   class IJobStore {
    public:
     virtual ~IJobStore() = default;
     virtual Result<JobRecord> enqueue(const JobSpec&) = 0;
     virtual Result<std::optional<JobRecord>> get(std::string_view job_id) = 0;
     virtual Result<std::vector<JobRecord>> list(const JobFilter& = {}) = 0;  // порядок п. 5
     virtual Result<JobRecord> transition(std::string_view job_id, JobState from, JobState to, const JobPatch& = {}) = 0;
     virtual Result<void> report(std::string_view job_id, const JobProgress&) = 0;   // только в running
     virtual Result<void> move_before(std::string_view job_id, std::optional<std::string_view> before_job_id) = 0;
     virtual Result<std::size_t> remove_finished(std::string_view before_timestamp) = 0;  // п. 7
   };
   }
   ```

   Набор методов — минимальный для M3a. Расширять его в этой задаче можно, только если без нового метода не проходит тест; новый метод описать в отчёте.
2. **Переходы** (`jobs_and_sessions.md` §1 + ADR-0033 п. 3). Других переходов нет; `transition` проверяет таблицу и текущее состояние атомарно (`UPDATE … WHERE job_id = ? AND state = ?`, ровно одна строка). Несовпадение текущего состояния → `Errc::conflict`, `am.jobs.state_conflict`; недопустимый переход → `Errc::invalid_argument`, `am.jobs.transition_invalid`.

   | Из | В |
   |---|---|
   | `queued` | `running`, `cancelled` |
   | `running` | `paused`, `needs_decision`, `waiting_resources`, `resource_error`, `cannot_continue_safely`, `succeeded`, `failed`, `cancelled`, `queued` (только восстановление п. 6) |
   | `paused` | `queued`, `cancelled` |
   | `needs_decision` | `queued` (ответ), `cancelled` |
   | `waiting_resources` | `queued`, `cancelled` |
   | `resource_error` | `queued` (повтор), `failed`, `cancelled` |
   | `cannot_continue_safely` | `queued` (явный повтор), `cancelled` |
   | `failed` | `queued` (повтор) |
   | `succeeded`, `cancelled` | — (конечные) |

   - `queued → running`: `attempt += 1`, `started_at` = сейчас, `finished_at` = NULL.
   - Переход в `succeeded`/`failed`/`cancelled`: `finished_at` = сейчас.
   - Переход в `needs_decision` требует `decision_json`; выход из него очищает `decision_json` (если не передано иное). В остальных переходах `decision_json` запрещён → `am.jobs.transition_invalid`.
   - `failed`/`cancelled` с `error_json` — по желанию вызывающего; `succeeded` с `result_json` — тоже.
   - Отмена родителя не каскадирует автоматически: каскад — политика исполнителя (M3a). `ON DELETE CASCADE` из схемы действует только при удалении (п. 7).
3. **Проверка входных данных** — до записи:
   - `type`: `^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*$`, до 64 байт;
   - `params_json`, `decision_json`, `result_json`, `error_json`: корректный JSON (`json::parse_strict` foundation, лимиты кодека), сохраняется исходный текст без перезаписи;
   - `job_id`: канонический UUIDv4; занятый → `Errc::conflict`, `am.jobs.exists`;
   - `parent_id`: существующее задание, не конечное; иначе `am.jobs.parent_invalid`. Вложенность ограничивается одним уровнем (родитель сам не может иметь родителя): так описана модель «пакет → подзадания»;
   - `JobProgress`: `done`, `total` ≤ `INT64_MAX` (колонки `INTEGER`), `done ≤ total` при известном `total`; иначе `am.jobs.progress_invalid`. Неизвестный `total` = NULL (§6.7 п. 4: неопределённый прогресс обозначается честно).
   - Ошибки ввода → `Errc::invalid_argument` с `message_id` из `am.jobs.*`.
4. **Реализация — `storage::db::StateJobStore`** (`include/am/storage/db/job_store.hpp`, `src/storage/db/job_store.cpp`) над **заимствованным** соединением `library.database(DatabaseKind::state)` и подставляемыми часами (`std::function<std::chrono::system_clock::time_point()>`, по умолчанию системные; формат времени — как у журнала фиксаций). Схема `state.sql` **не меняется**. Режим `read_only`: чтение работает, запись → `Errc::read_only` с существующим `am.catalog.read_only` (или существующим эквивалентом `am.library.*` — выбрать и указать в отчёте); SQLite-отказ записи не должен просачиваться как `io_error`.
5. **Порядок очереди.**
   - Новое задание получает `position = max(position) + 1` среди заданий с тем же `parent_id` (NULL — верхний уровень).
   - `list` сортирует по `(priority DESC, position ASC)` — как индекс `jobs_state`.
   - `move_before(id, before)`: `id` ставится непосредственно перед `before` среди заданий того же `parent_id`; `before = nullopt` — в конец. Двигать можно только `queued`/`paused`; иное → `am.jobs.transition_invalid`. Разные родители → `am.jobs.parent_invalid`. Перенумерация — одной транзакцией; позиции остаются уникальными в пределах родителя. `move_before` меняет только `position`, `priority` не трогает; непосредственное соседство в `list` гарантируется при равном `priority`, при разных приоритетах вызов успешен, `id` стоит перед `before` в последовательности позиций (В-2).
6. **Восстановление при открытии** — в `Catalog::open` (`read_write`), **после** шага восстановления `pending_ops` и переиндексации T-022, до отметки сессии:
   - каждое `running` → `queued` (`attempt` не меняется: он увеличится при следующем `queued → running`), если `attempt < max_attempts`; иначе → `failed` с `error_json` от ошибки `Errc::incomplete`/`am.jobs.attempts_exhausted`, `finished_at` = сейчас;
   - `max_attempts` — параметр `OpenOptions`/`StateJobStore` (по умолчанию 3; гипотеза, уточняется по плану измерений — в отчёте и в `docs/architecture/nfr_measurement_plan.md` не записывать как норматив);
   - остальные состояния не меняются: `paused`, `needs_decision`, `waiting_resources`, `resource_error`, `cannot_continue_safely` ждут пользователя или исполнителя;
   - в `OpenReport` — `std::vector<std::string> jobs_requeued, jobs_failed`; ошибка шага → предупреждение `am.jobs.recovery_failed`, открытие не прерывается (ZIP-восстановление уже выполнено);
   - режим `read_only` — ничего не меняется; число `running` — в предупреждении `am.jobs.recovery_pending`.
7. **Удаление истории заданий** — `remove_finished(before)`: удаляет конечные (`succeeded`, `cancelled`) и `failed` задания с `finished_at < before` и без незавершённых потомков; подзадания удаляются каскадом схемы. Автоматической политики хранения нет (срок хранения — открытый §10.10 п. 4, п. 7).
8. **Журнал библиотеки — `storage::db::OperationLog`** (`include/am/storage/db/operation_log.hpp`) над той же таблицей `op_log`:

   ```cpp
   struct LogEntry { std::int64_t seq = 0; std::string at, level, code; std::optional<std::string> job_id, asset_id;
                     std::string message; std::optional<std::string> detail_json; };
   struct LogQuery { std::optional<std::string> job_id, asset_id; std::optional<std::int64_t> after_seq; std::size_t limit = 100; };
   Result<std::int64_t> append(const LogEntry&);   // seq и at заполняет журнал
   Result<std::vector<LogEntry>> query(const LogQuery&) const;  // по seq по возрастанию
   Result<std::size_t> prune_before(std::string_view timestamp);
   ```

   - `level`: `info` | `warn` | `error` (как в схеме). `code` — стабильный машинный код события (п. 9). `message` — короткий технический текст на английском для диагностики, **не для показа пользователю** (ADR-0032: интерфейс строит текст по `code` и `detail_json`). `detail_json` — корректный JSON.
   - Автоматической очистки нет (п. 7).
9. **Что пишется в журнал** — из `Catalog` после завершения операции, по одной записи на ассет; сбой записи журнала не отменяет операцию: предупреждение `am.catalog.log_failed`, без пометки `rebuild_required`.

   | `code` | Когда | `asset_id` | `detail_json` |
   |---|---|---|---|
   | `library.import_ready` | каждый результат `import_ready` (включая `identical`, конфликты, отказы) | да | `{op_id?, classification, status, revision?, replaced_sha256?}` |
   | `library.copy` | `copy_independent` (успех и отказ) | новый ID; при отказе — источник | `{op_id?, source_asset:{asset_id, revision?}, message_id?}` |
   | `library.trash` / `library.restore` / `library.purge` | каждая операция | да | `{op_id?, message_id?}` |
   | `library.recovery` | каждый пункт `recovered` и каждое переставленное задание при открытии | да, если известен | `{op_id?, job_id?, outcome}` |
   | `library.verify` | итог `Catalog::verify` | — | `{level, total, checked, ok, warn, corrupt, unsupported, unreadable, cancelled}` |

   - `level`: успех — `info`; предупреждения и `identical`/конфликт без изменений — `warn`; отказ/повреждение — `error`.
   - `create`/`change` в журнал не пишутся: их история — внутри ZIP (SPEC §10), журнал §6.7 п. 5 относится к обработке и операциям библиотеки. `sync`/`rebuild` — тоже нет (их отчёт возвращается вызывающему).
   - Режим `read_only`: в журнал ничего не пишется.
10. **CLI `am library log <dir> [--asset <id>] [--job <id>] [--after <seq>] [--limit <n>]`**, общие `--json`/`--verbose`; открывает библиотеку `read_only`. JSON: `{"entries":[{"seq","at","level","code","job_id","asset_id","message","detail"}],"next_after":<seq|null>,"command":"library log"}`; `detail` — разобранный JSON. `--limit` 1…1000, по умолчанию 100; ошибка аргумента → 2. Код 0 при успешном чтении (в том числе пустом).

## Тесты

`RecordProperty("req", …)`: хранилище заданий — `NFR-14.4.3` (и `K-123` для восстановления); журнал — `NFR-14.4.9|K-31` для импорта, иначе `NFR-14.4.9`. Если `req_report` не знает какого-либо ID — добавить строку в `acceptance_matrix.md` (п. «Файлы»).

1. **Контракт:** `job_state_name`/`parse_job_state` взаимно обратны для всех 10 состояний, строки совпадают со списком в `state.sql`; `job_transition_allowed` совпадает с таблицей п. 2 для всех 100 пар (таблица в тесте записана независимо).
2. **Жизненный цикл:** enqueue → running (`attempt = 1`, `started_at`) → `report` → needs_decision с `decision_json` → queued (решение очищено) → running (`attempt = 2`) → succeeded с `result_json` (`finished_at`); `get` после переоткрытия базы возвращает то же.
3. **Недопустимые переходы и гонка:** каждый запрещённый переход → `am.jobs.transition_invalid`, строка без изменений; два `transition(queued → running)` для одного задания — один успех, второй `am.jobs.state_conflict`; `report` вне `running` отклоняется.
4. **Проверка ввода:** неверный `type`, некорректный JSON в каждом из четырёх полей, не-UUIDv4/занятый `job_id`, несуществующий/конечный/вложенный родитель, прогресс > `INT64_MAX` и `done > total` — свой `message_id`, ничего не записано. Непрозрачный JSON (`1e9999`, `-0`, порядок ключей, пробелы) хранится байт-в-байт.
5. **Порядок:** позиции по родителю; `list` по `(priority DESC, position ASC)`; `move_before` в начало, в середину, в конец; запрет для `running`/конечных и между разными родителями; позиции уникальны после каждого шага.
6. **Иерархия и удаление:** пакет с тремя подзаданиями; фильтры `parent_id`/`top_level_only`; `remove_finished` удаляет только конечные/`failed` старше порога и не удаляет родителя с незавершённым потомком; подзадания удалённого родителя удалены каскадом.
7. **Восстановление при открытии (`K-123`, `NFR-14.4.3`):** два задания `running` (`attempt` 1 и 3 при `max_attempts = 3`), по одному в остальных состояниях → `Catalog::open`: первое `queued`, второе `failed` с `am.jobs.attempts_exhausted`, остальные без изменений, `OpenReport` перечисляет оба; порядок — после восстановления `pending_ops` (тест со строкой журнала фиксаций и заданием одновременно: сначала ZIP, затем задание; проверить по `op_log` или по отчёту). `read_only` → ничего не изменено, `am.jobs.recovery_pending`.
8. **Режим `read_only` хранилища:** чтение работает, каждая запись отклоняется, база без изменений.
9. **Журнал:** `append`/`query` с фильтрами по `job_id`, `asset_id`, `after_seq`, `limit`; `seq` монотонен; `prune_before`; некорректный `level`/`detail_json` отклоняется.
10. **Записи операций (п. 9):** `import_ready` (новый, `identical`, конфликт, отказ), `copy` (успех и отказ), `trash`/`restore`/`purge`, восстановление при открытии, `verify` — каждая даёт одну запись с ожидаемыми `code`, `level`, `asset_id` и ключами `detail`; `create`/`change`/`sync`/`rebuild` записей не дают. Подставленный сбой журнала → операция успешна, предупреждение `am.catalog.log_failed`, ZIP и индекс корректны.
11. **CLI** (`cli_library_log`): init → `asset create` → `add-ready` (два раза: новый и `identical`) → `library log` → 0 и две записи `library.import_ready`; фильтры `--asset`, `--after`, `--limit`; `--limit 0` → 2. Чтение журнала работает и на Linux без SKIP (только `read_only`); записывающая часть — по существующей политике NTFS с явной причиной SKIP.

## Файлы (что можно менять)

- Изменяемые:
  - новые: `include/am/contracts/job_store.hpp`; `include/am/storage/db/job_store.hpp`, `src/storage/db/job_store.cpp`; `include/am/storage/db/operation_log.hpp`, `src/storage/db/operation_log.cpp`; регистрация в `src/storage/CMakeLists.txt`;
  - `include/am/services/catalog/indexer.hpp`, `src/services/catalog/indexer.cpp` (восстановление заданий в `open`, записи журнала, `OpenReport`);
  - `include/am/storage/library/library.hpp` — **только** поле `max_attempts` в `OpenOptions`, если параметр решено передавать через `Library`; иначе передавать через параметры `Catalog::open` без изменения `Library`;
  - фасад `library_service`, `src/application/cli/main.cpp`;
  - тесты: новые `tests/unit/test_job_store.cpp`, `tests/unit/test_operation_log.cpp` (можно один файл), дополнения в `tests/unit/test_open_recovery.cpp`, `tests/cli/test_library_cli.py`, регистрация в `tests/CMakeLists.txt`;
  - `resources/messages/en.toml` (`am.jobs.*`, `am.catalog.log_failed`), `docs/user/cli.md`, `CHANGELOG.md`;
  - `docs/plan/acceptance_matrix.md` — строки `NFR-14.4.3` (хранилище и восстановление заданий; без исполнителя — «реализовано частично») и `NFR-14.4.9` (журнал библиотеки; ротация и экспорт — не реализованы), если их нет;
  - этот файл (отчёт), строка T-023 в `docs/tasks/README.md`.
- **Не менять:**
  - `schemas/*` (включая `schemas/sql/state.sql`), golden ZIP, кодек, правила V1–V4;
  - протокол фиксации и журнал фиксаций (`src/storage/commit/*`, `state_journal.cpp`), `AssetRepository`;
  - `src/jobs/*` (исполнитель — M3a), `src/storage/library/*` (кроме оговорённого поля);
  - ADR, спецификации, roadmap, `coverage_matrix.md`, `jobs_and_sessions.md`, `vcpkg.json`.
- Если переход, состояние или колонка из этого файла расходятся со `state.sql` или `jobs_and_sessions.md` так, что без изменения схемы не обойтись, — остановиться и описать.

## Критерии приёмки

1. `tools\build_min.cmd --ci`, `tools\build_cli.cmd --ci`, `tools\build_cli.cmd --offline --ci` — PASS; `tools/build_linux.sh cli --ci` в WSL — PASS. Число тестов не уменьшилось; `req_report` — 0 неверных ID; `arch_check` PASS (контракт — только `foundation`).
2. Тесты 1–11 выполнены. В отчёте:
   - имена тестов;
   - где в коде проверяется таблица переходов и как обеспечена атомарность;
   - выбранный `message_id` для записи в `read_only` (п. 4) и способ передачи `max_attempts` (п. 6);
   - пример JSON `library log`.
3. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-023`, без слияния, push и переписывания истории.

## Вне объёма

- Исполнитель заданий, пулы потоков, безопасные точки, `stop_token`, `ResourceManager`, профили — M3a (ADR-0015, ADR-0033 п. 2, 4).
- CLI `am jobs list|pause|resume|cancel|retry` — M3a, вместе с исполнителем.
- Политика хранения и ротация журналов, экспорт обезличенной диагностики, spdlog — M3a (ТЗ §14.4 п. 9; открытый §10.10 п. 7).
- Сессии внешнего редактирования (`edit_sessions`) и очистка `snapshots` при старте — отдельные задачи.
- Генератор `gen_library` и замер К-112 — отдельная задача M2.

## Отчёт исполнителя

**Итог:** T-023 выполнена, статус `review`. Решения по В-1/В-2 учтены.

### Ветка и коммиты

- Ветка `codex/T-023` от `main` (`1c68c7c`); T-022 принята (`fe218e8`, приёмка `0a3818a`).
- `202c1eb` — реализация, пользовательская документация и тесты. Дополнительные проверки recovery-записей и req-метка — `c8c41c7` (Strengthen T-023 recovery log assertions). Итоговый отчёт, матрица и статус — отдельный завершающий коммит.
- Слияние в main, push и переписывание истории не выполнялись. Прежний пользовательский `3D_am_open_questions_e2472e9.zip` не изменён.

### Выполнение критериев приёмки

1. Фактические итоги обязательных четырёх команд приведены ниже. Добавлено 14 тестов: 12 GoogleTest и два CLI. Минимальная конфигурация остаётся 86 тестов; Windows CLI — 414 вместо 400; Linux CLI — 411 вместо 397.
2. Реализованы контракт, SQLite-хранилище, восстановление заданий в Catalog::open после pending_ops/refresh, журнал и интеграция операций, CLI library log, документация и тесты 1–11. Имена и методики — в таблице ниже.
3. Изменены только разрешённые файлы. Единственное изменение Library — поле max_attempts в OpenOptions. Схемы, AssetRepository, протокол и журнал фиксаций, src/jobs, ADR, спецификация, jobs_and_sessions.md, golden ZIP, roadmap, coverage_matrix и зависимости не менялись. Существующие SQLite и foundation JSON сверены с docs/research/libraries.md; новых библиотек и загрузок не требовалось.

Контракт `include/am/contracts/job_store.hpp` зависит только от foundation и стандартной библиотеки; функции имён/разбора/переходов — inline/constexpr. `job_terminal` считает конечными только succeeded/cancelled; failed допускает повтор.

Таблица переходов проверяется в `job_transition_allowed` и перед записью в `StateJobStore::transition` (`src/storage/db/job_store.cpp:130`). Изменение — один `UPDATE jobs … WHERE job_id=? AND state=? RETURNING …` (строка 141). Ровно одна возвращённая строка означает успех, отсутствие — `Errc::conflict/am.jobs.state_conflict`. Недопустимый переход — `Errc::invalid_argument/am.jobs.transition_invalid`; обычный transition отдельно отклоняет running → queued, зарезервированный для восстановления. В тесте гонки используются два независимых SQLite-соединения, а не только mutex одного объекта.

enqueue, перенумерация, очистка истории и восстановление — транзакции BEGIN IMMEDIATE. Mutex объекта и mutex заимствованного SQLite-соединения защищают составные операции. Хранилище не закрывает соединение владельца. Текст всех JSON-полей сохраняется без преобразования чисел; проверка — foundation `json::parse_strict` с существующими пределами 1024/128 MiB (гипотезы кодека, новых нормативов нет).

Запись в read_only возвращает **Errc::read_only/am.catalog.read_only** до SQL; дополнительно проверяется sqlite3_db_readonly. Чтение работает. Часы подставляются в StateJobStore/OperationLog; UTC имеет формат журнала фиксаций.

`max_attempts` передаётся через разрешённое поле `Library::OpenOptions::max_attempts` в конструктор StateJobStore. Значение по умолчанию **3 — гипотеза**, не норматив; nfr_measurement_plan.md не менялся. Конкретный метод `recover_running()` нужен для теста 7 и отделяет разрешённый startup-переход running → queued от обычного интерфейса IJobStore. Сам IJobStore сверх заданного набора методов не расширялся.

В `Catalog::open` строки 805–822 идут после ZIP-recover и прохода refresh, до чтения/записи writer_session: сначала журналируются пункты ZIP-recovery, затем восстанавливаются задания и журналируются их исходы. Ошибка job store даёт am.jobs.recovery_failed с причиной, не отменяя восстановление ZIP. В read_only выполняется только SELECT count(*) для running и предупреждение am.jobs.recovery_pending. В OpenReport добавлены jobs_requeued/jobs_failed.

OperationLog пишет seq/at самостоятельно, фильтрует по job/asset/after_seq, возвращает возрастающий seq; prune_before явный, автоматической очистки нет. remove_finished учитывает незавершённых детей и возвращает число фактически удалённых строк, включая каскадных потомков.

Catalog журналирует import_ready, copy, trash/restore/purge, каждый recovery и итог verify; create/change/sync/rebuild не журналируются. Ошибка append — am.catalog.log_failed без rebuild_required и без отмены операции. Для отказов copy/lifecycle добавлен узкий доступ к operation_warnings, чтобы фасад передавал предупреждение и при Result<Error>. Если посторонний/нечитаемый import-источник не содержит известного AssetID, asset_id остаётся NULL; вымышленный ID не создаётся.

CLI log открывает Catalog read_only. Для JSON используется отдельный LibraryLogOutcome: метаданные экранируются обычным JSON-сериализатором, а проверенный detail включается как исходный JSON. Это сохраняет 1e9999, -0 и порядок/пробелы без преобразования в double. Обычный текст строится по code через каталог; технический message показывается только с verbose. В JSON message присутствует как заданное диагностическое поле.

### Тесты 1–11

| Пункт | Имена и проверка |
|---|---|
| 1 | `JobContract.NamesAndAllTransitionsMatchIndependentTable`: все 10 имён, разбор, терминальность, сверка имён с state.sql и независимая таблица всех 100 пар. |
| 2 | `StateJobs.LifecyclePersistsExactJsonProgressAndDecisions`: queued/running/report/needs_decision/queued/running/succeeded, attempt 1/2, UTC и переоткрытие БД. |
| 3 | `StateJobs.ForbiddenTransitionsAndConcurrentCompareAndSwap`: все запрещённые пары без изменения строки; два SQLite-соединения конкурируют за queued → running, один успех и один state_conflict; report вне running и обычный running → queued запрещены. |
| 4 | `StateJobs.InputValidationDoesNotWriteAndOpaqueFieldsStayExact` и тест жизненного цикла: type, JSON всех четырёх полей, UUIDv4/дубликат, родители и вложенность, предел INTEGER и done/total; непрозрачные 1e9999/-0/пробелы/порядок ключей сохранены. |
| 5 | `StateJobs.OrderingMovePreservesPriorityAndSiblingPositionsAreUnique`: разные родители, priority DESC, перемещения начало/середина/конец, paused/running/terminal, уникальность позиций, priority не меняется при переносе через другую группу. |
| 6 | `StateJobs.HierarchyFiltersAndCleanupProtectUnfinishedChildren`: родитель с тремя детьми, фильтры, отсутствие каскадной отмены, защита от удаления незавершённых детей, cutoff и каскад удаления. |
| 7 | `JobRecovery.CommitRecoveryPrecedesJobsAndReadOnlyPreservesEverything`: verified pending_ops и running attempt 1/3 одновременно; read_only без изменений; затем ZIP/индекс восстановлены, jobs queued/failed с прежними attempt, остальные состояния и метаданные прежние; порядок подтверждён seq в op_log. `JobRecovery.StoreFailureWarnsAfterZipRecoveryWithoutPreventingOpen`: ошибка таблицы jobs предупреждает после успешного ZIP-recovery, индекс не требует перестройки. |
| 8 | `StateJobs.ReadOnlyRejectsEveryWriteWithoutChangingDatabase`: чтение, все шесть видов записи/восстановление, append/prune журнала отклонены; байты базы прежние. |
| 9 | `OperationLog.ExactJsonFiltersSequencePruningAndValidation`: часы/seq заполняет журнал, возрастающий seq, фильтры и limit, точный JSON, prune, недопустимые level/detail. |
| 10 | `CatalogLog.OperationsHaveOneEntryAndUnloggedOperationsStayUnlogged`: новый/identical/конфликт/отказ import_ready, успех/отказ copy, trash/restore/purge, verify, коды/уровни/ID/detail и отсутствие create/change/sync/rebuild. `CatalogLog.FailedAppendWarnsWithoutUndoingAssetOrMarkingIndexForRebuild`: подставленный сбой append, успешные trash/verify, предупреждение, корректные ZIP/индекс, без rebuild_required. Recovery проверен тестами п. 7. |
| 11 | `cli_library_log`: чтение на обеих ОС без SKIP, фильтры asset/job/after/limit, next_after, limit 0/-1/1001 → 2, точный JSON detail, пользовательский текст без технического message, неизменность файлов. `cli_library_log_writes`: init → asset create → add-ready два раза → две реальные записи info/warn; Windows PASS, на Linux отдельно проверен отказ production-записи и явный SKIP только этой части. |

### Команды и фактический вывод

```text
tools\build_min.cmd --ci
100% tests passed, 0 tests failed out of 86
Total Test time (real) = 19.80 sec
req_report: 13 IDs, 86 tests, 49 without req, 0 invalid IDs
```

Код 0; лог `.cache/t023-final-min.log`, JUnit/req_report — `.cache/t023-final-min-junit.xml`, `.cache/t023-final-min-req.json`.

```text
tools\build_cli.cmd --ci
100% tests passed, 0 tests failed out of 414
Total Test time (real) = 1059.79 sec
req_report: 53 IDs, 414 tests, 49 without req, 0 invalid IDs
```

Код 0, **414/414 PASS, 0 SKIP**. Лог `.cache/t023-final-win.log`, JUnit `.cache/t023-final-win-junit.xml`, трассировка `.cache/t023-final-win-req.json`. arch_check/schema_check/sql_check реально выполнены и PASS.

```text
tools/build_linux.sh cli --ci  (WSL, Ubuntu)
100% tests passed, 0 tests failed out of 411
Total Test time (real) = 1186.47 sec
req_report: 53 IDs, 411 tests, 48 without req, 0 invalid IDs
```

Код 0; JUnit: **389 PASS + 22 SKIP, 0 FAIL** из 411. До задачи — 397 тестов, 21 SKIP. Новый единственный пропуск — production-запись cli_library_log_writes по существующей политике fixed NTFS. cli_library_log реально выполнен без SKIP; все новые C++-тесты хранилища, журнала и recovery реально выполнены. Лог `.cache/t023-final-linux.log`, JUnit `.cache/t023-final-linux-junit.xml`, трассировка `.cache/t023-final-linux-req.json`.

```text
tools\build_cli.cmd --offline --ci
100% tests passed, 0 tests failed out of 414
Total Test time (real) = 1057.48 sec
req_report: 53 IDs, 414 tests, 49 without req, 0 invalid IDs
```

Код 0; **414/414 PASS, 0 SKIP**. Лог `.cache/t023-final-offline.log`, JUnit `.cache/t023-final-offline-junit.xml`, трассировка `.cache/t023-final-offline-req.json`. Origin-загрузки заблокированы (`x-block-origin`), зависимости использованы из существующих кешей. Во всех четырёх прогонах arch_check/schema_check/sql_check PASS и 0 неверных ID.

Усиленный JobRecovery повторён на Linux: 2/2 PASS, 6.14 sec, 0 invalid IDs; лог `.cache/t023-linux-recovery-log.log`. Реализация после 202c1eb не менялась, последующий c8c41c7 усиливает только assertions/req. Полный offline-прогон включает окончательную версию тестов.

Целевые Windows-проверки: Job* 9/9 PASS (17.85 sec); OperationLog/CatalogLog/CLI/message_catalog_check функционально PASS; arch_check через штатный скрипт отдельно 1/1 PASS (6.50 sec). Linux: Job* 9/9 PASS (9.09 sec), OperationLog/CatalogLog 3/3 PASS (7.85 sec), cli_library_log PASS без SKIP, cli_library_log_writes — явный платформенный SKIP.

После первого Windows-прогона уточнены только assertions и req-метка recovery-теста, без изменения реализации: `tools\build_cli.cmd --offline --ci -R JobRecovery` — 2/2 PASS, 5.87 sec, 0 invalid IDs (`.cache/t023-recovery-log-focus.log`). Проверки подтверждают code/level/asset_id/detail всех трёх recovery-записей и неизменность прогресса остальных состояний.

Прямой запуск CTest для arch_check вне MSVC/vcpkg-окружения повредил только сгенерированный CMake-кэш. Кэш восстановлен CMake --fresh и чистой сборкой с кодовой страницей 65001; проверка Ninja-зависимостей и arch_check после этого PASS. Исходные build-скрипты не менялись. Новых предупреждений MSVC/GCC в коде T-023 нет; прежние предупреждения GCC в неизменённых файлах вне задачи не исправлялись.

### Пример JSON library log

Реальные две записи после нового и повторного add-ready, код 0 (`.cache/t023-win-log-example.json`):

```json
{"entries": [{"seq": 1, "at": "2026-11-18T08:30:07Z", "level": "info", "code": "library.import_ready", "job_id": null, "asset_id": "3ddd34d3-00a8-4df8-9ec0-1939edfcbdd7", "message": "Library operation completed", "detail": {"classification": "new", "status": "imported", "op_id": "ad700109-6b71-4501-9ca5-076cbc4fd715", "revision": 1}}, {"seq": 2, "at": "2026-11-18T08:30:08Z", "level": "warn", "code": "library.import_ready", "job_id": null, "asset_id": "3ddd34d3-00a8-4df8-9ec0-1939edfcbdd7", "message": "Library operation completed", "detail": {"classification": "identical", "status": "identical", "revision": 1}}], "next_after": 2, "command": "library log"}
```

### Отклонения и решения владельца

- В-1 закрыт в чате: **выполнять правило T-023**, attempt увеличивать только при queued → running, не при восстановлении. jobs_and_sessions.md согласует управляющий; исполнитель документ не менял.
- В-2 закрыт в чате: **менять только position, не priority**; непосредственный порядок гарантирован внутри одного приоритета. before=nullopt даёт последнюю позицию среди братьев и, следовательно, конец своей группы приоритета.
- Тесты хранилища, контракта и журнала объединены в разрешённом test_job_store.cpp; recovery дополнен в разрешённом test_open_recovery.cpp. Реальная CLI-запись выделена в cli_library_log_writes, чтобы чтение журнала на Linux не помечалось SKIP.
- Строки матрицы NFR-14.4.3/NFR-14.4.9 отражают частичную реализацию: без исполнителя, ротации и экспорта, как предписано задачей.

### Вопросы

Открытых вопросов нет. В-1 и В-2 разрешены явным ответом владельца.

## Приёмка

Принята 2026-11-18 управляющим, слияние `27cbbb3` (ветка `codex/T-023`, итоговый коммит `8257769`).

**Прогоны управляющего** (все `.cpp` пересобраны): `build_min --ci` 86/86, `build_cli --ci` 414/414, `build_cli --offline --ci` 414/414 PASS. WSL — по отчёту исполнителя (389 PASS, 22 SKIP; `cli_library_log` без SKIP); CI GitHub — после публикации.

**Проверено по коду:**
- контракт `contracts::IJobStore` зависит только от `foundation`; таблица переходов совпадает с решением 2; `running → queued` через `transition` запрещён, доступен только `recover_running`;
- `transition` — один `UPDATE … WHERE job_id=? AND state=? RETURNING`, конфликт → `am.jobs.state_conflict`; `attempt` растёт только на `→ running`, там же сбрасывается `finished_at`; `decision_json` очищается при выходе из `needs_decision`;
- восстановление в `Catalog::open` идёт после `pending_ops` и переиндексации, до отметки сессии; `attempt` при восстановлении не меняется (В-1); `read_only` только предупреждает `am.jobs.recovery_pending`;
- `move_before` перенумеровывает позиции родителя в одной транзакции и не меняет `priority` (В-2);
- журнал: записи `import_ready` (в том числе отказ), `copy`, `trash/restore/purge`, `recovery` (ZIP и задания), `verify`; сбой записи → предупреждение `am.catalog.log_failed`, операция не отменяется; `am library log` открывает `read_only`, `detail` выводится байт-в-байт.

**Документы:** `docs/architecture/jobs_and_sessions.md` §1 исправлен по В-1 (`attempt` при восстановлении не растёт); решение 5 этой задачи дополнено по В-2.

**Замечания (не блокируют):**
1. `remove_finished` удаляет родителя, если все потомки завершены, даже когда `finished_at` потомка не старше порога; такие потомки уходят каскадом раньше срока. Уточнить вместе с политикой хранения (§10.10 п. 4).
2. В `read_only` ошибка запроса счётчика `running` молча игнорируется — предупреждение `am.jobs.recovery_pending` не выдаётся; допустимо, но стоит отразить в диагностике при работе над журналами M3a.
3. `Catalog::verify` пишет журнал только при успешном завершении; прерванная ошибкой проверка в `op_log` не попадает.
4. Проверка UUIDv4 продублирована ещё в двух местах `job_store.cpp` (`validate_id` и проверка `parent_id`) — к открытому замечанию T-022 о выносе в foundation.

