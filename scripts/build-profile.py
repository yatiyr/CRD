#!/usr/bin/env python3
"""Compile-time profile of one preset: which translation units, headers, classes, templates and functions cost the
most to compile.

`run` configures the preset into a fresh scratch directory with no compiler launcher (a cache hit never runs the
compiler, so it would hide every unit it serves) and the profiling switches of its compiler, builds it with an
explicit job count and writes a profile document:

- MSVC (cl): `/Bt+` (front-end and back-end time per unit) and `/d1reportTime` (per unit: the include tree with each
  header's inclusive parse time, and the time of every class and function definition). Both are compiler switches, so
  they need no elevation, unlike C++ Build Insights (vcperf needs an elevated shell, or a one-time elevated
  `vcperf /grantusercontrol`). The output reaches Ninja's log, which is parsed as one stream.
- Clang: `-ftime-trace` (one JSON per object), read by the pinned ClangBuildAnalyzer
  (`scripts/install-clang-build-analyzer.py`), whose report is parsed into the same document shape.

Per unit the document keeps the front-end and back-end seconds and the Ninja edge time; per header the summed
inclusive time (a header's own parse plus what it includes, over every unit that parses it), the summed exclusive time
(its own text only; these add up to the whole build's include time) and the units that parse it; per class, function
or template the summed time and the units. The host state is recorded before and after, as build-bench.py does, and a
contended host is refused unless `--allow-contended`.

`parse-msvc <log>` and `parse-clang <report>` rebuild a document from a kept log or report; `render` writes the
markdown tables docs/bench stores; `compare <before> <after>` the differences. Contract:
docs/design/build-performance.md#compile-time-profiles.
"""
from pathlib import Path
import argparse
import datetime
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
_bench_spec = importlib.util.spec_from_file_location('build_bench', ROOT / 'scripts/build-bench.py')
bench = importlib.util.module_from_spec(_bench_spec)
_bench_spec.loader.exec_module(bench)
SCHEMA = 'cerid-build-profile/1'
TOP = 40
MSVC_FLAGS = '/Bt+ /d1reportTime'
CLANG_FLAGS = '-ftime-trace'
STATUS = re.compile(r'^\[(\d+)/(\d+)\] (.*)$')
BT = re.compile(r'^time\((.+?)\)=([0-9.]+)s .*\[(.+)\]\s*$')
TIMED = re.compile(r'^(\t+)(.+): ([0-9.]+)s\s*$')
SECTION = {'Include Headers:': 'includes', 'Class Definitions:': 'classes', 'Function Definitions:': 'functions'}
CBA_SECTION = re.compile(r'^\*\*\*\* (.+?):?\s*$')
CBA_LINE = re.compile(r'^\s*(\d+) ms: (.+?)(?: \(([^()]*)\))?(?:, included via:.*)?\s*$')
CBA_KEYS = {'Files that took longest to parse (compiler frontend)': 'frontend',
            'Files that took longest to codegen (compiler backend)': 'backend',
            'Templates that took longest to instantiate': 'templates',
            'Template sets that took longest to instantiate': 'template_sets',
            'Functions that took longest to compile': 'functions',
            'Function sets that took longest to compile / optimize': 'function_sets',
            'Expensive headers': 'headers'}


def normal_path(path):
    return path.strip().replace('\\', '/')


def source_key(path, root=ROOT):
    """A path relative to the checkout when it lies inside it (lower-case drive letters compared), else as given."""
    text = normal_path(path)
    base = normal_path(str(root)).rstrip('/') + '/'
    if text.lower().startswith(base.lower()):
        return text[len(base):]
    return text


class Accumulator:
    """name -> [summed seconds, units]; the units counted once per unit."""

    def __init__(self):
        self.items = {}

    def add(self, name, seconds, unit):
        entry = self.items.setdefault(name, [0.0, set()])
        entry[0] += seconds
        entry[1].add(unit)

    def top(self, count=TOP):
        rows = sorted(self.items.items(), key=lambda item: -item[1][0])[:count]
        return [{'name': name, 'seconds': round(total, 3), 'units': len(units)} for name, (total, units) in rows]

    def total(self):
        return sum(entry[0] for entry in self.items.values())


