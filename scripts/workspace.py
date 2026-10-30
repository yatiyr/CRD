#!/usr/bin/env python3
"""Keep the whole Cerid working environment portable through the operator's NAS (stdlib only, Python 3.12+).

Two Git histories share one checkout. The code repository (`.git`) holds code and every document and is the only
history pushed to the code hosts. The agent overlay is a second repository whose Git directory is `.agent-overlay/`
and whose work tree is the same checkout root; it tracks only agent tool files (CLAUDE.md, opencode.json, .claude/,
.codex/, ...) and `.agent/`, which holds copies of the agent state kept outside the checkout (the Claude Code memory
folder of this project and the operator's workflow scripts). The overlay is pushed only to the NAS.

    python scripts/workspace.py sync [--dry-run] [--nas DIR] [--no-code-remotes]
    python scripts/workspace.py status [--nas DIR] [--offline]
    python scripts/workspace.py export [--nas DIR] [--output-dir DIR]
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
OVERLAY_GIT_DIR = '.agent-overlay'
OVERLAY_HOME = '.agent'
OVERLAY_BRANCH = 'main'
CONFIG_FILE = OVERLAY_HOME + '/workspace.json'
MEMORY_DIR = OVERLAY_HOME + '/claude-memory'
WORKFLOWS_DIR = OVERLAY_HOME + '/workflows'
# Agent tool files at the checkout root that the overlay tracks in place (the code repository ignores them).
OVERLAY_ROOT_FILES = ('CLAUDE.md', 'CLAUDE.local.md', 'opencode.json', '.mcp.json')
OVERLAY_ROOT_DIRS = ('.claude', '.codex', '.agents')
# Machine-local state inside those directories: never part of the overlay.
LOCAL_STATE_DIRS = {'worktrees', 'cache', 'caches', 'log', 'logs', 'tmp', 'temp', 'shell-snapshots', 'file-history',
                    'paste-cache', 'todos', 'statsig', 'sessions', 'projects', 'backups', '__pycache__'}
LOCAL_STATE_FILES = ('*.lock', '*.log', '*.tmp', '*.pid', '*.swp')
# The code repository must never receive these paths again (forward-only: history keeps its old copies).
AGENT_PATHS = OVERLAY_ROOT_FILES + OVERLAY_ROOT_DIRS + (OVERLAY_HOME, OVERLAY_GIT_DIR)
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


def collapse_root(data, root):
    """Replace this machine's project root with placeholders so stored agent state is machine-neutral."""
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        return data
    forms = [(str(Path(root)).replace('/', '\\'), NATIVE_ROOT_TOKEN), (Path(root).as_posix(), ROOT_TOKEN)]
    for form, token in sorted(forms, key=lambda item: -len(item[0])):
        if len(form) > 3:
            text = text.replace(form, token)
    return text.encode('utf-8')


def expand_root(data, root):
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        return data
    if ROOT_TOKEN not in text and NATIVE_ROOT_TOKEN not in text:
        return data
    return text.replace(NATIVE_ROOT_TOKEN, native_root(root)).replace(ROOT_TOKEN, posix(root)).encode('utf-8')


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


def safe(path):
    """A NAS share often reports another owner; trust exactly the repository this command opens."""
    return ['-c', f'safe.directory={posix(path)}']


