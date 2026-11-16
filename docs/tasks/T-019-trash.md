# T-019. Корзина библиотеки: `trash`, `restore`, `purge` (M2, P0.5 — часть 4)

**Статус:** ready · **Веха:** M2 · **Основание:** ТЗ v1.8 §10.8 п. 1–6 [СОГЛАСОВАНО]; ADR-0007 (раскладка `trash/<uuid>.zip`); ADR-0025 п. 1, 4, 6, 7 (единственный писатель, блокировки ассета, жизненный цикл, события после `indexed`); ADR-0006 (журнал `pending_ops`); `schemas/sql/state.sql` (`kind`: `trash | restore | purge`); T-017/T-018 (индекс, `in_trash`, `sync`)

## Цель

1. `AssetRepository` получает три операции: перемещение ZIP в корзину, восстановление из неё и окончательное удаление из корзины. Все три журналируются и восстанавливаются после сбоя.
2. После фиксации индекс обновляется через тот же наблюдатель `on_indexed`, что и для `create`/`change`.
3. CLI `am library trash|restore|purge`; `am library info` показывает размер корзины.

## Решения управляющего (не пересматривать в задаче)

1. **Корзина — это местоположение, а не состояние ассета.** `trash` и `restore` переименовывают файл между `assets/<xx>/<uuid>.zip` и `trash/<uuid>.zip`.
   - Байты ZIP не меняются.
   - `revision` не растёт, записи `history` нет.

   Основания:
   - в словаре `history.operation` формата 1.0 нет операции корзины, а формат не меняется;
   - корзина принадлежит библиотеке (§10.8 п. 1, 6): копия ZIP в другой библиотеке не должна нести признак удаления.
2. **Уровни (ADR-0024).**
   - `storage`: `AssetRepository::trash(const AssetRef&)`, `restore(const AssetRef&)`, `purge(const AssetRef&)` и их шаги в протоколе фиксации и журнале. Только этот код перемещает и удаляет ZIP (ADR-0025 п. 1).
   - `services`: `Catalog::trash/restore/purge` — под тем же mutex, возвращают результат и предупреждения, как `create`/`change`.
   - `application`: фасад; CLI — только через фасад.
3. **Блокировка и идентичность.**
   - Каждая операция берёт блокировку ассета `.am/locks/<uuid>.lock`, как фиксация.
   - Перед переименованием или удалением снимается `stat` источника (`base_size`, `base_mtime_ns` в `pending_ops`). Непосредственно перед действием `stat` проверяется повторно: несовпадение → `Errc::conflict`, файл не тронут (внешнее изменение, как в протоколе ADR-0006).
   - Читаемость манифеста **не требуется**: повреждённый ассет тоже можно убрать в корзину, вернуть и удалить. Идентичность определяет путь `<uuid>.zip` в своей библиотеке.
4. **Конфликты (§10.8 п. 2).** Если место назначения занято — `trash/<uuid>.zip` при `trash` или `assets/<xx>/<uuid>.zip` при `restore` — возвращается `Errc::conflict` (`am.repo.trash_conflict` / `am.repo.restore_conflict`). Ни один файл не заменяется и не удаляется. Переименование только без замены: `MoveFileExW` без `REPLACE_EXISTING` / `renameat2(RENAME_NOREPLACE)` или `link`+`unlink`, если `RENAME_NOREPLACE` недоступен; выбор описать в отчёте. Отсутствие источника → `Errc::not_found`.
5. **Журнал и восстановление** (`pending_ops.kind = trash | restore | purge`).
   - Состояния: `prepared → committed → indexed`, затем строка удаляется, как у `create`.
   - `target_path` — назначение (для `purge` — удаляемый файл), `temp_path` — источник (для `purge` — `NULL`).
   - `recover()` для `trash`/`restore`:
     - есть только источник → откат (строка удаляется);
     - есть только назначение → `committed`, затем событие и `indexed`;
     - есть оба или нет ни одного → `failed`, файлы не трогаются, строка остаётся для разбора; в `RecoveryItem` — причина.
   - `recover()` для `purge`:
     - файл есть → откат (окончательное удаление не довершается молча: пользователь подтверждал его в прошлом запуске);
     - файла нет → `committed` → `indexed`.
   - Схему `state.sql` не менять: имеющихся столбцов достаточно. Если нет — остановиться и описать.
