#!/usr/bin/env python3
"""Regression tests of the documentation drift guard (scripts/check-docs.py).

Each check is exercised on a small synthetic tree: one case that must fail (the drift the check exists for) and one
that must pass, so a check that stops seeing its drift class fails here. The last case runs every check on this
repository, which must be clean.

  python scripts/test-check-docs.py
"""
from pathlib import Path
import importlib.util
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_docs', ROOT / 'scripts' / 'check-docs.py')
docs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(docs)

BANNER = '<!-- doc-role: navigation -->\n> Navigation.\n\n'

CPU_HEADER = '''#pragma once
enum class CpuLevel : crd::u8
{
    Portable, // none
    Sse2,     // v1
    Sse42,    // v2
    Avx2,     // v3
    Avx512,   // v4
    Neon,     // arm
};
'''

CPU_SOURCE = '''const char* cpu_level_name(CpuLevel level) noexcept
{
    switch (level)
    {
        case CpuLevel::Portable:
            return "portable";
        case CpuLevel::Sse2:
            return "sse2";
        case CpuLevel::Sse42:
            return "sse4.2";
        case CpuLevel::Avx2:
            return "avx2";
        case CpuLevel::Avx512:
            return "avx512";
        case CpuLevel::Neon:
            return "neon";
    }
    return "?";
}
'''

CRD_SIMD = '''set(_CRD_KERNEL_LEVEL_portable 0)
set(_CRD_KERNEL_LEVEL_sse2 1)
set(_CRD_KERNEL_LEVEL_sse42 2)
set(_CRD_KERNEL_LEVEL_avx2 3)
set(_CRD_KERNEL_LEVEL_avx512 4)
set(_CRD_KERNEL_LEVEL_neon 5)
option(CRD_REAL_OPTION "a real option" ON)
'''

DETERMINISM = '''enum class DeterminismMode : crd::u8
{
    Default = 0,
    CrossPlatform = 1,
    BackwardCompat = 2
};
'''

ROOT_CMAKE = '''cmake_minimum_required(VERSION 3.25)
crd_module(engine/foundation/core engine/core TESTS foundation/core)
crd_module(engine/foundation/math engine/math DEPENDS core TESTS foundation/math support/diag)
crd_module(tools/ceridc HOST EXECUTABLES ceridc DEPENDS core)
crd_module(runtime DEPENDS core)
add_test(NAME crd-check-real-guard COMMAND x)
'''

PRESETS = '''{
  "version": 6,
  "configurePresets": [
    {"name": "win-debug"},
    {"name": "linux-gcc-debug"},
    {"name": "win-vs", "cacheVariables": {"CMAKE_CONFIGURATION_TYPES": "Debug;Release;Shipping"}}
  ],
  "testPresets": [{"name": "win-debug-at-sse2", "configurePreset": "win-debug"}]
}
'''

WORKFLOW = '''name: CI
jobs:
  linux-gcc:
    runs-on: ubuntu
  windows:
    runs-on: windows
'''

LICENSE = '''Example License

Copyright (c) 2026 Example Owner. All rights reserved.
'''


class Tree:
    """A minimal repository: the facts' sources, the master-plan validator and the documents a test adds."""

    def __init__(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / 'scripts').mkdir()
        shutil.copy(ROOT / 'scripts' / 'check-master-plan.py', self.root / 'scripts' / 'check-master-plan.py')
        self.write('scripts/tool-one.py', 'print(1)\n')
        self.write('engine/foundation/core/include/crd/core/cpu_features.hpp', CPU_HEADER)
        self.write('engine/foundation/core/src/cpu_features.cpp', CPU_SOURCE)
        self.write('engine/foundation/core/CMakeLists.txt', '# core\n')
        self.write('engine/foundation/math/include/crd/math/determinism_mode.hpp', DETERMINISM)
        self.write('cmake/CrdSimd.cmake', CRD_SIMD)
        self.write('cmake/pins.json', '{"schema": "x", "tools": {"sccache-windows": {}}}\n')
        self.write('CMakeLists.txt', ROOT_CMAKE)
        self.write('CMakePresets.json', PRESETS)
        self.write('LICENSE', LICENSE)
        self.write('.github/workflows/ci.yml', WORKFLOW)
        self.write('runtime/CMakeLists.txt', 'add_executable(cpu_level_record examples/cpu_level_record.cpp)\n')
        self.write('docs/decisions/README.md', BANNER + '| Reference | Purpose |\n|---|---|\n')

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode('utf-8'))
        return name

    def read(self, name):
        return (self.root / name).read_text(encoding='utf-8')

    def close(self):
        self.temp.cleanup()