def ls_remote(url):
    refs = {}
    for line in out(*safe(url), 'ls-remote', posix(url)).splitlines():
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

    def run(self, *args, data=None, check=True):
        return git('--literal-pathspecs', f'--git-dir={posix(self.git_dir)}', f'--work-tree={posix(self.root)}',
                   *args, cwd=self.root, data=data, check=check)

    def text(self, *args, **kwargs):
        return self.run(*args, **kwargs).stdout.decode('utf-8', errors='replace')

    def init(self):
        git('init', '--quiet', '--bare', f'--initial-branch={OVERLAY_BRANCH}', posix(self.git_dir))
        for key, value in (('core.bare', 'false'), ('core.worktree', '..'), ('core.autocrlf', 'false'),
                           ('core.safecrlf', 'false'), ('status.showUntrackedFiles', 'no')):
            git(f'--git-dir={posix(self.git_dir)}', 'config', key, value)
        exclude = self.git_dir / 'info' / 'exclude'
        exclude.parent.mkdir(parents=True, exist_ok=True)
        exclude.write_text('# The overlay stages an explicit file list (scripts/workspace.py); nothing else.\n*\n',
                           encoding='utf-8', newline='\n')

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

    def is_ancestor(self, old, new):
        return self.run('merge-base', '--is-ancestor', old, new, check=False).returncode == 0

    def has_commit(self, sha):
        return self.run('cat-file', '-e', sha + '^{commit}', check=False).returncode == 0

    def set_remote(self, url):
        if self.run('remote', 'get-url', 'nas', check=False).returncode == 0:
            self.run('remote', 'set-url', 'nas', posix(url))
        else:
            self.run('remote', 'add', 'nas', posix(url))


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
              'default_branch': 'main'}
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

    def copied_files(self):
        """`.agent/` copies of state outside the checkout: Claude memory and every session's workflow scripts."""
        files = {}
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
        return files

    def desired(self):
        files = self.in_place_files()
        files.update(self.copied_files())
        return files

    def live_targets(self):
        """Where each `.agent/` copy lives on this machine (for writing back after a merge or a restore)."""
        return {MEMORY_DIR: self.memory_dir, WORKFLOWS_DIR: self.memory_dir.parent}


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
    for managed in (MEMORY_DIR, WORKFLOWS_DIR):
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


def write_back(root, memory_dir, project_root, replace=True):
    """Copy the overlay's `.agent/` state to this machine's Claude Code folders (memory is mirrored exactly)."""
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
                written += write_if_changed(workflow_target(memory_dir.parent, rel),
                                            expand_root(path.read_bytes(), project_root))
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
                entry = found.setdefault(sha, {'section': section, 'name': name, 'file': file_name, 'sha256': sha,
                                               'size': path.stat().st_size, 'sources': [], 'path': path})
                entry['sources'].append(rel)
            else:
                unpinned.append(rel)
    return found, unpinned


def tools_manifest(tools_dir):
    path = Path(tools_dir) / 'manifest.json'
    if path.is_file():
        return {e['sha256']: e for e in json.loads(path.read_text(encoding='utf-8'))['entries']}
    return {}


def write_tools_manifest(tools_dir, entries):
    document = {'schema': NAS_SCHEMA + '#tools', 'entries': [
        {k: v for k, v in entry.items() if k != 'path'} for _, entry in sorted(entries.items(),
                                                                             key=lambda item: item[1]['store'])]}
    write_if_changed(Path(tools_dir) / 'manifest.json', (json.dumps(document, indent=2) + '\n').encode())


def store_path(entry):
    return f'{entry["section"]}/{entry["name"]}/{entry["file"]}'


def copy_verified(source, destination, sha):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + '.partial')
    shutil.copyfile(source, temporary)
    if file_sha256(temporary) != sha:
        temporary.unlink()
        raise WorkspaceError(f'Copy of {source} does not match its pinned SHA-256')
    os.replace(temporary, destination)


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


def nas_push_plan(root, nas_refs, refs):
    """[(refspec, note)] and [rejections]: a NAS ref moves only forward, or to a rewrite of a tip this checkout had."""
    plan, rejected = [], []
    for ref, sha in sorted(refs.items()):
        old = nas_refs.get(ref)
        if old == sha:
            continue
        if old is None:
            plan.append((f'{ref}:{ref}', 'new'))
        elif is_ancestor(root, old, sha):
            plan.append((f'{ref}:{ref}', 'fast-forward'))
        elif has_object(root, old):
            plan.append((f'+{ref}:{ref}', 'rewritten here (the old NAS tip is known locally)'))
        else:
            rejected.append(f'{ref}: the NAS holds commits this checkout has never seen; fetch them first')
    return plan, rejected


