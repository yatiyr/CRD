#!/usr/bin/env python3
"""Run the DIAG.10a host-separation rounds on one machine and report every event.

Contract: docs/design/test-instruments.md (host separation). The development host showed input-independent sanitizer
reports in the libFuzzer rounds and repetition replays of the CEIR and CKIR loader targets
(docs/sessions/2026-10-11-diag-10a-container-overflow-control.md). This driver repeats that exposure on another
machine: for each arm (the default ASan options, then the container-overflow control `detect_container_overflow=0`)
it runs one libFuzzer process on a scratch copy of the committed corpus for a fixed time, restarting it with the next
seed after an event until the time is spent, then replays an adopted unit a fixed number of times in one process.
The committed corpora are only read. An event is any artifact, any nonzero exit, or any sanitizer, assertion or
libFuzzer report in a log; the driver exits 1 when there was one. It never builds.

    python3 scripts/fuzz-host-check.py --build build/linux-clang-fuzz --target ceir-text --seed 7201 \
        --seconds 540 --repeat cd301be8 --repetitions 10000 --repeat-runs 3 --scratch <dir> \
        --json <file> --summary "$GITHUB_STEP_SUMMARY"
"""
import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fuzz  # noqa: E402  (the registered targets, limits and statistics parser)

UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
# The repetition options of the 2026-10-11 note ("The six artifacts").
REPEAT_ASAN_OPTIONS = ('detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:'
                       'strict_init_order=1:detect_leaks=1:halt_on_error=1')
ARMS = (('default', ''), ('container-overflow-off', 'detect_container_overflow=0'))
SEED_STRIDE = 100  # a restart after an event takes seed + 100 * restart
REPORT = re.compile(r'ERROR: (?:AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|MemorySanitizer)'
                    r'|runtime error:|AddressSanitizer:DEADLYSIGNAL|==\d+==ABORTING|deadly signal'
                    r'|libFuzzer: (?:timeout|out-of-memory|fuzz target exited|run interrupted)'
                    r'|Assertion|assert(?:ion)? failed|CRD_FATAL|fatal invariant|Segmentation fault|Aborted')
FRAME = re.compile(r'^\s*#\d+ 0x[0-9a-f]+ ')
DETAIL = re.compile(r'^\s*(?:expr|file|line|message): ')
EXECUTED = re.compile(r'^#(\d+)\s', re.M)


def options(base, arm_flag):
    return ':'.join(part for part in (base, arm_flag) if part)


def reports_in(text, frames=8):
    """Every report line in a log with an assertion's details and the first `frames` stack frames after it."""
    lines = text.splitlines()
    found = []
    index = 0
    while index < len(lines):
        if REPORT.search(lines[index]):
            head = [lines[index].strip()]
            cursor = index + 1
            taken = 0
            while cursor < len(lines) and cursor < index + 60 and taken < frames:
                if not taken and DETAIL.match(lines[cursor]):
                    head.append(lines[cursor].strip())  # the assertion's expression, place and message
                elif FRAME.match(lines[cursor]):
                    head.append(lines[cursor].strip())
                    taken += 1
                elif taken and not lines[cursor].strip():
                    break
                cursor += 1
            found.append(head)
            index = max(cursor, index + 1) if taken else index + 1
        else:
            index += 1
    return found


def executions_of(text, stats):
    """Executions a process ran: the final statistics, or the last status line when it died before printing them."""
    if 'number_of_executed_units' in stats:
        return stats['number_of_executed_units'], True
    counts = EXECUTED.findall(text)
    return (int(counts[-1]), False) if counts else (0, False)


def run_process(command, env, log_path, cwd):
    started = time.monotonic()
    result = subprocess.run(command, cwd=str(cwd), env=env, capture_output=True, text=True, encoding='utf-8',
                            errors='replace')
    wall = round(time.monotonic() - started, 1)
    output = result.stdout + result.stderr
    Path(log_path).write_text(output, encoding='utf-8')
    return result.returncode, wall, output


