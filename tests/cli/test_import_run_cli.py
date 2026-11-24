"""Durable import execution through the real CLI (SC-1/2, K-5/115/121)."""
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


class ImportRunCli(unittest.TestCase):
    def setUp(self):
        self.work = CONFIG.root / str(uuid.uuid4())
        self.source = self.work / 'source'
        self.source.mkdir(parents=True)
        self.library = self.work / 'library'
        self.plan = self.work / 'plan.json'
        self.env = os.environ.copy()
        self.env.pop('AM_LOG_DIR', None)
        self.env.pop('AM_LOG_LEVEL', None)
        profile = self.work / 'profile'
        profile.mkdir()
        for key in ('HOME', 'USERPROFILE', 'LOCALAPPDATA', 'XDG_STATE_HOME', 'TMP', 'TEMP', 'TMPDIR'):
            self.env[key] = str(profile)

    def am(self, *args, code=0):
        result = subprocess.run([str(CONFIG.am), *map(str, args)], cwd=self.work, env=self.env,
                                capture_output=True, text=True, encoding='utf8', timeout=120)
        self.assertEqual(result.returncode, code, (result.stdout, result.stderr))
        return json.loads(result.stdout)

    def prepare(self):
        if os.name != 'nt':
            self.skipTest('Import writes require verified fixed NTFS; Linux/DrvFS remains read-only')
        (self.source / 'payload').write_bytes(b'import payload')
        self.am('library', 'init', self.library, '--json')
        self.am('import', 'scan', self.source, '--plan', self.plan, '--json')

    def run_plan(self, *options, code=0):
        return self.am('import', 'run', '--plan', self.plan, '--library', self.library,
                       '--json', *options, code=code)

    def test_run_and_repeat(self):
        self.prepare()
        original = self.plan.read_bytes()
        summary = self.run_plan('--profile', 'performance', '--jobs', 4)
        row = summary['candidates'][0]
        self.assertEqual(row['status'], 'created', summary)
        self.assertEqual(summary['plan_sha256'], hashlib.sha256(original).hexdigest())
        again = self.run_plan()
        self.assertEqual(again['candidates'][0]['status'], 'already_imported')
        self.assertEqual(again['candidates'][0]['asset_id'], row['asset_id'])
        self.am('library', 'info', self.library, '--json')
        jobs = self.am('jobs', 'list', self.library, '--all', '--json')['jobs']
        self.assertEqual(len(jobs), 2)
        self.am('library', 'verify', self.library, '--full', '--json')
        self.assertEqual(self.plan.read_bytes(), original)

    def test_enqueue_and_jobs_run(self):
        self.prepare()
        summary = self.run_plan('--enqueue-only')
        self.assertEqual(summary['candidates'][0]['status'], 'already_queued')
        self.assertEqual(list(self.library.rglob('*.zip')), [])
        self.am('jobs', 'run', self.library, '--jobs', 2, '--json')
        repeat = self.run_plan()
        self.assertEqual(repeat['candidates'][0]['status'], 'already_imported')
        child = self.am('jobs', 'show', self.library, repeat['candidates'][0]['job_id'], '--json')
        self.assertEqual(child['state'], 'succeeded')
        self.assertEqual(child['result']['asset_id'], repeat['candidates'][0]['asset_id'])

    def test_deferred_and_failed_results(self):
        self.prepare()
        plan = json.loads(self.plan.read_text('utf8'))
        plan['candidates'][0]['entries'][0]['decision'] = 'ask'
        self.plan.write_text(json.dumps(plan), 'utf8')
        summary = self.run_plan(code=3)
        self.assertEqual(summary['candidates'][0]['message_id'], 'am.import.run.unresolved')
        plan['candidates'][0]['entries'][0]['decision'] = 'include'
        self.plan.write_text(json.dumps(plan), 'utf8')
        (self.source / 'payload').write_bytes(b'changed')
        summary = self.run_plan(code=4)
        row = summary['candidates'][0]
        self.assertEqual(row['error']['message_id'], 'am.import.source_changed')
        jobs = self.am('jobs', 'list', self.library, '--all', '--json')['jobs']
        failed = [job for job in jobs if job['state'] == 'failed']
        self.assertEqual(len(failed), 2)
        self.assertTrue(all('result' in job for job in failed))
        child = self.am('jobs', 'show', self.library, row['job_id'], '--json')
        self.assertEqual(child['result']['error']['message_id'], 'am.import.source_changed')
        self.assertEqual(self.run_plan(code=4)['candidates'][0]['retry_hint'], 'am jobs retry')
        self.assertEqual(list(self.library.rglob('*.zip')), [])

    def test_invalid_and_overlapping_plan(self):
        self.plan.write_text('{}', 'utf8')
        result = self.run_plan(code=2)
        self.assertEqual(result['error']['message_id'], 'am.import.plan_invalid')
        self.assertFalse(self.library.exists())
        (self.source / 'payload').write_bytes(b'payload')
        self.am('import', 'scan', self.source, '--plan', self.work / 'valid.json', '--json')
        self.plan = self.work / 'valid.json'
        self.library = self.source / 'nested-library'
        result = self.run_plan(code=2)
        self.assertEqual(result['error']['message_id'], 'am.import.library_overlaps_source')
        self.assertFalse(self.library.exists())


if __name__ == '__main__':
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf8', errors='backslashreplace')
    parser = argparse.ArgumentParser()
    for name in ('am', 'root'):
        parser.add_argument('--' + name, type=Path, required=True)
    CONFIG = parser.parse_args()
    CONFIG.am = CONFIG.am.resolve()
    CONFIG.root = CONFIG.root.resolve()
    unittest.main(argv=[__file__])
