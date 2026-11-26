"""Exercise declarative import cases through the real CLI, with source invariants."""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tools/fixtures'))
sys.path.insert(0, str(ROOT / 'tests/tools'))
from make_import_cases import check_requirements, fingerprint, generate, payload
from expect_match import mismatch

PROCESS_TIMEOUT = 120  # hypothesis: guard against a stalled CLI, not a performance norm


def compare(expected, actual, location=''):
    error = mismatch(expected, actual, location, json_pointer=True)
    if error:
        raise AssertionError(error)


def assert_source(source, before):
    after = fingerprint(source)
    if after != before:
        keys = [key for key in before.keys() | after.keys() if before.get(key) != after.get(key)]
        raise AssertionError(f'/invariants/source: changed {sorted(keys)!r}')


def assert_temporary(library):
    temporary = library / '.am/.am-tmp/import'
    # Empty managed parent is allowed; leases and per-attempt directories are not.
    if temporary.exists() and any(temporary.iterdir()):
        raise AssertionError(f'/invariants/temp: import area is not empty: {list(temporary.iterdir())}')
    if library.exists() and any(p.is_file() for p in library.rglob('*.am-tmp')):
        raise AssertionError('/invariants/temp: leftover temporary ZIP')


def plan_paths(plan, source):
    paths = {}
    for node in plan['nodes']:
        root = Path(plan['roots'][node['root']]['path'])
        path = root / node['path'] if node['path'] else root
        paths[node['id']] = path.relative_to(source).as_posix()
    for entry in plan.get('archive_entries', []):
        key = paths[entry['archive']] + '!' + entry['path']
        if key in paths.values():
            key += '#' + str(entry['index'])
        paths[entry['id']] = key
    return paths


def scan_view(summary, plan, source):
    paths = plan_paths(plan, source)
    candidates = []
    for candidate in plan['candidates']:
        view = copy.deepcopy(candidate)
        objects = []
        for inp in candidate['inputs']:
            for obj in inp['objects']:
                if 'node' in obj:
                    objects.append(paths[obj['node']])
                else:
                    volume = next(v for v in candidate['volume_sets'] if v['id'] == obj['volume_set'])
                    objects.extend(paths[n] for n in volume['members'])
        view['objects'] = objects
        view['entries'] = {paths[e.get('node', e.get('archive_entry'))]: e for e in candidate['entries']}
        view['reason_flags'] = dict.fromkeys(candidate['reasons'], True)
        view['warning_flags'] = dict.fromkeys(candidate['warnings'], True)
        view['match_flags'] = dict.fromkeys((m['kind'] for m in candidate['matches']), True)
        candidates.append(view)
    return {'summary': summary, 'completeness': plan['scan']['completeness'],
            'limits_hit': plan['scan']['limits_hit'], 'candidates': candidates,
            'nodes': {paths[n['id']]: n for n in plan['nodes']},
            'archives': {paths[a.get('node', a.get('entry'))]: a for a in plan.get('archives', [])},
            'archive_entries': {paths[e['id']]: e for e in plan.get('archive_entries', [])},
            'archive_indices': {paths[a.get('node', a.get('entry'))]:
                {str(e['index']): e for e in plan.get('archive_entries', []) if e['archive'] == a.get('node', a.get('entry'))}
                for a in plan.get('archives', [])}}


def edit_plan(plan, edits, source):
    paths = plan_paths(plan, source)
    def candidate_at(path):
        found = [c for c in plan['candidates'] if any(paths.get(e.get('node', e.get('archive_entry'))) == path for e in c['entries'])]
        if len(found) != 1:
            raise AssertionError(f'/plan_edits: {path!r} matches {len(found)} candidates')
        return found[0]
    for edit in edits:
        operation = edit['op']
        if operation == 'mode':
            plan['mode'] = edit['value']
        elif operation == 'decision':
            matches = [entry for c in plan['candidates'] for entry in c['entries']
                       if paths[entry.get('node', entry.get('archive_entry'))] == edit['path']]
            if len(matches) != 1:
                raise AssertionError(f'/plan_edits: selector {edit["path"]!r} matched {len(matches)} entries')
            matches[0]['decision'] = edit['value']
            # An explicit user decision supersedes an automatic rule's attribution.
            for key in ('rule_id', 'confidence', 'explanation_id'):
                matches[0].pop(key, None)
        elif operation == 'action':
            candidate_at(edit['path'])['action'] = edit['value']
        elif operation == 'merge':
            origin, target = candidate_at(edit['path']), candidate_at(edit['into'])
            if origin is target:
                raise AssertionError('/plan_edits: cannot merge a candidate with itself')
            for key in ('inputs', 'entries', 'directories', 'volume_sets', 'suggested_links', 'filtered_links'):
                target.setdefault(key, []).extend(origin.get(key, []))
            target['base_path'] = ''
            plan['candidates'].remove(origin)
        else:
            raise ValueError(f'unknown plan edit: {operation}')


