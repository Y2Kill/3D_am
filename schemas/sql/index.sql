-- index.sqlite — ПЕРЕСТРАИВАЕМЫЙ индекс библиотеки (ADR-0007).
-- Можно удалить в любой момент: `am library rebuild` восстановит его из ZIP-ассетов.
-- Поля следуют манифесту формата 1.0 (ADR-0035, docs/specifications/container_manifest_v1.md).
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA user_version = 1;

CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);

-- Одна строка на ZIP-ассет, найденный в библиотеке
CREATE TABLE assets (
  asset_id        TEXT PRIMARY KEY,               -- UUIDv4 (ADR-0035)
  revision        INTEGER NOT NULL,
  rel_path        TEXT NOT NULL UNIQUE,           -- assets/xx/<uuid>.zip  или trash/<uuid>.zip
  in_trash        INTEGER NOT NULL DEFAULT 0,
  zip_size        INTEGER NOT NULL,
  zip_mtime_ns    INTEGER NOT NULL,               -- быстрая сверка изменений (ТЗ 4.9.2)
  manifest_sha256 TEXT NOT NULL,
  format_major    INTEGER NOT NULL,
  format_minor    INTEGER NOT NULL,
  title           TEXT,
  description     TEXT,
  created_at      TEXT NOT NULL,
  modified_at     TEXT NOT NULL,
  incomplete      INTEGER NOT NULL DEFAULT 0,
  health          TEXT NOT NULL DEFAULT 'ok',     -- ok | warn | corrupt | unsupported | missing
  health_detail   TEXT,
  primary_preview TEXT,                           -- FileID файла previews.primary_id
  file_count      INTEGER NOT NULL DEFAULT 0,
  content_bytes   INTEGER NOT NULL DEFAULT 0,
  formats         TEXT,                           -- JSON массив id типов
  manifest_json   TEXT NOT NULL                   -- полная копия для запросов json_extract
);
CREATE INDEX assets_title ON assets(title COLLATE NOCASE);
CREATE INDEX assets_created ON assets(created_at);

-- Текущие файлы: только files[] манифеста (§19.4.4). Размеры в манифесте — строки uint64,
-- здесь INTEGER (int64 SQLite покрывает реальные размеры файлов).
CREATE TABLE files (
  asset_id     TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  file_id      TEXT NOT NULL,                     -- FileID, UUIDv4
  storage_path TEXT NOT NULL,
  name         TEXT NOT NULL,                     -- basename storage_path (для поиска)
  size         INTEGER NOT NULL,
  sha256       TEXT NOT NULL,
  origin_kind  TEXT NOT NULL,                     -- imported | user_added | generated
  type_id      TEXT,
  PRIMARY KEY (asset_id, file_id)
);
CREATE INDEX files_sha ON files(sha256);
CREATE INDEX files_name_size ON files(name COLLATE NOCASE, size);

-- История импорта: inputs[].entries[] (§19.4.3) — исходные имена, CRC и судьба каждой записи.
-- file_id исторический и может указывать на удалённый файл.
CREATE TABLE input_entries (
  asset_id   TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  entry_id   TEXT NOT NULL,                       -- SourceEntryID, UUIDv4
  input_id   TEXT NOT NULL,
  object_id  TEXT NOT NULL,
  src_path   TEXT NOT NULL,
  src_name   TEXT NOT NULL,                       -- basename исходного пути (для поиска)
  size       INTEGER,
  crc32      TEXT,
  result     TEXT NOT NULL,                       -- included | included_renamed | excluded | unavailable | failed | not_processed
  file_id    TEXT,
  PRIMARY KEY (asset_id, entry_id)
);
CREATE INDEX input_entries_name_size ON input_entries(src_name COLLATE NOCASE, size);
CREATE INDEX input_entries_crc_size ON input_entries(crc32, size) WHERE crc32 IS NOT NULL;

-- Входные объекты (архивы, тома, файлы) — «видели ли мы этот архив?» (ТЗ 6.3): inputs[].objects[]
CREATE TABLE input_objects (
  asset_id      TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  object_id     TEXT NOT NULL,                    -- InputObjectID, UUIDv4
  input_id      TEXT NOT NULL,
  kind          TEXT NOT NULL,                    -- file | directory | archive | multivolume_archive
  volume_index  INTEGER NOT NULL DEFAULT -1,      -- -1 = не том
  original_name TEXT NOT NULL,
  size          INTEGER,
  sha256        TEXT,
  PRIMARY KEY (asset_id, object_id, volume_index)
);
CREATE INDEX input_objects_sha ON input_objects(sha256);
CREATE INDEX input_objects_name_size ON input_objects(original_name COLLATE NOCASE, size);

CREATE TABLE sources (
  asset_id  TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  source_id TEXT NOT NULL,
  kind      TEXT NOT NULL,
  name      TEXT,
  url       TEXT,
  PRIMARY KEY (asset_id, source_id)
);
CREATE INDEX sources_name ON sources(name COLLATE NOCASE);

