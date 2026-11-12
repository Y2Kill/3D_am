# T-005. CI на GitHub Actions: Windows и Linux, минимальная и полная сборка

**Статус:** ready · **Веха:** M0 (P0.1), выполняется после M1-CLI · **Основание:** `docs/plan/roadmap.md` раздел «CI», ADR-0002 (vcpkg, кэши), ADR-0036 п. 8 (`AM_REQUIRE_PYTHON_TESTS=ON`), ADR-0038 (O-13 закрывается тестами кодека в CI), `docs/specifications/fs_support.md` (ext4 — вместе с Linux CI)

## Цель

1. Платформа CI — **GitHub Actions** (решение владельца 2026-11-12).
2. Один workflow с четырьмя заданиями из roadmap: `win-debug-min`, `win-cli`, `linux-min`, `linux-cli`. Каждое — сборка и полный ctest, включая `arch_check`, `schema_check`, `sql_check`, `message_catalog_check`, `cli_*`. Python-проверки обязательны: при их отсутствии сборка падает, а не пропускает тесты.
3. CI вызывает **те же скрипты**, что и локальная сборка. Отдельной «CI-логики» сборки нет: рецепт в workflow — подготовка окружения, вызов скрипта, публикация отчётов.
4. Впервые собрать **весь проект на Linux** (`linux-min`, `linux-cli`) и довести тесты до PASS. До сих пор на Linux собирался только спайк S7, а POSIX-ветки `commit_zip`, `file_sink`, `file_source` ни разу не компилировались.

Публичный репозиторий — https://github.com/Y2Kill/3D_am (создан по поручению владельца 2026-11-12). Публикует **только управляющий** скриптом `tools/publish_public.sh`: публичная копия истории без `data/inventory/*.zip` и с адресом noreply вместо почты; локальная история не меняется, поэтому хеши публичных коммитов отличаются от локальных. Исполнитель не делает push и не добавляет remote. Поэтому задача проверяется локально: Windows-задания — штатными скриптами, Linux-задания — теми же скриптами в WSL Ubuntu, синтаксис workflow — `actionlint`. Первый настоящий прогон на GitHub управляющий примет отдельно, после push.

## Решения управляющего (не пересматривать в задаче)

1. **Файлы workflow.**
   - `.github/workflows/ci.yml` — один workflow `ci`, четыре задания (`jobs`): `win-debug-min`, `win-cli`, `linux-min`, `linux-cli`; без `matrix`-магии, если она ухудшает читаемость.
   - Триггеры: `push` в `main` и `codex/**`, `pull_request` в `main`, `workflow_dispatch`. Расписания (`schedule`) нет: периодические тяжёлые прогоны — позже, отдельной задачей.
   - `permissions: contents: read` на уровне workflow; секретов и токенов workflow не использует.
   - `concurrency`: группа по workflow и ссылке, `cancel-in-progress: true` только для веток, не для `main`.
   - `timeout-minutes` у каждого задания: гипотеза 120 для `*-cli` (холодная сборка OpenSSL в vcpkg ≈ 17 мин локально), 30 для `*-min`; в отчёте — фактические времена локальных аналогов.
   - Образы раннеров закреплены по имени: `windows-2022`, `ubuntu-24.04` (не `*-latest`).
   - Все сторонние actions закреплены **полным SHA коммита** с комментарием версии (`uses: actions/checkout@<sha> # v4.x.y`). Разрешены только `actions/checkout`, `actions/setup-python`, `actions/cache`, `actions/upload-artifact`. Других actions (в том числе `lukka/run-vcpkg`, `ilammy/msvc-dev-cmd`) не использовать: окружение MSVC и vcpkg настраивают наши скрипты.

2. **Скрипты сборки — общие для CI и локальной работы.**
   - `tools\build_min.cmd`, `tools\build_cli.cmd`: путь к `vcvars64.bat` находится через `vswhere.exe` (`%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe`, `-latest -products * -version [17.0,18.0) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`). Нет vswhere или установки — понятная ошибка и код 1. Прежний жёсткий путь к Community удаляется. Локально скрипты должны по-прежнему выбирать VS 2022 с MSVC 14.44; в отчёте — какую установку выбрал vswhere на этой машине (здесь есть Community и BuildTools 2022) и версия `cl.exe`.
   - Новый ключ `--ci` у обоих `.cmd` и у нового `tools/build_linux.sh`:
     - конфигурация с `-DAM_REQUIRE_PYTHON_TESTS=ON`;
     - ctest с `--output-junit <build>/junit.xml` (CMake ≥ 3.21) и `--output-on-failure`;
     - остальное поведение — как без ключа. `--ci` и `--offline` совместимы, порядок ключей произвольный.
   - Дополнительные аргументы ctest после ключей передаются как сейчас (`-R`, `-E` и т. п.).
   - **`tools/build_linux.sh min|cli [--offline] [--ci] [ctest-args…]`** — Linux-аналог по образцу `spikes/s7/build_linux.sh`: `set -euo pipefail`, `TMPDIR` в `.cache/tmp-linux`, vcpkg-клон `.cache/vcpkg-linux` (LF-файлы, свой bootstrap), бинарный кэш `.cache/vcpkg-bin-linux`, общий кэш загрузок `.cache/vcpkg-assets`, офлайн-режим с `x-block-origin`; пресеты `linux-min`/`linux-cli`. Глобальную конфигурацию пользователя не меняет. Файл с битом исполнения в git (`git update-index --chmod=+x`) и `eol=lf` в `.gitattributes`.
   - Подготовка vcpkg для CI — скрипты `tools/ci/bootstrap_vcpkg.cmd` и `tools/ci/bootstrap_vcpkg.sh`: клон `https://github.com/microsoft/vcpkg` в `.cache/vcpkg` (Windows) / `.cache/vcpkg-linux` (Linux), checkout **ровно** коммита `builtin-baseline` из `vcpkg.json` (читается из файла, не дублируется в workflow), bootstrap с отключёнными метриками. Если клон уже есть и стоит на нужном коммите — ничего не делать. Локально эти скрипты тоже должны работать.

