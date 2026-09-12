"""Saved Visual Studio XML adapter. Never executes MSBuild/project content."""
from __future__ import annotations

import copy
import os
from pathlib import Path
import re
import uuid
import xml.etree.ElementTree as ET

from .model import KINDS, MANIFEST, cmake_model, load_manifest, physical_group, target_settings
from .operations import Plan, remap, register_module
from .storage import Conflict, Workspace, atomic_write, digest, json_bytes, portable_name, read_json

NS = 'http://schemas.microsoft.com/developer/msbuild/2003'
ET.register_namespace('', NS)


def xml(path: Path):
    data = path.read_bytes()
    if len(data) > 16 * 1024 * 1024 or re.search(br'<!\s*(DOCTYPE|ENTITY)', data, re.I):
        raise Conflict(f'Unsafe or oversized project XML: {path}')
    try:
        return ET.fromstring(data)
    except ET.ParseError as error:
        raise Conflict(f'Incomplete/invalid saved project XML: {path}: {error}') from error


def tag(element):
    return element.tag.rsplit('}', 1)[-1]


def included_path(ws: Workspace, project: Path, value: str):
    if '$' in value or '%' in value or ';' in value:
        raise Conflict(f'Unresolved MSBuild path expression in structural item: {value}')
    path = Path(value.replace('\\', '/'))
    path = path if path.is_absolute() else project.parent / path
    path = Path(os.path.abspath(path))
    name = ws.relative(path)
    ws.path(name)
    return name


def solution_file(build: Path):
    paths = sorted(build.glob('*.slnx')) or sorted(build.glob('*.sln'))
    if len(paths) != 1:
        raise Conflict('Expected exactly one generated native solution (.slnx or .sln)')
    return paths[0]


def classic_solution(ws: Workspace, path: Path):
    if path.stat().st_size > 16 * 1024 * 1024:
        raise Conflict('Oversized solution file')
    text = path.read_text(encoding='utf-8-sig')
    entries = {}
    for match in re.finditer(r'^Project\("\{([^}]+)\}"\) = "([^"]+)", "([^"]+)", "\{([^}]+)\}"', text, re.M):
        entries[match[4].lower()] = {'type': match[1].lower(), 'name': match[2], 'path': match[3]}
    nesting = re.search(r'GlobalSection\(NestedProjects\).*?\n(.*?)EndGlobalSection', text, re.S)
    parents = {match[1].lower(): match[2].lower() for match in re.finditer(r'\{([^}]+)\}\s*=\s*\{([^}]+)\}',
                                                                                nesting[1] if nesting else '')}
    folder_types = {'2150e333-8fdc-42a3-9474-1a3956d46de8', '66a26720-8fb5-11d2-aa7e-00c04f688dde'}
    def folder(identity, seen=()):
        if identity in seen or identity not in entries:
            raise Conflict('Cyclic/missing solution-folder parent')
        entry = entries[identity]
        if entry['type'] not in folder_types:
            raise Conflict('Solution parent is not a folder')
        parent = folder(parents[identity], seen + (identity,)) + '/' if identity in parents else ''
        return portable_name(parent + entry['name'])
    projects = {}
    folders = []
    for identity, entry in entries.items():
        if entry['type'] in folder_types:
            folders.append(folder(identity))
        else:
            name = included_path(ws, path, entry['path'])
            projects[name] = {'id': identity, 'folder': folder(parents[identity]) if identity in parents else ''}
    return {'path': ws.relative(path), 'projects': projects, 'folders': sorted(folders),
            'folder_ids': {folder(identity): identity for identity, entry in entries.items() if entry['type'] in folder_types}}


def solution(ws: Workspace, build: Path):
    path = solution_file(build)
    if path.suffix == '.sln':
        return classic_solution(ws, path)
    root = xml(path)
    folders, projects = {}, {}
    for folder in root.findall('Folder'):
        name = folder.get('Name', '').strip('/').replace('\\', '/')
        if name:
            portable_name(name)
        folders[name] = True
        for project in folder.findall('Project'):
            project_path = included_path(ws, path, project.get('Path', ''))
            projects[project_path] = {'folder': name, 'id': project.get('Id', '').strip('{}').lower()}
    for project in root.findall('Project'):
        project_path = included_path(ws, path, project.get('Path', ''))
        projects[project_path] = {'folder': '', 'id': project.get('Id', '').strip('{}').lower()}
    return {'path': ws.relative(path), 'projects': projects, 'folders': sorted(folders)}


