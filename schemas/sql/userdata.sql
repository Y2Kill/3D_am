-- userdata.sqlite — НЕВОССТАНАВЛИВАЕМЫЕ пользовательские данные библиотеки (ADR-0007).
-- Входит в резервное копирование (VACUUM INTO). Не перестраивается из ZIP.
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA foreign_keys = ON;
PRAGMA user_version = 1;

-- Реестр межассетных связей и заключений (ТЗ 7.6)
CREATE TABLE relations (
  rel_id      TEXT PRIMARY KEY,                   -- UUIDv7
  a_asset     TEXT NOT NULL, a_rev INTEGER NOT NULL, a_ref TEXT,   -- ref: NULL=ассет | file_id | group_id
  b_asset     TEXT NOT NULL, b_rev INTEGER NOT NULL, b_ref TEXT,
  kind        TEXT NOT NULL,                      -- duplicate | related | not_duplicate | deferred | scale_of | variant_of | addon_of
  status      TEXT NOT NULL DEFAULT 'current',    -- current | stale (ревизия изменилась)
  note        TEXT,
  evidence    TEXT,                               -- JSON: методы и оценки на момент решения
  created_at  TEXT NOT NULL,
  updated_at  TEXT NOT NULL,
  CHECK (a_asset < b_asset OR (a_asset = b_asset AND ifnull(a_ref,'') < ifnull(b_ref,'')))
);
CREATE UNIQUE INDEX relations_pair ON relations(a_asset, ifnull(a_ref,''), b_asset, ifnull(b_ref,''), kind);
CREATE INDEX relations_b ON relations(b_asset);

-- Виртуальные коллекции и избранное (ТЗ 10.3.3)
CREATE TABLE collections (
  coll_id    TEXT PRIMARY KEY,
  name       TEXT NOT NULL,
  parent_id  TEXT REFERENCES collections(coll_id) ON DELETE CASCADE,
  kind       TEXT NOT NULL DEFAULT 'manual',      -- manual | favorites
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL
);
CREATE TABLE collection_items (
  coll_id  TEXT NOT NULL REFERENCES collections(coll_id) ON DELETE CASCADE,
  asset_id TEXT NOT NULL,                         -- без FK: ассет может быть в корзине или временно отсутствовать
  added_at TEXT NOT NULL,
  position INTEGER,
  PRIMARY KEY (coll_id, asset_id)
);

-- Сохранённые поиски (ТЗ 10.4.5)
CREATE TABLE saved_searches (
  search_id  TEXT PRIMARY KEY,
  name       TEXT NOT NULL,
  query_json TEXT NOT NULL,                       -- условия, фильтры, область, сортировка, вид
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

-- Словарь тегов, синонимы (ТЗ 10.6.3, 10.6.6)
CREATE TABLE tag_dict (
  tag        TEXT PRIMARY KEY,                    -- каноническая нормализованная форма
  display    TEXT NOT NULL,
  color      TEXT,
  created_at TEXT NOT NULL
);
CREATE TABLE tag_aliases (
  alias TEXT PRIMARY KEY,
  tag   TEXT NOT NULL REFERENCES tag_dict(tag) ON UPDATE CASCADE ON DELETE CASCADE
);

-- Смысловые отношения тегов (ADR-0028, ТЗ §11.2.5): применяются только после решения пользователя.
CREATE TABLE tag_relations (
  tag_a    TEXT NOT NULL REFERENCES tag_dict(tag) ON UPDATE CASCADE ON DELETE CASCADE,
  tag_b    TEXT NOT NULL REFERENCES tag_dict(tag) ON UPDATE CASCADE ON DELETE CASCADE,
  kind     TEXT NOT NULL,                         -- synonym | related | parent (tag_a — родитель tag_b)
  status   TEXT NOT NULL DEFAULT 'proposed',      -- proposed | accepted | rejected
  source   TEXT,                                  -- user | handler id
  created_at TEXT NOT NULL,
  PRIMARY KEY (tag_a, tag_b, kind)
);

-- Справочник категорий библиотеки (ТЗ 5.7.2)
CREATE TABLE category_tree (
  path        TEXT PRIMARY KEY,                   -- "Фигурки/Аниме"
  display     TEXT NOT NULL,
  sort_order  INTEGER NOT NULL DEFAULT 0
);

-- Определения пользовательских полей (ADR-0013, потеря П-1)
CREATE TABLE field_defs (
  key        TEXT PRIMARY KEY,                    -- user.<key>
  label      TEXT NOT NULL,
  type       TEXT NOT NULL,                       -- string | text | int | real | bool | date | enum | url
  unit       TEXT,
  enum_json  TEXT,
  created_at TEXT NOT NULL
);

-- Сохранённое состояние ревью дубликатов (ТЗ 7.7.5)
CREATE TABLE dedup_reviews (
  review_id  TEXT PRIMARY KEY,
  name       TEXT,
  params     TEXT NOT NULL,
  cursor     TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
