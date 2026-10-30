#!/usr/bin/env python3
"""Contract tests of the Cerid orchestrator (ADR-0135 §12): the state machines, the pipeline document, the
compare-and-swap store (local and a Cloud Storage fake), authentication, the host adapters, the coordinator core, the
cloud dispatcher, and fault injection (lost agent, preempted VM, duplicate webhook, coordinator restart, concurrent
compare-and-swap). No cloud and no account: every external service is a fake. Run: python
scripts/test-ci-orchestrator.py (stdlib unittest). The end-to-end run is scripts/test-ci-orchestrator-e2e.py.
"""
from pathlib import Path
import base64
import hashlib
import http.server
import importlib.util
import io
import json
import os
import random
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'ci'))


def load(name, relative):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


pipeline = load('ci_pipeline', 'ci/pipeline.py')
from coordinator import auth, dispatch, hosts, scheduler, service, state, steps, triggers  # noqa: E402
from executors import gce_vm  # noqa: E402

REGISTRY = pipeline.load_registry(packs=[])


class Clock:
    def __init__(self, start=1_800_000_000.0):
        self.now = start

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


def fresh_store():
    scratch = tempfile.TemporaryDirectory()
    return scratch, pipeline.LocalStore(scratch.name)


CONFIG = {'url': 'https://ci.example', 'repositories': {
    'cerid': {'url': 'https://git.example/cerid.git', 'host': 'github', 'webhook_secret': 'hook',
              'trusted_branches': ['main'], 'pipeline': {'command': 'python ci/pipeline.py upload',
                                                         'queue': 'cerid-launcher'}}}}
COMMIT = 'a' * 40


def coordinator(store, clock=None, **kw):
    return scheduler.Coordinator(store, CONFIG, clock=clock or Clock(), log=scheduler.JsonLog(io.StringIO()), **kw)


def agent(c, queue='cerid-launcher', tags=None, name='a1'):
    token = c.create_registration(name, queue, tags)
    record, session, _ = c.register(token, {'name': name, 'protocol': 1, 'os': 'linux', 'arch': 'x86_64'})
    return record


def finish_plan(c, build, document):
    """Run the bootstrap `plan` job on a launcher agent: it uploads `document` and passes."""
    a = agent(c, name='launcher-%d' % build['number'])
    got = c.poll(a)
    job_id = got['job']['id']
    c.start(job_id, got['token'])
    c.upload(build['number'], document, by_job=job_id)
    c.finish(job_id, got['token'], 0)
    return got


# --------------------------------------------------------------------------------------------- state machine

class StateMachine(unittest.TestCase):
    def test_exactly_the_ten_job_states(self):
        self.assertEqual(state.JOB_STATES, ('scheduled', 'assigned', 'running', 'passed', 'failed', 'canceled',
                                            'timed_out', 'expired', 'budget', 'cached'))
        self.assertEqual(state.TERMINAL | {'scheduled', 'assigned', 'running'}, set(state.JOB_STATES))

    def test_every_listed_transition_is_allowed_and_every_other_is_refused(self):
        events = {e for v in state.TRANSITIONS.values() for e in v} | {'bogus'}
        for source in state.JOB_STATES:
            for target in state.JOB_STATES:
                for event in events:
                    job = {'state': source}
                    allowed = event in state.TRANSITIONS.get((source, target), ())
                    if allowed:
                        state.transition(job, target, event, 1.0, 'why')
                        self.assertEqual(job['state'], target)
                        self.assertEqual(job['history'][-1], {'at': 1.0, 'from': source, 'to': target, 'event': event,
                                                              'reason': 'why'})
                    else:
                        with self.assertRaises(state.TransitionError, msg='%s->%s by %s' % (source, target, event)):
                            state.transition(job, target, event, 1.0)
                        self.assertEqual(job['state'], source)

    def test_terminal_states_only_leave_by_a_retry_and_passed_cached_never(self):
        for (source, target), events in state.TRANSITIONS.items():
            if source in state.TERMINAL:
                self.assertEqual((target, events), ('scheduled', ('retry',)))
        self.assertFalse([k for k in state.TRANSITIONS if k[0] in ('passed', 'cached')])

    def test_build_transitions(self):
        for source in state.BUILD_STATES:
            for target in state.BUILD_STATES:
                for event in ('start', 'finish', 'retry'):
                    build = {'state': source}
                    if event in state.BUILD_TRANSITIONS.get((source, target), ()):
                        state.build_transition(build, target, event, 5.0)
                        self.assertEqual(build['state'], target)
                    else:
                        with self.assertRaises(state.TransitionError):
                            state.build_transition(build, target, event, 5.0)

    def test_outcome_soft_fail_and_dependencies(self):
        build = {'cancel_requested': False}
        passed = {'key': 'a', 'state': 'passed'}
        soft = {'key': 'b', 'state': 'failed', 'soft_fail': True, 'exit_status': 3}
        soft_list = {'key': 'c', 'state': 'failed', 'soft_fail': [{'exit_status': 3}], 'exit_status': 3}
        hard = {'key': 'd', 'state': 'failed', 'soft_fail': [{'exit_status': 4}], 'exit_status': 3}
        self.assertEqual(state.build_outcome(build, [passed, soft, soft_list, {'state': 'cached'},
                                                     {'state': 'budget'}]), 'passed')
        self.assertEqual(state.build_outcome(build, [passed, hard]), 'failed')
        self.assertIsNone(state.build_outcome(build, [passed, {'state': 'running'}]))
        self.assertEqual(state.build_outcome({'cancel_requested': True}, [passed]), 'canceled')
        by_key = {j['key']: j for j in (passed, soft, hard)}
        self.assertEqual(state.dependency_state({'depends_on': [{'step': 'a'}, {'step': 'b'}]}, by_key), 'ready')
        self.assertTrue(state.dependency_state({'depends_on': [{'step': 'd'}]}, by_key).startswith('broken'))
        self.assertEqual(state.dependency_state({'depends_on': [{'step': 'd', 'allow_failure': True}]}, by_key),
                         'ready')
        self.assertEqual(state.dependency_state({'depends_on': [{'step': 'x'}]}, by_key), 'waiting')

    def test_tags_and_retry_rules(self):
        self.assertTrue(state.tags_match({'queue': 'q', 'device': 'd1'}, {'queue': 'q', 'device': 'd1', 'os': 'l'}))
        self.assertFalse(state.tags_match({'device': 'd1'}, {'device': 'd2'}))
        self.assertTrue(state.tags_match({'os': '*'}, {'os': 'windows'}))
        self.assertFalse(state.tags_match({'gpu': '*'}, {}))
        job = {'retry': {'automatic': [{'reason': 'preempted', 'limit': 3}, {'exit_status': 9, 'limit': 1}]}}
        self.assertEqual(state.retry_limit(job, reason='preempted'), 3)
        self.assertEqual(state.retry_limit(job, reason='agent_lost'), 0)
        self.assertEqual(state.retry_limit(job, exit_status=9), 1)
        self.assertEqual(state.retry_limit(job, exit_status=1), 0)


# --------------------------------------------------------------------------------------------- pipeline document

class Document(unittest.TestCase):
    def test_wait_steps_dependencies_and_defaults(self):
        specs, timeout = steps.normalize({'timeout_in_minutes': 60, 'env': {'A': '1'}, 'steps': [
            {'key': 'a', 'command': 'x'}, {'key': 'b', 'command': ['y', 'z'], 'env': {'B': 2}}, 'wait',
            {'key': 'c', 'command': 'c', 'depends_on': 'a'},
            {'wait': None, 'continue_on_failure': True}, {'command': 'd', 'agents': {'queue': 'q', 'gpu': '*'}}]})
        self.assertEqual(timeout, 60)
        by = {s['key']: s for s in specs}
        self.assertEqual(by['b']['command'], 'y\nz')
        self.assertEqual(by['b']['env'], {'A': '1', 'B': '2'})
        self.assertEqual([d['step'] for d in by['c']['depends_on']], ['a', 'b'])
        self.assertEqual(by['step-6']['depends_on'], [{'step': 'a', 'allow_failure': False},
                                                      {'step': 'b', 'allow_failure': False},
                                                      {'step': 'c', 'allow_failure': True}])
        self.assertEqual(by['step-6']['agents'], {'queue': 'q', 'gpu': '*'})
        self.assertEqual(by['a']['agents'], {'queue': 'default'})

    def test_every_mistake_is_refused(self):
        cases = [
            ({'steps': [{'key': 'a', 'command': 'x', 'comand': 'y'}]}, 'unknown fields'),
            ({'steps': [{'key': 'a', 'command': 'x'}, {'key': 'a', 'command': 'y'}]}, 'duplicate key'),
            ({'steps': [{'key': 'a', 'command': 'x', 'depends_on': 'nope'}]}, 'unknown step'),
            ({'steps': [{'key': 'a', 'command': 'x', 'depends_on': 'b'}, {'key': 'b', 'command': 'y',
                                                                         'depends_on': 'a'}]}, 'cycle'),
            ({'steps': [{'key': 'a'}]}, 'command is required'),
            ({'steps': [{'key': 'a', 'command': 'x', 'skip': 'later'}]}, 'skip is'),
            ({'steps': [{'key': 'a', 'command': 'x', 'artifact_paths': '../x'}]}, 'leaves the checkout'),
            ({'steps': [{'key': 'a', 'command': 'x', 'concurrency': 2}]}, 'go together'),
            ({'steps': [{'key': 'a', 'command': 'x', 'retry': {'automatic': [{'reason': 'flaky'}]}}]},
             'unknown retry reason'),
            ({'steps': [{'key': 'a b', 'command': 'x'}]}, 'bad key'),
            ({'schema': 'other/1', 'steps': []}, 'schema'),
        ]
        for document, problem in cases:
            with self.assertRaises(steps.PipelineError) as raised:
                steps.normalize(document)
            self.assertIn(problem, str(raised.exception))
        with self.assertRaises(steps.PipelineError):
            steps.normalize({'steps': [{'key': 'a', 'command': 'x'}]}, existing_keys={'a'})

    def test_allow_dependency_failure_covers_the_wait_barrier_too(self):
        specs, _ = steps.normalize({'steps': [{'key': 'a', 'command': 'x'}, 'wait',
                                              {'key': 'r', 'command': 'x', 'allow_dependency_failure': True,
                                               'depends_on': ['a']}]})
        self.assertEqual(specs[1]['depends_on'], [{'step': 'a', 'allow_failure': True}])
        specs, _ = steps.normalize({'steps': [{'key': 'a', 'command': 'x'}, 'wait',
                                              {'key': 'r', 'command': 'x', 'allow_dependency_failure': True}]})
        self.assertEqual(specs[1]['depends_on'], [{'step': 'a', 'allow_failure': True}])

    def test_skip_secret_and_retry_forms(self):
        specs, _ = steps.normalize({'steps': [
            {'key': 'a', 'command': 'x', 'skip': 'cached: passed in build 4'},
            {'key': 'b', 'command': 'x', 'secrets': ['cloudflare-pages-token', {'name': 'k', 'env': 'K'}],
             'retry': {'automatic': True}, 'soft_fail': [{'exit_status': 2}]}]})
        self.assertEqual(specs[0]['skip'], {'state': 'cached', 'reason': 'cached: passed in build 4'})
        self.assertEqual(specs[1]['secrets'], [{'name': 'cloudflare-pages-token', 'env': 'CLOUDFLARE_PAGES_TOKEN'},
                                               {'name': 'k', 'env': 'K'}])
        self.assertEqual(specs[1]['retry']['automatic'], [{'reason': 'infrastructure', 'limit': 2}])


# --------------------------------------------------------------------------------------------- store CAS

class FakeGcs(http.server.BaseHTTPRequestHandler):
    """The four Cloud Storage JSON API calls the store makes, with real generation preconditions."""
    objects = {}
    lock = threading.Lock()
    counter = [1000]

    def log_message(self, *args):
        pass

    def _send(self, status, body=b'', headers=None):
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _parts(self):
        url = urllib.parse.urlparse(self.path)
        return url.path, dict(urllib.parse.parse_qsl(url.query))

    def do_GET(self):
        path, query = self._parts()
        if '/o/' in path:
            name = urllib.parse.unquote(path.split('/o/', 1)[1])
            with self.lock:
                item = self.objects.get(name)
            if item is None:
                return self._send(404, b'{}')
            return self._send(200, item[0], {'x-goog-generation': str(item[1])})
        with self.lock:
            names = sorted(n for n in self.objects if n.startswith(query.get('prefix', '')))
        start = int(query.get('pageToken', 0))
        page = {'items': [{'name': n} for n in names[start:start + 2]]}
        if start + 2 < len(names):
            page['nextPageToken'] = str(start + 2)
        self._send(200, json.dumps(page).encode())

    def do_POST(self):
        _, query = self._parts()
        data = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        name = query['name']
        with self.lock:
            current = self.objects.get(name, (None, 0))[1]
            if 'ifGenerationMatch' in query and int(query['ifGenerationMatch']) != current:
                return self._send(412, b'{"error": "conditionNotMet"}')
            self.counter[0] += 1
            self.objects[name] = (data, self.counter[0])
            generation = self.counter[0]
        self._send(200, json.dumps({'name': name, 'generation': str(generation)}).encode())

    def do_DELETE(self):
        path, query = self._parts()
        name = urllib.parse.unquote(path.split('/o/', 1)[1])
        with self.lock:
            if name not in self.objects:
                return self._send(404, b'{}')
            if 'ifGenerationMatch' in query and int(query['ifGenerationMatch']) != self.objects[name][1]:
                return self._send(412, b'{}')
            del self.objects[name]
        self._send(204)


def serve(handler):
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def increment(store, name, times):
    for _ in range(times):
        pipeline.update_json(store, name, lambda c: {'n': (c or {'n': 0})['n'] + 1}, attempts=500)


