#!/usr/bin/env python3
"""Compile changed public headers standalone with the flags of the owning module's own translation units.

Contract: docs/design/public-consumption.md. The hosted public-check lanes (CRD_PUBLIC_CHECKS) compile every public
header through its module's consumer view; this is the local changed-surface counterpart: the headers you changed
(or name) are compiled as their own translation unit with the command of a sibling unit taken from the compile
database of an existing build directory, so a header that only compiled because an earlier include happened to
provide a declaration fails here before a push. The synthesis is the tidy gate's (scripts/cerid_dev/tidy.py): the
precompiled-header inputs are stripped and the header becomes the main file; on top of that every output the
command names (object, program database, dependency file) is redirected into a scratch directory, so the build
tree is read and never written.

    python scripts/check-headers.py --build build/win-debug --changed
    python scripts/check-headers.py --build build/linux-gcc-debug engine/foundation/log/include/crd/log/log.hpp

MSVC commands need the compiler environment (scripts/msvc-env.bat). A header whose module has no translation unit
in the database is reported as unchecked with the reason, never as passed.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from cerid_dev import tidy  # noqa: E402

HEADER_SUFFIXES = {'.hpp', '.h'}
PUBLIC_PATTERN = re.compile(r'^engine/.+/include/.+\.(hpp|h)$')
MSVC_OUTPUT_FLAGS = ('Fo', 'Fd', 'Fe', 'Fa', 'Fi', 'Fm', 'FR', 'Fr', 'Fp')
GNU_OUTPUT_FLAGS = ('-o', '-MF', '-MT', '-MQ')
GNU_DROPPED_FLAGS = ('-MD', '-MMD', '-M', '-MM', '-MP')


def split_command(command):
    """Command string to argv: Windows rules through the shell API on Windows, POSIX rules elsewhere."""
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        argc = ctypes.c_int()
        shell32 = ctypes.windll.shell32
        shell32.CommandLineToArgvW.restype = ctypes.POINTER(wintypes.LPWSTR)
        argv = shell32.CommandLineToArgvW(command, ctypes.byref(argc))
        try:
            return [argv[index] for index in range(argc.value)]
        finally:
            ctypes.windll.kernel32.LocalFree(argv)
    return shlex.split(command)


def arguments_of(entry):
    if 'arguments' in entry:
        return list(entry['arguments'])
    return split_command(entry['command'])


def redirect_outputs(arguments, family, scratch, stem):
    """The command with every output it names moved under scratch (object, PDB, depfile), depfile generation
    dropped on GNU drivers; returns the new arguments and the outputs they name."""
    scratch = Path(scratch)
    result, outputs = [], []
    skip = False
    for index, token in enumerate(arguments):
        if skip:
            skip = False
            continue
        if family == 'msvc':
            match = re.match(r'^[-/](' + '|'.join(MSVC_OUTPUT_FLAGS) + r')(.*)$', token)
            if match:
                flag, value = match.group(1), match.group(2)
                if flag == 'Fp':
                    continue
                suffix = {'Fo': '.obj', 'Fd': '.pdb', 'Fe': '.exe', 'Fa': '.asm', 'Fi': '.i', 'Fm': '.map'}.get(flag, '.out')
                if value == '' and index + 1 < len(arguments):
                    skip = True
                target = scratch / f'{stem}{suffix}'
                outputs.append(str(target))
                result.append(f'/{flag}{target}')
                continue
            if token.lower() in ('-sourcedependencies', '/sourcedependencies'):
                skip = True
                continue
        else:
            if token in GNU_DROPPED_FLAGS:
                continue
            if token in GNU_OUTPUT_FLAGS:
                if token == '-o':
                    target = scratch / f'{stem}.o'
                    outputs.append(str(target))
                    result.extend(['-o', str(target)])
                skip = True
                continue
            if token.startswith('-o') and len(token) > 2 and not token.startswith('-open'):
                target = scratch / f'{stem}.o'
                outputs.append(str(target))
                result.append('-o' + str(target))
                continue
            if token.startswith(('-MF', '-MT', '-MQ')) and len(token) > 3:
                continue
        result.append(token)
    return result, outputs


def outputs_named(arguments, family):
    """Every path an argument list writes, for the proof that a redirected command names none inside the build tree."""
    named = []
    for index, token in enumerate(arguments):
        if family == 'msvc':
            match = re.match(r'^[-/](Fo|Fd|Fe|Fa|Fi|Fm|FR|Fr)(.*)$', token)
            if match:
                named.append(match.group(2) if match.group(2) else arguments[index + 1] if index + 1 < len(arguments) else '')
        elif token in ('-o', '-MF') and index + 1 < len(arguments):
            named.append(arguments[index + 1])
        elif token.startswith('-o') and len(token) > 2 and not token.startswith('-open'):
            named.append(token[2:])
        elif token.startswith('-MF') and len(token) > 3:
            named.append(token[3:])
    return named


def changed_headers(root):
    """Public headers changed against HEAD plus untracked ones, as repository-relative POSIX paths."""
    tracked = subprocess.run(['git', 'diff', '--name-only', 'HEAD', '--'], cwd=str(root), capture_output=True,
                             text=True, encoding='utf-8', check=True).stdout.splitlines()
    untracked = subprocess.run(['git', 'ls-files', '--others', '--exclude-standard'], cwd=str(root),
                               capture_output=True, text=True, encoding='utf-8', check=True).stdout.splitlines()
    names = sorted({name.strip() for name in tracked + untracked if PUBLIC_PATTERN.match(name.strip())})
    return [name for name in names if (root / name).is_file()]


def plan(root, build, files):
    database = Path(build) / 'compile_commands.json'
    if not database.is_file():
        raise SystemExit(f'{database} does not exist; configure the build directory first')
    entries = json.loads(database.read_text(encoding='utf-8-sig'))
    jobs, mirrored = tidy.prepare(root, entries, files)
    synthesized = iter(mirrored[len(entries):])
    for job in jobs:
        if job['status'] is None and job['source'] not in (None, 'database'):
            job['entry'] = next(synthesized)
        elif job['status'] is None and job['source'] == 'database':
            job.update(status='ungated', reason='a translation unit of the build, compiled by the build itself')
    return jobs


def compile_job(job, scratch, environment, timeout):
    entry = job['entry']
    family = job['family']
    stem = re.sub(r'[^A-Za-z0-9_]+', '_', job['path'])
    arguments, outputs = redirect_outputs(arguments_of(entry), family, scratch, stem)
    started = time.monotonic()
    try:
        result = subprocess.run(arguments, cwd=entry.get('directory') or str(scratch), capture_output=True,
                                text=True, encoding='utf-8', errors='replace', env=environment, timeout=timeout)
        text, code = result.stdout + result.stderr, result.returncode
    except (OSError, subprocess.TimeoutExpired) as error:
        text, code = str(error), -1
    job['seconds'] = round(time.monotonic() - started, 1)
    job['exit_code'] = code
    job['outputs'] = outputs
    job['status'] = 'passed' if code == 0 else 'failed'
    job['diagnostics'] = [line for line in text.splitlines()
                          if re.search(r'\berror\b|\bwarning\b|\bfatal\b|\bnote\b', line)][:40]
    return job


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', default='build/win-debug', help='configured build directory with compile_commands.json')
    parser.add_argument('--changed', action='store_true', help='check the public headers changed against HEAD')
    parser.add_argument('--jobs', type=int, default=max(1, min(16, os.cpu_count() or 1)))
    parser.add_argument('--scratch', help='scratch directory for the objects (default: a temporary directory)')
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--output', help='write the per-header records as JSON')
    parser.add_argument('files', nargs='*', help='headers to check (repository-relative or absolute)')
    args = parser.parse_args(argv)
    root = ROOT
    files = list(args.files)
    if args.changed:
        files.extend(changed_headers(root))
    files = sorted(dict.fromkeys(files))
    if not files:
        print('check-headers: no public header to check')
        return 0
    jobs = plan(root, root / args.build if not os.path.isabs(args.build) else Path(args.build), files)
    scratch = Path(args.scratch) if args.scratch else Path(tempfile.mkdtemp(prefix='crd-header-check-'))
    scratch.mkdir(parents=True, exist_ok=True)
    build_dir = (root / args.build if not os.path.isabs(args.build) else Path(args.build)).resolve()
    pending = [job for job in jobs if job.get('entry')]
    environment = dict(os.environ)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        list(pool.map(lambda job: compile_job(job, scratch, environment, args.timeout), pending))
    failed = [job for job in jobs if job['status'] == 'failed']
    for job in jobs:
        if job['status'] == 'passed':
            print(f'PASS  {job["path"]} ({job["source"]}, {job["seconds"]} s)')
        elif job['status'] == 'failed':
            print(f'FAIL  {job["path"]} ({job["source"]}, exit {job["exit_code"]})')
            for line in job['diagnostics'][:8]:
                print('      ' + line)
        else:
            print(f'SKIP  {job["path"]}: {job["reason"]}')
    unchecked = [job for job in jobs if job['status'] not in ('passed', 'failed')]
    print(f'check-headers: {len(jobs) - len(failed) - len(unchecked)} passed, {len(failed)} failed, '
          f'{len(unchecked)} unchecked; objects under {scratch}')
    if args.output:
        records = [{key: value for key, value in job.items() if key != 'entry'} for job in jobs]
        Path(args.output).write_text(json.dumps({'schema': 'cerid-check-headers/1', 'build': str(build_dir),
                                                 'headers': records}, indent=2), encoding='utf-8')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