3. **Пресеты.** У `linux-cli` свой каталог установленных пакетов: `VCPKG_INSTALLED_DIR = ${sourceDir}/.cache/vcpkg-installed-linux`. Сейчас он наследует общий `.cache/vcpkg-installed` от `vcpkg`, и сборка в WSL в той же папке, что и Windows, смешала бы базы установленных портов. Других изменений пресетов нет.

4. **Python в CI.**
   - `actions/setup-python` с закреплённой версией 3.12 (минимум для `Path.is_relative_to` и прочего, что используют проверки; если проверки требуют более новую — указать в отчёте и взять минимально достаточную).
   - Тестовые модули — файл `tools/ci/requirements-test.txt` с **точными версиями и хешами** (`--require-hashes`), установка в `.cache/pylib`: `python -m pip install --require-hashes --no-deps --target .cache/pylib -r tools/ci/requirements-test.txt`. Версии — как в локальном `.cache/pylib`: `jsonschema==4.26.0`, `jsonschema-specifications==2025.9.1`, `referencing==0.37.0`, `attrs==26.1.0`, `rpds-py==2026.9.1`, плюс прочие зависимости, если без них `import jsonschema` не работает. Хеши — для колёс `win_amd64` и `manylinux x86_64` под выбранную версию Python (pip берёт подходящий из перечисленных).
   - Локальный `.cache/pylib` задача не переустанавливает.

5. **Кэши GitHub Actions** (`actions/cache`), только каталоги внутри рабочей копии:
   - `.cache/vcpkg-bin` / `.cache/vcpkg-bin-linux` — бинарный кэш vcpkg. Ключ: ОС, триплет, хеш `vcpkg.json`, версия компилятора (строка из `cl` / `gcc --version`, вычисляется шагом). `restore-keys` — префикс «ОС + триплет + хеш `vcpkg.json`»: при смене компилятора старый кэш восстанавливается, а vcpkg сам отбрасывает несовместимые пакеты по ABI-хешу.
   - `.cache/vcpkg-assets` — кэш загрузок исходников портов (ключ по baseline).
   - `.cache/fetchcontent` — GoogleTest для `*-min` (ключ по хешу `tests/CMakeLists.txt` и корневого `CMakeLists.txt`).
   - Кэш vcpkg-клона не нужен: клон делается `git clone --filter=tree:0` или аналогом с последующим checkout SHA; время — в отчёт.
   - В CI vcpkg работает **онлайн** (кэш может быть пуст): `--offline` в workflow не используется.

6. **Отчёты и артефакты.**
   - Каждое задание загружает `junit.xml` (`actions/upload-artifact`, `if: always()`, имя артефакта — имя задания, срок хранения 14 дней).
   - При провале дополнительно загружается `build/<preset>/Testing/Temporary/LastTest.log`.
   - Отчёт трассировки требований (`req_report.py`) в задачу не входит: инструмента ещё нет (инфраструктура тестов M1).

