#!/usr/bin/env python3
"""DIAG.11b: verdict tests for diag-overhead.py.

The measurement itself runs crd-diag-overhead on a build. These tests need no build: they drive the statistics and
every verdict with fake processes, including the ones a quiet machine never produces: a cost interval straddling its
budget (unresolved, never a pass), a cost over it, a CPU per frame over its budget or noisier than it while the frame
time is within (the worse verdict decides), a process that exits nonzero, prints no record or CPU time, computes a
wrong answer or loses a capture (an instrument failure that takes the workload's verdicts with it), a device
validation arm reported over off and failed by any message it saw, and a board rendered twice the same way (the
bootstrap seed is fixed). The instruction board's fake cachegrind files check that
setup cancels between the two frame counts, that repeats which disagree widen the interval to unresolved, and that a
failed run takes the verdict. The peer (DIAG.11b clause (8)): the tracy arms are reported over off and full over
tracy-connected, per event on the site workloads; a connected process whose client's capture misses an event, a
record whose client was not there as the arm asks or whose zones did not close, and tools whose digests are not the
pinned ones each fail; the instruction board counts the same arms and their per-event instructions. The clock
(DIAG.11b (q2)): full over full-system-clock is judged at 0% on a frame of work (within, over and unresolved) and
reported per event on a site workload, the system arm runs with --clock system, and a record that did not run the
system clock fails its process. The CPU level (ADR-0134 §8): `--cpu-level` reaches every process, a record that did not
run it applied, ran under CRD_CPU_LEVEL without it or names no level fails its process (a before arm's older build
may name none), arms that ran different selections fail the workload, and the rendered head names the level.
Run: python scripts/test-diag-overhead.py
"""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('diag_overhead', ROOT / 'scripts' / 'diag-overhead.py')
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)


def selection(env=None, detected='avx2'):
    """The cpu_level object of a process started with `env` on an avx2 host whose kernels go to avx512."""
    level = (env or {}).get('CRD_CPU_LEVEL')
    return {'detected': detected, 'selected': level or detected, 'override': 'applied' if level else 'none',
            'kernel_cap': 'avx512'}


def record(workload, mode, samples, wrong=0, captures=3, delivered=None, resident=(1000, 1000), ring=0, cpu=None,
           warmup=1, validation='none', errors=0, tracy=None, clock='tick', cpu_level=None):
    """A process's record; `cpu` is its CPU per frame (cycles), by default its frame time (one busy thread), and
    False when it was not read. `tracy` is its tracy object (the tracy mode's); `cpu_level` its CPU selection (by
    default the detected level's, False for a build that records none)."""
    per_frame = mod.median(samples) if cpu is None else (0 if cpu is False else cpu)
    return {'tracy': tracy or {'started': False},
            'schema': mod.RECORD_SCHEMA, 'workload': workload, 'mode': mode, 'warmup': warmup,
            'iterations': len(samples), 'executed': len(samples) + warmup, 'wrong': wrong, 'samples_ns': samples,
            'cpu': {'read': cpu is not False, 'unit': 'cycles', 'timed': int(per_frame * len(samples))},
            'events': {'sites': 4, 'frames': 2, 'payloads': 0, 'recorded': 0, 'filtered': 6, 'profiler_drops': 0},
            'captures': {'requested': captures, 'delivered': captures if delivered is None else delivered,
                         'failures': 0, 'bytes_written': 100, 'longest_ns': 5},
            'memory': {'read': True, 'resident_before': resident[0], 'resident_frames': resident[1],
                       'resident_peak': resident[1], 'recorder_ring_bytes': ring},
            'validation': {'requested': validation} if validation == 'none' else {
                'requested': validation, 'errors': errors, 'warnings': 0, 'lost': 0, 'instrumentation_failures': 0},
            'lifetime': {}, 'host': {}, 'build': {'clock': clock, 'tick_clock_mapped': clock == 'tick'},
            **({} if cpu_level is False else {'cpu_level': cpu_level or selection()})}


class Args:
    def __init__(self, scratch, **kw):
        self.exe = 'measured'
        self.baseline_exe = 'compiled-out'
        self.workloads = ['alloc']
        self.arms = ['compiled-out', 'off', 'basic', 'full']
        self.processes = 6
        self.iterations = 5
        self.warmup = 1
        self.captures = 3
        self.threads = 4
        self.affinity = None
        self.cpu_level = None
        self.timeout = 5.0
        self.scratch = scratch
        self.label = 'test'
        self.progress = False
        self.__dict__.update(kw)


