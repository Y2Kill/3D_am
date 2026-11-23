"""Job process checks: K-115, K-121, NFR-14.4.3; synthetic libraries only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import signal
import sqlite3
import subprocess
import sys
import threading
import unittest
import uuid

CONFIG = None
WRITE_REASON = "Production writes require a verified fixed NTFS volume"


class JobsCli(unittest.TestCase):
    def setUp(self):
        self.work = CONFIG.root / str(uuid.uuid4())
        self.work.mkdir(parents=True)
        self.root = self.work / "library with spaces"
        self.am("library", "init", self.root)

    def am(self, *arguments, expected=0):
        run = subprocess.run([str(CONFIG.am), *map(str, arguments), "--json"],
                             cwd=self.work, capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(run.returncode, expected, (arguments, run.stdout, run.stderr))
        # json.loads rejects additional stdout records and trailing non-whitespace.
        result = json.loads(run.stdout)
        self.assertIsInstance(result, dict)
        return result

    def jobs(self, verb, *args, expected=0):
        result = self.am("jobs", verb, self.root, *args, expected=expected)
        self.assertEqual(result["command"], "jobs " + verb)
        return result

    def writable(self):
        if os.name != "nt":
            result = self.jobs("run", expected=4)
            self.assertEqual(result["error"]["message_id"], "am.library.storage_read_only")
            print(WRITE_REASON + "; executor and services use injected capabilities in unit tests")
            self.skipTest(WRITE_REASON)

    def enqueue(self, *args):
        return self.jobs("enqueue-dev", *args)["job_id"]

    def test_verify(self):
        self.writable()
        job = self.am("library", "verify", self.root, "--enqueue", "--full", "--no-trash")["job_id"]
        self.assertEqual([r["job_id"] for r in self.jobs("list")["jobs"]], [job])
        result = self.jobs("run", "--profile", "economy", "--jobs", "1")
        self.assertEqual((result["processed"], result["succeeded"], result["failed"]), (1, 1, 0))
        record = self.jobs("show", job)
        self.assertEqual(record["state"], "succeeded")
        self.assertEqual(record["params"], {"level": "full", "include_trash": False})
        self.assertEqual(set(record["result"]), {"total", "checked", "ok", "warn", "corrupt", "unsupported", "unreadable"})
        self.assertEqual(self.jobs("list")["jobs"], [])
        self.assertEqual(self.jobs("list", "--state", "succeeded")["jobs"][0]["job_id"], job)
        codes = [r["code"] for r in self.am("library", "log", self.root, "--limit", "100")["entries"]]
        for code in ("job.started", "library.verify", "job.finished"):
            self.assertIn(code, codes)
        self.assertEqual(self.jobs("run")["processed"], 0)

    def test_decision(self):
        self.writable()
        job = self.enqueue("--ask", "--steps", "5")
        self.assertEqual(self.jobs("run", expected=3)["attention"], 1)
        before = self.jobs("show", job)["decision"]
        self.jobs("answer", job, "invalid", expected=4)
        self.assertEqual(self.jobs("show", job)["decision"], before)
        self.jobs("answer", job, "continue")
        self.assertEqual(self.jobs("show", job)["decision"]["answer"], {"option": "continue"})
        self.assertEqual(self.jobs("run")["succeeded"], 1)
        self.assertIsNone(self.jobs("show", job)["decision"])

    def test_failure_and_controls(self):
        self.writable()
        failed = self.enqueue("--fail", "test.failure")
        healthy = self.enqueue()
        result = self.jobs("run", expected=1)
        self.assertEqual((result["failed"], result["succeeded"]), (1, 1))
        self.assertEqual(self.jobs("show", failed)["error"]["message_id"], "test.failure")
        self.assertEqual(self.jobs("show", healthy)["state"], "succeeded")
        self.jobs("retry", failed)
        self.jobs("cancel", failed)
        self.assertEqual(self.jobs("show", failed)["state"], "cancelled")
        a, b, c = (self.enqueue() for _ in range(3))
        self.jobs("pause", a)
        self.assertEqual(self.jobs("show", a)["attempt"], 0)
        self.jobs("resume", a)
        self.jobs("move", c, "--before", a)
        self.jobs("move", a, "--last")
        self.assertEqual([r["job_id"] for r in self.jobs("list")["jobs"]], [c, b, a])
        partial = self.jobs("pause", str(uuid.uuid4()), b, expected=4)
        self.assertEqual([r["ok"] for r in partial["results"]], [False, True])
        self.assertEqual(self.jobs("show", b)["state"], "paused")

    def test_locked(self):
        self.writable()
        job = self.enqueue()
        with (self.root / ".am/library.lock").open("w+b") as lock:
            lock.write(b'{"pid":123,"host":"process-test"}\n')
            lock.flush()
            if os.name == "nt":
                import msvcrt
                lock.seek(1 << 62)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.jobs("pause", job, expected=4)
            self.jobs("run", expected=4)
            self.assertEqual(self.jobs("list")["jobs"][0]["job_id"], job)
            self.assertEqual(self.jobs("show", job)["state"], "queued")

    def test_read_only_and_media(self):
        media = self.am("media", self.work)
        self.assertTrue(media["device_id"])
        self.assertIn(media["kind"], ("hdd", "ssd", "network", "removable", "unknown"))
        self.assertEqual(self.jobs("list")["jobs"], [])
        # Read-only inspection must preserve arbitrary valid number lexemes.
        job = str(uuid.uuid4())
        params = '{"huge":1e9999,"integer":184467440737095516160}'
        with sqlite3.connect(self.root / ".am/state.sqlite") as db:
            db.execute("INSERT INTO jobs(job_id,type,state,params_json,priority,position,created_at) "
                       "VALUES(?, 'test.opaque','queued',?,0,1,'2026-01-01T00:00:00Z')", (job, params))
        before = hashlib.sha256((self.root / ".am/state.sqlite").read_bytes()).digest()
        shown = self.jobs("show", job)
        self.assertEqual(shown["params"]["integer"], 184467440737095516160)
        raw = subprocess.run([str(CONFIG.am), "jobs", "show", str(self.root), job, "--json"],
                             capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(raw.returncode, 0, raw.stderr)
        self.assertIn("1e9999", raw.stdout)
        self.assertEqual(hashlib.sha256((self.root / ".am/state.sqlite").read_bytes()).digest(), before)
        if os.name != "nt":
            self.jobs("run", expected=4)

    def test_signal(self):
        self.writable()
        job = self.enqueue("--steps", "1000000000000")
        command = [str(CONFIG.am), "jobs", "run", str(self.root), "--json"]
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   encoding="utf-8", creationflags=flags)
        lines = []
        ready = queue.Queue()

        def read_events():
            for line in process.stderr:
                lines.append(line)
                if "Job started:" in line:
                    ready.put(True)
            ready.put(False)

        reader = threading.Thread(target=read_events)
        reader.start()
        try:
            self.assertTrue(ready.get(timeout=30), "Runner exited before its readiness event")
            process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
            process.wait(timeout=30)
            stdout = process.stdout.read()
            reader.join(timeout=30)
            self.assertFalse(reader.is_alive())
            self.assertEqual(process.returncode, 3, (stdout, lines))
            result = json.loads(stdout)
            self.assertTrue(result["interrupted"])
            self.assertIn("waiting for running jobs to reach a safe point", "".join(lines))
            record = self.jobs("show", job)
            self.assertEqual((record["state"], record["attempt"]), ("queued", 0))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=15)
            reader.join(timeout=15)
            process.stdout.close()
            process.stderr.close()


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--am", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--case")
    CONFIG = parser.parse_args()
    CONFIG.am = CONFIG.am.resolve()
    CONFIG.root = CONFIG.root.resolve()
    unittest.main(argv=[__file__] + (["JobsCli.test_" + CONFIG.case] if CONFIG.case else []))
