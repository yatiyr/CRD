#!/usr/bin/env python3
"""Adversarial project-structure tests; no live engine files or IDE are mutated."""
from pathlib import Path
import copy
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

from project_sync.model import MANIFEST, emit_cmake, json_bytes, load_manifest
from project_sync.operations import Plan, cmake_commands
from project_sync.storage import Conflict, Transaction, Workspace, atomic_write, incomplete, read_json, recover
from project_sync.visual_studio import NS, ide_plan, inventory, project, solution, source_plan


class Fixture(unittest.TestCase):
    def test_partial_native_exclusion_cannot_remove_all_configurations(self):
        self.native()
        owner = self.model['targets']['dense']
        path = self.root / owner['project']
        tree = ET.parse(path).getroot()
        ns = '{' + NS + '}'
        group = ET.SubElement(tree, ns + 'ItemGroup')
        for config in ('Debug', 'Release'):
            ET.SubElement(group, ns + 'ProjectConfiguration', Include=config + '|x64')
        item = next(p for p in tree.iter(ns + 'ClCompile'))
        ET.SubElement(item, ns + 'ExcludedFromBuild',
                      Condition="'$(Configuration)|$(Platform)'=='Debug|x64'").text = 'true'
        self.write(owner['project'], ET.tostring(tree))
        with self.assertRaisesRegex(Conflict, 'All Configurations'):
            project(self.ws, owner)
        ET.SubElement(item, ns + 'ExcludedFromBuild',
                      Condition="'$(Configuration)|$(Platform)'=='Release|x64'").text = 'true'
        self.write(owner['project'], ET.tostring(tree))
        self.assertTrue(any(value.get('excluded') for value in project(self.ws, owner)['items'].values()))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cerid-sync-')
        self.root = Path(self.temp.name).resolve()
        self.build = self.root / 'build/native'
        self.build.mkdir(parents=True)
        self.ws = Workspace(self.root, [self.build])
        self.name = 'engine/numerics/dense'
        self.write(self.name + '/src/main.cpp', 'int answer() { return 42; }\n')
        self.write(self.name + '/src/other.cpp', 'int other() { return 7; }\n')
        self.write(self.name + '/include/crd/dense/value.hpp', '#pragma once\n')
        self.write(MANIFEST, json_bytes({'version': 1, 'directories': [], 'targets': {}}))
        self.model = {'targets': {'dense': {'name': 'dense', 'source_dir': self.name, 'type': 'STATIC_LIBRARY',
                      'project': 'build/native/engine/dense/dense.vcxproj',
                      'sources': {self.name + '/src/main.cpp': 'src', self.name + '/src/other.cpp': 'src',
                                  self.name + '/include/crd/dense/value.hpp': 'include/crd/dense'},
                      'dependencies': []}}, 'inputs': {}}

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode('utf-8') if isinstance(text, str) else text)

    def run_plan(self, plan):
        with self.ws.lock():
            return plan.finish().apply()

    def native(self):
        owner = self.model['targets']['dense']
        project_root = ET.Element('{' + NS + '}Project')
        props = ET.SubElement(project_root, '{' + NS + '}PropertyGroup')
        ET.SubElement(props, '{' + NS + '}ProjectGuid').text = '{aaaa}'
        items = ET.SubElement(project_root, '{' + NS + '}ItemGroup')
        filters_root = ET.Element('{' + NS + '}Project')
        filters = ET.SubElement(filters_root, '{' + NS + '}ItemGroup')
        for index, group in enumerate(sorted(set(owner['sources'].values()))):
            item = ET.SubElement(filters, '{' + NS + '}Filter', Include=group.replace('/', '\\'))
            ET.SubElement(item, '{' + NS + '}UniqueIdentifier').text = '{filter-' + str(index) + '}'
        filter_items = ET.SubElement(filters_root, '{' + NS + '}ItemGroup')
        for name, group in owner['sources'].items():
            kind = 'ClCompile' if name.endswith('.cpp') else 'ClInclude'
            ET.SubElement(items, '{' + NS + '}' + kind, Include=str(self.root / name))
            item = ET.SubElement(filter_items, '{' + NS + '}' + kind, Include=str(self.root / name))
            ET.SubElement(item, '{' + NS + '}Filter').text = group.replace('/', '\\')
        self.write(owner['project'], ET.tostring(project_root, encoding='utf-8'))
        self.write(owner['project'] + '.filters', ET.tostring(filters_root, encoding='utf-8'))
        sln = ET.Element('Solution')
        folder = ET.SubElement(sln, 'Folder', Name='/engine/numerics/')
        ET.SubElement(folder, 'Project', Path='engine/dense/dense.vcxproj', Id='aaaa')
        self.write('build/native/Fixture.slnx', ET.tostring(sln, encoding='utf-8'))
        from project_sync.visual_studio import fingerprint
        snapshot = project(self.ws, owner)
        snapshot['folder'] = 'engine/numerics'
        return {'version': 1, 'root': str(self.root), 'build': str(self.build), 'model': copy.deepcopy(self.model),
                'solution': solution(self.ws, self.build), 'projects': {'dense': snapshot},
                'identities': {name: fingerprint(self.ws, name) for name in owner['sources']}}


