"""Manually regenerate deterministic ZIP interoperability fixtures (stdlib only)."""
import hashlib
import io
import json
from pathlib import Path
import zipfile


class NonSeekable(io.BytesIO):
    def seekable(self):
        return False

    def seek(self, *args):
        raise io.UnsupportedOperation("non-seekable fixture")


def main():
    root = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "zip"
    root.mkdir(parents=True, exist_ok=True)
    entries = {
        "manifest.json": b'{"format":{"id":"urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128","version":{"major":1,"minor":0}}}',
        "content/caf\u00e9.bin": bytes(range(256)) * 4,
        "generated/empty": b"",
    }
    expected = {}
    configurations = [
        ("python-stored.zip", zipfile.ZIP_STORED, False, False),
        ("python-deflate.zip", zipfile.ZIP_DEFLATED, False, False),
        ("python-descriptor.zip", zipfile.ZIP_DEFLATED, True, False),
        ("python-force-zip64.zip", zipfile.ZIP_DEFLATED, False, True),
        ("python-descriptor-zip64.zip", zipfile.ZIP_DEFLATED, True, True),
    ]
    for name, method, descriptor, force64 in configurations:
        buffer = NonSeekable() if descriptor else io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=method) as archive:
            for path, data in entries.items():
                info = zipfile.ZipInfo(path, date_time=(2020, 1, 1, 0, 0, 0))
                info.compress_type = method
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                with archive.open(info, "w", force_zip64=force64) as output:
                    output.write(data)
        payload = buffer.getvalue()
        (root / name).write_bytes(payload)
        expected[name] = {
            "archive_sha256": hashlib.sha256(payload).hexdigest(),
            "descriptor": descriptor,
            "entry_zip64": force64,
            "entries": {path: {"hex": data.hex(), "sha256": hashlib.sha256(data).hexdigest()} for path, data in entries.items()},
        }
    (root / "expected.json").write_text(json.dumps(expected, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    rows = "\n".join(f"| `{name}` | `{value['archive_sha256']}` |" for name, value in expected.items())
    (root / "README.md").write_text(
        "# ZIP: фикстуры Python zipfile\n\n"
        "Регенерация вручную: `python tools/fixtures/make_zip_fixtures.py` (стандартная библиотека Python). "
        "Тесты не запускают генератор. Дата заголовков фиксирована: 2020-01-01.\n\n"
        "Stored, Deflate, data descriptor при незаписываемом смещении, `force_zip64=True` для малых записей, "
        "ZIP64 с data descriptor. В каждом архиве одинаковые три записи: `manifest.json` (format.id, version 1.0), "
        "`content/café.bin` (байты 0…255, повторённые 4 раза), `generated/empty` (пусто). "
        "Точные байты и SHA-256 записей находятся в `expected.json`.\n\n"
        "| Архив | SHA-256 |\n|---|---|\n" + rows + "\n", encoding="utf-8")
    for name, value in expected.items():
        print(name, value["archive_sha256"])


if __name__ == "__main__":
    main()