class FakeRunner:
    """Answers each command from a table of per-arm medians (and of per-arm CPU per frame, `cpu`, jittered by
    `cpu_jitter`; by default the medians); `special` maps (arm, repetition) to (code, stdout)."""

    def __init__(self, medians, special=None, jitter=0.0, cpu=None, cpu_jitter=0.0):
        self.medians = medians
        self.special = special or {}
        self.jitter = jitter
        self.cpu = cpu
        self.cpu_jitter = cpu_jitter
        self.calls = []
        self.ports = []
        self.envs = []

    def __call__(self, command, timeout, env=None):
        exe = command[0]
        mode = command[command.index('--mode') + 1]
        arm = 'compiled-out' if exe == 'compiled-out' else ('before-' + mode if exe == 'before' else mode)
        validation = command[command.index('--validation') + 1] if '--validation' in command else 'none'
        arm = arm if validation == 'none' else 'validation-' + validation
        clock = command[command.index('--clock') + 1] if '--clock' in command else 'tick'
        arm = 'full-system-clock' if clock == 'system' else arm
        connected = '--tracy-connect' in command
        if connected:
            arm = 'tracy-connected'
            self.ports.append((env or {}).get('TRACY_PORT'))
        repetition = sum(1 for c in self.calls if c == arm)
        self.calls.append(arm)
        if (arm, repetition) in self.special:
            return self.special[(arm, repetition)]
        # Jitter moves the references (off, and the system clock's arm) and the candidates of one repetition in
        # opposite directions, alternately.
        sign = 1.0 if (repetition + (arm in ('off', 'full-system-clock'))) % 2 else -1.0
        base = self.medians[arm] * (1.0 + sign * self.jitter)
        samples = [int(base) - 2, int(base) - 1, int(base), int(base) + 1, int(base) + 2]
        cpu = None if self.cpu is None else self.cpu[arm] * (1.0 + sign * self.cpu_jitter)
        ring = 155648 if arm in ('basic', 'full') else 0
        tracy = None if mode != 'tracy' else {'started': True, 'connected': connected, 'connected_at_end': connected,
                                              'zones': 7, 'messages': 5, 'frame_marks': 6, 'unmatched': 0}
        self.envs.append(env)
        text = json.dumps(record('alloc', mode, samples, ring=ring, cpu=cpu, validation=validation, tracy=tracy,
                                 clock=clock, cpu_level=selection(env)))
        return 0, 'noise line\n' + text + '\n', ''


class Statistics(unittest.TestCase):
    def test_nearest_rank_percentiles_and_median(self):
        values = list(range(1, 101))
        self.assertEqual(mod.percentile(values, 0.50), 50)
        self.assertEqual(mod.percentile(values, 0.95), 95)
        self.assertEqual(mod.percentile(values, 0.99), 99)
        self.assertEqual(mod.percentile([7], 0.99), 7)
        self.assertIsNone(mod.percentile([], 0.5))
        self.assertEqual(mod.median([3, 1, 2]), 2)
        self.assertEqual(mod.median([4, 1, 3, 2]), 2.5)

    def test_bootstrap_is_seeded_and_brackets_the_median(self):
        ratios = [1.01, 1.02, 1.00, 1.03, 1.015, 1.025]
        first = mod.bootstrap_interval(ratios)
        self.assertEqual(first, mod.bootstrap_interval(ratios))
        self.assertLessEqual(first[0], mod.median(ratios))
        self.assertGreaterEqual(first[1], mod.median(ratios))
        self.assertEqual(mod.bootstrap_interval([1.0]), (None, None))

    def test_verdicts_at_their_boundaries(self):
        self.assertEqual(mod.verdict(-0.01, 0.02, 0.02), 'within')
        self.assertEqual(mod.verdict(0.021, 0.05, 0.02), 'over')
        self.assertEqual(mod.verdict(0.0, 0.03, 0.02), 'unresolved')
        self.assertEqual(mod.verdict(None, None, 0.02), 'unmeasured')

    def test_arm_order_rotates_every_arm_through_every_position(self):
        arms = ['a', 'b', 'c']
        orders = [mod.arm_order(arms, r) for r in range(3)]
        for position in range(3):
            self.assertEqual(sorted(order[position] for order in orders), arms)

    def test_a_record_is_the_last_parsable_line_of_its_schema(self):
        good = json.dumps(record('alloc', 'off', [1, 2]))
        self.assertEqual(mod.parse_record('x\n' + good + '\n')['mode'], 'off')
        self.assertIsNone(mod.parse_record('{"schema": "other"}'))
        self.assertIsNone(mod.parse_record('{broken'))
        self.assertIsNone(mod.parse_record(''))

    def test_record_problems(self):
        self.assertIsNone(mod.record_problem(record('alloc', 'off', [1, 2]), 3))
        self.assertIn('executed', mod.record_problem(record('alloc', 'off', [1, 2]), 9))
        self.assertIn('wrong answer', mod.record_problem(record('alloc', 'off', [1, 2], wrong=1), 3))
        self.assertIn('captures', mod.record_problem(record('alloc', 'off', [1, 2], delivered=1), 3))
        self.assertEqual(mod.record_problem(record('alloc', 'off', [1, 2], cpu=False), 3), 'no process CPU time')
        self.assertEqual(mod.record_problem(None, 3), 'no record')
        self.assertIsNone(mod.record_problem(record('render', 'off', [1, 2], validation='gpu'), 3))
        self.assertIn('errors 1', mod.record_problem(record('render', 'off', [1, 2], validation='gpu', errors=1), 3))


