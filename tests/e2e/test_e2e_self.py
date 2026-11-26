"""Negative tests for the e2e oracle, with no CLI dependency."""
import argparse
import copy
from contextlib import closing
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_e2e import ROOT, assert_isolated_arguments, digest, equal, index_snapshot, subset
from check_portability import safe_member, verify_files


class OracleTests(unittest.TestCase):
    def setUp(self):
        cache = ROOT / '.cache/t035/selftest'
        cache.mkdir(parents=True, exist_ok=True)
        self.area = tempfile.TemporaryDirectory(dir=cache)
        self.addCleanup(self.area.cleanup)
        self.work = Path(self.area.name)
        self.index = self.work / 'index.sqlite'
        with closing(sqlite3.connect(self.index)) as db:
            db.executescript('''
                CREATE TABLE assets(asset_id TEXT, title TEXT, rel_path TEXT,
                                    in_trash INTEGER, zip_mtime_ns INTEGER, zip_size INTEGER);
                INSERT INTO assets VALUES('id','Model','assets/id.zip',0,123,456);
                CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT);
                INSERT INTO meta VALUES('rebuild_required','0'),('rebuilt_at','old'),('writer_session','closed');
                CREATE TABLE empty_table(value TEXT);
                CREATE TABLE fts_names_owner(fts_rowid INTEGER, asset_id TEXT);
                CREATE TABLE fts_docs_owner(fts_rowid INTEGER, asset_id TEXT);
                CREATE VIRTUAL TABLE fts_assets USING fts5(title);
                CREATE VIRTUAL TABLE fts_names USING fts5(name);
                CREATE VIRTUAL TABLE fts_docs USING fts5(text);
                INSERT INTO fts_assets(rowid,title) VALUES(1,'Model');
                INSERT INTO fts_names(rowid,name) VALUES(2,'part.stl');
                INSERT INTO fts_docs(rowid,text) VALUES(3,'Description');
                INSERT INTO fts_names_owner VALUES(2,'id');
                INSERT INTO fts_docs_owner VALUES(3,'id');
            ''')

    def execute(self, statement):
        with closing(sqlite3.connect(self.index)) as db:
            db.executescript(statement)

    def test_changed_zip_field_reports_pointer(self):
        before = index_snapshot(self.index)
        self.execute("UPDATE assets SET title='Changed'")
        with self.assertRaisesRegex(AssertionError, '/index/assets/rows'):
            equal(before, index_snapshot(self.index), '/index')

    def test_extra_row_and_unknown_local_field_fail(self):
        before = index_snapshot(self.index)
        self.execute("INSERT INTO empty_table VALUES('unexpected')")
        with self.assertRaisesRegex(AssertionError, '/index/empty_table/rows'):
            equal(before, index_snapshot(self.index), '/index')
        self.execute("DELETE FROM empty_table; ALTER TABLE assets ADD COLUMN new_local_value TEXT")
        with self.assertRaisesRegex(AssertionError, '/index/assets'):
            equal(before, index_snapshot(self.index), '/index')

    def test_explicit_local_exceptions_and_rebuild_mtime(self):
        transfer = index_snapshot(self.index)
        rebuild = index_snapshot(self.index, 'rebuild')
        self.execute("UPDATE assets SET zip_mtime_ns=999; UPDATE meta SET value='new' WHERE key='rebuilt_at'")
        equal(transfer, index_snapshot(self.index))
        with self.assertRaisesRegex(AssertionError, '/index/assets/rows'):
            equal(rebuild, index_snapshot(self.index, 'rebuild'), '/index')
        self.execute("UPDATE meta SET value='1' WHERE key='rebuild_required'")
        with self.assertRaisesRegex(AssertionError, '/index/meta/rows'):
            equal(transfer, index_snapshot(self.index), '/index')

    def test_fts_rowids_normalize_but_text_and_tokens_remain(self):
        before = index_snapshot(self.index)
        self.execute('''UPDATE fts_names SET rowid=20 WHERE rowid=2;
                        UPDATE fts_names_owner SET fts_rowid=20;
                        UPDATE fts_docs SET rowid=30 WHERE rowid=3;
                        UPDATE fts_docs_owner SET fts_rowid=30;''')
        equal(before, index_snapshot(self.index))
        self.execute("UPDATE fts_names SET name='other.stl'")
        with self.assertRaises(AssertionError):
            equal(before, index_snapshot(self.index))
        changed = copy.deepcopy(before)
        changed['fts_names']['tokens'][0]['term'] = 'unexpected'
        with self.assertRaisesRegex(AssertionError, '/fts_names/tokens'):
            equal(before, changed)

    def test_hidden_sources_and_old_environment_cannot_be_cli_arguments(self):
        blocked = [self.work / name for name in ('A', 'A-unavailable', 'source', 'source-unavailable', 'old-environment')]
        for root in blocked:
            with self.subTest(root=root), self.assertRaisesRegex(AssertionError, '/isolation'):
                assert_isolated_arguments(['asset', 'inspect', root / 'file.zip'], self.work, blocked)
        assert_isolated_arguments(['library', 'list', self.work / 'B'], self.work, blocked)

    def test_bundle_integrity_and_unsafe_members(self):
        asset = self.work / 'asset.zip'
        asset.write_bytes(b'fixture')
        record = {'format': 'am-e2e-portability', 'version': 1,
                  'files': {'index.sqlite': {'sha256': digest(self.index), 'size': self.index.stat().st_size},
                            'asset.zip': {'sha256': digest(asset), 'size': asset.stat().st_size}}}
        verify_files(self.work, record)
        asset.write_bytes(b'changed')
        with self.assertRaisesRegex(AssertionError, '/bundle/files/asset.zip'):
            verify_files(self.work, record)
        for name in ('../file', '/file', 'C:/file', 'a\\b', 'a//b', './file'):
            with self.subTest(name=name), self.assertRaisesRegex(AssertionError, '/bundle/path'):
                safe_member(name)

    def test_exact_oracle_preserves_old_subset_default(self):
        subset({'value': 1}, {'value': 1, 'extra': True})
        with self.assertRaisesRegex(AssertionError, '/extra'):
            equal({'value': 1}, {'value': 1, 'extra': True})


