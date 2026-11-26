"""Negative runner checks: SC-1/4/13, ADR-0029 and ADR-0035."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_import_corpus import compare, assert_source, assert_temporary, edit_plan, run_case
from make_import_cases import fingerprint, generate, local_path
from expect_match import mismatch


class RunnerSelf(unittest.TestCase):
    def setUp(self):
        cache = ROOT / '.cache/import-corpus-self'
        cache.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=cache)
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)

    def test_wrong_expectation_has_json_pointer(self):
        with self.assertRaisesRegex(AssertionError, '/scan/candidates/0'):
            compare({'candidates': [{'action': 'ask'}]}, {'candidates': [{'action': 'import'}]}, '/scan')

    def test_changed_source_is_detected_even_with_same_size_and_mtime(self):
        source = self.work / 'source'
        source.mkdir()
        path = source / 'model.stl'
        path.write_bytes(b'original')
        before = fingerprint(source)
        stat = path.stat()
        path.write_bytes(b'modified')
        __import__('os').utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        with self.assertRaisesRegex(AssertionError, '/invariants/source'):
            assert_source(source, before)

    def test_extra_temporary_file_is_detected(self):
        folder = self.work / '.am/.am-tmp/import/attempt'
        folder.mkdir(parents=True)
        (folder / 'unpacked.zip').write_bytes(b'leftover')
        with self.assertRaisesRegex(AssertionError, '/invariants/temp'):
            assert_temporary(self.work)

    def test_subset_matching_is_bijective_and_order_independent(self):
        self.assertIsNone(mismatch([{'a': 1}, {'b': 2}], [{'a': 1, 'b': 2}, {'a': 1}]))
        self.assertIsNotNone(mismatch([{'b': 2}, {'b': 2}], [{'b': 2}, {'a': 1}]))
        self.assertIsNotNone(mismatch(True, 1))

    def test_generator_refuses_escape_before_write(self):
        for path in ('../escape', '/absolute', 'C:/drive', 'sub/../../escape', 'back\\slash'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                local_path(self.work, path)
        self.assertEqual(list(self.work.iterdir()), [])

    def test_generator_checks_destination_boundary(self):
        with self.assertRaisesRegex(ValueError, 'inside project'):
            generate({'source': []}, ROOT.parent / 'outside-test-must-not-exist')

    def test_unknown_selector_is_not_silently_ignored(self):
        plan = {'nodes': [], 'roots': [], 'candidates': []}
        with self.assertRaisesRegex(AssertionError, 'matched 0'):
            edit_plan(plan, [{'op': 'decision', 'path': 'missing', 'value': 'include'}], self.work)

    def test_case_selector_cannot_delete_a_parent_work_directory(self):
        sentinel = self.work / 'sentinel'
        sentinel.write_bytes(b'keep')
        with self.assertRaisesRegex(ValueError, '/case'):
            run_case(ROOT / 'unused', ROOT / 'tests/fixtures/import', self.work, '..')
        self.assertEqual(sentinel.read_bytes(), b'keep')


if __name__ == '__main__':
    unittest.main()
