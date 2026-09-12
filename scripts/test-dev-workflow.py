#!/usr/bin/env python3
"""Adversarial selector tests; --integration also builds a tiny real CMake/CTest consumer."""
from pathlib import Path
import argparse
import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from cerid_dev.selection import (SelectionError, changes_from_git, content_identity, load_model, parse_changes,
                                 select_targets, select_tests)

ROOT = Path(__file__).resolve().parents[1]


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding='utf-8')


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve() / 'source'
        self.build = Path(self.temp.name).resolve() / 'build'
        self.root.mkdir()
        self.build.mkdir()
        self.reply = self.build / '.cmake/api/v1/reply'
        self.reply.mkdir(parents=True)
        self.cmake = self.root / 'CMakeLists.txt'
        self.cmake.write_text('project(fixture)', encoding='utf-8')
        self.targets = {
            'base': {'id': 'opaque-dfa91', 'name': 'base', 'type': 'STATIC_LIBRARY',
                     'paths': {'source': 'engine/base', 'build': 'base'},
                     'sources': [{'path': 'engine/base/base.cpp'}],
                     'artifacts': [{'path': 'base/base.lib'}]},
            'middle': {'id': 'not-a-target-name', 'name': 'middle', 'type': 'STATIC_LIBRARY',
                       'paths': {'source': 'engine/middle', 'build': 'middle'},
                       'sources': [{'path': 'engine/middle/middle.cpp'}],
                       'dependencies': [{'id': 'opaque-dfa91'}]},
            'consumer': {'id': 'id-consumer', 'name': 'consumer', 'type': 'EXECUTABLE',
                         'paths': {'source': 'tests/consumer', 'build': 'consumer'},
                         'sources': [{'path': 'tests/consumer/test.cpp'},
                                     {'path': str(self.build / 'generated.hpp'), 'isGenerated': True}],
                         'dependencies': [{'id': 'not-a-target-name'}],
                         'compileGroups': [{'includes': [{'path': str(self.root / 'engine/header-only/include')}]}],
                         'artifacts': [{'path': 'consumer/test.exe'}]},
            'other': {'id': 'id-other', 'name': 'other', 'type': 'EXECUTABLE',
                      'paths': {'source': 'tests/other', 'build': 'other'},
                      'sources': [{'path': 'tests/other/test.cpp'}], 'artifacts': [{'path': 'other/test.exe'}]},
        }
        self.configure()

    def configure(self, configs=None):
        refs = []
        for name, target in self.targets.items():
            write_json(self.reply / (name + '.json'), target)
            refs.append({'id': target['id'], 'name': name, 'jsonFile': name + '.json'})
        write_json(self.reply / 'codemodel.json', {
            'paths': {'source': str(self.root), 'build': str(self.build)},
            'configurations': configs or [{'name': 'Debug', 'targets': refs}]})
        write_json(self.reply / 'cmakefiles.json', {'inputs': [{'path': 'CMakeLists.txt'}]})
        write_json(self.reply / 'index-001.json', {'cmake': {'generator': {'name': 'Ninja'}, 'paths': {}},
                   'objects': [{'kind': 'codemodel', 'version': {'major': 2}, 'jsonFile': 'codemodel.json'},
                               {'kind': 'cmakeFiles', 'version': {'major': 1}, 'jsonFile': 'cmakefiles.json'}]})

    def model(self):
        return load_model(self.root, self.build, 'Debug')

    def test_opaque_ids_reverse_consumers_and_private_source(self):
        plan = select_targets(self.root, [{'path': 'engine/base/base.cpp', 'status': 'M'}], self.model())
        self.assertEqual(plan['scope'], 'affected')
        self.assertEqual(plan['targets'], ['base', 'consumer', 'middle'])
        self.assertEqual(plan['owners']['engine/base/base.cpp'], ['base'])

    def test_header_only_propagated_include_and_generated_sources(self):
        model = self.model()
        plan = select_targets(self.root, [{'path': 'engine/header-only/include/public.hpp', 'status': 'M'}], model)
        self.assertEqual(plan['targets'], ['consumer'])
        self.assertEqual(plan['scope'], 'affected')
        self.assertIn(self.build / 'generated.hpp', model['targets']['consumer']['sources'])

    def test_unknown_asset_generator_deletion_and_new_file_broaden(self):
        for name, status in [('unknown.cpp', 'M'), ('assets/a.ckir', 'M'), ('scripts/generator.py', 'M'),
                             ('engine/base/base.cpp', 'D'), ('engine/base/new.cpp', '?')]:
            with self.subTest(name=name):
                plan = select_targets(self.root, [{'path': name, 'status': status}], self.model())
                self.assertEqual(plan['scope'], 'full')
                self.assertEqual(plan['targets'], sorted(self.targets))
                self.assertFalse(plan['local_sweep_allowed'])
                self.assertTrue(plan['reasons'])

    def test_documentation_needs_no_model_and_full_is_explicit(self):
        changes = [{'path': 'docs/design/a.md', 'status': '?'}]
        plan = select_targets(self.root, changes, model_error='missing')
        self.assertEqual(plan['scope'], 'documentation')
        self.assertEqual(plan['targets'], [])
        self.assertEqual(select_targets(self.root, changes, full=True)['scope'], 'full')

    def test_code_under_docs_is_not_a_documentation_only_change(self):
        plan = select_targets(self.root, [{'path': 'docs/generate.py', 'status': 'M'}], self.model())
        self.assertEqual(plan['scope'], 'full')
        model = self.model()
        model['inputs'].add(self.root / 'docs/options.md')
        plan = select_targets(self.root, [{'path': 'docs/options.md', 'status': 'M'}], model)
        self.assertEqual(plan['scope'], 'full')

    def test_missing_stale_and_failed_configure_models_rejected(self):
        with self.assertRaisesRegex(SelectionError, 'Missing'):
            load_model(self.root, self.build / 'absent')
        stamp = (self.reply / 'index-001.json').stat().st_mtime_ns + 10_000_000
        os.utime(self.cmake, ns=(stamp, stamp))
        with self.assertRaisesRegex(SelectionError, 'inputs changed'):
            self.model()
        os.utime(self.cmake, ns=(1_000_000, 1_000_000))
        error = self.reply / 'error-002.json'
        error.write_text('{}', encoding='utf-8')
        os.utime(error, ns=(stamp, stamp))
        with self.assertRaisesRegex(SelectionError, 'newer CMake'):
            self.model()

    def test_foreign_corrupt_and_escaping_references_rejected(self):
        path = self.reply / 'codemodel.json'
        original = json.loads(path.read_text())
        foreign = copy.deepcopy(original)
        foreign['paths']['source'] = str(self.root.parent)
        write_json(path, foreign)
        with self.assertRaisesRegex(SelectionError, 'another checkout'):
            self.model()
        path.write_text('{bad', encoding='utf-8')
        with self.assertRaisesRegex(SelectionError, 'invalid CMake'):
            self.model()
        original['configurations'][0]['targets'][0]['jsonFile'] = '../base.json'
        write_json(path, original)
        with self.assertRaisesRegex(SelectionError, 'local JSON'):
            self.model()

    def test_configuration_specific_edges_never_union(self):
        refs = json.loads((self.reply / 'codemodel.json').read_text())['configurations'][0]['targets']
        self.configure([{'name': 'Debug', 'targets': refs}, {'name': 'Release', 'targets': refs[-1:]}])
        with self.assertRaisesRegex(SelectionError, 'explicit --config'):
            load_model(self.root, self.build)
        with self.assertRaisesRegex(SelectionError, 'absent'):
            load_model(self.root, self.build, 'Shipping')
        release = load_model(self.root, self.build, 'Release')
        self.assertEqual(set(release['targets']), {'other'})

    def test_unknown_dependency_id_fails_closed(self):
        self.targets['middle']['dependencies'] = [{'id': 'base::invented'}]
        self.configure()
        with self.assertRaisesRegex(SelectionError, 'invalid CMake'):
            self.model()

    def test_generator_aggregates_do_not_force_full_build(self):
        self.targets['aggregate-one'] = {'id': 'gen-1', 'name': 'ALL_BUILD', 'isGeneratorProvided': True,
                                        'dependencies': [{'id': 'opaque-dfa91'}]}
        self.targets['aggregate-two'] = {'id': 'gen-2', 'name': 'ALL_BUILD', 'isGeneratorProvided': True,
                                        'dependencies': [{'id': 'id-other'}]}
        self.targets['base']['dependencies'] = [{'id': 'gen-1'}]
        self.configure()
        # Generator references carry their emitted name, not the fixture document's filename.
        path = self.reply / 'codemodel.json'
        value = json.loads(path.read_text())
        for ref in value['configurations'][0]['targets']:
            if ref['id'].startswith('gen-'):
                ref['name'] = 'ALL_BUILD'
        write_json(path, value)
        plan = select_targets(self.root, [{'path': 'engine/base/base.cpp', 'status': 'M'}], self.model())
        self.assertEqual(plan['targets'], ['base', 'consumer', 'middle'])

    def test_authored_utility_is_explained_but_never_an_automatic_build_command(self):
        self.targets['maintenance'] = {'id': 'utility', 'name': 'maintenance', 'type': 'UTILITY',
                                       'paths': {'source': '.', 'build': '.'},
                                       'dependencies': [{'id': 'opaque-dfa91'}]}
        self.configure()
        plan = select_targets(self.root, [{'path': 'engine/base/base.cpp', 'status': 'M'}], self.model())
        self.assertIn('maintenance', plan['targets'])
        self.assertNotIn('maintenance', plan['build_targets'])

    def test_git_renames_and_path_boundaries(self):
        self.assertEqual(parse_changes(b'R100\0old file.hpp\0new file.hpp\0'),
                         [{'path': 'old file.hpp', 'status': 'D'}, {'path': 'new file.hpp', 'status': 'A'}])
        for raw in (b'M\0../outside\0', b'M\0C:/outside\0', b'M\0missing-terminator', b'R100\0old\0'):
            with self.subTest(raw=raw), self.assertRaises(SelectionError):
                parse_changes(raw)

    def test_local_git_includes_net_tracked_and_untracked_without_index_writes(self):
        with patch('cerid_dev.selection.git', side_effect=[b'abc\n', b'M\0one.cpp\0D\0two.cpp\0', b'new.hpp\0']) as git:
            changes, revision = changes_from_git(self.root)
        self.assertEqual([item['path'] for item in changes], ['new.hpp', 'one.cpp', 'two.cpp'])
        self.assertEqual(revision['mode'], 'worktree')
        self.assertEqual([call.args[1] for call in git.call_args_list], ['rev-parse', 'diff', 'ls-files'])
        self.assertIn('HEAD', git.call_args_list[1].args)

    def test_ci_needs_clean_actual_candidate_and_explicit_pair(self):
        with self.assertRaisesRegex(SelectionError, 'both'):
            changes_from_git(self.root, base='base')
        with patch('cerid_dev.selection.git', side_effect=[b'actual', b'base', b'other']):
            with self.assertRaisesRegex(SelectionError, 'differs'):
                changes_from_git(self.root, 'base', 'candidate')
        with patch('cerid_dev.selection.git', side_effect=[b'head', b'base', b'head', b' M dirty.cpp\0']):
            with self.assertRaisesRegex(SelectionError, 'clean'):
                changes_from_git(self.root, 'base', 'head')

    def test_content_identity_changes_on_bytes_and_deletion(self):
        file = self.root / 'new.hpp'
        changes = [{'path': 'new.hpp', 'status': '?'}]
        file.write_bytes(b'one')
        first = content_identity(self.root, changes, {'head': 'a'})
        file.write_bytes(b'two')
        second = content_identity(self.root, changes, {'head': 'a'})
        file.unlink()
        third = content_identity(self.root, changes, {'head': 'a'})
        self.assertEqual(len({first['sha256'], second['sha256'], third['sha256']}), 3)
        self.assertEqual(third['files'][0]['kind'], 'missing')

    def inventory(self):
        def test(name, exe, **props):
            return {'name': name, 'command': [str(self.build / exe)],
                    'properties': [{'name': key, 'value': value} for key, value in props.items()]}
        return {'kind': 'ctestInfo', 'version': {'major': 1}, 'tests': [
            test('consumer', 'consumer/test.exe', FIXTURES_REQUIRED=['database'], RESOURCE_LOCK=['gpu'], TIMEOUT=45),
            test('unrelated', 'other/test.exe'),
            test('setup', 'other/test.exe', FIXTURES_SETUP=['database']),
            test('cleanup', 'other/test.exe', FIXTURES_CLEANUP=['database']),
            test('global-guard', 'python', DISABLED=True),
        ]}

    def test_ctest_artifacts_fixtures_guards_disabled_and_properties(self):
        tests = select_tests(self.inventory(), self.model(), ['consumer'])
        self.assertEqual([test['name'] for test in tests], ['cleanup', 'consumer', 'global-guard', 'setup'])
        consumer = next(test for test in tests if test['name'] == 'consumer')
        self.assertEqual(consumer['properties']['RESOURCE_LOCK'], ['gpu'])
        self.assertEqual(consumer['properties']['TIMEOUT'], 45)
        self.assertTrue(next(test for test in tests if test['name'] == 'global-guard')['disabled'])

    def test_unbuilt_zero_and_missing_dependency_are_not_passes(self):
        inventory = self.inventory()
        inventory['tests'][0].pop('command')
        with self.assertRaisesRegex(SelectionError, 'incomplete'):
            select_tests(inventory, self.model(), ['consumer'])
        inventory['tests'] = []
        with self.assertRaisesRegex(SelectionError, 'no discovered tests'):
            select_tests(inventory, self.model(), ['consumer'])
        inventory = self.inventory()
        inventory['tests'] = [inventory['tests'][-1]]
        with self.assertRaisesRegex(SelectionError, 'guard-only'):
            select_tests(inventory, self.model(), ['consumer'])
        inventory = self.inventory()
        inventory['tests'][0]['properties'].append({'name': 'DEPENDS', 'value': ['absent']})
        with self.assertRaisesRegex(SelectionError, 'unknown tests'):
            select_tests(inventory, self.model(), ['consumer'])