def remote_branch_checks(root, ref, remote):
    """Reasons a branch's commits that the remote lacks may not be pushed to a code host."""
    reasons = []
    pending = [ref, '--not', f'--remotes={remote}']
    touched = out('log', '--format=', '--name-only', *pending, '--', *AGENT_PATHS, cwd=root)
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


# ---------------------------------------------------------------------------------------------- NAS layout

def nas_layout(nas):
    nas = Path(nas)
    return {'code': nas / 'code.git', 'overlay': nas / 'overlay.git', 'tools': nas / 'tools',
            'exports': nas / 'exports', 'marker': nas / 'crd-nas.json'}


def ensure_nas(nas):
    nas = Path(nas)
    if not nas.parent.is_dir():
        raise WorkspaceError(f'NAS parent folder {nas.parent} is not reachable')
    layout = nas_layout(nas)
    nas.mkdir(exist_ok=True)
    for key in ('code', 'overlay'):
        if not (layout[key] / 'HEAD').is_file():
            git(*safe(layout[key]), 'init', '--quiet', '--bare', f'--initial-branch={OVERLAY_BRANCH}',
                posix(layout[key]))
    layout['tools'].mkdir(exist_ok=True)
    layout['exports'].mkdir(exist_ok=True)
    return layout


def source_info(ws, remotes):
    return {'project_path': str(ws.project_path), 'head_branch': code_head_branch(ws),
            'code_remotes': {name: strip_userinfo(url) for name, url in remotes.items()}}


def write_nas_marker(layout, info):
    document = {'schema': NAS_SCHEMA, 'layout': {'code.git': 'bare code repository (heads and tags)',
                                                 'overlay.git': 'bare agent overlay repository',
                                                 'tools': 'pinned tool downloads by cmake/pins.json SHA-256',
                                                 'exports': 'crd-workspace-YYYYMMDD.zip archives'},
                'source': info}
    return write_if_changed(layout['marker'], (json.dumps(document, indent=2) + '\n').encode())


# ---------------------------------------------------------------------------------------------- sync

def overlay_plan(ws):
    """(desired files, in-place files, copies, problems) with every refusal check applied first."""
    in_place = ws.in_place_files()
    copies = ws.copied_files()
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