class MsvcProfile:
    """Reads Ninja's output of a build compiled with /Bt+ /d1reportTime, one line at a time."""

    def __init__(self, root=ROOT):
        self.root = root
        self.units = {}
        self.headers = Accumulator()
        self.header_self = Accumulator()
        self.classes = Accumulator()
        self.functions = Accumulator()
        self.edge = None
        self.unit = None
        self.section = None
        self.stack = []  # (depth, header, inclusive seconds, children seconds)

    def _unit(self):
        if self.unit is None:
            self.unit = {'edge': self.edge, 'source': None, 'frontend_s': 0.0, 'backend_s': 0.0,
                         'include_s': 0.0, 'class_s': 0.0, 'function_s': 0.0}
        return self.unit

    def _close_includes(self, depth=-1):
        while self.stack and self.stack[-1][0] > depth:
            d, header, inclusive, children = self.stack.pop()
            self.header_self.add(header, max(0.0, inclusive - children), self._key())
            if self.stack:
                self.stack[-1][3] += inclusive

    def _key(self):
        unit = self._unit()
        return unit['source'] or unit['edge'] or '?'

    def _finish_unit(self):
        self._close_includes()
        if self.unit is not None:
            key = self._key()
            previous = self.units.get(key)
            if previous is not None:
                for field in ('frontend_s', 'backend_s', 'include_s', 'class_s', 'function_s'):
                    previous[field] += self.unit[field]
            else:
                self.units[key] = self.unit
        self.unit = None
        self.section = None

    def feed(self, line):
        line = line.rstrip('\r\n')
        status = STATUS.match(line)
        if status:
            self._finish_unit()
            self.edge = status.group(3)
            return
        bt = BT.match(line)
        if bt:
            unit = self._unit()
            source = source_key(bt.group(3), self.root)
            if unit['source'] is None:
                unit['source'] = source
            phase = Path(normal_path(bt.group(1))).name.lower()
            seconds = float(bt.group(2))
            if phase in ('c1xx.dll', 'c1.dll'):
                unit['frontend_s'] += seconds
            elif phase == 'c2.dll':
                unit['backend_s'] += seconds
            return
        stripped = line.strip()
        if stripped in SECTION:
            self._close_includes()
            self._unit()
            self.section = SECTION[stripped]
            return
        if self.section is None:
            return
        if stripped.startswith('Count:') or stripped.startswith('Top ') or not stripped:
            if stripped.startswith('Top '):
                self._close_includes()
                self.section = 'top'
            return
        if stripped.startswith('Total:'):
            self._close_includes()
            self.section = None
            return
        if self.section == 'top':
            return
        timed = TIMED.match(line)
        if not timed:
            return
        depth = len(timed.group(1)) - 2
        name = timed.group(2)
        seconds = float(timed.group(3))
        unit = self._unit()
        key = self._key()
        if self.section == 'includes':
            self._close_includes(depth - 1)
            header = source_key(name, self.root)
            self.headers.add(header, seconds, key)
            if depth == 0:
                unit['include_s'] += seconds
            self.stack.append([depth, header, seconds, 0.0])
        elif self.section == 'classes':
            self.classes.add(name, seconds, key)
            unit['class_s'] += seconds
        elif self.section == 'functions':
            self.functions.add(name, seconds, key)
            unit['function_s'] += seconds

    def close(self):
        self._finish_unit()

    def document(self, top=TOP):
        units = sorted(self.units.values(), key=lambda u: -(u['frontend_s'] + u['backend_s']))
        frontend = sum(u['frontend_s'] for u in units)
        backend = sum(u['backend_s'] for u in units)
        return {'compiler': 'msvc', 'units': len(units), 'frontend_s': round(frontend, 1),
                'backend_s': round(backend, 1), 'include_s': round(sum(u['include_s'] for u in units), 1),
                'top_units': [dict(u, frontend_s=round(u['frontend_s'], 3), backend_s=round(u['backend_s'], 3),
                                   include_s=round(u['include_s'], 3), class_s=round(u['class_s'], 3),
                                   function_s=round(u['function_s'], 3)) for u in units[:top]],
                'headers': self.headers.top(top), 'headers_exclusive': self.header_self.top(top),
                'classes': self.classes.top(top), 'functions': self.functions.top(top)}


def parse_msvc(lines, root=ROOT, top=TOP):
    profile = MsvcProfile(root)
    for line in lines:
        profile.feed(line)
    profile.close()
    return profile.document(top)


