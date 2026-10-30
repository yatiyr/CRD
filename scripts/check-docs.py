#!/usr/bin/env python3
"""Documentation drift guard: living documents must agree with the source tree they describe.

check-master-plan.py validates the tracker, the entry budgets and every link under docs/ and the root entry points.
This guard adds the drift classes found by the 2026-10-31 documentation audit
(docs/sessions/2026-10-31-docs-health.md):

  links      links and anchors in every tracked Markdown file outside docs/ and the root entry points
  refs       repository paths, script names, CMake presets, `-DCRD_*` options and `crd-*` CTest/target names named
             in a living document exist in the tree
  facts      generated fact blocks (`<!-- fact:NAME -->value<!-- /fact -->`) equal the value derived from their source
             of truth (CPU levels, determinism modes, module counts, entry budgets, the copyright line)
  indexes    every document of an indexed directory is named by that directory's README, and every document under
             docs/ outside the dated session record is linked from at least one other document
  adrs       the ADR index names every decision exactly once, and a supersession statement is reciprocal: the other
             ADR names this one back
  phrases    living documents carry no retired process rule (agents never commit, the rejected Buildkite route as
             current, no parallel agents) and no emoji; no TODO/TBD/FIXME hides missing information

Dated records (docs/sessions, docs/bench boards, docs/archive, docs/research, ADR bodies, numbered lessons and
the band-prefixed design notes) are evidence: they are checked for links and index membership only, never for
current truth.

  python scripts/check-docs.py              # every check (stdlib only; run by check-master-plan.py too)
  python scripts/check-docs.py --refresh    # rewrite every fact block from its source of truth
  python scripts/check-docs.py --only refs  # one check class
"""
from pathlib import Path
from urllib.parse import unquote
import argparse
import importlib.util
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {'.git', '.claude', '.codex', '.agents', '.vs', '.vscode', '.idea', '.cpm-cache', '.sccache', '.venv',
             'build', 'external', 'out', 'node_modules', '__pycache__'}
ENTRY_POINTS = ('README.md', 'START_HERE.md', 'AGENTS.md', 'MEMORY.md', 'context.md', 'docs/README.md',
                'docs/BUILDING.md', 'docs/CODING.md', 'docs/CONTRIBUTING.md', 'docs/PRINCIPLES.md', 'docs/SANITY.md')
# Tool-specific files owned by the agent workspace overlay, not by the documentation set.
TOOL_FILES = ('CLAUDE.md',)
DATED_ROLES = {'evidence', 'historical', 'archive', 'session'}
# Band- or slice-prefixed design notes are dated decision packets: evidence, not current contracts.
DATED_DESIGN = re.compile(r'^(?:ceir-|raf|ren-|hesap_fft_)')
# Directories whose README is the index of every document beside it.
INDEXED_DIRS = ('docs/decisions', 'docs/design', 'docs/recipes', 'docs/research', 'docs/systems', 'docs/detours',
                'docs/bench', 'docs/phases', 'docs/lessons', 'docs/archive')
TOP_DIRS = ('.github', 'assets', 'bench', 'ci', 'cmake', 'docs', 'engine', 'runtime', 'sandbox', 'scripts', 'tests',
            'tools')
