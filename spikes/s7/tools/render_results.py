"""Render the measured Windows matrix, including failures and unavailable metrics."""
import argparse
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--results', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, default=ROOT / 'spikes/s7/RESULTS.md')
args = parser.parse_args()
data = json.loads(args.results.read_text(encoding='utf-8'))
cases = data['cases']
lines = ['# S7: результаты Windows', '',
         '**Статус:** наблюдения спайка T-004. Выбор механизма и закрытие O-13 — за управляющим отдельной ADR после Linux. Linux **не выполнялся**.', '',
         '## Конфигурация', '',
         '```json', json.dumps(data['configuration'], indent=2, ensure_ascii=False), '```', '',
         'Машина: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz, 6 ядер / 12 потоков; физическая ОЗУ по Win32_ComputerSystem — 119 992 246 272 байта. '
         'Система: `' + data['platform'] + '`. Замеры Release, один процесс на повтор, пять последовательных повторов.', '',
         'Пик памяти включает библиотеки, V1/V2, ZIP, защитный эталон и сравнение. Это стоимость стенда/полного пути, не изолированного JSON-парсера. '
         'Результат crash не подменяется нулём памяти или успешным отказом: недоступные измерения обозначены «—».', '',
         'Гипотезы стенда: reference/D max-depth 1024; max JSON bytes 134217728; timeout процесса ' + str(data['hypothesis_timeout_seconds']) +
         ' с. Они параметризованы и не являются нормативами продукта. Нативные ограничения библиотек проверяются до эталона.', '',
         '## Зависимости и лицензии', '', '| Порт | Версия baseline | Лицензия |', '|---|---|---|']
for name in ['nlohmann-json','json-schema-validator','rapidjson','jsoncons','minizip-ng','gtest']:
    port = json.loads((ROOT / '.cache/vcpkg/ports' / name / 'vcpkg.json').read_text(encoding='utf-8'))
    version = next(port[k] for k in ('version','version-semver','version-string','version-date') if k in port)
    if port.get('port-version'):
        version += '#' + str(port['port-version'])
    lines.append(f"| {name} | {version} | {port['license']} |")

def adapter_lines(start, end):
    source = (ROOT / 'spikes/s7/src/candidates.cpp').read_text(encoding='utf-8')
    body = source[source.index(start):source.index(end,source.index(start))]
    return sum(bool(line.strip()) for line in body.splitlines())

loc = {'A':adapter_lines('struct Dom final','enum class EventKind'),
       'B':adapter_lines('struct Sax :','struct RapidHandler'),
       'C':adapter_lines('struct RapidHandler','struct RawCandidate'),
       'D':adapter_lines('struct RawCandidate','}  // namespace') ,
       'E':sum(bool(x.strip()) for x in (ROOT/'spikes/s7/src/jsoncons_candidate.cpp').read_text(encoding='utf-8').splitlines())}
shared = adapter_lines('enum class EventKind','struct Sax :')
reference_loc = sum(bool(x.strip()) for x in (ROOT/'spikes/s7/src/reference.cpp').read_text(encoding='utf-8').splitlines())
lines += ['', '## Матрица кандидатов', '',
          'A — nlohmann ordered DOM; B — nlohmann SAX с лексемами number_float и своими узлами; '
          'C — RapidJSON NumbersAsStrings + проверенный RawValue; D — строгие исходные фрагменты / nlohmann для известных полей; '
          'E — jsoncons lossless_number. Во всех случаях вложенные объекты и массивы входят в сравнение путей, типов и значений.', '',
          'В полном цикле A–D используют jsoncons 2020-12 на теневом документе, E — jsoncons на собственном DOM. '
          'pboettch испытан отдельно на исходных документах и shadow: его положительный результат сам по себе не доказывает ни точность лексем, ни полноту V2.', '',
          '| ID | Строгий native-разбор 12 недопустимых чисел | valid_all: полный цикл / 6 мест | Вложенность 1000 / 100000 | Fail-closed | LOC адаптера | Windows | Linux |',
          '|---|---|---|---|---|---:|---|---|']