6. **События и индекс.** `IndexedEvent.kind` — `trash`, `restore`, `purge`.
   - `Catalog`: `purge` → `Indexer::remove`. `trash`/`restore` → повторное чтение по **новому** месту (`in_trash` = 1 или 0). Сейчас `Indexer::update` строит путь только в `assets/`: дать ему местоположение из события.
   - Сбой индексации — как в T-017: файл остаётся перемещённым, предупреждение, `rebuild_required = 1`.
   - `list` по умолчанию скрывает корзину (уже так); `--trash` показывает.
7. **`userdata` не трогается ни одной из операций.** Связи и членство в коллекциях сохраняются (§10.8 п. 5). Судьба этих данных после `purge` — открытый §10.8 п. 7: здесь не решается, `purge` `userdata.sqlite` не открывает на запись.
8. **Изменение ассета в корзине** (`change` по `AssetRef`, ZIP которого в `trash/`) → `Errc::not_found`, как сейчас. Сначала `restore`. Поведение закрепить тестом.
9. **Размер корзины (§10.8 п. 4).** `am library info` добавляет `trash: {assets, bytes}` по индексу: число строк `in_trash = 1` и точная сумма `zip_size` в C++ `uint64` с контролем переполнения (переполнение → `null` + предупреждение). Не `SUM` в SQL (O-16). При `rebuild_required = 1` — `null` и предупреждение, как у `indexed_assets`. Автоочистки нет.
10. **CLI.**
    - `am library trash <dir> <asset-id>... [--json]`, `am library restore <dir> <asset-id>... [--json]`.
    - `am library purge <dir> (<asset-id>... | --all) --yes [--json]`. Без `--yes` — код 2 (подтверждение обязательно, §10.8 п. 3). `--all` — все `trash/*.zip` с корректным именем `<UUIDv4>.zip`, посторонние файлы не трогаются.
    - Каждый ID обрабатывается отдельно. JSON: `results[]` с `asset_id`, `status` (`done`/`failed`), `error`. Код 0 — все `done`; 4 — хотя бы один `failed` (конфликт, нет файла, блокировка, I/O).
    - Документация `docs/user/cli.md`, `CHANGELOG.md`.
11. **`message_id`:** `am.repo.trash_conflict`, `am.repo.restore_conflict`, `am.repo.not_in_trash`, `am.repo.asset_not_found` (если такого ещё нет — проверить каталог), `am.cli.purge_confirmation_required`; английский текст в `resources/messages/en.toml`.

## Тесты

