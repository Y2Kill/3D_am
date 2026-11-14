# T-016. Основа библиотеки: каталог, владелец записи, три базы и их версии (M2, P0.5 — часть 1)

**Статус:** ready · **Веха:** M2 · **Основание:** ADR-0007 (каталог и три базы SQLite), ADR-0031 пп. 1–3 (владелец записи, режим «только чтение», `StorageCapabilities`), ADR-0025 (`AssetRepository` — единственный писатель ZIP), `docs/specifications/versioning.md` строки 4–5 (версии баз и `am-library.toml`), ТЗ §14.5.10, §14.7.7, §14.7.11, критерий K-123; `docs/plan/roadmap.md` M2

## Цель

Дать слой «библиотека», на который лягут индексатор, `rebuild`/`sync`, корзина и `add-ready` (следующие задачи M2):

1. **Создание и открытие библиотеки** по раскладке ADR-0007 с файлом `am-library.toml`.
2. **Владелец записи** `.am/library.lock` (ADR-0031 п. 1): второй писатель получает `Errc::locked` с действием `open_read_only`.
3. **Режим «только чтение»** (ADR-0031 п. 2): открытие не меняет ни одного файла библиотеки.
4. **Три базы** `index.sqlite`, `userdata.sqlite`, `state.sqlite` из `schemas/sql/*.sql` с проверкой `PRAGMA user_version`, каркасом миграций (резервная копия `VACUUM INTO` перед миграцией) и пересозданием несовместимого индекса.
5. **CLI** `am library init` и `am library info`.

Индексатор и наполнение `index.sqlite` в задачу **не входят**: здесь индекс только создаётся по схеме и помечается как требующий перестройки.

## Решения управляющего (не пересматривать в задаче)

1. **Размещение.** Новый модуль `storage::library`: `include/am/storage/library/library.hpp`, `src/storage/library/*.cpp`. Базы — `src/storage/db/` (рядом с журналом фиксации). Уровень `storage`; `arch_check` не меняется. Фасад для CLI — `am_application` (как `asset_service`), отдельный заголовок `include/am/application/library_service.hpp`.
2. **Раскладка** (ADR-0007): `am-library.toml`, `assets/`, `trash/`, `.am/` с `index.sqlite`, `userdata.sqlite`, `state.sqlite`, `cache/`, `sessions/`, `.am-tmp/`, `locks/` (уже используется протоколом фиксации). Подкаталоги `assets/<xx>/` создаются по требованию писателем (как сейчас), не при `init`.
3. **`am-library.toml`** (toml++):
   ```toml
   format = "am-library"
   schema_version = 1
   library_id = "<UUIDv7>"
   name = "<строка UTF-8, может быть пустой>"
   created_at = "<UTC RFC 3339, миллисекунды, Z>"
   ```
   - Неизвестные ключи при чтении допускаются и не теряются: в этой задаче файл после `init` не переписывается, поэтому достаточно не отвергать их.
   - `format` ≠ `am-library`, отсутствие обязательного ключа, неверный тип или неверный UUIDv7 → `Errc::corrupt_data`, `am.library.config_invalid` (с ключом в `detail`).
   - `schema_version` > 1 → `Errc::unsupported`, `am.library.config_newer`, действие `open_read_only` не предлагается (содержимое не понято).
4. **`init(root, {name})`.**
   - `root` не существует — создаётся (родитель должен существовать); существует — должен быть **пустым** каталогом. Иначе `Errc::conflict`, `am.library.not_empty` (непустой каталог не превращается в библиотеку молча, §14.5.10).
   - Порядок: каталоги → три базы по схемам → `am-library.toml` **последним**, через временный файл в `.am/.am-tmp/` и переименование. Прерванный `init` оставляет каталог без `am-library.toml`; повторный `init` такого каталога отказывает по п. 4 — это допустимо и документируется в `cli.md` (удалить каталог и повторить).
   - После `init` библиотека **не** остаётся открытой.
5. **`open(root, {mode, application_version})` → `Library`.**
   - Нет `am-library.toml` → `Errc::not_found`, `am.library.not_found`.
   - `read_write`: проверка `StorageCapabilities` тем же путём, что в `AssetRepository` (`detect_storage_capabilities`; `read_only_default` → `Errc::read_only`, `am.library.storage_read_only`, действие `open_read_only`; нет права записи → `Errc::read_only`, `am.library.not_writable`). Неявного понижения до чтения нет: решение за вызывающим.
   - `read_write`: захват `.am/library.lock` **до** открытия баз, удержание всё время жизни `Library`, освобождение в деструкторе.
   - `Library` даёт доступ к `AssetRepository` (создаётся с `library_id` из toml, тем же `mode` и найденными возможностями) и к трём базам; второго писателя ZIP не появляется.
