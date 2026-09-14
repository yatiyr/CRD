#!/usr/bin/env python3
"""Measured build board for one preset: configure, cold, no-op, source edit, header edit, PCH-header edit, link.

The runner configures a preset into a scratch build directory (never a tree a developer builds in) with an explicit
job count and records, per row: wall time, the Ninja log of that row (edges, span, edge sum, effective parallelism,
slowest edges), memory pressure sampled once a second (minimum available, peak committed), the build-tree size and,
when a launcher is under test (`--sccache`), sccache's statistics zeroed before and read after the row. Edited files
are restored byte-for-byte with their original timestamps, so neither the working tree nor another build directory
sees the edit. Run it from a shell with the toolchain on PATH (scripts/msvc-env.bat on Windows).

`render` merges board documents into one markdown table for docs/bench. Contract: docs/design/build-performance.md.
"""
from pathlib import Path
import argparse
import ctypes
import datetime
import importlib.util
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('ci_evidence', ROOT / 'scripts/ci-evidence.py')
evidence = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(evidence)
SCHEMA = 'cerid-build-bench/1'
ROWS = ('configure', 'cold', 'noop', 'source', 'header', 'pch-header', 'link')
SLOWEST = 10
TOUCH = b'\n// build-bench touch: restored byte-for-byte after the row\n'
DEFAULT_SOURCE = 'engine/foundation/log/src/logger.cpp'
DEFAULT_HEADER = 'engine/foundation/log/include/crd/log/log.hpp'
DEFAULT_PCH_HEADER = 'engine/foundation/core/include/crd/core/types.hpp'
DEFAULT_LINK_TARGET = 'crd-ceir-tests'
GIB = 1024.0 ** 3


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
    """Samples available and committed memory once a second on a thread while a row runs."""

    def __init__(self, interval=1.0):
        self.interval = interval
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


# ---- ninja log ------------------------------------------------------------------------------------------------------

def ninja_entries(path, offset=0):
    """Entries (start_ms, end_ms, output) of a v7 .ninja_log after `offset` lines (one row = the lines it appended)."""
    path = Path(path)
    if not path.is_file():
        return []
    return evidence.ninja_entries(path.read_text(encoding='utf-8', errors='replace').splitlines()[offset:])


def ninja_line_count(path):
    path = Path(path)
    if not path.is_file():
        return 0
    return len(path.read_text(encoding='utf-8', errors='replace').splitlines())


def ninja_summary(entries, slowest=SLOWEST):
    """The evidence bundle's summary (span, edge sum, parallelism, slowest) so boards and lanes agree."""
    return evidence.ninja_summary(entries, slowest)


# ---- sccache --------------------------------------------------------------------------------------------------------

def sccache_command(exe, *arguments, env=None):
    return subprocess.run([str(exe), *arguments], capture_output=True, text=True, encoding='utf-8',
                          errors='replace', env=env, check=False)


def sccache_stats(exe, env=None):
    completed = sccache_command(exe, '--show-stats', '--stats-format', 'json', env=env)
    if completed.returncode != 0:
        return {'error': completed.stderr.strip()[-400:]}
    try:
        return json.loads(completed.stdout)
    except ValueError:
        return {'error': 'unparseable statistics'}


def _count(value):
    if isinstance(value, dict):
        if 'counts' in value and isinstance(value['counts'], dict):
            return sum(int(v) for v in value['counts'].values())
        return sum(_count(v) for v in value.values())
    if isinstance(value, (int, float)):
        return int(value)
    return 0


def sccache_summary(document):
    """Hits, misses, non-cacheable compiles and their reasons from `sccache --show-stats --stats-format json`."""
    if not document or 'error' in document:
        return document
    stats = document.get('stats', document)
    reasons = stats.get('not_cached') or {}
    return {'requests': _count(stats.get('compile_requests', 0)), 'hits': _count(stats.get('cache_hits', 0)),
            'misses': _count(stats.get('cache_misses', 0)),
            'non_cacheable': _count(stats.get('non_cacheable_compilations', 0)),
            'errors': _count(stats.get('cache_errors', 0)) + _count(stats.get('cache_read_errors', 0))
            + _count(stats.get('cache_write_errors', 0)),
            'reasons': {str(k): _count(v) for k, v in reasons.items()} if isinstance(reasons, dict) else {}}


# ---- rows -----------------------------------------------------------------------------------------------------------

class Touched:
    """Append TOUCH to a file for the duration of a row; restore bytes and timestamps exactly afterwards."""

    def __init__(self, path):
        self.path = Path(path)

    def __enter__(self):
        self.original = self.path.read_bytes()
        stat = self.path.stat()
        self.times = (stat.st_atime_ns, stat.st_mtime_ns)
        self.path.write_bytes(self.original + TOUCH)
        return self

    def __exit__(self, *_):
        self.path.write_bytes(self.original)
        os.utime(self.path, ns=self.times)


def tree_bytes(path):
    total = 0
    for directory, _, files in os.walk(path):
        for name in files:
            try:
                total += os.stat(os.path.join(directory, name)).st_size
            except OSError:
                pass
    return total