1. **trash/restore** (`req: K-3`): ассет создан → `trash` → байты ZIP (SHA-256) совпадают, файл в `trash/`, в индексе `in_trash = 1`, `list` без `--trash` его не показывает → `restore` → обратно, `in_trash = 0`, `revision` прежняя.
2. **purge:** только из корзины. `purge` ассета в `assets/` → `am.repo.not_in_trash`. Из корзины → файла нет, строки индекса нет.
3. **Конфликты:** при `trash` уже есть `trash/<uuid>.zip` → `conflict`, оба файла байт-в-байт прежние. Аналогично для `restore`.
4. **Внешнее изменение:** `stat` источника изменён между подготовкой и действием (hook) → `conflict`, файл не тронут, строка журнала удалена.
5. **Повреждённый ассет:** ZIP с испорченным манифестом проходит `trash` → `restore` → `trash` → `purge`. Индекс помечает его так же, как `sync`/`rebuild`.
6. **Восстановление после сбоя:** для каждой операции и каждой точки (до и после переименования/удаления, до `indexed`) — внедрённый отказ или kill по образцу kill-тестов T-009, затем `recover()`. Исход каждой ветки п. 5 проверить явно, включая «оба файла есть» → `failed` без изменений.
7. **Индекс:** сбой наблюдателя при `trash` → файл перемещён, предупреждение, `rebuild_required = 1`. После `rebuild`/`sync` строка с `in_trash = 1`.
8. **`userdata`:** SHA-256 основного `userdata.sqlite` до и после `trash`/`restore`/`purge` совпадает (метод из В-1 T-017).
9. **Изменение в корзине:** `change` ассета в корзине → `not_found`, ZIP не тронут.
10. **Размер корзины:** `info` показывает `trash.assets` и точные `trash.bytes` (два ассета). Переполнение проверить на уровне функции суммы, без реальных файлов.
11. **CLI** (`req: K-3`):
    - init → создание двух ассетов → `trash` обоих → `info` (размер корзины) → `restore` одного;
    - `purge` без `--yes` → 2; `purge --all --yes` → 0, посторонний файл в `trash/` остался;
    - смешанный список ID с несуществующим → 4 и поэлементные результаты.

    Ограничения Linux — как у прежних CLI-сценариев записи, с явной причиной SKIP.

## Файлы (что можно менять)

- Изменяемые:
  - `include/am/storage/repository/repository.hpp`, `src/storage/repository/repository.cpp`;
  - `src/storage/commit/*`, `include/am/storage/commit/*`, `src/storage/db/state_journal.cpp` и его заголовок — только новые виды операций;
  - `include/am/storage/db/index_store.hpp`, `src/storage/db/index_store.cpp` (выборка для размера корзины);
  - `include/am/services/catalog/indexer.hpp`, `src/services/catalog/indexer.cpp`;
  - фасад `library_service`, `src/application/cli/main.cpp`;
  - тесты: новые `tests/unit/test_trash.cpp`, дополнения в `tests/cli/test_library_cli.py`, регистрация в `tests/CMakeLists.txt`;
  - `resources/messages/en.toml`, `docs/user/cli.md`, `CHANGELOG.md`;
  - этот файл (отчёт), строка T-019 в `docs/tasks/README.md`.
- **Не менять:**
  - `schemas/sql/*.sql`;
  - формат 1.0 (схема, golden ZIP), кодек, V1–V4;
  - поведение `create`/`change` и их точки отказа — существующие kill-тесты должны пройти без правок;
  - `Library` (кроме случая, если без изменения не обойтись — тогда остановиться);
  - ADR, спецификации, матрицы, roadmap, `vcpkg.json`.

## Критерии приёмки

1. `tools\build_min.cmd --ci`, `tools\build_cli.cmd --ci`, `tools\build_cli.cmd --offline --ci` — PASS; `tools/build_linux.sh cli --ci` в WSL — PASS. Число тестов не уменьшилось; `req_report` — 0 неверных ID; `arch_check` PASS.
2. Тесты 1–11 выполнены. В отчёте — имена тестов, пример JSON `trash`/`purge`/`info`, выбранный способ переименования без замены на Windows и Linux.
3. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-019`, без слияния, push и переписывания истории.

## Вне объёма

- Судьба связей `userdata` после `purge` (открытый §10.8 п. 7) и очистка «висячих» ссылок.
- Автоматическая очистка корзины (по ТЗ её нет), сроки хранения.
- Восстановление `pending_ops` при открытии библиотеки и в GUI: здесь только `recover()` и его тесты.
- `add-ready`, конфликты ID при импорте, независимая копия (ADR-0036 п. 4) — следующая задача.
- Межбиблиотечное перемещение (ADR-0025 п. 9, P1).

## Отчёт исполнителя

_(заполняет исполнитель)_
