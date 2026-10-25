#!/usr/bin/env python3
"""Verdict tests for scripts/sample-overhead-wpr.py (DIAG.11b clause 8, external sampling of the overhead workloads).

No WPR session is started: wpr, xperf and the harness are replaced by fakes, and the dumper text is written here in
the form `xperf -a dumper -stacktimeshifting` prints. Run: python scripts/test-sample-overhead-wpr.py
"""
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
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
                       'wrong': 0, 'build': {'config': 'Shipping'}, 'host': {'threads': 4},
                       'cpu': {'read': True, 'unit': 'cycles', 'timed': 123456}})


class Dumper(unittest.TestCase):
    def test_counts_samples_per_process_and_the_leaf_most_diagnostic_class(self):
        lines = list(HEADERS)
        exe = 'crd-diag-overhead.exe'
        # pid 100: a sample in the allocator statistics, one in a system lock the flight recorder took under the
        # allocator watch (the leaf-most frame of the program decides), one plain sample, one with a template comma,
        # one whose frames only list diagnostic functions among their inlinees (`-add_inline` lists a function's
        # inlinees, not the address's chain), one in the workload under the CEIR bridge (an ancestor never claims it)
        # and one in the standard clock the bridge's per-op hook reads.
        lines += sample(10, exe, 100, 7, exe + '!crd::memory::MemoryStats::on_allocate',
                        [exe + '!crd::memory::MemoryStats::on_allocate',
                         exe + '!crd::memory::TlsfAllocator::allocate [crd::memory::MemoryStats::on_allocate]',
                         exe + '!main'])
        lines += sample(11, exe, 100, 7, 'ntdll.dll!RtlEnterCriticalSection',
                        ['ntdll.dll!RtlEnterCriticalSection', exe + '!crd::perf::FlightRecorder::record',
                         exe + '!crd::perf::FlightAllocatorWatch::allocate', exe + '!main'])
        lines += sample(12, exe, 100, 7, exe + '!diag_overhead::alloc_frame', [exe + '!main'])
        lines += sample(13, exe, 100, 8, exe + '!crd::containers::Array<int, 4>::push_back',
                        [exe + '!crd::containers::Array<int, 4>::push_back', exe + '!main'])
        lines += sample(14, exe, 100, 7, exe + '!diag_overhead::OfflineWorkload::iterate [crd::perf::Profiler::begin]',
                        [exe + '!diag_overhead::OfflineWorkload::iterate [crd::memory::MemoryStats::on_resize]',
                         exe + '!main [crd::perf::FlightRecorder::record crd::perf::DiagnosticHost::poll]'])
        lines += ['CSwitch, 14, %s (  100), 7, 9, 0' % exe]
        bridge = exe + '!crd::ceir::host::profiled_run'
        lines += sample(17, exe, 100, 7, exe + "!crd::ceir::plan::`anonymous namespace'::run_seq",
                        [exe + "!crd::ceir::plan::`anonymous namespace'::run_seq", bridge, exe + '!main'])
        lines += sample(18, exe, 100, 7, exe + '!std::chrono::steady_clock::now',
                        [exe + '!std::chrono::steady_clock::now',
                         exe + "!crd::ceir::host::`anonymous namespace'::adapter_pre", bridge, exe + '!main'])
        # pid 200 is not recorded; pid 300 has a sample whose stack lines carry another thread (not its stack); another
        # program that reused pid 100 in the trace is not the harness.
        lines += sample(15, 'other.exe', 200, 1, 'other.exe!crd::perf::Profiler::begin', ['other.exe!main'])
        lines += sample(19, 'vcpkgsrv.exe', 100, 3, 'vcpkgsrv.exe!crd::perf::Profiler::begin', ['vcpkgsrv.exe!main'])
        lines += sample(16, exe, 300, 5, exe + '!main')
        lines += ['Stack, 16, 6, 1, 0x1, ' + exe + '!crd::perf::Profiler::begin']
        counter = wpr.count_samples(lines, {100: exe, 300: exe, 400: exe})
        self.assertEqual(counter.samples, {100: 7, 300: 1, 400: 0})
        self.assertEqual(counter.diagnostic, {100: 3, 300: 0, 400: 0})
        self.assertEqual(counter.classes[100], {'memory-stats': 1, 'flight-recorder': 1, 'ceir-bridge': 1})
        self.assertEqual(counter.stack_lines, 19)

    def test_the_sampled_function_alone_counts_and_columns_are_found_by_name(self):
        lines = ['SampledProfile, TimeStamp, ThreadID, Process Name (PID), Image!Function, Count',
                 'SampledProfile, 5, 9, app.exe (42), app.exe!crd::ceir::host::detail::profiled_run, 1']
        counter = wpr.count_samples(lines, {42: 'App.exe'})
        self.assertEqual((counter.samples[42], counter.diagnostic[42]), (1, 1))
        self.assertEqual(counter.classes[42], {'ceir-bridge': 1})
        self.assertEqual(wpr.count_samples(['SampledProfile, 5, 9, app.exe (42)'], {42: 'app.exe'}).samples[42], 0,
                         'without a header nothing is read, so nothing is counted')

    def test_frame_classes(self):
        self.assertEqual(wpr.frame_class('x!crd::memory::MemoryStats::on_deallocate'), 'memory-stats')
        self.assertEqual(wpr.frame_class('x!crd::perf::FlightAllocatorWatch::deallocate'), 'allocator-watch')
        self.assertEqual(wpr.frame_class('x!crd::perf::FlightFrameWatch::observe'), 'flight-recorder')
        self.assertEqual(wpr.frame_class('x!crd::perf::DiagnosticHost::poll'), 'diagnostic-host')
        self.assertEqual(wpr.frame_class('x!crd::perf::install_jobs_adapter'), 'perf')
        self.assertIsNone(wpr.frame_class('x!crd::jobs::Scheduler::run'))
        self.assertIsNone(wpr.frame_class('x!main [crd::memory::MemoryStats::on_allocate]'), 'an inlinee list')

    def test_the_c_runtime_runs_for_its_caller_and_a_diagnostic_sample_names_its_call_site(self):
        exe = 'crd-diag-overhead.exe'
        lines = list(HEADERS)
        # full mode's payload formatting at a site in the job body (the macro expands there): `snprintf` is the
        # UCRT's inline wrapper in the image and runs for the job body.
        lines += sample(1, exe, 100, 7, 'ucrtbase.dll!__stdio_common_vsprintf',
                        ['ucrtbase.dll!__stdio_common_vsprintf', exe + '!snprintf',
                         exe + "!diag_overhead::`anonymous namespace'::sum_slice", exe + '!main'])
        # the CEIR bridge's hook: the executor that calls it is its call site, not the bridge around the run
        lines += sample(2, exe, 100, 7, exe + "!crd::ceir::host::`anonymous namespace'::adapter_pre",
                        [exe + "!crd::ceir::host::`anonymous namespace'::adapter_pre",
                         exe + "!crd::ceir::plan::`anonymous namespace'::run_seq",
                         exe + '!crd::ceir::host::profiled_run', exe + '!main'])
        # a copy inside the recorder under the jobs adapter's callback: the recorder decides, and the call site is
        # the first frame above them that is not diagnostic
        lines += sample(3, exe, 100, 7, exe + '!memcpy',
                        [exe + '!memcpy', exe + '!crd::perf::FlightRecorder::record',
                         exe + "!crd::perf::`anonymous namespace'::cb_on_job_end",
                         exe + '!crd::jobs::detail::WorkerPool::run_job_in_fiber', exe + '!main'])
        counter = wpr.count_samples(lines, {100: exe})
        self.assertEqual(counter.functions[100], {
            exe + "!diag_overhead::`anonymous namespace'::sum_slice": 1,
            exe + "!crd::ceir::host::`anonymous namespace'::adapter_pre": 1,
            exe + '!crd::perf::FlightRecorder::record': 1})
        self.assertEqual(counter.classes[100], {'ceir-bridge': 1, 'flight-recorder': 1})
        self.assertEqual(counter.callers[100], {
            exe + "!crd::ceir::plan::`anonymous namespace'::run_seq": 1,
            exe + '!crd::jobs::detail::WorkerPool::run_job_in_fiber': 1})
        self.assertIsNotNone(wpr.RUNS_FOR_CALLER.match('vsnprintf'))
        self.assertIsNone(wpr.RUNS_FOR_CALLER.match('fiber_switch'), "the image's own assembly decides")
        self.assertIsNone(wpr.RUNS_FOR_CALLER.match('main'))


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
    counter = wpr.DumperCounter({run['pid']: 'crd-diag-overhead.exe' for run in manifest_doc['runs']})
    for pid, key in mapping.items():
        counter.samples[pid] = samples
        counter.diagnostic[pid] = int(round(shares[key] * samples))
        counter.classes[pid] = {'flight-recorder': counter.diagnostic[pid]} if counter.diagnostic[pid] else {}
    return counter


