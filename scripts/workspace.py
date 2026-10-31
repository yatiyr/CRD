#!/usr/bin/env python3
"""Keep the whole Cerid working environment portable through the operator's NAS (stdlib only, Python 3.12+).

Two Git histories share one checkout. The code repository (`.git`) holds code and every document and is the only
history pushed to the code hosts. The agent overlay is a second repository whose Git directory is `.agent-overlay/`
and whose work tree is the same checkout root; it tracks only agent tool files (CLAUDE.md, opencode.json, .claude/,
.codex/, ...) and `.agent/`, which holds copies of the agent state kept outside the checkout (the Claude Code memory
folder of this project and the operator's workflow scripts). The overlay travels only to the NAS.

The NAS folder receives only a few large single files, each written as `<name>.partial`, read back, compared by
SHA-256 (and `git bundle verify`), then renamed: full Git bundles of the code and of the overlay (the newest
generations kept), the pinned tool archives, export archives and one MANIFEST.json. Nothing outside that folder is
touched, an existing file is never overwritten, only files recorded in the manifest are pruned, a lock file keeps
one writer at a time, and a restore only reads.

    python scripts/workspace.py sync [--dry-run] [--nas DIR] [--no-code-remotes] [--break-lock]
    python scripts/workspace.py status [--nas DIR] [--offline]
    python scripts/workspace.py export [--nas DIR | --output-dir DIR]
    python scripts/workspace.py restore --from <archive.zip | NAS dir> <destination folder>
    python scripts/workspace.py verify <archive.zip>
    python scripts/workspace.py overlay <git arguments...>

An export is one zip archive that is also a Python zip application: `python crd-workspace-YYYYMMDD.zip restore
<folder>` restores it on a machine that has only Python 3.12+ and Git. Transcripts, credential files and secret
token shapes are refused everywhere; an export refuses anything marked confidential. Contract and plain-English
steps: docs/recipes/workspace-sync.md and docs/decisions/0136-workspace-overlay.md.
"""
from pathlib import Path, PurePosixPath
import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

SCHEMA = 'cerid-workspace/1'
NAS_SCHEMA = 'cerid-workspace-nas/1'
MANIFEST_NAME = 'MANIFEST.json'
LOCK_NAME = 'crd.lock'
DEFAULT_GENERATIONS = 5
OVERLAY_GIT_DIR = '.agent-overlay'
OVERLAY_HOME = '.agent'
OVERLAY_BRANCH = 'main'
CONFIG_FILE = OVERLAY_HOME + '/workspace.json'
MEMORY_DIR = OVERLAY_HOME + '/claude-memory'
WORKFLOWS_DIR = OVERLAY_HOME + '/workflows'
USER_CLAUDE_DIR = OVERLAY_HOME + '/user-claude'
# User-level Claude Code files of this project, always synced (relative to the Claude Code folder); the
# configuration's user_claude_files adds more, and agents naming Cerid or crd and crd-* files join automatically.
DEFAULT_USER_CLAUDE_FILES = ('agents/arch-review.md', 'agents/ci-monitor.md', 'agents/doc-writer.md',
                             'agents/project-status.md', 'crd-loop-watchdog.ps1')
# Agent tool files at the checkout root that the overlay tracks in place (the code repository ignores them).
OVERLAY_ROOT_FILES = ('CLAUDE.md', 'CLAUDE.local.md', 'opencode.json', '.mcp.json')
OVERLAY_ROOT_DIRS = ('.claude', '.codex', '.agents')
# Machine-local state inside those directories: never part of the overlay.
LOCAL_STATE_DIRS = {'worktrees', 'cache', 'caches', 'log', 'logs', 'tmp', 'temp', 'shell-snapshots', 'file-history',
                    'paste-cache', 'todos', 'statsig', 'sessions', 'projects', 'backups', '__pycache__'}
LOCAL_STATE_FILES = ('*.lock', '*.log', '*.tmp', '*.pid', '*.swp')
# The code repository must never receive these paths again (forward-only: history keeps its old copies).
AGENT_PATHS = OVERLAY_ROOT_FILES + OVERLAY_ROOT_DIRS + (OVERLAY_HOME, OVERLAY_GIT_DIR)
OVERLAY_EXCLUDE = '# The overlay stages an explicit file list (scripts/workspace.py); nothing else.\n*\n'
OVERLAY_ATTRIBUTES = ('# Agent files are stored byte-exact: no line-ending, filter or encoding conversion.\n'
                      '* -text !eol -filter -ident !working-tree-encoding\n')
CONFIDENTIAL_MARKER = '.cerid-confidential'
ROOT_TOKEN = '{{CERID_ROOT}}'
NATIVE_ROOT_TOKEN = '{{CERID_ROOT_NATIVE}}'
TRANSCRIPT_SUFFIXES = ('.jsonl',)
CREDENTIAL_NAMES = ('.env', '.env.*', '*.pem', '*.key', '*.p12', '*.pfx', 'id_rsa*', 'id_dsa*', 'id_ecdsa*',
                    'id_ed25519*', '.credentials.json', 'credentials.json', 'auth.json', '.netrc', '_netrc',
                    '.git-credentials', '*.kdbx')
# High-confidence token shapes only: a keyword such as "password" occurs harmlessly in permission rules and notes.
SECRET_PATTERNS = {
    'private key': r'-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----',
    'AWS access key': r'\b(?:AKIA|ASIA)[0-9A-Z]{16}\b',
    'GitHub token': r'\bgh[pousr]_[A-Za-z0-9]{36,}\b',
    'GitHub fine-grained token': r'\bgithub_pat_[A-Za-z0-9_]{40,}\b',
    'GitLab token': r'\bglpat-[A-Za-z0-9_\-]{20,}\b',
    'Slack token': r'\bxox[abprs]-[A-Za-z0-9-]{10,}\b',
    'Anthropic key': r'\bsk-ant-[A-Za-z0-9_\-]{20,}\b',
    'OpenAI key': r'\bsk-(?:proj-)?[A-Za-z0-9_\-]{32,}\b',
    'Google API key': r'\bAIza[0-9A-Za-z_\-]{35}\b',
    'Hugging Face token': r'\bhf_[A-Za-z0-9]{34,}\b',
    'npm token': r'\bnpm_[A-Za-z0-9]{36}\b',
    'credentials in a URL': r'\b[a-z][a-z0-9+.\-]*://[^/\s:@"\'<>]+:[^/\s@"\'<>]+@[^\s/"\'<>]+',
}
SECRET_RE = re.compile('|'.join(f'(?P<p{i}>{p})' for i, p in enumerate(SECRET_PATTERNS.values())))
SECRET_NAMES = list(SECRET_PATTERNS)
STORED = ('.bundle', '.zip', '.gz', '.tgz', '.xz', '.bz2', '.zst', '.7z', '.whl', '.png', '.jpg')


class WorkspaceError(RuntimeError):
    pass


class Report:
    """Ordered result lines; any error makes the command exit 1 after the full report is printed."""

    def __init__(self):
        self.lines = []
        self.errors = 0

    def add(self, section, text):
        self.lines.append(f'[{section}] {text}')

    def error(self, section, text):
        self.errors += 1
        self.lines.append(f'[{section}] ERROR: {text}')

    def print(self, stream=None):
        stream = stream or sys.stdout
        for line in self.lines:
            print(line, file=stream)
        print('FAIL: %d issue(s)' % self.errors if self.errors else 'OK', file=stream)


# ---------------------------------------------------------------------------------------------- paths and keys

def claude_project_key(path):
    """Directory name Claude Code uses under <config>/projects for a project folder.

    Observed behaviour (not source-verified): every character outside [A-Za-z0-9] becomes '-', case is preserved.
    `D:\\Dev\\cerid` -> `D--Dev-cerid`, `/home/u/cerid` -> `-home-u-cerid`. Names longer than 200 characters are
    shortened by Claude Code with a suffix this tool does not reproduce; `claude_memory_dir` handles that case.
    """
    return re.sub(r'[^A-Za-z0-9]', '-', str(path))


def claude_config_dir(override=None):
    if override:
        return Path(override)
    configured = os.environ.get('CLAUDE_CONFIG_DIR')
    return Path(configured) if configured else Path.home() / '.claude'


def claude_project_dir(project_path, config_dir):
    key = claude_project_key(project_path)
    projects = Path(config_dir) / 'projects'
    if len(key) <= 200:
        return projects / key
    if projects.is_dir():
        matches = sorted(p for p in projects.iterdir() if p.is_dir() and p.name.startswith(key[:200] + '-'))
        if len(matches) == 1:
            return matches[0]
    raise WorkspaceError(f'The Claude Code project directory for {project_path} has a shortened, hashed name this '
                         f'tool cannot derive; pass --memory-dir <dir> (open Claude Code there once to create it).')


def default_downloads_dir():
    return Path.home() / '.cerid' / 'downloads'


def posix(path):
    return Path(path).as_posix()


def native_root(path):
    return str(Path(path)) if os.name == 'nt' else Path(path).as_posix()


def path_forms(path):
    """{suffix: spelling} of a path as agent files write it on this machine.

    '' is the forward-slash form; a Windows path also has '_NATIVE' (backslashes), '_ESCAPED' (doubled backslashes,
    as inside JSON or shell strings), '_ESCAPED2' (escaped twice: a rule string that is itself escaped, inside JSON),
    '_MSYS' (/d/...) and '_WSL' (/mnt/d/...).
    """
    text = str(path)
    forms = {'': text.replace('\\', '/') if re.match(r'^[A-Za-z]:[\\/]', text) else Path(path).as_posix()}
    match = re.match(r'^([A-Za-z]):[\\/](.*)$', text)
    if match:
        native = text.replace('/', '\\')
        rest = match.group(2).replace('\\', '/')
        forms.update({'_NATIVE': native, '_ESCAPED': native.replace('\\', '\\\\'),
                      '_ESCAPED2': native.replace('\\', '\\' * 4),
                      '_MSYS': f'/{match.group(1).lower()}/{rest}', '_WSL': f'/mnt/{match.group(1).lower()}/{rest}'})
    return forms


def collapse(data, places, ignore_case=False):
    """Replace each named place ({'CERID_ROOT': path, ...}) by {{NAME<suffix>}} placeholders in UTF-8 text."""
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        return data
    pairs = [(form, '{{' + name + suffix + '}}') for name, path in places.items()
             for suffix, form in path_forms(path).items() if len(form) > 3]
    for form, token in sorted(pairs, key=lambda item: -len(item[0])):
        text = re.sub(re.escape(form) + r'(?![A-Za-z0-9_-])', lambda _: token, text,
                      flags=re.IGNORECASE if ignore_case else 0)
    return text.encode('utf-8')


def expand(data, places):
    """Undo collapse() for this machine: every placeholder becomes the local spelling of its place."""
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        return data
    if '{{' not in text:
        return data
    for name, path in places.items():
        forms = path_forms(path)
        local = {'': forms['']}
        for suffix in ('_NATIVE', '_ESCAPED', '_ESCAPED2', '_MSYS', '_WSL'):
            local[suffix] = forms.get(suffix, forms[''])
        for suffix, form in local.items():
            text = text.replace('{{' + name + suffix + '}}', form)
    return text.encode('utf-8')


def collapse_root(data, root):
    """Replace this machine's project root with placeholders so stored agent state is machine-neutral."""
    return collapse(data, {'CERID_ROOT': root})


def expand_root(data, root):
    return expand(data, {'CERID_ROOT': root})


# ---------------------------------------------------------------------------------------------- shared settings

