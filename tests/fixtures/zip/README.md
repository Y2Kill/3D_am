# ZIP: фикстуры Python zipfile

Регенерация вручную: `python tools/fixtures/make_zip_fixtures.py` (стандартная библиотека Python). Тесты не запускают генератор. Дата заголовков фиксирована: 2020-01-01.

Stored, Deflate, data descriptor при незаписываемом смещении, `force_zip64=True` для малых записей, ZIP64 с data descriptor. В каждом архиве одинаковые три записи: `manifest.json` (format.id, version 1.0), `content/café.bin` (байты 0…255, повторённые 4 раза), `generated/empty` (пусто). Точные байты и SHA-256 записей находятся в `expected.json`.

| Архив | SHA-256 |
|---|---|
| `python-stored.zip` | `a814e22946f59ce709e91226cef5d8812afe6c53acd9036b343f4b904e9f8215` |
| `python-deflate.zip` | `cf3ed763baa9572855285ae9d7ef7d042821810111ca2d797d713663bf47f683` |
| `python-descriptor.zip` | `c1e3f6c5dfa60cc02ab0b88b0fb2b8d106b0e4d8f2a2ac571a91689e8da02bed` |
| `python-force-zip64.zip` | `325828dd435e28746ea51b0865a01637bdd19144d89c62b05e725b9bb4c4d7e2` |
| `python-descriptor-zip64.zip` | `a9a6e5c06faa8286dedeeac2791a00c1c50361e2ca333d9d68802c153494d913` |
