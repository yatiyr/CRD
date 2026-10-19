#!/usr/bin/env python3
"""Hold every first-party C-family source to a build route, judged against one configured tree.

A source that no target compiles is a test that never runs or code that rots unseen: the CEIR-17a scene-dialect
cases (tests/execution/ceir/test_scene.cpp) sat in no target's source list and had never run until 2026-10-17. A
lexical CMake check cannot see this (159 test sources are globbed, six are named by another directory's CMakeLists),
so this guard reads what a configured tree really compiles, its compile_commands.json, and judges every source under
the roots of scripts/source-routes.json (`git ls-files` of the checkout, untracked files that are not ignored
included):

- a source no entry names is compiled by a target of this tree;
- `built_when` names the conditions under which this tree compiles an entry's sources, all of which must hold:
  `platform` (the tree's CMAKE_SYSTEM_NAME), `option` (a CMakeCache.txt value, absent reading OFF; a CMake file must
  define the name) and `target` (a target that compiles a source in this tree; a CMake file must name it). A covered
  source is compiled exactly when they hold, so a source that falls out of its target fails, and so does an entry
  that no longer describes the tree;
- `built_by` names the file that builds an entry's sources outside this tree's targets (a script, a separate CMake
  project); it must exist and name every source, and this tree must not compile them;
- `included_by` names the source that #includes an entry's sources; it must include each one;
- `unrouted` names the ROADMAP row that owns giving the sources a route or retiring them: allowed only outside
  engine/, tests/, tools/ and sandbox/, the row must exist and must not be Done, and this tree must not compile them.

Every entry needs a `why`, and every pattern must match a source (a trailing `/` is a directory, `*` and `?` match
within one path component). The most specific pattern names a source (a file, then a glob, then the deepest
directory); two entries naming a source at the same rank is a finding.

    python scripts/check-source-routes.py --build build/win-debug        (the crd-check-source-routes CTest)

Exit 0: every source holds; 1: findings, each printed; 2: unusable input (no compile database, no manifest, no git).
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / 'scripts/source-routes.json'
ROADMAP = ROOT / 'docs/ROADMAP.md'
SCHEMA = 'cerid-source-routes/1'
ROUTES = ('built_when', 'built_by', 'included_by', 'unrouted')
CONDITIONS = ('platform', 'option', 'target')
PLATFORMS = ('Windows', 'Linux', 'Darwin')
# The module-system directories: every source there is a target's source or is gated by a condition the tree states.
NO_UNROUTED = ('engine/', 'tests/', 'tools/', 'sandbox/')
OBJECT_DIR = re.compile(r'CMakeFiles[/\\]([^/\\]+)\.dir[/\\]')
SYSTEM_NAME = re.compile(r'^\s*set\(CMAKE_SYSTEM_NAME "([^"]*)"\)', re.MULTILINE)
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)


def cmake_true(value):
    """CMake's if(<constant>) truth: 1, ON, YES, TRUE, Y or a non-zero number; *-NOTFOUND and the rest are false."""
    text = value.strip().upper()
    if text in ('ON', 'YES', 'TRUE', 'Y'):
        return True
    try:
        return float(text) != 0.0
    except ValueError:
        return False


def key(path):
    """A comparison key for a repository-relative POSIX path (case-insensitive where the file system is)."""
    return path.lower() if os.name == 'nt' else path