class Boards(unittest.TestCase):
    def measure(self, runner, **kw):
        with tempfile.TemporaryDirectory() as scratch:
            return mod.measure(Args(scratch, **kw), runner=runner)

    def test_a_quiet_host_qualifies_both_budgets(self):
        runner = FakeRunner({'compiled-out': 100000, 'off': 100400, 'basic': 101000, 'full': 150000})
        board = self.measure(runner)
        verdicts = board['workloads']['alloc']['verdicts']
        self.assertEqual(verdicts['disabled']['verdict'], 'within')
        self.assertEqual(verdicts['basic']['verdict'], 'within')
        self.assertEqual(verdicts['full']['verdict'], 'reported')
        self.assertAlmostEqual(verdicts['full']['cost'], 150000 / 100400 - 1.0, places=6)
        self.assertEqual(verdicts['storage']['verdict'], 'within')
        self.assertEqual(verdicts['storage']['recorder_ring_bytes'], 155648)
        self.assertEqual(board['workloads']['alloc']['failures'], [])
        # Interleaved: every repetition runs every arm before the next repetition starts.
        self.assertEqual(sorted(runner.calls[:4]), sorted(['compiled-out', 'off', 'basic', 'full']))
        arms = board['workloads']['alloc']['arms']
        self.assertEqual(arms['off']['usable'], 6)
        self.assertEqual(arms['off']['frames'], 30)
        self.assertEqual(arms['off']['p50_ns'], 100400)

    def test_a_cost_over_its_budget_is_over(self):
        board = self.measure(FakeRunner({'compiled-out': 100000, 'off': 100000, 'basic': 105000, 'full': 106000}))
        self.assertEqual(board['workloads']['alloc']['verdicts']['basic']['verdict'], 'over')

    def test_noise_wider_than_the_claim_is_unresolved_not_within(self):
        board = self.measure(FakeRunner({'compiled-out': 100000, 'off': 100000, 'basic': 101000, 'full': 101000},
                                        jitter=0.03), processes=8)
        self.assertEqual(board['workloads']['alloc']['verdicts']['basic']['verdict'], 'unresolved')

    def test_a_failed_process_is_an_instrument_failure_and_takes_the_verdicts(self):
        medians = {'compiled-out': 100000, 'off': 100000, 'basic': 101000, 'full': 101000}
        for special in ({('basic', 2): (3, '', 'the host did not start')},
                        {('off', 1): (0, 'no json here', '')},
                        {('full', 0): (0, json.dumps(record('alloc', 'full', [1, 2, 3, 4, 5], wrong=2)), '')},
                        {('basic', 0): (0, json.dumps(record('alloc', 'basic', [1, 2, 3, 4, 5], delivered=2)), '')}):
            board = self.measure(FakeRunner(medians, special))
            entry = board['workloads']['alloc']
            self.assertEqual(len(entry['failures']), 1, special)
            self.assertEqual(entry['verdicts'], {}, special)
            self.assertIn('instrument failure', mod.render([board]))

    def test_the_cpu_per_frame_is_judged_too_and_the_worse_verdict_decides(self):
        medians = {'compiled-out': 100000, 'off': 100000, 'basic': 100500, 'full': 101000}
        cpu = {'compiled-out': 400000, 'off': 400000, 'basic': 420000, 'full': 800000}
        board = self.measure(FakeRunner(medians, cpu=cpu))
        verdicts = board['workloads']['alloc']['verdicts']
        self.assertEqual(verdicts['basic']['wall_verdict'], 'within')
        self.assertEqual(verdicts['basic']['cpu']['verdict'], 'over')
        self.assertEqual(verdicts['basic']['verdict'], 'over')
        self.assertAlmostEqual(verdicts['basic']['cpu']['cost'], 0.05, places=6)
        self.assertEqual(verdicts['disabled']['verdict'], 'within')
        self.assertEqual(verdicts['full']['cpu']['verdict'], 'reported')
        self.assertEqual(board['workloads']['alloc']['arms']['basic']['cpu_per_frame'], 420000)
        noisy = self.measure(FakeRunner(medians, cpu=dict(cpu, basic=400000), cpu_jitter=0.03), processes=8)
        verdicts = noisy['workloads']['alloc']['verdicts']
        self.assertEqual(verdicts['basic']['wall_verdict'], 'within')
        self.assertEqual(verdicts['basic']['cpu']['verdict'], 'unresolved')
        self.assertEqual(verdicts['basic']['verdict'], 'unresolved')

    def test_without_a_compiled_out_binary_the_disabled_budget_is_not_measured(self):
        board = self.measure(FakeRunner({'off': 100000, 'basic': 100500, 'full': 120000}), baseline_exe=None)
        self.assertEqual(board['workloads']['alloc']['verdicts']['disabled']['verdict'], 'not measured')

    def test_a_site_microbenchmark_is_reported_never_judged(self):
        board = self.measure(FakeRunner({'compiled-out': 100, 'off': 100, 'basic': 9000, 'full': 90000}),
                             workloads=['events'])
        entry = board['workloads']['events']
        for name in ('disabled', 'basic', 'full', 'storage'):
            self.assertEqual(entry['verdicts'][name]['verdict'], 'reported', name)
        self.assertIsNone(entry['verdicts']['basic']['budget'])
        self.assertEqual(entry['arms']['basic']['site_p50_ns'], 9000 / 2.0) # 4 sites in 2 frames per record

    def test_a_validation_arm_is_reported_over_off_and_a_message_fails_its_run(self):
        medians = {'off': 100000, 'validation-core': 130000, 'validation-gpu': 250000}
        arms = ['off', 'validation-core', 'validation-gpu']
        board = self.measure(FakeRunner(medians), workloads=['render'], baseline_exe=None, arms=arms)
        verdicts = board['workloads']['render']['verdicts']
        self.assertEqual(verdicts['validation-gpu']['verdict'], 'reported')
        self.assertIsNone(verdicts['validation-gpu']['budget'])
        self.assertAlmostEqual(verdicts['validation-gpu']['cost'], 1.5, places=6)
        self.assertNotIn('validation-sync', verdicts)
        self.assertIn('| validation-core |', mod.render([board]))
        failed = record('render', 'off', [1, 2, 3, 4, 5], validation='gpu', errors=1)
        board = self.measure(FakeRunner(medians, {('validation-gpu', 3): (0, json.dumps(failed), '')}),
                             workloads=['render'], baseline_exe=None, arms=arms)
        self.assertEqual(board['workloads']['render']['failures'][0]['arm'], 'validation-gpu')
        with self.assertRaises(mod.OverheadError):
            self.measure(FakeRunner(medians), workloads=['alloc'], baseline_exe=None, arms=arms)

    def test_a_change_is_reported_against_the_same_modes_of_the_build_before_it(self):
        medians = {'off': 90000, 'basic': 91000, 'full': 120000, 'before-off': 100000, 'before-basic': 101000,
                   'before-full': 150000}
        arms = ['off', 'basic', 'full', 'before-off', 'before-basic', 'before-full']
        runner = FakeRunner(medians)
        board = self.measure(runner, baseline_exe=None, before_exe='before', arms=arms)
        verdicts = board['workloads']['alloc']['verdicts']
        for mode, cost in (('off', 0.9), ('basic', 91000 / 101000.0), ('full', 0.8)):
            change = verdicts['change-' + mode]
            self.assertEqual((change['candidate'], change['reference']), (mode, 'before-' + mode))
            self.assertAlmostEqual(change['cost'], cost - 1.0, places=6)
            self.assertAlmostEqual(change['cpu']['cost'], cost - 1.0, places=6, msg='the CPU per frame as well')
            self.assertEqual(change['verdict'], 'reported')
            self.assertIsNone(change['budget'], 'a change is reported, never budgeted')
        self.assertEqual(sorted(runner.calls[:6]), sorted(arms), 'interleaved with the arms it is paired with')
        self.assertEqual(board['binaries']['before-off']['path'], 'before')
        text = mod.render([board])
        self.assertIn('| alloc | off vs before-off |', text)
        self.assertIn('| alloc | full vs before-full |', text)
        self.assertIn('-20.00% (', text)
        self.assertTrue(all(line.count('|') == 14 for line in text.splitlines() if line.startswith('|')))
        plain = self.measure(FakeRunner(medians), baseline_exe=None, arms=['off', 'basic', 'full'])
        self.assertFalse([name for name in plain['workloads']['alloc']['verdicts'] if name.startswith('change-')])
        self.assertNotIn('vs before', mod.render([plain]))

    def test_the_tick_clock_is_judged_against_the_system_clock_at_zero(self):
        arms = ['off', 'full', 'full-system-clock']
        cheaper = FakeRunner({'off': 100000, 'full': 150000, 'full-system-clock': 154500})
        board = self.measure(cheaper, baseline_exe=None, arms=arms)
        clock = board['workloads']['alloc']['verdicts']['tick-clock']
        self.assertEqual((clock['candidate'], clock['reference'], clock['budget']), ('full', 'full-system-clock', 0.0))
        self.assertAlmostEqual(clock['cost'], 150000 / 154500.0 - 1.0, places=6)
        self.assertEqual(clock['verdict'], 'within')
        self.assertEqual(clock['cpu']['verdict'], 'within', 'the CPU per frame is judged too')
        text = mod.render([board])
        self.assertIn('| alloc | full vs full-system-clock |', text)
        self.assertIn('within (0%)', text)
        self.assertTrue(all(line.count('|') == 14 for line in text.splitlines() if line.startswith('|')))
        dearer = self.measure(FakeRunner({'off': 100000, 'full': 150000, 'full-system-clock': 147000}),
                              baseline_exe=None, arms=arms)
        self.assertEqual(dearer['workloads']['alloc']['verdicts']['tick-clock']['verdict'], 'over')
        noisy = self.measure(FakeRunner({'off': 100000, 'full': 150000, 'full-system-clock': 150300}, jitter=0.03),
                             baseline_exe=None, arms=arms, processes=8)
        self.assertEqual(noisy['workloads']['alloc']['verdicts']['tick-clock']['verdict'], 'unresolved')
        plain = self.measure(FakeRunner({'off': 100000, 'full': 150000}), baseline_exe=None, arms=['off', 'full'])
        self.assertNotIn('tick-clock', plain['workloads']['alloc']['verdicts'], 'judged only where both arms ran')

    def test_the_system_clock_arm_runs_the_system_clock(self):
        commands = []

        def runner(command, timeout, env=None):
            commands.append(command)
            return FakeRunner({'off': 100000, 'full': 150000, 'full-system-clock': 154500})(command, timeout, env)

        self.measure(runner, baseline_exe=None, arms=['full', 'full-system-clock'], processes=2)
        system = [c for c in commands if '--clock' in c]
        self.assertEqual(len(system), 2)
        self.assertTrue(all(c[c.index('--clock') + 1] == 'system' and c[c.index('--mode') + 1] == 'full'
                            for c in system))
        self.assertIsNone(mod.clock_problem(record('alloc', 'full', [1, 2], clock='system'), True))
        self.assertIn('names clock tick', mod.clock_problem(record('alloc', 'full', [1, 2]), True))
        counter = record('alloc', 'full', [1, 2], clock='system')
        counter['build']['tick_clock_mapped'] = True
        self.assertIn('cycle counter', mod.clock_problem(counter, True))
        medians = {'off': 100000, 'full': 150000, 'full-system-clock': 154500}
        board = self.measure(FakeRunner(medians, {('full-system-clock', 1): (0, json.dumps(record(
            'alloc', 'full', [1, 2, 3, 4, 5])), '')}), baseline_exe=None, arms=['full', 'full-system-clock'])
        entry = board['workloads']['alloc']
        self.assertEqual(entry['failures'][0]['arm'], 'full-system-clock')
        self.assertEqual(entry['verdicts'], {})

    def test_on_a_site_workload_the_tick_clock_is_reported_per_event(self):
        board = self.measure(FakeRunner({'off': 1000, 'full': 300000, 'full-system-clock': 328672}),
                             baseline_exe=None, arms=['off', 'full', 'full-system-clock'], workloads=['zones'])
        clock = board['workloads']['zones']['verdicts']['tick-clock']
        self.assertEqual(clock['verdict'], 'reported')
        self.assertIsNone(clock['budget'])
        self.assertAlmostEqual(clock['per_event_ns']['cost'], -28672 / 2048.0, places=6)

    def test_a_board_runs_the_cpu_level_it_asks_and_names_it(self):
        runner = FakeRunner({'compiled-out': 100000, 'off': 100400, 'basic': 101000, 'full': 150000})
        board = self.measure(runner, cpu_level='sse2')
        entry = board['workloads']['alloc']
        self.assertEqual(entry['failures'], [])
        self.assertTrue(all(env == {'CRD_CPU_LEVEL': 'sse2'} for env in runner.envs))
        self.assertEqual(board['parameters']['cpu_level'], 'sse2')
        self.assertEqual(entry['arms']['off']['cpu_level']['selected'], 'sse2')
        self.assertIn('CPU level sse2 (detected avx2, override applied, kernel cap avx512)', mod.render([board]))
        detected = self.measure(FakeRunner({'compiled-out': 100000, 'off': 100400, 'basic': 101000, 'full': 150000}))
        self.assertIn('CPU level avx2 (detected avx2, override none, kernel cap avx512)', mod.render([detected]))

    def test_a_record_that_did_not_run_the_asked_level_fails_its_process(self):
        medians = {'compiled-out': 100000, 'off': 100000, 'basic': 101000, 'full': 101000}
        refused = dict(selection(), override='above-hardware')
        for asked, cpu_level, problem in (('avx512', refused, 'CPU level avx512 was not applied'),
                                          (None, selection({'CRD_CPU_LEVEL': 'sse2'}), 'override applied'),
                                          (None, False, 'names no CPU level')):
            special = {('off', 1): (0, json.dumps(record('alloc', 'off', [1, 2, 3, 4, 5], cpu_level=cpu_level)), '')}
            entry = self.measure(FakeRunner(medians, special), cpu_level=asked)['workloads']['alloc']
            failures = [f for f in entry['failures'] if f['arm'] == 'off']
            self.assertEqual(len(failures), 1)
            self.assertTrue(any(problem in f['problem'] for f in failures), failures)
            self.assertEqual(entry['verdicts'], {})
        self.assertIsNone(mod.cpu_problem(record('alloc', 'off', [1], cpu_level=False), None, required=False))

    def test_arms_that_ran_different_selections_fail_the_workload(self):
        medians = {'compiled-out': 100000, 'off': 100000, 'basic': 101000, 'full': 101000}
        other = dict(selection(), detected='avx512', selected='avx512')
        special = {('basic', 3): (0, json.dumps(record('alloc', 'basic', [1, 2, 3, 4, 5], cpu_level=other)), '')}
        entry = self.measure(FakeRunner(medians, special))['workloads']['alloc']
        self.assertEqual([f['arm'] for f in entry['failures']], ['all'])
        self.assertIn('different CPU selections', entry['failures'][0]['problem'])
        self.assertEqual(entry['verdicts'], {})

    def test_render_is_stable_and_names_every_arm(self):
        board = self.measure(FakeRunner({'compiled-out': 100000, 'off': 100400, 'basic': 101000, 'full': 150000}))
        text = mod.render([board])
        self.assertEqual(text, mod.render([json.loads(json.dumps(board))]))
        for arm in ('compiled-out', 'off', 'basic', 'full', 'basic storage'):
            self.assertIn('| %s |' % arm, text)
        self.assertIn('within (2%)', text)
        self.assertIn('| 0.10 Mcycles |', text)
        self.assertTrue(all(line.count('|') == 14 for line in text.splitlines() if line.startswith('|')))


