#!/usr/bin/env python3
"""Run the NET.2 provider host-separation rounds: the threaded ML-DSA-65 signing case beside programs that split it.

Contract: docs/design/test-instruments.md (host separation). On hosted win-asan, four threads signing with one ML-DSA-65
SigningKey have had a thread's first signing refused (ProviderFailure) after 10 to 19 s, a second signing at once
succeeding, while the case passes in about 1 s (the nonreproduced register's
win-asan-ml-dsa-threaded-signing-refused). This driver splits the refusal between AWS-LC, crd-crypto and the test
program. Every round runs each arm once alone, in a fresh process (every sighting was a thread's first signing): the
case from crd-crypto-tests, directly and through CTest (as the lane runs it); crd-mldsa-threads-control, the same
provider calls through the same tree's AWS-LC alone, plain, with its threads' first signings released together (gate)
with the case's storage (storage) and with SigningKey::sign's and ProviderVerifier::verify's calls exactly (copy); and
crd-mldsa-threads-cerid, the case through crd-crypto's SigningKey without the test program, plain, gated, warmed by one
signing on the main thread (warm), with its signatures outside the engine's allocator (std-buffer), its key from a raw
seed (seed-key), verified by raw calls (raw-verify) or not at all (no-verify), and the control's raw AWS-LC calls made
inside it (raw): plain, as SigningKey::sign's exact calls written in the program (raw-copy) and verified by
ProviderVerifier (raw-crd-verify). --arms runs only the named arms. With --parallel N a round then runs N processes at
once, the arms in turn, as CTest's parallel jobs load the lane. An event is a nonzero exit, a timeout or a missing pass
line; a stall is a run over STALL_SECONDS, counted apart. The driver exits 1 when any run had an event. It builds
nothing.

    python scripts/provider-host-check.py --build build/win-asan --rounds 40 --scratch <dir> --json <file> \
        [--arms case,cerid,cerid-raw] --summary "$GITHUB_STEP_SUMMARY"
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

ROOT = Path(__file__).resolve().parent.parent
CASE = 'Provider: one verifier and one key serve several threads'
SUFFIX = '.exe' if os.name == 'nt' else ''
CONTROL = 'tests/crypto/crd-mldsa-threads-control' + SUFFIX
CERID = 'tests/crypto/crd-mldsa-threads-cerid' + SUFFIX
# name, the program's path in the tree (CTEST: the ctest on PATH over the tree's crypto tests), arguments, the line a
# passing run prints.
CTEST = 'ctest'
ARMS = (('case', 'tests/crypto/crd-crypto-tests' + SUFFIX, (CASE,), 'All tests passed'),
        ('case-ctest', CTEST, ('-R', '^' + CASE + '$', '--no-tests=error', '--output-on-failure'),
         '100% tests passed'),
        ('control', CONTROL, (), 'result: clean'),
        ('control-gate', CONTROL, ('gate',), 'result: clean'),
        ('control-storage', CONTROL, ('storage',), 'result: clean'),
        ('control-gate-storage', CONTROL, ('gate', 'storage'), 'result: clean'),
        ('control-copy', CONTROL, ('copy',), 'result: clean'),
        ('cerid', CERID, (), 'result: clean'),
        ('cerid-gate', CERID, ('gate',), 'result: clean'),
        ('cerid-warm', CERID, ('warm',), 'result: clean'),
        ('cerid-std-buffer', CERID, ('std-buffer',), 'result: clean'),
        ('cerid-seed-key', CERID, ('seed-key',), 'result: clean'),
        ('cerid-raw-verify', CERID, ('raw-verify',), 'result: clean'),
        ('cerid-no-verify', CERID, ('no-verify',), 'result: clean'),
        ('cerid-raw', CERID, ('raw',), 'result: clean'),
        ('cerid-raw-copy', CERID, ('raw', 'copy'), 'result: clean'),
        ('cerid-raw-crd-verify', CERID, ('raw', 'crd-verify'), 'result: clean'))
FAULT = re.compile(r'FAULT at iteration.*|signing refused after \d+ ms.*|FAILED:.*|==\d+==ERROR.*|result: SETUP.*')
FIRST_SIGNING = re.compile(r'first signing (\d+) ms')
STALL_SECONDS = 5.0
TIMEOUT_SECONDS = 300


def faults_in(text, limit=6):
    """The first `limit` fault lines of a run's output."""
    return [match.group(0).strip() for match in FAULT.finditer(text)][:limit]


