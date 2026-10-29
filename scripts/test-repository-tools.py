#!/usr/bin/env python3
"""Regression checks for hygiene rejection and CMake's generated IDE model."""
from pathlib import Path
import contextlib
import importlib.util
import hashlib
import http.client
import io
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import urllib.error
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
mold_spec = importlib.util.spec_from_file_location('install_mold', ROOT / 'scripts/install-mold.py')
install_mold = importlib.util.module_from_spec(mold_spec)
mold_spec.loader.exec_module(install_mold)
sde_spec = importlib.util.spec_from_file_location('install_sde', ROOT / 'scripts/install-sde.py')
install_sde = importlib.util.module_from_spec(sde_spec)
sde_spec.loader.exec_module(install_sde)
cba_spec = importlib.util.spec_from_file_location('install_cba', ROOT / 'scripts/install-clang-build-analyzer.py')
install_cba = importlib.util.module_from_spec(cba_spec)
cba_spec.loader.exec_module(install_cba)
tidy_spec = importlib.util.spec_from_file_location('install_tidy', ROOT / 'scripts/install-clang-tidy.py')
install_tidy = importlib.util.module_from_spec(tidy_spec)
tidy_spec.loader.exec_module(install_tidy)
ninja_spec = importlib.util.spec_from_file_location('install_ninja', ROOT / 'scripts/install-ninja.py')
install_ninja = importlib.util.module_from_spec(ninja_spec)
ninja_spec.loader.exec_module(install_ninja)
msvc_spec = importlib.util.spec_from_file_location('msvc_dev_env', ROOT / 'scripts/msvc-dev-env.py')
msvc_dev_env = importlib.util.module_from_spec(msvc_spec)
msvc_spec.loader.exec_module(msvc_dev_env)
profile_spec = importlib.util.spec_from_file_location('build_profile', ROOT / 'scripts/build-profile.py')
build_profile = importlib.util.module_from_spec(profile_spec)
profile_spec.loader.exec_module(build_profile)
budget_spec = importlib.util.spec_from_file_location('include_budget', ROOT / 'scripts/check-include-budget.py')
include_budget = importlib.util.module_from_spec(budget_spec)
budget_spec.loader.exec_module(include_budget)
cache_spec = importlib.util.spec_from_file_location('compiler_cache', ROOT / 'scripts/compiler-cache.py')
compiler_cache = importlib.util.module_from_spec(cache_spec)
cache_spec.loader.exec_module(compiler_cache)
mirror_spec = importlib.util.spec_from_file_location('source_mirror', ROOT / 'scripts/source-mirror.py')
source_mirror = importlib.util.module_from_spec(mirror_spec)
mirror_spec.loader.exec_module(source_mirror)
wait_spec = importlib.util.spec_from_file_location('wait_for', ROOT / 'scripts/wait-for.py')
wait_for = importlib.util.module_from_spec(wait_spec)
wait_spec.loader.exec_module(wait_for)
consumer_spec = importlib.util.spec_from_file_location('package_consumer', ROOT / 'scripts/test-package-consumer.py')
package_consumer = importlib.util.module_from_spec(consumer_spec)
consumer_spec.loader.exec_module(package_consumer)
headers_spec = importlib.util.spec_from_file_location('check_headers', ROOT / 'scripts/check-headers.py')
check_headers = importlib.util.module_from_spec(headers_spec)
headers_spec.loader.exec_module(check_headers)
fuzz_spec = importlib.util.spec_from_file_location('fuzz_driver', ROOT / 'scripts/fuzz.py')
fuzz_driver = importlib.util.module_from_spec(fuzz_spec)
fuzz_spec.loader.exec_module(fuzz_driver)
host_check_spec = importlib.util.spec_from_file_location('fuzz_host_check', ROOT / 'scripts/fuzz-host-check.py')
fuzz_host_check = importlib.util.module_from_spec(host_check_spec)
host_check_spec.loader.exec_module(fuzz_host_check)
tsan_host_check_spec = importlib.util.spec_from_file_location('tsan_host_check', ROOT / 'scripts/tsan-host-check.py')
tsan_host_check = importlib.util.module_from_spec(tsan_host_check_spec)
tsan_host_check_spec.loader.exec_module(tsan_host_check)
provenance_spec = importlib.util.spec_from_file_location('check_generated', ROOT / 'scripts/check-generated.py')
provenance = importlib.util.module_from_spec(provenance_spec)
provenance_spec.loader.exec_module(provenance)
license_spec = importlib.util.spec_from_file_location('gen_license_manifest', ROOT / 'scripts/gen_license_manifest.py')
license_manifest = importlib.util.module_from_spec(license_spec)
license_spec.loader.exec_module(license_manifest)
cold_spec = importlib.util.spec_from_file_location('check_cold_acquisition', ROOT / 'scripts/check-cold-acquisition.py')
cold_acquisition = importlib.util.module_from_spec(cold_spec)
cold_spec.loader.exec_module(cold_acquisition)
allman_spec = importlib.util.spec_from_file_location('check_allman_braces', ROOT / 'scripts/check-allman-braces.py')
allman = importlib.util.module_from_spec(allman_spec)
allman_spec.loader.exec_module(allman)
genmc_install_spec = importlib.util.spec_from_file_location('install_genmc', ROOT / 'scripts/install-genmc.py')
install_genmc = importlib.util.module_from_spec(genmc_install_spec)
genmc_install_spec.loader.exec_module(install_genmc)
genmc_models_spec = importlib.util.spec_from_file_location('check_genmc_models', ROOT / 'scripts/check-genmc-models.py')
genmc_models = importlib.util.module_from_spec(genmc_models_spec)
genmc_models_spec.loader.exec_module(genmc_models)
compile_commands_spec = importlib.util.spec_from_file_location('check_compile_commands',
                                                               ROOT / 'scripts/check-compile-commands.py')
compile_commands = importlib.util.module_from_spec(compile_commands_spec)
compile_commands_spec.loader.exec_module(compile_commands)
allocation_results_spec = importlib.util.spec_from_file_location('check_allocation_results',
                                                                 ROOT / 'scripts/check-allocation-results.py')
allocation_results = importlib.util.module_from_spec(allocation_results_spec)
allocation_results_spec.loader.exec_module(allocation_results)
runbooks_spec = importlib.util.spec_from_file_location('check_diag_runbooks', ROOT / 'scripts/check-diag-runbooks.py')
runbooks = importlib.util.module_from_spec(runbooks_spec)
runbooks_spec.loader.exec_module(runbooks)
source_routes_spec = importlib.util.spec_from_file_location('check_source_routes',
                                                            ROOT / 'scripts/check-source-routes.py')
source_routes = importlib.util.module_from_spec(source_routes_spec)
source_routes_spec.loader.exec_module(source_routes)
isa_spec = importlib.util.spec_from_file_location('check_isa_isolation', ROOT / 'scripts/check-isa-isolation.py')
isa_isolation = importlib.util.module_from_spec(isa_spec)
isa_spec.loader.exec_module(isa_isolation)
sys.path.insert(0, str(ROOT / 'scripts'))
import pins as pins_registry
from cerid_dev import tidy as tidy_gate  # noqa: E402  # noqa: E402  (scripts/pins.py; the installers import the same module)


class MasterPlanSequence(unittest.TestCase):
    @staticmethod
    def rows(*states):
        return [(f'S{i}', [str(i), f'**S{i}**', state, 'Contract', '—', 'Evidence'])
                for i, state in enumerate(states, 1)]

    def test_blocked_review_and_partial_are_not_bypassed(self):
        for state in ('Blocked', 'Review', 'Partial', 'Open', 'In progress'):
            with self.subTest(state=state):
                rows = self.rows('Done', 'Recorded', state, 'Open')
                self.assertEqual(master.first_unfinished(rows)[0], 'S3')
                self.assertEqual(master.sequence_errors(rows, ['S3']), [])
                self.assertTrue(master.sequence_errors(rows, ['S4']))

    def test_later_state_is_retired(self):
        self.assertNotIn('Later', master.STATES)

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
        original = 'void codelet2(T a) noexcept\n{}\n'
        self.assertEqual(canonicalize(original), '// NOLINTNEXTLINE(readability-function-size) -- Generated scheduled '
                                                 'DAG; retain fused scope.\n' + original)
        self.assertEqual(canonicalize(canonicalize(original)), canonicalize(original))
        with self.assertRaisesRegex(ValueError, 'Unbalanced'):
            canonicalize('const T a = f(x, b = 1;')

    def test_fft_codegen_split_moves_codelets_into_units_in_the_scheduled_order(self):
        from fft_codegen_split import split_kernels

        def codelet(name, prefix='CRD_FFT_CODELET_INLINE '):
            return [f'// {name} codelet. GENERATED.',
                    '// NOLINTNEXTLINE(readability-function-size) -- Generated scheduled DAG.',
                    f'{prefix}void {name}(const C* in,',
                    '    C* out, crd::usize b) noexcept',
                    '{',
                    '    using V = crd::hesap::fft::lanes::Lane<crd::f64>;',
                    '    namespace simd = crd::hesap::fft::lanes;',
                    '    for (crd::usize t = 0; t + 4 <= b; t += 4)',
                    '    {',
                    '        const crd::usize q = t >> 3;',
                    '        V n0r;',
                    '        V n0i; simd::load(in + t, n0r, n0i);',
                    '        const V n1r = n0r + n0i;',
                    '        const V n2r = n1r * n0r;',
                    '        { const V w = V(q);',
                    '          simd::store(out + t, n2r * w, n1r); }',
                    '    }',
                    '}']

        opening, closing = 'namespace gen::inline L {', '} // namespace gen::inline L'
        source = '\n'.join(['#pragma once', opening] + codelet('codelet2') + codelet('codelet4') + [closing, ''])
        header, units = split_kernels(source, 10, 2)
        # The header keeps the codelets' comments and declarations; each unit defines one codelet, its comment kept.
        self.assertEqual(header, '\n'.join(['#pragma once', opening,
                                            '// codelet2 codelet. GENERATED.', 'void codelet2(const C* in,',
                                            '    C* out, crd::usize b) noexcept;',
                                            '// codelet4 codelet. GENERATED.', 'void codelet4(const C* in,',
                                            '    C* out, crd::usize b) noexcept;', closing, '']))
        for unit, name in zip(units, ('codelet2', 'codelet4')):
            self.assertTrue(unit.startswith(opening + '\n') and unit.endswith(closing + '\n'))
            self.assertIn(f'// {name} codelet. GENERATED.\n// NOLINTNEXTLINE(readability-function-size) -- Generated '
                          f'scheduled DAG.\nvoid {name}(', unit)
        with self.assertRaisesRegex(ValueError, '2 codelets cannot fill 3 units'):
            split_kernels(source, 10, 3)

        # Every body runs in parts templated on the lane: the widest Lane over the whole tiles it holds, then a
        # TileLane over an odd last tile (at avx512, where a Lane holds two).
        single = '\n'.join([opening] + codelet('codelet2') + [closing, ''])
        drive = ('    const crd::usize tiles = b - b % lanes::kTile<crd::f64>;\n'
                 '    const crd::usize wide = tiles - tiles % lanes::kWidth<V>;\n'
                 '    for (crd::usize t = 0; t < wide; t += lanes::kWidth<V>)\n    {\n')
        _, (whole,) = split_kernels(single, 10, 1)
        self.assertIn('template <typename V> CRD_NOINLINE static void codelet2_part0([[maybe_unused]] const C* in, '
                      '[[maybe_unused]] C* out,\n    [[maybe_unused]] crd::usize b, [[maybe_unused]] crd::usize t) '
                      'noexcept\n{\n    namespace simd = crd::hesap::fft::lanes;\n    const crd::usize q = t >> 3;\n',
                      whole)
        self.assertIn('    using V = lanes::Lane<crd::f64>;\n' + drive + '        codelet2_part0<V>(in, out, b, t);\n'
                      '    }\n    if constexpr (lanes::kPairedTiles<crd::f64>)\n    {\n'
                      '        using H = lanes::TileLane<crd::f64>;\n        if (wide < tiles)\n        {\n'
                      '            codelet2_part0<H>(in, out, b, wide);\n        }\n    }\n}', whole)
        _, (split,) = split_kernels(single, 3, 1)
        self.assertIn('crd::usize t, V* sp) noexcept', split)
        self.assertIn('crd::usize t, const V* sp) noexcept', split)
        self.assertIn('    alignas(64) V sp[2];\n' + drive + '        codelet2_part0<V>(in, out, b, t, sp);\n'
                      '        codelet2_part1<V>(in, out, b, t, sp);\n    }\n', split)
        self.assertIn('        alignas(64) H sh[2];\n        if (wide < tiles)\n        {\n'
                      '            codelet2_part0<H>(in, out, b, wide, sh);\n'
                      '            codelet2_part1<H>(in, out, b, wide, sh);\n        }\n', split)
        # Every value crossing a cut is stored once by its part and read by each later part that uses it; the tile
        # local follows the part that reads it.
        self.assertEqual((split.count('    sp['), split.count('= sp[')), (2, 2))
        self.assertEqual(split.count('    const crd::usize q = t >> 3;'), 1)
        with self.assertRaisesRegex(ValueError, 'used before its definition'):
            split_kernels(single.replace('const V n1r = n0r + n0i;', 'const V n1r = n9r + n0i;'), 3, 1)
        with self.assertRaisesRegex(ValueError, 'codelet2: a 8-column step straddles a column block of 4'):
            split_kernels(single.replace('t >> 3', 't >> 2'), 10, 1)
        with self.assertRaisesRegex(ValueError, 'A tile of crd::f64 is 4 columns, not 8'):
            split_kernels(single.replace('t + 4 <= b; t += 4', 't + 8 <= b; t += 8'), 10, 1)
        # A codelet computes on the lanes only: neither its own lane-scalar edition nor fixed vector types.
        guarded = single.replace('noexcept\n{\n', 'noexcept\n{\n#ifdef CRD_FFT_GEN_SIMD_BODY\n', 1)
        fixed = single.replace('crd::hesap::fft::lanes::Lane<crd::f64>', 'crd::math::simd::Vec4d')
        for text in (guarded, fixed):
            with self.assertRaisesRegex(ValueError, 'unexpected kernel prologue'):
                split_kernels(text, 3, 1)

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

    def test_unregistered_capability_skip_is_neither_a_pass_nor_a_failure(self):
        # A device test that exits with its skip code on a lane without the device: reported as skipped, never failed.
        code, lines = self.gate(self.LANES, 'win-asan', {'A (DX12)': 'failed', 'B (DX12)': 'failed', 'V': 'skipped'}, 8)
        self.assertEqual(code, 0, lines)
        self.assertIn('skipped: 1;', lines[0])
        self.assertFalse(any('UNEXPECTED' in line for line in lines), lines)
        code, lines = self.gate(self.LANES, 'win-debug', {'C': 'passed', 'V': 'skipped'}, 0)
        self.assertEqual(code, 0, lines)
        self.assertIn('observed failures: 0; skipped: 1; results: 2', lines[0])

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
        self.assertEqual({entry['defect'] for entry in real['lanes']['linux-gcc-asan']}, {'TP-5'})
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
        self.assertEqual(visible - owned, {'win-tidy-local', 'linux-clang-fuzz', 'linux-clang-coverage'})
        for never_hosted in ('win-vs', 'win-relwithdebinfo', 'win-shipping-profile', 'win-debug-scalar',
                             'linux-gcc-debug-scalar'):
            self.assertEqual(self.tiers['presets'][never_hosted]['tier'], 'complete', never_hosted)
        lanes = gate.load_register(ROOT / 'docs/third-party-defects.md')['lanes']
        self.assertTrue(set(lanes) <= owned, set(lanes) - owned)

    def test_cached_lanes_follow_the_cache_rule_and_carry_every_cache_step(self):
        entries = self.tiers['presets']
        blocks = ci_tiers.workflow_jobs(self.workflow)
        cached = ci_tiers.env_preset_list(self.workflow, 'CRD_CACHED_PRESETS')
        self.assertIn('win-clang-cl-shipping', cached)
        self.assertIn('linux-clang-tsan', cached)
        for uncached in ('win-tidy', 'win-vs', 'win-release', 'win-relwithdebinfo', 'win-shipping',
                         'win-shipping-profile', 'win-debug-scalar', 'linux-gcc-debug-scalar'):
            self.assertNotIn(uncached, cached)
        self.assertEqual(ci_tiers.validate_cached_presets(entries, self.workflow, blocks), [])
        listed = re.search(r"^  CRD_CACHED_PRESETS: '[^']*'", self.workflow, re.M).group(0)

        def problems(text):
            return ci_tiers.validate_cached_presets(entries, text, ci_tiers.workflow_jobs(text))

        for added, expected in (('win-shipping', 'MSVC LTCG'), ('win-debug-scalar', 'cold-acquisition'),
                                ('linux-clang-fuzz', 'not a hosted preset'), ('win-debug', 'twice')):
            widened = self.workflow.replace(listed, listed[:-1] + added + " '")
            self.assertTrue(any(expected in p for p in problems(widened)), (added, problems(widened)))
        shipping = blocks['windows-clang-cl-shipping']
        without_stats = self.workflow.replace(shipping, shipping.replace('- name: Compiler cache statistics\n',
                                                                         '- name: Something else\n'))
        self.assertEqual(problems(without_stats), ["job windows-clang-cl-shipping builds cached preset "
                                                   "win-clang-cl-shipping but lacks the step 'Compiler cache statistics'"])
        self.assertEqual(shipping.count('--sccache-stats'), 1)
        without_bundle = self.workflow.replace(shipping, shipping.replace('--sccache-stats', '--no-statistics'))
        self.assertEqual(problems(without_bundle), ["job windows-clang-cl-shipping builds cached preset "
                                                    "win-clang-cl-shipping but its evidence bundle gets no "
                                                    "--sccache-stats"])
        self.assertEqual(problems(self.workflow.replace(listed, '')), ['the workflow defines no CRD_CACHED_PRESETS'])

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
        unshared = self.workflow.replace("github.event_name == 'push' && format('push-{0}', github.ref) || ", '', 1)
        self.assertNotEqual(unshared, self.workflow)
        problems = ci_tiers.validate(self.tiers, self.presets, unshared)
        self.assertTrue(any('share one concurrency group per ref' in p for p in problems), problems)
        starving = self.workflow.replace('|| github.run_id }}', "|| 'all' }}", 1)
        self.assertNotEqual(starving, self.workflow)
        problems = ci_tiers.validate(self.tiers, self.presets, starving)
        self.assertTrue(any('keep their own concurrency group' in p for p in problems), problems)
        ungated = self.workflow.replace("if: needs.preflight.outputs.presets_linux_gcc != '[]'", 'if: true')
        problems = ci_tiers.validate(self.tiers, self.presets, ungated)
        self.assertTrue(any('linux-gcc must be gated' in p for p in problems), problems)
        classes = json.loads(json.dumps(self.tiers['classes']))
        classes[-1]['patterns'] = ['engine/**']
        problems = ci_tiers.validate(self.mutated(classes=classes), self.presets, self.workflow)
        self.assertTrue(any('catch-all' in p for p in problems), problems)

    def test_other_workflows_are_dispatch_only_experiments(self):
        # A diagnostic preset may run hosted only in a workflow that never joins a tier: one triggered only by
        # workflow_dispatch (the DIAG.10a host check), which never cancels a running experiment.
        workflows = ROOT / '.github/workflows'
        self.assertEqual(ci_tiers.validate_experiments(workflows, workflows / 'ci.yml'), [])
        experiment = (workflows / 'host-check.yml').read_text(encoding='utf-8')
        self.assertIn('cmake --preset linux-clang-fuzz', experiment)
        self.assertEqual(ci_tiers.validate_experiment('host-check.yml', experiment), [])
        for trigger in ('  push:\n    branches: [main]\n', "  schedule:\n    - cron: '0 3 * * *'\n",
                        '  pull_request:\n'):
            problems = ci_tiers.validate_experiment('host-check.yml', experiment.replace('on:\n', 'on:\n' + trigger, 1))
            self.assertTrue(any('only by workflow_dispatch' in p for p in problems), (trigger, problems))
        cancelling = experiment.replace('cancel-in-progress: false', 'cancel-in-progress: true')
        problems = ci_tiers.validate_experiment('host-check.yml', cancelling)
        self.assertTrue(any('must not cancel' in p for p in problems), problems)

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
        self.assertEqual(change['jobs']['windows'], ['win-debug', 'win-release', 'win-asan'])
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

    def test_real_git_ranges_resolve_through_their_diff(self):
        # A repository of its own, so the case runs in every clone: the hosted checkout is one commit deep, and the
        # former case (a0419cf..18651d5 of this history) skipped on every hosted run. The reason must name the class
        # the diff found; the resolver's conservative fallback also says "complete", so the tier alone proves nothing.
        with tempfile.TemporaryDirectory() as temp:
            repo = Path(temp)

            def git(*arguments):
                # No automatic maintenance: since Git 2.47 a commit starts it detached, and it wrote into .git while
                # the temporary directory was being removed (hosted linux-gcc-shipping, run 40067359705).
                completed = subprocess.run(['git', '-c', 'user.name=t', '-c', 'user.email=t@t', '-c',
                                            'commit.gpgsign=false', '-c', 'maintenance.auto=false', *arguments],
                                           cwd=str(repo), capture_output=True, text=True, check=False)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                return completed.stdout.strip()

            git('init', '-q')
            (repo / 'docs').mkdir()
            (repo / 'docs/ROADMAP.md').write_text('a\n', encoding='utf-8')
            git('add', '.')
            git('commit', '-q', '-m', 'base')
            base = git('rev-parse', 'HEAD')
            (repo / 'docs/ROADMAP.md').write_text('b\n', encoding='utf-8')
            git('commit', '-q', '-am', 'docs')
            documentation = git('rev-parse', 'HEAD')
            (repo / 'CMakeLists.txt').write_text('project(x)\n', encoding='utf-8')
            git('add', '.')
            git('commit', '-q', '-m', 'build')
            build = git('rev-parse', 'HEAD')

            self.assertEqual(ci_tier.changed_paths(base, documentation, repo=repo), ['docs/ROADMAP.md'])
            docs = ci_tier.resolve(self.tiers, 'push', base, documentation,
                                   paths=ci_tier.changed_paths(base, documentation, repo=repo))
            self.assertEqual(docs['tier'], 'preflight', docs['reason'])
            paths = ci_tier.changed_paths(base, build, repo=repo)
            self.assertEqual(sorted(paths), ['CMakeLists.txt', 'docs/ROADMAP.md'])
            record = ci_tier.resolve(self.tiers, 'push', base, build, paths=paths)
            self.assertEqual(record['tier'], 'complete', record['reason'])
            self.assertIn('build-system', record['reason'])
            self.assertIsNone(ci_tier.changed_paths(base, 'f' * 40, repo=repo), 'an unknown head is not a diff')

    def test_push_base_is_the_newest_earlier_run_that_resolved_its_tier(self):
        # 2026-10-19: the push group replaces a pending run, so a push's `before` can be a revision no run ever
        # classified (39216534845 at 86f4b900 started no job); the base is the last head a preflight succeeded on.
        ok = [{'name': ci_tier.PREFLIGHT_JOB, 'conclusion': 'success'}, {'name': 'windows (win-debug)'}]
        jobs = {
            107: [{'name': ci_tier.PREFLIGHT_JOB, 'conclusion': 'success'}],  # newer than this run
            105: [],  # replaced while pending: no job ever started
            104: [{'name': ci_tier.PREFLIGHT_JOB, 'conclusion': 'cancelled'}],
            103: [{'name': 'Repository checks (ubuntu-24.04)', 'conclusion': 'success'}],  # no preflight
            102: ok,
            101: ok,
        }
        runs = [{'id': i, 'head_sha': 'sha%d' % i} for i in (107, 106, 105, 104, 103, 102, 101)]
        asked = []

        def jobs_of(run_id):
            asked.append(run_id)
            return jobs.get(run_id, [])

        self.assertEqual(ci_tier.covered_base(runs, 106, jobs_of), 'sha102')
        self.assertEqual(asked, [105, 104, 103, 102])  # this run and newer ones skipped; asked only until one qualifies
        self.assertEqual(ci_tier.covered_base(runs, 102, lambda run_id: jobs.get(run_id, [])), 'sha101')
        self.assertEqual(ci_tier.covered_base(runs[:5], 106, lambda run_id: jobs.get(run_id, [])), '')
        self.assertEqual(ci_tier.covered_base([], 106, lambda run_id: []), '')
        # An empty base is the conservative tier, never `before`.
        self.assertEqual(ci_tier.resolve(self.tiers, 'push', '', 'HEAD')['tier'], 'complete')
        with tempfile.TemporaryDirectory() as scratch:
            listing = Path(scratch) / 'runs.json'
            listing.write_text(json.dumps({'runs': [dict(r, jobs=jobs.get(r['id'], [])) for r in runs]}),
                               encoding='utf-8')
            output = Path(scratch) / 'out.txt'
            with contextlib.redirect_stdout(io.StringIO()):
                code = ci_tier.main(['base', '--repository', 'o/r', '--branch', 'main', '--current-run', '106',
                                     '--listing', str(listing), '--output', str(output)])
            self.assertEqual(code, 0)
            self.assertEqual(output.read_text(encoding='utf-8'), 'base=sha102\n')

    def test_preflight_takes_its_push_base_from_the_last_covered_run(self):
        blocks = ci_tiers.workflow_jobs(self.workflow)
        preflight = blocks['preflight']
        self.assertIn('name: %s' % ci_tier.PREFLIGHT_JOB, preflight)  # the job name the lookup recognises
        self.assertRegex(preflight, r'permissions:\s*\n\s*contents: read\s*\n\s*actions: read')
        self.assertIn('python scripts/ci-tier.py base --repository "$GITHUB_REPOSITORY"', preflight)
        self.assertNotIn('github.event.before', self.workflow)
        self.assertEqual(preflight.count('CRD_CI_BASE: ${{ steps.base.outputs.base }}'), 2)

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
        {'name': 'broken', 'properties': [{'name': 'LABELS', 'value': ['cerid.test.target=broken']}]},
        {'name': 'absent', 'properties': [{'name': 'LABELS', 'value': ['gpu']}]}]}

    def fixture(self, temp):
        build = Path(temp) / 'build'
        (build / 'Testing/Temporary').mkdir(parents=True)
        (build / 'CMakeFiles/4.3.2').mkdir(parents=True)
        (build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug\nCRD_SIMD_LEVEL:STRING=sse2\n'
                                              'CMAKE_GENERATOR:INTERNAL=Ninja\nOTHER:BOOL=ON\nCRD_ENABLE_ASAN:BOOL=ON\n'
                                              'CRD_ENABLE_TSAN:BOOL=OFF\n', encoding='utf-8')
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
            self.assertEqual(record['inventory'], {'tests': 3, 'labels': {'gpu': 2, 'cerid.test.target=broken': 1},
                                                   'resource_locks': {'crd_gpu_device': 1}})
            self.assertEqual(record['modes'], ['ASan'])
            results = record['results']
            self.assertEqual((results['tests'], results['passed'], results['failures'], results['skipped']), (3, 1, 1, 1))
            self.assertEqual(results['failed'][0]['name'], 'broken')
            self.assertEqual(results['failed'][0]['output_tail'], ['line1', 'line2', 'line3'])
            self.assertEqual([s['name'] for s in results['slowest']], ['slow', 'broken', 'absent'])
            self.assertEqual(results['labels'], {'gpu': {'tests': 2, 'passed': 1, 'failed': 0, 'skipped': 1}})
            self.assertEqual(record['adapters'], ['Name : Microsoft Basic Render Driver', 'DriverVersion : 10.0'])
            self.assertEqual(json.loads((out / 'conclusion.json').read_text(encoding='utf-8'))['status'], 'fail')
            self.assertTrue((out / 'ctest-junit.xml').is_file() and (out / 'inventory.json').is_file())
            markdown = ci_evidence.summary_markdown(record)
            self.assertIn('### win-debug: fail', markdown)
            self.assertIn('`broken` (0.5 s): boom', markdown)
            self.assertIn('3 executed: 1 passed, 1 failed, 1 skipped', markdown)
            self.assertIn('| Diagnostic modes | ASan |', markdown)
            self.assertIn('| By label | gpu 2 executed: 1 passed, 0 failed, 1 skipped |', markdown)
            passed = ci_evidence.bundle('win-debug', build, out, adapters=adapters, outcome='success', environ={})
            self.assertEqual(passed['status'], 'pass')
            self.assertEqual(ci_evidence.bundle('win-debug', build, out, outcome='failure', environ={})['status'], 'failure')

    def test_bundle_without_inputs_reports_what_is_missing(self):
        with tempfile.TemporaryDirectory() as temp:
            record = ci_evidence.bundle('none', Path(temp) / 'nowhere', Path(temp) / 'out', environ={})
            self.assertEqual(record['status'], 'missing')
            self.assertEqual(record['missing'], ['CMakeCache.txt', 'adapters', 'inventory', 'junit'])
            self.assertIn('Missing evidence: CMakeCache.txt, adapters, inventory, junit', ci_evidence.summary_markdown(record))

    def test_compare_takes_tests_that_passed_on_both_above_the_floor(self):
        def lane(root, preset, cases, modes=None):
            root.mkdir()
            body = ''.join('<testcase name="%s" time="%s" status="run">%s</testcase>\n' % (
                name, seconds, '' if passed else '<failure message="x"/>') for name, seconds, passed in cases)
            (root / 'ctest-junit.xml').write_text('<testsuite name="x">\n%s</testsuite>\n' % body, encoding='utf-8')
            record = {'preset': preset, 'results': {'duration_seconds': sum(c[1] for c in cases)}}
            if modes is not None:
                record['modes'] = modes
            (root / 'conclusion.json').write_text(json.dumps(record), encoding='utf-8')
            return root

        with tempfile.TemporaryDirectory() as temp:
            # Ratios 1, 2, 3 and 4; `fast` is under the floor on the reference and `flaky` failed there.
            mode = lane(Path(temp) / 'mode', 'win-asan', [('a', 2.0, True), ('b', 1.0, True), ('c', 3.0, True),
                                                          ('d', 6.0, True), ('fast', 1.0, True), ('flaky', 10.0, True),
                                                          ('broken', 9.0, False)], ['ASan'])
            reference = lane(Path(temp) / 'reference', 'win-debug', [('a', 2.0, True), ('b', 0.5, True),
                                                                     ('c', 1.0, True), ('d', 1.5, True),
                                                                     ('fast', 0.01, True), ('flaky', 1.0, False),
                                                                     ('broken', 1.0, True)])
            mode_junit, reference_junit = mode / 'ctest-junit.xml', reference / 'ctest-junit.xml'
            self.assertEqual(ci_evidence.slowdown(mode_junit, reference_junit),
                             {'tests': 4, 'p25': 1.0, 'median': 2.0, 'p75': 3.0, 'mode_seconds': 12.0,
                              'reference_seconds': 5.0})
            self.assertEqual(ci_evidence.compare_markdown(mode, reference),
                             '| win-asan (ASan) | win-debug (modes not recorded) | 4 | 2.00 | 1.00-3.00 | 12 s / 5 s '
                             '| 32 s / 7 s |')
            self.assertEqual(ci_evidence.slowdown(mode_junit, reference_junit, 5.0), {'tests': 0})

    def test_glob_patterns_match_segments_and_trees(self):
        matches = lambda pattern, path: bool(ci_tier.glob_regex(pattern).match(path))
        self.assertTrue(matches('**/*.md', 'a/b/c.md') and matches('**/*.md', 'c.md'))
        self.assertFalse(matches('*.md', 'a/c.md'))
        self.assertTrue(matches('docs/**', 'docs/x/y.json'))
        self.assertFalse(matches('CMakeLists.txt', 'engine/CMakeLists.txt'))
        self.assertTrue(matches('**', 'anything/at/all'))


