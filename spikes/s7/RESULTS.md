# S7: результаты T-004 после доработки

Эксперимент на закреплённом baseline, не утверждение формата 1.0 и не ADR. Первичный Windows-отчёт и его тестовые итоги сохранены в файле задачи; ниже — новые измерения со структурной правкой.

## Механизмы и сценарий

- A: nlohmann ordered DOM; правки в DOM.
- B: nlohmann SAX, лексемы number_float и собственные события; C: RapidJSON NumbersAsStrings + проверенный RawValue. B и C и в первом варианте меняли title вклейкой после сериализации; теперь структурные правки также выполняются по позициям токенов.
- D1: прежний D, правка исходного текста по позициям токенов. Он сохранён как отдельный механизм; его успех не доказывает работу DOM-гибрида.
- D2: известная часть в nlohmann ordered DOM; непрозрачные корни заменены значениями binary с 64-битным номером в отдельной таблице. JSON не содержит binary-типа: строки, объекты bytes/subtype и похожие маркеры из манифеста не создают заглушку. Заглушка переезжает вместе с владельцем при erase массива. Собственный писатель обходит DOM, экранирует ключи/обычные значения через nlohmann и выводит фрагмент только после нового строгого разбора ровно одного JSON-значения; подстановка по шаблонам отсутствует.
- E: jsoncons lossless_number, правки в его DOM, собственная V2 2020-12.

Все кандидаты меняют title, удаляют первый user custom_value перед plugin custom_value, добавляют одну metadata_changed-запись history.entries только с известными полями, увеличивают asset.revision и обновляют modified_at до timestamp новой записи. Проверяются входная V2 и выходная V2, ожидаемая известная часть, все шесть непрозрачных мест, содержимое ZIP и повторное открытие. Запись транзакционна: проверенный временный ZIP в той же папке, атомарная замена. При обработанном отказе исходный ZIP побайтово прежний; независимый guard проверяет SHA-256.

## Устойчивые ключи защиты

| Владелец | Ключ |
|---|---|
| extensions | ExtensionID — ключ объекта |
| field_definitions / custom_values | id / field_id (FieldID) |
| files, groups | id (FileID / GroupID); file_id в ссылках обозначает этот id |
| claims | id (ClaimID, SPEC §9.3) |
| history.entries | operation_id (OperationID, SPEC §11) |

Scope владельца включён в ключ: asset, конкретный FileID или GroupID. Неоднозначные одинаковые ID отвергаются. Внутри непрозрачного поддерева остаётся типизированный JSON-путь: перестановка чисел внутри массива обнаруживается. В changes[] собственного ID по SPEC нет: для дополнительного непрозрачного ключа сохраняется индекс изменения внутри OperationID; этот массив в сценарии не редактируется. Проверка не объявляет произвольную перестановку changes поддержанной. Индексная карта сохранена отдельно: на структурной правке она ожидаемо различается, на успешной правке только title — совпадает.

## Конфигурация и зависимости

Windows: `Windows-10-10.0.19045-SP0`

```json
{
  "cpu": "6 core 2592 MHz GenuineIntel Unknown P6 family",
  "ram_mib": 114433,
  "build_type": "Release",
  "compiler": "MSVC 19.44.35223.0",
  "baseline": "9eec002df1cd109f8f104cd966a763daf9751489"
}
```

Linux: `Linux-6.18.40.1-microsoft-standard-WSL2-x86_64-with-glibc2.39`

```json
{
  "cpu": "6 core Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz",
  "ram_mib": 56115,
  "build_type": "Release",
  "compiler": "GNU 13.3.0",
  "baseline": "9eec002df1cd109f8f104cd966a763daf9751489"
}
```

Intel Core i7-9750H @ 2.60GHz, 6 ядер / 12 потоков; физическая ОЗУ Windows — 119 992 246 272 байта. Release, пять отдельных последовательных процессов на пару документ/кандидат; дополнительно один title-only процесс на пару. WSL: установленная Ubuntu 24.04, GCC; все рабочие файлы через /mnt/d (drvfs). Этот путь обычно медленнее родной ФС и влияет на время ZIP/цикла; отдельного сравнения с ext4 в задаче нет. На сохранность лексем тип ФС не влияет.

Гипотезы, не нормативы продукта: max-depth reference/D1/D2 1024, max JSON bytes 134217728, тайм-аут процесса 60.0 с. Нативный разбор исполняется до общего эталона.

| Порт | Версия | Лицензия |
|---|---|---|
| nlohmann-json | 3.12.0#2 | MIT |
| json-schema-validator | 2.4.0 | MIT |
| rapidjson | 2025-02-26#1 | MIT |
| jsoncons | 1.9.0 | BSL-1.0 |
| minizip-ng | 4.1.0 | Zlib |
| gtest | 1.18.0 | BSD-3-Clause |

## Матрица

Шесть знаков: extensions, custom, generation, claims, fields, history. «+» означает совпадение чисел, типов и значений по устойчивому владельцу; «н/д» — guard не достигнут.

| ID | Windows valid_all / 6 мест | Linux valid_all / 6 мест | Строгость Windows / Linux | Fail-closed |
|---|---|---|---|---|
| A | unsafe_write / н/д | unsafe_write / н/д | все 12 отвергнуты / все 12 отвергнуты | SHA-256 guard PASS |
| B | unsafe_write / н/д | unsafe_write / н/д | все 12 отвергнуты / все 12 отвергнуты | SHA-256 guard PASS |
| C | unsafe_write / н/д | unsafe_write / н/д | все 12 отвергнуты / все 12 отвергнуты | SHA-256 guard PASS |
| D1 | success / ++++++ | success / ++++++ | все 12 отвергнуты / все 12 отвергнуты | SHA-256 guard PASS |
| D2 | success / ++++++ | success / ++++++ | все 12 отвергнуты / все 12 отвергнуты | SHA-256 guard PASS |
| E | unsafe_write / ------ | unsafe_write / ------ | все 12 отвергнуты / все 12 отвергнуты | SHA-256 guard PASS |

### Windows: сохранность и индексная проверка

| ID | extensions | custom | generation | claims | fields | history | title-only индексная карта |
|---|---|---|---|---|---|---|---|
| A | 2/11; н/д 3 | 2/11; н/д 3 | 2/11; н/д 3 | 2/11; н/д 3 | 2/11; н/д 3 | 2/11; н/д 3 | 2/14 совпали |
| B | 10/11; н/д 3 | 10/11; н/д 3 | 10/11; н/д 3 | 10/11; н/д 3 | 10/11; н/д 3 | 10/11; н/д 3 | 10/14 совпали |
| C | 11/11; н/д 3 | 11/11; н/д 3 | 11/11; н/д 3 | 11/11; н/д 3 | 11/11; н/д 3 | 11/11; н/д 3 | 11/14 совпали |
| D1 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14 совпали |
| D2 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14 совпали |
| E | 12/14; н/д 0 | 12/14; н/д 0 | 12/14; н/д 0 | 12/14; н/д 0 | 12/14; н/д 0 | 12/14; н/д 0 | 12/14 совпали |

E не прошёл следующие допустимые числовые фикстуры: `valid_all.json`, `valid_negative_zero.json`.

### Windows: дубли

| ID | Непрозрачный | Известный |
|---|---|---|
| A | coalesced: ['2'] | coalesced: ['last'] |
| B | preserved: ['1', '2'] | preserved: ['first', 'last'] |
| C | preserved: ['1', '2'] | preserved: ['first', 'last'] |
| D1 | отказ native: [] | отказ native: [] |
| D2 | отказ native: [] | отказ native: [] |
| E | coalesced: ['1'] | coalesced: ['first'] |

### Windows: глубина и причина отказа

| ID | Глубина | Итог | Этап | Ошибка / код |
|---|---:|---|---|---|
| A | 64 | success | reopen |  |
| A | 1000 | success | reopen |  |
| A | 100000 | crash | процесс | 0xc00000fd |
| B | 64 | success | reopen |  |
| B | 1000 | success | reopen |  |
| B | 100000 | unsafe_write | reference_v1 | hypothesis_max_depth exceeded at byte 7021 |
| C | 64 | success | reopen |  |
| C | 1000 | success | reopen |  |
| C | 100000 | unsafe_write | reference_v1 | hypothesis_max_depth exceeded at byte 7021 |
| D1 | 64 | success | reopen |  |
| D1 | 1000 | success | reopen |  |
| D1 | 100000 | unsafe_write | parse | hypothesis_max_depth exceeded at byte 7021 |
| D2 | 64 | success | reopen |  |
| D2 | 1000 | success | reopen |  |
| D2 | 100000 | unsafe_write | parse | hypothesis_max_depth exceeded at byte 7021 |
| E | 64 | success | reopen |  |
| E | 1000 | success | reopen |  |
| E | 100000 | unsafe_write | parse | Data item nesting exceeds limit in options at line 1 and column 7022 |

### Linux: сохранность и индексная проверка

| ID | extensions | custom | generation | claims | fields | history | title-only индексная карта |
|---|---|---|---|---|---|---|---|
| A | 2/11; н/д 3 | 2/11; н/д 3 | 2/11; н/д 3 | 2/11; н/д 3 | 2/11; н/д 3 | 2/11; н/д 3 | 2/14 совпали |
| B | 10/11; н/д 3 | 10/11; н/д 3 | 10/11; н/д 3 | 10/11; н/д 3 | 10/11; н/д 3 | 10/11; н/д 3 | 10/14 совпали |
| C | 11/11; н/д 3 | 11/11; н/д 3 | 11/11; н/д 3 | 11/11; н/д 3 | 11/11; н/д 3 | 11/11; н/д 3 | 11/14 совпали |
| D1 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14 совпали |
| D2 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14; н/д 0 | 14/14 совпали |
| E | 12/14; н/д 0 | 12/14; н/д 0 | 12/14; н/д 0 | 12/14; н/д 0 | 12/14; н/д 0 | 12/14; н/д 0 | 12/14 совпали |

