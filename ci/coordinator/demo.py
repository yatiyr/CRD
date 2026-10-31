#!/usr/bin/env python3
"""A local Cerid CI with fixture data, for looking at the web UI and the CLI without any cloud (ADR-0135 §16).

  python ci/coordinator/demo.py serve [--port 8790] [--minutes 30]     then open http://127.0.0.1:8790/
  python ci/ops.py --store <the printed directory> build list           the same state through the CLI

The state lives in a temporary directory on the filesystem backend. The fixture: finished builds (passed, failed
with a compiler error and a failing test, a soft failure, cached and budget steps), one build that keeps running with a
live log, lane test runs with a proven flaky test and its quarantine, a bisection, the month's ledger, SLO samples,
audit entries, two agents, the QUAL results of ci/dashboard/fixtures/results.json, the diagnostics of the failed
sanitizer lane (a sanitizer report, a flight record, a Perfetto trace and a withheld crash dump) and gold images to
triage (an exact lavapipe difference, a perceptual L4 difference, a first WARP image, a pass within tolerance and a
rejected image with its finding). Every value is invented and
labelled as fixture data; nothing here is evidence. The server listens on loopback only and stops by itself.
"""
from __future__ import annotations

from pathlib import Path
import argparse
import datetime as dt
import hashlib
import json
import sys
import tempfile
import threading
import time

CI = Path(__file__).resolve().parents[1]
if str(CI) not in sys.path:
    sys.path.insert(0, str(CI))

from coordinator import auth, diagnostics, gold, image, main as coordinator_main, reports, scheduler  # noqa: E402
from coordinator import service  # noqa: E402

COMMITS = [hashlib.sha1(('cerid fixture commit %d' % i).encode('ascii')).hexdigest() for i in range(8)]
LANE = 'linux-gcc-debug@gce-linux-x64-intel'
FAILED_LOG = (
    '~~~ cerid-agent 1.0.0 on cerid-ci-demo-a1 (Linux x86_64), job lane-linux-gcc-debug attempt 1\n'
    '~~~ checkout mirror at %s (main)\n'
    '~~~ running lane-linux-gcc-debug\n'
    '--- Configure\n-- The CXX compiler identification is GNU 14.2.0\n-- Configuring done (4.1s)\n'
    '--- Build\n[1/812] Building CXX object engine/foundation/core/src/core.cpp.o\n'
    '[402/812] Building CXX object engine/numerics/hesap-fft/src/plan.cpp.o\n'
    '\x1b[1mengine/numerics/hesap-fft/src/plan.cpp:214:9:\x1b[0m \x1b[1;31merror:\x1b[0m '
    "\x1b[1m'twiddle_bank' was not declared in this scope\x1b[0m\n"
    '  214 |         twiddle_bank.prepare(n);\n      |         ^~~~~~~~~~~~\n'
    'ninja: build stopped: subcommand failed.\n'
    '~~~ exit status 1\n~~~ 3 artifacts uploaded\n')
TEST_LOG = (
    '~~~ running lane-linux-gcc-asan\n--- Test\n'
    '      Start 118: crd-hesap-fft-tests\n'
    '118/412 Test #118: crd-hesap-fft-tests ..............***Failed    3.21 sec\n'
    'tests/numerics/fft/plan_test.cpp:88: FAILED:\n  REQUIRE( max_error < 1e-12 )\nwith expansion:\n'
    '  3.4e-09 < 0.0\n'
    '\x1b[32m99% tests passed\x1b[0m, \x1b[31m1 tests failed\x1b[0m out of 412\n'
    'The following tests FAILED:\n\t118 - crd-hesap-fft-tests (Failed)\n~~~ exit status 8\n')


class Clock:
    def __init__(self, now):
        self.now = now

    def __call__(self):
        return self.now


