# T-030. `import run`: создание ассетов по плану из файлов и папок (M3a)

**Статус:** review · **Веха:** M3a · **Основание:** ТЗ v1.8 §6.1, §6.3.3–6.3.5, §6.5.6, §6.6, §6.7, §6.8, §6.9.1–6.9.2, §19.4.3; ADR-0006, ADR-0012, ADR-0025, ADR-0037 (X-01); приёмки T-026 (замечание 1), T-028 (замечание 4), T-029 (замечание 1)

## Цель

Выполнить план импорта `am-import-plan` v1, отредактированный пользователем, в библиотеке:

1. Создать ассеты для кандидатов `new_asset`, у которых входы — файлы и папки файловой системы.
2. Разместить готовые ассеты (`ready_asset`) тем же путём, что и `library add-ready`.
3. Работать через общую систему заданий: одно задание на кандидата, пауза, отмена, повтор, восстановление после сбоя без повторного создания уже зафиксированных ассетов.
4. Ни один кандидат не теряется молча. Каждый получает итог: создан, пропущен, отложен (нужно решение или вход ещё не поддерживается), ошибка.

Архивы как входы (`archive`, `multivolume_archive`) раскрываются в следующей задаче («ZIP как вход»). В T-030 такие кандидаты не выполняются, а явно откладываются с причиной.

## Контекст (что прочитать)

1. `docs/architecture/import_pipeline.md` — этапы 4–11, раздел «План импорта v1 (T-028)», правила T-029.
2. `docs/tasks/T-028-import-scan.md` — формат плана, `parse_plan`, раздел «Приёмка» (замечание 4).
3. `docs/tasks/T-026-job-executor.md` — раздел «Приёмка», замечание 1 (масштаб выборки).
4. `docs/specifications/container_manifest_v1.md` §5 (`origin`, `inputs`), схема `schemas/manifest-1.0.schema.json` (`input`, `inputObject`, `inputEntry`, `source`, `rawName`, `problem`).
5. `include/am/storage/repository/repository.hpp` (`CreateRequest`, `AddFiles`, `FileInput`, `import_ready`), `include/am/services/catalog/indexer.hpp` (`Catalog::create`, журнал операций).
6. `src/application/common/library_service.cpp` — `add_ready_library` (образец: каталог, журнал операций, классификация готового ассета), `JobService`.
7. `src/application/common/asset_service.cpp` — `create_asset` (образец использования `ZipPathMapper` и `AddFiles`).
8. `include/am/jobs/executor.hpp`, `src/jobs/executor.cpp`, `include/am/contracts/job_store.hpp`, `src/storage/db/job_store.cpp`.

## Решения управляющего (не пересматривать в задаче)

### 1. Команда и поток выполнения

- `am import run --plan <plan.json> --library <lib> [--enqueue-only] [--json]`, плюс общие параметры исполнителя, как у `am jobs run` (профиль, число параллельных заданий).
- Порядок:
  1. `parse_plan` (строгий JSON, схема, семантика) → типизированный вид (п. 2).
  2. Проверка перед постановкой в очередь (п. 3).
  3. Постановка родительского задания `import.run` и дочерних `import.asset` — по одному на кандидата с `action = import`.
  4. Выполнение до простоя через `JobService`.
  5. Сводка.
- `--enqueue-only` только ставит задания; выполнит их `am jobs run`. Оба пути дают одинаковый результат.
- Сам файл плана не изменяется. Итоги пишутся в результаты заданий и журнал операций библиотеки.
- Коды возврата (как у прочих команд):
  - 0 — все кандидаты с `action = import` созданы или распознаны как уже импортированные;
  - 3 — есть отложенные кандидаты (решение, неподдерживаемый вход) или задания в `needs_decision`, а ошибок нет;
  - 4 — есть ошибки кандидатов или ошибка выполнения;
  - 2 — неверные аргументы или план.
  Код 3 при отмене — как в T-026.

### 2. Типизированный вид плана (замечание 4 T-028)

- В `services/import` — неизменяемое представление (например, `PlanView`): корни, узлы, кандидаты, входы, записи, наборы томов, контекст, совпадения, режим. Строится **только** из JSON, прошедшего `parse_plan`, и только с отсылками, которые `parse_plan` уже проверил.
- Код `import run` обращается к плану через этот вид. Строковые ключи JSON — только внутри построителя вида.
- Числа плана (размер, `mtime_ns`) — строки uint64/int64. В виде они становятся целыми с проверкой диапазона, без `double`.
- `ImportPlan` как JSON-обёртка остаётся для сериализации и `scan`.

### 3. Проверка перед постановкой и защита от повторного запуска

- Хранилище библиотеки и источники не пересекаются (как В-2 T-028). Иначе — код 2, `am.import.library_overlaps_source`.
- Идентичность плана — SHA-256 точных байтов файла плана. Ключ задания кандидата — `(plan_sha256, candidate_id)`.
- Если для ключа уже есть задание, повторной постановки нет:
  - успешное задание — итог `already_imported` с прежним `asset_id`;
  - активное или ожидающее — итог `already_queued`;
  - неуспешное — повтор через `am jobs retry`, а не новое задание. В сводке — подсказка.
- Изменённый план (другой SHA) — это новый план, совпадения с библиотекой показывает новый `import scan`. Объединение и перезапись не выполняются (ТЗ §6.3.3).
- При постановке в спецификацию задания записываются заранее сгенерированные UUIDv4: `asset_id`, `InputID`, ID объектов, записей и файлов. Повтор после сбоя использует те же ID. Если ассет с этим `asset_id` уже зафиксирован и его манифест содержит этот `InputID`, повтор завершается успехом без повторной записи (ТЗ §6.7.6–6.7.7). Если ассет с таким ID есть, а `InputID` в нём нет, — ошибка `am.import.run.asset_id_conflict`, без изменений.
- Полезная нагрузка задания содержит только то, что нужно для выполнения кандидата: путь и SHA плана, ID кандидата, заранее выданные ID. Копию плана в базу заданий не писать. Если план к моменту выполнения изменён или удалён (SHA не совпадает), задание завершается ошибкой `am.import.run.plan_changed`.

