"""M1 CLI process integration tests; all data is synthetic and stays in .cache."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import unittest
import uuid
import zipfile
import zlib

CONFIG = None
def strict_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("Duplicate JSON key")
            result[key] = value
        return result
    def constant(value):
        raise ValueError("Non-JSON number: " + value)
    result = json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    if not isinstance(result, dict):
        raise ValueError("Expected one JSON object")
    return result

def write_zip(path, manifest, payloads):
    """Small independent Stored fixture writer: UTF-8, regular files, no extras."""
    entries = [("manifest.json", json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).encode())]
    entries += list(payloads.items())
    output, central = bytearray(), bytearray()
    for name, data in entries:
        encoded, offset = name.encode("utf-8"), len(output)
        crc = zlib.crc32(data)
        output += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0x800, 0, 0, 0, crc, len(data), len(data), len(encoded), 0)
        output += encoded + data
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, (3 << 8) | 20, 20, 0x800, 0, 0, 0, crc, len(data), len(data), len(encoded), 0, 0, 0, 0, 0o100644 << 16, offset)
        central += encoded
    start = len(output)
    output += central
    output += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries), len(central), start, 0)
    path.write_bytes(output)

class AssetCli(unittest.TestCase):
    def setUp(self):
        self.root = CONFIG.root / self._testMethodName / str(uuid.uuid4())
        self.root.mkdir(parents=True)
        self.files = [self.root / "деталь с пробелом.txt", self.root / "second.bin"]
        for path, data in zip(self.files, ["синтетические данные\n".encode(), b"\x00\x01\xff\x02"]):
            path.write_bytes(data)

    def run_am(self, *args, expected=0, json_output=False):
        command = [str(CONFIG.am), *map(str, args)]
        if json_output:
            command += ["--json"]
        result = subprocess.run(command, cwd=self.root, capture_output=True, encoding="utf-8", errors="strict", timeout=60)
        self.assertEqual(result.returncode, expected, (command, result.stdout, result.stderr))
        if json_output:
            value = strict_json(result.stdout)
            self.assertEqual(value["command"], args[1])
            return value
        return result

    def stage_clean(self):
        self.assertFalse(list(self.root.glob(".am-create-*")))

    def create(self):
        target = self.root / "asset with spaces.zip"
        if os.name == "nt":
            self.run_am("asset", "create", *self.files, "-o", target, "--title", "Тестовый asset", json_output=True)
        else:
            # docs/specifications/fs_support.md v0.2: non-Windows writes stay read_only_default.
            # Independent fixtures exercise reading without bypassing the production policy.
            asset_id, input_id = str(uuid.uuid4()), str(uuid.uuid4())
            timestamp = "2026-11-12T00:00:00Z"
            payloads, records = {}, []
            for source in self.files:
                name, data = "content/" + input_id + "/" + source.name, source.read_bytes()
                payloads[name] = data
                records.append(dict(id=str(uuid.uuid4()), storage_path=name, size_bytes=str(len(data)), sha256=hashlib.sha256(data).hexdigest(), origin_kind="user_added"))
            manifest = dict(format=dict(id="urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128", version=dict(major=1, minor=0)), asset=dict(id=asset_id, revision=1, created_at=timestamp, modified_at=timestamp, metadata=dict(title="Тестовый asset")), files=records, inputs=[dict(id=input_id, imported_at=timestamp, mode="strict", completeness="complete", objects=[], entries=[])], history=dict(entries=[dict(revision=1, operation_id=str(uuid.uuid4()), timestamp=timestamp, operation="asset_created")]))
            write_zip(target, manifest, payloads)
        self.stage_clean()
        return target

    def fixture(self, source, mode):
        with zipfile.ZipFile(source) as archive:
            manifest = json.loads(archive.read("manifest.json"))
            payloads = {entry.filename: archive.read(entry) for entry in archive.infolist() if entry.filename != "manifest.json"}
        if mode == "license":
            manifest["asset"].setdefault("metadata", {})["license"] = dict(spdx="MIT OR OR Apache-2.0")
        if mode == "units":
            field = "user:" + str(uuid.uuid4())
            manifest["field_definitions"] = [dict(id=field, schema_revision=1, name="Length", type="decimal", scopes=["asset"], dimension="length", constraints=dict(maximum=dict(value="1", unit="cm")))]
            manifest["asset"].setdefault("metadata", {})["custom_values"] = [dict(field_id=field, value="2", unit="mm")]
        if mode == "minor":
            manifest["format"]["version"]["minor"] = 1
        if mode == "hash":
            manifest["files"][0]["sha256"] = "0" * 64
        if mode == "bad-entry":
            payloads["../bad.bin"] = b"opaque"
        if mode == "bad-encoding":
            payloads["opaque.bin"] = b"opaque"
        target = self.root / (mode + ".zip")
        write_zip(target, manifest, payloads)
        if mode == "bad-encoding":
            target.write_bytes(target.read_bytes().replace(b"opaque.bin", b"\xffpaque.bin"))
        if mode == "corrupt":
            with zipfile.ZipFile(target) as archive:
                entry = archive.getinfo(manifest["files"][0]["storage_path"])
                offset = entry.header_offset + 30 + len(entry.filename.encode("utf-8"))
            data = bytearray(target.read_bytes())
            data[offset] ^= 1
            target.write_bytes(data)
        if mode == "unsupported-payload":
            name = manifest["files"][0]["storage_path"].encode("utf-8")
            with zipfile.ZipFile(target) as archive:
                offset = archive.getinfo(manifest["files"][0]["storage_path"]).header_offset
            data = bytearray(target.read_bytes())
            struct.pack_into("<H", data, offset + 8, 12)
            for position in range(len(data) - 46 - len(name) + 1):
                if data[position:position + 4] == b"PK\x01\x02" and data[position + 46:position + 46 + len(name)] == name:
                    struct.pack_into("<H", data, position + 10, 12)
            target.write_bytes(data)
        if mode == "manifest-crc":
            with zipfile.ZipFile(target) as archive:
                entry = archive.getinfo("manifest.json")
                offset = entry.header_offset + 30 + len(entry.filename.encode("utf-8"))
            data = bytearray(target.read_bytes())
            data[offset] ^= 1
            target.write_bytes(data)
        return target, manifest

    def test_create_roundtrip(self):
        if os.name != "nt":
            # fs_support.md v0.2: ext4/WSL have not passed the S5 filesystem trials.
            result = self.run_am("asset", "create", self.files[0], "-o", self.root / "unsupported.zip", expected=4, json_output=True)
            self.assertEqual(result["error"]["message_id"], "am.repo.storage_read_only")
            self.stage_clean()
            return
        target = self.create()
        inspection = self.run_am("asset", "inspect", target, json_output=True)
        self.assertEqual(inspection["file_count"], 2)
        self.assertEqual(inspection["title"], "Тестовый asset")
        self.assertEqual([f["size_bytes"] for f in inspection["files"]], [str(f.stat().st_size) for f in self.files])
        self.assertTrue(all(f["origin_kind"] == "user_added" for f in inspection["files"]))
        self.run_am("asset", "verify", target, "--level", "full")
        output = self.root / "извлечённые файлы"
        self.run_am("asset", "extract", target, "-d", output, "--manifest", json_output=True)
        for source, record in zip(self.files, inspection["files"]):
            self.assertEqual((output / record["path"]).read_bytes(), source.read_bytes())
        with zipfile.ZipFile(target) as archive:
            self.assertEqual((output / "manifest.json").read_bytes(), archive.read("manifest.json"))

    @unittest.skipUnless(os.name == "nt", "create requires a verified Windows fixed NTFS filesystem")
    def test_create_refusals(self):
        existing = self.root / "existing.zip"
        existing.write_bytes(b"keep")
        result = self.run_am("asset", "create", self.files[0], "-o", existing, expected=4, json_output=True)
        self.assertEqual(result["error"]["message_id"], "am.app.output_exists")
        self.assertEqual(existing.read_bytes(), b"keep")
        self.stage_clean()
        for mode in ["collision", "directory", "missing"]:
            target = self.root / (mode + ".zip")
            if mode == "collision":
                left, right = self.root / "one", self.root / "two"
                left.mkdir(); right.mkdir()
                first, second = left / "A.txt", right / "a.txt"
                first.write_bytes(b"a"); second.write_bytes(b"b")
                inputs = [first, second]
            else:
                inputs = [self.root if mode == "directory" else self.root / "missing"]
            result = self.run_am("asset", "create", *inputs, "-o", target, expected=4, json_output=True)
            self.assertIn("code", result["error"])
            if mode == "collision": self.assertEqual(result["error"]["message_id"], "am.repo.path_collision")
            if mode == "directory": self.assertEqual(result["error"]["message_id"], "am.app.directory_not_supported")
            self.assertFalse(target.exists())
            self.stage_clean()

    def test_validate_verify(self):
        target = self.create()
        self.run_am("asset", "validate", target, json_output=True)
        self.run_am("asset", "verify", target, json_output=True)
        bad, _ = self.fixture(target, "license")
        result = self.run_am("asset", "validate", bad, expected=1, json_output=True)
        self.assertEqual(result["v3_findings"][0]["rule"], "license_spdx")
        self.assertIn("license_spdx", self.run_am("asset", "validate", bad, expected=1).stderr)
        partial, _ = self.fixture(target, "units")
        self.assertEqual(self.run_am("asset", "verify", partial, expected=3, json_output=True)["v3"], "partial")
        corrupt, _ = self.fixture(target, "corrupt")
        self.run_am("asset", "validate", corrupt)
        self.run_am("asset", "verify", corrupt, expected=1)
        info = self.run_am("asset", "inspect", target, json_output=True)
        self.run_am("asset", "verify", target, "--level", "quick", expected=3)
        self.run_am("asset", "verify", target, "--level", "selective", "--file", info["files"][0]["id"], expected=3)
        self.run_am("asset", "verify", target, "--level", "selective", "--file", info["files"][0]["id"], "--file", info["files"][1]["id"])
        self.run_am("asset", "verify", target, "--level", "selective", expected=2)
        unsafe, _ = self.fixture(target, "bad-entry")
        findings = self.run_am("asset", "validate", unsafe, expected=1, json_output=True)["v3_findings"]
        self.assertTrue(any(f["rule"] == "zip_paths" and f["pointer"] == "" and f["detail"] == "../bad.bin" for f in findings))
        invalid_encoding, _ = self.fixture(target, "bad-encoding")
        findings = self.run_am("asset", "validate", invalid_encoding, expected=1, json_output=True)["v3_findings"]
        self.assertTrue(any(f["rule"] == "zip_paths" and f["detail"] == "\\xFFpaque.bin" for f in findings))
        result = self.run_am("asset", "verify", invalid_encoding, expected=1, json_output=True)
        self.assertTrue(any(stream["path"] == "\\xFFpaque.bin" for stream in result["v4"]))

    def test_inspect_minor(self):
        base = self.create()
        target, _ = self.fixture(base, "minor")
        result = self.run_am("asset", "inspect", target, expected=3, json_output=True)
        self.assertEqual(result["format_support"], "newer_minor")
        self.assertEqual(result["v3"], "not_performed")
        broken, _ = self.fixture(base, "manifest-crc")
        result = self.run_am("asset", "inspect", broken, expected=1, json_output=True)
        self.assertEqual(result["v1"], "failed")
        self.assertIn("code", result["error"])

    def test_extract(self):
        target = self.create()
        inspection = self.run_am("asset", "inspect", target, json_output=True)
        selected = self.root / "selected"
        self.run_am("asset", "extract", target, "-d", selected, "--file", inspection["files"][0]["id"], json_output=True)
        self.assertEqual(len(list(selected.rglob("*.txt"))), 1)
        self.assertFalse((selected / inspection["files"][1]["path"]).exists())
        both = self.root / "both-selected"
        self.run_am("asset", "extract", target, "-d", both, "--file", inspection["files"][0]["id"], "--file", inspection["files"][1]["id"])
        self.run_am("asset", "extract", target, "-d", selected, expected=4)
        unknown = self.root / "unknown"
        self.run_am("asset", "extract", target, "-d", unknown, "--file", str(uuid.uuid4()), expected=4, json_output=True)
        self.assertFalse(unknown.exists())
        mismatched, manifest = self.fixture(target, "hash")
        output = self.root / "mismatched"
        self.run_am("asset", "validate", mismatched)
        result = self.run_am("asset", "extract", mismatched, "-d", output, expected=1, json_output=True)
        self.assertEqual([f["id"] for f in result["failed"]], [manifest["files"][0]["id"]])
        self.assertEqual(result["error"]["message_id"], "am.app.extract_hash_mismatch")
        self.assertFalse((output / manifest["files"][0]["storage_path"]).exists())
        self.assertTrue((output / manifest["files"][1]["storage_path"]).exists())
        self.assertFalse(list(output.rglob(".am-extract-*")))
        unsupported, _ = self.fixture(target, "unsupported-payload")
        destination = self.root / "unsupported"
        result = self.run_am("asset", "extract", unsupported, "-d", destination, expected=4, json_output=True)
        self.assertIn("code", result["error"])
        self.assertIn("message_id", result["error"])
        self.assertEqual(len(result["failed"]), 1)
        self.assertFalse(list(destination.rglob(".am-extract-*")))

    def test_json_examples(self):
        target = self.create()
        for command in ["inspect", "validate", "verify"]:
            text = self.run_am("asset", command, target)
            value = self.run_am("asset", command, target, json_output=True)
            print("EXAMPLE_" + command.upper() + "_TEXT", text.stdout.replace(str(self.root), "<dir>"))
            print("EXAMPLE_" + command.upper() + "_JSON", json.dumps(value, ensure_ascii=False).replace(json.dumps(str(self.root))[1:-1], "<dir>"))
        if os.name == "nt":
            for fmt in [False, True]:
                output = self.root / ("example-json.zip" if fmt else "example-text.zip")
                result = self.run_am("asset", "create", *self.files, "-o", output, json_output=fmt)
                print("EXAMPLE_CREATE_" + ("JSON" if fmt else "TEXT"), (json.dumps(result, ensure_ascii=False) if fmt else result.stdout).replace(json.dumps(str(self.root))[1:-1] if fmt else str(self.root), "<dir>"))
        for fmt in [False, True]:
            output = self.root / ("extract-json" if fmt else "extract-text")
            result = self.run_am("asset", "extract", target, "-d", output, json_output=fmt)
            print("EXAMPLE_EXTRACT_" + ("JSON" if fmt else "TEXT"), (json.dumps(result, ensure_ascii=False) if fmt else result.stdout).replace(json.dumps(str(self.root))[1:-1] if fmt else str(self.root), "<dir>"))
        error = self.run_am("asset", "inspect", self.root / "missing.zip", expected=4, json_output=True)
        self.assertIn("code", error["error"])
        self.assertIn("message_id", error["error"])

    def test_development(self):
        self.assertRegex(self.run_am("version").stdout, r"am .*asset format")
        values = self.run_am("uuid", "--v4", "2").stdout.splitlines()
        self.assertEqual(len(values), 2)
        self.assertTrue(all(uuid.UUID(value).version == 4 for value in values))
        root = "content/00000000-0000-4000-8000-000000000001"
        self.assertEqual(self.run_am("zip-path", root, "part.stl").stdout, root + "/part.stl  [none]\n")
        self.assertIn("Development", self.run_am("--help").stdout)
        for command in ["create", "inspect", "validate", "verify", "extract"]:
            self.assertIn(command, self.run_am("asset", command, "--help").stdout)
        self.run_am("unknown-command", expected=2)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--am", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--case", required=True)
    CONFIG = parser.parse_args()
    CONFIG.am = CONFIG.am.resolve()
    CONFIG.root = CONFIG.root.resolve()
    suite = unittest.TestSuite([AssetCli("test_" + CONFIG.case)])
    print("requirements: K-15|K-85|K-3|K-6")
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
