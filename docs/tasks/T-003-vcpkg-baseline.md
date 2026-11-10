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

## Решение управляющего по отчёту от 2026-11-10 (доработка)

**Статус: rework.** Остановка по AGENTS.md п. 3 была правильной: диагноз подтверждён.

1. **`arch_check` исправлен в `main`.** Самоссылка `X -> X` в графе CMake больше не считается зависимостью. Источник ребра — POST_BUILD-шаг vcpkg `z-applocal` с `$<TARGET_FILE:X>`; связать цель саму с собой CMake не позволяет, поэтому такое ребро не бывает связью уровней. Проверка уровней и поиск циклов между разными целями не изменились. Проверено: `arch_check` на `build/win-cli` — 0 нарушений, `tools\build_min.cmd` — 63/63. Файл `tools/arch_check/arch_check.py` в задаче по-прежнему не менять.
2. **Что сделать.** В ветке `codex/T-003` выполнить `git merge main` (без rebase), затем:
   - онлайн-прогон `tools\build_cli.cmd` — ожидается 63/63;
   - удалить `build/win-cli` и `.cache/vcpkg-installed`, выполнить офлайн-повтор `tools\build_cli.cmd --offline` (п. 4 требований);
   - `tools\build_min.cmd`.

   Отчёт дописать подразделом «Доработка» в конец своего раздела, статус — снова `review`.
3. **Удаление `environment` из пресета `vcpkg` принято.** Окружение пресета перекрывает окружение процесса, поэтому режимы `read` и `x-block-origin` из скрипта иначе не действовали бы. Следствие: пресеты `win-cli` и прочие на основе `vcpkg` запускаются только через `tools\build_cli.cmd`, иначе vcpkg возьмёт кэш по умолчанию в профиле пользователя, то есть за пределами проекта. Это управляющий допишет в README при приёмке.

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