class FakeCapture:
    """A connected process's client: `problems` maps the n-th client (0-based) to the problem its capture finds."""
    started = []
    problems = {}

    def __init__(self, tools, directory, port, timeout):
        self.port = port
        self.index = len(FakeCapture.started)
        FakeCapture.started.append(port)

    def start(self):
        pass

    def stop(self):
        pass

    def finish(self, record):
        return FakeCapture.problems.get(self.index)


class Peer(unittest.TestCase):
    MEDIANS = {'off': 100000, 'full': 300000, 'tracy': 100500, 'tracy-connected': 280000}

    def measure(self, runner, problems=None, **kw):
        FakeCapture.started = []
        FakeCapture.problems = problems or {}
        options = dict(baseline_exe=None, arms=['off', 'full', 'tracy', 'tracy-connected'], tracy_tools='unused')
        options.update(kw)
        with tempfile.TemporaryDirectory() as scratch:
            return mod.measure(Args(scratch, **options), runner=runner, capture=FakeCapture)

    def setUp(self):
        self.tools = mod.tracy_tools
        mod.tracy_tools = lambda source, scratch: {'capture': 'capture', 'csvexport': 'csvexport'}

    def tearDown(self):
        mod.tracy_tools = self.tools

    def test_the_peer_comparisons_are_reported_and_cost_per_event_on_a_site_workload(self):
        runner = FakeRunner(self.MEDIANS)
        board = self.measure(runner, workloads=['events'])
        verdicts = board['workloads']['events']['verdicts']
        for name, cost in (('tracy', 100500 / 100000.0), ('tracy-connected', 2.8),
                           ('full-vs-tracy-connected', 300000 / 280000.0)):
            self.assertEqual(verdicts[name]['verdict'], 'reported', name)
            self.assertIsNone(verdicts[name]['budget'], name)
            self.assertAlmostEqual(verdicts[name]['cost'], cost - 1.0, places=6, msg=name)
        self.assertEqual((verdicts['full-vs-tracy-connected']['candidate'],
                          verdicts['full-vs-tracy-connected']['reference']), ('full', 'tracy-connected'))
        self.assertAlmostEqual(verdicts['full-vs-tracy-connected']['per_event_ns']['cost'], 20000 / 4096.0, places=6)
        self.assertAlmostEqual(verdicts['tracy']['per_event_ns']['cost'], 500 / 4096.0, places=6)
        # every connected process had its own client on its own port, passed to it as TRACY_PORT
        self.assertEqual(len(FakeCapture.started), 6)
        self.assertEqual(len(set(FakeCapture.started)), 6)
        self.assertEqual(runner.ports, [str(port) for port in FakeCapture.started])
        text = mod.render([board])
        self.assertIn('| events | full vs tracy-connected |', text)
        self.assertIn('| events | full vs tracy-connected | +4.9 ns (', text)
        self.assertIn('| events | tracy |', text)
        alloc = self.measure(FakeRunner(self.MEDIANS), workloads=['alloc'])
        self.assertNotIn('per_event_ns', alloc['workloads']['alloc']['verdicts']['tracy'], 'not a site workload')

    def test_a_capture_that_misses_an_event_is_an_instrument_failure(self):
        board = self.measure(FakeRunner(self.MEDIANS), {2: 'the capture holds zones 6 of 7'}, workloads=['events'])
        entry = board['workloads']['events']
        self.assertEqual(entry['failures'], [{'arm': 'tracy-connected', 'repetition': 2,
                                              'problem': 'the capture holds zones 6 of 7'}])
        self.assertEqual(entry['verdicts'], {})

    def test_a_tracy_record_is_held_to_its_arm(self):
        tracy = {'started': True, 'connected': True, 'connected_at_end': True, 'unmatched': 0}
        good = record('events', 'tracy', [1, 2], tracy=tracy)
        self.assertIsNone(mod.tracy_problem(good, True))
        self.assertIn('connected', mod.tracy_problem(good, False))
        self.assertIn('not connected throughout', mod.tracy_problem(
            record('events', 'tracy', [1, 2], tracy=dict(tracy, connected_at_end=False)), True))
        self.assertIn('did not close', mod.tracy_problem(record('events', 'tracy', [1, 2],
                                                                tracy=dict(tracy, unmatched=3)), True))
        self.assertIn('did not start', mod.tracy_problem(record('events', 'off', [1, 2]), False))
        unconnected = dict(tracy, connected=False, connected_at_end=False)
        board = self.measure(FakeRunner(self.MEDIANS, {('tracy', 1): (0, json.dumps(record(
            'alloc', 'tracy', [1, 2, 3, 4, 5], tracy=dict(unconnected, unmatched=1))), '')}), workloads=['alloc'])
        self.assertEqual(board['workloads']['alloc']['failures'][0]['arm'], 'tracy')

    def test_a_connected_arm_needs_the_tools(self):
        with self.assertRaises(mod.OverheadError):
            self.measure(FakeRunner(self.MEDIANS), tracy_tools=None)

    def test_the_capture_summary_is_read_with_its_digit_grouping(self):
        text = 'Connecting to 127.0.0.1:8086...\nFrames: 11\nTime span: 4.22 ms\nZones: 1,234,567\n'
        self.assertEqual(mod.capture_number(text, 'Zones:'), 1234567)
        self.assertEqual(mod.capture_number(text, 'Frames:'), 11)
        self.assertIsNone(mod.capture_number(text, 'Messages:'))

    def test_tools_whose_digests_are_not_the_pinned_ones_are_refused(self):
        mod.tracy_tools = self.tools
        with tempfile.TemporaryDirectory() as raw:
            for name in ('tracy-capture.exe', 'tracy-csvexport.exe'):
                (Path(raw) / name).write_bytes(b'not the release')
            with self.assertRaises(ValueError): # pins.PinError
                mod.tracy_tools(raw, raw)


