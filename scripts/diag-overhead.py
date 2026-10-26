#!/usr/bin/env python3
"""DIAG.11b: measure the diagnostic modes' overhead and judge the frozen budgets.

`run` launches crd-diag-overhead (tests/support/diag-overhead) once per process for every workload and arm, with
the arms interleaved (each repetition rotates their order, so drift lands on every arm alike), and writes one board
document (schema cerid-diag-overhead-board/1). An arm is a binary and a mode: `off`, `basic` and `full` of the
measured binary, and `compiled-out` (the `off` mode of a binary built with profiling compiled out, `--baseline-exe`),
which is the true zero the runtime-disabled mode is held to. `validation-core`, `validation-sync` and
`validation-gpu` are the measured binary's off mode with that device validation (`--validation`, render only); their
cost over off is reported, never budgeted (ADR-0133 accounts GPU-assisted validation separately), and a run whose
validation reported or lost a message is an instrument failure. `before-off`, `before-basic` and `before-full` are the
modes of `--before-exe`, the same build made before a change; each mode's change (`change-<mode>`, the mode over its
before arm, interleaved and paired like every comparison) is reported, never budgeted.

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
scheduler; io runs its own two-thread pool. With `--before-exe` the off mode of the build made before a change is
counted too and the change reported (`off vs before-off`); either comparison binary may be left out, not both.

The peer (DIAG.11b clause (8)): with `--exe` the peer harness crd-diag-overhead-tracy (CRD_DIAG_PEER_TRACY), one
binary holds the Cerid modes and Tracy's counterparts at the same sites, so `tracy` (Tracy started, no client: every
site checks the connection and does nothing) and `tracy-connected` (a tracy-capture client on localhost from before
the first frame to after the last, `--tracy-tools`: the pinned Windows release archive or a directory holding its
two programs, each member checked against cmake/pins.json) are arms of the same binary as off, basic and full. Each
connected process gets its own client on its own port (TRACY_PORT); the run is an instrument failure unless the
client's capture holds every zone, frame mark and message the process emitted (its "Zones:" and "Frames:" lines and
tracy-csvexport's message list). The peer comparisons are reported, never budgeted: tracy over off, tracy-connected
over off, and full over tracy-connected (Cerid's recording against Tracy's, positive when Cerid costs more); on the
site workloads (events, zones, marks) each also as nanoseconds per event over its reference. `--memory` passes the
harness's --memory to the alloc workload: full counts the heap's statistics, the tracy arms report every operation.
`instructions` counts the same arms under cachegrind (`--arms`; tracy-connected with a client as above).

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
WORKLOADS = ('alloc', 'jobs', 'offline', 'io', 'render', 'events', 'zones', 'marks')
# Site microbenchmarks: their cost per event is reported, no budget judges them. The events one frame makes: a
# workload's sites (`sites` per frame), its zones (the harness's zones_per_iteration) or its marks beyond the frame's
# own.
SITE_WORKLOADS = ('events', 'zones', 'marks')
EVENTS_PER_FRAME = {'events': 4096, 'zones': 2048, 'marks': 64}
MODES = ('off', 'basic', 'full')
TRACY_ARMS = ('tracy', 'tracy-connected')
# The peer comparisons (name, candidate, reference), reported only.
PEER_COMPARISONS = (('tracy', 'tracy', 'off'), ('tracy-connected', 'tracy-connected', 'off'),
                    ('full-vs-tracy-connected', 'full', 'tracy-connected'))
TRACY_CONNECT_MS = 30000
TRACY_PORT_BASE = 18086
# The capture client's frame count over the frame marks it received: an on-demand client's trace opens with the
# profiler's start, the end of its initialisation and the connection (Tracy's Worker::Exec), measured 2026-10-26.
TRACY_FRAME_OFFSET = 3
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


def tracy_problem(record, connected):
    """Why a tracy arm's record cannot be used (the client's presence as the arm asks, every zone closed), or None."""
    tracy = record.get('tracy') or {}
    if not tracy.get('started'):
        return 'Tracy did not start (not the peer harness?)'
    if bool(tracy.get('connected')) != connected or bool(tracy.get('connected_at_end')) != connected:
        return 'the Tracy client was %s' % ('not connected throughout' if connected else 'connected')
    if tracy.get('unmatched'):
        return '%s zones did not close where they began' % tracy['unmatched']
    return None


def run_process(command, timeout, env=None):
    """Run one measurement process; (exit code, stdout, stderr). `env` adds variables to the inherited ones."""
    try:
        completed = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace',
                                   timeout=timeout, check=False, env=dict(os.environ, **env) if env else None)
    except subprocess.TimeoutExpired:
        return None, '', 'timed out after %s s' % timeout
    except OSError as error:
        return None, '', str(error)
    return completed.returncode, completed.stdout, completed.stderr


def tracy_tools(source, scratch):
    """The verified tracy-capture and tracy-csvexport: extracted from the pinned release archive (`source` a .zip) or
    found in a directory, each checked against its member digest in cmake/pins.json."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import pins # noqa: E402 (the registry helper beside this script)
    tools = {}
    for name in ('tracy-capture-windows', 'tracy-csvexport-windows'):
        pin = pins.entry('tools', name)
        source_path = Path(source)
        if source_path.is_dir():
            tool = source_path / pin['member']
            pins.verify(tool, pin['member_sha256'], pin['member'])
        else:
            tool = pins.install_member(pin, Path(scratch) / 'tracy-tools', archive=source_path)
        tools[name.split('-')[1]] = str(tool)
    return tools