def classify(exit_code, output, pass_line, timed_out=False):
    """The event of one run: None when it passed cleanly, else the reason."""
    if timed_out:
        return 'timeout'
    if exit_code != 0:
        return 'exit %d' % exit_code
    if pass_line not in output:
        return 'no pass line'
    return None


def slowest_first_signing(output):
    """The control's slowest first signing in ms, or None (the Cerid case prints none while it passes)."""
    times = [int(value) for value in FIRST_SIGNING.findall(output)]
    return max(times) if times else None


def record(arm, phase, round_index, exit_code, output, wall, timed_out, log):
    """One run's record; a run without an event keeps no log."""
    pass_line = next(line for name, _, _, line in ARMS if name == arm)
    event = classify(exit_code, output, pass_line, timed_out)
    if event is None:
        Path(log).unlink(missing_ok=True)
    return {'arm': arm, 'phase': phase, 'round': round_index, 'exit_code': exit_code, 'wall_seconds': wall,
            'stall': wall > STALL_SECONDS, 'first_signing_ms': slowest_first_signing(output), 'event': event,
            'faults': faults_in(output) if event else [], 'log': str(log) if event else ''}


def run_batch(commands, scratch, phase, round_index):
    """Runs (arm, command) pairs at once, each timed to its own exit; returns their records in order."""
    started = time.monotonic()
    runs = []
    for index, (arm, command) in enumerate(commands):
        log = scratch / f'{arm}-{phase}-{round_index}-{index}.log'
        with open(log, 'wb') as out:
            runs.append({'arm': arm, 'log': log, 'wall': None, 'timed_out': False,
                         'process': subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT)})
    while any(run['wall'] is None for run in runs):
        now = time.monotonic()
        for run in runs:
            if run['wall'] is not None:
                continue
            if run['process'].poll() is not None:
                run['wall'] = round(now - started, 2)
            elif now - started > TIMEOUT_SECONDS:
                run['process'].kill()
                run['process'].wait()
                run['wall'], run['timed_out'] = round(now - started, 2), True
        time.sleep(0.02)
    return [record(run['arm'], phase, round_index, -1 if run['timed_out'] else run['process'].returncode,
                   run['log'].read_text(encoding='utf-8', errors='replace'), run['wall'], run['timed_out'], run['log'])
            for run in runs]


def selected_arms(text):
    """The ARMS entries `text` names (comma-separated; kept in ARMS order; empty: every arm); ValueError on an unknown
    name."""
    if not text:
        return ARMS
    names = {name.strip() for name in text.split(',') if name.strip()}
    unknown = sorted(names - {name for name, _, _, _ in ARMS})
    if unknown or not names:
        raise ValueError('unknown arm ' + ', '.join(unknown) if unknown else 'no arm named')
    return tuple(arm for arm in ARMS if arm[0] in names)


def tally(records):
    """Per arm and phase that ran: runs, events, stalls and the slowest run, in ARMS order."""
    rows = []
    for name, _, _, _ in ARMS:
        for phase in ('alone', 'contended'):
            mine = [entry for entry in records if entry['arm'] == name and entry['phase'] == phase]
            if not mine:
                continue
            rows.append({'arm': name, 'phase': phase, 'runs': len(mine),
                         'events': sum(1 for entry in mine if entry['event']),
                         'stalls': sum(1 for entry in mine if entry['stall']),
                         'slowest_seconds': max((entry['wall_seconds'] for entry in mine), default=0.0)})
    return rows


def host_identity(build):
    """The CPU, its count, the system and the tree's compiler."""
    cpu = platform.processor() or 'unknown'
    if os.name == 'nt':
        import winreg  # pylint: disable=import-outside-toplevel
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
                cpu = winreg.QueryValueEx(key, 'ProcessorNameString')[0].strip()
        except OSError:
            pass
    else:
        for line in Path('/proc/cpuinfo').read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith('model name'):
                cpu = line.partition(':')[2].strip()
                break
    return {'cpu': cpu, 'logical_cpus': os.cpu_count(), 'system': platform.platform(), 'compiler': compiler_of(build)}


