# S7: результаты Windows

**Статус:** наблюдения спайка T-004. Выбор механизма и закрытие O-13 — за управляющим отдельной ADR после Linux. Linux **не выполнялся**.

## Конфигурация

```json
{
  "cpu": "6 core 2592 MHz GenuineIntel Unknown P6 family",
  "ram_mib": 114433,
  "build_type": "Release",
  "compiler": "MSVC 19.44.35223.0",
  "baseline": "9eec002df1cd109f8f104cd966a763daf9751489"
}
```

Машина: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz, 6 ядер / 12 потоков; физическая ОЗУ по Win32_ComputerSystem — 119 992 246 272 байта. Система: `Windows-10-10.0.19045-SP0`. Замеры Release, один процесс на повтор, пять последовательных повторов.

Пик памяти включает библиотеки, V1/V2, ZIP, защитный эталон и сравнение. Это стоимость стенда/полного пути, не изолированного JSON-парсера. Результат crash не подменяется нулём памяти или успешным отказом: недоступные измерения обозначены «—».

Гипотезы стенда: reference/D max-depth 1024; max JSON bytes 134217728; timeout процесса 60.0 с. Они параметризованы и не являются нормативами продукта. Нативные ограничения библиотек проверяются до эталона.

## Зависимости и лицензии

| Порт | Версия baseline | Лицензия |
|---|---|---|
| nlohmann-json | 3.12.0#2 | MIT |
| json-schema-validator | 2.4.0 | MIT |
| rapidjson | 2025-02-26#1 | MIT |
| jsoncons | 1.9.0 | BSL-1.0 |
| minizip-ng | 4.1.0 | Zlib |
| gtest | 1.18.0 | BSD-3-Clause |

## Матрица кандидатов

A — nlohmann ordered DOM; B — nlohmann SAX с лексемами number_float и своими узлами; C — RapidJSON NumbersAsStrings + проверенный RawValue; D — строгие исходные фрагменты / nlohmann для известных полей; E — jsoncons lossless_number. Во всех случаях вложенные объекты и массивы входят в сравнение путей, типов и значений.

В полном цикле A–D используют jsoncons 2020-12 на теневом документе, E — jsoncons на собственном DOM. pboettch испытан отдельно на исходных документах и shadow: его положительный результат сам по себе не доказывает ни точность лексем, ни полноту V2.

| ID | Строгий native-разбор 12 недопустимых чисел | valid_all: полный цикл / 6 мест | Вложенность 1000 / 100000 | Fail-closed | LOC адаптера | Windows | Linux |
|---|---|---|---|---|---:|---|---|
| A | отверг все | unsafe_write / н/д | success / crash | SHA-256 guard PASS | 6 | измерен | не выполнялся |
| B | отверг все | unsafe_write / н/д | success / unsafe_write | SHA-256 guard PASS | 28 | измерен | не выполнялся |
| C | отверг все | unsafe_write / н/д | success / unsafe_write | SHA-256 guard PASS | 27 | измерен | не выполнялся |
| D | отверг все | success / ++++++ | success / unsafe_write | SHA-256 guard PASS | 10 | измерен | не выполнялся |
| E | отверг все | unsafe_write / ------ | success / unsafe_write | SHA-256 guard PASS | 26 | измерен | не выполнялся |

LOC — непустые физические строки адаптеров. B/C дополнительно используют общий event writer (47 строк). У D основной механизм — общий эталонный токенизатор (236 строк) плюс адаптер; он же нужен стенду остальных кандидатов для независимого guard. E включает в свой подсчёт адаптер и вызов валидатора. Обвязка ZIP, транзакции, памяти и main общая и не включена в числа адаптеров.

Смысл колонок 6 мест: extensions.data (оба независимых расширения), custom_values.value, generation.params, claims.value, field_definitions.default/constraints, history.details/дополнительные changes[]. «+» — лексемы, пути и другие непрозрачные значения совпали; «-» — guard обнаружил различие; «н/д» — кандидат не дошёл до проверки. Порядок object-ключей и пробелы не считаются потерей.

### Сохранность по местам на отдельных допустимых фикстурах

