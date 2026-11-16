# T-019. Корзина библиотеки: `trash`, `restore`, `purge` (M2, P0.5 — часть 4)

**Статус:** done (принята 2026-11-16, `6a7f96d`) · **Веха:** M2 · **Основание:** ТЗ v1.8 §10.8 п. 1–6 [СОГЛАСОВАНО]; ADR-0007 (раскладка `trash/<uuid>.zip`); ADR-0025 п. 1, 4, 6, 7 (единственный писатель, блокировки ассета, жизненный цикл, события после `indexed`); ADR-0006 (журнал `pending_ops`); `schemas/sql/state.sql` (`kind`: `trash | restore | purge`); T-017/T-018 (индекс, `in_trash`, `sync`)

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

### Ветка и коммиты

- Ветка `codex/T-019`, создана от `main` (`eea91e5`).
- `b54bf83` — операции корзины, журнал и восстановление, наблюдатель индекса, размер корзины, CLI, тесты и пользовательская документация.
- `Document T-019 execution and validation` — отдельный коммит этого отчёта и перевода в `review`. Слияние, push и переписывание истории не выполнялись.

### Выполнение критериев

1. Все четыре обязательные сборки и прогона — PASS: min 86/86, Windows 357/357 в каждой конфигурации, WSL 337 PASS + 17 SKIP из 354, 0 FAIL. `arch_check`, `schema_check`, `sql_check` — PASS во всех четырёх прогонах; `req_report` — 0 неверных ID. Фактические итоги ниже.
2. Добавлены 13 тестов `Trash.*`, процессный `cli_library_trash`, реальные JSON-примеры ниже. Сценарии задачи покрыты так:

| № | Тесты и результат |
|---|---|
| 1 | `Trash.MoveRestorePreserveBytesRevisionAndOpenSnapshotAndUpdateIndex`: SHA-256 ZIP прежний, revision/history прежние, открытый снимок продолжает читать и проверять потоки, index/list отражают новое место. |
| 2 | `Trash.PurgeRequiresTrashRemovesIndexAndMissingIsPerAsset`: purge из assets запрещён, удаление из trash убирает файл и все строки индекса; повторный вызов сообщает not_found. |
| 3 | `Trash.ConflictsNeverReplaceEitherFile`: занятое назначение не меняется вместе с источником; проверена и гонка появления назначения непосредственно перед системным переименованием. |
| 4 | `Trash.ExternalStatChangeAbortsEachOperationAndRemovesPreparedRow`: для всех трёх операций изменение размера/mtime и внешнее удаление источника дают conflict; подготовленная строка убирается. |
| 5 | `Trash.CorruptManifestDoesNotPreventLifecycleAndUsesRebuildPolicy`: корректный ZIP с неразбираемым manifest.json проходит trash/restore/trash/purge без изменения байтов. Наблюдатель сообщает ошибку чтения и rebuild_required; rebuild сообщает unreadable по новому пути без выдумывания обязательных полей индекса. |
| 6 | `Trash.FaultsBeforeAndAfterActionRecoverEveryOperation`: три операции × пять точек отказа (after_prepared, before_replace, after_replace, after_committed_journal, after_indexed_journal). `Trash.RecoveryExplicitPresenceMatrixAndFailedRowsRemain`: trash/restore × четыре сочетания наличия файлов × prepared/committed/indexed. `Trash.PurgeRecoveryRollsBackExistingFileAndCompletesAbsentFile`: оба исхода purge × три состояния; temp_path действительно SQL NULL. Неоднозначные строки получают failed и сохраняются, файлы не меняются. |
| 7 | `Trash.ObserverFailureKeepsMoveAndRebuildRepairsIndex`: перенос остаётся совершённым, предупреждение и rebuild_required сохранены; rebuild исправляет in_trash, последующий sync видит unchanged. В тестах восстановления события также исправляют индекс по новому месту и удаляют строку после purge. |
| 8 | `Trash.UserdataMainFileSha256RemainsIdentical`: в userdata добавлена пользовательская строка; SHA-256 основного файла до открытия и после закрытия Catalog одинаков. Проверка CLI также сравнивает SHA-256. |
| 9 | `Trash.ChangeInTrashFailsWithoutTouchingBytes`: change возвращает not_found, ZIP в корзине побайтно прежний. |
| 10 | `Trash.SummaryAndInfoUseExactUint64AndReportOverflowOrRequiredRebuild`: точный размер двух ZIP; UINT64_MAX и переполнение на функции суммы без реальных больших файлов; bytes=null и предупреждение при переполнении, trash=null при rebuild_required. SQL SUM не используется. |
| 11 | `cli_library_trash`: два ассета → trash → info → restore → отказ purge без --yes → purge --all --yes; посторонний ZIP и каталог сохранены; смешанный список даёт failed/done и код 4; текстовый restore проверен. Linux проверяет отсутствие подтверждения и запрет производственной записи, затем явный SKIP по прежней политике NTFS. |

