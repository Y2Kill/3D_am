"""T-036: kill actual child processes and compare recovered logical indexes."""
import argparse
from contextlib import closing
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import stat
import subprocess
import sys
import time
import uuid
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/e2e'))
from run_e2e import index_snapshot
sys.path.insert(0, str(ROOT / 'tests/tools'))
from expect_match import mismatch

POINTS = ['after_lock', 'after_prepared', 'mid_write', 'after_finish', 'after_verify',
          'after_verified_journal', 'before_replace', 'after_replace', 'after_committed_journal', 'after_indexed_journal']
LIFECYCLE = ['after_lock', 'after_prepared', 'before_replace', 'after_replace', 'after_committed_journal', 'after_indexed_journal']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def zips(root):
    result = {}
    for where in ('assets', 'trash'):
        for path in (root / where).rglob('*.zip'):
            with zipfile.ZipFile(path) as archive:
                doc = json.loads(archive.read('manifest.json'))
            identifier = doc['asset']['id']
            assert identifier not in result, '/zip/duplicate_asset_id'
            result[identifier] = {'path': path, 'sha256': sha(path), 'revision': doc['asset']['revision']}
    return result


def equal(a, b, pointer):
    error = mismatch(a, b, pointer, json_pointer=True, exact=True)
    if error:
        raise AssertionError(error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--mode', choices=['init', 'copy', 'ready', 'ready_replace', 'lifecycle', 'rebuild', 'sync', 'operation_log', 'recovery', 'state_lock', 'state_readonly', 'wal_loss', 'import_folder', 'import_zip', 'import_nested', 'import_between', 'export_full', 'export_files', 'diagnostic'], required=True)
    parser.add_argument('--timeout', type=float, default=30, help='Hypothesis: stalled-child guard in seconds')
    args = parser.parse_args()
    work = args.work.resolve() / args.mode / str(uuid.uuid4())
    if not work.is_relative_to(ROOT / '.cache'):
        raise ValueError('Fault work must remain under project .cache/')
    work.mkdir(parents=True)
    helper = args.helper.resolve()
    env = os.environ.copy()
    env.pop('AM_LOG_DIR', None)
    env.pop('AM_LOG_LEVEL', None)
    for key in ('HOME', 'USERPROFILE', 'LOCALAPPDATA', 'XDG_STATE_HOME', 'TMP', 'TEMP', 'TMPDIR'):
        env[key] = str(work)

    def call(*argv):
        result = subprocess.run([str(helper), *map(str, argv)], capture_output=True, encoding='utf8',
                                cwd=work, env=env, timeout=args.timeout)
        if result.returncode:
            raise AssertionError(f'/helper/{argv[0]}: {result.returncode} {result.stdout} {result.stderr}')
        return json.loads(result.stdout) if result.stdout.strip() else {}

    def kill(root, operation, point, before_kill=None):
        marker = root.parent / 'ready'
        marker.unlink(missing_ok=True)
        log = root.parent / (operation + '-' + point + '.log')
        with log.open('w', encoding='utf8') as output:
            process = subprocess.Popen([str(helper), 'run', str(root), operation, point, str(marker)],
                                       cwd=work, env=env, stdout=output, stderr=output)
            try:
                until = time.monotonic() + args.timeout
                while not marker.exists() and process.poll() is None and time.monotonic() < until:
                    time.sleep(0.01)
                if not marker.exists():
                    raise AssertionError('/kill/checkpoint: not reached: ' + log.read_text('utf8'))
                if before_kill:
                    before_kill()
                process.kill()  # TerminateProcess / SIGKILL: child destructors do not run.
                process.wait(timeout=args.timeout)
                assert process.returncode != 0, '/kill: child completed normally'
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=args.timeout)

    operations = ['trash', 'restore', 'purge'] if args.mode == 'lifecycle' else [args.mode]
    if args.mode == 'state_readonly':
        if os.name != 'nt' and os.geteuid() == 0:
            print('SKIP state_readonly: root bypasses POSIX file permission denial')
            return 77
        root = work / 'readonly/library'
        call('seed', root, 'state_readonly')
        state = root / '.am/state.sqlite'
        old_mode = state.stat().st_mode
        before = {identifier: item['sha256'] for identifier, item in zips(root).items()}
        try:
            os.chmod(state, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
            assert not call('try_open', root, 'rw')['opened'], '/readonly/writer_opened'
            assert call('try_open', root, 'ro')['opened'], '/readonly/read_denied'
        finally:
            os.chmod(state, old_mode)
        equal(before, {identifier: item['sha256'] for identifier, item in zips(root).items()}, '/readonly/zips')
        print('PASS FaultProcess.state_readonly: actual read-only file refuses writer; reader and ZIPs preserved')
        return 0
    cases = []
    for operation in operations:
        points = LIFECYCLE if args.mode == 'lifecycle' else POINTS
        if args.mode in ('ready', 'ready_replace'):
            points = [p for p in POINTS if p not in ('after_verified_journal', 'after_replace')]
        if args.mode in ('rebuild', 'sync', 'operation_log', 'state_lock'):
            points = ['transaction']
        if args.mode == 'init':
            points = ['configuration']
        if args.mode == 'recovery':
            points = ['index']
        if args.mode == 'wal_loss':
            points = ['committed']
        if args.mode == 'import_between':
            points = ['candidate']
        if args.mode.startswith('export_'):
            points = ['mid_write', 'after_write', 'before_publish', 'after_publish']
        if args.mode == 'diagnostic':
            points = ['mid_write', 'after_write', 'published']
        for point in points:
            for repeat in range(3):
                cases.append((operation, point, repeat))
    def execute_case(case_args):
        operation, point, repeat = case_args
        case = work / f'{operation}-{point}-{repeat}'
        root = case / 'library'
        meta = call('seed', root, operation)
        def source_facts():
            if 'import_source' not in meta:
                return {}
            source = Path(meta['import_source'])
            return {p.relative_to(source).as_posix(): [sha(p), p.stat().st_size, p.stat().st_mtime_ns]
                    for p in source.rglob('*') if p.is_file()}
        source_before = source_facts()
        before = zips(root)
        user = root / '.am/userdata.sqlite'
        user_sha = sha(user) if user.exists() else None
        prior_evidence = {}
        if operation == 'recovery':
            kill(root, 'change', 'after_verified_journal')
            with closing(sqlite3.connect((root / '.am/state.sqlite').as_uri() + '?mode=ro', uri=True)) as db:
                prior_evidence = dict(db.execute('SELECT p.asset_id,n.new_file_sha256 FROM pending_ops p JOIN pending_ops_new n USING(op_id)'))
            assert meta['asset_id'] in prior_evidence, '/recovery/no_verified_revision'
        orphan = None
        if operation == 'wal_loss':
            orphan = root / '.am/.am-tmp' / (str(uuid.uuid4()) + '.zip')
            orphan.write_bytes(b'Partial ZIP without a journal row')
            old = time.time() - 72 * 3600  # hypothesis: older than retention
            os.utime(orphan, (old, old))
        def blocked_state():
            rejected = call('try_open', root, 'rw')
            assert not rejected['opened'] and rejected['code'] == 14, '/state_lock/writer'
            assert call('try_open', root, 'ro')['opened'], '/state_lock/reader'
        kill(root, operation, point, blocked_state if operation == 'state_lock' else None)
        wal_confirmed = None
        if operation == 'wal_loss':
            wal_confirmed = {key: item['sha256'] for key, item in zips(root).items()}
            assert wal_confirmed[meta['asset_id']] != before[meta['asset_id']]['sha256'], '/wal_loss/no_confirmed_revision'
            assert (root / '.am/state.sqlite-wal').stat().st_size > 0, '/wal_loss/no_committed_wal'
            for suffix in ('-wal', '-shm'):
                (root / ('.am/state.sqlite' + suffix)).unlink(missing_ok=True)
        if operation == 'init':
            assert not (root / 'am-library.toml').exists(), '/init/unconfirmed_configuration'
            assert not zips(root), '/init/unconfirmed_zip'
            return 1
        evidence = dict(prior_evidence)
        with closing(sqlite3.connect((root / '.am/state.sqlite').as_uri() + '?mode=ro', uri=True)) as db:
            evidence.update(db.execute('SELECT p.asset_id,n.new_file_sha256 FROM pending_ops p JOIN pending_ops_new n USING(op_id)'))
        recovered = call('recover', root)
        if orphan:
            assert orphan.exists(), '/wal_loss/orphan_deleted'
            assert any(item['detail'] == 'journal_may_be_lost' for item in recovered['recovered']), '/wal_loss/reason'
        if operation.startswith('import_'):
            confirmed = {key: item['sha256'] for key, item in zips(root).items()}
            call('resume_import', root)
            resumed = zips(root)
            for key, digest in confirmed.items():
                assert resumed[key]['sha256'] == digest, '/import/resume_rewrote_confirmed_asset'
            assert len(resumed) == (3 if operation == 'import_between' else 2), '/import/resumed_candidate_count'
            equal(source_before, source_facts(), '/import/source_changed')
        if operation.startswith('export_') or operation == 'diagnostic':
            target = case / ('diagnostic.zip' if operation == 'diagnostic' else 'exports/result.zip')
            published = point in ('after_publish', 'published')
            assert target.exists() == published, '/export/publication_boundary'
            confirmed = sha(target) if published else None
            if operation.startswith('export_'):
                for temporary in (case / 'exports').glob('.am-export-*.tmp'):
                    old = time.time() - 72 * 3600  # hypothesis: make crashed orphan eligible for normal cleanup
                    os.utime(temporary, (old, old))
                call('resume_export', root)
                assert not list((case / 'exports').glob('.am-export-*.tmp')), '/export/aged_orphan'
                assert (case / 'exports/confirmed.txt').read_text('utf8') == 'Confirmed previous external file', '/export/overwrote_other_file'
            elif not published:
                call('resume_diagnostic', root)
            else:
                assert call('try_diagnostic', root)['message_id'] == 'am.diag.output_exists', '/diagnostic/overwrite'
            if confirmed:
                assert sha(target) == confirmed, '/export/resume_rewrote_confirmed_target'
            with zipfile.ZipFile(target) as exported:
                assert exported.testzip() is None, '/export/crc'
                if operation == 'export_full':
                    assert sha(target) == before[meta['asset_id']]['sha256'], '/export/full_bytes'
                elif operation == 'export_files':
                    assert exported.namelist() == ['result/part.stl'], '/export/selected_names'
                    assert exported.read('result/part.stl') == b'x' * 8192, '/export/selected_bytes'
                else:
                    assert 'diagnostics.json' in exported.namelist(), '/diagnostic/metadata'
        after = zips(root)
        for identifier, old in before.items():
            if identifier not in after:
                assert operation == 'purge' and point in ('after_replace', 'after_committed_journal', 'after_indexed_journal'), '/zip/lost_confirmed_asset'
            elif wal_confirmed:
                assert after[identifier]['sha256'] == wal_confirmed[identifier], '/wal_loss/confirmed_revision_changed'
            elif identifier in evidence:
                assert after[identifier]['sha256'] == evidence[identifier], '/zip/not_verified_revision'
            else:
                assert after[identifier]['sha256'] == old['sha256'], '/zip/confirmed_bytes_changed'
            if operation in ('trash', 'restore'):
                acted = point in ('after_replace', 'after_committed_journal', 'after_indexed_journal')
                expected = ('trash' if acted else 'assets') if operation == 'trash' else ('assets' if acted else 'trash')
                assert after[identifier]['path'].relative_to(root).parts[0] == expected, '/lifecycle/location'
        assert sha(user) == user_sha, '/userdata/changed'
        temporary_root = root / '.am/.am-tmp'
        for path in temporary_root.rglob('*'):
            if not path.is_file():
                continue
            def covers(item):
                reported = Path(item['path'])
                if item['outcome'] not in ('skipped', 'skipped_locked') or not reported.exists():
                    return False
                current = path
                while current != temporary_root:
                    if os.path.samefile(current, reported):
                        return True
                    current = current.parent
                return False
            assert any(covers(item) for item in recovered['recovered']), '/temporary/not_reported'
        before_rebuild = index_snapshot(root / '.am/index.sqlite', 'rebuild')
        call('rebuild', root)
        if orphan:
            assert not orphan.exists(), '/wal_loss/later_cleanup'
        equal(before_rebuild, index_snapshot(root / '.am/index.sqlite', 'rebuild'), '/index/sync_vs_rebuild')
        with closing(sqlite3.connect((root / '.am/state.sqlite').as_uri() + '?mode=ro', uri=True)) as db:
            assert db.execute("SELECT count(*) FROM op_log WHERE code='unconfirmed'").fetchone()[0] == 0, '/log/uncommitted_row'
            if operation == 'wal_loss':
                assert db.execute("SELECT count(*) FROM op_log WHERE code='wal_only'").fetchone()[0] == 0, '/wal_loss/row_not_lost'
                assert db.execute("SELECT count(*) FROM op_log WHERE code='am.library.state_recreated'").fetchone()[0] == 0, '/wal_loss/false_recreated'
        return 1
    # Hypothesis: three independent libraries in flight, one per repetition.
    with ThreadPoolExecutor(max_workers=3) as pool:
        count = sum(pool.map(execute_case, cases))
    kills = count * (2 if args.mode == 'recovery' else 1)
    print(f'PASS FaultProcess.{args.mode}: {count} cases, {kills} killed children; ZIP/userdata preserved, recovered index equals rebuild')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print('FAIL FaultProcess: ' + str(error))
        raise SystemExit(1)
