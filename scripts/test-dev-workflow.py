#!/usr/bin/env python3
"""Adversarial selector tests; --integration also builds a tiny real CMake/CTest consumer."""
from pathlib import Path
import argparse
import copy
import hashlib
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
from cerid_dev.environment import (build_environment, cache_values, compiler_metadata, env_value, native_profile_issues, runtime_file,
                                  synchronization_state)
from cerid_dev.process import ProcessError, run_command
from cerid_dev.evidence import EvidenceError, inspect as inspect_evidence, seal
from cerid_dev.check import check, execution_doctor, junit_counts, native_configure_reason, test_indices
from dev import make_plan, parser as dev_parser

ROOT = Path(__file__).resolve().parents[1]


def fixture_process_running(pid):
    if os.name == 'nt':
        from project_sync.ide import process_alive
        return process_alive(pid)
    try:
        return not Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1].startswith('Z ')
    except FileNotFoundError:
        return False


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

    def test_doctor_cache_and_measured_compiler_metadata(self):
        (self.build / 'CMakeCache.txt').write_text('''// comment
# another comment
CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4
CMAKE_CACHE_MINOR_VERSION:INTERNAL=3
CMAKE_CACHE_PATCH_VERSION:INTERNAL=2
CMAKE_CXX_COMPILER:FILEPATH=/a path/clang++
VALUE:STRING=a=b=c
''', encoding='utf-8')
        self.assertEqual(cache_values(self.build)['VALUE'], 'a=b=c')
        self.assertNotIn('// comment', cache_values(self.build))
        correct = self.build / 'CMakeFiles/4.3.2/CMakeCXXCompiler.cmake'
        incorrect = self.build / 'CMakeFiles/9.0.0/CMakeCXXCompiler.cmake'
        for path, name in ((correct, 'Actual'), (incorrect, 'Unrelated')):
            path.parent.mkdir(parents=True)
            path.write_text(f'set(CMAKE_CXX_COMPILER_ID "{name}")\n', encoding='utf-8')
        self.assertEqual(compiler_metadata(self.build)['CMAKE_CXX_COMPILER_ID'], 'Actual')

    def test_doctor_runtime_path_is_not_an_executable_lookup(self):
        dll = self.root / 'runtime sample.dll'
        dll.write_bytes(b'fixture')
        environment = {'pAtH': str(self.root), 'UNRELATED_TOKEN': 'do-not-display'}
        self.assertEqual(env_value(environment, 'PATH'), str(self.root))
        self.assertEqual(runtime_file(dll.name, environment), str(dll.resolve()))
        self.assertIsNone(runtime_file('absent.dll', environment))

    def test_doctor_sync_status_does_not_create_workspace_files(self):
        before = sorted(str(path) for path in self.root.rglob('*'))
        result = synchronization_state(self.root, self.build)
        self.assertEqual(result['conflicts'], [])
        self.assertEqual(before, sorted(str(path) for path in self.root.rglob('*')))
        self.assertIn('not inspected', result['ide_buffers'])

    def test_doctor_cli_json_and_runtime_error_are_machine_readable(self):
        command = [sys.executable, str(ROOT / 'scripts/dev.py'), '--root', str(self.root), 'doctor',
                   '--build', str(self.build), '--inherit-env', '--json']
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        report = json.loads(result.stdout)
        self.assertFalse(report['configured'])
        self.assertFalse(report['environment_ready'])
        self.assertEqual(report['qualification'], 'environment diagnosis only; no build/tests executed')
        command[3] = str(self.root / 'missing')
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)['status'], 'instrument_failure')

    def test_doctor_rejects_empty_native_profile_compiler_defaults(self):
        cache = {'CRD_NATIVE_PROFILES': 'ON', 'CMAKE_CXX_FLAGS': '', 'CMAKE_CXX_FLAGS_DEBUG': '',
                 'CMAKE_CXX_FLAGS_RELEASE': '', 'CMAKE_CXX_FLAGS_RELWITHDEBINFO': ''}
        self.assertIn('empty compiler defaults', native_profile_issues(cache)[0])
        cache.update(CMAKE_CXX_FLAGS='/EHsc', CMAKE_CXX_FLAGS_DEBUG='/Od /RTC1',
                     CMAKE_CXX_FLAGS_RELEASE='/O2', CMAKE_CXX_FLAGS_RELWITHDEBINFO='/O2')
        self.assertEqual(native_profile_issues(cache), [])

    @unittest.skipUnless(os.name == 'nt', 'Windows native tool environment')
    def test_native_generator_initializes_runtime_tools_without_cached_compiler(self):
        scripts = self.root / 'scripts'
        scripts.mkdir()
        (scripts / 'msvc-env.bat').write_text('@echo off\n', encoding='utf-8')
        result = subprocess.CompletedProcess([], 0, stdout='Path=C:\\toolchain\n'.encode('utf-16-le'), stderr=b'')
        with patch('cerid_dev.environment.subprocess.run', return_value=result) as execute:
            environment, source = build_environment(self.root, {'CMAKE_GENERATOR': 'Visual Studio 18 2026'})
        execute.assert_called_once()
        self.assertEqual(env_value(environment, 'PATH'), 'C:\\toolchain')
        self.assertIn('msvc-env.bat', source)

    def test_doctor_distinguishes_live_generation_from_abandoned_record(self):
        registered = self.root / 'build/native'
        state = registered / 'cerid-project-sync'
        write_json(state / 'generation.json', {'pid': 1234})
        with patch('project_sync.service.registrations', return_value=[registered]):
            with patch('project_sync.ide.process_alive', return_value=True):
                live = synchronization_state(self.root, registered)
            with patch('project_sync.ide.process_alive', return_value=False):
                stopped = synchronization_state(self.root, registered)
        self.assertTrue(live['builds'][0]['generation_alive'])
        self.assertFalse(stopped['builds'][0]['generation_alive'])
        self.assertIn('Generation is active', live['conflicts'])
        self.assertIn('Generation needs reconciliation', stopped['conflicts'])

    def test_supervisor_preserves_native_failure_and_literal_arguments(self):
        output = self.build / 'command'
        literal = 'spaces & pipes | dollars $ and "quotes"'
        result = run_command([sys.executable, '-c', 'import sys; print(sys.argv[1]); sys.exit(7)', literal],
                             self.root, dict(os.environ), output, 10)
        self.assertEqual(result['status'], 'failed', result)
        self.assertEqual(result['exit_code'], 7)
        self.assertIn(literal, (output / 'output.log').read_text())
        self.assertEqual(json.loads((output / 'process.json').read_text())['exit_code'], 7)
        with self.assertRaises(FileExistsError):
            run_command([sys.executable, '--version'], self.root, dict(os.environ), output, 10)

    def test_supervisor_retries_status_read_without_repeating_native_command(self):
        read_text = Path.read_text
        denials = 0
        marker = self.root / 'executions.txt'

        def transient_denial(path, *args, **kwargs):
            nonlocal denials
            if path.name == 'native-status.json' and path.exists() and denials < 3:
                denials += 1
                raise PermissionError('fixture transient status-file denial')
            return read_text(path, *args, **kwargs)

        with patch.object(Path, 'read_text', transient_denial):
            result = run_command([sys.executable, '-c',
                                  'import sys; open(sys.argv[1], "a").write("once\\n"); sys.exit(7)', str(marker)],
                                 self.root, dict(os.environ), self.build / 'status-retry', 10)
        self.assertEqual(result['status'], 'failed', result)
        self.assertEqual(result['exit_code'], 7)
        self.assertEqual(result['status_read_retries'], 3)
        self.assertEqual(marker.read_text(), 'once\n')

    def test_supervisor_persistent_status_denial_is_an_instrument_failure(self):
        read_text = Path.read_text

        def persistent_denial(path, *args, **kwargs):
            if path.name == 'native-status.json' and path.exists():
                raise PermissionError('fixture persistent status-file denial')
            return read_text(path, *args, **kwargs)

        with patch.object(Path, 'read_text', persistent_denial):
            result = run_command([sys.executable, '--version'], self.root, dict(os.environ),
                                 self.build / 'status-denied', 10)
        self.assertEqual(result['status'], 'instrument_failure', result)
        self.assertEqual(result['exit_code'], 125)
        self.assertGreater(result['status_read_retries'], 1)
        self.assertLess(result['duration_seconds'], 8)
        self.assertFalse(fixture_process_running(result['supervisor_pid']))

    @unittest.skipUnless(os.name == 'nt' or sys.platform.startswith('linux'), 'Native process-liveness instrument unavailable')
    def test_supervisor_budget_stops_spawned_descendant(self):
        marker = self.root / 'grandchild.txt'
        script = self.root / 'tree.py'
        script.write_text('''import subprocess, sys, time
from pathlib import Path
if len(sys.argv) > 2:
    Path(sys.argv[1]).write_text(str(__import__('os').getpid()))
    time.sleep(60)
else:
    subprocess.Popen([sys.executable, __file__, sys.argv[1], 'grandchild'])
    time.sleep(60)
''', encoding='utf-8')
        result = run_command([sys.executable, str(script), str(marker)], self.root, dict(os.environ),
                             self.build / 'budget', 2)
        self.assertEqual(result['status'], 'budget_exhausted', result)
        self.assertTrue(marker.is_file(), 'The test must exercise a real spawned descendant before its budget expires')
        pid = int(marker.read_text())
        # Linux can retain a killed/reparented descendant as a zombie until init reaps it; it cannot execute.
        self.assertFalse(fixture_process_running(pid), 'A compiler-like child is still running after termination')

    def test_supervisor_rejects_nonfinite_budget_before_launch(self):
        for budget in (0, -1, float('inf'), float('nan')):
            with self.assertRaises(ProcessError):
                run_command([sys.executable, '--version'], self.root, dict(os.environ), self.build / 'invalid', budget)
        self.assertFalse((self.build / 'invalid').exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows Job Object assignment boundary')
    def test_supervisor_assignment_failure_cannot_start_native_tool(self):
        marker = self.root / 'must-not-start'
        (self.root / 'sitecustomize.py').write_text(f'from pathlib import Path\nPath({str(marker)!r}).touch()\n', encoding='utf-8')
        environment = dict(os.environ, PYTHONPATH=str(self.root))
        with patch('cerid_dev.process.WindowsJob.attach', side_effect=ProcessError('fixture assignment refusal')):
            result = run_command([sys.executable, '-c', 'from pathlib import Path; import sys; Path(sys.argv[1]).touch()',
                                  str(marker)], self.root, environment, self.build / 'refused', 10)
        self.assertEqual(result['status'], 'instrument_failure', result)
        self.assertFalse(marker.exists())
        from project_sync.ide import process_alive
        self.assertFalse(process_alive(result['supervisor_pid']))

    @unittest.skipUnless(os.name == 'nt' or sys.platform.startswith('linux'), 'Native process-liveness instrument unavailable')
    def test_supervisor_parent_death_does_not_leave_a_running_tool(self):
        import time
        marker = self.root / 'child-pid'
        launch = self.root / 'parent.py'
        launch.write_text('''import os, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from cerid_dev.process import run_command
run_command([sys.executable, '-c',
    'import os,sys,time; from pathlib import Path; Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(60)',
    sys.argv[2]], Path(sys.argv[3]), dict(os.environ), Path(sys.argv[4]), 30)
''', encoding='utf-8')
        outer = subprocess.Popen([sys.executable, str(launch), str(ROOT / 'scripts'), str(marker), str(self.root),
                                  str(self.build / 'parent-death')], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 8
            while not marker.exists() and outer.poll() is None and time.monotonic() < deadline:
                time.sleep(0.025)
            self.assertTrue(marker.exists(), 'The actual tool must start before parent-death qualification')
            pid = int(marker.read_text())
            outer.kill()
            outer.wait(timeout=10)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if not fixture_process_running(pid):
                    break
                time.sleep(0.025)
            else:
                self.fail('The supervised tool remained alive after its owning parent died')
        finally:
            if outer.poll() is None:
                outer.kill()
            outer.communicate(timeout=10)

    def test_evidence_integrity_does_not_turn_failure_into_success(self):
        run = self.build / 'failed-run'
        run.mkdir()
        (run / 'output.log').write_text('actual failure', encoding='utf-8')
        report = seal(run, {'status': 'failed', 'exit_code': 7})
        self.assertEqual(report['integrity'], 'verified')
        self.assertEqual(report['record']['exit_code'], 7)
        self.assertIn('content integrity only', report['qualification'])
        with self.assertRaisesRegex(EvidenceError, 'already sealed'):
            seal(run, {'status': 'passed'})

    def test_evidence_orders_portable_names_with_shared_file_directory_prefixes(self):
        run = self.build / 'prefix-order'
        (run / 'ctest').mkdir(parents=True)
        names = ['ctest/output.log', 'ctest.xml', 'Z.txt', 'a.txt']
        for name in names:
            (run / name).write_text(name, encoding='utf-8')
        report = seal(run, {'status': 'incomplete', 'exit_code': 2})
        self.assertEqual(report['artifact_count'], len(names))
        self.assertEqual(inspect_evidence(run)['record']['status'], 'incomplete')
        payload = json.loads((run / 'evidence.json').read_text())
        self.assertEqual([item['path'] for item in payload['artifacts']], sorted(names))

    def test_evidence_detects_changed_missing_and_extra_files(self):
        for mutation in ('change', 'delete', 'add'):
            run = self.build / mutation
            run.mkdir()
            (run / 'output.log').write_text('initial', encoding='utf-8')
            seal(run, {'status': 'passed'})
            if mutation == 'change':
                (run / 'output.log').write_text('changed', encoding='utf-8')
            elif mutation == 'delete':
                (run / 'output.log').unlink()
            else:
                (run / 'extra.log').touch()
            with self.subTest(mutation=mutation), self.assertRaises(EvidenceError):
                inspect_evidence(run)

    def test_evidence_rejects_forged_path_even_with_updated_metadata_checksum(self):
        run = self.build / 'escape'
        run.mkdir()
        (run / 'output.log').write_text('data', encoding='utf-8')
        seal(run, {'status': 'failed'})
        manifest = run / 'evidence.json'
        payload = json.loads(manifest.read_text())
        payload['artifacts'][0]['path'] = '../outside.log'
        payload.pop('payload_sha256')
        payload['payload_sha256'] = hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        manifest.write_text(json.dumps(payload), encoding='utf-8')
        with self.assertRaisesRegex(EvidenceError, 'artifact path'):
            inspect_evidence(run)

    def test_evidence_cli_rejects_modified_outcome(self):
        run = self.build / 'metadata'
        run.mkdir()
        seal(run, {'status': 'failed', 'exit_code': 9})
        command = [sys.executable, str(ROOT / 'scripts/dev.py'), 'evidence', '--run', str(run), '--json']
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['record']['exit_code'], 9)
        path = run / 'evidence.json'
        payload = json.loads(path.read_text())
        payload['record']['status'] = 'passed'
        path.write_text(json.dumps(payload), encoding='utf-8')
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 2)
        self.assertIn('checksum mismatch', json.loads(result.stdout)['error'])

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

    def test_object_consuming_guard_has_explicit_target_ownership(self):
        inventory = self.inventory()
        guard = inventory['tests'][-1]
        guard['properties'] = [{'name': 'LABELS', 'value': ['cerid.test.target=other']}]
        self.assertNotIn('global-guard', [item['name'] for item in select_tests(inventory, self.model(), ['consumer'])])
        selected = select_tests(inventory, self.model(), ['other'])
        self.assertEqual(next(item for item in selected if item['name'] == 'global-guard')['targets'], ['other'])
        guard['properties'][0]['value'] = ['cerid.test.target=missing']
        with self.assertRaisesRegex(SelectionError, 'unknown/non-buildable'):
            select_tests(inventory, self.model(), ['consumer'])

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

    def test_unbuilt_direct_test_requires_validated_artifact_and_fixture_ownership(self):
        inventory = self.inventory()
        direct = inventory['tests'][1]
        direct.pop('command')
        artifact = self.build / 'other/test.exe'
        annotation = {'name': 'LABELS', 'value': ['cerid.test.target=other',
                                               'cerid.test.executable=' + str(artifact)]}
        direct['properties'].append(annotation)
        self.assertNotIn('unrelated', [test['name'] for test in select_tests(inventory, self.model(), ['consumer'])])
        direct['properties'].append({'name': 'FIXTURES_SETUP', 'value': ['database']})
        selected = select_tests(inventory, self.model(), ['consumer'], allow_pending=True)
        self.assertTrue(next(test for test in selected if test['name'] == 'unrelated')['pending'])
        with self.assertRaisesRegex(SelectionError, 'fresh discovery'):
            select_tests(inventory, self.model(), ['consumer'])
        annotation['value'][-1] = 'cerid.test.executable=' + str(self.build / 'consumer/test.exe')
        with self.assertRaisesRegex(SelectionError, 'disagrees'):
            select_tests(inventory, self.model(), ['consumer'], allow_pending=True)
        annotation['value'][-1] = 'cerid.test.executable=' + str(artifact)
        direct['command'] = [str(self.build / 'consumer/test.exe')]
        with self.assertRaisesRegex(SelectionError, 'disagrees'):
            select_tests(inventory, self.model(), ['consumer'], allow_pending=True)
        direct.pop('command')
        artifact.parent.mkdir()
        artifact.touch()
        with self.assertRaisesRegex(SelectionError, 'incomplete'):
            select_tests(inventory, self.model(), ['consumer'], allow_pending=True)

    def test_pending_discovery_preserves_ownership_and_fixture_closure(self):
        inventory = self.inventory()
        setup = inventory['tests'][2]
        setup.pop('command')
        setup['name'] = 'cerid-pending-setup'
        setup['properties'].append({'name': 'LABELS', 'value': [
            'cerid.discovery.pending', 'cerid.discovery.target=other',
            'cerid.discovery.executable=' + str(self.build / 'other/test.exe')]})
        tests = select_tests(inventory, self.model(), ['consumer'], allow_pending=True)
        pending = next(test for test in tests if test['pending'])
        self.assertEqual(pending['targets'], ['other'])
        self.assertEqual(pending['reason'], 'fixture/dependency')
        with self.assertRaisesRegex(SelectionError, 'fresh discovery'):
            select_tests(inventory, self.model(), ['consumer'])
        setup['properties'] = [setup['properties'][-1]]
        self.assertNotIn(setup['name'], [test['name'] for test in select_tests(inventory, self.model(), ['consumer'])])

    def test_pending_annotation_must_match_actual_model_and_absent_artifact(self):
        inventory = self.inventory()
        pending = inventory['tests'][0]
        pending.pop('command')
        pending['name'] = 'cerid-pending-test'
        labels = {'name': 'LABELS', 'value': ['cerid.discovery.pending',
                  'cerid.discovery.executable=' + str(self.build / 'consumer/test.exe'),
                  'cerid.discovery.target=missing']}
        pending['properties'].append(labels)
        with self.assertRaisesRegex(SelectionError, 'unique configured target'):
            select_tests(inventory, self.model(), ['consumer'], allow_pending=True)
        labels['value'][-1] = 'cerid.discovery.target=base'
        with self.assertRaisesRegex(SelectionError, 'disagrees'):
            select_tests(inventory, self.model(), ['consumer'], allow_pending=True)
        labels['value'][-1] = 'cerid.discovery.target=consumer'
        artifact = self.build / 'consumer/test.exe'
        artifact.parent.mkdir()
        artifact.touch()
        with self.assertRaisesRegex(SelectionError, 'disagrees'):
            select_tests(inventory, self.model(), ['consumer'], allow_pending=True)

    def test_junit_requires_exact_names_and_counts_skips_and_failures(self):
        path = self.build / 'ctest.xml'
        selected = {name: {'disabled': name == 'disabled'} for name in ('pass', 'fail', 'skip', 'disabled')}
        path.write_text('''<testsuite tests="4"><testcase name="pass" status="run"/>
<testcase name="fail"><failure message="assertion"/></testcase>
<testcase name="skip"><skipped message="missing device"/></testcase>
<testcase name="disabled" status="notrun"/></testsuite>''', encoding='utf-8')
        self.assertEqual(junit_counts(path, selected), {'selected': 4, 'reported': 4, 'executed': 2, 'passed': 1,
                                                       'failed': 1, 'skipped': 2, 'disabled': 1})
        with self.assertRaisesRegex(SelectionError, 'exactly match'):
            junit_counts(path, {'pass': selected['pass']})
        path.write_text('<testsuite><testcase name="pass"/><testcase name="pass"/></testsuite>', encoding='utf-8')
        with self.assertRaisesRegex(SelectionError, 'exactly match'):
            junit_counts(path, {'pass': selected['pass']})
        path.write_text('<testsuite/>', encoding='utf-8')
        with self.assertRaisesRegex(SelectionError, 'Zero'):
            junit_counts(path, {})

    def test_ctest_index_file_never_uses_a_default_all_tests_range(self):
        inventory = {'tests': [{'name': str(i)} for i in range(10)]}
        self.assertEqual(test_indices(inventory, [{'name': '3'}, {'name': '8'}]), '4,4,1,9\n')
        with self.assertRaisesRegex(SelectionError, 'indices'):
            test_indices(inventory, [{'name': 'absent'}])

    def check_arguments(self, *extra):
        return dev_parser().parse_args(['--root', str(self.root), 'check', '--build', str(self.build),
                                        '--path', 'docs/sample.md', *extra])

    def test_documentation_check_runs_guards_without_compiler_and_seals_failures(self):
        scripts = self.root / 'scripts'
        scripts.mkdir()
        (scripts / 'check-master-plan.py').write_text('print("fixture document guard")\n', encoding='utf-8')
        (scripts / 'check-repository.py').write_text('raise SystemExit(7)\n', encoding='utf-8')
        with patch('cerid_dev.check.source_identity', return_value={'fixture': 'unchanged'}):
            report = check(self.check_arguments(), make_plan)
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['exit_code'], 7)
        self.assertEqual([phase['phase'] for phase in report['phases']], ['check-master-plan', 'check-repository'])
        self.assertEqual(inspect_evidence(Path(report['evidence_directory']))['record']['exit_code'], 7)
        (scripts / 'check-repository.py').write_text('print("fixture hygiene guard")\n', encoding='utf-8')
        with patch('cerid_dev.check.source_identity', side_effect=[{'fixture': 'before'}, {'fixture': 'after'}]):
            report = check(self.check_arguments(), make_plan)
        self.assertEqual(report['status'], 'instrument_failure')
        self.assertIn('changed during', report['error'])

    def test_check_dry_run_does_not_execute_or_create_evidence(self):
        with patch('cerid_dev.check.run_command') as run:
            report = check(self.check_arguments('--dry-run'), make_plan)
        run.assert_not_called()
        self.assertEqual(report['status'], 'dry_run')
        self.assertFalse((self.build / 'cerid-dev').exists())

    def test_check_lock_rejects_overlapping_checks_without_starting_tools(self):
        from project_sync.storage import Conflict, Workspace
        with Workspace(self.root).lock(name='developer-check'):
            with patch('cerid_dev.check.run_command') as run:
                with self.assertRaises(Conflict):
                    check(self.check_arguments(), make_plan)
        run.assert_not_called()

    def test_stale_watcher_snapshot_requires_authoritative_revalidation(self):
        from project_sync.storage import Conflict
        report = {'issues': ['Watcher recorded a structure conflict; reconcile before execution'],
                  'synchronization': {}, 'configured': True}
        with patch('cerid_dev.check.doctor', return_value=copy.deepcopy(report)):
            with patch('cerid_dev.check.sync_ready', side_effect=Conflict('Unsaved source')):
                with self.assertRaisesRegex(Conflict, 'Unsaved source'):
                    execution_doctor(self.root, self.build, 'Debug')
        with patch('cerid_dev.check.doctor', return_value=copy.deepcopy(report)):
            with patch('cerid_dev.check.sync_ready') as guard:
                result = execution_doctor(self.root, self.build, 'Debug')
        guard.assert_called_once_with(self.root, self.build)
        self.assertTrue(result['environment_ready'])
        self.assertIn('snapshot_revalidated', result['synchronization'])

    def test_simd_guard_preserves_decoder_failures_and_explicit_skips(self):
        # Deliberately controlled decoder output exercises guard reporting, not actual compiler emission.
        tool_dir = Path(self.temp.name) / 'decoder tools'
        tool_dir.mkdir()
        obj = tool_dir / 'fixture object.obj'
        obj.write_bytes(b'disassembly fixture')
        environment = dict(os.environ)
        path_key = next(key for key in environment if key.upper() == 'PATH')
        environment[path_key] = str(tool_dir) + os.pathsep + environment[path_key]
        decoder = tool_dir / ('dumpbin.bat' if os.name == 'nt' else 'objdump')
        command = ([shutil.which('powershell'), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                    str(ROOT / 'scripts/check_simd_emission.ps1'), '-Obj', str(obj)] if os.name == 'nt'
                   else [shutil.which('bash'), str(ROOT / 'scripts/check_simd_emission.sh'), '--obj', str(obj)])
        cases = [(9, 0, False, 'avx2', '0', 2), (9, 0, False, 'avx2', '1', 2),
                 (0, 0, False, 'avx2', '0', 1), (0, 0, False, 'avx2', '1', 77),
                 (0, 0, False, 'neon', '0', 77), (0, 110, False, 'scalar', '0', 0),
                 (0, 110, False, 'avx2', '0', 1), (0, 110, True, 'avx2', '0', 0),
                 (0, 110, True, 'sse2', '0', 1)]
        for index, (exit_code, count, ymm, expect, ipo, expected_exit) in enumerate(cases):
            with self.subTest(exit_code=exit_code, count=count, ymm=ymm, expect=expect, ipo=ipo):
                instruction = ('  0000000000000000: 90 nop' if os.name == 'nt' else '  0: 90 nop')
                lines = [instruction] * count + (['vaddps ymm0, ymm1, ymm2'] if ymm else [])
                if os.name == 'nt':
                    script = '@echo off\n' + ''.join('echo ' + line + '\n' for line in lines)
                    if exit_code:
                        script += 'echo fixture decoder diagnostic 1>&2\n'
                    script += f'exit /b {exit_code}\n'
                else:
                    script = '#!/usr/bin/env bash\n' + ''.join("printf '%s\\n' '" + line + "'\n" for line in lines)
                    script += f'exit {exit_code}\n'
                decoder.write_text(script, encoding='utf-8', newline='\n')
                decoder.chmod(0o755)
                args = ['-Expect', expect, '-Ipo', ipo] if os.name == 'nt' else ['--expect', expect, '--ipo', ipo]
                result = run_command(command + args, self.root, environment,
                                     Path(self.temp.name) / f'decoder-result-{index}', 15)
                output = Path(result['log']).read_text(errors='replace')
                self.assertEqual(result['exit_code'], expected_exit, output)
                if exit_code:
                    self.assertIn('exited 9', output)

    def test_native_check_uses_owner_baseline_even_with_a_readable_file_api_model(self):
        from project_sync.service import state_dir
        from project_sync.storage import Conflict
        with patch('project_sync.service.registrations', return_value=[self.build]):
            with patch('project_sync.service.needs_generation', return_value=True):
                self.assertIn('inputs changed', native_configure_reason(self.root, self.build))
            with patch('project_sync.service.needs_generation', return_value=False):
                self.assertIsNone(native_configure_reason(self.root, self.build))
                write_json(state_dir(self.build) / 'generation.json', {'pid': 1, 'failure': 1})
                self.assertIn('not finalized', native_configure_reason(self.root, self.build))
            with patch('project_sync.service.generation_idle', side_effect=Conflict('Live generation')):
                with self.assertRaisesRegex(Conflict, 'Live generation'):
                    native_configure_reason(self.root, self.build)