class TreeCase(unittest.TestCase):
    def setUp(self):
        self.tree = Tree()
        self.root = self.tree.root

    def tearDown(self):
        self.tree.close()


class Classification(TreeCase):
    def test_entry_points_and_contracts_are_living_dated_records_are_not(self):
        for name in ('README.md', 'AGENTS.md', 'docs/BUILDING.md', 'docs/design/ci-tiers.md',
                     'docs/design/ceir-14-render-dialect.md', 'docs/sessions/2026-10-31-x.md',
                     'docs/bench/2026-10-31-board.md', 'docs/systems/core.md', 'docs/hints/old.md'):
            self.tree.write(name, BANNER + 'text\n')
        self.tree.write('docs/hints/old.md', '<!-- doc-role: historical -->\n> Old.\n')
        living = docs.living_documents(self.root)
        for name in ('README.md', 'AGENTS.md', 'docs/BUILDING.md', 'docs/design/ci-tiers.md', 'docs/systems/core.md'):
            self.assertIn(name, living)
        for name in ('docs/design/ceir-14-render-dialect.md', 'docs/sessions/2026-10-31-x.md',
                     'docs/bench/2026-10-31-board.md', 'docs/hints/old.md'):
            self.assertNotIn(name, living)


class Links(TreeCase):
    def test_broken_link_and_anchor_outside_docs_fail(self):
        self.tree.write('scripts/README.md', '# Scripts\n\n## Usage\n\n[ok](tool-one.py) [anchor](#usage)\n')
        self.assertEqual(docs.link_errors(self.root, ['scripts/README.md']), [])
        self.tree.write('scripts/README.md', '# Scripts\n\n[gone](tool-two.py) [anchor](#missing)\n')
        errors = docs.link_errors(self.root, ['scripts/README.md'])
        self.assertEqual(len(errors), 2, errors)
        self.assertIn('missing path tool-two.py', errors[0])
        self.assertIn('missing anchor #missing', errors[1])


class References(TreeCase):
    def check(self, body):
        name = self.tree.write('docs/BUILDING.md', BANNER + body)
        return docs.reference_errors(self.root, [name])

    def test_existing_paths_scripts_presets_and_options_pass(self):
        self.assertEqual(self.check(
            'Run `python scripts/tool-one.py` and `tool-one.py`, see `cmake/CrdSimd.cmake` and engine/foundation/core/'
            'CMakeLists.txt.\n'
            '`./scripts/configure-preset.bat win-debug`, `ctest --preset win-debug-at-sse2`, the `linux-gcc` job,\n'
            'the registry entry `tools/sccache-windows`, the program `runtime/cpu_level_record`,\n'
            '`-DCRD_REAL_OPTION=OFF`, the guard `crd-check-real-guard`.\n'), [])

    def test_missing_path_script_preset_option_and_guard_fail(self):
        errors = self.check(
            'Run `scripts/gone.py` and `vanished.ps1`; read cmake/Missing.cmake.\n'
            '`cmake --preset win-imaginary`; `-DCRD_NO_SUCH_OPTION=ON`; `crd-check-imaginary`.\n')
        joined = '\n'.join(errors)
        for needle in ('missing path scripts/gone.py', 'missing script vanished.ps1', 'missing path cmake/Missing.cmake',
                       'missing preset win-imaginary', 'CRD_NO_SUCH_OPTION', 'crd-check-imaginary'):
            self.assertIn(needle, joined)

    def test_a_tool_in_the_untracked_build_tree_fails(self):
        errors = self.check('Apply it with `python build/scratch/apply_order.py --check`.\n')
        self.assertTrue(any('untracked build tree' in error for error in errors), errors)
        self.assertEqual(self.check('Write `build/my-task/operations.json`, then configure `build/win-debug`.\n'), [])

    def test_history_lines_and_placeholders_are_not_held_to_the_tree(self):
        self.assertEqual(self.check(
            'The former `scripts/deleted.py` was removed in 2026.\n'
            'Write `docs/sessions/YYYY-MM-DD-topic.md`; sources live in `engine/<family>/<module>/`.\n'
            'In prose a slash means or: engine/application work.\n'), [])

    def test_fenced_commands_are_checked_for_scripts_and_presets(self):
        errors = self.check('```powershell\n./scripts/configure-preset.bat win-gone\npython scripts/nope.py\n```\n')
        self.assertTrue(any('win-gone' in error for error in errors), errors)
        self.assertTrue(any('nope.py' in error for error in errors), errors)