def project(ws: Workspace, owner):
    path = ws.path(owner['project'])
    root = xml(path)
    configurations = {item.get('Include') for item in root.iter() if tag(item) == 'ProjectConfiguration'}
    filters_path = path.with_suffix('.vcxproj.filters')
    filters_root = xml(filters_path) if filters_path.exists() else ET.Element('Project')
    filters, groups = {}, {}
    for group in filters_root:
        for item in group:
            if tag(item) == 'Filter' and item.get('Include') is not None:
                name = item.get('Include').replace('\\', '/')
                identity = next((child.text for child in item if tag(child) == 'UniqueIdentifier'), None)
                filters[name] = (identity or 'name:' + name).lower()
            elif tag(item) in KINDS and item.get('Include') is not None:
                name = included_path(ws, path, item.get('Include'))
                groups[name] = next((child.text or '' for child in item if tag(child) == 'Filter'), '').replace('\\', '/')
    items, generated, references = {}, {}, []
    identity, label = '', ''
    for item in root.iter():
        item_tag = tag(item)
        if item_tag == 'ProjectGuid':
            identity = (item.text or '').strip('{}').lower()
        elif item_tag == 'ProjectName':
            label = item.text or ''
        elif item_tag == 'ProjectReference' and item.get('Include') is not None:
            references.append(included_path(ws, path, item.get('Include')))
        elif item_tag in KINDS and item.get('Include') is not None:
            name = included_path(ws, path, item.get('Include'))
            entry = {'kind': item_tag, 'group': groups.get(name, '')}
            excluded = [child for child in item if tag(child) == 'ExcludedFromBuild']
            if excluded:
                values = {child.text.strip().lower() for child in excluded if child.text}
                if len(values) > 1:
                    raise Conflict(f'Configuration-specific exclusion is not a portable membership edit: {name}')
                if configurations and not any(not child.get('Condition') for child in excluded):
                    covered = set()
                    for child in excluded:
                        match = re.fullmatch(r"\s*'\$\(Configuration\)\|\$\(Platform\)'\s*==\s*'([^']+)'\s*",
                                             child.get('Condition', ''))
                        if not match or match[1] not in configurations:
                            raise Conflict(f'Unsupported exclusion condition; express it in CMake: {name}')
                        covered.add(match[1])
                    if values == {'true'} and covered != configurations:
                        raise Conflict(f'Configuration-specific exclusion is not a portable membership edit: {name}; '
                                       'select All Configurations, or author the conditional source in CMake')
                entry['excluded'] = values == {'true'}
            if name.startswith('build/') and ('/CMakeFiles/' in name or '/_deps/' in name):
                generated[name] = entry
            elif name in items:
                raise Conflict(f'Duplicate project source item: {name}')
            else:
                items[name] = entry
    return {'items': items, 'generated': generated, 'filters': filters,
            'references': sorted(set(references)), 'guid': identity, 'label': label}


def fingerprint(ws: Workspace, name: str):
    path = ws.path(name)
    if not path.is_file():
        return None
    info = path.stat()
    return [info.st_dev, info.st_ino]


def module_catalog(ws: Workspace):
    result = {}
    for area in ('engine', 'tests'):
        for cmake in (ws.root / area).glob('*/*/CMakeLists.txt'):
            directory = cmake.parent
            name = ws.relative(directory)
            ws.path(name)
            info = directory.stat()
            result[name] = [info.st_dev, info.st_ino]
    return result


def inventory(ws: Workspace, model):
    files, directories = {}, set()
    modules = module_catalog(ws)
    for source_dir in sorted({owner['source_dir'] for owner in model['targets'].values()} | set(modules)):
        base = ws.path(source_dir)
        if not ws.exists_exact(source_dir):
            continue
        for directory, children, names in os.walk(base, followlinks=False):
            children[:] = [child for child in children if child not in {'.vs', '__pycache__', 'CMakeFiles'}]
            name = ws.relative(Path(directory))
            ws.path(name)
            directories.add(name)
            for filename in names:
                path = ws.relative(Path(directory) / filename)
                ws.path(path)
                files[path] = fingerprint(ws, path)
    return {'files': files, 'directories': sorted(directories), 'modules': modules}


