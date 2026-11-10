# Задания, безопасная фиксация и сессии редактирования

Связано с ADR-0006, ADR-0015 и ТЗ 4.6, 6.7, 8.7–8.8, 10.10. Схема БД: `schemas/sql/state.sql`.

## 1. Модель задания

```
          ┌──────── pause ───────┐
queued ──► running ──────────────► paused ──resume──► queued
   ▲          │  │  │
   │          │  │  └─► needs_decision ──(ответ пользователя)──► queued
 retry        │  └────► failed ──retry──┘
   │          └───────► succeeded
   └── cancelled ◄──── (cancel из любого незавершённого состояния)
```

- **Безопасные точки** (ТЗ 6.7.2): задание проверяет `stop_token` между файлами, но **никогда** не во время фазы commit протокола ADR-0006. Пауза — это «завершить текущий файл и остановиться». Отмена в фазе записи temp-ZIP удаляет temp-файл.
- **needs_decision** (ТЗ 10.10.5): задание сериализует вопрос в `decision_json` (`{kind: "duplicate"|"boundary"|"partial_import"|"id_conflict", options:[…], context:{…}}`), освобождает поток и не блокирует остальные. Счётчик таких заданий виден в индикаторе строки состояния (ADR-0017).
- **Иерархия:** пакетный импорт = родительское задание `import.batch` + дочерние `import.asset` (по одному на предполагаемый ассет). Прогресс родителя — агрегат по дочерним.
- **Восстановление после аварии:** при старте задания в состоянии `running` переводятся в `queued` с `attempt+1`, но сначала выполняется восстановление `pending_ops` (протокол ADR-0006). Повтор идемпотентен: импорт проверяет, существует ли ассет с `history[0].op_id == job.op_id`.
- **Пулы:** `io_pool` (лимит на физическое устройство: HDD — 1, SSD — 4, сеть — 2; определяется через `IOCTL_STORAGE_QUERY_PROPERTY` / `/sys/block/*/queue/rotational`, переопределяется настройками) и `cpu_pool` (`hardware_concurrency - 1`). Этап задания объявляет свой тип ресурса.

## 2. Интерфейсы (C++, ядро)

```cpp
namespace am::jobs {
struct Progress { std::optional<std::uint64_t> done, total; std::string unit, stage; };
class JobContext {                        // передаётся в Job::run
 public:
  std::stop_token stop() const;
  void report(const Progress&);
  void log(Level, std::string_view code, std::string_view msg);
  // Перевести в needs_decision и вернуть управление; run() будет вызван снова после ответа
  [[nodiscard]] DecisionTicket ask(Decision);
  ResourceLease acquire(ResourceKind, const DeviceId&);
};
class Job { public: virtual ~Job() = default; virtual JobOutcome run(JobContext&) = 0; };
class JobFactory { public: virtual std::unique_ptr<Job> create(std::string_view type, const Json& params) = 0; };
}
```

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
