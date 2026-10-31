"""The coordinator core of the Cerid orchestrator: builds, jobs, agents, assignment and the watchdog.

All state lives in the result store under `orchestrator/` (layout in docs/design/ci-platform.md), written with
compare-and-swap (`pipeline.update_json`), so a restarted coordinator, `ci/ops.py` and the coordinator itself can act
on the same build at once without losing an update; a lost race is re-read and re-decided, never overwritten.
Nothing here keeps state in memory that a restart would need.
"""
from __future__ import annotations

from pathlib import Path
import copy
import hashlib
import json
import os
import re
import sys
import threading
import time
import urllib.parse

CI = Path(__file__).resolve().parents[1]
if str(CI) not in sys.path:
    sys.path.insert(0, str(CI))

from coordinator import PROTOCOL, SCHEMA_VERSION, VERSION, auth, diagnostics, hosts, notify  # noqa: E402
from coordinator import reports, state, steps  # noqa: E402

PREFIX = 'orchestrator/'
DEFAULTS = {
    'lost_after_seconds': 120,        # no heartbeat for this long: the agent is lost
    'boot_seconds': 900,              # a reserved machine whose agent never started the job: the VM is lost
    'accept_seconds': 120,            # an assigned job an agent never started
    'cancel_grace_seconds': 120,      # after a cancel or timeout request, the agent has this long to stop
    'scheduled_expiry_minutes': 1440,  # a ready job no agent takes for this long expires
    'default_step_minutes': 360,
    'session_hours': 12,
    'idle_poll_seconds': 15,         # an idle agent asks again after this long: each poll reads the bucket
    'recent_builds': 200,
    'log_chunk_bytes': 256 * 1024,
    'log_chunks': 4096,
    'artifact_bytes': 100 * 1024 * 1024,
    'logs_days': 90,
    'artifacts_days': 30,
}
TRIGGER_SOURCES = ('webhook', 'poll', 'manual', 'schedule')
# How an agent gets the commit: `direct` fetches the configured repository URL as it is (any shape: https, ssh, a
# filesystem path), `mirror` fetches the coordinator's mirror with the job token. Only this field decides.
CHECKOUT_STRATEGIES = ('direct', 'mirror')
SAFE_PATH = re.compile(r'^[A-Za-z0-9_@+=,.\- /]{1,400}$')


class NotFound(LookupError):
    pass


class Refused(PermissionError):
    pass


def json_bytes(value):
    return (json.dumps(value, indent=1, sort_keys=True) + '\n').encode('utf-8')


