#!/usr/bin/env python3
"""Generate hesap-fft's per-level forward plans from SIMD-DISPATCH's saved fft board (ADR-0134 §7).

The board (`bench_fft_f32` at each CPU level, five passes) times every forward the planner offers at each size; its
data file keeps every arm's time per pass. A level's plan of a size is the board's fastest arm there (the lowest
median of its passes) when every pass of it beat every pass of the arm the level ran as planned during the board (the
ranges are the board's run-to-run noise), and the planned arm otherwise. The board's top level runs the canonical
planner, so its arms name each size's canonical forward. The header lists every measured level and size; FftPlan runs
a cell under DeterminismMode::Default and the canonical planner everywhere else and under the other modes.

  python scripts/gen_fft_level_plans.py                      # write the header from the data file
  python scripts/gen_fft_level_plans.py --check              # fail when the header differs from the data file's
  python scripts/gen_fft_level_plans.py --import DIR --commit TEXT
                                                             # rewrite the data file from a board directory:
                                                             # pass<k>-f32-<label>.log and pass<k>-level-<label>.txt
"""
from pathlib import Path
import argparse
import glob
import json
import re
import statistics
import sys

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'docs' / 'bench' / '2026-10-30-simd-dispatch-fft-f32-arms.json'
HEADER = ROOT / 'engine' / 'numerics' / 'hesap-fft' / 'include' / 'crd' / 'hesap' / 'fft' / 'detail' / \
    'level_plans_gen.hpp'
KERNELS = ROOT / 'engine' / 'numerics' / 'hesap-fft' / 'include' / 'crd' / 'hesap' / 'fft' / 'fft_kernels.hpp'
SCHEMA = 'cerid-fft-level-board/1'
BOARD = 'docs/bench/2026-10-30-simd-dispatch-fft-board.md#board-3-the-per-level-plans'
# The CRD_CPU_LEVEL spellings and their crd::CpuLevel enumerators, in enumeration order.
LEVELS = {'portable': 'Portable', 'sse2': 'Sse2', 'sse4.2': 'Sse42', 'avx2': 'Avx2', 'avx512': 'Avx512',
          'neon': 'Neon'}
ROW = re.compile(r'^(\d+)\s+\|\s+(\S+)\s+\|\s+([\d.]+)\s+\|')


def algorithms():
    """fft_algorithm_name's spellings and their FftAlgorithm enumerators (fft_kernels.hpp)."""
    text = KERNELS.read_text(encoding='utf-8')
    pairs = re.findall(r'case FftAlgorithm::(\w+):\s*return "([\w-]+)";', text)
    if not pairs:
        raise SystemExit(f'{KERNELS}: no fft_algorithm_name cases')
    return {name: enumerator for enumerator, name in pairs}


def import_board(directory, commit):
    """The data file's object from a board directory's logs."""
    levels = []
    for log in sorted(glob.glob(str(Path(directory) / 'pass1-f32-*.log'))):
        label = Path(log).name[len('pass1-f32-'):-len('.log')]
        lines = sorted(glob.glob(str(Path(directory) / f'pass*-level-{label}.txt')))
        if not lines:  # the pre-row programs: no level record, not this engine's arms
            continue
        selection = Path(lines[0]).read_text(encoding='utf-8').strip()
        level = re.search(r'\bselected=(\S+)', selection).group(1)
        sizes = {}
        for path in sorted(glob.glob(str(Path(directory) / f'pass*-f32-{label}.log'))):
            for line in Path(path).read_text(encoding='utf-8').splitlines():
                match = ROW.match(line)
                if match is None:
                    continue
                n, arm, ms = int(match.group(1)), match.group(2), float(match.group(3))
                cell = sizes.setdefault(n, {'n': n, 'planned': arm, 'arms': {}})
                cell['arms'].setdefault(arm, []).append(ms)
        levels.append({'level': level, 'selection': selection, 'sizes': [sizes[n] for n in sorted(sizes)]})
    levels.sort(key=lambda entry: list(LEVELS).index(entry['level']))
    return {
        'schema': SCHEMA,
        'note': 'SIMD-DISPATCH fft board (f32 forward arms, ms per transform, best of each program\'s repetitions, '
                'one value per pass); scripts/gen_fft_level_plans.py derives detail/level_plans_gen.hpp from it.',
        'board': BOARD,
        'program': 'bench_fft_f32',
        'element_bytes': 4,
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


def header(data):
    names = algorithms()
    canonical = {cell['n']: derive(cell) for cell in data['levels'][-1]['sizes']}
    rows = []
    for entry in data['levels']:
        for cell in entry['sizes']:
            n = cell['n']
            log2 = n.bit_length() - 1
            if n != 1 << log2:
                raise SystemExit(f'{DATA}: {n} is not a power of two')
            arm = derive(cell)
            code = (f'    {{{data["element_bytes"]}, crd::CpuLevel::{LEVELS[entry["level"]]}, {log2}, '
                    f'FftAlgorithm::{names[arm]}}},')
            note = f'{span(cell["arms"][arm])}'
            if arm != canonical[n]:
                note += f'; {canonical[n]} {span(cell["arms"][canonical[n]])}'
            rows.append((code, note))
    width = max(len(code) for code, _ in rows)
    out = [
        '// level_plans_gen.hpp -- GENERATED by scripts/gen_fft_level_plans.py; do not edit.',
        f'// Data: {DATA.relative_to(ROOT).as_posix()}',
        f'// ({data["program"]} at {data["commit"]};',
        f'// {data["board"]}).',
        '//',
        '// Each CPU level\'s forward of a size as SIMD-DISPATCH\'s fft board measured it (ADR-0134 §7): the board\'s',
        '// fastest arm (the lowest median of its passes) where its every pass beat every pass of the arm the level ran',
        '// as planned, the planned arm otherwise (equal medians: the lower mean). Each row\'s comment is its arm\'s',
        '// median ms, then the canonical arm\'s (the top level\'s) where the row differs from it. FftPlan runs a row',
        '// under DeterminismMode::Default; every level and size the board did not measure, and every other mode, runs',
        '// the canonical planner (detail::planned_forward).',
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
    parser.add_argument('--check', action='store_true', help='fail when the header differs from the data file')
    parser.add_argument('--import', dest='board', metavar='DIR', help='rewrite the data file from a board directory')
    parser.add_argument('--commit', help='the board binary\'s commit (with --import)')
    args = parser.parse_args()
    if args.board is not None:
        if not args.commit:
            parser.error('--import needs --commit')
        text = json.dumps(import_board(args.board, args.commit), indent=1)
        text = re.sub(r'\[\s+([\d.,\s]+?)\s+\]', lambda m: '[' + ' '.join(m.group(1).split()) + ']', text)
        DATA.write_bytes((text + '\n').encode('utf-8'))
    data = json.loads(DATA.read_text(encoding='utf-8'))
    if data.get('schema') != SCHEMA:
        raise SystemExit(f'{DATA}: schema must be {SCHEMA}')
    text = header(data)
    if args.check:
        current = HEADER.read_bytes().replace(b'\r\n', b'\n').decode('utf-8') if HEADER.exists() else ''
        if current != text:
            print(f'{HEADER.relative_to(ROOT).as_posix()} differs from {DATA.relative_to(ROOT).as_posix()}; '
                  'run python scripts/gen_fft_level_plans.py', file=sys.stderr)
            return 1
        return 0
    HEADER.write_bytes(text.encode('utf-8'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