class Transactions(Fixture):
    def test_empty_directory_delete_and_recovery(self):
        name = self.name + '/empty'
        (self.root / name).mkdir()
        plan = Plan(self.ws, self.model, 'delete directory')
        plan.remove_directory('dense', name, delete=True)
        identity = self.run_plan(plan)
        self.assertFalse((self.root / name).exists())
        recover(self.ws, identity)
        self.assertTrue((self.root / name).is_dir())

    def test_directory_delete_preserves_recovery_bytes(self):
        directory = self.name + '/include'
        plan = Plan(self.ws, self.model, 'delete include')
        plan.remove_directory('dense', directory, delete=True)
        identity = self.run_plan(plan)
        self.assertFalse((self.root / directory).exists())
        recover(self.ws, identity)
        self.assertEqual(self.ws.bytes(directory + '/crd/dense/value.hpp'), b'#pragma once\n')

    def test_remove_does_not_accept_foreign_owned_path(self):
        plan = Plan(self.ws, self.model, 'foreign')
        with self.assertRaisesRegex(Conflict, 'owned'):
            plan.exclude('dense', 'AGENTS.md', delete=True)
    def test_paths_reject_escapes_and_windows_ambiguities(self):
        for name in ('../outside', '/outside', 'C:/outside', 'engine/a/../b', 'engine/NUL.cpp',
                     'engine/file.', 'engine/file ', 'engine/a;b', '.git/config', 'engine/$HOME.cpp'):
            with self.subTest(name=name), self.assertRaises(Conflict):
                self.ws.path(name)

    def test_case_collision_rejected_before_write(self):
        tx = Transaction(self.ws, 'case')
        tx.write(self.name + '/src/MAIN.cpp', b'wrong')
        with self.assertRaisesRegex(Conflict, 'case collision'):
            tx.apply()
        self.assertEqual((self.root / self.name / 'src/main.cpp').read_text(), 'int answer() { return 42; }\n')

    def test_case_only_file_rename(self):
        tx = Transaction(self.ws, 'case rename')
        tx.move(self.name + '/src/main.cpp', self.name + '/src/Main.cpp')
        identity = tx.apply()
        self.assertIn('Main.cpp', os.listdir(self.root / self.name / 'src'))
        recover(self.ws, identity)
        self.assertIn('main.cpp', os.listdir(self.root / self.name / 'src'))

    def test_case_only_directory_rename(self):
        tx = Transaction(self.ws, 'case directory')
        tx.move(self.name + '/src', self.name + '/Src')
        identity = tx.apply()
        self.assertIn('Src', os.listdir(self.root / self.name))
        self.assertEqual(self.ws.bytes(self.name + '/Src/main.cpp'), b'int answer() { return 42; }\n')
        recover(self.ws, identity)
        self.assertIn('src', os.listdir(self.root / self.name))

    def test_conflicting_writer_does_not_overwrite(self):
        name = self.name + '/src/main.cpp'
        tx = Transaction(self.ws, 'conflict')
        tx.write(name, b'sync edit')
        self.write(name, 'human edit')
        with self.assertRaisesRegex(Conflict, 'Concurrent edit'):
            tx.apply()
        self.assertEqual((self.root / name).read_text(), 'human edit')

    def test_interrupted_move_is_recoverable(self):
        old, new = self.name + '/src/main.cpp', self.name + '/src/moved.cpp'
        tx = Transaction(self.ws, 'interrupt')
        tx.move(old, new)
        with self.assertRaisesRegex(Conflict, 'Injected interruption'):
            tx.apply(fail_after=1)
        identity = incomplete(self.ws)[0]
        recover(self.ws, identity)
        self.assertTrue((self.root / old).is_file())
        self.assertFalse((self.root / new).exists())
        self.assertEqual(incomplete(self.ws), [])

    def test_recovery_refuses_later_edit(self):
        old, new = self.name + '/src/main.cpp', self.name + '/src/moved.cpp'
        tx = Transaction(self.ws, 'move')
        tx.move(old, new)
        identity = tx.apply()
        self.write(new, 'later editor change')
        with self.assertRaisesRegex(Conflict, 'later edit'):
            recover(self.ws, identity)
        self.assertEqual((self.root / new).read_text(), 'later editor change')

    def test_symlink_escape_rejected(self):
        link = self.root / 'engine/link'
        try:
            link.symlink_to(self.build, target_is_directory=True)
        except OSError:
            if os.name == 'nt':
                # Directory junctions exercise the Windows reparse-point boundary without symlink privilege.
                result = subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(link), str(self.build)],
                                        capture_output=True, env=dict(os.environ))
                self.assertEqual(result.returncode, 0, result.stderr)
            else:
                raise
        try:
            with self.assertRaisesRegex(Conflict, 'Symlink/reparse'):
                self.ws.path('engine/link/source.cpp')
        finally:
            if link.is_symlink():
                link.unlink()
            else:
                link.rmdir()

    def test_lock_excludes_second_writer(self):
        with self.ws.lock():
            with self.assertRaisesRegex(Conflict, 'Another structure writer'):
                with self.ws.lock(timeout=0.05):
                    self.fail('second writer acquired the lock')