for candidate in 'ABCDE':
    all_case = cases['valid_all.json']['candidates'][candidate]['summary']
    depth = [','.join(cases[f'depth_{n}.json']['candidates'][candidate]['summary']['outcomes']) for n in (1000,100000)]
    sites = all_case['locations']
    status = ','.join(all_case['outcomes']) + ' / ' + ('н/д' if sites is None or all(s is None for s in sites) else ''.join('+' if s else '-' for s in sites))
    lines.append(f"| {candidate} | {'отверг все' if data['strict_parsers'][candidate] else 'принял часть (см. results.json)'} | {status} | {' / '.join(depth)} | SHA-256 guard PASS | {loc[candidate]} | измерен | не выполнялся |")
lines += ['', f'LOC — непустые физические строки адаптеров. B/C дополнительно используют общий event writer ({shared} строк). '
          f'У D основной механизм — общий эталонный токенизатор ({reference_loc} строк) плюс адаптер; он же нужен стенду остальных кандидатов для независимого guard. '
          'E включает в свой подсчёт адаптер и вызов валидатора. Обвязка ZIP, транзакции, памяти и main общая и не включена в числа адаптеров.', '',
          'Смысл колонок 6 мест: extensions.data (оба независимых расширения), custom_values.value, generation.params, claims.value, '
          'field_definitions.default/constraints, history.details/дополнительные changes[]. «+» — лексемы, пути и другие непрозрачные значения совпали; '
          '«-» — guard обнаружил различие; «н/д» — кандидат не дошёл до проверки. Порядок object-ключей и пробелы не считаются потерей.', '',
          '### Сохранность по местам на отдельных допустимых фикстурах', '',
          '| ID | extensions | custom | generation | claims | fields | history |', '|---|---|---|---|---|---|---|']
for candidate in 'ABCDE':
    columns = []
    selected = [case['candidates'][candidate]['summary']['locations'] for name,case in cases.items() if name.startswith('valid_')]
    for place in range(6):
        observed = [s[place] for s in selected if s is not None and s[place] is not None]
        columns.append(f'{sum(observed)}/{len(observed)} совпали; н/д {len(selected)-len(observed)}')
    lines.append('| ' + candidate + ' | ' + ' | '.join(columns) + ' |')

lines += ['', '### Дубли ключей', '', '| ID | Непрозрачный | Известный |', '|---|---|---|']
for candidate in 'ABCDE':
    columns = []
    for fixture in ['duplicate_opaque.json','duplicate_known.json']:
        trial = cases[fixture]['candidates'][candidate]['trials'][0]
        columns.append(trial.get('duplicate_policy', 'отказ native') + ': ' + str(trial.get('duplicate_values', [])))
    lines.append('| ' + candidate + ' | ' + ' | '.join(columns) + ' |')
lines += ['', 'Во всех случаях запись дублей запрещена общим guard. Это экспериментальная мера сохранности, не решение политики O-17.', '',
          '## V2: реальные результаты', '', 'Пример исходного манифеста:', '```json',
          json.dumps(data.get('example_validators', {}), indent=2, ensure_ascii=False), '```', '',
          'Отрицательная фикстура `schema_invalid_claim_extra.json` (лишнее поле claim, запрещённое unevaluatedProperties):',
          '```json', json.dumps(data['validators']['schema_invalid_claim_extra.json'], indent=2, ensure_ascii=False), '```', '',
          'pboettch с установленным default_string_format_check загружает данную схему, но загрузка не доказывает поддержку 2020-12: '
          'результат на запрещённом дополнительном поле отличается от Python-reference и jsoncons. Стенд проверяет поведение закреплённого порта, '
          'а не вывод по имени версии схемы. Полные пробы обоих валидаторов и shadow для всех 35 документов находятся в results.json.', '',
          'Shadow заменяет только непрозрачные поддеревья заглушками того же JSON-типа. В данной схеме для generation.params и history.details требуется object; '
          'extensions.data, plugin custom/claim values, plugin defaults/constraints и дополнительные change-ключи не ограничивают числовые значения. '
          'Нормативные поля и их ограничения остаются. Проверка лексем остаётся отдельным V1/guard; shadow не объявляет семантику плагина проверенной. '
          'При изменении схемы применимость shadow необходимо исследовать заново.', '',
          '### Показательные отказы', '', '| Фикстура | ID | Этап | Наблюдение |', '|---|---|---|---|']
