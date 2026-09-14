#!/usr/bin/env python3
"""Structured exact-revision evidence for one CI lane.

`inventory` records the CTest inventory of a build directory (`ctest --show-only=json-v1`) before anything runs.
`bundle` collects, after the lane's steps, the revision and tree identity, the toolchain fingerprint from the CMake
cache, the adapter census, the inventory, the JUnit results and the failing tests' output into
`<output>/conclusion.json`, copies the raw JUnit and inventory beside it and appends a concise job summary. Neither
command changes a lane's verdict: the register gate owns the exit code. Contract: docs/design/ci-tiers.md.
"""
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 'cerid-ci-evidence/1'
CACHE_KEYS = ('CMAKE_GENERATOR', 'CMAKE_BUILD_TYPE', 'CMAKE_CONFIGURATION_TYPES', 'CMAKE_CXX_COMPILER',
              'CMAKE_INTERPROCEDURAL_OPTIMIZATION', 'CRD_SIMD_LEVEL', 'CRD_SHIPPING', 'CRD_DETERMINISTIC_FP',
              'CRD_ENABLE_PROFILING', 'CRD_MODULES', 'CRD_BUILD_TESTS', 'CLANG_TIDY_EXE', 'CRD_ENABLE_PCH',
              'CRD_COMPILER_LAUNCHER')
RUN_KEYS = ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_JOB', 'GITHUB_EVENT_NAME', 'GITHUB_REF', 'GITHUB_SHA',
            'RUNNER_OS', 'RUNNER_ARCH', 'ImageOS', 'ImageVersion')
OUTPUT_TAIL_LINES = 60
SLOWEST = 15


def git(args, repo=ROOT):
    try:
        completed = subprocess.run(['git'] + list(args), cwd=str(repo), capture_output=True, text=True,
                                   encoding='utf-8', check=False)
    except OSError:
        return None
    return completed.stdout.strip() if completed.returncode == 0 else None


def revision_identity(repo=ROOT):
    return {'revision': git(['rev-parse', 'HEAD'], repo), 'tree': git(['rev-parse', 'HEAD^{tree}'], repo),
            'dirty': bool(git(['status', '--porcelain', '--untracked-files=no'], repo))}


def toolchain(build_dir):
    """Generator, build type, compiler identity and the Cerid switches from the CMake cache and compiler probe."""
    build_dir = Path(build_dir)
    result = {}
    cache = build_dir / 'CMakeCache.txt'
    if cache.is_file():
        for line in cache.read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.match(r'^([A-Za-z0-9_]+):[A-Z]+=(.*)$', line)
            if match and match.group(1) in CACHE_KEYS:
                result[match.group(1)] = match.group(2)
    for probe in sorted(build_dir.glob('CMakeFiles/*/CMakeCXXCompiler.cmake')):
        result['cmake_version'] = probe.parent.name
        for line in probe.read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.match(r'^set\((CMAKE_CXX_COMPILER(?:_ID|_VERSION)?) "(.*)"\)$', line)
            if match:
                result[match.group(1)] = match.group(2)
    return result


def inventory_summary(path):
    """Test count, label histogram and resource locks from a json-v1 inventory."""
    path = Path(path)
    if not path.is_file():
        return None
    document = json.loads(path.read_text(encoding='utf-8'))
    labels, locks = {}, {}
    tests = document.get('tests', [])
    for test in tests:
        properties = {p['name']: p['value'] for p in test.get('properties', [])}
        for label in properties.get('LABELS', []) or []:
            labels[label] = labels.get(label, 0) + 1
        for lock in properties.get('RESOURCE_LOCK', []) or []:
            locks[lock] = locks.get(lock, 0) + 1
    return {'tests': len(tests), 'labels': labels, 'resource_locks': locks}


