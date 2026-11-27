"""P0.8: actual CLI transfer, ZIP-derived index equality and portability bundles."""
import argparse
import copy
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tools/fixtures'))
sys.path.insert(0, str(ROOT / 'tests/tools'))
from make_import_cases import generate, fingerprint
from expect_match import mismatch

# Descriptions and expectations are executable declarations, not captured CLI output.
CASES = {
    'e2e-import-transfer': ('K-10', 'K-22', 'K-31'),
    'e2e-ready-transfer': ('K-10', 'K-22', 'K-31'),
    'e2e-copy-transfer': ('K-10', 'K-22', 'K-31', 'ADR-0036'),
    'e2e-index-rebuild': ('K-22', 'K-122'),
    'e2e-trash-roundtrip': ('K-10', 'K-31'),
    'e2e-selective-export': ('K-10', 'K-22'),
}
SOURCE = {'source': [
    {'kind': 'file', 'path': 'model.stl', 'content': 'solid e2e\nendsolid e2e\n'},
    {'kind': 'file', 'path': 'images/preview.png', 'hex': '89504e470d0a1a0a'},
    {'kind': 'file', 'path': 'notes.txt', 'content': 'Synthetic source; no original archive required.'},
    {'kind': 'zip', 'path': 'parts.zip', 'zip': {'entries': [
        {'name': 'body.step', 'content': 'ISO-10303-21; E2E STEP'},
        {'name': 'inner.zip', 'zip': {'entries': [
            {'name': 'detail.stl', 'content': 'solid detail\nendsolid detail\n'},
            {'name': 'info.txt', 'content': 'Nested information'}]}}]}}]}

# V-2: these are the only omitted logical values. Internal FTS row identifiers
# are replaced by row contents, not dropped together with their relationships.
LOCAL_FIELDS = {
    'transfer': {'assets': {'rel_path', 'in_trash', 'zip_mtime_ns'}},
    # Within one library rebuild must restore location and trash state from ZIP placement.
    'rebuild': {},
}
LOCAL_META = {'rebuilt_at', 'writer_session'}
FTS_TABLES = {'fts_assets', 'fts_names', 'fts_docs'}
OWNER_FTS = {'fts_names_owner': 'fts_names', 'fts_docs_owner': 'fts_docs'}
PROCESS_TIMEOUT = 120  # hypothesis: stalled-process guard, not a product limit
BUNDLE_BYTES = 2 * 1024 * 1024  # hypothesis from T-035


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(',', ':'), allow_nan=False)


def equal(expected, actual, pointer=''):
    error = mismatch(expected, actual, pointer, json_pointer=True, exact=True)
    if error:
        raise AssertionError(error)


def subset(expected, actual, pointer=''):
    error = mismatch(expected, actual, pointer, json_pointer=True)
    if error:
        raise AssertionError(error)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def work_path(path):
    path = path.resolve()
    if not any(path != ROOT / name and path.is_relative_to(ROOT / name) for name in ('build', '.cache')):
        raise ValueError('work must be inside project build/ or .cache/')
    return path


def sql_name(value):
    return '"' + value.replace('"', '""') + '"'


def sql_value(value):
    return {'blob': value.hex()} if isinstance(value, bytes) else value