7. **Linux: что должно работать и как.**
   - `linux-min`: GCC из образа (`ubuntu-24.04`, GCC 13) и Ninja (`apt-get install ninja-build`, если её нет в образе). Clang не нужен.
   - `linux-cli`: зависимости vcpkg на `x64-linux`; системные пакеты для сборки портов — минимальный набор, найденный в WSL, перечисленный в workflow одной командой `apt-get install --no-install-recommends` и в отчёте.
   - **Код ядра доводится до сборки на Linux** без изменения поведения на Windows: исправления под `#ifdef`/`#else` в уже существующих POSIX-ветках, недостающие заголовки, ошибки компиляции GCC. Если для Linux нужно **архитектурное решение** (новая абстракция, изменение публичного API, отказ от функции) — остановиться и описать вопрос.
   - **Предупреждения GCC.** `am_apply_defaults` на GCC включает `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor`; код проекта этим набором ещё не собирался, и `-Wconversion` почти наверняка даст много срабатываний. `AM_WARNINGS_AS_ERRORS` остаётся `OFF` (в CI тоже). Массово их не исправлять: исправить только те, что указывают на реальную ошибку (усечение размера/смещения 64→32, знаковое сравнение размеров, затенение, ведущее к неверной переменной). В отчёте — число предупреждений по флагам (`-Wconversion`, `-Wshadow`, …) и список исправленных с причиной. План снижения до нуля и включение `-Werror` на Linux — отдельная задача.
   - **Режим ФС.** По `fs_support.md` ext4 — `read_only_default`, пока не пройдены испытания S5. Задача это **не меняет**:
     - производственный путь на Linux отказывает в записи (`am.repo.storage_read_only`); тест `cli_create_*` на Linux ожидает именно этот отказ (код 4), а не успех; это различие явно оформлено в тесте по платформе, с комментарием-ссылкой на `fs_support.md`;
     - модульные тесты протокола фиксации и репозитория, которые подставляют `StorageCapabilities` явно, на Linux **выполняются** и проходят — это первая компиляция и прогон POSIX-ветки `commit_zip` (`rename` + `fsync` каталога);
     - тесты, принципиально зависящие от Windows (например, `SetFileInformationByHandle`, `FILE_SHARE_DELETE`, `MoveFileExW`, буквы дисков, `GetDriveTypeW`), на Linux пропускаются через `GTEST_SKIP() << "<причина>"` или не регистрируются; **каждый** такой тест перечисляется в отчёте с причиной. Ослаблять ожидания Windows-тестов нельзя.
     - kill-тесты и crash-helper'ы на Linux: если есть POSIX-реализация завершения процесса — выполняются; если нет — пропуск с причиной и запись в отчёт. Новых POSIX-реализаций kill-тестов задача не требует.
   - Тестовые каталоги в WSL лежат на `/mnt/d` (drvfs, не ext4); на раннере GitHub — на ext4. Это различие указать в отчёте; тесты не должны зависеть от типа ФС, кроме явно помеченных.

8. **Сеть и данные.** Тесты не обращаются к сети (К-133) и не читают коллекцию владельца; реестр реальных данных `tests/real/registry.json` в CI не используется (его проверки должны пропускаться при отсутствии коллекции — проверить, что так и есть, и указать в отчёте).

## Разрешения на эту задачу

- **Сеть:** скачивание для CI-инфраструктуры — клон vcpkg в `.cache/vcpkg-linux`, порты vcpkg для `x64-linux`, колёса Python для вычисления хешей (в `.cache/tmp`, не в систему), бинарник `actionlint` (закреплённая версия, в `.cache/tools/`), чтение страниц релизов GitHub для SHA actions.
- **WSL:** запуск WSL Ubuntu и установка через `apt` **только** пакетов, нужных для сборки (компилятор, Ninja, CMake при необходимости, `pkg-config`, autotools и т. п. для портов vcpkg). Конфигурацию WSL и Windows не менять; после работы — `wsl --terminate <дистрибутив>`. Это разрешение владельца, данное для спайка S7, повторно применяется к этой задаче.
- Глобальные переменные окружения, системный Python, системные пакеты Windows — не менять.

## Что прочитать

- `docs/plan/roadmap.md`, раздел «CI»; `docs/tasks/T-003-vcpkg-baseline.md` (кэши, офлайн-режим); `spikes/s7/build_linux.sh` (Linux-окружение vcpkg).
- `tools/build_min.cmd`, `tools/build_cli.cmd`, `CMakePresets.json`, `tests/CMakeLists.txt` (Python-проверки, `AM_REQUIRE_PYTHON_TESTS`).
- `docs/specifications/fs_support.md`, `src/storage/commit/protocol.cpp`, `src/storage/asset_zip/{sink,source}.cpp` (POSIX-ветки).

## Проверки

1. **Windows локально:** `tools\build_min.cmd --ci`, `tools\build_cli.cmd --ci`, `tools\build_cli.cmd --offline --ci` — PASS; `junit.xml` создан и содержит то же число тестов, что ctest; конфигурация действительно с `AM_REQUIRE_PYTHON_TESTS=ON` (показать строку из `CMakeCache.txt`). Без `--ci` скрипты работают как раньше.
2. **Отрицательная проверка Python:** при `--ci` и недоступном `jsonschema` (например, `AM_PYTHON_EXTRA_PATH` на пустой каталог) конфигурация падает с понятной ошибкой. Показать вывод.
3. **Linux в WSL:** `tools/build_linux.sh min --ci`, `tools/build_linux.sh cli --ci`, затем `tools/build_linux.sh cli --offline --ci` после удаления `build/linux-cli` и `.cache/vcpkg-installed-linux` — PASS. Число тестов и список пропущенных с причинами.
4. **Bootstrap:** `tools/ci/bootstrap_vcpkg.sh` на чистом каталоге (временно переименовать `.cache/vcpkg-linux`) даёт клон ровно на `builtin-baseline`; повторный запуск — ничего не делает. Для `.cmd` — то же на временном каталоге (параметр/переменная для пути клона допустимы; локальный `.cache/vcpkg` не трогать).
5. **Workflow:** `actionlint` без замечаний; YAML не содержит action без SHA; ручная сверка: шаги каждого задания повторяют проверенные локально команды.
6. **Хеши Python:** `pip install --require-hashes --no-deps --target <временный каталог> -r tools/ci/requirements-test.txt` на Windows и в WSL — успешно, `import jsonschema` работает с этим каталогом.

