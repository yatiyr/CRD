#!/usr/bin/env python3
"""Structured exact-revision evidence for one CI lane.

`inventory` records the CTest inventory of a build directory (`ctest --show-only=json-v1`) before anything runs.
`watch` runs the lane's test command with the host's memory sampled and records the machine and the samples. `bundle`
collects, after the lane's steps, the revision and tree identity, the toolchain fingerprint from the CMake cache, the
adapter census, the inventory, the JUnit results, the host record and the failing tests' output into
`<output>/conclusion.json`, copies the raw JUnit and inventory beside it and appends a concise job summary. No command
changes a lane's verdict (`watch` passes the test command's exit code on): the register gate owns the exit code.
`compare` reads two lanes' evidence and reports one lane's per-test slowdown against the other (a separate-binary
mode's cost, DIAG.11b). Contract: docs/design/ci-tiers.md.
"""
from pathlib import Path
import argparse
import ctypes
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 'cerid-ci-evidence/1'
# The diagnostic modes a lane's binaries are built with (runtime-diagnostics.md, DIAG.11b), named as a summary shows them.
MODES = (('CRD_ENABLE_ASAN', 'ASan'), ('CRD_ENABLE_UBSAN', 'UBSan'), ('CRD_ENABLE_TSAN', 'TSan'),
         ('CRD_ENABLE_FUZZER', 'libFuzzer'), ('CRD_ENABLE_COVERAGE', 'coverage'), ('CRD_ENABLE_ASSERTS', 'asserts'),
         ('CRD_ENABLE_PROFILING', 'profiling'), ('CRD_ENABLE_CLANG_TIDY', 'clang-tidy'))
CACHE_KEYS = ('CMAKE_GENERATOR', 'CMAKE_BUILD_TYPE', 'CMAKE_CONFIGURATION_TYPES', 'CMAKE_CXX_COMPILER',
              'CMAKE_INTERPROCEDURAL_OPTIMIZATION', 'CRD_SIMD_LEVEL', 'CRD_SHIPPING', 'CRD_DETERMINISTIC_FP',
              'CRD_MODULES', 'CRD_BUILD_TESTS', 'CLANG_TIDY_EXE', 'CRD_ENABLE_PCH',
              'CRD_COMPILER_LAUNCHER') + tuple(key for key, _ in MODES)
RUN_KEYS = ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_JOB', 'GITHUB_EVENT_NAME', 'GITHUB_REF', 'GITHUB_SHA',
            'RUNNER_OS', 'RUNNER_ARCH', 'ImageOS', 'ImageVersion',
            # The Cerid CI platform (ADR-0135 §12): the orchestrator's build, job, step and attempt, and the registry
            # cell it ran on.
            'CERID_CI_BUILD_NUMBER', 'CERID_CI_BUILD_URL', 'CERID_CI_JOB_ID', 'CERID_CI_JOB_URL', 'CERID_CI_STEP_KEY',
            'CERID_CI_RETRY_COUNT', 'CERID_CI_REPOSITORY', 'CERID_CI_BRANCH', 'CERID_CI_COMMIT', 'CERID_CI_SOURCE',
            'CERID_CI_EVENT', 'CERID_CI_AGENT_NAME', 'CERID_CI_DEVICE', 'CERID_CI_STEP', 'CERID_CI_INPUT_KEY',
            'CERID_CI_TIER')
OUTPUT_TAIL_LINES = 60
SLOWEST = 15
# Tests faster than this on the reference lane are left out of a comparison: their time is process start-up.
RATIO_FLOOR = 0.05
GIB = 1024.0 ** 3
# The Test step's host record, written by `watch` beside the JUnit and read by `bundle`.
HOST_TEST = 'Testing/Temporary/host-test.json'
# Seconds between the memory samples taken while a lane's tests run.
HOST_TEST_INTERVAL = 5.0


# ---- machine and memory -------------------------------------------------------------------------------------------

class _MemoryStatus(ctypes.Structure):
    _fields_ = [('dwLength', ctypes.c_uint32), ('dwMemoryLoad', ctypes.c_uint32), ('ullTotalPhys', ctypes.c_uint64),
                ('ullAvailPhys', ctypes.c_uint64), ('ullTotalPageFile', ctypes.c_uint64),
                ('ullAvailPageFile', ctypes.c_uint64), ('ullTotalVirtual', ctypes.c_uint64),
                ('ullAvailVirtual', ctypes.c_uint64), ('ullAvailExtendedVirtual', ctypes.c_uint64)]


