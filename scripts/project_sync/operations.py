"""Shared human/agent/IDE operations and reference migration."""
from __future__ import annotations

import copy
import os
from pathlib import Path, PurePosixPath
import re

from .model import MANIFEST, load_manifest, membership, save_manifest, target_settings
from .storage import Conflict, Transaction, Workspace, portable_name

TEXT_ROOTS = ('engine', 'tests', 'sandbox', 'runtime', 'tools', 'scripts', 'cmake', 'docs', 'assets', '.github')
TEXT_SUFFIXES = {'.cpp', '.hpp', '.h', '.c', '.cmake', '.txt', '.md', '.py', '.ps1', '.bat', '.json', '.toml',
                 '.yml', '.yaml', '.ckir', '.ceir', '.chir', '.crdl', '.crdv', '.crdp', '.frame'}


def remap(name: str, moves: dict[str, str]) -> str:
    for old, new in sorted(moves.items(), key=lambda pair: -len(pair[0])):
        if name == old or name.startswith(old + '/'):
            return new + name[len(old):]
    return name


def source_files(ws: Workspace):
    for path in ws.root.iterdir():
        if path.is_file() and path.suffix in TEXT_SUFFIXES:
            yield ws.relative(path)
    for area in TEXT_ROOTS:
        base = ws.root / area
        if not base.is_dir():
            continue
        for directory, children, files in os.walk(base, followlinks=False):
            children[:] = [name for name in children if name not in {'__pycache__', '.vs', '.git', 'node_modules', 'papers'}]
            for name in files:
                path = Path(directory) / name
                if path.suffix in TEXT_SUFFIXES:
                    ws.path(ws.relative(path))
                    yield ws.relative(path)


def relative_replacement(value: str, file: str, new_file: str, moves: dict[str, str], root: Path):
    """Resolve literal path tokens without executing either CMake or shell expressions."""
    prefix = ''
    original = value
    source_parent = PurePosixPath(file).parent
    new_parent = PurePosixPath(new_file).parent
    variables = {'${CMAKE_SOURCE_DIR}/': '.', '${PROJECT_SOURCE_DIR}/': '.',
                 '${CMAKE_CURRENT_SOURCE_DIR}/': str(source_parent), '${CMAKE_CURRENT_LIST_DIR}/': str(source_parent)}
    for variable, parent in variables.items():
        if value.startswith(variable):
            prefix, value = variable, value[len(variable):]
            source_parent = PurePosixPath(parent)
            if parent == '.':
                new_parent = PurePosixPath('.')
            break
    if '$' in value or ';' in value or not value:
        return original
    if value.startswith(root.as_posix() + '/'):
        tail = value[len(root.as_posix()) + 1:]
        return prefix + root.as_posix() + '/' + remap(tail, moves)
    candidate = Path(os.path.normpath(str(root / str(source_parent) / value)))
    if not candidate.is_relative_to(root):
        return original
    name = candidate.relative_to(root).as_posix()
    mapped = remap(name, moves)
    if mapped == name and (source_parent == new_parent or not candidate.exists()):
        return original
    result = os.path.relpath(root / mapped, root / str(new_parent)).replace('\\', '/')
    return prefix + result


def cmake_tokens(text: str):
    """Yield literal argument spans, preserving comments, quotes, brackets and whitespace."""
    index = 0
    while index < len(text):
        if text[index] == '#':
            bracket = re.match(r'#\[(=*)\[', text[index:])
            if bracket:
                end = text.find(']' + bracket[1] + ']', index + len(bracket[0]))
                if end < 0:
                    raise Conflict('Unterminated CMake bracket comment')
                index = end + len(bracket[1]) + 2
            else:
                end = text.find('\n', index)
                index = len(text) if end < 0 else end + 1
        elif text[index] == '"':
            start = index + 1
            index = start
            while index < len(text):
                if text[index] == '\\':
                    index += 2
                elif text[index] == '"':
                    break
                else:
                    index += 1
            if index >= len(text):
                raise Conflict('Unterminated CMake quoted argument')
            yield start, index, text[start:index]
            index += 1
        elif match := re.match(r'\[(=*)\[', text[index:]):
            start = index + len(match[0])
            end = text.find(']' + match[1] + ']', start)
            if end < 0:
                raise Conflict('Unterminated CMake bracket argument')
            yield start, end, text[start:end]
            index = end + len(match[1]) + 2
        elif text[index].isspace() or text[index] in '()':
            index += 1
        else:
            start = index
            while index < len(text) and not text[index].isspace() and text[index] not in '()#':
                index += 1
            yield start, index, text[start:index]