def integration(generator):
    """Actual propagated INTERFACE include, generated header, reverse link and CTest artifacts."""
    cmake, ctest = shutil.which('cmake'), shutil.which('ctest')
    if not cmake or not ctest:
        raise SelectionError('CMake and CTest are required for the integration fixture')
    with tempfile.TemporaryDirectory(prefix='cerid-dev-') as temp:
        root, build = Path(temp) / 'source', Path(temp) / 'build'
        (root / 'include').mkdir(parents=True)
        (root / 'include/value.hpp').write_text('inline int value() { return 7; }\n', encoding='utf-8')
        (root / 'base.cpp').write_text('#include <value.hpp>\nint base() { return value(); }\n', encoding='utf-8')
        (root / 'test.cpp').write_text('#include "generated.hpp"\nint base();\nint main() { return base() == expected ? 0 : 1; }\n', encoding='utf-8')
        (root / 'other.cpp').write_text('int main() { return 0; }\n', encoding='utf-8')
        (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.25)
project(DevFixture LANGUAGES CXX)
enable_testing()
add_library(headers INTERFACE)
target_include_directories(headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(base STATIC base.cpp)
target_link_libraries(base PUBLIC headers)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated.hpp" "constexpr int expected = 7;\\n")
add_executable(consumer test.cpp "${CMAKE_CURRENT_BINARY_DIR}/generated.hpp")
target_include_directories(consumer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(consumer PRIVATE base)
add_executable(other other.cpp)
add_test(NAME consumer-test COMMAND consumer)
add_test(NAME other-test COMMAND other)
''', encoding='utf-8')
        # Reuse the existing query writer; do not edit CMake reply files or evaluate CMake ourselves.
        from project_sync.model import request_file_api
        request_file_api(build)
        commands = [[cmake, '-S', str(root), '-B', str(build), '-G', generator, '-DCMAKE_BUILD_TYPE=Debug'],
                    [cmake, '--build', str(build), '--config', 'Debug', '--parallel', '2']]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise SelectionError(result.stdout + result.stderr)
        model = load_model(root, build, 'Debug')
        selection = select_targets(root, [{'path': 'include/value.hpp', 'status': 'M'}], model)
        if selection['scope'] != 'affected' or set(selection['targets']) != {'base', 'consumer'}:
            raise SelectionError('Real header-only interface did not select exactly its two consumers: ' + str(selection))
        result = subprocess.run([ctest, '--test-dir', str(build), '-C', 'Debug', '--show-only=json-v1'],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise SelectionError(result.stdout + result.stderr)
        selected = select_tests(json.loads(result.stdout), model, selection['targets'])
        if [item['name'] for item in selected] != ['consumer-test']:
            raise SelectionError('Real CTest artifact mapping selected unrelated tests: ' + str(selected))
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/dev.py'), '--root', str(root), 'plan',
                                 '--build', str(build), '--config', 'Debug', '--path', 'include/value.hpp', '--json'],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise SelectionError(result.stdout + result.stderr)
        plan = json.loads(result.stdout)
        if (plan['scope'] != 'affected' or plan['selected_test_count'] != 1
                or plan['revision']['mode'] != 'scenario' or not plan['content']['sha256']):
            raise SelectionError('CLI output disagrees with the qualified model selection: ' + str(plan))
        result = subprocess.run([ctest, '--test-dir', str(build), '-C', 'Debug', '-R', '^consumer-test$',
                                 '--no-tests=error', '--timeout', '30', '--output-on-failure'], timeout=60)
        if result.returncode:
            raise SelectionError(f'Real consumer CTest failed: {result.returncode}')
        print(f'PASS: {generator} real interface/generated-header/reverse-consumer/CTest fixture')


if __name__ == '__main__':
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--integration', action='store_true')
    args.add_argument('--generator', default='Ninja')
    options = args.parse_args()
    outcome = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(SelectionTests))
    if not outcome.wasSuccessful():
        raise SystemExit(1)
    if options.integration:
        integration(options.generator)