def project_projection(ws: Workspace, model):
    """Reapply persisted source groups/empty directories after CMake generates its filters."""
    manifest = load_manifest(ws)
    for name, owner in model['targets'].items():
        settings = manifest['targets'].get(name, {})
        if not settings.get('groups') and not settings.get('directories'):
            continue
        project_path = ws.path(owner['project'])
        filters_path = project_path.with_suffix('.vcxproj.filters')
        root = xml(filters_path) if filters_path.exists() else ET.Element('{' + NS + '}Project', ToolsVersion='4.0')
        available, desired = {}, {}
        for group in root:
            for item in group:
                if tag(item) == 'Filter' and item.get('Include') is not None:
                    available[item.get('Include').replace('\\', '/')] = item
                elif tag(item) in KINDS and item.get('Include') is not None:
                    source = included_path(ws, project_path, item.get('Include'))
                    if source in settings.get('groups', {}):
                        label = settings['groups'][source]
                        child = next((child for child in item if tag(child) == 'Filter'), None)
                        if child is None:
                            child = ET.SubElement(item, '{' + NS + '}Filter')
                        child.text = label.replace('/', '\\')
                        desired[label] = True
        for directory in settings.get('directories', []):
            if directory.startswith(owner['source_dir'] + '/'):
                desired[directory[len(owner['source_dir']) + 1:]] = True
        for label in list(desired):
            parts = label.split('/')
            for index in range(1, len(parts)):
                desired['/'.join(parts[:index])] = True
        additions = [label for label in sorted(desired) if label and label not in available]
        if additions:
            group = ET.SubElement(root, '{' + NS + '}ItemGroup')
            for label in additions:
                item = ET.SubElement(group, '{' + NS + '}Filter', Include=label.replace('/', '\\'))
                identity = uuid.uuid5(uuid.NAMESPACE_URL, 'cerid:' + name + ':' + label)
                ET.SubElement(item, '{' + NS + '}UniqueIdentifier').text = '{' + str(identity).upper() + '}'
        ET.indent(root, space='  ')
        atomic_write(filters_path, ET.tostring(root, encoding='utf-8', xml_declaration=True) + b'\n')


def solution_projection(ws: Workspace, build: Path):
    """CMake omits empty solution folders; materialize tracked physical family navigation after generation."""
    manifest = load_manifest(ws)
    desired = {name for name in manifest['directories'] if len(name.split('/')) == 2
               and name.split('/')[0] in {'engine', 'tests'}}
    desired |= {name.split('/')[0] for name in desired}
    current = solution(ws, build)
    missing = sorted(desired - set(current['folders']))
    if not missing:
        return
    path = ws.path(current['path'])
    if path.suffix == '.slnx':
        root = xml(path)
        for name in missing:
            ET.SubElement(root, 'Folder', Name='/' + name + '/')
        ET.indent(root, space='  ')
        atomic_write(path, ET.tostring(root, encoding='utf-8', xml_declaration=True) + b'\n')
    else:
        text = path.read_text(encoding='utf-8-sig')
        identities = dict(current['folder_ids'])
        entries, nested = [], []
        for name in missing:
            identity = str(uuid.uuid5(uuid.NAMESPACE_URL, 'cerid:solution:' + name)).upper()
            identities[name] = identity
            label = name.rsplit('/', 1)[-1]
            entries.append('Project("{2150E333-8FDC-42A3-9474-1A3956D46DE8}") = '
                           f'"{label}", "{label}", "{{{identity}}}"\nEndProject\n')
            if '/' in name:
                nested.append(f'\t\t{{{identity}}} = {{{identities[name.rsplit("/", 1)[0]]}}}\n')
        text = text.replace('\nGlobal\n', '\n' + ''.join(entries) + 'Global\n', 1)
        section = re.search(r'GlobalSection\(NestedProjects\).*?\n', text)
        if section:
            text = text[:section.end()] + ''.join(nested) + text[section.end():]
        elif nested:
            text = text.replace('\nEndGlobal', '\n\tGlobalSection(NestedProjects) = preSolution\n'
                                + ''.join(nested) + '\tEndGlobalSection\nEndGlobal', 1)
        atomic_write(path, text.encode('utf-8-sig'))


