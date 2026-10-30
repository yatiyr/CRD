#!/usr/bin/env python3
"""Contracts of the Cerid CI platform (ADR-0135; ci/): the device registry, affected selection, content keys, the
result store, the budget, the plan and its Buildkite document, the lanes against the hosted jobs they replace (the
CI-4 parity table), the four executors, the dispatcher, the console profile and the QUAL dashboard. No build, no
cloud, no account: every external service is a fake. Run: python scripts/test-ci-platform.py (stdlib unittest).
"""
from pathlib import Path
import base64
import contextlib
import datetime as dt
import importlib.util
import io
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

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
ops = load('ci_ops', 'ci/ops.py')
dashboard = load('ci_dashboard', 'ci/dashboard/generate.py')
master = load('check_master_plan', 'scripts/check-master-plan.py')
evidence = load('ci_evidence_platform', 'scripts/ci-evidence.py')
hygiene = load('hygiene_platform', 'scripts/check-repository.py')
import executors  # noqa: E402
from executors import firebase_test_lab, gce_vm, owned_agent, ssh_host  # noqa: E402

TIERS = pipeline.ci_tier.load_tiers(pipeline.TIERS)
REGISTRY = pipeline.load_registry(packs=[])
PRESETS = [p['name'] for p in json.loads((ROOT / 'CMakePresets.json').read_text(encoding='utf-8'))['configurePresets']
           if not p.get('hidden')]
ROWS = set(pipeline.roadmap_ids())
WORKFLOW = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')
ALL_ACCOUNTS = {'CERID_BUILDKITE_ORG': 'org', 'CERID_CI_GCP_PROJECT': 'cerid-ci', 'CERID_CI_BUDGET_SUBSCRIPTION': 's',
                'CERID_CI_RUNNER_SERVICE_ACCOUNT': 'runner@cerid-ci', 'CERID_CI_STORE': 'x', 'CERID_FTL_PROJECT': 'f',
                'CERID_CLOUDFLARE_ACCOUNT_ID': 'a', 'CERID_SSH_AMD_DEVCLOUD_HOST': 'h', 'CERID_SSH_MAC_HOST': 'h',
                'CERID_SSH_WINDOWS_ARM64_HOST': 'h', 'CERID_SSH_INTEL_TIBER_HOST': 'h', 'CERID_CI_OWNED_AGENTS': '1'}
SECRETS = {'buildkite-agent-token', 'cloudflare-pages-token'}


def fake_entries(extra=()):
    entries = [('CMakeLists.txt', 'a1'), ('cmake/pins.json', 'a2'), ('docs/ROADMAP.md', 'a3'),
               ('engine/foundation/core/src/core.cpp', 'b1'), ('engine/foundation/math/src/math.cpp', 'b2'),
               ('engine/numerics/fft/src/fft.cpp', 'b3'), ('tests/numerics/fft/test.cpp', 'b4'),
               ('engine/gpu/vulkan/src/vk.cpp', 'b5')]
    return sorted(dict(entries + list(extra)).items())


FIXTURE_CMAKE = '''
# crd_module(commented/out ignored)
crd_module(engine/foundation/core engine/core TESTS foundation/core)
crd_module(engine/foundation/math engine/math DEPENDS core TESTS foundation/math)
crd_module(engine/numerics/fft engine/fft DEPENDS core math PACKAGES zstd TESTS numerics/fft TEST_DEPENDS bench)
crd_module(engine/numerics/bench engine/bench DEPENDS core)
crd_module(engine/gpu/vulkan engine/vulkan DEPENDS core PACKAGES glfw)
crd_module(tools/shader-cook NAME shader_cook HOST EXECUTABLES cook DEPENDS core)
'''
MODULES = pipeline.parse_modules(FIXTURE_CMAKE)


class Registry(unittest.TestCase):
    def test_registry_is_valid_and_covers_the_matrix(self):
        self.assertEqual(pipeline.validate_registry(REGISTRY, TIERS, PRESETS, ROWS), [])

    def test_every_ci_tiers_job_has_one_host(self):
        hosts = pipeline.job_hosts(REGISTRY)
        self.assertEqual(sorted(hosts), sorted(TIERS['jobs']))

    def test_missing_host_kind_screen_and_coverage_are_refused(self):
        broken = json.loads(json.dumps(REGISTRY))
        devices = {d['id']: d for d in broken['devices']}
        devices['gce-linux-x64-intel']['ci_jobs'].remove('linux-gcc')
        devices['ftl-pixel-phone'].pop('screen')
        devices['gce-linux-arm64-ampere']['kind'] = 'mainframe'
        broken['devices'] = [d for d in broken['devices'] if d['gpu']['vendor'] != 'Imagination PowerVR']
        problems = '\n'.join(pipeline.validate_registry(broken, TIERS, PRESETS, ROWS))
        self.assertIn('ci-tiers job linux-gcc must be hosted by exactly one device', problems)
        self.assertIn('ftl-pixel-phone: a phone cell names its screen', problems)
        self.assertIn("kind 'mainframe'", problems)
        self.assertIn('no cell has a Imagination PowerVR GPU', problems)

    def test_unknown_roadmap_row_is_refused(self):
        broken = json.loads(json.dumps(REGISTRY))
        broken['features']['vulkan']['rows'].append('NO-SUCH-ROW')
        self.assertIn('feature vulkan names roadmap row NO-SUCH-ROW, which does not exist',
                      pipeline.validate_registry(broken, TIERS, PRESETS, ROWS))

    def test_console_cells_wait_on_their_platform_programs(self):
        consoles = [d for d in REGISTRY['devices'] if d['kind'] == 'console']
        self.assertEqual(sorted(d['id'] for d in consoles), ['nintendo-switch-2-devkit', 'playstation-5-devkit',
                                                              'playstation-5-testkit', 'xbox-series-devmode',
                                                              'xbox-series-gdk-devkit'])
        for device in consoles:
            self.assertEqual((device['status'], device['executor'], device['tier']),
                             ('waiting on platform program', 'owned_agent', 'C'))
            self.assertIn(device['accounts'][0], ('xbox-partner-center', 'playstation-partners',
                                                  'nintendo-developer-portal'))
        broken = json.loads(json.dumps(REGISTRY))
        next(d for d in broken['devices'] if d['id'] == 'ps5-devkit' or d['id'] == 'playstation-5-devkit')[
            'status'] = 'active'
        self.assertTrue(any('runs only through its private platform pack' in p
                            for p in pipeline.validate_registry(broken, TIERS, PRESETS, ROWS)))

    def test_engine_enforced_console_features_name_their_option_and_row(self):
        for name, feature in REGISTRY['features'].items():
            if feature.get('enforced_by') == 'engine':
                self.assertRegex(feature['option'], r'CRD_[A-Z0-9_]+')
                self.assertTrue(set(feature['rows']) <= ROWS, name)

    def test_platform_pack_extends_the_registry_and_never_replaces_an_entry(self):
        with tempfile.TemporaryDirectory() as scratch:
            pack = Path(scratch) / 'xbox'
            pack.mkdir()
            device = json.loads(json.dumps(next(d for d in REGISTRY['devices'] if d['id'] == 'xbox-series-devmode')))
            device['id'] = 'xbox-series-x-pack-kit'
            (pack / 'devices.json').write_text(json.dumps({'schema': pipeline.SCHEMA, 'devices': [device]}),
                                               encoding='utf-8')
            merged = pipeline.load_registry(packs=[pack])
            self.assertEqual(next(d for d in merged['devices'] if d['id'] == 'xbox-series-x-pack-kit')['pack'], 'xbox')
            device['id'] = 'xbox-series-devmode'
            (pack / 'devices.json').write_text(json.dumps({'schema': pipeline.SCHEMA, 'devices': [device]}),
                                               encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'collides'):
                pipeline.load_registry(packs=[pack])

    def test_a_pack_inside_the_tracked_tree_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'inside the tracked tree'):
            pipeline.pack_directories({'CERID_PLATFORM_PACKS': str(ROOT / 'scripts')})
        self.assertEqual(pipeline.pack_directories({'CERID_PLATFORM_PACKS': str(ROOT / 'external/none')}),
                         [(ROOT / 'external/none').resolve()])