def sync(ws, dry_run=False, push_code_remotes=True, report=None):
    report = report or Report()
    nas = ws.nas
    desired, copies, problems = overlay_plan(ws)
    if problems:
        for problem in problems:
            report.error('refused', problem)
        report.add('refused', 'Nothing was written. Remove or relocate the files above, then run sync again.')
        return report
    remotes, missing = code_remotes(ws) if push_code_remotes else ({}, [])
    for name in missing:
        report.error('code', f'configured code remote {name!r} does not exist in this checkout')
    layout = nas_layout(nas)
    nas_ready = (layout['code'] / 'HEAD').is_file() and (layout['overlay'] / 'HEAD').is_file()
    if dry_run:
        return sync_preview(ws, desired, layout, nas_ready, remotes, report)
    layout = ensure_nas(nas)

    # 1. Overlay: mirror the outside state into .agent/, commit, reconcile with the NAS, write back, push.
    written, removed = mirror_agent_home(ws.root, copies)
    report.add('overlay', f'{len(copies)} copied agent-state files ({written} updated, {removed} removed) in .agent/')
    overlay = ws.overlay
    created = not overlay.exists()
    if created:
        overlay.init()
    overlay.set_remote(layout['overlay'])
    remote_tip = ls_remote(layout['overlay']).get(f'refs/heads/{OVERLAY_BRANCH}')
    if remote_tip:
        overlay.run(*safe(layout['overlay']), 'fetch', '--quiet', 'nas',
                    f'+refs/heads/{OVERLAY_BRANCH}:refs/remotes/nas/{OVERLAY_BRANCH}')
    if created and remote_tip and not overlay.head():
        overlay.run('update-ref', f'refs/heads/{OVERLAY_BRANCH}', remote_tip)
        overlay.run('read-tree', remote_tip)
        report.add('overlay', 'adopted the NAS overlay history for this new checkout')
    if stage_overlay(ws, desired):
        overlay.run('commit', '--quiet', '--no-verify', '-m',
                    f'chore(overlay): sync from {platform.node() or "host"}')
        report.add('overlay', f'committed {len(desired)} tracked files as {overlay.head()[:12]}')
    else:
        report.add('overlay', f'no changes ({len(desired)} tracked files)')
    head = overlay.head()
    if remote_tip and head and remote_tip != head and not overlay.is_ancestor(remote_tip, head):
        merged = overlay.run('merge', '--quiet', '--no-edit', f'refs/remotes/nas/{OVERLAY_BRANCH}', check=False)
        if merged.returncode != 0:
            overlay.run('merge', '--abort', check=False)
            report.error('overlay', 'the NAS overlay and this checkout changed the same lines; nothing was pushed. '
                                    'Resolve with `workspace.py overlay merge nas/main`, then sync again.')
            return report
        written, removed = write_back(ws.root, ws.memory_dir, ws.project_path)
        report.add('overlay', f'merged the NAS overlay; wrote {written} and removed {removed} files in the '
                              f'Claude Code folders')
        head = overlay.head()
    if head and head != remote_tip:
        overlay.run(*safe(layout['overlay']), 'push', '--quiet', 'nas',
                    f'refs/heads/{OVERLAY_BRANCH}:refs/heads/{OVERLAY_BRANCH}')
        report.add('overlay', f'pushed {head[:12]} to {posix(layout["overlay"])}')
    else:
        report.add('overlay', 'NAS overlay already up to date')

    # 2. Code repository: every branch and tag to the NAS mirror, then the configured code hosts.
    refs = local_refs(ws.root)
    plan, rejected = nas_push_plan(ws.root, ls_remote(layout['code']), refs)
    for reason in rejected:
        report.error('code→nas', reason)
    if plan:
        git(*safe(layout['code']), 'push', '--quiet', posix(layout['code']), *[spec for spec, _ in plan], cwd=ws.root)
        for spec, note in plan:
            report.add('code→nas', f'{spec.lstrip("+").split(":")[0]} ({note})')
    else:
        report.add('code→nas', f'all {len(refs)} branches and tags up to date')
    head_branch = code_head_branch(ws)
    git(f'--git-dir={posix(layout["code"])}', *safe(layout['code']), 'symbolic-ref', 'HEAD',
        f'refs/heads/{head_branch}')
    for name, url in remotes.items():
        push_code_remote(ws, name, report)
    if not push_code_remotes:
        report.add('code→hosts', 'skipped (--no-code-remotes)')

    # 3. Pinned tool downloads, by cmake/pins.json SHA-256.
    sync_tools(ws, layout, report)
    write_nas_marker(layout, source_info(ws, remotes if push_code_remotes else code_remotes(ws)[0]))
    return report


