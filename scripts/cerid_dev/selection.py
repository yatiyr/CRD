"""Read-only affected-build planning from Git, CMake File API and CTest JSON."""
from __future__ import annotations

from collections import defaultdict, deque
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess


class SelectionError(RuntimeError):
    """The available evidence cannot justify a narrow selection."""


def run_json(command, root, timeout=60):
    result = subprocess.run(command, cwd=root, capture_output=True, timeout=timeout)
    if result.returncode:
        raise SelectionError(f'{command[0]} exited {result.returncode}: '
                             + result.stderr.decode('utf-8', errors='replace'))
    try:
        return json.loads(result.stdout)
    except ValueError as error:
        raise SelectionError(f'{command[0]} did not return valid JSON: {error}') from error


def git(root, *args):
    result = subprocess.run(['git', '-C', str(root), *args], capture_output=True, timeout=60)
    if result.returncode:
        raise SelectionError('Git discovery failed: ' + result.stderr.decode('utf-8', errors='replace'))
    return result.stdout


def relative_name(value):
    """Git paths are repository relative, with no traversal or ambiguous separators."""
    path = PurePosixPath(value)
    if (not value or path.is_absolute() or '..' in path.parts or '\\' in value
            or ':' in value or any(ord(c) < 32 for c in value) or path.as_posix() != value):
        raise SelectionError(f'Nonportable repository path: {value!r}')
    return value


def parse_changes(raw):
    fields = raw.decode('utf-8').split('\0')
    if fields[-1] != '':
        raise SelectionError('Truncated Git name-status output')
    fields.pop()
    changes = []
    while fields:
        status = fields.pop(0)
        count = 2 if status.startswith(('R', 'C')) else 1
        if len(fields) < count or not re.fullmatch(r'(?:[ADMTUXB]|[RC](?:100|[0-9]{1,2}))', status):
            raise SelectionError('Malformed Git name-status output')
        paths = [relative_name(fields.pop(0)) for _ in range(count)]
        if count == 2:
            changes += [{'path': paths[0], 'status': 'D' if status.startswith('R') else 'M'},
                        {'path': paths[1], 'status': 'A'}]
        else:
            changes.append({'path': paths[0], 'status': status})
    return changes


def changes_from_git(root, base=None, head=None):
    if bool(base) != bool(head):
        raise SelectionError('CI discovery needs both --base and --head')
    current = git(root, 'rev-parse', '--verify', 'HEAD').decode().strip()
    if base:
        base = git(root, 'rev-parse', '--verify', '--end-of-options', base + '^{commit}').decode().strip()
        head = git(root, 'rev-parse', '--verify', '--end-of-options', head + '^{commit}').decode().strip()
        if head != current:
            raise SelectionError('CI head differs from the configured checkout; check out the actual candidate first')
        # A dirty candidate cannot stand in for an exact published revision.
        if git(root, 'status', '--porcelain=v1', '-z', '--untracked-files=all'):
            raise SelectionError('CI revision selection requires a clean working tree')
        raw = git(root, 'diff', '--no-ext-diff', '--no-textconv', '--no-color', '--name-status', '-z',
                  '--no-renames', base, head, '--')
        return parse_changes(raw), {'mode': 'ci', 'base': base, 'head': head}
    changes = parse_changes(git(root, 'diff', '--no-ext-diff', '--no-textconv', '--no-color', '--name-status', '-z',
                               '--no-renames', 'HEAD', '--'))
    for path in git(root, 'ls-files', '-z', '--others', '--exclude-standard').decode('utf-8').split('\0'):
        if path:
            changes.append({'path': relative_name(path), 'status': '?'})
    return sorted(changes, key=lambda change: change['path']), {'mode': 'worktree', 'base': current, 'head': current}


