-- index.sqlite — ПЕРЕСТРАИВАЕМЫЙ индекс библиотеки (ADR-0007).
-- Можно удалить в любой момент: `am library rebuild` восстановит его из ZIP-ассетов.
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA user_version = 1;

CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);

-- Одна строка на ZIP-ассет, найденный в библиотеке
CREATE TABLE assets (
  asset_id        TEXT PRIMARY KEY,               -- UUIDv7
  revision        INTEGER NOT NULL,
  rel_path        TEXT NOT NULL UNIQUE,           -- assets/xx/<uuid>.zip  или trash/<uuid>.zip
  in_trash        INTEGER NOT NULL DEFAULT 0,
  zip_size        INTEGER NOT NULL,
  zip_mtime_ns    INTEGER NOT NULL,               -- быстрая сверка изменений (ТЗ 4.9.2)
  manifest_sha256 TEXT NOT NULL,
  format_version  INTEGER NOT NULL,
  title           TEXT,
  description     TEXT,
  created_at      TEXT NOT NULL,
  modified_at     TEXT,
  incomplete      INTEGER NOT NULL DEFAULT 0,
  health          TEXT NOT NULL DEFAULT 'ok',     -- ok | warn | corrupt | unsupported | missing
  health_detail   TEXT,
  primary_preview TEXT,                           -- zip_path
  file_count      INTEGER NOT NULL DEFAULT 0,
  content_bytes   INTEGER NOT NULL DEFAULT 0,
  formats         TEXT,                           -- JSON массив id типов
  manifest_json   TEXT NOT NULL                   -- полная копия для запросов json_extract
);
CREATE INDEX assets_title ON assets(title COLLATE NOCASE);
CREATE INDEX assets_created ON assets(created_at);

-- Записи files[] (включённые и исключённые)
CREATE TABLE files (
  asset_id   TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  file_id    TEXT NOT NULL,                       -- f00001
  input_id   TEXT,
  status     TEXT NOT NULL,                       -- included | included_renamed | excluded | unreadable
  zip_path   TEXT,
  src_path   TEXT NOT NULL,
  src_name   TEXT NOT NULL,                       -- basename исходного пути (для поиска)
  size       INTEGER,
  sha256     TEXT,
  crc32      TEXT,
  type_id    TEXT,
  PRIMARY KEY (asset_id, file_id)
);
CREATE INDEX files_sha ON files(sha256) WHERE sha256 IS NOT NULL;
CREATE INDEX files_name_size ON files(src_name COLLATE NOCASE, size);
CREATE INDEX files_crc_size ON files(crc32, size) WHERE crc32 IS NOT NULL;

-- Входные объекты (архивы, тома, файлы) — «видели ли мы этот архив?» (ТЗ 6.3)
CREATE TABLE inputs (
  asset_id      TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  input_id      TEXT NOT NULL,
  kind          TEXT NOT NULL,
  volume_index  INTEGER NOT NULL DEFAULT -1,      -- -1 = не том
  original_name TEXT NOT NULL,
  size          INTEGER,
  sha256        TEXT,
  PRIMARY KEY (asset_id, input_id, volume_index)
);
CREATE INDEX inputs_sha ON inputs(sha256);
CREATE INDEX inputs_name_size ON inputs(original_name COLLATE NOCASE, size);

CREATE TABLE sources (
  asset_id  TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  source_id TEXT NOT NULL,
  kind      TEXT NOT NULL,
  name      TEXT,
  url       TEXT,
  confirmed INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (asset_id, source_id)
);
CREATE INDEX sources_name ON sources(name COLLATE NOCASE);

CREATE TABLE asset_tags (
  asset_id  TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  tag       TEXT NOT NULL,                        -- нормализованная форма
  confirmed INTEGER NOT NULL,
  score     REAL,
  PRIMARY KEY (asset_id, tag)
);
CREATE INDEX asset_tags_tag ON asset_tags(tag, confirmed);

CREATE TABLE asset_categories (
  asset_id TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  path     TEXT NOT NULL,                         -- "Фигурки/Аниме"
  PRIMARY KEY (asset_id, path)
);
CREATE INDEX asset_categories_path ON asset_categories(path);

-- Типизированные характеристики и пользовательские поля (EAV для фильтров)
CREATE TABLE asset_fields (
  asset_id TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  key      TEXT NOT NULL,                         -- characteristics.scale | user.printed | x.plugin.key
  num      REAL,
  txt      TEXT,
  unit     TEXT,
  PRIMARY KEY (asset_id, key)
);
CREATE INDEX asset_fields_num ON asset_fields(key, num);
CREATE INDEX asset_fields_txt ON asset_fields(key, txt);

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
  target          TEXT NOT NULL,                  -- "tag" | "category" | "characteristic:<key>" | "license" | ...
  value_json      TEXT NOT NULL,
  handler         TEXT NOT NULL,
  handler_version TEXT NOT NULL,
  basis           TEXT,
  confidence      REAL,                           -- только для методов с осмысленной оценкой
  content_key     TEXT NOT NULL,
  created_at      TEXT NOT NULL,
  state           TEXT NOT NULL DEFAULT 'open'    -- open | dismissed (подтверждённое уходит в ZIP)
);
CREATE INDEX suggestions_asset ON suggestions(asset_id, target);
