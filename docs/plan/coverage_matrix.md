# Матрица покрытия сценариев импорта (ТЗ §13.2, ADR-0029)

Матрица из 16 универсальных сценариев импорта (ТЗ §13.2) с отдельным учётом фактов (§13.2.3):
- **(а)** найден ли реальный пример;
- **(б)** есть ли синтетический пример;
- **(в)** предусмотрена ли обработка в проекте;
- **(г)** реализована ли она;
- **(д)** протестирована ли и с каким результатом.

Сценарии имеют ID `SC-1…SC-16`. Сводная матрица приёмки по всем требованиям (К-n, НФТ §14) с пятью статусами ТЗ §14.9.12 — [`acceptance_matrix.md`](acceptance_matrix.md); эта таблица — её детализация для импорта.

**Неизвестное не записывается как «проверено».** Значения: `да`, `нет`, `частично (что именно)`, `неизвестно`, `н/п` (неприменимо).
Реальные примеры — только наблюдения в доступной коллекции ([отчёт 02](../reports/02_collection_findings.md)). Коллекция не эталон (ТЗ §13.1.1).

Вехи указаны с этапом P0–P3 (ТЗ §19.1, [`roadmap.md`](roadmap.md)).

**Обновлено T-034 (2026-11-26):** проверенный синтетический CLI-срез указан по сценариям ниже. Полнота сценария и проверка на конкретной ФС различаются; RAR/7z, анализаторы и поиск не объявляются готовыми по наличию ZIP-тестов.

