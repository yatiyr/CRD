#!/usr/bin/env python3
"""Run the JOBS-HARD.3 ThreadSanitizer host-separation rounds on one machine and report every event.

Contract: docs/design/test-instruments.md (host separation). On the development host the linux-clang-tsan run of the
jobs streaming case ("fiber keep: while tiny jobs stream ...") faults in about one run in ten with impossible values
in -O0 getter chains, and so does a Cerid-free control of the same shape, also with no fiber at all, while the
unsanitized control is clean (docs/sessions/2026-10-25-jobs-fiber-pool-contention.md). This driver repeats that
exposure on another machine. It compiles the control (tests/support/diag/host-check/tsan_control.cpp) with
-fsanitize=thread and without, then runs interleaved rounds of four arms: the jobs case from a configured
linux-clang-tsan tree, the control on fibers, the control on plain threads and the unsanitized control. Every process
is pinned to two CPUs beside one busy loop pinned to each of them. An event is a nonzero exit, a timeout, a missing
pass line, or a sanitizer report or "=== WRONG" line in a log; the driver exits 1 when there was one. It builds only
the control.

    python3 scripts/tsan-host-check.py --build build/linux-clang-tsan --rounds 150 --scratch <dir> \
        --json <file> --summary "$GITHUB_STEP_SUMMARY"
"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTROL = ROOT / 'tests/support/diag/host-check/tsan_control.cpp'
SUPPRESSIONS = ROOT / 'tests/support/diag/tsan-suppressions.txt'
CASE = 'fiber keep: while tiny jobs stream*'
# The options of the linux-clang-tsan test preset (CMakePresets.json), so the jobs arm runs as its CTest does.
TSAN_OPTIONS = 'halt_on_error=1 second_deadlock_stack=1 history_size=4 suppressions=%s' % SUPPRESSIONS.as_posix()
COMPILE = ('clang++', '-std=c++20', '-O0', '-g', '-fno-omit-frame-pointer', '-pthread')
# name, program key, arguments, the line a passing run prints. Frame counts give each control run about the length of
# a jobs-case run on a hosted runner.
ARMS = (('jobs', 'jobs', (CASE,), 'All tests passed'),
        ('control-fiber', 'tsan', ('fiber', '800'), 'cycle 1 done'),
        ('control-plain', 'tsan', ('plain', '3000'), 'cycle 1 done'),
        ('control-unsanitized', 'plain', ('plain', '10000'), 'cycle 1 done'))
REPORT = re.compile(r'WARNING: ThreadSanitizer|ERROR: ThreadSanitizer|ThreadSanitizer:DEADLYSIGNAL|==\d+==ABORTING'
                    r'|=== WRONG|FAILED:|CHECK failed|Segmentation fault|Illegal instruction|Aborted')
TIMEOUT_SECONDS = 120


def load_fuzz_host_check():
    """The host identity of the libFuzzer host check (CPU, microcode, hypervisor, kernel, compiler)."""
    spec = importlib.util.spec_from_file_location('fuzz_host_check', ROOT / 'scripts/fuzz-host-check.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def reports_in(text, limit=6):
    """The first `limit` report lines of a log, each with the line after it (the place of a ThreadSanitizer report)."""
    lines = text.splitlines()
    found = []
    for index, line in enumerate(lines):
        if REPORT.search(line):
            found.append([part.strip() for part in lines[index:index + 2]])
            if len(found) == limit:
                break
    return found


def classify(exit_code, output, pass_line, timed_out=False):
    """The event of one run: None when it passed cleanly, else the reason."""
    if timed_out:
        return 'timeout'
    if exit_code != 0:
        return 'exit %d' % exit_code
    if reports_in(output):
        return 'report'
    if pass_line not in output:
        return 'no pass line'
    return None


def pinned(cpus, command):
    return ['taskset', '-c', ','.join(str(cpu) for cpu in cpus), *command]


def busy_loops(cpus):
    return [subprocess.Popen(['taskset', '-c', str(cpu), 'sh', '-c', 'while :; do :; done']) for cpu in cpus]


def build_controls(scratch):
    """The control in two builds; returns {'tsan': path, 'plain': path}."""
    programs = {}
    for key, flags in (('tsan', ('-fsanitize=thread',)), ('plain', ())):
        out = scratch / ('tsan_control' if key == 'tsan' else 'plain_control')
        subprocess.run([*COMPILE, *flags, str(CONTROL), '-o', str(out)], check=True)
        programs[key] = out
    return programs


def run_arm(name, command, pass_line, env, log):
    started = time.monotonic()
    timed_out = False
    try:
        result = subprocess.run(command, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace',
                                timeout=TIMEOUT_SECONDS)
        code, output = result.returncode, result.stdout + result.stderr
    except subprocess.TimeoutExpired as expired:
        timed_out = True
        code = -1
        partial = expired.stdout or ''
        output = partial.decode('utf-8', 'replace') if isinstance(partial, bytes) else partial
    wall = round(time.monotonic() - started, 2)
    event = classify(code, output, pass_line, timed_out)
    if event is not None:
        Path(log).write_text(output, encoding='utf-8')
    return {'arm': name, 'exit_code': code, 'wall_seconds': wall, 'event': event,
            'reports': reports_in(output) if event else [], 'log': str(log) if event else ''}


def tally(records):
    """Per arm: runs, events and seconds, in ARMS order."""
    rows = []
    for name, _, _, _ in ARMS:
        mine = [record for record in records if record['arm'] == name]
        rows.append({'arm': name, 'runs': len(mine), 'events': sum(1 for record in mine if record['event']),
                     'seconds': round(sum(record['wall_seconds'] for record in mine), 1)})
    return rows


def markdown(host, cpus, rounds, records):
    lines = ['## TSan host check: jobs streaming case and the Cerid-free control', '',
             f'CPU **{host["cpu"]}** (microcode {host["microcode"]}, hypervisor {host["hypervisor"]}, '
             f'{host["logical_cpus"]} logical CPUs), kernel {host["kernel"]}, {host["compiler"]}. {rounds} rounds, '
             f'every process pinned to CPUs {",".join(str(cpu) for cpu in cpus)} beside a busy loop on each.', '',
             '| Arm | Runs | Events | Seconds |', '|---|---|---|---|']
    for row in tally(records):
        lines.append(f'| {row["arm"]} | {row["runs"]} | {row["events"]} | {row["seconds"]} |')
    events = [record for record in records if record['event']]
    lines += ['', f'**Events: {len(events)}.**']
    for record in events[:12]:
        lines += ['', f'### {record["arm"]}: {record["event"]}', '', '```']
        for report in record['reports'][:3] or [['(no report line; see the log)']]:
            lines += report
        lines.append('```')
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', required=True, help='a linux-clang-tsan build directory with crd-jobs-tests built')
    parser.add_argument('--rounds', type=int, default=150, help='rounds; each runs every arm once')
    parser.add_argument('--cpus', default='0,1', help='the two CPUs every run and the busy loops are pinned to')
    parser.add_argument('--scratch', required=True, help='scratch directory (the control builds, failing logs)')
    parser.add_argument('--json', help='write the records as JSON')
    parser.add_argument('--summary', help='append the Markdown summary to this file')
    args = parser.parse_args(argv)
    build = Path(args.build) if os.path.isabs(args.build) else ROOT / args.build
    jobs = build / 'tests/jobs/crd-jobs-tests'
    if not jobs.is_file():
        print(f'tsan-host-check: {jobs} is not built (cmake --build --preset linux-clang-tsan --target '
              'crd-jobs-tests)', file=sys.stderr)
        return 2
    cpus = [int(cpu) for cpu in args.cpus.split(',')]
    scratch = Path(args.scratch).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    programs = dict(build_controls(scratch), jobs=jobs)
    env = dict(os.environ, TSAN_OPTIONS=TSAN_OPTIONS)
    env.pop('DEBUGINFOD_URLS', None)  # a symbolizer waiting on an unreachable debuginfod turns a report into a hang
    host = load_fuzz_host_check().host_identity()
    records = []
    loops = busy_loops(cpus)
    try:
        for round_index in range(args.rounds):
            for name, key, arguments, pass_line in ARMS:
                command = pinned(cpus, [str(programs[key]), *arguments])
                log = scratch / f'{name}-{round_index}.log'
                records.append(dict(run_arm(name, command, pass_line, env, log), round=round_index))
                if records[-1]['event']:
                    print(f'round {round_index} {name}: {records[-1]["event"]}', flush=True)
    finally:
        for loop in loops:
            loop.kill()
    text = markdown(host, cpus, args.rounds, records)
    print(text)
    if args.json:
        Path(args.json).write_text(json.dumps({'schema': 'cerid-tsan-host-check/1', 'rounds': args.rounds,
                                               'cpus': cpus, 'host': host, 'tally': tally(records),
                                               'records': records}, indent=2), encoding='utf-8')
    if args.summary:
        with open(args.summary, 'a', encoding='utf-8') as summary:
            summary.write(text)
    return 1 if any(record['event'] for record in records) else 0


if __name__ == '__main__':
    sys.exit(main())