# Rule classes of a sharing export's settings (ADR-0136 §11). The owner's NAS sync never changes the file.
CREDENTIAL_RULE = re.compile(
    r'(?i)(?:\b(?:token|password|passwd|secret|api[_-]?key|authorization|bearer|cookie)\b\s*[=:]'
    r'|--(?:token|password|api-key|auth)\b|\b[A-Z0-9_]*(?:TOKEN|SECRET|PASSWORD|API_KEY)\s*='
    r'|\bAuthorization:|\bBearer\s+\S)')
ELEVATION_RULE = re.compile(r'(?i)(?:\bsudo\b|\brunas\b|-Verb\s+RunAs|Set-ExecutionPolicy|\breg(?:\.exe)?\s+'
                            r'(?:add|delete|import)\b|\bsetx\b|\bschtasks\b|\bRegister-ScheduledTask\b)')
PERSONAL_RULE = re.compile(
    r'(?i)(?:(?<![A-Za-z0-9])[A-Za-z]:[\\/]'                  # any drive path left after the project root
    r'|(?:^|[\s"\'(=])\\\\\\?\\?[A-Za-z0-9]'                   # UNC share (\\server, \\\\server escaped)
    r'|(?<![\w.:/])//?[A-Za-z](?:/|\*|\))'                   # /c/..., //c/... (Git Bash and Claude Code rules)
    r'|(?<![\w.:])/(?:mnt|home|Users|root|etc|usr|var|opt|tmp|private|Volumes)\b'
    r'|~[\\/]|\$HOME\b|\$\{HOME\}|%USERPROFILE%|%APPDATA%|%LOCALAPPDATA%|%TEMP%|%TMP%'
    r'|\$env:(?:USERPROFILE|APPDATA|LOCALAPPDATA|TEMP|TMP|HOMEPATH|HOMEDRIVE)\b)')
BROAD_ALLOW = re.compile(r'^(?:(?:Bash|PowerShell|Shell|Cmd)(?:\(\s*(?:\*|:\*)?\s*\))?'
                         r'|(?:Read|Edit|Write|MultiEdit|Glob|Grep|NotebookEdit)\((?://|/|~)\*{0,2}\))$')
CREDENTIAL_KEYS = {'apiKeyHelper', 'awsAuthRefresh', 'awsCredentialExport', 'otelHeadersHelper'}
SECRET_ENV_NAME = re.compile(r'(?i)token|secret|password|passwd|key|credential|auth|cookie')
SHARED_SETTINGS = re.compile(r'^\.claude/settings(?:\.local)?\.json$')
PERSONAL_FILES = ('CLAUDE.local.md',)


def classify_rule(text, places, allow=False):
    """The class of one rule, hook command or value: kept, rewritten (names the project root), credentials,
    elevation, broad (tool-wide grant) or personal (a location outside the project)."""
    if SECRET_RE.search(text) or CREDENTIAL_RULE.search(text):
        return 'credentials'
    if ELEVATION_RULE.search(text):
        return 'elevation'
    if allow and BROAD_ALLOW.match(text.strip()):
        return 'broad'
    rewritten = collapse(text.encode('utf-8'), places, ignore_case=True).decode('utf-8')
    if PERSONAL_RULE.search(rewritten.replace('{{', '\0').replace('}}', '\0')):
        return 'personal'
    return 'rewritten' if rewritten != text else 'kept'


def sanitize_settings(data, project_root):
    """A portable copy of a Claude Code settings file for sharing, and {class: count}.

    Kept: rules, hooks, MCP choices and other keys that stay inside the project. Rewritten: the same, with the owner's
    checkout root replaced by placeholders that a restore expands to the recipient's root. Dropped: credentials and
    token-bearing commands, elevation or system-wide changes, tool-wide grants (`Bash`, `Bash(*)`, `Read(//**)`), and
    anything naming a location outside the project (user profile, other drives, the NAS, UNC shares, system folders).
    """
    document = json.loads(data.decode('utf-8-sig'))
    places = {'CERID_ROOT': project_root}
    counts = {}

    def take(text, allow=False):
        kind = classify_rule(text, places, allow)
        counts[kind] = counts.get(kind, 0) + 1
        return text if kind in ('kept', 'rewritten') else None

    def scrub(value):
        """Any other JSON value: kept whole or dropped whole."""
        kind = classify_rule(json.dumps(value, ensure_ascii=False), places)
        counts[kind] = counts.get(kind, 0) + 1
        return value if kind in ('kept', 'rewritten') else None

    result = {}
    for key, value in document.items():
        if key in CREDENTIAL_KEYS:
            counts['credentials'] = counts.get('credentials', 0) + 1
        elif key == 'permissions' and isinstance(value, dict):
            permissions = {}
            for name, entries in value.items():
                if name in ('allow', 'ask', 'deny') and isinstance(entries, list):
                    permissions[name] = [r for r in (take(e, name == 'allow') for e in entries if isinstance(e, str))
                                         if r is not None]
                elif name == 'additionalDirectories' and isinstance(entries, list):
                    permissions[name] = [r for r in (take(e) for e in entries) if r is not None]
                else:
                    kept = scrub(entries)
                    if kept is not None:
                        permissions[name] = kept
            result[key] = permissions
        elif key == 'hooks' and isinstance(value, dict):
            hooks = {}
            for event, groups in value.items():
                kept_groups = []
                for group in groups if isinstance(groups, list) else []:
                    commands = [dict(h, command=c) for h in group.get('hooks', [])
                                for c in [take(h.get('command', ''))] if c is not None]
                    if commands:
                        kept_groups.append(dict(group, hooks=commands))
                if kept_groups:
                    hooks[event] = kept_groups
            result[key] = hooks
        elif key == 'env' and isinstance(value, dict):
            env = {}
            for name, text in value.items():
                if SECRET_ENV_NAME.search(name):
                    counts['credentials'] = counts.get('credentials', 0) + 1
                    continue
                kept = take(str(text))
                if kept is not None:
                    env[name] = kept
            result[key] = env
        else:
            kept = scrub(value)
            if kept is not None:
                result[key] = kept
    text = (json.dumps(result, indent=2, ensure_ascii=False) + '\n').encode('utf-8')
    return collapse(text, places, ignore_case=True), counts


def sanitize_workspace_config(data):
    """The overlay configuration without this machine's NAS folder (a recipient passes its own)."""
    document = json.loads(data.decode('utf-8'))
    document.pop('nas', None)
    return (json.dumps(document, indent=2) + '\n').encode('utf-8')


def filter_fast_export(stream, excluded):
    """A `git fast-export` stream without any change to the excluded paths (their blobs become unreachable)."""
    excluded = {p.encode('utf-8') for p in excluded}
    parts, index = [], 0
    while index < len(stream):
        end = stream.find(b'\n', index)
        end = len(stream) if end < 0 else end
        line = stream[index:end]
        index = end + 1
        if line.startswith(b'data '):
            size = int(line[5:])
            parts.append(line + b'\n' + stream[index:index + size])
            index += size
            continue
        if line.startswith(b'M ') and line.split(b' ', 3)[3] in excluded:
            continue
        if line.startswith(b'D ') and line[2:] in excluded:
            continue
        parts.append(line + b'\n')
    return b''.join(parts)


def sharing_overlay_bundle(ws, target, report):
    """The overlay for an export to share: history without the owner's settings, then one commit adding portable
    copies (sanitized settings, the configuration without the NAS folder); personal files left out."""
    tree = ws.overlay.tree()
    sanitized = {}
    for path in tree:
        data = ws.overlay.run('cat-file', 'blob', f'HEAD:{path}').stdout
        if SHARED_SETTINGS.match(path):
            sanitized[path], counts = sanitize_settings(data, ws.project_path)
            report.add('export', f'{path}: shared as a portable copy (' + ', '.join(
                f'{k} {v}' for k, v in sorted(counts.items())) + ')')
        elif path == CONFIG_FILE:
            sanitized[path] = sanitize_workspace_config(data)
    personal = [p for p in tree if p in PERSONAL_FILES]
    for path in personal:
        report.add('export', f'{path}: personal file left out (--include-local-settings keeps it)')
    stream = ws.overlay.run('fast-export', '--signed-tags=strip', '--tag-of-filtered-object=drop',
                            '--reencode=no', '--branches').stdout
    repo = Path(target).with_suffix('.git')
    git('init', '--quiet', '--bare', f'--initial-branch={OVERLAY_BRANCH}', posix(repo))
    git(f'--git-dir={posix(repo)}', 'fast-import', '--quiet',
        data=filter_fast_export(stream, set(sanitized) | set(personal)))
    head = out(f'--git-dir={posix(repo)}', 'rev-parse', f'refs/heads/{OVERLAY_BRANCH}').strip()
    message = b'chore(overlay): portable settings for sharing\n'
    commit = [b'commit refs/heads/' + OVERLAY_BRANCH.encode() + b'\n',
              b'committer Cerid workspace export <export@invalid> %d +0000\n' % int(datetime.datetime.now(
                  datetime.UTC).timestamp()),
              b'data %d\n' % len(message) + message, b'from ' + head.encode() + b'\n']
    for path, data in sorted(sanitized.items()):
        commit.append(b'M 100644 inline ' + path.encode('utf-8') + b'\ndata %d\n' % len(data) + data + b'\n')
    git(f'--git-dir={posix(repo)}', 'fast-import', '--quiet', data=b''.join(commit))
    return make_bundle([f'--git-dir={posix(repo)}'], target, '--branches'), sorted(sanitized), personal


# ---------------------------------------------------------------------------------------------- user-level files

def user_claude_names(config, claude_dir):
    """User-level Claude Code files of this project, relative to the Claude Code folder. Always synced.

    The defaults, the configuration's `user_claude_files`, every agent definition that names Cerid or crd, and
    every `crd-*` file at the top of the folder (logs excluded).
    """
    names = set(DEFAULT_USER_CLAUDE_FILES) | set(config.get('user_claude_files') or [])
    claude_dir = Path(claude_dir)
    agents = claude_dir / 'agents'
    if agents.is_dir():
        for path in agents.glob('*.md'):
            if re.search(rb'(?i)\b(?:cerid|crd)\b', path.read_bytes()):
                names.add(f'agents/{path.name}')
    if claude_dir.is_dir():
        for path in claude_dir.glob('crd-*'):
            if path.is_file() and not any(fnmatch.fnmatch(path.name, p) for p in LOCAL_STATE_FILES):
                names.add(path.name)
    return sorted(n for n in names if (claude_dir / n).is_file())


def user_places(project_root, claude_dir):
    return {'CERID_ROOT': project_root, 'CLAUDE_HOME': claude_dir}


def place_user_file(target, data, overwrite, conflicts):
    """Write a user-level file; a different existing file is backed up first (or kept, when not overwriting)."""
    target = Path(target)
    if target.is_file():
        if target.read_bytes() == data:
            return 0
        if not overwrite:
            return 0
        backup = target.with_name(target.name + '.before-workspace-' +
                                  datetime.datetime.now().strftime('%Y%m%d%H%M%S'))
        shutil.copy2(target, backup)
        conflicts.append(f'{target} differed; the previous version is kept as {backup.name}')
    return int(write_if_changed(target, data))


# ---------------------------------------------------------------------------------------------- git

def git(*args, cwd=None, data=None, check=True, env=None):
    result = subprocess.run(['git', *args], cwd=cwd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=env)
    if check and result.returncode != 0:
        message = result.stderr.decode('utf-8', errors='replace').strip()
        raise WorkspaceError(f'git {" ".join(str(a) for a in args[:4])} failed: {message}')
    return result


