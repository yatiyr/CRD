#!/usr/bin/env python3
"""Hold the diagnostic investigation runbooks to the tree they describe.

docs/design/diagnostic-runbooks.md is the DIAG.11b clause (10) runbook set: one entry per symptom, each a table of
fixed fields (Symptom, Command, Artifact, Read, Next test, Hosted, Adopt, Example, Cost, Owner). A runbook that names
a script flag, a diag command or a test that no longer exists sends an investigation the wrong way, and nothing else
would notice, so every checkable part of every entry is checked here:

- every code span of Command, Next test and Adopt rows is one command of a known form, and what it names exists: a
  script and each of its flags (its own --help), a wsl-lane.sh command and preset, a ceridc verb, flag, diag command
  and grant, a test executable's target, a CMake function, macro or C++ name;
- nothing publishes: no commit, push, release, gist, issue, pull request, workflow dispatch or `gh api` write, and no
  grant of the service's `upload` or `remote-enable` class (publication is the user's step);
- every Artifact code span names something the sources write (a schema id, a file-name pattern's fixed part, an
  existing path), every Example span is an existing path, and every Cost row links to its measurement or owner;
- every DIAG child of docs/design/runtime-diagnostics.md is the Owner of an entry or listed, with its reason, among
  the children without an entry;
- with --build, every `ctest ... -R "<regex>"` (and `wsl-lane.sh test <preset> -R ...`) of a row that applies to the
  tree matches at least one test it registers, and every named executable's binary directory exists in it. A row's
  qualifier names a platform (`(Windows)`, `(Linux)`) or a build property the configuration must compile in
  (`(asserts)`, `(profiling)`; read from CMakeCache.txt), or both (`(Windows, profiling)`).

    python scripts/check-diag-runbooks.py --build build/win-debug        (the crd-check-diag-runbooks CTest)
    python scripts/check-diag-runbooks.py --static                       (no tree: the test references unresolved)

One of --build and --static is required, so a run that resolves no test says so in its arguments. Exit 0: every
check passed; 1: findings, each printed; 2: unusable input (no runbook, an unreadable tree).
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
RUNBOOK = ROOT / 'docs/design/diagnostic-runbooks.md'
DESIGN = ROOT / 'docs/design/runtime-diagnostics.md'
FIELDS = ('Symptom', 'Command', 'Artifact', 'Read', 'Next test', 'Hosted', 'Adopt', 'Example', 'Cost', 'Owner')
COMMAND_FIELDS = ('Command', 'Next test', 'Adopt')
PLATFORMS = ('Windows', 'Linux')
# A row qualifier naming a build property resolves only in a tree whose configuration compiles it in: the cases of a
# mode a configuration compiles out are not registered there (the RT sentinels without asserts, the CPROF and Perfetto
# cases without profiling; the shipping and release lanes).
BUILD_PROPERTIES = {'asserts': 'CRD_ENABLE_ASSERTS', 'profiling': 'CRD_ENABLE_PROFILING'}
CORPUS_DIRS = ('engine', 'tools', 'sandbox', 'runtime', 'tests', 'scripts', 'cmake', '.github', 'assets/diag')
CORPUS_SUFFIXES = {'.cpp', '.hpp', '.h', '.inl', '.py', '.cmake', '.txt', '.toml', '.yml', '.json', '.sh', '.bat',
                   '.ps1'}
AUTHORITIES = {'none', 'read', 'record', 'inject', 'execute', 'remote-enable', 'upload', 'process-memory'}
PUBLISHING_AUTHORITIES = {'upload', 'remote-enable'}
CTEST_FLAGS = {'--test-dir', '-R', '-L', '-E', '-j', '-C', '--output-on-failure', '-V', '-VV', '--repeat',
               '--timeout', '--schedule-random', '--stop-on-failure', '--no-tests=error', '-N'}
GIT_READS = {'log', 'show', 'diff', 'status', 'blame', 'bisect', 'rev-parse', 'grep'}
GH_READS = {('run', 'list'), ('run', 'view'), ('pr', 'view'), ('pr', 'checks'), ('pr', 'diff')}
EXTERNAL_TOOLS = {'cdb', 'gdb'}
ID = re.compile(r'DIAG\.\d+[a-z]?')
CHILD_HEADING = re.compile(r'^## (DIAG\.\d+[a-z]?) — ', re.MULTILINE)
TARGET_DEFINITION = re.compile(
    r'\b(?:add_executable|add_library|add_custom_target|crd_diag_specimen|crd_test_target)\(\s*([A-Za-z0-9_.+-]+)')
FUNCTION_DEFINITION = re.compile(r'^\s*(?:function|macro)\(\s*([A-Za-z0-9_]+)', re.MULTILINE | re.IGNORECASE)
PLACEHOLDER = re.compile(r'<[^<>]*>')


class Tree:
    """What the checks look things up in: the checkout, and the configured tree when given."""

    def __init__(self, root=ROOT, tests=None, platform=None, build=None, properties=None):
        self.root = Path(root)
        self.tests = tests  # CTest names of the configured tree, or None (static)
        self.properties = properties  # the build properties the tree compiles in ({'asserts', 'profiling'}), or None
        self.build = Path(build) if build else None  # the configured tree, whose binary directories are checked
        self.platform = platform or ('Windows' if os.name == 'nt' else 'Linux')
        self._corpus = None
        self._targets = None
        self._functions = None
        self._helps = {}
        self.symbols = {}  # name -> defined, memoized across checks of one tree
        self._contains = {}
        self._commands = None

    def corpus(self):
        if self._corpus is None:
            parts = []
            for top in CORPUS_DIRS:
                base = self.root / top
                if not base.is_dir():
                    continue
                for directory, names, files in os.walk(base):
                    names[:] = [n for n in names if n not in ('build', '__pycache__', '.git')]
                    for name in files:
                        path = Path(directory) / name
                        # A tooling test's fixtures (this guard's mutations among them) are not sources.
                        if path.suffix in CORPUS_SUFFIXES and not (top == 'scripts' and name.startswith('test-')):
                            try:
                                parts.append(path.read_text(encoding='utf-8', errors='replace'))
                            except OSError:
                                continue
            top_level = self.root / 'CMakeLists.txt'
            if top_level.is_file():
                parts.append(top_level.read_text(encoding='utf-8', errors='replace'))
            self._corpus = '\n'.join(parts)
        return self._corpus

    def contains(self, text):
        if text not in self._contains:
            self._contains[text] = text in self.corpus()
        return self._contains[text]

    def cmake_sources(self):
        files = list((self.root / 'cmake').glob('*.cmake'))
        for directory, names, found in os.walk(self.root):
            if Path(directory) == self.root:
                names[:] = [n for n in names if n not in ('build', '.git', 'out') and not n.startswith('.')]
            names[:] = [n for n in names if n not in ('__pycache__', '_deps')]
            if 'CMakeLists.txt' in found:
                files.append(Path(directory) / 'CMakeLists.txt')
        return [p.read_text(encoding='utf-8', errors='replace') for p in files]

    def targets(self):
        if self._targets is None:
            self._targets = set()
            for text in self.cmake_sources():
                self._targets.update(TARGET_DEFINITION.findall(text))
        return self._targets

    def functions(self):
        if self._functions is None:
            self._functions = set()
            for path in (self.root / 'cmake').glob('*.cmake'):
                self._functions.update(name.lower() for name in
                                       FUNCTION_DEFINITION.findall(path.read_text(encoding='utf-8', errors='replace')))
        return self._functions

    def presets(self):
        document = json.loads((self.root / 'CMakePresets.json').read_text(encoding='utf-8'))
        return {preset['name'] for preset in document.get('configurePresets', [])}

    def lane_commands(self):
        text = (self.root / 'scripts/wsl-lane.sh').read_text(encoding='utf-8')
        return set(re.findall(r'^    ([a-z]+)\)$', text, re.MULTILINE))

    def ceridc_usage(self):
        text = (self.root / 'tools/ceridc/src/main.cpp').read_text(encoding='utf-8', errors='replace')
        start = text.index('void print_usage()')
        return text[start:text.index('\n}\n', start)]

    def diag_commands(self):
        # The service's built-ins are a table of specs ({{"diag.commands", "perf", ...}}); upper modules name theirs
        # as constants (kGpuResourcesCommand{"gpu.resources"}); the flight and fault commands are spec literals.
        if self._commands is None:
            corpus = self.corpus()
            self._commands = set(re.findall(r'\{\{"([a-z]+\.[a-z.]+)", "', corpus))
            self._commands.update(re.findall(r'k[A-Z][A-Za-z]*Command\{"([a-z]+\.[a-z.]+)"\}', corpus))
            self._commands.update(re.findall(r'"((?:flight|fault)\.[a-z]+)"', corpus))
        return self._commands

    def help_text(self, script, subcommand=None):
        key = (script, subcommand)
        if key not in self._helps:
            arguments = [sys.executable, str(self.root / script)] + ([subcommand] if subcommand else []) + ['--help']
            try:
                completed = subprocess.run(arguments, cwd=str(self.root), capture_output=True, text=True,
                                           encoding='utf-8', errors='replace', timeout=60, check=False)
                self._helps[key] = (completed.returncode, completed.stdout + completed.stderr)
            except (OSError, subprocess.TimeoutExpired) as error:
                self._helps[key] = (-1, str(error))
        return self._helps[key]


def parse(text):
    """[(id, title, line, [(field, platform, value)])] for every entry, and the exempt children {id: reason}."""
    entries = []
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        anchor = re.fullmatch(r'<a id="(rb-[a-z0-9-]+)"></a>', lines[index].strip())
        if not anchor:
            index += 1
            continue
        start = index
        title = lines[index + 1][4:].strip() if index + 1 < len(lines) and lines[index + 1].startswith('### ') else ''
        rows = []
        index += 2
        while index < len(lines) and not lines[index].startswith('|'):
            index += 1
        while index < len(lines) and lines[index].startswith('|'):
            cells = [cell.strip() for cell in split_row(lines[index])]
            if len(cells) == 2 and cells[0] not in ('Field', '---'):
                field = re.fullmatch(r'(.+?)(?: \(([A-Za-z, ]+)\))?', cells[0])
                rows.append((field[1], field[2], cells[1]))
            index += 1
        entries.append((anchor[1], title, start + 1, rows))
    exempt = {}
    section = text.find('<a id="children-without-an-entry"></a>')
    if section >= 0:
        for line in text[section:].splitlines():
            match = re.fullmatch(r'\| (DIAG\.\d+[a-z]?) \| (.+) \|', line.strip())
            if match:
                exempt[match[1]] = match[2]
    return entries, exempt


def split_row(line):
    """The cells of a markdown table row, keeping a `|` inside a code span."""
    cells, current, in_code = [], [], False
    for char in line.strip()[1:-1]:
        if char == '`':
            in_code = not in_code
        if char == '|' and not in_code:
            cells.append(''.join(current))
            current = []
        else:
            current.append(char)
    cells.append(''.join(current))
    return cells


def code_spans(value):
    return re.findall(r'`([^`]+)`', value)


def tokens(command):
    """Shell words, quotes removed, backslashes kept (Windows paths, regex escapes). A chaining operator outside quotes
    comes out as a word of its own, so a quoted debugger script (".ecxr;kP;q") is one word and `a && b` is three."""
    lexer = shlex.shlex(command, posix=True, punctuation_chars=';&|')
    lexer.whitespace_split = True
    lexer.escape = ''
    lexer.commenters = ''
    return list(lexer)


def normal(path):
    return path.replace('\\', '/')


def flags_of(arguments):
    return [argument.split('=', 1)[0] for argument in arguments if argument.startswith('--')]


def check_grant(value, where):
    classes = {part.strip() for part in value.split(',') if part.strip()}
    problems = ['%s: unknown authority %r' % (where, name) for name in sorted(classes - AUTHORITIES)]
    problems += ['%s: grants %s; publication is the user\'s step, never a runbook command' % (where, name)
                 for name in sorted(classes & PUBLISHING_AUTHORITIES)]
    return problems


def check_script(tree, words, where):
    script = normal(words[0])
    if not script.startswith('scripts/') or not (tree.root / script).is_file():
        return ['%s: no script %s' % (where, script)]
    rest = words[1:]
    code, text = tree.help_text(script)
    if code != 0:
        return ['%s: %s --help exited %d' % (where, script, code)]
    subcommand = None
    choices = re.search(r'\{([a-z,-]+)\}', text)
    if rest and not rest[0].startswith('-') and choices and rest[0] in choices[1].split(','):
        subcommand = rest[0]
        code, text = tree.help_text(script, subcommand)
        if code != 0:
            return ['%s: %s %s --help exited %d' % (where, script, subcommand, code)]
    elif choices and rest and not rest[0].startswith('-') and '<' not in rest[0]:
        return ['%s: %s has no subcommand %r' % (where, script, rest[0])]
    known = set(re.findall(r'(?<![\w-])(--[a-z0-9][a-z0-9-]*)', text))
    return ['%s: %s does not take %s' % (where, ' '.join([script] + ([subcommand] if subcommand else [])), flag)
            for flag in flags_of(rest) if flag not in known]


def ctest_regex(arguments):
    for i, argument in enumerate(arguments):
        if argument == '-R' and i + 1 < len(arguments):
            return arguments[i + 1]
    return None


def qualifiers_of(qualifier):
    return [part.strip() for part in qualifier.split(',')] if qualifier else []


def applies(tree, qualifier):
    """Whether a row's qualifier holds for this tree: its platform, and the build properties it names (asserts,
    profiling: the tests of a mode the configuration compiles out are not registered there). A static run knows no
    build property, so a row that names one resolves nothing."""
    for part in qualifiers_of(qualifier):
        if part in PLATFORMS and part != tree.platform:
            return False
        if part in BUILD_PROPERTIES and (tree.properties is None or part not in tree.properties):
            return False
    return True


def with_linux(qualifier):
    """A wsl-lane.sh command runs in a Linux tree whatever its row says."""
    parts = qualifiers_of(qualifier)
    return ', '.join(parts if 'Linux' in parts else parts + ['Linux'])


def check_regex(tree, regex, platform, where):
    if regex is None or PLACEHOLDER.search(regex):
        return []
    try:
        compiled = re.compile(regex)
    except re.error as error:
        return ['%s: the regex %r does not compile: %s' % (where, regex, error)]
    if tree.tests is None or not applies(tree, platform):
        return []
    if not any(compiled.search(name) for name in tree.tests):
        return ['%s: no test of this tree matches %r' % (where, regex)]
    return []


def check_ctest(tree, words, platform, where):
    problems = []
    arguments = words[1:]
    if '--test-dir' not in arguments:
        problems.append('%s: name the tree with --test-dir build/<preset>' % where)
    else:
        directory = normal(arguments[arguments.index('--test-dir') + 1])
        preset = directory.split('/')[1] if directory.startswith('build/') and directory.count('/') >= 1 else None
        if preset not in tree.presets():
            problems.append('%s: --test-dir %s is not build/<a configure preset>' % (where, directory))
    i = 0
    while i < len(arguments):
        argument = arguments[i]
        if argument.startswith('-'):
            if argument not in CTEST_FLAGS:
                problems.append('%s: ctest flag %s is not one the runbooks use' % (where, argument))
            if argument in ('--test-dir', '-R', '-L', '-E', '-j', '-C', '--repeat', '--timeout'):
                i += 1
        i += 1
    if ctest_regex(arguments) is None and '-L' not in arguments:
        problems.append('%s: select the tests with -R or -L' % where)
    return problems + check_regex(tree, ctest_regex(arguments), platform, where)


def check_lane(tree, words, platform, where):
    if normal(words[0]) != 'scripts/wsl-lane.sh':
        return ['%s: bash runs scripts/wsl-lane.sh, not %s' % (where, words[0])]
    if len(words) < 2 or words[1] not in tree.lane_commands():
        return ['%s: wsl-lane.sh has no command %r' % (where, words[1] if len(words) > 1 else '')]
    problems = []
    if words[1] in ('configure', 'build', 'test', 'run', 'dev', 'tree'):
        if len(words) < 3 or words[2] not in tree.presets():
            return ['%s: wsl-lane.sh %s needs a configure preset, not %r'
                    % (where, words[1], words[2] if len(words) > 2 else '')]
        if words[1] == 'test':
            problems += check_regex(tree, ctest_regex(words[3:]), with_linux(platform), where)
    return problems


def check_ceridc(tree, words, where):
    usage = tree.ceridc_usage()
    if len(words) < 2 or ('ceridc %s' % words[1]) not in usage:
        return ['%s: ceridc has no verb %r' % (where, words[1] if len(words) > 1 else '')]
    problems = []
    for flag in flags_of(words[2:]):
        if flag not in usage:
            problems.append('%s: ceridc %s does not take %s' % (where, words[1], flag))
    for i, word in enumerate(words):
        value = words[i + 1] if i + 1 < len(words) else ''
        if word == '--command' and value not in tree.diag_commands():
            problems.append('%s: the diag service registers no command %r' % (where, value))
        if word in ('--grant', '--diag-grant'):
            problems += check_grant(value, where)
        if word == '--param' and '=' in value:
            name = value.split('=', 1)[0]
            if not tree.contains('"%s"' % name):
                problems.append('%s: no command takes the argument %r' % (where, name))
        if word == '--diag-host' and not PLACEHOLDER.search(value) and not (tree.root / normal(value)).is_file():
            problems.append('%s: no host profile %s' % (where, value))
    return problems


def check_executable(tree, words, platform, where):
    path = normal(words[0])
    parts = path.split('/')
    if len(parts) < 3 or parts[1] not in tree.presets():
        return ['%s: %s is not under build/<a configure preset>' % (where, path)]
    stem = re.sub(r'\.exe$', '', parts[-1])
    if stem not in tree.targets():
        return ['%s: no CMake target builds %s' % (where, parts[-1])]
    # A binary directory follows the add_subdirectory layout, not the source path (tests/foundation/jobs builds into
    # tests/jobs), and is the same in every tree of a platform; the program itself may not be built (EXCLUDE_FROM_ALL).
    directory = '/'.join(parts[2:-1])
    if tree.build is not None and applies(tree, platform) and not (tree.build / directory).is_dir():
        return ['%s: the configured tree has no directory %s for %s' % (where, directory, parts[-1])]
    if stem == 'ceridc':
        return check_ceridc(tree, ['ceridc'] + words[1:], where)
    return []


def check_command(tree, command, platform, where):
    """Findings for one runbook command (one code span of a Command, Next test or Adopt row)."""
    call = re.fullmatch(r'([A-Za-z_][A-Za-z0-9_:]*)\(.*\)', command.strip(), re.DOTALL)
    if call:
        return check_symbol(tree, call[1], where)
    try:
        words = tokens(command)
    except ValueError as error:
        return ['%s: unparsable %r: %s' % (where, command, error)]
    if not words:
        return ['%s: an empty code span' % where]
    if any(re.fullmatch(r'[;&|]+', word) for word in words):
        return ['%s: one command per code span, not a chain (%r)' % (where, command)]
    first = normal(words[0])
    name = first.rsplit('/', 1)[-1]
    if first == 'set' or first == 'export':
        if len(words) != 2 or not re.fullmatch(r'[A-Z][A-Z0-9_]*=\S*', words[1]):
            return ['%s: %s takes one NAME=value' % (where, first)]
        variable = words[1].split('=', 1)[0]
        return [] if tree.contains(variable) else ['%s: nothing reads %s' % (where, variable)]
    if first in ('python', 'python3', 'py'):
        if len(words) < 2:
            return ['%s: python without a script' % where]
        return check_script(tree, words[1:], where)
    if first == 'ctest':
        return check_ctest(tree, words, platform, where)
    if first in ('bash', 'wsl.exe', 'wsl'):
        rest = words[1:]
        if first != 'bash':
            if rest[:1] != ['--cd'] or len(rest) < 4 or rest[2:4] != ['-e', 'bash']:
                return ['%s: run a lane as wsl.exe --cd <checkout> -e bash scripts/wsl-lane.sh ...' % where]
            rest = rest[4:]
        return check_lane(tree, rest, with_linux(platform), where) if rest else ['%s: bash without a script' % where]
    if first == 'cmd':
        if words[1:2] != ['/c'] or len(words) < 3:
            return ['%s: cmd runs as cmd /c scripts\\<file>.bat' % where]
        words = words[2:]
        first = normal(words[0])
    if first.startswith('scripts/') and first.endswith('.bat'):
        if not (tree.root / first).is_file():
            return ['%s: no script %s' % (where, first)]
        if first == 'scripts/build-target.bat':
            directory = normal(words[1]) if len(words) > 1 else ''
            if len(words) < 3 or not directory.startswith('build/') or directory.split('/')[1] not in tree.presets():
                return ['%s: build-target.bat build\\<preset> <target>' % where]
            if words[2] not in tree.targets():
                return ['%s: no CMake target %s' % (where, words[2])]
        return []
    if first == 'cmake':
        if words[1:2] != ['--build'] or len(words) < 5 or words[3] != '--target':
            return ['%s: cmake --build build/<preset> --target <target>' % where]
        directory = normal(words[2])
        problems = [] if directory.startswith('build/') and directory.split('/')[1] in tree.presets() else [
            '%s: %s is not build/<a configure preset>' % (where, directory)]
        return problems + ['%s: no CMake target %s' % (where, target) for target in words[4:]
                           if target not in tree.targets()]
    if name in ('ceridc', 'ceridc.exe') and first in ('ceridc', 'ceridc.exe'):
        return check_ceridc(tree, words, where)
    if first.startswith('build/'):
        return check_executable(tree, words, platform, where)
    if first == 'git':
        if len(words) < 2 or words[1] not in GIT_READS:
            return ['%s: git %s is not a read; commit and push are the user\'s step' % (where, ' '.join(words[1:2]))]
        return []
    if first == 'gh':
        if tuple(words[1:3]) in GH_READS:
            return []
        if words[1:2] == ['api']:
            writes = [w for w in words[2:] if w in ('-X', '--method', '-f', '-F', '--field', '--raw-field', '--input')]
            return ['%s: gh api %s writes; publication is the user\'s step' % (where, writes[0])] if writes else []
        return ['%s: gh %s is not a read; publication is the user\'s step' % (where, ' '.join(words[1:3]))]
    if first in EXTERNAL_TOOLS:
        return []
    return ['%s: %r is not a command form the guard knows' % (where, command)]


def check_symbol(tree, name, where):
    last = name.rsplit('::', 1)[-1]
    if last not in tree.symbols:
        corpus = tree.corpus()
        # Cheapest first: the literal-prefixed patterns scan the corpus fast, the class pattern only when they miss.
        tree.symbols[last] = last.lower() in tree.functions() or bool(
            re.search(r'#\s*define\s+%s\b' % re.escape(last), corpus) or
            re.search(r'%s\s*\(' % re.escape(last), corpus) or
            re.search(r'\b(?:class|struct|enum class)\s+%s\b' % re.escape(last), corpus))
    return [] if tree.symbols[last] else ['%s: nothing defines %s' % (where, name)]


def check_adopt(tree, span, platform, where):
    """An Adopt span is a path to copy, a call (a CMake function, a macro, a C++ name), a command, or a setting whose
    fixed text the sources read (a profile key)."""
    stripped = span.strip()
    path = normal(stripped)
    if '/' in path and ' ' not in path and '(' not in path and not PLACEHOLDER.search(path):
        return [] if (tree.root / path).exists() else ['%s: no path %s' % (where, path)]
    first = stripped.split()[0]
    if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_:]*\(.*\)', stripped, re.DOTALL) or \
            first in ('python', 'python3', 'ctest', 'cmake', 'bash', 'wsl.exe', 'cmd', 'set', 'ceridc') or \
            normal(first).startswith(('build/', 'scripts/')):
        return check_command(tree, span, platform, where)
    return check_fragment(tree, stripped, where)


def check_fragment(tree, span, where):
    pieces = [piece for piece in re.split(r'<[^<>]*>|\*', span) if len(piece.strip()) >= 3]
    if not pieces:
        return ['%s: %r has no fixed part to look for' % (where, span)]
    longest = max(pieces, key=len).strip()
    return [] if tree.contains(longest) else ['%s: nothing in the sources writes or reads %r' % (where, longest)]


def check_artifact(tree, span, where):
    path = normal(span.strip()).rstrip('/')
    if re.fullmatch(r'cerid-[a-z0-9-]+/\d+', path):
        return [] if tree.contains('"%s"' % path) else ['%s: no source declares the schema %s' % (where, path)]
    if '/' in path and not PLACEHOLDER.search(path) and '*' not in path and not path.startswith('build/'):
        return [] if (tree.root / path).exists() else ['%s: no path %s' % (where, path)]
    return check_fragment(tree, path.rsplit('/', 1)[-1] if '/' in path else path, where)


def check_cost(tree, value, where):
    links = re.findall(r'\]\(([^)\s]+)\)', value)
    if not links:
        return ['%s: the cost names no measurement or owner (no link)' % where]
    problems = []
    for link in links:
        target = link.split('#', 1)[0]
        if target and not (RUNBOOK.parent / target).resolve().exists():
            problems.append('%s: the cost links to %s, which does not exist' % (where, link))
    return problems


def check(tree, text, design_text):
    entries, exempt = parse(text)
    problems = []
    if not entries:
        return ['the runbook holds no entry']
    children = set(CHILD_HEADING.findall(design_text))
    owned = set()
    seen = set()
    for entry_id, title, line, rows in entries:
        where = '%s (line %d)' % (entry_id, line)
        if entry_id in seen:
            problems.append('%s: a second entry with this id' % where)
        seen.add(entry_id)
        if not title:
            problems.append('%s: no ### title under the anchor' % where)
        present = {field for field, _, _ in rows}
        problems += ['%s: no %s row' % (where, field) for field in FIELDS if field not in present]
        problems += ['%s: unknown row %r' % (where, field) for field in sorted(present - set(FIELDS))]
        for field, platform, value in rows:
            at = '%s %s%s' % (where, field, ' (%s)' % platform if platform else '')
            problems += ['%s: unknown qualifier %r (a platform, %s)' % (at, part, ' or '.join(sorted(BUILD_PROPERTIES)))
                         for part in qualifiers_of(platform) if part not in PLATFORMS + tuple(BUILD_PROPERTIES)]
            spans = code_spans(value)
            if field in COMMAND_FIELDS:
                if not spans:
                    problems.append('%s: no command in a code span' % at)
                for span in spans:
                    problems += (check_adopt if field == 'Adopt' else check_command)(tree, span, platform, at)
            elif field == 'Artifact':
                problems += [p for span in spans for p in check_artifact(tree, span, at)]
            elif field == 'Example':
                if not spans:
                    problems.append('%s: no example path' % at)
                problems += ['%s: no path %s' % (at, span) for span in spans
                             if not (tree.root / normal(span)).exists()]
            elif field == 'Cost':
                problems += check_cost(tree, value, at)
            elif field == 'Owner':
                ids = set(ID.findall(value))
                if not ids:
                    problems.append('%s: names no DIAG child' % at)
                problems += ['%s: %s is not a DIAG child of the contract' % (at, child)
                             for child in sorted(ids - children)]
                owned |= ids
    problems += ['%s has no runbook entry and is not listed among the children without one' % child
                 for child in sorted(children - owned - set(exempt))]
    problems += ['%s is listed without an entry but owns one' % child for child in sorted(set(exempt) & owned)]
    problems += ['%s is listed without an entry but is not a DIAG child' % child
                 for child in sorted(set(exempt) - children)]
    return problems


def build_properties(build):
    """The BUILD_PROPERTIES a configured tree compiles in, from its CMakeCache.txt (ON, TRUE, YES, Y or 1)."""
    cache = (Path(build) / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace')
    found = set()
    for name, variable in BUILD_PROPERTIES.items():
        match = re.search(r'^%s(?::[A-Z]+)?=(.*)$' % re.escape(variable), cache, re.MULTILINE)
        if match and match[1].strip().upper() in ('ON', 'TRUE', 'YES', 'Y', '1'):
            found.add(name)
    return found


def tree_tests(build, config=None):
    arguments = ['ctest', '--show-only=json-v1'] + (['-C', config] if config else [])
    completed = subprocess.run(arguments, cwd=str(build), capture_output=True, text=True, encoding='utf-8',
                               errors='replace', timeout=300, check=False)
    if completed.returncode != 0:
        raise SystemExit('ERROR: ctest --show-only in %s exited %d: %s'
                         % (build, completed.returncode, completed.stderr.strip()[:400]))
    names = [test['name'] for test in json.loads(completed.stdout).get('tests', [])]
    if not names:
        raise SystemExit('ERROR: %s registers no test; nothing could be resolved' % build)
    return names


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', help='a configured tree whose CTest names resolve the runbooks\' test references')
    mode.add_argument('--static', action='store_true', help='check everything but the test references')
    parser.add_argument('--config', help='the configuration of a multi-configuration tree')
    parser.add_argument('--runbook', default=str(RUNBOOK))
    args = parser.parse_args(argv)
    runbook = Path(args.runbook)
    if not runbook.is_file() or not DESIGN.is_file():
        print('ERROR: %s or %s is missing' % (runbook, DESIGN))
        return 2
    tests = None
    if args.build:
        build = Path(os.path.expanduser(args.build))
        if not (build / 'CTestTestfile.cmake').is_file():
            print('ERROR: %s is not a configured tree with tests' % build)
            return 2
        tests = tree_tests(build, args.config)
    build = Path(os.path.expanduser(args.build)) if args.build else None
    tree = Tree(tests=tests, build=build, properties=build_properties(build) if build else None)
    problems = check(tree, runbook.read_text(encoding='utf-8'), DESIGN.read_text(encoding='utf-8'))
    entries, exempt = parse(runbook.read_text(encoding='utf-8'))
    for problem in problems:
        print('ERROR: ' + problem)
    scope = ('test references resolved against %d tests of %s (%s rows)'
             % (len(tests), args.build, ', '.join([tree.platform] + sorted(tree.properties)))
             if tests is not None else 'test references not resolved (--static)')
    print('diagnostic runbooks: %d entries, %d children without an entry; %s; %s'
          % (len(entries), len(exempt), scope, 'PASS' if not problems else 'FAIL: %d findings' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
