"""Local diagnostic process checks (NFR-14.4.9, K-130); synthetic data only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import unittest
import uuid

CONFIG = None


class LoggingCli(unittest.TestCase):
    def setUp(self):
        self.work = CONFIG.root / str(uuid.uuid4())
        self.work.mkdir(parents=True)
        self.profile = self.work / 'profile'
        self.profile.mkdir()
        self.env = os.environ.copy()
        for name in ('AM_LOG_DIR', 'AM_LOG_LEVEL'):
            self.env.pop(name, None)
        for name in ('HOME', 'USERPROFILE', 'LOCALAPPDATA', 'XDG_STATE_HOME', 'TMP', 'TEMP', 'TMPDIR'):
            self.env[name] = str(self.profile)

    def run_am(self, *args, expected=0, env=None):
        result = subprocess.run([str(CONFIG.am), *map(str, args)], cwd=self.work, env=env or self.env,
                                capture_output=True, encoding='utf8', errors='strict', timeout=60)
        self.assertEqual(result.returncode, expected, (args, result.stdout, result.stderr))
        return result

    def records(self, directory):
        records = []
        for path in directory.glob('am-*.jsonl'):
            self.assertRegex(path.name, r'^am-\d{8}T\d{6}Z-\d+(\.\d+)?\.jsonl$')
            content = path.read_text('utf8')
            if content:
                self.assertTrue(content.endswith('\n'))
            records += [json.loads(line) for line in content.splitlines()]
        return records

    def snapshot(self):
        return {str(path.relative_to(self.work)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in self.work.rglob('*') if path.is_file()}

    def test_default_off(self):
        self.run_am('version')
        self.assertEqual(self.snapshot(), {})
        self.assertEqual(list(self.profile.iterdir()), [])

    def test_configuration(self):
        environment = self.env.copy()
        fallback = self.work / 'fallback'
        environment['AM_LOG_DIR'] = str(fallback)
        environment['AM_LOG_LEVEL'] = 'critical'
        explicit = self.work / 'explicit'
        self.run_am('--log-dir', explicit, '--log-level', 'info', 'version', env=environment)
        self.assertFalse(fallback.exists())
        rows = self.records(explicit)
        self.assertEqual([row['event'] for row in rows], ['app.start', 'app.exit'])
        self.assertEqual(rows[0]['fields']['command'], 'version')
        self.assertEqual(rows[-1]['fields']['exit_code'], 0)
        environment['AM_LOG_LEVEL'] = 'info'
        library = self.work / 'private-library-argument'
        self.run_am('library', 'init', library)
        self.run_am('library', 'info', library, env=environment)
        rows = self.records(fallback)
        self.assertEqual(rows[0]['fields']['command'], 'library info')
        self.assertNotIn(str(library), json.dumps(rows))
        self.assertEqual(list(self.profile.iterdir()), [])

    def test_failures(self):
        blocked = self.work / 'not-a-directory'
        blocked.write_text('owner file')
        result = self.run_am('--log-dir', blocked, 'version')
        self.assertIn('File logging is unavailable', result.stderr)
        self.assertEqual(blocked.read_text(), 'owner file')
        directory = self.work / 'logs'
        self.run_am('--log-dir', directory, 'library', 'info', self.work / 'missing', '--json', expected=4)
        rows = self.records(directory)
        self.assertIn('cli.error', [row['event'] for row in rows])
        self.assertEqual(rows[-1]['fields']['exit_code'], 4)
        self.run_am('--log-level', 'invalid', 'version', expected=2)

    def test_concurrent(self):
        directory = self.work / 'logs'
        command = [str(CONFIG.am), '--log-dir', str(directory), 'version']
        processes = [subprocess.Popen(command, cwd=self.work, env=self.env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding='utf8') for _ in range(2)]
        for process in processes:
            stdout, stderr = process.communicate(timeout=60)
            self.assertEqual(process.returncode, 0, (stdout, stderr))
        logs = list(directory.glob('am-*.jsonl'))
        self.assertEqual(len(logs), 2)
        rows = self.records(directory)
        self.assertEqual(len({row['pid'] for row in rows}), 2)
        self.assertEqual(len(rows), 4)

    def test_commit(self):
        source = self.work / 'input.bin'
        source.write_bytes(b'synthetic commit fixture')
        directory = self.work / 'logs'
        if os.name != 'nt':
            self.run_am('--log-dir', directory, '--log-level', 'debug', 'asset', 'create', source,
                        '-o', self.work / 'blocked.zip', '--json', expected=4)
            reason = 'Production writes require a verified fixed NTFS volume; debug commit logging is covered by injected-capability unit tests'
            print(reason)
            self.skipTest(reason)
        self.run_am('--log-dir', directory, 'asset', 'create', source, '-o', self.work / 'info.zip', '--json')
        self.assertNotIn('commit.phase', [row['event'] for row in self.records(directory)])
        self.run_am('--log-dir', directory, '--log-level', 'debug', 'asset', 'create', source,
                    '-o', self.work / 'debug.zip', '--json')
        phases = {row['fields']['phase'] for row in self.records(directory) if row['event'] == 'commit.phase'}
        self.assertEqual(phases, {'prepared', 'verified', 'committed', 'indexed'})

    def test_export(self):
        import zipfile
        directory = self.work / 'logs'
        directory.mkdir()
        record = {'ts': '2026-11-23T10:00:00.000Z', 'msg': 'PrivateOwnerCanary on PrivateHostCanary',
                  'fields': {'root': r'C:\Users\PrivateOwnerCanary\Secret', 'input': '/home/PrivateOwnerCanary/private.stl'},
                  'kinds': {'root': 'path', 'input': 'path'}}
        (directory / 'am-20261123T000000Z-123.jsonl').write_text(json.dumps(record) + '\nbroken JSON\n', encoding='utf8')
        environment = self.env.copy()
        environment.update(AM_LOG_DIR=str(directory), USERNAME='PrivateOwnerCanary', USER='PrivateOwnerCanary',
                           COMPUTERNAME='PrivateHostCanary', HOSTNAME='PrivateHostCanary')
        before = self.snapshot()
        output = self.work / 'diagnostic.zip'
        dry = json.loads(self.run_am('diag', 'export', output, '--dry-run', '--json', env=environment).stdout)
        self.assertTrue(dry['dry_run'])
        self.assertEqual(self.snapshot(), before)
        result = json.loads(self.run_am('diag', 'export', output, '--log-dir', directory, '--json', env=environment).stdout)
        self.assertTrue(result['logs_available'])
        with zipfile.ZipFile(output) as archive:
            combined = b''.join(archive.read(name) for name in archive.namelist()).lower()
            for private in (b'privateownercanary', b'privatehostcanary', b'c:\\users\\', b'/home/'):
                self.assertNotIn(private, combined)
            metadata = json.loads(archive.read('diagnostics.json'))
        sys.path.insert(0, str(CONFIG.pylib))
        import jsonschema
        schema = json.loads((CONFIG.source / 'schemas/diagnostics.v1.schema.json').read_text('utf8'))
        jsonschema.Draft202012Validator(schema, format_checker=jsonschema.FormatChecker()).validate(metadata)
        self.run_am('diag', 'export', output, '--json', expected=4)
        self.run_am('diag', 'export', self.work / 'bad.zip', '--since', 'bad', '--json', expected=2)
        self.run_am('diag', 'export', self.work / 'absent.zip', '--log-dir', self.work / 'missing', '--json', expected=4)
        self.assertFalse((self.work / 'absent.zip').exists())
        system_only = self.work / 'system.zip'
        value = json.loads(self.run_am('diag', 'export', system_only, '--json').stdout)
        self.assertFalse(value['logs_available'])
        missing_environment = self.env.copy()
        missing_environment['AM_LOG_DIR'] = str(self.work / 'unavailable' / 'blocked')
        (self.work / 'unavailable').write_text('owner file')
        value = json.loads(self.run_am('diag', 'export', self.work / 'fallback.zip', '--json', env=missing_environment).stdout)
        self.assertFalse(value['logs_available'])

    def test_killed(self):
        directory = self.work / 'logs'
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        process = subprocess.Popen([str(CONFIG.helper), str(directory)], cwd=self.work, env=self.env,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   encoding='utf8', creationflags=flags)
        ready = queue.Queue()
        reader = threading.Thread(target=lambda: ready.put(process.stdout.readline()))
        reader.start()
        try:
            self.assertEqual(ready.get(timeout=30).strip(), 'READY')
            process.kill()
            process.wait(timeout=30)
            reader.join(timeout=30)
            self.assertFalse(reader.is_alive())
            rows = self.records(directory)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['event'], 'crash.test')
            self.assertEqual(rows[0]['level'], 'error')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=30)
            reader.join(timeout=30)
            for stream in (process.stdin, process.stdout, process.stderr):
                stream.close()


if __name__ == '__main__':
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf8', errors='backslashreplace')
    parser = argparse.ArgumentParser()
    for name in ('am', 'root', 'helper', 'source', 'pylib'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--case', required=True)
    CONFIG = parser.parse_args()
    for name in ('am', 'root', 'helper', 'source', 'pylib'):
        setattr(CONFIG, name, getattr(CONFIG, name).resolve())
    unittest.main(argv=[__file__, 'LoggingCli.test_' + CONFIG.case])