def out(*args, **kwargs):
    return git(*args, **kwargs).stdout.decode('utf-8', errors='replace')


def ls_remote(url):
    refs = {}
    for line in out('ls-remote', str(url)).splitlines():
        sha, _, ref = line.partition('\t')
        if ref.startswith(('refs/heads/', 'refs/tags/')) and not ref.endswith('^{}'):
            refs[ref] = sha
    return refs


def local_refs(root, patterns=('refs/heads', 'refs/tags')):
    refs = {}
    for line in out('for-each-ref', '--format=%(objectname) %(refname)', *patterns, cwd=root).splitlines():
        sha, _, ref = line.partition(' ')
        refs[ref] = sha
    return refs


def has_object(root, sha):
    return git('cat-file', '-e', sha, cwd=root, check=False).returncode == 0


def is_ancestor(root, old, new):
    return git('merge-base', '--is-ancestor', old, new, cwd=root, check=False).returncode == 0


def common_root(root):
    """The main checkout of a repository (Claude Code keys a worktree's memory by its main checkout)."""
    common = out('rev-parse', '--path-format=absolute', '--git-common-dir', cwd=root).strip()
    return Path(common).parent if Path(common).name == '.git' else Path(root)


def blob_id(data):
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


class Overlay:
    """The overlay repository: Git directory `<root>/.agent-overlay`, work tree `<root>`."""

    def __init__(self, root):
        self.root = Path(root)
        self.git_dir = self.root / OVERLAY_GIT_DIR

    def exists(self):
        return (self.git_dir / 'HEAD').is_file()

    def args(self):
        return ['--literal-pathspecs', f'--git-dir={posix(self.git_dir)}', f'--work-tree={posix(self.root)}']

    def run(self, *args, data=None, check=True):
        return git(*self.args(), *args, cwd=self.root, data=data, check=check)

    def text(self, *args, **kwargs):
        return self.run(*args, **kwargs).stdout.decode('utf-8', errors='replace')

    def init(self):
        git('init', '--quiet', '--bare', f'--initial-branch={OVERLAY_BRANCH}', posix(self.git_dir))
        self.configure()

    def configure(self):
        """Settings of an overlay repository; idempotent. Returns whether the stored-bytes rule had to change.

        The code repository's .gitattributes sits in the overlay's work tree and would convert line endings
        (`* text=auto`, `*.json eol=lf`, ...); info/attributes outranks it, so agent files are stored byte-exact.
        """
        for key, value in (('core.bare', 'false'), ('core.worktree', '..'), ('core.autocrlf', 'false'),
                           ('core.safecrlf', 'false'), ('status.showUntrackedFiles', 'no')):
            git(f'--git-dir={posix(self.git_dir)}', 'config', key, value)
        info = self.git_dir / 'info'
        info.mkdir(parents=True, exist_ok=True)
        write_if_changed(info / 'exclude', OVERLAY_EXCLUDE.encode())
        return write_if_changed(info / 'attributes', OVERLAY_ATTRIBUTES.encode())

    def head(self):
        result = self.run('rev-parse', '--verify', '--quiet', 'HEAD^{commit}', check=False)
        return result.stdout.decode().strip() or None

    def tree(self, rev='HEAD'):
        if rev == 'HEAD' and not self.head():
            return {}
        files = {}
        for entry in self.text('ls-tree', '-r', '-z', rev).split('\0'):
            if entry:
                meta, _, path = entry.partition('\t')
                files[path] = meta.split()[2]
        return files

    def merging(self):
        return (self.git_dir / 'MERGE_HEAD').is_file()

    def unmerged(self):
        return sorted({p for p in self.text('diff', '--name-only', '-z', '--diff-filter=U').split('\0') if p})

    def is_ancestor(self, old, new):
        return self.run('merge-base', '--is-ancestor', old, new, check=False).returncode == 0

    def has_commit(self, sha):
        return self.run('cat-file', '-e', sha + '^{commit}', check=False).returncode == 0



# ---------------------------------------------------------------------------------------------- refusal checks

def credential_name(name):
    return any(fnmatch.fnmatch(name.lower(), pattern) for pattern in CREDENTIAL_NAMES)


def secret_findings(data):
    """[(line, pattern name)] for every high-confidence secret shape; the matched text is never returned."""
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        text = data.decode('latin-1')
    findings = []
    for match in SECRET_RE.finditer(text):
        name = SECRET_NAMES[int(match.lastgroup[1:])]
        findings.append((text.count('\n', 0, match.start()) + 1, name))
    return findings


def refusals(files):
    """Reasons a {relative path: bytes} set may not be stored: transcripts, credential files, secret shapes."""
    problems = []
    for path, data in sorted(files.items()):
        name = PurePosixPath(path).name
        if name.lower().endswith(TRANSCRIPT_SUFFIXES):
            problems.append(f'{path}: session transcript (*.jsonl) is never stored')
        elif credential_name(name):
            problems.append(f'{path}: credential file is never stored')
        else:
            for line, pattern in secret_findings(data):
                problems.append(f'{path}:{line}: looks like a {pattern}; remove it before syncing')
    return problems


# ---------------------------------------------------------------------------------------------- configuration

def load_config(root):
    path = Path(root) / CONFIG_FILE
    config = {'schema': SCHEMA, 'nas': None, 'code_remotes': None, 'code_remote_exclude': ['worktree-*'],
              'code_remote_exclude_tags': [], 'default_branch': 'main'}
    if path.is_file():
        stored = json.loads(path.read_text(encoding='utf-8'))
        if stored.get('schema') != SCHEMA:
            raise WorkspaceError(f'{CONFIG_FILE}: expected schema {SCHEMA}')
        config.update(stored)
    return config


def resolve_nas(option, config):
    value = option or os.environ.get('CERID_WORKSPACE_NAS') or config.get('nas')
    if not value:
        raise WorkspaceError('No NAS folder: pass --nas, set CERID_WORKSPACE_NAS or "nas" in ' + CONFIG_FILE)
    return Path(value)


class Workspace:
    def __init__(self, root, nas=None, claude_dir=None, downloads_dir=None, memory_dir=None):
        self.root = Path(root).resolve()
        if not (self.root / '.git').exists():
            raise WorkspaceError(f'{self.root} is not a Git checkout')
        self.config = load_config(self.root)
        self.nas_option = nas
        self.claude_dir = claude_config_dir(claude_dir)
        self.downloads = Path(downloads_dir) if downloads_dir else default_downloads_dir()
        self.project_path = common_root(self.root)
        self.memory_dir = Path(memory_dir) if memory_dir else claude_project_dir(self.project_path, self.claude_dir) / 'memory'
        self.overlay = Overlay(self.root)

    @property
    def nas(self):
        return resolve_nas(self.nas_option, self.config)

    # ---- the overlay's desired content

    def in_place_files(self):
        files = {}
        for name in OVERLAY_ROOT_FILES:
            path = self.root / name
            if path.is_file():
                files[name] = path.read_bytes()
        for top in OVERLAY_ROOT_DIRS:
            base = self.root / top
            for directory, subdirs, names in os.walk(base):
                subdirs[:] = sorted(d for d in subdirs if d not in LOCAL_STATE_DIRS and not os.path.islink(
                    os.path.join(directory, d)))
                for name in sorted(names):
                    if any(fnmatch.fnmatch(name, pattern) for pattern in LOCAL_STATE_FILES):
                        continue
                    path = Path(directory) / name
                    if path.is_file() and not path.is_symlink():
                        files[path.relative_to(self.root).as_posix()] = path.read_bytes()
        config = self.root / CONFIG_FILE
        if config.is_file():
            files[CONFIG_FILE] = config.read_bytes()
        return files

    def copied_files(self, from_work_tree=False):
        """`.agent/` copies of state outside the checkout: Claude memory and every session's workflow scripts.

        While a merge with the NAS overlay is unfinished the work tree's `.agent/` copies are the truth (they hold
        the resolution); otherwise the live Claude Code folders are.
        """
        files = {}
        if from_work_tree:
            for managed in (MEMORY_DIR, WORKFLOWS_DIR, USER_CLAUDE_DIR):
                base = self.root / managed
                if base.is_dir():
                    for path in sorted(base.rglob('*')):
                        if path.is_file():
                            files[path.relative_to(self.root).as_posix()] = path.read_bytes()
            return files
        if self.memory_dir.is_dir():
            for path in sorted(self.memory_dir.rglob('*')):
                if path.is_file() and not path.is_symlink():
                    rel = path.relative_to(self.memory_dir).as_posix()
                    files[f'{MEMORY_DIR}/{rel}'] = collapse_root(path.read_bytes(), self.project_path)
        project = self.memory_dir.parent
        if project.is_dir():
            for session in sorted(p for p in project.iterdir() if p.is_dir() and p.name != 'memory'):
                scripts = session / 'workflows' / 'scripts'
                if scripts.is_dir():
                    for path in sorted(scripts.rglob('*')):
                        if path.is_file() and not path.is_symlink():
                            rel = path.relative_to(scripts).as_posix()
                            files[f'{WORKFLOWS_DIR}/{session.name}/{rel}'] = collapse_root(path.read_bytes(),
                                                                                         self.project_path)
        places = user_places(self.project_path, self.claude_dir)
        for name in user_claude_names(self.config, self.claude_dir):
            files[f'{USER_CLAUDE_DIR}/{name}'] = collapse((self.claude_dir / name).read_bytes(), places)
        return files

    def desired(self):
        files = self.in_place_files()
        files.update(self.copied_files())
        return files


def workflow_target(project_dir, rel):
    session, _, rest = rel.partition('/')
    return Path(project_dir) / session / 'workflows' / 'scripts' / rest


def write_if_changed(path, data):
    path = Path(path)
    if path.is_file() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.workspace-partial')
    temporary.write_bytes(data)
    os.replace(temporary, path)
    return True


def mirror_agent_home(root, copies):
    """Make `.agent/claude-memory` and `.agent/workflows` exactly `copies`; returns (written, removed)."""
    written = removed = 0
    for rel, data in copies.items():
        written += write_if_changed(Path(root) / rel, data)
    for managed in (MEMORY_DIR, WORKFLOWS_DIR, USER_CLAUDE_DIR):
        base = Path(root) / managed
        if base.is_dir():
            for path in sorted(base.rglob('*'), reverse=True):
                rel = path.relative_to(root).as_posix()
                if path.is_file() and rel not in copies:
                    path.unlink()
                    removed += 1
                elif path.is_dir() and not any(path.iterdir()):
                    path.rmdir()
    return written, removed


