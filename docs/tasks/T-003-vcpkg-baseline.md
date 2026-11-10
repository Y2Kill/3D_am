# T-003. Базовая линия vcpkg: закреплённый реестр, сборка `win-cli`, офлайн-повтор

**Статус:** ready · **Веха:** M0 · **Основание:** ADR-0002 (vcpkg manifest mode, офлайн-режим через кэши в `.cache/`), `docs/plan/roadmap.md` M0 («Зафиксировать `builtin-baseline`…»)

## Цель

Сейчас `vcpkg.json` содержит заглушку `"builtin-baseline": "TODO-M0-pin-vcpkg-registry-commit"`, поэтому пресеты с vcpkg (`win-cli` и далее) не работают. Нужно:
1. закрепить реестр vcpkg на конкретном коммите;
2. собрать пресет `win-cli` со всеми зависимостями по умолчанию;
3. доказать, что повторная сборка проходит **без сети** — из кэшей внутри проекта.

От этой задачи зависят спайк S7 (T-004) и вся веха M1.

## Решения управляющего

- **Реестр — клон `microsoft/vcpkg` в `.cache/vcpkg`** (внутри проекта, папка `.cache/` уже в `.gitignore`). Встроенный в Visual Studio vcpkg не используется: у него нет клона реестра, а значит нет `builtin-baseline`.
- **Версия — последний релизный тег вида `YYYY.MM.DD`, выпущенный не позже чем за 14 дней до начала работы.** `builtin-baseline` — SHA коммита этого тега.
- **Глобальное окружение пользователя не меняется.** `VCPKG_ROOT` и прочие переменные задаёт только скрипт сборки на время своего запуска. Метрики vcpkg отключены (`VCPKG_DISABLE_METRICS=1`, `bootstrap-vcpkg.bat -disableMetrics`).
- **Состав зависимостей не меняется:** набор по умолчанию из `vcpkg.json`, features не включаются. Если какой-то порт не собирается на `x64-windows`, — остановиться и описать в отчёте: имя порта, версия, фрагмент лога. Манифест самостоятельно не править.

## Требования

### 1. Реестр (загрузка разрешена владельцем 2026-11-10)
- `git clone https://github.com/microsoft/vcpkg .cache/vcpkg`; `git -C .cache/vcpkg checkout <тег>`.
- `.cache\vcpkg\bootstrap-vcpkg.bat -disableMetrics`.
- В `vcpkg.json`: `"builtin-baseline": "<SHA тега>"`. Больше в манифесте ничего не менять.

### 2. Скрипт `tools/build_cli.cmd`
По образцу `tools/build_min.cmd`:
- `TMP`/`TEMP` → `.cache\tmp`;
- `VCPKG_ROOT=%ROOT%\.cache\vcpkg`, `VCPKG_DISABLE_METRICS=1`;
- `vcvars64.bat`;
- затем `cmake --preset win-cli`, `cmake --build --preset win-cli`, `ctest --preset win-cli`.

Ключ `--offline` (первым аргументом) дополнительно выставляет:
- `X_VCPKG_ASSET_SOURCES=clear;x-azurl,file:///<root>/.cache/vcpkg-assets,,read;x-block-origin`;
- `VCPKG_BINARY_SOURCES=clear;files,<root>/.cache/vcpkg-bin,read`.

Остальные аргументы передаются в `ctest`. Если пресет переопределяет эти переменные в `environment`, — описать в отчёте, как это решено. Сами пресеты менять можно только при необходимости, с обоснованием.

### 3. Онлайн-сборка
- `tools\build_cli.cmd` — полная сборка: установка зависимостей в `.cache/vcpkg-installed`, наполнение бинарного кэша `.cache/vcpkg-bin` и кэша загрузок `.cache/vcpkg-assets`.
- Все существующие тесты проходят и в этом пресете. GoogleTest теперь может прийти из vcpkg через `find_package(GTest CONFIG)` — проверить, что тесты обнаруживаются (их число совпадает с `win-debug-min`).