Дополнительно `Trash.AssetLockReadOnlyAndInvalidReferencesRefuseBeforeMutation` проверяет блокировку ассета, read-only и чужой LibraryID. Все новые юнит-тесты имеют `RecordProperty("req", ...)`; CLI помечен `req:K-3`.

3. Изменены только разрешённые файлы. Схемы, формат, golden ZIP, кодек, валидаторы, Library и существующие тесты create/change не менялись. Исходный пользовательский `3D_am_open_questions_e2472e9.zip` не тронут и не включён в коммиты.

### Реализация и выбранный способ переименования

- **Windows:** `MoveFileExW` с `MOVEFILE_WRITE_THROUGH`, без `MOVEFILE_REPLACE_EXISTING`. Гонка назначения также возвращает conflict, без замены.
- **Linux:** `renameat2(RENAME_NOREPLACE)` через syscall. При ENOSYS/EINVAL/EOPNOTSUPP — `link` без замены, затем `unlink` источника. Если сбой оставил обе ссылки, восстановление помечает операцию failed и сохраняет оба файла для разбора.
- Общий `AssetLock` удерживается через действие, переходы журнала и очистку. В pending_ops сохраняются исходные размер и mtime; непосредственно перед действием они проверяются ещё раз. Смена/исчезновение источника после подготовки — conflict.
- Новые операции проходят prepared → committed → indexed; verified и доказательство нового ZIP остаются обязательными для прежних create/update. Failed-переход разрешён только новым видам. `state.sql` не менялся: текстовый state уже допускает нужное значение.
- `AssetRepository` посылает события trash/restore/purge без разбора манифеста. `LifecycleResult` содержит ref/op_id; ревизия файла не изменяется и не выдумывается из повреждённого манифеста. В location-событиях существующее поле revision равно 0 (неизвестно); Indexer использует ref и новое местоположение, а не это поле. После сбоя уведомление может повторяться; обновление/удаление индекса идемпотентно.
- `Catalog::trash/restore/purge` используют тот же mutex, что create/change и наблюдатель. Ошибка индекса не отменяет файловое действие и даёт прежнее предупреждение/rebuild_required.
- Операции не обращаются к соединению userdata. Существующий `Library::open(read_write)` не менялся: служебное R/W-соединение владельца библиотеки остаётся, как принято в В-1 T-017, на метод проверки которого ссылается T-019. Сохранность данных подтверждена побайтовым SHA-256 основного файла после корректного закрытия базы; логическая замена сравнения не понадобилась.
- `info.trash` вычисляется по zip_size из индекса в C++ uint64; переполнение даёт null и `am.catalog.trash_size_overflow`. При rebuild_required новое поле trash целиком null с предупреждением. Автоочистки и удаления связей userdata нет.

### Команды и фактические результаты

`tools\build_min.cmd --ci` — PASS:

```text
100% tests passed, 0 tests failed out of 86
Total Test time (real) = 23.00 sec
req_report: 13 IDs, 86 tests, 49 without req, 0 invalid IDs
```

Целевые прогоны:

- `tools\build_cli.cmd --offline --ci -R StateJournal`: 3/3 PASS (прежние проверки переходов и evidence).
- `tools\build_cli.cmd --offline --ci -R Trash`: `100% tests passed, 0 tests failed out of 14`, 62.13 sec. Помимо 13 новых тестов regex включает прежний Sync-тест с TrashPath в имени.
- `tools\build_cli.cmd --offline --ci -R cli_library`: 7/7 PASS, 25.08 sec.
- WSL `tools/build_linux.sh cli --ci -R Trash`: `100% tests passed, 0 tests failed out of 14`, 70.33 sec, без SKIP; `req_report: 45 IDs, 354 tests, 48 without req, 0 invalid IDs`.

Полный `tools\build_cli.cmd --ci` — PASS, 357/357, без SKIP:

```text
100% tests passed, 0 tests failed out of 357
Total Test time (real) = 922.19 sec
req_report: 45 IDs, 357 tests, 49 without req, 0 invalid IDs
```

В том числе PASS: `CommitProtocol.KillEveryCheckpointThreeTimesAndRecover`, `AssetRepository.KillEveryRepositoryCheckpointThreeTimesWithOpenSnapshot`, все `Trash.*`, `cli_library_trash`, `arch_check`, `schema_check`, `sql_check`.

Полный `tools/build_linux.sh cli --ci` в WSL — PASS: 354 теста, 337 PASS, 17 SKIP, 0 FAIL:

```text
100% tests passed, 0 tests failed out of 354
Total Test time (real) = 1051.88 sec
req_report: 45 IDs, 354 tests, 48 without req, 0 invalid IDs
```

Все 13 новых `Trash.*` прошли без SKIP. Пропуски: 16 прежних ограничений платформы/производственной записи/точности mtime и новый `cli_library_trash` по той же политике записи только на проверенном фиксированном NTFS. Отказ без --yes и производственный read-only в этом CLI-сценарии проверяются до явного SKIP. `arch_check`, `schema_check`, `sql_check` — PASS.

