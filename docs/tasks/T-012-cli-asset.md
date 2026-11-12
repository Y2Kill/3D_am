# T-012. CLI `am asset …` (M1) поверх фасада приложения

**Статус:** ready

## Цель

1. Дать первый пользовательский интерфейс к готовому ядру (кодек T-006, ZIP T-007/T-008, фиксация T-009, `AssetRepository` T-010, V3 T-011) — команды вехи M1 из `docs/plan/roadmap.md`:
   - `am asset create <files…> -o <out.zip>`;
   - `am asset inspect <zip>`;
   - `am asset validate <zip>`;
   - `am asset verify <zip> --level quick|selective|full`;
   - `am asset extract <zip> -d <dir>`.
2. Заложить **фасад приложения** `am_application` (ТЗ §12.4.4: GUI и CLI используют единый публичный API; бизнес-операции в интерфейсе не дублируются). CLI только разбирает аргументы и печатает результат.
3. Каталог сообщений (ADR-0032 п. 3, ADR-0034 п. 2): CLI переводит `message_id` в английский текст.
4. Первый пользовательский `CHANGELOG.md` и справка по командам на английском (ADR-0034 п. 4–5).
5. Закрыть замечание 2 приёмки T-011: находка `zip_paths` указывает виновную запись.

Команды библиотеки (`am library init|open|…`, `.am/library.lock`, `am add-ready`) — веха M2, **в задачу не входят**. Поэтому изменение существующего ассета из CLI (`set`, `add`, `remove`) тоже не открывается: правка «действующего» ZIP возможна только внутри библиотеки (ADR-0025), а библиотеки как пользовательского объекта ещё нет.

## Решения управляющего (не пересматривать в задаче)

1. **Слои и размещение.**
   - Фасад: `include/am/application/asset_service.hpp`, `src/application/common/*.cpp` (или `src/application/asset/`). `am_application` перестаёт быть INTERFACE-целью и получает исходники.
   - Фасад зависит от `storage`, `domain`, `foundation`, `contracts` — по матрице ADR-0024 это разрешено. CLI зависит только от `am::application` и `foundation`; прямые `#include` из `am/storage/…` в `src/application/cli/` запрещены (проверить `arch_check` или правилом в нём, если правило уровня каталога не выражается — отметить в отчёте).
   - Публичный заголовок фасада не включает Windows, sqlite, zlib, CLI11, toml++.

2. **Сборка.**
   - CLI11 2.7.2 и toml++ 3.4 — из `vcpkg.json` (уже там). В минимальной сборке без vcpkg их нет, поэтому **исполняемый `am` собирается только при `AM_WITH_CONTAINER=ON`**; в `win-debug-min` цель `am_cli` не создаётся, остальная минимальная сборка и её 84 теста не меняются.
   - Аргументы на Windows — `CLI::App::ensure_utf8(argv)`; ручной `CommandLineToArgvW` удаляется. Вывод в консоль — UTF-8 (`SetConsoleOutputCP(CP_UTF8)` при запуске; манифест исполняемого файла уже задаёт UTF-8 code page).
   - Отладочные команды M0 (`version`, `uuid`, `probe`, `volumes`, `zip-path`, `fix-name`, `exclude`) **сохраняются с прежними именами и аргументами**, переносятся на CLI11 и выводятся в справке отдельной группой `Development`.

3. **Адресация.** Все команды M1 работают с **отдельным ZIP-файлом** по пути, вне библиотеки. Для чтения и проверки фасад открывает файл напрямую. Проверка — **та же реализация**, что у `AssetRepository::verify`: вынести общую часть `Impl::check` в функцию хранилища, например

   ```cpp
   // storage::repository
   Result<VerificationReport> verify_container(const std::filesystem::path& zip, const ContainerVerifyOptions&);
   ```

   где `ContainerVerifyOptions` содержит уровень, выбранные FileID, лимиты ZIP/кодека, бюджет чтения и `SemanticValidator` (по умолчанию — полный валидатор, как у репозитория). Отличия от проверки в библиотеке: нет сверки `asset.id` с именем файла (имя отдельного файла произвольное), нет правила `history_operation_id`. Дублировать логику V1–V4 в фасаде запрещено.

