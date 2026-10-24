#!/usr/bin/env python3
"""DIAG.11b: measure the diagnostic modes' overhead and judge the frozen budgets.

`run` launches crd-diag-overhead (tests/support/diag-overhead) once per process for every workload and arm, with
the arms interleaved (each repetition rotates their order, so drift lands on every arm alike), and writes one board
document (schema cerid-diag-overhead-board/1). An arm is a binary and a mode: `off`, `basic` and `full` of the
measured binary, and `compiled-out` (the `off` mode of a binary built with profiling compiled out, `--baseline-exe`),
which is the true zero the runtime-disabled mode is held to. `validation-core`, `validation-sync` and
`validation-gpu` are the measured binary's off mode with that device validation (`--validation`, render only); their
cost over off is reported, never budgeted (ADR-0133 accounts GPU-assisted validation separately), and a run whose
validation reported or lost a message is an instrument failure.

Per arm the board keeps the pooled frame percentiles (p50/p95/p99 by nearest rank over every timed frame of every
process), each process's median, the events (sites, payloads evaluated, recorded, filtered, profiler drops), the
captures (requested, delivered, bytes written, the longest), the resident-set growth, the recorder's ring and the
median CPU per frame (every thread's, in the record's unit: cycles on Windows, nanoseconds on POSIX). Each comparison
pairs processes of the same repetition, once for the frame time (the ratio of the arm's median to the reference's)
and once for the CPU per frame: the ratio's median over the repetitions and a percentile-bootstrap 95% interval (a
fixed seed, so a board re-renders the same).

Verdicts against ADR-0133 ID-5 (docs/design/runtime-diagnostics.md, modes and budgets):
  disabled  off vs compiled-out, budget 1%; basic  basic vs off, budget 2%; each judged on the frame time and on the
            CPU per frame, the worse of the two verdicts deciding.
  within      the interval's upper end is at or below the budget;
  over        the interval's lower end is above the budget;
  unresolved  the interval straddles it: the host's noise is wider than the claim, which is a measured loss, never
              a pass;
  full has no numeric budget: its cost is reported.
  storage   basic's recorder ring and its resident growth over off, each against 32 MiB.
A process that fails, prints no record or computes a wrong answer makes its arm an instrument failure: the board
says so and no verdict uses it.

`instructions` judges the disabled budget without the two-binary layout floor: it runs the off mode of both binaries
under Valgrind's cachegrind, each at two frame counts, and takes the difference per frame, so setup cancels. The
instructions executed do not depend on where the linker placed the code, and they are judged; the simulated cache's
misses, which do, are reported. Every count is repeated; the interval spans every pairing of the repeats, so a count
that moves between runs widens it. The jobs pool runs one thread by default: waiting workers make counts follow the
scheduler; io runs its own two-thread pool.

`render` turns boards into the markdown of docs/bench. Contract: docs/design/runtime-diagnostics.md (DIAG.11b).
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import platform
import random
import subprocess
import sys

SCHEMA = 'cerid-diag-overhead-board/1'
COUNT_SCHEMA = 'cerid-diag-overhead-instructions/1'
RECORD_SCHEMA = 'cerid-diag-overhead/1'
# cachegrind's events, summed into what a board shows: instructions (judged), first-level and last-level misses.
COUNTED = {'instructions': ('Ir',), 'L1 misses': ('I1mr', 'D1mr', 'D1mw'), 'LL misses': ('ILmr', 'DLmr', 'DLmw')}
WORKLOADS = ('alloc', 'jobs', 'offline', 'io', 'render', 'events')
SITE_WORKLOADS = ('events',) # site microbenchmarks: their cost per site is reported, no budget judges them
MODES = ('off', 'basic', 'full')
VALIDATIONS = ('core', 'sync', 'gpu') # device validation arms, `validation-<name>`: render's off mode, reported
VALIDATION_COUNTS = ('errors', 'warnings', 'lost', 'instrumentation_failures')
BUDGETS = {'disabled': ('off', 'compiled-out', 0.01), 'basic': ('basic', 'off', 0.02)}
STORAGE_BUDGET = 32 * 1024 * 1024
BOOTSTRAP = 2000
SEED = 0x11B


class OverheadError(RuntimeError):
    """The board cannot be produced (bad arguments, an unreadable board)."""


def percentile(values, q):
    """Nearest rank: the smallest value with at least q of the sorted values at or below it (q in (0, 1])."""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, -(-len(ordered) * q // 1))
    return ordered[int(rank) - 1]


def median(values):
    if not values:
        return None
    ordered = sorted(values)
    middle = len(ordered) // 2
    return ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2.0


def bootstrap_interval(ratios, seed=SEED, rounds=BOOTSTRAP):
    """Percentile-bootstrap 95% interval of the median of `ratios`; (None, None) with fewer than two."""
    if len(ratios) < 2:
        return None, None
    rng = random.Random(seed)
    medians = sorted(median([rng.choice(ratios) for _ in ratios]) for _ in range(rounds))
    return medians[int(0.025 * (rounds - 1))], medians[int(0.975 * (rounds - 1))]


def verdict(low, high, budget):
    """within, over or unresolved for a relative cost interval (ratio - 1) against `budget`."""
    if low is None or high is None:
        return 'unmeasured'
    if high <= budget:
        return 'within'
    if low > budget:
        return 'over'
    return 'unresolved'


def arm_order(arms, repetition):
    """The arms of one repetition, rotated so each arm takes every position in turn."""
    shift = repetition % len(arms)
    return arms[shift:] + arms[:shift]


def parse_record(stdout):
    """The JSON record a run printed (its last line that parses as one), or None."""
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if line.startswith('{'):
            try:
                record = json.loads(line)
            except ValueError:
                return None
            return record if record.get('schema') == RECORD_SCHEMA else None
    return None


def record_problem(record, expected_frames):
    """Why a record cannot be used, or None."""
    if record is None:
        return 'no record'
    if record.get('executed') != expected_frames:
        return 'executed %s of %s frames' % (record.get('executed'), expected_frames)
    if record.get('wrong'):
        return '%s frames computed the wrong answer' % record['wrong']
    if len(record.get('samples_ns', [])) != record.get('iterations'):
        return 'samples missing'
    if not record.get('cpu', {}).get('read'):
        return 'no process CPU time'
    captures = record.get('captures', {})
    if captures.get('delivered') != captures.get('requested') or captures.get('failures'):
        return 'captures delivered %s of %s (%s failures)' % (captures.get('delivered'), captures.get('requested'),
                                                              captures.get('failures'))
    validation = record.get('validation', {})
    if validation.get('requested', 'none') != 'none' and any(validation.get(k, 1) for k in VALIDATION_COUNTS):
        return 'validation: %s' % ', '.join('%s %s' % (k, validation.get(k)) for k in VALIDATION_COUNTS)
    return None


def run_process(command, timeout):
    """Run one measurement process; (exit code, stdout, stderr)."""
    try:
        completed = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace',
                                   timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        return None, '', 'timed out after %s s' % timeout
    except OSError as error:
        return None, '', str(error)
    return completed.returncode, completed.stdout, completed.stderr


def file_identity(path):
    path = Path(path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
    return {'path': str(path), 'sha256': digest}


def summarize_arm(runs):
    """The pooled statistics of one arm's usable runs (a list of records in repetition order, None for failures)."""
    usable = [r for r in runs if r is not None]
    pooled = [s for r in usable for s in r['samples_ns']]
    medians = [frame_wall(r) for r in usable]
    events = {key: sum(r['events'].get(key, 0) for r in usable) for key in
              ('sites', 'frames', 'payloads', 'recorded', 'filtered', 'profiler_drops')}
    captures = {key: sum(r['captures'].get(key, 0) for r in usable) for key in
                ('requested', 'delivered', 'failures', 'bytes_written')}
    captures['longest_ns'] = max((r['captures'].get('longest_ns', 0) for r in usable), default=0)
    growth = [r['memory']['resident_frames'] - r['memory']['resident_before'] for r in usable
              if r['memory'].get('read')]
    p50 = percentile(pooled, 0.50)
    sites_per_frame = events['sites'] / events['frames'] if events['frames'] else 0
    return {'processes': len(runs), 'usable': len(usable), 'frames': len(pooled),
            'p50_ns': p50, 'p95_ns': percentile(pooled, 0.95), 'p99_ns': percentile(pooled, 0.99),
            'site_p50_ns': None if p50 is None or not sites_per_frame else p50 / sites_per_frame,
            'process_medians_ns': medians, 'cpu_per_frame': median([frame_cpu(r) for r in usable]),
            'cpu_unit': usable[0]['cpu']['unit'] if usable else None, 'events': events,
            'captures': captures, 'resident_growth_bytes': median(growth),
            'resident_peak_bytes': max((r['memory']['resident_peak'] for r in usable), default=None),
            'recorder_ring_bytes': max((r['memory']['recorder_ring_bytes'] for r in usable), default=0),
            'lifetime': usable[0]['lifetime'] if usable else None, 'host': usable[0]['host'] if usable else None,
            'build': usable[0]['build'] if usable else None,
            'validation': usable[0].get('validation') if usable else None}


