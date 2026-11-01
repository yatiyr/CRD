#!/usr/bin/env python3
"""Generate hesap-fft's per-level forward plans from the saved fft level boards (ADR-0134 §7).

A board (`bench_fft_f32` and `bench_fft_vs_refs` at each CPU level in passes, scripts/board_fft_level_plans.sh) times
every forward the planner offers at each size of a type; each type's data file keeps every arm's time per pass under
the fft kernel level each log names (the level FftPlan keys on: a selection with no fft unit of its own, such as
sse4.2, runs the one below). Each level keeps the board that measured it: the machine, the binary's commit, the
board's record and the canonical planner's forward of each size as that binary printed it (`n=<n> canonical: <arm>`),
the arm the level's notes are compared with. An import replaces the levels its board ran and keeps the others, so one
machine's level (the benchmark machine's avx512) joins another's (the i9's portable, sse2 and avx2). A level's plan of
a size is the board's fastest arm there (the lowest median of its passes) when every pass of it beat every pass of the
arm the level ran as planned during the board (the ranges are the board's run-to-run noise), and the planned arm
otherwise. The header lists every measured type, level and size; FftPlan runs a cell under DeterminismMode::Default
and the canonical planner everywhere else and under the other modes.

  python scripts/gen_fft_level_plans.py                      # write the header from the data files
  python scripts/gen_fft_level_plans.py --check              # fail when the header differs from the data files'
  python scripts/gen_fft_level_plans.py --import DIR --type f32|f64 --commit TEXT --machine TEXT --board LINK
                                                             # replace the type's levels the board directory ran:
                                                             # pass<k>-<type>-<label>.log, pass<k>-level-<label>.txt
"""
from pathlib import Path
import argparse
import glob
import itertools
import json
import re
import statistics
import sys

ROOT = Path(__file__).resolve().parents[1]
# Each element type: its data file, the board program that times its arms and its element size.
TYPES = {
    'f32': (ROOT / 'docs' / 'bench' / '2026-10-30-simd-dispatch-fft-f32-arms.json', 'bench_fft_f32', 4),
    'f64': (ROOT / 'docs' / 'bench' / '2026-10-30-simd-dispatch-fft-f64-arms.json', 'bench_fft_vs_refs', 8),
}
HEADER = ROOT / 'engine' / 'numerics' / 'hesap-fft' / 'include' / 'crd' / 'hesap' / 'fft' / 'detail' / \
    'level_plans_gen.hpp'
KERNELS = ROOT / 'engine' / 'numerics' / 'hesap-fft' / 'include' / 'crd' / 'hesap' / 'fft' / 'fft_kernels.hpp'
SCHEMA = 'cerid-fft-level-board/2'
# What each level entry records of the board that measured it.
PROVENANCE = ('machine', 'commit', 'board')
# The CRD_CPU_LEVEL spellings and their crd::CpuLevel enumerators, in enumeration order.
LEVELS = {'portable': 'Portable', 'sse2': 'Sse2', 'sse4.2': 'Sse42', 'avx2': 'Avx2', 'avx512': 'Avx512',
          'neon': 'Neon'}
ROW = re.compile(r'^(\d+)\s+\|\s+(\S+)\s+\|\s+([\d.]+)\s+\|')
KERNEL_LEVEL = re.compile(r'^fft kernel level: (\S+)$', re.M)
CANONICAL = re.compile(r'^n=(\d+) canonical: (\S+)$')


def algorithms():
    """fft_algorithm_name's spellings and their FftAlgorithm enumerators (fft_kernels.hpp)."""
    text = KERNELS.read_text(encoding='utf-8')
    pairs = re.findall(r'case FftAlgorithm::(\w+):\s*return "([\w-]+)";', text)
    if not pairs:
        raise SystemExit(f'{KERNELS}: no fft_algorithm_name cases')
    return {name: enumerator for enumerator, name in pairs}


