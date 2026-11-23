# Задания, безопасная фиксация и сессии редактирования

Связано с ADR-0006, ADR-0015 и ТЗ 4.6, 6.7, 8.7–8.8, 10.10. Схема БД: `schemas/sql/state.sql`.

## 1. Модель задания

Состояния и разрешённые переходы совпадают с `contracts::job_transition_allowed`:

| Из | В |
|---|---|
| `queued` | `running`, `paused`, `cancelled` |
| `running` | `queued`, `paused`, `needs_decision`, `waiting_resources`, `resource_error`, `cannot_continue_safely`, `succeeded`, `failed`, `cancelled` |
| `paused`, `needs_decision`, `waiting_resources`, `cannot_continue_safely` | `queued`, `cancelled` |
| `resource_error` | `queued`, `failed`, `cancelled` |
| `failed` | `queued` |
| `succeeded`, `cancelled` | нет |

- **Безопасные точки** (ТЗ 6.7.2): обработчик проверяет `stop_token` между единицами работы, никогда внутри commit протокола ADR-0006. `interrupted` переводится в `paused`, `cancelled` или `queued` по причине `pause`, `cancel`, `shutdown`. Завершённый обработчиком `succeeded` сохраняется даже при позднем запросе остановки.
- **Попытки:** `queued → running` увеличивает `attempt`. Штатный `shutdown` использует `JobPatch::release_attempt` только на `running → queued` и уменьшает счётчик на 1 с нижней границей 0. При аварийном восстановлении счётчик сохраняется; при `attempt ≥ max_attempts` получается `failed` / `am.jobs.attempts_exhausted`. Сначала восстанавливаются `pending_ops`, затем задания. Повтор обработчика должен быть идемпотентен.
- **Решения** (ТЗ 10.10.5): `needs_decision` сохраняет `{kind, options:[…], context:{…}}` и освобождает поток. `answer` проверяет вариант, сохраняет исходный вопрос с `"answer":{"option":"id"}` и возвращает задание в `queued`. `JobContext::answer()` читает выбор. Завершение в `succeeded`, `failed`, `cancelled` очищает вопрос через `clear_decision`.
- **Иерархия:** один уровень «родитель → дети». Обработчик создаёт недостающих детей идемпотентно и возвращает `await_children`. Родитель остаётся `running`, освобождает поток и все аренды. Дети запускаются только у такого родителя, в собственном порядке `(priority DESC, position ASC)`. Когда каждый ребёнок находится в `succeeded`, `failed` или `cancelled`, родитель вызывается снова без новой попытки; прочие состояния блокируют продолжение. `pause/resume/cancel/retry` каскадируются на подходящие состояния детей; успешные дети не повторяются. Сводка содержит состояния и суммарный прогресс; неизвестные величины и несовместимые единицы не превращаются в вымышленные числа.
- **Выборка:** корни идут по `(priority DESC, position ASC)`. Одновременно заняты не более `limits.jobs` обычных слотов и одного резервного для `priority >= interactive_priority` (100). Фоновые задания резерв не занимают. Ожидающий детей родитель слотом не считается.
- **Ресурсы:** `ResourceManager` выдаёт набор CPU/read/write/external_process атомарно, через перемещаемую RAII-аренду. Очередь: интерактивные, затем приоритет по убыванию, затем FIFO. Чтение и запись имеют отдельные лимиты на `device_id`; разные устройства независимы. Повторный захват уже удерживаемого вида ресурса запрещён (`am.jobs.lease_nested`), остановка ожидания возвращает `cancelled` без частичной аренды. RAM/VRAM и GPU — последующие этапы.

Профили — **гипотезы**, а не нормативы (ADR-0033). `hc = max(1, hardware_concurrency)`:

