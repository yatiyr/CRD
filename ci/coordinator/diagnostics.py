"""Diagnostics of a failed job (CI-1, ADR-0135 §17): collect, type, scrub, link and reproduce.

When a lane fails, its `upload` phase (which always runs) calls `collect` before the upload. `collect` gathers what
the engine and the toolchain left behind for this run (files newer than the lane's start), types every item and
writes it under `<build dir>/diagnostics/<kind>/`, with `diagnostics/summary.json` naming the failed phase, the failed
tests, the CPU level, the exact test command and every item (or why one was withheld). The agent uploads the tree;
the coordinator types each artifact by its `diagnostics/<kind>/` path, and the web UI links them from the failed
step, the job page and the QUAL cell. `ops.py job reproduce <id>` downloads them with the job's reproducibility
record and prints (or runs) the exact local replay commands for that commit, preset and CPU level.

Kinds and where they come from (docs/design/runtime-diagnostics.md, docs/design/diagnostic-runbooks.md):

- crash-bundle, flight-record: cerid-diagnostics/1 bundles (`*.cdb`, `flight-<n>.cdb`, DIAG.5d and DIAG.9b)
- crash-record: the async-signal-safe crash record (`crash_*.log`, DIAG.5a/5b)
- crash-dump, gpu-dump, core-dump: raw memory (`crash_*.dmp`, `gpu_*.dmp`, `core`, `core.<pid>`), restricted
- replay-record: a CEIR run record (`*.crpl`, DIAG.9a)
- perfetto-trace, profile-capture: Perfetto trace-events JSON (`*.perfetto.json`, `*.pftrace`, DIAG.6c) and CPROF
  captures (`*.cprof`)
- sanitizer-report: ASan, LSan, TSan, UBSan and MSan reports cut out of the test output, one file per test
- gpu-validation: Vulkan validation layer, D3D12 debug layer and Metal API validation messages, one file per test
- summary: `summary.json`; the reproducibility record itself stays on the job (`job.repro`)

Privacy (ADR-0133's redaction contract, ADR-0135 §17): raw memory is restricted. It can hold the test process's
environment, so the job's secrets, and the engine itself never declares a raw dump redacted (ID-8). Raw dumps are
withheld unless the device opts in (`"diagnostics": {"raw_dumps": true}` in ci/devices.json, for an owned machine
whose jobs carry no secret); a bundle keeps every other section with its RawDump section replaced by an explicit
absent one. Every text item has the job's secret values replaced, and the agent scrubs every uploaded file again
(ci/agent/agent.py scrub_artifact). A withheld item stays in the summary by size and SHA-256, never silently.
"""
from __future__ import annotations

from pathlib import Path
import fnmatch
import hashlib
import importlib.util
import json
import os
import re
import shlex
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

SCHEMA = 'cerid-ci-diagnostics/1'
CI = Path(__file__).resolve().parents[1]

# kind: (title, restricted raw memory)
KINDS = {
    'crash-bundle': ('Crash bundle', False),
    'flight-record': ('Flight record', False),
    'crash-record': ('Crash record', False),
    'crash-dump': ('Crash dump', True),
    'gpu-dump': ('GPU crash dump', True),
    'core-dump': ('Core dump', True),
    'replay-record': ('Replay record', False),
    'perfetto-trace': ('Perfetto trace', False),
    'profile-capture': ('Profile capture', False),
    'sanitizer-report': ('Sanitizer report', False),
    'gpu-validation': ('GPU validation', False),
    'summary': ('Diagnostics summary', False),
    'gold-image': ('Rendered image', False),
    'gold-manifest': ('Gold manifest', False),
    'junit': ('Test results', False),
    'test-log': ('Test log', False),
    'evidence': ('Evidence', False),
    'file': ('File', False),
}
DIAGNOSTIC_KINDS = ('crash-bundle', 'flight-record', 'crash-record', 'crash-dump', 'gpu-dump', 'core-dump',
                    'replay-record', 'perfetto-trace', 'profile-capture', 'sanitizer-report', 'gpu-validation',
                    'summary')