def import_board(directory, kind, provenance):
    """The level entries a board directory's logs measured, one per fft kernel level they ran (a CPU selection without
    an fft unit of its own runs the one below it, so its passes join that level's), each with the board's provenance
    (machine, commit, board) and the canonical forward of each size its program printed."""
    by_level = {}
    canonical = {}
    for log in sorted(glob.glob(str(Path(directory) / f'pass1-{kind}-*.log'))):
        label = Path(log).name[len(f'pass1-{kind}-'):-len('.log')]
        lines = sorted(glob.glob(str(Path(directory) / f'pass*-level-{label}.txt')))
        if not lines:  # the pre-row programs: no level record, not this engine's arms
            continue
        selection = Path(lines[0]).read_text(encoding='utf-8').strip()
        for path in sorted(glob.glob(str(Path(directory) / f'pass*-{kind}-{label}.log'))):
            text = Path(path).read_text(encoding='utf-8')
            match = KERNEL_LEVEL.search(text)
            if match is None:
                raise SystemExit(f'{path}: no "fft kernel level:" line')
            entry = by_level.setdefault(match.group(1), {'level': match.group(1), **provenance, 'selections': [],
                                                         'sizes': {}})
            if selection not in entry['selections']:
                entry['selections'].append(selection)
            seen = set()
            for line in text.splitlines():
                match = CANONICAL.match(line)
                if match is not None:  # the program's canonical planner, the same on every level
                    n, arm = int(match.group(1)), match.group(2)
                    if canonical.setdefault(n, arm) != arm:
                        raise SystemExit(f'{path}: {n} canonical {arm}, another log {canonical[n]}')
                    continue
                match = ROW.match(line)
                if match is None:
                    continue
                n, arm, ms = int(match.group(1)), match.group(2), float(match.group(3))
                cell = entry['sizes'].setdefault(n, {'n': n, 'planned': arm, 'arms': {}})
                if n not in seen and arm != cell['planned']:  # each log lists its planned arm first
                    raise SystemExit(f'{path}: {n} planned {arm}, another log of its level {cell["planned"]}')
                seen.add(n)
                cell['arms'].setdefault(arm, []).append(ms)
    levels = sorted(by_level.values(), key=lambda entry: list(LEVELS).index(entry['level']))
    for entry in levels:
        missing = sorted(set(entry['sizes']) - set(canonical))
        if missing:
            raise SystemExit(f'{directory}: no "n=<n> canonical:" line for {entry["level"]} n={missing[0]}')
        entry['canonical'] = {str(n): canonical[n] for n in sorted(entry['sizes'])}
        entry['sizes'] = [entry['sizes'][n] for n in sorted(entry['sizes'])]
    return levels


def merge(data, levels, kind):
    """A type's data file object: the board's level entries in place of the existing file's of the same level (data,
    or None for a new file), the other levels kept as they were measured."""
    _, program, element_bytes = TYPES[kind]
    replaced = {entry['level'] for entry in levels}
    kept = [entry for entry in (data or {}).get('levels', []) if entry['level'] not in replaced]
    return {
        'schema': SCHEMA,
        'note': f'SIMD-DISPATCH fft level boards ({kind} forward arms, ms per transform, best of each program\'s '
                'repetitions, one value per pass; each level with the board that measured it); '
                'scripts/gen_fft_level_plans.py derives detail/level_plans_gen.hpp from it.',
        'program': program,
        'element_bytes': element_bytes,
        'levels': sorted(kept + levels, key=lambda entry: list(LEVELS).index(entry['level'])),
    }


def span(times):
    """A cell's median ms at the precision its program printed (3 decimals before 2026-11, 5 since)."""
    return f'{statistics.median(times):g}'


def derive(cell):
    """The cell's arm: the fastest median when its range lies wholly below the planned arm's, else the planned."""
    planned = cell['arms'][cell['planned']]
    times = cell['arms']
    fastest = min(times, key=lambda arm: (statistics.median(times[arm]), statistics.fmean(times[arm]), arm))
    if fastest != cell['planned'] and max(cell['arms'][fastest]) < min(planned):
        return fastest
    return cell['planned']


def rows_of(path, data):
    """One type's header rows and their notes, each level's cells against the canonical arms its own board's program
    printed."""
    names = algorithms()
    rows = []
    for entry in data['levels']:
        canonical = {int(n): arm for n, arm in entry['canonical'].items()}
        for cell in entry['sizes']:
            n = cell['n']
            log2 = n.bit_length() - 1
            if n != 1 << log2:
                raise SystemExit(f'{path}: {n} is not a power of two')
            arm = derive(cell)
            if canonical.get(n) not in cell['arms']:
                raise SystemExit(f'{path}: {entry["level"]} {n} has no arm of the canonical forward {canonical.get(n)}')
            code = (f'    {{{data["element_bytes"]}, crd::CpuLevel::{LEVELS[entry["level"]]}, {log2}, '
                    f'FftAlgorithm::{names[arm]}}},')
            note = f'{span(cell["arms"][arm])}'
            if arm != canonical[n]:
                note += f'; {canonical[n]} {span(cell["arms"][canonical[n]])}'
            rows.append((code, note))
    return rows


def sources(path, data):
    """A type's data lines: its file and program, then each run of levels one board measured, with that board."""
    out = [f'// Data: {path.relative_to(ROOT).as_posix()} ({data["program"]}):']
    for key, group in itertools.groupby(data['levels'], key=lambda entry: tuple(entry[k] for k in PROVENANCE)):
        machine, commit, board = key
        levels = ', '.join(entry['level'] for entry in group)
        out += [f'//   {levels}: at {commit} on {machine}', f'//   ({board}).']
    return out


