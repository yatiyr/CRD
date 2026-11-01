#!/usr/bin/env python3
"""Read-only repository hygiene and physical-layout gate (Windows/Linux, Python 3.12+)."""
from pathlib import Path
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# Ignored, untracked agent tool files that the NAS agent overlay keeps (scripts/workspace.py); never source.
AGENT_ROOT_FILES = {'CLAUDE.md', 'CLAUDE.local.md', 'opencode.json', '.mcp.json'}
AGENT_ROOT_DIRS = {'.claude', '.codex', '.agents', '.agent', '.agent-overlay'}
ROOT_FILES = {
    # `.git` is a file in a linked worktree (`gitdir: ...`), a directory in the main checkout.
    '.clang-format', '.clang-tidy', '.git', '.gitattributes', '.gitignore', '.natvis',
    'AGENTS.md', 'CMakeLists.txt', 'CMakePresets.json', 'CMakeUserPresets.json',
    'context.md', 'LICENSE', 'MEMORY.md', 'README.md', 'START_HERE.md',
} | AGENT_ROOT_FILES
ROOT_DIRS = AGENT_ROOT_DIRS | {
    '.git', '.github', '.vs', '.vscode', '.idea',
    '.cpm-cache', '.sccache', '.venv', 'assets', 'bench', 'build', 'ci', 'cmake', 'docs', 'engine',
    'external', 'out', 'runtime', 'sandbox', 'scripts', 'tests', 'tools',
}
SOURCE_FAMILIES = {
    'assets', 'execution', 'foundation', 'geometry', 'gpu', 'media', 'numerics',
    'physics', 'rendering', 'ui', 'world',
}
# Double-encoded UTF-8 (mojibake): UTF-8 bytes read as Windows-1252 and saved as UTF-8 again turn each byte 0x80-0xBF
# into its Windows-1252 character (a C1 control where Windows-1252 has none). A lead byte of a three- or four-byte
# character, or of a two-byte one from Latin-1 or Latin Extended-A (C2-C5), followed by such characters that decode
# back to one UTF-8 character is reported. Other two-byte leads are ordinary letters and signs ("x" followed by an en
# dash in "1.2x-3x" ranges, a capital U umlaut before an apostrophe in Turkish) and are not.
MOJIBAKE_TAIL = {(bytes([b]).decode('cp1252', errors='ignore') or chr(b)): b for b in range(0x80, 0xC0)}
MOJIBAKE = re.compile('[\u00c2-\u00c5\u00e0-\u00f4][' + re.escape(''.join(MOJIBAKE_TAIL)) + ']+')
# Legitimate text the mojibake rule matches, per file: the sequence and why it is right.
MOJIBAKE_EXCEPTIONS = {
    'engine/numerics/hesap-preconditioners/include/crd/hesap/preconditioners/spai.hpp':
        {'\u00c2\u00b7': 'A-hat (the column block A(I, J_k)) times m-hat'},
    'docs/sessions/2026-05-26-hesap-v4i-1-spai-fspai.md':
        {'\u00c2\u00b7': 'A-hat (the column block A(I, J_k)) times m-hat'},
}