class Graph(unittest.TestCase):
    def test_parser_reads_the_real_registry(self):
        text = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertEqual(set(pipeline.parse_modules(text)), master.registered_modules(text))

    def test_fixture_fields(self):
        self.assertNotIn('ignored', MODULES)
        self.assertEqual(MODULES['fft']['depends'], ['core', 'math'])
        self.assertEqual(MODULES['fft']['test_depends'], ['bench'])
        self.assertIn('shader_cook', MODULES)

    def test_a_core_change_reaches_its_users(self):
        selection, reasons = pipeline.affected_modules(['engine/foundation/core/src/core.cpp'], MODULES, TIERS)
        self.assertEqual(reasons, [])
        self.assertEqual(selection, ['bench', 'core', 'fft', 'math', 'shader_cook', 'vulkan'])

    def test_a_test_dependency_change_reaches_the_tests_that_use_it(self):
        selection, _ = pipeline.affected_modules(['engine/numerics/bench/b.cpp'], MODULES, TIERS)
        self.assertEqual(selection, ['bench', 'fft'])
        selection, _ = pipeline.affected_modules(['tests/numerics/fft/t.cpp'], MODULES, TIERS)
        self.assertEqual(selection, ['fft'])

    def test_broader_paths_build_everything(self):
        for path in ('cmake/CrdModules.cmake', 'assets/scene.ceir', 'engine/rendering/unknown/x.cpp',
                     'tests/CMakeLists.txt', 'ci/pipeline.py'):
            selection, reasons = pipeline.affected_modules([path], MODULES, TIERS)
            self.assertIsNone(selection, path)
            self.assertTrue(reasons, path)

    def test_documentation_selects_nothing(self):
        self.assertEqual(pipeline.affected_modules(['docs/ROADMAP.md', 'README.md'], MODULES, TIERS), ([], []))


class Keys(unittest.TestCase):
    def key(self, entries, selection=None, kind='lane', include_docs=False):
        return pipeline.input_key(entries, TIERS, kind, {'preset': 'p'}, modules=MODULES, selection=selection,
                                  include_docs=include_docs)

    def test_documentation_never_enters_a_build_key(self):
        base = fake_entries()
        docs = fake_entries([('docs/ROADMAP.md', 'zz')])
        self.assertEqual(self.key(base), self.key(docs))
        self.assertNotEqual(self.key(base, kind='repository', include_docs=True),
                            self.key(docs, kind='repository', include_docs=True))

    def test_a_narrowed_key_ignores_modules_outside_its_closure(self):
        base = fake_entries()
        vulkan = fake_entries([('engine/gpu/vulkan/src/vk.cpp', 'changed')])
        fft = fake_entries([('engine/numerics/fft/src/fft.cpp', 'changed')])
        selection = ['fft']
        self.assertEqual(self.key(base, selection), self.key(vulkan, selection))
        self.assertNotEqual(self.key(base, selection), self.key(fft, selection))
        self.assertNotEqual(self.key(base), self.key(vulkan))

    def test_global_inputs_always_count(self):
        self.assertNotEqual(self.key(fake_entries(), ['fft']),
                            self.key(fake_entries([('cmake/pins.json', 'new')]), ['fft']))

    def test_test_keys_follow_binary_device_and_runtime(self):
        inventory = {'tests': [{'name': 'a', 'command': ['x.exe', '[a]'], 'properties': []},
                               {'name': 'b', 'command': ['x.exe', '[b]'],
                                'properties': [{'name': 'ENVIRONMENT', 'value': ['K=1']}]}]}
        device = REGISTRY['devices'][0]
        one = pipeline.test_keys(inventory, device, 'r', read=lambda p: b'binary-1')
        two = pipeline.test_keys(inventory, device, 'r', read=lambda p: b'binary-2')
        other = pipeline.test_keys(inventory, REGISTRY['devices'][1], 'r', read=lambda p: b'binary-1')
        runtime = pipeline.test_keys(inventory, device, 's', read=lambda p: b'binary-1')
        self.assertNotEqual(one['a'], one['b'])
        for changed in (two, other, runtime):
            self.assertNotEqual(one['a'], changed['a'])


class Store(unittest.TestCase):
    def test_local_store_round_trip_and_containment(self):
        with tempfile.TemporaryDirectory() as scratch:
            store = pipeline.LocalStore(scratch)
            store.put_json('results/1/a.json', {'x': 1})
            store.put_json('results/2/b.json', {'x': 2})
            self.assertEqual(store.get_json('results/1/a.json'), {'x': 1})
            self.assertEqual(store.list('results/'), ['results/1/a.json', 'results/2/b.json'])
            self.assertIsNone(store.get('nothing.json'))
            with self.assertRaises(ValueError):
                store.put('../escape.json', b'x')

    def test_gcs_store_speaks_the_json_api(self):
        calls = []

        def http(method, url, token=None, raw=False, allow=(), data=None, content_type=None, **_):
            calls.append((method, url))
            if method == 'GET' and 'alt=media' in url:
                return (404, b'') if 'missing' in url else (200, b'{"x": 3}')
            if method == 'GET':
                return 200, {'items': [{'name': 'pre/results/1.json'}]}
            return 200, {}

        store = pipeline.GcsStore('bucket', 'pre', token=lambda: 't', http=http)
        self.assertEqual(store.get_json('results/1.json'), {'x': 3})
        self.assertIsNone(store.get('missing.json'))
        store.put_json('cache/k.json', {'outcome': 'pass'})
        self.assertEqual(store.list('results/'), ['results/1.json'])
        self.assertIn('name=pre%2Fcache%2Fk.json', calls[2][1])

    def test_record_result_writes_cache_only_on_pass(self):
        with tempfile.TemporaryDirectory() as scratch:
            store = pipeline.LocalStore(scratch)
            pipeline.record_result(store, {'step': 's', 'build': '9', 'outcome': 'fail', 'input_key': 'k1',
                                           'minutes': 3})
            self.assertIsNone(pipeline.cached_pass(store, 'k1'))
            pipeline.record_result(store, {'step': 's', 'build': '10', 'outcome': 'pass', 'input_key': 'k1',
                                           'minutes': 5})
            self.assertEqual(pipeline.cached_pass(store, 'k1')['build'], '10')
            self.assertEqual(pipeline.estimate_minutes(store, 's', 99), 5.0)