def index_snapshot(path, purpose='transfer'):
    """Read every logical table and FTS token instance; main DB stays mode=ro."""
    result = {}
    with closing(sqlite3.connect(path.resolve().as_uri() + '?mode=ro', uri=True)) as db:
        db.execute('PRAGMA temp_store=MEMORY')
        tables = {r[1]: r[2] for r in db.execute('PRAGMA main.table_list') if r[1] != 'sqlite_schema'}
        fts_rows = {}
        for name in FTS_TABLES:
            cursor = db.execute('SELECT rowid,* FROM ' + sql_name(name))
            keys = [d[0] for d in cursor.description]
            fts_rows[name] = {r[0]: dict(zip(keys[1:], map(sql_value, r[1:]))) for r in cursor}
        for name, kind in sorted(tables.items()):
            if kind == 'shadow':
                continue  # V-2: physical pages are represented by the FTS rows/tokens below.
            cursor = db.execute('SELECT * FROM ' + sql_name(name))
            keys = [d[0] for d in cursor.description]
            rows = []
            for values in cursor:
                row = dict(zip(keys, map(sql_value, values)))
                if name == 'meta' and row['key'] in LOCAL_META:
                    continue
                for key in LOCAL_FIELDS[purpose].get(name, set()):
                    row.pop(key, None)
                if name in OWNER_FTS:
                    row['fts_rowid'] = fts_rows[OWNER_FTS[name]][row['fts_rowid']]
                if name == 'suggestions':
                    row.pop('id', None)  # Explicit INTEGER PRIMARY KEY, local rowid in index.sql.
                rows.append(row)
            result[name] = {'columns': [k for k in keys if k not in LOCAL_FIELDS[purpose].get(name, set())
                                       and not (name == 'suggestions' and k == 'id')],
                            'rows': sorted(rows, key=canonical)}
        for name in sorted(FTS_TABLES):
            db.execute(f'CREATE VIRTUAL TABLE temp.{sql_name("vocab_" + name)} USING fts5vocab(main,{sql_name(name)},instance)')
            records = []
            for term, doc, column, offset in db.execute('SELECT term,doc,col,offset FROM temp.' + sql_name('vocab_' + name)):
                records.append({'term': term, 'doc': fts_rows[name][doc], 'column': column, 'offset': offset})
            result[name]['tokens'] = sorted(records, key=canonical)
        result['$user_version'] = db.execute('PRAGMA user_version').fetchone()[0]
    return result


def normalize_list(value):
    rows = copy.deepcopy(value['assets'])
    for row in rows:
        # Paths and trash membership identify a library location, not ZIP data.
        for key in ('rel_path', 'in_trash'):
            row.pop(key, None)
    return sorted(rows, key=canonical)


def userdata_empty(library):
    with closing(sqlite3.connect((library / '.am/userdata.sqlite').resolve().as_uri() + '?mode=ro', uri=True)) as db:
        for (name,) in db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"):
            if db.execute('SELECT count(*) FROM ' + sql_name(name)).fetchone()[0]:
                raise AssertionError('/userdata/' + name + ': clean library inherited user data')


def manifest(path):
    with zipfile.ZipFile(path) as z:
        return json.loads(z.read('manifest.json'))


def zip_paths(library):
    result = {}
    for category in ('assets', 'trash'):
        for path in (library / category).rglob('*.zip'):
            identifier = manifest(path)['asset']['id']
            if identifier in result:
                raise AssertionError('/assets: duplicate AssetID across locations')
            result[identifier] = path
    return result


