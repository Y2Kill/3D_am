# T-026. Исполнитель заданий, `ResourceManager` и `am jobs` (M3a, P0.6 — часть 1)

**Статус:** ready · **Веха:** M3a · **Основание:** ТЗ v1.8 §6.7 п. 1–7 [СОГЛАСОВАНО], §12.3 п. 1–4, §14.2 (п. 1, 5, 7, 8, 10), §14.4 п. 3; критерии К-115, К-121; ADR-0015, ADR-0024 (уровни: `jobs` зависит только от `foundation` и `contracts`), ADR-0033 п. 2–4; `docs/architecture/jobs_and_sessions.md` §1–2; T-023 (контракт `IJobStore`, `StateJobStore`, `op_log`, восстановление при открытии)

## Цель

1. Общий исполнитель фоновых заданий в `am_jobs`: выборка из `IJobStore`, параллельные рабочие потоки, безопасные точки, пауза, отмена, корректная остановка, `needs_decision`, иерархия «пакет → подзадания», ограниченные автоматические повторы. Исполнитель ничего не знает об импорте и других конкретных обработчиках (§12.3 п. 1, 4).
2. `jobs::ResourceManager` (ADR-0033 п. 2–4): лимиты CPU, чтения, записи и внешних процессов, лимиты чтения и записи по устройствам, приоритет интерактивных заданий, профили `economy|balanced|performance|custom`.
3. Определение типа носителя (HDD/SSD/сеть/съёмный/неизвестно) в `storage` — источник начальных лимитов ввода-вывода.
4. Первый настоящий тип задания — `library.verify` поверх существующего `Catalog::verify` — и служебный `dev.probe` для проверки CLI.
5. CLI `am jobs list|show|run|pause|resume|cancel|retry|answer|move`, а также `am library verify --enqueue`.

**План M3a (ориентир, уточняется по итогам каждой части):**
- T-026 — исполнитель и ресурсы;
- далее — журналы spdlog;
- `am import scan` (файлы и папки → план);
- `am import run` (сборка ассета, `inputs[]`, строгий и частичный режимы, пакет и подзадания);
- ZIP как вход;
- `am export`;
- корпус 16 сценариев без RAR/7z.

## Контекст (что прочитать)

- `docs/architecture/jobs_and_sessions.md` §1–2: модель, безопасные точки, пулы. После задачи документ приводится к принятым здесь решениям.
- `include/am/contracts/job_store.hpp`, `src/storage/db/job_store.cpp`, раздел «Решения управляющего» в T-023 (переходы, порядок, восстановление, журнал).
- `src/services/catalog/indexer.cpp` — `Catalog::open` (восстановление заданий), `Catalog::verify` (`progress`, `cancelled`).
- `src/application/common/library_service.cpp`, `src/application/cli/main.cpp` — устройство команд, коды выхода (T-012 п. 8), группа Development.
- ADR-0033 целиком.

## Решения управляющего (не пересматривать в задаче)

1. **Изменения контракта `IJobStore`** (T-023 разрешал расширение «если без него не проходит тест»; здесь оно нужно по существу):
   - новый переход `queued → paused` — удержание задания до старта (§6.7 п. 2 «приостановка задания или очереди»);
   - `running → queued` разрешён не только восстановлению, но и корректному прерыванию на безопасной точке;
   - `JobPatch::release_attempt` (`bool`, по умолчанию `false`) допустим только на `running → queued` и уменьшает `attempt` на 1, но не ниже 0. Корректная остановка исполнителя не должна расходовать попытку, иначе три штатных Ctrl+C превратят задание в `failed` при следующем аварийном восстановлении (лимит `max_attempts` из T-023 относится к авариям);
   - на `needs_decision → queued` разрешён `decision_json`: это исходный вопрос плюс поле `"answer"`, п. 7;
   - на переходах в `succeeded`, `failed`, `cancelled` разрешён `clear_decision`.

   Таблица в `job_store.hpp`, тесты T-023 (`JobContract.*`, `StateJobs.*`) и `jobs_and_sessions.md` §1 обновляются согласованно. Схема `state.sql` **не меняется**.

