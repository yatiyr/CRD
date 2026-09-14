#!/usr/bin/env python3
"""Read cmake/pins.json and acquire pinned inputs with SHA-256 verification.

The registry (schema cerid-pins/1) is the only place a version, URL or hash is written; every helper reads it.
`fetch()` downloads to a `.partial` file, verifies the digest, and renames only then, so a partial or corrupt
download never sits under the final name; an already downloaded archive is verified before use.
Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / 'cmake/pins.json'
SCHEMA = 'cerid-pins/1'
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


def download(url, destination, expected, label):
    """Download url to destination through a .partial file; verify before the final name exists."""
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + '.partial')
    request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response, partial.open('wb') as output:
        shutil.copyfileobj(response, output)
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