## Файлы (что можно менять)

- Новые: `.github/workflows/ci.yml`, `tools/build_linux.sh`, `tools/ci/bootstrap_vcpkg.cmd`, `tools/ci/bootstrap_vcpkg.sh`, `tools/ci/requirements-test.txt`, при необходимости `tools/ci/*.py` для вспомогательных шагов (чтение baseline, версия компилятора).
- Не менять `tools/publish_public.sh`.
- Изменяемые: `tools/build_min.cmd`, `tools/build_cli.cmd`, `CMakePresets.json` (только `VCPKG_INSTALLED_DIR` у `linux-cli`), `.gitattributes` (LF для `*.sh`), `README.md` (раздел о сборке: CI, Linux-скрипт, ключ `--ci`).
- Код и тесты — **только** для сборки и прогона на Linux (п. 7): POSIX-ветки и заголовки в `src/`, платформенные пропуски и ожидания в `tests/`. Каждое изменение кода перечислить в отчёте с причиной.
- **Не менять:** `vcpkg.json` (состав и baseline), ADR, спецификации, `fs_support.md`, `libraries.md`, схемы, матрицы, roadmap, логику ядра на Windows, ожидания существующих Windows-тестов.

## Критерии приёмки

1. Проверки 1–6 выполнены, фактический вывод в отчёте.
2. Windows: `build_min` 84/84, `build_cli` и `--offline` — все 278 прежних тестов PASS; число и имена тестов не уменьшились.
3. Linux: `linux-min` и `linux-cli` (онлайн и офлайн) PASS; таблица «тест → пропущен на Linux → причина»; список изменений кода ради Linux.
4. Workflow: 4 задания, actions по SHA, `permissions: contents: read`, кэши только внутри рабочей копии, артефакты `junit.xml`; `actionlint` чист.
5. В отчёте:
   - выбранная vswhere установка VS и версия `cl.exe` локально;
   - список apt-пакетов для Linux с объяснением, какой порт что требует;
   - фактические времена локальных аналогов каждого задания (холодный и тёплый кэш, если измерялось);
   - SHA и версии каждого action (управляющий перенесёт в `libraries.md`);
   - что ещё нужно для первого прогона на GitHub (настройки репозитория, включение Actions, ограничения).
6. Сдача по `AGENTS.md`: только разрешённые файлы, коммиты в `codex/T-005`, без слияния, push и переписывания истории.

## Вне объёма

- `git remote add`, push, настройки репозитория на GitHub — управляющий (`tools/publish_public.sh`).
- Отчёт трассировки требований `req_report.py`, golden ZIP — инфраструктура тестов M1.
- Испытания ext4 по S5/S5+ и смена режима ext4 в `fs_support.md` — отдельная задача после первого зелёного Linux CI.
- Release-сборки, артефакты установщика, подпись, `win-asan`, fuzz, шкала объёмов, расписание периодических прогонов.
- GUI и Qt в CI.

## Отчёт исполнителя

**2026-11-12. Ветка:** `codex/T-005`, создана от `main` после приёмки T-012. Коммиты реализации: `18b772a`, `8196cfb`, `1229373`, `94dc34f`. Отчёт и статус `review` — отдельный завершающий коммит. Push, remote и слияние не выполнялись.

### Критерии приёмки

1. Проверки 1–6 выполнены локально: Windows, отрицательная проверка Python, WSL online/offline, чистые bootstrap и повтор, actionlint, установка Python по хешам. Команды и фактические итоги ниже.
2. Windows: 84 теста минимума и все 278 прежних тестов CLI сохранены; оба полных CLI-прогона PASS. Имена проверены по JUnit и журналу принятой T-012, ожидания Windows не ослаблены.
3. Linux: минимальная и полная сборки PASS, в том числе offline после удаления `build/linux-cli` и `.cache/vcpkg-installed-linux`. Платформенные исключения, предупреждения и изменения для Linux перечислены ниже. Испытания ext4 по S5 не объявляются выполненными.
4. Workflow содержит четыре явных задания, образы `windows-2022`/`ubuntu-24.04`, заданные триггеры, `contents: read`, отмену предыдущих запусков только вне `main`, кэши внутри рабочей копии, JUnit и LastTest.log при провале, хранение 14 дней. Тайм-ауты 30/120 минут помечены как hypothesis. `actionlint` — PASS.
5. Выбор VS, apt-пакеты, времена, версии/SHA actions и оставшийся первый запуск на GitHub приведены ниже.
6. Изменены только разрешённые файлы. `.gitattributes` уже содержит `*.sh text eol=lf`, поэтому правка не потребовалась; оба новых `.sh` имеют LF и git mode `100755`. ADR, спецификации, `vcpkg.json`, библиотеки, схемы и `tools/publish_public.sh` не изменялись.