### 4. Офлайн-повтор (главный критерий)
- Удалить `build/win-cli` и `.cache/vcpkg-installed`. Кэши `.cache/vcpkg-bin` и `.cache/vcpkg-assets` сохранить.
- `tools\build_cli.cmd --offline` — сборка и тесты проходят. `x-block-origin` гарантирует, что ни одной загрузки из интернета не было. Если vcpkg всё же пытается что-то скачать, — это провал критерия: зафиксировать, что именно, и описать в отчёте.

### 5. Ничего не ломать
- `tools\build_min.cmd` по-прежнему работает без vcpkg: все тесты проходят.

## Файлы (что можно менять)

- `vcpkg.json` — только `builtin-baseline`
- `tools/build_cli.cmd` (новый)
- `CMakePresets.json` — только при необходимости, с обоснованием в отчёте
- `.cache/**` — рабочие данные, не коммитятся
- `docs/tasks/T-003-vcpkg-baseline.md` (раздел отчёта), `docs/tasks/README.md` (статус T-003)

## Критерии приёмки

1. `vcpkg.json` содержит SHA реального коммита релизного тега; в отчёте — тег, SHA, дата тега.
2. `tools\build_cli.cmd` — онлайн-сборка и все тесты проходят (число тестов = как в `win-debug-min`).
3. После удаления `build/win-cli` и `.cache/vcpkg-installed` команда `tools\build_cli.cmd --offline` проходит с `x-block-origin`. В отчёте — фактические итоговые строки обоих прогонов.
4. `tools\build_min.cmd` — все тесты проходят.
5. В отчёте:
   - вывод `vcpkg list` (порты и версии);
   - время онлайн- и офлайн-сборки;
   - размер `.cache/vcpkg`, `.cache/vcpkg-bin`, `.cache/vcpkg-assets`, `.cache/vcpkg-installed`;
   - любые проблемы с путём проекта: он содержит `!!!` — проверить, не ломает ли это vcpkg или cmd.
6. Глобальные переменные окружения и настройки пользователя не изменены; ничего не установлено вне папки проекта.

## Вне объёма

- Linux-пресеты и CI (T-005).
- Features `archive`, `formats3d` и другие, Qt.
- Изменение состава зависимостей и `docs/research/libraries.md` — это делает управляющий по итогам отчёта.

## Отчёт исполнителя

Дата: 2026-11-10. Ветка: `codex/T-003`. Статус в реестре: `review`, **с блокером: закончилось место на D:**. Задача полностью не выполнена: онлайн-сборка остановлена на OpenSSL, офлайн-повтор не запускался.

### Ветка и коммиты

- `a3d346f` — `Pin vcpkg baseline and add CLI build script for T-003`: baseline, скрипт и необходимая правка пресета.
- Завершающий коммит `Report T-003 disk-space blocker and mark for review`: отчёт и статус задачи.
- Push, слияние в `main` и переписывание истории не выполнялись.

### Выполнение критериев приёмки

| № | Результат |
|---|---|
| 1 | Выполнен: официальный реестр клонирован в `.cache/vcpkg`, checkout тега `2026.10.16`, bootstrap с `-disableMetrics`; изменён только `builtin-baseline` в манифесте. SHA: `9eec002df1cd109f8f104cd966a763daf9751489`. Дата создания подписанного тега: `2026-10-20T14:43:06-07:00`; дата публикации релиза: 2026-10-20. Это последний релизный тег, опубликованный до границы 2026-10-27 (14 дней до начала работы). |
| 2 | Не выполнен: `tools\build_cli.cmd` создан и запущен, но конфигурация завершилась `BUILD_FAILED` на `openssl:x64-windows@3.6.5#2`: при распаковке Perl исчерпано место. Установлены 10 из 18 запланированных пакетов. Сборка приложения и тесты `win-cli` не запускались, совпадение числа тестов не подтверждено. |
| 3 | Не выполнен: после сбоя порта работа остановлена согласно прямому требованию задачи. `build/win-cli` и `.cache/vcpkg-installed` не удалялись; офлайн-повтор не запускался. Режим `--offline` реализован, но успешная сборка с `x-block-origin` не доказана. |
| 4 | Выполнен: фактический прогон `tools\build_min.cmd` — **PASS, 63/63**, включая `arch_check`, `schema_check`, `sql_check`; без пропусков. |
| 5 | Ниже приведены фактический `vcpkg list`, длительность и размеры кэшей. На выполненных шагах проблемы с `!!!` не выявлены: bootstrap, обнаружение MSVC 14.44, сборка и кэширование первых портов прошли. Полная сборка по этому пути не проверена из-за заполнения диска. |
| 6 | Переменные скрипта ограничены `setlocal DisableDelayedExpansion`; загрузки, реестры, бинарные пакеты и временные файлы направлены внутрь проекта. Bootstrap запускался с процессными переменными и `-disableMetrics`. Глобальные переменные и пользовательские настройки не менялись, установка вне проекта не выполнялась. Использованы уже установленные компоненты Visual Studio. |

