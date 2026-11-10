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
  "wall_ms": 89.86449998337775,
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
  "wall_ms": 79.874099988956,
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
| depth_1000.json | A | success | 1.648 | 19.800 | 0.112 | 38.118 | 49.665 | 7.074 | 3.586 |
| depth_1000.json | B | success | 0.473 | 19.701 | 2.347 | 36.123 | 47.272 | 7.129 | 3.676 |
| depth_1000.json | C | success | 0.335 | 22.901 | 2.367 | 40.137 | 52.203 | 7.105 | 3.656 |
| depth_1000.json | D | success | 2.328 | 23.295 | 2.276 | 41.665 | 60.692 | 6.969 | 3.520 |
| depth_1000.json | E | success | 0.717 | 23.742 | 0.133 | 41.483 | 54.657 | 7.113 | 3.645 |
| depth_100000.json | A | crash | — | — | — | — | 56.228 | — | — |
| depth_100000.json | B | unsafe_write | 19.774 | — | — | 34.459 | 46.163 | 15.102 | 15.684 |
| depth_100000.json | C | unsafe_write | 12.394 | — | — | 25.286 | 37.000 | 16.398 | 16.523 |
| depth_100000.json | D | unsafe_write | 2.993 | — | — | 11.172 | 24.512 | 5.305 | 2.422 |
| depth_100000.json | E | unsafe_write | 0.584 | — | — | 8.074 | 18.903 | 4.613 | 1.902 |
| depth_64.json | A | success | 0.669 | 24.294 | 0.087 | 35.676 | 46.839 | 6.430 | 2.918 |
| depth_64.json | B | success | 0.214 | 21.208 | 1.668 | 34.464 | 45.200 | 6.441 | 2.941 |
| depth_64.json | C | success | 0.170 | 25.865 | 1.668 | 40.363 | 50.980 | 6.453 | 2.957 |
| depth_64.json | D | success | 0.932 | 20.142 | 1.378 | 33.447 | 43.595 | 6.367 | 2.879 |
| depth_64.json | E | success | 0.378 | 26.195 | 0.064 | 39.288 | 51.386 | 6.406 | 2.922 |
| duplicate_known.json | A | unsafe_write | 0.599 | — | — | 6.615 | 25.750 | 4.348 | 1.469 |
| duplicate_known.json | B | unsafe_write | 0.230 | — | — | 7.496 | 18.488 | 4.410 | 1.457 |
| duplicate_known.json | C | unsafe_write | 0.195 | — | — | 8.036 | 19.035 | 4.375 | 1.465 |
| duplicate_known.json | D | unsafe_write | 1.124 | — | — | 5.497 | 15.691 | 4.227 | 1.457 |
| duplicate_known.json | E | unsafe_write | 0.308 | — | — | 6.791 | 17.788 | 4.500 | 1.453 |
| duplicate_opaque.json | A | unsafe_write | 0.598 | — | — | 6.331 | 17.196 | 4.375 | 1.461 |
| duplicate_opaque.json | B | unsafe_write | 0.225 | — | — | 7.625 | 19.033 | 4.355 | 1.457 |
| duplicate_opaque.json | C | unsafe_write | 0.166 | — | — | 7.699 | 18.571 | 4.406 | 1.461 |
| duplicate_opaque.json | D | unsafe_write | 0.954 | — | — | 5.310 | 15.899 | 4.227 | 1.461 |
| duplicate_opaque.json | E | unsafe_write | 0.282 | — | — | 6.449 | 17.497 | 4.480 | 1.457 |
| invalid_00.json | A | unsafe_write | 0.168 | — | — | 4.114 | 14.370 | 4.121 | 1.461 |
| invalid_00.json | B | unsafe_write | 0.137 | — | — | 4.638 | 16.683 | 4.113 | 1.461 |
| invalid_00.json | C | unsafe_write | 0.106 | — | — | 4.418 | 14.809 | 4.109 | 1.453 |
| invalid_00.json | D | unsafe_write | 0.206 | — | — | 4.628 | 15.454 | 4.145 | 1.473 |
| invalid_00.json | E | unsafe_write | 0.197 | — | — | 4.712 | 15.974 | 4.242 | 1.461 |
| invalid_01.json | A | unsafe_write | 0.186 | — | — | 5.237 | 23.902 | 4.129 | 1.477 |
| invalid_01.json | B | unsafe_write | 0.144 | — | — | 5.192 | 34.446 | 4.117 | 1.469 |
| invalid_01.json | C | unsafe_write | 0.108 | — | — | 4.098 | 15.934 | 4.113 | 1.473 |
| invalid_01.json | D | unsafe_write | 0.227 | — | — | 5.214 | 16.121 | 4.148 | 1.461 |
| invalid_01.json | E | unsafe_write | 0.217 | — | — | 4.883 | 16.046 | 4.246 | 1.465 |
| invalid_02.json | A | unsafe_write | 0.171 | — | — | 4.463 | 15.513 | 4.125 | 1.469 |
| invalid_02.json | B | unsafe_write | 0.109 | — | — | 3.761 | 13.092 | 4.109 | 1.449 |
| invalid_02.json | C | unsafe_write | 0.106 | — | — | 4.820 | 15.871 | 4.125 | 1.465 |
| invalid_02.json | D | unsafe_write | 0.235 | — | — | 4.979 | 16.288 | 4.152 | 1.469 |
| invalid_02.json | E | unsafe_write | 0.204 | — | — | 4.768 | 14.953 | 4.242 | 1.457 |
| invalid_03.json | A | unsafe_write | 0.184 | — | — | 5.004 | 17.333 | 4.125 | 1.461 |
| invalid_03.json | B | unsafe_write | 0.137 | — | — | 4.726 | 16.029 | 4.109 | 1.469 |
| invalid_03.json | C | unsafe_write | 0.094 | — | — | 4.024 | 13.691 | 4.109 | 1.469 |
| invalid_03.json | D | unsafe_write | 0.193 | — | — | 3.900 | 15.410 | 4.141 | 1.457 |
| invalid_03.json | E | unsafe_write | 0.180 | — | — | 3.956 | 14.986 | 4.238 | 1.457 |
| invalid_04.json | A | unsafe_write | 0.169 | — | — | 4.298 | 14.197 | 4.125 | 1.465 |
| invalid_04.json | B | unsafe_write | 0.184 | — | — | 4.628 | 16.930 | 4.113 | 1.457 |
| invalid_04.json | C | unsafe_write | 0.102 | — | — | 4.454 | 14.391 | 4.113 | 1.449 |
| invalid_04.json | D | unsafe_write | 0.253 | — | — | 5.348 | 17.353 | 4.145 | 1.465 |
| invalid_04.json | E | unsafe_write | 0.244 | — | — | 5.050 | 17.984 | 4.238 | 1.461 |
| invalid_05.json | A | unsafe_write | 0.191 | — | — | 5.924 | 17.605 | 4.113 | 1.449 |
| invalid_05.json | B | unsafe_write | 0.107 | — | — | 3.885 | 16.099 | 4.102 | 1.453 |
| invalid_05.json | C | unsafe_write | 0.115 | — | — | 4.757 | 15.580 | 4.117 | 1.469 |
| invalid_05.json | D | unsafe_write | 0.197 | — | — | 4.195 | 14.872 | 4.148 | 1.461 |
| invalid_05.json | E | unsafe_write | 0.215 | — | — | 5.222 | 15.929 | 4.250 | 1.469 |
| invalid_06.json | A | unsafe_write | 0.179 | — | — | 4.684 | 16.032 | 4.125 | 1.469 |
| invalid_06.json | B | unsafe_write | 0.141 | — | — | 4.252 | 118.946 | 4.109 | 1.457 |
| invalid_06.json | C | unsafe_write | 0.095 | — | — | 4.459 | 14.702 | 4.117 | 1.461 |
| invalid_06.json | D | unsafe_write | 0.217 | — | — | 4.652 | 14.973 | 4.137 | 1.469 |
| invalid_06.json | E | unsafe_write | 0.180 | — | — | 4.193 | 14.187 | 4.234 | 1.449 |
| invalid_07.json | A | unsafe_write | 0.172 | — | — | 4.918 | 20.494 | 4.129 | 1.465 |
| invalid_07.json | B | unsafe_write | 0.144 | — | — | 5.319 | 19.514 | 4.125 | 1.473 |
| invalid_07.json | C | unsafe_write | 0.105 | — | — | 4.420 | 14.906 | 4.102 | 1.469 |
| invalid_07.json | D | unsafe_write | 0.239 | — | — | 5.175 | 17.038 | 4.148 | 1.469 |
| invalid_07.json | E | unsafe_write | 0.205 | — | — | 4.983 | 15.329 | 4.238 | 1.449 |
| invalid_08.json | A | unsafe_write | 0.173 | — | — | 4.887 | 15.401 | 4.125 | 1.469 |
| invalid_08.json | B | unsafe_write | 0.122 | — | — | 4.330 | 15.021 | 4.102 | 1.449 |
| invalid_08.json | C | unsafe_write | 0.108 | — | — | 4.515 | 15.244 | 4.109 | 1.465 |
| invalid_08.json | D | unsafe_write | 0.238 | — | — | 5.068 | 17.214 | 4.141 | 1.453 |
| invalid_08.json | E | unsafe_write | 0.203 | — | — | 4.708 | 16.407 | 4.242 | 1.465 |
| invalid_09.json | A | unsafe_write | 0.161 | — | — | 4.423 | 15.015 | 4.121 | 1.457 |
| invalid_09.json | B | unsafe_write | 0.130 | — | — | 4.391 | 15.116 | 4.117 | 1.473 |
| invalid_09.json | C | unsafe_write | 0.100 | — | — | 4.430 | 14.980 | 4.109 | 1.465 |
| invalid_09.json | D | unsafe_write | 0.227 | — | — | 5.005 | 17.544 | 4.148 | 1.465 |
| invalid_09.json | E | unsafe_write | 0.187 | — | — | 4.027 | 14.596 | 4.242 | 1.453 |
| invalid_10.json | A | unsafe_write | 0.163 | — | — | 4.402 | 14.672 | 4.125 | 1.473 |
| invalid_10.json | B | unsafe_write | 0.149 | — | — | 4.745 | 16.994 | 4.129 | 1.473 |
| invalid_10.json | C | unsafe_write | 0.111 | — | — | 4.959 | 16.500 | 4.117 | 1.477 |
| invalid_10.json | D | unsafe_write | 0.191 | — | — | 4.280 | 14.620 | 4.141 | 1.461 |
| invalid_10.json | E | unsafe_write | 0.197 | — | — | 4.621 | 15.460 | 4.246 | 1.473 |
| invalid_11.json | A | unsafe_write | 0.172 | — | — | 4.438 | 15.228 | 4.129 | 1.465 |
| invalid_11.json | B | unsafe_write | 0.132 | — | — | 5.037 | 17.914 | 4.113 | 1.453 |
| invalid_11.json | C | unsafe_write | 0.116 | — | — | 4.521 | 15.117 | 4.113 | 1.457 |
| invalid_11.json | D | unsafe_write | 0.202 | — | — | 4.589 | 15.312 | 4.152 | 1.469 |
| invalid_11.json | E | unsafe_write | 0.202 | — | — | 5.115 | 15.804 | 4.246 | 1.465 |
| lexeme_16m.json | A | unsafe_write | 215.864 | 35.444 | 0.104 | 647.982 | 700.664 | 85.152 | 81.961 |
| lexeme_16m.json | B | success | 207.746 | 30.780 | 55.698 | 961.719 | 973.009 | 165.191 | 162.141 |
| lexeme_16m.json | C | success | 108.221 | 29.918 | 83.159 | 881.525 | 950.618 | 165.188 | 162.074 |
| lexeme_16m.json | D | success | 13.215 | 27.189 | 22.401 | 722.379 | 733.248 | 165.164 | 162.031 |
| lexeme_16m.json | E | success | 19.732 | 20.562 | 9.313 | 721.337 | 748.607 | 165.230 | 170.129 |
| lexeme_1m.json | A | unsafe_write | 19.010 | 29.138 | 0.113 | 99.792 | 194.450 | 10.109 | 6.680 |
| lexeme_1m.json | B | success | 18.158 | 27.474 | 6.306 | 122.798 | 135.399 | 15.191 | 11.785 |
| lexeme_1m.json | C | success | 9.376 | 27.857 | 6.901 | 101.831 | 113.033 | 15.211 | 11.805 |
| lexeme_1m.json | D | success | 1.953 | 25.683 | 2.770 | 88.631 | 100.239 | 15.125 | 11.758 |
| lexeme_1m.json | E | success | 2.245 | 34.654 | 1.046 | 107.441 | 119.871 | 15.219 | 12.328 |
| manifest_64m.json | A | success | 817.790 | 42.194 | 402.741 | 6872.504 | 6885.465 | 837.215 | 858.023 |
| manifest_64m.json | B | success | 850.697 | 43.944 | 1562.003 | 8649.333 | 8662.849 | 837.168 | 835.348 |
| manifest_64m.json | C | success | 449.831 | 57.058 | 1297.470 | 7817.200 | 7828.209 | 837.180 | 835.395 |
| manifest_64m.json | D | success | 1123.388 | 44.217 | 1119.288 | 8497.269 | 8511.525 | 837.055 | 835.277 |
| manifest_64m.json | E | success | 127.823 | 24.526 | 35.880 | 6299.216 | 6315.034 | 837.176 | 867.441 |
| schema_invalid_claim_extra.json | A | unsafe_write | 0.718 | 23.432 | — | 30.255 | 43.529 | 6.551 | 2.922 |
| schema_invalid_claim_extra.json | B | unsafe_write | 0.243 | 20.307 | — | 24.747 | 42.493 | 6.562 | 2.938 |
| schema_invalid_claim_extra.json | C | unsafe_write | 0.148 | 19.190 | — | 25.432 | 38.212 | 6.562 | 2.938 |
| schema_invalid_claim_extra.json | D | unsafe_write | 0.985 | 21.770 | — | 28.033 | 40.050 | 6.508 | 2.906 |
| schema_invalid_claim_extra.json | E | unsafe_write | 0.314 | 23.752 | — | 29.530 | 41.950 | 6.543 | 2.934 |
| valid_all.json | A | unsafe_write | 0.212 | — | — | 8.751 | 21.315 | 4.445 | 1.730 |
| valid_all.json | B | unsafe_write | 0.148 | — | — | 7.330 | 18.850 | 4.426 | 1.762 |
| valid_all.json | C | unsafe_write | 0.171 | — | — | 10.946 | 128.176 | 4.422 | 1.758 |
| valid_all.json | D | success | 1.242 | 25.021 | 1.578 | 49.076 | 61.715 | 6.703 | 3.211 |
| valid_all.json | E | unsafe_write | 0.435 | 22.739 | 0.322 | 34.472 | 47.759 | 6.625 | 3.137 |
| valid_control.json | A | success | 0.638 | 23.181 | 0.101 | 39.265 | 52.185 | 6.453 | 2.930 |
| valid_control.json | B | success | 0.197 | 23.369 | 1.696 | 35.412 | 142.781 | 6.449 | 2.965 |
| valid_control.json | C | success | 0.176 | 22.589 | 1.424 | 35.328 | 46.947 | 6.438 | 2.938 |
| valid_control.json | D | success | 0.781 | 21.932 | 1.684 | 36.113 | 98.752 | 6.383 | 2.895 |
| valid_control.json | E | success | 0.273 | 22.577 | 0.058 | 33.660 | 44.884 | 6.418 | 2.922 |
| valid_exponent_1000.json | A | unsafe_write | 0.209 | — | — | 5.332 | 16.517 | 4.227 | 1.508 |
| valid_exponent_1000.json | B | unsafe_write | 0.168 | — | — | 5.429 | 120.188 | 4.230 | 1.523 |
| valid_exponent_1000.json | C | unsafe_write | 0.117 | — | — | 5.505 | 16.804 | 4.234 | 1.523 |
| valid_exponent_1000.json | D | success | 0.908 | 22.673 | 1.419 | 39.158 | 61.834 | 6.461 | 2.980 |
| valid_exponent_1000.json | E | success | 0.406 | 34.259 | 0.157 | 52.989 | 67.093 | 6.473 | 2.992 |
| valid_exponent_style.json | A | unsafe_write | 0.703 | 25.630 | 0.098 | 34.009 | 48.000 | 6.441 | 2.922 |
| valid_exponent_style.json | B | success | 0.298 | 31.618 | 2.076 | 45.307 | 60.253 | 6.449 | 2.938 |
| valid_exponent_style.json | C | success | 0.167 | 26.165 | 1.500 | 39.749 | 54.792 | 6.449 | 2.949 |
| valid_exponent_style.json | D | success | 0.945 | 23.249 | 1.352 | 37.730 | 54.241 | 6.383 | 2.902 |
| valid_exponent_style.json | E | success | 0.264 | 29.795 | 0.061 | 41.592 | 55.942 | 6.383 | 2.891 |
| valid_fraction_10000.json | A | unsafe_write | 3.909 | 24.863 | 0.091 | 45.029 | 56.310 | 6.941 | 3.430 |
| valid_fraction_10000.json | B | success | 3.171 | 25.018 | 3.166 | 53.045 | 73.417 | 7.547 | 4.250 |
| valid_fraction_10000.json | C | success | 1.913 | 24.037 | 2.787 | 52.658 | 62.385 | 7.531 | 4.293 |
| valid_fraction_10000.json | D | success | 1.191 | 23.692 | 1.875 | 46.862 | 57.604 | 7.438 | 4.098 |
| valid_fraction_10000.json | E | success | 0.605 | 27.315 | 0.567 | 51.313 | 61.975 | 7.461 | 4.266 |
| valid_huge_negative.json | A | unsafe_write | 0.721 | 23.317 | 0.100 | 36.395 | 47.178 | 6.461 | 2.938 |
| valid_huge_negative.json | B | success | 0.234 | 22.299 | 1.429 | 33.356 | 57.772 | 6.453 | 2.949 |
| valid_huge_negative.json | C | success | 0.228 | 23.766 | 1.446 | 37.278 | 144.690 | 6.445 | 2.953 |
| valid_huge_negative.json | D | success | 0.883 | 22.794 | 1.490 | 37.151 | 49.823 | 6.375 | 2.891 |
| valid_huge_negative.json | E | success | 0.319 | 23.092 | 0.059 | 37.760 | 47.537 | 6.387 | 2.898 |
| valid_huge_positive.json | A | unsafe_write | 0.186 | — | — | 4.822 | 16.061 | 4.141 | 1.465 |
| valid_huge_positive.json | B | unsafe_write | 0.141 | — | — | 4.145 | 14.235 | 4.129 | 1.469 |
| valid_huge_positive.json | C | unsafe_write | 0.116 | — | — | 4.327 | 15.210 | 4.125 | 1.477 |
| valid_huge_positive.json | D | success | 0.962 | 25.195 | 1.915 | 41.750 | 54.513 | 6.395 | 2.914 |
| valid_huge_positive.json | E | success | 0.263 | 23.883 | 0.057 | 34.625 | 46.835 | 6.406 | 2.926 |
| valid_int_overflow.json | A | unsafe_write | 0.684 | 25.837 | 0.121 | 34.646 | 45.025 | 6.449 | 2.918 |
| valid_int_overflow.json | B | success | 0.316 | 28.947 | 2.231 | 47.121 | 61.508 | 6.465 | 2.949 |
| valid_int_overflow.json | C | success | 0.167 | 25.829 | 1.400 | 38.202 | 47.916 | 6.441 | 2.957 |
| valid_int_overflow.json | D | success | 0.902 | 20.932 | 1.340 | 41.223 | 52.734 | 6.395 | 2.914 |
| valid_int_overflow.json | E | success | 0.270 | 19.165 | 0.082 | 29.769 | 60.123 | 6.418 | 2.934 |
| valid_negative_zero.json | A | unsafe_write | 0.771 | 24.959 | 0.112 | 33.392 | 46.526 | 6.438 | 2.930 |
| valid_negative_zero.json | B | unsafe_write | 0.218 | 22.690 | 1.636 | 30.649 | 46.927 | 6.449 | 2.949 |
| valid_negative_zero.json | C | success | 0.193 | 24.425 | 1.378 | 45.723 | 59.231 | 6.438 | 2.945 |
| valid_negative_zero.json | D | success | 0.965 | 26.733 | 1.366 | 42.859 | 59.606 | 6.379 | 2.898 |
| valid_negative_zero.json | E | unsafe_write | 0.314 | 26.309 | 0.076 | 34.218 | 49.874 | 6.398 | 2.902 |
| valid_negative_zero_float.json | A | success | 1.071 | 28.814 | 0.120 | 49.977 | 69.923 | 6.449 | 2.934 |
| valid_negative_zero_float.json | B | success | 0.299 | 24.907 | 2.528 | 39.829 | 53.551 | 6.449 | 2.941 |
| valid_negative_zero_float.json | C | success | 0.216 | 25.611 | 1.574 | 41.525 | 56.183 | 6.441 | 2.938 |
| valid_negative_zero_float.json | D | success | 0.767 | 17.604 | 1.141 | 29.169 | 54.693 | 6.379 | 2.887 |
| valid_negative_zero_float.json | E | success | 0.266 | 18.130 | 0.060 | 33.034 | 48.616 | 6.410 | 2.922 |
| valid_precision.json | A | unsafe_write | 0.729 | 23.279 | 0.111 | 35.395 | 47.505 | 6.453 | 2.941 |
| valid_precision.json | B | success | 0.259 | 24.752 | 1.455 | 40.809 | 55.219 | 6.461 | 2.949 |
| valid_precision.json | C | success | 0.171 | 20.646 | 1.263 | 32.835 | 45.287 | 6.445 | 2.941 |
| valid_precision.json | D | success | 0.888 | 23.837 | 1.562 | 37.400 | 48.809 | 6.391 | 2.906 |
| valid_precision.json | E | success | 0.270 | 27.007 | 0.060 | 43.008 | 53.553 | 6.430 | 2.949 |
| valid_trailing_zeros.json | A | unsafe_write | 0.656 | 26.283 | 0.127 | 39.303 | 153.535 | 6.410 | 2.906 |
| valid_trailing_zeros.json | B | success | 0.235 | 24.403 | 2.167 | 38.136 | 50.263 | 6.453 | 2.961 |
| valid_trailing_zeros.json | C | success | 0.186 | 26.267 | 1.379 | 38.252 | 60.284 | 6.441 | 2.941 |
| valid_trailing_zeros.json | D | success | 0.873 | 24.352 | 1.414 | 41.156 | 54.002 | 6.391 | 2.906 |
| valid_trailing_zeros.json | E | success | 0.288 | 26.574 | 0.059 | 37.083 | 53.345 | 6.398 | 2.910 |
| valid_uint_overflow.json | A | unsafe_write | 0.639 | 23.371 | 0.093 | 31.418 | 42.746 | 6.449 | 2.934 |
| valid_uint_overflow.json | B | success | 0.258 | 23.722 | 1.990 | 41.852 | 52.542 | 6.465 | 2.953 |
| valid_uint_overflow.json | C | success | 0.196 | 22.863 | 1.588 | 35.482 | 50.170 | 6.426 | 2.934 |
| valid_uint_overflow.json | D | success | 0.929 | 22.069 | 1.506 | 36.173 | 47.354 | 6.395 | 2.910 |
| valid_uint_overflow.json | E | success | 0.311 | 23.580 | 0.060 | 37.273 | 53.034 | 6.422 | 2.941 |
| valid_zero_exponent.json | A | unsafe_write | 0.658 | 23.158 | 0.110 | 30.800 | 42.712 | 6.441 | 2.938 |
| valid_zero_exponent.json | B | success | 0.258 | 24.027 | 1.747 | 38.702 | 50.011 | 6.449 | 2.949 |
| valid_zero_exponent.json | C | success | 0.167 | 23.771 | 1.607 | 36.231 | 46.746 | 6.445 | 2.953 |
| valid_zero_exponent.json | D | success | 0.876 | 23.805 | 1.352 | 36.426 | 48.540 | 6.383 | 2.898 |
| valid_zero_exponent.json | E | success | 0.261 | 23.223 | 0.064 | 34.761 | 55.310 | 6.398 | 2.918 |