def frame_wall(record):
    """A process's frame time: the median of its timed frames, in nanoseconds."""
    return median(record['samples_ns'])


def frame_cpu(record):
    """A process's CPU per frame: every thread's CPU over the timed frames over their count, in the record's unit."""
    return record['cpu']['timed'] / record['iterations']


def compare(candidate_runs, reference_runs, value=frame_wall):
    """Paired ratios of a per-process value (same repetition), their median and 95% interval, as relative costs."""
    ratios = [value(c) / value(r) for c, r in zip(candidate_runs, reference_runs)
              if c is not None and r is not None and value(r)]
    if not ratios:
        return {'pairs': 0, 'cost': None, 'low': None, 'high': None}
    low, high = bootstrap_interval(ratios)
    return {'pairs': len(ratios), 'cost': median(ratios) - 1.0, 'low': None if low is None else low - 1.0,
            'high': None if high is None else high - 1.0}


def worse(first, second):
    """The verdict of a budget judged on two measurands: over if either is over, within only if both are within."""
    for name in ('over', 'unmeasured', 'unresolved'):
        if name in (first, second):
            return name
    return first


def judge(arms, budgeted=True):
    """Comparisons and verdicts of one workload's arms: each budget is judged on the frame time and on the CPU per
    frame, and its verdict is the worse of the two. A site microbenchmark (`budgeted` false) is reported, never
    judged: the budgets bound the cost of a frame of work, not of an empty loop of sites."""
    result = {}
    pairs = [(name, candidate, reference, budget if budgeted else None)
             for name, (candidate, reference, budget) in BUDGETS.items()] + [('full', 'full', 'off', None)] + [
                 ('validation-' + v, 'validation-' + v, 'off', None) for v in VALIDATIONS]
    for name, candidate, reference, budget in pairs:
        if candidate not in arms or reference not in arms:
            if name in BUDGETS:
                result[name] = {'verdict': 'not measured', 'budget': budget}
            continue
        comparison = compare(arms[candidate]['runs'], arms[reference]['runs'])
        cpu = compare(arms[candidate]['runs'], arms[reference]['runs'], frame_cpu)
        wall_verdict, cpu['verdict'] = ('reported', 'reported') if budget is None else (
            verdict(comparison['low'], comparison['high'], budget), verdict(cpu['low'], cpu['high'], budget))
        comparison.update({'budget': budget, 'candidate': candidate, 'reference': reference, 'cpu': cpu,
                           'wall_verdict': wall_verdict, 'verdict': worse(wall_verdict, cpu['verdict'])})
        result[name] = comparison
    if 'basic' in arms and 'off' in arms:
        basic, off = arms['basic']['summary'], arms['off']['summary']
        growth = None
        if basic['resident_growth_bytes'] is not None and off['resident_growth_bytes'] is not None:
            growth = basic['resident_growth_bytes'] - off['resident_growth_bytes']
        ring = basic['recorder_ring_bytes']
        storage = 'within' if (growth is not None and ring <= STORAGE_BUDGET and growth <= STORAGE_BUDGET) else (
            'over' if growth is not None else 'unmeasured')
        result['storage'] = {'budget': STORAGE_BUDGET, 'recorder_ring_bytes': ring, 'resident_growth_over_off': growth,
                             'verdict': storage if budgeted else 'reported'}
    return result