class Operations(Fixture):
    def test_cmake_command_parser_preserves_comments_and_brackets(self):
        text = '# add_subdirectory(fake) )\nmessage([=[text ( )]=])\nadd_subdirectory("engine/numerics/dense" engine/dense)\n'
        commands = list(cmake_commands(text))
        self.assertEqual([item[2] for item in commands], ['message', 'add_subdirectory'])
        self.assertEqual(commands[1][3], ['engine/numerics/dense', 'engine/dense'])

    def test_remove_module_preserves_files_and_unregisters(self):
        self.write('CMakeLists.txt', 'add_subdirectory(engine/numerics/dense engine/dense)\n')
        plan = Plan(self.ws, self.model, 'remove module')
        plan.remove_module(self.name)
        self.run_plan(plan)
        self.assertNotIn('add_subdirectory(', (self.root / 'CMakeLists.txt').read_text())
        self.assertTrue((self.root / self.name / 'src/main.cpp').is_file())
        self.assertIn(self.name, load_manifest(self.ws)['excluded_modules'])

    def test_remove_module_rejects_live_dependents(self):
        self.model['targets']['consumer'] = dict(self.model['targets']['dense'], source_dir='tools/consumer', dependencies=['dense'])
        with self.assertRaisesRegex(Conflict, 'depends on'):
            Plan(self.ws, self.model, 'remove module').remove_module(self.name)

    def test_new_module_registers_before_folder_projection(self):
        self.write('CMakeLists.txt', 'project(Fixture)\ncrd_organize_targets("${CMAKE_CURRENT_SOURCE_DIR}")\n')
        plan = Plan(self.ws, self.model, 'new module')
        plan.module('crd-extra', 'engine/numerics/extra', 'STATIC', {'src/value.cpp': b'int value() { return 1; }\n'})
        self.run_plan(plan)
        text = (self.root / 'CMakeLists.txt').read_text()
        self.assertLess(text.index('add_subdirectory(engine/numerics/extra engine/extra)'), text.index('crd_organize_targets'))
        self.assertIn('add_library(crd-extra STATIC', (self.root / 'engine/numerics/extra/CMakeLists.txt').read_text())

    def test_registry_root_registers_crd_module_before_resolution(self):
        self.write('CMakeLists.txt', 'project(Fixture)\ncrd_module(engine/numerics/dense engine/dense)\ncrd_resolve_modules()\n'
                   'crd_add_modules()\ncrd_organize_targets("${CMAKE_CURRENT_SOURCE_DIR}")\n')
        plan = Plan(self.ws, self.model, 'new module')
        plan.module('crd-extra', 'engine/numerics/extra', 'STATIC', {'src/value.cpp': b'int value() { return 1; }\n'}, ['dense'])
        self.run_plan(plan)
        text = (self.root / 'CMakeLists.txt').read_text()
        self.assertIn('crd_module(engine/numerics/extra engine/extra DEPENDS dense)\n', text)
        self.assertLess(text.index('crd_module(engine/numerics/extra'), text.index('crd_resolve_modules'))
        self.assertNotIn('add_subdirectory(', text)

    def test_registry_test_directory_registers_crd_tests_and_declares_the_owner(self):
        self.write('CMakeLists.txt', 'crd_module(engine/numerics/dense engine/dense DEPENDS core)\ncrd_resolve_modules()\n'
                   'crd_add_modules()\n')
        self.write('tests/CMakeLists.txt', 'crd_tests(foundation/core core)\ncrd_stage_warp_dll("${CMAKE_CURRENT_SOURCE_DIR}")\n')
        plan = Plan(self.ws, self.model, 'new tests')
        plan.module('crd-dense-tests', 'tests/numerics/dense', 'EXECUTABLE', {'test.cpp': b'int main() { return 0; }\n'})
        self.run_plan(plan)
        tests = (self.root / 'tests/CMakeLists.txt').read_text()
        self.assertIn('crd_tests(numerics/dense dense)\n', tests)
        self.assertLess(tests.index('crd_tests(numerics/dense'), tests.index('crd_stage_warp_dll'))
        self.assertIn('crd_module(engine/numerics/dense engine/dense DEPENDS core TESTS numerics/dense)',
                      (self.root / 'CMakeLists.txt').read_text())

    def test_registry_unregister_removes_the_whole_crd_module_call(self):
        self.write('CMakeLists.txt', 'crd_module(engine/numerics/dense engine/dense DEPENDS core\n    TESTS numerics/dense)\n'
                   'crd_resolve_modules()\n')
        plan = Plan(self.ws, self.model, 'remove module')
        plan.remove_module(self.name)
        self.run_plan(plan)
        self.assertEqual((self.root / 'CMakeLists.txt').read_text(), 'crd_resolve_modules()\n')

    def test_cmake_projection_handles_explicit_and_discovered_sources(self):
        name = self.name + '/src/new.cpp'
        self.write(name, 'int new_answer() { return 9; }\n')
        plan = Plan(self.ws, self.model, 'projector')
        plan.exclude('dense', self.name + '/src/main.cpp')
        plan.add('dense', name)
        self.run_plan(plan)
        emitted = self.build / 'structure.cmake'
        emit_cmake(self.ws, emitted)
        cmake = shutil.which('cmake')
        self.assertIsNotNone(cmake)
        for discovery in (False, True):
            source = 'file(GLOB_RECURSE source CONFIGURE_DEPENDS "engine/numerics/dense/src/*.cpp")\n' if discovery else (
                'set(source "engine/numerics/dense/src/main.cpp" "engine/numerics/dense/src/other.cpp")\n')
            self.write('CMakeLists.txt', 'cmake_minimum_required(VERSION 3.25)\nproject(Projection LANGUAGES NONE)\n'
                       + source + 'add_library(dense INTERFACE ${source})\n'
                       + f'include("{emitted.as_posix()}")\ncrd_apply_project_structure()\n'
                       + 'get_target_property(actual dense SOURCES)\n'
                       + 'if(actual MATCHES "main.cpp" OR NOT actual MATCHES "new.cpp")\n'
                       + 'message(FATAL_ERROR "Incorrect projected membership: ${actual}")\nendif()\n')
            result = subprocess.run([cmake, '-S', str(self.root), '-B', str(self.root / ('build/glob' if discovery else 'build/list'))],
                                    env=dict(os.environ), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout.decode('utf-8', errors='replace'))

    def test_add_and_remove_keep_bytes(self):
        plan = Plan(self.ws, self.model, 'add')
        name = self.name + '/src/new.cpp'
        plan.add('dense', name, content=b'int new_value() { return 9; }\n')
        self.run_plan(plan)
        self.assertIn(name, load_manifest(self.ws)['targets']['dense']['add'])
        plan = Plan(self.ws, self.model, 'exclude')
        plan.exclude('dense', name)
        self.run_plan(plan)
        self.assertTrue((self.root / name).is_file())
        self.assertIn(name, load_manifest(self.ws)['targets']['dense']['remove'])

    def test_delete_updates_all_owners_and_recovers(self):
        self.model['targets']['other'] = copy.deepcopy(self.model['targets']['dense'])
        name = self.name + '/src/main.cpp'
        plan = Plan(self.ws, self.model, 'delete')
        plan.exclude('dense', name, delete=True)
        identity = self.run_plan(plan)
        self.assertFalse((self.root / name).exists())
        for target in ('dense', 'other'):
            self.assertIn(name, load_manifest(self.ws)['targets'][target]['remove'])
        recover(self.ws, identity)
        self.assertTrue((self.root / name).is_file())

    def test_empty_directory_is_durable(self):
        plan = Plan(self.ws, self.model, 'directory')
        name = self.name + '/src/empty'
        plan.directory('dense', name)
        self.run_plan(plan)
        self.assertTrue((self.root / name).is_dir())
        self.assertIn(name, load_manifest(self.ws)['targets']['dense']['directories'])

    def test_move_header_migrates_public_include_and_membership(self):
        old = self.name + '/include/crd/dense/value.hpp'
        new = self.name + '/include/crd/dense/new_value.hpp'
        self.write('tests/test.cpp', '#include <crd/dense/value.hpp>\n')
        plan = Plan(self.ws, self.model, 'header rename')
        plan.move(old, new)
        self.run_plan(plan)
        self.assertEqual((self.root / 'tests/test.cpp').read_text(), '#include <crd/dense/new_value.hpp>\n')
        self.assertIn(old, load_manifest(self.ws)['targets']['dense']['remove'])
        self.assertIn(new, load_manifest(self.ws)['targets']['dense']['add'])

    def test_module_move_updates_cmake_script_and_document_link(self):
        self.write('CMakeLists.txt', 'add_subdirectory(engine/numerics/dense engine/dense)\n')
        self.write(self.name + '/CMakeLists.txt', 'add_library(dense src/main.cpp src/other.cpp)\n')
        self.write('docs/overview.md', '[source](../engine/numerics/dense/src/main.cpp)\n')
        self.write('scripts/build.py', 'source = "engine/numerics/dense/src/main.cpp"\n')
        destination = 'engine/geometry/dense'
        plan = Plan(self.ws, self.model, 'module move')
        plan.move(self.name, destination)
        self.run_plan(plan)
        self.assertFalse((self.root / self.name).exists())
        self.assertIn('add_subdirectory(engine/geometry/dense engine/dense)', (self.root / 'CMakeLists.txt').read_text())
        self.assertIn('../engine/geometry/dense/src/main.cpp', (self.root / 'docs/overview.md').read_text())
        self.assertIn('engine/geometry/dense/src/main.cpp', (self.root / 'scripts/build.py').read_text())
        self.assertEqual((self.root / destination / 'CMakeLists.txt').read_text(),
                         'add_library(dense src/main.cpp src/other.cpp)\n')

    def test_last_translation_unit_cannot_be_silently_removed(self):
        plan = Plan(self.ws, self.model, 'empty target')
        plan.exclude('dense', self.name + '/src/main.cpp')
        plan.exclude('dense', self.name + '/src/other.cpp')
        with self.assertRaisesRegex(Conflict, 'no translation unit'):
            plan.finish()