### Общие скрипты и окружение

`build_min.cmd` и `build_cli.cmd` находят VS через требуемый запрос vswhere; на этой машине выбран `C:\Program Files\Microsoft Visual Studio\2022\Community`, `cl 19.44.35223 x64` (MSVC 14.44). `--ci` включает обязательные Python-проверки и JUnit, дополнительные аргументы передаются ctest, оба порядка `--ci`/`--offline` проверены. `AM_PYTHON_EXTRA_PATH` позволяет выбрать каталог тестовых модулей. Запуски без `--ci` также проверены.

Linux — WSL Ubuntu 24.04.4, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1. Рабочая папка на `/mnt/d`: `9p`, `aname=drvfs`, том Windows `D:` (NTFS); это **не ext4** раннера GitHub. Режим Linux остаётся `read_only_default` по `fs_support.md` v0.2. Производственный create проверяется по коду 4 и `am.repo.storage_read_only`; тестовые записи репозитория используют явно подставленные synthetic capabilities.

Linux получает отдельные `.cache/vcpkg-linux`, `.cache/vcpkg-installed-linux`, `.cache/vcpkg-bin-linux` и `.cache/vcpkg-registries-linux`. Общий asset-кэш — `.cache/vcpkg-assets`. Первый Linux-min configure обнаружил Windows CMakeCache в общем FetchContent; Linux-скрипт теперь хранит его source/build/subbuild в `.cache/fetchcontent/linux` (внутри кэшируемого каталога). Пресеты, кроме разрешённого `VCPKG_INSTALLED_DIR`, не изменены.

В WSL Python 3.12.3, локально Windows — 3.14.0, в CI закреплена ветка 3.12. Установки по `--require-hashes --no-deps --target` во временные `.cache/tmp/t005-pylib-win` и `.cache/t005-pylib-linux` прошли, `import jsonschema` — PASS на обеих ОС. Существующий `.cache/pylib` не переустанавливался. Хеши включают cp312-win_amd64, cp312-manylinux x86_64 и дополнительно cp314-win_amd64 для локального Windows; чистые Python-колёса общие. Добавлен `typing-extensions==4.15.0` с маркером Python < 3.13 — зависимость `referencing`. Остальные пять версий точно соответствуют задаче.

**Отрицательная проверка:** `AM_PYTHON_EXTRA_PATH` на пустой `.cache/tmp/t005-empty-pylib`, затем `tools\build_min.cmd --ci`:

```text
CMake Error at tests/CMakeLists.txt:129 (message):
  Python module 'jsonschema' is required for schema_check (pip install
  jsonschema)
-- Configuring incomplete, errors occurred!
exit=1
```

После проверки восстановлен путь `.cache/pylib`. В сохранённых CMakeCache всех CI-прогонов (`.cache/t005-<имя>-CMakeCache.txt`): `AM_REQUIRE_PYTHON_TESTS:BOOL=ON`, `AM_WARNINGS_AS_ERRORS:BOOL=OFF`. При запуске без `--ci` скрипт явно возвращает `AM_REQUIRE_PYTHON_TESTS=OFF`, чтобы CI-настройка не оставалась в локальном кэше.

### Команды и фактические результаты

```text
tools\build_min.cmd --ci
100% tests passed, 0 tests failed out of 84
Total Test time (real) =  32.93 sec
```

```text
tools\build_cli.cmd --ci
100% tests passed, 0 tests failed out of 278
Total Test time (real) = 944.57 sec
```

```text
tools\build_cli.cmd --offline --ci
100% tests passed, 0 tests failed out of 278
Total Test time (real) = 1466.71 sec
```

```text
tools/build_linux.sh min --ci
100% tests passed, 0 tests failed out of 82
Total Test time (real) =  16.23 sec
```

```text
tools/build_linux.sh cli --ci
100% tests passed, 0 tests failed out of 275
Total Test time (real) = 1197.62 sec
```

```text
tools/build_linux.sh cli --offline --ci
100% tests passed, 0 tests failed out of 275
Total Test time (real) = 835.80 sec
```

| Прогон | Тестов | SKIP | Полное время, с |
|---|---|---|---|
| `win-min` | 84 | 0 | 40.46 |
| `win-cli` | 278 | 0 | 955.33 |
| `win-offline` | 278 | 0 | 1508.80 |
| `linux-min` | 82 | 0 | 118.00 |
| `linux-cli` | 275 | 13 | 1369.00 |
| `linux-offline` | 275 | 13 | 1264.00 |