E не прошёл следующие допустимые числовые фикстуры: `valid_all.json`, `valid_negative_zero.json`.

### Linux: дубли

| ID | Непрозрачный | Известный |
|---|---|---|
| A | coalesced: ['2'] | coalesced: ['last'] |
| B | preserved: ['1', '2'] | preserved: ['first', 'last'] |
| C | preserved: ['1', '2'] | preserved: ['first', 'last'] |
| D1 | отказ native: [] | отказ native: [] |
| D2 | отказ native: [] | отказ native: [] |
| E | coalesced: ['1'] | coalesced: ['first'] |

### Linux: глубина и причина отказа

| ID | Глубина | Итог | Этап | Ошибка / код |
|---|---:|---|---|---|
| A | 64 | success | reopen |  |
| A | 1000 | success | reopen |  |
| A | 100000 | unsafe_write | reference_v1 | hypothesis_max_depth exceeded at byte 7021 |
| B | 64 | success | reopen |  |
| B | 1000 | success | reopen |  |
| B | 100000 | unsafe_write | reference_v1 | hypothesis_max_depth exceeded at byte 7021 |
| C | 64 | success | reopen |  |
| C | 1000 | success | reopen |  |
| C | 100000 | unsafe_write | reference_v1 | hypothesis_max_depth exceeded at byte 7021 |
| D1 | 64 | success | reopen |  |
| D1 | 1000 | success | reopen |  |
| D1 | 100000 | unsafe_write | parse | hypothesis_max_depth exceeded at byte 7021 |
| D2 | 64 | success | reopen |  |
| D2 | 1000 | success | reopen |  |
| D2 | 100000 | unsafe_write | parse | hypothesis_max_depth exceeded at byte 7021 |
| E | 64 | success | reopen |  |
| E | 1000 | success | reopen |  |
| E | 100000 | unsafe_write | parse | Data item nesting exceeds limit in options at line 1 and column 7022 |

Дубли всегда блокируют запись в стенде. Это не утверждение политики O-17.

## V2

A–D2 используют jsoncons 2020-12 на теневом документе, E — на собственном DOM. pboettch отдельно испытан на исходном документе и shadow. Само успешное чтение схемы не доказывает поддержку её диалекта.

Windows, исходный пример:
```json
{
  "pboettch_native": {
    "schema_loaded": true,
    "accepted": true
  },
  "jsoncons_native": {
    "accepted": true
  },
  "pboettch_shadow": {
    "schema_loaded": true,
    "accepted": true
  },
  "jsoncons_shadow": {
    "accepted": true
  },
  "wall_ms": 101.70120000839233,
  "exit_code": 0
}
```

Windows, запрещённое дополнительное поле claim (unevaluatedProperties):
```json
{
  "pboettch_native": {
    "schema_loaded": true,
    "accepted": true
  },
  "jsoncons_native": {
    "accepted": false,
    "error": "/asset/metadata/claims/1/unexpected: Unevaluated property 'unexpected' but the schema does not allow unevaluated properties."
  },
  "pboettch_shadow": {
    "schema_loaded": true,
    "accepted": true
  },
  "jsoncons_shadow": {
    "accepted": false,
    "error": "/asset/metadata/claims/1/unexpected: Unevaluated property 'unexpected' but the schema does not allow unevaluated properties."
  },
  "wall_ms": 60.70749997161329,
  "exit_code": 0
}
```

Linux, исходный пример:
```json
{
  "pboettch_native": {
    "schema_loaded": true,
    "accepted": true
  },
  "jsoncons_native": {
    "accepted": true
  },
  "pboettch_shadow": {
    "schema_loaded": true,
    "accepted": true
  },
  "jsoncons_shadow": {
    "accepted": true
  },
  "wall_ms": 55.745984999987286,
  "exit_code": 0
}
```

Linux, запрещённое дополнительное поле claim (unevaluatedProperties):
```json
{
  "pboettch_native": {
    "schema_loaded": true,
    "accepted": true
  },
  "jsoncons_native": {
    "accepted": false,
    "error": "/asset/metadata/claims/1/unexpected: Unevaluated property 'unexpected' but the schema does not allow unevaluated properties."
  },
  "pboettch_shadow": {
    "schema_loaded": true,
    "accepted": true
  },
  "jsoncons_shadow": {
    "accepted": false,
    "error": "/asset/metadata/claims/1/unexpected: Unevaluated property 'unexpected' but the schema does not allow unevaluated properties."
  },
  "wall_ms": 60.2706519999856,
  "exit_code": 0
}
```

pboettch загружает схему, но ложно принимает запрещённое поле claim; jsoncons и Python отвергают. Наблюдение закреплённого порта подтверждено отдельно на обеих ОС.

Shadow сохраняет JSON-тип непрозрачного корня. Схема требует object для generation.params и history.details; значения чисел внутри остальных перечисленных plugin-поддеревьев не ограничивает. Известные ограничения остаются; лексемы проверяет V1/guard. Проверка семантики плагина из этого не следует. При смене схемы применимость shadow проверяется заново.

## Память: разложение стенда

Ниже медианы пяти изолированных процессов на manifest_64m, МиБ. input — только чтение исходного текста; reference — чтение и токенизатор; reference_maps — также одна пара устойчивых карт значений/чисел; candidate — чтение, разбор, правка и сериализация одного кандидата без ZIP/V2/общего guard. Значения включают общий процесс и вход. Их нельзя складывать: общие байты учитываются повторно, аллокаторы и временные копии имеют разные времена жизни. Разности относительно input — оценка дополнительных затрат. Полный цикл держит одновременно два документа и карты before/after, ZIP-копии и проверки повторного открытия. Первоначальные ~837 МиБ были пиком всего стенда, а не памятью каждого парсера.

| ОС | Компонент | Peak RSS/working set | сверх input | оценка сверх input / полный цикл |
|---|---|---:|---:|---:|
| Windows | input:D2 | 91.398 | 0.000 | 0.0% |
| Windows | reference:D2 | 260.926 | 169.527 | 20.2% |
| Windows | reference_maps:D2 | 387.961 | 296.562 | 35.4% |
| Windows | candidate:A | 324.652 | 233.254 | 27.9% |
| Windows | candidate:B | 389.145 | 297.746 | 35.6% |
| Windows | candidate:C | 389.090 | 297.691 | 35.6% |
| Windows | candidate:D1 | 324.656 | 233.258 | 27.9% |
| Windows | candidate:D2 | 325.082 | 233.684 | 27.9% |
| Windows | candidate:E | 260.066 | 168.668 | 20.1% |
| Linux | input:D2 | 124.164 | 0.000 | 0.0% |
| Linux | reference:D2 | 260.133 | 135.969 | 17.5% |
| Linux | reference_maps:D2 | 260.059 | 135.895 | 17.5% |
| Linux | candidate:A | 324.246 | 200.082 | 25.8% |
| Linux | candidate:B | 388.523 | 264.359 | 34.1% |
| Linux | candidate:C | 388.438 | 264.273 | 34.1% |
| Linux | candidate:D1 | 324.398 | 200.234 | 25.8% |
| Linux | candidate:D2 | 324.258 | 200.094 | 25.8% |
| Linux | candidate:E | 260.422 | 136.258 | 17.6% |

Проценты — только оценка масштаба компонента относительно пика полного цикла, не аддитивные доли. reference_maps включает reference, а кандидаты B/C/D1/D2 также используют эталон внутри своего механизма. Разность reference_maps − reference оценивает добавочную цену одной пары карт; в полном guard одновременно есть карты до/после. Из-за повторного использования аллокатором временных буферов карты могут не увеличить уже достигнутый пик (наблюдается на Linux); это не означает нулевой размер карт. Остаток нельзя приписать конкретной библиотеке: он включает повторное открытие, ZIP, копии текста и аллокаторы.

Linux ru_maxrss — RSS, не commit. PeakPagefile на Linux API не измеряет; в машинных данных стоит 0, в таблице ниже «н/д». Для crash пик памяти недоступен и не заменяется нулём.

## Различия ОС

- `depth_100000.json`, A, outcomes: Windows `['crash']`, Linux `['unsafe_write']`.
- `depth_100000.json`, A, parse_accepted: Windows `0`, Linux `5`.
- `depth_100000.json`, A, locations: Windows `None`, Linux `[None, None, None, None, None, None]`.

Коды crash платформенные; на depth_100000 учитываются отдельно в машинных данных. Время и RSS между ОС напрямую не эквивалентны (drvfs, GCC/MSVC, аллокаторы, системные библиотеки).

## Размер реализации

Непустые физические строки (включая комментарии), не метрика сложности:

| Файл | Строк |
|---|---:|
| candidates.cpp | 231 |
| candidates.hpp | 21 |
| jsoncons_candidate.cpp | 37 |
| main.cpp | 196 |
| platform_memory.cpp | 45 |
| platform_memory.hpp | 9 |
| reference.cpp | 295 |
| reference.hpp | 37 |
| test_reference.cpp | 64 |
| zip.cpp | 58 |
| zip.hpp | 10 |

candidates.cpp содержит A/B/C/D1/D2 и общий писатель событий; reference.cpp — общий строгий проверяющий код и D1; jsoncons_candidate.cpp — E и V2. Память/ZIP/main — общая обвязка.

| Адаптер / общая часть | Непустых строк |
|---|---:|
| A | 6 |
| B | 28 |
| C | 27 |
| D1 | 10 |
| D2 | 21 |
| B/C общий event writer | 47 |
| D2 собственный writer | 37 |
| Общая DOM-правка и запись истории | 18 |
| E | см. jsoncons_candidate.cpp выше (включает общий V2) |

D1 использует также edit_known/edit_title и токенизатор из reference.cpp; B/C делят этот код. D2 также использует эталонный токенизатор для строгой проверки фрагментов. Общий код не скрыт внутри числа адаптера.

## Полные замеры

Медианы пяти отдельных структурных запусков. Время в мс, память в МиБ. Цикл включает ZIP, V1/V2, правку, guard и транзакцию; при отказе указано время до отказа. «—» — этап не достигнут/процесс упал; wall — время дочернего процесса. Исходные повторы, title-only, хеши фикстур и validator probes: build/s7/results.json и build/s7-linux/results.json.

