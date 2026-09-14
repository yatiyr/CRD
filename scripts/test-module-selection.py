#!/usr/bin/env python3
"""Module registry contracts (cmake/CrdModules.cmake) on a small fixture project; no engine build.

Every scenario configures a LANGUAGES NONE project through the real registry mechanism and reads back which
directories were configured, which packages were selected and which declaration errors stopped the configure.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULES = ROOT / 'cmake/CrdModules.cmake'
EXE = '.exe' if os.name == 'nt' else ''


def ninja_program():
    ninja = shutil.which('ninja')
    if not ninja:
        cache = ROOT / 'build/win-debug/CMakeCache.txt'
        if cache.exists():
            match = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$', cache.read_text(encoding='utf-8'), re.M)
            ninja = match[1].strip() if match else None
    return ninja


def module_cmake(name, links=(), guard=None):
    """A fixture module: an INTERFACE library that records its configuration and links the given names."""
    lines = []
    if guard:
        lines += [f'if({guard})', f'    message(STATUS "crd-{name}: skipped")', '    return()', 'endif()']
    lines += [f'file(APPEND "${{CMAKE_BINARY_DIR}}/configured.txt" "{name}\\n")', f'add_library(crd-{name} INTERFACE)']
    for link in links:
        if link.startswith('?'):
            lines += [f'if(TARGET {link[1:]})', f'    target_link_libraries(crd-{name} INTERFACE {link[1:]})', 'endif()']
        else:
            lines.append(f'target_link_libraries(crd-{name} INTERFACE {link})')
    return '\n'.join(lines) + '\n'


def tests_cmake(name, links=()):
    lines = [f'file(APPEND "${{CMAKE_BINARY_DIR}}/configured.txt" "tests/{name}\\n")',
             f'add_library(crd-{name}-tests INTERFACE)']
    for link in links:
        lines.append(f'target_link_libraries(crd-{name}-tests INTERFACE {link})')
    return '\n'.join(lines) + '\n'


ROOT_CMAKE = '''cmake_minimum_required(VERSION 3.25)
project(Selection LANGUAGES NONE)
option(CRD_BUILD_TESTS "" ON)
option(CRD_FIXTURE_NO_SDK "" OFF)
include("{modules}")
crd_module(engine/foundation/core engine/core TESTS foundation/core)
crd_module(engine/foundation/math engine/math DEPENDS core TESTS foundation/math)
crd_module(engine/numerics/fft engine/fft DEPENDS core math PACKAGES zstd TESTS numerics/fft TEST_DEPENDS bench)
crd_module(engine/numerics/bench engine/bench DEPENDS core)
crd_module(engine/gpu/vulkan engine/vulkan DEPENDS core PACKAGES glfw)
crd_module(engine/rendering/render engine/render DEPENDS core vulkan TESTS rendering/render{render_extra})
crd_module(tools/cook HOST EXECUTABLES cook DEPENDS core)
crd_module(sandbox DEPENDS core cook)
{late}crd_resolve_modules()
file(WRITE "${{CMAKE_BINARY_DIR}}/packages.txt" "${{CRD_SELECTED_PACKAGES}}")
crd_add_modules()
if(CRD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
crd_verify_modules()
'''

SANDBOX_CMAKE = '''file(APPEND "${CMAKE_BINARY_DIR}/configured.txt" "sandbox\\n")
add_library(crd-sandbox INTERFACE)
target_link_libraries(crd-sandbox INTERFACE crd-core)
get_target_property(_imported cook IMPORTED)
if(_imported)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/tool.txt" CONTENT "$<TARGET_FILE:cook>")
endif()
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/pack.bin"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/pack.bin"
    DEPENDS cook VERBATIM)
add_custom_target(pack ALL DEPENDS "${CMAKE_BINARY_DIR}/pack.bin")
'''

COOK_CMAKE = '''file(APPEND "${CMAKE_BINARY_DIR}/configured.txt" "cook\\n")
add_custom_target(cook)
'''


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cerid-modules-')
        self.source = Path(self.temp.name) / 'source'
        self.build = Path(self.temp.name) / 'build'
        self.cmake = shutil.which('cmake')
        self.assertIsNotNone(self.cmake, 'CMake is required for the module registry gate')
        self.ninja = ninja_program()
        self.assertTrue(self.ninja and Path(self.ninja).is_file(), 'Ninja is required for the module registry gate')
        self.write('engine/foundation/core/CMakeLists.txt', module_cmake('core'))
        self.write('engine/foundation/math/CMakeLists.txt', module_cmake('math', ['crd-core']))
        self.write('engine/numerics/fft/CMakeLists.txt', module_cmake('fft', ['crd-core', 'crd-math']))
        self.write('engine/numerics/bench/CMakeLists.txt', module_cmake('bench', ['crd-core']))
        self.write('engine/gpu/vulkan/CMakeLists.txt', module_cmake('vulkan', ['crd-core', 'glfw'], guard='CRD_FIXTURE_NO_SDK'))
        self.write('engine/rendering/render/CMakeLists.txt', module_cmake('render', ['crd-core', '?crd-vulkan']))
        self.write('tools/cook/CMakeLists.txt', COOK_CMAKE)
        self.write('sandbox/CMakeLists.txt', SANDBOX_CMAKE)
        self.write('tests/CMakeLists.txt', 'crd_tests(foundation/core core)\ncrd_tests(foundation/math math)\n'
                   'crd_tests(numerics/fft fft)\ncrd_tests(rendering/render render)\n')
        self.write('tests/foundation/core/CMakeLists.txt', tests_cmake('core', ['crd-core']))
        self.write('tests/foundation/math/CMakeLists.txt', tests_cmake('math', ['crd-math']))
        self.write('tests/numerics/fft/CMakeLists.txt', tests_cmake('fft', ['crd-fft', 'crd-bench']))
        self.write('tests/rendering/render/CMakeLists.txt', tests_cmake('render', ['crd-render']))
        self.root(render_extra='', late='')

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, text):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def root(self, render_extra='', late=''):
        self.write('CMakeLists.txt', ROOT_CMAKE.format(modules=MODULES.as_posix(), render_extra=render_extra, late=late))

    def configure(self, *options, fresh=True):
        if fresh and self.build.exists():
            shutil.rmtree(self.build)
        command = [self.cmake, '-S', str(self.source), '-B', str(self.build), '-G', 'Ninja',
                   f'-DCMAKE_MAKE_PROGRAM={self.ninja}', *options]
        return subprocess.run(command, capture_output=True, text=True, timeout=120)

    def configured(self):
        path = self.build / 'configured.txt'
        return set(path.read_text(encoding='utf-8').split()) if path.exists() else set()

    def packages(self):
        return set((self.build / 'packages.txt').read_text(encoding='utf-8').split(';')) - {''}

    def assert_configured(self, result, expected):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.configured(), set(expected))

    def test_full_configuration_adds_everything_and_every_package(self):
        result = self.configure()
        self.assert_configured(result, ['core', 'math', 'fft', 'bench', 'vulkan', 'render', 'cook', 'sandbox',
                                        'tests/core', 'tests/math', 'tests/fft', 'tests/render'])
        self.assertIn('8 of 8 configured (full configuration)', result.stdout)
        self.assertEqual(self.packages(), {'glfw', 'zstd', 'stb', 'cgltf', 'mikktspace', 'imgui'})

    def test_selection_closes_over_dependencies_tests_and_packages(self):
        result = self.configure('-DCRD_MODULES=fft')
        self.assert_configured(result, ['core', 'math', 'fft', 'bench', 'tests/fft'])
        self.assertEqual(self.packages(), {'zstd'})
        self.assertIn('4 of 8 configured', result.stdout)
        self.assertIn('omitted: vulkan render cook sandbox', result.stdout)

    def test_tests_off_drops_test_only_dependencies(self):
        result = self.configure('-DCRD_MODULES=fft', '-DCRD_BUILD_TESTS=OFF')
        self.assert_configured(result, ['core', 'math', 'fft'])

    def test_family_and_dependency_tests_are_not_pulled_for_transitive_modules(self):
        result = self.configure('-DCRD_MODULES=numerics')
        self.assert_configured(result, ['core', 'math', 'fft', 'bench', 'tests/fft'])
        result = self.configure('-DCRD_MODULES=render,host-tools')
        self.assert_configured(result, ['core', 'vulkan', 'render', 'cook', 'tests/render'])
        self.assertEqual(self.packages(), {'glfw'})

    def test_unknown_selection_names_the_known_modules_and_families(self):
        result = self.configure('-DCRD_MODULES=nope')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown module or family 'nope'", result.stderr)
        self.assertIn('Families: foundation, numerics, gpu, rendering, tools, sandbox, host-tools, all', result.stderr)

    def test_undeclared_module_edge_fails_the_full_configure(self):
        self.write('engine/rendering/render/CMakeLists.txt', module_cmake('render', ['crd-core', 'crd-math', '?crd-vulkan']))
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('crd-render (module:render) links crd-math of module math, which crd_module(render) does not declare',
                      result.stderr)
        self.root(render_extra=' TEST_DEPENDS math')
        result = self.configure()
        self.assertNotEqual(result.returncode, 0, 'a test-only declaration must not cover a module link')
        self.write('engine/rendering/render/CMakeLists.txt', module_cmake('render', ['crd-core', '?crd-vulkan']))
        self.write('tests/rendering/render/CMakeLists.txt', tests_cmake('render', ['crd-render', 'crd-math']))
        self.assert_configured(self.configure(), ['core', 'math', 'fft', 'bench', 'vulkan', 'render', 'cook', 'sandbox',
                                                  'tests/core', 'tests/math', 'tests/fft', 'tests/render'])

    def test_unconfigured_link_target_and_undeclared_package_are_configure_errors(self):
        self.write('engine/numerics/fft/CMakeLists.txt', module_cmake('fft', ['crd-core', 'crd-math', 'crd-render']))
        result = self.configure('-DCRD_MODULES=fft')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('links crd-render, which is not a configured target', result.stderr)
        self.write('engine/numerics/fft/CMakeLists.txt', module_cmake('fft', ['crd-core', 'crd-math', 'glfw']))
        result = self.configure('-DCRD_MODULES=fft')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('crd_module(fft) must declare PACKAGES/TEST_PACKAGES glfw', result.stderr)

    def test_conditional_generator_expression_links_are_checked_only_when_present(self):
        self.write('engine/numerics/fft/CMakeLists.txt',
                   module_cmake('fft', ['crd-core', 'crd-math', '$<$<BOOL:OFF>:crd-absent>']))
        self.assert_configured(self.configure('-DCRD_MODULES=fft'), ['core', 'math', 'fft', 'bench', 'tests/fft'])
        self.write('engine/numerics/fft/CMakeLists.txt',
                   module_cmake('fft', ['crd-core', '$<$<BOOL:ON>:crd-math>', '$<$<BOOL:OFF>:crd-render>']))
        self.assert_configured(self.configure('-DCRD_MODULES=fft'), ['core', 'math', 'fft', 'bench', 'tests/fft'])
        self.write('engine/numerics/fft/CMakeLists.txt', module_cmake('fft', ['crd-core', '$<$<BOOL:ON>:crd-bench>']))
        result = self.configure('-DCRD_MODULES=fft')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('links crd-bench of module bench, which crd_module(fft) does not declare', result.stderr)

    def test_unowned_test_registration_fails_in_every_mode(self):
        self.write('tests/orphan/CMakeLists.txt', tests_cmake('orphan'))
        self.write('tests/CMakeLists.txt', 'crd_tests(foundation/core core)\ncrd_tests(orphan)\n')
        for options in ((), ('-DCRD_MODULES=core',)):
            result = self.configure(*options)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('tests/orphan is registered but no crd_module() declares it in TESTS', result.stderr)

    def test_self_skipping_optional_module_is_tolerated_by_guarded_consumers(self):
        result = self.configure('-DCRD_MODULES=render', '-DCRD_FIXTURE_NO_SDK=ON')
        self.assert_configured(result, ['core', 'render', 'tests/render'])
        self.assertIn('crd-vulkan: skipped', result.stdout)

    def test_host_tools_import_replaces_the_tool_module(self):
        tools = Path(self.temp.name) / 'host-tools'
        tools.mkdir()
        result = self.configure('-DCRD_MODULES=sandbox', f'-DCRD_HOST_TOOLS_DIR={tools.as_posix()}')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Host tool 'cook' of module cook is missing", result.stderr)
        (tools / ('cook' + EXE)).write_bytes(b'')
        result = self.configure('-DCRD_MODULES=sandbox', f'-DCRD_HOST_TOOLS_DIR={tools.as_posix()}')
        self.assert_configured(result, ['core', 'sandbox'])
        self.assertEqual((self.build / 'tool.txt').read_text(encoding='utf-8'), (tools / ('cook' + EXE)).as_posix())
        self.assertIn(f'host tool cook imported from {(tools / ("cook" + EXE)).as_posix()}', result.stdout)
        self.assertIn('cook' + EXE, (self.build / 'build.ninja').read_text(encoding='utf-8'))

    def test_registrations_after_resolution_and_duplicates_are_rejected(self):
        self.write('engine/numerics/late/CMakeLists.txt', module_cmake('late'))
        self.write('CMakeLists.txt', (self.source / 'CMakeLists.txt').read_text(encoding='utf-8').replace(
            'crd_resolve_modules()\n', 'crd_resolve_modules()\ncrd_module(engine/numerics/late engine/late)\n'))
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('registered after crd_resolve_modules()', result.stderr)
        self.root(late='crd_module(engine/numerics/bench engine/again)\n')
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("module 'bench' is already registered", result.stderr)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
