"""Immutable local result envelopes and content verification; not a signature or remote qualification."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath

MANIFEST = 'evidence.json'


class EvidenceError(RuntimeError):
    pass


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode('utf-8')


def file_digest(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def artifact_path(directory, name):
    path = PurePosixPath(name)
    if (not isinstance(name, str) or not name or path.is_absolute() or '..' in path.parts
            or path.as_posix() != name or '\\' in name or ':' in name or name == MANIFEST
            or any(ord(character) < 32 for character in name)):
        raise EvidenceError(f'Invalid evidence artifact path: {name!r}')
    candidate = directory / name
    current = directory
    for component in path.parts:
        current /= component
        if current.is_symlink() or current.is_junction():
            raise EvidenceError(f'Evidence cannot traverse a link: {name}')
    if not candidate.resolve().is_relative_to(directory):
        raise EvidenceError(f'Evidence path escapes its envelope: {name}')
    return candidate


def files(directory):
    result = []
    for path in sorted(directory.rglob('*')):
        if path.name == MANIFEST and path.parent == directory:
            continue
        name = path.relative_to(directory).as_posix()
        checked = artifact_path(directory, name)
        if checked.is_file():
            result.append(name)
    # Path ordering compares components (and folds case on Windows); envelopes compare portable POSIX names.
    # A directory "ctest/" and sibling "ctest.xml" need the same order during sealing and inspection.
    return sorted(result)


def seal(directory, record):
    if not isinstance(record, dict):
        raise EvidenceError('Evidence record must be an object')
    directory = Path(directory).resolve(strict=True)
    destination = directory / MANIFEST
    if destination.exists():
        raise EvidenceError('Evidence is already sealed; create a new run instead of rewriting its result')
    artifacts = [{'path': name, 'size': (directory / name).stat().st_size,
                  'sha256': file_digest(directory / name)} for name in files(directory)]
    payload = {'version': 1, 'kind': 'cerid-dev-evidence', 'record': record, 'artifacts': artifacts}
    payload['payload_sha256'] = hashlib.sha256(canonical(payload)).hexdigest()
    # Exclusive create makes retries/competing publishers fail instead of replacing evidence. A crash during this
    # small write produces an invalid/unsealed document, which inspect rejects; it cannot produce a false success.
    with destination.open('xb') as output:
        output.write(canonical(payload) + b'\n')
        output.flush()
        os.fsync(output.fileno())
    return inspect(directory)


def inspect(directory):
    directory = Path(directory).resolve(strict=True)
    manifest = directory / MANIFEST
    if not manifest.is_file() or manifest.stat().st_size > 32 * 1024 * 1024 or manifest.is_symlink():
        raise EvidenceError('Evidence manifest is missing, linked or exceeds 32 MiB')
    try:
        payload = json.loads(manifest.read_text(encoding='utf-8'))
        if (type(payload.get('version')) is not int or payload['version'] != 1
                or payload.get('kind') != 'cerid-dev-evidence' or not isinstance(payload['record'], dict)
                or not isinstance(payload['artifacts'], list)):
            raise EvidenceError('Unsupported evidence schema')
        expected = payload.pop('payload_sha256')
        if hashlib.sha256(canonical(payload)).hexdigest() != expected:
            raise EvidenceError('Evidence metadata checksum mismatch')
        names = []
        for artifact in payload['artifacts']:
            path = artifact_path(directory, artifact['path'])
            if not path.is_file():
                raise EvidenceError(f'Missing evidence artifact: {artifact["path"]}')
            if path.stat().st_size != artifact['size'] or file_digest(path) != artifact['sha256']:
                raise EvidenceError(f'Evidence artifact changed: {artifact["path"]}')
            names.append(artifact['path'])
        if len(set(names)) != len(names) or sorted(names) != files(directory):
            raise EvidenceError('Evidence artifact inventory changed or contains duplicate paths')
        return {'version': 1, 'kind': 'evidence-inspection', 'integrity': 'verified',
                'directory': str(directory), 'payload_sha256': expected, 'artifact_count': len(names),
                'record': payload['record'],
                'qualification': 'content integrity only; the recorded scope/outcome and external gates still apply'}
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        raise EvidenceError(f'Invalid or incomplete evidence: {error}') from error