Отдельная холодная подготовка `tools/build_linux.sh cli --ci -N`: **3053 с**, 68 шагов компиляции/линковки, 275 тестов перечислены без запуска. Установка портов заняла 41 минуту, из них OpenSSL — 24 минуты. Полный online-прогон после этой подготовки использовал готовые зависимости. Офлайн-прогон ниже выполнен после очистки build/installed, с полной пересборкой проекта.

JUnit каждого прогона разобран как XML; число `testcase` совпадает с числом ctest, ошибок 0. Время таблицы — весь вызов скрипта; строки `Total Test time` — только ctest. Windows CLI выполнялся параллельно только с подготовкой/компиляцией Linux (`ctest -N`), полные тесты разных ОС запускались последовательно из-за общих каталогов фикстур.

Linux offline выполнен после удаления только двух разрешённых каталогов сборки/установки. SHA-256, размеры и набор файлов кэшей до/после совпали: **9280 файлов, изменённых 0**. Бинарные и source-кэши read-only, `x-block-origin`; попыток authoritative download в итоговом offline-логе нет.

Логи и копии JUnit/LastTest: `.cache/t005-<имя>.log`, `.cache/t005-<имя>-junit.xml`, `.cache/t005-<имя>-tests.log`; времена — `*-result.json`. Дополнительные проверки: `.cache/t005-negative-python.log`, `t005-pip-{win,win-import,linux}.log`, `t005-workflow-check.log`, `t005-linux-environment.log`.

### Bootstrap и пакеты Linux

Оба bootstrap читают SHA из `vcpkg.json` через общий Python helper, клонируют с `--filter=tree:0 --depth 1 --no-checkout`, переключаются на `9eec002df1cd109f8f104cd966a763daf9751489` и вызывают bootstrap с `-disableMetrics`. Каталог ограничен проектным `.cache`; существующие tracked-правки не затираются. На точном SHA с готовым инструментом повтор ничего не делает.

Windows: чистый `.cache/t005-bootstrap-win`, **84.83 с**, повтор **0.07 с**, `vcpkg unchanged`. Исходный `.cache/vcpkg` не менялся. Linux: исходный `.cache/vcpkg-linux` временно переименован, чистый bootstrap — **363.79 с**, повтор **0.05 с**, HEAD совпал с baseline; затем исходный клон восстановлен, тестовый сохранён в `.cache/t005-bootstrap-linux`. Время включает клон, checkout и bootstrap, отдельно время только clone не измерялось.

| Пакет | Для чего |
|---|---|
| `ninja-build` | Сборка основного проекта и CMake-портов; устанавливается обоими Linux-заданиями |
| `pkg-config` | `.pc`-метаданные и проверки портов; Linux CLI |
| `make`, `perl`, `linux-libc-dev` | OpenSSL 3.6.5: собственный Configure/Makefile и заголовки Linux; Linux CLI |
| `build-essential`, `cmake`, `git`, `curl`, `zip`, `unzip`, `tar` | Уже присутствовали в WSL; компилятор, root CMake, bootstrap/получение и распаковка портов |
| `python3-pip` | Установлен в WSL для проверки тестовых колёс; в CI pip предоставляет setup-python |

При подготовке WSL лишними оказались `autoconf`, `automake`, `libtool` и их новые зависимости `m4`/`autotools-dev`: порты используют собственный Configure OpenSSL, CMake и Meson. Перед offline-прогоном отменена только эта собственная установка: apt simulation показала ровно пять удаляемых пакетов, затем удалены только они, без autoremove; все пять до задачи отсутствовали (первый apt-лог помечает их NEW). В итоговую команду CI эти autotools не включены. Python/pip сохранены. Глобальная конфигурация Windows/Python/WSL и пользовательского git не менялась. WSL завершён командой `wsl --terminate Ubuntu` после проверок.

### Изменения кода и Linux-исключения

Все слои, CLI и обе crash-helper программы скомпилированы GCC 13.3 без ошибок компиляции. Первый полный запуск выявил runtime-проблему POSIX-публикации на DrvFS: **87% tests passed, 37 tests failed out of 275** (`Total Test time = 1114.10 sec`), полный вызов 1277 с. Лог/JUnit сохранены как `.cache/t005-linux-cli-before-drvfs-*` и `.cache/t005-linux-cli-before-drvfs.log`.

Причина отделена от ядра минимальным Python repro: `rename` открытого источника возвращает успех, но новый путь даёт ENOENT до закрытия исходного дескриптора; при закрытом источнике новый путь видим, старый читатель цели продолжает читать старые байты. FaultHooks-проба до исправления показывала `after_replace target=0 temp=0`, после — `target=1 temp=0`, SUCCESS. Код и логи — `.cache/t005_rename_probe.py`, `.cache/t005_rename_variants.py`, `.cache/t005-commit-probe{,-after}.log`.

