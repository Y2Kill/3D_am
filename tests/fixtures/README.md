# tests/fixtures — синтетические тестовые наборы (ADR-0029, ТЗ §13.4)

Каждый случай — каталог `<case>/`:
- `input/` — малые сгенерированные входные данные;
- `expected.json` — машинно-читаемые ожидания: состав ассетов, пути, SHA-256, исключения с причинами, статусы, ошибки. Схема — `schemas/expected.v1.schema.json` (M1). ID и время проверяются по свойствам, а не побайтово.

Генератор — `tools/fixtures/` (M1). Файлы контрольной коллекции сюда **не копируются** (CLAUDE.md).

Независимый корпус ZIP: [`golden/README.md`](golden/README.md). Исходные манифесты и параметры упаковки — `tools/fixtures/golden/`, генератор — `tools/fixtures/make_golden_zips.py`. `golden_regen_check` сравнивает результат побайтно, `golden_zip` запускает собранный CLI; эталоны проверяются `schema_check` по `schemas/expected.v1.schema.json`.

## Корпус импорта T-034

В `import/<case>/` хранятся только `case.json` и независимо написанный
`expected.json`. Все SC-1…SC-16 представлены; `ch-*` проверяют отдельные
характеристики. Входные байты, длинные пути, Unicode и ссылки создаются при
каждом прогоне в `<build>/import_corpus/<case>/source/`, с фиксированным mtime.
Коллекция владельца не используется. Готовые ассеты берутся из golden без
изменения исходных файлов. Общий `tools/fixtures/zipgen.py` сохраняет прежние
байты golden; Deflate используется с уровнем 0.

Схемы: `schemas/import-case.v1.schema.json`,
`schemas/expected-import.v1.schema.json`; проверяет `schema_check`.
`source[]` описывает относительные пути и kind: file, directory, zip, symlink,
golden. Байты задаются ровно одним способом: content (UTF-8), hex,
pattern + size (байты), zip с рекурсивными entries. Имена записей ZIP могут
иметь raw_name_hex/flags; patches изменяют заголовки, как в golden.

`scan` задаёт roots (относительно source, по умолчанию `.`), args,
необязательный profile_toml и library. Правки plan_edits выбирают узел
по пути (`archive.zip!entry!nested-entry` для записей), а не по UUID:
decision, action, mode, merge (весь кандидат в кандидат с узлом into).
Правка решения записи сама по себе не меняет action=ask кандидата:
явное подтверждение задаётся отдельной правкой action.

`steps` задаёт scan/run/verify/rescan/add-ready и exit_code; name позволяет
повторять команду с разными ожиданиями. `@library` в аргументах подставляет
изолированную библиотеку случая. У шага run допустимы свои plan_edits
(проверка strict и partial по одному входу). Все AM_LOG_* убираются из окружения;
профиль пользователя и временные файлы находятся внутри каталога случая.
Каждый вызов CLI оставляет `command-NN.json` с аргументами, кодом и выводом.

Ожидания в steps сравниваются рекурсивным подмножеством; массивы имеют
точную длину, порядок произволен, сопоставление взаимно однозначно.
Пути в диагностике — JSON Pointer. Scan-представление содержит summary,
candidates, nodes по относительным путям, archives, archive_entries и
archive_indices (путь архива → индекс → запись). У кандидата entries
индексированы путями, reason_flags/warning_flags/match_flags — множества
с истинными значениями. Полный план остаётся в plan-N.json.

Run-представление содержит summary и assets. Для каждого ассета доступны
исходный manifest, files с storage_relative/source_paths, input_count,
object_counts, entries, object_entries (имя объекта!путь → запись с file_sha256),
raw_entries (Base64 → запись), nested_actions,
input_completeness, has_previews/has_sources и source_contexts с has_review.
Словари entries — удобный вид по пути; при одинаковых путях разных объектов
проверяйте полный manifest, object_counts и размещение каждого files[].
UUID, ревизия, байты каждого файла, SHA-256, неизменность источника и
пустая временная область проверяются самим прогонщиком. Исходная внешняя
упаковка не может оказаться полезным файлом; preserve_container допускает
это только для специализированного контейнера в соответствующем случае.

Добавление случая: сначала по требованию написать вход и ожидание с basis;
указать хотя бы один SC-n в scenarios и req. Для особенностей ОС указать
requires (windows/linux/symlink). Затем выполнить:

```text
python -I tests/import_corpus/run_import_corpus.py --am build/win-cli/bin/am.exe --cases tests/fixtures/import --work build/win-cli/import_corpus --case sc01-single-stl
python -I tools/fixtures/make_import_cases.py --work .cache/import-regen --check
ctest --test-dir build/win-cli -L import_corpus --output-on-failure
```

CMake обнаруживает каталоги автоматически и создаёт import_corpus.<case>
с метками import_corpus/req:*. На Linux проверяется весь scan и отказ run
с am.library.storage_read_only; проверки записи помечаются NOT_APPLICABLE.
Ссылки без привилегий явно SKIP (код 77). Ветвь выбирается по
write_unavailable_reason из library info, поэтому после разрешения записи
на Linux ожидания run начнут проверяться без правки случаев.
SC-15 выполняет оба rescan и на read-only ФС: read_only_steps в expected.json
задаёт полный состав источника и пустые matches, action=import. Отсутствие
импорта не выдается за проверенное совпадение или skip-identical; основные
ожидания steps автоматически применяются, когда запись разрешена.
Отрицательные самопроверки прогонщика — import_corpus_selftest,
детерминизм и границы генератора — import_corpus_regen_check.