def junit_summary(path):
    """Counts, duration, failing tests with output tails and the slowest tests from a CTest JUnit document."""
    path = Path(path)
    if not path.is_file():
        return None
    root = ET.parse(path).getroot()
    suites = [root] if root.tag == 'testsuite' else list(root.iter('testsuite'))
    cases = []
    for suite in suites:
        cases.extend(suite.findall('testcase'))
    failed, skipped, durations = [], 0, []
    for case in cases:
        name = case.get('name', '?')
        seconds = float(case.get('time') or 0.0)
        durations.append((seconds, name))
        if case.find('skipped') is not None:
            skipped += 1
            continue
        failure = case.find('failure')
        if failure is not None or case.get('status') == 'fail':
            output = (case.findtext('system-out') or '').splitlines()
            failed.append({'name': name, 'time': seconds, 'message': (failure.get('message') if failure is not None else '') or '',
                           'output_tail': output[-OUTPUT_TAIL_LINES:]})
    durations.sort(reverse=True)
    return {'tests': len(cases), 'failures': len(failed), 'skipped': skipped,
            'passed': len(cases) - len(failed) - skipped, 'duration_seconds': round(sum(d for d, _ in durations), 3),
            'failed': failed, 'slowest': [{'name': n, 'time': round(t, 3)} for t, n in durations[:SLOWEST]]}


def read_lines(path):
    path = Path(path) if path else None
    if not path or not path.is_file():
        return None
    return [line.rstrip() for line in path.read_text(encoding='utf-8', errors='replace').splitlines() if line.strip()]


def ninja_entries(lines):
    """(start_ms, end_ms, output) per v7 .ninja_log line; the header and malformed lines are skipped."""
    entries = []
    for line in lines:
        fields = line.split('\t')
        if line.startswith('#') or len(fields) < 5:
            continue
        try:
            entries.append((int(fields[0]), int(fields[1]), fields[3]))
        except ValueError:
            continue
    return entries


def ninja_summary(entries, slowest=SLOWEST):
    """Edges, span, edge sum, effective parallelism and the slowest edges of Ninja log entries.

    Ninja appends entries in completion order with times relative to its own start, so a drop in the end time marks
    a new invocation; the span is the sum of the per-invocation spans. A hosted checkout builds into a fresh
    directory in one invocation, so a lane's log is the cold-build board of the exact revision; a local log
    accumulates incremental invocations and is reported with their count.
    """
    if not entries:
        return {'edges': 0, 'invocations': 0, 'span_seconds': 0.0, 'edge_seconds': 0.0, 'parallelism': 0.0,
                'slowest': []}
    invocations = []
    for entry in entries:
        if invocations and entry[1] >= invocations[-1][-1][1]:
            invocations[-1].append(entry)
        else:
            invocations.append([entry])
    span = sum(max(e[1] for e in group) - min(e[0] for e in group) for group in invocations)
    total = sum(e[1] - e[0] for e in entries)
    ranked = sorted(entries, key=lambda e: e[0] - e[1])[:slowest]
    return {'edges': len(entries), 'invocations': len(invocations), 'span_seconds': round(span / 1000.0, 3),
            'edge_seconds': round(total / 1000.0, 3), 'parallelism': round(total / span, 2) if span else 0.0,
            'slowest': [{'output': e[2], 'seconds': round((e[1] - e[0]) / 1000.0, 3)} for e in ranked]}


def ninja_log_summary(path):
    path = Path(path) if path else None
    if not path or not path.is_file():
        return None
    return ninja_summary(ninja_entries(path.read_text(encoding='utf-8', errors='replace').splitlines()))


def _count(value):
    if isinstance(value, dict):
        if isinstance(value.get('counts'), dict):
            return sum(int(v) for v in value['counts'].values())
        return sum(_count(v) for v in value.values())
    if isinstance(value, (int, float)):
        return int(value)
    return 0


def sccache_summary(path):
    """Hits, misses, non-cacheable compiles and reasons from `sccache --show-stats --stats-format json`."""
    path = Path(path) if path else None
    if not path or not path.is_file():
        return None
    try:
        document = json.loads(path.read_text(encoding='utf-8-sig'))
    except ValueError:
        return {'error': 'unparseable statistics'}
    stats = document.get('stats', document) if isinstance(document, dict) else {}
    reasons = stats.get('not_cached') or {}
    return {'requests': _count(stats.get('compile_requests', 0)), 'hits': _count(stats.get('cache_hits', 0)),
            'misses': _count(stats.get('cache_misses', 0)),
            'non_cacheable': _count(stats.get('non_cacheable_compilations', 0)),
            'errors': _count(stats.get('cache_errors', 0)) + _count(stats.get('cache_read_errors', 0))
            + _count(stats.get('cache_write_errors', 0)),
            'reasons': {str(k): _count(v) for k, v in reasons.items()} if isinstance(reasons, dict) else {}}


