"""Canonical structure overrides and CMake file-API ownership (no CMake evaluation)."""
from __future__ import annotations

import copy
import json
from pathlib import Path, PurePosixPath
import re

from .storage import Conflict, Transaction, Workspace, atomic_write, digest, json_bytes, portable_name, read_json

MANIFEST = 'cmake/project-structure.json'
KINDS = {'ClCompile', 'ClInclude', 'None', 'ResourceCompile', 'MASM', 'NASM'}


def load_manifest(ws: Workspace):
    value = read_json(ws.path(MANIFEST), {'version': 1, 'directories': [], 'targets': {}})
    if (not isinstance(value, dict) or value.get('version') != 1
            or set(value) - {'version', 'directories', 'targets', 'excluded_modules'}
            or not {'directories', 'targets'}.issubset(value)):
        raise Conflict('Unsupported project-structure manifest; expected schema version 1')
    value.setdefault('excluded_modules', [])
    if (not isinstance(value['targets'], dict) or not isinstance(value['directories'], list)
            or not isinstance(value['excluded_modules'], list)):
        raise Conflict('Invalid structure manifest collections')
    for name in value['directories']:
        ws.path(name)
    for name in value['excluded_modules']:
        ws.path(name)
    for target, settings in value['targets'].items():
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.+-]*', target):
            raise Conflict(f'Invalid CMake target identifier: {target!r}')
        if not isinstance(settings, dict) or set(settings) - {'add', 'remove', 'groups', 'directories'}:
            raise Conflict(f'Unsupported target structure fields: {target}')
        for key in ('add', 'remove', 'directories'):
            if not isinstance(settings.get(key, []), list):
                raise Conflict(f'{target}.{key} must be an array')
            for path in settings.get(key, []):
                ws.path(path)
        if not isinstance(settings.get('groups', {}), dict):
            raise Conflict(f'{target}.groups must be an object')
        for path, group in settings.get('groups', {}).items():
            ws.path(path)
            if group:
                portable_name(group)
    return value


def target_settings(manifest, target):
    settings = manifest['targets'].setdefault(target, {})
    for name in ('add', 'remove', 'directories'):
        settings.setdefault(name, [])
    settings.setdefault('groups', {})
    return settings


def membership(manifest, target, name, present):
    name = portable_name(name)
    settings = target_settings(manifest, target)
    include, exclude = ('add', 'remove') if present else ('remove', 'add')
    settings[exclude] = [path for path in settings[exclude] if path != name]
    if name not in settings[include]:
        settings[include].append(name)
    if not present:
        settings['groups'].pop(name, None)


def save_manifest(tx: Transaction, manifest):
    manifest = copy.deepcopy(manifest)
    manifest['directories'] = sorted(set(manifest['directories']))
    manifest['excluded_modules'] = sorted(set(manifest.get('excluded_modules', [])))
    for name, settings in list(manifest['targets'].items()):
        for key in ('add', 'remove', 'directories'):
            if key in settings:
                settings[key] = sorted(set(settings[key]))
        settings = {key: value for key, value in settings.items() if value}
        if settings:
            manifest['targets'][name] = settings
        else:
            del manifest['targets'][name]
    tx.write(MANIFEST, json_bytes(manifest))


def request_file_api(build: Path):
    query = build / '.cmake/api/v1/query/client-cerid-sync/query.json'
    payload = json_bytes({'requests': [{'kind': 'codemodel', 'version': 2}, {'kind': 'cmakeFiles', 'version': 1}]})
    if not query.exists() or query.read_bytes() != payload:
        atomic_write(query, payload)


