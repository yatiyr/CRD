"""Portable strict LLVM-20 analysis of specific C++ files with the configured build's real compile flags.

The compile database is the source of every flag. The only tokens removed are precompiled-header inputs, which clang
cannot consume from another compiler: MSVC `/Yu`, `/Fp` and the forced `/FI cmake_pch.hxx`; GCC `-include cmake_pch.hxx`
and `-Winvalid-pch`. clang-tidy drops `/`-spelled MSVC flags that reach it through the database (measured in the
CRD_ENABLE_CLANG_TIDY block of the root CMakeLists), so the analysis-relevant ones are restated through `--extra-arg`:
`/EHsc` and the CMake-owned `CRD_SIMD_MSVC_ARCH_FLAG` from the build cache. A GCC database carries warning flags clang
does not know and `-Werror`; `-Wno-unknown-warning-option` keeps those from reading as parse failures, and `-Wno-error`
keeps clang's own compiler warnings under GCC's flags from failing units GCC compiles clean (GCC is that
configuration's compiler of record; tidy checks stay warnings-as-errors). Headers are
analysed as the main file of a translation unit of their owning target (the plan's owners), then of a sibling in the
same module. A file with no usable command, an unresolved include, a tool exit without diagnostics or an unavailable
tool is reported as ungated or unavailable, never clean.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import time

from .environment import REQUIRED_TIDY_MAJOR, cache_values, resolve_clang_tidy
from .selection import SelectionError

DEFAULT_BUILD = 'build/win-debug' if os.name == 'nt' else 'build/linux-gcc-debug'
SOURCE_SUFFIXES = {'.cpp', '.cc', '.cxx'}
HEADER_SUFFIXES = {'.h', '.hpp', '.hxx', '.inl', '.inc', '.ipp'}
MSVC_DRIVERS = {'cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'}
QUOTED_OR_BARE = r'(?:"[^"]*"|[^\s"]*)'
PCH_COMMAND_TOKENS = [re.compile(r'(?<!\S)[-/]Yu' + QUOTED_OR_BARE + r'\s*'),
                      re.compile(r'(?<!\S)[-/]Fp' + QUOTED_OR_BARE + r'\s*'),
                      re.compile(r'(?<!\S)[-/]FI(?:"[^"]*cmake_pch[^"]*"|[^\s"]*cmake_pch[^\s"]*)\s*'),
                      re.compile(r'(?<!\S)-include\s*(?:"[^"]*cmake_pch[^"]*"|[^\s"]*cmake_pch[^\s"]*)\s*'),
                      re.compile(r'(?<!\S)-Winvalid-pch\s*')]
DIAGNOSTIC = re.compile(r'(?:warning|error):')
UNRESOLVED = 'file not found'
UNAVAILABLE_EXIT = 99


def normalized(path, directory=None):
    text = str(path)
    if directory and not os.path.isabs(text):
        text = os.path.join(str(directory), text)
    return os.path.normcase(os.path.normpath(os.path.abspath(text)))


def module_key(relative):
    """Case-folded path components (Windows paths compare case-insensitively) for module lookups."""
    return tuple(os.path.normcase(part) for part in relative.parts)


def first_token(command):
    command = command.lstrip()
    if command.startswith('"'):
        return command[1:command.find('"', 1)]
    return command.split(None, 1)[0] if command else ''


def compiler_family(entry):
    """'msvc' for cl/clang-cl command lines (slash-spelled flags), 'gnu' for every other driver."""
    driver = entry['arguments'][0] if entry.get('arguments') else first_token(entry.get('command', ''))
    return 'msvc' if Path(driver.replace('\\', '/')).name.lower() in MSVC_DRIVERS else 'gnu'


def strip_pch_command(command):
    for pattern in PCH_COMMAND_TOKENS:
        command = pattern.sub('', command)
    return command.strip()


def strip_pch_arguments(arguments):
    result, skip = [], False
    for index, token in enumerate(arguments):
        if skip:
            skip = False
            continue
        following = arguments[index + 1] if index + 1 < len(arguments) else ''
        if re.match(r'^[-/](Yu|Fp)', token) or re.match(r'^[-/]FI.*cmake_pch', token) or token == '-Winvalid-pch':
            continue
        if token == '-include' and 'cmake_pch' in following:
            skip = True
            continue
        if token.startswith('-include') and 'cmake_pch' in token:
            continue
        result.append(token)
    return result


def stripped(entry):
    result = dict(entry)
    if 'arguments' in entry:
        result['arguments'] = strip_pch_arguments(entry['arguments'])
    if 'command' in entry:
        result['command'] = strip_pch_command(entry['command'])
    return result


def extra_arguments(family, cache, header=False):
    """Flags restated on the one channel clang-tidy honors; the MSVC ISA flag is CMake's own cache value, never a
    literal. A header analysed as the main file legitimately carries `#pragma once`, so that single main-file
    diagnostic is disabled for header units only; every check and every other diagnostic stays strict."""
    if family == 'msvc':
        result = ['--extra-arg=/EHsc']
        arch = (cache.get('CRD_SIMD_MSVC_ARCH_FLAG') or '').strip()
        if arch:
            result.append('--extra-arg=' + arch)
        result.append('--extra-arg=-Wno-unused-command-line-argument')
    else:
        # GCC is the compiler of record for a GCC database (CI compiles that configuration with -Werror). clang's
        # own opinion of GCC's warning flags is not the gate: measured 2026-09-13, -Wpedantic reports
        # nested-anon-types in containers/string.hpp and -Wconversion implies -Wsign-conversion in clang only, so
        # every unit including those headers would fail on diagnostics GCC never issues. -Wno-error keeps clang's
        # compiler warnings as warnings (hidden by the Checks glob); every tidy check stays warnings-as-errors and
        # every hard error still fails.
        result = ['--extra-arg=-Wno-unknown-warning-option', '--extra-arg=-Wno-unused-command-line-argument',
                  '--extra-arg=-Wno-error']
    if header:
        result.append('--extra-arg=-Wno-pragma-once-outside-header')
    return result


def target_of(entry):
    match = re.search(r'CMakeFiles/([^/]+)\.dir/', (entry.get('output') or '').replace('\\', '/'))
    return match[1] if match else None


def module_directory(path, root):
    """The module owning a file: the parent of its `include` component, else the file's own directory."""
    parts = path.relative_to(root).parts
    if 'include' in parts:
        return root.joinpath(*parts[:parts.index('include')])
    return path.parent