def push_code_remote(ws, remote, report):
    git('fetch', '--quiet', remote, cwd=ws.root)
    excluded = ws.config.get('code_remote_exclude') or []
    spec = []
    for ref in sorted(local_refs(ws.root, ('refs/heads',))):
        branch = ref[len('refs/heads/'):]
        if any(fnmatch.fnmatch(branch, pattern) for pattern in excluded):
            continue
        reasons = remote_branch_checks(ws.root, ref, remote)
        if reasons:
            report.error(f'code→{remote}', f'{branch} not pushed: ' + '; '.join(reasons))
        else:
            spec.append(f'{ref}:{ref}')
    spec.append('refs/tags/*:refs/tags/*')
    result = git('push', '--porcelain', remote, *spec, cwd=ws.root, check=False)
    for line in result.stdout.decode('utf-8', errors='replace').splitlines():
        flag, _, rest = line.partition('\t')
        if flag in ('!',):
            report.error(f'code→{remote}', 'rejected ' + rest)
        elif flag in (' ', '+', '*', '-'):
            report.add(f'code→{remote}', 'pushed ' + rest)
    if result.returncode != 0 and not any(line.startswith('!') for line in result.stdout.decode().splitlines()):
        report.error(f'code→{remote}', result.stderr.decode('utf-8', errors='replace').strip())


def sync_tools(ws, layout, report):
    found, unpinned = local_downloads(ws.downloads, ws.root / 'cmake' / 'pins.json')
    stored = tools_manifest(layout['tools'])
    copied = 0
    for sha, entry in found.items():
        entry['store'] = store_path(entry)
        target = layout['tools'] / entry['store']
        if not (target.is_file() and target.stat().st_size == entry['size'] and sha in stored):
            copy_verified(entry['path'], target, sha)
            copied += 1
        previous = stored.get(sha, {})
        entry['sources'] = sorted(set(previous.get('sources', [])) | set(entry['sources']))
        stored[sha] = entry
    write_tools_manifest(layout['tools'], stored)
    report.add('tools', f'{len(found)} pinned downloads ({copied} copied); {len(stored)} on the NAS')
    if unpinned:
        report.add('tools', f'{len(unpinned)} unpinned files left local: ' + ', '.join(unpinned))


def sync_preview(ws, desired, layout, nas_ready, remotes, report):
    overlay_tree = ws.overlay.tree() if ws.overlay.exists() else {}
    added, changed, removed = describe_changes(overlay_tree, desired)
    report.add('dry-run', f'overlay: {len(desired)} files; {len(added)} new, {len(changed)} changed, '
                          f'{len(removed)} removed since the last overlay commit')
    for path in (added + changed + removed)[:20]:
        report.add('dry-run', '  ' + path)
    if not nas_ready:
        report.add('dry-run', f'NAS repositories would be created under {posix(ws.nas)}')
    refs = local_refs(ws.root)
    nas_refs = ls_remote(layout['code']) if nas_ready else {}
    plan, rejected = nas_push_plan(ws.root, nas_refs, refs)
    report.add('dry-run', f'code→nas: {len(plan)} of {len(refs)} branches/tags would be pushed')
    for reason in rejected:
        report.error('dry-run', 'code→nas ' + reason)
    for name in remotes:
        report.add('dry-run', f'code→{name}: branches except {ws.config.get("code_remote_exclude")} and all tags '
                              f'(checked for agent files, transcripts and secrets first)')
    found, unpinned = local_downloads(ws.downloads, ws.root / 'cmake' / 'pins.json')
    stored = tools_manifest(layout['tools']) if layout['tools'].is_dir() else {}
    report.add('dry-run', f'tools: {len([s for s in found if s not in stored])} of {len(found)} pinned downloads '
                          f'would be copied; {len(unpinned)} unpinned stay local')
    report.add('dry-run', 'nothing was written')
    return report


# ---------------------------------------------------------------------------------------------- status