PRESET_NAME = re.compile(r'(?:win|linux|macos|mac|arm64|wasm|android|ios)-[a-z0-9]+(?:-[a-z0-9]+)*')
EMOJI = re.compile('[\u2600-\u27bf\u2b05-\u2b07\u2b50-\u2b55\ufe0f\U0001f000-\U0001faff]')
TODO = re.compile(r'\b(?:TODO|TBD|FIXME)\b')
# Retired process rules: a living document that states one contradicts the current rule in AGENTS.md.
# Each entry: pattern, reason, documents exempt (the owner of the current rule may state its own exceptions).
STALE_PHRASES = (
    (re.compile(r'never (?:run )?`?git commit|never commit or push|agents never commit|no agent commits|'
                r'nobody but the maintainer|user commits\b|user owns the final commit', re.I),
     'retired commit rule; AGENTS.md#git-and-workspace names who commits and pushes', {'AGENTS.md'}),
    (re.compile(r'no parallel agents', re.I),
     'retired 2026-10-04 assignment; parallel agents run by explicit user order (AGENTS.md)', set()),
    (re.compile(r'\bBuildkite\b(?![^.]*\b(?:reject|replac|supersed|instead of|first decision))', re.I),
     'Buildkite was rejected (ADR-0135 section 13); name it only as the rejected alternative',
     {'docs/decisions/0135-cerid-ci-platform.md'}),
    (re.compile(r'renderer[^.\n]{0,60}(?:\u2192|->|, then)[^.\n]{0,40}(?:crd-ui|\bUI\b|CR-D007)[^.\n]{0,60}'
                r'(?:\u2192|->|, then)[^.\n]{0,30}hesap GPU', re.I),
     'pre-2026-10-27 product order; the delivery plan and the ADR-0129 amendment place hesap GPU and the other '
     'backbones before the renderer library', set()),
)