def parse_clang_report(text, root=ROOT):
    """ClangBuildAnalyzer's --analyze report: the time summary and every `<n> ms: <name> (<detail>)` list."""
    document = {'compiler': 'clang', 'summary': {}}
    section = None
    for line in text.splitlines():
        header = CBA_SECTION.match(line)
        if header:
            title = header.group(1).strip()
            section = CBA_KEYS.get(title, 'summary' if title.startswith('Time summary') else None)
            continue
        if section is None:
            continue
        if section == 'summary':
            match = re.match(r'^(.+?):\s+([0-9.]+) s\s*$', line.strip())
            if match:
                document['summary'][match.group(1)] = float(match.group(2))
            continue
        entry = CBA_LINE.match(line)
        if not entry:
            continue
        name = entry.group(2)
        row = {'name': source_key(name, root) if section in ('frontend', 'backend', 'headers') else name,
               'seconds': int(entry.group(1)) / 1000.0}
        detail = entry.group(3) or ''
        units = re.search(r'included (\d+) times|(\d+) times', detail)
        if units:
            row['count'] = int(units.group(1) or units.group(2))
        document.setdefault(section, []).append(row)
    return document


CBA_INI = '[counts]\nfileParse=%d\nfileCodegen=%d\ntemplate=%d\nfunction=%d\nheader=%d\nheaderChain=3\n' \
          '[misc]\nmaxNameLength=200\nonlyRootHeaders=true\n'


def analyze_clang(tool, build_dir, work, top=TOP):
    """ClangBuildAnalyzer over every -ftime-trace JSON of the tree; returns (document, report text)."""
    work.mkdir(parents=True, exist_ok=True)
    (work / 'ClangBuildAnalyzer.ini').write_text(CBA_INI % ((top,) * 5), encoding='utf-8')
    capture = work / 'capture.bin'
    subprocess.run([str(tool), '--all', str(build_dir), str(capture)], cwd=str(work), check=True,
                   stdout=subprocess.DEVNULL)
    report = subprocess.run([str(tool), '--analyze', str(capture)], cwd=str(work), check=True, capture_output=True,
                            text=True, encoding='utf-8', errors='replace').stdout
    report = re.sub(r'\x1b\[[0-9;]*m', '', report)
    (work / 'report.txt').write_text(report, encoding='utf-8')
    return parse_clang_report(report), report


def edge_times(build_dir):
    """Per object: the Ninja edge seconds of the profiling build (the last log line of each output)."""
    log = Path(build_dir) / '.ninja_log'
    result = {}
    if log.is_file():
        for start, end, output in bench.ninja_entries(log):
            result[output] = (end - start) / 1000.0
    return result


def configure_command(preset, build_dir, compiler, defines):
    flags = MSVC_FLAGS if compiler == 'msvc' else CLANG_FLAGS
    command = ['cmake', '--preset', preset, '-B', str(build_dir), '-DCMAKE_CXX_FLAGS_INIT=' + flags,
               '-DCMAKE_C_FLAGS_INIT=' + ('/Bt+' if compiler == 'msvc' else CLANG_FLAGS),
               '-DCRD_COMPILER_LAUNCHER=']
    return command + ['-D' + d for d in defines]


def vcperf_commands(vcperf, session, stem, noadmin=False):
    """C++ Build Insights around the build: start, then stop into a raw trace and analyse it twice (the WPA trace with
    template instantiations, and a Chrome trace-event JSON)."""
    start = [vcperf, '/start'] + (['/noadmin'] if noadmin else []) + [session]
    raw = str(stem) + '-raw.etl'
    stop = [vcperf, '/stopnoanalyze', session, raw]
    analyze = [[vcperf, '/analyze', '/templates', raw, str(stem) + '.etl'],
               [vcperf, '/analyze', '/templates', raw, '/timetrace', str(stem) + '.json']]
    return start, stop, analyze


