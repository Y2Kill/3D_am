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

*(заполняет исполнитель)*