def source_payloads(config):
    records = {}
    def collect(item, name):
        if item.get('kind') in ('directory', 'symlink', 'golden'):
            return
        records[name] = payload(item)
        if 'zip' in item:
            for index, entry in enumerate(item['zip']['entries']):
                key = name + '!' + entry['name']
                if key in records:
                    key += '#' + str(index)
                collect(entry, key)
    for item in config['source']:
        collect(item, item['path'])
    return records


def asset_views(library, config):
    views = []
    if list((library / 'trash').rglob('*.zip')):
        raise AssertionError('/invariants/assets: unexpected asset in trash')
    originals = source_payloads(config)
    external = {payload(item) for item in config['source'] if item['kind'] == 'zip' and not item.get('preserve_container', False)}
    for path in sorted((library / 'assets').rglob('*.zip')):
        with zipfile.ZipFile(path) as archive:
            manifest = json.loads(archive.read('manifest.json'))
            definitions = [manifest['asset']['id']]
            for collection in ('files', 'inputs', 'groups', 'previews'):
                definitions.extend(value['id'] for value in manifest.get(collection, []))
            for inp in manifest.get('inputs', []):
                definitions.extend(v['id'] for key in ('objects', 'entries') for v in inp[key])
            definitions.extend(s['id'] for s in manifest.get('origin', {}).get('sources', []))
            definitions.extend(c['id'] for s in manifest.get('origin', {}).get('sources', []) for c in s.get('claims', []))
            definitions.extend(e['operation_id'] for e in manifest['history']['entries'])
            if len(definitions) != len(set(definitions)):
                raise AssertionError('/invariants/ids: duplicate definition UUID')
            for identifier in definitions:
                if str(uuid.UUID(identifier)) != identifier or uuid.UUID(identifier).version != 4:
                    raise AssertionError(f'/invariants/ids: not a UUIDv4: {identifier}')
            if manifest['asset']['revision'] != 1:
                raise AssertionError('/invariants/revision: expected revision 1')
            times = [manifest['asset'][key] for key in ('created_at', 'modified_at')]
            times.extend(i['imported_at'] for i in manifest.get('inputs', []))
            times.extend(e['timestamp'] for e in manifest['history']['entries'])
            for value in times:
                if datetime.fromisoformat(value).utcoffset() != timezone.utc.utcoffset(None):
                    raise AssertionError(f'/invariants/time: expected UTC timestamp: {value}')
            files = []
            ready = [item for item in config['source'] if item['kind'] == 'golden']
            owners = {e['file_id']: inp['id'] for inp in manifest.get('inputs', []) for e in inp['entries'] if 'file_id' in e}
            for file in manifest.get('files', []):
                if file['origin_kind'] == 'imported' and (file['id'] not in owners or not file['storage_path'].startswith('content/' + owners[file['id']] + '/')):
                    raise AssertionError('/invariants/storage_path: imported file is outside its InputID')
                # Safe destinations are required even when malicious input names were excluded.
                components = file['storage_path'].split('/')
                if file['storage_path'].startswith('/') or '\\' in file['storage_path'] or any(p in ('', '.', '..') or ':' in p for p in components):
                    raise AssertionError('/invariants/storage_path: unsafe destination')
                data = archive.read(file['storage_path'])
                if hashlib.sha256(data).hexdigest() != file['sha256'] or len(data) != int(file['size_bytes']):
                    raise AssertionError('/invariants/payload: file bytes differ from manifest')
                if data in external:
                    raise AssertionError('/invariants/external_archive: original outer ZIP stored whole')
                if not ready and data not in originals.values():
                    raise AssertionError('/invariants/payload: unrequested conversion or unknown content')
                row = dict(file)
                row['source_paths'] = [name for name, value in originals.items() if data == value]
                row['storage_relative'] = '/'.join(file['storage_path'].split('/')[2:])
                files.append(row)
            # Ready assets must be imported byte for byte, including their ID/revision.
            if ready and not any(path.read_bytes() == (ROOT / 'tests/fixtures/golden' / item['golden'] / 'asset.zip').read_bytes() for item in ready):
                raise AssertionError('/invariants/ready_asset: normalization changed ready ZIP')
            entries = [e for inp in manifest.get('inputs', []) for e in inp['entries']]
            objects = [o for inp in manifest.get('inputs', []) for o in inp['objects']]
            for obj in objects:
                if obj['kind'] in ('file', 'archive'):
                    candidates = [data for name, data in originals.items() if name.rsplit('!', 1)[-1].rsplit('/', 1)[-1] == obj['name']]
                    if candidates and not any(('size_bytes' not in obj or int(obj['size_bytes']) == len(data)) and
                        ('sha256' not in obj or obj['sha256'] == hashlib.sha256(data).hexdigest()) for data in candidates):
                        raise AssertionError('/invariants/objects: recorded input size/SHA does not match source')
            source_contexts = [{'kind': s['kind'], 'name': s.get('name', ''),
                'claims': [{'method': claim['method'], 'has_review': 'review' in claim} for claim in s.get('claims', [])]}
                for s in manifest.get('origin', {}).get('sources', [])]
            object_names = {o['id']: o['name'] for o in objects}
            file_hashes = {f['id']: f['sha256'] for f in files}
            object_entries = {object_names[e['object_id']] + '!' + e['path']:
                {**e, **({'file_sha256': file_hashes[e['file_id']]} if e.get('file_id') in file_hashes else {})} for e in entries}
            views.append({'manifest': manifest, 'files': files, 'has_previews': bool(manifest.get('previews')),
                          'has_sources': bool(manifest.get('origin', {}).get('sources')),
                          'input_count': len(manifest.get('inputs', [])),
                          'object_counts': {kind: sum(o['kind'] == kind for o in objects) for kind in ('file', 'directory', 'archive')},
                          'entries': {e['path']: object_entries[object_names[e['object_id']] + '!' + e['path']] for e in entries},
                          'object_entries': object_entries,
                          'raw_entries': {e['path_raw']['base64']: e for e in entries if 'path_raw' in e},
                          'nested_actions': [o['nested_action'] for o in objects if 'nested_action' in o],
                          'input_completeness': [i['completeness'] for i in manifest.get('inputs', [])],
                          'source_contexts': source_contexts})
    return views