| Параметр | economy | balanced (по умолчанию) | performance |
|---|---|---|---|
| jobs | 1 | 2 | max(2, hc/2) |
| cpu | 1 | max(1, hc/2) | max(1, hc−1) |
| external_process | 1 | 1 | 2 |
| io_hdd / io_removable | 1 / 1 | 1 / 1 | 1 / 1 |
| io_ssd | 1 | 2 | 4 |
| io_network | 1 | 2 | 2 |
| io_unknown | 1 | 1 | 2 |

`custom` — balanced с явными переопределениями. Нулевые лимиты отвергаются (`am.jobs.limits_invalid`). `params.resource_profile` влияет только на профиль внутри обработчика, не увеличивает глобальные пределы. `storage::detect_media` возвращает собственный тип (Windows: volume GUID / seek penalty; Linux: st_dev / statfs / sysfs); приложение отображает его в `jobs::MediaKind`. Неопределённость означает `unknown`.

Итоги обработчиков:

| Итог | Действие исполнителя |
|---|---|
| `succeeded` | проверенный JSON результата, состояние `succeeded` |
| `failed` + insufficient_space / resource_exhausted | `waiting_resources` |
| `failed` + read_only / locked | `cannot_continue_safely` |
| `failed` + cancelled | `cancelled` |
| иной `failed` | `failed` |
| явные `waiting_resources`, `resource_error`, `cannot_continue_safely` | соответствующее состояние, приоритет над отображением кода |
| `needs_decision`, `interrupted`, `await_children` | правила выше |

Повтор `waiting_resources` через 30 с, максимум 3 за один запуск — **гипотезы**, задаются `retry_interval` / `auto_retries`. Найденное на старте ожидание возвращается в `queued` и расходует первый повтор; следующий исход после исчерпания бюджета — `resource_error`. Неизвестный тип даёт `am.jobs.type_unknown`; исключение фабрики/обработчика — internal / `am.jobs.handler_exception`, подробности в `error_json.detail`. Соседние задания продолжают работу.

## 2. Интерфейсы (C++, ядро)

Публичные определения — `include/am/jobs/resources.hpp`, `executor.hpp`. `am_jobs` зависит только от foundation/contracts. `HandlerRegistry` хранит фабрики по строковому типу, повторная регистрация запрещена; наполнение завершается до запуска. `JobHandler::run(JobContext&)` возвращает `JobOutcome`; контекст предоставляет запись, stop/reason, report, acquire, answer/profile и enqueue_child/children. `Executor` выполняет `run_until_idle`, pause/resume/cancel/retry/answer. Управление доступно и без активного запуска; конкурентный второй запуск отклоняется.

`report_interval` 250 мс — **гипотеза**. Смена стадии и последний отчёт перед исходом пишутся сразу. Скорость вычисляется по последовательным отчётам и передаётся только в событии, неизвестный `total` сохраняется. `JobObserver::on_event` вызывается без блокировок исполнителя и имеет `noexcept`; из него допустим `pause`. Наблюдатель может получать события разных рабочих потоков и должен сам синхронизировать своё состояние.

`ExecutorOptions::clock` позволяет подменить монотонное время в детерминированных тестах. Из-за отсутствия уведомления enqueue в `IJobStore` исполнитель проверяет новые записи с `poll_interval` 250 мс (**гипотеза**); `wake()` будит его немедленно. `ResourceManager` допускает наблюдатель регистрации ожидания без своей блокировки — для детерминированных проверок очереди.

`application::JobService` владеет Catalog, ресурсами, реестром и исполнителем. `library.verify` находится в services/catalog, берёт read-аренду на весь вызов, использует безопасные точки Catalog и возвращает его семь счётчиков. Найденное повреждение — успешное выполнение проверки. Catalog пишет `library.verify`; наблюдатель приложения пишет `job.started` / `job.finished`, сбои журнала становятся предупреждениями без изменения исхода. `Catalog::verify` сериализуется общей рекурсивной блокировкой Catalog; параллельные записывающие сценарии здесь не заявляются.

