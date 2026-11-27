"""K-117/K-122: finish every case and clean only owned test-volume directories."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
from unittest import mock
import uuid
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_small_volume as driver


class SmallVolumeDriver(unittest.TestCase):
    def setUp(self):
        self.area = driver.ROOT / '.cache/small-volume-driver-tests' / str(uuid.uuid4())
        self.volume, self.work = self.area / 'volume', self.area / 'work'
        self.volume.mkdir(parents=True)
        self.work.mkdir()
        self.sentinel = self.volume / 'owner-data.txt'
        self.sentinel.write_text('Owner data', encoding='utf8')
        self.visited, self.inspected = [], []

    def tearDown(self):
        target = self.area.resolve()
        self.assertTrue(target.is_relative_to((driver.ROOT / '.cache').resolve()))
        shutil.rmtree(target)

    def execute(self, behaviors, oracle_failure=None):
        cases = ['SmallVolume.Case' + str(i) for i in range(len(behaviors))]

        def child(command, *, env, timeout):
            name = command[1].split('=', 1)[1]
            index = cases.index(name)
            self.visited.append(name)
            case = Path(env['AM_SMALL_VOLUME'])
            root = case / 'am-t036-fixture/library'
            root.mkdir(parents=True)
            (case / 'filler.bin').write_bytes(b'partial test data')
            if behaviors[index] == 'timeout':
                raise subprocess.TimeoutExpired(command, timeout)
            document = ET.Element('testsuites')
            test = ET.SubElement(document, 'testcase', classname='SmallVolume', name=name.split('.')[1])
            props = ET.SubElement(test, 'properties')
            values = {'libraries': json.dumps([str(root)])}
            if behaviors[index] == 'not_applicable':
                values.update(r_applicability='not_applicable', r_reason='ntfs_folder_mount_local_only')
            for key, value in values.items():
                ET.SubElement(props, 'property', name=key, value=value)
            if behaviors[index] == 'failed':
                ET.SubElement(test, 'failure', message='Injected expectation failure')
            ET.ElementTree(document).write(command[2].split('xml:', 1)[1])
            return subprocess.CompletedProcess(command, 1 if behaviors[index] == 'failed' else 0)

        def inspect(xml, *_args, **_kwargs):
            index = int(xml.stem)
            self.inspected.append(index)
            if index == oracle_failure:
                raise AssertionError('/index: injected mismatch')

        output = io.StringIO()
        with mock.patch.object(driver.subprocess, 'run', side_effect=child), \
                mock.patch.object(driver, 'check_indexes', side_effect=inspect), redirect_stdout(output):
            code = driver.run_cases(cases, Path('test-exe'), Path('helper-exe'), self.work, self.volume)
        self.assertEqual(self.visited, cases)
        self.assertEqual(self.sentinel.read_text(encoding='utf8'), 'Owner data')
        summary = json.loads((self.work / 'summary.json').read_text(encoding='utf8'))
        self.assertIn('SUMMARY small_volume:', output.getvalue())
        return code, summary

    def test_unit_and_oracle_failures_do_not_stop_remaining_cases(self):
        code, summary = self.execute(['failed', 'passed', 'passed'], oracle_failure=1)
        self.assertEqual(code, 1)
        self.assertEqual([r['status'] for r in summary['results']], ['failed', 'failed', 'passed'])
        self.assertEqual(self.inspected, [0, 1, 2])
        self.assertFalse(list(self.volume.glob('am-t036-case-*')))

    def test_timeout_without_xml_is_cleaned_and_next_case_runs(self):
        code, summary = self.execute(['timeout', 'not_applicable'])
        self.assertEqual(code, 1)
        self.assertEqual(summary['failed'], 1)
        self.assertEqual(summary['not_applicable'], 1)
        self.assertFalse(list(self.volume.glob('am-t036-case-*')))

    def test_applicable_pass_and_explicit_nonapplicability_succeed(self):
        code, summary = self.execute(['passed', 'not_applicable'])
        self.assertEqual(code, 0)
        self.assertEqual((summary['passed'], summary['not_applicable'], summary['failed']), (1, 1, 0))
        self.assertTrue(all(r['cleanup'] == 'ok' for r in summary['results']))
        self.assertFalse(list(self.volume.glob('am-t036-case-*')))

    def test_cleanup_failure_is_reported_but_does_not_stop_next_case(self):
        original = shutil.rmtree
        calls = []

        def remove(path):
            calls.append(path)
            if len(calls) == 1:
                raise OSError('Injected removal failure')
            original(path)

        with mock.patch.object(driver.shutil, 'rmtree', side_effect=remove):
            code, summary = self.execute(['passed', 'passed'])
        self.assertEqual(code, 1)
        self.assertEqual([r['status'] for r in summary['results']], ['failed', 'passed'])
        self.assertEqual(summary['results'][0]['cleanup'], 'failed')
        self.assertEqual(len(list(self.volume.glob('am-t036-case-*'))), 1)

    def test_cleanup_refuses_a_directory_outside_the_named_volume(self):
        foreign = self.area / 'am-t036-case-foreign'
        foreign.mkdir()
        with self.assertRaises(AssertionError):
            driver.cleanup_case(foreign, self.volume, foreign.resolve())
        self.assertTrue(foreign.exists())

    def test_creation_collision_does_not_delete_an_existing_directory(self):
        foreign = self.volume / 'am-t036-case-existing'
        foreign.mkdir()
        marker = foreign / 'confirmed.txt'
        marker.write_text('Confirmed data', encoding='utf8')
        with mock.patch.object(driver.uuid, 'uuid4', return_value='existing'), \
                mock.patch.object(driver.subprocess, 'run') as child, redirect_stdout(io.StringIO()):
            code = driver.run_cases(['SmallVolume.Collision'], Path('test-exe'), Path('helper-exe'), self.work, self.volume)
        self.assertEqual(code, 1)
        child.assert_not_called()
        self.assertEqual(marker.read_text(encoding='utf8'), 'Confirmed data')
        summary = json.loads((self.work / 'summary.json').read_text(encoding='utf8'))
        self.assertEqual(summary['results'][0]['cleanup'], 'not_created')


if __name__ == '__main__':
    unittest.main()