class FakeCachegrind:
    """Writes each run's cachegrind file: a setup cost plus a per-frame cost per arm (`frame`, instructions; the
    misses a hundredth of them in every column), the instructions moved by `wobble` on every second repeat;
    `special` maps (arm, frames, repeat) to (code, stdout) without a file."""
    SETUP = 5000000

    def __init__(self, frame, wobble=0, special=None):
        self.frame = frame
        self.wobble = wobble
        self.special = special or {}
        self.seen = {}

    def __call__(self, command, timeout, env=None):
        exe = command[command.index('--workload') - 1]
        mode = command[command.index('--mode') + 1]
        arm = {'compiled-out': 'compiled-out', 'before': 'before-off'}.get(exe, mode)
        arm = 'tracy-connected' if '--tracy-connect' in command else arm
        frames = int(command[command.index('--iterations') + 1])
        repeat = self.seen.get((arm, frames), 0)
        self.seen[(arm, frames)] = repeat + 1
        if (arm, frames, repeat) in self.special:
            return self.special[(arm, frames, repeat)]
        output = Path(next(c for c in command if c.startswith('--cachegrind-out-file=')).split('=', 1)[1])
        instructions = self.SETUP + self.frame[arm] * frames + (self.wobble if repeat % 2 else 0)
        misses = ' '.join([str(instructions // 100)] * 8)
        output.write_text('desc: I1 cache\ncmd: x\nevents: Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw\nfl=a\n'
                          'summary: %d %s\n' % (instructions, misses), encoding='utf-8')
        tracy = None if mode != 'tracy' else {'started': True, 'connected': arm == 'tracy-connected',
                                              'connected_at_end': arm == 'tracy-connected', 'unmatched': 0}
        return 0, json.dumps(record('offline', mode, [100] * frames, captures=0, warmup=20, tracy=tracy,
                                    cpu_level=selection(env))) + '\n', ''


class CountArgs:
    def __init__(self, scratch, **kw):
        self.exe = 'measured'
        self.baseline_exe = 'compiled-out'
        self.workloads = ['offline']
        self.frames = [100, 300]
        self.warmup = 20
        self.threads = 1
        self.repeats = 2
        self.valgrind = 'valgrind'
        self.timeout = 5.0
        self.scratch = scratch
        self.label = 'test'
        self.progress = False
        self.__dict__.update(kw)


class Instructions(unittest.TestCase):
    def count(self, runner, **kw):
        with tempfile.TemporaryDirectory() as scratch:
            return mod.count(CountArgs(scratch, **kw), runner=runner)

    def test_a_cachegrind_summary_sums_into_the_counted_events(self):
        totals = mod.parse_cachegrind('events: Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw\nsummary: 10 1 2 3 4 5 6 7 8\n')
        self.assertEqual(totals, {'instructions': 10, 'L1 misses': 12, 'LL misses': 15})
        for broken in ('events: Ir I1mr\n', 'events: Ir\nsummary: 1 2\n', ''):
            with self.assertRaises(mod.OverheadError):
                mod.parse_cachegrind(broken)

    def test_setup_cancels_and_the_instructions_decide(self):
        board = self.count(FakeCachegrind({'compiled-out': 10000000, 'off': 10050000}))
        entry = board['workloads']['offline']
        self.assertEqual(entry['per_frame']['off']['instructions'], (10050000.0, 10050000.0))
        self.assertAlmostEqual(entry['disabled']['instructions']['low'], 0.005, places=9)
        self.assertEqual(entry['disabled']['verdict'], 'within')
        over = self.count(FakeCachegrind({'compiled-out': 10000000, 'off': 10200000}))
        self.assertEqual(over['workloads']['offline']['disabled']['verdict'], 'over')

    def test_a_count_that_moves_between_repeats_widens_the_interval(self):
        board = self.count(FakeCachegrind({'compiled-out': 10000000, 'off': 10090000}, wobble=400000000))
        disabled = board['workloads']['offline']['disabled']
        self.assertLess(disabled['instructions']['low'], 0.01)
        self.assertGreater(disabled['instructions']['high'], 0.01)
        self.assertEqual(disabled['verdict'], 'unresolved')

    def test_a_failed_run_is_an_instrument_failure_and_takes_the_verdict(self):
        frame = {'compiled-out': 10000000, 'off': 10000000}
        for special in ({('off', 300, 1): (3, '', 'valgrind: unhandled instruction')},
                        {('compiled-out', 100, 0): (0, 'no record', '')}):
            board = self.count(FakeCachegrind(frame, special=special))
            entry = board['workloads']['offline']
            self.assertEqual(len(entry['failures']), 1, special)
            self.assertEqual(entry['disabled'], {}, special)
            self.assertIn('instrument failure', mod.render_counts(board))

    def test_a_change_is_counted_against_the_build_before_it(self):
        frame = {'compiled-out': 10000000, 'off': 9000000, 'before-off': 10000000}
        board = self.count(FakeCachegrind(frame), before_exe='before')
        entry = board['workloads']['offline']
        self.assertAlmostEqual(entry['change']['instructions']['low'], -0.1, places=9)
        self.assertEqual(entry['change']['verdict'], 'reported')
        self.assertEqual(entry['disabled']['verdict'], 'within', 'off below compiled-out is within the budget')
        self.assertIn('| offline | off vs before-off | -10.00% | -10.00% | -10.00% | reported |',
                      mod.render_counts(board))
        alone = self.count(FakeCachegrind(frame), baseline_exe=None, before_exe='before')
        self.assertEqual(alone['workloads']['offline']['disabled'], {}, 'no compiled-out arm, no disabled verdict')
        self.assertEqual(sorted(alone['workloads']['offline']['per_frame']), ['before-off', 'off'])
        self.assertEqual(self.count(FakeCachegrind(frame))['workloads']['offline']['change'], {})
        self.assertEqual(mod.main(['instructions', '--exe', __file__, '--scratch', '.', '--label', 'x',
                                   '--output', 'unused.json']), 2, 'nothing to compare with')

    def test_the_peer_arms_are_counted_with_their_instructions_per_event(self):
        FakeCapture.started = []
        FakeCapture.problems = {}
        frame = {'off': 1000000, 'full': 1409600, 'tracy': 1004096, 'tracy-connected': 1307200}
        tools = mod.tracy_tools
        mod.tracy_tools = lambda source, scratch: {'capture': 'capture', 'csvexport': 'csvexport'}
        try:
            with tempfile.TemporaryDirectory() as scratch:
                board = mod.count(CountArgs(scratch, workloads=['events'], baseline_exe=None, tracy_tools='unused',
                                            arms=['off', 'full', 'tracy', 'tracy-connected']),
                                  runner=FakeCachegrind(frame), capture=FakeCapture)
        finally:
            mod.tracy_tools = tools
        peers = board['workloads']['events']['peers']
        self.assertEqual(sorted(peers), ['full', 'full-vs-tracy-connected', 'tracy', 'tracy-connected'])
        self.assertEqual(peers['full-vs-tracy-connected']['per_event'], {'low': 25.0, 'high': 25.0})
        self.assertEqual(peers['tracy']['per_event'], {'low': 1.0, 'high': 1.0})
        self.assertAlmostEqual(peers['full']['instructions']['low'], 0.4096, places=9)
        self.assertEqual(len(FakeCapture.started), 4,
                         'a client for each connected count (two frame counts, two repeats)')
        text = mod.render_counts(board)
        self.assertIn('| events | full vs tracy-connected | +7.83% | +7.83% | +7.83% | reported; 25 instructions per '
                      'event |', text)
        self.assertTrue(all(line.count('|') == 7 for line in text.splitlines() if line.startswith('|')))

    def test_render_is_stable_and_names_the_verdict(self):
        board = self.count(FakeCachegrind({'compiled-out': 10000000, 'off': 10008000}))
        text = mod.render_counts(board)
        self.assertEqual(text, mod.render_counts(json.loads(json.dumps(board))))
        self.assertIn('| offline | off vs compiled-out | +0.08% | +0.08% | +0.08% | within (1%) |', text)
        self.assertTrue(all(line.count('|') == 7 for line in text.splitlines() if line.startswith('|')))


if __name__ == '__main__':
    unittest.main()