| ID | extensions | custom | generation | claims | fields | history |
|---|---|---|---|---|---|---|
| A | 2/11 совпали; н/д 3 | 2/11 совпали; н/д 3 | 2/11 совпали; н/д 3 | 2/11 совпали; н/д 3 | 2/11 совпали; н/д 3 | 2/11 совпали; н/д 3 |
| B | 10/11 совпали; н/д 3 | 10/11 совпали; н/д 3 | 10/11 совпали; н/д 3 | 10/11 совпали; н/д 3 | 10/11 совпали; н/д 3 | 10/11 совпали; н/д 3 |
| C | 11/11 совпали; н/д 3 | 11/11 совпали; н/д 3 | 11/11 совпали; н/д 3 | 11/11 совпали; н/д 3 | 11/11 совпали; н/д 3 | 11/11 совпали; н/д 3 |
| D | 14/14 совпали; н/д 0 | 14/14 совпали; н/д 0 | 14/14 совпали; н/д 0 | 14/14 совпали; н/д 0 | 14/14 совпали; н/д 0 | 14/14 совпали; н/д 0 |
| E | 12/14 совпали; н/д 0 | 12/14 совпали; н/д 0 | 12/14 совпали; н/д 0 | 12/14 совпали; н/д 0 | 12/14 совпали; н/д 0 | 12/14 совпали; н/д 0 |

### Дубли ключей

| ID | Непрозрачный | Известный |
|---|---|---|
| A | coalesced: ['2'] | coalesced: ['last'] |
| B | preserved: ['1', '2'] | preserved: ['first', 'last'] |
| C | preserved: ['1', '2'] | preserved: ['first', 'last'] |
| D | отказ native: [] | отказ native: [] |
| E | coalesced: ['1'] | coalesced: ['first'] |

Во всех случаях запись дублей запрещена общим guard. Это экспериментальная мера сохранности, не решение политики O-17.

## V2: реальные результаты

Пример исходного манифеста:
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
  "wall_ms": 65.87150000268593,
  "exit_code": 0
}
```

Отрицательная фикстура `schema_invalid_claim_extra.json` (лишнее поле claim, запрещённое unevaluatedProperties):
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
  "wall_ms": 81.43140000174753,
  "exit_code": 0
}
```

pboettch с установленным default_string_format_check загружает данную схему, но загрузка не доказывает поддержку 2020-12: результат на запрещённом дополнительном поле отличается от Python-reference и jsoncons. Стенд проверяет поведение закреплённого порта, а не вывод по имени версии схемы. Полные пробы обоих валидаторов и shadow для всех 35 документов находятся в results.json.

Shadow заменяет только непрозрачные поддеревья заглушками того же JSON-типа. В данной схеме для generation.params и history.details требуется object; extensions.data, plugin custom/claim values, plugin defaults/constraints и дополнительные change-ключи не ограничивают числовые значения. Нормативные поля и их ограничения остаются. Проверка лексем остаётся отдельным V1/guard; shadow не объявляет семантику плагина проверенной. При изменении схемы применимость shadow необходимо исследовать заново.

### Показательные отказы

| Фикстура | ID | Этап | Наблюдение |
|---|---|---|---|
| valid_huge_positive.json | A | parse | [json.exception.out_of_range.406] number overflow parsing '1e400' |
| valid_huge_positive.json | B | parse | [json.exception.out_of_range.406] number overflow parsing '1e400' |
| valid_huge_positive.json | C | parse | RapidJSON parse error 13 at 616 |
| valid_negative_zero.json | A | guard | unsafe write: opaque paths, types, values or numeric lexemes changed |
| valid_negative_zero.json | B | guard | unsafe write: opaque paths, types, values or numeric lexemes changed |
| valid_negative_zero.json | E | guard | unsafe write: opaque paths, types, values or numeric lexemes changed |
| valid_exponent_1000.json | A | parse | [json.exception.out_of_range.406] number overflow parsing '1e99999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999 |
| valid_exponent_1000.json | B | parse | [json.exception.out_of_range.406] number overflow parsing '1e99999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999 |
| valid_exponent_1000.json | C | parse | RapidJSON parse error 13 at 616 |
| depth_100000.json | A | процесс | 0xc00000fd |
| depth_100000.json | B | reference_v1 | hypothesis_max_depth exceeded at byte 7021 |
| depth_100000.json | C | reference_v1 | hypothesis_max_depth exceeded at byte 7021 |
| depth_100000.json | D | parse | hypothesis_max_depth exceeded at byte 7021 |
| depth_100000.json | E | parse | Data item nesting exceeds limit in options at line 1 and column 7022 |

## Замеры: все документы