2. **Состав `am_jobs`** — заголовки `include/am/jobs/`, код `src/jobs/`. Зависимости — только `foundation` и `contracts` (`arch_check`). Набросок, имена можно уточнить; отклонения — в отчёте:

   ```cpp
   namespace am::jobs {
   enum class ResourceKind { cpu, read, write, external_process };      // gpu — M6
   enum class MediaKind { hdd, ssd, network, removable, unknown };
   struct ResourceRequest { ResourceKind kind; std::string device; };   // device пуст для cpu/external_process
   enum class Profile { economy, balanced, performance, custom };
   struct ResourceLimits { unsigned jobs, cpu, external_process;
                           unsigned io_hdd, io_ssd, io_network, io_removable, io_unknown; };
   ResourceLimits profile_limits(Profile, unsigned hardware_concurrency);   // п. 4
   class Lease;                                                         // RAII, только перемещение
   class ResourceManager {
    public:
     explicit ResourceManager(ResourceLimits);
     void set_media(std::string device, MediaKind);                    // неизвестное устройство → unknown
     // Всё или ничего: набор выдаётся целиком, иначе ожидание. Остановка → Errc::cancelled.
     Result<Lease> acquire(std::span<const ResourceRequest>, bool interactive, int priority, std::stop_token);
     ResourceSnapshot snapshot() const;                                 // занято/лимит по ключам, очередь ожидания
   };
   enum class StopReason { none, pause, cancel, shutdown };
   struct Decision { std::string kind; std::vector<std::string> options; std::string context_json = "{}"; };
   class JobContext {
    public:
     const contracts::JobRecord& record() const;
     std::stop_token stop() const; StopReason stop_reason() const;     // проверка — в безопасных точках
     void report(const contracts::JobProgress&);                        // с прореживанием, п. 8
     Result<Lease> acquire(std::span<const ResourceRequest>);
     std::optional<std::string> answer() const;                         // выбранный вариант после needs_decision
     Profile profile() const;                                           // из params.resource_profile или общий
     Result<contracts::JobRecord> enqueue_child(contracts::JobSpec);    // только для родителя
     ChildrenSummary children() const;                                  // счётчики по состояниям
   };
   struct JobOutcome;   // succeeded{result_json} | failed{Error} | needs_decision{Decision}
                        // | waiting_resources{Error} | resource_error{Error} | cannot_continue_safely{Error}
                        // | interrupted | await_children
   class JobHandler { public: virtual ~JobHandler() = default; virtual JobOutcome run(JobContext&) = 0; };
   class HandlerRegistry {          // type → фабрика; повторная регистрация → am.jobs.type_duplicate
    public: Result<void> add(std::string type, std::function<std::unique_ptr<JobHandler>(const contracts::JobRecord&)>);
   };
   struct JobEvent;                 // started | progress{rate?} | state_changed | finished
   class JobObserver { public: virtual ~JobObserver() = default; virtual void on_event(const JobEvent&) noexcept = 0; };
   struct ExecutorOptions { ResourceLimits limits; Profile profile = Profile::balanced;
                            std::chrono::milliseconds report_interval{250}, retry_interval{30000};
                            int auto_retries = 3; };
   class Executor {
    public:
     Executor(contracts::IJobStore&, ResourceManager&, const HandlerRegistry&, ExecutorOptions, JobObserver* = nullptr);
     Result<RunSummary> run_until_idle(std::stop_token);  // остановка = shutdown, п. 6
     // Управление (п. 7); работает и без запущенного run_until_idle.
     Result<void> pause(std::string_view job_id);  Result<void> resume(std::string_view job_id);
     Result<void> cancel(std::string_view job_id); Result<void> retry(std::string_view job_id);
     Result<void> answer(std::string_view job_id, std::string_view option);
   };
   }
   ```

