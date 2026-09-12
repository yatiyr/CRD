#!/usr/bin/env python3
"""Portable preset contract tests; --generator additionally compiles/runs the eight MSVC profiles."""
from pathlib import Path
import argparse
import copy
import importlib.util
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from native_build_profiles import VENDOR, cmake_text, profiles, resolve

ROOT = Path(__file__).resolve().parents[1]
DOCUMENT = json.loads((ROOT / 'CMakePresets.json').read_text(encoding='utf-8'))
# Independent expected public behaviour: assertions, profiling, debug macro, log threshold, ISA, NDEBUG.
EXPECTED = {
    'Debug': (1, 1, 1, 0, 2, 0), 'Release': (0, 0, 0, 2, 2, 1),
    'RelWithDebInfo': (1, 1, 1, 0, 2, 1), 'ASan': (1, 1, 1, 0, 2, 0),
    'Shipping': (0, 0, 0, 2, 2, 1), 'ShippingProfile': (0, 1, 0, 2, 2, 1),
    'DebugScalar': (1, 1, 1, 0, 0, 0), 'DebugSSE2': (1, 1, 1, 0, 1, 0),
}


class PresetContract(unittest.TestCase):
    def test_synchronizer_legacy_name_routes_to_callable_preset(self):
        spec = importlib.util.spec_from_file_location('project_sync_cli', ROOT / 'scripts/project-sync.py')
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        for command in ('open', 'configure'):
            args = cli.parser().parse_args([command, '--preset', 'win-vs-debug'])
            self.assertEqual(args.preset, 'win-vs')
            preset = next(p for p in DOCUMENT['configurePresets'] if p['name'] == args.preset)
            self.assertFalse(preset.get('hidden'))

    def test_matrix_and_build_test_selectors(self):
        self.assertEqual(list(profiles(DOCUMENT)), list(EXPECTED))
        for section in ('buildPresets', 'testPresets'):
            native = [p for p in DOCUMENT[section] if p['configurePreset'] == 'win-vs']
            self.assertEqual({p['configuration'] for p in native}, set(EXPECTED))
        native = resolve(DOCUMENT, 'win-vs')
        self.assertEqual(native['cacheVariables']['CMAKE_CONFIGURATION_TYPES'].split(';'), list(EXPECTED))
        self.assertEqual(native['cacheVariables'], resolve(DOCUMENT, 'win-vs-debug')['cacheVariables'])
        self.assertEqual(len([p for p in DOCUMENT['configurePresets'] if not p.get('hidden')]), 20)

    def test_inherited_shipping_semantics(self):
        values = profiles(DOCUMENT)
        for config, expected in EXPECTED.items():
            value = values[config]
            self.assertEqual(value['CRD_ENABLE_ASSERTS'] == 'ON', bool(expected[0]))
            self.assertEqual(value['CRD_ENABLE_PROFILING'] == 'ON', bool(expected[1]))
            self.assertEqual(value['CRD_ENABLE_ASAN'] == 'ON', config == 'ASan')
            self.assertEqual(value['CRD_SHIPPING'] == 'ON', config.startswith('Shipping'))
            self.assertEqual(value['CRD_BUILD_BENCHMARKS'] == 'ON', not config.startswith('Shipping'))

    def test_parent_order_and_child_null(self):
        doc = {'configurePresets': [
            {'name': 'one', 'cacheVariables': {'A': 'one', 'B': 'one'}},
            {'name': 'two', 'cacheVariables': {'A': 'two', 'C': 'two'}},
            {'name': 'child', 'inherits': ['one', 'two'], 'cacheVariables': {'B': None}}]}
        self.assertEqual(resolve(doc, 'child')['cacheVariables'], {'A': 'one', 'B': None, 'C': 'two'})

    def test_bad_mapping_refuses_false_configuration(self):
        for preset in ('linux-gcc-debug', 'win-clang-cl', 'win-tidy', 'missing'):
            doc = copy.deepcopy(DOCUMENT)
            doc['vendor'][VENDOR]['Debug'] = preset
            with self.assertRaises(ValueError):
                cmake_text(doc)

    def test_cycles_and_injected_names_fail(self):
        doc = copy.deepcopy(DOCUMENT)
        doc['configurePresets'].append({'name': 'cycle', 'inherits': 'cycle'})
        with self.assertRaisesRegex(ValueError, 'Cyclic'):
            resolve(doc, 'cycle')
        doc['vendor'][VENDOR]['Bad);message(FATAL_ERROR'] = 'win-debug'
        with self.assertRaisesRegex(ValueError, 'Invalid'):
            cmake_text(doc)

    def test_unprojected_profile_difference_is_not_silently_lost(self):
        doc = copy.deepcopy(DOCUMENT)
        preset = next(p for p in doc['configurePresets'] if p['name'] == 'win-release')
        preset['cacheVariables']['CRD_DETERMINISTIC_FP'] = 'OFF'
        with self.assertRaisesRegex(ValueError, 'explicit native projection'):
            profiles(doc)


