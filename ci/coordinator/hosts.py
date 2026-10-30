"""Git host adapters of the Cerid orchestrator: signed webhooks, remote polling, schedules and commit status.

Any git host works through one of three triggers: a host webhook (GitHub, Gitea/Forgejo/Codeberg, GitLab), the
host-neutral signed webhook (`generic`, for any host or relay), or polling the remote with `git ls-remote` (no host
configuration at all). Webhooks are verified on the raw body before anything is parsed: an HMAC-SHA256 signature
(GitHub `X-Hub-Signature-256`, Gitea `X-Gitea-Signature`, generic `X-Cerid-Signature` over `<timestamp>.<body>`) or
GitLab's shared token, always compared in constant time. Replay protection: every delivery id is recorded
create-only in the store (a repeated id is acknowledged and ignored), the generic signature carries a timestamp
that must be within five minutes, and the trigger key (repository, event, branch, commit, pull request) is also
create-only, so a push seen by both a webhook and the poller builds once.
"""
from __future__ import annotations

import calendar
import hashlib
import hmac
import json
import re
import subprocess
import time
import urllib.parse

REPLAY_WINDOW = 300
PR_ACTIONS = ('opened', 'synchronize', 'synchronized', 'reopened', 'ready_for_review', 'edited')


class WebhookError(ValueError):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def hmac_hex(secret, data):
    return hmac.new(secret.encode('utf-8'), data, hashlib.sha256).hexdigest()


def header(headers, name):
    for key, value in headers.items():
        if key.lower() == name.lower():
            return value
    return None


def verify(kind, secret, headers, body, now=None):
    """Raise WebhookError(401) unless the delivery is authentic. Returns the delivery id (or a body digest)."""
    now = time.time() if now is None else now
    if not secret:
        raise WebhookError(503, 'no webhook secret configured for this repository')
    if kind == 'github':
        given = header(headers, 'X-Hub-Signature-256') or ''
        if not hmac.compare_digest(given.encode('utf-8'), ('sha256=' + hmac_hex(secret, body)).encode('utf-8')):
            raise WebhookError(401, 'bad signature')
        return header(headers, 'X-GitHub-Delivery') or hashlib.sha256(body).hexdigest()
    if kind == 'gitea':
        given = header(headers, 'X-Gitea-Signature') or header(headers, 'X-Forgejo-Signature') or ''
        if not hmac.compare_digest(given.encode('utf-8'), hmac_hex(secret, body).encode('utf-8')):
            raise WebhookError(401, 'bad signature')
        return (header(headers, 'X-Gitea-Delivery') or header(headers, 'X-Forgejo-Delivery')
                or hashlib.sha256(body).hexdigest())
    if kind == 'gitlab':
        given = header(headers, 'X-Gitlab-Token') or ''
        if not hmac.compare_digest(given.encode('utf-8'), secret.encode('utf-8')):
            raise WebhookError(401, 'bad token')
        return (header(headers, 'X-Gitlab-Event-UUID') or header(headers, 'Idempotency-Key')
                or hashlib.sha256(body).hexdigest())
    if kind == 'generic':
        fields = dict(part.split('=', 1) for part in (header(headers, 'X-Cerid-Signature') or '').split(',')
                      if '=' in part)
        try:
            stamp = int(fields.get('t', ''))
        except ValueError:
            raise WebhookError(401, 'no signature timestamp') from None
        if abs(now - stamp) > REPLAY_WINDOW:
            raise WebhookError(401, 'signature timestamp outside the replay window')
        expected = hmac_hex(secret, str(stamp).encode('ascii') + b'.' + body)
        if not hmac.compare_digest(fields.get('v1', '').encode('utf-8'), expected.encode('utf-8')):
            raise WebhookError(401, 'bad signature')
        return header(headers, 'X-Cerid-Delivery') or hashlib.sha256(body).hexdigest()
    raise WebhookError(404, 'unknown host kind %r' % kind)


def sign_generic(secret, body, now=None):
    """The X-Cerid-Signature header value a relay sends (also what the tests send)."""
    stamp = int(time.time() if now is None else now)
    return 't=%d,v1=%s' % (stamp, hmac_hex(secret, str(stamp).encode('ascii') + b'.' + body))


def branch_of(ref):
    return ref[len('refs/heads/'):] if ref and ref.startswith('refs/heads/') else None


