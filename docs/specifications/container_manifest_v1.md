# Контейнер ассета и манифест — спецификация v1 (черновик для реализации)

Связанные решения: ADR-0005, 0006, 0008, 0010–0013. JSON Schema: [`schemas/manifest.v1.schema.json`](../../schemas/manifest.v1.schema.json).

## 1. Правила

1. Первая запись ZIP — `am-asset`: метод STORED, содержимое ровно `application/vnd.am.asset+zip;v=1\n`. Это признак «свой формат» (ТЗ 6.1.3). Если маркер есть, а `manifest.json` повреждён, контейнер считается **своим, но повреждённым** (не чужим ZIP).
2. Вторая запись — `manifest.json`, UTF-8 без BOM, JSON-объект.
3. Каждая запись ZIP, кроме `am-asset` и `manifest.json`, **обязана** быть описана в манифесте (`files[]`, `previews[]`, `scenes[]`, `derived[]` или `ext`). Лишняя запись при быстрой проверке даёт предупреждение «unregistered entry», и она не удаляется.
4. **Неизвестные поля сохраняются** при перезаписи (ТЗ 5.1.3): программа читает манифест в `ordered_json`, изменяет только свои ключи, остальное переносит как есть.
5. `format.version` — целое число. Если читающая программа видит версию выше поддерживаемой, она открывает ассет **только на чтение** (ТЗ 4.5.4). Секция `format.min_reader` позволяет новым версиям объявить: «старые читатели могут читать».
6. Время — ISO 8601 UTC с `Z` и миллисекундами. Размеры — целые байты. Хеши — hex в нижнем регистре.
7. Пути внутри ZIP — `/`, UTF-8, ≤ 200 байт (ADR-0012). Исходные пути — как были, с исходными разделителями, нормализованными к `/`.

## 2. Пример (сокращён)

```json
{
  "format": { "id": "am-asset", "version": 1, "min_reader": 1 },
  "asset": {
    "id": "019a5d2e-7c41-7b3e-9a10-4f2c8e1d0a77",
    "revision": 3,
    "created": "2026-11-06T10:15:03.120Z",
    "modified": "2026-11-06T12:01:44.002Z",
    "title": "Winry Rockbell + ALT",
    "description": null,
    "authors": [ { "name": "Dinamuuu3D", "origin": "inferred", "confirmed": false } ],
    "publisher": null,
    "license": { "text": null, "code": null, "file_ref": "f00031", "url": null },
    "links": [],
    "dates": { "published": null, "acquired": "2023-07-01" },
    "flags": { "incomplete": false },
    "derived_from": null
  },
  "provenance": {
    "sources": [
      { "id": "s1", "kind": "telegram_channel", "name": "3D Printing Adult", "url": null,
        "origin": "inferred:parent_folder", "confirmed": false }
    ]
  },
  "inputs": [
    { "id": "i001", "kind": "archive", "container": "rar5",
      "original_name": "0722 - Winry Rockbell + ALT - FullMetal Allchemist.rar",
      "original_path": "3D Printing Adult/0722 - Winry Rockbell - FullMetal Allchemist/",
      "size": 175815111, "sha256": "…", "mtime": "2026-11-03T09:37:25.603Z",
      "volumes": null, "parent": null, "encoding": { "names": "utf-16(rar)" },
      "content_root": "content/i001/" },
    { "id": "i002", "kind": "file", "original_name": "preview.jpg", "size": 248113, "sha256": "…",
      "content_root": "content/i002/" }
  ],
  "files": [
    { "id": "f00001", "input": "i001", "status": "included",
      "src": { "path": "Dinamuuu3D_WinryRockbell_Alt/AN_Winry_Hips.stl", "size": 25649684,
               "crc32": "f3c218f0", "mtime": "2021-06-30T02:48:13.327Z" },
      "zip_path": "content/i001/Dinamuuu3D_WinryRockbell_Alt/AN_Winry_Hips.stl",
      "size": 25649684, "sha256": "…", "type": { "id": "mesh.stl.binary", "by": "signature" } },
    { "id": "f00002", "input": "i001", "status": "excluded",
      "src": { "path": "__MACOSX/._AN_Winry_Hips.stl", "size": 4096, "crc32": "…" },
      "exclusion": { "rule": "macos.appledouble", "rules_version": 1, "by": "auto" } },
    { "id": "f00003", "input": "i001", "status": "included_renamed",
      "rename": { "reason": "path_too_long" },
      "src": { "path": "…очень длинный путь….stl", "name_raw_b64": null },
      "zip_path": "content/i001/Very long folder~3fa2b81c/part~9c01d2aa.stl" }
  ],
  "groups": [
    { "id": "g001", "kind": "scale", "label": "1/6", "files": ["f00001"], "origin": "inferred", "confirmed": false }
  ],
  "classification": {
    "categories": [ ["Фигурки", "Аниме"] ],
    "tags": { "confirmed": ["anime", "fma"],
              "suggested": [ { "tag": "nsfw", "status": "suggested", "origin": "inferred:name",
                               "origin_detail": { "handler": "am.import.name_hints", "handler_version": "1", "basis": "folder name" } } ] }
  },
  "characteristics": {
    "scale": { "type": "string", "value": "1/6", "status": "suggested", "origin": "inferred", "confirmed": false },
    "presupported": { "type": "bool", "value": true, "origin": "user", "confirmed": true }
  },
  "fields": {
    "user": {
      "printed": { "def": { "type": "bool", "label": "Напечатано" }, "value": false }
    }
  },
  "previews": [
    { "id": "pv1", "role": "primary", "file": "f00042" },
    { "id": "pv2", "role": "extra", "zip_path": "previews/p-019a….png", "generated": true,
      "render": "previews/p-019a….render.json" }
  ],
  "scenes": [],
  "derived": [
    { "id": "d001", "zip_path": "derived/019a…/AN_Winry_Hips.obj", "sha256": "…",
      "inputs": [ { "file": "f00001", "sha256": "…" } ],
      "converter": { "id": "am.convert.assimp", "version": "6.0.3", "params": { "units": "mm" } },
      "created": "…" }
  ],
  "history": [
    { "rev": 1, "at": "…", "op": "create", "op_id": "019a…",
      "app": { "name": "am", "version": "0.1.0", "build": "abc1234" },
      "import": { "profile": "telegram", "profile_sha256": "…", "rules_version": 1,
                  "handlers": { "archive": "libarchive 3.8.1" } } },
    { "rev": 2, "at": "…", "op": "set_preview", "target": "pv1" },
    { "rev": 3, "at": "…", "op": "add_derived", "target": "d001" }
  ],
  "integrity": { "manifest_covers_all_entries": true },
  "ext": {
    "x.am.stl": { "f00001": { "triangles": 513000, "bbox_mm": [41.2, 38.0, 77.9], "volume_mm3": 21040.5 } }
  }
}
```