def status(ws, offline=False, report=None):
    report = report or Report()
    differences = 0
    desired, copies, problems = overlay_plan(ws)
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
    layout = nas_layout(ws.nas)
    if not (layout['code'] / 'HEAD').is_file():
        report.add('nas', f'no NAS repositories at {posix(ws.nas)} yet')
        return report, differences + 1
    head = ws.overlay.head() if ws.overlay.exists() else None
    nas_overlay = ls_remote(layout['overlay']).get(f'refs/heads/{OVERLAY_BRANCH}')
    if nas_overlay != head:
        differences += 1
        known = head and nas_overlay and ws.overlay.has_commit(nas_overlay) and ws.overlay.is_ancestor(
            nas_overlay, head)
        report.add('overlay', 'NAS overlay is behind this checkout' if known else
                   'NAS overlay has commits this checkout lacks (sync merges them)')
    else:
        report.add('overlay', 'NAS overlay matches this checkout')
    refs = local_refs(ws.root)
    plan, rejected = nas_push_plan(ws.root, ls_remote(layout['code']), refs)
    differences += len(plan) + len(rejected)
    report.add('code→nas', f'{len(plan)} branches/tags to push' if plan else 'NAS mirror has every branch and tag')
    for spec, note in plan:
        report.add('code→nas', f'  {spec.lstrip("+").split(":")[0]} ({note})')
    for reason in rejected:
        report.add('code→nas', '  ' + reason)
    if not offline:
        remotes, _ = code_remotes(ws)
        excluded = ws.config.get('code_remote_exclude') or []
        for name in remotes:
            remote_refs = ls_remote(remotes[name])
            for ref, sha in sorted(refs.items()):
                branch = ref.split('/', 2)[2]
                if ref.startswith('refs/heads/') and any(fnmatch.fnmatch(branch, p) for p in excluded):
                    continue
                if remote_refs.get(ref) != sha:
                    differences += 1
                    report.add(f'code→{name}', f'  {ref} differs')
    found, _ = local_downloads(ws.downloads, ws.root / 'cmake' / 'pins.json')
    stored = tools_manifest(layout['tools'])
    missing = [e['file'] for s, e in found.items() if s not in stored]
    differences += len(missing)
    report.add('tools', f'{len(missing)} pinned downloads not on the NAS' if missing else
               f'NAS has all {len(found)} local pinned downloads ({len(stored)} stored)')
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