| № | Сценарий | (а) Реальный пример | (б) Синтетический | (в) Предусмотрено | (г) Реализовано | (д) Тест | Критерии §15 |
|---|---|---|---|---|---|---|---|
| 1 | Одиночный файл | да (единичные STL вне архивов) | да — `sc01-single-stl` | да — `import_pipeline.md`, M3a (P0); скан и план (T-028) | да — CLI: файл в ассете, предпросмотр необязателен | `import_corpus.sc01-single-stl`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 1, 8 |
| 2 | Несколько связанных файлов | частично (архивы STL + изображения; связка STL+STEP не найдена) | да — `sc02-related-files` | да — M3a (P0); зависимости OBJ/MTL — M7 (P2); скан и план (T-028) | частично — STL/STEP/PNG в одном ассете; специализированные связи обработчиков не проверены | `import_corpus.sc02-related-files`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 2, 52, 56 |
| 3 | Каталог материалов | да (657 папок, ~66 % «1 архив + 1 изображение») | да — `sc03-material-folders-telegram`, `sc03-merge-by-edit` | да — `BoundaryProposer`, профиль; скан и план (T-028) | частично — профиль telegram, правка границ и объединение; неподдерживаемые архивы — M3b | `import_corpus.sc03-material-folders-telegram`, `import_corpus.sc03-merge-by-edit`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 32, 101 |
| 4 | Обычный архив | да (697 архивов верхнего уровня) | да — `sc04-plain-zip`, `sc04-zip-cp437-names` | да — ZIP: M3a (P0); RAR/7z: S1, M3b (P1); внешняя упаковка в ассет не вкладывается (§19.4.1.5) | частично — ZIP stored/deflate; RAR/7z — M3b | `import_corpus.sc04-plain-zip`, `import_corpus.sc04-zip-cp437-names`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 3, 4 |
| 5 | Несколько архивов одного ресурса | да (~38 папок) | да — `sc05-two-archives` | да — один акт импорта/Input на кандидата, отдельный InputObject archive и подкаталог на архив (T-034 В-1; ADR-0035) | частично — два ZIP, один Input, два объекта с раздельными путями; другие архивы — M3b | `import_corpus.sc05-two-archives`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 2, 20 |
| 6 | Многотомный архив | да (11 `.NNN`, 4 `.partN.rar`, 5 наборов `copy_suffix`) | да — `sc06-volume-gap`, `sc06-volume-names` | да — ADR-0011, подтверждение по заголовкам после S1, M3b (P1); скан и план (T-028) | частично — неподтверждённые наборы и пропуск номера; run deferred, раскрытия нет | `import_corpus.sc06-volume-gap`, `import_corpus.sc06-volume-names`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 34 |
| 7 | Вложенные архивы | да (227) | да — `sc07-depth-limit`, `sc07-nested-exclude`, `sc07-nested-expand`, `sc07-nested-preserve`, `sc07-ratio-limit` | да — профиль `[nested]`, лимиты `[limits]`, действие `extract/preserve/exclude` (§19.4.3.3) | частично — ZIP expand/preserve/exclude и лимиты; RAR/7z — M3b | `import_corpus.sc07-depth-limit`, `import_corpus.sc07-nested-exclude`, `import_corpus.sc07-nested-expand`, `import_corpus.sc07-nested-preserve`, `import_corpus.sc07-ratio-limit`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 35, 104 |
| 8 | Смешанный входной набор | да (папки с архивами, свободными изображениями и STL) | да — `sc08-mixed`, `sc08-mixed-single` | да — M3a (P0), с RAR/7z — M3b (P1); скан и план (T-028) | частично — файлы/папки/ZIP, --single и правка границ; RAR/7z — M3b | `import_corpus.sc08-mixed`, `import_corpus.sc08-mixed-single`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 32, 101 |
| 9 | Несколько ресурсов в одном архиве | неизвестно (содержимое не исследовано на этом уровне) | да — `sc09-multi-resource-zip` | да — предложение разделения (`BoundaryProposer`) | частично — один ZIP в одном ассете без автоматического разделения; предложения разделения нет | `import_corpus.sc09-multi-resource-zip`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 32 |
| 10 | Специализированный проектный контейнер | да (19 `.3mf`) | да — `sc10-3mf`, `sc10-3mf-in-zip`, `sc10-fake-3mf` | да — ТЗ 6.4.5, сохраняется целиком (полезный файл, не внешняя упаковка; §19.4.1.5); скан и план (T-028) | частично — 3MF сохраняется файлом, включая запись ZIP; другие форматы не проверены | `import_corpus.sc10-3mf`, `import_corpus.sc10-3mf-in-zip`, `import_corpus.sc10-fake-3mf`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 35 |
| 11 | Ресурс без 3D-геометрии | частично (14 папок только с изображениями; DXF/SVG не найдены) | да — `sc11-no-geometry` | да — M3a, просмотр растров — M6a (P1), 2D — M6b (P2); скан и план (T-028) | да — хранение PNG/SVG/DXF/PDF без геометрии; просмотр — M6 | `import_corpus.sc11-no-geometry`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 51 |
| 12 | Неизвестный формат | да (686 файлов без расширения, `.lys`, `.chitubox`) | да — `sc12-unknown` | да — хранение байтов без обработчика; скан и план (T-028) | да — неизвестные и пустые файлы без обработчика | `import_corpus.sc12-unknown`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 8 |
| 13 | Повреждённые или неполные данные | да (`Sanix…rar`, 3 «не-zip» `.zip`, `part2-4` с нулевыми томами) | да — `sc13-corrupt-zip-strict`, `sc13-not-a-zip`, `sc13-partial-allowed` | да — строгий режим, S1; скан и план (T-028) | частично — CRC strict/partial, не-ZIP и лимиты; RAR/7z — M3b | `import_corpus.sc13-corrupt-zip-strict`, `import_corpus.sc13-not-a-zip`, `import_corpus.sc13-partial-allowed`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 13, 37, 110 |
| 14 | Готовый ассет | н/п (формат новый) | да — `sc14-ready-asset`, `sc14-ready-invalid` | да — распознавание по `format.id` (§19.4.14.2, ADR-0035), M2 (P0.5); конфликт ID → независимая копия: rev 1 `asset_created` + `independent_copy` (ADR-0036 п. 4); скан и план (T-028) | да — готовый ZIP сохраняет ID/ревизию/байты; недействительный требует решения | `import_corpus.sc14-ready-asset`, `import_corpus.sc14-ready-invalid`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 31 |
| 15 | Повторно предъявленный ресурс | неизвестно (SHA-256 в инвентаризации не считались; 4 027 групп одинаковых имён) | да — `sc15-repeat` | да — ADR-0016, M4 (P1); скан и план (T-028) | частично — подсказки name/hash и skip-identical; поиск и сравнение геометрии — M4 | `import_corpus.sc15-repeat`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 5, 33, 70, 109 |
| 16 | Ресурс без сведений о происхождении | н/п (любой локальный файл) | да — `sc01-single-stl` | да — источники необязательны (ТЗ §5.2); скан и план (T-028) | да — происхождение необязательно; sc01-single-stl | `import_corpus.sc01-single-stl`; Windows PASS; Linux scan PASS, run → storage_read_only (детали ниже) | 27 |