def content_identity(root, changes, revision):
    """Build-tree identity includes net bytes, deletions and symlink targets, without touching the index."""
    digest = hashlib.sha256(json.dumps(revision, sort_keys=True).encode())
    files = []
    for change in sorted(changes, key=lambda item: (item['path'], item['status'])):
        name = relative_name(change['path'])
        path = root / name
        if path.is_symlink():
            payload = str(path.readlink()).encode('utf-8')
            kind = 'symlink'
        elif path.is_file():
            payload = path.read_bytes()
            kind = 'file'
        elif path.exists():
            raise SelectionError(f'Changed path is not a regular file: {name}')
        else:
            payload = b''
            kind = 'missing'
        entry = {**change, 'kind': kind, 'sha256': hashlib.sha256(payload).hexdigest(),
                 'executable': bool(path.stat().st_mode & 0o111) if path.is_file() else False}
        digest.update(json.dumps(entry, sort_keys=True).encode())
        files.append(entry)
    return {'sha256': digest.hexdigest(), 'files': files}


def absolute(value, base):
    path = Path(value)
    return (path if path.is_absolute() else base / path).resolve()


def load_model(root, build, configuration=None):
    root, build = root.resolve(), build.resolve()
    reply = build / '.cmake/api/v1/reply'
    indices = sorted(reply.glob('index-*.json'))
    if not indices:
        raise SelectionError('Missing CMake File API reply; request codemodel v2/cmakeFiles v1 before configuring')
    index_path = indices[-1]
    if any(path.stat().st_mtime_ns > index_path.stat().st_mtime_ns for path in reply.glob('error-*.json')):
        raise SelectionError('A newer CMake configure error invalidates the previous model')
    fingerprints = {}

    def document(name):
        if not isinstance(name, str) or Path(name).name != name or '/' in name or '\\' in name:
            raise SelectionError('CMake reply reference must name one local JSON file')
        path = reply / name
        if path.resolve().parent != reply.resolve():
            raise SelectionError('CMake reply escaped its directory')
        data = path.read_bytes()
        fingerprints[name] = hashlib.sha256(data).hexdigest()
        return json.loads(data)

    try:
        index = document(index_path.name)
        objects = {item['kind']: item for item in index['objects']}
        for kind, version in (('codemodel', 2), ('cmakeFiles', 1)):
            if objects[kind]['version']['major'] != version:
                raise SelectionError(f'Unsupported {kind} version')
        model = document(objects['codemodel']['jsonFile'])
        if absolute(model['paths']['source'], root) != root or absolute(model['paths']['build'], build) != build:
            raise SelectionError('CMake model belongs to another checkout or build directory')
        configurations = model['configurations']
        if configuration is None:
            if len(configurations) != 1:
                raise SelectionError('Multi-configuration build requires an explicit --config')
            selected = configurations[0]
        else:
            selected = next((item for item in configurations if item['name'] == configuration), None)
            if selected is None:
                raise SelectionError(f'Configuration is absent from this model: {configuration}')
        references = selected['targets']
        names = {item['id']: item['name'] for item in references}
        if len(names) != len(references):
            raise SelectionError('Duplicate target IDs in CMake model')
        documents = {ref['id']: document(ref['jsonFile']) for ref in references}
        # Visual Studio emits an ALL_BUILD per project(). These are generator aggregates, not consumers.
        # Traversing their reverse edges would silently turn any edit into a whole-repository build.
        provided = {identifier for identifier, target in documents.items() if target.get('isGeneratorProvided')}
        authored_names = [name for identifier, name in names.items() if identifier not in provided]
        if len(set(authored_names)) != len(authored_names):
            raise SelectionError('Duplicate authored target names in CMake model')
        targets = {}
        for ref in references:
            target = documents[ref['id']]
            if target['id'] != ref['id'] or target['name'] != ref['name']:
                raise SelectionError('CMake target reference does not match its document')
            if ref['id'] in provided:
                continue
            targets[target['name']] = {
                'type': target['type'],
                'directory': absolute(target['paths']['source'], root),
                'sources': {absolute(item['path'], root) for item in target.get('sources', [])},
                'includes': {absolute(item['path'], root) for group in target.get('compileGroups', [])
                             for item in group.get('includes', [])},
                'artifacts': {absolute(item['path'], build) for item in target.get('artifacts', [])},
                'dependencies': {names[item['id']] for item in target.get('dependencies', [])
                                 if item['id'] not in provided},
            }
        cmake_files = document(objects['cmakeFiles']['jsonFile'])
        inputs = {absolute(item['path'], root) for item in cmake_files['inputs']
                  if not item.get('isGenerated') and not item.get('isExternal')}
        inputs.update(root / name for name in ('CMakePresets.json', 'CMakeUserPresets.json',
                                              'cmake/project-structure.json') if (root / name).is_file())
        stale = [str(path) for path in inputs if not path.is_file()
                 or path.stat().st_mtime_ns > index_path.stat().st_mtime_ns]
        if stale:
            raise SelectionError('Configure inputs changed after generation: ' + ', '.join(sorted(stale)[:8]))
        if index_path != sorted(reply.glob('index-*.json'))[-1]:
            raise SelectionError('CMake generation changed while reading the model; retry once generation finishes')
        return {'targets': targets, 'inputs': inputs, 'configuration': selected['name'],
                'cmake': index['cmake'], 'index': str(index_path),
                'sha256': hashlib.sha256(json.dumps(fingerprints, sort_keys=True).encode()).hexdigest()}
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        raise SelectionError(f'Incomplete or invalid CMake model: {error}') from error


