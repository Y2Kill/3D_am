"""Library process checks (K-7, K-123, ADR-0007, ADR-0031), synthetic data only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
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