class Budget(unittest.TestCase):
    def steps(self):
        return [{'key': k, 'state': 'run', 'priority': p, 'order': i, 'est_usd': c, 'reason': ''}
                for i, (k, p, c) in enumerate([('free', 0, 0.0), ('change', 1, 3.0), ('complete', 2, 4.0),
                                               ('gpu', 3, 2.0)])]

    def test_cap_marks_the_lowest_priority_paid_steps(self):
        steps = self.steps()
        pipeline.apply_budget(steps, cap=50.0, spent=44.0)
        self.assertEqual([s['state'] for s in steps], ['run', 'run', 'budget', 'run'])
        self.assertTrue(steps[2]['reason'].startswith('budget:'))

    def test_gcp_stop_stops_every_paid_step(self):
        steps = self.steps()
        pipeline.apply_budget(steps, cap=50.0, spent=0.0, stop=True)
        self.assertEqual([s['state'] for s in steps], ['run', 'budget', 'budget', 'budget'])

    def test_spend_is_the_ledger_and_the_gcp_notification_decides_the_stop(self):
        with tempfile.TemporaryDirectory() as scratch:
            store = pipeline.LocalStore(scratch)
            store.put_json('ledger/2026-10/a.json', {'usd': 1.25})
            store.put_json('ledger/2026-10/b.json', {'usd': 2.5})
            store.put_json('ledger/2026-09/c.json', {'usd': 99})
            self.assertEqual(pipeline.month_spend(store, '2026-10'), 3.75)
            self.assertFalse(pipeline.gcp_budget_stop(store, '2026-10'))
            store.put_json('budget/gcp-2026-10.json', {'costAmount': 50.2, 'budgetAmount': 50.0})
            self.assertTrue(pipeline.gcp_budget_stop(store, '2026-10'))

    def test_budget_notifications_are_pulled_and_acknowledged(self):
        note = {'costAmount': 51.0, 'budgetAmount': 50.0, 'costIntervalStart': '2026-10-01T07:00:00Z'}
        calls = []

        def http(method, url, token=None, body=None, **_):
            calls.append((url, body))
            if url.endswith(':pull'):
                return 200, {'receivedMessages': [{'ackId': 'a1', 'message': {
                    'data': base64.b64encode(json.dumps(note).encode()).decode()}}]}
            return 200, {}

        with tempfile.TemporaryDirectory() as scratch:
            store = pipeline.LocalStore(scratch)
            pipeline.pull_budget(store, 'projects/cerid-ci/subscriptions/budget', http=http, token=lambda: 't')
            self.assertTrue(pipeline.gcp_budget_stop(store, '2026-10'))
            self.assertEqual(calls[-1][1], {'ackIds': ['a1']})