def write_back(root, memory_dir, project_root, replace=True, overwrite=True, claude_dir=None, conflicts=None):
    """Copy the overlay's `.agent/` state to this machine's Claude Code folders.

    replace: memory is mirrored exactly (files the overlay lacks are removed). overwrite=False only adds files this
    machine does not have yet (adopting the NAS overlay in a new checkout never discards a live note).
    User-level files go to claude_dir and are never deleted; a different existing one is backed up before it is
    replaced, and listed in conflicts.
    """
    root = Path(root)
    memory_dir = Path(memory_dir)
    written = removed = 0
    source = root / MEMORY_DIR
    wanted = {}
    if source.is_dir():
        for path in source.rglob('*'):
            if path.is_file():
                wanted[path.relative_to(source).as_posix()] = expand_root(path.read_bytes(), project_root)
    for rel, data in wanted.items():
        if overwrite or not (memory_dir / rel).exists():
            written += write_if_changed(memory_dir / rel, data)
    if replace and memory_dir.is_dir():
        for path in sorted(memory_dir.rglob('*'), reverse=True):
            rel = path.relative_to(memory_dir).as_posix()
            if path.is_file() and rel not in wanted:
                path.unlink()
                removed += 1
    workflows = root / WORKFLOWS_DIR
    if workflows.is_dir():
        for path in workflows.rglob('*'):
            if path.is_file():
                rel = path.relative_to(workflows).as_posix()
                target = workflow_target(memory_dir.parent, rel)
                if overwrite or not target.exists():
                    written += write_if_changed(target, expand_root(path.read_bytes(), project_root))
    user = root / USER_CLAUDE_DIR
    if claude_dir is not None and user.is_dir():
        places = user_places(project_root, claude_dir)
        for path in sorted(user.rglob('*')):
            if path.is_file():
                written += place_user_file(Path(claude_dir) / path.relative_to(user),
                                           expand(path.read_bytes(), places), overwrite,
                                           conflicts if conflicts is not None else [])
    return written, removed


# ---------------------------------------------------------------------------------------------- pinned downloads

def pinned_index(pins_path):
    index = {}
    if not Path(pins_path).is_file():
        return index
    document = json.loads(Path(pins_path).read_text(encoding='utf-8'))
    for section, entries in document.items():
        if isinstance(entries, dict):
            for name, entry in entries.items():
                if isinstance(entry, dict) and isinstance(entry.get('sha256'), str):
                    index[entry['sha256'].lower()] = (section, name, entry.get('file') or name)
    return index


def file_sha256(path):
    with open(path, 'rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def local_downloads(downloads, pins_path):
    """({sha: entry} for pinned files under the downloads folder, [unpinned relative paths])."""
    index = pinned_index(pins_path)
    found, unpinned = {}, []
    if Path(downloads).is_dir():
        for path in sorted(Path(downloads).rglob('*')):
            if not path.is_file() or path.name.endswith('.partial'):
                continue
            rel = path.relative_to(downloads).as_posix()
            sha = file_sha256(path)
            if sha in index:
                section, name, file_name = index[sha]
                entry = found.setdefault(sha, {'kind': 'tool', 'section': section, 'name': name, 'file': file_name,
                                               'sha256': sha, 'size': path.stat().st_size, 'sources': [],
                                               'path': path})
                entry['sources'].append(rel)
            else:
                unpinned.append(rel)
    return found, unpinned


def uncached_sha256(path):
    """SHA-256 of a file read past this machine's file cache, so a NAS copy is checked as the NAS holds it.

    Windows opens it with FILE_FLAG_NO_BUFFERING, which makes the SMB client fetch from the server instead of its
    cache (page-aligned 1 MiB reads; the last read may return less). POSIX drops the cached pages first
    (posix_fadvise DONTNEED; a CIFS mount with cache=strict or none honours it).
    """
    hasher = hashlib.sha256()
    if os.name != 'nt':
        descriptor = os.open(path, os.O_RDONLY)
        try:
            if hasattr(os, 'posix_fadvise'):
                os.posix_fadvise(descriptor, 0, 0, os.POSIX_FADV_DONTNEED)
            while chunk := os.read(descriptor, 1 << 20):
                hasher.update(chunk)
        finally:
            os.close(descriptor)
        return hasher.hexdigest()
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                ctypes.c_void_p]
    kernel.VirtualAlloc.restype = ctypes.c_void_p
    kernel.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
    kernel.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateFileW(str(path), 0x80000000, 0x1, None, 3, 0x20000000 | 0x08000000, None)
    if handle in (None, wintypes.HANDLE(-1).value):
        raise WorkspaceError(f'cannot open {path} unbuffered (Windows error {ctypes.get_last_error()})')
    size = 1 << 20
    buffer = kernel.VirtualAlloc(None, size, 0x3000, 0x04)
    try:
        count = wintypes.DWORD()
        while True:
            if not kernel.ReadFile(handle, buffer, size, ctypes.byref(count), None):
                raise WorkspaceError(f'unbuffered read of {path} failed (Windows error {ctypes.get_last_error()})')
            if not count.value:
                break
            hasher.update(ctypes.string_at(buffer, count.value))
    finally:
        kernel.VirtualFree(buffer, 0, 0x8000)
        kernel.CloseHandle(handle)
    return hasher.hexdigest()


def copy_verified(source, destination, sha):
    """Local copy through a .partial file, renamed only after its SHA-256 matches."""
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + '.partial')
    shutil.copyfile(source, temporary)
    if file_sha256(temporary) != sha:
        temporary.unlink()
        raise WorkspaceError(f'Copy of {source} does not match its SHA-256')
    os.replace(temporary, destination)


def size_text(size):
    for unit, scale in (('TiB', 1 << 40), ('GiB', 1 << 30), ('MiB', 1 << 20)):
        if size >= scale:
            return f'{size / scale:.1f} {unit} ({size:,} bytes)'
    return f'{size:,} bytes'


# ---------------------------------------------------------------------------------------------- the NAS folder

def pid_alive(pid):
    """Whether a process exists on this machine (never signals it: os.kill would terminate it on Windows)."""
    if os.name == 'nt':
        import ctypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        handle = kernel.OpenProcess(0x1000, False, int(pid))
        if not handle:
            return ctypes.get_last_error() == 5
        code = ctypes.c_ulong()
        kernel.GetExitCodeProcess(handle, ctypes.byref(code))
        kernel.CloseHandle(handle)
        return code.value == 259
    try:
        os.kill(int(pid), 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


class Nas:
    """The operator's NAS folder, written only as a few large single files.

    Layout: MANIFEST.json; code/code-<UTC>.bundle and overlay/overlay-<UTC>.bundle (full Git bundles, the newest
    `generations` of each kept); tools/<pinned archive>; exports/crd-workspace-YYYYMMDD.zip; crd.lock while a sync
    or export runs. Every path is confined to the folder after resolving links and junctions; every file is written
    to `<name>.partial`, flushed, read back from the NAS and compared by SHA-256 (bundles also pass `git bundle
    verify`) and only then renamed; an existing file is never overwritten. Only files recorded in MANIFEST.json
    are ever pruned. MANIFEST.json itself is replaced atomically after the same read-back check.
    """

    def __init__(self, root):
        self.root = Path(root)
        if not self.root.is_dir():
            raise WorkspaceError(f'NAS folder {self.root} does not exist or is not reachable; create it first '
                                 f'(this tool never creates or touches anything outside it)')
        self.real = Path(os.path.realpath(self.root))

    def path(self, rel):
        target = self.root / rel
        real = Path(os.path.realpath(target))
        if real == self.real or not real.is_relative_to(self.real):
            raise WorkspaceError(f'{rel} resolves outside the NAS folder {self.root}; refusing')
        return target

    # ---- reading (restore and status only read)

    def manifest(self):
        path = self.path(MANIFEST_NAME)
        if not path.is_file():
            return {'schema': NAS_SCHEMA, 'source': {}, 'files': {}}
        document = json.loads(path.read_text(encoding='utf-8'))
        if document.get('schema') != NAS_SCHEMA:
            raise WorkspaceError(f'{path}: expected schema {NAS_SCHEMA}')
        return document

    def latest(self, manifest, kind):
        entries = sorted((e['created'], rel) for rel, e in manifest['files'].items() if e['kind'] == kind)
        return (entries[-1][1], manifest['files'][entries[-1][1]]) if entries else (None, None)

    def verified(self, rel, entry):
        path = self.path(rel)
        if not path.is_file() or path.stat().st_size != entry['size'] or uncached_sha256(path) != entry['sha256']:
            raise WorkspaceError(f'{rel} on the NAS does not match MANIFEST.json (size or SHA-256)')
        return path

    def lock_holder(self):
        path = self.path(LOCK_NAME)
        if not path.is_file():
            return None
        try:
            return json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return {'host': '?', 'pid': 0, 'started': '?'}

    # ---- writing (sync and export, always under the lock)

    def lock(self, report, break_lock=False):
        path = self.path(LOCK_NAME)
        me = {'host': platform.node(), 'pid': os.getpid(),
              'started': datetime.datetime.now(datetime.UTC).isoformat(timespec='seconds'), 'tool': SCHEMA}
        for _ in range(2):
            try:
                handle = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, 'O_BINARY', 0))
            except FileExistsError:
                holder = self.lock_holder() or {}
                stale = holder.get('host') == me['host'] and not pid_alive(holder.get('pid', 0))
                if stale or break_lock:
                    path.unlink()
                    report.add('nas', f'removed a {"stale" if stale else "broken"} lock of {holder.get("host")} '
                                      f'pid {holder.get("pid")} from {holder.get("started")}')
                    continue
                raise WorkspaceError(f'The NAS folder is locked by {holder.get("host")} (pid {holder.get("pid")}, '
                                     f'since {holder.get("started")}); wait for that sync to finish. If that machine '
                                     f'is not syncing, run again with --break-lock.')
            with os.fdopen(handle, 'w', encoding='utf-8') as stream:
                json.dump(me, stream)
                stream.flush()
                os.fsync(stream.fileno())
            return NasLock(self, me)
        raise WorkspaceError('could not take the NAS lock')

    def write_file(self, rel, source, sha, bundle_repo=None):
        """Copy a local file to the NAS: .partial, flush, read back, compare, verify, rename. Never overwrites."""
        target = self.path(rel)
        partial = self.path(rel + '.partial')
        if target.exists():
            raise WorkspaceError(f'{rel} already exists on the NAS; existing files are never overwritten')
        target.parent.mkdir(exist_ok=True)
        if partial.exists():
            partial.unlink()  # a .partial of this tool, left by an interrupted run while it held the lock
        try:
            with open(source, 'rb') as reader, open(partial, 'xb') as writer:
                shutil.copyfileobj(reader, writer, 8 << 20)
                writer.flush()
                os.fsync(writer.fileno())
            if uncached_sha256(partial) != sha:
                raise WorkspaceError(f'{rel}: the copy read back from the NAS does not match its SHA-256')
            if bundle_repo is not None:
                check = git('bundle', 'verify', '--quiet', posix(partial), cwd=bundle_repo, check=False)
                if check.returncode:
                    raise WorkspaceError(f'{rel}: git bundle verify failed on the NAS copy: '
                                         + check.stderr.decode('utf-8', errors='replace').strip())
        except BaseException:
            if partial.exists():
                partial.unlink()
            raise
        os.rename(partial, target)

    def write_manifest(self, document):
        data = (json.dumps(document, indent=2, sort_keys=True) + '\n').encode('utf-8')
        target = self.path(MANIFEST_NAME)
        partial = self.path(MANIFEST_NAME + '.partial')
        try:
            with open(partial, 'wb') as writer:
                writer.write(data)
                writer.flush()
                os.fsync(writer.fileno())
            if uncached_sha256(partial) != hashlib.sha256(data).hexdigest():
                raise WorkspaceError('MANIFEST.json read back from the NAS differs from what was written')
        except BaseException:
            if partial.exists():
                partial.unlink()
            raise
        os.replace(partial, target)

    def prune_plan(self, manifest, kind, generations):
        entries = sorted((e['created'], rel) for rel, e in manifest['files'].items() if e['kind'] == kind)
        return [rel for _, rel in entries[:-generations]] if len(entries) > generations else []

    def prune(self, manifest, generations, report):
        """Drop the oldest recorded generations: the manifest first, then exactly those recorded files."""
        doomed = [rel for kind in ('code', 'overlay', 'export') for rel in self.prune_plan(manifest, kind,
                                                                                            generations)]
        if not doomed:
            return
        for rel in doomed:
            self.path(rel)
            del manifest['files'][rel]
        self.write_manifest(manifest)
        for rel in doomed:
            path = self.path(rel)
            if path.is_file():
                path.unlink()
            report.add('nas', f'pruned {rel} (older than the newest {generations} generations)')


