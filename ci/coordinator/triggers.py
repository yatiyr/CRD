"""Polling and scheduled triggers of the Cerid orchestrator, and the bare mirror runners fetch through.

Polling is the fallback that needs no host configuration: `git ls-remote` of each repository's watched branches on
an interval; a new head is a push build (the trigger key makes a push seen by both a webhook and the poller build
once). Schedules are cron expressions in the configuration; each fires once per matching minute, recorded with
compare-and-swap so a restarted coordinator neither misses (within the catch-up window) nor repeats one.

The mirror (`checkout: mirror`) keeps a bare clone per repository on the coordinator, fetched with the coordinator
machine's own git credentials (a read-only deploy key), pins every build's commit as `refs/cerid/builds/<n>` and
serves fetches through `git http-backend` (upload-pack only) to agents holding that build's job token. Runners then
need no repository credential at all, and a shallow fetch of one commit stays small.
"""
from __future__ import annotations

from pathlib import Path
import os
import re
import subprocess

from coordinator import hosts

CATCH_UP_SECONDS = 6 * 3600


def poll(coordinator, run=subprocess.run, now=None):
    """Builds for new heads of every polled repository. Returns the builds created."""
    now = coordinator.clock() if now is None else now
    created = []
    for name, repo in coordinator.repositories.items():
        spec = repo.get('poll')
        if not spec:
            continue
        state_name = 'heads/%s.json' % name
        heads_state = coordinator.get(state_name) or {}
        if now - heads_state.get('polled', 0) < float(spec.get('interval_seconds', 60)):
            continue
        try:
            heads = hosts.ls_remote(repo['url'], spec.get('branches', ['main']), run=run)
        except Exception as error:
            coordinator.log('poll_failed', level='warning', repository=name, error=str(error)[:300])
            coordinator.update(state_name, lambda c: dict(c or {}, polled=now))
            continue
        previous = heads_state.get('heads', {})
        for branch, commit in sorted(heads.items()):
            if previous.get(branch) == commit or name not in coordinator.repositories:
                continue
            if branch not in previous and not spec.get('build_on_first_poll', False):
                continue  # the first poll learns the heads; it does not rebuild history
            build, made = coordinator.create_build(name, {'event': 'push', 'branch': branch, 'commit': commit,
                                                          'before': previous.get(branch, ''), 'trusted': True},
                                                   'poll')
            if made:
                created.append(build)
        coordinator.update(state_name, lambda c: {'heads': dict((c or {}).get('heads', {}), **heads), 'polled': now})
    return created


def schedules(coordinator, run=subprocess.run, now=None):
    """Builds for every schedule whose latest matching minute has not fired yet. Returns the builds created."""
    now = coordinator.clock() if now is None else now
    created = []
    for name, repo in coordinator.repositories.items():
        for entry in repo.get('schedules', []):
            cron = hosts.Cron(entry['cron'])
            due = cron.last(now, horizon=CATCH_UP_SECONDS)
            record_name = 'schedules/%s/%s.json' % (name, re.sub(r'[^A-Za-z0-9_-]', '-', entry['name']))
            record = coordinator.get(record_name) or {}
            if due is None or record.get('fired', 0) >= due:
                continue
            claimed = {}

            def claim(current):
                if (current or {}).get('fired', 0) >= due:
                    return None
                claimed['ok'] = True
                return {'fired': due, 'at': now}
            coordinator.update(record_name, claim)
            if not claimed:
                continue  # another process fired it
            branch = entry.get('branch', 'main')
            try:
                commit = hosts.ls_remote(repo['url'], [branch], run=run)[branch]
            except Exception as error:
                coordinator.log('schedule_failed', level='error', repository=name, schedule=entry['name'],
                                error=str(error)[:300])
                continue
            build, _ = coordinator.create_build(name, {'event': 'push', 'branch': branch, 'commit': commit,
                                                       'message': 'schedule %s' % entry['name'], 'trusted': True},
                                                'schedule', env=entry.get('env', {}), created_by='schedule:%s' %
                                                entry['name'])
            created.append(build)
    return created