def check_workflow(path):
    # Optional local gate: use the already installed WSL PyYAML, never install it.
    import yaml
    workflow = yaml.load(path.read_text('utf8'), Loader=yaml.BaseLoader)
    jobs = workflow['jobs']
    equal(['linux-cli', 'linux-min', 'portability-linux', 'win-cli', 'win-debug-min'], sorted(jobs), '/jobs')
    equal('win-cli', jobs['portability-linux']['needs'], '/jobs/portability-linux/needs')
    uploads = [s for s in jobs['win-cli']['steps'] if s.get('with', {}).get('name') == 'e2e-portability']
    downloads = [s for s in jobs['portability-linux']['steps'] if s.get('uses', '').startswith('actions/download-artifact@')]
    equal(1, len(uploads), '/artifact/uploads')
    equal(1, len(downloads), '/artifact/downloads')
    equal(uploads[0]['with']['name'], downloads[0]['with']['name'], '/artifact/name')
    equal('build/e2e/portability/', uploads[0]['with']['path'], '/artifact/upload/path')
    equal('build/e2e/portability', downloads[0]['with']['path'], '/artifact/download/path')
    command = 'python3 -I tests/e2e/check_portability.py --am build/linux-cli/bin/am --bundle build/e2e/portability --work build/e2e/portability-linux'
    if not any(s.get('run') == command for s in jobs['portability-linux']['steps']):
        raise AssertionError('/jobs/portability-linux: checker command differs from local WSL gate')
    print('PASS workflow YAML: five jobs, needs, artifact names/paths, shared checker command')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workflow', type=Path)
    args = parser.parse_args()
    if args.workflow:
        check_workflow(args.workflow)
    else:
        unittest.main(argv=[sys.argv[0]], verbosity=2)
