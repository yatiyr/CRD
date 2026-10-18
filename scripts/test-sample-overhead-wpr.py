#!/usr/bin/env python3
"""Verdict tests for scripts/sample-overhead-wpr.py (DIAG.11b clause 8, external sampling of the overhead workloads).

No WPR session is started: wpr, xperf and the harness are replaced by fakes, and the dumper text is written here in
the form `xperf -a dumper -stacktimeshifting -add_inline` prints. Run: python scripts/test-sample-overhead-wpr.py
"""
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
import hashlib
import importlib.util
import json
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('sample_overhead_wpr', ROOT / 'scripts/sample-overhead-wpr.py')
wpr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(wpr)

HEADERS = [
    '                   SampledProfile,  TimeStamp,     Process Name ( PID),   ThreadID,           PrgrmCtr, CPU, '
    'ThreadStartImage!Function, Image!Function, Count, SampledProfile type',
    '                            Stack,  TimeStamp,           ThreadID,        No.,            Address, Image!Function',
    '                          CSwitch,  TimeStamp, New Process Name ( PID),    New TID, NPri, NQnt',
]


def sample(timestamp, process, pid, tid, function, frames=()):
    lines = ['SampledProfile, %d, %s (%5d), %d, 0x00007ff6a0001000, 3, ntdll.dll!RtlUserThreadStart, %s, 1, Unbatched'
             % (timestamp, process, pid, tid, function)]
    for number, frame in enumerate(frames, 1):
        lines.append('Stack, %d, %d, %d, 0x00007ff6a00%05x, %s' % (timestamp, tid, number, number, frame))
    return lines


def harness_record(median_ns, frames=1000):
    return json.dumps({'schema': 'cerid-diag-overhead/1', 'samples_ns': [median_ns] * frames, 'executed': frames,
                       'wrong': 0, 'build': {'config': 'Shipping'}})


class Dumper(unittest.TestCase):
    def test_counts_samples_per_process_and_the_leaf_most_diagnostic_class(self):
        lines = list(HEADERS)
        exe = 'crd-diag-overhead.exe'
        # pid 100: a sample inside the allocator statistics (inlined into the allocator), one in the flight recorder
        # under the allocator watch (the leaf-most class wins), one plain sample, one with a template comma.
        lines += sample(10, exe, 100, 7, exe + '!crd::memory::TlsfAllocator::allocate',
                        [exe + '!crd::memory::TlsfAllocator::allocate [crd::memory::MemoryStats::on_allocate]',
                         exe + '!main'])
        lines += sample(11, exe, 100, 7, 'ntdll.dll!RtlEnterCriticalSection',
                        ['ntdll.dll!RtlEnterCriticalSection', exe + '!crd::perf::FlightRecorder::record',
                         exe + '!crd::perf::FlightAllocatorWatch::allocate', exe + '!main'])
        lines += sample(12, exe, 100, 7, exe + '!diag_overhead::alloc_frame', [exe + '!main'])
        lines += sample(13, exe, 100, 8, exe + '!crd::containers::Array<int, 4>::push_back',
                        [exe + '!crd::containers::Array<int, 4>::push_back', exe + '!main'])
        lines += ['CSwitch, 14, %s (  100), 7, 9, 0' % exe]
        # pid 200 is not recorded; pid 300 has a sample whose stack lines carry another thread (not its stack).
        lines += sample(15, 'other.exe', 200, 1, 'other.exe!crd::perf::Profiler::begin', ['other.exe!main'])
        lines += sample(16, exe, 300, 5, exe + '!main')
        lines += ['Stack, 16, 6, 1, 0x1, ' + exe + '!crd::perf::Profiler::begin']
        counter = wpr.count_samples(lines, [100, 300, 400])
        self.assertEqual(counter.samples, {100: 4, 300: 1, 400: 0})
        self.assertEqual(counter.diagnostic, {100: 2, 300: 0, 400: 0})
        self.assertEqual(counter.classes[100], {'memory-stats': 1, 'flight-recorder': 1})
        self.assertEqual(counter.stack_lines, 9)

    def test_the_sampled_function_alone_counts_and_columns_are_found_by_name(self):
        lines = ['SampledProfile, TimeStamp, ThreadID, Process Name (PID), Image!Function, Count',
                 'SampledProfile, 5, 9, app.exe (42), app.exe!crd::ceir::host::detail::profiled_run, 1']
        counter = wpr.count_samples(lines, [42])
        self.assertEqual((counter.samples[42], counter.diagnostic[42]), (1, 1))
        self.assertEqual(counter.classes[42], {'ceir-bridge': 1})
        self.assertEqual(wpr.count_samples(['SampledProfile, 5, 9, app.exe (42)'], [42]).samples[42], 0,
                         'without a header nothing is read, so nothing is counted')

    def test_frame_classes(self):
        self.assertEqual(wpr.frame_class('x!crd::memory::MemoryStats::on_deallocate'), 'memory-stats')
        self.assertEqual(wpr.frame_class('x!crd::perf::FlightAllocatorWatch::deallocate'), 'allocator-watch')
        self.assertEqual(wpr.frame_class('x!crd::perf::FlightFrameWatch::observe'), 'flight-recorder')
        self.assertEqual(wpr.frame_class('x!crd::perf::DiagnosticHost::poll'), 'diagnostic-host')
        self.assertEqual(wpr.frame_class('x!crd::perf::install_jobs_adapter'), 'perf')
        self.assertIsNone(wpr.frame_class('x!crd::jobs::Scheduler::run'))


