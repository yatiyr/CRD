"""The HTTP service of the Cerid orchestrator: webhooks, the agent API, the UI API and the web UI.

Three surfaces, three kinds of authentication, none anonymous:

- /webhooks/<repository>   the host's HMAC signature (or GitLab token) over the raw body, replay-protected
- /api/agent/...           bearer tokens: a registration token once, then an agent session or a job token; a cloud
                           VM's Google identity token for /api/agent/acquire
- /, /api/..., /qual       the web UI, behind Cloudflare Access or Google IAP (their signed assertion is verified
                           here too, so a request that bypasses the proxy is refused); `local` only on loopback

/healthz and the agent program (/agent/cerid-agent.py, public code with its SHA-256 in a header) are the only
unauthenticated paths. State-changing UI calls also need the `X-Cerid-Request` header (a cross-site form cannot set
it) and a matching Origin. Every response carries a strict Content-Security-Policy.
"""
from __future__ import annotations

from pathlib import Path
import datetime as dt
import hashlib
import http.server
import json
import re
import threading
import time
import traceback
import urllib.parse

from coordinator import PROTOCOL, VERSION, auth, hosts, quality, reports, state, steps
from coordinator.scheduler import NotFound, Refused, PREFIX

CI = Path(__file__).resolve().parents[1]
WEB = CI / 'web'
AGENT = CI / 'agent/agent.py'
JSON_LIMIT = 4 * 1024 * 1024
CSP = ("default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; "
       "frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
QUAL_CSP = "default-src 'none'; style-src 'unsafe-inline'; img-src data:; frame-ancestors 'self'"
STATIC = {'/': ('index.html', 'text/html; charset=utf-8'), '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
          '/style.css': ('style.css', 'text/css; charset=utf-8')}


class HttpError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def status_of(error):
    if isinstance(error, HttpError):
        return error.status
    if isinstance(error, hosts.WebhookError):
        return error.status
    if isinstance(error, auth.AuthError):
        return 401
    if isinstance(error, NotFound):
        return 404
    if isinstance(error, (Refused, state.TransitionError)):
        return 409
    if isinstance(error, (steps.PipelineError, ValueError, KeyError, TypeError)):
        return 400
    return 500


class Service:
    def __init__(self, coordinator, ui, config=None, registry=None, identity=None, mirror=None, web=WEB,
                 agent_file=AGENT, dashboard=None, qual_data=None, stream_seconds=300.0, stream_interval=1.0):
        self.c = coordinator
        self.ui = ui
        self.config = config or {}
        self.registry = registry
        self.identity = identity
        self.mirror = mirror
        self.web = Path(web)
        self.agent_file = Path(agent_file)
        self.dashboard = dashboard
        self.qual_data = qual_data
        self.qual_cache = (0.0, b'')
        self.stream_seconds = stream_seconds
        self.stream_interval = stream_interval

    def agent_bytes(self):
        data = self.agent_file.read_bytes()
        return data, hashlib.sha256(data).hexdigest()

    def webhook_secret(self, repo):
        name = repo.get('webhook_secret')
        if not name or self.c.secrets is None:
            return ''
        try:
            return self.c.secrets.value(name)
        except Exception:
            return ''

    def webhook(self, repository, headers, body):
        repo = self.c.repository(repository)
        kind = repo.get('host', 'github')
        delivery = hosts.verify(kind, self.webhook_secret(repo), headers, body)
        seen = hashlib.sha256(('%s\0%s' % (repository, delivery)).encode('utf-8')).hexdigest()
        if not self.c.create('deliveries/%s.json' % seen, {'repository': repository, 'at': self.c.clock()}):
            self.c.log('webhook_duplicate', repository=repository, delivery=delivery)
            return {'duplicate': True}
        trigger = hosts.parse(kind, headers, body)
        if trigger is None:
            return {'ignored': True}
        build, created = self.c.create_build(repository, trigger, 'webhook')
        if build is None:
            return {'ignored': True, 'reason': 'fork pull request (the repository does not set build_forks)'}
        return {'build': build['number'], 'created': created}

    def spend(self):
        p = self.c.p
        store = self.c.store
        month = p.month_of(dt.datetime.fromtimestamp(self.c.clock(), dt.timezone.utc))
        cap = float((self.registry or {}).get('budget', {}).get('monthly_cap_usd', 50.0))
        by_device = {}
        for name in store.list('ledger/%s/' % month):
            entry = store.get_json(name) or {}
            item = by_device.setdefault(entry.get('device', '?'), {'device': entry.get('device', '?'), 'usd': 0.0,
                                                                   'hours': 0.0, 'machines': 0})
            item['usd'] = round(item['usd'] + float(entry.get('usd', 0.0)), 4)
            item['hours'] = round(item['hours'] + float(entry.get('hours', 0.0)), 4)
            item['machines'] += 1
        running = []
        for name in store.list('machines/'):
            record = store.get_json(name)
            if record and not record.get('ended'):
                running.append({k: record.get(k) for k in ('machine', 'device', 'job', 'build', 'key', 'started',
                                                           'rate', 'bound_usd')})
        note = store.get_json('budget/gcp-%s.json' % month)
        return {'month': month, 'cap_usd': cap, 'spent_usd': p.month_spend(store, month),
                'gcp_stop': p.gcp_budget_stop(store, month), 'gcp_notification': note,
                'by_device': sorted(by_device.values(), key=lambda d: -d['usd']), 'running': running,
                'running_bound_usd': round(sum(float(r['bound_usd'] or 0) for r in running), 4)}

    def quality(self):
        out = []
        for lane in quality.lanes(self.c.store):
            tests = quality.summary(self.c.store, lane)
            out.append({'lane': lane, 'tests': tests[:200], 'flaky': [t for t in tests if t['flaky']],
                        'failing': [t for t in tests if t['last'] == 'fail']})
        quarantines = [r for r in (self.c.store.get_json(n) for n in self.c.store.list(PREFIX + 'quarantine/')) if r]
        issues = [r for r in (self.c.store.get_json(n) for n in self.c.store.list(PREFIX + 'issues/')) if r]
        bisections = [r for r in (self.c.store.get_json(n) for n in self.c.store.list(PREFIX + 'bisect/')) if r]
        return {'lanes': out, 'quarantine': quarantines, 'issues': issues, 'bisections': bisections,
                'now': self.c.clock()}

    def qual(self):
        stamp, page = self.qual_cache
        if time.time() - stamp < 60 and page:
            return page
        if self.dashboard is None:
            page = b'<!doctype html><title>QUAL</title><p>The QUAL dashboard is not configured.</p>'
        else:
            page = self.dashboard().encode('utf-8')
        self.qual_cache = (time.time(), page)
        return page


SURFACES = ('ui', 'agent', 'webhook')


def make_handler(service, surfaces=SURFACES, ui=None):
    """The request handler of one listener. `surfaces` says which parts this listener serves: `ui` (the web UI
    and its API), `agent` (the agent API, the git mirror, the agent program) and `webhook`; /healthz is everywhere."""
    c = service.c
    viewer = ui or service.ui  # a listener may have its own access mode (the Tailscale add-on beside the IAP tunnel)

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        server_version = 'cerid-coordinator/%s' % VERSION
        sys_version = ''

        def log_message(self, fmt, *args):
            pass

        # Plumbing ------------------------------------------------------------------------------------------
        def body(self, limit=JSON_LIMIT):
            if 'chunked' in (self.headers.get('Transfer-Encoding') or '').lower():
                data = bytearray()
                while True:
                    size = int(self.rfile.readline().split(b';')[0].strip() or b'0', 16)
                    if size == 0:
                        self.rfile.readline()
                        return bytes(data)
                    data += self.rfile.read(size)
                    self.rfile.readline()
                    if len(data) > limit:
                        raise HttpError(413, 'request body too large')
            length = int(self.headers.get('Content-Length') or 0)
            if length > limit:
                raise HttpError(413, 'request body too large')
            return self.rfile.read(length) if length else b''

        def json_body(self):
            data = self.body()
            try:
                return json.loads(data.decode('utf-8')) if data.strip() else {}
            except ValueError:
                raise HttpError(400, 'body is not JSON') from None

        def send(self, status, payload, content_type='application/json', headers=None, csp=CSP):
            if isinstance(payload, (dict, list)):
                payload = (json.dumps(payload, sort_keys=True, default=str) + '\n').encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(payload)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Referrer-Policy', 'no-referrer')
            self.send_header('Content-Security-Policy', csp)
            for key, value in (headers or {}).items():
                self.send_header(key, value)
            self.end_headers()
            if self.command != 'HEAD':
                self.wfile.write(payload)

        def fail(self, error):
            status = status_of(error)
            if status == 500:
                c.log('request_failed', level='error', path=self.path.split('?')[0], error=repr(error)[:300],
                      trace=traceback.format_exc()[-2000:])
                message = 'internal error'
            else:
                message = str(error)[:500]
            self.send(status, {'error': message})

        def route(self):
            url = urllib.parse.urlsplit(self.path)
            return url.path, dict(urllib.parse.parse_qsl(url.query)), url.query

        def token(self):
            token = auth.bearer(self.headers)
            if not token:
                raise auth.AuthError('a bearer token is required')
            return token

        def ui_user(self, changing=False):
            user = viewer.user(self.headers, self.client_address)
            if changing:
                if self.headers.get('X-Cerid-Request') != '1':
                    raise HttpError(403, 'state-changing calls need the X-Cerid-Request header')
                origin = self.headers.get('Origin')
                if origin and urllib.parse.urlsplit(origin).netloc != self.headers.get('Host'):
                    raise HttpError(403, 'cross-origin request refused')
            return user

        def protocol(self):
            sent = self.headers.get('X-Cerid-Agent-Protocol')
            if sent is not None and sent != str(PROTOCOL):
                raise HttpError(426, 'agent protocol %s; this coordinator %s speaks %d' % (sent, VERSION, PROTOCOL))

        # Methods -------------------------------------------------------------------------------------------
        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            try:
                self.dispatch('GET')
            except Exception as error:  # every failure becomes a JSON error with its status
                self.fail(error)

        def do_POST(self):
            try:
                self.dispatch('POST')
            except Exception as error:
                self.fail(error)

        def do_PUT(self):
            try:
                self.dispatch('PUT')
            except Exception as error:
                self.fail(error)

        def dispatch(self, method):
            path, query, raw_query = self.route()
            match = re.match(r'^/webhooks/([A-Za-z0-9_.-]+)$', path)
            agent_path = path.startswith('/api/agent/') or path.startswith('/git/') or path == '/agent/cerid-agent.py'
            if match and 'webhook' not in surfaces:
                raise HttpError(404, 'webhooks are not served on this listener')
            if agent_path and 'agent' not in surfaces:
                raise HttpError(404, 'the agent API is not served on this listener')
            if not (agent_path or match) and path != '/healthz' and 'ui' not in surfaces:
                raise HttpError(404, 'the web UI is not served on this listener')
            if path == '/healthz':
                beat = c.get('coordinator/heartbeat.json') or {}
                age = round(c.clock() - beat.get('at', 0), 1) if beat else None
                return self.send(200, {'ok': True, 'version': VERSION, 'protocol': PROTOCOL, 'tick_age_s': age})
            if path == '/agent/cerid-agent.py':
                data, digest = service.agent_bytes()
                return self.send(200, data, 'text/x-python; charset=utf-8', {'X-Cerid-Agent-SHA256': digest})
            if match and method == 'POST':
                return self.send(200, service.webhook(match.group(1), dict(self.headers.items()), self.body()))
            if path.startswith('/api/agent/'):
                self.protocol()
                return self.agent_api(method, path[len('/api/agent/'):], query)
            if path.startswith('/git/'):
                return self.git(method, path[len('/git'):], raw_query)
            if path.startswith('/api/'):
                return self.ui_api(method, path[len('/api/'):], query)
            if path == '/qual' and method == 'GET':
                self.ui_user()
                return self.send(200, service.qual(), 'text/html; charset=utf-8', csp=QUAL_CSP)
            if path in STATIC and method == 'GET':
                self.ui_user()
                name, kind = STATIC[path]
                return self.send(200, (service.web / name).read_bytes(), kind)
            raise HttpError(404, 'no such page')

        # Agent API -----------------------------------------------------------------------------------------
        def agent_api(self, method, rest, query):
            if rest == 'register' and method == 'POST':
                body = self.json_body()
                agent, session, expires = c.register(body.get('registration_token'), body)
                return self.send(200, {'agent': agent, 'session_token': session, 'expires': expires})
            if rest == 'poll' and method == 'POST':
                agent = c.session_agent(self.token())
                self.json_body()
                assignment = c.poll(agent)
                return self.send(200, assignment or {'job': None, 'wait': 5})
            if rest == 'acquire' and method == 'POST':
                if service.identity is None:
                    raise HttpError(503, 'cloud identity is not configured on this coordinator')
                instance = service.identity(self.json_body().get('identity_token', ''))
                return self.send(200, c.acquire(instance))
            if rest == 'claim' and method == 'POST':
                return self.send(200, c.claim_with_token(self.token()))
            match = re.match(r'^jobs/([0-9a-f]{32})/(start|environment|heartbeat|log|finish|pipeline|annotations|'
                             r'artifacts|meta-data/[^/]+|steps/[^/]+)$', rest)
            if not match:
                raise HttpError(404, 'no such agent call')
            job_id, call = match.groups()
            token = self.token()
            if call == 'start' and method == 'POST':
                job = c.start(job_id, token)
                return self.send(200, {'state': job['state']})
            if call == 'environment' and method == 'POST':
                c.record_environment(job_id, token, self.json_body())
                return self.send(200, {'ok': True})
            if call == 'heartbeat' and method == 'POST':
                return self.send(200, c.heartbeat(job_id, token))
            if call == 'log' and method == 'PUT':
                c.append_log(job_id, token, int(query.get('attempt', 0)), int(query.get('seq', -1)),
                             self.body(c.limits['log_chunk_bytes']))
                return self.send(200, {'ok': True})
            if call == 'finish' and method == 'POST':
                body = self.json_body()
                job = c.finish(job_id, token, int(body.get('exit_status', -1)), infra=body.get('infra'),
                               detail=str(body.get('detail', ''))[:500], timed_out=bool(body.get('timed_out')))
                return self.send(200, {'state': job['state']})
            if call == 'pipeline' and method == 'POST':
                job = c.job_for_token(job_id, token)
                if job['state'] != 'running':
                    raise Refused('only a running job uploads steps')
                created = c.upload(job['build'], self.json_body(), by_job=job_id)
                return self.send(200, {'steps': [j['key'] for j in created]})
            if call == 'annotations' and method == 'POST':
                body = self.json_body()
                c.annotate(job_id, token, body.get('context'), body.get('style'), str(body.get('body', '')),
                           bool(body.get('append')))
                return self.send(200, {'ok': True})
            if call == 'artifacts' and method == 'PUT':
                entry = c.put_artifact(job_id, token, query.get('path', ''), self.body(c.limits['artifact_bytes']),
                                       self.headers.get('X-Content-SHA256', ''))
                return self.send(200, entry)
            if call == 'artifacts' and method == 'GET':
                job = c.job_for_token(job_id, token, writing=False)
                _, data = c.artifact(job['build'], query.get('path', ''))
                return self.send(200, data, 'application/octet-stream')
            if call.startswith('meta-data/'):
                key = urllib.parse.unquote(call[len('meta-data/'):])
                if method == 'PUT':
                    c.set_meta(job_id, token, key, self.json_body().get('value', ''))
                    return self.send(200, {'ok': True})
                return self.send(200, {'value': c.get_meta(job_id, token, key)})
            if call.startswith('steps/') and method == 'GET':
                return self.send(200, c.step_outcome(job_id, token, urllib.parse.unquote(call[len('steps/'):])))
            raise HttpError(405, 'method not allowed')

        def git(self, method, path_info, raw_query):
            if service.mirror is None:
                raise HttpError(404, 'no mirror on this coordinator')
            token = self.token()
            record = c.get('tokens/%s.json' % auth.token_hash(token))
            if not record or record['expires'] < c.clock():
                raise auth.AuthError('unknown or expired job token')
            job = c.job_for_token(record['job'], token, writing=False)
            build = c.build(job['build'])
            if not path_info.startswith('/%s.git/' % build['repository']):
                raise HttpError(403, 'the token is for repository %s' % build['repository'])
            status, headers, payload = service.mirror.serve(method, path_info, raw_query, dict(self.headers.items()),
                                                            self.body(64 * 1024 * 1024) if method == 'POST' else b'')
            return self.send(status, payload, headers.pop('Content-Type', 'application/octet-stream'), headers)

        # UI API --------------------------------------------------------------------------------------------
        def ui_api(self, method, rest, query):
            changing = method != 'GET'
            user = self.ui_user(changing)
            if rest == 'me':
                return self.send(200, {'user': user, 'version': VERSION})
            if rest == 'builds' and method == 'GET':
                return self.send(200, {'builds': c.recent(int(query.get('limit', 50)))})
            if rest == 'builds' and method == 'POST':
                body = self.json_body()
                trigger = {'event': 'push', 'branch': body.get('branch', 'main'), 'commit': body.get('commit', ''),
                           'message': body.get('message', 'manual build by %s' % user), 'trusted': True}
                if not trigger['commit']:
                    repo = c.repository(body.get('repository', ''))
                    trigger['commit'] = hosts.ls_remote(repo['url'], [trigger['branch']])[trigger['branch']]
                env = {k: str(v) for k, v in (body.get('env') or {}).items() if k in ('CERID_CI_TIER',
                                                                                     'CERID_CI_FRESH')}
                build, _ = c.create_build(body.get('repository', ''), trigger, 'manual', env=env, created_by=user)
                return self.send(200, {'build': build['number']})
            match = re.match(r'^builds/(\d+)(/cancel)?$', rest)
            if match:
                number = int(match.group(1))
                if match.group(2) and method == 'POST':
                    c.cancel_build(number, by=user)
                    return self.send(200, {'ok': True})
                return self.send(200, c.build_view(number))
            match = re.match(r'^jobs/([0-9a-f]{32})(/retry|/cancel|/log|/artifacts)?$', rest)
            if match:
                job_id, action = match.groups()
                if action == '/retry' and method == 'POST':
                    return self.send(200, {'state': c.retry_job(job_id, by=user)['state']})
                if action == '/cancel' and method == 'POST':
                    job = c.cancel_job(job_id, reason='canceled by %s' % user)
                    c.audit(user, 'job-cancel', 'build %d %s' % (job['build'], job['key']), '')
                    return self.send(200, {'ok': True})
                if action == '/log':
                    data, nxt, live, job = c.read_log(job_id, query.get('attempt'), int(query.get('from', 0)))
                    if query.get('raw'):
                        return self.send(200, data, 'text/plain; charset=utf-8', {
                            'Content-Disposition': 'attachment; filename="%s-%s.log"' % (job['key'].replace(
                                '"', ''), query.get('attempt') or job['attempt'])})
                    return self.send(200, {'text': data.decode('utf-8', errors='replace'), 'next': nxt,
                                           'live': live, 'state': job['state'], 'attempt': job['attempt']})
                if action == '/artifacts':
                    job = c.job(job_id)
                    if query.get('path'):
                        _, data = c.artifact(job['build'], query['path'], job_id=job_id)
                        return self.send(200, data, 'application/octet-stream', {
                            'Content-Disposition': 'attachment; filename="%s"' % Path(query['path']).name.replace(
                                '"', '')})
                    return self.send(200, {'artifacts': [a for a in c.artifacts(job['build']) if a['job'] == job_id]})
                return self.send(200, c.job(job_id) | {'token_hash': None})
            if rest == 'agents':
                return self.send(200, {'agents': [{k: a.get(k) for k in (
                    'id', 'name', 'queue', 'tags', 'os', 'arch', 'version', 'mode', 'status', 'job', 'last_seen',
                    'registered')} for a in c.agents()]})
            match = re.match(r'^agents/([0-9a-f]{24})/stop$', rest)
            if match and method == 'POST':
                c.stop_agent(match.group(1), by=user)
                return self.send(200, {'ok': True})
            if rest == 'queues':
                return self.send(200, {'queues': c.queues()})
            if rest == 'spend':
                return self.send(200, service.spend())
            if rest == 'costs':
                month = query.get('month') or c.p.month_of(dt.datetime.fromtimestamp(c.clock(), dt.timezone.utc))
                return self.send(200, reports.costs(c, service.registry or {'budget': {'monthly_cap_usd': 50.0}},
                                                    month))
            if rest == 'slo':
                return self.send(200, reports.slo(c, config=service.config.get('slo')))
            if rest == 'audit':
                return self.send(200, {'entries': reports.audit_entries(c, days=int(query.get('days', 7)))})
            if rest == 'quality':
                return self.send(200, service.quality())
            if rest == 'quality/test':
                return self.send(200, {'history': quality.history(c.store, query['lane'], query['test'])})
            if rest == 'quality/release' and method == 'POST':
                body = self.json_body()
                return self.send(200, quality.release(c, body['lane'], body['test'], user))
            if rest == 'qual':
                if service.qual_data is None:
                    raise HttpError(404, 'the QUAL matrix is not configured')
                return self.send(200, service.qual_data())
            if rest == 'stream':
                return self.stream(query)
            if rest == 'log':
                return self.send(200, {'records': c.log.records[-200:]})
            raise HttpError(404, 'no such call')

        def stream(self, query):
            """Server-Sent Events: `build=N` sends the build view whenever it changes, `job=ID` the new log text and
            the job's state, otherwise the recent builds. A stream lasts at most `stream_seconds`; the browser's
            EventSource reconnects by itself."""
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Content-Security-Policy', CSP)
            self.end_headers()
            self.close_connection = True
            deadline = time.time() + service.stream_seconds
            last = None
            seq = int(query.get('from', 0))
            beat = time.time()
            try:
                while time.time() < deadline:
                    events = []
                    if query.get('job'):
                        data, seq_next, live, job = c.read_log(query['job'], query.get('attempt'), seq)
                        if data:
                            events.append(('log', {'text': data.decode('utf-8', errors='replace'), 'next': seq_next}))
                            seq = seq_next
                        status = {'state': job['state'], 'reason': job.get('reason', ''), 'attempt': job['attempt'],
                                  'live': live}
                        if status != last:
                            events.append(('job', status))
                            last = status
                        if not live:
                            events.append(('end', status))
                    else:
                        payload = c.build_view(int(query['build'])) if query.get('build') else {
                            'builds': c.recent(int(query.get('limit', 50)))}
                        text = json.dumps(payload, sort_keys=True, default=str)
                        if text != last:
                            events.append(('build' if query.get('build') else 'builds', payload))
                            last = text
                    for name, payload in events:
                        self.wfile.write(('event: %s\ndata: %s\n\n' % (name, json.dumps(
                            payload, default=str))).encode('utf-8'))
                    if not events and time.time() - beat > 15:
                        self.wfile.write(b': keep-alive\n\n')
                        beat = time.time()
                    self.wfile.flush()
                    if any(name == 'end' for name, _ in events):
                        return
                    time.sleep(service.stream_interval)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                return

    return Handler


def serve(service, host='127.0.0.1', port=8780, surfaces=SURFACES, ui=None):
    server = http.server.ThreadingHTTPServer((host, port), make_handler(service, surfaces, ui))
    server.daemon_threads = True
    return server


class Loop:
    """The coordinator's background work: the watchdog tick, the dispatcher, polling, schedules and retention.
    Every step is independent and logged; one failing never stops the others."""

    def __init__(self, coordinator, dispatcher=None, interval=5.0, poll=None, schedules=None, quality=None,
                 quality_seconds=300.0):
        self.c = coordinator
        self.dispatcher = dispatcher
        self.interval = interval
        self.poll = poll
        self.schedules = schedules
        self.stop = threading.Event()
        self.last_sweep = 0.0
        self.last_orphans = 0.0
        self.quality = quality
        self.quality_seconds = quality_seconds
        self.last_quality = 0.0

    def once(self):
        for name, work in (('tick', self.c.tick), ('dispatch', self.dispatcher.tick if self.dispatcher else None),
                           ('poll', self.poll), ('schedules', self.schedules)):
            if work is None:
                continue
            try:
                work()
            except Exception as error:
                self.c.log('loop_failed', level='error', step=name, error=repr(error)[:300],
                           trace=traceback.format_exc()[-1500:])
        now = time.time()
        if now - self.last_sweep > 3600:
            self.last_sweep = now
            try:
                self.c.sweep()
            except Exception as error:
                self.c.log('sweep_failed', level='error', error=repr(error)[:300])
        if self.quality is not None and now - self.last_quality > self.quality_seconds:
            self.last_quality = now
            try:
                self.quality()
            except Exception as error:
                self.c.log('quality_failed', level='error', error=repr(error)[:300],
                           trace=traceback.format_exc()[-1500:])
        if self.dispatcher is not None and now - self.last_orphans > 600:
            self.last_orphans = now
            try:
                self.dispatcher.sweep_orphans()
            except Exception as error:
                self.c.log('orphans_failed', level='error', error=repr(error)[:300])

    def run(self):
        while not self.stop.is_set():
            self.once()
            self.stop.wait(self.interval)

    def start(self):
        thread = threading.Thread(target=self.run, daemon=True)
        thread.start()
        return thread