def host_path(path, exe):
    """`path` as `exe` reads it: a Windows path for a Windows program run from WSL (through its interop)."""
    if platform.system() == 'Linux' and str(exe).lower().endswith('.exe'):
        return subprocess.run(['wslpath', '-w', str(path)], capture_output=True, text=True, check=True).stdout.strip()
    return str(path)


def capture_number(text, label):
    """The integer tracy-capture printed after `label` (its digit grouping removed), or None."""
    for line in text.splitlines():
        if line.strip().startswith(label):
            digits = ''.join(c for c in line.split(':', 1)[1] if c.isdigit())
            return int(digits) if digits else None
    return None


class TracyCapture:
    """One connected process's client: tracy-capture connecting to it on localhost at `port` (it retries until the
    process listens), then its capture held to what the process emitted."""

    def __init__(self, tools, directory, port, timeout):
        self.tools = tools
        self.trace = Path(directory) / 'capture.tracy'
        self.port = port
        self.timeout = timeout
        self.process = None

    def start(self):
        capture = self.tools['capture']
        self.process = subprocess.Popen([capture, '-o', host_path(self.trace, capture), '-a', '127.0.0.1', '-p',
                                         str(self.port), '-f'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, encoding='utf-8', errors='replace')

    def stop(self):
        """End a client whose process never connected; nothing to judge."""
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.communicate()

    def finish(self, record):
        """None when the capture holds every zone, frame mark and message `record` says the process emitted."""
        try:
            output, _ = self.process.communicate(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            self.stop()
            return 'tracy-capture did not finish within %s s' % self.timeout
        if self.process.returncode != 0 or not self.trace.is_file():
            return 'tracy-capture exited %s without a trace: %s' % (self.process.returncode, output.strip()[-300:])
        csvexport = self.tools['csvexport']
        listed = subprocess.run([csvexport, '-m', host_path(self.trace, csvexport)], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=self.timeout, check=False)
        lines = [line for line in listed.stdout.splitlines() if line.strip()]
        messages = 0 if lines[:1] == ['There are currently no messages!'] else max(len(lines) - 1, 0)
        counts = {'zones': capture_number(output, 'Zones:'), 'frames': capture_number(output, 'Frames:'),
                  'messages': messages if listed.returncode == 0 else None}
        tracy = record.get('tracy') or {}
        expected = {'zones': tracy.get('zones'), 'messages': tracy.get('messages'),
                    'frames': (tracy.get('frame_marks') or 0) + TRACY_FRAME_OFFSET}
        wrong = ['%s %s of %s' % (key, counts[key], expected[key]) for key in ('zones', 'frames', 'messages')
                 if counts[key] != expected[key]]
        self.trace.unlink(missing_ok=True)
        return ('the capture holds ' + ', '.join(wrong)) if wrong else None


def arm_specs(args):
    """Every selectable arm: (name, program, mode, extra arguments, connected)."""
    arms = [('off', args.exe, 'off', [], False), ('basic', args.exe, 'basic', [], False),
            ('full', args.exe, 'full', [], False)]
    arms += [('validation-' + v, args.exe, 'off', ['--validation', v], False) for v in VALIDATIONS]
    arms += [('tracy', args.exe, 'tracy', [], False),
             ('tracy-connected', args.exe, 'tracy', ['--tracy-connect', str(TRACY_CONNECT_MS)], True)]
    if args.baseline_exe:
        arms.insert(0, ('compiled-out', args.baseline_exe, 'off', [], False))
    if getattr(args, 'before_exe', None):
        arms += [('before-' + m, args.before_exe, m, [], False) for m in MODES]
    return arms


class Processes:
    """Runs one arm's process, with its own Tracy client when the arm is connected (each on the next port)."""

    def __init__(self, args, runner, capture):
        self.args = args
        self.runner = runner
        self.capture = capture
        self.port = TRACY_PORT_BASE
        self.tools = None

    def run(self, command, connected, directory, timeout):
        """(exit code, stdout, stderr, problem): a connected process's capture problem, else None."""
        if not connected:
            code, stdout, stderr = self.runner(command, timeout)
            return code, stdout, stderr, None
        if self.tools is None:
            if not getattr(self.args, 'tracy_tools', None):
                raise OverheadError('tracy-connected needs --tracy-tools (the pinned Tracy release archive)')
            self.tools = tracy_tools(self.args.tracy_tools, self.args.scratch)
        self.port += 1
        client = self.capture(self.tools, directory, self.port, timeout)
        client.start()
        code, stdout, stderr = self.runner(command, timeout, {'TRACY_PORT': str(self.port)})
        record = parse_record(stdout) if code == 0 else None
        if record is None:
            client.stop()
            return code, stdout, stderr, None
        return code, stdout, stderr, client.finish(record)


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


def per_event(candidate_runs, reference_runs, events, value=frame_wall):
    """The paired difference of a per-process value (same repetition) over `events` events per frame: its median and
    95% interval, in the value's unit per event."""
    differences = [(value(c) - value(r)) / float(events) for c, r in zip(candidate_runs, reference_runs)
                   if c is not None and r is not None]
    if not differences:
        return {'pairs': 0, 'cost': None, 'low': None, 'high': None}
    low, high = bootstrap_interval(differences)
    return {'pairs': len(differences), 'cost': median(differences), 'low': low, 'high': high}


def judge(arms, budgeted=True, events_per_frame=None):
    """Comparisons and verdicts of one workload's arms: each budget is judged on the frame time and on the CPU per
    frame, and its verdict is the worse of the two. A site microbenchmark (`budgeted` false) is reported, never
    judged: the budgets bound the cost of a frame of work, not of an empty loop of sites; its comparisons also give the
    cost per event (`events_per_frame`). The peer comparisons are reported only."""
    result = {}
    pairs = [(name, candidate, reference, budget if budgeted else None)
             for name, (candidate, reference, budget) in BUDGETS.items()] + [('full', 'full', 'off', None)] + [
                 ('validation-' + v, 'validation-' + v, 'off', None) for v in VALIDATIONS] + [
                 ('change-' + m, m, 'before-' + m, None) for m in MODES] + [
                 (name, candidate, reference, None) for name, candidate, reference in PEER_COMPARISONS]
    for name, candidate, reference, budget in pairs:
        if candidate not in arms or reference not in arms:
            if name in BUDGETS:
                result[name] = {'verdict': 'not measured', 'budget': budget}
            continue
        comparison = compare(arms[candidate]['runs'], arms[reference]['runs'])
        cpu = compare(arms[candidate]['runs'], arms[reference]['runs'], frame_cpu)
        if events_per_frame:
            comparison['per_event_ns'] = per_event(arms[candidate]['runs'], arms[reference]['runs'], events_per_frame)
            cpu['per_event'] = per_event(arms[candidate]['runs'], arms[reference]['runs'], events_per_frame,
                                         frame_cpu)
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


def count(args, runner=run_process, capture=TracyCapture):
    """The instruction board: each arm's events per frame under cachegrind, and the disabled budget judged on the
    instructions (the misses are reported). By default the off mode of the measured build, of the compiled-out one and,
    with `before_exe` (the same build made before a change), of the build before it, whose change is reported; `arms`
    selects others (basic, full, the tracy arms), whose comparisons with off and the peer comparisons are reported."""
    names = getattr(args, 'arms', None) or ['compiled-out', 'off', 'before-off']
    arms = [a for a in arm_specs(args) if a[0] in names and not a[0].startswith('validation-')]
    if not arms:
        raise OverheadError('no arm selected')
    short, long = args.frames
    scratch = Path(args.scratch).resolve()
    memory = getattr(args, 'memory', False)
    board = board_head(COUNT_SCHEMA, args, {name: exe for name, exe, _, _, _ in arms},
                       {'frames': [short, long], 'warmup': args.warmup, 'threads': args.threads,
                        'repeats': args.repeats, 'memory': memory,
                        'tool': [args.valgrind, '--tool=cachegrind', '--cache-sim=yes']})
    processes = Processes(args, runner, capture)
    for workload in args.workloads:
        totals = {name: {short: [], long: []} for name, _, _, _, _ in arms}
        failures = []
        for repeat in range(args.repeats):
            for name, exe, mode, extra, connected in arm_order(arms, repeat):
                for frames in (short, long):
                    directory = scratch / ('%s-%s-%d-%d' % (workload, name, frames, repeat))
                    directory.mkdir(parents=True, exist_ok=True)
                    output = directory / 'cachegrind.out'
                    command = [args.valgrind, '--tool=cachegrind', '--cache-sim=yes',
                               '--cachegrind-out-file=' + str(output), str(exe), '--workload', workload, '--mode',
                               mode, '--iterations', str(frames), '--warmup', str(args.warmup), '--captures', '0',
                               '--threads', str(args.threads), '--scratch', str(directory)] + extra
                    if memory and workload == 'alloc':
                        command.append('--memory')
                    outcome = processes.run(command, connected, directory, args.timeout)
                    _, problem = usable_record(name, mode, connected, frames + args.warmup, *outcome)
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
                        for event in COUNTED} for name, _, _, _, _ in arms}
        costs = {}
        change = {}
        peers = {}
        if not failures and 'compiled-out' in rates and 'off' in rates:
            costs = relative_rates(rates['off'], rates['compiled-out'])
            _, _, budget = BUDGETS['disabled']
            costs['verdict'] = verdict(costs['instructions']['low'], costs['instructions']['high'], budget)
        if not failures and 'before-off' in rates and 'off' in rates:
            change = relative_rates(rates['off'], rates['before-off'])
            change['verdict'] = 'reported'
        if not failures:
            for name, candidate, reference in (('basic', 'basic', 'off'), ('full', 'full', 'off')) + PEER_COMPARISONS:
                if candidate in rates and reference in rates:
                    peers[name] = relative_rates(rates[candidate], rates[reference])
                    peers[name].update({'candidate': candidate, 'reference': reference, 'verdict': 'reported'})
                    events = EVENTS_PER_FRAME.get(workload)
                    if events:
                        (least, most), (ref_least, ref_most) = (rates[candidate]['instructions'],
                                                                rates[reference]['instructions'])
                        peers[name]['per_event'] = {'low': (least - ref_most) / events,
                                                    'high': (most - ref_least) / events}
        board['workloads'][workload] = {'per_frame': rates, 'disabled': costs, 'change': change, 'peers': peers,
                                        'failures': failures}
    return board