6. **Блокировка `.am/library.lock`.**
   - Windows — `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY)` на один байт по смещению `1 << 62` (за концом файла, чтобы содержимое оставалось читаемым другим процессам); Linux — `flock(LOCK_EX | LOCK_NB)` на отдельном дескрипторе.
   - Содержимое (после захвата, перезаписью): JSON одной строкой `{"pid":…, "host":"…", "application_version":"…", "acquired_at":"…"}`. Только диагностика: доверие — блокировке ОС, а не содержимому (ADR-0031 п. 1); устаревшее содержимое после аварии **не** мешает захвату.
   - Занято → `Errc::locked`, `am.library.locked`, действие `open_read_only`, в `detail` — содержимое файла, если читается.
   - Повторное `open(read_write)` той же библиотеки в **том же** процессе тоже получает `locked` (блокировка на дескриптор; это и проверяется тестом).
7. **`read_only`.**
   - Не создаёт, не изменяет и не удаляет **ни одного** файла в `root` (включая `-wal`/`-shm`, `library.lock`, `.am/.am-tmp`): базы открываются `SQLITE_OPEN_READONLY`, при невозможности открыть базу без записи — база считается недоступной (статус в `info`), а не открывается на запись.
   - Блокировку не берёт; `info` в режиме чтения сообщает состояние владельца пробным неблокирующим захватом с немедленным освобождением — это единственное допустимое действие с `library.lock`, файл при этом не создаётся, если его нет.
   - Изменяющие операции `AssetRepository` получают `Errc::read_only` (уже реализовано; проверить через `Library`).
8. **Базы и версии.** Схемы встраиваются при сборке из `schemas/sql/*.sql` (как `state_schema.hpp`). Поддерживаемая версия каждой базы — 1.
   - **`userdata` и `state`:** `user_version` = 0 и пустая база → создание по схеме; = 1 → открыть; < 1 при непустой базе или > 1 → для `read_write` `Errc::unsupported`, `am.library.db_version` (имя базы и версии в `detail`), действие `open_read_only` для версии > 1. Каркас миграций: упорядоченный список шагов `vN → vN+1` для каждой базы (для версии 1 пуст); перед первым шагом — `VACUUM INTO .am/backup/<db>-v<N>-<UTC>.sqlite`, после — проверка `PRAGMA integrity_check` и `user_version`; ошибка шага — откат транзакции, база остаётся в прежней версии, резервная копия не удаляется (§14.7.7). Каркас проверяется тестовым шагом, внедрённым через параметр (не через глобальное состояние).
   - **`index`:** не мигрирует (§14.7.11). Отсутствует, повреждён (`integrity_check` ≠ `ok`) или `user_version` ≠ 1 → в `read_write` файл (с `-wal`/`-shm`) переименовывается в `.am/index.sqlite.stale-<UTC>`, создаётся новый по схеме, в `meta` пишется `rebuild_required = 1`. В `read_only` — статус `missing`/`incompatible`, файлы не трогаются. Удаление старых `stale` — вне задачи.
   - Режимы журналов по ADR-0007: WAL; `synchronous=NORMAL` для `index`, `FULL` для `userdata` и `state`; `foreign_keys=ON`.
9. **Расхождение `state.sql` и журнала фиксации (находка управляющего).** `StateJournal::open` создаёт таблицу `pending_ops_new` на лету, а в `schemas/sql/state.sql` её нет. Перенести определение в `state.sql` (версия остаётся 1: данных пользователей версии 1 ещё нет), убрать создание из кода журнала; `sql_check` — расширить проверкой, что журнал и `Library` используют одну схему (например, что `state.sql` содержит все таблицы, к которым обращается `state_journal.cpp`). `StateJournal` и `Library` открывают один и тот же `.am/state.sqlite`; создание схемы — в одном месте.
10. **CLI.**
    - `am library init <dir> [--name <text>] [--json]` → код 0; непустой каталог / существующая библиотека → 4 (`am.library.not_empty`); ошибки ввода-вывода → 4.
    - `am library info <dir> [--json]` — открывает **только для чтения**: `library_id`, `name`, `created_at`, `schema_version`, для каждой базы `{state: ok|missing|incompatible|newer|unavailable, user_version}`, `rebuild_required` индекса, `owner: free|locked` (+ диагностическое содержимое при `locked`), `capabilities` (поля `StorageCapabilities`), `mode` и причина, если запись была бы невозможна. Код 0, если библиотека прочитана; не библиотека → 4 (`am.library.not_found`); повреждённый toml → 1 (`am.library.config_invalid`, это провал проверки, а не операции — как не-ZIP в T-013).
    - Новые `message_id` — в `resources/messages/en.toml` (полноту проверяет `message_catalog_check`); `docs/user/cli.md` — раздел `am library`; `CHANGELOG.md` — строки Added.
11. **Зависимости.** Новых нет: toml++ 3.4, sqlite3 — уже в `vcpkg.json`; блокировки — API ОС. Строку о `library.lock` (LockFileEx/flock, без библиотеки) добавляет управляющий в `docs/research/libraries.md` (сделано вместе с этой задачей).

## Тесты

Все новые тесты — с `RecordProperty("req", …)` по грамматике T-013 (К, ADR).