def pipeline_module():
    if 'ci_pipeline' in sys.modules:
        return sys.modules['ci_pipeline']
    import importlib.util
    spec = importlib.util.spec_from_file_location('ci_pipeline', CI / 'pipeline.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules['ci_pipeline'] = module
    spec.loader.exec_module(module)
    return module


class JsonLog:
    """Structured log of the coordinator itself: one JSON object per line (journald keeps it on the launcher)."""

    def __init__(self, stream=None, clock=time.time):
        self.stream = stream if stream is not None else sys.stderr
        self.clock = clock
        self.lock = threading.Lock()
        self.records = []

    def __call__(self, event, level='info', **fields):
        record = {'ts': round(self.clock(), 3), 'level': level, 'event': event}
        record.update({k: v for k, v in fields.items() if v is not None})
        self.records = (self.records + [record])[-500:]
        with self.lock:
            try:
                self.stream.write(json.dumps(record, sort_keys=True, default=str) + '\n')
                self.stream.flush()
            except (OSError, ValueError):
                pass


class Coordinator:
    def __init__(self, store, config=None, clock=time.time, secrets=None, http=None, log=None, mirror=None,
                 plan_env=None):
        self.p = pipeline_module()
        self.store = store
        self.config = copy.deepcopy(config or {})  # its own copy: a caller's later edits never leak in
        self.clock = clock
        self.secrets = secrets
        self.http = http
        self.log = log or JsonLog()
        self.mirror = mirror
        self.plan_env = dict(plan_env or {})
        self.limits = dict(DEFAULTS)
        self.limits.update(self.config.get('limits', {}))
        self.repositories = self.config.get('repositories', {})
        self.url = self.config.get('url', '').rstrip('/')
        self.notifier = notify.Notifier(self.config.get('notifications'), secrets, self.create, self.log)

    # Audit and notifications -----------------------------------------------------------------------------------
    def audit(self, who, action, target, detail='', via=None):
        """One record per manual or automatic decision (who, what, on what, when); never rewritten."""
        now = self.clock()
        entry = {'at': round(now, 3), 'who': str(who or 'unknown')[:200], 'action': action, 'target': str(target)[:300],
                 'detail': str(detail)[:1000], 'via': via}
        name = 'audit/%s/%013d-%s.json' % (reports.day(now), int(now * 1000), os.urandom(4).hex())
        self.create(name, entry)
        self.log('audit', **{k: v for k, v in entry.items() if k != 'at'})
        return entry

    def notify(self, event, title, body, key, branch=None):
        return self.notifier(event, title, body + ('\n\n%s' % self.url if self.url else ''), key, branch)

    # Storage -------------------------------------------------------------------------------------------------
    def get(self, name):
        return self.store.get_json(PREFIX + name)

    def update(self, name, change):
        return self.p.update_json(self.store, PREFIX + name, change)

    def create(self, name, value):
        """Create-only write: False when the object already exists (replay protection, idempotent chunks)."""
        return self.store.write_if(PREFIX + name, json_bytes(value), 0) is not None

    def ensure_schema(self):
        def change(current):
            if current is None:
                return {'schema': SCHEMA_VERSION, 'created': self.clock(), 'version': VERSION}
            if current['schema'] > SCHEMA_VERSION:
                raise RuntimeError('the store was written by schema %d; this coordinator (%s) knows %d: upgrade it'
                                   % (current['schema'], VERSION, SCHEMA_VERSION))
            if current['schema'] < SCHEMA_VERSION:
                return dict(current, schema=SCHEMA_VERSION, migrated=self.clock())
            return None
        return self.update('schema.json', change)

    def build_name(self, number):
        return 'builds/%010d.json' % int(number)

    def build(self, number):
        record = self.get(self.build_name(number))
        if record is None:
            raise NotFound('build %s' % number)
        return record

    def job(self, job_id):
        if not re.match(r'^[0-9a-f]{32}$', str(job_id)):
            raise NotFound('job %s' % job_id)
        record = self.get('jobs/%s.json' % job_id)
        if record is None:
            raise NotFound('job %s' % job_id)
        return record

    def jobs_of(self, build):
        return [j for j in (self.get('jobs/%s.json' % i) for i in build.get('jobs', [])) if j]

    def active_builds(self):
        return list((self.get('index/active.json') or {}).get('builds', []))

    def _index_active(self, number, add):
        def change(current):
            builds = set((current or {}).get('builds', []))
            if (number in builds) == add:
                return None
            builds = builds | {number} if add else builds - {number}
            return {'builds': sorted(builds)}
        self.update('index/active.json', change)

    def _index_recent(self, build):
        summary = {k: build.get(k) for k in ('number', 'repository', 'state', 'branch', 'commit', 'message', 'event',
                                             'source', 'created', 'started_at', 'finished_at', 'pull_request')}
        limit = self.limits['recent_builds']

        def change(current):
            items = [b for b in (current or {}).get('builds', []) if b['number'] != build['number']]
            items.append(summary)
            items.sort(key=lambda b: b['number'])
            return {'builds': items[-limit:]}
        self.update('index/recent.json', change)

    def recent(self, limit=50):
        return list(reversed((self.get('index/recent.json') or {}).get('builds', [])))[:limit]

    # Builds --------------------------------------------------------------------------------------------------
    def repository(self, name):
        if name not in self.repositories:
            raise NotFound('repository %s' % name)
        return self.repositories[name]

    def bootstrap(self, repo):
        """The build's first document: the plan step on the launcher. It carries the launcher's settings the plan reads
        (`plan_env`, from pipeline.plan_environment): an agent gives every job a clean environment, so without them
        the plan finds no account configured and plans no step."""
        spec = repo.get('pipeline', {})
        return {'schema': steps.SCHEMA, 'env': dict(self.plan_env), 'steps': [{
            'key': 'plan', 'label': ':pipeline: plan', 'command': spec.get('command', 'python ci/pipeline.py upload'),
            'agents': {'queue': spec.get('queue', 'cerid-launcher')},
            'timeout_in_minutes': spec.get('timeout_in_minutes', 30),
            'retry': {'automatic': [{'reason': 'infrastructure', 'limit': 2}]}}]}

    def trusted(self, repo, trigger):
        if not trigger.get('trusted', False):
            return False
        branches = repo.get('trusted_branches', ['main'])
        return trigger.get('event') != 'pull_request' and (trigger.get('branch') in branches or '*' in branches)

    def create_build(self, repository, trigger, source, document=None, env=None, created_by=None):
        """A new build for `trigger` ({event, branch, commit, ...}); webhook, poll and schedule triggers build each
        (repository, event, branch, commit, pull request) once. Returns (build, created)."""
        if source not in TRIGGER_SOURCES:
            raise ValueError('unknown trigger source %s' % source)
        repo = self.repository(repository)
        if not re.match(r'^[0-9a-f]{7,64}$', trigger.get('commit', '')):
            raise ValueError('a build needs a full commit id, got %r' % trigger.get('commit'))
        if source in ('webhook', 'poll') and trigger.get('event') == 'pull_request' and not trigger.get('trusted') \
                and not repo.get('build_forks', False):
            # A fork's pull request would run its own ci/pipeline.py as the build's first step, on the launcher and
            # its service account: it is never built unless the repository opts in with `build_forks`.
            self.log('build_refused_fork', repository=repository, pull_request=trigger.get('pull_request'),
                     commit=trigger.get('commit'))
            return None, False
        now = self.clock()
        number = self.update('counters/build.json', lambda c: {'next': (c or {'next': 1})['next'] + 1})['next'] - 1
        if source in ('webhook', 'poll'):
            key = hosts.trigger_key(repository, trigger)
            if not self.create('triggers/%s.json' % key, {'build': number, 'at': now}):
                existing = self.get('triggers/%s.json' % key) or {}
                found = self.get(self.build_name(existing.get('build', 0))) if existing.get('build') else None
                if found is not None or now - existing.get('at', now) < 60:
                    return found, False
                number = existing['build']  # the earlier creator died between the trigger and the build
        build = {'schema': SCHEMA_VERSION, 'number': number, 'repository': repository, 'state': 'scheduled',
                 'source': source, 'event': trigger.get('event', 'push'), 'branch': trigger.get('branch', ''),
                 'commit': trigger['commit'], 'before': trigger.get('before', ''),
                 'pull_request': trigger.get('pull_request', ''), 'base_branch': trigger.get('base_branch', ''),
                 'message': trigger.get('message', ''), 'author': trigger.get('author', ''),
                 'trusted': self.trusted(repo, trigger) if source in ('webhook', 'poll') else bool(
                     trigger.get('trusted', True)),
                 'env': dict(env or {}), 'created': now, 'created_by': created_by, 'jobs': [], 'keys': {},
                 'meta': {}, 'annotations': {}, 'cancel_requested': False, 'history': [],
                 'url': '%s/#/builds/%d' % (self.url, number)}
        if not self.create(self.build_name(number), build):
            return self.build(number), False
        self._index_active(number, True)
        self._index_recent(build)
        if self.mirror is not None and repo.get('checkout', 'direct') == 'mirror':
            try:
                self.mirror.pin(repository, repo, number, build['commit'])
            except Exception as error:  # the agent fails its checkout visibly; the build still exists
                self.log('mirror_failed', level='error', build=number, error=str(error))
        self.upload(number, document or self.bootstrap(repo))
        if source == 'manual':
            self.audit(created_by or 'unknown', 'build-create', 'build %d' % number, '%s %s at %s %s' % (
                repository, build['branch'], build['commit'][:12], ' '.join(
                    '%s=%s' % kv for kv in sorted(build['env'].items()))))
        self.log('build_created', build=number, repository=repository, source=source, branch=build['branch'],
                 commit=build['commit'])
        return self.build(number), True

    def upload(self, number, document, by_job=None):
        """Add the steps of `document` to build `number` (a step uploading more steps, or the first pipeline)."""
        build = self.build(number)
        specs, timeout = steps.normalize(document, existing_keys=build['keys'], order_start=len(build['jobs']))
        now = self.clock()
        created = []
        for spec in specs:
            job_id = hashlib.sha256(('%d\0%s\0%s' % (number, spec['key'], os.urandom(8).hex())).encode()).hexdigest()[:32]
            skip = spec.pop('skip')
            job = dict(spec, schema=SCHEMA_VERSION, id=job_id, build=number, state='scheduled', reason='', attempt=1,
                       attempts=[], infra_retries=0, exit_retries=0, created=now, ready_at=None, agent=None,
                       machine=None, assigned_at=None, started_at=None, finished_at=None, exit_status=None,
                       cancel_requested=None, uploaded_by=by_job, history=[], token_issued=False)
            if skip:
                job['state'] = skip['state']
                job['reason'] = skip.get('reason', skip['state'])[:500]
                job['finished_at'] = now
            if not self.create('jobs/%s.json' % job_id, job):
                raise RuntimeError('job id collision')
            created.append(job)

        def change(current):
            if any(j['key'] in current['keys'] for j in created):
                raise steps.PipelineError(['a concurrent upload used the same keys'])
            current['jobs'] = current['jobs'] + [j['id'] for j in created]
            current['keys'] = dict(current['keys'], **{j['key']: j['id'] for j in created})
            if timeout and not current.get('timeout_minutes'):
                current['timeout_minutes'] = timeout
            if current['state'] in state.BUILD_TERMINAL and any(j['state'] not in state.TERMINAL for j in created):
                state.build_transition(current, 'running', 'retry', now)
            return current
        build = self.update(self.build_name(number), change)
        if build['state'] not in state.BUILD_TERMINAL:
            self._index_active(number, True)
        self.log('steps_uploaded', build=number, steps=[j['key'] for j in created], by_job=by_job)
        return created

    def cancel_build(self, number, by=None, reason='canceled'):
        now = self.clock()

        def change(current):
            if current['state'] in state.BUILD_TERMINAL or current['cancel_requested']:
                return None
            current['cancel_requested'] = True
            current['canceled_by'] = by
            current['cancel_reason'] = reason
            return current
        before = self.build(number)
        build = self.update(self.build_name(number), change)
        for job_id in build['jobs']:
            self.cancel_job(job_id, reason=reason, now=now)
        if not before['cancel_requested'] and before['state'] not in state.BUILD_TERMINAL:
            self.audit(by or 'unknown', 'build-cancel', 'build %d' % number, reason)
        return self.build(number)

    def cancel_job(self, job_id, reason='canceled', now=None):
        now = self.clock() if now is None else now
        timeout = reason.startswith('timeout')

        def change(job):
            if job['state'] in ('scheduled', 'assigned'):
                return state.transition(job, 'timed_out' if timeout and job['state'] == 'assigned' else 'canceled',
                                        'timeout' if timeout and job['state'] == 'assigned' else 'cancel', now,
                                        reason)
            if job['state'] == 'running' and not job['cancel_requested']:
                job['cancel_requested'] = 'timeout' if timeout else 'canceled'
                job['cancel_at'] = now
                job['reason'] = reason
                return job
            return None
        job = self.update('jobs/%s.json' % job_id, change)
        self._released(job)
        return job

    def retry_job(self, job_id, by=None):
        """A manual retry: a failed, canceled, timed-out, expired or budget job runs again as its next attempt; its
        dependents broken by the failure wait again."""
        now = self.clock()
        job = self.job(job_id)
        if not (job.get('retry') or {}).get('manual', {}).get('allowed', True):
            raise Refused('step %s does not allow manual retries' % job['key'])
        if job['state'] not in ('failed', 'canceled', 'timed_out', 'expired', 'budget'):
            raise Refused('a %s job cannot be retried' % job['state'])

        def change(current):
            if current['state'] != job['state'] or current['attempt'] != job['attempt']:
                raise Refused('the job changed; reload and retry')
            return self._next_attempt(current, 'retry', now, 'manual retry by %s' % (by or 'operator'))
        self.update('jobs/%s.json' % job_id, change)

        def reopen(build):
            if build['state'] in state.BUILD_TERMINAL:
                state.build_transition(build, 'running', 'retry', now)
            build['cancel_requested'] = False
            return build
        build = self.update(self.build_name(job['build']), reopen)
        self._index_active(build['number'], True)
        self._index_recent(build)
        for other in self.jobs_of(build):
            if other['state'] == 'canceled' and other.get('reason', '').startswith('dependency'):
                self.update('jobs/%s.json' % other['id'],
                            lambda j: self._next_attempt(j, 'retry', now, 'dependency retried')
                            if j['state'] == 'canceled' else None)
        self.audit(by or 'unknown', 'job-retry', 'build %d %s' % (job['build'], job['key']),
                   'attempt %d' % (job['attempt'] + 1))
        return self.job(job_id)

    def _next_attempt(self, job, event, now, reason):
        summary = {k: job.get(k) for k in ('attempt', 'state', 'reason', 'agent', 'machine', 'exit_status',
                                           'assigned_at', 'started_at', 'finished_at')}
        state.transition(job, 'scheduled', event, now, reason)
        job['attempts'] = job.get('attempts', []) + [summary]
        job['attempt'] += 1
        for key in ('agent', 'machine', 'assigned_at', 'started_at', 'finished_at', 'exit_status', 'cancel_requested',
                    'cancel_at', 'token_hash'):
            job[key] = None
        job['token_issued'] = False
        job['ready_at'] = None
        return job

    # Concurrency groups ----------------------------------------------------------------------------------------
    def _slot(self, job, take):
        group = job.get('concurrency_group')
        if not group:
            return True
        name = 'groups/%s.json' % hashlib.sha256(group.encode('utf-8')).hexdigest()[:32]
        result = {}

        def change(current):
            holders = list((current or {}).get('jobs', []))
            if take:
                if job['id'] in holders:
                    result['ok'] = True
                    return None
                if len(holders) >= int(job.get('concurrency') or 1):
                    result['ok'] = False
                    return None
                result['ok'] = True
                return {'group': group, 'jobs': holders + [job['id']]}
            if job['id'] not in holders:
                return None
            return {'group': group, 'jobs': [h for h in holders if h != job['id']]}
        self.update(name, change)
        return result.get('ok', True)

    def _released(self, job):
        if job and job['state'] not in state.ACTIVE:
            self._slot(job, take=False)

    # Assignment ------------------------------------------------------------------------------------------------
    def ready_jobs(self, queue=None):
        """Scheduled jobs whose dependencies are satisfied, highest priority first, then oldest build, then order."""
        ready = []
        for number in self.active_builds():
            build = self.get(self.build_name(number))
            if build is None or build['cancel_requested']:
                continue
            jobs = self.jobs_of(build)
            by_key = {j['key']: j for j in jobs}
            for job in jobs:
                if job['state'] != 'scheduled' or (queue and job['agents'].get('queue') != queue):
                    continue
                if state.dependency_state(job, by_key) == 'ready':
                    if job['ready_at'] is None:
                        # The moment it became ready, even when an agent takes it before the next watchdog tick:
                        # the queue-time objective measures every assignment.
                        now = self.clock()
                        job = self.update('jobs/%s.json' % job['id'], lambda j: dict(j, ready_at=now)
                                          if j['ready_at'] is None else None)
                    ready.append((build, job))
        ready.sort(key=lambda bj: (-bj[1]['priority'], bj[1]['build'], bj[1]['order']))
        return ready

    def _claim(self, job, target_agent=None, machine=None, issue_token=True, agent_name=None):
        """scheduled -> assigned, holding a concurrency slot; returns (job, token) or (None, None) on a lost race."""
        if not self._slot(job, take=True):
            return None, None
        now = self.clock()
        token = auth.new_token('cjt') if issue_token else None
        claim = os.urandom(8).hex()

        def change(current):
            if current['state'] != 'scheduled' or current['attempt'] != job['attempt']:
                return None
            current['claim'] = claim
            state.transition(current, 'assigned', 'reserve' if machine else 'assign', now,
                             'reserved for %s' % machine if machine else 'assigned to %s' % target_agent)
            current['agent'] = target_agent
            current['agent_name'] = agent_name or machine or target_agent
            current['machine'] = machine
            current['assigned_at'] = now
            if token:
                current['token_hash'] = auth.token_hash(token)
                current['token_issued'] = True
            return current
        updated = self.update('jobs/%s.json' % job['id'], change)
        if updated.get('claim') != claim:
            self._slot(job, take=False)
            return None, None
        if token:
            self._record_token(updated, token)
        if updated.get('ready_at'):
            try:
                reports.record_queue(self, updated['agents'].get('queue', 'default'), now - updated['ready_at'], now)
            except Exception as error:  # a metric never blocks an assignment
                self.log('metric_failed', level='warning', error=str(error)[:200])
        return updated, token

    def _record_token(self, job, token):
        hours = (job.get('timeout_minutes') or self.limits['default_step_minutes']) / 60.0 + 2.0
        self.create('tokens/%s.json' % auth.token_hash(token), {'job': job['id'], 'attempt': job['attempt'],
                                                                'expires': self.clock() + hours * 3600})

    def poll(self, agent):
        """The next job for a polling agent (owned or launcher queue), or None."""
        offered = dict(agent.get('tags', {}), queue=agent['queue'])
        for build, job in self.ready_jobs(agent['queue']):
            if not state.tags_match(job['agents'], offered):
                continue
            claimed, token = self._claim(job, target_agent=agent['id'], agent_name=agent.get('name'))
            if claimed:
                self.update('agents/%s.json' % agent['id'], lambda a: dict(a, job=claimed['id'],
                                                                          last_seen=self.clock()))
                self.log('job_assigned', job=claimed['id'], build=claimed['build'], key=claimed['key'],
                         agent=agent['id'])
                return self.assignment(build, claimed, token, agent)
        return None

    def reserve(self, job, machine, issue_token=False):
        """Reserve a ready cloud job for a machine the dispatcher is about to launch."""
        return self._claim(job, machine=machine, issue_token=issue_token)

    def acquire(self, instance):
        """A cloud machine's agent, authenticated by its Google identity, takes the one job reserved for it."""
        record = self.get('instances/%s.json' % instance)
        if not record:
            raise Refused('no job is reserved for instance %s' % instance)
        token = auth.new_token('cjt')
        now = self.clock()

        def change(job):
            if job['state'] != 'assigned' or job['machine'] != instance or job['attempt'] != record['attempt']:
                raise Refused('the job reserved for %s is not waiting for it' % instance)
            if job['token_issued']:
                raise Refused('the job of %s was already acquired' % instance)
            job['token_issued'] = True
            job['token_hash'] = auth.token_hash(token)
            job['agent'] = 'vm:%s' % instance
            job['acquired_at'] = now
            return job
        job = self.update('jobs/%s.json' % record['job'], change)
        self._record_token(job, token)
        build = self.build(job['build'])
        self.log('job_acquired', job=job['id'], build=job['build'], instance=instance)
        return self.assignment(build, job, token, {'id': job['agent'], 'name': instance})

    def claim_with_token(self, token):
        """A one-shot agent holding the one job token its machine was launched with (an SSH host, a launcher-side
        device job) takes that job."""
        record = self.get('tokens/%s.json' % auth.token_hash(token or '')) if token else None
        if not record:
            raise auth.AuthError('unknown job token')
        job = self.job_for_token(record['job'], token)
        if job['state'] != 'assigned':
            raise Refused('job %s is %s' % (job['id'], job['state']))
        self.log('job_claimed', job=job['id'], build=job['build'], machine=job['machine'])
        return self.assignment(self.build(job['build']), job, token, {'id': job['machine'] or job['agent'],
                                                                      'name': job['machine'] or job['agent']})

    def assignment(self, build, job, token, agent):
        repo = self.repositories.get(build['repository'], {})
        env = {'CERID_CI': '1', 'CERID_CI_BUILD_NUMBER': str(build['number']), 'CERID_CI_BUILD_URL': build['url'],
               'CERID_CI_JOB_ID': job['id'], 'CERID_CI_JOB_URL': '%s/#/jobs/%s' % (self.url, job['id']),
               'CERID_CI_STEP_KEY': job['key'], 'CERID_CI_COMMIT': build['commit'],
               'CERID_CI_BRANCH': build['branch'], 'CERID_CI_SOURCE': build['source'],
               'CERID_CI_EVENT': build['event'], 'CERID_CI_PULL_REQUEST': build['pull_request'] or 'false',
               'CERID_CI_PULL_REQUEST_BASE_BRANCH': build['base_branch'],
               'CERID_CI_REPOSITORY': build['repository'], 'CERID_CI_RETRY_COUNT': str(job['attempt'] - 1),
               'CERID_CI_AGENT_NAME': str(agent.get('name', agent.get('id'))), 'CERID_CI_COORDINATOR': self.url}
        env.update(build.get('env', {}))
        env.update(job['env'])
        secrets = {}
        if job['secrets']:
            if not build['trusted']:
                raise Refused('step %s declares secrets and build %d is not trusted (a pull request or an untrusted '
                              'branch)' % (job['key'], build['number']))
            for item in job['secrets']:
                secrets[item['env']] = self.secrets.value(item['name']) if self.secrets else ''
        checkout = {'url': repo.get('url', ''), 'commit': build['commit'], 'branch': build['branch'],
                    'strategy': repo.get('checkout', 'direct'), 'depth': int(repo.get('depth', 50))}
        if checkout['strategy'] not in CHECKOUT_STRATEGIES:
            raise ValueError('repository %s: unknown checkout strategy %r' % (build['repository'],
                                                                             checkout['strategy']))
        if checkout['strategy'] == 'mirror':
            # Relative: the agent fetches from the coordinator address it already uses (a cloud VM inside the
            # project's network, an owned machine through the tunnel).
            checkout['url'] = '/git/%s.git' % build['repository']
            checkout['ref'] = 'refs/cerid/builds/%d' % build['number']
        return {'protocol': PROTOCOL, 'token': token,
                'job': {'id': job['id'], 'build': build['number'], 'key': job['key'], 'label': job['label'],
                        'command': job['command'], 'attempt': job['attempt'], 'env': env,
                        'artifact_paths': job['artifact_paths'], 'timeout_minutes': job['timeout_minutes']
                        or self.limits['default_step_minutes'], 'secrets': secrets},
                'checkout': checkout}

    # Agents ----------------------------------------------------------------------------------------------------
    def create_registration(self, name, queue, tags=None, by='operator'):
        """A registration token for an owned machine (returned once; only its hash is stored)."""
        if not steps.NAME.match(name):
            raise ValueError('agent names are letters, digits, - and _')
        token = auth.new_token('crt')
        self.create('registrations/%s.json' % auth.token_hash(token),
                    {'name': name, 'queue': queue, 'tags': dict(tags or {}), 'created': self.clock(),
                     'revoked': False})
        self.audit(by, 'agent-token-create', name, 'queue %s tags %s' % (queue, dict(tags or {})))
        return token

    def revoke_registration(self, name, by='operator'):
        revoked = 0
        for item in self.store.list(PREFIX + 'registrations/'):
            record = self.store.get_json(item)
            if record and record['name'] == name and not record['revoked']:
                self.p.update_json(self.store, item, lambda r: dict(r, revoked=True, revoked_at=self.clock()))
                revoked += 1
        self.audit(by, 'agent-token-revoke', name, '%d revoked' % revoked)
        return revoked

    def register(self, registration_token, info):
        record = self.get('registrations/%s.json' % auth.token_hash(registration_token or ''))
        if not record or record['revoked']:
            raise auth.AuthError('unknown or revoked registration token')
        if int(info.get('protocol', 0)) != PROTOCOL:
            raise Refused('agent protocol %s; this coordinator speaks %d (version %s)' % (
                info.get('protocol'), PROTOCOL, VERSION))
        name = str(info.get('name') or record['name'])[:100]
        agent_id = hashlib.sha256(('%s\0%s' % (record['name'], name)).encode('utf-8')).hexdigest()[:24]
        tags = {str(k): str(v) for k, v in (info.get('tags') or {}).items() if k != 'queue'}
        tags.update(record['tags'])  # the registration's tags win: an agent cannot claim another device
        session = auth.new_token('cst')
        now = self.clock()
        expires = now + self.limits['session_hours'] * 3600
        agent = {'schema': SCHEMA_VERSION, 'id': agent_id, 'name': name, 'registration': record['name'],
                 'queue': record['queue'], 'tags': tags, 'os': str(info.get('os', ''))[:40],
                 'arch': str(info.get('arch', ''))[:40], 'version': str(info.get('version', ''))[:40],
                 'mode': str(info.get('mode', 'long-running'))[:20], 'registered': now, 'last_seen': now,
                 'job': None, 'stopped': False}
        self.update('agents/%s.json' % agent_id, lambda current: dict(current or {}, **agent))
        self.create('sessions/%s.json' % auth.token_hash(session), {'agent': agent_id, 'expires': expires})
        self.log('agent_registered', agent=agent_id, name=name, queue=record['queue'])
        return agent, session, expires

    def session_agent(self, token):
        record = self.get('sessions/%s.json' % auth.token_hash(token or '')) if token else None
        if not record or record['expires'] < self.clock():
            raise auth.AuthError('unknown or expired agent session')
        agent = self.get('agents/%s.json' % record['agent'])
        if not agent or agent.get('stopped'):
            raise auth.AuthError('agent stopped by the operator')
        self.update('agents/%s.json' % agent['id'], lambda a: dict(a, last_seen=self.clock()))
        return agent

    def stop_agent(self, agent_id, by='operator'):
        agent = self.update('agents/%s.json' % agent_id, lambda a: dict(a, stopped=True) if a else None)
        if agent is None:
            raise NotFound('agent %s' % agent_id)
        self.audit(by, 'agent-stop', agent.get('name', agent_id), agent_id)
        return agent

    def job_for_token(self, job_id, token, writing=True):
        record = self.get('tokens/%s.json' % auth.token_hash(token or '')) if token else None
        if not record or record['job'] != job_id or record['expires'] < self.clock():
            raise auth.AuthError('unknown or expired job token')
        job = self.job(job_id)
        if not auth.same(job.get('token_hash'), auth.token_hash(token)) or job['attempt'] != record['attempt']:
            raise auth.AuthError('the token belongs to an earlier attempt of this job')
        if writing and job['state'] in state.TERMINAL:
            raise Refused('job %s is %s' % (job_id, job['state']))
        return job

    # Job lifecycle (agent side) ----------------------------------------------------------------------------------
    def start(self, job_id, token):
        self.job_for_token(job_id, token)
        now = self.clock()

        def change(job):
            if job['state'] == 'running':
                return None
            job['started_at'] = now
            job['reason'] = ''
            return state.transition(job, 'running', 'start', now)
        job = self.update('jobs/%s.json' % job_id, change)
        self.store.put_json(PREFIX + 'heartbeats/%s.json' % job_id, {'at': now, 'attempt': job['attempt']})
        self._build_running(job['build'])
        self.log('job_started', job=job_id, build=job['build'], key=job['key'], agent=job['agent'])
        return job

    def record_environment(self, job_id, token, record):
        """The agent's reproducibility record of this attempt: machine, image, toolchain versions, environment
        (secret values never included), commit and device."""
        job = self.job_for_token(job_id, token)
        record = {k: record.get(k) for k in ('os', 'platform', 'arch', 'cpu', 'cpus', 'machine', 'image', 'tools',
                                            'env', 'commit', 'agent_version', 'python', 'device', 'checkout')}
        text = json.dumps(record)
        if len(text) > 64 * 1024:
            raise Refused('the reproducibility record is larger than 64 KiB')
        attempt = job['attempt']
        self.update('jobs/%s.json' % job_id, lambda j: dict(j, repro=dict(record, attempt=attempt))
                    if j['attempt'] == attempt else None)
        return True

    def heartbeat(self, job_id, token):
        job = self.job_for_token(job_id, token)
        self.store.put_json(PREFIX + 'heartbeats/%s.json' % job_id, {'at': self.clock(), 'attempt': job['attempt']})
        return {'cancel': bool(job['cancel_requested']), 'reason': job['reason'] if job['cancel_requested'] else ''}

    def finish(self, job_id, token, exit_status, infra=None, detail='', timed_out=False):
        """The agent's report. `infra` names an infrastructure loss the agent saw (preempted, agent_stop)."""
        job = self.job_for_token(job_id, token)
        if infra:
            return self.infrastructure(job_id, infra if infra in steps.INFRA_REASONS else 'infrastructure', detail,
                                       attempt=job['attempt'])
        now = self.clock()

        def change(current):
            if current['state'] in state.TERMINAL or current['attempt'] != job['attempt']:
                return None
            if current['state'] == 'assigned':
                current['started_at'] = now
                state.transition(current, 'running', 'start', now)
            current['exit_status'] = exit_status
            if timed_out or current['cancel_requested'] == 'timeout':
                return state.transition(current, 'timed_out', 'finish', now, current['reason'] or 'timeout')
            if current['cancel_requested']:
                return state.transition(current, 'canceled', 'finish', now, current['reason'] or 'canceled')
            if exit_status == 0:
                return state.transition(current, 'passed', 'finish', now)
            if current['exit_retries'] < state.retry_limit(current, exit_status=exit_status):
                current['exit_retries'] += 1
                return self._next_attempt(current, 'retry_exit', now, 'exit status %s: automatic retry' % exit_status)
            return state.transition(current, 'failed', 'finish', now, detail or 'exit status %s' % exit_status)
        job = self.update('jobs/%s.json' % job_id, change)
        self._released(job)
        if job.get('agent') and not str(job['agent']).startswith('vm:'):
            self.update('agents/%s.json' % job['agent'], lambda a: dict(a, job=None) if a else None)
        self.log('job_finished', job=job_id, build=job['build'], key=job['key'], state=job['state'],
                 exit_status=exit_status)
        self.settle(job['build'])
        return job

    def infrastructure(self, job_id, reason, detail='', attempt=None):
        """An infrastructure loss (agent lost, VM preempted or lost, launch failed): the next attempt on a fresh
        agent or machine while the step's automatic retries last, else failed. A failed test is never retried
        here: only `exit_status` rules retry a command's own failure."""
        now = self.clock()

        def change(job):
            if job['state'] in state.TERMINAL or (attempt is not None and job['attempt'] != attempt):
                return None
            limit = state.retry_limit(job, reason=reason)
            why = 'infrastructure: %s%s' % (reason, ': ' + detail if detail else '')
            if job['infra_retries'] < limit and not job['cancel_requested']:
                job['infra_retries'] += 1
                return self._next_attempt(job, 'requeue', now, why)
            if job['state'] == 'scheduled':
                return state.transition(job, 'failed', 'launch_failed', now, why)
            return state.transition(job, 'canceled' if job['cancel_requested'] == 'canceled' else 'failed',
                                    'cancel' if job['cancel_requested'] == 'canceled' else 'infrastructure', now, why)
        job = self.update('jobs/%s.json' % job_id, change)
        self._slot(job, take=False)
        self.log('job_infrastructure', level='warning', job=job_id, reason=reason, detail=detail, state=job['state'],
                 attempt=job['attempt'])
        self.settle(job['build'])
        return job

    def mark_budget(self, job_id, detail):
        now = self.clock()
        job = self.update('jobs/%s.json' % job_id, lambda j: state.transition(j, 'budget', 'budget', now, detail)
                          if j['state'] == 'scheduled' else None)
        self.settle(job['build'])
        return job

    def fail_job(self, job_id, event, detail):
        now = self.clock()
        job = self.update('jobs/%s.json' % job_id, lambda j: state.transition(j, 'failed', event, now, detail)
                          if j['state'] == 'scheduled' else None)
        self.settle(job['build'])
        return job

    # Job services (agent side, job token) ----------------------------------------------------------------------
    def append_log(self, job_id, token, attempt, seq, data):
        job = self.job_for_token(job_id, token)
        if int(attempt) != job['attempt']:
            raise Refused('log for attempt %s; the job is on attempt %d' % (attempt, job['attempt']))
        if not 0 <= int(seq) < self.limits['log_chunks'] or len(data) > self.limits['log_chunk_bytes']:
            raise Refused('log chunk outside the limits')
        name = 'logs/%s/%d/%08d.log' % (job_id, job['attempt'], int(seq))
        if not self.store.write_if(PREFIX + name, data, 0) is not None:
            if self.store.get(PREFIX + name) != data:
                raise Refused('log chunk %s already holds other bytes' % seq)
        return True

    def read_log(self, job_id, attempt=None, start=0):
        job = self.job(job_id)
        attempt = int(attempt or job['attempt'])
        names = self.store.list(PREFIX + 'logs/%s/%d/' % (job_id, attempt))
        chunks = []
        nxt = start
        for name in names:
            seq = int(name.rsplit('/', 1)[-1].split('.')[0])
            if seq < start:
                continue
            if seq != nxt:
                break  # a gap: wait for the missing chunk rather than show the log out of order
            chunks.append(self.store.get(name) or b'')
            nxt = seq + 1
        live = job['state'] not in state.TERMINAL and attempt == job['attempt']
        return b''.join(chunks), nxt, live, job

    def put_artifact(self, job_id, token, path, data, digest, kind=None):
        """Store one artifact of the job's attempt, typed by `kind` or by its path (coordinator/diagnostics.py)."""
        job = self.job_for_token(job_id, token)
        path = path.replace('\\', '/').lstrip('/')
        if not SAFE_PATH.match(path) or '..' in path.split('/'):
            raise Refused('artifact path %r refused' % path)
        if len(data) > self.limits['artifact_bytes']:
            raise Refused('artifact larger than %d bytes' % self.limits['artifact_bytes'])
        if not auth.same(hashlib.sha256(data).hexdigest(), digest):
            raise Refused('artifact digest mismatch')
        name = 'artifacts/%d/%s/%d/%s' % (job['build'], job_id, job['attempt'], path)
        self.store.put(PREFIX + name, data)
        if kind is not None and kind not in diagnostics.KINDS:
            raise Refused('artifact kind %r refused' % kind)
        entry = {'job': job_id, 'key': job['key'], 'attempt': job['attempt'], 'path': path, 'size': len(data),
                 'sha256': digest, 'at': self.clock(), 'object': name,
                 'kind': kind or diagnostics.artifact_kind(path)}

        def change(current):
            entries = [e for e in (current or {}).get('entries', [])
                       if not (e['job'] == job_id and e['attempt'] == job['attempt'] and e['path'] == path)]
            return {'entries': entries + [entry]}
        self.update('artifacts/%d/manifest.json' % job['build'], change)
        if entry['kind'] == 'gold-manifest':
            from coordinator import gold
            gold.register_manifest(self, job, entry, data)
        return entry

    def artifacts(self, number):
        """Every artifact entry of a build, each with its kind (entries stored before kinds are typed by path)."""
        return [dict(e, kind=e.get('kind') or diagnostics.artifact_kind(e['path']))
                for e in (self.get('artifacts/%d/manifest.json' % int(number)) or {}).get('entries', [])]

    def artifact(self, number, path, job_id=None):
        matches = [e for e in self.artifacts(number) if e['path'] == path and (job_id is None or e['job'] == job_id)]
        if not matches:
            raise NotFound('artifact %s in build %s' % (path, number))
        entry = max(matches, key=lambda e: e['at'])
        data = self.store.get(PREFIX + entry['object'])
        if data is None:
            raise NotFound('artifact %s was removed by retention' % path)
        return entry, data

    def annotate(self, job_id, token, context, style, body, append=False):
        job = self.job_for_token(job_id, token)
        context = str(context or 'default')[:100]
        style = style if style in ('success', 'info', 'warning', 'error') else 'info'

        def change(build):
            notes = dict(build.get('annotations', {}))
            old = notes.get(context, {}).get('body', '') if append else ''
            notes[context] = {'style': style, 'body': (old + body)[-200000:], 'job': job_id}
            build['annotations'] = notes
            return build
        self.update(self.build_name(job['build']), change)

    def set_meta(self, job_id, token, key, value):
        job = self.job_for_token(job_id, token)
        if not steps.KEY.match(key):
            raise Refused('meta-data keys are step-key characters')
        self.update(self.build_name(job['build']), lambda b: dict(b, meta=dict(b.get('meta', {}), **{key: str(
            value)[:100000]})))

    def get_meta(self, job_id, token, key):
        job = self.job_for_token(job_id, token, writing=False)
        meta = self.build(job['build']).get('meta', {})
        if key not in meta:
            raise NotFound('meta-data %s' % key)
        return meta[key]

    def step_outcome(self, job_id, token, key):
        job = self.job_for_token(job_id, token, writing=False)
        build = self.build(job['build'])
        other_id = build['keys'].get(key)
        if not other_id:
            return {'state': 'absent', 'outcome': 'absent'}
        other = self.job(other_id)
        outcome = {'passed': 'passed', 'cached': 'passed', 'budget': 'budget'}.get(other['state'])
        if outcome is None:
            outcome = 'soft_failed' if state.soft_failed(other) else (
                'failed' if other['state'] in state.TERMINAL else 'running')
        return {'state': other['state'], 'outcome': outcome}

    # Build settlement and status -------------------------------------------------------------------------------
    def _build_running(self, number):
        now = self.clock()
        build = self.update(self.build_name(number), lambda b: state.build_transition(b, 'running', 'start', now)
                            if b['state'] == 'scheduled' else None)
        if build['state'] == 'running':
            self._index_recent(build)
            self.report_status(build)

    def settle(self, number):
        """Finish a build whose jobs are all terminal; report its commit status."""
        build = self.build(number)
        outcome = state.build_outcome(build, self.jobs_of(build))
        if outcome is None or build['state'] in state.BUILD_TERMINAL:
            return build
        now = self.clock()
        build = self.update(self.build_name(number), lambda b: state.build_transition(b, outcome, 'finish', now)
                            if b['state'] not in state.BUILD_TERMINAL else None)
        self._index_active(number, False)
        self._index_recent(build)
        self.report_status(build)
        self.log('build_finished', build=number, state=build['state'])
        self.notify_outcome(build)
        return build

    def previous_build(self, build):
        """The newest earlier finished build of the same repository and branch (from the recent index)."""
        for item in self.recent(200):
            if item['number'] < build['number'] and item['repository'] == build['repository'] and \
                    item['branch'] == build['branch'] and item['state'] in ('passed', 'failed'):
                return item
        return None

    def notify_outcome(self, build):
        if build['state'] not in ('passed', 'failed') or build['source'] == 'manual' and build.get(
                'created_by') == 'bisector':
            return None
        previous = self.previous_build(build)
        if build['state'] == 'failed':
            failed = [j['key'] for j in self.jobs_of(build) if j['state'] in ('failed', 'timed_out', 'expired')
                      and not state.soft_failed(j)]
            return self.notify('failure', 'Build #%d failed on %s' % (build['number'], build['branch']),
                               '%s %s at %s: %s failed.' % (build['repository'], build['branch'], build['commit'][:12],
                                                           ', '.join(failed) or 'a step'),
                               key='failure:%d' % build['number'], branch=build['branch'])
        if previous and previous['state'] == 'failed':
            return self.notify('recovery', 'Build #%d recovered %s' % (build['number'], build['branch']),
                               '%s %s is green again at %s (build #%d had failed).' % (
                                   build['repository'], build['branch'], build['commit'][:12], previous['number']),
                               key='recovery:%d' % build['number'], branch=build['branch'])
        return None

    def report_status(self, build):
        repo = self.repositories.get(build['repository'], {})
        spec = repo.get('status')
        if not spec or self.http is None:
            return None
        description = {'scheduled': 'scheduled', 'running': 'running', 'passed': 'passed', 'failed': 'failed',
                       'canceled': 'canceled'}[build['state']]
        request = hosts.status_request(spec['kind'], spec['api'], spec['repository'], build['commit'], build['state'],
                                       build['url'], 'Cerid CI build %d %s' % (build['number'], description),
                                       spec.get('context', 'cerid-ci'))
        if request is None:
            return None
        try:
            token = self.secrets.value(spec['token_secret']) if self.secrets else ''
            method, url, body = request
            self.http(method, url, body=body, headers=hosts.auth_header(spec['kind'], token))
            return True
        except Exception as error:  # a status is courtesy: never fail a build or the tick for it
            self.log('status_failed', level='warning', build=build['number'], error=str(error)[:300])
            return False

    # Watchdog --------------------------------------------------------------------------------------------------
    def tick(self):
        """One pass of the watchdog over every active build: dependencies, expiry, lost agents, timeouts, build
        timeouts and settlement. Safe to run from any process at any time."""
        now = self.clock()
        limits = self.limits
        for number in self.active_builds():
            build = self.get(self.build_name(number))
            if build is None:
                self._index_active(number, False)
                continue
            if build.get('timeout_minutes') and build['state'] == 'running' and not build['cancel_requested'] and \
                    now - build.get('started_at', now) > build['timeout_minutes'] * 60:
                self.cancel_build(number, by='watchdog', reason='timeout: the build ran over %s minutes' %
                                  build['timeout_minutes'])
            jobs = self.jobs_of(build)
            by_key = {j['key']: j for j in jobs}
            for job in jobs:
                if job['state'] == 'scheduled':
                    deps = state.dependency_state(job, by_key)
                    if deps.startswith('broken'):
                        self.update('jobs/%s.json' % job['id'], lambda j: state.transition(
                            j, 'canceled', 'dependency_failed', now, deps[len('broken: '):])
                            if j['state'] == 'scheduled' else None)
                    elif deps == 'ready' and job['ready_at'] is None:
                        self.update('jobs/%s.json' % job['id'], lambda j: dict(j, ready_at=now)
                                    if j['state'] == 'scheduled' and j['ready_at'] is None else None)
                    elif deps == 'ready' and now - job['ready_at'] > limits['scheduled_expiry_minutes'] * 60:
                        self.update('jobs/%s.json' % job['id'], lambda j: state.transition(
                            j, 'expired', 'expire', now, 'no agent took the job in %d minutes' %
                            limits['scheduled_expiry_minutes']) if j['state'] == 'scheduled' else None)
                elif job['state'] == 'assigned':
                    waited = now - (job['assigned_at'] or now)
                    if job['machine'] and waited > limits['boot_seconds']:
                        self.infrastructure(job['id'], 'vm_lost', 'the machine never started the job in %d s' %
                                            limits['boot_seconds'], attempt=job['attempt'])
                    elif not job['machine'] and waited > limits['accept_seconds']:
                        self.infrastructure(job['id'], 'agent_lost', 'the agent never started the job',
                                            attempt=job['attempt'])
                elif job['state'] == 'running':
                    beat = self.store.get_json(PREFIX + 'heartbeats/%s.json' % job['id']) or {}
                    last = beat.get('at', job['started_at'] or now) if beat.get('attempt') == job['attempt'] else (
                        job['started_at'] or now)
                    timeout = (job['timeout_minutes'] or limits['default_step_minutes']) * 60
                    if job['cancel_requested'] and now - (job.get('cancel_at') or now) > limits[
                            'cancel_grace_seconds']:
                        target = 'timed_out' if job['cancel_requested'] == 'timeout' else 'canceled'
                        self.update('jobs/%s.json' % job['id'], lambda j: state.transition(
                            j, target, 'timeout' if target == 'timed_out' else 'cancel', now,
                            (j['reason'] or target) + ' (the agent did not stop in time)')
                            if j['state'] == 'running' else None)
                        self._slot(job, take=False)
                    elif now - last > limits['lost_after_seconds']:
                        self.infrastructure(job['id'], 'agent_lost', 'no heartbeat for %d s' % int(now - last),
                                            attempt=job['attempt'])
                    elif not job['cancel_requested'] and now - (job['started_at'] or now) > timeout:
                        self.cancel_job(job['id'], reason='timeout: ran over %s minutes' % (
                            job['timeout_minutes'] or limits['default_step_minutes']), now=now)
            self.settle(number)
        self.store.put_json(PREFIX + 'coordinator/heartbeat.json', {'at': now, 'version': VERSION,
                                                                   'pid': os.getpid()})
        reports.record_tick(self, now)

    # Retention -------------------------------------------------------------------------------------------------
    def sweep(self, limit=200):
        """Delete logs and artifacts of builds finished longer ago than the retention; build and job records stay."""
        now = self.clock()
        cursor = (self.get('index/retention.json') or {}).get('next', 1)
        last = (self.get('counters/build.json') or {'next': 1})['next'] - 1
        done = 0
        number = cursor
        while number <= last and done < limit:
            build = self.get(self.build_name(number))
            if build is not None:
                if build['state'] not in state.BUILD_TERMINAL:
                    break
                age_days = (now - build.get('finished_at', now)) / 86400.0
                purged = dict(build.get('purged', {}))
                if age_days < min(self.limits['logs_days'], self.limits['artifacts_days']):
                    break
                if age_days >= self.limits['artifacts_days'] and not purged.get('artifacts'):
                    for name in self.store.list(PREFIX + 'artifacts/%d/' % number):
                        if not name.endswith('/manifest.json'):
                            self.store.delete(name)
                    purged['artifacts'] = now
                if age_days >= self.limits['logs_days'] and not purged.get('logs'):
                    for job_id in build['jobs']:
                        for name in self.store.list(PREFIX + 'logs/%s/' % job_id):
                            self.store.delete(name)
                    purged['logs'] = now
                self.update(self.build_name(number), lambda b: dict(b, purged=purged))
                if not (purged.get('logs') and purged.get('artifacts')):
                    break
            number += 1
            done += 1
        self.update('index/retention.json', lambda c: {'next': number} if (c or {}).get('next', 1) != number else None)
        return number

    # Views -------------------------------------------------------------------------------------------------------
    def agents(self):
        out = []
        now = self.clock()
        for name in self.store.list(PREFIX + 'agents/'):
            agent = self.store.get_json(name)
            if not agent:
                continue
            idle = now - agent.get('last_seen', 0)
            agent['status'] = 'stopped' if agent.get('stopped') else ('busy' if agent.get('job') else (
                'idle' if idle < self.limits['lost_after_seconds'] else 'lost'))
            out.append(agent)
        return out

    def queues(self):
        counts = {}
        for number in self.active_builds():
            build = self.get(self.build_name(number))
            if not build:
                continue
            for job in self.jobs_of(build):
                queue = job['agents'].get('queue', 'default')
                entry = counts.setdefault(queue, {'queue': queue, 'scheduled': 0, 'assigned': 0, 'running': 0,
                                                  'agents': 0, 'oldest_wait_s': 0})
                if job['state'] in ('scheduled', 'assigned', 'running'):
                    entry[job['state']] += 1
                if job['state'] == 'scheduled' and job['ready_at']:
                    entry['oldest_wait_s'] = max(entry['oldest_wait_s'], int(self.clock() - job['ready_at']))
        for agent in self.agents():
            if agent['status'] in ('idle', 'busy'):
                entry = counts.setdefault(agent['queue'], {'queue': agent['queue'], 'scheduled': 0, 'assigned': 0,
                                                           'running': 0, 'agents': 0, 'oldest_wait_s': 0})
                entry['agents'] += 1
        return sorted(counts.values(), key=lambda q: q['queue'])

    def build_view(self, number):
        build = self.build(number)
        jobs = self.jobs_of(build)
        by_key = {j['key']: j for j in jobs}
        view = []
        for job in jobs:
            item = {k: job.get(k) for k in ('id', 'key', 'label', 'state', 'reason', 'attempt', 'agent', 'machine',
                                            'agent_name', 'exit_status', 'priority', 'depends_on', 'created',
                                            'started_at', 'finished_at', 'soft_fail', 'agents', 'cancel_requested',
                                            'attempts', 'concurrency_group', 'timeout_minutes')}
            item['soft_failed'] = state.soft_failed(job)
            if job['state'] == 'scheduled':
                item['waiting'] = state.dependency_state(job, by_key)
            view.append(item)
        return dict(build, jobs=view, artifacts=self.artifacts(number))


class DirectorySecrets:
    """Secrets as files in one directory (owned machines and tests): the file name is the secret name."""

    def __init__(self, root):
        self.root = Path(root)

    def value(self, name):
        if not steps.NAME.match(name):
            raise Refused('bad secret name')
        path = self.root / name
        if not path.is_file():
            raise NotFound('secret %s' % name)
        return path.read_text(encoding='utf-8').strip()


class SecretManager:
    """Secrets from the CI project's Secret Manager, read with the coordinator's own identity (names come from the
    configuration and the declaring step; values are never logged)."""

    def __init__(self, project):
        self.project = project

    def value(self, name):
        from executors import gce_vm
        return gce_vm.secret_value(self.project, name)


def quote(text):
    return urllib.parse.quote(text, safe='')