3. **Выборка и параллелизм.**
   - Кандидаты — `queued`. Порядок: `(priority DESC, position ASC)` верхнего уровня. Подзадания допускаются к запуску, только пока их родитель в `running` (ожидает подзадания, п. 5), и идут в своём порядке.
   - Одновременно выполняется не более `limits.jobs` заданий плюс **один резервный слот** для интерактивных. Интерактивное задание — `priority >= jobs::interactive_priority` (константа 100). Фоновая очередь не может занять резерв, поэтому не блокирует интерактивную операцию (К-115).
   - Ожидающие в `ResourceManager` обслуживаются в порядке: интерактивные, затем `priority DESC`, затем FIFO.
   - `acquire` выдаёт набор ресурсов атомарно («всё или ничего»). Это исключает взаимоблокировку двух заданий, берущих чтение и запись в разном порядке. Повторный `acquire` при уже удерживаемой аренде того же вида → `am.jobs.lease_nested`.
   - Лимиты ввода-вывода считаются на устройство: два задания на разных дисках не делят один лимит.

4. **Профили — гипотезы** (ADR-0033 п. 4; в `nfr_measurement_plan.md` не вносить как норматив). `hc` = `hardware_concurrency`, не меньше 1.

   | Параметр | economy | balanced (по умолчанию) | performance |
   |---|---|---|---|
   | `jobs` | 1 | 2 | max(2, hc/2) |
   | `cpu` | 1 | max(1, hc/2) | max(1, hc−1) |
   | `external_process` | 1 | 1 | 2 |
   | `io_hdd` / `io_removable` | 1 / 1 | 1 / 1 | 1 / 1 |
   | `io_ssd` | 1 | 2 | 4 |
   | `io_network` | 1 | 2 | 2 |
   | `io_unknown` | 1 | 1 | 2 |

   - `custom` = `balanced` с явными переопределениями.
   - Нулевой лимит → `invalid_argument`, `am.jobs.limits_invalid`.
   - Профиль конкретного задания (`params.resource_profile`) задаёт только внутренний параллелизм обработчика (`JobContext::profile()`), но не общие лимиты исполнителя: профиль не подменяет фактические ограничения (ADR-0033 п. 4).

5. **Иерархия.**
   - Родитель запускается первым. Обработчик создаёт подзадания через `enqueue_child`, проверяя, что их ещё нет: повторный запуск идемпотентен. Затем он возвращает `await_children`.
   - Родитель остаётся в `running`, но не занимает рабочий поток и слот.
   - Когда все подзадания **урегулированы** (`succeeded | failed | cancelled`), обработчик родителя вызывается снова с `children()` и возвращает итог.
   - Подзадания в `needs_decision`, `paused`, `waiting_resources`, `resource_error`, `cannot_continue_safely` держат родителя в ожидании.
   - Каскад — политика исполнителя (T-023 п. 2):
     - `pause`, `resume`, `cancel`, `retry` родителя применяются к его подзаданиям в подходящих состояниях;
     - отказ одного подзадания не останавливает соседние (К-121).
   - Вложенность — один уровень (T-023 п. 3).

6. **Безопасные точки и остановка.**
   - Исполнитель передаёт в задание `stop_token` и причину (`pause | cancel | shutdown`). Обработчик проверяет их **только** между единицами работы. Внутри фазы фиксации ADR-0006 остановки нет. Исполнитель никогда не убивает поток, а ждёт безопасной точки (`jobs_and_sessions.md` §1).
   - `interrupted` отображается так:
     - `pause` → `paused`;
     - `cancel` → `cancelled`;
     - `shutdown` → `queued` с `release_attempt`.
   - Если обработчик после запроса остановки всё же вернул `succeeded`, итог — `succeeded`: работа уже сделана.
   - `run_until_idle(stop)`: при остановке новые задания не берутся, выполняемые получают `shutdown`. Родители, ожидающие подзадания, переводятся в `queued` с `release_attempt`.
   - Возврат — когда нет ни выполняемых заданий, ни кандидатов, ни отложенных повторов п. 9.