class Tree:
    """What one configured tree compiles: repository sources, the targets that compile them, its cache and system."""

    def __init__(self, root, compiled, targets, cache, system):
        self.root = Path(root)
        self.compiled = compiled
        self.targets = targets
        self.cache = cache
        self.system = system

    @classmethod
    def load(cls, root, build):
        build = Path(build)
        database = build / 'compile_commands.json'
        if not database.is_file():
            raise SystemExit('ERROR: %s has no compile_commands.json (configure it with a Ninja or Makefiles '
                             'generator and CMAKE_EXPORT_COMPILE_COMMANDS ON)' % build)
        entries = json.loads(database.read_text(encoding='utf-8'))
        if not entries:
            raise SystemExit('ERROR: %s lists no compile command; nothing could be judged' % database)
        compiled = {}
        targets = set()
        root_text = os.path.normcase(os.path.normpath(str(root)))
        build_text = os.path.normcase(os.path.normpath(os.path.abspath(str(build))))
        for entry in entries:
            directory = entry.get('directory', '')
            source = entry.get('file', '')
            if not os.path.isabs(source):
                source = os.path.join(directory, source)
            normal = os.path.normpath(source)
            folded = os.path.normcase(normal)
            output = entry.get('output') or output_of(entry)
            if output is None:
                raise SystemExit('ERROR: %s: the entry for %s names no output (CMake 3.20 or later writes one)'
                                 % (database, source))
            target = OBJECT_DIR.search(output)
            if target:
                targets.add(target.group(1))
            if folded.startswith(build_text + os.sep) or not folded.startswith(root_text + os.sep):
                continue
            relative = normal[len(root_text) + 1:].replace(os.sep, '/')
            compiled[key(relative)] = target.group(1) if target else '?'
        return cls(root, compiled, targets, read_cache(build), read_system(build))


def output_of(entry):
    """The object a compile command writes, for a database without the `output` field."""
    words = entry.get('arguments') or shlex.split(entry.get('command', ''), posix=False)
    for index, word in enumerate(words):
        if word == '-o' and index + 1 < len(words):
            return words[index + 1]
        for prefix in ('/Fo', '-Fo'):
            if word.startswith(prefix) and len(word) > len(prefix):
                return word[len(prefix):]
    return None


def read_cache(build):
    path = Path(build) / 'CMakeCache.txt'
    if not path.is_file():
        raise SystemExit('ERROR: %s has no CMakeCache.txt' % build)
    cache = {}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        match = re.match(r'^([A-Za-z0-9_.+-]+)(?::[A-Z]+)?=(.*)$', line)
        if match:
            cache[match.group(1)] = match.group(2)
    return cache


def read_system(build):
    candidates = sorted(Path(build).glob('CMakeFiles/*/CMakeSystem.cmake'), key=lambda p: p.stat().st_mtime)
    for path in reversed(candidates):
        match = SYSTEM_NAME.search(path.read_text(encoding='utf-8', errors='replace'))
        if match:
            return match.group(1)
    raise SystemExit('ERROR: %s has no CMakeFiles/<version>/CMakeSystem.cmake naming CMAKE_SYSTEM_NAME' % build)


def repository_sources(root, roots, extensions):
    """Every tracked source under the roots, and every untracked one that is not ignored."""
    try:
        completed = subprocess.run(['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others',
                                    '--exclude-standard', '--'] + list(roots),
                                   capture_output=True, timeout=120, check=False)
    except OSError as error:
        raise SystemExit('ERROR: git is needed to list the sources of %s: %s' % (root, error))
    if completed.returncode != 0:
        raise SystemExit('ERROR: git ls-files in %s exited %d: %s'
                         % (root, completed.returncode, completed.stderr.decode('utf-8', 'replace').strip()))
    paths = completed.stdout.decode('utf-8', 'replace').split('\0')
    return sorted({path for path in paths if path and Path(path).suffix in extensions})


def pattern_regex(pattern):
    text = ''
    for character in pattern:
        if character == '*':
            text += '[^/]*'
        elif character == '?':
            text += '[^/]'
        else:
            text += re.escape(character)
    return re.compile(text + '$', re.IGNORECASE if os.name == 'nt' else 0)


def rank(pattern):
    """Specificity: a file names a source before a glob, a glob before a directory, a deeper directory first."""
    if pattern.endswith('/'):
        return (0, pattern.count('/'))
    if any(character in pattern for character in '*?'):
        return (1, pattern.count('/'))
    return (2, 0)


def matches(pattern, source):
    if pattern.endswith('/'):
        return key(source).startswith(key(pattern))
    if any(character in pattern for character in '*?'):
        return pattern_regex(pattern).match(source) is not None
    return key(source) == key(pattern)


def as_list(value):
    return value if isinstance(value, list) else [value]


def open_rows(roadmap_text):
    """ROADMAP row ID -> status for every slice row."""
    rows = {}
    for match in re.finditer(r'<a id="slice-[^"]+"></a>\*\*([A-Za-z0-9.-]+)\*\* \| ([^|]+?) \|', roadmap_text):
        rows[match.group(1)] = match.group(2).strip()
    return rows