Медианы по пяти отдельным процессам, время в мс, память в МиБ. Parse — нативный разбор/хранение кандидата, V2 — выбранный validator path, serialize — сериализация с правкой title. Цикл включает создание/чтение ZIP, reference, V2, защитное сравнение и транзакцию; при отказе это время до отказа, а не успешно выполненный цикл. `—` — этап не достигнут или процесс упал до отчёта. При crash доступны только время дочернего процесса wall и код завершения; память/этапы не выдумываются. Результаты всех пяти запусков и хеши входов — в build/s7/results.json.

| Документ | ID | Итог | parse | V2 | serialize | цикл/отказ | wall | PeakWorkingSet | PeakPagefile |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| depth_1000.json | A | success | 1.629 | 25.748 | 0.123 | 43.837 | 55.234 | 7.098 | 3.605 |
| depth_1000.json | B | success | 0.571 | 28.205 | 2.545 | 50.633 | 71.629 | 7.113 | 3.656 |
| depth_1000.json | C | success | 0.389 | 26.353 | 2.414 | 44.674 | 55.156 | 7.105 | 3.652 |
| depth_1000.json | D | success | 2.584 | 21.639 | 2.229 | 44.689 | 58.487 | 7.027 | 3.562 |
| depth_1000.json | E | success | 0.883 | 21.734 | 0.118 | 39.690 | 51.719 | 7.105 | 3.637 |
| depth_100000.json | A | crash | — | — | — | — | 55.588 | — | — |
| depth_100000.json | B | unsafe_write | 19.448 | — | — | 32.414 | 43.246 | 14.887 | 14.691 |
| depth_100000.json | C | unsafe_write | 15.065 | — | — | 30.256 | 41.527 | 16.051 | 16.207 |
| depth_100000.json | D | unsafe_write | 3.119 | — | — | 12.933 | 23.829 | 5.379 | 2.418 |
| depth_100000.json | E | unsafe_write | 0.926 | — | — | 14.473 | 30.113 | 4.629 | 1.906 |
| depth_64.json | A | success | 0.793 | 23.722 | 0.089 | 42.126 | 54.134 | 6.422 | 2.914 |
| depth_64.json | B | success | 0.323 | 26.192 | 1.858 | 42.091 | 57.808 | 6.441 | 2.938 |
| depth_64.json | C | success | 0.201 | 25.085 | 1.828 | 41.412 | 53.872 | 6.449 | 2.961 |
| depth_64.json | D | success | 1.239 | 25.690 | 1.393 | 38.734 | 49.670 | 6.383 | 2.883 |
| depth_64.json | E | success | 0.402 | 22.777 | 0.071 | 34.950 | 46.089 | 6.406 | 2.922 |
| duplicate_known.json | A | unsafe_write | 0.710 | — | — | 7.012 | 18.977 | 4.410 | 1.461 |
| duplicate_known.json | B | unsafe_write | 0.247 | — | — | 7.838 | 25.171 | 4.391 | 1.477 |
| duplicate_known.json | C | unsafe_write | 0.194 | — | — | 9.104 | 21.919 | 4.371 | 1.465 |
| duplicate_known.json | D | unsafe_write | 1.135 | — | — | 7.049 | 20.346 | 4.191 | 1.457 |
| duplicate_known.json | E | unsafe_write | 0.279 | — | — | 6.889 | 18.188 | 4.480 | 1.461 |
| duplicate_opaque.json | A | unsafe_write | 0.782 | — | — | 7.552 | 21.873 | 4.402 | 1.469 |
| duplicate_opaque.json | B | unsafe_write | 0.393 | — | — | 8.690 | 22.480 | 4.375 | 1.477 |
| duplicate_opaque.json | C | unsafe_write | 0.151 | — | — | 7.504 | 18.724 | 4.387 | 1.465 |
| duplicate_opaque.json | D | unsafe_write | 0.938 | — | — | 5.456 | 17.261 | 4.207 | 1.461 |
| duplicate_opaque.json | E | unsafe_write | 0.304 | — | — | 6.682 | 19.174 | 4.527 | 1.469 |
| invalid_00.json | A | unsafe_write | 0.141 | — | — | 4.307 | 125.223 | 4.117 | 1.457 |
| invalid_00.json | B | unsafe_write | 0.144 | — | — | 4.696 | 16.068 | 4.125 | 1.469 |
| invalid_00.json | C | unsafe_write | 0.110 | — | — | 4.625 | 15.532 | 4.094 | 1.457 |
| invalid_00.json | D | unsafe_write | 0.264 | — | — | 5.339 | 17.225 | 4.145 | 1.465 |
| invalid_00.json | E | unsafe_write | 0.222 | — | — | 5.517 | 17.085 | 4.242 | 1.457 |
| invalid_01.json | A | unsafe_write | 0.219 | — | — | 5.151 | 16.847 | 4.133 | 1.477 |
| invalid_01.json | B | unsafe_write | 0.149 | — | — | 5.049 | 16.466 | 4.121 | 1.469 |
| invalid_01.json | C | unsafe_write | 0.122 | — | — | 5.132 | 16.811 | 4.105 | 1.465 |
| invalid_01.json | D | unsafe_write | 0.241 | — | — | 5.085 | 16.457 | 4.137 | 1.449 |
| invalid_01.json | E | unsafe_write | 0.219 | — | — | 5.384 | 17.078 | 4.242 | 1.465 |
| invalid_02.json | A | unsafe_write | 0.194 | — | — | 5.034 | 16.649 | 4.121 | 1.461 |
| invalid_02.json | B | unsafe_write | 0.142 | — | — | 4.965 | 16.829 | 4.105 | 1.453 |
| invalid_02.json | C | unsafe_write | 0.115 | — | — | 4.750 | 16.140 | 4.117 | 1.465 |
| invalid_02.json | D | unsafe_write | 0.292 | — | — | 5.405 | 117.492 | 4.145 | 1.461 |
| invalid_02.json | E | unsafe_write | 0.222 | — | — | 4.744 | 16.214 | 4.246 | 1.457 |
| invalid_03.json | A | unsafe_write | 0.198 | — | — | 5.298 | 17.080 | 4.125 | 1.465 |
| invalid_03.json | B | unsafe_write | 0.136 | — | — | 4.792 | 16.415 | 4.109 | 1.461 |
| invalid_03.json | C | unsafe_write | 0.103 | — | — | 4.354 | 14.745 | 4.102 | 1.457 |
| invalid_03.json | D | unsafe_write | 0.241 | — | — | 4.945 | 16.046 | 4.148 | 1.453 |
| invalid_03.json | E | unsafe_write | 0.291 | — | — | 7.085 | 23.468 | 4.254 | 1.473 |
| invalid_04.json | A | unsafe_write | 0.188 | — | — | 4.206 | 123.380 | 4.121 | 1.445 |
| invalid_04.json | B | unsafe_write | 0.203 | — | — | 7.273 | 18.905 | 4.113 | 1.469 |
| invalid_04.json | C | unsafe_write | 0.125 | — | — | 5.104 | 16.236 | 4.098 | 1.453 |
| invalid_04.json | D | unsafe_write | 0.201 | — | — | 4.494 | 16.774 | 4.145 | 1.469 |
| invalid_04.json | E | unsafe_write | 0.230 | — | — | 4.857 | 22.367 | 4.242 | 1.461 |
| invalid_05.json | A | unsafe_write | 0.177 | — | — | 5.087 | 16.199 | 4.129 | 1.469 |
| invalid_05.json | B | unsafe_write | 0.141 | — | — | 4.473 | 16.054 | 4.113 | 1.449 |
| invalid_05.json | C | unsafe_write | 0.120 | — | — | 4.744 | 16.600 | 4.117 | 1.469 |
| invalid_05.json | D | unsafe_write | 0.240 | — | — | 5.789 | 19.365 | 4.156 | 1.469 |
| invalid_05.json | E | unsafe_write | 0.228 | — | — | 5.770 | 18.703 | 4.250 | 1.477 |
| invalid_06.json | A | unsafe_write | 0.220 | — | — | 6.765 | 21.568 | 4.137 | 1.465 |
| invalid_06.json | B | unsafe_write | 0.133 | — | — | 4.873 | 16.427 | 4.109 | 1.457 |
| invalid_06.json | C | unsafe_write | 0.135 | — | — | 5.435 | 18.783 | 4.109 | 1.473 |
| invalid_06.json | D | unsafe_write | 0.268 | — | — | 6.010 | 21.535 | 4.145 | 1.469 |
| invalid_06.json | E | unsafe_write | 0.270 | — | — | 5.860 | 22.786 | 4.250 | 1.480 |
| invalid_07.json | A | unsafe_write | 0.182 | — | — | 5.636 | 17.806 | 4.125 | 1.469 |
| invalid_07.json | B | unsafe_write | 0.149 | — | — | 4.812 | 17.451 | 4.113 | 1.465 |
| invalid_07.json | C | unsafe_write | 0.103 | — | — | 4.551 | 15.286 | 4.121 | 1.473 |
| invalid_07.json | D | unsafe_write | 0.264 | — | — | 4.860 | 17.759 | 4.156 | 1.473 |
| invalid_07.json | E | unsafe_write | 0.228 | — | — | 5.207 | 17.891 | 4.242 | 1.461 |
| invalid_08.json | A | unsafe_write | 0.189 | — | — | 5.288 | 18.734 | 4.125 | 1.457 |
| invalid_08.json | B | unsafe_write | 0.150 | — | — | 5.382 | 18.943 | 4.117 | 1.465 |
| invalid_08.json | C | unsafe_write | 0.090 | — | — | 4.363 | 14.673 | 4.102 | 1.453 |
| invalid_08.json | D | unsafe_write | 0.225 | — | — | 4.761 | 16.847 | 4.152 | 1.469 |
| invalid_08.json | E | unsafe_write | 0.233 | — | — | 5.730 | 19.163 | 4.246 | 1.473 |
| invalid_09.json | A | unsafe_write | 0.188 | — | — | 4.939 | 17.935 | 4.133 | 1.469 |
| invalid_09.json | B | unsafe_write | 0.137 | — | — | 5.047 | 15.725 | 4.121 | 1.465 |
| invalid_09.json | C | unsafe_write | 0.114 | — | — | 4.636 | 16.247 | 4.117 | 1.465 |
| invalid_09.json | D | unsafe_write | 0.356 | — | — | 5.257 | 19.493 | 4.141 | 1.465 |
| invalid_09.json | E | unsafe_write | 0.415 | — | — | 10.559 | 38.356 | 4.254 | 1.473 |
| invalid_10.json | A | unsafe_write | 0.154 | — | — | 4.002 | 14.242 | 4.121 | 1.453 |
| invalid_10.json | B | unsafe_write | 0.256 | — | — | 10.717 | 36.676 | 4.105 | 1.461 |
| invalid_10.json | C | unsafe_write | 0.106 | — | — | 5.240 | 19.694 | 4.105 | 1.461 |
| invalid_10.json | D | unsafe_write | 0.277 | — | — | 5.732 | 18.906 | 4.145 | 1.465 |
| invalid_10.json | E | unsafe_write | 0.228 | — | — | 5.584 | 19.489 | 4.246 | 1.477 |
| invalid_11.json | A | unsafe_write | 0.201 | — | — | 5.485 | 17.160 | 4.125 | 1.461 |
| invalid_11.json | B | unsafe_write | 0.189 | — | — | 5.202 | 16.361 | 4.113 | 1.465 |
| invalid_11.json | C | unsafe_write | 0.112 | — | — | 5.131 | 16.149 | 4.102 | 1.465 |
| invalid_11.json | D | unsafe_write | 0.248 | — | — | 5.350 | 17.936 | 4.164 | 1.484 |
| invalid_11.json | E | unsafe_write | 0.401 | — | — | 10.189 | 32.957 | 4.242 | 1.465 |
| lexeme_16m.json | A | unsafe_write | 208.995 | 29.137 | 0.099 | 640.965 | 651.027 | 85.168 | 81.938 |
| lexeme_16m.json | B | success | 229.768 | 30.316 | 64.070 | 1108.657 | 1149.144 | 165.219 | 162.121 |
| lexeme_16m.json | C | success | 126.028 | 37.250 | 95.344 | 1079.679 | 1095.031 | 165.195 | 162.074 |
| lexeme_16m.json | D | success | 16.657 | 37.440 | 22.567 | 879.932 | 897.730 | 165.180 | 162.059 |
| lexeme_16m.json | E | success | 25.514 | 25.617 | 12.299 | 829.430 | 842.619 | 165.266 | 170.160 |
| lexeme_1m.json | A | unsafe_write | 15.642 | 23.006 | 0.088 | 69.663 | 81.580 | 10.160 | 6.770 |
| lexeme_1m.json | B | success | 17.980 | 26.917 | 4.516 | 103.726 | 117.857 | 15.191 | 11.809 |
| lexeme_1m.json | C | success | 8.973 | 25.231 | 6.737 | 98.958 | 109.616 | 15.199 | 11.809 |
| lexeme_1m.json | D | success | 2.096 | 25.751 | 2.720 | 88.079 | 98.808 | 15.176 | 11.805 |
| lexeme_1m.json | E | success | 1.897 | 23.989 | 0.852 | 89.263 | 101.413 | 15.250 | 12.324 |
| manifest_64m.json | A | success | 859.865 | 40.397 | 406.608 | 7040.348 | 7055.530 | 837.258 | 858.043 |
| manifest_64m.json | B | success | 837.907 | 49.359 | 1610.185 | 8261.583 | 8275.032 | 837.121 | 835.352 |
| manifest_64m.json | C | success | 413.918 | 40.363 | 1213.217 | 7254.271 | 7267.252 | 837.176 | 835.379 |
| manifest_64m.json | D | success | 1071.848 | 41.087 | 1063.167 | 7796.880 | 7808.430 | 837.117 | 835.305 |
| manifest_64m.json | E | success | 117.208 | 22.751 | 38.239 | 5820.134 | 5831.497 | 837.254 | 867.500 |
| schema_invalid_claim_extra.json | A | unsafe_write | 0.594 | 21.413 | — | 27.431 | 40.845 | 6.559 | 2.926 |
| schema_invalid_claim_extra.json | B | unsafe_write | 0.230 | 22.207 | — | 26.890 | 42.188 | 6.555 | 2.934 |
| schema_invalid_claim_extra.json | C | unsafe_write | 0.137 | 19.936 | — | 24.707 | 44.524 | 6.543 | 2.922 |
| schema_invalid_claim_extra.json | D | unsafe_write | 0.848 | 18.265 | — | 25.386 | 36.767 | 6.516 | 2.902 |
| schema_invalid_claim_extra.json | E | unsafe_write | 0.266 | 25.890 | — | 31.836 | 48.484 | 6.555 | 2.941 |
| valid_all.json | A | unsafe_write | 0.195 | — | — | 7.665 | 18.573 | 4.430 | 1.730 |
| valid_all.json | B | unsafe_write | 0.145 | — | — | 7.010 | 18.293 | 4.414 | 1.758 |
| valid_all.json | C | unsafe_write | 0.121 | — | — | 7.601 | 19.064 | 4.414 | 1.719 |
| valid_all.json | D | success | 0.941 | 21.244 | 1.515 | 39.500 | 50.934 | 6.711 | 3.223 |
| valid_all.json | E | unsafe_write | 0.477 | 25.484 | 0.279 | 35.937 | 45.763 | 6.637 | 3.145 |
| valid_control.json | A | success | 0.687 | 24.981 | 0.105 | 37.970 | 49.154 | 6.426 | 2.930 |
| valid_control.json | B | success | 0.251 | 27.640 | 1.619 | 42.664 | 55.757 | 6.430 | 2.938 |
| valid_control.json | C | success | 0.181 | 29.302 | 1.454 | 43.924 | 55.799 | 6.430 | 2.941 |
| valid_control.json | D | success | 0.872 | 24.556 | 1.425 | 38.734 | 49.028 | 6.391 | 2.910 |
| valid_control.json | E | success | 0.283 | 22.767 | 0.059 | 35.143 | 46.923 | 6.398 | 2.914 |
| valid_exponent_1000.json | A | unsafe_write | 0.235 | — | — | 5.155 | 15.264 | 4.230 | 1.520 |
| valid_exponent_1000.json | B | unsafe_write | 0.170 | — | — | 5.590 | 17.861 | 4.223 | 1.512 |
| valid_exponent_1000.json | C | unsafe_write | 0.114 | — | — | 5.902 | 17.813 | 4.230 | 1.520 |
| valid_exponent_1000.json | D | success | 1.070 | 25.111 | 1.587 | 41.107 | 52.890 | 6.480 | 2.980 |
| valid_exponent_1000.json | E | success | 0.305 | 21.282 | 0.113 | 34.225 | 43.728 | 6.453 | 2.969 |
| valid_exponent_style.json | A | unsafe_write | 0.733 | 24.604 | 0.093 | 33.273 | 46.734 | 6.453 | 2.930 |
| valid_exponent_style.json | B | success | 0.221 | 20.998 | 1.555 | 33.915 | 53.377 | 6.445 | 2.945 |
| valid_exponent_style.json | C | success | 0.166 | 23.595 | 1.434 | 36.033 | 51.938 | 6.438 | 2.938 |
| valid_exponent_style.json | D | success | 0.959 | 26.886 | 1.422 | 42.364 | 56.909 | 6.387 | 2.902 |
| valid_exponent_style.json | E | success | 0.279 | 22.674 | 0.061 | 34.279 | 49.095 | 6.398 | 2.922 |
| valid_fraction_10000.json | A | unsafe_write | 3.800 | 22.620 | 0.088 | 37.785 | 49.686 | 6.953 | 3.441 |
| valid_fraction_10000.json | B | success | 3.550 | 26.239 | 3.645 | 56.782 | 68.097 | 7.625 | 4.414 |
| valid_fraction_10000.json | C | success | 2.005 | 28.468 | 3.264 | 58.211 | 69.305 | 7.559 | 4.344 |
| valid_fraction_10000.json | D | success | 1.335 | 25.653 | 2.148 | 54.099 | 65.875 | 7.441 | 4.125 |
| valid_fraction_10000.json | E | success | 0.697 | 22.939 | 0.841 | 48.924 | 60.820 | 7.516 | 4.234 |
| valid_huge_negative.json | A | unsafe_write | 0.745 | 24.175 | 0.094 | 31.612 | 43.799 | 6.465 | 2.945 |
| valid_huge_negative.json | B | success | 0.278 | 22.599 | 1.773 | 35.456 | 45.899 | 6.453 | 2.953 |
| valid_huge_negative.json | C | success | 0.173 | 22.962 | 1.371 | 35.686 | 47.809 | 6.445 | 2.953 |
| valid_huge_negative.json | D | success | 1.074 | 32.696 | 1.739 | 50.240 | 64.376 | 6.387 | 2.902 |
| valid_huge_negative.json | E | success | 0.288 | 22.451 | 0.060 | 33.660 | 44.626 | 6.398 | 2.922 |
| valid_huge_positive.json | A | unsafe_write | 0.189 | — | — | 4.647 | 16.651 | 4.148 | 1.469 |
| valid_huge_positive.json | B | unsafe_write | 0.160 | — | — | 4.946 | 15.958 | 4.133 | 1.461 |
| valid_huge_positive.json | C | unsafe_write | 0.111 | — | — | 4.766 | 16.145 | 4.125 | 1.461 |
| valid_huge_positive.json | D | success | 1.063 | 21.442 | 1.382 | 37.997 | 50.925 | 6.387 | 2.898 |
| valid_huge_positive.json | E | success | 0.255 | 29.175 | 0.055 | 39.964 | 55.670 | 6.430 | 2.945 |
| valid_int_overflow.json | A | unsafe_write | 0.669 | 24.312 | 0.096 | 31.453 | 41.262 | 6.449 | 2.922 |
| valid_int_overflow.json | B | success | 0.261 | 23.316 | 1.910 | 38.093 | 48.886 | 6.453 | 2.941 |
| valid_int_overflow.json | C | success | 0.190 | 21.159 | 1.343 | 38.769 | 49.539 | 6.438 | 2.941 |
| valid_int_overflow.json | D | success | 1.052 | 22.603 | 1.601 | 37.283 | 49.634 | 6.383 | 2.906 |
| valid_int_overflow.json | E | success | 0.353 | 24.757 | 0.064 | 39.128 | 147.837 | 6.398 | 2.910 |
| valid_negative_zero.json | A | unsafe_write | 0.721 | 24.037 | 0.105 | 32.444 | 43.008 | 6.422 | 2.910 |
| valid_negative_zero.json | B | unsafe_write | 0.299 | 24.857 | 1.829 | 34.821 | 46.592 | 6.434 | 2.934 |
| valid_negative_zero.json | C | success | 0.173 | 24.054 | 1.339 | 36.373 | 46.904 | 6.438 | 2.941 |
| valid_negative_zero.json | D | success | 0.851 | 24.383 | 1.238 | 40.066 | 50.881 | 6.383 | 2.902 |
| valid_negative_zero.json | E | unsafe_write | 0.234 | 20.625 | 0.071 | 29.233 | 144.138 | 6.391 | 2.895 |
| valid_negative_zero_float.json | A | success | 0.645 | 21.983 | 0.092 | 34.023 | 45.026 | 6.445 | 2.934 |
| valid_negative_zero_float.json | B | success | 0.255 | 22.648 | 1.976 | 37.710 | 49.808 | 6.434 | 2.934 |
| valid_negative_zero_float.json | C | success | 0.188 | 25.668 | 1.234 | 40.864 | 66.726 | 6.438 | 2.949 |
| valid_negative_zero_float.json | D | success | 0.962 | 22.727 | 1.409 | 39.059 | 51.282 | 6.363 | 2.879 |
| valid_negative_zero_float.json | E | success | 0.420 | 24.332 | 0.067 | 36.843 | 48.529 | 6.383 | 2.898 |
| valid_precision.json | A | unsafe_write | 0.689 | 26.291 | 0.109 | 33.456 | 152.887 | 6.445 | 2.930 |
| valid_precision.json | B | success | 0.272 | 23.747 | 1.655 | 38.144 | 50.892 | 6.438 | 2.941 |
| valid_precision.json | C | success | 0.181 | 26.421 | 1.785 | 42.334 | 56.132 | 6.434 | 2.938 |
| valid_precision.json | D | success | 1.010 | 24.359 | 1.434 | 42.009 | 55.753 | 6.398 | 2.906 |
| valid_precision.json | E | success | 0.319 | 29.744 | 0.060 | 44.678 | 55.240 | 6.430 | 2.938 |
| valid_trailing_zeros.json | A | unsafe_write | 0.654 | 22.953 | 0.082 | 31.105 | 41.301 | 6.445 | 2.938 |
| valid_trailing_zeros.json | B | success | 0.226 | 24.448 | 1.927 | 39.063 | 51.332 | 6.430 | 2.934 |
| valid_trailing_zeros.json | C | success | 0.152 | 24.335 | 1.211 | 35.664 | 46.791 | 6.434 | 2.938 |
| valid_trailing_zeros.json | D | success | 0.916 | 22.462 | 1.344 | 36.712 | 47.206 | 6.410 | 2.922 |
| valid_trailing_zeros.json | E | success | 0.266 | 22.267 | 0.067 | 34.394 | 45.652 | 6.410 | 2.918 |
| valid_uint_overflow.json | A | unsafe_write | 0.659 | 22.150 | 0.095 | 30.323 | 41.319 | 6.453 | 2.930 |
| valid_uint_overflow.json | B | success | 0.316 | 25.437 | 1.814 | 40.112 | 54.434 | 6.441 | 2.934 |
| valid_uint_overflow.json | C | success | 0.172 | 22.265 | 1.479 | 35.515 | 47.312 | 6.441 | 2.941 |
| valid_uint_overflow.json | D | success | 0.899 | 22.165 | 1.323 | 35.101 | 46.683 | 6.383 | 2.898 |
| valid_uint_overflow.json | E | success | 0.289 | 24.225 | 0.062 | 37.329 | 49.676 | 6.414 | 2.930 |
| valid_zero_exponent.json | A | unsafe_write | 0.731 | 23.748 | 0.099 | 32.384 | 45.628 | 6.430 | 2.918 |
| valid_zero_exponent.json | B | success | 0.227 | 22.752 | 1.535 | 37.099 | 154.398 | 6.438 | 2.930 |
| valid_zero_exponent.json | C | success | 0.179 | 27.832 | 1.536 | 41.757 | 54.934 | 6.438 | 2.941 |
| valid_zero_exponent.json | D | success | 0.935 | 24.441 | 1.333 | 38.294 | 51.960 | 6.355 | 2.875 |
| valid_zero_exponent.json | E | success | 0.318 | 24.462 | 0.061 | 36.671 | 47.380 | 6.395 | 2.914 |

