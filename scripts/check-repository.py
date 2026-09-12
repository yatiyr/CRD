#!/usr/bin/env python3
"""Read-only repository hygiene and physical-layout gate (Windows/Linux, Python 3.12+)."""
from pathlib import Path
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ROOT_FILES = {
    '.clang-format', '.clang-tidy', '.gitattributes', '.gitignore', '.natvis',
    'AGENTS.md', 'CLAUDE.md', 'CMakeLists.txt', 'CMakePresets.json', 'CMakeUserPresets.json',
    'context.md', 'MEMORY.md', 'opencode.json', 'README.md', 'START_HERE.md',
}
ROOT_DIRS = {
    '.git', '.github', '.vs', '.vscode', '.idea', '.claude', '.codex', '.agents',
    '.cpm-cache', '.venv', 'assets', 'bench', 'build', 'cmake', 'docs', 'engine',
    'external', 'out', 'runtime', 'sandbox', 'scripts', 'tests', 'tools',
}
SOURCE_FAMILIES = {
    'assets', 'execution', 'foundation', 'geometry', 'gpu', 'media', 'numerics',
    'physics', 'rendering', 'ui', 'world',
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


def check(root=ROOT):
    errors = canonical_asset_errors(root)
    structure_file = root / 'cmake/project-structure.json'
    try:
        from project_sync.model import load_manifest
        from project_sync.storage import Workspace, Conflict
        structure = load_manifest(Workspace(root)) if structure_file.exists() else {}
    except (Conflict, ValueError, TypeError, OSError) as error:
        errors.append(f'Invalid project structure manifest: {error}')
        structure = {}
    excluded_modules = set(structure.get('excluded_modules', []))
    authored_families = {name.split('/')[1] for name in structure.get('directories', [])
                        if name.startswith('engine/') and len(name.split('/')) == 2}
    for path in root.iterdir():
        allowed = ROOT_DIRS if path.is_dir() else ROOT_FILES
        if path.name not in allowed and not (path.is_dir() and path.name.startswith('cmake-build-')):
            errors.append(f'Unexpected root artifact: {path.name}; scratch belongs under build/<task>/')
    modules = sorted((root / 'engine').glob('*/*/CMakeLists.txt'))
    if not modules:
        errors.append('No engine modules found; layout scan did not exercise the source tree')
    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8-sig')
    for family in (root / 'engine').iterdir():
        if family.is_dir() and family.name not in SOURCE_FAMILIES | authored_families:
            errors.append(f'Unclassified engine directory: {family.relative_to(root)}')
    for module in modules:
        relative = module.parent.relative_to(root).as_posix()
        if relative not in excluded_modules and not re.search(r'add_subdirectory\(' + re.escape(relative) + r'(?:\s|\))', cmake):
            errors.append(f'Module absent from build registration: {relative}')
    test_cmake = (root / 'tests/CMakeLists.txt').read_text(encoding='utf-8-sig')
    for source in re.findall(r'add_subdirectory\(([^\s)]+)', test_cmake):
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
        visible = ['AGENTS.md', 'CLAUDE.md', 'context.md', 'MEMORY.md', 'START_HERE.md',
                   'assets/example.sdf', 'assets/example.ckir', 'assets/example.ceir',
                   'tests/example/reference.glb', 'docs/bench/example.md']
        hidden = ['build/scratch/run.txt', '.vs/session.bin', 'CMakeUserPresets.json',
                  'external/ilupack/lib.a', '.cpm-cache/package/file', '__pycache__/module.pyc']
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
