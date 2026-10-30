#!/usr/bin/env python3
"""Tests for scripts/workspace.py: the agent overlay, NAS sync, export and restore (no network, no build).

Synthetic checkouts in temporary folders stand in for the code repository, the NAS, a code host, the Claude Code
configuration folder and the pinned download cache, so every path runs on any host with Git and Python 3.12+:
the Claude Code project-key derivation against names observed on a real machine; refusal of transcripts,
credential files and secret token shapes (with harmless look-alikes accepted); the code repository's ignore rules
for every agent path; a dry run that writes nothing; an idempotent sync; the code-host checks (excluded branches,
commits that touch agent files); the NAS rewrite lease; an export restored through the archive's own entry point
into a new folder (refs, files, overlay, memory with path placeholders, workflow scripts and tool downloads); a
restore from the NAS followed by a two-machine overlay merge; and an export refused for a confidential marker.
Run: python scripts/test-workspace.py
"""
import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
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

    def test_root_placeholders_round_trip_byte_exact(self):
        root = Path('D:/Dev/cerid') if os.name == 'nt' else Path('/home/u/cerid')
        original = (f'See {root.as_posix()}/MEMORY.md and {ws_mod.native_root(root)}\\docs\\x.md; '
                    f'{root.as_posix()}x stays.\n').encode()
        stored = ws_mod.collapse_root(original, root)
        self.assertNotIn(root.as_posix().encode(), stored)
        self.assertIn(ws_mod.ROOT_TOKEN.encode(), stored)
        self.assertEqual(ws_mod.expand_root(stored, root), original)
        moved = ws_mod.expand_root(stored, Path('/srv/friend/crd'))
        self.assertIn(b'/srv/friend/crd/MEMORY.md', moved)
        self.assertEqual(ws_mod.collapse_root(b'\xff\xfe binary', root), b'\xff\xfe binary')


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
        (self.base / 'nas').mkdir()
        self.host = self.base / 'host.git'
        git('init', '--quiet', '--bare', '-b', 'main', str(self.host))
        self.a = Machine(self.base, 'a')
        self.make_checkout(self.a)

    def tearDown(self):
        for key, value in self.saved_env.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        remove_tree(self.base)

    def make_checkout(self, machine):
        root = machine.root
        root.mkdir(parents=True)
        git('init', '--quiet', '-b', 'main', cwd=root)
        git('config', 'core.autocrlf', 'false', cwd=root)
        shutil.copyfile(ROOT / '.gitignore', root / '.gitignore')
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
        git('remote', 'add', 'host', str(self.host), cwd=root)
        # Agent files the overlay keeps, and machine-local state it must not.
        (root / 'CLAUDE.md').write_text('# Local Claude entry pointer\n')
        (root / 'opencode.json').write_text('{"share": "disabled"}\n')
        (root / '.claude' / 'worktrees' / 'agent-1').mkdir(parents=True)
        (root / '.claude' / 'worktrees' / 'agent-1' / 'big.txt').write_text('worktree copy')
        (root / '.claude' / 'settings.local.json').write_bytes(b'{"permissions": {"allow": ["Bash(ls)"]}}\r\n')
        (root / '.claude' / 'scheduled_tasks.lock').write_text('{"pid": 1}')
        (root / '.agent').mkdir()
        (root / '.agent' / 'workspace.json').write_text(json.dumps({
            'schema': ws_mod.SCHEMA, 'code_remotes': ['host'], 'code_remote_exclude': ['worktree-*']}))
        memory = machine.memory()
        memory.mkdir(parents=True)
        (memory / 'MEMORY.md').write_bytes(f'# memory\nEntry: [{root.as_posix()}/MEMORY.md]\n'.encode())
        (memory / 'lesson.md').write_text('a lesson\n')
        project = memory.parent
        (project / 'session-1.jsonl').write_text('{"transcript": true}\n')
        (project / 'session-1' / 'subagents').mkdir(parents=True)
        (project / 'session-1' / 'subagents' / 'agent.jsonl').write_text('{}\n')
        (project / 'session-1' / 'workflows' / 'scripts').mkdir(parents=True)
        (project / 'session-1' / 'workflows' / 'scripts' / 'serial.js').write_text(f'// {root.as_posix()}\n')
        (machine.downloads / 'demo-1.0').mkdir(parents=True)
        (machine.downloads / 'demo-1.0' / 'demo-1.0.tar.gz').write_bytes(tool)
        (machine.downloads / 'unpinned.json').write_text('{}')

    def sync(self, machine, **kwargs):
        report = ws_mod.sync(machine.workspace(self.nas), **kwargs)
        return report, '\n'.join(report.lines)

    def overlay_files(self, nas_overlay):
        return set(git('--git-dir', str(nas_overlay), 'ls-tree', '-r', '--name-only', 'main').splitlines())

    def test_dry_run_writes_nothing_and_sync_is_idempotent(self):
        report, text = self.sync(self.a, dry_run=True)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('nothing was written', text)
        self.assertFalse(self.nas.exists())
        self.assertFalse((self.a.root / '.agent-overlay').exists())
        self.assertFalse((self.a.root / '.agent' / 'claude-memory').exists())

        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 0, text)
        files = self.overlay_files(self.nas / 'overlay.git')
        self.assertEqual(files, {'CLAUDE.md', 'opencode.json', '.claude/settings.local.json', '.agent/workspace.json',
                                 '.agent/claude-memory/MEMORY.md', '.agent/claude-memory/lesson.md',
                                 '.agent/workflows/session-1/serial.js'})
        stored = git('--git-dir', str(self.nas / 'overlay.git'), 'show', 'main:.agent/claude-memory/MEMORY.md')
        self.assertIn(ws_mod.ROOT_TOKEN + '/MEMORY.md', stored)
        settings = subprocess.run(['git', '--git-dir', str(self.nas / 'overlay.git'), 'show',
                                   'main:.claude/settings.local.json'], stdout=subprocess.PIPE).stdout
        self.assertTrue(settings.endswith(b'\r\n'), 'agent files are stored byte-exact')
        nas_refs = ws_mod.ls_remote(self.nas / 'code.git')
        self.assertEqual(set(nas_refs), {'refs/heads/main', 'refs/heads/feature', 'refs/heads/worktree-agent-x',
                                         'refs/tags/v1'})
        host_refs = ws_mod.ls_remote(self.host)
        self.assertEqual(set(host_refs), {'refs/heads/main', 'refs/heads/feature', 'refs/tags/v1'})
        tools = json.loads((self.nas / 'tools' / 'manifest.json').read_text())['entries']
        self.assertEqual([(e['store'], e['sources']) for e in tools], [('tools/demo/demo-1.0.tar.gz',
                                                                         ['demo-1.0/demo-1.0.tar.gz'])])
        self.assertIn('unpinned.json', text)
        self.assertEqual(git('status', '--porcelain', cwd=self.a.root), '', 'overlay paths stay ignored by code')

        head = git('--git-dir', str(self.nas / 'overlay.git'), 'rev-parse', 'main')
        marker = (self.nas / 'crd-nas.json').read_bytes()
        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('no changes', text)
        self.assertIn('NAS overlay already up to date', text)
        self.assertIn('all 4 branches and tags up to date', text)
        self.assertNotIn('pushed', text.replace('NAS overlay', ''))
        self.assertEqual(git('--git-dir', str(self.nas / 'overlay.git'), 'rev-parse', 'main'), head)
        self.assertEqual((self.nas / 'crd-nas.json').read_bytes(), marker)
        status, differences = ws_mod.status(self.a.workspace(self.nas))
        self.assertEqual(differences, 0, '\n'.join(status.lines))

    def test_sync_refuses_secrets_and_transcripts_before_writing(self):
        (self.a.memory() / 'leak.md').write_text('token ' + FAKE['GitHub token'] + '\n')
        (self.a.memory() / 'chat.jsonl').write_text('{}\n')
        report, text = self.sync(self.a)
        self.assertEqual(report.errors, 2, text)
        self.assertNotIn(FAKE['GitHub token'], text)
        self.assertFalse(self.nas.exists())
        self.assertFalse((self.a.root / '.agent-overlay').exists())

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
        self.assertIn('refs/heads/leak', ws_mod.ls_remote(self.nas / 'code.git'), 'the private NAS mirror has it')

    def test_nas_ref_rewrites_need_a_tip_this_checkout_knew(self):
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
        other = self.base / 'other'
        git('clone', '--quiet', '--branch', 'feature', str(self.nas / 'code.git'), str(other))
        (other / 'docs' / 'g.md').write_text('elsewhere\n')
        git('add', 'docs/g.md', cwd=other)
        git('commit', '--quiet', '-m', 'work from another machine', cwd=other)
        git('push', '--quiet', 'origin', 'feature', cwd=other)
        git('commit', '--quiet', '--amend', '-m', 'feature work, reworded again', cwd=root)
        report, text = self.sync(self.a, push_code_remotes=False)
        self.assertEqual(report.errors, 1, text)
        self.assertIn('refs/heads/feature: the NAS holds commits this checkout has never seen', text)

    def test_export_restores_through_its_own_entry_point(self):
        self.assertEqual(self.sync(self.a)[0].errors, 0)
        out_dir = self.base / 'exports'
        report, archive = ws_mod.export(self.a.workspace(self.nas), out_dir)
        self.assertEqual(report.errors, 0, '\n'.join(report.lines))
        self.assertRegex(archive.name, r'^crd-workspace-\d{8}\.zip$')
        self.assertTrue(archive.with_name(archive.name + '.sha256').is_file())
        self.assertEqual(ws_mod.verify_archive(archive).errors, 0)
        b = Machine(self.base, 'b')
        destination = self.base / 'friend folder' / 'crd'
        result = subprocess.run([sys.executable, str(archive), '--claude-config-dir', str(b.claude),
                                 '--downloads-dir', str(b.downloads), 'restore', str(destination)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertIn('[next]', result.stdout.decode())
        destination = destination.resolve()
        for ref in ('refs/heads/main', 'refs/heads/feature', 'refs/tags/v1'):
            self.assertEqual(git('rev-parse', ref, cwd=destination), git('rev-parse', ref, cwd=self.a.root))
        self.assertEqual(git('symbolic-ref', '--short', 'HEAD', cwd=destination), 'main')
        self.assertEqual(git('remote', 'get-url', 'host', cwd=destination), str(self.host))
        self.assertEqual(git('status', '--porcelain', cwd=destination), '')
        for name in ('CLAUDE.md', 'opencode.json', '.claude/settings.local.json'):
            self.assertEqual((destination / name).read_bytes(), (self.a.root / name).read_bytes())
        self.assertFalse((destination / '.claude' / 'worktrees').exists())
        memory = b.memory(destination)
        self.assertEqual((memory / 'MEMORY.md').read_text(), f'# memory\nEntry: [{destination.as_posix()}/MEMORY.md]\n')
        self.assertEqual((memory / 'lesson.md').read_text(), 'a lesson\n')
        flow = memory.parent / 'session-1' / 'workflows' / 'scripts' / 'serial.js'
        self.assertEqual(flow.read_text(), f'// {destination.as_posix()}\n')
        self.assertFalse((memory.parent / 'session-1.jsonl').exists())
        self.assertEqual((b.downloads / 'demo-1.0' / 'demo-1.0.tar.gz').read_bytes(), self.a.tool)
        restored = ws_mod.Workspace(destination, self.nas, b.claude, b.downloads)
        report, differences = ws_mod.status(restored, offline=True)
        self.assertNotIn('changed', '\n'.join(l for l in report.lines if l.startswith('[overlay]')))
        with self.assertRaisesRegex(ws_mod.WorkspaceError, 'not empty'):
            ws_mod.restore(archive, destination, b.claude, b.downloads)

    def test_nas_restore_then_two_machines_merge_their_memory(self):
        self.assertEqual(self.sync(self.a)[0].errors, 0)
        b = Machine(self.base, 'b')
        destination = (self.base / 'b' / 'crd')
        report = ws_mod.restore(self.nas, destination, b.claude, b.downloads)
        self.assertEqual(report.errors, 0, '\n'.join(report.lines))
        b.root = destination.resolve()
        (b.memory() / 'from-b.md').write_text('learned on b\n')
        report, text = self.sync(b, push_code_remotes=False)
        self.assertEqual(report.errors, 0, text)
        (self.a.memory() / 'from-a.md').write_text('learned on a\n')
        report, text = self.sync(self.a, push_code_remotes=False)
        self.assertEqual(report.errors, 0, text)
        self.assertIn('merged the NAS overlay', text)
        self.assertEqual((self.a.memory() / 'from-b.md').read_text(), 'learned on b\n')
        self.assertTrue({'.agent/claude-memory/from-a.md', '.agent/claude-memory/from-b.md'} <=
                        self.overlay_files(self.nas / 'overlay.git'))
        self.assertIn(f'[{self.a.root.as_posix()}/MEMORY.md]', (self.a.memory() / 'MEMORY.md').read_text())

    def test_export_refuses_confidential_material(self):
        root = self.a.root
        git('checkout', '--quiet', '-b', 'console', cwd=root)
        (root / 'docs' / 'console').mkdir()
        (root / 'docs' / 'console' / ws_mod.CONFIDENTIAL_MARKER).write_text('NDA\n')
        git('add', 'docs/console', cwd=root)
        git('commit', '--quiet', '-m', 'console notes', cwd=root)
        git('checkout', '--quiet', 'main', cwd=root)
        self.assertEqual(self.sync(self.a, push_code_remotes=False)[0].errors, 0)
        report, archive = ws_mod.export(self.a.workspace(self.nas), self.base / 'exports')
        self.assertIsNone(archive)
        self.assertIn('marked confidential', '\n'.join(report.lines))
        self.assertFalse((self.base / 'exports').exists())


if __name__ == '__main__':
    unittest.main()
