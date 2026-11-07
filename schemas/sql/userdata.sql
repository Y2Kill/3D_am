-- userdata.sqlite — НЕВОССТАНАВЛИВАЕМЫЕ пользовательские данные библиотеки (ADR-0007).
-- Входит в резервное копирование (VACUUM INTO). Не перестраивается из ZIP.
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA foreign_keys = ON;
PRAGMA user_version = 1;

-- Реестр межассетных связей и заключений (ТЗ 7.6)
CREATE TABLE relations (
  rel_id      TEXT PRIMARY KEY,                   -- UUIDv7 (вне ZIP, ADR-0035 п. 4)
  a_asset     TEXT NOT NULL, a_rev INTEGER NOT NULL, a_ref TEXT,   -- ref: NULL=ассет | FileID | GroupID (UUIDv4)
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

-- Каталог определений пользовательских полей библиотеки (ADR-0035 п. 9, потеря П-1).
-- Переносимое определение живёт в ZIP (field_definitions[]); здесь — справочник для GUI.
CREATE TABLE field_defs (
  field_id        TEXT PRIMARY KEY,               -- user:<uuid> | plugin:<id>:<key>
  schema_revision INTEGER NOT NULL,
  name            TEXT NOT NULL,
  type            TEXT NOT NULL,                  -- один из 11 базовых типов | plugin:<PluginID>:<TypeKey> (ADR-0036 п. 6)
  scopes          TEXT NOT NULL,                  -- JSON: ["asset","group","file"]
  dimension       TEXT,
  unit            TEXT,
  options_json    TEXT,                           -- [{id, label, deprecated?}]
  definition_json TEXT NOT NULL,                  -- полное определение как в манифесте
  created_at      TEXT NOT NULL,
  updated_at      TEXT NOT NULL
);

-- Решения пользователя по предложениям анализаторов (ADR-0036 п. 3, R-06). ЧЕРНОВОЙ КОНТРАКТ.
-- Источник истины для Dismiss: переживает перестроение index.sqlite и входит в резервную копию.
-- Не ссылается на index.sqlite (в том числе на suggestions.id — это перестраиваемый rowid).
-- Алгоритмы candidate_key/basis_key и канонизация значений открыты (O-03, O-04), поэтому
-- ограничений уникальности по ключам нет; поиск решений — по индексу ниже.
CREATE TABLE suggestion_decisions (
  decision_id   TEXT PRIMARY KEY,                 -- UUIDv7 (ID вне ZIP, ADR-0035 п. 4)
  asset_id      TEXT NOT NULL,                    -- AssetID; решение не удаляется, если ZIP временно недоступен
  owner_scope   TEXT NOT NULL,                    -- asset | group | file — владелец metadata адресата
  owner_id      TEXT NOT NULL,                    -- AssetID | GroupID | FileID
  target_json   TEXT NOT NULL,                    -- типизированный адресат claims.target (ADR-0036 п. 1)
  value_json    TEXT,                             -- отклонённое типизированное значение (NULL для scope = asset_field_value)
  candidate_key TEXT NOT NULL,                    -- хеш (AssetID, адресат, значение); алгоритм — O-03
  basis_key     TEXT,                             -- хеш основания (входные данные); алгоритм — O-03; NULL для asset_field_value
  decision      TEXT NOT NULL DEFAULT 'dismissed', -- dismissed (иные решения — при расширении модели)
  scope         TEXT NOT NULL DEFAULT 'current_basis', -- current_basis | asset_field_value
  handler       TEXT,                             -- анализатор и версия — только для диагностики, не часть ключа
  handler_version TEXT,
  created_at    TEXT NOT NULL,
  note          TEXT
);
CREATE INDEX suggestion_decisions_lookup ON suggestion_decisions(asset_id, candidate_key);

-- Сохранённое состояние ревью дубликатов (ТЗ 7.7.5)
CREATE TABLE dedup_reviews (
  review_id  TEXT PRIMARY KEY,
  name       TEXT,
  params     TEXT NOT NULL,
  cursor     TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
