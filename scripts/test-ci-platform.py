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
import urllib.parse

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
ALL_ACCOUNTS = {'CERID_CI_URL': 'https://ci.example', 'CERID_CI_GCP_PROJECT': 'cerid-ci', 'CERID_CI_BUDGET_SUBSCRIPTION': 's',
                'CERID_CI_RUNNER_SERVICE_ACCOUNT': 'runner@cerid-ci', 'CERID_CI_STORE': 'x', 'CERID_FTL_PROJECT': 'f',
                'CERID_CLOUDFLARE_ACCOUNT_ID': 'a', 'CERID_SSH_AMD_DEVCLOUD_HOST': 'h', 'CERID_SSH_MAC_HOST': 'h',
                'CERID_SSH_WINDOWS_ARM64_HOST': 'h', 'CERID_SSH_INTEL_TIBER_HOST': 'h', 'CERID_CI_OWNED_AGENTS': '1'}
SECRETS = {'cloudflare-pages-token'}
COMPUTE_ZONE = 'https://compute.googleapis.com/compute/v1/projects/cerid-ci/zones/us-central1-a'


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

    def test_a_hyperdisk_only_machine_needs_a_hyperdisk(self):
        broken = json.loads(json.dumps(REGISTRY))
        axion = pipeline.devices_by_id(broken)['gce-linux-arm64-axion']['executor_config']
        axion.pop('disk_type')
        self.assertIn('device gce-linux-arm64-axion: c4a machines take no Persistent Disk; set disk_type '
                      'hyperdisk-balanced',
                      pipeline.validate_registry(broken, TIERS, PRESETS, ROWS))
        executor = gce_vm.GceVm({})
        devices = pipeline.devices_by_id(REGISTRY)
        for ident, disk in (('gce-linux-arm64-axion', 'hyperdisk-balanced'), ('gce-linux-x64-intel', 'pd-balanced')):
            _, body = executor.body('m', devices[ident], AGENT_SPEC)
            _, bake = executor.bake_body(devices[ident], 'script')
            for made in (body, bake):
                self.assertEqual(made['disks'][0]['initializeParams']['diskType'],
                                 'zones/us-central1-a/diskTypes/%s' % disk, ident)

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
    def plan(self, event='push', paths=None, requested=None, store=None, environ=None, registry=None, fresh=False,
             baked=True):
        store = store or pipeline.LocalStore(tempfile.mkdtemp(prefix='cerid-plan-'))
        for device in (registry or REGISTRY)['devices'] if baked else ():
            family = device.get('executor_config', {}).get('image_family')
            if family and not store.get_json('images/%s.json' % family):
                store.put_json('images/%s.json' % family, {'image': family + '-202610310900'})
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

    def test_the_plan_step_environment_carries_every_launcher_account_and_nothing_else(self):
        """The plan runs in an agent's clean environment, so plan_environment is all of the launcher it sees: every
        environment account must be in it (without them the plan planned nothing) and no other launcher variable."""
        accounts = REGISTRY['accounts'].values()
        launcher = {spec['name']: 'v' for spec in accounts if spec.get('kind') == 'env'}
        launcher.update({spec['env_file']: '/etc/cerid-ci/f' for spec in accounts if spec.get('env_file')})
        launcher.update({'CERID_CI_OWNED_AGENTS': '1', 'CERID_CI_SCCACHE_BUCKET': 'c', 'HOME': '/var/lib/cerid-ci',
                         'CERID_CI_REPOSITORY_URL': 'https://git.example/cerid.git', 'CERID_CI_MIRROR_DIR': '/m'})
        passed = pipeline.plan_environment(REGISTRY, launcher)
        self.assertEqual(set(launcher) - set(passed), {'HOME', 'CERID_CI_REPOSITORY_URL', 'CERID_CI_MIRROR_DIR'})
        self.assertEqual(pipeline.plan_environment(REGISTRY, {'CERID_CI_GCP_PROJECT': ''}), {})
        planned = self.plan(event='schedule', environ=pipeline.plan_environment(REGISTRY, ALL_ACCOUNTS))
        self.assertEqual([s['key'] for s in planned['steps']], [s['key'] for s in self.plan(event='schedule')['steps']])
        self.assertTrue(planned['steps'])

    def test_a_device_whose_image_family_is_not_baked_waits(self):
        store = pipeline.LocalStore(tempfile.mkdtemp(prefix='cerid-plan-'))
        store.put_json('images/cerid-ci-ubuntu-2404-arm64.json', {'image': 'cerid-ci-ubuntu-2404-arm64-202610310401'})
        planned = self.plan(event='schedule', store=store, baked=False)
        devices = pipeline.devices_by_id(REGISTRY)
        stepped = {s['device'] for s in planned['steps']}
        self.assertIn('gce-linux-arm64-ampere', stepped)
        self.assertFalse({d for d in stepped if devices[d].get('executor_config', {}).get('image_family')} -
                         {'gce-linux-arm64-ampere', 'gce-linux-arm64-axion'})
        reasons = {c['reason'] for c in planned['cells'] if c['device'] == 'gce-windows-x64'}
        self.assertIn('waiting: image cerid-ci-windows-2025 not baked (ops.py device bake gce-windows-x64)', reasons)

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

    def test_orchestrator_document(self):
        from coordinator import steps as orchestrator_steps
        store = pipeline.LocalStore(tempfile.mkdtemp(prefix='cerid-plan-'))
        store.put_json('ledger/2026-10/x.json', {'usd': 49.9})
        planned = self.plan(event='schedule', store=store)
        document = pipeline.orchestrator_pipeline(planned, REGISTRY['queues'])
        normalized, _ = orchestrator_steps.normalize(json.loads(json.dumps(document)), existing_keys={'plan'})
        self.assertEqual(document['schema'], orchestrator_steps.SCHEMA)
        steps = {s['key']: s for s in normalized}
        required = steps['required']
        self.assertEqual({d['step'] for d in required['depends_on']}, {s['key'] for s in planned['steps']})
        self.assertTrue(all(d['allow_failure'] for d in required['depends_on']))
        self.assertEqual(steps['dashboard']['depends_on'], [{'step': 'required', 'allow_failure': True}])
        for step in planned['steps']:
            entry = steps[step['key']]
            self.assertEqual(entry['agents']['device'], step['device'])
            self.assertEqual(entry['retry'], pipeline.RETRY)
            self.assertEqual(entry['priority'], 3 - step['priority'])
            if step['state'] in ('budget', 'cached'):
                self.assertEqual(entry['skip'], {'state': step['state'], 'reason': step['reason']})
            else:
                self.assertIsNone(entry['skip'])
            if entry['agents']['queue'] == 'cerid-cloud':
                self.assertEqual((entry['concurrency_group'], entry['concurrency']), pipeline.CLOUD_GROUP)
        owned = [s for s in planned['steps'] if pipeline.devices_by_id(REGISTRY)[s['device']][
            'executor'] == 'owned_agent']
        for step in owned:
            self.assertEqual(steps[step['key']]['agents']['queue'], 'cerid-owned')
            self.assertIsNone(steps[step['key']]['concurrency_group'])

    def test_runner_settings_travel_in_the_document_and_secrets_do_not(self):
        planned = self.plan(paths=['docs/ROADMAP.md'])
        document = pipeline.orchestrator_pipeline(planned, REGISTRY['queues'], environ={
            'CERID_CI_STORE': 'gs://cerid-ci-results', 'CERID_CI_SCCACHE_BUCKET': 'cerid-ci-sccache',
            'CERID_CI_GCP_PROJECT': 'cerid-ci', 'CERID_CI_SECRETS_DIR': '/run/secrets'})
        self.assertEqual(document['env']['CERID_CI_STORE'], 'gs://cerid-ci-results')
        self.assertEqual(document['env']['CERID_CI_SCCACHE_BUCKET'], 'cerid-ci-sccache')
        self.assertNotIn('CERID_CI_SECRETS_DIR', json.dumps(document))

    def test_orchestrator_events(self):
        self.assertEqual(pipeline.cerid_event({'CERID_CI_SOURCE': 'schedule'})[0], 'schedule')
        self.assertEqual(pipeline.cerid_event({'CERID_CI_SOURCE': 'manual', 'CERID_CI_TIER': 'change'}),
                         ('workflow_dispatch', '', 'change'))
        self.assertEqual(pipeline.cerid_event({'CERID_CI_SOURCE': 'webhook', 'CERID_CI_EVENT': 'pull_request',
                                               'CERID_CI_PULL_REQUEST': '7',
                                               'CERID_CI_PULL_REQUEST_BASE_BRANCH': 'main'}),
                         ('pull_request', 'origin/main', None))
        self.assertEqual(pipeline.cerid_event({'CERID_CI_SOURCE': 'webhook', 'CERID_CI_PULL_REQUEST': 'false'})[0],
                         'push')
        self.assertEqual(pipeline.cerid_event({'CERID_CI_SOURCE': 'poll'})[0], 'push')

    def test_a_soft_failed_step_counts_as_passed_for_the_required_result(self):
        from unittest import mock
        done = subprocess.CompletedProcess([], 0, 'soft_failed\n', '')
        with mock.patch.object(pipeline.subprocess, 'run', return_value=done) as run:
            self.assertEqual(pipeline.orchestrator_outcome('lane-x'), 'passed')
        self.assertEqual(run.call_args[0][0][-3:], ['step', 'outcome', 'lane-x'])

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
        run = pipeline.Runner(environ={'PATH': '', 'CERID_CI': '1'}, dry=True, log=lambda *_: None)
        device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-intel']
        lane = pipeline.Lane('linux-gcc-release', device, tiers=TIERS, run=run, temp=tempfile.mkdtemp())

        def broken():
            raise pipeline.LaneFailure(['cmake --build'], 1)

        lane.build = broken
        self.assertEqual(lane.execute(), 1)
        text = [' '.join(map(str, c['argv'])) for c in run.commands]
        self.assertTrue(any('ci-evidence.py bundle' in t and '--outcome failure' in t for t in text))
        self.assertTrue(any('artifact upload' in t for t in text))
        # The summary is the annotation's body: a file, because the runner gives commands no standard input.
        self.assertTrue(any('annotate --context linux-gcc-release --style error --body-file' in t and
                            t.endswith('summary.md') for t in text))
        self.assertFalse(any('ci-evidence.py inventory' in t for t in text))

    def test_per_test_cache_is_one_record_per_run(self):
        class CountingStore(pipeline.LocalStore):
            reads = 0

            def get(self, name):
                CountingStore.reads += 1
                return super().get(name)

        with tempfile.TemporaryDirectory() as scratch:
            build = Path(scratch) / 'build'
            (build / 'Testing/Temporary').mkdir(parents=True)
            exe = build / 't.exe'
            exe.write_bytes(b'binary')
            inventory = {'tests': [{'name': 'case-%d' % i, 'command': [str(exe), str(i)], 'properties': []}
                                   for i in range(300)]}
            (build / 'Testing/inventory.json').write_text(json.dumps(inventory), encoding='utf-8')
            junit = build / 'Testing/Temporary/ctest-junit.xml'
            junit.write_text('<testsuite>' + ''.join('<testcase name="case-%d"/>' % i for i in range(299)) +
                             '<testcase name="case-299"><failure/></testcase></testsuite>', encoding='utf-8')
            store = CountingStore(Path(scratch) / 'store')
            device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-intel']
            run = pipeline.Runner(environ={'CERID_CI_BUILD_NUMBER': '7'}, dry=False, log=lambda *_: None)
            lane = pipeline.Lane('linux-gcc-release', device, tiers=TIERS, run=run, temp=scratch, store=store)
            lane.build_dir = str(build)  # absolute: ROOT / build_dir is the scratch tree
            lane.record_tests(str(junit))
            self.assertEqual(len(store.list(lane.test_cache_prefix())), 1)
            CountingStore.reads = 0
            path = lane.cached_tests()
            self.assertLessEqual(CountingStore.reads, 5)
            names = path.read_text(encoding='utf-8').split()
            self.assertEqual(len(names), 299)
            self.assertNotIn('case-299', names)

    def test_a_store_failure_never_changes_a_lane_verdict(self):
        class BrokenStore(pipeline.LocalStore):
            def put(self, name, data):
                raise OSError('bucket unreachable')

            def get(self, name):
                raise OSError('bucket unreachable')

        run = pipeline.Runner(environ={'PATH': ''}, dry=True, log=lambda *_: None)
        device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-intel']
        lane = pipeline.Lane('linux-gcc-release', device, tiers=TIERS, run=run, temp=tempfile.mkdtemp(),
                             store=BrokenStore(tempfile.mkdtemp()))
        self.assertEqual(lane.execute(), 0)

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
        command = pipeline.console_command(['ctest'], profile, False, Path('/b'), Path('/t'), {
            'PATH': '/usr/bin', 'VULKAN_SDK': '/b/vk', 'VK_LAYER_PATH': '/b/layers', 'LD_LIBRARY_PATH': '/b/lib',
            'CRD_PLATFORM_HEADLESS': '1', 'BUILDKITE_AGENT_ACCESS_TOKEN': 'secret-job-token',
            'CERID_BUILDKITE_AGENT_TOKEN_FILE': '/run/t'})
        text = ' '.join(command)
        for needle in ('--setenv=VULKAN_SDK=/b/vk', '--setenv=VK_LAYER_PATH=/b/layers',
                       '--setenv=LD_LIBRARY_PATH=/b/lib', '--setenv=CRD_PLATFORM_HEADLESS=1', '--setenv=PATH=/usr/bin'):
            self.assertIn(needle, command)
        self.assertNotIn('secret-job-token', text)
        self.assertNotIn('CERID_BUILDKITE_AGENT_TOKEN_FILE', text)
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

    @unittest.skipUnless(os.name == 'nt', 'Job Objects are Windows')
    def test_cores_probe_reads_the_real_affinity(self):
        self.assertEqual(pipeline.windows_affinity_count(), os.cpu_count())
        code = ('import importlib.util, sys; s = importlib.util.spec_from_file_location("p", r"%s"); '
                'm = importlib.util.module_from_spec(s); sys.modules["p"] = m; s.loader.exec_module(m); '
                'sys.exit(m.windows_affinity_count())' % (ROOT / 'ci/pipeline.py'))
        self.assertEqual(pipeline.jail(1024 ** 3, 1, [sys.executable, '-c', code]), 1)

    def test_console_lane_keeps_its_tools_where_the_tests_can_read_them(self):
        run = pipeline.Runner(environ={'PATH': ''}, dry=True, log=lambda *_: None)
        device = pipeline.devices_by_id(REGISTRY)['gce-console-profile-playstation']
        lane = pipeline.Lane('linux-gcc-shipping', device, tiers=TIERS, run=run, temp=tempfile.mkdtemp(),
                             console='playstation-5', registry=REGISTRY)
        lane.vulkan_headers()
        self.assertEqual(lane.env['VULKAN_SDK'], str(lane.temp / 'vulkan-sdk'))

    def test_wx_probe_sees_an_unrestricted_host(self):
        self.assertTrue(pipeline.wx_allocation_allowed())