def discovery_integration(generator, catch_dir):
    """Actual Catch.cmake/CatchAddTests with tiny list-protocol executables, not a replacement Catch runtime."""
    cmake, ctest = shutil.which('cmake'), shutil.which('ctest')
    if not cmake or not ctest or not (catch_dir / 'Catch.cmake').is_file():
        raise SelectionError('Discovery fixture requires CMake/CTest and --catch-dir containing actual Catch.cmake')
    with tempfile.TemporaryDirectory(prefix='cerid-discovery-') as temp:
        root, build = Path(temp) / 'source', Path(temp) / 'build'
        root.mkdir()
        (root / 'probe.cpp').write_text('''#include <cstdio>
#include <cstring>
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--list-tests") == 0) {
        std::puts(PROBE_NAME "-" PROBE_CONFIGURATION); return 0;
    }
    return argc > 1 && std::strcmp(argv[1], PROBE_NAME "-" PROBE_CONFIGURATION) == 0 ? 0 : 1;
}
''', encoding='utf-8')
        (root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.25)
project(DiscoveryFixture LANGUAGES CXX)
enable_testing()
list(APPEND CMAKE_MODULE_PATH "{catch_dir.as_posix()}" "{(ROOT / 'cmake').as_posix()}")
include(CrdTestDiscovery)
foreach(name consumer setup other)
    add_executable(${{name}} probe.cpp)
    target_compile_definitions(${{name}} PRIVATE PROBE_NAME="${{name}}" PROBE_CONFIGURATION="$<CONFIG>")