def cmake_model(ws: Workspace, build: Path):
    reply = build / '.cmake/api/v1/reply'
    def document(name):
        if not isinstance(name, str) or '/' in name or '\\' in name or portable_name(name) != name:
            raise Conflict('CMake reply reference must name one local JSON file')
        path = reply / name
        ws.path(ws.relative(path))
        return read_json(path)
    indices = sorted(reply.glob('index-*.json'))
    if not indices:
        raise Conflict('CMake file API is unavailable; configure through project-sync first')
    index = read_json(indices[-1])
    objects = {item['kind']: item['jsonFile'] for item in index.get('objects', [])}
    if 'codemodel' not in objects or 'cmakeFiles' not in objects:
        raise Conflict('Missing CMake ownership model; reconfigure to produce the file API reply')
    model = document(objects['codemodel'])
    if Path(model['paths']['source']).resolve() != ws.root or Path(model['paths']['build']).resolve() != build.resolve():
        raise Conflict('CMake ownership model belongs to another checkout/build')
    # Structural membership must be identical across the offered native configurations.
    configurations = model['configurations']
    if not configurations:
        raise Conflict('Empty CMake configuration model')
    targets = {}
    for configuration in configurations:
        current = {}
        for reference in configuration['targets']:
            target = document(reference['jsonFile'])
            source = Path(target['paths']['source'])
            source = source if source.is_absolute() else ws.root / source
            if not source.is_relative_to(ws.root) or source.is_relative_to(ws.root / 'build'):
                continue
            relative = source.relative_to(ws.root).as_posix()
            if relative.split('/')[0] not in {'engine', 'tests', 'sandbox', 'runtime', 'tools'}:
                continue
            if target['type'] not in {'STATIC_LIBRARY', 'SHARED_LIBRARY', 'MODULE_LIBRARY', 'EXECUTABLE', 'OBJECT_LIBRARY'}:
                continue
            binary = Path(target['paths']['build'])
            binary = binary if binary.is_absolute() else build / binary
            sources = {}
            groups = target.get('sourceGroups', [])
            for item in target.get('sources', []):
                path = Path(item['path'])
                path = path if path.is_absolute() else ws.root / path
                if item.get('isGenerated') or not path.is_relative_to(ws.root) or path.is_relative_to(ws.root / 'build'):
                    continue
                name = path.relative_to(ws.root).as_posix()
                ws.path(name)
                group = groups[item['sourceGroupIndex']]['name'] if 'sourceGroupIndex' in item else ''
                sources[name] = group.replace('\\', '/')
            current[target['name']] = {'name': target['name'], 'source_dir': relative, 'type': target['type'],
                                      'project': ws.relative(binary / (target['name'] + '.vcxproj')),
                                      'sources': sources,
                                      'dependencies': sorted({item['id'].split('::', 1)[0] for item in target.get('dependencies', [])})}
        if targets and current != targets:
            raise Conflict('Configuration-specific project membership requires separate build directories')
        targets = current
    files = document(objects['cmakeFiles'])
    inputs = {}
    for item in files['inputs']:
        path = Path(item['path'])
        path = path if path.is_absolute() else ws.root / path
        if (path.is_relative_to(ws.root) and not path.is_relative_to(ws.root / 'build')
                and not item.get('isGenerated') and not item.get('isExternal') and path.is_file()):
            relative = ws.relative(path)
            inputs[relative] = digest(ws.bytes(relative))
    inputs[MANIFEST] = digest(ws.bytes(MANIFEST))
    if (ws.root / 'CMakePresets.json').is_file():
        inputs['CMakePresets.json'] = digest(ws.bytes('CMakePresets.json'))
    return {'targets': targets, 'inputs': inputs, 'index': indices[-1].name}


def emit_cmake(ws: Workspace, destination: Path):
    manifest = load_manifest(ws)
    # Paths reject semicolons, quotes, dollar expansion and traversal before reaching generated CMake text.
    lines = ['# Generated from cmake/project-structure.json; edit the tracked manifest through project-sync.',
             'function(crd_apply_project_structure)']
    for target, settings in sorted(manifest['targets'].items()):
        lines += [f'  if(TARGET "{target}")', f'    get_target_property(_crd_sources "{target}" SOURCES)',
                  f'    get_target_property(_crd_source_dir "{target}" SOURCE_DIR)', '    set(_crd_kept)',
                  '    if(_crd_sources)', '      foreach(_crd_source IN LISTS _crd_sources)',
                  '        if(_crd_source MATCHES "^\\\\$<")', '          list(APPEND _crd_kept "${_crd_source}")',
                  '          continue()', '        endif()',
                  '        get_filename_component(_crd_absolute "${_crd_source}" ABSOLUTE BASE_DIR "${_crd_source_dir}")',
                  '        file(TO_CMAKE_PATH "${_crd_absolute}" _crd_absolute)', '        set(_crd_excluded FALSE)']
        for name in settings.get('remove', []):
            lines += [f'        if(_crd_absolute STREQUAL "${{CMAKE_SOURCE_DIR}}/{name}")',
                      '          set(_crd_excluded TRUE)', '        endif()']
        lines += ['        if(NOT _crd_excluded)', '          list(APPEND _crd_kept "${_crd_source}")',
                  '        endif()', '      endforeach()', '    endif()',
                  f'    set_property(TARGET "{target}" PROPERTY SOURCES "${{_crd_kept}}")']
        for name in settings.get('add', []):
            lines += [f'    target_sources("{target}" PRIVATE "${{CMAKE_SOURCE_DIR}}/{name}")']
        lines += ['  endif()']
    lines += ['endfunction()', '']
    data = '\n'.join(lines).encode('utf-8')
    if not destination.exists() or destination.read_bytes() != data:
        atomic_write(destination, data)


def physical_group(ws: Workspace, target, group: str) -> str:
    group = group.replace('\\', '/').strip('/')
    root = target['source_dir']
    aliases = {'Source Files': 'src', 'Header Files': 'include', 'Resource Files': 'resources'}
    parts = group.split('/') if group else []
    if parts and parts[0] in aliases:
        conventional = aliases[parts.pop(0)]
        if ws.path(root + '/' + conventional).is_dir():
            parts.insert(0, conventional)
    return portable_name('/'.join([root] + parts))
