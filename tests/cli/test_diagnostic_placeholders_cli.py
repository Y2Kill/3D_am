"""CLI regression coverage for SC-3 / NFR-14.4.9 / K-130, including raw metadata injection."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import uuid
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--am', required=True)
parser.add_argument('--driver', required=True)
parser.add_argument('--root', required=True)
parser.add_argument('--metadata-root', required=True)
parser.add_argument('--case', required=True, choices=['json', 'text', 'unchanged'])
args = parser.parse_args()
work = Path(args.root).resolve() / uuid.uuid4().hex
work.mkdir(parents=True)
logs = work / 'logs'
logs.mkdir()
name = 'am-20261124T000000Z-999999991.jsonl'
record = {'v': 1, 'ts': '2026-11-24T00:00:00.000Z', 'level': 'info', 'component': 'test',
          'event': 'test.fixture', 'msg': 'LocalDiagnosticCanary', 'pid': 123, 'tid': 1,
          'ctx': dict.fromkeys(['job_id', 'asset_id', 'library_id', 'operation']), 'fields': {}, 'kinds': {}}
(logs / name).write_text(json.dumps(record) + '\n', encoding='utf8')
env = os.environ.copy()
for variable in ['HOME', 'USERPROFILE', 'TMP', 'TEMP', 'TMPDIR']:
    env[variable] = str(work)
env['AM_LOG'] = 'off'
env.pop('AM_LOG_DIR', None)
env.pop('AM_TEST_DIAG_PLACEHOLDER', None)

def invoke(executable, label, json_output, inject=False):
    child_env = env.copy()
    if inject:
        child_env['AM_TEST_DIAG_PLACEHOLDER'] = name
    output = work / (label + '.zip')
    command = [executable, 'diag', 'export', str(output), '--log-dir', str(logs)]
    if json_output:
        command.append('--json')
    result = subprocess.run(command, env=child_env, capture_output=True, timeout=90)
    assert result.returncode == 0, (result.returncode, result.stderr.decode('utf8', 'replace'))
    assert output.exists()
    return result.stdout, output

if args.case == 'unchanged':
    normal_json, _ = invoke(args.am, 'legacy-contract', True)
    measured = json.loads(normal_json)['replacements']
    assert set(measured) == {'path', 'name', 'user', 'host'}
    replacements = {key: measured[key] for key in ['path', 'name', 'user', 'host']}
    legacy = {'command': 'diag export', 'entries': ['diagnostics.json', 'logs/' + name],
              'logs_available': True, 'dry_run': False, 'replacements': replacements}
    encoded = json.dumps(legacy, separators=(',', ':')).encode('utf8')
    eol = b'\r\n' if os.name == 'nt' else b'\n'
    assert normal_json == encoded + eol, normal_json
    for json_output in [False, True]:
        actual, _ = invoke(args.am, 'production-' + str(json_output), json_output)
        driver, _ = invoke(args.driver, 'driver-' + str(json_output), json_output)
        assert actual == driver, (actual, driver)
        expected = encoded + eol if json_output else b'Diagnostic export: ' + encoded + eol
        assert actual == expected, (actual, expected)
        assert b'unavailable' not in actual
    # A test-only environment variable must not alter the production executable.
    actual, _ = invoke(args.am, 'production-injection-ignored', True, inject=True)
    assert 'unavailable' not in json.loads(actual)
else:
    stdout, output = invoke(args.driver, args.case, args.case == 'json', inject=True)
    expected = [{'source_kind': 'log', 'name': name, 'reason': 'cloud_placeholder'}]
    with zipfile.ZipFile(output) as archive:
        assert 'logs/' + name not in archive.namelist()
        metadata = json.loads(archive.read('diagnostics.json'))
        assert metadata['unavailable'] == expected
        assert all(b'LocalDiagnosticCanary' not in archive.read(entry) for entry in archive.namelist())
    if args.case == 'json':
        assert json.loads(stdout)['unavailable'] == expected
    else:
        lines = stdout.decode('utf8').splitlines()
        assert lines[-1] == 'Skipped 1 unavailable source(s): cloud_placeholder.', lines
        assert sum('Skipped ' in line for line in lines) == 1
    metadata_root = Path(args.metadata_root)
    metadata_root.mkdir(parents=True, exist_ok=True)
    (metadata_root / ('cli-cloud-' + args.case + '.json')).write_text(json.dumps(metadata), encoding='utf8')
print('PASS diagnostic placeholders ' + args.case)
