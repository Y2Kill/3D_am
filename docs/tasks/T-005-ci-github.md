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