def board_head(schema, args, binaries, parameters):
    """A board's identity: when, where and what it measured; its workloads follow."""
    return {'schema': schema, 'label': args.label,
            'measured_at': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
            'host': {'system': platform.system(), 'release': platform.release(), 'machine': platform.machine(),
                     'processor': platform.processor(), 'cpus': os.cpu_count(), 'python': platform.python_version()},
            'binaries': {name: file_identity(exe) for name, exe in binaries.items()}, 'parameters': parameters,
            'workloads': {}}


def parse_cachegrind(text):
    """A cachegrind output file's totals by event."""
    names = totals = None
    for line in text.splitlines():
        if line.startswith('events:'):
            names = line.split()[1:]
        elif line.startswith('summary:'):
            totals = [int(value) for value in line.split()[1:]]
    if not names or totals is None or len(names) != len(totals):
        raise OverheadError('not a cachegrind summary')
    return {name: sum(dict(zip(names, totals)).get(event, 0) for event in events)
            for name, events in COUNTED.items()}


def per_frame(short_runs, long_runs, frames):
    """(least, most) per frame of a total counted at two frame counts `frames` apart, over every pairing of the
    repeats; (None, None) without a run of each."""
    deltas = [(long - short) / float(frames) for long in long_runs for short in short_runs]
    return (min(deltas), max(deltas)) if deltas else (None, None)