# (file name pattern, kind); the first match wins.
PATTERNS = (
    ('crash_*.dmp', 'crash-dump'), ('crash_*.log', 'crash-record'), ('gpu_*.dmp', 'gpu-dump'),
    ('flight-*.cdb', 'flight-record'), ('*.cdb', 'crash-bundle'), ('*.crpl', 'replay-record'),
    ('*.perfetto.json', 'perfetto-trace'), ('*.pftrace', 'perfetto-trace'), ('*.perfetto-trace', 'perfetto-trace'),
    ('*.cprof', 'profile-capture'), ('core', 'core-dump'), ('core.[0-9]*', 'core-dump'),
)
SKIP_DIRS = {'_deps', 'CMakeFiles', 'diagnostics', 'evidence', '.cmake', '.git', 'gold'}
MAX_ITEM_BYTES = 90 * 1024 * 1024    # under the coordinator's 100 MiB artifact limit
MAX_TOTAL_BYTES = 400 * 1024 * 1024  # per job
MAX_REPORT_LINES = 2000

SANITIZER_START = re.compile(r'^(?:==\d+==\s*)?(?:ERROR|WARNING): (AddressSanitizer|LeakSanitizer|ThreadSanitizer|'
                             r'MemorySanitizer|UndefinedBehaviorSanitizer)\b')
UBSAN_LINE = re.compile(r'^\S.*:\d+(?::\d+)?: runtime error: ')
SANITIZER_END = re.compile(r'^SUMMARY: (\w+Sanitizer)')
GPU_VALIDATION = re.compile(r'Validation (?:Error|Warning)|\bVUID-|\bUNASSIGNED-|D3D12 (?:ERROR|CORRUPTION|WARNING):|'
                            r'DXGI (?:ERROR|WARNING):|GPU-BASED VALIDATION|-\[MTLDebug|Metal API Validation|'
                            r'validateFunctionArguments')
CTEST_TEST = re.compile(r'^\s*\d+/\d+ Test(?:ing)?: (.+?)\s*$')
SAFE = re.compile(r'[^A-Za-z0-9._-]+')


def agent_module():
    """ci/agent/agent.py (standalone on machines, so loaded by path): the CDB1 reader and writer and the scrubber."""
    module = sys.modules.get('cerid_ci_agent')
    if module is None:
        spec = importlib.util.spec_from_file_location('cerid_ci_agent', CI / 'agent' / 'agent.py')
        module = importlib.util.module_from_spec(spec)
        sys.modules['cerid_ci_agent'] = module
        spec.loader.exec_module(module)
    return module


def artifact_kind(path):
    """The kind of an uploaded artifact by its path: `diagnostics/<kind>/...` names it; otherwise its file name."""
    parts = str(path).replace('\\', '/').split('/')
    if 'diagnostics' in parts:
        index = parts.index('diagnostics')
        if index + 2 < len(parts) and parts[index + 1] in KINDS:
            return parts[index + 1]
        if parts[-1] == 'summary.json':
            return 'summary'
    if 'gold' in parts[:-1] and parts[-1] == 'manifest.json':
        return 'gold-manifest'
    if 'gold' in parts[:-1] and parts[-1].lower().endswith('.png'):
        return 'gold-image'
    name = parts[-1]
    for pattern, kind in PATTERNS:
        if fnmatch.fnmatchcase(name, pattern):
            return kind
    if name.startswith('ctest-junit') and name.endswith('.xml'):
        return 'junit'
    if 'evidence' in parts:
        return 'evidence'
    if name.endswith('.log') and 'Temporary' in parts:
        return 'test-log'
    return 'file'


def kind_of_file(name):
    for pattern, kind in PATTERNS:
        if fnmatch.fnmatchcase(name, pattern):
            return kind
    return None


# --------------------------------------------------------------------------------------------- test output

def split_by_test(text):
    """[(test or '', line)] of a CTest LastTest.log (or any test output): the test whose section a line is in."""
    test = ''
    for line in text.splitlines():
        match = CTEST_TEST.match(line)
        if match:
            test = match.group(1)
        yield test, line


def sanitizer_reports(text):
    """[{'test', 'sanitizer', 'text'}]: each report from its first line to its SUMMARY line (UBSan's one-line
    reports with their stack), at most MAX_REPORT_LINES lines each."""
    reports = []
    current = None
    for test, line in split_by_test(text):
        if current is not None:
            if current['one_line'] and not line.startswith((' ', '\t')) and not SANITIZER_END.match(line):
                reports.append(current)  # a UBSan report ends with its stack; this line may start the next one
                current = None
            else:
                current['lines'].append(line)
                if SANITIZER_END.match(line) or len(current['lines']) >= MAX_REPORT_LINES:
                    reports.append(current)
                    current = None
                continue
        start = SANITIZER_START.match(line)
        if start:
            current = {'test': test, 'sanitizer': start.group(1), 'lines': [line], 'one_line': False}
        elif UBSAN_LINE.match(line):
            current = {'test': test, 'sanitizer': 'UndefinedBehaviorSanitizer', 'lines': [line], 'one_line': True}
    if current is not None:
        reports.append(current)
    return [{'test': r['test'], 'sanitizer': r['sanitizer'], 'text': '\n'.join(r['lines']) + '\n'} for r in reports]