def source_pattern(file):
    variants = {file, file.replace('\\', '/'), file.replace('/', '\\')}
    alternatives = '|'.join(re.escape(variant) for variant in sorted(variants))
    return re.compile(r'(?<!\S)"?(?:' + alternatives + r')"?(?=\s|$)', re.IGNORECASE if os.name == 'nt' else 0)


def synthesize(sibling, header, family):
    """The sibling translation unit's command with the header as its main file, or None when the command
    does not name its own source (then nothing proves which flags the sibling was compiled with)."""
    entry = stripped(sibling)
    quoted = f'"{header}"' if ' ' in header else header
    if 'arguments' in entry:
        arguments = entry['arguments']
        matches = [index for index, token in enumerate(arguments)
                   if normalized(token, sibling.get('directory')) == normalized(sibling['file'], sibling.get('directory'))]
        if not matches:
            return None
        index = matches[-1]
        arguments[index] = header
        if family == 'gnu':
            arguments[index:index] = ['-x', 'c++']
        elif '/TP' not in arguments:
            arguments.insert(1, '/TP')
        entry['arguments'] = arguments
    else:
        pattern = source_pattern(sibling['file'])
        replacement = ('-x c++ ' if family == 'gnu' else '') + quoted.replace('\\', '\\\\')
        command, count = pattern.subn(replacement, entry['command'], count=1)
        if not count:
            return None
        if family == 'msvc' and not re.search(r'(?<!\S)/TP(?=\s|$)', command):
            driver = first_token(command)
            command = command.replace(driver, driver + ' /TP', 1) if not command.startswith('"') \
                else command.replace(f'"{driver}"', f'"{driver}" /TP', 1)
        entry['command'] = command
    entry['file'] = header
    entry.pop('output', None)
    return entry