4. **`am asset create`** — через единственного писателя (ADR-0025), без отдельного пути записи:
   1. Выходной файл не должен существовать — иначе `am.app.output_exists`, ничего не создаётся. Ключа перезаписи в задаче нет.
   2. Рядом с выходным файлом (тот же каталог, тот же том) создаётся служебная библиотека `.am-create-<uuid>/`; `AssetRepository::open` с `detect_storage_capabilities` этого каталога. Если ФС даёт `read_only_default` (съёмный носитель, сеть, не NTFS, не Windows — `fs_support.md` v0.2) — отказ `am.repo.storage_read_only`, служебный каталог удаляется.
   3. `create()` с одним `AddFiles`: один вход для всех переданных файлов, `origin_kind = user_added`; путь внутри входа — имя файла, приведённое существующим отображением путей ADR-0012 (`foundation/paths`, то же, что у `am zip-path`). Коллизия имён — `am.repo.path_collision` до чтения файлов (проверка T-011). Каталоги в аргументах отклоняются (`am.app.directory_not_supported`; рекурсивный ввод — импорт M3a).
   4. Необязательные метаданные: `--title <text>` → `asset.metadata.title`. Других ключей метаданных в задаче нет.
   5. Готовый ZIP переносится из служебной библиотеки в выходной путь **без замены**: Windows — `MoveFileExW` без `MOVEFILE_REPLACE_EXISTING`, с `MOVEFILE_WRITE_THROUGH`. Если цель появилась за это время — `am.app.output_exists`, служебный ZIP удаляется.
   6. После переноса — `verify_container(..., full)`; провал — выходной файл удаляется (он создан этой командой), код выхода 1.
   7. Служебный каталог удаляется при любом исходе. Если процесс убит, каталог `.am-create-*` остаётся; автоматическая очистка — вне задачи, поведение описать в справке.
   8. Вывод: `asset_id`, ревизия, число файлов, путь; с `--json` — объект (п. 7).

5. **`am asset inspect`** — сводка без проверки потоков: версия формата и `format_support`, `asset.id`, ревизия, `created_at`/`modified_at`, заголовок, число файлов, исторических файлов, входов, записей истории; список текущих файлов (`id`, путь, размер, `origin_kind`). V1 и V2 выполняются (без них манифест не читается), их статус выводится. Новые минорные версии читаются (`newer_minor`), как у снимка T-010.

6. **`am asset validate`** — V1–V3 без чтения потоков: `verify_container` с уровнем, при котором V4 не выполняется (добавить такой режим в `ContainerVerifyOptions`, например флаг `streams = false`; `VerifyLevel` не расширять). **`am asset verify`** — V1–V4 с уровнем `quick|selective|full` (по умолчанию `full`); `selective` требует `--file <FileID>` (повторяемый).

7. **Вывод.**
   - stdout — результат, stderr — ошибки и предупреждения.
   - `--json` у всех пяти команд: один JSON-объект в UTF-8, без лишних строк. Для `validate`/`verify` — `verification_json()` (T-010/T-011) как есть, плюс поле `"command"`. Ошибка операции при `--json` — объект `{"command":…,"error":{"code":errc_name,"message_id":…,"detail":…}}` в stdout и тот же код выхода.
   - Текстовый вывод: строки из каталога сообщений по `message_id`; `detail` и цепочка `cause` — только с `--verbose`. Находки V3 печатаются как `<severity> <rule> <pointer>: <текст каталога>`.
   - Абсолютные пути пользователя выводятся только в `detail` и в собственных строках команды (путь результата `create`, `extract`).

8. **Коды выхода** (стабильны, документируются в справке):

   | Код | Значение |
   |---|---|
   | 0 | успех; для проверок — `passed` на всех выполненных уровнях |
   | 1 | проверка не пройдена: какой-либо уровень `failed`, несовпадение хеша при `extract`, провал проверки после `create` |
   | 2 | ошибка использования: неизвестная команда, неверные аргументы (CLI11) |
   | 3 | проверка прошла частично: нет `failed`, но есть `partial` (находки `unverifiable`, `verified_crc_size_only`, `newer_minor` без V3) |
   | 4 | операция не выполнена: файл не найден или не читается, ZIP не открывается, блокировка, нехватка места, выход существует |