def capture(ws: Workspace, build: Path):
    model = cmake_model(ws, build)
    project_projection(ws, model)
    solution_projection(ws, build)
    view = solution(ws, build)
    snapshots = {}
    for name, owner in model['targets'].items():
        if owner['project'] not in view['projects']:
            raise Conflict(f'Generated solution is missing {name}')
        snapshot = project(ws, owner)
        # CMake model must really describe the generated project, not a stale/half-written generation.
        missing = set(owner['sources']) - set(snapshot['items'])
        if missing:
            raise Conflict(f'Generated project/model disagree for {name}: {sorted(missing)[:5]}')
        snapshot['folder'] = view['projects'][owner['project']]['folder']
        snapshots[name] = snapshot
    identities = {path: fingerprint(ws, path) for target in model['targets'].values() for path in target['sources']}
    return {'version': 1, 'root': str(ws.root), 'build': str(build), 'model': model, 'solution': view,
            'projects': snapshots, 'identities': identities, 'filesystem': inventory(ws, model)}


def source_plan(ws: Workspace, baseline):
    """Reconcile external filesystem-only edits when CMake has not already supplied their intent."""
    if 'filesystem' not in baseline:
        return None
    for path, expected in baseline['model']['inputs'].items():
        if digest(ws.bytes(path)) != expected:
            return None  # Explicit CMake edits own their source selection; reconfigure to observe that selection.
    model = baseline['model']
    before = baseline['filesystem']
    current = inventory(ws, model)
    removed = set(before['files']) - set(current['files'])
    added = set(current['files']) - set(before['files'])
    new_directories = set(current['directories']) - set(before['directories'])
    if not removed and not added and not new_directories:
        return None
    plan = Plan(ws, model, 'Reconcile source filesystem structure edits')
    matched_old, matched_new = set(), set()
    old_modules, new_modules = before.get('modules', {}), current.get('modules', {})
    missing_modules = set(old_modules) - set(new_modules)
    added_modules = set(new_modules) - set(old_modules)
    for destination in sorted(added_modules):
        matches = [source for source in missing_modules if old_modules[source] == new_modules[destination]]
        if len(matches) > 1:
            raise Conflict(f'Ambiguous module rename: {destination}; select the source with edit/move')
        if matches:
            source = matches[0]
            plan.move(source, destination, already_moved=True)
            missing_modules.remove(source)
            matched_old.update(path for path in removed if path.startswith(source + '/'))
            matched_new.update(path for path in added if path.startswith(destination + '/'))
        else:
            register_module(plan.tx, destination)
            matched_new.update(path for path in added if path.startswith(destination + '/'))
        plan.manifest['directories'].append(destination.rsplit('/', 1)[0])
    for source in missing_modules:
        if any(owner['source_dir'] == source for owner in model['targets'].values()):
            plan.remove_module(source)
            matched_old.update(path for path in removed if path.startswith(source + '/'))
    for destination in added:
        if destination in matched_new:
            continue
        identity = current['files'][destination]
        matches = [source for source in removed - matched_old if before['files'][source] == identity]
        if len(matches) > 1:
            raise Conflict(f'Ambiguous source rename: {destination}; use an explicit operation')
        if matches:
            plan.move(matches[0], destination, already_moved=True)
            matched_old.add(matches[0])
            matched_new.add(destination)
    for source in removed - matched_old:
        for target, owner in model['targets'].items():
            if source in owner['sources']:
                plan.exclude(target, source, delete=True)
    for source in added - matched_new:
        if not source.endswith(('.cpp', '.c', '.cxx', '.cc', '.hpp', '.h', '.rc', '.natvis', '.asm', '.s')):
            continue
        owners = [target for target, owner in model['targets'].items() if source.startswith(owner['source_dir'] + '/')]
        if len(owners) > 1:
            raise Conflict(f'New source {source} has multiple candidate targets {owners}; select one with edit/add')
        if owners:
            owner = model['targets'][owners[0]]
            group = str(Path(source).parent.relative_to(owner['source_dir'])).replace('\\', '/')
            plan.add(owners[0], source, '' if group == '.' else group)
    for directory in new_directories:
        if any(directory == name or directory.startswith(name + '/') for name in added_modules):
            continue
        owners = [target for target, owner in model['targets'].items() if directory.startswith(owner['source_dir'] + '/')]
        if len(owners) == 1:
            plan.directory(owners[0], directory)
    return plan.finish()


