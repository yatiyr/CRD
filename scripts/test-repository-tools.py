#!/usr/bin/env python3
"""Regression checks for hygiene rejection and CMake's generated IDE model."""
from pathlib import Path
import importlib.util
import io
import re
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hygiene', ROOT / 'scripts/check-repository.py')
hygiene = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hygiene)


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


if __name__ == '__main__':
    unittest.main()
