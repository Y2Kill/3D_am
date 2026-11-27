"""T-036: state recreation is visible in JSON, library info and operation history."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--am', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    work = args.root.resolve() / str(uuid.uuid4())
    work.mkdir(parents=True)
    library = work / 'library'
    environment = os.environ.copy()
    profile = work / 'profile'
    profile.mkdir()
    for key in ('HOME', 'USERPROFILE', 'LOCALAPPDATA', 'XDG_STATE_HOME', 'TMP', 'TEMP', 'TMPDIR'):
        environment[key] = str(profile)

    def am(*argv, code=0):
        result = subprocess.run([str(args.am.resolve()), *map(str, argv), '--json'], cwd=work,
                                env=environment, capture_output=True, encoding='utf8', timeout=60)
        if result.returncode != code:
            raise AssertionError(f'/cli/exit: expected {code}, got {result.returncode}: {result.stdout} {result.stderr}')
        return json.loads(result.stdout)

    am('library', 'init', library)
    before = am('library', 'info', library)
    state = library / '.am/state.sqlite'
    for suffix in ('', '-wal', '-shm'):
        Path(str(state) + suffix).unlink(missing_ok=True)
    orphan = library / '.am/.am-tmp' / (str(uuid.uuid4()) + '.zip')
    orphan.write_bytes(b'Unconfirmed partial ZIP')
    old = time.time() - 72 * 3600  # hypothesis: safely older than orphan retention
    os.utime(orphan, (old, old))
    preserved = {str(p.relative_to(library)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in library.rglob('*') if p.is_file() and p.name in ('userdata.sqlite', 'am-library.toml')}
    missing = am('library', 'info', library)
    assert missing['databases']['state']['state'] == 'missing', '/info/missing'
    assert not state.exists(), '/readonly/state: info recreated the database'
    if before['write_unavailable_reason'] == 'storage_read_only':
        refused = am('library', 'sync', library, code=4)
        assert refused['error']['message_id'] == 'am.library.storage_read_only', '/read_only/reason'
        assert not state.exists(), '/readonly/state: denied writer created the database'
        assert orphan.exists(), '/readonly/orphan'
        print('PASS state CLI: read-only inspection and denial; recreation NOT_APPLICABLE by filesystem policy')
        return 0
    result = am('library', 'sync', library)
    assert any(e['message_id'] == 'am.library.state_recreated' for e in result.get('warnings', [])), '/json/warnings'
    assert orphan.exists(), '/recreation/orphan: deleted in first opening'
    info = am('library', 'info', library)['databases']['state']
    assert info['state'] == 'ok' and info['reason'] == 'recreated', '/info/state'
    assert isinstance(info['recreated_at'], str) and info['recreated_at'].endswith('Z'), '/info/recreated_at'
    log = am('library', 'log', library)
    events = [row for row in log['entries'] if row['code'] == 'am.library.state_recreated']
    assert len(events) == 1 and events[0]['at'] == info['recreated_at'], '/log/recreated'
    am('library', 'sync', library)
    assert not orphan.exists(), '/retention: later opening did not apply orphan_age'
    again = am('library', 'info', library)['databases']['state']
    assert again['recreated_at'] == info['recreated_at'], '/info/history: recreation time changed'
    for suffix in ('', '-wal', '-shm'):
        Path(str(state) + suffix).unlink(missing_ok=True)
    repeat = am('library', 'sync', library)
    assert any(e['message_id'] == 'am.library.state_recreated' for e in repeat.get('warnings', [])), '/repeat/warnings'
    for name, sha in preserved.items():
        assert hashlib.sha256((library / name).read_bytes()).hexdigest() == sha, '/protected/' + name
    print('PASS state CLI: warning JSON, historical healthy info/time, operation log, first-opening orphan protection')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print('FAIL state CLI: ' + str(error))
        raise SystemExit(1)