### 4. Итоги кандидатов

| Кандидат | Итог |
|---|---|
| `action = skip` | `skipped` (задание не создаётся, строка в сводке) |
| `action = ask` или хотя бы одна запись с `decision = ask` | `deferred`, `am.import.run.unresolved`; задание не создаётся |
| `new_asset` с входом `archive` / `multivolume_archive` | `deferred`, `am.import.run.archive_input_unsupported`; задание не создаётся |
| `new_asset` только с `file` / `directory` | задание `import.asset` → `created` / `failed` / `needs_decision` |
| `ready_asset` | задание `import.asset` → классификация и размещение как `add-ready` |

- Решение `ask` не превращается в `include` или `exclude` по умолчанию. Режима «принять всё» в T-030 нет: план редактирует пользователь.
- Готовый ассет: `import_ready` без замены. Существующий ассет с тем же ID при любом соотношении ревизий не заменяется автоматически (ТЗ §6.3.4). Итог — классификация `add-ready` и `deferred` с причиной, если нужно решение.
- Отложенные кандидаты перечисляются в сводке с причиной, чтобы пользователь отредактировал план и запустил его снова (новый SHA).

### 5. Создание ассета из файлов и папок

- **Свежесть источника.** Перед записью для каждого включаемого файла сравнить размер и `mtime_ns` с планом, а если в плане есть `sha256`, то и SHA, вычисленный при упаковке. Расхождение — ошибка кандидата `am.import.source_changed` (строгий режим) с указанием узла. Сканировать заново и молча брать новое содержимое нельзя.
- **После фиксации** повторно сравнить размер и `mtime_ns` включённых файлов. Если файл изменился во время упаковки, созданный ассет переносится в корзину (`Catalog::trash`, обратимо), задание завершается ошибкой `am.import.source_changed_during_import`. В итоге указывается `asset_id` в корзине.
- **Состав.** Один Input на кандидата: `mode` из плана, `completeness` по факту.
  - `objects[]` — по одному на вход кандидата: `file` или `directory`, `name`, `name_raw`, `relative_path` от `base_path` кандидата, `size_bytes`, `sha256` для файлов.
  - `entries[]` — по записи на каждый узел кандидата, включая каталоги (`entry_kind = directory`) и исключённые. `path` — исходный путь относительно объекта, `path_raw` — исходные байты.
  - Итог записи:
    - `include` → `included` или `included_renamed` с `file_id`;
    - `exclude` → `excluded` с `exclusion{rule, rules_version, by}`;
    - `skip` (ссылка) → `not_processed` с `problem{code: link_not_followed, message_id}`.
  - `by`: `auto`, если решение совпадает с исходным предложением правила (`certain` → `exclude`). `user`, если пользователь исключил запись без правила или против предложения. Исходное предложение — `rule_id` / `confidence` записи плана.
- **Недоступные узлы.**
  - Строгий режим (по умолчанию): любой `unavailable` узел с решением `include` или `ask` делает кандидата ошибкой `am.import.run.source_unavailable` до записи. Облачная заглушка (`cloud_placeholder`) — не исключение: run её не открывает.
  - Частичный режим (`mode = partial` в плане, только явный выбор пользователя): ассет создаётся из доступного. У недоступных записей `result = unavailable` с `problem` из плана, `completeness = incomplete`. Запись, которую не удалось прочитать при упаковке, — `failed`.
- **Пути в ZIP.** Используется `paths::ZipPathMapper` (ADR-0012) с префиксом `content/<InputID>/`. Исходная иерархия от `base_path` сохраняется. Переименования — `included_renamed` с `rename_reason`, коллизии без учёта регистра (`case_collision` в плане) разрешаются переименованием, а не отбрасыванием.
- **Исходные имена.** Hex плана (`name_raw` / `path_raw`) переводится в строгий Base64 (`rawName.base64`, RFC 4648). `raw_encoding` плана переходит в `encoding_guess`: `utf-16le` / `posix`. `rawName` пишется, только если исходные байты не совпадают с UTF-8 отображаемого имени.
- **Происхождение.** Каждый `context.sources[]` кандидата → `origin.sources[]`: `{id, kind, name}` и claim `directory_inference` без `review`, т. е. не подтверждён (container_manifest_v1 §5, §9.3). Input получает `source_refs`. Если формы claim по спецификации недостаточно для однозначной записи — остановиться и задать вопрос, не упрощать молча. `suggested_links` в T-030 в манифест не пишутся (только в итог задания), `filtered_links` — нигде.
- **Метаданные.** `title` — имя каталога или файла корня кандидата (отображаемое). Прочие метаданные не выводятся.
- Если `CreateRequest` не позволяет передать `origin` и `source_refs`, расширить его необязательными полями в `storage::repository`. Проверку инвариантов (V3) не ослаблять. Запись ZIP — только через `AssetRepository` / `Catalog` (ADR-0025).

### 6. Задания, ресурсы и параллельность

- `import.run` — родитель (`await_children`), `import.asset` — дочерние. Пауза, отмена и повтор — как в T-026. Безопасная точка — между кандидатами и до фиксации. Отмена до фиксации удаляет временные данные (протокол фиксации делает это сам), ассет не появляется.
- Ресурс: аренда `io` на задание кандидата, `cpu` — по необходимости хеширования. Параллельность — из профиля исполнителя.
- **Потокобезопасность `Catalog` и `AssetRepository`.** До включения параллельных заданий установить и описать гарантию. Если одновременные `create` / `import_ready` из разных потоков не доказаны безопасными, обращения к каталогу сериализуются: чтение и хеширование источников идут параллельно, фиксация — по одной. Выбранную схему описать в `jobs_and_sessions.md` или `import_pipeline.md` и покрыть тестом с несколькими параллельными кандидатами.
- **Масштаб выборки (замечание 1 T-026).** Цикл исполнителя читает из `jobs` только активные состояния (`JobFilter::states` или эквивалент в хранилище). Подзадания выбираются по `parent_id` через индекс, а не перебором «родитель × все задания». Тест: шаг планировщика при 10 000 завершённых заданий в истории выполняет столько же запросов к хранилищу и выбирает столько же строк, что и при пустой истории (счётчик через внедрение или тестовое хранилище; без порогов по времени). Схема `state.sqlite` меняется только добавлением индекса, если его нет. Изменение версии схемы — по `versioning.md`.