def check_entry_shape(index, entry, cmake_text, rows):
    """Problems with one manifest entry itself, independent of any tree."""
    where = 'entry %d (%s)' % (index, ', '.join(as_list(entry.get('paths', ['?']))))
    problems = []
    unknown = sorted(set(entry) - {'paths', 'why'} - set(ROUTES))
    if unknown:
        problems.append('%s: unknown keys %s' % (where, ', '.join(unknown)))
    routes = [route for route in ROUTES if route in entry]
    if len(routes) != 1:
        problems.append('%s: needs exactly one of %s' % (where, ', '.join(ROUTES)))
    if not str(entry.get('why', '')).strip():
        problems.append('%s: says nothing in `why`' % where)
    paths = entry.get('paths')
    if not isinstance(paths, list) or not paths or not all(isinstance(p, str) and p for p in paths):
        problems.append('%s: `paths` must be a non-empty list of patterns' % where)
    conditions = entry.get('built_when')
    if conditions is not None:
        if not isinstance(conditions, dict) or not conditions:
            problems.append('%s: `built_when` must name at least one of %s' % (where, ', '.join(CONDITIONS)))
        else:
            for name in sorted(set(conditions) - set(CONDITIONS)):
                problems.append('%s: unknown condition %s' % (where, name))
            for value in as_list(conditions.get('platform', [])):
                if value not in PLATFORMS:
                    problems.append('%s: platform %r is not one of %s' % (where, value, ', '.join(PLATFORMS)))
            for kind in ('option', 'target'):
                for value in as_list(conditions.get(kind, [])):
                    if not re.search(r'(?<![A-Za-z0-9_-])%s(?![A-Za-z0-9_-])' % re.escape(value), cmake_text):
                        problems.append('%s: no CMake file defines the %s %s' % (where, kind, value))
    owner = entry.get('unrouted')
    if owner is not None:
        status = rows.get(owner)
        if status is None:
            problems.append('%s: unrouted owner %s is not a ROADMAP row' % (where, owner))
        elif status == 'Done':
            problems.append('%s: unrouted owner %s is Done; route or retire the sources' % (where, owner))
        for pattern in as_list(paths or []):
            if any(key(pattern).startswith(prefix) for prefix in NO_UNROUTED):
                problems.append('%s: %s lies in a module directory, where every source needs a route' % (where, pattern))
    return problems


def holds(conditions, tree):
    for platform in as_list(conditions.get('platform', [])):
        if tree.system != platform:
            return False
    for option in as_list(conditions.get('option', [])):
        if not cmake_true(tree.cache.get(option, '')):
            return False
    for target in as_list(conditions.get('target', [])):
        if target not in tree.targets:
            return False
    return True


def describe(conditions):
    parts = []
    for kind in CONDITIONS:
        for value in as_list(conditions.get(kind, [])):
            parts.append('%s %s' % (kind, value))
    return ' and '.join(parts)


def names_source(text, source):
    return Path(source).name in text


def includes_source(text, source):
    name = Path(source).name
    return any(Path(included).name == name for included in INCLUDE.findall(text))