def run_job(c, agent, log=b'', code=0, minutes=1.0, clock=None, artifacts=None):
    got = c.poll(agent)
    job_id, token = got['job']['id'], got['token']
    c.start(job_id, token)
    if log:
        c.append_log(job_id, token, got['job']['attempt'], 0, log)
    c.record_environment(job_id, token, {'commit': got['checkout']['commit'], 'machine': agent['name'],
                                         'platform': 'Linux-6.8-x86_64 (fixture)', 'cpus': 8, 'checkout': 'mirror',
                                         'tools': {'cmake': 'cmake version 4.0.3', 'gcc': 'gcc 14.2.0',
                                                   'git': 'git version 2.43.0'},
                                         'env': {'CERID_CI': '1', 'PATH': '/usr/local/bin:/usr/bin'},
                                         'image': 'cerid-ci-ubuntu-2404-x64-202610300300 (fixture)'})
    for path, data in (artifacts or {}).items():
        c.put_artifact(job_id, token, path, data, hashlib.sha256(data).hexdigest())
    if clock is not None:
        clock.now += minutes * 60
    c.finish(job_id, token, code)
    return got


SANITIZER = ('==4242==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010 (fixture)\n'
             'READ of size 8 at 0x602000000010 thread T0\n'
             '    #0 0x4f1a2b in crd::hesap::TwiddleBank::prepare(unsigned long) plan.cpp:214\n'
             '    #1 0x4f0c11 in crd::hesap::Plan::Plan(unsigned long) plan.cpp:96\n'
             'SUMMARY: AddressSanitizer: heap-use-after-free plan.cpp:214 in TwiddleBank::prepare\n')


def diagnostic_artifacts(commit):
    """The typed diagnostics a failed sanitizer lane uploads (fixture data)."""
    agent = diagnostics.agent_module()
    flight = agent.bundle_bytes({'schema': 1, 'flags': 0, 'created': 1}, [
        (1, 0, b'{"manifest": "fixture"}', 23), (3, agent.SECTION_ABSENT, b'', 0), (6, 0, b'flight events', 13)])
    base = 'build/linux-gcc-asan/diagnostics/'
    summary = {'schema': diagnostics.SCHEMA, 'preset': 'linux-gcc-asan', 'test_preset': 'linux-gcc-asan',
               'device': 'gce-linux-x64-intel', 'commit': commit, 'phase': 'test', 'cpu_level': 'native',
               'failed_tests': ['crd-hesap-fft-tests'], 'privacy': {'raw_dumps': False, 'secret_values_scrubbed': 0},
               'items': [{'kind': 'crash-dump', 'source': 'tests/numerics/crash_4242.dmp', 'size': 1048576,
                          'sha256': '0' * 64, 'withheld': 'raw memory is restricted (fixture)'}]}
    return {base + 'summary.json': json.dumps(summary, indent=1).encode('utf-8'),
            base + 'sanitizer-report/crd-hesap-fft-tests-1-AddressSanitizer.txt': SANITIZER.encode('utf-8'),
            base + 'flight-record/tests__numerics__flight-1.cdb': flight,
            base + 'perfetto-trace/tests__numerics__fft.perfetto.json': b'{"traceEvents": []}'}


def render(width=160, height=120, shift=0, tint=(255, 140, 40), shadow=(30, 30, 46), noise=0, seed=3, sky=False,
           banding=False):
    """A small synthetic rendering for the gold fixture: a sky gradient, a lit triangle and its shadow."""
    import random
    rnd = random.Random(seed)
    out = bytearray()
    a, b, c3 = (40 + shift, 96), (120 + shift, 96), (80 + shift, 22)

    def inside(x, y):
        def edge(p, q):
            return (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0])
        e1, e2, e3 = edge(a, b), edge(b, c3), edge(c3, a)
        return (e1 >= 0 and e2 >= 0 and e3 >= 0) or (e1 <= 0 and e2 <= 0 and e3 <= 0)
    for y in range(height):
        for x in range(width):
            t = y / (height - 1)
            if banding:
                t = round(t * 6) / 6
            pixel = [int(20 + 60 * t), int(40 + 80 * t), int(90 + 120 * t)] if sky or banding else [24, 26, 34]
            if y > 98 and 46 + shift <= x <= 134 + shift and abs(y - 104) < 4:
                pixel = list(shadow)
            if inside(x, y):
                light = 0.55 + 0.45 * (1 - (y - 22) / 74)
                pixel = [int(v * light) for v in tint]
            if noise:
                pixel = [min(255, max(0, v + rnd.randint(-noise, noise))) for v in pixel]
            out += bytes(pixel + [255])
    return image.Image(width, height, out)


