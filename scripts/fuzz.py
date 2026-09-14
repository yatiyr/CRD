#!/usr/bin/env python3
"""Bounded libFuzzer runs over the ingestion fuzz targets, corpus merge and minimization, and corpus replay.

Contract: docs/design/test-instruments.md. The targets are built by cmake/CrdFuzz.cmake: every configuration builds
the replay executable and its `<target>-corpus` CTest; a configuration with CRD_ENABLE_FUZZER=ON (clang; the `linux-clang-fuzz` preset) also
builds `<target>-libfuzzer`. This script never builds; it drives an existing build directory.

    python scripts/fuzz.py run --build build/linux-clang-fuzz --target ceir-text --seconds 600
    python scripts/fuzz.py merge --build build/linux-clang-fuzz --target ceir-text --from <working corpus>
    python scripts/fuzz.py minimize --build build/linux-clang-fuzz --target ceir-text --artifact <crash file>
    python scripts/fuzz.py replay --build build/win-debug --target all

Every run is bounded: -max_total_time, -max_len (the harness's own limit), -timeout, -rss_limit_mb and
-malloc_limit_mb, with artifacts (crash-*, timeout-*, oom-*, leak-*) written under the scratch directory and the
final statistics recorded as a JSON board. A finding enters the committed corpus only through `adopt`, after the
loader is fixed, so the replay test on every lane keeps it as a regression; an unfixed finding stays an artifact
with its owner named in the session record, never a silent skip.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

TARGETS = {
    'ceir-text': {'corpus': 'tests/execution/ceir/fuzz/corpus/text', 'executable': 'tests/ceir/fuzz/crd-fuzz-ceir-text'},
    'ceir-binary': {'corpus': 'tests/execution/ceir/fuzz/corpus/binary', 'executable': 'tests/ceir/fuzz/crd-fuzz-ceir-binary'},
    'ckir-text': {'corpus': 'tests/gpu/kir/fuzz/corpus/text', 'executable': 'tests/kir/fuzz/crd-fuzz-ckir-text'},
    'ckir-binary': {'corpus': 'tests/gpu/kir/fuzz/corpus/binary', 'executable': 'tests/kir/fuzz/crd-fuzz-ckir-binary'},
}
LIMITS = {'max_len': 65536, 'timeout': 10, 'rss_limit_mb': 2048, 'malloc_limit_mb': 256}
STAT_PATTERN = re.compile(r'^stat::(\w+):\s+(\d+)', re.MULTILINE)
ARTIFACT_PREFIXES = ('crash-', 'timeout-', 'oom-', 'leak-', 'slow-unit-')


class FuzzError(Exception):
    pass


def executable(build, target, libfuzzer):
    path = Path(build) / TARGETS[target]['executable']
    if libfuzzer:
        path = path.with_name(path.name + '-libfuzzer')
    if os.name == 'nt':
        path = path.with_suffix('.exe')
    if not path.is_file():
        kind = 'libFuzzer executable (configure with -DCRD_ENABLE_FUZZER=ON under clang)' if libfuzzer else 'replay executable'
        raise FuzzError(f'{path} does not exist: build the {kind} first')
    return path


def corpus_directory(target):
    path = ROOT / TARGETS[target]['corpus']
    if not path.is_dir():
        raise FuzzError(f'{path} is not a directory')
    return path


def targets_of(name):
    if name == 'all':
        return list(TARGETS)
    if name not in TARGETS:
        raise FuzzError(f'unknown target {name!r}; known: {", ".join(TARGETS)}')
    return [name]


def limit_arguments(limits):
    return [f'-{key}={value}' for key, value in limits.items()]


def parse_stats(text):
    return {key: int(value) for key, value in STAT_PATTERN.findall(text)}


def artifacts_in(directory):
    return sorted(path for path in Path(directory).iterdir() if path.name.startswith(ARTIFACT_PREFIXES))


def run_target(build, target, seconds, jobs, scratch, limits, extra):
    """One bounded libFuzzer run on a working copy of the committed corpus; returns the board record."""
    program = executable(build, target, libfuzzer=True)
    committed = corpus_directory(target)
    work = Path(scratch) / target
    working_corpus = work / 'corpus'
    artifacts = work / 'artifacts'
    if working_corpus.exists():
        shutil.rmtree(working_corpus)
    shutil.copytree(committed, working_corpus)
    artifacts.mkdir(parents=True, exist_ok=True)
    command = [str(program), str(working_corpus), f'-max_total_time={seconds}', '-print_final_stats=1',
               f'-artifact_prefix={artifacts.as_posix()}/', *limit_arguments(limits)]
    if jobs > 1:
        command += [f'-jobs={jobs}', f'-workers={jobs}']
    command += extra
    started = time.monotonic()
    result = subprocess.run(command, cwd=str(work), capture_output=True, text=True, encoding='utf-8', errors='replace')
    wall = round(time.monotonic() - started, 1)
    output = result.stdout + result.stderr
    (work / 'fuzzer.log').write_text(output, encoding='utf-8')
    stats = parse_stats(output)
    if jobs > 1:
        for log in sorted(work.glob('fuzz-*.log')):
            for key, value in parse_stats(log.read_text(encoding='utf-8', errors='replace')).items():
                stats[key] = stats.get(key, 0) + value
    record = {'target': target, 'seconds': seconds, 'wall_seconds': wall, 'exit_code': result.returncode,
              'limits': limits, 'jobs': jobs, 'corpus_before': len(list(committed.iterdir())),
              'corpus_after': len(list(working_corpus.iterdir())),
              'artifacts': [path.name for path in artifacts_in(artifacts)], 'stats': stats,
              'working_corpus': str(working_corpus), 'log': str(work / 'fuzzer.log')}
    return record


def merge_target(build, target, source):
    """libFuzzer -merge=1: add to the committed corpus only the units of `source` that add coverage."""
    program = executable(build, target, libfuzzer=True)
    committed = corpus_directory(target)
    before = {path.name for path in committed.iterdir()}
    command = [str(program), '-merge=1', str(committed), str(source), *limit_arguments(LIMITS)]
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if result.returncode != 0:
        raise FuzzError(f'merge failed with exit code {result.returncode}:\n{result.stdout}{result.stderr}')
    after = {path.name for path in committed.iterdir()}
    return sorted(after - before)


def minimize_artifact(build, target, artifact, scratch, runs):
    program = executable(build, target, libfuzzer=True)
    work = Path(scratch) / target / 'minimize'
    work.mkdir(parents=True, exist_ok=True)
    command = [str(program), '-minimize_crash=1', f'-runs={runs}', f'-artifact_prefix={work.as_posix()}/',
               str(artifact), *limit_arguments(LIMITS)]
    result = subprocess.run(command, cwd=str(work), capture_output=True, text=True, encoding='utf-8', errors='replace')
    (work / 'minimize.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    minimized = sorted(work.glob('minimized-*'), key=lambda path: path.stat().st_size)
    return minimized[0] if minimized else Path(artifact)


def adopt_artifact(target, artifact):
    """Copy a (minimized) artifact into the committed corpus under libFuzzer's content-hash name."""
    import hashlib
    data = Path(artifact).read_bytes()
    name = hashlib.sha1(data).hexdigest()
    destination = corpus_directory(target) / name
    destination.write_bytes(data)
    return destination