def parse(kind, headers, body):
    """The trigger a verified delivery asks for, or None (ping, tag, deletion, closed pull request)."""
    try:
        payload = json.loads(body.decode('utf-8'))
    except ValueError:
        raise WebhookError(400, 'body is not JSON') from None
    if kind in ('github', 'gitea'):
        event = (header(headers, 'X-GitHub-Event') or header(headers, 'X-Gitea-Event')
                 or header(headers, 'X-Forgejo-Event') or '')
        if event == 'push':
            branch = branch_of(payload.get('ref'))
            after = payload.get('after') or ''
            if not branch or payload.get('deleted') or not after.strip('0'):
                return None
            head = payload.get('head_commit') or {}
            return {'event': 'push', 'branch': branch, 'commit': after, 'before': payload.get('before', ''),
                    'message': (head.get('message') or '')[:500], 'author': (head.get('author') or {}).get('name', ''),
                    'repository': (payload.get('repository') or {}).get('full_name', ''), 'trusted': True}
        if event == 'pull_request':
            pull = payload.get('pull_request') or {}
            if payload.get('action') not in PR_ACTIONS or pull.get('state', 'open') != 'open':
                return None
            head, base = pull.get('head') or {}, pull.get('base') or {}
            fork = (head.get('repo') or {}).get('full_name') != (base.get('repo') or {}).get('full_name')
            return {'event': 'pull_request', 'branch': head.get('ref', ''), 'commit': head.get('sha', ''),
                    'pull_request': str(pull.get('number', '')), 'base_branch': base.get('ref', 'main'),
                    'message': (pull.get('title') or '')[:500], 'author': (pull.get('user') or {}).get('login', ''),
                    'repository': (base.get('repo') or {}).get('full_name', ''), 'trusted': not fork}
        return None
    if kind == 'gitlab':
        event = header(headers, 'X-Gitlab-Event') or ''
        project = (payload.get('project') or {}).get('path_with_namespace', '')
        if event == 'Push Hook':
            branch = branch_of(payload.get('ref'))
            after = payload.get('checkout_sha') or payload.get('after') or ''
            if not branch or not after.strip('0'):
                return None
            return {'event': 'push', 'branch': branch, 'commit': after, 'before': payload.get('before', ''),
                    'message': '', 'author': payload.get('user_name', ''), 'repository': project, 'trusted': True}
        if event == 'Merge Request Hook':
            attrs = payload.get('object_attributes') or {}
            if attrs.get('action') not in ('open', 'update', 'reopen') or attrs.get('state') != 'opened':
                return None
            fork = attrs.get('source_project_id') != attrs.get('target_project_id')
            return {'event': 'pull_request', 'branch': attrs.get('source_branch', ''),
                    'commit': (attrs.get('last_commit') or {}).get('id', ''), 'pull_request': str(attrs.get('iid', '')),
                    'base_branch': attrs.get('target_branch', 'main'), 'message': (attrs.get('title') or '')[:500],
                    'author': (payload.get('user') or {}).get('username', ''), 'repository': project,
                    'trusted': not fork}
        return None
    if kind == 'generic':
        # The neutral form a relay sends: {"event": "push"|"pull_request", "branch", "commit", ...}.
        if payload.get('event') not in ('push', 'pull_request') or not payload.get('commit'):
            return None
        out = {k: str(payload.get(k, '')) for k in ('event', 'branch', 'commit', 'before', 'pull_request',
                                                     'base_branch', 'message', 'author', 'repository')}
        out['trusted'] = payload.get('event') == 'push'
        return out
    return None


SHA = re.compile(r'^[0-9a-f]{40}([0-9a-f]{24})?$')


def trigger_key(repository, trigger):
    """The create-only key that makes one (repository, event, branch, commit, pull request) build exactly once."""
    text = '\0'.join([repository, trigger.get('event', ''), trigger.get('branch', ''), trigger.get('commit', ''),
                      trigger.get('pull_request', '')])
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


# --------------------------------------------------------------------------------------------- polling