7. **Управление** (`Executor::pause/…`; CLI п. 12).

   | Команда | Состояния → итог | `running` в этом процессе |
   |---|---|---|
   | `pause` | `queued` → `paused` | остановка `pause` → `paused` |
   | `resume` | `paused`, `waiting_resources` → `queued` | — |
   | `cancel` | `queued`, `paused`, `needs_decision`, `waiting_resources`, `resource_error`, `cannot_continue_safely` → `cancelled` | остановка `cancel` → `cancelled` |
   | `retry` | `failed`, `resource_error`, `cannot_continue_safely` → `queued` | — |
   | `answer` | `needs_decision` → `queued` с `decision_json` + `"answer": {"option": "<id>"}` | — |

   - Вариант, которого нет в `options`, → `invalid_argument`, `am.jobs.answer_invalid`.
   - Неподходящее состояние → ошибка контракта T-023 (`am.jobs.transition_invalid`/`state_conflict`).
   - Задание `running` в **другом** процессе управлять нельзя: библиотека захвачена владельцем записи → `Errc::locked` (ADR-0031). Это ожидаемое поведение P0.
   - `decision_json` очищается на итоговом переходе (`clear_decision`).

8. **Прогресс и события.**
   - `report` пишет в хранилище не чаще `report_interval` (гипотеза 250 мс; в тестах 0). Исключения — смена `stage` и последний отчёт перед итогом: они пишутся сразу.
   - Скорость (единиц в секунду) исполнитель оценивает по последовательным отчётам и передаёт только в `JobEvent::progress`, не в хранилище. Неизвестный `total` остаётся неизвестным (§6.7 п. 4).
   - `JobObserver::on_event` вызывается без удерживаемых блокировок исполнителя и не должен бросать исключения.

9. **Итоги обработчика и повторы.**
   - `failed{Error}` с `Errc::insufficient_space` или `resource_exhausted` исполнитель трактует как `waiting_resources`.
   - `read_only` и `locked` → `cannot_continue_safely`.
   - `cancelled` → `cancelled`.
   - Остальные коды → `failed`.
   - Явный итог обработчика (`resource_error{…}` и т. п.) имеет приоритет над этим отображением.
   - `waiting_resources`: через `retry_interval` исполнитель сам возвращает задание в `queued`, не более `auto_retries` раз за один `run_until_idle`. Следующий такой итог превращается в `resource_error` прямо из `running` с той же ошибкой. Бесконечных повторов нет (ADR-0033 п. 3).
   - Задания `waiting_resources`, найденные при старте `run_until_idle`, возвращаются в `queued` (это засчитывается как первый повтор).
   - Числа 30 с / 3 — гипотезы.
   - Неизвестный `type` → `failed`, `am.jobs.type_unknown`.
   - Исключение из обработчика ловится на границе исполнителя → `failed`, `Errc::internal`, `am.jobs.handler_exception` (без текста исключения в `message_id`; текст — в `error_json.detail`).
   - Ни один из этих случаев не останавливает другие задания (К-121).
   - `error_json`/`result_json` — корректный JSON по правилам T-023.

10. **Тип носителя — `storage`**, рядом с `detect_storage_capabilities` (ADR-0031):

    ```cpp
    struct MediaInfo { std::string device_id; jobs::MediaKind kind; };
    MediaInfo detect_media(const std::filesystem::path&) noexcept;    // никогда не ошибка: при сомнении unknown
    ```

    - **Windows:**
      - `GetVolumePathNameW` → `GetVolumeNameForVolumeMountPointW` (`device_id`);
      - `GetDriveTypeW`: `DRIVE_REMOTE` → `network`, `DRIVE_REMOVABLE` → `removable`;
      - для фиксированных — `IOCTL_STORAGE_QUERY_PROPERTY` / `StorageDeviceSeekPenaltyProperty` (`IncursSeekPenalty` → `hdd`, иначе `ssd`);
      - отказ запроса → `unknown`.
    - **Linux:**
      - `st_dev` (`device_id` = `major:minor`);
      - `statfs` (NFS/CIFS/SMB2/FUSE-сетевые → `network`);
      - `/sys/dev/block/<maj>:<min>`, при необходимости родительское устройство раздела: `queue/rotational` (1 → `hdd`, 0 → `ssd`), `removable` (1 → `removable`);
      - корень sysfs — параметр внутренней функции для тестов.
    - Переопределение типа носителя пользователем — через `ResourceManager::set_media` из настроек; формат настроек — не здесь.
    - `jobs::MediaKind` объявлен в `am_jobs`; `storage` от `jobs` зависеть не может (ADR-0024). Поэтому `detect_media` возвращает собственный `storage`-тип, а `application` отображает его в `jobs::MediaKind`.

