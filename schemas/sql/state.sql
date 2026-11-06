-- state.sqlite — ОПЕРАЦИОННОЕ состояние библиотеки: журнал фиксаций, очередь заданий, сессии (ADR-0006, 0015).
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA foreign_keys = ON;
PRAGMA user_version = 1;

-- Журнал незавершённых операций записи ZIP (протокол ADR-0006)
CREATE TABLE pending_ops (
  op_id        TEXT PRIMARY KEY,
  asset_id     TEXT NOT NULL,
  kind         TEXT NOT NULL,                     -- create | update | trash | restore | purge | import_ready | move (ADR-0025)
  target_path  TEXT NOT NULL,
  temp_path    TEXT,
  backup_path  TEXT,
  base_rev     INTEGER,
  base_size    INTEGER, base_mtime_ns INTEGER, base_manifest_sha256 TEXT,
  state        TEXT NOT NULL,                     -- prepared | verified | committed | indexed;
                                                  -- move: copying | copied | verified | registered | source_removed
  target_library_id TEXT,                         -- только для move (ADR-0025 п. 9)
  expected_rev INTEGER,                           -- ChangeRequest.expected_rev (ADR-0025 п. 5)
  started_at   TEXT NOT NULL,
  updated_at   TEXT NOT NULL,
  job_id       TEXT
);

-- Активные снимки чтения для режима «поколений» (ADR-0025 п. 3): старая ревизия
-- .am/gen/<uuid>.<rev>.zip удаляется, когда на неё нет снимков. При старте таблица
-- очищается (процессы-читатели завершены) и ненужные поколения удаляются.
CREATE TABLE snapshots (
  asset_id   TEXT NOT NULL,
  revision   INTEGER NOT NULL,
  gen_path   TEXT,                                  -- NULL = читается действующий <uuid>.zip
  holders    INTEGER NOT NULL DEFAULT 0,
  pid        INTEGER NOT NULL,
  opened_at  TEXT NOT NULL,
  PRIMARY KEY (asset_id, revision, pid)
);

-- Очередь фоновых заданий (ТЗ 6.7, 9.5.3, 10.10)
CREATE TABLE jobs (
  job_id        TEXT PRIMARY KEY,
  parent_id     TEXT REFERENCES jobs(job_id) ON DELETE CASCADE,   -- пакет → подзадания
  type          TEXT NOT NULL,                    -- import.scan | import.asset | convert | thumbs | verify | reanalyze | bulk.meta | ...
  params_json   TEXT NOT NULL,
  state         TEXT NOT NULL,                    -- queued | running | paused | needs_decision | succeeded | failed | cancelled
  priority      INTEGER NOT NULL DEFAULT 0,
  position      INTEGER NOT NULL,
  attempt       INTEGER NOT NULL DEFAULT 0,
  progress_done INTEGER, progress_total INTEGER, progress_unit TEXT,   -- NULL total = неопределённый прогресс
  stage         TEXT,
  decision_json TEXT,                             -- вопрос пользователю и варианты (needs_decision)
  result_json   TEXT,                             -- созданные/обновлённые asset_id, предупреждения
  error_json    TEXT,
  created_at    TEXT NOT NULL,
  started_at    TEXT, finished_at TEXT
);
CREATE INDEX jobs_state ON jobs(state, priority DESC, position);
CREATE INDEX jobs_parent ON jobs(parent_id);

-- Журнал операций обработки (ТЗ 6.7.5, не равен history внутри ZIP)
CREATE TABLE op_log (
  seq       INTEGER PRIMARY KEY AUTOINCREMENT,
  at        TEXT NOT NULL,
  level     TEXT NOT NULL,                        -- info | warn | error
  job_id    TEXT,
  asset_id  TEXT,
  code      TEXT,
  message   TEXT NOT NULL,
  detail    TEXT
);
CREATE INDEX op_log_job ON op_log(job_id);

-- Сессии внешнего редактирования (ТЗ 8.7–8.8)
CREATE TABLE edit_sessions (
  session_id   TEXT PRIMARY KEY,
  asset_id     TEXT NOT NULL,
  base_rev     INTEGER NOT NULL,
  workdir      TEXT NOT NULL,                     -- <lib>/.am/sessions/<8hex>
  state        TEXT NOT NULL,                     -- active | reviewing | committed | abandoned
  app_hint     TEXT,
  created_at   TEXT NOT NULL,
  updated_at   TEXT NOT NULL
);
CREATE TABLE edit_session_files (
  session_id TEXT NOT NULL REFERENCES edit_sessions(session_id) ON DELETE CASCADE,
  rel_path   TEXT NOT NULL,                       -- путь в workdir
  file_id    TEXT,                                -- NULL для добавленных вручную
  size       INTEGER NOT NULL,
  mtime_ns   INTEGER NOT NULL,
  sha256     TEXT NOT NULL,
  PRIMARY KEY (session_id, rel_path)
);
