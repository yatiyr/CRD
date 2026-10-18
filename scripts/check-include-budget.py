#!/usr/bin/env python3
"""Hold hot headers to their include budget: standard headers they must not reach.

The 2026-10-18 compile-time profiles (docs/bench/2026-10-18-build-speed-compile-time.md) found most of the build's
front end in a few standard headers that hot project headers pulled into every unit (<chrono> through Catch2's
assertion path, <iterator> through Array, <functional> through the hash containers, <immintrin.h> through the scalar
math). Removing them is easy to undo by accident: one include in a header that a thousand units reach, and nothing
fails. This guard makes that a failure. scripts/include-budget.json names each header (`include`), a unit of the
configuration whose compile command to borrow (`flags_of`, matched by path suffix in compile_commands.json) and the
standard headers it must not reach (`forbid` everywhere, `forbid_gnu` on GCC and Clang). For each entry a probe unit
`#include <...>` is compiled with that command, its precompiled header removed, outputs in a scratch directory, and
the compiler's own include listing (MSVC /showIncludes with /Zs, GCC and Clang -H with -fsyntax-only); a forbidden
header anywhere in the listing fails, with the chain of includes that reached it.

    python scripts/check-include-budget.py --build build/win-debug
    python scripts/check-include-budget.py --build ~/cerid-mirror-build/linux-gcc-debug

MSVC commands need the compiler environment; without one (no INCLUDE variable) the guard takes it from
scripts/msvc-env.bat. An entry whose `flags_of` unit is not in the database, or whose probe does not compile, fails:
nothing was checked. Contract: docs/design/build-performance.md#include-budget.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from cerid_dev import tidy  # noqa: E402

_spec = __import__('importlib.util').util.spec_from_file_location('check_headers', ROOT / 'scripts/check-headers.py')
headers = __import__('importlib.util').util.module_from_spec(_spec)
_spec.loader.exec_module(headers)

SCHEMA = 'cerid-include-budget/1'
REGISTRY = ROOT / 'scripts/include-budget.json'
LAUNCHERS = {'sccache', 'ccache'}
DEFAULT_MSVC_PREFIX = 'Note: including file:'


def load(path=REGISTRY):
    document = json.loads(Path(path).read_text(encoding='utf-8'))
    if document.get('schema') != SCHEMA:
        raise SystemExit('%s: expected schema %s' % (path, SCHEMA))
    for entry in document['entries']:
        for field in ('include', 'flags_of', 'forbid', 'why'):
            if field not in entry:
                raise SystemExit('%s: an entry lacks %s' % (path, field))
    return document


def without_launcher(entry):
    """The entry with a compiler launcher (sccache, ccache) removed from the front of its command."""
    result = dict(entry)
    if 'arguments' in entry:
        arguments = list(entry['arguments'])
        if arguments and Path(arguments[0].replace('\\', '/')).stem.lower() in LAUNCHERS:
            arguments = arguments[1:]
        result['arguments'] = arguments
    else:
        command = entry.get('command', '')
        first = tidy.first_token(command)
        if Path(first.replace('\\', '/')).stem.lower() in LAUNCHERS:
            command = command.lstrip()
            command = command[len(first) + (2 if command.startswith('"') else 0):].lstrip()
        result['command'] = command
    return result


def find_unit(entries, suffix):
    wanted = suffix.replace('\\', '/').lower()
    for entry in entries:
        if entry.get('file', '').replace('\\', '/').lower().endswith(wanted):
            return entry
    return None


def msvc_prefix(build):
    """The localized /showIncludes prefix CMake detected for this tree (Ninja reads the same one)."""
    rules = Path(build) / 'CMakeFiles' / 'rules.ninja'
    if rules.is_file():
        for line in rules.read_text(encoding='utf-8', errors='replace').splitlines():
            if line.strip().startswith('msvc_deps_prefix = '):
                return line.split('=', 1)[1].strip()
    return DEFAULT_MSVC_PREFIX


def parse_listing(text, family, prefix=DEFAULT_MSVC_PREFIX):
    """[(depth, path)] of every include in the compiler's listing, depth 1 for the probe's own include. MSVC indents
    one space per level after a localized prefix whose own spacing differs between languages (`Note: including
    file: ` against `Not: eklenen dosya:  `), so its depths count from the shallowest line."""
    result = []
    prefix = prefix.rstrip()
    indents = []
    for line in text.splitlines():
        if family == 'msvc':
            if not line.startswith(prefix):
                continue
            rest = line[len(prefix):]
            indents.append(len(rest) - len(rest.lstrip(' ')))
            result.append((indents[-1], rest.strip()))
        else:
            # One dot per level; GCC's closing "Multiple include guards may be useful for:" list has none.
            match = re.match(r'^(\.+) (.+)$', line)
            if match:
                result.append((len(match.group(1)), match.group(2).strip()))
    if indents:
        base = min(indents) - 1
        result = [(depth - base, path) for depth, path in result]
    return result


def chains(listing, names):
    """For every listed include whose file name is in `names`: the chain of includes that reached it."""
    found = {}
    stack = []
    for depth, path in listing:
        del stack[depth - 1:]
        stack.append(path)
        name = re.split(r'[\\/]', path)[-1]
        if name in names and name not in found:
            found[name] = list(stack)
    return found


def probe_command(unit, probe, family, scratch):
    entry = tidy.synthesize(without_launcher(unit), str(probe), family)
    if entry is None:
        return None
    arguments = headers.arguments_of(entry)
    arguments, _ = headers.redirect_outputs(arguments, family, scratch, probe.stem)
    if family == 'msvc':
        arguments = [a for a in arguments if not re.match(r'^[-/]c$', a)] + ['/showIncludes', '/Zs']
    else:
        arguments = [a for a in arguments if a not in ('-c',)] + ['-H', '-fsyntax-only']
    return arguments


def msvc_environment():
    """The environment scripts/msvc-env.bat sets, for a shell that has none."""
    result = subprocess.run(['cmd', '/d', '/c', 'call', str(ROOT / 'scripts' / 'msvc-env.bat'), '>nul', '&&', 'set'],
                            capture_output=True, text=True, encoding='utf-8', errors='replace', check=False)
    environment = dict(os.environ)
    for line in result.stdout.splitlines():
        name, sep, value = line.partition('=')
        if sep and name:
            environment[name] = value
    return environment


def check(build, registry, scratch, runner=subprocess.run, environment=None):
    """[(entry, verdict, detail)] for every registry entry."""
    database = Path(build) / 'compile_commands.json'
    entries = json.loads(database.read_text(encoding='utf-8-sig'))
    prefix = msvc_prefix(build)
    results = []
    for index, item in enumerate(registry['entries']):
        unit = find_unit(entries, item['flags_of'])
        if unit is None:
            results.append((item, 'unchecked', '%s is not in %s' % (item['flags_of'], database)))
            continue
        family = tidy.compiler_family(without_launcher(unit))
        probe = Path(scratch) / ('budget_probe_%d.cpp' % index)
        probe.write_text('#include <%s>\n' % item['include'], encoding='utf-8')
        arguments = probe_command(unit, probe, family, scratch)
        if arguments is None:
            results.append((item, 'unchecked', 'the command of %s does not name its source' % item['flags_of']))
            continue
        completed = runner(arguments, cwd=unit.get('directory') or str(scratch), capture_output=True, text=True,
                           encoding='utf-8', errors='replace', env=environment, check=False)
        text = (completed.stdout or '') + (completed.stderr or '')
        listing = parse_listing(text, family, prefix)
        if completed.returncode != 0 or not listing:
            tail = [line for line in text.splitlines() if 'error' in line.lower()][:4]
            results.append((item, 'unchecked', 'the probe did not compile (exit %d): %s'
                            % (completed.returncode, ' | '.join(tail) or 'no include listing')))
            continue
        forbidden = set(item['forbid']) | (set(item.get('forbid_gnu', [])) if family == 'gnu' else set())
        reached = chains(listing, forbidden)
        if reached:
            detail = '; '.join('<%s> through %s' % (name, ' -> '.join(short(p) for p in chain))
                               for name, chain in sorted(reached.items()))
            results.append((item, 'over', detail))
        else:
            results.append((item, 'within', '%d headers, none of %s' % (len(listing),
                                                                        ', '.join(sorted(forbidden)))))
    return results


def short(path):
    text = path.replace('\\', '/')
    for marker in ('/include/', '/src/'):
        if marker in text:
            return text.rsplit(marker, 1)[1]
    return text.rsplit('/', 1)[-1]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--build', required=True, help='a configured tree with compile_commands.json')
    parser.add_argument('--registry', default=str(REGISTRY))
    parser.add_argument('--scratch', help='directory for the probes (default: a temporary directory)')
    args = parser.parse_args(argv)
    build = Path(os.path.expanduser(args.build))
    if not (build / 'compile_commands.json').is_file():
        print('ERROR: %s has no compile_commands.json' % build)
        return 1
    registry = load(args.registry)
    scratch = Path(args.scratch) if args.scratch else Path(tempfile.mkdtemp(prefix='crd-include-budget-'))
    scratch.mkdir(parents=True, exist_ok=True)
    environment = None
    if os.name == 'nt' and not os.environ.get('INCLUDE'):
        environment = msvc_environment()
    results = check(build, registry, scratch, environment=environment)
    failed = 0
    for item, verdict, detail in results:
        print('%-9s %s: %s' % (verdict.upper(), item['include'], detail))
        failed += verdict != 'within'
    print('include budget: %d of %d headers within' % (len(results) - failed, len(results)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