def find_output(build_dir, name):
    """The linked artifact for `name` (an executable name without extension) under the build directory."""
    for candidate in sorted(Path(build_dir).rglob(name + '.exe')) + sorted(Path(build_dir).rglob(name)):
        if candidate.is_file() and candidate.suffix in ('', '.exe') and 'CMakeFiles' not in candidate.parts:
            return candidate
    return None


class Board:
    def __init__(self, preset, build_dir, jobs, defines, label, sccache=None, source=DEFAULT_SOURCE,
                 header=DEFAULT_HEADER, pch_header=DEFAULT_PCH_HEADER, link_target=DEFAULT_LINK_TARGET,
                 repo=ROOT, environ=None, log=print):
        self.preset = preset
        self.build_dir = Path(build_dir)
        self.jobs = jobs
        self.defines = list(defines)
        self.label = label
        self.sccache = Path(sccache) if sccache else None
        self.source = repo / source
        self.header = repo / header
        self.pch_header = repo / pch_header
        self.link_target = link_target
        self.repo = repo
        self.env = dict(os.environ if environ is None else environ)
        self.log = log
        if self.sccache and not self.env.get('SCCACHE_DIR'):
            raise SystemExit('--sccache requires SCCACHE_DIR in the environment so the cache location is explicit')
        self.record = {'schema': SCHEMA, 'label': label, 'preset': preset, 'jobs': jobs, 'defines': self.defines,
                       'build_dir': str(self.build_dir).replace('\\', '/'), 'launcher': str(self.sccache or ''),
                       'edits': {'source': source, 'header': header, 'pch_header': pch_header, 'link_target': link_target},
                       'machine': machine(), 'started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                       'rows': []}

    def run(self, command, cwd=None):
        self.log('  $ ' + ' '.join(command))
        return subprocess.run(command, cwd=str(cwd or self.repo), env=self.env, check=False)

    def row(self, name, action):
        self.log('[%s] %s' % (self.label, name))
        log_path = self.build_dir / '.ninja_log'
        offset = ninja_line_count(log_path)
        if self.sccache:
            sccache_command(self.sccache, '--zero-stats', env=self.env)
        with PressureSampler() as sampler:
            started = time.perf_counter()
            code = action()
            wall = time.perf_counter() - started
        entry = {'row': name, 'exit_code': code, 'wall_seconds': round(wall, 2),
                 'ninja': ninja_summary(ninja_entries(log_path, offset)), 'memory': sampler.summary()}
        if self.sccache:
            entry['sccache'] = sccache_summary(sccache_stats(self.sccache, env=self.env))
            sccache_command(self.sccache, '--stop-server', env=self.env)
        self.record['rows'].append(entry)
        self.log('  %.1f s wall, %d edges, exit %d' % (wall, entry['ninja']['edges'], code))
        if code != 0:
            raise SystemExit('row %s failed with exit code %d' % (name, code))
        return entry

    def configure(self):
        command = ['cmake', '--preset', self.preset, '-B', str(self.build_dir)]
        command += ['-D' + define for define in self.defines]
        return self.run(command).returncode

    def build(self):
        return self.run(['cmake', '--build', str(self.build_dir), '--parallel', str(self.jobs)]).returncode

    def edit(self, path):
        with Touched(path):
            return self.build()

    def relink(self):
        output = find_output(self.build_dir, self.link_target)
        if output is None:
            raise SystemExit('link row: %s was not found under %s' % (self.link_target, self.build_dir))
        output.unlink()
        return self.build()

    def toolchain(self):
        result = {}
        cache = self.build_dir / 'CMakeCache.txt'
        if cache.is_file():
            for line in cache.read_text(encoding='utf-8', errors='replace').splitlines():
                match = re.match(r'^(CMAKE_GENERATOR|CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER|CRD_ENABLE_PCH|'
                                 r'CRD_COMPILER_LAUNCHER|CMAKE_MSVC_DEBUG_INFORMATION_FORMAT|CRD_COMPILE_JOBS|'
                                 r'CRD_LINK_JOBS):[A-Z]+=(.*)$', line)
                if match:
                    result[match.group(1)] = match.group(2)
        for probe in sorted(self.build_dir.glob('CMakeFiles/*/CMakeCXXCompiler.cmake')):
            for line in probe.read_text(encoding='utf-8', errors='replace').splitlines():
                match = re.match(r'^set\((CMAKE_CXX_COMPILER_ID|CMAKE_CXX_COMPILER_VERSION) "(.*)"\)$', line)
                if match:
                    result[match.group(1)] = match.group(2)
        return result

    def execute(self, rows):
        if self.build_dir.exists():
            raise SystemExit('%s exists; the board needs a fresh scratch directory' % self.build_dir)
        if self.sccache:
            self.record['sccache_dir'] = self.env['SCCACHE_DIR']
            self.record['cache_bytes_before'] = tree_bytes(self.env['SCCACHE_DIR'])
        actions = {'configure': self.configure, 'cold': self.build, 'noop': self.build,
                   'source': lambda: self.edit(self.source), 'header': lambda: self.edit(self.header),
                   'pch-header': lambda: self.edit(self.pch_header), 'link': self.relink}
        if 'configure' not in rows:
            rows = ['configure'] + list(rows)
        for name in rows:
            self.row(name, actions[name])
            if name == 'cold':
                self.record['build_tree_gib_after_cold'] = round(tree_bytes(self.build_dir) / GIB, 2)
        self.record['toolchain'] = self.toolchain()
        self.record['build_tree_gib'] = round(tree_bytes(self.build_dir) / GIB, 2)
        if self.sccache:
            self.record['cache_gib'] = round(tree_bytes(self.env['SCCACHE_DIR']) / GIB, 2)
        self.record['finished'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        return self.record


# ---- rendering ------------------------------------------------------------------------------------------------------

def row_cells(record, entry):
    ninja = entry['ninja']
    memory = entry['memory']
    cells = [record['label'], entry['row'], '%.1f' % entry['wall_seconds'], str(ninja['edges']),
             '%.1f' % ninja['span_seconds'], '%.0f' % ninja['edge_seconds'], '%.1f' % ninja['parallelism'],
             '%.1f' % memory['pressure_gib'], '%.1f' % memory['min_available_gib']]
    stats = entry.get('sccache')
    if stats and 'error' not in stats:
        reasons = ', '.join('%s %d' % (k, v) for k, v in sorted(stats['reasons'].items())) if stats['reasons'] else ''
        cells.append('%d / %d / %d%s' % (stats['hits'], stats['misses'], stats['non_cacheable'],
                                         (' (' + reasons + ')') if reasons else ''))
    else:
        cells.append('uncached' if not record.get('launcher') else 'no statistics')
    return cells


def render(records):
    lines = ['| Board | Row | Wall s | Edges | Ninja span s | Edge sum s | Parallelism | Pressure GiB | Min avail GiB | '
             'sccache hits / misses / non-cacheable |', '|---|---|---|---|---|---|---|---|---|---|']
    for record in records:
        for entry in record['rows']:
            lines.append('| ' + ' | '.join(row_cells(record, entry)) + ' |')
    return '\n'.join(lines) + '\n'


def render_context(records):
    lines = []
    for record in records:
        tool = record.get('toolchain', {})
        lines.append('- **%s**: preset `%s`, jobs %d, defines `%s`; %s %s; PCH %s; launcher `%s`; tree %.1f GiB%s.' % (
            record['label'], record['preset'], record['jobs'], ' '.join(record['defines']) or 'none',
            tool.get('CMAKE_CXX_COMPILER_ID', '?'), tool.get('CMAKE_CXX_COMPILER_VERSION', '?'),
            tool.get('CRD_ENABLE_PCH', '?'), record.get('launcher') or 'none', record.get('build_tree_gib', 0.0),
            ('; cache %.1f GiB' % record['cache_gib']) if 'cache_gib' in record else ''))
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    run = sub.add_parser('run', help='measure one board into a fresh scratch build directory')
    run.add_argument('--preset', required=True)
    run.add_argument('--build-dir', required=True, help='fresh scratch directory (refused when it exists)')
    run.add_argument('--jobs', type=int, required=True, help='explicit --parallel for every build row')
    run.add_argument('--define', action='append', default=[], help='extra cache entry NAME=VALUE (repeatable)')
    run.add_argument('--label', required=True)
    run.add_argument('--rows', default=','.join(ROWS), help='comma-separated subset of: ' + ', '.join(ROWS))
    run.add_argument('--sccache', help='sccache executable; zero/read statistics per row (needs SCCACHE_DIR)')
    run.add_argument('--source', default=DEFAULT_SOURCE)
    run.add_argument('--header', default=DEFAULT_HEADER)
    run.add_argument('--pch-header', default=DEFAULT_PCH_HEADER)
    run.add_argument('--link-target', default=DEFAULT_LINK_TARGET)
    run.add_argument('--output', required=True, help='board JSON document')
    run.add_argument('--delete', action='store_true', help='remove the scratch build directory afterwards')
    show = sub.add_parser('render', help='merge board documents into one markdown table')
    show.add_argument('documents', nargs='+')
    args = parser.parse_args(argv)
    if args.command == 'render':
        records = [json.loads(Path(p).read_text(encoding='utf-8')) for p in args.documents]
        sys.stdout.write(render_context(records) + '\n' + render(records))
        return 0
    rows = [r.strip() for r in args.rows.split(',') if r.strip()]
    unknown = [r for r in rows if r not in ROWS]
    if unknown:
        parser.error('unknown rows: ' + ', '.join(unknown))
    board = Board(args.preset, args.build_dir, args.jobs, args.define, args.label, args.sccache, args.source,
                  args.header, args.pch_header, args.link_target)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        record = board.execute(rows)
    finally:
        if board.record['rows']:
            output.write_text(json.dumps(board.record, indent=2) + '\n', encoding='utf-8')
    sys.stdout.write(render([record]))
    if args.delete:
        shutil.rmtree(board.build_dir, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