Полный `tools\build_cli.cmd --offline --ci` — PASS, 357/357, без SKIP:

```text
100% tests passed, 0 tests failed out of 357
Total Test time (real) = 894.31 sec
req_report: 45 IDs, 357 tests, 49 without req, 0 invalid IDs
```

Число тестов не уменьшилось относительно принятой T-018: min 86 → 86, Windows 343 → 357, Linux 340 → 354. Все обязательные проверки завершены; прежние kill-тесты на Windows прошли в обеих конфигурациях без правок.

Логи, JUnit, req_report и машинные итоги сохранены в `.cache/t019-*`. Предупреждения GCC в прежних строках за пределами изменений не исправлялись, чтобы не менять create/change и посторонний код; новые участки без предупреждений.

### Примеры реального JSON

Синтетическая библиотека `.cache/t019-cli-example/library`, два ZIP. `am library trash <dir> <id-1> <id-2> --json`:

```json
{"results":[{"asset_id":"cb35846c-3c6d-4c29-9419-058fb325e583","status":"done","error":null},{"asset_id":"030a0768-ea8b-4258-81f6-80c07c409e75","status":"done","error":null}],"command":"library trash"}
```

`am library info <dir> --json` — фрагмент фактического результата:

```json
{"trash":{"assets":2,"bytes":"1749"},"indexed_assets":2}
```

`am library purge <dir> --all --yes --json`:

```json
{"results":[{"asset_id":"030a0768-ea8b-4258-81f6-80c07c409e75","status":"done","error":null},{"asset_id":"cb35846c-3c6d-4c29-9419-058fb325e583","status":"done","error":null}],"command":"library purge"}
```

Полные JSON сохранены в `.cache/t019-cli-example/{trash,info,purge}.json`.

### Отклонения и вопросы

- Архитектурных отклонений нет. Зависимости не добавлялись, материалы владельца и реальная коллекция не использовались.
- Промежуточные тесты обнаружили сравнение эквивалентных путей Windows с разными разделителями в журнале. Сравнение исправлено лексической нормализацией; матрица повторно прошла на обеих платформах. Проверкой внешнего исчезновения источника закреплён conflict после подготовки.
- **Вопросы:** нет.

## Приёмка

**Принята 2026-11-16**, слияние `6a7f96d` (`git merge --no-ff codex/T-019`, коммиты `b54bf83`, `c94d197`).

Прогоны управляющего на ветке перед слиянием (все `.cpp` в `src`/`tests/unit` обновлены по времени перед каждой сборкой):

- `toolsuild_min.cmd --ci` — 86/86 PASS;
- `toolsuild_cli.cmd --ci` — 357/357 PASS;
- `toolsuild_cli.cmd --offline --ci` — 357/357 PASS.

Проверено по diff:

- переименование без замены: `MoveFileExW` без `MOVEFILE_REPLACE_EXISTING`; в Linux — `renameat2(RENAME_NOREPLACE)`, запасной путь `link` + `unlink`, при обеих ссылках восстановление даёт `failed`;
- блокировка ассета и повторная сверка `stat` перед действием; при расхождении — conflict, подготовленная строка удаляется;
- восстановление следует п. 5 решений: trash/restore — только источник → откат, только назначение → доведение, оба или ни одного → `failed` без изменения файлов; purge — файл есть → откат, нет → доведение; `temp_path` для purge — SQL NULL;
- короткий переход `prepared → committed` и состояние `failed` разрешены только видам trash/restore/purge, прежние переходы create/update не ослаблены;
- события trash/restore индексируются по новому месту, purge удаляет строку; ошибка индекса не отменяет перемещение, ставит `rebuild_required`;
- `userdata` не открывается операциями; размер корзины считается в C++ `uint64`, без SQL `SUM`;
- схемы, формат, golden ZIP и `state.sql` не менялись.

Замечания (не блокируют, учесть в следующих задачах):

1. В `state.sql` комментарий к `state` перечисляет `prepared | verified | committed | indexed`; фактически журнал теперь хранит и `failed` для операций корзины. Исправить комментарий при ближайшем изменении `state.sql` (через миграцию по правилам T-016), без отдельной задачи.
2. `purge --all` при отсутствующем каталоге `trash/` вернёт ошибку ввода-вывода от `directory_iterator`, а не пустой список. Каталог создаётся `init`, поэтому это касается только повреждённой раскладки; при случае — пустой результат с предупреждением.
3. В событиях перемещения `revision = 0` означает «неизвестно». Это задокументировано в отчёте; потребителей поля, кроме индексатора, пока нет. При появлении новых наблюдателей сделать поле `std::optional`.
4. `recover()` по-прежнему не вызывается при открытии библиотеки и из CLI (вне объёма, как и в T-019).

