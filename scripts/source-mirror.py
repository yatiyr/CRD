#!/usr/bin/env python3
"""Keep a copy of the checkout on a native Linux disk (the WSL source mirror) and refuse a stale one.

    source-mirror.py sync  [--source DIR] [--mirror DIR] [--full]
        makes the mirror hold exactly the files git sees in the checkout (tracked and untracked, never the ignored
        ones) with the same bytes and times, and a copy of `.git` (the repository guards run git in the source
        directory). An incremental sync copies only what git reports as different on either side; --full (and the
        first sync) compares the bytes of every file and copies those that differ. Then it runs the check.
    source-mirror.py check [--source DIR] [--mirror DIR] [--full]
        exit 0 when the mirror and the checkout have the same HEAD, the same `git status` (every modified, added,
        deleted and untracked path) and identical bytes in each of those paths; every other file then equals the
        same index entry on both sides. --full compares the bytes of every file instead (about a minute through the
        drive mount). Exit 3 with the differences otherwise. Every build and test of a mirror tree runs a check first
        (scripts/wsl-lane.sh), so Linux evidence never comes from stale sources.

The checkout's status is read with the Windows git through WSL interop when it is available (git on a drvfs
checkout re-reads every file, about 20 s of stat calls alone), else with the Linux git. The source defaults to the
checkout this script lives in when that is a /mnt drive, or CRD_MIRROR_SOURCE; the mirror to CRD_SOURCE_MIRROR, else
~/cerid-src. Build trees live outside the mirror: a sync deletes every file git does not see there, ignored ones
included. Contract: docs/design/build-performance.md#wsl-source-mirror.
"""
from pathlib import Path
import argparse
import os
import shutil
import stat
import subprocess
import sys

STALE = 3
CHUNK = 1 << 20
WINDOWS_GIT = ('git.exe', '/mnt/c/Program Files/Git/cmd/git.exe')


def default_source():
    if os.environ.get('CRD_MIRROR_SOURCE'):
        return Path(os.environ['CRD_MIRROR_SOURCE'])
    here = Path(__file__).resolve().parents[1]
    if str(here).startswith('/mnt/'):
        return here
    raise SystemExit('source-mirror: give --source (or CRD_MIRROR_SOURCE); %s is not a /mnt checkout' % here)


def default_mirror():
    return Path(os.environ.get('CRD_SOURCE_MIRROR') or Path.home() / 'cerid-src')


class Git:
    """Runs git for one side: the Windows git (by its Windows path) for a /mnt checkout when interop works."""

    def __init__(self, root, windows=None):
        self.root = Path(root)
        self.command = ['git', '-C', str(self.root)]
        if windows is None:
            windows = str(self.root).startswith('/mnt/')
        if windows:
            native = windows_git()
            translated = to_windows_path(self.root) if native else None
            if native and translated:
                self.command = [native, '-C', translated]

    def run(self, *arguments):
        return subprocess.run(self.command + list(arguments), capture_output=True, check=True).stdout

    def status(self):
        """{path: 'XY'} for every path git reports (porcelain v1, untracked files listed one by one, no renames)."""
        raw = self.run('status', '--porcelain=v1', '-z', '--untracked-files=all', '--no-renames')
        entries = {}
        for record in raw.split(b'\0'):
            if record:
                text = record.decode('utf-8', errors='surrogateescape')
                entries[text[3:]] = text[:2]
        return entries

    def head(self):
        return self.run('rev-parse', 'HEAD').decode('ascii').strip()

    def files(self):
        listed = self.run('ls-files', '-z', '--cached', '--others', '--exclude-standard')
        names = set(name.decode('utf-8', errors='surrogateescape') for name in listed.split(b'\0') if name)
        return sorted(name for name in names if os.path.lexists(self.root / name))

    def ignored(self):
        listed = self.run('ls-files', '-z', '--others', '--ignored', '--exclude-standard')
        return sorted(name.decode('utf-8', errors='surrogateescape') for name in listed.split(b'\0') if name)