def seed_gold(c, registry):
    """Gold results of the fixture builds 102 (lavapipe, L4) and 103 (WARP), through the real comparison."""
    policy = gold.load_policy()
    pictures = {}

    def job(number, device, test_images, approve=(), reject=()):
        job_id = hashlib.sha256(('gold fixture %d %s' % (number, device)).encode('ascii')).hexdigest()[:32]
        record = {'build': number, 'id': job_id, 'attempt': 1, 'key': 'lane@' + device,
                  'env': {'CERID_CI_DEVICE': device}}
        manifest = {'schema': gold.SCHEMA, 'images': [{'test': t, 'file': '%s.png' % t, 'api': api}
                                                       for t, (api, _) in test_images.items()]}
        for test, (_, picture) in test_images.items():
            pictures['b/gold/%s.png' % test] = image.encode_png(picture)
        name = gold.register_manifest(c, record, {'path': 'b/gold/manifest.json'}, json.dumps(manifest).encode())
        gold.process(c, name, registry, policy, lambda rec, path: pictures[path])
        for test in approve + reject:
            entry = next(e for e in c.get(name)['entries'] if e['test'] == test)
            gold.triage(c, entry['grouping'], entry['digest'], 'untriaged', 'positive' if test in approve else
                        'negative', 'fixture@example.invalid', 'fixture baseline' if test in approve else
                        'banding in the sky gradient (fixture)')
    job(101, 'gce-linux-x64-intel', {'render.triangle': ('Vulkan', render())}, approve=('render.triangle',))
    job(101, 'gce-linux-x64-nvidia-l4', {'render.shadows': ('Vulkan', render(sky=True)),
                                         'render.ui-text': ('Vulkan', render(tint=(220, 220, 230))),
                                         'render.sky': ('Vulkan', render(banding=True))},
        approve=('render.shadows', 'render.ui-text'), reject=('render.sky',))
    job(102, 'gce-linux-x64-intel', {'render.triangle': ('Vulkan', render(shift=1))})
    job(102, 'gce-linux-x64-nvidia-l4', {'render.shadows': ('Vulkan', render(sky=True, shift=6, shadow=(10, 10, 16))),
                                         'render.ui-text': ('Vulkan', render(tint=(220, 220, 230), noise=1))})
    job(103, 'gce-windows-x64', {'app-launch.menu': ('DX12', render(tint=(60, 200, 120), sky=True))})