class NasLock:
    def __init__(self, nas, me):
        self.nas, self.me = nas, me

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.nas.lock_holder() == self.me:
            self.nas.path(LOCK_NAME).unlink()


def stamp(now=None):
    """File-name time stamp (seconds; unique_name adds -2, -3 within one second)."""
    return (now or datetime.datetime.now(datetime.UTC)).strftime('%Y%m%dT%H%M%SZ')


def created_now():
    """Manifest order key: microseconds, so generations written within one second still sort by age."""
    return datetime.datetime.now(datetime.UTC).isoformat(timespec='microseconds')


def unique_name(nas, manifest, directory, stem, suffix):
    rel, counter = f'{directory}/{stem}{suffix}', 2
    while rel in manifest['files'] or nas.path(rel).exists():
        rel, counter = f'{directory}/{stem}-{counter}{suffix}', counter + 1
    return rel


def bundle_heads(path):
    refs = {}
    for line in out('bundle', 'list-heads', posix(path)).splitlines():
        sha, _, ref = line.partition(' ')
        if ref.startswith(('refs/heads/', 'refs/tags/')):
            refs[ref] = sha
    return refs


def make_bundle(repo_args, path, *selection):
    git(*repo_args, 'bundle', 'create', '--quiet', posix(path), *selection)
    return {'size': Path(path).stat().st_size, 'sha256': file_sha256(path), 'refs': bundle_heads(path)}


def check_free_space(nas, planned, report):
    free = shutil.disk_usage(nas.root).free
    if planned and free < 2 * planned:
        raise WorkspaceError(f'The NAS share has {size_text(free)} free; this run writes {size_text(planned)} and '
                             f'needs twice that free. Nothing was written.')
    report.add('nas', f'{size_text(free)} free; this run writes {size_text(planned)}')


# ---------------------------------------------------------------------------------------------- code repository

def code_head_branch(ws):
    refs = local_refs(ws.root, ('refs/heads',))
    default = ws.config.get('default_branch') or 'main'
    if f'refs/heads/{default}' in refs:
        return default
    return out('rev-parse', '--abbrev-ref', 'HEAD', cwd=ws.root).strip()


def strip_userinfo(url):
    return re.sub(r'^([a-z][a-z0-9+.\-]*://)[^/@]+@', r'\1', url)


def code_remotes(ws):
    names = out('remote', cwd=ws.root).split()
    wanted = ws.config.get('code_remotes')
    selected = names if wanted is None else [n for n in wanted if n in names]
    remotes = {}
    for name in selected:
        url = out('remote', 'get-url', '--push', name, cwd=ws.root).strip()
        if strip_userinfo(url) != url:
            raise WorkspaceError(f'Remote {name} embeds credentials in its URL; use a credential helper instead')
        remotes[name] = url
    missing = [] if wanted is None else [n for n in wanted if n not in names]
    return remotes, missing


def ref_plan(root, nas_refs, refs):
    """(changes, rejections) of local refs against the newest NAS code bundle.

    A ref may move forward, or be rewritten when its old NAS tip is known here (a rebase made on this machine).
    A NAS tip this checkout has never seen came from another machine: the new bundle would drop it, so it is
    reported and no code bundle is written until it is fetched. Refs deleted here stay in older generations.
    """
    changes, rejected = [], []
    for ref, sha in sorted(refs.items()):
        old = nas_refs.get(ref)
        if old == sha:
            continue
        if old is None:
            changes.append((ref, 'new'))
        elif is_ancestor(root, old, sha):
            changes.append((ref, 'fast-forward'))
        elif has_object(root, old):
            changes.append((ref, 'rewritten here (the old NAS tip is known locally)'))
        else:
            rejected.append(f'{ref}: the NAS holds commits this checkout has never seen; fetch them first')
    for ref, old in sorted(nas_refs.items()):
        if ref not in refs:
            if has_object(root, old):
                changes.append((ref, 'deleted here (kept in older NAS generations until they are pruned)'))
            else:
                rejected.append(f'{ref}: only the NAS has this ref, from another machine; fetch it first')
    return changes, rejected


def remote_branch_checks(root, ref, remote):
    """Reasons a branch's commits that the remote lacks may not be pushed to a code host."""
    reasons = []
    pending = [ref, '--not', f'--remotes={remote}']
    touched = out('log', '--format=', '--name-only', '--diff-filter=d', *pending, '--', *AGENT_PATHS, cwd=root)
    touched = sorted({line for line in touched.splitlines() if line})
    if touched:
        reasons.append('new commits touch agent files (' + ', '.join(touched[:5]) + ')')
    transcripts = out('log', '--format=', '--name-only', '--diff-filter=AM', *pending, '--', '*.jsonl', cwd=root)
    if transcripts.strip():
        reasons.append('new commits add session transcripts (*.jsonl)')
    patch = git('log', '-p', '--no-color', '--no-ext-diff', '--format=commit %H', *pending, cwd=root).stdout
    added = b'\n'.join(line[1:] for line in patch.splitlines() if line.startswith(b'+') and not line.startswith(b'+++'))
    found = sorted({name for _, name in secret_findings(added)})
    if found:
        reasons.append('new commits add text shaped like: ' + ', '.join(found))
    return reasons


def remote_excluded(ws, ref):
    """Whether a sync keeps this branch or tag off the code hosts (the NAS code bundle still carries it)."""
    if ref.startswith('refs/heads/'):
        name, patterns = ref[len('refs/heads/'):], ws.config.get('code_remote_exclude') or []
    elif ref.startswith('refs/tags/'):
        name, patterns = ref[len('refs/tags/'):], ws.config.get('code_remote_exclude_tags') or []
    else:
        return False
    return any(fnmatch.fnmatch(name, pattern) for pattern in patterns)


def push_code_remote(ws, remote, report):
    git('fetch', '--quiet', remote, cwd=ws.root)
    spec = []
    for ref in sorted(local_refs(ws.root, ('refs/heads',))):
        branch = ref[len('refs/heads/'):]
        if remote_excluded(ws, ref):
            continue
        reasons = remote_branch_checks(ws.root, ref, remote)
        if reasons:
            report.error(f'code->{remote}', f'{branch} not pushed: ' + '; '.join(reasons))
        else:
            spec.append(f'{ref}:{ref}')
    spec += [f'{ref}:{ref}' for ref in sorted(local_refs(ws.root, ('refs/tags',))) if not remote_excluded(ws, ref)]
    if not spec:
        # A bare 'git push' would push the current branch (push.default), refused or not.
        report.add(f'code->{remote}', 'nothing to push')
        return
    result = git('push', '--porcelain', remote, *spec, cwd=ws.root, check=False)
    lines = result.stdout.decode('utf-8', errors='replace').splitlines()
    for line in lines:
        flag, _, rest = line.partition('\t')
        if flag == '!':
            report.error(f'code->{remote}', 'rejected ' + rest)
        elif flag in ('+', '*', '-'):
            report.add(f'code->{remote}', 'pushed ' + rest)
    if result.returncode != 0 and not any(line.startswith('!') for line in lines):
        report.error(f'code->{remote}', result.stderr.decode('utf-8', errors='replace').strip())
    elif not any(line.startswith(('+', '*', '-')) for line in lines):
        report.add(f'code->{remote}', 'up to date')


def source_info(ws, remotes):
    return {'project_path': str(ws.project_path), 'head_branch': code_head_branch(ws),
            'code_remotes': {name: strip_userinfo(url) for name, url in remotes.items()}}


# ---------------------------------------------------------------------------------------------- sync

def overlay_plan(ws, from_work_tree=False):
    """(desired files, copies, problems) with every refusal check applied first."""
    in_place = ws.in_place_files()
    copies = ws.copied_files(from_work_tree)
    desired = dict(in_place)
    desired.update(copies)
    return desired, copies, refusals(desired)


def describe_changes(old_tree, desired):
    added = [p for p in desired if p not in old_tree]
    changed = [p for p in desired if p in old_tree and old_tree[p] != blob_id(desired[p])]
    removed = [p for p in old_tree if p not in desired]
    return added, changed, removed


def stage_overlay(ws, desired):
    overlay = ws.overlay
    tracked = set(p for p in overlay.text('ls-files', '-z').split('\0') if p)
    stale = sorted(tracked - set(desired))
    if stale:
        overlay.run('rm', '--cached', '--quiet', '--ignore-unmatch', '--pathspec-from-file=-', '--pathspec-file-nul',
                    data=('\0'.join(stale) + '\0').encode())
    if desired:
        overlay.run('add', '--force', '--pathspec-from-file=-', '--pathspec-file-nul',
                    data=('\0'.join(sorted(desired)) + '\0').encode())
    return overlay.run('diff', '--cached', '--quiet', check=False).returncode != 0


def generations(ws):
    value = int(ws.config.get('generations') or DEFAULT_GENERATIONS)
    if value < 1:
        raise WorkspaceError('"generations" must be at least 1')
    return value