class Plan(unittest.TestCase):
    def plan(self, event='push', paths=None, requested=None, store=None, environ=None, registry=None, fresh=False):
        store = store or pipeline.LocalStore(tempfile.mkdtemp(prefix='cerid-plan-'))
        return pipeline.plan(event, 'base', 'head', requested, paths=paths, registry=registry or REGISTRY,
                             tiers=TIERS, modules=pipeline.parse_modules(
                                 (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')),
                             entries=fake_entries(), store=store, environ=ALL_ACCOUNTS if environ is None else environ,
                             secrets=SECRETS, fresh=fresh, month='2026-10')

    def test_documentation_push_runs_only_the_repository_checks(self):
        planned = self.plan(paths=['docs/ROADMAP.md'])
        self.assertEqual(planned['tier'], 'preflight')
        self.assertEqual({s['kind'] for s in planned['steps']}, {'repository'})

    def test_source_push_narrows_lanes_but_never_the_full_scope_presets(self):
        planned = self.plan(paths=['engine/numerics/hesap-fft/src/plan.cpp'])
        self.assertEqual(planned['tier'], 'change')
        lanes = {s['preset']: s for s in planned['steps'] if s['kind'] == 'lane'}
        self.assertIn('hesap-fft', planned['selection'])
        self.assertIn('--modules', lanes['win-release']['command'])
        for preset in ('win-debug', 'win-asan', 'linux-gcc-debug', 'linux-gcc-asan'):
            self.assertNotIn('--modules', lanes[preset]['command'], preset)
        self.assertEqual(set(lanes), {p for p, e in TIERS['presets'].items() if e['tier'] == 'change'})

    def test_complete_tier_adds_device_matrix_and_console_profiles(self):
        planned = self.plan(event='schedule')
        keys = {s['key'] for s in planned['steps']}
        self.assertIn('lane-linux-gcc-release@gce-linux-x64-nvidia-l4', keys)
        self.assertIn('console-gce-console-profile-xbox', keys)
        self.assertIn('console-gce-console-profile-playstation', keys)
        console = next(s for s in planned['steps'] if s['key'] == 'console-gce-console-profile-xbox')
        self.assertIn('--console xbox-series-x', console['command'])
        self.assertNotIn('console-aot', console['features'])
        waiting = {(c['device'], f) for c in planned['cells'] for f in c['features']}
        self.assertIn(('gce-console-profile-xbox', 'console-aot'), waiting)
        self.assertIn(('xbox-series-gdk-devkit', 'console-platform'), waiting)

    def test_missing_accounts_turn_cells_into_waiting(self):
        planned = self.plan(event='schedule', environ={})
        self.assertEqual(planned['steps'], [])
        self.assertTrue(all(c['state'] == 'waiting' for c in planned['cells']))

    def test_budget_and_cache_states(self):
        store = pipeline.LocalStore(tempfile.mkdtemp(prefix='cerid-plan-'))
        store.put_json('ledger/2026-10/x.json', {'usd': 49.5})
        planned = self.plan(event='schedule', store=store)
        paid = [s for s in planned['steps'] if s['est_usd'] > 0]
        self.assertTrue(paid and all(s['state'] == 'budget' for s in paid if s['est_usd'] > 0.5))
        lane = next(s for s in planned['steps'] if s['key'] == 'lane-win-debug')
        store2 = pipeline.LocalStore(tempfile.mkdtemp(prefix='cerid-plan-'))
        store2.put_json('cache/%s.json' % lane['input_key'], {'outcome': 'pass', 'build': '41'})
        cached = next(s for s in self.plan(event='schedule', store=store2)['steps'] if s['key'] == 'lane-win-debug')
        self.assertEqual(cached['state'], 'cached')
        fresh = next(s for s in self.plan(event='schedule', store=store2, fresh=True)['steps']
                     if s['key'] == 'lane-win-debug')
        self.assertEqual(fresh['state'], 'run')

    def test_buildkite_document(self):
        store = pipeline.LocalStore(tempfile.mkdtemp(prefix='cerid-plan-'))
        store.put_json('ledger/2026-10/x.json', {'usd': 49.9})
        planned = self.plan(event='schedule', store=store)
        document = pipeline.buildkite_pipeline(planned, REGISTRY['queues'])
        json.dumps(document)
        steps = {s['key']: s for s in document['steps']}
        required = steps['required']
        self.assertEqual({d['step'] for d in required['depends_on']},
                         {s['key'] for s in planned['steps']})
        for step in planned['steps']:
            entry = steps[step['key']]
            self.assertEqual(entry['agents']['device'], step['device'])
            self.assertEqual(entry['retry'], pipeline.RETRY)
            if step['state'] in ('budget', 'cached'):
                self.assertLessEqual(len(entry['skip']), 70)
                self.assertTrue(entry['skip'].startswith(step['state']))
            else:
                self.assertNotIn('skip', entry)
        owned = [s for s in planned['steps'] if REGISTRY and pipeline.devices_by_id(REGISTRY)[s['device']][
            'executor'] == 'owned_agent']
        for step in owned:
            self.assertEqual(steps[step['key']]['agents']['queue'], 'cerid-owned')

    def test_buildkite_events(self):
        self.assertEqual(pipeline.buildkite_event({'BUILDKITE_SOURCE': 'schedule'})[0], 'schedule')
        self.assertEqual(pipeline.buildkite_event({'BUILDKITE_SOURCE': 'ui', 'CERID_CI_TIER': 'change'}),
                         ('workflow_dispatch', '', 'change'))
        self.assertEqual(pipeline.buildkite_event({'BUILDKITE_SOURCE': 'webhook', 'BUILDKITE_PULL_REQUEST': '7',
                                                   'BUILDKITE_PULL_REQUEST_BASE_BRANCH': 'main'}),
                         ('pull_request', 'origin/main', None))
        self.assertEqual(pipeline.buildkite_event({'BUILDKITE_SOURCE': 'webhook'})[0], 'push')

    def test_conclusion_never_passes_budget_and_accepts_cached(self):
        planned = {'steps': [{'key': 'a', 'state': 'run'}, {'key': 'b', 'state': 'cached'}]}
        ok, _ = pipeline.conclude(planned, lambda key: 'passed')
        self.assertTrue(ok)
        planned['steps'].append({'key': 'c', 'state': 'budget'})
        ok, rows = pipeline.conclude(planned, lambda key: 'passed')
        self.assertFalse(ok)
        self.assertIn(('c', 'budget', 'expected but not successful'), rows)
        ok, _ = pipeline.conclude({'steps': [{'key': 'a', 'state': 'run'}]}, lambda key: 'hard_failed')
        self.assertFalse(ok)


def scripts_in(text):
    return set(re.findall(r'scripts/[\w.-]+\.py', text))


class Lanes(unittest.TestCase):
    def dry(self, preset, console=None, device=None):
        run = pipeline.Runner(environ={'PATH': '', 'CRD_CI_TIDY': 'clang-tidy'}, dry=True, log=lambda *_: None)
        device = device or pipeline.devices_by_id(REGISTRY)[pipeline.job_hosts(REGISTRY)[TIERS['presets'][preset]['job']]]
        lane = pipeline.Lane(preset, device, tiers=TIERS, run=run, temp=tempfile.mkdtemp(prefix='cerid-lane-'),
                             console=console, registry=REGISTRY)
        self.assertEqual(lane.execute(), 0)
        return lane, run.commands

    def test_every_hosted_job_helper_is_called_by_its_lane(self):
        steps = pipeline.workflow_steps(WORKFLOW)
        for job in pipeline.Lane.PHASES:
            presets = [p for p, e in TIERS['presets'].items() if e.get('job') == job]
            called = set()
            saved = pipeline.CACHED_PRESETS
            try:
                # A step the workflow gates on the cached list exists in the job even when none of its presets is
                # cached today (windows-shipping); the lane must still carry it.
                for cached in (saved, tuple(presets)):
                    pipeline.CACHED_PRESETS = cached
                    for preset in presets:
                        _, commands = self.dry(preset)
                        called |= scripts_in(' '.join(' '.join(map(str, c['argv'])) for c in commands)
                                             .replace('\\', '/'))
            finally:
                pipeline.CACHED_PRESETS = saved
            hosted = scripts_in(' '.join(run for _, run in steps[job]))
            self.assertEqual(hosted - called, set(), job)

    def test_win_debug_lane_commands(self):
        lane, commands = self.dry('win-debug')
        text = [' '.join(map(str, c['argv'])) for c in commands]
        self.assertTrue(any('-DCRD_COMPILER_LAUNCHER=' in t and '-DCRD_ENABLE_PCH=OFF' in t for t in text))
        self.assertTrue(any("'^crd-dx12-device-info$'" in t or '^crd-dx12-device-info$' in t for t in text))
        self.assertTrue(any('--preset win-debug-at-sse2' in t for t in text))
        self.assertTrue(any('diag-seeded-faults.py' in t for t in text))
        self.assertTrue(any('check-registered-failures.py --lane win-debug-at-sse2' in t for t in text))
        sse2 = next(c for c in commands if '[cpu-level-applied]' in ' '.join(map(str, c['argv'])))
        self.assertEqual(sse2['env'], {'CRD_CPU_LEVEL': 'sse2'})

    def test_narrowed_lane_configures_the_selection(self):
        run = pipeline.Runner(environ={'PATH': ''}, dry=True, log=lambda *_: None)
        device = pipeline.devices_by_id(REGISTRY)['gce-windows-x64']
        lane = pipeline.Lane('win-release', device, tiers=TIERS, run=run, temp=tempfile.mkdtemp(),
                             modules=['hesap-fft', 'core'])
        lane.execute()
        self.assertTrue(any('-DCRD_MODULES=hesap-fft;core' in ' '.join(map(str, c['argv'])) for c in run.commands))

    def test_genmc_runs_on_the_tsan_lane_only(self):
        _, tsan = self.dry('linux-clang-tsan')
        _, debug = self.dry('linux-gcc-debug')
        self.assertTrue(any('check-genmc-models.py' in ' '.join(map(str, c['argv'])) for c in tsan))
        self.assertFalse(any('check-genmc-models.py' in ' '.join(map(str, c['argv'])) for c in debug))

    def test_a_failed_phase_still_bundles_and_uploads(self):
        run = pipeline.Runner(environ={'PATH': '', 'BUILDKITE': 'true'}, dry=True, log=lambda *_: None)
        device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-intel']
        lane = pipeline.Lane('linux-gcc-release', device, tiers=TIERS, run=run, temp=tempfile.mkdtemp())

        def broken():
            raise pipeline.LaneFailure(['cmake --build'], 1)

        lane.build = broken
        self.assertEqual(lane.execute(), 1)
        text = [' '.join(map(str, c['argv'])) for c in run.commands]
        self.assertTrue(any('ci-evidence.py bundle' in t and '--outcome failure' in t for t in text))
        self.assertTrue(any('artifact upload' in t for t in text))
        self.assertFalse(any('ci-evidence.py inventory' in t for t in text))

    def test_env_files_merge_like_the_hosted_runner(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / 'env'
            path.write_text('A=1\nB<<EOF\nline one\nline two\nEOF\nC=x=y\n', encoding='utf-8')
            environ = {}
            pipeline.apply_env_file(path, environ)
            self.assertEqual(environ, {'A': '1', 'B': 'line one\nline two', 'C': 'x=y'})
            self.assertEqual(path.read_text(encoding='utf-8'), '')


class Console(unittest.TestCase):
    def test_linux_profile_command(self):
        profile = REGISTRY['console_profiles']['playstation-5']
        command = pipeline.console_command(['ctest'], profile, False, Path('/b'), Path('/t'))
        text = ' '.join(command)
        for needle in ('systemd-run', 'MemoryMax=%d' % int(12.5 * 1024 ** 3), 'MemorySwapMax=0', 'AllowedCPUs=0-13',
                       'MemoryDenyWriteExecute=yes', 'ProtectSystem=strict', 'ProtectHome=read-only',
                       'PrivateTmp=yes', 'NoNewPrivileges=yes'):
            self.assertIn(needle, text)
        self.assertEqual(command[-1], 'ctest')

    def test_windows_profile_command_and_preparation(self):
        profile = REGISTRY['console_profiles']['xbox-series-x']
        command = pipeline.console_command(['ctest', '--preset', 'win-shipping'], profile, True, Path('B'), Path('T'))
        self.assertEqual(command[2:7], ['jail', '--memory-bytes', str(int(13.5 * 1024 ** 3)), '--cpus', '14'])
        script = pipeline.console_prepare_windows(Path('B'), Path('T'))[-1]
        self.assertIn('Set-ProcessMitigation', script)
        self.assertIn('DynamicCode', script)
        self.assertIn('/setintegritylevel', script)

    def test_console_lane_runs_its_controls_inside_the_constraints(self):
        run = pipeline.Runner(environ={'PATH': ''}, dry=True, log=lambda *_: None)
        device = pipeline.devices_by_id(REGISTRY)['gce-console-profile-playstation']
        lane = pipeline.Lane('linux-gcc-shipping', device, tiers=TIERS, run=run, temp=tempfile.mkdtemp(),
                             console='playstation-5', registry=REGISTRY)
        self.assertEqual(lane.execute(), 0)
        probes = [c['argv'] for c in run.commands if 'console-probe' in c['argv']]
        self.assertEqual(sorted(a[a.index('console-probe') + 1] for a in probes), sorted(pipeline.CONSOLE_PROBES))
        self.assertTrue(all(a[1] == 'systemd-run' for a in probes))
        tests = [c['argv'] for c in run.commands if 'ctest' in c['argv'] and '--output-junit' in c['argv']]
        self.assertTrue(tests and all('systemd-run' in a for a in tests))

    @unittest.skipUnless(os.name == 'nt', 'Job Objects are Windows')
    def test_jail_caps_memory_for_the_whole_process_tree(self):
        code = 'import sys; b = bytearray(%d); b[::4096] = b"\\1" * len(b[::4096]); sys.exit(42)'
        cap = 256 * 1024 ** 2
        small = pipeline.jail(cap, 1, [sys.executable, '-c', code % (32 * 1024 ** 2)])
        large = pipeline.jail(cap, 1, [sys.executable, '-c', code % (512 * 1024 ** 2)])
        self.assertEqual(small, 42)
        self.assertNotEqual(large, 42)

    def test_wx_probe_sees_an_unrestricted_host(self):
        self.assertTrue(pipeline.wx_allocation_allowed())


class Executors(unittest.TestCase):
    PINS = json.loads((ROOT / 'cmake/pins.json').read_text(encoding='utf-8'))

    def test_agent_pins_exist_and_no_digest_lives_in_ci_sources(self):
        for name in executors.AGENT_PINS.values():
            pin = self.PINS['tools'][name]
            self.assertRegex(pin['sha256'], r'^[0-9a-f]{64}$')
            self.assertRegex(pin['member_sha256'], r'^[0-9a-f]{64}$')
            self.assertEqual(pin['version'], '4.6.0')
        for path in list((ROOT / 'ci').rglob('*.py')) + [ROOT / 'ci/devices.json']:
            self.assertIsNone(re.search(r'\b[0-9a-f]{64}\b', path.read_text(encoding='utf-8')), path)

    def test_gce_body_is_a_bounded_spot_machine(self):
        executor = gce_vm.GceVm({'CERID_CI_GCP_PROJECT': 'cerid-ci', 'CERID_CI_RUNNER_SERVICE_ACCOUNT': 'r@x'},
                                pins=self.PINS)
        device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-nvidia-l4']
        zone, body = executor.body('0190ABCD-1234', device, 'acquisition-token')
        self.assertEqual(body['scheduling']['provisioningModel'], 'SPOT')
        self.assertEqual(body['scheduling']['instanceTerminationAction'], 'DELETE')
        self.assertEqual(body['scheduling']['maxRunDuration'], {'seconds': 4 * 3600})
        self.assertEqual(body['scheduling']['onHostMaintenance'], 'TERMINATE')
        self.assertIn('acceleratorTypes/nvidia-l4', body['guestAccelerators'][0]['acceleratorType'])
        self.assertRegex(body['name'], r'^[a-z][a-z0-9-]{0,61}$')
        items = {i['key']: i['value'] for i in body['metadata']['items']}
        pin = self.PINS['tools']['buildkite-agent-linux-amd64']
        self.assertEqual((items['cerid-agent-url'], items['cerid-agent-sha256']), (pin['url'], pin['sha256']))
        self.assertIn('BUILDKITE_AGENT_ACQUIRE_JOB', items['startup-script'])
        self.assertIn('sha256sum -c', items['startup-script'])
        self.assertIn('device=gce-linux-x64-nvidia-l4', items['cerid-agent-tags'])
        self.assertIn('images/family/cerid-ci-ubuntu-2404-x64-nvidia', body['disks'][0]['initializeParams']['sourceImage'])

    def test_gce_console_and_android_machine_features(self):
        executor = gce_vm.GceVm({}, pins=self.PINS)
        _, xbox = executor.body('j', pipeline.devices_by_id(REGISTRY)['gce-console-profile-xbox'], 't')
        self.assertEqual(xbox['minCpuPlatform'], 'AMD Rome')
        self.assertEqual(xbox['advancedMachineFeatures'], {'threadsPerCore': 2})
        self.assertIn('windows-startup-script-ps1', {i['key'] for i in xbox['metadata']['items']})
        _, android = executor.body('j', pipeline.devices_by_id(REGISTRY)['android-emulator-phone-x86_64'], 't')
        self.assertTrue(android['advancedMachineFeatures']['enableNestedVirtualization'])

    def test_gce_launch_and_teardown_calls(self):
        calls = []
        executor = gce_vm.GceVm({'CERID_CI_GCP_PROJECT': 'cerid-ci'}, http=lambda *a, **k: calls.append((a, k)) or (
            200, {}), token=lambda: 't', pins=self.PINS)
        handle = executor.launch('job-1', pipeline.devices_by_id(REGISTRY)['gce-linux-x64-intel'], 'tok')
        executor.teardown(handle)
        self.assertEqual(calls[0][0][0], 'POST')
        self.assertTrue(calls[0][0][1].endswith('/projects/cerid-ci/zones/us-central1-a/instances'))
        self.assertEqual(calls[1][0][0], 'DELETE')
        self.assertEqual(calls[1][1]['allow'], (404,))

    def test_bake_provisioning(self):
        device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-nvidia-l4']
        script = gce_vm.provision_script(device, list(pipeline.APT_PACKAGES['x64']))
        self.assertIn('mesa-vulkan-drivers', script)
        self.assertIn('ubuntu-drivers install', script)
        self.assertIn('image-census.txt', script)
        windows = gce_vm.provision_script(pipeline.devices_by_id(REGISTRY)['gce-windows-x64'], [])
        self.assertIn('Microsoft.VisualStudio.2026.BuildTools', windows)
        self.assertNotIn('{extra}', windows)

    def test_ssh_host_sends_the_token_on_standard_input_only(self):
        seen = {}

        def run(argv, input=None, **_):
            seen['argv'], seen['input'] = argv, input
            return subprocess.CompletedProcess(argv, 0, '', '')

        environ = {'CERID_SSH_AMD_DEVCLOUD_HOST': 'cerid@amd.example', 'CERID_SSH_AMD_DEVCLOUD_KEY_FILE': 'k',
                   'CERID_SSH_AMD_DEVCLOUD_KNOWN_HOSTS': 'kh'}
        executor = ssh_host.SshHost(environ, run=run, pins=self.PINS)
        device = pipeline.devices_by_id(REGISTRY)['amd-developer-cloud-mi300x']
        self.assertEqual(executor.missing(device), [])
        executor.launch('job-9', device, 'SECRET-TOKEN')
        self.assertNotIn('SECRET-TOKEN', ' '.join(seen['argv']))
        self.assertIn('StrictHostKeyChecking=yes', seen['argv'])
        lines = seen['input'].split('\n')
        self.assertEqual(lines[:2], ['read -r token', 'SECRET-TOKEN'])
        self.assertIn(self.PINS['tools']['buildkite-agent-linux-amd64']['sha256'], seen['input'])
        self.assertEqual(ssh_host.SshHost({}, pins=self.PINS).missing(device),
                         ['CERID_SSH_AMD_DEVCLOUD_HOST', 'CERID_SSH_AMD_DEVCLOUD_KEY_FILE',
                          'CERID_SSH_AMD_DEVCLOUD_KNOWN_HOSTS'])

    def test_firebase_command_and_quota(self):
        executor = firebase_test_lab.FirebaseTestLab({'CERID_FTL_PROJECT': 'ftl-project'}, pins=self.PINS)
        device = pipeline.devices_by_id(REGISTRY)['ftl-pixel-phone']
        command = executor.test_command(device, 'app.apk')
        self.assertEqual(command[:5], ['gcloud', 'firebase', 'test', 'android', 'run'])
        self.assertIn('model=shiba,version=34,locale=en,orientation=portrait', command)
        ios = executor.test_command(pipeline.devices_by_id(REGISTRY)['ftl-iphone'], 'tests.zip', kind='xctest')
        self.assertEqual(ios[3], 'ios')
        with tempfile.TemporaryDirectory() as scratch:
            store = pipeline.LocalStore(scratch)
            for i in range(5):
                self.assertEqual(executor.quota_left(store, device, today='2026-10-30'), 5 - i)
                executor.take_quota(store, device, 'j%d' % i, today='2026-10-30')
            self.assertEqual(executor.quota_left(store, device, today='2026-10-30'), 0)
            self.assertEqual(executor.quota_left(store, device, today='2026-10-31'), 5)

    def test_owned_agent_config_names_a_token_file_never_a_token(self):
        device = pipeline.devices_by_id(REGISTRY)['owned-windows-x64-workstation']
        agent = owned_agent.OwnedAgent({}, pins=self.PINS)
        config = agent.config(device)
        self.assertIn('token="file://C:/cerid-ci/agent-token"', config)
        self.assertIn('device=owned-windows-x64-workstation', config)
        self.assertIn('queue=cerid-owned', config)
        with self.assertRaises(RuntimeError):
            agent.launch('j', device, 't')


class FakeStacks:
    """Buildkite's Stacks API as a fake."""

    def __init__(self, jobs, paused=False):
        self.jobs = jobs
        self.paused = paused
        self.calls = []
        self.finished = {}
        self.states = {}

    def __call__(self, method, url, token=None, scheme=None, body=None, **_):
        self.calls.append((method, url, body))
        path = url.split('/v3/stacks', 1)[1]
        if path == '/register':
            return 201, {'key': body['key']}
        if '/scheduled-jobs?' in path:
            return 200, {'jobs': self.jobs, 'page_info': {'has_next_page': False},
                         'cluster_queue': {'dispatch_paused': self.paused}}
        if path.endswith('/batch-reserve'):
            return 200, {'reserved': body['job_uuids'], 'not_reserved': []}
        if path.endswith('/job-acquisition-tokens'):
            return 201, {'job_acquisition_tokens': [{'job_uuid': j, 'job_acquisition_token': 'acq-' + j}
                                                    for j in body['job_uuids']], 'not_issued': []}
        if path.endswith('/finish'):
            self.finished[path.split('/')[-2]] = body
            return 200, {}
        if path.endswith('/get-states'):
            return 200, {'states': {j: self.states.get(j, 'running') for j in body['job_uuids']}}
        raise AssertionError(path)


class FakeExecutor:
    def __init__(self, fail=False):
        self.launched = []
        self.torn = []
        self.fail = fail

    def launch(self, job_id, device, token):
        if self.fail:
            raise RuntimeError('quota exceeded')
        self.launched.append((job_id, device['id'], token))
        return {'instance': job_id}

    def teardown(self, handle):
        self.torn.append(handle)


class Dispatch(unittest.TestCase):
    def make(self, jobs, store, paused=False, fail=False, now=None):
        http = FakeStacks(jobs, paused)
        executor = FakeExecutor(fail)
        dispatcher = pipeline.Dispatcher(REGISTRY, store, 'agent-token', http=http, environ={},
                                         executors={'gce_vm': executor, 'ssh_host': executor,
                                                    'firebase_test_lab': executor},
                                         now=now or (lambda: dt.datetime(2026, 10, 30, 12, tzinfo=dt.timezone.utc)))
        return dispatcher, http, executor

    def test_jobs_route_by_their_device_rule_and_get_one_job_tokens(self):
        store = pipeline.LocalStore(tempfile.mkdtemp())
        jobs = [{'id': 'j1', 'agent_query_rules': ['queue=cerid-cloud', 'device=gce-linux-x64-intel']},
                {'id': 'j2', 'agent_query_rules': ['queue=cerid-cloud', 'device=no-such-device']}]
        dispatcher, http, executor = self.make(jobs, store)
        dispatcher.register()
        launched = dispatcher.tick()
        self.assertEqual([r['job'] for r in launched], ['j1'])
        self.assertEqual(executor.launched, [('j1', 'gce-linux-x64-intel', 'acq-j1')])
        self.assertEqual(http.finished['j2']['exit_status'], 1)
        self.assertEqual(store.get_json('machines/j1.json')['device'], 'gce-linux-x64-intel')

    def test_paid_jobs_past_the_cap_end_as_budget(self):
        store = pipeline.LocalStore(tempfile.mkdtemp())
        store.put_json('ledger/2026-10/old.json', {'usd': 49.9})
        jobs = [{'id': 'j1', 'agent_query_rules': ['device=gce-windows-x64']}]
        dispatcher, http, executor = self.make(jobs, store)
        dispatcher.tick()
        self.assertEqual(executor.launched, [])
        self.assertEqual(http.finished['j1']['exit_status'], pipeline.BUDGET_EXIT)
        self.assertTrue(http.finished['j1']['detail'].startswith('budget:'))

    def test_gcp_stop_and_dispatch_pause(self):
        store = pipeline.LocalStore(tempfile.mkdtemp())
        store.put_json('budget/gcp-2026-10.json', {'costAmount': 50.0, 'budgetAmount': 50.0})
        jobs = [{'id': 'j1', 'agent_query_rules': ['device=gce-linux-x64-intel']}]
        dispatcher, http, executor = self.make(jobs, store)
        dispatcher.tick()
        self.assertEqual(http.finished['j1']['exit_status'], pipeline.BUDGET_EXIT)
        dispatcher, http, executor = self.make(jobs, pipeline.LocalStore(tempfile.mkdtemp()), paused=True)
        self.assertEqual(dispatcher.tick(), [])
        self.assertEqual(http.finished, {})

    def test_launch_failure_finishes_the_job_with_its_reason(self):
        store = pipeline.LocalStore(tempfile.mkdtemp())
        jobs = [{'id': 'j1', 'agent_query_rules': ['device=gce-linux-x64-intel']}]
        dispatcher, http, _ = self.make(jobs, store, fail=True)
        dispatcher.tick()
        self.assertIn('quota exceeded', http.finished['j1']['detail'])

    def test_reaper_tears_down_and_writes_the_ledger(self):
        store = pipeline.LocalStore(tempfile.mkdtemp())
        jobs = [{'id': 'j1', 'agent_query_rules': ['device=gce-linux-x64-intel']}]
        dispatcher, http, executor = self.make(jobs, store)
        dispatcher.tick()
        http.jobs = []
        http.states['j1'] = 'finished'
        dispatcher.now = lambda: dt.datetime(2026, 10, 30, 14, tzinfo=dt.timezone.utc)
        ended = dispatcher.reap()
        self.assertEqual(len(ended), 1)
        self.assertEqual(executor.torn, [{'instance': 'j1'}])
        entry = store.get_json('ledger/2026-10/j1.json')
        self.assertAlmostEqual(entry['usd'], 0.24, places=4)
        self.assertEqual(dispatcher.reap(), [])


class Parity(unittest.TestCase):
    def test_every_hosted_job_and_step_has_a_buildkite_step(self):
        rows, problems = pipeline.parity_rows(WORKFLOW, TIERS)
        self.assertEqual(problems, [])
        jobs = {row[0] for row in rows}
        self.assertEqual(jobs, set(pipeline.workflow_steps(WORKFLOW)))

    def test_lane_lists_match_the_workflow(self):
        env = re.search(r"CRD_CACHED_PRESETS: '([^']*)'", WORKFLOW, re.S).group(1).split()
        cold = re.search(r"CRD_COLD_PRESETS: '([^']*)'", WORKFLOW).group(1).split()
        self.assertEqual(list(pipeline.CACHED_PRESETS), env)
        self.assertEqual(list(pipeline.COLD_PRESETS), cold)
        steps = pipeline.workflow_steps(WORKFLOW)
        for job, key in (('linux-gcc', 'x64'), ('linux-arm64', 'arm64')):
            run = dict(steps[job])['Install system dependencies']
            body = run.split('install -y', 1)[1]
            listed = [token for line in body.splitlines() for token in line.split('#', 1)[0].replace('\\', ' ').split()]
            self.assertEqual(listed, list(pipeline.APT_PACKAGES[key]), job)
        genmc = dict(steps['linux-gcc'])['Install GenMC toolchain'].split('install -y', 1)[1].split('\n')[0].split()
        self.assertEqual(genmc, list(pipeline.APT_PACKAGES['genmc']))
        self.assertIn(pipeline.NATIVE_TESTS, dict(steps['windows-native'])['Test'])

    def test_repository_checks_match_the_workflow(self):
        hosted = [(label, run) for label, run in pipeline.workflow_steps(WORKFLOW)['repository']
                  if run.startswith('python ')]
        ours = {title: args for title, args, _ in pipeline.REPOSITORY_CHECKS}
        for label, run in hosted:
            if label == 'Install pinned Ninja 1.11.1':
                continue
            self.assertEqual(shlex.split(run)[1:], ours[label], label)
        self.assertEqual({label for label, _ in hosted} - {'Install pinned Ninja 1.11.1'}, set(ours))

    def test_design_document_carries_the_generated_table(self):
        rows, _ = pipeline.parity_rows(WORKFLOW, TIERS)
        text = (ROOT / 'docs/design/ci-platform.md').read_text(encoding='utf-8')
        begin = text.index('<!-- parity:begin -->') + len('<!-- parity:begin -->\n')
        end = text.index('<!-- parity:end -->')
        expected = pipeline.parity_markdown(rows) + '\n' + pipeline.parity_markdown(pipeline.experiment_rows())
        self.assertEqual(text[begin:end], expected)

    def test_host_check_experiments_stay_listed_as_open(self):
        rows = pipeline.experiment_rows()
        self.assertTrue(rows)
        self.assertTrue(all(r[3].startswith('open (CI-4)') for r in rows))


class Dashboard(unittest.TestCase):
    def render(self):
        data = json.loads(dashboard.FIXTURE.read_text(encoding='utf-8'))
        return dashboard.render(REGISTRY, data['results'], data['plan'], dashboard.roadmap_rows(), data['budget'],
                                fixture=True)

    def test_states_regressions_and_never_a_false_pass(self):
        page, document = self.render()
        cells = {(c['device'], c['feature']): c['state'] for c in document['cells']}
        self.assertEqual(cells[('gce-linux-x64-intel', 'unit')], 'pass')
        self.assertEqual(cells[('gce-linux-x64-intel', 'sanitizers')], 'fail')
        self.assertEqual(cells[('gce-windows-x64', 'build')], 'cached')
        self.assertEqual(cells[('gce-windows-x64-nvidia-t4', 'dx12')], 'budget')
        self.assertEqual(cells[('windows-arm64-host', 'build')], 'waiting')
        for console in ('xbox-series-devmode', 'xbox-series-gdk-devkit', 'playstation-5-devkit',
                        'playstation-5-testkit', 'nintendo-switch-2-devkit'):
            self.assertEqual(cells[(console, 'console-platform')], 'waiting')
        self.assertEqual(document['regressions'], [{'device': 'gce-linux-x64-intel', 'feature': 'sanitizers'}])
        self.assertIn('Fixture data', page)
        self.assertIn('31.40 / 50 USD', page)

    def test_a_result_for_a_waiting_device_is_not_shown_as_pass(self):
        data = json.loads(dashboard.FIXTURE.read_text(encoding='utf-8'))
        data['results'].append({'step': 'x', 'device': 'playstation-5-devkit', 'outcome': 'pass', 'build': '200',
                                'features': ['console-platform']})
        _, document = dashboard.render(REGISTRY, data['results'], data['plan'], {}, data['budget'])
        cells = {(c['device'], c['feature']): c['state'] for c in document['cells']}
        self.assertEqual(cells[('playstation-5-devkit', 'console-platform')], 'waiting')

    def test_clauses_without_evidence_name_real_rows(self):
        _, document = self.render()
        cells = {(c['device'], c['feature']): c for c in document['cells']}
        listing = dashboard.clauses_without_evidence(REGISTRY, cells, dashboard.roadmap_rows())
        rows = {item['row'] for item in listing}
        self.assertTrue({'CEIR-JIT.3', '6.0', 'MAC.1', 'WEB.1'} <= rows)
        self.assertTrue(all(item['state'] != '?' for item in listing))

    def test_page_is_escaped_and_deterministic(self):
        first, _ = self.render()
        second, _ = self.render()
        self.assertEqual(first, second)
        self.assertNotIn('<script', first)
        registry = json.loads(json.dumps(REGISTRY))
        registry['devices'][0]['gpu']['model'] = '<b>x</b>'
        page, _ = dashboard.render(registry, [], None, {}, None)
        self.assertIn('&lt;b&gt;x&lt;/b&gt;', page)

    def test_publish_without_an_account_keeps_the_artifact(self):
        calls = []
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(dashboard.publish(Path('site'), environ={}, run=lambda *a, **k: calls.append(a)), 0)
        self.assertEqual(calls, [])
        self.assertIn('not configured', out.getvalue())

    def test_publisher_is_locked(self):
        manifest = json.loads((ROOT / 'ci/dashboard/package.json').read_text(encoding='utf-8'))
        lock = json.loads((ROOT / 'ci/dashboard/package-lock.json').read_text(encoding='utf-8'))
        version = manifest['devDependencies']['wrangler']
        self.assertRegex(version, r'^\d+\.\d+\.\d+$')
        self.assertEqual(lock['packages']['node_modules/wrangler']['version'], version)
        for name, entry in lock['packages'].items():
            if name:
                self.assertTrue(entry.get('integrity', '').startswith('sha512-'), name)


class Ops(unittest.TestCase):
    def test_accounts_report_names_only(self):
        environ = {'CERID_CI_GCP_PROJECT': 'very-secret-project-value', 'CERID_BUILDKITE_ORG': 'org-value'}
        rows = ops.accounts_report(REGISTRY, environ, set())
        text = json.dumps(rows)
        self.assertNotIn('very-secret-project-value', text)
        self.assertNotIn('org-value', text)
        states = {row[0]: row[3] for row in rows}
        self.assertEqual(states['gcp-project'], 'configured')
        self.assertEqual(states['cloudflare-pages'], 'missing')
        self.assertEqual(states['xbox-partner-center'], 'user program')
        self.assertEqual(states['buildkite-api-token'], 'missing (optional)')

    def test_add_device_appends_and_keeps_the_existing_text(self):
        text = pipeline.DEVICES.read_text(encoding='utf-8')
        entry = {'id': 'owned-test', 'status': 'planned'}
        updated = ops.insert_device(text, entry)
        self.assertTrue(updated.startswith(text[:text.rstrip().rfind(']')].rstrip()))
        self.assertEqual(json.loads(updated)['devices'][-1], entry)

    def test_add_device_refuses_an_invalid_cell_and_writes_nothing(self):
        before = pipeline.DEVICES.read_bytes()
        argv = ['add-device', '--id', 'gce-linux-x64-intel', '--kind', 'VM', '--os', 'Ubuntu', '--os-version', '24.04',
                '--arch', 'x86_64', '--cpu-model', 'x', '--levels', 'portable', '--gpu-vendor', 'software',
                '--gpu-model', 'none', '--executor', 'owned_agent', '--tier', 'B', '--cost-class', 'free',
                '--features', 'build', '--route', 'duplicate id']
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(ops.main(argv), 1)
        self.assertIn('registered twice', out.getvalue())
        self.assertEqual(pipeline.DEVICES.read_bytes(), before)
        argv[2] = 'owned-test-laptop'
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(ops.main(argv + ['--dry-run']), 0)
        self.assertIn('device=owned-test-laptop', out.getvalue())
        self.assertEqual(pipeline.DEVICES.read_bytes(), before)

    def test_parity_command_passes(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ops.main(['parity']), 0)


class RepositoryFixes(unittest.TestCase):
    def test_evidence_records_buildkite_run_identity(self):
        environ = {'BUILDKITE_BUILD_NUMBER': '12', 'BUILDKITE_JOB_ID': 'j', 'BUILDKITE_STEP_KEY': 'lane-win-debug',
                   'BUILDKITE_PIPELINE_SLUG': 'cerid', 'BUILDKITE_COMMIT': 'abc', 'CERID_CI_DEVICE': 'gce-windows-x64'}
        for key in environ:
            self.assertIn(key, evidence.RUN_KEYS)

    def test_root_layout_accepts_a_worktree_git_file_and_the_ci_directory(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            (root / '.git').write_text('gitdir: /elsewhere\n', encoding='utf-8')
            (root / 'ci').mkdir()
            (root / 'stray.txt').write_text('x', encoding='utf-8')
            errors = hygiene.root_artifact_errors(root)
            self.assertEqual(errors, ['Unexpected root artifact: stray.txt; scratch belongs under build/<task>/'])


if __name__ == '__main__':
    unittest.main()