class Cas(unittest.TestCase):
    def contract(self, store):
        self.assertEqual(store.read('a/x.json'), (None, 0))
        g1 = store.write_if('a/x.json', b'1', 0)
        self.assertTrue(g1)
        self.assertIsNone(store.write_if('a/x.json', b'2', 0), 'create-only must lose to an existing object')
        data, generation = store.read('a/x.json')
        self.assertEqual((data, generation), (b'1', g1))
        g2 = store.write_if('a/x.json', b'2', g1)
        self.assertGreater(g2, g1)
        self.assertIsNone(store.write_if('a/x.json', b'3', g1), 'a stale generation must lose')
        self.assertEqual(store.get('a/x.json'), b'2')
        self.assertFalse(store.delete('a/x.json', g1))
        self.assertTrue(store.delete('a/x.json', g2))
        self.assertEqual(store.read('a/x.json'), (None, 0))
        for i in range(5):
            store.put_json('a/list/%d.json' % i, {'i': i})
        self.assertEqual(store.list('a/list/'), ['a/list/%d.json' % i for i in range(5)])

    def test_local_backend_contract(self):
        scratch, store = fresh_store()
        with scratch:
            self.contract(store)
            self.assertNotIn('.cas', ' '.join(store.list('')))
            with self.assertRaises(ValueError):
                store.read('.cas/a/x.json.gen')

    def test_local_generations_never_repeat_after_a_delete(self):
        scratch, store = fresh_store()
        with scratch:
            g1 = store.write_if('k', b'1', 0)
            store.delete('k')
            g2 = store.write_if('k', b'2', 0)
            self.assertGreater(g2, g1)

    def test_gcs_backend_contract_through_real_http(self):
        FakeGcs.objects = {}
        server = serve(FakeGcs)
        try:
            store = pipeline.GcsStore('bucket', 'pre', token=lambda: 't',
                                      endpoint='http://127.0.0.1:%d' % server.server_address[1])
            self.contract(store)
            self.assertTrue(all(n.startswith('pre/') for n in FakeGcs.objects))
        finally:
            server.shutdown()
            server.server_close()

    def test_concurrent_threads_lose_no_update(self):
        scratch, store = fresh_store()
        with scratch:
            threads = [threading.Thread(target=increment, args=(store, 'counter.json', 25)) for _ in range(4)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            self.assertEqual(store.get_json('counter.json'), {'n': 100})

    def test_concurrent_processes_lose_no_update(self):
        scratch, store = fresh_store()
        code = ('import sys; sys.path.insert(0, r"%s"); import importlib.util as u; '
                's = u.spec_from_file_location("p", r"%s"); m = u.module_from_spec(s); sys.modules["p"] = m; '
                's.loader.exec_module(m); st = m.LocalStore(sys.argv[1]); '
                '[m.update_json(st, "c.json", lambda c: {"n": (c or {"n": 0})["n"] + 1}, attempts=2000) '
                'for _ in range(20)]') % (ROOT / 'ci', ROOT / 'ci/pipeline.py')
        with scratch:
            procs = [subprocess.Popen([sys.executable, '-c', code, scratch.name]) for _ in range(3)]
            self.assertEqual([p.wait(timeout=120) for p in procs], [0, 0, 0])
            self.assertEqual(store.get_json('c.json'), {'n': 60})

    def test_concurrent_gcs_writers_lose_no_update(self):
        FakeGcs.objects = {}
        server = serve(FakeGcs)
        try:
            store = pipeline.GcsStore('b', '', token=lambda: 't',
                                      endpoint='http://127.0.0.1:%d' % server.server_address[1])
            threads = [threading.Thread(target=increment, args=(store, 'n.json', 10)) for _ in range(3)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            self.assertEqual(store.get_json('n.json'), {'n': 30})
        finally:
            server.shutdown()
            server.server_close()

    @unittest.skipUnless(os.name == 'nt', 'Windows sharing violations')
    def test_a_rename_rides_out_a_sharing_violation(self):
        from unittest import mock
        with mock.patch('os.replace', side_effect=[PermissionError(13, 'denied'), PermissionError(13, 'denied'),
                                                   None]) as replace:
            pipeline.replace_file('a', 'b', sleep=lambda s: None)
        self.assertEqual(replace.call_count, 3)
        with mock.patch('os.replace', side_effect=PermissionError(13, 'denied')):
            with self.assertRaises(PermissionError):
                pipeline.replace_file('a', 'b', attempts=3, sleep=lambda s: None)

    def test_a_live_lock_raises_instead_of_overwriting(self):
        class Racing:
            def read(self, name):
                return b'{"n": 1}', 1

            def write_if(self, name, data, generation):
                return None
        with self.assertRaises(pipeline.Conflict):
            pipeline.update_json(Racing(), 'x', lambda c: {'n': 2}, attempts=3, sleep=lambda s: None)


# --------------------------------------------------------------------------------------------- authentication

def is_probable_prime(n, rounds=20):
    if n < 4:
        return n in (2, 3)
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29):
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    rng = random.Random(n)
    for _ in range(rounds):
        x = pow(rng.randrange(2, n - 2), d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def rsa_key(bits=1024, seed=7):
    rng = random.Random(seed)
    e = 65537
    while True:
        primes = []
        while len(primes) < 2:
            candidate = rng.getrandbits(bits // 2) | (1 << (bits // 2 - 1)) | 1
            if is_probable_prime(candidate) and (candidate - 1) % e:
                primes.append(candidate)
        p, q = primes
        n = p * q
        if n.bit_length() == bits:
            return n, e, pow(e, -1, (p - 1) * (q - 1))


RSA = rsa_key()


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


def rs256(claims, kid='k1', key=RSA, header=None):
    n, _, d = key
    head = b64(json.dumps(header or {'alg': 'RS256', 'kid': kid}).encode())
    body = b64(json.dumps(claims).encode())
    k = (n.bit_length() + 7) // 8
    digest = auth.SHA256_PREFIX + hashlib.sha256(('%s.%s' % (head, body)).encode()).digest()
    em = b'\x00\x01' + b'\xff' * (k - len(digest) - 3) + b'\x00' + digest
    return '%s.%s.%s' % (head, body, b64(pow(int.from_bytes(em, 'big'), d, n).to_bytes(k, 'big')))


def rsa_jwk(kid='k1', key=RSA):
    n, e, _ = key
    return {'kid': kid, 'kty': 'RSA', 'alg': 'RS256', 'n': b64(n.to_bytes((n.bit_length() + 7) // 8, 'big')),
            'e': b64(e.to_bytes(3, 'big'))}


EC_D = 0x1d3f5b7c9e2a4c6e8f0a1b3c5d7e9f1a2b4c6d8e0f1a3b5c7d9e1f2a4b6c8d0e
EC_Q = auth.ec_mul(EC_D, auth.G)


def es256(claims, kid='e1'):
    head = b64(json.dumps({'alg': 'ES256', 'kid': kid}).encode())
    body = b64(json.dumps(claims).encode())
    z = int.from_bytes(hashlib.sha256(('%s.%s' % (head, body)).encode()).digest(), 'big')
    rng = random.Random(z)
    while True:
        k = rng.randrange(1, auth.N)
        r = auth.ec_mul(k, auth.G)[0] % auth.N
        s = pow(k, -1, auth.N) * (z + r * EC_D) % auth.N
        if r and s:
            break
    return '%s.%s.%s' % (head, body, b64(r.to_bytes(32, 'big') + s.to_bytes(32, 'big')))


def ec_jwk(kid='e1'):
    return {'kid': kid, 'kty': 'EC', 'crv': 'P-256', 'x': b64(EC_Q[0].to_bytes(32, 'big')),
            'y': b64(EC_Q[1].to_bytes(32, 'big'))}


NOW = 1_800_000_000


class Auth(unittest.TestCase):
    def jwks(self, *keys):
        return auth.Jwks('https://certs', lambda url: {'keys': list(keys)}, clock=lambda: NOW)

    def claims(self, **extra):
        base = {'iss': 'https://accounts.google.com', 'aud': 'https://ci.example', 'exp': NOW + 300, 'iat': NOW}
        base.update(extra)
        return base

    def test_rs256_verification_and_every_refusal(self):
        jwks = self.jwks(rsa_jwk())
        verify = lambda token, **kw: auth.verify_jwt(token, jwks, 'https://ci.example', auth.GOOGLE_ISSUERS,
                                                     now=NOW, **kw)
        self.assertEqual(verify(rs256(self.claims(sub='x')))['sub'], 'x')
        good = rs256(self.claims())
        head, body, sig = good.split('.')
        tampered = '%s.%s.%s' % (head, b64(json.dumps(self.claims(sub='evil')).encode()), sig)
        other = rsa_key(seed=11)
        refusals = [
            (tampered, 'bad signature'),
            (rs256(self.claims(), key=other), 'bad signature'),
            (rs256(self.claims(exp=NOW - 120)), 'expired'),
            (rs256(self.claims(nbf=NOW + 600)), 'not yet valid'),
            (rs256(self.claims(aud='https://other')), 'audience'),
            (rs256(self.claims(iss='https://evil')), 'issuer'),
            (rs256(self.claims(), kid='nope'), 'unknown signing key'),
            (rs256(self.claims(), header={'alg': 'none', 'kid': 'k1'}), 'algorithm'),
            (rs256(self.claims(), header={'alg': 'HS256', 'kid': 'k1'}), 'algorithm'),
            ('a.b', 'malformed'),
        ]
        for token, why in refusals:
            with self.assertRaises(auth.AuthError) as raised:
                verify(token)
            self.assertIn(why, str(raised.exception))

    def test_an_rsa_key_cannot_verify_es256_and_back(self):
        jwks = self.jwks(dict(rsa_jwk(), kid='e1'))
        with self.assertRaises(auth.AuthError):
            auth.verify_jwt(es256(self.claims()), jwks, 'https://ci.example', auth.GOOGLE_ISSUERS, now=NOW)

    def test_es256_verification(self):
        self.assertTrue(auth.on_curve(EC_Q))
        jwks = self.jwks(ec_jwk())
        claims = auth.verify_jwt(es256(self.claims(email='a@b')), jwks, 'https://ci.example', auth.GOOGLE_ISSUERS,
                                 now=NOW)
        self.assertEqual(claims['email'], 'a@b')
        token = es256(self.claims())
        head, body, sig = token.split('.')
        raw = bytearray(auth.b64url(sig))
        raw[5] ^= 1
        with self.assertRaises(auth.AuthError):
            auth.verify_jwt('%s.%s.%s' % (head, body, b64(bytes(raw))), jwks, 'https://ci.example',
                            auth.GOOGLE_ISSUERS, now=NOW)

    def test_google_instance_identity(self):
        claims = self.claims(email='runner@cerid-ci.iam.gserviceaccount.com', email_verified=True,
                             google={'compute_engine': {'project_id': 'cerid-ci', 'instance_name': 'cerid-ci-x',
                                                        'zone': 'us-central1-a'}})
        self.assertEqual(auth.google_instance(claims, 'runner@cerid-ci.iam.gserviceaccount.com', 'cerid-ci'),
                         ('cerid-ci-x', 'us-central1-a'))
        for bad in (dict(claims, email='other@x'), dict(claims, email_verified=False),
                    dict(claims, google={'compute_engine': {'project_id': 'other', 'instance_name': 'x'}})):
            with self.assertRaises(auth.AuthError):
                auth.google_instance(bad, 'runner@cerid-ci.iam.gserviceaccount.com', 'cerid-ci')

    def test_ui_modes_and_no_anonymous_access(self):
        fetch = lambda url: {'keys': [rsa_jwk()] if 'cloudflareaccess' in url else [ec_jwk()]}
        cf = auth.UiAuth('cloudflare-access', audience='aud-tag', team='cerid.cloudflareaccess.com', fetch=fetch,
                         allowed=['me@example.com'], clock=lambda: NOW)
        token = rs256({'iss': 'https://cerid.cloudflareaccess.com', 'aud': ['aud-tag'], 'exp': NOW + 60,
                       'email': 'me@example.com'})
        self.assertEqual(cf.user({'Cf-Access-Jwt-Assertion': token}), 'me@example.com')
        with self.assertRaises(auth.AuthError):
            cf.user({})
        stranger = rs256({'iss': 'https://cerid.cloudflareaccess.com', 'aud': 'aud-tag', 'exp': NOW + 60,
                          'email': 'x@example.com'})
        with self.assertRaises(auth.AuthError):
            cf.user({'Cf-Access-Jwt-Assertion': stranger})
        iap = auth.UiAuth('iap', audience='/projects/1/global/backendServices/2', fetch=fetch, clock=lambda: NOW)
        token = es256({'iss': 'https://cloud.google.com/iap', 'aud': '/projects/1/global/backendServices/2',
                       'exp': NOW + 60, 'email': 'me@example.com'})
        self.assertEqual(iap.user({'x-goog-iap-jwt-assertion': token}), 'me@example.com')
        with self.assertRaises(ValueError):
            auth.UiAuth('local', loopback=False)
        with self.assertRaises(ValueError):
            auth.UiAuth('anonymous')
        self.assertEqual(auth.UiAuth('local', loopback=True).user({}), 'local-operator')

    def test_tokens_are_random_and_stored_hashed(self):
        a, b = auth.new_token('cjt'), auth.new_token('cjt')
        self.assertNotEqual(a, b)
        self.assertGreaterEqual(len(a), 40)
        self.assertEqual(auth.token_hash(a), hashlib.sha256(a.encode()).hexdigest())
        self.assertEqual(auth.bearer({'Authorization': 'Bearer ' + a}), a)
        self.assertIsNone(auth.bearer({'Authorization': 'Basic x'}))


# --------------------------------------------------------------------------------------------- host adapters

def github_push(branch='main', commit=COMMIT, **extra):
    payload = {'ref': 'refs/heads/' + branch, 'after': commit, 'before': 'b' * 40,
               'head_commit': {'message': 'm', 'author': {'name': 'n'}}, 'repository': {'full_name': 'o/cerid'}}
    payload.update(extra)
    return json.dumps(payload).encode()


class Hosts(unittest.TestCase):
    def test_signatures_are_checked_on_the_raw_body(self):
        body = github_push()
        good = {'X-Hub-Signature-256': 'sha256=' + hosts.hmac_hex('s', body), 'X-GitHub-Delivery': 'd1'}
        self.assertEqual(hosts.verify('github', 's', good, body), 'd1')
        for headers, data in ((good, body + b' '), ({'X-Hub-Signature-256': 'sha256=00'}, body), ({}, body)):
            with self.assertRaises(hosts.WebhookError) as raised:
                hosts.verify('github', 's', headers, data)
            self.assertEqual(raised.exception.status, 401)
        with self.assertRaises(hosts.WebhookError) as raised:
            hosts.verify('github', '', good, body)
        self.assertEqual(raised.exception.status, 503)
        self.assertEqual(hosts.verify('gitea', 's', {'X-Gitea-Signature': hosts.hmac_hex('s', body),
                                                     'X-Gitea-Delivery': 'g'}, body), 'g')
        self.assertTrue(hosts.verify('gitlab', 's', {'X-Gitlab-Token': 's'}, body))
        with self.assertRaises(hosts.WebhookError):
            hosts.verify('gitlab', 's', {'X-Gitlab-Token': 't'}, body)

    def test_generic_signature_has_a_replay_window(self):
        body = b'{"event": "push", "branch": "main", "commit": "%s"}' % COMMIT.encode()
        headers = {'X-Cerid-Signature': hosts.sign_generic('s', body, now=NOW), 'X-Cerid-Delivery': 'x'}
        self.assertEqual(hosts.verify('generic', 's', headers, body, now=NOW + 10), 'x')
        with self.assertRaises(hosts.WebhookError):
            hosts.verify('generic', 's', headers, body, now=NOW + 301)
        with self.assertRaises(hosts.WebhookError):
            hosts.verify('generic', 's', {'X-Cerid-Signature': 't=%d,v1=00' % NOW}, body, now=NOW)
        self.assertEqual(hosts.parse('generic', headers, body)['branch'], 'main')

    def test_events(self):
        push = hosts.parse('github', {'X-GitHub-Event': 'push'}, github_push())
        self.assertEqual((push['event'], push['branch'], push['commit'], push['trusted']), ('push', 'main', COMMIT,
                                                                                             True))
        self.assertIsNone(hosts.parse('github', {'X-GitHub-Event': 'push'}, github_push(deleted=True)))
        self.assertIsNone(hosts.parse('github', {'X-GitHub-Event': 'push'}, github_push(ref='refs/tags/v1')))
        self.assertIsNone(hosts.parse('github', {'X-GitHub-Event': 'ping'}, b'{}'))
        pull = {'action': 'synchronize', 'pull_request': {
            'number': 7, 'state': 'open', 'title': 't', 'head': {'sha': COMMIT, 'ref': 'f', 'repo': {
                'full_name': 'fork/cerid'}}, 'base': {'ref': 'main', 'repo': {'full_name': 'o/cerid'}}}}
        parsed = hosts.parse('github', {'X-GitHub-Event': 'pull_request'}, json.dumps(pull).encode())
        self.assertEqual((parsed['pull_request'], parsed['base_branch'], parsed['trusted']), ('7', 'main', False))
        pull['action'] = 'closed'
        self.assertIsNone(hosts.parse('github', {'X-GitHub-Event': 'pull_request'}, json.dumps(pull).encode()))
        gitlab = {'ref': 'refs/heads/main', 'checkout_sha': COMMIT, 'project': {'path_with_namespace': 'o/c'}}
        self.assertEqual(hosts.parse('gitlab', {'X-Gitlab-Event': 'Push Hook'}, json.dumps(gitlab).encode())['commit'],
                         COMMIT)
        self.assertNotEqual(hosts.trigger_key('r', push), hosts.trigger_key('r', dict(push, commit='c' * 40)))

    def test_cron(self):
        nightly = hosts.Cron('0 3 * * *')
        t = hosts.parse_utc('2026-10-30T03:00:00Z')
        self.assertTrue(nightly.matches(t))
        self.assertFalse(nightly.matches(t + 60))
        self.assertEqual(nightly.last(t + 3600), t)
        weekly = hosts.Cron('30 4 * * 0')  # 2026-11-01 is a Sunday
        self.assertTrue(weekly.matches(hosts.parse_utc('2026-11-01T04:30:00Z')))
        self.assertFalse(weekly.matches(hosts.parse_utc('2026-11-02T04:30:00Z')))
        self.assertTrue(hosts.Cron('*/15 9-17 * * 1-5').matches(hosts.parse_utc('2026-10-30T09:45:00Z')))
        either = hosts.Cron('0 0 1 * 1')  # day 1 or a Monday
        self.assertTrue(either.matches(hosts.parse_utc('2026-11-02T00:00:00Z')))
        self.assertTrue(either.matches(hosts.parse_utc('2026-12-01T00:00:00Z')))
        for bad in ('* * *', '61 * * * *', '*/0 * * * *'):
            with self.assertRaises(ValueError):
                hosts.Cron(bad)

    def test_ls_remote_and_status_requests(self):
        class Done:
            returncode = 0
            stdout = '%s\trefs/heads/main\n%s\trefs/heads/mainline\n' % (COMMIT, 'b' * 40)
            stderr = ''
        seen = []
        self.assertEqual(hosts.ls_remote('u', ['main'], run=lambda argv, **kw: seen.append(argv) or Done()),
                         {'main': COMMIT})
        self.assertEqual(seen[0][:3], ['git', 'ls-remote', '--heads'])
        method, url, body = hosts.status_request('github', 'https://api.github.com', 'o/c', COMMIT, 'failed', 'u', 'd')
        self.assertEqual((method, url, body['state']), ('POST', 'https://api.github.com/repos/o/c/statuses/' + COMMIT,
                                                        'failure'))
        _, url, body = hosts.status_request('gitlab', 'https://gitlab.com/api/v4', 'o/c', COMMIT, 'running', 'u', 'd')
        self.assertIn('/projects/o%2Fc/statuses/', url)
        self.assertEqual(body['state'], 'running')
        self.assertIsNone(hosts.status_request('bitbucket', 'x', 'o/c', COMMIT, 'passed', 'u', 'd'))


# --------------------------------------------------------------------------------------------- coordinator

class Base(unittest.TestCase):
    def setUp(self):
        self.scratch, self.store = fresh_store()
        self.clock = Clock()
        self.c = coordinator(self.store, self.clock)
        self.c.ensure_schema()

    def tearDown(self):
        self.scratch.cleanup()

    def build(self, document=None, **trigger):
        trigger = dict({'event': 'push', 'branch': 'main', 'commit': COMMIT, 'trusted': True}, **trigger)
        build, created = self.c.create_build('cerid', trigger, 'webhook')
        self.assertTrue(created)
        if document is not None:
            finish_plan(self.c, build, document)
        return self.c.build(build['number'])

    def jobs(self, build):
        return {j['key']: j for j in self.c.jobs_of(self.c.build(build['number']))}

    def run_job(self, a, exit_status=0):
        got = self.c.poll(a)
        self.assertIsNotNone(got)
        self.c.start(got['job']['id'], got['token'])
        return self.c.finish(got['job']['id'], got['token'], exit_status), got



class Core(Base):
    def test_a_build_starts_with_its_plan_on_the_launcher_queue(self):
        build = self.build()
        self.assertEqual(build['state'], 'scheduled')
        plan = self.jobs(build)['plan']
        self.assertEqual((plan['agents'], plan['command']), ({'queue': 'cerid-launcher'},
                                                             'python ci/pipeline.py upload'))
        self.assertIsNone(self.c.poll(agent(self.c, queue='cerid-owned')))
        got = self.c.poll(agent(self.c, name='l'))
        self.assertEqual(got['job']['env']['CERID_CI_COMMIT'], COMMIT)
        self.assertEqual(got['checkout'], {'url': 'https://git.example/cerid.git', 'commit': COMMIT,
                                           'branch': 'main', 'strategy': 'direct', 'depth': 50})

    def test_dependencies_wait_and_priority(self):
        build = self.build({'steps': [{'key': 'low', 'command': 'x', 'agents': {'queue': 'q'}},
                                      {'key': 'high', 'command': 'x', 'agents': {'queue': 'q'}, 'priority': 5},
                                      'wait', {'key': 'last', 'command': 'x', 'agents': {'queue': 'q'}}]})
        worker = agent(self.c, queue='q', name='w')
        first = self.c.poll(worker)
        self.assertEqual(first['job']['key'], 'high')
        second = self.c.poll(agent(self.c, queue='q', name='w2'))
        self.assertEqual(second['job']['key'], 'low')
        self.assertIsNone(self.c.poll(agent(self.c, queue='q', name='w3')), 'last waits for the wait step')
        for got in (first, second):
            self.c.start(got['job']['id'], got['token'])
            self.c.finish(got['job']['id'], got['token'], 0)
        self.assertEqual(self.c.poll(worker)['job']['key'], 'last')

    def test_tags_route_jobs_to_matching_agents(self):
        self.build({'steps': [{'key': 'gpu', 'command': 'x', 'agents': {'queue': 'cerid-owned', 'device': 'rtx'}}]})
        self.assertIsNone(self.c.poll(agent(self.c, queue='cerid-owned', tags={'device': 'other'}, name='o')))
        self.assertEqual(self.c.poll(agent(self.c, queue='cerid-owned', tags={'device': 'rtx'}, name='r'))['job']['key'],
                         'gpu')

    def test_registration_tags_cannot_be_overridden_by_the_agent(self):
        token = self.c.create_registration('box', 'cerid-owned', {'device': 'mine'})
        record, _, _ = self.c.register(token, {'name': 'box', 'protocol': 1, 'tags': {'device': 'stolen',
                                                                                      'queue': 'cerid-launcher'}})
        self.assertEqual((record['queue'], record['tags']['device']), ('cerid-owned', 'mine'))
        with self.assertRaises(auth.AuthError):
            self.c.register('crt_wrong', {'protocol': 1})
        with self.assertRaises(scheduler.Refused):
            self.c.register(token, {'protocol': 99})
        self.assertEqual(self.c.revoke_registration('box'), 1)
        with self.assertRaises(auth.AuthError):
            self.c.register(token, {'protocol': 1})

    def test_concurrency_group_limits_running_jobs(self):
        self.build({'steps': [{'key': 'k%d' % i, 'command': 'x', 'agents': {'queue': 'q'},
                               'concurrency_group': 'gce', 'concurrency': 2} for i in range(4)]})
        got = [self.c.poll(agent(self.c, queue='q', name='w%d' % i)) for i in range(4)]
        self.assertEqual([g is not None for g in got], [True, True, False, False])
        self.c.start(got[0]['job']['id'], got[0]['token'])
        self.c.finish(got[0]['job']['id'], got[0]['token'], 0)
        self.assertIsNotNone(self.c.poll(agent(self.c, queue='q', name='w9')))

    def test_soft_fail_exit_retry_and_build_outcome(self):
        build = self.build({'steps': [
            {'key': 'soft', 'command': 'x', 'agents': {'queue': 'q'}, 'soft_fail': [{'exit_status': 3}]},
            {'key': 'flaky', 'command': 'x', 'agents': {'queue': 'q'}, 'retry': {'automatic': [
                {'exit_status': 7, 'limit': 1}]}},
            {'key': 'after', 'command': 'x', 'agents': {'queue': 'q'}, 'depends_on': ['soft', 'flaky']}]})
        w = agent(self.c, queue='q', name='w')
        self.run_job(w, 3)
        job, _ = self.run_job(w, 7)
        self.assertEqual((job['state'], job['attempt']), ('scheduled', 2))
        self.run_job(w, 0)
        self.run_job(w, 0)
        self.assertEqual(self.c.build(build['number'])['state'], 'passed')

    def test_a_failed_dependency_cancels_dependents_and_fails_the_build(self):
        build = self.build({'steps': [{'key': 'a', 'command': 'x', 'agents': {'queue': 'q'}},
                                      {'key': 'b', 'command': 'x', 'agents': {'queue': 'q'}, 'depends_on': 'a'},
                                      {'key': 'c', 'command': 'x', 'agents': {'queue': 'q'},
                                       'depends_on': {'step': 'a', 'allow_failure': True}}]})
        w = agent(self.c, queue='q', name='w')
        self.run_job(w, 1)
        self.c.tick()
        jobs = self.jobs(build)
        self.assertEqual(jobs['b']['state'], 'canceled')
        self.assertTrue(jobs['b']['reason'].startswith('dependency a failed'))
        self.run_job(w, 0)
        self.assertEqual(self.c.build(build['number'])['state'], 'failed')
        # A manual retry reopens the build and the dependents it broke.
        self.c.retry_job(jobs['a']['id'], by='me')
        self.assertEqual(self.c.build(build['number'])['state'], 'running')
        self.assertEqual(self.jobs(build)['b']['state'], 'scheduled')
        self.run_job(w, 0)
        self.run_job(w, 0)
        self.assertEqual(self.c.build(build['number'])['state'], 'passed')
        with self.assertRaises(scheduler.Refused):
            self.c.retry_job(jobs['a']['id'])

    def test_cancellation_reaches_the_running_agent(self):
        build = self.build({'steps': [{'key': 'run', 'command': 'x', 'agents': {'queue': 'q'}},
                                      {'key': 'wait', 'command': 'x', 'agents': {'queue': 'q'}, 'depends_on': 'run'}]})
        got = self.c.poll(agent(self.c, queue='q', name='w'))
        self.c.start(got['job']['id'], got['token'])
        self.c.cancel_build(build['number'], by='me')
        self.assertEqual(self.c.heartbeat(got['job']['id'], got['token']), {'cancel': True, 'reason': 'canceled'})
        self.assertEqual(self.jobs(build)['wait']['state'], 'canceled')
        self.c.finish(got['job']['id'], got['token'], -1)
        self.assertEqual(self.jobs(build)['run']['state'], 'canceled')
        self.assertEqual(self.c.build(build['number'])['state'], 'canceled')

    def test_step_timeout_then_grace(self):
        build = self.build({'steps': [{'key': 'slow', 'command': 'x', 'agents': {'queue': 'q'},
                                       'timeout_in_minutes': 1}, {'key': 'stuck', 'command': 'x',
                                                                  'agents': {'queue': 'q'}, 'timeout_in_minutes': 1}]})
        a = self.c.poll(agent(self.c, queue='q', name='w1'))
        b = self.c.poll(agent(self.c, queue='q', name='w2'))
        for got in (a, b):
            self.c.start(got['job']['id'], got['token'])
        self.clock.advance(61)
        for got in (a, b):
            self.c.heartbeat(got['job']['id'], got['token'])
        self.c.tick()
        self.assertTrue(self.c.heartbeat(a['job']['id'], a['token'])['cancel'])
        self.c.finish(a['job']['id'], a['token'], -1)
        self.assertEqual(self.jobs(build)['slow']['state'], 'timed_out')
        self.clock.advance(self.c.limits['cancel_grace_seconds'] + 1)
        self.c.heartbeat(b['job']['id'], b['token'])
        self.c.tick()
        stuck = self.jobs(build)['stuck']
        self.assertEqual(stuck['state'], 'timed_out')
        self.assertIn('did not stop in time', stuck['reason'])

    def test_build_timeout_cancels_the_build(self):
        build = self.build({'timeout_in_minutes': 5, 'steps': [{'key': 'a', 'command': 'x',
                                                                 'agents': {'queue': 'q'}}]})
        got = self.c.poll(agent(self.c, queue='q', name='w'))
        self.c.start(got['job']['id'], got['token'])
        self.clock.advance(301)
        self.c.heartbeat(got['job']['id'], got['token'])
        self.c.tick()
        self.assertTrue(self.c.build(build['number'])['cancel_requested'])
        self.assertTrue(self.c.heartbeat(got['job']['id'], got['token'])['cancel'])

    def test_scheduled_jobs_expire(self):
        build = self.build({'steps': [{'key': 'nobody', 'command': 'x', 'agents': {'queue': 'nowhere'}}]})
        self.c.tick()
        self.clock.advance(self.c.limits['scheduled_expiry_minutes'] * 60 + 1)
        self.c.tick()
        self.assertEqual(self.jobs(build)['nobody']['state'], 'expired')
        self.assertEqual(self.c.build(build['number'])['state'], 'failed')

    def test_tokens_are_bound_to_one_job_and_attempt(self):
        self.build({'steps': [{'key': 'a', 'command': 'x', 'agents': {'queue': 'q'},
                               'retry': {'automatic': [{'reason': 'agent_lost', 'limit': 1}]}},
                              {'key': 'b', 'command': 'x', 'agents': {'queue': 'q'}}]})
        a = self.c.poll(agent(self.c, queue='q', name='w1'))
        b = self.c.poll(agent(self.c, queue='q', name='w2'))
        with self.assertRaises(auth.AuthError):
            self.c.start(a['job']['id'], b['token'])
        with self.assertRaises(auth.AuthError):
            self.c.start(a['job']['id'], 'cjt_forged')
        self.c.infrastructure(a['job']['id'], 'agent_lost')
        with self.assertRaises(auth.AuthError):
            self.c.start(a['job']['id'], a['token'])

    def test_secrets_only_for_trusted_builds(self):
        secrets_dir = tempfile.TemporaryDirectory()
        with secrets_dir:
            (Path(secrets_dir.name) / 'deploy-key').write_text('s3cret-value\n', encoding='utf-8')
            self.c.secrets = scheduler.DirectorySecrets(secrets_dir.name)
            document = {'steps': [{'key': 's', 'command': 'x', 'agents': {'queue': 'q'}, 'secrets': ['deploy-key']}]}
            self.build(document)
            got = self.c.poll(agent(self.c, queue='q', name='w'))
            self.assertEqual(got['job']['secrets'], {'DEPLOY_KEY': 's3cret-value'})
            self.c.start(got['job']['id'], got['token'])
            self.c.finish(got['job']['id'], got['token'], 0)
            stored = json.dumps([self.store.get_json(n) for n in self.store.list('orchestrator/')])
            self.assertNotIn('s3cret-value', stored, 'a secret value never reaches the store')
            # A pull request from the repository itself builds, untrusted: its secrets are refused.
            self.build(document, commit='c' * 40, event='pull_request', branch='feature', pull_request='4',
                       trusted=True)
            with self.assertRaises(scheduler.Refused):
                self.c.poll(agent(self.c, queue='q', name='w2'))

    def test_checkout_strategy_is_explicit_and_validated(self):
        pinned = []

        class Mirror:
            def pin(self, repository, repo, number, commit):
                pinned.append((repository, number))
        self.c.mirror = Mirror()
        self.c.repositories['cerid']['url'] = '/srv/git/cerid.git'
        build = self.build()
        got = self.c.poll(agent(self.c, name='l'))
        self.assertEqual((got['checkout']['strategy'], got['checkout']['url']), ('direct', '/srv/git/cerid.git'))
        self.assertEqual(pinned, [], 'a direct repository is never pinned in the mirror')
        self.c.repositories['cerid']['checkout'] = 'mirror'
        mirrored = self.build(commit='b' * 40)
        got = self.c.poll(agent(self.c, name='l2'))
        self.assertEqual((got['checkout']['url'], got['checkout']['ref']),
                         ('/git/cerid.git', 'refs/cerid/builds/%d' % mirrored['number']))
        self.assertEqual(pinned, [('cerid', mirrored['number'])])
        self.c.repositories['cerid']['checkout'] = 'mirrror'
        self.build(commit='c' * 40)
        with self.assertRaises(ValueError):
            self.c.poll(agent(self.c, name='l3'))
        from coordinator import main as coordinator_main
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / 'c.json'
            path.write_text(json.dumps({'schema': coordinator_main.SCHEMA, 'repositories': {
                'r': {'url': '/srv/git/r.git', 'checkout': 'mirrror'}}}), encoding='utf-8')
            _, problems = coordinator_main.load_config(path, {})
        self.assertEqual(problems, ["repository r: checkout 'mirrror' is not one of direct, mirror"])
        self.assertEqual(build['number'], 1)

    def test_fork_pull_requests_never_build_unless_the_repository_opts_in(self):
        fork = {'event': 'pull_request', 'branch': 'evil', 'commit': 'f' * 40, 'pull_request': '9',
                'base_branch': 'main', 'trusted': False}
        self.assertEqual(self.c.create_build('cerid', fork, 'webhook'), (None, False))
        self.assertEqual(self.c.recent(), [])
        self.assertTrue(any(r['event'] == 'build_refused_fork' for r in self.c.log.records))
        self.c.repositories['cerid']['build_forks'] = True
        build, created = self.c.create_build('cerid', fork, 'webhook')
        self.assertTrue(created)
        self.assertFalse(build['trusted'])
        push = {'event': 'push', 'branch': 'feature', 'commit': 'e' * 40, 'trusted': True}
        build, created = self.c.create_build('cerid', push, 'webhook')
        self.assertTrue(created, 'a push to any branch of the repository builds')
        self.assertFalse(build['trusted'], 'outside the trusted branches it gets no secrets')

    def test_logs_are_ordered_idempotent_chunks(self):
        self.build({'steps': [{'key': 'a', 'command': 'x', 'agents': {'queue': 'q'}}]})
        got = self.c.poll(agent(self.c, queue='q', name='w'))
        job_id, token = got['job']['id'], got['token']
        self.c.start(job_id, token)
        self.c.append_log(job_id, token, 1, 0, b'one\n')
        self.c.append_log(job_id, token, 1, 2, b'three\n')
        self.c.append_log(job_id, token, 1, 0, b'one\n')  # a retried upload of the same chunk
        with self.assertRaises(scheduler.Refused):
            self.c.append_log(job_id, token, 1, 0, b'other\n')
        data, nxt, live, _ = self.c.read_log(job_id)
        self.assertEqual((data, nxt, live), (b'one\n', 1, True), 'a gap holds the tail back')
        self.c.append_log(job_id, token, 1, 1, b'two\n')
        self.assertEqual(self.c.read_log(job_id)[:2], (b'one\ntwo\nthree\n', 3))
        self.assertEqual(self.c.read_log(job_id, start=2)[0], b'three\n')
        with self.assertRaises(scheduler.Refused):
            self.c.append_log(job_id, token, 1, 1, b'x' * (self.c.limits['log_chunk_bytes'] + 1))

    def test_artifacts_annotations_meta_and_outcomes(self):
        build = self.build({'steps': [{'key': 'a', 'command': 'x', 'agents': {'queue': 'q'}},
                                      {'key': 'b', 'command': 'x', 'agents': {'queue': 'q'}, 'soft_fail': True}]})
        a = self.c.poll(agent(self.c, queue='q', name='w1'))
        data = b'{"plan": 1}'
        self.c.put_artifact(a['job']['id'], a['token'], 'ci-plan.json', data, hashlib.sha256(data).hexdigest())
        with self.assertRaises(scheduler.Refused):
            self.c.put_artifact(a['job']['id'], a['token'], 'x.json', data, '0' * 64)
        with self.assertRaises(scheduler.Refused):
            self.c.put_artifact(a['job']['id'], a['token'], '../../escape', data, hashlib.sha256(data).hexdigest())
        self.assertEqual(self.c.artifact(build['number'], 'ci-plan.json')[1], data)
        self.c.annotate(a['job']['id'], a['token'], 'lane', 'error', '# one\n')
        self.c.annotate(a['job']['id'], a['token'], 'lane', 'error', 'two\n', append=True)
        self.c.set_meta(a['job']['id'], a['token'], 'tier', 'change')
        self.assertEqual(self.c.get_meta(a['job']['id'], a['token'], 'tier'), 'change')
        record = self.c.build(build['number'])
        self.assertEqual(record['annotations']['lane'], {'style': 'error', 'body': '# one\ntwo\n',
                                                         'job': a['job']['id']})
        self.assertEqual(self.c.step_outcome(a['job']['id'], a['token'], 'b')['outcome'], 'running')
        b = self.c.poll(agent(self.c, queue='q', name='w2'))
        self.c.start(b['job']['id'], b['token'])
        self.c.finish(b['job']['id'], b['token'], 2)
        self.assertEqual(self.c.step_outcome(a['job']['id'], a['token'], 'b')['outcome'], 'soft_failed')
        self.assertEqual(self.c.step_outcome(a['job']['id'], a['token'], 'zzz')['outcome'], 'absent')

    def test_skipped_steps_are_created_terminal(self):
        build = self.build({'steps': [{'key': 'c', 'command': 'x', 'skip': {'state': 'cached',
                                                                            'reason': 'cached: passed in build 3'}},
                                      {'key': 'b', 'command': 'x', 'skip': 'budget: 49.00 of 50.00 USD'}]})
        jobs = self.jobs(build)
        self.assertEqual((jobs['c']['state'], jobs['b']['state']), ('cached', 'budget'))
        self.assertEqual(self.c.build(build['number'])['state'], 'passed')

    def test_retention_removes_old_logs_and_artifacts(self):
        build = self.build({'steps': [{'key': 'a', 'command': 'x', 'agents': {'queue': 'q'}}]})
        got = self.c.poll(agent(self.c, queue='q', name='w'))
        self.c.start(got['job']['id'], got['token'])
        self.c.append_log(got['job']['id'], got['token'], 1, 0, b'log')
        self.c.put_artifact(got['job']['id'], got['token'], 'a.txt', b'x', hashlib.sha256(b'x').hexdigest())
        self.c.finish(got['job']['id'], got['token'], 0)
        self.c.sweep()
        self.assertTrue(self.store.list('orchestrator/logs/'))
        self.clock.advance(31 * 86400)
        self.c.sweep()
        self.assertFalse([n for n in self.store.list('orchestrator/artifacts/') if not n.endswith('manifest.json')])
        self.assertTrue(self.store.list('orchestrator/logs/'))
        self.clock.advance(60 * 86400)
        self.c.sweep()
        self.assertFalse(self.store.list('orchestrator/logs/%s/' % got['job']['id']))
        self.assertTrue(self.c.build(build['number'])['purged']['logs'])

    def test_commit_status_is_reported_and_never_fatal(self):
        calls = []
        config = json.loads(json.dumps(CONFIG))
        config['repositories']['cerid']['status'] = {'kind': 'github', 'api': 'https://api.github.com',
                                                     'repository': 'o/cerid', 'token_secret': 'status-token'}

        class Secrets:
            def value(self, name):
                return 'tok'
        c = scheduler.Coordinator(self.store, config, clock=self.clock, secrets=Secrets(),
                                  http=lambda m, u, **kw: calls.append((m, u, kw['body']['state'], kw['headers'])),
                                  log=scheduler.JsonLog(io.StringIO()))
        build, _ = c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': 'd' * 40, 'trusted': True},
                                  'manual')
        got = c.poll(agent(c, name='l'))
        c.start(got['job']['id'], got['token'])
        c.finish(got['job']['id'], got['token'], 0)
        self.assertEqual([s for _, _, s, _ in calls], ['pending', 'success'])
        self.assertEqual(calls[0][3]['Authorization'], 'Bearer tok')

        def broken(*a, **kw):
            raise OSError('host down')
        c.http = broken
        build, _ = c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': 'e' * 40}, 'manual')
        got = c.poll(agent(c, name='l2'))
        c.start(got['job']['id'], got['token'])
        self.assertEqual(c.finish(got['job']['id'], got['token'], 0)['state'], 'passed')


# --------------------------------------------------------------------------------------------- fault injection

class Faults(Base):
    def test_lost_agent_is_retried_on_another_agent(self):
        build = self.build({'steps': [{'key': 'a', 'command': 'x', 'agents': {'queue': 'q'},
                                       'retry': {'automatic': [{'reason': 'agent_lost', 'limit': 2}]}}]})
        first = self.c.poll(agent(self.c, queue='q', name='dies'))
        self.c.start(first['job']['id'], first['token'])
        self.clock.advance(self.c.limits['lost_after_seconds'] + 1)
        self.c.tick()
        job = self.jobs(build)['a']
        self.assertEqual((job['state'], job['attempt'], job['attempts'][0]['state']), ('scheduled', 2, 'running'))
        self.assertIn('agent_lost', job['reason'])
        second = self.c.poll(agent(self.c, queue='q', name='lives'))
        self.c.start(second['job']['id'], second['token'])
        self.c.finish(second['job']['id'], second['token'], 0)
        self.assertEqual(self.c.build(build['number'])['state'], 'passed')
        with self.assertRaises(auth.AuthError):  # the lost agent comes back late: its token is dead
            self.c.finish(first['job']['id'], first['token'], 0)

    def test_retries_are_bounded(self):
        build = self.build({'steps': [{'key': 'a', 'command': 'x', 'agents': {'queue': 'q'},
                                       'retry': {'automatic': [{'reason': 'agent_lost', 'limit': 1}]}}]})
        for name in ('x1', 'x2'):
            got = self.c.poll(agent(self.c, queue='q', name=name))
            self.c.start(got['job']['id'], got['token'])
            self.clock.advance(self.c.limits['lost_after_seconds'] + 1)
            self.c.tick()
        job = self.jobs(build)['a']
        self.assertEqual((job['state'], job['attempt']), ('failed', 2))
        self.assertEqual(self.c.build(build['number'])['state'], 'failed')

    def test_an_agent_that_takes_a_job_and_vanishes(self):
        build = self.build({'steps': [{'key': 'a', 'command': 'x', 'agents': {'queue': 'q'},
                                       'retry': {'automatic': [{'reason': 'infrastructure', 'limit': 1}]}}]})
        self.c.poll(agent(self.c, queue='q', name='w'))
        self.clock.advance(self.c.limits['accept_seconds'] + 1)
        self.c.tick()
        self.assertEqual(self.jobs(build)['a']['state'], 'scheduled')

    def test_duplicate_webhook_deliveries_build_once(self):
        trigger = {'event': 'push', 'branch': 'main', 'commit': COMMIT, 'trusted': True}
        first, created = self.c.create_build('cerid', trigger, 'webhook')
        again, created_again = self.c.create_build('cerid', trigger, 'poll')
        self.assertTrue(created)
        self.assertFalse(created_again)
        self.assertEqual(again['number'], first['number'])
        self.assertEqual(len(self.c.recent()), 1)
        manual, created = self.c.create_build('cerid', trigger, 'manual')
        self.assertTrue(created)
        self.assertNotEqual(manual['number'], first['number'])

    def test_coordinator_restart_mid_build(self):
        build = self.build({'steps': [{'key': 'a', 'command': 'x', 'agents': {'queue': 'q'}},
                                      {'key': 'b', 'command': 'x', 'agents': {'queue': 'q'}, 'depends_on': 'a'}]})
        got = self.c.poll(agent(self.c, queue='q', name='w'))
        self.c.start(got['job']['id'], got['token'])
        restarted = coordinator(self.store, self.clock)  # a new process: nothing carried over in memory
        restarted.ensure_schema()
        self.clock.advance(30)
        restarted.heartbeat(got['job']['id'], got['token'])
        restarted.tick()
        restarted.finish(got['job']['id'], got['token'], 0)
        nxt = restarted.poll(agent(restarted, queue='q', name='w2'))
        self.assertEqual(nxt['job']['key'], 'b')
        restarted.start(nxt['job']['id'], nxt['token'])
        restarted.finish(nxt['job']['id'], nxt['token'], 0)
        self.assertEqual(restarted.build(build['number'])['state'], 'passed')

    def test_a_newer_store_schema_is_refused(self):
        self.store.put_json('orchestrator/schema.json', {'schema': 99})
        with self.assertRaises(RuntimeError):
            coordinator(self.store, self.clock).ensure_schema()

    def test_concurrent_polls_assign_a_job_once(self):
        self.build({'steps': [{'key': 'only', 'command': 'x', 'agents': {'queue': 'q'}}]})
        agents = [agent(self.c, queue='q', name='w%d' % i) for i in range(6)]
        results = []
        barrier = threading.Barrier(len(agents))

        def poll(a):
            barrier.wait()
            results.append(self.c.poll(a))
        threads = [threading.Thread(target=poll, args=(a,)) for a in agents]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(len([r for r in results if r]), 1)

    def test_concurrent_uploads_never_lose_a_step(self):
        build = self.build()
        errors = []

        def upload(i):
            try:
                self.c.upload(build['number'], {'steps': [{'key': 'u%d' % i, 'command': 'x'}]})
            except Exception as error:
                errors.append(error)
        threads = [threading.Thread(target=upload, args=(i,)) for i in range(5)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        self.assertEqual(set(self.c.build(build['number'])['keys']), {'plan', 'u0', 'u1', 'u2', 'u3', 'u4'})


# --------------------------------------------------------------------------------------------- dispatcher

class FakeExecutor:
    def __init__(self, fail=False):
        self.launched = []
        self.torn = []
        self.dead = set()
        self.fail = fail

    def launch(self, machine, device, agent):
        if self.fail:
            raise RuntimeError('ZONE_RESOURCE_POOL_EXHAUSTED')
        self.launched.append((machine, device['id'], dict(agent)))
        return {'instance': machine}

    def alive(self, handle):
        return handle['instance'] not in self.dead

    def teardown(self, handle):
        self.torn.append(handle['instance'])

    def machines(self):
        return [{'name': 'cerid-ci-orphan', 'created': 0, 'handle': {'instance': 'cerid-ci-orphan'}}]


def cloud_steps(*devices, **extra):
    return {'steps': [dict({'key': 'lane-%s' % d, 'command': 'x', 'agents': {'queue': 'cerid-cloud', 'device': d},
                            'retry': {'automatic': [{'reason': 'infrastructure', 'limit': 2}]}}, **extra)
                      for d in devices]}


class Dispatching(Base):
    def dispatcher(self, executor, cap=None):
        registry = json.loads(json.dumps(REGISTRY))
        if cap is not None:
            registry['budget']['monthly_cap_usd'] = cap
        return dispatch.Dispatcher(self.c, registry, {'gce_vm': executor, 'ssh_host': executor},
                                   agent={'coordinator': 'http://10.0.0.2:8780', 'agent_sha256': 'f' * 64})

    def test_cloud_jobs_get_one_machine_each_and_no_token_for_gce(self):
        self.build(cloud_steps('gce-linux-x64-intel', 'gce-windows-x64'))
        executor = FakeExecutor()
        launched = self.dispatcher(executor).tick()
        self.assertEqual(len(launched), 2)
        self.assertNotIn('token', executor.launched[0][2], 'a GCE machine proves its identity; it gets no token')
        machine = launched[0]['machine']
        got = self.c.acquire(machine)
        self.assertEqual(got['job']['key'], launched[0]['key'])
        with self.assertRaises(scheduler.Refused):
            self.c.acquire(machine)  # the identity token replayed: the job was already taken
        with self.assertRaises(scheduler.Refused):
            self.c.acquire('cerid-ci-unknown')
        self.assertEqual(self.dispatcher(executor).tick(), [], 'a reserved job is never launched twice')

    def test_preempted_vm_is_retried_on_a_new_machine(self):
        build = self.build(cloud_steps('gce-linux-x64-intel'))
        executor = FakeExecutor()
        d = self.dispatcher(executor)
        first = d.tick()[0]
        got = self.c.acquire(first['machine'])
        self.c.start(got['job']['id'], got['token'])
        executor.dead.add(first['machine'])
        d.reap()
        job = self.jobs(build)['lane-gce-linux-x64-intel']
        self.assertEqual((job['state'], job['attempt']), ('scheduled', 2))
        self.assertIn('preempted', job['reason'])
        self.assertIn(first['machine'], executor.torn)
        self.assertEqual(len(self.store.list('ledger/')), 1)
        second = d.tick()[0]
        self.assertNotEqual(second['machine'], first['machine'])
        got = self.c.acquire(second['machine'])
        self.c.start(got['job']['id'], got['token'])
        self.c.finish(got['job']['id'], got['token'], 0)
        d.reap()
        self.assertEqual(len(self.store.list('ledger/')), 2)
        self.assertEqual(self.c.build(build['number'])['state'], 'passed')

    def test_a_vm_that_never_boots_is_replaced(self):
        build = self.build(cloud_steps('gce-linux-x64-intel'))
        executor = FakeExecutor()
        d = self.dispatcher(executor)
        d.tick()
        self.clock.advance(self.c.limits['boot_seconds'] + 1)
        self.c.tick()
        self.assertEqual(self.jobs(build)['lane-gce-linux-x64-intel']['attempt'], 2)
        d.reap()
        self.assertEqual(len(executor.torn), 1)

    def test_launch_failure_retries_then_fails(self):
        build = self.build(cloud_steps('gce-linux-x64-intel'))
        d = self.dispatcher(FakeExecutor(fail=True))
        for _ in range(3):
            d.tick()
        job = self.jobs(build)['lane-gce-linux-x64-intel']
        self.assertEqual(job['state'], 'failed')
        self.assertIn('ZONE_RESOURCE_POOL_EXHAUSTED', job['reason'])

    def test_budget_stops_paid_launches_and_is_never_a_pass(self):
        build = self.build(cloud_steps('gce-linux-x64-intel'))
        executor = FakeExecutor()
        self.dispatcher(executor, cap=0.1).tick()
        job = self.jobs(build)['lane-gce-linux-x64-intel']
        self.assertEqual(job['state'], 'budget')
        self.assertEqual(executor.launched, [])
        self.assertEqual(self.store.get_json('results/%d/lane-gce-linux-x64-intel.json' % build['number'])['outcome'],
                         'budget')

    def test_running_machines_count_against_the_cap(self):
        self.build(cloud_steps('gce-linux-x64-intel', 'gce-linux-x64-amd-zen4', timeout_in_minutes=60))
        executor = FakeExecutor()
        launched = self.dispatcher(executor, cap=0.2).tick()
        self.assertEqual(len(launched), 1, 'the second worst case would pass the cap')

    def test_ssh_machines_get_a_one_job_token(self):
        self.build(cloud_steps('windows-arm64-host'))
        executor = FakeExecutor()
        self.dispatcher(executor).tick()
        token = executor.launched[0][2]['token']
        job_id = self.c.get('instances/%s.json' % executor.launched[0][0])['job']
        self.assertEqual(self.c.start(job_id, token)['state'], 'running')

    def test_unknown_device_fails_the_job(self):
        build = self.build(cloud_steps('no-such-device'))
        self.dispatcher(FakeExecutor()).tick()
        self.assertEqual(self.jobs(build)['lane-no-such-device']['state'], 'failed')

    def test_orphan_machines_are_deleted(self):
        executor = FakeExecutor()
        self.assertEqual(self.dispatcher(executor).sweep_orphans(), ['cerid-ci-orphan'])
        self.assertEqual(executor.torn, ['cerid-ci-orphan'])


# --------------------------------------------------------------------------------------------- triggers

class Done:
    def __init__(self, heads):
        self.returncode = 0
        self.stdout = ''.join('%s\trefs/heads/%s\n' % (sha, branch) for branch, sha in heads.items())
        self.stderr = ''


class Triggers(Base):
    def setUp(self):
        super().setUp()
        self.c.repositories['cerid']['poll'] = {'branches': ['main'], 'interval_seconds': 60}
        self.c.repositories['cerid']['schedules'] = [{'name': 'nightly', 'cron': '0 3 * * *', 'branch': 'main'}]

    def test_polling_learns_heads_then_builds_new_ones_once(self):
        heads = {'main': COMMIT}
        run = lambda argv, **kw: Done(heads)
        self.assertEqual(triggers.poll(self.c, run=run), [], 'the first poll only learns the heads')
        heads['main'] = 'c' * 40
        self.assertEqual(triggers.poll(self.c, run=run), [], 'the poll interval holds')
        self.clock.advance(61)
        made = triggers.poll(self.c, run=run)
        self.assertEqual([(b['commit'], b['source'], b['before']) for b in made], [('c' * 40, 'poll', COMMIT)])
        _, created = self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': 'c' * 40}, 'webhook')
        self.assertFalse(created, 'the webhook for the polled push builds nothing more')

    def test_schedules_fire_once_per_occurrence_across_restarts(self):
        self.clock.now = hosts.parse_utc('2026-10-31T03:00:30Z')
        run = lambda argv, **kw: Done({'main': COMMIT})
        made = triggers.schedules(self.c, run=run)
        self.assertEqual([(b['source'], b['created_by']) for b in made], [('schedule', 'schedule:nightly')])
        again = scheduler.Coordinator(self.store, self.c.config, clock=self.clock, log=self.c.log)  # a restart
        self.assertEqual(triggers.schedules(again, run=run), [], 'a restarted coordinator does not fire it again')
        self.clock.now = hosts.parse_utc('2026-11-01T05:00:00Z')
        self.assertEqual(len(triggers.schedules(again, run=run)), 1, 'missed by two hours: caught up once')
        self.clock.now = hosts.parse_utc('2026-11-02T12:00:00Z')
        self.assertEqual(triggers.schedules(again, run=run), [], 'missed by nine hours: outside the catch-up window')

    def test_mirror_serves_fetches_only(self):
        mirror = triggers.Mirror(self.scratch.name)
        status, _, _ = mirror.serve('POST', '/cerid.git/git-receive-pack', '', {}, b'')
        self.assertEqual(status, 403)
        status, _, _ = mirror.serve('GET', '/cerid.git/info/refs', 'service=git-receive-pack', {}, b'')
        self.assertEqual(status, 403)
        status, _, _ = mirror.serve('GET', '/../etc/passwd', '', {}, b'')
        self.assertEqual(status, 403)


# --------------------------------------------------------------------------------------------- agent

agent_module = load('cerid_agent', 'ci/agent/agent.py')


class AgentProgram(unittest.TestCase):
    def test_redaction_holds_across_writes(self):
        secret = 'hunter2-value'
        for split in range(1, len(secret)):
            r = agent_module.Redactor([secret, 'x'])
            out = r.feed(b'before ' + secret[:split].encode()) + r.feed(secret[split:].encode() + b' after') + r.close()
            self.assertEqual(out, b'before [REDACTED] after', split)
        r = agent_module.Redactor([])
        self.assertEqual(r.feed(b'plain') + r.close(), b'plain')
        self.assertEqual(agent_module.Redactor(['abcd1234']).text('error abcd1234'), 'error [REDACTED]')

    def checkout_argv(self, checkout):
        """Every git command the agent's checkout runs for `checkout`, with git itself replaced by a recorder."""
        from unittest import mock
        seen = []

        def run(argv, **kwargs):
            seen.append(argv[1:])
            out = (checkout['commit'] + '\n').encode() if argv[1:2] == ['rev-parse'] else b''
            return subprocess.CompletedProcess(argv, 1 if argv[1:3] == ['remote', 'get-url'] else 0, out, b'')
        with tempfile.TemporaryDirectory() as scratch:
            a = agent_module.Agent('http://127.0.0.1:8780', scratch, name='t', mode='one-shot')
            log = type('Log', (), {'line': lambda self, text: None, 'write': lambda self, data: None})()
            with mock.patch.object(agent_module.subprocess, 'run', side_effect=run):
                a.checkout({'checkout': checkout, 'token': 'cjt_x', 'job': {'id': 'j' * 32, 'env': {}}}, log)
        return seen

    def test_the_strategy_alone_decides_where_a_checkout_fetches_from(self):
        commit = 'a' * 40
        # A direct checkout of a POSIX filesystem repository starts with '/', like the mirror's path; it must be
        # fetched exactly as configured (on Linux the agent used to prefix the coordinator's address to it).
        for url in ('/srv/git/cerid.git', 'https://git.example/cerid.git', 'git@git.example:o/cerid.git',
                    'C:\\repos\\cerid', 'file:///srv/git/cerid.git'):
            argv = self.checkout_argv({'url': url, 'commit': commit, 'branch': 'main', 'strategy': 'direct',
                                       'depth': 1})
            self.assertIn(['remote', 'add', 'origin', url], argv, url)
        argv = self.checkout_argv({'url': '/git/cerid.git', 'commit': commit, 'branch': 'main', 'strategy': 'mirror',
                                   'ref': 'refs/cerid/builds/4', 'depth': 1})
        self.assertIn(['remote', 'add', 'origin', 'http://127.0.0.1:8780/git/cerid.git'], argv)
        self.assertIn(['fetch', '--no-tags', '--quiet', '--depth=1', 'origin', 'refs/cerid/builds/4'], argv)
        resolve = agent_module.resolve_checkout
        self.assertEqual(resolve('https://ci', {'url': '/srv/x', 'strategy': 'direct'})['url'], '/srv/x')
        self.assertEqual(resolve('https://ci', {'url': '/srv/x'})['url'], '/srv/x', 'no strategy means direct')
        with self.assertRaises(ValueError):
            resolve('https://ci', {'url': '/git/x.git', 'strategy': 'mirrror'})
        with self.assertRaises(ValueError):
            resolve('https://ci', {'url': 'https://elsewhere/x.git', 'strategy': 'mirror'})

    def test_tokens_never_cross_the_internet_in_the_clear(self):
        self.assertEqual(agent_module.safe_url('https://ci.example/'), 'https://ci.example')
        for url in ('http://127.0.0.1:8780', 'http://10.128.0.2:8780', 'http://localhost:1'):
            self.assertTrue(agent_module.safe_url(url))
        with self.assertRaises(SystemExit):
            agent_module.safe_url('http://ci.example')
        with self.assertRaises(SystemExit):
            agent_module.safe_url('ftp://10.0.0.1')
        self.assertTrue(agent_module.safe_url('http://ci.example', allow_insecure=True))

    def test_clean_environment_is_an_allow_list(self):
        host = {'PATH': '/bin', 'HOME': '/home/c', 'AWS_SECRET_ACCESS_KEY': 'x', 'GITHUB_TOKEN': 'y',
                'CERID_CI_STORE': 'gs://z'}
        env = agent_module.clean_environment({'env': {'A': '1'}, 'secrets': {'S': 'v'}}, {'CERID_AGENT_JOB_ID': 'j'},
                                             host=host)
        self.assertEqual(env, {'PATH': '/bin', 'HOME': '/home/c', 'A': '1', 'S': 'v', 'CERID_AGENT_JOB_ID': 'j',
                               'PYTHONUTF8': '1', 'CERID_CI': '1'})

    def test_step_scripts_stop_at_the_first_failure(self):
        with tempfile.TemporaryDirectory() as scratch:
            argv = agent_module.script_for('%s -c "raise SystemExit(4)"\n%s -c "print(1)"' % (
                sys.executable, sys.executable), scratch)
            completed = subprocess.run(argv, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 4)
            self.assertNotIn('1', completed.stdout)

    def test_kill_tree_stops_children(self):
        child = subprocess.Popen([sys.executable, '-c', 'import subprocess, sys, time; '
                                  'subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"]); '
                                  'time.sleep(60)'], **({'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP}
                                                         if os.name == 'nt' else {'start_new_session': True}))
        time.sleep(0.5)
        started = time.time()
        agent_module.kill_tree(child, grace=2)
        self.assertIsNotNone(child.poll())
        self.assertLess(time.time() - started, 20)


# --------------------------------------------------------------------------------------------- service

class Service(Base):
    def setUp(self):
        super().setUp()
        self.secrets = tempfile.TemporaryDirectory()
        (Path(self.secrets.name) / 'hook').write_text('whsec', encoding='utf-8')
        self.c.secrets = scheduler.DirectorySecrets(self.secrets.name)
        fetch = lambda url: {'keys': [rsa_jwk()]}
        self.ui = auth.UiAuth('cloudflare-access', audience='aud', team='t.cloudflareaccess.com', fetch=fetch,
                              clock=lambda: self.clock())
        self.server = service.serve(service.Service(self.c, self.ui, CONFIG), '127.0.0.1', 0)
        self.base = 'http://127.0.0.1:%d' % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.secrets.cleanup()
        super().tearDown()

    def call(self, method, path, body=b'', headers=None):
        request = urllib.request.Request(self.base + path, data=body if method != 'GET' else None, method=method,
                                         headers=headers or {})
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, response.read(), dict(response.headers)
        except urllib.error.HTTPError as error:
            data = error.read()
            headers = dict(error.headers)
            error.close()
            return error.code, data, headers

    def access(self, email='me@example.com'):
        return {'Cf-Access-Jwt-Assertion': rs256({'iss': 'https://t.cloudflareaccess.com', 'aud': 'aud',
                                                  'exp': self.clock() + 60, 'email': email})}

    def test_the_ui_has_no_anonymous_access(self):
        for path in ('/', '/api/builds', '/qual', '/app.js'):
            status, _, headers = self.call('GET', path)
            self.assertEqual(status, 401, path)
            self.assertIn("default-src", headers['Content-Security-Policy'])
        status, body, headers = self.call('GET', '/api/builds', headers=self.access())
        self.assertEqual((status, json.loads(body)), (200, {'builds': []}))
        self.assertEqual(headers['X-Content-Type-Options'], 'nosniff')
        status, body, _ = self.call('GET', '/', headers=self.access())
        self.assertIn(b'app.js', body)

    def test_state_changes_need_the_request_header_and_same_origin(self):
        build, _ = self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': COMMIT}, 'manual')
        path = '/api/builds/%d/cancel' % build['number']
        self.assertEqual(self.call('POST', path, b'{}', self.access())[0], 403)
        self.assertEqual(self.call('POST', path, b'{}', dict(self.access(), **{
            'X-Cerid-Request': '1', 'Origin': 'https://evil.example'}))[0], 403)
        self.assertEqual(self.call('POST', path, b'{}', dict(self.access(), **{'X-Cerid-Request': '1'}))[0], 200)
        self.assertTrue(self.c.build(build['number'])['cancel_requested'])

    def test_webhooks_are_verified_deduplicated_and_build(self):
        body = github_push()
        headers = {'X-Hub-Signature-256': 'sha256=' + hosts.hmac_hex('whsec', body), 'X-GitHub-Event': 'push',
                   'X-GitHub-Delivery': 'abc', 'Content-Type': 'application/json'}
        status, answer, _ = self.call('POST', '/webhooks/cerid', body, headers)
        self.assertEqual((status, json.loads(answer)['created']), (200, True))
        self.assertEqual(json.loads(self.call('POST', '/webhooks/cerid', body, headers)[1]), {'duplicate': True})
        self.assertEqual(self.call('POST', '/webhooks/cerid', body + b' ', headers)[0], 401)
        self.assertEqual(self.call('POST', '/webhooks/nope', body, headers)[0], 404)
        ping = dict(headers, **{'X-GitHub-Event': 'ping', 'X-GitHub-Delivery': 'p'})
        self.assertEqual(json.loads(self.call('POST', '/webhooks/cerid', body, ping)[1]), {'ignored': True})
        pull = json.dumps({'action': 'opened', 'pull_request': {
            'number': 3, 'state': 'open', 'head': {'sha': 'd' * 40, 'ref': 'x', 'repo': {'full_name': 'fork/c'}},
            'base': {'ref': 'main', 'repo': {'full_name': 'o/cerid'}}}}).encode()
        fork = dict(headers, **{'X-GitHub-Event': 'pull_request', 'X-GitHub-Delivery': 'f',
                                'X-Hub-Signature-256': 'sha256=' + hosts.hmac_hex('whsec', pull)})
        answer = json.loads(self.call('POST', '/webhooks/cerid', pull, fork)[1])
        self.assertEqual((answer['ignored'], answer['reason'].startswith('fork')), (True, True))
        self.assertEqual(len(self.c.recent()), 1)

    def test_agent_api_and_public_paths(self):
        status, data, headers = self.call('GET', '/agent/cerid-agent.py')
        self.assertEqual(headers['X-Cerid-Agent-SHA256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(self.call('POST', '/api/agent/poll', b'{}', {'X-Cerid-Agent-Protocol': '2'})[0], 426)
        self.assertEqual(self.call('POST', '/api/agent/poll', b'{}')[0], 401)
        self.assertEqual(self.call('POST', '/api/agent/acquire', b'{"identity_token": "x"}')[0], 503)
        token = self.c.create_registration('w', 'q')
        status, body, _ = self.call('POST', '/api/agent/register', json.dumps({
            'registration_token': token, 'protocol': 1, 'name': 'w'}).encode())
        session = json.loads(body)['session_token']
        status, body, _ = self.call('POST', '/api/agent/poll', b'{}', {'Authorization': 'Bearer ' + session})
        self.assertEqual(json.loads(body), {'job': None, 'wait': 5})
        self.assertEqual(json.loads(self.call('GET', '/healthz')[1])['ok'], True)
        self.assertEqual(self.call('GET', '/../../etc/passwd')[0], 404)


# --------------------------------------------------------------------------------------------- test quality

from coordinator import notify, quality, reports  # noqa: E402
ops = load('ci_ops_orchestrator', 'ci/ops.py')
coordinator_main = load('coordinator.main', 'ci/coordinator/main.py')


def put_run(store, lane, build, tests, commit=None, branch='main', bisect=None):
    """One lane run record as pipeline.Lane.record_tests writes it: {test: (outcome, key)}."""
    store.put_json('test-runs/%s/%010d.json' % (lane, build), {
        'build': str(build), 'commit': commit or ('%040x' % build), 'branch': branch, 'repository': 'cerid',
        'lane': lane, 'bisect': bisect, 'tests': {name: {'outcome': o, 'seconds': 1.0 + build / 10.0, 'key': k}
                                                  for name, (o, k) in tests.items()}})


class Recorder:
    """Notification channels as a recorder (the senders the Notifier calls)."""

    def __init__(self):
        self.sent = []

    def __call__(self, channel, secrets, title, body, **kw):
        self.sent.append((channel['kind'], title, body))


def with_notifications(c, recorder, **config):
    c.notifier = notify.Notifier(dict({'channels': [{'kind': 'ntfy', 'url': 'http://ntfy.local/ci'}]}, **config),
                                 None, c.create, c.log, senders={'ntfy': recorder, 'email': recorder})


LANE = 'linux-gcc-debug@gce-linux-x64-intel'


class Quality(Base):
    def test_flaky_only_on_proof_from_one_key(self):
        put_run(self.store, LANE, 1, {'a': ('pass', 'k1'), 'b': ('pass', 'k1'), 'c': ('pass', 'k1')})
        put_run(self.store, LANE, 2, {'a': ('fail', 'k1'), 'b': ('fail', 'k2'), 'c': ('pass', 'k1')})
        found = quality.flaky(self.store, LANE)
        self.assertEqual(set(found), {'a'}, 'b failed on a changed key: a regression, never flaky')
        self.assertEqual((found['a']['passed_in'], found['a']['failed_in']), (['1'], ['2']))
        rows = {t['test']: t for t in quality.summary(self.store, LANE)}
        self.assertEqual((rows['a']['runs'], rows['a']['pass'], rows['a']['fail'], rows['a']['flaky']),
                         (2, 1, 1, True))
        self.assertEqual(rows['b']['last'], 'fail')
        self.assertEqual([h['outcome'] for h in quality.history(self.store, LANE, 'a')], ['pass', 'fail'])

    def test_quarantine_has_an_issue_an_audit_a_notification_and_an_end(self):
        recorder = Recorder()
        with_notifications(self.c, recorder)
        put_run(self.store, LANE, 1, {'a': ('pass', 'k1')})
        put_run(self.store, LANE, 2, {'a': ('fail', 'k1')})
        quarantined, _ = quality.scan(self.c, days=7)
        self.assertEqual([r['test'] for r in quarantined], ['a'])
        record = quarantined[0]
        issue = self.c.get('issues/%s.json' % record['issue'])
        self.assertIn('file this as a finding row in docs/ROADMAP.md', issue['text'])
        self.assertEqual(list(quality.active_quarantine(self.store, LANE, self.clock())), ['a'])
        self.assertTrue(any(e['action'] == 'quarantine' for e in reports.audit_entries(self.c)))
        self.assertEqual(quality.scan(self.c)[0], [], 'already quarantined')
        self.clock.advance(7 * 86400 + 1)
        _, expired = quality.scan(self.c)
        self.assertEqual([r['test'] for r in expired], ['a'])
        self.assertEqual(quality.active_quarantine(self.store, LANE, self.clock()), {})
        self.assertEqual(quality.scan(self.c)[0], [], 'the same evidence never quarantines twice')
        self.assertEqual([t for _, t, _ in recorder.sent], ['Flaky test quarantined: a', 'Quarantine expired: a'])

    def test_quarantine_limit_and_release(self):
        for i in range(3):
            put_run(self.store, LANE, 1 + 2 * i, {'t%d' % i: ('pass', 'k')})
            put_run(self.store, LANE, 2 + 2 * i, {'t%d' % i: ('fail', 'k')})
        quarantined, _ = quality.scan(self.c, max_active=2)
        self.assertEqual(len(quarantined), 2)
        released = quality.release(self.c, LANE, quarantined[0]['test'], 'me')
        self.assertFalse(released['active'])
        with self.assertRaises(LookupError):
            quality.release(self.c, LANE, 'nothing', 'me')

    def test_quarantine_view_separates_held_failures(self):
        self.assertEqual(quality.quarantine_view({'a': 'fail', 'b': 'pass', 'c': 'fail'}, {'a': {}}),
                         (['a', 'c'], ['a'], ['c']))

    def test_narrowing_names_the_first_bad_commit(self):
        record = {'good': {'commit': 'G'}, 'bad': {'commit': 'B'}}
        known = {'c2': 'pass', 'c4': 'fail', 'c5': 'fail'}
        remaining = quality.narrow(record, ['c1', 'c2', 'c3', 'c4', 'c5'], known.get)
        self.assertEqual((record['good']['commit'], record['bad']['commit'], remaining), ('c2', 'c4', ['c3']))
        record = {'good': {'commit': 'G'}, 'bad': {'commit': 'B'}}
        self.assertEqual(quality.narrow(record, ['c1', 'c2'], {}.get), ['c1', 'c2'])

    def test_bisection_from_recorded_runs_then_targeted_reruns(self):
        recorder = Recorder()
        with_notifications(self.c, recorder)
        commits = ['%040x' % n for n in range(10, 15)]  # good, three between, bad
        put_run(self.store, LANE, 1, {'t': ('pass', 'k1')}, commit=commits[0])
        put_run(self.store, LANE, 2, {'t': ('fail', 'k2')}, commit=commits[4])
        between = lambda repository, good, bad: commits[commits.index(good) + 1:commits.index(bad)]
        opened = quality.tick(self.c, REGISTRY, between)
        self.assertEqual(len(opened), 1)
        record = self.c.get('bisect/%s.json' % opened[0]['id'])
        self.assertEqual((record['state'], len(record['reruns'])), ('rerunning', 1))
        rerun = record['reruns'][0]
        self.assertEqual(rerun['commit'], commits[2], 'the middle of the three unknown commits')
        job = self.c.jobs_of(self.c.build(rerun['build']))[0]
        self.assertIn('--tests ^t$', job['command'])
        self.assertEqual(job['agents'], {'queue': 'cerid-cloud', 'device': 'gce-linux-x64-intel'})
        self.assertEqual(self.c.build(rerun['build'])['env'], {'CERID_CI_BISECT': record['id']})
        put_run(self.store, LANE, rerun['build'], {'t': ('fail', 'k2')}, commit=commits[2], bisect=record['id'])
        quality.tick(self.c, REGISTRY, between)
        record = self.c.get('bisect/%s.json' % record['id'])
        self.assertEqual(record['reruns'][-1]['commit'], commits[1])
        put_run(self.store, LANE, record['reruns'][-1]['build'], {'t': ('pass', 'k1')}, commit=commits[1],
                bisect=record['id'])
        quality.tick(self.c, REGISTRY, between)
        record = self.c.get('bisect/%s.json' % record['id'])
        self.assertEqual((record['state'], record['first_bad']), ('found', commits[2]))
        self.assertIn('First bad commit for t', [t for _, t, _ in recorder.sent])
        self.assertEqual(len(quality.tick(self.c, REGISTRY, between)), 0, 're-runs never open new bisections')

    def test_bisection_stops_when_a_rerun_cannot_run_the_test(self):
        commits = ['%040x' % n for n in range(20, 24)]
        put_run(self.store, LANE, 1, {'t': ('pass', 'k1')}, commit=commits[0])
        put_run(self.store, LANE, 2, {'t': ('fail', 'k2')}, commit=commits[3])
        between = lambda repository, good, bad: commits[commits.index(good) + 1:commits.index(bad)]
        record = quality.tick(self.c, REGISTRY, between)[0]
        record = self.c.get('bisect/%s.json' % record['id'])
        self.c.cancel_build(record['reruns'][0]['build'], by='test')
        self.c.tick()
        quality.tick(self.c, REGISTRY, between)
        self.assertEqual(self.c.get('bisect/%s.json' % record['id'])['state'], 'stopped')


class LaneQuality(unittest.TestCase):
    def lane(self, scratch, failures, quarantined=(), tests=None):
        store = pipeline.LocalStore(Path(scratch) / 'store')
        for name in quarantined:
            store.put_json('orchestrator/' + quality.quarantine_name(LANE, name), {
                'lane': LANE, 'test': name, 'active': True, 'expires': time.time() + 3600, 'issue': 'CI-FLAKY-1',
                'expires_text': 'soon'})
        build = Path(scratch) / 'build'
        (build / 'Testing/Temporary').mkdir(parents=True)
        junit = build / 'Testing/Temporary/ctest-junit.xml'
        cases = ''.join('<testcase name="%s" time="0.5">%s</testcase>' % (
            name, '<failure/>' if name in failures else '') for name in ('a', 'b', 'c'))
        junit.write_text('<testsuite>%s</testsuite>' % cases, encoding='utf-8')

        class Run(pipeline.Runner):
            def __call__(self, argv, check=True, **kw):
                self.commands.append({'argv': argv})
                text = ' '.join(map(str, argv))
                if 'ci-evidence.py watch' in text:
                    return 8 if failures else 0
                if 'check-registered-failures.py' in text:
                    self.gate = argv
                return 0
        run = Run(environ={'CERID_CI_BUILD_NUMBER': '12', 'CERID_CI_COMMIT': 'c' * 40, 'CERID_CI_BRANCH': 'main',
                           'CERID_CI': '1'}, dry=False, log=lambda *_: None)
        device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-intel']
        lane = pipeline.Lane('linux-gcc-debug', device, tiers=pipeline.ci_tier.load_tiers(pipeline.TIERS), run=run,
                             temp=scratch, store=store, tests=tests)
        lane.build_dir = str(build)
        lane.junit = lambda name='ctest-junit.xml': str(junit)
        lane.test_keys = lambda: {'a': 'ka', 'b': 'kb', 'c': 'kc'}
        lane.cached_tests = lambda: None
        lane.test()
        return lane, run, store, junit

    def test_runs_are_recorded_with_outcome_duration_and_key(self):
        with tempfile.TemporaryDirectory() as scratch:
            _, run, store, _ = self.lane(scratch, failures=())
            record = store.get_json('test-runs/%s/0000000012.json' % LANE)
            self.assertEqual(record['tests']['a'], {'outcome': 'pass', 'seconds': 0.5, 'key': 'ka'})
            self.assertEqual((record['commit'], record['branch']), ('c' * 40, 'main'))

    def test_a_quarantined_flaky_failure_is_reported_and_does_not_fail_the_lane(self):
        with tempfile.TemporaryDirectory() as scratch:
            lane, run, store, junit = self.lane(scratch, failures=('b',), quarantined=('b',))
            gate = ' '.join(map(str, run.gate))
            self.assertIn('ctest-junit-quarantine-view.xml', gate)
            self.assertTrue(gate.endswith('--ctest-exit 0'))
            self.assertIn('<failure', junit.read_text(encoding='utf-8'), 'the original evidence keeps the failure')
            self.assertEqual(lane.quarantined, ['b'])
            self.assertTrue(any('annotate --context quarantine-linux-gcc-debug --style warning' in ' '.join(
                map(str, c['argv'])) for c in run.commands))
            record = store.get_json('test-runs/%s/0000000012.json' % LANE)
            self.assertEqual(record['tests']['b']['outcome'], 'fail', 'the history records the failure')

    def test_any_other_failure_keeps_the_full_verdict(self):
        with tempfile.TemporaryDirectory() as scratch:
            _, run, _, junit = self.lane(scratch, failures=('b', 'c'), quarantined=('b',))
            self.assertEqual(run.gate[run.gate.index('--junit') + 1], str(junit))
            self.assertEqual(run.gate[-1], '8')

    def test_a_bisection_rerun_selects_its_test_and_skips_the_cache(self):
        with tempfile.TemporaryDirectory() as scratch:
            _, run, store, _ = self.lane(scratch, failures=(), tests='^b$')
            watch = next(c['argv'] for c in run.commands if 'ci-evidence.py' in ' '.join(map(str, c['argv'])))
            self.assertEqual(watch[watch.index('-R') + 1], '^b$')
            self.assertEqual(store.list('tests-passed/'), [], 'a re-run never feeds the per-test cache')


# --------------------------------------------------------------------------------------------- notifications

class Notifications(Base):
    def test_once_per_key_filtered_and_never_fatal(self):
        recorder = Recorder()
        with_notifications(self.c, recorder, branches=['main'], events=['failure', 'budget'])
        self.assertEqual(self.c.notify('failure', 't', 'b', key='k1', branch='main'), ['ntfy'])
        self.assertEqual(self.c.notify('failure', 't', 'b', key='k1', branch='main'), [], 'sent once')
        self.assertEqual(self.c.notify('failure', 't', 'b', key='k2', branch='feature'), [])
        self.assertEqual(self.c.notify('recovery', 't', 'b', key='k3', branch='main'), [])

        def broken(*a, **kw):
            raise OSError('ntfy down')
        self.c.notifier.senders['ntfy'] = broken
        self.assertEqual(self.c.notify('budget', 't', 'b', key='k4'), [])
        self.assertTrue(any(r['event'] == 'notify_failed' for r in self.c.log.records))

    def test_failure_then_recovery_on_the_branch(self):
        recorder = Recorder()
        with_notifications(self.c, recorder)
        w = agent(self.c, queue='q', name='w')
        for commit, code in (('a' * 40, 1), ('b' * 40, 0)):
            build, _ = self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': commit,
                                                     'trusted': True}, 'webhook',
                                           document={'steps': [{'key': 's', 'command': 'x', 'agents': {
                                               'queue': 'q'}}]})
            got = self.c.poll(w)
            self.c.start(got['job']['id'], got['token'])
            self.c.finish(got['job']['id'], got['token'], code)
        titles = [t for _, t, _ in recorder.sent]
        self.assertEqual(len(titles), 2)
        self.assertTrue(titles[0].startswith('Build #1 failed'))
        self.assertTrue(titles[1].startswith('Build #2 recovered'))

    def test_email_and_ntfy_senders(self):
        class Secrets:
            def value(self, name):
                return 'pw-' + name
        sent = {}

        class Smtp:
            def __init__(self, host, port, timeout=None, context=None):
                sent['server'] = (host, port)

            def starttls(self, context=None):
                sent['tls'] = True

            def login(self, user, password):
                sent['login'] = (user, password)

            def send_message(self, message):
                sent['message'] = message

            def quit(self):
                sent['quit'] = True
        notify.email({'host': 'smtp.example', 'port': 587, 'from': 'ci@example', 'to': ['me@example'],
                      'username': 'ci', 'password_secret': 'smtp'}, Secrets(), 'Build failed', 'body', smtp=Smtp)
        self.assertEqual((sent['server'], sent['tls'], sent['login'], sent['quit']),
                         (('smtp.example', 587), True, ('ci', 'pw-smtp'), True))
        self.assertEqual(sent['message']['Subject'], '[Cerid CI] Build failed')
        requests = []

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def read(self):
                return b''
        notify.ntfy({'url': 'http://ntfy.tailnet/ci', 'token_secret': 'ntfy'}, Secrets(), 'T', 'B', priority='high',
                    opener=lambda request, timeout=None: requests.append(request) or Response())
        self.assertEqual(requests[0].get_header('Authorization'), 'Bearer pw-ntfy')
        self.assertEqual(requests[0].data, b'B')

    def test_budget_alerts_once_per_threshold(self):
        recorder = Recorder()
        with_notifications(self.c, recorder)
        self.store.put_json('ledger/%s/m.json' % pipeline.month_of(dt_now(self.clock)), {'usd': 41.0})
        d = dispatch.Dispatcher(self.c, REGISTRY, {})
        d.tick()
        d.tick()
        self.assertEqual([t for _, t, _ in recorder.sent], ['CI spend at 80 % of the 50 USD cap'])


def dt_now(clock):
    import datetime
    return datetime.datetime.fromtimestamp(clock(), datetime.timezone.utc)


# --------------------------------------------------------------------------------------------- reports

class Reports(Base):
    def test_costs_by_build_lane_device_with_a_forecast(self):
        month = pipeline.month_of(dt_now(self.clock))
        for i, (build, key, device, usd) in enumerate(((1, 'lane-a', 'd1', 1.0), (1, 'lane-b', 'd2', 2.0),
                                                       (2, 'lane-a', 'd1', 3.0))):
            self.store.put_json('ledger/%s/m%d.json' % (month, i), {'build': build, 'key': key, 'device': device,
                                                                   'usd': usd, 'hours': 1.0})
        data = reports.costs(self.c, REGISTRY, month)
        self.assertEqual(data['spent_usd'], 6.0)
        self.assertEqual({r['key']: r['usd'] for r in data['by_lane']}, {'lane-a': 4.0, 'lane-b': 2.0})
        self.assertEqual(data['by_build'][0], {'build': '1', 'usd': 3.0, 'hours': 2.0, 'machines': 2})
        self.assertGreaterEqual(data['forecast_usd'], 6.0)
        self.assertEqual(len(data['months']), 6)

    def test_queue_time_and_availability_slos(self):
        self.build_one = self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': COMMIT,
                                                       'trusted': True}, 'webhook')
        self.c.tick()
        self.clock.advance(30)
        self.c.poll(agent(self.c, name='l'))
        for minute in range(10):
            self.clock.advance(60)
            self.c.tick()
        data = reports.slo(self.c)
        queue = data['queues'][0]
        self.assertEqual((queue['queue'], queue['samples'], queue['p95_s'], queue['met']),
                         ('cerid-launcher', 1, 30.0, True))
        self.assertGreater(data['availability'], 0.8)
        self.clock.advance(3600)
        self.c.tick()
        self.assertLess(reports.slo(self.c)['availability'], data['availability'], 'an hour without ticks counts')

    def test_a_job_taken_before_any_watchdog_tick_is_still_measured(self):
        self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': COMMIT, 'trusted': True},
                            'webhook')
        self.c.poll(agent(self.c, name='l'))
        self.assertEqual([q['samples'] for q in reports.slo(self.c)['queues']], [1])

    def test_audit_records_manual_actions(self):
        build, _ = self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': COMMIT}, 'manual',
                                       created_by='alice')
        self.c.cancel_build(build['number'], by='bob')
        actions = [(e['who'], e['action']) for e in reports.audit_entries(self.c)]
        self.assertIn(('alice', 'build-create'), actions)
        self.assertIn(('bob', 'build-cancel'), actions)


# --------------------------------------------------------------------------------------------- access

class Access(unittest.TestCase):
    def test_iap_tunnel_mode_accepts_loopback_and_the_iap_range_only(self):
        ui = auth.UiAuth('iap-tunnel')
        self.assertEqual(ui.user({}, ('127.0.0.1', 5)), 'operator (local)')
        self.assertEqual(ui.user({}, ('35.235.241.7', 5)), 'operator (IAP tunnel)')
        for peer in (('10.128.0.9', 5), ('8.8.8.8', 5), ('35.235.0.1', 5), ('', 0)):
            with self.assertRaises(auth.AuthError):
                ui.user({'Cf-Access-Jwt-Assertion': 'x', 'Tailscale-User-Login': 'me'}, peer)

    def test_tailscale_mode_asks_tailscaled_and_never_trusts_a_header(self):
        class Whois:
            def login(self, host, port):
                return {'100.101.1.2': 'me@example.com', '100.101.1.3': 'other@example.com'}[host]
        ui = auth.UiAuth('tailscale', allowed=['me@example.com'], whois=Whois())
        self.assertEqual(ui.user({}, ('100.101.1.2', 4)), 'me@example.com')
        with self.assertRaises(auth.AuthError):
            ui.user({'Tailscale-User-Login': 'me@example.com'}, ('100.101.1.3', 4))
        with self.assertRaises(auth.AuthError):
            ui.user({'Tailscale-User-Login': 'me@example.com'}, ('10.0.0.2', 4))
        with self.assertRaises(ValueError):
            auth.UiAuth('tailscale')

    def test_whois_through_the_tailscale_command(self):
        calls = []

        def run(argv, **kw):
            calls.append(argv)
            body = {'UserProfile': {'LoginName': 'me@example.com'}, 'Node': {'Tags': None}}
            if argv[-1].startswith('100.64.0.9'):
                body['Node']['Tags'] = ['tag:server']
            return subprocess.CompletedProcess(argv, 0, json.dumps(body), '')
        whois = auth.TailscaleWhois(run=run, socket_path=str(ROOT / 'no-such-socket'))
        self.assertEqual(whois.login('100.64.0.8', 51000), 'me@example.com')
        self.assertEqual(whois.login('100.64.0.8', 51000), 'me@example.com')
        self.assertEqual(len(calls), 1, 'cached')
        self.assertEqual(calls[0], ['tailscale', 'whois', '--json', '100.64.0.8:51000'])
        with self.assertRaises(auth.AuthError):
            whois.login('100.64.0.9', 1)

    def test_listeners(self):
        self.assertEqual(coordinator_main.listeners({}, {}), [('127.0.0.1', 8780, ('ui', 'agent'))])
        self.assertEqual(coordinator_main.listeners({}, {'CERID_CI_LISTEN': '127.0.0.1:8780,10.128.0.2:8780=agent'}),
                         [('127.0.0.1', 8780, ('ui', 'agent')), ('10.128.0.2', 8780, ('agent',))])
        with self.assertRaises(SystemExit):
            coordinator_main.listeners({}, {'CERID_CI_LISTEN': '1.2.3.4:1=everything'})
        config, problems = coordinator_main.load_config(ROOT / 'ci/coordinator.json', {
            'CERID_CI_REPOSITORY_URL': 'git@example:o/r.git'})
        self.assertEqual((problems, config['ui']['mode'], config['repositories']['cerid']['poll']['interval_seconds']),
                         ([], 'iap-tunnel', 60))
        self.assertNotIn('webhook_secret', config['repositories']['cerid'], 'webhooks are an optional add-on')


class Surfaces(Base):
    def serve(self, surfaces, ui=None):
        server = service.serve(service.Service(self.c, ui or auth.UiAuth('local', loopback=True), CONFIG,
                                               registry=REGISTRY, stream_seconds=5, stream_interval=0.05),
                               '127.0.0.1', 0, surfaces)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return 'http://127.0.0.1:%d' % server.server_address[1]

    def get(self, url, headers=None):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers or {}), timeout=10) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            body = error.read()
            error.close()
            return error.code, body

    def test_each_listener_serves_only_its_surfaces(self):
        agent_only = self.serve(('agent',))
        self.assertEqual(self.get(agent_only + '/api/builds')[0], 404)
        self.assertEqual(self.get(agent_only + '/')[0], 404)
        self.assertEqual(self.get(agent_only + '/healthz')[0], 200)
        self.assertEqual(self.get(agent_only + '/agent/cerid-agent.py')[0], 200)
        ui_only = self.serve(('ui',))
        self.assertEqual(self.get(ui_only + '/api/builds')[0], 200)
        self.assertEqual(self.get(ui_only + '/agent/cerid-agent.py')[0], 404)
        self.assertEqual(self.get(ui_only + '/webhooks/cerid')[0], 404)

    def test_iap_tunnel_listener_refuses_a_network_peer(self):
        base = self.serve(('ui', 'agent'), ui=auth.UiAuth('iap-tunnel', networks=('35.235.240.0/20',)))
        self.assertEqual(self.get(base + '/api/builds')[0], 401, '127.0.0.1 is not in this listener\'s networks')

    def test_server_sent_events_follow_a_build_and_a_log(self):
        base = self.serve(('ui', 'agent'))
        build, _ = self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': COMMIT,
                                                 'trusted': True}, 'webhook')
        got = self.c.poll(agent(self.c, name='l'))
        self.c.start(got['job']['id'], got['token'])
        self.c.append_log(got['job']['id'], got['token'], 1, 0, b'\x1b[31mred\x1b[0m line\n')

        def finish():
            time.sleep(0.3)
            self.c.append_log(got['job']['id'], got['token'], 1, 1, b'second\n')
            self.c.finish(got['job']['id'], got['token'], 0)
        threading.Thread(target=finish, daemon=True).start()
        status, body = self.get(base + '/api/stream?job=%s' % got['job']['id'])
        text = body.decode('utf-8')
        self.assertEqual(status, 200)
        self.assertIn('event: log', text)
        self.assertIn('red', text)
        self.assertIn('second', text)
        self.assertIn('event: end', text)
        status, body = self.get(base + '/api/stream?build=%d' % build['number'])
        self.assertIn('event: build', body.decode('utf-8'))

    def test_report_endpoints(self):
        base = self.serve(('ui',))
        for path in ('costs', 'slo', 'audit', 'quality'):
            status, body = self.get(base + '/api/' + path)
            self.assertEqual(status, 200, path)
            json.loads(body)


