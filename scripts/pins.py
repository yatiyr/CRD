#!/usr/bin/env python3
"""Read cmake/pins.json and acquire pinned inputs with SHA-256 verification.

The registry (schema cerid-pins/1) is the only place a version, URL or hash is written; every helper reads it.
`fetch()` downloads to a `.partial` file, verifies the digest, and renames only then, so a partial or corrupt
download never sits under the final name; an already downloaded archive is verified before use.
Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path, PurePosixPath
import argparse
import copy
import hashlib
import http.client
import json
import os
import platform
import posixpath
import shutil
import sys
import tarfile
import time
import urllib.error
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / 'cmake/pins.json'
SCHEMA = 'cerid-pins/1'
WINDOWS = platform.system() == 'Windows'
# The public SDK endpoints reject Python's default user agent. Identify the actual client; identity still comes from
# the pinned SHA-256, never from the HTTP response alone.
USER_AGENT = 'Cerid-SDK-Installer/1.0 (+https://github.com/yatiyr/CRD)'


class PinError(ValueError):
    pass


def load(path=PINS):
    document = json.loads(Path(path).read_text(encoding='utf-8'))
    if document.get('schema') != SCHEMA:
        raise PinError(f'{path}: expected schema {SCHEMA}, got {document.get("schema")!r}')
    return document


def entry(section, name, document=None):
    document = load() if document is None else document
    try:
        return document[section][name]
    except KeyError as error:
        raise PinError(f'cmake/pins.json has no {section}/{name}') from error


def sha256(path):
    with Path(path).open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def verify(path, expected, label):
    actual = sha256(path)
    if actual != expected:
        raise PinError(f'{label} checksum mismatch: expected {expected}, got {actual} ({path})')
    return Path(path)


# A transfer that fails in the network (a stalled read, a reset or refused connection, a truncated body, a server-side
# HTTP status) is tried again after these waits in seconds; a client-side HTTP status and a checksum mismatch never are.
RETRY_WAITS = (5, 15, 45)


def transient(error):
    if isinstance(error, urllib.error.HTTPError):
        return error.code >= 500 or error.code in (408, 429)
    return isinstance(error, (TimeoutError, ConnectionError, http.client.HTTPException, urllib.error.URLError))


def download(url, destination, expected, label, sleep=time.sleep):
    """Download url to destination through a .partial file; verify before the final name exists."""
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + '.partial')
    request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    for attempt in range(len(RETRY_WAITS) + 1):
        try:
            with urllib.request.urlopen(request, timeout=120) as response, partial.open('wb') as output:
                shutil.copyfileobj(response, output)
            break
        except (OSError, http.client.HTTPException) as error:
            partial.unlink(missing_ok=True)
            if attempt == len(RETRY_WAITS) or not transient(error):
                raise
            print(f'{label}: download attempt {attempt + 1} failed ({error!r}); retrying in {RETRY_WAITS[attempt]} s',
                  file=sys.stderr)
            sleep(RETRY_WAITS[attempt])
    try:
        verify(partial, expected, label)
    except PinError:
        partial.unlink(missing_ok=True)
        raise
    partial.replace(destination)
    return destination


def fetch(pin, destination_dir, archive=None, label=None, url=None, sha=None, file=None):
    """Return the verified local path of a pinned input.

    `pin` is a registry entry (or None when url/sha/file are given). An explicit `archive` is verified in place;
    otherwise `<destination_dir>/<file>` is verified when present and downloaded when absent.
    """
    url = url or pin['url']
    sha = sha or pin['sha256']
    file = file or pin.get('file') or url.rsplit('/', 1)[-1]
    label = label or file
    if archive is not None:
        return verify(archive, sha, label)
    destination = Path(destination_dir) / file
    if destination.exists():
        return verify(destination, sha, label)
    return download(url, destination, sha, label)


def extract_member(archive, member, output):
    """Copy exactly `member` out of a zip or tar archive to `output`; archive paths never choose the location, and a
    member that is not a regular file (a directory, a link) is refused."""
    archive = Path(archive)
    output = Path(output)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as package:
            info = package.getinfo(member)
            if info.is_dir():
                raise PinError(f'{archive.name}: member is not a regular file: {member}')
            with package.open(info) as source, output.open('wb') as target:
                shutil.copyfileobj(source, target)
        return output
    with tarfile.open(archive, 'r:*') as package:
        info = package.getmember(member)
        if not info.isfile():
            raise PinError(f'{archive.name}: member is not a regular file: {member}')
        source = package.extractfile(info)
        with source, output.open('wb') as target:
            shutil.copyfileobj(source, target)
    return output


def install_member(pin, destination, archive=None, download_dir=None, label=None):
    """Fetch (or verify) `pin`'s archive, extract its `member` into `destination` and verify `member_sha256`; a
    mismatching member is removed before the error. Returns the installed file."""
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    download_dir = Path(download_dir).resolve() if download_dir else destination
    label = label or pin.get('file') or pin['member']
    archive = fetch(pin, download_dir, archive, label=label + ' archive')
    output = extract_member(archive, pin['member'], destination / Path(pin['member']).name)
    actual = sha256(output)
    if actual != pin['member_sha256']:
        output.unlink()
        raise PinError(f'{label} binary checksum mismatch: expected {pin["member_sha256"]}, got {actual}')
    output.chmod(0o755)
    return output


def tree_members(package, root, top=None):
    """The tar archive's members below `root` (which must end in a slash), with the root stripped, and the files
    directly in `top` (an installer's archive, whose top directory holds its component `root` beside the installer's
    own files: its script, metadata and licences), named as in the top; any other member is refused, so the archive
    cannot place a file beside the tree. Links sort last, so on Windows a link's target is always extracted before
    it. Returns the members below the root and those beside it."""
    if not root.endswith('/'):
        raise PinError(f'the pinned root must end in a slash: {root}')
    if top is not None and (not top.endswith('/') or not root.startswith(top) or root == top):
        raise PinError(f'the pinned top {top} must be a directory holding the root {root}')
    members = []
    beside = []
    for info in package.getmembers():
        name = info.name.rstrip('/')
        if name == root.rstrip('/') or (top is not None and name == top.rstrip('/')):
            continue
        if name.startswith(root):
            info.name = name[len(root):]
            members.append(info)
        elif top is not None and name.startswith(top) and '/' not in name[len(top):] and info.isfile():
            info.name = name[len(top):]
            beside.append(info)
        else:
            raise PinError(f'archive member outside {root}: {info.name}')
    return sorted(members, key=lambda info: info.issym() or info.islnk()), beside


def tree_filter(member, path):
    """tarfile's `data` filter (no absolute or escaping path or link, no device file); on Windows a symlink it
    accepted becomes a hard link to the same member: a Windows symlink needs a privilege and cannot resolve a POSIX
    separator."""
    member = tarfile.data_filter(member, path)
    if WINDOWS and member.issym():
        target = posixpath.normpath(posixpath.join(posixpath.dirname(member.name), member.linkname))
        member = copy.copy(member)
        member.type, member.linkname = tarfile.LNKTYPE, target
    return member


def tree_parts(pin):
    """The archives of a pinned tree: the pin itself, or each of its `files` (one prefix unpacked from several
    archives, each below its own `root`), with the archive's file name."""
    if 'files' not in pin:
        return [dict(pin, file=pin.get('file') or pin['url'].rsplit('/', 1)[-1])]
    return [dict(spec, file=name) for name, spec in pin['files'].items()]