def count(args, runner=run_process):
    """The instruction board: each arm's events per frame under cachegrind, and the disabled budget judged on the
    instructions (the misses are reported)."""
    arms = [('compiled-out', args.baseline_exe), ('off', args.exe)]
    short, long = args.frames
    scratch = Path(args.scratch).resolve()
    board = board_head(COUNT_SCHEMA, args, dict(arms),
                       {'frames': [short, long], 'warmup': args.warmup, 'threads': args.threads,
                        'repeats': args.repeats, 'tool': [args.valgrind, '--tool=cachegrind', '--cache-sim=yes']})
    for workload in args.workloads:
        totals = {name: {short: [], long: []} for name, _ in arms}
        failures = []
        for repeat in range(args.repeats):
            for name, exe in arm_order(arms, repeat):
                for frames in (short, long):
                    directory = scratch / ('%s-%s-%d-%d' % (workload, name, frames, repeat))
                    directory.mkdir(parents=True, exist_ok=True)
                    output = directory / 'cachegrind.out'
                    command = [args.valgrind, '--tool=cachegrind', '--cache-sim=yes',
                               '--cachegrind-out-file=' + str(output), str(exe), '--workload', workload, '--mode',
                               'off', '--iterations', str(frames), '--warmup', str(args.warmup), '--captures', '0',
                               '--threads', str(args.threads), '--scratch', str(directory)]
                    code, stdout, stderr = runner(command, args.timeout)
                    problem = ('exit %s: %s' % (code, stderr.strip()[-300:])) if code != 0 else record_problem(
                        parse_record(stdout), frames + args.warmup)
                    if not problem:
                        try:
                            totals[name][frames].append(parse_cachegrind(output.read_text(encoding='utf-8')))
                        except (OSError, OverheadError) as error:
                            problem = str(error)
                    if problem:
                        failures.append({'arm': name, 'repetition': repeat, 'problem': problem})
                    if args.progress:
                        print('%s %s %d frames #%d: %s' % (workload, name, frames, repeat, problem or 'ok'),
                              file=sys.stderr, flush=True)
        rates = {name: {event: per_frame([t[event] for t in totals[name][short]],
                                         [t[event] for t in totals[name][long]], long - short)
                        for event in COUNTED} for name, _ in arms}
        costs = {}
        if not failures:
            for event in COUNTED:
                (off_least, off_most), (out_least, out_most) = rates['off'][event], rates['compiled-out'][event]
                costs[event] = ({'low': off_least / out_most - 1.0, 'high': off_most / out_least - 1.0}
                                if out_least > 0 else {'low': None, 'high': None})
            _, _, budget = BUDGETS['disabled']
            costs['verdict'] = verdict(costs['instructions']['low'], costs['instructions']['high'], budget)
        board['workloads'][workload] = {'per_frame': rates, 'disabled': costs, 'failures': failures}
    return board