def prepare(root, entries, files, owners=None):
    """Per-file analysis jobs and the mirrored database entries (PCH inputs stripped, header units appended)."""
    owners = owners or {}
    index = {}
    for entry in entries:
        if 'file' in entry:
            index.setdefault(normalized(entry['file'], entry.get('directory')), entry)
    by_target, by_module = {}, {}
    for entry in entries:
        file = Path(entry['file'] if os.path.isabs(entry['file']) else os.path.join(entry.get('directory', ''), entry['file']))
        if file.suffix.lower() not in SOURCE_SUFFIXES or 'cmake_pch' in file.name:
            continue
        target = target_of(entry)
        if target:
            by_target.setdefault(target, []).append(entry)
        try:
            key = module_key(file.resolve().relative_to(root.resolve()))
        except ValueError:
            continue
        for depth in range(1, len(key)):
            by_module.setdefault(key[:depth], []).append(entry)
    mirrored = [stripped(entry) for entry in entries]
    jobs = []
    for requested in files:
        path = Path(requested) if os.path.isabs(requested) else root / requested
        try:
            name = path.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            name = str(path)
        job = {'path': name, 'absolute': str(path), 'status': None, 'reason': None, 'source': None, 'family': None,
               'target': None, 'exit_code': None, 'diagnostics': [], 'seconds': None}
        jobs.append(job)
        if not path.is_file():
            job.update(status='missing', reason='asked to gate a file that does not exist')
            continue
        entry = index.get(normalized(path))
        if entry is not None and path.suffix.lower() in SOURCE_SUFFIXES:
            job.update(source='database', family=compiler_family(entry), target=target_of(entry))
            continue
        if path.suffix.lower() not in SOURCE_SUFFIXES | HEADER_SUFFIXES:
            job.update(status='ungated', reason='not a C++ source or header the gate analyses')
            continue
        sibling, source = None, None
        try:
            module = module_directory(path.resolve(), root.resolve()).relative_to(root.resolve())
        except ValueError:
            module = None
        key = module_key(module) if module else None

        def within_module(entry):
            file = entry['file'] if os.path.isabs(entry['file']) else os.path.join(entry.get('directory', ''), entry['file'])
            try:
                return key is not None and module_key(Path(file).resolve().relative_to(root.resolve()))[:len(key)] == key
            except ValueError:
                return False

        # The defining module's own target first (an owner with a translation unit under the header's module
        # directory), then the plan's order; inside a target, a unit under the module before any other.
        owned = [target for target in (owners.get(name) or []) if by_target.get(target)]
        owned.sort(key=lambda target: (not any(within_module(entry) for entry in by_target[target]), target))
        for target in owned:
            candidates = sorted(by_target[target], key=lambda item: (not within_module(item), item['file']))
            sibling, source = candidates[0], f'owner:{target}'
            break
        if sibling is None and key is not None:
            candidates = sorted(by_module.get(key, []), key=lambda item: item['file'])
            if candidates:
                sibling, source = candidates[0], 'module:' + module.as_posix()
        if sibling is None:
            job.update(status='ungated', reason='no translation unit of an owning target or the same module is in the '
                                                'compile database; the configuration does not compile this file')
            continue
        family = compiler_family(sibling)
        entry = synthesize(sibling, str(path), family)
        if entry is None:
            job.update(status='ungated', reason=f'the {source} translation unit command does not name its source')
            continue
        mirrored.append(entry)
        job.update(source=source, family=family, target=target_of(sibling))
    return jobs, mirrored