## 3. Статусы записи `files[]`

| status | Есть содержимое в ZIP | Обязательные поля |
|---|---|---|
| `included` | да | `zip_path`, `size`, `sha256`, `src` |
| `included_renamed` | да | + `rename.reason ∈ {path_too_long, invalid_chars, encoding, collision, reserved_name}` |
| `excluded` | нет | `src`, `exclusion{rule, by: auto\|user\|profile, rules_version}` |
| `unreadable` | нет | `src`, `error{code, message}` — только если ассет `flags.incomplete = true` (ТЗ 6.6.2) |

## 4. Ревизии и история

- `asset.revision` увеличивается на 1 при **каждой зафиксированной** записи ZIP (ADR-0006).
- `history[]` — компактная запись: `rev`, `at`, `op`, `target?`, `op_id?`, `note?`. Без снимков данных (ТЗ 4.5.2).
- Перечень `op`: `create`, `import_ready` (импорт готового ассета не меняет ревизию — запись в журнал библиотеки, а не в ZIP), `edit_meta`, `add_files`, `remove_files`, `replace_file`, `set_preview`, `add_preview`, `add_derived`, `remove_derived`, `reanalyze`, `session_commit`, `classify`, `migrate_format`.

## 5. Минимальный валидный манифест

Обязательны: `format.id`, `format.version`, `asset.id`, `asset.revision`, `asset.created`, `inputs` (может быть `[]`), `files` (может быть `[]`), `history` (≥ 1 запись). Всё остальное необязательно (ТЗ 5.1.1).

## Статусы значений и предложения (ADR-0028)

- У значений с происхождением поле `status ∈ {suggested, computed, confirmed}` (ТЗ §11.2.2) дополняет `origin`/`confirmed`. Соответствие: `measured` → `computed`; `inferred`/`plugin` без подтверждения → `suggested`; `confirmed: true` → `confirmed`. Необязательный `origin_detail{handler, handler_version, basis, analyzed_at, confidence}`; `confidence` — только для методов с осмысленной оценкой.
- `classification.tags.suggested[]` и неподтверждённые `characteristics`/`groups` в ZIP содержат **только предположения, сделанные при создании ассета** (provenance, ТЗ §5.2.4). Предложения последующего анализа (классификаторы, LLM) хранятся в `index.sqlite: suggestions` и не увеличивают ревизию (ТЗ §11.2.7); при подтверждении значение записывается в ZIP.
