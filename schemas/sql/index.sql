-- index.sqlite — ПЕРЕСТРАИВАЕМЫЙ индекс библиотеки (ADR-0007).
-- Можно удалить в любой момент: `am library rebuild` восстановит его из ZIP-ассетов.
-- Поля следуют манифесту формата 1.0 (ADR-0035, docs/specifications/container_manifest_v1.md).
--
-- Размеры uint64 (O-16, ADR-0037 п. 10): канонический десятичный TEXT COLLATE BINARY с CHECK
-- (только цифры, без ведущих нулей, не длиннее 20, не больше 18446744073709551615).
-- NULL = неизвестно, '0' = известный ноль. Порядок и сравнение — по (length(x), x);
-- CAST, SUM(), TOTAL() и double запрещены: точные суммы считаются в C++ с контролем переполнения.
-- Перестроение индекса при смене схемы не меняет ZIP и его ревизию.
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
  zip_size        TEXT COLLATE BINARY NOT NULL    -- размер ZIP на ФС, uint64 (O-16)
                  CHECK (typeof(zip_size) = 'text' AND length(zip_size) BETWEEN 1 AND 20 AND zip_size NOT GLOB '*[^0-9]*'
                         AND (zip_size = '0' OR substr(zip_size, 1, 1) <> '0')
                         AND (length(zip_size) < 20 OR zip_size <= '18446744073709551615')),
  zip_mtime_ns    INTEGER NOT NULL,               -- быстрая сверка изменений (ТЗ 4.9.2)
  manifest_sha256 TEXT NOT NULL,
  format_major    INTEGER NOT NULL,
  format_minor    INTEGER NOT NULL,
  title           TEXT,
  description     TEXT,
  created_at      TEXT NOT NULL,
  modified_at     TEXT NOT NULL,
  incomplete      INTEGER NOT NULL DEFAULT 0,     -- производный: 1, если есть Input с completeness IN ('incomplete','unknown') (X-01, K-2)
  health          TEXT NOT NULL DEFAULT 'ok',     -- ok | warn | corrupt | unsupported | missing
  health_detail   TEXT,                           -- диагностики, например content_bytes_overflow (K-3)
  primary_preview TEXT,                           -- FileID файла previews.primary_id
  file_count      INTEGER NOT NULL DEFAULT 0,
  content_bytes   TEXT COLLATE BINARY             -- сумма files[].size_bytes (K-3): считается в C++; NULL — сумма вне uint64 (+ диагностика) или неизвестна
                  CHECK (content_bytes IS NULL OR (typeof(content_bytes) = 'text' AND length(content_bytes) BETWEEN 1 AND 20
                         AND content_bytes NOT GLOB '*[^0-9]*' AND (content_bytes = '0' OR substr(content_bytes, 1, 1) <> '0')
                         AND (length(content_bytes) < 20 OR content_bytes <= '18446744073709551615'))),
  formats         TEXT,                           -- JSON массив id типов
  manifest_json   TEXT NOT NULL                   -- полная копия для запросов json_extract
);
CREATE INDEX assets_title ON assets(title COLLATE NOCASE);
CREATE INDEX assets_created ON assets(created_at);

-- Текущие файлы: только files[] манифеста (§19.4.4). Размер — точная строка uint64 (O-16).
CREATE TABLE files (
  asset_id     TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  file_id      TEXT NOT NULL,                     -- FileID, UUIDv4
  storage_path TEXT NOT NULL,
  name         TEXT NOT NULL,                     -- basename storage_path (для поиска)
  size         TEXT COLLATE BINARY NOT NULL
               CHECK (typeof(size) = 'text' AND length(size) BETWEEN 1 AND 20 AND size NOT GLOB '*[^0-9]*'
                      AND (size = '0' OR substr(size, 1, 1) <> '0')
                      AND (length(size) < 20 OR size <= '18446744073709551615')),
  sha256       TEXT NOT NULL,
  origin_kind  TEXT NOT NULL,                     -- imported | user_added | generated
  type_id      TEXT,
  PRIMARY KEY (asset_id, file_id)
);
CREATE INDEX files_sha ON files(sha256);
CREATE INDEX files_name_size ON files(name COLLATE NOCASE, length(size), size);