## Характеристики (ТЗ §13.4.2) с уже существующими юнит-проверками

| Характеристика | Тест | Статус |
|---|---|---|
| AppleDouble по сигнатуре, а не по имени | `Exclusion.AppleDouble*`, `Exclusion.RealFileStartingWithDotUnderscoreWithoutSignatureIsKept` | пройдены |
| Длинные пути (`\\?\`) | `Paths.*` | пройдены |
| Имена ZIP без флага UTF-8 (CP437-mojibake) | `TextEncoding.*` | пройдены |
| Суффикс копии ` (N)` как подсказка тома, настраиваемая профилем | `Volumes.CopySuffix*` | пройдены |

Матрица обновляется при каждом изменении статуса: новая реализация, тест или найденный пример (порядок работы, `roadmap.md`).

## Дополнение T-030

В-10 дополнительно проверен тестами AssetRepository.ExplicitIdentity* / TrashSnapshot* и Decisions/ImportRunTrashRetry.*: занятые ID в assets/trash отклоняются под блокировкой, повтор после source_changed_during_import требует решения, restore/purge/skip не создают дубликатов. Снимок корзины не разрешает изменение.

Синтетические ImportPlanView.*, ImportRun.* и cli_import_run покрывают выполнение файлового/каталожного подмножества SC-1/2/3: байты и V1–V4, исключённые/недоступные узлы, источники, готовые ассеты, свежесть, частичный режим, повтор и остановку. Это частичное покрытие сценариев: раскрытие архивов, обработчики и полный корпус 16 сценариев остаются следующими задачами. Проверки JobsExecutor на обоих хранилищах покрывают результаты неуспешной попытки и независимость выборки от 10 000 завершённых заданий. Фактические команды и итоги — в отчёте T-030.

## Дополнение T-031

Реализовано подмножество ZIP для SC-1/2/8/13: скан v2, перенос байтов без извлечения на диск, метаданные и исходные имена всех записей, строгий/частичный импорт, безопасные пути, лимиты, свежесть и компактные параметры. ZipInput.*, ImportZipScan.*, ImportZipRun.*, ImportZipRepository.*, ImportZipPlan.* и cli_import_zip проверяют это подмножество. Для SC-7 реализован только preserve/exclude вложенной записи, раскрытие остаётся отложенным; SC-10 внутри ZIP — сохранение по подсказке расширения без утверждения проверенного типа. RAR/7z, многотомные входы и полный корпус 16 сценариев не завершены. Фактические результаты и ограничения — в отчёте T-031.

## Дополнение T-032 (этапы 32a и 32b)

SC-7 для ZIP внутри ZIP: скан перечисляет вложенные ZIP до `max_depth` (по умолчанию 3, потолок 8), решение `expand` раскрывает их без сохранения контейнера, манифест хранит объект `nested_action: extract` и запись родителя `excluded`/`nested_extract`. Сжатые вложенные архивы распаковываются только во временную область библиотеки. RAR/7z внутри ZIP, вложенные кандидаты и корпус 16 сценариев не завершены; этап 32b добавил проверку 3MF по центральному каталогу (`specialized_container_verified`) и уборку сирот временной области импорта при открытии библиотеки.

## Дополнение T-034

Корпус — `tests/fixtures/import/`, только case.json/expected.json; входы
генерируются при каждом прогоне. Полные команды, логи и числа тестов —
в отчёте T-034. Таблица ниже перечисляет фактические ctest-результаты;
Windows — нормальный прогон, также проверен offline. Linux проверяет весь
scan и ожидаемый отказ run по политике ФС; это не проверка записи на ext4.
UUID/время проверяются по свойствам, SHA/состав и исходные имена — по
декларативным ожиданиям. Источник неизменен, временная область пуста.

Уточнения: SC-5 — один Input на акт импорта и два объекта архивов с разными
подкаталогами (В-1). Контекст Telegram в манифесте — источник с claim
directory_inference без review, а не подтверждённое происхождение (В-2).
У корневого 3MF подтверждение — detected=project.3mf, у записи ZIP —
explanation_id=specialized_container_verified (В-4); новое поле не вводилось.

| ctest | SC | Windows | Linux |
|---|---|---|---|
| `import_corpus.ch-appledouble` | SC-12 | PASS | PASS (scan; запись н/п) |
| `import_corpus.ch-case-collision` | SC-4 | PASS | PASS (scan; запись н/п) |
| `import_corpus.ch-links` | SC-1 | SKIP | PASS (scan; запись н/п) |
| `import_corpus.ch-long-paths` | SC-1, SC-4 | PASS | PASS (scan; запись н/п) |
| `import_corpus.ch-unicode-names` | SC-1, SC-4 | PASS | PASS (scan; запись н/п) |
| `import_corpus.ch-unsafe-zip-paths` | SC-4, SC-13 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc01-single-stl` | SC-1, SC-16 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc02-related-files` | SC-2 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc03-material-folders-telegram` | SC-3 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc03-merge-by-edit` | SC-3, SC-8 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc04-plain-zip` | SC-4 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc04-zip-cp437-names` | SC-4 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc05-two-archives` | SC-5 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc06-volume-gap` | SC-6, SC-13 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc06-volume-names` | SC-6 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc07-depth-limit` | SC-7, SC-13 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc07-nested-exclude` | SC-7 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc07-nested-expand` | SC-7 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc07-nested-preserve` | SC-7 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc07-ratio-limit` | SC-7, SC-13 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc08-mixed` | SC-8 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc08-mixed-single` | SC-8 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc09-multi-resource-zip` | SC-9 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc10-3mf` | SC-10 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc10-3mf-in-zip` | SC-10, SC-4 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc10-fake-3mf` | SC-10, SC-12 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc11-no-geometry` | SC-11 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc12-unknown` | SC-12 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc13-corrupt-zip-strict` | SC-13 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc13-not-a-zip` | SC-13, SC-12 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc13-partial-allowed` | SC-13 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc14-ready-asset` | SC-14 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc14-ready-invalid` | SC-14 | PASS | PASS (scan; запись н/п) |
| `import_corpus.sc15-repeat` | SC-15 | PASS | PASS (scan; запись н/п) |

### Характеристики §13.4.2

| Характеристика | Случай / ctest import_corpus.* | Проверенное свойство |
|---|---|---|
| AppleDouble | ch-appledouble | сигнатура исключает; одно имя ._* без сигнатуры сохраняется |
| Длина | ch-long-paths | источник >260 символов; ZIP-путь >200 байтов переименован без потери байтов |
| Unicode | ch-unicode-names | NFC/NFD, кириллица, эмодзи; исходный path не нормализован |
| Не-UTF-8 ZIP | sc04-zip-cp437-names | исходные байты Base64; догадка кодировки не выдана за достоверную |
| Коллизия регистра | ch-case-collision | явный include, included_renamed/collision; оба содержимых сохранены |
| Безопасность пути | ch-unsafe-zip-paths | ../, абсолютный путь, диск и backslash требуют решения, затем исключены |
| Ссылка | ch-links | вложенная ссылка skip/not_processed, без разыменования; Windows SKIP без привилегии |
| Размер/режим отказа | sc07-depth-limit, sc07-ratio-limit, sc13-* | неполный scan по лимиту; strict без ассета; явный partial с failed |

### Разрывы

- SC-4/6/7/8/13: RAR/7z и настоящие многотомные архивы — M3b; синтетические
  тома здесь проверяют имена/неполноту и deferred, а не раскрытие.
- SC-9: предложения разделения ещё нет; подпапки сами не становятся ассетами.
- SC-15: подсказка по сохранённому SHA и skip-identical не заменяют поиск по
  содержимому/геометрии (M4). Проверки после успешного импорта на Linux н/п;
  повторные сканы пустой библиотеки трактуются по решению В-3 в отчёте.
- SC-2: связи специализированных обработчиков/OBJ-MTL не проверены.
- SC-10: проверены только 3MF и ложный 3MF, не все специализированные форматы.
- Нет реальных примеров текущей коллекции, анализа без импорта и цепочки
  импорт → экспорт → чистая библиотека: отдельные задачи M3b/P0.8.
