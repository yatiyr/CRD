#!/usr/bin/env python3
"""Generate hesap-fft's per-level forward plans from SIMD-DISPATCH's saved fft board (ADR-0134 §7).

The board (`bench_fft_f32` and `bench_fft_vs_refs` at each CPU level, five passes) times every forward the planner
offers at each size of a type; each type's data file keeps every arm's time per pass under the fft kernel level each
log names (the level FftPlan keys on: a selection with no fft unit of its own, such as sse4.2, runs the one below). A
level's plan of a size is the board's fastest arm there (the lowest median of its passes) when every pass of it beat
every pass of the arm the level ran as planned during the board (the ranges are the board's run-to-run noise), and the
planned arm otherwise. The board's top level ran the canonical planner (it had no row of its own), so its planned arms
name each size's canonical forward, and its own winners become rows like any level's. The header lists every measured type, level and size; FftPlan runs a cell under DeterminismMode::Default and
the canonical planner everywhere else and under the other modes.

  python scripts/gen_fft_level_plans.py                      # write the header from the data files
  python scripts/gen_fft_level_plans.py --check              # fail when the header differs from the data files'
  python scripts/gen_fft_level_plans.py --import DIR --type f32|f64 --commit TEXT
                                                             # rewrite the type's data file from a board directory:
                                                             # pass<k>-<type>-<label>.log, pass<k>-level-<label>.txt
"""
from pathlib import Path
import argparse
import glob
import json
import re
import statistics
import sys

ROOT = Path(__file__).resolve().parents[1]
# Each element type: its data file, the board program that times its arms, its element size and the board section.
TYPES = {
    'f32': (ROOT / 'docs' / 'bench' / '2026-10-30-simd-dispatch-fft-f32-arms.json', 'bench_fft_f32', 4,
            'docs/bench/2026-10-30-simd-dispatch-fft-board.md#board-3-the-per-level-plans'),
    'f64': (ROOT / 'docs' / 'bench' / '2026-10-30-simd-dispatch-fft-f64-arms.json', 'bench_fft_vs_refs', 8,
            'docs/bench/2026-10-30-simd-dispatch-fft-board.md#board-4-the-f64-plans-and-the-avx2-re-run'),
}
HEADER = ROOT / 'engine' / 'numerics' / 'hesap-fft' / 'include' / 'crd' / 'hesap' / 'fft' / 'detail' / \
    'level_plans_gen.hpp'
KERNELS = ROOT / 'engine' / 'numerics' / 'hesap-fft' / 'include' / 'crd' / 'hesap' / 'fft' / 'fft_kernels.hpp'
SCHEMA = 'cerid-fft-level-board/1'
# The CRD_CPU_LEVEL spellings and their crd::CpuLevel enumerators, in enumeration order.
LEVELS = {'portable': 'Portable', 'sse2': 'Sse2', 'sse4.2': 'Sse42', 'avx2': 'Avx2', 'avx512': 'Avx512',
          'neon': 'Neon'}
ROW = re.compile(r'^(\d+)\s+\|\s+(\S+)\s+\|\s+([\d.]+)\s+\|')
KERNEL_LEVEL = re.compile(r'^fft kernel level: (\S+)$', re.M)


def algorithms():
    """fft_algorithm_name's spellings and their FftAlgorithm enumerators (fft_kernels.hpp)."""
    text = KERNELS.read_text(encoding='utf-8')
    pairs = re.findall(r'case FftAlgorithm::(\w+):\s*return "([\w-]+)";', text)
    if not pairs:
        raise SystemExit(f'{KERNELS}: no fft_algorithm_name cases')
    return {name: enumerator for enumerator, name in pairs}


def import_board(directory, commit, kind):
    """The data file's object of a type from a board directory's logs, one entry per fft kernel level the logs ran (a CPU
    selection without an fft unit of its own runs the one below it, so its passes join that level's)."""
    by_level = {}
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
            entry = by_level.setdefault(match.group(1), {'level': match.group(1), 'selections': [], 'sizes': {}})
            if selection not in entry['selections']:
                entry['selections'].append(selection)
            seen = set()
            for line in text.splitlines():
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
        entry['sizes'] = [entry['sizes'][n] for n in sorted(entry['sizes'])]
    _, program, element_bytes, board = TYPES[kind]
    return {
        'schema': SCHEMA,
        'note': f'SIMD-DISPATCH fft board ({kind} forward arms, ms per transform, best of each program\'s '
                'repetitions, one value per pass); scripts/gen_fft_level_plans.py derives detail/level_plans_gen.hpp '
                'from it.',
        'board': board,
        'program': program,
        'element_bytes': element_bytes,
        'commit': commit,
        'levels': levels,
    }