-- История импорта: inputs[].entries[] (§19.4.3) — исходные имена, CRC и судьба каждой записи.
-- file_id исторический и может указывать на удалённый файл.
CREATE TABLE input_entries (
  asset_id   TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  entry_id   TEXT NOT NULL,                       -- SourceEntryID, UUIDv4
  input_id   TEXT NOT NULL,
  object_id  TEXT NOT NULL,
  src_path   TEXT NOT NULL,
  src_name   TEXT NOT NULL,                       -- basename исходного пути (для поиска)
  entry_kind TEXT NOT NULL,                       -- file | directory (X-01)
  size       TEXT COLLATE BINARY                  -- NULL = размер неизвестен (O-16)
             CHECK (size IS NULL OR (typeof(size) = 'text' AND length(size) BETWEEN 1 AND 20 AND size NOT GLOB '*[^0-9]*'
                    AND (size = '0' OR substr(size, 1, 1) <> '0')
                    AND (length(size) < 20 OR size <= '18446744073709551615'))),
  crc32      TEXT,
  result     TEXT NOT NULL,                       -- included | included_renamed | excluded | unavailable | failed | not_processed
  file_id    TEXT,
  PRIMARY KEY (asset_id, entry_id)
);
CREATE INDEX input_entries_name_size ON input_entries(src_name COLLATE NOCASE, length(size), size);
CREATE INDEX input_entries_crc_size ON input_entries(crc32, length(size), size) WHERE crc32 IS NOT NULL;

-- Входные объекты (архивы, тома, файлы) — «видели ли мы этот архив?» (ТЗ 6.3): inputs[].objects[]
CREATE TABLE input_objects (
  asset_id      TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  object_id     TEXT NOT NULL,                    -- InputObjectID, UUIDv4
  input_id      TEXT NOT NULL,
  kind          TEXT NOT NULL,                    -- file | directory | archive | multivolume_archive
  volume_index  INTEGER NOT NULL DEFAULT -1,      -- -1 = не том; иначе volumes[].index (с 0, X-01)
  original_name TEXT NOT NULL,
  size          TEXT COLLATE BINARY               -- NULL = размер неизвестен (O-16)
                CHECK (size IS NULL OR (typeof(size) = 'text' AND length(size) BETWEEN 1 AND 20 AND size NOT GLOB '*[^0-9]*'
                       AND (size = '0' OR substr(size, 1, 1) <> '0')
                       AND (length(size) < 20 OR size <= '18446744073709551615'))),
  sha256        TEXT,
  PRIMARY KEY (asset_id, object_id, volume_index)
);
CREATE INDEX input_objects_sha ON input_objects(sha256);
CREATE INDEX input_objects_name_size ON input_objects(original_name COLLATE NOCASE, length(size), size);

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
  tag       TEXT NOT NULL,                        -- поисковый ключ (ADR-0036 п. 5, алгоритм — O-08); написание в ZIP не меняется
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
  type_id    TEXT,                                -- тип поля из field_definitions (базовый или plugin TypeID)
  num        REAL,                                -- для фильтров; точное значение — txt. Для plugin TypeID не заполняется
  txt        TEXT,                                -- для plugin TypeID — сырой JSON только для отображения (ADR-0036 п. 6)
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
-- не входят в ZIP и не меняют ревизию. id — локальный rowid, снаружи на него не ссылаются.
-- Решение Dismiss хранится в userdata.sqlite: suggestion_decisions (ADR-0036 п. 3);
-- state здесь — производный статус, вычисляемый при отображении/переиндексации.
CREATE TABLE suggestions (
  id              INTEGER PRIMARY KEY,
  asset_id        TEXT NOT NULL REFERENCES assets(asset_id) ON DELETE CASCADE,
  owner_scope     TEXT NOT NULL,                  -- asset | group | file
  owner_id        TEXT NOT NULL,                  -- AssetID | GroupID | FileID
  target_json     TEXT NOT NULL,                  -- типизированный claims.target: {"kind":"standard","field":"tags"} | {"kind":"custom","field_id":...}
  value_json      TEXT NOT NULL,
  handler         TEXT NOT NULL,
  handler_version TEXT NOT NULL,
  basis           TEXT,
  confidence      TEXT,                           -- decimal-строка 0..1, только для методов с осмысленной оценкой
  content_key     TEXT NOT NULL,                  -- прообраз basis_key
  candidate_key   TEXT NOT NULL,                  -- (AssetID, адресат, значение); алгоритм — O-03
  basis_key       TEXT NOT NULL,                  -- основание; алгоритм — O-03
  created_at      TEXT NOT NULL,
  state           TEXT NOT NULL DEFAULT 'open'    -- open | dismissed — ПРОИЗВОДНЫЙ от userdata.suggestion_decisions
);
CREATE INDEX suggestions_asset ON suggestions(asset_id, owner_scope, owner_id);
CREATE INDEX suggestions_candidate ON suggestions(asset_id, candidate_key);