def fuzz_round(program, target, arm, arm_flag, seed, seconds, scratch):
    """One round: libFuzzer processes on one working corpus until `seconds` are spent; returns the process records."""
    work = scratch / arm / 'round'
    corpus = work / 'corpus'
    artifacts = work / 'artifacts'
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(fuzz.corpus_directory(target), corpus)
    artifacts.mkdir(parents=True)
    env = dict(os.environ, UBSAN_OPTIONS=UBSAN_OPTIONS)
    env.pop('ASAN_OPTIONS', None)
    if arm_flag:
        env['ASAN_OPTIONS'] = arm_flag
    records = []
    deadline = time.monotonic() + seconds
    restart = 0
    while True:
        remaining = int(deadline - time.monotonic())
        if remaining < 5:
            break
        process_seed = seed + SEED_STRIDE * restart
        before = set(fuzz.artifacts_in(artifacts))
        command = [str(program), str(corpus), f'-seed={process_seed}', f'-max_total_time={remaining}',
                   '-print_final_stats=1', f'-artifact_prefix={artifacts.as_posix()}/',
                   *fuzz.limit_arguments(fuzz.LIMITS)]
        log = work / f'fuzzer-{restart}.log'
        code, wall, output = run_process(command, env, log, work)
        stats = fuzz.parse_stats(output)
        executions, counted = executions_of(output, stats)
        new_artifacts = sorted(set(fuzz.artifacts_in(artifacts)) - before)
        reports = reports_in(output)
        records.append({'kind': 'round', 'arm': arm, 'asan_options': env.get('ASAN_OPTIONS', ''), 'seed': process_seed,
                        'seconds': remaining, 'wall_seconds': wall, 'exit_code': code, 'executions': executions,
                        'executions_counted': counted, 'stats': stats, 'log': str(log),
                        'artifacts': [str(path) for path in new_artifacts], 'reports': reports,
                        'event': bool(code != 0 or new_artifacts or reports)})
        if not records[-1]['event']:
            break
        restart += 1
    return records


def repeat_unit(program, arm, arm_flag, unit, repetitions, runs, scratch):
    work = scratch / arm / 'repeat'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, UBSAN_OPTIONS=UBSAN_OPTIONS, ASAN_OPTIONS=options(REPEAT_ASAN_OPTIONS, arm_flag))
    records = []
    for run in range(runs):
        log = work / f'repeat-{unit.name[:8]}-{run}.log'
        before = set(fuzz.artifacts_in(work))
        command = [str(program), f'-runs={repetitions}', f'-artifact_prefix={work.as_posix()}/', str(unit)]
        code, wall, output = run_process(command, env, log, work)
        new_artifacts = sorted(set(fuzz.artifacts_in(work)) - before)
        reports = reports_in(output)
        records.append({'kind': 'repeat', 'arm': arm, 'asan_options': env['ASAN_OPTIONS'], 'unit': unit.name,
                        'repetitions': repetitions, 'wall_seconds': wall, 'exit_code': code,
                        'executions': repetitions if code == 0 else 0, 'executions_counted': code == 0,
                        'log': str(log), 'artifacts': [str(path) for path in new_artifacts], 'reports': reports,
                        'event': bool(code != 0 or new_artifacts or reports)})
    return records


def find_unit(target, prefix):
    matches = sorted(path for path in fuzz.corpus_directory(target).iterdir() if path.name.startswith(prefix))
    if len(matches) != 1:
        raise fuzz.FuzzError(f'{prefix!r} names {len(matches)} units of the {target} corpus')
    return matches[0]


def read_text(path):
    try:
        return Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return ''