class Facts(TreeCase):
    def test_fact_values_come_from_their_sources(self):
        values = docs.fact_values(self.root)
        self.assertEqual(values['cpu-levels'], '`portable`, `sse2`, `sse4.2`, `avx2`, `avx512` and `neon`')
        self.assertEqual(values['determinism-modes'], '`Default`, `CrossPlatform` and `BackwardCompat`')
        self.assertEqual(values['module-count'], '4')
        self.assertEqual(values['engine-module-count'], '2')
        self.assertEqual(values['other-module-count'], '2')
        self.assertEqual(values['host-tool-count'], '1')
        self.assertEqual(values['test-directory-count'], '3')
        self.assertEqual(values['native-profile-count'], '3')
        self.assertEqual(values['copyright'], 'Copyright (c) 2026 Example Owner. All rights reserved.')

    def test_stale_fact_fails_and_refresh_rewrites_it(self):
        name = self.tree.write('README.md', BANNER + 'Levels: <!-- fact:cpu-levels -->`sse2`<!-- /fact -->.\n')
        errors = docs.fact_errors(self.root, [name])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn('fact cpu-levels', errors[0])
        self.assertEqual(docs.fact_errors(self.root, [name], refresh=True), [])
        self.assertIn('`portable`, `sse2`', self.tree.read(name))
        self.assertEqual(docs.fact_errors(self.root, [name]), [])

    def test_unknown_fact_fails(self):
        name = self.tree.write('README.md', BANNER + '<!-- fact:no-such-fact -->1<!-- /fact -->\n')
        self.assertIn('unknown fact block no-such-fact', '\n'.join(docs.fact_errors(self.root, [name])))

    def test_level_sources_that_disagree_fail(self):
        self.tree.write('cmake/CrdSimd.cmake', CRD_SIMD.replace('_CRD_KERNEL_LEVEL_avx2 3', '_CRD_KERNEL_LEVEL_avx2 4'))
        self.assertTrue(any('CPU level sources disagree' in error for error in docs.fact_errors(self.root, [])))

    def test_hand_written_counts_must_match_their_fact(self):
        name = self.tree.write('docs/design/module-registry.md', BANNER + 'The build has 96 engine modules and all '
                                                                  'eight native profiles.\n')
        errors = docs.fact_errors(self.root, [name])
        self.assertEqual(len(errors), 2, errors)
        self.tree.write(name, BANNER + 'The build has 2 engine modules and three native profiles.\n')
        self.assertEqual(docs.fact_errors(self.root, [name]), [])


class Indexes(TreeCase):
    def test_unindexed_and_unlinked_documents_fail(self):
        self.tree.write('docs/README.md', BANNER + '[design](design/README.md)\n')
        self.tree.write('docs/design/README.md', BANNER + '| [A](a.md) | a |\n')
        self.tree.write('docs/design/a.md', BANNER + 'a\n')
        self.tree.write('docs/design/b.md', BANNER + 'b\n')
        self.tree.write('docs/hints/orphan.md', BANNER + 'orphan\n')
        self.tree.write('docs/sessions/2026-10-31-x.md', BANNER + 'sessions are reached from rows\n')
        files = docs.markdown_files(self.root)
        errors = docs.index_errors(self.root, files)
        joined = '\n'.join(errors)
        self.assertIn('docs/design/README.md does not index b.md', joined)
        self.assertIn('docs/hints/orphan.md: no document links to it', joined)
        self.assertNotIn('sessions', joined)
        self.assertNotIn('a.md', joined)

    def test_a_named_board_counts_as_indexed(self):
        self.tree.write('docs/README.md', BANNER + '[bench](bench/README.md) [ADRs](decisions/README.md)\n')
        self.tree.write('docs/bench/README.md', BANNER + '- **2026-10-31-board.md** - verdict\n')
        self.tree.write('docs/bench/2026-10-31-board.md', BANNER + 'board\n')
        self.assertEqual(docs.index_errors(self.root, docs.markdown_files(self.root)), [])