def compile_profiles(generator):
    cmake = os.environ.get('CRD_CMAKE') or shutil.which('cmake')
    if not cmake:
        raise RuntimeError('CMake is required')
    base = ROOT / 'build/native-profile-tests'
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='matrix-', dir=base) as temporary:
        root = Path(temporary).resolve()
        if not root.is_relative_to(base.resolve()):
            raise RuntimeError('Fixture cleanup escaped its intended build directory')
        (root / 'scripts').mkdir()
        shutil.copyfile(ROOT / 'scripts/native_build_profiles.py', root / 'scripts/native_build_profiles.py')
        shutil.copyfile(ROOT / 'CMakePresets.json', root / 'CMakePresets.json')
        shutil.copyfile(ROOT / 'engine/foundation/core/include/crd/core/build_config.hpp.in', root / 'config.hpp.in')
        for name in ('CrdBuildProfiles.cmake', 'CrdSimd.cmake'):
            shutil.copyfile(ROOT / 'cmake' / name, root / name)
        source = '''#include <crd/core/build_config.hpp>
#if defined(CRD_DEBUG)
#define PROBE_DEBUG 1
#else
#define PROBE_DEBUG 0
#endif
#if defined(NDEBUG)
#define PROBE_NDEBUG 1
#else
#define PROBE_NDEBUG 0
#endif
static_assert(CRD_ENABLE_ASSERTS == EXPECT_ASSERTS);
static_assert(CRD_ENABLE_PROFILING == EXPECT_PROFILE);
static_assert(PROBE_DEBUG == EXPECT_DEBUG);
static_assert(CRD_LOG_MIN_LEVEL_NUM == EXPECT_LOG);
static_assert(CRD_SIMD_TARGET == EXPECT_ISA);
static_assert(PROBE_NDEBUG == EXPECT_NDEBUG);
int main() { return 0; }
'''
        (root / 'probe.cpp').write_text(source, encoding='utf-8')
        (root / 'noipo.cpp').write_text('int noipo() { return 42; }\n', encoding='utf-8')
        # Exercise the actual top-level sanitizer/shipping flags, not a copied test implementation.
        original = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        flags = original[original.index('# ---- Sanitizers'):original.index('# ----- Compiler Warnings')]
        text = '''cmake_minimum_required(VERSION 3.25)
project(ProfileProbe VERSION 0.1.0 LANGUAGES C CXX)
set(CRD_NATIVE_PROFILES ON)
include(CrdBuildProfiles.cmake)
include(CrdSimd.cmake)
set(CRD_LOG_MIN_LEVEL "")
set(CRD_VERSION_MAJOR 0)
set(CRD_VERSION_MINOR 1)
set(CRD_VERSION_PATCH 0)
set(CRD_VERSION 0.1.0)
crd_configure_build_header("${CMAKE_CURRENT_SOURCE_DIR}/config.hpp.in" "${CMAKE_CURRENT_BINARY_DIR}/include")
''' + flags + '''
add_library(noipo STATIC noipo.cpp)
set_property(TARGET noipo PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
add_executable(probe probe.cpp)
target_include_directories(probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/include/$<CONFIG>")
target_link_libraries(probe PRIVATE crd-simd-flags noipo)
target_compile_options(probe PRIVATE /W4 /WX)
crd_apply_native_profiles("${CMAKE_CURRENT_SOURCE_DIR}")
enable_testing()
add_test(NAME profile-probe COMMAND probe)
'''
        for config, expected in EXPECTED.items():
            for key, value in zip(('ASSERTS', 'PROFILE', 'DEBUG', 'LOG', 'ISA', 'NDEBUG'), expected):
                text += f'target_compile_definitions(probe PRIVATE $<$<CONFIG:{config}>:EXPECT_{key}={value}>)\n'
        (root / 'CMakeLists.txt').write_text(text, encoding='utf-8')
        build = root / 'build'
        environment = dict(os.environ)
        def run(*args):
            subprocess.run(list(args), cwd=root, env=environment, check=True, timeout=180)
        run(cmake, '-S', str(root), '-B', str(build), '-G', generator, '-A', 'x64')
        compiler_files = list((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
        if len(compiler_files) != 1:
            raise RuntimeError('Expected one generated compiler descriptor')
        compiler_path = re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)',
                                  compiler_files[0].read_text(encoding='utf-8'))
        if not compiler_path:
            raise RuntimeError('Missing configured MSVC compiler path')
        runtime_directory = Path(compiler_path[1].strip()).parent
        if not (runtime_directory / 'clang_rt.asan_dynamic-x86_64.dll').is_file():
            raise RuntimeError('Install the AddressSanitizer component for the selected MSVC toolset')
        environment['PATH'] = str(runtime_directory) + os.pathsep + environment.get('PATH', '')
        ns = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
        project = ET.parse(build / 'probe.vcxproj')
        noipo = (build / 'noipo.vcxproj').read_text(encoding='utf-8-sig')
        assert '<WholeProgramOptimization>true' not in noipo, 'Target IPO exemption was lost'
        for config in EXPECTED:
            groups = [p for p in project.findall('m:ItemDefinitionGroup', ns)
                      if "'" + config + "|x64'" in p.get('Condition', '')]
            assert len(groups) == 1, (config, 'Missing configuration property group')
            group = groups[0]
            properties = [p for p in project.findall('m:PropertyGroup', ns)
                          if p.get('Label') == 'Configuration' and "'" + config + "|x64'" in p.get('Condition', '')]
            assert len(properties) == 1, (config, 'Missing configuration properties')
            assert (properties[0].findtext('m:EnableAsan', '', ns) == 'true') == (config == 'ASan')
            assert (properties[0].findtext('m:WholeProgramOptimization', '', ns) == 'true') == (
                config in {'Release', 'RelWithDebInfo', 'Shipping', 'ShippingProfile'})
            compiler = group.find('m:ClCompile', ns)
            assert compiler.findtext('m:Optimization', '', ns) == (
                'MaxSpeed' if EXPECTED[config][5] else 'Disabled'), config
            isa = compiler.findtext('m:EnableEnhancedInstructionSet', '', ns)
            assert (isa == 'AdvancedVectorExtensions2') == (EXPECTED[config][4] == 2), (config, isa)
            runtime = compiler.findtext('m:RuntimeLibrary', '', ns)
            assert ('Debug' in runtime) == (EXPECTED[config][5] == 0), (config, runtime)
            if config == 'ASan':
                assert compiler.findtext('m:BasicRuntimeChecks', '', ns) in ('', 'Default'), 'ASan has RTC enabled'
            if config.startswith('Shipping'):
                linker = group.find('m:Link', ns)
                assert linker.findtext('m:OptimizeReferences', '', ns) == 'true', config
                assert linker.findtext('m:EnableCOMDATFolding', '', ns) == 'true', config
            run(cmake, '--build', str(build), '--config', config, '--target', 'probe', '--parallel', '2')
            run(str(Path(cmake).with_name('ctest.exe')), '--test-dir', str(build), '-C', config,
                '-R', '^profile-probe$', '--timeout', '30', '--no-tests=error', '--output-on-failure')
        print('PASS: all eight native profiles compiled, executed and retained target IPO exclusions.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generator', help='Native Visual Studio generator; omit for portable contract tests only')
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(PresetContract))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.generator:
        compile_profiles(args.generator)