### 7. Журнал операций и журнал программы

- На каждый созданный или размещённый ассет — запись журнала операций библиотеки, как у `add-ready`: план (SHA), кандидат, `asset_id`, итог. Отложенные и пропущенные кандидаты попадают в итог родительского задания, а не в журнал операций.
- Журнал программы: `import.run.started` / `finished` (info), `import.run.candidate_failed` (warning). Пути — только `FieldKind::path`.

### 8. Чего в T-030 нет

- Раскрытия архивов, томов, вложенных контейнеров, `nested_action` (следующая задача «ZIP как вход»). Перевод индексов томов с 1 на 0 выполняется там же.
- Экстракторов метаданных, предпросмотров, миниатюр (этапы 10, 12).
- Объединения кандидата с существующим ассетом, дополнения происхождения существующего ассета, замены ревизии.
- Режима «принять все предложения» и GUI.
- Корпуса 16 сценариев (отдельная задача M3a). Здесь — синтетические фикстуры задачи.

## Тесты

Синтетические фикстуры создаются в `.cache/` тестом. Реальная коллекция не используется. Записывающие сценарии на WSL/DrvFS пропускаются по прежнему правилу (запись требует проверенного фиксированного NTFS), с причиной SKIP.

- `ImportPlanView.*` — построение вида из валидного плана; отказ построителя на JSON, который не проходил `parse_plan`, невозможен по API. Числа вне диапазона отвергаются уже `parse_plan`.
- `ImportRun.*` (services/application):
  1. Папка с вложенными каталогами, исключённым `.DS_Store` и `skip`-ссылкой → один ассет. В манифесте:
     - объекты и записи всех узлов, каталоги — `directory`;
     - `excluded` с `by = auto`, ссылка — `not_processed`;
     - файлы побайтно равны источникам, SHA совпадает.
  2. Пользователь исключил файл без правила → `by = user`. Пользователь включил предложенный к исключению файл → `included`.
  3. Имена с повреждённой кодировкой (POSIX-байты и UTF-16 с одиночным суррогатом, где платформа позволяет) → `name_raw` / `path_raw` Base64, отображение с U+FFFD. Коллизия регистра → `included_renamed` / `collision`.
  4. Профиль `telegram`: `origin.sources[]` с `telegram_channel`, claim без `review`, `source_refs` у Input.
  5. `ready_asset` → размещён, как `add-ready`. Тот же ID уже в библиотеке → без замены, `deferred` с классификацией.
  6. Кандидат с `archive` → `deferred` / `archive_input_unsupported`; `ask` → `deferred` / `unresolved`; `skip` → `skipped`. Код возврата 3.
  7. Строгий режим: недоступный узел (через внедрение) → ошибка до записи, временных файлов нет. Частичный режим: ассет с `unavailable`, `completeness = incomplete`, V1–V4 проходят.
  8. Источник изменён после скана (размер / `mtime`) → `source_changed`, ассета нет. Изменение во время упаковки (внедрение между упаковкой и проверкой после фиксации) → ассет в корзине, `source_changed_during_import`.
  9. Повторный запуск того же плана → `already_imported`, второго ассета нет. Сбой после фиксации до отметки задания (внедрение) → повтор находит ассет по `asset_id` + `InputID` и завершается успехом. Чужой ассет с тем же ID → `asset_id_conflict`.
  10. План изменён между постановкой и выполнением → `plan_changed`.
  11. Отмена во время выполнения → заданий `running` не остаётся, незафиксированных ассетов и временных файлов нет, завершённые кандидаты сохранены. Пауза и возобновление.
  12. Несколько кандидатов параллельно (2–4 задания) → все созданы, `library verify` чист, индекс согласован.
- `JobsExecutor.*` / хранилище — независимость шага от объёма истории (п. 6). Прежние тесты исполнителя проходят на обоих хранилищах.
- CLI `tests/cli/test_import_run_cli.py`: `scan` → правка плана (JSON) → `run` → `library status` / `jobs list`; коды 0 / 2 / 3 / 4; `--enqueue-only` + `am jobs run` дают тот же итог; `--json`-сводка; повторный запуск.
- Все созданные ассеты проходят `validate` (V1–V4) и `verify --full`.

Новые тесты — с `RecordProperty("req", …)`: К-n импорта, SC-n сценариев, NFR надёжности по `acceptance_matrix.md`.

## Файлы (что можно менять)

- `include/am/services/import/` (новые `plan_view.hpp`, `run.hpp` — имена уточнить), `src/services/import/` (новые `.cpp`; `plan.cpp` — только для построителя вида).
- `include/am/application/import_service.hpp`, `src/application/common/import_service.cpp`, `src/application/common/library_service.cpp` (регистрация обработчиков в `JobService`, общий код с `add-ready`), `src/application/cli/main.cpp` (подкоманда `import run`).
- `include/am/storage/repository/repository.hpp`, `src/storage/repository/repository.cpp` — только необязательные `origin` / `source_refs` в `CreateRequest` (п. 5), если нужны.
- `include/am/contracts/job_store.hpp`, `src/storage/db/job_store.cpp`, `src/jobs/executor.cpp` — только выборка по состояниям и `parent_id` (п. 6); SQL-схема `state.sqlite` — только индекс.
- `resources/messages/en.toml`.
- Тесты: новые `tests/unit/test_import_run.cpp`, `tests/unit/test_import_plan_view.cpp`, `tests/cli/test_import_run_cli.py`; дополнения `tests/unit/test_jobs.cpp`; `tests/CMakeLists.txt`.
- Документы: `docs/architecture/import_pipeline.md` (раздел «Выполнение плана (T-030)»), `docs/architecture/jobs_and_sessions.md` (если меняется гарантия), `docs/specifications/versioning.md` (только при смене версии схемы `state.sqlite`), `docs/plan/coverage_matrix.md`, `docs/plan/acceptance_matrix.md`, `CHANGELOG.md`, этот файл (статус и отчёт), строка задачи в `docs/tasks/README.md`.

