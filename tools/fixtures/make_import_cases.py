"""Generate declarative import inputs; never read the owner's collection."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from zipgen import Utf8Info, fixture_info, pack_zip, patch_archive

ROOT = Path(__file__).resolve().parents[2]
FIXED_NS = 1577836800000000000


def local_path(root, name):
    # Source tree names are portable relative paths; archive names are raw data.
    if not name or name.startswith(('/', '\\')) or '\\' in name or ':' in name:
        raise ValueError(f"unsafe fixture path: {name!r}")
    if any(part in ('', '.', '..') for part in name.split('/')):
        raise ValueError(f"unsafe fixture component: {name!r}")
    path = root / name
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"fixture escapes destination: {name!r}")
    return path


class RawInfo(Utf8Info):
    def _encodeFilenameFlags(self):
        return self.raw_name, self.raw_flags


def payload(item):
    if 'zip' in item:
        return zip_bytes(item['zip'])
    if 'hex' in item:
        return bytes.fromhex(item['hex'])
    if 'pattern' in item:
        pattern = item['pattern'].encode('utf8')
        if not pattern:
            raise ValueError('empty byte pattern')
        return (pattern * ((item['size'] + len(pattern) - 1) // len(pattern)))[:item['size']]
    return item.get('content', '').encode('utf8')


def zip_bytes(config):
    def input_info(entry):
        info = fixture_info(entry, RawInfo)
        info.raw_name = bytes.fromhex(entry['raw_name_hex']) if 'raw_name_hex' in entry else entry['name'].encode('utf8')
        info.raw_flags = entry.get('flags', 0x800)
        info.external_attr = (0o40755 if entry['name'].endswith('/') else 0o100644) << 16
        return info
    data = pack_zip(config['entries'], payload, info_factory=input_info)
    return patch_archive(data, config.get('patches', []))


def fingerprint(root):
    """Include directories and links; never follow a source symlink."""
    records = {}
    def visit(directory):
        for path in sorted(directory.iterdir()):
            stat = path.lstat()
            name = path.relative_to(root).as_posix()
            if path.is_symlink():
                records[name] = ('link', os.readlink(path), stat.st_mtime_ns)
            elif path.is_dir():
                records[name] = ('directory', stat.st_mtime_ns)
                visit(path)
            else:
                records[name] = ('file', stat.st_size, stat.st_mtime_ns, hashlib.sha256(path.read_bytes()).hexdigest())
    visit(root)
    return records


def check_requirements(config, work):
    for condition in config.get('requires', []):
        if condition in ('windows', 'linux'):
            if (condition == 'windows') != (os.name == 'nt'):
                return f'requires {condition}'
        elif condition == 'symlink':
            probe = work / 'symlink-probe'
            try:
                probe.symlink_to('nonexistent')
            except OSError as error:
                return f'requires symlink privilege: {error}'
            finally:
                probe.unlink(missing_ok=True)
        else:
            raise ValueError(f'unknown requirement: {condition}')
    return None


def generate(config, destination):
    destination = destination.resolve()
    if not (destination.is_relative_to(ROOT / 'build') or destination.is_relative_to(ROOT / '.cache')):
        raise ValueError('fixture output must be inside project build/ or .cache/')
    destination.mkdir(parents=True, exist_ok=True)
    if any(destination.iterdir()):
        raise ValueError(f'fixture output is not empty: {destination}')
    for item in config['source']:
        path = local_path(destination, item['path'])
        path.parent.mkdir(parents=True, exist_ok=True)
        if item['kind'] == 'directory':
            path.mkdir(exist_ok=True)
        elif item['kind'] == 'symlink':
            target = local_path(destination, item['target'])
            path.symlink_to(os.path.relpath(target, path.parent), target_is_directory=item.get('directory', False))
        elif item['kind'] == 'golden':
            golden = ROOT / 'tests/fixtures/golden' / item['golden'] / 'asset.zip'
            if not golden.resolve().is_relative_to(ROOT / 'tests/fixtures/golden'):
                raise ValueError('golden source escapes corpus')
            path.write_bytes(golden.read_bytes())
        else:
            path.write_bytes(payload(item))
    # Fix directories last: creating children changes their parents' mtimes.
    paths = sorted(destination.rglob('*'), key=lambda p: len(p.parts), reverse=True)
    for path in paths:
        if path.is_symlink() and os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
            kernel.CreateFileW.restype = wintypes.HANDLE
            kernel.SetFileTime.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            handle = kernel.CreateFileW(str(path), 0x100, 7, None, 3, 0x02200000, None)
            if handle == wintypes.HANDLE(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                ticks = ctypes.c_uint64(FIXED_NS // 100 + 116444736000000000)
                if not kernel.SetFileTime(handle, None, None, ctypes.byref(ticks)):
                    raise ctypes.WinError(ctypes.get_last_error())
            finally:
                kernel.CloseHandle(handle)
        elif path.is_symlink():
            os.utime(path, ns=(FIXED_NS, FIXED_NS), follow_symlinks=False)
        else:
            os.utime(path, ns=(FIXED_NS, FIXED_NS))
    os.utime(destination, ns=(FIXED_NS, FIXED_NS))
    return fingerprint(destination)


def regen_check(cases, work):
    work = work.resolve()
    if not (work.is_relative_to(ROOT / 'build') or work.is_relative_to(ROOT / '.cache')):
        raise ValueError('regen work must be inside project build/ or .cache/')
    work.mkdir(parents=True, exist_ok=True)
    count = skipped = 0
    case_files = sorted(cases.glob('*/case.json'))
    if not case_files:
        raise ValueError('no import cases selected')
    for case in case_files:
        config = json.loads(case.read_text('utf8'))
        with tempfile.TemporaryDirectory(prefix='regen-', dir=work) as temporary:
            root = Path(temporary)
            reason = check_requirements(config, root)
            if reason:
                print(f'SKIP {config["case"]}: {reason}')
                skipped += 1
                continue
            # A sentinel sibling detects accidental writes outside the destination.
            sentinel = root / 'sentinel'
            sentinel.write_bytes(b'outside destination')
            before = fingerprint(root)
            first = generate(config, root / 'first')
            second = generate(config, root / 'second')
            if first != second:
                raise AssertionError(f'{config["case"]}: nondeterministic generated trees')
            outside = {k: v for k, v in fingerprint(root).items() if k.split('/')[0] not in ('first', 'second')}
            if outside != before:
                raise AssertionError(f'{config["case"]}: generator wrote outside destination')
            count += 1
    print(f'import_corpus_regen_check: {count} byte/mtime-identical cases, {skipped} explicit skips')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cases', type=Path, default=ROOT / 'tests/fixtures/import')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--case')
    args = parser.parse_args()
    if args.check:
        regen_check(args.cases, args.work)
    else:
        if not args.case:
            parser.error('--case is required without --check')
        output_case = local_path(args.work, args.case)
        if (args.cases / args.case).resolve().parent != args.cases.resolve():
            parser.error('--case must be a direct case directory')
        config = json.loads((args.cases / args.case / 'case.json').read_text('utf8'))
        generate(config, output_case / 'source')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
