#!/usr/bin/env python3
"""Tests for scripts/workspace.py: the agent overlay, NAS sync, export and restore (no network, no build).

Synthetic checkouts in temporary folders stand in for the code repository, the NAS folder (beside a neighbour file
that must never change), a code host, the Claude Code configuration folder and the pinned download cache, so every
path runs on any host with Git and Python 3.12+: the Claude Code project-key derivation against names observed on a
real machine; refusal of transcripts, credential files and secret token shapes (with harmless look-alikes
accepted); the code repository's ignore rules for every agent path; a dry run that lists every NAS file and size
and writes nothing; a sync that writes only verified single files and nothing on a second run; a copy that reads
back wrong (deleted, loud failure, NAS unchanged); the single-writer lock (another host refused, a dead local
process's lock taken over, a restore refused while a sync writes); the free-space check; confinement to the NAS
folder; generations pruned only among recorded files; the code-host checks; the NAS code lease against another
machine's commits; an export written to the NAS and restored through its own entry point while the NAS is only
read (refs, files, overlay, memory with path placeholders, workflow scripts, tool downloads); a NAS restore with a
two-machine memory merge and a conflict resolved across syncs; and an export refused for a confidential marker.
Run: python scripts/test-workspace.py
"""
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('workspace', ROOT / 'scripts' / 'workspace.py')
ws_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ws_mod)
_hygiene_spec = importlib.util.spec_from_file_location('hygiene', ROOT / 'scripts' / 'check-repository.py')
hygiene = importlib.util.module_from_spec(_hygiene_spec)
_hygiene_spec.loader.exec_module(hygiene)

# Secret-shaped fixtures are assembled at run time so this file never contains one.
FAKE = {
    'GitHub token': 'gh' + 'p_' + 'A1b2C3d4' * 5,
    'GitLab token': 'gl' + 'pat-' + 'x' * 24,
    'AWS access key': 'AK' + 'IA' + 'ABCDEFGHIJKLMNOP',
    'Anthropic key': 'sk-' + 'ant-' + 'a' * 30,
    'private key': '-----BEGIN ' + 'OPENSSH PRIVATE KEY-----',
    'credentials in a URL': 'https://' + 'user:' + 'hunter2' + '@example.com/repo.git',
}