def run_profile(args, runner=subprocess.run, host=bench.host_state, which=shutil.which):
    build_dir = Path(args.build_dir)
    if build_dir.exists():
        raise SystemExit('%s exists; the profile needs a fresh scratch directory' % build_dir)
    vcperf = None
    if getattr(args, 'vcperf', None):
        if args.compiler != 'msvc':
            raise SystemExit('--vcperf profiles an MSVC build')
        vcperf = which('vcperf')
        if vcperf is None:
            raise SystemExit('vcperf is not on PATH (run from the MSVC environment: scripts/msvc-env.bat)')
    states = host()
    reasons = [reason for state in states for reason in bench.contention(state)]
    print('host: ' + bench.host_summary(states), flush=True)
    if reasons and not args.allow_contended:
        raise SystemExit('the host is not idle (%s); profile on an idle host or pass --allow-contended'
                         % '; '.join(reasons))
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    log_path = output.with_suffix('.log')
    started = time.perf_counter()
    configure = configure_command(args.preset, build_dir, args.compiler, args.define)
    if runner(configure, cwd=str(ROOT), check=False).returncode != 0:
        raise SystemExit('configure failed: ' + ' '.join(configure))
    configured = time.perf_counter()
    build = ['cmake', '--build', str(build_dir), '--parallel', str(args.jobs)]
    for target in args.target:
        build += ['--target', target]
    insights = None
    if vcperf:
        start, stop, analyze = vcperf_commands(vcperf, args.vcperf, output.with_suffix('').with_name(
            output.stem + '-vcperf'), getattr(args, 'vcperf_noadmin', False))
        started_trace = runner(start, cwd=str(ROOT), check=False, capture_output=True, text=True)
        if started_trace.returncode != 0:
            raise SystemExit('vcperf could not start (%s): it needs an elevated shell, or a one-time elevated '
                             '`vcperf /grantusercontrol` and --vcperf-noadmin'
                             % ((started_trace.stdout or '') + (started_trace.stderr or '')).strip()[-300:])
        insights = {'session': args.vcperf, 'start': start}
    try:
        with log_path.open('w', encoding='utf-8', errors='replace') as log:
            code = runner(build, cwd=str(ROOT), stdout=log, stderr=subprocess.STDOUT, check=False).returncode
    finally:
        if insights is not None:
            insights['stop_exit'] = runner(stop, cwd=str(ROOT), check=False).returncode
    if insights is not None:
        insights['analyze_exits'] = [runner(command, cwd=str(ROOT), check=False).returncode for command in analyze]
        insights['outputs'] = [analyze[0][-1], analyze[1][-1]]
    built = time.perf_counter()
    if args.compiler == 'msvc':
        with log_path.open('r', encoding='utf-8', errors='replace') as log:
            document = parse_msvc(log, top=args.top)
    else:
        tool = Path(args.analyzer).expanduser()
        document, _ = analyze_clang(tool, build_dir, output.parent / (output.stem + '-cba'), args.top)
    edges = edge_times(build_dir)
    document.update({'schema': SCHEMA, 'label': args.label, 'preset': args.preset, 'jobs': args.jobs,
                     'defines': args.define, 'targets': args.target, 'build_exit': code,
                     'configure_s': round(configured - started, 1), 'build_s': round(built - configured, 1),
                     'edges': len(edges), 'edge_s': round(sum(edges.values()), 1),
                     'slowest_edges': [{'output': o, 'seconds': s} for o, s in
                                       sorted(edges.items(), key=lambda item: -item[1])[:args.top]],
                     'host_before': states, 'host_after': host(), 'contended': reasons, 'build_insights': insights,
                     'machine': bench.machine(), 'log': str(log_path),
                     'finished': datetime.datetime.now(datetime.timezone.utc).isoformat()})
    output.write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
    print('profile: %s (build exit %d, %.0f s)' % (output, code, built - configured))
    return 0 if code == 0 else 1


# ---- rendering ------------------------------------------------------------------------------------------------------