| Файл | Изменение и причина |
|---|---|
| `src/storage/commit/protocol.cpp` | Только POSIX: проверочный дескриптор закрывается перед pathname-rename; затем сохраняется fsync каталога и сверяется SHA-256 всего опубликованного файла с verified evidence, как в существующем Windows fallback. Это исправляет DrvFS lookup и обнаруживает подмену имени после проверки. Windows-ветка и публичный API не изменены. |
| `tests/unit/test_asset_repository.cpp` | Только на Linux общий fixture явно задаёт synthetic `StorageMode::full`, чтобы прогонять запись/фиксацию, не меняя реальную политику ФС. Проверка detect подтверждает `read_only_default`; сценарий max_file_size использует synthetic override. Windows-ветки прежние. |
| `tests/unit/test_commit_protocol.cpp` | Только на Linux fixture помечает capabilities как synthetic/full; проверяется `Replacement::rename` и отказ при подмене temp после verify с тем же размером. Busy-fixture держит явный LOCK_SH, соответствующий POSIX cleanup probe: обычный POSIX open не создаёт Win32 deny-delete. `WindowsAndFilesystemEvidence` получает явный GTEST_SKIP вместо пустого PASS. Windows-ожидания прежние. |
| `tests/unit/test_asset_service.cpp` | Комментарий с источником `fs_support.md` к уже существовавшему Linux-ожиданию отказа create; поведение не изменено. |
| `tests/cli/test_asset_cli.py` | Для Linux create проверяется точный JSON message_id `am.repo.storage_read_only`, добавлены ссылки на fs_support. Аргументные/путевые create-отказы больше не пропускаются на Linux. Для чтения сохранены независимые synthetic ZIP-фикстуры. Windows-ожидания сохранены. |

После исправления целевой Linux-прогон протокола — **18 тестов, 0 ошибок, 7 skips**, быстрый прогон репозитория — **36 тестов, 0 ошибок, 2 skips** (без OneGiB). Дополнительно Windows пересобран после POSIX-правки: все **18/18** тестов CommitProtocol PASS, пропусков и предупреждений MSVC нет (`.cache/t005-win-commit-after-posix.log`, ctest 52.63 с). Полные итоговые прогоны приведены выше. Остальные Win32 skips уже существовали; список фактически полученных исключений ниже. POSIX rename/fsync, file_sink/file_source, SQLite и запись через AssetRepository реально выполняются с разрешённым тестовым override. Надёжность файловой системы и произвольных внешних читателей этим не сертифицируется: S5/ext4 остаётся отдельной задачей.


| Тест | На Linux | Причина |
|---|---|---|
| `Paths.ContainerPathAllocationFailureHasIdentity` | не зарегистрирован | Win32/_DEBUG allocation-failure hook |
| `Paths.NativeLongPathPrefix` | не зарегистрирован | Win32 extended-length path prefix |
| `ZipWriter.UnicodeKeyAllocationFailureIsFatalOutOfMemory` | не зарегистрирован | Win32/_DEBUG allocation-failure hook |
| `AssetService.CreateRefusalsAndPublishRaceNeverReplaceOutput` | GTEST_SKIP | Create requires fixed NTFS |
| `AssetService.PostCreateFailureDeletesOnlyTheNewOutput` | GTEST_SKIP | Create requires fixed NTFS |
| `AssetService.ValidationAndExtractionUseSharedV3AndPreserveSuccessfulFiles` | GTEST_SKIP | Fixture creation requires fixed NTFS; process tests cover read-only fixtures |
| `AssetService.StreamScopesMinorAndCorruption` | GTEST_SKIP | Read-only process fixtures cover non-Windows builds |
| `CommitProtocol.FallbackOnlyForUnsupportedAndChecksTheMovedIdentity` | GTEST_SKIP | Win32 fallback test |
| `CommitProtocol.ProtectedTemporaryFileDeniesAnotherWriter` | GTEST_SKIP | Win32 sharing test |
| `CommitProtocol.LockHeldByAnotherProcessIsReleasedByKill` | GTEST_SKIP | Win32 child process test |
| `CommitProtocol.KillEveryCheckpointThreeTimesAndRecover` | GTEST_SKIP | Win32 kill matrix |
| `CommitProtocol.RecoveryUsesWholeFileIdentityNotOnlyManifest` | GTEST_SKIP | Win32 crash identity test |
| `CommitProtocol.OpenReaderKeepsOldBytesAndNewReadersSeeNewVersion` | GTEST_SKIP | Win32 open reader replacement |
| `CommitProtocol.WindowsAndFilesystemEvidence` | GTEST_SKIP | Win32 OS and NTFS evidence APIs |
| `AssetRepository.AnotherProcessLockPreventsLostRevisionAndIsReleasedByKill` | GTEST_SKIP | Win32 process locks |
| `AssetRepository.KillEveryRepositoryCheckpointThreeTimesWithOpenSnapshot` | GTEST_SKIP | Win32 repository kill tests |