def compiler_of(build):
    """The tree's C++ compiler as CMake identified it ("MSVC 19.51.36231.0"), or 'unknown'."""
    for path in sorted(build.glob('CMakeFiles/*/CMakeCXXCompiler.cmake')):
        text = path.read_text(encoding='utf-8', errors='replace')
        found = dict(re.findall(r'^set\(CMAKE_CXX_COMPILER_(ID|VERSION) "([^"]*)"\)$', text, re.MULTILINE))
        if found.get('ID'):
            return f'{found["ID"]} {found.get("VERSION", "")}'.strip()
    return 'unknown'


def markdown(host, rounds, parallel, records):
    contended = f', then {parallel} processes at once' if parallel else ''
    lines = ['## Provider host check: threaded ML-DSA-65 signing, its control and its crd-crypto program', '',
             f'CPU **{host["cpu"]}** ({host["logical_cpus"]} logical CPUs), {host["system"]}, {host["compiler"]}. '
             f'{rounds} rounds; each runs every arm alone{contended}. A stall is a run over {STALL_SECONDS:g} s.', '',
             '| Arm | Phase | Runs | Events | Stalls | Slowest s |', '|---|---|---|---|---|---|']
    for row in tally(records):
        lines.append(f'| {row["arm"]} | {row["phase"]} | {row["runs"]} | {row["events"]} | {row["stalls"]} | '
                     f'{row["slowest_seconds"]} |')
    events = [entry for entry in records if entry['event']]
    lines += ['', f'**Events: {len(events)}.**']
    for entry in events[:12]:
        lines += ['', f'### {entry["arm"]}, round {entry["round"]} ({entry["phase"]}): {entry["event"]}, '
                      f'{entry["wall_seconds"]} s', '', '```']
        lines += entry['faults'][:4] or ['(no fault line; see the log)']
        lines.append('```')
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', required=True,
                        help='a build tree with crd-crypto-tests, crd-mldsa-threads-control and '
                             'crd-mldsa-threads-cerid built')
    parser.add_argument('--rounds', type=int, default=40, help='rounds; each runs every arm alone')
    parser.add_argument('--parallel', type=int, default=0,
                        help='then this many processes at once, the arms in turn (0: no contended phase)')
    parser.add_argument('--arms', default='', help='comma-separated arms to run (default: every arm)')
    parser.add_argument('--scratch', required=True, help='scratch directory (the logs of failing runs)')
    parser.add_argument('--json', help='write the records as JSON')
    parser.add_argument('--summary', help='append the Markdown summary to this file')
    args = parser.parse_args(argv)
    if args.rounds < 1 or args.parallel < 0:
        parser.error('--rounds must be at least 1 and --parallel at least 0')
    try:
        arms = selected_arms(args.arms)
    except ValueError as error:
        parser.error(str(error))
    build = (Path(args.build) if os.path.isabs(args.build) else ROOT / args.build).resolve()
    programs = {}
    for name, path, arguments, _ in arms:
        if path == CTEST:
            programs[name] = [shutil.which(CTEST) or CTEST, '--test-dir', str(build / 'tests/crypto'), *arguments]
            continue
        program = build / path
        if not program.is_file():
            print(f'provider-host-check: {program} is not built (cmake --build <tree> --target crd-crypto-tests '
                  'crd-mldsa-threads-control crd-mldsa-threads-cerid)', file=sys.stderr)
            return 2
        programs[name] = [str(program), *arguments]
    scratch = Path(args.scratch).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    host = host_identity(build)
    records = []
    for round_index in range(args.rounds):
        batches = [('alone', [(name, programs[name])]) for name, _, _, _ in arms]
        if args.parallel:
            turn = [arms[(round_index * args.parallel + index) % len(arms)][0] for index in range(args.parallel)]
            batches.append(('contended', [(name, programs[name]) for name in turn]))
        for phase, commands in batches:
            for entry in run_batch(commands, scratch, phase, round_index):
                records.append(entry)
                if entry['event']:
                    print(f'round {round_index} {entry["arm"]} ({phase}): {entry["event"]}', flush=True)
    text = markdown(host, args.rounds, args.parallel, records)
    print(text)
    if args.json:
        Path(args.json).write_text(json.dumps({'schema': 'cerid-provider-host-check/1', 'rounds': args.rounds,
                                               'parallel': args.parallel, 'host': host, 'tally': tally(records),
                                               'records': records}, indent=2), encoding='utf-8')
    if args.summary:
        with open(args.summary, 'a', encoding='utf-8') as summary:
            summary.write(text)
    return 1 if any(entry['event'] for entry in records) else 0


if __name__ == '__main__':
    sys.exit(main())