def export(ws, output_dir=None, report=None, today=None):
    report = report or Report()
    desired, _, problems = overlay_plan(ws)
    for problem in problems:
        report.error('refused', problem)
    marked = confidential_paths(ws)
    for path in marked:
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
    stamp = (today or datetime.date.today()).strftime('%Y%m%d')
    target_dir = Path(output_dir) if output_dir else ensure_nas(ws.nas)['exports']
    target_dir.mkdir(parents=True, exist_ok=True)
    name, counter = f'crd-workspace-{stamp}.zip', 2
    while (target_dir / name).exists():
        name, counter = f'crd-workspace-{stamp}-{counter}.zip', counter + 1
    archive = target_dir / name
    with tempfile.TemporaryDirectory(prefix='crd-export-') as scratch:
        scratch = Path(scratch)
        git('bundle', 'create', '--quiet', posix(scratch / 'code.bundle'), '--branches', '--tags', cwd=ws.root)
        ws.overlay.run('bundle', 'create', '--quiet', posix(scratch / 'overlay.bundle'), '--branches')
        members = {'code.bundle': scratch / 'code.bundle', 'overlay.bundle': scratch / 'overlay.bundle'}
        found, _ = local_downloads(ws.downloads, ws.root / 'cmake' / 'pins.json')
        try:
            nas_tools = nas_layout(ws.nas)['tools']
        except WorkspaceError:
            nas_tools = None
        stored = tools_manifest(nas_tools) if nas_tools and nas_tools.is_dir() else {}
        tools = {}
        for sha, entry in stored.items():
            if (nas_tools / entry['store']).is_file():
                tools[sha] = dict(entry, path=nas_tools / entry['store'])
        for sha, entry in found.items():
            entry['store'] = store_path(entry)
            if sha in tools:
                tools[sha]['sources'] = sorted(set(tools[sha]['sources']) | set(entry['sources']))
            else:
                tools[sha] = entry
        for entry in tools.values():
            members['tools/' + entry['store']] = entry['path']
        source = Path(__file__).read_bytes()
        (scratch / 'workspace.py').write_bytes(source)
        (scratch / '__main__.py').write_text(MAIN_SOURCE, encoding='utf-8', newline='\n')
        (scratch / 'RESTORE.txt').write_text(RESTORE_TEXT.format(name=name), encoding='utf-8', newline='\n')
        for extra in ('workspace.py', '__main__.py', 'RESTORE.txt'):
            members[extra] = scratch / extra
        remotes, _ = code_remotes(ws)
        manifest = {'schema': SCHEMA + '#export', 'created': datetime.datetime.now(datetime.UTC).isoformat(
                    timespec='seconds'), 'host': platform.node(), 'source': source_info(ws, remotes),
                    'overlay_head': ws.overlay.head(),
                    'tools': [{k: v for k, v in e.items() if k != 'path'} for e in tools.values()],
                    'files': {}}
        for member, path in sorted(members.items()):
            manifest['files'][member] = {'size': Path(path).stat().st_size, 'sha256': file_sha256(path)}
        (scratch / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
        members['manifest.json'] = scratch / 'manifest.json'
        sums = ''.join(f'{file_sha256(members[m])}  {m}\n' for m in sorted(members))
        (scratch / 'SHA256SUMS').write_text(sums, encoding='utf-8', newline='\n')
        members['SHA256SUMS'] = scratch / 'SHA256SUMS'
        partial = archive.with_name(archive.name + '.partial')
        with zipfile.ZipFile(partial, 'w', allowZip64=True) as bundle:
            for member in ['__main__.py', 'workspace.py', 'RESTORE.txt', 'manifest.json', 'SHA256SUMS'] + sorted(
                    m for m in members if m not in ('__main__.py', 'workspace.py', 'RESTORE.txt', 'manifest.json',
                                                    'SHA256SUMS')):
                method = zipfile.ZIP_STORED if member.lower().endswith(STORED) else zipfile.ZIP_DEFLATED
                bundle.write(members[member], member, compress_type=method)
        os.replace(partial, archive)
    digest = file_sha256(archive)
    archive.with_name(archive.name + '.sha256').write_text(f'{digest}  {archive.name}\n', encoding='utf-8',
                                                           newline='\n')
    report.add('export', f'{posix(archive)} ({archive.stat().st_size / 1048576:.1f} MiB, {len(members)} members, '
                         f'{len(tools)} pinned downloads)')
    report.add('export', f'SHA-256 {digest}')
    return report, archive


MAIN_SOURCE = '''"""Run this archive with Python: `python <archive>.zip restore <folder>` (see RESTORE.txt)."""
import sys

import workspace

sys.exit(workspace.main(sys.argv[1:], archive=sys.argv[0]))
'''

RESTORE_TEXT = '''Cerid workspace archive {name}

This file holds the Cerid code repository and the agent overlay with full history (Git bundles), the
pinned tool downloads and this restore program. You need Git and Python 3.12 or newer.

  1. Check the archive:   python {name} verify
  2. Restore it:          python {name} restore <empty or new folder>
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
    """A restore source: an extracted export archive or the NAS folder."""

    def __init__(self, location, scratch):
        location = Path(location)
        if location.is_file():
            report = verify_archive(location)
            if report.errors:
                raise WorkspaceError('archive failed verification: ' + '; '.join(report.lines))
            with zipfile.ZipFile(location) as bundle:
                bundle.extractall(scratch)
            self.base = Path(scratch)
            manifest = json.loads((self.base / 'manifest.json').read_text(encoding='utf-8'))
            self.info = manifest['source']
            self.code = self.base / 'code.bundle'
            self.overlay = self.base / 'overlay.bundle'
            self.tools = {e['sha256']: dict(e, path=self.base / 'tools' / e['store']) for e in manifest['tools']}
            self.nas = None
        else:
            layout = nas_layout(location)
            if not layout['marker'].is_file():
                raise WorkspaceError(f'{location} is neither an export archive nor a NAS workspace folder')
            self.info = json.loads(layout['marker'].read_text(encoding='utf-8'))['source']
            self.code, self.overlay = layout['code'], layout['overlay']
            self.tools = {sha: dict(e, path=layout['tools'] / e['store'])
                          for sha, e in tools_manifest(layout['tools']).items()}
            self.nas = location


def restore(location, destination, claude_dir=None, downloads_dir=None, memory_dir=None, replace=False,
            report=None):
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
        destination.mkdir(parents=True, exist_ok=True)
        # Code repository: every branch and tag, then the default branch checked out and the host remotes.
        git('init', '--quiet', posix(destination))
        git(*safe(source.code), 'fetch', '--quiet', '--update-head-ok', posix(source.code),
            '+refs/heads/*:refs/heads/*', '+refs/tags/*:refs/tags/*', cwd=destination)
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
        git(*safe(source.overlay), 'clone', '--quiet', '--bare', posix(source.overlay), posix(overlay.git_dir))
        git(f'--git-dir={posix(overlay.git_dir)}', 'remote', 'remove', 'origin')
        for key, value in (('core.bare', 'false'), ('core.worktree', '..'), ('core.autocrlf', 'false'),
                           ('core.safecrlf', 'false'), ('status.showUntrackedFiles', 'no')):
            git(f'--git-dir={posix(overlay.git_dir)}', 'config', key, value)
        (overlay.git_dir / 'info').mkdir(exist_ok=True)
        (overlay.git_dir / 'info' / 'exclude').write_text('*\n', encoding='utf-8', newline='\n')
        overlay.run('reset', '--quiet', '--hard', OVERLAY_BRANCH)
        if source.nas:
            overlay.set_remote(source.nas / 'overlay.git')
        report.add('overlay', f'{len(overlay.tree())} agent files restored at {overlay.head()[:12]}')
        # Claude Code memory and workflow scripts for this folder path on this machine.
        if memory.is_dir() and any(memory.iterdir()):
            backup = memory.with_name('memory.before-restore-' + datetime.datetime.now().strftime('%Y%m%d%H%M%S'))
            memory.rename(backup)
            report.add('memory', f'previous memory moved to {backup}')
        written, _ = write_back(destination, memory, destination, replace=True)
        report.add('memory', f'{written} files placed under {memory.parent} (key {claude_project_key(destination)})')
        # Pinned tool downloads, verified by SHA-256.
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
    sync_parser = commands.add_parser('sync', help='commit and push the overlay and code to the NAS and code hosts')
    sync_parser.add_argument('--nas', type=Path)
    sync_parser.add_argument('--dry-run', action='store_true', help='report the plan; write nothing')
    sync_parser.add_argument('--no-code-remotes', action='store_true', help='push code only to the NAS')
    status_parser = commands.add_parser('status', help='what differs between this checkout, the NAS and the hosts')
    status_parser.add_argument('--nas', type=Path)
    status_parser.add_argument('--offline', action='store_true', help='do not contact the code hosts')
    export_parser = commands.add_parser('export', help='write one restorable archive (default: <NAS>/exports)')
    export_parser.add_argument('--nas', type=Path)
    export_parser.add_argument('--output-dir', type=Path)
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
                return subprocess.run(['git', f'--git-dir={posix(ws.overlay.git_dir)}',
                                       f'--work-tree={posix(ws.root)}', *args.args], cwd=ws.root).returncode
            if args.command == 'sync':
                report = sync(ws, args.dry_run, not args.no_code_remotes)
            elif args.command == 'status':
                report, differences = status(ws, args.offline)
                report.add('status', 'in sync' if not differences else f'{differences} difference(s)')
            else:
                report, _ = export(ws, args.output_dir)
    except WorkspaceError as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    report.print()
    return 1 if report.errors else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