def check(tree, manifest, sources, cmake_text, roadmap_text, read=None):
    """(findings, counts) for one tree; read(path) returns a repository file's text (None when it is missing)."""
    read = read or (lambda path: (tree.root / path).read_text(encoding='utf-8', errors='replace')
                    if (tree.root / path).is_file() else None)
    problems = []
    if manifest.get('schema') != SCHEMA:
        return ['the manifest is not %s' % SCHEMA], {}
    rows = open_rows(roadmap_text)
    entries = manifest.get('entries', [])
    for index, entry in enumerate(entries):
        problems.extend(check_entry_shape(index, entry, cmake_text, rows))
    if problems:
        return problems, {}
    named = {}
    used = set()
    for source in sources:
        best = None
        for index, entry in enumerate(entries):
            for pattern in entry['paths']:
                if matches(pattern, source):
                    candidate = (rank(pattern), index, pattern)
                    if best is None or candidate[0] > best[0]:
                        best = candidate
                    elif candidate[0] == best[0] and index != best[1]:
                        problems.append('%s is named by entries %d (%s) and %d (%s) at the same rank'
                                        % (source, best[1], best[2], index, pattern))
        if best is not None:
            named[source] = best[1]
            used.add((best[1], best[2]))
    for index, entry in enumerate(entries):
        for pattern in entry['paths']:
            if (index, pattern) not in used:
                problems.append('entry %d: %s names no source the guard judges (a stale or misspelled pattern)'
                                % (index, pattern))
    counts = {'compiled': 0, 'gated off': 0, 'built outside': 0, 'included': 0, 'unrouted': 0}
    for source in sources:
        compiled = key(source) in tree.compiled
        index = named.get(source)
        if index is None:
            if compiled:
                counts['compiled'] += 1
            else:
                problems.append('%s: no target of this tree compiles it; add it to its target (or name its route in '
                                'scripts/source-routes.json)' % source)
            continue
        entry = entries[index]
        if 'built_when' in entry:
            expected = holds(entry['built_when'], tree)
            if expected and not compiled:
                problems.append('%s: built when %s, which holds here, but no target compiles it'
                                % (source, describe(entry['built_when'])))
            elif compiled and not expected:
                problems.append('%s: compiled here (%s) though entry %d says it is built only when %s'
                                % (source, tree.compiled[key(source)], index, describe(entry['built_when'])))
            else:
                counts['compiled' if compiled else 'gated off'] += 1
            continue
        if compiled:
            problems.append('%s: compiled here (%s), so entry %d no longer describes it'
                            % (source, tree.compiled[key(source)], index))
            continue
        if 'built_by' in entry:
            text = read(entry['built_by'])
            if text is None:
                problems.append('%s: built by %s, which does not exist' % (source, entry['built_by']))
            elif not names_source(text, source):
                problems.append('%s: built by %s, which does not name it' % (source, entry['built_by']))
            else:
                counts['built outside'] += 1
        elif 'included_by' in entry:
            text = read(entry['included_by'])
            if text is None:
                problems.append('%s: included by %s, which does not exist' % (source, entry['included_by']))
            elif not includes_source(text, source):
                problems.append('%s: included by %s, which does not include it' % (source, entry['included_by']))
            else:
                counts['included'] += 1
        else:
            counts['unrouted'] += 1
    return problems, counts


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--build', required=True, help='a configured tree with compile_commands.json')
    parser.add_argument('--manifest', default=str(MANIFEST))
    args = parser.parse_args(argv)
    manifest_path = Path(args.manifest)
    if not manifest_path.is_file() or not ROADMAP.is_file():
        print('ERROR: %s or %s is missing' % (manifest_path, ROADMAP))
        return 2
    try:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        tree = Tree.load(ROOT, Path(os.path.expanduser(args.build)))
        sources = repository_sources(ROOT, manifest.get('roots', []), set(manifest.get('extensions', [])))
    except SystemExit as error:
        print(error)
        return 2
    except (OSError, ValueError) as error:
        print('ERROR: %s' % error)
        return 2
    if not sources:
        print('ERROR: no source under %s; nothing was judged' % ', '.join(manifest.get('roots', [])))
        return 2
    cmake_files = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z', '--', '*CMakeLists.txt', '*.cmake'],
                                 capture_output=True, timeout=120, check=False).stdout.decode('utf-8').split('\0')
    cmake_text = '\n'.join((ROOT / path).read_text(encoding='utf-8', errors='replace')
                           for path in cmake_files if path and (ROOT / path).is_file())
    problems, counts = check(tree, manifest, sources, cmake_text, ROADMAP.read_text(encoding='utf-8'))
    for problem in problems:
        print('ERROR: ' + problem)
    summary = ', '.join('%d %s' % (count, label) for label, count in counts.items())
    print('source routes: %d sources under %s on %s (%d targets compile sources); %s; %s'
          % (len(sources), ', '.join(manifest.get('roots', [])), tree.system, len(tree.targets),
             summary or 'entries not applied', 'PASS' if not problems else 'FAIL: %d findings' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
