"""Project the canonical MSVC Ninja presets into native multi-config CMake profiles."""
from pathlib import Path
import argparse
import json
import re

VENDOR = 'cerid.dev/NativeProfiles/1.0'
DEFAULTS = {
    'CRD_ENABLE_ASSERTS': 'ON', 'CRD_ENABLE_PROFILING': 'OFF',
    'CRD_ENABLE_ASAN': 'OFF', 'CRD_ENABLE_UBSAN': 'OFF', 'CRD_SHIPPING': 'OFF',
    'CMAKE_INTERPROCEDURAL_OPTIMIZATION': 'OFF', 'CRD_BUILD_BENCHMARKS': 'ON',
    'CRD_SIMD_LEVEL': 'auto', 'CRD_ENABLE_CLANG_TIDY': 'OFF',
}


def resolve(document, name, seen=()):
    if name in seen:
        raise ValueError('Cyclic preset inheritance: ' + name)
    presets = {preset['name']: preset for preset in document['configurePresets']}
    if name not in presets:
        raise ValueError('Missing preset: ' + name)
    preset = presets[name]
    parents = preset.get('inherits', [])
    parents = [parents] if isinstance(parents, str) else parents
    result = {'cacheVariables': {}}
    # CMake: the first parent wins, then the child's fields override its parents.
    for parent in reversed(parents):
        inherited = resolve(document, parent, seen + (name,))
        result['cacheVariables'].update(inherited['cacheVariables'])
        for key in ('generator', 'architecture'):
            if key in inherited:
                result[key] = inherited[key]
    result['cacheVariables'].update(preset.get('cacheVariables', {}))
    for key in ('generator', 'architecture'):
        if key in preset:
            result[key] = preset[key]
    return result


def profiles(document):
    mapping = document['vendor'][VENDOR]
    if not mapping or len({name.lower() for name in mapping}) != len(mapping):
        raise ValueError('Empty or case-colliding native configurations')
    result = {}
    common = None
    for config, preset in mapping.items():
        if not re.fullmatch(r'[A-Za-z][A-Za-z0-9]*', config):
            raise ValueError('Invalid native configuration: ' + config)
        resolved = resolve(document, preset)
        values = DEFAULTS | resolved['cacheVariables']
        invariant = {key: value for key, value in values.items()
                     if key not in DEFAULTS and key != 'CMAKE_BUILD_TYPE' and not key.startswith('_NOTE')}
        if common is None:
            common = invariant
        elif invariant != common:
            changed = sorted(key for key in invariant.keys() | common.keys() if invariant.get(key) != common.get(key))
            raise ValueError('Per-profile settings need an explicit native projection: ' + ', '.join(changed))
        if (resolved.get('generator') != 'Ninja' or values.get('CMAKE_CXX_COMPILER') != 'cl'
                or values['CRD_ENABLE_CLANG_TIDY'] != 'OFF' or values['CRD_ENABLE_UBSAN'] != 'OFF'):
            raise ValueError('Native profiles require MSVC presets without Ninja-only analysis: ' + preset)
        if values.get('CMAKE_BUILD_TYPE') not in {'Debug', 'Release', 'RelWithDebInfo'}:
            raise ValueError('Unsupported base configuration: ' + preset)
        if values['CRD_SIMD_LEVEL'] not in {'auto', 'scalar', 'sse2'}:
            raise ValueError('Unsupported native SIMD projection: ' + preset)
        values = {key: values[key] for key in (*DEFAULTS, 'CMAKE_BUILD_TYPE')}
        for key, value in values.items():
            if key not in {'CRD_SIMD_LEVEL', 'CMAKE_BUILD_TYPE'} and value not in {'ON', 'OFF'}:
                raise ValueError('Expected explicit ON/OFF for ' + key + ' in ' + preset)
        if values['CRD_ENABLE_ASAN'] == 'ON' and values['CMAKE_INTERPROCEDURAL_OPTIMIZATION'] == 'ON':
            raise ValueError('MSVC ASan and IPO must use separate profiles')
        result[config] = values
    return result


def cmake_text(document):
    values = profiles(document)
    lines = ['# Generated from CMakePresets.json. Do not edit.',
             'set(CRD_NATIVE_CONFIGS "' + ';'.join(values) + '")']
    for config, fields in values.items():
        for key, value in fields.items():
            lines.append(f'set(CRD_PROFILE_{config}_{key} "{value}")')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('presets', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    content = cmake_text(json.loads(args.presets.read_text(encoding='utf-8')))
    if not args.output.exists() or args.output.read_text(encoding='utf-8') != content:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(content, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