EXE = 'crd-diag-overhead.exe!'
ALLOCATE = EXE + 'crd::memory::TlsfAllocator::try_allocate'
RUN_SEQ = EXE + "crd::ceir::plan::`anonymous namespace'::run_seq"
HOOK = EXE + "crd::ceir::host::`anonymous namespace'::adapter_pre"
RECORD = EXE + 'crd::perf::FlightRecorder::record'
ACQUIRE = EXE + 'crd::jobs::detail::FiberPool::acquire'


def cells_doc(specs, frames=1000):
    """A manifest and a counter for cells given as {(workload, arm): spec}; each cell is two processes that share the
    spec's samples, diagnostic samples, deciding functions and call sites. spec: median_ns (or medians, one per
    process), samples, diagnostic, functions {name: samples}, callers {name: samples}, cpu (per frame, or None)."""
    runs = []
    processes = {}
    pid = 2000
    for (workload, arm), spec in specs.items():
        for repetition in range(2):
            medians = spec.get('medians') or [spec['median_ns']] * 2
            cpu = spec.get('cpu')
            runs.append({'workload': workload, 'arm': arm, 'repetition': repetition, 'pid': pid, 'exit_code': 0,
                         'wall_seconds': 2.6, 'harness': {
                             'frames': frames, 'frame_seconds': 2.5, 'median_ns': medians[repetition],
                             'cpu_timed': None if cpu is None else cpu * frames, 'cpu_unit': 'cycles'}})
            processes[pid] = spec
            pid += 1
    counter = wpr.DumperCounter({p: 'crd-diag-overhead.exe' for p in processes})
    for p, spec in processes.items():
        counter.samples[p] = spec['samples'] // 2
        counter.diagnostic[p] = spec.get('diagnostic', 0) // 2
        counter.classes[p] = {'flight-recorder': counter.diagnostic[p]} if counter.diagnostic[p] else {}
        counter.functions[p] = {name: count // 2 for name, count in spec.get('functions', {}).items()}
        counter.callers[p] = {name: count // 2 for name, count in spec.get('callers', {}).items()}
    return {'schema': wpr.SCHEMA, 'trace': 'overhead.etl', 'interval_ms': 1.0, 'runs': runs}, counter


class Judge(unittest.TestCase):
    def judged(self, specs, frames=1000):
        doc, counter = cells_doc(specs, frames)
        return wpr.judge(doc, counter)

    # The 2026-10-25 trace's alloc pair: off's frames are 10.0% longer than the separately linked compiled-out
    # binary's, its samples 8.4% more, none of them diagnostic; the gap sits in the function the statistics gate is
    # force-inlined into.
    ALLOC = {('alloc', 'compiled-out'): {'median_ns': 79250, 'samples': 6554, 'diagnostic': 4,
                                         'functions': {ALLOCATE: 3184}},
             ('alloc', 'off'): {'median_ns': 87200, 'samples': 7104, 'functions': {ALLOCATE: 3688}}}

    def test_two_binaries_are_judged_on_the_diagnostic_code_off_runs(self):
        cells, lines, worst = self.judged(self.ALLOC, frames=40000)
        self.assertEqual(worst, 'AGREES', lines)
        cell = cells[('alloc', 'compiled-out')]
        self.assertAlmostEqual(cell['measured_cost'], 87200 / 79250 - 1)
        self.assertEqual(cell['measurand'], 'frame time', 'one-thread frames: their CPU is their frame time')
        self.assertAlmostEqual(cell['external_cost'], 7104 / 6554 - 1)
        self.assertAlmostEqual(cell['diagnostic_cost'], -4 / 6554)
        text = '\n'.join(lines)
        self.assertIn('off runs no out-of-line diagnostic code', text)
        self.assertIn('TlsfAllocator::try_allocate +7.7%', text, 'the gap is located')
        self.assertIn('judged by their instruction count', text)

    def test_two_binaries_disagree_when_off_runs_diagnostic_code(self):
        specs = {key: dict(spec) for key, spec in self.ALLOC.items()}
        specs[('alloc', 'off')]['diagnostic'] = 568  # 8% of off's samples in an out-of-line diagnostic frame
        specs[('alloc', 'off')]['functions'] = {ALLOCATE: 3120, RECORD: 568}
        _, lines, worst = self.judged(specs, frames=40000)
        self.assertEqual(worst, 'DISAGREES', lines)
        self.assertIn('off runs its out-of-line diagnostic code (+8.6%', '\n'.join(lines))

    def test_a_cost_in_the_diagnostic_code_and_its_call_sites_agrees(self):
        # full doubles the CPU: half in the hooks, half in the executor that calls them (spills and reloads around two
        # calls per op); the hooks alone (+50%) do not explain +100%.
        specs = {('offline', 'off'): {'median_ns': 500000, 'samples': 1000, 'functions': {RUN_SEQ: 400}},
                 ('offline', 'full'): {'median_ns': 1000000, 'samples': 2000, 'diagnostic': 500,
                                       'functions': {RUN_SEQ: 900, HOOK: 500}, 'callers': {RUN_SEQ: 500}}}
        cells, lines, worst = self.judged(specs)
        self.assertEqual(worst, 'AGREES', lines)
        cell = cells[('offline', 'full')]
        self.assertAlmostEqual(cell['external_cost'], 1.0)
        self.assertAlmostEqual(cell['diagnostic_cost'], 0.5)
        self.assertAlmostEqual(cell['call_site_cost'], 0.5)
        self.assertAlmostEqual(cell['elsewhere_cost'], 0.0)
        self.assertIn('its call sites +50.0% (plan::run_seq)', '\n'.join(lines))
        # The same growth in a function that never calls the diagnostic code is not explained.
        specs[('offline', 'full')]['callers'] = {}
        _, lines, worst = self.judged(specs)
        self.assertEqual(worst, 'DISAGREES', lines)
        self.assertIn('the cost is not where the diagnostic code runs', '\n'.join(lines))

    def test_a_multithreaded_frame_is_compared_on_its_cpu(self):
        # Eight busy threads: frame time and CPU differ; the harness's CPU per frame is the measurand the samples
        # estimate. Frame time read -14% where the CPU rose 37%.
        specs = {('jobs', 'off'): {'median_ns': 440000, 'samples': 7238, 'cpu': 1.1e7,
                                   'functions': {ACQUIRE: 2000}},
                 ('jobs', 'full'): {'median_ns': 378400, 'samples': 9916, 'diagnostic': 2200, 'cpu': 1.507e7,
                                    'functions': {ACQUIRE: 2000, RECORD: 2200, RUN_SEQ: 478},
                                    'callers': {RUN_SEQ: 2200}}}
        cells, lines, worst = self.judged(specs)
        self.assertEqual(worst, 'AGREES', lines)
        cell = cells[('jobs', 'full')]
        self.assertEqual(cell['measurand'], 'CPU')
        self.assertAlmostEqual(cell['measured_cost'], 0.37)
        self.assertIn('full over off measured +37.0% (CPU)', '\n'.join(lines))
        for spec in specs.values():
            spec['cpu'] = None
        cells, lines, worst = self.judged(specs)
        self.assertIsNone(cells[('jobs', 'full')]['measured_cost'], 'no CPU measurand: frame time is not compared')
        self.assertIn('measured -14.0% in frame time, not compared', '\n'.join(lines))
        self.assertEqual(worst, 'AGREES', 'the location is still judged')

    def test_a_cost_the_samples_do_not_explain_disagrees(self):
        # The pool's contended free list (37% of off's CPU) falls as full's per-job work spaces its acquires: the
        # sampled CPU falls 7.5% while the diagnostic code adds 17%.
        specs = {('jobs', 'off'): {'median_ns': 440000, 'samples': 7238, 'cpu': 1.1e7,
                                   'functions': {ACQUIRE: 2700}},
                 ('jobs', 'full'): {'median_ns': 378400, 'samples': 6696, 'diagnostic': 1260, 'cpu': 1.0175e7,
                                    'functions': {ACQUIRE: 900, RECORD: 1260}}}
        cells, lines, worst = self.judged(specs)
        self.assertEqual(worst, 'DISAGREES', lines)
        self.assertIn('FiberPool::acquire -24.9%', '\n'.join(lines), 'the moved cost is named')

    def test_processes_that_disagree_are_unresolved(self):
        specs = {('jobs', 'off'): {'medians': [441400, 438000], 'samples': 7238},
                 ('jobs', 'basic'): {'medians': [393100, 470500], 'samples': 7300}}
        _, lines, worst = self.judged(specs)
        self.assertEqual(worst, 'UNRESOLVED', lines)
        self.assertIn('the processes of basic disagree, their median frames 19.7% apart', '\n'.join(lines))

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
        _, lines, worst = wpr.judge(none, wpr.DumperCounter({1000: 'a.exe', 1001: 'a.exe'}))
        self.assertEqual(worst, 'UNREADABLE', 'no samples for a recorded process is never a zero share')
        failed = manifest({('alloc', 'off'): 100}, {})
        failed['runs'][0]['exit_code'] = 1
        _, lines, worst = wpr.judge(failed, wpr.DumperCounter({1000: 'a.exe', 1001: 'a.exe'}))
        self.assertEqual(worst, 'UNRESOLVED')
        self.assertIn('UNRESOLVED: alloc off: a harness process failed', '\n'.join(lines))
        reused = manifest({('alloc', 'off'): 100}, {})
        reused['runs'][1]['pid'] = 1000
        self.assertEqual(wpr.judge(reused, counter_for(reused, {1000: ('alloc', 'off')}, {('alloc', 'off'): 0.0}))[2],
                         'UNRESOLVED', 'two recorded processes with one PID cannot be told apart')


# The manifest shapes CMake 4.3's Ninja generator writes for a tree with CONFIGURE_DEPENDS globs (excerpts of
# build/win-shipping-profile/build.ninja and ~/cerid-mirror-build/linux-gcc-shipping/build.ninja, 2026-10-25).
WINDOWS_MANIFEST = r"""# CMAKE generated file: DO NOT EDIT!
include CMakeFiles\rules.ninja

build tests\diag-overhead\crd-diag-overhead.exe: CXX_EXECUTABLE_LINKER__crd-diag-overhead_Release main.cpp.obj
  LINK_FLAGS = /machine:x64

#############################################
# Phony target to force glob verification run.

build D$:\b\CMakeFiles\VerifyGlobs.cmake_force: phony


#############################################
# Re-run CMake to check if globbed directories changed.

build D$:\b\CMakeFiles\cmake.verify_globs: VERIFY_GLOBS | D$:\b\CMakeFiles\VerifyGlobs.cmake_force
  pool = console
  restat = 1


#############################################
# Re-run CMake if any of its inputs changed.

build build.ninja D$:\b\cmake_install.cmake: RERUN_CMAKE D$:\b\CMakeFiles\cmake.verify_globs | ..\..\CMakeLists.txt
  pool = console
"""
LINUX_MANIFEST = """include CMakeFiles/rules.ninja

build /home/u/b/CMakeFiles/VerifyGlobs.cmake_force: phony

build /home/u/b/CMakeFiles/cmake.verify_globs: VERIFY_GLOBS | $
    /home/u/b/CMakeFiles/VerifyGlobs.cmake_force
  pool = console
  restat = 1

build build.ninja: RERUN_CMAKE /home/u/b/CMakeFiles/cmake.verify_globs | /home/u/s/CMakeLists.txt
  pool = console
"""
# What ninja 1.13.2 printed on build/win-shipping-profile at c4a56808, crd-diag-overhead built and nothing touched:
# a dry run that would regenerate the manifest exits before it looks at the target.
GLOB_CHAIN = '[0/2] Re-checking globbed directories...\n[1/2] Re-running CMake...\n'
GLOB_CHAIN_EXPLAIN = ('ninja explain: D:/b/CMakeFiles/VerifyGlobs.cmake_force is dirty\n'
                      'ninja explain: D:/b/CMakeFiles/cmake.verify_globs is dirty\n')
CURRENT = "ninja: Entering directory `D:/b'\nninja: no work to do.\n"
TOUCHED_SOURCE = ("ninja: Entering directory `D:/b'\n"
                  '[1/2] Building CXX object tests\\diag-overhead\\CMakeFiles\\crd-diag-overhead.dir\\main.cpp.obj\n'
                  '[2/2] Linking CXX executable tests\\diag-overhead\\crd-diag-overhead.exe\n')
TOUCHED_SOURCE_EXPLAIN = ('ninja explain: output tests/diag-overhead/CMakeFiles/crd-diag-overhead.dir/main.cpp.obj '
                          'older than most recent input D:/Dev/cerid/tests/support/diag-overhead/main.cpp '
                          '(8146394240954498 vs 8146400852579344)\n')
TOUCHED_HEADER_EXPLAIN = ('ninja explain: output engine/jobs/CMakeFiles/crd-jobs.dir/src/fiber_pool.cpp.obj older '
                          'than most recent input ../../engine/foundation/jobs/src/fiber_pool.hpp '
                          '(8146394078539297 vs 8146401006733599)\n')
TOUCHED_CMAKE = "ninja: Entering directory `D:/b'\n[0/1] Re-running CMake...\n"
TOUCHED_CMAKE_EXPLAIN = ('ninja explain: output build.ninja older than most recent input '
                         'D:/Dev/cerid/tests/support/diag-overhead/CMakeLists.txt (8146394041416084 vs 8146400858137685)\n')


def fake_run(calls, start_code=0, dry_run=CURRENT, explain='', globs_code=0):
    """subprocess.run for wpr, the glob check, the build tool's dry run and git; every command is kept in `calls`.

    Like the real tool, the fake ninja reports the glob chain (and nothing about the target) for a dry run of a manifest
    that still has the glob-verification edge, and `dry_run` only for one past it."""

    def run(command, **_):
        calls.append(list(command))
        if command[0] == 'git':
            return SimpleNamespace(returncode=0, stdout='0123abcd\n' if 'rev-parse' in command else '', stderr='')
        if command[0] == 'cmake':
            return SimpleNamespace(returncode=globs_code, stdout='', stderr='CMake Error: cannot read the glob')
        if command[0] == 'ninja':
            manifest = Path(command[command.index('-f') + 1]) if '-f' in command else None
            if manifest is None or ': VERIFY_GLOBS' in manifest.read_text(encoding='utf-8'):
                return SimpleNamespace(returncode=0, stdout=GLOB_CHAIN, stderr=GLOB_CHAIN_EXPLAIN)
            return SimpleNamespace(returncode=0, stdout=dry_run, stderr=explain)
        return SimpleNamespace(returncode=start_code if command[1] == '-start' else 0, stdout='',
                               stderr='Failed to enable the policy to profile system performance.')

    return run


def write_tree(temp, globs=True):
    """A CMake Ninja tree's files as tree_current reads them."""
    temp = Path(temp)
    (temp / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_MAKE_PROGRAM:FILEPATH=ninja\n'
                                         'CMAKE_COMMAND:INTERNAL=cmake\n', encoding='utf-8')
    (temp / 'build.ninja').write_text(WINDOWS_MANIFEST if globs else 'build build.ninja: RERUN_CMAKE | a\n',
                                      encoding='utf-8')
    if globs:
        (temp / 'CMakeFiles').mkdir(exist_ok=True)
        (temp / 'CMakeFiles/VerifyGlobs.cmake').write_text('file(GLOB NEW_GLOB "src/*.cpp")\n', encoding='utf-8')


class TreeCurrent(unittest.TestCase):
    def test_the_glob_verification_edge_alone_is_removed(self):
        for manifest in (WINDOWS_MANIFEST, LINUX_MANIFEST):
            text, removed = wpr.without_glob_verification(manifest)
            self.assertEqual(removed, 1)
            self.assertNotIn(': VERIFY_GLOBS', text)
            self.assertNotIn('restat = 1', text, "the edge's bindings go with it")
            self.assertNotIn('VerifyGlobs.cmake_force\n  pool', text, 'a continuation line goes with its statement')
            for kept in ('VerifyGlobs.cmake_force: phony', ': RERUN_CMAKE', 'include CMakeFiles'):
                self.assertIn(kept, text)
            self.assertEqual(text.count('pool = console'), 1, "build.ninja's own bindings stay")
        self.assertEqual(wpr.without_glob_verification('build a: phony\n'), ('build a: phony\n', 0))
        self.assertEqual(wpr.build_rule(r'build D$:\b\x.obj: CXX_COMPILER__crd D$:\s\x.cpp'), 'CXX_COMPILER__crd',
                         'an escaped drive colon is part of a path')

    def check(self, temp, **fake):
        calls = []
        return wpr.tree_current(Path(temp) / 'crd-diag-overhead.exe', fake_run(calls, **fake)), calls

    def test_a_tree_whose_only_work_is_the_glob_chain_is_current(self):
        with tempfile.TemporaryDirectory() as temp:
            write_tree(temp)
            why, calls = self.check(temp)
            self.assertIsNone(why)
            tree = Path(temp).resolve()
            self.assertEqual(calls[0], ['cmake', '-P', str(tree / 'CMakeFiles/VerifyGlobs.cmake')],
                             'the glob check runs for real first')
            self.assertEqual(calls[1][:4], ['ninja', '-C', str(tree), '-f'])
            self.assertEqual(calls[1][-5:], ['-n', '-d', 'explain', 'build.ninja', 'crd-diag-overhead'],
                             'build.ninja is asked for by name, so a changed CMake input is work')
            self.assertTrue(Path(calls[1][4]).is_absolute(), 'no node of the graph: ninja does not regenerate it')
            self.assertNotEqual(Path(calls[1][4]).parent, tree, 'the tree is not written')
            self.assertEqual((tree / 'build.ninja').read_text(encoding='utf-8'), WINDOWS_MANIFEST)
        with tempfile.TemporaryDirectory() as temp:
            write_tree(temp, globs=False)
            why, calls = self.check(temp)
            self.assertIsNone(why, 'a tree without globs is dry-run from its own manifest')
            self.assertEqual([c[0] for c in calls], ['ninja'])
            self.assertEqual(calls[0][4], str((Path(temp) / 'build.ninja').resolve()))

    def test_the_former_check_refused_every_current_tree(self):
        """The defect repaired: a plain `ninja -n <target>` (the 9254f216 check) sees only the glob chain."""
        plain = fake_run([])(['ninja', '-C', 'b', '-n', 'crd-diag-overhead'])
        self.assertNotIn('no work to do', plain.stdout)
        self.assertIn('Re-running CMake', plain.stdout)

    def test_real_work_is_refused(self):
        for dry_run, explain, expect in ((TOUCHED_SOURCE, TOUCHED_SOURCE_EXPLAIN, 'main.cpp.obj older than'),
                                         (TOUCHED_SOURCE, TOUCHED_HEADER_EXPLAIN, 'fiber_pool.hpp'),
                                         (TOUCHED_CMAKE, TOUCHED_CMAKE_EXPLAIN, 'build.ninja older than most recent'),
                                         (TOUCHED_SOURCE, '', '[1/2] Building CXX object')):
            with tempfile.TemporaryDirectory() as temp:
                write_tree(temp)
                why, _ = self.check(temp, dry_run=dry_run, explain=explain)
                self.assertIn('is not current with the checkout', why)
                self.assertIn(expect, why, 'the refusal names the work')
        with tempfile.TemporaryDirectory() as temp:
            write_tree(temp)
            why, calls = self.check(temp, globs_code=1)
            self.assertIn('the glob check of', why)
            self.assertEqual([c[0] for c in calls], ['cmake'], 'no dry run after a failed glob check')
            (Path(temp) / 'build.ninja').write_text(WINDOWS_MANIFEST + WINDOWS_MANIFEST, encoding='utf-8')
            self.assertIn('2 glob-verification edges', self.check(temp)[0], 'an unknown shape is refused')
            (Path(temp) / 'build.ninja').unlink()
            self.assertIn('has no build.ninja', self.check(temp)[0])


def real_tool(variable, name):
    """A build tool for the real-tree test: the CTest-given one (tests/CMakeLists.txt), else the one on PATH."""
    return os.environ.get(variable) or shutil.which(name)


class RealTree(unittest.TestCase):
    """tree_current on a tree CMake and ninja really generate and build, with a CONFIGURE_DEPENDS glob.

    Under CTest the tree's own CMake and ninja are given (CRD_TEST_CMAKE, CRD_TEST_NINJA), so it always runs there."""

    @classmethod
    def setUpClass(cls):
        cls.cmake = real_tool('CRD_TEST_CMAKE', 'cmake')
        cls.ninja = real_tool('CRD_TEST_NINJA', 'ninja')
        if not cls.cmake or not cls.ninja:
            raise unittest.SkipTest('cmake or ninja not found (CTest names both: CRD_TEST_CMAKE, CRD_TEST_NINJA)')

    def run_tool(self, *command):
        done = subprocess.run(list(command), capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return done

    def test_current_and_touched_trees(self):
        with tempfile.TemporaryDirectory() as temp:
            source, tree = Path(temp) / 'src', Path(temp) / 'build'
            (source / 'parts').mkdir(parents=True)
            (source / 'parts/a.txt').write_text('a\n', encoding='utf-8')
            (source / 'header.txt').write_text('h\n', encoding='utf-8')
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(probe NONE)\n'
                'file(GLOB parts CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/parts/*.txt")\n'
                'add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/probe.out"\n'
                '    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/parts/a.txt" "${CMAKE_BINARY_DIR}/probe.out"\n'
                '    DEPENDS ${parts} "${CMAKE_SOURCE_DIR}/header.txt")\n'
                'add_custom_target(crd-probe ALL DEPENDS "${CMAKE_BINARY_DIR}/probe.out")\n', encoding='utf-8')
            self.run_tool(self.cmake, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + self.ninja, '-S', str(source),
                          '-B', str(tree))
            self.run_tool(self.ninja, '-C', str(tree))
            exe = tree / 'crd-probe.exe'
            self.assertIsNone(wpr.tree_current(exe, subprocess.run), 'a built tree is current')
            plain = self.run_tool(self.ninja, '-C', str(tree), '-n', 'crd-probe')
            self.assertNotIn('no work to do', plain.stdout, 'the former check refused this current tree')
            for touched, newer_than in ((source / 'parts/a.txt', tree / 'probe.out'),
                                        (source / 'header.txt', tree / 'probe.out'),
                                        (source / 'CMakeLists.txt', tree / 'build.ninja')):
                before = touched.stat()
                later = newer_than.stat().st_mtime_ns + 5_000_000_000
                os.utime(touched, ns=(later, later))
                why = wpr.tree_current(exe, subprocess.run)
                self.assertIsNotNone(why, '%s touched: the tree has work' % touched.name)
                self.assertIn('not current with the checkout', why)
                os.utime(touched, ns=(before.st_atime_ns, before.st_mtime_ns))
                self.assertIsNone(wpr.tree_current(exe, subprocess.run), '%s restored: current' % touched.name)
            (source / 'parts/b.txt').write_text('b\n', encoding='utf-8')
            why = wpr.tree_current(exe, subprocess.run)
            self.assertIsNotNone(why, 'a new file a glob matches: CMake must re-run')
            self.assertIn('cmake.verify_globs', why, 'found by the real glob check')
            self.run_tool(self.ninja, '-C', str(tree))
            self.assertIsNone(wpr.tree_current(exe, subprocess.run), 'rebuilt: current')


def sampler_calls(calls):
    return [c[1] for c in calls if c[0] == 'wpr']


class Record(unittest.TestCase):
    def args(self, temp, **extra):
        exe = Path(temp) / 'crd-diag-overhead.exe'
        base = Path(temp) / 'base.exe'
        exe.write_bytes(b'profile')
        base.write_bytes(b'baseline')
        exe.with_suffix('.pdb').write_bytes(b'profile symbols')
        write_tree(temp)
        values = {'exe': str(exe), 'baseline_exe': str(base), 'workloads': 'alloc', 'arms': 'off,full',
                  'repetitions': 2, 'timeout': 5.0, 'out': str(Path(temp) / 'out'), 'progress': False}
        values.update(extra)
        return SimpleNamespace(**values)

    def test_a_denied_sampler_is_permission_denied_and_leaves_nothing(self):
        with tempfile.TemporaryDirectory() as temp:
            calls = []
            code = wpr.record(self.args(temp), runner=None, which=lambda name: name,
                              subprocess_run=fake_run(calls, start_code=0xc5585011))
            self.assertEqual(code, 3)
            self.assertEqual(sampler_calls(calls), ['-start'])
            self.assertEqual(list((Path(temp) / 'out').iterdir()), [], 'no run directory without a trace')

    def test_a_tree_not_current_with_the_checkout_is_refused_before_the_sampler(self):
        with tempfile.TemporaryDirectory() as temp:
            calls = []
            code = wpr.record(self.args(temp), runner=None, which=lambda name: name,
                              subprocess_run=fake_run(calls, dry_run='[1/2] Building CXX object x.obj\n'))
            self.assertEqual(code, 2)
            self.assertEqual(sampler_calls(calls), [], 'refused before the kernel sampler starts')
            dry = [c for c in calls if c[0] == 'ninja']
            self.assertEqual(len(dry), 1)
            self.assertEqual(dry[0][-5:], ['-n', '-d', 'explain', 'build.ninja', 'crd-diag-overhead'])
            self.assertFalse((Path(temp) / 'out').exists())
            calls.clear()
            args = self.args(temp, arms='compiled-out,off')
            code = wpr.record(args, runner=None, which=lambda name: name,
                              subprocess_run=fake_run(calls, start_code=0xc5585011))
            self.assertEqual(code, 3, 'both trees current: the sampler is asked (and the fake denies it)')
            self.assertEqual([c[-1] for c in calls if c[0] == 'ninja'], ['crd-diag-overhead', 'base'],
                             'the compiled-out binary is checked as well')
        with tempfile.TemporaryDirectory() as temp:
            args = self.args(temp)
            (Path(temp) / 'CMakeCache.txt').unlink()
            self.assertIn('is not in a CMake build tree', wpr.tree_current(args.exe, fake_run([])))

    def test_a_recorded_session_runs_every_arm_interleaved_and_keeps_its_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            calls = []
            processes = []

            def runner(command, timeout):
                processes.append(command)
                return 5000 + len(processes), 0, harness_record(100 if '--mode' in command else 0), 2.6

            with mock.patch.object(wpr, 'analyze_run', return_value=0) as analysed:
                code = wpr.record(self.args(temp), runner=runner, which=lambda name: name,
                                  subprocess_run=fake_run(calls))
            self.assertEqual(code, 0)
            self.assertEqual(sampler_calls(calls), ['-start', '-stop'], 'one session; never cancelled')
            modes = [p[p.index('--mode') + 1] for p in processes]
            self.assertEqual(modes, ['off', 'full', 'full', 'off'], 'arms rotate per repetition')
            for option, value in (('--affinity', '5555'), ('--threads', '4')):
                self.assertEqual(processes[0][processes[0].index(option) + 1], value, "the boards' shape")
            run_dir = analysed.call_args[0][0]
            saved = json.loads((run_dir / 'manifest.json').read_text(encoding='utf-8'))
            self.assertEqual([r['pid'] for r in saved['runs']], [5001, 5002, 5003, 5004])
            self.assertEqual(saved['runs'][0]['harness']['median_ns'], 100)
            self.assertEqual(saved['runs'][0]['harness']['cpu_timed'], 123456, 'the CPU measurand is kept')
            self.assertEqual(saved['runs'][0]['harness']['cpu_unit'], 'cycles')
            self.assertEqual(saved['source'], {'head': '0123abcd', 'modified': False})
            self.assertEqual((saved['threads'], saved['affinity']), (4, '5555'))
            self.assertEqual(saved['exe']['sha256'], hashlib.sha256(b'profile').hexdigest())
            self.assertEqual(saved['trace'], 'overhead.etl')
            self.assertEqual((run_dir / 'symbols/profile/crd-diag-overhead.pdb').read_bytes(), b'profile symbols',
                             'the PDB of record time stays with the trace')
            self.assertFalse((run_dir / 'symbols/compiled-out').exists(), 'a binary without a PDB keeps none')

    def test_an_interrupted_session_is_cancelled(self):
        with tempfile.TemporaryDirectory() as temp:
            calls = []

            def runner(command, timeout):
                raise KeyboardInterrupt

            with self.assertRaises(KeyboardInterrupt):
                wpr.record(self.args(temp), runner=runner, which=lambda name: name, subprocess_run=fake_run(calls))
            self.assertEqual(sampler_calls(calls), ['-start', '-cancel'], 'a kernel session is never left running')

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
                             'command': ['D:/b/crd-diag-overhead.exe', '--workload', 'alloc'],
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
        self.assertEqual(wpr.arm_command('p.exe', 'b.exe', 'jobs', 'off', 's', 6000)[9:13],
                         ['--threads', '4', '--affinity', '5555'], "the boards' shape")
        self.assertIsNone(wpr.harness_summary('not json'))
        self.assertIsNone(wpr.harness_summary(json.dumps({'schema': 'other'})))


if __name__ == '__main__':
    unittest.main()