# --------------------------------------------------------------------------------------------- reproducibility

class Reproducibility(Base):
    def test_the_record_leaves_out_tokens_and_secret_values(self):
        job = {'secrets': {'DEPLOY_KEY': 's3cret'}, 'env': {'CERID_CI_DEVICE': 'gce-linux-x64-intel'}}
        env = {'PATH': '/bin', 'DEPLOY_KEY': 's3cret', 'CERID_AGENT_JOB_TOKEN': 'cjt_x', 'GIT_CONFIG_VALUE_0': 'B t'}
        record = agent_module.reproducibility(job, env, {'commit': COMMIT, 'strategy': 'mirror'}, 'vm-1',
                                              run=lambda argv, **kw: subprocess.CompletedProcess(argv, 0, 'git 2.0\n',
                                                                                                 ''),
                                              which=lambda name, path=None: '/bin/' + name if name == 'git' else None)
        text = json.dumps(record)
        self.assertNotIn('s3cret', text)
        self.assertNotIn('cjt_x', text)
        self.assertEqual(record['env']['DEPLOY_KEY'], '[secret]')
        self.assertEqual(record['tools'], {'git': 'git 2.0'})
        self.assertEqual((record['commit'], record['device'], record['checkout']),
                         (COMMIT, 'gce-linux-x64-intel', 'mirror'))
        build, _ = self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': COMMIT}, 'manual')
        got = self.c.poll(agent(self.c, name='l'))
        self.c.start(got['job']['id'], got['token'])
        self.c.record_environment(got['job']['id'], got['token'], record)
        self.assertEqual(self.c.job(got['job']['id'])['repro']['tools'], {'git': 'git 2.0'})