def measure(args, runner=run_process):
    arms = [('off', args.exe, 'off', []), ('basic', args.exe, 'basic', []), ('full', args.exe, 'full', [])]
    arms += [('validation-' + v, args.exe, 'off', ['--validation', v]) for v in VALIDATIONS]
    if args.baseline_exe:
        arms.insert(0, ('compiled-out', args.baseline_exe, 'off', []))
    arms = [a for a in arms if a[0] in args.arms]
    if not arms:
        raise OverheadError('no arm selected')
    if any(extra for _, _, _, extra in arms) and args.workloads != ['render']:
        raise OverheadError('the validation arms measure the render workload alone')
    scratch = Path(args.scratch).resolve()
    frames = args.iterations + args.warmup
    board = board_head(SCHEMA, args, {name: exe for name, exe, _, _ in arms},
                       {'processes': args.processes, 'iterations': args.iterations, 'warmup': args.warmup,
                        'captures': args.captures, 'threads': args.threads, 'affinity': args.affinity,
                        'bootstrap': BOOTSTRAP, 'seed': SEED})
    for workload in args.workloads:
        runs = {name: [] for name, _, _, _ in arms}
        failures = []
        for repetition in range(args.processes):
            for name, exe, mode, extra in arm_order(arms, repetition):
                directory = scratch / ('%s-%s-%d' % (workload, name, repetition))
                directory.mkdir(parents=True, exist_ok=True)
                command = [str(exe), '--workload', workload, '--mode', mode, '--iterations', str(args.iterations),
                           '--warmup', str(args.warmup), '--captures', str(args.captures), '--threads',
                           str(args.threads), '--scratch', str(directory)] + extra
                if args.affinity:
                    command += ['--affinity', args.affinity]
                code, stdout, stderr = runner(command, args.timeout)
                record = parse_record(stdout) if code == 0 else None
                problem = ('exit %s: %s' % (code, stderr.strip()[-300:])) if code != 0 else record_problem(record,
                                                                                                          frames)
                if problem:
                    failures.append({'arm': name, 'repetition': repetition, 'problem': problem})
                    record = None
                runs[name].append(record)
                if args.progress:
                    print('%s %s #%d: %s' % (workload, name, repetition, problem or 'ok'), file=sys.stderr, flush=True)
        entry = {name: {'runs': runs[name], 'summary': summarize_arm(runs[name])} for name in runs}
        board['workloads'][workload] = {'arms': {name: entry[name]['summary'] for name in entry},
                                        'verdicts': judge(entry, workload not in SITE_WORKLOADS) if not failures
                                        else {}, 'failures': failures}
    return board


def percent(value):
    return 'n/a' if value is None else '%+.2f%%' % (100.0 * value)


def micro(ns):
    return 'n/a' if ns is None else '%.1f' % (ns / 1000.0)


def cpu_text(summary):
    """An arm's CPU per frame: microseconds when the unit is nanoseconds, else millions of the unit."""
    value, unit = summary.get('cpu_per_frame'), summary.get('cpu_unit')
    if value is None:
        return 'n/a'
    return '%.1f µs' % (value / 1000.0) if unit == 'ns' else '%.2f M%s' % (value / 1e6, unit)


