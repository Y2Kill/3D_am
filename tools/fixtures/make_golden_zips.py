"""Build independent, deterministic golden ZIPs from raw manifests (stdlib only)."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import tempfile
import zipfile


class Utf8Info(zipfile.ZipInfo):
    def _encodeFilenameFlags(self):
        # The container profile requires bit 11 even for ASCII entry names.
        return self.filename.encode("utf-8"), self.flag_bits | 0x800


class NonSeekable(io.BytesIO):
    def seekable(self):
        return False

    def seek(self, *args):
        raise io.UnsupportedOperation("descriptor fixture")


def local_path(root, name):
    path = root / name
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Source path escapes case: {name}")
    return path


def patch_archive(data, patches):
    """Apply declared local/central header patches, never search payload bytes."""
    data = bytearray(data)
    eocd = data.rfind(b"PK\x05\x06")
    central = struct.unpack_from("<I", data, eocd + 16)[0]
    records = []
    while data[central:central + 4] == b"PK\x01\x02":
        local = struct.unpack_from("<I", data, central + 42)[0]
        lengths = struct.unpack_from("<HHH", data, central + 28)
        records.append((local, central))
        central += 46 + sum(lengths)
    for patch in patches:
        if patch["kind"] == "truncate":
            del data[patch["size"]:]
            continue
        local, central = records[patch["entry"]]
        kind = patch["kind"]
        if kind == "name":
            name = patch["value"].encode("utf-8")
            for start, length_offset, fixed in [(local, 26, 30), (central, 28, 46)]:
                length = struct.unpack_from("<H", data, start + length_offset)[0]
                if len(name) != length:
                    raise ValueError("Name patches must preserve encoded length")
                data[start + fixed:start + fixed + length] = name
        elif kind in ("method", "flags", "crc"):
            offsets, fmt = {"method": ((8, 10), "<H"), "flags": ((6, 8), "<H"),
                            "crc": ((14, 16), "<I")}[kind]
            for start, offset in zip((local, central), offsets):
                struct.pack_into(fmt, data, start + offset, patch["value"])
        elif kind == "external_attr":
            struct.pack_into("<I", data, central + 38, patch["value"])
        else:
            raise ValueError(f"Unknown patch kind: {kind}")
    return bytes(data)


def build_case(source, target):
    config = json.loads((source / "case.json").read_text(encoding="utf-8"))
    payloads = {entry["name"]: local_path(source, entry["source"]).read_bytes()
                for entry in config["entries"] if "source" in entry}
    manifest = (source / "manifest.json").read_bytes()
    for name, payload in payloads.items():
        manifest = manifest.replace(("@sha256:" + name).encode(), hashlib.sha256(payload).hexdigest().encode())
        manifest = manifest.replace(("@size:" + name).encode(), str(len(payload)).encode())
    if b"@sha256:" in manifest or b"@size:" in manifest:
        raise ValueError(f"{source.name}: unresolved manifest substitution")
    buffer = NonSeekable() if config.get("descriptor", False) else io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for entry in config["entries"]:
            payload = manifest if entry.get("manifest") else payloads[entry["name"]]
            info = Utf8Info(entry["name"], (2020, 1, 1, 0, 0, 0))
            info.compress_type = {"stored": zipfile.ZIP_STORED, "deflate": zipfile.ZIP_DEFLATED}[entry.get("method", "stored")]
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.extra = bytes.fromhex(entry.get("extra_hex", ""))
            info.comment = entry.get("comment", "").encode("utf-8")
            with archive.open(info, "w", force_zip64=config.get("zip64", False)) as stream:
                stream.write(payload)
    data = patch_archive(buffer.getvalue(), config.get("patches", []))
    if len(data) > 65536:
        raise ValueError(f"{source.name}: ZIP exceeds the 64 KiB repository-size hypothesis")
    target.mkdir(parents=True, exist_ok=True)
    (target / "asset.zip").write_bytes(data)
    shutil.copyfile(source / "expected.json", target / "expected.json")
    return json.loads((source / "expected.json").read_text(encoding="utf-8"))


def generate(sources, output):
    cases = sorted(p.parent for p in sources.glob("*/case.json"))
    if not cases:
        raise ValueError(f"No source cases in {sources}")
    for source in cases:
        build_case(source, output / source.name)
    return {p.relative_to(output).as_posix(): p.read_bytes() for p in output.glob("*/*") if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    output = args.output or args.root / "tests/fixtures/golden"
    sources = args.root / "tools/fixtures/golden"
    if args.check:
        cache = args.root / ".cache"
        cache.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="golden-regen-", dir=cache) as temporary:
            actual = generate(sources, Path(temporary))
            expected = {p.relative_to(output).as_posix(): p.read_bytes() for p in output.glob("*/*") if p.is_file()}
            different = sorted(key for key in actual.keys() | expected.keys() if actual.get(key) != expected.get(key))
            for key in different:
                print(f"FAIL golden_regen_check: {key}")
            if different:
                return 1
            print(f"PASS golden_regen_check: {len(actual) // 2} cases, byte-identical")
    else:
        files = generate(sources, output)
        print(f"Generated {len(files) // 2} cases in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