def sync(ws, dry_run=False, push_code_remotes=True, break_lock=False, report=None):
    report = report or Report()
    nas = Nas(ws.nas)
    merging = ws.overlay.exists() and ws.overlay.merging()
    desired, copies, problems = overlay_plan(ws, from_work_tree=merging)
    if merging:
        for path in ws.overlay.unmerged():
            data = desired.get(path, b'')
            if re.search(rb'^(?:<<<<<<<|>>>>>>>)( |$)', data, flags=re.M):
                report.error('overlay', f'{path} still has conflict markers; keep the wanted lines, delete the '
                                        f'markers, then run sync again')
        if report.errors:
            return report
    if problems:
        for problem in problems:
            report.error('refused', problem)
        report.add('refused', 'Nothing was written. Remove or relocate the files above, then run sync again.')
        return report
    remotes, missing = code_remotes(ws) if push_code_remotes else ({}, [])
    for name in missing:
        report.error('code', f'configured code remote {name!r} does not exist in this checkout')
    if dry_run:
        return sync_preview(ws, nas, desired, remotes, report)
    with tempfile.TemporaryDirectory(prefix='crd-sync-') as scratch, nas.lock(report, break_lock):
        scratch = Path(scratch)
        manifest = nas.manifest()
        writes = []  # (rel, local path, entry, bundle repo or None)
        now = stamp()

        # 1. Overlay: mirror the outside state into .agent/, commit, merge the newest NAS overlay, write back.
        overlay = ws.overlay
        if overlay.exists() and overlay.configure() and overlay.head():
            overlay.run('add', '--renormalize', '--force', '--', '.')
            report.add('overlay', 'agent files are now stored byte-exact (line-ending conversion turned off)')
        created = not overlay.exists()
        if created:
            overlay.init()
        latest_rel, latest = nas.latest(manifest, 'overlay')
        remote_tip = latest['refs'].get(f'refs/heads/{OVERLAY_BRANCH}') if latest else None
        if remote_tip and not overlay.has_commit(remote_tip):
            overlay.run('fetch', '--quiet', posix(nas.verified(latest_rel, latest)),
                        f'+refs/heads/{OVERLAY_BRANCH}:refs/remotes/nas/{OVERLAY_BRANCH}')
        elif remote_tip:
            overlay.run('update-ref', f'refs/remotes/nas/{OVERLAY_BRANCH}', remote_tip)
        if created and remote_tip and not overlay.head():
            # A checkout that was not restored: take the NAS history, add only the files missing here (live agent
            # files and notes win), give Claude Code the NAS-only notes, then list the files again.
            overlay.run('update-ref', f'refs/heads/{OVERLAY_BRANCH}', remote_tip)
            overlay.run('read-tree', remote_tip)
            overlay.run('checkout-index', '--all', '--quiet', check=False)
            added, _ = write_back(ws.root, ws.memory_dir, ws.project_path, replace=False, overwrite=False,
                                  claude_dir=ws.claude_dir)
            report.add('overlay', f'adopted the NAS overlay history for this new checkout ({added} NAS-only files '
                                  f'added to the Claude Code folders; files already here were kept)')
            ws.config = load_config(ws.root)  # the adopted configuration decides this run too
            desired, copies, problems = overlay_plan(ws)
            if problems:
                for problem in problems:
                    report.error('refused', problem)
                return report
        if merging:
            stage_overlay(ws, desired)
            overlay.run('commit', '--quiet', '--no-verify', '--no-edit')
            conflicts = []
            written, removed = write_back(ws.root, ws.memory_dir, ws.project_path, claude_dir=ws.claude_dir,
                                          conflicts=conflicts)
            report.add('overlay', f'finished the merge with the NAS overlay; wrote {written} and removed {removed} '
                                  f'files in the Claude Code folders')
            for conflict in conflicts:
                report.add('overlay', conflict)
        else:
            written, removed = mirror_agent_home(ws.root, copies)
            report.add('overlay', f'{len(copies)} copied agent-state files ({written} updated, {removed} removed) '
                                  f'in .agent/')
        if not merging and stage_overlay(ws, desired):
            overlay.run('commit', '--quiet', '--no-verify', '-m',
                        f'chore(overlay): sync from {platform.node() or "host"}')
            report.add('overlay', f'committed {len(desired)} tracked files as {overlay.head()[:12]}')
        elif not merging:
            report.add('overlay', f'no changes ({len(desired)} tracked files)')
        head = overlay.head()
        if remote_tip and head and remote_tip != head and not overlay.is_ancestor(remote_tip, head):
            merged = overlay.run('merge', '--quiet', '--no-edit', f'refs/remotes/nas/{OVERLAY_BRANCH}', check=False)
            if merged.returncode != 0:
                conflicts = overlay.unmerged() if overlay.merging() else []
                if not conflicts:
                    overlay.run('merge', '--abort', check=False)
                    report.error('overlay', 'merging the NAS overlay failed: ' +
                                 merged.stderr.decode('utf-8', errors='replace').strip())
                    return report
                for path in conflicts:
                    report.error('overlay', f'{path} changed here and on the NAS: edit it (keep the wanted lines, '
                                            f'delete the <<<<<<< ======= >>>>>>> markers), then run sync again')
                report.add('overlay', 'nothing was written to the NAS; the merge stays open until those files are '
                                      'resolved')
                return report
            conflicts = []
            written, removed = write_back(ws.root, ws.memory_dir, ws.project_path, claude_dir=ws.claude_dir,
                                          conflicts=conflicts)
            report.add('overlay', f'merged the NAS overlay; wrote {written} and removed {removed} files in the '
                                  f'Claude Code folders')
            for conflict in conflicts:
                report.add('overlay', conflict)
            head = overlay.head()
        if head and head != remote_tip:
            local = scratch / 'overlay.bundle'
            entry = make_bundle(overlay.args(), local, '--branches')
            writes.append((unique_name(nas, manifest, 'overlay', f'overlay-{now}', '.bundle'), local,
                           dict(entry, kind='overlay'), overlay.git_dir))
        else:
            report.add('overlay', 'the newest NAS overlay bundle already has this overlay')

        # 2. Code repository: one full bundle of every branch and tag when they changed.
        refs = local_refs(ws.root)
        code_rel, code_latest = nas.latest(manifest, 'code')
        changes, rejected = ref_plan(ws.root, code_latest['refs'] if code_latest else {}, refs)
        for reason in rejected:
            report.error('code->nas', reason)
        if rejected:
            report.add('code->nas', 'no code bundle written until the refs above are fetched')
        elif changes or not code_latest:
            local = scratch / 'code.bundle'
            entry = make_bundle(['-C', posix(ws.root)], local, '--branches', '--tags')
            writes.append((unique_name(nas, manifest, 'code', f'code-{now}', '.bundle'), local,
                           dict(entry, kind='code', head_branch=code_head_branch(ws)), ws.root))
            for ref, note in changes:
                report.add('code->nas', f'{ref} ({note})')
        else:
            report.add('code->nas', f'the newest NAS code bundle already has all {len(refs)} branches and tags')

        # 3. Pinned tool downloads, by cmake/pins.json SHA-256 (single compressed archives).
        found, unpinned = local_downloads(ws.downloads, ws.root / 'cmake' / 'pins.json')
        tool_writes = tool_plan(manifest, found, report)
        writes.extend(tool_writes)
        if unpinned:
            report.add('tools', f'{len(unpinned)} unpinned files left local: ' + ', '.join(unpinned))

        # 4. Write: free space first, every file verified, then the manifest, then pruning.
        check_free_space(nas, sum(entry['size'] for _, _, entry, _ in writes), report)
        for rel, local, entry, bundle_repo in writes:
            nas.write_file(rel, local, entry['sha256'], bundle_repo)
            record = {k: v for k, v in entry.items() if k != 'path'}
            record.update(created=created_now(), host=platform.node())
            if entry['kind'] == 'tool' and rel in manifest['files']:
                continue
            manifest['files'][rel] = record
            report.add('nas', f'wrote {rel} ({size_text(entry["size"])}), verified')
        info = source_info(ws, remotes if push_code_remotes else code_remotes(ws)[0])
        if writes or manifest.get('source') != info:
            manifest['source'] = info
            nas.write_manifest(manifest)
        nas.prune(manifest, generations(ws), report)
        if not writes:
            report.add('nas', 'nothing to write; the NAS is up to date')

    # 5. Code hosts (network), after the NAS is safe; the configuration may have arrived with the NAS overlay.
    if push_code_remotes:
        remotes, missing = code_remotes(ws)
        for name in missing:
            report.error('code', f'configured code remote {name!r} does not exist in this checkout')
    for name in remotes:
        push_code_remote(ws, name, report)
    if not push_code_remotes:
        report.add('code->hosts', 'skipped (--no-code-remotes)')
    return report


def tool_plan(manifest, found, report):
    writes, merged = [], 0
    recorded = {e['sha256']: rel for rel, e in manifest['files'].items() if e['kind'] == 'tool'}
    for sha, entry in sorted(found.items(), key=lambda item: item[1]['file']):
        if sha in recorded:
            stored = manifest['files'][recorded[sha]]
            sources = sorted(set(stored.get('sources', [])) | set(entry['sources']))
            if sources != stored.get('sources'):
                stored['sources'] = sources
                merged += 1
            continue
        rel = f'tools/{entry["file"]}'
        if rel in manifest['files']:
            report.error('tools', f'{rel} is recorded with another SHA-256; not overwritten')
            continue
        writes.append((rel, entry['path'], entry, None))
    report.add('tools', f'{len(found)} pinned downloads here; {len(writes)} new for the NAS')
    return writes


def preview_overlay_bundle(ws, desired, scratch):
    """Size of the overlay bundle a sync would write: the desired tree committed in a scratch copy."""
    repo = Path(scratch) / 'preview.git'
    git('init', '--quiet', '--bare', f'--initial-branch={OVERLAY_BRANCH}', posix(repo))
    head = ws.overlay.head() if ws.overlay.exists() else None
    if head:
        git(f'--git-dir={posix(repo)}', 'fetch', '--quiet', posix(ws.overlay.git_dir),
            f'+refs/heads/{OVERLAY_BRANCH}:refs/heads/{OVERLAY_BRANCH}')
    stream = [b'commit refs/heads/main\ncommitter Preview <preview@invalid> 0 +0000\ndata 7\npreview\n']
    if head:
        stream.append(b'from ' + head.encode() + b'\n')
    stream.append(b'deleteall\n')
    for path, data in sorted(desired.items()):
        stream.append(b'M 100644 inline ' + path.encode('utf-8') + b'\ndata %d\n' % len(data) + data + b'\n')
    git(f'--git-dir={posix(repo)}', 'fast-import', '--quiet', data=b''.join(stream))
    return make_bundle([f'--git-dir={posix(repo)}'], Path(scratch) / 'overlay.bundle', '--branches')


def sync_preview(ws, nas, desired, remotes, report):
    """Dry run: exactly the files a sync would write to the NAS, with their sizes; writes nothing there."""
    manifest = nas.manifest()
    overlay_tree = ws.overlay.tree() if ws.overlay.exists() else {}
    added, changed, removed = describe_changes(overlay_tree, desired)
    report.add('dry-run', f'overlay: {len(desired)} tracked files; {len(added)} new, {len(changed)} changed, '
                          f'{len(removed)} removed since the last overlay commit')
    writes = []
    now = stamp()
    with tempfile.TemporaryDirectory(prefix='crd-preview-') as scratch:
        latest_rel, latest = nas.latest(manifest, 'overlay')
        if added or changed or removed or not latest:
            entry = preview_overlay_bundle(ws, desired, scratch)
            writes.append((unique_name(nas, manifest, 'overlay', f'overlay-{now}', '.bundle'), entry['size']))
            if latest:
                report.add('dry-run', 'a sync also merges the newest NAS overlay first when another machine wrote it')
        refs = local_refs(ws.root)
        code_rel, code_latest = nas.latest(manifest, 'code')
        changes, rejected = ref_plan(ws.root, code_latest['refs'] if code_latest else {}, refs)
        for reason in rejected:
            report.error('dry-run', 'code->nas ' + reason)
        if not rejected and (changes or not code_latest):
            entry = make_bundle(['-C', posix(ws.root)], Path(scratch) / 'code.bundle', '--branches',
                                '--tags')
            writes.append((unique_name(nas, manifest, 'code', f'code-{now}', '.bundle'), entry['size']))
            report.add('dry-run', f'code: {len(changes)} of {len(refs)} branches and tags changed since the newest '
                                  f'NAS code bundle')
    found, unpinned = local_downloads(ws.downloads, ws.root / 'cmake' / 'pins.json')
    for rel, _, entry, _ in tool_plan(manifest, found, Report()):
        writes.append((rel, entry['size']))
    planned = sum(size for _, size in writes)
    for rel, size in writes:
        report.add('dry-run', f'would write {posix(nas.root)}/{rel}  {size_text(size)}')
    if writes:
        report.add('dry-run', f'would write {posix(nas.root)}/{MANIFEST_NAME}  (index, replaced atomically)')
    future = json.loads(json.dumps(manifest))
    for rel, size in writes:
        kind = rel.split('/')[0].rstrip('s') if not rel.startswith('tools/') else 'tool'
        future['files'][rel] = {'kind': kind, 'created': '9999'}
    for kind in ('code', 'overlay', 'export'):
        for rel in nas.prune_plan(future, kind, generations(ws)):
            report.add('dry-run', f'would prune {posix(nas.root)}/{rel} (recorded, older than the newest '
                                  f'{generations(ws)})')
    free = shutil.disk_usage(nas.root).free
    report.add('dry-run', f'total {size_text(planned)}; NAS free {size_text(free)} (needs twice the total)')
    if planned and free < 2 * planned:
        report.error('dry-run', 'not enough free space on the NAS share')
    report.add('dry-run', f'{len(unpinned)} unpinned downloads stay local')
    for name in remotes:
        report.add('dry-run', f'code->{name}: branches except {ws.config.get("code_remote_exclude")} and tags '
                              f'except {ws.config.get("code_remote_exclude_tags")} (checked for agent files, '
                              f'transcripts and secrets first)')
    report.add('dry-run', 'nothing was written')
    return report