1. **init/open** (`req: K-7|ADR-0007`): раскладка, содержимое toml, три базы с `user_version = 1`, `rebuild_required = 1`; повторный `init` → `not_empty`; `init` в непустой каталог → `not_empty`; `open` каталога без toml → `not_found`; toml с неверным `format`, без `library_id`, с неверным UUID, `schema_version = 2` → ожидаемые коды; неизвестный ключ в toml не мешает открытию.
2. **Прерванный init** (`req: ADR-0007`): имитация сбоя перед записью toml (внедрение ошибки через параметр/шов теста) → каталог без `am-library.toml`, `open` → `not_found`.
3. **Владелец записи** (`req: K-123|ADR-0031`): два `open(read_write)` подряд в одном процессе → второй `locked` с `open_read_only` и диагностикой в `detail`; после уничтожения первого — второй успешен; `read_only` при занятой блокировке успешен; устаревшее содержимое `library.lock` (чужой PID) не мешает захвату.
4. **Только чтение не меняет файлы** (`req: ADR-0031|NFR-14.5.10` — если такого ID нет в матрице, то только `ADR-0031`): снимок `{путь, размер, mtime}` всех файлов `root` до и после `open(read_only)` + чтение + `info` — совпадает; нет новых файлов; изменяющая операция через `AssetRepository` → `read_only`.
5. **Версии баз** (`req: ADR-0007|ADR-0030`): `userdata`/`state` с `user_version = 2` → `read_write` отказ `db_version` с `open_read_only`, `read_only` — открывается, статус `newer`; `index` с `user_version = 2` или испорченным файлом → в `read_write` пересоздан, старый переименован в `stale-…`, `rebuild_required = 1`; удалённый `index.sqlite` → пересоздан, ассеты и `userdata` не затронуты.
6. **Каркас миграций** (`req: ADR-0007`): внедрённый шаг `userdata` v1→v2 → резервная копия в `.am/backup/`, `user_version = 2` после успеха; шаг с ошибкой → `user_version = 1`, данные целы, копия сохранена.
7. **`state.sql` и журнал** (`req: ADR-0006|ADR-0007`): существующие тесты протокола фиксации и репозитория проходят без изменений ожиданий; `StateJournal` на базе, созданной `Library`, работает.
8. **`AssetRepository` через `Library`** (`req: ADR-0025|ADR-0031`): создание ассета в открытой на запись библиотеке → ZIP в `assets/<xx>/`; тот же ассет читается через вторую `Library` в `read_only`.
9. **CLI** (`tests/cli`, Python, `req: K-7|K-123`): `init` + `info --json` (поля п. 10); `info` на пустом каталоге → 4; на испорченном toml → 1; `info` при блокировке, удерживаемой самим тестом через `msvcrt.locking`/`fcntl.flock` по тому же смещению `1 << 62`, → `owner: locked`. Вывод JSON — UTF-8, как в существующих CLI-тестах.

## Файлы (что можно менять)

- Новые: `include/am/storage/library/library.hpp`, `src/storage/library/*.cpp`, `include/am/application/library_service.hpp`, `src/application/common/library_service.cpp` (или по образцу `asset_service`), `tests/unit/test_library.cpp`, CLI-тесты в `tests/cli/`.
- Изменяемые: `src/storage/CMakeLists.txt` (встраивание трёх схем, новые исходники), `src/storage/db/state_journal.cpp` (только удаление создания `pending_ops_new` и общий источник схемы), `schemas/sql/state.sql` (только `pending_ops_new`), `tests/schema/sql_check.py` (проверка п. 9), `src/application/**/CMakeLists.txt`, `src/application/cli/main.cpp` (подкоманда `library`), `tests/CMakeLists.txt`, `resources/messages/en.toml`, `docs/user/cli.md`, `CHANGELOG.md`, этот файл (отчёт), строка T-016 в `docs/tasks/README.md` (статус).
- **Не менять:** формат 1.0 (схема манифеста, golden ZIP — опубликованы и неизменяемы), кодек, V1–V4, протокол фиксации (кроме п. 9), `AssetRepository` (кроме того, что нужно для создания через `Library`, — если понадобится изменение интерфейса, остановиться и описать), `index.sql` и `userdata.sql`, ADR, спецификации, матрицы, roadmap, `vcpkg.json`.

## Критерии приёмки

1. `tools\build_min.cmd --ci`, `tools\build_cli.cmd --ci`, `tools\build_cli.cmd --offline --ci` — PASS; `tools/build_linux.sh cli --ci` в WSL — PASS. Число тестов не уменьшилось; `req_report` — 0 неверных ID.
2. Тесты 1–9 выполнены; для каждого — имя теста в отчёте.
3. Отчёт перечисляет новые `message_id` и пример вывода `am library info --json`.
4. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-016`, без слияния, push и переписывания истории.

## Вне объёма

- Индексатор, наполнение `index.sqlite`, `rebuild`, `sync` (efsw), шина событий — T-017 и далее.
- Корзина, `add-ready`, конфликты ID, `IJobStore`, задания (ADR-0015).
- Статус `unavailable` при отключении носителя во время работы, IPC к владельцу, межбиблиотечное перемещение, резервное копирование (ADR-0030, P1).
- Генератор синтетических библиотек и замеры К-112.
- Удаление старых `index.sqlite.stale-*`.

## Отчёт исполнителя

_(заполняет исполнитель)_
