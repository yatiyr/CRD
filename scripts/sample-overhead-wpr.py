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

A sample's deciding frame is the leaf-most frame of the program's own image that is not `std::` (DumperCounter), and
a frame is classified by its own function only. xperf's `-add_inline` lists every function inlined anywhere into a
frame's function, not the inline chain at the sampled address; with it, and with any diagnostic ancestor claiming a
sample, `main` and the workload code under the CEIR bridge and the jobs adapter fell into classes (2026-10-23). A frame
sampler attributes force-inlined code, such as the allocator statistics gate, to its host function: off over
compiled-out is compared with off's out-of-line diagnostic samples, and the inlined gate's cost is the exact count of
DIAG.11b clause (1).

Per workload and arm the analysis reports the samples, the CPU milliseconds per frame (samples times the sampling
interval over the timed frames: the CPU-time measurand the jobs workload needs) and the diagnostic share. Against the
`off` arm it compares the harness's measured cost (the median frame ratio) with the external estimate
(1 - share_off) / (1 - share_arm) - 1. They agree when the two costs differ by at most AGREE_ABSOLUTE plus
AGREE_RELATIVE of the measured cost. Each process's frames must cover at least FRAME_FRACTION of its wall time, or the
comparison is unresolved (setup and teardown would dilute the share).