def gpu_validation(text):
    """{test: [message line, ...]} of the validation layers' messages in the test output."""
    found = {}
    for test, line in split_by_test(text):
        if GPU_VALIDATION.search(line):
            lines = found.setdefault(test, [])
            if len(lines) < MAX_REPORT_LINES:
                lines.append(line)
    return found


def failed_tests(junits):
    names = []
    for path in junits:
        try:
            root = ET.parse(path).getroot()
        except (OSError, ET.ParseError):
            continue
        for case in root.iter('testcase'):
            if (case.find('failure') is not None or case.find('error') is not None) and case.get('name') not in names:
                names.append(case.get('name'))
    return names


# --------------------------------------------------------------------------------------------- collection

def find_files(roots, since, skip=SKIP_DIRS, depth=12):
    """[(path, kind)] of engine diagnostics under the roots, modified at or after `since` (a run's own files, never a
    previous run's left on a long-lived machine)."""
    found = []
    seen = set()
    for root in roots:
        root = Path(root)
        if not root.is_dir():
            continue
        base = len(root.parts)
        for directory, dirs, files in os.walk(root):
            if len(Path(directory).parts) - base >= depth:
                dirs[:] = []
            dirs[:] = sorted(d for d in dirs if d not in skip)
            for name in sorted(files):
                kind = kind_of_file(name)
                if kind is None:
                    continue
                path = Path(directory) / name
                try:
                    stat = path.stat()
                except OSError:
                    continue
                key = os.path.normcase(str(path.resolve()))
                if stat.st_mtime + 2 < since or key in seen:
                    continue
                seen.add(key)
                found.append((path, kind))
    return found


def strip_raw_dump(data):
    """(bytes, stripped payload bytes, note) of a CDB1 bundle with every RawDump section an explicit absent section;
    None when it is not a readable bundle."""
    agent = agent_module()
    parsed = agent.bundle_sections(data)
    if parsed is None:
        return None
    header, sections, complete = parsed
    stripped = 0
    kept = []
    for tag, flags, payload, original in sections:
        if tag == agent.SECTION_RAW_DUMP and payload:
            stripped += len(payload)
            kept.append((tag, agent.SECTION_ABSENT, b'', 0))
        else:
            kept.append((tag, flags, payload, original))
    notes = []
    if stripped:
        notes.append('raw dump section (%d bytes) left out' % stripped)
    if not complete:
        notes.append('recovered %d complete sections of a truncated bundle' % len(sections))
    return agent.bundle_bytes(header, kept), stripped, '; '.join(notes)


def item_name(path, roots):
    path = Path(path)
    for root in roots:
        try:
            relative = path.resolve().relative_to(Path(root).resolve())
            return SAFE.sub('_', '__'.join(relative.parts))[-180:]
        except ValueError:
            continue
    return SAFE.sub('_', path.name)[-180:]


