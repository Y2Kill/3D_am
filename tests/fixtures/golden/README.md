# Golden ZIP — кандидат формата 1.0 (T-013)

Исходники: `tools/fixtures/golden/<case>/`. Генератор читает `manifest.json` как байты; только явные метки `@sha256:<ZIP-path>` и `@size:<ZIP-path>` заменяются SHA-256 и десятичным размером соответствующего payload из `case.json`. JSON не пересериализуется. Порядок записей задан массивом `entries`; время фиксировано 2020-01-01 00:00:00, UTF-8 bit 11 установлен даже для ASCII. Повреждения задаются `patches` в исходнике; меняются поля local/central заголовков, не payload. ZIP64 принудителен в local header (`force_zip64`), descriptor создаёт non-seekable поток.

`python -I tools/fixtures/make_golden_zips.py` — генерация; `--check` — регенерация в `.cache/` и побайтное сравнение `asset.zip` и `expected.json`; `--output` задаёт отдельную копию корпуса. Файлы ZIP не правятся вручную. Каждый ZIP не более 64 КиБ (гипотеза для размера репозитория).

Deflate использует уровень 0: это исключает различия эвристик zlib и zlib-ng при регенерации на разных платформах. Метод записи остаётся Deflate (8), полезные байты не меняются.

Ожидания задаются исходными `expected.json`, никогда не снимаются с проверяемой программы. Объекты сравниваются рекурсивным подмножеством, массивы — без порядка с взаимно однозначным сопоставлением; длина массива должна совпадать. ID и время не входят в шаблоны.