endforeach()
crd_discover_tests(consumer PROPERTIES FIXTURES_REQUIRED database LABELS OFF TIMEOUT 30 RESOURCE_LOCK gpu)
crd_discover_tests(setup TEST_PREFIX "fixture-" PROPERTIES FIXTURES_SETUP database LABELS "existing;two")
crd_discover_tests(other)
''', encoding='utf-8')
        from project_sync.model import request_file_api
        request_file_api(build)
        counter = 0

        def run(command):
            nonlocal counter
            directory = Path(temp) / f'command-{counter}'
            counter += 1
            result = run_command(command, root, dict(os.environ), directory, 120)
            output = Path(result['log']).read_text(errors='replace')
            if result['exit_code']:
                raise SelectionError(str(result) + '\n' + output)
            return output

        def inventory(config):
            return json.loads(run([ctest, '--test-dir', str(build), '-C', config, '--show-only=json-v1']))

        run([cmake, '-S', str(root), '-B', str(build), '-G', generator, '-DCMAKE_BUILD_TYPE=Debug'])
        initial = inventory('Debug')
        model = load_model(root, build, 'Debug')
        selected = select_tests(initial, model, ['consumer'], allow_pending=True)
        if len(selected) != 2 or {owner for test in selected for owner in test['targets']} != {'consumer', 'setup'}:
            raise SelectionError('Unbuilt fixture ownership did not select exactly consumer and setup: ' + str(selected))
        for config in (('Debug', 'Release') if generator.startswith('Visual Studio') else ('Debug',)):
            run([cmake, '--build', str(build), '--config', config, '--target', 'consumer', 'setup', '--parallel', '2'])
        # After building Release last, -C Debug must still list and execute the Debug artifacts.
        for config in (('Debug', 'Release') if generator.startswith('Visual Studio') else ('Debug',)):
            tests = select_tests(inventory(config), load_model(root, build, config), ['consumer'])
            if [test['name'] for test in tests] != [f'consumer-{config}', f'fixture-setup-{config}']:
                raise SelectionError('Discovery selected another configuration or unrelated unbuilt target: ' + str(tests))
            consumer = tests[0]
            setup = tests[1]
            if (consumer['properties']['RESOURCE_LOCK'] != ['gpu'] or consumer['properties']['TIMEOUT'] != 30
                    or consumer['properties']['LABELS'] != ['OFF']
                    or not {'existing', 'two'} <= set(setup['properties']['LABELS'])):
                raise SelectionError('Original discovery properties were lost: ' + str(tests))
            print(run([ctest, '--test-dir', str(build), '-C', config, '-R', f'^consumer-{config}$',
                       '--no-tests=error', '--timeout', '30', '--output-on-failure']))
        print(f'PASS: {generator} unbuilt ownership, fixture expansion, properties and configuration-specific discovery')


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
''' + f'include("{(ROOT / "cmake/CrdTestOwnership.cmake").as_posix()}")\n'
            + 'crd_test_target(consumer-test consumer)\ncrd_test_target(other-test other)\n', encoding='utf-8')
        # Reuse the existing query writer; do not edit CMake reply files or evaluate CMake ourselves.
        from project_sync.model import request_file_api
        request_file_api(build)
        commands = [[cmake, '-S', str(root), '-B', str(build), '-G', generator, '-DCMAKE_BUILD_TYPE=Debug'],
                    [cmake, '--build', str(build), '--config', 'Debug', '--target', 'consumer', '--parallel', '2']]
        for number, command in enumerate(commands):
            result = run_command(command, root, dict(os.environ), Path(temp) / f'process-{number}', 120)
            seal(Path(temp) / f'process-{number}', {'phase': 'configure' if number == 0 else 'build',
                                                  'status': result['status'], 'exit_code': result['exit_code']})
            if result['exit_code']:
                raise SelectionError(str(result) + '\n' + Path(result['log']).read_text(errors='replace'))
        model = load_model(root, build, 'Debug')
        selection = select_targets(root, [{'path': 'include/value.hpp', 'status': 'M'}], model)
        if selection['scope'] != 'affected' or set(selection['targets']) != {'base', 'consumer'}:
            raise SelectionError('Real header-only interface did not select exactly its two consumers: ' + str(selection))
        result = subprocess.run([ctest, '--test-dir', str(build), '-C', 'Debug', '--show-only=json-v1'],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise SelectionError(result.stdout + result.stderr)
        selected = select_tests(json.loads(result.stdout), model, selection['targets'])
        if any(path.exists() for path in model['targets']['other']['artifacts']):
            raise SelectionError('Unrelated executable was built; fixture did not exercise partial-build ownership')
        if [item['name'] for item in selected] != ['consumer-test']:
            raise SelectionError('Real CTest artifact mapping selected unrelated tests: ' + str(selected))
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/dev.py'), '--root', str(root), 'plan',
                                 '--build', str(build), '--config', 'Debug', '--path', 'include/value.hpp', '--json'],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise SelectionError(result.stdout + result.stderr)
        plan = json.loads(result.stdout)
        if (plan['scope'] != 'affected' or not plan['test_discovery'].startswith('pending check:')
                or plan['revision']['mode'] != 'scenario' or not plan['content']['sha256']):
            raise SelectionError('CLI output disagrees with the qualified model selection: ' + str(plan))
        result = run_command([ctest, '--test-dir', str(build), '-C', 'Debug', '-R', '^consumer-test$',
                              '--no-tests=error', '--timeout', '30', '--output-on-failure'],
                             root, dict(os.environ), Path(temp) / 'process-test', 60)
        seal(Path(temp) / 'process-test', {'phase': 'ctest', 'status': result['status'],
                                          'exit_code': result['exit_code'], 'selected_tests': ['consumer-test']})
        print(Path(result['log']).read_text(errors='replace'))
        if result['exit_code']:
            raise SelectionError(f'Real consumer CTest failed: {result}')
        (root / 'scripts').mkdir()
        if os.name == 'nt':
            shutil.copy2(ROOT / 'scripts/msvc-env.bat', root / 'scripts/msvc-env.bat')
        for guard in ('check-master-plan.py', 'check-repository.py'):
            (root / 'scripts' / guard).write_text('print("temporary fixture guard executed")\n', encoding='utf-8')
        arguments = dev_parser().parse_args(['--root', str(root), 'check', '--build', str(build), '--config', 'Debug',
                                             '--path', 'docs/fixture.md', '--target', 'consumer'])
        # This temporary fixture has no Git repository. Git/source identity has independent adversarial tests;
        # these real tool invocations qualify orchestration against a fixed fixture identity only.
        with patch('cerid_dev.check.source_identity', return_value={'fixture': 'temporary-cmake-consumer'}):
            report = check(arguments, make_plan)
        if report['exit_code'] or report.get('tests', {}).get('executed') != 1:
            raise SelectionError('Complete check orchestration failed: ' + json.dumps(report, indent=2))
        inspect_evidence(Path(report['evidence_directory']))
        print('PASS: complete diagnostic check, fresh CTest/JUnit counts, guards and sealed evidence')
        print(f'PASS: {generator} real interface/generated-header/reverse-consumer/CTest fixture')


if __name__ == '__main__':
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--integration', action='store_true')
    args.add_argument('--generator', default='Ninja')
    args.add_argument('--catch-dir', type=Path, help='Actual Catch2 extras directory for discovery integration')
    options = args.parse_args()
    outcome = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(SelectionTests))
    if not outcome.wasSuccessful():
        raise SystemExit(1)
    if options.integration:
        integration(options.generator)
        if options.catch_dir:
            discovery_integration(options.generator, options.catch_dir.resolve(strict=True))
