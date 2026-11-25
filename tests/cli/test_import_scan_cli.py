"""Read-only import scan process checks (SC-1/2/3/8/13/14/15)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import threading
import unittest
import uuid

CONFIG = None


class ImportScanCli(unittest.TestCase):
    def setUp(self):
        self.work = CONFIG.root / str(uuid.uuid4())
        self.work.mkdir(parents=True)
        self.source = self.work / 'source'
        self.source.mkdir()
        self.output = self.work / 'plan.json'
        self.env = os.environ.copy()
        self.env.pop('AM_LOG_DIR', None)
        self.env.pop('AM_LOG_LEVEL', None)
        self.profile = self.work / 'profile'
        self.profile.mkdir()
        for key in ('HOME', 'USERPROFILE', 'LOCALAPPDATA', 'XDG_STATE_HOME', 'TMP', 'TEMP', 'TMPDIR'):
            self.env[key] = str(self.profile)

    def run_am(self, *arguments, code=0):
        process = subprocess.run([str(CONFIG.am), *map(str, arguments)], cwd=self.work,
                                 env=self.env, capture_output=True, text=True, encoding='utf8', timeout=120)
        self.assertEqual(process.returncode, code, (process.stdout, process.stderr))
        return process

    def scan(self, *options, code=0):
        return self.run_am('import', 'scan', self.source, '--plan', self.output, '--json', *options, code=code)

    def snapshot(self):
        return {str(p.relative_to(self.source)): (p.stat().st_size, p.stat().st_mtime_ns,
                                                  hashlib.sha256(p.read_bytes()).hexdigest())
                for p in self.source.rglob('*') if p.is_file()}

    def test_summary(self):
        (self.source / 'part.bin').write_bytes(b'input bytes')
        before = self.snapshot()
        summary = json.loads(self.scan('--hash').stdout)
        self.assertEqual(summary['command'], 'import scan')
        self.assertEqual(summary['candidates'], 1)
        self.assertEqual(summary['completeness'], 'complete')
        self.assertEqual(self.snapshot(), before)
        plan = json.loads(self.output.read_text('utf8'))
        self.assertEqual(plan['version'], 2)
        sys.path.insert(0, str(CONFIG.pylib))
        import jsonschema
        schema = json.loads((CONFIG.source / 'schemas/import-plan.v2.schema.json').read_text('utf8'))
        jsonschema.Draft202012Validator(schema).validate(plan)
        self.assertEqual(list(self.profile.iterdir()), [])
        self.output.unlink()
        plain = self.run_am('import', 'scan', self.source, '--plan', self.output)
        self.assertIn('Import scan:', plain.stdout)

    def test_errors(self):
        (self.source / 'part').write_bytes(b'unchanged')
        self.output.write_bytes(b'owner output')
        result = json.loads(self.scan(code=4).stdout)
        self.assertEqual(result['error']['message_id'], 'am.import.output_exists')
        self.assertEqual(self.output.read_bytes(), b'owner output')
        self.run_am('import', 'scan', self.source, '--plan', self.source / 'bad.json', '--json', code=2)
        self.assertFalse((self.source / 'bad.json').exists())
        self.scan('--skip-identical', code=2)
        self.output.unlink()
        result = json.loads(self.scan('--profile', self.work / 'missing.toml', code=2).stdout)
        self.assertEqual(result['error']['message_id'], 'am.import.profile_invalid')
        self.assertEqual(list(self.work.glob('.am-import-plan-*')), [])

    def test_profiles(self):
        directory = self.source / 'Channel' / 'Model'
        directory.mkdir(parents=True)
        (directory / 'part').write_bytes(b'part')
        self.scan('--profile', 'telegram')
        plan = json.loads(self.output.read_text('utf8'))
        self.assertEqual(plan['candidates'][0]['context']['sources'][0]['name'], 'Channel')
        self.assertFalse(plan['candidates'][0]['context']['sources'][0]['confirmed'])
        custom = self.work / 'custom.toml'
        raw = b'id="custom"\nversion=1\n[boundaries]\ncandidate="root"\n'
        custom.write_bytes(raw)
        self.output.unlink()
        self.scan('--profile', custom)
        plan = json.loads(self.output.read_text('utf8'))
        self.assertEqual(plan['profile']['sha256'], hashlib.sha256(raw).hexdigest())

    def test_overlap(self):
        library = self.source / 'library'
        self.run_am('library', 'init', library)
        for source in (self.source, library / 'assets'):
            result = self.run_am('import', 'scan', source, '--library', library,
                                 '--plan', self.output, '--json', code=2)
            self.assertEqual(json.loads(result.stdout)['error']['message_id'], 'am.import.library_overlaps_source')
            self.assertFalse(self.output.exists())

    def test_ready(self):
        import shutil
        shutil.copyfile(CONFIG.source / 'tests/fixtures/golden/pos-minimal/asset.zip', self.source / 'ready.zip')
        (self.source / 'part').write_bytes(b'part')
        summary = json.loads(self.scan('--single').stdout)
        self.assertEqual(summary['kinds'], {'new_asset': 1, 'ready_asset': 1})
        plan = json.loads(self.output.read_text('utf8'))
        remaining = next(c for c in plan['candidates'] if c['kind'] == 'new_asset')
        self.assertIn('ready_asset_separated', remaining['reasons'])

    def test_signal(self):
        source = self.source / 'large.bin'
        with source.open('wb') as output:
            output.truncate(1 << 30)
        before = source.stat()
        command = [str(CONFIG.am), 'import', 'scan', str(self.source), '--hash', '--plan', str(self.output), '--json']
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
        process = subprocess.Popen(command, cwd=self.work, env=self.env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, encoding='utf8', creationflags=flags)
        ready = queue.Queue()
        errors = []

        def read_events():
            for line in process.stderr:
                errors.append(line)
                if 'Scanning source inputs.' in line:
                    ready.put(True)
            ready.put(False)

        reader = threading.Thread(target=read_events)
        reader.start()
        try:
            self.assertTrue(ready.get(timeout=30), errors)
            process.send_signal(signal.CTRL_BREAK_EVENT if os.name == 'nt' else signal.SIGINT)
            process.wait(timeout=30)
            stdout = process.stdout.read()
            reader.join(timeout=30)
            self.assertEqual(process.returncode, 3, (stdout, errors))
            self.assertFalse(self.output.exists())
            self.assertEqual(list(self.work.glob('.am-import-plan-*')), [])
            self.assertEqual((source.stat().st_size, source.stat().st_mtime_ns), (before.st_size, before.st_mtime_ns))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=30)
            reader.join(timeout=30)
            process.stdout.close()
            process.stderr.close()


if __name__ == '__main__':
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf8', errors='backslashreplace')
    parser = argparse.ArgumentParser()
    for name in ('am', 'root', 'source', 'pylib'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--case', required=True)
    CONFIG = parser.parse_args()
    for name in ('am', 'root', 'source', 'pylib'):
        setattr(CONFIG, name, getattr(CONFIG, name).resolve())
    unittest.main(argv=[__file__, 'ImportScanCli.test_' + CONFIG.case])