# ---------------------------------------------------------------------------------------------- status

def status(ws, offline=False, report=None):
    """What differs between this checkout, the NAS and the code hosts. Reads only."""
    report = report or Report()
    differences = 0
    merging = ws.overlay.exists() and ws.overlay.merging()
    if merging:
        differences += 1
        report.add('overlay', 'a merge with the NAS overlay is open: ' + ', '.join(ws.overlay.unmerged()))
    desired, copies, problems = overlay_plan(ws, from_work_tree=merging)
    for problem in problems:
        report.error('refused', problem)
    overlay_tree = ws.overlay.tree() if ws.overlay.exists() else {}
    added, changed, removed = describe_changes(overlay_tree, desired)
    if not ws.overlay.exists():
        report.add('overlay', 'no overlay repository in this checkout yet (sync or restore creates it)')
        differences += 1
    elif added or changed or removed:
        differences += 1
        report.add('overlay', f'{len(added)} new, {len(changed)} changed, {len(removed)} removed files not synced')
        for path in (added + changed + removed)[:20]:
            report.add('overlay', '  ' + path)
    else:
        report.add('overlay', f'working copy matches the last overlay commit ({len(desired)} files)')
    nas = Nas(ws.nas)
    manifest = nas.manifest()
    holder = nas.lock_holder()
    if holder:
        report.add('nas', f'locked by {holder.get("host")} pid {holder.get("pid")} since {holder.get("started")}')
    head = ws.overlay.head() if ws.overlay.exists() else None
    _, latest = nas.latest(manifest, 'overlay')
    nas_overlay = latest['refs'].get(f'refs/heads/{OVERLAY_BRANCH}') if latest else None
    if nas_overlay != head:
        differences += 1
        ahead = head and nas_overlay and ws.overlay.has_commit(nas_overlay) and ws.overlay.is_ancestor(nas_overlay,
                                                                                                     head)
        report.add('overlay', 'the NAS overlay is behind this checkout' if ahead or not nas_overlay else
                   'the NAS overlay has commits this checkout lacks (sync merges them)')
    else:
        report.add('overlay', 'the newest NAS overlay bundle matches this checkout')
    refs = local_refs(ws.root)
    _, code_latest = nas.latest(manifest, 'code')
    changes, rejected = ref_plan(ws.root, code_latest['refs'] if code_latest else {}, refs)
    differences += len(changes) + len(rejected) + (0 if code_latest else 1)
    report.add('code->nas', f'{len(changes)} branches/tags differ from the newest NAS code bundle' if changes or
               not code_latest else 'the newest NAS code bundle has every branch and tag')
    for ref, note in changes:
        report.add('code->nas', f'  {ref} ({note})')
    for reason in rejected:
        report.add('code->nas', '  ' + reason)
    if not offline:
        remotes, _ = code_remotes(ws)
        for name in remotes:
            remote_refs = ls_remote(remotes[name])
            for ref, sha in sorted(refs.items()):
                if remote_excluded(ws, ref):
                    continue
                if remote_refs.get(ref) != sha:
                    differences += 1
                    report.add(f'code->{name}', f'  {ref} differs')
    found, _ = local_downloads(ws.downloads, ws.root / 'cmake' / 'pins.json')
    stored = {e['sha256'] for e in manifest['files'].values() if e['kind'] == 'tool'}
    missing = [e['file'] for s, e in found.items() if s not in stored]
    differences += len(missing)
    report.add('tools', f'{len(missing)} pinned downloads not on the NAS' if missing else
               f'the NAS has all {len(found)} local pinned downloads ({len(stored)} stored)')
    return report, differences


# ---------------------------------------------------------------------------------------------- export

def confidential_paths(ws):
    marked = []
    for line in out('ls-files', '-z', cwd=ws.root).split('\0'):
        if PurePosixPath(line).name == CONFIDENTIAL_MARKER:
            marked.append(line)
    history = out('log', '--branches', '--tags', '--format=', '--name-only', '--diff-filter=A', '--',
                  CONFIDENTIAL_MARKER, f':(glob)**/{CONFIDENTIAL_MARKER}', cwd=ws.root)
    marked.extend(f'history: {line}' for line in history.splitlines() if line)
    for path in ws.desired():
        if PurePosixPath(path).name == CONFIDENTIAL_MARKER:
            marked.append(path)
    return sorted(set(marked))


def export(ws, output_dir=None, break_lock=False, report=None, today=None, include_local_settings=False):
    """One restorable archive, built on local disk; to the NAS as one verified file unless output_dir is given."""
    report = report or Report()
    desired, _, problems = overlay_plan(ws)
    for problem in problems:
        report.error('refused', problem)
    for path in confidential_paths(ws):
        report.error('refused', f'{path}: marked confidential ({CONFIDENTIAL_MARKER}); exports never carry it')
    tips = sorted(set(local_refs(ws.root).values()))
    grep = git('grep', '-I', '-l', '-P', '|'.join(f'(?:{p})' for p in SECRET_PATTERNS.values()), *tips, '--',
               cwd=ws.root, check=False)
    if grep.returncode not in (0, 1):
        report.error('export', 'git grep -P could not scan the code branches: ' +
                     grep.stderr.decode('utf-8', errors='replace').strip())
    for line in grep.stdout.decode('utf-8', errors='replace').splitlines():
        report.error('refused', f'code {line}: contains a secret-shaped token')
    if not ws.overlay.exists():
        report.error('export', 'no overlay repository here; run sync first')
    elif any(describe_changes(ws.overlay.tree(), desired)):
        report.error('export', 'agent files changed since the last overlay commit; run sync first')
    if report.errors:
        return report, None
    nas = None if output_dir else Nas(ws.nas)
    with tempfile.TemporaryDirectory(prefix='crd-export-') as scratch:
        scratch = Path(scratch)
        build = scratch / 'build'
        build.mkdir()
        members = {}
        make_bundle(['-C', posix(ws.root)], build / 'code.bundle', '--branches', '--tags')
        if include_local_settings:
            make_bundle(ws.overlay.args(), build / 'overlay.bundle', '--branches')
            sharing = {'mode': 'owner (local settings included verbatim)'}
            report.add('export', 'owner copy: local settings and personal files included verbatim')
        else:
            _, portable, personal = sharing_overlay_bundle(ws, build / 'overlay.bundle', report)
            sharing = {'mode': 'shared', 'portable': portable, 'left_out': personal}
        members['code.bundle'] = build / 'code.bundle'
        members['overlay.bundle'] = build / 'overlay.bundle'
        tools = {}
        found, _ = local_downloads(ws.downloads, ws.root / 'cmake' / 'pins.json')
        for sha, entry in found.items():
            tools[sha] = dict(entry, store=f'tools/{entry["file"]}')
        if nas is not None:
            manifest = nas.manifest()
            for rel, entry in manifest['files'].items():
                if entry['kind'] == 'tool' and entry['sha256'] not in tools:
                    tools[entry['sha256']] = dict(entry, store=rel, path=nas.verified(rel, entry))
        for entry in tools.values():
            members[entry['store']] = entry['path']
        (build / 'workspace.py').write_bytes(Path(__file__).read_bytes())
        (build / '__main__.py').write_text(MAIN_SOURCE, encoding='utf-8', newline='\n')
        stamp_text = (today or datetime.date.today()).strftime('%Y%m%d')
        if nas is not None:
            rel = unique_name(nas, manifest, 'exports', f'crd-workspace-{stamp_text}', '.zip')
        else:
            rel = f'crd-workspace-{stamp_text}.zip'
            counter = 2
            while (Path(output_dir) / rel).exists():
                rel, counter = f'crd-workspace-{stamp_text}-{counter}.zip', counter + 1
        name = PurePosixPath(rel).name
        (build / 'RESTORE.txt').write_text(RESTORE_TEXT.format(name=name), encoding='utf-8', newline='\n')
        for extra in ('workspace.py', '__main__.py', 'RESTORE.txt'):
            members[extra] = build / extra
        remotes, _ = code_remotes(ws)
        manifest_doc = {'schema': SCHEMA + '#export', 'created': datetime.datetime.now(datetime.UTC).isoformat(
                        timespec='seconds'), 'host': platform.node(), 'source': source_info(ws, remotes),
                        'overlay_head': ws.overlay.head(), 'overlay_sharing': sharing,
                        'tools': [{k: v for k, v in e.items() if k != 'path'} for e in tools.values()],
                        'files': {}}
        for member, path in sorted(members.items()):
            manifest_doc['files'][member] = {'size': Path(path).stat().st_size, 'sha256': file_sha256(path)}
        (build / 'manifest.json').write_text(json.dumps(manifest_doc, indent=2) + '\n', encoding='utf-8',
                                             newline='\n')
        members['manifest.json'] = build / 'manifest.json'
        sums = ''.join(f'{file_sha256(members[m])}  {m}\n' for m in sorted(members))
        (build / 'SHA256SUMS').write_text(sums, encoding='utf-8', newline='\n')
        members['SHA256SUMS'] = build / 'SHA256SUMS'
        archive = scratch / name
        first = ['__main__.py', 'workspace.py', 'RESTORE.txt', 'manifest.json', 'SHA256SUMS']
        with zipfile.ZipFile(archive, 'w', allowZip64=True) as bundle:
            for member in first + sorted(m for m in members if m not in first):
                method = zipfile.ZIP_STORED if member.lower().endswith(STORED) else zipfile.ZIP_DEFLATED
                bundle.write(members[member], member, compress_type=method)
        if verify_archive(archive).errors:
            raise WorkspaceError('the archive built on local disk failed its own verification')
        digest = file_sha256(archive)
        size = archive.stat().st_size
        if nas is None:
            target = Path(output_dir) / name
            target.parent.mkdir(parents=True, exist_ok=True)
            copy_verified(archive, target, digest)
        else:
            with nas.lock(report, break_lock):
                manifest = nas.manifest()
                check_free_space(nas, size, report)
                nas.write_file(rel, archive, digest)
                manifest['files'][rel] = {'kind': 'export', 'size': size, 'sha256': digest, 'created': created_now(),
                                          'host': platform.node()}
                nas.write_manifest(manifest)
                nas.prune(manifest, generations(ws), report)
            target = nas.root / rel
    report.add('export', f'{posix(target)} ({size_text(size)}, {len(members)} members, {len(tools)} pinned '
                         f'downloads), verified')
    report.add('export', f'SHA-256 {digest}')
    return report, target


MAIN_SOURCE = '''"""Run this archive with Python: `python <archive>.zip restore <folder>` (see RESTORE.txt)."""
import sys

import workspace

sys.exit(workspace.main(sys.argv[1:], archive=sys.argv[0]))
'''

RESTORE_TEXT = '''Cerid workspace archive {name}

This file holds the Cerid code repository and the agent overlay with full history (Git bundles), the
pinned tool downloads and this restore program. You need Git and Python 3.12 or newer.

  1. Check the archive:   python {name} verify
  2. Restore it:          python {name} restore <new or empty folder>
  3. Follow the "next steps" it prints.

If your Python cannot run the archive directly, unzip it and run:  python workspace.py restore --from {name} <folder>
Plain-English guide inside the restored folder: docs/recipes/workspace-sync.md
'''


