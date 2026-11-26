"""T-033: real CLI export, immutable sources, queue registration and exit codes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sqlite3
import sys
import unittest
import uuid
import zipfile
from contextlib import closing

CONFIG = None


class Utf8Info(zipfile.ZipInfo):
    def _encodeFilenameFlags(self):
        return self.filename.encode('utf8'), self.flag_bits | 0x800


class ExportCli(unittest.TestCase):
    def setUp(self):
        self.work = CONFIG.root / str(uuid.uuid4())
        self.work.mkdir(parents=True)
        self.library = self.work / 'library'
        self.target = self.work / 'output'
        self.target.mkdir()
        self.env = os.environ.copy()
        self.env.pop('AM_LOG_DIR', None)
        self.env.pop('AM_LOG_LEVEL', None)
        profile = self.work / 'profile'
        profile.mkdir()
        for key in ('HOME', 'USERPROFILE', 'LOCALAPPDATA', 'XDG_STATE_HOME', 'TMP', 'TEMP', 'TMPDIR'):
            self.env[key] = str(profile)
        self.am('library', 'init', self.library, '--json')
        # Install a synthetic initial library fixture, before any writer opens
        # it. Golden bytes are read only; the corpus is never regenerated here.
        self.asset = CONFIG.project / 'tests/fixtures/golden/pos-mixed-compression/asset.zip'
        with zipfile.ZipFile(self.asset) as archive:
            self.manifest = json.loads(archive.read('manifest.json'))
        self.asset_id = self.manifest['asset']['id']
        location = self.library / 'assets' / self.asset_id[-2:] / (self.asset_id + '.zip')
        location.parent.mkdir(parents=True, exist_ok=True)
        location.write_bytes(self.asset.read_bytes())
        self.source = location

    def am(self, *args, code=0, json_output=True):
        result = subprocess.run([str(CONFIG.am), *map(str, args)], cwd=self.work, env=self.env,
                                capture_output=True, text=True, encoding='utf8', timeout=120)
        self.assertEqual(result.returncode, code, (result.stdout, result.stderr))
        return json.loads(result.stdout) if json_output else result.stdout

    def export(self, *options, code=0):
        return self.am('export', self.library, self.asset_id, '--to', self.target, '--json', *options, code=code)

    def database_dump(self, name, ignore_session=False):
        result = {}
        with closing(sqlite3.connect((self.library / '.am' / name).as_uri() + '?mode=ro', uri=True)) as database:
            tables = database.execute("SELECT name,sql FROM sqlite_master WHERE type='table' ORDER BY name").fetchall()
            for table, sql in tables:
                cursor = database.execute('SELECT * FROM "' + table.replace('"', '""') + '"')
                columns = tuple(column[0] for column in cursor.description)
                rows = []
                for row in cursor:
                    # The exception is an exact key set, never the entire meta table.
                    if ignore_session and table == 'meta' and dict(zip(columns, row)).get('key') in {'writer_session'}:
                        continue
                    rows.append(tuple((type(value).__name__, value.hex() if isinstance(value, bytes) else value)
                                      for value in row))
                result[table] = (sql, columns, sorted(rows, key=repr))
        return result

    def immutable_library(self):
        paths = [self.library / 'am-library.toml', self.library / '.am/userdata.sqlite']
        paths += list((self.library / 'assets').rglob('*.zip')) + list((self.library / 'trash').rglob('*.zip'))
        return {str(path.relative_to(self.library)): path.read_bytes() for path in paths}

    def test_full_selective_conflicts_and_usage(self):
        original = self.source.read_bytes()
        unchanged = self.immutable_library()
        index = self.database_dump('index.sqlite', ignore_session=os.name == 'nt')
        read_only_databases = ({name: self.database_dump(name) for name in ('index.sqlite', 'userdata.sqlite', 'state.sqlite')}
                               if os.name != 'nt' else {})
        summary = self.export('--name', 'Full')
        self.assertEqual(summary['assets'][0]['status'], 'exported', summary)
        self.assertEqual(summary['assets'][0]['revision'], self.manifest['asset']['revision'])
        self.assertEqual(summary['assets'][0]['asset_id'], self.asset_id)
        self.assertEqual((self.target / 'Full.zip').read_bytes(), original)
        self.assertEqual(summary['queued'], os.name == 'nt')
        conflict = self.export('--name', 'Full', code=4)
        self.assertEqual(conflict['assets'][0]['error']['message_id'], 'am.export.target_exists')
        self.assertEqual((self.target / 'Full.zip').read_bytes(), original)
        renamed = self.export('--name', 'Full', '--on-conflict', 'rename')
        self.assertEqual(renamed['assets'][0]['target_name'], 'Full (2).zip')
        selective = self.export('--files', 'all', '--name', 'Selected')
        self.assertEqual(selective['assets'][0]['mode'], 'files')
        with zipfile.ZipFile(self.target / 'Selected.zip') as archive, zipfile.ZipFile(self.source) as source:
            self.assertFalse(archive.comment)
            self.assertNotIn('manifest.json', archive.namelist())
            self.assertEqual(len(archive.namelist()), len(self.manifest['files']))
            actual = {hashlib.sha256(archive.read(name)).hexdigest() for name in archive.namelist()}
            expected = {file['sha256'] for file in self.manifest['files']}
            self.assertEqual(actual, expected)
            for entry, file in zip(archive.infolist(), self.manifest['files']):
                self.assertTrue(entry.filename.startswith('Selected/'))
                self.assertEqual(entry.compress_type, source.getinfo(file['storage_path']).compress_type)
                self.assertEqual(entry.CRC, source.getinfo(file['storage_path']).CRC)
                self.assertTrue(entry.flag_bits & 0x800)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(self.export('--name', 'CON', code=2)['error']['message_id'], 'am.export.name_invalid')
        self.assertEqual(self.export('--file', str(uuid.uuid4()), code=2)['error']['message_id'], 'am.export.file_unknown')
        duplicate = self.am('export', self.library, self.asset_id, self.asset_id,
                            '--to', self.target, '--json', code=2)
        self.assertEqual(duplicate['error']['message_id'], 'am.export.arguments_invalid')
        invalid_variant = self.am('export', self.library, '00000000-0000-4000-0000-000000000000',
                                  '--to', self.target, '--json', code=2)
        self.assertEqual(invalid_variant['error']['message_id'], 'am.export.arguments_invalid')
        text = self.am('export', self.library, self.asset_id, '--to', self.target, '--name', 'Text', json_output=False)
        self.assertIn('Export results', text)
        self.assertIn('exported', text)
        self.assertEqual(list(self.target.glob('.am-export-*.tmp')), [])
        self.assertEqual(self.immutable_library(), unchanged)
        self.assertEqual(self.database_dump('index.sqlite', ignore_session=os.name == 'nt'), index)
        for name, dump in read_only_databases.items():
            self.assertEqual(self.database_dump(name), dump)

    def test_enqueue_only_and_jobs_run_or_read_only_refusal(self):
        if os.name != 'nt':
            before = (self.library / '.am/state.sqlite').read_bytes()
            rejected = self.export('--enqueue-only', code=4)
            self.assertEqual(rejected['error']['message_id'], 'am.export.queue_unavailable')
            self.assertEqual((self.library / '.am/state.sqlite').read_bytes(), before)
            self.assertEqual(list(self.target.iterdir()), [])
            return
        summary = self.export('--enqueue-only', '--name', 'Queued')
        self.assertTrue(summary['queued'])
        self.assertEqual(summary['assets'][0]['status'], 'queued')
        self.assertEqual(list(self.target.iterdir()), [])
        child_id = summary['assets'][0]['job_id']
        self.am('jobs', 'run', self.library, '--json')
        child = self.am('jobs', 'show', self.library, child_id, '--json')
        self.assertEqual(child['state'], 'succeeded')
        self.assertEqual(child['result']['target_name'], 'Queued.zip')
        self.assertEqual((self.target / 'Queued.zip').read_bytes(), self.source.read_bytes())

    def test_batch_keeps_going_after_asset_in_trash(self):
        extra_ids = [str(uuid.uuid4()), str(uuid.uuid4())]
        for index, asset_id in enumerate(extra_ids):
            location = (self.library / 'trash' / (asset_id + '.zip') if index == 0 else
                        self.library / 'assets' / asset_id[-2:] / (asset_id + '.zip'))
            location.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(self.asset) as original, zipfile.ZipFile(location, 'w') as clone:
                for entry in original.infolist():
                    data = original.read(entry)
                    if entry.filename == 'manifest.json':
                        data = data.replace(self.asset_id.encode('ascii'), asset_id.encode('ascii'))
                    info = Utf8Info(entry.filename, entry.date_time)
                    info.compress_type = entry.compress_type
                    clone.writestr(info, data)
        result = self.am('export', self.library, self.asset_id, *extra_ids, '--to', self.target,
                         '--on-conflict', 'rename', '--json', code=4)
        rows = {row['asset_id']: row for row in result['assets']}
        self.assertEqual(rows[self.asset_id]['status'], 'exported', result)
        self.assertEqual(rows[extra_ids[0]]['error']['message_id'], 'am.export.asset_in_trash')
        self.assertEqual(rows[extra_ids[1]]['status'], 'exported', result)
        self.assertEqual(len(list(self.target.glob('*.zip'))), 2)


if __name__ == '__main__':
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf8', errors='backslashreplace')
    parser = argparse.ArgumentParser()
    for name in ('am', 'root', 'project'):
        parser.add_argument('--' + name, type=Path, required=True)
    CONFIG = parser.parse_args()
    for name in ('am', 'root', 'project'):
        setattr(CONFIG, name, getattr(CONFIG, name).resolve())
    unittest.main(argv=[__file__])