def command_line(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
    except OSError:
        return 'unavailable'
    return (result.stdout or result.stderr).strip().splitlines()[0] if (result.stdout or result.stderr) else ''


def host_identity():
    cpuinfo = read_text('/proc/cpuinfo')
    fields = {}
    for line in cpuinfo.splitlines():
        key, _, value = line.partition(':')
        key = key.strip()
        if key in ('model name', 'microcode', 'flags') and key not in fields:
            fields[key] = value.strip()
    return {'cpu': fields.get('model name', platform.processor() or 'unknown'),
            'microcode': fields.get('microcode', 'unknown'),
            'hypervisor': 'yes' if ' hypervisor' in (' ' + fields.get('flags', '')) else 'no',
            'logical_cpus': os.cpu_count(), 'kernel': platform.release(),
            'mmap_rnd_bits': read_text('/proc/sys/vm/mmap_rnd_bits').strip() or 'unknown',
            'compiler': command_line(['clang++', '--version'])}


def markdown(target, seed, host, records):
    lines = [f'## Host check: {target}, seed {seed}', '',
             f'CPU **{host["cpu"]}** (microcode {host["microcode"]}, hypervisor {host["hypervisor"]}, '
             f'{host["logical_cpus"]} logical CPUs), kernel {host["kernel"]}, '
             f'vm.mmap_rnd_bits {host["mmap_rnd_bits"]}, {host["compiler"]}.', '',
             '| Arm | Kind | Seed or unit | Budget s | Wall s | Executions | Exit | Event |',
             '|---|---|---|---|---|---|---|---|']
    for record in records:
        what = record.get('seed', record.get('unit', '')[:8])
        executions = f'{record["executions"]:,}' + ('' if record['executions_counted'] else ' (at least)')
        budget = record.get('seconds', '')
        lines.append(f'| {record["arm"]} | {record["kind"]} | {what} | {budget} | {record["wall_seconds"]} | '
                     f'{executions} | {record["exit_code"]} | {"yes" if record["event"] else "no"} |')
    for arm, _ in ARMS:
        rounds = [r for r in records if r['arm'] == arm and r['kind'] == 'round']
        repeats = [r for r in records if r['arm'] == arm and r['kind'] == 'repeat']
        lines.append('')
        lines.append(f'- {arm}: libFuzzer {sum(r["executions"] for r in rounds):,} executions in {len(rounds)} '
                     f'process(es), repetitions {sum(r["executions"] for r in repeats):,}, events '
                     f'{sum(1 for r in rounds + repeats if r["event"])}.')
    events = [record for record in records if record['event']]
    lines += ['', f'**Events: {len(events)}.**']
    for record in events:
        lines += ['', f'### {record["arm"]} {record["kind"]} {record.get("seed", record.get("unit", ""))}: '
                      f'exit {record["exit_code"]}', '']
        for artifact in record['artifacts']:
            lines.append(f'- artifact `{Path(artifact).name}`')
        lines.append('```')
        for report in record['reports'][:6] or [['(no report line; see the log)']]:
            lines += report
        lines.append('```')
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', required=True, help='a configured linux-clang-fuzz build directory')
    parser.add_argument('--target', required=True, choices=sorted(fuzz.TARGETS))
    parser.add_argument('--seed', type=int, required=True, help='libFuzzer -seed of the first process of each arm')
    parser.add_argument('--seconds', type=int, default=540, help='libFuzzer time per arm')
    parser.add_argument('--repeat', help='a committed unit (name prefix) to replay in one process per run')
    parser.add_argument('--repetitions', type=int, default=10000, help='executions of the unit per run (-runs)')
    parser.add_argument('--repeat-runs', type=int, default=3, help='repetition processes per arm')
    parser.add_argument('--scratch', required=True, help='scratch directory (working corpora, artifacts, logs)')
    parser.add_argument('--json', help='write the records as JSON')
    parser.add_argument('--summary', help='append the Markdown summary to this file')
    args = parser.parse_args(argv)
    build = Path(args.build) if os.path.isabs(args.build) else fuzz.ROOT / args.build
    scratch = Path(args.scratch).resolve()
    try:
        program = fuzz.executable(build, args.target, libfuzzer=True)
        unit = find_unit(args.target, args.repeat) if args.repeat else None
    except fuzz.FuzzError as error:
        print(f'fuzz-host-check: {error}', file=sys.stderr)
        return 2
    host = host_identity()
    records = []
    for arm, arm_flag in ARMS:
        records += fuzz_round(program, args.target, arm, arm_flag, args.seed, args.seconds, scratch)
        if unit is not None:
            records += repeat_unit(program, arm, arm_flag, unit, args.repetitions, args.repeat_runs, scratch)
    for record in records:
        print(f'{record["arm"]} {record["kind"]} {record.get("seed", record.get("unit", ""))}: exit '
              f'{record["exit_code"]}, {record["executions"]} executions, event {record["event"]}')
    text = markdown(args.target, args.seed, host, records)
    print(text)
    if args.json:
        Path(args.json).write_text(json.dumps({'schema': 'cerid-fuzz-host-check/1', 'target': args.target,
                                               'seed': args.seed, 'seconds': args.seconds, 'host': host,
                                               'records': records}, indent=2), encoding='utf-8')
    if args.summary:
        with open(args.summary, 'a', encoding='utf-8') as summary:
            summary.write(text)
    return 1 if any(record['event'] for record in records) else 0


if __name__ == '__main__':
    sys.exit(main())