9. **`am asset extract <zip> -d <dir> [--file <FileID>]… [--manifest]`.**
   - Целевой каталог не существует или пуст — иначе `am.app.target_not_empty`. Создаётся командой.
   - Извлекаются текущие `files[]` (по умолчанию все, иначе выбранные FileID; неизвестный FileID — `am.app.unknown_file_id`, код 4, до записи). `--manifest` дополнительно пишет `manifest.json` как есть (байты из ZIP).
   - Путь на диске — `storage_path` записи относительно `<dir>`; пути уже проверены V3 (`storage_paths`), но перед записью — повторно `paths::check_container_path`, и итоговый путь обязан оставаться внутри `<dir>` после нормализации. Длинные пути — через `paths::native`.
   - Перед извлечением выполняется V1–V3 (как `validate`); при `failed` извлечение не начинается, код 1.
   - Каждый поток читается через `ZipArchive::open_entry(..., sha256 = true)` и пишется во временный файл в том же каталоге; после проверки размера и SHA-256 с манифестом — переименование в итоговое имя без замены. Несовпадение — временный файл удаляется, остальные уже извлечённые файлы остаются, код 1, отчёт перечисляет неудачные FileID.
   - Исторические файлы, вложения расширений и непрозрачные записи не извлекаются.

10. **Каталог сообщений.**
    - Ресурс `resources/messages/en.toml`: ключ — `message_id`, значение — английский текст для пользователя. Подстановок в тексте нет: текст статичен, переменная часть (`pointer`, путь, `detail`) печатается отдельно (п. 7).
    - Встраивается в исполняемый файл при сборке (CMake читает файл и генерирует строковый литерал/`#embed`-заменитель); поиск файла при запуске не нужен.
    - Разбор — toml++ внутри `am_application`; публичный API фасада: `std::string_view message_text(std::string_view message_id)` с откатом к `Error::message`, затем к самому `message_id`.
    - Полнота: каталог содержит **все** `message_id`, которые могут дойти до пользователя через пять команд: все `am.manifest.<rule>` каталога V3 (проверка — по `SemanticReport::checked_rules` и `deferred_rules` полного прогона эталона), `am.manifest.*` V1/V2/V4 и кодека, `am.repo.*`, `am.zip.*`, `am.commit.*`, `am.app.*`. Дополнительно ctest `message_catalog_check` (Python, по образцу `schema_check`) ищет строковые литералы `"am.[a-z0-9_.]+"` в `src/` и сверяет с каталогом; литералы, которые пользователю заведомо не показываются, перечисляются в явном списке исключений в самом тесте с причиной.

11. **Замечание 2 приёмки T-011.** Находка `zip_paths` в `Invariants` получает `detail` с именем записи ZIP (как есть, UTF-8); `pointer` остаётся пустым — записи ZIP не адресуются JSON Pointer.

12. **Документация (английский).**
    - `docs/user/cli.md` — команды M1, ключи, коды выхода, формат `--json`, поведение при прерывании `create` (каталог `.am-create-*`).
    - `CHANGELOG.md` в корне (Keep a Changelog), раздел `Unreleased` с командами M1.
    - Справка `am --help` и `am asset <cmd> --help` согласована с `cli.md`.

## Контекст (что прочитать)

- `docs/plan/roadmap.md`, M1 (раздел CLI) и M2 (что отложено).
- `include/am/storage/repository/repository.hpp` и `src/storage/repository/repository.cpp` (`check`, `verify`, `create`, `guard`, `Invariants`).
- `include/am/storage/asset_zip/reader.hpp` (`ZipArchive`, `open_entry`, `ReadBudget`).
- `include/am/foundation/paths.hpp` (отображение путей, `check_container_path`, `native`), `include/am/foundation/error.hpp` (`errc_name`, `message_id`, `cause`).
- ADR-0024 (уровни), ADR-0025 (единственный писатель), ADR-0032, ADR-0034; `docs/specifications/fs_support.md` v0.2.
- `src/application/cli/main.cpp` — текущие отладочные команды.

## Тесты

Интеграционные тесты запускают собранный `am` как процесс (ctest, путь через `$<TARGET_FILE:am_cli>`), рабочие каталоги — в `.cache/cli-tests/<test>/`, входные файлы генерируются тестом. Реальная коллекция не используется.