def relative_rates(candidate, reference):
    """Per event, the least and the most relative cost of a candidate's per-frame range over a reference's."""
    costs = {}
    for event in COUNTED:
        (least, most), (ref_least, ref_most) = candidate[event], reference[event]
        costs[event] = ({'low': least / ref_most - 1.0, 'high': most / ref_least - 1.0}
                        if ref_least > 0 else {'low': None, 'high': None})
    return costs


def usable_record(name, mode, connected, frames, code, stdout, stderr, capture_problem):
    """(record, problem) of one process: its record when nothing is wrong with it."""
    record = parse_record(stdout) if code == 0 else None
    problem = ('exit %s: %s' % (code, (stderr or '').strip()[-300:])) if code != 0 else record_problem(record, frames)
    if not problem and mode == 'tracy':
        problem = tracy_problem(record, connected) or capture_problem
    return (None if problem else record), problem


def measure(args, runner=run_process, capture=TracyCapture):
    arms = [a for a in arm_specs(args) if a[0] in args.arms]
    if not arms:
        raise OverheadError('no arm selected')
    if any(name.startswith('validation-') for name, _, _, _, _ in arms) and args.workloads != ['render']:
        raise OverheadError('the validation arms measure the render workload alone')
    scratch = Path(args.scratch).resolve()
    frames = args.iterations + args.warmup
    memory = getattr(args, 'memory', False)
    board = board_head(SCHEMA, args, {name: exe for name, exe, _, _, _ in arms},
                       {'processes': args.processes, 'iterations': args.iterations, 'warmup': args.warmup,
                        'captures': args.captures, 'threads': args.threads, 'affinity': args.affinity,
                        'memory': memory, 'bootstrap': BOOTSTRAP, 'seed': SEED})
    processes = Processes(args, runner, capture)
    for workload in args.workloads:
        runs = {name: [] for name, _, _, _, _ in arms}
        failures = []
        for repetition in range(args.processes):
            for name, exe, mode, extra, connected in arm_order(arms, repetition):
                directory = scratch / ('%s-%s-%d' % (workload, name, repetition))
                directory.mkdir(parents=True, exist_ok=True)
                command = [str(exe), '--workload', workload, '--mode', mode, '--iterations', str(args.iterations),
                           '--warmup', str(args.warmup), '--captures', str(args.captures), '--threads',
                           str(args.threads), '--scratch', str(directory)] + extra
                if memory and workload == 'alloc':
                    command.append('--memory')
                if args.affinity:
                    command += ['--affinity', args.affinity]
                outcome = processes.run(command, connected, directory, args.timeout)
                record, problem = usable_record(name, mode, connected, frames, *outcome)
                if problem:
                    failures.append({'arm': name, 'repetition': repetition, 'problem': problem})
                runs[name].append(record)
                if args.progress:
                    print('%s %s #%d: %s' % (workload, name, repetition, problem or 'ok'), file=sys.stderr, flush=True)
        entry = {name: {'runs': runs[name], 'summary': summarize_arm(runs[name])} for name in runs}
        site = workload in SITE_WORKLOADS
        board['workloads'][workload] = {'arms': {name: entry[name]['summary'] for name in entry},
                                        'verdicts': judge(entry, not site, EVENTS_PER_FRAME.get(workload))
                                        if not failures else {}, 'failures': failures}
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
            for name in ['change-' + m for m in MODES] + ['full-vs-tracy-connected']:
                change = verdicts.get(name)
                if change and change.get('cost') is not None:
                    cpu = change.get('cpu') or {}
                    row = (workload, '%s vs %s' % (change['candidate'], change['reference']), '', '', '', '',
                           '%s (%s .. %s)' % (percent(change['cost']), percent(change['low']), percent(change['high'])),
                           '', 'n/a' if cpu.get('cost') is None else '%s (%s .. %s)' % (
                               percent(cpu['cost']), percent(cpu['low']), percent(cpu['high'])),
                           change['verdict'], '', '', '')
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
        events = [(workload, entry['verdicts']) for workload, entry in board['workloads'].items()
                  if any('per_event_ns' in v for v in entry['verdicts'].values() if isinstance(v, dict))]
        if events:
            lines += ['Per event (the paired difference of the frame medians, and of the CPU per frame, over the '
                      'events one frame makes):', '',
                      '| Workload | Comparison | ns per event (95% interval) | CPU per event (95% interval) |',
                      '|---|---|---:|---:|']
            for workload, verdicts in events:
                for name, judged in verdicts.items():
                    if not isinstance(judged, dict) or 'per_event_ns' not in judged:
                        continue
                    wall, cpu = judged['per_event_ns'], (judged.get('cpu') or {}).get('per_event') or {}
                    unit = board['workloads'][workload]['arms'][judged['candidate']].get('cpu_unit') or ''
                    lines.append('| %s | %s vs %s | %s | %s |' % (workload, judged['candidate'], judged['reference'],
                                                                 interval(wall, 'ns'), interval(cpu, unit)))
            lines.append('')
    return '\n'.join(lines)


