"""Run real disk-full tests on an owner-provided NTFS image or isolated tmpfs."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_fault_unit import check_indexes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tests', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--child', action='store_true')
    parser.add_argument('--mount', type=Path)
    args = parser.parse_args()
    work = args.work.resolve() / str(uuid.uuid4())
    if not work.is_relative_to(ROOT / '.cache'):
        raise ValueError('Work must remain in project cache')
    work.mkdir(parents=True)
    if args.child:
        if not args.mount or not args.mount.resolve().is_relative_to(ROOT / '.cache'):
            raise ValueError('Namespace mount must remain in project cache')
        args.mount.mkdir(parents=True, exist_ok=True)
        mounted = subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=32m', 'tmpfs', str(args.mount)], capture_output=True, text=True)
        if mounted.returncode:
            print('SKIP small_volume: isolated tmpfs mount unavailable: ' + mounted.stderr.strip())
            return 77
        os.environ['AM_SMALL_VOLUME'] = str(args.mount)
    elif not os.environ.get('AM_SMALL_VOLUME'):
        if os.name == 'nt':
            print('SKIP small_volume: AM_SMALL_VOLUME is unset; only the owner may attach the NTFS test image')
            return 77
        probe = subprocess.run(['unshare', '--user', '--map-root-user', '--mount', 'true'], capture_output=True, text=True)
        if probe.returncode:
            print('SKIP small_volume: unprivileged namespaces unavailable: ' + probe.stderr.strip())
            return 77
        command = ['unshare', '--user', '--map-root-user', '--mount', sys.executable, '-I', str(Path(__file__).resolve()),
                   '--child', '--mount', str(work / 'mnt'), '--tests', str(args.tests.resolve()),
                   '--helper', str(args.helper.resolve()), '--work', str(args.work.resolve())]
        return subprocess.run(command, cwd=ROOT).returncode
    volume = Path(os.path.abspath(os.environ['AM_SMALL_VOLUME']))
    try:
        if not volume.is_relative_to(ROOT / '.cache'):
            raise AssertionError('/volume: test mount must be named inside project cache')
        if not volume.is_dir():
            raise AssertionError('/volume: configured mount directory does not exist')
        if os.name != 'nt':
            actual = subprocess.run(['stat', '-f', '-c', '%T', str(volume)], capture_output=True, text=True, check=True)
            print('Actual OS filesystem: ' + actual.stdout.strip(), flush=True)
        listing = subprocess.run([str(args.tests.resolve()), '--gtest_list_tests'], capture_output=True, text=True, check=True)
        cases = []
        suite = ''
        for line in listing.stdout.splitlines():
            if line and not line.startswith(' ') and line.endswith('.'):
                suite = line.strip()
            elif suite == 'SmallVolume.' and line.startswith('  '):
                cases.append(suite + line.strip().split(' ')[0])
        if len(cases) < 5:
            raise AssertionError('/cases: real-volume minimum scenarios are missing')
        for index, name in enumerate(cases):
            xml = work / f'{index}.xml'
            result = subprocess.run([str(args.tests.resolve()), '--gtest_filter=' + name, '--gtest_output=xml:' + str(xml)], timeout=300)
            if result.returncode:
                return result.returncode
            check_indexes(xml, args.helper.resolve(), volume=volume, snapshot_area=work)
            properties = {p.get('name'): p.get('value') for p in ET.parse(xml).getroot().iter('property')}
            print('Classifier mode=' + properties.get('actual_storage_mode', '?') +
                  '; classifier filesystem=' + properties.get('actual_filesystem', '') +
                  '; capacity=' + properties.get('volume_capacity_bytes', '?'), flush=True)
            for prop in ET.parse(xml).getroot().iter('property'):
                if prop.get('name') != 'libraries':
                    continue
                for path in json.loads(prop.get('value')):
                    area = Path(path).absolute().parent
                    # Resolve immediately before recursive deletion; only this test's
                    # uniquely named directory inside the explicit volume is eligible.
                    if not area.name.startswith('am-t036-') or not area.resolve().is_relative_to(volume.resolve()):
                        raise AssertionError('/cleanup: root escaped the intended test volume')
                    shutil.rmtree(area)
            print('PASS ' + name + ': real OS full-volume error and recovered full index equality')
        print(f'PASS small_volume: {len(cases)} real-volume scenarios; no policy/classifier changes')
        return 0
    finally:
        if args.child:
            result = subprocess.run(['umount', str(args.mount)], capture_output=True, text=True)
            if result.returncode:
                print('FAIL small_volume cleanup: ' + result.stderr.strip())
                raise RuntimeError('Unable to detach isolated tmpfs')


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print('FAIL small_volume: ' + str(error))
        raise SystemExit(1)