def manifest(costs, shares_runs, frame_seconds=2.5, wall=2.6):
    runs = []
    pid = 1000
    for (workload, arm), median_ns in costs.items():
        for repetition in range(2):
            runs.append({'workload': workload, 'arm': arm, 'repetition': repetition, 'pid': pid, 'exit_code': 0,
                         'wall_seconds': wall, 'harness': {'frames': 1000, 'frame_seconds': frame_seconds,
                                                           'median_ns': median_ns}})
            shares_runs[pid] = (workload, arm)
            pid += 1
    return {'schema': wpr.SCHEMA, 'trace': 'overhead.etl', 'interval_ms': 1.0, 'runs': runs}


def counter_for(manifest_doc, mapping, shares, samples=1000):
    counter = wpr.DumperCounter([run['pid'] for run in manifest_doc['runs']])
    for pid, key in mapping.items():
        counter.samples[pid] = samples
        counter.diagnostic[pid] = int(round(shares[key] * samples))
        counter.classes[pid] = {'flight-recorder': counter.diagnostic[pid]} if counter.diagnostic[pid] else {}
    return counter


class Judge(unittest.TestCase):
    def test_a_cost_explained_by_its_diagnostic_samples_agrees(self):
        mapping = {}
        doc = manifest({('alloc', 'compiled-out'): 100, ('alloc', 'off'): 137, ('alloc', 'full'): 150}, mapping)
        # off's 37% over compiled-out sits in the allocator statistics (27% of off's samples); full adds 9% more.
        shares = {('alloc', 'compiled-out'): 0.0, ('alloc', 'off'): 0.27, ('alloc', 'full'): 0.33}
        cells, lines, worst = wpr.judge(doc, counter_for(doc, mapping, shares))
        self.assertEqual(worst, 'AGREES', lines)
        self.assertAlmostEqual(cells[('alloc', 'compiled-out')]['measured_cost'], 0.37)
        self.assertAlmostEqual(cells[('alloc', 'compiled-out')]['external_cost'], 1.0 / 0.73 - 1.0)
        self.assertAlmostEqual(cells[('alloc', 'full')]['external_cost'], 0.73 / 0.67 - 1.0)
        self.assertEqual(cells[('alloc', 'off')]['cpu_ms_per_frame'], 1.0, '2000 samples over 2000 frames')
        self.assertTrue(any(line.startswith('SAMPLED: alloc off') for line in lines))

    def test_a_cost_the_samples_do_not_explain_disagrees(self):
        mapping = {}
        doc = manifest({('jobs', 'off'): 400, ('jobs', 'full'): 800}, mapping)
        shares = {('jobs', 'off'): 0.0, ('jobs', 'full'): 0.05}
        _, lines, worst = wpr.judge(doc, counter_for(doc, mapping, shares))
        self.assertEqual(worst, 'DISAGREES')
        self.assertIn('full over off measured +100.0%, external +5.3%', '\n'.join(lines))

    def test_too_few_samples_short_frames_unreadable_and_failed_processes(self):
        mapping = {}
        doc = manifest({('alloc', 'off'): 100, ('alloc', 'full'): 110}, mapping)
        shares = {('alloc', 'off'): 0.0, ('alloc', 'full'): 0.1}
        _, lines, worst = wpr.judge(doc, counter_for(doc, mapping, shares, samples=100))
        self.assertEqual(worst, 'UNRESOLVED', 'too few samples')
        short = manifest({('alloc', 'off'): 100}, {}, frame_seconds=0.03, wall=0.16)
        self.assertEqual(wpr.judge(short, counter_for(short, {1000: ('alloc', 'off'), 1001: ('alloc', 'off')},
                                                      {('alloc', 'off'): 0.0}))[2], 'UNRESOLVED',
                         'frames covering 19% of the process are diluted by its setup')
        none = manifest({('alloc', 'off'): 100}, {})
        _, lines, worst = wpr.judge(none, wpr.DumperCounter([1000, 1001]))
        self.assertEqual(worst, 'UNREADABLE', 'no samples for a recorded process is never a zero share')
        failed = manifest({('alloc', 'off'): 100}, {})
        failed['runs'][0]['exit_code'] = 1
        _, lines, worst = wpr.judge(failed, wpr.DumperCounter([1000, 1001]))
        self.assertEqual(worst, 'UNRESOLVED')
        self.assertIn('UNRESOLVED: alloc off: a harness process failed', '\n'.join(lines))