def reverse_closure(targets, roots):
    reverse = defaultdict(set)
    for name, target in targets.items():
        for dependency in target['dependencies']:
            reverse[dependency].add(name)
    result, pending = set(roots), deque(sorted(roots))
    while pending:
        for name in sorted(reverse[pending.popleft()] - result):
            result.add(name)
            pending.append(name)
    return sorted(result)


def documentation(name):
    return (name.startswith('docs/') and Path(name).suffix.lower() in {'.md', '.png', '.jpg', '.svg', '.pdf'}) or name in {
        'AGENTS.md', 'CLAUDE.md', 'MEMORY.md', 'README.md', 'START_HERE.md', 'context.md'}


def select_targets(root, changes, model=None, model_error=None, full=False):
    targets = model['targets'] if model else {}
    reasons, owners = [], {}
    selected = set()
    risks = {'affected Windows/Linux CI; full matrix remains required for qualification'}
    known_inputs = model['inputs'] | {path for target in targets.values() for path in target['sources']} if model else set()
    code_changes = [item for item in changes if not documentation(item['path'])
                    or (root / item['path']).resolve() in known_inputs]
    if full:
        reasons.append('Explicit full qualification requested')
    if code_changes and not model:
        reasons.append(model_error or 'No configured target graph is available')
    for change in code_changes:
        name = relative_name(change['path'])
        path = (root / name).resolve()
        if not path.is_relative_to(root.resolve()):
            reasons.append(f'{name}: source resolves outside the checkout')
            continue
        if change['status'] not in {'M', 'T'}:
            reasons.append(f'{name}: addition/removal/rename needs conservative ownership and regenerated discovery')
        if (name.startswith(('assets/', 'scripts/', 'cmake/', '.github/', 'tools/'))
                or Path(name).name == 'CMakeLists.txt' or len(PurePosixPath(name).parts) == 1):
            reasons.append(f'{name}: build/tool/generated/runtime-asset consumption is broader than link edges')
        matched = set()
        for target_name, target in targets.items():
            if path in target['sources']:
                matched.add(target_name)
            if path.suffix.lower() in {'.h', '.hpp', '.hxx', '.inl', '.inc', '.ipp'}:
                if any(path.is_relative_to(include) for include in target['includes']):
                    matched.add(target_name)
        owners[name] = sorted(matched)
        selected.update(matched)
        if not matched:
            reasons.append(f'{name}: no proven source/header owner in this configuration')
        if '/include/' in name or path.suffix.lower() in {'.h', '.hpp', '.inl', '.inc', '.ipp'}:
            risks.add('public/header consumers, standalone headers and compiler/ABI diversity')
        if any(part in PurePosixPath(name).parts for part in ('gpu', 'rendering', 'execution')):
            risks.add('affected actual providers, validation, declared CPU/image oracle and hardware tuple')
        if any(part in PurePosixPath(name).parts for part in ('memory', 'jobs', 'vm', 'containers')):
            risks.add('lifetime/concurrency sanitizer and fiber instrumentation')
    scope = 'full' if reasons else ('affected' if code_changes else 'documentation')
    closure = sorted(targets) if scope == 'full' else reverse_closure(targets, selected)
    return {'scope': scope, 'reasons': sorted(set(reasons)), 'owners': owners, 'targets': closure,
            'build_targets': buildable_targets(targets, closure),
            'graph_available': bool(model), 'risk_checks': sorted(risks),
            'local_sweep_allowed': False,
            'guards': ['scripts/check-master-plan.py', 'scripts/check-repository.py'],
            'tidy_files': sorted(item['path'] for item in code_changes
                                 if Path(item['path']).suffix.lower() in {'.cpp', '.hpp', '.h', '.cc', '.cxx'}
                                 and (root / item['path']).is_file())}


