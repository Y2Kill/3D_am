"""Verify a Windows portability bundle using the same entry point in WSL and CI."""
import argparse
from contextlib import closing
import json
from pathlib import Path, PurePosixPath
import shutil
import sqlite3
import sys
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_e2e import Cli, check_zip, digest, equal, snapshot, subset, work_path, zip_paths


def safe_member(name):
    parts = name.split('/')
    if not name or name.startswith('/') or '\\' in name or any(p in ('', '.', '..') or ':' in p for p in parts):
        raise AssertionError('/bundle/path: unsafe member ' + name)
    return PurePosixPath(name)


def verify_files(bundle, record):
    if record.get('format') != 'am-e2e-portability' or record.get('version') != 1:
        raise AssertionError('/bundle: unsupported format/version')
    actual = {p.relative_to(bundle).as_posix() for p in bundle.rglob('*') if p.is_file() and p != bundle / 'bundle.json'}
    equal(sorted(record['files']), sorted(actual), '/bundle/files')
    for name, expected in record['files'].items():
        path = bundle / safe_member(name)
        if path.is_symlink() or not path.resolve().is_relative_to(bundle.resolve()):
            raise AssertionError('/bundle/path: link outside bundle')
        equal(expected, {'sha256': digest(path), 'size': path.stat().st_size}, '/bundle/files/' + name)


def database_rows(library):
    result = {}
    for name in ('index', 'userdata', 'state'):
        with closing(sqlite3.connect((library / '.am' / (name + '.sqlite')).resolve().as_uri() + '?mode=ro', uri=True)) as db:
            tables = db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").fetchall()
            result[name] = {t: sorted(db.execute('SELECT * FROM "' + t.replace('"', '""') + '"').fetchall(), key=repr) for (t,) in tables}
    return result


def check(am, bundle, work):
    bundle = bundle.resolve()
    record = json.loads((bundle / 'bundle.json').read_text('utf8'))
    verify_files(bundle, record)  # All hashes first, before even starting the CLI.
    work = work_path(work)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    cli = Cli(am, work, 'linux-environment')
    for asset in record['assets']:
        path = bundle / safe_member(asset['path'])
        check_zip(cli, path)
        equal(asset['inspect'], cli('asset', 'inspect', path, code=3 if asset['inspect']['format_support'] == 'newer_minor' else 0), '/inspect')
    for item in record['selected']:
        with zipfile.ZipFile(bundle / safe_member(item['path'])) as z:
            equal(sorted(item['entries']), sorted(z.namelist()), '/selected/names')
            for name, sha in item['entries'].items():
                import hashlib
                equal(sha, hashlib.sha256(z.read(name)).hexdigest(), '/selected/' + name)
            if z.comment or 'manifest.json' in z.namelist():
                raise AssertionError('/selected: asset marker present')
    library = work / 'library'
    library.mkdir()
    with zipfile.ZipFile(bundle / 'library.zip') as z:
        for info in z.infolist():
            name = safe_member(info.filename)
            if ((info.external_attr >> 16) & 0o170000) == 0o120000:
                raise AssertionError('/bundle/library: symlink member')
            target = library / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(z.read(info))
    before = database_rows(library)
    zips = {k: digest(p) for k, p in zip_paths(library).items()}
    info = cli('library', 'info', library)
    subset({'mode': 'read_only', 'capabilities': {'mode': 'read_only_default'}, 'write_unavailable_reason': 'storage_read_only'}, info)
    equal(record['snapshot'], snapshot(cli, library), '/library')
    rows = cli('library', 'list', library, '--trash')['assets']
    equal(sorted(record['trash_ids']), sorted(r['asset_id'] for r in rows if r['in_trash']), '/library/trash_ids')
    subset({'corrupt': 0}, cli('library', 'verify', library, '--full'))
    output = work / 'export'
    output.mkdir()
    by_id = {key: bundle / safe_member(path) for key, path in record['library_exports'].items()}
    for index, row in enumerate(r for r in rows if not r['in_trash']):
        name = 'linux-' + str(index)
        report = cli('export', library, row['asset_id'], '--to', output, '--name', name)
        subset({'queued': False, 'assets': [{'status': 'exported'}]}, report)
        equal(digest(by_id[row['asset_id']]), digest(output / (name + '.zip')), '/readonly-export')
    rejected = cli('library', 'add-ready', library, bundle / record['assets'][0]['path'], code=4)
    subset({'error': {'message_id': 'am.library.storage_read_only'}}, rejected)
    equal(before, database_rows(library), '/readonly-databases')
    equal(zips, {k: digest(p) for k, p in zip_paths(library).items()}, '/readonly-zips')
    print(f'PASS portability-linux: {len(record["assets"])} standalone ZIPs, selective files, index/FTS, trash, read-only export/refusal')


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf8', errors='backslashreplace')
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('am', 'bundle', 'work'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    try:
        check(args.am.resolve(), args.bundle, args.work)
        return 0
    except (AssertionError, ValueError, OSError, zipfile.BadZipFile) as error:
        print('FAIL portability-linux: ' + str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