def bundle(preset, build_dir, output, junit=None, inventory=None, adapters=None, outcome=None, repo=ROOT,
           environ=None, packages=None, sccache=None):
    """Write conclusion.json and copies of the raw inputs; return the conclusion record."""
    environ = os.environ if environ is None else environ
    build_dir = Path(build_dir)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    junit = Path(junit) if junit else build_dir / 'Testing/Temporary/ctest-junit.xml'
    inventory = Path(inventory) if inventory else build_dir / 'Testing/inventory.json'
    missing = []
    record = {'schema': SCHEMA, 'preset': preset, 'build_dir': str(build_dir).replace('\\', '/'),
              'run': {key: environ[key] for key in RUN_KEYS if environ.get(key)}}
    record.update(revision_identity(repo))
    record['toolchain'] = toolchain(build_dir)
    if not record['toolchain']:
        missing.append('CMakeCache.txt')
    census = read_lines(adapters)
    record['adapters'] = census
    if census is None:
        missing.append('adapters')
    record['packages'] = read_lines(packages)
    record['build'] = {'ninja': ninja_log_summary(build_dir / '.ninja_log'), 'sccache': sccache_summary(sccache)}
    record['inventory'] = inventory_summary(inventory)
    if record['inventory'] is None:
        missing.append('inventory')
    else:
        shutil.copyfile(inventory, output / 'inventory.json')
    record['results'] = junit_summary(junit)
    if record['results'] is None:
        missing.append('junit')
    else:
        shutil.copyfile(junit, output / 'ctest-junit.xml')
    record['outcome'] = outcome
    if outcome:
        record['status'] = 'pass' if outcome == 'success' else outcome
    elif record['results'] is None:
        record['status'] = 'missing'
    else:
        record['status'] = 'pass' if record['results']['failures'] == 0 else 'fail'
    record['missing'] = missing
    (output / 'conclusion.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    return record


def build_markdown(build):
    if not build or not build.get('ninja'):
        return 'no Ninja log'
    ninja = build['ninja']
    text = '%d edges in %d invocation%s, span %.1f s, edge sum %.0f s, parallelism %.1f' % (
        ninja['edges'], ninja['invocations'], '' if ninja['invocations'] == 1 else 's', ninja['span_seconds'],
        ninja['edge_seconds'], ninja['parallelism'])
    if ninja['slowest']:
        text += '; slowest `%s` %.1f s' % (ninja['slowest'][0]['output'], ninja['slowest'][0]['seconds'])
    stats = build.get('sccache')
    if stats and 'error' not in stats:
        text += '; sccache %d hits, %d misses, %d non-cacheable' % (stats['hits'], stats['misses'], stats['non_cacheable'])
        if stats['reasons']:
            text += ' (' + ', '.join('%s %d' % (k, v) for k, v in sorted(stats['reasons'].items())) + ')'
        if stats['errors']:
            text += ', %d cache errors' % stats['errors']
    elif stats:
        text += '; sccache statistics ' + stats['error']
    return text


def summary_markdown(record):
    tool = record['toolchain']
    results = record['results']
    inventory = record['inventory']
    compiler = ' '.join(x for x in (tool.get('CMAKE_CXX_COMPILER_ID'), tool.get('CMAKE_CXX_COMPILER_VERSION')) if x)
    rows = [('Status', record['status']),
            ('Revision', '%s (tree %s%s)' % ((record.get('revision') or '?')[:12], (record.get('tree') or '?')[:12],
                                             ', dirty' if record.get('dirty') else '')),
            ('Toolchain', '%s; %s %s; CMake %s' % (compiler or 'unknown compiler', tool.get('CMAKE_GENERATOR', '?'),
                                                    tool.get('CMAKE_BUILD_TYPE') or tool.get('CMAKE_CONFIGURATION_TYPES', ''),
                                                    tool.get('cmake_version', '?'))),
            ('Switches', ', '.join('%s=%s' % (k, tool[k]) for k in ('CRD_SIMD_LEVEL', 'CRD_SHIPPING', 'CRD_DETERMINISTIC_FP',
                                                                   'CRD_ENABLE_PROFILING', 'CMAKE_INTERPROCEDURAL_OPTIMIZATION',
                                                                   'CRD_ENABLE_PCH', 'CRD_COMPILER_LAUNCHER')
                                   if k in tool) or 'none recorded'),
            ('Adapters', '; '.join(record['adapters'][:6]) if record['adapters'] else 'not recorded'),
            ('Packages', '; '.join(record['packages'][:12]) if record.get('packages') else 'not recorded'),
            ('Build', build_markdown(record.get('build'))),
            ('Inventory', '%d tests, %d labelled, %d resource-locked' % (inventory['tests'], sum(inventory['labels'].values()),
                                                                          sum(inventory['resource_locks'].values()))
             if inventory else 'not recorded'),
            ('Results', '%d executed: %d passed, %d failed, %d skipped in %.1f s' % (
                results['tests'], results['passed'], results['failures'], results['skipped'], results['duration_seconds'])
             if results else 'no JUnit document')]
    lines = ['### %s: %s' % (record['preset'], record['status']), '', '| Field | Value |', '|---|---|']
    lines += ['| %s | %s |' % (k, v.replace('|', '\\|')) for k, v in rows]
    if results and results['failed']:
        lines += ['', '**Failed tests**', '']
        lines += ['- `%s` (%.1f s)%s' % (f['name'], f['time'], (': ' + f['message']) if f['message'] else '')
                  for f in results['failed'][:20]]
    if results and results['slowest']:
        lines += ['', 'Slowest: ' + ', '.join('`%s` %.1f s' % (s['name'], s['time']) for s in results['slowest'][:5])]
    if record['missing']:
        lines += ['', 'Missing evidence: ' + ', '.join(record['missing'])]
    return '\n'.join(lines) + '\n'


def run_inventory(build_dir, config=None, output=None):
    build_dir = Path(build_dir)
    output = Path(output) if output else build_dir / 'Testing/inventory.json'
    command = ['ctest', '--test-dir', str(build_dir), '--show-only=json-v1']
    if config:
        command += ['-C', config]
    completed = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', check=False)
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        return completed.returncode
    document = json.loads(completed.stdout)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, indent=1) + '\n', encoding='utf-8')
    print('Inventoried %d tests into %s' % (len(document.get('tests', [])), output))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    inv = sub.add_parser('inventory', help='record ctest --show-only=json-v1 before the tests run')
    inv.add_argument('--build-dir', required=True)
    inv.add_argument('--config', help='multi-config generator configuration')
    inv.add_argument('--output', help='inventory path (default: <build-dir>/Testing/inventory.json)')
    bun = sub.add_parser('bundle', help='write conclusion.json and the job summary')
    bun.add_argument('--preset', required=True)
    bun.add_argument('--build-dir', required=True)
    bun.add_argument('--output', required=True, help='evidence directory')
    bun.add_argument('--junit', help='JUnit document (default: <build-dir>/Testing/Temporary/ctest-junit.xml)')
    bun.add_argument('--inventory', help='inventory document (default: <build-dir>/Testing/inventory.json)')
    bun.add_argument('--adapters', help='adapter census text')
    bun.add_argument('--packages', help='OS package census text (dpkg-query output)')
    bun.add_argument('--sccache-stats', help='sccache --show-stats --stats-format json output of the build step')
    bun.add_argument('--outcome', help='outcome of the step that decides the lane (success, failure, cancelled, skipped)')
    bun.add_argument('--summary', help='GITHUB_STEP_SUMMARY file to append to')
    args = parser.parse_args(argv)
    if args.command == 'inventory':
        return run_inventory(args.build_dir, args.config, args.output)
    record = bundle(args.preset, args.build_dir, args.output, args.junit, args.inventory, args.adapters,
                    args.outcome or None, packages=args.packages, sccache=args.sccache_stats)
    markdown = summary_markdown(record)
    if args.summary:
        with open(args.summary, 'a', encoding='utf-8') as handle:
            handle.write(markdown)
    print(markdown)
    return 0


if __name__ == '__main__':
    sys.exit(main())