# --------------------------------------------------------------------------------------------- ops CLI

class Cli(Base):
    def ops(self, *argv, environ=None):
        out, err = io.StringIO(), io.StringIO()
        import contextlib
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ops.main(['--store', self.scratch.name] + list(argv), environ=environ or {})
        return code, out.getvalue(), err.getvalue()

    def test_every_command_speaks_json(self):
        build, _ = self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': COMMIT}, 'manual')
        for argv in (('build', 'list'), ('build', 'show', str(build['number'])), ('agent', 'list'),
                     ('queue', 'list'), ('test', 'list'), ('test', 'flaky'), ('quarantine', 'list'),
                     ('bisect', 'list'), ('audit', 'list'), ('slo',), ('health',), ('device', 'list'),
                     ('accounts',), ('cost',)):
            code, out, err = self.ops('--json', *argv)
            self.assertIn(code, (0, 1), (argv, err))
            json.loads(out)

    def test_legacy_verbs_still_work(self):
        self.assertEqual(ops.legacy(['builds']), ['build', 'list'])
        self.assertEqual(ops.legacy(['--json', 'add-device', '--id', 'x']), ['device', 'add', '--id', 'x', '--json'])
        self.assertEqual(ops.legacy(['build', '--number', '3']), ['build', 'show', '3'])
        self.assertEqual(ops.legacy(['cancel', '--job', 'j']), ['job', 'cancel', 'j'])
        self.assertEqual(ops.legacy(['agent-token', 'create', '--name', 'n']), ['token', 'create', '--name', 'n'])
        self.assertEqual(ops.legacy(['backup', '--out', 'f']), ['backup', 'create', '--out', 'f'])
        self.assertEqual(ops.legacy(['--store', 's', 'budget', '--pull']), ['--store', 's', 'cost', '--pull'])

    def test_errors_say_what_to_do_next(self):
        code, out, err = self.ops('build', 'show', '999')
        self.assertEqual(code, 2)
        self.assertIn('error: build 999', err)
        self.assertIn('next:', err)
        code, out, err = self.ops('--json', 'build', 'show', '999')
        self.assertEqual(json.loads(out)['next'].split(':')[0], 'list what exists')

    def test_human_output_is_plain_without_a_terminal(self):
        self.c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': COMMIT}, 'manual')
        code, out, _ = self.ops('build', 'list')
        self.assertNotIn('\x1b[', out)
        self.assertIn('scheduled', out)
        self.assertIn('\x1b[', ops.Out(True).state('failed'))

    def test_completion_scripts_name_every_command(self):
        for shell in ('bash', 'zsh', 'powershell'):
            code, out, _ = self.ops('completion', shell)
            self.assertEqual(code, 0)
            for word in ('build', 'quarantine', 'release', '--json', 'ui'):
                self.assertIn(word, out, shell)
            self.assertNotIn('add-device', out, 'legacy verbs are not offered')

    def test_ui_opens_the_iap_tunnel_then_the_browser(self):
        started, opened = [], []

        class Tunnel:
            stdout = io.StringIO('')
            polls = [None]

            def poll(self):
                return None

            def wait(self):
                return 0

            def terminate(self):
                started.append('terminated')
        args = ops.build_parser().parse_args(['ui', '--project', 'cerid-ci', '--zone', 'us-central1-a', '--once',
                                              '--port', '0'])
        ctx = type('Ctx', (), {'args': args, 'environ': {}, 'out': ops.Out(False)})()
        readiness = iter([False, True])
        original = ops.gcloud_path
        ops.gcloud_path = lambda: 'gcloud'
        try:
            code, data, _ = ops.cmd_ui(ctx, popen=lambda argv, **kw: started.append(argv) or Tunnel(),
                                       ready=lambda port: next(readiness), browser=opened.append, sleep=lambda s: None)
        finally:
            ops.gcloud_path = original
        self.assertEqual(started[0][:5], ['gcloud', 'compute', 'start-iap-tunnel', 'cerid-ci-launcher', '8780'])
        self.assertIn('--project', started[0])
        self.assertEqual(opened, [data['url']])
        self.assertEqual(started[-1], 'terminated')

    def test_ui_explains_a_refused_tunnel(self):
        class Dead:
            stdout = io.StringIO('ERROR: (gcloud.compute.start-iap-tunnel) Error while connecting [4033: not '
                                 'authorized]')

            def poll(self):
                return 1

            def terminate(self):
                pass
        args = ops.build_parser().parse_args(['ui', '--once', '--port', '0'])
        ctx = type('Ctx', (), {'args': args, 'environ': {}, 'out': ops.Out(False)})()
        original = ops.gcloud_path
        ops.gcloud_path = lambda: 'gcloud'
        try:
            with self.assertRaises(ops.OpsError) as raised:
                ops.cmd_ui(ctx, popen=lambda argv, **kw: Dead(), ready=lambda port: False, sleep=lambda s: None)
        finally:
            ops.gcloud_path = original
        self.assertIn('4033', str(raised.exception))
        self.assertIn('IAP-secured Tunnel User', raised.exception.hint)