class Decisions(TreeCase):
    def adr(self, number, slug, body):
        return self.tree.write(f'docs/decisions/{number}-{slug}.md',
                               f'# ADR-{number}\n\n<!-- doc-role: decision -->\n> Decision.\n\n{body}\n\n## Context\n')

    def index(self, rows):
        self.tree.write('docs/decisions/README.md', BANNER + '| Reference | Purpose |\n|---|---|\n' + rows)

    def test_non_reciprocal_supersession_fails_and_a_note_back_fixes_it(self):
        self.adr('0001', 'old', '**Status:** Superseded by ADR-0002.')
        self.adr('0002', 'new', '**Status:** Accepted.')
        self.index('| [0001-old](0001-old.md) | old; superseded by ADR-0002 |\n| [0002-new](0002-new.md) | new |\n')
        errors = docs.adr_errors(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn('ADR-0001 states a supersession with ADR-0002', errors[0])
        self.adr('0002', 'new', '**Status:** Accepted. **Supersedes:** ADR-0001.')
        self.assertEqual(docs.adr_errors(self.root), [])

    def test_index_must_name_each_record_once_and_show_its_supersession(self):
        self.adr('0001', 'old', '**Status:** Superseded by ADR-0002.')
        self.adr('0002', 'new', '**Supersedes:** ADR-0001.')
        self.adr('0003', 'other', '**Status:** Accepted; does not supersede ADR-0001.')
        self.index('| [0001-old](0001-old.md) | old |\n| [0002-new](0002-new.md) | new |\n'
                   '| [0002-new](0002-new.md) | again |\n')
        joined = '\n'.join(docs.adr_errors(self.root))
        self.assertIn('the row of ADR-0001 omits the supersession', joined)
        self.assertIn('links 0002-new.md 2 times', joined)
        self.assertIn('links 0003-other.md 0 times', joined)
        self.assertNotIn('ADR-0003 states', joined)

    def test_duplicate_numbers_fail(self):
        self.adr('0004', 'one', '**Status:** Accepted.')
        self.adr('0004', 'two', '**Status:** Accepted.')
        self.index('| [0004-one](0004-one.md) | a |\n| [0004-two](0004-two.md) | b |\n')
        self.assertIn('ADR number 0004 used by', '\n'.join(docs.adr_errors(self.root)))


class Phrases(TreeCase):
    def check(self, body, name='docs/BUILDING.md'):
        self.tree.write(name, BANNER + body)
        return docs.phrase_errors(self.root, [name])

    def test_retired_rules_fail_outside_their_owner(self):
        self.assertTrue(self.check('Agents never commit or push.\n'))
        self.assertTrue(self.check('Work serially, no parallel agents.\n', 'context.md'))
        self.assertTrue(self.check('CI runs on Buildkite agents.\n'))
        self.assertTrue(self.check('Coding conventions live in CLAUDE.md.\n'))
        self.assertTrue(self.check('- **Never run `git commit` or `git push`.** The user commits.\n'))
        self.assertTrue(self.check("- Build commands → that's `CLAUDE.md` / `AGENTS.md`.\n"))
        self.assertEqual(self.check('CLAUDE.md lives only in the agent workspace overlay.\n'), [])
        self.assertTrue(self.check('Entire renderer → crd-ui and CR-D007 → hesap GPU and the notebook.\n'))
        self.assertEqual(self.check('Any other agent proposes the message and the user commits.\n', 'AGENTS.md'), [])
        self.assertEqual(self.check('Buildkite was the rejected alternative.\n'), [])

    def test_emoji_in_prose_fails_but_captured_output_is_kept(self):
        self.assertTrue(self.check('Status ✅ done.\n'))
        self.assertEqual(self.check('```\n1.23x ✓\n```\n'), [])

    def test_todo_needs_an_owning_row(self):
        self.assertTrue(self.check('TODO: write this section.\n'))
        self.assertEqual(self.check('TBD in [ROWX](ROADMAP.md#slice-rowx).\n'), [])
        self.assertEqual(self.check('The generator emits a `TODO` scaffold; the note said "owner TBD".\n'), [])


class Repository(unittest.TestCase):
    def test_this_repository_is_clean(self):
        count, living, errors = docs.check(ROOT)
        self.assertGreater(count, 1000)
        self.assertGreater(living, 50)
        self.assertEqual(errors, [])


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    unittest.main()