### Windows

| Документ | ID | Итог | parse | V2 | serialize | цикл/отказ | wall | Peak RSS/WS | PeakPagefile |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| depth_1000.json | A | success | 1.382 | 21.134 | 0.135 | 61.063 | 70.648 | 7.527 | 4.020 |
| depth_1000.json | B | success | 0.455 | 19.149 | 2.389 | 58.869 | 69.227 | 7.520 | 4.043 |
| depth_1000.json | C | success | 0.337 | 21.158 | 2.209 | 61.017 | 72.783 | 7.543 | 4.055 |
| depth_1000.json | D1 | success | 2.071 | 22.380 | 2.326 | 63.490 | 74.197 | 7.438 | 3.938 |
| depth_1000.json | D2 | success | 2.635 | 20.419 | 1.105 | 61.801 | 72.573 | 7.539 | 4.031 |
| depth_1000.json | E | success | 0.555 | 17.519 | 0.151 | 49.344 | 65.433 | 7.543 | 4.051 |
| depth_100000.json | A | crash | — | — | — | — | 52.863 | — | — |
| depth_100000.json | B | unsafe_write | 17.886 | — | — | 29.162 | 40.551 | 14.926 | 14.699 |
| depth_100000.json | C | unsafe_write | 11.534 | — | — | 23.614 | 34.197 | 16.074 | 16.062 |
| depth_100000.json | D1 | unsafe_write | 2.590 | — | — | 10.161 | 21.234 | 5.375 | 2.418 |
| depth_100000.json | D2 | unsafe_write | 2.962 | — | — | 11.746 | 22.364 | 5.461 | 2.473 |
| depth_100000.json | E | unsafe_write | 0.650 | — | — | 9.489 | 20.007 | 4.668 | 1.910 |
| depth_64.json | A | success | 0.615 | 22.134 | 0.107 | 51.449 | 61.234 | 6.707 | 3.148 |
| depth_64.json | B | success | 0.227 | 21.545 | 1.840 | 57.148 | 70.142 | 6.715 | 3.148 |
| depth_64.json | C | success | 0.149 | 21.531 | 1.497 | 57.323 | 67.779 | 6.707 | 3.160 |
| depth_64.json | D1 | success | 1.316 | 26.200 | 1.601 | 79.935 | 119.840 | 6.688 | 3.133 |
| depth_64.json | D2 | success | 1.489 | 23.822 | 0.500 | 60.387 | 71.929 | 6.691 | 3.141 |
| depth_64.json | E | success | 0.251 | 20.631 | 0.112 | 51.209 | 165.764 | 6.711 | 3.160 |
| duplicate_known.json | A | unsafe_write | 0.584 | — | — | 6.348 | 16.200 | 4.457 | 1.461 |
| duplicate_known.json | B | unsafe_write | 0.199 | — | — | 7.953 | 18.568 | 4.395 | 1.445 |
| duplicate_known.json | C | unsafe_write | 0.188 | — | — | 8.371 | 20.313 | 4.383 | 1.441 |
| duplicate_known.json | D1 | unsafe_write | 0.852 | — | — | 5.069 | 15.300 | 4.238 | 1.461 |
| duplicate_known.json | D2 | unsafe_write | 0.955 | — | — | 5.093 | 14.746 | 4.219 | 1.449 |
| duplicate_known.json | E | unsafe_write | 0.302 | — | — | 6.618 | 17.750 | 4.504 | 1.461 |
| duplicate_opaque.json | A | unsafe_write | 0.531 | — | — | 6.104 | 15.808 | 4.363 | 1.449 |
| duplicate_opaque.json | B | unsafe_write | 0.171 | — | — | 6.854 | 17.158 | 4.410 | 1.445 |
| duplicate_opaque.json | C | unsafe_write | 0.195 | — | — | 7.754 | 20.069 | 4.391 | 1.438 |
| duplicate_opaque.json | D1 | unsafe_write | 0.769 | — | — | 4.372 | 14.467 | 4.238 | 1.445 |
| duplicate_opaque.json | D2 | unsafe_write | 0.815 | — | — | 4.724 | 14.128 | 4.246 | 1.445 |
| duplicate_opaque.json | E | unsafe_write | 0.247 | — | — | 5.790 | 15.309 | 4.500 | 1.457 |
| invalid_00.json | A | unsafe_write | 0.142 | — | — | 4.211 | 13.984 | 4.133 | 1.445 |
| invalid_00.json | B | unsafe_write | 0.141 | — | — | 4.954 | 19.519 | 4.137 | 1.449 |
| invalid_00.json | C | unsafe_write | 0.083 | — | — | 3.787 | 13.249 | 4.148 | 1.461 |
| invalid_00.json | D1 | unsafe_write | 0.194 | — | — | 4.221 | 14.445 | 4.168 | 1.445 |
| invalid_00.json | D2 | unsafe_write | 0.179 | — | — | 4.051 | 13.809 | 4.180 | 1.461 |
| invalid_00.json | E | unsafe_write | 0.177 | — | — | 4.064 | 14.062 | 4.285 | 1.449 |
| invalid_01.json | A | unsafe_write | 0.134 | — | — | 3.865 | 12.915 | 4.148 | 1.461 |
| invalid_01.json | B | unsafe_write | 0.120 | — | — | 4.181 | 13.797 | 4.137 | 1.449 |
| invalid_01.json | C | unsafe_write | 0.095 | — | — | 4.166 | 14.193 | 4.160 | 1.488 |
| invalid_01.json | D1 | unsafe_write | 0.176 | — | — | 3.919 | 13.522 | 4.176 | 1.457 |
| invalid_01.json | D2 | unsafe_write | 0.189 | — | — | 4.339 | 14.411 | 4.184 | 1.461 |
| invalid_01.json | E | unsafe_write | 0.195 | — | — | 4.489 | 15.197 | 4.285 | 1.457 |
| invalid_02.json | A | unsafe_write | 0.129 | — | — | 4.163 | 13.558 | 4.141 | 1.465 |
| invalid_02.json | B | unsafe_write | 0.115 | — | — | 4.973 | 14.445 | 4.133 | 1.449 |
| invalid_02.json | C | unsafe_write | 0.086 | — | — | 4.079 | 13.600 | 4.133 | 1.449 |
| invalid_02.json | D1 | unsafe_write | 0.228 | — | — | 4.921 | 16.289 | 4.176 | 1.465 |
| invalid_02.json | D2 | unsafe_write | 0.199 | — | — | 4.617 | 14.971 | 4.180 | 1.457 |
| invalid_02.json | E | unsafe_write | 0.185 | — | — | 4.245 | 14.134 | 4.281 | 1.453 |
| invalid_03.json | A | unsafe_write | 0.140 | — | — | 4.480 | 14.786 | 4.133 | 1.449 |
| invalid_03.json | B | unsafe_write | 0.132 | — | — | 5.703 | 122.170 | 4.148 | 1.465 |
| invalid_03.json | C | unsafe_write | 0.113 | — | — | 4.899 | 20.094 | 4.141 | 1.461 |
| invalid_03.json | D1 | unsafe_write | 0.170 | — | — | 3.733 | 12.496 | 4.184 | 1.469 |
| invalid_03.json | D2 | unsafe_write | 0.180 | — | — | 4.138 | 13.373 | 4.172 | 1.465 |
| invalid_03.json | E | unsafe_write | 0.172 | — | — | 4.252 | 13.780 | 4.285 | 1.457 |
| invalid_04.json | A | unsafe_write | 0.131 | — | — | 4.178 | 14.410 | 4.148 | 1.473 |
| invalid_04.json | B | unsafe_write | 0.117 | — | — | 4.633 | 14.854 | 4.145 | 1.461 |
| invalid_04.json | C | unsafe_write | 0.099 | — | — | 4.309 | 14.635 | 4.145 | 1.445 |
| invalid_04.json | D1 | unsafe_write | 0.170 | — | — | 3.935 | 12.772 | 4.168 | 1.449 |
| invalid_04.json | D2 | unsafe_write | 0.171 | — | — | 3.926 | 12.913 | 4.172 | 1.453 |
| invalid_04.json | E | unsafe_write | 0.183 | — | — | 4.366 | 14.327 | 4.281 | 1.457 |
| invalid_05.json | A | unsafe_write | 0.151 | — | — | 4.245 | 14.785 | 4.141 | 1.457 |
| invalid_05.json | B | unsafe_write | 0.131 | — | — | 4.201 | 16.952 | 4.137 | 1.461 |
| invalid_05.json | C | unsafe_write | 0.085 | — | — | 4.173 | 14.686 | 4.152 | 1.465 |
| invalid_05.json | D1 | unsafe_write | 0.187 | — | — | 3.945 | 13.316 | 4.168 | 1.457 |
| invalid_05.json | D2 | unsafe_write | 0.175 | — | — | 4.183 | 13.179 | 4.176 | 1.461 |
| invalid_05.json | E | unsafe_write | 0.186 | — | — | 4.378 | 16.681 | 4.289 | 1.461 |
| invalid_06.json | A | unsafe_write | 0.175 | — | — | 5.074 | 16.646 | 4.145 | 1.473 |
| invalid_06.json | B | unsafe_write | 0.133 | — | — | 4.836 | 16.639 | 4.133 | 1.449 |
| invalid_06.json | C | unsafe_write | 0.105 | — | — | 4.612 | 16.035 | 4.141 | 1.453 |
| invalid_06.json | D1 | unsafe_write | 0.189 | — | — | 4.520 | 15.785 | 4.164 | 1.441 |
| invalid_06.json | D2 | unsafe_write | 0.208 | — | — | 4.619 | 16.679 | 4.184 | 1.469 |
| invalid_06.json | E | unsafe_write | 0.192 | — | — | 4.470 | 15.021 | 4.293 | 1.465 |
| invalid_07.json | A | unsafe_write | 0.192 | — | — | 5.462 | 17.850 | 4.152 | 1.465 |
| invalid_07.json | B | unsafe_write | 0.118 | — | — | 4.511 | 121.008 | 4.137 | 1.457 |
| invalid_07.json | C | unsafe_write | 0.123 | — | — | 5.776 | 21.953 | 4.133 | 1.445 |
| invalid_07.json | D1 | unsafe_write | 0.198 | — | — | 4.365 | 15.258 | 4.176 | 1.457 |
| invalid_07.json | D2 | unsafe_write | 0.208 | — | — | 4.782 | 15.363 | 4.168 | 1.461 |
| invalid_07.json | E | unsafe_write | 0.199 | — | — | 4.849 | 15.108 | 4.277 | 1.457 |
| invalid_08.json | A | unsafe_write | 0.155 | — | — | 4.890 | 16.329 | 4.148 | 1.453 |
| invalid_08.json | B | unsafe_write | 0.110 | — | — | 4.364 | 14.671 | 4.137 | 1.453 |
| invalid_08.json | C | unsafe_write | 0.100 | — | — | 4.472 | 15.134 | 4.152 | 1.461 |
| invalid_08.json | D1 | unsafe_write | 0.207 | — | — | 4.496 | 15.645 | 4.188 | 1.473 |
| invalid_08.json | D2 | unsafe_write | 0.194 | — | — | 4.436 | 14.914 | 4.176 | 1.461 |
| invalid_08.json | E | unsafe_write | 0.205 | — | — | 4.645 | 15.822 | 4.301 | 1.461 |
| invalid_09.json | A | unsafe_write | 0.160 | — | — | 4.622 | 19.534 | 4.141 | 1.457 |
| invalid_09.json | B | unsafe_write | 0.137 | — | — | 5.117 | 16.627 | 4.148 | 1.453 |
| invalid_09.json | C | unsafe_write | 0.109 | — | — | 4.711 | 15.839 | 4.148 | 1.457 |
| invalid_09.json | D1 | unsafe_write | 0.226 | — | — | 5.402 | 17.499 | 4.180 | 1.465 |
| invalid_09.json | D2 | unsafe_write | 0.212 | — | — | 4.498 | 15.694 | 4.176 | 1.461 |
| invalid_09.json | E | unsafe_write | 0.211 | — | — | 5.053 | 15.905 | 4.277 | 1.445 |
| invalid_10.json | A | unsafe_write | 0.141 | — | — | 4.297 | 15.365 | 4.148 | 1.469 |
| invalid_10.json | B | unsafe_write | 0.120 | — | — | 4.881 | 16.193 | 4.148 | 1.453 |
| invalid_10.json | C | unsafe_write | 0.098 | — | — | 4.369 | 14.359 | 4.141 | 1.457 |
| invalid_10.json | D1 | unsafe_write | 0.231 | — | — | 5.058 | 16.288 | 4.180 | 1.469 |
| invalid_10.json | D2 | unsafe_write | 0.202 | — | — | 4.577 | 14.614 | 4.180 | 1.465 |
| invalid_10.json | E | unsafe_write | 0.196 | — | — | 4.592 | 14.595 | 4.285 | 1.449 |
| invalid_11.json | A | unsafe_write | 0.210 | — | — | 5.608 | 18.607 | 4.133 | 1.449 |
| invalid_11.json | B | unsafe_write | 0.135 | — | — | 4.982 | 16.899 | 4.145 | 1.461 |
| invalid_11.json | C | unsafe_write | 0.103 | — | — | 4.348 | 17.284 | 4.141 | 1.445 |
| invalid_11.json | D1 | unsafe_write | 0.196 | — | — | 4.721 | 15.593 | 4.168 | 1.453 |
| invalid_11.json | D2 | unsafe_write | 0.194 | — | — | 4.334 | 15.431 | 4.176 | 1.461 |
| invalid_11.json | E | unsafe_write | 0.202 | — | — | 4.788 | 15.577 | 4.281 | 1.457 |
| lexeme_16m.json | A | unsafe_write | 221.751 | 32.779 | 0.124 | 679.895 | 690.043 | 101.160 | 97.871 |
| lexeme_16m.json | B | success | 233.678 | 23.573 | 50.882 | 1075.212 | 1086.061 | 165.375 | 170.168 |
| lexeme_16m.json | C | success | 104.971 | 21.349 | 78.656 | 890.279 | 900.954 | 165.410 | 170.254 |
| lexeme_16m.json | D1 | success | 11.569 | 25.424 | 25.732 | 732.203 | 786.955 | 165.395 | 170.211 |
| lexeme_16m.json | D2 | success | 18.473 | 27.901 | 18.080 | 779.650 | 789.676 | 165.379 | 170.227 |
| lexeme_16m.json | E | success | 17.284 | 18.669 | 7.115 | 714.728 | 724.619 | 165.344 | 170.258 |
| lexeme_1m.json | A | unsafe_write | 13.109 | 22.186 | 0.108 | 65.073 | 74.879 | 11.191 | 7.801 |
| lexeme_1m.json | B | success | 12.288 | 20.790 | 4.793 | 112.943 | 122.243 | 15.363 | 12.410 |
| lexeme_1m.json | C | success | 6.550 | 18.851 | 5.806 | 101.125 | 111.769 | 15.426 | 12.430 |
| lexeme_1m.json | D1 | success | 1.581 | 21.541 | 2.964 | 100.148 | 108.819 | 15.293 | 12.316 |
| lexeme_1m.json | D2 | success | 2.594 | 20.024 | 1.456 | 96.108 | 105.624 | 15.277 | 12.359 |
| lexeme_1m.json | E | success | 1.452 | 18.123 | 0.746 | 95.322 | 104.965 | 15.340 | 12.359 |
| manifest_64m.json | A | success | 688.915 | 40.586 | 358.149 | 5863.867 | 5874.834 | 837.305 | 858.094 |
| manifest_64m.json | B | success | 662.607 | 39.411 | 1263.364 | 6689.446 | 6700.061 | 837.246 | 867.449 |
| manifest_64m.json | C | success | 360.600 | 43.535 | 961.441 | 6095.706 | 6106.598 | 837.270 | 867.551 |
| manifest_64m.json | D1 | success | 753.584 | 38.028 | 810.078 | 6443.168 | 6454.999 | 837.230 | 867.477 |
| manifest_64m.json | D2 | success | 764.069 | 36.855 | 803.514 | 6342.454 | 6352.493 | 837.223 | 867.426 |
| manifest_64m.json | E | success | 87.087 | 18.643 | 29.563 | 5020.413 | 5032.475 | 837.359 | 867.559 |
| schema_invalid_claim_extra.json | A | unsafe_write | 0.562 | 19.021 | — | 23.782 | 34.193 | 6.566 | 2.910 |
| schema_invalid_claim_extra.json | B | unsafe_write | 0.202 | 18.305 | — | 23.025 | 34.055 | 6.582 | 2.926 |
| schema_invalid_claim_extra.json | C | unsafe_write | 0.136 | 17.735 | — | 22.641 | 32.697 | 6.582 | 2.922 |
| schema_invalid_claim_extra.json | D1 | unsafe_write | 0.799 | 18.845 | — | 24.144 | 34.216 | 6.559 | 2.914 |
| schema_invalid_claim_extra.json | D2 | unsafe_write | 1.446 | 19.389 | — | 25.599 | 36.410 | 6.590 | 2.922 |
| schema_invalid_claim_extra.json | E | unsafe_write | 0.328 | 19.222 | — | 25.625 | 40.136 | 6.562 | 2.934 |
| valid_all.json | A | unsafe_write | 0.138 | — | — | 5.626 | 14.575 | 4.461 | 1.734 |
| valid_all.json | B | unsafe_write | 0.123 | — | — | 6.681 | 17.210 | 4.473 | 1.762 |
| valid_all.json | C | unsafe_write | 0.087 | — | — | 5.801 | 15.146 | 4.469 | 1.754 |
| valid_all.json | D1 | success | 0.911 | 21.715 | 1.803 | 66.851 | 76.479 | 7.672 | 4.184 |
| valid_all.json | D2 | success | 1.453 | 21.492 | 0.922 | 67.038 | 77.848 | 7.680 | 4.152 |
| valid_all.json | E | unsafe_write | 0.424 | 20.174 | 0.315 | 32.778 | 45.835 | 6.652 | 3.129 |
| valid_control.json | A | success | 0.660 | 19.218 | 0.099 | 57.639 | 67.112 | 6.758 | 3.203 |
| valid_control.json | B | success | 0.176 | 23.150 | 1.584 | 55.572 | 67.167 | 6.719 | 3.152 |
| valid_control.json | C | success | 0.134 | 21.330 | 1.306 | 53.375 | 62.955 | 6.758 | 3.203 |
| valid_control.json | D1 | success | 0.761 | 21.134 | 1.444 | 56.137 | 65.627 | 6.676 | 3.133 |
| valid_control.json | D2 | success | 1.336 | 20.124 | 0.446 | 55.952 | 65.095 | 6.746 | 3.199 |
| valid_control.json | E | success | 0.262 | 20.024 | 0.106 | 50.584 | 66.047 | 6.676 | 3.133 |
| valid_exponent_1000.json | A | unsafe_write | 0.173 | — | — | 4.657 | 14.177 | 4.266 | 1.500 |
| valid_exponent_1000.json | B | unsafe_write | 0.139 | — | — | 5.009 | 14.790 | 4.266 | 1.527 |
| valid_exponent_1000.json | C | unsafe_write | 0.099 | — | — | 4.904 | 14.694 | 4.262 | 1.512 |
| valid_exponent_1000.json | D1 | success | 0.857 | 21.377 | 1.340 | 58.555 | 68.301 | 6.840 | 3.301 |
| valid_exponent_1000.json | D2 | success | 1.182 | 18.802 | 0.413 | 59.502 | 69.968 | 6.891 | 3.359 |
| valid_exponent_1000.json | E | success | 0.309 | 21.263 | 0.151 | 61.560 | 72.660 | 6.859 | 3.328 |
| valid_exponent_style.json | A | unsafe_write | 0.605 | 20.117 | 0.101 | 27.511 | 36.975 | 6.484 | 2.930 |
| valid_exponent_style.json | B | success | 0.179 | 20.591 | 1.474 | 53.087 | 62.274 | 6.758 | 3.203 |
| valid_exponent_style.json | C | success | 0.139 | 18.188 | 1.282 | 49.879 | 61.582 | 6.746 | 3.203 |
| valid_exponent_style.json | D1 | success | 0.740 | 19.971 | 1.269 | 56.168 | 65.769 | 6.668 | 3.125 |
| valid_exponent_style.json | D2 | success | 1.161 | 20.841 | 0.399 | 56.705 | 66.086 | 6.730 | 3.188 |
| valid_exponent_style.json | E | success | 0.249 | 24.286 | 0.119 | 59.171 | 69.102 | 6.723 | 3.176 |
| valid_fraction_10000.json | A | unsafe_write | 4.326 | 19.351 | 0.103 | 41.996 | 73.643 | 6.973 | 3.426 |
| valid_fraction_10000.json | B | success | 3.033 | 21.546 | 3.115 | 69.911 | 79.607 | 8.555 | 5.172 |
| valid_fraction_10000.json | C | success | 1.417 | 20.514 | 3.386 | 65.714 | 74.852 | 8.547 | 5.184 |
| valid_fraction_10000.json | D1 | success | 1.101 | 20.483 | 1.905 | 69.121 | 79.291 | 8.410 | 5.016 |
| valid_fraction_10000.json | D2 | success | 1.629 | 21.701 | 0.881 | 66.180 | 81.725 | 8.449 | 4.996 |
| valid_fraction_10000.json | E | success | 0.517 | 20.759 | 0.632 | 62.125 | 78.338 | 8.418 | 4.980 |
| valid_huge_negative.json | A | unsafe_write | 0.515 | 18.529 | 0.091 | 25.133 | 34.922 | 6.461 | 2.906 |
| valid_huge_negative.json | B | success | 0.191 | 21.331 | 1.710 | 55.862 | 74.069 | 6.750 | 3.188 |
| valid_huge_negative.json | C | success | 0.124 | 21.172 | 1.413 | 56.724 | 66.767 | 6.719 | 3.164 |
| valid_huge_negative.json | D1 | success | 0.691 | 20.899 | 1.314 | 55.301 | 64.548 | 6.688 | 3.148 |
| valid_huge_negative.json | D2 | success | 1.199 | 22.834 | 0.435 | 57.758 | 66.668 | 6.746 | 3.188 |
| valid_huge_negative.json | E | success | 0.257 | 21.447 | 0.106 | 52.887 | 63.399 | 6.703 | 3.172 |
| valid_huge_positive.json | A | unsafe_write | 0.127 | — | — | 3.798 | 13.517 | 4.148 | 1.457 |
| valid_huge_positive.json | B | unsafe_write | 0.121 | — | — | 4.462 | 14.376 | 4.156 | 1.453 |
| valid_huge_positive.json | C | unsafe_write | 0.101 | — | — | 4.257 | 15.797 | 4.148 | 1.457 |
| valid_huge_positive.json | D1 | success | 0.712 | 18.409 | 1.147 | 53.328 | 69.529 | 6.684 | 3.152 |
| valid_huge_positive.json | D2 | success | 1.248 | 21.702 | 0.416 | 54.548 | 69.294 | 6.734 | 3.184 |
| valid_huge_positive.json | E | success | 0.264 | 21.623 | 0.112 | 59.570 | 70.866 | 6.730 | 3.195 |
| valid_int_overflow.json | A | unsafe_write | 0.768 | 27.217 | 0.121 | 38.061 | 50.469 | 6.492 | 2.934 |
| valid_int_overflow.json | B | success | 0.198 | 20.049 | 1.685 | 57.362 | 68.169 | 6.723 | 3.168 |
| valid_int_overflow.json | C | success | 0.143 | 19.616 | 1.545 | 55.410 | 169.737 | 6.746 | 3.199 |
| valid_int_overflow.json | D1 | success | 0.864 | 21.582 | 1.479 | 60.233 | 70.651 | 6.684 | 3.141 |
| valid_int_overflow.json | D2 | success | 1.244 | 23.981 | 0.489 | 62.917 | 73.699 | 6.738 | 3.184 |
| valid_int_overflow.json | E | success | 0.258 | 21.052 | 0.099 | 52.509 | 61.476 | 6.695 | 3.156 |
| valid_negative_zero.json | A | unsafe_write | 0.554 | 20.005 | 0.098 | 26.831 | 37.220 | 6.473 | 2.918 |
| valid_negative_zero.json | B | unsafe_write | 0.201 | 21.989 | 1.712 | 30.386 | 40.983 | 6.484 | 2.953 |
| valid_negative_zero.json | C | success | 0.144 | 17.977 | 1.177 | 52.468 | 63.521 | 6.750 | 3.203 |
| valid_negative_zero.json | D1 | success | 0.825 | 22.968 | 1.287 | 60.264 | 70.534 | 6.684 | 3.141 |
| valid_negative_zero.json | D2 | success | 1.556 | 24.816 | 0.449 | 71.469 | 90.188 | 6.742 | 3.199 |
| valid_negative_zero.json | E | unsafe_write | 0.234 | 19.454 | 0.112 | 26.347 | 35.926 | 6.445 | 2.926 |
| valid_negative_zero_float.json | A | success | 0.616 | 21.197 | 0.107 | 56.621 | 67.701 | 6.746 | 3.203 |
| valid_negative_zero_float.json | B | success | 0.214 | 20.482 | 1.579 | 54.753 | 65.927 | 6.730 | 3.184 |
| valid_negative_zero_float.json | C | success | 0.169 | 25.470 | 1.645 | 71.792 | 83.418 | 6.750 | 3.199 |
| valid_negative_zero_float.json | D1 | success | 0.859 | 21.444 | 1.397 | 57.249 | 73.492 | 6.668 | 3.121 |
| valid_negative_zero_float.json | D2 | success | 1.227 | 19.120 | 0.462 | 50.660 | 59.818 | 6.715 | 3.180 |
| valid_negative_zero_float.json | E | success | 0.290 | 19.749 | 0.105 | 53.293 | 69.654 | 6.730 | 3.184 |
| valid_precision.json | A | unsafe_write | 0.629 | 24.141 | 0.125 | 32.046 | 55.668 | 6.480 | 2.930 |
| valid_precision.json | B | success | 0.219 | 20.717 | 1.498 | 53.582 | 62.471 | 6.762 | 3.215 |
| valid_precision.json | C | success | 0.144 | 19.682 | 1.386 | 53.558 | 66.056 | 6.758 | 3.199 |
| valid_precision.json | D1 | success | 0.785 | 20.290 | 1.263 | 55.295 | 64.895 | 6.672 | 3.141 |
| valid_precision.json | D2 | success | 1.154 | 20.219 | 0.394 | 53.644 | 62.503 | 6.758 | 3.211 |
| valid_precision.json | E | success | 0.233 | 23.061 | 0.124 | 57.075 | 67.531 | 6.754 | 3.203 |
| valid_trailing_zeros.json | A | unsafe_write | 0.512 | 22.608 | 0.116 | 29.750 | 145.943 | 6.469 | 2.930 |
| valid_trailing_zeros.json | B | success | 0.191 | 20.282 | 1.378 | 53.812 | 65.141 | 6.754 | 3.207 |
| valid_trailing_zeros.json | C | success | 0.132 | 21.221 | 1.129 | 53.150 | 62.810 | 6.750 | 3.203 |
| valid_trailing_zeros.json | D1 | success | 0.761 | 21.245 | 1.365 | 56.698 | 65.642 | 6.660 | 3.117 |
| valid_trailing_zeros.json | D2 | success | 1.121 | 22.464 | 0.412 | 56.326 | 66.536 | 6.754 | 3.203 |
| valid_trailing_zeros.json | E | success | 0.231 | 21.544 | 0.103 | 53.572 | 62.942 | 6.723 | 3.184 |
| valid_uint_overflow.json | A | unsafe_write | 0.574 | 21.935 | 0.115 | 29.980 | 40.359 | 6.473 | 2.910 |
| valid_uint_overflow.json | B | success | 0.206 | 17.598 | 1.391 | 50.505 | 59.635 | 6.750 | 3.184 |
| valid_uint_overflow.json | C | success | 0.159 | 22.063 | 1.413 | 56.808 | 66.975 | 6.723 | 3.180 |
| valid_uint_overflow.json | D1 | success | 0.766 | 19.309 | 1.232 | 53.665 | 62.759 | 6.672 | 3.133 |
| valid_uint_overflow.json | D2 | success | 1.148 | 23.849 | 0.516 | 60.196 | 70.103 | 6.691 | 3.133 |
| valid_uint_overflow.json | E | success | 0.278 | 22.044 | 0.113 | 58.429 | 75.134 | 6.680 | 3.160 |
| valid_zero_exponent.json | A | unsafe_write | 0.612 | 20.694 | 0.118 | 28.491 | 41.142 | 6.473 | 2.930 |
| valid_zero_exponent.json | B | success | 0.226 | 22.117 | 1.762 | 61.537 | 72.992 | 6.766 | 3.215 |
| valid_zero_exponent.json | C | success | 0.169 | 21.962 | 1.341 | 58.996 | 70.155 | 6.766 | 3.215 |
| valid_zero_exponent.json | D1 | success | 0.886 | 22.725 | 1.341 | 58.243 | 68.681 | 6.684 | 3.137 |
| valid_zero_exponent.json | D2 | success | 1.348 | 24.904 | 0.410 | 65.000 | 92.243 | 6.680 | 3.129 |
| valid_zero_exponent.json | E | success | 0.258 | 19.880 | 0.111 | 51.744 | 67.949 | 6.723 | 3.184 |
### Linux