def replay_target(build, target, verbose):
    program = executable(build, target, libfuzzer=False)
    command = [str(program)] + (['--verbose'] if verbose else []) + [str(corpus_directory(target))]
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
    return result.returncode, result.stdout + result.stderr


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('run', 'merge', 'minimize', 'adopt', 'replay'))
    parser.add_argument('--build', required=True, help='configured build directory')
    parser.add_argument('--target', default='all', help='target name or "all"')
    parser.add_argument('--seconds', type=int, default=600, help='run: -max_total_time per target')
    parser.add_argument('--jobs', type=int, default=1, help='run: parallel libFuzzer jobs')
    parser.add_argument('--scratch', help='run/minimize: scratch directory (default: a temporary directory)')
    parser.add_argument('--from', dest='source', help='merge: working corpus directory to merge from')
    parser.add_argument('--artifact', help='minimize/adopt: the artifact file')
    parser.add_argument('--runs', type=int, default=10000, help='minimize: -runs')
    parser.add_argument('--output', help='run: write the board records as JSON')
    parser.add_argument('--verbose', action='store_true')
    parser.add_argument('--max-len', type=int, default=LIMITS['max_len'])
    parser.add_argument('--timeout', type=int, default=LIMITS['timeout'])
    parser.add_argument('--rss-limit-mb', type=int, default=LIMITS['rss_limit_mb'])
    parser.add_argument('--malloc-limit-mb', type=int, default=LIMITS['malloc_limit_mb'])
    parser.add_argument('extra', nargs='*', help='run: extra libFuzzer flags after --')
    args = parser.parse_args(argv)
    limits = {'max_len': args.max_len, 'timeout': args.timeout, 'rss_limit_mb': args.rss_limit_mb,
              'malloc_limit_mb': args.malloc_limit_mb}
    build = Path(args.build) if os.path.isabs(args.build) else ROOT / args.build
    try:
        names = targets_of(args.target)
        if args.command == 'run':
            scratch = Path(args.scratch) if args.scratch else Path(tempfile.mkdtemp(prefix='crd-fuzz-'))
            records = []
            for name in names:
                record = run_target(build, name, args.seconds, args.jobs, scratch, limits, args.extra)
                records.append(record)
                stats = record['stats']
                print(f'{name}: exit {record["exit_code"]}, {record["wall_seconds"]} s, '
                      f'{stats.get("number_of_executed_units", 0)} executions '
                      f'({stats.get("average_exec_per_sec", 0)}/s), corpus {record["corpus_before"]} -> '
                      f'{record["corpus_after"]}, new units {stats.get("new_units_added", 0)}, '
                      f'peak rss {stats.get("peak_rss_mb", 0)} MiB, artifacts {len(record["artifacts"])}')
                for artifact in record['artifacts']:
                    print(f'  finding: {artifact}')
            if args.output:
                Path(args.output).write_text(json.dumps({'schema': 'cerid-fuzz-board/1', 'build': str(build),
                                                         'runs': records}, indent=2), encoding='utf-8')
            return 1 if any(record['artifacts'] for record in records) else 0
        if args.command == 'merge':
            if not args.source:
                raise FuzzError('merge needs --from <working corpus>')
            for name in names:
                added = merge_target(build, name, args.source)
                print(f'{name}: {len(added)} unit(s) added to the committed corpus' + (': ' + ', '.join(added) if added else ''))
            return 0
        if args.command == 'minimize':
            if len(names) != 1 or not args.artifact:
                raise FuzzError('minimize needs one --target and --artifact')
            scratch = Path(args.scratch) if args.scratch else Path(tempfile.mkdtemp(prefix='crd-fuzz-'))
            result = minimize_artifact(build, names[0], args.artifact, scratch, args.runs)
            print(f'{names[0]}: minimized artifact {result} ({result.stat().st_size} bytes)')
            return 0
        if args.command == 'adopt':
            if len(names) != 1 or not args.artifact:
                raise FuzzError('adopt needs one --target and --artifact')
            destination = adopt_artifact(names[0], args.artifact)
            print(f'{names[0]}: adopted as {destination.relative_to(ROOT).as_posix()}')
            return 0
        failed = 0
        for name in names:
            code, output = replay_target(build, name, args.verbose)
            print(f'{name}: exit {code}\n{output.strip()}')
            failed += code != 0
        return 1 if failed else 0
    except FuzzError as error:
        print(f'fuzz: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