def import_project(plan: Plan, project_path: str, folder: str):
    """Adopt a plain native C++ source project as an explicit portable CMake module."""
    path = plan.ws.path(project_path)
    parts = folder.split('/')
    if len(parts) != 2 or parts[0] not in {'engine', 'tests'}:
        raise Conflict('Place a new C++ project in engine/<family> or tests/<family> before importing it')
    root = xml(path)
    types = {node.text for node in root.iter() if tag(node) == 'ConfigurationType'}
    kinds = {'StaticLibrary': 'STATIC', 'DynamicLibrary': 'SHARED', 'Application': 'EXECUTABLE'}
    if len(types) != 1 or next(iter(types)) not in kinds:
        raise Conflict('New project must consistently be a C++ static/shared library or executable')
    for node in root.iter():
        if tag(node) in {'CustomBuild', 'PreBuildEvent', 'PostBuildEvent', 'PreLinkEvent'}:
            raise Conflict('Foreign custom build steps must be authored explicitly in CMake before importing this project')
        if tag(node) in {'CLRSupport', 'UseOfMfc', 'UseOfAtl'} and node.text not in {None, '', 'false', 'None'}:
            raise Conflict('CLR/MFC/ATL project semantics need an explicit platform-specific CMake contract')
        if tag(node) in {'AdditionalIncludeDirectories', 'AdditionalLibraryDirectories', 'AdditionalDependencies'}:
            if node.text and node.text.strip() not in {'%(' + tag(node) + ')', ''}:
                raise Conflict('New project has custom includes/libraries; declare them in CMake rather than dropping them')
    target = path.stem
    module_name = target[4:] if target.startswith('crd-') else target
    directory = folder + '/' + module_name
    owner = {'project': project_path, 'source_dir': directory}
    view = project(plan.ws, owner)
    files, originals = {}, {}
    for source, item in view['items'].items():
        absolute = plan.ws.path(source)
        if not absolute.is_relative_to(path.parent):
            raise Conflict(f'New project source is shared/foreign; import its target through CMake explicitly: {source}')
        relative = absolute.relative_to(path.parent).as_posix()
        if '/' not in relative:
            group = item['group']
            if group == 'Source Files':
                relative = 'src/' + relative
            elif group == 'Header Files':
                relative = 'include/' + relative
            elif group:
                relative = portable_name(group) + '/' + relative
        if relative in files:
            raise Conflict(f'New project item collision: {relative}')
        data = plan.tx.read(source)
        if data is None:
            raise Conflict(f'New project source has not been saved: {source}')
        files[relative] = data
        originals[source] = directory + '/' + relative
    project_targets = {owner['project']: name for name, owner in plan.model['targets'].items()}
    if any(reference not in project_targets for reference in view['references']):
        raise Conflict('New project references a foreign/new target; register dependencies through CMake first')
    plan.module(target, directory, kinds[next(iter(types))], files,
                [project_targets[reference] for reference in view['references']])
    for source, destination in originals.items():
        if source.startswith('build/'):
            plan.tx.write(source, None)
            plan.tx.moves[source] = destination