## Наблюдения и рекомендация исполнителя

**Рекомендация, не архитектурное решение:** продолжить исследование D / гибридного хранения проверенных исходных непрозрачных фрагментов. На данной Windows-выборке D прошёл весь обязательный набор лексем; это не основание закрывать S7 без Linux. A непригоден для общей гарантии. B требует отдельного решения для integer callbacks и диапазонных отказов до number_float. C с данным Reader/флагами ограничен диапазонной обработкой экспонент. E полезен для V2 2020-12 и точных decimal/bigint, но отдельный integer -0 не сохраняется. Проверка безопасности записи обязательна и при использовании стороннего lossless API.

## Вопросы O-17 и ограничения эксперимента

- Политика дублей, размерные/глубинные лимиты и тайм-ауты остаются открыты. Успех на глубине 1000 не даёт норматива; отказ reference/D на 100000 — результат настроенной гипотезы, а не доказательство принципиального ограничения токенизации.
- A на глубине 100000 завершился переполнением стека 0xc00000fd; процесс изолирован. Остальные результаты различают native parse и reference guard.
- Для упавшего процесса нет достоверного post-exit пика памяти: он отмечен как недоступный. Пять запусков crash не усредняются с нулями.
- Данный стенд проверяет V1/V2 и сохранение содержимого, не V3/V4, доверие плагинов или ZIP-bomb-политику. Временный ZIP проверяется до атомарной замены; SHA-256 внешнего guard проверяет исходный ZIP при небезопасной записи.
- Linux не выполнялся. WSL обнаружен: Ubuntu (default), docker-desktop, оба Stopped, версия 2. Установка/включение и Linux-компилятор не запускались.
- Схема, SPEC, ADR, libraries.md и продуктовые зависимости не менялись; формат 1.0 и O-13 не объявляются утверждёнными.