Не менять: опубликованную схему манифеста 1.0 и golden-корпус, схему плана v1 (если без изменения не обойтись — вопрос), семантику остановки `Executor`, ADR, ТЗ, материалы владельца.

## Критерии приёмки

1. Четыре прогона зелёные: `build_min --ci`, `build_cli --ci -LE periodic`, `build_cli --offline --ci -LE periodic`, Linux CLI `--offline --ci -LE periodic` (SKIP — с причинами).
2. Каждый кандидат плана получает ровно один итог из п. 4. Ни одно решение `ask` не исполнено без пользователя, ни один архивный вход не обработан частично.
3. Созданные ассеты проходят V1–V4 и `verify --full`. Байты файлов равны источникам. Исключённые и недоступные записи учтены в `inputs[].entries`.
4. Повторный запуск и повтор после сбоя не создают второго ассета (тесты 9–10).
5. Изменение источника после скана или во время упаковки не даёт молча созданного ассета (тест 8).
6. Шаг исполнителя не зависит от объёма истории (тест п. 6). Гарантия параллельного доступа к каталогу описана и проверена (тест 12).
7. `arch_check` проходит. Запись ZIP — только через `AssetRepository`.
8. Матрицы покрытия и приёмки обновлены по фактическому статусу.

## Отчёт исполнителя

Ветка `codex/T-030`, база `1f53244`, начало работы `92d525c`. Основная реализация — `7e693ff`, защита ID и повтор через корзину по В-10 — `2fb2069`, устранение предупреждений без изменения поведения — `74e4668`. Документальные контрольные точки — `b226d3b`, `74bc0e4`. Работа завершена, статус `review`; push и слияние не выполнялись.

### Выполнение критериев

1. Четыре обязательных полных прогона завершены с кодом 0: min — 148 PASS, Windows CLI — 637 PASS, Windows CLI offline — 637 PASS, Linux CLI offline — 577 PASS и 57 обоснованных SKIP. Ошибок нет; таблица и фактические итоговые строки ниже.
2. Кандидаты получают created/imported, skipped, deferred, failed, already_imported/already_queued; ask не исполняется, архивный вход откладывает весь кандидат. Готовые ассеты классифицируются без замены; явное решение skip не открывает источник повторно.
3. Один Input и реальные объекты корней; все наблюдённые узлы, каталоги, исключения, ссылки и недоступные файлы учтены. SHA/байты и V1–V4 проверяются через репозиторий и library verify. Частичный режим повторяет только структурно определённую ошибку чтения, исключая каждый файл не более одного раза.
4. Ключ — SHA точных байтов плана и candidate_id. Payload хранит путь, SHA и заранее выданные UUIDv4, без копии плана. Повтор после фиксации узнаёт InputID и не переписывает ZIP. Свой ассет в корзине требует решения; после ручного restore повтор не пишет ZIP, после purge создаёт с прежними ID, skip оставляет корзину. Чужой InputID или две локации одного ID дают конфликт. Исходный план не меняется.
5. Свежесть проверяется для всех фактически включаемых файлов, в том числе представленных только inputs.objects при пустом entries (отдельный регрессионный тест). Плановый SHA сверяется с упаковкой; без него после фиксации — stat, при изменении ассет в корзине, asset_id сохранён в failed-результате.
6. Планировщик выбирает состояния и parent_id по индексам. Один параметризованный тест на memory/SQLite сравнивает число запросов и выбранных строк с 10 000 завершённых заданий; время не нормируется. Предварительное чтение/хеширование параллельно, Catalog-мутации и упаковка сериализованы общим mutex. Проверены четыре параллельных кандидата, полный verify и индекс.
7. ZIP пишет только AssetRepository/Catalog. Потокобезопасность и граница отмены описаны в import_pipeline.md. После replace поздняя отмена игнорируется; до него временный ZIP и строка журнала очищаются.
8. Матрицы содержат фактическое частичное покрытие SC-1/2/3, K-5/115/121; полные 16 сценариев и распаковка архивов не объявлены завершёнными.

### Проверки

- Повтор `tools\build_min.cmd --ci` после устранения предупреждений: `100% tests passed, 0 tests failed out of 148`, `Total Test time (real) = 64.26 sec`, код 0. Лог `.cache/t030/final-min.log`.
- `ImportRun.*` и `ImportRunManifest.*`: подтверждены создание, provenance, недоступность/частичный режим, свежесть, повтор после фиксации, чужой ID, четыре параллельных кандидата, пауза/отмена, готовые ассеты и повреждённый UTF-16. Исправление неявного включения из inputs.objects проверено отдельным тестом; решение skip готового ассета — повтором после удаления источника.
- Новые тесты jobs/storage прошли на memory/SQLite; дополнительные проверки репозитория покрывают SHA при упаковке, структурный FileID ошибки чтения и отсутствие такого FileID при отказе записи назначения.
- CLI после исправления: 17/17 целевых проверок (вместе с PlanView/планом/репозиторием), 11.77 s; cli_import_run проверяет коды 0/2/3/4, enqueue-only + jobs run, сводку, повтор и failed-результаты show/list.
- `tools\build_cli.cmd --offline --ci -V -R ImportRunManifest`: 6/6 PASS, 12.38 s. Логи и измерения — `.cache/t030/manifest-final-targeted2.log`, `.cache/t030/pattern-results.json`, `.cache/t030/anchor-results.json`.
- После В-10: целевой `ctest --test-dir build/win-cli --output-on-failure -R "ImportRun|AssetRepository.(ExplicitIdentity|TrashSnapshot|DuplicateOperation|ExpectedSource|PackingSha)|IndependentCopy.*Collision|Trash.(ConflictsNever|RecoveryExplicit)"` — `100% tests passed, 0 tests failed out of 37`, `Total Test time (real) = 58.84 sec`. Лог `.cache/t030/v10-final-targeted.log`. Включает restore/purge/skip, отсутствие дубля ID, защиту прямого create/copy и гонку между preflight и блокировкой фиксации.
- Полные прогоны выполнены на `2fb2069`, включая arch_check, schema_check, sql_check, golden_zip и golden_regen_check; `periodic` исключён согласно задаче. Периодический замер 250 000 не запускался. Логи и JUnit — `.cache/t030/full-{min,windows,offline,linux}.{log,xml}`.

