#!/usr/bin/env python3
"""End-to-end test of the Cerid orchestrator on this machine (ADR-0135 §12): the coordinator's HTTP service and
watchdog in-process on 127.0.0.1 with the local filesystem backend, one real agent process in long-running mode,
and a throwaway git repository with a trivial pipeline (no engine build). It drives a signed webhook (and its
replay), the bootstrap step uploading the pipeline, dependencies and a wait step, a dynamic upload, a soft failure,
a declared secret redacted from the live log, ANSI output, an artifact, an annotation, build meta-data, a step
outcome, the build's checkout through the coordinator's git mirror with the job token, and a cancellation that
stops the running command. Run: python scripts/test-ci-orchestrator-e2e.py (stdlib unittest; about 20 s).
"""
from pathlib import Path
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
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
from coordinator import auth, hosts, scheduler, service, triggers  # noqa: E402

PY = sys.executable
SECRET = 'e2e-secret-4b1d9f'
HELLO = r'''import os, pathlib
print("\x1b[1;32mhello from the agent\x1b[0m")
print("secret is " + os.environ["DEMO_SECRET"])
print("split " + os.environ["DEMO_SECRET"][:5], end="", flush=True)
print(os.environ["DEMO_SECRET"][5:], flush=True)
assert "CERID_E2E_HOST_ONLY" not in os.environ, "the host environment leaked into the step"
pathlib.Path("out").mkdir(exist_ok=True)
pathlib.Path("out/result.txt").write_text("artifact " + os.environ["CERID_CI_COMMIT"], encoding="utf-8")
'''
META = r'''import os, subprocess, sys
agent = [sys.executable, os.environ["CERID_AGENT_SCRIPT"]]
subprocess.run(agent + ["meta-data", "set", "tier", "change"], check=True)
value = subprocess.run(agent + ["meta-data", "get", "tier"], check=True, capture_output=True, text=True).stdout
assert value.strip() == "change", value
outcome = subprocess.run(agent + ["step", "outcome", "hello"], check=True, capture_output=True, text=True).stdout
assert outcome.strip() == "passed", outcome
subprocess.run(agent + ["annotate", "--context", "e2e", "--style", "success", "# E2E\n| a | b |\n|---|---|\n| 1 | 2 |"],
               check=True)
subprocess.run(agent + ["pipeline", "upload", "more.json"], check=True)
data = subprocess.run(agent + ["artifact", "download", "out/result.txt", "got.txt"], check=True)
print(open("got.txt", encoding="utf-8").read())
'''
SLEEP = r'''import time
print("sleeping", flush=True)
time.sleep(120)
'''


def git(repo, *args):
    subprocess.run(['git', '-C', str(repo)] + list(args), check=True, capture_output=True)