def classify(text, exit_code):
    lines = text.splitlines()
    unresolved = [line for line in lines if UNRESOLVED in line]
    if unresolved:
        return 'ungated', 'includes did not resolve; no checks ran', unresolved[:5]
    diagnostics = [line for line in lines if DIAGNOSTIC.search(line)]
    if diagnostics:
        return 'issues', f'{len(diagnostics)} diagnostic line(s)', diagnostics[:20]
    if exit_code:
        return 'ungated', f'clang-tidy exited {exit_code} without a recognized diagnostic', lines[-5:]
    return 'clean', None, []


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def analyse(root, build, files, environment, *, owners=None, tool=None, scratch=None, export_fixes=None, timeout=600):
    root, build = Path(root).resolve(strict=True), Path(build).resolve()
    if not files:
        raise SelectionError('Name at least one C++ source or header to analyse')
    summary = {'version': 1, 'kind': 'tidy', 'root': str(root), 'build': str(build), 'host': platform.system(),
               'required_llvm_major': REQUIRED_TIDY_MAJOR, 'tool': None, 'tool_version': None, 'tool_reason': None,
               'database': None, 'database_entries': 0, 'database_sha256': None, 'mirror_sha256': None,
               'files': [], 'counts': {'clean': 0, 'issues': 0, 'ungated': 0, 'missing': 0},
               'status': 'instrument_failure', 'exit_code': 2, 'qualification': 'none'}
    resolution = resolve_clang_tidy(environment, tool)
    summary.update(tool=resolution['path'], tool_version=resolution['version'], tool_reason=resolution['reason'],
                   rejected_tools=resolution['rejected'])
    names = []
    for requested in files:
        path = Path(requested) if os.path.isabs(requested) else root / requested
        try:
            names.append(path.resolve().relative_to(root.resolve()).as_posix())
        except ValueError:
            names.append(str(path))
    if not resolution['path']:
        summary['files'] = [{'path': name, 'status': 'ungated', 'reason': resolution['reason'], 'source': None,
                             'family': None, 'target': None, 'exit_code': None, 'diagnostics': [], 'seconds': None}
                            for name in names]
        summary['counts']['ungated'] = len(names)
        summary.update(status='unavailable', exit_code=UNAVAILABLE_EXIT,
                       qualification=f'no strict analysis ran; changed C++ remains ungated on this host: {resolution["reason"]}')
        return summary
    database = build / 'compile_commands.json'
    if not database.is_file():
        raise SelectionError(f'No compile database at {database}; configure the preset with CMAKE_EXPORT_COMPILE_COMMANDS first')
    entries = json.loads(database.read_text(encoding='utf-8-sig'))
    if not isinstance(entries, list) or not entries or not all(isinstance(entry, dict) and 'file' in entry for entry in entries):
        raise SelectionError(f'Compile database {database} is empty or malformed')
    cache = cache_values(build)
    summary.update(database=str(database), database_entries=len(entries), database_sha256=digest(database),
                   cache_arch_flag=cache.get('CRD_SIMD_MSVC_ARCH_FLAG') or None)
    jobs, mirrored = prepare(root, entries, files, owners)
    temporary = scratch is None
    scratch = Path(tempfile.mkdtemp(prefix='crd-tidy-')) if temporary else Path(scratch)
    try:
        scratch.mkdir(parents=True, exist_ok=True)
        mirror = scratch / 'compile_commands.json'
        mirror.write_text(json.dumps(mirrored, indent=1), encoding='utf-8')
        summary['mirror_sha256'] = digest(mirror)
        if export_fixes:
            Path(export_fixes).mkdir(parents=True, exist_ok=True)
        for job in jobs:
            if job['status']:
                continue
            header = Path(job['absolute']).suffix.lower() in HEADER_SUFFIXES
            # Main-file diagnostics only, on every host. The repository's HeaderFilterRegex is spelled with `/`
            # separators, which never match the backslash paths of the hosted strict lane or the Windows helper, so
            # the contract every lane has ever enforced is the main file; on Linux the same regex matches and would
            # add header diagnostics no lane enforces (measured 2026-09-13: 235 macro-usage hits in 27 units).
            # Headers are gated by naming them, which makes each one its own main file.
            command = [resolution['path'], job['absolute'], '--warnings-as-errors=*', '--quiet', '--header-filter=',
                       '-p', str(scratch), *extra_arguments(job['family'], cache, header)]
            if export_fixes:
                command.append('--export-fixes=' + str(Path(export_fixes) / (re.sub(r'[:/\\]', '_', job['path']) + '.yaml')))
            started = time.monotonic()
            try:
                result = subprocess.run(command, cwd=str(root), env=environment, capture_output=True, timeout=timeout)
            except subprocess.TimeoutExpired:
                job.update(status='ungated', reason=f'clang-tidy exceeded the {timeout:g} s per-file budget',
                           seconds=round(time.monotonic() - started, 3))
                continue
            except OSError as error:
                job.update(status='ungated', reason=f'clang-tidy could not be started ({error})', seconds=0.0)
                continue
            text = result.stdout.decode('utf-8', errors='replace') + result.stderr.decode('utf-8', errors='replace')
            status, reason, diagnostics = classify(text, result.returncode)
            job.update(status=status, reason=reason, diagnostics=diagnostics, exit_code=result.returncode,
                       seconds=round(time.monotonic() - started, 3))
    finally:
        if temporary:
            shutil.rmtree(scratch, ignore_errors=True)
    for job in jobs:
        job.pop('absolute', None)
        summary['counts'][job['status']] += 1
    summary['files'] = jobs
    counts = summary['counts']
    summary['exit_code'] = counts['issues'] + counts['ungated'] + counts['missing']
    if counts['issues']:
        summary.update(status='failed', qualification=f'{counts["issues"]} file(s) with strict-analysis findings')
    elif counts['ungated'] or counts['missing']:
        summary.update(status='incomplete', qualification=f'{counts["ungated"]} ungated and {counts["missing"]} missing '
                                                          'file(s); an unparsed file is never clean')
    else:
        summary.update(status='passed', qualification=f'all {len(jobs)} file(s) parsed and analysed clean with '
                                                      f'LLVM {resolution["version"]} against {database}')
    return summary