Kill-helper’ы имеют ветку `std::_Exit` на POSIX, но родительские `Child`/`Process` используют только Win32 CreateProcess/TerminateProcess; POSIX-harness отсутствует и по задаче не добавлялся. Пустой WindowsAndFilesystemEvidence на Linux заменён явным skip. Проверка старого открытого снимка репозитория и остальные платформонезависимые проверки фиксации выполняются.

Реальная коллекция не читалась: `tests/real/registry.json` содержит `entries: []`, тестов с меткой `real` сейчас нет, активных обращений к коллекции в тестах нет. Все использованные данные синтетические; тесты не загружают зависимости и не обращаются к сети. Клон и порты скачиваются только при подготовке сборки.

### Предупреждения GCC

Чистая итоговая offline-сборка GCC, предупреждений: **30**.

| Флаг | Количество |
|---|---|
| `-Wconversion` | 6 |
| `-Wdangling-else` | 5 |
| `-Wmisleading-indentation` | 11 |
| `-Wmissing-field-initializers` | 1 |
| `-Wrange-loop-construct` | 1 |
| `-Wshadow` | 3 |
| `-Wunused-function` | 3 |

`linux-min`: 1 × `-Wmissing-field-initializers` (`CommandInvocation::op_id` в существующем тесте). Набор флагов не менялся; `-Werror` не включён. Полные строки — `.cache/t005-linux-offline.log`, сводка — `.cache/t005-warning-audit.log`. Массового изменения стиля/подавления предупреждений нет.

Предупреждения проверены: `-Wconversion` — выбор конструктора `Json → optional<Json>` (3 места, по две диагностические строки), а не усечение числа/смещения. `-Wshadow` — имя `info` внутри лямбды макроса INSTANTIATE_TEST_SUITE_P, используется именно параметр лямбды. Остальные — однострочные if/for, GoogleTest-макросы без внешнего else, копия строки в fixture-loop, пустой op_id по умолчанию и неиспользуемые test helpers. Реальных ошибок усечения/сравнения размеров/неверной переменной не выявлено; исправлений производственного кода ради предупреждений нет. Счётчик включает повторные диагностики шаблонов.

### Закреплённые actions и первый запуск на GitHub

Все четыре выпуска используют Node 24; SHA проверены по страницам релизов и `action.yml`. Старые Node 20-выпуски не оставлены: [GitHub завершил их runtime-переход 23.09.2026](https://github.blog/changelog/2026-09-23-node-20-is-no-longer-available-in-github-actions/).

| Action | Выпуск | SHA |
|---|---|---|
| checkout | [v7.0.1](https://github.com/actions/checkout/releases/tag/v7.0.1) | `3d3c42e5aac5ba805825da76410c181273ba90b1` |
| setup-python | [v7.1.0](https://github.com/actions/setup-python/releases/tag/v7.1.0) | `6d4fd22b20465ccc0f7354476588e8df7c9ca0e0` |
| cache | [v6.1.0](https://github.com/actions/cache/releases/tag/v6.1.0) | `55cc8345863c7cc4c66a329aec7e433d2d1c52a9` |
| upload-artifact | [v7.0.2](https://github.com/actions/upload-artifact/releases/tag/v7.0.2) | `cf430e030ddbb5b0abf93d22962f4752f3646cd9` |

`actionlint 1.7.7` установлен в `.cache/tools/`, SHA-256 архива сверен с опубликованными checksums; итог без замечаний. Ручная сверка четырёх заданий подтверждена локальными вызовами тех же build/bootstrap-скриптов. Бинарный cache key включает ОС, triplet, хеш манифеста и хеш строки компилятора; restore-prefix заканчивается на хеше манифеста. Asset key содержит ОС/baseline, FetchContent — ОС/хеш двух CMakeLists. Клон не кэшируется; `--offline` в workflow отсутствует.

Первого настоящего прогона GitHub **ещё нет**: по задаче публикует управляющий через `tools/publish_public.sh`. Ему требуется включённый Actions и разрешение четырёх указанных actions (либо их SHA в allow-list), доступ hosted runner к GitHub/PyPI/источникам портов и приём артефактов на 14 дней. Специальные secrets, PAT и write-permissions не нужны. Для Node 24 нужен runner ≥ 2.327.1; workflow использует обновляемые hosted-образы, self-hosted не настраивались. Времена/лимиты и работу на ext4 нужно подтвердить первым реальным запуском, затем управляющий сможет назначить required checks.

### Отклонения и вопросы

Архитектурных отклонений и новых открытых вопросов нет. Разделение Linux FetchContent и тестовых capabilities — детали локального запуска в общей рабочей папке; режим производственной записи не расширялся. Первый запуск на GitHub оставлен управляющему согласно явной границе задачи.

Все изменения исполнителя закоммичены. Существовавший неотслеживаемый `3D_am_open_questions_e2472e9.zip` не изменялся и в коммиты не включён.
