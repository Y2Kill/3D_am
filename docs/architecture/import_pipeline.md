# Конвейер импорта

ТЗ раздел 6 и §13.3; ADR-0011, 0012, 0014, 0015, 0025, 0029. Реализация: `src/services/import/` (уровень `services`). Чтение архивов — `src/storage/archive/`. Фиксация — `storage::AssetRepository`.

**Режим исследования (`am analyze`, ADR-0029).** Это этапы 1–3 и 6–7 без упаковки и фиксации, на тех же обработчиках, что и импорт (ТЗ §13.3.9).
- Уровень `inventory` — этап 1.
- Уровень `structure` — этапы 1–3 и 6–7.
- Уровень `deep` — дополнительно анализаторы содержимого (этап 10 без записи).

Каждый объект получает статус полноты. Достижение лимита профиля (`[limits]`) — статус `incomplete{limit}`.

## Этапы и компоненты

| # | Этап | Компонент | Вход → выход | Ресурс |
|---|---|---|---|---|
| 1 | Обход (read-only) | `Scanner` | корни → `ScanTree` (узлы ФС: путь, размер, mtime, тип по сигнатуре — первые 16 байт) | io |
| 2 | Сборка томов | `VolumeSetDetector` | файлы-архивы → `VolumeSet{volumes[], complete, missing[]}` по заголовкам (+ имена как подсказка) | io |
| 3 | Предложение границ | `BoundaryProposer` + профиль | `ScanTree` → `Candidate[]{root, inputs[], context:{channel}, confidence, reasons[]}` | cpu |
| 4 | Пересмотр пользователем | GUI/CLI (`--accept-all`, `--plan plan.json`) | кандидаты → `ImportPlan` (сериализуемый JSON) | — |
| 5 | Ранние совпадения | `KnownInputMatcher` | `(name, size)`, затем SHA-256 входов → `Match[]` → политика (skip / ask / continue) | io |
| 6 | Инвентаризация содержимого | `ArchiveReader` (libarchive/7z) | входы → `EntryList` (имя: raw-байты + декодированное, размер, CRC, mtime, тип по сигнатуре) | io |
| 7 | Классификация записей | `ExclusionEngine` (правила v1) + `NestedPolicy` | записи → include/exclude/expand/keep-as-file; рекурсия для вложенных ≤ max_depth | cpu |
| 8 | Отображение путей | `PathMapper` | исходные пути → `zip_path` (ADR-0012), коллизии и переименования | cpu |
| 9 | Упаковка | `AssetBuilder` → `ZipWriter` | поток из архива → SHA-256 + Deflate/Stored в temp ZIP; SHA-256 входов параллельно | io+cpu |
| 10 | Метаданные | `MetadataExtractors` (плагины) | лёгкие экстракторы во время упаковки (STL-статистика, размеры картинок, `.url`, `.blend`-thumb) | cpu |
| 11 | Фиксация | `AssetRepository` → протокол фиксации (ADR-0006, 0025) | temp ZIP → `assets/xx/<id>.zip` + индекс | io |
| 12 | Пост-обработка | дочерние задания | миниатюры, pHash, mesh-features, FTS документов (низкий приоритет) | cpu |

## Профиль «Telegram» (пример профиля для коллекций вида `канал/модель/…`)

Ядро не содержит правил, специфичных для Telegram (ТЗ §13.2.2). Ниже фрагмент профиля; актуальный файл — [`profiles/telegram.toml`](../../profiles/telegram.toml), включая секции `[volumes]` и `[limits]`.

```toml
# profiles/telegram.toml
id = "telegram"
version = 1
[boundaries]
candidate = "leaf_dir_with_payload"      # лист с архивом/моделью
context_levels = 1                       # 1-й уровень под корнем = канал
[provenance]
parent_as = "telegram_channel"           # предположение, confirmed=false
url_files = "suggest"                    # .url → предложение, с фильтром рекламы
url_ad_patterns = ["(?i)free ?download", "(?i)extra creativity", "(?i)patreon\\.com/join"]
[nested]
action = "expand"
max_depth = 3
[exclusions]
rules = ["macos.appledouble", "macos.ds_store", "windows.thumbs_db", "windows.desktop_ini", "os.icon_r"]
[previews]
primary = ["sibling_image_largest", "archive_top_image_largest", "embedded", "render"]
[groups]
hints = ["scale", "presupport", "variant_nsfw"]
[mode]
strict = true                             # ТЗ 6.6.1
```

## Распознавание сигнатур (минимальная таблица ядра)

| Тип | Смещение: байты |
|---|---|
| ZIP (локальный заголовок / пустой / spanned) | 0: `50 4B 03 04` / `50 4B 05 06` / `50 4B 07 08` |
| RAR4 | 0: `52 61 72 21 1A 07 00` |
| RAR5 | 0: `52 61 72 21 1A 07 01 00` |
| 7z | 0: `37 7A BC AF 27 1C` |
| AppleDouble | 0: `00 05 16 07` |
| .DS_Store | 0: `00 00 00 01 42 75 64 31` |
| OLE (Thumbs.db) | 0: `D0 CF 11 E0 A1 B1 1A E1` |
| PNG / JPEG / GIF / WebP | `89 50 4E 47` / `FF D8 FF` / `GIF8` / `RIFF....WEBP` |
| Binary STL | размер == 84 + 50 × uint32@80 |
| ASCII STL | начинается с `solid` **и** бинарная проверка не прошла **и** содержит `facet` |
| 3MF | ZIP + запись `3D/3dmodel.model` |
| Blend | 0: `BLENDER` |
| PDF | 0: `%PDF-` |
| RTF | 0: `{\rtf` |
| CTB | 0: `12 FD 00 07` (v2–4) / `86 00 FD 12` |

## Режимы ошибок

- Строгий режим: любая ошибка чтения обязательной записи переводит задание в `needs_decision` с вариантами: «пропустить ассет», «импорт частично», «повторить с другим бэкендом архивов (7z)».
- Частичный импорт: `asset.flags.incomplete = true`, недоступные записи получают `status=unreadable`.
