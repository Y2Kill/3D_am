"""Run the C++ fixture generator, then independently check every written ZIP."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--generator", type=Path)
    args = parser.parse_args()
    if args.generator:
        subprocess.run([str(args.generator), "--gtest_filter=ZipWriterInterop.*"], check=True)
    expected = json.loads((args.directory / "expected.json").read_text(encoding="utf-8"))
    actual_archives = {path.name for path in args.directory.glob("*.zip")}
    if actual_archives != set(expected):
        raise AssertionError("ZIP file set differs from expectations")
    count = 0
    for name, entries in expected.items():
        with zipfile.ZipFile(args.directory / name) as archive:
            info = archive.infolist()
            if len(info) != len(entries) or set(archive.namelist()) != set(entries):
                raise AssertionError(f"{name}: entry names differ")
            if archive.testzip() is not None:
                raise AssertionError(f"{name}: CRC verification failed")
            for entry in info:
                if not (entry.flag_bits & 0x800) or entry.flag_bits & 8:
                    raise AssertionError(f"{name}/{entry.filename}: invalid flags")
                if entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                    raise AssertionError(f"{name}/{entry.filename}: invalid compression method")
                if hashlib.sha256(archive.read(entry)).hexdigest() != entries[entry.filename]:
                    raise AssertionError(f"{name}/{entry.filename}: SHA-256 differs")
                count += 1
    print(f"zip_writer_interop: PASS ({len(expected)} archives, {count} entries)")


if __name__ == "__main__":
    main()
