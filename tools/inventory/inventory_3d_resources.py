#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Read-only inventory of a directory and contents of supported archives.

No extraction to disk. Standard library for ZIP/TAR. 7-Zip CLI (optional)
for RAR/7z. Nested ZIP/TAR are inspected in memory up to a size limit.

Usage:
  cd /d "D:\\some\\folder"             (Windows CMD)
  py "C:\\tools\\inventory_3d_resources.py"
  py inventory_3d_resources.py --root "D:\\models" --sha256-files
"""

import argparse
from collections import Counter, defaultdict
from datetime import datetime
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

TAR_SUFFIXES = ('.tar', '.tar.gz', '.tgz', '.tar.bz2', '.tbz2', '.tar.xz', '.txz')
ZIP_SUFFIXES = ('.zip', '.cbz', '.3mf', '.sl1', '.ufp')
SEVENZIP_SUFFIXES = ('.7z', '.rar')


def archive_type(name):
    n = name.casefold()
    if n.endswith(TAR_SUFFIXES):
        return 'tar'
    if n.endswith(ZIP_SUFFIXES):
        return 'zip'
    if n.endswith(SEVENZIP_SUFFIXES):
        return '7zip-cli'
    return None


def ext(name):
    n = name.casefold()
    for suffix in ('.tar.gz', '.tar.bz2', '.tar.xz', '.gcode.3mf'):
        if n.endswith(suffix):
            return suffix
    return Path(name).suffix.casefold() or '[без расширения]'


def suspect(path):
    p = path.replace('\\', '/')
    items = p.casefold().split('/')
    bad = []
    if '__macosx' in items or any(part.startswith('._') for part in items):
        bad.append('AppleDouble/__MACOSX')
    if items[-1] in ('.ds_store', 'thumbs.db', 'desktop.ini'):
        bad.append('служебный файл ОС')
    if p.startswith('/') or re.match(r'^[A-Za-z]:', p) or '..' in items:
        bad.append('потенциально опасный путь')
    return bad


def sha256_of_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def json_output(obj):
    return json.dumps(obj, ensure_ascii=False, sort_keys=True)


def safe_rel(path, root):
    return path.relative_to(root).as_posix()


def locate_7zip(requested):
    if requested:
        return str(Path(requested).resolve())
    for candidate in ('7zz', '7z', '7za'):
        found = shutil.which(candidate)
        if found:
            return found
    if os.name == 'nt':
        for base in (os.environ.get('ProgramFiles'), os.environ.get('ProgramFiles(x86)')):
            if base:
                c = Path(base) / '7-Zip' / '7z.exe'
                if c.is_file():
                    return str(c)
    return None


class Scanner:
    def __init__(self, args, root, outdir):
        self.args = args
        self.root = root
        self.outdir = outdir
        self.f_tree = open(outdir / 'tree.txt', 'w', encoding='utf-8', newline='\n')
        self.f_records = open(outdir / 'inventory.jsonl', 'w', encoding='utf-8', newline='\n')
        self.f_errors = open(outdir / 'errors.txt', 'w', encoding='utf-8', newline='\n')
        self.counter = Counter()
        self.extensions = Counter()
        self.inner_extensions = Counter()
        self.archive_types = Counter()
        self.suspicious_counts = Counter()
        self.suspicious_examples = []
        self.duplicate_examples = []
        self.equal_size_examples = []
        self.errors = []
        self.sevenzip = locate_7zip(args.sevenzip)
        self.nested_limit = int(args.nested_max_mb * 1024 * 1024)

    def close(self):
        for f in (self.f_tree, self.f_records, self.f_errors):
            f.close()

    def tree(self, indent, text):
        self.f_tree.write('  ' * indent + text + '\n')

    def record(self, **data):
        self.f_records.write(json_output(data) + '\n')

    def error(self, where, message):
        msg = f'{where}: {message}'
        self.counter['errors'] += 1
        self.f_errors.write(msg + '\n')
        if len(self.errors) < 50:
            self.errors.append(msg)

    def mark_suspicious(self, display_path, inner):
        found = suspect(inner)
        for reason in found:
            self.suspicious_counts[reason] += 1
            if len(self.suspicious_examples) < 70:
                self.suspicious_examples.append((reason, display_path))
        return found

    def archive(self, filename, path_id, indent, depth=0):
        kind = archive_type(path_id)
        if not kind:
            return
        self.counter['archives_detected'] += 1
        self.archive_types[ext(path_id)] += 1
        self.tree(indent, f'[АРХИВ {kind}] {PurePosixPath(path_id.split("::")[-1]).name}')
        self.record(kind='archive', virtual_path=path_id, archive_type=kind, depth=depth)
        if kind == '7zip-cli':
            if depth != 0 or not isinstance(filename, Path):
                self.skip_archive(path_id, indent + 1, 'вложенный RAR/7z: не читается из памяти')
                return
            if not self.sevenzip:
                self.skip_archive(path_id, indent + 1, 'не найден 7-Zip (7z.exe/7zz)')
                return
            self.scan_7zip(filename, path_id, indent + 1)
        elif kind == 'zip':
            try:
                with zipfile.ZipFile(filename, 'r') as zf:
                    files = []
                    sizes = []
                    for info in zf.infolist():
                        name = info.filename
                        item_id = path_id + '::' + name
                        is_dir = info.is_dir()
                        if not is_dir:
                            files.append(name)
                            sizes.append((name, info.file_size))
                        self.member(item_id, name, info.file_size, info.compress_size,
                                    is_dir, indent + 1, 'zip', info.CRC,
                                    modified='-'.join(str(n) for n in info.date_time[:3]))
                        if (not is_dir and archive_type(name)):
                            self.nested_zip(zf, info, item_id, indent + 2, depth)
                    self.duplicate_names(path_id, files)
                    self.equal_sizes(path_id, sizes)
            except (OSError, zipfile.BadZipFile, RuntimeError, ValueError, EOFError) as exc:
                self.error(path_id, f'ZIP: {exc}')
                self.tree(indent + 1, '[ОШИБКА ЧТЕНИЯ ZIP]')
        else:
            try:
                with tarfile.open(fileobj=filename if not isinstance(filename, Path) else None,
                                  name=str(filename) if isinstance(filename, Path) else None,
                                  mode='r:*') as tf:
                    files = []
                    sizes = []
                    for info in tf:
                        name = info.name
                        is_dir = info.isdir()
                        item_id = path_id + '::' + name
                        if info.isfile():
                            files.append(name)
                            sizes.append((name, info.size))
                        self.member(item_id, name, info.size if info.isfile() else 0,
                                    None, is_dir, indent + 1, 'tar', None,
                                    special_type=None if (is_dir or info.isfile()) else 'symlink/other')
                        if info.isfile() and archive_type(name):
                            self.nested_tar(tf, info, item_id, indent + 2, depth)
                    self.duplicate_names(path_id, files)
                    self.equal_sizes(path_id, sizes)
            except (OSError, tarfile.TarError, ValueError, EOFError) as exc:
                self.error(path_id, f'TAR: {exc}')
                self.tree(indent + 1, '[ОШИБКА ЧТЕНИЯ TAR]')

    def member(self, item_id, inner_name, size, packed, is_dir, indent, fmt,
               crc=None, modified=None, special_type=None):
        self.counter['archived_dirs' if is_dir else 'archived_files'] += 1
        if not is_dir:
            self.counter['archived_unpacked_bytes'] += size
            self.inner_extensions[ext(inner_name)] += 1
        flags = self.mark_suspicious(item_id, inner_name)
        extras = ', '.join(flags) if flags else ''
        mark = '[D]' if is_dir else '[F]'
        self.tree(indent, f'{mark} {inner_name} ({size} B)' + (f'  [{extras}]' if extras else ''))
        self.record(kind='archive_dir' if is_dir else 'archive_file',
                    virtual_path=item_id, archive_member=inner_name, format=fmt,
                    size=size, packed_size=packed, crc32=(f'{crc:08x}' if crc is not None else None),
                    modified=modified, special_type=special_type, flags=flags)

    def skip_archive(self, path_id, indent, reason):
        self.counter['archives_skipped'] += 1
        self.tree(indent, f'[СОДЕРЖИМОЕ НЕ ПРОЧИТАНО] {reason}')
        self.record(kind='archive_skipped', virtual_path=path_id, reason=reason)

    def nested_zip(self, zf, info, path_id, indent, depth):
        if depth >= self.args.max_nested_depth:
            self.skip_archive(path_id, indent, 'достигнут предел вложенности')
            return
        if info.file_size > self.nested_limit:
            self.skip_archive(path_id, indent, f'размер > {self.args.nested_max_mb:g} МиБ')
            return
        try:
            with zf.open(info, 'r') as stream:
                raw = stream.read(self.nested_limit + 1)
            if len(raw) > self.nested_limit:
                self.skip_archive(path_id, indent, 'превышен лимит чтения')
                return
            self.archive(io.BytesIO(raw), path_id, indent, depth + 1)
        except (OSError, RuntimeError, ValueError, EOFError, zipfile.BadZipFile) as exc:
            self.error(path_id, f'вложенный архив: {exc}')

    def nested_tar(self, tf, info, path_id, indent, depth):
        if depth >= self.args.max_nested_depth:
            self.skip_archive(path_id, indent, 'достигнут предел вложенности')
            return
        if info.size > self.nested_limit:
            self.skip_archive(path_id, indent, f'размер > {self.args.nested_max_mb:g} МиБ')
            return
        try:
            with tf.extractfile(info) as stream:
                raw = stream.read(self.nested_limit + 1)
            if len(raw) > self.nested_limit:
                self.skip_archive(path_id, indent, 'превышен лимит чтения')
                return
            self.archive(io.BytesIO(raw), path_id, indent, depth + 1)
        except (OSError, tarfile.TarError, EOFError, ValueError) as exc:
            self.error(path_id, f'вложенный архив: {exc}')

    def duplicate_names(self, container, file_names):
        groups = defaultdict(list)
        for name in file_names:
            groups[PurePosixPath(name.replace('\\', '/')).name.casefold()].append(name)
        for group in groups.values():
            if len(group) >= 2:
                self.counter['repeated_basename_groups'] += 1
                if len(self.duplicate_examples) < 50:
                    self.duplicate_examples.append({'archive': container, 'names': group[:20]})

    def equal_sizes(self, container, sized_names):
        # Signal only: multiple same-sized files in the same inner directory.
        groups = defaultdict(list)
        for name, size in sized_names:
            parent = str(PurePosixPath(name.replace('\\', '/')).parent).casefold()
            groups[(parent, size)].append(name)
        for (parent, size), names in groups.items():
            if len(names) >= 3:
                self.counter['equal_size_groups'] += 1
                if len(self.equal_size_examples) < 50:
                    self.equal_size_examples.append({'archive': container, 'folder': parent,
                                                     'size': size, 'names': names[:20]})

    def scan_7zip(self, filename, path_id, indent):
        try:
            p = subprocess.run([self.sevenzip, 'l', '-slt', '-sccUTF-8', str(filename)],
                               capture_output=True, text=True, encoding='utf-8', errors='replace',
                               timeout=self.args.archive_timeout)
            if p.returncode != 0:
                self.error(path_id, f'7-Zip exit={p.returncode}: {(p.stderr or p.stdout)[-350:]}')
                return
            inside = False
            current = {}
            names = []
            sizes = []
            def commit():
                if 'Path' not in current:
                    return
                name = current['Path']
                is_dir = current.get('Folder') == '+' or name.endswith('/') or \
                    current.get('Attributes', '').startswith('D')
                size = int(current.get('Size', '0') or 0)
                packed = current.get('Packed Size')
                crc = current.get('CRC')
                crc_num = int(crc, 16) if crc and re.fullmatch(r'[0-9a-fA-F]{8}', crc) else None
                self.member(path_id + '::' + name, name, size,
                            int(packed) if packed and packed.isdigit() else None,
                            is_dir, indent, '7zip-cli', crc_num,
                            modified=current.get('Modified'))
                if not is_dir:
                    names.append(name)
                    sizes.append((name, size))
                    if archive_type(name):
                        self.skip_archive(path_id + '::' + name, indent + 1,
                                          'вложенный архив в RAR/7z: не извлекается')
            for line in p.stdout.splitlines():
                if not inside:
                    if line.strip().startswith('----------'):
                        inside = True
                    continue
                if not line.strip():
                    commit()
                    current = {}
                elif ' = ' in line:
                    k, v = line.split(' = ', 1)
                    current[k] = v
            commit()
            self.duplicate_names(path_id, names)
            self.equal_sizes(path_id, sizes)
        except (OSError, subprocess.TimeoutExpired, ValueError) as exc:
            self.error(path_id, f'7-Zip: {exc}')

    def scan(self):
        self.tree(0, f'[КОРЕНЬ] {self.root}')
        script_path = Path(__file__).resolve()
        for dirpath, dirnames, filenames in os.walk(self.root, followlinks=False,
                                                     onerror=lambda e: self.error(str(e.filename), str(e))):
            dp = Path(dirpath)
            rel_dir = safe_rel(dp, self.root)
            depth = 0 if rel_dir == '.' else len(dp.relative_to(self.root).parts)
            if rel_dir != '.':
                self.counter['physical_dirs'] += 1
                self.tree(depth, f'[ПАПКА] {dp.name}/')
                self.record(kind='directory', path=rel_dir)
            dirnames.sort(key=str.casefold)
            filenames.sort(key=str.casefold)
            # Symlinked directories are not traversed by os.walk(followlinks=False).
            for d in list(dirnames):
                p = dp / d
                if p.is_symlink():
                    dirnames.remove(d)
                    self.counter['symlinks'] += 1
                    self.tree(depth + 1, f'[ССЫЛКА НА ПАПКУ] {d} (без обхода)')
                    self.record(kind='symlink_dir', path=safe_rel(p, self.root))
            for file_name in filenames:
                fp = dp / file_name
                if fp == script_path:
                    continue
                rel = safe_rel(fp, self.root)
                if fp.is_symlink():
                    self.counter['symlinks'] += 1
                    self.tree(depth + 1, f'[ССЫЛКА] {file_name} (без чтения)')
                    self.record(kind='symlink_file', path=rel)
                    continue
                try:
                    st = fp.stat()
                    size = st.st_size
                    digest = sha256_of_file(fp) if self.args.sha256_files else None
                except OSError as exc:
                    self.error(rel, f'stat/hash: {exc}')
                    continue
                self.counter['physical_files'] += 1
                self.counter['physical_total_bytes'] += size
                self.extensions[ext(file_name)] += 1
                flags = self.mark_suspicious(rel, rel)
                suffix = ' [' + ', '.join(flags) + ']' if flags else ''
                self.tree(depth + 1, f'[ФАЙЛ] {file_name} ({size} B){suffix}')
                self.record(kind='file', path=rel, size=size, mtime=st.st_mtime,
                            sha256=digest, flags=flags)
                if archive_type(file_name):
                    self.archive(fp, rel, depth + 2)

    def write_report(self):
        summary = {
            'root': str(self.root), 'output': str(self.outdir),
            'created': datetime.now().astimezone().isoformat(),
            'sevenzip_found': self.sevenzip,
            'parameters': vars(self.args),
            'counts': dict(self.counter),
            'physical_extensions': dict(self.extensions.most_common()),
            'inside_archive_extensions': dict(self.inner_extensions.most_common()),
            'archive_extensions': dict(self.archive_types.most_common()),
            'suspicious_counts': dict(self.suspicious_counts),
            'suspicious_examples': self.suspicious_examples,
            'repeated_basename_examples': self.duplicate_examples,
            'equal_size_examples': self.equal_size_examples,
            'error_examples': self.errors,
        }
        (self.outdir / 'summary.json').write_text(
            json.dumps(summary, ensure_ascii=False, indent=2, default=str) + '\n', encoding='utf-8')
        with open(self.outdir / 'report.md', 'w', encoding='utf-8', newline='\n') as f:
            f.write('# Отчёт об инвентаризации 3D/2D-ресурсов\n\n')
            f.write(f'Корневая директория: `{self.root}`.\n\n')
            f.write('## Статистика\n\n')
            for k, v in sorted(self.counter.items()):
                f.write(f'- {k}: {v}\n')
            f.write('\n## Типы физических файлов (топ-60)\n\n')
            for k, v in self.extensions.most_common(60):
                f.write(f'- `{k}`: {v}\n')
            f.write('\n## Типы файлов внутри архивов (топ-60)\n\n')
            for k, v in self.inner_extensions.most_common(60):
                f.write(f'- `{k}`: {v}\n')
            f.write('\n## Типы архивов\n\n')
            for k, v in self.archive_types.most_common():
                f.write(f'- `{k}`: {v}\n')
            f.write('\n## Подозрительные служебные файлы и пути\n\n')
            for reason, count in self.suspicious_counts.items():
                f.write(f'- {reason}: {count}\n')
            for reason, loc in self.suspicious_examples[:30]:
                f.write(f'- `{loc}` — {reason}\n')
            f.write('\n## Повторяющиеся имена файлов внутри архивов (не обязательно дубликаты)\n\n')
            for group in self.duplicate_examples[:30]:
                f.write(f'- `{group["archive"]}`: {json.dumps(group["names"], ensure_ascii=False)}\n')
            f.write('\n## Группы файлов одинакового размера внутри одной папки архива\n\n')
            for group in self.equal_size_examples[:30]:
                f.write(f'- `{group["archive"]}` / `{group["folder"]}`, размер {group["size"]} байт: {json.dumps(group["names"], ensure_ascii=False)}\n')
            f.write('\n## Ошибки (первые 30)\n\n')
            for err in self.errors[:30]:
                f.write(f'- {err}\n')
            f.write('\n## Ограничения\n\n')
            f.write('- Скрипт только читает входную коллекцию; никакие архивы на диск не распаковываются.\n')
            f.write('- RAR/7z перечисляются через установленный 7-Zip; без него отмечаются как не просмотренные.\n')
            f.write('- Вложенные ZIP/TAR читаются только в памяти и в установленных пределах глубины/размера.\n')
            f.write('- Одинаковые имена файлов и служебные файлы только помечаются; ничего не удаляется.\n')
            f.write('- CRC32 внутри ZIP — не криптографический хеш; SHA-256 физических файлов опционален.\n')
            f.write('- Ссылки на Telegram/веб-страницы из файловой структуры не определяются автоматически.\n')


def main():
    ap = argparse.ArgumentParser(description='Read-only map of folders and archive contents')
    ap.add_argument('--root', default='.', help='Исследуемая директория (по умолчанию текущая)')
    ap.add_argument('--out', default=None, help='Каталог для отчёта (только ВНЕ исследуемой директории)')
    ap.add_argument('--sha256-files', action='store_true', help='Посчитать SHA-256 физических файлов (медленнее)')
    ap.add_argument('--max-nested-depth', type=int, default=2,
                    help='Глубина вложенных архивов после внешнего архива (по умолчанию 2)')
    ap.add_argument('--nested-max-mb', type=float, default=64,
                    help='Максимальный размер вложенного архива в памяти, МиБ (по умолчанию 64)')
    ap.add_argument('--sevenzip', default=None, help='Путь к 7z.exe / 7zz, если не найден автоматически')
    ap.add_argument('--archive-timeout', type=int, default=180, help='Таймаут 7-Zip (сек)')
    args = ap.parse_args()
    if args.max_nested_depth < 0 or args.nested_max_mb <= 0:
        ap.error('Глубина должна быть >= 0, размер > 0')
    root = Path(args.root).resolve()
    if not root.is_dir():
        ap.error(f'Нет директории: {root}')
    if args.out:
        outdir = Path(args.out).resolve()
    else:
        stamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        outdir = root.parent / f'{root.name}_inventory_{stamp}'
    if outdir == root or root in outdir.parents:
        ap.error('Выходная папка должна быть ВНЕ исследуемого каталога (режим только чтения)')
    if outdir.exists() and any(outdir.iterdir()):
        ap.error(f'Выходная папка не пуста: {outdir}')
    outdir.mkdir(parents=True, exist_ok=True)
    scanner = Scanner(args, root, outdir)
    try:
        scanner.scan()
        scanner.write_report()
    finally:
        scanner.close()
    bundle = outdir.with_suffix('.zip')
    if bundle.exists():
        print(f'Пакет отчёта уже существует, не перезаписываю: {bundle}')
    else:
        with zipfile.ZipFile(bundle, 'w', compression=zipfile.ZIP_DEFLATED) as result:
            for report in sorted(outdir.iterdir()):
                if report.is_file():
                    result.write(report, arcname=report.name)
    print('Готово. Входная директория не изменялась.')
    print('Отчёты:', outdir)
    print('Единый архив отчёта:', bundle)
    print('Физических файлов:', scanner.counter['physical_files'])
    print('Архивов обнаружено:', scanner.counter['archives_detected'])
    print('Файлов внутри архивов:', scanner.counter['archived_files'])
    print('Архивов пропущено:', scanner.counter['archives_skipped'])
    print('Ошибок:', scanner.counter['errors'])
    print('7-Zip:', scanner.sevenzip or 'не найден')


if __name__ == '__main__':
    main()
