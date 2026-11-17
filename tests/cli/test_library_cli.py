"""Library process checks (K-7, K-123, ADR-0007, ADR-0031), synthetic data only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import unittest
import uuid
import zipfile
from datetime import datetime, timedelta

CONFIG = None


class LibraryCli(unittest.TestCase):
    def setUp(self):
        self.work = CONFIG.root / str(uuid.uuid4())
        self.work.mkdir(parents=True)
        self.root = self.work / ("библиотека с пробелом #%" + ("?" if os.name != "nt" else ""))

    def run_am(self, verb, expected=0, *extra):
        command = [str(CONFIG.am), "library", verb, str(self.root), *extra, "--json"]
        run = subprocess.run(command, cwd=self.work, capture_output=True, encoding="utf-8", errors="strict", timeout=60)
        self.assertEqual(run.returncode, expected, (command, run.stdout, run.stderr))
        value = json.loads(run.stdout)
        self.assertIsInstance(value, dict)
        self.assertEqual(value["command"], "library " + verb)
        return value

    def snapshot(self):
        result = {}
        for path in self.root.rglob("*"):
            if path.is_file():
                relative = path.relative_to(self.root)
                if str(relative).replace("\\", "/") in {
                    f".am/{db}.sqlite{suffix}" for db in ("index", "userdata", "state") for suffix in ("-wal", "-shm")
                }:
                    continue
                result[str(relative)] = (path.stat().st_size, hashlib.sha256(path.read_bytes()).hexdigest())
        return result

    def test_init_info(self):
        created = self.run_am("init", 0, "--name", "Моя библиотека")
        self.assertEqual(created["name"], "Моя библиотека")
        self.assertEqual(uuid.UUID(created["library_id"]).version, 7)
        before = self.snapshot()
        info = self.run_am("info")
        self.assertEqual(info["library_id"], created["library_id"])
        self.assertEqual(info["mode"], "read_only")
        self.assertEqual(info["owner"], "free")
        self.assertEqual(info["schema_version"], 1)
        for name in ("index", "userdata", "state"):
            self.assertEqual(info["databases"][name]["state"], "ok")
            self.assertEqual(info["databases"][name]["user_version"], 1)
            self.assertIn(info["databases"][name]["read_mode"], ("wal", "immutable"))
        self.assertTrue(info["databases"]["index"]["rebuild_required"])
        self.assertIn("writable", info["capabilities"])
        self.assertIn("write_unavailable_reason", info)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.root / ".am/library.lock").exists())
        text = subprocess.run([str(CONFIG.am), "library", "info", str(self.root)], capture_output=True, encoding="utf-8")
        self.assertEqual(text.returncode, 0, text.stderr)
        self.assertIn("Library ID:", text.stdout)
        self.assertIn(created["library_id"], text.stdout)

    def test_refusals(self):
        self.root.mkdir()
        self.assertEqual(self.run_am("info", 4)["error"]["message_id"], "am.library.not_found")
        (self.root / "existing").write_text("data")
        self.assertEqual(self.run_am("init", 4)["error"]["message_id"], "am.library.not_empty")
        (self.root / "existing").unlink()
        self.run_am("init")
        self.assertEqual(self.run_am("init", 4)["error"]["message_id"], "am.library.not_empty")
        (self.root / "am-library.toml").write_text("invalid = [", encoding="utf-8")
        self.assertEqual(self.run_am("info", 1)["error"]["message_id"], "am.library.config_invalid")

    def test_missing_databases(self):
        self.run_am("init")
        for name in ("index", "userdata", "state"):
            (self.root / f".am/{name}.sqlite").unlink()
        before = self.snapshot()
        info = self.run_am("info")
        for db in info["databases"].values():
            self.assertEqual(db["state"], "missing")
            self.assertIsNone(db["user_version"])
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(list((self.root / ".am").glob("*.sqlite*")), [])
        (self.root / ".am/state.sqlite").mkdir()
        unavailable = self.run_am("info")["databases"]["state"]
        self.assertEqual(unavailable["state"], "unavailable")
        self.assertEqual(unavailable["reason"], "open_failed")
        self.assertTrue(unavailable["detail"])

    def test_owner_lock(self):
        self.run_am("init")
        path = self.root / ".am/library.lock"
        with path.open("w+b") as lock:
            lock.write(b'{"pid":123,"host":"process-test"}\n')
            lock.flush()
            if os.name == "nt":
                import msvcrt
                lock.seek(1 << 62)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            info = self.run_am("info")
            self.assertEqual(info["owner"], "locked")
            self.assertIn("process-test", info["owner_detail"])
        self.assertEqual(self.run_am("info")["owner"], "free")

    def test_add_ready(self):
        self.run_am("init")
        bad_arguments = self.run_am("add-ready", 2, "first.zip", "second.zip", "--replace", "0" * 64)
        self.assertEqual(bad_arguments["error"]["message_id"], "am.repo.invalid_operation")
        if os.name != "nt":
            refused = self.run_am("add-ready", 4, "source.zip")
            self.assertEqual(refused["error"]["message_id"], "am.library.storage_read_only")
            reason = "Production writes require a verified fixed NTFS volume; service add-ready is tested with injected capabilities"
            print(reason)
            self.skipTest(reason)
        self.run_am("rebuild")
        source = self.work / "ready-source.bin"
        source.write_bytes(b"ready asset payload")
        ready = self.work / "ready.zip"
        created = subprocess.run([str(CONFIG.am), "asset", "create", str(source), "-o", str(ready), "--title", "Ready original", "--json"], capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(created.returncode, 0, (created.stdout, created.stderr))
        foreign = self.work / "foreign.zip"
        with zipfile.ZipFile(foreign, "w") as archive:
            archive.writestr("source.txt", b"ordinary ZIP")
        batch = self.run_am("add-ready", 4, str(ready), str(ready), str(foreign))
        self.assertEqual([r["status"] for r in batch["results"]], ["imported", "identical", "failed"])
        first, identical, rejected = batch["results"]
        self.assertEqual(first["classification"], "new")
        self.assertTrue(identical["zip_bytes_identical"])
        self.assertEqual(rejected["classification"], "not_own")
        self.assertIsNone(rejected["asset_id"])
        self.assertEqual(rejected["error"]["message_id"], "am.repo.not_own_format")
        asset_id = first["asset_id"]
        destination = self.root / "assets" / asset_id[-2:] / (asset_id + ".zip")
        self.assertEqual(destination.read_bytes(), ready.read_bytes())
        with zipfile.ZipFile(ready) as archive:
            contents = {info.filename: archive.read(info) for info in archive.infolist()}
        manifest = json.loads(contents["manifest.json"])
        modified = datetime.fromisoformat(manifest["asset"]["modified_at"].replace("Z", "+00:00")) + timedelta(seconds=1)
        timestamp = modified.isoformat(timespec="milliseconds").replace("+00:00", "Z")
        manifest["asset"]["revision"] = 2
        manifest["asset"]["modified_at"] = timestamp
        manifest["asset"]["metadata"]["title"] = "Ready newer"
        manifest["history"]["entries"].append({
            "revision": 2, "operation_id": str(uuid.uuid4()), "timestamp": timestamp,
            "operation": "metadata_changed", "tool": {"application_version": "0.1.0-test"},
            "changes": [{"kind": "metadata_set", "target": "asset", "field": "title"}], "details": {}})
        contents["manifest.json"] = json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        class Utf8Info(zipfile.ZipInfo):
            def _encodeFilenameFlags(self):
                return self.filename.encode("utf-8"), self.flag_bits | 0x800
        newer = self.work / "newer.zip"
        with zipfile.ZipFile(newer, "w") as archive:
            for name, data in contents.items():
                info = Utf8Info(name)
                info.create_system = 0
                info.external_attr = 0x20
                archive.writestr(info, data)
        valid = subprocess.run([str(CONFIG.am), "asset", "validate", str(newer), "--json"], capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(valid.returncode, 0, (valid.stdout, valid.stderr))
        current_bytes = destination.read_bytes()
        conflict = self.run_am("add-ready", 4, str(newer))["results"][0]
        self.assertEqual(conflict["status"], "conflict")
        self.assertEqual(conflict["classification"], "newer_revision")
        self.assertEqual((conflict["current"]["revision"], conflict["incoming"]["revision"]), (1, 2))
        self.assertEqual(destination.read_bytes(), current_bytes)
        userdata = hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest()
        replaced = self.run_am("add-ready", 0, str(newer), "--replace", conflict["current"]["manifest_sha256"], "--verbose")["results"][0]
        self.assertEqual(replaced["status"], "replaced")
        self.assertIn("verification", replaced)
        self.assertEqual(destination.read_bytes(), newer.read_bytes())
        self.assertEqual(self.run_am("list")["assets"][0]["revision"], 2)
        self.assertEqual(hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest(), userdata)

    def test_copy(self):
        self.run_am("init")
        if os.name != "nt":
            refused = self.run_am("copy", 4, str(uuid.uuid4()))
            self.assertEqual(refused["error"]["message_id"], "am.library.storage_read_only")
            reason = "Production writes require a verified fixed NTFS volume; service copy is tested with injected capabilities"
            print(reason)
            self.skipTest(reason)
        self.run_am("rebuild")
        payload = self.work / "copy-source.bin"
        payload.write_bytes(b"independent copy payload")
        ready = self.work / "ready-copy.zip"
        created = subprocess.run([str(CONFIG.am), "asset", "create", str(payload), "-o", str(ready), "--json"],
                                 capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(created.returncode, 0, (created.stdout, created.stderr))
        source_id = self.run_am("add-ready", 0, str(ready))["results"][0]["asset_id"]
        source_path = self.root / "assets" / source_id[-2:] / (source_id + ".zip")
        before = source_path.read_bytes()
        userdata = hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest()
        copied = self.run_am("copy", 0, source_id)
        (CONFIG.root / "t021-copy-success.json").write_text(json.dumps(copied, ensure_ascii=False), encoding="utf-8")
        self.assertNotEqual(copied["asset_id"], source_id)
        self.assertEqual(copied["source"], {"asset_id": source_id, "revision": 1})
        self.assertEqual(copied["revision"], 1)
        self.assertEqual(len(copied["files"]), 1)
        self.assertIsNone(copied["error"])
        listed = self.run_am("list")["assets"]
        self.assertEqual({row["asset_id"] for row in listed}, {source_id, copied["asset_id"]})
        destination = self.root / "assets" / copied["asset_id"][-2:] / (copied["asset_id"] + ".zip")
        verified = subprocess.run([str(CONFIG.am), "asset", "verify", str(destination), "--json"],
                                  capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(verified.returncode, 0, (verified.stdout, verified.stderr))
        self.assertEqual(source_path.read_bytes(), before)
        self.assertEqual(hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest(), userdata)
        invalid = self.run_am("copy", 2, "invalid-id")
        self.assertEqual(invalid["error"]["message_id"], "am.repo.invalid_operation")
        with zipfile.ZipFile(source_path) as archive:
            contents = {info.filename: archive.read(info) for info in archive.infolist()}
        manifest = json.loads(contents["manifest.json"])
        manifest["extensions"] = {"org.test.copy": {"schema_version": "1", "data": {}}}
        contents["manifest.json"] = json.dumps(manifest, ensure_ascii=False).encode("utf-8")

        class Utf8Info(zipfile.ZipInfo):
            def _encodeFilenameFlags(self):
                return self.filename.encode("utf-8"), self.flag_bits | 0x800

        with zipfile.ZipFile(source_path, "w") as archive:
            for name, data in contents.items():
                info = Utf8Info(name)
                info.create_system = 0
                info.external_attr = 0x20
                archive.writestr(info, data)
        source_before_refusal = source_path.read_bytes()
        refused = self.run_am("copy", 4, source_id, "--verbose")
        (CONFIG.root / "t021-copy-refusal.json").write_text(json.dumps(refused, ensure_ascii=False), encoding="utf-8")
        self.assertEqual(refused["error"]["message_id"], "am.repo.copy_extensions_unsupported")
        self.assertEqual(source_path.read_bytes(), source_before_refusal)
        self.assertEqual(len(self.run_am("list")["assets"]), 2)
        self.assertEqual(list((self.root / ".am/.am-tmp").iterdir()), [])
        del manifest["extensions"]
        old_path = manifest["files"][0]["storage_path"]
        unsafe_path = old_path.rsplit("/", 1)[0] + "/unsafe?.bin"
        manifest["files"][0]["storage_path"] = unsafe_path
        contents[unsafe_path] = contents.pop(old_path)
        contents["manifest.json"] = json.dumps(manifest, ensure_ascii=False).encode("utf-8")
        with zipfile.ZipFile(source_path, "w") as archive:
            for name, data in contents.items():
                info = Utf8Info(name)
                info.create_system = 0
                info.external_attr = 0x20
                archive.writestr(info, data)
        rejected_path = self.run_am("copy", 4, source_id)
        self.assertEqual(rejected_path["error"]["message_id"], "am.repo.invalid_operation")
        self.assertEqual(len(self.run_am("list")["assets"]), 2)

    def test_trash(self):
        self.run_am("init")
        self.assertIsNone(self.run_am("info")["trash"])
        refused = self.run_am("purge", 2, "--all")
        self.assertEqual(refused["error"]["message_id"], "am.cli.purge_confirmation_required")
        if os.name != "nt":
            refused = self.run_am("trash", 4, str(uuid.uuid4()))
            self.assertEqual(refused["error"]["message_id"], "am.library.storage_read_only")
            reason = "Production writes require a verified fixed NTFS volume; service trash is tested with injected capabilities"
            print(reason)
            self.skipTest(reason)
        self.run_am("rebuild")
        ids, originals, sizes = [], {}, {}
        for number in range(2):
            source = self.work / f"trash-source-{number}.bin"
            source.write_bytes(bytes([number]) * 23)
            output = self.work / f"trash-asset-{number}.zip"
            created = subprocess.run([str(CONFIG.am), "asset", "create", str(source), "-o", str(output), "--title", f"Trash {number}", "--json"], capture_output=True, encoding="utf-8", timeout=60)
            self.assertEqual(created.returncode, 0, (created.stdout, created.stderr))
            inspected = subprocess.run([str(CONFIG.am), "asset", "inspect", str(output), "--json"], capture_output=True, encoding="utf-8", timeout=60)
            self.assertEqual(inspected.returncode, 0, inspected.stderr)
            asset_id = json.loads(inspected.stdout)["asset_id"]
            destination = self.root / "assets" / asset_id[-2:] / (asset_id + ".zip")
            destination.parent.mkdir(exist_ok=True)
            shutil.copyfile(output, destination)
            ids.append(asset_id)
            originals[asset_id] = hashlib.sha256(output.read_bytes()).hexdigest()
            sizes[asset_id] = output.stat().st_size
        self.assertEqual(self.run_am("sync")["added"], 2)
        userdata = hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest()
        moved = self.run_am("trash", 0, *ids)
        self.assertEqual([row["asset_id"] for row in moved["results"]], ids)
        self.assertEqual([row["status"] for row in moved["results"]], ["done", "done"])
        self.assertTrue(all(row["error"] is None for row in moved["results"]))
        self.assertEqual(self.run_am("list")["assets"], [])
        self.assertTrue(all(row["in_trash"] for row in self.run_am("list", 0, "--trash")["assets"]))
        self.assertEqual(self.run_am("info")["trash"], {"assets": 2, "bytes": str(sum(sizes.values()))})
        for asset_id in ids:
            self.assertEqual(hashlib.sha256((self.root / "trash" / (asset_id + ".zip")).read_bytes()).hexdigest(), originals[asset_id])
        self.assertEqual(self.run_am("restore", 0, ids[0])["results"][0]["status"], "done")
        self.assertEqual(self.run_am("info")["trash"], {"assets": 1, "bytes": str(sizes[ids[1]])})
        self.assertEqual(self.run_am("purge", 2, ids[1])["error"]["message_id"], "am.cli.purge_confirmation_required")
        self.assertTrue((self.root / "trash" / (ids[1] + ".zip")).exists())
        foreign = self.root / "trash/foreign.zip"
        foreign.write_bytes(b"unrelated file")
        foreign_directory = self.root / "trash" / (str(uuid.uuid4()) + ".zip")
        foreign_directory.mkdir()
        purged = self.run_am("purge", 0, "--all", "--yes")
        self.assertEqual(purged["results"], [{"asset_id": ids[1], "status": "done", "error": None}])
        self.assertEqual(foreign.read_bytes(), b"unrelated file")
        self.assertTrue(foreign_directory.is_dir())
        missing = str(uuid.uuid4())
        mixed = self.run_am("trash", 4, missing, ids[0])
        self.assertEqual([row["status"] for row in mixed["results"]], ["failed", "done"])
        self.assertEqual(mixed["results"][0]["error"]["message_id"], "am.repo.asset_not_found")
        plain = subprocess.run([str(CONFIG.am), "library", "restore", str(self.root), ids[0]], capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(plain.returncode, 0, plain.stderr)
        self.assertIn(ids[0], plain.stdout)
        self.assertIn("done", plain.stdout)
        self.assertEqual(hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest(), userdata)

    def test_sync(self):
        self.run_am("init")
        if os.name != "nt":
            refused = self.run_am("sync", 4)
            self.assertEqual(refused["error"]["message_id"], "am.library.storage_read_only")
            reason = "Production writes require a verified fixed NTFS volume; service sync is tested with injected capabilities"
            print(reason)
            self.skipTest(reason)
        refused = self.run_am("sync", 4)
        self.assertEqual(refused["error"]["message_id"], "am.catalog.rebuild_required")
        source = self.work / "source.bin"
        source.write_bytes(b"synthetic sync payload")
        output = self.work / "asset.zip"
        created = subprocess.run([str(CONFIG.am), "asset", "create", str(source), "-o", str(output), "--title", "Sync title", "--json"], capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(created.returncode, 0, (created.stdout, created.stderr))
        inspected = subprocess.run([str(CONFIG.am), "asset", "inspect", str(output), "--json"], capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(inspected.returncode, 0, inspected.stderr)
        asset_id = json.loads(inspected.stdout)["asset_id"]
        destination = self.root / "assets" / asset_id[-2:] / (asset_id + ".zip")
        destination.parent.mkdir(exist_ok=True)
        shutil.copyfile(output, destination)
        self.run_am("rebuild")
        userdata = hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest()
        report = self.run_am("sync")
        self.assertEqual((report["added"], report["updated"], report["unchanged"]), (0, 0, 1))
        self.assertEqual(report["missing"], [])
        self.assertEqual(report["skipped"], [])
        self.assertEqual(report["ignored"], [])
        self.assertFalse(report["full"])
        self.assertGreaterEqual(report["duration_ms"], 0)
        full = self.run_am("sync", 0, "--full", "--verbose")
        self.assertTrue(full["full"])
        self.assertEqual(full["updated"], 1)
        destination.unlink()
        missing = self.run_am("sync")
        self.assertEqual(missing["missing"], [asset_id])
        shutil.copyfile(output, destination)
        self.assertEqual(self.run_am("sync")["updated"], 1)
        destination.unlink()
        plain = subprocess.run([str(CONFIG.am), "library", "sync", str(self.root)], capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(plain.returncode, 0, plain.stderr)
        self.assertIn("missing", plain.stderr.lower())
        listed = self.run_am("list")["assets"]
        self.assertEqual(len(listed), 1)
        self.assertEqual(listed[0]["health"], "missing")
        self.assertEqual(listed[0]["title"], "Sync title")
        repeated = self.run_am("sync")
        self.assertEqual(repeated["missing"], [])
        self.assertEqual(repeated["unchanged"], 0)
        shutil.copyfile(output, destination)
        self.assertEqual(self.run_am("sync")["updated"], 1)
        destination.write_bytes(b"corrupt")
        failed_file = self.run_am("sync")
        self.assertEqual(len(failed_file["skipped"]), 1)
        self.assertEqual(set(failed_file["skipped"][0]), {"path", "reason", "message_id"})
        self.assertEqual(hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest(), userdata)

    def test_rebuild_list(self):
        self.run_am("init")
        initial = self.run_am("list")
        self.assertEqual(initial["assets"], [])
        self.assertTrue(initial["rebuild_required"])
        self.assertEqual(self.run_am("info")["indexed_assets"], 0)
        if os.name != "nt":
            refused = self.run_am("rebuild", 4)
            self.assertEqual(refused["error"]["message_id"], "am.library.storage_read_only")
            reason = "Production writes require a verified fixed NTFS volume; service rebuild is tested with injected capabilities"
            print(reason)
            self.skipTest(reason)
        rebuilt = self.run_am("rebuild")
        self.assertEqual(rebuilt["indexed"], 0)
        self.assertEqual(rebuilt["skipped"], [])
        self.assertEqual(rebuilt["ignored"], [])
        self.assertGreaterEqual(rebuilt["duration_ms"], 0)
        for number in range(2):
            source = self.work / f"source-{number}.bin"
            source.write_bytes(bytes([number]) * 19)
            output = self.work / f"asset-{number}.zip"
            created = subprocess.run([str(CONFIG.am), "asset", "create", str(source), "-o", str(output), "--title", f"Indexed {number}", "--json"], capture_output=True, encoding="utf-8", timeout=60)
            self.assertEqual(created.returncode, 0, (created.stdout, created.stderr))
            inspected = subprocess.run([str(CONFIG.am), "asset", "inspect", str(output), "--json"], capture_output=True, encoding="utf-8", timeout=60)
            self.assertEqual(inspected.returncode, 0, inspected.stderr)
            asset_id = json.loads(inspected.stdout)["asset_id"]
            destination = self.root / "assets" / asset_id[-2:] / (asset_id + ".zip")
            destination.parent.mkdir(exist_ok=True)
            shutil.copyfile(output, destination)
        before = self.snapshot()
        userdata_before = hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest()
        rebuilt = self.run_am("rebuild")
        self.assertEqual(rebuilt["indexed"], 2)
        listed = self.run_am("list")
        self.assertFalse(listed["rebuild_required"])
        self.assertEqual({row["title"] for row in listed["assets"]}, {"Indexed 0", "Indexed 1"})
        for row in listed["assets"]:
            self.assertEqual(row["revision"], 1)
            self.assertEqual(row["content_bytes"], "19")
            self.assertEqual(row["file_count"], 1)
            self.assertEqual(row["health"], "ok")
            self.assertFalse(row["in_trash"])
        self.assertEqual(self.run_am("info")["indexed_assets"], 2)
        after = self.snapshot()
        for path, data in before.items():
            if path.endswith(".zip") or path == "am-library.toml":
                self.assertEqual(after[path], data)
        self.assertEqual(hashlib.sha256((self.root / ".am/userdata.sqlite").read_bytes()).hexdigest(), userdata_before)
        self.assertEqual(self.run_am("list", 0, "--trash")["assets"], listed["assets"])
        plain = subprocess.run([str(CONFIG.am), "library", "list", str(self.root)], capture_output=True, encoding="utf-8")
        self.assertEqual(plain.returncode, 0, plain.stderr)
        self.assertIn("Indexed 0", plain.stdout)


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--am", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--case", required=True)
    CONFIG = parser.parse_args()
    CONFIG.am = CONFIG.am.resolve()
    CONFIG.root = CONFIG.root.resolve()
    unittest.main(argv=[__file__, "LibraryCli.test_" + CONFIG.case])
