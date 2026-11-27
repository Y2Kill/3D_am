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


def cleanup_case(case, volume, expected):
    if not case.exists():
        return
    resolved = case.resolve()
    # Recheck the absolute target immediately before deletion, including on Windows
    # where a folder-mounted volume resolves to its volume GUID rather than D:.
    if (not case.name.startswith('am-t036-case-') or resolved != expected or
            resolved == volume.resolve() or not resolved.is_relative_to(volume.resolve())):
        raise AssertionError('/cleanup: case escaped its owned directory in the test volume')
    shutil.rmtree(resolved)


def run_cases(cases, tests, helper, work, volume):
    results = []
    for index, name in enumerate(cases):
        case = volume / ('am-t036-case-' + str(uuid.uuid4()))
        expected = volume.resolve() / case.name
        xml = work / f'{index}.xml'
        row = {'name': name, 'status': 'failed', 'return_code': None, 'errors': []}
        created = False
        try:
            case.mkdir()
            created = True
            if case.resolve() != expected:
                raise AssertionError('/case: test directory was redirected')
            environment = os.environ.copy()
            environment['AM_SMALL_VOLUME'] = str(case)
            # A known per-case parent permits cleanup even if construction fails
            # before GTest writes libraries[], or the child times out without XML.
            result = subprocess.run([str(tests), '--gtest_filter=' + name, '--gtest_output=xml:' + str(xml)],
                                    env=environment, timeout=300)
            row['return_code'] = result.returncode
            if result.returncode:
                row['errors'].append('GTest exit code ' + str(result.returncode))
            document = ET.parse(xml).getroot()
            testcases = list(document.iter('testcase'))
            if len(testcases) != 1 or testcases[0].get('classname') + '.' + testcases[0].get('name') != name:
                raise AssertionError('/case: expected exactly the requested GTest')
            if list(document.iter('failure')) or list(document.iter('error')) or list(document.iter('skipped')):
                row['errors'].append('GTest reported failure or an unexpected skip')
            properties = {p.get('name'): p.get('value') for p in document.iter('property')}
            row['properties'] = properties
            if result.returncode == 0 and not json.loads(properties.get('libraries', '[]')):
                raise AssertionError('/case: successful test omitted its library integrity properties')
            # Also inspect libraries from a failed test; a failed expectation must
            # not hide a separate recovery/index problem.
            check_indexes(xml, helper, volume=volume, snapshot_area=work)
            print('Classifier mode=' + properties.get('actual_storage_mode', '?') +
                  '; classifier filesystem=' + properties.get('actual_filesystem', '') +
                  '; capacity=' + properties.get('volume_capacity_bytes', '?'), flush=True)
            if not row['errors']:
                if properties.get('r_applicability') == 'not_applicable':
                    row['status'] = 'not_applicable'
                    row['reason'] = properties['r_reason']
                else:
                    row['status'] = 'passed'
        except (AssertionError, OSError, ValueError, TypeError, KeyError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
            row['errors'].append(str(error))
        finally:
            try:
                if created:
                    cleanup_case(case, volume, expected)
                row['cleanup'] = 'ok' if created else 'not_created'
            except (AssertionError, OSError) as error:
                row['errors'].append('Cleanup failed: ' + str(error))
                row['cleanup'] = 'failed'
            if row['errors']:
                row['status'] = 'failed'
        results.append(row)
        status = {'passed': 'PASS', 'not_applicable': 'NOT_APPLICABLE', 'failed': 'FAIL'}[row['status']]
        details = '; '.join(row['errors']) or row.get('reason', '')
        outcome = row.get('properties', {}).get('init_configuration_outcome')
        if outcome:
            details += '; init_configuration_outcome=' + outcome
        print(status + ' ' + name + (': ' + details if details else ': integrity and recovered index checked'), flush=True)
    summary = {status: sum(row['status'] == status for row in results)
               for status in ('passed', 'not_applicable', 'failed')}
    summary.update(total=len(results), results=results)
    (work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf8')
    print('SUMMARY small_volume: total={total}; passed={passed}; not_applicable={not_applicable}; failed={failed}'.format(**summary), flush=True)
    return 1 if summary['failed'] else 0


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
        return run_cases(cases, args.tests.resolve(), args.helper.resolve(), work, volume)
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
