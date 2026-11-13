# T-015. Изменение ассета с чужими extra fields и комментариями записей (ADR-0039 п. 7)

**Статус:** ready · **Веха:** M1 · **Основание:** ADR-0039 п. 7 (Ф-7, O-15), SPEC §1 (extra fields), находка управляющего при приёмке T-014 (2026-11-13)

## Цель

ADR-0039 п. 7: читатель принимает любые корректно оформленные extra fields, смысл берёт только из `0x0001`, прочие **игнорирует и в изменённые заголовки не переносит**; писатель 1.0 пишет только `0x0001`. SPEC §1: неизвестные extra fields и комментарии записей вслепую не переносятся.

Сейчас `AssetRepository` (`src/storage/repository/repository.cpp`, producer в фиксации) переносит неизменённые записи через `ZipWriter::copy_raw(old->archive(), index, budget)` с `RawCopyOptions{}` по умолчанию. `carried_extra` (`src/storage/asset_zip/writer.cpp`) при неразрешённом id **отказывает** (`am.zip.write.extra_not_preserved`), так же — при непустом комментарии записи. Итог: ассет, у любой записи которого есть, например, `0x5455` (Info-ZIP/7-Zip, обычное дело после перепаковки), **нельзя изменить** — ни метаданные, ни файлы. Это поведение T-008/T-010, принятое до ADR-0039, и оно ему противоречит. Тест T-014 `ZipWriter.AssetRewritePreservesOnlyUnchangedRawExtraFields` проверяет писатель с явным `carry_extra_ids = {0x5455}` и этот путь не покрывает.

## Решения управляющего (не пересматривать в задаче)

1. **Писатель.** В `RawCopyOptions` добавить явный режим отбрасывания: `bool drop_unlisted = false;`. При `true` неразрешённые extra fields (кроме `0x0001`, который писатель формирует сам, как сейчас) и комментарий записи **не переносятся** в новые local/central headers; данные записи копируются побайтно, как сейчас. По умолчанию (`false`) поведение прежнее — отказ: явный выбор остаётся у вызывающего, молча ничего не теряется. `carry_extra_ids`/`carry_comment` сохраняют приоритет: явно разрешённое переносится и при `drop_unlisted`.
2. **Репозиторий.** `AssetRepository` вызывает `copy_raw` с `drop_unlisted = true` и пустым `carry_extra_ids`, `carry_comment = false`: формат 1.0 не придаёт им смысла (ADR-0039 п. 7). Других изменений producer нет.
3. **Сведения об отброшенном.** Тихой потери быть не должно: `WrittenEntry` получает счётчики `dropped_extra_fields` (число полей) и `dropped_comment` (bool), заполняемые при `drop_unlisted`. Включать их в `ChangeResult`/CLI в этой задаче **не нужно** — достаточно, чтобы данные были доступны и проверялись тестом; вывод для пользователя — отдельное решение.
4. **Чтение и V4 не меняются**: читатель уже принимает чужие поля (проверить тестом, что `verify` такого ассета — `full`, если хеши совпадают).
5. **`create` не затрагивается**: новые записи и так пишутся без чужих полей.

## Тесты

1. Писатель: `copy_raw` с `drop_unlisted = true` у записи с `0x5455` и комментарием — заголовки без `0x5455` и без комментария; сжатые байты, CRC, размеры совпадают с источником; `verify_written` PASS; счётчики `dropped_extra_fields = 1` (в local и central — уточнить в отчёте, как считается), `dropped_comment = true`. Явный `carry_extra_ids = {0x5455}` при `drop_unlisted = true` — поле переносится.
2. Писатель: `drop_unlisted = false` (по умолчанию) — прежний отказ сохранён (существующие тесты не меняются).
3. Репозиторий: синтетический ассет 1.0, у одной из записей `content/...` есть `0x5455` в local и central headers и комментарий записи (собрать ZIP через существующие помощники тестов или вручную, **не** через ослабление писателя). `AssetRepository::change` с `SetMetadata` — успех; в новом ZIP у этой записи нет `0x5455` и комментария; содержимое и SHA-256 прежние; `verify` — `full`; V3 без `error`. До исправления этот тест должен падать с `am.zip.write.extra_not_preserved` — подтвердить в отчёте.
4. `RecordProperty("req", "ADR-0039")` во всех новых тестах.

## Файлы (что можно менять)

- `include/am/storage/asset_zip/writer.hpp`, `src/storage/asset_zip/writer.cpp` — только `RawCopyOptions`, `WrittenEntry` (новые поля) и `carried_extra`/`copy_raw`.
- `src/storage/repository/repository.cpp` — только вызов `copy_raw` в producer.
- `tests/unit/test_zip_writer.cpp`, `tests/unit/test_asset_repository.cpp`.
- `CHANGELOG.md` (строка: изменение ассетов с чужими extra fields теперь возможно, поля не переносятся).
- **Не менять:** читатель ZIP, протокол фиксации, кодек, V3, схему, спецификацию, ADR.

