# Golden ZIP — T-013 (частичный корпус)

Исходники: `tools/fixtures/golden/<case>/`. Генератор читает `manifest.json` как байты; только явные метки `@sha256:<ZIP-path>` и `@size:<ZIP-path>` заменяются SHA-256 и десятичным размером соответствующего payload из `case.json`. JSON не пересериализуется. Порядок записей задан массивом `entries`; время фиксировано 2020-01-01 00:00:00, UTF-8 bit 11 установлен даже для ASCII. Повреждения задаются `patches` в исходнике; меняются поля local/central заголовков, не payload. ZIP64 принудителен в local header (`force_zip64`), descriptor создаёт non-seekable поток.

`python -I tools/fixtures/make_golden_zips.py` — генерация; `--check` — регенерация в `.cache/` и побайтное сравнение `asset.zip` и `expected.json`; `--output` задаёт отдельную копию корпуса. Файлы ZIP не правятся вручную. Каждый ZIP не более 64 КиБ (гипотеза для размера репозитория).

Ожидания задаются исходными `expected.json`, никогда не снимаются с проверяемой программы. Объекты сравниваются рекурсивным подмножеством, массивы — без порядка с взаимно однозначным сопоставлением; длина массива должна совпадать. ID и время не входят в шаблоны.

| Случай | Описание | validate | verify full |
|---|---|---:|---:|
| `pos-minimal` | Minimal asset: one generated file | 0 | 0 |
| `pos-descriptor` | Deflate streams with signed data descriptors | 0 | 0 |
| `pos-zip64` | Forced ZIP64 local sizes for small entries | 0 | 0 |
| `pos-descriptor-zip64` | Forced ZIP64 and data descriptors | 0 | 0 |
| `pos-mixed-compression` | Stored manifest and Deflate payload | 0 | 0 |
| `pos-foreign-extra` | Read foreign 0x5455 local/central extra and entry comment | 0 | 0 |
| `pos-opaque-stream` | Safe unknown stream is verified by CRC only (decision V-2) | 0 | 3 |
| `pos-unicode-spaces` | UTF-8 path with spaces | 0 | 0 |
| `pos-newer-minor` | Newer minor manifest preserves unknown structural keys | 3 | 3 |
| `neg-zip-symlink` | Registered entry marked as Unix symlink | 1 | 1 |
| `neg-zip-opaque-symlink` | Unregistered entry marked as Unix symlink | 1 | 1 |