| Документ | ID | Итог | parse | V2 | serialize | цикл/отказ | wall | Peak RSS/WS | PeakPagefile |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| depth_1000.json | A | success | 0.977 | 16.544 | 1.307 | 77.113 | 89.537 | 18.480 | н/д |
| depth_1000.json | B | success | 0.715 | 20.457 | 2.915 | 82.435 | 95.127 | 18.480 | н/д |
| depth_1000.json | C | success | 0.144 | 20.097 | 2.667 | 79.934 | 91.130 | 18.480 | н/д |
| depth_1000.json | D1 | success | 0.925 | 21.066 | 2.499 | 86.987 | 98.979 | 18.480 | н/д |
| depth_1000.json | D2 | success | 2.338 | 20.088 | 0.516 | 91.282 | 104.879 | 18.480 | н/д |
| depth_1000.json | E | success | 2.759 | 14.010 | 1.142 | 77.608 | 88.642 | 18.480 | н/д |
| depth_100000.json | A | unsafe_write | 29.505 | — | — | 64.021 | 79.505 | 25.367 | н/д |
| depth_100000.json | B | unsafe_write | 12.162 | — | — | 44.487 | 60.294 | 18.980 | н/д |
| depth_100000.json | C | unsafe_write | 13.514 | — | — | 45.405 | 61.304 | 18.980 | н/д |
| depth_100000.json | D1 | unsafe_write | 2.776 | — | — | 30.362 | 43.304 | 18.980 | н/д |
| depth_100000.json | D2 | unsafe_write | 2.800 | — | — | 28.223 | 40.461 | 18.980 | н/д |
| depth_100000.json | E | unsafe_write | 3.868 | — | — | 30.152 | 43.354 | 18.980 | н/д |
| depth_64.json | A | success | 0.804 | 16.860 | 1.211 | 74.032 | 85.443 | 19.047 | н/д |
| depth_64.json | B | success | 0.644 | 17.201 | 2.151 | 73.418 | 85.769 | 19.047 | н/д |
| depth_64.json | C | success | 0.053 | 17.978 | 2.151 | 73.573 | 85.122 | 19.047 | н/д |
| depth_64.json | D1 | success | 0.424 | 17.400 | 1.954 | 74.483 | 84.803 | 19.047 | н/д |
| depth_64.json | D2 | success | 1.847 | 18.238 | 0.321 | 81.045 | 93.048 | 19.047 | н/д |
| depth_64.json | E | success | 2.560 | 14.439 | 1.091 | 74.818 | 88.276 | 19.047 | н/д |
| duplicate_known.json | A | unsafe_write | 0.798 | — | — | 22.251 | 34.669 | 19.422 | н/д |
| duplicate_known.json | B | unsafe_write | 0.603 | — | — | 23.297 | 35.231 | 19.422 | н/д |
| duplicate_known.json | C | unsafe_write | 0.068 | — | — | 27.493 | 41.134 | 19.422 | н/д |
| duplicate_known.json | D1 | unsafe_write | 1.154 | — | — | 20.987 | 33.443 | 19.422 | н/д |
| duplicate_known.json | D2 | unsafe_write | 1.495 | — | — | 21.747 | 34.364 | 19.422 | н/д |
| duplicate_known.json | E | unsafe_write | 2.680 | — | — | 26.819 | 39.579 | 19.422 | н/д |
| duplicate_opaque.json | A | unsafe_write | 0.722 | — | — | 22.469 | 34.192 | 19.688 | н/д |
| duplicate_opaque.json | B | unsafe_write | 0.542 | — | — | 23.127 | 34.808 | 19.688 | н/д |
| duplicate_opaque.json | C | unsafe_write | 0.055 | — | — | 22.366 | 34.351 | 19.688 | н/д |
| duplicate_opaque.json | D1 | unsafe_write | 1.297 | — | — | 23.033 | 35.780 | 19.688 | н/д |
| duplicate_opaque.json | D2 | unsafe_write | 1.707 | — | — | 23.218 | 35.900 | 19.688 | н/д |
| duplicate_opaque.json | E | unsafe_write | 2.828 | — | — | 27.281 | 41.771 | 19.688 | н/д |
| invalid_00.json | A | unsafe_write | 1.460 | — | — | 23.217 | 36.261 | 19.945 | н/д |
| invalid_00.json | B | unsafe_write | 1.216 | — | — | 22.312 | 35.388 | 19.945 | н/д |
| invalid_00.json | C | unsafe_write | 1.099 | — | — | 22.532 | 34.958 | 19.945 | н/д |
| invalid_00.json | D1 | unsafe_write | 1.004 | — | — | 21.570 | 33.615 | 19.945 | н/д |
| invalid_00.json | D2 | unsafe_write | 1.314 | — | — | 23.502 | 35.674 | 19.945 | н/д |
| invalid_00.json | E | unsafe_write | 3.978 | — | — | 25.838 | 38.131 | 19.945 | н/д |
| invalid_01.json | A | unsafe_write | 1.329 | — | — | 22.544 | 38.157 | 20.320 | н/д |
| invalid_01.json | B | unsafe_write | 1.254 | — | — | 22.109 | 34.855 | 20.320 | н/д |
| invalid_01.json | C | unsafe_write | 1.209 | — | — | 23.166 | 38.214 | 20.320 | н/д |
| invalid_01.json | D1 | unsafe_write | 1.011 | — | — | 20.980 | 32.868 | 20.320 | н/д |
| invalid_01.json | D2 | unsafe_write | 1.297 | — | — | 20.854 | 33.319 | 20.320 | н/д |
| invalid_01.json | E | unsafe_write | 3.519 | — | — | 24.243 | 36.725 | 20.320 | н/д |
| invalid_02.json | A | unsafe_write | 1.415 | — | — | 22.763 | 36.259 | 20.688 | н/д |
| invalid_02.json | B | unsafe_write | 1.378 | — | — | 22.757 | 35.602 | 20.688 | н/д |
| invalid_02.json | C | unsafe_write | 1.065 | — | — | 22.503 | 35.299 | 20.688 | н/д |
| invalid_02.json | D1 | unsafe_write | 1.189 | — | — | 22.679 | 35.942 | 20.688 | н/д |
| invalid_02.json | D2 | unsafe_write | 1.333 | — | — | 22.472 | 35.307 | 20.688 | н/д |
| invalid_02.json | E | unsafe_write | 3.568 | — | — | 24.752 | 37.470 | 20.688 | н/д |
| invalid_03.json | A | unsafe_write | 1.409 | — | — | 22.875 | 36.143 | 21.059 | н/д |
| invalid_03.json | B | unsafe_write | 1.268 | — | — | 22.389 | 34.757 | 21.059 | н/д |
| invalid_03.json | C | unsafe_write | 1.056 | — | — | 22.256 | 34.419 | 21.059 | н/д |
| invalid_03.json | D1 | unsafe_write | 0.929 | — | — | 21.833 | 33.796 | 21.059 | н/д |
| invalid_03.json | D2 | unsafe_write | 1.258 | — | — | 22.022 | 33.790 | 21.059 | н/д |
| invalid_03.json | E | unsafe_write | 3.480 | — | — | 24.658 | 36.650 | 21.059 | н/д |
| invalid_04.json | A | unsafe_write | 1.355 | — | — | 21.553 | 34.334 | 21.398 | н/д |
| invalid_04.json | B | unsafe_write | 1.275 | — | — | 22.967 | 36.313 | 21.398 | н/д |
| invalid_04.json | C | unsafe_write | 1.065 | — | — | 21.272 | 33.547 | 21.398 | н/д |
| invalid_04.json | D1 | unsafe_write | 1.178 | — | — | 22.519 | 35.211 | 21.398 | н/д |
| invalid_04.json | D2 | unsafe_write | 1.440 | — | — | 23.027 | 36.549 | 21.398 | н/д |
| invalid_04.json | E | unsafe_write | 4.365 | — | — | 30.634 | 45.024 | 21.398 | н/д |
| invalid_05.json | A | unsafe_write | 1.496 | — | — | 26.901 | 42.143 | 21.398 | н/д |
| invalid_05.json | B | unsafe_write | 1.343 | — | — | 23.977 | 37.721 | 21.398 | н/д |
| invalid_05.json | C | unsafe_write | 1.110 | — | — | 26.696 | 40.586 | 21.398 | н/д |
| invalid_05.json | D1 | unsafe_write | 1.180 | — | — | 28.429 | 43.054 | 21.398 | н/д |
| invalid_05.json | D2 | unsafe_write | 1.571 | — | — | 27.655 | 42.953 | 21.398 | н/д |
| invalid_05.json | E | unsafe_write | 4.457 | — | — | 29.144 | 42.916 | 21.398 | н/д |
| invalid_06.json | A | unsafe_write | 1.517 | — | — | 26.594 | 40.732 | 21.980 | н/д |
| invalid_06.json | B | unsafe_write | 1.351 | — | — | 26.972 | 40.950 | 21.980 | н/д |
| invalid_06.json | C | unsafe_write | 1.208 | — | — | 25.938 | 39.779 | 21.980 | н/д |
| invalid_06.json | D1 | unsafe_write | 1.094 | — | — | 24.573 | 39.063 | 21.980 | н/д |
| invalid_06.json | D2 | unsafe_write | 1.548 | — | — | 26.837 | 43.597 | 21.980 | н/д |
| invalid_06.json | E | unsafe_write | 4.473 | — | — | 30.576 | 45.155 | 21.980 | н/д |
| invalid_07.json | A | unsafe_write | 1.596 | — | — | 27.237 | 41.443 | 22.176 | н/д |
| invalid_07.json | B | unsafe_write | 1.402 | — | — | 23.454 | 37.415 | 22.176 | н/д |
| invalid_07.json | C | unsafe_write | 1.202 | — | — | 27.164 | 39.538 | 22.176 | н/д |
| invalid_07.json | D1 | unsafe_write | 1.364 | — | — | 26.034 | 39.622 | 22.176 | н/д |
| invalid_07.json | D2 | unsafe_write | 1.550 | — | — | 25.060 | 39.225 | 22.176 | н/д |
| invalid_07.json | E | unsafe_write | 3.931 | — | — | 29.613 | 44.774 | 22.176 | н/д |
| invalid_08.json | A | unsafe_write | 1.733 | — | — | 27.796 | 43.231 | 22.566 | н/д |
| invalid_08.json | B | unsafe_write | 1.576 | — | — | 26.273 | 40.378 | 22.566 | н/д |
| invalid_08.json | C | unsafe_write | 1.073 | — | — | 24.033 | 39.420 | 22.566 | н/д |
| invalid_08.json | D1 | unsafe_write | 1.117 | — | — | 25.590 | 39.835 | 22.566 | н/д |
| invalid_08.json | D2 | unsafe_write | 1.493 | — | — | 25.767 | 39.301 | 22.566 | н/д |
| invalid_08.json | E | unsafe_write | 4.597 | — | — | 30.250 | 45.469 | 22.566 | н/д |
| invalid_09.json | A | unsafe_write | 1.778 | — | — | 26.366 | 41.262 | 22.801 | н/д |
| invalid_09.json | B | unsafe_write | 1.719 | — | — | 27.386 | 41.540 | 22.801 | н/д |
| invalid_09.json | C | unsafe_write | 1.125 | — | — | 28.080 | 43.164 | 22.801 | н/д |
| invalid_09.json | D1 | unsafe_write | 1.046 | — | — | 26.675 | 40.637 | 22.801 | н/д |
| invalid_09.json | D2 | unsafe_write | 1.518 | — | — | 26.027 | 40.100 | 22.801 | н/д |
| invalid_09.json | E | unsafe_write | 4.320 | — | — | 30.062 | 43.944 | 22.801 | н/д |
| invalid_10.json | A | unsafe_write | 1.575 | — | — | 23.984 | 42.187 | 22.996 | н/д |
| invalid_10.json | B | unsafe_write | 1.398 | — | — | 25.515 | 39.496 | 22.996 | н/д |
| invalid_10.json | C | unsafe_write | 1.290 | — | — | 26.234 | 39.805 | 22.996 | н/д |
| invalid_10.json | D1 | unsafe_write | 1.049 | — | — | 26.391 | 39.736 | 22.996 | н/д |
| invalid_10.json | D2 | unsafe_write | 1.606 | — | — | 25.114 | 40.333 | 22.996 | н/д |
| invalid_10.json | E | unsafe_write | 4.259 | — | — | 28.440 | 41.822 | 22.996 | н/д |
| invalid_11.json | A | unsafe_write | 1.451 | — | — | 23.104 | 36.720 | 23.195 | н/д |
| invalid_11.json | B | unsafe_write | 1.549 | — | — | 26.774 | 40.745 | 23.195 | н/д |
| invalid_11.json | C | unsafe_write | 1.339 | — | — | 24.667 | 39.112 | 23.195 | н/д |
| invalid_11.json | D1 | unsafe_write | 1.338 | — | — | 28.940 | 44.943 | 23.195 | н/д |
| invalid_11.json | D2 | unsafe_write | 1.528 | — | — | 27.771 | 41.708 | 23.195 | н/д |
| invalid_11.json | E | unsafe_write | 4.213 | — | — | 29.349 | 44.587 | 23.195 | н/д |
| lexeme_16m.json | A | unsafe_write | 178.403 | 28.910 | 1.362 | 778.835 | 796.061 | 93.410 | н/д |
| lexeme_16m.json | B | success | 176.624 | 20.345 | 43.683 | 1111.560 | 1128.341 | 165.516 | н/д |
| lexeme_16m.json | C | success | 94.300 | 21.964 | 71.251 | 1005.536 | 1021.994 | 151.820 | н/д |
| lexeme_16m.json | D1 | success | 19.358 | 29.737 | 25.767 | 894.312 | 912.757 | 151.422 | н/д |
| lexeme_16m.json | D2 | success | 23.139 | 26.717 | 17.730 | 898.942 | 918.975 | 151.320 | н/д |
| lexeme_16m.json | E | success | 24.132 | 15.888 | 19.334 | 891.980 | 907.782 | 165.496 | н/д |
| lexeme_1m.json | A | unsafe_write | 13.318 | 21.752 | 1.318 | 105.438 | 118.607 | 37.828 | н/д |
| lexeme_1m.json | B | success | 9.990 | 21.060 | 5.240 | 140.585 | 152.159 | 37.828 | н/д |
| lexeme_1m.json | C | success | 7.375 | 21.502 | 6.228 | 142.898 | 154.813 | 37.828 | н/д |
| lexeme_1m.json | D1 | success | 1.448 | 21.275 | 3.423 | 135.396 | 147.370 | 37.828 | н/д |
| lexeme_1m.json | D2 | success | 3.331 | 20.590 | 1.407 | 136.315 | 150.345 | 37.828 | н/д |
| lexeme_1m.json | E | success | 4.277 | 15.506 | 2.218 | 130.906 | 142.228 | 37.828 | н/д |
| manifest_64m.json | A | success | 707.671 | 51.728 | 301.385 | 6586.091 | 6599.037 | 775.359 | н/д |
| manifest_64m.json | B | success | 639.392 | 53.409 | 1297.885 | 7495.224 | 7511.364 | 775.598 | н/д |
| manifest_64m.json | C | success | 254.253 | 52.266 | 1031.157 | 6899.057 | 6913.110 | 775.461 | н/д |
| manifest_64m.json | D1 | success | 762.659 | 52.215 | 884.704 | 7213.172 | 7226.452 | 775.367 | н/д |
| manifest_64m.json | D2 | success | 809.710 | 52.351 | 812.470 | 7014.409 | 7028.834 | 775.430 | н/д |
| manifest_64m.json | E | success | 150.681 | 16.373 | 65.625 | 6012.917 | 6028.641 | 775.230 | н/д |
| schema_invalid_claim_extra.json | A | unsafe_write | 1.168 | 24.046 | — | 57.765 | 73.755 | 87.605 | н/д |
| schema_invalid_claim_extra.json | B | unsafe_write | 1.149 | 34.551 | — | 71.636 | 88.886 | 87.605 | н/д |
| schema_invalid_claim_extra.json | C | unsafe_write | 0.074 | 24.817 | — | 55.001 | 73.092 | 87.605 | н/д |
| schema_invalid_claim_extra.json | D1 | unsafe_write | 0.482 | 22.750 | — | 48.274 | 69.125 | 87.605 | н/д |
| schema_invalid_claim_extra.json | D2 | unsafe_write | 1.791 | 20.487 | — | 45.676 | 58.663 | 87.605 | н/д |
| schema_invalid_claim_extra.json | E | unsafe_write | 2.604 | 19.183 | — | 42.778 | 56.343 | 87.605 | н/д |
| valid_all.json | A | unsafe_write | 1.285 | — | — | 25.029 | 37.509 | 87.605 | н/д |
| valid_all.json | B | unsafe_write | 1.252 | — | — | 24.563 | 36.678 | 87.605 | н/д |
| valid_all.json | C | unsafe_write | 1.184 | — | — | 27.075 | 40.316 | 87.605 | н/д |
| valid_all.json | D1 | success | 0.654 | 20.721 | 2.156 | 113.253 | 132.525 | 87.605 | н/д |
| valid_all.json | D2 | success | 1.954 | 18.135 | 0.349 | 81.803 | 93.944 | 87.605 | н/д |
| valid_all.json | E | unsafe_write | 2.565 | 16.276 | 1.245 | 49.014 | 63.069 | 87.605 | н/д |
| valid_control.json | A | success | 0.896 | 20.213 | 1.593 | 76.608 | 89.100 | 87.605 | н/д |
| valid_control.json | B | success | 0.565 | 20.748 | 2.441 | 80.610 | 91.877 | 87.605 | н/д |
| valid_control.json | C | success | 0.056 | 17.648 | 2.174 | 71.390 | 82.837 | 87.605 | н/д |
| valid_control.json | D1 | success | 0.423 | 20.032 | 2.268 | 79.655 | 92.415 | 87.605 | н/д |
| valid_control.json | D2 | success | 1.981 | 17.190 | 0.291 | 74.410 | 85.469 | 87.605 | н/д |
| valid_control.json | E | success | 4.167 | 18.807 | 0.995 | 94.312 | 106.421 | 87.605 | н/д |
| valid_exponent_1000.json | A | unsafe_write | 1.436 | — | — | 23.360 | 36.165 | 87.605 | н/д |
| valid_exponent_1000.json | B | unsafe_write | 1.305 | — | — | 25.117 | 42.589 | 87.605 | н/д |
| valid_exponent_1000.json | C | unsafe_write | 1.136 | — | — | 23.902 | 37.880 | 87.605 | н/д |
| valid_exponent_1000.json | D1 | success | 0.465 | 19.465 | 2.344 | 81.341 | 92.334 | 87.605 | н/д |
| valid_exponent_1000.json | D2 | success | 1.742 | 19.547 | 0.377 | 85.650 | 96.671 | 87.605 | н/д |
| valid_exponent_1000.json | E | success | 2.754 | 16.898 | 1.107 | 80.360 | 92.390 | 87.605 | н/д |
| valid_exponent_style.json | A | unsafe_write | 0.816 | 16.897 | 1.455 | 44.763 | 58.193 | 87.605 | н/д |
| valid_exponent_style.json | B | success | 0.654 | 19.236 | 2.601 | 84.819 | 96.679 | 87.605 | н/д |
| valid_exponent_style.json | C | success | 0.063 | 19.848 | 2.461 | 83.836 | 96.050 | 87.605 | н/д |
| valid_exponent_style.json | D1 | success | 0.472 | 21.540 | 2.336 | 89.985 | 102.049 | 87.605 | н/д |
| valid_exponent_style.json | D2 | success | 1.910 | 19.781 | 0.338 | 85.524 | 103.155 | 87.605 | н/д |
| valid_exponent_style.json | E | success | 2.514 | 17.039 | 1.091 | 78.333 | 91.298 | 87.605 | н/д |
| valid_fraction_10000.json | A | unsafe_write | 2.789 | 18.307 | 1.057 | 52.573 | 65.430 | 87.605 | н/д |
| valid_fraction_10000.json | B | success | 2.842 | 23.824 | 2.794 | 100.249 | 112.286 | 87.605 | н/д |
| valid_fraction_10000.json | C | success | 1.860 | 20.901 | 3.263 | 94.578 | 106.644 | 87.605 | н/д |
| valid_fraction_10000.json | D1 | success | 0.691 | 19.755 | 2.357 | 89.566 | 100.597 | 87.605 | н/д |
| valid_fraction_10000.json | D2 | success | 2.143 | 18.266 | 0.646 | 88.855 | 101.496 | 87.605 | н/д |
| valid_fraction_10000.json | E | success | 2.679 | 15.828 | 1.283 | 89.190 | 101.014 | 87.605 | н/д |
| valid_huge_negative.json | A | unsafe_write | 0.797 | 18.821 | 1.415 | 45.008 | 60.401 | 87.605 | н/д |
| valid_huge_negative.json | B | success | 0.614 | 18.820 | 2.496 | 77.270 | 89.714 | 87.605 | н/д |
| valid_huge_negative.json | C | success | 0.059 | 21.088 | 2.535 | 85.458 | 97.571 | 87.605 | н/д |
| valid_huge_negative.json | D1 | success | 0.486 | 20.899 | 2.284 | 77.286 | 88.438 | 87.605 | н/д |
| valid_huge_negative.json | D2 | success | 1.607 | 16.830 | 0.262 | 71.613 | 83.071 | 87.605 | н/д |
| valid_huge_negative.json | E | success | 2.561 | 14.517 | 1.074 | 72.504 | 83.687 | 87.605 | н/д |
| valid_huge_positive.json | A | unsafe_write | 1.968 | — | — | 26.204 | 41.865 | 87.605 | н/д |
| valid_huge_positive.json | B | unsafe_write | 1.226 | — | — | 21.465 | 33.772 | 87.605 | н/д |
| valid_huge_positive.json | C | unsafe_write | 1.122 | — | — | 23.514 | 35.621 | 87.605 | н/д |
| valid_huge_positive.json | D1 | success | 0.515 | 21.397 | 2.401 | 80.990 | 93.464 | 87.605 | н/д |
| valid_huge_positive.json | D2 | success | 1.807 | 19.855 | 0.298 | 83.528 | 94.892 | 87.605 | н/д |
| valid_huge_positive.json | E | success | 2.670 | 16.414 | 1.077 | 73.377 | 84.786 | 87.605 | н/д |
| valid_int_overflow.json | A | unsafe_write | 0.772 | 16.070 | 1.404 | 38.888 | 52.012 | 87.605 | н/д |
| valid_int_overflow.json | B | success | 0.560 | 16.483 | 1.687 | 71.403 | 83.238 | 87.605 | н/д |
| valid_int_overflow.json | C | success | 0.055 | 17.121 | 2.017 | 72.025 | 83.900 | 87.605 | н/д |
| valid_int_overflow.json | D1 | success | 0.482 | 18.707 | 2.299 | 80.725 | 92.917 | 87.605 | н/д |
| valid_int_overflow.json | D2 | success | 1.622 | 16.711 | 0.243 | 72.720 | 82.989 | 87.605 | н/д |
| valid_int_overflow.json | E | success | 2.372 | 14.843 | 1.181 | 74.790 | 85.838 | 87.605 | н/д |
| valid_negative_zero.json | A | unsafe_write | 0.738 | 17.087 | 1.419 | 40.847 | 53.874 | 87.605 | н/д |
| valid_negative_zero.json | B | unsafe_write | 0.543 | 18.290 | 2.472 | 43.554 | 58.113 | 87.605 | н/д |
| valid_negative_zero.json | C | success | 0.059 | 18.119 | 2.239 | 75.233 | 87.741 | 87.605 | н/д |
| valid_negative_zero.json | D1 | success | 0.413 | 18.644 | 1.716 | 75.016 | 86.027 | 87.605 | н/д |
| valid_negative_zero.json | D2 | success | 1.722 | 17.890 | 0.279 | 77.312 | 89.348 | 87.605 | н/д |
| valid_negative_zero.json | E | unsafe_write | 2.769 | 15.840 | 0.996 | 43.492 | 56.057 | 87.605 | н/д |
| valid_negative_zero_float.json | A | success | 0.724 | 16.897 | 1.373 | 74.900 | 86.854 | 87.605 | н/д |
| valid_negative_zero_float.json | B | success | 0.567 | 17.451 | 2.334 | 80.059 | 91.980 | 87.605 | н/д |
| valid_negative_zero_float.json | C | success | 0.051 | 18.262 | 2.301 | 73.440 | 85.509 | 87.605 | н/д |
| valid_negative_zero_float.json | D1 | success | 0.415 | 18.209 | 2.240 | 75.864 | 86.808 | 87.605 | н/д |
| valid_negative_zero_float.json | D2 | success | 1.681 | 17.187 | 0.274 | 77.538 | 90.070 | 87.605 | н/д |
| valid_negative_zero_float.json | E | success | 2.555 | 16.103 | 1.114 | 77.682 | 88.816 | 87.605 | н/д |
| valid_precision.json | A | unsafe_write | 0.810 | 16.513 | 1.415 | 39.674 | 53.702 | 87.605 | н/д |
| valid_precision.json | B | success | 0.638 | 19.543 | 2.749 | 81.342 | 92.381 | 87.605 | н/д |
| valid_precision.json | C | success | 0.057 | 19.019 | 2.313 | 80.444 | 91.783 | 87.605 | н/д |
| valid_precision.json | D1 | success | 0.449 | 18.965 | 2.240 | 76.024 | 88.813 | 87.605 | н/д |
| valid_precision.json | D2 | success | 1.747 | 19.240 | 0.308 | 79.639 | 90.493 | 87.605 | н/д |
| valid_precision.json | E | success | 2.758 | 15.518 | 1.152 | 74.608 | 88.977 | 87.605 | н/д |
| valid_trailing_zeros.json | A | unsafe_write | 0.843 | 16.833 | 1.478 | 42.388 | 54.760 | 87.605 | н/д |
| valid_trailing_zeros.json | B | success | 0.608 | 20.072 | 2.553 | 81.288 | 93.155 | 87.605 | н/д |
| valid_trailing_zeros.json | C | success | 0.055 | 20.570 | 2.500 | 84.971 | 97.692 | 87.605 | н/д |
| valid_trailing_zeros.json | D1 | success | 0.416 | 18.325 | 2.087 | 74.086 | 86.530 | 87.605 | н/д |
| valid_trailing_zeros.json | D2 | success | 1.726 | 17.461 | 0.309 | 72.874 | 90.240 | 87.605 | н/д |
| valid_trailing_zeros.json | E | success | 2.543 | 15.286 | 1.234 | 74.203 | 86.301 | 87.605 | н/д |
| valid_uint_overflow.json | A | unsafe_write | 0.750 | 17.076 | 1.499 | 42.649 | 57.699 | 87.605 | н/д |
| valid_uint_overflow.json | B | success | 0.578 | 17.979 | 2.303 | 77.121 | 88.751 | 87.605 | н/д |
| valid_uint_overflow.json | C | success | 0.054 | 17.229 | 1.973 | 80.048 | 94.451 | 87.605 | н/д |
| valid_uint_overflow.json | D1 | success | 0.432 | 18.590 | 2.036 | 77.096 | 89.004 | 87.605 | н/д |
| valid_uint_overflow.json | D2 | success | 1.904 | 16.848 | 0.297 | 73.852 | 84.480 | 87.605 | н/д |
| valid_uint_overflow.json | E | success | 2.654 | 17.016 | 1.161 | 79.017 | 91.302 | 87.605 | н/д |
| valid_zero_exponent.json | A | unsafe_write | 0.781 | 18.697 | 1.655 | 43.619 | 57.304 | 87.605 | н/д |
| valid_zero_exponent.json | B | success | 0.595 | 19.510 | 2.412 | 76.398 | 89.838 | 87.605 | н/д |
| valid_zero_exponent.json | C | success | 0.055 | 19.697 | 2.365 | 80.308 | 92.029 | 87.605 | н/д |
| valid_zero_exponent.json | D1 | success | 0.488 | 20.431 | 2.202 | 83.603 | 95.219 | 87.605 | н/д |
| valid_zero_exponent.json | D2 | success | 1.727 | 19.518 | 0.316 | 76.326 | 88.956 | 87.605 | н/д |
| valid_zero_exponent.json | E | success | 2.513 | 16.041 | 1.150 | 77.213 | 88.848 | 87.605 | н/д |

## Рекомендация исполнителя и вопросы

**Рекомендация, не архитектурное решение:** рассматривать D2 как измеренный гибрид для дальнейшего проектирования: результат теперь включает реальный DOM-обход, повторную проверку фрагментов и сдвиг индексов. D1 оставлен как контроль другого подхода. A не обеспечивает обещание; B теряет integer -0 и отказывает на 1e400; C ограничен диапазоном экспоненты; E сохраняет многие decimal/bigint, но теряет integer -0. Fail-closed нужен независимо от библиотеки.

- O-17: политика дублей, измеренные лимиты глубины/размера/времени остаются решением управляющего/владельца. Гипотеза 1024 не является фундаментальным пределом токенизатора.
- Выбор JSON-стека, итоговая ADR, libraries.md, утверждение формата и закрытие O-13 остаются вне задачи.
- Стенд проверяет V1/V2 и сохранность, не V3/V4 и не политику ZIP bomb. Идентификаторы синтетической новой операции детерминированы для одного цикла эксперимента, это не реализация генератора OperationID продукта.
- Для перестановки changes[] потребуется отдельно определить устойчивое соответствие изменений без ID; текущий сценарий сдвигает custom_values и добавляет history.entries, существующие changes не переставляет.