def buildable_targets(targets, selected):
    # Never execute arbitrary UTILITY targets such as clean-all/uninstall/dashboard tasks from graph selection.
    kinds = {'EXECUTABLE', 'STATIC_LIBRARY', 'SHARED_LIBRARY', 'MODULE_LIBRARY', 'OBJECT_LIBRARY'}
    return sorted(name for name in selected if targets[name]['type'] in kinds)


def select_tests(inventory, model, target_names, full=False):
    if inventory.get('kind') != 'ctestInfo' or inventory.get('version', {}).get('major') != 1:
        raise SelectionError('Unsupported CTest JSON inventory')
    def normalized(path):
        return os.path.normcase(os.path.normpath(str(path)))

    artifacts = {normalized(path) for name in target_names for path in model['targets'][name]['artifacts']}
    artifact_owners = defaultdict(set)
    for name, target in model['targets'].items():
        for path in target['artifacts']:
            artifact_owners[normalized(path)].add(name)
    tests = {}
    for item in inventory['tests']:
        name = item['name']
        if name in tests:
            raise SelectionError(f'Duplicate CTest name: {name}')
        properties = {prop['name']: prop['value'] for prop in item.get('properties', [])}
        command = item.get('command', [])
        if not command or name.endswith('_NOT_BUILT'):
            raise SelectionError(f'CTest inventory is incomplete; build affected executables then rediscover: {name}')
        cwd = Path(properties.get('WORKING_DIRECTORY', Path(model['index']).parents[4]))
        # Artifact arguments also cover CMake/environment/emulator wrappers around an executable.
        # Normalize text without stat/resolve on arbitrary Catch2 filter arguments (which may exceed path limits).
        invoked = {normalized(os.path.join(str(cwd), arg))
                   for arg in command if isinstance(arg, str) and not arg.startswith('-')}
        bound = bool(invoked & artifact_owners.keys())
        target_owners = {owner for path in invoked for owner in artifact_owners.get(path, set())}
        tests[name] = {'properties': properties, 'targets': sorted(target_owners),
                       'selected': full or bool(invoked & artifacts) or not bound,
                       'reason': 'full' if full else ('target artifact' if bound else 'global/unresolved guard')}
    chosen = {name for name, test in tests.items() if test['selected']}
    while True:
        needed = {fixture for name in chosen for fixture in tests[name]['properties'].get('FIXTURES_REQUIRED', [])}
        dependencies = {name for selected in chosen for name in tests[selected]['properties'].get('DEPENDS', [])}
        unknown = dependencies - tests.keys()
        if unknown:
            raise SelectionError('CTest depends on unknown tests: ' + ', '.join(sorted(unknown)))
        expanded = chosen | dependencies | {
            name for name, test in tests.items()
            if needed & set(test['properties'].get('FIXTURES_SETUP', []) + test['properties'].get('FIXTURES_CLEANUP', []))}
        if expanded == chosen:
            break
        chosen = expanded
    if target_names and not any(set(tests[name]['targets']) & set(target_names) for name in chosen):
        raise SelectionError('Affected targets have no discovered tests; empty or guard-only runs cannot qualify them')
    return [{'name': name, 'reason': tests[name]['reason'] if tests[name]['selected'] else 'fixture/dependency',
             'disabled': bool(tests[name]['properties'].get('DISABLED', False)),
             'targets': tests[name]['targets'], 'properties': tests[name]['properties']} for name in sorted(chosen)]