def windows_git():
    for candidate in WINDOWS_GIT:
        found = shutil.which(candidate) or (candidate if os.path.isfile(candidate) else None)
        if found:
            try:
                subprocess.run([found, '--version'], capture_output=True, check=True, timeout=30)
                return found
            except (OSError, subprocess.SubprocessError):
                continue
    return None


def to_windows_path(path):
    try:
        return subprocess.run(['wslpath', '-w', str(path)], capture_output=True, text=True, check=True,
                              timeout=30).stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def mirror_files(mirror):
    """Every file and link in the mirror outside its .git directory, relative."""
    found = []
    for directory, subdirectories, files in os.walk(mirror):
        if directory == str(mirror):
            subdirectories[:] = [d for d in subdirectories if d != '.git']
        for name in files:
            found.append(os.path.relpath(os.path.join(directory, name), mirror).replace(os.sep, '/'))
    return sorted(found)


def same_bytes(first, second):
    with open(first, 'rb') as a, open(second, 'rb') as b:
        while True:
            left, right = a.read(CHUNK), b.read(CHUNK)
            if left != right:
                return False
            if not left:
                return True


def same_path(source, mirror, name):
    left, right = os.path.join(source, name), os.path.join(mirror, name)
    exists = os.path.lexists(left), os.path.lexists(right)
    if exists != (True, True):
        return exists == (False, False)
    return os.path.getsize(left) == os.path.getsize(right) and same_bytes(left, right)


def differences(source, mirror, full=False, checkout=None, copy=None):
    """Lines naming each difference between the checkout and the mirror (empty when they agree)."""
    checkout = checkout or Git(source)
    copy = copy or Git(mirror, windows=False)
    if not (Path(mirror) / '.git').exists():
        return ['the mirror %s has never been synced' % mirror]
    problems = []
    checkout_head, mirror_head = checkout.head(), copy.head()
    if checkout_head != mirror_head:
        problems.append('HEAD differs: checkout %s, mirror %s' % (checkout_head, mirror_head))
    if full:
        wanted, present = set(checkout.files()), set(mirror_files(mirror))
        problems += ['missing in the mirror: %s' % name for name in sorted(wanted - present)]
        problems += ['not in the checkout: %s' % name for name in sorted(present - wanted)]
        names = sorted(wanted & present)
    else:
        left, right = checkout.status(), copy.status()
        for name in sorted(set(left) | set(right)):
            if left.get(name) != right.get(name):
                problems.append('status differs: %s (checkout %r, mirror %r)' % (name, left.get(name), right.get(name)))
        names = sorted(set(left) & set(right))
    problems += ['different bytes: %s' % name for name in names if not same_path(source, mirror, name)]
    return problems


def remove_file(path):
    """Unlink a file, also one git or the copy made read-only (Windows refuses to delete those)."""
    try:
        os.unlink(path)
    except PermissionError:
        os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
        os.unlink(path)


def replace_file(source, target):
    """Copy bytes, times and mode over whatever the target was."""
    if os.path.lexists(target):
        remove_file(target)
    shutil.copy2(source, target)


def copy_one(source, mirror, name):
    target = os.path.join(mirror, name)
    if os.path.lexists(os.path.join(source, name)):
        os.makedirs(os.path.dirname(target), exist_ok=True)
        replace_file(os.path.join(source, name), target)
        return 'copied'
    if os.path.lexists(target):
        remove_file(target)
        return 'removed'
    return None


def remove_empty_directories(mirror):
    for directory, _, _ in os.walk(mirror, topdown=False):
        relative = os.path.relpath(directory, mirror).replace(os.sep, '/')
        if relative != '.' and relative.split('/')[0] != '.git' and not os.listdir(directory):
            os.rmdir(directory)