CLI предоставляет `jobs list/show` в read_only, остальные команды с блокировкой владельца библиотеки. События и ожидание безопасной остановки идут в stderr, единственный итог — stdout. Windows CTRL_C/CTRL_BREAK и POSIX SIGINT/SIGTERM только запрашивают штатный shutdown. `dev.probe` и `am media` — группа Development. Коды `jobs run`: 0 — успех/пусто, 1 — failed, 3 — внимание/прервано, 4 — невозможно открыть библиотеку. Пакетное управление применяет все ID и возвращает 4 при любой неудаче.

## 3. Протокол фиксации — псевдокод

```text
commit_update(asset, mutate):
  op = journal.insert(prepared, base=stat(asset.zip) + sha256(manifest))
  tmp = lib/.am/.am-tmp/<asset>.<op>.zip
  m = manifest.clone(); staged = mutate(m)     // SHA-256 и размеры новых данных известны заранее (подготовка/lookup)
  if m == manifest: journal.drop(op); return    // no-op: ревизия не меняется (ADR-0035 п. 5)
  m.asset.revision += 1; m.history.entries.push(op, ts, ...)
  m.asset.modified_at = ts                     // = timestamp последней записи истории (O-14)
  if has_unknown_numbers(m) and not exact_number_safe: abort(write_blocked)   // R-09.8 / O-13, до итога S7
  w = ZipWriter(tmp)
  w.add("manifest.json", m.dump())             // первой записью (ADR-0035 п. 2)
  for s in staged: w.add_stream(s)              // потоковый SHA-256 сверяется с объявленным
  for e in old.entries if e not removed:
    if e.opaque and not safe_to_preserve(e): abort(read_only)   // O-15: extra fields/смещения/флаги; без silent drop
    w.copy_raw(e)                               // в т. ч. неизвестные записи; CRC сверяется по распакованному потоку
  w.close(); fsync(tmp)
  verify(tmp, level = full)                     // V1–V4: CRC + SHA-256 всех зарегистрированных потоков (ADR-0035 п. 7);
                                                // opaque — не выше verified_crc_size_only (O-15); failed → abort, исходник не тронут
  journal.set(verified)
  if stat(asset.zip) != base: abort("изменён извне")    // ТЗ 8.7.8, внешнее изменение
  replace(asset.zip, tmp, backup)                       // ReplaceFileW / rename
  journal.set(committed)
  index.upsert(m); journal.set(indexed); delete backup; journal.drop(op)
```

## 4. Сессия внешнего редактирования

1. `session.open(asset, files[], app?)` → создаётся `workdir = <lib>/.am/sessions/<8hex>/` (короткий корень — из-за путей длиннее 260 символов), файлы извлекаются **с сохранением относительных путей** вместе с зависимыми (OBJ→MTL→текстуры через `IDependencyResolver` формата). В `edit_session_files` записывается снимок: путь, размер, mtime и SHA-256.
2. Запуск приложения: `ShellExecuteExW` / `xdg-open` (ассоциация) или пользовательская команда. Завершение процесса **не закрывает** сессию (ТЗ 8.7.6).
3. «Проверить изменения» (по кнопке или по событию efsw с debounce 2 с): diff по `(size, mtime)` → SHA-256 только у изменившихся → список `modified / added / deleted`. Пользователь может добавить файлы из любого места (ТЗ 8.7.4).
4. «Применить выбранное» → `commit_update` (раздел 3) с записью `history.op = session_commit`. Перед этим проверяется, что `asset.revision == base_rev`. Если ревизия изменилась, выдаётся `needs_decision` («ассет изменён после открытия сессии»).
5. «Завершить сессию» → `state=committed|abandoned`, workdir удаляется **только** если в нём нет неприменённых изменений либо пользователь подтвердил их потерю (ТЗ 8.8.3).
6. При старте программы находятся сессии `active/reviewing` → уведомление «Есть незавершённые сессии (N)» с действиями восстановления.