class PinnedInputs(unittest.TestCase):
    """The pin registry, its guard, the acquisition helpers and the build-owned patch (docs/design/pinned-inputs.md)."""

    def test_cold_acquisition_checker_needs_an_empty_cache_and_every_added_pin(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            cache, build = root / 'cpm', root / 'build'
            self.assertTrue(cold_acquisition.cache_is_empty(cache))  # absent counts as cold
            cache.mkdir()
            self.assertTrue(cold_acquisition.cache_is_empty(cache))
            (cache / 'catch2' / 'origin').mkdir(parents=True)
            (cache / 'catch2' / 'origin' / 'CMakeLists.txt').write_text('x', encoding='utf-8')
            self.assertFalse(cold_acquisition.cache_is_empty(cache))  # a restored cache is not cold

            build.mkdir()
            pins = ROOT / 'cmake/pins.json'
            ok, lines = cold_acquisition.check_after(cache, build, pins)
            self.assertFalse(ok)  # no CMakeCache.txt: nothing was acquired
            (build / 'CMakeCache.txt').write_text('CPM_PACKAGES:INTERNAL=Catch2;glfw\n', encoding='utf-8')
            ok, lines = cold_acquisition.check_after(cache, build, pins)
            self.assertFalse(ok)  # glfw was added but is not in the cache
            self.assertTrue(any(line.startswith('glfw: not present') for line in lines), lines)
            (cache / 'glfw' / 'origin').mkdir(parents=True)
            (cache / 'glfw' / 'origin' / 'CMakeLists.txt').write_text('x', encoding='utf-8')
            ok, lines = cold_acquisition.check_after(cache, build, pins)
            self.assertTrue(ok, lines)
            self.assertTrue(any(line.startswith('not selected by this configuration:') and 'eigen' in line
                                for line in lines), lines)
            (build / 'CMakeCache.txt').write_text('CPM_PACKAGES:INTERNAL=Catch2;unpinned\n', encoding='utf-8')
            ok, lines = cold_acquisition.check_after(cache, build, pins)
            self.assertFalse(ok)  # an added package outside the registry fails

    def test_cold_acquisition_fetches_and_verifies_every_unselected_pin(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            archive = root / 'bench-1.0.tar.gz'
            archive.write_bytes(b'pinned bytes')
            digest = hashlib.sha256(b'pinned bytes').hexdigest()
            cache, build, fetched = root / 'cpm', root / 'build', root / 'fetched'
            (cache / 'used' / 'origin').mkdir(parents=True)
            (cache / 'used' / 'origin' / 'CMakeLists.txt').write_text('x', encoding='utf-8')
            build.mkdir()
            (build / 'CMakeCache.txt').write_text('CPM_PACKAGES:INTERNAL=used\n', encoding='utf-8')
            registry = {'packages': {'used': {'url': 'unused', 'sha256': '0' * 64},
                                     'bench': {'url': archive.as_uri(), 'file': 'bench-1.0.tar.gz',
                                               'sha256': digest, 'gated': 'CRD_BUILD_BENCH'}}}
            pins_file = root / 'pins.json'
            pins_file.write_text(json.dumps(registry), encoding='utf-8')

            ok, lines = cold_acquisition.check_after(cache, build, pins_file, fetched)
            self.assertTrue(ok, lines)
            self.assertTrue((fetched / 'bench-1.0.tar.gz').is_file())
            self.assertFalse((fetched / 'bench-1.0.tar.gz.partial').exists())
            self.assertTrue(any(line.startswith('bench: archive downloaded cold and verified') and
                                'CRD_BUILD_BENCH' in line for line in lines), lines)

            registry['packages']['bench']['sha256'] = 'f' * 64  # a drifted archive fails and leaves no file
            pins_file.write_text(json.dumps(registry), encoding='utf-8')
            ok, lines = cold_acquisition.check_after(cache, build, pins_file, fetched)
            self.assertFalse(ok, lines)
            self.assertFalse((fetched / 'bench-1.0.tar.gz').exists())
            self.assertFalse((fetched / 'bench-1.0.tar.gz.partial').exists())
            self.assertTrue(any(line.startswith('bench: archive NOT acquired') for line in lines), lines)

            ok, lines = cold_acquisition.check_after(cache, build, pins_file)  # without the option: named only
            self.assertTrue(ok, lines)

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
        self.assertEqual(self.workflow.count("python-version: '3.12'"), 13)
        for package in ('Catch2', 'glfw', 'zstd', 'stb', 'cgltf', 'MikkTSpace', 'imgui', 'eigen', 'OpenBLAS', 'tracy'):
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
        # The dispatch-only experiment workflows keep the same input rules as the CI workflow.
        workflows = ROOT / '.github/workflows'
        self.assertEqual(check_pins.validate_other_workflows(self.pins, workflows, workflows / 'ci.yml'), [])
        with tempfile.TemporaryDirectory() as temp:
            experiment = (workflows / 'host-check.yml').read_text(encoding='utf-8')
            (Path(temp) / 'host-check.yml').write_text(experiment.replace(pinned, 'actions/checkout@v4', 1)
                                                       .replace('runs-on: ubuntu-24.04', 'runs-on: ubuntu-latest'),
                                                       encoding='utf-8')
            problems = check_pins.validate_other_workflows(self.pins, temp, workflows / 'ci.yml')
            self.assertTrue(any(p.startswith('host-check.yml: workflow uses actions/checkout@v4') for p in problems),
                            problems)
            self.assertTrue(any('ubuntu-latest' in p for p in problems), problems)
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

    def test_download_retries_only_network_failures_a_bounded_number_of_times(self):
        good = b'pinned bytes'
        digest = hashlib.sha256(good).hexdigest()
        url = 'https://example.invalid/a.bin'
        missing = urllib.error.HTTPError(url, 404, 'Not Found', {}, None)
        busy = urllib.error.HTTPError(url, 503, 'Service Unavailable', {}, None)
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / 'a.bin'
            waits = []
            stalls = [TimeoutError('read timed out'), http.client.IncompleteRead(b'pin'), busy, io.BytesIO(good)]
            with patch.object(pins_registry.urllib.request, 'urlopen', side_effect=stalls), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(pins_registry.download(url, target, digest, 'a', sleep=waits.append), target)
            self.assertEqual(target.read_bytes(), good)
            self.assertEqual(waits, list(pins_registry.RETRY_WAITS))
            target.unlink()
            waits.clear()
            with patch.object(pins_registry.urllib.request, 'urlopen', side_effect=[missing]):
                with self.assertRaises(urllib.error.HTTPError):
                    pins_registry.download(url, target, digest, 'a', sleep=waits.append)
            self.assertEqual(waits, [])
            stalls = [TimeoutError('read timed out')] * (len(pins_registry.RETRY_WAITS) + 1)
            with patch.object(pins_registry.urllib.request, 'urlopen', side_effect=stalls), \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(TimeoutError):
                    pins_registry.download(url, target, digest, 'a', sleep=waits.append)
            self.assertEqual(len(waits), len(pins_registry.RETRY_WAITS))
            self.assertEqual(list(Path(temp).iterdir()), [])

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
                             {'location': None, 'requests': 10, 'hits': 6, 'misses': 3, 'non_cacheable': 1, 'errors': 0,
                              'reasons': {'/Fp': 1}})
            markdown = ci_evidence.summary_markdown(record)
            self.assertIn('5 edges in 2 invocations, span 4.4 s', markdown)
            self.assertIn('sccache 6 hits, 3 misses, 1 non-cacheable (/Fp 1)', markdown)
            # The hosted lanes cache through the GitHub Actions service: the bundle names the backend it used.
            stats.write_text(json.dumps(dict(self.STATS, cache_location='GHA, name: sccache, prefix: /sccache/')),
                             encoding='utf-8')
            hosted = ci_evidence.bundle('win-debug', build, Path(temp) / 'out', outcome='success', environ={},
                                        sccache=stats)
            self.assertEqual(hosted['build']['sccache']['location'], 'GHA, name: sccache, prefix: /sccache/')
            self.assertIn('(cache: GHA, name: sccache', ci_evidence.summary_markdown(hosted))
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
            # Ninja recompacts a long log to one line per output: the row is then what changed since the snapshot.
            snapshot = build_bench.ninja_snapshot(log)
            offset = build_bench.ninja_line_count(log)
            log.write_text('# ninja log v7\n0\t200\t1\tc.obj\t4\n100\t900\t1\tapp.exe\t5\n0\t500\t9\tnew.obj\t7\n'
                           '0\t1000\t1\ta.obj\t1\n500\t3000\t1\tb.obj\t2\n', encoding='utf-8')
            entries, recompacted = build_bench.ninja_row_entries(log, offset, snapshot)
            self.assertTrue(recompacted)
            self.assertEqual([e[2] for e in entries], ['new.obj'])
            self.assertEqual(build_bench.ninja_row_entries(log, 0, snapshot)[1], False)
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
        self.assertIn('linker `default`', build_bench.render_context([record]))
        linked = json.loads(json.dumps(record))
        linked['toolchain'].update({'CRD_LINKER': '/t/mold/mold', 'CRD_LINKER_THREADS': '2'})
        self.assertIn('linker `/t/mold/mold, 2 threads per link`', build_bench.render_context([linked]))
        self.assertEqual(build_bench.linker_label({'CRD_LINKER': '', 'CRD_LINKER_THREADS': '2'}), 'default')
        self.assertEqual(build_bench.linker_label({'CRD_LINKER': '/t/mold/mold'}), '/t/mold/mold')
        with self.assertRaises(SystemExit):
            build_bench.Board('win-debug', Path(temp) / 'b', 4, [], 'x', sccache='sccache', environ={})

    def test_teeth_rows_break_restore_and_settle_and_targets_scope_every_build(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'header.hpp'
            source.write_bytes(b'inline int f() { return 1; } // one\n')
            os.utime(source, ns=(1_600_000_000_000_000_000, 1_600_000_000_000_000_100))
            before = source.stat().st_mtime_ns
            with self.assertRaises(SystemExit):
                build_bench.Tooth(source, 'return 2;', 'return 3;').apply()
            tooth = build_bench.Tooth(source, 'return 1;', 'return 2;')
            tooth.apply()
            self.assertEqual(source.read_bytes(), b'inline int f() { return 2; } // one\n')
            tooth.restore()
            self.assertEqual(source.read_bytes(), b'inline int f() { return 1; } // one\n')
            self.assertNotEqual(source.stat().st_mtime_ns, before, 'the restore build must see the file as changed')
            tooth.settle()
            self.assertEqual(source.stat().st_mtime_ns, before, 'afterwards no other tree sees the cycle')
            board = build_bench.Board('win-debug', Path(temp) / 'b', 4, [], 'x', targets=['crd-memory-tests', 'all'],
                                      tooth=(source.name, 'return 1;', 'return 2;'), repo=Path(temp), log=lambda _: None)
            commands = []
            board.run = lambda command, cwd=None: (commands.append(command), SimpleNamespace(returncode=0))[1]
            self.assertEqual(board.teeth_break(), 0)
            self.assertIn(b'return 2;', source.read_bytes())
            self.assertEqual(board.teeth_restore(), 0)
            self.assertIn(b'return 1;', source.read_bytes())
            self.assertEqual(source.stat().st_mtime_ns, before)
            self.assertEqual(commands[0][-4:], ['--target', 'crd-memory-tests', '--target', 'all'])
            with self.assertRaises(SystemExit):
                board.teeth_restore()
            self.assertEqual(board.record['edits']['tooth'], [source.name, 'return 1;', 'return 2;'])
            self.assertEqual(board.record['targets'], ['crd-memory-tests', 'all'])
            # Every finished row is written at once, so a board stopped by a deadline keeps it; an applied tooth is
            # restored on the way out.
            saved = []
            stopped = build_bench.Board('win-debug', Path(temp) / 'fresh', 4, [], 'x', repo=Path(temp),
                                        tooth=(source.name, 'return 1;', 'return 2;'), log=lambda _: None,
                                        host=lambda: [])
            stopped.checkpoint = lambda record: saved.append([r['row'] for r in record['rows']])

            def row(name, action):
                if name == 'teeth-restore':
                    raise SystemExit('deadline')
                stopped.record['rows'].append({'row': name})
                if name == 'teeth-break':
                    stopped.tooth.apply()

            stopped.row = row
            with self.assertRaises(SystemExit):
                stopped.execute(['configure', 'cold', 'teeth-break', 'teeth-restore'])
            self.assertEqual(saved[-1], ['configure', 'cold', 'teeth-break'])
            self.assertIn(b'return 1;', source.read_bytes(), 'the tooth is restored when the board stops')
            self.assertEqual(source.stat().st_mtime_ns, before)

    def test_relink_all_deletes_every_linked_program_and_nothing_else(self):
        listing = ('tests/core/crd-core-tests: CXX_EXECUTABLE_LINKER__crd-core-tests_Debug\n'
                   'engine/core/libcrd-core.a: CXX_STATIC_LIBRARY_LINKER__crd-core_Debug\n'
                   'engine/core/CMakeFiles/crd-core.dir/src/assert.cpp.o: CXX_COMPILER__crd-core_unscanned_Debug\n'
                   'tools/plugin.so: CXX_SHARED_LIBRARY_LINKER__plugin_Debug\n'
                   'tools/module.so: CXX_MODULE_LIBRARY_LINKER__module_Debug\n'
                   'bin/app.exe: C_EXECUTABLE_LINKER__app_Debug\n'
                   'all: phony\n'
                   'build.ninja: RERUN_CMAKE\n')
        outputs = build_bench.linked_outputs(listing, Path('B'))
        self.assertEqual([p.as_posix() for p in outputs],
                         ['B/tests/core/crd-core-tests', 'B/tools/plugin.so', 'B/tools/module.so', 'B/bin/app.exe'])
        with tempfile.TemporaryDirectory() as temp:
            tree = Path(temp) / 'tree'
            (tree / 'tests/core').mkdir(parents=True)
            (tree / 'engine/core').mkdir(parents=True)
            program = tree / 'tests/core/crd-core-tests'
            archive = tree / 'engine/core/libcrd-core.a'
            for path in (program, archive):
                path.write_bytes(b'x')
            (tree / 'CMakeCache.txt').write_text('CMAKE_MAKE_PROGRAM:FILEPATH=/usr/bin/ninja\n', encoding='utf-8')
            self.assertEqual(build_bench.make_program(tree), '/usr/bin/ninja')
            board = build_bench.Board('linux-gcc-debug', tree, 4, [], 'x', repo=Path(temp), log=lambda _: None)
            commands = []
            board.run = lambda command, cwd=None: (commands.append(command), SimpleNamespace(returncode=0))[1]
            with patch.object(build_bench.subprocess, 'run', return_value=SimpleNamespace(stdout=listing)) as query:
                self.assertEqual(board.relink_all(), 0)
            self.assertEqual(query.call_args[0][0], ['/usr/bin/ninja', '-C', str(tree), '-t', 'targets', 'all'])
            self.assertFalse(program.exists(), 'the program is deleted so the build relinks it')
            self.assertTrue(archive.exists(), 'archives are not links: deleting them would recompile nothing but '
                                              'rebuild the archive step')
            self.assertEqual(commands[0][:2], ['cmake', '--build'])
            with patch.object(build_bench.subprocess, 'run', return_value=SimpleNamespace(stdout=listing)):
                with self.assertRaisesRegex(SystemExit, 'no linked program'):
                    board.relink_all()
            # A scoped board relinks only its targets' programs.
            program.write_bytes(b'x')
            scoped = build_bench.Board('linux-gcc-debug', tree, 4, [], 'x', repo=Path(temp), log=lambda _: None,
                                       targets=['crd-core-tests'])
            scoped.run = board.run
            self.assertEqual(scoped.relink_all(), 0)
            self.assertFalse(program.exists())
            self.assertEqual(commands[-1][-2:], ['--target', 'crd-core-tests'])
            (tree / 'CMakeCache.txt').write_text('CMAKE_MAKE_PROGRAM:FILEPATH=\n', encoding='utf-8')
            with self.assertRaisesRegex(SystemExit, 'CMAKE_MAKE_PROGRAM'):
                build_bench.make_program(tree)
        self.assertIn('relink-all', build_bench.ROWS)

    def test_boards_record_the_host_and_refuse_a_contended_one(self):
        gib = int(build_bench.GIB)
        before = {1: ('ComfyUI', 25 * gib, 100.0), 2: ('opera', gib // 2, 10.0), 3: ('idle', 0, None),
                  4: ('gone', 0, 1.0), 9: ('build-bench', 0, 1.0)}
        after = {1: ('ComfyUI', 25 * gib, 108.0), 2: ('opera', gib // 2, 11.0), 3: ('idle', 0, None),
                 4: ('reused', 0, 9.0), 5: ('new', 0, 50.0), 9: ('build-bench', 0, 99.0)}
        by_cpu, by_memory, cores = build_bench.busiest(before, after, 2.0, own_pids={9})
        self.assertEqual([(p['name'], p['cores']) for p in by_cpu], [('ComfyUI', 4.0), ('opera', 0.5), ('idle', 0.0)],
                         'a process seen once, a reused pid and the board itself are left out')
        self.assertEqual(by_memory[0]['gib'], 25.0)
        self.assertEqual(cores, 4.5)
        busy = {'host': 'windows', 'total_gib': 63.7, 'available_gib': 0.7, 'cores': cores, 'busiest': by_cpu}
        reasons = build_bench.contention(busy)
        self.assertEqual(len(reasons), 3)
        self.assertIn('0.7 of 63.7 GiB available', reasons[0])
        self.assertIn('ComfyUI (pid 1) used 4.0 cores', reasons[1])
        self.assertIn('4.5 cores together', reasons[2])
        browsing = {'host': 'windows', 'total_gib': 63.7, 'available_gib': 36.0, 'cores': 0.9,
                    'busiest': [{'name': 'opera', 'pid': 2, 'cores': 0.9, 'gib': 0.5}]}
        self.assertEqual(build_bench.contention(browsing), [], 'a browser tab below a core is background')
        many = dict(browsing, cores=2.4)
        self.assertEqual(len(build_bench.contention(many)), 1, 'several small processes add up')
        self.assertEqual(build_bench.contention({'host': 'windows', 'error': 'x'}), [])
        self.assertTrue(build_bench.host_summary([browsing]).endswith('; idle'))
        summary = build_bench.host_summary([busy, {'host': 'windows', 'error': 'no interop'}])
        self.assertIn('windows 0.7 of 63.7 GiB available, others 4.5 cores, busiest ComfyUI 4.0 cores', summary)
        self.assertIn('not sampled (no interop)', summary)
        self.assertIn('; CONTENDED: ', summary)
        # The Windows sampler: PowerShell's JSON (WSL reaches it through interop); a failure is recorded, not raised.
        document = {'total': 64 * gib, 'available': 30 * gib,
                    'before': [{'pid': 1, 'name': 'dota2', 'rss': 4 * gib, 'cpu': 10.0}],
                    'after': [{'pid': 1, 'name': 'dota2', 'rss': 4 * gib, 'cpu': 14.0},
                              {'pid': 2, 'name': 'System', 'rss': 0, 'cpu': None}]}
        with patch.object(build_bench.subprocess, 'run',
                          return_value=SimpleNamespace(stdout=json.dumps(document))) as call:
            state = build_bench.windows_host_state('powershell.exe', seconds=2.0)
        self.assertIn('Start-Sleep -Milliseconds 2000', call.call_args[0][0][-1])
        self.assertIn('$_.Id -ne $PID', call.call_args[0][0][-1], 'the sampler leaves itself out')
        self.assertEqual((state['total_gib'], state['available_gib'], state['cores']), (64.0, 30.0, 2.0))
        self.assertEqual(state['busiest'][0]['name'], 'dota2')
        with patch.object(build_bench.subprocess, 'run', return_value=SimpleNamespace(stdout='not json')):
            self.assertIn('error', build_bench.windows_host_state('powershell.exe'))
        with patch.object(build_bench.subprocess, 'run', side_effect=OSError('missing')):
            self.assertEqual(build_bench.windows_host_state('powershell.exe')['error'], 'missing')
        # A board refuses a contended host unless told to record it, and names the load in its record and context.
        with tempfile.TemporaryDirectory() as temp:
            refused = build_bench.Board('win-debug', Path(temp) / 'b', 4, [], 'x', repo=Path(temp),
                                        log=lambda _: None, host=lambda: [busy])
            refused.row = lambda name, action: refused.record['rows'].append({'row': name})
            with self.assertRaisesRegex(SystemExit, 'the host is not idle .*allow-contended'):
                refused.execute(['configure'])
            self.assertEqual(refused.record['rows'], [], 'nothing is measured on a refused host')
            recorded = build_bench.Board('win-debug', Path(temp) / 'c', 4, [], 'x', repo=Path(temp),
                                         log=lambda _: None, host=lambda: [busy])
            recorded.allow_contended = True
            recorded.row = lambda name, action: recorded.record['rows'].append({'row': name})
            record = recorded.execute(['configure'])
            self.assertEqual(record['contended'], reasons)
            self.assertEqual(record['host_after'], [busy])
            record.update({'jobs': 4, 'defines': [], 'toolchain': {}})
            context = build_bench.render_context([record])
            self.assertIn('host before: windows 0.7 of 63.7 GiB', context)
            self.assertIn('host after: ', context)
        if Path('/proc/self/stat').is_file():
            processes = build_bench.linux_processes()
            self.assertIn(os.getpid(), processes)
            self.assertGreater(processes[os.getpid()][1], 0)

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

    def test_mold_installer_extracts_the_binary_names_ld_mold_and_checks_digest_and_version(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = b'#!/bin/sh\necho "mold 3.0.0 (8de38c35; compatible with GNU ld)"\n'
            archive = root / 'mold-3.0.0-x86_64-linux.tar.gz'
            with tarfile.open(archive, 'w:gz') as package:
                for name, data in (('mold-3.0.0-x86_64-linux/bin/mold', binary),
                                   ('mold-3.0.0-x86_64-linux/share/doc/mold/LICENSE', b'MIT')):
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    package.addfile(info, io.BytesIO(data))
                link = tarfile.TarInfo('mold-3.0.0-x86_64-linux/bin/ld.mold')
                link.type, link.linkname = tarfile.SYMTYPE, 'mold'
                package.addfile(link)
            pin = {'version': '3.0.0', 'url': 'https://example.invalid/mold', 'file': archive.name,
                   'sha256': pins_registry.sha256(archive), 'member': 'mold-3.0.0-x86_64-linux/bin/mold',
                   'member_sha256': hashlib.sha256(binary).hexdigest()}
            installed = install_mold.install(root / 'tools', archive=archive, pin=pin, check=False)
            self.assertEqual(installed.read_bytes(), binary)
            self.assertEqual(sorted(p.name for p in (root / 'tools').iterdir()), ['ld.mold', 'mold'],
                             'only the binary and the name -fuse-ld=mold looks for')
            self.assertTrue((root / 'tools' / 'ld.mold').samefile(installed))
            # A second install over the first replaces the alias instead of failing on it.
            install_mold.install(root / 'tools', archive=archive, pin=pin, check=False)
            self.assertTrue((root / 'tools' / 'ld.mold').samefile(installed))
            with self.assertRaisesRegex(pins_registry.PinError, 'binary checksum mismatch'):
                install_mold.install(root / 'wrong', archive=archive, pin=dict(pin, member_sha256='0' * 64),
                                     check=False)
            self.assertEqual(list((root / 'wrong').iterdir()), [])
            with self.assertRaises(KeyError):
                install_mold.install(root / 'symlink', archive=archive, check=False,
                                     pin=dict(pin, member='mold-3.0.0-x86_64-linux/bin/absent'))
            with self.assertRaisesRegex(pins_registry.PinError, 'not a regular file'):
                install_mold.install(root / 'symlink', archive=archive, check=False,
                                     pin=dict(pin, member='mold-3.0.0-x86_64-linux/bin/ld.mold'))
            for reported, accepted in (('mold 3.0.0 (8de38c35; compatible with GNU ld)\n', True),
                                       ('mold 2.42.1 (abc; compatible with GNU ld)\n', False),
                                       ('mold 3.0.01 (abc)\n', False)):
                target = root / ('version-%s' % accepted) / reported.split()[1]
                with patch.object(install_mold.subprocess, 'run', return_value=SimpleNamespace(stdout=reported)):
                    if accepted:
                        install_mold.install(target, archive=archive, pin=pin)
                        self.assertEqual(sorted(p.name for p in target.iterdir()), ['ld.mold', 'mold'])
                    else:
                        with self.assertRaisesRegex(pins_registry.PinError, 'reports'):
                            install_mold.install(target, archive=archive, pin=pin)
                        self.assertEqual(list(target.iterdir()), [], 'a wrong version leaves nothing installed')
        pin = pins_registry.entry('tools', 'mold-linux')
        self.assertEqual(pin['member'], 'mold-%s-x86_64-linux/bin/mold' % pin['version'])
        self.assertIn('/v%s/' % pin['version'], pin['url'])
        self.assertRegex(pin['member_sha256'], r'^[0-9a-f]{64}$')
        self.assertIn('scripts/install-mold.py', check_pins.HELPERS)

    def test_sde_installer_extracts_the_whole_kit_keeps_inner_links_and_refuses_what_escapes_it(self):
        def kit(path, extra=()):
            with tarfile.open(path, 'w:xz') as package:
                # The link precedes its target, so a Windows install must extract it after every file.
                for name, target in (('kit/intel64/pinrt/pin.ld.so', '../pin_lib/libpincrt.so'), *extra):
                    link = tarfile.TarInfo(name)
                    link.type, link.linkname = tarfile.SYMTYPE, target
                    package.addfile(link)
                for name, data in (('kit/sde64', b'#!/bin/sh\n'), ('kit/intel64/pin_lib/libpincrt.so', b'crt'),
                                   ('kit/misc/cpuid/skx/cpuid.def', b'skx')):
                    info = tarfile.TarInfo(name)
                    info.size, info.mode = len(data), 0o755
                    package.addfile(info, io.BytesIO(data))
            return {'version': '10.13.1', 'url': 'https://example.invalid/sde', 'file': path.name,
                    'sha256': pins_registry.sha256(path), 'root': 'kit/', 'binary': 'sde64'}

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pin = kit(root / 'sde.tar.xz')
            tool = install_sde.install(root / 'tools', archive=root / 'sde.tar.xz', pin=pin, check=False)
            self.assertEqual(tool, (root / 'tools' / 'sde64').resolve())
            self.assertEqual(sorted(p.name for p in (root / 'tools').iterdir()),
                             ['cerid-sde.json', 'intel64', 'misc', 'sde64'], 'the kit without its root directory')
            self.assertEqual((root / 'tools/intel64/pinrt/pin.ld.so').read_bytes(), b'crt', 'the inner link resolves')
            self.assertEqual((root / 'tools/intel64/pinrt/pin.ld.so').is_symlink(), not install_sde.WINDOWS,
                             'a symlink on POSIX, a hard link to the same file on Windows')
            self.assertEqual((root / 'tools/misc/cpuid/skx/cpuid.def').read_bytes(), b'skx')
            # A matching stamp is reused; a directory this script did not install is never replaced.
            (root / 'tools/misc/cpuid/skx/cpuid.def').write_bytes(b'kept')
            install_sde.install(root / 'tools', archive=root / 'sde.tar.xz', pin=pin, check=False)
            self.assertEqual((root / 'tools/misc/cpuid/skx/cpuid.def').read_bytes(), b'kept')
            (root / 'foreign').mkdir()
            with self.assertRaisesRegex(pins_registry.PinError, 'no kit this script installed'):
                install_sde.install(root / 'foreign', archive=root / 'sde.tar.xz', pin=pin, check=False)
            with self.assertRaisesRegex(pins_registry.PinError, 'checksum mismatch'):
                install_sde.install(root / 'wrong', archive=root / 'sde.tar.xz', pin=dict(pin, sha256='0' * 64),
                                    check=False)
            for name, extra, error in (('outside', [('other/file', 'kit/sde64')], 'outside kit/'),
                                       ('escaping', [('kit/up', '../../etc/passwd')], 'outside the destination'),
                                       ('absolute', [('kit/abs', '/etc/passwd')], 'absolute|outside the destination')):
                evil = kit(root / f'{name}.tar.xz', extra)
                with self.assertRaisesRegex(Exception, error):
                    install_sde.install(root / name, archive=root / f'{name}.tar.xz', pin=evil, check=False)
                self.assertFalse((root / name).exists(), f'{name}: nothing installed')
                self.assertFalse((root / f'{name}.partial').exists(), f'{name}: the staging tree is removed')
            for reported, accepted in (('Intel(R) Software Development Emulator.  Version:  10.13.1 external\n', True),
                                       ('Intel(R) Software Development Emulator.  Version:  10.12.0 external\n', False)):
                target = root / ('version-%s' % accepted)
                with patch.object(install_sde.subprocess, 'run',
                                  return_value=SimpleNamespace(stdout=reported, stderr='', returncode=0)):
                    if accepted:
                        install_sde.install(target, archive=root / 'sde.tar.xz', pin=pin)
                    else:
                        with self.assertRaisesRegex(pins_registry.PinError, 'reports'):
                            install_sde.install(target, archive=root / 'sde.tar.xz', pin=pin)
                self.assertEqual(target.exists(), accepted, 'a wrong version leaves nothing installed')
        for name, binary in (('sde-windows', 'sde.exe'), ('sde-linux', 'sde64')):
            pin = pins_registry.entry('tools', name)
            self.assertEqual(pin['binary'], binary)
            self.assertEqual(pin['root'], pin['file'].removesuffix('.tar.xz') + '/')
            self.assertIn(pin['version'], pin['url'])
        self.assertIn('scripts/install-sde.py', check_pins.HELPERS)

    def test_clang_tidy_installer_keeps_the_resource_layout_and_checks_digests_count_and_version(self):
        with tempfile.TemporaryDirectory() as temp:
            # The installer resolves its destination; a hosted runner's temporary directory is an 8.3 short name
            # (C:/Users/RUNNER~1/...), which resolving expands, so the expectations start from the resolved path.
            root = Path(temp).resolve()
            binary = b'#!/bin/sh\necho "LLVM (http://llvm.org/):"; echo "  LLVM version 20.1.0"\n'
            headers = {'stddef.h': b'/* stddef */', 'module.modulemap': b'module _Builtin {}',
                       'cuda_wrappers/new': b'//'}
            include = 'clang_tidy/data/lib/clang/20/include/'
            wheel = root / 'clang_tidy-20.1.0-py2.py3-none-manylinux_2_17_x86_64.whl'
            with zipfile.ZipFile(wheel, 'w') as package:
                package.writestr('clang_tidy/__init__.py', b'')
                package.writestr('clang_tidy/data/bin/clang-tidy', binary)
                package.writestr(include, b'')
                for name, data in headers.items():
                    package.writestr(include + name, data)
                package.writestr('clang_tidy-20.1.0.dist-info/METADATA', b'Name: clang-tidy')
            pin = {'version': '20.1.0', 'url': 'https://example.invalid/clang-tidy.whl', 'file': wheel.name,
                   'sha256': pins_registry.sha256(wheel), 'member': 'clang_tidy/data/bin/clang-tidy',
                   'member_sha256': hashlib.sha256(binary).hexdigest(), 'headers': include,
                   'header_count': len(headers)}
            destination = root / 'tools' / 'clang-tidy'
            installed = install_tidy.install(destination, archive=wheel, pin=pin, check=False)
            self.assertEqual(installed, destination / 'bin' / 'clang-tidy')
            self.assertEqual(installed.read_bytes(), binary)
            files = sorted(p.relative_to(destination).as_posix() for p in destination.rglob('*') if p.is_file())
            self.assertEqual(files, ['bin/clang-tidy', 'cerid-clang-tidy.json',
                                     *sorted('lib/clang/20/include/' + name for name in headers)],
                             'the binary, its resource directory and the stamp; nothing else from the wheel')
            self.assertFalse(destination.with_name('clang-tidy.partial').exists())
            # A matching stamp is reused without reading the wheel again.
            with patch.object(install_tidy.pins, 'fetch', side_effect=AssertionError('refetched')):
                self.assertEqual(install_tidy.install(destination, archive=wheel, pin=pin, check=False), installed)
            for name, changed, message in (('binary', dict(pin, member_sha256='0' * 64), 'binary checksum mismatch'),
                                           ('count', dict(pin, header_count=4), '3 files under'),
                                           ('wheel', dict(pin, sha256='0' * 64), 'checksum mismatch'),
                                           ('member', dict(pin, member='clang_tidy/data/clang-tidy'), 'bin/clang-tidy'),
                                           ('escape', dict(pin, member='clang_tidy/data/../x'), 'escapes')):
                target = root / 'refused' / name
                with self.assertRaisesRegex(pins_registry.PinError, message):
                    install_tidy.install(target, archive=wheel, pin=changed, check=False)
                self.assertFalse(target.exists(), f'{name}: a refused install leaves nothing in place')
                self.assertFalse(target.with_name(name + '.partial').exists())
            # A symbolic link among the headers is refused, whatever it points at.
            linked = root / 'linked.whl'
            with zipfile.ZipFile(wheel) as source, zipfile.ZipFile(linked, 'w') as package:
                for info in source.infolist():
                    package.writestr(info, source.read(info))
                link = zipfile.ZipInfo(include + 'escape.h')
                link.external_attr = 0o120777 << 16
                package.writestr(link, b'../../../../../etc/passwd')
            with self.assertRaisesRegex(pins_registry.PinError, 'not a regular file'):
                install_tidy.install(root / 'refused' / 'link', archive=linked, check=False,
                                     pin=dict(pin, sha256=pins_registry.sha256(linked), header_count=4))
            self.assertFalse((root / 'refused' / 'link').exists())
            for reported, accepted in (('LLVM (http://llvm.org/):\n  LLVM version 20.1.0\n', True),
                                       ('LLVM (http://llvm.org/):\n  LLVM version 20.1.8\n', False),
                                       ('Ubuntu clang-tidy\n', False)):
                target = root / ('version-%s' % accepted) / reported.split()[-1]
                result = SimpleNamespace(stdout=reported, returncode=0)
                with patch.object(install_tidy.subprocess, 'run', return_value=result):
                    if accepted:
                        install_tidy.install(target, archive=wheel, pin=pin)
                        self.assertTrue((target / 'bin' / 'clang-tidy').is_file())
                    else:
                        with self.assertRaisesRegex(pins_registry.PinError, 'reports'):
                            install_tidy.install(target, archive=wheel, pin=pin)
                        self.assertFalse(target.exists(), 'a wrong version leaves nothing installed')
        pin = pins_registry.entry('tools', 'clang-tidy-linux')
        self.assertIn('/clang_tidy-%s-' % pin['version'], pin['url'])
        self.assertEqual(pin['file'], pin['url'].rsplit('/', 1)[-1])
        self.assertEqual(pin['version'].split('.')[0], str(tidy_gate.REQUIRED_TIDY_MAJOR))
        self.assertEqual(pin['headers'], 'clang_tidy/data/lib/clang/%d/include/' % tidy_gate.REQUIRED_TIDY_MAJOR)
        self.assertRegex(pin['member_sha256'], r'^[0-9a-f]{64}$')
        self.assertIn('scripts/install-clang-tidy.py', check_pins.HELPERS)

    def test_ninja_installer_extracts_only_the_pinned_member_and_checks_digest_and_version(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = b'#!/bin/sh\necho 1.11.1\n'
            archive = root / 'ninja-linux.zip'
            with zipfile.ZipFile(archive, 'w') as package:
                package.writestr('ninja', binary)
                package.writestr('README', b'not extracted')
            pin = {'version': '1.11.1', 'url': 'https://example.invalid/ninja-linux.zip', 'file': archive.name,
                   'sha256': pins_registry.sha256(archive), 'member': 'ninja',
                   'member_sha256': hashlib.sha256(binary).hexdigest()}
            with patch.object(install_ninja.subprocess, 'run', return_value=SimpleNamespace(stdout='1.11.1\n')):
                installed = install_ninja.install(root / 'tools', archive=archive, pin=pin)
            self.assertEqual(installed.read_bytes(), binary)
            self.assertEqual(sorted(p.name for p in (root / 'tools').iterdir()), ['ninja'])
            with self.assertRaisesRegex(pins_registry.PinError, 'binary checksum mismatch'):
                install_ninja.install(root / 'wrong', archive=archive, pin=dict(pin, member_sha256='0' * 64),
                                      check=False)
            self.assertEqual(list((root / 'wrong').iterdir()), [])
            with patch.object(install_ninja.subprocess, 'run', return_value=SimpleNamespace(stdout='1.13.2\n')):
                with self.assertRaisesRegex(pins_registry.PinError, 'reports'):
                    install_ninja.install(root / 'version', archive=archive, pin=pin)
            self.assertEqual(list((root / 'version').iterdir()), [], 'a wrong version leaves nothing installed')
        for name, member in (('ninja-windows', 'ninja.exe'), ('ninja-linux', 'ninja')):
            pin = pins_registry.entry('tools', name)
            self.assertEqual(pin['member'], member)
            self.assertIn('/v%s/' % pin['version'], pin['url'])
            self.assertRegex(pin['member_sha256'], r'^[0-9a-f]{64}$')
        self.assertEqual(pins_registry.entry('tools', 'ninja-windows')['version'],
                         pins_registry.entry('tools', 'ninja-linux')['version'])
        self.assertIn('scripts/install-ninja.py', check_pins.HELPERS)

    def test_msvc_environment_exports_only_what_vcvars_changed(self):
        before = {'PATH': r'C:\Windows', 'TEMP': r'C:\Temp', 'PROMPT': '$P$G'}
        after = {'Path': r'C:\VC\bin;C:\Windows', 'TEMP': r'C:\Temp', 'INCLUDE': r'C:\VC\include',
                 'PROMPT': '$P$G$_', 'VCToolsVersion': '14.51.36231'}
        self.assertEqual(msvc_dev_env.changed(before, after),
                         {'Path': r'C:\VC\bin;C:\Windows', 'INCLUDE': r'C:\VC\include',
                          'VCToolsVersion': '14.51.36231'},
                         'names compare case-insensitively; an unchanged value and the prompt are not exported')
        self.assertEqual(msvc_dev_env.github_env_lines({'B': 'two', 'A': 'one'}), ['A=one', 'B=two'])
        self.assertEqual(msvc_dev_env.github_env_lines({'M': 'x\nCRD_MSVC_ENV_EOF'}),
                         ['M<<CRD_MSVC_ENV_EOF_', 'x\nCRD_MSVC_ENV_EOF', 'CRD_MSVC_ENV_EOF_'],
                         'a value with a line break takes a delimiter it does not contain')
        self.assertIn('scripts/msvc-dev-env.py', check_pins.HELPERS)

    def test_clang_tidy_installer_reads_the_windows_release_archive_in_one_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            top = 'clang+llvm-20.1.8-x86_64-pc-windows-msvc/'
            binary = b'MZ clang-tidy'
            headers = {'stddef.h': b'/* stddef */', 'cuda_wrappers/new': b'//'}
            archive = root / 'clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz'

            def add(package, name, data=None, kind=tarfile.REGTYPE, target=''):
                info = tarfile.TarInfo(name)
                info.type = kind
                info.linkname = target
                if data is not None:
                    info.size = len(data)
                package.addfile(info, io.BytesIO(data) if data is not None else None)

            def build(path, extra=None):
                with tarfile.open(path, 'w:xz') as package:
                    add(package, top + 'bin/clang.exe', b'not extracted')
                    add(package, top + 'bin/clang-tidy.exe', binary)
                    add(package, top + 'lib/clang/20/include', kind=tarfile.DIRTYPE)
                    for name, data in headers.items():
                        add(package, top + 'lib/clang/20/include/' + name, data)
                    if extra is not None:
                        extra(package)
                return path

            build(archive)
            pin = {'version': '20.1.8', 'url': 'https://example.invalid/llvm.tar.xz', 'file': archive.name,
                   'sha256': pins_registry.sha256(archive), 'root': top, 'member': top + 'bin/clang-tidy.exe',
                   'member_sha256': hashlib.sha256(binary).hexdigest(), 'headers': top + 'lib/clang/20/include/',
                   'header_count': len(headers)}
            destination = root / 'tools' / 'clang-tidy'
            installed = install_tidy.install(destination, archive=archive, pin=pin, check=False)
            self.assertEqual(installed, destination / 'bin' / 'clang-tidy.exe')
            self.assertEqual(installed.read_bytes(), binary)
            files = sorted(p.relative_to(destination).as_posix() for p in destination.rglob('*') if p.is_file())
            self.assertEqual(files, ['bin/clang-tidy.exe', 'cerid-clang-tidy.json',
                                     *sorted('lib/clang/20/include/' + name for name in headers)])
            with patch.object(install_tidy.pins, 'fetch', side_effect=AssertionError('refetched')):
                self.assertEqual(install_tidy.install(destination, archive=archive, pin=pin, check=False), installed)
            for name, changed, message in (('binary', dict(pin, member_sha256='0' * 64), 'binary checksum mismatch'),
                                           ('count', dict(pin, header_count=3), '2 files under'),
                                           ('missing', dict(pin, member=top + 'bin/clang-tidy-20.exe'),
                                            'bin/clang-tidy'),
                                           ('escape', dict(pin, member=top + 'x/../bin/clang-tidy.exe'), 'escapes')):
                target = root / 'refused' / name
                with self.assertRaisesRegex(pins_registry.PinError, message):
                    install_tidy.install(target, archive=archive, pin=changed, check=False)
                self.assertFalse(target.exists(), f'{name}: a refused install leaves nothing in place')
            linked = build(root / 'linked.tar.xz',
                           lambda package: add(package, top + 'lib/clang/20/include/escape.h', kind=tarfile.SYMTYPE,
                                               target='../../../../../etc/passwd'))
            with self.assertRaisesRegex(pins_registry.PinError, 'not a regular file'):
                install_tidy.install(root / 'refused' / 'link', archive=linked, check=False,
                                     pin=dict(pin, sha256=pins_registry.sha256(linked)))
            self.assertFalse((root / 'refused' / 'link').exists())
        pin = pins_registry.entry('tools', 'llvm')
        self.assertEqual(pin['file'], pin['url'].rsplit('/', 1)[-1])
        self.assertIn('/llvmorg-%s/' % pin['version'], pin['url'])
        self.assertEqual(pin['member'], pin['root'] + 'bin/clang-tidy.exe')
        self.assertEqual(pin['headers'], pin['root'] + 'lib/clang/%d/include/' % tidy_gate.REQUIRED_TIDY_MAJOR)
        self.assertRegex(pin['member_sha256'], r'^[0-9a-f]{64}$')

    MSVC_PROFILE = (
        '[1/3] Building CXX object engine/a.cpp.obj\n'
        'a.cpp\n'
        'Include Headers:\n'
        '\tCount: 3\n'
        '\t\tD:\\Dev\\cerid\\engine\\x.hpp: 0.500000s\n'
        '\t\t\tC:\\VC\\include\\span: 0.300000s\n'
        '\t\t\t\tC:\\VC\\include\\xutility: 0.200000s\n'
        '\t\t\tD:\\Dev\\cerid\\engine\\y.hpp: 0.100000s\n'
        '\t\tD:\\Dev\\cerid\\engine\\z.hpp: 0.050000s\n'
        '\n'
        '\tTop 2 (top-level only):\n'
        '\t\tD:\\Dev\\cerid\\engine\\x.hpp: 0.500000s\n'
        '\n'
        '\tTotal: 0.550000s\n'
        'Class Definitions:\n'
        '\tCount: 2\n'
        '\t\tcrd::ceir::Context: 0.010000s\n'
        "\t\tstd::vector<`template-type-parameter-1'>: 0.002000s\n"
        '\n'
        '\tTotal: 0.012000s\n'
        'Function Definitions:\n'
        '\tCount: 1\n'
        '\t\tcrd::f: 0.004000s\n'
        '\n'
        '\tTotal: 0.004000s\n'
        'time(C:\\VC\\bin\\c1xx.dll)=0.80000s < 1 - 2 > BB [D:\\Dev\\cerid\\engine\\a.cpp]\n'
        'time(C:\\VC\\bin\\c2.dll)=0.20000s < 3 - 4 > BB [D:\\Dev\\cerid\\engine\\a.cpp]\n'
        '[2/3] Building CXX object engine/b.cpp.obj\n'
        'b.cpp\n'
        'Include Headers:\n'
        '\tCount: 1\n'
        '\t\tD:\\Dev\\cerid\\engine\\x.hpp: 0.400000s\n'
        '\t\t\tC:\\VC\\include\\span: 0.250000s\n'
        '\n'
        '\tTotal: 0.400000s\n'
        'Function Definitions:\n'
        '\tCount: 1\n'
        '\t\tcrd::f: 0.006000s\n'
        '\n'
        '\tTotal: 0.006000s\n'
        'time(C:\\VC\\bin\\c1xx.dll)=0.60000s < 1 - 2 > BB [D:\\Dev\\cerid\\engine\\b.cpp]\n'
        '[3/3] Linking CXX executable app.exe\n')

    CBA_REPORT = ('Analyzing build trace from capture.bin...\n'
                  '**** Time summary:\n'
                  'Compilation (1863 times):\n'
                  '  Parsing (frontend):         1234.5 s\n'
                  '  Codegen & opts (backend):    345.6 s\n'
                  '\n'
                  '**** Files that took longest to parse (compiler frontend):\n'
                  '  9012 ms: /home/u/cerid-src/build/tests/ceir/x.cpp.o\n'
                  '**** Templates that took longest to instantiate:\n'
                  '  4321 ms: crd::containers::Array<int> (1234 times, avg 3 ms)\n'
                  '**** Expensive headers:\n'
                  '  70000 ms: /home/u/cerid-src/engine/execution/ceir/include/crd/ceir/ir.hpp (included 500 times, '
                  'avg 140 ms), included via:\n'
                  '    345x: context.hpp\n'
                  '  done in 0.4s.\n')

    def test_compile_profile_reads_msvc_timings_and_clang_build_analyzer_reports(self):
        root = Path('D:/Dev/cerid')
        document = build_profile.parse_msvc(self.MSVC_PROFILE.splitlines(True), root=root)
        self.assertEqual(document['units'], 2)
        self.assertEqual((document['frontend_s'], document['backend_s']), (1.4, 0.2))
        self.assertAlmostEqual(document['include_s'], 0.95, delta=0.051, msg='0.55 + 0.4, the top-level includes')
        headers = {row['name']: (row['seconds'], row['units']) for row in document['headers']}
        self.assertEqual(headers['engine/x.hpp'], (0.9, 2), 'inclusive time summed over the units that parse it')
        self.assertEqual(headers['C:/VC/include/span'], (0.55, 2))
        exclusive = {row['name']: row['seconds'] for row in document['headers_exclusive']}
        self.assertEqual(exclusive['engine/x.hpp'], 0.25, 'x.hpp: 0.5 - (0.3 + 0.1) in a, 0.4 - 0.25 in b')
        self.assertEqual(exclusive['C:/VC/include/span'], 0.35, '0.3 - 0.2 in a, 0.25 in b')
        self.assertEqual(exclusive['C:/VC/include/xutility'], 0.2)
        self.assertEqual(exclusive['engine/z.hpp'], 0.05)
        self.assertAlmostEqual(sum(exclusive.values()), 0.95, places=6,
                               msg='exclusive times add up to the build include time')
        self.assertEqual(document['classes'][0], {'name': 'crd::ceir::Context', 'seconds': 0.01, 'units': 1})
        self.assertEqual(document['functions'][0], {'name': 'crd::f', 'seconds': 0.01, 'units': 2})
        first = document['top_units'][0]
        self.assertEqual((first['source'], first['frontend_s'], first['backend_s']), ('engine/a.cpp', 0.8, 0.2))
        self.assertNotIn('app.exe', [u['edge'] for u in document['top_units']], 'a link edge prints no timing')
        clang = build_profile.parse_clang_report(self.CBA_REPORT, root=Path('/home/u/cerid-src'))
        self.assertEqual(clang['summary']['Parsing (frontend)'], 1234.5)
        self.assertEqual(clang['frontend'], [{'name': 'build/tests/ceir/x.cpp.o', 'seconds': 9.012}])
        self.assertEqual(clang['templates'][0]['count'], 1234)
        self.assertEqual(clang['headers'], [{'name': 'engine/execution/ceir/include/crd/ceir/ir.hpp',
                                             'seconds': 70.0, 'count': 500}])
        rows = build_profile.compare({'headers': [{'name': 'a', 'seconds': 2.0}, {'name': 'b', 'seconds': 1.0}]},
                                     {'headers': [{'name': 'a', 'seconds': 1.5}]})
        self.assertEqual(rows, [{'name': 'a', 'before': 2.0, 'after': 1.5, 'delta': -0.5},
                                {'name': 'b', 'before': 1.0, 'after': None, 'delta': None}])
        markdown = build_profile.render(dict(document, label='w', preset='win-debug', jobs=16, build_s=10,
                                             edge_s=100, edges=3, defines=[]))
        self.assertIn('| `engine/x.hpp` | 0.9 | 2 |', markdown)
        self.assertIn('front end 1 s, back end 0 s', markdown)
        self.assertIn('| `crd::containers::Array<int>` | 4.321 | 1234 |', build_profile.render(clang))

    def test_compile_profile_runs_uncached_in_a_fresh_tree_and_refuses_a_busy_host(self):
        command = build_profile.configure_command('win-debug', Path('B'), 'msvc', ['CRD_ENABLE_PCH=OFF'])
        self.assertIn('-DCMAKE_CXX_FLAGS_INIT=/Bt+ /d1reportTime', command)
        self.assertIn('-DCRD_COMPILER_LAUNCHER=', command, 'a cache hit would hide the units it serves')
        self.assertEqual(command[-1], '-DCRD_ENABLE_PCH=OFF')
        self.assertIn('-DCMAKE_CXX_FLAGS_INIT=-ftime-trace',
                      build_profile.configure_command('linux-clang-tsan', Path('B'), 'clang', []))
        with tempfile.TemporaryDirectory() as temp:
            args = SimpleNamespace(build_dir=str(Path(temp) / 'tree'), allow_contended=False, output=str(
                Path(temp) / 'out' / 'p.json'), preset='win-debug', compiler='msvc', define=[], jobs=4, target=[],
                label='x', top=5, analyzer='cba')
            busy = [{'host': 'windows', 'total_gib': 64.0, 'available_gib': 1.0, 'cores': 0.0, 'busiest': []}]
            with self.assertRaisesRegex(SystemExit, 'not idle'):
                build_profile.run_profile(args, runner=None, host=lambda: busy)
            calls = []

            def runner(command, cwd=None, stdout=None, stderr=None, check=False, **_):
                calls.append(command)
                if stdout is not None:
                    stdout.write(self.MSVC_PROFILE)
                return SimpleNamespace(returncode=0)

            self.assertEqual(build_profile.run_profile(args, runner=runner, host=lambda: []), 0)
            saved = json.loads((Path(temp) / 'out' / 'p.json').read_text(encoding='utf-8'))
            self.assertEqual((saved['units'], saved['build_exit'], saved['contended']), (2, 0, []))
            self.assertEqual(calls[1][:2], ['cmake', '--build'])
            self.assertTrue((Path(temp) / 'out' / 'p.log').is_file(), 'the log stays for parse-msvc')
            (Path(temp) / 'tree').mkdir()
            with self.assertRaisesRegex(SystemExit, 'fresh scratch directory'):
                build_profile.run_profile(args, runner=runner, host=lambda: [])
            # C++ Build Insights around the build: started before it, stopped even when it fails, analysed twice.
            calls.clear()
            traced = SimpleNamespace(**dict(vars(args), build_dir=str(Path(temp) / 'traced'), vcperf='Session',
                                            vcperf_noadmin=False, output=str(Path(temp) / 'out' / 'v.json')))

            def failing_build(command, cwd=None, stdout=None, stderr=None, check=False, **_):
                calls.append(command)
                return SimpleNamespace(returncode=1 if command[:2] == ['cmake', '--build'] else 0, stdout='',
                                       stderr='')

            self.assertEqual(build_profile.run_profile(traced, runner=failing_build, host=lambda: [],
                                                       which=lambda name: 'vcperf.exe'), 1)
            verbs = [c[1] for c in calls]
            self.assertEqual(verbs, ['--preset', '/start', '--build', '/stopnoanalyze', '/analyze', '/analyze'])
            self.assertEqual(calls[1], ['vcperf.exe', '/start', 'Session'])
            self.assertIn('/timetrace', calls[5])
            saved = json.loads((Path(temp) / 'out' / 'v.json').read_text(encoding='utf-8'))
            self.assertEqual(saved['build_insights']['outputs'][1][-11:], 'vcperf.json')
            noadmin = build_profile.vcperf_commands('v', 'S', 'x', noadmin=True)[0]
            self.assertEqual(noadmin, ['v', '/start', '/noadmin', 'S'])
            calls.clear()
            denied = SimpleNamespace(**dict(vars(traced), build_dir=str(Path(temp) / 'denied')))

            def no_admin(command, cwd=None, stdout=None, stderr=None, check=False, **_):
                calls.append(command)
                refused = command[1:2] == ['/start']
                return SimpleNamespace(returncode=1 if refused else 0, stdout='requires administrator privileges',
                                       stderr='')

            with self.assertRaisesRegex(SystemExit, 'grantusercontrol'):
                build_profile.run_profile(denied, runner=no_admin, host=lambda: [], which=lambda name: 'vcperf.exe')
            self.assertNotIn(['cmake', '--build'], [c[:2] for c in calls], 'no build without the trace')
            with self.assertRaisesRegex(SystemExit, 'MSVC build'):
                build_profile.run_profile(SimpleNamespace(**dict(vars(denied), compiler='clang')), runner=no_admin,
                                          host=lambda: [])

    def test_include_budget_reads_both_listings_and_fails_a_forbidden_header_with_its_chain(self):
        gnu = ('. /src/engine/include/crd/containers/array.hpp\n'
               '.. /src/engine/include/crd/core/assert.hpp\n'
               '... /usr/include/c++/13/cstddef\n'
               '.. /usr/include/c++/13/iterator\n'
               '... /usr/include/c++/13/bits/streambuf_iterator.h\n'
               'Multiple include guards may be useful for:\n'
               '/usr/include/c++/13/iterator\n')
        listing = include_budget.parse_listing(gnu, 'gnu')
        self.assertEqual([depth for depth, _ in listing], [1, 2, 3, 2, 3], 'the include-guard summary is not listed')
        found = include_budget.chains(listing, {'iterator', 'chrono'})
        self.assertEqual(list(found), ['iterator'])
        self.assertEqual([include_budget.short(p) for p in found['iterator']], ['crd/containers/array.hpp', 'c++/13/iterator'])
        # MSVC indents one space per level after a localized prefix with its own spacing.
        turkish = ('budget_probe_0.cpp\n'
                   'Not: eklenen dosya:  D:\\src\\engine\\include\\crd\\containers\\hash_map.hpp\n'
                   'Not: eklenen dosya:   D:\\src\\engine\\include\\crd\\containers\\hash.hpp\n'
                   'Not: eklenen dosya:    C:\\VC\\include\\functional\n')
        listing = include_budget.parse_listing(turkish, 'msvc', 'Not: eklenen dosya:  ')
        self.assertEqual([depth for depth, _ in listing], [1, 2, 3])
        english = 'Note: including file: C:\\a.hpp\nNote: including file:  C:\\VC\\include\\chrono\n'
        self.assertEqual(include_budget.parse_listing(english, 'msvc'), [(1, 'C:\\a.hpp'), (2, 'C:\\VC\\include\\chrono')])
        launched = {'command': 'C:\\tools\\sccache.exe C:\\VC\\cl.exe /nologo /c D:\\src\\a.cpp', 'file': 'D:\\src\\a.cpp'}
        self.assertTrue(include_budget.without_launcher(launched)['command'].startswith('C:\\VC\\cl.exe'))
        self.assertEqual(include_budget.without_launcher({'arguments': ['/usr/bin/ccache', 'g++', '-c', 'a.cpp']})
                         ['arguments'], ['g++', '-c', 'a.cpp'])
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp) / 'build'
            build.mkdir()
            unit = {'directory': str(build), 'file': '/src/tests/foundation/containers/test_containers.cpp',
                    'arguments': ['g++', '-Iinc', '-include', '/b/cmake_pch.hxx', '-o', 'x.o', '-c',
                                  '/src/tests/foundation/containers/test_containers.cpp']}
            (build / 'compile_commands.json').write_text(json.dumps([unit]), encoding='utf-8')
            registry = {'schema': include_budget.SCHEMA, 'entries': [
                {'include': 'crd/containers/array.hpp', 'flags_of': 'containers/test_containers.cpp',
                 'forbid': ['iterator'], 'why': 'x'},
                {'include': 'crd/math/transcendental.hpp', 'flags_of': 'containers/test_containers.cpp',
                 'forbid': ['chrono'], 'forbid_gnu': ['immintrin.h'], 'why': 'x'},
                {'include': 'crd/x.hpp', 'flags_of': 'absent/test_x.cpp', 'forbid': ['chrono'], 'why': 'x'},
                # A list is tried in order: the first unit the configuration has lends its command.
                {'include': 'crd/containers/array.hpp', 'flags_of': ['Unity/unity_0_cxx.cxx', 'test_containers.cpp'],
                 'forbid': ['functional'], 'why': 'x'},
                {'include': 'crd/x.hpp', 'flags_of': ['Unity/unity_0_cxx.cxx', 'absent/test_y.cpp'],
                 'forbid': ['chrono'], 'why': 'x'}]}
            commands = []

            def compiler(arguments, **_):
                commands.append(arguments)
                probe = Path(arguments[-3]).read_text(encoding='utf-8')
                text = gnu if 'array' in probe else '. /src/crd/math/transcendental.hpp\n.. /usr/lib/immintrin.h\n'
                return SimpleNamespace(returncode=0, stdout='', stderr=text)

            results = include_budget.check(build, registry, Path(temp), runner=compiler)
            self.assertEqual([verdict for _, verdict, _ in results], ['over', 'over', 'unchecked', 'within', 'unchecked'])
            self.assertIn('<iterator> through crd/containers/array.hpp -> c++/13/iterator', results[0][2])
            self.assertIn('<immintrin.h>', results[1][2], 'forbid_gnu applies on GCC and Clang')
            self.assertIn('absent/test_x.cpp is not in', results[2][2])
            self.assertIn('Unity/unity_0_cxx.cxx or absent/test_y.cpp is not in', results[4][2])
            self.assertNotIn('cmake_pch', ' '.join(commands[0]), 'the precompiled header is removed')
            self.assertEqual(commands[0][-2:], ['-H', '-fsyntax-only'])
            self.assertNotIn('-c', commands[0])

            # clang-cl: CMake ends its commands with `-- <source>`, so the listing options go before the separator,
            # and its -TP is not doubled (a second spelling is -Woverriding-option, an error under /WX).
            clang_cl = {'directory': str(build), 'file': 'D:/s/tests/foundation/containers/test_containers.cpp',
                        'arguments': ['clang-cl.exe', '-TP', '/WX', '/FoX.obj', '-c', '--',
                                      'D:/s/tests/foundation/containers/test_containers.cpp']}
            probe = Path(temp) / 'budget_probe_9.cpp'
            arguments = include_budget.probe_command(clang_cl, probe, 'msvc', Path(temp))
            cut = arguments.index('--')
            self.assertEqual(arguments[cut - 2:cut], ['/showIncludes', '/Zs'])
            self.assertEqual(arguments[cut + 1:], [str(probe)])
            self.assertEqual([token for token in arguments if token in ('/TP', '-TP')], ['-TP'])
            self.assertNotIn('-c', arguments)

            def broken(arguments, **_):
                return SimpleNamespace(returncode=1, stdout='', stderr='fatal error: crd/x.hpp: No such file')

            self.assertEqual(include_budget.check(build, registry, Path(temp), runner=broken)[0][1], 'unchecked',
                             'a probe that does not compile is never within')
        document = include_budget.load()
        self.assertTrue(all(entry['why'] for entry in document['entries']))
        self.assertIn('catch2/catch_test_macros.hpp', [entry['include'] for entry in document['entries']])
        # Catch2's own units are held to the patched copies too (CMakeLists.txt puts them first for Catch2 itself).
        library = [entry for entry in document['entries'] if entry['include'] == 'catch2/catch_tostring.hpp']
        self.assertEqual(len(library), 1)
        self.assertIn('Catch2.dir/Unity/unity_0_cxx.cxx', library[0]['flags_of'])
        self.assertIn('chrono', library[0]['forbid'])

    def test_include_budget_exits_nonzero_unless_every_entry_is_within(self):
        # The guard's exit, not only its verdict: an entry it could not check (no unit to borrow, a probe that did not
        # compile, as on the clang-cl lanes before 471109f6) fails the CTest exactly like a forbidden header.
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp) / 'build'
            build.mkdir()
            unit = {'directory': str(build), 'file': '/src/tests/foundation/containers/test_containers.cpp',
                    'arguments': ['g++', '-c', '/src/tests/foundation/containers/test_containers.cpp']}
            (build / 'compile_commands.json').write_text(json.dumps([unit]), encoding='utf-8')
            registry = Path(temp) / 'budget.json'
            registry.write_text(json.dumps({'schema': include_budget.SCHEMA, 'entries': [
                {'include': 'crd/x.hpp', 'flags_of': 'absent/test_x.cpp', 'forbid': ['chrono'], 'why': 'x'}]}),
                encoding='utf-8')
            arguments = ['--build', str(build), '--registry', str(registry), '--scratch', str(Path(temp) / 'probes')]

            def run(verdicts=None):
                out = io.StringIO()
                fake = None if verdicts is None else (
                    lambda build, registry, scratch, **_: [(entry, verdicts, 'x') for entry in registry['entries']])
                with patch.object(include_budget, 'msvc_environment', return_value=None), \
                        patch.object(include_budget, 'check', fake or include_budget.check), \
                        patch('sys.stdout', out):
                    return include_budget.main(arguments), out.getvalue()

            code, text = run()
            self.assertEqual(code, 1, text)
            self.assertIn('UNCHECKED', text)
            self.assertIn('absent/test_x.cpp is not in', text)
            self.assertIn('include budget: 0 of 1 headers within', text)
            self.assertEqual(run('unchecked')[0], 1, 'a probe that did not compile fails the guard')
            self.assertEqual(run('over')[0], 1)
            self.assertEqual(run('within')[0], 0, 'the same registry passes once every entry is within')

    def test_clang_build_analyzer_installer_builds_the_verified_source_and_checks_the_version(self):
        with tempfile.TemporaryDirectory() as temp:
            # The installer resolves its destination; a hosted runner's temporary directory is an 8.3 short name
            # (C:/Users/RUNNER~1/...), which resolving expands, so the expectations start from the resolved path.
            root = Path(temp).resolve()
            archive = root / 'ClangBuildAnalyzer-test.tar.gz'
            with tarfile.open(archive, 'w:gz') as package:
                data = b'project(ClangBuildAnalyzer)\n'
                info = tarfile.TarInfo('ClangBuildAnalyzer-abc/CMakeLists.txt')
                info.size = len(data)
                package.addfile(info, io.BytesIO(data))
            pin = {'version': '1.6.0', 'commit': 'b' * 40, 'url': 'https://example.invalid/cba.tar.gz',
                   'file': archive.name, 'sha256': pins_registry.sha256(archive)}
            commands = []
            destination = root / 'tools'

            def fake_build(command, log, reported='ClangBuildAnalyzer \x1b[1m1.6.0\x1b[0m\n'):
                commands.append(command)
                if command[1] == '--install':
                    tool = install_cba.binary(destination)
                    tool.parent.mkdir(parents=True, exist_ok=True)
                    tool.write_text('#!/bin/sh\nprintf "%s"\n' % reported, encoding='utf-8')

            def version_of(command, **_):
                return SimpleNamespace(stdout='ClangBuildAnalyzer \x1b[1m1.6.0\x1b[0m\n', stderr='')

            with patch.object(install_cba.subprocess, 'run', side_effect=version_of):
                tool = install_cba.install(destination, archive=archive, work_dir=root / 'work', pin=pin,
                                           runner=fake_build)
                self.assertEqual(tool, install_cba.binary(destination))
                self.assertEqual([c[1] for c in commands], ['-S', '--build', '--install'])
                self.assertIn('-DCMAKE_BUILD_TYPE=Release', commands[0])
                self.assertIn('-DCMAKE_INSTALL_PREFIX=%s' % destination.resolve(), commands[0])
                self.assertEqual(json.loads((destination / install_cba.STAMP).read_text(encoding='utf-8')),
                                 install_cba.expected_stamp(pin))
                # A matching stamp and version are reused without a build; a changed pin rebuilds.
                commands.clear()
                install_cba.install(destination, archive=archive, work_dir=root / 'work', pin=pin, runner=fake_build)
                self.assertEqual(commands, [])
                self.assertFalse(install_cba.reusable(destination, dict(install_cba.expected_stamp(pin), commit='c')))
            old = root / 'old'

            def build_old(command, log):
                if command[1] == '--install':
                    install_cba.binary(old).parent.mkdir(parents=True, exist_ok=True)
                    install_cba.binary(old).write_text('x', encoding='utf-8')

            with patch.object(install_cba.subprocess, 'run',
                              return_value=SimpleNamespace(stdout='ClangBuildAnalyzer 1.5.0\n', stderr='')):
                with self.assertRaisesRegex(pins_registry.PinError, "reports '1.5.0'"):
                    install_cba.install(old, archive=archive, work_dir=root / 'work2', pin=pin, runner=build_old)
                self.assertFalse((old / install_cba.STAMP).exists(), 'a wrong version is never stamped')
            with self.assertRaisesRegex(pins_registry.PinError, 'checksum mismatch'):
                install_cba.install(root / 'bad', archive=archive, work_dir=root / 'work3',
                                    pin=dict(pin, sha256='0' * 64), runner=fake_build)
        pin = pins_registry.entry('tools', 'clang-build-analyzer')
        self.assertIn(pin['commit'], pin['url'])
        self.assertEqual(pin['license'], 'Unlicense')
        self.assertIn('scripts/install-clang-build-analyzer.py', check_pins.HELPERS)

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
                'get_directory_property(_link LINK_OPTIONS)\nget_directory_property(_compile COMPILE_OPTIONS)\n'
                'message(STATUS "LAUNCHER=[${CMAKE_CXX_COMPILER_LAUNCHER}] POOL_COMPILE=[${CMAKE_JOB_POOL_COMPILE}] '
                'POOL_LINK=[${CMAKE_JOB_POOL_LINK}] EMBEDDED=[${CRD_EMBEDDED_DEBUG_INFO}] '
                'FORMAT=[${CMAKE_MSVC_DEBUG_INFORMATION_FORMAT}]")\n'
                'message(STATUS "LINK=[${_link}] COMPILE=[${_compile}]")\n', encoding='utf-8')

            def configure(build, *defines):
                result = subprocess.run([cmake, '-S', str(project), '-B', str(build), '-G', 'Ninja',
                                         f'-DCMAKE_MAKE_PROGRAM={ninja}', *defines],
                                        capture_output=True, text=True, timeout=300)
                return result.returncode, result.stdout + result.stderr

            code, output = configure(Path(temp) / 'default')
            self.assertEqual(code, 0, output)
            self.assertIn('LAUNCHER=[] POOL_COMPILE=[] POOL_LINK=[] EMBEDDED=[OFF] FORMAT=[]', output)
            self.assertIn('LINK=[] COMPILE=[]', output, 'the default graph links with the toolchain default')
            self.assertNotIn('[crd] Compiler launcher', output)
            self.assertNotIn('[crd] Linker', output)
            # The linker: only on link lines (no compile option, so a cached tree keeps its keys), only for the
            # installed mold with the ld.mold name beside it, and ignored by MSVC-compatible compilers.
            tools = Path(temp) / 'tools'
            tools.mkdir()
            mold = tools / 'mold'
            mold.write_bytes(b'')
            code, output = configure(Path(temp) / 'no-alias', '-DCRD_LINKER=%s' % mold.as_posix())
            self.assertNotEqual(code, 0)
            self.assertIn('has no ld.mold, which -fuse-ld=mold looks for', ' '.join(output.split()))
            (tools / 'ld.mold').write_bytes(b'')
            code, output = configure(Path(temp) / 'mold', '-DCRD_LINKER=%s' % mold.as_posix())
            self.assertEqual(code, 0, output)
            self.assertIn('[crd] Linker: %s (GCC and Clang links)' % mold.as_posix(), output)
            self.assertIn('LINK=[$<$<LINK_LANGUAGE:C,CXX>:-fuse-ld=mold>;$<$<LINK_LANGUAGE:C,CXX>:-B%s>] COMPILE=[]'
                          % tools.as_posix(), output)
            self.assertIn('LAUNCHER=[] POOL_COMPILE=[]', output, 'the linker turns nothing else on')
            # The thread bound: one more link option, on mold's link lines only, a positive integer, and nothing
            # without the linker (a stale entry after the linker is turned off configures the default graph).
            code, output = configure(Path(temp) / 'mold-threads', '-DCRD_LINKER=%s' % mold.as_posix(),
                                     '-DCRD_LINKER_THREADS=2')
            self.assertEqual(code, 0, output)
            self.assertIn('[crd] Linker: %s (GCC and Clang links, 2 threads per link)' % mold.as_posix(), output)
            self.assertIn('LINK=[$<$<LINK_LANGUAGE:C,CXX>:-fuse-ld=mold>;$<$<LINK_LANGUAGE:C,CXX>:-B%s>;'
                          '$<$<LINK_LANGUAGE:C,CXX>:-Wl,--threads=2>] COMPILE=[]' % tools.as_posix(), output)
            for wrong in ('0', 'two', '-1'):
                code, output = configure(Path(temp) / 'threads-wrong', '-DCRD_LINKER=%s' % mold.as_posix(),
                                         '-DCRD_LINKER_THREADS=%s' % wrong)
                self.assertNotEqual(code, 0, wrong)
                self.assertIn('CRD_LINKER_THREADS must be a positive integer', output)
                shutil.rmtree(Path(temp) / 'threads-wrong')
            code, output = configure(Path(temp) / 'threads-alone', '-DCRD_LINKER_THREADS=2')
            self.assertEqual(code, 0, output)
            self.assertIn('CRD_LINKER_THREADS ignored', output)
            self.assertIn('LINK=[] COMPILE=[]', output)
            code, output = configure(Path(temp) / 'msvc-linker', '-DCRD_LINKER=%s' % mold.as_posix(), '-DMSVC=ON')
            self.assertEqual(code, 0, output)
            self.assertIn('Linker override ignored', output)
            self.assertIn('LINK=[] COMPILE=[]', output)
            for wrong in ((tools / 'ld.mold').as_posix(), (tools / 'absent' / 'mold').as_posix()):
                code, output = configure(Path(temp) / 'wrong', '-DCRD_LINKER=%s' % wrong)
                self.assertNotEqual(code, 0, wrong)
                self.assertIn('CRD_LINKER must name an installed mold binary', ' '.join(output.split()))
                shutil.rmtree(Path(temp) / 'wrong')
            self.assertNotIn('pool crd_', (Path(temp) / 'default/CMakeFiles/rules.ninja').read_text(encoding='utf-8'))
            code, output = configure(Path(temp) / 'tuned', '-DCRD_COMPILER_LAUNCHER=/opt/sccache', '-DCRD_ENABLE_PCH=OFF',
                                     '-DCRD_COMPILE_JOBS=3', '-DCRD_LINK_JOBS=1')
            self.assertEqual(code, 0, output)
            self.assertIn('[crd] Compiler launcher: /opt/sccache (Ninja', output)
            self.assertIn('LAUNCHER=[/opt/sccache] POOL_COMPILE=[crd_compile] POOL_LINK=[crd_link]', output)
            self.assertIn('[crd] Ninja job pools: crd_compile=3;crd_link=1', output)
            rules = (Path(temp) / 'tuned/CMakeFiles/rules.ninja').read_text(encoding='utf-8')
            self.assertRegex(rules, r'pool crd_compile\n  depth = 3\n')
            self.assertRegex(rules, r'pool crd_link\n  depth = 1\n')
            self.assertIn('EMBEDDED=[OFF] FORMAT=[]', output, 'GCC and Clang keep their DWARF flags')
            # Shipping is cached too; an MSVC-compatible compiler (MSVC set as CMake sets it for cl and clang-cl)
            # embeds its debug information: Debug and RelWithDebInfo through the format, Shipping through the flag
            # the root build file picks from CRD_EMBEDDED_DEBUG_INFO.
            code, output = configure(Path(temp) / 'shipping', '-DCRD_COMPILER_LAUNCHER=/opt/sccache', '-DCRD_SHIPPING=ON',
                                     '-DMSVC=ON', '-DCRD_ENABLE_PCH=OFF')
            self.assertEqual(code, 0, output)
            self.assertNotIn('Compiler launcher ignored', output)
            self.assertIn('LAUNCHER=[/opt/sccache] POOL_COMPILE=[]', output)
            self.assertIn('EMBEDDED=[ON] FORMAT=[$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>]', output)
            self.assertIn('debug information embedded', output)
            code, output = configure(Path(temp) / 'msvc-default', '-DMSVC=ON')
            self.assertEqual(code, 0, output)
            self.assertIn('LAUNCHER=[] POOL_COMPILE=[] POOL_LINK=[] EMBEDDED=[OFF] FORMAT=[]', output)
            root = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
            self.assertIn('if(CRD_EMBEDDED_DEBUG_INFO)\n            set(_crd_shipping_debug_info /Z7)\n        else()\n'
                          '            set(_crd_shipping_debug_info /Zi)', root)
            self.assertIn(':${_crd_shipping_debug_info}>', root)
            self.assertIn('if(CRD_EMBEDDED_DEBUG_INFO)\n            set(${out_var} "embedded" PARENT_SCOPE)',
                          (ROOT / 'cmake/CrdPackage.cmake').read_text(encoding='utf-8'))
            # A cached tree has no precompiled header, on every compiler: the launcher refuses one.
            for compiler in ((), ('-DMSVC=ON',)):
                code, output = configure(Path(temp) / 'pch', '-DCRD_COMPILER_LAUNCHER=/opt/sccache', *compiler)
                self.assertNotEqual(code, 0, compiler)
                self.assertIn('needs -DCRD_ENABLE_PCH=OFF', ' '.join(output.split()), compiler)
                shutil.rmtree(Path(temp) / 'pch')
            code, output = configure(Path(temp) / 'bad', '-DCRD_COMPILE_JOBS=many')
            self.assertNotEqual(code, 0)
            self.assertIn('CRD_COMPILE_JOBS must be a positive integer', output)


class CompilerCache(unittest.TestCase):
    """scripts/compiler-cache.py: the cache entries the local helpers add and the server a cached tree uses."""

    PRESETS = {'configurePresets': [
        {'name': 'base', 'hidden': True, 'cacheVariables': {'CRD_BUILD_TESTS': 'ON'}},
        {'name': 'win-base', 'hidden': True, 'inherits': 'base', 'generator': 'Ninja',
         'cacheVariables': {'CMAKE_C_COMPILER': 'cl', 'CMAKE_CXX_COMPILER': 'cl'}},
        {'name': 'linux-base', 'hidden': True, 'inherits': 'base', 'generator': 'Ninja',
         'cacheVariables': {'CMAKE_CXX_COMPILER': 'g++'}},
        {'name': 'win-debug', 'inherits': 'win-base', 'cacheVariables': {'CMAKE_BUILD_TYPE': 'Debug'}},
        {'name': 'win-tidy', 'inherits': 'win-debug', 'cacheVariables': {'CRD_ENABLE_CLANG_TIDY': 'ON'}},
        {'name': 'win-vs', 'inherits': 'win-base', 'generator': 'Visual Studio 18 2026',
         'cacheVariables': {'CMAKE_C_COMPILER': None, 'CMAKE_CXX_COMPILER': None}},
        {'name': 'win-clang-cl', 'inherits': 'win-base', 'cacheVariables': {'CMAKE_CXX_COMPILER': 'clang-cl'}},
        {'name': 'win-release', 'inherits': 'win-base', 'cacheVariables': {'CMAKE_INTERPROCEDURAL_OPTIMIZATION': 'ON'}},
        {'name': 'win-shipping', 'inherits': 'win-release', 'cacheVariables': {'CRD_SHIPPING': 'ON'}},
        {'name': 'win-clang-cl-shipping', 'inherits': 'win-base',
         'cacheVariables': {'CMAKE_CXX_COMPILER': 'clang-cl', 'CMAKE_INTERPROCEDURAL_OPTIMIZATION': 'ON'}},
        {'name': 'linux-gcc-shipping', 'inherits': 'linux-base',
         'cacheVariables': {'CMAKE_INTERPROCEDURAL_OPTIMIZATION': 'ON'}},
        {'name': 'linux-gcc-debug', 'inherits': 'linux-base'},
        {'name': 'mixed', 'inherits': ['linux-base', 'win-base']}]}

    def test_configure_args_follow_generator_compiler_and_tidy(self):
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            installed = compiler_cache.default_launcher('windows', home)
            installed.parent.mkdir(parents=True)
            installed.write_bytes(b'')

            def args(preset, environ=None, host='windows'):
                return compiler_cache.configure_args(preset, environ or {}, host, home, self.PRESETS)

            exe = installed.as_posix()
            self.assertEqual(args('win-debug'), ['-DCRD_COMPILER_LAUNCHER=%s' % exe, '-DCRD_ENABLE_PCH=OFF'])
            self.assertEqual(args('win-clang-cl'), ['-DCRD_COMPILER_LAUNCHER=%s' % exe, '-DCRD_ENABLE_PCH=OFF'])
            self.assertEqual(args('win-tidy'), [], 'clang-tidy presets analyse every unit and stay uncached')
            self.assertEqual(args('win-shipping'), [], 'MSVC LTCG trees stay uncached: the link generates the code')
            self.assertEqual(args('win-release', {'CRD_SCCACHE': 'off'}), [], 'and are never converted back either')
            self.assertEqual(args('win-clang-cl-shipping'), ['-DCRD_COMPILER_LAUNCHER=%s' % exe, '-DCRD_ENABLE_PCH=OFF'])
            self.assertEqual(args('linux-gcc-shipping'), ['-DCRD_COMPILER_LAUNCHER=%s' % exe, '-DCRD_ENABLE_PCH=OFF'])
            self.assertEqual(args('win-vs'), [], 'CMake applies no launcher to a Visual Studio generator')
            self.assertEqual(args('mixed'), ['-DCRD_COMPILER_LAUNCHER=%s' % exe, '-DCRD_ENABLE_PCH=OFF'],
                             'every cached tree drops its precompiled header')
            self.assertEqual(args('win-debug', {'CRD_SCCACHE': 'off'}),
                             ['-DCRD_COMPILER_LAUNCHER=', '-DCRD_ENABLE_PCH=ON'], 'off returns the default graph')
            self.assertEqual(args('linux-gcc-debug', {'CRD_SCCACHE': 'OFF'}),
                             ['-DCRD_COMPILER_LAUNCHER=', '-DCRD_ENABLE_PCH=ON'])
            self.assertEqual(args('linux-gcc-debug', {'CRD_SCCACHE': '/opt/sccache'}),
                             ['-DCRD_COMPILER_LAUNCHER=/opt/sccache', '-DCRD_ENABLE_PCH=OFF'])
            self.assertEqual(args('linux-gcc-debug', host='linux'), [], 'nothing installed: nothing added')
            installed.unlink()
            self.assertEqual(args('win-debug'), [])
            with self.assertRaises(KeyError):
                args('no-such-preset')
        for name in ('win-debug', 'win-asan', 'win-shipping', 'win-clang-cl-shipping', 'linux-gcc-debug',
                     'linux-clang-tsan'):
            generator, _ = compiler_cache.resolve_preset(name)
            self.assertEqual(generator, 'Ninja', name)
        self.assertEqual(compiler_cache.configure_args('win-vs', {'CRD_SCCACHE': 'x'}), [])
        self.assertEqual(compiler_cache.configure_args('win-tidy-local', {'CRD_SCCACHE': 'x'}), [])
        for name in ('win-release', 'win-relwithdebinfo', 'win-shipping', 'win-shipping-profile'):
            self.assertEqual(compiler_cache.configure_args(name, {'CRD_SCCACHE': 'x'}), [], name)
        for name in ('win-debug', 'win-asan', 'win-clang-cl', 'win-clang-cl-shipping'):
            self.assertEqual(compiler_cache.configure_args(name, {'CRD_SCCACHE': 'x'}),
                             ['-DCRD_COMPILER_LAUNCHER=x', '-DCRD_ENABLE_PCH=OFF'], name)

    def test_linker_args_link_the_measured_wsl_lanes_with_the_pinned_mold(self):
        presets = {'configurePresets': self.PRESETS['configurePresets'] + [
            {'name': 'linux-gcc-asan', 'inherits': 'linux-base'},
            {'name': 'linux-clang-tsan', 'inherits': 'linux-base', 'cacheVariables': {'CMAKE_CXX_COMPILER': 'clang++'}},
            {'name': 'linux-clang-coverage', 'inherits': 'linux-base'}]}
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)

            def args(preset, environ=None, host='linux'):
                return compiler_cache.configure_args(preset, environ or {}, host, home, presets)

            self.assertEqual(args('linux-gcc-debug'), [], 'no mold installed: nothing added')
            mold = compiler_cache.default_mold(home)
            mold.parent.mkdir(parents=True)
            mold.write_bytes(b'')
            with patch('os.cpu_count', return_value=32):
                expected = ['-DCRD_LINKER=%s' % mold.as_posix(), '-DCRD_LINKER_THREADS=2']
                for name in ('linux-gcc-debug', 'linux-gcc-asan', 'linux-clang-tsan'):
                    self.assertEqual(args(name), expected, name)
                self.assertEqual(args('linux-gcc-debug', {'CRD_LANE_JOBS': '4'}),
                                 ['-DCRD_LINKER=%s' % mold.as_posix(), '-DCRD_LINKER_THREADS=8'],
                                 'four parallel links share the 32 CPUs eight ways')
                self.assertEqual(compiler_cache.cache_args('linux-gcc-debug', {}, 'linux', home, presets), [],
                                 'the linker is no cache entry: check-ci-tiers.py judges the cache rule alone')
                self.assertEqual(args('linux-gcc-shipping'), [], 'a lane the board did not measure faster')
                self.assertEqual(args('linux-clang-coverage'), [], 'an unmeasured lane keeps the toolchain linker')
                self.assertEqual(args('linux-gcc-debug', host='windows'), [], 'mold links ELF programs: WSL only')
                self.assertEqual(args('win-debug'), [], 'MSVC links through link.exe')
                self.assertEqual(args('linux-gcc-debug', {'CRD_MOLD': 'off'}),
                                 ['-DCRD_LINKER=', '-DCRD_LINKER_THREADS='], 'off returns the toolchain linker')
                self.assertEqual(args('linux-gcc-debug', {'CRD_MOLD': '/opt/m/mold', 'CRD_MOLD_THREADS': '3'}),
                                 ['-DCRD_LINKER=/opt/m/mold', '-DCRD_LINKER_THREADS=3'])
                sccache = compiler_cache.default_launcher('linux', home)
                sccache.parent.mkdir(parents=True)
                sccache.write_bytes(b'')
                self.assertEqual(args('linux-gcc-debug'),
                                 ['-DCRD_COMPILER_LAUNCHER=%s' % sccache.as_posix(), '-DCRD_ENABLE_PCH=OFF'] + expected)
                self.assertEqual(args('linux-gcc-debug', {'CRD_SCCACHE': 'off', 'CRD_MOLD': 'off'}),
                                 ['-DCRD_COMPILER_LAUNCHER=', '-DCRD_ENABLE_PCH=ON', '-DCRD_LINKER=',
                                  '-DCRD_LINKER_THREADS='])
                self.assertEqual(compiler_cache.link_threads({'CRD_LANE_JOBS': '64'}), 1, 'never below one')
            with patch('os.cpu_count', return_value=None):
                self.assertEqual(compiler_cache.link_threads({}), 1)
            for wrong in ({'CRD_MOLD_THREADS': '0'}, {'CRD_MOLD_THREADS': 'two'}, {'CRD_LANE_JOBS': 'many'}):
                with self.assertRaises(SystemExit):
                    compiler_cache.link_threads(wrong)
        for name in compiler_cache.MOLD_PRESETS:
            generator, variables = compiler_cache.resolve_preset(name)
            self.assertEqual(generator, 'Ninja', name)
            self.assertNotIn(str(variables.get('CMAKE_CXX_COMPILER', '')).lower(), ('cl', 'clang-cl'), name)

    def test_cache_locations_tree_launcher_and_server_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertEqual(compiler_cache.cache_dir({}, 'windows', root / 'home', root / 'repo'),
                             root / 'repo' / 'build' / 'sccache')
            self.assertEqual(compiler_cache.cache_dir({}, 'linux', root / 'home', root / 'repo'),
                             root / 'home' / '.cache' / 'cerid-sccache', 'outside the WSL source mirror')
            self.assertEqual(compiler_cache.cache_dir({'CRD_SCCACHE_DIR': 'X'}, 'linux'), Path('X'))
            self.assertEqual(compiler_cache.server_environment({}, 'linux', root)['SCCACHE_CACHE_SIZE'], '30G')
            # WSL2 relays a Linux server's localhost port to Windows: the two hosts' servers never share one.
            self.assertEqual(compiler_cache.server_environment({}, 'linux', root)['SCCACHE_SERVER_PORT'], '4227')
            self.assertEqual(compiler_cache.server_environment({}, 'windows', root)['SCCACHE_SERVER_PORT'], '4226')
            self.assertEqual(compiler_cache.server_port({'CRD_SCCACHE_PORT': '5000'}, 'linux'), '5000')
            for wrong in ('0', 'port', '70000'):
                with self.assertRaises(SystemExit):
                    compiler_cache.server_port({'CRD_SCCACHE_PORT': wrong}, 'windows')
            out = io.StringIO()
            with patch.dict(os.environ, {'CRD_SCCACHE_PORT': '4999'}):
                self.assertEqual(compiler_cache.main(['port'], out=out), 0)
            self.assertEqual(out.getvalue(), '4999\n')
            # A server that cannot start (another process holds the port) fails the command; the build helpers stop.
            variables = {'SCCACHE_DIR': str(root / 'cache'), 'SCCACHE_CACHE_SIZE': '30G', 'SCCACHE_SERVER_PORT': '4227'}
            here = {'cache_location': 'Local disk: "%s"' % (root / 'cache').as_posix()}
            other = {'cache_location': 'Local disk: "/home/u/.cache/cerid-sccache"'}
            refused = subprocess.CompletedProcess(['sccache', '--start-server'], 2)
            started = subprocess.CompletedProcess(['sccache', '--start-server'], 0)
            exe = root / 'sccache'

            def run(listens, answers, result):
                calls = []

                def fake_run(command, **_):
                    calls.append(command[1])
                    if isinstance(result, BaseException):
                        raise result
                    return result
                with patch.object(compiler_cache, 'listening', side_effect=list(listens)), \
                        patch.object(compiler_cache, 'stats', side_effect=list(answers)), \
                        patch.object(compiler_cache.subprocess, 'run', side_effect=fake_run):
                    return compiler_cache.ensure_server(exe, variables, {}), calls

            restart = ['--stop-server', '--start-server']
            # Nothing listens: no statistics query (it would start a server that exits with it), an explicit start.
            self.assertEqual(run([False, True], [here], started), ('restarted', restart))
            # The server on the port caches here: kept, nothing started.
            self.assertEqual(run([True], [here], started), ('running', []))
            # Another cache answers (a board's, or a Linux server relayed to Windows): restarted on this one.
            self.assertEqual(run([True, True], [other, here], started), ('restarted', restart))
            # A refused start is fine when this cache now answers (another start won a race) ...
            self.assertEqual(run([True, True], [other, here], refused), ('restarted', restart))
            # ... and fails the command when something else holds the port.
            with self.assertRaises(SystemExit) as failure:
                run([True, True], [other, None], refused)
            self.assertIn('port 4227, exit 2', str(failure.exception))
            # A start that leaves nothing listening fails, without the query that would start a passing transient one.
            with self.assertRaises(SystemExit) as failure:
                run([False, False], [here], started)
            self.assertIn('port 4227, exit 0', str(failure.exception))
            # A listener that accepts and never answers (measured: the client waits out its timeout) is no server.
            hung = subprocess.TimeoutExpired(['sccache'], 60)
            with patch.object(compiler_cache.subprocess, 'run', side_effect=hung):
                self.assertIsNone(compiler_cache.stats(exe, {}))
            with self.assertRaises(SystemExit) as failure:
                run([True, True], [None, None], hung)
            self.assertIn('port 4227, exit 124', str(failure.exception))
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', 0))
                probe.listen(1)
                self.assertTrue(compiler_cache.listening(probe.getsockname()[1]))
            wsl_lane = (ROOT / 'scripts/wsl-lane.sh').read_text(encoding='utf-8')
            self.assertIn('SCCACHE_SERVER_PORT=$(python3 "$HERE/compiler-cache.py" port)', wsl_lane)
            self.assertIn('server_env=$(python3 "$MIRROR/scripts/compiler-cache.py" server', wsl_lane)
            self.assertNotIn('eval "$(python3', wsl_lane, 'eval over a substitution hides a failed server start')
            # A discovery build after a widely included header change keeps going and reports every broken unit.
            self.assertIn('if [[ "${CRD_LANE_KEEP_GOING:-}" == 1 ]]; then\n            keep=(-- -k 0)', wsl_lane)
            self.assertIn('cmake --build "$tree" -j "$JOBS" "${targets[@]}" "${keep[@]}"', wsl_lane)
            # The focused plan's lane route (wsl-lane.sh dev): dev.py check in the lane's tree under the lane lock,
            # with the build's cache server and the test preset's environment, refused on a stale mirror.
            self.assertIn('configure|build|test|run|dev)\n        heavy "$@"', wsl_lane)
            dev_branch = wsl_lane[wsl_lane.index('\n    dev)\n'):]
            dev_branch = dev_branch[:dev_branch.index('\n        ;;')]
            for needle in ('mirror check', 'server_env=$(python3 "$MIRROR/scripts/compiler-cache.py" server "$tree"',
                           'eval "$server_env"', 'eval "$(preset_test_settings "$preset")"',
                           'python3 "$MIRROR/scripts/dev.py" --root "$MIRROR" check --build "$tree" "$@"'):
                self.assertIn(needle, dev_branch)
            build_target = (ROOT / 'scripts/build-target.bat').read_text(encoding='utf-8')
            self.assertIn('server "%~1" --shell cmd > "%CRD_SERVER_ENV%"\nif errorlevel 1 exit /b 1\n', build_target)
            tree = root / 'tree'
            tree.mkdir()
            self.assertIsNone(compiler_cache.tree_launcher(tree))
            (tree / 'CMakeCache.txt').write_text('CRD_COMPILER_LAUNCHER:FILEPATH=\n', encoding='utf-8')
            self.assertIsNone(compiler_cache.tree_launcher(tree))
            (tree / 'CMakeCache.txt').write_text('X:BOOL=ON\nCRD_COMPILER_LAUNCHER:FILEPATH=C:/t/sccache.exe\n',
                                                 encoding='utf-8')
            self.assertEqual(compiler_cache.tree_launcher(tree), Path('C:/t/sccache.exe'))
            out = io.StringIO()
            (tree / 'CMakeCache.txt').write_text('CRD_COMPILER_LAUNCHER:FILEPATH=\n', encoding='utf-8')
            self.assertEqual(compiler_cache.main(['server', str(tree)], out=out), 0)
            self.assertEqual(out.getvalue(), '', 'an uncached tree gets no server and no environment')
        windows = {'cache_location': 'Local disk: "D:\\\\Dev\\\\cerid\\\\build\\\\sccache"'}
        self.assertTrue(compiler_cache.serves(windows, 'D:\\Dev\\cerid\\build\\sccache'))
        self.assertTrue(compiler_cache.serves(windows, 'd:/dev/cerid/build/sccache/'))
        self.assertFalse(compiler_cache.serves(windows, 'D:\\Dev\\cerid\\build\\bench-sccache'))
        self.assertFalse(compiler_cache.serves(None, 'x'))
        self.assertTrue(compiler_cache.serves({'cache_location': 'Local disk: "/home/u/.cache/cerid-sccache"'},
                                              '/home/u/.cache/cerid-sccache'))
        self.assertEqual(compiler_cache.shell_lines({'SCCACHE_DIR': "/a b/it's"}, 'sh'),
                         ["export SCCACHE_DIR='/a b/it'\\''s'"])
        self.assertEqual(compiler_cache.shell_lines({'SCCACHE_DIR': 'D:\\c'}, 'cmd'), ['SCCACHE_DIR=D:\\c'])


class SourceMirror(unittest.TestCase):
    """scripts/source-mirror.py: the mirror holds exactly the checkout's git-visible files, and a stale one is refused."""

    def git(self, root, *arguments):
        # No automatic maintenance, which a commit starts detached (see CiTiers' git ranges case).
        return subprocess.run(['git', '-C', str(root), '-c', 'user.name=fixture', '-c', 'user.email=fixture@invalid',
                               '-c', 'core.autocrlf=false', '-c', 'maintenance.auto=false', *arguments],
                              capture_output=True, check=True)

    def mirror(self, command, source, mirror, *extra):
        out = io.StringIO()
        code = source_mirror.main([command, '--source', str(source), '--mirror', str(mirror), *extra], out=out)
        return code, out.getvalue()

    def test_sync_copies_exactly_the_visible_files_and_check_refuses_every_kind_of_staleness(self):
        with tempfile.TemporaryDirectory() as temp:
            source, mirror = Path(temp) / 'checkout', Path(temp) / 'mirror'
            (source / 'engine').mkdir(parents=True)
            self.git(source, 'init', '-q')
            self.git(source, 'config', 'core.autocrlf', 'false')
            (source / '.gitignore').write_text('/build/\n*.log\n', encoding='utf-8')
            (source / 'engine/a.cpp').write_text('int a = 1;\n', encoding='utf-8')
            (source / 'engine/b.hpp').write_text('#pragma once\n', encoding='utf-8')
            (source / 'build').mkdir()
            (source / 'build/object.o').write_bytes(b'ignored')
            (source / 'run.log').write_text('ignored\n', encoding='utf-8')
            self.git(source, 'add', '.')
            self.git(source, 'commit', '-q', '-m', 'fixture')
            (source / 'engine/new.cpp').write_text('int n;\n', encoding='utf-8')  # untracked, not ignored

            code, output = self.mirror('check', source, mirror)
            self.assertEqual(code, source_mirror.STALE, 'a mirror that was never synced is refused')
            code, output = self.mirror('sync', source, mirror)
            self.assertEqual(code, 0, output)
            self.assertIn('full sync', output)
            self.assertEqual(source_mirror.mirror_files(mirror), ['.gitignore', 'engine/a.cpp', 'engine/b.hpp',
                                                                  'engine/new.cpp'])
            self.assertEqual(os.stat(mirror / 'engine/a.cpp').st_mtime_ns, os.stat(source / 'engine/a.cpp').st_mtime_ns)
            self.assertEqual(self.mirror('check', source, mirror, '--full')[0], 0)

            def stale(change, expected):
                change()
                code, output = self.mirror('check', source, mirror)
                self.assertEqual(code, source_mirror.STALE, output)
                self.assertIn(expected, output)
                code, output = self.mirror('sync', source, mirror)
                self.assertEqual(code, 0, output)
                self.assertIn('incremental sync', output)
                self.assertEqual(self.mirror('check', source, mirror, '--full')[0], 0)

            stale(lambda: (source / 'engine/a.cpp').write_text('int a = 2;\n', encoding='utf-8'), 'engine/a.cpp')
            stale(lambda: (source / 'engine/c.cpp').write_text('int c;\n', encoding='utf-8'), 'engine/c.cpp')
            stale(lambda: (source / 'engine/b.hpp').unlink(), 'engine/b.hpp')
            self.assertFalse((mirror / 'engine/b.hpp').exists())
            stale(lambda: (mirror / 'engine/litter.txt').write_text('test output\n', encoding='utf-8'),
                  'engine/litter.txt')
            self.assertFalse((mirror / 'engine/litter.txt').exists())
            stale(lambda: (source / 'engine/a.cpp').write_text('int a = 1;\n', encoding='utf-8'), 'engine/a.cpp')
            stale(lambda: (self.git(source, 'add', '.'), self.git(source, 'commit', '-q', '-m', 'second')),
                  'HEAD differs')
            # An ignored file in the mirror is not a source difference, but the next sync removes it.
            (mirror / 'scratch.log').write_text('x\n', encoding='utf-8')
            self.assertEqual(self.mirror('check', source, mirror)[0], 0)
            self.assertEqual(self.mirror('sync', source, mirror)[0], 0)
            self.assertFalse((mirror / 'scratch.log').exists())
            # A mirror edit that keeps size and time: the full check compares bytes.
            target = mirror / 'engine/a.cpp'
            times = os.stat(target).st_atime_ns, os.stat(target).st_mtime_ns
            target.write_text('int a = 9;\n', encoding='utf-8')
            os.utime(target, ns=times)
            code, output = self.mirror('check', source, mirror, '--full')
            self.assertEqual(code, source_mirror.STALE, output)
            self.assertIn('different bytes: engine/a.cpp', output)
            self.assertEqual(self.mirror('sync', source, mirror, '--full')[0], 0)
            self.assertEqual(target.read_text(encoding='utf-8'), 'int a = 1;\n')
            with self.assertRaises(SystemExit):
                self.mirror('sync', source, source / 'inside')


class WaitHelper(unittest.TestCase):
    """scripts/wait-for.py: bounded waits on files, logs, exit-code sentinels, processes and commands."""

    def run_main(self, *argv):
        out = io.StringIO()
        code = wait_for.main(list(argv), out=out)
        return code, out.getvalue()

    def test_timeouts_are_bounded_under_the_watchdog_and_polls_are_short(self):
        def never(*args, **kwargs):
            raise AssertionError('an out-of-range timeout or interval reached a wait')

        # Refused before any wait: a lost bound fails here at once instead of waiting out the 1141 s.
        with patch.object(wait_for, 'Waiter', never):
            code, output = self.run_main('--timeout', '1141', 'file', 'x')
            self.assertEqual(code, wait_for.USAGE)
            self.assertIn('watchdog', output)
            self.assertEqual(self.run_main('--timeout', '0', 'file', 'x')[0], wait_for.USAGE)
            self.assertEqual(self.run_main('--interval', '31', 'file', 'x')[0], wait_for.USAGE)
        with tempfile.TemporaryDirectory() as temp:
            started = wait_for.time.monotonic()
            code, output = self.run_main('--timeout', '0.3', '--interval', '0.05', 'file', str(Path(temp) / 'never'))
            self.assertEqual(code, wait_for.TIMED_OUT)
            self.assertLess(wait_for.time.monotonic() - started, 5)
            self.assertIn('did not appear', output)

    def test_heartbeat_reports_a_long_wait(self):
        ticks = iter([0.0, 0.0, 61.0, 61.0, 125.0, 125.0, 200.0])
        out = io.StringIO()
        waiter = wait_for.Waiter(150, 2, out=out, clock=lambda: next(ticks), sleep=lambda _: None)
        self.assertEqual(waiter.wait(lambda: None, 'thing')[0], None)
        self.assertEqual(out.getvalue().count('still waiting for thing'), 2)

    def test_file_and_log_conditions(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'build.log'
            scanner = wait_for.LogScanner(log, contains=r'^\[\d+/\d+\] Linking .*crd-core-tests')
            self.assertIsNone(scanner.poll())
            log.write_text('[1/3] Building a.obj\n', encoding='utf-8')
            self.assertIsNone(scanner.poll())
            with log.open('a', encoding='utf-8') as handle:
                handle.write('[3/3] Linking CXX executable tests/core/crd-core-')
            self.assertIsNone(scanner.poll())
            with log.open('a', encoding='utf-8') as handle:
                handle.write('tests.exe\n')
            self.assertEqual(scanner.poll()[0], 'ok', 'a match spanning two reads is found')
            failing = wait_for.LogScanner(log, contains='DONE', fail='FAILED:|ninja: build stopped')
            log.write_text('FAILED: a.obj\nninja: build stopped\nDONE\n', encoding='utf-8')
            self.assertEqual(failing.poll(), ('fail', 'FAILED:'))
            log.write_text('DONE\nFAILED: later\n', encoding='utf-8')
            self.assertEqual(wait_for.LogScanner(log, contains='DONE', fail='FAILED:').poll(), ('ok', 'DONE'))
            code, output = self.run_main('--timeout', '2', '--interval', '0.05', 'file', str(log), '--contains', 'DONE')
            self.assertEqual(code, 0, output)
            log.write_text('FAILED: x\n', encoding='utf-8')
            self.assertEqual(self.run_main('--timeout', '2', '--interval', '0.05', 'file', str(log), '--contains',
                                           'DONE', '--fail', 'FAILED:')[0], 1)
            truncated = wait_for.LogScanner(log, contains='again')
            self.assertIsNone(truncated.poll())
            log.write_text('', encoding='utf-8')
            self.assertIsNone(truncated.poll())
            log.write_text('again\n', encoding='utf-8')
            self.assertEqual(truncated.poll()[0], 'ok', 'a rewritten log is read from its start')

    def test_exit_file_returns_the_recorded_code_once_it_is_complete(self):
        with tempfile.TemporaryDirectory() as temp:
            sentinel = Path(temp) / 'build.rc'
            log = Path(temp) / 'build.log'
            log.write_text('\n'.join('line %d' % i for i in range(30)) + '\n', encoding='utf-8')
            self.assertIsNone(wait_for.read_exit_code(sentinel))
            sentinel.write_text('', encoding='utf-8')
            self.assertIsNone(wait_for.read_exit_code(sentinel), 'an empty sentinel is still being written')
            sentinel.write_text('3\n', encoding='utf-8')
            code, output = self.run_main('--timeout', '2', '--interval', '0.05', 'exit-file', str(sentinel),
                                         '--log', str(log), '--tail', '2')
            self.assertEqual(code, 3)
            self.assertIn('line 29', output)
            self.assertNotIn('line 27', output)
            sentinel.write_text('3221225477\n', encoding='utf-8')
            code, output = self.run_main('--timeout', '2', '--interval', '0.05', 'exit-file', str(sentinel))
            self.assertEqual(code, 1, 'a code outside 0..255 is still a failure')
            self.assertIn('recorded exit 3221225477', output)

    def test_pid_and_run_modes_follow_real_processes(self):
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(0.5)'])
        try:
            self.assertTrue(wait_for.process_alive(child.pid))
            code, output = self.run_main('--timeout', '20', '--interval', '0.05', 'pid', str(child.pid))
            self.assertEqual(code, 0, output)
        finally:
            child.wait()
        self.assertFalse(wait_for.process_alive(child.pid))
        code, output = self.run_main('--timeout', '20', '--interval', '0.05', 'run', '--', sys.executable, '-c',
                                     'import sys; sys.exit(7)')
        self.assertEqual(code, 7, output)
        with tempfile.TemporaryDirectory() as temp:
            marker = Path(temp) / 'grandchild.pid'
            script = ('import subprocess, sys, time\n'
                      'child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])\n'
                      'open(sys.argv[1], "w").write(str(child.pid))\n'
                      'time.sleep(60)\n')
            started = wait_for.time.monotonic()
            code, output = self.run_main('--timeout', '3', '--interval', '0.05', 'run', '--log', str(Path(temp) / 'o'),
                                         '--', sys.executable, '-c', script, str(marker))
            self.assertEqual(code, wait_for.TIMED_OUT, output)
            self.assertLess(wait_for.time.monotonic() - started, 30)
            self.assertIn('process tree was killed', output)
            grandchild = int(marker.read_text(encoding='utf-8'))
            deadline = wait_for.time.monotonic() + 10
            while wait_for.process_alive(grandchild) and wait_for.time.monotonic() < deadline:
                wait_for.time.sleep(0.1)
            self.assertFalse(wait_for.process_alive(grandchild), 'the command tree is killed, not only its head')

    def test_start_returns_at_once_and_the_supervisor_records_code_and_deadline(self):
        self.assertEqual(self.run_main('start', '--deadline', '3601', '--log', 'l', '--exit-file', 'e', '--', 'x')[0],
                         wait_for.USAGE)
        with tempfile.TemporaryDirectory() as temp:
            log, sentinel = Path(temp) / 'logs' / 'job.log', Path(temp) / 'job.rc'
            sentinel.write_text('0\n', encoding='utf-8')
            started = wait_for.time.monotonic()
            code, output = self.run_main('start', '--log', str(log), '--exit-file', str(sentinel), '--',
                                         sys.executable, '-c', 'import sys, time; time.sleep(1); print("built");'
                                         ' sys.exit(5)')
            self.assertEqual(code, 0, output)
            self.assertLess(wait_for.time.monotonic() - started, 5, 'start does not wait for the command')
            self.assertIsNone(wait_for.read_exit_code(sentinel), 'a stale sentinel is removed before the start')
            code, output = self.run_main('--timeout', '60', '--interval', '0.1', 'exit-file', str(sentinel), '--log',
                                         str(log))
            self.assertEqual(code, 5, output)
            self.assertIn('built', output)
            self.assertIn('exited 5', log.read_text(encoding='utf-8'))
            # Both streams reach the log through the supervisor, in order, whatever the command does with its handles.
            mixed = Path(temp) / 'mixed.log'
            script = ('import sys\nfor i in range(200):\n    print("out", i, flush=True)\n'
                      '    print("err", i, file=sys.stderr, flush=True)\n')
            self.assertEqual(self.run_main('run', '--log', str(mixed), '--', sys.executable, '-c', script)[0], 0)
            lines = mixed.read_text(encoding='utf-8').splitlines()
            self.assertEqual(lines, [line for i in range(200) for line in ('out %d' % i, 'err %d' % i)])
            code, output = self.run_main('start', '--deadline', '1', '--log', str(log), '--exit-file', str(sentinel),
                                         '--', sys.executable, '-c', 'import time; time.sleep(60)')
            self.assertEqual(code, 0, output)
            code, output = self.run_main('--timeout', '60', '--interval', '0.1', 'exit-file', str(sentinel))
            self.assertEqual(code, wait_for.TIMED_OUT, output)
            self.assertIn('reached the 1 s deadline', log.read_text(encoding='utf-8'))


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
            problems, libraries = package_consumer.check_layout(prefix, modules=('core',), assets={})
            self.assertEqual((problems, libraries), ([], ['crd-core.lib']))
            # Debug information as declared: pdb needs each archive's compile PDB beside it, the others none.
            problems, _ = package_consumer.check_layout(prefix, modules=('core',), debug_information='pdb', assets={})
            self.assertEqual(problems, ['debug information is pdb but crd-core.pdb is not installed beside its archive'])
            (prefix / 'lib/crd-core.pdb').write_bytes(b'pdb')
            self.assertEqual(package_consumer.check_layout(prefix, modules=('core',), debug_information='pdb',
                                                           assets={})[0], [])
            self.assertEqual(package_consumer.check_layout(prefix, modules=('core',), debug_information='dwarf',
                                                           assets={})[0],
                             ['debug information is dwarf but PDBs are installed: crd-core.pdb'])
            (prefix / 'lib/crd-core.pdb').unlink()
            # One archive per packaged module and no other; the assets byte for byte.
            (prefix / 'lib/libcrd-extra.a').write_bytes(b'!<arch>\n')
            source_assets = Path(temp) / 'assets'
            source_assets.mkdir()
            (source_assets / 'a.host.toml').write_text('kind = "diagnostic-host"\n', encoding='utf-8')
            (source_assets / 'b.flight.toml').write_text('kind = "flight-policy"\n', encoding='utf-8')
            (prefix / 'share/cerid/diag').mkdir(parents=True)
            (prefix / 'share/cerid/diag/a.host.toml').write_text('kind = "edited"\n', encoding='utf-8')
            (prefix / 'share/cerid/diag/stray.toml').write_text('', encoding='utf-8')
            problems, _ = package_consumer.check_layout(prefix, modules=('core', 'perf'), assets={'diag': source_assets})
            self.assertIn('no crd-perf archive under lib/', problems)
            self.assertIn('unexpected archive under lib/: libcrd-extra.a', problems)
            self.assertIn('asset not installed: share/cerid/diag/b.flight.toml', problems)
            self.assertIn('unexpected installed asset: share/cerid/diag/stray.toml', problems)
            self.assertIn('installed asset differs from its source: share/cerid/diag/a.host.toml', problems)
            self.assertTrue(any(p.startswith('public header not installed: crd/perf/') for p in problems), problems)
            (prefix / 'lib/libcrd-extra.a').unlink()
            shutil.rmtree(prefix / 'share')
            self.assertEqual(package_consumer.archive_module(Path('libcrd-hesap-dense.a')), 'hesap-dense')
            self.assertIsNone(package_consumer.archive_module(Path('crd-core.pdb')))
            (cmake_dir / 'CeridConfig.cmake').write_text('set(Cerid_LIBRARIES "crd-core crd-perf")\n'
                                                         'set(Cerid_DEBUG_INFORMATION "pdb")\n', encoding='utf-8')
            self.assertEqual(package_consumer.package_record(prefix),
                             {'Cerid_LIBRARIES': 'crd-core crd-perf', 'Cerid_DEBUG_INFORMATION': 'pdb'})
            (cmake_dir / 'CeridConfig.cmake').write_text('get_filename_component(_IMPORT_PREFIX '
                                                         '"${CMAKE_CURRENT_LIST_FILE}" PATH)\n', encoding='utf-8')
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
            problems, _ = package_consumer.check_layout(prefix, modules=('core',), assets={})
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

    def test_package_consumer_judges_symbols_and_the_diagnostic_applications_investigation(self):
        # The probes name what they rely on: an engine-only member, and the application's failing line.
        recorder = (ROOT / 'engine/foundation/perf/src/flight_recorder.cpp').read_text(encoding='utf-8')
        self.assertIn(package_consumer.ENGINE_ONLY_NAME.decode(), recorder)
        header = (ROOT / 'engine/foundation/perf/include/crd/perf/flight_recorder.hpp').read_text(encoding='utf-8')
        self.assertNotIn(package_consumer.ENGINE_ONLY_NAME.decode(), header)
        application = (package_consumer.DIAG_CONSUMER / 'main.cpp').read_text(encoding='utf-8')
        self.assertEqual(application.count(package_consumer.FAILURE_SOURCE), 1)
        self.assertIn(package_consumer.FAILURE_MESSAGE, application)
        self.assertEqual(sorted(package_consumer.MODULE_INCLUDES),
                         sorted(package_consumer.FAMILY))
        for include in package_consumer.MODULE_INCLUDES.values():
            self.assertTrue(include.is_dir(), include)

        def profile(compiler, build_type, **defines):
            return {'cxx_compiler': compiler, 'build_type': build_type, 'defines': defines}
        expected = package_consumer.expected_debug_information
        self.assertEqual(expected(profile('cl', 'Debug')), 'pdb')
        self.assertEqual(expected(profile('C:/VS/bin/cl.exe', 'Release', CRD_SHIPPING='ON')), 'pdb')
        self.assertEqual(expected(profile('cl', 'Debug', CRD_COMPILER_LAUNCHER='C:/sccache.exe')), 'embedded')
        self.assertEqual(expected(profile('cl', 'Release', CRD_SHIPPING='ON', CRD_COMPILER_LAUNCHER='sccache')),
                         'embedded', 'Shipping under the launcher compiles /Z7 as well')
        self.assertEqual(expected(profile('cl', 'Release', CRD_SHIPPING='ON', CRD_COMPILER_LAUNCHER='')), 'pdb')
        self.assertEqual(expected(profile('cl', 'Release')), 'none')
        self.assertEqual(expected(profile('clang-cl', 'RelWithDebInfo')), 'embedded')
        self.assertEqual(expected(profile('/usr/bin/g++-13', 'Debug')), 'dwarf')
        self.assertEqual(expected(profile('g++', 'Release', CRD_SHIPPING='OFF')), 'none')

        with tempfile.TemporaryDirectory() as temp:
            # A minimal 64-bit little-endian ELF with .shstrtab and one .debug_str section.
            names = b'\0.shstrtab\0.debug_str\0'
            debug = b'\0' + package_consumer.ENGINE_ONLY_NAME + b'\0'
            names_at, debug_at = 64, 64 + len(names)
            headers_at = debug_at + len(debug)
            header = bytearray(64)
            header[:6] = b'\x7fELF\x02\x01'
            header[0x28:0x30] = headers_at.to_bytes(8, 'little')
            header[0x3A:0x40] = (64).to_bytes(2, 'little') + (3).to_bytes(2, 'little') + (1).to_bytes(2, 'little')

            def section(name, kind, offset, size):
                entry = bytearray(64)
                entry[0:4] = name.to_bytes(4, 'little')
                entry[4:8] = kind.to_bytes(4, 'little')
                entry[24:32] = offset.to_bytes(8, 'little')
                entry[32:40] = size.to_bytes(8, 'little')
                return bytes(entry)
            image = bytes(header) + names + debug + section(0, 0, 0, 0) + section(1, 3, names_at, len(names)) + \
                section(11, 1, debug_at, len(debug))
            program = Path(temp) / 'diag-consumer'
            program.write_bytes(image)
            self.assertEqual(package_consumer.elf_sections(program, ('.debug_info', '.debug_str')), debug)
            self.assertIsNone(package_consumer.engine_debug_information_problem(program, 'dwarf'))
            program.write_bytes(image.replace(package_consumer.ENGINE_ONLY_NAME, b'x' * 21))
            self.assertIn('no engine DWARF', package_consumer.engine_debug_information_problem(program, 'dwarf'))
            windows = Path(temp) / 'diag-consumer.exe'
            windows.write_bytes(b'MZ')
            self.assertIsNone(package_consumer.elf_sections(windows, ('.debug_str',)))
            self.assertIn('not a 64-bit ELF', package_consumer.engine_debug_information_problem(windows, 'dwarf'))
            self.assertIn('no program database', package_consumer.engine_debug_information_problem(windows, 'pdb'))
            windows.with_suffix('.pdb').write_bytes(b'Microsoft C/C++ MSF 7.00' + package_consumer.ENGINE_ONLY_NAME)
            self.assertIsNone(package_consumer.engine_debug_information_problem(windows, 'pdb'))
            windows.with_suffix('.pdb').write_bytes(b'Microsoft C/C++ MSF 7.00')
            self.assertIn('LNK4099', package_consumer.engine_debug_information_problem(windows, 'pdb'))
            self.assertIsNone(package_consumer.engine_debug_information_problem(windows, 'none'))

        # The investigation: everything bundle.inspect must show, and each mismatch named.
        def page(items, summary=None, status='ok'):
            return {'status': status, 'summary': summary or {}, 'items': items}
        summary = {'status': 'ok', 'flight_record': True, 'flight_refused': False, 'build_identity': True}
        trigger = {'kind': 'trigger', 'trigger': 'fatal-invariant', 'code': package_consumer.FAILURE_CODE,
                   'file': 'C:\\scratch\\diag-consumer\\main.cpp', 'line': 122,
                   'message': package_consumer.FAILURE_MESSAGE}
        flight = {'kind': 'flight', 'end': 'fatal', 'detail': 'full', 'recorded_before_trigger': 40,
                  'pre_retained': 40, 'events': 40, 'replay_available': True,
                  'replay': 'diag-consumer fail <profile.host.toml>'}
        events = [{'kind': 'event', 'index': i, 'side': 'before', 'code': 0x00A10001, 'value_a': i,
                   'value_b': 1 if i % 7 == 6 else 0, 'message': f'order {i} booked {-900 if i % 7 == 6 else 25}',
                   'severity': 'warning' if i % 7 == 6 else 'info'} for i in range(40)]
        build = {'kind': 'build', 'same_as_host': True, 'fault_seams': True}
        symbol = {'kind': 'symbol', 'name': 'diag-consumer.exe'}
        pages = [page([trigger, flight] + events[:20], summary), page(events[20:] + [build, symbol], summary)]
        judge = package_consumer.investigation_problems
        self.assertEqual(judge(pages, 122, 'diag-consumer.exe', True), [])
        self.assertEqual(judge(pages, 123, 'diag-consumer.exe', True), ['trigger line: expected 123, got 122'])
        self.assertEqual(judge(pages, 122, 'other.exe', True),
                         ["no module identity names other.exe: ['diag-consumer.exe']"])
        self.assertEqual(judge(pages, 122, 'diag-consumer.exe', False),
                         ['build fault_seams: expected False, got True'])
        shifted = [page([trigger, flight] + events[1:] + [build, symbol], summary)]
        self.assertEqual(judge(shifted, 122, 'diag-consumer.exe', True), ['expected 40 events, got 39'])
        wrong_side = [dict(event, side='after') if event['index'] == 39 else event for event in events]
        problems = judge([page([trigger, flight] + wrong_side + [build, symbol], summary)], 122, 'diag-consumer.exe',
                         True)
        self.assertEqual(len(problems), 1, problems)
        self.assertTrue(problems[0].startswith('event 39:'), problems)
        refused = dict(summary, flight_record=False, flight_refused=True)
        problems = judge([page([build, symbol], refused)], 122, 'diag-consumer.exe', True)
        self.assertIn("summary flight_record: expected True, got False", problems)
        self.assertIn('no trigger item', problems)
        self.assertEqual(judge([], 122, 'diag-consumer.exe', True), ['inspect printed no page'])
        refund = [dict(event, value_b=0) if event['index'] == 6 else event for event in events]
        problems = judge([page([trigger, flight] + refund + [build, symbol], summary)], 122, 'diag-consumer.exe', True)
        self.assertEqual(len(problems), 1, problems)
        self.assertTrue(problems[0].startswith('event 6:'), problems)

        # The same failure at the basic detail: only the five refunds (orders 6, 13, 20, 27, 34) are kept, renumbered.
        self.assertEqual(package_consumer.REFUNDS, (6, 13, 20, 27, 34))
        basic_flight = dict(flight, detail='basic', recorded_before_trigger=5, pre_retained=5, events=5)
        basic_events = [dict(events[order], index=index) for index, order in enumerate(package_consumer.REFUNDS)]
        basic = [page([trigger, basic_flight] + basic_events + [build, symbol], summary)]
        self.assertEqual(judge(basic, 122, 'diag-consumer.exe', True, 'basic'), [])
        self.assertEqual(judge(basic, 122, 'diag-consumer.exe', True),
                         ["flight detail: expected 'full', got 'basic'",
                          'flight recorded_before_trigger: expected 40, got 5',
                          'flight pre_retained: expected 40, got 5', 'flight events: expected 40, got 5',
                          'expected 40 events, got 5'])
        self.assertIn('flight detail: expected \'basic\', got \'full\'',
                      judge(pages, 122, 'diag-consumer.exe', True, 'basic'))
        informational = [page([trigger, basic_flight] + [dict(events[0], index=0)] + basic_events[1:] + [build, symbol],
                              summary)]
        problems = judge(informational, 122, 'diag-consumer.exe', True, 'basic')
        self.assertEqual(len(problems), 1, problems)
        self.assertTrue(problems[0].startswith('event 0:'), problems)

        # The application's sites formatted exactly the messages the recorder kept: an eager site formats all forty.
        sites = package_consumer.sites_problems
        self.assertEqual(sites('CONSUMER_SITES detail=full formatted=40 recorded=40 filtered=0\n', 'full'), [])
        self.assertEqual(sites('CONSUMER_SITES detail=basic formatted=5 recorded=5 filtered=35\n', 'basic'), [])
        self.assertEqual(sites('CONSUMER_SITES detail=basic formatted=40 recorded=5 filtered=35\n', 'basic'),
                         ["CONSUMER_SITES formatted: expected '5', got '40'"])
        self.assertEqual(sites('CONSUMER_SITES detail=full formatted=40 recorded=40 filtered=0\n', 'basic'),
                         ["CONSUMER_SITES detail: expected 'basic', got 'full'",
                          "CONSUMER_SITES formatted: expected '5', got '40'",
                          "CONSUMER_SITES recorded: expected '5', got '40'",
                          "CONSUMER_SITES filtered: expected '35', got '0'"])
        self.assertEqual(sites('no sites line\n', 'full'), ['CONSUMER_SITES: line missing'])
        self.assertEqual(package_consumer.inspect_pages('noise\nCONSUMER_INSPECT_PAGE {"status":"ok"}\n'),
                         [{'status': 'ok'}])
        caps = {'items': [{'name': 'fault-injection', 'compiled': False}]}
        self.assertEqual(package_consumer.capability(caps, 'fault-injection'), {'name': 'fault-injection',
                                                                                'compiled': False})
        self.assertIsNone(package_consumer.capability(caps, 'basic-recording'))

    def test_package_consumer_judges_the_diagnostic_applications_examples(self):
        pc = package_consumer
        # The script's codes are the application's: a drift between the two would judge the wrong events.
        header = (pc.DIAG_CONSUMER / 'consumer_support.hpp').read_text(encoding='utf-8')
        codes = {name: int(value, 16) for name, value in re.findall(r'(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)U;', header)}
        self.assertEqual((codes['kOrderReserved'], codes['kOrderReleased'], codes['kHeapViolation'],
                          codes['kSliceSummed'], codes['kLimitsKept'], codes['kFrameWork']),
                         (pc.CODE_ORDER_RESERVED, pc.CODE_ORDER_RELEASED, pc.CODE_HEAP_VIOLATION,
                          pc.CODE_SLICE_SUMMED, pc.CODE_LIMITS_KEPT, pc.CODE_FRAME_WORK))
        sources = {path.name for path in pc.DIAG_CONSUMER.glob('example_*.cpp')}
        self.assertEqual(sources, {'example_allocator.cpp', 'example_jobs.cpp', 'example_reload.cpp',
                                   'example_capture.cpp'})
        self.assertEqual({name.partition('-')[0] for name in pc.EXAMPLES}, {'allocator', 'jobs', 'reload', 'capture'})
        self.assertEqual(pc.author_assignments('a = 1\nb = "x"\n', {'b': '"y"'}, 'asset'), 'a = 1\nb = "y"\n')
        with self.assertRaises(pc.ConsumerError):
            pc.author_assignments('a = 1\n', {'b': '2'}, 'asset')
        self.assertEqual(pc.marker_fields('CONSUMER_X a=1 reason="two words" b=c\nCONSUMER_XY a=2\n', 'CONSUMER_X'),
                         [{'a': '1', 'reason': 'two words', 'b': 'c'}])

        def page(items):
            return [{'status': 'ok', 'items': items}]

        def event(index, side, code, a=0, b=0, message=''):
            return {'kind': 'event', 'index': index, 'side': side, 'code': code, 'value_a': a, 'value_b': b,
                    'message': message}

        # Allocator selection: the diagnostic choice, as the application prints and bundles it.
        booked = 6
        output = '\n'.join([
            'CONSUMER_ALLOCATOR chosen=orders-diagnostic heap_bytes=65536 order_bytes=1024',
            'CONSUMER_ALLOC_CHECKED violations=1',
            f'CONSUMER_ALLOC_EXHAUSTED booked={booked} exhausted=1',
            'CONSUMER_BUNDLE label=allocation-failure name=flight-0.cdb written=1 failures=0',
            'CONSUMER_ALLOC_DIAGNOSTIC violations=3 first=redzone-overrun offset=1024 leaks=0 quarantined_bytes=4096'])
        trigger = {'kind': 'trigger', 'trigger': 'allocation-failure', 'value_a': 1024,
                   'message': 'allocator orders-diagnostic failed to allocate 1024 bytes (alignment 16)'}
        flight = {'kind': 'flight', 'end': 'post-events-full'}
        before = [event(i, 'before', pc.CODE_ORDER_RESERVED, i, 1024) for i in range(3)]
        before += [event(3, 'before', pc.CODE_HEAP_VIOLATION, 1, 1024)]
        before += [event(4 + i, 'before', pc.CODE_ORDER_RESERVED, 3 + i, 1024) for i in range(booked - 3)]
        after = [event(10, 'after', pc.CODE_ORDER_RELEASED, 0), event(11, 'after', pc.CODE_ORDER_RELEASED, 1)]
        pages = page([trigger, flight] + before + after)
        self.assertEqual(pc.allocator_problems(output, pages, 'diagnostic'), [])
        problems = pc.allocator_problems(output, pages, 'tlsf')
        self.assertIn("CONSUMER_ALLOCATOR chosen: expected 'orders-tlsf', got 'orders-diagnostic'", problems)
        self.assertIn('the tlsf choice reported diagnostic-allocator results', problems)
        self.assertIn('the tlsf choice recorded heap violations', problems)
        moved = [before[0], before[1], before[3], before[2]] + before[4:]
        self.assertIn('the overrun is not recorded right after order 2 was reserved',
                      pc.allocator_problems(output, page([trigger, flight] + moved + after), 'diagnostic'))
        leaked = output.replace('leaks=0', 'leaks=1')
        self.assertEqual(pc.allocator_problems(leaked, pages, 'diagnostic'),
                         ["CONSUMER_ALLOC_DIAGNOSTIC leaks: expected '0', got '1'"])
        self.assertIn('the bundle does not hold the releases after the failure: []',
                      pc.allocator_problems(output, page([trigger, flight] + before), 'diagnostic'))

        # Jobs: the known total, the hang, the slices before it.
        n = pc.SLICES * pc.SLICE_LENGTH
        output = '\n'.join([
            'CONSUMER_JOBS_POOL threads=4 watchdog_ms=50',
            f'CONSUMER_JOBS_SUM slices=64 total={n * (n + 1) // 2} expected={n * (n + 1) // 2}',
            'CONSUMER_JOBS_STALL captured=1 late_job_ran_before_pump=0',
            'CONSUMER_BUNDLE label=hang name=flight-0.cdb written=1 failures=0',
            'CONSUMER_JOBS_RECOVERED late_job_ran=1'])
        slices = [event(i, 'before', pc.CODE_SLICE_SUMMED, i,
                        sum(range(i * pc.SLICE_LENGTH + 1, (i + 1) * pc.SLICE_LENGTH + 1))) for i in range(pc.SLICES)]
        hang = {'kind': 'trigger', 'trigger': 'hang', 'value_a': 0, 'value_b': 1}
        self.assertEqual(pc.jobs_problems(output, page([hang] + slices[::-1]), 50), [])
        self.assertEqual(pc.jobs_problems(output, page([hang] + slices[1:]), 50),
                         ['the bundle does not hold the 64 summed slices before the hang'])
        self.assertEqual(pc.jobs_problems(output.replace('before_pump=0', 'before_pump=1'), page([hang] + slices), 50),
                         ["CONSUMER_JOBS_STALL late_job_ran_before_pump: expected '0', got '1'"])
        self.assertEqual(pc.jobs_problems(output, page([dict(hang, value_a=2)] + slices), 50),
                         ['trigger value_a: expected 0, got 2'])

        # Reload, with fault seams and without.
        common = [
            'CONSUMER_RELOAD_DATA attempt=1 result=loaded why=none generation=1 max_order=500',
            'CONSUMER_RELOAD_APPLY step=unchanged answer=unchanged policy_generation=1 watchdog_ms=50 reason=""',
            'CONSUMER_RELOAD_APPLY step=policy-edited answer=applied policy_generation=2 watchdog_ms=50 reason=""',
            f'CONSUMER_RELOAD_POLICY hitch_threshold_ns={2 * pc.EXAMPLE_HITCH_NS}',
            'CONSUMER_RELOAD_APPLY step=watchdog-changed answer=restart-required policy_generation=2 watchdog_ms=50 '
            'reason="these fields take effect at the next start: watchdog_period_ms"',
            'CONSUMER_RELOAD_APPLY step=capture-dir-changed answer=applied policy_generation=2 watchdog_ms=50 '
            'reason=""']
        seams = '\n'.join(common + [
            'CONSUMER_COMMAND command=fault.arm status=ok {"plan":"seed=1;failed-read@0+1%1000000"}',
            'CONSUMER_RELOAD_DATA attempt=2 result=kept why=read-failed generation=1 max_order=500',
            'CONSUMER_BUNDLE label=injected-fault name=flight-0.cdb written=1 failures=0',
            'CONSUMER_COMMAND command=fault.disarm status=ok {}',
            'CONSUMER_RELOAD_DATA attempt=3 result=loaded why=none generation=2 max_order=750'])
        injected = {'kind': 'trigger', 'trigger': 'injected-fault',
                    'message': 'fault-plan/1 fired=failed-read#0 minimal=seed=1;failed-read@0+1%1000000'}
        fault_events = [event(0, 'before', pc.CODE_FAULT_INJECTED, 1, 0,
                              'injected failed-read at hit 0 (seed 1) on limits.toml'),
                        event(1, 'after', pc.CODE_LIMITS_KEPT, 1, 500)]
        self.assertEqual(pc.reload_problems(seams, page([injected] + fault_events), True, 50), [])
        self.assertIn('no event after the trigger says generation 1 (max_order 500) was kept',
                      pc.reload_problems(seams, page([injected] + fault_events[:1]), True, 50))
        unchanged_generation = seams.replace('policy_generation=2 watchdog_ms=50 reason=""\nCONSUMER_RELOAD_POLICY',
                                             'policy_generation=1 watchdog_ms=50 reason=""\nCONSUMER_RELOAD_POLICY')
        self.assertIn('the edited policy did not take a new generation (1 -> 1)',
                      pc.reload_problems(unchanged_generation, page([injected] + fault_events), True, 50))
        self.assertIn("reload 2 result: expected 'kept', got 'loaded'",
                      pc.reload_problems(seams.replace('result=kept why=read-failed', 'result=loaded why=none'),
                                         page([injected] + fault_events), True, 50))
        none = '\n'.join(common + [
            'CONSUMER_COMMAND command=fault.arm status=unknown-command {}',
            'CONSUMER_RELOAD_DATA attempt=2 result=loaded why=none generation=2 max_order=750',
            'CONSUMER_RELOAD_DATA attempt=3 result=loaded why=none generation=3 max_order=750'])
        self.assertEqual(pc.reload_problems(none, None, False, 50), [])
        self.assertIn("fault.arm answered ['unknown-command']", pc.reload_problems(none, None, True, 50))
        self.assertIn('a build without fault seams captured an injected fault',
                      pc.reload_problems(none, page([injected]), False, 50))

        # Capture: three requests, two bundles kept, the frames around the last request.
        output = '\n'.join(['CONSUMER_COMMAND command=flight.trigger status=ok {}'] * 3 + [
            'CONSUMER_COMMAND command=flight.status status=ok {}',
            'CONSUMER_CAPTURE requested=3 written=3 all_written=1 frames=140 hitches=0 max_bundles=2',
            'CONSUMER_BUNDLE label=manual name=flight-2.cdb written=3 failures=0'])
        manual = {'kind': 'trigger', 'trigger': 'manual', 'message': 'capture requested at frame 100'}
        window = {'kind': 'flight', 'end': 'post-events-full', 'post_retained': 16}
        frames = [event(i, 'before', pc.CODE_FRAME_WORK, 98 + i) for i in range(3)]
        frames += [event(3 + i, 'after', pc.CODE_FRAME_WORK, 101 + i) for i in range(4)]
        kept = ['flight-1.cdb', 'flight-2.cdb']
        self.assertEqual(pc.capture_problems(output, page([manual, window] + frames), kept), [])
        self.assertIn("the capture directory keeps ['flight-0.cdb', 'flight-1.cdb', 'flight-2.cdb']; the newest 2 "
                      "are ['flight-1.cdb', 'flight-2.cdb']",
                      pc.capture_problems(output, page([manual, window] + frames), ['flight-0.cdb'] + kept))
        gap = frames[:3] + frames[4:]
        self.assertEqual(pc.capture_problems(output, page([manual, window] + gap), kept),
                         ['the frames after the request do not follow frame 100: [102, 103, 104]'])
        one_fewer = output.replace('CONSUMER_COMMAND command=flight.trigger status=ok {}\n', '', 1)
        self.assertIn("flight.trigger answered ['ok', 'ok']",
                      pc.capture_problems(one_fewer, page([manual, window] + frames), kept))

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
        # GCC cannot compile a header with #pragma once as its main file under -Werror: a unit includes it.
        through, unit, text = check_headers.include_unit(arguments, 'gnu', '/repo/include/crd/y.hpp', scratch, 'y_hpp')
        self.assertEqual(unit, scratch / 'y_hpp.cpp')
        self.assertEqual(through[-1], str(unit))
        self.assertNotIn('/repo/include/crd/y.hpp', through)
        self.assertEqual(text, '#include "/repo/include/crd/y.hpp"\n')
        self.assertEqual(check_headers.include_unit(['cl.exe', 'x.hpp'], 'msvc', 'x.hpp', scratch, 'x_hpp'),
                         (['cl.exe', 'x.hpp'], None, None))


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

    def test_every_registered_fuzz_target_has_a_driver_entry(self):
        # DIAG.10a: every ingress surface has a fuzz owner. Each crd_fuzz_target() in the tree has exactly one
        # fuzz.py entry naming its committed corpus and its build-relative executable, and no entry is stale.
        aliases = dict(re.findall(r'^crd_tests\(([\w/.-]+) ([\w.-]+)\)', (ROOT / 'tests/CMakeLists.txt').read_text(
            encoding='utf-8'), re.MULTILINE))
        registered = {}
        for cmake in sorted((ROOT / 'tests').rglob('CMakeLists.txt')):
            text = cmake.read_text(encoding='utf-8')
            pattern = r'crd_fuzz_target\(([\w-]+).*?CORPUS "\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^"]+)"'
            for name, corpus in re.findall(pattern, text, re.DOTALL):
                directory = cmake.parent.relative_to(ROOT / 'tests')
                owner = next((source for source in aliases if directory.as_posix() == source
                              or directory.as_posix().startswith(source + '/')), None)
                self.assertIsNotNone(owner, f'{cmake}: {name} lies in no crd_tests() directory')
                build = Path('tests') / aliases[owner] / directory.relative_to(owner) / name
                registered[name] = {'corpus': (Path('tests') / directory / corpus).as_posix(),
                                    'executable': build.as_posix()}
        self.assertGreaterEqual(len(registered), 6)
        driver = {Path(entry['executable']).name: entry for entry in fuzz_driver.TARGETS.values()}
        self.assertEqual(sorted(driver), sorted(registered), 'fuzz.py TARGETS and the crd_fuzz_target() set differ')
        for name, entry in registered.items():
            self.assertEqual(driver[name], entry, name)

    def test_fuzz_driver_bounds_every_run_and_names_its_corpora(self):
        self.assertEqual(sorted(fuzz_driver.TARGETS), ['ceir-binary', 'ceir-differential', 'ceir-record', 'ceir-text',
                                                       'ceridc-diag', 'ckir-binary', 'ckir-text', 'memory-allocators',
                                                       'perf-assets', 'perf-bundle', 'perf-capture', 'perf-flight',
                                                       'resources-reload'])
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

    def test_host_check_counts_every_report_and_unfinished_process(self):
        # DIAG.10a host separation: an event is any report line (with its stack head), and a process that died before
        # its final statistics still counts the executions its last status line shows, marked as a lower bound.
        log = ('INFO: Seed: 7101\n#2\tINITED cov: 10\n#262144\tpulse  cov: 10\n'
               '==12==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000004\n'
               '    #0 0x55d1 in asan_annotate_live_range array.hpp:126:1\n'
               '    #1 0x55d2 in crd::containers::Array<unsigned char>::push_back(unsigned char) array.hpp:342\n\n'
               'SUMMARY: AddressSanitizer: SEGV\n')
        reports = fuzz_host_check.reports_in(log)
        self.assertEqual(len(reports), 1, reports)
        self.assertIn('AddressSanitizer: SEGV', reports[0][0])
        self.assertEqual(len(reports[0]), 3)
        self.assertEqual(fuzz_host_check.executions_of(log, {}), (262144, False))
        finished = log + 'stat::number_of_executed_units: 300000\n'
        self.assertEqual(fuzz_host_check.executions_of(finished, fuzz_driver.parse_stats(finished)), (300000, True))
        ubsan = 'array.hpp:618:5: runtime error: member call on address 0x1 with insufficient space\n'
        self.assertEqual(len(fuzz_host_check.reports_in(ubsan)), 1)
        clean = 'INFO: Seed: 1\nDone 300000 runs in 540 second(s)\nstat::number_of_executed_units: 300000\n'
        self.assertEqual(fuzz_host_check.reports_in(clean), [])
        repeat = 'Running: unit\nExecuted unit in 3 ms\n*** NOTE: fuzzing was not performed\n'
        self.assertEqual(fuzz_host_check.reports_in(repeat), [])
        self.assertEqual([arm for arm, _ in fuzz_host_check.ARMS], ['default', 'container-overflow-off'])
        self.assertEqual(fuzz_host_check.options('', 'detect_container_overflow=0'), 'detect_container_overflow=0')
        self.assertTrue(fuzz_host_check.options(fuzz_host_check.REPEAT_ASAN_OPTIONS, 'detect_container_overflow=0')
                        .endswith(':halt_on_error=1:detect_container_overflow=0'))
        self.assertTrue(fuzz_host_check.find_unit('ceir-text', 'cd301be8').is_file())
        self.assertTrue(fuzz_host_check.find_unit('ckir-binary', 'ace35b97').is_file())
        with self.assertRaises(fuzz_host_check.fuzz.FuzzError):  # its own import of fuzz.py
            fuzz_host_check.find_unit('ceir-text', 'seed-')

    def test_tsan_host_check_counts_every_failed_run_as_an_event(self):
        # JOBS-HARD.3 host separation: a run is clean only with a zero exit, no report line and its pass line; a
        # ThreadSanitizer SEGV, the control's "=== WRONG" abort, a report under a zero exit, a missing pass line and a
        # timeout are each an event.
        passed = 'Filters: "fiber keep: while tiny jobs stream*"\nAll tests passed (16 assertions in 1 test case)\n'
        self.assertIsNone(tsan_host_check.classify(0, passed, 'All tests passed'))
        self.assertIsNone(tsan_host_check.classify(0, 'cycle 0 done\ncycle 1 done, 204800 jobs\n', 'cycle 1 done'))
        segv = ('ThreadSanitizer:DEADLYSIGNAL\n==27426==ERROR: ThreadSanitizer: SEGV on unknown address '
                '0x03e800006b22 (pc 0x5555557207b3)\n    #0 __tsan_atomic32_load\n')
        self.assertEqual(tsan_host_check.classify(66, segv, 'All tests passed'), 'exit 66')
        reports = tsan_host_check.reports_in(segv)
        self.assertEqual(len(reports), 2, reports)
        self.assertIn('SEGV on unknown address 0x03e800006b22', reports[1][0])
        self.assertEqual(reports[1][1], '#0 __tsan_atomic32_load')
        wrong = '=== WRONG progress: got 0x7ffff36fb5f0, want 0x7240000000c0\n'
        self.assertEqual(tsan_host_check.classify(134, wrong, 'cycle 1 done'), 'exit 134')
        self.assertEqual(tsan_host_check.classify(0, wrong + 'cycle 1 done\n', 'cycle 1 done'), 'report')
        race = 'WARNING: ThreadSanitizer: data race (pid=28158)\n  Write of size 8\n' + passed
        self.assertEqual(tsan_host_check.classify(0, race, 'All tests passed'), 'report')
        self.assertEqual(tsan_host_check.classify(0, 'cycle 0 done\n', 'cycle 1 done'), 'no pass line')
        self.assertEqual(tsan_host_check.classify(0, passed, 'All tests passed', timed_out=True), 'timeout')
        self.assertEqual(tsan_host_check.pinned([2, 3], ['exe', 'plain']), ['taskset', '-c', '2,3', 'exe', 'plain'])
        self.assertEqual([arm[0] for arm in tsan_host_check.ARMS],
                         ['jobs', 'control-fiber', 'control-plain', 'control-unsanitized'])
        records = [{'arm': 'jobs', 'event': None, 'wall_seconds': 1.5},
                   {'arm': 'jobs', 'event': 'exit 66', 'wall_seconds': 0.5},
                   {'arm': 'control-plain', 'event': 'report', 'wall_seconds': 2.0}]
        self.assertEqual(tsan_host_check.tally(records)[0], {'arm': 'jobs', 'runs': 2, 'events': 1, 'seconds': 2.0})
        self.assertEqual(tsan_host_check.tally(records)[2]['events'], 1)
        self.assertTrue(tsan_host_check.CONTROL.is_file())
        # The jobs arm runs under the options of the linux-clang-tsan test preset, so it sees what the CTest sees.
        presets = json.loads((ROOT / 'CMakePresets.json').read_text(encoding='utf-8'))
        tsan = next(preset for preset in presets['testPresets'] if preset['name'] == 'linux-clang-tsan')
        expected = tsan['environment']['TSAN_OPTIONS'].replace('${sourceDir}', ROOT.as_posix())
        self.assertEqual(tsan_host_check.TSAN_OPTIONS, expected)

    def test_host_check_reads_mmap_rnd_bits_through_each_route(self):
        # The /proc file is root-only on hosted runners and sysctl reads the same file: each route is tried in turn,
        # only a zero exit with a number counts, and 'unknown' is left only when every route fails.
        def result(code, out='', err=''):
            return subprocess.CompletedProcess([], code, stdout=out, stderr=err)

        def runner(answers):
            calls = []

            def run(command, **kwargs):
                calls.append(command[0])
                self.assertEqual(kwargs.get('timeout'), 10)
                answer = answers[len(calls) - 1]
                if isinstance(answer, BaseException):
                    raise answer
                return answer
            return run, calls

        denied = result(255, err="sysctl: permission denied on key 'vm.mmap_rnd_bits'\n")
        run, calls = runner([])
        self.assertEqual(fuzz_host_check.mmap_rnd_bits(read=lambda path: '28\n', run=run), '28')
        self.assertEqual(calls, [])
        run, calls = runner([result(0, '32\n')])
        self.assertEqual(fuzz_host_check.mmap_rnd_bits(read=lambda path: '', run=run), '32')
        self.assertEqual(calls, ['sysctl'])
        run, calls = runner([denied, result(0, '28\n')])
        self.assertEqual(fuzz_host_check.mmap_rnd_bits(read=lambda path: '', run=run), '28')
        self.assertEqual(calls, ['sysctl', 'sudo'])
        run, calls = runner([FileNotFoundError('sysctl'), result(0, '30\n')])
        self.assertEqual(fuzz_host_check.mmap_rnd_bits(read=lambda path: '', run=run), '30')
        run, calls = runner([denied, result(1, err='sudo: a password is required\n')])
        self.assertEqual(fuzz_host_check.mmap_rnd_bits(read=lambda path: '', run=run), 'unknown')
        self.assertEqual(calls, ['sysctl', 'sudo'])
        run, calls = runner([result(0, 'not a number\n'), subprocess.TimeoutExpired('sudo', 10)])
        self.assertEqual(fuzz_host_check.mmap_rnd_bits(read=lambda path: 'garbage', run=run), 'unknown')

    def test_instrument_options_fail_the_configure_instead_of_warning(self):
        text = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertNotIn('UBSan not supported on MSVC, ignoring', text)
        self.assertIn('-fno-sanitize-recover=undefined', text)
        self.assertIn('CRD_ENABLE_TSAN and CRD_ENABLE_ASAN are exclusive', text)
        self.assertIn('CRD_ENABLE_FUZZER needs clang', text)
        header = (ROOT / 'engine/foundation/jobs/src/sanitizer_fibers.hpp').read_text(encoding='utf-8')
        for symbol in ('__sanitizer_start_switch_fiber', '__sanitizer_finish_switch_fiber', '__tsan_create_fiber',
                       '__tsan_switch_to_fiber', '__tsan_destroy_fiber', '__tsan_func_exit'):
            self.assertIn(symbol, header)
        # The switch hooks run in their caller's frame at every optimisation level (TSan's lane is -O0).
        for hook in ('sanitizer_switch_begin(', 'sanitizer_switch_end(', 'sanitizer_fiber_finished('):
            self.assertIn('CRD_ALWAYS_INLINE void ' + hook, header)
        platform = (ROOT / 'engine/foundation/core/include/crd/core/platform.hpp').read_text(encoding='utf-8')
        start = platform.rindex('#if', 0, platform.index('#define CRD_ALWAYS_INLINE'))
        self.assertNotIn('__OPTIMIZE__', platform[start:platform.index('#endif', start)])
        # Switches, then finishes: the trampoline's entry also restores the fiber's fake stack, and the completing
        # trampoline pops its TSan frame once.
        sites = {'engine/foundation/jobs/src/counter.cpp': (1, 1, 0),
                 'engine/foundation/jobs/src/worker_pool.cpp': (2, 3, 1)}
        for path, (switches, ends, finished) in sites.items():
            source = (ROOT / path).read_text(encoding='utf-8')
            self.assertEqual(source.count('fiber_switch('), switches, path)
            self.assertEqual(source.count('sanitizer_switch_begin('), switches, path)
            self.assertEqual(source.count('sanitizer_switch_end('), ends, path)
            self.assertEqual(source.count('sanitizer_fiber_finished('), finished, path)


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


class CompileCommands(unittest.TestCase):
    CL = r'C:\PROGRA~1\MICROS~1\18\COMMUN~1\VC\Tools\MSVC\14.51\bin\Hostx64\x64\cl.exe'

    def entry(self, command, name='a.cpp'):
        return {'directory': 'D:/build', 'command': command, 'file': 'D:/src/' + name}

    def test_a_second_level_is_reported_with_its_file(self):
        checked, problems = compile_commands.check([
            self.entry(self.CL + ' /nologo /TP /O2 /Ob2 /DNDEBUG /Zi /W4 /WX /Od /Y- -c a.cpp', 'worker_pool.cpp'),
            self.entry(self.CL + ' /nologo /TP /O2 /Ob2 /DNDEBUG -c b.cpp', 'b.cpp')])
        self.assertEqual(2, checked)
        self.assertEqual(['D:/src/worker_pool.cpp: optimization levels /O2 then /Od on one command '
                          '(cl.exe D9025; the last one wins)'], problems)

    def test_one_level_a_repeat_and_inline_expansion_pass(self):
        checked, problems = compile_commands.check([
            self.entry(self.CL + ' /TP /Ob2 /DNDEBUG /Od -c a.cpp'),
            self.entry(self.CL + ' /TP /O2 /Ob2 /O2 -c b.cpp'),
            self.entry(self.CL + ' /TP /Ob0 /Od /RTC1 -c c.cpp')])
        self.assertEqual((3, []), (checked, problems))

    def test_clang_cl_a_launcher_quotes_and_the_dash_form(self):
        quoted = '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo -TP -O2 /Od -c -- a.cpp'
        launched = {'file': 'D:/src/b.cpp', 'arguments': ['sccache.exe', self.CL, '/O1', '/Ox', '-c', 'b.cpp']}
        checked, problems = compile_commands.check([self.entry(quoted), launched])
        self.assertEqual(2, checked)
        self.assertEqual(['D:/src/a.cpp: optimization levels /O2 then /Od on one command '
                          '(cl.exe D9025; the last one wins)',
                          'D:/src/b.cpp: optimization levels /O1 then /Ox on one command '
                          '(cl.exe D9025; the last one wins)'], problems)

    def test_other_compilers_are_not_judged_and_an_empty_check_fails(self):
        gcc = [self.entry('/usr/bin/g++ -O2 -g -O0 -c a.cpp'), self.entry('/usr/bin/clang++ -O1 -O3 -c b.cpp')]
        self.assertEqual((0, []), compile_commands.check(gcc))
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / 'compile_commands.json'
            path.write_text(json.dumps(gcc), encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(1, compile_commands.main(['check', str(path)]))
            self.assertIn('holds no cl.exe or clang-cl compile command; nothing was checked', out.getvalue())
            path.write_text(json.dumps(gcc + [self.entry(self.CL + ' /O2 -c c.cpp', 'c.cpp')]), encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(0, compile_commands.main(['check', str(path)]))
            self.assertIn('Checked 1 MSVC-style compile commands', out.getvalue())
            path.write_text(json.dumps([self.entry(self.CL + ' /O2 /Od -c d.cpp', 'd.cpp')]), encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(1, compile_commands.main(['check', str(path)]))
            self.assertIn('ERROR: D:/src/d.cpp: optimization levels /O2 then /Od', out.getvalue())


class AllocationResults(unittest.TestCase):
    def test_an_unchecked_result_is_reported_by_line(self):
        text = ('void f(crd::memory::IAllocator* a)\n'
                '{\n'
                '    void* const mem = a->allocate(16U, 8U);\n'
                '    auto* t = new (mem) T();\n'
                '    auto* p = static_cast<int*>(\n'
                '        a->allocate(4U * sizeof(int), alignof(int)));\n'
                '    p[0] = 1;\n'
                '    auto* m = crd::memory::construct<Model>(*a, 3);\n'
                '    m->run();\n'
                '}\n')
        self.assertEqual([3, 6, 8], allocation_results.unchecked_calls(text))

    def test_checked_passed_up_conditional_and_fatal_forms_pass(self):
        text = ('void* Wrapper::allocate(usize n, usize al) { return m_backing->allocate(n, al); }\n'
                'void g(crd::memory::IAllocator* a, Gpu& gpu)\n'
                '{\n'
                '    void* raw = a->allocate(16U, 8U);\n'
                '    if (raw == nullptr)\n'
                '    {\n'
                '        return;\n'
                '    }\n'
                '    auto* q = static_cast<T*>(a->try_allocate(sizeof(T), alignof(T)));\n'
                '    if (!q)\n'
                '    {\n'
                '        return;\n'
                '    }\n'
                '    if (!gpu.allocate(desc, out))\n'
                '    {\n'
                '        return;\n'
                '    }\n'
                '    const Allocation x = oa.allocate(size, align);\n'
                '    if (x.valid())\n'
                '    {\n'
                '        use(x);\n'
                '    }\n'
                '    auto* h = crd::memory::default_allocator()->allocate(8U, 8U);\n'
                '    auto* f = crd::memory::allocate_array_or_fatal<int>(*a, 4U, "out of memory");\n'
                '    auto* c = crd::memory::construct_or_fatal<T>(*a, "out of memory", 1);\n'
                '    Slot s = m_slots.allocate();\n'
                '    // a->allocate(1U, 1U) in a comment\n'
                '}\n')
        self.assertEqual([], allocation_results.unchecked_calls(text))

    def test_a_check_too_far_from_the_call_does_not_count(self):
        filler = ''.join('    step(%d);\n' % i for i in range(allocation_results.WINDOW + 1))
        text = '    void* p = a->allocate(8U, 8U);\n' + filler + '    if (p == nullptr)\n'
        self.assertEqual([1], allocation_results.unchecked_calls(text))

    def test_the_tree_check_counts_allowances_and_fails_an_empty_tree(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(1, allocation_results.main(['check', str(root)]))
            self.assertIn('nothing was checked', out.getvalue())
            source = root / 'engine' / 'a.cpp'
            source.parent.mkdir(parents=True)
            source.write_text('void f(I* a)\n{\n    int* p = static_cast<int*>(a->allocate(4U, 4U));\n'
                              '    *p = 1;\n}\n', encoding='utf-8')
            seen, problems = allocation_results.check(root)
            self.assertEqual(1, seen)
            self.assertEqual(1, len(problems))
            self.assertIn('engine/a.cpp: allocation result used without a check at line(s) 3', problems[0])
            with patch.dict(allocation_results.ALLOWED, {'engine/a.cpp': (1, 'a heap allocator')}):
                self.assertEqual((1, []), allocation_results.check(root))
            with patch.dict(allocation_results.ALLOWED, {'engine/a.cpp': (2, 'a heap allocator')}):
                seen, problems = allocation_results.check(root)
                self.assertIn('1 unchecked allocations where 2 are allowed', problems[0])
            allocator = root / 'engine' / 'foundation' / 'memory' / 'src' / 'allocators' / 'pool.cpp'
            allocator.parent.mkdir(parents=True)
            allocator.write_text('void* p = m_parent->allocate(n, al);\n*static_cast<char*>(p) = 0;\n',
                                 encoding='utf-8')
            source.write_text('void f(I* a)\n{\n    int* p = static_cast<int*>(a->allocate(4U, 4U));\n'
                              '    if (p != nullptr)\n    {\n        *p = 1;\n    }\n}\n', encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(0, allocation_results.main(['check', str(root)]))
            self.assertIn('1 allocation calls, every result checked or passed up', out.getvalue())
        # --help prints the usage; it was read as a repository root with nothing in it (exit 1).
        with patch('sys.stdout', new_callable=io.StringIO) as out, self.assertRaises(SystemExit) as stop:
            allocation_results.main(['check', '--help'])
        self.assertEqual(stop.exception.code, 0)
        self.assertIn('repository root', out.getvalue())


class DiagRunbooks(unittest.TestCase):
    """DIAG.11b clause (10): scripts/check-diag-runbooks.py holds docs/design/diagnostic-runbooks.md to the tree."""

    @classmethod
    def setUpClass(cls):
        cls.text = runbooks.RUNBOOK.read_text(encoding='utf-8')
        cls.design = runbooks.DESIGN.read_text(encoding='utf-8')
        cls.tree = runbooks.Tree()

    def problems(self, text, tree=None, design=None):
        return runbooks.check(tree or self.tree, text, design or self.design)

    def mutated(self, old, new):
        self.assertEqual(self.text.count(old), 1, old)
        return self.text.replace(old, new)

    def test_the_committed_runbook_passes_and_covers_every_child(self):
        self.assertEqual([], self.problems(self.text))
        entries, exempt = runbooks.parse(self.text)
        self.assertGreaterEqual(len(entries), 19)
        self.assertEqual(set(exempt), {'DIAG.11c', 'DIAG.12'})
        owned = {child for _, _, _, rows in entries for field, _, value in rows if field == 'Owner'
                 for child in runbooks.ID.findall(value)}
        self.assertEqual(owned | set(exempt), set(runbooks.CHILD_HEADING.findall(self.design)))

    def test_each_broken_reference_fails_with_its_reason(self):
        cases = [
            ('--build build/win-debug --json`', '--build build/win-debug --jsn`', 'does not take --jsn'),
            ('`python scripts/fuzz.py replay', '`python scripts/fuzzz.py replay', 'no script scripts/fuzzz.py'),
            ('`python scripts/diag-overhead.py run --help`', '`python scripts/diag-overhead.py rn`',
             "has no subcommand 'rn'"),
            ('--command jobs.waits`', '--command jobs.wait`', "registers no command 'jobs.wait'"),
            ('`ceridc diag --command replay.prepare --path <program.ceir> --root <dir>`',
             '`ceridc diag --command replay.prepare --path <program.ceir> --rooot <dir>`', 'does not take --rooot'),
            ('--param breaks=<line>`', '--param brakes=<line>`', "no command takes the argument 'brakes'"),
            ('scripts/wsl-lane.sh dev linux-clang-tsan', 'scripts/wsl-lane.sh deb linux-clang-tsan', "no command 'deb'"),
            ('scripts/wsl-lane.sh dev linux-gcc-asan --build-timeout 3000` |\n| Artifact | The sanitizer',
             'scripts/wsl-lane.sh dev linux-gcc-asn --build-timeout 3000` |\n| Artifact | The sanitizer',
             'needs a configure preset'),
            ('crd-jobs-schedcheck-tests.exe`', 'crd-jobs-schedcheck-testz.exe`', 'no CMake target builds'),
            ('build-target.bat build\\win-debug crd-jobs-schedcheck-tests`',
             'build-target.bat win-debug crd-jobs-schedcheck-tests`', 'build-target.bat build\\<preset> <target>'),
            ('build-target.bat build\\win-debug crd-jobs-schedcheck-tests`',
             'build-target.bat build\\win-debug crd-jobs-schedcheck-testz`', 'no CMake target crd-jobs-schedcheck-testz'),
            ('`crd_diag_fatal_tests(', '`crd_diag_fatal_testz(', 'nothing defines crd_diag_fatal_testz'),
            ('`tests/support/diag/specimens/leak_specimen.cpp`', '`tests/support/diag/specimens/leak.cpp`',
             'no path tests/support/diag/specimens/leak.cpp'),
            ('`cerid-diag-overhead/1`', '`cerid-diag-overhead/9`', 'no source declares the schema'),
            ('| Owner | DIAG.10a |', '| Owner | DIAG.99 |', 'DIAG.99 is not a DIAG child'),
            ('| Owner | DIAG.10a |', '| Owner | DIAG.10b |', 'DIAG.10a has no runbook entry'),
            ('| DIAG.12 | The independent', '| DIAG.10a | x |\n| DIAG.12 | The independent',
             'DIAG.10a is listed without an entry but owns one'),
            ('| Cost | Measured: [overhead board](../bench/2026-10-16-diag-11b-overhead.md#verdicts); against Tracy: '
             '[peer board](../bench/2026-10-26-diag-11b-tracy-peer.md). |',
             '| Cost | Measured somewhere. |', 'names no measurement or owner'),
            ('| Hosted | linux-clang-tsan. The', '| Hostd | linux-clang-tsan. The', 'no Hosted row'),
            ('`python scripts/debugger-recipes.py --probe`', '`make crash`', 'not a command form the guard knows'),
            ('`python scripts/debugger-recipes.py --probe`', '`python scripts/debugger-recipes.py --probe && x`',
             'one command per code span'),
        ]
        for old, new, expected in cases:
            problems = self.problems(self.mutated(old, new))
            self.assertTrue(any(expected in p for p in problems), (new, expected, problems))

    def test_publication_is_refused(self):
        for command, expected in [('git push origin main', 'git push is not a read'),
                                  ('git commit -m x', 'git commit is not a read'),
                                  ('gh release create v1', 'gh release create is not a read'),
                                  ('gh workflow run ci.yml', 'gh workflow run is not a read'),
                                  ('gh gist create bundle.json', 'gh gist create is not a read'),
                                  ('gh api -X POST repos/yatiyr/CRD/issues', 'gh api -X writes'),
                                  ('gh api --method PATCH repos/yatiyr/CRD', 'gh api --method writes'),
                                  ('ceridc diag --command bundle.inspect --grant read,upload', 'grants upload'),
                                  ('ceridc mcp --diag-grant read,remote-enable', 'grants remote-enable'),
                                  ('ceridc diag --command diag.commands --grant read,admin', "unknown authority")]:
            problems = runbooks.check_command(self.tree, command, None, 'x')
            self.assertTrue(any(expected in p for p in problems), (command, problems))
        for command in ('gh run view 1 --json jobs', 'gh api repos/yatiyr/CRD/actions/jobs/1/logs', 'git log -1',
                        'ceridc diag --command program.inspect --grant read,execute'):
            self.assertEqual([], runbooks.check_command(self.tree, command, None, 'x'), command)

    def test_test_references_resolve_on_the_rows_for_this_host(self):
        design = '## DIAG.1a — x\n'
        entry = ('<a id="rb-x"></a>\n### X\n\n| Field | Value |\n|---|---|\n| Symptom | s |\n'
                 '| Command | `python scripts/dev.py plan --build build/win-debug` |\n| Artifact | a |\n| Read | r |\n'
                 '| Next test | `ctest --test-dir build/win-debug -R "^alpha: "` |\n'
                 '| Next test (Windows) | `ctest --test-dir build/win-debug -R "^windows only"` |\n'
                 '| Next test (Linux) | `wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh test linux-gcc-debug '
                 '-R "^linux only"`; `ctest --test-dir build/win-debug -R "<failing test>"` |\n'
                 '| Hosted | h |\n| Adopt | `crd_discover_tests(<target>)` |\n'
                 '| Example | `scripts/dev.py` |\n| Cost | [c](ci-tiers.md) |\n| Owner | DIAG.1a |\n')
        linux = runbooks.Tree(tests=['alpha: one', 'linux only: two'], platform='Linux')
        self.assertEqual([], runbooks.check(linux, entry, design), 'Windows rows are not resolved on Linux')
        windows = runbooks.Tree(tests=['alpha: one', 'windows only: three'], platform='Windows')
        self.assertEqual([], runbooks.check(windows, entry, design), 'Linux rows are not resolved on Windows')
        dead = runbooks.Tree(tests=['beta'], platform='Linux')
        problems = runbooks.check(dead, entry, design)
        self.assertTrue(any("no test of this tree matches '^alpha: '" in p for p in problems), problems)
        self.assertTrue(any("no test of this tree matches '^linux only'" in p for p in problems), problems)
        self.assertFalse(any('windows only' in p for p in problems), problems)
        self.assertFalse(any('failing test' in p for p in problems), 'a placeholder regex is not resolved')
        self.assertEqual([], runbooks.check(runbooks.Tree(), entry, design), 'static: nothing resolved')
        # An executable's binary directory must exist in the configured tree of its host (the add_subdirectory
        # layout: tests/foundation/jobs builds into tests/jobs); the program itself may be unbuilt.
        program = entry.replace('| Artifact | a |', '| Command (Windows) | '
                                '`build/win-debug/tests/jobs/crd-jobs-schedcheck-tests.exe` |\n| Artifact | a |')
        with tempfile.TemporaryDirectory() as temp:
            tree = runbooks.Tree(tests=['alpha: one', 'windows only: three'], platform='Windows', build=temp)
            problems = runbooks.check(tree, program, design)
            self.assertTrue(any('no directory tests/jobs' in p for p in problems), problems)
            (Path(temp) / 'tests/jobs').mkdir(parents=True)
            self.assertEqual([], runbooks.check(tree, program, design))
            linux = runbooks.Tree(tests=['alpha: one', 'linux only: two'], platform='Linux', build=Path(temp) / 'x')
            self.assertEqual([], runbooks.check(linux, program, design), 'a Windows row is not resolved on Linux')

    def test_a_label_selection_must_name_a_label_of_the_tree(self):
        design = '## DIAG.1a — x\n'
        entry = ('<a id="rb-x"></a>\n### X\n\n| Field | Value |\n|---|---|\n| Symptom | s |\n'
                 '| Command | `python scripts/dev.py plan --build build/win-debug` |\n| Artifact | a |\n| Read | r |\n'
                 '| Next test | `ctest --test-dir build/win-debug -L diag-workflow`; `wsl.exe --cd <checkout> -e bash '
                 'scripts/wsl-lane.sh test linux-gcc-debug -L diag-workflow` |\n'
                 '| Hosted | h |\n| Adopt | `crd_discover_tests(<target>)` |\n'
                 '| Example | `scripts/dev.py` |\n| Cost | [c](ci-tiers.md) |\n| Owner | DIAG.1a |\n')
        labelled = runbooks.Tree(tests=['alpha: one'], platform='Linux', labels={'diag', 'diag-workflow'})
        self.assertEqual([], runbooks.check(labelled, entry, design))
        unlabelled = runbooks.Tree(tests=['alpha: one'], platform='Linux', labels={'diag'})
        problems = runbooks.check(unlabelled, entry, design)
        self.assertEqual(2, len(problems), problems)
        self.assertTrue(all("no test label of this tree matches 'diag-workflow'" in p for p in problems), problems)
        self.assertEqual([], runbooks.check(runbooks.Tree(), entry, design), 'static: nothing resolved')

    def test_build_qualified_rows_resolve_only_where_the_configuration_compiles_them_in(self):
        # The shipping and release lanes compile asserts and profiling out, and register none of those modes' cases.
        design = '## DIAG.1a — x\n'
        entry = ('<a id="rb-x"></a>\n### X\n\n| Field | Value |\n|---|---|\n| Symptom | s |\n'
                 '| Command | `python scripts/dev.py plan --build build/win-debug` |\n| Artifact | a |\n| Read | r |\n'
                 '| Next test | `ctest --test-dir build/win-debug -R "^alpha: "` |\n'
                 '| Next test (profiling) | `ctest --test-dir build/win-debug -R "^profiled: "` |\n'
                 '| Next test (Linux, asserts) | `ctest --test-dir build/win-debug -R "^asserted: "` |\n'
                 '| Hosted | h |\n| Adopt | `crd_discover_tests(<target>)` |\n'
                 '| Example | `scripts/dev.py` |\n| Cost | [c](ci-tiers.md) |\n| Owner | DIAG.1a |\n')
        shipping = runbooks.Tree(tests=['alpha: one'], platform='Linux', properties=set())
        self.assertEqual([], runbooks.check(shipping, entry, design))
        profiled = runbooks.Tree(tests=['alpha: one'], platform='Linux', properties={'profiling'})
        problems = runbooks.check(profiled, entry, design)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("no test of this tree matches '^profiled: '", problems[0])
        debug = runbooks.Tree(tests=['alpha: one', 'profiled: a', 'asserted: b'], platform='Linux',
                              properties={'asserts', 'profiling'})
        self.assertEqual([], runbooks.check(debug, entry, design))
        windows = runbooks.Tree(tests=['alpha: one', 'profiled: a'], platform='Windows',
                                properties={'asserts', 'profiling'})
        self.assertEqual([], runbooks.check(windows, entry, design), 'a (Linux, asserts) row needs both')
        problems = runbooks.check(debug, entry.replace('(Linux, asserts)', '(Linux, gpu)'), design)
        self.assertTrue(any("unknown qualifier 'gpu'" in p for p in problems), problems)
        with tempfile.TemporaryDirectory() as temp:
            (Path(temp) / 'CMakeCache.txt').write_text('CRD_ENABLE_ASSERTS:BOOL=OFF\nCRD_ENABLE_PROFILING:BOOL=ON\n',
                                                       encoding='utf-8')
            self.assertEqual({'profiling'}, runbooks.build_properties(temp))
            (Path(temp) / 'CMakeCache.txt').write_text('CRD_ENABLE_ASSERTS:BOOL=ON\n', encoding='utf-8')
            self.assertEqual({'asserts'}, runbooks.build_properties(temp))

    def test_main_needs_a_mode_and_a_usable_tree(self):
        with patch('sys.stderr', new_callable=io.StringIO), self.assertRaises(SystemExit) as stop:
            runbooks.main([])
        self.assertEqual(stop.exception.code, 2)
        with tempfile.TemporaryDirectory() as temp, patch('sys.stdout', new_callable=io.StringIO) as out:
            self.assertEqual(runbooks.main(['--build', temp]), 2)
        self.assertIn('not a configured tree', out.getvalue())
        with patch('sys.stdout', new_callable=io.StringIO) as out:
            self.assertEqual(runbooks.main(['--static']), 0, out.getvalue())
        self.assertIn('test references not resolved (--static); PASS', out.getvalue())
        # A module selection's tree registers part of the tests the commands' full presets do: none is resolved.
        with tempfile.TemporaryDirectory() as temp, patch('sys.stdout', new_callable=io.StringIO) as out:
            (Path(temp) / 'CTestTestfile.cmake').write_text('', encoding='utf-8')
            (Path(temp) / 'CMakeCache.txt').write_text('CRD_ENABLE_ASSERTS:BOOL=ON\n', encoding='utf-8')
            with patch.object(runbooks, 'tree_tests', side_effect=AssertionError('resolved against a selection')):
                self.assertEqual(runbooks.main(['--build', temp, '--omitted-modules', 'jobs,sandbox']), 0,
                                 out.getvalue())
        self.assertIn('test references not resolved (the module selection omits jobs, sandbox); PASS',
                      out.getvalue())


class SourceRoutes(unittest.TestCase):
    """DIAG.11b follow-up (n): scripts/check-source-routes.py holds every source to a route, per configured tree."""

    ROADMAP = ('| 1 | <a id="slice-repo.3"></a>**REPO.3** | Partial | x |\n'
               '| 2 | <a id="slice-old.1"></a>**OLD.1** | Done | x |\n')
    CMAKE = 'option(CRD_BUILD_BENCHMARKS "b" ON)\nadd_library(crd-kir-cuda STATIC x.cpp)\n'

    def tree(self, compiled=(), targets=('crd-core',), cache=None, system='Windows'):
        return source_routes.Tree(ROOT, {source_routes.key(path): 'crd-core' for path in compiled}, set(targets),
                                  cache or {}, system)

    def run_check(self, tree, entries, sources, files=None):
        manifest = {'schema': source_routes.SCHEMA, 'entries': entries}
        files = files or {}
        return source_routes.check(tree, manifest, sources, self.CMAKE, self.ROADMAP, read=files.get)

    def test_the_committed_manifest_is_well_formed(self):
        manifest = json.loads(source_routes.MANIFEST.read_text(encoding='utf-8'))
        self.assertEqual(source_routes.SCHEMA, manifest['schema'])
        cmake_text = '\n'.join(path.read_text(encoding='utf-8', errors='replace')
                               for path in list(ROOT.glob('**/CMakeLists.txt')) + list(ROOT.glob('cmake/*.cmake'))
                               if 'build' not in path.relative_to(ROOT).parts[:1]
                               and 'external' not in path.relative_to(ROOT).parts[:1])
        rows = source_routes.open_rows(source_routes.ROADMAP.read_text(encoding='utf-8'))
        problems = [problem for index, entry in enumerate(manifest['entries'])
                    for problem in source_routes.check_entry_shape(index, entry, cmake_text, rows)]
        self.assertEqual([], problems)
        for entry in manifest['entries']:
            if 'unrouted' in entry:
                self.assertFalse([p for p in entry['paths'] if p.startswith(source_routes.NO_UNROUTED)])

    def test_a_source_no_target_compiles_fails_by_name(self):
        problems, counts = self.run_check(self.tree(compiled=['tests/a/test_a.cpp']), [],
                                          ['tests/a/test_a.cpp', 'tests/a/test_scene.cpp'])
        self.assertEqual(['tests/a/test_scene.cpp: no target of this tree compiles it; add it to its target (or name '
                          'its route in scripts/source-routes.json)'], problems)
        self.assertEqual(1, counts['compiled'])

    def test_a_platform_condition_is_held_both_ways(self):
        entry = {'paths': ['engine/dx12/'], 'built_when': {'platform': 'Windows'}, 'why': 'w'}
        sources = ['engine/dx12/a.cpp']
        self.assertEqual(([], {'compiled': 1, 'gated off': 0, 'built outside': 0, 'included': 0, 'unrouted': 0}),
                         self.run_check(self.tree(compiled=sources), [entry], sources))
        self.assertEqual([], self.run_check(self.tree(system='Linux'), [entry], sources)[0])
        missing, _ = self.run_check(self.tree(), [entry], sources)
        self.assertEqual(['engine/dx12/a.cpp: built when platform Windows, which holds here, but no target compiles '
                          'it'], missing)
        stale, _ = self.run_check(self.tree(compiled=sources, system='Linux'), [entry], sources)
        self.assertIn('though entry 0 says it is built only when platform Windows', stale[0])

    def test_an_option_reads_off_when_absent_and_must_be_defined(self):
        entry = {'paths': ['tests/bench/'], 'built_when': {'option': 'CRD_BUILD_BENCHMARKS'}, 'why': 'w'}
        sources = ['tests/bench/b.cpp']
        self.assertEqual([], self.run_check(self.tree(), [entry], sources)[0])
        self.assertEqual([], self.run_check(self.tree(cache={'CRD_BUILD_BENCHMARKS': 'OFF'}), [entry], sources)[0])
        on, _ = self.run_check(self.tree(cache={'CRD_BUILD_BENCHMARKS': 'ON'}), [entry], sources)
        self.assertIn('built when option CRD_BUILD_BENCHMARKS, which holds here', on[0])
        typo = dict(entry, built_when={'option': 'CRD_BUILD_BENCHMARK'})
        self.assertEqual(['entry 0 (tests/bench/): no CMake file defines the option CRD_BUILD_BENCHMARK'],
                         self.run_check(self.tree(), [typo], sources)[0])

    def test_a_target_condition_follows_the_tree_and_must_be_named(self):
        entry = {'paths': ['tests/gpu/kir-cuda/'], 'built_when': {'target': 'crd-kir-cuda'}, 'why': 'w'}
        sources = ['tests/gpu/kir-cuda/t.cpp']
        self.assertEqual([], self.run_check(self.tree(), [entry], sources)[0])
        broken, _ = self.run_check(self.tree(targets=('crd-kir-cuda',)), [entry], sources)
        self.assertIn('built when target crd-kir-cuda, which holds here, but no target compiles it', broken[0])
        unknown = dict(entry, built_when={'target': 'crd-kir-cudaa'})
        self.assertIn('no CMake file defines the target crd-kir-cudaa', self.run_check(self.tree(), [unknown],
                                                                                         sources)[0][0])
        self.assertIn("platform 'Win' is not one of",
                      self.run_check(self.tree(), [dict(entry, built_when={'platform': 'Win'})], sources)[0][0])

    def test_a_route_outside_the_tree_must_exist_name_the_source_and_not_be_compiled(self):
        entry = {'paths': ['scripts/b.cpp'], 'built_by': 'scripts/run_b.sh', 'why': 'w'}
        sources = ['scripts/b.cpp']
        self.assertEqual([], self.run_check(self.tree(), [entry], sources, {'scripts/run_b.sh': 'g++ scripts/b.cpp'})[0])
        self.assertEqual(['scripts/b.cpp: built by scripts/run_b.sh, which does not exist'],
                         self.run_check(self.tree(), [entry], sources)[0])
        self.assertEqual(['scripts/b.cpp: built by scripts/run_b.sh, which does not name it'],
                         self.run_check(self.tree(), [entry], sources, {'scripts/run_b.sh': 'g++ c.cpp'})[0])
        compiled, _ = self.run_check(self.tree(compiled=sources), [entry], sources, {'scripts/run_b.sh': 'b.cpp'})
        self.assertEqual(['scripts/b.cpp: compiled here (crd-core), so entry 0 no longer describes it'], compiled)

    def test_an_included_source_must_be_included(self):
        entry = {'paths': ['bench/gen/*.cu'], 'included_by': 'bench/p.cu', 'why': 'w'}
        sources = ['bench/gen/k.cu']
        self.assertEqual([], self.run_check(self.tree(), [entry], sources,
                                            {'bench/p.cu': '#include "gen/k.cu"\n'})[0])
        self.assertEqual(['bench/gen/k.cu: included by bench/p.cu, which does not include it'],
                         self.run_check(self.tree(), [entry], sources, {'bench/p.cu': '// gen/k.cu\n'})[0])

    def test_an_unrouted_source_needs_an_open_owner_outside_the_module_directories(self):
        entry = {'paths': ['bench/'], 'unrouted': 'REPO.3', 'why': 'w'}
        sources = ['bench/x.cu']
        self.assertEqual([], self.run_check(self.tree(), [entry], sources)[0])
        self.assertEqual(['entry 0 (bench/): unrouted owner OLD.1 is Done; route or retire the sources'],
                         self.run_check(self.tree(), [dict(entry, unrouted='OLD.1')], sources)[0])
        self.assertEqual(['entry 0 (bench/): unrouted owner NEW.9 is not a ROADMAP row'],
                         self.run_check(self.tree(), [dict(entry, unrouted='NEW.9')], sources)[0])
        waived, _ = self.run_check(self.tree(), [{'paths': ['tests/a/'], 'unrouted': 'REPO.3', 'why': 'w'}],
                                   ['tests/a/t.cpp'])
        self.assertEqual(['entry 0 (tests/a/): tests/a/ lies in a module directory, where every source needs a '
                          'route'], waived)
        self.assertIn('so entry 0 no longer describes it',
                      self.run_check(self.tree(compiled=sources), [entry], sources)[0][0])

    def test_patterns_rank_and_a_dead_pattern_fails(self):
        directory = {'paths': ['bench/'], 'unrouted': 'REPO.3', 'why': 'w'}
        glob = {'paths': ['bench/gen/*.cu'], 'included_by': 'bench/p.cu', 'why': 'w'}
        sources = ['bench/p.cu', 'bench/gen/k.cu', 'bench/gen/deep/z.cu']
        problems, counts = self.run_check(self.tree(), [directory, glob], sources,
                                          {'bench/p.cu': '#include "gen/k.cu"'})
        self.assertEqual([], problems)
        self.assertEqual((2, 1), (counts['unrouted'], counts['included']))
        dead, _ = self.run_check(self.tree(), [directory, dict(glob, paths=['bench/gen/*.cu', 'bench/old.cu'])],
                                 sources, {'bench/p.cu': '#include "gen/k.cu"'})
        self.assertEqual(['entry 1: bench/old.cu names no source the guard judges (a stale or misspelled pattern)'],
                         dead)
        twice, _ = self.run_check(self.tree(), [directory, dict(directory, unrouted='REPO.3')], ['bench/p.cu'])
        self.assertIn('bench/p.cu is named by entries 0 (bench/) and 1 (bench/) at the same rank', twice)

    def test_an_entry_needs_one_route_and_a_reason(self):
        problems, _ = self.run_check(self.tree(), [{'paths': ['bench/'], 'unrouted': 'REPO.3', 'built_by': 'x',
                                                    'why': ' ', 'via': 1}], ['bench/a.c'])
        self.assertEqual(['entry 0 (bench/): unknown keys via',
                          'entry 0 (bench/): needs exactly one of built_when, built_by, included_by, unrouted',
                          'entry 0 (bench/): says nothing in `why`'], problems)

    def test_cmake_truth_and_the_object_of_a_command(self):
        for value in ('ON', 'TRUE', 'yes', 'Y', '1', '2'):
            self.assertTrue(source_routes.cmake_true(value), value)
        for value in ('', 'OFF', '0', 'NO', 'FALSE', 'N', 'IGNORE', 'NOTFOUND', 'X-NOTFOUND', 'maybe'):
            self.assertFalse(source_routes.cmake_true(value), value)
        self.assertEqual(r'tests\CMakeFiles\t.dir\a.cpp.obj', source_routes.output_of(
            {'command': r'cl.exe /nologo /Fotests\CMakeFiles\t.dir\a.cpp.obj -c a.cpp'}))
        self.assertEqual('CMakeFiles/t.dir/a.cpp.o', source_routes.output_of(
            {'arguments': ['g++', '-o', 'CMakeFiles/t.dir/a.cpp.o', '-c', 'a.cpp']}))
        self.assertIsNone(source_routes.output_of({'command': 'g++ -c a.cpp'}))

    def test_the_guard_judges_a_configured_tree_from_its_compile_database(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            for path in ('tests/a/test_a.cpp', 'tests/a/test_lost.cpp', 'scripts/b.cpp', 'tests/a/notes.m'):
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text('int x;\n', encoding='utf-8')
            (root / 'scripts/run_b.sh').write_text('g++ scripts/b.cpp\n', encoding='utf-8')
            (root / 'CMakeLists.txt').write_text('add_executable(t tests/a/test_a.cpp)\n', encoding='utf-8')
            (root / 'docs').mkdir()
            (root / 'docs/ROADMAP.md').write_text(self.ROADMAP, encoding='utf-8')
            build = root / 'build/t'
            (build / 'CMakeFiles/4.3.2').mkdir(parents=True)
            (build / 'CMakeFiles/4.3.2/CMakeSystem.cmake').write_text('set(CMAKE_SYSTEM_NAME "Linux")\n',
                                                                       encoding='utf-8')
            (build / 'CMakeCache.txt').write_text('CRD_BUILD_BENCHMARKS:BOOL=ON\n', encoding='utf-8')
            (root / '.gitignore').write_text('build/\n', encoding='utf-8')
            compiled = [{'directory': str(build), 'file': str(root / 'tests/a/test_a.cpp'),
                         'command': 'c++ -c x', 'output': 'tests/a/CMakeFiles/t.dir/test_a.cpp.o'}]
            (build / 'compile_commands.json').write_text(json.dumps(compiled), encoding='utf-8')
            manifest = root / 'routes.json'
            manifest.write_text(json.dumps({
                'schema': source_routes.SCHEMA, 'roots': ['tests', 'scripts'], 'extensions': ['.cpp'],
                'entries': [{'paths': ['scripts/b.cpp'], 'built_by': 'scripts/run_b.sh', 'why': 'peer'}]}),
                encoding='utf-8')
            with patch.object(source_routes, 'ROOT', root), \
                    patch.object(source_routes, 'ROADMAP', root / 'docs/ROADMAP.md'), \
                    patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(1, source_routes.main(['--build', str(build), '--manifest', str(manifest)]))
            self.assertIn('ERROR: tests/a/test_lost.cpp: no target of this tree compiles it', out.getvalue())
            self.assertIn('3 sources under tests, scripts on Linux (1 targets compile sources); 1 compiled, '
                          '0 gated off, 1 built outside', out.getvalue())
            compiled.append({'directory': str(build), 'file': 'tests/a/test_lost.cpp', 'command': 'c++',
                             'output': 'tests/a/CMakeFiles/t.dir/test_lost.cpp.o'})
            compiled[1]['file'] = str(root / compiled[1]['file'])
            (build / 'compile_commands.json').write_text(json.dumps(compiled), encoding='utf-8')
            with patch.object(source_routes, 'ROOT', root), \
                    patch.object(source_routes, 'ROADMAP', root / 'docs/ROADMAP.md'), \
                    patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(0, source_routes.main(['--build', str(build), '--manifest', str(manifest)]))
            self.assertIn('PASS', out.getvalue())
            (build / 'compile_commands.json').write_text(json.dumps([{'directory': str(build), 'file': 'x.cpp',
                                                                       'command': 'c++ -c x.cpp'}]), encoding='utf-8')
            with patch.object(source_routes, 'ROOT', root), \
                    patch.object(source_routes, 'ROADMAP', root / 'docs/ROADMAP.md'), \
                    patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(2, source_routes.main(['--build', str(build), '--manifest', str(manifest)]))
            self.assertIn('names no output', out.getvalue())
            (build / 'compile_commands.json').unlink()
            with patch.object(source_routes, 'ROOT', root), \
                    patch.object(source_routes, 'ROADMAP', root / 'docs/ROADMAP.md'), \
                    patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(2, source_routes.main(['--build', str(build), '--manifest', str(manifest)]))
            self.assertIn('has no compile_commands.json', out.getvalue())


class AllmanBraces(unittest.TestCase):
    @staticmethod
    def kinds(source):
        model = allman.FileModel(source.split('\n'))
        return [kind for kind, _, _, _ in allman.analyse(model)]

    @staticmethod
    def fixed(source):
        lines, _, _ = allman.fix_lines(source.split('\n'))
        return '\n'.join(lines)

    def test_the_tree_is_clean(self):
        files = allman.tracked_sources()
        self.assertGreater(len(files), 1000)
        self.assertNotIn('bench/reference/shewchuk-predicates.c', files)  # vendored
        self.assertNotIn('engine/execution/ceir/generated/crd/ceir/gen/arith_ops.cpp', files)  # generated

    def test_each_violation_kind_is_found(self):
        self.assertEqual(self.kinds('void f()\n{\n    if (x) { y(); }\n}'), ['ONE_LINE'])
        self.assertEqual(self.kinds('void f()\n{\n    if (x)\n        y();\n}'), ['NO_BRACE'])
        self.assertEqual(self.kinds('void f()\n{\n    for (;;) y();\n}'), ['NO_BRACE'])
        self.assertEqual(self.kinds('void f()\n{\n    while (x) {\n        y();\n    }\n}'), ['KR_OPEN'])
        self.assertEqual(self.kinds('void f()\n{\n    if (x)\n    {\n    } else\n    {\n    }\n}'), ['CUDDLED'])
        self.assertEqual(self.kinds('void f()\n{\n    a(); if (x)\n    {\n        y();\n    }\n}'), ['MIDLINE'])

    def test_literals_macros_and_do_while_are_not_statements(self):
        clean = ('#define CHECKED(x) if (x) { return; }\n'
                 'const char* s = "if (x) { y; }";\n'
                 'const char* r = R"(\n    if (x) { y; }\n)";\n'
                 'void f()\n{\n    // if (x) { y; }\n    do\n    {\n        g();\n    } while (x);\n'
                 '    if (x)\n    {\n    }\n}')
        self.assertEqual(self.kinds(clean), [])

    def test_fix_produces_allman_blocks(self):
        self.assertEqual(self.fixed('    if (x) { a(); b(); } // why'),
                         '    if (x) // why\n    {\n        a();\n        b();\n    }')
        self.assertEqual(self.fixed('    if (x) return;\n    else y();'),
                         '    if (x)\n    {\n        return;\n    }\n    else\n    {\n        y();\n    }')
        self.assertEqual(self.fixed('    switch (n) { case 1: return a; default: break; }'),
                         '    switch (n)\n    {\n        case 1:\n            return a;\n        default:\n'
                         '            break;\n    }')
        self.assertEqual(self.kinds(self.fixed('    for (;;) if (x) { y(); } else z();')), [])


class GenmcModelLane(unittest.TestCase):
    """DIAG.4b: the pinned GenMC build helper and the three-variant verdict check (check-genmc-models.py)."""

    CLEAN = ('*** Verification complete.\nNo errors were detected.\nNumber of complete executions explored: 6\n'
             'Number of blocked executions seen: 2\nTotal wall-clock time: 0.13s\n')
    FREED = ('Error: Attempt to access freed memory!\nEvent (3, 4) conflicts with event (2, 7) in graph:\n'
             '\n*** Verification unsuccesful.\nNumber of complete executions explored: 4\n')
    ASSERTED = ('Error: Safety violation!\n\t(1, 11): ERROR counter_publish_reclaim.c:114\n'
                '[sched_finish_park] counter_publish_reclaim.c: 114: '
                'assert(atomic_load_explicit(&g_gen, memory_order_relaxed) == snapshot);\n'
                '\n*** Verification unsuccesful.\nNumber of complete executions explored: 1\n')
    OTHER = 'Error: Non-atomic race!\n\n*** Verification unsuccesful.\nNumber of complete executions explored: 2\n'

    def test_repaired_model_must_verify_with_explored_executions(self):
        passed, reason = genmc_models.judge('repaired', 0, self.CLEAN)
        self.assertTrue(passed, reason)
        self.assertIn('6 complete executions', reason)
        self.assertFalse(genmc_models.judge('repaired', 42, self.FREED)[0])
        self.assertFalse(genmc_models.judge('repaired', 0, 'Number of complete executions explored: 3\n')[0])
        self.assertFalse(genmc_models.judge('repaired', 0, self.CLEAN.replace(': 6', ': 0'))[0])
        facts = genmc_models.summarize(self.CLEAN)
        self.assertEqual((facts['executions'], facts['blocked'], facts['genmc_seconds']), (6, 2, 0.13))

    def test_seeded_variants_must_report_their_own_defect(self):
        self.assertTrue(genmc_models.judge('handshake', 42, self.FREED)[0])
        self.assertTrue(genmc_models.judge('handshake', 42, self.ASSERTED)[0])
        self.assertTrue(genmc_models.judge('early-drained', 42, self.FREED)[0])
        self.assertFalse(genmc_models.judge('early-drained', 42, self.ASSERTED)[0])  # wrong defect class
        passed, reason = genmc_models.judge('handshake', 0, self.CLEAN)
        self.assertFalse(passed)
        self.assertIn('no teeth', reason)
        passed, reason = genmc_models.judge('early-drained', 42, self.OTHER)
        self.assertFalse(passed)
        self.assertIn('Non-atomic race', reason)
        passed, reason = genmc_models.judge('handshake', 139, 'Segmentation fault\n')
        self.assertFalse(passed)
        self.assertIn('tool failure', reason)

    def test_commands_disable_estimation_and_trace_only_seeded_variants(self):
        model = Path('model.c')
        self.assertEqual(genmc_models.command_for('genmc', 'repaired', model),
                         ['genmc', '--disable-estimation', '--', 'model.c'])
        self.assertEqual(genmc_models.command_for('genmc', 'early-drained', model),
                         ['genmc', '--disable-estimation', '-print-error-trace', '--', '-DCRD_MODEL_EARLY_DRAINED',
                          'model.c'])
        text = genmc_models.MODEL.read_text(encoding='utf-8')
        for variant, (defines, _) in genmc_models.VARIANTS.items():
            for define in defines:
                self.assertIn(define[2:], text, variant)

    def test_installer_refuses_another_llvm_major_and_reuses_only_a_matching_stamp(self):
        pin = pins_registry.entry('tools', 'genmc')
        self.assertIn(pin['commit'], pin['url'])
        install_genmc.check_llvm(pin, pin['llvm_major'] + '.1.2')
        with self.assertRaisesRegex(pins_registry.PinError, 'pinned against LLVM'):
            install_genmc.check_llvm(pin, '18.1.3')
        workflow = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        for package in ('llvm-%s-dev' % pin['llvm_major'], 'clang-%s' % pin['llvm_major']):
            self.assertIn(package, workflow)
            self.assertIn(package, self_unpinned_apt())
        self.assertIn('scripts/install-genmc.py', check_pins.HELPERS)
        drifted = json.loads(json.dumps(pins_registry.load()))
        drifted['tools']['genmc']['url'] = drifted['tools']['genmc']['url'].replace(pin['commit'], 'v' + pin['version'])
        problems = check_pins.validate_registry(drifted)
        self.assertTrue(any('tool genmc: url must be addressed by its commit' in p for p in problems), problems)
        stamp =install_genmc.expected_stamp(pin, '20.1.2')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertFalse(install_genmc.reusable(root, stamp))
            (root / install_genmc.STAMP).write_text(json.dumps(stamp), encoding='utf-8')
            with patch.object(install_genmc, 'installed_version', return_value='GenMC v%s' % pin['version']):
                self.assertTrue(install_genmc.reusable(root, stamp))
                self.assertFalse(install_genmc.reusable(root, dict(stamp, llvm='20.1.3')))
            with patch.object(install_genmc, 'installed_version', return_value=None):
                self.assertFalse(install_genmc.reusable(root, stamp))

    def test_extraction_needs_one_top_directory_and_keeps_links_inside(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)

            def archive(name, members):
                path = root / name
                with tarfile.open(path, 'w:gz') as package:
                    for member, link in members:
                        info = tarfile.TarInfo(member)
                        if link is None:
                            info.size = 1
                            package.addfile(info, io.BytesIO(b'x'))
                        else:
                            info.type = tarfile.SYMTYPE
                            info.linkname = link
                            package.addfile(info)
                return path

            good = archive('good.tar.gz', [('genmc-abc/CMakeLists.txt', None), ('genmc-abc/c/cassert', 'assert.h'),
                                           ('genmc-abc/c/assert.h', None)])
            top = install_genmc.extract(good, root / 'good')
            self.assertEqual(top, root / 'good' / 'genmc-abc')
            self.assertTrue((top / 'CMakeLists.txt').is_file())
            two = archive('two.tar.gz', [('a/x', None), ('b/y', None)])
            with self.assertRaisesRegex(pins_registry.PinError, 'one top-level directory'):
                install_genmc.extract(two, root / 'two')
            escaping = archive('escape.tar.gz', [('genmc-abc/x', None), ('genmc-abc/out', '../../outside')])
            with self.assertRaises(tarfile.FilterError):
                install_genmc.extract(escaping, root / 'escape')


def self_unpinned_apt():
    return pins_registry.load()['unpinned']['apt']


class CiPresetSteps(unittest.TestCase):
    """A preset step (the GenMC model on linux-clang-tsan) stays in its preset's job, gated and tier-matched."""

    def test_preset_step_must_exist_be_gated_and_match_its_preset_tier(self):
        tiers = ci_tiers.load_json(ROOT / '.github/ci-tiers.json')
        presets = ci_tiers.load_json(ROOT / 'CMakePresets.json')
        workflow = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        self.assertEqual(ci_tiers.validate(tiers, presets, workflow), [])
        self.assertEqual(tiers['preset_steps']['GenMC weak-memory model']['preset'], 'linux-clang-tsan')
        step_body = "\n        run: |\n          python3 scripts/install-genmc"
        gate_line = "if: \"!cancelled() && matrix.preset == 'linux-clang-tsan'\"" + step_body
        self.assertIn(gate_line, workflow)
        ungated = workflow.replace(gate_line, "if: \"!cancelled()\"" + step_body)
        problems = ci_tiers.validate(tiers, presets, ungated)
        self.assertTrue(any('must be gated' in p for p in problems), problems)
        missing = workflow.replace('- name: GenMC weak-memory model', '- name: Something else')
        problems = ci_tiers.validate(tiers, presets, missing)
        self.assertTrue(any('lacks the step' in p for p in problems), problems)
        retiered = json.loads(json.dumps(tiers))
        retiered['preset_steps']['GenMC weak-memory model']['tier'] = 'change'
        problems = ci_tiers.validate(retiered, presets, workflow)
        self.assertTrue(any('declared change-tier' in p for p in problems), problems)
        retiered['preset_steps']['GenMC weak-memory model'] = {'preset': 'linux-clang-fuzz', 'tier': 'complete'}
        problems = ci_tiers.validate(retiered, presets, workflow)
        self.assertTrue(any('not a hosted preset' in p for p in problems), problems)

    def test_a_step_on_several_presets_is_gated_on_exactly_those_in_each_job(self):
        tiers = ci_tiers.load_json(ROOT / '.github/ci-tiers.json')
        presets = ci_tiers.load_json(ROOT / 'CMakePresets.json')
        workflow = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        step = 'Seeded detector failures'
        self.assertEqual(tiers['preset_steps'][step],
                         {'presets': ['win-debug', 'win-asan', 'linux-gcc-debug', 'linux-gcc-asan', 'linux-clang-tsan']})
        windows_gate = "if: matrix.preset == 'win-debug' || matrix.preset == 'win-asan'\n"
        self.assertIn(windows_gate, workflow)
        dropped = workflow.replace(windows_gate, "if: matrix.preset == 'win-debug'\n")
        problems = ci_tiers.validate(tiers, presets, dropped)
        self.assertEqual(["step %r must be gated on matrix.preset == 'win-asan'" % step], problems)
        widened = workflow.replace(windows_gate, windows_gate[:-1] + " || matrix.preset == 'win-release'\n")
        problems = ci_tiers.validate(tiers, presets, widened)
        self.assertEqual(["step %r in job windows is gated on win-release, which the mapping does not list" % step],
                         problems)
        linux_step = workflow.rindex('- name: %s' % step)
        missing = workflow[:linux_step] + '- name: Something else' + workflow[linux_step + len(step) + 8:]
        problems = ci_tiers.validate(tiers, presets, missing)
        self.assertEqual(['job linux-gcc lacks the step %r' % step] * 3, problems)
        for spec, text in (({'presets': ['win-debug', 'win-debug']}, 'once each'),
                           ({'presets': []}, 'once each'),
                           ({'presets': ['win-debug'], 'tier': 'change'}, 'no tier of its own'),
                           ({'preset': 'win-debug'}, 'preset and tier'),
                           ({'presets': ['win-debug', 'linux-clang-coverage']}, 'not a hosted preset')):
            respecified = json.loads(json.dumps(tiers))
            respecified['preset_steps'][step] = spec
            problems = ci_tiers.validate(respecified, presets, workflow)
            self.assertTrue(any(text in p for p in problems), (spec, problems))


def _elf_object(sections):
    """A minimal little-endian ELF64 object with the named sections: (name, flags, size)."""
    names = b'\0' + b''.join(name + b'\0' for name, _, _ in sections) + b'.shstrtab\0'
    headers = [struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    offset = 1
    for name, flags, size in sections:
        headers.append(struct.pack('<IIQQQQIIQQ', offset, 1, flags, 0, 0, size, 0, 0, 1, 0))
        offset += len(name) + 1
    names_at = 64
    headers.append(struct.pack('<IIQQQQIIQQ', offset, 3, 0, 0, names_at, len(names), 0, 0, 1, 0))
    shoff = names_at + len(names)
    header = bytearray(64)
    header[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<Q', header, 0x28, shoff)
    struct.pack_into('<HHH', header, 0x3A, 64, len(headers), len(headers) - 1)
    return bytes(header) + names + b''.join(headers)


class IsaIsolation(unittest.TestCase):
    """The ISA isolation guard (ADR-0134 §6) on decoder-output fixtures."""

    def setUp(self):
        self.exceptions = isa_isolation.Exceptions()

    def test_encodings_in_64_bit_mode(self):
        cases = {'C5 F8 77': 'VEX', 'C4 E2 79 18': 'VEX', '64 C5 F9 7E': 'VEX', '67 62 F1 7C 48': 'EVEX',
                 'D5 10 89 C0': 'REX2', '8F E8 78 C2': 'XOP', '8F 00': None, '66 0F 38 00 C1': None,
                 '48 89 C8': None, 'F3 48 0F B8 C7': None}
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(expected, isa_isolation.encoding([int(byte, 16) for byte in text.split()]))

    def test_floor_scan_names_each_function_and_category(self):
        dump = ('Dump of file a.obj\n\nFile Type: COFF OBJECT\n\n?f@@YAXXZ (void __cdecl f(void)):\n'
                '  0000000000000000: C5 FC 58 C1        vaddps      ymm0,ymm0,ymm1\n'
                '$LN4:\n'
                '  0000000000000004: 64 C5 F9 7E 00     vmovd       dword ptr fs:[rax],xmm0\n'
                '?g@@YAXXZ (void __cdecl g(void)):\n'
                '  0000000000000010: F3 48 0F B8 C7     popcnt      rax,rdi\n'
                '  0000000000000015: 66 0F 38 00 C1     pshufb      xmm0,xmm1\n'
                '  000000000000001A: F0 48 0F C7 0E     lock cmpxchg16b oword ptr [rsi]\n'
                '  000000000000001F: 48 89 C8           mov         rax,rcx\n')
        hits = [(function, kind) for function, _address, kind, _line in isa_isolation.scan_x64(dump, True)]
        self.assertEqual([('?f@@YAXXZ', 'VEX'), ('?f@@YAXXZ', 'VEX'), ('?g@@YAXXZ', 'popcnt'),
                          ('?g@@YAXXZ', 'pshufb')], hits)
        elf = ('0000000000000000 <crd::f()>:\n   0:\tc5 fc 58 c1          \tvaddps ymm0,ymm0,ymm1\n'
               '0000000000000010 <crd::g()>:\n  10:\tf0 48 0f c7 0e       \tlock cmpxchg16b OWORD PTR [rsi]\n')
        hits = [(function, kind) for function, _address, kind, _line in isa_isolation.scan_x64(elf, False)]
        self.assertEqual([('crd::f()', 'VEX'), ('crd::g()', 'cmpxchg16b')], hits)

    def test_general_symbols_hold_no_level(self):
        judge = isa_isolation.judge_general_symbols
        self.assertEqual([], judge([('crd::math::general::Vec4f::load(float const*)', True), ('?f@@YAXXZ', True)]))
        self.assertEqual(1, len(judge([('?add@avx2@math@crd@@YA?AUVec8f@123@U4123@0@Z', True)])))
        self.assertEqual(1, len(judge([('crd::math::sse2::exp(double)', False)])))
        self.assertEqual(1, len(judge([('crd::hesap::tensor::kTensorKernelsAvx2', True)])))

    def test_kernel_symbols_stay_in_their_level(self):
        allowed = self.exceptions.kernel_symbols
        judge = isa_isolation.judge_kernel_symbols
        good = [('?kTensorKernelsAvx2@tensor@hesap@crd@@3UTensorKernels@123@B', True),
                ('__real@3ff0000000000000', True),
                ('??$bit_cast@VVec8f@avx2@math@crd@@@std@@YA?AVVec8f@avx2@math@crd@@AEBM@Z', True),
                ('crd::hesap::tensor::avx2::walk(int)', True), ('local_helper', False)]
        self.assertEqual(([], 1), judge('avx2', good, allowed))
        failures, _ = judge('avx2', [('??$min@H@std@@YAAEBHAEBH0@Z', True)], allowed)
        self.assertEqual(1, len(failures))
        failures, tables = judge('avx2', [('crd::hesap::tensor::kTensorKernelsAvx2AvxVnni', True)], allowed)
        self.assertEqual((1, 0), (len(failures), tables))
        self.assertEqual(([], 1), judge('avx2_avxvnni', [('crd::hesap::tensor::kTensorKernelsAvx2AvxVnni', True)],
                                        allowed))
        failures, _ = judge('sse2', [('crd::math::avx2::exp(double)', True)], allowed)
        self.assertEqual(1, len(failures))

    def test_kernel_code_stays_within_its_level(self):
        judge = isa_isolation.judge_kernel_code
        vex = ('f', 0, 'VEX', 'vaddps')
        self.assertEqual([], judge('avx2', [vex, ('f', 4, 'lzcnt', 'lzcnt'), ('f', 8, 'popcnt', 'popcnt')], True))
        self.assertEqual(1, len(judge('avx2_avxvnni', [vex, ('f', 4, 'EVEX', 'vpdpbusd')], True)))
        self.assertEqual([], judge('avx512', [('f', 0, 'EVEX', 'vaddps zmm0'), vex], True))
        self.assertEqual(1, len(judge('avx2', [('f', 0, 'REX2', 'push2')], True)))
        self.assertEqual([], judge('sse42', [('f', 0, 'pshufb', 'pshufb'), ('f', 4, 'popcnt', 'popcnt')], True))
        self.assertEqual(1, len(judge('sse42', [('f', 0, 'EVEX', 'vaddps zmm0')], True)))
        self.assertEqual(1, len(judge('sse2', [('f', 0, 'pshufb', 'pshufb')], True)))
        self.assertEqual([], judge('sse2', [('f', 0, 'cmpxchg16b', 'lock cmpxchg16b')], True))
        self.assertEqual(1, len(judge('sse2', [('f', 0, 'cmpxchg16b', 'lock cmpxchg16b')], False)))
        self.assertEqual([], judge('portable', [], True))

    def test_each_family_level_holds_its_table_and_encoding(self):
        judge = isa_isolation.judge_families
        self.assertEqual([], judge({('t-avx2.dir', 'avx2'): [1, {'VEX'}], ('t-avx512.dir', 'avx512'): [1, {'EVEX'}],
                                    ('t-sse2.dir', 'sse2'): [1, set()]}, 'x64'))
        self.assertEqual(1, len(judge({('t-avx2.dir', 'avx2'): [1, {'popcnt'}]}, 'x64')))
        self.assertEqual(1, len(judge({('t-avx512.dir', 'avx512'): [1, {'VEX'}]}, 'x64')))
        self.assertEqual(1, len(judge({('t-sse2.dir', 'sse2'): [0, set()]}, 'x64')))
        self.assertEqual([], judge({('t-neon.dir', 'neon'): [1, set()]}, 'arm64'))
        self.assertEqual('crd-fft-fft-avx512.dir',
                         isa_isolation.object_library('/b/engine/CMakeFiles/crd-fft-fft-avx512.dir/kernels/c_0.cpp.o'))

    def test_msvc_jump_tables_in_objects_are_data(self):
        dump = ('?f@@YAXXZ (void __cdecl f(void)):\n'
                '  0000000000000000: FF E0              jmp         rax\n'
                '$LN5:\n'
                '  0000000000000002: B8 01 00 00 00     mov         eax,1\n'
                '  0000000000000007: C5 F8 77           vzeroupper\n'
                '$LN9:\n'
                '  0000000000000010: 62\n'
                '  0000000000000011: C5\n'
                '?g@@YAXXZ (void __cdecl g(void)):\n'
                '  0000000000000000: C5 F8 77           vzeroupper\n')
        layout = ({('3', 0x10)}, {'?f@@YAXXZ': '3', '?g@@YAXXZ': '4'})
        hits = [(f, a) for f, a, _k, _l in isa_isolation.scan_x64(dump, True, layout)]
        self.assertEqual([('?f@@YAXXZ', 7), ('?g@@YAXXZ', 0)], hits)
        self.assertEqual(4, len(isa_isolation.scan_x64(dump, True)))
        symbols = ('008 00000000 SECT3  notype ()    External     | ?f@@YAXXZ (void __cdecl f(void))\n'
                   '009 00000002 SECT3  notype       Label        | $LN5\n'
                   '00A 00000010 SECT3  notype       Static       | $LN9\n'
                   '00B 00000000 SECT4  notype ()    Static       | ?g@@YAXXZ (void __cdecl g(void))\n'
                   '00C 00000000 SECT5  notype       External     | ??_C@_04GEEJMEMG@avx2@\n')
        defined, found = isa_isolation.coff_symbols(symbols)
        self.assertEqual(({('3', 0x10)}, {'?f@@YAXXZ': '3', '?g@@YAXXZ': '4'}), found)
        self.assertEqual(['?f@@YAXXZ', '$LN9', '?g@@YAXXZ', '??_C@_04GEEJMEMG@avx2@'], [name for name, _ in defined])
        self.assertEqual([], isa_isolation.judge_general_symbols(defined))
        self.assertEqual(1, len(isa_isolation.judge_general_symbols([('??Havx2@simd@math@crd@@YAXXZ', True)])))

    def test_reviewed_exceptions_name_their_scope(self):
        allows = self.exceptions.allows
        self.assertTrue(allows('VEX', 'HUF_decompress4X1_usingDTable_internal_bmi2', 'libzstd_static/x.c.o', set()))
        self.assertTrue(allows('VEX', 'ZSTD_decompressSequences_bmi2.constprop.0', '', set()))
        self.assertTrue(allows('VEX', 'HUF_decompress4X2_usingDTable_internal_fast_asm_loop', '', set()))
        self.assertTrue(allows('VEX', 'HUF_decompress4X1_usingDTable_internal_fast_c_loop', 'zstd/h.c.o', set()))
        self.assertTrue(allows('lzcnt', 'HUF_decompress4X2_usingDTable_internal_fast', '', set()))
        self.assertFalse(allows('VEX', 'HUF_decompress4X2_usingDTable_internal_default', '', set()))
        self.assertFalse(allows('EVEX', 'HUF_decompress4X1_usingDTable_internal_bmi2', '', set()))
        self.assertFalse(allows('VEX', 'crd::hesap::tensor::einsum(int)', 'crd-hesap-tensor/einsum.cpp.obj', set()))
        self.assertTrue(allows('popcnt', '?count@@YAHI@Z', 'crd-hesap-tensor/einsum.cpp.obj', {'?count@@YAHI@Z'}))
        self.assertFalse(allows('popcnt', '?count@@YAHI@Z', 'crd-hesap-tensor/einsum.cpp.obj', set()))
        self.assertTrue(allows('lzcnt', '??$_Countl_zero_lzcnt@I@std@@YAHI@Z', 'crd-x/lbvh.cpp.obj', set()))
        self.assertTrue(allows('popcnt', '??$_Unchecked_popcount@_K@std@@YAH_K@Z', 'crd-x/a.cpp.obj', set()))
        self.assertFalse(allows('lzcnt', '?count@crd@@YAHI@Z', 'crd-x/lbvh.cpp.obj', set()))
        self.assertTrue(allows('xgetbv', 'crd::(anonymous namespace)::read_xcr0()', '', set()))
        self.assertFalse(allows('xgetbv', 'crd::(anonymous namespace)::detect()', '', set()))

    def test_decoded_instructions_are_counted_and_decoders_checked(self):
        shapes = {('x64', True): '  0000000000000000: C5 F8 77           vzeroupper\n                    00\n',
                  ('x64', False): '   0:\tc5 f8 77             \tvzeroupper\n',
                  ('arm64', True): '  0000000000000000: D10043FF  sub         sp,sp,#0x10\n',
                  ('arm64', False): '   0:\td10043ff \tsub\tsp, sp, #0x10\n'}
        for shape, text in shapes.items():
            with self.subTest(shape=shape):
                self.assertEqual(1, len(isa_isolation.INSTRUCTION[shape].findall(text + 'Summary\n')))
                self.assertEqual(0, len(isa_isolation.INSTRUCTION[shape].findall('   0: c5 f8 77  vzeroupper\n')))
        with patch.object(isa_isolation.shutil, 'which', return_value='/usr/bin/llvm-objdump'):
            with patch.object(isa_isolation, '_run', return_value='LLVM (http://llvm.org/):\n'):
                with self.assertRaisesRegex(isa_isolation.GuardError, 'not GNU binutils'):
                    isa_isolation.resolve_tools(SimpleNamespace(gcc_nm=None), False)
        self.assertTrue(any(p.search('__clang_call_terminate') for p in self.exceptions.kernel_symbols))
        self.assertTrue(any(p.search('DW.ref.__gcc_personality_v0') for p in self.exceptions.kernel_symbols))
        self.assertTrue(any(p.search('__llvm_profile_raw_version') for p in self.exceptions.kernel_symbols))
        self.assertTrue(self.exceptions.allows('VEX', 'ZSTD_decompressSequences_bmi2.cold', '', set()))

    def test_arm64_floor(self):
        above = isa_isolation.arm64_above
        self.assertEqual('SVE/SME', above('ld1w', '{z0.s}, p0/z, [x0]'))
        self.assertEqual('ldaddal', above('ldaddal', 'w0, w1, [x2]'))
        self.assertEqual('casal', above('casal', 'x0, x1, [x2]'))
        self.assertEqual('FP16', above('fadd', 'h0, h1, h2'))
        self.assertIsNone(above('fadd', 's0, s1, s2'))
        self.assertIsNone(above('fcvt', 'h0, s0'))
        self.assertIsNone(above('zip1', 'v0.4s, v1.4s, v2.4s'))
        self.assertIsNone(above('add', 'x0, x1, #0x10'))
        self.assertIsNone(above('.word', '0x05a0e000'))
        dump = ('0000000000000000 <crd::f()>:\n   0:\ta5404000 \tld1w\t{z0.s}, p0/z, [x0]\n'
                '   4:\t8b020020 \tadd\tx0, x1, x2\n')
        self.assertEqual([('crd::f()', 'SVE/SME')],
                         [(f, k) for f, _a, k, _l in isa_isolation.scan_arm64(dump, False)])

    def test_arm64_msvc_constants_in_text_are_data(self):
        literal = isa_isolation.arm64_literal
        self.assertEqual((8, 8), literal(0x58000048))
        self.assertEqual((8, 4), literal(0x18000040))
        self.assertEqual((8, 4), literal(0x98000040))
        self.assertEqual((8, 16), literal(0x9C000040))
        self.assertEqual((-4, 8), literal(0x58FFFFE8))
        self.assertIsNone(literal(0xD8000040))
        self.assertIsNone(literal(0xD65F03C0))
        dump = ('Dump of file a.obj\n\nFile Type: COFF OBJECT\n\n?f@@YA_K_K@Z (unsigned __int64 __cdecl f(void)):\n'
                '  0000000000000000: 58000048  ldr         x8,0000000000000008\n'
                '  0000000000000004: D65F03C0  ret\n'
                '  0000000000000008: 92492492  orr         x18,x4,#0x9249249249249249\n'
                '  000000000000000C: 04924924  mla         z4.s,p2/m,z9.s,z18.s\n'
                '  0000000000000010: 05555555  mov         z21.h,p5/m,#-0x56\n'
                '  0000000000000014: 58FFFFE8  ldr         x8,0000000000000010\n'
                '?g@@YAXXZ (void __cdecl g(void)):\n'
                '  0000000000000000: D65F03C0  ret\n'
                '  0000000000000004: D65F03C0  ret\n'
                '  0000000000000008: D65F03C0  ret\n'
                '  000000000000000C: A5404000  ld1w        {z0.s},p0/z,[x0]\n'
                '__swt:\n'
                '  0000000000000010: C4B7B7AA  ldff1sh     {z10.d},p5/z,[z29.d,#0x2E]\n'
                '?h@@YAXXZ (void __cdecl h(void)):\n'
                '  0000000000000018: 05555555  mov         z21.h,p5/m,#-0x56\n')
        self.assertEqual([('?g@@YAXXZ', 12), ('?h@@YAXXZ', 24)],
                         [(f, a) for f, a, _k, _l in isa_isolation.scan_arm64(dump, True)])

    def test_object_formats(self):
        with tempfile.TemporaryDirectory() as temp:
            def kind(name, data):
                path = Path(temp) / name
                path.write_bytes(data)
                return isa_isolation.object_format(str(path))
            self.assertEqual('coff', kind('a.obj', b'\x64\x86' + bytes(62)))
            self.assertEqual('coff', kind('big.obj', b'\x00\x00\xff\xff\x02\x00\x64\x86' + bytes(4)
                                          + isa_isolation.BIGOBJ_CLASS + bytes(36)))
            self.assertEqual('ltcg', kind('gl.obj', b'\x00\x00\xff\xff\x01\x00\x64\x86' + bytes(56)))
            self.assertEqual('bitcode', kind('thin.obj', b'BC\xc0\xde' + bytes(60)))
            self.assertEqual('gnu-lto', kind('slim.o', _elf_object([(b'.gnu.lto_.decls', 0, 8), (b'.text', 6, 0)])))
            self.assertEqual('elf', kind('fat.o', _elf_object([(b'.gnu.lto_.decls', 0, 8), (b'.text', 6, 4)])))
            self.assertEqual('elf', kind('plain.o', _elf_object([(b'.text', 6, 4)])))
            with self.assertRaises(isa_isolation.GuardError):
                kind('junk.o', b'junk' + bytes(60))

    def test_decoder_failures_and_silence_fail(self):
        with self.assertRaisesRegex(isa_isolation.GuardError, 'exited 3'):
            isa_isolation._run([sys.executable, '-c', 'import sys; sys.exit(3)'])
        with self.assertRaisesRegex(isa_isolation.GuardError, 'printed nothing'):
            isa_isolation._run([sys.executable, '-c', 'pass'])
        stream = isa_isolation._stream
        with self.assertRaisesRegex(isa_isolation.GuardError, 'exited 4'):
            list(stream([sys.executable, '-c', 'import sys; print("x"); sys.exit(4)'], isa_isolation._LABEL_ELF))
        with self.assertRaisesRegex(isa_isolation.GuardError, 'printed nothing'):
            list(stream([sys.executable, '-c', 'pass'], isa_isolation._LABEL_ELF))
        with tempfile.TemporaryDirectory() as temp:
            empty = Path(temp) / 'declares.o'
            empty.write_bytes(_elf_object([(b'.text', 6, 0), (b'.data', 3, 8)]))
            code = Path(temp) / 'code.o'
            code.write_bytes(_elf_object([(b'.text', 6, 4)]))
            silent = {'nm': sys.executable}
            with patch.object(isa_isolation, '_run', lambda command, empty_ok=False: ''):
                found, _layouts, functions = isa_isolation.symbols(silent, [str(empty)], False)
                self.assertEqual(({str(empty): []}, set()), (found, functions))
                with self.assertRaisesRegex(isa_isolation.GuardError, 'printed nothing'):
                    isa_isolation.symbols(silent, [str(empty), str(code)], False)

    def test_image_dumps_stream_in_chunks_that_keep_their_function(self):
        lines = ''.join(f'{i:016x} <f{i}()>:\n  {i:x}:\tc5 f8 77             \tvzeroupper\n' for i in range(400))
        # dumpbin ends its lines with CR LF; a pair split across two reads still ends one line.
        crlf = lines.replace('\n', '\r\n').encode()
        for script in (f'import sys; sys.stdout.write({lines!r})', f'import sys; sys.stdout.buffer.write({crlf!r})'):
            with patch.object(isa_isolation, 'CHUNK', 511):
                chunks = list(isa_isolation._stream([sys.executable, '-c', script], isa_isolation._LABEL_ELF))
                hits = [hit for chunk in chunks for hit in isa_isolation.scan_x64(chunk, False)]
            self.assertGreater(len(chunks), 10)
            self.assertFalse(any('\r' in chunk for chunk in chunks))
            self.assertEqual([(f'f{i}()', i) for i in range(400)],
                             [(function, address) for function, address, _, _ in hits])

    def test_map_owners_and_jump_tables(self):
        kernel = os.path.normpath('/b/x-avx2.dir/kernels/tensor_kernels.cpp.obj')
        kernels = {kernel: ('avx2', '/b/crd-hesap-tensor.lib')}
        generals = {os.path.normpath('/b/x.dir/src/einsum.cpp.obj'): ('crd-hesap-tensor', '/b/crd-hesap-tensor.lib'),
                    os.path.normpath('/b/t.dir/main.cpp.obj'): ('crd-t', '/b/t.exe')}
        owners = isa_isolation.Owners(kernels, generals)
        image = '/b/t.exe'
        self.assertEqual('kernel', owners.classify('crd-hesap-tensor:tensor_kernels.cpp.obj', image)[0])
        self.assertEqual('general', owners.classify('crd-hesap-tensor:einsum.cpp.obj', image)[0])
        self.assertEqual('general', owners.classify('main.cpp.obj', image)[0])
        self.assertEqual('general', owners.classify('t.exe.lto.crd-hesap-tensor.libeinsum.cpp.obj', image)[0])
        self.assertEqual('kernel', owners.classify('t.exe.lto.crd-hesap-tensor.libtensor_kernels.cpp.obj', image)[0])
        self.assertEqual('library', owners.classify('libvcruntime:ehhelpers.obj', image)[0])
        self.assertEqual('library', owners.classify('<linker defined>', image)[0])
        clash = dict(generals, **{os.path.normpath('/b/y.dir/tensor_kernels.cpp.obj'): ('crd-y', '')})
        with self.assertRaisesRegex(isa_isolation.GuardError, 'shares its name'):
            isa_isolation.Owners(kernels, clash)
        base = 0x140000000
        code = [(0x140001070, 'main', 'a.obj'), (0x1400010FC, '$LN15', 'a.obj'), (0x140001120, 'helper', 'b.obj'),
                (0x140001130, '$LN3', 'b.obj')]
        starts = [0x140001070, 0x140001120, 0x140001200]
        memory = {0x1400010FC: struct.pack('<I', 0x1091), 0x140001130: struct.pack('<I', 0x9999)}
        tables = isa_isolation.jump_tables(base, starts, code, lambda address, size: memory.get(address, b''))
        self.assertEqual([(0x1400010FC, 0x140001120)], tables)

    def test_manifest_and_il_without_images(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            kernel = root / 'k.obj'
            kernel.write_bytes(b'\x00\x00\xff\xff\x01\x00\x64\x86' + bytes(56))
            general = root / 'g.obj'
            general.write_bytes(b'\x00\x00\xff\xff\x01\x00\x64\x86' + bytes(56))
            manifest = root / 'm.manifest'
            lines = [f'kernel\tavx2\t\t{root / "x.lib"}\t{kernel}', f'general\tcrd-x\t{root / "x.lib"}\t{general}',
                     f'general\tcrd-x\t{manifest}\t{root / "gone.obj"}', f'image\tcrd-t\t{root / "t.exe"}']
            manifest.write_text('\n'.join(lines) + '\n', encoding='utf-8')
            args = SimpleNamespace(manifest=str(manifest), exceptions=str(isa_isolation.EXCEPTIONS), os='windows',
                                   arch='x64', jobs=1, limit=60)
            output = io.StringIO()
            with patch.object(isa_isolation, 'resolve_tools', return_value={}), contextlib.redirect_stdout(output):
                self.assertEqual(1, isa_isolation.run(args))
            text = output.getvalue()
            self.assertIn('kernel unit holds link-time IL', text)
            self.assertIn('no image was built', text)
            self.assertIn('gone.obj: missing, yet', text)
            manifest.write_text('kernel\tavx3\t\tcrd-x.lib\tk.obj\n', encoding='utf-8')
            with self.assertRaisesRegex(isa_isolation.GuardError, 'unknown level'):
                isa_isolation.read_manifest(str(manifest))
            manifest.write_text('object\tk.obj\n', encoding='utf-8')
            with self.assertRaisesRegex(isa_isolation.GuardError, 'malformed'):
                isa_isolation.read_manifest(str(manifest))


if __name__ == '__main__':
    unittest.main()