def render(document, count=15):
    lines = ['- **%s**: preset `%s`, %s, jobs %s, defines `%s`; build %s s (edge sum %s s, %s edges).' % (
        document.get('label', '?'), document.get('preset', '?'), document.get('compiler'), document.get('jobs', '?'),
        ' '.join(document.get('defines') or []) or 'none', document.get('build_s', '?'), document.get('edge_s', '?'),
        document.get('edges', '?'))]
    if 'host_before' in document:
        lines.append('  host before: %s.' % bench.host_summary(document['host_before']))
    if document.get('compiler') == 'msvc':
        lines.append('  front end %.0f s, back end %.0f s, include parse %.0f s over %d units.' % (
            document['frontend_s'], document['backend_s'], document['include_s'], document['units']))
        tables = (('Units (front + back end)', [
            {'name': u['source'] or u['edge'], 'seconds': round(u['frontend_s'] + u['backend_s'], 2),
             'units': '%.1f / %.1f' % (u['frontend_s'], u['backend_s'])} for u in document['top_units']]),
            ('Headers, inclusive', document['headers']), ('Headers, exclusive', document['headers_exclusive']),
            ('Classes', document['classes']), ('Functions', document['functions']))
    else:
        tables = (('Parse (front end)', document.get('frontend', [])), ('Codegen', document.get('backend', [])),
                  ('Headers', document.get('headers', [])), ('Templates', document.get('templates', [])),
                  ('Template sets', document.get('template_sets', [])),
                  ('Function sets', document.get('function_sets', [])))
    for title, rows in tables:
        lines += ['', '| %s | Seconds | Units |' % title, '|---|---|---|']
        for row in rows[:count]:
            lines.append('| `%s` | %s | %s |' % (str(row['name']).replace('|', '\\|')[:160], row['seconds'],
                                                 row.get('units', row.get('count', ''))))
    return '\n'.join(lines) + '\n'


def compare(before, after, key='headers', count=TOP):
    """Rows of one list in both documents: name, seconds before and after, difference."""
    old = {row['name']: row['seconds'] for row in before.get(key, [])}
    new = {row['name']: row['seconds'] for row in after.get(key, [])}
    names = sorted(set(old) | set(new), key=lambda n: -max(old.get(n, 0.0), new.get(n, 0.0)))[:count]
    return [{'name': n, 'before': old.get(n), 'after': new.get(n),
             'delta': None if n not in old or n not in new else round(new[n] - old[n], 3)} for n in names]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    run = sub.add_parser('run', help='profile one preset in a fresh scratch build directory')
    run.add_argument('--compiler', choices=('msvc', 'clang'), required=True)
    run.add_argument('--preset', required=True)
    run.add_argument('--build-dir', required=True)
    run.add_argument('--jobs', type=int, required=True)
    run.add_argument('--define', action='append', default=[])
    run.add_argument('--target', action='append', default=[])
    run.add_argument('--label', required=True)
    run.add_argument('--output', required=True, help='profile JSON (the build log is kept beside it)')
    run.add_argument('--analyzer', default='~/.cerid/tools/clang-build-analyzer/bin/ClangBuildAnalyzer')
    run.add_argument('--top', type=int, default=TOP)
    run.add_argument('--allow-contended', action='store_true')
    run.add_argument('--vcperf', metavar='SESSION',
                     help='also capture C++ Build Insights around the build (an elevated shell, see --vcperf-noadmin)')
    run.add_argument('--vcperf-noadmin', action='store_true',
                     help='start vcperf with /noadmin (after a one-time elevated `vcperf /grantusercontrol`)')
    parse = sub.add_parser('parse-msvc', help='rebuild a document from a kept MSVC build log')
    parse.add_argument('log')
    parse.add_argument('--output', required=True)
    parse.add_argument('--top', type=int, default=TOP)
    clang = sub.add_parser('parse-clang', help='rebuild a document from a kept ClangBuildAnalyzer report')
    clang.add_argument('report')
    clang.add_argument('--output', required=True)
    show = sub.add_parser('render', help='markdown tables of a profile document')
    show.add_argument('document')
    show.add_argument('--count', type=int, default=15)
    diff = sub.add_parser('compare', help='one list of two documents side by side')
    diff.add_argument('before')
    diff.add_argument('after')
    diff.add_argument('--key', default='headers')
    args = parser.parse_args(argv)
    if args.command == 'run':
        return run_profile(args)
    if args.command == 'parse-msvc':
        with open(args.log, 'r', encoding='utf-8', errors='replace') as log:
            document = parse_msvc(log, top=args.top)
        Path(args.output).write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
        return 0
    if args.command == 'parse-clang':
        document = parse_clang_report(Path(args.report).read_text(encoding='utf-8', errors='replace'))
        Path(args.output).write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
        return 0
    if args.command == 'render':
        sys.stdout.write(render(json.loads(Path(args.document).read_text(encoding='utf-8')), args.count))
        return 0
    rows = compare(json.loads(Path(args.before).read_text(encoding='utf-8')),
                   json.loads(Path(args.after).read_text(encoding='utf-8')), args.key)
    for row in rows:
        print('%-100s %10s %10s %10s' % (row['name'][:100], row['before'], row['after'], row['delta']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