def seed(store, now=None):
    """Fill `store` with the fixture; returns the coordinator (its clock is real time from here on)."""
    now = now or time.time()
    clock = Clock(now - 3 * 86400)
    config = {'url': '', 'repositories': {'cerid': {'url': 'https://example.invalid/cerid.git',
                                                    'trusted_branches': ['main']}},
              'notifications': {'channels': []}}
    c = scheduler.Coordinator(store, config, clock=clock, log=scheduler.JsonLog(open(__import__('os').devnull, 'w')))
    c.ensure_schema()
    launcher = c.register(c.create_registration('launcher', 'cerid-launcher', by='fixture'),
                          {'name': 'launcher', 'protocol': 1, 'os': 'linux', 'arch': 'x86_64'})[0]
    owned = c.register(c.create_registration('owned-windows-x64-workstation', 'cerid-owned', {
        'device': 'owned-windows-x64-workstation'}, by='fixture'), {'name': 'owned-windows-x64-workstation',
                                                                     'protocol': 1, 'os': 'windows', 'arch': 'x86_64'})[0]
    cloud = c.register(c.create_registration('cloud', 'cerid-cloud', {'device': 'gce-linux-x64-intel'}, by='fixture'),
                       {'name': 'cerid-ci-demo-a1', 'protocol': 1, 'os': 'linux', 'arch': 'x86_64'})[0]
    lanes = [
        {'key': 'repository-linux', 'label': ':broom: Repository checks (linux)', 'command': 'python ci/pipeline.py run '
         'repository', 'agents': {'queue': 'cerid-cloud', 'device': 'gce-linux-x64-intel'}},
        {'key': 'lane-linux-gcc-debug', 'label': ':hammer: linux-gcc-debug', 'command': 'python ci/pipeline.py lane '
         '--preset linux-gcc-debug --device gce-linux-x64-intel', 'agents': {'queue': 'cerid-cloud',
                                                                             'device': 'gce-linux-x64-intel'}},
        {'key': 'lane-linux-gcc-asan', 'label': ':hammer: linux-gcc-asan', 'command': 'python ci/pipeline.py lane '
         '--preset linux-gcc-asan --device gce-linux-x64-intel', 'agents': {'queue': 'cerid-cloud',
                                                                            'device': 'gce-linux-x64-intel'}},
        {'key': 'lane-win-debug', 'label': ':hammer: win-debug', 'command': 'python ci/pipeline.py lane --preset '
         'win-debug --device gce-windows-x64', 'skip': {'state': 'cached', 'reason': 'cached: passed in build 2'}},
        {'key': 'lane-win-release@owned-windows-x64-workstation', 'label': ':gpu: win-release on the workstation',
         'command': 'python ci/pipeline.py lane --preset win-release --device owned-windows-x64-workstation',
         'agents': {'queue': 'cerid-owned', 'device': 'owned-windows-x64-workstation'}},
        {'key': 'lane-linux-gcc-release@gce-linux-x64-nvidia-l4', 'label': ':gpu: linux-gcc-release on an L4',
         'command': 'python ci/pipeline.py lane', 'skip': {'state': 'budget', 'reason': 'budget: 49.20 of 50.00 USD'}},
        'wait',
        {'key': 'required', 'label': ':white_check_mark: Required result', 'command': 'python ci/pipeline.py conclude',
         'agents': {'queue': 'cerid-launcher'}, 'allow_dependency_failure': True,
         'soft_fail': [{'exit_status': 3}]}]
    stories = [('main', COMMITS[0], 'hesap-fft: fold the radix-8 twiddles', {}),
               ('main', COMMITS[1], 'ci: record test durations per lane', {}),
               ('main', COMMITS[2], 'hesap-fft: share the twiddle bank across plans',
                {'lane-linux-gcc-debug': (FAILED_LOG % COMMITS[2], 1), 'lane-linux-gcc-asan': (TEST_LOG, 8)}),
               ('feature/sse2-floor', COMMITS[3], 'simd: keep the SSE2 floor in the dispatcher', {})]
    diagnosed = None
    for branch, commit, message, failures in stories:
        build, _ = c.create_build('cerid', {'event': 'push', 'branch': branch, 'commit': commit, 'message': message,
                                            'trusted': branch == 'main'}, 'poll')
        run_job(c, launcher, b'~~~ running plan\nplanned 7 steps\n~~~ exit status 0\n', clock=clock)
        c.upload(build['number'], {'steps': lanes})
        for key in ('repository-linux', 'lane-linux-gcc-debug', 'lane-linux-gcc-asan'):
            log, code = failures.get(key, ('~~~ running %s\n--- Build\n[812/812] Linking\n--- Test\n100%% tests '
                                           'passed, 0 tests failed out of 412\n~~~ exit status 0\n' % key, 0))
            extra = diagnostic_artifacts(commit) if key == 'lane-linux-gcc-asan' and code else None
            got = run_job(c, cloud, log.encode('utf-8'), code, minutes=40, clock=clock, artifacts=extra)
            if extra:
                diagnosed = got['job']['id']
        run_job(c, owned, b'~~~ running on the workstation\n100% tests passed\n~~~ exit status 0\n', minutes=30,
                clock=clock)
        run_job(c, launcher, b'Required result\n', 3 if failures else 0, clock=clock)
        clock.now += 6 * 3600
    # Lane test runs: a flaky test (same key passed and failed), a regression on the newest run.
    for i, build in enumerate((1, 2, 3)):
        tests = {'crd-core-tests': ('pass', 'k-core'), 'crd-jobs-tests': ('fail' if i == 1 else 'pass', 'k-jobs'),
                 'crd-hesap-fft-tests': ('fail' if i == 2 else 'pass', 'k-fft-%d' % (i // 2))}
        store.put_json('test-runs/%s/%010d.json' % (LANE, build), {
            'build': str(build), 'commit': COMMITS[i], 'branch': 'main', 'repository': 'cerid', 'lane': LANE,
            'tests': {name: {'outcome': o, 'seconds': 1.5 + i * 0.3 + (8.0 if 'fft' in name else 0.0), 'key': k}
                      for name, (o, k) in tests.items()}})
    from coordinator import quality
    quality.scan(c)
    quality.open_bisections(c)
    # Ledger, machines, SLO samples, QUAL results.
    month = c.p.month_of(dt.datetime.fromtimestamp(now, dt.timezone.utc))
    for i, (build, key, device, usd) in enumerate(((1, 'lane-linux-gcc-debug', 'gce-linux-x64-intel', 0.42),
                                                   (2, 'lane-linux-gcc-asan', 'gce-linux-x64-intel', 0.61),
                                                   (3, 'lane-win-debug', 'gce-windows-x64', 2.84),
                                                   (3, 'console-gce-console-profile-xbox', 'gce-console-profile-xbox',
                                                    1.90))):
        store.put_json('ledger/%s/fixture-%d.json' % (month, i), {'build': build, 'key': key, 'device': device,
                                                                  'usd': usd, 'hours': usd / 0.3})
    fixtures = json.loads((CI / 'dashboard/fixtures/results.json').read_text(encoding='utf-8'))
    for record in fixtures['results']:
        if record['outcome'] == 'fail' and record['step'] == 'lane-linux-gcc-asan':
            record = dict(record, job_id=diagnosed, failed_phase='test', diagnostics=[
                'flight-record', 'perfetto-trace', 'sanitizer-report', 'summary'])
        store.put_json('results/%s/%s.json' % (record['build'], record['step']), record)
    seed_gold(c, c.p.load_registry())
    for minute in range(0, 3 * 1440, 7):
        reports.record_tick(c, now - 3 * 86400 + minute * 60)
    c.clock = time.time
    return c


def live_build(c, stop):
    """A build that keeps running: its step writes a log line a second until `stop` is set."""
    worker = c.register(c.create_registration('live', 'cerid-live', by='fixture'),
                        {'name': 'cerid-ci-demo-live', 'protocol': 1, 'os': 'linux', 'arch': 'x86_64'})[0]
    build, _ = c.create_build('cerid', {'event': 'push', 'branch': 'main', 'commit': COMMITS[4], 'trusted': True,
                                        'message': 'diag: live build of the fixture'}, 'manual', created_by='fixture',
                              document={'steps': [{'key': 'lane-linux-clang-tsan', 'label': ':hammer: '
                                                   'linux-clang-tsan', 'command': 'python ci/pipeline.py lane',
                                                   'agents': {'queue': 'cerid-live'}, 'timeout_in_minutes': 600},
                                                  {'key': 'required', 'label': ':white_check_mark: Required result',
                                                   'command': 'conclude', 'agents': {'queue': 'cerid-live'},
                                                   'depends_on': 'lane-linux-clang-tsan'}]})
    got = c.poll(worker)
    c.start(got['job']['id'], got['token'])
    seq = 0
    while not stop.wait(1.0):
        c.append_log(got['job']['id'], got['token'], 1, seq, ('[%d/920] Building CXX object engine/jobs/src/part_%d.cpp.o'
                                                               '\n' % (seq + 1, seq)).encode('utf-8'))
        c.heartbeat(got['job']['id'], got['token'])
        seq += 1
    return build


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    serve = sub.add_parser('serve')
    serve.add_argument('--port', type=int, default=8790)
    serve.add_argument('--minutes', type=float, default=30.0)
    serve.add_argument('--store', help='a directory to keep (default: a new temporary one)')
    args = parser.parse_args(argv)
    root = Path(args.store or tempfile.mkdtemp(prefix='cerid-ci-demo-'))
    store = scheduler.pipeline_module().LocalStore(root)
    c = seed(store)
    registry = c.p.load_registry()
    web = service.Service(c, auth.UiAuth('local', loopback=True), {}, registry=registry,
                          qual_data=coordinator_main.qual_renderer(store, registry, c),
                          dashboard=coordinator_main.dashboard_renderer(store, registry, c))
    server = service.serve(web, '127.0.0.1', args.port)
    stop = threading.Event()
    threading.Thread(target=live_build, args=(c, stop), daemon=True).start()
    loop = service.Loop(c, interval=2.0)
    loop.start()
    threading.Timer(args.minutes * 60, server.shutdown).start()
    print('Cerid CI demo (fixture data) at http://127.0.0.1:%d/  state: %s  stops in %g minutes' % (
        server.server_address[1], root, args.minutes), flush=True)
    try:
        server.serve_forever()
    finally:
        stop.set()
        loop.stop.set()
        server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