def render(boards):
    lines = []
    for board in boards:
        host = board['host']
        lines += ['### %s' % board['label'], '',
                  '%s %s (%s), %s logical processors; %s processes per arm, %s timed frames after %s warmup each; '
                  'affinity %s.' % (host['system'], host['release'], host['machine'], host['cpus'],
                                    board['parameters']['processes'], board['parameters']['iterations'],
                                    board['parameters']['warmup'], board['parameters']['affinity'] or 'not pinned'),
                  '']
        lines += ['| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | '
                  'CPU per frame | CPU cost (95% interval) | Verdict | Payloads / recorded / filtered | '
                  'Captures (bytes) | Resident growth |',
                  '|---|---|---:|---:|---:|---:|---|---:|---|---|---|---|---:|']
        for workload, entry in board['workloads'].items():
            verdicts = entry['verdicts']
            by_arm = {'compiled-out': None, 'off': verdicts.get('disabled'), 'basic': verdicts.get('basic'),
                      'full': verdicts.get('full')}
            for arm, summary in entry['arms'].items():
                judged = by_arm[arm] if arm in by_arm else verdicts.get(arm)
                cost = 'reference'
                cpu_cost = ''
                verdict_text = ''
                if judged and judged.get('cost') is not None:
                    cost = '%s vs %s (%s .. %s)' % (percent(judged['cost']), judged['reference'],
                                                    percent(judged['low']), percent(judged['high']))
                    cpu = judged.get('cpu') or {}
                    cpu_cost = 'n/a' if cpu.get('cost') is None else '%s (%s .. %s)' % (
                        percent(cpu['cost']), percent(cpu['low']), percent(cpu['high']))
                    budget = judged.get('budget')
                    verdict_text = judged['verdict'] + ('' if budget is None else ' (%.0f%%)' % (100.0 * budget))
                events = summary['events']
                captures = summary['captures']
                growth = summary['resident_growth_bytes']
                site = summary.get('site_p50_ns')
                row = (workload, arm, micro(summary['p50_ns']), micro(summary['p95_ns']), micro(summary['p99_ns']),
                       'n/a' if site is None else '%.1f' % site, cost, cpu_text(summary), cpu_cost, verdict_text,
                       '%s / %s / %s' % (events['payloads'], events['recorded'], events['filtered']),
                       '%s of %s (%s)' % (captures['delivered'], captures['requested'], captures['bytes_written']),
                       'n/a' if growth is None else '%.1f MiB' % (growth / 1048576.0))
                lines.append('| %s |' % ' | '.join(str(cell) for cell in row))
            storage = verdicts.get('storage')
            if storage:
                lines.append('| %s | basic storage | | | | | ring %.0f KiB, resident growth over off %s | | | '
                             '%s (32 MiB) | | | |' % (workload, storage['recorder_ring_bytes'] / 1024.0,
                                'n/a' if storage['resident_growth_over_off'] is None
                                else '%.2f MiB' % (storage['resident_growth_over_off'] / 1048576.0),
                                storage['verdict']))
            for failure in entry['failures']:
                lines.append('| %s | %s | | | | | instrument failure #%d: %s | | | | | | |' % (
                    workload, failure['arm'], failure['repetition'], failure['problem'].replace('|', '/')))
        lines.append('')
    return '\n'.join(lines)