AGENT_SPEC = {'coordinator': 'http://10.128.0.2:8780', 'external': 'https://ci.example',
              'agent_url': 'http://10.128.0.2:8780/agent/cerid-agent.py', 'agent_sha256': 'ab' * 32}


class Executors(unittest.TestCase):
    def test_no_buildkite_agent_and_no_digest_lives_in_ci_sources(self):
        pins = json.loads((ROOT / 'cmake/pins.json').read_text(encoding='utf-8'))['tools']
        self.assertFalse([name for name in pins if name.startswith('buildkite')])
        for path in list((ROOT / 'ci').rglob('*.py')) + [ROOT / 'ci/devices.json', ROOT / 'ci/coordinator.json']:
            text = path.read_text(encoding='utf-8')
            self.assertIsNone(re.search(r'\b[0-9a-f]{64}\b', text), path)
            self.assertNotIn('buildkite-agent', text, path)

    def test_gce_body_is_a_bounded_spot_machine_without_a_credential(self):
        executor = gce_vm.GceVm({'CERID_CI_GCP_PROJECT': 'cerid-ci', 'CERID_CI_RUNNER_SERVICE_ACCOUNT': 'r@x'})
        device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-nvidia-l4']
        zone, body = executor.body('cerid-ci-0190abcd-a1', device, AGENT_SPEC)
        # The coordinator finds a machine's reserved job by the instance name in its identity token, so the instance
        # is named exactly as the dispatcher named the machine (a doubled prefix left every machine refused).
        self.assertEqual(body['name'], 'cerid-ci-0190abcd-a1')
        self.assertIn({'key': 'cerid-machine', 'value': 'cerid-ci-0190abcd-a1'}, body['metadata']['items'])
        self.assertEqual(gce_vm.instance_name('bake-gce-windows-x64'), 'cerid-ci-bake-gce-windows-x64')
        self.assertEqual(body['scheduling']['provisioningModel'], 'SPOT')
        self.assertEqual(body['scheduling']['instanceTerminationAction'], 'DELETE')
        self.assertEqual(body['scheduling']['maxRunDuration'], {'seconds': 4 * 3600})
        self.assertEqual(body['scheduling']['onHostMaintenance'], 'TERMINATE')
        self.assertIn('acceleratorTypes/nvidia-l4', body['guestAccelerators'][0]['acceleratorType'])
        self.assertRegex(body['name'], r'^[a-z][a-z0-9-]{0,61}$')
        items = {i['key']: i['value'] for i in body['metadata']['items']}
        self.assertEqual((items['cerid-agent-url'], items['cerid-agent-sha256'], items['cerid-coordinator']),
                         (AGENT_SPEC['agent_url'], AGENT_SPEC['agent_sha256'], AGENT_SPEC['coordinator']))
        self.assertIn('--identity gce', items['startup-script'])
        self.assertIn('sha256sum -c', items['startup-script'])
        self.assertIn('device=gce-linux-x64-nvidia-l4', items['cerid-agent-tags'])
        self.assertFalse([k for k in items if 'token' in k], 'a GCE machine is given no token at all')
        self.assertIn('images/family/cerid-ci-ubuntu-2404-x64-nvidia', body['disks'][0]['initializeParams']['sourceImage'])
        with self.assertRaises(ValueError):
            executor.body('m', device, dict(AGENT_SPEC, token='cjt_x'))

    def test_gce_names_a_preemption_only_when_compute_engine_recorded_one(self):
        seen = []

        def http(method, url, **kwargs):
            seen.append(url)
            return 200, {'items': [{'targetLink': COMPUTE_ZONE + '/instances/cerid-ci-a-a1'}]}
        executor = gce_vm.GceVm({}, http=http, token=lambda: 't')
        handle = {'project': 'cerid-ci', 'zone': 'us-central1-a', 'instance': 'cerid-ci-a-a1'}
        self.assertEqual(executor.lost_reason(handle), 'preempted')
        self.assertEqual(executor.lost_reason(dict(handle, instance='cerid-ci-a-a2')), 'vm_lost')
        self.assertIn('compute.instances.preempted', urllib.parse.unquote(seen[0]))

    def test_gce_console_and_android_machine_features(self):
        executor = gce_vm.GceVm({})
        _, xbox = executor.body('m', pipeline.devices_by_id(REGISTRY)['gce-console-profile-xbox'], AGENT_SPEC)
        self.assertEqual(xbox['minCpuPlatform'], 'AMD Rome')
        self.assertEqual(xbox['advancedMachineFeatures'], {'threadsPerCore': 2})
        items = {i['key']: i['value'] for i in xbox['metadata']['items']}
        self.assertIn('Get-FileHash -Algorithm SHA256', items['windows-startup-script-ps1'])
        _, android = executor.body('m', pipeline.devices_by_id(REGISTRY)['android-emulator-phone-x86_64'], AGENT_SPEC)
        self.assertTrue(android['advancedMachineFeatures']['enableNestedVirtualization'])

    def test_gce_launch_probe_list_and_teardown_calls(self):
        calls = []

        def http(method, url, **kwargs):
            calls.append((method, url, kwargs))
            if method == 'GET' and '/aggregated/' in url:
                return 200, {'items': {'zones/us-central1-a': {'instances': [
                    {'name': 'cerid-ci-x', 'creationTimestamp': '2026-10-30T12:00:00.000-07:00'}]}}}
            if method == 'GET':
                return (404, {}) if url.endswith('/gone') else (200, {'status': 'RUNNING'})
            return 200, {}
        executor = gce_vm.GceVm({'CERID_CI_GCP_PROJECT': 'cerid-ci'}, http=http, token=lambda: 't')
        handle = executor.launch('cerid-ci-job-1-a1', pipeline.devices_by_id(REGISTRY)['gce-linux-x64-intel'],
                                 AGENT_SPEC)
        self.assertTrue(executor.alive(handle))
        self.assertFalse(executor.alive(dict(handle, instance='gone')))
        self.assertEqual([m['name'] for m in executor.machines()], ['cerid-ci-x'])
        self.assertIn('filter=labels.cerid-ci%3Djob', calls[3][1])
        executor.teardown(handle)
        self.assertEqual(calls[0][0], 'POST')
        self.assertTrue(calls[0][1].endswith('/projects/cerid-ci/zones/us-central1-a/instances'))
        self.assertEqual(calls[-1][0], 'DELETE')
        self.assertEqual(calls[-1][2]['allow'], (404,))

    def test_bake_provisioning(self):
        device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-nvidia-l4']
        script = gce_vm.provision_script(device, list(pipeline.APT_PACKAGES['x64']))
        self.assertIn('mesa-vulkan-drivers', script)
        self.assertIn('ubuntu-drivers install', script)
        self.assertIn('image-census.txt', script)
        # Lane commands start with `python`; Ubuntu ships only python3 unless python-is-python3 is installed.
        self.assertIn('python-is-python3', script)
        windows = gce_vm.provision_script(pipeline.devices_by_id(REGISTRY)['gce-windows-x64'], [])
        self.assertIn('Microsoft.VisualStudio.2026.BuildTools', windows)
        self.assertIn('--scope machine --id Python.Python.3.12', windows)
        self.assertNotIn('{extra}', windows)
        # The completion marker is the last step before the power-off (Linux) or before sysprep (Windows).
        self.assertLess(script.index(gce_vm.BAKE_MARKER), script.index('sleep 90'))
        self.assertLess(script.index('sleep 90'), script.rindex('shutdown -h now'))
        self.assertLess(script.index("trap '"), script.index('set -Eeuxo'), 'a traced trap prints the failed marker')
        self.assertEqual(script.count(gce_vm.BAKE_FAILED_MARKER), 1)
        self.assertLess(windows.index(gce_vm.BAKE_MARKER), windows.index('sysprep.exe'))
        self.assertIn(gce_vm.BAKE_FAILED_MARKER, windows)

    def bake_calls(self, serial, operation_error=None):
        calls, clock = [], [0.0]

        def http(method, url, **kwargs):
            calls.append((method, url))
            running = clock[0] < 60
            if method == 'GET' and 'serialPort' in url:
                if not running:  # Compute Engine refuses a stopped instance's serial console
                    raise gce_vm.HttpError(400, '{"error": {"errors": [{"reason": "resourceNotReady"}]}}')
                start = int(url.rsplit('&start=', 1)[1])
                return 200, {'contents': serial[start:], 'next': len(serial)}
            if method == 'GET' and '/global/operations/' in url:
                done = {'name': 'op-image', 'status': 'DONE'}
                return 200, dict(done, error=operation_error) if operation_error else done
            if method == 'GET':
                return 200, {'status': 'RUNNING' if running else 'TERMINATED'}
            if method == 'POST' and url.endswith('/global/images'):
                return 200, {'name': 'op-image', 'status': 'RUNNING'}
            return 200, {}

        def sleep(seconds):
            clock[0] += seconds
        executor = gce_vm.GceVm({'CERID_CI_GCP_PROJECT': 'cerid-ci'}, http=http, token=lambda: 't')
        device = pipeline.devices_by_id(REGISTRY)['gce-linux-x64-intel']
        try:
            image = executor.bake(device, 'script', '202610310900', log=lambda line: None, sleep=sleep,
                                  clock=lambda: clock[0])
        except gce_vm.BakeError as error:
            return calls, error
        return calls, image

    def test_bake_deletes_the_instance_only_after_the_image_is_done(self):
        calls, image = self.bake_calls('startup-script: + echo cerid-ci-bake-complete\ncerid-ci-bake-complete\n')
        self.assertEqual(image['family'], 'cerid-ci-ubuntu-2404-x64')
        order = [m + ' ' + ('operation' if '/global/operations/' in u else 'image' if u.endswith('/global/images')
                            else 'serial' if 'serialPort' in u else 'instance') for m, u in calls]
        self.assertLess(order.index('GET serial'), order.index('POST image'), 'the marker is read while it runs')
        self.assertEqual(order[order.index('POST image') - 1], 'GET instance', 'the image follows the power-off')
        self.assertLess(order.index('GET operation'), order.index('DELETE instance'),
                        'deleting the instance first removes the image source disk (RESOURCE_NOT_FOUND)')
        self.assertEqual(order[-1], 'DELETE instance')

    def test_bake_refuses_an_unfinished_provision_and_a_failed_image(self):
        calls, error = self.bake_calls('startup-script: apt-get failed\n')
        self.assertIsInstance(error, gce_vm.BakeError)
        self.assertIn('stays for inspection', str(error))
        self.assertFalse([u for m, u in calls if m == 'POST' and u.endswith('/global/images')], 'no image is made')
        self.assertFalse([m for m, _ in calls if m == 'DELETE'])
        calls, error = self.bake_calls('cerid-ci-bake-complete\n', operation_error={'errors': [
            {'code': 'RESOURCE_NOT_FOUND'}]})
        self.assertIsInstance(error, gce_vm.BakeError)
        self.assertIn('RESOURCE_NOT_FOUND', str(error))
        self.assertFalse([m for m, _ in calls if m == 'DELETE'], 'the instance stays when the image failed')

    def test_bake_stops_a_failed_provision_at_once(self):
        for line in ('cerid-ci-bake-failed: line 7 exited 100',
                     '[WARNING]: Script "windows-startup-script-ps1" failed with error: exit status 1'):
            calls, error = self.bake_calls('startup-script: apt-get update\n%s\n' % line)
            self.assertIsInstance(error, gce_vm.BakeError)
            self.assertIn(line[:30], str(error))
            stops = [i for i, (m, u) in enumerate(calls) if m == 'POST' and u.endswith('/stop')]
            self.assertEqual(len(stops), 1, 'the idle instance is stopped, not left running for hours')
            self.assertEqual(len([m for m, u in calls[:stops[0]] if m == 'GET' and 'serialPort' not in u]), 1)
            self.assertFalse([u for m, u in calls if u.endswith('/global/images')])

    def test_ssh_host_sends_the_token_on_standard_input_only(self):
        seen = {}

        def run(argv, input=None, **_):
            seen['argv'], seen['input'] = argv, input
            return subprocess.CompletedProcess(argv, 0, '', '')

        environ = {'CERID_SSH_AMD_DEVCLOUD_HOST': 'cerid@amd.example', 'CERID_SSH_AMD_DEVCLOUD_KEY_FILE': 'k',
                   'CERID_SSH_AMD_DEVCLOUD_KNOWN_HOSTS': 'kh'}
        executor = ssh_host.SshHost(environ, run=run)
        device = pipeline.devices_by_id(REGISTRY)['amd-developer-cloud-mi300x']
        self.assertEqual(executor.missing(device), [])
        executor.launch('cerid-ci-j-a1', device, dict(AGENT_SPEC, token='SECRET-TOKEN'))
        self.assertNotIn('SECRET-TOKEN', ' '.join(seen['argv']))
        self.assertIn('StrictHostKeyChecking=yes', seen['argv'])
        lines = seen['input'].split('\n')
        self.assertEqual(lines[:2], ['read -r token', 'SECRET-TOKEN'])
        self.assertIn(AGENT_SPEC['agent_sha256'], seen['input'])
        self.assertIn("'https://ci.example/agent/cerid-agent.py'", seen['input'], 'an SSH host uses the external URL')
        self.assertIn('--token-file', seen['input'])
        self.assertEqual(seen['input'].count('SECRET-TOKEN'), 1)
        with self.assertRaises(RuntimeError):
            executor.launch('m', device, AGENT_SPEC)
        self.assertEqual(ssh_host.SshHost({}).missing(device),
                         ['CERID_SSH_AMD_DEVCLOUD_HOST', 'CERID_SSH_AMD_DEVCLOUD_KEY_FILE',
                          'CERID_SSH_AMD_DEVCLOUD_KNOWN_HOSTS'])

    def test_firebase_command_quota_and_launch(self):
        launched = {}

        class Process:
            pid = 7

            class stdin:
                @staticmethod
                def write(data):
                    launched['stdin'] = data

                @staticmethod
                def close():
                    launched['closed'] = True

        def popen(argv, **kwargs):
            launched['argv'] = argv
            return Process()
        executor = firebase_test_lab.FirebaseTestLab({'CERID_FTL_PROJECT': 'ftl-project'}, popen=popen)
        device = pipeline.devices_by_id(REGISTRY)['ftl-pixel-phone']
        command = executor.test_command(device, 'app.apk')
        self.assertEqual(command[:5], ['gcloud', 'firebase', 'test', 'android', 'run'])
        self.assertIn('model=shiba,version=34,locale=en,orientation=portrait', command)
        ios = executor.test_command(pipeline.devices_by_id(REGISTRY)['ftl-iphone'], 'tests.zip', kind='xctest')
        self.assertEqual(ios[3], 'ios')
        executor.launch('cerid-ci-f-a1', device, dict(AGENT_SPEC, token='ONE-JOB'))
        self.assertNotIn('ONE-JOB', ' '.join(launched['argv']))
        self.assertEqual((launched['stdin'], launched['closed']), (b'ONE-JOB\n', True))
        self.assertIn('--token-stdin', launched['argv'])
        with tempfile.TemporaryDirectory() as scratch:
            store = pipeline.LocalStore(scratch)
            for i in range(5):
                self.assertEqual(executor.quota_left(store, device, today='2026-10-30'), 5 - i)
                executor.take_quota(store, device, 'j%d' % i, today='2026-10-30')
            self.assertEqual(executor.quota_left(store, device, today='2026-10-30'), 0)
            self.assertEqual(executor.quota_left(store, device, today='2026-10-31'), 5)

    def test_owned_agent_names_a_registration_token_file_never_a_token(self):
        device = pipeline.devices_by_id(REGISTRY)['owned-windows-x64-workstation']
        agent = owned_agent.OwnedAgent({})
        config = agent.config(device, 'https://ci.example')
        self.assertIn('--registration-token-file C:\\cerid-ci\\registration-token', config)
        self.assertIn('device=owned-windows-x64-workstation', config)
        self.assertIn('--mode long-running', config)
        self.assertTrue(any('agent-token create --name owned-windows-x64-workstation' in s
                            for s in agent.install(device)))
        with self.assertRaises(RuntimeError):
            agent.launch('j', device, AGENT_SPEC)


