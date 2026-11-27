"""Run an injected-fault unit test, then independently compare its recovered index."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path
import subprocess
import sys
import uuid
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/e2e'))
from run_e2e import index_snapshot
sys.path.insert(0, str(ROOT / 'tests/tools'))
from expect_match import mismatch


def check_indexes(xml, helper, volume=None, snapshot_area=None):
    for property in ET.parse(xml).getroot().iter('property'):
        if property.get('name') != 'libraries':
            continue
        for name in json.loads(property.get('value')):
            root = Path(name).absolute()
            canonical = root.resolve()
            if not canonical.is_relative_to(ROOT / '.cache') and not (volume and canonical.is_relative_to(volume.resolve())):
                raise AssertionError('/library: injected test root escaped project cache')
            def command(verb):
                run = subprocess.run([str(helper), verb, str(root)], capture_output=True, encoding='utf8', timeout=60)
                if run.returncode:
                    raise AssertionError('/' + verb + ': ' + run.stdout + run.stderr)
            def protected():
                paths = [root / '.am/userdata.sqlite']
                paths += list((root / 'assets').rglob('*.zip')) + list((root / 'trash').rglob('*.zip'))
                return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
            confirmed = protected()
            def snapshot(tag):
                source = root / '.am/index.sqlite'
                if snapshot_area:
                    # All helper processes have closed. A closed main DB can be
                    # copied read-only, avoiding Windows volume-GUID URI parsing.
                    wal = Path(str(source) + '-wal')
                    if wal.exists() and wal.stat().st_size:
                        raise AssertionError('/snapshot: closed database still has a nonempty WAL')
                    target = snapshot_area / (tag + '-' + str(uuid.uuid4()) + '.sqlite')
                    shutil.copyfile(source, target)
                    return index_snapshot(target, 'rebuild')
                return index_snapshot(source, 'rebuild')
            command('recover')
            before = snapshot('sync')
            command('rebuild')
            error = mismatch(before, snapshot('rebuild'), '/index', exact=True, json_pointer=True)
            if error:
                raise AssertionError(error)
            error = mismatch(confirmed, protected(), '/confirmed_zip_or_userdata', exact=True, json_pointer=True)
            if error:
                raise AssertionError(error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tests', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    work = args.work.resolve() / str(uuid.uuid4())
    if not work.is_relative_to(ROOT / '.cache'):
        raise ValueError('Work must remain in project cache')
    work.mkdir(parents=True)
    xml = work / 'unit.xml'
    run = subprocess.run([str(args.tests.resolve()), '--gtest_filter=' + args.name, '--gtest_output=xml:' + str(xml)], timeout=300)
    if run.returncode:
        return run.returncode
    check_indexes(xml, args.helper.resolve())
    print('PASS ' + args.name + ': recovered full logical index/FTS equals rebuild, including ZIP mtime')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print('FAIL fault unit oracle: ' + str(error))
        raise SystemExit(1)