def render_counts(board):
    def rate(least, most):
        return 'n/a' if least is None else ('%.0f' % least if least == most else '%.0f .. %.0f' % (least, most))

    def cost(entry):
        return 'n/a' if not entry or entry['low'] is None else (
            percent(entry['low']) if entry['low'] == entry['high'] else '%s .. %s' % (percent(entry['low']),
                                                                                     percent(entry['high'])))

    parameters = board['parameters']
    lines = ['### %s' % board['label'], '',
             'cachegrind with a simulated cache; per frame = the difference of runs of %d and %d timed frames after %d '
             'warmup, %d repeats each; %d jobs thread(s).' % (parameters['frames'][0], parameters['frames'][1],
                                                            parameters['warmup'], parameters['repeats'],
                                                            parameters['threads']), '',
             '| Workload | Arm | %s | Verdict |' % ' | '.join('%s per frame' % event for event in COUNTED),
             '|---|---|%s---|' % ('---:|' * len(COUNTED))]
    for workload, entry in board['workloads'].items():
        for arm, rates in entry['per_frame'].items():
            lines.append('| %s | %s | %s | |' % (workload, arm, ' | '.join(rate(*rates[e]) for e in COUNTED)))
        disabled = entry['disabled']
        if disabled:
            lines.append('| %s | off vs compiled-out | %s | %s (1%%) |' % (
                workload, ' | '.join(cost(disabled[e]) for e in COUNTED), disabled['verdict']))
        for failure in entry['failures']:
            lines.append('| %s | %s | instrument failure #%d: %s |%s |' % (
                workload, failure['arm'], failure['repetition'], failure['problem'].replace('|', '/'),
                ' |' * (len(COUNTED) - 1)))
    return '\n'.join(lines + [''])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest='command', required=True)
    run = commands.add_parser('run', help='measure and write a board')
    run.add_argument('--exe', required=True, help='crd-diag-overhead of the measured build')
    run.add_argument('--baseline-exe', help='crd-diag-overhead built with profiling compiled out (the compiled-out arm)')
    run.add_argument('--workloads', default=','.join(WORKLOADS))
    run.add_argument('--arms', default='compiled-out,off,basic,full')
    run.add_argument('--processes', type=int, default=10)
    run.add_argument('--iterations', type=int, default=200)
    run.add_argument('--warmup', type=int, default=20)
    run.add_argument('--captures', type=int, default=3)
    run.add_argument('--threads', type=int, default=4,
                     help='the jobs pool, the main thread included; at most the physical cores the affinity leaves, '
                          'or thread placement spreads the processes wider than the budgets')
    run.add_argument('--affinity', help='hex processor mask every process pins itself to')
    run.add_argument('--timeout', type=float, default=600.0, help='seconds per process')
    run.add_argument('--scratch', required=True, help='an ignored directory for the crash and capture directories')
    run.add_argument('--label', required=True, help='the lane and host the board names')
    run.add_argument('--output', required=True)
    run.add_argument('--progress', action='store_true')
    counted = commands.add_parser('instructions', help='judge the disabled budget on counts under cachegrind')
    counted.add_argument('--exe', required=True, help='crd-diag-overhead of the measured build (its off mode)')
    counted.add_argument('--baseline-exe', required=True, help='crd-diag-overhead built with profiling compiled out')
    counted.add_argument('--workloads', default='offline,jobs')
    counted.add_argument('--frames', default='100,300', help='the two timed frame counts, shorter first')
    counted.add_argument('--warmup', type=int, default=20)
    counted.add_argument('--threads', type=int, default=1, help='the jobs pool, the main thread included')
    counted.add_argument('--repeats', type=int, default=2)
    counted.add_argument('--valgrind', default='valgrind')
    counted.add_argument('--timeout', type=float, default=1200.0, help='seconds per process')
    counted.add_argument('--scratch', required=True, help='an ignored directory for the runs and their counts')
    counted.add_argument('--label', required=True, help='the lane and host the board names')
    counted.add_argument('--output', required=True)
    counted.add_argument('--progress', action='store_true')
    show = commands.add_parser('render', help='markdown of one or more boards')
    show.add_argument('boards', nargs='+')
    args = parser.parse_args(argv)
    try:
        if args.command == 'render':
            boards = [json.loads(Path(p).read_text(encoding='utf-8')) for p in args.boards]
            if any(b.get('schema') not in (SCHEMA, COUNT_SCHEMA) for b in boards):
                raise OverheadError('not an overhead board')
            print('\n'.join(render_counts(b) if b['schema'] == COUNT_SCHEMA else render([b]) for b in boards))
            return 0
        args.workloads = [w for w in args.workloads.split(',') if w]
        unknown = [w for w in args.workloads if w not in WORKLOADS]
        if args.command == 'instructions':
            args.frames = [int(f) for f in args.frames.split(',') if f]
            if unknown or len(args.frames) != 2 or not 0 < args.frames[0] < args.frames[1] or args.repeats < 1:
                raise OverheadError('bad workloads %s, or not two increasing frame counts, or no repeat' % unknown)
        else:
            args.arms = [a for a in args.arms.split(',') if a]
            if unknown or args.processes < 2 or args.iterations < 1:
                raise OverheadError('bad workloads %s, or fewer than two processes or one frame' % unknown)
        for name in ('exe', 'baseline_exe'):
            if getattr(args, name):
                path = Path(getattr(args, name)).resolve()
                if not path.is_file():
                    raise OverheadError('no such program: %s' % path)
                setattr(args, name, str(path))
        board = count(args) if args.command == 'instructions' else measure(args)
        Path(args.output).write_text(json.dumps(board, indent=1) + '\n', encoding='utf-8')
        print(render_counts(board) if args.command == 'instructions' else render([board]))
        failed = any(entry['failures'] for entry in board['workloads'].values())
        return 1 if failed else 0
    except (OverheadError, OSError, ValueError) as error:
        print('diag-overhead: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    raise SystemExit(main())