| Команда | Фактический итог ctest | Время ctest | Код команды |
|---|---|---|---|
| `tools\build_min.cmd --ci` | `100% tests passed, 0 tests failed out of 148` — 148 PASS, 0 SKIP | `Total Test time (real) = 29.71 sec` | 0 |
| `tools\build_cli.cmd --ci -LE periodic` | `100% tests passed, 0 tests failed out of 637` — 637 PASS, 0 SKIP | `Total Test time (real) = 1561.34 sec` | 0 |
| `tools\build_cli.cmd --offline --ci -LE periodic` | `100% tests passed, 0 tests failed out of 637` — 637 PASS, 0 SKIP | `Total Test time (real) = 1552.71 sec` | 0 |
| WSL `tools/build_linux.sh cli --offline --ci -LE periodic` | `100% tests passed, 0 tests failed out of 634` — 577 PASS, 57 SKIP | `Total Test time (real) = 2009.46 sec` | 0 |

Число `req_report` для CLI (638 Windows / 635 Linux) включает один зарегистрированный periodic-тест, исключённый из прогона. Итоги таблицы взяты из ctest/JUnit, а не из числа регистраций. Невалидных req ID — 0 во всех четырёх прогонах. Время команд вместе с конфигурацией и сборкой: 33.54 / 1586.91 / 1560.61 / 2730.82 s соответственно.

Причины всех 57 Linux SKIP (WSL/DrvFS):

| Количество | Группа | Причина |
|---|---|---|
| 23 | ImportRun.* и Decisions/ImportRunTrashRetry.* | Продукционная запись допускается на проверенном fixed NTFS; WSL/DrvFS остаётся read-only. Все эти случаи прошли на Windows. |
| 12 | cli_logging_commit; cli_jobs_{verify,decision,failure_and_controls,locked,signal}; cli_library_{rebuild_list,sync,trash,add_ready,copy,log_writes} | Те же ограничения записи. Исполнитель и storage/services проверены на Linux через внедрённые синтетические capabilities; CLI чтения проходят без SKIP. |
| 4 | AssetService.{CreateRefusalsAndPublishRaceNeverReplaceOutput,PostCreateFailureDeletesOnlyTheNewOutput,ValidationAndExtractionUseSharedV3AndPreserveSuccessfulFiles,StreamScopesMinorAndCorruption} | Создание fixtures требует fixed NTFS; process-тесты проверяют read-only варианты. |
| 1 | ImportReady.KillVerifiedAndReplacedForNewAndReplacementRecoverWithIndex | Crash-process и fixed-NTFS доказательства относятся к Windows; инъекции отказов протокола проходят и на Linux. |
| 1 | LibraryVerify.PurgeAllWithMissingTrashIsEmptySuccess | Purge — продукционная запись; сама проверка библиотеки тестируется без SKIP. |
| 12 | FileLog (1), DiagExport offline (2), CommitProtocol Win32 (7), AssetRepository process/kill (2) | Специфичные Win32 sharing, FILE_ATTRIBUTE_OFFLINE, fallback, OS/NTFS evidence и завершение дочерних Win32-процессов. |
| 2 | ImportRunManifest.{PublishedPatternSurveyOnMsvc,AnchoredPatternSurveyReportsV2AndIndependentV3} | Измеряется именно MSVC std::regex. Эквивалентность линейных проверок, длинные значения и пользовательские схемы проверены на обеих платформах. |
| 1 | ImportScan.RawInvalidNamesArePreservedWhenTheFilesystemRetainsThem | Данный mount нормализует неверное native-имя до перечисления и не сохраняет raw-fixture. |
| 1 | Sync.FullDetectsEqualSizeAndRestoredMtime | Данный mount не восстанавливает mtime точно до исходного значения. |

После полных прогонов в `74e4668` механически устранены предупреждения GCC в новом коде: явное создание optional, локальная строка ID перед получением ссылки, явная инициализация пустого stop_token и скобки вокруг тестовых макросов. Поведение и принимаемые данные не изменены. На этой версии дополнительно выполнены:

- `tools\build_min.cmd --ci`: `100% tests passed, 0 tests failed out of 148`, `Total Test time (real) = 64.26 sec`, код 0 (`final-min.log`).
- `tools\build_cli.cmd --offline --ci -R ImportRun`: `100% tests passed, 0 tests failed out of 29`, `Total Test time (real) = 82.08 sec`, код 0 (`warnings-windows.log`).
- `ctest --test-dir build/win-cli --output-on-failure -R "AssetRepository.(ExplicitIdentity|TrashSnapshot|ExpectedSource|PackingSha)|CommitProtocol.Cancellation|cli_import_run"`: `100% tests passed, 0 tests failed out of 7`, `Total Test time (real) = 23.81 sec`, код 0 (`warnings-storage-cli-windows.log`).
- WSL `tools/build_linux.sh cli --offline --ci -R 'ImportRun|AssetRepository.(ExplicitIdentity|TrashSnapshot|ExpectedSource|PackingSha)|ImportReady|CommitProtocol.Cancellation'`: `100% tests passed, 0 tests failed out of 49`, `Total Test time (real) = 91.89 sec`, код 0; 23 PASS и 26 SKIP по тем же причинам выше (`warnings-linux.log`).

Все дополнительные логи — в `.cache/t030/`. Предупреждений в новых строках после повторной сборки нет; оставшиеся предупреждения GCC относятся к неизменённым строкам, вне исправлений задачи. Полные прогоны на `2fb2069` и целевые проверки механических изменений на `74e4668` указаны раздельно.

### Уточнения и согласованные выходы за рамки

