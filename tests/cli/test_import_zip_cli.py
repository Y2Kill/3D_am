"""T-031: ZIP scan/run summaries, source immutability and real CLI exit codes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import unittest
import uuid
import zipfile

CONFIG = None


class ImportZipCli(unittest.TestCase):
    def setUp(self):
        self.work = CONFIG.root / str(uuid.uuid4())
        self.source = self.work / 'source'
        self.source.mkdir(parents=True)
        self.library = self.work / 'library'
        self.plan = self.work / 'plan.json'
        self.env = os.environ.copy()
        profile = self.work / 'profile'
        profile.mkdir()
        for key in ('HOME', 'USERPROFILE', 'LOCALAPPDATA', 'XDG_STATE_HOME', 'TMP', 'TEMP', 'TMPDIR'):
            self.env[key] = str(profile)
        self.env.pop('AM_LOG_DIR', None)
        self.env.pop('AM_LOG_LEVEL', None)

    def am(self, *args, code=0):
        result = subprocess.run([str(CONFIG.am), *map(str, args)], cwd=self.work, env=self.env,
                                capture_output=True, text=True, encoding='utf8', timeout=120)
        self.assertEqual(result.returncode, code, (result.stdout, result.stderr))
        return json.loads(result.stdout)

    def fingerprint(self):
        return {str(p.relative_to(self.source)): (p.stat().st_size, p.stat().st_mtime_ns,
                hashlib.sha256(p.read_bytes()).hexdigest()) for p in self.source.rglob('*') if p.is_file()}

    def test_zip_roundtrip_and_exit_codes(self):
        with zipfile.ZipFile(self.source / 'model.zip', 'w') as archive:
            archive.writestr('folder/', b'')
            archive.writestr('folder/stored.bin', b'stored bytes')
            archive.writestr('deflated.bin', b'deflated bytes', compress_type=zipfile.ZIP_DEFLATED)
        before = self.fingerprint()
        summary = self.am('import', 'scan', self.source, '--plan', self.plan, '--json')
        self.assertEqual(summary['archives'], 1)
        self.assertEqual(summary['archive_entries'], 3)
        self.assertEqual(self.fingerprint(), before)
        self.am('library', 'init', self.library, '--json')
        if os.name != 'nt':
            report = self.am('import', 'run', '--plan', self.plan, '--library', self.library, '--json', code=4)
            self.assertEqual(report['error']['message_id'], 'am.library.storage_read_only')
            return
        report = self.am('import', 'run', '--plan', self.plan, '--library', self.library, '--json')
        self.assertEqual(report['archives'], 1)
        self.assertEqual(report['archive_entries'], 3)
        self.assertEqual(report['deferred'], 0)
        self.assertEqual(report['candidates'][0]['status'], 'created')
        self.assertEqual(self.fingerprint(), before)
        self.am('library', 'verify', self.library, '--full', '--json')
        assets = list((self.library / 'assets').rglob('*.zip'))
        self.assertEqual(len(assets), 1)
        with zipfile.ZipFile(assets[0]) as archive:
            payloads = sorted(archive.read(name) for name in archive.namelist() if name.startswith('content/'))
            self.assertEqual(payloads, [b'deflated bytes', b'stored bytes'])
            manifest = json.loads(archive.read('manifest.json'))
            input_object = manifest['inputs'][0]['objects'][1]
            self.assertEqual(input_object['kind'], 'archive')
            self.assertEqual(input_object['sha256'], before['model.zip'][2])
            self.assertEqual(input_object['size_bytes'], str(before['model.zip'][0]))
            records = {entry['path']: entry['crc32'] for entry in manifest['inputs'][0]['entries'] if entry['object_id'] == input_object['id']}
            with zipfile.ZipFile(self.source / 'model.zip') as original:
                self.assertEqual(records, {entry.filename.rstrip('/'): f'{entry.CRC:08x}' for entry in original.infolist()})
        plan = json.loads(self.plan.read_text(encoding='utf8'))
        plan['candidates'][0]['action'] = 'ask'
        self.plan.write_text(json.dumps(plan), encoding='utf8')
        deferred = self.am('import', 'run', '--plan', self.plan, '--library', self.library, '--json', code=3)
        self.assertEqual(deferred['deferred'], 1)
        plan['candidates'][0]['action'] = 'import'
        self.plan.write_text(json.dumps(plan), encoding='utf8')
        with (self.source / 'model.zip').open('ab') as stream:
            stream.write(b'changed')
        failed = self.am('import', 'run', '--plan', self.plan, '--library', self.library, '--json', code=4)
        self.assertEqual(failed['candidates'][0]['error']['message_id'], 'am.import.source_changed')
        self.plan.write_text('{}', encoding='utf8')
        invalid = self.am('import', 'run', '--plan', self.plan, '--library', self.library, '--json', code=2)
        self.assertEqual(invalid['error']['message_id'], 'am.import.plan_invalid')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--am', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    CONFIG, remaining = parser.parse_known_args()
    CONFIG.am = CONFIG.am.resolve()
    CONFIG.root = CONFIG.root.resolve()
    unittest.main(argv=[__file__, *remaining])