## Наблюдения и рекомендация исполнителя

**Рекомендация, не архитектурное решение:** продолжить исследование D / гибридного хранения проверенных исходных непрозрачных фрагментов. На данной Windows-выборке D прошёл весь обязательный набор лексем; это не основание закрывать S7 без Linux. A непригоден для общей гарантии. B требует отдельного решения для integer callbacks и диапазонных отказов до number_float. C с данным Reader/флагами ограничен диапазонной обработкой экспонент. E полезен для V2 2020-12 и точных decimal/bigint, но отдельный integer -0 не сохраняется. Проверка безопасности записи обязательна и при использовании стороннего lossless API.

## Вопросы O-17 и ограничения эксперимента

- Политика дублей, размерные/глубинные лимиты и тайм-ауты остаются открыты. Успех на глубине 1000 не даёт норматива; отказ reference/D на 100000 — результат настроенной гипотезы, а не доказательство принципиального ограничения токенизации.
- A на глубине 100000 завершился переполнением стека 0xc00000fd; процесс изолирован. Остальные результаты различают native parse и reference guard.
- Для упавшего процесса нет достоверного post-exit пика памяти: он отмечен как недоступный. Пять запусков crash не усредняются с нулями.
- Данный стенд проверяет V1/V2 и сохранение содержимого, не V3/V4, доверие плагинов или ZIP-bomb-политику. Временный ZIP проверяется до атомарной замены; SHA-256 внешнего guard проверяет исходный ZIP при небезопасной записи.
- Linux не выполнялся. WSL обнаружен: Ubuntu (default), docker-desktop, оба Stopped, версия 2. Установка/включение и Linux-компилятор не запускались.
- Схема, SPEC, ADR, libraries.md и продуктовые зависимости не менялись; формат 1.0 и O-13 не объявляются утверждёнными.