def header(boards):
    """The header from every type's (data file, data) pair, in TYPES order."""
    rows = []
    data_lines = []
    for path, data in boards:
        rows += rows_of(path, data)
        data_lines += sources(path, data)
    width = max(len(code) for code, _ in rows)
    out = [
        '// level_plans_gen.hpp -- GENERATED by scripts/gen_fft_level_plans.py; do not edit.',
        *data_lines,
        '//',
        '// Each CPU level\'s forward of a type and size as its fft level board measured it (ADR-0134 §7): the',
        '// board\'s fastest arm (the lowest median of its passes) where its every pass beat every pass of the',
        '// arm the level ran as planned, the planned arm otherwise (equal medians: the lower mean). Each row\'s',
        '// comment is its arm\'s median ms, then the canonical planner\'s arm\'s (as the level\'s board binary',
        '// planned it) where the row differs from it. FftPlan runs a row under DeterminismMode::Default; every',
        '// type, level and size no board measured, and every other mode, runs the canonical planner',
        '// (detail::planned_forward).',
        '',
        '#pragma once',
        '',
        '#include <crd/core/cpu_features.hpp>',
        '#include <crd/core/types.hpp>',
        '#include <crd/hesap/fft/fft_kernels.hpp>',
        '',
        'namespace crd::hesap::fft::detail',
        '{',
        '',
        'struct FftLevelPlan',
        '{',
        '    crd::u8 element_bytes; // 4: f32, 8: f64',
        '    crd::CpuLevel level;',
        '    crd::u8 log2;',
        '    FftAlgorithm forward;',
        '};',
        '',
        'inline constexpr FftLevelPlan kFftLevelPlans[] = {',
    ]
    for code, note in rows:
        out.append(f'{code.ljust(width)} // {note}')
    out += [
        '};',
        '',
        '} // namespace crd::hesap::fft::detail',
        '',
    ]
    for line in out:
        if len(line) > 120:
            raise SystemExit(f'line over 120 columns: {line}')
    return '\n'.join(out)


def load(kind):
    """A type's data file object, checked against its schema, program, element size and level provenance."""
    path, program, element_bytes = TYPES[kind]
    data = json.loads(path.read_text(encoding='utf-8'))
    if data.get('schema') != SCHEMA:
        raise SystemExit(f'{path}: schema must be {SCHEMA}')
    if data.get('element_bytes') != element_bytes or data.get('program') != program:
        raise SystemExit(f'{path}: not the {kind} board ({program}, {element_bytes}-byte elements)')
    for entry in data['levels']:
        missing = [key for key in (*PROVENANCE, 'canonical') if not entry.get(key)]
        if missing:
            raise SystemExit(f'{path}: level {entry.get("level")} has no {missing[0]}')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--check', action='store_true', help='fail when the header differs from the data files')
    parser.add_argument('--import', dest='board_dir', metavar='DIR',
                        help='replace the levels a board directory ran in its type\'s data file')
    parser.add_argument('--type', choices=sorted(TYPES), help='the element type to import (with --import)')
    parser.add_argument('--commit', help='the board binary\'s commit (with --import)')
    parser.add_argument('--machine', help='the machine the board ran on (with --import)')
    parser.add_argument('--board', help='the board\'s record, a docs/bench link (with --import)')
    args = parser.parse_args()
    if args.board_dir is not None:
        if not all((args.type, args.commit, args.machine, args.board)):
            parser.error('--import needs --type, --commit, --machine and --board')
        levels = import_board(args.board_dir, args.type,
                              {'machine': args.machine, 'commit': args.commit, 'board': args.board})
        if not levels:
            raise SystemExit(f'{args.board_dir}: no pass1-{args.type}-<label>.log with its level record')
        path = TYPES[args.type][0]
        text = json.dumps(merge(load(args.type) if path.exists() else None, levels, args.type), indent=1)
        text = re.sub(r'\[\s+([\d.,\s]+?)\s+\]', lambda m: '[' + ' '.join(m.group(1).split()) + ']', text)
        path.write_bytes((text + '\n').encode('utf-8'))
    text = header([(TYPES[kind][0], load(kind)) for kind in TYPES])
    if args.check:
        current = HEADER.read_bytes().replace(b'\r\n', b'\n').decode('utf-8') if HEADER.exists() else ''
        if current != text:
            print(f'{HEADER.relative_to(ROOT).as_posix()} differs from the board data files; '
                  'run python scripts/gen_fft_level_plans.py', file=sys.stderr)
            return 1
        return 0
    HEADER.write_bytes(text.encode('utf-8'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