def placed_alike(package, info, path):
    """Whether `path`, placed by an earlier archive of the same tree, is the directory, link or file `info` is."""
    if info.isdir():
        return path.is_dir() and not path.is_symlink()
    if info.issym():
        return path.is_symlink() and os.readlink(path) == info.linkname
    if info.isfile() and path.is_file() and not path.is_symlink() and path.stat().st_size == info.size:
        with package.extractfile(info) as member:
            return member.read() == path.read_bytes()
    return False


def notices_apart(members, notices, root):
    """`members` with the archive's own top-level notices (the names in `notices`, such as LICENSE) moved to
    `licenses/<archive root>/`, so each archive of a tree keeps its own licence text beside the others'."""
    for info in members:
        if info.name in notices:
            info.name = f'licenses/{root.rstrip("/")}/{info.name}'
    return members


def part_members(package, part, pin):
    """The members one archive of a pinned tree places, named as they land: those below the part's `root` but the
    names the pin's `omit` lists (an installer's per-component file that would clash in the prefix), and the archive's
    notices under `licenses/<archive top>/`. With a part `top`, the notices are its files beside the root and every
    other such file (an installer's script and metadata) is left out; without one, they are the root's own top-level
    files (notices_apart)."""
    top = part.get('top')
    members, beside = tree_members(package, part['root'], top)
    notices = pin.get('notices', ())
    members = [info for info in members if info.name not in pin.get('omit', ())]
    if top is None:
        return notices_apart(members, notices, part['root'])
    return members + notices_apart([info for info in beside if info.name in notices], notices, top)


def unplaced(package, members, staging, name):
    """`members` without those an earlier archive of the tree already placed; a path two archives both place must be
    the same directory, link or file in each, else the tree is refused."""
    fresh = []
    for info in members:
        path = staging / PurePosixPath(info.name)
        if not os.path.lexists(path):
            fresh.append(info)
        elif not placed_alike(package, info, path):
            raise PinError(f'{name}: {info.name} differs from what an earlier archive of the tree placed')
    return fresh