class NativeAdapter(Fixture):
    def test_plain_cpp_project_add_is_adopted(self):
        baseline = self.native()
        self.write('CMakeLists.txt', 'project(Fixture)\n')
        source = self.build / 'new/new.cpp'
        source.parent.mkdir()
        source.write_text('int from_new() { return 4; }\n', encoding='utf-8')
        project_path = self.build / 'new/crd-new.vcxproj'
        project_path.write_text('<Project xmlns="' + NS + '"><PropertyGroup><ConfigurationType>StaticLibrary</ConfigurationType>'
                               '<ProjectGuid>{bbbb}</ProjectGuid></PropertyGroup><ItemGroup><ClCompile Include="new.cpp"/>'
                               '</ItemGroup></Project>', encoding='utf-8')
        path = self.build / 'Fixture.slnx'
        root = ET.parse(path).getroot()
        ET.SubElement(root.find('Folder'), 'Project', Path='new/crd-new.vcxproj', Id='bbbb')
        path.write_bytes(ET.tostring(root))
        ide_plan(self.ws, baseline).apply()
        self.assertTrue((self.root / 'engine/numerics/new/new.cpp').is_file())
        self.assertFalse(source.exists())
        self.assertIn('add_subdirectory(engine/numerics/new engine/new)', (self.root / 'CMakeLists.txt').read_text())

    def test_project_remove_unregisters_and_keeps_module(self):
        baseline = self.native()
        self.write('CMakeLists.txt', 'add_subdirectory(engine/numerics/dense engine/dense)\n')
        path = self.build / 'Fixture.slnx'
        root = ET.parse(path).getroot()
        folder = root.find('Folder')
        folder.remove(folder.find('Project'))
        path.write_bytes(ET.tostring(root))
        ide_plan(self.ws, baseline).apply()
        self.assertIn(self.name, load_manifest(self.ws)['excluded_modules'])
        self.assertTrue((self.root / self.name).is_dir())

    def test_external_source_add_is_imported_for_explicit_target(self):
        baseline = self.native()
        baseline['filesystem'] = inventory(self.ws, self.model)
        name = self.name + '/src/external.cpp'
        self.write(name, 'int external() { return 0; }\n')
        source_plan(self.ws, baseline).apply()
        self.assertIn(name, load_manifest(self.ws)['targets']['dense']['add'])

    def test_external_header_rename_migrates_includes(self):
        baseline = self.native()
        baseline['filesystem'] = inventory(self.ws, self.model)
        self.write('tests/use.cpp', '#include <crd/dense/value.hpp>\n')
        old = self.name + '/include/crd/dense/value.hpp'
        new = self.name + '/include/crd/dense/renamed.hpp'
        (self.root / old).rename(self.root / new)
        source_plan(self.ws, baseline).apply()
        self.assertEqual((self.root / 'tests/use.cpp').read_text(), '#include <crd/dense/renamed.hpp>\n')

    def test_filter_rename_moves_directory_and_files(self):
        baseline = self.native()
        owner = self.model['targets']['dense']
        path = (self.root / owner['project']).with_suffix('.vcxproj.filters')
        root = ET.parse(path).getroot()
        for node in root.iter():
            if node.tag.endswith('Filter') and node.get('Include') == 'src':
                node.set('Include', 'implementation')
            elif node.tag.endswith('Filter') and node.text == 'src':
                node.text = 'implementation'
        path.write_bytes(ET.tostring(root))
        ide_plan(self.ws, baseline).apply()
        self.assertFalse((self.root / self.name / 'src').exists())
        self.assertTrue((self.root / self.name / 'implementation/main.cpp').is_file())

    def test_unchanged_generation_is_noop(self):
        baseline = self.native()
        self.assertIsNone(ide_plan(self.ws, baseline))

    def test_partial_xml_is_conflict(self):
        baseline = self.native()
        self.write(self.model['targets']['dense']['project'], '<Project><')
        with self.assertRaisesRegex(Conflict, 'Incomplete/invalid'):
            ide_plan(self.ws, baseline)

    def test_add_item_in_generated_directory_is_relocated(self):
        baseline = self.native()
        owner = self.model['targets']['dense']
        project_path = self.root / owner['project']
        new_file = project_path.parent / 'new.cpp'
        new_file.write_text('int added() { return 1; }\n', encoding='utf-8')
        root = ET.parse(project_path).getroot()
        group = ET.SubElement(root, '{' + NS + '}ItemGroup')
        ET.SubElement(group, '{' + NS + '}ClCompile', Include='new.cpp')
        project_path.write_bytes(ET.tostring(root))
        filters_path = project_path.with_suffix('.vcxproj.filters')
        filters = ET.parse(filters_path).getroot()
        group = ET.SubElement(filters, '{' + NS + '}ItemGroup')
        item = ET.SubElement(group, '{' + NS + '}ClCompile', Include='new.cpp')
        ET.SubElement(item, '{' + NS + '}Filter').text = 'src'
        filters_path.write_bytes(ET.tostring(filters))
        tx = ide_plan(self.ws, baseline)
        self.assertIsNotNone(tx)
        tx.apply()
        self.assertFalse(new_file.exists())
        self.assertTrue((self.root / self.name / 'src/new.cpp').exists())

    def test_ide_remove_keeps_source(self):
        baseline = self.native()
        owner = self.model['targets']['dense']
        path = self.root / owner['project']
        root = ET.parse(path).getroot()
        source = self.name + '/src/main.cpp'
        for group in root:
            for item in list(group):
                if item.get('Include') == str(self.root / source):
                    group.remove(item)
        path.write_bytes(ET.tostring(root))
        ide_plan(self.ws, baseline).apply()
        self.assertTrue((self.root / source).is_file())
        self.assertIn(source, load_manifest(self.ws)['targets']['dense']['remove'])

    def test_module_move_from_solution_folder(self):
        baseline = self.native()
        self.write('CMakeLists.txt', 'add_subdirectory(engine/numerics/dense engine/dense)\n')
        path = self.build / 'Fixture.slnx'
        root = ET.parse(path).getroot()
        root.find('Folder').set('Name', '/engine/geometry/')
        path.write_bytes(ET.tostring(root))
        ide_plan(self.ws, baseline).apply()
        self.assertTrue((self.root / 'engine/geometry/dense/src/main.cpp').is_file())
        self.assertIn('engine/geometry/dense', (self.root / 'CMakeLists.txt').read_text())

    def test_xml_entity_is_rejected(self):
        baseline = self.native()
        self.write(self.model['targets']['dense']['project'], '<!DOCTYPE foo [<!ENTITY x SYSTEM "file:///etc/passwd">]><Project/>')
        with self.assertRaisesRegex(Conflict, 'Unsafe'):
            ide_plan(self.ws, baseline)