class Cli:
    def __init__(self, am, work, environment='environment'):
        self.am = am.resolve()
        self.work = work
        profile = work / environment
        profile.mkdir(parents=True, exist_ok=True)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('AM_LOG_')}
        for key in ('HOME', 'USERPROFILE', 'LOCALAPPDATA', 'XDG_STATE_HOME', 'TMP', 'TEMP', 'TMPDIR'):
            self.env[key] = str(profile)
        self.calls = 0
        self.blocked_paths = []

    def __call__(self, *args, code=0):
        assert_isolated_arguments(args, self.work, self.blocked_paths)
        self.calls += 1
        run = subprocess.run([str(self.am), *map(str, args), '--json'], cwd=self.work, env=self.env,
                             capture_output=True, encoding='utf8', timeout=PROCESS_TIMEOUT)
        record = {'args': list(map(str, args)), 'exit_code': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
        (self.work / f'{Path(self.env["HOME"]).name}-command-{self.calls:03d}.json').write_text(canonical(record), 'utf8')
        if run.returncode != code:
            raise AssertionError(f'/cli/{self.calls}/exit_code: expected {code}, got {run.returncode}; {run.stdout} {run.stderr}')
        return json.loads(run.stdout)


def init(cli, path):
    cli('library', 'init', path)
    info = cli('library', 'info', path)
    subset({'databases': {'index': {'rebuild_required': False}}}, info, '/init/info')
    if 'error' in info:
        raise AssertionError('/init/info: unexpected index warning')
    subset({'assets': [], 'rebuild_required': False}, cli('library', 'list', path), '/init/list')
    return info['write_unavailable_reason'] == 'storage_read_only'


def snapshot(cli, library, purpose='transfer'):
    paths = zip_paths(library)
    return {'list': normalize_list(cli('library', 'list', library, '--trash')),
            'inspect': {key: cli('asset', 'inspect', path, code=3 if manifest(path)['format']['version']['minor'] else 0)
                        for key, path in sorted(paths.items())},
            'index': index_snapshot(library / '.am/index.sqlite', purpose),
            'zip_sha256': {key: digest(path) for key, path in sorted(paths.items())}}


def check_zip(cli, path):
    # Verdicts follow the immutable golden contract, including legitimate partial verification.
    m = manifest(path)
    label = next((p.parent.name for p in (ROOT / 'tests/fixtures/golden').glob('pos-*/asset.zip') if digest(p) == digest(path)), None)
    checks = json.loads((ROOT / 'tests/fixtures/golden' / label / 'expected.json').read_text('utf8'))['checks'] if label else [
        {'command': 'validate', 'exit_code': 0, 'json': {'v1': 'passed', 'v2': 'passed', 'v3': 'passed'}},
        {'command': 'verify', 'args': ['--level', 'full'], 'exit_code': 0, 'json': {'container_verification': 'full'}}]
    for check in checks:
        value = cli('asset', check['command'], path, *check.get('args', []), code=check['exit_code'])
        subset(check['json'], value, '/' + check['command'])
    return m


def seed_userdata(library):
    with closing(sqlite3.connect(library / '.am/userdata.sqlite')) as db:
        db.execute("INSERT INTO collections(coll_id,name,kind,created_at) VALUES('e2e-local','Local favorites','favorites','2020-01-01T00:00:00Z')")
        db.commit()


def assert_invariants(source, before, library):
    if fingerprint(source) != before:
        raise AssertionError('/source: input tree changed')
    area = library / '.am/.am-tmp/import'
    if area.exists() and any(area.iterdir()):
        raise AssertionError('/temporary: import area not empty')
    if any(p.is_file() for p in library.rglob('*.am-tmp')):
        raise AssertionError('/temporary: commit ZIP remains')


def imported(cli, work, library):
    source = work / 'source'
    before = generate(SOURCE, source)
    plan = work / 'plan.json'
    cli('import', 'scan', source, '--plan', plan)
    p = json.loads(plan.read_text('utf8'))
    subset({'version': 3, 'scan': {'completeness': 'complete'}, 'candidates': [{'action': 'import'}]}, p, '/plan')
    report = cli('import', 'run', '--plan', plan, '--library', library)
    subset({'candidates': [{'status': 'created'}]}, report, '/run')
    assert_invariants(source, before, library)
    check_zip(cli, next(iter(zip_paths(library).values())))
    return source, before


def export_all(cli, library, output, prefix='asset'):
    output.mkdir(parents=True, exist_ok=True)
    result = []
    for index, (identifier, path) in enumerate(sorted(zip_paths(library).items())):
        name = f'{prefix}-{index}'
        report = cli('export', library, identifier, '--to', output, '--name', name)
        subset({'assets': [{'status': 'exported', 'asset_id': identifier}]}, report)
        target = output / (name + '.zip')
        equal(digest(path), digest(target), '/export/sha256')
        result.append(target)
    return result


def conceal(paths):
    """Make original paths unavailable before B exists; no fallback discovery."""
    hidden = []
    for path in paths:
        destination = path.with_name(path.name + '-unavailable')
        path.rename(destination)
        hidden.append(destination)
    assert_unavailable(paths)
    return hidden


def assert_unavailable(paths):
    for path in paths:
        if path.exists():
            raise AssertionError('/isolation: original path remains accessible: ' + str(path))


def assert_isolated_arguments(args, cwd, blocked):
    for arg in args:
        path = Path(str(arg))
        if not path.is_absolute():
            path = cwd / path
        path = path.resolve()
        if any(path.is_relative_to(root.resolve()) for root in blocked):
            raise AssertionError('/isolation: attempted access to A, its environment or source: ' + str(arg))


def transfer(cli, work, a, source=None, prefix='transfer'):
    before = snapshot(cli, a)
    output = work / 'exports'
    paths = export_all(cli, a, output, prefix)
    if source:
        assert_invariants(source[0], source[1], a)
    seed_userdata(a)
    originals = [a] + ([source[0]] if source else [])
    hidden = conceal(originals)
    b = work / 'B'
    other = Cli(cli.am, work, 'B-environment')
    other.blocked_paths = originals + hidden + [Path(cli.env['HOME'])]
    init(other, b)
    for path in paths:
        result = other('library', 'add-ready', b, path)
        subset({'results': [{'status': 'imported'}]}, result)
    assert_unavailable(originals)
    equal(before, snapshot(other, b), '/transfer')
    userdata_empty(b)
    verified = other('library', 'verify', b, '--full')
    subset({'corrupt': 0}, verified)
    return b


def selective(cli, library, output):
    output.mkdir(parents=True, exist_ok=True)
    files = []
    for identifier, path in sorted(zip_paths(library).items()):
        m = manifest(path)
        for preset in ('3d', '3d_preview', 'slicer', 'all'):
            # Import fixture uses only suffix classification and no preview links.
            suffixes = {'3d': {'stl', 'step'}, '3d_preview': {'stl', 'step', 'png'}, 'slicer': {'stl'},
                        'all': {'stl', 'step', 'png', 'txt'}}[preset]
            selected = [f for f in m['files'] if f['storage_path'].rsplit('.', 1)[-1] in suffixes]
            name = preset + '-' + identifier
            report = cli('export', library, identifier, '--to', output, '--name', name, '--files', preset)
            subset({'assets': [{'status': 'exported', 'mode': 'files'}]}, report)
            target = output / (name + '.zip')
            expected = {}
            with zipfile.ZipFile(path) as original, zipfile.ZipFile(target) as z:
                for f in selected:
                    relative = '/'.join(f['storage_path'].split('/')[2:])
                    expected[name + '/' + relative] = original.read(f['storage_path'])
                equal(sorted(expected), z.namelist(), '/selective/names')
                for key, data in expected.items():
                    equal(hashlib.sha256(data).hexdigest(), hashlib.sha256(z.read(key)).hexdigest(), '/selective/' + key)
                if 'manifest.json' in z.namelist() or z.comment:
                    raise AssertionError('/selective: asset marker or manifest leaked')
            files.append((target, {key: hashlib.sha256(data).hexdigest() for key, data in expected.items()}))
    return files


def read_only_case(cli, work, library, name):
    source = work / 'source'
    before = generate(SOURCE, source)
    cli('import', 'scan', source, '--plan', work / 'plan.json')
    rejected = cli('import', 'run', '--plan', work / 'plan.json', '--library', library, code=4)
    subset({'error': {'message_id': 'am.library.storage_read_only'}}, rejected)
    golden = ROOT / 'tests/fixtures/golden/pos-full/asset.zip'
    for args in [('library', 'add-ready', library, golden), ('library', 'rebuild', library), ('library', 'sync', library, '--full')]:
        subset({'error': {'message_id': 'am.library.storage_read_only'}}, cli(*args, code=4))
    check_zip(cli, golden)
    assert_invariants(source, before, library)
    if zip_paths(library):
        raise AssertionError('/read_only: denied operation published an asset')
    print(f'PASS {name}: scan/read validation and storage_read_only refusals; write chain NOT_APPLICABLE')


def run_case(am, work, name):
    if name not in CASES:
        raise ValueError('unknown e2e case')
    work = work_path(work / name)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    cli = Cli(am, work)
    a = work / 'A'
    if init(cli, a):
        read_only_case(cli, work, a, name)
        return
    if name == 'e2e-index-rebuild':
        cli('library', 'sync', a)  # A fresh empty index needs no rebuild.
    if name == 'e2e-ready-transfer':
        for index, golden in enumerate(sorted((ROOT / 'tests/fixtures/golden').glob('pos-*/asset.zip'))):
            sub = work / golden.parent.name
            sub.mkdir()
            c = Cli(am, sub)
            a = sub / 'A'
            init(c, a)
            check_zip(c, golden)
            subset({'results': [{'status': 'imported'}]}, c('library', 'add-ready', a, golden))
            if golden.parent.name == 'pos-newer-minor':
                verified_copy = sub / 'full-verification'
                verified_copy.mkdir()
                identifier = manifest(golden)['asset']['id']
                c('export', a, identifier, '--to', verified_copy, '--name', 'full', '--verify', 'full')
                equal(digest(golden), digest(verified_copy / 'full.zip'), '/newer_minor/full/sha256')
                refused = sub / 'selective-refused'
                refused.mkdir()
                subset({'assets': [{'error': {'message_id': 'am.export.source_invalid'}}]},
                       c('export', a, identifier, '--to', refused, '--files', 'all', code=4))
                if list(refused.iterdir()):
                    raise AssertionError('/newer_minor: refused selection left files')
            b = transfer(c, sub, a, prefix=golden.parent.name)
            if golden.parent.name == 'pos-newer-minor':
                with closing(sqlite3.connect((b / '.am/index.sqlite').as_uri() + '?mode=ro', uri=True)) as db:
                    equal([('warn', 'am.catalog.newer_minor')], db.execute('SELECT health,health_detail FROM assets').fetchall(), '/newer_minor/health')
    else:
        source = imported(cli, work, a)
        if name == 'e2e-copy-transfer':
            path = next(iter(zip_paths(a).values()))
            original = manifest(path)
            cli('library', 'copy', a, original['asset']['id'])
            copies = [manifest(p) for key, p in zip_paths(a).items() if key != original['asset']['id']]
            if len(copies) != 1:
                raise AssertionError('/copy: expected one new asset')
            copy_doc = copies[0]
            subset({'asset': {'revision': 1}, 'history': {'entries': [{'revision': 1, 'operation': 'asset_created',
                'details': {'creation_kind': 'independent_copy', 'source_asset': {'asset_id': original['asset']['id'], 'revision': original['asset']['revision']}}}]}}, copy_doc)
            equal(sorted(f['sha256'] for f in original['files']), sorted(f['sha256'] for f in copy_doc['files']), '/copy/files')
        if name == 'e2e-trash-roundtrip':
            identifier, path = next(iter(zip_paths(a).items()))
            old = digest(path)
            cli('library', 'trash', a, identifier)
            equal(old, digest(zip_paths(a)[identifier]), '/trash/sha256')
            output = work / 'trash-export'
            output.mkdir()
            subset({'assets': [{'error': {'message_id': 'am.export.asset_in_trash'}}]},
                   cli('export', a, identifier, '--to', output, code=4))
            if list(output.iterdir()):
                raise AssertionError('/trash/export: refusal left files')
            cli('library', 'restore', a, identifier)
            equal(old, digest(zip_paths(a)[identifier]), '/restore/sha256')
        if name == 'e2e-index-rebuild':
            # Include rich, distinct-ID metadata alongside the imported fixture.
            cli('library', 'add-ready', a, ROOT / 'tests/fixtures/golden/pos-full/asset.zip')
            trashed = manifest(ROOT / 'tests/fixtures/golden/pos-full/asset.zip')['asset']['id']
            cli('library', 'trash', a, trashed)  # rel_path/in_trash must survive rebuild
            cli('library', 'sync', a)
            original = snapshot(cli, a, 'rebuild')
            cli('library', 'rebuild', a)
            equal(original, snapshot(cli, a, 'rebuild'), '/rebuild')
            index = a / '.am/index.sqlite'
            for suffix in ('', '-wal', '-shm'):
                Path(str(index) + suffix).unlink(missing_ok=True)
            refused = cli('library', 'sync', a, '--full', code=4)
            subset({'error': {'message_id': 'am.catalog.rebuild_required'}}, refused, '/missing-index')
            subset({'databases': {'index': {'rebuild_required': True}}}, cli('library', 'info', a))
            cli('library', 'rebuild', a)
            equal(original, snapshot(cli, a, 'rebuild'), '/missing-index-open')
            cli('library', 'sync', a, '--full')
            equal(original, snapshot(cli, a, 'rebuild'), '/sync-full')
            with closing(sqlite3.connect(index.resolve().as_uri() + '?mode=ro', uri=True)) as db:
                equal([(1,)], db.execute('SELECT in_trash FROM assets WHERE asset_id=?', (trashed,)).fetchall(), '/rebuild/in_trash')
            cli('library', 'restore', a, trashed)
        if name == 'e2e-selective-export':
            selective(cli, a, work / 'selected')
        transfer(cli, work, a, source)
    print('PASS ' + name + ': isolated transfer, full index/FTS equality and ZIP bytes')


def bundle(am, work, output):
    work = work_path(work / 'bundle-source')
    output = work_path(output)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    cli = Cli(am, work)
    a = work / 'library'
    if init(cli, a):
        print('NOT_APPLICABLE bundle creation: storage_read_only; use Windows bundle with check_portability.py')
        return
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    source = imported(cli, work, a)
    selected = selective(cli, a, output / 'selected')
    cli('library', 'add-ready', a, ROOT / 'tests/fixtures/golden/pos-full/asset.zip')
    cli('library', 'add-ready', a, ROOT / 'tests/fixtures/golden/pos-minimal/asset.zip')
    exported = export_all(cli, a, output / 'assets')
    library_exports = {manifest(p)['asset']['id']: p.relative_to(output).as_posix() for p in exported}
    # Separate libraries preserve every positive golden despite overlapping IDs.
    for golden in sorted((ROOT / 'tests/fixtures/golden').glob('pos-*/asset.zip')):
        sub = work / golden.parent.name
        sub.mkdir()
        other = Cli(am, sub)
        ready = sub / 'library'
        init(other, ready)
        other('library', 'add-ready', ready, golden)
        exported.extend(export_all(other, ready, output / 'assets', golden.parent.name))
        conceal([ready])
    trash_id = manifest(ROOT / 'tests/fixtures/golden/pos-minimal/asset.zip')['asset']['id']
    cli('library', 'trash', a, trash_id)
    # Source and A are made unavailable to every consumer of the finished bundle.
    state = snapshot(cli, a)
    originals = [a, source[0]]
    equal(source[1], fingerprint(source[0]), '/source')
    hidden = conceal(originals)
    a = hidden[0]
    with zipfile.ZipFile(output / 'library.zip', 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for path in sorted(a.rglob('*')):
            if path.is_file():
                z.write(path, path.relative_to(a).as_posix())
    records = {'format': 'am-e2e-portability', 'version': 1, 'snapshot': state, 'trash_ids': [trash_id],
               'library_exports': library_exports,
               'assets': [{'path': p.relative_to(output).as_posix(),
                           'inspect': cli('asset', 'inspect', p, code=3 if manifest(p)['format']['version']['minor'] else 0)} for p in exported],
               'selected': [{'path': p.relative_to(output).as_posix(), 'entries': entries} for p, entries in selected]}
    records['files'] = {p.relative_to(output).as_posix(): {'sha256': digest(p), 'size': p.stat().st_size}
                        for p in sorted(output.rglob('*')) if p.is_file()}
    (output / 'bundle.json').write_text(canonical(records), 'utf8')
    total = sum(p.stat().st_size for p in output.rglob('*') if p.is_file())
    if total > BUNDLE_BYTES:
        raise AssertionError(f'/bundle/size: {total} exceeds {BUNDLE_BYTES} byte CI hypothesis')
    print(f'PASS e2e_portability_bundle: {len(records["files"])} files, {total} bytes')


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf8', errors='backslashreplace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--am', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--case', choices=sorted(CASES))
    parser.add_argument('--bundle', type=Path)
    args = parser.parse_args()
    try:
        if args.bundle:
            bundle(args.am, args.work, args.bundle)
        else:
            run_case(args.am, args.work, args.case)
        return 0
    except (AssertionError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        print('FAIL e2e: ' + str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