# --------------------------------------------------------------------------------------------- web UI logic

NODE = __import__('shutil').which('node')
WEB_CHECKS = r'''
const assert = require('assert');
const m = require(process.argv[2]);
// Live log text arrives in chunks that end anywhere: a line split across two chunks is one line.
const lines = [];
let open = m.appendLog(lines, false, 'one\ntw');
open = m.appendLog(lines, open, 'o\nthree\n');
open = m.appendLog(lines, open, 'four\n');
assert.deepStrictEqual(lines, ['one', 'two', 'three', 'four']);
assert.strictEqual(open, false);
open = m.appendLog(lines, open, 'five');
assert.deepStrictEqual(lines.slice(-1), ['five']);
assert.strictEqual(open, true);
// ANSI colours become escaped spans; other escape sequences vanish.
assert.strictEqual(m.ansiToHtml('\x1b[1;31mred <b>\x1b[0m ok\x1b[2K'), '<span class="ansi-bold fg-31">red &lt;b&gt;</span> ok');
// The first error line, never a zero count or the agent's own progress line.
assert.strictEqual(m.firstErrorLine(['~~~ running x', '0 errors', '100% tests passed, 0 tests failed out of 5',
  'plan.cpp:214:9: error: twiddle_bank was not declared', 'later: error: second']), 3);
assert.strictEqual(m.isErrorLine('The following tests FAILED:'), true);
assert.strictEqual(m.isErrorLine('LINK : fatal error LNK1120: 1 unresolved externals'), true);
assert.strictEqual(m.isErrorLine('Traceback (most recent call last):'), true);
assert.strictEqual(m.isErrorLine('-- Looking for -Werror support'), false);
assert.strictEqual(m.isErrorLine('~~~ agent error: git fetch exited 128'), true);
// Status is an icon and a word, never colour alone.
const html = m.statusHtml('timed_out');
assert.ok(html.includes('#i-failed') && html.includes('Timed out'));
assert.ok(m.statusHtml('failed', {soft: true}).includes('Soft fail'));
assert.strictEqual(m.cleanLabel(':hammer: win-debug'), 'win-debug');
// The QUAL filters.
const data = {features: {build: {title: 'Build'}, gpu: {title: 'GPU'}},
  devices: [{id: 'a', tier: 'A', os: 'Ubuntu', gpu: 'L4', arch: 'x86_64', kind: 'VM', features: ['build', 'gpu']},
            {id: 'b', tier: 'C', os: 'Windows', gpu: 'none', arch: 'aarch64', kind: 'VM', features: ['build']}],
  cells: [{device: 'a', feature: 'build', state: 'fail'}, {device: 'b', feature: 'build', state: 'waiting'}]};
assert.deepStrictEqual(m.matrixRows(data, {}).rows.map((r) => r.device.id), ['a', 'b']);
assert.deepStrictEqual(m.matrixRows(data, {tier: 'C'}).rows.map((r) => r.device.id), ['b']);
assert.deepStrictEqual(m.matrixRows(data, {state: 'fail'}).rows.map((r) => r.device.id), ['a']);
assert.deepStrictEqual(m.matrixRows(data, {results: true}).rows.map((r) => r.device.id), ['a']);
assert.deepStrictEqual(m.matrixRows(data, {query: 'windows'}).rows.map((r) => r.device.id), ['b']);
// Markdown escapes before it formats.
assert.ok(!m.markdown('<script>x</script>').includes('<script>'));
console.log('web checks passed');
'''