def memory_status():
    """(total physical, available physical, committed) in bytes; committed is the system commit charge."""
    if platform.system() == 'Windows':
        status = _MemoryStatus()
        status.dwLength = ctypes.sizeof(status)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
        return status.ullTotalPhys, status.ullAvailPhys, status.ullTotalPageFile - status.ullAvailPageFile
    values = {}
    try:
        for line in Path('/proc/meminfo').read_text(encoding='utf-8').splitlines():
            key, _, rest = line.partition(':')
            values[key] = int(rest.split()[0]) * 1024
    except OSError:
        return 0, 0, 0
    return values.get('MemTotal', 0), values.get('MemAvailable', 0), values.get('Committed_AS', 0)


def cpu_name():
    if platform.system() == 'Windows':
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0')
            return winreg.QueryValueEx(key, 'ProcessorNameString')[0].strip()
        except OSError:
            return platform.processor()
    try:
        for line in Path('/proc/cpuinfo').read_text(encoding='utf-8').splitlines():
            if line.startswith('model name'):
                return line.split(':', 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def machine():
    total, _, _ = memory_status()
    return {'cpu': cpu_name(), 'logical_cpus': os.cpu_count(), 'memory_gib': round(total / GIB, 1),
            'os': platform.platform(), 'python': platform.python_version()}


class PressureSampler:
    """Samples available and committed memory on a thread while a board row or a lane's tests run."""

    def __init__(self, interval=1.0, on_sample=None):
        self.interval = interval
        self.on_sample = on_sample
        self.samples = []
        self._stop = threading.Event()
        self._thread = None

    def __enter__(self):
        self.samples = [memory_status()]
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def _run(self):
        while not self._stop.wait(self.interval):
            self.samples.append(memory_status())
            if self.on_sample:
                self.on_sample(self)

    def __exit__(self, *_):
        self._stop.set()
        self._thread.join()
        self.samples.append(memory_status())

    def summary(self):
        baseline_available, baseline_committed = self.samples[0][1], self.samples[0][2]
        min_available = min(s[1] for s in self.samples)
        max_committed = max(s[2] for s in self.samples)
        return {'samples': len(self.samples), 'baseline_available_gib': round(baseline_available / GIB, 2),
                'min_available_gib': round(min_available / GIB, 2),
                'baseline_committed_gib': round(baseline_committed / GIB, 2),
                'peak_committed_gib': round(max_committed / GIB, 2),
                'pressure_gib': round((max_committed - baseline_committed) / GIB, 2)}


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


def test_labels(path):
    """Each test's labels from a json-v1 inventory, without the `cerid.` ownership annotations."""
    path = Path(path)
    if not path.is_file():
        return {}
    labels = {}
    for test in json.loads(path.read_text(encoding='utf-8')).get('tests', []):
        for prop in test.get('properties', []):
            if prop['name'] == 'LABELS':
                labels[test['name']] = [label for label in prop['value'] or [] if not label.startswith('cerid.')]
    return labels


def junit_cases(path):
    """The testcase elements of a CTest JUnit document, or None when it is absent."""
    path = Path(path)
    if not path.is_file():
        return None
    root = ET.parse(path).getroot()
    suites = [root] if root.tag == 'testsuite' else list(root.iter('testsuite'))
    return [case for suite in suites for case in suite.findall('testcase')]


def case_outcome(case):
    if case.find('skipped') is not None:
        return 'skipped'
    if case.find('failure') is not None or case.get('status') == 'fail':
        return 'failed'
    return 'passed'


def junit_summary(path, labels=None):
    """Counts, duration, failing tests with output tails, the slowest tests and, per label, the executed tests'
    outcomes from a CTest JUnit document; `labels` maps a test name to its labels (test_labels)."""
    cases = junit_cases(path)
    if cases is None:
        return None
    failed, skipped, durations, by_label = [], 0, [], {}
    for case in cases:
        name = case.get('name', '?')
        seconds = float(case.get('time') or 0.0)
        durations.append((seconds, name))
        outcome = case_outcome(case)
        if outcome == 'skipped':
            skipped += 1
        elif outcome == 'failed':
            failure = case.find('failure')
            output = (case.findtext('system-out') or '').splitlines()
            failed.append({'name': name, 'time': seconds, 'message': (failure.get('message') if failure is not None else '') or '',
                           'output_tail': output[-OUTPUT_TAIL_LINES:]})
        for label in (labels or {}).get(name, []):
            tally = by_label.setdefault(label, {'tests': 0, 'passed': 0, 'failed': 0, 'skipped': 0})
            tally['tests'] += 1
            tally[outcome] += 1
    durations.sort(reverse=True)
    return {'tests': len(cases), 'failures': len(failed), 'skipped': skipped,
            'passed': len(cases) - len(failed) - skipped, 'duration_seconds': round(sum(d for d, _ in durations), 3),
            'failed': failed, 'slowest': [{'name': n, 'time': round(t, 3)} for t, n in durations[:SLOWEST]],
            'labels': dict(sorted(by_label.items()))}


def passed_seconds(path):
    """Seconds per passed test of a CTest JUnit document."""
    return {case.get('name', '?'): float(case.get('time') or 0.0) for case in junit_cases(path) or []
            if case_outcome(case) == 'passed'}


def slowdown(mode_junit, reference_junit, floor=RATIO_FLOOR):
    """One lane's per-test time over another's, over the tests that passed on both and took at least `floor`
    seconds on the reference: the count, the nearest-rank quartiles and median of the ratios, and both sums."""
    mode, reference = passed_seconds(mode_junit), passed_seconds(reference_junit)
    names = sorted(name for name in mode if reference.get(name, 0.0) >= floor)
    ratios = sorted(mode[name] / reference[name] for name in names)
    if not ratios:
        return {'tests': 0}
    rank = lambda q: ratios[max(0, -(-len(ratios) * q // 100) - 1)]
    return {'tests': len(ratios), 'p25': round(rank(25), 2), 'median': round(rank(50), 2), 'p75': round(rank(75), 2),
            'mode_seconds': round(sum(mode[name] for name in names), 1),
            'reference_seconds': round(sum(reference[name] for name in names), 1)}


def compare_markdown(mode_dir, reference_dir, floor=RATIO_FLOOR):
    """A board row: both lanes' presets and recorded modes, the per-test slowdown and the lanes' test totals."""
    lanes = [json.loads((Path(d) / 'conclusion.json').read_text(encoding='utf-8')) for d in (mode_dir, reference_dir)]
    ratio = slowdown(Path(mode_dir) / 'ctest-junit.xml', Path(reference_dir) / 'ctest-junit.xml', floor)
    name = lambda lane: '%s (%s)' % (lane['preset'],
                                     ', '.join(lane['modes']) if 'modes' in lane else 'modes not recorded')
    if not ratio['tests']:
        return '| %s | %s | 0 | - | - | - | - |' % (name(lanes[0]), name(lanes[1]))
    return '| %s | %s | %d | %.2f | %.2f-%.2f | %.0f s / %.0f s | %.0f s / %.0f s |' % (
        name(lanes[0]), name(lanes[1]), ratio['tests'], ratio['median'], ratio['p25'], ratio['p75'],
        ratio['mode_seconds'], ratio['reference_seconds'], lanes[0]['results']['duration_seconds'],
        lanes[1]['results']['duration_seconds'])


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
    location = document.get('cache_location') if isinstance(document, dict) else None
    return {'location': str(location) if location else None,
            'requests': _count(stats.get('compile_requests', 0)), 'hits': _count(stats.get('cache_hits', 0)),
            'misses': _count(stats.get('cache_misses', 0)),
            'non_cacheable': _count(stats.get('non_cacheable_compilations', 0)),
            'errors': _count(stats.get('cache_errors', 0)) + _count(stats.get('cache_read_errors', 0))
            + _count(stats.get('cache_write_errors', 0)),
            'reasons': {str(k): _count(v) for k, v in reasons.items()} if isinstance(reasons, dict) else {}}


def bundle(preset, build_dir, output, junit=None, inventory=None, adapters=None, outcome=None, repo=ROOT,
           environ=None, packages=None, sccache=None, host=machine):
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
    record['modes'] = [name for key, name in MODES
                       if record['toolchain'].get(key, '').upper() in ('ON', 'TRUE', 'YES', 'Y', '1')]
    census = read_lines(adapters)
    record['adapters'] = census
    if census is None:
        missing.append('adapters')
    record['packages'] = read_lines(packages)
    record['build'] = {'ninja': ninja_log_summary(build_dir / '.ninja_log'), 'sccache': sccache_summary(sccache)}
    record['host'] = {'machine': host(), 'test': read_json(build_dir / HOST_TEST)}
    if record['host']['test'] is None:
        missing.append('host-test')
    record['inventory'] = inventory_summary(inventory)
    if record['inventory'] is None:
        missing.append('inventory')
    else:
        shutil.copyfile(inventory, output / 'inventory.json')
    record['results'] = junit_summary(junit, test_labels(inventory))
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


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def host_markdown(host):
    if not host:
        return 'not recorded'
    box = host['machine']
    text = '%s, %s logical CPUs, %.1f GiB' % (box.get('cpu') or '?', box.get('logical_cpus') or '?',
                                             box.get('memory_gib') or 0.0)
    test = host.get('test')
    if test:
        memory = test['memory']
        text += '; tests %.0f s wall, at least %.1f GiB available (%.1f before), commit peak %.1f GiB' % (
            test['seconds'], memory['min_available_gib'], memory['baseline_available_gib'],
            memory['peak_committed_gib'])
        if test.get('exit') is None:
            text += ' (the tests did not return)'
    return text


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
        if stats.get('location'):
            text += ' (cache: %s)' % stats['location']
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
                                                                   'CMAKE_INTERPROCEDURAL_OPTIMIZATION',
                                                                   'CRD_ENABLE_PCH', 'CRD_COMPILER_LAUNCHER')
                                   if k in tool) or 'none recorded'),
            ('Diagnostic modes', ', '.join(record.get('modes') or []) or 'none'),
            ('Adapters', '; '.join(record['adapters'][:6]) if record['adapters'] else 'not recorded'),
            ('Packages', '; '.join(record['packages'][:12]) if record.get('packages') else 'not recorded'),
            ('Build', build_markdown(record.get('build'))),
            ('Host', host_markdown(record.get('host'))),
            ('Inventory', '%d tests, %d labelled, %d resource-locked' % (inventory['tests'], sum(inventory['labels'].values()),
                                                                          sum(inventory['resource_locks'].values()))
             if inventory else 'not recorded'),
            ('Results', '%d executed: %d passed, %d failed, %d skipped in %.1f s' % (
                results['tests'], results['passed'], results['failures'], results['skipped'], results['duration_seconds'])
             if results else 'no JUnit document'),
            ('By label', '; '.join('%s %d executed: %d passed, %d failed, %d skipped' % (
                label, t['tests'], t['passed'], t['failed'], t['skipped'])
                for label, t in results['labels'].items()) or 'no labelled test executed'
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


def watch(build_dir, command, interval=HOST_TEST_INTERVAL, runner=subprocess.call, clock=time.monotonic,
          host=machine):
    """Run a lane's test command with the host's memory sampled and return its exit code. HOST_TEST is rewritten at
    every sample, `exit` null until the command returns, so a job stopped at its time limit keeps the record so far."""
    path = Path(build_dir) / HOST_TEST
    path.parent.mkdir(parents=True, exist_ok=True)
    box = host()
    start = clock()

    def write(sampler, code=None):
        record = {'command': list(command), 'exit': code, 'seconds': round(clock() - start, 1), 'machine': box,
                  'memory': sampler.summary()}
        partial = path.with_suffix('.partial')
        partial.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        os.replace(partial, path)

    with PressureSampler(interval, on_sample=write) as sampler:
        code = runner(command)
    write(sampler, code)
    return code


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
    wat = sub.add_parser('watch', help="run a lane's tests with the host's memory sampled (exit code passed on)")
    wat.add_argument('--build-dir', required=True, help='build directory; the record goes to <build-dir>/' + HOST_TEST)
    wat.add_argument('command_line', nargs=argparse.REMAINDER, metavar='command', help='the test command, after --')
    cmp = sub.add_parser('compare', help="a board row: one lane's per-test slowdown against another's")
    cmp.add_argument('--mode', required=True, help='evidence directory of the lane built with the mode')
    cmp.add_argument('--reference', required=True, help='evidence directory of the lane without it')
    cmp.add_argument('--floor', type=float, default=RATIO_FLOOR, help='least reference seconds of a compared test')
    args = parser.parse_args(argv)
    if args.command == 'inventory':
        return run_inventory(args.build_dir, args.config, args.output)
    if args.command == 'watch':
        command = args.command_line[1:] if args.command_line[:1] == ['--'] else args.command_line
        return watch(args.build_dir, command) if command else 2
    if args.command == 'compare':
        print(compare_markdown(args.mode, args.reference, args.floor))
        return 0
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