| Случай | Описание | validate | verify full |
|---|---|---:|---:|
| `neg-v1-duplicate-known` | V1 duplicate-known | 1 | 1 |
| `neg-v1-duplicate-opaque` | V1 duplicate-opaque | 1 | 1 |
| `neg-v1-leading-zero` | Invalid JSON number 01 | 1 | 1 |
| `neg-v1-missing-manifest` | Missing root manifest | 1 | 1 |
| `neg-v1-nan` | Invalid JSON number NaN | 1 | 1 |
| `neg-v1-plus` | Invalid JSON number +1 | 1 | 1 |
| `neg-v1-two-manifests` | Duplicate root manifests | 1 | 1 |
| `neg-v1-utf8` | V1 utf8 | 1 | 1 |
| `neg-v2-dimension` | V2 invalid dimension | 1 | 1 |
| `neg-v2-generation-tool` | V2 invalid generation-tool | 1 | 1 |
| `neg-v2-required` | V2 invalid required | 1 | 1 |
| `neg-v2-type` | V2 invalid type | 1 | 1 |
| `neg-v2-unevaluated` | V2 invalid unevaluated | 1 | 1 |
| `neg-v2-version` | V2 invalid version | 1 | 1 |
| `neg-v3-asset_times` | V3 asset_times | 1 | 1 |
| `neg-v3-byte_sizes_uint64` | V3 byte_sizes_uint64 | 1 | 1 |
| `neg-v3-change_kind_fields` | V3 change_kind_fields | 1 | 1 |
| `neg-v3-claim_evidence` | V3 claim_evidence | 1 | 1 |
| `neg-v3-claim_target_custom` | V3 claim_target_custom | 1 | 1 |
| `neg-v3-claim_target_item` | V3 claim_target_item | 1 | 1 |
| `neg-v3-claim_target_standard` | V3 claim_target_standard | 1 | 1 |
| `neg-v3-claim_value_type` | V3 claim_value_type | 1 | 1 |
| `neg-v3-claim_value_unit` | V3 claim_value_unit | 1 | 1 |
| `neg-v3-custom_value_field` | V3 custom_value_field | 1 | 1 |
| `neg-v3-dependency_refs` | V3 dependency_refs | 1 | 1 |
| `neg-v3-derived_from` | V3 derived_from | 1 | 1 |
| `neg-v3-field_constraints` | V3 field_constraints | 1 | 1 |
| `neg-v3-field_default` | V3 field_default | 1 | 1 |
| `neg-v3-field_default-physical` | Physical default missing unit (ADR-0039) | 1 | 1 |
| `neg-v3-field_options` | V3 field_options | 1 | 1 |
| `neg-v3-group_refs` | V3 group_refs | 1 | 1 |
| `neg-v3-historical_files` | V3 historical_files | 1 | 1 |
| `neg-v3-history_compacted_summary` | V3 history_compacted_summary | 1 | 1 |
| `neg-v3-history_coverage` | V3 history_coverage | 1 | 1 |
| `neg-v3-history_operation_id_unique` | V3 history_operation_id_unique | 1 | 1 |
| `neg-v3-input_directory_consistency` | V3 input_directory_consistency | 1 | 1 |
| `neg-v3-input_entry_file` | V3 input_entry_file | 1 | 1 |
| `neg-v3-input_entry_object` | V3 input_entry_object | 1 | 1 |
| `neg-v3-input_nesting` | V3 input_nesting | 1 | 1 |
| `neg-v3-input_source_refs` | V3 input_source_refs | 1 | 1 |
| `neg-v3-license_file_ref` | V3 license_file_ref | 1 | 1 |
| `neg-v3-license_spdx` | V3 license_spdx | 1 | 1 |
| `neg-v3-preview_refs` | V3 preview_refs | 1 | 1 |
| `neg-v3-storage_paths` | V3 storage_paths | 1 | 1 |
| `neg-v3-timestamps` | V3 timestamps | 1 | 1 |
| `neg-v3-unique_ids` | V3 unique_ids | 1 | 1 |
| `neg-v3-urls` | V3 urls | 1 | 1 |
| `neg-v3-value_constraints` | V3 value_constraints | 1 | 1 |
| `neg-v3-value_dependencies_scope` | V3 value_dependencies_scope | 1 | 1 |
| `neg-v3-value_file_ref` | V3 value_file_ref | 1 | 1 |
| `neg-v3-value_options` | V3 value_options | 1 | 1 |
| `neg-v3-value_representation` | V3 value_representation | 1 | 1 |
| `neg-v3-value_unit_required` | V3 value_unit_required | 1 | 1 |
| `neg-v4-crc` | V4 crc | 0 | 1 |
| `neg-v4-encryption` | V4 encryption | 0 | 1 |
| `neg-v4-method` | V4 method | 0 | 1 |
| `neg-v4-missing` | Registered stream is absent | 0 | 1 |
| `neg-v4-sha` | V4 sha mismatch | 0 | 1 |
| `neg-v4-size` | V4 size mismatch | 0 | 1 |
| `neg-zip-absolute` | Unsafe ZIP name: absolute | 1 | 1 |
| `neg-zip-backslash` | Unsafe ZIP name: backslash | 1 | 1 |
| `neg-zip-case-collision` | Unsafe ZIP name: case-collision | 1 | 1 |
| `neg-zip-duplicate` | Unsafe ZIP name: duplicate | 1 | 1 |
| `neg-zip-not-zip` | No EOCD / not a ZIP | 1 | 1 |
| `neg-zip-opaque-symlink` | Unregistered entry marked as Unix symlink | 1 | 1 |
| `neg-zip-symlink` | Registered entry marked as Unix symlink | 1 | 1 |
| `neg-zip-traversal` | Unsafe ZIP name: traversal | 1 | 1 |
| `pos-descriptor` | Deflate streams with signed data descriptors | 0 | 0 |
| `pos-descriptor-zip64` | Forced ZIP64 and data descriptors | 0 | 0 |
| `pos-exact-numbers` | Exact S7 numeric lexemes in all six opaque owner categories | 0 | 0 |
| `pos-foreign-extra` | Read foreign 0x5455 local/central extra and entry comment | 0 | 0 |
| `pos-full` | All optional sections, provenance, claims, groups, historical files, previews, license and dependencies | 0 | 0 |
| `pos-minimal` | Minimal asset: one generated file | 0 | 0 |
| `pos-mixed-compression` | Stored manifest and Deflate payload | 0 | 0 |
| `pos-newer-minor` | Newer minor manifest preserves unknown structural keys | 3 | 3 |
| `pos-opaque-stream` | Safe unknown stream is verified by CRC only (decision V-2) | 0 | 3 |
| `pos-unicode-spaces` | UTF-8 path with spaces | 0 | 0 |
| `pos-unverifiable` | Physical values require unavailable unit conversion | 3 | 3 |
| `pos-zip64` | Forced ZIP64 local sizes for small entries | 0 | 0 |
