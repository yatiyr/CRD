#!/usr/bin/env python3
"""Regression checks for hygiene rejection and CMake's generated IDE model."""
from pathlib import Path
import importlib.util
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hygiene', ROOT / 'scripts/check-repository.py')
hygiene = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hygiene)
master_spec = importlib.util.spec_from_file_location('master_plan', ROOT / 'scripts/check-master-plan.py')
master = importlib.util.module_from_spec(master_spec)
master_spec.loader.exec_module(master)
gate_spec = importlib.util.spec_from_file_location('registered_failures', ROOT / 'scripts/check-registered-failures.py')
gate = importlib.util.module_from_spec(gate_spec)
gate_spec.loader.exec_module(gate)
tiers_spec = importlib.util.spec_from_file_location('ci_tiers', ROOT / 'scripts/check-ci-tiers.py')
ci_tiers = importlib.util.module_from_spec(tiers_spec)
tiers_spec.loader.exec_module(ci_tiers)
tier_spec = importlib.util.spec_from_file_location('ci_tier', ROOT / 'scripts/ci-tier.py')
ci_tier = importlib.util.module_from_spec(tier_spec)
tier_spec.loader.exec_module(ci_tier)
evidence_spec = importlib.util.spec_from_file_location('ci_evidence', ROOT / 'scripts/ci-evidence.py')
ci_evidence = importlib.util.module_from_spec(evidence_spec)
evidence_spec.loader.exec_module(ci_evidence)
pins_check_spec = importlib.util.spec_from_file_location('check_pins', ROOT / 'scripts/check-pins.py')
check_pins = importlib.util.module_from_spec(pins_check_spec)
pins_check_spec.loader.exec_module(check_pins)
bench_spec = importlib.util.spec_from_file_location('build_bench', ROOT / 'scripts/build-bench.py')
build_bench = importlib.util.module_from_spec(bench_spec)
bench_spec.loader.exec_module(build_bench)
sccache_spec = importlib.util.spec_from_file_location('install_sccache', ROOT / 'scripts/install-sccache.py')
install_sccache = importlib.util.module_from_spec(sccache_spec)
sccache_spec.loader.exec_module(install_sccache)
consumer_spec = importlib.util.spec_from_file_location('package_consumer', ROOT / 'scripts/test-package-consumer.py')
package_consumer = importlib.util.module_from_spec(consumer_spec)
consumer_spec.loader.exec_module(package_consumer)
headers_spec = importlib.util.spec_from_file_location('check_headers', ROOT / 'scripts/check-headers.py')
check_headers = importlib.util.module_from_spec(headers_spec)
headers_spec.loader.exec_module(check_headers)
fuzz_spec = importlib.util.spec_from_file_location('fuzz_driver', ROOT / 'scripts/fuzz.py')
fuzz_driver = importlib.util.module_from_spec(fuzz_spec)
fuzz_spec.loader.exec_module(fuzz_driver)
provenance_spec = importlib.util.spec_from_file_location('check_generated', ROOT / 'scripts/check-generated.py')
provenance = importlib.util.module_from_spec(provenance_spec)
provenance_spec.loader.exec_module(provenance)
license_spec = importlib.util.spec_from_file_location('gen_license_manifest', ROOT / 'scripts/gen_license_manifest.py')
license_manifest = importlib.util.module_from_spec(license_spec)
license_spec.loader.exec_module(license_manifest)
sys.path.insert(0, str(ROOT / 'scripts'))
import pins as pins_registry
from cerid_dev import tidy as tidy_gate  # noqa: E402  # noqa: E402  (scripts/pins.py; the installers import the same module)


class MasterPlanSequence(unittest.TestCase):
    @staticmethod
    def rows(*states):
        return [(f'S{i}', [str(i), f'**S{i}**', state, 'Contract', '—', 'Evidence'])
                for i, state in enumerate(states, 1)]

    def test_blocked_review_and_partial_are_not_bypassed(self):
        for state in ('Blocked', 'Review', 'Partial', 'Open', 'Later', 'In progress'):
            with self.subTest(state=state):
                rows = self.rows('Done', 'Recorded', state, 'Open')
                self.assertEqual(master.first_unfinished(rows)[0], 'S3')
                self.assertEqual(master.sequence_errors(rows, ['S3']), [])
                self.assertTrue(master.sequence_errors(rows, ['S4']))

    def test_later_completion_or_activity_cannot_jump_a_gap(self):
        for state in ('Done', 'In progress'):
            with self.subTest(state=state):
                errors = master.sequence_errors(self.rows('Done', 'Blocked', state), ['S2'])
                self.assertEqual(len(errors), 1)
                self.assertIn('S3', errors[0])
        self.assertEqual(master.sequence_errors(self.rows('Done', 'Blocked', 'Partial'), ['S2']), [])

    def test_missing_duplicate_stale_and_unknown_pointers_fail(self):
        rows = self.rows('Done', 'Open', 'Partial')
        for current in ([], ['S1'], ['unknown'], ['S2', 'S2'], ['S2', 'S3']):
            with self.subTest(current=current):
                self.assertTrue(master.sequence_errors(rows, current))

    def test_complete_table_has_no_active_pointer(self):
        rows = self.rows('Done', 'Recorded')
        self.assertIsNone(master.first_unfinished(rows))
        self.assertEqual(master.sequence_errors(rows, []), [])
        self.assertTrue(master.sequence_errors(rows, ['S1']))

    def test_ci_wait_keeps_gate_without_stopping_earliest_work(self):
        rows = self.rows('Done', 'Needs CI', 'Needs CI', 'Partial', 'Open')
        self.assertEqual(master.first_unfinished(rows)[0], 'S4')
        self.assertEqual(master.sequence_errors(rows, ['S4']), [])
        self.assertTrue(master.sequence_errors(rows, ['S5']))
        self.assertEqual(master.sequence_errors(self.rows('Needs CI', 'Done', 'Open'), ['S3']), [])
        self.assertEqual(master.sequence_errors(self.rows('Done', 'Needs CI'), []), [])

    def test_future_work_requires_explicit_referenced_user_direction(self):
        rows = self.rows('Done', 'Partial', 'In progress')
        self.assertTrue(master.sequence_errors(rows, ['S3']))
        rows[2][1][5] += (' <!-- user-order: sessions/example.md -->'
                          ' [User direction](sessions/example.md)')
        self.assertEqual(master.sequence_errors(rows, ['S3']), [])
        rows[1][1][2] = 'In progress'
        self.assertTrue(master.sequence_errors(rows, ['S3']))
        rows[1][1][2] = 'Partial'
        rows[2][1][2] = 'Done'
        self.assertTrue(master.sequence_errors(rows, ['S3']))
        self.assertEqual(master.sequence_errors(rows, ['S2']), [])
        rows[2][1][5] = '<!-- user-order: sessions/example.md -->'
        self.assertTrue(master.sequence_errors(rows, ['S2']))

    def test_next_command_rejects_conflicting_context(self):
        args = SimpleNamespace(memory=None, slice=None, find=None, next=True)
        rows = self.rows('Done', 'Blocked', 'Partial')
        for pointer, expected in (('S2', 0), ('S3', 1)):
            with self.subTest(pointer=pointer), patch.object(master, 'read', return_value=
                    f'<!-- current-slice: {pointer} -->'), patch('sys.stdout', new_callable=io.StringIO) as output:
                self.assertEqual(master.query(args, rows), expected)
                self.assertIn('S2 [Blocked]', output.getvalue())
                self.assertIn('CI-only waits do not stop available work', output.getvalue())