CREATE TABLE asset_tags (
  asset_id  TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  tag       TEXT NOT NULL,                        -- нормализованная форма; исходное написание — в манифесте
  PRIMARY KEY (asset_id, tag)
);
CREATE INDEX asset_tags_tag ON asset_tags(tag);

CREATE TABLE asset_categories (
  asset_id TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  path     TEXT NOT NULL,                         -- "Фигурки/Аниме"
  PRIMARY KEY (asset_id, path)
);
CREATE INDEX asset_categories_path ON asset_categories(path);

-- Значения custom_values[] (EAV для фильтров; §19.4.10.7 — производный индекс)
CREATE TABLE asset_fields (
  asset_id   TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  scope      TEXT NOT NULL,                       -- asset | group | file
  owner_id   TEXT NOT NULL,                       -- AssetID | GroupID | FileID
  field_id   TEXT NOT NULL,                       -- user:<uuid> | plugin:<id>:<key>
  num        REAL,                                -- для фильтров; точное значение — txt (строки int64/decimal)
  txt        TEXT,
  unit       TEXT,
  PRIMARY KEY (asset_id, scope, owner_id, field_id)
);
CREATE INDEX asset_fields_num ON asset_fields(field_id, num);
CREATE INDEX asset_fields_txt ON asset_fields(field_id, txt);

-- Полнотекст: название, описание, авторы, теги, источники
CREATE VIRTUAL TABLE fts_assets USING fts5(asset_id UNINDEXED, title, description, authors, tags, sources, tokenize = 'unicode61 remove_diacritics 2');
-- Подстрочный поиск по именам файлов/архивов (ТЗ 10.4.1, критерий 62)
CREATE VIRTUAL TABLE fts_names USING fts5(asset_id UNINDEXED, name, tokenize = 'trigram');
-- Текст документов (плагин, ТЗ 10.5)
CREATE VIRTUAL TABLE fts_docs USING fts5(asset_id UNINDEXED, file_id UNINDEXED, body, tokenize = 'unicode61 remove_diacritics 2');

-- Признаки для поиска похожих (ТЗ 7)
CREATE TABLE image_hashes (
  asset_id TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  ref      TEXT NOT NULL,                         -- file_id или preview id
  sha256   TEXT NOT NULL,
  phash    INTEGER NOT NULL,                      -- 64 бита
  dhash    INTEGER NOT NULL,
  w INTEGER, h INTEGER,
  ph0 INTEGER GENERATED ALWAYS AS ((phash >>  0) & 65535) VIRTUAL,  -- multi-index hashing
  ph1 INTEGER GENERATED ALWAYS AS ((phash >> 16) & 65535) VIRTUAL,
  ph2 INTEGER GENERATED ALWAYS AS ((phash >> 32) & 65535) VIRTUAL,
  ph3 INTEGER GENERATED ALWAYS AS ((phash >> 48) & 65535) VIRTUAL,
  PRIMARY KEY (asset_id, ref)
);
CREATE INDEX ih0 ON image_hashes(ph0);
CREATE INDEX ih1 ON image_hashes(ph1);
CREATE INDEX ih2 ON image_hashes(ph2);
CREATE INDEX ih3 ON image_hashes(ph3);

CREATE TABLE mesh_features (
  asset_id   TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  file_id    TEXT NOT NULL,
  sha256     TEXT NOT NULL,                       -- кэш по содержимому: пересчёт не нужен при переносе
  triangles  INTEGER,
  area       REAL, volume REAL,
  ext_sorted TEXT,                                -- JSON [a,b,c] габариты после PCA, по убыванию
  d2_hist    BLOB,                                -- 64 x uint8
  geom_fp    TEXT,                                -- хеш квантованной геометрии (инвариант порядка треугольников)
  algo_ver   INTEGER NOT NULL,
  PRIMARY KEY (asset_id, file_id)
);
CREATE INDEX mesh_fp ON mesh_features(geom_fp);

-- MinHash по множеству sha256 файлов ассета (быстрое частичное пересечение комплектов)
CREATE TABLE asset_minhash (
  asset_id TEXT PRIMARY KEY REFERENCES assets(asset_id) ON DELETE CASCADE,
  sig      BLOB NOT NULL                          -- 128 x uint32
);

-- Предложения последующего анализа (ADR-0028, ТЗ §11.2.7): воспроизводимые данные,
-- не входят в ZIP и не меняют ревизию. content_key = хеш входных данных обработчика.
CREATE TABLE suggestions (
  id              INTEGER PRIMARY KEY,
  asset_id        TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  target          TEXT NOT NULL,                  -- claim target: "/tags" | "/categories" | "/custom_values/<field_id>" | "/license" | ...
  value_json      TEXT NOT NULL,
  handler         TEXT NOT NULL,
  handler_version TEXT NOT NULL,
  basis           TEXT,
  confidence      TEXT,                           -- decimal-строка 0..1, только для методов с осмысленной оценкой
  content_key     TEXT NOT NULL,
  created_at      TEXT NOT NULL,
  state           TEXT NOT NULL DEFAULT 'open'    -- open | dismissed (подтверждённое уходит в ZIP как claim)
);
CREATE INDEX suggestions_asset ON suggestions(asset_id, target);