11. **Обработчики и сборка в `application`.**
    - **`library.verify`** (`services/catalog`, `services` может зависеть от `jobs`):
      - параметры `{"level":"quick|full","include_trash":true}` (неизвестные поля → `failed`, `am.jobs.params_invalid`);
      - одна аренда `read` на устройство библиотеки на всё время;
      - прогресс `assets`, стадия `verify`;
      - отмена через `LibraryVerifyOptions::cancelled` → `interrupted`;
      - `result_json` — счётчики отчёта (`total, checked, ok, warn, corrupt, unsupported, unreadable`);
      - найденные повреждения — это `succeeded` (задание выполнило работу), а не `failed`;
      - запись `library.verify` в журнале пишет сам `Catalog`, как в T-023.
    - **`dev.probe`** — служебный тип для тестов CLI (группа Development, как `am probe`):
      - параметры `{"steps":N,"ask":bool,"fail":"<message_id>"|null}`;
      - шаги без сна, с безопасной точкой между шагами;
      - `ask` → `needs_decision` с `options ["continue","stop"]`, после ответа продолжает или завершается `cancelled`.
    - **`application::JobService`** собирает всё вместе:
      - `Catalog` (нужен доступ `Catalog::job_store()` к `StateJobStore` поверх `state.sqlite` и `Catalog::operation_log()`; добавить);
      - `ResourceManager` с профилем и `detect_media(root)`;
      - реестр обработчиков;
      - наблюдатель, который пишет в `op_log`:
        - `job.started` (`info`, `{type, attempt}`);
        - `job.finished` (`succeeded` → `info`; `cancelled`, `paused`, `needs_decision`, `waiting_resources` → `warn`; `failed`, `resource_error`, `cannot_continue_safely` → `error`; `{type, state, attempt, message_id?}`).
    - Сбой записи журнала не меняет состояние задания: предупреждение `am.catalog.log_failed`, как в T-023.
    - Потокобезопасность `Catalog` для параллельных записывающих заданий в этой задаче **не утверждается**. Единственный записывающий сценарий M3a (импорт) решит её отдельно. Если `Catalog::verify` нельзя вызывать параллельно с другими операциями `Catalog`, это фиксируется в отчёте.

12. **CLI** (общие `--json`/`--verbose`; тексты английские, каталог `en.toml`; коды выхода — таблица T-012 п. 8).
    - `am jobs list <dir> [--state <s>]... [--parent <id>] [--all]` — режим `read_only`. По умолчанию — верхний уровень без завершённых (`succeeded`, `cancelled`); `--all` — все. Для родителей — сводка подзаданий по состояниям и суммарный прогресс. JSON: `{"jobs":[{job_id,type,state,priority,position,attempt,progress:{done,total,unit,stage},parent_id,children?:{<state>:n},created_at,started_at,finished_at}],"command":"jobs list"}`.
    - `am jobs show <dir> <id>` — `read_only`, полная запись; `params`, `decision`, `result`, `error` — разобранный JSON.
    - `am jobs run <dir> [--profile economy|balanced|performance] [--jobs N]` — режим записи, `run_until_idle`.
      - Ctrl+C (Windows: `CTRL_C_EVENT` и `CTRL_BREAK_EVENT` через `SetConsoleCtrlHandler`; POSIX: `SIGINT`, `SIGTERM`) → корректная остановка п. 6. Сообщение «waiting for running jobs to reach a safe point» выводится в stderr.
      - Строки событий выводятся в stderr; итог — в stdout (JSON: `{"processed","succeeded","failed","attention","interrupted":bool,"jobs":[{job_id,type,state,message_id?}],"command":"jobs run"}`).
      - Коды выхода:
        - 0 — все задания, завершённые в этом запуске, `succeeded`, либо очередь пуста;
        - 1 — есть `failed`;
        - 3 — нет `failed`, но есть требующие внимания (`needs_decision`, `resource_error`, `cannot_continue_safely`, `paused`) или запуск прерван;
        - 4 — не удалось открыть библиотеку или библиотека захвачена.
    - `am jobs pause|resume|cancel|retry <dir> <id>...` — режим записи. Результат — по каждому ID. Код 0, если применено всё; 4, если хотя бы один ID не применён: остальные всё равно применяются, JSON `{"results":[{job_id,ok,state?,message_id?}]}`.
    - `am jobs answer <dir> <id> <option>`, `am jobs move <dir> <id> (--before <id> | --last)`.
    - `am library verify <dir> [--full] [--no-trash] --enqueue` — ставит `library.verify` в очередь и выводит `job_id` (код 0). Без `--enqueue` команда работает как раньше.
    - `am jobs enqueue-dev <dir> [--steps N] [--ask] [--fail <message_id>]` — группа Development, `dev.probe`.
    - `am media <path>` — группа Development, вывод `detect_media` (`device_id` в выводе допустим: команда локальная; в результаты замеров и отчёт не копировать).