def collect(build_dir, since, context, secrets=(), raw_dumps=False, extra_roots=(), logs=None, junits=None,
            max_item=MAX_ITEM_BYTES, max_total=MAX_TOTAL_BYTES, root=None):
    """Collect a failed run's diagnostics into `<build_dir>/diagnostics/` and return the summary (also written as
    `summary.json`; item paths are relative to `root`, the checkout, as the upload names them). `context`: preset,
    test_preset, device, commit, build, job, phase, cpu_level, ctest, lane_command."""
    agent = agent_module()
    build_dir = Path(build_dir)
    root = Path(root) if root is not None else build_dir.parent.parent
    out = build_dir / 'diagnostics'
    secrets = [s for s in secrets if s]
    temporary = build_dir / 'Testing' / 'Temporary'
    logs = list(logs) if logs is not None else sorted(temporary.glob('LastTest*.log'))
    junits = list(junits) if junits is not None else sorted(temporary.glob('ctest-junit*.xml'))
    roots = [build_dir] + [Path(r) for r in extra_roots]
    items = []
    total = [0]

    def scrub_text(text):
        data, _ = agent.scrub_artifact('report.txt', text.encode('utf-8'), secrets)
        return data

    def place(kind, name, data, source, test=None, note=None):
        entry = {'kind': kind, 'source': str(source), 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                 'test': test, 'note': note}
        if len(data) > max_item:
            entry['withheld'] = 'larger than the %d MiB artifact limit' % (max_item // (1024 * 1024))
        elif total[0] + len(data) > max_total:
            entry['withheld'] = 'over the %d MiB diagnostics limit of a job' % (max_total // (1024 * 1024))
        else:
            target = out / kind / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            total[0] += len(data)
            try:
                entry['path'] = os.path.relpath(target, root).replace('\\', '/')
            except ValueError:  # another drive: the upload cannot name it relative to the checkout
                entry['path'] = target.as_posix()
        items.append(entry)

    for path, kind in find_files([build_dir], since) + find_files(extra_roots, since, depth=4):
        name = item_name(path, roots)
        try:
            size = path.stat().st_size
        except OSError:
            continue
        restricted = KINDS[kind][1]
        if restricted and not raw_dumps:
            items.append({'kind': kind, 'source': str(path), 'size': size, 'sha256': file_sha256(path),
                          'withheld': 'raw memory is restricted (it can hold the process environment); the device '
                                      'does not opt in to raw dumps', 'test': None, 'note': None})
            continue
        if size > max_item:
            items.append({'kind': kind, 'source': str(path), 'size': size, 'sha256': file_sha256(path),
                          'withheld': 'larger than the %d MiB artifact limit' % (max_item // (1024 * 1024)),
                          'test': None, 'note': None})
            continue
        data = path.read_bytes()
        note = None
        if kind in ('crash-bundle', 'flight-record') and not raw_dumps:
            stripped = strip_raw_dump(data)
            if stripped is None:
                items.append({'kind': kind, 'source': str(path), 'size': size, 'sha256': hashlib.sha256(
                    data).hexdigest(), 'withheld': 'not a readable cerid-diagnostics bundle, so it may carry raw '
                                                   'memory', 'test': None, 'note': None})
                continue
            data, _, note = stripped
        cleaned, scrub_note = agent.scrub_artifact(path.name, data, secrets)
        if cleaned is None:
            items.append({'kind': kind, 'source': str(path), 'size': size, 'sha256': hashlib.sha256(
                data).hexdigest(), 'withheld': scrub_note, 'test': None, 'note': note})
            continue
        place(kind, name, cleaned, path, note='; '.join(n for n in (note, scrub_note) if n) or None)

    text = ''
    for log in logs:
        try:
            text += Path(log).read_text(encoding='utf-8', errors='replace') + '\n'
        except OSError:
            continue
    counts = {}
    for report in sanitizer_reports(text):
        test = report['test'] or 'unattributed'
        counts[test] = counts.get(test, 0) + 1
        name = '%s-%d-%s.txt' % (SAFE.sub('_', test)[:120], counts[test], report['sanitizer'])
        place('sanitizer-report', name, scrub_text(report['text']), 'test output', test=report['test'] or None,
              note=report['sanitizer'])
    for test, lines in sorted(gpu_validation(text).items()):
        name = '%s.txt' % SAFE.sub('_', test or 'unattributed')[:120]
        place('gpu-validation', name, scrub_text('\n'.join(lines) + '\n'), 'test output', test=test or None,
              note='%d messages' % len(lines))

    summary = dict({k: context.get(k) for k in ('preset', 'test_preset', 'device', 'commit', 'build', 'job', 'phase',
                                                 'cpu_level', 'ctest', 'lane_command')},
                   schema=SCHEMA, failed_tests=failed_tests(junits), items=items,
                   privacy={'raw_dumps': bool(raw_dumps), 'secret_values_scrubbed': len(secrets)},
                   collected=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
    out.mkdir(parents=True, exist_ok=True)
    (out / 'summary.json').write_text(json.dumps(summary, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    return summary


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def describe(summary):
    """One line per item for the job log."""
    lines = ['diagnostics: %d items, %d failed tests (%s)' % (
        len(summary['items']), len(summary['failed_tests']), ', '.join(summary['failed_tests'][:5]) or 'none named')]
    for item in summary['items']:
        lines.append('  %-16s %s%s' % (item['kind'], item.get('path') or item['source'],
                                       ' WITHHELD: ' + item['withheld'] if item.get('withheld') else ''))
    return lines


def default_roots():
    """Where a crashed test leaves files besides the build tree: the system temporary directory."""
    return [tempfile.gettempdir()]


# --------------------------------------------------------------------------------------------- reproduction

def regex_for(tests):
    return '^(%s)$' % '|'.join(re.escape(t) for t in tests) if tests else None


def quote(argument, windows):
    if windows:
        return '"%s"' % argument if re.search(r'[\s|^&<>()"]', argument) else argument
    return shlex.quote(argument)


def reproduce_steps(job, summary, files, windows, checkout='.'):
    """[(title, command or None, note)] that reproduce a failed job locally: check out the commit, run the lane as
    CI ran it (the failed tests alone), run the failed tests at the job's CPU level, and open each diagnostic with
    the tool that reads it (the commands of docs/design/diagnostic-runbooks.md)."""
    repro = job.get('repro') or {}
    commit = repro.get('commit') or job.get('commit') or (summary or {}).get('commit')
    env = job.get('env') or {}
    summary = summary or {}
    preset = summary.get('preset') or env.get('CERID_CI_PRESET')
    device = summary.get('device') or env.get('CERID_CI_DEVICE')
    tests = summary.get('failed_tests') or []
    pattern = regex_for(tests)
    steps = []
    if commit:
        steps.append(('Check out the commit in a separate worktree',
                      'git worktree add --detach %s %s' % (quote(checkout, windows), commit),
                      'or `git checkout --detach %s` in a clean checkout' % commit))
    if preset and device:
        lane = 'python ci/pipeline.py lane --preset %s --device %s' % (preset, device)
        if pattern:
            lane += ' --tests %s' % ('"%s"' % pattern)
        steps.append(('Run the lane as CI ran it' + (' (the failed tests only)' if pattern else ''), lane,
                      'configures, builds and tests exactly as the %s cell does' % device))
    else:
        steps.append(('Run the step as CI ran it', job.get('command'), None))
    test_preset = summary.get('test_preset') or preset
    if test_preset:
        level = summary.get('cpu_level')
        ctest_preset = test_preset + '-at-sse2' if level == 'sse2' else test_preset
        command = 'ctest --preset %s --output-on-failure' % ctest_preset
        if pattern:
            command += ' -R "%s"' % pattern
        note = None
        if level and level not in ('native', 'sse2'):
            command = ('set CRD_CPU_LEVEL=%s&& ' if windows else 'CRD_CPU_LEVEL=%s ') % level + command
        if level:
            note = 'at the CPU level %s, as the failed phase ran' % level
        steps.append(('Run the failed tests in the built tree', command, note))
    ceridc = 'build/%s/tools/ceridc/ceridc%s' % (preset or '<preset>', '.exe' if windows else '')
    for path, kind in files:
        local = str(path)
        parent = str(Path(path).parent)
        if kind in ('crash-bundle', 'flight-record'):
            steps.append(('Inspect %s' % Path(path).name, '%s diag --command bundle.inspect --path %s --root %s' % (
                ceridc, quote(local, windows), quote(parent, windows)), 'runbook: docs/design/diagnostic-runbooks.md'
                '#rb-fatal'))
        elif kind == 'replay-record':
            steps.append(('Replay %s to its first divergence' % Path(path).name,
                          '%s diag --command replay.run --grant read,execute --path %s --root %s' % (
                              ceridc, quote(local, windows), quote(parent, windows)),
                          'runbook: docs/design/diagnostic-runbooks.md#rb-replay'))
        elif kind == 'perfetto-trace':
            steps.append(('Open %s' % Path(path).name, None, 'in https://ui.perfetto.dev (Open trace file); the '
                                                             'file stays on this machine'))
        elif kind == 'crash-dump' and local.endswith('.dmp'):
            steps.append(('Analyse %s' % Path(path).name, 'cdb -z %s -c "!analyze -v; q"' % quote(local, windows),
                          'with the build\'s PDBs on the symbol path'))
        elif kind == 'core-dump':
            steps.append(('Analyse %s' % Path(path).name, 'gdb <test executable> %s' % quote(local, windows), None))
        elif kind in ('sanitizer-report', 'gpu-validation', 'crash-record'):
            steps.append(('Read %s' % Path(path).name, None, local))
    return steps


def diagnostic_entries(entries, job_id, attempt=None):
    """The artifacts of one job attempt (the newest by default) that are diagnostics."""
    mine = [e for e in entries if e['job'] == job_id]
    if attempt is None and mine:
        attempt = max(e['attempt'] for e in mine)
    return [dict(e, kind=e.get('kind') or artifact_kind(e['path'])) for e in mine if e['attempt'] == attempt and (
        e.get('kind') or artifact_kind(e['path'])) in DIAGNOSTIC_KINDS + ('junit',)]