def git(*args, cwd=None):
    result = subprocess.run(['git', *args], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise AssertionError(f'git {args} failed: {result.stderr.decode()}')
    return result.stdout.decode().strip()


def remove_tree(path):
    def retry(function, target, _):
        os.chmod(target, stat.S_IWRITE)
        function(target)
    shutil.rmtree(path, onexc=retry)


class KeyTests(unittest.TestCase):
    def test_project_key_matches_names_observed_on_a_real_machine(self):
        observed = {
            'D:\\Dev\\cerid': 'D--Dev-cerid',
            'C:\\Users\\abici': 'C--Users-abici',
            'D:\\Dev\\nav\\.claude\\worktrees\\elastic-kepler-ad555e':
                'D--Dev-nav--claude-worktrees-elastic-kepler-ad555e',
            'D:\\Dev\\nav\\Engine\\Source\\Navigation\\Native': 'D--Dev-nav-Engine-Source-Navigation-Native',
            'D:\\Dev\\navpower\\navpower_scenario': 'D--Dev-navpower-navpower-scenario',
            '/home/user/cerid': '-home-user-cerid',
            '/Users/Ada/Dev/Cerid': '-Users-Ada-Dev-Cerid',
        }
        for path, key in observed.items():
            self.assertEqual(ws_mod.claude_project_key(path), key)

    def test_long_project_path_needs_an_existing_folder_or_an_explicit_memory_dir(self):
        with tempfile.TemporaryDirectory() as temp:
            long_path = '/work/' + 'x' * 250
            with self.assertRaisesRegex(ws_mod.WorkspaceError, '--memory-dir'):
                ws_mod.claude_project_dir(long_path, temp)
            existing = Path(temp) / 'projects' / (ws_mod.claude_project_key(long_path)[:200] + '-abc123')
            existing.mkdir(parents=True)
            self.assertEqual(ws_mod.claude_project_dir(long_path, temp), existing)

    def test_config_dir_honours_claude_config_dir(self):
        old = os.environ.get('CLAUDE_CONFIG_DIR')
        try:
            os.environ['CLAUDE_CONFIG_DIR'] = '/somewhere/claude'
            self.assertEqual(ws_mod.claude_config_dir(), Path('/somewhere/claude'))
            self.assertEqual(ws_mod.claude_config_dir('/explicit'), Path('/explicit'))
        finally:
            if old is None:
                os.environ.pop('CLAUDE_CONFIG_DIR', None)
            else:
                os.environ['CLAUDE_CONFIG_DIR'] = old


class RefusalTests(unittest.TestCase):
    def test_every_secret_shape_is_refused_without_echoing_it(self):
        for name, token in FAKE.items():
            problems = ws_mod.refusals({'.agent/claude-memory/note.md': f'line one\nkey {token}\n'.encode()})
            self.assertEqual(len(problems), 1, name)
            self.assertIn(':2:', problems[0])
            self.assertIn(name, problems[0])
            self.assertNotIn(token, problems[0])

    def test_look_alikes_in_permission_rules_and_notes_are_accepted(self):
        harmless = ('PowerShell(wsl -- bash -c "sudo -n true && echo NOPROMPT || echo NEEDS_PASSWORD")\n'
                    'the password prompt appears; the token budget is 12k; git@github.com:yatiyr/CRD.git\n'
                    'https://github.com/yatiyr/CRD.git and sk-short\n')
        self.assertEqual(ws_mod.refusals({'.claude/settings.local.json': harmless.encode()}), [])

    def test_transcripts_and_credential_files_are_refused(self):
        problems = ws_mod.refusals({'.agent/claude-memory/session.jsonl': b'{}', '.claude/.credentials.json': b'{}',
                                    '.codex/auth.json': b'{}', '.claude/agent.pem': b'', '.claude/.env': b''})
        self.assertEqual(len(problems), 5)
        self.assertTrue(any('transcript' in p for p in problems))

    def test_uncached_read_back_hashes_every_size_like_a_plain_read(self):
        with tempfile.TemporaryDirectory() as temp:
            for size in (0, 1, 4095, 4096, (1 << 20) - 1, 1 << 20, (1 << 20) + 3, 3 * (1 << 20) + 517):
                path = Path(temp) / f'f{size}'
                path.write_bytes(os.urandom(size))
                self.assertEqual(ws_mod.uncached_sha256(path), ws_mod.file_sha256(path), size)

    def test_root_placeholders_round_trip_byte_exact(self):
        root = Path('D:/Dev/cerid') if os.name == 'nt' else Path('/home/u/cerid')
        original = (f'See {root.as_posix()}/MEMORY.md and {ws_mod.native_root(root)}\\docs\\x.md; '
                    f'{root.as_posix()}-old is another folder.\n').encode()
        stored = ws_mod.collapse_root(original, root)
        self.assertNotIn(root.as_posix().encode() + b'/', stored)
        self.assertIn(root.as_posix().encode() + b'-old', stored, 'a longer folder name is not the root')
        self.assertIn(ws_mod.ROOT_TOKEN.encode(), stored)
        self.assertEqual(ws_mod.expand_root(stored, root), original)
        moved = ws_mod.expand_root(stored, Path('/srv/friend/crd'))
        self.assertIn(b'/srv/friend/crd/MEMORY.md', moved)
        self.assertEqual(ws_mod.collapse_root(b'\xff\xfe binary', root), b'\xff\xfe binary')


OWNER_ROOT = 'D:\\Dev\\cerid'
RECIPIENT_ROOT = 'E:\\Work\\crd'


def owner_settings():
    """A realistic synthetic settings.local.json of the owner (no real secret: token shapes are assembled)."""
    return {
        'permissions': {
            'allow': [
                'Bash(ls:*)',
                'Bash(python scripts/check-master-plan.py)',
                'PowerShell(cd D:\\Dev\\cerid; git status)',
                'PowerShell(cd D:\\\\Dev\\\\cerid; python scripts/x.py)',
                'Read(//d/Dev/cerid/docs/**)',
                'Bash(wsl -e bash -lc "cd /mnt/d/Dev/cerid && ls")',
                'Read(//c/Users/owner/.claude/**)',
                'PowerShell(Get-Content Z:\\Arsivlik\\Assets\\Repos\\crd\\MANIFEST.json)',
                'PowerShell(& "C:\\Program Files\\CMake\\bin\\cmake.exe" --version)',
                'Bash(cat ~/.ssh/config)',
                'Bash(curl -H "Authorization: Bearer abc" https://api.example.com)',
                'Bash(export GITLAB_TOKEN=abc)',
                'Bash(git push https://' + 'user:' + 'pw' + '@gitlab.com/x.git)',
                'Bash(sudo apt-get install ninja-build)',
                'PowerShell(wsl -d Ubuntu -- bash -c "sudo -n true 2>&1 && echo NOPROMPT || echo NEEDS_PASSWORD")',
                'Bash',
                'PowerShell(*)',
                'Read(//**)',
                'WebFetch(domain:docs.python.org)',
                'mcp__searxng__search',
                'WebSearch',
            ],
            'deny': ['Bash(rm -rf:*)'],
            'additionalDirectories': ['D:\\Dev\\cerid\\build', 'C:\\Users\\owner\\Downloads'],
        },
        'hooks': {
            'PreToolUse': [{'matcher': 'Bash', 'hooks': [
                {'type': 'command', 'command': 'python D:/Dev/cerid/scripts/hook.py'}]}],
            'Stop': [{'hooks': [{'type': 'command', 'command': 'powershell -File C:/Users/owner/notify.ps1'}]}],
        },
        'env': {'CRD_SCCACHE': 'off', 'GITLAB_TOKEN': 'abc', 'TMPDIR': 'C:\\Users\\owner\\tmp'},
        'apiKeyHelper': '/home/owner/bin/key.sh',
        'enabledMcpjsonServers': ['searxng'],
        'model': 'opus',
    }


class SanitizeTests(unittest.TestCase):
    def test_shared_settings_keep_project_rules_rewrite_the_root_and_drop_personal_access(self):
        data = (json.dumps(owner_settings(), indent=2) + '\n').encode()
        shared, counts = ws_mod.sanitize_settings(data, OWNER_ROOT)
        self.assertEqual(counts, {'kept': 9, 'rewritten': 6, 'personal': 7, 'credentials': 5, 'elevation': 2,
                                  'broad': 3})
        for leak in (b'owner', b'Arsivlik', b'Dev/cerid', b'Dev\\\\cerid', b'abc', b'Program Files'):
            self.assertNotIn(leak, shared)
        local = json.loads(ws_mod.expand(shared, {'CERID_ROOT': RECIPIENT_ROOT}))
        self.assertEqual(local['permissions']['allow'], [
            'Bash(ls:*)',
            'Bash(python scripts/check-master-plan.py)',
            'PowerShell(cd E:\\Work\\crd; git status)',
            'PowerShell(cd E:\\\\Work\\\\crd; python scripts/x.py)',
            'Read(//e/Work/crd/docs/**)',
            'Bash(wsl -e bash -lc "cd /mnt/e/Work/crd && ls")',
            'WebFetch(domain:docs.python.org)',
            'mcp__searxng__search',
            'WebSearch',
        ])
        self.assertEqual(local['permissions']['deny'], ['Bash(rm -rf:*)'])
        self.assertEqual(local['permissions']['additionalDirectories'], ['E:\\Work\\crd\\build'])
        self.assertEqual(local['hooks'], {'PreToolUse': [{'matcher': 'Bash', 'hooks': [
            {'type': 'command', 'command': 'python E:/Work/crd/scripts/hook.py'}]}]})
        self.assertEqual(local['env'], {'CRD_SCCACHE': 'off'})
        self.assertNotIn('apiKeyHelper', local)
        self.assertEqual((local['enabledMcpjsonServers'], local['model']), (['searxng'], 'opus'))
        on_linux = json.loads(ws_mod.expand(shared, {'CERID_ROOT': '/home/friend/crd'}))
        self.assertIn('PowerShell(cd /home/friend/crd; git status)', on_linux['permissions']['allow'])
        self.assertIn('Read(//home/friend/crd/docs/**)', on_linux['permissions']['allow'])

    def test_the_shared_configuration_drops_only_the_nas_folder(self):
        config = {'schema': ws_mod.SCHEMA, 'nas': 'Z:/Arsivlik/Assets/Repos/crd', 'code_remotes': ['gitlab']}
        shared = json.loads(ws_mod.sanitize_workspace_config(json.dumps(config).encode()))
        self.assertEqual(shared, {'schema': ws_mod.SCHEMA, 'code_remotes': ['gitlab']})


class IgnoreTests(unittest.TestCase):
    AGENT = ['CLAUDE.md', 'CLAUDE.local.md', 'opencode.json', '.mcp.json', '.claude/settings.local.json',
             '.codex/config.toml', '.agents/skills/x.md', '.agent/claude-memory/MEMORY.md', '.agent-overlay/HEAD']
    DOCS = ['AGENTS.md', 'MEMORY.md', 'context.md', 'START_HERE.md', 'README.md', 'docs/sessions/x.md',
            'docs/recipes/workspace-sync.md', 'scripts/workspace.py']

    def test_code_repository_ignores_every_agent_path_and_no_document(self):
        result = subprocess.run(['git', '-C', str(ROOT), 'check-ignore', '--no-index', '--stdin', '-z'],
                                input=('\0'.join(self.AGENT + self.DOCS) + '\0').encode(), stdout=subprocess.PIPE)
        ignored = set(p for p in result.stdout.decode().split('\0') if p)
        self.assertEqual(sorted(ignored), sorted(self.AGENT))

    def test_agent_files_are_no_longer_tracked(self):
        tracked = git('-C', str(ROOT), 'ls-files', '--', *ws_mod.AGENT_PATHS).splitlines()
        self.assertEqual(tracked, [])

    def test_repository_guard_accepts_the_untracked_agent_files_as_ignored_local_state(self):
        source = (ROOT / 'scripts' / 'check-repository.py').read_text(encoding='utf-8')
        self.assertEqual(hygiene.AGENT_ROOT_FILES, set(ws_mod.OVERLAY_ROOT_FILES))
        self.assertEqual(hygiene.AGENT_ROOT_DIRS, set(ws_mod.OVERLAY_ROOT_DIRS) | {ws_mod.OVERLAY_HOME,
                                                                                 ws_mod.OVERLAY_GIT_DIR})
        self.assertIn('*sorted(AGENT_ROOT_FILES)', source.split('hidden = [')[1].split(']')[0])
        for name in ws_mod.OVERLAY_ROOT_FILES:
            self.assertIn(name, hygiene.ROOT_FILES)
            self.assertNotIn(f"'{name}'", source.split('visible = [')[1].split(']')[0])
        self.assertEqual(hygiene.check()[1], [])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ('CLAUDE.md', 'opencode.json', 'CLAUDE.local.md', '.mcp.json'):
                (root / name).write_text('x')
            for name in ('.agent-overlay', '.agent', '.claude', '.codex', '.agents'):
                (root / name).mkdir()
            self.assertEqual(hygiene.root_artifact_errors(root), [])


class Machine:
    """One synthetic machine: a code checkout, its Claude Code folder and its download cache."""

    def __init__(self, base, name, root=None):
        self.base = base / name
        self.root = Path(root) if root else self.base / 'crd'
        self.claude = self.base / 'claude'
        self.downloads = self.base / 'downloads'

    def workspace(self, nas):
        return ws_mod.Workspace(self.root, nas, self.claude, self.downloads)

    def memory(self, root=None):
        return self.claude / 'projects' / ws_mod.claude_project_key(Path(root or self.root).resolve()) / 'memory'


def nas_listing(nas):
    """{relative path: (size, mtime_ns)} of every file in the NAS folder."""
    return {p.relative_to(nas).as_posix(): (p.stat().st_size, p.stat().st_mtime_ns)
            for p in sorted(nas.rglob('*')) if p.is_file()}


class EndToEndTests(unittest.TestCase):
    def setUp(self):
        self.base = Path(tempfile.mkdtemp(prefix='crd-ws-test-')).resolve()
        self.saved_env = {k: os.environ.get(k) for k in ('GIT_CONFIG_GLOBAL', 'GIT_CONFIG_NOSYSTEM',
                                                         'GIT_AUTHOR_NAME', 'GIT_AUTHOR_EMAIL',
                                                         'GIT_COMMITTER_NAME', 'GIT_COMMITTER_EMAIL',
                                                         'CERID_WORKSPACE_NAS', 'CLAUDE_CONFIG_DIR')}
        (self.base / 'gitconfig').write_text('')
        os.environ.update({'GIT_CONFIG_GLOBAL': str(self.base / 'gitconfig'), 'GIT_CONFIG_NOSYSTEM': '1',
                           'GIT_AUTHOR_NAME': 'Test', 'GIT_AUTHOR_EMAIL': 'test@example.invalid',
                           'GIT_COMMITTER_NAME': 'Test', 'GIT_COMMITTER_EMAIL': 'test@example.invalid'})
        os.environ.pop('CERID_WORKSPACE_NAS', None)
        os.environ.pop('CLAUDE_CONFIG_DIR', None)
        self.nas = self.base / 'nas' / 'crd'
        self.nas.mkdir(parents=True)
        (self.base / 'nas' / 'neighbour.txt').write_text('another folder on the share; never touched\n')
        self.host = self.base / 'host.git'
        git('init', '--quiet', '--bare', '-b', 'main', str(self.host))
        self.mirror = self.base / 'mirror.git'
        git('init', '--quiet', '--bare', '-b', 'main', str(self.mirror))
        self.a = Machine(self.base, 'a')
        self.make_checkout(self.a)

    def tearDown(self):
        for key, value in self.saved_env.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        self.assertEqual((self.base / 'nas' / 'neighbour.txt').read_text(),
                         'another folder on the share; never touched\n')
        self.assertEqual(sorted(p.name for p in (self.base / 'nas').iterdir()), ['crd', 'neighbour.txt'])
        remove_tree(self.base)

    def make_checkout(self, machine, generations=None):
        root = machine.root
        root.mkdir(parents=True)
        git('init', '--quiet', '-b', 'main', cwd=root)
        git('config', 'core.autocrlf', 'false', cwd=root)
        shutil.copyfile(ROOT / '.gitignore', root / '.gitignore')
        shutil.copyfile(ROOT / '.gitattributes', root / '.gitattributes')  # it also applies to the overlay
        (root / 'docs').mkdir()
        (root / 'docs' / 'README.md').write_text('# docs\n')
        (root / 'cmake').mkdir()
        tool = b'pinned tool archive bytes\n'
        machine.tool = tool
        (root / 'cmake' / 'pins.json').write_text(json.dumps({'schema': 'cerid-pins/1', 'tools': {'demo': {
            'file': 'demo-1.0.tar.gz', 'sha256': ws_mod.hashlib.sha256(tool).hexdigest()}}}))
        git('add', '-A', cwd=root)
        git('commit', '--quiet', '-m', 'initial', cwd=root)
        git('branch', 'feature', cwd=root)
        git('branch', 'worktree-agent-x', cwd=root)
        git('tag', 'v1', cwd=root)
        git('tag', 'backup-before-strip', cwd=root)
        git('remote', 'add', 'host', str(self.host), cwd=root)
        git('remote', 'add', 'mirror', str(self.mirror), cwd=root)
        # Agent files the overlay keeps, and machine-local state it must not.
        (root / 'CLAUDE.md').write_text('# Local Claude entry pointer\n')
        (root / 'opencode.json').write_text('{"share": "disabled"}\n')
        (root / '.claude' / 'worktrees' / 'agent-1').mkdir(parents=True)
        (root / '.claude' / 'worktrees' / 'agent-1' / 'big.txt').write_text('worktree copy')
        settings = {'permissions': {'allow': ['Bash(ls)', f'PowerShell(cd {root}; git status)',
                                              'Read(//c/Users/owner/**)']}}
        (root / '.claude' / 'settings.local.json').write_bytes(json.dumps(settings).encode() + b'\r\n')
        claude = machine.claude
        (claude / 'agents').mkdir(parents=True)
        (claude / 'agents' / 'arch-review.md').write_text('---\nname: arch-review\n---\nReview Cerid designs.\n')
        (claude / 'agents' / 'doc-writer.md').write_text('---\nname: doc-writer\n---\nUpdate status tables.\n')
        (claude / 'agents' / 'navcore-reviewer.md').write_text('---\nname: navcore-reviewer\n---\nAnother project.\n')
        (claude / 'crd-loop-watchdog.ps1').write_bytes(
            f"$log = '{claude}\\crd-loop-watchdog.log'\r\n# repository {root.as_posix()}\r\n".encode())
        (claude / 'crd-loop-watchdog.log').write_text('machine-local log\n')
        (root / '.claude' / 'scheduled_tasks.lock').write_text('{"pid": 1}')
        (root / '.agent').mkdir()
        config = {'schema': ws_mod.SCHEMA, 'code_remotes': ['host'], 'code_remote_exclude': ['worktree-*'],
                  'code_remote_exclude_tags': ['backup-*']}
        if generations:
            config['generations'] = generations
        (root / '.agent' / 'workspace.json').write_text(json.dumps(config))
        memory = machine.memory()
        memory.mkdir(parents=True)
        (memory / 'MEMORY.md').write_bytes(f'# memory\nEntry: [{root.as_posix()}/MEMORY.md]\n'.encode())
        (memory / 'lesson.md').write_text('a lesson\n')
        project = memory.parent
        (project / 'session-1.jsonl').write_text('{"transcript": true}\n')
        (project / 'session-1' / 'subagents').mkdir(parents=True)
        (project / 'session-1' / 'subagents' / 'agent.jsonl').write_text('{}\n')
        (project / 'session-1' / 'workflows' / 'scripts').mkdir(parents=True)
        (project / 'session-1' / 'workflows' / 'scripts' / 'serial.js').write_bytes(
            f'// {root.as_posix()}\r\nexport const a = 1\r\n'.encode())
        (memory / 'crlf-note.md').write_bytes(b'a note saved with CRLF\r\nsecond line\r\n')
        (machine.downloads / 'demo-1.0').mkdir(parents=True)
        (machine.downloads / 'demo-1.0' / 'demo-1.0.tar.gz').write_bytes(tool)
        (machine.downloads / 'unpinned.json').write_text('{}')

    def sync(self, machine, **kwargs):
        report = ws_mod.sync(machine.workspace(self.nas), **kwargs)
        return report, '\n'.join(report.lines)

    def manifest(self):
        return json.loads((self.nas / 'MANIFEST.json').read_text())

    def latest(self, kind):
        rel, entry = ws_mod.Nas(self.nas).latest(self.manifest(), kind)
        return self.nas / rel, entry

    def bundle_repo(self, kind):
        """A scratch bare repository holding the newest NAS bundle of `kind` (for reading its trees)."""
        path, _ = self.latest(kind)
        repo = self.base / f'read-{kind}-{len(list(self.base.glob("read-*")))}.git'
        git('init', '--quiet', '--bare', str(repo))
        git('--git-dir', str(repo), 'fetch', '--quiet', str(path), '+refs/heads/*:refs/heads/*',
            '+refs/tags/*:refs/tags/*')
        return repo

    def overlay_files(self):
        repo = self.bundle_repo('overlay')
        return set(git('--git-dir', str(repo), 'ls-tree', '-r', '--name-only', 'main').splitlines())

    def test_dry_run_lists_every_nas_file_and_writes_nothing(self):
        report, text = self.sync(self.a, dry_run=True)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('nothing was written', text)
        self.assertEqual(nas_listing(self.nas), {})
        self.assertFalse((self.a.root / '.agent-overlay').exists())
        self.assertFalse((self.a.root / '.agent' / 'claude-memory').exists())
        written = [line for line in report.lines if 'would write' in line]
        self.assertEqual(len(written), 4, text)
        self.assertRegex(written[0], r'/overlay/overlay-\d{8}T\d{6}Z\.bundle  [\d,]+ bytes$')
        self.assertRegex(written[1], r'/code/code-\d{8}T\d{6}Z\.bundle  [\d,]+ bytes$')
        self.assertIn('/tools/demo-1.0.tar.gz  26 bytes', written[2])
        self.assertIn('/MANIFEST.json', written[3])
        self.assertEqual(self.sync(self.a)[0].errors, 0)
        _, overlay = self.latest('overlay')
        planned = int(written[0].rsplit('  ', 1)[1].split()[0].replace(',', ''))
        self.assertLess(abs(overlay['size'] - planned), 64, 'the preview bundle matches the real one in size')

    def test_sync_writes_only_verified_single_files_and_is_idempotent(self):
        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 0, text)
        listing = nas_listing(self.nas)
        self.assertEqual(sorted(re.sub(r'\d{8}T\d{6}Z', 'T', p) for p in listing),
                         ['MANIFEST.json', 'code/code-T.bundle', 'overlay/overlay-T.bundle', 'tools/demo-1.0.tar.gz'])
        manifest = self.manifest()
        for rel, entry in manifest['files'].items():
            self.assertEqual(ws_mod.file_sha256(self.nas / rel), entry['sha256'], rel)
            self.assertEqual((self.nas / rel).stat().st_size, entry['size'], rel)
        self.assertEqual(self.overlay_files(), {
            'CLAUDE.md', 'opencode.json', '.claude/settings.local.json', '.agent/workspace.json',
            '.agent/claude-memory/MEMORY.md', '.agent/claude-memory/lesson.md', '.agent/claude-memory/crlf-note.md',
            '.agent/workflows/session-1/serial.js', '.agent/user-claude/agents/arch-review.md',
            '.agent/user-claude/agents/doc-writer.md', '.agent/user-claude/crd-loop-watchdog.ps1'})
        repo = self.bundle_repo('overlay')
        stored = git('--git-dir', str(repo), 'show', 'main:.agent/claude-memory/MEMORY.md')
        self.assertIn(ws_mod.ROOT_TOKEN + '/MEMORY.md', stored)
        watchdog = git('--git-dir', str(repo), 'show', 'main:.agent/user-claude/crd-loop-watchdog.ps1')
        self.assertIn('{{CLAUDE_HOME_NATIVE}}\\crd-loop-watchdog.log' if os.name == 'nt' else
                      '{{CLAUDE_HOME}}/crd-loop-watchdog.log', watchdog)
        self.assertNotIn(str(self.a.claude), watchdog)
        settings = subprocess.run(['git', '--git-dir', str(repo), 'show', 'main:.claude/settings.local.json'],
                                  stdout=subprocess.PIPE).stdout
        self.assertTrue(settings.endswith(b'\r\n'), 'agent files are stored byte-exact')
        _, code = self.latest('code')
        self.assertEqual(set(code['refs']), {'refs/heads/main', 'refs/heads/feature', 'refs/heads/worktree-agent-x',
                                             'refs/tags/v1', 'refs/tags/backup-before-strip'})
        self.assertEqual(code['head_branch'], 'main')
        self.assertEqual(set(ws_mod.ls_remote(self.host)), {'refs/heads/main', 'refs/heads/feature', 'refs/tags/v1'},
                         'excluded branches and tags stay off the code host; the NAS bundle keeps them')
        self.assertEqual(ws_mod.ls_remote(self.mirror), {}, 'only the configured code remotes are pushed')
        tool = manifest['files']['tools/demo-1.0.tar.gz']
        self.assertEqual(tool['sources'], ['demo-1.0/demo-1.0.tar.gz'])
        self.assertIn('unpinned.json', text)
        self.assertEqual(git('status', '--porcelain', cwd=self.a.root), '', 'overlay paths stay ignored by code')

        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('no changes', text)
        self.assertIn('nothing to write; the NAS is up to date', text)
        self.assertEqual(nas_listing(self.nas), listing, 'a second sync writes nothing to the NAS')
        status, differences = ws_mod.status(self.a.workspace(self.nas))
        self.assertEqual(differences, 0, '\n'.join(status.lines))

    def test_an_overlay_made_before_the_byte_exact_rule_is_renormalized(self):
        self.assertEqual(self.sync(self.a, push_code_remotes=False)[0].errors, 0)
        overlay = ws_mod.Overlay(self.a.root)
        (overlay.git_dir / 'info' / 'attributes').unlink()
        overlay.run('rm', '--cached', '--quiet', '--', '.claude/settings.local.json')
        overlay.run('add', '--force', '--', '.claude/settings.local.json')
        overlay.run('commit', '--quiet', '-m', 'stored the way an early overlay did')
        normalized = overlay.run('show', 'HEAD:.claude/settings.local.json').stdout
        self.assertFalse(normalized.endswith(b'\r\n'), 'the code .gitattributes converted it')
        report, text = self.sync(self.a, push_code_remotes=False)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('now stored byte-exact', text)
        repo = self.bundle_repo('overlay')
        stored = subprocess.run(['git', '--git-dir', str(repo), 'show', 'main:.claude/settings.local.json'],
                                stdout=subprocess.PIPE).stdout
        self.assertEqual(stored, (self.a.root / '.claude' / 'settings.local.json').read_bytes())

    def test_sync_refuses_secrets_and_transcripts_before_writing(self):
        (self.a.memory() / 'leak.md').write_text('token ' + FAKE['GitHub token'] + '\n')
        (self.a.memory() / 'chat.jsonl').write_text('{}\n')
        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 2, text)
        self.assertNotIn(FAKE['GitHub token'], text)
        self.assertEqual(nas_listing(self.nas), {})
        self.assertFalse((self.a.root / '.agent-overlay').exists())

    def test_a_copy_that_reads_back_wrong_is_deleted_and_fails_loudly(self):
        self.assertEqual(self.sync(self.a, push_code_remotes=False)[0].errors, 0)
        before = nas_listing(self.nas)
        (self.a.memory() / 'new.md').write_text('new lesson\n')
        real_copy = shutil.copyfileobj

        def corrupting(reader, writer, length=0):
            real_copy(reader, writer, length)
            writer.write(b'!')
        with mock.patch.object(ws_mod.shutil, 'copyfileobj', corrupting):
            with self.assertRaisesRegex(ws_mod.WorkspaceError, 'does not match its SHA-256'):
                self.sync(self.a, push_code_remotes=False)
        self.assertEqual(nas_listing(self.nas), before, 'only the .partial was written, and it was deleted')
        self.assertEqual(self.sync(self.a, push_code_remotes=False)[0].errors, 0)

    def test_one_writer_at_a_time(self):
        lock = self.nas / 'crd.lock'
        lock.write_text(json.dumps({'host': 'other-machine', 'pid': 4242, 'started': '2026-10-31T09:00:00+00:00'}))
        with self.assertRaisesRegex(ws_mod.WorkspaceError, 'locked by other-machine'):
            self.sync(self.a)
        self.assertEqual(nas_listing(self.nas), {'crd.lock': nas_listing(self.nas)['crd.lock']})
        with self.assertRaisesRegex(ws_mod.WorkspaceError, 'other-machine is writing'):
            ws_mod.restore(self.nas, self.base / 'restore', self.base / 'c', self.base / 'd')
        dead = subprocess.Popen([sys.executable, '-c', 'pass'])
        dead.wait()
        lock.write_text(json.dumps({'host': ws_mod.platform.node(), 'pid': dead.pid, 'started': 'earlier'}))
        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('removed a stale lock', text)
        self.assertFalse(lock.exists(), 'the lock is released after the run')

    def test_free_space_is_checked_before_any_write(self):
        usage = shutil.disk_usage(self.nas)._replace(free=10)
        with mock.patch.object(ws_mod.shutil, 'disk_usage', return_value=usage):
            with self.assertRaisesRegex(ws_mod.WorkspaceError, 'needs twice that free'):
                self.sync(self.a)
        self.assertEqual(nas_listing(self.nas), {})

    def test_paths_never_resolve_outside_the_nas_folder(self):
        nas = ws_mod.Nas(self.nas)
        for rel in ('../neighbour.txt', '..', '.', 'code/../../neighbour.txt'):
            with self.assertRaisesRegex(ws_mod.WorkspaceError, 'outside the NAS folder'):
                nas.path(rel)
        if os.name == 'nt':
            import _winapi  # a junction needs no privilege, unlike a Windows symlink
            _winapi.CreateJunction(str(self.base / 'nas'), str(self.nas / 'code'))
        else:
            os.symlink(self.base / 'nas', self.nas / 'code', target_is_directory=True)
        with self.assertRaisesRegex(ws_mod.WorkspaceError, 'outside the NAS folder'):
            self.sync(self.a)
        self.assertEqual(sorted(p.name for p in (self.base / 'nas').iterdir()), ['crd', 'neighbour.txt'])
        if os.name == 'nt':
            os.rmdir(self.nas / 'code')
        else:
            os.remove(self.nas / 'code')
        with self.assertRaisesRegex(ws_mod.WorkspaceError, 'does not exist'):
            ws_mod.Nas(self.base / 'nas' / 'missing')

    def test_generations_are_pruned_only_among_recorded_files(self):
        remove_tree(self.a.base)
        self.make_checkout(self.a, generations=2)
        (self.nas / 'code').mkdir()
        (self.nas / 'code' / 'kept-by-the-user.bundle').write_text('not ours')
        for round_number in range(4):
            (self.a.root / 'docs' / f'r{round_number}.md').write_text(f'{round_number}\n')
            git('add', 'docs', cwd=self.a.root)
            git('commit', '--quiet', '-m', f'round {round_number}', cwd=self.a.root)
            (self.a.memory() / f'r{round_number}.md').write_text(f'{round_number}\n')
            report, text = self.sync(self.a, push_code_remotes=False)
            self.assertEqual(report.errors, 0, text)
        kinds = [ws_mod.Nas(self.nas).latest(self.manifest(), k) for k in ('code', 'overlay')]
        code = sorted(p.name for p in (self.nas / 'code').iterdir())
        self.assertEqual(len(code), 3, code)
        self.assertIn('kept-by-the-user.bundle', code)
        self.assertEqual(len(list((self.nas / 'overlay').iterdir())), 2)
        self.assertEqual(len([e for e in self.manifest()['files'].values() if e['kind'] == 'code']), 2)
        self.assertTrue(all(rel for rel, _ in kinds))
        self.assertIn('pruned code/code-', text)

    def test_code_hosts_never_receive_new_agent_files(self):
        root = self.a.root
        git('checkout', '--quiet', '-b', 'leak', cwd=root)
        git('add', '--force', 'CLAUDE.md', cwd=root)
        git('commit', '--quiet', '-m', 'add agent file', cwd=root)
        git('checkout', '--quiet', 'main', cwd=root)
        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 1, text)
        self.assertIn('leak not pushed: new commits touch agent files (CLAUDE.md)', text)
        host_refs = ws_mod.ls_remote(self.host)
        self.assertNotIn('refs/heads/leak', host_refs)
        self.assertIn('refs/heads/feature', host_refs)
        self.assertIn('refs/heads/leak', self.latest('code')[1]['refs'], 'the private NAS bundle has it')

    def test_a_refused_current_branch_stays_off_the_code_hosts_when_nothing_else_is_pushed(self):
        root = self.a.root
        git('branch', '--quiet', '-D', 'feature', cwd=root)
        git('tag', '-d', 'v1', 'backup-before-strip', cwd=root)
        (root / 'notes.txt').write_text('remote = ' + FAKE['credentials in a URL'] + '\n')
        git('add', 'notes.txt', cwd=root)
        git('commit', '--quiet', '-m', 'a URL with credentials', cwd=root)
        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 1, text)
        self.assertIn('main not pushed: new commits add text shaped like: credentials in a URL', text)
        self.assertNotIn('refs/heads/main', ws_mod.ls_remote(self.host),
                         'an empty refspec list must not fall back to pushing the current branch')

    def test_a_commit_that_untracks_agent_files_reaches_the_code_hosts(self):
        root = self.a.root
        git('checkout', '--quiet', '-b', 'cleanup', cwd=root)
        git('add', '--force', 'CLAUDE.md', cwd=root)
        git('commit', '--quiet', '-m', 'history from before the overlay', cwd=root)
        git('push', '--quiet', 'host', 'cleanup', cwd=root)
        git('rm', '--cached', '--quiet', 'CLAUDE.md', cwd=root)
        git('commit', '--quiet', '-m', 'stop tracking the agent entry file', cwd=root)
        git('checkout', '--quiet', 'main', cwd=root)
        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 0, text)
        self.assertEqual(ws_mod.ls_remote(self.host)['refs/heads/cleanup'], git('rev-parse', 'cleanup', cwd=root))

    def test_an_adopted_configuration_decides_the_code_hosts_of_that_first_run(self):
        self.assertEqual(self.sync(self.a, push_code_remotes=False)[0].errors, 0)
        b = Machine(self.base, 'b')
        b.root.mkdir(parents=True)
        git('clone', '--quiet', str(self.a.root), str(b.root))
        git('remote', 'add', 'host', str(self.host), cwd=b.root)
        git('remote', 'add', 'mirror', str(self.mirror), cwd=b.root)
        report, text = self.sync(b)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('refs/heads/main', ws_mod.ls_remote(self.host))
        self.assertEqual(ws_mod.ls_remote(self.mirror), {}, 'the NAS configuration lists only "host"')

    def test_a_new_checkout_adopts_the_nas_overlay_without_losing_either_side(self):
        self.assertEqual(self.sync(self.a, push_code_remotes=False)[0].errors, 0)
        b = Machine(self.base, 'b')
        b.root.mkdir(parents=True)
        git('clone', '--quiet', str(self.a.root), str(b.root))
        (b.root / 'CLAUDE.md').write_text('# Local Claude entry pointer\n')
        memory = b.memory()
        memory.mkdir(parents=True)
        (memory / 'lesson.md').write_text('a lesson, newer on b\n')
        (memory / 'b-only.md').write_text('only on b\n')
        report, text = self.sync(b, push_code_remotes=False)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('adopted the NAS overlay history', text)
        self.assertTrue((b.root / '.agent' / 'workspace.json').is_file(), 'the NAS config reaches the new checkout')
        self.assertEqual((memory / 'MEMORY.md').read_text(), f'# memory\nEntry: [{b.root.resolve().as_posix()}/MEMORY.md]\n')
        self.assertEqual((memory / 'lesson.md').read_text(), 'a lesson, newer on b\n', 'live notes are not overwritten')
        files = self.overlay_files()
        self.assertTrue({'.agent/workspace.json', '.agent/claude-memory/MEMORY.md', '.agent/claude-memory/b-only.md',
                         'opencode.json', '.claude/settings.local.json'} <= files, files)
        repo = self.bundle_repo('overlay')
        self.assertEqual(git('--git-dir', str(repo), 'show', 'main:.agent/claude-memory/lesson.md'),
                         'a lesson, newer on b')

    def test_nas_code_moves_on_only_from_a_checkout_that_knew_its_tips(self):
        root = self.a.root
        self.assertEqual(self.sync(self.a, push_code_remotes=False)[0].errors, 0)
        git('checkout', '--quiet', 'feature', cwd=root)
        (root / 'docs' / 'f.md').write_text('feature\n')
        git('add', 'docs/f.md', cwd=root)
        git('commit', '--quiet', '-m', 'feature work', cwd=root)
        self.assertEqual(self.sync(self.a, push_code_remotes=False)[0].errors, 0)
        git('commit', '--quiet', '--amend', '-m', 'feature work, reworded', cwd=root)
        report, text = self.sync(self.a, push_code_remotes=False)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('refs/heads/feature (rewritten here', text)
        b = Machine(self.base, 'b')
        self.assertEqual(ws_mod.restore(self.nas, b.root, b.claude, b.downloads).errors, 0)
        git('checkout', '--quiet', 'feature', cwd=b.root)
        (b.root / 'docs' / 'g.md').write_text('elsewhere\n')
        git('add', 'docs/g.md', cwd=b.root)
        git('commit', '--quiet', '-m', 'work from another machine', cwd=b.root)
        self.assertEqual(self.sync(b, push_code_remotes=False)[0].errors, 0)
        git('commit', '--quiet', '--amend', '-m', 'feature work, reworded again', cwd=root)
        before = nas_listing(self.nas)
        report, text = self.sync(self.a, push_code_remotes=False)
        self.assertEqual(report.errors, 1, text)
        self.assertIn('refs/heads/feature: the NAS holds commits this checkout has never seen', text)
        self.assertEqual({k for k in nas_listing(self.nas) if k.startswith('code/')},
                         {k for k in before if k.startswith('code/')}, 'no code bundle drops the other machine')

    def test_export_to_the_nas_restores_through_its_own_entry_point_reading_only(self):
        self.assertEqual(self.sync(self.a)[0].errors, 0)
        report, archive = ws_mod.export(self.a.workspace(self.nas))
        self.assertEqual(report.errors, 0, '\n'.join(report.lines))
        self.assertEqual(archive.parent, self.nas / 'exports')
        self.assertRegex(archive.name, r'^crd-workspace-\d{8}\.zip$')
        entry = self.manifest()['files']['exports/' + archive.name]
        self.assertEqual((entry['kind'], entry['sha256']), ('export', ws_mod.file_sha256(archive)))
        self.assertEqual(ws_mod.verify_archive(archive).errors, 0)
        before = nas_listing(self.nas)
        b = Machine(self.base, 'b')
        (b.claude / 'agents').mkdir(parents=True)
        (b.claude / 'agents' / 'arch-review.md').write_text('the friend\'s own reviewer\n')
        destination = self.base / 'friend folder' / 'crd'
        result = subprocess.run([sys.executable, str(archive), '--claude-config-dir', str(b.claude),
                                 '--downloads-dir', str(b.downloads), 'restore', str(destination)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertIn('[next]', result.stdout.decode())
        self.assertEqual(nas_listing(self.nas), before, 'a restore never writes to the NAS')
        destination = destination.resolve()
        for ref in ('refs/heads/main', 'refs/heads/feature', 'refs/tags/v1'):
            self.assertEqual(git('rev-parse', ref, cwd=destination), git('rev-parse', ref, cwd=self.a.root))
        self.assertEqual(git('symbolic-ref', '--short', 'HEAD', cwd=destination), 'main')
        self.assertEqual(git('remote', 'get-url', 'host', cwd=destination), str(self.host))
        self.assertEqual(git('status', '--porcelain', cwd=destination), '')
        for name in ('CLAUDE.md', 'opencode.json'):
            self.assertEqual((destination / name).read_bytes(), (self.a.root / name).read_bytes())
        shared = json.loads((destination / '.claude' / 'settings.local.json').read_text())
        self.assertEqual(shared['permissions']['allow'], ['Bash(ls)', f'PowerShell(cd {destination}; git status)'])
        history = git('--git-dir', str(destination / '.agent-overlay'), 'log', '-p', '--all')
        self.assertNotIn('Users/owner', history, 'the shared history never held the owner\'s rules')
        self.assertNotIn('"nas"', (destination / '.agent' / 'workspace.json').read_text())
        self.assertEqual((b.claude / 'agents' / 'arch-review.md').read_bytes(),
                         (self.a.claude / 'agents' / 'arch-review.md').read_bytes())
        backups = list((b.claude / 'agents').glob('arch-review.md.before-workspace-*'))
        self.assertEqual([p.read_text() for p in backups], ['the friend\'s own reviewer\n'])
        self.assertIn('arch-review.md differed', result.stdout.decode())
        self.assertTrue((b.claude / 'agents' / 'doc-writer.md').is_file())
        self.assertFalse((b.claude / 'agents' / 'navcore-reviewer.md').exists())
        self.assertFalse((b.claude / 'crd-loop-watchdog.log').exists())
        self.assertEqual((b.claude / 'crd-loop-watchdog.ps1').read_bytes(),
                         f"$log = '{b.claude}\\crd-loop-watchdog.log'\r\n# repository "
                         f"{destination.as_posix()}\r\n".encode())
        self.assertFalse((destination / '.claude' / 'worktrees').exists())
        memory = b.memory(destination)
        self.assertEqual((memory / 'MEMORY.md').read_text(), f'# memory\nEntry: [{destination.as_posix()}/MEMORY.md]\n')
        self.assertEqual((memory / 'lesson.md').read_text(), 'a lesson\n')
        flow = memory.parent / 'session-1' / 'workflows' / 'scripts' / 'serial.js'
        self.assertEqual(flow.read_bytes(), f'// {destination.as_posix()}\r\nexport const a = 1\r\n'.encode())
        self.assertEqual((memory / 'crlf-note.md').read_bytes(), b'a note saved with CRLF\r\nsecond line\r\n')
        self.assertFalse((memory.parent / 'session-1.jsonl').exists())
        self.assertEqual((b.downloads / 'demo-1.0' / 'demo-1.0.tar.gz').read_bytes(), self.a.tool)
        restored = ws_mod.Workspace(destination, self.nas, b.claude, b.downloads)
        report, _ = ws_mod.status(restored, offline=True)
        self.assertEqual([l for l in report.lines if l.startswith('[overlay]   ')],
                         ['[overlay]   .claude/settings.local.json'], 'only the adapted settings await a first sync')
        with self.assertRaisesRegex(ws_mod.WorkspaceError, 'not empty'):
            ws_mod.restore(archive, destination, b.claude, b.downloads)
        local = self.base / 'local-exports'
        report, copy = ws_mod.export(self.a.workspace(self.nas), local, include_local_settings=True)
        self.assertEqual((report.errors, copy.parent), (0, local))
        self.assertEqual(nas_listing(self.nas), before, 'an export to a local folder leaves the NAS alone')
        c = Machine(self.base, 'c')
        self.assertEqual(ws_mod.restore(copy, c.root, c.claude, c.downloads).errors, 0)
        self.assertEqual((c.root / '.claude' / 'settings.local.json').read_bytes(),
                         (self.a.root / '.claude' / 'settings.local.json').read_bytes(), 'the owner copy is verbatim')

    def test_nas_restore_then_two_machines_merge_their_memory(self):
        self.assertEqual(self.sync(self.a)[0].errors, 0)
        before = nas_listing(self.nas)
        b = Machine(self.base, 'b')
        report = ws_mod.restore(self.nas, b.root, b.claude, b.downloads)
        self.assertEqual(report.errors, 0, '\n'.join(report.lines))
        self.assertEqual(nas_listing(self.nas), before, 'a restore never writes to the NAS')
        b.root = b.root.resolve()
        (b.memory() / 'from-b.md').write_text('learned on b\n')
        report, text = self.sync(b, push_code_remotes=False)
        self.assertEqual(report.errors, 0, text)
        (self.a.memory() / 'from-a.md').write_text('learned on a\n')
        report, text = self.sync(self.a, push_code_remotes=False)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('merged the NAS overlay', text)
        self.assertEqual((self.a.memory() / 'from-b.md').read_text(), 'learned on b\n')
        self.assertTrue({'.agent/claude-memory/from-a.md', '.agent/claude-memory/from-b.md'} <= self.overlay_files())
        self.assertIn(f'[{self.a.root.as_posix()}/MEMORY.md]', (self.a.memory() / 'MEMORY.md').read_text())

    def test_conflicting_memory_edits_stop_the_sync_until_resolved(self):
        self.assertEqual(self.sync(self.a)[0].errors, 0)
        b = Machine(self.base, 'b')
        self.assertEqual(ws_mod.restore(self.nas, b.root, b.claude, b.downloads).errors, 0)
        b.root = b.root.resolve()
        (b.memory() / 'lesson.md').write_text('a lesson, as b sees it\n')
        self.assertEqual(self.sync(b, push_code_remotes=False)[0].errors, 0)
        (self.a.memory() / 'lesson.md').write_text('a lesson, as a sees it\n')
        before = nas_listing(self.nas)
        report, text = self.sync(self.a, push_code_remotes=False)
        self.assertEqual(report.errors, 1, text)
        self.assertIn('.agent/claude-memory/lesson.md', text)
        self.assertEqual(nas_listing(self.nas), before, 'nothing reaches the NAS while the merge is open')
        conflicted = self.a.root / '.agent' / 'claude-memory' / 'lesson.md'
        self.assertIn('<<<<<<<', conflicted.read_text())
        report, text = self.sync(self.a, push_code_remotes=False)
        self.assertEqual(report.errors, 1, text)
        self.assertIn('still has conflict markers', text)
        conflicted.write_text('a lesson, as both see it\n')
        report, text = self.sync(self.a, push_code_remotes=False)
        self.assertEqual(report.errors, 0, text)
        self.assertEqual((self.a.memory() / 'lesson.md').read_text(), 'a lesson, as both see it\n')
        repo = self.bundle_repo('overlay')
        self.assertEqual(git('--git-dir', str(repo), 'show', 'main:.agent/claude-memory/lesson.md'),
                         'a lesson, as both see it')
        self.assertEqual(self.sync(b, push_code_remotes=False)[0].errors, 0)
        self.assertEqual((b.memory() / 'lesson.md').read_text(), 'a lesson, as both see it\n')

    def test_export_refuses_confidential_material(self):
        root = self.a.root
        git('checkout', '--quiet', '-b', 'console', cwd=root)
        (root / 'docs' / 'console').mkdir()
        (root / 'docs' / 'console' / ws_mod.CONFIDENTIAL_MARKER).write_text('NDA\n')
        git('add', 'docs/console', cwd=root)
        git('commit', '--quiet', '-m', 'console notes', cwd=root)
        git('checkout', '--quiet', 'main', cwd=root)
        self.assertEqual(self.sync(self.a, push_code_remotes=False)[0].errors, 0)
        before = nas_listing(self.nas)
        report, archive = ws_mod.export(self.a.workspace(self.nas))
        self.assertIsNone(archive)
        self.assertIn('marked confidential', '\n'.join(report.lines))
        self.assertEqual(nas_listing(self.nas), before)


if __name__ == '__main__':
    unittest.main()