- В-1: repository.hpp/.cpp — необязательные stop_token, expected_sha256, SourceFailure; commit/protocol.hpp/.cpp — отмена до публикации; src/services/CMakeLists.txt — новые .cpp. Прежнее поведение без новых параметров сохранено; отдельное изменение контракта явно занятого ID согласовано позже в В-10.
- В-2: executor.cpp — явно заданные непустые результаты failed/needs_decision сохраняются вместе с error/decision; `{}` по умолчанию не записывается. Для interrupted/resource-состояний результата нет. В начале queued → running он сбрасывается; SQL-схема и её версия прежние.
- В-3: любое ask раньше unavailable: deferred/unresolved, без дочернего задания; сводка отмечает нерешённую недоступность. Include: strict — ошибка, partial — unavailable/incomplete; exclude не читает источник. Уточнение внесено в import_pipeline.md.
- В-4: виртуальный объект по title_hint не создаётся. InputObject на реальный корень; внутри профильной границы — существующий каталог границы. Несколько корней получают отдельные префиксы ZIP, коллизии переименовываются. title_hint — только title. Уточнение внесено в import_pipeline.md.
- В-5: tests/unit/job_test_store.hpp — только сброс result_json на queued → running. Контракт и jobs_and_sessions.md дополнены. Общий параметризованный тест проверяет оба хранилища.
- В-6: parse_plan — линейная проверка raw-hex точно по `^([0-9a-f]{2})+$`; копия для схемы компактная, исходные значения неизменны. Список трёх полей сверяется со схемой, 65536 hex-байт проходят, неверные значения дают прежний JSON Pointer. Ограничения minLength/maxLength учитываются; неизвестные будущие ограничения закрывают этот путь до обновления реализации.
- В-7: src/domain/manifest/schema_validator.cpp — линейный rawName.base64 по опубликованному языку: пустая строка и AB== разрешены, хвостовые биты не ужесточены. Обе ссылки rawName сверены со схемой. V1+V2 проходят на 8200 байтах Base64, V2 — на 65536. Документ, opaque-значения и опубликованная схема не меняются.
- В-8а: встраивание схемы механически перенесено из storage в domain: src/domain/CMakeLists.txt, src/storage/CMakeLists.txt, getter published_schema_parts в codec.hpp и два прежних потребителя repository.cpp. Заголовок приватный, генерируется в build, в Git не добавляется; дублирования нет. Полное сравнение DOM включает линейные проверки только для нормативной схемы. Побайтовое совпадение со schema-файлом и прежний путь пользовательских схем проверены.
- В-8б: измеренные дефекты исправляются по нормативному определению/указателю, без ограничений длины. Кроме rawName падает только file.type.id с точечными компонентами: 297 PASS / 299 error_stack; после — 65537 PASS. Точка в опубликованном шаблоне действительно экранирована. Остальные шаблоны не исправлялись.
- В-9: оракул коротких строк — std::regex_match, семантика ECMA-262 без m. Якоря относятся ко всей строке, поэтому LF/CR/CRLF в начале/середине/конце отклоняются на обеих платформах для двух исправленных шаблонов. Это устранение отклонения Windows-валидатора от нормы, не ужесточение формата. Пользовательские схемы сохраняют jsoncons без подстановок.
- В-10: repository.hpp/.cpp — `snapshot(ref, include_trash=false)`, признак `in_trash`, снимок корзины только для чтения. Явные ID create/copy проверяются в обеих локациях под той же AssetLock, что и фиксация. Для этого в уже согласованном commit/protocol.hpp/.cpp добавлено необязательное предусловие до создания временного ZIP и строки журнала. Пустое предусловие не меняет прежний путь. Тест с отдельным репозиторием создаёт активный/удалённый ID после preflight и подтверждает отказ под удерживаемой блокировкой. Два прежних ожидания ошибок для явно занятых ID в test_asset_repository.cpp и test_independent_copy.cpp механически обновлены на предписанный `am.repo.asset_id_exists`; тесты create без asset_id не менялись. Run требует решения retry/skip для своего InputID в корзине, сохраняет asset_id и предупреждение о возможном отличии восстановленного содержимого. Уточнение внесено в import_pipeline.md.

Проверка длинного rawName выявила дефект V2 опубликованного 1.0 независимо от производителя ZIP. Исправлен валидатор, не формат; версия и golden-корпус неизменны. Заметка о линейной реализации — рядом с валидатором, CHANGELOG содержит исправление.

### Измерение всех шаблонов манифеста (MSVC 14.44)

Это измерения, а не лимиты. Для ограниченных шаблоном ID проверена максимально допустимая длина и большое число разделителей; для неограниченных — указанные размеры. Порог rawName измерен на квартетах A, file.type.id — на a.a.…; отдельный файл-тип из одного компонента проходит 32768 байт. Непроверенные большие длины не объявляются безопасными.