class Record(unittest.TestCase):
    def args(self, temp, **extra):
        exe = Path(temp) / 'crd-diag-overhead.exe'
        base = Path(temp) / 'base.exe'
        exe.write_bytes(b'profile')
        base.write_bytes(b'baseline')
        values = {'exe': str(exe), 'baseline_exe': str(base), 'workloads': 'alloc', 'arms': 'off,full',
                  'repetitions': 2, 'timeout': 5.0, 'out': str(Path(temp) / 'out'), 'progress': False}
        values.update(extra)
        return SimpleNamespace(**values)

    def test_a_denied_sampler_is_permission_denied_and_leaves_nothing(self):
        with tempfile.TemporaryDirectory() as temp:
            calls = []

            def run(command, **_):
                calls.append(command)
                return SimpleNamespace(returncode=0xc5585011 if command[1] == '-start' else 0, stdout='',
                                       stderr='Failed to enable the policy to profile system performance.')

            code = wpr.record(self.args(temp), runner=None, which=lambda name: name, subprocess_run=run)
            self.assertEqual(code, 3)
            self.assertEqual(calls, [['wpr', '-start', 'CPU', '-filemode']])
            self.assertEqual(list((Path(temp) / 'out').iterdir()), [], 'no run directory without a trace')

    def test_a_recorded_session_runs_every_arm_interleaved_and_keeps_its_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            calls = []
            processes = []

            def run(command, **_):
                calls.append(command[:2])
                return SimpleNamespace(returncode=0, stdout='', stderr='')

            def runner(command, timeout):
                processes.append(command)
                return 5000 + len(processes), 0, harness_record(100 if '--mode' in command else 0), 2.6

            with mock.patch.object(wpr, 'analyze_run', return_value=0) as analysed:
                code = wpr.record(self.args(temp), runner=runner, which=lambda name: name, subprocess_run=run)
            self.assertEqual(code, 0)
            self.assertEqual([c[1] for c in calls], ['-start', '-stop'], 'one session; never cancelled')
            modes = [p[p.index('--mode') + 1] for p in processes]
            self.assertEqual(modes, ['off', 'full', 'full', 'off'], 'arms rotate per repetition')
            self.assertIn('--affinity', processes[0])
            run_dir = analysed.call_args[0][0]
            saved = json.loads((run_dir / 'manifest.json').read_text(encoding='utf-8'))
            self.assertEqual([r['pid'] for r in saved['runs']], [5001, 5002, 5003, 5004])
            self.assertEqual(saved['runs'][0]['harness']['median_ns'], 100)
            self.assertEqual(saved['exe']['sha256'], hashlib.sha256(b'profile').hexdigest())
            self.assertEqual(saved['trace'], 'overhead.etl')

    def test_an_interrupted_session_is_cancelled(self):
        with tempfile.TemporaryDirectory() as temp:
            calls = []

            def run(command, **_):
                calls.append(command[1])
                return SimpleNamespace(returncode=0, stdout='', stderr='')

            def runner(command, timeout):
                raise KeyboardInterrupt

            with self.assertRaises(KeyboardInterrupt):
                wpr.record(self.args(temp), runner=runner, which=lambda name: name, subprocess_run=run)
            self.assertEqual(calls, ['-start', '-cancel'], 'a kernel session is never left running')

    def test_missing_tools_and_harness(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual(wpr.record(self.args(temp), which=lambda name: None), 2)
            args = self.args(temp, exe=str(Path(temp) / 'absent.exe'))
            self.assertEqual(wpr.record(args, which=lambda name: name), 2)
            args = self.args(temp, arms='compiled-out,off', baseline_exe=str(Path(temp) / 'absent.exe'))
            self.assertEqual(wpr.record(args, which=lambda name: name), 2)

    def test_analysis_of_a_kept_run_needs_no_elevation(self):
        with tempfile.TemporaryDirectory() as temp:
            run_dir = Path(temp)
            doc = {'schema': wpr.SCHEMA, 'trace': 'overhead.etl', 'interval_ms': 1.0,
                   'runs': [{'workload': 'alloc', 'arm': 'off', 'repetition': 0, 'pid': 100, 'exit_code': 0,
                             'wall_seconds': 2.6, 'harness': {'frames': 1000, 'frame_seconds': 2.5,
                                                              'median_ns': 100}}]}
            (run_dir / 'manifest.json').write_text(json.dumps(doc), encoding='utf-8')
            lines = list(HEADERS)
            for t in range(600):
                lines += sample(t, 'crd-diag-overhead.exe', 100, 1, 'crd-diag-overhead.exe!main')
            self.assertEqual(wpr.analyze_run(run_dir, lines_source=lines), 0)
            analysis = json.loads((run_dir / 'analysis.json').read_text(encoding='utf-8'))
            self.assertEqual(analysis['cells']['alloc off']['samples'], 600)
            self.assertEqual(analysis['verdict'], 'AGREES')
            self.assertEqual(wpr.analyze_run(run_dir / 'nothing'), 64)
            with mock.patch.object(wpr.shutil, 'which', return_value=None):
                self.assertEqual(wpr.analyze_run(run_dir), 2)

    def test_command_line(self):
        self.assertEqual(wpr.main(['record', '--workloads', 'events']), 64, 'events is never judged; not sampled')
        self.assertEqual(wpr.main(['nonsense']), 64)
        self.assertEqual(wpr.arm_order(['a', 'b', 'c'], 1), ['b', 'c', 'a'])
        self.assertEqual(wpr.arm_command('p.exe', 'b.exe', 'jobs', 'compiled-out', 's', 6000)[:5],
                         ['b.exe', '--workload', 'jobs', '--mode', 'off'])
        self.assertIsNone(wpr.harness_summary('not json'))
        self.assertIsNone(wpr.harness_summary(json.dumps({'schema': 'other'})))


if __name__ == '__main__':
    unittest.main()