# ---------------------------------------------------------------------------------------------- verify / restore

def verify_archive(path, report=None):
    report = report or Report()
    with zipfile.ZipFile(path) as bundle:
        names = set(bundle.namelist())
        sums = bundle.read('SHA256SUMS').decode('utf-8')
        listed = set()
        for line in sums.splitlines():
            digest, _, member = line.partition('  ')
            listed.add(member)
            if member not in names:
                report.error('verify', f'{member} is listed but missing')
                continue
            hasher = hashlib.sha256()
            with bundle.open(member) as handle:
                for chunk in iter(lambda: handle.read(1 << 20), b''):
                    hasher.update(chunk)
            if hasher.hexdigest() != digest:
                report.error('verify', f'{member} does not match its SHA-256')
        for extra in sorted(names - listed - {'SHA256SUMS'}):
            report.error('verify', f'{extra} is not listed in SHA256SUMS')
    if not report.errors:
        report.add('verify', f'{len(listed)} members match SHA256SUMS')
    return report


class Source:
    """A restore source, opened read-only: an export archive (extracted locally) or the NAS folder."""

    def __init__(self, location, scratch):
        location = Path(location)
        if location.is_file():
            report = verify_archive(location)
            if report.errors:
                raise WorkspaceError('archive failed verification: ' + '; '.join(report.lines))
            with zipfile.ZipFile(location) as bundle:
                bundle.extractall(scratch)
            base = Path(scratch)
            manifest = json.loads((base / 'manifest.json').read_text(encoding='utf-8'))
            self.info = manifest['source']
            self.code = base / 'code.bundle'
            self.overlay = base / 'overlay.bundle'
            self.tools = {e['sha256']: dict(e, path=base / e['store']) for e in manifest['tools']}
            self.described = f'archive {location.name}'
            return
        nas = Nas(location)
        holder = nas.lock_holder()
        if holder:
            raise WorkspaceError(f'{holder.get("host")} is writing to the NAS folder (since {holder.get("started")}); '
                                 f'restore after that sync finishes')
        manifest = nas.manifest()
        code_rel, code = nas.latest(manifest, 'code')
        overlay_rel, overlay = nas.latest(manifest, 'overlay')
        if not code or not overlay:
            raise WorkspaceError(f'{location} has no code or overlay bundle recorded in {MANIFEST_NAME}')
        self.info = dict(manifest.get('source', {}))
        self.info['head_branch'] = code.get('head_branch') or self.info.get('head_branch')
        self.code = nas.verified(code_rel, code)
        self.overlay = nas.verified(overlay_rel, overlay)
        self.tools = {e['sha256']: dict(e, path=nas.path(rel)) for rel, e in manifest['files'].items()
                      if e['kind'] == 'tool'}
        self.described = f'NAS {code_rel} and {overlay_rel}'


def restore(location, destination, claude_dir=None, downloads_dir=None, memory_dir=None, replace=False,
            report=None):
    """Recreate the workspace in a new or empty folder. The source (archive or NAS) is only read."""
    report = report or Report()
    destination = Path(destination).resolve()
    if destination.exists() and any(destination.iterdir()):
        raise WorkspaceError(f'{destination} is not empty; restore into a new or empty folder')
    config_dir = claude_config_dir(claude_dir)
    memory = Path(memory_dir) if memory_dir else claude_project_dir(destination, config_dir) / 'memory'
    if memory.is_dir() and any(memory.iterdir()) and not replace:
        raise WorkspaceError(f'Claude Code memory already exists at {memory}; pass --replace to back it up and '
                             f'replace it')
    with tempfile.TemporaryDirectory(prefix='crd-restore-') as scratch:
        source = Source(location, scratch)
        report.add('source', source.described)
        destination.mkdir(parents=True, exist_ok=True)
        # Code repository: every branch and tag, then the default branch checked out and the host remotes.
        git('init', '--quiet', posix(destination))
        verify = git('bundle', 'verify', '--quiet', posix(source.code), cwd=destination, check=False)
        if verify.returncode:
            raise WorkspaceError('the code bundle failed git bundle verify: ' + verify.stderr.decode(errors='replace'))
        git('fetch', '--quiet', '--update-head-ok', posix(source.code), '+refs/heads/*:refs/heads/*',
            '+refs/tags/*:refs/tags/*', cwd=destination)
        head = source.info.get('head_branch') or 'main'
        git('symbolic-ref', 'HEAD', f'refs/heads/{head}', cwd=destination)
        git('reset', '--quiet', '--hard', cwd=destination)
        for name, url in sorted(source.info.get('code_remotes', {}).items()):
            git('remote', 'add', name, url, cwd=destination)
        restored = local_refs(destination)
        tags = len([r for r in restored if r.startswith('refs/tags/')])
        report.add('code', f'{len(restored) - tags} branches and {tags} tags restored; {head} checked out at '
                           f'{out("rev-parse", "--short", "HEAD", cwd=destination).strip()}')
        # Agent overlay: same work tree, its own Git directory.
        overlay = Overlay(destination)
        overlay.init()
        overlay.run('fetch', '--quiet', '--update-head-ok', posix(source.overlay),
                    f'+refs/heads/{OVERLAY_BRANCH}:refs/heads/{OVERLAY_BRANCH}')
        overlay.run('reset', '--quiet', '--hard', OVERLAY_BRANCH)
        adapted = 0
        for path in overlay.tree():
            if not path.startswith(OVERLAY_HOME + '/'):
                data = (destination / path).read_bytes()
                local = expand(data, {'CERID_ROOT': destination})
                if local != data:
                    (destination / path).write_bytes(local)
                    adapted += 1
        report.add('overlay', f'{len(overlay.tree())} agent files restored at {overlay.head()[:12]}' +
                   (f'; {adapted} shared settings files adapted to this folder' if adapted else ''))
        # Claude Code memory and workflow scripts for this folder path on this machine.
        if memory.is_dir() and any(memory.iterdir()):
            backup = memory.with_name('memory.before-restore-' + datetime.datetime.now().strftime('%Y%m%d%H%M%S'))
            memory.rename(backup)
            report.add('memory', f'previous memory moved to {backup}')
        conflicts = []
        written, _ = write_back(destination, memory, destination, replace=True, claude_dir=config_dir,
                                conflicts=conflicts)
        report.add('memory', f'{written} files placed under {memory.parent} (key {claude_project_key(destination)}) '
                             f'and the user-level Claude Code folder {config_dir}')
        for conflict in conflicts:
            report.add('memory', conflict)
        # Pinned tool downloads, verified by SHA-256 on the local copy.
        downloads = Path(downloads_dir) if downloads_dir else default_downloads_dir()
        placed = 0
        for sha, entry in source.tools.items():
            for rel in entry.get('sources') or [entry['file']]:
                target = downloads / rel
                if target.is_file() and file_sha256(target) == sha:
                    continue
                copy_verified(entry['path'], target, sha)
                placed += 1
        report.add('tools', f'{placed} pinned downloads placed under {downloads} ({len(source.tools)} available)')
    report.add('next', f'cd "{destination}"')
    report.add('next', 'python scripts/check-master-plan.py   (documentation validator; proves the checkout)')
    report.add('next', 'python scripts/workspace.py status --nas <NAS folder>   (or set CERID_WORKSPACE_NAS)')
    report.add('next', 'Windows: scripts\\configure-preset.bat win-debug; Linux: see docs/BUILDING.md')
    report.add('next', 'git fetch --all   (refresh remote-tracking branches from the code hosts)')
    report.add('next', 'Open Claude Code in this folder: its memory is already in place.')
    return report


# ---------------------------------------------------------------------------------------------- command line

def build_parser():
    parser = argparse.ArgumentParser(prog='workspace.py', description=__doc__.split('\n\n')[0])
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1] if
                        (Path(__file__).resolve().parents[1] / '.git').exists() else Path.cwd(),
                        help='checkout root (default: this script\'s checkout)')
    parser.add_argument('--claude-config-dir', type=Path, help='Claude Code configuration folder (default: '
                        'CLAUDE_CONFIG_DIR or ~/.claude)')
    parser.add_argument('--memory-dir', type=Path, help='explicit Claude Code memory folder for this checkout')
    parser.add_argument('--downloads-dir', type=Path, help='pinned download folder (default: ~/.cerid/downloads)')
    commands = parser.add_subparsers(dest='command', required=True)
    sync_parser = commands.add_parser('sync', help='commit the overlay; write verified bundles to the NAS; push code')
    sync_parser.add_argument('--nas', type=Path)
    sync_parser.add_argument('--dry-run', action='store_true', help='list every file it would write; write nothing')
    sync_parser.add_argument('--no-code-remotes', action='store_true', help='leave the code hosts alone')
    sync_parser.add_argument('--break-lock', action='store_true', help='remove another machine\'s NAS lock first')
    status_parser = commands.add_parser('status', help='what differs between this checkout, the NAS and the hosts')
    status_parser.add_argument('--nas', type=Path)
    status_parser.add_argument('--offline', action='store_true', help='do not contact the code hosts')
    export_parser = commands.add_parser('export', help='write one restorable archive (default: <NAS>/exports)')
    export_parser.add_argument('--nas', type=Path)
    export_parser.add_argument('--output-dir', type=Path, help='write the archive here instead of the NAS')
    export_parser.add_argument('--break-lock', action='store_true')
    export_parser.add_argument('--include-local-settings', action='store_true',
                               help='owner copy: keep local settings and personal files verbatim')
    restore_parser = commands.add_parser('restore', help='recreate the workspace from an archive or the NAS')
    restore_parser.add_argument('--from', dest='source', type=Path, help='export archive or NAS folder')
    restore_parser.add_argument('--replace', action='store_true', help='back up and replace existing memory')
    restore_parser.add_argument('destination', type=Path)
    verify_parser = commands.add_parser('verify', help='check an export archive against its SHA256SUMS')
    verify_parser.add_argument('archive', type=Path, nargs='?')
    overlay_parser = commands.add_parser('overlay', help='run git on the overlay repository')
    overlay_parser.add_argument('args', nargs=argparse.REMAINDER)
    return parser


def main(argv=None, archive=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(errors='replace')  # a legacy console code page must never stop a report
    args = build_parser().parse_args(argv)
    try:
        if args.command == 'restore':
            location = args.source or archive
            if not location:
                raise WorkspaceError('restore needs --from <archive.zip | NAS folder>')
            report = restore(location, args.destination, args.claude_config_dir, args.downloads_dir,
                             args.memory_dir, args.replace)
        elif args.command == 'verify':
            target = args.archive or archive
            if not target:
                raise WorkspaceError('verify needs an archive path')
            report = verify_archive(target)
        else:
            ws = Workspace(args.root, getattr(args, 'nas', None), args.claude_config_dir, args.downloads_dir,
                           args.memory_dir)
            if args.command == 'overlay':
                if not ws.overlay.exists():
                    raise WorkspaceError('no overlay repository here; run sync or restore first')
                return subprocess.run(['git', *ws.overlay.args(), *args.args], cwd=ws.root).returncode
            if args.command == 'sync':
                report = sync(ws, args.dry_run, not args.no_code_remotes, args.break_lock)
            elif args.command == 'status':
                report, differences = status(ws, args.offline)
                report.add('status', 'in sync' if not differences else f'{differences} difference(s)')
            else:
                report, _ = export(ws, args.output_dir, args.break_lock,
                                   include_local_settings=args.include_local_settings)
    except WorkspaceError as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    report.print()
    return 1 if report.errors else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