def cmake_commands(text: str):
    """Command spans, with quoted/bracket/comment parentheses excluded from matching."""
    mask = list(text)
    token_spans = list(cmake_tokens(text))
    index = 0
    while index < len(text):
        start = index
        bracket = re.match(r'#?\[(=*)\[', text[index:])
        if bracket:
            end = text.find(']' + bracket[1] + ']', index + len(bracket[0]))
            index = len(text) if end < 0 else end + len(bracket[1]) + 2
        elif text[index] == '#':
            end = text.find('\n', index)
            index = len(text) if end < 0 else end
        elif text[index] == '"':
            index += 1
            while index < len(text):
                if text[index] == '\\':
                    index += 2
                elif text[index] == '"':
                    index += 1
                    break
                else:
                    index += 1
        else:
            index += 1
            continue
        mask[start:index] = ' ' * (index - start)
    masked = ''.join(mask)
    for match in re.finditer(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', masked):
        start, cursor, value = match.start(), match.end() - 1, match[1]
        depth = 1
        close = cursor + 1
        while close < len(text) and depth:
            if masked[close] == '(':
                depth += 1
            elif masked[close] == ')':
                depth -= 1
            close += 1
        if depth:
            raise Conflict(f'Unbalanced CMake command: {value}')
        arguments = [token for left, right, token in token_spans if left > cursor and right < close]
        yield start, close, value.lower(), arguments


REGISTRATIONS = {'add_subdirectory', 'crd_module', 'crd_tests'}
REGISTRY_ANCHORS = {'crd_resolve_modules', 'crd_stage_warp_dll', 'crd_apply_project_structure', 'crd_organize_targets'}


def registration_file(directory: str):
    prefix = 'tests/' if directory.startswith('tests/') else ''
    return prefix, ('tests/CMakeLists.txt' if prefix else 'CMakeLists.txt')


def declare_tests(text: str, module_source: str, test_source: str) -> str:
    """Add a TESTS entry to the crd_module() registration of the owning engine module, when one exists."""
    for start, end, command, args in cmake_commands(text):
        if command == 'crd_module' and args and args[0] == module_source:
            if test_source in args:
                return text
            close = text.rindex(')', start, end + 1)
            keyword = '' if 'TESTS' in args else 'TESTS '
            insert = f' {keyword}{test_source}'
            if 'TESTS' in args:
                # append after the last TESTS value: the keyword group runs until the next keyword
                keywords = {'HOST', 'EXECUTABLES', 'DEPENDS', 'PACKAGES', 'TESTS', 'TEST_DEPENDS', 'TEST_PACKAGES'}
                spans = [(left, right, token) for left, right, token in cmake_tokens(text[start:close])]
                index = next(i for i, span in enumerate(spans) if span[2] == 'TESTS')
                last = index
                for i in range(index + 1, len(spans)):
                    if spans[i][2] in keywords:
                        break
                    last = i
                position = start + spans[last][1]
                return text[:position] + insert + text[position:]
            return text[:close] + insert + text[close:]
    return text


def register_module(tx: Transaction, directory: str, depends=()):
    prefix, registration = registration_file(directory)
    raw = tx.read(registration)
    if raw is None:
        raise Conflict(f'Missing module registration file: {registration}')
    source = directory[len(prefix):]
    text = raw.decode('utf-8')
    commands = list(cmake_commands(text))
    if any(command in REGISTRATIONS and args and args[0] == source for _, _, command, args in commands):
        return
    binary = ('engine/' if directory.startswith('engine/') else '') + directory.rsplit('/', 1)[-1]
    registry = any(command in {'crd_resolve_modules', 'crd_add_modules', 'crd_tests'} for _, _, command, _ in commands)
    if registry and prefix:
        line = f'crd_tests({source} {binary})\n'
    elif registry:
        line = f'crd_module({source} {binary}' + (' DEPENDS ' + ' '.join(depends) if depends else '') + ')\n'
    else:
        line = f'add_subdirectory({source} {binary})\n'
    anchors = [start for start, _, command, _ in commands if command in REGISTRY_ANCHORS]
    anchor = min(anchors) if anchors else -1
    if anchor >= 0:
        text = text[:anchor] + line + text[anchor:]
    else:
        text = text.rstrip() + '\n' + line
    tx.write(registration, text.encode('utf-8'))
    if registry and prefix:
        root_raw = tx.read('CMakeLists.txt')
        if root_raw is not None:
            root_text = declare_tests(root_raw.decode('utf-8'), 'engine/' + source, source)
            if root_text != root_raw.decode('utf-8'):
                tx.write('CMakeLists.txt', root_text.encode('utf-8'))


def unregister_module(tx: Transaction, directory: str):
    prefix, registration = registration_file(directory)
    raw = tx.read(registration)
    if raw is None:
        raise Conflict(f'Missing module registration file: {registration}')
    text = raw.decode('utf-8')
    matches = [(start, end) for start, end, command, args in cmake_commands(text)
               if command in REGISTRATIONS and args and args[0] == directory[len(prefix):]]
    if len(matches) != 1:
        raise Conflict(f'{directory} is not registered by one literal add_subdirectory/crd_module/crd_tests; '
                       'edit its CMake definition explicitly')
    start, end = matches[0]
    line_start = text.rfind('\n', 0, start) + 1
    if text[line_start:start].strip() == '':
        start = line_start
    line_end = text.find('\n', end)
    if line_end >= 0 and text[end:line_end].strip() == '':
        end = line_end + 1
    tx.write(registration, (text[:start] + text[end:]).encode('utf-8'))


def migrate_references(tx: Transaction, moves: dict[str, str], known_sources=()):
    ws = tx.ws
    include_map = {}
    for old, new in moves.items():
        path = ws.path(old)
        files = [path] if path.is_file() else [p for p in path.rglob('*') if p.is_file()]
        files += [ws.root / name for name in known_sources if name == old or name.startswith(old + '/')]
        for header in files:
            if header.suffix not in {'.h', '.hpp'}:
                continue
            name = ws.relative(header)
            mapped = remap(name, moves)
            if '/include/' in name and '/include/' in mapped:
                include_map[name.split('/include/', 1)[1]] = mapped.split('/include/', 1)[1]
    for name in source_files(ws):
        if name == MANIFEST:
            continue
        new_name = remap(name, moves)
        raw = tx.read(name)
        if raw is None:
            # A move has scheduled deletion of this old name; migrate its original content at the destination.
            raw = tx.before.get(name)
        if raw is None:
            continue
        try:
            text = raw.decode('utf-8')
        except UnicodeError:
            continue
        original = text
        if name.endswith('CMakeLists.txt') or name.endswith('.cmake'):
            edits = []
            for start, end, value in cmake_tokens(text):
                replacement = relative_replacement(value, name, new_name, moves, ws.root)
                if replacement != value:
                    edits.append((start, end, replacement))
            for start, end, replacement in reversed(edits):
                text = text[:start] + replacement + text[end:]
        else:
            # Full repository-relative and absolute path references in scripts, live docs and asset declarations.
            for old, new in sorted(moves.items(), key=lambda pair: -len(pair[0])):
                text = re.sub(r'(?<![A-Za-z0-9_.-])' + re.escape(old) + r'(?=/|[\s\]"\'`<>):#]|$)',
                              lambda match, new=new: new, text)
                if '\\' in text:
                    old_windows, new_windows = old.replace('/', '\\'), new.replace('/', '\\')
                    text = text.replace(old_windows + '\\', new_windows + '\\')
            if name.endswith('.md'):
                def link(match):
                    value = match[1]
                    angle = value.startswith('<') and value.endswith('>')
                    value = value[1:-1] if angle else value
                    if re.match(r'[A-Za-z]+:', value) or value.startswith('#'):
                        return match[0]
                    parts = re.split(r'(?=[#:])', value, maxsplit=1)
                    mapped = relative_replacement(parts[0], name, new_name, moves, ws.root)
                    mapped += ''.join(parts[1:])
                    return '](' + ('<' + mapped + '>' if angle else mapped) + ')'
                # Resolve against the ORIGINAL text to avoid remapping a just-replaced root-relative spelling.
                text = re.sub(r'\]\(([^)]+)\)', link, text)
        if name.endswith(('.h', '.hpp', '.cpp', '.c')):
            def include(match):
                value = match[2]
                mapped = include_map.get(value)
                if mapped is None and match[1].endswith('"'):
                    mapped = relative_replacement(value, name, new_name, moves, ws.root)
                return match[1] + (mapped or value) + match[3]
            text = re.sub(r'(^\s*#\s*include\s*[<"])([^>"\r\n]+)([>"])', include, text, flags=re.M)
        if text != original:
            tx.write(new_name, text.encode('utf-8'))


class Plan:
    def __init__(self, ws: Workspace, model, reason):
        self.ws = ws
        self.model = model
        self.tx = Transaction(ws, reason)
        self.manifest = copy.deepcopy(load_manifest(ws))
        self.tx.read(MANIFEST)
        self.moves = {}
        self.removed_targets = set()

    def module(self, target: str, directory: str, kind: str, files: dict[str, bytes], dependencies=()):
        directory = portable_name(directory)
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.+-]*', target) or target in self.model['targets']:
            raise Conflict(f'Invalid or existing new target name: {target}')
        parts = directory.split('/')
        if len(parts) != 3 or parts[0] not in {'engine', 'tests'}:
            raise Conflict('New module directory must be engine/<family>/<module> or tests/<family>/<module>')
        if kind not in {'STATIC', 'SHARED', 'EXECUTABLE'}:
            raise Conflict(f'Unsupported module type: {kind}')
        if self.ws.path(directory).exists():
            raise Conflict(f'New module directory already exists: {directory}')
        if not files or not any(name.endswith(('.cpp', '.cxx', '.cc', '.c')) for name in files):
            raise Conflict('A compiled module needs at least one real translation unit')
        for name, data in files.items():
            self.tx.write(directory + '/' + portable_name(name), data)
        function = 'add_executable' if kind == 'EXECUTABLE' else 'add_library'
        option = '' if kind == 'EXECUTABLE' else ' ' + kind
        text = f'{function}({target}{option}\n' + ''.join(f'    "{portable_name(name)}"\n' for name in sorted(files)) + ')\n'
        text += f'target_compile_features({target} PUBLIC cxx_std_20)\n'
        text += f'target_include_directories({target} PUBLIC "${{CMAKE_CURRENT_SOURCE_DIR}}/include"\n'
        text += '    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")\n'
        if dependencies:
            for dependency in dependencies:
                self.target(dependency)
            text += f'target_link_libraries({target} PRIVATE ' + ' '.join(dependencies) + ')\n'
        text += f'if(TARGET crd-warnings)\n    target_link_libraries({target} PRIVATE crd-warnings)\nendif()\n'
        self.tx.write(directory + '/CMakeLists.txt', text.encode('utf-8'))
        modules = sorted({self.model['targets'][d]['source_dir'].rsplit('/', 1)[-1] for d in dependencies
                          if self.model['targets'][d]['source_dir'].startswith('engine/')})
        register_module(self.tx, directory, modules)
        self.manifest['directories'].append('/'.join(parts[:2]))

    def remove_module(self, directory: str):
        members = {name for name, target in self.model['targets'].items() if target['source_dir'] == directory}
        if not members:
            raise Conflict(f'Unknown module: {directory}')
        for name, target in self.model['targets'].items():
            if name not in members and set(target['dependencies']) & members:
                raise Conflict(f'{name} depends on the removed module; reroute/remove that dependency in CMake first')
        unregister_module(self.tx, directory)
        self.manifest['excluded_modules'].append(directory)
        self.removed_targets |= members
        for member in members:
            self.manifest['targets'].pop(member, None)

    def rename_target(self, source: str, destination: str):
        self.target(source)
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.+-]*', destination) or destination in self.model['targets']:
            raise Conflict(f'Invalid/existing target name: {destination}')
        pattern = re.compile(r'(?<![A-Za-z0-9_.+-])' + re.escape(source) + r'(?![A-Za-z0-9_.+-])')
        for name in source_files(self.ws):
            if not name.endswith(('CMakeLists.txt', '.cmake', '.py', '.ps1', '.bat', '.md', '.yml', '.json')) or name == MANIFEST:
                continue
            raw = self.tx.read(name)
            try:
                text = raw.decode('utf-8')
            except (UnicodeError, AttributeError):
                continue
            updated = pattern.sub(destination, text)
            if updated != text:
                self.tx.write(name, updated.encode('utf-8'))
        if source in self.manifest['targets']:
            self.manifest['targets'][destination] = self.manifest['targets'].pop(source)

    def target(self, name):
        if name not in self.model['targets']:
            raise Conflict(f'Unknown source-owned target: {name}')
        return self.model['targets'][name]

    def add(self, target: str, path: str, group=None, *, content=None):
        owner = self.target(target)
        path = portable_name(path)
        self.ws.path(path)
        shared = any(path in existing['sources'] for existing in self.model['targets'].values())
        if not path.startswith(owner['source_dir'] + '/') and not shared:
            raise Conflict(f'New item must be inside {owner["source_dir"]}; shared files require an existing source path')
        if content is not None:
            if self.ws.path(path).exists():
                raise Conflict(f'Add would overwrite an existing item: {path}')
            self.tx.write(path, content)
        elif self.tx.read(path) is None:
            raise Conflict(f'Added item does not exist: {path}')
        membership(self.manifest, target, path, True)
        if group is not None:
            target_settings(self.manifest, target)['groups'][path] = group

    def exclude(self, target: str, path: str, *, delete=False):
        owner = self.target(target)
        path = portable_name(path)
        if not path.startswith(owner['source_dir'] + '/') and path not in owner['sources']:
            raise Conflict(f'Removal must name a file owned by {target}: {path}')
        if delete:
            if self.ws.path(path).is_dir():
                raise Conflict('Directory deletion requires an explicit list of files; use a planned directory operation')
            for other, owner in self.model['targets'].items():
                if path in owner['sources']:
                    membership(self.manifest, other, path, False)
            self.tx.write(path, None)
        else:
            membership(self.manifest, target, path, False)

    def remove_directory(self, target: str, path: str, *, delete=False):
        owner = self.target(target)
        path = portable_name(path)
        if not path.startswith(owner['source_dir'] + '/'):
            raise Conflict('Directory removal must stay inside its owning module; use remove-module for the module')
        directory = self.ws.path(path)
        if not directory.is_dir():
            raise Conflict(f'Directory does not exist: {path}')
        if delete:
            # Enumerate and validate every item first. Application journals bytes before deleting anything.
            for item in sorted(directory.rglob('*')):
                name = self.ws.relative(item)
                self.ws.path(name)
                if item.is_dir():
                    self.tx.remove_directories.add(name)
                else:
                    self.exclude(target, name, delete=True)
            self.tx.remove_directories.add(path)
        else:
            for name in owner['sources']:
                if name.startswith(path + '/'):
                    self.exclude(target, name)
        settings = target_settings(self.manifest, target)
        settings['directories'] = [name for name in settings['directories']
                                   if name != path and not name.startswith(path + '/')]
        settings['groups'] = {name: group for name, group in settings['groups'].items()
                              if not name.startswith(path + '/')}

    def directory(self, target: str, path: str):
        owner = self.target(target)
        path = portable_name(path)
        if not path.startswith(owner['source_dir'] + '/'):
            raise Conflict(f'Directory must belong to {owner["source_dir"]}: {path}')
        self.tx.mkdir(path)
        settings = target_settings(self.manifest, target)
        settings['directories'].append(path)

    def move(self, source: str, destination: str, *, already_moved=False):
        source, destination = portable_name(source), portable_name(destination)
        self.ws.path(source)
        self.ws.path(destination)
        if source in self.moves and self.moves[source] != destination:
            raise Conflict(f'Two destinations requested for {source}')
        if any(source.startswith(old + '/') or old.startswith(source + '/') for old in self.moves):
            raise Conflict(f'Overlapping move operations require one common ancestor: {source}')
        if not already_moved:
            self.tx.move(source, destination)
        elif self.ws.exists_exact(source) or not self.ws.exists_exact(destination):
            raise Conflict(f'Cannot reconcile external rename: {source} -> {destination}')
        self.moves[source] = destination
        self.tx.moves[source] = destination
        for target, owner in self.model['targets'].items():
            for name in owner['sources']:
                mapped = remap(name, {source: destination})
                if mapped != name:
                    membership(self.manifest, target, name, False)
                    membership(self.manifest, target, mapped, True)
            settings = target_settings(self.manifest, target)
            settings['directories'] = [remap(p, {source: destination}) for p in settings['directories']]
            settings['groups'] = {remap(p, {source: destination}): group for p, group in settings['groups'].items()}
        self.manifest['directories'] = [remap(p, {source: destination}) for p in self.manifest['directories']]
        self.manifest['excluded_modules'] = [remap(p, {source: destination}) for p in self.manifest['excluded_modules']]

    def finish(self):
        if self.moves:
            migrate_references(self.tx, self.moves, {name for target in self.model['targets'].values()
                                                    for name in target['sources']})
        # A static/executable target cannot silently become unbuildable by removal of its final translation unit.
        for target, owner in self.model['targets'].items():
            if target in self.removed_targets:
                continue
            settings = self.manifest['targets'].get(target, {})
            sources = (set(owner['sources']) - set(settings.get('remove', []))) | set(settings.get('add', []))
            before_cpp = [p for p in owner['sources'] if p.endswith(('.cpp', '.c', '.cxx', '.cc'))]
            if before_cpp and not any(p.endswith(('.cpp', '.c', '.cxx', '.cc')) for p in sources):
                raise Conflict(f'{target} would have no translation unit; remove/redefine the target explicitly in CMake')
        save_manifest(self.tx, self.manifest)
        self.tx.validate()
        return self.tx