## Критерии приёмки

1. `tools\build_min.cmd --ci`, `tools\build_cli.cmd --ci`, `tools\build_cli.cmd --offline --ci` — PASS; `tools/build_linux.sh cli --ci` в WSL — PASS. Число тестов не уменьшилось.
2. Тесты 1–3 выполнены; тест 3 подтверждённо падал до исправления.
3. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-015`, без слияния, push и переписывания истории.

## Вне объёма

- Показ отброшенных полей пользователю (CLI/GUI), политика «сохранять известные безопасные поля».
- Golden ZIP (T-013): там добавляется положительный случай чтения с чужим extra field.

## Отчёт исполнителя

**2026-11-13. Ветка:** `codex/T-015`, создана от `main` (`f68fab7`). Коммиты: `9635238` — воспроизведение исходного дефекта; `557159c` — реализация и тесты писателя. Завершающий коммит содержит этот отчёт и статус `review`. Push, слияние и переписывание истории не выполнялись.

**Результат: T-015 выполнена.** Ассет с чужими extra fields и комментарием записи можно изменить через `AssetRepository`; содержимое, сжатые байты и SHA-256 сохраняются, чужие поля и комментарий в новые заголовки не попадают.

### Критерии приёмки

1. Windows min/cli/offline и Linux CLI в WSL — PASS. JUnit сверены с ctest; все имена прежних тестов принятой T-014 сохранены. Windows CLI **290 → 294**, Linux **287 → 291**: добавлены ровно четыре новых теста, старые не удалены и не переименованы. Минимум — прежние 84; 13 платформенных Linux-пропусков совпали с T-014.
2. Режим отбрасывания, счётчики, приоритет явного переноса и исходный отказ проверены; регрессионный тест репозитория зафиксирован до исправления. Команды и фактические результаты ниже.
3. Изменены только разрешённые файлы, отчёт и строка реестра. Читатель ZIP, кодек, V3, протокол фиксации, схема, спецификация и ADR не менялись. Коллекция владельца не читалась, зависимости не скачивались; использованы синтетические данные и существующие кэши.

### Реализация и тесты

В `RawCopyOptions` добавлено `drop_unlisted = false`. По умолчанию неизвестное/неразрешённое extra field либо непустой комментарий по-прежнему вызывает `am.zip.write.extra_not_preserved` до начала записи entry. При `true` неразрешённая метаинформация не переносится, а явно разрешённые `carry_extra_ids` и `carry_comment` имеют приоритет. Поле `0x0001` по-прежнему формируется писателем из фактических размеров/смещений, исходное поле не копируется вслепую.

В `WrittenEntry` добавлены `dropped_extra_fields` и `dropped_comment`; они сохраняются и в `WriteSummary`. **`dropped_extra_fields` — число уникальных отброшенных ID на запись по обоим заголовкам вместе**, не сумма физических экземпляров TLV: `0x5455` в local и central → **1**; `0x5455` в обоих и ещё `0x7875` → **2**. Повторения одного ID считаются один раз. `0x0001` и явно перенесённые поля в этот счётчик не входят. `dropped_comment` истинен только для непустого неразрешённого комментария, реально отброшенного режимом `drop_unlisted`. Новые записи имеют 0/false.

В producer репозитория изменён только вызов `copy_raw`: передаётся `RawCopyOptions` с `drop_unlisted = true`; списки явного переноса пусты, `carry_comment = false`. Декодирование/CRC/SHA-256/бюджет проверяются прежним кодом. `ChangeResult` и CLI не расширялись; путь создания новых записей не менялся. В `CHANGELOG.md` добавлена английская строка о возможности изменения таких ассетов.

| Тест | Что проверяет |
|---|---|
| `ZipWriter.RawCopyDropsForeignExtraAndCommentWithoutChangingPayload` | Stored и Deflate; исходный ZIP64; одинаковый `0x5455` в local/central и бинарный комментарий; результат без них, счётчики 1/true, те же сжатые байты/CRC/размеры/SHA-256 и расход бюджета; `verify_written` PASS, новые записи 0/false |
| `ZipWriter.RawCopyExplicitCarryOverridesDropPolicy` | Явный перенос `0x5455` сохраняется при drop; `0x7875` отбрасывается; `carry_comment` имеет приоритет; `verify_written` PASS |
| `ZipWriter.RawCopyCountsDistinctDroppedExtraIdsAcrossHeaders` | Точная семантика счётчика: два разных ID при дублированном `0x5455` дают 2; пустой комментарий не считается отброшенным |
| `AssetRepository.MetadataChangeDropsForeignExtraAndCommentWithoutChangingPayload` | Настоящий синтетический ассет 1.0, `0x5455` в обоих заголовках content-entry и комментарий; исходный `verify` full; `SetMetadata` проходит; новые заголовки чистые, сжатые байты/CRC/размеры/SHA-256 прежние; итоговый `verify` full, V3 без находок |
| Прежний `RawExtraAndCommentPolicyIsExplicitAndRecoverable` | Без изменений: сохранены отказы по умолчанию и возможность повторить вызов после явного разрешения переноса |

Все четыре новых теста содержат `RecordProperty("req", "ADR-0039")`. Фикстура репозитория собрана через существующий независимый `zip_test::build`, без разрешения чужих полей в писателе.

### Доказательство исходного дефекта

До изменений продукционного кода добавлен тест репозитория и выполнен:

```text
tools\build_cli.cmd --ci -R AssetRepository.MetadataChangeDropsForeignExtraAndCommentWithoutChangingPayload
0% tests passed, 1 tests failed out of 1
Total Test time (real) = 1.42 sec
am.repo.unsupported_container cause=am.zip.write.extra_not_preserved
Extra id=21589 is not authorized for transfer
```

Внешний `message_id` переведён существующей границей репозитория в `am.repo.unsupported_container`; исходный требуемый `am.zip.write.extra_not_preserved` сохранён в `cause` и явно выведен тестом. Исходный `verify` уже возвращал `full`: отказ возникал именно при изменении. Логи: `.cache/t015-red-repository-cause.log`, `*-tests.log`, `*-junit.xml`, `*-result.json`; предыдущий первый запуск — `.cache/t015-red-repository.log`.

После исправления тот же тест прошёл 1/1 PASS (ctest 1.63 с), а затем выполнен во всех полных CLI-прогонах. Отдельный набор писателя `tools\build_cli.cmd --ci -R ZipWriter.Raw` — 7/7 PASS, ctest 0.89 с; лог `.cache/t015-targeted-writer.log`.

### Команды и фактические результаты

```text
tools\build_min.cmd --ci
100% tests passed, 0 tests failed out of 84
Total Test time (real) =  34.36 sec
```

Полный вызов: 56.86 с; пропусков: 0.

```text
tools\build_cli.cmd --ci
100% tests passed, 0 tests failed out of 294
Total Test time (real) = 983.37 sec
```

Полный вызов: 1040.10 с; пропусков: 0.

```text
tools\build_cli.cmd --offline --ci
100% tests passed, 0 tests failed out of 294
Total Test time (real) = 841.18 sec
```

Полный вызов: 850.22 с; пропусков: 0.

```text
tools/build_linux.sh cli --ci
100% tests passed, 0 tests failed out of 291
Total Test time (real) = 1161.98 sec
```

Полный вызов: 1310.00 с; пропусков: 13.

Обязательные `arch_check`, `message_catalog_check`, `schema_check`, `sql_check`, CLI и ZIP interoperability выполнены во всех полных CLI-прогонах. JUnit разобран как XML; числа `testcase` совпадают с ctest, failures/errors — 0. CMakeCache CI-прогонов содержит `AM_REQUIRE_PYTHON_TESTS:BOOL=ON`. В Windows offline подтверждены бинарный cache `read` и asset cache `read;x-block-origin`; загрузок зависимостей нет.

Логи и снимки: `.cache/t015-<имя>.log`, `.cache/t015-<имя>-{junit.xml,tests.log,CMakeCache.txt,result.json}`; имена — `final-min`, `final-cli`, `final-offline`, `final-linux-cli`. Linux — WSL Ubuntu, рабочая папка `/mnt/d` (DrvFS/NTFS), не ext4; политика ФС и synthetic capabilities прежние. Python-модули: Windows `.cache/pylib`, Linux `AM_PYTHON_EXTRA_PATH=$PWD/.cache/t005-pylib-linux`, без переустановки. Linux-подготовка с `--offline --ci -N` — только компиляция/перечисление, полный ctest проверен отдельно. Полные Windows- и Linux-прогоны запускались последовательно. WSL после проверок остановлен через `wsl --terminate Ubuntu`.

### Отклонения и вопросы

Открытых вопросов и изменений вне объёма нет. Уточнение счётчика по уникальным ID явно записано в комментарии `WrittenEntry` и выше; оно даёт требуемое значение 1 для одного ID в обоих заголовках. Исходные тесты отказа/явного переноса не менялись; новые тесты добавлены без уменьшения старых проверок.

Потребовалась **чистая пересборка Windows** после изменения структуры `WrittenEntry`: первый инкрементальный запуск упал с SEH `0xc0000005`. `ninja -t deps` показал `#deps 0` у объекта теста репозитория; после правки заголовка он оставался старым и был несовместим с новым layout. `cmake --build --preset win-cli --clean-first` пересобрал все 68 шагов, затем регрессионный тест — PASS. Логи: `.cache/t015-ninja-deps.log`, `.cache/t015-green-repository.log` (промежуточный SEH), `.cache/t015-green-clean-repository.log` (чистая сборка и PASS). Продукционные build-скрипты не менялись, артефакты пересозданы внутри проекта; итоговые проверки использовали согласованную сборку.

Все изменения исполнителя закоммичены. Существующий неотслеживаемый `3D_am_open_questions_e2472e9.zip` не менялся и в коммиты не включён.