## Тесты

GoogleTest с `RecordProperty("req", …)`:
- `K-115` — ресурсы и приоритеты;
- `K-121` — изоляция отказов;
- `NFR-14.4.3` — персистентность и восстановление.

Синхронизация в тестах — латчи, барьеры и условные переменные, а **не** `sleep_for`. Таймауты допустимы только как аварийная защита от зависания.

- **`JobsResources.*`:**
  - таблица профилей;
  - нулевые лимиты;
  - лимит на устройство;
  - «всё или ничего» (два задания берут `{read A, write B}` и `{write B, read A}` — нет взаимоблокировки; детерминированный стресс с фиксированным seed);
  - порядок ожидания (интерактивные → приоритет → FIFO);
  - остановка во время ожидания → `cancelled`;
  - вложенная аренда.
- **`JobsExecutor.*`** — на тестовом `IJobStore` в памяти, соблюдающем `job_transition_allowed`, и повторно на `StateJobStore` поверх временной библиотеки:
  - успех;
  - порядок по приоритету и позиции;
  - максимум одновременно выполняемых не превышает `jobs` (счётчик максимума);
  - резервный интерактивный слот при занятой фоновой очереди;
  - неизвестный тип, исключение, отказ одного не останавливает соседей;
  - `pause`/`cancel`/`shutdown` на безопасной точке;
  - `release_attempt`;
  - три подряд `shutdown` и затем аварийное восстановление T-023 не дают `attempts_exhausted`;
  - `needs_decision` → `answer` → повторный запуск видит ответ;
  - неверный вариант ответа;
  - `waiting_resources`: автоповторы и затем `resource_error`;
  - отображение кодов п. 9;
  - родитель: `await_children`, повторный вызов после урегулирования, ожидание при `needs_decision` у подзадания, каскад `pause/resume/cancel/retry`, идемпотентный повтор родителя после восстановления;
  - прореживание `report` (`report_interval` > 0: не больше ожидаемого числа записей; смена стадии пишется сразу);
  - наблюдатель вызывается без блокировок (обработчик из наблюдателя может вызвать `Executor::pause`).
- **`JobContract.*`/`StateJobs.*`** — обновлённая таблица переходов, `release_attempt`, `decision_json` при ответе.
- **`StorageMedia.*`:**
  - разбор поддельного дерева sysfs (раздел → родитель, `rotational`, `removable`, отсутствующие файлы → `unknown`);
  - на Windows — дымовой тест на каталоге сборки (не падает, `device_id` не пуст, тип из перечня);
  - на Linux CI — то же.
- **`VerifyJob.*`:**
  - задание проверяет библиотеку с повреждённым ZIP → `succeeded`, `corrupt = 1` в `result_json`, запись `library.verify` в журнале;
  - отмена во время проверки → `cancelled`;
  - `shutdown` → `queued`, попытка не израсходована;
  - неверные параметры.
- **CLI (Python, `cli_jobs`, ctest с `--ci`):**
  - `verify --enqueue` → `jobs list` → `jobs run` (0) → `jobs show` с `result`;
  - `enqueue-dev --ask` → `run` (3) → `answer continue` → `run` (0);
  - `enqueue-dev --fail` → `run` (1) → `retry` → `cancel`;
  - `pause`/`resume`/`move` на `queued`;
  - `jobs pause` при захваченной библиотеке → 4;
  - `am media` — дымовой;
  - журнал содержит `job.started`/`job.finished`.