class Demo(unittest.TestCase):
    def test_the_fixture_fills_every_page(self):
        from coordinator import demo
        with tempfile.TemporaryDirectory() as scratch:
            store = pipeline.LocalStore(scratch)
            c = demo.seed(store)
            self.assertEqual([b['state'] for b in c.recent()], ['passed', 'failed', 'passed', 'passed'])
            self.assertEqual(len(store.list('orchestrator/quarantine/')), 1)
            self.assertEqual(len(store.list('orchestrator/bisect/')), 1)
            self.assertTrue(reports.audit_entries(c))
            self.assertGreater(reports.costs(c, REGISTRY, pipeline.month_of())['spent_usd'], 0)


class WebUi(unittest.TestCase):
    @unittest.skipUnless(NODE, 'Node.js runs the web UI\'s pure functions')
    def test_pure_functions_of_the_web_ui(self):
        with tempfile.TemporaryDirectory() as scratch:
            script = Path(scratch) / 'checks.js'
            script.write_text(WEB_CHECKS, encoding='utf-8')
            completed = subprocess.run([NODE, str(script), str(ROOT / 'ci/web/app.js')], capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stderr[-2000:])

    def test_the_page_loads_nothing_from_elsewhere_and_has_no_inline_code(self):
        html = (ROOT / 'ci/web/index.html').read_text(encoding='utf-8')
        self.assertNotRegex(html, r'(src|href)="https?://')
        self.assertNotIn('<script>', html)
        self.assertNotRegex(html, r'\son[a-z]+="', 'no inline event handler (CSP forbids them)')
        self.assertNotIn(' style="', html + (ROOT / 'ci/web/app.js').read_text(encoding='utf-8'))
        self.assertIn('class="skip"', html)
        css = (ROOT / 'ci/web/style.css').read_text(encoding='utf-8')
        for token in ('prefers-color-scheme: dark', 'prefers-reduced-motion', ':focus-visible', 'data-theme="dark"'):
            self.assertIn(token, css)

    def test_text_and_status_colours_meet_wcag_aa_in_both_themes(self):
        import re
        css = (ROOT / 'ci/web/style.css').read_text(encoding='utf-8')

        def tokens(block):
            return dict(re.findall(r'--([a-z0-9-]+):\s*(#[0-9a-f]{6})', block))
        light = tokens(css.split('@media (prefers-color-scheme: dark)')[0])
        dark = dict(light, **tokens(css.split(':root[data-theme="dark"] {')[1].split('}')[0]))
        auto_dark = tokens(css.split(':root:not([data-theme="light"]) {')[1].split('}')[0])
        self.assertEqual(tokens(css.split(':root[data-theme="dark"] {')[1].split('}')[0]), auto_dark,
                         'the chosen dark theme and the system dark theme are the same tokens')

        def luminance(colour):
            channels = [int(colour[i:i + 2], 16) / 255 for i in (1, 3, 5)]
            channels = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
            return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2]

        def contrast(a, b):
            high, low = sorted((luminance(a), luminance(b)), reverse=True)
            return (high + 0.05) / (low + 0.05)
        pairs = [('text', 'bg'), ('text', 'surface'), ('text-2', 'surface'), ('text-2', 'surface-2'),
                 ('text-3', 'surface'), ('text-3', 'surface-2'), ('accent', 'surface'), ('accent-text', 'accent'),
                 ('pass', 'pass-bg'), ('fail', 'fail-bg'), ('run', 'run-bg'), ('wait', 'wait-bg'), ('cache', 'cache-bg'),
                 ('budget', 'budget-bg'), ('soft', 'soft-bg'), ('cancel', 'cancel-bg'), ('log-text', 'log-bg'),
                 ('log-dim', 'log-bg')]
        for name, theme in (('light', light), ('dark', dark)):
            for fg, bg in pairs:
                self.assertGreaterEqual(contrast(theme[fg], theme[bg]), 4.5, '%s: %s on %s' % (name, fg, bg))


if __name__ == '__main__':
    unittest.main()
