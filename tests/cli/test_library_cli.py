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
