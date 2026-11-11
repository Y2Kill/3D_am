"""Render measured Windows/Linux matrices without choosing the product JSON stack."""
import argparse
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[3]
CANDIDATES = ('A', 'B', 'C', 'D1', 'D2', 'E')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--results', type=pathlib.Path, required=True)
parser.add_argument('--linux-results', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
args = parser.parse_args()
platforms = {'Windows': json.loads(args.results.read_text(encoding='utf-8')),
             'Linux': json.loads(args.linux_results.read_text(encoding='utf-8'))}
lines = ['# S7: результаты T-004 после доработки', '',
         'Эксперимент на закреплённом baseline, не утверждение формата 1.0 и не ADR. '
         'Первичный Windows-отчёт и его тестовые итоги сохранены в файле задачи; ниже — новые измерения со структурной правкой.', '',
         '## Механизмы и сценарий', '',
         '- A: nlohmann ordered DOM; правки в DOM.',
         '- B: nlohmann SAX, лексемы number_float и собственные события; C: RapidJSON NumbersAsStrings + проверенный RawValue. '
         'B и C и в первом варианте меняли title вклейкой после сериализации; теперь структурные правки также выполняются по позициям токенов.',
         '- D1: прежний D, правка исходного текста по позициям токенов. Он сохранён как отдельный механизм; его успех не доказывает работу DOM-гибрида.',
         '- D2: известная часть в nlohmann ordered DOM; непрозрачные корни заменены значениями binary с 64-битным номером в отдельной таблице. '
         'JSON не содержит binary-типа: строки, объекты bytes/subtype и похожие маркеры из манифеста не создают заглушку. '
         'Заглушка переезжает вместе с владельцем при erase массива. Собственный писатель обходит DOM, экранирует ключи/обычные значения через nlohmann '
         'и выводит фрагмент только после нового строгого разбора ровно одного JSON-значения; подстановка по шаблонам отсутствует.',
         '- E: jsoncons lossless_number, правки в его DOM, собственная V2 2020-12.', '',
         'Все кандидаты меняют title, удаляют первый user custom_value перед plugin custom_value, добавляют одну metadata_changed-запись '
         'history.entries только с известными полями, увеличивают asset.revision и обновляют modified_at до timestamp новой записи. '
         'Проверяются входная V2 и выходная V2, ожидаемая известная часть, все шесть непрозрачных мест, содержимое ZIP и повторное открытие. '
         'Запись транзакционна: проверенный временный ZIP в той же папке, атомарная замена. При обработанном отказе исходный ZIP побайтово прежний; '
         'независимый guard проверяет SHA-256.', '',
         '## Устойчивые ключи защиты', '',
         '| Владелец | Ключ |', '|---|---|',
         '| extensions | ExtensionID — ключ объекта |',
         '| field_definitions / custom_values | id / field_id (FieldID) |',
         '| files, groups | id (FileID / GroupID); file_id в ссылках обозначает этот id |',
         '| claims | id (ClaimID, SPEC §9.3) |',
         '| history.entries | operation_id (OperationID, SPEC §11) |', '',
         'Scope владельца включён в ключ: asset, конкретный FileID или GroupID. Неоднозначные одинаковые ID отвергаются. '
         'Внутри непрозрачного поддерева остаётся типизированный JSON-путь: перестановка чисел внутри массива обнаруживается. '
         'В changes[] собственного ID по SPEC нет: для дополнительного непрозрачного ключа сохраняется индекс изменения внутри OperationID; '
         'этот массив в сценарии не редактируется. Проверка не объявляет произвольную перестановку changes поддержанной. '
         'Индексная карта сохранена отдельно: на структурной правке она ожидаемо различается, на успешной правке только title — совпадает.', '',
         '## Конфигурация и зависимости', '']
for label, data in platforms.items():
    lines += [label + ': `' + data['platform'] + '`', '', '```json',
              json.dumps(data['configuration'], indent=2, ensure_ascii=False), '```', '']
lines += ['Intel Core i7-9750H @ 2.60GHz, 6 ядер / 12 потоков; физическая ОЗУ Windows — 119 992 246 272 байта. '
          'Release, пять отдельных последовательных процессов на пару документ/кандидат; дополнительно один title-only процесс на пару. '
          'WSL: установленная Ubuntu 24.04, GCC; все рабочие файлы через /mnt/d (drvfs). '
          'Этот путь обычно медленнее родной ФС и влияет на время ZIP/цикла; отдельного сравнения с ext4 в задаче нет. '
          'На сохранность лексем тип ФС не влияет.', '',
          'Гипотезы, не нормативы продукта: max-depth reference/D1/D2 1024, max JSON bytes 134217728, '
          'тайм-аут процесса ' + str(platforms['Windows']['hypothesis_timeout_seconds']) + ' с. Нативный разбор исполняется до общего эталона.', '',
          '| Порт | Версия | Лицензия |', '|---|---|---|']
for name in ('nlohmann-json', 'json-schema-validator', 'rapidjson', 'jsoncons', 'minizip-ng', 'gtest'):
    port = json.loads((ROOT / '.cache/vcpkg/ports' / name / 'vcpkg.json').read_text(encoding='utf-8'))
    version = next(port[k] for k in ('version', 'version-semver', 'version-string', 'version-date') if k in port)
    if port.get('port-version'):
        version += '#' + str(port['port-version'])
    lines.append(f"| {name} | {version} | {port['license']} |")


def outcome(entry):
    summary = entry['summary']
    sites = summary.get('locations')
    return ','.join(summary['outcomes']) + ' / ' + ('н/д' if not sites or all(x is None for x in sites)
                                                  else ''.join('+' if x else '-' for x in sites))


lines += ['', '## Матрица', '',
          'Шесть знаков: extensions, custom, generation, claims, fields, history. '
          '«+» означает совпадение чисел, типов и значений по устойчивому владельцу; «н/д» — guard не достигнут.', '',
          '| ID | Windows valid_all / 6 мест | Linux valid_all / 6 мест | Строгость Windows / Linux | Fail-closed |', '|---|---|---|---|---|']
for candidate in CANDIDATES:
    values = [outcome(data['cases']['valid_all.json']['candidates'][candidate]) for data in platforms.values()]
    strict = ['все 12 отвергнуты' if data['strict_parsers'][candidate] else 'есть принятые' for data in platforms.values()]
    lines.append('| ' + candidate + ' | ' + ' | '.join(values) + ' | ' + ' / '.join(strict) + ' | SHA-256 guard PASS |')
for label, data in platforms.items():
    lines += ['', '### ' + label + ': сохранность и индексная проверка', '',
              '| ID | extensions | custom | generation | claims | fields | history | title-only индексная карта |',
              '|---|---|---|---|---|---|---|---|']
    for candidate in CANDIDATES:
        entries = [case['candidates'][candidate] for name, case in data['cases'].items() if name.startswith('valid_')]
        columns = []
        for place in range(6):
            observed = [e['summary']['locations'][place] for e in entries if e['summary'].get('locations') and e['summary']['locations'][place] is not None]
            columns.append(f'{sum(observed)}/{len(observed)}; н/д {len(entries)-len(observed)}')
        title = [e['title_only'] for e in entries]
        columns.append(f"{sum(t.get('indexed_equal') is True for t in title)}/{len(title)} совпали")
        lines.append('| ' + candidate + ' | ' + ' | '.join(columns) + ' |')
    failed_e = [name for name, case in data['cases'].items() if name.startswith('valid_') and
                case['candidates']['E']['summary']['outcomes'] != ['success']]
    lines += ['', 'E не прошёл следующие допустимые числовые фикстуры: ' + ', '.join('`' + n + '`' for n in failed_e) + '.', '',
              '### ' + label + ': дубли', '', '| ID | Непрозрачный | Известный |', '|---|---|---|']
    for candidate in CANDIDATES:
        values = []
        for fixture in ('duplicate_opaque.json', 'duplicate_known.json'):
            trial = data['cases'][fixture]['candidates'][candidate]['trials'][0]
            values.append(trial.get('duplicate_policy', 'отказ native') + ': ' + str(trial.get('duplicate_values', [])))
        lines.append('| ' + candidate + ' | ' + ' | '.join(values) + ' |')
    lines += ['', '### ' + label + ': глубина и причина отказа', '',
              '| ID | Глубина | Итог | Этап | Ошибка / код |', '|---|---:|---|---|---|']
    for candidate in CANDIDATES:
        for depth in (64, 1000, 100000):
            trial = data['cases'][f'depth_{depth}.json']['candidates'][candidate]['trials'][0]
            reason = trial.get('error', trial.get('exit_hex', '')).replace('|', '\\|').replace('\n', ' ')[:180]
            lines.append(f"| {candidate} | {depth} | {trial['status']} | {trial.get('stage', 'процесс')} | {reason} |")
lines += ['', 'Дубли всегда блокируют запись в стенде. Это не утверждение политики O-17.', '', '## V2', '',
          'A–D2 используют jsoncons 2020-12 на теневом документе, E — на собственном DOM. pboettch отдельно испытан на исходном '
          'документе и shadow. Само успешное чтение схемы не доказывает поддержку её диалекта.', '']
for label, data in platforms.items():
    lines += [label + ', исходный пример:', '```json', json.dumps(data['example_validators'], indent=2, ensure_ascii=False), '```', '',
              label + ', запрещённое дополнительное поле claim (unevaluatedProperties):', '```json',
              json.dumps(data['validators']['schema_invalid_claim_extra.json'], indent=2, ensure_ascii=False), '```', '']
lines += ['pboettch загружает схему, но ложно принимает запрещённое поле claim; jsoncons и Python отвергают. '
          'Наблюдение закреплённого порта подтверждено отдельно на обеих ОС.', '',
          'Shadow сохраняет JSON-тип непрозрачного корня. Схема требует object для generation.params и history.details; '
          'значения чисел внутри остальных перечисленных plugin-поддеревьев не ограничивает. '
          'Известные ограничения остаются; лексемы проверяет V1/guard. Проверка семантики плагина из этого не следует. '
          'При смене схемы применимость shadow проверяется заново.', '', '## Память: разложение стенда', '',
          'Ниже медианы пяти изолированных процессов на manifest_64m, МиБ. input — только чтение исходного текста; reference — чтение '
          'и токенизатор; reference_maps — также одна пара устойчивых карт значений/чисел; candidate — чтение, разбор, правка и сериализация '
          'одного кандидата без ZIP/V2/общего guard. Значения включают общий процесс и вход. Их нельзя складывать: общие байты учитываются '
          'повторно, аллокаторы и временные копии имеют разные времена жизни. Разности относительно input — оценка дополнительных затрат. '
          'Полный цикл держит одновременно два документа и карты before/after, ZIP-копии и проверки повторного открытия. '
          'Первоначальные ~837 МиБ были пиком всего стенда, а не памятью каждого парсера.', '',
          '| ОС | Компонент | Peak RSS/working set | сверх input |', '|---|---|---:|---:|']
for label, data in platforms.items():
    memory = data['memory_components']
    baseline = memory['input:D2']['summary']['medians']['peak_working_set']
    for key, entry in memory.items():
        peak = entry['summary']['medians']['peak_working_set']
        lines.append(f'| {label} | {key} | {peak/1048576:.3f} | {(peak-baseline)/1048576:.3f} |')
lines += ['', 'Linux ru_maxrss — RSS, не commit. PeakPagefile на Linux API не измеряет; в машинных данных стоит 0, '
          'в таблице ниже «н/д». Для crash пик памяти недоступен и не заменяется нулём.', '', '## Различия ОС', '']
differences = []
for name, case in platforms['Windows']['cases'].items():
    other = platforms['Linux']['cases'][name]
    assert case['sha256'] == other['sha256'], name
    for candidate in CANDIDATES:
        w = case['candidates'][candidate]['summary']
        l = other['candidates'][candidate]['summary']
        for key in ('outcomes', 'parse_accepted', 'locations'):
            if w.get(key) != l.get(key):
                differences.append(f"- `{name}`, {candidate}, {key}: Windows `{w.get(key)}`, Linux `{l.get(key)}`.")
lines += differences or ['Сводные исходы, native parse acceptance и сохранность шести мест совпали для всех пар.']
lines += ['', 'Коды crash платформенные; на depth_100000 учитываются отдельно в машинных данных. Время и RSS '
          'между ОС напрямую не эквивалентны (drvfs, GCC/MSVC, аллокаторы, системные библиотеки).', '', '## Размер реализации', '',
          'Непустые физические строки (включая комментарии), не метрика сложности:', '', '| Файл | Строк |', '|---|---:|']
for path in sorted((ROOT / 'spikes/s7/src').glob('*')):
    count = sum(bool(line.strip()) for line in path.read_text(encoding='utf-8').splitlines())
    lines.append(f'| {path.name} | {count} |')
lines += ['', 'candidates.cpp содержит A/B/C/D1/D2 и общий писатель событий; reference.cpp — общий строгий проверяющий код и D1; '
          'jsoncons_candidate.cpp — E и V2. Память/ZIP/main — общая обвязка.', '']
source = (ROOT / 'spikes/s7/src/candidates.cpp').read_text(encoding='utf-8')
def loc(start, end):
    begin = source.index(start)
    body = source[begin:source.index(end, begin)]
    return sum(bool(line.strip()) for line in body.splitlines())
lines += ['| Адаптер / общая часть | Непустых строк |', '|---|---:|',
          f"| A | {loc('struct Dom final', 'enum class EventKind')} |",
          f"| B | {loc('struct Sax :', 'struct RapidHandler')} |",
          f"| C | {loc('struct RapidHandler', 'struct RawCandidate')} |",
          f"| D1 | {loc('struct RawCandidate', 'struct HybridCandidate')} |",
          f"| D2 | {loc('struct HybridCandidate', '}  // namespace')} |",
          f"| B/C общий event writer | {loc('enum class EventKind', 'struct Sax :')} |",
          f"| D2 собственный writer | {loc('std::string write_hybrid', 'namespace {')} |",
          f"| Общая DOM-правка и запись истории | {loc('Ordered history_entry', 'std::string write_hybrid')} |",
          '| E | см. jsoncons_candidate.cpp выше (включает общий V2) |', '',
          'D1 использует также edit_known/edit_title и токенизатор из reference.cpp; B/C делят этот код. '
          'D2 также использует эталонный токенизатор для строгой проверки фрагментов. Общий код не скрыт внутри числа адаптера.', '',
          '## Полные замеры', '',
          'Медианы пяти отдельных структурных запусков. Время в мс, память в МиБ. Цикл включает ZIP, V1/V2, правку, guard '
          'и транзакцию; при отказе указано время до отказа. «—» — этап не достигнут/процесс упал; wall — время дочернего процесса. '
          'Исходные повторы, title-only, хеши фикстур и validator probes: build/s7/results.json и build/s7-linux/results.json.', '']
for label, data in platforms.items():
    lines += ['### ' + label, '', '| Документ | ID | Итог | parse | V2 | serialize | цикл/отказ | wall | Peak RSS/WS | PeakPagefile |',
              '|---|---|---|---:|---:|---:|---:|---:|---:|---:|']
    for name, case in data['cases'].items():
        for candidate in CANDIDATES:
            entry = case['candidates'][candidate]['summary']
            values = []
            for key in ('parse_ms', 'v2_ms', 'serialize_ms', 'cycle_ms', 'wall_ms', 'peak_working_set', 'peak_pagefile'):
                value = entry['medians'][key]
                values.append('—' if value is None else 'н/д' if label == 'Linux' and key == 'peak_pagefile' else
                              f'{value/1048576 if key.startswith("peak_") else value:.3f}')
            lines.append('| ' + name + ' | ' + candidate + ' | ' + ','.join(entry['outcomes']) + ' | ' + ' | '.join(values) + ' |')
lines += ['', '## Рекомендация исполнителя и вопросы', '',
          '**Рекомендация, не архитектурное решение:** рассматривать D2 как измеренный гибрид для дальнейшего проектирования: '
          'результат теперь включает реальный DOM-обход, повторную проверку фрагментов и сдвиг индексов. D1 оставлен как контроль другого подхода. '
          'A не обеспечивает обещание; B теряет integer -0 и отказывает на 1e400; C ограничен диапазоном экспоненты; '
          'E сохраняет многие decimal/bigint, но теряет integer -0. Fail-closed нужен независимо от библиотеки.', '',
          '- O-17: политика дублей, измеренные лимиты глубины/размера/времени остаются решением управляющего/владельца. '
          'Гипотеза 1024 не является фундаментальным пределом токенизатора.',
          '- Выбор JSON-стека, итоговая ADR, libraries.md, утверждение формата и закрытие O-13 остаются вне задачи.',
          '- Стенд проверяет V1/V2 и сохранность, не V3/V4 и не политику ZIP bomb. Идентификаторы синтетической новой операции '
          'детерминированы для одного цикла эксперимента, это не реализация генератора OperationID продукта.',
          '- Для перестановки changes[] потребуется отдельно определить устойчивое соответствие изменений без ID; текущий сценарий '
          'сдвигает custom_values и добавляет history.entries, существующие changes не переставляет.', '']
args.output.write_text('\n'.join(lines), encoding='utf-8', newline='\n')
print('Rendered Windows/Linux matrices: ' + str(args.output))