def report_lines(summary):
    labels = {'clean': 'clean      ', 'issues': 'TIDY ISSUES', 'ungated': 'UNGATED    ', 'missing': 'MISSING    '}
    lines = [f'tool: {summary.get("tool") or "unavailable"}'
             + (f' (LLVM {summary["tool_version"]})' if summary.get('tool_version') else ''),
             f'database: {summary.get("database") or "not read"} ({summary.get("database_entries", 0)} entries)']
    if summary.get('tool_reason'):
        lines.append(summary['tool_reason'])
    for item in summary['files']:
        detail = f'  <-- {item["reason"]}' if item.get('reason') and item['status'] != 'issues' else ''
        source = f'  [{item["source"]}]' if item.get('source') else ''
        lines.append(f'{labels[item["status"]]} {item["path"]}{source}{detail}')
        lines.extend('  ' + line for line in item.get('diagnostics') or [])
    counts = summary['counts']
    if counts['missing']:
        lines.append(f'{counts["missing"]} file(s) MISSING - check the paths you passed; a skipped file is not a clean one.')
    if counts['ungated']:
        lines.append(f'{counts["ungated"]} file(s) UNGATED - the gate could not analyse them; never treat this as clean.')
    if counts['issues']:
        lines.append(f'{counts["issues"]} file(s) with tidy issues - fix before closing the slice.')
    lines.append(f'{summary["status"]}: {summary["qualification"]}')
    return lines