def interval(entry, unit):
    """An absolute cost and its interval: `+12.3 ns (+11.0 .. +13.1)`."""
    if not entry or entry.get('cost') is None:
        return 'n/a'
    if entry.get('low') is None:
        return '%+.1f %s' % (entry['cost'], unit)
    return '%+.1f %s (%+.1f .. %+.1f)' % (entry['cost'], unit, entry['low'], entry['high'])


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
        change = entry.get('change')
        if change:
            lines.append('| %s | off vs before-off | %s | %s |' % (
                workload, ' | '.join(cost(change[e]) for e in COUNTED), change['verdict']))
        for peer in (entry.get('peers') or {}).values():
            each = peer.get('per_event')
            lines.append('| %s | %s vs %s | %s | %s%s |' % (
                workload, peer['candidate'], peer['reference'], ' | '.join(cost(peer[e]) for e in COUNTED),
                peer['verdict'], '' if not each else '; %s instructions per event' % rate(each['low'], each['high'])))
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
    run.add_argument('--before-exe', help='the measured build made before a change: its before-off, before-basic and '
                     'before-full arms, each mode reported against (select them in --arms)')
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
    run.add_argument('--memory', action='store_true', help="the harness's --memory for the alloc workload")
    run.add_argument('--tracy-tools', help='the pinned Tracy release archive (or a directory of its tracy-capture and '
                     'tracy-csvexport): the client of the tracy-connected arm')
    counted = commands.add_parser('instructions', help='judge the disabled budget on counts under cachegrind')
    counted.add_argument('--exe', required=True, help='crd-diag-overhead of the measured build (its off mode)')
    counted.add_argument('--baseline-exe', help='crd-diag-overhead built with profiling compiled out')
    counted.add_argument('--before-exe', help='the measured build made before a change (its off mode, reported)')
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
    counted.add_argument('--arms', help='the counted arms (default compiled-out,off,before-off: the ones given)')
    counted.add_argument('--memory', action='store_true', help="the harness's --memory for the alloc workload")
    counted.add_argument('--tracy-tools', help='as for run: the client of the tracy-connected arm')
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
            args.arms = [a for a in (args.arms or '').split(',') if a] or None
            args.frames = [int(f) for f in args.frames.split(',') if f]
            if unknown or len(args.frames) != 2 or not 0 < args.frames[0] < args.frames[1] or args.repeats < 1:
                raise OverheadError('bad workloads %s, or not two increasing frame counts, or no repeat' % unknown)
        else:
            args.arms = [a for a in args.arms.split(',') if a]
            if unknown or args.processes < 2 or args.iterations < 1:
                raise OverheadError('bad workloads %s, or fewer than two processes or one frame' % unknown)
        if args.command == 'instructions' and not args.baseline_exe and not args.before_exe and not args.arms:
            raise OverheadError('instructions needs --baseline-exe, --before-exe or --arms to compare with')
        for name in ('exe', 'baseline_exe', 'before_exe'):
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