class Lifecycle(Fixture):
    def test_external_case_only_header_rename_migrates_includes(self):
        self.write('tests/use.cpp', '#include <crd/dense/value.hpp>\n')
        baseline = self.native()
        baseline['filesystem'] = inventory(self.ws, self.model)
        old = self.name + '/include/crd/dense/value.hpp'
        new = self.name + '/include/crd/dense/Value.hpp'
        (self.root / old).rename(self.root / new)
        source_plan(self.ws, baseline).apply()
        self.assertEqual(self.ws.bytes('tests/use.cpp'), b'#include <crd/dense/Value.hpp>\n')

    def test_generation_does_not_absorb_a_late_source_add(self):
        from project_sync import service
        baseline = self.native()
        expected = inventory(self.ws, self.model)
        atomic_write(service.state_dir(self.build) / 'generation.json', json_bytes({'filesystem': expected}))
        self.write(self.name + '/src/late.cpp', 'int late() { return 1; }\n')
        baseline['filesystem'] = inventory(self.ws, self.model)
        with patch.object(service, 'capture', return_value=baseline):
            service.finalize(self.root, self.build)
        pending = service.ingest(self.ws, self.build)
        self.assertIsNotNone(pending)
        self.assertIn(self.name + '/src/late.cpp', str(pending))

    def test_empty_solution_family_persists_in_slnx(self):
        from project_sync.visual_studio import solution_projection
        self.native()
        manifest = load_manifest(self.ws)
        manifest['directories'].append('engine/empty-family')
        atomic_write(self.root / MANIFEST, json_bytes(manifest))
        solution_projection(self.ws, self.build)
        self.assertIn('engine/empty-family', solution(self.ws, self.build)['folders'])
        before = (self.build / 'Fixture.slnx').read_bytes()
        solution_projection(self.ws, self.build)
        self.assertEqual(before, (self.build / 'Fixture.slnx').read_bytes())

    def test_empty_solution_family_persists_in_classic_sln(self):
        from project_sync.visual_studio import solution_projection
        self.write('build/native/Fixture.sln', 'Microsoft Visual Studio Solution File, Format Version 12.00\nGlobal\nEndGlobal\n')
        manifest = load_manifest(self.ws)
        manifest['directories'].append('engine/empty-family')
        atomic_write(self.root / MANIFEST, json_bytes(manifest))
        solution_projection(self.ws, self.build)
        self.assertIn('engine/empty-family', solution(self.ws, self.build)['folders'])

    def test_external_module_move_migrates_root_registration(self):
        self.write(self.name + '/CMakeLists.txt', 'add_library(dense src/main.cpp src/other.cpp)\n')
        self.write('CMakeLists.txt', 'add_subdirectory(engine/numerics/dense engine/dense)\n')
        baseline = self.native()
        baseline['filesystem'] = inventory(self.ws, self.model)
        (self.root / 'engine/scientific').mkdir()
        (self.root / self.name).rename(self.root / 'engine/scientific/dense')
        source_plan(self.ws, baseline).apply()
        self.assertIn('engine/scientific/dense', self.ws.bytes('CMakeLists.txt').decode())
        self.assertIn('engine/scientific', load_manifest(self.ws)['directories'])

    def test_external_new_module_is_registered_without_enabling_old_disabled_modules(self):
        self.write('CMakeLists.txt', 'add_subdirectory(engine/numerics/dense engine/dense)\n')
        self.write('engine/numerics/disabled/CMakeLists.txt', '# remains disabled\n')
        baseline = self.native()
        baseline['filesystem'] = inventory(self.ws, self.model)
        self.write('engine/numerics/added/CMakeLists.txt', 'add_library(added STATIC src/new.cpp)\n')
        self.write('engine/numerics/added/src/new.cpp', 'int added() { return 5; }\n')
        source_plan(self.ws, baseline).apply()
        text = self.ws.bytes('CMakeLists.txt').decode()
        self.assertIn('engine/numerics/added', text)
        self.assertNotIn('disabled', text)

    def test_moved_source_rebases_relative_include_to_unmoved_header(self):
        self.write(self.name + '/src/main.cpp', '#include "../include/crd/dense/value.hpp"\n')
        plan = Plan(self.ws, self.model, 'rebase include')
        plan.move(self.name + '/src/main.cpp', self.name + '/src/detail/main.cpp')
        self.run_plan(plan)
        self.assertIn(b'../../include/crd/dense/value.hpp', self.ws.bytes(self.name + '/src/detail/main.cpp'))

    def test_fresh_generation_does_not_feedback_configure(self):
        from project_sync import service
        baseline = self.native()
        atomic_write(service.state_file(self.build), json_bytes(baseline))
        with patch.object(service, 'configure') as configure:
            self.assertEqual(service.reconcile(self.root, self.build), (True, None))
            configure.assert_not_called()

    def test_changed_cmake_requires_generation_even_without_ide_delta(self):
        from project_sync import service
        from project_sync.storage import digest
        self.write('CMakeLists.txt', '# initial\n')
        baseline = self.native()
        baseline['model']['inputs']['CMakeLists.txt'] = digest(self.ws.bytes('CMakeLists.txt'))
        atomic_write(service.state_file(self.build), json_bytes(baseline))
        self.write('CMakeLists.txt', '# changed\n')
        with patch.object(service, 'configure') as configure:
            service.reconcile(self.root, self.build)
            configure.assert_called_once()

    def test_watcher_processes_initial_pending_and_retries_busy_ide(self):
        from project_sync import service
        calls = []
        def reconcile(root, build):
            calls.append(1)
            if len(calls) == 1:
                raise Conflict('Visual Studio is building')
            atomic_write(service.state_dir(build) / 'stop', b'stop')
            return True, {'transaction': 'example'}
        with patch.object(service, 'signature', return_value=['unchanged']), patch.object(service, 'reconcile', reconcile):
            service.watch_loop(self.root, self.build, interval=0.001)
        self.assertEqual(len(calls), 2)
        self.assertEqual(read_json(service.state_dir(self.build) / 'watcher.json')['status'], 'stopped')

    def test_dead_generation_is_actionable_conflict(self):
        from project_sync import service
        atomic_write(service.state_dir(self.build) / 'generation.json', json_bytes({'pid': 1234}))
        with patch.object(service.ide, 'process_alive', return_value=False):
            with self.assertRaisesRegex(Conflict, 'Generation did not finish'):
                service.reconcile(self.root, self.build)

    def test_classic_solution_folder_mapping(self):
        from project_sync.visual_studio import classic_solution
        self.write('build/native/Fixture.sln', '''Microsoft Visual Studio Solution File, Format Version 12.00
Project("{2150E333-8FDC-42A3-9474-1A3956D46DE8}") = "engine", "engine", "{111}"
EndProject
Project("{2150E333-8FDC-42A3-9474-1A3956D46DE8}") = "numerics", "numerics", "{222}"
EndProject
Project("{BC8A1FFA-BEE3-4634-8014-F334798102B3}") = "dense", "engine/dense/dense.vcxproj", "{333}"
EndProject
Global
GlobalSection(NestedProjects) = preSolution
{222} = {111}
{333} = {222}
EndGlobalSection
EndGlobal
''')
        result = classic_solution(self.ws, self.build / 'Fixture.sln')
        self.assertEqual(result['projects']['build/native/engine/dense/dense.vcxproj']['folder'], 'engine/numerics')

    def test_family_rename_moves_disabled_module_too(self):
        baseline = self.native()
        self.write('CMakeLists.txt', 'add_subdirectory(engine/numerics/dense engine/dense)\n')
        self.write('engine/numerics/disabled/CMakeLists.txt', '# optional\n')
        self.write('engine/numerics/disabled/note.txt', 'retained')
        solution_path = self.build / 'Fixture.slnx'
        root = ET.parse(solution_path).getroot()
        root.find('Folder').set('Name', '/engine/scientific/')
        solution_path.write_bytes(ET.tostring(root))
        ide_plan(self.ws, baseline).apply()
        self.assertEqual(self.ws.bytes('engine/scientific/disabled/note.txt'), b'retained')
        self.assertFalse((self.root / 'engine/numerics').exists())


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    unittest.main()