def ls_remote(url, branches, run=subprocess.run, timeout=60):
    """{branch: commit} of the remote's heads named in `branches`, through git's own credentials on this machine."""
    completed = run(['git', 'ls-remote', '--heads', url] + ['refs/heads/%s' % b for b in branches],
                    capture_output=True, text=True, timeout=timeout, check=False)
    if completed.returncode != 0:
        raise RuntimeError('git ls-remote %s exited %d: %s' % (url, completed.returncode,
                                                                (completed.stderr or '')[-300:]))
    heads = {}
    for line in completed.stdout.splitlines():
        sha, _, ref = line.partition('\t')
        branch = branch_of(ref.strip())
        if branch in branches and SHA.match(sha.strip()):
            heads[branch] = sha.strip()
    return heads


# --------------------------------------------------------------------------------------------- schedules

FIELDS = (('minute', 0, 59), ('hour', 0, 23), ('day', 1, 31), ('month', 1, 12), ('weekday', 0, 7))


def cron_field(text, low, high):
    values = set()
    for part in text.split(','):
        step = 1
        if '/' in part:
            part, step_text = part.split('/', 1)
            step = int(step_text)
            if step < 1:
                raise ValueError('cron step must be positive')
        if part == '*':
            start, end = low, high
        elif '-' in part:
            start, end = (int(v) for v in part.split('-', 1))
        else:
            start = end = int(part)
            if step > 1:
                end = high
        if not low <= start <= end <= high:
            raise ValueError('cron value %s outside %d-%d' % (part, low, high))
        values.update(range(start, end + 1, step))
    return values


class Cron:
    """A five-field cron expression in UTC (minute hour day month weekday; lists, ranges, steps; weekday 0 and 7 are
    Sunday; when both day and weekday are restricted either matches, as in Vixie cron)."""

    def __init__(self, text):
        parts = text.split()
        if len(parts) != 5:
            raise ValueError('cron expression needs five fields: %r' % text)
        self.text = text
        self.sets = [cron_field(p, lo, hi) for p, (_, lo, hi) in zip(parts, FIELDS)]
        if 7 in self.sets[4]:
            self.sets[4].add(0)
        self.day_any = parts[2] == '*'
        self.weekday_any = parts[4] == '*'

    def matches(self, when):
        t = time.gmtime(when)
        minute, hour, day, month = self.sets[:4]
        if t.tm_min not in minute or t.tm_hour not in hour or t.tm_mon not in month:
            return False
        weekday = (t.tm_wday + 1) % 7  # cron: Sunday 0
        day_ok = t.tm_mday in day
        weekday_ok = weekday in self.sets[4]
        if self.day_any or self.weekday_any:
            return day_ok and weekday_ok
        return day_ok or weekday_ok

    def last(self, now, horizon=8 * 24 * 3600):
        """The latest matching minute at or before `now` within `horizon`, or None."""
        minute = int(now // 60) * 60
        floor = now - horizon
        while minute >= floor:
            if self.matches(minute):
                return minute
            minute -= 60
        return None


def parse_utc(text):
    return calendar.timegm(time.strptime(text, '%Y-%m-%dT%H:%M:%SZ'))


# --------------------------------------------------------------------------------------------- commit status

STATES = {
    'github': {'scheduled': 'pending', 'running': 'pending', 'passed': 'success', 'failed': 'failure',
               'canceled': 'error'},
    'gitea': {'scheduled': 'pending', 'running': 'pending', 'passed': 'success', 'failed': 'failure',
              'canceled': 'error'},
    'gitlab': {'scheduled': 'pending', 'running': 'running', 'passed': 'success', 'failed': 'failed',
               'canceled': 'canceled'},
}


def status_request(kind, api, repository, commit, state, url, description, context='cerid-ci'):
    """(method, url, headers-without-token, body) of a commit status, or None for a host without an adapter."""
    if kind not in STATES:
        return None
    value = STATES[kind][state]
    if kind == 'gitlab':
        target = '%s/projects/%s/statuses/%s' % (api.rstrip('/'), urllib.parse.quote(repository, safe=''), commit)
        return 'POST', target, {'state': value, 'name': context, 'target_url': url, 'description': description[:140]}
    target = '%s/repos/%s/statuses/%s' % (api.rstrip('/'), repository, commit)
    return 'POST', target, {'state': value, 'context': context, 'target_url': url, 'description': description[:140]}


def auth_header(kind, token):
    if kind == 'gitlab':
        return {'PRIVATE-TOKEN': token}
    if kind == 'gitea':
        return {'Authorization': 'token %s' % token}
    return {'Authorization': 'Bearer %s' % token, 'Accept': 'application/vnd.github+json'}
