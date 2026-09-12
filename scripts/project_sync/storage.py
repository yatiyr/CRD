"""Contained paths, exclusive writers, atomic files and recoverable transactions.

The journal is a write-ahead log, not a claim of multi-file filesystem atomicity.
Every destructive write has an expected old digest and a durable recovery blob.
"""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import time
import uuid


class Conflict(RuntimeError):
    """A rejected operation which requires an explicit correction or reconciliation."""


def digest(data: bytes | None) -> str | None:
    return hashlib.sha256(data).hexdigest() if data is not None else None


def json_bytes(value) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True) + '\n').encode('utf-8')


def read_json(path: Path, default=None):
    if not path.exists():
        return default
    if path.stat().st_size > 32 * 1024 * 1024:
        raise Conflict(f'Metadata exceeds 32 MiB: {path}')
    try:
        return json.loads(path.read_text(encoding='utf-8-sig'))
    except (ValueError, UnicodeError) as error:
        raise Conflict(f'Invalid JSON in {path}: {error}') from error


def atomic_write(path: Path, data: bytes, mode=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    if mode is None and path.is_file():
        mode = stat.S_IMODE(path.stat().st_mode)
    temp = path.with_name(f'.{path.name}.sync-{uuid.uuid4().hex}')
    try:
        with temp.open('xb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if mode is not None:
            temp.chmod(mode)
        os.replace(temp, path)
        if os.name != 'nt':
            descriptor = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
    finally:
        if temp.exists():
            temp.unlink()


def portable_name(name: str) -> str:
    """One canonical relative spelling on Windows, Linux and case-sensitive Macs."""
    if not isinstance(name, str):
        raise Conflict('A project path must be a string')
    name = name.replace('\\', '/')
    if not name or name.startswith('/') or '\x00' in name:
        raise Conflict(f'Expected a nonempty relative path: {name!r}')
    parts = name.split('/')
    for part in parts:
        if (not part or part in {'.', '..'} or part[-1:] in {' ', '.'}
                or re.search(r'[<>:"|?*;$`\x00-\x1f\x7f]', part)
                or re.fullmatch(r'(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?', part)):
            raise Conflict(f'Nonportable or unsafe path: {name!r}')
    return PurePosixPath(*parts).as_posix()


class Workspace:
    def __init__(self, root: Path, build_roots=()):
        self.root = root.resolve(strict=True)
        self.build_roots = [p.resolve(strict=True) for p in build_roots]
        for build in self.build_roots:
            if not build.is_relative_to(self.root / 'build'):
                raise Conflict(f'IDE build must be beneath {self.root / "build"}: {build}')
        self.state = self.root / 'build/project-sync'
        self.path('build/project-sync', internal=True)

    def relative(self, path: Path) -> str:
        try:
            return path.absolute().relative_to(self.root).as_posix()
        except ValueError as error:
            raise Conflict(f'Path escapes repository: {path}') from error

    def path(self, name: str, *, internal=False) -> Path:
        name = portable_name(name)
        path = self.root / name
        if name.split('/')[0] in {'.git', '.codex', '.agents', '.claude', 'external', 'out', '.cpm-cache'}:
            raise Conflict(f'Protected repository area: {name}')
        if name.split('/')[0] == 'build' and not internal:
            if not any(path.is_relative_to(build) for build in self.build_roots):
                raise Conflict(f'Build output is not source: {name}')
        current = self.root
        for part in PurePosixPath(name).parts:
            current = current / part
            if current.exists() or current.is_symlink():
                info = current.lstat()
                if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
                    raise Conflict(f'Symlink/reparse traversal is forbidden: {current}')
        if not path.resolve(strict=False).is_relative_to(self.root):
            raise Conflict(f'Resolved path escapes repository: {name}')
        return path

    def bytes(self, name: str, *, internal=False) -> bytes | None:
        path = self.path(name, internal=internal)
        if not path.is_file() or not self.exists_exact(name, internal=internal):
            return None
        return path.read_bytes()

    def exists_exact(self, name: str, *, internal=False) -> bool:
        path = self.path(name, internal=internal)
        if not path.exists():
            return False
        # On case-insensitive filesystems distinguish the two sides of a case-only rename.
        current = self.root
        for part in PurePosixPath(name.replace('\\', '/')).parts:
            if not any(child.name == part for child in current.iterdir()):
                return False
            current /= part
        return True

    @contextmanager
    def lock(self, timeout=10.0, name='writer'):
        name = portable_name(name)
        if '/' in name:
            raise Conflict('Lock name must be one path component')
        self.path('build/project-sync', internal=True).mkdir(parents=True, exist_ok=True)
        lock_file = self.path('build/project-sync/' + name + '.lock', internal=True)
        with lock_file.open('a+b') as stream:
            if lock_file.stat().st_size == 0:
                stream.write(b'\0')
                stream.flush()
            deadline = time.monotonic() + timeout
            while True:
                try:
                    stream.seek(0)
                    if os.name == 'nt':
                        import msvcrt
                        msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    else:
                        import fcntl
                        fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except (OSError, BlockingIOError) as error:
                    if time.monotonic() >= deadline:
                        raise Conflict('Another structure writer holds the repository lock; retry when it finishes') from error
                    time.sleep(0.05)
            try:
                yield
            finally:
                stream.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


class Transaction:
    def __init__(self, workspace: Workspace, reason: str):
        self.ws = workspace
        self.reason = reason
        self.writes: dict[str, bytes | None] = {}
        self.before: dict[str, bytes | None] = {}
        self.directories: set[str] = set()
        self.remove_directories: set[str] = set()
        self.moves: dict[str, str] = {}
        self.modes: dict[str, int] = {}
        self.reads: dict[str, str | None] = {}

    def read(self, name: str) -> bytes | None:
        name = portable_name(name)
        value = self.ws.bytes(name)
        self.reads.setdefault(name, digest(value))
        return self.writes.get(name, value)

    def write(self, name: str, value: bytes | None):
        name = portable_name(name)
        self.ws.path(name)
        if name not in self.before:
            self.before[name] = self.ws.bytes(name)
            self.reads.setdefault(name, digest(self.before[name]))
        self.writes[name] = value

    def mkdir(self, name: str):
        name = portable_name(name)
        path = self.ws.path(name)
        if path.exists() and not path.is_dir():
            raise Conflict(f'Directory collides with file: {name}')
        self.directories.add(name)

    def move(self, source: str, destination: str):
        source, destination = portable_name(source), portable_name(destination)
        old, new = self.ws.path(source), self.ws.path(destination)
        if source == destination:
            return
        self.moves[source] = destination
        if not old.exists():
            raise Conflict(f'Move source is missing: {source}')
        same_casefold = source.casefold() == destination.casefold()
        if new.is_relative_to(old) and not same_casefold:
            raise Conflict(f'Cannot move a directory inside itself: {source} -> {destination}')
        if new.exists() and not same_casefold:
            raise Conflict(f'Move destination already exists: {destination}')
        if old.is_dir():
            self.mkdir(destination)
            for item in sorted(old.rglob('*')):
                relative = self.ws.relative(item)
                self.ws.path(relative)
                suffix = item.relative_to(old).as_posix()
                if item.is_dir():
                    self.mkdir(destination + '/' + suffix)
                    self.remove_directories.add(relative)
                else:
                    self.write(destination + '/' + suffix, self.read(relative))
                    self.modes[destination + '/' + suffix] = stat.S_IMODE(item.stat().st_mode)
                    self.write(relative, None)
            self.remove_directories.add(source)
        else:
            data = self.read(source)
            self.write(destination, data)
            self.modes[destination] = stat.S_IMODE(old.stat().st_mode)
            self.write(source, None)

    def validate(self):
        final_paths = {name for name, value in self.writes.items() if value is not None}
        folded = {}
        for name in final_paths | self.directories:
            if name.casefold() in folded and folded[name.casefold()] != name:
                raise Conflict(f'Case collision: {folded[name.casefold()]} and {name}')
            folded[name.casefold()] = name
            path = self.ws.path(name)
            current = self.ws.root
            prefix = []
            for part in PurePosixPath(name).parts:
                if current.is_dir():
                    for child in current.iterdir():
                        if child.name.casefold() == part.casefold() and child.name != part:
                            collision = '/'.join(prefix + [child.name])
                            if collision not in self.remove_directories and self.writes.get(collision, b'') is not None:
                                raise Conflict(f'Portable case collision: {name} with {collision}')
                prefix.append(part)
                current /= part
            if path.is_dir() and name in final_paths:
                raise Conflict(f'File collides with directory: {name}')
        for name, expected in self.reads.items():
            if digest(self.ws.bytes(name)) != expected:
                raise Conflict(f'Concurrent edit: {name}; re-plan from current files')

    def describe(self):
        result = {'reason': self.reason,
                'writes': [{'path': name, 'before': digest(self.before[name]), 'after': digest(value),
                            'action': 'delete' if value is None else 'write'}
                           for name, value in sorted(self.writes.items()) if value != self.before[name]],
                'directories': sorted(self.directories), 'remove_empty_directories': sorted(self.remove_directories)}
        manifest = 'cmake/project-structure.json'
        if self.writes.get(manifest) is not None and self.writes[manifest] != self.before[manifest]:
            result['structure'] = json.loads(self.writes[manifest])
        return result

    def apply(self, *, fail_after=None):
        self.validate()
        changed = [(name, value) for name, value in self.writes.items() if value != self.before[name]]
        if (not changed and all(self.ws.path(p).is_dir() for p in self.directories)
                and not any(self.ws.path(p).exists() for p in self.remove_directories)):
            return None
        pending = incomplete(self.ws)
        if pending:
            raise Conflict(f'Incomplete transaction {pending[0]}; run recover before new writes')
        identity = time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:12]
        folder = self.ws.path(f'build/project-sync/journal/{identity}', internal=True)
        folder.mkdir(parents=True)
        records = []
        # Delete old spellings first, including case-only renames. All bytes already reside in the journal.
        for index, (name, value) in enumerate(sorted(changed, key=lambda item: (item[1] is not None, item[0]))):
            entry = {'path': name, 'before': digest(self.before[name]), 'after': digest(value)}
            entry['before_mode'] = stat.S_IMODE(self.ws.path(name).stat().st_mode) if self.before[name] is not None else None
            entry['after_mode'] = self.modes.get(name, entry['before_mode'])
            for label, data in [('before', self.before[name]), ('after', value)]:
                if data is not None:
                    blob = f'{index}-{label}.bin'
                    atomic_write(folder / blob, data)
                    entry[label + '_blob'] = blob
            records.append(entry)
        created = set()
        for name in self.directories | {str(PurePosixPath(n).parent) for n, v in changed if v is not None}:
            while name != '.' and not self.ws.exists_exact(name):
                created.add(name)
                name = str(PurePosixPath(name).parent)
        journal = {'version': 1, 'id': identity, 'root': str(self.ws.root), 'reason': self.reason,
                   'build_roots': [str(p) for p in self.ws.build_roots], 'phase': 'prepared',
                   'records': records, 'created_directories': sorted(created),
                   'removed_directories': sorted(self.remove_directories)}
        atomic_write(folder / 'transaction.json', json_bytes(journal))
        journal['phase'] = 'applying'
        atomic_write(folder / 'transaction.json', json_bytes(journal))
        count = 0
        directories_prepared = False
        # Deliberately do not auto-rollback exceptions: preserve conflicting user edits and explicit recovery.
        for entry in records:
            if entry['after'] is not None and not directories_prepared:
                for name in sorted(self.remove_directories, key=lambda value: -value.count('/')):
                    directory = self.ws.path(name)
                    if directory.is_dir():
                        if any(directory.iterdir()):
                            raise Conflict(f'Directory gained an unplanned item during transaction {identity}: {name}')
                        directory.rmdir()
                for name in sorted(self.directories, key=lambda value: value.count('/')):
                    self.ws.path(name).mkdir(parents=True, exist_ok=True)
                directories_prepared = True
            path = self.ws.path(entry['path'])
            if digest(self.ws.bytes(entry['path'])) != entry['before']:
                raise Conflict(f'Concurrent write during transaction {identity}: {entry["path"]}; recovery required')
            if entry['after'] is None:
                if path.exists():
                    path.unlink()
            else:
                atomic_write(path, (folder / entry['after_blob']).read_bytes(), entry['after_mode'])
            count += 1
            if fail_after == count:
                raise Conflict(f'Injected interruption in transaction {identity}')
        for name in sorted(self.directories, key=lambda value: value.count('/')):
            self.ws.path(name).mkdir(parents=True, exist_ok=True)
        for name in sorted(self.remove_directories, key=lambda value: -value.count('/')):
            path = self.ws.path(name)
            if path.is_dir() and not any(path.iterdir()):
                path.rmdir()
        journal['phase'] = 'applied'
        atomic_write(folder / 'transaction.json', json_bytes(journal))
        return identity


def incomplete(ws: Workspace):
    base = ws.path('build/project-sync/journal', internal=True)
    return [path.parent.name for path in sorted(base.glob('*/transaction.json'))
            if read_json(path)['phase'] in {'prepared', 'applying'}]


def recover(ws: Workspace, identity: str):
    identity = portable_name(identity)
    if '/' in identity:
        raise Conflict('Expected a transaction ID, not a path')
    folder = ws.path(f'build/project-sync/journal/{identity}', internal=True)
    journal = read_json(folder / 'transaction.json')
    if not journal or journal.get('version') != 1 or journal['root'] != str(ws.root):
        raise Conflict(f'Unknown or foreign transaction: {identity}')
    ws = Workspace(ws.root, [Path(p) for p in journal['build_roots']])
    for entry in journal['records']:
        current = digest(ws.bytes(entry['path']))
        if current not in {entry['before'], entry['after']}:
            raise Conflict(f'Recovery would overwrite a later edit: {entry["path"]}; preserve/reconcile it first')
        for label in ('before', 'after'):
            if label + '_blob' in entry:
                blob = folder / portable_name(entry[label + '_blob'])
                if digest(blob.read_bytes()) != entry[label]:
                    raise Conflict(f'Corrupt recovery blob: {blob}')
    # Remove created destinations first so a case-only rename can restore the original spelling.
    for entry in reversed(journal['records']):
        if entry['before'] is None and ws.bytes(entry['path']) is not None:
            ws.path(entry['path']).unlink()
    for name in sorted(journal['created_directories'], key=lambda value: -value.count('/')):
        path = ws.path(name)
        if path.is_dir() and not any(path.iterdir()):
            path.rmdir()
    for entry in journal['records']:
        if entry['before'] is not None:
            atomic_write(ws.path(entry['path']), (folder / entry['before_blob']).read_bytes(), entry.get('before_mode'))
    for name in journal['removed_directories']:
        ws.path(name).mkdir(parents=True, exist_ok=True)
    for name in sorted(journal['created_directories'], key=lambda value: -value.count('/')):
        path = ws.path(name)
        if path.is_dir() and not any(path.iterdir()):
            path.rmdir()
    journal['phase'] = 'rolled-back'
    atomic_write(folder / 'transaction.json', json_bytes(journal))
