#!/usr/bin/env python3
"""DIAG.11b clause (8): independent external sampling of the overhead workloads (Windows WPR and xperf).

The overhead harness (`crd-diag-overhead`, tests/support/diag-overhead) times its own frames. This runbook asks an
external sampler where the CPU went in the same processes, so a mode's measured cost is checked against the samples
that land in the diagnostic mechanisms (the allocator statistics, the allocator watch, the flight recorder, the
diagnostic host, the profiler and the CEIR profiling bridge).

  record   (an elevated shell: WPR's kernel sampler needs it) one WPR CPU session with stacks; inside it every arm of
           every workload runs `--repetitions` times as its own process, the arms interleaved and rotated per
           repetition, each process's PID, wall time and own JSON record kept in `manifest.json`. The trace and both
           binaries' PDBs stay in the run directory (a later rebuild cannot orphan the trace); `analyze` runs at once.
  analyze  (no elevation) streams `xperf -a dumper -stacktimeshifting` of the trace, counts every CPU sample of each
           recorded process and the samples whose deciding frame is a diagnostic one (by class), and writes
           `analysis.json`.

A sample's deciding frame is the leaf-most frame of the program's own image that is neither `std::` nor a C runtime
function compiled into the image (`snprintf`, `memcpy`: RUNS_FOR_CALLER; they run on behalf of their caller, as the
system modules do), and a frame is classified by its own function only. xperf's `-add_inline` lists every function
inlined anywhere into a frame's function, not the inline chain at the sampled address; with it, and with any diagnostic
ancestor claiming a sample, `main` and the workload code under the CEIR bridge and the jobs adapter fell into classes
(2026-10-23). For a diagnostic sample the next non-diagnostic frame of the image is its call site (the CEIR executor's
`run_seq` for the bridge's per-op hooks).

Per workload and arm the analysis reports the samples, the CPU milliseconds per frame (samples times the sampling
interval over the timed frames) and the diagnostic share. Each row compares two cells (an arm over `off`; `off` over
`compiled-out`) twice, as of 2026-10-25:

- magnitude: the external cost is the ratio of the sampled CPU per frame; the measured cost is the harness's own CPU
  per frame (`cpu.timed` over the frames, kept in the manifest) when both cells recorded it, else the median frame
  ratio when both cells' frames run on one thread (sampled CPU over the frame time at most SERIAL_LIMIT), else it is
  not compared: a multi-threaded frame's CPU is not its wall time (jobs full read 14% faster in frame time and 7.5%
  cheaper in sampled CPU at eight threads, 2026-10-25).
- location, same binary (basic, full): the added CPU per frame of the diagnostic frames and the growth of their call
  sites, over the reference's CPU per frame, must explain the external cost. The earlier estimate
  (1 - share_off) / (1 - share_arm) - 1 normalised the diagnostic samples by the arm's own non-diagnostic time, so it
  missed every cost the mode adds outside its frames: offline full +134.5% against +252.6% sampled, the rest in the
  executor's hook call sites.
- location, two binaries (off over compiled-out): two separately linked executables differ in layout and in the
  force-inlined gates a frame sampler leaves in their hosts (the allocator statistics gate in
  `TlsfAllocator::try_allocate`), so their frame-time gap is judged by the instruction count (`diag-overhead.py
  instructions`), not here. What the samples can judge is that off runs no out-of-line diagnostic code: its added
  diagnostic CPU per frame over compiled-out must be within AGREE_ABSOLUTE of none. The gap's location is reported.

Two costs agree when they differ by at most AGREE_ABSOLUTE plus AGREE_RELATIVE of the one compared against. Each
process's frames must cover at least FRAME_FRACTION of its wall time, and a cell's processes must agree on their median
frame within SPREAD_LIMIT, or the comparison is unresolved (setup would dilute the samples; jobs basic's two processes
read 393 and 471 us at eight threads on sixteen logical processors).

Verdicts (one line each; the exit code is the worst):
  AGREES          (0)  every resolved comparison agrees
  DISAGREES       (6)  a mode's measured cost is not explained by its diagnostic samples (a finding, recorded)
  UNRESOLVED      (4)  too few samples (MIN_SAMPLES) or frames that do not dominate their process
  UNREADABLE      (5)  the trace held no sampled-profile events for a recorded process (never read as zero)
  PERMISSION_DENIED (3) wpr could not start the kernel sampler (re-run from an elevated shell)
  TOOL_MISSING    (2)  wpr/xperf or the harness is missing, or a harness tree is not current with the checkout
  USAGE           (64) bad arguments

`record` runs every workload in the boards' shape (`diag-overhead.py run`: the jobs pool at THREADS, every process
pinned to AFFINITY, one logical processor of each core) and refuses, before the sampler starts, a harness whose build
tree is not current with the checkout (the glob check run for real, then a dry run of build.ninja and the target past
the always-dirty glob-verification edge: tree_current), so the two binaries come from one source; the manifest keeps
the checkout's commit and each harness record's CPU measurand.

The one command (an elevated PowerShell in the checkout, after building crd-diag-overhead on win-shipping-profile and
win-shipping):
  python scripts/sample-overhead-wpr.py record
Afterwards, unelevated:
  python scripts/sample-overhead-wpr.py analyze build/diag-overhead-wpr/<run>
Contract: docs/design/runtime-diagnostics.md#diag-11b; runbook: docs/recipes (external sampling).
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 'cerid-diag-overhead-wpr/1'
DEFAULT_EXE = ROOT / 'build/win-shipping-profile/tests/diag-overhead/crd-diag-overhead.exe'
DEFAULT_BASELINE = ROOT / 'build/win-shipping/tests/diag-overhead/crd-diag-overhead.exe'
DEFAULT_OUT = ROOT / 'build/diag-overhead-wpr'
WORKLOADS = ('alloc', 'jobs', 'offline', 'io')
ARMS = ('compiled-out', 'off', 'basic', 'full')
# Timed frames per process, sized so the frames take at least 3.2 s in the off arm on the 2026-10-19 capture's machine
# (alloc 83-92 us, jobs 370-417 us and offline 350-420 us per frame; io 592-596 us on 2026-10-24) and cover
# FRAME_FRACTION with a margin of the process: the basic arm's setup takes 0.35 s (it left alloc's 20,000 frames 84% of
# the process).
ITERATIONS = {'alloc': 40000, 'jobs': 9000, 'offline': 10000, 'io': 6000}
WARMUP = 50
# The boards' shape (diag-overhead.py run --threads 4 --affinity 5555): a four-thread jobs pool, one logical processor
# of each of eight cores. The 2026-10-25 trace ran the harness's default eight threads on 16 logical processors, where
# off spent 37% of its CPU in one compare-exchange of the fiber pool's free list.
THREADS = 4
AFFINITY = '5555'
INTERVAL_MS = 1.0  # WPR's CPU profile samples every 1 ms per processor
MIN_SAMPLES = 500
FRAME_FRACTION = 0.85
SPREAD_LIMIT = 0.02  # a cell's processes' median frames, highest over lowest
SERIAL_LIMIT = 1.25  # sampled CPU per frame over the median frame time of a frame that runs on one thread
AGREE_ABSOLUTE = 0.02
AGREE_RELATIVE = 0.25
TOP_ELSEWHERE = 3
VERDICT_CODES = {'AGREES': 0, 'TOOL_MISSING': 2, 'PERMISSION_DENIED': 3, 'UNRESOLVED': 4, 'UNREADABLE': 5,
                 'DISAGREES': 6, 'USAGE': 64}
# Diagnostic frame classes, matched against a frame's own `module!function` text. The first match decides a frame's
# class; DumperCounter says which frame decides a sample's.
CLASSES = (
    ('memory-stats', re.compile(r'MemoryStats::')),
    ('allocator-watch', re.compile(r'FlightAllocatorWatch')),
    ('flight-recorder', re.compile(r'Flight(?:Recorder|FrameWatch|CommandHost)')),
    ('diagnostic-host', re.compile(r'Diagnostic(?:Host|Recorder)')),
    ('ceir-bridge', re.compile(r'crd::ceir::host::(?:.*::)?(?:profiled_run|PlanProfile|adapter_pre|adapter_post)\b')),
    ('perf', re.compile(r'crd::perf::')),
)
# Functions of the image that run on behalf of their caller: the standard library and the C runtime's formatting and
# memory functions compiled into it (`snprintf` is an inline wrapper of the UCRT's; full mode's payload formatting at
# the sites in the jobs workload's job body decided 3,883 jobs full samples as `snprintf`, 2026-10-25), and crd-time's
# clocks, which a zone, a site or a frame mark reads as steady_clock was read before them (TickClock::now_ns and its
# re-anchor since 2026-10-26: the profiler's and the diagnostics' per-event clock, DIAG.11b clause (8)).
RUNS_FOR_CALLER = re.compile(r'^(?:std::|_*(?:v?s?n?printf(?:_s)?|v?f?printf|mem(?:cpy|move|set|cmp)|'
                             r'str(?:len|cmp|ncmp|cpy|ncpy|chr)|_*stdio_common_\w+)$|'
                             r"crd::time::(?:TickClock|MonotonicClock|CycleCounter|`anonymous namespace')::\w+$)")


# ---- recording ------------------------------------------------------------------------------------------------------

def file_identity(path):
    path = Path(path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return {'path': str(path), 'sha256': digest, 'bytes': path.stat().st_size,
            'modified': datetime.datetime.fromtimestamp(path.stat().st_mtime, datetime.timezone.utc).isoformat()}


def arm_order(arms, repetition):
    """Rotate the arms by the repetition, so every arm runs early and late once in the session."""
    shift = repetition % len(arms)
    return list(arms[shift:]) + list(arms[:shift])


def arm_command(exe, baseline, workload, arm, scratch, iterations):
    program = baseline if arm == 'compiled-out' else exe
    mode = 'off' if arm == 'compiled-out' else arm
    return [str(program), '--workload', workload, '--mode', mode, '--iterations', str(iterations),
            '--warmup', str(WARMUP), '--threads', str(THREADS), '--affinity', AFFINITY, '--scratch', str(scratch)]


def build_tree(exe):
    """The CMake build tree an executable was linked in (the nearest parent with a CMakeCache.txt); None outside one."""
    for parent in list(Path(exe).resolve().parents)[:5]:
        if (parent / 'CMakeCache.txt').is_file():
            return parent
    return None


def cache_value(tree, name):
    """One entry of a build tree's CMakeCache.txt; None when it is not there."""
    for line in (tree / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith(name + ':'):
            return line.split('=', 1)[1].strip() or None
    return None


def build_rule(statement):
    """The rule of a ninja `build` statement: the word after the first unescaped `:` (`D$:` is an escaped colon)."""
    match = re.search(r'(?<!\$):[ \t]*([A-Za-z0-9_.-]+)', statement)
    return match.group(1) if match else None


def without_glob_verification(text):
    """A CMake Ninja manifest's text without its glob-verification edge (rule VERIFY_GLOBS, its statement with any
    `$` line continuations and its indented bindings), and how many edges were removed.

    CMake gives every tree with CONFIGURE_DEPENDS globs the chain `build.ninja <- CMakeFiles/cmake.verify_globs <-
    CMakeFiles/VerifyGlobs.cmake_force`, the last a phony with no inputs and no file, so dirty in every dry run. The
    real build runs `cmake -P VerifyGlobs.cmake` with restat, which touches the stamp only when a glob's matches changed;
    a dry run cannot restat, so `ninja -n` of any target on such a tree reports `Re-checking globbed directories...`,
    `Re-running CMake...` and stops: a dry run that would regenerate the manifest exits at once, before it looks at the
    target. Without the edge the stamp is a plain input of build.ninja, judged by its time like every CMake input."""
    lines = text.split('\n')
    kept = []
    removed = 0
    index = 0
    while index < len(lines):
        line = lines[index]
        if not line.startswith('build '):
            kept.append(line)
            index += 1
            continue
        statement = [line]
        while (len(statement[-1].rstrip('\r')) - len(statement[-1].rstrip('\r').rstrip('$'))) % 2 == 1 and \
                index + len(statement) < len(lines):
            statement.append(lines[index + len(statement)])
        index += len(statement)
        if build_rule(' '.join(part.rstrip('\r').rstrip('$') for part in statement)) != 'VERIFY_GLOBS':
            kept.extend(statement)
            continue
        removed += 1
        while index < len(lines) and lines[index][:1] in (' ', '\t') and lines[index].strip():
            index += 1
    return '\n'.join(kept), removed


def tree_current(exe, subprocess_run):
    """None when `exe`'s tree has no work left for its target (it is the checkout's source as built), else why not.

    The check is the build's own first two steps without their effects: the glob check runs for real (`cmake -P
    CMakeFiles/VerifyGlobs.cmake`, as every build runs it; it touches the stamp only when a glob's matches changed), then
    ninja dry-runs build.ninja and the target from a copy of the manifest without the glob-verification edge
    (without_glob_verification). The copy is named by its absolute path, which is no node of the graph, so ninja does not
    first try to regenerate it; build.ninja is asked for by name instead, so a changed CMake input (or a changed glob,
    through the stamp) is work, as a changed source or header of the target is."""
    tree = build_tree(exe)
    if tree is None:
        return '%s is not in a CMake build tree' % exe
    make = cache_value(tree, 'CMAKE_MAKE_PROGRAM')
    if not make:
        return '%s names no CMAKE_MAKE_PROGRAM' % (tree / 'CMakeCache.txt')
    manifest = tree / 'build.ninja'
    if not manifest.is_file():
        return '%s has no build.ninja (the check reads a Ninja tree)' % tree
    target = Path(exe).stem
    verify = tree / 'CMakeFiles' / 'VerifyGlobs.cmake'
    with tempfile.TemporaryDirectory(prefix='crd-tree-current-') as scratch:
        dry_manifest = manifest
        if verify.is_file():
            cmake = cache_value(tree, 'CMAKE_COMMAND')
            if not cmake:
                return '%s names no CMAKE_COMMAND for its glob check' % (tree / 'CMakeCache.txt')
            try:
                globs = subprocess_run([cmake, '-P', str(verify)], capture_output=True, text=True, cwd=str(tree))
            except OSError as error:
                return 'cannot run %s: %s' % (cmake, error)
            if globs.returncode != 0:
                message = (globs.stderr or globs.stdout or '').strip().replace('\n', ' ')
                return 'the glob check of %s failed (rc=%d): %s' % (tree, globs.returncode, message)
            text, removed = without_glob_verification(manifest.read_bytes().decode('utf-8', 'surrogateescape'))
            if removed != 1:
                return ('%s has %d glob-verification edges, not one: the check knows no other manifest shape'
                        % (manifest, removed))
            dry_manifest = Path(scratch) / 'build.ninja'
            dry_manifest.write_bytes(text.encode('utf-8', 'surrogateescape'))
        command = [make, '-C', str(tree), '-f', str(dry_manifest.resolve()), '-n', '-d', 'explain', 'build.ninja',
                   target]
        try:
            dry = subprocess_run(command, capture_output=True, text=True)
        except OSError as error:
            return 'cannot run %s: %s' % (make, error)
    if dry.returncode != 0 or 'no work to do' not in (dry.stdout or ''):
        why = [line.split('explain: ', 1)[1] for line in (dry.stderr or '').splitlines() if 'explain: ' in line]
        work = [line for line in (dry.stdout or '').splitlines() if line.startswith('[')]
        return ('%s in %s is not current with the checkout (%s); build it before recording, so both binaries come '
                'from one source' % (target, tree, '; '.join((why or work or ['ninja rc=%d' % dry.returncode])[:3])))
    return None


def checkout_identity(subprocess_run):
    """The checkout's commit and whether tracked files differ from it (the source both trees were checked against)."""
    try:
        head = subprocess_run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True)
        status = subprocess_run(['git', '-C', str(ROOT), 'status', '--porcelain', '--untracked-files=no'],
                                capture_output=True, text=True)
    except OSError:
        return None
    return {'head': (head.stdout or '').strip() or None, 'modified': bool((status.stdout or '').strip())}


