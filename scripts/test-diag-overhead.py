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
failed run takes the verdict.
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


def record(workload, mode, samples, wrong=0, captures=3, delivered=None, resident=(1000, 1000), ring=0, cpu=None,
           warmup=1, validation='none', errors=0):
    """A process's record; `cpu` is its CPU per frame (cycles), by default its frame time (one busy thread), and
    False when it was not read."""
    per_frame = mod.median(samples) if cpu is None else (0 if cpu is False else cpu)
    return {'schema': mod.RECORD_SCHEMA, 'workload': workload, 'mode': mode, 'warmup': warmup,
            'iterations': len(samples), 'executed': len(samples) + warmup, 'wrong': wrong, 'samples_ns': samples,
            'cpu': {'read': cpu is not False, 'unit': 'cycles', 'timed': int(per_frame * len(samples))},
            'events': {'sites': 4, 'frames': 2, 'payloads': 0, 'recorded': 0, 'filtered': 6, 'profiler_drops': 0},
            'captures': {'requested': captures, 'delivered': captures if delivered is None else delivered,
                         'failures': 0, 'bytes_written': 100, 'longest_ns': 5},
            'memory': {'read': True, 'resident_before': resident[0], 'resident_frames': resident[1],
                       'resident_peak': resident[1], 'recorder_ring_bytes': ring},
            'validation': {'requested': validation} if validation == 'none' else {
                'requested': validation, 'errors': errors, 'warnings': 0, 'lost': 0, 'instrumentation_failures': 0},
            'lifetime': {}, 'host': {}, 'build': {}}


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

    def __call__(self, command, timeout):
        exe = command[0]
        mode = command[command.index('--mode') + 1]
        arm = 'compiled-out' if exe == 'compiled-out' else mode
        validation = command[command.index('--validation') + 1] if '--validation' in command else 'none'
        arm = arm if validation == 'none' else 'validation-' + validation
        repetition = sum(1 for c in self.calls if c == arm)
        self.calls.append(arm)
        if (arm, repetition) in self.special:
            return self.special[(arm, repetition)]
        # Jitter moves the reference and the candidates of one repetition in opposite directions, alternately.
        sign = 1.0 if (repetition + (arm == 'off')) % 2 else -1.0
        base = self.medians[arm] * (1.0 + sign * self.jitter)
        samples = [int(base) - 2, int(base) - 1, int(base), int(base) + 1, int(base) + 2]
        cpu = None if self.cpu is None else self.cpu[arm] * (1.0 + sign * self.cpu_jitter)
        ring = 155648 if arm in ('basic', 'full') else 0
        text = json.dumps(record('alloc', mode, samples, ring=ring, cpu=cpu, validation=validation))
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

    def test_render_is_stable_and_names_every_arm(self):
        board = self.measure(FakeRunner({'compiled-out': 100000, 'off': 100400, 'basic': 101000, 'full': 150000}))
        text = mod.render([board])
        self.assertEqual(text, mod.render([json.loads(json.dumps(board))]))
        for arm in ('compiled-out', 'off', 'basic', 'full', 'basic storage'):
            self.assertIn('| %s |' % arm, text)
        self.assertIn('within (2%)', text)
        self.assertIn('| 0.10 Mcycles |', text)
        self.assertTrue(all(line.count('|') == 14 for line in text.splitlines() if line.startswith('|')))


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

    def __call__(self, command, timeout):
        exe = command[command.index('--workload') - 1]
        arm = 'compiled-out' if exe == 'compiled-out' else 'off'
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
        return 0, json.dumps(record('offline', 'off', [100] * frames, captures=0, warmup=20)) + '\n', ''


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

    def test_render_is_stable_and_names_the_verdict(self):
        board = self.count(FakeCachegrind({'compiled-out': 10000000, 'off': 10008000}))
        text = mod.render_counts(board)
        self.assertEqual(text, mod.render_counts(json.loads(json.dumps(board))))
        self.assertIn('| offline | off vs compiled-out | +0.08% | +0.08% | +0.08% | within (1%) |', text)
        self.assertTrue(all(line.count('|') == 7 for line in text.splitlines() if line.startswith('|')))


if __name__ == '__main__':
    unittest.main()