def run_case(am, cases, work, name):
    if not re.fullmatch(r'(?:sc[0-9]{2}|ch)-[a-z0-9-]+', name) or (cases / name).resolve().parent != cases.resolve():
        raise ValueError('/case: expected a direct corpus case directory')
    config = json.loads((cases / name / 'case.json').read_text('utf8'))
    expected = json.loads((cases / name / 'expected.json').read_text('utf8'))
    if config['case'] != name or expected['case'] != name:
        raise AssertionError('/case: directory/name mismatch')
    destination = (work / name).resolve()
    if not (destination.is_relative_to(ROOT / 'build') or destination.is_relative_to(ROOT / '.cache')):
        raise ValueError('work must be inside project build/ or .cache/')
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    reason = check_requirements(config, destination)
    if reason:
        print(f'SKIP {name}: {reason}')
        return 77
    source, library = destination / 'source', destination / 'library'
    before = generate(config, source)
    env = {k: v for k, v in os.environ.items() if not k.startswith('AM_LOG_')}
    profile = destination / 'environment'
    profile.mkdir()
    for key in ('HOME', 'USERPROFILE', 'LOCALAPPDATA', 'XDG_STATE_HOME', 'TMP', 'TEMP', 'TMPDIR'):
        env[key] = str(profile)
    calls = 0
    def cli(arguments, code):
        nonlocal calls
        calls += 1
        result = subprocess.run([str(am), *map(str, arguments), '--json'], cwd=destination,
                                env=env, capture_output=True, encoding='utf8', timeout=PROCESS_TIMEOUT)
        (destination / f'command-{calls:02d}.json').write_text(json.dumps({'args': list(map(str, arguments)),
            'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}, ensure_ascii=False, indent=2), 'utf8')
        if result.returncode != code:
            raise AssertionError(f'/commands/{calls}/exit_code: expected {code}, got {result.returncode}; {result.stdout} {result.stderr}')
        return json.loads(result.stdout)
    cli(['library', 'init', library], 0)
    capabilities = cli(['library', 'info', library], 0)
    storage_readonly = capabilities.get('write_unavailable_reason') == 'storage_read_only'
    scan = config['scan']
    scan_args = list(scan.get('args', []))
    if 'profile_toml' in scan:
        profile_path = destination / 'import.toml'
        profile_path.write_text(scan['profile_toml'], 'utf8')
        scan_args.extend(['--profile', str(profile_path)])
    if scan.get('library'):
        scan_args.extend(['--library', str(library)])
    roots = [source if p == '.' else source / p for p in scan.get('roots', ['.'])]
    readonly = False
    plan_path = None
    retained_assets = {}
    try:
        for index, step in enumerate(config['steps']):
            command = step['command']
            label = step.get('name', command)
            code = step['exit_code']
            extra = [str(library) if value == '@library' else value for value in step.get('args', [])]
            if command in ('scan', 'rescan'):
                plan_path = destination / f'plan-{index}.json'
                args = ['import', 'scan', *roots, '--plan', plan_path, *scan_args, *extra]
                summary = cli(args, code)
                plan = json.loads(plan_path.read_text('utf8'))
                expectation = expected['steps'][label]
                if readonly and command == 'rescan':
                    expectation = expected.get('read_only_steps', {}).get(label, expectation)
                compare(expectation, scan_view(summary, plan, source), '/' + label)
                if readonly and command == 'rescan':
                    print(f'PASS {label}: empty-library expectation; post-import matching NOT_APPLICABLE on this filesystem')
                if command == 'scan':
                    edit_plan(plan, config.get('plan_edits', []), source)
                    plan_path.write_text(json.dumps(plan, ensure_ascii=False, indent=2), 'utf8')
            elif command in ('run', 'add-ready'):
                if step.get('plan_edits'):
                    plan = json.loads(plan_path.read_text('utf8'))
                    edit_plan(plan, step['plan_edits'], source)
                    plan_path.write_text(json.dumps(plan, ensure_ascii=False, indent=2), 'utf8')
                args = ['import', 'run', '--plan', plan_path, '--library', library] if command == 'run' else ['library', 'add-ready', library, source / step['path']]
                if storage_readonly:
                    result = cli([*args, *extra], 4)
                    compare({'error': {'message_id': 'am.library.storage_read_only'}}, result, '/' + label)
                    if list((library / 'assets').rglob('*.zip')) or list((library / 'trash').rglob('*.zip')):
                        raise AssertionError('/invariants/read_only: refusal left an asset')
                    readonly = True
                    print(f'PASS {label}: am.library.storage_read_only; write expectations not applicable on this filesystem')
                    continue
                result = cli([*args, *extra], code)
                compare(expected['steps'][label], {'summary': result, 'assets': asset_views(library, config)}, '/' + label)
            elif command == 'verify':
                if readonly:
                    print(f'NOT_APPLICABLE {label}: not applicable on this filesystem')
                    continue
                report = cli(['library', 'verify', library, '--full', *extra], code)
                compare(expected['steps'][label], report, '/' + label)
                if report.get('corrupt', 0):
                    raise AssertionError('/verify/corrupt: corrupt asset')
            else:
                raise ValueError(f'unknown command: {command}')
            assert_source(source, before)
            assert_temporary(library)
            # Subsequent scans, skips and verification never authorize replacing an asset.
            for path, digest in retained_assets.items():
                if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                    raise AssertionError('/invariants/assets: existing asset changed without permission')
            retained_assets.update({p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (library / 'assets').rglob('*.zip')})
    finally:
        assert_source(source, before)
        assert_temporary(library)
    checks = 'source/temp invariants checked; asset checks NOT_APPLICABLE' if readonly else 'source/temp/payload/UUID invariants checked'
    print(f'PASS import_corpus.{name}: {calls} CLI calls; {checks}')
    return 0


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf8', errors='backslashreplace')
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('am', 'cases', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--case', required=True)
    args = parser.parse_args()
    try:
        return run_case(args.am.resolve(), args.cases.resolve(), args.work.resolve(), args.case)
    except (AssertionError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(f'FAIL import_corpus.{args.case}: {error}')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
