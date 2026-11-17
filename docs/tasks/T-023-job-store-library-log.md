# T-023. Хранилище заданий `IJobStore` и журнал библиотеки `op_log` (M2, P0.5 — часть 8)

**Статус:** ready · **Веха:** M2 · **Основание:** ТЗ v1.8 §6.7 п. 1, 2, 5–7 [СОГЛАСОВАНО], §10.10 п. 1, 4, 5, §14.4 п. 3, 4, 6; SPEC `container_manifest_v1.md` §10 («импорт готового ассета … запись идёт в журнал библиотеки»); ADR-0015, ADR-0024 (`IJobStore` — уровень `contracts`, реализация — `storage`), ADR-0033 п. 3; `docs/architecture/jobs_and_sessions.md` §1; `schemas/sql/state.sql` (таблицы `jobs`, `op_log` уже опубликованы); T-020 (журнал библиотеки отложен до этой задачи), T-022 (порядок восстановления при открытии)

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
   - `move_before(id, before)`: `id` ставится непосредственно перед `before` среди заданий того же `parent_id`; `before = nullopt` — в конец. Двигать можно только `queued`/`paused`; иное → `am.jobs.transition_invalid`. Разные родители → `am.jobs.parent_invalid`. Перенумерация — одной транзакцией; позиции остаются уникальными в пределах родителя.
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