def git(*args, data=None):
    result = subprocess.run(['git', '-C', str(ROOT), *args], input=data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode not in (0, 1) or (result.returncode == 1 and args[0] != 'check-ignore'):
        raise RuntimeError(result.stderr.decode('utf-8', errors='replace'))
    return result.stdout.decode('utf-8')


def canonical_asset_errors(root):
    errors = []
    for path in (root / 'assets').rglob('*'):
        if path.is_file() and path.suffix in {'.ceir', '.ckir', '.chir', '.chirgraph'}:
            raw = path.read_bytes()
            try:
                raw.decode('utf-8')
            except UnicodeDecodeError:
                errors.append(f'Canonical authored text must be valid UTF-8: {path.relative_to(root)}')
            if b'\r' in raw or raw.startswith(b'\xef\xbb\xbf'):
                errors.append(f'Canonical authored text must be UTF-8 without BOM and use LF: {path.relative_to(root)}')
    return errors


def mojibake(text):
    """Yield each double-encoded UTF-8 character of `text` as (the sequence found, the character it stands for)."""
    for match in MOJIBAKE.finditer(text):
        lead = ord(match[0][0])
        width = 2 if lead < 0xE0 else 3 if lead < 0xF0 else 4
        sequence = match[0][:width]
        if len(sequence) == width:
            try:
                yield sequence, bytes([lead, *(MOJIBAKE_TAIL[c] for c in sequence[1:])]).decode('utf-8')
            except UnicodeDecodeError:
                continue


def text_encoding_errors(root, names):
    """Authored text is UTF-8 without a byte order mark and holds no double-encoded UTF-8."""
    errors = []
    for name in names:
        raw = (root / name).read_bytes()
        if raw.startswith(b'\xef\xbb\xbf'):
            errors.append(f'Text file starts with a UTF-8 byte order mark: {name}')
        try:
            text = raw.decode('utf-8')
        except UnicodeDecodeError as error:
            errors.append(f'Text file is not valid UTF-8 (byte {error.start}): {name}')
            continue
        allowed = MOJIBAKE_EXCEPTIONS.get(name, {})
        found = [(sequence, character) for sequence, character in mojibake(text) if sequence not in allowed]
        if found:
            sequence, character = found[0]
            errors.append(f'Double-encoded UTF-8 in {name}: {len(found)} sequences, the first {ascii(sequence)} '
                          f'for {ascii(character)}')
    return errors


def text_names(listing):
    """The paths `git ls-files --eol -z` lists as text: not unset by attributes, not detected as binary."""
    names = []
    for entry in filter(None, listing.split('\0')):
        info, name = entry.split('\t', 1)
        index, _, attributes = info.split(None, 2)
        if index != 'i/-text' and attributes.strip() != 'attr/-text':
            names.append(name)
    return names


def root_artifact_errors(root):
    errors = []
    for path in sorted(root.iterdir()):
        allowed = ROOT_DIRS if path.is_dir() else ROOT_FILES
        if path.name not in allowed and not (path.is_dir() and path.name.startswith('cmake-build-')):
            errors.append(f'Unexpected root artifact: {path.name}; scratch belongs under build/<task>/')
    return errors


def check(root=ROOT):
    errors = canonical_asset_errors(root)
    structure_file = root / 'cmake/project-structure.json'
    from project_sync.model import load_manifest
    from project_sync.storage import Workspace, Conflict
    try:
        structure = load_manifest(Workspace(root)) if structure_file.exists() else {}
    except (Conflict, ValueError, TypeError, OSError) as error:
        errors.append(f'Invalid project structure manifest: {error}')
        structure = {}
    excluded_modules = set(structure.get('excluded_modules', []))
    authored_families = {name.split('/')[1] for name in structure.get('directories', [])
                        if name.startswith('engine/') and len(name.split('/')) == 2}
    errors.extend(root_artifact_errors(root))
    modules = sorted((root / 'engine').glob('*/*/CMakeLists.txt'))
    if not modules:
        errors.append('No engine modules found; layout scan did not exercise the source tree')
    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8-sig')
    for family in (root / 'engine').iterdir():
        if family.is_dir() and family.name not in SOURCE_FAMILIES | authored_families:
            errors.append(f'Unclassified engine directory: {family.relative_to(root)}')
    for module in modules:
        relative = module.parent.relative_to(root).as_posix()
        if relative not in excluded_modules and not re.search(r'(?:add_subdirectory|crd_module)\(' + re.escape(relative) + r'(?:\s|\))', cmake):
            errors.append(f'Module absent from build registration: {relative}')
    test_cmake = (root / 'tests/CMakeLists.txt').read_text(encoding='utf-8-sig')
    for source in re.findall(r'(?:add_subdirectory|crd_tests)\(([^\s)]+)', test_cmake):
        if not (root / 'tests' / source / 'CMakeLists.txt').is_file():
            errors.append(f'Test registration points to missing directory: {source}')
    if root == ROOT:
        tracked = [p for p in git('ls-files', '-z', '--cached', '--others', '--exclude-standard').split('\0')
                   if p and (root / p).is_file()]
        for name in tracked:
            if any(ord(c) < 32 or 0xe000 <= ord(c) <= 0xf8ff for c in name):
                errors.append(f'Nonportable/control character in tracked path: {name!r}')
            if re.search(r'\.(?:exe|obj|pdb|pyc|log)$', name, flags=re.I):
                errors.append(f'Generated binary/capture in source: {name}')
            if re.search(r'(?:^|/)[^/]*[-_]tmp\.(?:bat|ps1)$', name):
                errors.append(f'One-shot temporary script in source: {name}')
        errors.extend(text_encoding_errors(root, [name for name in text_names(git('ls-files', '--eol', '-z'))
                                                  if (root / name).is_file()]))
        visible = ['AGENTS.md', 'context.md', 'MEMORY.md', 'START_HERE.md', 'docs/README.md',
                   'assets/example.sdf', 'assets/example.ckir', 'assets/example.ceir',
                   'tests/example/reference.glb', 'docs/bench/example.md']
        hidden = ['build/scratch/run.txt', '.vs/session.bin', 'CMakeUserPresets.json',
                  'external/ilupack/lib.a', '.cpm-cache/package/file', '__pycache__/module.pyc',
                  *sorted(AGENT_ROOT_FILES), *(name + '/state' for name in sorted(AGENT_ROOT_DIRS))]
        errors.extend('Agent file tracked by the code repository (it belongs to the NAS overlay): ' + p
                      for p in tracked if p.split('/')[0] in AGENT_ROOT_FILES | AGENT_ROOT_DIRS)
        ignored = set(git('check-ignore', '--no-index', '-z', '--stdin',
                          data=('\0'.join(visible + hidden) + '\0').encode()).split('\0'))
        errors.extend('Source hidden by ignore rules: ' + p for p in visible if p in ignored)
        errors.extend('Generated/local path is not ignored: ' + p for p in hidden if p not in ignored)
    return len(modules), errors


def main():
    count, errors = check()
    print(f'Checked repository root, {count} engine modules, test registrations and ignore contracts.')
    for error in errors:
        print('ERROR: ' + error)
    print('PASS' if not errors else f'FAIL: {len(errors)} issues')
    return bool(errors)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