class RepositoryTools(unittest.TestCase):
    def test_fft_codegen_declarations_preserve_expression_commas(self):
        from fft_codegen_style import canonicalize
        original = '        const T re = std::fma(a, b, c), im = data[2].im;\n'
        expected = '        const T re = std::fma(a, b, c);\n        const T im = data[2].im;\n'
        self.assertEqual(canonicalize(original), expected)
        self.assertEqual(canonicalize('    V re, im;\n'), '    V re;\n    V im;\n')
        original = '    { const crd::usize hi_ = crd::usize{4} << shift, lo = f(a, b);\n'
        expected = '    { const crd::usize high_im = crd::usize{4} << shift;\n      const crd::usize lo = f(a, b);\n'
        self.assertEqual(canonicalize(original), expected)
        self.assertEqual(canonicalize('void f(T a, T b);\n'), 'void f(T a, T b);\n')
        original = '    const crd::f64* const tr = twr + n * 64, * const ti = twi + n * 64;\n'
        expected = '    const crd::f64* const tr = twr + n * 64;\n    const crd::f64* const ti = twi + n * 64;\n'
        self.assertEqual(canonicalize(original), expected)
        original = 'CRD_FFT_GEN_INLINE void codelet() {}\n'
        self.assertEqual(canonicalize(canonicalize(original)), canonicalize(original))
        with self.assertRaisesRegex(ValueError, 'Unbalanced'):
            canonicalize('const T a = f(x, b = 1;')

    def test_validation_request_identifies_client_and_still_checks_bytes(self):
        installer_spec = importlib.util.spec_from_file_location('validation', ROOT / 'scripts/install-vulkan-validation.py')
        installer = importlib.util.module_from_spec(installer_spec)
        installer_spec.loader.exec_module(installer)
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp) / 'validation'
            with patch.object(installer.pins.urllib.request, 'urlopen', return_value=io.BytesIO(b'untrusted response')) as fetch:
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    installer.install(destination, None)
            request = fetch.call_args.args[0]
            self.assertEqual(request.full_url, installer.URL)
            self.assertEqual(request.get_header('User-agent'), installer.USER_AGENT)
            self.assertEqual(fetch.call_args.kwargs['timeout'], 120)
            self.assertFalse((destination / 'lib').exists())
            self.assertEqual([p.name for p in destination.iterdir()], [], 'no partial download may remain')

    def test_validation_download_rejects_wrong_checksum(self):
        installer_spec = importlib.util.spec_from_file_location('validation', ROOT / 'scripts/install-vulkan-validation.py')
        installer = importlib.util.module_from_spec(installer_spec)
        installer_spec.loader.exec_module(installer)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root / 'sdk.tar.xz'
            archive.write_bytes(b'not the pinned SDK')
            destination = root / 'validation'
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                installer.install(destination, archive)
            self.assertEqual(list(destination.iterdir()), [])

    def test_root_artifact_and_missing_registration_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            module = root / 'engine/numerics/hesap-dense'
            module.mkdir(parents=True)
            (module / 'CMakeLists.txt').write_text('', encoding='utf-8')
            (root / 'tests').mkdir()
            (root / 'tests/CMakeLists.txt').write_text('', encoding='utf-8')
            (root / 'CMakeLists.txt').write_text('add_subdirectory(engine/numerics/hesap-dense)', encoding='utf-8')
            self.assertEqual(hygiene.check(root), (1, []))
            (root / 'scratch_head.txt').write_text('scratch', encoding='utf-8')
            self.assertTrue(any('root artifact' in e for e in hygiene.check(root)[1]))
            (root / 'scratch_head.txt').unlink()
            (root / 'CMakeLists.txt').write_text('', encoding='utf-8')
            self.assertTrue(any('absent from build' in e for e in hygiene.check(root)[1]))

    def test_registry_registrations_count_and_missing_test_directories_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            module = root / 'engine/numerics/hesap-dense'
            module.mkdir(parents=True)
            (module / 'CMakeLists.txt').write_text('', encoding='utf-8')
            (root / 'tests').mkdir()
            (root / 'tests/CMakeLists.txt').write_text('', encoding='utf-8')
            (root / 'CMakeLists.txt').write_text('crd_module(engine/numerics/hesap-dense engine/hesap-dense DEPENDS core\n'
                                                 '    TESTS numerics/hesap-dense)\ncrd_resolve_modules()\n', encoding='utf-8')
            self.assertEqual(hygiene.check(root), (1, []))
            (root / 'tests/CMakeLists.txt').write_text('crd_tests(numerics/hesap-dense hesap-dense)\n', encoding='utf-8')
            self.assertTrue(any('missing directory' in e for e in hygiene.check(root)[1]))
            (root / 'CMakeLists.txt').write_text('crd_module(engine/numerics/hesap-dense-other engine/x)\n', encoding='utf-8')
            self.assertTrue(any('absent from build' in e for e in hygiene.check(root)[1]))

    def test_canonical_asset_encoding_and_newlines(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            assets = root / 'assets'
            assets.mkdir()
            fixture = assets / 'example.chir'
            fixture.write_bytes(b'program example {}\r\n')
            self.assertEqual(len(hygiene.canonical_asset_errors(root)), 1)
            fixture.write_bytes(b'\xef\xbb\xbfprogram example {}\n')
            self.assertEqual(len(hygiene.canonical_asset_errors(root)), 1)
            fixture.write_bytes(b'program example {}\r')
            self.assertEqual(len(hygiene.canonical_asset_errors(root)), 1)
            fixture.write_bytes(b'program example {\xff}\n')
            self.assertEqual(len(hygiene.canonical_asset_errors(root)), 1)
            fixture.write_bytes(b'program example {}\n')
            self.assertEqual(hygiene.canonical_asset_errors(root), [])

    def test_generated_target_folders(self):
        cmake = shutil.which('cmake')
        self.assertIsNotNone(cmake, 'CMake is required for the build-tool regression gate')
        ninja = shutil.which('ninja')
        if not ninja:
            cache = ROOT / 'build/win-debug/CMakeCache.txt'
            if cache.exists():
                match = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$', cache.read_text(encoding='utf-8'), re.M)
                ninja = match[1].strip() if match else None
        self.assertTrue(ninja and Path(ninja).is_file(), 'Ninja is required for the generated model gate')
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'source'
            build = Path(temp) / 'build'
            source.mkdir()
            expected = {'dense': 'engine/numerics', 'geometry': 'engine/geometry',
                        'dense_test': 'tests/numerics', 'peer': 'benchmarks',
                        'tool': 'applications/tools', 'vendor': 'dependencies'}
            directories = ['engine/numerics/hesap-dense', 'engine/geometry/geometry-bvh',
                           'tests/numerics/hesap-dense', 'tests/bench/numerics', 'tools/cooker', '_deps/vendor']
            lines = ['cmake_minimum_required(VERSION 3.25)', 'project(Folders LANGUAGES NONE)',
                     f'include("{(ROOT / "cmake/CrdIdeFolders.cmake").as_posix()}")']
            for directory, target in zip(directories, expected):
                path = source / directory
                path.mkdir(parents=True)
                (path / 'CMakeLists.txt').write_text(f'add_library({target} INTERFACE)\n', encoding='utf-8')
                lines.append(f'add_subdirectory({directory})')
            lines.append('crd_organize_targets("${CMAKE_CURRENT_SOURCE_DIR}")')
            # Check every property at configure time, including INTERFACE targets omitted by some IDEs.
            for target, folder in expected.items():
                lines.extend([f'get_target_property(actual {target} FOLDER)',
                              f'if(NOT actual STREQUAL "{folder}")',
                              f'  message(FATAL_ERROR "Wrong folder for {target}: ${{actual}}")', 'endif()'])
            (source / 'CMakeLists.txt').write_text('\n'.join(lines), encoding='utf-8')
            result = subprocess.run([cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
                                     f'-DCMAKE_MAKE_PROGRAM={ninja}'], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class PinnedWarp(unittest.TestCase):
    @staticmethod
    def installer():
        spec = importlib.util.spec_from_file_location('warp', ROOT / 'scripts/install-warp.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_warp_request_identifies_client_and_still_checks_bytes(self):
        installer = self.installer()
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp) / 'warp'
            with patch.object(installer.pins.urllib.request, 'urlopen', return_value=io.BytesIO(b'untrusted response')) as fetch:
                with self.assertRaisesRegex(ValueError, 'package checksum mismatch'):
                    installer.install(destination, None)
            request = fetch.call_args.args[0]
            self.assertEqual(request.full_url, installer.URL)
            self.assertEqual(request.get_header('User-agent'), installer.USER_AGENT)
            self.assertEqual(fetch.call_args.kwargs['timeout'], 120)
            self.assertFalse((destination / installer.OUTPUT).exists())
            self.assertEqual([p.name for p in destination.iterdir()], [], 'no partial download may remain')

    def test_warp_package_and_member_checksums_are_both_required(self):
        installer = self.installer()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root / 'warp.nupkg'
            archive.write_bytes(b'not the pinned package')
            destination = root / 'warp'
            with self.assertRaisesRegex(ValueError, 'package checksum mismatch'):
                installer.install(destination, archive)
            self.assertEqual(list(destination.iterdir()), [])
            with zipfile.ZipFile(archive, 'w') as package:
                package.writestr(installer.MEMBER, b'not the signed rasterizer')
                package.writestr('build/native/bin/arm64/d3d10warp.dll', b'wrong architecture')
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            with patch.object(installer, 'SHA256', digest):
                with self.assertRaisesRegex(ValueError, 'DLL checksum mismatch'):
                    installer.install(destination, archive)
            self.assertFalse((destination / installer.OUTPUT).exists())

    def test_staging_copies_the_dll_beside_every_executable(self):
        cmake = shutil.which('cmake')
        if cmake is None or os.name != 'nt':
            self.skipTest('Windows CMake unavailable')
        # Ninja needs a compiler already in the environment; otherwise CMake's default Visual Studio generator locates
        # the toolchain itself (the hosted repository job has CMake but no developer command prompt).
        ninja = shutil.which('ninja') if shutil.which('cl') is not None else None
        generator = ['-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={ninja}'] if ninja is not None else []
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'source'
            (source / 'nested').mkdir(parents=True)
            (source / 'optional').mkdir()
            dll = root / 'd3d10warp.dll'
            dll.write_bytes(b'pinned rasterizer stand-in')
            (source / 'main.c').write_text('int main(void) { return 0; }\n', encoding='utf-8')
            (source / 'CMakeLists.txt').write_text('\n'.join([
                'cmake_minimum_required(VERSION 3.25)', 'project(staging C)',
                f'list(APPEND CMAKE_MODULE_PATH "{(ROOT / "cmake").as_posix()}")', 'include(CrdWarp)',
                'add_executable(top main.c)', 'add_library(helper STATIC main.c)', 'add_subdirectory(nested)',
                'add_executable(bench EXCLUDE_FROM_ALL main.c)', 'add_subdirectory(optional EXCLUDE_FROM_ALL)',
                'crd_stage_warp_dll("${CMAKE_CURRENT_SOURCE_DIR}")', '']), encoding='utf-8')
            (source / 'nested/CMakeLists.txt').write_text('add_executable(inner ../main.c)\n', encoding='utf-8')
            (source / 'optional/CMakeLists.txt').write_text('add_executable(tool ../main.c)\n', encoding='utf-8')
            build = root / 'build'
            configure = subprocess.run([cmake, '-S', str(source), '-B', str(build), *generator, f'-DCRD_WARP_DLL={dll}'],
                                       capture_output=True, text=True, timeout=300)
            self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
            built = subprocess.run([cmake, '--build', str(build), '--config', 'Debug'],
                                   capture_output=True, text=True, timeout=600)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            staged = sorted(build.rglob('d3d10warp.dll'))
            self.assertEqual(len(staged), 2, staged) # one per ALL executable output directory, none for the library
            for copy in staged:
                self.assertEqual(copy.read_bytes(), dll.read_bytes(), copy)
                self.assertTrue(any(exe.suffix == '.exe' for exe in copy.parent.iterdir()), copy)
            # Executables excluded from ALL (target and directory forms) are neither staged nor forced to build.
            self.assertEqual([exe.name for exe in build.rglob('*.exe')].count('bench.exe'), 0)
            self.assertEqual([exe.name for exe in build.rglob('*.exe')].count('tool.exe'), 0)
            missing = subprocess.run([cmake, '-S', str(source), '-B', str(root / 'missing'), *generator,
                                      f'-DCRD_WARP_DLL={root / "absent.dll"}'],
                                     capture_output=True, text=True, timeout=300)
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn('CRD_WARP_DLL does not exist', missing.stdout + missing.stderr)


class RegisteredFailures(unittest.TestCase):
    """The third-party register gate is two-sided: unexpected failures and unexpected passes both fail the lane."""

    @staticmethod
    def register_text(lanes):
        block = json.dumps({'lanes': lanes}, indent=2)
        return ('# Register\n\n<!-- doc-role: reference -->\n\n<!-- registered-failures -->\n```json\n' + block
                + '\n```\n\n<a id="tp-1"></a>\n## TP-1: a provider defect\n\n<a id="tp-2"></a>\n## TP-2: another\n')

    @staticmethod
    def junit_text(states):
        cases = []
        for name, state in states.items():
            child = {'passed': '', 'failed': '<failure message="x"/>', 'skipped': '<skipped message="s"/>'}[state]
            status = {'passed': 'run', 'failed': 'fail', 'skipped': 'notrun'}[state]
            cases.append(f'<testcase name="{name}" classname="{name}" time="0" status="{status}">{child}</testcase>')
        return '<?xml version="1.0"?><testsuite name="s" tests="%d">%s</testsuite>' % (len(states), ''.join(cases))

    def gate(self, lanes, lane, states, ctest_exit):
        with tempfile.TemporaryDirectory() as temp:
            register = Path(temp) / 'register.md'
            register.write_text(self.register_text(lanes), encoding='utf-8')
            junit = Path(temp) / 'ctest.xml'
            junit.write_text(self.junit_text(states), encoding='utf-8')
            expected = gate.registered(gate.load_register(register), lane)
            return gate.evaluate(expected, gate.junit_results(junit), ctest_exit)

    LANES = {'win-asan': [{'test': 'A (DX12)', 'defect': 'TP-1'}, {'test': 'B (DX12)', 'defect': 'TP-1'}]}

    def test_exact_registered_set_passes_and_is_reported(self):
        code, lines = self.gate(self.LANES, 'win-asan', {'A (DX12)': 'failed', 'B (DX12)': 'failed', 'C': 'passed'}, 8)
        self.assertEqual(code, 0, lines)
        self.assertTrue(any('registered TP-1: failed  A (DX12)' in line for line in lines), lines)
        self.assertTrue(lines[-1].startswith('gate: PASS'), lines)

    def test_unexpected_failure_and_unexpected_pass_both_fail(self):
        code, lines = self.gate(self.LANES, 'win-asan', {'A (DX12)': 'failed', 'B (DX12)': 'passed', 'C': 'failed'}, 8)
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith('  UNEXPECTED FAILURE: C') for line in lines), lines)
        self.assertTrue(any('UNEXPECTED PASSED: registered TP-1 did not fail: B (DX12)' in line for line in lines), lines)
        code, _ = self.gate(self.LANES, 'win-asan', {'A (DX12)': 'failed', 'B (DX12)': 'failed', 'C': 'failed'}, 8)
        self.assertEqual(code, 1)
        code, _ = self.gate(self.LANES, 'win-asan', {'A (DX12)': 'passed', 'B (DX12)': 'passed', 'C': 'passed'}, 0)
        self.assertEqual(code, 1)

    def test_registered_test_skipped_or_absent_fails(self):
        code, lines = self.gate(self.LANES, 'win-asan', {'A (DX12)': 'failed', 'B (DX12)': 'skipped'}, 8)
        self.assertEqual(code, 1, lines)
        self.assertTrue(any('UNEXPECTED SKIPPED' in line for line in lines), lines)
        code, lines = self.gate(self.LANES, 'win-asan', {'A (DX12)': 'failed', 'C': 'passed'}, 8)
        self.assertEqual(code, 1, lines)
        self.assertTrue(any('UNEXPECTED ABSENT' in line for line in lines), lines)

    def test_lane_without_entries_requires_zero_failures_and_propagates_ctest_exit(self):
        self.assertEqual(self.gate(self.LANES, 'win-debug', {'A (DX12)': 'passed'}, 0)[0], 0)
        self.assertEqual(self.gate(self.LANES, 'win-debug', {'A (DX12)': 'passed'}, 3)[0], 3)
        self.assertEqual(self.gate(self.LANES, 'win-debug', {'A (DX12)': 'failed'}, 8)[0], 1)
        self.assertEqual(self.gate(self.LANES, 'win-asan', {'A (DX12)': 'failed', 'B (DX12)': 'failed'}, 255)[0], 255)

    def test_register_and_junit_evidence_are_validated(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'register.md'
            path.write_text('# no marker\n', encoding='utf-8')
            with self.assertRaisesRegex(gate.RegisterError, 'marker'):
                gate.load_register(path)
            path.write_text(self.register_text({'win-asan': [{'test': 'A', 'defect': 'TP-9'}]}), encoding='utf-8')
            with self.assertRaisesRegex(gate.RegisterError, 'TP-9 has no'):
                gate.load_register(path)
            path.write_text(self.register_text({'win-asan': [{'test': 'A', 'defect': 'TP-1'}, {'test': 'A', 'defect': 'TP-2'}]}),
                            encoding='utf-8')
            with self.assertRaisesRegex(gate.RegisterError, 'duplicate'):
                gate.load_register(path)
            junit = Path(temp) / 'empty.xml'
            junit.write_text('<testsuite tests="0"></testsuite>', encoding='utf-8')
            with self.assertRaisesRegex(gate.RegisterError, 'Zero CTest results'):
                gate.junit_results(junit)
        real = gate.load_register(ROOT / 'docs/third-party-defects.md')
        self.assertIn('win-asan', real['lanes'])
        self.assertEqual({entry['defect'] for entry in real['lanes']['win-asan']}, {'TP-1'})
        presets = {p['name'] for p in json.loads((ROOT / 'CMakePresets.json').read_text(encoding='utf-8'))['configurePresets']}
        self.assertTrue(set(real['lanes']) <= presets, set(real['lanes']) - presets)


class CiTiers(unittest.TestCase):
    """Tier mapping, resolution and the required aggregate (docs/design/ci-tiers.md)."""

    @classmethod
    def setUpClass(cls):
        cls.tiers = ci_tiers.load_json(ROOT / '.github/ci-tiers.json')
        cls.presets = ci_tiers.load_json(ROOT / 'CMakePresets.json')
        cls.workflow = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')

    def mutated(self, **changes):
        tiers = json.loads(json.dumps(self.tiers))
        for key, value in changes.items():
            tiers[key] = value
        return tiers

    def test_real_mapping_owns_every_visible_preset(self):
        self.assertEqual(ci_tiers.validate(self.tiers, self.presets, self.workflow), [])
        visible = set(ci_tiers.visible_presets(self.presets))
        owned = {n for n, e in self.tiers['presets'].items() if e['tier'] != 'diagnostic'}
        self.assertEqual(visible - owned, {'win-tidy-local', 'linux-clang-fuzz'})
        for never_hosted in ('win-vs', 'win-relwithdebinfo', 'win-shipping-profile', 'win-debug-scalar',
                             'linux-gcc-debug-scalar'):
            self.assertEqual(self.tiers['presets'][never_hosted]['tier'], 'complete', never_hosted)
        lanes = gate.load_register(ROOT / 'docs/third-party-defects.md')['lanes']
        self.assertTrue(set(lanes) <= owned, set(lanes) - owned)

    def test_missing_owner_unknown_preset_unreferenced_job_and_unsafe_cancellation_fail(self):
        presets = dict(self.tiers['presets'])
        del presets['win-debug']
        presets['win-nope'] = {'tier': 'change', 'job': 'windows'}
        problems = ci_tiers.validate(self.mutated(presets=presets), self.presets, self.workflow)
        self.assertTrue(any('win-debug has no tier owner' in p for p in problems), problems)
        self.assertTrue(any('win-nope is not a visible' in p for p in problems), problems)
        jobs = dict(self.tiers['jobs'], extra={'runner': 'ubuntu-latest'})
        presets = dict(self.tiers['presets'], **{'win-debug': {'tier': 'change', 'job': 'extra'}})
        problems = ci_tiers.validate(self.mutated(jobs=jobs, presets=presets), self.presets, self.workflow)
        self.assertTrue(any('workflow lacks job extra' in p for p in problems), problems)
        self.assertTrue(any('required job must need extra' in p for p in problems), problems)
        invoked = self.workflow.replace('cmake --preset ${{ matrix.preset }}', 'cmake --preset win-tidy-local', 1)
        problems = ci_tiers.validate(self.tiers, self.presets, invoked)
        self.assertTrue(any('diagnostic preset win-tidy-local is invoked' in p for p in problems), problems)
        unsafe = re.sub(r'cancel-in-progress: .*', 'cancel-in-progress: true', self.workflow)
        problems = ci_tiers.validate(self.tiers, self.presets, unsafe)
        self.assertTrue(any('limited to pull_request' in p for p in problems), problems)
        ungated = self.workflow.replace("if: needs.preflight.outputs.presets_linux_gcc != '[]'", 'if: true')
        problems = ci_tiers.validate(self.tiers, self.presets, ungated)
        self.assertTrue(any('linux-gcc must be gated' in p for p in problems), problems)
        classes = json.loads(json.dumps(self.tiers['classes']))
        classes[-1]['patterns'] = ['engine/**']
        problems = ci_tiers.validate(self.mutated(classes=classes), self.presets, self.workflow)
        self.assertTrue(any('catch-all' in p for p in problems), problems)

    def test_paths_resolve_to_the_highest_tier_and_unknown_bases_are_conservative(self):
        classified = dict((p, (c, t)) for p, c, t in ci_tier.classify(
            ['docs/ROADMAP.md', 'scripts/README.md', 'docs/third-party-defects.md', '.github/workflows/ci.yml',
             'engine/core/src/a.cpp', 'tests/core/CMakeLists.txt', 'CMakeLists.txt', 'engine/x/CMakeLists.txt',
             'assets/source/BoomBox.glb'], self.tiers['classes']))
        self.assertEqual(classified['docs/ROADMAP.md'], ('documentation', 'preflight'))
        self.assertEqual(classified['scripts/README.md'], ('documentation', 'preflight'))
        self.assertEqual(classified['docs/third-party-defects.md'], ('register', 'complete'))
        self.assertEqual(classified['.github/workflows/ci.yml'], ('build-system', 'complete'))
        self.assertEqual(classified['CMakeLists.txt'], ('build-system', 'complete'))
        for source in ('engine/core/src/a.cpp', 'tests/core/CMakeLists.txt', 'engine/x/CMakeLists.txt',
                       'assets/source/BoomBox.glb'):
            self.assertEqual(classified[source], ('source', 'change'), source)
        docs = ci_tier.resolve(self.tiers, 'push', 'a', 'b', paths=['docs/ROADMAP.md', 'context.md'])
        self.assertEqual(docs['tier'], 'preflight')
        self.assertEqual(docs['expected'], ['repository'])
        self.assertTrue(all(not presets for presets in docs['jobs'].values()))
        change = ci_tier.resolve(self.tiers, 'push', 'a', 'b', paths=['engine/core/src/a.cpp', 'docs/x.md'])
        self.assertEqual(change['tier'], 'change')
        self.assertEqual(change['jobs']['windows'], ['win-debug', 'win-release', 'win-asan', 'win-debug-sse2'])
        self.assertEqual(change['jobs']['windows-native'], [])
        self.assertNotIn('windows-native', change['expected'])
        complete = ci_tier.resolve(self.tiers, 'push', 'a', 'b', paths=['engine/core/src/a.cpp', 'cmake/x.cmake'])
        self.assertEqual(complete['tier'], 'complete')
        self.assertIn('win-vs', complete['jobs']['windows-native'])
        self.assertEqual(set(complete['expected']), {'repository'} | set(self.tiers['jobs']))
        for event, base, paths in (('push', '0' * 40, None), ('push', 'a', []), ('schedule', None, None)):
            with self.subTest(event=event, base=base):
                self.assertEqual(ci_tier.resolve(self.tiers, event, base, 'b', paths=paths)['tier'], 'complete')
        self.assertEqual(ci_tier.resolve(self.tiers, 'workflow_dispatch', requested='change')['tier'], 'change')
        self.assertEqual(ci_tier.resolve(self.tiers, 'workflow_dispatch')['tier'], 'complete')
        with self.assertRaises(SystemExit):
            ci_tier.resolve(self.tiers, 'workflow_dispatch', requested='preflight')
        self.assertIsNone(ci_tier.changed_paths('no-such-revision-1', 'no-such-revision-2'))
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'outputs'
            ci_tier.write_outputs(change, out)
            keys = {line.split('=', 1)[0] for line in out.read_text(encoding='utf-8').splitlines()}
            self.assertEqual(keys, {'tier', 'reason', 'expected'} | {ci_tier.output_key(j) for j in self.tiers['jobs']})
            for key in keys:
                self.assertIn('%s: ${{ steps.tier.outputs.%s }}' % (key, key), self.workflow) if key != 'reason' else None

    def test_real_build_system_revisions_resolve_to_the_complete_tier(self):
        known = subprocess.run(['git', 'cat-file', '-e', '18651d5^{commit}'], cwd=str(ROOT), capture_output=True)
        if known.returncode != 0:
            self.skipTest('revision 18651d5 is not in this clone')
        record = ci_tier.resolve(self.tiers, 'push', 'a0419cf', '18651d5')
        self.assertEqual(record['tier'], 'complete', record['reason'])

    def test_required_result_needs_preflight_and_every_expected_job(self):
        ok = {'preflight': {'result': 'success'}, 'repository': {'result': 'success'}, 'windows': {'result': 'success'},
              'clang-tidy': {'result': 'skipped'}}
        self.assertTrue(ci_tier.conclude(ok, ['repository', 'windows'])[0])
        self.assertFalse(ci_tier.conclude(ok, ['repository', 'windows', 'clang-tidy'])[0])
        self.assertFalse(ci_tier.conclude(dict(ok, windows={'result': 'skipped'}), ['repository', 'windows'])[0])
        self.assertFalse(ci_tier.conclude(dict(ok, **{'clang-tidy': {'result': 'failure'}}), ['repository', 'windows'])[0])
        self.assertFalse(ci_tier.conclude(dict(ok, **{'clang-tidy': {'result': 'cancelled'}}), ['repository', 'windows'])[0])
        self.assertFalse(ci_tier.conclude(dict(ok, preflight={'result': 'failure'}), ['repository', 'windows'])[0])
        self.assertFalse(ci_tier.conclude(ok, [])[0])
        self.assertFalse(ci_tier.conclude({'preflight': {'result': 'success'}}, ['repository'])[0])
        with patch.dict(os.environ, {'NEEDS': json.dumps(ok), 'EXPECTED': json.dumps(['repository', 'windows'])}):
            self.assertEqual(ci_tier.main(['conclude']), 0)
        with patch.dict(os.environ, {'NEEDS': json.dumps(ok), 'EXPECTED': json.dumps(['repository', 'clang-tidy'])}):
            self.assertEqual(ci_tier.main(['conclude']), 1)


class CiEvidence(unittest.TestCase):
    JUNIT = ('<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="x" tests="3" failures="1" skipped="1">\n'
             '<testcase name="slow" classname="slow" time="2.5" status="run"><system-out>ok</system-out></testcase>\n'
             '<testcase name="broken" classname="broken" time="0.5" status="run"><failure message="boom"/>'
             '<system-out>line1\nline2\nline3</system-out></testcase>\n'
             '<testcase name="absent" classname="absent" time="0" status="notrun"><skipped message="no device"/></testcase>\n'
             '</testsuite>\n')
    INVENTORY = {'kind': 'ctest', 'tests': [
        {'name': 'slow', 'properties': [{'name': 'LABELS', 'value': ['gpu']}, {'name': 'RESOURCE_LOCK', 'value': ['crd_gpu_device']}]},
        {'name': 'broken', 'properties': []}, {'name': 'absent', 'properties': [{'name': 'LABELS', 'value': ['gpu']}]}]}

    def fixture(self, temp):
        build = Path(temp) / 'build'
        (build / 'Testing/Temporary').mkdir(parents=True)
        (build / 'CMakeFiles/4.3.2').mkdir(parents=True)
        (build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug\nCRD_SIMD_LEVEL:STRING=sse2\n'
                                              'CMAKE_GENERATOR:INTERNAL=Ninja\nOTHER:BOOL=ON\n', encoding='utf-8')
        (build / 'CMakeFiles/4.3.2/CMakeCXXCompiler.cmake').write_text(
            'set(CMAKE_CXX_COMPILER "cl.exe")\nset(CMAKE_CXX_COMPILER_ID "MSVC")\n'
            'set(CMAKE_CXX_COMPILER_VERSION "19.51.36246.0")\n', encoding='utf-8')
        (build / 'Testing/inventory.json').write_text(json.dumps(self.INVENTORY), encoding='utf-8')
        (build / 'Testing/Temporary/ctest-junit.xml').write_text(self.JUNIT, encoding='utf-8')
        adapters = Path(temp) / 'adapters.txt'
        adapters.write_text('Name : Microsoft Basic Render Driver\n\nDriverVersion : 10.0\n', encoding='utf-8')
        return build, adapters

    def test_bundle_records_identity_toolchain_inventory_and_failures(self):
        with tempfile.TemporaryDirectory() as temp:
            build, adapters = self.fixture(temp)
            out = Path(temp) / 'evidence'
            record = ci_evidence.bundle('win-debug', build, out, adapters=adapters, environ={'GITHUB_RUN_ID': '7'})
            self.assertEqual(record['status'], 'fail')
            self.assertEqual(record['missing'], [])
            self.assertEqual(record['run'], {'GITHUB_RUN_ID': '7'})
            self.assertTrue(record['revision'] and record['tree'])
            self.assertEqual(record['toolchain']['CMAKE_CXX_COMPILER_VERSION'], '19.51.36246.0')
            self.assertEqual(record['toolchain']['CRD_SIMD_LEVEL'], 'sse2')
            self.assertNotIn('OTHER', record['toolchain'])
            self.assertEqual(record['inventory'], {'tests': 3, 'labels': {'gpu': 2}, 'resource_locks': {'crd_gpu_device': 1}})
            results = record['results']
            self.assertEqual((results['tests'], results['passed'], results['failures'], results['skipped']), (3, 1, 1, 1))
            self.assertEqual(results['failed'][0]['name'], 'broken')
            self.assertEqual(results['failed'][0]['output_tail'], ['line1', 'line2', 'line3'])
            self.assertEqual([s['name'] for s in results['slowest']], ['slow', 'broken', 'absent'])
            self.assertEqual(record['adapters'], ['Name : Microsoft Basic Render Driver', 'DriverVersion : 10.0'])
            self.assertEqual(json.loads((out / 'conclusion.json').read_text(encoding='utf-8'))['status'], 'fail')
            self.assertTrue((out / 'ctest-junit.xml').is_file() and (out / 'inventory.json').is_file())
            markdown = ci_evidence.summary_markdown(record)
            self.assertIn('### win-debug: fail', markdown)
            self.assertIn('`broken` (0.5 s): boom', markdown)
            self.assertIn('3 executed: 1 passed, 1 failed, 1 skipped', markdown)
            passed = ci_evidence.bundle('win-debug', build, out, adapters=adapters, outcome='success', environ={})
            self.assertEqual(passed['status'], 'pass')
            self.assertEqual(ci_evidence.bundle('win-debug', build, out, outcome='failure', environ={})['status'], 'failure')

    def test_bundle_without_inputs_reports_what_is_missing(self):
        with tempfile.TemporaryDirectory() as temp:
            record = ci_evidence.bundle('none', Path(temp) / 'nowhere', Path(temp) / 'out', environ={})
            self.assertEqual(record['status'], 'missing')
            self.assertEqual(record['missing'], ['CMakeCache.txt', 'adapters', 'inventory', 'junit'])
            self.assertIn('Missing evidence: CMakeCache.txt, adapters, inventory, junit', ci_evidence.summary_markdown(record))

    def test_glob_patterns_match_segments_and_trees(self):
        matches = lambda pattern, path: bool(ci_tier.glob_regex(pattern).match(path))
        self.assertTrue(matches('**/*.md', 'a/b/c.md') and matches('**/*.md', 'c.md'))
        self.assertFalse(matches('*.md', 'a/c.md'))
        self.assertTrue(matches('docs/**', 'docs/x/y.json'))
        self.assertFalse(matches('CMakeLists.txt', 'engine/CMakeLists.txt'))
        self.assertTrue(matches('**', 'anything/at/all'))


class PinnedInputs(unittest.TestCase):
    """The pin registry, its guard, the acquisition helpers and the build-owned patch (docs/design/pinned-inputs.md)."""

    @classmethod
    def setUpClass(cls):
        cls.pins = pins_registry.load()
        cls.workflow = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        cls.cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8-sig')
        cls.tiers = json.loads((ROOT / '.github/ci-tiers.json').read_text(encoding='utf-8'))

    def test_registry_workflow_build_and_helpers_agree(self):
        self.assertEqual(check_pins.validate(self.pins, self.workflow, self.cmake, self.tiers), [])
        self.assertEqual(re.findall(r'uses:\s*\S+@v\d', self.workflow), [])
        for action, pin in self.pins['actions'].items():
            self.assertIn('%s@%s # %s' % (action, pin['sha'], pin['ref']), self.workflow)
        self.assertEqual(re.findall(r'runs-on:\s*\S*-latest', self.workflow), [])
        self.assertEqual(len(re.findall(r'uses:\s*actions/checkout@', self.workflow)),
                         self.workflow.count('persist-credentials: false'))
        self.assertEqual(self.workflow.count("python-version: '3.12'"), 11)
        for package in ('Catch2', 'glfw', 'zstd', 'stb', 'cgltf', 'MikkTSpace', 'imgui', 'eigen', 'OpenBLAS'):
            self.assertIn('crd_add_pinned_package(%s' % package, self.cmake)

    def test_unpinned_references_fail_the_guard(self):
        checkout = self.pins['actions']['actions/checkout']
        pinned = 'actions/checkout@%s # %s' % (checkout['sha'], checkout['ref'])
        self.assertIn(pinned, self.workflow)
        floating = self.workflow.replace(pinned, 'actions/checkout@%s' % checkout['ref'], 1)
        problems = check_pins.validate_workflow(self.pins, floating)
        self.assertTrue(any('actions/checkout@%s' % checkout['ref'] in p for p in problems), problems)
        problems = check_pins.validate_workflow(self.pins, self.workflow.replace('runs-on: windows-2025', 'runs-on: windows-latest', 1))
        self.assertTrue(any('windows-latest' in p for p in problems), problems)
        problems = check_pins.validate_workflow(self.pins, self.workflow.replace('persist-credentials: false', 'persist-credentials: true', 1))
        self.assertTrue(any('persist-credentials' in p for p in problems), problems)
        problems = check_pins.validate_workflow(self.pins, self.workflow + '\n      - run: curl https://sdk.lunarg.com/x\n')
        self.assertTrue(any('sdk.lunarg.com' in p for p in problems), problems)
        unpinned, replaced = re.subn(r'crd_add_pinned_package\(glfw[^)]*\)', 'CPMAddPackage("gh:glfw/glfw#3.4")', self.cmake, count=1)
        self.assertEqual(replaced, 1)  # the call carries SYSTEM/EXCLUDE_FROM_ALL since the first complete-tier run
        problems = check_pins.validate_cmake(self.pins, unpinned)
        self.assertTrue(any('outside the registry' in p for p in problems) and any('glfw is never added' in p for p in problems), problems)
        broken = json.loads(json.dumps(self.pins))
        broken['packages']['imgui']['sha256'] = 'abc'
        broken['runners']['linux'] = 'ubuntu-latest'
        broken['actions']['actions/cache']['sha'] = 'v4'
        broken['packages']['imgui']['patches'] = ['no-such-patch']
        problems = check_pins.validate_registry(broken)
        for expected in ('imgui: sha256', 'floating label', 'actions/cache needs', 'no-such-patch'):
            self.assertTrue(any(expected in p for p in problems), (expected, problems))
        with tempfile.TemporaryDirectory() as temp:
            (Path(temp) / 'scripts').mkdir()
            (Path(temp) / 'scripts/pins.py').write_text("SHA = '%s'\n" % ('a' * 64), encoding='utf-8')
            problems = check_pins.validate_helpers(Path(temp))
            self.assertTrue(any('scripts/pins.py carries a literal digest' in p for p in problems), problems)

    def test_fetch_verifies_before_the_final_name_exists(self):
        good = b'pinned bytes'
        digest = hashlib.sha256(good).hexdigest()
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp)
            with patch.object(pins_registry.urllib.request, 'urlopen', return_value=io.BytesIO(b'wrong bytes')):
                with self.assertRaisesRegex(pins_registry.PinError, 'checksum mismatch'):
                    pins_registry.fetch(None, destination, url='https://example.invalid/a.bin', sha=digest, file='a.bin')
            self.assertEqual(list(destination.iterdir()), [])
            with patch.object(pins_registry.urllib.request, 'urlopen', return_value=io.BytesIO(good)) as fetch:
                path = pins_registry.fetch(None, destination, url='https://example.invalid/a.bin', sha=digest, file='a.bin')
            self.assertEqual(path.read_bytes(), good)
            self.assertEqual(fetch.call_args.args[0].get_header('User-agent'), pins_registry.USER_AGENT)
            with patch.object(pins_registry.urllib.request, 'urlopen', side_effect=AssertionError('no download expected')):
                self.assertEqual(pins_registry.fetch(None, destination, url='https://example.invalid/a.bin', sha=digest, file='a.bin'), path)
            path.write_bytes(b'corrupted later')
            with patch.object(pins_registry.urllib.request, 'urlopen', side_effect=AssertionError('no download expected')):
                with self.assertRaisesRegex(pins_registry.PinError, 'checksum mismatch'):
                    pins_registry.fetch(None, destination, url='https://example.invalid/a.bin', sha=digest, file='a.bin')

    def test_vulkan_sdk_installer_verifies_runs_and_checks_the_layout(self):
        spec = importlib.util.spec_from_file_location('sdk', ROOT / 'scripts/install-vulkan-sdk.py')
        sdk = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sdk)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'VulkanSDK'
            installer = Path(temp) / 'installer.exe'
            installer.write_bytes(b'installer')

            def fake_run(command, check):
                self.assertEqual(command[0], str(installer))
                self.assertEqual(tuple(command[1:]), sdk.INSTALL_ARGUMENTS)
                for relative in sdk.LAYOUT:
                    (root / relative).parent.mkdir(parents=True, exist_ok=True)
                    (root / relative).write_bytes(b'x')
                return subprocess.CompletedProcess(command, 0)

            with patch.object(sdk.pins, 'fetch', return_value=installer) as fetch:
                self.assertEqual(sdk.install(Path(temp), None, root, run=fake_run), root)
            self.assertEqual(fetch.call_args.args[1], Path(temp))
            with patch.object(sdk.pins, 'fetch', side_effect=AssertionError('installed layouts are not re-fetched')):
                self.assertEqual(sdk.install(Path(temp), None, root, run=fake_run), root)
            shutil.rmtree(root)
            with patch.object(sdk.pins, 'fetch', return_value=installer):
                with self.assertRaisesRegex(pins_registry.PinError, 'lacks'):
                    sdk.install(Path(temp), None, root, run=lambda command, check: subprocess.CompletedProcess(command, 0))
                with self.assertRaisesRegex(pins_registry.PinError, 'exited with 3'):
                    sdk.install(Path(temp), None, root, run=lambda command, check: subprocess.CompletedProcess(command, 3))

    def test_vulkan_headers_extract_only_the_include_subtree(self):
        spec = importlib.util.spec_from_file_location('headers', ROOT / 'scripts/install-vulkan-headers.py')
        headers = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(headers)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root / 'Vulkan-Headers.tar.gz'
            with tarfile.open(archive, 'w:gz') as tar:
                for name, data in (('Vulkan-Headers-abc/include/vulkan/vulkan.h', b'// vulkan'),
                                   ('Vulkan-Headers-abc/include/vk_video/x.h', b'// video'),
                                   ('Vulkan-Headers-abc/README.md', b'not extracted'),
                                   ('Vulkan-Headers-abc/include/../escape.h', b'not extracted')):
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
            files = {}
            for name in ('spirv_reflect.h', 'spirv_reflect.c', 'spirv.h'):
                files[name] = root / name
                files[name].write_bytes(name.encode())
            fetched = [archive, files['spirv_reflect.h'], files['spirv_reflect.c'], files['spirv.h']]
            with patch.object(headers.pins, 'fetch', side_effect=fetched):
                destination = headers.install(root / 'sdk', download_dir=root / 'downloads')
            self.assertEqual((destination / 'include/vulkan/vulkan.h').read_bytes(), b'// vulkan')
            self.assertTrue((destination / 'include/vk_video/x.h').is_file())
            self.assertFalse((destination / 'README.md').exists())
            self.assertFalse((destination / 'escape.h').exists())
            self.assertEqual((destination / 'Source/SPIRV-Reflect/spirv_reflect.c').read_bytes(), b'spirv_reflect.c')
            self.assertEqual((destination / 'Source/SPIRV-Reflect/include/spirv/unified1/spirv.h').read_bytes(), b'spirv.h')

    @staticmethod
    def tree_hash(root):
        digest = hashlib.sha256()
        for path in sorted(p for p in Path(root).rglob('*') if p.is_file()):
            digest.update(str(path.relative_to(root)).replace('\\', '/').encode())
            digest.update(path.read_bytes())
        return digest.hexdigest()

    def test_shared_source_cache_stays_pristine_and_patches_are_build_owned(self):
        cmake = shutil.which('cmake')
        if cmake is None:
            self.skipTest('CMake unavailable')
        ninja = shutil.which('ninja')
        generator = ['-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={ninja}'] if ninja else []
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archives = root / 'archives'
            archives.mkdir()
            archive = archives / 'fixture-1.0.tar.gz'
            with tarfile.open(archive, 'w:gz') as tar:
                for name, data in (('fixture-1.0/CMakeLists.txt', b'add_library(fixture INTERFACE)\n'),
                                   ('fixture-1.0/src/thing.h', b'// anchor: upstream value\nint thing = 1;\n')):
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
            registry = {'schema': 'cerid-pins/1', 'actions': {}, 'runners': {},
                        'tools': {'cpm': self.pins['tools']['cpm']},
                        'packages': {'fixture': {'version': '1.0', 'url': 'https://example.invalid/fixture-1.0.tar.gz',
                                                 'file': 'fixture-1.0.tar.gz', 'sha256': pins_registry.sha256(archive),
                                                 'license': 'MIT'}}}
            project = root / 'project'
            (project / 'cmake/patches').mkdir(parents=True)
            shutil.copyfile(ROOT / 'cmake/CrdPins.cmake', project / 'cmake/CrdPins.cmake')
            shutil.copyfile(ROOT / 'cmake/CPM.cmake', project / 'cmake/CPM.cmake')
            (project / 'cmake/patches/fixture-patch.cmake').write_text(
                'crd_patch_replace([[upstream value]] [[patched value]])\n', encoding='utf-8')
            (project / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(fixture_consumer LANGUAGES NONE)\n'
                'include(cmake/CPM.cmake)\ncrd_add_pinned_package(fixture DOWNLOAD_ONLY YES)\n'
                'crd_patched_copy(patched fixture src/thing.h fixture-patch)\n'
                'file(READ "${patched}" text)\nmessage(STATUS "PATCHED_TEXT=${text}")\n', encoding='utf-8')
            cache = root / 'cache'
            (cache / 'cpm').mkdir(parents=True)
            bootstrap = 'CPM_%s.cmake' % self.pins['tools']['cpm']['version']
            seeds = list(ROOT.glob('build/*/cmake/' + bootstrap))
            if os.environ.get('CPM_SOURCE_CACHE'):
                seeds.append(Path(os.environ['CPM_SOURCE_CACHE']) / 'cpm' / bootstrap)
            for seed in seeds:
                if seed.is_file() and pins_registry.sha256(seed) == self.pins['tools']['cpm']['sha256']:
                    shutil.copyfile(seed, cache / 'cpm' / bootstrap)
                    break

            def configure(build, archive_dir=archives, pins_document=registry):
                (project / 'cmake/pins.json').write_text(json.dumps(pins_document), encoding='utf-8')
                result = subprocess.run([cmake, '-S', str(project), '-B', str(build), *generator,
                                         f'-DCPM_SOURCE_CACHE={cache}', f'-DCRD_INPUT_ARCHIVES={archive_dir}'],
                                        capture_output=True, text=True, timeout=300)
                return result.returncode, result.stdout + result.stderr

            code, output = configure(root / 'build-a')
            self.assertEqual(code, 0, output)
            self.assertIn('PATCHED_TEXT=// anchor: patched value', output)
            self.assertIn('Patched fixture/src/thing.h with fixture-patch (1 replacements)', output)
            cached = [p for p in cache.rglob('thing.h') if 'patched' not in p.parts]
            self.assertEqual(len(cached), 1, cached)
            self.assertIn('upstream value', cached[0].read_text(encoding='utf-8'))
            self.assertIn('patched value', (root / 'build-a/patched/fixture/src/thing.h').read_text(encoding='utf-8'))
            first = self.tree_hash(cache)
            code, output = configure(root / 'build-b')
            self.assertEqual(code, 0, output)
            self.assertEqual(self.tree_hash(cache), first, 'a second consumer must not change the shared cache')
            code, output = configure(root / 'build-a')
            self.assertEqual(code, 0, output)
            self.assertNotIn('Patched fixture/src/thing.h', output, 'an unchanged patch is not rewritten')
            self.assertEqual(self.tree_hash(cache), first)
            wrong = json.loads(json.dumps(registry))
            wrong['packages']['fixture']['sha256'] = '0' * 64
            code, output = configure(root / 'build-c', pins_document=wrong)
            self.assertNotEqual(code, 0)
            self.assertIn('hash', output.lower())
            self.assertFalse((root / 'build-c/patched').exists())
            truncated = root / 'truncated'
            truncated.mkdir()
            (truncated / archive.name).write_bytes(archive.read_bytes()[: archive.stat().st_size // 2])
            code, output = configure(root / 'build-d', archive_dir=truncated)
            self.assertNotEqual(code, 0)
            self.assertIn('hash', output.lower())
            (project / 'cmake/patches/fixture-patch.cmake').write_text(
                'crd_patch_replace([[text upstream no longer has]] [[x]])\n', encoding='utf-8')
            code, output = configure(root / 'build-e')
            self.assertNotEqual(code, 0)
            self.assertIn('Patch fixture-patch no longer applies', output)
            self.assertEqual(self.tree_hash(cache), first)


class BuildPerformance(unittest.TestCase):
    """The build board runner, the lane build evidence, the sccache installer and the opt-in CMake module
    (docs/design/build-performance.md)."""

    NINJA_LOG = ('# ninja log v7\n'
                 '0\t1000\t1\ta.obj\t1\n'
                 '500\t3000\t1\tb.obj\t2\n'
                 '3000\t3500\t1\tapp.exe\t3\n'
                 '0\t200\t1\tc.obj\t4\n'      # end drops: a second invocation
                 '100\t900\t1\tapp.exe\t5\n'
                 'short line\n')
    STATS = {'stats': {'compile_requests': 10, 'cache_hits': {'counts': {'C/C++': 6}},
                       'cache_misses': {'counts': {'C/C++': 3}}, 'non_cacheable_compilations': 1,
                       'not_cached': {'/Fp': 1}, 'cache_errors': 0, 'cache_read_errors': 0, 'cache_write_errors': 0}}

    def test_ninja_log_and_sccache_summaries_feed_the_evidence_bundle(self):
        entries = ci_evidence.ninja_entries(self.NINJA_LOG.splitlines())
        self.assertEqual(len(entries), 5)
        summary = ci_evidence.ninja_summary(entries, slowest=2)
        self.assertEqual((summary['edges'], summary['invocations']), (5, 2))
        self.assertEqual(summary['span_seconds'], 3.5 + 0.9)
        self.assertEqual(summary['edge_seconds'], 1.0 + 2.5 + 0.5 + 0.2 + 0.8)
        self.assertEqual(summary['parallelism'], round(5.0 / 4.4, 2))
        self.assertEqual([s['output'] for s in summary['slowest']], ['b.obj', 'a.obj'])
        self.assertEqual(ci_evidence.ninja_summary([])['edges'], 0)
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp) / 'build'
            build.mkdir()
            (build / '.ninja_log').write_text(self.NINJA_LOG, encoding='utf-8')
            stats = Path(temp) / 'sccache.json'
            stats.write_text(json.dumps(self.STATS), encoding='utf-8')
            record = ci_evidence.bundle('win-debug', build, Path(temp) / 'out', outcome='success', environ={},
                                        sccache=stats)
            self.assertEqual(record['build']['ninja']['edges'], 5)
            self.assertEqual(record['build']['sccache'],
                             {'requests': 10, 'hits': 6, 'misses': 3, 'non_cacheable': 1, 'errors': 0, 'reasons': {'/Fp': 1}})
            markdown = ci_evidence.summary_markdown(record)
            self.assertIn('5 edges in 2 invocations, span 4.4 s', markdown)
            self.assertIn('sccache 6 hits, 3 misses, 1 non-cacheable (/Fp 1)', markdown)
            plain = ci_evidence.bundle('win-debug', build, Path(temp) / 'out', outcome='success', environ={})
            self.assertIsNone(plain['build']['sccache'])
            self.assertNotIn('sccache', ci_evidence.summary_markdown(plain).split('| Build |')[1].split('\n')[0])
            self.assertEqual(ci_evidence.bundle('x', Path(temp) / 'nowhere', Path(temp) / 'o', environ={})['build'],
                             {'ninja': None, 'sccache': None})

    def test_bench_rows_restore_edited_files_and_render_boards(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'thing.cpp'
            source.write_bytes(b'int x;\n')
            os.utime(source, ns=(1_600_000_000_000_000_000, 1_600_000_000_123_456_700))
            before = source.stat()
            with build_bench.Touched(source):
                self.assertTrue(source.read_bytes().endswith(build_bench.TOUCH))
                self.assertNotEqual(source.stat().st_mtime_ns, before.st_mtime_ns)
            self.assertEqual(source.read_bytes(), b'int x;\n')
            self.assertEqual(source.stat().st_mtime_ns, before.st_mtime_ns)
            log = Path(temp) / '.ninja_log'
            log.write_text(self.NINJA_LOG, encoding='utf-8')
            offset = build_bench.ninja_line_count(log)
            self.assertEqual(build_bench.ninja_entries(log, offset), [])
            log.write_text(self.NINJA_LOG + '0\t400\t1\tnew.obj\t6\n', encoding='utf-8')
            self.assertEqual(build_bench.ninja_summary(build_bench.ninja_entries(log, offset))['edges'], 1)
            self.assertEqual(build_bench.sccache_summary(self.STATS)['hits'], 6)
            self.assertEqual(build_bench.sccache_summary({'error': 'x'}), {'error': 'x'})
            total, available, committed = build_bench.memory_status()
            self.assertGreater(total, 0)
            self.assertTrue(build_bench.machine()['logical_cpus'])
        record = {'schema': build_bench.SCHEMA, 'label': 'A', 'preset': 'win-debug', 'jobs': 16, 'defines': [],
                  'launcher': '', 'toolchain': {'CRD_ENABLE_PCH': 'ON'}, 'build_tree_gib': 4.2,
                  'rows': [{'row': 'cold', 'exit_code': 0, 'wall_seconds': 100.0,
                            'ninja': {'edges': 3, 'invocations': 1, 'span_seconds': 99.0, 'edge_seconds': 900.0,
                                      'parallelism': 9.09, 'slowest': []},
                            'memory': {'pressure_gib': 5.5, 'min_available_gib': 40.0}}]}
        cached = json.loads(json.dumps(record))
        cached.update({'label': 'C', 'launcher': 'sccache', 'cache_gib': 1.5})
        cached['rows'][0]['sccache'] = build_bench.sccache_summary(self.STATS)
        table = build_bench.render([record, cached])
        self.assertIn('| A | cold | 100.0 | 3 | 99.0 | 900 | 9.1 | 5.5 | 40.0 | uncached |', table)
        self.assertIn('| C | cold | 100.0 | 3 | 99.0 | 900 | 9.1 | 5.5 | 40.0 | 6 / 3 / 1 (/Fp 1) |', table)
        self.assertIn('cache 1.5 GiB', build_bench.render_context([cached]))
        with self.assertRaises(SystemExit):
            build_bench.Board('win-debug', Path(temp) / 'b', 4, [], 'x', sccache='sccache', environ={})

    def test_sccache_installer_extracts_only_the_pinned_member_and_checks_its_digest(self):
        for archive_kind in ('zip', 'tar'):
            with tempfile.TemporaryDirectory() as temp, self.subTest(archive=archive_kind):
                root = Path(temp)
                binary = b'#!/bin/sh\necho sccache 0.17.0\n'
                archive = root / ('sccache.zip' if archive_kind == 'zip' else 'sccache.tar.gz')
                if archive_kind == 'zip':
                    with zipfile.ZipFile(archive, 'w') as package:
                        package.writestr('sccache-v0.17.0/sccache.exe', binary)
                        package.writestr('sccache-v0.17.0/README.md', b'not extracted')
                else:
                    with tarfile.open(archive, 'w:gz') as package:
                        for name, data in (('sccache-v0.17.0/sccache', binary), ('sccache-v0.17.0/README.md', b'no')):
                            info = tarfile.TarInfo(name)
                            info.size = len(data)
                            package.addfile(info, io.BytesIO(data))
                member = 'sccache-v0.17.0/sccache.exe' if archive_kind == 'zip' else 'sccache-v0.17.0/sccache'
                pin = {'version': '0.17.0', 'url': 'https://example.invalid/sccache', 'file': archive.name,
                       'sha256': pins_registry.sha256(archive), 'member': member,
                       'member_sha256': hashlib.sha256(binary).hexdigest()}
                installed = install_sccache.install(root / 'tools', archive=archive, pin=pin, check=False)
                self.assertEqual(installed.read_bytes(), binary)
                self.assertEqual(sorted(p.name for p in (root / 'tools').iterdir()), [Path(member).name])
                wrong = dict(pin, member_sha256='0' * 64)
                with self.assertRaisesRegex(pins_registry.PinError, 'binary checksum mismatch'):
                    install_sccache.install(root / 'wrong', archive=archive, pin=wrong, check=False)
                self.assertEqual(list((root / 'wrong').iterdir()), [])
                with patch.object(install_sccache.subprocess, 'run',
                                  return_value=SimpleNamespace(stdout='sccache 0.16.0\n')):
                    with self.assertRaisesRegex(pins_registry.PinError, 'reports'):
                        install_sccache.install(root / 'version', archive=archive, pin=pin)
                self.assertEqual(list((root / 'version').iterdir()), [])
        for name in ('sccache-windows', 'sccache-linux'):
            pin = pins_registry.entry('tools', name)
            self.assertTrue(pin['member'].endswith('sccache.exe' if name.endswith('windows') else '/sccache'))
            self.assertRegex(pin['member_sha256'], r'^[0-9a-f]{64}$')
        self.assertIn('scripts/install-sccache.py', check_pins.HELPERS)

    def test_cmake_module_applies_launcher_and_pools_only_when_asked(self):
        cmake = shutil.which('cmake')
        ninja = shutil.which('ninja')
        if cmake is None or ninja is None:
            self.skipTest('CMake and Ninja are required')
        with tempfile.TemporaryDirectory() as temp:
            project = Path(temp) / 'project'
            (project / 'cmake').mkdir(parents=True)
            shutil.copyfile(ROOT / 'cmake/CrdBuildPerformance.cmake', project / 'cmake/CrdBuildPerformance.cmake')
            (project / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(fixture LANGUAGES NONE)\n'
                'option(CRD_SHIPPING "" OFF)\noption(CRD_ENABLE_PCH "" ON)\n'
                'list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")\ninclude(CrdBuildPerformance)\n'
                'message(STATUS "LAUNCHER=[${CMAKE_CXX_COMPILER_LAUNCHER}] POOL_COMPILE=[${CMAKE_JOB_POOL_COMPILE}] '
                'POOL_LINK=[${CMAKE_JOB_POOL_LINK}]")\n', encoding='utf-8')

            def configure(build, *defines):
                result = subprocess.run([cmake, '-S', str(project), '-B', str(build), '-G', 'Ninja',
                                         f'-DCMAKE_MAKE_PROGRAM={ninja}', *defines],
                                        capture_output=True, text=True, timeout=300)
                return result.returncode, result.stdout + result.stderr

            code, output = configure(Path(temp) / 'default')
            self.assertEqual(code, 0, output)
            self.assertIn('LAUNCHER=[] POOL_COMPILE=[] POOL_LINK=[]', output)
            self.assertNotIn('[crd] Compiler launcher', output)
            self.assertNotIn('pool crd_', (Path(temp) / 'default/CMakeFiles/rules.ninja').read_text(encoding='utf-8'))
            code, output = configure(Path(temp) / 'tuned', '-DCRD_COMPILER_LAUNCHER=/opt/sccache',
                                     '-DCRD_COMPILE_JOBS=3', '-DCRD_LINK_JOBS=1')
            self.assertEqual(code, 0, output)
            self.assertIn('[crd] Compiler launcher: /opt/sccache (Ninja', output)
            self.assertIn('LAUNCHER=[/opt/sccache] POOL_COMPILE=[crd_compile] POOL_LINK=[crd_link]', output)
            self.assertIn('[crd] Ninja job pools: crd_compile=3;crd_link=1', output)
            rules = (Path(temp) / 'tuned/CMakeFiles/rules.ninja').read_text(encoding='utf-8')
            self.assertRegex(rules, r'pool crd_compile\n  depth = 3\n')
            self.assertRegex(rules, r'pool crd_link\n  depth = 1\n')
            code, output = configure(Path(temp) / 'shipping', '-DCRD_COMPILER_LAUNCHER=/opt/sccache', '-DCRD_SHIPPING=ON')
            self.assertEqual(code, 0, output)
            self.assertIn('Compiler launcher ignored: Shipping', output)
            self.assertIn('LAUNCHER=[] POOL_COMPILE=[]', output)
            code, output = configure(Path(temp) / 'bad', '-DCRD_COMPILE_JOBS=many')
            self.assertNotEqual(code, 0)
            self.assertIn('CRD_COMPILE_JOBS must be a positive integer', output)


class PublicConsumption(unittest.TestCase):
    """REPO.DEV.8: public-header checks, the relocatable package consumer test and the local header check."""

    FIXTURE_MODULE = {
        'mod/CMakeLists.txt': 'add_library(mod-lib STATIC src/a.cpp)\n'
                              'target_include_directories(mod-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")\n',
        'mod/src/a.cpp': '#include <mod/good.hpp>\nint a() { return good().value_or(0); }\n',
        'mod/include/mod/good.hpp': '#pragma once\n#include <optional>\ninline std::optional<int> good() { return 1; }\n',
        # Compiles today only because its includer provided <optional> first: the check must name it.
        'mod/include/mod/bad.hpp': '#pragma once\ninline std::optional<int> bad() { return 2; }\n',
        'mod/include/mod/table.hpp': 'ROW(1)\nROW(2)\n',
        'mod/include/mod/config.hpp.in': '#define VALUE @VALUE@\n',
    }

    @staticmethod
    def fixture(temp, exclusion):
        project = Path(temp) / 'project'
        (project / 'cmake').mkdir(parents=True)
        for name in ('CrdModules.cmake', 'CrdPublicChecks.cmake'):
            shutil.copyfile(ROOT / 'cmake' / name, project / 'cmake' / name)
        for name, content in PublicConsumption.FIXTURE_MODULE.items():
            path = project / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding='utf-8')
        (project / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(fixture LANGUAGES CXX)\n'
            'set(CMAKE_CXX_STANDARD 20)\noption(CRD_BUILD_TESTS "" OFF)\n'
            'list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")\n'
            'include(CrdModules)\ninclude(CrdPublicChecks)\n'
            'crd_module(mod)\n'
            f'crd_public_header_exclude(mod {exclusion} REASON "X-macro row table, included by its users")\n'
            'crd_add_modules()\ncrd_add_public_header_checks()\n', encoding='utf-8')
        return project

    def test_public_header_checks_compile_each_header_alone_through_the_consumer_view(self):
        cmake, ninja = shutil.which('cmake'), shutil.which('ninja')
        compiler = shutil.which('cl') or shutil.which('g++') or shutil.which('c++') or shutil.which('clang++')
        if cmake is None or ninja is None or compiler is None:
            self.skipTest('CMake, Ninja and a C++ compiler are required')
        with tempfile.TemporaryDirectory() as temp:
            project = self.fixture(temp, 'mod/table.hpp')

            def cmake_run(*arguments, timeout=600):
                result = subprocess.run([cmake, *arguments], capture_output=True, text=True, timeout=timeout,
                                        encoding='utf-8', errors='replace')
                return result.returncode, result.stdout + result.stderr

            default = Path(temp) / 'default'
            code, output = cmake_run('-S', str(project), '-B', str(default), '-G', 'Ninja',
                                     f'-DCMAKE_MAKE_PROGRAM={ninja}')
            self.assertEqual(code, 0, output)
            self.assertNotIn('Public header checks', output)
            self.assertNotIn('crd-header-checks', (default / 'build.ninja').read_text(encoding='utf-8'))

            checked = Path(temp) / 'checked'
            code, output = cmake_run('-S', str(project), '-B', str(checked), '-G', 'Ninja',
                                     f'-DCMAKE_MAKE_PROGRAM={ninja}', '-DCRD_PUBLIC_CHECKS=ON')
            self.assertEqual(code, 0, output)
            self.assertIn('[crd] Public header checks: 2 headers of 1 modules compiled standalone '
                          '(1 excluded with a reason)', output)
            shims = sorted(path.relative_to(checked / 'public-checks').as_posix()
                           for path in (checked / 'public-checks').rglob('*.cpp'))
            self.assertEqual(shims, ['mod/mod/bad.hpp.cpp', 'mod/mod/good.hpp.cpp'])
            self.assertIn('#include <mod/good.hpp>', (checked / 'public-checks/mod/mod/good.hpp.cpp').read_text())
            code, output = cmake_run('--build', str(checked), '--target', 'crd-header-checks', '--', '-k', '0')
            self.assertNotEqual(code, 0, output)
            self.assertIn('bad.hpp', output)
            self.assertTrue(list(checked.rglob('good.hpp.cpp.o*')), 'the self-contained header compiled')
            self.assertFalse(list(checked.rglob('bad.hpp.cpp.o*')), 'the non-self-contained header did not')
            self.assertFalse(list(checked.rglob('table.hpp.cpp*')), 'the excluded fragment has no shim')
            self.assertFalse(list(checked.rglob('config.hpp.in*')), 'a template is not a header')

            stale = self.fixture(Path(temp) / 'stale', 'mod/missing.hpp')
            code, output = cmake_run('-S', str(stale), '-B', str(Path(temp) / 'stale-build'), '-G', 'Ninja',
                                     f'-DCMAKE_MAKE_PROGRAM={ninja}', '-DCRD_PUBLIC_CHECKS=ON')
            self.assertNotEqual(code, 0)
            self.assertIn('crd_public_header_exclude(mod mod/missing.hpp)', output)
            self.assertIn('does not exist; remove the stale exclusion', output.replace('\n  ', ' '))

    def test_package_consumer_checks_layout_profile_relocatability_and_inherited_flags(self):
        self.assertEqual(package_consumer.expected_values({'build_type': 'Release', 'defines': {
            'CRD_ENABLE_ASSERTS': 'OFF', 'CRD_ENABLE_PROFILING': 'OFF'}}),
            {'asserts': 0, 'profiling': 0, 'debug': 0, 'release': 1, 'log_min_level': 2})
        self.assertEqual(package_consumer.expected_values({'build_type': 'Debug', 'defines': {
            'CRD_ENABLE_ASSERTS': 'ON', 'CRD_ENABLE_PROFILING': '1', 'CRD_LOG_MIN_LEVEL': 'Warn'}}),
            {'asserts': 1, 'profiling': 1, 'debug': 1, 'release': 0, 'log_min_level': 3})
        self.assertEqual(package_consumer.parse_output('version=0.1.0 0.1.0\nasserts=1\nplatform=windows compiler=msvc\n'),
                         {'version': '0.1.0', 'asserts': '1', 'platform': 'windows', 'compiler': 'msvc'})
        profile = package_consumer.profile_from_preset('win-debug')
        self.assertEqual((profile['generator'], profile['cxx_compiler'], profile['build_type']), ('Ninja', 'cl', 'Debug'))
        self.assertEqual(profile['defines']['CRD_ENABLE_PROFILING'], 'ON')
        self.assertNotIn('CMAKE_BUILD_TYPE', profile['defines'])
        arguments = package_consumer.cmake_arguments(profile, {'CRD_MODULES': 'core'})
        self.assertIn('-DCRD_ENABLE_PROFILING=ON', arguments)
        self.assertIn('-DCRD_MODULES=core', arguments)
        self.assertNotIn('-DCRD_ENABLE_PROFILING=ON',
                         package_consumer.cmake_arguments(profile, {}, build_type='Release', switches=False))
        with tempfile.TemporaryDirectory() as temp:
            prefix = Path(temp) / 'prefix'
            shutil.copytree(package_consumer.CORE_INCLUDE, prefix / 'include')
            (prefix / 'include/crd/core/build_config.hpp.in').unlink()
            (prefix / 'include/crd/core/build_config.hpp').write_text('#pragma once\n', encoding='utf-8')
            cmake_dir = prefix / 'lib/cmake/Cerid'
            cmake_dir.mkdir(parents=True)
            for name in ('CeridConfig.cmake', 'CeridConfigVersion.cmake', 'CeridTargets.cmake'):
                (cmake_dir / name).write_text('get_filename_component(_IMPORT_PREFIX "${CMAKE_CURRENT_LIST_FILE}" PATH)\n',
                                              encoding='utf-8')
            (prefix / 'lib/crd-core.lib').write_bytes(b'!<arch>\n')
            problems, libraries = package_consumer.check_layout(prefix)
            self.assertEqual((problems, libraries), ([], ['crd-core.lib']))
            self.assertEqual(package_consumer.relocatability_problems(prefix, [Path(temp) / 'source', Path(temp) / 'build']), [])
            source = Path(temp) / 'Source Tree'
            (cmake_dir / 'CeridTargets.cmake').write_text(
                f'set_target_properties(Cerid::core PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "{source}/include")\n',
                encoding='utf-8')
            problems = package_consumer.relocatability_problems(prefix, [str(source).upper() if os.name == 'nt' else source])
            self.assertEqual(len(problems), 1, problems)
            self.assertIn('CeridTargets.cmake names', problems[0])
            (prefix / 'include/crd/core/build_config.hpp.in').write_text('', encoding='utf-8')
            (prefix / 'include/crd/core/core.hpp').unlink()
            problems, _ = package_consumer.check_layout(prefix)
            self.assertEqual(problems, ['missing include/crd/core/core.hpp',
                                        'installed include/crd/core/build_config.hpp.in',
                                        'public header not installed: crd/core/core.hpp'])
            build = Path(temp) / 'consumer-build'
            build.mkdir()
            (build / 'compile_commands.json').write_text(json.dumps([
                {'directory': str(build), 'file': 'main.cpp', 'command': 'cl.exe /nologo /W4 /WX /EHsc -c main.cpp'}]),
                encoding='utf-8')
            self.assertEqual(package_consumer.compile_command_flags(build), ['/W4', '/WX'])
            (build / 'compile_commands.json').write_text(json.dumps([
                {'directory': str(build), 'file': 'main.cpp', 'arguments': ['g++', '-O2', '-Wshadow', '-c', 'main.cpp']}]),
                encoding='utf-8')
            self.assertEqual(package_consumer.compile_command_flags(build), [])

    def test_header_check_commands_write_only_into_the_scratch_directory(self):
        build = 'D:/repo/build' if os.name == 'nt' else '/repo/build'
        scratch = Path('D:/scratch' if os.name == 'nt' else '/scratch')
        msvc = {'directory': build, 'file': 'D:/repo/src/a.cpp', 'output': 'obj/a.cpp.obj',
                'arguments': ['C:/tools/cl.exe', '/nologo', '/TP', '-ID:/repo/include', '/Yucrd/core/pch.hpp',
                              f'/Fp{build}/pch/x.pch', f'/Fo{build}/obj/a.cpp.obj', f'/Fd{build}/lib.pdb',
                              '/Zi', '-c', 'D:/repo/src/a.cpp']}
        entry = tidy_gate.synthesize(msvc, 'D:/repo/include/crd/x.hpp', 'msvc')
        self.assertIsNotNone(entry)
        original = check_headers.outputs_named(msvc['arguments'], 'msvc')
        self.assertTrue(original and all(path.startswith(build) for path in original), original)
        arguments, outputs = check_headers.redirect_outputs(check_headers.arguments_of(entry), 'msvc', scratch, 'x_hpp')
        named = check_headers.outputs_named(arguments, 'msvc')
        self.assertEqual(sorted(named), sorted(outputs))
        self.assertTrue(named and all(Path(path).parent == scratch for path in named), named)
        self.assertFalse([token for token in arguments if token.startswith(('/Fp', '/Yu', '-Fp', '-Yu'))], arguments)
        self.assertIn('D:/repo/include/crd/x.hpp', arguments)
        gnu = {'directory': build, 'file': '/repo/src/a.cpp', 'output': 'obj/a.cpp.o',
               'arguments': ['/usr/bin/g++', '-I/repo/include', '-include', f'{build}/cmake_pch.hxx', '-Winvalid-pch',
                             '-MD', '-MT', 'obj/a.cpp.o', '-MF', 'obj/a.cpp.o.d', '-o', 'obj/a.cpp.o', '-c',
                             '/repo/src/a.cpp']}
        entry = tidy_gate.synthesize(gnu, '/repo/include/crd/y.hpp', 'gnu')
        arguments, outputs = check_headers.redirect_outputs(check_headers.arguments_of(entry), 'gnu', scratch, 'y_hpp')
        self.assertEqual(check_headers.outputs_named(gnu['arguments'], 'gnu'), ['obj/a.cpp.o.d', 'obj/a.cpp.o'])
        self.assertEqual(check_headers.outputs_named(arguments, 'gnu'), outputs)
        self.assertEqual(outputs, [str(scratch / 'y_hpp.o')])
        for flag in ('-MD', '-MT', '-MF', '-include'):
            self.assertNotIn(flag, arguments)
        self.assertEqual(arguments[-1], '/repo/include/crd/y.hpp')
        self.assertIn('-x', arguments)


class TestInstruments(unittest.TestCase):
    """REPO.DEV.9: the bounded fuzz harness registration, the fuzz driver and the instrument policy."""

    def test_fuzz_target_registration_requires_a_committed_corpus(self):
        cmake = shutil.which('cmake')
        ninja = shutil.which('ninja')
        if cmake is None or ninja is None:
            self.skipTest('CMake and Ninja are required')
        with tempfile.TemporaryDirectory() as temp:
            project = Path(temp) / 'project'
            (project / 'cmake').mkdir(parents=True)
            shutil.copyfile(ROOT / 'cmake/CrdFuzz.cmake', project / 'cmake/CrdFuzz.cmake')
            (project / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(fixture LANGUAGES NONE)\n'
                'option(CRD_ENABLE_FUZZER "" OFF)\n'
                'list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")\ninclude(CrdFuzz)\n'
                'add_library(crd-fuzz-harness INTERFACE)\n'
                'crd_fuzz_target(crd-fuzz-fixture SOURCES fixture.cpp LIBRARIES crd-fuzz-harness\n'
                '    CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")\n', encoding='utf-8')
            result = subprocess.run([cmake, '-S', str(project), '-B', str(Path(temp) / 'build'), '-G', 'Ninja',
                                     f'-DCMAKE_MAKE_PROGRAM={ninja}'], capture_output=True, text=True, timeout=300)
            self.assertNotEqual(result.returncode, 0)
            output = ' '.join((result.stdout + result.stderr).split())
            self.assertIn('corpus directory', output)
            self.assertIn('does not exist; the committed corpus is the regression set', output)

    def test_fuzz_driver_bounds_every_run_and_names_its_corpora(self):
        self.assertEqual(sorted(fuzz_driver.TARGETS), ['ceir-binary', 'ceir-text', 'ckir-binary', 'ckir-text'])
        for name, target in fuzz_driver.TARGETS.items():
            corpus = ROOT / target['corpus']
            self.assertTrue(corpus.is_dir(), f'{name}: committed corpus {corpus} is missing')
            self.assertTrue(any(corpus.iterdir()), f'{name}: the committed corpus is empty')
            for path in corpus.iterdir():
                self.assertFalse(path.suffix, f'{name}: corpus files carry no extension ({path.name})')
        arguments = fuzz_driver.limit_arguments(fuzz_driver.LIMITS)
        for flag in ('-max_len=', '-timeout=', '-rss_limit_mb=', '-malloc_limit_mb='):
            self.assertTrue(any(argument.startswith(flag) for argument in arguments), flag)
        self.assertLessEqual(fuzz_driver.LIMITS['max_len'], 64 * 1024)
        stats = fuzz_driver.parse_stats('INFO: seed\nstat::number_of_executed_units: 1234\nstat::average_exec_per_sec: 56\n'
                                        'stat::new_units_added: 7\nstat::peak_rss_mb: 89\n')
        self.assertEqual(stats, {'number_of_executed_units': 1234, 'average_exec_per_sec': 56, 'new_units_added': 7,
                                 'peak_rss_mb': 89})
        with tempfile.TemporaryDirectory() as temp:
            artifacts = Path(temp) / 'artifacts'
            artifacts.mkdir()
            for name in ('crash-abc', 'timeout-def', 'oom-123', 'fuzz-0.log', 'notes.txt'):
                (artifacts / name).write_bytes(b'x')
            self.assertEqual([path.name for path in fuzz_driver.artifacts_in(artifacts)],
                             ['crash-abc', 'oom-123', 'timeout-def'])
            with self.assertRaises(fuzz_driver.FuzzError):
                fuzz_driver.executable(Path(temp), 'ceir-text', libfuzzer=True)
            with self.assertRaises(fuzz_driver.FuzzError):
                fuzz_driver.targets_of('nope')
        self.assertEqual(fuzz_driver.targets_of('all'), list(fuzz_driver.TARGETS))

    def test_instrument_options_fail_the_configure_instead_of_warning(self):
        text = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertNotIn('UBSan not supported on MSVC, ignoring', text)
        self.assertIn('-fno-sanitize-recover=undefined', text)
        self.assertIn('CRD_ENABLE_TSAN and CRD_ENABLE_ASAN are exclusive', text)
        self.assertIn('CRD_ENABLE_FUZZER needs clang', text)
        header = (ROOT / 'engine/foundation/jobs/src/sanitizer_fibers.hpp').read_text(encoding='utf-8')
        for symbol in ('__sanitizer_start_switch_fiber', '__sanitizer_finish_switch_fiber', '__tsan_create_fiber',
                       '__tsan_switch_to_fiber', '__tsan_destroy_fiber'):
            self.assertIn(symbol, header)
        sites = {'engine/foundation/jobs/src/counter.cpp': 1, 'engine/foundation/jobs/src/worker_pool.cpp': 2}
        for path, count in sites.items():
            source = (ROOT / path).read_text(encoding='utf-8')
            self.assertEqual(source.count('fiber_switch('), count, path)
            self.assertEqual(source.count('sanitizer_switch_begin('), count, path)
            self.assertEqual(source.count('sanitizer_switch_end('), count, path)


class Provenance(unittest.TestCase):
    """REPO.DEV.10: generated-source provenance, the license manifest and the registry/systems-map route."""

    def test_this_checkout_passes_every_provenance_guard(self):
        self.assertEqual(provenance.check(ROOT, ROOT / 'scripts/generated-sources.json'), [])
        registry = json.loads((ROOT / 'cmake/pins.json').read_text(encoding='utf-8'))
        committed = (ROOT / 'docs/generated/dependency-licenses.md').read_bytes().replace(b'\r\n', b'\n')
        self.assertEqual(license_manifest.render(registry).encode('utf-8'), committed)
        self.assertEqual(master.registry_map_errors(), [])

    def test_changed_bytes_unlisted_markers_missing_generators_and_owners_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'engine').mkdir()
            (root / 'scripts').mkdir()
            generated = root / 'engine' / 'table.hpp'
            original = '// GENERATED by scripts/gen_table.py\nint t = 1;\n'
            generated.write_text(original, encoding='utf-8')
            generator = root / 'scripts' / 'gen_table.py'
            generator.write_text('print(1)\n', encoding='utf-8')
            manifest = root / 'scripts' / 'generated-sources.json'

            def write(entries, **extra):
                data = {'schema': provenance.SCHEMA, 'entries': entries}
                data.update(extra)
                manifest.write_text(json.dumps(data), encoding='utf-8')

            def errors():
                return provenance.check(root, manifest)

            entry = {'path': 'engine/table.hpp', 'generator': 'scripts/gen_table.py', 'status': 'reproducible',
                     'sha256': provenance.sha256(generated)}
            write([entry])
            self.assertEqual(errors(), [])
            generated.write_text(original.replace('1;', '2;'), encoding='utf-8')
            self.assertTrue(any('bytes changed' in error for error in errors()), errors())
            self.assertEqual(provenance.refresh(root, manifest, ['engine']), ['engine/table.hpp'])  # a directory
            self.assertEqual(errors(), [])
            generated.write_text(original, encoding='utf-8')
            self.assertTrue(any('bytes changed' in error for error in errors()), errors())
            self.assertEqual(provenance.refresh(root, manifest, ['engine/table.hpp']), ['engine/table.hpp'])
            self.assertEqual(errors(), [])
            with self.assertRaises(SystemExit):
                provenance.refresh(root, manifest, ['engine/absent.hpp'])
            write([entry])
            self.assertEqual(errors(), [])
            marker = root / 'engine' / 'refs.inc'
            marker.write_text('// GENERATED — scipy reference vectors\nconst int r[] = {1};\n', encoding='utf-8')
            self.assertTrue(any('carries a generation marker' in error for error in errors()), errors())
            write([entry], not_generated={'engine/refs.inc': 'a hand-written table that opens with the word'})
            self.assertEqual(errors(), [])
            write([entry], not_generated={'engine/refs.inc': ''})
            self.assertTrue(any('needs a reason' in error for error in errors()), errors())
            marker.unlink()
            generator.unlink()
            write([entry])
            self.assertTrue(any('not in the repository' in error for error in errors()), errors())
            write([dict(entry, status='generator-absent')])  # a declared property: no owner needed (REPO.DEV.11)
            self.assertEqual(errors(), [])
            write([dict(entry, status='invented', owner='REPO.DEV.11')])
            self.assertTrue(any('unknown status' in error for error in errors()), errors())
            write([dict(entry, status='generator-absent', sha256='0' * 64)])
            self.assertTrue(any('bytes changed' in error for error in errors()), errors())
            generator.write_text('print(1)\n', encoding='utf-8')
            write([dict(entry, status='drifted')])  # the one status whose truth is undecided names its row
            self.assertTrue(any('owning ROADMAP row' in error for error in errors()), errors())
            write([dict(entry, status='drifted', owner='HGP-4')])
            self.assertEqual(errors(), [])
            write([dict(entry, status='hand-maintained')])
            self.assertEqual(errors(), [])
            write([dict(entry, status='formatted')])
            self.assertTrue(any('names its formatter' in error for error in errors()), errors())
            write([dict(entry, status='formatted', format='clang-format')])
            self.assertEqual(errors(), [])
            generator.unlink()
            write([dict(entry, status='hand-maintained')])
            self.assertTrue(any('not in the repository' in error for error in errors()), errors())

    def test_marker_rule_matches_generated_files_and_not_prose_about_them(self):
        for head in ('// GENERATED by ceir_opgen.py from ops/arith.ceirop.toml - DO NOT EDIT\n',
                     '// AUTO-GENERATED by scripts/gen_aos_codelets.py — AoS codelets (DO NOT EDIT).\n',
                     '#pragma once\n\n// erk_tableaus.hpp -- GENERATED by scripts/gen_erk_tableaus.py.\n',
                     '// GENERATED — scipy sosfilt reference (v11-i). Plain C arrays.\n',
                     '// generated by gen_integrate_refs.py — scipy reference values\n',
                     '// ref_tt.inc — v14-k TT oracle expectations. GENERATED by scripts/v14k_tt_oracle.py\n',
                     '# GPU capability matrix\n> GENERATED by tools/gen_matrix.py\n'):
            with self.subTest(head=head):
                self.assertIsNotNone(provenance.MARKER.search(head))
        for head in ('// CEIR-2a+2b - the GENERATED-dialect gate: the arith dialect is DEFINED in TOML and\n',
                     '// crd-hesap-special v12-a — gamma gates. Reference vectors GENERATED from scipy.special\n',
                     '// window gates: vs scipy reference vectors (window_refs.inc,\n'
                     '// generated by gen_window_refs.py) to ~1e-12, plus ENBW sanity.\n',
                     '// The verifier is generated by the dialect table; this file is written by hand.\n'):
            with self.subTest(head=head):
                self.assertIsNone(provenance.MARKER.search(head))

    def test_registry_and_systems_map_are_held_to_one_set_of_names(self):
        build = ('crd_module(engine/foundation/core engine/core DEPENDS x TESTS foundation/core)\n'
                 'crd_module(tools/shader-cook NAME shader_cook HOST EXECUTABLES shader_cook DEPENDS core)\n'
                 'crd_module(sandbox HOST DEPENDS core)\n')
        systems = '| `core` | a | b |\n| `shader_cook` | a | b |\n| `sandbox` | a | b |\n'
        self.assertEqual(master.registered_modules(build), {'core', 'shader_cook', 'sandbox'})
        self.assertEqual(master.registry_map_errors(build, systems), [])
        missing = master.registry_map_errors(build, '| `core` | a | b |\n| `sandbox` | a | b |\n')
        self.assertEqual(len(missing), 1)
        self.assertIn('shader_cook', missing[0])
        extra = master.registry_map_errors(build, systems + '| `rhi` | a | b |\n')
        self.assertEqual(len(extra), 1)
        self.assertIn('unregistered module: rhi', extra[0])
        twice = master.registry_map_errors(build, systems + '| `core` | c | d |\n')
        self.assertEqual(len(twice), 1)
        self.assertIn('twice: core', twice[0])

    def test_license_manifest_render_follows_the_registry(self):
        registry = json.loads((ROOT / 'cmake/pins.json').read_text(encoding='utf-8'))
        rendered = license_manifest.render(registry)
        self.assertIn('| Catch2 | 3.7.1 | BSL-1.0 |', rendered)
        self.assertIn('assets/source/LICENSES.md', rendered)
        changed = json.loads(json.dumps(registry))
        changed['packages']['Catch2']['license'] = 'MIT'
        self.assertNotEqual(license_manifest.render(changed), rendered)
        self.assertIn('| Catch2 | 3.7.1 | MIT |', license_manifest.render(changed))
        for entry in changed['tools'].values():
            self.assertTrue(entry.get('license'), entry)


if __name__ == '__main__':
    unittest.main()