class Dispatch(unittest.TestCase):
    def test_a_budget_refusal_is_a_budget_cell_on_the_dashboard(self):
        from coordinator import dispatch, scheduler
        with tempfile.TemporaryDirectory() as scratch:
            store = pipeline.LocalStore(scratch)
            store.put_json('ledger/%s/old.json' % pipeline.month_of(), {'usd': 49.9})
            c = scheduler.Coordinator(store, {'repositories': {'cerid': {'url': 'u'}}},
                                      log=scheduler.JsonLog(io.StringIO()))
            build, _ = c.create_build('cerid', {'commit': 'a' * 40, 'branch': 'main'}, 'manual',
                                      document={'steps': [{'key': 'lane-win-debug', 'command': 'x', 'agents': {
                                          'queue': 'cerid-cloud', 'device': 'gce-windows-x64'}}]})
            launched = []
            executor = type('E', (), {'launch': lambda self, *a: launched.append(a) or {}})()
            dispatch.Dispatcher(c, REGISTRY, {'gce_vm': executor}).tick()
            self.assertEqual(launched, [])
            record = store.get_json('results/%d/lane-win-debug.json' % build['number'])
            self.assertEqual(record['outcome'], 'budget')
            cells, _ = dashboard.matrix(REGISTRY, [record])
            self.assertEqual(cells[('gce-windows-x64', 'build')]['state'], 'budget')