Verdicts (one line each; the exit code is the worst):
  AGREES          (0)  every resolved comparison agrees
  DISAGREES       (6)  a mode's measured cost is not explained by its diagnostic samples (a finding, recorded)
  UNRESOLVED      (4)  too few samples (MIN_SAMPLES) or frames that do not dominate their process
  UNREADABLE      (5)  the trace held no sampled-profile events for a recorded process (never read as zero)
  PERMISSION_DENIED (3) wpr could not start the kernel sampler (re-run from an elevated shell)
  TOOL_MISSING    (2)  wpr/xperf or the harness is missing
  USAGE           (64) bad arguments

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
import time

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 'cerid-diag-overhead-wpr/1'
DEFAULT_EXE = ROOT / 'build/win-shipping-profile/tests/diag-overhead/crd-diag-overhead.exe'
DEFAULT_BASELINE = ROOT / 'build/win-shipping/tests/diag-overhead/crd-diag-overhead.exe'
DEFAULT_OUT = ROOT / 'build/diag-overhead-wpr'
WORKLOADS = ('alloc', 'jobs', 'offline')
ARMS = ('compiled-out', 'off', 'basic', 'full')
# Timed frames per process, sized so the frames take at least 3.2 s in the off arm on the 2026-10-19 capture's machine
# (alloc 83-92 us, jobs 370-417 us and offline 350-420 us per frame) and cover FRAME_FRACTION with a margin of the
# process: the basic arm's setup takes 0.35 s (it left alloc's 20,000 frames 84% of the process).
ITERATIONS = {'alloc': 40000, 'jobs': 9000, 'offline': 10000}
WARMUP = 50
AFFINITY = 'FFFF'
INTERVAL_MS = 1.0  # WPR's CPU profile samples every 1 ms per processor
MIN_SAMPLES = 500
FRAME_FRACTION = 0.85
AGREE_ABSOLUTE = 0.02
AGREE_RELATIVE = 0.25
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
            '--warmup', str(WARMUP), '--affinity', AFFINITY, '--scratch', str(scratch)]


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
    """The frame count, the summed and the median frame time of a harness record; None when it is not one."""
    try:
        record = json.loads(stdout)
    except ValueError:
        return None
    samples = sorted(record.get('samples_ns') or [])
    if record.get('schema') != 'cerid-diag-overhead/1' or not samples:
        return None
    return {'frames': len(samples), 'frame_seconds': sum(samples) / 1e9, 'median_ns': samples[len(samples) // 2],
            'build': record.get('build'), 'executed': record.get('executed'), 'wrong': record.get('wrong')}


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
                'runs': []}
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

    The leaf-most frame of the process's own image that is not `std::` decides a sample: system modules and the
    standard library run on behalf of their caller (the bridge's clock read, the recorder's lock), and a diagnostic
    frame further up (the CEIR bridge's `profiled_run` around the program, the jobs adapter around a job) does not
    claim the workload code it calls."""

    def __init__(self, processes):
        self.images = {pid: image.lower() for pid, image in processes.items()}  # a PID counts only under its image
        self.pids = set(self.images)
        self.headers = {}
        self.samples = {pid: 0 for pid in self.pids}
        self.diagnostic = {pid: 0 for pid in self.pids}
        self.classes = {pid: {} for pid in self.pids}
        self.current = None  # [pid, timestamp, tid, image prefix, decided, class of the deciding frame or None]
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
            self.current = [pid, fields[1], tid, image, False, None]
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
        if self.current[4] or not frame.startswith(image) or frame[len(image):].startswith('std::'):
            return
        self.current[4] = True
        self.current[5] = frame_class(frame)

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


def judge(manifest, counter, interval_ms=INTERVAL_MS):
    """Per (workload, arm): samples, CPU ms per frame, diagnostic share and classes; against off: the measured and the
    external cost and a verdict. Returns (cells, verdict lines, worst verdict)."""
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
                                      'frame_fraction': [], 'unreadable': False, 'failed': False})
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
        for cls, count in counter.classes.get(pid, {}).items():
            cell['classes'][cls] = cell['classes'].get(cls, 0) + count
        cell['frames'] += harness['frames']
        cell['medians'].append(harness['median_ns'])
        cell['frame_fraction'].append(harness['frame_seconds'] / run['wall_seconds'] if run['wall_seconds'] else 0.0)
    for (workload, arm), cell in sorted(cells.items()):
        cell['share'] = cell['diagnostic'] / cell['samples'] if cell['samples'] else None
        cell['cpu_ms_per_frame'] = cell['samples'] * interval_ms / cell['frames'] if cell['frames'] else None
        cell['median_ns'] = median(cell['medians'])
        cell['min_frame_fraction'] = min(cell['frame_fraction']) if cell['frame_fraction'] else None
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
        if arm == 'off' or reference is None or reference.get('share') is None or not reference.get('median_ns'):
            lines.append('SAMPLED: %s: %d samples, %.3f CPU ms per frame, diagnostic share %.1f%% %s'
                         % (label, cell['samples'], cell['cpu_ms_per_frame'], 100.0 * cell['share'],
                            json.dumps(cell['classes'], sort_keys=True)))
            continue
        measured = cell['median_ns'] / reference['median_ns'] - 1.0
        if arm == 'compiled-out':
            # The reference is the faster arm here: off's cost over compiled-out, explained by off's own samples.
            measured = reference['median_ns'] / cell['median_ns'] - 1.0
            external = (1.0 - cell['share']) / (1.0 - reference['share']) - 1.0
        else:
            external = (1.0 - reference['share']) / (1.0 - cell['share']) - 1.0 if cell['share'] < 1.0 else float('inf')
        agree = abs(external - measured) <= AGREE_ABSOLUTE + AGREE_RELATIVE * abs(measured)
        cell['measured_cost'] = measured
        cell['external_cost'] = external
        name = 'AGREES' if agree else 'DISAGREES'
        worse(name)
        what = 'off over compiled-out' if arm == 'compiled-out' else '%s over off' % arm
        lines.append('%s: %s: %s measured %+.1f%%, external %+.1f%% (share %.1f%% against %.1f%%; %d samples) %s'
                     % (name, label, what, 100.0 * measured, 100.0 * external, 100.0 * cell['share'],
                        100.0 * reference['share'], cell['samples'], json.dumps(cell['classes'], sort_keys=True)))
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
                'agree_relative': AGREE_RELATIVE, 'min_samples': MIN_SAMPLES, 'frame_fraction': FRAME_FRACTION}
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
