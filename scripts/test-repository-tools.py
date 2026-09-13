#!/usr/bin/env python3
"""Regression checks for hygiene rejection and CMake's generated IDE model."""
from pathlib import Path
import importlib.util
import hashlib
import io
import os
import re
import shutil
import subprocess
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
            with patch.object(installer.urllib.request, 'urlopen', return_value=io.BytesIO(b'untrusted response')) as fetch:
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    installer.install(destination, None)
            request = fetch.call_args.args[0]
            self.assertEqual(request.full_url, installer.URL)
            self.assertEqual(request.get_header('User-agent'), installer.USER_AGENT)
            self.assertEqual(fetch.call_args.kwargs['timeout'], 60)
            self.assertFalse((destination / 'lib').exists())

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
            with patch.object(installer.urllib.request, 'urlopen', return_value=io.BytesIO(b'untrusted response')) as fetch:
                with self.assertRaisesRegex(ValueError, 'package checksum mismatch'):
                    installer.install(destination, None)
            request = fetch.call_args.args[0]
            self.assertEqual(request.full_url, installer.URL)
            self.assertEqual(request.get_header('User-agent'), installer.USER_AGENT)
            self.assertEqual(fetch.call_args.kwargs['timeout'], 60)
            self.assertFalse((destination / installer.OUTPUT).exists())

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


if __name__ == '__main__':
    unittest.main()