def mirror_directory(source, target):
    """Make target a copy of the source directory (files compared by size and time, extras deleted)."""
    source, target = Path(source), Path(target)
    seen = set()
    for directory, _, files in os.walk(source):
        relative = os.path.relpath(directory, source)
        os.makedirs(target / relative, exist_ok=True)
        for name in files:
            path = os.path.normpath(os.path.join(relative, name))
            seen.add(path)
            left, right = source / path, target / path
            mine = os.lstat(left)
            try:
                other = os.lstat(right)
                current = other.st_size == mine.st_size and other.st_mtime_ns == mine.st_mtime_ns
            except FileNotFoundError:
                current = False
            if not current:
                replace_file(left, right)
    for directory, _, files in os.walk(target, topdown=False):
        for name in files:
            path = os.path.normpath(os.path.join(os.path.relpath(directory, target), name))
            if path not in seen:
                remove_file(target / path)
        if directory != str(target) and not os.listdir(directory):
            os.rmdir(directory)


def sync(source, mirror, full=False, out=sys.stdout):
    source, mirror = Path(source), Path(mirror)
    if not (source / '.git').exists():
        raise SystemExit('source-mirror: %s is not a git checkout' % source)
    if mirror.resolve() == source.resolve() or source.resolve() in mirror.resolve().parents:
        raise SystemExit('source-mirror: the mirror must lie outside the checkout')
    first = not (mirror / '.git').exists()
    mirror.mkdir(parents=True, exist_ok=True)
    # The repository first: the mirror's status is then taken against the checkout's index and HEAD.
    mirror_directory(source / '.git', mirror / '.git')
    checkout, copy = Git(source), Git(mirror, windows=False)
    counts = {'copied': 0, 'removed': 0}
    if first or full:
        files = checkout.files()
        for name in files:
            left, right = source / name, mirror / name
            if not os.path.lexists(right) or not same_path(source, mirror, name):
                right.parent.mkdir(parents=True, exist_ok=True)
                replace_file(left, right)
                counts['copied'] += 1
            elif os.lstat(left).st_mtime_ns != os.lstat(right).st_mtime_ns:
                shutil.copystat(left, right)
        wanted = set(files)
        for name in mirror_files(mirror):
            if name not in wanted:
                remove_file(mirror / name)
                counts['removed'] += 1
    else:
        for name in sorted(set(checkout.status()) | set(copy.status())):
            done = copy_one(source, mirror, name)
            if done:
                counts[done] += 1
        for name in copy.ignored():
            remove_file(mirror / name)
            counts['removed'] += 1
    # The statuses above may have refreshed either index; take the checkout's again, so the mirror's next status
    # compares content against it instead of trusting stat data its own git recorded before the copies.
    replace_file(source / '.git' / 'index', mirror / '.git' / 'index')
    remove_empty_directories(mirror)
    out.write('source-mirror: %s sync of %s: %d copied, %d removed\n'
              % ('full' if first or full else 'incremental', mirror, counts['copied'], counts['removed']))


def report(problems, out):
    if not problems:
        return 0
    out.write('source-mirror: the mirror differs from the checkout (%d differences); sync it first '
              '(scripts/wsl-lane.sh sync):\n' % len(problems))
    for line in problems[:20]:
        out.write('  ' + line + '\n')
    if len(problems) > 20:
        out.write('  ... %d more\n' % (len(problems) - 20))
    return STALE


def main(argv=None, out=sys.stdout):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('command', choices=('sync', 'check'))
    parser.add_argument('--source', type=Path)
    parser.add_argument('--mirror', type=Path)
    parser.add_argument('--full', action='store_true', help='copy or compare every file, not only what git reports')
    args = parser.parse_args(argv)
    source = args.source or default_source()
    mirror = args.mirror or default_mirror()
    if args.command == 'sync':
        sync(source, mirror, args.full, out)
    code = report(differences(source, mirror, args.full), out)
    if code == 0:
        out.write('source-mirror: %s matches %s (%s check)\n' % (mirror, source, 'full' if args.full else 'status'))
    return code


if __name__ == '__main__':
    sys.exit(main())