def span(times):
    return f'{statistics.median(times):.3f}'


def derive(cell):
    """The cell's arm: the fastest median when its range lies wholly below the planned arm's, else the planned."""
    planned = cell['arms'][cell['planned']]
    times = cell['arms']
    fastest = min(times, key=lambda arm: (statistics.median(times[arm]), statistics.fmean(times[arm]), arm))
    if fastest != cell['planned'] and max(cell['arms'][fastest]) < min(planned):
        return fastest
    return cell['planned']


def rows_of(path, data):
    """One type's header rows and their notes, each level's cells against the canonical arms: the ones the top level
    ran as planned (it had no row of its own during the boards)."""
    names = algorithms()
    canonical = {cell['n']: cell['planned'] for cell in data['levels'][-1]['sizes']}
    rows = []
    for entry in data['levels']:
        for cell in entry['sizes']:
            n = cell['n']
            log2 = n.bit_length() - 1
            if n != 1 << log2:
                raise SystemExit(f'{path}: {n} is not a power of two')
            arm = derive(cell)
            code = (f'    {{{data["element_bytes"]}, crd::CpuLevel::{LEVELS[entry["level"]]}, {log2}, '
                    f'FftAlgorithm::{names[arm]}}},')
            note = f'{span(cell["arms"][arm])}'
            if arm != canonical[n]:
                note += f'; {canonical[n]} {span(cell["arms"][canonical[n]])}'
            rows.append((code, note))
    return rows


def header(boards):
    """The header from every type's (data file, data) pair, in TYPES order."""
    rows = []
    sources = []
    for path, data in boards:
        rows += rows_of(path, data)
        sources += [f'// Data: {path.relative_to(ROOT).as_posix()}', f'// ({data["program"]} at {data["commit"]};',
                    f'// {data["board"]}).']
    width = max(len(code) for code, _ in rows)
    out = [
        '// level_plans_gen.hpp -- GENERATED by scripts/gen_fft_level_plans.py; do not edit.',
        *sources,
        '//',
        '// Each CPU level\'s forward of a type and size as SIMD-DISPATCH\'s fft board measured it (ADR-0134 §7): the',
        '// board\'s fastest arm (the lowest median of its passes) where its every pass beat every pass of the arm the',
        '// level ran as planned, the planned arm otherwise (equal medians: the lower mean). Each row\'s comment is its',
        '// arm\'s median ms, then the canonical arm\'s (the top level\'s) where the row differs from it. FftPlan runs a',
        '// row under DeterminismMode::Default; every type, level and size the board did not measure, and every other',
        '// mode, runs the canonical planner (detail::planned_forward).',
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--check', action='store_true', help='fail when the header differs from the data files')
    parser.add_argument('--import', dest='board', metavar='DIR', help='rewrite a data file from a board directory')
    parser.add_argument('--type', choices=sorted(TYPES), help='the element type to import (with --import)')
    parser.add_argument('--commit', help='the board binary\'s commit (with --import)')
    args = parser.parse_args()
    if args.board is not None:
        if not args.commit or not args.type:
            parser.error('--import needs --type and --commit')
        text = json.dumps(import_board(args.board, args.commit, args.type), indent=1)
        text = re.sub(r'\[\s+([\d.,\s]+?)\s+\]', lambda m: '[' + ' '.join(m.group(1).split()) + ']', text)
        TYPES[args.type][0].write_bytes((text + '\n').encode('utf-8'))
    boards = []
    for kind, (path, program, element_bytes, _) in TYPES.items():
        data = json.loads(path.read_text(encoding='utf-8'))
        if data.get('schema') != SCHEMA:
            raise SystemExit(f'{path}: schema must be {SCHEMA}')
        if data.get('element_bytes') != element_bytes or data.get('program') != program:
            raise SystemExit(f'{path}: not the {kind} board ({program}, {element_bytes}-byte elements)')
        boards.append((path, data))
    text = header(boards)
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