1. **create:** два файла → ZIP; `verify --level full` код 0; `inspect --json` показывает два файла с верными размерами и `origin_kind = user_added`; `--title` попадает в манифест. Имена с кириллицей и пробелами (аргументы и содержимое) — round-trip через `extract`.
2. **create, отказы:** выход существует (код 4, файл не изменён); коллизия имён `A.txt`/`a.txt` (код 4, `am.repo.path_collision`, выход не создан); каталог в аргументах; несуществующий файл; после каждого отказа нет `.am-create-*`.
3. **create, перенос:** подстановкой (тестовый хук фасада) цель появляется между фиксацией и переносом → `am.app.output_exists`, чужой файл не изменён, служебный каталог удалён.
4. **validate/verify:** эталонный ассет — 0; V3 `error` (ZIP с манифестом, нарушающим правило каталога, собранный тестовым писателем) — 1 и находка в тексте и JSON; только `unverifiable` — 3; повреждённый поток — `validate` 0, `verify full` 1; `quick`/`selective` с `--file`; `selective` без `--file` — 2.
5. **inspect:** манифест новой минорной версии 1.1 — код 3, `format_support = newer_minor`, V3 `not_performed`.
6. **extract:** все файлы и выбранный FileID; непустой каталог — 4; неизвестный FileID — 4 без записи; поток с испорченным содержимым при верном CRC невозможен, поэтому несовпадение SHA-256 — ZIP, где манифест указывает чужой хеш (V1–V3 проходят, V4 не выполнялась) → код 1, файл не появился, остальные извлечены; `--manifest` даёт байты, равные записи в ZIP.
7. **JSON:** вывод каждой команды с `--json` разбирается строгим JSON-разборщиком и содержит `command`; ошибка — объект `error` с `code`, `message_id`.
8. **Коды выхода 0–4** — каждый получен хотя бы одним тестом выше; сводная таблица «код → тест» в отчёте.
9. **Каталог сообщений:** модульный тест полноты по прогону V3 эталона; `message_catalog_check` PASS; отсутствующий ключ — откат к `message`.
10. **Отладочные команды:** `am version`, `am uuid --v4 2`, `am zip-path` дают прежний вывод.
11. **`zip_paths`:** ZIP с недопустимым именем записи → находка с именем в `detail`.
12. **Фасад без CLI:** модульные тесты `asset_service` (create/inspect/validate/verify/extract) в `am_unit_tests` — те же сценарии 1, 4, 6 на уровне API, чтобы будущий GUI получал проверенное поведение.

Пометки `RecordProperty("req", …)`: `K-15`, `K-85` (единое ядро для CLI), `K-3`, `K-6` (проверка формата), по смыслу теста.

## Файлы (что можно менять)

- Новые: `include/am/application/*.hpp`, `src/application/common/*.cpp` (или `src/application/asset/*.cpp`), `resources/messages/en.toml`, `tests/unit/test_asset_service.cpp`, `tests/cli/*` (интеграционные тесты CLI, скрипт или C++), `tests/tools/message_catalog_check.py` (или рядом с `schema_check`), `docs/user/cli.md`, `CHANGELOG.md`.
- Изменяемые: `src/application/CMakeLists.txt`, `src/application/cli/CMakeLists.txt`, `src/application/cli/main.cpp`, `include/am/storage/repository/repository.hpp`, `src/storage/repository/repository.cpp` (вынос `verify_container`, `zip_paths`), `tests/unit/test_asset_repository.cpp` (только новые тесты `verify_container`), `tests/CMakeLists.txt`, `tools/arch_check` (только правило «CLI не включает storage», если нужно), этот файл (отчёт), строка T-012 в `docs/tasks/README.md` (статус).
- **Не менять:** схему, эталон, спецификацию, ADR, `fs_support.md`, `libraries.md`, матрицы, roadmap, кодек, код V3 (кроме `zip_paths` в `Invariants`), протокол фиксации, `vcpkg.json` (новых зависимостей нет).

## Критерии приёмки

1. `tools\build_min.cmd` — PASS, 84 теста, без новых зависимостей; цель `am_cli` в минимальной сборке не создаётся.
2. `tools\build_cli.cmd` и `tools\build_cli.cmd --offline` — PASS; все 262 прежних теста сохранены, ожидания не ослаблены.
3. Тесты 1–12 выполнены; коды выхода 0–4 покрыты.
4. `arch_check` PASS; CLI не включает заголовки `storage`; публичные заголовки фасада без Windows/sqlite/zlib/CLI11/toml++.
5. В отчёте:
   - таблица «команда → тесты» и «код выхода → тест»;
   - реальные примеры текстового и JSON-вывода всех пяти команд (с путями пользователя, заменёнными на `<dir>`);
   - перечень исключений `message_catalog_check` с причинами;
   - где проходит граница фасада и CLI (какие функции фасада вызывает каждая команда).
6. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-012`, без слияния и переписывания истории.

## Вне объёма

- `am library …`, `.am/library.lock`, `am add-ready`, индекс — M2.
- Изменение существующего ассета из CLI (`set`/`add`/`remove`/`replace`) — после M2 (библиотека как пользовательский объект).
- Рекурсивный ввод каталогов, профили, правила исключения, `inputs[].objects/entries` — импорт M3a; экспорт (`am export`) — M3a.
- Перезапись выходного файла, автоматическая очистка оставшихся `.am-create-*`.
- Локализации кроме английской; журналы spdlog (M3a).
- Linux: на не-Windows `create` отказывает по `fs_support.md` (ФС не проверена); чтение, проверка и извлечение должны собираться и работать, но CI Linux — задача T-005.

## Отчёт исполнителя

**2026-11-12. Ветка:** `codex/T-012`, создана от `main` после приёмки T-011. Реализация и тесты — `48c6c91`, `3d5a5e0`; отчёт и статус — отдельный коммит `T-012: report CLI validation and hand off for review`.

### Выполнение критериев приёмки

1. **Минимальная сборка PASS, 84/84.** Новых зависимостей нет. В выводе `cmake --build --preset win-debug-min --target help` цели `am_cli` нет. При `AM_WITH_CONTAINER=OFF` фасадные исходники и CLI не подключаются, `am_application` остаётся прежней INTERFACE-целью; в контейнерной сборке это STATIC-библиотека с фасадом.
2. **CLI и offline PASS, 278/278 каждый.** Все 262 прежних теста сохранены; их имена сверены с полным логом T-011. Старые проверки/ожидания не ослаблены, в `test_asset_repository.cpp` добавлены только два новых теста. CLI11 2.7.2 и toml++ 3.4 взяты из существующего vcpkg, манифест зависимостей не изменён.
3. Тесты 1–12 выполнены: процессные сценарии CLI, прямые вызовы фасада без CLI, гонка публикации и отказ после публикации, UTF-8/кириллица/пробелы, строгий JSON, коды 0–4, режимы проверки, SHA-256 при извлечении, новый minor, каталог сообщений и прежние диагностические команды. Таблицы ниже.
4. **`arch_check` PASS.** CLI содержит только проектные заголовки `application`; бизнес-операции — в фасаде, общий V1–V4 — в storage. Добавлено точечное правило запрета `am/storage/…` в `src/application/cli/`. Публичные заголовки фасада без Windows/sqlite/zlib/CLI11/toml++.
5. Примеры вывода, исключения каталога и граница фасада приведены ниже; `docs/user/cli.md` и `CHANGELOG.md` написаны на английском. Справка CLI11 содержит команды M1, `Development`, коды выхода и поведение прерванного create.
6. Изменены только разрешённые файлы. Коммиты — в `codex/T-012`, без push, слияния и переписывания истории. Статус строки T-012 — `review`.

### Граница фасада и интерфейса

Публичный `asset_service.hpp` предоставляет запросы, `AssetOutcome` (структурированные JSON-данные, статус, диагностика), `ExitCode` и `Result`-функции. CLI переводит UTF-8-аргументы в запрос, вызывает фасад, форматирует stdout/stderr; сам не открывает ZIP и не выполняет операции с файлами.

| Команда | Функция фасада | Реализация операции |
|---|---|---|
| create | `create_asset(CreateAssetOptions)` | Маппер foundation, один AddFiles, служебная библиотека на том же томе, `AssetRepository::create`, перенос без замены, общий full verify, очистка |
| inspect | `inspect_asset(path)` | Общий `verify_container` с `streams=false, semantics=false`, сводка из прочитанного документа |
| validate | `validate_asset(path)` | Общий `verify_container`, `streams=false` |
| verify | `verify_asset(path, VerifyAssetOptions)` | Общий `verify_container`, quick/selective/full |
| extract | `extract_asset(path, ExtractAssetOptions)` | Общий V1–V3, разрешение FileID до записи, безопасные пути, временные файлы, CRC/size/SHA-256, публикация без замены |

`verify_container` и `AssetRepository::Impl::check` используют один `check_container`; логика V1–V4 не копировалась в application. Необязательный `ContainerContents` сохраняет тот же открытый ZIP, документ, исходные байты манифеста и общий бюджет для последующего inspect/extract. Отдельное имя ZIP не сверяется с AssetID, правило последней операции не вызывается. Библиотечные проверки и фиксация сохраняют свой контекст.

Старые команды вызывают `development_command`, поэтому CLI не включает storage/processing/services. Механизм сообщений разбирает встроенный TOML один раз; `message_text(Error)` использует каталог, затем `Error::message`, затем сам ID. Прямая зависимость объекта `messages.cpp` от TOML/генерируемого ресурса обеспечивает пересборку после правки каталога.

`zip_paths.detail` содержит исходное имя без изменений для корректного UTF-8, pointer пуст. Для недопустимых байтов имени используется диагностическое экранирование `\xHH`; тот же способ применяется к диагностическому пути V4, чтобы JSON повреждённого ZIP оставался корректным. Байты самого ZIP и пути для чтения не меняются.

### Команды и тесты

| Команда/требование | Процессные тесты | Прямой API / storage |
|---|---|---|
| create, inspect, полный verify, Unicode round-trip | `cli_create_roundtrip` | `AssetService.CreateInspectValidateVerifyExtractRoundTrip` |
| create: существующий выход, коллизия A/a, каталог, отсутствующий файл, очистка | `cli_create_refusals` | `CreateRefusalsAndPublishRaceNeverReplaceOutput` |
| create: цель появилась перед переносом; провал после переноса | — | `CreateRefusalsAndPublishRaceNeverReplaceOutput`, `PostCreateFailureDeletesOnlyTheNewOutput` |
| validate/verify: passed, error, unverifiable, повреждённые потоки | `cli_validate_verify` | `ValidationAndExtractionUseSharedV3AndPreserveSuccessfulFiles`, `StreamScopesMinorAndCorruption` |
| quick/selective, повтор `--file`, отсутствие FileID | `cli_validate_verify` | `StreamScopesMinorAndCorruption`, проверка API usage в `ValidationAndExtractionUseSharedV3AndPreserveSuccessfulFiles` |
| inspect новой minor 1.1 и код 1 для провала V1/CRC манифеста | `cli_inspect_minor` | `StreamScopesMinorAndCorruption` |
| extract: все/выбранные, manifest byte-for-byte, непустой каталог, неизвестный ID, несовпадение SHA | `cli_create_roundtrip`, `cli_extract` | `CreateInspectValidateVerifyExtractRoundTrip`, `ValidationAndExtractionUseSharedV3AndPreserveSuccessfulFiles`, `StreamScopesMinorAndCorruption` |
| один строгий JSON-объект, command, error.code/message_id; текстовые примеры | `cli_json_examples`, остальные CLI-тесты с `json_output=True` | `error_json`, `AssetOutcome` |
| каталог и fallback | `message_catalog_check` | `MessageCatalogCoversAllCheckedAndDeferredRulesAndFallback` |
| development version/uuid/zip-path/help | `cli_development` | `development_command` |
| общий verify, произвольное имя, V1 failure, zip_paths.detail | `cli_validate_verify` | `AssetRepository.VerifyContainerSharesReportsAndAcceptsArbitraryFilename`, `VerifyContainerReturnsV1FailureAndZipPathDetail` |

| Код выхода | Фактически получен в тестах |
|---|---|
| 0 | `cli_create_roundtrip`; validate/verify валидного ZIP; selective со всеми FileID; успешный extract |
| 1 | `cli_validate_verify` (license_spdx, повреждённый поток, недопустимое ZIP-имя); `cli_extract` (SHA mismatch); API post-create failure |
| 2 | `cli_validate_verify` (selective без FileID); `cli_development` (неизвестная команда); API selective без FileID |
| 3 | `cli_validate_verify` (unverifiable/quick/частичная выборка); `cli_inspect_minor` |
| 4 | `cli_create_refusals`; `cli_extract` (непустой каталог/неизвестный ID/неподдерживаемый метод потока, верхнеуровневый error в JSON); `cli_json_examples` (отсутствующий ZIP) |

### Полные команды и фактический вывод

```text
tools\build_min.cmd
100% tests passed, 0 tests failed out of 84
Total Test time (real) =  18.79 sec
```

```text
tools\build_cli.cmd
100% tests passed, 0 tests failed out of 278
Total Test time (real) = 912.58 sec
```

```text
tools\build_cli.cmd --offline
100% tests passed, 0 tests failed out of 278
Total Test time (real) = 632.61 sec
```

Логи: `.cache/t012-final-{min,cli,offline}.log`; полный stdout CTest — `.cache/t012-final-{cli,offline}-tests.log`; список целей минимума — `.cache/t012-min-targets.log`. Итоговые сборки без новых предупреждений MSVC/CMake. Снимки SHA-256 кэшей до/после offline: 81 файл, изменённых — 0. Offline — read-only-кэши и `x-block-origin`; зависимости не скачивались.

При отладке прямой `ctest` вне среды MSVC запустил реконфигурацию из `arch_check` с неверным окружением и сбил CMake-кэш. Восстановлен только `build/win-cli` командой `cmake --fresh --preset win-cli` из `.cache/t012_reconfigure.cmd` в штатной среде MSVC/vcpkg. Затем `tools\build_cli.cmd --offline -R arch_check` — PASS; полные итоговые прогоны выполнены штатными скриптами. Исходники, пресеты и инструментальная конфигурация проекта ради этого не менялись.

### Исключения message_catalog_check

Каталог содержит 159 непустых английских сообщений. Все полные строковые литералы ID из `src/` включены. Дополнительная проверка по эталону покрывает все `checked_rules` и `deferred_rules` V3. Исключены только точные префиксы, которые сами не являются пользовательскими message_id:

| Литерал | Причина |
|---|---|
| `am.app.label.` | Префикс составных меток представления; полные ключи находятся в каталоге |
| `am.commit.` | Классификатор семейства ошибок в guard репозитория |
| `am.json.` | Классификатор ошибок строгого JSON в кодеке |
| `am.manifest.` | Префикс составных V3 ID / классификатор; составные правила проверяет модульный тест |
| `am.zip.` | Классификатор семейства ошибок в guard репозитория |

Исключения и причины закреплены явно в `tests/tools/message_catalog_check.py`.

### Реальные примеры вывода

Примеры получены из итогового `cli_json_examples`; абсолютный тестовый каталог заменён на `<dir>`, относительные пути контейнера и пользовательские имена сохранены. Для каждого вызова JSON — один объект stdout; предупреждения/ошибки выводятся отдельно в stderr.

**create**

```text
Asset ID: 3b8368c8-5238-461b-a01b-f79441febc96
Revision: 1
Files: 2
Path: <dir>\example-text.zip
```

```json
{"asset_id": "9441ad08-e5ee-4b5e-b77f-b657da49d61c", "revision": 1, "file_count": 2, "path": "<dir>\\example-json.zip", "command": "create"}
```

**inspect**

```text
v1: passed
v2: passed
Asset ID: b4f182d9-62d2-4ed2-bca0-206e3e7739f2
Revision: 1
Format support: supported
Created at: 2026-11-12T16:07:43.835Z
Modified at: 2026-11-12T16:07:43.835Z
Title: Тестовый asset
Files: 2
Historical files: 0
Inputs: 1
History entries: 1
Format: 1.0
eec13790-4a63-43ce-bea6-72fabf3b7898 content/9972b880-8edb-4ce2-9da9-d0c7401906e5/деталь с пробелом.txt 40 user_added
cfc6ec4c-b865-4b4d-9a71-eb89ff199341 content/9972b880-8edb-4ce2-9da9-d0c7401906e5/second.bin 4 user_added
```

```json
{"format": {"id": "urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128", "version": {"major": 1, "minor": 0}}, "format_support": "supported", "v1": "passed", "v2": "passed", "v3": "not_performed", "asset_id": "b4f182d9-62d2-4ed2-bca0-206e3e7739f2", "revision": 1, "created_at": "2026-11-12T16:07:43.835Z", "modified_at": "2026-11-12T16:07:43.835Z", "title": "Тестовый asset", "file_count": 2, "historical_file_count": 0, "input_count": 1, "history_entry_count": 1, "files": [{"id": "eec13790-4a63-43ce-bea6-72fabf3b7898", "path": "content/9972b880-8edb-4ce2-9da9-d0c7401906e5/деталь с пробелом.txt", "size_bytes": "40", "origin_kind": "user_added"}, {"id": "cfc6ec4c-b865-4b4d-9a71-eb89ff199341", "path": "content/9972b880-8edb-4ce2-9da9-d0c7401906e5/second.bin", "size_bytes": "4", "origin_kind": "user_added"}], "command": "inspect"}
```

**validate**

```text
v1: passed
v2: passed
v3: passed
Container verification: partial
```

```json
{"format_support": "supported", "v1": "passed", "v2": "passed", "v3": "passed", "v3_rules": ["asset_times", "byte_sizes_uint64", "claim_evidence", "claim_target_custom", "claim_target_item", "claim_target_standard", "claim_value_type", "claim_value_unit_unexpressible", "custom_value_field", "dependency_refs", "derived_from", "field_constraints", "field_default", "field_options", "group_refs", "historical_files", "history_compacted_summary", "history_coverage", "history_operation_id_unique", "input_entry_file", "input_entry_object", "input_nesting", "input_source_refs", "license_file_ref", "license_spdx", "preview_refs", "storage_paths", "timestamps", "unique_ids", "urls", "value_constraints", "value_dependencies_scope", "value_file_ref", "value_options", "value_representation", "value_unit_required", "x02_physical_default_open", "x02_unit_conversion_open", "zip_paths"], "v3_complete": true, "v3_findings": [], "v3_deferred_rules": [{"rule": "tags_search_key", "reason": "O-08"}], "plugin_validation": "not_performed", "container_verification": "partial", "v4": [], "command": "validate"}
```

**verify**

```text
v1: passed
v2: passed
v3: passed
Container verification: full
```

```json
{"format_support": "supported", "v1": "passed", "v2": "passed", "v3": "passed", "v3_rules": ["asset_times", "byte_sizes_uint64", "claim_evidence", "claim_target_custom", "claim_target_item", "claim_target_standard", "claim_value_type", "claim_value_unit_unexpressible", "custom_value_field", "dependency_refs", "derived_from", "field_constraints", "field_default", "field_options", "group_refs", "historical_files", "history_compacted_summary", "history_coverage", "history_operation_id_unique", "input_entry_file", "input_entry_object", "input_nesting", "input_source_refs", "license_file_ref", "license_spdx", "preview_refs", "storage_paths", "timestamps", "unique_ids", "urls", "value_constraints", "value_dependencies_scope", "value_file_ref", "value_options", "value_representation", "value_unit_required", "x02_physical_default_open", "x02_unit_conversion_open", "zip_paths"], "v3_complete": true, "v3_findings": [], "v3_deferred_rules": [{"rule": "tags_search_key", "reason": "O-08"}], "plugin_validation": "not_performed", "container_verification": "full", "v4": [{"id": "eec13790-4a63-43ce-bea6-72fabf3b7898", "path": "content/9972b880-8edb-4ce2-9da9-d0c7401906e5/деталь с пробелом.txt", "status": "verified_sha256"}, {"id": "cfc6ec4c-b865-4b4d-9a71-eb89ff199341", "path": "content/9972b880-8edb-4ce2-9da9-d0c7401906e5/second.bin", "status": "verified_sha256"}], "command": "verify"}
```

**extract**

```text
Files: 2
Path: <dir>\extract-text
eec13790-4a63-43ce-bea6-72fabf3b7898 content/9972b880-8edb-4ce2-9da9-d0c7401906e5/деталь с пробелом.txt
cfc6ec4c-b865-4b4d-9a71-eb89ff199341 content/9972b880-8edb-4ce2-9da9-d0c7401906e5/second.bin
```

```json
{"path": "<dir>\\extract-json", "file_count": 2, "extracted": [{"id": "eec13790-4a63-43ce-bea6-72fabf3b7898", "path": "content/9972b880-8edb-4ce2-9da9-d0c7401906e5/деталь с пробелом.txt"}, {"id": "cfc6ec4c-b865-4b4d-9a71-eb89ff199341", "path": "content/9972b880-8edb-4ce2-9da9-d0c7401906e5/second.bin"}], "failed": [], "manifest_written": false, "verification": {"format_support": "supported", "v1": "passed", "v2": "passed", "v3": "passed", "v3_rules": ["asset_times", "byte_sizes_uint64", "claim_evidence", "claim_target_custom", "claim_target_item", "claim_target_standard", "claim_value_type", "claim_value_unit_unexpressible", "custom_value_field", "dependency_refs", "derived_from", "field_constraints", "field_default", "field_options", "group_refs", "historical_files", "history_compacted_summary", "history_coverage", "history_operation_id_unique", "input_entry_file", "input_entry_object", "input_nesting", "input_source_refs", "license_file_ref", "license_spdx", "preview_refs", "storage_paths", "timestamps", "unique_ids", "urls", "value_constraints", "value_dependencies_scope", "value_file_ref", "value_options", "value_representation", "value_unit_required", "x02_physical_default_open", "x02_unit_conversion_open", "zip_paths"], "v3_complete": true, "v3_findings": [], "v3_deferred_rules": [{"rule": "tags_search_key", "reason": "O-08"}], "plugin_validation": "not_performed", "container_verification": "partial", "v4": []}, "command": "extract"}
```

### Отклонения и вопросы

Отклонений от объёма задачи нет. `ContainerContents`, отключение семантики только для inspect и тестовый хук после публикации — детали API для повторного использования проверенного чтения и проверки очистки при отказе; бизнес-путь записи остаётся единственным — через AssetRepository. Команды библиотеки, изменение существующего ZIP, импорт каталогов, новые зависимости и автоматическая очистка после kill не добавлены.

Новых вопросов нет. Все изменения задачи закоммичены. Существовавший до начала работы неотслеживаемый `3D_am_open_questions_e2472e9.zip` не изменялся и в коммиты не включён.