- **Ctrl+C из CLI** — тест через `subprocess`: Windows — `CREATE_NEW_PROCESS_GROUP` + `CTRL_BREAK_EVENT`, Linux — `SIGINT`; задание `dev.probe` с большим числом шагов и ожиданием готовности по строке в stderr. Если на CI тест нестабилен, он помечается меткой `manual` и исключается из CI с причиной в отчёте; семантика при этом покрыта `JobsExecutor.*`.

## Файлы (что можно менять)

- `include/am/jobs/**`, `src/jobs/**` (новое), `src/jobs/CMakeLists.txt`.
- `include/am/contracts/job_store.hpp`; `include/am/storage/db/job_store.hpp`, `src/storage/db/job_store.cpp` (п. 1).
- `detect_media`: заголовок и реализация рядом с `detect_storage_capabilities` в `storage`.
- `include/am/services/catalog/*`, `src/services/catalog/*` — `Catalog::job_store()`, `operation_log()`, обработчик `library.verify`.
- `include/am/application/*`, `src/application/common/*`, `src/application/cli/main.cpp`, каталог сообщений `en.toml`, `CHANGELOG.md` (английский).
- `tests/**` (новые тесты и обновление тестов T-023 по п. 1), `CMakeLists.txt` тестов.
- `docs/architecture/jobs_and_sessions.md` (§1–2 по итогам: таблица переходов, `release_attempt`, резервный слот, профили-гипотезы, итоги обработчика), `docs/research/libraries.md` (если используется что-то сверх стандартной библиотеки и ОС API — сначала описать там; ожидается, что не понадобится), `docs/plan/acceptance_matrix.md` (строка NFR-14.4.3; новые строки К-115, К-121 — статус «реализовано частично: исполнитель и ресурсы; бюджеты RAM/VRAM и GUI — позже»).
- Раздел «Отчёт исполнителя» этого файла.

Не менять: `schemas/**` (в том числе `state.sql`), опубликованную схему и golden-корпус, файлы владельца по `CLAUDE.md`.

## Критерии приёмки

1. `tools\build_min.cmd --ci`, `build_cli --ci -LE periodic`, `build_cli --offline --ci -LE periodic` зелёные на Windows; Linux (WSL или CI) — зелёный, пропуски объяснены.
2. `arch_check`: `am_jobs` зависит только от `foundation` и `contracts`, `storage` — не от `jobs`.
3. Все пункты раздела «Тесты» реализованы. В тестах нет синхронизации через `sleep_for`.
4. Корректная остановка не расходует попытку (тест «три `shutdown` + авария»).
5. Отказ, исключение или неизвестный тип одного задания не останавливает другие (К-121). Резервный слот даёт интерактивному заданию стартовать при занятой фоновой очереди (К-115).
6. `am jobs run` возвращает коды 0/1/3/4 по п. 12; каждый код получен тестом.
7. `jobs_and_sessions.md` и таблица в `job_store.hpp` согласованы; гипотезы (профили, 250 мс, 30 с × 3) помечены как гипотезы.
8. Отчёт исполнителя: изменения контракта, отклонения от наброска п. 2, вывод о потокобезопасности `Catalog::verify`, результаты прогонов (числа тестов, без имён компьютера, пользователя и путей).

## Вне объёма

- Импорт, экспорт, `import.batch`/`import.asset` — следующие части M3a.
- Журналы spdlog и экспорт диагностики — отдельная часть M3a.
- Бюджеты RAM/VRAM, GPU, процесс `am-worker` — M6/M9 (ADR-0033 п. 2).
- Управление выполняемым заданием из другого процесса (IPC) — по необходимости GUI (M5).
- Политика хранения истории заданий — открытый §10.10 п. 4.
- Формат файла пользовательских настроек ресурсов — вместе с настройками приложения.

## Отчёт исполнителя

_(заполняет исполнитель)_
