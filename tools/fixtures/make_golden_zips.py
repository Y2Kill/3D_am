"""Build independent, deterministic golden ZIPs from raw manifests (stdlib only)."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile


import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from zipgen import pack_zip, patch_archive


def local_path(root, name):
    path = root / name
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Source path escapes case: {name}")
    return path


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
    data = pack_zip(config["entries"], lambda entry: manifest if entry.get("manifest") else payloads[entry["name"]],
                    descriptor=config.get("descriptor", False), zip64=config.get("zip64", False))
    data = patch_archive(data, config.get("patches", []))
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