for name in ['valid_huge_positive.json','valid_negative_zero.json','valid_exponent_1000.json','depth_100000.json']:
    for candidate in 'ABCDE':
        trial = cases[name]['candidates'][candidate]['trials'][0]
        if trial.get('status') != 'success':
            error = trial.get('error',trial.get('exit_hex',trial['status'])).replace('|','\\|').replace('\n',' ')[:240]
            lines.append(f"| {name} | {candidate} | {trial.get('stage','процесс')} | {error} |")

lines += ['', '## Замеры: все документы', '',
          'Медианы по пяти отдельным процессам, время в мс, память в МиБ. Parse — нативный разбор/хранение кандидата, V2 — выбранный validator path, '
          'serialize — сериализация с правкой title. Цикл включает создание/чтение ZIP, reference, V2, защитное сравнение и транзакцию; '
          'при отказе это время до отказа, а не успешно выполненный цикл. `—` — этап не достигнут или процесс упал до отчёта. '
          'При crash доступны только время дочернего процесса wall и код завершения; память/этапы не выдумываются. '
          'Результаты всех пяти запусков и хеши входов — в build/s7/results.json.', '',
          '| Документ | ID | Итог | parse | V2 | serialize | цикл/отказ | wall | PeakWorkingSet | PeakPagefile |',
          '|---|---|---|---:|---:|---:|---:|---:|---:|---:|']
for name,case in cases.items():
    for candidate,entry in case['candidates'].items():
        medians = entry['summary']['medians']
        values = []
        for key in ['parse_ms','v2_ms','serialize_ms','cycle_ms','wall_ms','peak_working_set','peak_pagefile']:
            value = medians[key]
            values.append('—' if value is None else f'{value/(1024*1024) if key.startswith("peak_") else value:.3f}')
        lines.append('| ' + name + ' | ' + candidate + ' | ' + ','.join(entry['summary']['outcomes']) + ' | ' + ' | '.join(values) + ' |')
lines += ['', '## Наблюдения и рекомендация исполнителя', '',
          '**Рекомендация, не архитектурное решение:** продолжить исследование D / гибридного хранения проверенных исходных непрозрачных фрагментов. '
          'На данной Windows-выборке D прошёл весь обязательный набор лексем; это не основание закрывать S7 без Linux. '
          'A непригоден для общей гарантии. B требует отдельного решения для integer callbacks и диапазонных отказов до number_float. '
          'C с данным Reader/флагами ограничен диапазонной обработкой экспонент. E полезен для V2 2020-12 и точных decimal/bigint, '
          'но отдельный integer -0 не сохраняется. Проверка безопасности записи обязательна и при использовании стороннего lossless API.', '',
          '## Вопросы O-17 и ограничения эксперимента', '',
          '- Политика дублей, размерные/глубинные лимиты и тайм-ауты остаются открыты. Успех на глубине 1000 не даёт норматива; '
          'отказ reference/D на 100000 — результат настроенной гипотезы, а не доказательство принципиального ограничения токенизации.',
          '- A на глубине 100000 завершился переполнением стека 0xc00000fd; процесс изолирован. Остальные результаты различают native parse и reference guard.',
          '- Для упавшего процесса нет достоверного post-exit пика памяти: он отмечен как недоступный. Пять запусков crash не усредняются с нулями.',
          '- Данный стенд проверяет V1/V2 и сохранение содержимого, не V3/V4, доверие плагинов или ZIP-bomb-политику. '
          'Временный ZIP проверяется до атомарной замены; SHA-256 внешнего guard проверяет исходный ZIP при небезопасной записи.',
          '- Linux не выполнялся. WSL обнаружен: Ubuntu (default), docker-desktop, оба Stopped, версия 2. Установка/включение и Linux-компилятор не запускались.',
          '- Схема, SPEC, ADR, libraries.md и продуктовые зависимости не менялись; формат 1.0 и O-13 не объявляются утверждёнными.', '']
args.output.write_text('\n'.join(lines), encoding='utf-8', newline='\n')
print('Rendered ' + str(args.output) + f' ({len(cases)} fixtures, 5 candidates)')