def run_process(command, timeout):
    """(pid, exit code, stdout, wall seconds) of one harness process."""
    started = time.perf_counter()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8',
                               errors='replace')
    try:
        out, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        out, _ = process.communicate()
        return process.pid, 124, out, time.perf_counter() - started
    return process.pid, process.returncode, out, time.perf_counter() - started


def harness_summary(stdout):
    """The frame count, the summed and the median frame time and the CPU over the timed frames (every thread's: the
    measurand a sampler's CPU is compared with) of a harness record; None when it is not one."""
    try:
        record = json.loads(stdout)
    except ValueError:
        return None
    samples = sorted(record.get('samples_ns') or [])
    if record.get('schema') != 'cerid-diag-overhead/1' or not samples:
        return None
    cpu = record.get('cpu') or {}
    return {'frames': len(samples), 'frame_seconds': sum(samples) / 1e9, 'median_ns': samples[len(samples) // 2],
            'cpu_timed': cpu.get('timed') if cpu.get('read') else None, 'cpu_unit': cpu.get('unit'),
            'threads': (record.get('host') or {}).get('threads'), 'build': record.get('build'),
            'executed': record.get('executed'), 'wrong': record.get('wrong')}


def record(args, runner=run_process, which=shutil.which, subprocess_run=subprocess.run):
    wpr, xperf = which('wpr'), which('xperf')
    if wpr is None or xperf is None:
        return verdict('TOOL_MISSING', 'wpr/xperf not on PATH (install the Windows Performance Toolkit)')
    exe = Path(args.exe)
    baseline = Path(args.baseline_exe) if args.baseline_exe else None
    arms = [a.strip() for a in args.arms.split(',') if a.strip()]
    if 'compiled-out' in arms and (baseline is None or not baseline.is_file()):
        return verdict('TOOL_MISSING', 'the compiled-out arm needs --baseline-exe (not built: %s)' % baseline)
    if not exe.is_file():
        return verdict('TOOL_MISSING', 'crd-diag-overhead not built: %s' % exe)
    for program in (exe, baseline if 'compiled-out' in arms else None):
        stale = tree_current(program, subprocess_run) if program is not None else None
        if stale:
            return verdict('TOOL_MISSING', stale)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    run_dir = Path(args.out) / stamp
    scratch = run_dir / 'scratch'
    scratch.mkdir(parents=True, exist_ok=False)
    etl = run_dir / 'overhead.etl'
    for name, program in (('profile', exe), ('compiled-out', baseline)):
        pdb = program.with_suffix('.pdb') if program else None
        if pdb is not None and pdb.is_file():
            (run_dir / 'symbols' / name).mkdir(parents=True)
            shutil.copy2(pdb, run_dir / 'symbols' / name / pdb.name)
    manifest = {'schema': SCHEMA, 'started': stamp, 'interval_ms': INTERVAL_MS, 'exe': file_identity(exe),
                'baseline_exe': file_identity(baseline) if baseline else None, 'arms': arms,
                'workloads': list(args.workloads.split(',')), 'repetitions': args.repetitions, 'trace': etl.name,
                'threads': THREADS, 'affinity': AFFINITY, 'source': checkout_identity(subprocess_run), 'runs': []}
    start = subprocess_run([wpr, '-start', 'CPU', '-filemode'], capture_output=True, text=True)
    if start.returncode != 0:
        message = (start.stderr or start.stdout or '').strip().replace('\n', ' ')
        shutil.rmtree(run_dir, ignore_errors=True)
        return verdict('PERMISSION_DENIED', 'wpr -start failed (rc=%d): %s; re-run from an elevated shell'
                       % (start.returncode, message))
    started = True
    try:
        for repetition in range(args.repetitions):
            for workload in manifest['workloads']:
                for arm in arm_order(arms, repetition):
                    command = arm_command(exe, baseline, workload, arm, scratch, ITERATIONS.get(workload, 5000))
                    pid, code, out, wall = runner(command, args.timeout)
                    manifest['runs'].append({'workload': workload, 'arm': arm, 'repetition': repetition, 'pid': pid,
                                             'exit_code': code, 'wall_seconds': round(wall, 3),
                                             'harness': harness_summary(out), 'command': command})
                    if args.progress:
                        print('  %s %s r%d pid %d exit %d %.1f s' % (workload, arm, repetition, pid, code, wall))
        stop = subprocess_run([wpr, '-stop', str(etl)], capture_output=True, text=True)
        started = False
        if stop.returncode != 0:
            message = (stop.stderr or stop.stdout or '').strip().replace('\n', ' ')
            return verdict('PERMISSION_DENIED', 'wpr -stop failed (rc=%d): %s' % (stop.returncode, message))
    finally:
        if started:
            subprocess_run([wpr, '-cancel'], capture_output=True, text=True)  # never orphan a kernel session
        (run_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print('recorded %s (%d processes); analysing (no elevation needed to repeat: analyze %s)'
          % (etl, len(manifest['runs']), run_dir))
    return analyze_run(run_dir, xperf=xperf)


# ---- analysis -------------------------------------------------------------------------------------------------------

PROCESS = re.compile(r'^(.*)\((\s*\d+)\)\s*$')


def column(header, name):
    """The index of a dumper column by its name, ignoring spaces (`Process Name ( PID)`); None when absent."""
    wanted = name.replace(' ', '').lower()
    for index, field in enumerate(header):
        if field.replace(' ', '').lower() == wanted:
            return index
    return None


INLINEES = re.compile(r'\s*\[.*$')  # the inlinee list `-add_inline` appends: not the sampled address's chain


def frame_class(frame):
    frame = INLINEES.sub('', frame)
    for name, pattern in CLASSES:
        if pattern.search(frame):
            return name
    return None


class DumperCounter:
    """Counts sampled-profile events per process from `xperf -a dumper -stacktimeshifting` lines, and the samples whose
    stack holds a diagnostic frame. The dumper writes, for every event type, a header line naming its columns
    (`SampledProfile, TimeStamp, Process Name ( PID), ...`); with -stacktimeshifting a sample's `Stack` lines follow
    it (same TimeStamp and ThreadID), leaf first, the frame text last on the line (it may contain commas).

    The leaf-most frame of the process's own image that does not run for its caller (RUNS_FOR_CALLER) decides a
    sample: system modules, the standard library and the C runtime's formatting run on behalf of their caller (the
    bridge's clock read, the recorder's lock, a payload's `snprintf`), and a diagnostic frame further up (the CEIR
    bridge's `profiled_run` around the program, the jobs adapter around a job) does not claim the workload code it
    calls. Per PID it also counts the deciding functions and, for a diagnostic sample, its call site: the next frame of
    the image above it that is not diagnostic and does not run for its caller."""

    def __init__(self, processes):
        self.images = {pid: image.lower() for pid, image in processes.items()}  # a PID counts only under its image
        self.pids = set(self.images)
        self.headers = {}
        self.samples = {pid: 0 for pid in self.pids}
        self.diagnostic = {pid: 0 for pid in self.pids}
        self.classes = {pid: {} for pid in self.pids}
        self.functions = {pid: {} for pid in self.pids}  # deciding function -> samples
        self.callers = {pid: {} for pid in self.pids}  # a diagnostic sample's call site -> samples
        # [pid, timestamp, tid, image prefix, decided, class of the deciding frame or None, call site pending]
        self.current = None
        self.stack_lines = 0

    def _finish(self):
        if self.current is not None:
            pid, cls = self.current[0], self.current[5]
            if cls is not None:
                self.diagnostic[pid] += 1
                self.classes[pid][cls] = self.classes[pid].get(cls, 0) + 1
        self.current = None

    def feed(self, line):
        fields = [f.strip() for f in line.split(',')]
        if len(fields) < 2:
            return
        kind = fields[0]
        if fields[1] == 'TimeStamp':
            self.headers[kind] = fields
            return
        if kind == 'SampledProfile':
            self._finish()
            header = self.headers.get(kind)
            process = column(header, 'Process Name ( PID)') if header else None
            if process is None or process >= len(fields):
                return
            match = PROCESS.match(fields[process])
            if not match:
                return
            pid = int(match.group(2))
            if pid not in self.pids or match.group(1).strip().lower() != self.images[pid]:
                return
            image = match.group(1).strip() + '!'
            self.samples[pid] += 1
            thread = column(header, 'ThreadID')
            tid = fields[thread] if thread is not None and thread < len(fields) else ''
            # The sampled instruction's own function, which may carry commas: everything between its column and the
            # columns after it.
            self.current = [pid, fields[1], tid, image, False, None, False]
            first = column(header, 'Image!Function')
            if first is not None:
                tail = len(header) - first - 1
                self._frame(','.join(fields[first:len(fields) - tail] if tail else fields[first:]))
            return
        if kind == 'Stack' and self.current is not None:
            header = self.headers.get(kind)
            if header is None:
                return
            timestamp = fields[1]
            thread = column(header, 'ThreadID')
            tid = fields[thread] if thread is not None and thread < len(fields) else ''
            if timestamp != self.current[1] or tid != self.current[2]:
                self._finish()
                return
            self.stack_lines += 1
            first = column(header, 'Image!Function')
            if first is not None:
                self._frame(','.join(fields[first:]))
            return
        self._finish()

    def _frame(self, frame):
        image = self.current[3]
        if not frame.startswith(image):
            return
        name = INLINEES.sub('', frame)
        if RUNS_FOR_CALLER.match(name[len(image):]):
            return
        pid = self.current[0]
        if not self.current[4]:
            self.current[4] = True
            self.current[5] = frame_class(name)
            self.current[6] = self.current[5] is not None
            self.functions[pid][name] = self.functions[pid].get(name, 0) + 1
            return
        if self.current[6] and frame_class(name) is None:
            self.current[6] = False
            self.callers[pid][name] = self.callers[pid].get(name, 0) + 1

    def close(self):
        self._finish()


def count_samples(lines, processes):
    counter = DumperCounter(processes)
    for line in lines:
        counter.feed(line)
    counter.close()
    return counter


def dumper_lines(xperf, etl, symbols):
    env = dict(os.environ)
    env['_NT_SYMBOL_PATH'] = ';'.join(str(s) for s in symbols)  # local symbols only, never a symbol server
    process = subprocess.Popen([xperf, '-i', str(etl), '-symbols', '-a', 'dumper', '-stacktimeshifting'],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding='utf-8',
                               errors='replace', env=env)
    try:
        for line in process.stdout:
            yield line
    finally:
        process.stdout.close()
        process.wait()


def median(values):
    ordered = sorted(values)
    if not ordered:
        return None
    middle = len(ordered) // 2
    return ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2.0


def tolerance(cost):
    return AGREE_ABSOLUTE + AGREE_RELATIVE * abs(cost)


def short_name(function):
    """`image!crd::ns::Type::method` -> `Type::method` for a verdict line (the analysis keeps the full names)."""
    name = function.split('!', 1)[-1]
    parts = [p for p in re.split(r'::(?![^<]*>)', name) if p and not p.startswith('`anonymous namespace')]
    return '::'.join(parts[-2:]) if len(parts) > 1 else name


def judge(manifest, counter, interval_ms=INTERVAL_MS):
    """Per (workload, arm): samples, CPU ms per frame, diagnostic share and classes; per row (an arm over off, off over
    compiled-out): the measured and the external cost, where the added CPU sits and a verdict. Returns (cells, verdict
    lines, worst verdict)."""
    cells = {}
    lines = []
    worst = 'AGREES'

    def worse(name):
        nonlocal worst
        if VERDICT_CODES[name] > VERDICT_CODES[worst]:
            worst = name

    pids = [run['pid'] for run in manifest['runs']]
    for run in manifest['runs']:
        key = (run['workload'], run['arm'])
        cell = cells.setdefault(key, {'samples': 0, 'diagnostic': 0, 'classes': {}, 'frames': 0, 'medians': [],
                                      'frame_fraction': [], 'unreadable': False, 'failed': False, 'functions': {},
                                      'callers': {}, 'cpu_timed': 0, 'cpu_units': [], 'cpu_missing': False})
        pid = run['pid']
        harness = run.get('harness')
        if run.get('exit_code') != 0 or harness is None or pids.count(pid) > 1:
            cell['failed'] = True
            continue
        samples = counter.samples.get(pid, 0)
        if samples == 0:
            cell['unreadable'] = True
        cell['samples'] += samples
        cell['diagnostic'] += counter.diagnostic.get(pid, 0)
        for into, source in ((cell['classes'], counter.classes), (cell['functions'], counter.functions),
                             (cell['callers'], counter.callers)):
            for name, count in source.get(pid, {}).items():
                into[name] = into.get(name, 0) + count
        cell['frames'] += harness['frames']
        cell['medians'].append(harness['median_ns'])
        cell['frame_fraction'].append(harness['frame_seconds'] / run['wall_seconds'] if run['wall_seconds'] else 0.0)
        if harness.get('cpu_timed') is None:
            cell['cpu_missing'] = True
        else:
            cell['cpu_timed'] += harness['cpu_timed']
            cell['cpu_units'].append(harness.get('cpu_unit'))
    for (workload, arm), cell in sorted(cells.items()):
        cell['share'] = cell['diagnostic'] / cell['samples'] if cell['samples'] else None
        cell['cpu_ms_per_frame'] = cell['samples'] * interval_ms / cell['frames'] if cell['frames'] else None
        cell['median_ns'] = median(cell['medians'])
        cell['min_frame_fraction'] = min(cell['frame_fraction']) if cell['frame_fraction'] else None
        cell['spread'] = max(cell['medians']) / min(cell['medians']) - 1.0 if cell['medians'] and min(
            cell['medians']) > 0 else None
        # threads busy per frame: the sampled CPU over the frame time (1 for a frame that runs on one thread)
        cell['parallelism'] = (cell['cpu_ms_per_frame'] * 1e6 / cell['median_ns']
                               if cell['cpu_ms_per_frame'] is not None and cell['median_ns'] else None)
        units = set(cell['cpu_units'])
        cell['harness_cpu_per_frame'] = (cell['cpu_timed'] / cell['frames'] if cell['frames'] and not
                                         cell['cpu_missing'] and len(units) == 1 else None)
        cell['harness_cpu_unit'] = units.pop() if len(units) == 1 else None
    for (workload, arm), cell in sorted(cells.items()):
        label = '%s %s' % (workload, arm)
        if cell['failed']:
            lines.append('UNRESOLVED: %s: a harness process failed, printed no record or shared its PID' % label)
            worse('UNRESOLVED')
            continue
        if cell['unreadable']:
            lines.append('UNREADABLE: %s: no sampled-profile events for a recorded process' % label)
            worse('UNREADABLE')
            continue
        if cell['samples'] < MIN_SAMPLES or (cell['min_frame_fraction'] or 0.0) < FRAME_FRACTION:
            lines.append('UNRESOLVED: %s: %d samples, frames cover %.0f%% of the process (need %d and %.0f%%)'
                         % (label, cell['samples'], 100.0 * (cell['min_frame_fraction'] or 0.0), MIN_SAMPLES,
                            100.0 * FRAME_FRACTION))
            worse('UNRESOLVED')
            continue
        reference = cells.get((workload, 'off'))
        usable = (reference is not None and not reference['failed'] and not reference['unreadable']
                  and reference['samples'] >= MIN_SAMPLES and reference.get('median_ns'))
        if arm == 'off' or not usable:
            lines.append('SAMPLED: %s: %d samples, %.3f CPU ms per frame, %.1f threads busy, diagnostic share %.1f%% %s'
                         % (label, cell['samples'], cell['cpu_ms_per_frame'], cell['parallelism'] or 0.0,
                            100.0 * cell['share'], json.dumps(cell['classes'], sort_keys=True)))
            continue
        cross = arm == 'compiled-out'
        # base: the cheaper side of the row (off for an arm, compiled-out for off); other: the side whose cost is read.
        base, other = (cell, reference) if cross else (reference, cell)
        what = 'off over compiled-out' if cross else '%s over off' % arm
        spread = [(name, c['spread']) for name, c in (('off', reference), (arm, cell))
                  if c['spread'] is not None and c['spread'] > SPREAD_LIMIT]
        if spread:
            lines.append('UNRESOLVED: %s: %s: the processes of %s disagree, their median frames %s apart (need at most '
                         '%.0f%%)' % (label, what, ' and '.join(n for n, _ in spread),
                                      ' and '.join('%.1f%%' % (100.0 * s) for _, s in spread), 100.0 * SPREAD_LIMIT))
            worse('UNRESOLVED')
            continue
        per_frame = base['samples'] / float(base['frames'])

        def growth(function, base=base, other=other, per_frame=per_frame):
            return (other['functions'].get(function, 0) / float(other['frames'])
                    - base['functions'].get(function, 0) / float(base['frames'])) / per_frame

        external = other['cpu_ms_per_frame'] / base['cpu_ms_per_frame'] - 1.0
        if base['harness_cpu_per_frame'] and other['harness_cpu_per_frame'] and \
                base['harness_cpu_unit'] == other['harness_cpu_unit']:
            measured, measurand = other['harness_cpu_per_frame'] / base['harness_cpu_per_frame'] - 1.0, 'CPU'
        elif max(base['parallelism'] or 0.0, other['parallelism'] or 0.0) <= SERIAL_LIMIT:
            measured, measurand = other['median_ns'] / base['median_ns'] - 1.0, 'frame time'
        else:
            measured, measurand = None, None
        diagnostic = (other['diagnostic'] / float(other['frames']) - base['diagnostic'] / float(base['frames'])) \
            / per_frame
        sites = 0.0 if cross else sum(max(0.0, growth(f)) for f in other['callers'])
        elsewhere = external - diagnostic - sites
        moved = sorted((f for f in set(base['functions']) | set(other['functions'])
                        if frame_class(f) is None and (cross or f not in other['callers'])),
                       key=lambda f: (-abs(growth(f)), f))[:TOP_ELSEWHERE]
        magnitude_ok = measured is None or abs(external - measured) <= tolerance(measured)
        location_ok = abs(diagnostic) <= AGREE_ABSOLUTE if cross else \
            abs(diagnostic + sites - external) <= tolerance(external)
        cell.update({'measured_cost': measured, 'measurand': measurand, 'external_cost': external,
                     'diagnostic_cost': diagnostic, 'call_site_cost': sites, 'elsewhere_cost': elsewhere,
                     'elsewhere': {f: growth(f) for f in moved}, 'call_sites': sorted(other['callers'])})
        name = 'AGREES' if magnitude_ok and location_ok else 'DISAGREES'
        worse(name)
        if measured is None:
            shown = 'measured %+.1f%% in frame time, not compared (%.1f threads busy per frame, no CPU measurand ' \
                    'recorded)' % (100.0 * (other['median_ns'] / base['median_ns'] - 1.0),
                                   max(base['parallelism'] or 0.0, other['parallelism'] or 0.0))
        else:
            shown = 'measured %+.1f%% (%s)' % (100.0 * measured, measurand)
        places = ', '.join('%s %+.1f%%' % (short_name(f), 100.0 * growth(f)) for f in moved)
        if cross:
            where = ('off runs %s out-of-line diagnostic code (%+.1f%% of compiled-out\'s CPU, need within %.0f%%); '
                     'the gap is the two binaries\' code (%s), judged by their instruction count'
                     % ('no' if location_ok else 'its', 100.0 * diagnostic, 100.0 * AGREE_ABSOLUTE, places))
        else:
            where = ('the diagnostic code %+.1f%% %s, its call sites %+.1f%% (%s), elsewhere %+.1f%% (%s)%s'
                     % (100.0 * diagnostic, json.dumps(cell['classes'], sort_keys=True), 100.0 * sites,
                        ', '.join(short_name(f) for f in sorted(other['callers'], key=lambda f: -growth(f))[:2])
                        or 'none', 100.0 * elsewhere, places,
                        '' if location_ok else ': the cost is not where the diagnostic code runs'))
        lines.append('%s: %s: %s %s, external %+.1f%% (sampled CPU per frame, %d against %d samples); %s'
                     % (name, label, what, shown, 100.0 * external, other['samples'], base['samples'], where))
    return cells, lines, worst


def analyze_run(run_dir, xperf=None, lines_source=None):
    run_dir = Path(run_dir)
    manifest_path = run_dir / 'manifest.json'
    if not manifest_path.is_file():
        return verdict('USAGE', 'no manifest.json in %s' % run_dir)
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    etl = run_dir / manifest['trace']
    if lines_source is None:
        xperf = xperf or shutil.which('xperf')
        if xperf is None:
            return verdict('TOOL_MISSING', 'xperf not on PATH (install the Windows Performance Toolkit)')
        if not etl.is_file():
            return verdict('USAGE', 'no trace %s' % etl)
        kept = sorted(str(d) for d in (run_dir / 'symbols').glob('*') if d.is_dir())  # the PDBs of record time first
        symbols = kept + sorted({str(Path(manifest[k]['path']).parent) for k in ('exe', 'baseline_exe')
                                 if manifest.get(k)})
        lines_source = dumper_lines(xperf, etl, symbols)
    counter = count_samples(lines_source, {run['pid']: Path(run['command'][0]).name for run in manifest['runs']})
    cells, lines, worst = judge(manifest, counter, manifest.get('interval_ms', INTERVAL_MS))
    analysis = {'schema': SCHEMA, 'trace': str(etl), 'stack_lines': counter.stack_lines, 'verdict': worst,
                'lines': lines, 'cells': {'%s %s' % key: cell for key, cell in sorted(cells.items())},
                'classes': [name for name, _ in CLASSES], 'agree_absolute': AGREE_ABSOLUTE,
                'agree_relative': AGREE_RELATIVE, 'min_samples': MIN_SAMPLES, 'frame_fraction': FRAME_FRACTION,
                'spread_limit': SPREAD_LIMIT, 'serial_limit': SERIAL_LIMIT, 'source': manifest.get('source'),
                'threads': manifest.get('threads'), 'affinity': manifest.get('affinity')}
    (run_dir / 'analysis.json').write_text(json.dumps(analysis, indent=2, default=str) + '\n', encoding='utf-8')
    for line in lines:
        print(line)
    if counter.stack_lines == 0 and any(counter.samples.values()):
        print('note: the trace decoded no stack lines; only the sampled instruction was attributed')
    return verdict(worst, 'analysis.json in %s' % run_dir)


def verdict(name, message):
    print('%s: %s' % (name, message))
    return VERDICT_CODES[name]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    rec = sub.add_parser('record', help='record the WPR trace of the overhead workloads (an elevated shell)')
    rec.add_argument('--exe', default=str(DEFAULT_EXE), help='crd-diag-overhead of the profiling build')
    rec.add_argument('--baseline-exe', default=str(DEFAULT_BASELINE),
                     help='crd-diag-overhead built with profiling compiled out (the compiled-out arm)')
    rec.add_argument('--workloads', default=','.join(WORKLOADS))
    rec.add_argument('--arms', default=','.join(ARMS))
    rec.add_argument('--repetitions', type=int, default=2)
    rec.add_argument('--timeout', type=float, default=300.0, help='seconds per process')
    rec.add_argument('--out', default=str(DEFAULT_OUT))
    rec.add_argument('--progress', action='store_true')
    ana = sub.add_parser('analyze', help='analyse a recorded run directory (no elevation)')
    ana.add_argument('run_dir')
    try:
        args = parser.parse_args(argv)
    except SystemExit as stop:
        return VERDICT_CODES['USAGE'] if stop.code else 0
    if sys.platform != 'win32':
        return verdict('USAGE', 'WPR and xperf are Windows tools; WSL2 has no perf (DIAG.11b clause 8)')
    if args.command == 'record':
        unknown = [w for w in args.workloads.split(',') if w not in WORKLOADS]
        bad_arms = [a for a in args.arms.split(',') if a not in ARMS]
        if unknown or bad_arms or args.repetitions < 1:
            return verdict('USAGE', 'workloads %s, arms %s, repetitions >= 1' % (','.join(WORKLOADS), ','.join(ARMS)))
        return record(args)
    return analyze_run(args.run_dir)


if __name__ == '__main__':
    sys.exit(main())