class Parity(unittest.TestCase):
    def test_every_hosted_job_and_step_has_a_cerid_step(self):
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
        environ = {'CERID_CI_GCP_PROJECT': 'very-secret-project-value', 'CERID_CI_URL': 'org-value'}
        rows = ops.accounts_report(REGISTRY, environ, set())
        text = json.dumps(rows)
        self.assertNotIn('very-secret-project-value', text)
        self.assertNotIn('org-value', text)
        states = {row[0]: row[3] for row in rows}
        self.assertEqual(states['gcp-project'], 'configured')
        self.assertEqual(states['coordinator'], 'configured')
        self.assertEqual(states['cloudflare-access'], 'missing (optional)')
        self.assertEqual(states['xbox-partner-center'], 'user program')
        self.assertEqual(states['webhook-secret'], 'missing (optional)')
        self.assertEqual(states['cloudflare-pages'], 'missing (optional)')
        self.assertFalse([name for name in states if name.startswith('buildkite')])

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
    def test_evidence_records_the_orchestrator_run_identity(self):
        environ = {'CERID_CI_BUILD_NUMBER': '12', 'CERID_CI_JOB_ID': 'j', 'CERID_CI_STEP_KEY': 'lane-win-debug',
                   'CERID_CI_RETRY_COUNT': '1', 'CERID_CI_COMMIT': 'abc', 'CERID_CI_DEVICE': 'gce-windows-x64',
                   'CERID_CI_AGENT_NAME': 'cerid-ci-x-a2'}
        for key in environ:
            self.assertIn(key, evidence.RUN_KEYS)
        self.assertFalse([key for key in evidence.RUN_KEYS if key.startswith('BUILDKITE')])

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