def link_inside_tree(staging, name, target):
    """Make `name` a relative symbolic link to `target`, both inside the tree: the layout a kit's tools expect where its
    archives place a directory under another name (a pin's `links`)."""
    link = staging / PurePosixPath(name)
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
    if posixpath.isabs(target) or resolved.startswith('../') or resolved == '..':
        raise PinError(f'the link {name} -> {target} leaves the tree')
    if not (staging / PurePosixPath(resolved)).exists():
        raise PinError(f'the link {name} -> {target} names nothing in the tree')
    if os.path.lexists(link):
        raise PinError(f'the link {name} -> {target} would replace a path an archive placed')
    os.symlink(target, link, target_is_directory=(staging / PurePosixPath(resolved)).is_dir())


def install_tree(pin, destination, download_dir, binary, stamp, archive=None, label=None, version_of=None):
    """Install every member of `pin`'s tar archive below the pin's `root` into `destination` and return the
    `binary` (a POSIX path inside the tree), unless a matching install is already there.

    A pin with `files` is one tree from several archives (tree_parts), each unpacked below its own root into the same
    prefix, in the registry's order; a path two of them place must be identical in both, the top-level files the
    pin's `notices` names go to `licenses/<archive top>/` and the names its `omit` lists stay out (part_members); the
    pin's `links` then add the relative links its tools expect (link_inside_tree). Each archive is fetched (or
    verified) through fetch(); `archive` hands in the one archive of a single-archive pin. The tree is assembled
    beside the destination and renamed into place only when complete; a pin with `member_sha256` has that member's
    digest checked inside the tree; with `version_of` (a callable returning the version the binary reports, or None)
    the binary must report the pinned version. The `stamp` file records the pin's version and digests, so a matching
    install is reused and a directory without one is never replaced."""
    parts = tree_parts(pin)
    if archive is not None and len(parts) > 1:
        raise PinError('a tree of several archives takes them from its download directory, not one handed-in archive')
    label = label or pin.get('file') or binary
    destination = Path(destination).resolve()
    digests = pin['sha256'] if 'files' not in pin else {part['file']: part['sha256'] for part in parts}
    expected = {'version': pin['version'], 'sha256': digests}
    expected.update({key: pin[key] for key in ('notices', 'omit', 'links') if key in pin})
    tool = destination / PurePosixPath(binary)

    def reported_version(path):
        return version_of(path) if version_of is not None else pin['version']

    try:
        recorded = json.loads((destination / stamp).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        recorded = None
    if recorded == expected and tool.is_file() and reported_version(tool) == pin['version']:
        print(f'{label} {pin["version"]} already installed at {tool}; stamp and version match.')
        return tool
    if destination.exists() and not (destination / stamp).is_file():
        raise PinError(f'{destination} exists and holds no kit this script installed; choose another destination')
    verified = [fetch(part, Path(download_dir).resolve(), archive, label=f'{label} archive {part["file"]}')
                for part in parts]
    staging = destination.with_name(destination.name + '.partial')
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    try:
        for part, path in zip(parts, verified):
            with tarfile.open(path, 'r:*', errorlevel=2) as package:
                members = unplaced(package, part_members(package, part, pin), staging, path.name)
                package.extractall(staging, members=members, filter=tree_filter)
        for name, target in pin.get('links', {}).items():
            link_inside_tree(staging, name, target)
        staged = staging / PurePosixPath(binary)
        if not staged.is_file():
            raise PinError(f'{label}: the kit has no {binary}')
        if pin.get('member_sha256'):
            member = PurePosixPath(pin['member'])
            if str(member) != pin['root'] + binary:
                raise PinError(f'the pinned member {member} is not the binary {pin["root"]}{binary}')
            actual = sha256(staged)
            if actual != pin['member_sha256']:
                raise PinError(f'{label} binary checksum mismatch: expected {pin["member_sha256"]}, got {actual}')
        reported = reported_version(staged)
        if reported != pin['version']:
            raise PinError(f'installed {label} reports {reported!r}, the registry pins {pin["version"]}')
        (staging / stamp).write_text(json.dumps(expected, indent=2) + '\n', encoding='utf-8')
        if destination.exists():
            shutil.rmtree(destination)
        staging.rename(destination)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    archives = 'archive' if len(parts) == 1 else f'{len(parts)} archives\''
    checks = f'{archives} and binary SHA-256 verified' if pin.get('member_sha256') else f'{archives} SHA-256 verified'
    print(f'Installed {label} {pin["version"]}; {checks}' + (', the binary reports the version.' if version_of
                                                              else '.'))
    return tool


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    show = sub.add_parser('show', help='print one pinned entry as JSON')
    show.add_argument('section')
    show.add_argument('name')
    get = sub.add_parser('fetch', help='download (or verify) one pinned archive')
    get.add_argument('section')
    get.add_argument('name')
    get.add_argument('--destination', type=Path, required=True, help='directory receiving the verified file')
    get.add_argument('--archive', type=Path, help='an already downloaded copy to verify instead of downloading')
    args = parser.parse_args(argv)
    pin = entry(args.section, args.name)
    if args.command == 'show':
        print(json.dumps(pin, indent=2))
        return 0
    path = fetch(pin, args.destination, args.archive, label=f'{args.section}/{args.name}')
    print(f'{path} verified ({pin["sha256"]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
