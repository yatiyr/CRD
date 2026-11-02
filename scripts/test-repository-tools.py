#!/usr/bin/env python3
"""Regression checks for hygiene rejection and CMake's generated IDE model."""
from pathlib import Path
import base64
import contextlib
import copy
import datetime
import gzip
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
import time
import tomllib
import xml.etree.ElementTree as ET
import unittest
import urllib.error
import urllib.parse
import urllib.request
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
node_spec = importlib.util.spec_from_file_location('install_node', ROOT / 'scripts/install-node.py')
install_node = importlib.util.module_from_spec(node_spec)
node_spec.loader.exec_module(install_node)
cuda_spec = importlib.util.spec_from_file_location('install_cuda', ROOT / 'scripts/install-cuda.py')
install_cuda = importlib.util.module_from_spec(cuda_spec)
cuda_spec.loader.exec_module(install_cuda)
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
apt_spec = importlib.util.spec_from_file_location('install_apt', ROOT / 'scripts/install-apt-packages.py')
install_apt = importlib.util.module_from_spec(apt_spec)
apt_spec.loader.exec_module(install_apt)
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
environment_reads_spec = importlib.util.spec_from_file_location('check_environment_reads',
                                                                ROOT / 'scripts/check-environment-reads.py')
environment_reads = importlib.util.module_from_spec(environment_reads_spec)
environment_reads_spec.loader.exec_module(environment_reads)
frame_resets_spec = importlib.util.spec_from_file_location('check_frame_resets', ROOT / 'scripts/check-frame-resets.py')
frame_resets = importlib.util.module_from_spec(frame_resets_spec)
frame_resets_spec.loader.exec_module(frame_resets)
capability_owners_spec = importlib.util.spec_from_file_location('check_capability_owners',
                                                                ROOT / 'scripts/check-capability-owners.py')
capability_owners = importlib.util.module_from_spec(capability_owners_spec)
capability_owners_spec.loader.exec_module(capability_owners)
peer_headers_spec = importlib.util.spec_from_file_location('check_peer_headers', ROOT / 'scripts/check-peer-headers.py')
peer_headers = importlib.util.module_from_spec(peer_headers_spec)
peer_headers_spec.loader.exec_module(peer_headers)
fft_plans_spec = importlib.util.spec_from_file_location('gen_fft_level_plans', ROOT / 'scripts/gen_fft_level_plans.py')
fft_plans = importlib.util.module_from_spec(fft_plans_spec)
fft_plans_spec.loader.exec_module(fft_plans)
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
registers_spec = importlib.util.spec_from_file_location('check_registers', ROOT / 'scripts/check-registers.py')
registers = importlib.util.module_from_spec(registers_spec)
registers_spec.loader.exec_module(registers)
register_ids_spec = importlib.util.spec_from_file_location('gen_register_ids', ROOT / 'scripts/gen-register-ids.py')
register_ids = importlib.util.module_from_spec(register_ids_spec)
register_ids_spec.loader.exec_module(register_ids)
comply_rules_spec = importlib.util.spec_from_file_location('gen_comply_rules', ROOT / 'scripts/gen-comply-rules.py')
comply_rules = importlib.util.module_from_spec(comply_rules_spec)
comply_rules_spec.loader.exec_module(comply_rules)
crypto_inventory_spec = importlib.util.spec_from_file_location('gen_crypto_inventory',
                                                               ROOT / 'scripts/gen-crypto-inventory.py')
crypto_inventory = importlib.util.module_from_spec(crypto_inventory_spec)
crypto_inventory_spec.loader.exec_module(crypto_inventory)
text_fold_spec = importlib.util.spec_from_file_location('gen_text_fold', ROOT / 'scripts/gen-text-fold.py')
text_fold = importlib.util.module_from_spec(text_fold_spec)
text_fold_spec.loader.exec_module(text_fold)
sanctions_spec = importlib.util.spec_from_file_location('import_sanctions', ROOT / 'scripts/import-sanctions.py')
sanctions_import = importlib.util.module_from_spec(sanctions_spec)
sanctions_spec.loader.exec_module(sanctions_import)
review_spec = importlib.util.spec_from_file_location('register_review', ROOT / 'scripts/register-review.py')
register_review = importlib.util.module_from_spec(review_spec)
review_spec.loader.exec_module(register_review)
ab_spec = importlib.util.spec_from_file_location('ab_board', ROOT / 'scripts/ab-board.py')
ab_board = importlib.util.module_from_spec(ab_spec)
ab_spec.loader.exec_module(ab_board)
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

    def test_a_folded_row_cannot_stay_open(self):
        rows = self.rows('Done', 'Open', 'Partial')
        self.assertEqual(master.moved_row_errors(rows), [])
        rows[1][1][3] = 'Folded into [S3](#slice-s3) by the user; row id and anchor stay for links.'
        self.assertEqual(len(master.moved_row_errors(rows)), 1)
        rows[1][1][2] = 'Done'
        self.assertEqual(master.moved_row_errors(rows), [])

    def test_delivery_plan_holds_every_row_once_in_table_order(self):
        ids = ['S1', 'S2', 'S3', 'S4']
        plan = '### P00 A\n\nRows in order: S1, S2.\n\n### P01 B\n\nRows in order: S3,\nS4 (inserted by the applier).\n'
        self.assertEqual(master.delivery_plan_errors(ids, plan), [])
        for broken in (plan.replace('S3,\nS4', 'S4,\nS3'), plan.replace('S1, S2.', 'S1.'),
                       plan.replace('S1, S2.', 'S1, S2, S9.'), plan.replace('S1, S2.', 'S1, S2, S2.')):
            with self.subTest(plan=broken):
                self.assertTrue(master.delivery_plan_errors(ids, broken))

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
        _, (both,) = split_kernels(single, 3, 1)
        # Each unit holds every codelet whole for the levels whose lane is one register, then the split edition.
        self.assertTrue(both.startswith(opening + '\n#ifdef CRD_FFT_GEN_WHOLE_BODY\n'))
        self.assertTrue(both.endswith('\n#endif\n' + closing + '\n'))
        whole, split = both[len(opening) + 1:].split('\n#else\n')
        self.assertIn('codelet2_part0<V>(in, out, b, t);\n    }\n', whole)
        self.assertNotIn('_part1', whole)
        self.assertNotIn('sp[', whole)
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
        # The whole edition bounds a part's lines with its slot loads and stores, as MSVC stops optimizing a larger
        # function.
        _, (framed,) = split_kernels(single, 3, 1, whole_budget=5)
        whole = framed[len(opening) + 1:].split('\n#else\n')[0]
        self.assertIn('codelet2_part0<V>(in, out, b, t, sp);\n        codelet2_part1<V>(in, out, b, t, sp);\n', whole)
        self.assertNotIn('_part2', whole)
        with self.assertRaisesRegex(ValueError, 'codelet2: no part fits in 3 lines'):
            split_kernels(single, 3, 1, whole_budget=3)
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

    def test_text_encoding(self):
        def corrupt(text):
            # UTF-8 read as Windows-1252 (its five unassigned bytes as the C1 controls Windows gives them), saved as UTF-8.
            return ''.join(bytes([b]).decode('cp1252', errors='ignore') or chr(b) for b in text.encode('utf-8')).encode()

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fixture = root / 'note.md'

            def errors(data):
                fixture.write_bytes(data)
                return hygiene.text_file_errors(root, ['note.md'])

            clean = 'em dash \u2014, section \u00a7, \u00e9, \u011f, star \u2b50, 1.2\u00d7\u20133\u00d7, ODT\u00dc\u2019de\n'
            self.assertEqual(errors(clean.encode('utf-8')), [])
            self.assertEqual(errors(b'\xef\xbb\xbf' + clean.encode('utf-8')),
                             ['Text file starts with a UTF-8 byte order mark: note.md'])
            self.assertEqual(errors(b'caf\xe9\n'), ['Text file is not valid UTF-8 (byte 3): note.md'])
            # Three-byte (an em dash, and a star whose last byte 0x90 has no Windows-1252 character), two-byte Latin-1
            # and Latin Extended-A (a section sign, e acute, g breve) and a four-byte character, each double-encoded.
            for character in ('\u2014', '\u2b50', '\u00a7', '\u00e9', '\u011f', '\U0001f600'):
                found = errors(corrupt(f'x {character} y\n'))
                self.assertEqual(len(found), 1, ascii(character))
                self.assertIn(f'for {ascii(character)}', found[0])
            self.assertIn('2 sequences', errors(corrupt('\u2014 and \u2192\n'))[0])
            # A lead with too few tail characters, or whose bytes are no UTF-8 character (E0 80 80 is overlong), is text.
            self.assertEqual(errors('\u00e2\u20acA \u00e0\u20ac\u20ac\n'.encode('utf-8')), [])
            # Per-file exceptions: the sequence is accepted in that file only.
            notation = 'where \u00c2\u00b7m = e\n'.encode('utf-8')
            self.assertEqual(len(errors(notation)), 1)
            with patch.dict(hygiene.MOJIBAKE_EXCEPTIONS, {'note.md': {'\u00c2\u00b7': 'A-hat times m'}}):
                self.assertEqual(errors(notation), [])
                self.assertEqual(len(errors(corrupt('\u2014\n'))), 1)

    def test_checkout_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)

            def errors(name, text):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding='utf-8')
                return hygiene.text_file_errors(root, [name])

            spellings = ('D:/Dev/cerid/tests/a.inc', 'd:\\Dev\\cerid\\scripts', 'D:\\\\Dev\\\\cerid-track-b',
                         '/mnt/d/Dev/cerid/scripts/x.sh', 'C://dev//Cerid-perf', '/mnt/e/dev/CERID')
            for spelling in spellings:
                found = errors('scripts/gen.py', f'import os\nOUT = "{spelling}"\n')
                self.assertEqual(len(found), 1, spelling)
                self.assertIn('scripts/gen.py:2 (1 in the file, the first ', found[0])
                self.assertIn(repr(spelling[:spelling.lower().index('cerid') + 5]), found[0])
            self.assertIn('scripts/gen.py:1 (2 in the file, the first ',
                          errors('scripts/gen.py', 'a = "D:/Dev/cerid"\nb = "/mnt/d/Dev/cerid"\n')[0])
            self.assertEqual(len(errors('engine/a.cpp', '// run D:/Dev/cerid/build/x\n')), 1)
            # Records and the tool tests' sample text name checkouts; other paths are not checkouts.
            for name in ('docs/sessions/note.txt', 'MEMORY.md', 'tools/README.md', 'scripts/test-tool.py'):
                self.assertEqual(errors(name, 'ran in D:\\Dev\\cerid and /mnt/d/Dev/cerid\n'), [], name)
            for text in ('/home/u/cerid-src', '~/cerid-mirror-build', 'D:/Dev/nav/cerid', '/dev/cerid',
                         'C:/Users/u/Dev', 'scripts/cerid/dev/x'):
                self.assertEqual(errors('scripts/gen.py', text + '\n'), [], text)

    def test_text_names_follow_git_text_detection(self):
        listing = ('i/lf    w/lf    attr/text eol=lf      \tsrc/a.cpp\0'
                   'i/-text w/-text attr/text=auto        \tdata/b.npy\0'
                   'i/lf    w/lf    attr/-text            \ttests/fuzz/corpus/seed\0'
                   'i/none  w/none  attr/text=auto        \tdocs/with space.md\0'
                   'i/lf    w/crlf  attr/text eol=crlf    \tscripts/run.bat\0')
        self.assertEqual(hygiene.text_names(listing), ['src/a.cpp', 'docs/with space.md', 'scripts/run.bat'])

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

    def test_a_shard_is_held_to_the_registered_tests_it_runs_and_each_must_be_in_the_lane(self):
        def inventory(directory, name, tests):
            path = Path(directory) / name
            path.write_text(json.dumps({'kind': 'ctest', 'tests': [{'name': n} for n in tests]}), encoding='utf-8')
            return str(path)

        with tempfile.TemporaryDirectory() as temp:
            register = Path(temp) / 'register.md'
            register.write_text(self.register_text(self.LANES), encoding='utf-8')
            lane = inventory(temp, 'lane.json', ['A (DX12)', 'B (DX12)', 'C', 'D'])
            first = inventory(temp, 'first.json', ['A (DX12)', 'C'])
            second = inventory(temp, 'second.json', ['B (DX12)', 'D'])

            def run(states, selection, ctest_exit, lane_inventory=lane):
                junit = Path(temp) / 'ctest.xml'
                junit.write_text(self.junit_text(states), encoding='utf-8')
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    code = gate.main(['--register', str(register), '--lane', 'win-asan', '--junit', str(junit),
                                      '--ctest-exit', str(ctest_exit), '--selection', selection,
                                      '--lane-inventory', lane_inventory])
                return code, output.getvalue()

            # Each shard sees its own registered failure; the other shard's is not expected of it.
            self.assertEqual(run({'A (DX12)': 'failed', 'C': 'passed'}, first, 8)[0], 0)
            self.assertEqual(run({'B (DX12)': 'failed', 'D': 'passed'}, second, 8)[0], 0)
            code, report = run({'A (DX12)': 'passed', 'C': 'passed'}, first, 0)
            self.assertEqual(code, 1, report)
            self.assertIn('UNEXPECTED PASSED: registered TP-1 did not fail: A (DX12)', report)
            code, report = run({'B (DX12)': 'failed', 'D': 'failed'}, second, 8)
            self.assertEqual(code, 1, report)
            self.assertIn('UNEXPECTED FAILURE: D', report)
            # A registered test no shard runs fails every shard's gate.
            short = inventory(temp, 'short.json', ['A (DX12)', 'C', 'D'])
            code, report = run({'A (DX12)': 'failed', 'C': 'passed'}, first, 8, short)
            self.assertEqual(code, 1, report)
            self.assertIn('UNEXPECTED ABSENT: registered TP-1 is not in the lane inventory: B (DX12)', report)
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                gate.main(['--lane', 'win-asan', '--junit', 'x.xml', '--selection', first])
            junit = Path(temp) / 'ctest.xml'
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(gate.main(['--register', str(register), '--lane', 'win-asan', '--junit', str(junit),
                                            '--selection', str(register), '--lane-inventory', lane]), 2)

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

    def test_sharded_lanes_carry_their_shard_and_every_job_stops_within_the_limit(self):
        entries = self.tiers['presets']
        self.assertEqual({n for n, e in entries.items() if 'shards' in e}, {'win-asan', 'linux-gcc-asan'})
        blocks = ci_tiers.workflow_jobs(self.workflow)
        steps = self.tiers['preset_steps']
        self.assertEqual(ci_tiers.validate_shards(entries, blocks, steps), [])
        self.assertEqual(ci_tiers.validate_time_limits(blocks), [])
        self.assertEqual(len(ci_tiers.command_calls(blocks['windows'], 'ci-evidence.py bundle')), 1)

        def problems(job, old, new, mapping=entries):
            changed = dict(blocks, **{job: blocks[job].replace(old, new)})
            self.assertNotEqual(changed[job], blocks[job], old)
            return ci_tiers.validate_shards(mapping, changed, steps)

        self.assertTrue(any('does not set CRD_SHARD' in p for p in problems('windows', ci_tiers.SHARD_ENV, 'X: 1')))
        bundle = problems('linux-gcc', '--shard="$CRD_SHARD" --summary', '--summary')
        self.assertEqual(bundle, ['job linux-gcc runs sharded preset linux-gcc-asan but its ci-evidence.py bundle '
                                  'does not take --shard='])
        shared = problems('windows', "format('-shard-{0}-of-{1}'", "format('-{0}-of-{1}'")
        self.assertEqual(shared, ['job windows runs sharded preset win-asan but its legs share one evidence name'])
        every_shard = problems('linux-gcc', ' && (!matrix.shard || matrix.shard == 1)', '')
        self.assertEqual(every_shard, ["step 'Seeded detector failures' rides on sharded preset linux-gcc-asan but "
                                       'is not gated on (!matrix.shard || matrix.shard == 1)'])
        ungated = problems('windows', "'--lane-inventory'", "'--lane'")
        self.assertTrue(any('gets no --lane-inventory' in p for p in ungated), ungated)
        for count in (1, '2', True):
            mapping = dict(entries, **{'win-asan': dict(entries['win-asan'], shards=count)})
            self.assertEqual(ci_tiers.validate_shards(mapping, blocks, steps),
                             ['preset win-asan: shards is a count of at least 2 on a hosted preset'], count)
        unlimited = dict(blocks, **{'clang-cl': blocks['clang-cl'].replace('    timeout-minutes: 300\n', '')})
        self.assertEqual(ci_tiers.validate_time_limits(unlimited), ['job clang-cl declares no timeout-minutes'])
        longer = dict(blocks, windows=blocks['windows'].replace('timeout-minutes: 300', 'timeout-minutes: 360'))
        self.assertEqual(ci_tiers.validate_time_limits(longer), ['job windows may run 360 minutes, more than 300'])
        listed = self.workflow.replace('include: ${{ fromJSON(needs.preflight.outputs.presets_clang_cl) }}',
                                       'preset: ${{ fromJSON(needs.preflight.outputs.presets_clang_cl) }}')
        self.assertTrue(any('clang-cl must take its matrix entries' in p
                            for p in ci_tiers.validate(self.tiers, self.presets, listed)))

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
        self.assertEqual(change['matrix']['windows'], [{'preset': 'win-debug'}, {'preset': 'win-release'},
                                                       {'preset': 'win-asan', 'shard': 1, 'shards': 2},
                                                       {'preset': 'win-asan', 'shard': 2, 'shards': 2}])
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
            written = dict(line.split('=', 1) for line in out.read_text(encoding='utf-8').splitlines())
            self.assertEqual(json.loads(written['presets_windows']), change['matrix']['windows'])
            self.assertIn('`win-asan` 2/2', ci_tier.resolution_markdown(change))
            keys = set(written)
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
    MACHINE = {'cpu': 'AMD EPYC 7763', 'logical_cpus': 4, 'memory_gib': 16.0}
    HOST_TEST = {'command': ['ctest'], 'exit': 0, 'seconds': 7200.0, 'machine': MACHINE,
                 'memory': {'samples': 1441, 'baseline_available_gib': 13.1, 'min_available_gib': 9.4,
                            'baseline_committed_gib': 4.0, 'peak_committed_gib': 9.9, 'pressure_gib': 5.9}}

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
        (build / ci_evidence.HOST_TEST).write_text(json.dumps(self.HOST_TEST), encoding='utf-8')
        adapters = Path(temp) / 'adapters.txt'
        adapters.write_text('Name : Microsoft Basic Render Driver\n\nDriverVersion : 10.0\n', encoding='utf-8')
        return build, adapters

    def test_bundle_records_identity_toolchain_inventory_and_failures(self):
        with tempfile.TemporaryDirectory() as temp:
            build, adapters = self.fixture(temp)
            out = Path(temp) / 'evidence'
            record = ci_evidence.bundle('win-debug', build, out, adapters=adapters, environ={'GITHUB_RUN_ID': '7'},
                                        host=lambda: self.MACHINE)
            self.assertEqual(record['status'], 'fail')
            self.assertEqual(record['host'], {'machine': self.MACHINE, 'test': self.HOST_TEST})
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
            self.assertIsNone(record['shard'])
            shard = ci_evidence.bundle('win-asan', build, out, environ={}, host=lambda: self.MACHINE, shard=(2, 2))
            self.assertEqual(shard['shard'], '2/2')
            self.assertIn('### win-asan shard 2/2: fail', ci_evidence.summary_markdown(shard))
            self.assertIn('`broken` (0.5 s): boom', markdown)
            self.assertIn('3 executed: 1 passed, 1 failed, 1 skipped', markdown)
            self.assertIn('| Diagnostic modes | ASan |', markdown)
            self.assertIn('| By label | gpu 2 executed: 1 passed, 0 failed, 1 skipped |', markdown)
            self.assertIn('| Host | AMD EPYC 7763, 4 logical CPUs, 16.0 GiB; tests 7200 s wall, at least 9.4 GiB '
                          'available (13.1 before), commit peak 9.9 GiB |', markdown)
            passed = ci_evidence.bundle('win-debug', build, out, adapters=adapters, outcome='success', environ={})
            self.assertEqual(passed['status'], 'pass')
            self.assertEqual(ci_evidence.bundle('win-debug', build, out, outcome='failure', environ={})['status'], 'failure')

    def test_bundle_without_inputs_reports_what_is_missing(self):
        with tempfile.TemporaryDirectory() as temp:
            record = ci_evidence.bundle('none', Path(temp) / 'nowhere', Path(temp) / 'out', environ={})
            self.assertEqual(record['status'], 'missing')
            self.assertEqual(record['missing'], ['CMakeCache.txt', 'adapters', 'host-test', 'inventory', 'junit'])
            self.assertEqual(record['host']['test'], None)
            self.assertTrue(record['host']['machine']['logical_cpus'])
            self.assertIn('Missing evidence: CMakeCache.txt, adapters, host-test, inventory, junit',
                          ci_evidence.summary_markdown(record))

    @unittest.skipUnless(os.name == 'nt', 'Windows sharing violations')
    def test_the_host_record_rename_rides_out_a_sharing_violation(self):
        from unittest import mock
        with mock.patch('os.replace', side_effect=[PermissionError(13, 'denied'), PermissionError(13, 'denied'),
                                                   None]) as replace:
            ci_evidence.replace_file('a', 'b', sleep=lambda s: None)
        self.assertEqual(replace.call_count, 3)
        with mock.patch('os.replace', side_effect=PermissionError(13, 'denied')):
            with self.assertRaises(PermissionError):
                ci_evidence.replace_file('a', 'b', attempts=3, sleep=lambda s: None)

    def test_watch_runs_the_tests_with_the_host_sampled_and_passes_their_exit_code_on(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / ci_evidence.HOST_TEST
            seen = []

            def runner(command):
                deadline = time.monotonic() + 10.0
                while not path.is_file() and time.monotonic() < deadline:
                    time.sleep(0.01)
                if path.is_file():
                    seen.append(json.loads(path.read_text(encoding='utf-8')))
                return 7

            ticks = iter(range(10 ** 6))
            code = ci_evidence.watch(temp, ['ctest', '--preset', 'win-asan'], interval=0.01, runner=runner,
                                     clock=lambda: 10.0 + next(ticks), host=lambda: self.MACHINE)
            self.assertEqual(code, 7)
            self.assertTrue(seen, 'no record while the tests ran')
            self.assertIsNone(seen[0]['exit'])
            self.assertEqual(seen[0]['machine'], self.MACHINE)
            record = json.loads(path.read_text(encoding='utf-8'))
            self.assertEqual((record['command'], record['exit'], record['machine']),
                             (['ctest', '--preset', 'win-asan'], 7, self.MACHINE))
            self.assertGreater(record['seconds'], seen[0]['seconds'])
            self.assertEqual(list(path.parent.glob('*.partial')), [])
            unfinished = dict(self.HOST_TEST, exit=None)
            self.assertTrue(ci_evidence.host_markdown({'machine': self.MACHINE, 'test': unfinished})
                            .endswith('commit peak 9.9 GiB (the tests did not return)'))
            self.assertGreaterEqual(record['memory']['samples'], 2)
            self.assertGreater(record['memory']['baseline_available_gib'], 0)
            exit_four = [sys.executable, '-c', 'raise SystemExit(4)']
            self.assertEqual(ci_evidence.main(['watch', '--build-dir', temp, '--'] + exit_four), 4)
            self.assertEqual(ci_evidence.main(['watch', '--build-dir', temp] + exit_four), 4)
            self.assertEqual(json.loads((Path(temp) / ci_evidence.HOST_TEST).read_text(encoding='utf-8'))['command'],
                             exit_four)
            self.assertEqual(ci_evidence.main(['watch', '--build-dir', temp]), 2)

    def test_a_shard_inventory_records_the_lane_and_checks_the_stride_partition(self):
        lane = {'kind': 'ctest', 'tests': [{'name': 't%d' % i, 'properties': []} for i in range(1, 6)]
                + [{'name': 'setup', 'properties': [{'name': 'FIXTURES_SETUP', 'value': ['f']}]}]}

        def stride(index, count, extra=()):
            names = [test['name'] for test in lane['tests']][index - 1::count]
            return {'kind': 'ctest', 'tests': [{'name': n} for n in list(names) + list(extra)]}

        self.assertEqual(ci_evidence.parse_shard(''), None)
        self.assertEqual(ci_evidence.parse_shard('2/3'), (2, 3))
        for bad in ('0/2', '3/2', '1/1', '1-2', '/2'):
            with self.assertRaises(ValueError):
                ci_evidence.parse_shard(bad)
        self.assertEqual(ci_evidence.selection_args((2, 3)), ['-I', '2,,3'])
        self.assertEqual(ci_evidence.selection_args(None), [])
        self.assertEqual(ci_evidence.partition_problems(lane, [stride(1, 2, ['setup']), stride(2, 2)]), [])
        self.assertEqual(ci_evidence.partition_problems(lane, [stride(1, 2, ['t2']), {'tests': [{'name': 't4'}]}]),
                         ['shard 1/2 also runs t2', 'shard 2/2 lacks setup', 'shard 2/2 lacks t2'])

        def ctest(answers):
            seen = []

            def run(command, **_):
                seen.append(command)
                key = command[command.index('-I') + 1] if '-I' in command else 'lane'
                return subprocess.CompletedProcess(command, 0, json.dumps(answers[key]), '')
            return run, seen

        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            run, seen = ctest({'lane': lane, '1,,2': stride(1, 2), '2,,2': stride(2, 2)})
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(ci_evidence.run_inventory(build, shard=(2, 2), run=run), 0)
            self.assertEqual([c[-2:] for c in seen], [[str(build), '--show-only=json-v1'], ['-I', '1,,2'],
                                                      ['-I', '2,,2']])
            recorded = json.loads((build / 'Testing/inventory.json').read_text(encoding='utf-8'))
            self.assertEqual([test['name'] for test in recorded['tests']], ['t2', 't4', 'setup'])
            self.assertEqual(json.loads((build / 'Testing' / ci_evidence.LANE_INVENTORY).read_text(encoding='utf-8')), lane)
            run, _ = ctest({'lane': lane, '1,,2': stride(1, 2), '2,,2': {'tests': [{'name': 't2'}]}})
            (build / 'Testing' / ci_evidence.LANE_INVENTORY).unlink()
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(ci_evidence.run_inventory(build, shard=(1, 2), run=run), 1)
            self.assertIn('FAIL: shard 2/2 lacks t4', output.getvalue())
            self.assertFalse((build / 'Testing' / ci_evidence.LANE_INVENTORY).exists())
            run, seen = ctest({'lane': lane})
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(ci_evidence.run_inventory(build, run=run), 0)
            self.assertEqual(len(seen), 1)
            self.assertEqual(json.loads((build / 'Testing/inventory.json').read_text(encoding='utf-8')), lane)
            exit_args = [sys.executable, '-c', 'import sys; raise SystemExit(len(sys.argv))']
            self.assertEqual(ci_evidence.main(['watch', '--build-dir', temp, '--shard=2/3', '--'] + exit_args), 3)
            self.assertEqual(json.loads((build / ci_evidence.HOST_TEST).read_text(encoding='utf-8'))['command'],
                             exit_args + ['-I', '2,,3'])
            self.assertEqual(ci_evidence.main(['watch', '--build-dir', temp, '--shard=', '--'] + exit_args), 1)
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                ci_evidence.main(['watch', '--build-dir', temp, '--shard=4/3', '--'] + exit_args)

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
            # A sharded lane is read from its shards' directories together.
            first = lane(Path(temp) / 'first', 'win-asan', [('a', 2.0, True), ('b', 1.0, True)], ['ASan'])
            second = lane(Path(temp) / 'second', 'win-asan', [('c', 3.0, True), ('d', 6.0, True)], ['ASan'])
            self.assertEqual(ci_evidence.compare_markdown([first, second], [reference]),
                             '| win-asan (ASan) | win-debug (modes not recorded) | 4 | 2.00 | 1.00-3.00 | 12 s / 5 s '
                             '| 12 s / 7 s |')

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
        workflows = ''.join(path.read_text(encoding='utf-8')
                            for path in sorted((ROOT / '.github/workflows').glob('*.yml')))
        for action, pin in self.pins['actions'].items():
            self.assertIn('%s@%s # %s' % (action, pin['sha'], pin['ref']), workflows)
        self.assertEqual(re.findall(r'runs-on:\s*\S*-latest', self.workflow), [])
        self.assertEqual(len(re.findall(r'uses:\s*actions/checkout@', self.workflow)),
                         self.workflow.count('persist-credentials: false'))
        self.assertEqual(self.workflow.count("python-version: '3.12'"), 13)
        for package in ('Catch2', 'glfw', 'zstd', 'aws-lc', 'nasm', 'wycheproof', 'mls-vectors', 'stb', 'cgltf',
                        'MikkTSpace', 'imgui', 'eigen', 'OpenBLAS', 'boost', 'hptt', 'pocketfft', 'highway', 'tracy',
                        'nlopt'):
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

            repository = (Path(temp) / sdk.REPOSITORY).resolve()

            def fake_run(command, check):
                self.assertEqual(command[0], str(installer))
                self.assertEqual(command[1:], [*sdk.INSTALL_ARGUMENTS, '--set-temp-repository', repository.as_uri()])
                updates = (repository / 'Updates.xml').read_text(encoding='utf-8')
                self.assertEqual((updates.count('<Updates>'), updates.count('<PackageUpdate>')), (1, 0))
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

    def test_vulkan_sdk_installer_arms_record_their_evidence_and_are_judged_against_two_baselines(self):
        spec = importlib.util.spec_from_file_location('installer_check', ROOT / 'scripts/vulkan-sdk-installer-check.py')
        check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(check)
        workflow = (ROOT / '.github/workflows/host-check.yml').read_text(encoding='utf-8')
        matrix = re.search(r'arm: \[([^\]]*)\]', workflow).group(1)
        self.assertEqual([arm.strip() for arm in matrix.split(',')], list(check.ARMS))
        options = re.search(r'options: \[([^\]]*)\]', workflow).group(1)
        self.assertEqual([option.strip() for option in options.split(',')][-2:],
                         ['vulkan-sdk-installer', 'vulkan-sdk-installer-emitter'])
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            installer = temp / 'installer.exe'
            installer.write_bytes(b'installer')
            evidence, cache = temp / 'evidence', temp / 'cache'
            commands = []
            layout = check.sdk.LAYOUT

            def installing(warn, remote, extra=None, log=b'run 1', code=0):
                def fake_run(command, stdout, stderr, timeout, check):
                    commands.append(command)
                    self.assertEqual((stdout, stderr, check), (subprocess.PIPE, subprocess.STDOUT, False))
                    for relative in layout:
                        (root / relative).parent.mkdir(parents=True, exist_ok=True)
                        (root / relative).write_bytes(b'payload')
                    (root / 'InstallationLog.txt').write_bytes(log)
                    if extra:
                        (root / extra).write_bytes(b'extra')
                    lines = [b'Fetching latest update information...', b'Loading component scripts...']
                    if warn:
                        lines.append(b'[1837] Warning: QFont::setPixelSize: Pixel size <= 0 (-1)')
                    if remote:
                        lines.append(b'Retrieving meta information from remote repository... https://sdk.lunarg.com/x')
                        (cache / 'entry').mkdir(parents=True, exist_ok=True)
                        (cache / 'entry' / 'repository.txt').write_bytes(b'https://sdk.lunarg.com/x\n')
                    return subprocess.CompletedProcess(command, code, stdout=b'\n'.join(lines))
                return fake_run

            plan = {'baseline': installing(True, True, log=b'run 1'),
                    'baseline-repeat': installing(True, True, log=b'run 22'),
                    'platform-minimal': installing(False, True),
                    'temp-repository-updates': installing(True, False, extra='Bin/extra.dll'),
                    'minimal-updates': installing(False, False),
                    'offscreen-updates': installing(False, False, code=4)}
            for arm, fake_run in plan.items():
                root = temp / 'machines' / arm / 'VulkanSDK'
                shutil.rmtree(cache, ignore_errors=True)
                with patch.object(check.pins, 'fetch', return_value=installer), \
                        contextlib.redirect_stdout(io.StringIO()):
                    result = check.run_arm(arm, temp / 'downloads', evidence, root=root, cache=cache, run=fake_run)
                self.assertEqual(result['arm'], arm)
            with self.assertRaisesRegex(check.pins.PinError, 'exists before'):
                check.run_arm('baseline', temp / 'downloads', evidence, root=root, cache=cache, run=None)
            with self.assertRaisesRegex(check.pins.PinError, 'unknown arm'):
                check.run_arm('offline', temp / 'downloads', evidence, root=temp / 'none', cache=cache, run=None)

            self.assertEqual(commands[0][1:], [*check.sdk.INSTALL_ARGUMENTS])
            updates = (temp / 'downloads' / 'installer-repositories' / check.UPDATES).resolve()
            self.assertEqual(commands[4][1:], [*check.sdk.INSTALL_ARGUMENTS, '--platform', 'minimal',
                                               '--set-temp-repository', updates.as_uri()])
            self.assertIn('<Checksum>true</Checksum>', (updates / 'Updates.xml').read_text(encoding='utf-8'))
            baseline = json.loads((evidence / 'baseline' / 'result.json').read_text(encoding='utf-8'))
            self.assertEqual(len(baseline['qt_font']), 1)
            self.assertEqual(len(baseline['remote']), 1)
            self.assertEqual(baseline['hosts'], ['sdk.lunarg.com'])
            self.assertEqual((baseline['cache_before'], baseline['cache_after']), ({}, {'entry/repository.txt': 25}))
            self.assertEqual(baseline['cache_repositories'], {'entry/repository.txt': 'https://sdk.lunarg.com/x'})
            self.assertEqual(baseline['layout_missing'], [])
            self.assertEqual(baseline['tree']['files'], len(check.sdk.LAYOUT) + 1)
            self.assertIn('QFont', (evidence / 'baseline' / 'log.txt').read_text(encoding='utf-8'))

            lines, judgement = check.judge(evidence)
            self.assertEqual(judgement['variable'], ['InstallationLog.txt'])
            verdicts = {arm: entry['verdict'] for arm, entry in judgement['arms'].items()}
            self.assertEqual(verdicts, {'baseline': 'findings remain', 'baseline-repeat': 'findings remain',
                                        'verbose': 'no evidence', 'platform-minimal': 'findings remain',
                                        'platform-offscreen': 'no evidence', 'temp-repository-empty': 'no evidence',
                                        'temp-repository-updates': 'tree differs',
                                        'minimal-updates': 'removes both, same tree',
                                        'offscreen-updates': 'install failed'})
            self.assertEqual(judgement['arms']['temp-repository-updates']['added'], ['Bin/extra.dll'])
            self.assertIn('| minimal-updates | --platform minimal --set-temp-repository updates-repository | 0 | 0 | 0 '
                          '| none | 0 | same | removes both, same tree |', lines)
            self.assertIn('- temp-repository-updates, added: `Bin/extra.dll`', lines)

            # Recorded evidence is judged by the current patterns: a result.json written by an older one that
            # counted the framework's generic fetch line still judges from its log.
            stale = evidence / 'minimal-updates' / 'result.json'
            stale.write_text(json.dumps({**json.loads(stale.read_text(encoding='utf-8')),
                                         'remote': ['Fetching latest update information...']}), encoding='utf-8')
            self.assertEqual(check.judge(evidence)[1]['arms']['minimal-updates']['verdict'], 'removes both, same tree')

            summary = temp / 'summary.md'
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(check.main(['judge', '--evidence', str(evidence), '--summary', str(summary)]), 0)
            self.assertIn('removes both, same tree', summary.read_text(encoding='utf-8'))
            self.assertTrue((evidence / 'judgement.json').is_file())
            shutil.rmtree(evidence / 'baseline-repeat')
            with contextlib.redirect_stderr(io.StringIO()) as error:
                self.assertEqual(check.main(['judge', '--evidence', str(evidence)]), 1)
            self.assertIn('baseline-repeat arm left no evidence', error.getvalue())

    def test_vulkan_sdk_installer_trace_names_the_qfont_warnings_caller(self):
        spec = importlib.util.spec_from_file_location('installer_check', ROOT / 'scripts/vulkan-sdk-installer-check.py')
        check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(check)
        workflow = (ROOT / '.github/workflows/host-check.yml').read_text(encoding='utf-8')
        self.assertIn("inputs.experiment == 'vulkan-sdk-installer-emitter'", workflow)
        self.assertIn('vulkan-sdk-installer-check.py trace', workflow)
        self.assertEqual(len(check.CALLERS), 15)
        self.assertEqual(check.CENSUS_SHA256, pins_registry.entry('tools', 'vulkan-sdk-windows')['sha256'])
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            installer = temp / 'installer.exe'
            header = bytearray(4096)
            header[:2] = b'MZ'
            struct.pack_into('<I', header, 0x3c, 0x80)
            header[0x80:0x84] = b'PE\0\0'
            struct.pack_into('<I', header, 0x80 + 24 + 16, 0x1234)
            installer.write_bytes(bytes(header))
            self.assertEqual(check.entry_rva(installer), 0x1234)
            (temp / 'not-pe.exe').write_bytes(b'MZ' + bytes(4094))
            with self.assertRaisesRegex(check.pins.PinError, 'not a PE image'):
                check.entry_rva(temp / 'not-pe.exe')

            kits = temp / 'kits'
            with self.assertRaisesRegex(check.pins.PinError, 'neither on PATH'):
                check.find_cdb(which=lambda name: None, program_files=kits)
            (kits / check.CDB_DEFAULT).parent.mkdir(parents=True)
            (kits / check.CDB_DEFAULT).write_bytes(b'cdb')
            self.assertEqual(check.find_cdb(which=lambda name: None, program_files=kits), kits / check.CDB_DEFAULT)
            self.assertEqual(check.find_cdb(which=lambda name: 'C:/tools/cdb.exe'), Path('C:/tools/cdb.exe'))

            root = temp / 'VulkanSDK'

            def stack(*sites):
                # As run 41312886689 printed them: padded columns, sites named by the image's nearest export.
                lines = ['Child-SP          RetAddr               Call Site']
                lines += ['0000006b`947ce098 00007ff6`c02bdef1     ' + site for site in sites]
                return lines

            log = ['0:000> g', '[74] Loading component scripts...', 'edx=00000010', '$t0=0000000000209c3a',
                   *stack('installerbase!qt_startup_hook+0x79ed10', 'installerbase!qt_startup_hook+0x1a1eba',
                          'KERNEL32!BaseThreadInitThunk+0x17'),
                   'edx=ffffffff', '$t0=000000000091def1',
                   *stack('installerbase!qt_startup_hook+0x79ed10', 'installerbase!qt_startup_hook+0x8b6171'),
                   '[485] Warning: QFont::setPixelSize: Pixel size <= 0 (-1)',
                   'edx=00000000', '$t0=0000000000123456', *stack('installerbase+0x806a90', 'installerbase+0x123456'),
                   'edx=fffffffe', '$t0=ffff80097fe4ed21',
                   *stack('installerbase+0x806a90', 'ntdll!RtlUserThreadStart+0x21'),
                   'edx=ffffffff', *stack('installerbase+0x806a90')]
            commands = []
            layout = check.sdk.LAYOUT

            def debugging(command, stdin, stdout, stderr, timeout, check):
                commands.append(command)
                self.assertEqual((stdin, stdout, stderr, check),
                                 (subprocess.DEVNULL, subprocess.PIPE, subprocess.STDOUT, False))
                for relative in layout:
                    (root / relative).parent.mkdir(parents=True, exist_ok=True)
                    (root / relative).write_bytes(b'payload')
                return subprocess.CompletedProcess(command, 0, stdout='\n'.join(log).encode('utf-8'))

            with patch.object(check.pins, 'fetch', return_value=installer), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = check.trace(temp / 'downloads', temp / 'evidence', root=root, run=debugging,
                                     which=lambda name: None, program_files=kits)
            repository = (temp / 'downloads' / check.sdk.REPOSITORY).resolve()
            self.assertEqual(commands[0], [str(kits / check.CDB_DEFAULT), '-G', '-cf',
                                           str(temp / 'evidence' / 'trace' / 'commands.txt'), str(installer),
                                           *check.sdk.INSTALL_ARGUMENTS, '--set-temp-repository', repository.as_uri()])
            self.assertEqual((temp / 'evidence' / 'trace' / 'commands.txt').read_text(encoding='ascii'),
                             'bp @$exentry-0x1234+0x806a90 "r edx; r $t0 = poi(@rsp)-(@$exentry-0x1234); r $t0; '
                             'k 12; gc"\ng\nq\n')
            self.assertEqual([call['pixel_size'] for call in result['calls']], [16, -1, 0, -2, -1])
            self.assertEqual([call['caller_rva'] for call in result['calls']],
                             ['0x209c3a', '0x91def1', '0x123456', 'outside the image', None])
            self.assertEqual([call['caller'] for call in result['calls']],
                             [check.CALLERS[0x209c3a], check.CALLERS[0x91def1], None, None, None])
            self.assertIn('QTextFormatPrivate::resolveFont', check.CALLERS[0x91def1])
            self.assertEqual(result['calls'][0]['frames'],
                             ['installerbase!qt_startup_hook+0x79ed10', 'installerbase!qt_startup_hook+0x1a1eba',
                              'KERNEL32!BaseThreadInitThunk+0x17'])
            self.assertEqual(result['emitters'], [check.CALLERS[0x91def1]])
            self.assertEqual(result['unnamed'], ['0x123456', 'no return address', 'outside the image'])
            self.assertEqual(result['qt_font'], ['[485] Warning: QFont::setPixelSize: Pixel size <= 0 (-1)'])
            self.assertEqual(result['layout_missing'], [])
            recorded = json.loads((temp / 'evidence' / 'trace' / 'result.json').read_text(encoding='utf-8'))
            self.assertEqual(recorded['emitters'], result['emitters'])

            # The leg passes only when the install completed, the breakpoint saw a size <= 0 and the census names
            # the caller of each such call.
            arguments = ['trace', '--download-dir', str(temp / 'downloads'), '--evidence', str(temp / 'evidence')]
            for outcome, code in (({'exit': 0, 'layout_missing': [], 'emitters': ['x'], 'unnamed': []}, 0),
                                  ({'exit': 0, 'layout_missing': ['Bin/glslc.exe'], 'emitters': ['x'], 'unnamed': []},
                                   1),
                                  ({'exit': 0, 'layout_missing': [], 'emitters': [], 'unnamed': []}, 1),
                                  ({'exit': 0, 'layout_missing': [], 'emitters': ['x'], 'unnamed': ['0x123456']}, 1),
                                  ({'exit': 0, 'layout_missing': [], 'emitters': [], 'unnamed': ['0x123456']}, 1),
                                  ({'exit': 'timeout', 'layout_missing': [], 'emitters': ['x'], 'unnamed': []}, 1)):
                with patch.object(check, 'trace', return_value=outcome):
                    self.assertEqual(check.main(arguments), code)
            pin = dict(pins_registry.entry('tools', 'vulkan-sdk-windows'), sha256='0' * 64)
            with patch.object(check.pins, 'entry', return_value=pin):
                with self.assertRaisesRegex(check.pins.PinError, 'take the census again'):
                    check.trace(temp / 'downloads', temp / 'evidence', run=None)

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
            total, available, committed = ci_evidence.memory_status()
            self.assertGreater(total, 0)
            self.assertTrue(ci_evidence.machine()['logical_cpus'])
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
        gib = int(ci_evidence.GIB)
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

    def test_node_installer_installs_the_whole_distribution_and_checks_digests_and_version(self):
        def distribution(path, node=b'#!/bin/sh\n'):
            with tarfile.open(path, 'w:xz') as package:
                link = tarfile.TarInfo('node-v9.9.9-linux-x64/bin/npm')
                link.type, link.linkname = tarfile.SYMTYPE, '../lib/node_modules/npm/bin/npm-cli.js'
                package.addfile(link)
                for name, data in (('node-v9.9.9-linux-x64/bin/node', node),
                                   ('node-v9.9.9-linux-x64/lib/node_modules/npm/bin/npm-cli.js', b'// npm\n')):
                    info = tarfile.TarInfo(name)
                    info.size, info.mode = len(data), 0o755
                    package.addfile(info, io.BytesIO(data))
            return {'version': '9.9.9', 'url': 'https://example.invalid/node', 'file': path.name,
                    'sha256': pins_registry.sha256(path), 'root': 'node-v9.9.9-linux-x64/',
                    'member': 'node-v9.9.9-linux-x64/bin/node',
                    'member_sha256': hashlib.sha256(b'#!/bin/sh\n').hexdigest()}

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pin = distribution(root / 'node.tar.xz')
            with contextlib.redirect_stdout(io.StringIO()):
                node = install_node.install(root / 'node', archive=root / 'node.tar.xz', pin=pin, check=False)
                self.assertEqual(node, (root / 'node/bin/node').resolve())
                self.assertEqual(sorted(p.name for p in (root / 'node').iterdir()), ['bin', 'cerid-node.json', 'lib'])
                self.assertEqual((root / 'node/bin/npm').read_bytes(), b'// npm\n', 'the npm link resolves')
                self.assertEqual(install_node.npm_command(node),
                                 [str(node), str(root.resolve() / 'node/lib/node_modules/npm/bin/npm-cli.js')])
                # A matching stamp is reused.
                (root / 'node/lib/node_modules/npm/bin/npm-cli.js').write_bytes(b'kept')
                install_node.install(root / 'node', archive=root / 'node.tar.xz', pin=pin, check=False)
                self.assertEqual((root / 'node/lib/node_modules/npm/bin/npm-cli.js').read_bytes(), b'kept')
                # bin/node's own digest is checked inside the tree, and the member must be the binary.
                distribution(root / 'other.tar.xz', node=b'#!/bin/sh\nexit 1\n')
                with self.assertRaisesRegex(pins_registry.PinError, 'binary checksum mismatch'):
                    install_node.install(root / 'other', archive=root / 'other.tar.xz', check=False,
                                         pin=dict(pin, sha256=pins_registry.sha256(root / 'other.tar.xz')))
                self.assertFalse((root / 'other').exists() or (root / 'other.partial').exists())
                with self.assertRaisesRegex(pins_registry.PinError, 'is not the binary'):
                    install_node.install(root / 'member', archive=root / 'node.tar.xz', check=False,
                                         pin=dict(pin, member='node-v9.9.9-linux-x64/bin/npm'))
                for reported, accepted in (('v9.9.9\n', True), ('v9.9.10\n', False), ('9.9.9\n', False)):
                    target = root / ('version-%s' % reported.strip())
                    with patch.object(install_node.subprocess, 'run',
                                      return_value=SimpleNamespace(stdout=reported, returncode=0)):
                        if accepted:
                            install_node.install(target, archive=root / 'node.tar.xz', pin=pin)
                        else:
                            with self.assertRaisesRegex(pins_registry.PinError, 'reports'):
                                install_node.install(target, archive=root / 'node.tar.xz', pin=pin)
                    self.assertEqual(target.exists(), accepted, 'a wrong version leaves nothing installed')
        pin = pins_registry.entry('tools', 'node-linux-x64')
        self.assertEqual(pin['root'], pin['file'].removesuffix('.tar.xz') + '/')
        self.assertEqual(pin['member'], pin['root'] + install_node.NODE)
        self.assertIn('/v%s/' % pin['version'], pin['url'])
        self.assertIn('scripts/install-node.py', check_pins.HELPERS)

    def test_cuda_installer_unpacks_every_component_archive_into_one_prefix(self):
        def component(directory, root, members):
            path = directory / (root + '.tar.xz')
            with tarfile.open(path, 'w:xz') as package:
                for name, data in members.items():
                    info = tarfile.TarInfo(root + '/' + name)
                    info.size, info.mode = len(data), 0o755
                    package.addfile(info, io.BytesIO(data))
            return path.name, {'component': root.split('-')[0], 'version': root.split('-')[-1],
                               'url': 'https://example.invalid/' + path.name, 'sha256': pins_registry.sha256(path),
                               'root': root + '/'}

        def toolkit(directory, extra=None):
            directory.mkdir()
            parts = [component(directory, 'cuda_nvcc-linux-x86_64-9.9.73',
                               {'bin/nvcc': b'#!/bin/sh\n', 'LICENSE': b'EULA', 'include/common.h': b'same'}),
                     component(directory, 'cuda_cudart-linux-x86_64-9.9.29',
                               {'lib/libcudart.so': b'ELF', 'LICENSE': b'EULA, runtime', 'include/common.h': b'same'})]
            if extra is not None:
                parts.append(component(directory, 'libcufft-linux-x86_64-9.9.1', extra))
            return {'version': '9.9.1', 'binary': 'bin/nvcc', 'notices': ['LICENSE'], 'files': dict(parts),
                    'license': 'LicenseRef-NVIDIA-CUDA-Toolkit-EULA'}

        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            root = Path(temp).resolve()
            pin = toolkit(root / 'dl')
            prefix = install_cuda.install(root / 'cuda', root / 'dl', pin=pin, check=False)
            self.assertEqual(prefix, root / 'cuda')
            self.assertEqual((prefix / 'lib/libcudart.so').read_bytes(), b'ELF', 'every archive lands in one prefix')
            self.assertEqual((prefix / 'include/common.h').read_bytes(), b'same', 'an identical path is accepted')
            self.assertFalse((prefix / 'LICENSE').exists(), 'no archive notice is overwritten by another')
            self.assertEqual((prefix / 'licenses/cuda_nvcc-linux-x86_64-9.9.73/LICENSE').read_bytes(), b'EULA')
            self.assertEqual((prefix / 'licenses/cuda_cudart-linux-x86_64-9.9.29/LICENSE').read_bytes(),
                             b'EULA, runtime')
            stamp = json.loads((prefix / install_cuda.STAMP).read_text(encoding='utf-8'))
            self.assertEqual(stamp['sha256'], {name: spec['sha256'] for name, spec in pin['files'].items()})
            # A matching stamp is reused; a changed layout (a new link) is installed again.
            (prefix / 'lib/libcudart.so').write_bytes(b'kept')
            install_cuda.install(root / 'cuda', root / 'dl', pin=pin, check=False)
            self.assertEqual((prefix / 'lib/libcudart.so').read_bytes(), b'kept')
            if not pins_registry.WINDOWS:
                install_cuda.install(root / 'cuda', root / 'dl', pin=dict(pin, links={'lib64': 'lib'}), check=False)
                self.assertTrue((prefix / 'lib64').is_symlink())
                self.assertEqual((prefix / 'lib64/libcudart.so').read_bytes(), b'ELF')
                for links, message in (({'lib64': '../outside'}, 'leaves the tree'),
                                       ({'lib64': 'missing'}, 'names nothing'),
                                       ({'lib': 'include'}, 'would replace')):
                    with self.assertRaisesRegex(pins_registry.PinError, message):
                        install_cuda.install(root / ('bad-' + message.split()[0]), root / 'dl', check=False,
                                             pin=dict(pin, links=links))
            # Two archives placing one path differently refuse the tree, and nothing is left behind.
            clash = toolkit(root / 'clash', extra={'include/common.h': b'different'})
            with self.assertRaisesRegex(pins_registry.PinError, 'include/common.h differs'):
                install_cuda.install(root / 'clashed', root / 'clash', pin=clash, check=False)
            self.assertFalse((root / 'clashed').exists() or (root / 'clashed.partial').exists())
            with self.assertRaisesRegex(pins_registry.PinError, 'not one handed-in archive'):
                pins_registry.install_tree(pin, root / 'other', root / 'dl', 'bin/nvcc', 'stamp.json',
                                           archive=root / 'dl' / next(iter(pin['files'])))
            # nvcc --version names the compiler component's version, which stands for the toolkit's.
            for reported, version in (('Cuda compilation tools, release 9.9, V9.9.73\nBuild cuda_9.9\n', '9.9.1'),
                                      ('Cuda compilation tools, release 9.9, V9.9.70\n', '9.9.70'),
                                      ('no version\n', None)):
                with patch.object(install_cuda.subprocess, 'run',
                                  return_value=SimpleNamespace(stdout=reported, returncode=0)):
                    self.assertEqual(install_cuda.installed_version(prefix / 'bin/nvcc', pin), version)
            with self.assertRaisesRegex(pins_registry.PinError, 'exactly one cuda_nvcc'):
                install_cuda.compiler_version(dict(pin, files={}))
        pin = pins_registry.entry('tools', install_cuda.PIN_NAME)
        self.assertEqual(pin['binary'], install_cuda.NVCC)
        for name, spec in pin['files'].items():
            self.assertEqual(spec['root'], name.removesuffix('.tar.xz') + '/')
            self.assertTrue(spec['url'].endswith('/' + name) and spec['version'] in name)
        self.assertIn('cuda_nvcc-linux-x86_64-%s-archive.tar.xz' % install_cuda.compiler_version(pin), pin['files'])
        self.assertIn('scripts/install-cuda.py', check_pins.HELPERS)

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

    def test_apt_installer_bounds_every_attempt_retries_and_names_each_outcome(self):
        def runner(codes):
            commands = []

            def run(command):
                commands.append(command)
                return codes.pop(0)
            return commands, run

        commands, run = runner([124, 0, 0])
        with contextlib.redirect_stdout(io.StringIO()):
            install_apt.install(['gdb'], update=True, update_seconds=7, install_seconds=9, run=run)
        self.assertEqual([c[:4] for c in commands], [['sudo', 'timeout', '--kill-after=30', '7']] * 2
                         + [['sudo', 'timeout', '--kill-after=30', '9']])
        self.assertEqual(commands[0][4:6] + commands[0][-1:], ['apt-get', '-q', 'update'])
        for option in ('Acquire::Retries=3', 'Acquire::http::Timeout=60', 'Acquire::https::Timeout=60',
                       'DPkg::Lock::Timeout=120'):
            self.assertIn(option, commands[0])
            self.assertIn(option, commands[2])
        self.assertEqual(commands[2][-2:], ['install', 'gdb'])

        commands, run = runner([100, 0, 0])
        with contextlib.redirect_stdout(io.StringIO()):
            install_apt.install(['gdb'], update=False, run=run)
        self.assertEqual(commands[1][4:], ['dpkg', '--configure', '-a'],
                         'a failed install finishes dpkg before it is retried')
        self.assertEqual(commands[2][-2:], ['install', 'gdb'])

        commands, run = runner([124, 137, 100])
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(install_apt.AptError, r'update failed on all 3 attempts \(timed out after 180 s; '
                                        r'timed out after 180 s; exited 100\)'):
                install_apt.install([], update=True, run=run)
        self.assertEqual(len(commands), 3)
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                install_apt.main([])

        workflows = sorted((ROOT / '.github/workflows').glob('*.yml'))
        self.assertTrue(workflows)
        for workflow in workflows:
            text = workflow.read_text(encoding='utf-8')
            self.assertNotRegex(text, r'\bapt-get\b', '%s calls apt-get outside the bounded helper' % workflow.name)
            for step in re.split(r'\n\s+- name: ', text):
                if 'install-apt-packages.py' in step:
                    self.assertRegex(step, r'\n\s+timeout-minutes: \d+\n', '%s: an apt step without a step timeout'
                                     % workflow.name)

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
            # CMake reports the real path: on Windows its case can differ from TEMP's spelling (C:\Windows\TEMP).
            tools = Path(temp).resolve() / 'tools'
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
            # A checkout on a native Linux disk (the benchmark machine) is its own source: no mirror to sync or check,
            # while a /mnt checkout, or an explicit CRD_MIRROR_SOURCE, keeps the mirror and its refusal.
            self.assertIn('if [[ $CHECKOUT != /mnt/* && -z ${CRD_MIRROR_SOURCE:-} ]]; then\n    NATIVE=1', wsl_lane)
            self.assertIn('if [[ $NATIVE == 1 ]]; then\n    MIRROR=$CHECKOUT\nelse\n    MIRROR=${CRD_SOURCE_MIRROR:-',
                          wsl_lane)
            mirror_function = wsl_lane[wsl_lane.index('\nmirror() {\n'):]
            mirror_function = mirror_function[:mirror_function.index('\n}\n')]
            self.assertLess(mirror_function.index('return 0'), mirror_function.index('source-mirror.py'))
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
            self.assertGreaterEqual(os.stat(mirror / 'engine/a.cpp').st_mtime_ns,
                                    os.stat(source / 'engine/a.cpp').st_mtime_ns)
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
            # An edit made while a build ran: the build compiled the old bytes after the edit's time, so the copy
            # must be newer than its object; a sync that finds the bytes equal leaves the time alone.
            edited = source / 'engine/c.cpp'
            edited.write_text('int c = 3;\n', encoding='utf-8')
            edit_time = os.stat(edited).st_mtime_ns - 60 * 10**9
            os.utime(edited, ns=(edit_time, edit_time))
            built = edit_time + 4 * 10**9  # the object of the old bytes
            code, output = self.mirror('sync', source, mirror)
            self.assertEqual(code, 0, output)
            self.assertIn('1 copied', output)
            copied = os.stat(mirror / 'engine/c.cpp').st_mtime_ns
            self.assertGreater(copied, built)
            code, output = self.mirror('sync', source, mirror)
            self.assertEqual(code, 0, output)
            self.assertIn('0 copied', output)
            self.assertEqual(os.stat(mirror / 'engine/c.cpp').st_mtime_ns, copied)
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

    def test_a_linked_worktree_mirrors_as_a_standalone_repository_of_its_own_branch(self):
        with tempfile.TemporaryDirectory() as temp:
            main, worktree, mirror = Path(temp) / 'main', Path(temp) / 'track', Path(temp) / 'mirror'
            main.mkdir()
            self.git(main, 'init', '-q', '-b', 'main')
            self.git(main, 'config', 'core.autocrlf', 'false')
            (main / 'a.cpp').write_text('int a = 1;\n', encoding='utf-8')
            self.git(main, 'add', '.')
            self.git(main, 'commit', '-q', '-m', 'base')
            self.git(main, 'worktree', 'add', '-q', '-b', 'track', str(worktree))
            (worktree / 'b.cpp').write_text('int b;\n', encoding='utf-8')
            self.git(worktree, 'add', '.')
            self.git(worktree, 'commit', '-q', '-m', 'track work')
            (main / 'a.cpp').write_text('int a = 2;\n', encoding='utf-8')
            self.git(main, 'add', '.')  # the main checkout's index differs from the worktree's
            self.assertTrue((worktree / '.git').is_file(), 'a linked worktree names its git directory in a file')

            code, output = self.mirror('sync', worktree, mirror)
            self.assertEqual(code, 0, output)
            self.assertTrue((mirror / '.git').is_dir())
            for link in ('commondir', 'gitdir', 'worktrees'):
                self.assertFalse((mirror / '.git' / link).exists(), link)
            head = self.git(worktree, 'rev-parse', 'HEAD').stdout
            self.assertEqual(self.git(mirror, 'rev-parse', 'HEAD').stdout, head)
            self.assertEqual(self.git(mirror, 'symbolic-ref', 'HEAD').stdout.strip(), b'refs/heads/track')
            self.assertEqual(self.git(mirror, 'status', '--porcelain').stdout, b'', 'the worktree index, not main\'s')
            self.assertEqual(source_mirror.mirror_files(mirror), ['a.cpp', 'b.cpp'])
            self.assertEqual(self.mirror('check', worktree, mirror, '--full')[0], 0)

            (worktree / 'b.cpp').write_text('int b = 3;\n', encoding='utf-8')
            self.assertEqual(self.mirror('check', worktree, mirror)[0], source_mirror.STALE)
            code, output = self.mirror('sync', worktree, mirror)
            self.assertEqual(code, 0, output)
            self.assertIn('incremental sync', output)
            self.git(worktree, 'commit', '-q', '-am', 'second')
            self.assertIn('HEAD differs', self.mirror('check', worktree, mirror)[1])
            self.assertEqual(self.mirror('sync', worktree, mirror)[0], 0)
            self.assertEqual(self.git(mirror, 'rev-parse', 'HEAD').stdout,
                             self.git(worktree, 'rev-parse', 'HEAD').stdout)
            self.assertEqual(self.mirror('check', worktree, mirror, '--full')[0], 0)

            # The same mirror then synced from the main checkout drops the worktree's HEAD and index.
            self.assertEqual(self.mirror('sync', main, mirror)[0], 0)
            self.assertEqual(self.git(mirror, 'symbolic-ref', 'HEAD').stdout.strip(), b'refs/heads/main')
            self.assertEqual(self.mirror('check', main, mirror, '--full')[0], 0)


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
            # The core-only layout of these cases: no pinned dependency and no assets unless a case names them.
            def layout(modules=('core',), **options):
                options.setdefault('dependencies', {})
                options.setdefault('assets', {})
                return package_consumer.check_layout(prefix, modules=modules, **options)

            problems, libraries = layout(modules=('core',), assets={})
            self.assertEqual((problems, libraries), ([], ['crd-core.lib']))
            # Debug information as declared: pdb needs each archive's compile PDB beside it, the others none.
            problems, _ = layout(modules=('core',), debug_information='pdb', assets={})
            self.assertEqual(problems, ['debug information is pdb but crd-core.pdb is not installed beside its archive'])
            (prefix / 'lib/crd-core.pdb').write_bytes(b'pdb')
            self.assertEqual(layout(modules=('core',), debug_information='pdb', assets={})[0], [])
            self.assertEqual(layout(modules=('core',), debug_information='dwarf', assets={})[0],
                             ['debug information is dwarf but PDBs are installed: crd-core.pdb'])
            (prefix / 'lib/crd-core.pdb').unlink()
            # A pinned dependency (zstd behind compression): exactly one of its archive names, its compile PDB under
            # pdb (named after its CMake target), and neither when the package does not link it.
            zstd = {'zstd': ('libzstd_static', ('libzstd.a', 'zstd_static.lib'))}
            aws_lc = {'aws-lc-ssl': ('ssl', ('libssl.a', 'ssl.lib')),
                      'aws-lc-crypto': ('crypto', ('libcrypto.a', 'crypto.lib'))}
            self.assertEqual(package_consumer.DEPENDENCIES, {**zstd, **aws_lc})
            self.assertIn('crd_package_dependency(libzstd_static NAME zstd)',
                          (ROOT / 'engine/foundation/compression/CMakeLists.txt').read_text(encoding='utf-8'))
            crypto_cmake = (ROOT / 'engine/foundation/crypto/CMakeLists.txt').read_text(encoding='utf-8')
            for target, name in (('ssl', 'aws-lc-ssl'), ('crypto', 'aws-lc-crypto')):
                self.assertIn(f'crd_package_dependency({target} NAME {name})', crypto_cmake)
            self.assertIn("'CRD_MODULES': 'perf,math,crypto'", (ROOT / 'scripts/test-package-consumer.py').read_text(
                encoding='utf-8'))
            self.assertEqual(layout(dependencies=zstd)[0], ['the zstd dependency needs exactly one of libzstd.a, '
                                                            'zstd_static.lib under lib/, found []'])
            (prefix / 'lib/zstd_static.lib').write_bytes(b'!<arch>\n')
            self.assertEqual(layout(dependencies=zstd), ([], ['crd-core.lib', 'zstd_static.lib']))
            self.assertEqual(layout(dependencies=zstd, debug_information='pdb')[0],
                             ['debug information is pdb but crd-core.pdb is not installed beside its archive',
                              'debug information is pdb but libzstd_static.pdb is not installed beside its archive'])
            (prefix / 'lib/crd-core.pdb').write_bytes(b'pdb')
            (prefix / 'lib/libzstd_static.pdb').write_bytes(b'pdb')
            self.assertEqual(layout(dependencies=zstd, debug_information='pdb')[0], [])
            self.assertEqual(layout(debug_information='pdb')[0], ['unexpected archive under lib/: zstd_static.lib',
                                                                  'unexpected PDB under lib/: libzstd_static.pdb'])
            (prefix / 'lib/libzstd.a').write_bytes(b'!<arch>\n')
            self.assertIn("the zstd dependency needs exactly one of libzstd.a, zstd_static.lib under lib/, found "
                          "['libzstd.a', 'zstd_static.lib']", layout(dependencies=zstd)[0])
            for name in ('crd-core.pdb', 'libzstd_static.pdb', 'zstd_static.lib', 'libzstd.a'):
                (prefix / 'lib' / name).unlink()
            # One archive per packaged module and no other; the assets byte for byte.
            (prefix / 'lib/libcrd-extra.a').write_bytes(b'!<arch>\n')
            source_assets = Path(temp) / 'assets'
            source_assets.mkdir()
            (source_assets / 'a.host.toml').write_text('kind = "diagnostic-host"\n', encoding='utf-8')
            (source_assets / 'b.flight.toml').write_text('kind = "flight-policy"\n', encoding='utf-8')
            (prefix / 'share/cerid/diag').mkdir(parents=True)
            (prefix / 'share/cerid/diag/a.host.toml').write_text('kind = "edited"\n', encoding='utf-8')
            (prefix / 'share/cerid/diag/stray.toml').write_text('', encoding='utf-8')
            problems, _ = layout(modules=('core', 'perf'), assets={'diag': source_assets})
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
            problems, _ = layout(modules=('core',), assets={})
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
            engine = (('the engine archives', package_consumer.ENGINE_ONLY_NAME),)

            def problem(executable, debug_information, probes=engine):
                return package_consumer.debug_information_problem(executable, debug_information, probes)
            self.assertIsNone(problem(program, 'dwarf'))
            program.write_bytes(image.replace(package_consumer.ENGINE_ONLY_NAME, b'x' * 21))
            self.assertIn('no DWARF of the engine archives', problem(program, 'dwarf'))
            windows = Path(temp) / 'diag-consumer.exe'
            windows.write_bytes(b'MZ')
            self.assertIsNone(package_consumer.elf_sections(windows, ('.debug_str',)))
            self.assertIn('not a 64-bit ELF', problem(windows, 'dwarf'))
            self.assertIn('no program database', problem(windows, 'pdb'))
            windows.with_suffix('.pdb').write_bytes(b'Microsoft C/C++ MSF 7.00' + package_consumer.ENGINE_ONLY_NAME)
            self.assertIsNone(problem(windows, 'pdb'))
            windows.with_suffix('.pdb').write_bytes(b'Microsoft C/C++ MSF 7.00')
            self.assertIn('LNK4099', problem(windows, 'pdb'))
            self.assertIsNone(problem(windows, 'none'))
            # Every probe of the package: the engine's and each compile PDB of AWS-LC's two archives, each named when
            # its library's debug information is missing.
            probes = package_consumer.DEBUG_PROBES
            self.assertEqual(probes[0], engine[0])
            names = [name for _, name in probes]
            windows.with_suffix('.pdb').write_bytes(b'Microsoft C/C++ MSF 7.00' + b'\0'.join(names))
            self.assertIsNone(problem(windows, 'pdb', probes))
            windows.with_suffix('.pdb').write_bytes(b'Microsoft C/C++ MSF 7.00' + b'\0'.join(names[:2]))
            self.assertEqual(problem(windows, 'pdb', probes),
                             "the debug information of AWS-LC's libcrypto (jitterentropy.h, rand_data) did not reach "
                             'diag-consumer.pdb (no lag_prediction_success_run); a compile PDB is missing beside its '
                             'archive (LNK4099)')
            # No probe is a name the application or the engine's own sources could carry.
            engine_sources = '\n'.join(path.read_text(encoding='utf-8', errors='replace')
                                       for path in (ROOT / 'engine').rglob('*') if path.suffix in ('.hpp', '.cpp'))
            consumer_sources = '\n'.join(path.read_text(encoding='utf-8')
                                         for path in package_consumer.DIAG_CONSUMER.glob('*.?pp'))
            for _, name in probes[1:]:
                self.assertNotIn(name.decode(), engine_sources)
                self.assertNotIn(name.decode(), consumer_sources)

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
                                   'example_capture.cpp', 'example_codecs.cpp', 'example_crypto.cpp'})
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
        # The flight record (tag 6, kept in step with the engine's enum) stored compressed, decoded to its size.
        tags = (ROOT / 'engine/foundation/perf/include/crd/perf/bundle.hpp').read_text(encoding='utf-8')
        self.assertEqual(re.search(r'FlightRecord\s*=\s*(\d+)', tags).group(1), str(pc.FLIGHT_RECORD_TAG))
        record = {'kind': 'section', 'tag': pc.FLIGHT_RECORD_TAG, 'compressed': True, 'stored_length': 300,
                  'original_length': 900, 'payload_length': 900}
        frames += [record]
        kept = ['flight-1.cdb', 'flight-2.cdb']
        self.assertEqual(pc.capture_problems(output, page([manual, window] + frames), kept), [])
        raw = dict(record, compressed=False, stored_length=900)
        self.assertIn(f'the flight-record section is not stored compressed and decoded to its size: {raw}',
                      pc.capture_problems(output, page([manual, window] + frames[:-1] + [raw]), kept))
        self.assertIn('the bundle has no flight-record section',
                      pc.capture_problems(output, page([manual, window] + frames[:-1]), kept))
        self.assertIn("the capture directory keeps ['flight-0.cdb', 'flight-1.cdb', 'flight-2.cdb']; the newest 2 "
                      "are ['flight-1.cdb', 'flight-2.cdb']",
                      pc.capture_problems(output, page([manual, window] + frames), ['flight-0.cdb'] + kept))
        gap = frames[:3] + frames[4:]  # the record stays at the end
        self.assertEqual(pc.capture_problems(output, page([manual, window] + gap), kept),
                         ['the frames after the request do not follow frame 100: [102, 103, 104]'])
        one_fewer = output.replace('CONSUMER_COMMAND command=flight.trigger status=ok {}\n', '', 1)
        self.assertIn("flight.trigger answered ['ok', 'ok']",
                      pc.capture_problems(one_fewer, page([manual, window] + frames), kept))

        # Codecs: both round trips, each codec smaller than the text.
        codecs = 'CONSUMER_CODECS bytes=7000 deflate=900 zstd=400 deflate_round_trip=1 zstd_round_trip=1'
        self.assertEqual(pc.codecs_problems(codecs), [])
        self.assertEqual(pc.codecs_problems(codecs.replace('zstd_round_trip=1', 'zstd_round_trip=0')),
                         ["CONSUMER_CODECS zstd_round_trip: expected '1', got '0'"])
        self.assertEqual(pc.codecs_problems(codecs.replace('deflate=900', 'deflate=7000')),
                         ['CONSUMER_CODECS deflate: 7000 bytes from 7000'])
        self.assertEqual(pc.codecs_problems(''), ['CONSUMER_CODECS: line missing'])
        # Crypto: the hybrid group, ML-DSA-65 both ways, zstd, a register AEAD, and every verdict.
        crypto = ('CONSUMER_CRYPTO group=0x0101 aead=0x0501 signature=0x0302 certificate_compression=3 handshake=1 '
                  'mutual=1 round_trip=1 closed=1')
        self.assertEqual(pc.crypto_problems(crypto), [])
        self.assertEqual(pc.crypto_problems(crypto.replace('aead=0x0501', 'aead=0x0503')), [])
        self.assertEqual(pc.crypto_problems(crypto.replace('group=0x0101', 'group=0x0201')),
                         ["CONSUMER_CRYPTO group: expected '0x0101', got '0x0201'"])
        self.assertEqual(pc.crypto_problems(crypto.replace('certificate_compression=3', 'certificate_compression=0')),
                         ["CONSUMER_CRYPTO certificate_compression: expected '3', got '0'"])
        self.assertEqual(pc.crypto_problems(crypto.replace('mutual=1', 'mutual=0')),
                         ["CONSUMER_CRYPTO mutual: expected '1', got '0'"])
        self.assertEqual(pc.crypto_problems(crypto.replace('aead=0x0501', 'aead=0x0000')),
                         ['CONSUMER_CRYPTO aead: 0x0000 is not a TLS 1.3 AEAD of the register'])
        self.assertEqual(pc.crypto_problems(crypto + '\n' + crypto), ['CONSUMER_CRYPTO: line missing'])
        self.assertEqual(pc.crypto_problems(''), ['CONSUMER_CRYPTO: line missing'])

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
                                                       'ceridc-diag', 'ckir-binary', 'ckir-text', 'comply-rules',
                                                       'comply-sanctions', 'crypto-capability', 'crypto-mls',
                                                       'crypto-mls-commit', 'crypto-mls-group', 'crypto-mls-message',
                                                       'crypto-mls-tree',
                                                       'crypto-policy', 'crypto-provider', 'crypto-quic', 'crypto-tls',
                                                       'memory-allocators',
                                                       'net-negotiation', 'perf-assets',
                                                       'perf-bundle', 'perf-capture', 'perf-flight',
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

    def test_generated_cuda_kernels_under_bench_must_be_listed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'scripts').mkdir()
            (root / 'tests').mkdir()
            (root / 'bench' / 'gpu').mkdir(parents=True)
            generator = root / 'tests' / 'test_emit.cpp'
            generator.write_text('// the hidden emit case\n', encoding='utf-8')
            kernel = root / 'bench' / 'gpu' / 'k_gen.cu'
            kernel.write_text('// GENERATED by test_emit.cpp [.emit] -- a kernel.\n__global__ void k() {}\n',
                              encoding='utf-8')
            manifest = root / 'scripts' / 'generated-sources.json'
            manifest.write_text(json.dumps({'schema': provenance.SCHEMA, 'entries': []}), encoding='utf-8')
            errors = provenance.check(root, manifest)
            self.assertTrue(any('bench/gpu/k_gen.cu' in error and 'generation marker' in error for error in errors),
                            errors)
            entry = {'path': 'bench/gpu/k_gen.cu', 'generator': 'tests/test_emit.cpp', 'status': 'reproducible',
                     'sha256': provenance.sha256(kernel)}
            manifest.write_text(json.dumps({'schema': provenance.SCHEMA, 'entries': [entry]}), encoding='utf-8')
            self.assertEqual(provenance.check(root, manifest), [])

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
                '    auto* s = static_cast<float*>(crd::jobs::frame_try_alloc(64U, 16U));\n'
                '    s[0] = 1.0F;\n'
                '}\n')
        self.assertEqual([3, 6, 8, 10], allocation_results.unchecked_calls(text))

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
                '    void* const stage = crd::jobs::frame_try_alloc(bytes, 64U);\n'
                '    if (stage == nullptr)\n'
                '    {\n'
                '        return;\n'
                '    }\n'
                '}\n'
                '[[nodiscard]] void* frame_try_alloc(crd::usize size, crd::usize alignment) noexcept;\n'
                'void* frame_try_alloc(crd::usize size, crd::usize alignment) noexcept\n')
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


class EnvironmentReads(unittest.TestCase):
    """scripts/check-environment-reads.py: environment variables are read only through crd/core/environment.hpp."""

    def test_every_direct_read_is_reported_by_line(self):
        text = ('const char* a = std::getenv("A");\n'
                'const char* b = getenv("B");\n'
                '(void)_dupenv_s(&p, &n, "C");\n'
                'const DWORD d = ::GetEnvironmentVariableA("D", buf, 32U);\n'
                'char** e = environ;\n'
                'const auto ok = crd::read_environment_text("F", text);\n')
        self.assertEqual([1, 2, 3, 4, 5], environment_reads.raw_reads(text))

    def test_comments_strings_and_other_names_are_not_reads(self):
        text = ('// std::getenv("A") in a line comment\n'
                '/* getenv("B") in a block\n   _dupenv_s(&p, &n, "C") */\n'
                'const char* m = "std::getenv(\\"D\\") in a string";\n'
                'const char* r = R"x(GetEnvironmentVariableA("E"))x";\n'
                'bool environment_set(const char* name);\n'
                'my_getenv_wrapper(name);\n'
                'config.environ = 1;\n')
        self.assertEqual([], environment_reads.raw_reads(text))

    def test_the_tree_check_allows_only_the_reader_and_fails_an_empty_one(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            (root / 'scripts').mkdir()
            (root / 'scripts' / 'source-routes.json').write_text('{"roots": ["engine", "tests"]}', encoding='utf-8')
            reader = root / environment_reads.READER
            reader.parent.mkdir(parents=True)
            reader.write_text('// nothing yet\n', encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(1, environment_reads.main(['check', str(root)]))
            self.assertIn('nothing was checked', out.getvalue())
            reader.write_text('const char* v = std::getenv(name);\n', encoding='utf-8')
            source = root / 'tests' / 'a' / 'test_a.cpp'
            source.parent.mkdir(parents=True)
            source.write_text('#include <x>\nconst char* v = std::getenv("X");\n', encoding='utf-8')
            in_reader, problems = environment_reads.check(root)
            self.assertEqual(1, in_reader)
            self.assertEqual(1, len(problems))
            self.assertIn('tests/a/test_a.cpp: reads the environment directly at line(s) 2', problems[0])
            source.write_text('crd::containers::String v;\n(void)crd::read_environment_text("X", v);\n',
                              encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(0, environment_reads.main(['check', str(root)]))
            self.assertIn('every environment read goes through', out.getvalue())


class FrameResets(unittest.TestCase):
    """scripts/check-frame-resets.py: no engine library resets the crd::jobs frame arenas."""

    def test_calls_are_reported_by_line_and_comments_and_strings_are_not(self):
        text = ('crd::jobs::frame_reset();\n'
                '// crd::jobs::frame_reset(); in a comment\n'
                'const char* s = "frame_reset()";\n'
                'jobs::frame_reset ();\n'
                'my_frame_reset_count(1);\n')
        self.assertEqual([1, 4], frame_resets.resets(text))

    def test_the_tree_check_allows_only_the_jobs_module_and_fails_an_empty_one(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            jobs = root / frame_resets.OWNER / 'src' / 'jobs.cpp'
            jobs.parent.mkdir(parents=True)
            jobs.write_text('// nothing yet\n', encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(1, frame_resets.main(['check', str(root)]))
            self.assertIn('nothing was checked', out.getvalue())
            jobs.write_text('void frame_reset()\n{\n}\n', encoding='utf-8')
            library = root / 'engine' / 'numerics' / 'a' / 'a.hpp'
            library.parent.mkdir(parents=True)
            library.write_text('#pragma once\ninline void f()\n{\n    crd::jobs::frame_reset();\n}\n', encoding='utf-8')
            test = root / 'tests' / 'a' / 'test_a.cpp'
            test.parent.mkdir(parents=True)
            test.write_text('crd::jobs::frame_reset();\n', encoding='utf-8')
            in_owner, problems = frame_resets.check(root)
            self.assertEqual(1, in_owner)
            self.assertEqual(1, len(problems))
            self.assertIn('engine/numerics/a/a.hpp: calls crd::jobs::frame_reset at line(s) 4', problems[0])
            library.write_text('#pragma once\ninline void f()\n{\n}\n', encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(0, frame_resets.main(['check', str(root)]))
            self.assertIn('no engine library resets', out.getvalue())


class CapabilityOwners(unittest.TestCase):
    """scripts/check-capability-owners.py: one owning module for every engine capability (OWN.1, OWN.2)."""

    CMAKE = ('crd_module(engine/foundation/containers engine/containers DEPENDS core TESTS foundation/containers)\n'
             'crd_module(engine/foundation/core engine/core TESTS foundation/core)\n'
             'crd_module(engine/assets/a engine/a DEPENDS containers\n    TESTS assets/a)\n'
             'crd_module(tools/shader-cook NAME shader_cook HOST EXECUTABLES shader_cook)\n'
             'crd_module(engine/assets/shader-cook NAME engine-shader-cook DEPENDS core)\n')
    ROADMAP = '| 1 | <a id="slice-own.1"></a>**OWN.1** | Open |\n| 2 | <a id="slice-ship.0"></a>**SHIP.0** | Open |\n'
    PRIME = [{'regex': r'\b1099511628211[uUlL]*\b', 'what': 'the FNV-1a 64-bit prime'}]

    def fixture(self, root, capabilities):
        root = Path(root)
        (root / 'cmake').mkdir()
        (root / 'docs').mkdir()
        (root / 'CMakeLists.txt').write_text(self.CMAKE, encoding='utf-8')
        (root / 'docs/ROADMAP.md').write_text(self.ROADMAP, encoding='utf-8')
        api = root / 'engine/foundation/containers/include/crd/containers/hash.hpp'
        api.parent.mkdir(parents=True)
        api.write_text('constexpr u64 prime = 1099511628211ULL;\n', encoding='utf-8')
        (root / 'engine/assets/a/src').mkdir(parents=True)
        self.write_registry(root, capabilities)
        return root

    @staticmethod
    def write_registry(root, capabilities):
        registry = {'schema': capability_owners.SCHEMA, 'capabilities': capabilities}
        (Path(root) / capability_owners.REGISTRY).write_text(json.dumps(registry), encoding='utf-8')

    def fnv(self, **extra):
        capability = {'id': 'hash.fnv1a', 'summary': 'FNV-1a', 'owner': 'containers',
                      'api': ['engine/foundation/containers/include/crd/containers/hash.hpp'],
                      'signatures': self.PRIME}
        capability.update(extra)
        return capability

    def test_module_names_are_the_ones_depends_uses_and_tools_are_not_engine_modules(self):
        modules = capability_owners.registered_modules(self.CMAKE)
        self.assertEqual({'containers': 'engine/foundation/containers/', 'core': 'engine/foundation/core/',
                          'a': 'engine/assets/a/', 'engine-shader-cook': 'engine/assets/shader-cook/'}, modules)

    def test_a_planted_copy_outside_the_owner_fails_and_the_owner_and_comments_do_not(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.fixture(scratch, [self.fnv()])
            copy = root / 'engine/assets/a/src/a.cpp'
            copy.write_text('// FNV prime 1099511628211ULL in a comment\nconst char* s = "1099511628211";\n',
                            encoding='utf-8')
            self.assertEqual([], capability_owners.check(root)[2])
            copy.write_text('u64 h(u64 x)\n{\n    return (x ^ 7U) * 1099511628211ULL;\n}\n', encoding='utf-8')
            problems = capability_owners.check(root)[2]
            self.assertEqual(1, len(problems))
            self.assertIn('engine/assets/a/src/a.cpp: hash.fnv1a implements it outside its owner containers at '
                          'line(s) 3 (the FNV-1a 64-bit prime)', problems[0])
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(1, capability_owners.main(['check', '--root', str(root), '--audit']))
            self.assertIn('engine/assets/a/src/a.cpp: 3 [UNLISTED]', out.getvalue())

    def test_host_tools_and_the_sandbox_are_scanned_and_tests_and_benches_are_not(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.fixture(scratch, [self.fnv()])
            for name in ('tools/asset_cooker/src/hash.cpp', 'sandbox/src/demo.cpp', 'tests/a/test_a.cpp',
                         'runtime/examples/bench_a.cpp'):
                (root / name).parent.mkdir(parents=True)
                (root / name).write_text('u64 k = 1099511628211ULL;\n', encoding='utf-8')
            flagged = sorted(p.split(':')[0] for p in capability_owners.check(root)[2])
            self.assertEqual(['sandbox/src/demo.cpp', 'tools/asset_cooker/src/hash.cpp'], flagged)

    def test_pending_and_allowed_files_pass_until_they_go_stale(self):
        with tempfile.TemporaryDirectory() as scratch:
            pending = [{'path': 'engine/assets/a/src/a.cpp', 'row': 'OWN.1'}]
            root = self.fixture(scratch, [self.fnv(pending=pending)])
            copy = root / 'engine/assets/a/src/a.cpp'
            copy.write_text('u64 k = 1099511628211ULL;\n', encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(0, capability_owners.main(['check', '--root', str(root)]))
            self.assertIn('1 capabilities, each with one owner; 1 file(s) pending', out.getvalue())
            copy.write_text('u64 k = crd::containers::fnv1a_64(p, n);\n', encoding='utf-8')
            self.assertEqual(['hash.fnv1a: engine/assets/a/src/a.cpp lists engine/assets/a/src/a.cpp as pending, '
                              'but no signature matches there any more; remove the entry'],
                             capability_owners.check(root)[2])
            allowed = [{'path': 'engine/assets/a/src/a.cpp', 'why': 'emits the constant into generated source'}]
            self.write_registry(root, [self.fnv(allowed=allowed)])
            copy.write_text('emit(1099511628211ULL);\n', encoding='utf-8')
            self.assertEqual([], capability_owners.check(root)[2])

    def test_the_registry_refuses_unknown_owners_rows_api_files_and_bad_shapes(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.fixture(scratch, [])
            (root / 'engine/assets/a/src/a.cpp').write_text('int a;\n', encoding='utf-8')

            def problems(*capabilities):
                self.write_registry(root, list(capabilities))
                return capability_owners.check(root)[2]
            self.assertEqual(['capabilities must be a non-empty list'], problems())
            self.assertIn('hash.fnv1a: id registered twice', problems(self.fnv(), self.fnv()))
            self.assertIn("hash.fnv1a: owner 'hesap' is not a crd_module registration (planned owners say "
                          '"status": "planned")', problems(self.fnv(owner='hesap')))
            self.assertIn("hash.fnv1a: API file 'engine/assets/a/a.hpp' is not inside the owner "
                          'engine/foundation/containers/', problems(self.fnv(api=['engine/assets/a/a.hpp'])))
            missing = 'engine/foundation/containers/include/crd/containers/gone.hpp'
            self.assertIn('hash.fnv1a: API file %s does not exist' % missing, problems(self.fnv(api=[missing])))
            self.assertIn('hash.fnv1a: a landed owner lists its public API files', problems(self.fnv(api=[])))
            broken = self.fnv(signatures=[{'regex': '(', 'what': 'broken'}])
            self.assertTrue(any("signature 'broken' does not compile" in p for p in problems(broken)))
            self.assertIn('Hash: id must be lower-case words joined by "." or "-"', problems(self.fnv(id='Hash')))
            self.assertIn('hash.fnv1a: unknown keys extra', problems(self.fnv(extra=1)))
            self.assertIn('hash.fnv1a: a pending entry is {"path", "row"}', problems(self.fnv(pending=[{'path': 'x'}])))
            self.assertIn("hash.fnv1a: pending file engine/assets/a/src/a.cpp names 'NOPE', not a ROADMAP row",
                          problems(self.fnv(pending=[{'path': 'engine/assets/a/src/a.cpp', 'row': 'NOPE'}])))
            self.assertIn('hash.fnv1a: allowed file engine/assets/a/src/a.cpp needs the reason',
                          problems(self.fnv(allowed=[{'path': 'engine/assets/a/src/a.cpp', 'why': ' '}])))
            self.assertIn("hash.fnv1a: consumer row 'X.1' is not a ROADMAP row", problems(self.fnv(consumers=['X.1'])))
            ship = {'id': 'publishing.shipping', 'summary': 'ship', 'owner': 'ship', 'status': 'planned', 'row': 'SHIP.0'}
            self.assertEqual([], problems(self.fnv(), ship))
            self.assertIn('publishing.shipping: a planned owner names the ROADMAP row that creates it',
                          problems(dict(ship, row='SHIP.9')))
            self.assertIn("publishing.shipping: owner 'a' is registered now; mark it landed with its API files",
                          problems(dict(ship, owner='a')))
            self.assertIn('publishing.shipping: a planned owner has no directory for signatures to protect yet',
                          problems(dict(ship, signatures=self.PRIME)))
            assigned = {'id': 'random.seed', 'summary': 'seeds', 'owner': 'core', 'status': 'assigned',
                        'row': 'OWN.1', 'signatures': [{'regex': 'splitmix', 'what': 'SplitMix64'}]}
            self.assertEqual([], problems(assigned))
            self.assertIn('random.seed: an assigned capability has no API yet; mark it landed with its API files',
                          problems(dict(assigned, api=self.fnv()['api'])))
            self.assertIn('random.seed: status must be "landed", "assigned" or "planned"',
                          problems(dict(assigned, status='moving')))

    def test_an_assigned_owner_guards_its_signatures_before_its_api_lands(self):
        with tempfile.TemporaryDirectory() as scratch:
            assigned = {'id': 'random.seed', 'summary': 'seeds', 'owner': 'core', 'status': 'assigned',
                        'row': 'OWN.1', 'signatures': [{'regex': r'\b0xBF58476D1CE4E5B9\b', 'what': 'SplitMix64'}]}
            root = self.fixture(scratch, [assigned])
            (root / 'engine/assets/a/src/a.cpp').write_text('z *= 0xBF58476D1CE4E5B9;\n', encoding='utf-8')
            problems = capability_owners.check(root)[2]
            self.assertEqual(1, len(problems))
            self.assertIn('random.seed implements it outside its owner core', problems[0])

    def test_this_checkout_has_one_owner_for_every_capability(self):
        _, hits, problems = capability_owners.check(ROOT)
        self.assertEqual([], problems)
        self.assertTrue(hits, 'the signatures match the files the audit recorded as pending')


class PeerHeaders(unittest.TestCase):
    """scripts/check-peer-headers.py: only the board peer libraries include a peer header library."""

    def test_header_library_includes_are_found_and_declarations_comments_and_strings_are_not(self):
        text = ('#include <Eigen/Dense>\n'
                '#include "boost/math/special_functions/erf.hpp"\n'
                '#  include <unsupported/Eigen/FFT>\n'
                '#include <pocketfft_hdronly.h>\n'
                '#include <hptt.h>\n'
                '#include <hwy/highway.h>\n'
                '// #include <boost/version.hpp>\n'
                '/* #include <Eigen/Core> */\n'
                'const char* s = "#include <boost/x.hpp>";\n'
                '#include <cblas.h>\n'
                '#include <fftw3.h>\n'
                '#include <mkl_dfti.h>\n'
                '#include "boost_peer.hpp"\n'
                '#include "pocketfft_peer.hpp"\n'
                '#include "hptt_peer.hpp"\n'
                '#include "highway_peer.hpp"\n'
                '#include <cub/cub.cuh>\n')
        self.assertEqual([(1, 'Eigen', 'Eigen/Dense'), (2, 'Boost', 'boost/math/special_functions/erf.hpp'),
                          (3, 'Eigen', 'unsupported/Eigen/FFT'), (4, 'pocketfft', 'pocketfft_hdronly.h'),
                          (5, 'HPTT', 'hptt.h'), (6, 'Highway', 'hwy/highway.h')],
                         peer_headers.peer_includes(text))

    def test_the_tree_check_allows_only_the_peers_and_fails_an_empty_one(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            peer = root / peer_headers.PEERS / 'boost_peer.cpp'
            peer.parent.mkdir(parents=True)
            peer.write_text('#include "boost_peer.hpp"\n', encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(1, peer_headers.main(['check', str(root)]))
            self.assertIn('nothing was checked', out.getvalue())
            peer.write_text('#include "boost_peer.hpp"\n#include <boost/math/tools/config.hpp>\n', encoding='utf-8')
            bench = root / 'runtime' / 'examples' / 'bench_a.cpp'
            bench.write_text('#include "boost_peer.hpp"\n#include <Eigen/Dense>\n', encoding='utf-8')
            test = root / 'tests' / 'a' / 'test_a.cpp'
            test.parent.mkdir(parents=True)
            test.write_text('#include <boost/version.hpp>\n', encoding='utf-8')
            in_peers, problems = peer_headers.check(root)
            self.assertEqual(1, in_peers)
            self.assertEqual(2, len(problems))
            self.assertIn('runtime/examples/bench_a.cpp:2: includes Eigen (Eigen/Dense)', problems[0])
            self.assertIn('tests/a/test_a.cpp:1: includes Boost (boost/version.hpp)', problems[1])
            bench.write_text('#include "boost_peer.hpp"\n', encoding='utf-8')
            test.write_text('// #include <boost/version.hpp>\n', encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO) as out:
                self.assertEqual(0, peer_headers.main(['check', str(root)]))
            self.assertIn('only runtime/examples/peers/ includes a peer header library', out.getvalue())

    def test_this_checkout_passes(self):
        in_peers, problems = peer_headers.check(ROOT)
        self.assertEqual([], problems)
        self.assertGreater(in_peers, 0)


class FftLevelPlans(unittest.TestCase):
    """scripts/gen_fft_level_plans.py: a board's logs become a type's data file, and its rows the header's."""

    PROBE = {'machine': 'probe machine', 'commit': 'probe', 'board': 'docs/bench/probe.md'}

    @staticmethod
    def write_board(root, planned_2k, canonical_2k, level='sse2'):
        """Two passes of one level: 1K and 2K, the planned arm first, each pass beating it with four-step at 2K."""
        for p, (ip4, four) in enumerate(((0.010, 0.008), (0.011, 0.009)), start=1):
            (root / f'pass{p}-level-{level}.txt').write_text(f'cpu detected=avx512 selected={level}\n',
                                                             encoding='utf-8')
            lines = [f'fft kernel level: {level}',
                     f'1024      | ip4             |     {ip4:.3f} | 4.1e-16', 'n=1024 canonical: ip4',
                     f'2048      | {planned_2k:<15} |     {ip4:.3f} | 4.0e-16',
                     f'2048      | four-step       |     {four:.3f} | 8.7e-16',
                     f'n=2048 canonical: {canonical_2k[p - 1]}']
            if planned_2k != 'ip4':
                lines.append('2048      | ip4             |     0.016 | 4.0e-16')
            (root / f'pass{p}-f64-{level}.log').write_text('\n'.join(lines) + '\n', encoding='utf-8')

    def test_the_printed_canonical_forward_is_each_row_s_reference(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_board(root, 'standalone-hier', ('ip4', 'ip4'))
            levels = fft_plans.import_board(root, 'f64', self.PROBE)
            self.assertEqual(['sse2'], [entry['level'] for entry in levels])
            self.assertEqual({'1024': 'ip4', '2048': 'ip4'}, levels[0]['canonical'])
            self.assertEqual(('probe machine', 'probe', 'docs/bench/probe.md'),
                             tuple(levels[0][key] for key in fft_plans.PROVENANCE))
            data = fft_plans.merge(None, levels, 'f64')
            self.assertEqual(('bench_fft_vs_refs', 8), (data['program'], data['element_bytes']))
            rows = fft_plans.rows_of(root, data)
            self.assertIn('FftAlgorithm::Ip4}', rows[0][0])
            self.assertIn('{8, crd::CpuLevel::Sse2, 11, FftAlgorithm::FourStep}', rows[1][0])
            self.assertTrue(rows[1][1].endswith('; ip4 0.016'), rows[1][1])

    def test_logs_that_disagree_on_the_canonical_forward_are_refused(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_board(root, 'ip4', ('ip4', 'stockham'))
            with self.assertRaisesRegex(SystemExit, '2048 canonical stockham, another log ip4'):
                fft_plans.import_board(root, 'f64', self.PROBE)

    def test_an_import_replaces_its_levels_and_keeps_another_machine_s(self):
        with tempfile.TemporaryDirectory() as scratch:
            first, second = Path(scratch) / 'first', Path(scratch) / 'second'
            first.mkdir()
            second.mkdir()
            self.write_board(first, 'standalone-hier', ('ip4', 'ip4'))
            self.write_board(first, 'ip4', ('ip4', 'ip4'), level='avx512')
            data = fft_plans.merge(None, fft_plans.import_board(first, 'f64', self.PROBE), 'f64')
            # A later board of avx512 alone, on another machine whose binary's canonical 2K is four-step.
            self.write_board(second, 'standalone-hier', ('four-step', 'four-step'), level='avx512')
            other = {'machine': 'other machine', 'commit': 'later', 'board': 'docs/bench/later.md'}
            data = fft_plans.merge(data, fft_plans.import_board(second, 'f64', other), 'f64')
            self.assertEqual(['sse2', 'avx512'], [entry['level'] for entry in data['levels']])
            self.assertEqual('probe machine', data['levels'][0]['machine'])
            self.assertEqual(('other machine', 'later', {'1024': 'ip4', '2048': 'four-step'}),
                             tuple(data['levels'][1][key] for key in ('machine', 'commit', 'canonical')))
            rows = fft_plans.rows_of(second, data)
            # sse2's note compares with its own board's canonical ip4; avx512's winner four-step is its canonical.
            self.assertTrue(rows[1][1].endswith('; ip4 0.016'), rows[1][1])
            self.assertIn('{8, crd::CpuLevel::Avx512, 11, FftAlgorithm::FourStep}', rows[3][0])
            self.assertNotIn(';', rows[3][1])
            lines = fft_plans.sources(ROOT / 'docs' / 'bench' / 'probe.json', data)
            self.assertEqual(['//   sse2: at probe on probe machine', '//   (docs/bench/probe.md).',
                              '//   avx512: at later on other machine', '//   (docs/bench/later.md).'], lines[1:])

    def test_a_judged_size_takes_the_paired_board_s_arm_until_its_level_is_imported_again(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_board(root, 'ip4', ('ip4', 'ip4'))
            data = fft_plans.merge(None, fft_plans.import_board(root, 'f64', self.PROBE), 'f64')
            # The passes put four-step ahead at 2K; the paired board kept the canonical ip4 there.
            self.assertIn('FftAlgorithm::FourStep}', fft_plans.rows_of(root, data)[1][0])
            data = fft_plans.judge(data, ['sse2:2048=ip4'], 'docs/bench/paired.md#board-2')
            self.assertEqual({'2048': {'arm': 'ip4', 'board': 'docs/bench/paired.md#board-2'}},
                             data['levels'][0]['judged'])
            rows = fft_plans.rows_of(root, data)
            self.assertIn('{8, crd::CpuLevel::Sse2, 11, FftAlgorithm::Ip4}', rows[1][0])
            self.assertEqual('0.0105, judged', rows[1][1])
            self.assertEqual(['//   sse2\'s judged sizes:', '//   (docs/bench/paired.md#board-2).'],
                             fft_plans.sources(ROOT / 'docs' / 'bench' / 'probe.json', data)[-2:])
            for text, refusal in (('sse2:2048=stockham', 'did not time stockham at 2048'),
                                  ('avx2:2048=ip4', 'no avx2 level'), ('sse2-2048-ip4', 'not LEVEL:N=ARM')):
                with self.assertRaisesRegex(SystemExit, refusal):
                    fft_plans.judge(data, [text], 'docs/bench/paired.md')
            # A new board of the level replaces its entry, and the judgement made of the old one with it.
            data = fft_plans.merge(data, fft_plans.import_board(root, 'f64', self.PROBE), 'f64')
            self.assertNotIn('judged', data['levels'][0])

    def test_a_level_without_its_board_s_provenance_is_refused(self):
        data = fft_plans.load('f32')
        del data['levels'][0]['machine']
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / 'f32.json'
            path.write_text(json.dumps(data), encoding='utf-8')
            with patch.dict(fft_plans.TYPES, {'f32': (path, 'bench_fft_f32', 4)}):
                with self.assertRaisesRegex(SystemExit, 'level portable has no machine'):
                    fft_plans.load('f32')

    def test_this_checkout_s_header_matches_its_data_files(self):
        with patch('sys.argv', ['gen_fft_level_plans.py', '--check']):
            self.assertEqual(0, fft_plans.main())


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
        self.assertNotIn('tests/geometry/geometry-primitives/reference/shewchuk-predicates.c', files)  # vendored
        self.assertNotIn('engine/execution/ceir/generated/crd/ceir/gen/arith_ops.cpp', files)  # generated
        self.assertIn('bench/gpu-compute/mlp_backward_bench.cu', files)  # CUDA sources are held too
        self.assertIn('bench/gpu-fft/spectrum_multiply.cuh', files)
        self.assertNotIn('bench/gpu-compute/ckir_onesweep_gen.cu', files)  # CKIR-emitted

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
        shard_one = ' && (!matrix.shard || matrix.shard == 1)\n'
        windows_gate = "if: (matrix.preset == 'win-debug' || matrix.preset == 'win-asan')" + shard_one
        self.assertIn(windows_gate, workflow)
        dropped = workflow.replace(windows_gate, "if: (matrix.preset == 'win-debug')" + shard_one)
        problems = ci_tiers.validate(tiers, presets, dropped)
        self.assertEqual(["step %r must be gated on matrix.preset == 'win-asan'" % step], problems)
        widened = workflow.replace(windows_gate, windows_gate.replace("'win-asan')", "'win-asan' || "
                                                                      "matrix.preset == 'win-release')"))
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

    def test_pool_workers_end_with_a_parent_that_crashed(self):
        # A parent that ends without shutting its pool down: its workers exit instead of holding ctest's pipe.
        parent = ('import concurrent.futures, importlib.util, json, os, sys\n'
                  'spec = importlib.util.spec_from_file_location("guard", sys.argv[1])\n'
                  'guard = sys.modules["guard"] = importlib.util.module_from_spec(spec)\n'
                  'spec.loader.exec_module(guard)\n'
                  'if __name__ == "__main__":\n'
                  '    pool = concurrent.futures.ProcessPoolExecutor(2, initializer=guard._exit_with_parent)\n'
                  '    print(json.dumps(sorted({pool.submit(os.getpid).result() for _ in range(8)})), flush=True)\n'
                  '    os._exit(3)\n')

        def alive(pid):
            if os.name != 'nt':
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return False
                return True
            import ctypes
            kernel = ctypes.WinDLL('kernel32')
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
            if not handle:
                return False
            running = kernel.WaitForSingleObject(handle, 0) == 0x102  # WAIT_TIMEOUT
            kernel.CloseHandle(handle)
            return running

        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp, 'parent.py')
            script.write_text(parent, encoding='utf-8')
            out = Path(tmp, 'pids.json')
            with out.open('w', encoding='utf-8') as stream:  # a file, not a pipe the workers could hold open
                done = subprocess.run([sys.executable, str(script), str(ROOT / 'scripts/check-isa-isolation.py')],
                                      stdout=stream, timeout=120, check=False)
            self.assertEqual(3, done.returncode)
            workers = json.loads(out.read_text(encoding='utf-8'))
            self.assertTrue(workers)
            deadline = time.monotonic() + 30.0
            try:
                while workers and time.monotonic() < deadline:
                    workers = [pid for pid in workers if alive(pid)]
                    time.sleep(0.1)
                self.assertEqual([], workers)
            finally:
                for pid in workers:
                    with contextlib.suppress(OSError):
                        os.kill(pid, 9)

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

    def test_a_provider_is_left_out_of_elf_images(self):
        # nm over a provider archive: member headers, defined functions (global and local) and data.
        listing = ('\naesni-x86_64.S.o:\n0000000000000000 T aes_hw_encrypt\n0000000000000040 t _aesni_ctr32_6x\n'
                   '\ncpu.c.o:\n0000000000000000 B OPENSSL_ia32cap_P\n')
        with patch.object(isa_isolation, '_run', return_value=listing), \
                patch.object(isa_isolation.os.path, 'exists', return_value=True):
            provided = isa_isolation.provider_functions({'nm': 'nm'}, {'/b/libcrypto.a': 'crypto'})
        self.assertEqual(frozenset({'aes_hw_encrypt', '_aesni_ctr32_6x'}), provided)
        self.assertEqual(frozenset(), isa_isolation.provider_functions({'nm': 'nm'}, {'/b/missing.a': 'crypto'}))
        with patch.object(isa_isolation, '_run', return_value='\n'), \
                patch.object(isa_isolation.os.path, 'exists', return_value=True):
            with self.assertRaisesRegex(isa_isolation.GuardError, 'listed no function'):
                isa_isolation.provider_functions({'nm': 'nm'}, {'/b/libcrypto.a': 'crypto'})
        # The provider's above-floor routine is left out; Cerid's own is still judged.
        elf = ('0000000000000000 <aes_hw_encrypt>:\n   0:\tc5 fc 58 c1          \tvaddps ymm0,ymm0,ymm1\n'
               '0000000000000010 <crd::g()>:\n  10:\tc5 fc 58 c1          \tvaddps ymm0,ymm0,ymm1\n')
        exceptions = isa_isolation.Exceptions()
        with patch.object(isa_isolation, '_stream', return_value=[elf]):
            failures = isa_isolation.judge_elf_image({'objdump': 'objdump'}, 't', 'x64', exceptions, provided)
            unprovided = isa_isolation.judge_elf_image({'objdump': 'objdump'}, 't', 'x64', exceptions)
        self.assertEqual(1, len(failures))
        self.assertIn('crd::g()', failures[0])
        self.assertEqual(2, len(unprovided))
        # The manifest names a provider's archive on its own line.
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / 'm.manifest'
            manifest.write_text('kernel\tavx2\t\tx.a\tk.o\ngeneral\tcrd-x\tx.a\tg.o\n'
                                'provider\tcrypto\t/b/libcrypto.a\n', encoding='utf-8')
            providers = isa_isolation.read_manifest(str(manifest))[3]
        self.assertEqual({os.path.normpath('/b/libcrypto.a'): 'crypto'}, providers)

    def test_general_symbols_hold_no_level(self):
        judge = isa_isolation.judge_general_symbols
        self.assertEqual([], judge([('crd::math::general::Vec4f::load(float const*)', True), ('?f@@YAXXZ', True)]))
        self.assertEqual(1, len(judge([('?add@avx2@math@crd@@YA?AUVec8f@123@U4123@0@Z', True)])))
        self.assertEqual(1, len(judge([('crd::math::sse2::exp(double)', False)])))
        self.assertEqual(1, len(judge([('crd::hesap::tensor::kTensorKernelsAvx2', True)])))
        # MSVC ASan's descriptor of a global is judged as that global: a literal's passes, a level table's fails.
        self.assertEqual([], judge([('L__asan_global_??_C@_04FNEJMADF@sse2@', False)]))
        self.assertEqual(1, len(judge([('L__asan_global_?kTensorKernelsAvx2@tensor@hesap@crd@@3UTensorKernels@123@B',
                                        False)])))

    def test_kernel_symbols_stay_in_their_level(self):
        allowed = self.exceptions.kernel_symbols
        judge = isa_isolation.judge_kernel_symbols
        good = [('?kTensorKernelsAvx2@tensor@hesap@crd@@3UTensorKernels@123@B', True),
                ('__real@3ff0000000000000', True),
                ('??$bit_cast@VVec8f@avx2@math@crd@@@std@@YA?AVVec8f@avx2@math@crd@@AEBM@Z', True),
                ('crd::hesap::tensor::avx2::walk(int)', True), ('local_helper', False),
                ('___asan_globals_registered', True), ('L__asan_global_??_C@_04FNEJMADF@sse2@', True),
                ('L__asan_global_?table@avx2@tensor@hesap@crd@@3HB', True),
                ('L__asan_global_?kTensorKernelsAvx2@tensor@hesap@crd@@3UTensorKernels@123@B', True)]
        self.assertEqual(([], 1), judge('avx2', good, allowed))
        failures, _ = judge('avx2', [('L__asan_global_?table@@3HB', True)], allowed)
        self.assertEqual(1, len(failures))
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
        clmul = ('f', 0, 'pclmulqdq', 'pclmulqdq xmm0, xmm1, 0')
        self.assertEqual([], judge('sse42_pclmulqdq', [clmul, ('f', 4, 'crc32', 'crc32 rax, rcx')], True))
        self.assertEqual(1, len(judge('sse42', [clmul], True)))
        self.assertEqual([], judge('avx512_vpclmulqdq', [('f', 0, 'EVEX', 'vpclmulqdq zmm0')], True))
        self.assertEqual([], judge('sse2', [('f', 0, 'cmpxchg16b', 'lock cmpxchg16b')], True))
        self.assertEqual(1, len(judge('sse2', [('f', 0, 'cmpxchg16b', 'lock cmpxchg16b')], False)))
        self.assertEqual([], judge('portable', [], True))

    def test_arm64_kernel_code_stays_within_its_feature(self):
        judge = isa_isolation.judge_arm64_kernel_code
        crc = ('f', 0, 'crc32cx', 'crc32cx w0, w0, x1')
        pmull = ('f', 4, 'pmull', 'pmull v0.1q, v1.1d, v2.1d')
        pmull2 = ('f', 8, 'pmull2', 'pmull2 v0.1q, v1.2d, v2.2d')
        self.assertEqual([], judge('neon_crc32', [crc, ('f', 4, 'crc32b', 'crc32b w0, w0, w1')]))
        self.assertEqual([], judge('neon_pmull', [crc, pmull, pmull2]))
        self.assertEqual(1, len(judge('neon_crc32', [crc, pmull])))
        self.assertEqual(1, len(judge('neon', [crc])))
        self.assertEqual(1, len(judge('neon', [pmull2])))
        self.assertEqual(1, len(judge('portable', [crc])))
        self.assertEqual(1, len(judge('neon_pmull', [('f', 0, 'SVE/SME', 'ld1w {z0.s}, p0/z, [x0]')])))
        self.assertEqual([], judge('neon', []))

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
        self.assertTrue(allows('roundpd', '__sse4_floor2@@16', 'MSVCRT:svml_dfloor2_sse4.obj', set()))
        self.assertTrue(allows('EVEX', '__avx512_exp8@@64', 'MSVCRT:svml_dexp8_avx512.obj', set()))
        self.assertFalse(allows('roundpd', '__vdecl_floor2', 'MSVCRT:svml_dfloor2_dispatch.obj', set()))

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
        self.assertEqual('crc32cx', above('crc32cx', 'w0, w0, x1'))
        # PMULL to 128 bits is FEAT_PMULL in objdump's and dumpbin's spelling; the 8-bit polynomial form is base NEON.
        self.assertEqual('pmull', above('pmull', 'v0.1q, v1.1d, v2.1d'))
        self.assertEqual('pmull2', above('pmull2', 'v16.1Q,v16.2D,v17.2D'))
        self.assertIsNone(above('pmull', 'v0.8h, v1.8b, v2.8b'))
        self.assertIsNone(above('pmull2', 'v0.8h, v1.16b, v2.16b'))
        self.assertIsNone(above('.word', '0x05a0e000'))
        self.assertEqual('SVE/SME', above('ld1d', '{z1.d}, p7/z, [x0, z2.d, lsl #3]'))
        self.assertEqual('SVE/SME', above('zero', '{za}'))
        # A branch target's symbol is no operand, in objdump's annotation or dumpbin's decorated name.
        self.assertIsNone(above('bl', '11c <(anonymous namespace)::Rng::p3(float)>'))
        self.assertIsNone(above('b.eq', '1ec <(anonymous namespace)::Rng::p3(float)+0xd0>'))
        self.assertIsNone(above('bl', '?p3@Rng@?A0x1c2d@@QEAAMM@Z'))
        self.assertIsNone(above('ldr', 'x0, [x1, #8]  // 1f0 <z1>'))
        dump = ('0000000000000000 <crd::f()>:\n   0:\ta5404000 \tld1w\t{z0.s}, p0/z, [x0]\n'
                '   4:\t8b020020 \tadd\tx0, x1, x2\n'
                '0000000000000008 <void (anonymous namespace)::g<int>(int)>:\n'
                '   8:\t04a00000 \tadd\tz0.s, z0.s, z0.s\n')
        # objdump -C names a function whole: its return type and (anonymous namespace) are part of it.
        self.assertEqual([('crd::f()', 'SVE/SME'), ('void (anonymous namespace)::g<int>(int)', 'SVE/SME')],
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
        self.assertEqual((9, 32), isa_isolation.arm64_adr(0x10000109))
        self.assertEqual((9, -4), isa_isolation.arm64_adr(0x10FFFFE9))
        self.assertIsNone(isa_isolation.arm64_adr(0x90000009))
        self.assertEqual(9, isa_isolation.arm64_indexed_load(0x38684928))
        self.assertEqual(9, isa_isolation.arm64_indexed_load(0xB8687928))
        self.assertIsNone(isa_isolation.arm64_indexed_load(0x38284928))
        self.assertIsNone(isa_isolation.arm64_indexed_load(0xF8A87928))
        # A compact switch: the ADR an indexed load reads makes its byte table data to the next function; the second
        # ADR forms the cases' code address, so an SVE word in a case is still found, and so is one past the table.
        dump = ('Dump of file a.obj\n\nFile Type: COFF OBJECT\n\n?n@@YAPEBDH@Z (char const * __cdecl n(int)):\n'
                '  0000000000000000: 7100111F  cmp         w8,#4\n'
                '  0000000000000004: 10000109  adr         x9,0000000000000024\n'
                '  0000000000000008: 38684928  ldrb        w8,[x9,w8 uxtw #0]\n'
                '  000000000000000C: 1000006A  adr         x10,0000000000000018\n'
                '  0000000000000010: 8B280948  add         x8,x10,w8,uxtb #2\n'
                '  0000000000000014: D61F0100  br          x8\n'
                '  0000000000000018: A5404000  ld1w        {z0.s},p0/z,[x0]\n'
                '  000000000000001C: D65F03C0  ret\n'
                '  0000000000000020: D65F03C0  ret\n'
                '  0000000000000024: E4DED8D2  st1h        {z18.s},p6,[x6,z30.s,sxtw]\n'
                '  0000000000000028: C4C4C4C4  ld1h        {z4.d},p1/z,[x6,z4.d]\n'
                '?k@@YAXXZ (void __cdecl k(void)):\n'
                '  000000000000002C: A5404000  ld1w        {z0.s},p0/z,[x0]\n')
        self.assertEqual([('?n@@YAPEBDH@Z', 24), ('?k@@YAXXZ', 44)],
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
        # A provider's archive (AWS-LC behind crd-crypto) is neither Cerid's code nor a judged library.
        self.assertEqual('library', owners.classify('crypto:x86_64-mont5.asm.obj', image)[0])
        providing = isa_isolation.Owners(kernels, generals, {os.path.normpath('/b/_deps/crypto.lib'): 'crypto'})
        self.assertEqual('provider', providing.classify('crypto:x86_64-mont5.asm.obj', image)[0])
        self.assertEqual('provider', providing.classify('t.exe.lto.crypto.libx86_64-mont5.asm.obj', image)[0])
        self.assertEqual('general', providing.classify('crd-hesap-tensor:einsum.cpp.obj', image)[0])
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

    def test_map_code_symbols_lie_inside_their_segments_code(self):
        # link.exe lists an import piece it dropped at a wrapped offset below its segment (the image base); in a
        # segment that holds code it would be a function start inside the PE header.
        text = (' t\n\n Preferred load address is 0000000140000000\n\n'
                ' Start         Length     Name                   Class\n'
                ' 0001:00000000 00000100H .text$mn                CODE\n'
                ' 0001:00000100 00000040H .text$x                 CODE\n'
                ' 0002:00000000 00000080H .rdata                  DATA\n\n'
                '  Address         Publics by Value              Rva+Base               Lib:Object\n\n'
                ' 0000:00000000       __ImageBase                0000000140000000     <linker-defined>\n'
                ' 0001:00000010       main                       0000000140001010 f   a.obj\n'
                ' 0001:00000120       unwind                     0000000140001120 f   a.obj\n'
                ' 0001:00000140       past                       0000000140001140     a.obj\n'
                ' 0001:fffff000       .idata$6                   0000000140000000     vulkan-1:vulkan-1.dll\n'
                ' 0001:fffff00d       .idata$6                   000000014000000d     vulkan-1:vulkan-1.dll\n'
                ' 0002:ffffe000       .idata$6                   0000000140000000     ntdll:ntdll.dll\n')
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 't.map'
            path.write_text(text, encoding='utf-8')
            base, code, entries = isa_isolation.read_map(str(path))
        self.assertEqual(0x140000000, base)
        self.assertEqual([(0x140001010, 'main', 'a.obj'), (0x140001120, 'unwind', 'a.obj')], code)
        self.assertEqual(7, len(entries))

    def test_a_function_start_the_sweep_misread_is_decoded_again(self):
        # A table's last bytes swallow the start of g (48 8B), so the sweep reads a lone C4 there and misses g's real
        # vzeroupper; the decode from g's own start finds it. h, which the sweep reads from its start, keeps its hit.
        sweep = ('  0000000140001000: C3                 ret\n'
                 '  0000000140001001: 00 00 00 00 00 48  add         byte ptr [rax],al\n'
                 '                    8B\n'
                 '  0000000140001008: C4\n'
                 '  0000000140001009: E2 F8              loop        0000000140001003\n'
                 '  000000014000100B: 77 C3              ja          00000001400010D0\n'
                 '  0000000140001010: C5 F8 77           vzeroupper\n')
        again = ('  0000000140001006: 48 8B C4           mov         rax,rsp\n'
                 '  0000000140001009: C5 F8 77           vzeroupper\n'
                 '  000000014000100C: C3                 ret\n')
        code = [(0x140001000, 'f', 'a.obj'), (0x140001006, 'g', 'a.obj'), (0x140001010, 'h', 'a.obj')]
        owners = isa_isolation.Owners({}, {os.path.normpath('/b/t.dir/a.obj'): ('crd-t', '/b/t.exe')})
        ranges = []

        def run(command, empty_ok=False):
            ranges.append(command[-2])
            return again
        with patch.object(isa_isolation, 'read_map', lambda path: (0x140000000, code, [])), \
                patch.object(isa_isolation, 'pe_reader', lambda path: lambda address, size: b''), \
                patch.object(isa_isolation, '_stream', lambda command, label: iter([sweep])), \
                patch.object(isa_isolation, '_run', run):
            failures = isa_isolation.judge_map_image({'dumpbin': 'dumpbin'}, '/b/t.exe', '/b/t.map', owners,
                                                     self.exceptions)
        self.assertEqual(['/range:0x140001006,0x14000100f'], ranges)
        self.assertEqual(['g first at 0x140001009', 'h first at 0x140001010'],
                         sorted(re.sub(r'^.* in (\w+) \(.*, (first at 0x[0-9a-f]+):.*$', r'\1 \2', failure)
                                for failure in failures))

    def test_map_image_leaves_out_a_providers_code(self):
        # AWS-LC's Montgomery multiplication uses MULX behind its own CPU check; in a general object it is a failure.
        code = [(0x140001000, 'mulx4x_internal', 'crypto:x86_64-mont5.asm.obj'), (0x140001010, 'f', 'a.obj')]
        sweep = ('  0000000140001000: C4 E2 BB F6 06     mulx        rax,r8,qword ptr [rsi]\n'
                 '  0000000140001005: C3                 ret\n'
                 '  0000000140001010: C3                 ret\n')
        generals = {os.path.normpath('/b/t.dir/a.obj'): ('crd-t', '/b/t.exe')}

        def judge(owners):
            with patch.object(isa_isolation, 'read_map', lambda path: (0x140000000, code, [])), \
                    patch.object(isa_isolation, 'pe_reader', lambda path: lambda address, size: b''), \
                    patch.object(isa_isolation, '_stream', lambda command, label: iter([sweep])):
                return isa_isolation.judge_map_image({'dumpbin': 'dumpbin'}, '/b/t.exe', '/b/t.map', owners,
                                                     self.exceptions)
        failures = judge(isa_isolation.Owners({}, generals))
        self.assertEqual(1, len(failures), failures)
        self.assertIn('mulx4x_internal (crypto:x86_64-mont5.asm.obj)', failures[0])
        self.assertEqual([], judge(isa_isolation.Owners({}, generals, {os.path.normpath('/b/crypto.lib'): 'crypto'})))

    def test_map_judges_an_asan_descriptor_as_its_global(self):
        # win-asan's cpu_features.obj names its level strings ("sse2") through ASan descriptors of the literals.
        code = [(0x140001000, 'f', 'a.obj')]
        entries = [('0002', 0x140002000, 'L__asan_global_??_C@_04FNEJMADF@sse2@', 'a.obj'),
                   ('0002', 0x140002040, 'L__asan_global_?x@avx2@math@crd@@3HB', 'a.obj')]
        owners = isa_isolation.Owners({}, {os.path.normpath('/b/t.dir/a.obj'): ('crd-t', '/b/t.exe')})
        with patch.object(isa_isolation, 'read_map', lambda path: (0x140000000, code, entries)), \
                patch.object(isa_isolation, 'pe_reader', lambda path: lambda address, size: b''), \
                patch.object(isa_isolation, '_stream',
                             lambda command, label: iter(['  0000000140001000: C3                 ret\n'])):
            failures = isa_isolation.judge_map_image({'dumpbin': 'dumpbin'}, '/b/t.exe', '/b/t.map', owners,
                                                     self.exceptions)
        self.assertEqual(['/b/t.exe: per-level symbol L__asan_global_?x@avx2@math@crd@@3HB comes from general object '
                          'a.obj'], failures)

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


class RegisterFixture:
    """A fixture tree holding a standards and a legal register, shared by the guard's and the review tool's cases."""

    ROADMAP = ('| # | Slice | Status | Scope | Requires | Sources |\n|---|---|---|---|---|---|\n'
               '| 001 | <a id="slice-net-std"></a>**NET-STD** | Partial | x | y | z |\n'
               '| 002 | <a id="slice-net.2"></a>**NET.2** | Open | x | y | z |\n'
               '| 003 | <a id="slice-done"></a>**DONE** | Done | x | y | z |\n')
    DRAFT = {'type': 'draft', 'ref': 'draft-ietf-webtrans-http3', 'revision': '16', 'posted': '2026-07',
             'status': 'active'}

    def entry(self, **changes):
        entry = {'id': 'quic.transport', 'title': 'QUIC', 'kind': 'protocol', 'state': 'planned', 'owner': 'net',
                 'rows': ['NET-STD'], 'spec': {'type': 'rfc', 'ref': 'RFC 9000', 'status': 'current'},
                 'cryptography': [],
                 'codepoints': [{'registry': 'QUIC Versions', 'name': 'v1', 'value': '0x00000001'}],
                 'source': {'ref': 'docs/research.md#n83', 'read': '2026-10-27', 'verified': True},
                 'review': '2026-12-15', 'evidence': [], 'deprecation': None}
        entry.update(changes)
        return entry

    def schedule(self, **changes):
        """A review schedule due on 2026-12-15: 25 days after IETF 127, the first event after the full research."""
        schedule = {'cadence': 'after each IETF meeting', 'next': '2026-12-15', 'lag_days': 25,
                    'full_every_days': 365,
                    'calendar': [{'event': 'IETF 126', 'day': '2026-07-24', 'scan': 'light', 'verified': True},
                                 {'event': 'IETF 127', 'day': '2026-11-20', 'scan': 'light', 'verified': False}],
                    'runs': [{'day': '2026-10-27', 'scan': 'full', 'ref': 'docs/research.md#n83', 'triggers': []}]}
        schedule.update(changes)
        return schedule

    def register(self, entries, **changes):
        register = {'schema': registers.SCHEMA, 'domain': 'standards', 'about': 'fixture',
                    'snapshot': {'date': '2026-10-31', 'method': 'fixture'},
                    'review': self.schedule(),
                    'governed_rows': ['NET-STD'], 'entries': entries,
                    'checklist': [{'section': '2.1', 'item': 'Algorithm identifiers', 'mechanism': 'the register',
                                   'rows': ['NET-STD'], 'evidence': []}]}
        register.update(changes)
        return register

    KVKK = 'https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=6698&MevzuatTur=1&MevzuatTertip=5'
    READ = {'version': 'Consolidated text of 2024-03-12', 'read': '2026-10-30', 'sha256': 'ab' * 32}

    def legal_entry(self, **changes):
        """A pinned Turkish privacy obligation on a planned crash-report flow, read from no primary source yet."""
        entry = {'id': 'tr.kvkk.processing', 'title': 'KVKK processing conditions', 'kind': 'obligation',
                 'topic': 'privacy', 'state': 'planned', 'owner': 'comply', 'rows': ['NET.2'],
                 'instrument': {'name': 'Law No. 6698', 'article': 'Articles 4-7', 'type': 'law', 'jurisdiction': 'TR',
                                'primary': self.KVKK, 'consolidated': None},
                 'obligation': 'Process personal data lawfully.', 'applies_to': ['flow.crash-reports'],
                 'check': {'ref': 'docs/ROADMAP.md#slice-net.2', 'name': 'data inventory CTest', 'state': 'planned'},
                 'effective': '2016-04-07', 'sunset': None, 'counsel': {'needed': True, 'reviewed': None},
                 'source': {'ref': 'docs/research.md#n83', 'read': '2026-10-27', 'verified': False},
                 'review': '2027-10-25', 'evidence': [], 'deprecation': None}
        entry.update(changes)
        return entry

    def instrument(self, **changes):
        return dict(self.legal_entry()['instrument'], **changes)

    def scope(self, **changes):
        scope = {'id': 'flow.crash-reports', 'kind': 'flow', 'title': 'Crash reports leaving a machine',
                 'state': 'planned', 'rows': ['NET.2'], 'evidence': []}
        scope.update(changes)
        return scope

    def legal_register(self, entries=None, **changes):
        """A legal register due on 2027-10-25 (25 days after its quarterly review), so every case's day is before it."""
        quarterly = [{'event': 'Quarterly review', 'day': '2027-09-30', 'scan': 'light', 'verified': True}]
        register = {'schema': registers.SCHEMA, 'domain': 'legal', 'about': 'fixture',
                    'snapshot': {'date': '2026-10-31', 'method': 'fixture'},
                    'review': self.schedule(next='2027-10-25', calendar=quarterly),
                    'governed_rows': ['NET.2'],
                    'jurisdictions': {'TR': {'name': 'Türkiye', 'hosts': ['mevzuat.gov.tr']},
                                      'EU': {'name': 'European Union', 'hosts': ['eur-lex.europa.eu']},
                                      'PLATFORM': {'name': 'Platforms', 'hosts': ['developer.apple.com']}},
                    'scopes': [self.scope()], 'entries': [self.legal_entry()] if entries is None else entries}
        register.update(changes)
        return register

    def write_tree(self, root, register, legal=None):
        for directory in ('docs', 'cmake', 'tests'):
            (root / directory).mkdir(exist_ok=True)
        (root / 'docs' / 'ROADMAP.md').write_text(self.ROADMAP, encoding='utf-8')
        (root / 'docs' / 'research.md').write_text('<a id="n83"></a>N83. QUIC WG documents\n', encoding='utf-8')
        (root / 'tests' / 'test_quic.cpp').write_text('// evidence\n', encoding='utf-8')
        (root / 'CMakeLists.txt').write_text('crd_module(engine/foundation/core engine/core)\n', encoding='utf-8')
        (root / 'cmake' / 'standards-register.json').write_text(json.dumps(register), encoding='utf-8')
        legal = legal or self.legal_register()
        if 'rule_generations' not in legal:
            legal = dict(legal, rule_generations=self.reviewed(root, legal))
        if 'counsel_reviews' not in legal:
            legal = dict(legal, counsel_reviews=self.counsel_reviews(legal))
        if 'sanctions_lists' not in legal:
            legal = dict(legal, sanctions_lists={'about': 'fixture', 'lists': {}, 'pending': {},
                                                 'embargoes': {'about': 'fixture', 'generations': {}, 'reasons': {}}})
        (root / 'cmake' / 'legal-register.json').write_text(json.dumps(legal), encoding='utf-8')
        capabilities = {'capabilities': [{'id': 'network.transport', 'owner': 'net', 'status': 'planned'},
                                         {'id': 'compliance.obligations', 'owner': 'comply', 'status': 'planned'}]}
        (root / 'cmake' / 'capability-owners.json').write_text(json.dumps(capabilities), encoding='utf-8')

    @staticmethod
    def ask(items):
        return [{target: item_id, 'question': f'Is {item_id} right?'} for target, item_id in items]

    @classmethod
    def counsel_reviews(cls, legal):
        """The reviews of a consistent register: one answered review per answer day its items record, and one open
        review asking about every outstanding item."""
        states = registers.counsel_states(legal)
        days = sorted({counsel['reviewed'] for counsel in states.values() if isinstance(counsel.get('reviewed'), str)})
        reviews = [{'id': f'counsel.answered-{day}', 'title': 'Answered', 'opened': '2026-10-27', 'answered': day,
                    'questions': cls.ask(sorted(item for item, counsel in states.items()
                                                if counsel.get('reviewed') == day))} for day in days]
        asked = sorted(item for item, counsel in states.items() if registers.outstanding(counsel))
        if asked:
            reviews.append({'id': 'counsel.open', 'title': 'Open', 'opened': '2026-10-31', 'answered': None,
                            'questions': cls.ask(asked)})
        return reviews

    @staticmethod
    def reviewed(root, legal):
        """Generation 1 of every jurisdiction with rules, written into the tree and recorded as reviewed (a register
        too malformed to generate from records nothing; the entry errors name it)."""
        try:
            texts = comply_rules.outputs(legal, {code: 1 for code in comply_rules.rules_by_jurisdiction(legal)})
        except (comply_rules.RuleError, KeyError, TypeError):
            return {}
        records = {}
        for relative, text in texts.items():
            (root / relative).parent.mkdir(parents=True, exist_ok=True)
            (root / relative).write_bytes(text.encode('utf-8'))
            code = text.split('jurisdiction = "', 1)[1].split('"', 1)[0]
            records[code] = {'generation': 1, 'reviewed': '2026-10-31', 'sha256': comply_rules.digest(text)}
        return records

    def guard(self, root, today):
        with patch('sys.stdout', new_callable=io.StringIO) as out:
            code = registers.main(['--root', str(root), '--today', today])
        return code, out.getvalue()

    def run_guard(self, register, today='2026-11-01', legal=None):
        """(exit code, output) of the guard over a fixture tree holding these registers."""
        with tempfile.TemporaryDirectory() as scratch:
            self.write_tree(Path(scratch), register, legal)
            return self.guard(Path(scratch), today)

    def assert_fails(self, register, message, today='2026-11-01', legal=None):
        code, output = self.run_guard(register, today, legal)
        self.assertEqual(1, code, output)
        self.assertIn(message, output)

    def run_legal(self, legal, today='2026-11-01'):
        return self.run_guard(self.register([self.entry()]), today, legal)

    def assert_legal_fails(self, legal, message, today='2026-11-01'):
        self.assert_fails(self.register([self.entry()]), message, today, legal)


class Registers(RegisterFixture, unittest.TestCase):
    """scripts/check-registers.py: the NET-STD standards register fails on expired pinned drafts, obsoleted pinned
    RFCs, overdue reviews, a review schedule that is not the computed one, unknown owners and rows, uncovered governed
    rows and unproven implemented entries."""

    def test_a_complete_register_passes_and_names_the_first_expiry(self):
        webtransport = self.entry(id='webtransport.h3', codepoints=[], spec=self.DRAFT)
        code, output = self.run_guard(self.register([self.entry(), webtransport]))
        self.assertEqual(0, code, output)
        self.assertIn('2 entries, 2 pinned', output)
        self.assertIn('webtransport.h3 on 2027-01-02', output)

    def test_a_pinned_draft_expires_185_days_from_the_start_of_its_posted_period(self):
        later = self.schedule(next='2027-06-01', calendar=[{'event': 'IETF 128', 'day': '2027-05-07', 'scan': 'light',
                                                            'verified': False}])
        register = self.register([self.entry(spec=self.DRAFT, review='2027-06-01')], review=later)
        self.assertEqual(0, self.run_guard(register, today='2027-01-01')[0])
        self.assert_fails(register, 'expired on 2027-01-02', today='2027-01-02')
        exact = self.register([self.entry(spec=dict(self.DRAFT, posted='2026-07-20'), review='2027-06-01')],
                              review=later)
        self.assertEqual(0, self.run_guard(exact, today='2027-01-20')[0])
        self.assert_fails(exact, 'expired on 2027-01-21', today='2027-01-21')
        held = self.register([self.entry(spec=dict(self.DRAFT, status='rfc-editor'), review='2027-06-01')],
                             review=later)
        self.assertEqual(0, self.run_guard(held, today='2027-03-01')[0])
        watched = self.register([self.entry(state='watch', spec=dict(self.DRAFT, status='expired'))])
        self.assertEqual(0, self.run_guard(watched)[0])
        self.assert_fails(self.register([self.entry(spec=dict(self.DRAFT, status='replaced'))]),
                          'is replaced in the snapshot')
        unpinnable = {'type': 'draft', 'ref': 'draft-x', 'status': 'active'}
        self.assert_fails(self.register([self.entry(spec=unpinnable)]), 'names its revision and posted date')

    def test_a_pinned_rfc_obsoleted_in_the_snapshot_fails(self):
        obsoleted = {'type': 'rfc', 'ref': 'RFC 8446', 'status': 'obsoleted', 'obsoleted_by': ['RFC 9999']}
        self.assert_fails(self.register([self.entry(spec=obsoleted)]), 'obsoleted by RFC 9999')
        self.assertEqual(0, self.run_guard(self.register([self.entry(state='watch', spec=obsoleted)]))[0])
        self.assert_fails(self.register([self.entry(spec=dict(obsoleted, obsoleted_by=[]))]),
                          'exactly when the status is obsoleted')

    def test_a_paper_is_published_or_withdrawn_and_a_withdrawn_document_is_never_pinned(self):
        paper = {'type': 'paper', 'ref': 'https://doi.org/10.1007/978-3-642-34931-7_28', 'posted': '2012-12',
                 'status': 'published'}
        self.assertEqual(0, self.run_guard(self.register([self.entry(spec=paper)]))[0])
        withdrawn = dict(paper, status='withdrawn')
        self.assertEqual(0, self.run_guard(self.register([self.entry(state='watch', spec=withdrawn)]))[0])
        # A withdrawn document, a paper or a NIST publication, cannot stay pinned.
        for spec in (withdrawn, {'type': 'nist', 'ref': 'FIPS 186-4', 'status': 'withdrawn'}):
            with self.subTest(spec=spec['ref']):
                self.assert_fails(self.register([self.entry(spec=spec)]),
                                  f'pinned {spec["ref"]} is withdrawn in the snapshot')
        self.assert_fails(self.register([self.entry(spec=dict(paper, status='final'))]),
                          "spec.status of a paper must be one of ['published', 'withdrawn']")

    def test_overdue_reviews_and_future_reads_fail(self):
        self.assert_fails(self.register([self.entry(review='2026-10-30')]), 'review was due on 2026-10-30')
        self.assert_fails(self.register([self.entry(review='2027-01-01')]),
                          'the scheduled light review was due on 2026-12-15 (IETF 127 (2026-11-20))',
                          today='2026-12-16')
        future = {'ref': 'docs/research.md#n83', 'read': '2026-11-05', 'verified': True}
        self.assert_fails(self.register([self.entry(source=future)]), 'after the snapshot or today')
        self.assert_fails(self.register([self.entry(review='2026-12')]), 'review must be a YYYY-MM-DD date')

    def test_owners_rows_and_sources_must_exist(self):
        self.assert_fails(self.register([self.entry(owner='nowhere')]), "owner 'nowhere' is neither")
        self.assert_fails(self.register([self.entry(rows=['NET-STD', 'NO-SUCH'])]), "unknown ROADMAP row 'NO-SUCH'")
        self.assertEqual(0, self.run_guard(self.register([self.entry(owner='core')]))[0])
        self.assert_fails(self.register([self.entry()], governed_rows=['NET-STD', 'NET.2']),
                          'governed row NET.2 names no register entry')
        anchorless = {'ref': 'docs/research.md#n99', 'read': '2026-10-27', 'verified': True}
        self.assert_fails(self.register([self.entry(source=anchorless)]), "has no anchor 'n99'")
        self.assert_fails(self.register([self.entry(), self.entry()]), 'duplicate entry quic.transport')

    def test_tls_codepoints_record_the_iana_recommended_column(self):
        group = {'registry': 'TLS Supported Groups', 'name': 'x25519', 'value': '0x001D', 'recommended': 'Y'}
        self.assertEqual(0, self.run_guard(self.register([self.entry(codepoints=[group])]))[0])
        unrecorded = {key: value for key, value in group.items() if key != 'recommended'}
        self.assertEqual(0, self.run_guard(self.register([self.entry(codepoints=[dict(unrecorded,
                                                                                       verified=False)])]))[0])
        self.assert_fails(self.register([self.entry(codepoints=[unrecorded])]),
                          "the verified codepoint x25519 does not record IANA's Recommended value")
        self.assert_fails(self.register([self.entry(codepoints=[dict(group, recommended='y')])]),
                          "codepoint x25519 has Recommended 'y', not Y, N or D")
        quic = dict(self.entry()['codepoints'][0], recommended='Y')
        self.assert_fails(self.register([self.entry(codepoints=[quic])]),
                          'codepoint v1 records a Recommended value, which the QUIC Versions registry has no column')
        discouraged = dict(group, name='secp224r1', value='21', recommended='D')
        self.assert_fails(self.register([self.entry(codepoints=[discouraged])]),
                          'IANA discourages secp224r1 (Recommended "D"); drop it from the entry or deprecate')
        self.assertEqual(0, self.run_guard(self.register([self.entry(state='watch', codepoints=[discouraged])]))[0])

    def test_finished_rows_require_implemented_entries_with_evidence_and_verified_codepoints(self):
        self.assert_fails(self.register([self.entry(rows=['DONE'])], governed_rows=[]),
                          'every row (DONE) is Done; mark it implemented')
        self.assert_fails(self.register([self.entry(state='implemented')]), 'names its evidence')
        self.assert_fails(self.register([self.entry(state='implemented', evidence=['tests/missing.cpp'])]),
                          "evidence 'tests/missing.cpp' does not exist")
        unverified = [{'registry': 'QUIC Versions', 'name': 'v1', 'value': '0x00000001', 'verified': False}]
        self.assert_fails(self.register([self.entry(state='implemented', evidence=['tests/test_quic.cpp'],
                                                    codepoints=unverified)]), 'unverified codepoint v1')
        proven = self.entry(state='implemented', evidence=['tests/test_quic.cpp'])
        self.assertEqual(0, self.run_guard(self.register([proven]))[0])
        self.assert_fails(self.register([self.entry(state='deprecated')]), 'names its deprecation date')
        done_item = [{'section': '2.1', 'item': 'i', 'mechanism': 'm', 'rows': ['DONE'], 'evidence': []}]
        self.assert_fails(self.register([self.entry()], checklist=done_item), 'checklist 2.1: every row is Done')

    def test_a_wrong_schema_or_an_empty_register_fails(self):
        self.assert_fails(self.register([self.entry()], schema='cerid-register/0'),
                          "schema must be 'cerid-register/1'")
        self.assert_fails(self.register([]), 'entries is a non-empty list')
        self.assert_fails(self.register([self.entry(colour='red')]), "unknown keys ['colour']")
        self.assert_fails(self.register([self.entry()], owners={'net': 'NET-STD'}), "unknown keys ['owners']")

    def test_the_review_schedule_is_the_computed_one(self):
        self.assert_fails(self.register([self.entry()], review=self.schedule(next='2026-12-01')),
                          'review.next 2026-12-01 is not the due day 2026-12-15 (light scan after IETF 127')
        past = [{'event': 'IETF 126', 'day': '2026-07-24', 'scan': 'light', 'verified': True}]
        self.assert_fails(self.register([self.entry()], review=self.schedule(calendar=past, next='2027-10-27')),
                          'names no event after the latest run (2026-10-27)')
        self.assert_fails(self.register([self.entry()], review=self.schedule(lag_days=0)), 'positive number of days')
        self.assert_fails(self.register([self.entry()], review=self.schedule(lag_days=True)), 'positive number')
        self.assert_fails(self.register([self.entry()], review={'cadence': 'c', 'next': '2026-12-15'}),
                          'review has cadence, calendar, full_every_days, lag_days, next, runs')
        self.assert_fails(self.register([self.entry()], review=self.schedule(calendar=[])), 'non-empty lists')
        unordered = list(reversed(self.schedule()['calendar']))
        self.assert_fails(self.register([self.entry()], review=self.schedule(calendar=unordered)),
                          'strictly increasing day order (IETF 126)')
        malformed = [{'event': 'IETF 127', 'day': '2026-11-20', 'scan': 'weekly', 'verified': False}]
        self.assert_fails(self.register([self.entry()], review=self.schedule(calendar=malformed)),
                          'a calendar event has event, day, scan (light or full) and verified')
        full = self.schedule()['runs'][0]
        self.assert_fails(self.register([self.entry()], review=self.schedule(runs=[dict(full, ref='docs/gone.md')])),
                          "run 2026-10-27 ref 'docs/gone.md' does not exist")
        self.assert_fails(self.register([self.entry()], review=self.schedule(runs=[dict(full, scan='light')])),
                          'records the full research the register was assembled from')
        self.assert_fails(self.register([self.entry()], review=self.schedule(runs=[dict(full, day='2026-11-05')])),
                          'the run of 2026-11-05 is after today')
        unknown = self.schedule(runs=[full, dict(full, day='2026-10-30', scan='light', triggers=['nothing'])])
        self.assert_fails(self.register([self.entry()], review=unknown), "unknown triggering entries ['nothing']")
        backwards = self.schedule(runs=[full, dict(full, day='2026-10-20', scan='light')])
        self.assert_fails(self.register([self.entry()], review=backwards), 'runs are recorded in day order')
        self.assert_fails(self.register([self.entry()], review=self.schedule(runs=[dict(full, triggers='x')])),
                          'a run has day, scan (light or full), ref (its report) and triggers')

    def test_review_due_takes_the_first_of_events_triggers_and_the_year(self):
        schedule = self.schedule()
        self.assertEqual((datetime.date(2026, 12, 15), 'light', 'IETF 127 (2026-11-20)'),
                         registers.review_due(schedule))
        full = schedule['runs'][0]
        light = {'day': '2026-12-10', 'scan': 'light', 'ref': 'docs/research.md', 'triggers': []}
        covered = self.schedule(runs=[full, light])
        self.assertEqual((datetime.date(2027, 10, 27), 'full'), registers.review_due(covered)[:2])
        triggered = self.schedule(runs=[full, dict(light, triggers=['quic.transport', 'webtransport.h3'])])
        day, scan, reason = registers.review_due(triggered)
        self.assertEqual((datetime.date(2027, 1, 4), 'full'), (day, scan))
        self.assertIn('quic.transport, webtransport.h3', reason)
        answered = self.schedule(runs=[full, dict(light, triggers=['quic.transport']),
                                       dict(light, day='2026-12-20', scan='full')])
        self.assertEqual(datetime.date(2027, 12, 20), registers.review_due(answered)[0])
        yearly = self.schedule(full_every_days=30)
        self.assertEqual((datetime.date(2026, 11, 26), 'full'), registers.review_due(yearly)[:2])
        later = triggered['calendar'] + [{'event': 'IETF 128', 'day': '2027-03-26', 'scan': 'light', 'verified': False}]
        held = self.register([self.entry(review='2027-06-01'),
                              self.entry(id='webtransport.h3', codepoints=[], spec=self.DRAFT, review='2027-06-01')],
                             review=dict(triggered, calendar=later, next='2027-01-04'))
        self.assertEqual(0, self.run_guard(held, today='2026-12-31')[0])


class LegalRegister(RegisterFixture, unittest.TestCase):
    """scripts/check-registers.py, legal domain (COMPLY.0): a binding obligation needs an active check, primary sources
    are official, reading precedes closing, counsel never blocks implementation but every outstanding item is asked
    about in an open review, scopes turn live with their rows and Turkish law is never omitted."""

    def live(self, **changes):
        return self.scope(state='live', evidence=['tests/test_quic.cpp'], **changes)

    def test_a_complete_legal_register_passes_and_names_the_next_effective_day(self):
        code, output = self.run_legal(self.legal_register())
        self.assertEqual(0, code, output)
        self.assertIn('PASS: legal register: 1 entries, 1 pinned, next review 2027-10-25; 0 binding on 0 live scopes; '
                      'open counsel questions: 1 in 1 review', output)
        later = self.legal_entry(id='tr.5651.game-platforms', topic='privacy', effective='2026-11-01')
        code, output = self.run_legal(self.legal_register([self.legal_entry(), later]), today='2026-10-31')
        self.assertEqual(0, code, output)
        self.assertIn('the next to take effect is tr.5651.game-platforms on 2026-11-01', output)

    def test_a_binding_obligation_needs_an_active_check(self):
        self.assert_legal_fails(self.legal_register(scopes=[self.live()]),
                                'binding since 2016-04-07 on the live flow.crash-reports without an active check; '
                                'land data inventory CTest first')
        active = {'ref': 'tests/test_quic.cpp', 'name': 'crd-inventory-tests', 'state': 'active'}
        code, output = self.run_legal(self.legal_register([self.legal_entry(check=active)], scopes=[self.live()]))
        self.assertEqual(0, code, output)
        self.assertIn('1 binding on 1 live scopes', output)
        future = self.legal_register([self.legal_entry(effective='2026-12-09')], scopes=[self.live()])
        self.assertEqual(0, self.run_legal(future, today='2026-12-08')[0])
        self.assert_legal_fails(future, 'binding since 2026-12-09', today='2026-12-09')
        month = self.legal_register([self.legal_entry(effective='2026-12')], scopes=[self.live()])
        self.assert_legal_fails(month, 'binding since 2026-12', today='2026-12-01')
        watched = self.legal_register([self.legal_entry(state='watch', effective=None)], scopes=[self.live()])
        self.assertEqual(0, self.run_legal(watched)[0])
        ended = self.legal_register([self.legal_entry(sunset='2026-10-01')], scopes=[self.live()])
        self.assert_legal_fails(ended, 'Law No. 6698 ended on 2026-10-01; mark the entry deprecated')
        self.assertNotIn('binding since', self.run_legal(ended)[1])
        self.assert_legal_fails(self.legal_register([self.legal_entry(sunset='2016-01-01')]),
                                'sunset 2016-01-01 is not after effective 2016-04-07')

    def test_a_scope_turns_live_with_any_of_its_rows(self):
        self.assert_legal_fails(self.legal_register(scopes=[self.scope(rows=['NET.2', 'DONE'])]),
                                'legal scope flow.crash-reports: DONE is Done, so it is live; mark it live')
        self.assert_legal_fails(self.legal_register(scopes=[self.scope(state='live')]),
                                'a live scope names the evidence that it shipped')
        self.assert_legal_fails(self.legal_register(scopes=[self.scope(rows=['NO-SUCH'])]), "unknown: ['NO-SUCH']")
        self.assert_legal_fails(self.legal_register(scopes=[self.scope(), self.scope()]), 'id is unique')
        self.assert_legal_fails(self.legal_register(scopes=[self.scope(kind='service')]), 'kind is one of')
        self.assert_legal_fails(self.legal_register(scopes=[]), 'scopes lists the products')
        self.assert_legal_fails(self.legal_register([self.legal_entry(applies_to=['flow.nowhere'])]),
                                "applies_to names unknown scopes ['flow.nowhere']")
        self.assert_legal_fails(self.legal_register([self.legal_entry(applies_to=[])]), 'applies_to names the')

    def test_primary_sources_are_official_and_jurisdictions_known(self):
        foreign = self.instrument(primary='https://www.mondaq.com/turkey/kvkk')
        self.assert_legal_fails(self.legal_register([self.legal_entry(instrument=foreign)]),
                                'primary source host www.mondaq.com is not an official host of TR (mevzuat.gov.tr)')
        lookalike = self.instrument(primary='https://evilmevzuat.gov.tr/x')
        self.assert_legal_fails(self.legal_register([self.legal_entry(instrument=lookalike)]),
                                'is not an official host of TR')
        plain = self.instrument(primary='http://www.mevzuat.gov.tr/x')
        self.assert_legal_fails(self.legal_register([self.legal_entry(instrument=plain)]), 'must be an https URL')
        unknown = self.instrument(jurisdiction='XX')
        self.assert_legal_fails(self.legal_register([self.legal_entry(instrument=unknown)]),
                                "jurisdiction 'XX' is not in the register's jurisdictions")
        europe = {'EU': {'name': 'EU', 'hosts': ['eur-lex.europa.eu']}}
        self.assert_legal_fails(self.legal_register(jurisdictions=europe), 'includes TR')
        bad = {'TR': {'name': 'Türkiye', 'hosts': ['mevzuat.gov.tr']}, 'Europe': {'name': 'x', 'hosts': ['A b']}}
        self.assert_legal_fails(self.legal_register(jurisdictions=bad), "jurisdiction code 'Europe'")
        self.assert_legal_fails(self.legal_register(jurisdictions=bad), 'jurisdiction Europe has a name and its')
        self.assert_legal_fails(self.legal_register([self.legal_entry(instrument=self.instrument(type='memo'))]),
                                'instrument.type must be one of')
        self.assert_legal_fails(self.legal_register([self.legal_entry(instrument={'name': 'x'})]),
                                'instrument has article, consolidated, jurisdiction, name, primary, type')

    def test_reading_precedes_closing_and_counsel_never_blocks_implementing(self):
        read = self.instrument(consolidated=self.READ)
        verified = {'ref': 'docs/research.md#n83', 'read': '2026-10-27', 'verified': True}
        self.assert_legal_fails(self.legal_register([self.legal_entry(instrument=read)]),
                                'the consolidated text was read, so the source is verified')
        self.assertEqual(0, self.run_legal(self.legal_register([self.legal_entry(instrument=read,
                                                                                 source=verified)]))[0])
        home = self.instrument(consolidated=self.READ, primary='https://www.mevzuat.gov.tr/')
        self.assert_legal_fails(self.legal_register([self.legal_entry(instrument=home, source=verified)]),
                                'cites the instrument\'s own document, not the home page')
        for broken in ({'version': 'v'}, dict(self.READ, sha256='abc'), dict(self.READ, version='')):
            self.assert_legal_fails(self.legal_register([self.legal_entry(instrument=self.instrument(
                consolidated=broken), source=verified)]), 'instrument.consolidated is null until read')
        self.assert_legal_fails(self.legal_register([self.legal_entry(instrument=self.instrument(
            consolidated=dict(self.READ, read='2026-11-05')), source=verified)]), 'is after today')
        self.assert_legal_fails(self.legal_register([self.legal_entry(rows=['DONE'])], governed_rows=[]),
                                'every row (DONE) is Done, but Law No. 6698 was never read')
        active = {'ref': 'tests/test_quic.cpp', 'name': 'crd-inventory-tests', 'state': 'active'}
        proven = dict(state='implemented', evidence=['tests/test_quic.cpp'], check=active)
        self.assert_legal_fails(self.legal_register([self.legal_entry(**proven)]),
                                'implemented before Law No. 6698 was read in its consolidated text')
        code, output = self.run_legal(self.legal_register([self.legal_entry(**proven, instrument=read,
                                                                            source=verified)]))
        self.assertEqual(0, code, output)  # the open review asks about it; implementation never waits for counsel
        reviewed = {'needed': True, 'reviewed': '2026-10-30'}
        self.assertEqual(0, self.run_legal(self.legal_register([self.legal_entry(
            **proven, instrument=read, source=verified, counsel=reviewed)]))[0])
        self.assert_legal_fails(self.legal_register([self.legal_entry(
            **dict(proven, check=dict(active, ref='docs/ROADMAP.md#slice-net.2', state='planned')),
            instrument=read, source=verified, counsel=reviewed)]), 'implemented, so its check is active')
        self.assert_legal_fails(self.legal_register([self.legal_entry(counsel={'needed': False,
                                                                               'reviewed': '2026-10-30'})]),
                                'counsel.reviewed records the review a needed flag asked for')
        self.assert_legal_fails(self.legal_register([self.legal_entry(counsel={'needed': 'yes', 'reviewed': None})]),
                                'counsel has needed')
        self.assert_legal_fails(self.legal_register([self.legal_entry(counsel={'needed': True,
                                                                               'reviewed': '2026-11-05'})]),
                                'counsel.reviewed 2026-11-05 is after today')

    def test_a_scan_still_owed_reads_what_a_closed_row_left_unread(self):
        """User decision 2026-11-01: an instrument not yet read is read at the review cadence, by the scan a calendar
        event owes, never as a clause of the row that implements its obligation."""
        active = {'ref': 'tests/test_quic.cpp', 'name': 'crd-inventory-tests', 'state': 'active'}
        proven = self.legal_entry(state='implemented', evidence=['tests/test_quic.cpp'], check=active)
        closed = self.legal_entry(id='tr.kvkk.rights', rows=['DONE'], state='watch', effective=None)
        quarterly = {'event': 'Quarterly review', 'day': '2027-09-30', 'scan': 'light', 'verified': True}

        def owing(reads, earlier=None):
            calendar = [dict(quarterly, reads=reads)] if earlier is None else \
                [{'event': 'Covered', 'day': '2026-10-01', 'scan': 'light', 'verified': True, 'reads': earlier},
                 quarterly]
            return self.legal_register([proven, closed], review=self.schedule(next='2027-10-25', calendar=calendar))

        code, output = self.run_legal(owing(['tr.kvkk.processing', 'tr.kvkk.rights']))
        self.assertEqual(0, code, output)
        self.assert_legal_fails(owing(['tr.kvkk.processing']),
                                'tr.kvkk.rights: every row (DONE) is Done, but Law No. 6698 was never read in its '
                                'consolidated text from the primary source, and no scan still owed reads it')
        self.assert_legal_fails(owing(['tr.kvkk.rights']),
                                'tr.kvkk.processing: implemented before Law No. 6698 was read in its consolidated text '
                                'from the primary source, and no scan still owed reads it')
        covered = owing(None, earlier=['tr.kvkk.processing', 'tr.kvkk.rights'])
        self.assert_legal_fails(covered, 'tr.kvkk.rights: every row (DONE) is Done')
        self.assert_legal_fails(covered, 'tr.kvkk.processing: implemented before Law No. 6698 was read')
        verified = {'ref': 'docs/research.md#n83', 'read': '2026-10-27', 'verified': True}
        read = self.legal_entry(id='tr.kvkk.read', instrument=self.instrument(consolidated=self.READ), source=verified)
        cases = (
            ([], 'calendar Quarterly review: reads lists the entries whose instruments the scan reads, each once'),
            ('tr.kvkk.rights', 'reads lists the entries whose instruments the scan reads, each once'),
            ([7], 'reads lists the entries whose instruments the scan reads, each once'),
            (['tr.kvkk.rights', 'tr.kvkk.rights'], 'reads lists the entries whose instruments the scan reads, each'),
            (['tr.nowhere'], 'calendar Quarterly review: reads tr.nowhere, which is no instrument of the register'),
            (['tr.kvkk.read'], 'calendar Quarterly review: reads tr.kvkk.read, whose text was read; drop it'),
        )
        for reads, message in cases:
            with self.subTest(message=message):
                self.assert_legal_fails(self.legal_register([self.legal_entry(), read], review=self.schedule(
                    next='2027-10-25', calendar=[dict(quarterly, reads=reads)])), message)
        ietf = {'event': 'IETF 127', 'day': '2026-11-20', 'scan': 'light', 'verified': False,
                'reads': ['quic.transport']}
        self.assert_fails(self.register([self.entry()], review=self.schedule(calendar=[ietf])),
                          'calendar IETF 127: reads names the instruments a legal scan reads; a standards scan reads '
                          'every record')
        self.assert_fails(self.register([self.entry()], review=self.schedule(calendar=[dict(ietf, note='x')])),
                          'a calendar event has event, day, scan (light or full) and verified, and in the legal '
                          'domain may name the instruments its scan reads')

    def test_every_outstanding_counsel_item_is_asked_about_in_one_open_review(self):
        question = {'entry': 'tr.kvkk.processing', 'question': 'Which Article 5 condition applies?'}
        review = {'id': 'counsel.2026-11-01', 'title': 'First review', 'opened': '2026-11-01', 'answered': None,
                  'questions': [question]}

        def reviews(*items):
            return self.legal_register(counsel_reviews=list(items))

        code, output = self.run_legal(reviews(review))
        self.assertEqual(0, code, output)
        cases = (
            ('none', 'counsel_reviews lists the dated, named counsel reviews and their questions'),
            ([], 'the entry tr.kvkk.processing needs counsel, but no open review asks about it; add its exact question '
                 'to an open review'),
            ([dict(review, answered='2026-11-01')], 'the entry tr.kvkk.processing needs counsel, but no open review'),
            ([{'id': 'counsel.x'}], 'a review has an id, a title, the day it was opened'),
            ([dict(review, id='Counsel 1')], 'a review has an id, a title'),
            ([dict(review, title='')], 'a review has an id, a title'),
            ([review, review], 'counsel_reviews: counsel.2026-11-01 is named twice'),
            ([dict(review, opened='soon')], "opened must be a YYYY-MM-DD date, not 'soon'"),
            ([dict(review, opened='2026-11-02')], 'opened 2026-11-02 and answered None are not in order up to today'),
            ([dict(review, questions=[])], 'questions lists what counsel is asked, one item each'),
            ([dict(review, questions=[dict(question, question='Which condition applies.')])],
             'a question names one entry or classification and asks its question (ending with "?")'),
            ([dict(review, questions=[dict(question, classification='tool')])], 'a question names one entry or'),
            ([dict(review, questions=[{'entry': 'tr.kvkk.processing'}])], 'a question names one entry or'),
            ([dict(review, questions=[question, dict(question, entry='tr.nowhere')])],
             'asks about the entry tr.nowhere, which the register does not hold'),
            ([dict(review, questions=[question, question])], 'asks about the entry tr.kvkk.processing twice'),
            ([review, dict(review, id='counsel.again')],
             'counsel.again: the entry tr.kvkk.processing is already asked about in the open review '
             'counsel.2026-11-01'),
        )
        for items, message in cases:
            with self.subTest(message=message):
                self.assert_legal_fails(self.legal_register(counsel_reviews=items), message)
        answered = {'needed': True, 'reviewed': '2026-10-30'}
        settled = dict(review, opened='2026-10-27', answered='2026-10-30')
        self.assertEqual(0, self.run_legal(self.legal_register([self.legal_entry(counsel=answered)],
                                                               counsel_reviews=[settled]))[0])
        self.assert_legal_fails(self.legal_register([self.legal_entry(counsel=answered)], counsel_reviews=[]),
                                "the entry tr.kvkk.processing records counsel's answer of 2026-10-30, but no review "
                                'answered that day asked about it')
        self.assert_legal_fails(self.legal_register([self.legal_entry(counsel=answered)], counsel_reviews=[
            dict(settled, answered='2026-10-29')]), "records counsel's answer of 2026-10-30, but no review")
        self.assert_legal_fails(self.legal_register([self.legal_entry(counsel=answered)], counsel_reviews=[
            dict(settled, answered='2026-10-26')]), 'opened 2026-10-27 and answered 2026-10-26 are not in order')
        self.assert_legal_fails(self.legal_register([self.legal_entry(counsel=answered)], counsel_reviews=[
            settled, review]), 'counsel.2026-11-01: asks about the entry tr.kvkk.processing, whose counsel is not '
                               'outstanding; drop the question or record the answer')
        none = {'needed': False, 'reviewed': None}
        self.assert_legal_fails(self.legal_register([self.legal_entry(counsel=none)], counsel_reviews=[review]),
                                'whose counsel is not outstanding')
        code, output = self.run_legal(self.legal_register([self.legal_entry(counsel=none)], counsel_reviews=[]))
        self.assertEqual(0, code, output)
        self.assertIn('open counsel questions: 0 in 0 reviews', output)

    def test_turkish_law_is_never_omitted(self):
        eu = self.legal_entry(id='eu.ai-act.transparency', topic='ai', instrument=self.instrument(
            jurisdiction='EU', primary='https://eur-lex.europa.eu/eli/reg/2024/1689/oj'))
        self.assert_legal_fails(self.legal_register([self.legal_entry(), eu]),
                                'legal register: topic ai has no Turkish (TR) entry')
        platform = dict(eu, id='platform.apple-sdk-minimum', topic='platform', instrument=self.instrument(
            jurisdiction='PLATFORM', primary='https://developer.apple.com/news/upcoming-requirements/'))
        self.assertEqual(0, self.run_legal(self.legal_register([self.legal_entry(), platform]))[0])
        self.assertEqual(0, self.run_legal(self.legal_register([self.legal_entry(), dict(eu, topic='privacy')]))[0])

    def test_the_format_of_entries_checks_and_dates(self):
        self.assert_legal_fails(self.legal_register([self.legal_entry(effective=None)]),
                                'a pinned obligation names its effective date')
        self.assert_legal_fails(self.legal_register([self.legal_entry(effective='2016-13')]),
                                "effective must be YYYY, YYYY-MM or YYYY-MM-DD")
        self.assert_legal_fails(self.legal_register([self.legal_entry(topic='weather')]), 'topic must be one of')
        self.assert_legal_fails(self.legal_register([self.legal_entry(kind='protocol')]), 'kind must be one of')
        self.assert_legal_fails(self.legal_register([self.legal_entry(obligation='')]), 'obligation states the duty')
        self.assert_legal_fails(self.legal_register([self.legal_entry(check=None)]), 'check has ref, name and state')
        self.assert_legal_fails(self.legal_register([self.legal_entry(check={
            'ref': 'tests/test_quic.cpp', 'name': 'x', 'state': 'planned'})]), 'a planned check cites the ROADMAP row')
        self.assert_legal_fails(self.legal_register([self.legal_entry(check={
            'ref': 'docs/ROADMAP.md#slice-net.2', 'name': 'x', 'state': 'active'})]),
            'an active check cites the test, script or step that runs it')
        self.assert_legal_fails(self.legal_register([self.legal_entry(check={
            'ref': 'docs/ROADMAP.md#slice-net.2', 'name': 'x', 'state': 'green'})]), 'check.state must be one of')
        self.assert_legal_fails(self.legal_register([self.legal_entry(check={
            'ref': 'tests/gone.cpp', 'name': 'x', 'state': 'active'})]), "check 'tests/gone.cpp' does not exist")
        self.assert_legal_fails(self.legal_register([self.legal_entry(rows=['NET-STD'])]),
                                'governed row NET.2 names no register entry')
        self.assert_legal_fails(self.legal_register([self.legal_entry(owner='nowhere')]), "owner 'nowhere' is neither")
        self.assert_legal_fails(self.legal_register([self.legal_entry(spec={'type': 'rfc'})]), "unknown keys ['spec']")
        self.assert_legal_fails(self.legal_register(checklist=[]), "unknown keys ['checklist']")

    def test_a_missing_legal_register_fails(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_tree(root, self.register([self.entry()]))
            (root / 'cmake' / 'legal-register.json').unlink()
            code, output = self.guard(root, '2026-11-01')
            self.assertEqual(1, code, output)
            self.assertIn('cmake/legal-register.json:', output)


class ComplyRules(RegisterFixture, unittest.TestCase):
    """scripts/gen-comply-rules.py and the guard's rule generation rules (COMPLY.0): one canonical generation per
    jurisdiction with rules, reviewed before it ships, its file held to the digest its review recorded."""

    def entries(self):
        reviewed = self.legal_entry(id='tr.5651.game-platform', topic='online-safety', state='implemented',
                                    instrument=self.instrument(name='Law No. 5651', consolidated=self.READ),
                                    title='Game "platform" duties\\ rules', obligation='Rate\tgames.',
                                    check={'ref': 'tests/test_quic.cpp', 'name': 'ratings', 'state': 'active'},
                                    effective='2026-11', sunset='2030-01-01', evidence=['tests/test_quic.cpp'],
                                    source={'ref': 'docs/research.md#n83', 'read': '2026-10-27', 'verified': True},
                                    counsel={'needed': True, 'reviewed': '2026-10-30'})
        action = self.legal_entry(id='eu.gdpr.records', kind='user-action', counsel={'needed': False,
                                                                                        'reviewed': None},
                                  instrument=self.instrument(name='GDPR', jurisdiction='EU',
                                                             primary='https://eur-lex.europa.eu/eli/reg/2016/679/oj'))
        watched = self.legal_entry(id='tr.ai.bill', state='watch', effective=None)
        return [self.legal_entry(), reviewed, action, watched]

    def legal(self, **changes):
        scopes = [self.scope(id='product.store', kind='product', title='The store'), self.scope()]
        return self.legal_register(self.entries(), scopes=scopes, **changes)

    def tree(self, scratch, legal):
        root = Path(scratch)
        self.write_tree(root, self.register([self.entry()]), legal)
        return root

    def run_tool(self, root, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = comply_rules.main(['--root', str(root), *args])
        return code, output.getvalue()

    def test_one_canonical_generation_per_jurisdiction_with_rules(self):
        texts = comply_rules.outputs(self.legal(), {'TR': 4, 'EU': 1})
        self.assertEqual(['assets/comply/tr.rules.toml', 'assets/comply/eu.rules.toml'], list(texts))
        tr = texts['assets/comply/tr.rules.toml']
        self.assertTrue(tr.startswith('# Generated by scripts/gen-comply-rules.py from cmake/legal-register.json; '
                                      'never edit by hand.\n# A change is reviewed with: python '
                                      'scripts/gen-comply-rules.py --review TR\nformat = "cerid-rules/1"\n'
                                      'jurisdiction = "TR"\ngeneration = 4\nregister = "2026-10-31"\n\n[[scope]]\n'
                                      'id = "flow.crash-reports"\nkind = "flow"\nstate = "planned"\n\n[[rule]]\n'
                                      'id = "tr.kvkk.processing"\n'), tr)
        self.assertNotIn('product.store', tr)  # only the scopes its rules name
        self.assertNotIn('tr.ai.bill', tr)  # a watched entry is not a rule
        self.assertIn('read = false\nobligation = "Process personal data lawfully."\n'
                      'applies_to = ["flow.crash-reports"]\ncheck = "planned"\n', tr)
        self.assertIn('effective = "2016-04-07"\nsunset = ""\ncounsel = "needed"\ncounsel_reviewed = ""\n'
                      'rows = ["NET.2"]\n', tr)
        self.assertIn('title = "Game \\"platform\\" duties\\\\ rules"\n', tr)
        self.assertIn('obligation = "Rate\\u0009games."\n', tr)
        self.assertIn('state = "implemented"\n', tr)
        self.assertIn('read = true\n', tr)
        self.assertIn('effective = "2026-11"\nsunset = "2030-01-01"\ncounsel = "reviewed"\n'
                      'counsel_reviewed = "2026-10-30"\n', tr)
        self.assertTrue(tr.endswith('rows = ["NET.2"]\n'))
        eu = texts['assets/comply/eu.rules.toml']
        self.assertIn('kind = "user-action"\n', eu)
        self.assertIn('counsel = "none"\n', eu)
        self.assertEqual(texts, comply_rules.outputs(self.legal(), {'TR': 4, 'EU': 1}))
        with self.assertRaisesRegex(comply_rules.RuleError, 'no generation for TR'):
            comply_rules.outputs(self.legal())
        unknown = self.legal_register([self.legal_entry(applies_to=['flow.none'])])
        with self.assertRaisesRegex(comply_rules.RuleError, "unknown scope 'flow.none'"):
            comply_rules.outputs(unknown, {'TR': 1})

    def test_check_names_drift_and_stray_files(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.tree(scratch, self.legal())
            self.assertEqual((0, 'rule generations current: 2 jurisdiction(s)\n'), self.run_tool(root, '--check'))
            path = root / 'assets/comply/tr.rules.toml'
            path.write_bytes(path.read_bytes().replace(b'\n', b'\r\n'))
            code, output = self.run_tool(root, '--check')
            self.assertEqual(1, code)
            self.assertIn("FAIL: assets/comply/tr.rules.toml differs from scripts/gen-comply-rules.py's output", output)
            self.assertEqual(0, self.run_tool(root)[0])
            (root / 'assets/comply/gb.rules.toml').write_text('x', encoding='utf-8')
            code, output = self.run_tool(root, '--check')
            self.assertEqual(1, code)
            self.assertIn("FAIL: assets/comply/gb.rules.toml is no jurisdiction's rule generation", output)

    def test_review_records_the_next_generation_only_for_changed_rules(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.tree(scratch, self.legal(rule_generations={}))
            self.assertEqual((0, 'reviewed TR: generation 1 on 2026-10-31, assets/comply/tr.rules.toml\n'),
                             self.run_tool(root, '--review', 'TR', '--day', '2026-10-31'))
            code, output = self.run_tool(root, '--review', 'TR', '--day', '2026-11-02')
            self.assertEqual(1, code)
            self.assertIn("TR's rules are unchanged since generation 1; nothing to review", output)
            legal = json.loads((root / 'cmake/legal-register.json').read_text(encoding='utf-8'))
            legal['entries'][0]['obligation'] = 'Process personal data lawfully and minimally.'
            (root / 'cmake/legal-register.json').write_text(json.dumps(legal), encoding='utf-8')
            self.assertEqual(0, self.run_tool(root, '--review', 'TR', '--day', '2026-11-02')[0])
            legal = json.loads((root / 'cmake/legal-register.json').read_text(encoding='utf-8'))
            record = legal['rule_generations']['TR']
            text = (root / 'assets/comply/tr.rules.toml').read_bytes()
            self.assertEqual({'generation': 2, 'reviewed': '2026-11-02',
                              'sha256': hashlib.sha256(text).hexdigest()}, record)
            self.assertIn(b'generation = 2\n', text)
            self.assertIn(b'and minimally', text)
            self.assertEqual(['TR'], list(legal['rule_generations']))
            code, output = self.run_tool(root, '--review', 'GB')
            self.assertEqual(1, code)
            self.assertIn('GB holds no rule', output)

    def test_the_guard_holds_each_file_to_its_reviewed_digest(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.tree(scratch, self.legal())
            self.assertEqual(0, self.guard(root, '2026-11-01')[0])
            path = root / 'assets/comply/tr.rules.toml'
            path.write_bytes(path.read_bytes() + b'\n')
            code, output = self.guard(root, '2026-11-01')
            self.assertEqual(1, code)
            self.assertIn('legal register: rule_generations: assets/comply/tr.rules.toml changed since generation 1 '
                          'was reviewed; review it with python scripts/gen-comply-rules.py --review TR', output)
            path.unlink()
            code, output = self.guard(root, '2026-11-01')
            self.assertIn("TR's generation assets/comply/tr.rules.toml is missing", output)

    def test_the_guard_wants_one_well_formed_record_per_jurisdiction_with_rules(self):
        legal = self.legal()
        with tempfile.TemporaryDirectory() as scratch:
            records = self.reviewed(Path(scratch), legal)
        good = records['TR']
        self.assert_legal_fails(self.legal(rule_generations={'EU': records['EU']}),
                                'TR has rules but no reviewed generation; review them with python '
                                'scripts/gen-comply-rules.py --review TR')
        self.assert_legal_fails(self.legal(rule_generations=dict(records, GB=good)),
                                'GB holds no rule, so it has no generation')
        for bad in (dict(good, generation=0), dict(good, generation=True), dict(good, generation='1'),
                    dict(good, sha256='AB' * 32), {'generation': 1, 'reviewed': '2026-10-31'}, ['TR'], 5):
            with self.subTest(bad=bad):
                self.assert_legal_fails(self.legal(rule_generations=dict(records, TR=bad)),
                                        'TR has a generation (from 1), its reviewed day and the sha256 of its file')
        self.assert_legal_fails(self.legal(rule_generations=dict(records, TR=dict(good, reviewed='2026-11-02'))),
                                'TR was reviewed on 2026-11-02, after today')
        self.assert_legal_fails(self.legal(rule_generations=dict(records, TR=dict(good, reviewed='2026-11'))),
                                'reviewed must be a YYYY-MM-DD date')
        self.assert_legal_fails(self.legal(rule_generations=[]),
                                'rule_generations maps each jurisdiction with rules to its reviewed generation')


class TextFold(unittest.TestCase):
    """scripts/gen-text-fold.py (COMPLY.1): crd-core's search-folding table from the Unicode Character Database, each
    letter folded by the first rule that answers, every fold a fixed point, and the committed table held by --check."""

    def run_tool(self, root, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = text_fold.main(['--root', str(root), *args])
        return code, output.getvalue()

    def test_each_letter_folds_by_the_first_rule_that_answers(self):
        fold = text_fold.fold_of
        self.assertEqual('ss', fold(0x00DF))  # manual: sharp s
        self.assertEqual('th', fold(0x00FE))  # manual: thorn
        self.assertEqual('a', fold(0x0259))  # manual: schwa
        self.assertEqual('oe', fold(0x0152))  # manual: the OE ligature
        self.assertEqual('s', fold(0x015E))  # NFKD: S with cedilla
        self.assertEqual('i', fold(0x0130))  # NFKD: I with dot above
        self.assertEqual('a', fold(0xFF21))  # NFKD: fullwidth A
        self.assertEqual('dz', fold(0x01C5))  # NFKD: two letters
        self.assertEqual('o', fold(0x00D8))  # the name: O WITH STROKE
        self.assertEqual('i', fold(0x0131))  # the name: DOTLESS I
        self.assertEqual('l', fold(0x0142))  # the name: L WITH STROKE
        self.assertEqual('z', fold(0x01EE))  # NFKD to small ezh, which folds again to z
        self.assertEqual('ж', fold(0x0416))  # case: Cyrillic Zhe
        self.assertEqual('σ', fold(0x03C2))  # case fold: final sigma
        self.assertEqual('α', fold(0x0386))  # accent and case: Alpha with tonos
        self.assertIsNone(fold(ord('a')))  # already folded
        self.assertIsNone(fold(0x0436))
        self.assertIsNone(fold(0x01C0))  # a click letter: no rule answers, kept

    def test_code_points_are_letters_separators_or_elided(self):
        kind = text_fold.kind_of
        self.assertEqual('letter', kind(ord('a')))
        self.assertEqual('letter', kind(ord('7')))
        self.assertEqual('separator', kind(ord(' ')))
        self.assertEqual('separator', kind(ord('-')))
        self.assertEqual('separator', kind(0x00D7))  # multiplication sign
        self.assertEqual('elided', kind(ord("'")))
        self.assertEqual('elided', kind(0x2019))
        self.assertEqual('elided', kind(0x0301))  # a combining mark
        self.assertEqual('elided', kind(0x02BF))  # ayn: a modifier letter, elided

    def test_tables_cover_the_blocks_and_hold_every_fold_to_a_fixed_point(self):
        ranges, mappings = text_fold.tables()
        for first, last, kind in ranges:
            self.assertLessEqual(first, last)
            self.assertIn(kind, ('separator', 'elided'))
            self.assertTrue(text_fold.covered(first) and text_fold.covered(last))
        for (_, last, _), (first, _, _) in zip(ranges, ranges[1:]):
            self.assertLess(last, first)
        folded = dict(mappings)
        self.assertEqual(sorted(folded), [cp for cp, _ in mappings])
        for cp, out in mappings:
            self.assertTrue(1 <= len(out) <= 2, hex(cp))
            for target in out:
                self.assertEqual([target], folded.get(target, [target]), hex(cp))
        with patch.dict(text_fold.MANUAL, {0x00DF: 'ßß'}):
            with self.assertRaises(SystemExit):
                text_fold.tables()

    def test_check_holds_the_committed_table(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            target = root / text_fold.OUT
            target.parent.mkdir(parents=True)
            self.assertEqual(1, self.run_tool(root, '--check')[0])  # missing
            code, output = self.run_tool(root)
            self.assertEqual(0, code, output)
            self.assertEqual(text_fold.render().encode('utf-8'), target.read_bytes())
            self.assertEqual(0, self.run_tool(root, '--check')[0])
            target.write_bytes(target.read_bytes().replace(b'0x0131U', b'0x0132U', 1))
            code, output = self.run_tool(root, '--check')
            self.assertEqual(1, code)
            self.assertIn('differs', output)
        self.assertEqual(0, self.run_tool(ROOT, '--check')[0])  # the committed table


class CryptoInventory(RegisterFixture, unittest.TestCase):
    """scripts/gen-crypto-inventory.py and the guard's cryptography rules (COMPLY.1): every standards entry declares its
    cryptographic functions and every algorithm its key lengths, every export's inventory is read from the build and
    the register, and the guard holds each export to the inventory its head classification record was reviewed
    against."""

    CMAKE = ('crd_module(engine/foundation/core engine/core)\n'
             'crd_module(engine/foundation/crypto engine/crypto DEPENDS core)\n'
             '# crd_module(tools/retired HOST EXECUTABLES retired DEPENDS crypto)\n'
             'crd_module(engine/foundation/net engine/net DEPENDS core)\n'
             'crd_module(tools/tool HOST EXECUTABLES tool DEPENDS crypto\n    PACKAGES zstd)\n')

    def algorithm(self, **changes):
        """AES-256-GCM, implemented by the crypto module."""
        entry = self.entry(id='crypto.aes-gcm', kind='algorithm', owner='crypto', state='implemented',
                           evidence=['tests/test_quic.cpp'], cryptography=['confidentiality', 'integrity'],
                           spec={'type': 'rfc', 'ref': 'RFC 5116', 'status': 'current'},
                           codepoints=[{'registry': 'Cerid algorithm identifiers', 'name': 'aes-256-gcm',
                                        'value': '0x0502', 'key_bits': [256]}])
        entry.update(changes)
        return entry

    def policy(self):
        """A Cerid format that only authenticates (the signed policy generations), implemented by crypto."""
        return self.entry(id='cerid.policy-generation', kind='format', owner='crypto', state='implemented',
                          evidence=['tests/test_quic.cpp'], cryptography=['authentication'], codepoints=[],
                          spec={'type': 'cerid', 'ref': 'docs/design/network-foundation.md', 'status': 'stable'})

    def standards(self, algorithm_state='planned'):
        algorithm = self.algorithm() if algorithm_state == 'implemented' else \
            self.algorithm(state=algorithm_state, evidence=[])
        return self.register([self.entry(), self.policy(), algorithm])

    def classified(self, entry_id='tr.btk.encryption', **changes):
        """An export-control obligation whose active check is the classification."""
        return self.legal_entry(id=entry_id, topic='export-control',
                                check={'ref': crypto_inventory.GENERATOR, 'name': 'classification', 'state': 'active'},
                                **changes)

    def finding(self, **changes):
        finding = {'entry': 'tr.btk.encryption', 'classification': 'Carries no encryption',
                   'authorization': 'None required', 'report': False, 'basis': 'It encrypts nothing.'}
        finding.update(changes)
        return finding

    def head(self, inventory='0' * 64, **changes):
        record = {'release': None, 'inventory': inventory, 'reviewed': '2026-10-31', 'findings': [self.finding()],
                  'counsel': {'needed': True, 'reviewed': None}}
        record.update(changes)
        return record

    def tree(self, scratch, classifications=None, legal_entries=None, algorithm_state='planned'):
        """Core and crypto are packaged, net is not, and the tool program reaches crypto; zstd is its package."""
        root = Path(scratch)
        legal = self.legal_register([self.legal_entry(), self.classified()] if legal_entries is None else
                                    legal_entries)
        if classifications is not None:
            legal['crypto_classifications'] = classifications
        self.write_tree(root, self.standards(algorithm_state), legal)
        (root / 'CMakeLists.txt').write_text(self.CMAKE, encoding='utf-8')
        for module, text in (('core', 'crd_package_library(crd-core NAME core)\n'),
                             ('crypto', 'crd_package_library(crd-crypto NAME crypto)\n'),
                             ('net', 'add_library(crd-net STATIC)\n')):
            (root / 'engine/foundation' / module).mkdir(parents=True)
            (root / 'engine/foundation' / module / 'CMakeLists.txt').write_text(text, encoding='utf-8')
        (root / 'scripts').mkdir(exist_ok=True)
        (root / crypto_inventory.GENERATOR).write_text('# fixture\n', encoding='utf-8')
        return root

    def digests(self, root):
        return {export['id']: crypto_inventory.digest(export) for export in crypto_inventory.build(root)['exports']}

    def reviewed_tree(self, scratch, **changes):
        """A tree whose two exports carry head records reviewed against their current inventories."""
        root = self.tree(scratch, {}, **changes)
        self.set_classifications(root, {export: [self.head(digest)] for export, digest in self.digests(root).items()})
        return root

    @staticmethod
    def legal_of(root):
        return json.loads((root / 'cmake/legal-register.json').read_text(encoding='utf-8'))

    def set_classifications(self, root, classifications, counsel_reviews=None):
        legal = self.legal_of(root)
        legal['crypto_classifications'] = classifications
        legal['counsel_reviews'] = self.counsel_reviews(legal) if counsel_reviews is None else counsel_reviews
        (root / 'cmake/legal-register.json').write_text(json.dumps(legal), encoding='utf-8')

    @staticmethod
    def implement_algorithm(root):
        path = root / 'cmake/standards-register.json'
        standards = json.loads(path.read_text(encoding='utf-8'))
        standards['entries'][2].update(state='implemented', evidence=['tests/test_quic.cpp'])
        path.write_text(json.dumps(standards), encoding='utf-8')

    def run_tool(self, root, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = crypto_inventory.main(['--root', str(root), *args])
        return code, output.getvalue()

    def assert_guard_fails(self, root, message):
        code, output = self.guard(root, '2026-11-01')
        self.assertEqual(1, code, output)
        self.assertIn(message, output)

    def test_every_standards_entry_declares_its_cryptography(self):
        missing = self.entry()
        del missing['cryptography']
        message = 'cryptography lists the functions it performs, each once in the order authentication, '
        for broken in (missing, self.entry(cryptography=['integrity', 'authentication']),
                       self.entry(cryptography=['encryption']), self.entry(cryptography=['integrity', 'integrity']),
                       self.entry(cryptography='authentication')):
            with self.subTest(broken=broken.get('cryptography')):
                self.assert_fails(self.register([broken]), message)
        algorithm = self.algorithm(owner='core')
        self.assertEqual(0, self.run_guard(self.register([self.entry(), algorithm]))[0])
        self.assert_fails(self.register([self.algorithm(owner='core', cryptography=['integrity'])]),
                          'algorithm aes-256-gcm (authenticated encryption with associated data) performs '
                          'confidentiality, which cryptography does not name')
        point = algorithm['codepoints'][0]
        for key_bits in (None, [], [0], ['256'], [True], 256):
            with self.subTest(key_bits=key_bits):
                self.assert_fails(self.register([self.algorithm(owner='core', codepoints=[dict(point,
                                                                                                key_bits=key_bits)])]),
                                  'algorithm aes-256-gcm names key_bits')
        self.assert_fails(self.register([self.algorithm(owner='core', codepoints=[dict(point, value='0x0901')])]),
                          "algorithm aes-256-gcm = '0x0901' is not in a class of the Cerid algorithm identifiers")
        # A key encapsulation exchanges keys; a keyed PRF protects integrity.
        for value, function, meaning in (('0x0701', 'key-exchange', 'post-quantum key encapsulation'),
                                         ('0x0801', 'integrity', 'keyed pseudorandom function')):
            with self.subTest(value=value):
                moved = [dict(point, value=value)]
                self.assertEqual(0, self.run_guard(self.register([self.algorithm(
                    owner='core', cryptography=[function], codepoints=moved)]))[0])
                other = 'authentication' if function != 'authentication' else 'integrity'
                self.assert_fails(self.register([self.algorithm(owner='core', cryptography=[other],
                                                                codepoints=moved)]),
                                  f'algorithm aes-256-gcm ({meaning}) performs {function}, which cryptography does '
                                  'not name')
        quic = dict(self.entry()['codepoints'][0], key_bits=[128])
        self.assert_fails(self.register([self.entry(codepoints=[quic])]),
                          "codepoint v1 names an algorithm's key_bits outside the Cerid algorithm identifiers")

    def test_the_inventory_reads_the_build_and_the_register(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.tree(scratch)
            inventory = crypto_inventory.build(root)
            self.assertEqual(crypto_inventory.FORMAT, inventory['format'])
            self.assertEqual(['cerid-package', 'tool'], [export['id'] for export in inventory['exports']])
            package, tool = inventory['exports']
            self.assertEqual(('package', [], ['authentication'], False),
                             (package['kind'], package['packages'], package['functions'], package['confidentiality']))
            self.assertEqual([{'id': 'cerid.policy-generation', 'kind': 'format', 'owner': 'crypto',
                               'functions': ['authentication'], 'spec': 'docs/design/network-foundation.md',
                               'standard': False}], package['entries'])
            self.assertEqual([], package['algorithms'])  # a planned algorithm is in no export
            self.assertEqual(('program', ['zstd'], 'The tool program (tools/tool)'),
                             (tool['kind'], tool['packages'], tool['title']))
            self.assertEqual(package['entries'], tool['entries'])  # reached through DEPENDS crypto
            self.implement_algorithm(root)
            package = crypto_inventory.build(root)['exports'][0]
            self.assertEqual([{'name': 'aes-256-gcm', 'value': '0x0502', 'class': 'Aead',
                               'functions': ['confidentiality', 'integrity'], 'key_bits': [256],
                               'entry': 'crypto.aes-gcm', 'spec': 'RFC 5116', 'standard': True, 'owner': 'crypto'}],
                             package['algorithms'])
            self.assertEqual((['authentication', 'confidentiality', 'integrity'], True),
                             (package['functions'], package['confidentiality']))
            for extra, message in (('crd_module(tools/broken HOST DEPENDS nowhere)\n',
                                    'the module nowhere is depended on but not registered'),
                                   ('crd_module(tools/x NAME cerid-package)\n', 'may not be named cerid-package'),
                                   ('crd_module(tools/tool)\n', 'registers the module tool twice')):
                with self.subTest(extra=extra):
                    (root / 'CMakeLists.txt').write_text(self.CMAKE + extra, encoding='utf-8')
                    with self.assertRaisesRegex(crypto_inventory.InventoryError, message):
                        crypto_inventory.build(root)

    def test_check_names_drift(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.tree(scratch)
            self.assertEqual((0, 'wrote assets/comply/crypto-inventory.json\n'), self.run_tool(root))
            self.assertEqual((0, 'cryptography inventory current: 2 export(s)\n'), self.run_tool(root, '--check'))
            self.implement_algorithm(root)
            code, output = self.run_tool(root, '--check')
            self.assertEqual(1, code)
            self.assertIn("FAIL: assets/comply/crypto-inventory.json differs from scripts/gen-crypto-inventory.py's "
                          'output', output)
            (root / 'CMakeLists.txt').write_text(self.CMAKE + 'crd_module(tools/broken DEPENDS nowhere)\n',
                                                 encoding='utf-8')
            code, output = self.run_tool(root, '--check')
            self.assertEqual(1, code)
            self.assertIn('FAIL: the module nowhere is depended on but not registered', output)

    def test_the_guard_holds_every_export_to_its_reviewed_inventory(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.tree(scratch)
            self.assert_guard_fails(root, 'the export cerid-package has no classification; author its head record '
                                          'with its findings and review it with python scripts/gen-crypto-inventory.py '
                                          '--review cerid-package')
            self.set_classifications(root, {'cerid-package': [self.head()], 'tool': [self.head()]})
            for export in ('cerid-package', 'tool'):
                self.assertEqual(0, self.run_tool(root, '--review', export, '--day', '2026-10-31')[0])
            code, output = self.guard(root, '2026-11-01')
            self.assertEqual(0, code, output)
            self.implement_algorithm(root)  # the package and the tool now carry encryption
            self.assert_guard_fails(root, "cerid-package's cryptography inventory changed since its classification "
                                          'was reviewed on 2026-10-31; restate its findings and review them with '
                                          'python scripts/gen-crypto-inventory.py --review cerid-package')
            self.assert_guard_fails(root, "tool's cryptography inventory changed")
            for export in ('cerid-package', 'tool'):
                self.assertEqual(0, self.run_tool(root, '--review', export, '--day', '2026-11-01')[0])
            self.assertEqual(0, self.guard(root, '2026-11-01')[0])
            head = self.legal_of(root)['crypto_classifications']['tool'][-1]
            self.assertEqual(('2026-11-01', {'needed': True, 'reviewed': None}), (head['reviewed'], head['counsel']))
            (root / 'CMakeLists.txt').write_text(self.CMAKE + 'crd_module(tools/broken DEPENDS nowhere)\n',
                                                 encoding='utf-8')
            self.assert_guard_fails(root, 'crypto_classifications: the module nowhere is depended on but not '
                                          'registered')

    def test_a_head_record_answers_every_classified_obligation(self):
        eu = self.classified('eu.dual-use', instrument=self.instrument(
            jurisdiction='EU', primary='https://eur-lex.europa.eu/eli/reg/2021/821/oj'))
        cases = (
            ({}, 'the findings answer tr.btk.encryption, not the obligations whose active check is the '
                 'classification (eu.dual-use, tr.btk.encryption)', [self.legal_entry(), self.classified(), eu]),
            ({'findings': [self.finding(entry='tr.kvkk.processing')]},
             'finding tr.kvkk.processing is no export-control entry of the register', None),
            ({'findings': [self.finding(), self.finding()]}, 'finding tr.btk.encryption is stated twice', None),
            ({'findings': [self.finding(report='no')]}, 'a finding has entry, classification, authorization, report',
             None),
            ({'findings': [self.finding(basis='')]}, 'a finding has entry, classification', None),
            ({'findings': []}, 'findings classify the export against each obligation', None),
            ({'inventory': 'AB' * 32}, "inventory is the sha256 of the export's inventory", None),
            ({'reviewed': '2026-11-02'}, 'reviewed 2026-11-02 is after today', None),
            ({'counsel': {'needed': False, 'reviewed': None}},
             'tr.btk.encryption need counsel, so the classification does', None),
            ({'counsel': {'needed': False, 'reviewed': '2026-10-31'}},
             'counsel.reviewed records the review a needed flag asked for', None),
            ({'counsel': {'needed': True, 'reviewed': '2026-10-30'}},
             'counsel.reviewed 2026-10-30 is not between', None),
            ({'counsel': {'needed': 'yes', 'reviewed': None}}, 'counsel has needed and reviewed', None),
            ({'colour': 'red'}, 'a record has release, inventory, reviewed, findings and counsel', None),
        )
        for changes, message, legal_entries in cases:
            with self.subTest(message=message), tempfile.TemporaryDirectory() as scratch:
                root = self.reviewed_tree(scratch, legal_entries=legal_entries)
                classifications = self.legal_of(root)['crypto_classifications']
                classifications['tool'][-1].update(changes)
                self.set_classifications(root, classifications)
                self.assert_guard_fails(root, message)
        with tempfile.TemporaryDirectory() as scratch:
            root = self.reviewed_tree(scratch)
            self.assertEqual(0, self.guard(root, '2026-11-01')[0])
            classifications = self.legal_of(root)['crypto_classifications']
            classifications['tool'][-1]['counsel']['reviewed'] = '2026-11-01'
            self.set_classifications(root, classifications)
            self.assertEqual(0, self.guard(root, '2026-11-01')[0])
            unanswered = [review for review in self.counsel_reviews(self.legal_of(root)) if review['answered'] is None]
            self.set_classifications(root, classifications, counsel_reviews=unanswered)
            self.assert_guard_fails(root, "the classification tool records counsel's answer of 2026-11-01, but no "
                                          'review answered that day asked about it')
            classifications['tool'][-1]['counsel']['reviewed'] = None
            self.set_classifications(root, classifications, counsel_reviews=[])
            self.assert_guard_fails(root, 'the classification tool needs counsel, but no open review asks about it')

    def test_records_end_with_one_head_and_releases_are_frozen(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.reviewed_tree(scratch)
            good = self.legal_of(root)['crypto_classifications']
            head = good['tool'][-1]
            answered = dict(head, counsel={'needed': True, 'reviewed': '2026-11-01'})
            released = dict(answered, release='1.0.0', inventory='cd' * 32)
            cases = (
                ({'tool': [head, released]}, 'tool ends with exactly one head record (release null)'),
                ({'tool': [released]}, 'tool ends with exactly one head record'),
                ({'tool': [dict(released, release='1.0'), head]}, "tool's releases are distinct"),
                ({'tool': [released, released, head]}, "tool's releases are distinct"),
                ({'tool': [dict(released, counsel=head['counsel']), head]},
                 'tool 1.0.0: released before counsel reviewed its classification (a user action)'),
                ({'gone': [head]}, 'gone is no longer an export; delete its head record'),
                ({'tool': []}, 'tool holds a list of classification records'),
            )
            for changes, message in cases:
                with self.subTest(message=message):
                    self.set_classifications(root, dict(good, **changes))
                    self.assert_guard_fails(root, message)
            self.set_classifications(root, dict(good, tool=[released, head], gone=[released]))
            code, output = self.guard(root, '2026-11-01')
            self.assertEqual(0, code, output)
            self.set_classifications(root, [])
            self.assert_guard_fails(root, 'crypto_classifications maps each export to its classification records')

    def test_review_release_and_facts(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = self.reviewed_tree(scratch)
            code, output = self.run_tool(root, '--review', 'tool')
            self.assertEqual(1, code)
            self.assertIn("tool's inventory is unchanged since its review on 2026-10-31; nothing to review", output)
            code, output = self.run_tool(root, '--release', 'tool', '1.0.0')
            self.assertEqual(1, code)
            self.assertIn("tool's classification awaits its counsel review (a user action)", output)
            classifications = self.legal_of(root)['crypto_classifications']
            classifications['tool'][-1]['counsel']['reviewed'] = '2026-11-01'
            self.set_classifications(root, classifications)
            self.assertIn('is not a release version', self.run_tool(root, '--release', 'tool', 'v1')[1])
            self.assertEqual((0, 'released tool 1.0.0\n'), self.run_tool(root, '--release', 'tool', '1.0.0'))
            records = self.legal_of(root)['crypto_classifications']['tool']
            self.assertEqual(['1.0.0', None], [record['release'] for record in records])
            self.assertEqual(records[0]['inventory'], records[1]['inventory'])
            self.assertEqual(0, self.guard(root, '2026-11-01')[0])
            self.assertIn('already has a classification record for 1.0.0',
                          self.run_tool(root, '--release', 'tool', '1.0.0')[1])
            facts = json.loads(self.run_tool(root, '--facts', 'tool')[1])
            self.assertEqual((False, {'uses_non_exempt_encryption': False}, [], [self.finding()]),
                             (facts['confidentiality'], facts['apple'], facts['reports'], facts['findings']))
            self.implement_algorithm(root)
            for args in (('--facts', 'tool'), ('--release', 'tool', '1.1.0')):
                code, output = self.run_tool(root, *args)
                self.assertEqual(1, code)
                self.assertIn("tool's inventory changed since its head record was reviewed; review it first", output)
            classifications = self.legal_of(root)['crypto_classifications']
            classifications['tool'][-1]['findings'] = [self.finding(classification='5D992.c', report=True)]
            self.set_classifications(root, classifications)
            self.assertEqual(0, self.run_tool(root, '--review', 'tool', '--day', '2026-11-01')[0])
            head = self.legal_of(root)['crypto_classifications']['tool'][-1]
            self.assertIsNone(head['counsel']['reviewed'])  # a changed inventory needs counsel again
            facts = json.loads(self.run_tool(root, '--facts', 'tool')[1])
            self.assertEqual({'uses_non_exempt_encryption': True}, facts['apple'])
            self.assertEqual([{'name': 'aes-256-gcm', 'key_bits': [256], 'spec': 'RFC 5116', 'standard': True}],
                             facts['algorithms'])
            self.assertEqual([{'entry': 'tr.btk.encryption', 'classification': '5D992.c',
                               'authorization': 'None required'}], facts['reports'])
            self.assertEqual([], facts['non_standard'])
            self.assertIn('nowhere is not an export (cerid-package, tool)', self.run_tool(root, '--facts', 'nowhere')[1])
            classifications['cerid-package'] = []
            self.set_classifications(root, classifications)
            self.assertIn('cerid-package has no head record', self.run_tool(root, '--review', 'cerid-package')[1])
            classifications['cerid-package'] = [self.head(findings=[])]
            self.set_classifications(root, classifications)
            self.assertIn("cerid-package's head record states no findings",
                          self.run_tool(root, '--review', 'cerid-package')[1])


class SanctionsLists(RegisterFixture, unittest.TestCase):
    """scripts/import-sanctions.py and the legal register's sanctions_lists (COMPLY.1): the official files are read
    by format (OFAC, the EU's FSF, the UN Security Council, the UK Sanctions List) into cerid-sanctions/1 generations,
    every unknown value refused; the guard holds each list to a screening entry, an official host, a format, a bound
    age and the record of the file read."""

    LISTS = ROOT / 'tests/foundation/comply/lists'
    SCREENING = {'ref': 'tests/foundation/comply/test_sanctions.cpp', 'name': 'screening', 'state': 'active'}
    URL = 'https://www.mevzuat.gov.tr/list.xml'

    def screening_entry(self, **changes):
        return self.legal_entry(id='tr.asset-freeze', topic='privacy', check=dict(self.SCREENING), **changes)

    def sanctions(self, **changes):
        config = {'entry': 'tr.asset-freeze', 'title': 'A list', 'format': 'ofac-xml', 'url': self.URL, 'post': None,
                  'delivery': ['files.example.org'], 'max_age_days': 31,
                  'read': {'day': '2026-11-01', 'published': '2026-10-28', 'bytes': 12, 'sha256': 'ab' * 32,
                           'parties': 3}}
        config.update(changes)
        return {'about': 'fixture', 'lists': {'tr.masak': config}, 'pending': {}}

    def run_lists(self, section, entries=None, today='2026-11-01'):
        entries = entries or [self.screening_entry()]
        if isinstance(section, dict) and 'embargoes' not in section:  # the list cases: no destination embargoed
            reasons = {entry['id']: 'no embargo' for entry in entries if entry.get('check') == self.SCREENING}
            section = dict(section, embargoes={'about': 'fixture', 'generations': {}, 'reasons': reasons})
        legal = self.legal_register(entries, sanctions_lists=section)
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_tree(root, self.register([self.entry()]), legal)
            (root / 'tests/foundation/comply').mkdir(parents=True)
            (root / 'tests/foundation/comply/test_sanctions.cpp').write_text('// screening\n', encoding='utf-8')
            return self.guard(root, today)

    def assert_lists_fail(self, section, message, entries=None, today='2026-11-01'):
        code, output = self.run_lists(section, entries, today)
        self.assertEqual(1, code, output)
        self.assertIn(message, output)

    def test_a_list_on_an_official_host_with_its_read_passes(self):
        code, output = self.run_lists(self.sanctions())
        self.assertEqual(0, code, output)
        code, output = self.run_lists(self.sanctions(read=None, delivery=[]))
        self.assertEqual(0, code, output)
        instant = self.sanctions()
        instant['lists']['tr.masak']['read']['published'] = '2026-10-31T23:00:07Z'
        self.assertEqual(0, self.run_lists(instant)[0])
        self.assertEqual(0, self.run_lists({'about': 'x', 'lists': {}, 'pending': {'tr.asset-freeze': 'later'}})[0])

    def test_every_list_names_a_screening_entry_an_official_url_a_format_and_a_bounded_age(self):
        where = 'legal register: sanctions_lists: tr.masak'
        self.assert_lists_fail(None, 'legal register: sanctions_lists holds about, lists')
        self.assert_lists_fail(self.sanctions(entry='tr.kvkk.processing'),
                               f"{where}: entry 'tr.kvkk.processing' is no entry whose active check is the screening")
        self.assert_lists_fail(self.sanctions(url='https://lists.example.org/x.xml'),
                               f'{where}: https://lists.example.org/x.xml is not an https URL on an official TR host')
        self.assert_lists_fail(self.sanctions(url='http://www.mevzuat.gov.tr/list.xml'), 'not an https URL')
        self.assert_lists_fail(self.sanctions(format='csv'), f'{where}: format is one of eu-fsf-xml, gb-uksl-xml')
        self.assert_lists_fail(self.sanctions(delivery=['Files.Example.org']), f'{where}: delivery lists the hosts')
        for age in (0, 367, True, '31'):
            with self.subTest(age=age):
                self.assert_lists_fail(self.sanctions(max_age_days=age), f'{where}: max_age_days is 1 to 366')
        self.assert_lists_fail(self.sanctions(title=''), f'{where}: title names the list')
        unknown = self.sanctions()
        unknown['lists']['tr.masak']['note'] = 'x'
        self.assert_lists_fail(unknown, f'{where} has delivery, entry, format, max_age_days, post, read, title, url')
        self.assert_lists_fail({'about': 'x', 'lists': {'TR.masak': self.sanctions()['lists']['tr.masak']},
                                'pending': {}}, 'an authority is a dotted lower-case id')

    def test_the_record_of_the_file_read_is_complete_and_in_order(self):
        where = 'legal register: sanctions_lists: tr.masak'
        for key, value in (('sha256', 'AB' * 32), ('bytes', 0), ('parties', -1), ('published', 7)):
            with self.subTest(key=key):
                section = self.sanctions()
                section['lists']['tr.masak']['read'][key] = value
                self.assert_lists_fail(section, f'{where}: read is null or holds the day, publication')
        section = self.sanctions()
        section['lists']['tr.masak']['read']['published'] = '2026-11-02'
        self.assert_lists_fail(section, f'{where}: the file read on 2026-11-01 was published after it, on 2026-11-02',
                               today='2026-11-03')
        section['lists']['tr.masak']['read']['published'] = '28/10/2026'
        self.assert_lists_fail(section, f'{where}: read.published is the publication day or UTC instant')
        self.assert_lists_fail(self.sanctions(), f'{where}: read on 2026-11-01, after today', today='2026-10-31')

    def test_every_screening_entry_has_a_list_or_a_pending_reason_never_both(self):
        where = 'legal register: sanctions_lists'
        empty = {'about': 'x', 'lists': {}, 'pending': {}}
        self.assert_lists_fail(empty, f'{where}: the screening entry tr.asset-freeze has no list and no pending reason')
        both = self.sanctions()
        both['pending'] = {'tr.asset-freeze': 'later'}
        self.assert_lists_fail(both, f'{where}: tr.asset-freeze has a list, so it is not pending')
        self.assert_lists_fail({'about': 'x', 'lists': {}, 'pending': {'tr.asset-freeze': ''}},
                               f'{where}: pending tr.asset-freeze gives its reason')
        stray = self.sanctions()
        stray['pending'] = {'tr.kvkk.processing': 'x'}
        self.assert_lists_fail(stray, f"{where}: pending names 'tr.kvkk.processing', which is no screening entry")

    # The embargoed destinations, authored per authority.

    READ = {'version': '2026-10-15', 'read': '2026-10-31', 'sha256': 'cd' * 32}

    def read_screening_entry(self):
        return self.screening_entry(instrument=self.instrument(consolidated=dict(self.READ)),
                                    source={'ref': 'docs/research.md#n83', 'read': '2026-10-31', 'verified': True})

    def read_order(self, **instrument):
        """An order the screening entry's regulations incorporate, read with them."""
        return self.legal_entry(id='tr.order', instrument=self.instrument(
            name='Order 1', primary=self.KVKK.replace('6698', '6699'), consolidated=dict(self.READ, sha256='ab' * 32),
            **instrument), source={'ref': 'docs/research.md#n83', 'read': '2026-10-31', 'verified': True})

    def umbrella_embargo(self):
        """Destinations each read in its own act, under a screening entry whose instrument is a class of acts that no
        single text consolidates (the EU's Article 215 regulations)."""
        record = self.embargo()
        for destination in record['destinations']:
            destination['entry'] = 'tr.order'
        return record

    @staticmethod
    def embargo(**changes):
        def destination(region, program, measure, basis):
            return {'region': region, 'program': program, 'measure': measure, 'basis': basis,
                    'entry': 'tr.asset-freeze'}
        record = {'entry': 'tr.asset-freeze', 'title': 'Embargoes', 'max_age_days': 92,
                  'destinations': [destination('CU', 'P 515', 'comprehensive', '515.201'),
                                   destination('UA-43', 'P 589', 'comprehensive', '589.207'),
                                   destination('RU', 'P 589', 'sectoral', '589.202')],
                  'open': None}
        record.update(changes)
        return record

    def embargoes(self, record=None, reasons=None, **lists):
        section = {'about': 'fixture', 'lists': {}, 'pending': {'tr.asset-freeze': 'later'}}
        section.update(lists)
        section['embargoes'] = {'about': 'fixture', 'generations': {'tr.masak.embargo': record or self.embargo()},
                                'reasons': reasons or {}}
        return section

    def assert_embargo_fails(self, section, message):
        self.assert_lists_fail(section, message, entries=[self.read_screening_entry()])

    def test_an_authored_embargo_on_a_read_screening_entry_passes(self):
        code, output = self.run_lists(self.embargoes(), entries=[self.read_screening_entry()])
        self.assertEqual(0, code, output)
        code, output = self.run_lists(self.embargoes(self.embargo(open='The regimes read next.')),
                                      entries=[self.read_screening_entry()])
        self.assertEqual(0, code, output)
        # A destination read in an order the regulations incorporate, part of a region whose boundary no subdivision
        # draws.
        incorporated = self.embargo()
        incorporated['destinations'].append({'region': 'UA-14', 'program': 'Order 1', 'measure': 'partial',
                                             'basis': 'Section 1', 'entry': 'tr.order'})
        code, output = self.run_lists(self.embargoes(incorporated), entries=[self.read_screening_entry(),
                                                                             self.read_order()])
        self.assertEqual(0, code, output)
        # A screening entry whose instrument is a class of acts is never read itself: each destination is read in its
        # own act.
        code, output = self.run_lists(self.embargoes(self.umbrella_embargo()),
                                      entries=[self.screening_entry(), self.read_order()])
        self.assertEqual(0, code, output)

    def test_a_destination_resting_on_an_ended_measure_is_refused_from_its_end(self):
        # A measure renewed for a period rests on the renewal, whose sunset is the period's end: on that day the guard
        # and the importer ask for the next renewal.
        renewed = dict(self.read_order(), effective='2026-05-29', sunset='2026-11-02')
        record = self.umbrella_embargo()
        where = "legal register: sanctions_lists: embargoes: tr.masak.embargo: CU: entry 'tr.order' ended on 2026-11-02"
        for today, ended in (('2026-11-01', False), ('2026-11-02', True), ('2026-12-01', True)):
            code, output = self.run_lists(self.embargoes(record), entries=[self.screening_entry(), renewed],
                                          today=today)
            self.assertEqual((int(ended), ended), (code, where in output), output)
        self.assertIn('read the resolution or act that renews or replaces its measure, or drop the destination',
                      output)
        register = {'entries': [self.screening_entry(), renewed]}
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(3, sanctions_import.import_embargo('tr.masak.embargo', record, register, out,
                                                                '2026-11-01')[1])
            with self.assertRaisesRegex(sanctions_import.SanctionsError, "'tr.order' ended on 2026-11-02"):
                sanctions_import.import_embargo('tr.masak.embargo', record, register, out, '2026-11-02')

    def test_every_embargo_names_a_read_screening_entry_iso_regions_measures_and_provisions(self):
        where = 'legal register: sanctions_lists: embargoes: tr.masak.embargo'
        self.assert_lists_fail(self.embargoes(), f"{where}: CU: entry 'tr.asset-freeze' is not an instrument read "
                                                 'from its primary source')
        self.assert_embargo_fails(self.embargoes(self.embargo(entry='tr.kvkk.processing')),
                                  f"{where}: entry 'tr.kvkk.processing' is no entry whose active check is the "
                                  'screening')
        self.assert_embargo_fails(self.embargoes(self.embargo(entry='tr.none')),
                                  f"{where}: entry 'tr.none' is no entry of the register with an instrument and its "
                                  'jurisdiction')
        foreign = self.umbrella_embargo()
        self.assert_lists_fail(self.embargoes(foreign),
                               f"{where}: RU: entry 'tr.order' is an instrument of EU, not of TR",
                               entries=[self.screening_entry(), self.read_order(jurisdiction='EU')])
        unknown = self.embargo()
        unknown['note'] = 'x'
        self.assert_embargo_fails(self.embargoes(unknown), f'{where} has destinations, entry, max_age_days, open')
        self.assert_embargo_fails(self.embargoes(self.embargo(title='')), f'{where}: title names the embargoes')
        for age in (0, 367, True):
            with self.subTest(age=age):
                self.assert_embargo_fails(self.embargoes(self.embargo(max_age_days=age)),
                                          f'{where}: max_age_days is 1 to 366')
        self.assert_embargo_fails(self.embargoes(self.embargo(open='')), f'{where}: open is null or says what')
        self.assert_embargo_fails(self.embargoes(self.embargo(destinations=[])),
                                  f'{where}: destinations lists 1 to 1024 embargoed regions')
        for region in ('XX', 'Cuba', 'cu', 'UA-4321', 'UA43', 'ZZ-43'):
            with self.subTest(region=region):
                bad = self.embargo()
                bad['destinations'][0]['region'] = region
                self.assert_embargo_fails(self.embargoes(bad), f'{where}: {region} is no ISO 3166-1 country')
        bad = self.embargo()
        bad['destinations'][0]['measure'] = 'regional'
        self.assert_embargo_fails(self.embargoes(bad), f'{where}: CU: measure is comprehensive, sectoral or partial')
        for key in ('basis', 'entry'):
            with self.subTest(key=key):
                bad = self.embargo()
                bad['destinations'][0][key] = ''
                self.assert_embargo_fails(self.embargoes(bad), f'{where}: a destination has basis, entry, measure, '
                                                               'program, region')
        for entry in ('tr.order', 'tr.kvkk.processing', 'us.none'):
            with self.subTest(entry=entry):
                bad = self.embargo()
                bad['destinations'][0]['entry'] = entry
                unread = self.read_order()
                unread['source']['verified'] = False
                self.assert_lists_fail(self.embargoes(bad), f"{where}: CU: entry '{entry}' is not an instrument read "
                                                            'from its primary source',
                                       entries=[self.read_screening_entry(), unread])
        bad = self.embargo()
        bad['destinations'][0]['program'] = 'p' * 4097
        self.assert_embargo_fails(self.embargoes(bad), f'{where}: CU: a value longer than 4096 bytes')
        twice = self.embargo()
        twice['destinations'].append(dict(twice['destinations'][1], measure='sectoral'))
        self.assert_embargo_fails(self.embargoes(twice), f'{where}: UA-43 is embargoed twice under P 589')
        other = self.embargo()
        other['destinations'].append(dict(other['destinations'][1], program='P 14065'))  # two programmes: allowed
        self.assertEqual(0, self.run_lists(self.embargoes(other), entries=[self.read_screening_entry()])[0])
        self.assert_embargo_fails(self.embargoes(lists={'tr.masak.embargo': self.sanctions()['lists']['tr.masak']},
                                                 pending={}),
                                  f'{where} is the authority of an official file')
        section = self.embargoes()
        section['embargoes']['generations'] = {'TR.embargo': self.embargo()}
        self.assert_embargo_fails(section, 'embargoes: TR.embargo: an authority is a dotted lower-case id')

    def test_every_screening_entry_has_an_embargo_or_a_reason_never_both(self):
        where = 'legal register: sanctions_lists: embargoes'
        section = self.embargoes()
        section['embargoes']['generations'] = {}
        self.assert_embargo_fails(section, f'{where}: the screening entry tr.asset-freeze has no embargo generation '
                                           'and no reason')
        self.assert_embargo_fails(self.embargoes(reasons={'tr.asset-freeze': 'none'}),
                                  f'{where}: tr.asset-freeze has an embargo generation, so it needs no reason')
        section['embargoes']['reasons'] = {'tr.asset-freeze': ''}
        self.assert_embargo_fails(section, f'{where}: reasons tr.asset-freeze gives its reason')
        self.assert_embargo_fails(self.embargoes(reasons={'tr.kvkk.processing': 'x'}),
                                  f"{where}: reasons names 'tr.kvkk.processing', which is no screening entry")
        section = self.embargoes()
        del section['embargoes']['reasons']
        self.assert_embargo_fails(section, f'{where} holds about, generations')
        section = self.embargoes()
        del section['embargoes']
        section['extra'] = {}
        self.assert_embargo_fails(section, 'legal register: sanctions_lists holds about, lists')

    @staticmethod
    def import_masak(record, register, out):
        return sanctions_import.import_embargo('tr.masak.embargo', record, register, out, '2026-11-01')

    def test_the_importer_writes_an_embargo_generation_dated_by_its_reading(self):
        entry = self.read_screening_entry()
        register = {'entries': [entry]}
        record = self.embargo()
        with tempfile.TemporaryDirectory() as out:
            result = self.import_masak(record, register, out)
            self.assertEqual((20261031000000, 3, '2026-10-31'), result)
            path = Path(out) / 'tr.masak.embargo.sanctions.toml'
            text = path.read_text(encoding='utf-8')
            self.assertTrue(text.startswith('# Authored in cmake/legal-register.json from '))
            document = tomllib.loads(text)
            primary = entry['instrument']['primary']
            self.assertEqual(('tr.masak.embargo', 'TR', 20261031000000, '2026-10-31', 92, primary),
                             (document['authority'], document['jurisdiction'], document['generation'],
                              document['published'], document['max_age_days'], document['source']))
            self.assertEqual(sanctions_import.embargo_reading(record, register), document['source_sha256'])
            self.assertEqual([{key: item[key] for key in ('region', 'program', 'measure')}
                              for item in record['destinations']], document['destination'])
            self.assertNotIn('party', document)
            # The same reading is already written; a corrected one, or a new text read the same day, is the next
            # generation; a reading older than the written generation is refused.
            self.assertIsNone(self.import_masak(record, register, out)[0])
            record['destinations'][2]['measure'] = 'comprehensive'
            self.assertEqual(20261031000001,
                             self.import_masak(record, register, out)[0])
            entry['instrument']['consolidated']['sha256'] = 'ef' * 32
            self.assertEqual(20261031000002,
                             self.import_masak(record, register, out)[0])
            entry['instrument']['consolidated']['read'] = '2026-10-30'
            with self.assertRaisesRegex(sanctions_import.SanctionsError, 'older than the imported 20261031000002'):
                self.import_masak(record, register, out)
            # A destination read in an incorporated order: the generation is published on the latest reading, and a
            # new text of the order is a new reading.
            order = self.read_order()
            order['instrument']['consolidated']['read'] = '2026-11-01'
            register['entries'].append(order)
            record['destinations'].append({'region': 'UA-14', 'program': 'Order 1', 'measure': 'partial',
                                           'basis': 'Section 1', 'entry': 'tr.order'})
            self.assertEqual((20261101000000, 4, '2026-11-01'),
                             self.import_masak(record, register, out))
            document = tomllib.loads(path.read_text(encoding='utf-8'))
            self.assertEqual((primary, 'partial'), (document['source'], document['destination'][3]['measure']))
            reading = sanctions_import.embargo_reading(record, register)
            order['instrument']['consolidated']['sha256'] = '12' * 32
            self.assertNotEqual(reading, sanctions_import.embargo_reading(record, register))
            self.assertEqual(20261101000001,
                             self.import_masak(record, register, out)[0])
            unverified = self.read_screening_entry()
            unverified['source']['verified'] = False  # a consolidated text not read from its primary source
            for unread in (self.screening_entry(), unverified):
                with self.assertRaisesRegex(sanctions_import.SanctionsError, 'is not an instrument read from'):
                    self.import_masak(record, {'entries': [unread]}, out)

    def test_an_umbrella_embargo_is_read_in_its_destinations_acts_alone(self):
        screening, order = self.screening_entry(), self.read_order()
        register = {'entries': [screening, order]}
        record = self.umbrella_embargo()
        reading = sanctions_import.embargo_reading(record, register)
        self.assertEqual(['tr.order'], list(sanctions_import.embargo_texts(record, register)))
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual((20261031000000, 3, '2026-10-31'),
                             self.import_masak(record, register, out))
            document = tomllib.loads((Path(out) / 'tr.masak.embargo.sanctions.toml').read_text(encoding='utf-8'))
            self.assertEqual(('TR', screening['instrument']['primary'], reading),
                             (document['jurisdiction'], document['source'], document['source_sha256']))
            # Reading the screening entry's own text (were it one) changes nothing the destinations rest on.
            screening['instrument']['consolidated'] = dict(self.READ)
            self.assertEqual(reading, sanctions_import.embargo_reading(record, register))
            self.assertIsNone(self.import_masak(record, register, out)[0])
            order['instrument']['jurisdiction'] = 'EU'
            with self.assertRaisesRegex(sanctions_import.SanctionsError,
                                        "'tr.order' is an instrument of EU, not of TR"):
                self.import_masak(record, register, out)
            with self.assertRaisesRegex(sanctions_import.SanctionsError, "entry 'tr.none' is no entry of the register"):
                self.import_masak(dict(record, entry='tr.none'), register, out)

    def test_the_us_embargo_is_written_from_31_cfr_chapter_v(self):
        register = sanctions_import.load_register(ROOT)
        with tempfile.TemporaryDirectory() as out:
            with patch('sys.stdout', new_callable=io.StringIO) as printed:
                self.assertEqual(0, sanctions_import.main(['import', '--from', str(self.LISTS), '--out', out,
                                                           '--authority', 'us.ofac.embargo']))
            self.assertIn('us.ofac.embargo: 10 embargoed destinations read 2026-11-01: generation 20261101000000',
                          printed.getvalue())
            document = tomllib.loads((Path(out) / 'us.ofac.embargo.sanctions.toml').read_text(encoding='utf-8'))
        self.assertEqual(('US', register['entries'][[e['id'] for e in register['entries']].index('us.sanctions')]
                          ['instrument']['primary']), (document['jurisdiction'], document['source']))
        self.assertEqual([('CU', '31 CFR 515', 'comprehensive'), ('IR', '31 CFR 560', 'comprehensive'),
                          ('KP', '31 CFR 510', 'comprehensive'), ('UA-43', '31 CFR 589', 'comprehensive'),
                          ('UA-40', '31 CFR 589', 'comprehensive'), ('UA-14', 'E.O. 14065', 'partial'),
                          ('UA-09', 'E.O. 14065', 'partial'), ('RU', '31 CFR 589', 'sectoral'),
                          ('RU', 'E.O. 14071', 'sectoral'), ('VE', '31 CFR 591', 'sectoral')],
                         [(item['region'], item['program'], item['measure']) for item in document['destination']])

    def test_the_eu_embargo_is_written_from_the_article_215_regulations(self):
        register = sanctions_import.load_register(ROOT)
        record = register['sanctions_lists']['embargoes']['generations']['eu.council.embargo']
        with tempfile.TemporaryDirectory() as out:
            with patch('sys.stdout', new_callable=io.StringIO) as printed:
                self.assertEqual(0, sanctions_import.main(['import', '--from', str(self.LISTS), '--out', out,
                                                           '--authority', 'eu.council.embargo']))
            self.assertIn('eu.council.embargo: 25 embargoed destinations read 2026-11-01: generation 20261101000000',
                          printed.getvalue())
            document = tomllib.loads((Path(out) / 'eu.council.embargo.sanctions.toml').read_text(encoding='utf-8'))
        # The screening entry is the class of Article 215 regulations. Its own text, the EU Sanctions Map's regime
        # index, enumerates the regulations to read and is not part of the reading: the reading is the 21 regulations
        # its destinations are read in.
        self.assertEqual(('EU', 'https://www.sanctionsmap.eu/api/v1/regime'),
                         (document['jurisdiction'], document['source']))
        umbrella = sanctions_import.read_instrument(register, 'eu.restrictive-measures')
        self.assertEqual('55 regimes, 26 attached to a place', umbrella['consolidated']['version'])
        texts = list(sanctions_import.embargo_texts(record, register))
        self.assertEqual(['eu.reg-1183-2005.drc', 'eu.reg-1412-2006.lebanon', 'eu.reg-147-2003.somalia',
                          'eu.reg-2015-735.south-sudan', 'eu.reg-2016-44.libya', 'eu.reg-2017-1509.dprk',
                          'eu.reg-2017-2063.venezuela', 'eu.reg-2022-2309.haiti', 'eu.reg-2022-263.occupied-oblasts',
                          'eu.reg-2023-1529.iran-military-support', 'eu.reg-2024-1485.russia-human-rights',
                          'eu.reg-224-2014.central-african-republic',
                          'eu.reg-267-2012.iran', 'eu.reg-314-2004.zimbabwe', 'eu.reg-359-2011.iran-human-rights',
                          'eu.reg-36-2012.syria', 'eu.reg-401-2013.myanmar', 'eu.reg-692-2014.crimea',
                          'eu.reg-747-2014.sudan', 'eu.reg-765-2006.belarus', 'eu.reg-833-2014.russia'], texts)
        # A regulation read whose measures attach only to listed persons (the EU financial sanctions file screens
        # them) or restrict no supply to a place (Iraq's cultural property and claims, the blocking statute) is a text
        # read without a destination, so not in the reading.
        for no_destination in ('eu.reg-1210-2003.iraq', 'eu.reg-3541-92.iraq-claims', 'eu.reg-356-2010.somalia',
                               'eu.reg-1352-2014.yemen', 'eu.reg-753-2011.afghanistan',
                               'eu.reg-2271-96.blocking-statute'):
            self.assertIsNotNone(sanctions_import.read_instrument(register, no_destination))
            self.assertNotIn(no_destination, texts)
        self.assertIsNone(record['open'])
        self.assertEqual([('RU', 'Regulation (EU) No 833/2014', 'sectoral'),
                          ('RU', 'Regulation (EU) 2024/1485', 'sectoral'),
                          ('BY', 'Regulation (EC) No 765/2006', 'sectoral'),
                          ('UA-43', 'Regulation (EU) No 692/2014', 'sectoral'),
                          ('UA-40', 'Regulation (EU) No 692/2014', 'sectoral'),
                          ('UA-14', 'Regulation (EU) 2022/263', 'partial'),
                          ('UA-65', 'Regulation (EU) 2022/263', 'partial'),
                          ('UA-09', 'Regulation (EU) 2022/263', 'partial'),
                          ('UA-23', 'Regulation (EU) 2022/263', 'partial'),
                          ('IR', 'Regulation (EU) No 267/2012', 'sectoral'),
                          ('IR', 'Regulation (EU) No 359/2011', 'sectoral'),
                          ('IR', 'Regulation (EU) 2023/1529', 'sectoral'),
                          ('KP', 'Regulation (EU) 2017/1509', 'sectoral'),
                          ('SY', 'Regulation (EU) No 36/2012', 'sectoral'),
                          ('MM', 'Regulation (EU) No 401/2013', 'sectoral'),
                          ('VE', 'Regulation (EU) 2017/2063', 'sectoral'),
                          ('LY', 'Regulation (EU) 2016/44', 'sectoral'),
                          ('LB', 'Regulation (EC) No 1412/2006', 'sectoral'),
                          ('SO', 'Regulation (EC) No 147/2003', 'sectoral'),
                          ('SS', 'Regulation (EU) 2015/735', 'sectoral'),
                          ('SD', 'Regulation (EU) No 747/2014', 'sectoral'),
                          ('CF', 'Regulation (EU) No 224/2014', 'sectoral'),
                          ('CD', 'Regulation (EC) No 1183/2005', 'sectoral'),
                          ('ZW', 'Regulation (EC) No 314/2004', 'sectoral'),
                          ('HT', 'Regulation (EU) 2022/2309', 'sectoral')],
                         [(item['region'], item['program'], item['measure']) for item in document['destination']])

    def test_the_uk_embargo_is_written_from_the_regime_regulations(self):
        register = sanctions_import.load_register(ROOT)
        record = register['sanctions_lists']['embargoes']['generations']['gb.fcdo.embargo']
        with tempfile.TemporaryDirectory() as out:
            with patch('sys.stdout', new_callable=io.StringIO) as printed:
                self.assertEqual(0, sanctions_import.main(['import', '--from', str(self.LISTS), '--out', out,
                                                           '--authority', 'gb.fcdo.embargo']))
            self.assertIn('gb.fcdo.embargo: 40 embargoed destinations read 2026-11-01: generation 20261101000000',
                          printed.getvalue())
            document = tomllib.loads((Path(out) / 'gb.fcdo.embargo.sanctions.toml').read_text(encoding='utf-8'))
        # The screening entry is the class of regulations made under the Sanctions and Anti-Money Laundering Act 2018.
        # Its own text, the FCDO's collection of the current regimes, enumerates the regulations to read; the reading
        # is the 20 instruments its destinations are read in, the Export Control Order 2008 among them.
        collection = 'https://www.gov.uk/government/collections/uk-sanctions-regimes-under-the-sanctions-act'
        self.assertEqual(('GB', collection), (document['jurisdiction'], document['source']))
        umbrella = sanctions_import.read_instrument(register, 'gb.sanctions')
        self.assertEqual('71 documents: 53 by country, 18 by theme, 1 listed unpublished',
                         umbrella['consolidated']['version'])
        texts = list(sanctions_import.embargo_texts(record, register))
        self.assertEqual(['gb.si-2008-3231.export-control-order', 'gb.si-2019-135.venezuela',
                          'gb.si-2019-411.dprk', 'gb.si-2019-433.drc', 'gb.si-2019-438.south-sudan',
                          'gb.si-2019-461.iran-nuclear', 'gb.si-2019-600.belarus', 'gb.si-2019-604.zimbabwe',
                          'gb.si-2019-792.syria', 'gb.si-2019-855.russia', 'gb.si-2020-1665.libya',
                          'gb.si-2020-612.lebanon', 'gb.si-2020-616.central-african-republic', 'gb.si-2020-642.somalia',
                          'gb.si-2020-707.iraq', 'gb.si-2020-753.sudan', 'gb.si-2021-496.myanmar',
                          'gb.si-2022-1281.haiti', 'gb.si-2023-1314.iran'], texts)
        # A regulation read whose measures attach only to designated persons (the UK Sanctions List screens them) or
        # restrict no supply to a place (Syria's cultural property) is a text read without a destination.
        for no_destination in ('gb.si-2020-948.afghanistan', 'gb.si-2020-608.bosnia-and-herzegovina',
                               'gb.si-2019-1145.guinea', 'gb.si-2019-554.guinea-bissau',
                               'gb.si-2020-617.lebanon-hariri', 'gb.si-2020-705.mali', 'gb.si-2020-610.nicaragua', 'gb.si-2020-1278.yemen',
                               'gb.si-2020-1233.syria-cultural-property'):
            self.assertIsNotNone(sanctions_import.read_instrument(register, no_destination))
            self.assertNotIn(no_destination, texts)
        self.assertIsNone(record['open'])
        self.assertNotIn('gb.sanctions', register['sanctions_lists']['embargoes']['reasons'])
        destinations = [(item['region'], item['program'], item['measure']) for item in document['destination']]
        regulations = [('RU', 'S.I. 2019/855', 'sectoral'), ('UA-43', 'S.I. 2019/855', 'sectoral'),
                       ('UA-40', 'S.I. 2019/855', 'sectoral'), ('UA-14', 'S.I. 2019/855', 'partial'),
                       ('UA-65', 'S.I. 2019/855', 'partial'), ('UA-09', 'S.I. 2019/855', 'partial'),
                       ('UA-23', 'S.I. 2019/855', 'partial'), ('BY', 'S.I. 2019/600', 'sectoral'),
                       ('KP', 'S.I. 2019/411', 'sectoral'), ('IR', 'S.I. 2023/1314', 'sectoral'),
                       ('IR', 'S.I. 2019/461', 'sectoral'), ('IQ', 'S.I. 2020/707', 'sectoral'),
                       ('LB', 'S.I. 2020/612', 'sectoral'), ('LY', 'S.I. 2020/1665', 'sectoral'),
                       ('MM', 'S.I. 2021/496', 'sectoral'), ('SO', 'S.I. 2020/642', 'sectoral'),
                       ('SS', 'S.I. 2019/438', 'sectoral'), ('SD', 'S.I. 2020/753', 'sectoral'),
                       ('SY', 'S.I. 2019/792', 'sectoral'), ('VE', 'S.I. 2019/135', 'sectoral'),
                       ('ZW', 'S.I. 2019/604', 'sectoral'), ('HT', 'S.I. 2022/1281', 'sectoral'),
                       ('CF', 'S.I. 2020/616', 'sectoral'), ('CD', 'S.I. 2019/433', 'sectoral')]
        # The Export Control Order's embargoed destinations (Schedule 4 Parts 1 and 2) restrict the supply of goods
        # subject to trade controls to each, China, Hong Kong and Macao included.
        embargoed = [(region, 'S.I. 2008/3231', 'sectoral') for region in (
            'KP', 'IR', 'BY', 'MM', 'CF', 'CN', 'CD', 'HK', 'LB', 'LY', 'MO', 'RU', 'SS', 'SD', 'VE', 'ZW')]
        self.assertEqual(regulations + embargoed, destinations)

    def test_the_un_embargo_is_written_from_the_resolutions(self):
        register = sanctions_import.load_register(ROOT)
        record = register['sanctions_lists']['embargoes']['generations']['un.sc.embargo']
        with tempfile.TemporaryDirectory() as out:
            with patch('sys.stdout', new_callable=io.StringIO) as printed:
                self.assertEqual(0, sanctions_import.main(['import', '--from', str(self.LISTS), '--out', out,
                                                           '--authority', 'un.sc.embargo']))
            self.assertIn('un.sc.embargo: 15 embargoed destinations read 2026-11-01: generation 20261101000000',
                          printed.getvalue())
            document = tomllib.loads((Path(out) / 'un.sc.embargo.sanctions.toml').read_text(encoding='utf-8'))
        self.assertEqual(('INT', 'https://scsanctions.un.org/resources/xml/en/consolidated.xml'),
                         (document['jurisdiction'], document['source']))
        # The screening entry's own text is the Consolidated List, whose regimes index the resolutions to read; each
        # destination is read in the resolution in force, a renewal (whose sunset ends the period) where the Council
        # renews the measure for one.
        self.assertEqual('14 regimes', sanctions_import.read_instrument(register, 'int.un-sanctions')['consolidated'][
            'version'])
        texts = sanctions_import.embargo_texts(record, register)
        self.assertEqual(['un.res-1701-2006.lebanon', 'un.res-1718-2006.dprk', 'un.res-1970-2011.libya',
                          'un.res-2231-2015.iran', 'un.res-2806-2025.somalia',
                          'un.res-2821-2026.south-sudan-renewal', 'un.res-2825-2026.drc-renewal',
                          'un.res-2827-2026.central-african-republic-renewal', 'un.res-2830-2026.sudan-renewal',
                          'un.res-2831-2026.haiti-renewal', 'un.res-687-1991.iraq-arms'], sorted(texts))
        sunsets = {entry_id: sanctions_import.entry_of(register, entry_id)['sunset'] for entry_id in texts}
        self.assertEqual({'un.res-2821-2026.south-sudan-renewal': '2027-05-31',
                          'un.res-2825-2026.drc-renewal': '2027-07-01',
                          'un.res-2827-2026.central-african-republic-renewal': '2027-07-31',
                          'un.res-2830-2026.sudan-renewal': '2026-12-11',
                          'un.res-2831-2026.haiti-renewal': '2027-10-14'},
                         {entry_id: sunset for entry_id, sunset in sunsets.items() if sunset})
        # The DPRK's measures rest on resolution 1718 (2006) as the resolutions after it extend them; Iran's on
        # resolutions 1737 (2006) to 1929 (2010), applied again from 28 September 2025 under resolution 2231 (2015).
        self.assertEqual([('CD', 'S/RES/1807 (2008)'), ('CF', 'S/RES/2745 (2024)'), ('HT', 'S/RES/2794 (2025)'),
                          ('IQ', 'S/RES/687 (1991)'), ('IR', 'S/RES/1737 (2006)'), ('KP', 'S/RES/1718 (2006)'),
                          ('LB', 'S/RES/1701 (2006)'), ('LY', 'S/RES/1970 (2011)'),
                          ('SD-DC', 'S/RES/1556 (2004)'), ('SD-DE', 'S/RES/1556 (2004)'),
                          ('SD-DN', 'S/RES/1556 (2004)'), ('SD-DS', 'S/RES/1556 (2004)'),
                          ('SD-DW', 'S/RES/1556 (2004)'), ('SO', 'S/RES/2713 (2023)'), ('SS', 'S/RES/2428 (2018)')],
                         [(item['region'], item['program']) for item in document['destination']])
        self.assertEqual({'sectoral'}, {item['measure'] for item in document['destination']})
        # A resolution read whose measures attach only to the persons listed (the Consolidated List screens them), or
        # whose text the destinations rest on beside the one in force, is read without a destination of its own.
        for no_destination in ('un.res-1267-1999.isil-al-qaida', 'un.res-1988-2011.taliban',
                               'un.res-2048-2012.guinea-bissau', 'un.res-2140-2014.yemen', 'un.res-1518-2003.iraq',
                               'un.res-1483-2003.iraq-trade', 'un.res-1546-2004.iraq-government',
                               'un.res-1957-2010.iraq-wmd', 'un.res-1533-2004.drc', 'un.res-1807-2008.drc-arms',
                               'un.res-1591-2005.sudan', 'un.res-1556-2004.darfur-arms',
                               'un.res-2127-2013.central-african-republic',
                               'un.res-2745-2024.central-african-republic-armed-groups', 'un.res-2653-2022.haiti',
                               'un.res-2794-2025.haiti-arms', 'un.res-2713-2023.al-shabaab',
                               'un.res-2206-2015.south-sudan', 'un.res-2428-2018.south-sudan-arms',
                               'un.res-1874-2009.dprk-arms', 'un.res-2094-2013.dprk-finance', 'un.res-2270-2016.dprk',
                               'un.res-2321-2016.dprk', 'un.res-2375-2017.dprk', 'un.res-2397-2017.dprk',
                               'un.res-1737-2006.iran', 'un.res-1747-2007.iran-arms-exports', 'un.res-1803-2008.iran',
                               'un.res-1929-2010.iran-arms'):
            self.assertIsNotNone(sanctions_import.read_instrument(register, no_destination))
            self.assertNotIn(no_destination, texts)
        self.assertNotIn('int.un-sanctions', register['sanctions_lists']['embargoes']['reasons'])
        # With the DPRK and Iran regimes read nothing is open, so the generation is written.
        self.assertIsNone(record['open'])
        # Iran's measures apply again under paragraph 12, never as their own adoption: the reapplied resolutions are
        # in force from that day, and resolution 2231 (2015) has no end, since paragraph 9 kept paragraph 8's
        # termination from occurring.
        self.assertIsNone(sanctions_import.entry_of(register, 'un.res-2231-2015.iran')['sunset'])
        for reapplied in ('un.res-1737-2006.iran', 'un.res-1747-2007.iran-arms-exports', 'un.res-1803-2008.iran',
                          'un.res-1929-2010.iran-arms'):
            self.assertEqual('2025-09-28', sanctions_import.entry_of(register, reapplied)['effective'])

    def test_an_open_reading_is_never_written(self):
        register = {'entries': [self.screening_entry(), self.read_order()]}
        record = dict(self.umbrella_embargo(), open='The regimes read next.')
        with tempfile.TemporaryDirectory() as out:
            with self.assertRaisesRegex(sanctions_import.OpenReading, '^3 embargoed destinations authored, not written '
                                        'while the reading is open: The regimes read next.$'):
                self.import_masak(record, register, out)
            self.assertEqual([], list(Path(out).iterdir()))
            # A malformed open reading is refused for what it is first.
            with self.assertRaisesRegex(sanctions_import.SanctionsError, 'max_age_days is 1 to'):
                self.import_masak(dict(record, max_age_days=0), register, out)
            # Closed, the same reading is written.
            self.assertEqual(3, self.import_masak(dict(record, open=None), register, out)[1])

    # The importer, on the committed excerpts of the official files.

    def imported(self, authority, out):
        register = sanctions_import.load_register(ROOT)
        config = register['sanctions_lists']['lists'][authority]
        record, body = sanctions_import.fetched(self.LISTS, authority, config)
        result = sanctions_import.import_list(authority, config, sanctions_import.jurisdiction_of(register, config),
                                              record, body, out)
        return result, tomllib.loads((Path(out) / f'{authority}.sanctions.toml').read_text(encoding='utf-8'))

    @staticmethod
    def party(document, party_id):
        return next(party for party in document['party'] if party['id'] == party_id)

    def test_each_official_format_imports_its_parties_births_countries_and_identifiers(self):
        with tempfile.TemporaryDirectory() as out:
            (generation, parties, published), sdn = self.imported('us.ofac.sdn', out)
            self.assertEqual((20261028000000, 8, '2026-10-28'), (generation, parties, published))
            self.assertEqual('cerid-sanctions/1', sdn['format'])
            self.assertEqual(('us.ofac.sdn', 'US', 31), (sdn['authority'], sdn['jurisdiction'], sdn['max_age_days']))
            self.assertEqual('https://sanctionslistservice.ofac.treas.gov/api/PublicationPreview/exports/SDN.XML',
                             sdn['source'])
            bank = self.party(sdn, 'us.ofac.sdn.306')
            self.assertEqual(('entity', ['BANCO NACIONAL DE CUBA', 'BNC', 'NATIONAL BANK OF CUBA'], ['CUBA'], ''),
                             (bank['kind'], bank['names'], bank['programs'], bank['listed']))
            self.assertEqual(['CH', 'ES', 'JP', 'PA'], bank['countries'])
            khorasan = self.party(sdn, 'us.ofac.sdn.18647')
            self.assertEqual(134, len(khorasan['identifiers']))
            self.assertTrue(all(item.startswith('crypto-') for item in khorasan['identifiers']))
            vessel = self.party(sdn, 'us.ofac.sdn.4249')
            self.assertEqual(('vessel', ['CU'], ['imo:7206512', 'call-sign:CL2059']),
                             (vessel['kind'], vessel['countries'], vessel['identifiers']))
            self.assertEqual(['1961-01-01/1962-12-31'], self.party(sdn, 'us.ofac.sdn.15962')['births'])
            self.assertEqual([], self.party(sdn, 'us.ofac.sdn.7782')['births'])  # circa: no period
            self.assertEqual(['passport.tr:315408', 'driving-licence.tr:11550'],
                             self.party(sdn, 'us.ofac.sdn.10589')['identifiers'])
            self.assertEqual(['UA'], self.party(sdn, 'us.ofac.sdn.18961')['countries'])  # Region: Crimea
            self.assertEqual(['aircraft-serial:391', 'aircraft-tail:EP-MMH'],
                             self.party(sdn, 'us.ofac.sdn.18150')['identifiers'])

            (generation, parties, published), eu = self.imported('eu.fsf', out)
            self.assertEqual((20261023150543, 7, '2026-10-23T15:05:43Z'), (generation, parties, published))
            self.assertEqual('2026-10-23', eu['published'])
            self.assertEqual(['1924-07-31/1925-07-23', '1964-03-18/1965-03-22', '1965', '1964'],
                             self.party(eu, 'eu.fsf.127388')['births'])
            self.assertEqual(['1900/1980'], self.party(eu, 'eu.fsf.117053')['births'])
            asiri = self.party(eu, 'eu.fsf.6177')
            self.assertEqual(['1982-04-19', '1982-04-18'], asiri['births'])
            self.assertEqual(['national-id.sa:1028745097', 'passport.sa:F654645'], asiri['identifiers'])
            self.assertEqual('2003-07-08', self.party(eu, 'eu.fsf.13')['listed'])

            (generation, parties, published), un = self.imported('un.sc', out)
            self.assertEqual((20261031230007, 7), (generation, parties))
            hijri = self.party(un, 'un.sc.qdi.291')
            self.assertIn('1982-04-17/1982-04-21', hijri['births'])
            self.assertIn('Ibrahim Hassan Tali Asiri (إبراهيم حسن '
                          'طالع عسيري)', hijri['names'])
            self.assertEqual('2015-07-01', self.party(un, 'un.sc.ssi.001')['listed'])  # printed with an offset
            self.assertEqual(['1973/1974'], self.party(un, 'un.sc.cdi.030')['births'])
            self.assertEqual('entity', self.party(un, 'un.sc.kpe.001')['kind'])

            (generation, parties, published), uk = self.imported('gb.fcdo', out)
            self.assertEqual((20261030000000, 6), (generation, parties))
            self.assertEqual(['1900/1999'], self.party(uk, 'gb.fcdo.rus2701')['births'])  # 15/08/19yy
            self.assertEqual(['1975'], self.party(uk, 'gb.fcdo.rus1683')['births'])  # 00/00/1975
            self.assertEqual(['email:S@synesis.y', 'website:https://synesis.partners', 'registration:190950894',
                              'registration:7704734000', 'registration:770301001'],
                             self.party(uk, 'gb.fcdo.bel0065')['identifiers'])
            self.assertEqual(('vessel', ['KM'], ['imo:9562233']),
                             tuple(self.party(uk, 'gb.fcdo.dpr0075')[key] for key in ('kind', 'countries',
                                                                                       'identifiers')))

    def test_generations_count_publications_and_refuse_older_ones(self):
        generation_of = sanctions_import.generation_of
        self.assertEqual(20261028000000, generation_of('2026-10-28', None, 'a', 'x'))
        self.assertEqual(20261031230007, generation_of('2026-10-31T23:00:07Z', None, 'a', 'x'))
        self.assertIsNone(generation_of('2026-10-28', (20261028000000, 'a'), 'a', 'x'))
        self.assertEqual(20261028000001, generation_of('2026-10-28', (20261028000000, 'a'), 'b', 'x'))
        self.assertEqual(20261028000002, generation_of('2026-10-28', (20261028000001, 'b'), 'c', 'x'))
        self.assertEqual(20261029000000, generation_of('2026-10-29', (20261028000001, 'b'), 'c', 'x'))
        for published, previous in (('2026-10-27', (20261028000000, 'a')),
                                    ('2026-10-31T23:00:07Z', (20261031230007, 'a')),
                                    ('2026-10-31T22:00:00Z', (20261031230007, 'a'))):
            with self.subTest(published=published):
                with self.assertRaisesRegex(sanctions_import.SanctionsError, 'older than the imported'):
                    generation_of(published, previous, 'b', 'x')
        with tempfile.TemporaryDirectory() as out:
            (first, _, _), _ = self.imported('gb.fcdo', out)
            before = (Path(out) / 'gb.fcdo.sanctions.toml').read_bytes()
            (again, _, _), _ = self.imported('gb.fcdo', out)
            self.assertIsNone(again)  # the same file: already imported, left as it is
            self.assertEqual(before, (Path(out) / 'gb.fcdo.sanctions.toml').read_bytes())
            (Path(out) / 'gb.fcdo.sanctions.toml').write_text('generation = 1\n', encoding='utf-8')
            with self.assertRaisesRegex(sanctions_import.SanctionsError, 'not a generation'):
                self.imported('gb.fcdo', out)
            self.assertEqual(20261030000000, first)

    def mutated(self, authority, old, new):
        body = (self.LISTS / f'{authority}.xml').read_bytes()
        self.assertIn(old, body)
        return body.replace(old, new, 1)

    def read(self, authority, body):
        register = sanctions_import.load_register(ROOT)
        config = register['sanctions_lists']['lists'][authority]
        return sanctions_import.READERS[config['format']].read(body, authority, None, config)

    def test_every_unknown_value_refuses_the_import(self):
        cases = [
            ('us.ofac.sdn', b'<idType>Passport</idType>', b'<idType>Ration Card</idType>',
             "the identifier type 'Ration Card' is not in"),
            ('us.ofac.sdn', b'<country>Switzerland</country>', b'<country>Atlantis</country>',
             "the country 'Atlantis' is not in"),
            ('us.ofac.sdn', b'<sdnType>Vessel</sdnType>', b'<sdnType>Submarine</sdnType>', "has the type 'Submarine'"),
            ('us.ofac.sdn', b'<Record_Count>8</Record_Count>', b'<Record_Count>9</Record_Count>',
             '8 entries, but the file says 9'),
            ('us.ofac.sdn', b'<dateOfBirth>circa ', b'<dateOfBirth>about ', 'the date of birth'),
            ('us.ofac.sdn', b'<Publish_Date>10/28/2026</Publish_Date>', b'<Publish_Date>2026-10-28</Publish_Date>',
             'the publish date'),
            ('eu.fsf', b'calendarType="ISLAMIC"', b'calendarType="HEBREW"', "the calendar 'HEBREW'"),
            ('eu.fsf', b'identificationTypeCode="imo"', b'identificationTypeCode="vin"',
             "the identification type 'vin' is not in"),
            ('eu.fsf', b'countryIso2Code="IQ"', b'countryIso2Code="Iraq"', "the country code 'Iraq'"),
            ('eu.fsf', b'<subjectType code="person"', b'<subjectType code="group"', 'has no known subject type'),
            ('un.sc', b'<TYPE_OF_DATE>BETWEEN</TYPE_OF_DATE>', b'<TYPE_OF_DATE>CIRCA</TYPE_OF_DATE>',
             "the birth type 'CIRCA'"),
            ('un.sc', b'(Hijri Calendar)', b'(Coptic Calendar)', 'the birth note'),
            ('un.sc', b'<TYPE_OF_DOCUMENT>Passport</TYPE_OF_DOCUMENT>',
             b'<TYPE_OF_DOCUMENT>Library card</TYPE_OF_DOCUMENT>', "the document type 'Library card' is not in"),
            ('gb.fcdo', b'<IndividualEntityShip>Ship</IndividualEntityShip>',
             b'<IndividualEntityShip>Plane</IndividualEntityShip>', "is a 'Plane'"),
            ('gb.fcdo', b'<DOB>dd/mm/', b'<DOB>12/mm/', 'the date of birth'),
            ('gb.fcdo', b'<DateGenerated>30/10/2026</DateGenerated>', b'<DateGenerated>2026</DateGenerated>',
             'the generation day'),
        ]
        for authority, old, new, message in cases:
            with self.subTest(authority=authority, new=new):
                with self.assertRaisesRegex(sanctions_import.SanctionsError, re.escape(message)):
                    self.read(authority, self.mutated(authority, old, new))
        for body, message in ((b'<!DOCTYPE x [<!ENTITY a "b">]><x/>', 'a document type declaration'),
                              (b'<!-- padding -->' * 8192 + b'<!DOCTYPE x SYSTEM "https://e.example/x.dtd"><x/>',
                               'a document type declaration'),
                              (b'<sdnList><unclosed>', 'not well-formed XML'), (b'<other/>', "not OFAC's sdnList")):
            with self.subTest(body=body):
                with self.assertRaisesRegex(sanctions_import.SanctionsError, re.escape(message)):
                    self.read('us.ofac.sdn', body)
        # OFAC's dated directive and programme notes are a family of attributes, read and never compared.
        for note in (b'Listing Date (EO 14024 Directive 9):', b'Executive Order 14999 information:'):
            with self.subTest(note=note):
                _, parties = self.read('us.ofac.sdn', self.mutated('us.ofac.sdn', b'<idType>Passport</idType>',
                                                                   b'<idType>' + note + b'</idType>'))
                duzcan = next(party for party in parties if party.id == 'us.ofac.sdn.10589')
                self.assertEqual(['driving-licence.tr:11550'], duzcan.identifiers)
        with patch.object(sanctions_import, 'MAX_IDENTIFIERS', 133):
            with self.assertRaisesRegex(sanctions_import.SanctionsError, '134 identifiers, more than the 133'):
                self.read('us.ofac.sdn', (self.LISTS / 'us.ofac.sdn.xml').read_bytes())
        twice = self.mutated('us.ofac.sdn', b'<uid>4249</uid>', b'<uid>306</uid>')
        register = sanctions_import.load_register(ROOT)
        config = register['sanctions_lists']['lists']['us.ofac.sdn']
        with tempfile.TemporaryDirectory() as out:
            with self.assertRaisesRegex(sanctions_import.SanctionsError, 'us.ofac.sdn.306 is listed twice'):
                sanctions_import.import_list('us.ofac.sdn', config, 'US', None, twice, out)

    def test_the_bounds_are_the_decoders(self):
        """A file the decoder would refuse is refused by the importer, naming the party, never written."""
        source = ''.join((ROOT / f'engine/foundation/comply/include/crd/comply/{name}.hpp').read_text(encoding='utf-8')
                         for name in ('sanctions', 'generation'))
        constants = {name: int(value) for name, value in
                     re.findall(r'inline constexpr \w+ (kMax\w+) = (\d+)U;', source)}
        self.assertEqual({'kMaxSanctionsBytes': sanctions_import.FILE_LIMIT,
                          'kMaxParties': sanctions_import.MAX_PARTIES,
                          'kMaxPartyNames': sanctions_import.MAX_NAMES, 'kMaxPartyItems': sanctions_import.MAX_ITEMS,
                          'kMaxPartyIdentifiers': sanctions_import.MAX_IDENTIFIERS,
                          'kMaxTextBytes': sanctions_import.MAX_TEXT, 'kMaxListAgeDays': registers.MAX_LIST_AGE_DAYS},
                         {name: constants[name] for name in ('kMaxSanctionsBytes', 'kMaxParties', 'kMaxPartyNames',
                                                             'kMaxPartyItems', 'kMaxPartyIdentifiers', 'kMaxTextBytes',
                                                             'kMaxListAgeDays')})

    def test_calendars_and_periods(self):
        self.assertEqual('1982-04-17/1982-04-21', sanctions_import.hijri_period(24, 6, 1402, 'x'))
        self.assertEqual(['1924-07-31/1925-07-23', '1964-03-18/1965-03-22'],
                         sanctions_import.hijri_year_periods(1343, 'x'))
        self.assertEqual('1965-03/1965', sanctions_import.interval('1965-03', '1965', 'x'))
        self.assertEqual('1965', sanctions_import.interval('1965', '1965', 'x'))
        with self.assertRaisesRegex(sanctions_import.SanctionsError, 'ends before it starts'):
            sanctions_import.interval('1966', '1965-12', 'x')
        self.assertEqual((datetime.date(2024, 2, 1), datetime.date(2024, 2, 29)), sanctions_import.bounds('2024-02'))
        self.assertEqual('1962-02-17', sanctions_import.ofac_birth('17 Feb 1962', 'x'))
        self.assertEqual('1962-02', sanctions_import.ofac_birth('Feb 1962', 'x'))
        self.assertEqual('1961-01-01/1962-12-31', sanctions_import.ofac_birth('01 Jan 1961 to 31 Dec 1962', 'x'))
        approximate = sanctions_import.APPROXIMATE
        self.assertEqual(approximate, sanctions_import.ofac_birth('circa 1958', 'x'))
        # An approximate date (circa) is APPROXIMATE: the party then records no period, so no birth contradicts it.
        circa = ET.fromstring('<birthdate circa="true" calendarType="GREGORIAN" year="1960"/>')
        self.assertEqual(approximate, sanctions_import.eu_birth(circa, 'x'))
        circa.set('circa', 'false')
        self.assertEqual(['1960'], sanctions_import.eu_birth(circa, 'x'))
        about = ET.fromstring('<D><TYPE_OF_DATE>APPROXIMATELY</TYPE_OF_DATE><YEAR>1960</YEAR></D>')
        self.assertEqual(approximate, sanctions_import.un_birth(about, 'x'))
        about.find('TYPE_OF_DATE').text = 'EXACT'
        self.assertEqual('1960', sanctions_import.un_birth(about, 'x'))
        self.assertEqual('1975', sanctions_import.uk_birth('00/00/1975', 'x'))
        self.assertEqual('1968-11', sanctions_import.uk_birth('dd/11/1968', 'x'))
        for bad in ('12/mm/1968', '31/02/1968', '1968-11-12'):
            with self.subTest(bad=bad):
                with self.assertRaises(sanctions_import.SanctionsError):
                    sanctions_import.uk_birth(bad, 'x')

    def test_a_fetched_file_must_be_the_one_its_record_names_and_record_writes_the_read(self):
        with tempfile.TemporaryDirectory() as scratch:
            root, fetched = Path(scratch) / 'root', Path(scratch) / 'fetched'
            shutil.copytree(self.LISTS, fetched)
            (root / 'cmake').mkdir(parents=True)
            shutil.copy(ROOT / sanctions_import.REGISTER, root / sanctions_import.REGISTER)
            with patch('sys.stdout', new_callable=io.StringIO):
                self.assertEqual(0, sanctions_import.main(['--root', str(root), 'record', '--from', str(fetched),
                                                           '--authority', 'un.sc']))
            register = json.loads((root / sanctions_import.REGISTER).read_text(encoding='utf-8'))
            read = register['sanctions_lists']['lists']['un.sc']['read']
            self.assertEqual({'day': '2026-11-01', 'published': '2026-10-31T23:00:07Z', 'parties': 7},
                             {key: read[key] for key in ('day', 'published', 'parties')})
            self.assertEqual(hashlib.sha256((fetched / 'un.sc.xml').read_bytes()).hexdigest(), read['sha256'])
            body = (fetched / 'un.sc.xml').read_bytes()
            (fetched / 'un.sc.xml').write_bytes(body[:-1] + bytes([body[-1] ^ 1]))  # one bit, the same length
            with patch('sys.stderr', new_callable=io.StringIO) as error:
                self.assertEqual(1, sanctions_import.main(['--root', str(root), 'import', '--from', str(fetched),
                                                           '--out', str(Path(scratch) / 'out'), '--authority',
                                                           'un.sc']))
            self.assertIn('is not the file its fetch record names; fetch it again', error.getvalue())
            with patch('sys.stderr', new_callable=io.StringIO) as error:
                self.assertEqual(1, sanctions_import.main(['--root', str(root), 'record', '--from', str(fetched),
                                                           '--authority', 'xx.none']))
            self.assertIn('sanctions_lists has no xx.none', error.getvalue())

    def test_fetch_follows_redirects_only_to_the_delivery_hosts(self):
        config = sanctions_import.load_register(ROOT)['sanctions_lists']['lists']['un.sc']
        review, _ = sanctions_import.opener_for(config)
        handler = review.SecureRedirects({'scsanctions.un.org', *config['delivery']})
        request = urllib.request.Request(config['url'], headers={'User-Agent': 'u'})
        signed = 'https://unsolprodfiles.blob.core.windows.net/publiclegacyxmlfiles/EN/x.xml?sig=s'
        self.assertEqual(signed, handler.redirect_request(request, None, 302, 'Found', {}, signed).full_url)
        for target in ('https://evil.example/x.xml', 'https://unsolprodfiles.blob.core.windows.net.evil.example/x'):
            with self.subTest(target=target):
                with self.assertRaises(urllib.error.URLError):
                    handler.redirect_request(request, io.BytesIO(b'moved'), 302, 'Found', {}, target)

    def test_fetch_fails_cleanly_on_a_file_cut_short(self):
        review, _ = sanctions_import.opener_for({'url': 'https://sanctionslist.ofac.treas.gov/x.xml', 'delivery': []})

        def truncated(request, timeout):
            raise http.client.IncompleteRead(b'<sdnList', 4096)

        with tempfile.TemporaryDirectory() as into:
            with patch.object(sanctions_import, 'opener_for', lambda config: (review, truncated)), \
                    patch('sys.stderr', new_callable=io.StringIO) as error:
                self.assertEqual(1, sanctions_import.main(['fetch', '--into', into, '--authority', 'us.ofac.sdn']))
            self.assertEqual('import-sanctions: IncompleteRead(8 bytes read, 4096 more expected)\n', error.getvalue())
            self.assertEqual([], os.listdir(into))

    # Turkey's asset freezes: MASAK's application answers a search per sanction type, kept in canonical form.

    MASAK = ('tr.masak.un', 'tr.masak.foreign', 'tr.masak.domestic', 'tr.masak.7262')

    def test_masak_answers_import_kinds_births_identifiers_names_and_countries(self):
        with tempfile.TemporaryDirectory() as out:
            documents = {}
            for authority in self.MASAK:
                (generation, parties, published), documents[authority] = self.imported(authority, out)
                self.assertEqual(('TR', 31), (documents[authority]['jurisdiction'],
                                              documents[authority]['max_age_days']))
            self.assertEqual((20261101110458, 6, '2026-11-01T11:04:58Z'), (generation, parties, published))
            un, foreign = documents['tr.masak.un'], documents['tr.masak.foreign']
            domestic, proliferation = documents['tr.masak.domestic'], documents['tr.masak.7262']
            # A natural person's birth or parent makes an individual, an IMO number a vessel; the rest name no kind.
            yildirim = self.party(domestic, 'tr.masak.domestic.24')
            self.assertEqual(('individual', ['1978-04-13'], ['TR'], ['national-id.tr:55771274198'], '2021-04-07'),
                             tuple(yildirim[key] for key in ('kind', 'births', 'countries', 'identifiers', 'listed')))
            self.assertEqual(['İç Dondurma (6415 sayılı kanun 7. Maddesine istinaden)', 'DEAŞ'], yildirim['programs'])
            self.assertEqual('unknown', self.party(un, 'tr.masak.un.7')['kind'])
            self.assertEqual(('vessel', ['imo:5571322']), tuple(self.party(proliferation, 'tr.masak.7262.529')[key]
                                                                for key in ('kind', 'identifiers')))
            # 7/2/1966 reads both ways; 23/4/1966 and 4/13/1978 one way; glued a) b) e) give three days.
            self.assertEqual(['1966-02-07', '1966-07-02'], self.party(un, 'tr.masak.un.5')['births'])
            self.assertEqual(['1964-06-25', '1969-12-05', '1965-05-12'], self.party(un, 'tr.masak.un.1904')['births'])
            self.assertEqual(['DZ'], self.party(un, 'tr.masak.un.1904')['countries'])  # "ORAN, CEZAYİR"
            self.assertEqual(['AZ'], self.party(foreign, 'tr.masak.foreign.425')['countries'])  # "USSR, AZERBAIDJAN"
            # Identifiers: two passports joined by VE, a TCKN and a VKN typed as one number or glued to their words,
            # a VKN without its leading zero, a passport and a national id written out in Turkish, SWIFT, e-mail.
            self.assertEqual(['passport:ct4156839', 'passport:ch1457591'],
                             self.party(foreign, 'tr.masak.foreign.56')['identifiers'])
            self.assertEqual(['national-id.tr:15170270542', 'tax-id.tr:7291094811'],
                             self.party(domestic, 'tr.masak.domestic.347')['identifiers'])
            self.assertEqual(['national-id.tr:31849452040', 'tax-id.tr:5980156465'],
                             self.party(domestic, 'tr.masak.domestic.475')['identifiers'])
            self.assertEqual(['passport:551606028', 'tax-id.tr:0551606028'],
                             self.party(domestic, 'tr.masak.domestic.292')['identifiers'])
            self.assertEqual(['passport:d000857', 'national-id:300786'],
                             self.party(un, 'tr.masak.un.64')['identifiers'])
            self.assertEqual(['swift:ftbdkppy'], self.party(proliferation, 'tr.masak.7262.576')['identifiers'])
            self.assertEqual(['email:daesong@star-co.net.kp'],
                             self.party(proliferation, 'tr.masak.7262.1204')['identifiers'])
            # Names: parentheses as aliases less notes, a) b) glued to a name, 1- 2- 3-, an original script.
            self.assertEqual(['ABD AL-BASET AZZOUZ', 'ABDELBASSED AZOUZ', 'AA', 'ABDUL BASET AZOUZ'],
                             self.party(un, 'tr.masak.un.5')['names'])
            self.assertEqual(['ABU RUSDAN', 'Abu Thorjq', 'Rusdjan', 'Rusjan', 'Rusydan', 'Thoriquddin',
                              'Thoriquiddin', 'Thoriquidmîı', 'Toriquddin'], self.party(un, 'tr.masak.un.117')['names'])
            self.assertEqual(['39. ODA', 'OFFICE 39', 'Office #39', 'Office No. 39', 'Bureau 39',
                              'Central Committee Bureau 39', 'Third FloorDivision 39'],
                             self.party(proliferation, 'tr.masak.7262.1')['names'])
            chonmyong = self.party(proliferation, 'tr.masak.7262.529')['names']
            self.assertNotIn('Kore Demokratik Halk Cumhuriyeti', chonmyong)  # a country in an address is no name
            self.assertEqual(['MUHAMMED AL VAKİ', 'MOHAMAD ALSAİED ALHMİDAN', 'MOHAMAD ALUOALİ', 'WALİD AYSSA'],
                             self.party(domestic, 'tr.masak.domestic.1505')['names'])
            self.assertEqual('', self.party(domestic, 'tr.masak.domestic.1505')['listed'])  # "/ 32153": no day
            self.assertIn('АХМАД ШИШАНИ', self.party(un, 'tr.masak.un.211')['names'])

    def test_masak_births_identifiers_and_check_digits(self):
        births = sanctions_import.masak_births
        cases = [('', ([], False)), ('7/2/1966', (['1966-02-07', '1966-07-02'], True)),
                 ('23/4/1966', (['1966-04-23'], True)), ('10.08..1948', (['1948-08-10', '1948-10-08'], True)),
                 ('"a) 1974   \nb) 1975 \n"\n', (['1974', '1975'], True)), ('1969-1971  Arası', (['1969/1971'], True)),
                 ('1977 -1982 Arası', (['1977/1982'], True)),
                 ('1961 veya 20 Temmuz 1962', (['1961', '1962-07-20'], True)),
                 ('a)17 Aug 1972\n b) 1 Jan 1972', (['1972-08-17', '1972-01-01'], True)),
                 ('16 KASIM 1977 - 16 KASIM 1974', (['1977-11-16', '1974-11-16'], True)),
                 ('Tahmini olarak 1953', (None, True)), ('1977 (Tahmini)', (None, True)),
                 ('a) 15 Ekim 1963 b) Tahmini olarak 1957', (None, True)),
                 ('WİJİJOKO SANTOSO, 14 TEMMUZ 1975', (None, True)), ('TEXAS/ABD', (None, False)),
                 ('31/31/1966', (None, True)), ('1899', (None, True)), ('21.03.199613.11.1977', (None, True))]
        for text, expected in cases:
            with self.subTest(text=text):
                self.assertEqual(expected, births(text))
        self.assertTrue(sanctions_import.tckn_valid('55771274198'))
        # The last digit wrong; the tenth wrong with the last still their sum's; a leading zero; short; a letter.
        for bad in ('55771274199', '55771274109', '05771274198', '5577127419', '5577127419a'):
            self.assertFalse(sanctions_import.tckn_valid(bad), bad)
        self.assertTrue(sanctions_import.vkn_valid('0680925635'))
        self.assertFalse(sanctions_import.vkn_valid('0680925636'))
        self.assertFalse(sanctions_import.vkn_valid('068092563'))

        def identifiers(text):
            party = sanctions_import.Party('tr.x', 1, 'unknown')
            sanctions_import.masak_identifiers(party, text)
            return party.identifiers

        self.assertEqual(['national-id.tr:36113043528', 'tax-id.tr:9611118807'], identifiers('36113043528-9611118807'))
        self.assertEqual(['passport:27082171', 'national-id:156-92-9858'],
                         identifiers('": 27082171 (Amerika, 21 Haziran 1992)\n'
                                     'Ulusal Kimlik Numarası : SSN 156-92-9858'))
        self.assertEqual(['passport:653225197110100533'], identifiers('Çin 653225197110100533'))
        self.assertEqual(['national-id.tr:99637657350', 'tax-id.tr:5240949089'],
                         identifiers('TCK:99637657350YKN:5240949089'))  # check digits outrank the word
        self.assertEqual([], identifiers('Telephone: +850-2-181118208.Fax: +850-2-381-4432'))
        self.assertEqual(['tax-id:32084989741'], identifiers('Yabancı Ülke VKN: 32084989741'))
        self.assertEqual(['passport:d0009724'],
                         identifiers('Afganistan pasaport numarası u No:D0009724, 13 Ekim 2021’de. Son: 13 Ekim2026'))
        self.assertEqual(['passport:tr-p614166'], identifiers('pasaport numarası TR-P 614166 (22 Mart 2006)'))
        self.assertEqual([], identifiers('Afgan ulusal kimlik kartı (tazkira) numarası 4414'))

    def test_masak_answers_refuse_other_shapes(self):
        register = sanctions_import.load_register(ROOT)
        config = register['sanctions_lists']['lists']['tr.masak.domestic']
        record, body = sanctions_import.fetched(self.LISTS, 'tr.masak.domestic', config)
        read = sanctions_import.READERS['tr-masak-json'].read

        def refused(message, document=None, record=record, config=config, raw=None):
            text = raw if raw is not None else \
                json.dumps(document, ensure_ascii=False).encode('utf-8')
            with self.assertRaisesRegex(sanctions_import.SanctionsError, re.escape(message)):
                read(text, 'tr.masak.domestic', record, config)

        def changed(change):
            document = json.loads(body.decode('utf-8'))
            change(document)
            return document

        refused('the answer holds', changed(lambda d: d.update(elapsedTime=4)))
        refused('the answer holds', changed(lambda d: d.pop('errors')))
        refused('the answer is no success', changed(lambda d: d.update(statusCode=500)))
        refused('the answer is no success', changed(lambda d: d.update(errors=['x'])))
        refused('the columns are', changed(lambda d: d['columnNames'].reverse()))
        refused('numRows is not the number of rows', changed(lambda d: d.update(numRows=7)))
        refused('a row without exactly the columns', changed(lambda d: d['data'][0].pop('orgutu')))
        refused("row 24 has the dogum_tarihi 1978", changed(lambda d: d['data'][0].update(dogum_tarihi=1978)))
        refused("row '24' has the sira_no '24'", changed(lambda d: d['data'][0].update(sira_no='24')))
        refused('row 24 is a', changed(lambda d: d['data'][0].update(yaptirim_turu='Yeni')))
        refused("the country 'Atlantis' is not in", changed(lambda d: d['data'][0].update(uyrugu='Atlantis')))
        refused("the decision '2021 / 31447' is no Official Gazette day",
                changed(lambda d: d['data'][0].update(karar_sayisi='2021 / 31447')))
        refused('the JSON constant NaN', raw=body.replace(b'"numRows": 6', b'"numRows": NaN'))
        refused('not a JSON answer in UTF-8', raw=b'\xff' + body)
        refused('the generation time', changed(lambda d: None), record=dict(record, instant=None))
        refused('post names the sanction type', changed(lambda d: None), config=dict(config, post=None))
        with patch.object(sanctions_import, 'FILE_LIMIT', len(body) - 1):
            refused('larger than', raw=body)

    def test_masak_fetch_posts_the_type_keeps_the_canonical_answer_and_refuses_an_unread_type(self):
        register = sanctions_import.load_register(ROOT)
        lists = register['sanctions_lists']['lists']
        types = [lists[authority]['post']['yaptirim'] for authority in self.MASAK]
        rows = json.loads((self.LISTS / 'tr.masak.domestic.json').read_text(encoding='utf-8'))['data']
        answers = iter(range(1, 1000))
        posted = []

        def envelope(columns, data):
            return {'statusCode': 200, 'statusDescription': 'OK', 'message': None, 'columnNames': columns,
                    'numRows': len(data), 'data': data, 'result': None, 'errors': [], 'elapsedTime': next(answers)}

        def opener(offered):
            def answer(request, timeout):
                if request.full_url == sanctions_import.MASAK_TYPES:
                    document = envelope(['yaptirim_turu'], [{'yaptirim_turu': kind} for kind in offered])
                else:
                    sent = json.loads(request.data)
                    posted.append((request.get_method(), request.get_header('Content-type'), sent))
                    kept = [dict(row, yaptirim_turu=sent['yaptirim']) for row in rows]
                    document = envelope(sanctions_import.MASAK_COLUMNS, kept)
                return io.BytesIO(json.dumps(document, ensure_ascii=False).encode('utf-8'))
            return answer

        review, _ = sanctions_import.opener_for(lists['tr.masak.domestic'])
        with tempfile.TemporaryDirectory() as scratch:
            first, second = Path(scratch) / 'first', Path(scratch) / 'second'
            for into in (first, second):
                with patch.object(sanctions_import, 'opener_for', lambda config: (review, opener(types))), \
                        patch('sys.stdout', new_callable=io.StringIO):
                    self.assertEqual(0, sanctions_import.main(['fetch', '--into', str(into), '--authority',
                                                               'tr.masak.domestic']))
            self.assertEqual(('POST', 'application/json', lists['tr.masak.domestic']['post']), posted[0])
            kept = (first / 'tr.masak.domestic.json').read_bytes()
            self.assertNotIn(b'elapsedTime', kept)
            self.assertEqual(kept, (second / 'tr.masak.domestic.json').read_bytes())  # another elapsedTime
            record = json.loads((first / 'tr.masak.domestic.fetch.json').read_text(encoding='utf-8'))
            self.assertRegex(record['instant'], r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$')
            self.assertEqual(record['day'], record['instant'][:10])
            # A sanction type no list of the register posts refuses the fetch, and so does one withdrawn.
            for offered in (types + ['Yeni Yaptırım'], types[1:]):
                with self.subTest(offered=len(offered)), \
                        patch.object(sanctions_import, 'opener_for', lambda config: (review, opener(offered))), \
                        patch('sys.stderr', new_callable=io.StringIO) as error:
                    self.assertEqual(1, sanctions_import.main(['fetch', '--into', str(Path(scratch) / 'third'),
                                                               '--authority', 'tr.masak.un']))
                self.assertIn('read the new type and add or retire its list', error.getvalue())
                self.assertFalse((Path(scratch) / 'third' / 'tr.masak.un.json').exists())

    def test_an_answer_read_again_later_is_a_new_generation(self):
        generation_of = sanctions_import.generation_of
        self.assertEqual(20261101120000, generation_of('2026-11-01T12:00:00Z', (20261101110458, 'a'), 'a', 'x'))
        self.assertIsNone(generation_of('2026-11-01T11:04:58Z', (20261101110458, 'a'), 'a', 'x'))
        self.assertIsNone(generation_of('2026-11-01T10:00:00Z', (20261101110458, 'a'), 'a', 'x'))
        with tempfile.TemporaryDirectory() as scratch:
            register = sanctions_import.load_register(ROOT)
            config = register['sanctions_lists']['lists']['tr.masak.7262']
            record, body = sanctions_import.fetched(self.LISTS, 'tr.masak.7262', config)
            self.assertEqual(20261101110458, sanctions_import.import_list('tr.masak.7262', config, 'TR', record, body,
                                                                         scratch)[0])
            later = dict(record, instant='2026-11-20T08:00:00Z')
            self.assertEqual(20261120080000, sanctions_import.import_list('tr.masak.7262', config, 'TR', later, body,
                                                                         scratch)[0])
            text = (Path(scratch) / 'tr.masak.7262.sanctions.toml').read_text(encoding='utf-8')
            self.assertIn('published = "2026-11-20"', text)

    def test_an_approximate_birth_leaves_the_party_no_period(self):
        party = sanctions_import.Party('x', 1, 'individual')
        party.birth('1968')
        party.birth(sanctions_import.APPROXIMATE)
        party.name('A B')
        party.program('P')
        self.assertEqual([], party.finished().births)
        exact = sanctions_import.Party('x', 2, 'individual')
        exact.birth(['1968', '1970'])
        exact.name('A B')
        exact.program('P')
        self.assertEqual(['1968', '1970'], exact.finished().births)
        with self.assertRaisesRegex(sanctions_import.SanctionsError, 'a birth on a entity'):
            sanctions_import.Party('x', 3, 'entity').birth(sanctions_import.APPROXIMATE)

    def test_a_list_posts_its_search_only_when_its_format_is_an_applications_answer(self):
        where = 'legal register: sanctions_lists: tr.masak'
        message = f"{where}: post is null for a file, the search posted (an object of texts)"
        self.assert_lists_fail(self.sanctions(post={'yaptirim': 'x'}), message)
        masak = {'format': 'tr-masak-json', 'delivery': [], 'read': None}
        self.assertEqual(0, self.run_lists(self.sanctions(post={'ad': '', 'yaptirim': 'x'}, **masak))[0])
        for post in (None, {}, {'yaptirim': 7}, ['yaptirim']):
            with self.subTest(post=post):
                self.assert_lists_fail(self.sanctions(post=post, **masak), message)


class RegisterReview(RegisterFixture, unittest.TestCase):
    """scripts/register-review.py: the scheduled re-research plans the primary sources a refresh downloads (for the
    user to approve), reads them with digests, judges them against the register, proposes ROADMAP amendments without
    editing a row, applies register changes on request and records each run so the guard's due day moves."""

    # The shapes of the IANA files the first refresh read (2026-10-31): QUIC Versions records carry no name, only the
    # RFC they cite; an ALPN record keys its protocol by name and spells the identification sequence.
    QUIC_XML = ('<?xml version="1.0"?><registry xmlns="http://www.iana.org/assignments" id="quic">'
                '<title>QUIC</title>'
                '<registry id="quic-versions"><title>QUIC Versions</title><xref type="rfc" data="rfc9000"/>'
                '<range><value>value 0x?a?a?a?a</value><registration_rule>Reserved</registration_rule></range>'
                '<record><value>0x00000000</value><status>permanent</status><xref type="rfc" data="rfc9000"/>'
                '<notes>Reserved for Version Negotiation</notes></record>'
                '<record><value>0x00000001</value><status>permanent</status><xref type="rfc" data="rfc9000"/>'
                '<contact><xref type="person" data="QUIC_WG"/></contact><notes/></record>'
                '<record><value>0x6b3343cf</value><status>permanent</status><xref type="rfc" data="rfc9369"/>'
                '<notes/></record>'
                '</registry>'
                '<registry id="quic-transport"><title>QUIC Transport Parameters</title>'
                '<record><value>0x20</value><description>max_datagram_frame_size</description></record>'
                '<record><value>0x11</value><description>version_information</description></record>'
                '<record><value>0x2ab2</value><description>grease_quic_bit</description>'
                '<xref type="draft" data="draft-ietf-quic-bit-grease-04"/></record>'
                '</registry></registry>')
    ALPN_XML = ('<?xml version="1.0"?><registry xmlns="http://www.iana.org/assignments" id="tls-extensiontype-values">'
                '<registry id="alpn-protocol-ids">'
                '<title>TLS Application-Layer Protocol Negotiation (ALPN) Protocol IDs</title>'
                '<record><value>HTTP/3</value><description>0x68 0x33 ("h3")</description>'
                '<xref type="rfc" data="rfc9114"/></record></registry>'
                '<registry id="tls-extensiontype-values-1"><title>TLS ExtensionType Values</title>'
                '<record><value>27</value><description>compress_certificate</description><rec>Y</rec></record>'
                '<record><value>3-255</value><description>Unassigned</description></record>'
                '</registry></registry>')

    # The Recommended column (RFC 9847) as the TLS parameters file spells it: `<rec>` in Supported Groups, where
    # RFC 9847 discourages secp224r1, and `<recommended>` in SignatureScheme; x25519's record lacks the column here.
    TLS_XML = ('<?xml version="1.0"?><registry xmlns="http://www.iana.org/assignments" id="tls-parameters">'
               '<registry id="tls-parameters-8"><title>TLS Supported Groups</title>'
               '<record><value>21</value><description>secp224r1</description><dtls>Y</dtls><rec>D</rec>'
               '<xref type="rfc" data="rfc9847"/></record>'
               '<record><value>29</value><description>x25519</description><dtls>Y</dtls>'
               '<xref type="rfc" data="rfc9846"/></record></registry>'
               '<registry id="tls-signaturescheme"><title>TLS SignatureScheme</title>'
               '<record><value>0x0807</value><description>ed25519</description><recommended>Y</recommended>'
               '<xref type="rfc" data="rfc9846"/></record></registry></registry>')

    def register(self, entries, **changes):
        """The fixture register with IETF 128 on its calendar, so a run on the fixture's day still owes a scan."""
        calendar = self.schedule()['calendar'] + [{'event': 'IETF 128', 'day': '2027-03-26', 'scan': 'light',
                                                   'verified': False}]
        return super().register(entries, **dict({'review': self.schedule(calendar=calendar)}, **changes))

    def entries(self):
        """A pinned RFC, a pinned and a watched draft, a W3C document and IANA and manual codepoints."""
        quic = self.entry(codepoints=[
            {'registry': 'QUIC Versions', 'name': 'QUIC', 'value': '0x00000001', 'verified': False},
            {'registry': 'QUIC Versions', 'name': 'QUIC v2', 'value': '0x6b3343cf', 'verified': False},
            {'registry': 'QUIC Transport Parameters', 'name': 'max_datagram_frame_size', 'value': '0x21',
             'verified': False},
            {'registry': 'QUIC Transport Parameters', 'name': 'ack_frequency', 'value': '0xff04de1b',
             'verified': False},
            {'registry': 'QUIC Transport Parameters', 'name': 'version_information', 'value': '0x11'}])
        h3 = self.entry(id='http.h3', spec={'type': 'rfc', 'ref': 'RFC 9114', 'status': 'current'},
                        codepoints=[{'registry': 'TLS ALPN Protocol IDs', 'name': 'HTTP/3', 'value': 'h3',
                                     'verified': False},
                                    {'registry': 'TLS ExtensionType Values', 'name': 'compress_certificate',
                                     'value': '27', 'verified': False},
                                    {'registry': 'Object identifiers', 'name': 'id-ml-dsa-44',
                                     'value': '2.16.840.1.101.3.4.3.17', 'verified': False}])
        pinned = self.entry(id='webtransport.h3', spec=self.DRAFT, codepoints=[])
        watched = self.entry(id='quic.multipath', state='watch', codepoints=[],
                             spec={'type': 'draft', 'ref': 'draft-ietf-quic-multipath', 'revision': '21',
                                   'posted': '2026-03-17', 'status': 'active'})
        published = self.entry(id='quic.reliable-reset', state='watch', codepoints=[],
                               spec={'type': 'draft', 'ref': 'draft-ietf-quic-reliable-stream-reset',
                                     'revision': '11', 'status': 'rfc-editor'})
        trace = self.entry(id='ops.trace-context', codepoints=[],
                           spec={'type': 'w3c', 'ref': 'https://www.w3.org/TR/trace-context/',
                                 'status': 'recommendation'})
        entries = [quic, h3, pinned, watched, published, trace]
        for entry in entries:
            entry['review'] = '2027-06-01'
        return entries

    def tool(self, root, *argv, today='2026-12-16'):
        with patch('sys.stdout', new_callable=io.StringIO) as out, \
                patch('sys.stderr', new_callable=io.StringIO) as err:
            code = register_review.main(['--root', str(root), '--today', today, *argv])
        return code, out.getvalue() + err.getvalue()

    def records(self, rfc9000=None, rfc9114=None, webtransport=None, multipath=None, reset=None):
        """The fetched bodies by source id; None keeps the record unchanged from the register."""
        def tracker(name, rev, state, published, **extra):
            return json.dumps(dict({'name': name, 'rev': rev, 'state': state, 'iesg_state': None,
                                    'rfceditor_state': None, 'aliases': [name],
                                    'rev_history': [{'rev': rev, 'published': published + 'T12:00:00Z'}]}, **extra))
        return {
            'rfc9000': rfc9000 or json.dumps({'doc_id': 'RFC9000', 'obsoleted_by': []}),
            'rfc9114': rfc9114 or json.dumps({'doc_id': 'RFC9114', 'obsoleted_by': []}),
            'draft-ietf-webtrans-http3': webtransport or tracker('draft-ietf-webtrans-http3', '16', 'Active',
                                                                 '2026-07-01'),
            'draft-ietf-quic-multipath': multipath or tracker('draft-ietf-quic-multipath', '21', 'Active',
                                                              '2026-03-17'),
            'draft-ietf-quic-reliable-stream-reset': reset or tracker('draft-ietf-quic-reliable-stream-reset', '11',
                                                                      'Active', '2026-05-01',
                                                                      rfceditor_state='EDIT'),
            'iana-quic': self.QUIC_XML,
            'iana-tls-extensiontype-values': self.ALPN_XML,
        }

    def opener(self, bodies):
        def urlopen(request, timeout):
            self.assertEqual('cerid-register-review/1', request.get_header('User-agent'))
            self.assertEqual(register_review.FETCH_TIMEOUT, timeout)
            for source_id, body in bodies.items():
                path = f'/assignments/{source_id[5:]}/' if source_id.startswith('iana-') else f'/{source_id}'
                if path in request.full_url:
                    if isinstance(body, Exception):
                        raise body
                    return contextlib.closing(SimpleNamespace(read=lambda limit, body=body: body.encode()[:limit],
                                                              close=lambda: None))
            raise urllib.error.URLError('no fixture')
        return urlopen

    def fetched(self, root, bodies):
        """Plans and fetches the fixture's sources into root/fetched; returns the plan."""
        self.assertEqual(0, self.tool(root, 'plan', '--out', str(root / 'sources.json'))[0])
        plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
        register_review.fetch_sources(plan, root / 'fetched', opener=self.opener(bodies),
                                      today=datetime.date(2026, 12, 16))
        return plan

    # The shapes of the official texts the legal register's first read fetched (2026-11-01): mevzuat.gov.tr's
    # consolidated HTML with its amendment notes, CELLAR's SPARQL listing and English XHTML, legislation.gov.uk's
    # Crown Legislation Markup and the eCFR versioner.
    MEVZUAT_HTML = ('<html><head><style>p { margin: 0 }</style><script>window.av = {}</script></head><body>'
                    '<p>KİŞİSEL VERİLERİN KORUNMASI KANUNU</p><p>Kanun Numarası : 6698</p>'
                    '<p>Kabul Tarihi : 24/3/2016</p><p>MADDE 9- (Değişik:2/3/2024-7499/34 md.) (1) ...</p>'
                    '<p>MADDE 18- (Mülga: 2/7/2018-KHK-703/163 md.) ...</p></body></html>')
    CELLAR_XHTML = ('<?xml version="1.0" encoding="UTF-8"?><html xmlns="http://www.w3.org/1999/xhtml"><body>'
                    '<p>Regulation (EU) 2016/679 of the European Parliament and of the Council</p></body></html>')
    LEGISLATION_XML = ('<Legislation xmlns="http://www.legislation.gov.uk/namespaces/legislation"><ukm:Metadata '
                       'xmlns:ukm="http://www.legislation.gov.uk/namespaces/metadata" '
                       'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dct="http://purl.org/dc/terms/">'
                       '<dc:identifier>http://www.legislation.gov.uk/ukpga/2018/12</dc:identifier>'
                       '<dct:valid>2026-09-30</dct:valid></ukm:Metadata></Legislation>')

    @staticmethod
    def sparql(*rows):
        """CELLAR's answer to the version index: a consolidated version's CELEX number, or an act changing the act as
        (CELEX number, day[, a consolidated version consolidating it])."""
        names = ('celex', 'day', 'by')
        return json.dumps({'head': {'vars': list(names)}, 'results': {'bindings': [
            {name: {'type': 'literal', 'value': value}
             for name, value in zip(names, (row,) if isinstance(row, str) else row)} for row in rows]}})

    def routed(self, routes, requests=None):
        """An opener answering each request with the body of the first route its URL contains: text, bytes, an
        exception to raise, or (body, headers)."""
        def urlopen(request, timeout):
            self.assertEqual(register_review.USER_AGENT, request.get_header('User-agent'))
            self.assertEqual('gzip', request.get_header('Accept-encoding'))
            if requests is not None:
                requests.append(request)
            for part, body in routes.items():
                if part in request.full_url:
                    body, headers = body if isinstance(body, tuple) else (body, {})
                    if isinstance(body, Exception):
                        raise body
                    data = body.encode() if isinstance(body, str) else body
                    return contextlib.closing(SimpleNamespace(read=lambda limit, data=data: data[:limit],
                                                              headers=headers, close=lambda: None))
            raise urllib.error.URLError(f'no route for {request.full_url}')
        return urlopen

    def test_the_legal_domain_reads_each_hosts_document_and_judges_version_and_digest(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            gdpr = 'https://eur-lex.europa.eu/eli/reg/2016/679/oj'
            eu = {'jurisdiction': 'EU', 'primary': gdpr}
            verified = {'ref': 'docs/research.md#n83', 'read': '2026-10-27', 'verified': True}
            read = dict(self.READ, version='02016R0679-20160504',
                        sha256=hashlib.sha256(self.CELLAR_XHTML.encode()).hexdigest())
            legal = self.legal_register([
                self.legal_entry(),
                self.legal_entry(id='eu.gdpr.principles', instrument=self.instrument(**eu, consolidated=read),
                                 source=verified),
                self.legal_entry(id='eu.gdpr.rights', instrument=self.instrument(**eu, consolidated=dict(
                    read, sha256='cd' * 32)), source=verified),
                self.legal_entry(id='eu.gdpr.breach', instrument=self.instrument(**eu, consolidated=dict(
                    read, version='32016R0679')), source=verified)])
            self.write_tree(root, self.register(self.entries()), legal)
            argv = ('--register', 'cmake/legal-register.json')
            code, output = self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))
            self.assertEqual(0, code, output)
            self.assertIn('3 sources to download once the user approves them, 1 items read by hand', output)
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            sources = {source['id']: source for source in plan['sources']}
            self.assertEqual(['eu.gdpr.principles', 'eu.gdpr.rights', 'eu.gdpr.breach'],
                             sources['celex-32016r0679']['entries'])
            self.assertEqual('celex-32016r0679-versions', sources['celex-32016r0679']['index'])
            self.assertEqual('index', sources['celex-32016r0679-versions']['kind'])
            self.assertEqual('primary', sources['mevzuat-1-5-6698']['kind'])
            self.assertIn('https://www.resmigazete.gov.tr/', {item['url'] for item in plan['manual']})
            requests = []
            routes = {'MevzuatNo=6698': self.MEVZUAT_HTML,
                      'webapi/rdf/sparql': self.sparql('02016R0679-20160504', '02016R0679-20991231'),
                      '/resource/celex/02016R0679-20160504': self.CELLAR_XHTML}
            manifest = register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes, requests),
                                                     today=datetime.date(2026, 12, 16))
            self.assertEqual('index', next(source['kind'] for source in plan['sources']
                                          if source['id'] == manifest['items'][0]['id']))
            items = {item['id']: item for item in manifest['items']}
            self.assertEqual('02016R0679-20160504', items['celex-32016r0679']['version'])
            self.assertEqual('https://publications.europa.eu/resource/celex/02016R0679-20160504',
                             items['celex-32016r0679']['url'])
            cellar = next(request for request in requests if '/resource/celex/' in request.full_url)
            self.assertEqual('application/xhtml+xml', cellar.get_header('Accept'))
            self.assertEqual('eng', cellar.get_header('Accept-language'))
            report = root / 'docs' / 'legal-review.md'
            code, output = self.tool(root, *argv, 'compare', '--fetched', str(root / 'fetched'), '--report',
                                     str(report), '--apply')
            self.assertEqual(0, code, output)
            self.assertIn('0 register changes, 3 instruments to read', output)
            text = report.read_text(encoding='utf-8')
            self.assertIn('legal register', text)
            self.assertIn('- tr.kvkk.processing (NET.2): Law No. 6698, Articles 4-7: never read from its primary '
                          'source; read https://www.mevzuat.gov.tr/anasayfa/MevzuatFihristDetayIframe?MevzuatTur=1&'
                          'MevzuatNo=6698&MevzuatTertip=5 (version 2024-03-02 (7499), SHA-256 ', text)
            self.assertIn('- eu.gdpr.rights (NET.2): Law No. 6698, Articles 4-7: the primary text changed since it '
                          'was read on 2026-10-30', text)
            self.assertIn('- eu.gdpr.breach (NET.2): Law No. 6698, Articles 4-7: amended to 02016R0679-20160504 since '
                          'it was read on 2026-10-30 (32016R0679)', text)
            self.assertNotIn('eu.gdpr.principles', text)
            register = json.loads((root / 'cmake' / 'legal-register.json').read_text(encoding='utf-8'))
            self.assertEqual('docs/legal-review.md', register['review']['runs'][-1]['ref'])
            legal_errors = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 12, 16))}
            self.assertEqual([], legal_errors['legal'])
            code, output = self.tool(root, *argv, 'record', '--scan', 'light', '--ref', 'docs/research.md#n83',
                                     '--day', '2026-12-17', '--trigger', 'eu.gdpr.rights', today='2026-12-17')
            self.assertEqual(0, code, output)
            self.assertIn('recorded the light run of 2026-12-17 with triggers eu.gdpr.rights; the next review is a '
                          'full scan due on 2027-01-11', output)
            code, output = self.tool(root, *argv, 'record', '--scan', 'light', '--ref', 'docs/research.md',
                                     '--trigger', 'eu.nothing', today='2026-12-17')
            self.assertEqual(2, code)
            self.assertIn("unknown triggering entries ['eu.nothing']", output)

    def test_read_records_what_a_person_read_and_refuses_anything_it_cannot_prove(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            apple = self.instrument(jurisdiction='PLATFORM', primary='https://developer.apple.com/news/')
            calendar = [{'event': 'Second quarter', 'day': '2027-06-30', 'scan': 'light', 'verified': True,
                         'reads': ['tr.kvkk.processing']},
                        {'event': 'Third quarter', 'day': '2027-09-30', 'scan': 'light', 'verified': True,
                         'reads': ['tr.kvkk.rights', 'platform.apple']}]
            legal = self.legal_register([
                self.legal_entry(),
                self.legal_entry(id='tr.kvkk.rights', instrument=self.instrument(article='Article 11')),
                self.legal_entry(id='platform.apple', instrument=apple)],
                review=self.schedule(next='2027-07-25', calendar=calendar))
            self.write_tree(root, self.register(self.entries()), legal)
            (root / 'docs' / 'reading.md').write_text('<a id="kvkk"></a>Read 6698.\n', encoding='utf-8')
            argv = ('--register', 'cmake/legal-register.json')
            code, output = self.tool(root, *argv, 'due', today='2026-11-01')
            self.assertEqual(0, code, output)
            self.assertIn('the scan after Second quarter (2027-06-30) reads 1 instruments not yet read', output)
            self.assertIn('the scan after Third quarter (2027-09-30) reads 2 instruments not yet read', output)
            self.assertEqual(0, self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))[0])
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            routes = {'MevzuatNo=6698': self.MEVZUAT_HTML, 'developer.apple.com': '<html>news</html>'}
            register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes),
                                          today=datetime.date(2026, 12, 16))
            fetched = ('--fetched', str(root / 'fetched'))
            path = root / 'cmake' / 'legal-register.json'
            before = path.read_bytes()
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#kvkk',
                                     '--entry', 'tr.kvkk.processing')
            self.assertEqual(2, code)
            self.assertIn("the fetch of 2026-12-16 is not the register's snapshot (2026-10-31); compare --apply it "
                          'first', output)
            report = root / 'docs' / 'legal-review.md'
            self.assertEqual(0, self.tool(root, *argv, 'compare', *fetched, '--report', str(report), '--apply')[0])
            before = path.read_bytes()
            for entries, ref, message in (
                    (['tr.kvkk.processing', 'platform.apple'], 'docs/reading.md',
                     'platform.apple: https://developer.apple.com/news/ has no reader of its host'),
                    (['tr.kvkk.processing', 'eu.nothing'], 'docs/reading.md', 'eu.nothing: not an instrument'),
                    (['quic.transport'], 'docs/reading.md', 'quic.transport: not an instrument'),
                    (['tr.kvkk.processing'], 'docs/unwritten.md', 'docs/unwritten.md does not exist')):
                code, output = self.tool(root, *argv, 'read', *fetched, '--ref', ref,
                                         *[part for entry in entries for part in ('--entry', entry)])
                self.assertEqual(2, code, entries)
                self.assertIn(message, output)
                self.assertEqual(before, path.read_bytes(), 'a refused read writes nothing')
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#kvkk', '--entry',
                                     'tr.kvkk.processing', '--entry', 'tr.kvkk.rights', '--entry', 'tr.kvkk.rights')
            self.assertEqual(0, code, output)
            self.assertIn('tr.kvkk.processing: read Law No. 6698 at 2024-03-02 (7499) on 2026-12-16', output)
            register = json.loads(path.read_text(encoding='utf-8'))
            entries = {entry['id']: entry for entry in register['entries']}
            digest = hashlib.sha256(self.MEVZUAT_HTML.encode()).hexdigest()
            for entry_id in ('tr.kvkk.processing', 'tr.kvkk.rights'):
                self.assertEqual({'version': '2024-03-02 (7499)', 'read': '2026-12-16', 'sha256': digest},
                                 entries[entry_id]['instrument']['consolidated'])
                self.assertEqual({'ref': 'docs/reading.md#kvkk', 'read': '2026-12-16', 'verified': True},
                                 entries[entry_id]['source'])
            self.assertIsNone(entries['platform.apple']['instrument']['consolidated'])
            self.assertEqual([{'event': 'Second quarter', 'day': '2027-06-30', 'scan': 'light', 'verified': True},
                              dict(calendar[1], reads=['platform.apple'])], register['review']['calendar'],
                             'a reading recorded leaves the scans that scheduled it')
            checked = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 12, 16))}
            self.assertEqual([], checked['legal'])
            code, output = self.tool(root, *argv, 'compare', *fetched, '--report', str(report))
            self.assertEqual(0, code, output)
            self.assertIn('0 register changes, 1 instruments to read', output)
            (root / 'fetched' / 'mevzuat-1-5-6698').write_text('tampered', encoding='utf-8')
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md',
                                     '--entry', 'tr.kvkk.rights')
            self.assertEqual(2, code)
            self.assertIn('does not match the SHA-256 the manifest recorded', output)
            plan['register'] = 'cmake/standards-register.json'
            register_review.fetch_sources(plan, root / 'standards', opener=self.routed(routes),
                                          today=datetime.date(2026, 12, 16))
            code, output = self.tool(root, 'read', '--fetched', str(root / 'standards'), '--ref', 'docs/reading.md',
                                     '--entry', 'quic.transport')
            self.assertEqual(2, code)
            self.assertIn('read records the instruments of the legal register', output)

    def test_read_refuses_an_entry_whose_document_was_not_read_or_is_unreadable(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            gdpr = self.instrument(jurisdiction='EU', primary='https://eur-lex.europa.eu/eli/reg/2016/679/oj')
            self.write_tree(root, self.register(self.entries()), self.legal_register([
                self.legal_entry(), self.legal_entry(id='eu.gdpr.rights', instrument=gdpr)]))
            argv = ('--register', 'cmake/legal-register.json')
            self.assertEqual(0, self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))[0])
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            routes = {'MevzuatNo=6698': self.MEVZUAT_HTML.replace('6698', '6699'),
                      'webapi/rdf/sparql': urllib.error.URLError('down\r\n  for now')}
            register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes),
                                          today=datetime.date(2026, 12, 16))
            fetched = ('--fetched', str(root / 'fetched'))
            code, output = self.tool(root, *argv, 'compare', *fetched, '--report', str(root / 'docs' / 'r.md'),
                                     '--apply')
            self.assertEqual(0, code, output)
            text = (root / 'docs' / 'r.md').read_text(encoding='utf-8')
            self.assertIn('- mevzuat-1-5-6698: not the consolidated text of No. 6698', text)
            self.assertIn('- celex-32016r0679 (https://publications.europa.eu/resource/celex/{version}): its version '
                          'index celex-32016r0679-versions was not read', text)
            self.assertIn('- celex-32016r0679-versions (https://publications.europa.eu/webapi/', text)
            self.assertIn('): URLError: <urlopen error down for now>\n', text)
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/research.md', '--entry',
                                     'tr.kvkk.processing', '--entry', 'eu.gdpr.rights')
            self.assertEqual(2, code)
            self.assertIn('tr.kvkk.processing: mevzuat-1-5-6698: not the consolidated text of No. 6698', output)
            self.assertIn('eu.gdpr.rights: celex-32016r0679 was not read (its version index '
                          'celex-32016r0679-versions was not read)', output)

    def test_the_acts_a_consolidated_text_lacks_are_read_as_published_before_it_is_recorded(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            eur_lex = 'https://eur-lex.europa.eu/eli/reg/{}/oj'
            legal = self.legal_register([self.legal_entry()] + [
                self.legal_entry(id=entry_id,
                                 instrument=self.instrument(jurisdiction='EU', primary=eur_lex.format(act)))
                for entry_id, act in (('eu.gdpr.rights', '2016/679'), ('eu.cra.reporting', '2024/2847'),
                                      ('eu.ai.literacy', '2024/1689'))])
            self.write_tree(root, self.register(self.entries()), legal)
            (root / 'docs' / 'reading.md').write_text('<a id="acts"></a>Read.\n', encoding='utf-8')
            argv = ('--register', 'cmake/legal-register.json')
            code, output = self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))
            self.assertEqual(0, code, output)
            self.assertIn('7 sources to download once the user approves them', output)
            self.assertIn('  and, as published, each act a consolidated text lacks '
                          '(https://publications.europa.eu/resource/celex/<CELEX>)', output)
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            # Each act's index answers by its CELEX number: GDPR lacks a corrigendum CELLAR cannot serve, the CRA one
            # that does not name its act, and all three the regulation amending them (consolidated in the AI Act only
            # by a version not yet in force), fetched once.
            ehds = ('32025R0327', '2025-02-11')
            routes = {'MevzuatNo=6698': self.MEVZUAT_HTML,
                      '%2232016R0679%22': self.sparql('02016R0679-20160504', ('32016R0679R(02)', '2018-05-23'), ehds),
                      '%2232024R2847%22': self.sparql('02024R2847-20241120', ('32024R2847R(05)', '2026-10-01'), ehds,
                                                      ('32024R2847R(01)', '2024-12-05', '02024R2847-20241120')),
                      '%2232024R1689%22': self.sparql('02024R1689-20260727', (*ehds, '02024R1689-20270802')),
                      '/resource/celex/32016R0679R%2802%29': urllib.error.URLError('gone'),
                      '/resource/celex/32024R2847R%2805%29': '<html><p>Corrigendum to (EU) 2024/2846</p></html>',
                      '/resource/celex/32025R0327': '<html><p>Regulation (EU) 2025/327</p></html>',
                      '/resource/celex/02016R0679-20160504': self.CELLAR_XHTML,
                      '/resource/celex/02024R2847-20241120': '<html><p>Regulation (EU) 2024/2847</p></html>',
                      '/resource/celex/02024R1689-20260727': '<html><p>Regulation (EU) 2024/1689</p></html>'}
            requests = []
            manifest = register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes, requests),
                                                     today=datetime.date(2026, 11, 1))
            self.assertEqual(1, sum(request.full_url.endswith('/32025R0327') for request in requests))
            published = next(request for request in requests if request.full_url.endswith('/32025R0327'))
            self.assertEqual(('application/xhtml+xml', 'eng'),
                             (published.get_header('Accept'), published.get_header('Accept-language')))
            items = {item['id']: item for item in manifest['items']}
            self.assertEqual(10, len(items))
            self.assertEqual('02016R0679-20160504 + 32016R0679R(02) + 32025R0327', items['celex-32016r0679']['version'])
            self.assertEqual(['celex-32016r0679r-02-published', 'celex-32025r0327-published'],
                             items['celex-32016r0679']['documents'])
            self.assertEqual('https://publications.europa.eu/resource/celex/02016R0679-20160504',
                             items['celex-32016r0679']['url'])
            self.assertEqual('02024R2847-20241120 + 32024R2847R(05) + 32025R0327',
                             items['celex-32024r2847']['version'])
            self.assertEqual({'id': 'celex-32025r0327-published', 'act': '32025R0327', 'reader': 'cellar',
                              'url': 'https://publications.europa.eu/resource/celex/32025R0327', 'file':
                              'celex-32025r0327-published', 'bytes': 44,
                              'sha256': hashlib.sha256(b'<html><p>Regulation (EU) 2025/327</p></html>').hexdigest()},
                             items['celex-32025r0327-published'])
            self.assertEqual('URLError: <urlopen error gone>', items['celex-32016r0679r-02-published']['error'])
            fetched = ('--fetched', str(root / 'fetched'))
            report = root / 'docs' / 'legal-review.md'
            code, output = self.tool(root, *argv, 'compare', *fetched, '--report', str(report), '--apply',
                                     today='2026-11-01')
            self.assertEqual(0, code, output)
            text = report.read_text(encoding='utf-8')
            self.assertIn('Sources: 7 planned and 3 documents their indexes name, 9 read on 2026-11-01;', text)
            self.assertIn('- eu.ai.literacy (NET.2): Law No. 6698, Articles 4-7: never read from its primary source; '
                          'read https://publications.europa.eu/resource/celex/02024R1689-20260727 (version '
                          '02024R1689-20260727 + 32025R0327, SHA-256 ', text)
            self.assertIn(') in its consolidated text and record it with `read`; read the acts its consolidated text '
                          'lacks as published (celex-32025r0327-published)\n', text)
            self.assertIn('- celex-32016r0679r-02-published (https://publications.europa.eu/resource/celex/'
                          '32016R0679R%2802%29): URLError: <urlopen error gone>', text)
            path = root / 'cmake' / 'legal-register.json'
            before = path.read_bytes()
            for entry, message in (
                    ('eu.gdpr.rights', 'eu.gdpr.rights: celex-32016r0679: the document it names, '
                                       'celex-32016r0679r-02-published, was not read (URLError: <urlopen error gone>)'),
                    ('eu.cra.reporting', 'eu.cra.reporting: celex-32024r2847: not the XHTML of 32024R2847R(05)')):
                code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#acts', '--entry',
                                         'eu.ai.literacy', '--entry', entry, today='2026-11-01')
                self.assertEqual(2, code, entry)
                self.assertIn(message, output)
                self.assertEqual(before, path.read_bytes(), 'a refused read writes nothing')
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#acts', '--entry',
                                     'eu.ai.literacy', today='2026-11-01')
            self.assertEqual(0, code, output)
            self.assertIn('eu.ai.literacy: read Law No. 6698 at 02024R1689-20260727 + 32025R0327 on 2026-11-01', output)
            entries = {entry['id']: entry for entry in json.loads(path.read_text(encoding='utf-8'))['entries']}
            self.assertEqual({'version': '02024R1689-20260727 + 32025R0327', 'read': '2026-11-01', 'sha256':
                              hashlib.sha256(b'<html><p>Regulation (EU) 2024/1689</p></html>').hexdigest()},
                             entries['eu.ai.literacy']['instrument']['consolidated'])
            checked = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 11, 1))}
            self.assertEqual([], checked['legal'])

    @staticmethod
    def uk_text(act, *effects):
        """A revised UK text valid on 2026-09-30 with its unapplied effects: (affecting URI, RequiresApplied, in-force
        days), a day None for a prospective one, as legislation.gov.uk's Crown Legislation Markup carries them."""
        def effect(number, uri, owed, days):
            in_force = ''.join('<ukm:InForce Prospective="true" Qualification=""/>' if day is None else
                               f'<ukm:InForce Date="{day}" Qualification="wholly in force"/>' for day in days)
            return (f'<ukm:UnappliedEffect EffectId="key-{number}" RequiresApplied="{owed}" AffectingURI="{uri}" '
                    f'Type="words substituted"><ukm:InForceDates>{in_force}</ukm:InForceDates></ukm:UnappliedEffect>')
        return ('<Legislation xmlns="http://www.legislation.gov.uk/namespaces/legislation"><ukm:Metadata '
                'xmlns:ukm="http://www.legislation.gov.uk/namespaces/metadata" '
                'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dct="http://purl.org/dc/terms/">'
                f'<dc:identifier>http://www.legislation.gov.uk/{act}</dc:identifier><dct:valid>2026-09-30</dct:valid>'
                '<ukm:UnappliedEffects>' + ''.join(effect(number, *item) for number, item in enumerate(effects)) +
                '</ukm:UnappliedEffects></ukm:Metadata></Legislation>')

    @staticmethod
    def uk_published(act, status='final'):
        return ('<Legislation xmlns="http://www.legislation.gov.uk/namespaces/legislation"><ukm:Metadata '
                'xmlns:ukm="http://www.legislation.gov.uk/namespaces/metadata" '
                f'xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier>http://www.legislation.gov.uk/{act}'
                f'</dc:identifier><ukm:DocumentStatus Value="{status}"/></ukm:Metadata></Legislation>')

    def test_a_revised_uk_text_names_the_acts_in_force_it_has_not_applied(self):
        effects = register_review.read_legislation_gov_uk_effects
        uk, unreadable = {'act': 'ukpga/2023/50'}, register_review.Unreadable
        crime = 'http://www.legislation.gov.uk/id/ukpga/2026/20'
        # The Online Safety Act's owed effects of the Crime and Policing Act 2026, commenced from 2026-11-06 (the
        # 2026-11-01 read), beside a conditional amendment its editors do not owe and a prospective one.
        text = self.uk_text('ukpga/2023/50', (crime, 'true', (None, '2026-11-06')), (crime, 'true', ('2026-11-06',)),
                            ('http://www.legislation.gov.uk/id/uksi/2026/386', 'false', ('2026-09-30',)),
                            ('http://www.legislation.gov.uk/id/nia/2022/18', 'true', (None,))).encode()
        self.assertEqual('2026-09-30', effects(text, uk, datetime.date(2026, 11, 5)))
        self.assertEqual(('2026-09-30', '2026-09-30 + ukpga/2026/20', [
            {'id': 'legislation.gov.uk-ukpga-2026-20-enacted', 'act': 'ukpga/2026/20/enacted', 'headers': {},
             'reader': 'legislation-gov-uk-published',
             'url': 'https://www.legislation.gov.uk/ukpga/2026/20/enacted/data.xml'}]),
            effects(text, uk, datetime.date(2026, 11, 6)))
        self.assertEqual('2026-09-30', effects(self.LEGISLATION_XML.encode(), {'act': 'ukpga/2018/12'},
                                               datetime.date(2026, 11, 6)), 'a text without unapplied effects')
        for uri, owed, days, message in (
                (crime, 'yes', ('2026-11-06',), "an unapplied effect whose RequiresApplied is 'yes'"),
                (crime, 'true', (), 'the unapplied effect key-0 has no in-force day'),
                (crime, 'true', ('2026-11',), 'the unapplied effect key-0 has an in-force day that is not YYYY-MM-DD'),
                (crime, 'true', ('2026-02-30',), 'the day 30/02/2026 does not exist'),
                ('http://www.legislation.gov.uk/id/ukmo/2026/1', 'true', ('2026-01-01',),
                 "the unapplied effect key-0 names 'http://www.legislation.gov.uk/id/ukmo/2026/1', not an act whose "
                 'published form is known'),
                ('https://www.legislation.gov.uk/ukpga/2026/20', 'true', ('2026-01-01',), 'not an act whose')):
            with self.assertRaises(unreadable) as refused:
                effects(self.uk_text('ukpga/2023/50', (uri, owed, days)).encode(), uk, datetime.date(2026, 11, 6))
            self.assertIn(message, str(refused.exception))
        read = register_review.read_legislation_gov_uk
        self.assertEqual('2026-09-30 + ukpga/2026/20', read(text, uk, '2026-09-30 + ukpga/2026/20'))
        with self.assertRaises(unreadable) as refused:
            read(text, uk, '2026-10-01 + ukpga/2026/20')
        self.assertIn('the version 2026-10-01 + ukpga/2026/20 is not of the text read, valid on 2026-09-30',
                      str(refused.exception))
        published = register_review.read_legislation_gov_uk_published
        made = {'act': 'uksi/2026/386/made'}
        self.assertIsNone(published(self.uk_published('uksi/2026/386/made').encode(), made, None))
        for body, message in ((self.uk_published('uksi/2026/386/made', 'revised'), 'not the final text of uksi/'),
                              (self.uk_published('uksi/2026/386'), 'not the data of uksi/2026/386/made'),
                              (self.uk_published('uksi/2026/386/made').replace('ukm:DocumentStatus', 'ukm:Status'),
                               'not the final text of uksi/2026/386/made')):
            with self.assertRaises(unreadable) as refused:
                published(body.encode(), made, None)
            self.assertIn(message, str(refused.exception))

    def test_the_acts_a_revised_uk_text_has_not_applied_are_read_as_published_before_it_is_recorded(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            legal = self.legal_register([
                self.legal_entry(id=entry_id, instrument=self.instrument(
                    jurisdiction='GB', primary=f'https://www.legislation.gov.uk/{act}')) for entry_id, act in (
                    ('gb.dpa.rights', 'ukpga/2018/12'), ('gb.osa.duties', 'ukpga/2023/50'),
                    ('gb.samla.screening', 'ukpga/2018/13'))],
                jurisdictions={'GB': {'name': 'United Kingdom', 'hosts': ['legislation.gov.uk']}})
            self.write_tree(root, self.register(self.entries()), legal)
            (root / 'docs' / 'reading.md').write_text('<a id="uk"></a>Read.\n', encoding='utf-8')
            argv = ('--register', 'cmake/legal-register.json')
            code, output = self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))
            self.assertEqual(0, code, output)
            self.assertIn('  and, as published, each act in force a revised UK text has not applied '
                          '(https://www.legislation.gov.uk/<act>/<enacted|made>/data.xml)', output)
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            crime, regulations = ('http://www.legislation.gov.uk/id/ukpga/2026/20',
                                  'http://www.legislation.gov.uk/id/uksi/2026/386')
            scottish = 'http://www.legislation.gov.uk/id/asp/2025/1'
            # Two texts lack the Crime and Policing Act 2026, fetched once; the DPA's instrument cannot be served and
            # the Scottish act answers with another act's text.
            routes = {'/ukpga/2018/12/data.xml': self.uk_text('ukpga/2018/12', (crime, 'true', ('2026-11-06',)),
                                                              (regulations, 'true', ('2026-10-01',))),
                      '/ukpga/2023/50/data.xml': self.uk_text('ukpga/2023/50', (crime, 'true', (None, '2026-11-06'))),
                      '/ukpga/2018/13/data.xml': self.uk_text('ukpga/2018/13', (scottish, 'true', ('2025-04-01',))),
                      '/ukpga/2026/20/enacted/data.xml': self.uk_published('ukpga/2026/20/enacted'),
                      '/uksi/2026/386/made/data.xml': urllib.error.URLError('gone'),
                      '/asp/2025/1/enacted/data.xml': self.uk_published('asp/2025/2/enacted')}
            requests = []
            manifest = register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes, requests),
                                                     today=datetime.date(2026, 11, 6))
            self.assertEqual(1, sum(request.full_url.endswith('/ukpga/2026/20/enacted/data.xml')
                                    for request in requests))
            items = {item['id']: item for item in manifest['items']}
            self.assertEqual(6, len(items))
            dpa = items['legislation.gov.uk-ukpga-2018-12']
            self.assertEqual('2026-09-30 + ukpga/2026/20 + uksi/2026/386', dpa['version'])
            self.assertEqual(['legislation.gov.uk-ukpga-2026-20-enacted', 'legislation.gov.uk-uksi-2026-386-made'],
                             dpa['documents'])
            self.assertEqual('2026-09-30 + ukpga/2026/20', items['legislation.gov.uk-ukpga-2023-50']['version'])
            self.assertEqual('URLError: <urlopen error gone>', items['legislation.gov.uk-uksi-2026-386-made']['error'])
            self.assertEqual({'id', 'url', 'act', 'reader', 'file', 'bytes', 'sha256'},
                             set(items['legislation.gov.uk-ukpga-2026-20-enacted']))
            fetched = ('--fetched', str(root / 'fetched'))
            report = root / 'docs' / 'legal-review.md'
            code, output = self.tool(root, *argv, 'compare', *fetched, '--report', str(report), '--apply',
                                     today='2026-11-06')
            self.assertEqual(0, code, output)
            text = report.read_text(encoding='utf-8')
            self.assertIn('Sources: 3 planned and 3 documents their indexes name, 5 read on 2026-11-06;', text)
            self.assertIn('- gb.osa.duties (NET.2): Law No. 6698, Articles 4-7: never read from its primary source; '
                          'read https://www.legislation.gov.uk/ukpga/2023/50/data.xml (version 2026-09-30 + '
                          'ukpga/2026/20, SHA-256 ', text)
            self.assertIn('record it with `read`; read the acts its consolidated text lacks as published '
                          '(legislation.gov.uk-ukpga-2026-20-enacted)\n', text)
            path = root / 'cmake' / 'legal-register.json'
            before = path.read_bytes()
            for entry, message in (
                    ('gb.dpa.rights', 'gb.dpa.rights: legislation.gov.uk-ukpga-2018-12: the document it names, '
                                      'legislation.gov.uk-uksi-2026-386-made, was not read (URLError: <urlopen error '
                                      'gone>)'),
                    ('gb.samla.screening', 'gb.samla.screening: legislation.gov.uk-ukpga-2018-13: not the data of '
                                           'asp/2025/1/enacted')):
                code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#uk', '--entry',
                                         'gb.osa.duties', '--entry', entry, today='2026-11-06')
                self.assertEqual(2, code, entry)
                self.assertIn(message, output)
                self.assertEqual(before, path.read_bytes(), 'a refused read writes nothing')
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#uk', '--entry',
                                     'gb.osa.duties', today='2026-11-06')
            self.assertEqual(0, code, output)
            self.assertIn('gb.osa.duties: read Law No. 6698 at 2026-09-30 + ukpga/2026/20 on 2026-11-06', output)
            entries = {entry['id']: entry for entry in json.loads(path.read_text(encoding='utf-8'))['entries']}
            self.assertEqual('2026-09-30 + ukpga/2026/20',
                             entries['gb.osa.duties']['instrument']['consolidated']['version'])

    def test_each_host_resolves_to_the_document_its_reader_reads(self):
        resolve = register_review.resolve_primary
        self.assertEqual([{'id': 'mevzuat-1-3-5846', 'kind': 'primary', 'reader': 'mevzuat', 'act': '5846',
                           'url': 'https://www.mevzuat.gov.tr/anasayfa/MevzuatFihristDetayIframe?MevzuatTur=1&'
                                  'MevzuatNo=5846&MevzuatTertip=3'}],
                         resolve('https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=5846&MevzuatTur=1&MevzuatTertip=3'))
        index, document = resolve('https://eur-lex.europa.eu/eli/dir/2002/58/oj')
        self.assertEqual(('celex-32002l0058-versions', 'index', 'cellar-versions', '32002L0058'),
                         (index['id'], index['kind'], index['reader'], index['act']))
        query = urllib.parse.unquote(index['url'].partition('&query=')[2])
        english = 'cdm:expression_uses_language <http://publications.europa.eu/resource/authority/language/ENG>'
        self.assertIn('?act cdm:resource_legal_id_celex "32002L0058"^^<http://www.w3.org/2001/XMLSchema#string> . '
                      '{ ?work cdm:act_consolidated_based_on_resource_legal ?act', query)
        self.assertIn(f'?expression cdm:expression_belongs_to_work ?work ; {english}', query)
        self.assertIn('FILTER(STR(?type) = "xhtml")', query)
        # The acts changing its English text: amending it, correcting it or correcting an act amending it, with an
        # English text, and every consolidated version of the act that consolidates each.
        self.assertIn('{ ?change cdm:resource_legal_amends_resource_legal ?act } UNION { ?change '
                      'cdm:resource_legal_corrects_resource_legal ?act } UNION { ?change '
                      'cdm:resource_legal_corrects_resource_legal ?amending . ?amending '
                      'cdm:resource_legal_amends_resource_legal ?act }', query)
        self.assertIn(f'?text cdm:expression_belongs_to_work ?change ; {english}', query)
        self.assertIn('OPTIONAL { ?consolidation cdm:act_consolidated_based_on_resource_legal ?act ; '
                      'cdm:act_consolidated_consolidates_resource_legal ?change ; cdm:resource_legal_id_celex ?by }',
                      query)
        self.assertEqual({'id': 'celex-32002l0058', 'kind': 'primary', 'reader': 'cellar', 'act': '32002L0058',
                          'index': 'celex-32002l0058-versions', 'url': register_review.CELLAR,
                          'headers': {'Accept': 'application/xhtml+xml', 'Accept-Language': 'eng'}}, document)
        self.assertEqual('32024R2847', resolve('https://eur-lex.europa.eu/eli/reg/2024/2847/oj')[0]['act'])
        self.assertEqual('32022D0001', resolve('https://eur-lex.europa.eu/eli/dec/2022/1/oj')[0]['act'])
        self.assertEqual([{'id': 'legislation.gov.uk-ukpga-2018-12', 'kind': 'primary',
                           'reader': 'legislation-gov-uk', 'act': 'ukpga/2018/12',
                           'url': 'https://www.legislation.gov.uk/ukpga/2018/12/data.xml'}],
                         resolve('https://www.legislation.gov.uk/ukpga/2018/12'))
        self.assertEqual([{'id': 'legislation.gov.uk-eur-2016-679', 'kind': 'primary', 'reader': 'legislation-gov-uk',
                           'act': 'eur/2016/679', 'url': 'https://www.legislation.gov.uk/eur/2016/679/data.xml'}],
                         resolve('https://www.legislation.gov.uk/eur/2016/679'))
        index, document = resolve('https://www.ecfr.gov/current/title-15/subtitle-B/chapter-VII/subchapter-C')
        self.assertEqual({'id': 'ecfr-title-15-subtitle-b-chapter-vii-subchapter-c-versions', 'kind': 'index',
                          'reader': 'ecfr-versions', 'act': 'title=15&subtitle=B&chapter=VII&subchapter=C',
                          'url': 'https://www.ecfr.gov/api/versioner/v1/versions/title-15.json?subtitle=B&'
                                 'chapter=VII&subchapter=C'}, index)
        self.assertEqual('https://www.ecfr.gov/api/versioner/v1/full/{version}/title-15.xml?subtitle=B&chapter=VII&'
                         'subchapter=C', document['url'])
        self.assertEqual(index['id'], document['index'])
        index, document = resolve('https://www.wassenaar.org/control-lists/')
        self.assertEqual(('wassenaar-control-lists', 'https://www.wassenaar.org/control-lists/'),
                         (index['id'], index['url']))
        self.assertEqual(('pdf', 'https://www.wassenaar.org/wp-content/uploads/{version}'),
                         (document['reader'], document['url']))
        self.assertEqual([{'id': 'sanctionsmap-regimes', 'kind': 'primary', 'reader': 'sanctions-map', 'act': 'regime',
                           'url': 'https://www.sanctionsmap.eu/api/v1/regime',
                           'headers': {'Accept': 'application/json'}}],
                         resolve('https://www.sanctionsmap.eu/api/v1/regime'))
        for url in ('https://developer.apple.com/news/', 'https://www.sanctionsmap.eu/',
                    'http://www.sanctionsmap.eu/api/v1/regime', 'https://www.sanctionsmap.eu/api/v1/regime/26',
                    'https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=6698',
                    'https://eur-lex.europa.eu/eli/reg/2016/679/oj/eng', 'https://www.ecfr.gov/current/title-15',
                    'https://www.legislation.gov.uk/ukpga/2018/12/contents',
                    'http://www.legislation.gov.uk/ukpga/2018/12', 'https://www.legislation.gov.uk/eudr/2019/790',
                    'https://www.resmigazete.gov.tr/eskiler/2025/12/20251225.htm',
                    'https://www.resmigazete.gov.tr/eskiler/2025/12/20251125-21.pdf',
                    'https://www.resmigazete.gov.tr/eskiler/2025/12/20251225-0.pdf',
                    'https://www.resmigazete.gov.tr/eskiler/2025/12/20251225-21.doc',
                    'http://www.resmigazete.gov.tr/eskiler/2025/12/20251225-21.pdf'):
            self.assertEqual([{'id': register_review.source_id(url), 'kind': 'primary', 'url': url}], resolve(url),
                             url)

    def test_fetch_reads_the_index_first_decodes_gzip_and_bounds_every_body(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            sources = [dict(source, entries=['e'], registries=[])
                       for url in ('https://eur-lex.europa.eu/eli/reg/2016/679/oj',
                                   'https://www.ecfr.gov/current/title-16/chapter-I/subchapter-C/part-312',
                                   'https://www.wassenaar.org/control-lists/')
                       for source in register_review.resolve_primary(url)]
            plan = {'schema': register_review.PLAN_SCHEMA, 'register': 'cmake/legal-register.json',
                    'sources': sources, 'manual': []}
            xhtml = gzip.compress(self.CELLAR_XHTML.encode())
            ecfr = json.dumps({'meta': {'title': '16', 'chapter': 'I', 'subchapter': 'C', 'part': '312',
                                        'latest_amendment_date': '2025-06-23'}})
            routes = {'webapi/rdf/sparql': (gzip.compress(self.sparql().encode()), {'Content-Encoding': 'gzip'}),
                      '/resource/celex/32016R0679': (xhtml, {'Content-Encoding': 'GZIP '}),
                      'versions/title-16.json': ecfr,
                      'full/2025-06-23/title-16.xml': ('<DIV5 N="312"/>', {'Content-Encoding': 'br'}),
                      'control-lists': 'https://www.wassenaar.org/wp-content/uploads/2026/07/'
                                       'List-of-Dual-Use-Goods-and-Technologies-and-ML-2025-Corr.pdf',
                      '-Corr.pdf': (gzip.compress(b'%PDF-1.7')[:-6], {'Content-Encoding': 'gzip'})}
            manifest = register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes),
                                                     today=datetime.date(2026, 12, 16))
            items = {item['id']: item for item in manifest['items']}
            self.assertEqual(['index'] * 3, [source['kind'] for source in sources
                                             for item in manifest['items'][:3] if item['id'] == source['id']])
            self.assertEqual('32016R0679', items['celex-32016r0679']['version'])
            self.assertEqual(hashlib.sha256(self.CELLAR_XHTML.encode()).hexdigest(),
                             items['celex-32016r0679']['sha256'])
            self.assertEqual(self.CELLAR_XHTML.encode(), (root / 'fetched' / 'celex-32016r0679').read_bytes())
            self.assertEqual("the unrequested content encoding 'br'",
                             items['ecfr-title-16-chapter-i-subchapter-c-part-312']['error'])
            self.assertEqual('a truncated gzip body', items['wassenaar-dual-use-list']['error'])
            self.assertEqual('2026/07/List-of-Dual-Use-Goods-and-Technologies-and-ML-2025-Corr.pdf',
                             items['wassenaar-dual-use-list']['version'])
            routes.update({'webapi/rdf/sparql': ('not gzip', {'Content-Encoding': 'gzip'}),
                           'versions/title-16.json': ecfr.replace('2025-06-23', '../2025'),
                           'control-lists': '<html>no list</html>'})
            manifest = register_review.fetch_sources(plan, root / 'unreadable', opener=self.routed(routes),
                                                     today=datetime.date(2026, 12, 16))
            items = {item['id']: item for item in manifest['items']}
            self.assertIn('a broken gzip body', items['celex-32016r0679-versions']['error'])
            self.assertEqual("the latest amendment day '../2025' is not YYYY-MM-DD",
                             items['ecfr-title-16-chapter-i-subchapter-c-part-312-versions']['error'])
            self.assertEqual('0 links to the List-of-Dual-Use-Goods-and-Technologies PDF, not one',
                             items['wassenaar-control-lists']['error'])
            for document, index in (('celex-32016r0679', 'celex-32016r0679-versions'),
                                    ('wassenaar-dual-use-list', 'wassenaar-control-lists')):
                self.assertEqual(f'its version index {index} was not read', items[document]['error'])
                self.assertNotIn('file', items[document])
            routes['webapi/rdf/sparql'] = (gzip.compress(b'x' * 65), {'Content-Encoding': 'gzip'})
            with patch.object(register_review, 'FETCH_LIMIT', 64):
                manifest = register_review.fetch_sources(plan, root / 'bomb', opener=self.routed(routes),
                                                         today=datetime.date(2026, 12, 16))
            self.assertEqual('larger than 64 bytes', manifest['items'][0]['error'])
            with patch.object(register_review, 'INDEX_READERS', dict(register_review.INDEX_READERS,
                                                                     **{'wassenaar-lists': lambda *_: 'a/../b'})):
                manifest = register_review.fetch_sources(plan, root / 'escape', opener=self.routed(routes),
                                                         today=datetime.date(2026, 12, 16))
            items = {item['id']: item for item in manifest['items']}
            self.assertEqual("the version 'a/../b' cannot be part of a URL", items['wassenaar-control-lists']['error'])

    def test_redirects_stay_on_the_host_and_are_upgraded_to_https(self):
        handler = register_review.SecureRedirects()
        request = urllib.request.Request('https://publications.europa.eu/resource/celex/32016R0679',
                                         headers={'Accept': 'application/xhtml+xml', 'User-Agent': 'u'})
        upgraded = handler.redirect_request(request, None, 303, 'See Other', {},
                                            'http://publications.europa.eu/resource/cellar/abc/DOC_1')
        self.assertEqual('https://publications.europa.eu/resource/cellar/abc/DOC_1', upgraded.full_url)
        self.assertEqual('application/xhtml+xml', upgraded.get_header('Accept'))
        self.assertEqual('https://publications.europa.eu/resource/x',
                         handler.redirect_request(request, None, 302, 'Found', {}, '/resource/x').full_url)
        for target in ('https://main.un.org/securitycouncil/', 'http://evil.example/resource', 'ftp://publications.'
                       'europa.eu/x', 'https://publications.europa.eu.evil.example/x'):
            response = io.BytesIO(b'moved')
            with self.assertRaises(urllib.error.URLError) as refused:
                handler.redirect_request(request, response, 301, 'Moved', {}, target)
            self.assertIn(f'refused a redirect to {target}', str(refused.exception))
            self.assertTrue(response.closed)

    # A by-law, regulation or communiqué on mevzuat.gov.tr (the 2026-11-01 reads): the cited page names its Official
    # Gazette; the consolidated text has its title, the amending gazettes' notes and, once amended, a closing table of
    # its own gazette and the amending ones (empty trailing rows included).
    BYLAW_CITED = ('<html><body><div>Anasayfa</div><span>Resmî Gazete Tarihi: 27.11.2014</span>'
                   '<span>Resmî Gazete Sayısı: 29188</span><p>* Kanunlar Fihristinde ...</p></body></html>')

    @staticmethod
    def bylaw(*notes, form='YÖNETMELİĞİ', table=('27/11/2014 29188', '1. 9/2/2019 30681 2. 23/8/2022 31932 3.')):
        title = f'<p>MESAFELİ SÖZLEŞMELER {form}</p><p>MADDE 1 – (1) Bu Yönetmeliğin amacı ...</p>'
        body = ''.join(f'<p>{note}</p>' for note in notes)
        closing = (f'<table><tr><td>Yönetmeliğin Yayımlandığı Resmî Gazete’nin</td><td>Tarihi</td><td>Sayısı</td></tr>'
                   f'<tr><td>{table[0]}</td></tr><tr><td>Yönetmelikte Değişiklik Yapan Yönetmeliklerin Yayımlandığı '
                   f'Resmî Gazetelerin</td><td>Tarihi</td><td>Sayısı</td></tr><tr><td>{table[1]}</td></tr></table>'
                   '<p>EK ÖRNEK CAYMA FORMU</p>') if table else ''
        return (title + body + closing).encode()

    def test_mevzuat_by_laws_are_known_by_the_gazette_that_published_them(self):
        resolve = register_review.resolve_primary
        index, document = resolve('https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=20237&MevzuatTur=7&MevzuatTertip=5')
        self.assertEqual({'id': 'mevzuat-7-5-20237-gazette', 'kind': 'index', 'reader': 'mevzuat-gazette',
                          'act': '20237',
                          'url': 'https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=20237&MevzuatTur=7&MevzuatTertip=5'},
                         index)
        self.assertEqual({'id': 'mevzuat-7-5-20237', 'kind': 'primary', 'reader': 'mevzuat', 'act': '20237',
                          'url': 'https://www.mevzuat.gov.tr/anasayfa/MevzuatFihristDetayIframe?MevzuatTur=7&'
                                 'MevzuatNo=20237&MevzuatTertip=5', 'form': 'YÖNETMELİ',
                          'index': 'mevzuat-7-5-20237-gazette'}, document)
        self.assertEqual('TEBLİĞ', resolve('https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=24454&MevzuatTur=9&'
                                           'MevzuatTertip=5')[1]['form'])
        self.assertEqual([{'id': 'mevzuat-21-5-2837', 'kind': 'primary',
                           'url': 'https://www.mevzuat.gov.tr/anasayfa/MevzuatFihristDetayIframe?MevzuatTur=21&'
                                  'MevzuatNo=2837&MevzuatTertip=5'}],
                         resolve('https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=2837&MevzuatTur=21&MevzuatTertip=5'))
        unreadable = register_review.Unreadable
        cited = register_review.read_mevzuat_gazette
        source = {'act': '20237'}
        self.assertEqual('2014-11-27/29188', cited(self.BYLAW_CITED.encode(), source, None))
        self.assertEqual('2020-12-31/31351-m5', cited(self.BYLAW_CITED.replace('27.11.2014', '31.12.2020').replace(
            '29188', '31351 (5. Mükerrer)').encode(), source, None))
        for body, message in ((b'<html>Anasayfa</html>', '0 Official Gazettes of publication on the cited page of '
                                                         'No. 20237, not one'),
                              ((self.BYLAW_CITED + self.BYLAW_CITED.replace('29188', '29189')).encode(), '2 Official'),
                              (self.BYLAW_CITED.replace('27.11.2014', '31.11.2014').encode(),
                               'the day 31/11/2014 does not exist')):
            with self.assertRaises(unreadable) as refused:
                cited(body, source, None)
            self.assertIn(message, str(refused.exception))
        read = register_review.read_mevzuat
        regulation = {'act': '20237', 'form': 'YÖNETMELİ'}
        published = '2014-11-27/29188'
        self.assertEqual('2014-11-27/29188 as published', read(self.bylaw(table=None), regulation, published))
        self.assertEqual('2014-11-27/29188 at 2022-08-23 (RG 31932)', read(self.bylaw(), regulation, published))
        self.assertEqual('2014-11-27/29188 at 2024-05-24 (RG 32909)', read(self.bylaw(
            '(Değişik ibare:RG-24/5/2024-32909)', '(Ek:RG-23/8/2022-31932)'), regulation, published))
        # The table lists an amendment no note shows; a repeated issue keeps its number.
        self.assertEqual('2014-11-27/29188 at 2025-12-31 (RG 33124-m5)', read(self.bylaw(table=(
            '27/11/2014 29188', '1 9/2/2019 30681 2 31/12/2025 33124 (5.Mükerrer) 3')), regulation, published))
        # A court annulment counts on its decision's day, whatever days the annulled text it quotes holds.
        council = ('(Danıştay Onuncu Dairesinin 6/5/2026 tarihli ve E.:2022/5534; K.:2026/2753 sayılı kararı ile iptal '
                   'bent: 1/1/2030 tarihinden sonra kurulan sözleşmeler. )')
        self.assertEqual('2014-11-27/29188 at 2026-05-06 (Danıştay)', read(self.bylaw(council), regulation, published))
        communique = {'act': '24454', 'form': 'TEBLİĞ'}
        self.assertEqual('2014-11-27/29188 at 2022-08-23 (RG 31932)', read(self.bylaw(form='TEBLİĞİ'), communique,
                                                                            published))
        for body, source, gazette, message in (
                (self.bylaw(), regulation, '2014-11-27/29189', 'published in the Official Gazette 2014-11-27/29188, '
                                                               'not 2014-11-27/29189 as its cited page says'),
                (self.bylaw(), communique, published, 'not a TEBLİĞ text: its title does not name one'),
                (self.bylaw(table=None), regulation, None, 'the Official Gazette of No. 20237 is unknown; its cited '
                                                           'page is read first'),
                (self.bylaw('(Değişik:RG-29/2/2023-32000)'), regulation, published,
                 'the day 29/2/2023 does not exist')):
            with self.assertRaises(unreadable) as refused:
                read(body, source, gazette)
            self.assertIn(message, str(refused.exception))
        law = '<p>Kanun Numarası : 5378</p><p>Kabul Tarihi : 1/7/2005</p><p>{}</p>'
        court = ('(İptal üçüncü cümle: Anayasa Mahkemesinin 22/6/2023 Tarihli ve E: 2022/110, K: 2023/115 Sayılı '
                 'Kararı ile.)')
        for note, version in ((court, '2023-06-22 (Anayasa Mahkemesi)'),
                              (court.replace('ile.)', 'ile; 1/1/2030 tarihinde yürürlüğe girer.)'),
                               '2023-06-22 (Anayasa Mahkemesi)'),
                              ('(Anayasa Mahkemesinin 18/1/2024 tarihli ve E.: 2021/28, K.: 2024/11 sayılı Kararı ile '
                               'iptal: 1/3/2030 tarihine kadar uygulanır.)', '2024-01-18 (Anayasa Mahkemesi)')):
            self.assertEqual(version, read(law.format('(Ek fıkra: 4/7/2012-6353/34 md.) ' + note).encode(),
                                           {'act': '5378'}, None), note)

    def test_read_records_a_by_law_by_its_gazette_and_refuses_one_its_cited_page_contradicts(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            cited = 'https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=20237&MevzuatTur=7&MevzuatTertip=5'
            other = 'https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=24454&MevzuatTur=9&MevzuatTertip=5'
            self.write_tree(root, self.register(self.entries()), self.legal_register([
                self.legal_entry(),
                self.legal_entry(id='tr.6502.distance-contracts-regulation', instrument=self.instrument(
                    name='Regulation on Distance Contracts', type='regulation', primary=cited)),
                self.legal_entry(id='tr.kvkk.notice-communique', instrument=self.instrument(
                    name='Communiqué on the obligation to inform', type='by-law', primary=other))]))
            (root / 'docs' / 'reading.md').write_text('<a id="r"></a>Read.\n', encoding='utf-8')
            argv = ('--register', 'cmake/legal-register.json')
            self.assertEqual(0, self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))[0])
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            sources = {source['id']: source for source in plan['sources']}
            self.assertEqual('mevzuat-7-5-20237-gazette', sources['mevzuat-7-5-20237']['index'])
            self.assertEqual(['tr.6502.distance-contracts-regulation'], sources['mevzuat-7-5-20237']['entries'])
            routes = {'MevzuatNo=6698': self.MEVZUAT_HTML,
                      'mevzuat?MevzuatNo=20237': self.BYLAW_CITED,
                      'Iframe?MevzuatTur=7&MevzuatNo=20237': self.bylaw(),
                      'mevzuat?MevzuatNo=24454': self.BYLAW_CITED.replace('29188', '30356'),
                      'Iframe?MevzuatTur=9&MevzuatNo=24454': self.bylaw(form='TEBLİĞ')}
            manifest = register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes),
                                                     today=datetime.date(2026, 12, 16))
            items = {item['id']: item for item in manifest['items']}
            self.assertEqual('2014-11-27/29188', items['mevzuat-7-5-20237']['version'])
            self.assertEqual(routes['Iframe?MevzuatTur=7&MevzuatNo=20237'],
                             (root / 'fetched' / items['mevzuat-7-5-20237']['file']).read_bytes())
            fetched = ('--fetched', str(root / 'fetched'))
            report = root / 'docs' / 'legal-review.md'
            code, output = self.tool(root, *argv, 'compare', *fetched, '--report', str(report), '--apply')
            self.assertEqual(0, code, output)
            text = report.read_text(encoding='utf-8')
            self.assertIn('Regulation on Distance Contracts, Articles 4-7: never read from its primary source; read '
                          'https://www.mevzuat.gov.tr/anasayfa/MevzuatFihristDetayIframe?MevzuatTur=7&MevzuatNo=20237&'
                          'MevzuatTertip=5 (version 2014-11-27/29188 at 2022-08-23 (RG 31932), SHA-256 ', text)
            self.assertIn('- mevzuat-9-5-24454: published in the Official Gazette 2014-11-27/29188, not '
                          '2014-11-27/30356 as its cited page says', text)
            path = root / 'cmake' / 'legal-register.json'
            before = path.read_bytes()
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#r', '--entry',
                                     'tr.6502.distance-contracts-regulation', '--entry', 'tr.kvkk.notice-communique')
            self.assertEqual(2, code)
            self.assertIn('tr.kvkk.notice-communique: mevzuat-9-5-24454: published in the Official Gazette '
                          '2014-11-27/29188, not 2014-11-27/30356', output)
            self.assertEqual(before, path.read_bytes(), 'a refused read writes nothing')
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#r', '--entry',
                                     'tr.6502.distance-contracts-regulation')
            self.assertEqual(0, code, output)
            entries = {entry['id']: entry for entry in json.loads(path.read_text(encoding='utf-8'))['entries']}
            self.assertEqual({'version': '2014-11-27/29188 at 2022-08-23 (RG 31932)', 'read': '2026-12-16',
                              'sha256': hashlib.sha256(routes['Iframe?MevzuatTur=7&MevzuatNo=20237']).hexdigest()},
                             entries['tr.6502.distance-contracts-regulation']['instrument']['consolidated'])
            checked = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 12, 16))}
            self.assertEqual([], checked['legal'])

    # The Official Gazette's day page (the pages of 2017 and 2025, 2026-11-01): windows-1254, its heading, and one link
    # per item whose title names a decision's number, the closing parenthesis sometimes missing.
    GAZETTE_DAY = ('<html><head><meta http-equiv="Content-Type" content="text/html; charset=windows-1254"></head>'
                   '<body><p>T.C. Resmî Gazete</p><p>25 Aralık 2025 Tarihli ve 33118 Sayılı Resmî Gazete</p>'
                   '<p><a href="20251225-12.pdf">–– Bazı Cumhurbaşkanlığı Kararnamelerinde Değişiklik Yapılmasına Dair '
                   'Cumhurbaşkanlığı Kararnamesi (Kararname Numarası: 191)</a></p>'
                   '<p><a href="20251225-21.pdf">–– 7194 Sayılı Dijital Hizmet Vergisi ... Oranının Yeniden '
                   'Belirlenmesi Hakkında Karar (Karar Sayısı: 10767</a></p>'
                   '<p><a href="20251225-1.htm">–– Genel Aydınlatma Yönetmeliğinde Değişiklik Yapılmasına Dair '
                   'Yönetmelik</a></p><a href="http://www.resmigazete.gov.tr/eskiler/2025/12/20251224.htm">ÖNCEKİ</a>'
                   '</body></html>')

    def test_gazette_items_are_known_by_their_day_page(self):
        resolve = register_review.resolve_primary
        index, document = resolve('https://www.resmigazete.gov.tr/eskiler/2025/12/20251225-21.pdf')
        self.assertEqual({'id': 'resmigazete-20251225-21-day', 'kind': 'index', 'reader': 'gazette-day',
                          'act': '20251225-21.pdf', 'url': 'https://www.resmigazete.gov.tr/eskiler/2025/12/20251225.htm'},
                         index)
        self.assertEqual({'id': 'resmigazete-20251225-21', 'kind': 'primary', 'reader': 'gazette',
                          'act': '20251225-21.pdf', 'index': 'resmigazete-20251225-21-day',
                          'url': 'https://www.resmigazete.gov.tr/eskiler/2025/12/20251225-21.pdf'}, document)
        index, document = resolve('https://www.resmigazete.gov.tr/eskiler/2025/12/20251231M5-1.htm')
        self.assertEqual(('resmigazete-20251231m5-1-day', 'https://www.resmigazete.gov.tr/eskiler/2025/12/20251231M5.htm',
                          'resmigazete-20251231m5-1', '20251231M5-1.htm'),
                         (index['id'], index['url'], document['id'], document['act']))
        day = register_review.read_gazette_day
        page = self.GAZETTE_DAY.encode('cp1254')
        decision = {'act': '20251225-21.pdf'}
        self.assertEqual('2025-12-25/33118/10767', day(page, decision, None))
        self.assertEqual('2025-12-25/33118/191', day(page, {'act': '20251225-12.pdf'}, None))
        self.assertEqual('2025-12-25/33118', day(page, {'act': '20251225-1.htm'}, None))
        repeated = self.GAZETTE_DAY.replace('25 Aralık', '31 Aralık').replace('33118 Sayılı Resmî Gazete',
                                                                                '33124 Sayılı Resmî Gazete - 5. Mükerrer')
        self.assertEqual('2025-12-31/33124-m5/10767', day(repeated.replace('20251225-21', '20251231M5-1').encode(
            'cp1254'), {'act': '20251231M5-1.pdf'}, None))
        unreadable = register_review.Unreadable
        for body, source, message in (
                (self.GAZETTE_DAY.encode(), decision, '0 Official Gazette headings'),
                (self.GAZETTE_DAY.replace('; charset=windows-1254', '').encode('cp1254'), decision,
                 '0 declared charsets'),
                (self.GAZETTE_DAY.replace('</head>', '<meta charset="utf-8"></head>').encode('cp1254'), decision,
                 '2 declared charsets'),
                (self.GAZETTE_DAY.replace('windows-1254', 'no-such-codec').encode('cp1254'), decision,
                 'not no-such-codec'),
                (page.replace(b'Tarihli ve', b'Tarihli'), decision, '0 Official Gazette headings'),
                (page + self.GAZETTE_DAY.replace('33118', '33119').encode('cp1254'), decision, '2 Official Gazette'),
                (self.GAZETTE_DAY.replace('Aralık', 'December').encode('cp1254'), decision,
                 "'December' is not a Turkish month"),
                (self.GAZETTE_DAY.replace('25 Aralık', '31 Şubat').encode('cp1254'), decision,
                 'the day 31/2/2025 does not exist'),
                (self.GAZETTE_DAY.replace('25 Aralık', '24 Aralık').encode('cp1254'), decision,
                 'the day page of the Official Gazette 33118 of 2025-12-24, not of 20251225-21.pdf'),
                (repeated.encode('cp1254'), {'act': '20251231-21.pdf'}, 'the day page of the Official Gazette 33124-m5'),
                (page, {'act': '20251225-22.pdf'}, '0 links to 20251225-22.pdf on its day page, not one'),
                (page + b'<a href="20251225-21.pdf">again</a>', decision, '2 links to 20251225-21.pdf'),
                (page.replace(b'10767', b'10767) (Karar Say\xfds\xfd: 10768'), decision,
                 'the title of 20251225-21.pdf names 2 decision numbers')):
            with self.assertRaises(unreadable) as refused:
                day(body, source, None)
            self.assertIn(message, str(refused.exception))
        read = register_review.read_gazette
        self.assertEqual('2025-12-25/33118 No. 10767 as published', read(b'%PDF-1.4', decision, '2025-12-25/33118/10767'))
        self.assertEqual('2025-12-31/33124-m5 as published', read(b'<HTML><body>Yonetmelik</body></HTML>',
                                                                   {'act': '20251231M5-1.htm'}, '2025-12-31/33124-m5'))
        for body, source, version, message in ((b'<html>', decision, '2025-12-25/33118', 'not a PDF'),
                                               (b'%PDF-1.4', {'act': '20251225-1.htm'}, '2025-12-25/33118',
                                                'not an HTML page'),
                                               (b'%PDF-1.4', decision, None, 'the Official Gazette of 20251225-21.pdf '
                                                                             'is unknown; its day page is read first')):
            with self.assertRaises(unreadable) as refused:
                read(body, source, version)
            self.assertIn(message, str(refused.exception))

    def test_read_records_a_gazette_decision_by_its_day_page(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            cited = 'https://www.resmigazete.gov.tr/eskiler/2025/12/20251225-21.pdf'
            other = 'https://www.resmigazete.gov.tr/eskiler/2025/12/20251225-22.pdf'
            hosts = {'TR': {'name': 'Türkiye', 'hosts': ['mevzuat.gov.tr', 'resmigazete.gov.tr']}}
            legal = self.legal_register([
                self.legal_entry(),
                self.legal_entry(id='tr.7194.rate-decision', instrument=self.instrument(
                    name='Presidential Decision No. 10767', type='decision', primary=cited)),
                self.legal_entry(id='tr.other-decision', instrument=self.instrument(
                    name='Presidential Decision No. 10768', type='decision', primary=other))])
            legal['jurisdictions'].update(hosts)
            self.write_tree(root, self.register(self.entries()), legal)
            (root / 'docs' / 'reading.md').write_text('<a id="r"></a>Read.\n', encoding='utf-8')
            argv = ('--register', 'cmake/legal-register.json')
            self.assertEqual(0, self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))[0])
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            sources = {source['id']: source for source in plan['sources']}
            # Two items of one day: each reads the day page for itself.
            self.assertEqual(['20251225-21.pdf', '20251225-22.pdf'], [sources[f'resmigazete-20251225-{item}-day']['act']
                                                                      for item in (21, 22)])
            pdf = b'%PDF-1.4\n%decision'
            routes = {'MevzuatNo=6698': self.MEVZUAT_HTML, '20251225.htm': self.GAZETTE_DAY.encode('cp1254'),
                      '20251225-21.pdf': pdf, '20251225-22.pdf': pdf}
            manifest = register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes),
                                                     today=datetime.date(2026, 12, 16))
            items = {item['id']: item for item in manifest['items']}
            self.assertEqual('2025-12-25/33118/10767', items['resmigazete-20251225-21']['version'])
            self.assertEqual('0 links to 20251225-22.pdf on its day page, not one',
                             items['resmigazete-20251225-22-day']['error'])
            self.assertEqual('its version index resmigazete-20251225-22-day was not read',
                             items['resmigazete-20251225-22']['error'])
            fetched = ('--fetched', str(root / 'fetched'))
            report = root / 'docs' / 'legal-review.md'
            code, output = self.tool(root, *argv, 'compare', *fetched, '--report', str(report), '--apply')
            self.assertEqual(0, code, output)
            self.assertIn('Presidential Decision No. 10767, Articles 4-7: never read from its primary source; read '
                          f'{cited} (version 2025-12-25/33118 No. 10767 as published, SHA-256 '
                          f'{hashlib.sha256(pdf).hexdigest()})', report.read_text(encoding='utf-8'))
            path = root / 'cmake' / 'legal-register.json'
            before = path.read_bytes()
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#r', '--entry',
                                     'tr.7194.rate-decision', '--entry', 'tr.other-decision')
            self.assertEqual(2, code)
            self.assertIn('tr.other-decision: resmigazete-20251225-22 was not read (its version index', output)
            self.assertEqual(before, path.read_bytes(), 'a refused read writes nothing')
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#r', '--entry',
                                     'tr.7194.rate-decision')
            self.assertEqual(0, code, output)
            entries = {entry['id']: entry for entry in json.loads(path.read_text(encoding='utf-8'))['entries']}
            self.assertEqual({'version': '2025-12-25/33118 No. 10767 as published', 'read': '2026-12-16',
                              'sha256': hashlib.sha256(pdf).hexdigest()},
                             entries['tr.7194.rate-decision']['instrument']['consolidated'])
            checked = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 12, 16))}
            self.assertEqual([], checked['legal'])

    # The Federal Register's record and raw text of an Executive order (the 2026-11-01 reads, cut to the fields read).
    FR_RECORD = {'document_number': '2022-05554', 'publication_date': '2022-03-15', 'citation': '87 FR 14381',
                 'raw_text_url': 'https://www.federalregister.gov/documents/full_text/text/2022/03/15/2022-05554.txt',
                 'executive_order_notes': 'See: EO 14024, April 15, 2021; Notice of March 24, 2026 (91 FR 15515)\r\n'
                                          'Amended by: EO 14114, December 12, 2023',
                 'corrections': [], 'type': 'Presidential Document'}
    FR_TEXT = ('<html><head><title>Federal Register, Volume 87 Issue 50</title></head><body><pre>\n'
               '[Federal Register Volume 87, Number 50 (Tuesday, March 15, 2022)]\n[Presidential Documents]\n'
               '[Pages 14381-14384]\n[FR Doc No: 2022-05554]\n\n Executive Order 14068 of March 11, 2022\n</pre>')

    def test_federal_register_documents_are_known_by_their_record(self):
        index, document = register_review.resolve_primary('https://www.federalregister.gov/d/2022-05554')
        self.assertEqual({'id': 'federalregister-2022-05554-record', 'kind': 'index',
                          'reader': 'federal-register-record', 'act': '2022-05554',
                          'url': 'https://www.federalregister.gov/api/v1/documents/2022-05554.json'}, index)
        self.assertEqual({'id': 'federalregister-2022-05554', 'kind': 'primary', 'reader': 'federal-register',
                          'act': '2022-05554', 'index': 'federalregister-2022-05554-record',
                          'url': 'https://www.federalregister.gov/documents/full_text/text/{version}.txt'}, document)
        self.assertEqual('federalregister-e7-2034', register_review.resolve_primary(
            'https://www.federalregister.gov/d/E7-2034')[1]['id'])
        for url in ('https://www.federalregister.gov/d/2022-05554/x', 'http://www.federalregister.gov/d/2022-05554',
                    'https://www.federalregister.gov/documents/2022/03/15/2022-05554/prohibiting',
                    'https://www.federalregister.gov/d/2022-5'):
            self.assertEqual([{'id': register_review.source_id(url), 'kind': 'primary', 'url': url}],
                             register_review.resolve_primary(url), url)
        record = register_review.read_federal_register_record
        source = {'act': '2022-05554'}
        self.assertEqual(('2022/03/15/2022-05554',
                          '2022-03-15 (87 FR 14381) as published; Amended by: EO 14114, December 12, 2023'),
                         record(json.dumps(self.FR_RECORD).encode(), source, None))
        determination = dict(self.FR_RECORD, executive_order_notes=None, corrections=[
            'https://www.federalregister.gov/api/v1/documents/C1-2022-05554'])
        self.assertEqual('2022-03-15 (87 FR 14381) as published; Corrected by: C1-2022-05554',
                         record(json.dumps(determination).encode(), source, None)[1])
        revoked = dict(self.FR_RECORD, executive_order_notes='See: EO 13660\nRevoked by:  EO 15000,\tMay 1, 2027 ')
        self.assertEqual('2022-03-15 (87 FR 14381) as published; Revoked by: EO 15000, May 1, 2027',
                         record(json.dumps(revoked).encode(), source, None)[1])
        unreadable = register_review.Unreadable
        for changes, message in (({'document_number': '2022-05555'}, "the record of document '2022-05555', not of"),
                                 ({'publication_date': '2022-02-30'}, "the publication day '2022-02-30' is not"),
                                 ({'publication_date': '2022-03-16'}, 'is not the document published on 2022-03-16'),
                                 ({'raw_text_url': 'https://evil.example/2022-05554.txt'}, 'is not the document'),
                                 ({'citation': '87 FR'}, "the citation '87 FR' is not"),
                                 ({'executive_order_notes': ['Amended by: x']}, 'notes or corrections have another'),
                                 ({'corrections': ['https://evil.example/C1']}, 'notes or corrections have another'),
                                 ({'corrections': None}, 'notes or corrections have another')):
            with self.assertRaises(unreadable) as refused:
                record(json.dumps(dict(self.FR_RECORD, **changes)).encode(), source, None)
            self.assertIn(message, str(refused.exception))
        for body in (b'[]', b'{"document_number": "2022-05554"}', b'not json'):
            with self.assertRaises(unreadable) as refused:
                record(body, source, None)
            self.assertIn('not a Federal Register document record', str(refused.exception))
        read = register_review.read_federal_register
        version = '2022-03-15 (87 FR 14381) as published'
        self.assertEqual(version, read(self.FR_TEXT.encode(), source, version))
        for body, version_read, message in (
                (self.FR_TEXT.replace('2022-05554', '2022-05555').encode(), version,
                 'not the raw text of Federal Register document 2022-05554'),
                (b'\xff' + self.FR_TEXT.encode(), version, 'not UTF-8'),
                (self.FR_TEXT.encode(), None, 'the record of 2022-05554 is unknown')):
            with self.assertRaises(unreadable) as refused:
                read(body, source, version_read)
            self.assertIn(message, str(refused.exception))

    def test_read_records_a_federal_register_document_by_its_record(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            legal = self.legal_register([
                self.legal_entry(),
                self.legal_entry(id='us.eo-14068', instrument=self.instrument(
                    name='Executive Order 14068', type='executive-order', jurisdiction='US',
                    primary='https://www.federalregister.gov/d/2022-05554'))])
            legal['jurisdictions']['US'] = {'name': 'United States', 'hosts': ['federalregister.gov']}
            self.write_tree(root, self.register(self.entries()), legal)
            (root / 'docs' / 'reading.md').write_text('<a id="r"></a>Read.\n', encoding='utf-8')
            argv = ('--register', 'cmake/legal-register.json')
            self.assertEqual(0, self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))[0])
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            requests = []
            routes = {'MevzuatNo=6698': self.MEVZUAT_HTML, 'api/v1/documents/2022-05554.json': json.dumps(self.FR_RECORD),
                      'full_text/text/2022/03/15/2022-05554.txt': self.FR_TEXT}
            manifest = register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes, requests),
                                                     today=datetime.date(2026, 12, 16))
            items = {item['id']: item for item in manifest['items']}
            version = '2022-03-15 (87 FR 14381) as published; Amended by: EO 14114, December 12, 2023'
            self.assertEqual(('https://www.federalregister.gov/documents/full_text/text/2022/03/15/2022-05554.txt',
                              version), (items['federalregister-2022-05554']['url'],
                                         items['federalregister-2022-05554']['version']))
            self.assertEqual('federalregister-2022-05554.txt', items['federalregister-2022-05554']['file'])
            fetched = ('--fetched', str(root / 'fetched'))
            code, output = self.tool(root, *argv, 'compare', *fetched, '--report', str(root / 'docs' / 'review.md'),
                                     '--apply')
            self.assertEqual(0, code, output)
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#r', '--entry',
                                     'us.eo-14068')
            self.assertEqual(0, code, output)
            path = root / 'cmake' / 'legal-register.json'
            entries = {entry['id']: entry for entry in json.loads(path.read_text(encoding='utf-8'))['entries']}
            self.assertEqual({'version': version, 'read': '2026-12-16',
                              'sha256': hashlib.sha256(self.FR_TEXT.encode()).hexdigest()},
                             entries['us.eo-14068']['instrument']['consolidated'])
            checked = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 12, 16))}
            self.assertEqual([], checked['legal'])
            # A later order amending it is a new version: the next scan reads the amendment.
            routes['api/v1/documents/2022-05554.json'] = json.dumps(dict(
                self.FR_RECORD, executive_order_notes=self.FR_RECORD['executive_order_notes'] +
                '\nAmended by: EO 15001, June 1, 2027'))
            register_review.fetch_sources(plan, root / 'again', opener=self.routed(routes),
                                          today=datetime.date(2026, 12, 17))
            code, output = self.tool(root, *argv, 'compare', '--fetched', str(root / 'again'), '--report',
                                     str(root / 'docs' / 'again.md'), today='2026-12-17')
            self.assertEqual(0, code, output)
            self.assertIn(f'Executive Order 14068, Articles 4-7: amended to {version}; Amended by: EO 15001, June 1, '
                          '2027 since it was read on 2026-12-16', (root / 'docs' / 'again.md').read_text(encoding='utf-8'))

    # The shape of the EU Sanctions Map's regime index (`/api/v1/regime`, 2026-11-01): a regime's country is empty for
    # a thematic regime; its acts mix decisions, resolutions, basic Council regulations (linked by CELEX number or ELI,
    # with a `qid` that changes), their implementing regulations and amendments not yet consolidated.
    @staticmethod
    def regime(regime_id, country, measures, *acts, notes='', amendment=1792969200):
        return {'id': regime_id, 'type': 0, 'specification': f'Regime {regime_id}', 'notes': notes,
                'amendment': amendment, 'country': {'data': {'code': country, 'title': country} if country else []},
                'legal_acts': {'data': [{'id': index, 'title': 'An act', 'number': number, 'url': url,
                                         'type': {'data': {'id': 1, 'title': title}}}
                                        for index, (title, number, url) in enumerate(acts)]},
                'measures': {'data': [{'id': index, 'description': f'{title}.', 'suspend': False,
                                       'type': {'data': {'id': index, 'title': title,
                                                         'icon': f'https://icons/{index}'}}}
                                      for index, title in enumerate(measures)]}}

    def regime_index(self, *changed):
        """Russia (a regulation by CELEX, its implementing act and an amendment not yet consolidated), Iraq (two
        regulations by ELI and by an old CELEX link), Burundi (claims and freezes only), a thematic regime with an arms
        export and China (no regulation), with `changed` regimes replacing those of their id."""
        freeze, claims = 'Asset freeze and prohibition to make funds available', 'Prohibition to satisfy claims'
        index = {regime['id']: regime for regime in (
            self.regime(26, 'RU', ['Arms export', freeze, claims],
                        ('EU Council Decision', '2014/512/CFSP', 'https://eur-lex.europa.eu/eli/dec/2014/512/oj'),
                        ('EU Council Regulation', '24.07.2026 consolidated basic legal act - (EU) No 833/2014',
                         'https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX%3A02014R0833-20260724&qid=1'),
                        ('EU Council Implementing Regulation', '2026/1817',
                         'https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=OJ:L_202601817'),
                        ('EC Council Regulation', '11.08.2026 amendments not yet included in consolidated legal act '
                         '- (EU) 2026/506', 'https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX%3A32026R0506R'
                         '%2803%29')),
            self.regime(19, 'IQ', ['Arms embargo', 'Cultural property', freeze],
                        ('EC Council Regulation', '24.12.2025 consolidated basic legal act - (EC) No 1210/2003',
                         'http://data.europa.eu/eli/reg/2003/1210/2025-12-24'),
                        ('EC Council Regulation', '07.12.1992 consolidated basic legal act - (EEC) No 3541/92',
                         'http://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:31992R3541'),
                        ('UN Security Council Resolution', 'S/RES/661 (1990)', 'https://undocs.org/S/RES/661(1990)')),
            self.regime(7, 'BI', [freeze, claims, 'Restrictions on admission'],
                        ('EU Council Regulation', '24.09.2025 consolidated basic legal act - (EU) 2015/1755',
                         'https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX%3A02015R1755-20250924')),
            self.regime(5, None, ['Arms export', freeze],
                        ('EU Council Regulation', '24.11.2025 consolidated basic legal act - (EU) 2016/1686',
                         'http://data.europa.eu/eli/reg/2016/1686/2025-11-24')),
            self.regime(10, 'CN', ['Arms embargo']))}
        index.update({regime['id']: regime for regime in changed})
        return json.dumps({'data': list(index.values()), 'meta': {'version': '1'}})

    def test_the_sanctions_map_is_read_by_its_regime_index_and_judges_the_place_regimes(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)

            def regulation(entry_id, name, year, number, **changes):
                return self.legal_entry(id=entry_id, **changes, instrument=self.instrument(
                    name=name, type='regulation', jurisdiction='EU',
                    primary=f'https://eur-lex.europa.eu/eli/reg/{year}/{number}/oj'))
            umbrella = self.legal_entry(id='eu.restrictive-measures', instrument=self.instrument(
                name='Council regulations adopted under Article 215 TFEU', type='regulation', jurisdiction='EU',
                primary='https://www.sanctionsmap.eu/api/v1/regime'))
            legal = self.legal_register([
                self.legal_entry(), umbrella,
                regulation('eu.russia', 'Council Regulation (EU) No 833/2014', 2014, 833),
                regulation('eu.iraq', 'Council Regulation (EC) No 1210/2003', 2003, 1210),
                regulation('eu.libya', 'Council Regulation (EU) 2016/44', 2016, 44),
                regulation('eu.ended', 'Council Regulation (EU) No 204/2011', 2011, 204, effective='2011-03-03',
                           sunset='2016-01-19', state='deprecated', deprecation='2016-01-19'),
                regulation('eu.dual-use', 'Regulation (EU) 2021/821', 2021, 821)])
            legal['jurisdictions']['EU']['hosts'].append('sanctionsmap.eu')
            self.write_tree(root, self.register(self.entries()), legal)
            (root / 'docs' / 'reading.md').write_text('<a id="r"></a>Read.\n', encoding='utf-8')
            argv = ('--register', 'cmake/legal-register.json')
            self.assertEqual(0, self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))[0])
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            source = next(source for source in plan['sources'] if source['id'] == 'sanctionsmap-regimes')
            self.assertEqual((['eu.restrictive-measures'], 'https://www.sanctionsmap.eu/api/v1/regime'),
                             (source['entries'], source['url']))
            requests = []
            routes = {'MevzuatNo=6698': self.MEVZUAT_HTML, 'sanctionsmap.eu/api/v1/regime': self.regime_index(),
                      'webapi/rdf/sparql': urllib.error.URLError('not this case')}
            register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes, requests),
                                          today=datetime.date(2026, 12, 16))
            self.assertEqual('application/json', next(request for request in requests
                                                      if 'sanctionsmap' in request.full_url).get_header('Accept'))
            fetched = ('--fetched', str(root / 'fetched'))
            report = root / 'docs' / 'review.md'
            code, output = self.tool(root, *argv, 'compare', *fetched, '--report', str(report), '--apply')
            self.assertEqual(0, code, output)
            record = register_review.sanctions_map_record(self.regime_index().encode(), source)
            digest = hashlib.sha256(record).hexdigest()
            version = '5 regimes, 3 attached to a place'
            text = report.read_text(encoding='utf-8')
            what = '- eu.restrictive-measures (NET.2)'
            self.assertIn(f'{what}: Council regulations adopted under Article 215 TFEU, Articles 4-7: never read from '
                          'its primary source; read https://www.sanctionsmap.eu/api/v1/regime (version '
                          f'{version}, SHA-256 {digest})', text)
            # Iraq's second regulation is no entry; Burundi's freezes and claims, the thematic regime's arms export to
            # listed persons, China's regime without a regulation and the implementing and unconsolidated acts owe
            # nothing; Libya's regulation is listed by no regime, the ended one and the dual-use regulation are not
            # judged.
            self.assertIn(f'{what}: regime 19 (IQ) restricts a supply to a place, and its regulation 31992R3541 is no '
                          'entry of the register; add it, read it with `read` and author what it restricts in the '
                          'embargo generation', text)
            self.assertIn(f"{what}: no regime lists eu.libya's regulation 32016R0044 any more; read its repeal and "
                          "record the entry's sunset", text)
            for absent in ('32015R1755', '32016R1686', 'regime 10', '32026R0506', '2026/1817', 'eu.ended',
                           'eu.dual-use', '32003R1210', '32014R0833'):
                self.assertNotIn(absent, text.partition('## Proposed ROADMAP amendments')[0], absent)
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#r', '--entry',
                                     'eu.restrictive-measures')
            self.assertEqual(0, code, output)
            path = root / 'cmake' / 'legal-register.json'
            entries = {entry['id']: entry for entry in json.loads(path.read_text(encoding='utf-8'))['entries']}
            self.assertEqual({'version': version, 'read': '2026-12-16', 'sha256': digest},
                             entries['eu.restrictive-measures']['instrument']['consolidated'])
            checked = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 12, 16))}
            self.assertEqual([], checked['legal'])

            def judged(index, day):
                """The report lines of a scan of `index` on `day`, the register as read."""
                routes['sanctionsmap.eu/api/v1/regime'] = index
                register_review.fetch_sources(plan, root / day, opener=self.routed(routes),
                                              today=datetime.date.fromisoformat(day))
                code, output = self.tool(root, *argv, 'compare', '--fetched', str(root / day), '--report',
                                         str(root / 'docs' / f'{day}.md'), today=day)
                self.assertEqual(0, code, output)
                lines = (root / 'docs' / f'{day}.md').read_text(encoding='utf-8').splitlines()
                return [line for line in lines if line.startswith(f'{what}: ')]
            regulations = [line for line in judged(self.regime_index(), '2026-12-17')]
            self.assertEqual(2, len(regulations), regulations)
            # Notes, amendment times, icons, descriptions, a link's query and the order of regimes and measures are
            # not the text read; a regime's measures, country and regulations are.
            noisy = json.loads(self.regime_index())
            noisy['data'].reverse()
            for regime in noisy['data']:
                regime.update(notes='Amended.', amendment=1800000000)
                regime['measures']['data'].reverse()
                for measure in regime['measures']['data']:
                    measure['description'] += ' Reworded.'
                    measure['type']['data']['icon'] = 'https://icons/new'
                for act in regime['legal_acts']['data']:
                    act['url'] = act['url'].replace('qid=1', 'qid=2')
            self.assertEqual(record, register_review.sanctions_map_record(json.dumps(noisy).encode(), source))
            self.assertEqual(regulations, judged(json.dumps(noisy), '2026-12-18'))
            # Burundi gains an arms embargo: a place regime whose regulation the register lacks, under a new version.
            armed = self.regime(7, 'BI', ['Arms export', 'Prohibition to satisfy claims'],
                                ('EU Council Regulation', '24.09.2025 consolidated basic legal act - (EU) 2015/1755',
                                 'https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX%3A02015R1755-20250924'))
            lines = judged(self.regime_index(armed), '2026-12-19')
            self.assertIn(f'{what}: Council regulations adopted under Article 215 TFEU, Articles 4-7: amended to 5 '
                          'regimes, 4 attached to a place since it was read on 2026-12-16 (5 regimes, 3 attached to a '
                          'place); read the amendment', '\n'.join(lines))
            self.assertIn(f'{what}: regime 7 (BI) restricts a supply to a place, and its regulation 32015R1755 is no '
                          'entry', '\n'.join(lines))
            # A regulation replaced under the same counts is a new digest of the text read.
            replaced = self.regime(10, 'CN', ['Arms embargo'], ('EU Council Regulation', '(EU) 2027/1',
                                                                'http://data.europa.eu/eli/reg/2027/1/2027-01-01'))
            lines = judged(self.regime_index(replaced), '2026-12-20')
            self.assertIn(f'{what}: Council regulations adopted under Article 215 TFEU, Articles 4-7: the primary text '
                          'changed since it was read on 2026-12-16 (5 regimes, 3 attached to a place)',
                          '\n'.join(lines))
            self.assertIn('regime 10 (CN) restricts a supply to a place, and its regulation 32027R0001 is no entry',
                          '\n'.join(lines))
            # An index the reader cannot read judges nothing and is reported once, as a source not read.
            routes['sanctionsmap.eu/api/v1/regime'] = self.regime_index().replace('"version": "1"', '"version": "2"')
            register_review.fetch_sources(plan, root / 'unread', opener=self.routed(routes),
                                          today=datetime.date(2026, 12, 21))
            code, output = self.tool(root, *argv, 'compare', '--fetched', str(root / 'unread'), '--report',
                                     str(root / 'docs' / 'unread.md'), today='2026-12-21')
            self.assertEqual(0, code, output)
            text = (root / 'docs' / 'unread.md').read_text(encoding='utf-8')
            self.assertIn("- sanctionsmap-regimes: regime index version '2', not '1'\n", text)
            self.assertNotIn(what, text)

    def test_the_regime_index_reader_refuses_other_shapes(self):
        unreadable, read = register_review.Unreadable, register_review.read_sanctions_map
        self.assertEqual('5 regimes, 3 attached to a place', read(self.regime_index().encode(), {}, None))
        regimes = register_review.sanctions_map_regimes(self.regime_index().encode())
        self.assertEqual([(5, None, ['32016R1686']), (7, 'BI', ['32015R1755']), (10, 'CN', []),
                          (19, 'IQ', ['31992R3541', '32003R1210']), (26, 'RU', ['32014R0833'])],
                         [(regime['id'], regime['country'], regime['regulations']) for regime in regimes])
        index = json.loads(self.regime_index())

        def changed(change):
            document = copy.deepcopy(index)
            change(document)
            return json.dumps(document).encode()
        unlinked = self.regime(8, 'MM', ['Arms export'], ('EU Council Regulation', '(EU) No 401/2013',
                                                          'https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=OJ:L_1'))
        for body, message in (
                (changed(lambda document: document['meta'].update(version='2')), "regime index version '2'"),
                (changed(lambda document: document.update(data=[])), 'without regimes'),
                (changed(lambda document: document['data'].append(document['data'][0])), 'with a regime twice'),
                (changed(lambda document: document['data'][0].pop('measures')), 'not the EU Sanctions Map regime'),
                (changed(lambda document: document['data'][0].update(country={'data': {'code': 'RUS'}})),
                 'regime 26 has another shape'),
                (changed(lambda document: document['data'][0].update(id='26')), "regime '26' has another shape"),
                (changed(lambda document: document['data'][0]['legal_acts']['data'][0].update(url=None)),
                 'regime 26 has another shape'),
                (changed(lambda document: document['data'][0]['measures']['data'][0]['type']['data'].update(
                    title='')), 'regime 26 has another shape'),
                (changed(lambda document: document['data'].append(unlinked)),
                 "regime 8: the regulation '(EU) No 401/2013' links no CELEX number or ELI"),
                (b'[]', 'not the EU Sanctions Map regime index'), (b'\xff', 'not the EU Sanctions Map regime index')):
            with self.assertRaises(unreadable) as refused:
                read(body, {}, None)
            self.assertIn(message, str(refused.exception))

    # The shape of gov.uk's content API for the FCDO's regime collection and a regime's documents (2026-11-01): the
    # groups list content ids, `links.documents` the record of each published one; a regime's guidance names its
    # regulation by its first legislation.gov.uk link (as `/contents`, made or revised, or a provision), an amending
    # instrument after it.
    UK_REGIMES_PATH = '/government/collections/uk-sanctions-regimes-under-the-sanctions-act'

    def uk_regimes(self, country=None, theme=None, unpublished=('gone-1',)):
        """The collection: Russia's and Guinea's guidance and Russia's designations by country, cyber's guidance by
        theme, an unpublished id in the country group and another under More information."""
        country = country or ['/government/publications/russia-sanctions-guidance',
                              '/guidance/russia-list-of-designations-and-sanctions-notices',
                              '/government/publications/guinea-sanctions-guidance']
        theme = theme or ['/government/publications/cyber-sanctions-guidance']
        records = [{'content_id': f'id{index}', 'base_path': path, 'document_type': 'statutory_guidance',
                    'title': path, 'withdrawn': False} for index, path in enumerate(country + theme)]
        ids = [record['content_id'] for record in records]
        return json.dumps({
            'base_path': self.UK_REGIMES_PATH, 'document_type': 'document_collection',
            'public_updated_at': '2026-03-25T12:03:32+00:00', 'title': 'Current UK sanctions regimes',
            'details': {'body': '<p>A regime</p>', 'collection_groups': [
                {'title': 'Regimes listed by country', 'documents': ids[:len(country)] + list(unpublished)},
                {'title': 'Regimes listed by theme', 'documents': ids[len(country):]},
                {'title': 'More information', 'documents': ['gone-2']}]},
            'links': {'documents': records}})

    @staticmethod
    def uk_document(path, *acts):
        links = ''.join(f'<a href="https://www.legislation.gov.uk/{act}">The act</a> ' for act in acts)
        return json.dumps({'base_path': path, 'document_type': 'statutory_guidance', 'public_updated_at': '2026-03-25',
                           'details': {'body': f'<p>{links}are in force.</p>'}})

    def uk_routes(self, **acts):
        """The documents of the collection by path: Russia's guidance names its regulations and then an amending
        instrument, its designations page the same regulations, Guinea's guidance its regulations as made and cyber's
        guidance its own; `acts` replaces a slug's links (None drops the document's route)."""
        links = {'russia-sanctions-guidance': ('uksi/2019/855/contents', 'uksi/2022/195/contents/made'),
                 'russia-list-of-designations-and-sanctions-notices': ('uksi/2019/855/regulation/51',),
                 'guinea-sanctions-guidance': ('uksi/2019/1145/contents/made',),
                 'cyber-sanctions-guidance': ('uksi/2020/597/contents',)}
        links.update(acts)
        routes = {}
        for slug, found in links.items():
            path = ('/guidance/' if 'list-of' in slug else '/government/publications/') + slug
            routes[f'/api/content{path}'] = (urllib.error.URLError('gone') if found is None else
                                             self.uk_document(path, *found))
        return routes

    def test_the_uk_regime_collection_is_read_with_its_documents_and_judges_the_country_regimes(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)

            def regulation(entry_id, path, **changes):
                return self.legal_entry(id=entry_id, **{'rows': ['COMPLY.1'], **changes},
                                        instrument=self.instrument(name=f'The {path} regulations', type='regulation',
                                                                   jurisdiction='GB',
                                                                   primary=f'https://www.legislation.gov.uk/{path}'))
            umbrella = self.legal_entry(id='gb.sanctions', rows=['COMPLY.1'], instrument=self.instrument(
                name='Sanctions and Anti-Money Laundering Act 2018', article='Regulations made under section 1',
                type='law', jurisdiction='GB', primary=f'https://www.gov.uk{self.UK_REGIMES_PATH}'))
            legal = self.legal_register([
                self.legal_entry(), umbrella,
                regulation('gb.russia', 'uksi/2019/855'),
                regulation('gb.burundi', 'uksi/2019/1142'),
                regulation('gb.ended', 'uksi/2019/604', effective='2019-12-31', sunset='2026-01-01',
                           state='deprecated', deprecation='2026-01-01'),
                regulation('gb.data', 'ukpga/2018/12', rows=['NET.2'])])
            legal['jurisdictions']['GB'] = {'name': 'United Kingdom', 'hosts': ['legislation.gov.uk', 'gov.uk']}
            self.write_tree(root, self.register(self.entries()), legal)
            (root / 'docs' / 'reading.md').write_text('<a id="r"></a>Read.\n', encoding='utf-8')
            argv = ('--register', 'cmake/legal-register.json')
            code, output = self.tool(root, *argv, 'plan', '--out', str(root / 'sources.json'))
            self.assertEqual(0, code, output)
            self.assertIn("  and each document of the regimes the UK's collection lists "
                          '(https://www.gov.uk/api/content<page>)\n', output)
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            source = next(source for source in plan['sources'] if source['id'] == 'gov.uk-uk-sanctions-regimes')
            self.assertEqual((['gb.sanctions'], f'https://www.gov.uk/api/content{self.UK_REGIMES_PATH}',
                              'gov-uk-regimes', {'Accept': 'application/json'}),
                             (source['entries'], source['url'], source['reader'], source['headers']))
            requests = []
            routes = {**self.uk_routes(), '/api/content' + self.UK_REGIMES_PATH: self.uk_regimes(),
                      'MevzuatNo=6698': self.MEVZUAT_HTML,
                      **{f'legislation.gov.uk/{act}/': self.LEGISLATION_XML.replace('ukpga/2018/12', act)
                         for act in ('uksi/2019/855', 'uksi/2019/1142', 'uksi/2019/604', 'ukpga/2018/12')}}
            manifest = register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes, requests),
                                                     today=datetime.date(2026, 12, 16))
            items = {item['id']: item for item in manifest['items']}
            documents = ['gov.uk-government-publications-russia-sanctions-guidance',
                         'gov.uk-guidance-russia-list-of-designations-and-sanctions-notices',
                         'gov.uk-government-publications-guinea-sanctions-guidance',
                         'gov.uk-government-publications-cyber-sanctions-guidance']
            version = '4 documents: 3 by country, 1 by theme, 1 listed unpublished'
            self.assertEqual(version, items['gov.uk-uk-sanctions-regimes']['version'])
            self.assertEqual(sorted(documents), sorted(items['gov.uk-uk-sanctions-regimes']['documents']))
            self.assertTrue(all('error' not in items[document] for document in documents))
            self.assertTrue(all(request.get_header('Accept') == 'application/json' for request in requests
                                if 'gov.uk/api' in request.full_url))
            fetched = ('--fetched', str(root / 'fetched'))
            report = root / 'docs' / 'review.md'
            code, output = self.tool(root, *argv, 'compare', *fetched, '--report', str(report), '--apply')
            self.assertEqual(0, code, output)
            record = register_review.uk_regimes_record(self.uk_regimes().encode(), source)
            digest = hashlib.sha256(record).hexdigest()
            text = report.read_text(encoding='utf-8')
            self.assertIn('Sources: 6 planned and 4 documents their indexes name, 10 read on 2026-12-16;', text)
            what = '- gb.sanctions (COMPLY.1)'
            self.assertIn(f'{what}: Sanctions and Anti-Money Laundering Act 2018, Regulations made under section 1: '
                          f'never read from its primary source; read https://www.gov.uk/api/content'
                          f'{self.UK_REGIMES_PATH} (version {version}, SHA-256 {digest}) in its consolidated text and '
                          'record it with `read`\n', text)
            # Guinea's regulations are no entry; Russia's are, its amending instrument and the cyber regime owe
            # nothing; Burundi's regulations are named by no regime, the ended and the NET.2 entry are not judged.
            self.assertIn(f'{what}: the regime of /government/publications/guinea-sanctions-guidance rests on '
                          'uksi/2019/1145, which is no entry of the register; add it, read it with `read` and author '
                          'what it restricts in the embargo generation', text)
            self.assertIn(f"{what}: no regime names gb.burundi's regulation uksi/2019/1142 any more; read its "
                          "revocation and record the entry's sunset", text)
            judged = '\n'.join(line for line in text.splitlines() if line.startswith(what))
            for absent in ('uksi/2022/195', 'uksi/2020/597', 'uksi/2019/855', 'gb.ended', 'gb.data',
                           'consolidated text lacks'):
                self.assertNotIn(absent, judged, absent)
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#r', '--entry',
                                     'gb.sanctions')
            self.assertEqual(0, code, output)
            path = root / 'cmake' / 'legal-register.json'
            entries = {entry['id']: entry for entry in json.loads(path.read_text(encoding='utf-8'))['entries']}
            self.assertEqual({'version': version, 'read': '2026-12-16', 'sha256': digest},
                             entries['gb.sanctions']['instrument']['consolidated'])

            def judged(day, collection=None, **acts):
                """The report of a scan on `day` of the register as read, its unread and judged lines."""
                routes.update(self.uk_routes(**acts))
                routes['/api/content' + self.UK_REGIMES_PATH] = collection or self.uk_regimes()
                register_review.fetch_sources(plan, root / day, opener=self.routed(routes),
                                              today=datetime.date.fromisoformat(day))
                code, output = self.tool(root, *argv, 'compare', '--fetched', str(root / day), '--report',
                                         str(root / 'docs' / f'{day}.md'), today=day)
                self.assertEqual(0, code, output)
                return (root / 'docs' / f'{day}.md').read_text(encoding='utf-8')
            self.assertEqual(2, judged('2026-12-17').count(f'{what}: '))
            # Russia's guidance gone: its designations still name the regulations; Guinea's: nothing is judged ended.
            text = judged('2026-12-18', **{'guinea-sanctions-guidance': None})
            self.assertIn(f'{what}: 1 regime documents were not read, so no regulation is judged ended', text)
            self.assertNotIn("gb.burundi's", text)
            self.assertNotIn('uksi/2019/1145', text.partition('## Sources not read')[0])
            # Guinea's regulations as an entry, then cyber moved to the country regimes: its regulations are owed, a
            # new version of the text read; Russia's guidance linking another regulation first owes that one.
            legal = json.loads(path.read_text(encoding='utf-8'))
            legal['entries'].append(regulation('gb.guinea', 'uksi/2019/1145'))
            path.write_text(json.dumps(legal), encoding='utf-8')
            self.assertEqual(1, judged('2026-12-19', **{'guinea-sanctions-guidance': ('uksi/2019/1145',)}).count(
                f'{what}: '))
            moved = self.uk_regimes(country=['/government/publications/russia-sanctions-guidance',
                                             '/guidance/russia-list-of-designations-and-sanctions-notices',
                                             '/government/publications/guinea-sanctions-guidance',
                                             '/government/publications/cyber-sanctions-guidance'], theme=['/x'])
            text = judged('2026-12-20', moved, **{'russia-list-of-designations-and-sanctions-notices': (
                'uksi/2026/1/contents', 'uksi/2019/855')})
            self.assertIn(f'{what}: Sanctions and Anti-Money Laundering Act 2018, Regulations made under section 1: '
                          'amended to 5 documents: 4 by country, 1 by theme, 1 listed unpublished since it was read on '
                          f'2026-12-16 ({version})', text)
            self.assertIn('the regime of /government/publications/cyber-sanctions-guidance rests on uksi/2020/597, '
                          'which is no entry', text)
            self.assertIn('the regime of /guidance/russia-list-of-designations-and-sanctions-notices rests on '
                          'uksi/2026/1, which is no entry', text)
            self.assertIn('- gov.uk-x (https://www.gov.uk/api/content/x): URLError', text)
            # Guinea's guidance replaced by Mali's under the same counts is a new digest of the text read, and Mali's
            # unread document holds every ending back.
            replaced = self.uk_regimes(country=['/government/publications/russia-sanctions-guidance',
                                                '/guidance/russia-list-of-designations-and-sanctions-notices',
                                                '/government/publications/mali-sanctions-guidance'])
            text = judged('2026-12-21', replaced)
            self.assertIn(f'{what}: Sanctions and Anti-Money Laundering Act 2018, Regulations made under section 1: '
                          f'the primary text changed since it was read on 2026-12-16 ({version})', text)
            self.assertIn(f'{what}: 1 regime documents were not read, so no regulation is judged ended', text)
            self.assertNotIn("gb.guinea's", text)

    def test_the_uk_regime_readers_refuse_other_shapes(self):
        unreadable, source = register_review.Unreadable, {'act': self.UK_REGIMES_PATH}
        body = self.uk_regimes().encode()
        self.assertEqual('4 documents: 3 by country, 1 by theme, 1 listed unpublished',
                         register_review.read_uk_regimes(body, source, None))
        day, version, documents = register_review.read_uk_regimes_index(body, source, datetime.date(2026, 11, 1))
        self.assertEqual(('2026-03-25', 4), (day, len(documents)))
        self.assertEqual({'id': 'gov.uk-government-publications-cyber-sanctions-guidance',
                          'act': '/government/publications/cyber-sanctions-guidance', 'reader': 'gov-uk-document',
                          'headers': {'Accept': 'application/json'},
                          'url': 'https://www.gov.uk/api/content/government/publications/cyber-sanctions-guidance'},
                         documents[-1])
        with self.assertRaises(unreadable) as refused:
            register_review.read_uk_regimes(body, source, '5 documents: 4 by country, 1 by theme')
        self.assertIn('is not of the collection read, 4 documents', str(refused.exception))
        collection = json.loads(body)

        def changed(change):
            document = copy.deepcopy(collection)
            change(document)
            return json.dumps(document).encode()
        groups = collection['details']['collection_groups']
        for refused_body, message in (
                (changed(lambda document: document.update(base_path='/government/collections/other')),
                 f'not the gov.uk document_collection {self.UK_REGIMES_PATH}'),
                (changed(lambda document: document.update(document_type='guide')), 'not the gov.uk document_coll'),
                (changed(lambda document: document['details']['collection_groups'].append(
                    {'title': 'Regimes listed by region', 'documents': []})), 'the collection groups'),
                (changed(lambda document: document['details']['collection_groups'].pop()), 'the collection groups'),
                (changed(lambda document: document['links']['documents'][0].update(base_path='russia')),
                 'a collection document with another shape'),
                (changed(lambda document: document['details']['collection_groups'][0].update(documents='id0')),
                 'a collection document with another shape'),
                (changed(lambda document: document.update(public_updated_at='2026-02-30T00:00:00Z')),
                 'no public update day'),
                (changed(lambda document: document['details']['collection_groups'][0].update(documents=['gone-1'])),
                 'without regimes by country'),
                (changed(lambda document: document['details']['collection_groups'][1].update(
                    documents=groups[0]['documents'][:1])), 'with a document in both regime groups'),
                (changed(lambda document: document['links'].pop('documents')), 'not the UK sanctions regimes'),
                (changed(lambda document: document.pop('details')), 'not a gov.uk content record'),
                (b'[]', 'not a gov.uk content record'), (b'\xff', 'not a gov.uk content record')):
            with self.assertRaises(unreadable) as refused:
                register_review.read_uk_regimes_index(refused_body, source, datetime.date(2026, 11, 1))
            self.assertIn(message, str(refused.exception))
        guidance = {'act': '/government/publications/syria-sanctions-guidance'}
        regulation = register_review.uk_regime_regulation
        for links, act in ((('uksi/2019/792/contents', 'uksi/2025/507/contents/made'), 'uksi/2019/792'),
                           (('uksi/2008/3231/schedule/4/part/4/made',), 'uksi/2008/3231'),
                           (('uksi/2023/1314/regulation/51',), 'uksi/2023/1314'), (('uksi/2020/948',), 'uksi/2020/948'),
                           ((), None), (('ukpga/2018/13/section/1',), 'ukpga/2018/13'),
                           (('id/uksi/2019/792',), None), (('uksi/2019/0792',), None)):
            found = regulation(self.uk_document(guidance['act'], *links).encode(), guidance)
            self.assertEqual(act, found, links)
        self.assertEqual('uksi/2020/680', regulation(self.uk_document(guidance['act']).replace(
            '<p>', '<p><a href=\\"http://www.legislation.gov.uk/uksi/2020/680/contents\\">').encode(), guidance))
        self.assertIsNone(register_review.read_gov_uk_document(self.uk_document(guidance['act']).encode(), guidance,
                                                               None))
        for refused_body, message in (
                (self.uk_document('/government/publications/iran-sanctions-guidance', 'uksi/2023/1314'),
                 'not the gov.uk page /government/publications/syria'),
                (json.dumps({'base_path': guidance['act'], 'document_type': 'x', 'details': []}),
                 'not the gov.uk page'),
                (json.dumps({'base_path': guidance['act'], 'document_type': 'x', 'details': {'body': 7}}),
                 'has a body that is no text'),
                ('{}', 'not a gov.uk content record')):
            with self.assertRaises(unreadable) as refused:
                regulation(refused_body.encode(), guidance)
            self.assertIn(message, str(refused.exception))

    # The UN Digital Library's MARC XML record of a resolution and the Security Council Consolidated List (2026-11-01):
    # a record names its symbol (with or without a space before the year), its day and one 856 field per language file,
    # whose subfields come in any order; the list's parties carry their regime's list type.
    UN_RECORD = '584729'
    UN_PDF = b'%PDF-1.4\n' + b'0' * 91

    def un_record(self, record=UN_RECORD, symbol='S/RES/1718(2006)', day='2006-10-14', files=None):
        files = [('English', 'S_RES_1718%282006%29-EN.pdf', len(self.UN_PDF)),
                 ('Français', 'S_RES_1718%282006%29-FR.pdf', 48639)] if files is None else files
        fields = ''.join(
            ('<datafield tag="856" ind1="4" ind2=" "><subfield code="u">https://digitallibrary.un.org/record/'
             f'{record}/files/{name}</subfield><subfield code="y">{language}</subfield><subfield code="s">{size}'
             '</subfield></datafield>') if index % 2 == 0 else
            (f'<datafield tag="856" ind1="4" ind2=" "><subfield code="y">{language}</subfield><subfield code="s">{size}'
             f'</subfield><subfield code="u">https://digitallibrary.un.org/record/{record}/files/{name}</subfield>'
             '</datafield>') for index, (language, name, size) in enumerate(files))
        return ('<?xml version="1.0" encoding="UTF-8"?><collection xmlns="http://www.loc.gov/MARC21/slim"><record>'
                f'<controlfield tag="001">{record}</controlfield><datafield tag="191" ind1=" " ind2=" ">'
                f'<subfield code="a">{symbol}</subfield><subfield code="b">S/</subfield></datafield>'
                f'<datafield tag="269" ind1=" " ind2=" "><subfield code="a">{day}</subfield></datafield>{fields}'
                '</record></collection>')

    @staticmethod
    def un_list(*regimes, generated='2026-10-31T23:00:07.052Z'):
        """The Consolidated List with one individual per regime and one entity of the first."""
        people = ''.join(f'<INDIVIDUAL><DATAID>{index}</DATAID><UN_LIST_TYPE>{regime}</UN_LIST_TYPE></INDIVIDUAL>'
                         for index, regime in enumerate(regimes))
        return (f'<?xml version="1.0" encoding="UTF-8"?><CONSOLIDATED_LIST dateGenerated="{generated}">'
                f'<INDIVIDUALS>{people}</INDIVIDUALS><ENTITIES><ENTITY><UN_LIST_TYPE> {regimes[0]} </UN_LIST_TYPE>'
                '</ENTITY></ENTITIES></CONSOLIDATED_LIST>')

    def resolution(self, entry_id, number, year, record, **changes):
        return self.legal_entry(id=entry_id, **changes, instrument=self.instrument(
            name=f'Security Council resolution {number} ({year})', article='Paragraph 8', type='resolution',
            jurisdiction='INT', primary=f'https://digitallibrary.un.org/record/{record}'))

    def un_tree(self, root, *entries):
        legal = self.legal_register([self.legal_entry(), *entries])
        legal['jurisdictions']['INT'] = {'name': 'International bodies', 'hosts': ['un.org']}
        self.write_tree(root, self.register(self.entries()), legal)
        (root / 'docs' / 'reading.md').write_text('<a id="r"></a>Read.\n', encoding='utf-8')
        self.assertEqual(0, self.tool(root, '--register', 'cmake/legal-register.json', 'plan', '--out',
                                      str(root / 'sources.json'))[0])
        return json.loads((root / 'sources.json').read_text(encoding='utf-8'))

    def test_a_security_council_resolution_is_read_by_its_library_record_and_english_text(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            plan = self.un_tree(root, self.resolution('un.dprk', 1718, 2006, self.UN_RECORD))
            sources = {source['id']: source for source in plan['sources'] if source['id'].startswith('un-')}
            library = f'https://digitallibrary.un.org/record/{self.UN_RECORD}'
            self.assertEqual({'un-library-584729-record': ('index', f'{library}?of=xm', 'un-library-record', None),
                              'un-library-584729': ('primary', 'https://digitallibrary.un.org/record/{version}',
                                                    'un-resolution', 'un-library-584729-record')},
                             {key: (source['kind'], source['url'], source['reader'], source.get('index'))
                              for key, source in sources.items()})
            text = f'{library}/files/S_RES_1718(2006)-EN.pdf'
            routes = {f'{library}?of=xm': self.un_record(), text: self.UN_PDF, 'MevzuatNo=6698': self.MEVZUAT_HTML}
            requests = []
            manifest = register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes, requests),
                                                     today=datetime.date(2026, 12, 16))
            items = {item['id']: item for item in manifest['items']}
            version = 'S/RES/1718 (2006) of 2006-10-14, English text of 100 bytes'
            item = items['un-library-584729']
            self.assertEqual((text, version), (item['url'], item['version']))
            self.assertIn(text, [request.full_url for request in requests])
            argv, fetched = ('--register', 'cmake/legal-register.json'), ('--fetched', str(root / 'fetched'))
            report = root / 'docs' / 'review.md'
            self.assertEqual(0, self.tool(root, *argv, 'compare', *fetched, '--report', str(report), '--apply')[0])
            digest = hashlib.sha256(self.UN_PDF).hexdigest()
            self.assertIn(f'- un.dprk (NET.2): Security Council resolution 1718 (2006), Paragraph 8: never read from '
                          f'its primary source; read {text} (version {version}, SHA-256 {digest}) in its consolidated '
                          'text and record it with `read`\n', report.read_text(encoding='utf-8'))
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#r', '--entry', 'un.dprk')
            self.assertEqual(0, code, output)
            path = root / 'cmake' / 'legal-register.json'
            legal = json.loads(path.read_text(encoding='utf-8'))
            entries = {entry['id']: entry for entry in legal['entries']}
            self.assertEqual({'version': version, 'read': '2026-12-16', 'sha256': digest},
                             entries['un.dprk']['instrument']['consolidated'])
            checked = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 12, 16))}
            self.assertEqual([], checked['legal'])
            # The guard holds a resolution to its name and to the symbol it was read as.
            entries['un.dprk']['instrument']['name'] = 'Security Council resolution 1874 (2009)'
            path.write_text(json.dumps(legal), encoding='utf-8')
            checked = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 12, 16))}
            self.assertEqual([f"legal entry un.dprk: read as '{version}', which is not Security Council resolution "
                              '1874 (2009)'], checked['legal'])
            entries['un.dprk']['instrument']['name'] = 'Resolution 1718'
            path.write_text(json.dumps(legal), encoding='utf-8')
            checked = {domain: errors for domain, _, errors in registers.check(root, datetime.date(2026, 12, 16))}
            self.assertEqual(['legal entry un.dprk: a resolution is named "Security Council resolution <number> '
                              '(<year>)"'], checked['legal'])
            entries['un.dprk']['instrument']['name'] = 'Security Council resolution 1718 (2006)'
            path.write_text(json.dumps(legal), encoding='utf-8')

            def judged(day, record, pdf):
                """The report of a scan on `day` with the record and English text given."""
                routes.update({f'{library}?of=xm': record, text: pdf})
                register_review.fetch_sources(plan, root / day, opener=self.routed(routes),
                                              today=datetime.date.fromisoformat(day))
                code, output = self.tool(root, *argv, 'compare', '--fetched', str(root / day), '--report',
                                         str(root / 'docs' / f'{day}.md'), today=day)
                self.assertEqual(0, code, output)
                return (root / 'docs' / f'{day}.md').read_text(encoding='utf-8')
            self.assertNotIn('- un.dprk', judged('2026-12-17', self.un_record(), self.UN_PDF))
            # A text other than the record's size is not read; a reissued text is a new version to read.
            text_report = judged('2026-12-18', self.un_record(), self.UN_PDF[:-1])
            self.assertIn('- un-library-584729: not the 100-byte PDF its record names (99 bytes)', text_report)
            reissued = self.UN_PDF + b'1'
            files = [('English', 'S_RES_1718%282006%29-EN.pdf', len(reissued))]
            text_report = judged('2026-12-19', self.un_record(files=files), reissued)
            self.assertIn('- un.dprk (NET.2): Security Council resolution 1718 (2006), Paragraph 8: amended to '
                          'S/RES/1718 (2006) of 2006-10-14, English text of 101 bytes since it was read on 2026-12-16 '
                          f'({version})', text_report)

    def test_the_un_list_is_read_by_its_regimes_and_judges_the_regime_resolutions(self):
        list_url = register_review.UN_LIST
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            umbrella = self.legal_entry(id='int.un-sanctions', instrument=self.instrument(
                name='UN Security Council Consolidated List', type='treaty', jurisdiction='INT', primary=list_url))
            plan = self.un_tree(root, umbrella, self.resolution('un.dprk', 1718, 2006, 584729),
                                self.resolution('un.libya', 1970, 2011, 698927),
                                self.resolution('un.guinea-bissau', 2048, 2012, 727466),
                                self.resolution('un.ended', 1267, 1999, 287334, effective='1999-10-15',
                                                sunset='2026-01-01', state='deprecated', deprecation='2026-01-01'),
                                self.resolution('un.lebanon', 1701, 2006, 581053))
            source = next(source for source in plan['sources'] if source['id'] == 'un-sc-consolidated-list')
            self.assertEqual((['int.un-sanctions'], list_url, 'un-sc-list'),
                             (source['entries'], source['url'], source['reader']))
            listed = self.un_list('DPRK', 'Libya', 'Iran', 'Cyber')
            routes = {list_url: listed, 'MevzuatNo=6698': self.MEVZUAT_HTML,
                      'digitallibrary': urllib.error.URLError('not this case')}
            register_review.fetch_sources(plan, root / 'fetched', opener=self.routed(routes),
                                          today=datetime.date(2026, 12, 16))
            argv, fetched = ('--register', 'cmake/legal-register.json'), ('--fetched', str(root / 'fetched'))
            report = root / 'docs' / 'review.md'
            self.assertEqual(0, self.tool(root, *argv, 'compare', *fetched, '--report', str(report), '--apply')[0])
            record = register_review.un_list_record(listed.encode(), source)
            self.assertEqual(b'["Cyber","DPRK","Iran","Libya"]', record)
            text = report.read_text(encoding='utf-8')
            what = '- int.un-sanctions (NET.2)'
            self.assertIn(f'{what}: UN Security Council Consolidated List, Articles 4-7: never read from its primary '
                          f'source; read {list_url} (version 4 regimes, SHA-256 {hashlib.sha256(record).hexdigest()})',
                          text)
            # Iran's establishing resolution is no entry and Cyber is no regime the reader knows; Guinea-Bissau's
            # regime lists no one any more. The DPRK and Libya owe nothing; the ended 1267 entry and Lebanon's
            # resolution (no list) are not judged.
            self.assertIn(f'{what}: the Iran regime rests on S/RES/1737 (2006), which is no entry of the register; add '
                          'it, read it with `read` and author what it restricts in the embargo generation', text)
            self.assertIn(f"{what}: the list names the regime 'Cyber', which UN_REGIMES does not know; read the "
                          'resolution establishing it', text)
            self.assertIn(f"{what}: the list names no party of the GB regime any more; read whether a resolution "
                          "ended un.guinea-bissau's S/RES/2048 (2012) and record the entry's sunset", text)
            judged = '\n'.join(line for line in text.splitlines() if line.startswith(what))
            for absent in ('DPRK regime', 'Libya regime', 'un.ended', 'Al-Qaida', 'un.lebanon', '1701'):
                self.assertNotIn(absent, judged, absent)
            self.assertEqual(4, judged.count(f'{what}: '))
            code, output = self.tool(root, *argv, 'read', *fetched, '--ref', 'docs/reading.md#r', '--entry',
                                     'int.un-sanctions')
            self.assertEqual(0, code, output)
            # Designations, their order and the generation instant are not the text read; a regime is.
            self.assertEqual(record, register_review.un_list_record(self.un_list(
                'Libya', 'DPRK', 'Cyber', 'Iran', 'Iran', generated='2026-12-17T23:00:00Z').encode(), source))
            self.assertNotEqual(record, register_review.un_list_record(self.un_list('DPRK', 'Libya', 'Iran').encode(),
                                                                       source))

    def test_the_un_list_is_fetched_through_its_delivery_hosts(self):
        list_url = register_review.UN_LIST
        umbrella = self.legal_entry(id='int.un-sanctions', instrument=self.instrument(
            name='UN Security Council Consolidated List', type='treaty', jurisdiction='INT', primary=list_url))
        register = {'domain': 'legal', 'entries': [umbrella], 'sanctions_lists': {'lists': {'un.sc': {
            'url': list_url, 'delivery': ['files.example.net', 'b.example.net']}}}}
        sources, _ = register_review.plan_sources(register)
        self.assertEqual(['b.example.net', 'files.example.net'], sources[0]['delivery'])
        self.assertNotIn('delivery', register_review.plan_sources({'domain': 'legal', 'entries': [umbrella]})[0][0])
        plan = {'schema': register_review.PLAN_SCHEMA, 'sources': sources}
        handlers = []

        def build_opener(handler):
            handlers.append(handler)
            return SimpleNamespace(open=self.routed({list_url: self.un_list('DPRK')}))
        with tempfile.TemporaryDirectory() as scratch, \
                patch.object(register_review.urllib.request, 'build_opener', build_opener):
            manifest = register_review.fetch_sources(plan, scratch, today=datetime.date(2026, 12, 16))
        self.assertNotIn('error', manifest['items'][0])
        self.assertEqual([frozenset({'scsanctions.un.org', 'b.example.net', 'files.example.net'})],
                         [handler.hosts for handler in handlers])

    def test_the_un_readers_refuse_other_shapes(self):
        unreadable, source = register_review.Unreadable, {'act': self.UN_RECORD}
        index = register_review.read_un_library_record
        today = datetime.date(2026, 11, 1)
        self.assertEqual(('584729/files/S_RES_1718(2006)-EN.pdf',
                          'S/RES/1718 (2006) of 2006-10-14, English text of 100 bytes'),
                         index(self.un_record().encode(), source, today))
        spaced = self.un_record(symbol='S/RES/2821 (2026)', files=[('English', 'S_RES_2821_%282026%29-EN.pdf', 7)])
        self.assertEqual(('584729/files/S_RES_2821_(2026)-EN.pdf', 'S/RES/2821 (2026) of 2006-10-14, English text of '
                          '7 bytes'), index(spaced.encode(), source, today))
        self.assertEqual('S/RES/1718 (2006) of 2006-10-14, English text of 100 bytes',
                         register_review.read_un_resolution(self.UN_PDF, source, index(self.un_record().encode(),
                                                                                      source, today)[1]))
        two = self.un_record().replace('</record></collection>', '</record><record/></collection>')
        for body, message in (
                (self.un_record(record='584730'), 'not the one UN Digital Library record 584729'),
                (two, 'not the one UN Digital Library record 584729'),
                (self.un_record(symbol='A/RES/61/1'), 'record 584729 is no Security Council resolution'),
                (self.un_record(symbol='S/RES/1718(06)'), 'is no Security Council resolution'),
                (self.un_record(day='2006-02-30'), 'is no Security Council resolution'),
                (self.un_record(day='2006-10'), 'is no Security Council resolution'),
                (self.un_record().replace('<datafield tag="191"', '<datafield tag="191" ind1=" " ind2=" "><subfield '
                                          'code="a">S/RES/1(1946)</subfield></datafield><datafield tag="191"'),
                 'is no Security Council resolution'),
                (self.un_record(files=[('English', '../x-EN.pdf', 5)]), 'links a file'),
                (self.un_record(files=[('English', 'S_RES_1718%282006%29-EN.pdf', 0)]), 'of another shape'),
                (self.un_record(files=[('English', 'a%3Fb-EN.pdf', 5)]), 'cannot be part of a URL'),
                (self.un_record(files=[('English', 'a%2F..%2Fb-EN.pdf', 5)]), 'cannot be part of a URL'),
                (self.un_record(files=[('Français', 'x-FR.pdf', 5)]), 'has 0 English texts, not one'),
                (self.un_record(files=[('English', 'x-EN.pdf', 5), ('English', 'y-EN.pdf', 5)]), 'has 2 English'),
                (self.un_record().replace('584729/files', '584730/files', 1), 'links a file'),
                ('<!DOCTYPE x [<!ENTITY a "b">]>' + self.un_record()[38:], 'a document type declaration'),
                ('<collection', 'not well-formed XML')):
            with self.assertRaises(unreadable) as refused:
                index(body.encode(), source, today)
            self.assertIn(message, str(refused.exception), body)
        for body, version, message in (
                (self.UN_PDF, None, 'no record version'), (self.UN_PDF, 'S/RES/1718 (2006)', 'no record version'),
                (b'<html>' + self.UN_PDF[6:], 'S/RES/1718 (2006) of 2006-10-14, English text of 100 bytes',
                 'not the 100-byte PDF'),
                (self.UN_PDF + b'0', 'S/RES/1718 (2006) of 2006-10-14, English text of 100 bytes',
                 'not the 100-byte PDF its record names (101 bytes)')):
            with self.assertRaises(unreadable) as refused:
                register_review.read_un_resolution(body, source, version)
            self.assertIn(message, str(refused.exception))
        self.assertEqual('2 regimes', register_review.read_un_list(self.un_list('DPRK', 'Iraq').encode(), {}, None))
        entity = self.un_list('DPRK').replace('<ENTITY><UN_LIST_TYPE> DPRK ', '<ENTITY><UN_LIST_TYPE> Iraq ')
        self.assertEqual(['DPRK', 'Iraq'], register_review.un_list_regimes(entity.encode()))
        for body, message in (
                (self.un_list('DPRK').replace('CONSOLIDATED_LIST', 'LIST'), 'not the UN Consolidated List'),
                (self.un_list('DPRK', '  '), 'a party without its list type'),
                ('<CONSOLIDATED_LIST><INDIVIDUALS/></CONSOLIDATED_LIST>', 'not the UN Consolidated List'),
                ('<!DOCTYPE CONSOLIDATED_LIST><CONSOLIDATED_LIST/>', 'a document type declaration'),
                ('<CONSOLIDATED_LIST>', 'not well-formed XML')):
            with self.assertRaises(unreadable) as refused:
                register_review.read_un_list(body.encode(), {}, None)
            self.assertIn(message, str(refused.exception))

    def test_readers_find_each_hosts_version_and_refuse_other_shapes(self):
        unreadable = register_review.Unreadable
        mevzuat = {'act': '6698'}

        def law(*notes, number='6698', enacted='Kabul Tarihi : 24/3/2016'):
            return (f'<p>Kanun Numarası : {number}</p><p>{enacted}</p>' + ''.join(f'<p>{note}</p>' for note in notes)
                    ).encode()
        read = register_review.read_mevzuat
        self.assertEqual('2016-03-24 (as enacted)', read(law(), mevzuat, None))
        self.assertEqual('2024-03-02 (7499)', read(law('(Değişik:2/3/2024-7499/34 md.)',
                                                       '(Ek: 28/11/2017 - 7061/120 md.)'), mevzuat, None))
        for note, version in (('(Ek cümle: 1/2/2025 – 7530/3 md.)', '2025-02-01 (7530)'),
                              ('(Başlığı ile Birlikte Değişik:5/5/2025-7540/1 md.)', '2025-05-05 (7540)'),
                              ('(Mülga: 2/7/2018-KHK-703/163 md.; Aynen kabul: 5/7/2025-7541/2 md.)',
                               '2025-07-05 (7541)'),
                              ('(Değişik: 1/1/2025-KHK/690-12 md.)', '2025-01-01 (KHK-690)'),
                              ('(Yeniden düzenleme: 9/9/2025-7550/4 md.)', '2025-09-09 (7550)'),
                              ('(9/10/2025-7551/1 md.)', '2025-10-09 (7551)'),
                              ('(İptal birinci cümle: Anayasa Mahkemesinin 11/12/2025 tarihli ve E.: 2024/1, K.: '
                               '2025/2 sayılı Kararı ile.)', '2025-12-11 (Anayasa Mahkemesi)'),
                              ('(Ek fıkra: 3/3/2025)', '2025-03-03')):
            self.assertEqual(version, read(law('(Değişik:2/3/2024-7499/34 md.)', note), mevzuat, None), note)
        self.assertEqual('2024-03-02 (7499)', read(law('(Değişik:2/3/2024-7499/34 md.)', '(1/1/2030 tarihinde)',
                                                       '(Ekim 2030)'), mevzuat, None))
        for body, message in ((law(number='66980'), 'not the consolidated text of No. 6698'),
                              (law('(Değişik:31/2/2024-7499/34 md.)'), 'the day 31/2/2024 does not exist'),
                              (law(enacted=''), 'neither an amendment note nor the day of enactment'),
                              (b'\xff', 'not UTF-8')):
            with self.assertRaises(unreadable) as refused:
                read(body, mevzuat, None)
            self.assertIn(message, str(refused.exception))
        uk = {'act': 'ukpga/2018/12'}
        self.assertEqual('2026-09-30', register_review.read_legislation_gov_uk(self.LEGISLATION_XML.encode(), uk, None))
        for body, message in ((self.LEGISLATION_XML.replace('2018/12<', '2018/120<'), 'not the data of ukpga/2018/12'),
                              (self.LEGISLATION_XML.replace('2026-09-30', '2026-9-30'), 'no point-in-time day'),
                              (self.LEGISLATION_XML.replace('dct:valid', 'dct:modified'), 'no point-in-time day'),
                              ('<Legislation', 'not Crown Legislation Markup')):
            with self.assertRaises(unreadable) as refused:
                register_review.read_legislation_gov_uk(body.encode(), uk, None)
            self.assertIn(message, str(refused.exception))
        celex = {'act': '32021R0821'}
        versions = register_review.read_cellar_versions
        today = datetime.date(2026, 9, 21)
        self.assertEqual('02021R0821-20260921', versions(self.sparql(
            '02021R0821-20251115', '02021R0821-20260921', '02021R0821-20261231').encode(), celex, today))
        self.assertEqual('32021R0821', versions(self.sparql('02021R0821-20261231').encode(), celex, today))
        # The acts the version read lacks: none consolidated by a version up to it (one consolidated only by a later
        # version included), and of those CELLAR consolidates nowhere only the ones dated after it.
        lacks = versions(self.sparql(
            '02021R0821-20251115', '02021R0821-20260921', '02021R0821-20261231',
            ('32025R0001', '2025-01-10', '02021R0821-20251115'), ('32025R0006', '2025-02-01', '02021R0821-20261231'),
            ('32025R0006', '2025-02-01', '02021R0821-20260921'), ('32026R0002', '2026-09-30', '02021R0821-20261231'),
            ('32026D0003R(01)', '2026-09-22'), ('32024R0004R(02)', '2024-03-01'),
            ('32026L0005', '2026-09-21')).encode(), celex, today)
        self.assertEqual(('02021R0821-20260921', '02021R0821-20260921 + 32026D0003R(01) + 32026R0002', [
            {'id': 'celex-32026d0003r-01-published', 'act': '32026D0003R(01)',
             'url': 'https://publications.europa.eu/resource/celex/32026D0003R%2801%29',
             'headers': {'Accept': 'application/xhtml+xml', 'Accept-Language': 'eng'}, 'reader': 'cellar'},
            {'id': 'celex-32026r0002-published', 'act': '32026R0002',
             'url': 'https://publications.europa.eu/resource/celex/32026R0002',
             'headers': {'Accept': 'application/xhtml+xml', 'Accept-Language': 'eng'}, 'reader': 'cellar'}]), lacks)
        # The act read as adopted lacks every change no version up to today consolidates.
        path, version, documents = versions(self.sparql(
            '02021R0821-20261231', ('32026R0002', '2026-09-30', '02021R0821-20261231'),
            ('32024R0004R(02)', '2024-03-01')).encode(), celex, today)
        self.assertEqual(('32021R0821', '32021R0821 + 32024R0004R(02) + 32026R0002'), (path, version))
        self.assertEqual(['32024R0004R(02)', '32026R0002'], [document['act'] for document in documents])
        for body, message in ((self.sparql('02021R08210-20251115'), "'02021R08210-20251115' is not a consolidated "
                                                                     'version of 32021R0821'),
                              (self.sparql('02021R0821-20250231'), 'the day 31/02/2025 does not exist'),
                              ('{"results": {}}', 'not a SPARQL result of CELEX numbers'),
                              ('{"results": {"bindings": [1]}}', 'not a SPARQL result of CELEX numbers'),
                              (self.sparql(('32021R0821', '2026-01-01')), "'32021R0821' is not an act changing "
                                                                          '32021R0821'),
                              (self.sparql(('C2026/123/01', '2026-01-01')), "'C2026/123/01' is not an act changing"),
                              (self.sparql(('32026R0002R(1)', '2026-01-01')), "'32026R0002R(1)' is not an act"),
                              (self.sparql(('32026R0002', '2026-1-30')), "32026R0002 is dated '2026-1-30', not "
                                                                          'YYYY-MM-DD'),
                              (self.sparql(('32026R0002', '2026-02-30')), 'the day 30/02/2026 does not exist'),
                              (self.sparql(('32026R0002', '2026-01-30'), ('32026R0002', '2026-01-31')),
                               '32026R0002 is dated both 2026-01-30 and 2026-01-31'),
                              (self.sparql(('32026R0002', '2026-01-30', '02021R0822-20260101')),
                               "'02021R0822-20260101' is not a consolidated version of 32021R0821"),
                              (json.dumps({'results': {'bindings': [{'celex': {'value': '02021R0821-20251115'},
                                                                     'by': {'value': '02021R0821-20251115'}}]}}),
                               "a consolidated version with ['by', 'celex']")):
            with self.assertRaises(unreadable) as refused:
                versions(body.encode(), celex, today)
            self.assertIn(message, str(refused.exception))
        cellar = register_review.read_cellar
        self.assertEqual('v', cellar(self.CELLAR_XHTML.encode(), {'act': '32016R0679'}, 'v'))
        self.assertEqual('v', cellar(b'<html><p>Directive 2002/58/EC</p></html>', {'act': '32002L0058'}, 'v'))
        self.assertEqual('v', cellar(b'<html><p>Regulation (EC) No 1907/2006</p></html>', {'act': '32006R1907'}, 'v'))
        self.assertEqual('v', cellar(b'<html><p>COUNCIL REGULATION (EEC) NO 3541/92</p></html>', {'act': '31992R3541'},
                                     'v'))
        self.assertEqual('v', cellar(b'<html><p>Regulation (EC) No 2271/96</p></html>', {'act': '31996R2271'}, 'v'))
        for body, act in ((self.CELLAR_XHTML.replace('2016/679', '2016/678'), '32016R0679'),
                          ('<p>Regulation (EU) 2016/679</p>', '32016R0679'),
                          ('<html><p>Regulation (EU) 2016/6790</p></html>', '32016R0679'),
                          ('<html><p>Regulation (EEC) No 13541/92</p></html>', '31992R3541'),
                          ('<html><p>Regulation (EEC) No 3541/921</p></html>', '31992R3541'),
                          ('<html><p>Regulation (EU) 44/16</p></html>', '32016R0044')):
            with self.assertRaises(unreadable, msg=body):
                cellar(body.encode(), {'act': act}, 'v')
        ecfr = {'act': 'title=15&subtitle=B&chapter=VII&subchapter=C'}
        meta = {'title': '15', 'subtitle': 'B', 'chapter': 'VII', 'subchapter': 'C',
                'latest_amendment_date': '2026-10-26'}
        self.assertEqual('2026-10-26', register_review.read_ecfr_versions(json.dumps({'meta': meta}).encode(), ecfr,
                                                                          today))
        for changed, message in (({'subtitle': 'A'}, 'the versions of another hierarchy'),
                                 ({'latest_amendment_date': None}, 'is not YYYY-MM-DD')):
            with self.assertRaises(unreadable) as refused:
                register_review.read_ecfr_versions(json.dumps({'meta': dict(meta, **changed)}).encode(), ecfr, today)
            self.assertIn(message, str(refused.exception))
        with self.assertRaises(unreadable):
            register_review.read_ecfr_versions(b'{"content_versions": []}', ecfr, today)
        title = ('<ECFR><DIV1 N="15" TYPE="TITLE"><DIV2 N="A" TYPE="SUBTITLE"><DIV3 N="VII" TYPE="CHAPTER">'
                 '<DIV4 N="C" TYPE="SUBCHAP"><P>subtitle A</P></DIV4></DIV3></DIV2><DIV2 N="B" TYPE="SUBTITLE">'
                 '<DIV3 N="I" TYPE="CHAPTER"><P>chapter I</P></DIV3><DIV3 N="VII" TYPE="CHAPTER">'
                 '<DIV4 N="C" TYPE="SUBCHAP"><DIV5 N="740" TYPE="PART"><P>ENC</P></DIV5></DIV4></DIV3></DIV2>'
                 '</DIV1></ECFR>')
        record = register_review.ecfr_record(title.encode(), ecfr)
        self.assertEqual(b'<DIV4 N="C" TYPE="SUBCHAP"><DIV5 N="740" TYPE="PART"><P>ENC</P></DIV5></DIV4>', record)
        self.assertEqual(record, register_review.ecfr_record(title.replace('chapter I', 'amended').encode(), ecfr))
        self.assertEqual('v', register_review.read_ecfr(title.encode(), ecfr, 'v'))
        part = {'act': 'title=15&subtitle=B&chapter=VII&subchapter=C&part=740'}
        self.assertEqual(b'<DIV5 N="740" TYPE="PART"><P>ENC</P></DIV5>',
                         register_review.ecfr_record(b'<DIV5 N="740" TYPE="PART"><P>ENC</P></DIV5>', part))
        self.assertEqual(b'<DIV5 N="740" TYPE="PART"><P>ENC</P></DIV5>', register_review.ecfr_record(title.encode(),
                                                                                                    part))
        for body, act, message in ((title.replace('N="740"', 'N="742"'), part, 'no part 740'),
                                   (title.replace('N="B"', 'N="Z"'), ecfr, 'no subtitle B'),
                                   ('<DIV5 N="742" TYPE="PART"/>', part, 'no part 740'),
                                   ('<DIV5', part, 'not eCFR XML')):
            with self.assertRaises(unreadable) as refused:
                register_review.read_ecfr(body.encode(), act, 'v')
            self.assertIn(message, str(refused.exception))
        with tempfile.TemporaryDirectory() as scratch:
            (Path(scratch) / 'title.xml').write_bytes(title.encode())
            manifest = {'items': [{'id': 'ecfr', 'file': 'title.xml', 'version': '2026-10-26',
                                   'sha256': hashlib.sha256(title.encode()).hexdigest()}]}
            reading = register_review.Review({}, manifest, scratch).reading(dict(ecfr, id='ecfr', reader='ecfr'))
            self.assertEqual(('2026-10-26', hashlib.sha256(record).hexdigest()), reading)
        lists = {'act': register_review.WASSENAAR_LIST}
        link = 'https://www.wassenaar.org/wp-content/uploads/2026/07/List-of-Dual-Use-Goods-and-Technologies-2025.pdf'
        self.assertEqual('2026/07/List-of-Dual-Use-Goods-and-Technologies-2025.pdf',
                         register_review.read_wassenaar_lists(f'<a href="{link}">{link}</a>'.encode(), lists, today))
        with self.assertRaises(unreadable):
            register_review.read_wassenaar_lists(f'{link} {link.replace("2025", "2024")}'.encode(), lists, today)
        self.assertEqual('v', register_review.read_pdf(b'%PDF-1.7', {}, 'v'))
        with self.assertRaises(unreadable):
            register_review.read_pdf(b'<html>', {}, 'v')

    def test_due_prints_the_computed_review_and_unverified_events(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_tree(root, self.register(self.entries()))
            code, output = self.tool(root, 'due', today='2026-11-01')
            self.assertEqual(0, code, output)
            self.assertIn('a light review is due on 2026-12-15, after IETF 127 (2026-11-20)', output)
            self.assertIn('IETF 127 on 2026-11-20 is unverified', output)
            self.assertNotIn('overdue', output)
            self.assertIn('overdue', self.tool(root, 'due')[1])
            (root / 'cmake' / 'standards-register.json').write_text(json.dumps(self.register(
                self.entries(), review={'cadence': 'c', 'next': '2026-12-15'})), encoding='utf-8')
            code, output = self.tool(root, 'due')
            self.assertEqual(2, code)
            self.assertIn('the review schedule is malformed', output)

    def test_plan_reads_a_paper_by_hand_at_its_link(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            paper = self.entry(id='crypto.siphash', codepoints=[], review='2027-06-01',
                               spec={'type': 'paper', 'ref': 'https://doi.org/10.1007/978-3-642-34931-7_28',
                                     'posted': '2012-12', 'status': 'published'})
            self.write_tree(root, self.register(self.entries() + [paper]))
            code, output = self.tool(root, 'plan', '--out', str(root / 'sources.json'))
            self.assertEqual(0, code, output)
            self.assertIn('7 sources to download once the user approves them, 4 items read by hand', output)
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            item = next(item for item in plan['manual'] if item['entries'] == ['crypto.siphash'])
            self.assertEqual('https://doi.org/10.1007/978-3-642-34931-7_28', item['url'])
            self.assertIn('was withdrawn or superseded', item['read'])

    def test_plan_lists_every_primary_source_and_the_items_read_by_hand(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_tree(root, self.register(self.entries()))
            code, output = self.tool(root, 'plan', '--out', str(root / 'sources.json'))
            self.assertEqual(0, code, output)
            self.assertIn('7 sources to download once the user approves them, 3 items read by hand', output)
            plan = json.loads((root / 'sources.json').read_text(encoding='utf-8'))
            self.assertEqual(register_review.PLAN_SCHEMA, plan['schema'])
            urls = {source['id']: source['url'] for source in plan['sources']}
            self.assertEqual('https://www.rfc-editor.org/rfc/rfc9000.json', urls['rfc9000'])
            self.assertEqual('https://datatracker.ietf.org/doc/draft-ietf-quic-multipath/doc.json',
                             urls['draft-ietf-quic-multipath'])
            self.assertEqual('https://www.iana.org/assignments/quic/quic.xml', urls['iana-quic'])
            quic = next(source for source in plan['sources'] if source['id'] == 'iana-quic')
            self.assertEqual(['QUIC Versions', 'QUIC Transport Parameters'], quic['registries'])
            manual = {item['url']: item for item in plan['manual']}
            self.assertEqual(['ops.trace-context'], manual['https://www.w3.org/TR/trace-context/']['entries'])
            self.assertIn('Object identifiers', next(item['read'] for item in plan['manual']
                                                     if item['entries'] == ['http.h3']))
            self.assertIn('https://www.ietf.org/meeting/upcoming/', manual)
            for broken, message in (({'spec': {'type': 'rfc', 'ref': 'RFC9000bis', 'status': 'current'}},
                                     'is not "RFC <number>"'),
                                    ({'spec': dict(self.DRAFT, ref='Draft QUIC')}, 'is not a draft name'),
                                    ({'codepoints': [{'registry': 'Nowhere', 'name': 'n', 'value': '1'}]},
                                     "registry 'Nowhere' has no source")):
                (root / 'cmake' / 'standards-register.json').write_text(
                    json.dumps(self.register([self.entry(**broken)])), encoding='utf-8')
                code, output = self.tool(root, 'plan', '--out', str(root / 'sources.json'))
                self.assertEqual(2, code)
                self.assertIn(message, output)

    def test_fetch_records_digests_and_refuses_or_records_what_it_cannot_read(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_tree(root, self.register(self.entries()))
            bodies = self.records()
            bodies['rfc9114'] = urllib.error.URLError('unreachable')
            bodies['iana-quic'] = http.client.IncompleteRead(b'part')  # a body cut short is no OSError
            bodies['draft-ietf-quic-multipath'] = 'x' * 64
            with patch.object(register_review, 'FETCH_LIMIT', 63):
                plan = self.fetched(root, bodies)
            manifest = json.loads((root / 'fetched' / 'manifest.json').read_text(encoding='utf-8'))
            self.assertEqual(register_review.FETCH_SCHEMA, manifest['schema'])
            self.assertEqual('2026-12-16', manifest['day'])
            self.assertEqual(plan, manifest['plan'])
            items = {item['id']: item for item in manifest['items']}
            body = (root / 'fetched' / 'rfc9000.json').read_bytes()
            self.assertEqual(hashlib.sha256(body).hexdigest(), items['rfc9000']['sha256'])
            self.assertEqual(len(body), items['rfc9000']['bytes'])
            self.assertIn('unreachable', items['rfc9114']['error'])
            self.assertEqual('IncompleteRead: IncompleteRead(4 bytes read)', items['iana-quic']['error'])
            self.assertEqual('larger than 63 bytes', items['draft-ietf-quic-multipath']['error'])
            insecure = dict(plan, sources=[dict(plan['sources'][0], url='http://www.rfc-editor.org/x.json')])
            refused = register_review.fetch_sources(insecure, root / 'refused', opener=self.opener(bodies))
            self.assertIn('refused', refused['items'][0]['error'])
            with self.assertRaises(register_review.ReviewError):
                register_review.fetch_sources({'schema': 'other'}, root / 'refused')

    def test_compare_reports_changes_and_proposals_and_edits_no_row(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_tree(root, self.register(self.entries()))
            roadmap = (root / 'docs' / 'ROADMAP.md').read_bytes()
            before = (root / 'cmake' / 'standards-register.json').read_bytes()
            obsoleted = json.dumps({'doc_id': 'RFC9000', 'obsoleted_by': ['RFC9999']})
            tracker = json.loads(self.records()['draft-ietf-quic-multipath'])
            newer = json.dumps(dict(tracker, rev='22', rev_history=[{'rev': '22', 'published': '2026-11-30T08:00:00Z'}],
                                    iesg_state='IESG Evaluation'))
            rfc = json.dumps(dict(json.loads(self.records()['draft-ietf-quic-reliable-stream-reset']), state='RFC',
                                  aliases=['draft-ietf-quic-reliable-stream-reset', 'rfc9911']))
            self.fetched(root, self.records(rfc9000=obsoleted, rfc9114='{"doc_id": "RFC9114"}', multipath=newer,
                                            reset=rfc))
            report = root / 'docs' / 'review.md'
            code, output = self.tool(root, 'compare', '--fetched', str(root / 'fetched'), '--report', str(report))
            self.assertEqual(0, code, output)
            self.assertEqual(before, (root / 'cmake' / 'standards-register.json').read_bytes())
            text = report.read_text(encoding='utf-8')
            self.assertIn('proposed, not applied', text)
            self.assertIn('quic.transport: RFC 9000 is obsoleted by RFC 9999', text)
            self.assertIn('the pinned RFC 9000 is now obsoleted by RFC 9999 (was current)', text)
            self.assertIn('quic.multipath: draft-ietf-quic-multipath-22 posted 2026-11-30 (was -21 posted 2026-03-17)',
                          text)
            self.assertIn('the watched draft-ietf-quic-multipath is now iesg (was active)', text)
            self.assertIn('is now published as RFC 9911 (re-pin the entry to it', text)
            self.assertIn('QUIC Versions QUIC = 0x00000001 confirmed by IANA (an unnamed record citing RFC 9000)', text)
            self.assertIn('TLS ALPN Protocol IDs HTTP/3 = h3 confirmed by IANA', text)
            self.assertIn('TLS ExtensionType Values compress_certificate = 27 confirmed by IANA', text)
            self.assertIn("QUIC v2 = 0x6b3343cf: IANA names 0x6b3343cf [] and cites ['RFC 9369']", text)
            self.assertIn('max_datagram_frame_size = 0x21: IANA assigns max_datagram_frame_size another value',
                          text)
            self.assertIn('ack_frequency = 0xff04de1b: IANA has neither', text)
            self.assertNotIn('version_information', text)
            self.assertIn("rfc9114: not an RFC Editor record ('obsoleted_by')", text)
            self.assertIn('## Triggers a full re-research\n\n- quic.transport\n', text)
            self.assertIn('the Object identifiers values', text)
            self.assertIn('https://www.ietf.org/meeting/upcoming/', text)
            self.assertIn('webtransport.h3: draft-ietf-webtrans-http3-16 posted 2026-07-01 (was -16 posted 2026-07)',
                          text)
            self.assertIn('Retrieved on 2026-12-16 by `scripts/register-review.py fetch`.', text)
            quic = self.QUIC_XML.encode()
            self.assertIn(f'| [iana-quic](https://www.iana.org/assignments/quic/quic.xml) | {len(quic)} | '
                          f'`{hashlib.sha256(quic).hexdigest()}` |', text)
            code, output = self.tool(root, 'compare', '--fetched', str(root / 'fetched'), '--report', str(report),
                                     '--apply')
            self.assertEqual(0, code, output)
            self.assertIn('the next review is a full scan due on 2027-01-10', output)
            register = json.loads((root / 'cmake' / 'standards-register.json').read_text(encoding='utf-8'))
            entries = {entry['id']: entry for entry in register['entries']}
            self.assertEqual({'type': 'rfc', 'ref': 'RFC 9000', 'status': 'obsoleted', 'obsoleted_by': ['RFC 9999']},
                             entries['quic.transport']['spec'])
            self.assertNotIn('verified', entries['quic.transport']['codepoints'][0])
            self.assertFalse(entries['quic.transport']['codepoints'][1]['verified'])
            self.assertEqual(('22', '2026-11-30', 'iesg'), tuple(entries['quic.multipath']['spec'][key]
                                                                 for key in ('revision', 'posted', 'status')))
            self.assertEqual('rfc-editor', entries['quic.reliable-reset']['spec']['status'])
            self.assertEqual({'day': '2026-12-16', 'scan': 'light', 'ref': 'docs/review.md',
                              'triggers': ['quic.transport']}, register['review']['runs'][-1])
            self.assertEqual('2027-01-10', register['review']['next'])
            self.assertEqual('2026-12-16', register['snapshot']['date'])
            self.assertEqual(roadmap, (root / 'docs' / 'ROADMAP.md').read_bytes())
            self.assertEqual(b'\n', (root / 'cmake' / 'standards-register.json').read_bytes()[-1:])
            self.assertNotIn(b'\r', (root / 'cmake' / 'standards-register.json').read_bytes())
            code, output = self.guard(root, '2026-12-16')
            self.assertEqual(1, code, output)
            self.assertEqual(['FAIL: standards entry quic.transport: pinned RFC 9000 is obsoleted by RFC 9999 in the '
                              'snapshot; pin the replacement'],
                             [line for line in output.splitlines() if 'legal register' not in line])

    def test_a_clean_refresh_passes_the_guard_and_tampering_or_strays_are_refused(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_tree(root, self.register(self.entries()))
            self.fetched(root, self.records())
            outside = Path(scratch).parent / 'review-outside.md'
            code, output = self.tool(root, 'compare', '--fetched', str(root / 'fetched'), '--report', str(outside),
                                     '--apply')
            outside.unlink()
            self.assertEqual(2, code)
            self.assertIn('outside the repository', output)
            report = root / 'docs' / 'review.md'
            code, output = self.tool(root, 'compare', '--fetched', str(root / 'fetched'), '--report', str(report),
                                     '--apply')
            self.assertEqual(0, code, output)
            self.assertIn('the next review is a light scan due on 2027-04-20, after IETF 128 (2027-03-26)', output)
            self.assertIn('Register changes', report.read_text(encoding='utf-8'))
            self.assertIn('quic.reliable-reset: draft-ietf-quic-reliable-stream-reset-11 posted 2026-05-01 (was -11)\n',
                          report.read_text(encoding='utf-8'))
            self.assertIn('## Triggers a full re-research\n\nNone.', report.read_text(encoding='utf-8'))
            code, output = self.guard(root, '2026-12-16')
            self.assertEqual(0, code, output)
            self.assertIn('next review 2027-04-20', output)
            (root / 'fetched' / 'rfc9000.json').write_text('{"tampered": true}', encoding='utf-8')
            code, output = self.tool(root, 'compare', '--fetched', str(root / 'fetched'), '--report', str(report))
            self.assertEqual(2, code)
            self.assertIn('does not match the SHA-256', output)
            code, output = self.tool(root, 'compare', '--fetched', str(root / 'nothing'), '--report', str(report))
            self.assertEqual(2, code)

    def test_the_recommended_column_is_recorded_and_a_discouraged_pinned_codepoint_triggers(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            tls = {'type': 'rfc', 'ref': 'RFC 9846', 'status': 'current'}
            groups = self.entry(id='crypto.groups', spec=tls, review='2027-06-01', codepoints=[
                {'registry': 'TLS Supported Groups', 'name': 'secp224r1', 'value': '21', 'recommended': 'Y'},
                {'registry': 'TLS Supported Groups', 'name': 'x25519', 'value': '0x001D', 'recommended': 'Y'},
                {'registry': 'TLS SignatureScheme', 'name': 'ed25519', 'value': '0x0807', 'verified': False}])
            legacy = self.entry(id='crypto.legacy', state='watch', review='2027-06-01',
                                spec={'type': 'rfc', 'ref': 'RFC 9847', 'status': 'current'},
                                codepoints=[{'registry': 'TLS Supported Groups', 'name': 'secp224r1', 'value': '21',
                                             'recommended': 'N'}])
            self.write_tree(root, self.register(self.entries() + [groups, legacy]))
            bodies = dict(self.records(), **{'iana-tls-parameters': self.TLS_XML,
                                             'rfc9846': json.dumps({'doc_id': 'RFC9846', 'obsoleted_by': []}),
                                             'rfc9847': json.dumps({'doc_id': 'RFC9847', 'obsoleted_by': []})})
            self.fetched(root, bodies)
            report = root / 'docs' / 'review.md'
            code, output = self.tool(root, 'compare', '--fetched', str(root / 'fetched'), '--report', str(report),
                                     '--apply')
            self.assertEqual(0, code, output)
            self.assertIn('the next review is a full scan due on 2027-01-10', output)
            text = report.read_text(encoding='utf-8')
            self.assertIn('- crypto.groups: TLS Supported Groups secp224r1 is Recommended "D" by IANA (was "Y")\n',
                          text)
            self.assertIn('- crypto.groups (NET-STD): IANA now discourages secp224r1 (Recommended "D"); drop it from '
                          'the accepted policy and the entry before the full re-research this triggers', text)
            self.assertIn('## Triggers a full re-research\n\n- crypto.groups\n\n', text)
            self.assertIn('- crypto.groups: TLS Supported Groups x25519 has no single Recommended value in the IANA '
                          'file (None)', text)
            self.assertIn('- crypto.groups: TLS SignatureScheme ed25519 = 0x0807 confirmed by IANA\n', text)
            self.assertIn('- crypto.groups: TLS SignatureScheme ed25519 is Recommended "Y" by IANA (not recorded)',
                          text)
            self.assertIn('- crypto.legacy: TLS Supported Groups secp224r1 is Recommended "D" by IANA (was "N")', text)
            self.assertNotIn('crypto.legacy (', text)
            self.assertIn('- http.h3: TLS ExtensionType Values compress_certificate is Recommended "Y" by IANA (not '
                          'recorded)', text)
            register = json.loads((root / 'cmake' / 'standards-register.json').read_text(encoding='utf-8'))
            entries = {entry['id']: entry for entry in register['entries']}
            self.assertEqual(['D', 'Y', 'Y'], [point.get('recommended')
                                               for point in entries['crypto.groups']['codepoints']])
            self.assertNotIn('verified', entries['crypto.groups']['codepoints'][2])
            self.assertEqual('D', entries['crypto.legacy']['codepoints'][0]['recommended'])
            self.assertEqual(['crypto.groups'], register['review']['runs'][-1]['triggers'])
            code, output = self.guard(root, '2026-12-16')
            self.assertEqual(1, code, output)
            self.assertIn('IANA discourages secp224r1', output)
            self.assertNotIn('crypto.legacy', output)

    def test_a_full_scan_reads_the_triggering_documents_and_records_its_re_research(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            entries = self.entries()
            review = self.register(entries)['review']
            review['runs'].append({'day': '2026-11-02', 'scan': 'light', 'ref': 'docs/research.md',
                                   'triggers': ['ops.trace-context', 'quic.transport', 'webtransport.h3']})
            review['next'] = '2026-11-27'
            self.write_tree(root, self.register(entries, review=review))
            self.assertIn('full review is due on 2026-11-27', self.tool(root, 'due')[1])
            plan = self.fetched(root, self.records())
            self.assertEqual('light', plan['scan'])
            self.assertNotIn('text', {source['kind'] for source in plan['sources']})
            code, output = self.tool(root, 'plan', '--scan', 'full', '--out', str(root / 'full.json'))
            self.assertEqual(0, code, output)
            self.assertIn('9 sources to download once the user approves them, 4 items read by hand', output)
            full = json.loads((root / 'full.json').read_text(encoding='utf-8'))
            self.assertEqual('full', full['scan'])
            texts = {source['id']: (source['url'], source['entries']) for source in full['sources']
                     if source['kind'] == 'text'}
            self.assertEqual({'rfc9000-text': ('https://www.rfc-editor.org/rfc/rfc9000.txt', ['quic.transport']),
                              'draft-ietf-webtrans-http3-16-text': (
                                  'https://www.ietf.org/archive/id/draft-ietf-webtrans-http3-16.txt',
                                  ['webtransport.h3'])}, texts)
            manual = {item['read']: item['entries'] for item in full['manual']}
            self.assertEqual(['ops.trace-context'], manual['the full text of https://www.w3.org/TR/trace-context/'])
            manifest = register_review.fetch_sources(full, root / 'full', opener=self.opener(self.records()),
                                                     today=datetime.date(2026, 12, 16))
            self.assertIn('draft-ietf-webtrans-http3-16-text.txt', {item['file'] for item in manifest['items']})
            report = root / 'docs' / 'full-review.md'
            argv = ('compare', '--scan', 'full', '--report', str(report))
            for fetched, extra, message in (
                    ('fetched', (), 'a full scan compares a plan made with `plan --scan full`'),
                    ('full', ('--apply',), 'an applied full scan records its re-research document'),
                    ('full', ('--ref', 'docs/research.md'), '--ref names the re-research document of an applied full'),
                    ('full', ('--apply', '--ref', 'docs/research.md#n83'), 'docs/research.md#n83 does not cite the '
                                                                          'report full-review.md'),
                    ('full', ('--apply', '--ref', 'docs/missing.md'), 'docs/missing.md does not exist')):
                with self.subTest(extra=extra):
                    code, output = self.tool(root, *argv, '--fetched', str(root / fetched), *extra)
                    self.assertEqual(2, code, output)
                    self.assertIn(message, output)
            code, output = self.tool(root, 'compare', '--ref', 'docs/research.md', '--fetched', str(root / 'full'),
                                     '--report', str(report), '--apply')
            self.assertEqual(2, code, output)
            self.assertIn('a light run cites its report', output)
            (root / 'docs' / 're-research.md').write_text('<a id="delta"></a>Pins: [scan](full-review.md).\n',
                                                          encoding='utf-8')
            code, output = self.tool(root, *argv, '--fetched', str(root / 'full'), '--apply', '--ref',
                                     'docs/re-research.md#delta')
            self.assertEqual(0, code, output)
            self.assertIn('the next review is a light scan due on 2027-04-20, after IETF 128 (2027-03-26)', output)
            text = report.read_text(encoding='utf-8')
            self.assertIn('(full scan, standards register)', text)
            self.assertIn('## Read in full\n\n'
                          '- webtransport.h3: https://www.ietf.org/archive/id/draft-ietf-webtrans-http3-16.txt\n'
                          '- quic.transport: https://www.rfc-editor.org/rfc/rfc9000.txt\n', text)
            register = json.loads((root / 'cmake' / 'standards-register.json').read_text(encoding='utf-8'))
            self.assertEqual({'day': '2026-12-16', 'scan': 'full', 'ref': 'docs/re-research.md#delta',
                              'triggers': []}, register['review']['runs'][-1])
            self.assertIn('report docs/full-review.md', register['snapshot']['method'])
            self.assertEqual(0, self.tool(root, 'plan', '--scan', 'full', '--out', str(root / 'again.json'))[0])
            again = json.loads((root / 'again.json').read_text(encoding='utf-8'))
            self.assertNotIn('text', {source['kind'] for source in again['sources']})
            for trigger, message in (('quic.gone', 'names the triggering entry quic.gone, which the register lacks'),
                                     ('webtransport.h3', 'reads draft-ietf-webtrans-http3 at its revision')):
                with self.subTest(trigger=trigger):
                    owed = dict(review, runs=review['runs'][:1] + [dict(review['runs'][1], triggers=[trigger])])
                    bare = [dict(entry, spec={key: value for key, value in entry['spec'].items()
                                              if key != 'revision'}) for entry in entries]
                    self.write_tree(root, self.register(bare, review=owed))
                    code, output = self.tool(root, 'plan', '--scan', 'full', '--out', str(root / 'refused.json'))
                    self.assertEqual(2, code, output)
                    self.assertIn(message, output)

    def test_record_adds_a_run_by_hand_and_moves_the_due_day(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            self.write_tree(root, self.register(self.entries()))
            code, output = self.tool(root, 'record', '--scan', 'light', '--ref', 'docs/research.md#n83', '--day',
                                     '2026-11-01', today='2026-11-01')
            self.assertEqual(0, code, output)
            self.assertIn('the next review is a light scan due on 2026-12-15', output)
            code, output = self.tool(root, 'record', '--scan', 'full', '--ref', 'docs/research.md', '--day',
                                     '2026-11-25', today='2026-11-25')
            self.assertIn('the next review is a light scan due on 2027-04-20', output)
            register = json.loads((root / 'cmake' / 'standards-register.json').read_text(encoding='utf-8'))
            self.assertEqual(['2026-10-27', '2026-11-01', '2026-11-25'],
                             [run['day'] for run in register['review']['runs']])
            self.assertEqual(('2026-11-25', 'full', 'docs/research.md'),
                             tuple(register['review']['runs'][-1][key] for key in ('day', 'scan', 'ref')))
            self.assertEqual('2027-04-20', register['review']['next'])
            self.assertEqual(0, self.guard(root, '2026-11-25')[0])
            self.assertEqual(2, self.tool(root, 'record', '--scan', 'full', '--ref', '../elsewhere.md')[0])
            self.assertEqual(2, self.tool(root, 'record', '--scan', 'full', '--ref', 'docs/missing.md')[0])
            self.assertEqual(2, self.tool(root, 'record', '--scan', 'full', '--ref', 'docs/research.md',
                                          '--day', '2026-11')[0])

    def test_record_readers_refuse_unexpected_shapes(self):
        for body in ('[]', '{"obsoleted_by": "RFC 1"}', '{"obsoleted_by": ["errata"]}', 'not json'):
            with self.assertRaises(register_review.Unreadable):
                register_review.read_rfc_editor(body)
        for body in ('{}', '{"rev": 1, "state": "Active"}', '{"rev": "01", "state": "Dormant"}'):
            with self.assertRaises(register_review.Unreadable):
                register_review.read_datatracker(body)
        self.assertEqual({'status': 'expired', 'revision': '03', 'posted': '2026-01-02', 'rfc': None},
                         register_review.read_datatracker('{"rev": "03", "state": "Expired", '
                                                          '"time": "2026-01-02T00:00:00Z"}'))
        self.assertEqual('replaced', register_review.read_datatracker('{"rev": "03", "state": "Replaced"}')['status'])
        queued = '{"rev": "11", "state": "Active", "iesg_state": "RFC Ed Queue", "rfceditor_state": "AUTH48"}'
        self.assertEqual('rfc-editor', register_review.read_datatracker(queued)['status'])
        self.assertEqual('active', register_review.read_datatracker('{"rev": "11", "state": "Active", '
                                                                    '"iesg_state": "I-D Exists"}')['status'])
        with self.assertRaises(register_review.Unreadable):
            register_review.read_iana('<registry', ['QUIC Versions'])
        with self.assertRaises(register_review.Unreadable):
            register_review.read_iana(self.QUIC_XML, ['QUIC Frame Types'])

    def test_iana_records_are_read_in_the_shapes_the_registry_files_have(self):
        quic = register_review.read_iana(self.QUIC_XML, ['QUIC Versions', 'QUIC Transport Parameters'])
        self.assertEqual([({0}, set(), {'RFC 9000'}, None), ({1}, set(), {'RFC 9000'}, None),
                          ({0x6b3343cf}, set(), {'RFC 9369'}, None)], quic['QUIC Versions'])
        self.assertEqual(({0x2ab2}, {'greasequicbit'}, {'draft-ietf-quic-bit-grease'}, None),
                         quic['QUIC Transport Parameters'][2])
        tls = register_review.read_iana(self.TLS_XML, ['TLS Supported Groups', 'TLS SignatureScheme'])
        self.assertEqual(['D', None], [record.recommended for record in tls['TLS Supported Groups']])
        self.assertEqual(({0x0807}, {'ed25519'}, {'RFC 9846'}, 'Y'), tls['TLS SignatureScheme'][0])
        alpn = register_review.read_iana(self.ALPN_XML, ['TLS Application-Layer Protocol Negotiation (ALPN) Protocol '
                                                         'IDs', 'TLS ExtensionType Values'])
        self.assertEqual([({'h3'}, {'http3'}, {'RFC 9114'}, None)],
                         alpn['TLS Application-Layer Protocol Negotiation (ALPN) Protocol IDs'])
        self.assertEqual(({27}, {'compresscertificate'}, set(), 'Y'), alpn['TLS ExtensionType Values'][0])
        self.assertEqual((set(), {'unassigned'}, set(), None), alpn['TLS ExtensionType Values'][1])
        self.assertIsNone(register_review.sequence_of('0x68 0x33'))
        self.assertIsNone(register_review.sequence_of('"h3"'))
        self.assertEqual('h2', register_review.sequence_of('0x68 0x32 ("h2")'))
        self.assertIsNone(register_review.document_of('uri', 'https://example.org'))
        self.assertIsNone(register_review.document_of('rfc', 'errata'))
        self.assertEqual('draft-ietf-moq-transport', register_review.document_of('draft', 'draft-ietf-moq-transport'))
        versions = quic['QUIC Versions']
        unnamed = {'registry': 'QUIC Versions', 'name': 'QUIC version 2', 'value': '0x6b3343cf'}
        self.assertEqual(('verified', ' (an unnamed record citing RFC 9369)'),
                         register_review.judge_codepoint(unnamed, versions, 'RFC 9369'))
        self.assertEqual('name', register_review.judge_codepoint(unnamed, versions, None)[0])
        grease = {'registry': 'QUIC Transport Parameters', 'name': 'grease_quic_bit', 'value': '0x2ab2'}
        parameters = quic['QUIC Transport Parameters']
        self.assertEqual(('verified', ''), register_review.judge_codepoint(grease, parameters, 'RFC 9000'))
        # A named record confirms by its name alone: citing the entry's document never excuses another name.
        self.assertEqual('name', register_review.judge_codepoint(dict(grease, name='quic_bit'), parameters,
                                                                 'draft-ietf-quic-bit-grease')[0])


class RegisterIds(unittest.TestCase):
    """scripts/gen-register-ids.py: each owning module's header of the codepoints the standards register assigns to it,
    with GREASE patterns expanded, unverified IANA values kept apart and every unusable input refused."""

    HEADER = 'engine/foundation/net/include/crd/net/register_ids.hpp'
    CRYPTO = 'engine/foundation/crypto/include/crd/crypto/register_ids.hpp'

    @staticmethod
    def entry(identifier, owner, *codepoints):
        return {'id': identifier, 'owner': owner, 'codepoints': [dict(point) for point in codepoints]}

    def entries(self):
        return [self.entry('cerid.game', 'net', {'registry': 'Cerid protocol identifiers', 'name': 'game',
                                                 'value': '0x0002'}),
                self.entry('cerid.agent', 'net', {'registry': 'Cerid protocol identifiers', 'name': 'agent',
                                                  'value': '0x0006'}),
                self.entry('cerid.negotiation', 'net', {'registry': 'Cerid GREASE values', 'name': 'grease',
                                                        'value': '0x?A?A'}),
                self.entry('quic.transport', 'net', {'registry': 'QUIC Versions', 'name': 'QUIC version 1',
                                                     'value': '0x00000001', 'verified': False}),
                self.entry('http.h3', 'net', {'registry': 'TLS ALPN Protocol IDs', 'name': 'HTTP/3', 'value': 'h3'}),
                self.entry('tls.hybrid-kex', 'crypto', {'registry': 'TLS Supported Groups', 'name': 'X25519MLKEM768',
                                                        'value': '0x11EC', 'verified': False})]

    def generate(self, entries, modules=('core', 'net'), check=False, header=None, output_path=None):
        """(exit code, output, header text or None) of the generator over a fixture tree; the header read back is
        crd-net's unless `output_path` names another owner's."""
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            (root / 'cmake').mkdir()
            (root / 'cmake' / 'standards-register.json').write_text(json.dumps({'entries': entries}),
                                                                     encoding='utf-8')
            (root / 'CMakeLists.txt').write_text(''.join(f'crd_module(engine/foundation/{name} engine/{name})\n'
                                                         for name in modules), encoding='utf-8')
            if header is not None:
                (root / self.HEADER).parent.mkdir(parents=True)
                (root / self.HEADER).write_text(header, encoding='utf-8')
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = register_ids.main(['--root', str(root)] + (['--check'] if check else []))
            written = root / (output_path or self.HEADER)
            return code, output.getvalue(), written.read_text(encoding='utf-8') if written.exists() else None

    def test_header_holds_protocols_grease_and_typed_iana_values(self):
        code, _, header = self.generate(self.entries())
        self.assertEqual(0, code)
        self.assertIn('    Game  = 0x0002U, // cerid.game', header)
        self.assertIn('    case ProtocolId::Agent:\n        return "cerid.agent";', header)
        self.assertIn('0x0A0AU, 0x1A1AU, 0x2A2AU', header)
        self.assertIn('0xFAFAU,', header)
        self.assertIn('return (value & 0x0F0FU) == 0x0A0AU &&\n           ((value >> 12U) & 0x0FU) == '
                      '((value >> 4U) & 0x0FU);', header)
        self.assertIn('namespace iana::unverified\n{\n// quic.transport: QUIC Versions, QUIC version 1\n'
                      'inline constexpr u32 kQuicVersion1 = 0x00000001U;', header)
        self.assertIn('namespace iana\n{\n// http.h3: TLS ALPN Protocol IDs, HTTP/3\n'
                      'inline constexpr std::string_view kAlpnHttp3 = "h3";', header)
        self.assertNotIn('X25519', header)  # crypto's codepoints go to crypto's header
        self.assertNotIn('\r', header)

    def test_crypto_header_holds_algorithm_ids_their_class_and_typed_iana_values(self):
        entries = self.entries() + [
            self.entry('crypto.ml-dsa', 'crypto', {'registry': 'Cerid algorithm identifiers', 'name': 'ml-dsa-65',
                                                   'value': '0x0302'},
                       {'registry': 'Cerid algorithm identifiers', 'name': 'ml-dsa-87', 'value': '0x0303'}),
            self.entry('crypto.aes-gcm', 'crypto', {'registry': 'Cerid algorithm identifiers', 'name': 'aes-128-gcm',
                                                    'value': '0x0501'},
                       {'registry': 'AEAD Algorithms', 'name': 'AEAD_AES_128_GCM', 'value': '1', 'verified': False}),
            self.entry('pki.ml-dsa', 'crypto', {'registry': 'Object identifiers', 'name': 'id-ml-dsa-65',
                                                'value': '2.16.840.1.101.3.4.3.18', 'verified': False})]
        code, _, header = self.generate(entries, modules=('core', 'net', 'crypto'), output_path=self.CRYPTO)
        self.assertEqual(0, code)
        self.assertIn('namespace crd::crypto\n{', header)
        self.assertIn('enum class AlgorithmId : u16\n{\n    MlDsa65   = 0x0302U, // crypto.ml-dsa\n'
                      '    MlDsa87   = 0x0303U, // crypto.ml-dsa\n    Aes128Gcm = 0x0501U, // crypto.aes-gcm\n};', header)
        self.assertIn('    case AlgorithmId::MlDsa65:\n    case AlgorithmId::MlDsa87:\n        return "crypto.ml-dsa";\n'
                      '    case AlgorithmId::Aes128Gcm:', header)  # one entry's values share a return
        self.assertIn('inline constexpr AlgorithmId kAlgorithmIds[] = {', header)
        self.assertIn('    case AlgorithmId::Aes128Gcm:\n        return "crypto.aes-gcm";', header)
        self.assertIn('    PostQuantumSignature = 0x03U, // post-quantum signature', header)
        self.assertIn('    return static_cast<AlgorithmClass>(static_cast<u16>(id) >> 8U);', header)
        self.assertIn('inline constexpr u16 kGroupX25519mlkem768 = 0x11ECU;', header)
        self.assertIn('inline constexpr u16 kAeadAes128Gcm = 0x0001U;', header)
        self.assertIn('inline constexpr std::string_view kOidIdMlDsa65 = "2.16.840.1.101.3.4.3.18";', header)
        self.assertNotIn('ProtocolId', header)
        self.assertNotIn('Grease', header)

    def test_pattern_tests_follow_any_pattern(self):
        self.assertEqual(['    return (value & 0x0F0FU) == 0x0A0AU &&',
                          '           ((value >> 12U) & 0x0FU) == ((value >> 4U) & 0x0FU);'],
                         register_ids.pattern_test('0x?A?A'))
        self.assertEqual(['    return (value & 0xFF00U) == 0xBE00U &&',
                          '           ((value >> 4U) & 0x0FU) == ((value >> 0U) & 0x0FU);'],
                         register_ids.pattern_test('0xBE??'))
        self.assertEqual(['    return ((value >> 12U) & 0x0FU) == ((value >> 8U) & 0x0FU) &&',
                          '           ((value >> 12U) & 0x0FU) == ((value >> 4U) & 0x0FU) &&',
                          '           ((value >> 12U) & 0x0FU) == ((value >> 0U) & 0x0FU);'],
                         register_ids.pattern_test('0x????'))
        self.assertEqual([0x0B0B + 0x1010 * nibble for nibble in range(16)], register_ids.expand('w', '0x?B?B'))

    def test_check_passes_on_the_committed_bytes_and_fails_on_drift(self):
        _, _, header = self.generate(self.entries())
        self.assertEqual(0, self.generate(self.entries(), check=True, header=header)[0])
        code, output, _ = self.generate(self.entries(), check=True, header=header.replace('0x0002U', '0x0003U'))
        self.assertEqual(1, code)
        self.assertIn('differs from scripts/gen-register-ids.py', output)
        self.assertEqual(1, self.generate(self.entries(), check=True)[0])
        self.assertEqual(0, self.generate(self.entries(), modules=('core',))[0])  # no net module: nothing to write

    def assert_refused(self, entries, message, modules=('core', 'net')):
        code, output, header = self.generate(entries, modules=modules)
        self.assertEqual(1, code)
        self.assertIn(message, output)
        self.assertIsNone(header)

    def test_unusable_registers_are_refused(self):
        def point(registry, value, **extra):
            return self.entry('cerid.x', 'net', {'registry': registry, 'name': 'x', 'value': value, **extra})
        self.assert_refused([point('TLS Cipher Suites', '0x1301')], "registry 'TLS Cipher Suites' has no type")
        self.assert_refused([point('QUIC Versions', '1x5')], "value '1x5' is neither hexadecimal")
        self.assert_refused([point('QUIC Versions', '0x1FFFFFFFF')], 'does not fit u32')
        self.assert_refused([point('Cerid protocol identifiers', '0x10000')], 'does not fit u16')
        self.assert_refused([point('Cerid GREASE values', '0x?A?')], 'is not a 16-bit identifier')
        self.assert_refused([point('Cerid GREASE values', '0x0A0A')], 'is not 0x followed by hexadecimal digits and ?')
        self.assert_refused([point('TLS ALPN Protocol IDs', 'h 3')], 'is not a printable ALPN identifier')
        self.assert_refused([point('Cerid protocol identifiers', '0x0001', verified=False)],
                            'cannot be unverified')
        self.assert_refused([point('Cerid protocol identifiers', '0x0001'),
                             point('Cerid protocol identifiers', '0x0002')], 'constant X is also generated for cerid.x')
        twins = [self.entry('cerid.a', 'net', {'registry': 'Cerid protocol identifiers', 'name': 'a', 'value': '0x1'}),
                 self.entry('cerid.b', 'net', {'registry': 'Cerid protocol identifiers', 'name': 'b', 'value': '0x1'})]
        self.assert_refused(twins, 'two Cerid protocol identifiers share a value')
        reserved = [self.entry('cerid.a', 'net', {'registry': 'Cerid protocol identifiers', 'name': 'a',
                                                  'value': '0x1A1A'}),
                    self.entry('cerid.g', 'net', {'registry': 'Cerid GREASE values', 'name': 'g', 'value': '0x?A?A'})]
        self.assert_refused(reserved, 'lies in a reserved pattern')
        self.assert_refused([point('Object identifiers', '2.16.x')], "'2.16.x' is not a dotted object identifier")
        self.assert_refused([point('Object identifiers', '02.16')], "'02.16' is not a dotted object identifier")
        self.assert_refused([point('Cerid algorithm identifiers', '0x0901')], 'is not a class byte of ALGORITHM_CLASSES')
        self.assert_refused([point('Cerid algorithm identifiers', '0x0300')], 'followed by a nonzero number')
        self.assert_refused([point('Cerid algorithm identifiers', '0x0301', verified=False)], 'cannot be unverified')
        algorithms = [self.entry('crypto.a', 'net', {'registry': 'Cerid algorithm identifiers', 'name': 'a',
                                                     'value': '0x0101'}),
                      self.entry('crypto.b', 'net', {'registry': 'Cerid algorithm identifiers', 'name': 'b',
                                                     'value': '0x0101'})]
        self.assert_refused(algorithms, 'two Cerid algorithm identifiers share a value')
        audio = [self.entry('media.x', 'audio', {'registry': 'QUIC Versions', 'name': 'x', 'value': '0x1'})]
        self.assert_refused(audio, 'registered owners audio own codepoints and have no output',
                            modules=('core', 'net', 'audio'))


class AbBoard(unittest.TestCase):
    """scripts/ab-board.py: interleaved arms, paired speed ratios with a bootstrap interval, a decisive stop."""

    ROW = r'^(?P<key>\d+) (?P<value>[\d.]+)'

    def fake_runner(self, outputs, calls):
        """A runner whose command `name` prints outputs[name](call index); records each command line it ran."""
        def run(line, shell, capture_output, text):
            self.assertTrue(shell and capture_output and text)
            calls.append(line)
            name = line.split()[-1]
            return SimpleNamespace(returncode=0, stdout=outputs[name](len(calls)), stderr='')
        return run

    def arms(self, commands, *specs):
        return [ab_board.parse_arm(spec, commands) for spec in specs]

    def test_verdicts_and_ratios(self):
        self.assertEqual(ab_board.verdict(1.05, 1.08, 0.02), 'faster')
        self.assertEqual(ab_board.verdict(0.90, 0.97, 0.02), 'slower')
        self.assertEqual(ab_board.verdict(0.99, 1.01, 0.02), 'level')
        self.assertEqual(ab_board.verdict(0.99, 1.03, 0.02), 'unresolved')
        self.assertEqual(ab_board.verdict(None, None, 0.02), 'unresolved')
        # Above 1 the arm is faster: the reference's time over the arm's, or the arm's rate over the reference's.
        self.assertEqual(ab_board.speed_ratio(2.0, 1.0, False), 2.0)
        self.assertEqual(ab_board.speed_ratio(2.0, 1.0, True), 0.5)
        self.assertEqual([ab_board.number(v) for v in (None, 51634.2, 0.00057, 1.30449)],
                         ['-', '51634', '0.00057', '1.304'])

    def test_arguments_are_checked(self):
        commands = {'a': 'x'}
        with self.assertRaisesRegex(ab_board.BoardError, 'NAME:REGEX'):
            ab_board.parse_arm('arm=b:' + self.ROW, commands)
        with self.assertRaisesRegex(ab_board.BoardError, 'groups key and value'):
            ab_board.parse_arm(r'arm=a:^(\d+)', commands)
        with self.assertRaisesRegex(ab_board.BoardError, 'NAME=VALUE'):
            ab_board.parse_assignment('novalue', '--cmd')
        arms = self.arms(commands, 'ref=a:' + self.ROW)
        with self.assertRaisesRegex(ab_board.BoardError, 'at least one other arm'):
            ab_board.Board(commands, arms, 'ref')
        with self.assertRaisesRegex(ab_board.BoardError, 'is not an arm'):
            ab_board.Board(commands, arms, 'other')

    def test_instrument_failures_stop_the_board(self):
        regex = re.compile(self.ROW, re.M)
        with self.assertRaisesRegex(ab_board.BoardError, 'printed twice'):
            ab_board.arm_values('x', regex, '1 2.0\n1 3.0\n')
        with self.assertRaisesRegex(ab_board.BoardError, 'no line'):
            ab_board.arm_values('x', regex, 'nothing\n')
        commands = {'a': 'run a', 'b': 'run b'}
        arms = self.arms(commands, 'ref=a:' + self.ROW, 'cand=b:' + self.ROW)

        def failing(line, **_):
            return SimpleNamespace(returncode=3, stdout='', stderr='boom')
        board = ab_board.Board(commands, arms, 'ref', runner=failing, out=io.StringIO())
        with self.assertRaisesRegex(ab_board.BoardError, 'exited 3: boom'):
            board.run()

    def test_interleaves_pins_and_stops_once_decided(self):
        # The candidate is 25% faster at key 1 and level at key 2, with a little noise on both.
        commands = {'a': 'run a', 'b': 'run b'}
        noise = [1.0, 1.004, 0.997, 1.002, 0.999, 1.001, 0.998]
        outputs = {'a': lambda i: '1 %.4f\n2 %.4f\n' % (10.0 * noise[i % 7], 5.0 * noise[(i + 3) % 7]),
                   'b': lambda i: '1 %.4f\n2 %.4f\n' % (8.0 * noise[(i + 1) % 7], 5.0 * noise[(i + 5) % 7])}
        calls = []
        with tempfile.TemporaryDirectory() as scratch:
            board = ab_board.Board(commands, self.arms(commands, 'ref=a:' + self.ROW, 'cand=b:' + self.ROW), 'ref',
                                   cpus='4', warmup=1, min_reps=4, max_reps=12,
                                   runner=self.fake_runner(outputs, calls), log_dir=scratch, out=io.StringIO())
            document = board.run()
            self.assertTrue((Path(scratch) / 'warmup1-a.log').is_file())
            self.assertTrue((Path(scratch) / 'rep4-b.log').is_file())
        self.assertEqual(document['stop'], 'every cell decided after 4 repetitions')
        self.assertEqual(len(document['repetitions']), 4)
        self.assertEqual(len(calls), 10, 'one warm-up and four repetitions of both commands')
        self.assertTrue(all(line.startswith('taskset -c 4 run ') for line in calls))
        # Each repetition rotates the order: a b, b a, a b, ...
        self.assertEqual([line[-1] for line in calls[:4]], ['a', 'b', 'b', 'a'])
        cells = {cell['key']: cell for cell in document['comparisons']['cand']}
        self.assertEqual(cells['1']['verdict'], 'faster')
        self.assertAlmostEqual(cells['1']['ratio'], 1.25, delta=0.01)
        self.assertEqual(cells['2']['verdict'], 'level')
        table = ab_board.render(document)
        self.assertIn('| key | ref | cand | speed ratio | verdict |', table)
        self.assertIn('| faster |', table)

    def test_one_process_gives_two_arms_and_noise_runs_to_the_maximum(self):
        # A program that times Cerid and its peer in the same process (bench_fft_f32's planned row and MKL); the
        # peer's time swings by 10% each run, so the cell stays unresolved and the board runs to its maximum.
        commands = {'p': 'run p'}
        outputs = {'p': lambda i: '1 | 1.000 | %.3f\n' % (1.0 + (0.1 if i % 2 else -0.1))}
        calls = []
        arms = self.arms(commands, r'cerid=p:^(?P<key>\d+) \| (?P<value>[\d.]+) \|',
                         r'mkl=p:^(?P<key>\d+) \| [\d.]+ \| (?P<value>[\d.]+)')
        board = ab_board.Board(commands, arms, 'mkl', warmup=0, min_reps=2, max_reps=5,
                               runner=self.fake_runner(outputs, calls), out=io.StringIO())
        document = board.run()
        self.assertEqual(len(calls), 5)
        self.assertEqual(document['stop'], 'the maximum of 5 repetitions')
        self.assertEqual(document['comparisons']['cerid'][0]['verdict'], 'unresolved')

    def test_runs_real_processes_through_the_shell(self):
        python = '"%s"' % sys.executable
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as scratch:
            report = Path(scratch) / 'board.json'
            code = ab_board.main(['--cmd', 'a=%s -c "print(7, 2.0)"' % python,
                                  '--cmd', 'b=%s -c "print(7, 1.0)"' % python,
                                  '--arm', 'ref=a:' + self.ROW, '--arm', 'cand=b:' + self.ROW, '--reference', 'ref',
                                  '--warmup', '0', '--min-reps', '2', '--max-reps', '3', '--json', str(report)],
                                 out=out)
            self.assertEqual(code, 0)
            document = json.loads(report.read_text(encoding='utf-8'))
        self.assertEqual(document['schema'], 'cerid-ab-board/1')
        self.assertEqual(document['comparisons']['cand'][0]['ratio'], 2.0)
        self.assertIn('| 7 | 2 | 1 | 2 [2-2] | faster |', out.getvalue())

    def test_same_results_are_checked(self):
        commands = {'a': 'run a', 'b': 'run b'}
        bits = r'^(?P<key>\d+) [\d.]+ bits=(?P<value>[0-9a-f]+)'
        for spec in ('a:' + bits, 'a,z:' + bits, 'a,a:' + bits):
            with self.assertRaisesRegex(ab_board.BoardError, 'two or more distinct'):
                ab_board.parse_same(spec, commands)
        with self.assertRaisesRegex(ab_board.BoardError, 'groups key and value'):
            ab_board.parse_same(r'a,b:^(\d+)', commands)
        same = [ab_board.parse_same('a,b:' + bits, commands)]
        arms = self.arms(commands, 'ref=a:' + self.ROW, 'cand=b:' + self.ROW)
        # The candidate is faster and prints the reference's digest at every key: the board runs and records them.
        outputs = {'a': lambda i: '1 2.0 bits=ab\n2 4.0 bits=cd\n', 'b': lambda i: '1 1.0 bits=ab\n2 2.0 bits=cd\n'}
        board = ab_board.Board(commands, arms, 'ref', warmup=0, min_reps=2, max_reps=3, same=same,
                               runner=self.fake_runner(outputs, []), out=io.StringIO())
        document = board.run()
        self.assertEqual(document['same'][0]['values'], {'1': 'ab', '2': 'cd'})
        self.assertIn('a, b printed the same results', ab_board.render(document))
        # Another digest at a key, one that changes between repetitions, or a key one command lacks stops the board.
        failures = ((lambda i: '1 1.0 bits=ab\n2 2.0 bits=ce\n', 'printed ce at key 2 where a printed cd'),
                    (lambda i: '1 1.0 bits=ab\n2 2.0 bits=%s\n' % ('cd' if i < 3 else 'cf'), 'printed cf'),
                    (lambda i: '1 1.0 bits=ab\n', 'printed different keys'))
        for candidate, message in failures:
            board = ab_board.Board(commands, arms, 'ref', warmup=0, min_reps=2, max_reps=3, same=same,
                                   runner=self.fake_runner({'a': outputs['a'], 'b': candidate}, []),
                                   out=io.StringIO())
            with self.assertRaisesRegex(ab_board.BoardError, message):
                board.run()

class JsonSchemaSubset(unittest.TestCase):
    """scripts/json-schema-subset.py: the MCP conformance oracle's JSON Schema 2020-12 subset (MCP-ADAPT.0)."""

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('json_schema_subset', ROOT / 'scripts/json-schema-subset.py')
        cls.subset = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.subset)

    def test_types_follow_json_not_python(self):
        v = self.subset.Validator({})
        self.assertTrue(v.valid(1.0, {'type': 'integer'}))  # 2020-12: a zero fraction is an integer
        self.assertFalse(v.valid(1.5, {'type': 'integer'}))
        self.assertFalse(v.valid(True, {'type': 'integer'}))  # a boolean is never a number
        self.assertFalse(v.valid(True, {'const': 1}))
        self.assertFalse(v.valid(0, {'enum': [False]}))
        self.assertTrue(v.valid(1, {'const': 1.0}))
        self.assertTrue(v.valid('x', {'type': ['integer', 'string']}))
        self.assertFalse(v.valid(None, {'type': ['integer', 'string']}))
        self.assertTrue(v.valid({'a': [1, {'b': None}]}, {'const': {'a': [1, {'b': None}]}}))
        self.assertFalse(v.valid({'a': [1]}, {'const': {'a': [True]}}))

    def test_objects_arrays_and_bounds(self):
        v = self.subset.Validator({})
        schema = {'type': 'object', 'properties': {'n': {'type': 'integer', 'minimum': 0, 'maximum': 9}},
                  'required': ['n'], 'additionalProperties': False}
        self.assertEqual([], v.errors({'n': 3}, schema))
        self.assertIn("$: the member 'n' is required", v.errors({}, schema))
        self.assertEqual(['$.n: -1 is below the minimum 0'], v.errors({'n': -1}, schema))
        self.assertEqual(['$.x: no value is allowed here'], v.errors({'n': 1, 'x': 0}, schema))
        self.assertTrue(v.valid({'a': 'b'}, {'type': 'object', 'additionalProperties': {'type': 'string'}}))
        self.assertFalse(v.valid({'a': 1}, {'type': 'object', 'additionalProperties': {'type': 'string'}}))
        self.assertFalse(v.valid([1, 'x'], {'type': 'array', 'items': {'type': 'integer'}}))
        self.assertFalse(v.valid([1, 2], {'maxItems': 1}))
        self.assertTrue(v.valid('éé', {'maxLength': 2}))  # characters, not UTF-8 bytes
        self.assertFalse(v.valid('', {'minLength': 1}))

    def test_composition(self):
        v = self.subset.Validator({})
        any_of = {'anyOf': [{'type': 'string'}, {'type': 'integer'}]}
        self.assertTrue(v.valid(3, any_of))
        self.assertFalse(v.valid(None, any_of))
        self.assertFalse(v.valid(5, {'allOf': [{'minimum': 0}, {'maximum': 4}]}))
        self.assertFalse(v.valid(3, {'oneOf': [{'type': 'integer'}, {'minimum': 0}]}))  # both match
        self.assertTrue(v.valid('x', {'not': {'type': 'integer'}}))

    def test_references_resolve_inside_the_document_only(self):
        document = {'$defs': {'Id': {'type': ['string', 'integer']},
                              'Message': {'type': 'object', 'properties': {'id': {'$ref': '#/$defs/Id'}},
                                          'required': ['id']}}}
        v = self.subset.Validator(document)
        self.assertEqual([], v.ref_errors({'id': 'a'}, '#/$defs/Message'))
        self.assertEqual(['$.id: null is not of type string/integer'], v.ref_errors({'id': None}, '#/$defs/Message'))
        with self.assertRaises(self.subset.SchemaError):
            v.ref_errors({}, '#/$defs/Missing')
        with self.assertRaises(self.subset.SchemaError):
            self.subset.Validator({'$ref': 'https://example.com/schema.json'})
        loop = self.subset.Validator({'$defs': {'A': {'$ref': '#/$defs/B'}, 'B': {'$ref': '#/$defs/A'}}})
        with self.assertRaises(self.subset.SchemaError):
            loop.ref_errors(1, '#/$defs/A')

    def test_an_unknown_keyword_refuses_the_schema(self):
        for schema in ({'type': 'object', 'patternProperties': {}}, {'unevaluatedProperties': False},
                       {'properties': {'a': {'dependentRequired': {}}}}, {'type': 'decimal'}, {'anyOf': []},
                       {'required': [1]}, {'minLength': -1}):
            with self.subTest(schema=schema), self.assertRaises(self.subset.SchemaError):
                self.subset.check_schema(schema)
        self.subset.check_schema({'type': 'string', 'format': 'uri', 'description': 'an annotation'})

    def test_the_command_line(self):
        with tempfile.TemporaryDirectory() as scratch:
            schema = Path(scratch, 's.json')
            schema.write_text(json.dumps({'$defs': {'N': {'type': 'integer'}}}), encoding='utf-8')
            good, bad = Path(scratch, 'good.json'), Path(scratch, 'bad.json')
            good.write_text('3', encoding='utf-8')
            bad.write_text('"3"', encoding='utf-8')
            with patch('sys.stdout', new_callable=io.StringIO):
                self.assertEqual(0, self.subset.main([str(schema), str(good), '--ref', '#/$defs/N']))
                self.assertEqual(1, self.subset.main([str(schema), str(bad), '--ref', '#/$defs/N']))
            with patch('sys.stderr', new_callable=io.StringIO) as err:
                self.assertEqual(2, self.subset.main([str(schema), str(Path(scratch, 'none.json'))]))
            self.assertIn('json-schema-subset:', err.getvalue())

    def test_the_pinned_mcp_schemas_match_their_fetch_records_and_load(self):
        spec = importlib.util.spec_from_file_location('mcp_conformance', ROOT / 'tests/tools/ceridc/mcp_conformance.py')
        conformance = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(conformance)
        for revision in ('2026-07-28', '2025-11-25'):
            validator = conformance.load_schema(revision)
            self.assertIn('CallToolResult', validator.document['$defs'])
        modern = conformance.load_schema('2026-07-28')
        reply = {'jsonrpc': '2.0', 'id': 1, 'error': {'code': -32022, 'message': 'Unsupported protocol version',
                                                      'data': {'supported': ['2026-07-28'], 'requested': 'x'}}}
        self.assertEqual([], modern.ref_errors(reply, '#/$defs/UnsupportedProtocolVersionError'))
        reply['id'] = None  # MCP leaves an id it cannot read out; null is not a request id
        self.assertTrue(modern.ref_errors(reply, '#/$defs/UnsupportedProtocolVersionError'))


class McpInspector(unittest.TestCase):
    """tests/tools/ceridc/mcp_inspector.py: the pinned MCP Inspector's lock, its recording relay and the transcript
    check that holds every recorded message to the pinned schemas (MCP-ADAPT.0)."""

    TOOL = {'name': 'echo', 'inputSchema': {'type': 'object'},
            'outputSchema': {'type': 'object', 'properties': {'ok': {'type': 'boolean'}}, 'required': ['ok'],
                             'additionalProperties': False}}

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('mcp_inspector', ROOT / 'tests/tools/ceridc/mcp_inspector.py')
        cls.inspector = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.inspector)
        conformance = cls.inspector.conformance
        cls.schemas = {conformance.MODERN: conformance.load_schema(conformance.MODERN),
                       conformance.LEGACY: conformance.load_schema(conformance.LEGACY)}

    def connection(self, modern, report=None):
        """A recorded connection of `modern` or legacy era: discovery or the handshake, tools/list, one call."""
        meta = {'_meta': self.inspector.conformance.META} if modern else {}
        complete = {'resultType': 'complete'} if modern else {}
        cached = dict(complete, ttlMs=1, cacheScope='public') if modern else {}
        report = {'ok': True} if report is None else report
        call = {'content': [{'type': 'text', 'text': json.dumps(report)}], 'structuredContent': report,
                'isError': report.get('ok') is not True}
        if modern:
            opening = [('client', {'jsonrpc': '2.0', 'id': 0, 'method': 'server/discover', 'params': dict(meta)}),
                       ('server', {'jsonrpc': '2.0', 'id': 0, 'result': dict(
                           cached, supportedVersions=['2026-07-28', '2025-11-25'],
                           capabilities={'tools': {'listChanged': False}}, serverInfo={'name': 's', 'version': '1'})})]
        else:
            opening = [('client', {'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': {
                           'protocolVersion': '2025-11-25', 'capabilities': {},
                           'clientInfo': {'name': 'c', 'version': '1'}}}),
                       ('server', {'jsonrpc': '2.0', 'id': 0, 'result': {
                           'protocolVersion': '2025-11-25', 'capabilities': {'tools': {'listChanged': False}},
                           'serverInfo': {'name': 's', 'version': '1'}}}),
                       ('client', {'jsonrpc': '2.0', 'method': 'notifications/initialized'})]
        return opening + [
            ('client', {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/list', 'params': dict(meta)}),
            ('server', {'jsonrpc': '2.0', 'id': 1, 'result': dict(cached, tools=[self.TOOL])}),
            ('client', {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call',
                        'params': dict(meta, name='echo', arguments={})}),
            ('server', {'jsonrpc': '2.0', 'id': 2, 'result': dict(complete, **call)})]

    def check(self, entries, era, raw=None):
        """The Transcript of `entries` (or of `raw` lines, verbatim) recorded as the relay writes them."""
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp, 'transcript.jsonl')
            lines = raw or [(source, json.dumps(message).encode('utf-8') + b'\n') for source, message in entries]
            log.write_text(''.join(json.dumps({'source': source, 'data': base64.b64encode(data).decode('ascii')}) +
                                   '\n' for source, data in lines) + '{"exit": 0}\n', encoding='utf-8')
            return self.inspector.Transcript(log, self.schemas, {}, era)

    def test_a_valid_connection_of_each_era_passes(self):
        legacy = self.check(self.connection(False), 'legacy')
        self.assertEqual((legacy.methods, legacy.revisions, legacy.messages, legacy.exit),
                         (['initialize', 'tools/list', 'tools/call'], {'2025-11-25'}, 7, 0))
        for era in ('modern', 'auto'):
            modern = self.check(self.connection(True), era)
            self.assertEqual((modern.methods, modern.revisions, modern.messages),
                             (['server/discover', 'tools/list', 'tools/call'], {'2026-07-28'}, 6))

    def test_every_departure_fails_the_run(self):
        failure = self.inspector.Failure
        valid = self.connection(True)
        cases = {
            'the wrong era': (self.connection(False), 'modern', 'was sent in revision 2025-11-25'),
            'a modern request in the legacy era': (valid, 'legacy', 'was sent in revision 2026-07-28'),
            'no resultType': ([(s, {k: v for k, v in m.items()} if 'result' not in m else
                                dict(m, result={k: v for k, v in m['result'].items() if k != 'resultType'}))
                               for s, m in valid], 'modern', "'resultType' is required"),
            'an unknown method': ([valid[0], ('client', {'jsonrpc': '2.0', 'id': 7, 'method': 'resources/list',
                                                         'params': valid[0][1]['params']})], 'modern',
                                  'a method this run does not know'),
            'an unknown notification': ([('client', {'jsonrpc': '2.0', 'method': 'notifications/progress'})],
                                        'legacy', 'which this run does not know'),
            'a reply to no request': ([valid[1]], 'modern', 'a reply to no request'),
            'a request without its reply': (valid[:1], 'modern', 'requests without a reply'),
            'a report outside the output schema': (self.connection(True, {'ok': True, 'extra': 1}), 'modern',
                                                   'does not fit its outputSchema'),
            'a call before its tool was listed': (valid[:2] + valid[4:], 'modern', 'before its output schema'),
            'a text other than the report': ([*valid[:5], ('server', dict(valid[5][1], result=dict(
                valid[5][1]['result'], content=[{'type': 'text', 'text': '{}'}])))], 'modern',
                                             'the text content is the structured result'),
            'two content blocks': ([*valid[:5], ('server', dict(valid[5][1], result=dict(
                valid[5][1]['result'], content=valid[5][1]['result']['content'] * 2)))], 'modern',
                                   'one text content block'),
            'isError against the report': ([*valid[:5], ('server', dict(valid[5][1], result=dict(
                valid[5][1]['result'], isError=True)))], 'modern', 'isError follows'),
        }
        for name, (entries, era, message) in cases.items():
            with self.subTest(name), self.assertRaisesRegex(failure, message):
                self.check(entries, era)
        lines = [(source, json.dumps(message).encode('utf-8') + b'\n') for source, message in valid]
        for name, data in (('a CRLF line', lines[1][1][:-1] + b'\r\n'), ('a line without its LF', lines[1][1][:-1])):
            with self.subTest(name), self.assertRaisesRegex(failure, 'ending in LF alone'):
                self.check(None, 'modern', [lines[0], ('server', data)])

    def test_the_relay_copies_every_line_unchanged_and_records_it(self):
        server = 'import sys\nfor line in sys.stdin.buffer:\n    sys.stdout.buffer.write(line.upper())\nsys.exit(3)\n'
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp, 'relay.jsonl')
            done = subprocess.run([sys.executable, str(ROOT / 'tests/tools/ceridc/mcp_inspector.py'), 'relay', '--log',
                                   str(log), '--', sys.executable, '-c', server], input=b'a\r\nb\n\x1ac',
                                  capture_output=True, timeout=60)
            self.assertEqual((done.returncode, done.stdout), (3, b'A\r\nB\n\x1aC'))
            entries = [json.loads(line) for line in log.read_text(encoding='utf-8').splitlines()]
        self.assertEqual(entries[-1], {'exit': 3})
        recorded = {source: [base64.b64decode(e['data']) for e in entries if e.get('source') == source]
                    for source in ('client', 'server')}
        self.assertEqual(recorded, {'client': [b'a\r\n', b'b\n', b'\x1ac'], 'server': [b'A\r\n', b'B\n', b'\x1aC']})

    def test_the_inspector_is_locked_with_every_package_integrity(self):
        package = ROOT / 'tests/tools/ceridc/mcp/inspector'
        manifest = json.loads((package / 'package.json').read_text(encoding='utf-8'))
        lock = json.loads((package / 'package-lock.json').read_text(encoding='utf-8'))
        version = manifest['devDependencies'][self.inspector.INSPECTOR]
        self.assertRegex(version, r'^\d+\.\d+\.\d+$')
        self.assertEqual(version, self.inspector.pinned_version())
        self.assertEqual(lock['lockfileVersion'], 3)
        self.assertEqual(lock['packages']['']['devDependencies'], manifest['devDependencies'])
        self.assertEqual(lock['packages']['node_modules/' + self.inspector.INSPECTOR]['version'], version)
        for name, entry in lock['packages'].items():
            if name:
                self.assertTrue(entry.get('integrity', '').startswith('sha512-'), name)
                self.assertTrue(entry['resolved'].startswith('https://registry.npmjs.org/'), name)
        self.assertEqual(self.inspector.launcher(Path('x')).as_posix(),
                         'x/node_modules/@modelcontextprotocol/inspector/clients/launcher/build/index.js')


if __name__ == '__main__':
    unittest.main()