def load_master_plan(root=ROOT):
    spec = importlib.util.spec_from_file_location('check_master_plan', root / 'scripts' / 'check-master-plan.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read(path):
    return path.read_text(encoding='utf-8-sig')


def markdown_files(root):
    result = []
    stack = [root]
    while stack:
        current = stack.pop()
        for child in sorted(current.iterdir()):
            if child.is_dir():
                if child.name not in SKIP_DIRS and not child.name.startswith('cmake-build-'):
                    stack.append(child)
            elif child.suffix.lower() == '.md':
                result.append(child.relative_to(root).as_posix())
    return sorted(result)


def doc_role(text):
    match = re.search(r'<!-- doc-role: ([a-z-]+) -->', text[:2000])
    return match[1] if match else None


def is_generated(text):
    return bool(re.search(r'GENERATED\b', '\n'.join(text.splitlines()[:3])))


def living_documents(root, files=None):
    """Documents that state current truth: entry points, rules, contracts, maps, indexes and tool READMEs."""
    files = markdown_files(root) if files is None else files
    result = []
    for name in files:
        parts = name.split('/')
        if name in TOOL_FILES or name == 'docs/ROADMAP.md':
            continue
        if name in ENTRY_POINTS or parts[-1] == 'README.md':
            result.append(name)
        elif len(parts) == 2 and parts[0] == 'docs':
            result.append(name)
        elif parts[0] == 'docs' and parts[1] == 'design' and len(parts) == 3 and not DATED_DESIGN.match(parts[2]):
            result.append(name)
        elif parts[0] == 'docs' and parts[1] in ('systems', 'hints', 'protocols') and len(parts) == 3:
            result.append(name)
        elif parts[0] not in ('docs',) and len(parts) > 1 and not is_generated(read(root / name)):
            result.append(name)
    # A dated record keeps its words: a document whose role marks it as evidence or history is never held to today.
    return [name for name in result if not is_generated(read(root / name))
            and doc_role(read(root / name)) not in DATED_ROLES]


def strip_code(text):
    """Prose only: fenced blocks and inline code spans removed, line structure kept."""
    lines = []
    fence = None
    for line in text.splitlines():
        marker = re.match(r'^\s*(`{3,}|~{3,})', line)
        if marker:
            if fence is None:
                fence = marker[1][0]
            elif marker[1][0] == fence:
                fence = None
            lines.append('')
            continue
        lines.append('' if fence else re.sub(r'(`+).*?\1', '', line))
    return lines


def code_tokens(text):
    """Inline code spans and fenced lines, which name paths, commands and presets."""
    tokens = []
    fence = None
    for line in text.splitlines():
        marker = re.match(r'^\s*(`{3,}|~{3,})', line)
        if marker:
            if fence is None:
                fence = marker[1][0]
            elif marker[1][0] == fence:
                fence = None
            continue
        if fence:
            tokens.append(line)
        else:
            tokens.extend(match[2] for match in re.finditer(r'(`+)(.+?)\1', line))
    return tokens


# ---------------------------------------------------------------------------------------------------------------
# links


def link_errors(root, files, master=None):
    """Links and anchors in the Markdown files check-master-plan.py does not walk (outside docs/ and the root)."""
    master = master or load_master_plan(root)
    errors = []
    cache = {}
    for name in files:
        path = root / name
        body = re.sub(r'(`+).*?\1', '', master.outside_fences(read(path)))
        for match in re.finditer(r'\]\((<[^>\n]+>|[^)\s]+)(?:\s+"[^"\n]*")?\)', body):
            url = match[1].strip('<>')
            if re.match(r'[A-Za-z][A-Za-z\d+.-]*:', url) or url.startswith('//'):
                continue
            dest, _, fragment = unquote(url).partition('#')
            target = (path.parent / dest).resolve() if dest else path
            if not target.exists():
                errors.append(f'{name}: missing path {url}')
            elif fragment and target.is_file() and target.suffix.lower() == '.md':
                if target not in cache:
                    cache[target] = master.anchors(target)
                if fragment not in cache[target]:
                    errors.append(f'{name}: missing anchor {url}')
    return errors


def extra_link_files(root, files):
    return [name for name in files if '/' in name and not name.startswith('docs/') and name not in TOOL_FILES]


# ---------------------------------------------------------------------------------------------------------------
# refs


def preset_names(root):
    data = json.loads(read(root / 'CMakePresets.json'))
    names = set()
    for key, value in data.items():
        if key.endswith('Presets') and isinstance(value, list):
            names.update(entry['name'] for entry in value if 'name' in entry)
    return names


def cmake_identifiers(root):
    names = set()
    sources = [root / 'CMakeLists.txt', root / 'CMakePresets.json']
    for top in ('cmake', 'engine', 'tests', 'tools', 'runtime', 'sandbox'):
        if (root / top).is_dir():
            sources.extend(path for path in (root / top).rglob('*')
                           if path.name == 'CMakeLists.txt' or path.suffix in ('.cmake', '.json'))
    for path in sources:
        if path.is_file():
            text = path.read_text(encoding='utf-8', errors='replace')
            names.update(re.findall(r'\bCRD_[A-Z0-9_]+\b', text))
            names.update(re.findall(r'\bcrd-[a-z0-9][a-z0-9-]*[a-z0-9]\b', text))
    return names


def tool_names(root):
    names = set()
    for top in ('scripts', 'tools', 'ci', 'tests', '.github', 'cmake', 'bench', 'sandbox', 'assets', 'runtime',
                'engine'):
        if (root / top).is_dir():
            names.update(path.name for path in (root / top).rglob('*')
                         if path.suffix in ('.py', '.bat', '.ps1', '.sh', '.cmd')
                         and not set(path.relative_to(root).parts) & SKIP_DIRS)
    return names


def workflow_jobs(root):
    """Hosted job names (`linux-gcc`, `host-check/fuzz`): CI vocabulary that looks like a preset name."""
    names = set()
    folder = root / '.github' / 'workflows'
    if folder.is_dir():
        for path in folder.glob('*.yml'):
            jobs = re.findall(r'^  ([a-z0-9][a-z0-9-]*):\s*$', read(path), flags=re.M)
            names.update(jobs)
            names.update(f'{path.stem}/{job}' for job in jobs)
    return names


def pin_names(root):
    path = root / 'cmake' / 'pins.json'
    if not path.is_file():
        return set()
    data = json.loads(read(path))
    return {f'{section}/{name}' for section, entries in data.items() if isinstance(entries, dict)
            for name in entries}


def source_macros(root):
    names = set()
    for top in ('engine', 'tests', 'tools', 'runtime', 'sandbox'):
        if (root / top).is_dir():
            for path in (root / top).rglob('*'):
                if path.suffix in ('.hpp', '.h', '.cpp', '.inl'):
                    names.update(re.findall(r'\bCRD_[A-Z0-9_]+\b', path.read_text(encoding='utf-8', errors='replace')))
    return names


def built_target(root, path):
    """A build-tree program path such as `runtime/cpu_level_record`: its directory's CMakeLists defines it."""
    folder, _, target = path.rpartition('/')
    cmake = root / folder / 'CMakeLists.txt'
    return bool(folder) and cmake.is_file() and \
        re.search(r'add_executable\(\s*' + re.escape(target) + r'\b', read(cmake)) is not None


def glob_escape(text):
    return re.sub(r'([*?\[])', r'[\1]', text)


# A line that narrates history (a deleted file, a former build, a rename) names paths that are gone by design.
HISTORY = re.compile(r'\b(?:former(?:ly)?|deleted|retired|removed|renamed|no longer|was|were|replaced)\b', re.I)
EXTERNAL_SCRIPTS = {'vcvars64.bat', 'vcvarsall.bat', 'VsDevCmd.bat'}


def reference_errors(root, docs):
    presets = preset_names(root)
    jobs = workflow_jobs(root)
    pins = pin_names(root)
    identifiers = cmake_identifiers(root)
    tools = tool_names(root) | EXTERNAL_SCRIPTS
    macros = None
    tops = '|'.join(re.escape(top) for top in TOP_DIRS)
    any_path = re.compile(r'(?<![\w/.\-~$%])((?:' + tops + r')/[\w./-]*[\w/])(?![\w<*{$]|\.[*{])')
    file_path = re.compile(r'(?<![\w/.\-~$%])((?:' + tops + r')/[\w./-]*\.[A-Za-z0-9]{1,6})(?![\w<*{$/]|\.[*{])')
    errors = []

    def missing_path(path):
        # A stem names its sources (`sandbox/src/inspect_panel` is the .hpp/.cpp pair).
        target = root / path
        return not (target.exists() or path in pins or built_target(root, path) or
                    (target.parent.is_dir() and any(target.parent.glob(glob_escape(target.name) + '.*'))))

    for name in docs:
        text = read(root / name)
        seen = set()

        def report(key, message):
            if key not in seen:
                seen.add(key)
                errors.append(f'{name}: {message}')
        # Prose: only file-like paths (a slash also means "or" in prose); link targets are the link checks' work.
        for line in strip_code(text):
            if HISTORY.search(line):
                continue
            line = re.sub(r'\]\([^)]*\)', ']', line)
            line = re.sub(r'https?://\S+', '', line)
            for match in file_path.finditer(line):
                path = match[1]
                if '...' not in path and 'YYYY' not in path and missing_path(path):
                    report(path, f'names a missing path {path}')
        fence = None
        for line in text.splitlines():
            marker = re.match(r'^\s*(`{3,}|~{3,})', line)
            if marker:
                fence = marker[1][0] if fence is None else (None if marker[1][0] == fence else fence)
                continue
            if HISTORY.search(line) and not fence:
                continue
            tokens = [line] if fence else [match[2] for match in re.finditer(r'(`+)(.+?)\1', line)]
            for token in tokens:
                for match in any_path.finditer(token):
                    path = match[1]
                    # A fence is example code: only its commands' scripts are held to the tree.
                    if fence and not re.search(r'\.(?:py|bat|ps1|sh)$', path):
                        continue
                    if '...' not in path and 'YYYY' not in path and missing_path(path):
                        report(path, f'names a missing path {path}')
                for match in re.finditer(r'(?<![\w/.\-<])([\w-]+\.(?:py|bat|ps1|sh))\b', token):
                    if match[1] not in tools:
                        report(match[1], f'names a missing script {match[1]}')
                # The build tree is ignored scratch: a tool kept there is not a repository source anyone can run.
                for match in re.finditer(r'(?<![\w/.\-~])(build/[\w./-]+\.(?:py|bat|ps1|sh))\b', token):
                    report(match[1], f'runs a tool from the untracked build tree: {match[1]} (commit it under '
                                     'scripts/ or describe it as a dated, local tool)')
                for match in re.finditer(r'(?:--preset[ =]|configure-preset\.bat |reconfigure-preset\.bat |'
                                         r'wsl-lane\.sh \w+ |build/)(' + PRESET_NAME.pattern + r')\b', token):
                    if match[1] not in presets:
                        report(match[1], f'names a missing preset {match[1]}')
                stripped = token.strip()
                if not fence and PRESET_NAME.fullmatch(stripped) and stripped not in presets | jobs:
                    report(stripped, f'names a missing preset or CI job {stripped}')
                for match in re.finditer(r'-D(CRD_[A-Z0-9_]+)', token):
                    if match[1] not in identifiers:
                        macros = source_macros(root) if macros is None else macros
                        if match[1] not in macros:
                            report(match[1], f'names a CMake option or macro nothing defines: {match[1]}')
                if not fence and re.fullmatch(r'crd-(?:check|no)-[a-z0-9-]+', stripped) and \
                        stripped not in identifiers:
                    report(stripped, f'names a guard CTest no CMake source registers: {stripped}')
    return errors


# ---------------------------------------------------------------------------------------------------------------
# facts


def cpu_levels(root):
    """The CPU dispatch levels, lowest first, with the source agreement errors (ADR-0134)."""
    header = read(root / 'engine/foundation/core/include/crd/core/cpu_features.hpp')
    body = re.search(r'enum class CpuLevel\b[^{]*\{(.*?)\};', header, flags=re.S)
    enum = re.findall(r'^\s*(\w+)\s*,', body[1], flags=re.M) if body else []
    source = read(root / 'engine/foundation/core/src/cpu_features.cpp')
    names = dict(re.findall(r'case CpuLevel::(\w+):\s*return "([^"]+)";', source))
    cmake = dict((key, int(value)) for key, value in
                 re.findall(r'set\(_CRD_KERNEL_LEVEL_(\w+) (\d+)\)', read(root / 'cmake/CrdSimd.cmake')))
    errors = []
    levels = []
    for index, member in enumerate(enum):
        spelling = names.get(member)
        if spelling is None:
            errors.append(f'cpu_level_name has no spelling for CpuLevel::{member}')
            continue
        levels.append(spelling)
        if cmake.get(spelling.replace('.', '')) != index:
            errors.append(f'cmake/CrdSimd.cmake _CRD_KERNEL_LEVEL_{spelling.replace(".", "")} is not {index}')
    if not enum:
        errors.append('CpuLevel enum not found in crd/core/cpu_features.hpp')
    if set(cmake) != {level.replace('.', '') for level in levels}:
        errors.append('cmake/CrdSimd.cmake kernel levels differ from CpuLevel: ' + ', '.join(sorted(cmake)))
    return levels, errors


def determinism_modes(root):
    header = read(root / 'engine/foundation/math/include/crd/math/determinism_mode.hpp')
    body = re.search(r'enum class DeterminismMode\b[^{]*\{(.*?)\};', header, flags=re.S)
    return re.findall(r'^\s*(\w+)\s*=', body[1], flags=re.M) if body else []


def module_paths(root):
    """The `crd_module()` registrations of the root CMakeLists: name -> directory."""
    text = read(root / 'CMakeLists.txt')
    result = {}
    for body in re.findall(r'crd_module\(([^()]*(?:\([^()]*\)[^()]*)*)\)', text):
        tokens = body.split()
        if not tokens:
            continue
        override = re.search(r'\bNAME\s+([A-Za-z][A-Za-z0-9_-]*)', body)
        result[override[1] if override else tokens[0].rsplit('/', 1)[-1]] = tokens[0]
    return result


def module_test_directories(root):
    """The distinct `tests/<dir>` suites the `crd_module()` rows declare under TESTS."""
    keywords = {'HOST', 'NAME', 'EXECUTABLES', 'DEPENDS', 'PACKAGES', 'TESTS', 'TEST_DEPENDS', 'TEST_PACKAGES'}
    result = set()
    for body in re.findall(r'crd_module\(([^()]*(?:\([^()]*\)[^()]*)*)\)', read(root / 'CMakeLists.txt')):
        current = None
        for token in re.sub(r'#[^\n]*', '', body).split():
            if token in keywords:
                current = token
            elif current == 'TESTS':
                result.add(token)
    return result


def native_configurations(root):
    """The native Visual Studio configurations: CMAKE_CONFIGURATION_TYPES of the multi-config preset."""
    data = json.loads(read(root / 'CMakePresets.json'))
    for preset in data.get('configurePresets', []):
        value = preset.get('cacheVariables', {}).get('CMAKE_CONFIGURATION_TYPES')
        if isinstance(value, str):
            return [name for name in value.split(';') if name]
    return []


def series(words):
    quoted = [f'`{word}`' for word in words]
    return quoted[0] if len(quoted) == 1 else ', '.join(quoted[:-1]) + ' and ' + quoted[-1]


def fact_values(root, master=None):
    """Every fact a document may quote, derived from its single source of truth."""
    master = master or load_master_plan(root)
    levels, _ = cpu_levels(root)
    modules = module_paths(root)
    engine = [name for name, path in modules.items() if path.startswith('engine/')]
    labels = {'docs/README.md': 'this map'}
    budgets = '; '.join(f'{labels.get(path, Path(path).stem)} {limit:,}' for path, limit in master.BUDGETS.items())
    license_line = next((line.strip() for line in read(root / 'LICENSE').splitlines()
                         if line.startswith('Copyright (c)')), '')
    return {
        'cpu-levels': series(levels),
        'determinism-modes': series(determinism_modes(root)),
        'module-count': str(len(modules)),
        'engine-module-count': str(len(engine)),
        'other-module-count': str(len(modules) - len(engine)),
        'host-tool-count': str(sum(1 for path in modules.values() if path.startswith('tools/'))),
        'test-directory-count': str(len(module_test_directories(root))),
        'entry-budgets': budgets,
        'native-profile-count': str(len(native_configurations(root))),
        'copyright': license_line,
    }


FACT = re.compile(r'<!-- fact:([a-z0-9-]+) -->(.*?)<!-- /fact -->', re.S)
NUMBER_WORDS = ('zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine', 'ten', 'eleven',
                'twelve')
# A hand-written count of something a fact block derives: it must use the fact block (or no number at all).
COUNT_CLAIMS = (
    (re.compile(r'\b(\d+|' + '|'.join(NUMBER_WORDS) + r')\s+native\s+(?:MSVC\s+)?(?:profiles|configurations)\b',
                re.I), 'native-profile-count'),
    (re.compile(r'\b(\d+|' + '|'.join(NUMBER_WORDS) + r')\s+registered\s+modules\b', re.I), 'module-count'),
    (re.compile(r'\b(\d+|' + '|'.join(NUMBER_WORDS) + r')\s+engine\s+modules\b', re.I), 'engine-module-count'),
)


def fact_errors(root, docs, refresh=False, master=None):
    values = fact_values(root, master)
    errors = []
    for name in docs:
        path = root / name
        text = read(path)

        def replace(match):
            key = match[1]
            if key not in values:
                errors.append(f'{name}: unknown fact block {key}')
                return match[0]
            if match[2] != values[key]:
                if not refresh:
                    errors.append(f'{name}: fact {key} reads "{match[2]}", its source says "{values[key]}" '
                                  '(python scripts/check-docs.py --refresh)')
                return f'<!-- fact:{key} -->{values[key]}<!-- /fact -->'
            return match[0]
        updated = FACT.sub(replace, text)
        if refresh and updated != text:
            path.write_bytes(updated.encode('utf-8'))
        for number, line in enumerate(strip_code(FACT.sub('', read(path))), 1):
            if HISTORY.search(line):
                continue
            for pattern, key in COUNT_CLAIMS:
                for match in pattern.finditer(line):
                    word = match[1].lower()
                    value = int(word) if word.isdigit() else NUMBER_WORDS.index(word)
                    if str(value) != values[key]:
                        errors.append(f'{name}:{number}: "{match[0]}" disagrees with its source ({values[key]}); '
                                      f'use <!-- fact:{key} -->')
    _, level_errors = cpu_levels(root)
    errors.extend('CPU level sources disagree: ' + error for error in level_errors)
    return errors


# ---------------------------------------------------------------------------------------------------------------
# indexes


def index_errors(root, files):
    """Indexed directories name every member in their README; every other document under docs/ (outside the dated
    sessions) is reached by a link from some other document."""
    errors = []
    indexed = set()
    for directory in INDEXED_DIRS:
        folder = root / directory
        if not folder.is_dir():
            continue
        members = [path.name for path in sorted(folder.glob('*.md')) if path.name != 'README.md']
        index = folder / 'README.md'
        if not index.is_file():
            if members:
                errors.append(f'{directory}: no README.md index for {len(members)} documents')
            continue
        text = read(index)
        for member in members:
            if member not in text:
                errors.append(f'{directory}/README.md does not index {member}')
            else:
                indexed.add(f'{directory}/{member}')
    inbound = set()
    for name in files:
        path = root / name
        for match in re.finditer(r'\]\((<[^>\n]+>|[^)\s]+)', read(path)):
            url = match[1].strip('<>')
            if re.match(r'[A-Za-z][A-Za-z\d+.-]*:', url):
                continue
            dest = unquote(url).partition('#')[0]
            if dest:
                target = (path.parent / dest).resolve()
                if target != path.resolve():
                    inbound.add(target)
    for name in files:
        if not name.startswith('docs/') or name.startswith('docs/sessions/') or name == 'docs/README.md' or \
                name in indexed:
            continue
        if (root / name).resolve() not in inbound:
            errors.append(f'{name}: no document links to it (add it to its index or owning row)')
    return errors


# ---------------------------------------------------------------------------------------------------------------
# adrs


def adr_errors(root):
    folder = root / 'docs/decisions'
    files = sorted(path for path in folder.glob('[0-9][0-9][0-9][0-9]-*.md'))
    errors = []
    numbers = {}
    for path in files:
        numbers.setdefault(path.name[:4], []).append(path.name)
    errors.extend(f'ADR number {number} used by {", ".join(names)}' for number, names in numbers.items()
                  if len(names) > 1)
    index = read(folder / 'README.md')
    for path in files:
        count = len(re.findall(r'\]\(' + re.escape(path.name) + r'\)', index))
        if count != 1:
            errors.append(f'docs/decisions/README.md links {path.name} {count} times (exactly once required)')
    texts = {path.name[:4]: read(path) for path in files}
    rows = {match[1]: match[0] for match in re.finditer(r'^\|\s*\[(\d{4})-[^]]*\]\([^)]*\)\s*\|.*$', index, re.M)}
    for path in files:
        number = path.name[:4]
        if header_superseded(texts[number]) and number in rows and not re.search(r'supersed', rows[number], re.I):
            errors.append(f'docs/decisions/README.md: the row of ADR-{number} omits the supersession its status '
                          'records')
    for number, other in supersession_pairs(texts):
        if other not in texts:
            errors.append(f'ADR-{number} names a missing ADR-{other} in a supersession statement')
            continue
        if (number, other) in RECIPROCITY_EXEMPT:
            continue
        back = re.compile(r'ADR-' + number + r'\b|\b' + number + r'-[\w-]+\.md|ADR-\d{4}(?:/\d{4})*/' + number +
                          r'\b')
        if not back.search(texts[other]):
            errors.append(f'ADR-{number} states a supersession with ADR-{other}, which does not name ADR-{number} '
                          'back (add a supersession note to the other record; never rewrite the decision)')
    return sorted(set(errors))


# "<this> superseded [in place|in part] [(...)] by [the user-chosen] ADR-Y[/Z]" and "<this> supersedes ADR-Y",
# "[ADR-Y]...superseded in place (ADR-Z)": each names the other record of a supersession.
SUPERSEDED_BY = re.compile(r'superseded(?:[- ]in[- ](?:place|part))?\**\s*(?:\((?=ADR)|(?:\([^)]*\)\s*)?by\s+'
                           r'(?:the\s+)?(?:[\w-]+\s+){0,2}\[?)ADR-(\d{4})((?:/\d{4})*)', re.I)
SUPERSEDES = re.compile(r'\bsupersedes(?:/strikes|-in-place(?: target)?)?(?:\s*\(on acceptance\))?:?\**\s*\**\[?'
                        r'ADR-(\d{4})((?:/\d{4})*)', re.I)
SUBJECT_SUPERSEDED = re.compile(r'\[?ADR-(\d{4})\]?(?:\([^)]*\))?\s+(?:is|are)\s+\**superseded', re.I)
# Pairs whose wording names an ADR beside "superseded" without a decision-to-decision supersession.
RECIPROCITY_EXEMPT = {
    ('0105', '0104'): 'ADR-0105 lists a module "mostly superseded by ADR-0104 deploy", not a superseded decision',
}


def header_superseded(text):
    """The record's own status (the header before its first section) says it is wholly or partly superseded."""
    header = re.split(r'^## ', text, maxsplit=1, flags=re.M)[0]
    return any(re.search(r'superseded', line, re.I) and re.search(r'status|superseded-by', line, re.I)
               for line in re.sub(r'\]\([^)]*\)', ']', header).splitlines())


def supersession_pairs(texts):
    pairs = set()
    for number, text in texts.items():
        prose = re.sub(r'\]\([^)]*\)', ']', text)
        for line in prose.splitlines():
            if re.search(r"\bnot supersede|doesn't supersede|supersedes:?\**\s*\(?none", line, re.I):
                continue
            for pattern in (SUPERSEDED_BY, SUPERSEDES):
                for match in pattern.finditer(line):
                    for other in [match[1]] + re.findall(r'\d{4}', match[2]):
                        if other != number:
                            pairs.add((number, other))
            for match in SUBJECT_SUPERSEDED.finditer(line):
                if match[1] != number:
                    pairs.add((number, match[1]))
    return sorted(pairs)


# ---------------------------------------------------------------------------------------------------------------
# phrases


def phrase_errors(root, docs):
    errors = []
    for name in docs:
        # Fenced blocks are captured tool output (a checkmark column of a board); prose speaks in plain words.
        prose = strip_code(read(root / name))
        for number, line in enumerate(prose, 1):
            if EMOJI.search(line):
                errors.append(f'{name}:{number}: emoji or pictograph {EMOJI.search(line)[0]!r} (plain words only)')
            owned = 'ROADMAP.md#slice-' in line
            line = re.sub(r'\]\([^)]*\)', ']', line)
            unquoted = re.sub(r'"[^"]*"', '', line)
            if TODO.search(unquoted) and not owned:
                errors.append(f'{name}:{number}: {TODO.search(unquoted)[0]} without an owning ROADMAP row link')
            for pattern, reason, exempt in STALE_PHRASES:
                if name not in exempt and pattern.search(line):
                    errors.append(f'{name}:{number}: {reason}')
    return errors


# ---------------------------------------------------------------------------------------------------------------


CHECKS = ('links', 'refs', 'facts', 'indexes', 'adrs', 'phrases')


def check(root=ROOT, only=None, refresh=False):
    master = load_master_plan(root)
    files = markdown_files(root)
    docs = living_documents(root, files)
    selected = only or CHECKS
    errors = []
    if 'links' in selected:
        errors += link_errors(root, extra_link_files(root, files), master)
    if 'refs' in selected:
        errors += reference_errors(root, docs)
    if 'facts' in selected:
        errors += fact_errors(root, docs, refresh, master)
    if 'indexes' in selected:
        errors += index_errors(root, [name for name in files if name not in TOOL_FILES])
    if 'adrs' in selected:
        errors += adr_errors(root)
    if 'phrases' in selected:
        errors += phrase_errors(root, docs)
    return len(files), len(docs), errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--refresh', action='store_true', help='rewrite every fact block from its source of truth')
    parser.add_argument('--only', choices=CHECKS, action='append', help='run one check class (repeatable)')
    args = parser.parse_args(argv)
    count, living, errors = check(ROOT, args.only, args.refresh)
    print(f'Checked {count} Markdown documents, {living} of them living.')
    for error in errors:
        print('ERROR: ' + error)
    print('PASS' if not errors else f'FAIL: {len(errors)} issues')
    return 1 if errors else 0


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