def ide_plan(ws: Workspace, baseline):
    build = Path(baseline['build'])
    current_solution = solution(ws, build)
    expected_projects = set(baseline['solution']['projects'])
    added_projects = set(current_solution['projects']) - expected_projects
    removed_projects = expected_projects - set(current_solution['projects'])
    relocations = {}
    for destination in list(added_projects):
        identity = current_solution['projects'][destination]['id']
        matches = [source for source in removed_projects if identity
                   and baseline['solution']['projects'][source]['id'] == identity]
        if len(matches) == 1:
            relocations[matches[0]] = destination
            removed_projects.remove(matches[0])
            added_projects.remove(destination)
    owners = copy.deepcopy(baseline['model']['targets'])
    by_project = {owner['project']: name for name, owner in owners.items()}
    if any(path not in by_project for path in removed_projects | set(relocations)):
        raise Conflict('Generated utility/vendor projects are protected; restore their solution entries')
    removed_targets = {by_project[path] for path in removed_projects}
    for name in removed_targets:
        del owners[name]
    for old, new in relocations.items():
        owners[by_project[old]]['project'] = new
    current_projects = {name: project(ws, owner) for name, owner in owners.items()}
    # Compare normalized structures, not XML whitespace, UUID capitalization or source content.
    changed = current_solution != baseline['solution'] or any(
        current_projects[name] != {key: value for key, value in old.items() if key != 'folder'}
        for name, old in baseline['projects'].items() if name in current_projects)
    if not changed:
        return None
    for path, expected in baseline['model']['inputs'].items():
        if digest(ws.bytes(path)) != expected:
            raise Conflict(f'Both CMake/manifest and IDE structure changed since the common generation: {path}. '
                           'Save a plan and reconcile before regenerating')
    plan = Plan(ws, baseline['model'], 'Import saved Visual Studio structure edits')
    removed_modules = {baseline['model']['targets'][name]['source_dir'] for name in removed_targets}
    for directory in removed_modules:
        remaining = [name for name, owner in owners.items() if owner['source_dir'] == directory]
        if remaining:
            raise Conflict(f'{directory} defines multiple targets; remove them together or edit the CMake target definition')
        plan.remove_module(directory)
    for new in added_projects:
        import_project(plan, new, current_solution['projects'][new]['folder'])
    # A whole-family folder rename moves every physical child, including targets disabled in this preset.
    family_moves = {}
    removed_folders = set(baseline['solution']['folders']) - set(current_solution['folders'])
    new_folders = set(current_solution['folders']) - set(baseline['solution']['folders'])
    for old_folder in removed_folders:
        if len(old_folder.split('/')) != 2 or old_folder.split('/')[0] not in {'engine', 'tests'}:
            continue
        old_members = {path for path, data in baseline['solution']['projects'].items() if data['folder'] == old_folder}
        matches = [folder for folder in new_folders if folder.split('/')[0] == old_folder.split('/')[0]
                   and len(folder.split('/')) == 2 and old_members
                   and {path for path, data in current_solution['projects'].items() if data['folder'] == folder}
                   == {relocations.get(path, path) for path in old_members}]
        if len(matches) == 1 and not ws.path(matches[0]).exists():
            family_moves[old_folder] = matches[0]
    folder_moves, target_renames = {}, {}
    for name, owner in owners.items():
        old = baseline['projects'][name]
        current = current_projects[name]
        if current['generated'] != old['generated']:
            raise Conflict(f'Compiler-generated source entries were edited in {name}; restore those entries')
        if current['references'] != sorted(relocations.get(path, path) for path in old['references']):
            raise Conflict(f'Project references changed in {name}; express dependency/public visibility in CMake')
        if current['guid'] != old['guid']:
            raise Conflict(f'Target GUID changed in {name}; this is not an unambiguous project rename')
        new_name = Path(owner['project']).stem
        if current['label'] != old['label'] and current['label']:
            new_name = current['label']
        if new_name != name:
            target_renames[name] = new_name
        folder = current_solution['projects'][owner['project']]['folder']
        if folder != old['folder']:
            parts = folder.split('/')
            source = owner['source_dir']
            if len(parts) != 2 or parts[0] not in {'engine', 'tests'} or source.split('/')[0] != parts[0]:
                raise Conflict(f'Module placement must be {source.split("/")[0]}/<family>: {folder}')
            destination = folder + '/' + source.rsplit('/', 1)[-1]
            if source in folder_moves and folder_moves[source] != destination:
                raise Conflict(f'Targets sharing {source} were moved to different families')
            folder_moves[source] = destination
        old_filters = {identity: label for label, identity in old['filters'].items()}
        for label, identity in current['filters'].items():
            if label == old_filters.get(identity):
                continue
            directory = physical_group(ws, owner, label)
            if identity in old_filters:
                previous = physical_group(ws, owner, old_filters[identity])
                if previous != directory and previous != owner['source_dir']:
                    plan.move(previous, directory)
            elif label not in old['filters']:
                plan.directory(name, directory)
        old_items, new_items = old['items'], current['items']
        removed_filters = set(old['filters']) - set(current['filters'])
        settings = target_settings(plan.manifest, name)
        removed_directories = {physical_group(ws, owner, label) for label in removed_filters}
        settings['directories'] = [path for path in settings['directories'] if path not in removed_directories]
        removed = set(old_items) - set(new_items)
        added = set(new_items) - set(old_items)
        renamed_old, renamed_new = set(), set()
        for destination in added:
            identity = fingerprint(ws, destination)
            matches = [source for source in removed if identity is not None
                       and baseline['identities'].get(source) == identity and not ws.exists_exact(source)]
            if len(matches) > 1:
                raise Conflict(f'Ambiguous physical rename into {destination}; use an explicit move plan')
            if matches:
                source = matches[0]
                plan.move(source, destination, already_moved=True)
                renamed_old.add(source)
                renamed_new.add(destination)
        for source in removed - renamed_old:
            plan.exclude(name, source, delete=not ws.path(source).exists())
        for source, item in new_items.items():
            if source in renamed_new:
                continue
            if item.get('excluded') and not old_items.get(source, {}).get('excluded'):
                plan.exclude(name, source)
                continue
            if old_items.get(source, {}).get('excluded') and not item.get('excluded'):
                plan.add(name, source, item['group'])
            if source in old_items and item['kind'] != old_items[source]['kind']:
                raise Conflict(f'Item build type changed for {source}; declare its language/action in CMake')
            label = item['group']
            if source in added:
                destination = source
                if source.startswith('build/'):
                    generated_dir = str(Path(owner['project']).parent).replace('\\', '/')
                    if not source.startswith(generated_dir + '/'):
                        raise Conflict(f'New build-tree item is outside its project directory: {source}')
                    destination = physical_group(ws, owner, label) + '/' + Path(source).name
                    plan.tx.move(source, destination)
                    # The file will be created by the move transaction; register membership directly.
                    from .model import membership
                    membership(plan.manifest, name, destination, True)
                    target_settings(plan.manifest, name)['groups'][destination] = label
                else:
                    plan.add(name, destination, label)
            elif label != old_items[source]['group']:
                old_directory = physical_group(ws, owner, old_items[source]['group'])
                new_directory = physical_group(ws, owner, label)
                destination = new_directory + '/' + Path(source).name
                dissolved = old_items[source]['group'] in removed_filters and not label
                if not dissolved and old_directory != new_directory and source.startswith(owner['source_dir'] + '/'):
                    if not any(source == old_path or source.startswith(old_path + '/') for old_path in plan.moves):
                        plan.move(source, destination)
                target_settings(plan.manifest, name)['groups'][remap(source, plan.moves)] = label
    for source, destination in folder_moves.items():
        for name, owner in owners.items():
            if owner['source_dir'] == source:
                folder = current_solution['projects'][owner['project']]['folder']
                if folder + '/' + source.rsplit('/', 1)[-1] != destination:
                    raise Conflict(f'Move all targets sharing the module directory together: {source}')
        if not any(source.startswith(old + '/') and remap(source, {old: new}) == destination
                   for old, new in family_moves.items()):
            plan.move(source, destination)
    for source, destination in family_moves.items():
        plan.move(source, destination)
    for folder in set(current_solution['folders']) - set(baseline['solution']['folders']):
        parts = folder.split('/')
        if len(parts) == 2 and parts[0] in {'engine', 'tests'}:
            plan.tx.mkdir(folder)
            plan.manifest['directories'].append(folder)
    plan.manifest['directories'] = [name for name in plan.manifest['directories'] if name not in removed_folders]
    for source, destination in target_renames.items():
        plan.rename_target(source, destination)
    return plan.finish()