Тег и публикация подтверждены локальными `git rev-parse HEAD`, `git for-each-ref refs/tags/2026.10.16` и [официальной страницей релиза](https://github.com/microsoft/vcpkg/releases/tag/2026.10.16). Версия загрузившегося инструмента: `2026-09-26-51bf87ca6e9bf3e622d84ff323bd202ab1ca0c0b`.

### Скрипт и изменение пресета

`tools/build_cli.cmd` задаёт `TMP`/`TEMP`, `VCPKG_ROOT`, `VCPKG_DISABLE_METRICS=1`, `VCPKG_DOWNLOADS`, `VCPKG_REGISTRIES_CACHE` и `VCPKG_DEFAULT_BINARY_CACHE` внутри проекта, вызывает `vcvars64.bat`, затем configure/build/ctest для `win-cli`. Начальный `--offline` снимается через `shift`, оставшиеся аргументы собираются для `ctest`. Отложенное раскрытие переменных отключено, чтобы сохранить `!!!` в пути.

Из общего пресета `vcpkg` удалён блок `environment` с двумя фиксированными значениями кэшей. Иначе пресет перекрывает значения `read`/`x-block-origin`, выставленные скриптом. Онлайн-значения `readwrite` перенесены в скрипт; офлайн-значения — `read` и `x-block-origin`. Остальные поля пресетов не изменены. При прямом вызове vcpkg-пресета без скрипта теперь необходимо самостоятельно задавать эти переменные окружения.

Состав зависимостей и features манифеста не менялся, дополнительные features проекта не включались.

### Команды, время и фактические результаты

Подготовка:

```text
git clone https://github.com/microsoft/vcpkg .cache/vcpkg
git -C .cache/vcpkg checkout 2026.10.16
.cache\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

Для bootstrap `TMP`/`TEMP`, `VCPKG_ROOT`, `VCPKG_DISABLE_METRICS` и `VCPKG_REGISTRIES_CACHE` заданы только в процессе запуска. Успешный вывод bootstrap сохранён в `.cache/t003_bootstrap.log`.

Онлайн-прогон: `tools\build_cli.cmd`, полный вывод — `.cache/t003_online.log` и `build/win-cli/vcpkg-manifest-install.log`; подробности порта — `.cache/vcpkg/buildtrees/openssl/stdout-x64-windows.log`.

```text
cmake -E tar: error: No space left on device
CMake Error: Problem extracting tar:
  D:/!!!_USER_FOLDERS/General_projects_folder/3D_am/.cache/vcpkg/downloads/strawberry-perl-5.42.2.1-64bit-portable.zip
CMake Error at scripts/cmake/vcpkg_find_acquire_program.cmake:267 (message):
  Unable to find PERL
error: building openssl:x64-windows failed with: BUILD_FAILED
Elapsed time to handle openssl:x64-windows: 2.9 min
-- Running vcpkg install - failed
-- Configuring incomplete, errors occurred!
ElapsedSeconds=490.804 ExitCode=1
```

Итог: **FAIL**, код 1, 490,804 с (8 мин 10,804 с), включая загрузки инструментов и сборку зависимостей; клонирование и bootstrap в это время не входят. Число выполненных тестов `win-cli`: 0. Сообщение о ненайденном Ninja появилось после сбоя toolchain при конфигурации; первичная ошибка — отсутствие места при распаковке Perl.

Офлайн-прогон: **не запускался**, время и итог отсутствуют.

Минимальная сборка: `tools\build_min.cmd`, полный вывод — `.cache/t003_min.log`.

```text
61/63 Test #61: arch_check ..........................................................   Passed    2.05 sec
62/63 Test #62: schema_check ........................................................   Passed    0.89 sec
63/63 Test #63: sql_check ...........................................................   Passed    0.18 sec
100% tests passed, 0 tests failed out of 63
Total Test time (real) =   5.25 sec
ElapsedSeconds=20.946 ExitCode=0
```

Итог: **PASS, 63/63**, код 0, полное время сборки и тестов 20,946 с. Запуск выполнен с расширенным доступом к существующему `.cache/pylib`, без изменения зависимостей Python и без ослабления проверок. В выводе сборки кода проекта предупреждений нет. В онлайн-прогоне порт `minizip-ng` выдал предупреждение CMake о неиспользуемой переменной `CMAKE_DISABLE_FIND_PACKAGE_ZLIBNG`, после чего успешно собрался.

Фактический вывод `.cache\vcpkg\vcpkg.exe list --x-install-root=<project>/.cache/vcpkg-installed` после остановки (описания опущены):

```text
cli11:x64-windows                         2.7.2
efsw:x64-windows                          1.7.2
gtest:x64-windows                         1.18.0
json-schema-validator:x64-windows         2.4.0
minizip-ng:x64-windows                    4.1.0
minizip-ng[zlib]:x64-windows
nlohmann-json:x64-windows                 3.12.0#2
vcpkg-cmake-config:x64-windows            2026-07-21
vcpkg-cmake-get-vars:x64-windows          2025-05-29
vcpkg-cmake:x64-windows                   2025-08-07
zlib:x64-windows                          1.3.2#2
```

Размеры — сумма логических длин файлов, включая скрытые файлы; измерены после описанной ниже очистки одной дублирующей загрузки:

| Каталог | Байты |
|---|---:|
| `.cache/vcpkg` | 1 853 254 252 |
| `.cache/vcpkg-bin` | 26 315 307 |
| `.cache/vcpkg-assets` | 537 173 518 |
| `.cache/vcpkg-installed` | 127 534 201 |

`git diff --check` — без ошибок пробельных символов.

### Отклонения от задачи и состояние папки

- По условию остановки при сбое порта дальнейшие сборки не выполнялись. Перенос на другой диск, замена Perl/OpenSSL, изменение портов и состава зависимостей не предпринимались.
- `Get-PSDrive D` после сбоя показал **0 свободных байт**. Для возможности записать отчёт и коммиты удалена только созданная этой задачей дублирующая копия `.cache/vcpkg/downloads/strawberry-perl-5.42.2.1-64bit-portable.zip` размером 304 301 401 байт. Перед удалением проверено совпадение SHA-512 с полной копией в `.cache/vcpkg-assets/e37c541bb6c4f1c0187bf8ba22b19ce9ead87f6bd1e68c05e7fc4eb2c10a4183c3ee732bf1b26071939f525861fae62b6bef04100b31504ab50fffe7526f84e3`; копия в asset-кэше сохранена. После этого измерено 304 250 880 свободных байт. Прочие кэши, частичная распаковка и логи оставлены для диагностики.
- Изменены только разрешённые файлы. Кэши и сборочные файлы не коммитятся. Существовавший до начала задачи незарегистрированный `3D_am_open_questions_e2472e9.zip` оставлен без изменений. Все изменения исполнителя сохранены коммитами.

### Вопросы

1. Блокер для владельца/управляющего: требуется освободить место на D: перед продолжением. После устранения причины нужно вернуть T-003 на доработку, завершить онлайн-прогон, затем выполнить удаление двух каталогов и офлайн-повтор по исходной задаче. Необходимый объём свободного места не измерен; новый норматив не назначался.
2. До успешной онлайн-сборки и офлайн-повтора считать T-003 и зависимые от неё работы завершёнными нельзя.