| Определение / указатель | Шаблон | Порог до исправления (байт) | Проверено после / без исправления | Применённые ссылки |
|---|---|---:|---|---|
| `/$defs/claimCore/properties/confidence` | `^(0(\.[0-9]+)?\|1(\.0+)?)$` | не найден на проверенных длинах | 32770 PASS; исправление не требуется | нет линейной замены |
| `/$defs/crc32` | `^[0-9a-f]{8}$` | не найден на проверенных длинах | 8 PASS; исправление не требуется | нет линейной замены |
| `/$defs/date` | `^[0-9]{4}-[0-9]{2}-[0-9]{2}$` | не найден на проверенных длинах | 10 PASS; исправление не требуется | нет линейной замены |
| `/$defs/decimalString` | `^-?(0\|[1-9][0-9]*)(\.[0-9]+)?$` | не найден на проверенных длинах | 32770 PASS; исправление не требуется | нет линейной замены |
| `/$defs/extension/properties/attachments/items/properties/path` | `^extensions/` | не найден на проверенных длинах | 200 PASS; исправление не требуется | нет линейной замены |
| `/$defs/fieldDefinition/properties/dimension/anyOf/0` | `^[a-z][a-z0-9_]{0,63}$` | не найден на проверенных длинах | 64 PASS; исправление не требуется | нет линейной замены |
| `/$defs/fieldDefinition/properties/dimension/anyOf/1` | `^plugin:(?=[^:]{3,96}:)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+:(?=[^:]{1,64}$)[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)*$` | не найден на проверенных длинах | 168 PASS; исправление не требуется | нет линейной замены |
| `/$defs/fieldId` | `^(?:user:[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\|plugin:(?=[^:]{3,96}:)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+:(?=[^:]{1,64}$)[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)*)$` | не найден на проверенных длинах | 168 PASS; исправление не требуется | нет линейной замены |
| `/$defs/file/properties/generation/properties/tool/anyOf/0` | `^core:(?=[^:]{1,64}$)[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)*$` | не найден на проверенных длинах | 69 PASS; исправление не требуется | нет линейной замены |
| `/$defs/file/properties/generation/properties/tool/anyOf/1` | `^plugin:(?=[^:]{3,96}:)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+:(?=[^:]{1,64}$)[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)*$` | не найден на проверенных длинах | 168 PASS; исправление не требуется | нет линейной замены |
| `/$defs/file/properties/type/properties/id` | `^[a-z0-9]+(\.[a-z0-9_-]+)*$` | 299 | 65537, V2 PASS | `#/$defs/file`: `/properties/files/items` → `/files/*/type/id` |
| `/$defs/int64String` | `^-?(0\|[1-9][0-9]{0,18})$` | не найден на проверенных длинах | 20 PASS; исправление не требуется | нет линейной замены |
| `/$defs/pluginId` | `^(?=.{3,96}$)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$` | не найден на проверенных длинах | 96 PASS; исправление не требуется | нет линейной замены |
| `/$defs/rawName/properties/base64` | `^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==\|[A-Za-z0-9+/]{3}=)?$` | 340 | 65536 Base64; V1+V2 также 8200 | `#/$defs/rawName`: `/$defs/inputObject/properties/name_raw`, `/$defs/inputEntry/properties/path_raw` |
| `/$defs/relKey/anyOf/0` | `^[a-z][a-z0-9_-]{0,63}$` | не найден на проверенных длинах | 64 PASS; исправление не требуется | нет линейной замены |
| `/$defs/relKey/anyOf/1` | `^plugin:(?=[^:]{3,96}:)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+:(?=[^:]{1,64}$)[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)*$` | не найден на проверенных длинах | 168 PASS; исправление не требуется | нет линейной замены |
| `/$defs/revisionRecord/properties/changes/items/properties/kind/anyOf/0` | `^[a-z][a-z0-9_]{0,63}$` | не найден на проверенных длинах | 64 PASS; исправление не требуется | нет линейной замены |
| `/$defs/revisionRecord/properties/changes/items/properties/kind/anyOf/1` | `^plugin:(?=[^:]{3,96}:)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+:(?=[^:]{1,64}$)[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)*$` | не найден на проверенных длинах | 168 PASS; исправление не требуется | нет линейной замены |
| `/$defs/revisionRecord/properties/operation/anyOf/0` | `^[a-z][a-z0-9_]{0,63}$` | не найден на проверенных длинах | 64 PASS; исправление не требуется | нет линейной замены |
| `/$defs/revisionRecord/properties/operation/anyOf/1` | `^plugin:(?=[^:]{3,96}:)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+:(?=[^:]{1,64}$)[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)*$` | не найден на проверенных длинах | 168 PASS; исправление не требуется | нет линейной замены |
| `/$defs/sha256` | `^[0-9a-f]{64}$` | не найден на проверенных длинах | 64 PASS; исправление не требуется | нет линейной замены |
| `/$defs/storagePath` | `^(?!.*(^\|/)\.\.?(/\|$))(?!.*//)(content\|generated\|extensions)/[^\\]*[^/\\]$` | не найден на проверенных длинах | 200 PASS; исправление не требуется | нет линейной замены |
| `/$defs/timestamp` | `^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?Z$` | не найден на проверенных длинах | 32789 PASS; исправление не требуется | нет линейной замены |
| `/$defs/typeId/anyOf/1` | `^plugin:(?=[^:]{3,96}:)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+:(?=[^:]{1,64}$)[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)*$` | не найден на проверенных длинах | 168 PASS; исправление не требуется | нет линейной замены |
| `/$defs/uint64` | `^(0\|[1-9][0-9]{0,19})$` | не найден на проверенных длинах | 20 PASS; исправление не требуется | нет линейной замены |
| `/$defs/url` | `^[A-Za-z][A-Za-z0-9+.-]*:[^\s]+$` | не найден на проверенных длинах | 32792 PASS; исправление не требуется | нет линейной замены |
| `/$defs/uuid4` | `^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$` | не найден на проверенных длинах | 36 PASS; исправление не требуется | нет линейной замены |

### Измерение якорей и независимой V3 (В-9)

Суффикс — `<допустимое>\nX`, префикс — `X\n<допустимое>`. Старый путь движка измерен через SchemaValidator с пользовательской leaf-схемой на известном строковом поле, чтобы охватить и определения, которые V2 не применяет прямо к custom_values. Нормативный V2 измерен на указанном реальном поле. V3 запущена независимо даже при отказе V2 только для измерения: сравниваются новые ошибки с базовым вариантом. У plugin-операции истории уже есть одна базовая ошибка history_coverage, поэтому отсутствие новой не означает успешную V3. В нормальном pipeline V3 выполняется после V2.