def make_repository(root):
    repo = Path(root) / 'demo'
    repo.mkdir()
    (repo / 'steps').mkdir()
    (repo / 'steps/hello.py').write_text(HELLO, encoding='utf-8')
    (repo / 'steps/meta.py').write_text(META, encoding='utf-8')
    (repo / 'steps/sleep.py').write_text(SLEEP, encoding='utf-8')
    py = '"%s"' % PY
    queue = {'queue': 'e2e'}
    (repo / 'pipeline.json').write_text(json.dumps({'schema': 'cerid-ci-pipeline/1', 'env': {'CERID_E2E': '1'},
                                                    'steps': [
        {'key': 'hello', 'label': 'hello', 'command': py + ' steps/hello.py', 'agents': queue,
         'secrets': ['demo-secret'], 'artifact_paths': 'out/*.txt'},
        {'key': 'meta', 'label': 'meta', 'command': py + ' steps/meta.py', 'agents': queue, 'depends_on': 'hello'},
        'wait',
        {'key': 'soft', 'label': 'soft failure', 'command': py + ' -c "raise SystemExit(3)"', 'agents': queue,
         'soft_fail': [{'exit_status': 3}]}]}), encoding='utf-8')
    (repo / 'more.json').write_text(json.dumps({'steps': [
        {'key': 'dynamic', 'label': 'uploaded by a step', 'command': py + ' -c "print(42)"', 'agents': queue}]}),
        encoding='utf-8')
    (repo / 'cancel.json').write_text(json.dumps({'steps': [
        {'key': 'sleep', 'label': 'long', 'command': py + ' steps/sleep.py', 'agents': queue}]}), encoding='utf-8')
    git(repo, 'init', '-q', '-b', 'main')
    git(repo, 'add', '.')
    git(repo, '-c', 'user.name=e2e', '-c', 'user.email=e2e@example.invalid', 'commit', '-q', '-m', 'e2e')
    return repo, subprocess.run(['git', '-C', str(repo), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                                check=True).stdout.strip()


def upload_command(document):
    return '"%s" "%s" pipeline upload %s' % (PY, ROOT / 'ci/agent/agent.py', document)


class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory()
        root = Path(cls.scratch.name)
        cls.repo, cls.commit = make_repository(root)
        secrets = root / 'secrets'
        secrets.mkdir()
        (secrets / 'demo-secret').write_text(SECRET, encoding='utf-8')
        (secrets / 'hook-secret').write_text('whsec-e2e', encoding='utf-8')
        cls.store = pipeline.LocalStore(root / 'state')
        repo_config = {'url': str(cls.repo), 'host': 'generic', 'webhook_secret': 'hook-secret',
                       'trusted_branches': ['main'], 'depth': 0,
                       'pipeline': {'command': upload_command('pipeline.json'), 'queue': 'e2e'}}
        config = {'url': '', 'repositories': {
            'demo': repo_config,
            'mirrored': dict(repo_config, checkout='mirror', depth=1,
                             pipeline={'command': upload_command('cancel.json'), 'queue': 'e2e'})},
            'limits': {'lost_after_seconds': 30, 'cancel_grace_seconds': 20, 'idle_poll_seconds': 0.2}}
        cls.log = scheduler.JsonLog(open(os.devnull, 'w'))
        mirror = triggers.Mirror(root / 'mirror')
        cls.c = scheduler.Coordinator(cls.store, config, secrets=scheduler.DirectorySecrets(secrets), log=cls.log,
                                      mirror=mirror)
        cls.c.ensure_schema()
        web = service.Service(cls.c, auth.UiAuth('local', loopback=True), config, mirror=mirror,
                              dashboard=lambda: '<!doctype html><title>QUAL</title>')
        cls.server = service.serve(web, '127.0.0.1', 0)
        cls.base = 'http://127.0.0.1:%d' % cls.server.server_address[1]
        cls.c.url = cls.base
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.loop = service.Loop(cls.c, interval=0.2)
        cls.loop.start()
        token_file = root / 'registration-token'
        token_file.write_text(cls.c.create_registration('e2e-agent', 'e2e', {'os': sys.platform}), encoding='utf-8')
        env = {k: v for k, v in os.environ.items()}
        env['CERID_E2E_HOST_ONLY'] = 'must not reach a step'
        cls.agent_log = open(root / 'agent.log', 'wb')
        cls.agent = subprocess.Popen([PY, str(ROOT / 'ci/agent/agent.py'), 'start', '--coordinator', cls.base,
                                      '--registration-token-file', str(token_file), '--name', 'e2e-agent',
                                      '--build-path', str(root / 'builds'), '--poll-seconds', '0.2',
                                      '--heartbeat-seconds', '0.5', '--exit-when-idle', '120'],
                                     env=env, stdout=cls.agent_log, stderr=subprocess.STDOUT)

    @classmethod
    def tearDownClass(cls):
        cls.agent.terminate()
        try:
            cls.agent.wait(timeout=20)
        except subprocess.TimeoutExpired:
            cls.agent.kill()
        cls.agent_log.close()
        cls.loop.stop.set()
        cls.server.shutdown()
        cls.server.server_close()
        cls.log.stream.close()
        for _ in range(20):
            try:
                cls.scratch.cleanup()
                break
            except OSError:
                time.sleep(0.25)

    def call(self, method, path, body=None, headers=None, raw=False):
        data = json.dumps(body).encode() if isinstance(body, dict) else body
        request = urllib.request.Request(self.base + path, data=data, method=method, headers=dict(
            {'Content-Type': 'application/json'}, **(headers or {})))
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = response.read()
                return response.status, (payload if raw else json.loads(payload or b'{}'))
        except urllib.error.HTTPError as error:
            payload = error.read()
            error.close()
            return error.code, json.loads(payload or b'{}') if not raw else payload

    def wait_for(self, number, states, timeout=90):
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, build = self.call('GET', '/api/builds/%d' % number)
            if build['state'] in states:
                return build
            time.sleep(0.25)
        agent_output = Path(self.scratch.name, 'agent.log').read_bytes().decode('utf-8', errors='replace')
        self.fail('build %d stayed %s: %s\nagent:\n%s' % (number, build['state'], json.dumps(
            [(j['key'], j['state'], j['reason']) for j in build['jobs']]), agent_output[-3000:]))

    def test_1_webhook_build_runs_end_to_end(self):
        body = json.dumps({'event': 'push', 'branch': 'main', 'commit': self.commit, 'message': 'e2e'}).encode()
        headers = {'X-Cerid-Signature': hosts.sign_generic('whsec-e2e', body), 'X-Cerid-Delivery': 'e2e-1'}
        status, answer = self.call('POST', '/webhooks/demo', body, headers)
        self.assertEqual((status, answer['created']), (200, True))
        self.assertEqual(self.call('POST', '/webhooks/demo', body, headers)[1], {'duplicate': True})
        status, _ = self.call('POST', '/webhooks/demo', body, {'X-Cerid-Signature': 't=1,v1=00'})
        self.assertEqual(status, 401)
        number = answer['build']
        build = self.wait_for(number, ('passed', 'failed', 'canceled'))
        states = {j['key']: (j['state'], j['soft_failed']) for j in build['jobs']}
        self.assertEqual(build['state'], 'passed', states)
        self.assertEqual(states, {'plan': ('passed', False), 'hello': ('passed', False), 'meta': ('passed', False),
                                  'soft': ('failed', True), 'dynamic': ('passed', False)})
        hello = next(j for j in build['jobs'] if j['key'] == 'hello')
        _, log = self.call('GET', '/api/jobs/%s/log' % hello['id'])
        self.assertFalse(log['live'])
        self.assertIn('\x1b[1;32mhello from the agent', log['text'])
        self.assertIn('secret is [REDACTED]', log['text'])
        self.assertIn('split [REDACTED]', log['text'], 'a secret split across two writes is still redacted')
        self.assertNotIn(SECRET, log['text'])
        raw = self.call('GET', '/api/jobs/%s/log?raw=1' % hello['id'], raw=True)[1]
        self.assertNotIn(SECRET.encode(), raw)
        everything = b''.join(self.store.get(n) or b'' for n in self.store.list(''))
        self.assertNotIn(SECRET.encode(), everything, 'the secret value reached the state store')
        _, data = self.call('GET', '/api/jobs/%s/artifacts?path=out/result.txt' % hello['id'], raw=True)
        self.assertEqual(data.decode(), 'artifact ' + self.commit)
        self.assertEqual(build['meta'], {'tier': 'change'})
        self.assertIn('| a | b |', build['annotations']['e2e']['body'])
        meta = next(j for j in build['jobs'] if j['key'] == 'meta')
        self.assertIn('artifact ' + self.commit, self.call('GET', '/api/jobs/%s/log' % meta['id'])[1]['text'])
        _, agents = self.call('GET', '/api/agents')
        self.assertEqual([a['name'] for a in agents['agents']], ['e2e-agent'])

    def test_2_mirror_checkout_and_cancellation(self):
        status, answer = self.call('POST', '/api/builds', {'repository': 'mirrored', 'branch': 'main',
                                                            'commit': self.commit},
                                   headers={'X-Cerid-Request': '1'})
        self.assertEqual(status, 200, answer)
        number = answer['build']
        deadline = time.time() + 60
        sleeper = None
        while time.time() < deadline:
            _, build = self.call('GET', '/api/builds/%d' % number)
            sleeper = next((j for j in build['jobs'] if j['key'] == 'sleep'), None)
            if sleeper and sleeper['state'] == 'running':
                _, log = self.call('GET', '/api/jobs/%s/log' % sleeper['id'])
                if 'sleeping' in log['text']:
                    break
            if build['state'] in ('passed', 'failed', 'canceled'):
                self.fail('the build ended before the long step ran: %s' % json.dumps(build['jobs'])[:2000])
            time.sleep(0.25)
        else:
            self.fail('the long step never started')
        _, log = self.call('GET', '/api/jobs/%s/log' % sleeper['id'])
        self.assertIn('checkout mirror at %s' % self.commit, log['text'])
        status, _ = self.call('POST', '/api/builds/%d/cancel' % number, {})
        self.assertEqual(status, 403, 'a state-changing call without X-Cerid-Request is refused')
        started = time.time()
        self.assertEqual(self.call('POST', '/api/builds/%d/cancel' % number, {}, {'X-Cerid-Request': '1'})[0], 200)
        build = self.wait_for(number, ('canceled',), timeout=30)
        self.assertLess(time.time() - started, 20, 'cancellation reached the agent through its heartbeat')
        self.assertEqual({j['key']: j['state'] for j in build['jobs']}, {'plan': 'passed', 'sleep': 'canceled'})

    def test_3_agent_api_refuses_bad_tokens_and_protocols(self):
        self.assertEqual(self.call('POST', '/api/agent/poll', {}, {'Authorization': 'Bearer cst_forged'})[0], 401)
        self.assertEqual(self.call('POST', '/api/agent/poll', {}, {'X-Cerid-Agent-Protocol': '99'})[0], 426)
        status, health = self.call('GET', '/healthz')
        self.assertEqual((status, health['ok']), (200, True))
        status, agent = self.call('GET', '/agent/cerid-agent.py', raw=True)
        self.assertEqual(status, 200)
        self.assertIn(b'Cerid CI agent', agent)


if __name__ == '__main__':
    unittest.main()