class Mirror:
    UPLOAD_PATHS = re.compile(r'^/(?P<repo>[A-Za-z0-9_.-]+)\.git/(?P<rest>info/refs|git-upload-pack|HEAD)$')

    def __init__(self, root, git='git', run=subprocess.run):
        self.root = Path(root)
        self.git = git
        self.run = run

    def path(self, repository):
        if not re.match(r'^[A-Za-z0-9_.-]+$', repository):
            raise ValueError('bad repository name')
        return self.root / ('%s.git' % repository)

    def _git(self, path, *args, check=True):
        completed = self.run([self.git, '-C', str(path)] + list(args), capture_output=True, text=True,
                             timeout=1800, check=False)
        if check and completed.returncode != 0:
            raise RuntimeError('git %s exited %d: %s' % (args[0], completed.returncode,
                                                          (completed.stderr or '')[-300:]))
        return completed

    def pin(self, repository, repo, number, commit):
        path = self.path(repository)
        if not path.exists():
            self.root.mkdir(parents=True, exist_ok=True)
            self.run([self.git, 'init', '--bare', '--quiet', str(path)], capture_output=True, check=True)
            self._git(path, 'remote', 'add', 'origin', repo['url'])
            self._git(path, 'config', 'uploadpack.allowAnySHA1InWant', 'true')
            self._git(path, 'config', 'uploadpack.allowFilter', 'true')
        if self._git(path, 'cat-file', '-e', '%s^{commit}' % commit, check=False).returncode != 0:
            fetched = self._git(path, 'fetch', '--quiet', '--no-tags', 'origin', commit, check=False)
            if fetched.returncode != 0:
                self._git(path, 'fetch', '--quiet', '--no-tags', 'origin', '+refs/heads/*:refs/heads/*')
        self._git(path, 'update-ref', 'refs/cerid/builds/%d' % int(number), commit)

    def unpin(self, repository, number):
        path = self.path(repository)
        if path.exists():
            self._git(path, 'update-ref', '-d', 'refs/cerid/builds/%d' % int(number), check=False)

    def serve(self, method, path_info, query, headers, body):
        """(status, headers, body) of a smart-HTTP fetch through `git http-backend`; pushes are refused."""
        match = self.UPLOAD_PATHS.match(path_info)
        if not match or (match.group('rest') == 'info/refs' and query != 'service=git-upload-pack'):
            return 403, {'Content-Type': 'text/plain'}, b'only fetches are served\n'
        env = {'GIT_PROJECT_ROOT': str(self.root), 'GIT_HTTP_EXPORT_ALL': '1', 'PATH_INFO': path_info,
               'REQUEST_METHOD': method, 'QUERY_STRING': query, 'CONTENT_TYPE': headers.get('Content-Type', ''),
               'CONTENT_LENGTH': str(len(body)), 'PATH': os.environ.get('PATH', ''),
               'SYSTEMROOT': os.environ.get('SYSTEMROOT', ''), 'GIT_PROTOCOL': headers.get('Git-Protocol', '')}
        if headers.get('Content-Encoding'):
            env['HTTP_CONTENT_ENCODING'] = headers['Content-Encoding']
        completed = self.run([self.git, 'http-backend'], input=body, env=env, capture_output=True, timeout=1800,
                             check=False)
        head, _, payload = completed.stdout.partition(b'\r\n\r\n')
        if not _:
            head, _, payload = completed.stdout.partition(b'\n\n')
        status = 200
        out = {}
        for line in head.decode('latin-1').splitlines():
            name, _, value = line.partition(':')
            if name.lower() == 'status':
                status = int(value.strip().split()[0])
            elif name:
                out[name.strip()] = value.strip()
        return status, out, payload