| Шаблон (указатель) / фактическое поле | Старый V2-движок: суффикс / префикс | Нормативный V2 после двух исправлений: суффикс / префикс | Новые ошибки независимой V3: суффикс / префикс |
|---|---|---|---|
| `/$defs/claimCore/properties/confidence`<br>`/asset/metadata/claims/0/confidence` | принят / принят | принят / принят | нет / нет |
| `/$defs/crc32`<br>`/inputs/0/entries/0/crc32` | принят / принят | принят / принят | нет / нет |
| `/$defs/date`<br>`/asset/metadata/dates/acquired` | принят / принят | принят / принят | timestamps:/asset/metadata/dates/acquired / timestamps:/asset/metadata/dates/acquired |
| `/$defs/decimalString`<br>`/asset/metadata/custom_values/0/value` | принят / принят | принят / принят | value_representation:/asset/metadata/custom_values/0/value / value_representation:/asset/metadata/custom_values/0/value |
| `/$defs/extension/properties/attachments/items/properties/path`<br>`/extensions/org.example.stlstats/attachments/0/path` | принят / принят | принят / принят | storage_paths:/extensions/org.example.stlstats/attachments/0/path / storage_paths:/extensions/org.example.stlstats/attachments/0/path |
| `/$defs/fieldDefinition/properties/dimension/anyOf/0`<br>`/field_definitions/0/dimension` | принят / принят | принят / принят | нет / нет |
| `/$defs/fieldDefinition/properties/dimension/anyOf/1`<br>`/field_definitions/0/dimension` | принят / принят | принят / принят | нет / нет |
| `/$defs/fieldId`<br>`/field_definitions/0/id` | принят / принят | принят / принят | custom_value_field:/asset/metadata/custom_values/0/field_id / custom_value_field:/asset/metadata/custom_values/0/field_id |
| `/$defs/file/properties/generation/properties/tool/anyOf/0`<br>`/files/0/generation/tool` | принят / принят | отклонён / отклонён | нет / нет |
| `/$defs/file/properties/generation/properties/tool/anyOf/1`<br>`/files/0/generation/tool` | принят / принят | отклонён / отклонён | нет / нет |
| `/$defs/file/properties/type/properties/id`<br>`/files/0/type/id` | принят / принят | отклонён / отклонён | нет / нет |
| `/$defs/int64String`<br>`/asset/metadata/custom_values/0/value` | принят / принят | принят / принят | value_representation:/asset/metadata/custom_values/0/value / value_representation:/asset/metadata/custom_values/0/value |
| `/$defs/pluginId`<br>`/extensions/<key>` | принят / принят | принят / принят | нет / нет |
| `/$defs/rawName/properties/base64`<br>`/inputs/0/entries/0/path_raw/base64` | принят / принят | отклонён / отклонён | нет / нет |
| `/$defs/relKey/anyOf/0`<br>`/asset/metadata/links/0/rel` | принят / принят | принят / принят | нет / нет |
| `/$defs/relKey/anyOf/1`<br>`/asset/metadata/links/0/rel` | принят / принят | принят / принят | нет / нет |
| `/$defs/revisionRecord/properties/changes/items/properties/kind/anyOf/0`<br>`/history/entries/0/changes/0/kind` | принят / принят | принят / принят | нет / нет |
| `/$defs/revisionRecord/properties/changes/items/properties/kind/anyOf/1`<br>`/history/entries/0/changes/0/kind` | принят / принят | принят / принят | нет / нет |
| `/$defs/revisionRecord/properties/operation/anyOf/0`<br>`/history/entries/0/operation` | принят / принят | принят / принят | history_coverage:/history / history_coverage:/history |
| `/$defs/revisionRecord/properties/operation/anyOf/1`<br>`/history/entries/0/operation` | принят / принят | принят / принят | нет / нет |
| `/$defs/sha256`<br>`/files/0/sha256` | принят / принят | принят / принят | нет / нет |
| `/$defs/storagePath`<br>`/files/0/storage_path` | принят / принят | принят / принят | storage_paths:/files/0/storage_path / storage_paths:/files/0/storage_path |
| `/$defs/timestamp`<br>`/asset/created_at` | принят / принят | принят / принят | timestamps:/asset/created_at, asset_times:/asset/created_at / timestamps:/asset/created_at, asset_times:/asset/created_at |
| `/$defs/typeId/anyOf/1`<br>`/field_definitions/0/type` | принят / принят | принят / принят | нет / value_representation:/asset/metadata/custom_values/0/value |
| `/$defs/uint64`<br>`/files/0/size_bytes` | принят / принят | принят / принят | byte_sizes_uint64:/files/0/size_bytes / byte_sizes_uint64:/files/0/size_bytes |
| `/$defs/url`<br>`/asset/metadata/links/0/url` | принят / принят | принят / принят | urls:/asset/metadata/links/0/url / urls:/asset/metadata/links/0/url |
| `/$defs/uuid4`<br>`/asset/id` | принят / принят | принят / принят | нет / нет |

Python: schema_check использует Draft202012Validator из .cache/pylib. Фактически у Base64 и file.type.id допускается только завершающий LF (`<допустимое>\n`), но не начальный LF и не обе формы с X; это поведение Python re.search. schema_check — тест/CI, не проверка пользовательского импорта. make_golden_zips.py использует только stdlib, не jsonschema; run_golden.py проверяет скомпилированный CLI. Инструменты в T-030 не исправлялись, замечание оставлено для управляющего.

Аудит golden: 79 ZIP, 74 разобранных манифеста; LF/CR в двух исправленных полях не найдено. Лог `.cache/t030/golden-newline-audit.json`. Исходники корпуса не менялись; окончательные golden_zip/golden_regen_check входят в полные прогоны.

### Вопросы

В-1…В-10 приняты. Исходная репродукция В-10 (`.cache/t030/trash-retry-probe.json`) создавала два ZIP одного ID после возврата источника к данным/mtime плана. Исправление проверяется тремя сценариями restore/purge/skip и отдельными тестами прямого create/copy, гонки после preflight и чужого/двойного ID.

Замечание по п. 5 В-10: существующий POSIX fallback перемещения использует link/unlink; сбой между вызовами может оставить две локации. Recovery намеренно не выбирает одну копию: отмечает журнал failed и сохраняет оба файла. Это существующая политика T-019, воспроизводимая тестом `Trash.RecoveryExplicitPresenceMatrixAndFailedRowsRemain` (presence=3, trash/restore, prepared/committed/indexed). `AssetRepository.ExplicitIdentityRejectsActiveTrashAndDualLocations` дополнительно воспроизводит отказ restore без изменения обеих копий. Правило восстановления не изменено; если требуется автоматическое разрешение такой неоднозначности, нужен отдельный вопрос/задача управляющего.

Для отдельной задачи управляющего остаются измеренные расхождения якорей прочих шаблонов MSVC и замечание к Python-тестам. Как предписано В-9, эти шаблоны и Python-инструменты попутно не исправляются в T-030. Таблица показывает, где V3 не обнаруживает новое отклонение.
