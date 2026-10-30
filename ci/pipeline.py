#!/usr/bin/env python3
"""The Cerid CI pipeline: registry, affected selection, result cache, budget, lanes and the launcher (ADR-0135).

One program on every machine. On the launcher it plans a build and uploads the Buildkite pipeline (`upload`), runs the
dispatcher that turns scheduled cloud jobs into one-job machines (`dispatch`) and concludes the required result
(`conclude`). On a runner it executes one lane with the same phases as the hosted GitHub job it replaces (`lane`), or
the preflight and repository checks (`run`). Coverage stays owned by `.github/ci-tiers.json` through
`scripts/ci-tier.py`; the module graph is the `crd_module()` registry of the root CMakeLists.txt; nothing here
re-implements a helper the repository already has. Contract: docs/design/ci-platform.md.
"""
from __future__ import annotations

from pathlib import Path
import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
CI = ROOT / 'ci'
DEVICES = CI / 'devices.json'
TIERS = ROOT / '.github/ci-tiers.json'
PINS = ROOT / 'cmake/pins.json'
WORKFLOW = ROOT / '.github/workflows/ci.yml'
if str(CI) not in sys.path:
    sys.path.insert(0, str(CI))


def load_script(name, relative):
    """Import a repository script whose file name is not an identifier (scripts/ci-tier.py)."""
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ci_tier = load_script('ci_tier', 'scripts/ci-tier.py')

SCHEMA = 'cerid-ci-devices/1'
PLAN_SCHEMA = 'cerid-ci-plan/1'
RESULT_SCHEMA = 'cerid-ci-result/1'
KINDS = ('phone', 'tablet', 'desktop', 'laptop', 'browser', 'emulator', 'simulator', 'VM', 'cloud-GPU', 'console')
TIERS_ABC = ('A', 'B', 'C')
STATUSES = ('active', 'planned', 'waiting on platform program')
COST_CLASSES = ('free', 'free-quota', 'spot', 'paid', 'credit')
EXECUTORS = ('gce_vm', 'owned_agent', 'firebase_test_lab', 'ssh_host')
SCREEN_KINDS = ('phone', 'tablet', 'laptop')
CELL_STATES = ('pass', 'fail', 'cached', 'waiting', 'budget')
# The user's matrix (2026-10-30): every platform, CPU level, GPU vendor and API has at least one registry cell.
REQUIRED_COVERAGE = {
    'platforms': [('Windows', 'x86_64'), ('Windows', 'aarch64'), ('Ubuntu', 'x86_64'), ('Ubuntu', 'aarch64'),
                  ('macOS', 'aarch64'), ('iOS', 'aarch64'), ('iPadOS', 'aarch64'), ('Android phone', 'aarch64'),
                  ('Android tablet', 'aarch64'), ('Android emulator', 'x86_64')],
    'browsers': ['Chromium', 'Firefox', 'WebKit'],
    'levels': ['portable', 'sse2', 'sse4.2', 'avx2', 'avx512', 'neon', 'wasm-simd128'],
    'vendors': ['NVIDIA', 'AMD', 'Intel', 'Apple', 'Qualcomm Adreno', 'Arm Mali', 'Imagination PowerVR'],
    'apis': ['Vulkan', 'DX12', 'Metal', 'WebGPU', 'CUDA', 'HIP'],
    'consoles': ['Xbox', 'PlayStation 5', 'Nintendo'],
}
# Compiler-cached and cold-acquisition presets (ci.yml env CRD_CACHED_PRESETS / CRD_COLD_PRESETS; the parity test
# holds these equal to the workflow while both run).
CACHED_PRESETS = ('win-debug', 'win-asan', 'win-public-checks', 'win-clang-cl', 'win-clang-cl-shipping',
                  'linux-gcc-debug', 'linux-gcc-release', 'linux-gcc-relwithdebinfo', 'linux-gcc-asan',
                  'linux-gcc-public-checks', 'linux-clang-tsan', 'linux-gcc-shipping')
COLD_PRESETS = ('win-debug-scalar', 'linux-gcc-debug-scalar')
APT_PACKAGES = {
    'x64': ('libvulkan-dev', 'glslang-tools', 'libshaderc-dev', 'libwayland-dev', 'wayland-protocols',
            'libxkbcommon-dev', 'libx11-dev', 'libxrandr-dev', 'libxinerama-dev', 'libxcursor-dev', 'libxi-dev',
            'mesa-common-dev', 'mesa-vulkan-drivers', 'gdb'),
    'arm64': ('ninja-build', 'gdb', 'clang', 'libclang-rt-18-dev'),
    'genmc': ('llvm-20-dev', 'clang-20', 'g++-14', 'libedit-dev'),
}
GENMC_CENSUS = ('llvm-20-dev', 'clang-20', 'libllvm20', 'g++-14', 'libedit-dev', 'libffi-dev')
NATIVE_TESTS = ('^crd-(repository-hygiene|master-plan|ci-tiers|repository-tools|developer-workflow|'
                'project-structure-sync|build-profiles|module-selection|project-structure-native|native-build-profiles)$')
DX12_PROBE = '^crd-dx12-device-info$'
# Spot preemption and lost agents are infrastructure loss, not test outcomes: the job is retried on a new machine.
# A failed test is never retried (no retry-as-proof).
RETRY = {'automatic': [{'exit_status': -1, 'limit': 2}, {'signal_reason': 'agent_stop', 'limit': 2}]}
BUDGET_EXIT = 75
SKIP_LIMIT = 70  # Buildkite's skip reason length


# --------------------------------------------------------------------------------------------- registry and graph

def load_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def roadmap_ids(path=ROOT / 'docs/ROADMAP.md'):
    master = load_script('check_master_plan', 'scripts/check-master-plan.py')
    return [name for name, _ in master.parse_rows(Path(path).read_text(encoding='utf-8'))]


def load_registry(path=DEVICES, packs=None):
    """The device registry plus every console or platform pack's registry (ADR-0135 §8). A pack lives outside this
    repository (a private clone, default `external/platform-packs/<name>`, an ignored directory) and contributes
    devices and features through the same schema; a pack can never replace a public entry."""
    document = load_json(path)
    for pack in packs if packs is not None else pack_directories():
        extension = load_json(Path(pack) / 'devices.json')
        if extension.get('schema') != SCHEMA:
            raise ValueError('%s: pack registry schema must be %s' % (pack, SCHEMA))
        known = {device['id'] for device in document['devices']}
        for device in extension.get('devices', []):
            if device['id'] in known:
                raise ValueError('%s: pack device %s collides with a registry entry' % (pack, device['id']))
            device = dict(device)
            device['pack'] = Path(pack).name
            document['devices'].append(device)
        for name, feature in extension.get('features', {}).items():
            if name in document['features']:
                raise ValueError('%s: pack feature %s collides with a registry feature' % (pack, name))
            document['features'][name] = feature
    return document


def pack_directories(environ=None, root=ROOT):
    """Platform packs named by CERID_PLATFORM_PACKS (path list) or present under external/platform-packs; a pack
    inside the tracked tree is refused, because NDA material must never be committed here."""
    environ = os.environ if environ is None else environ
    listed = [p for p in environ.get('CERID_PLATFORM_PACKS', '').split(os.pathsep) if p]
    default = root / 'external/platform-packs'
    if not listed and default.is_dir():
        listed = [str(p) for p in sorted(default.iterdir()) if (p / 'devices.json').is_file()]
    packs = []
    for item in listed:
        path = Path(item).resolve()
        if tracked_inside(path, root):
            raise ValueError('platform pack %s is inside the tracked tree; packs live in their own private '
                             'repository (ADR-0135 §8)' % item)
        packs.append(path)
    return packs


def tracked_inside(path, root=ROOT):
    try:
        relative = path.relative_to(root.resolve())
    except ValueError:
        return False
    completed = subprocess.run(['git', '-C', str(root), 'ls-files', '--', relative.as_posix()], capture_output=True,
                               text=True, check=False)
    return bool(completed.stdout.strip())


def validate_registry(document, tiers, presets=None, rows=None):
    """Problems of the registry: shape, references, ownership of every ci-tiers job and the required coverage."""
    problems = []
    if document.get('schema') != SCHEMA:
        problems.append('schema must be %s' % SCHEMA)
    features = document.get('features', {})
    accounts = document.get('accounts', {})
    levels = set(document.get('cpu_levels', []))
    apis = set(document.get('apis', []))
    vendors = set(document.get('gpu_vendors', []))
    profiles = document.get('console_profiles', {})
    if not isinstance(document.get('budget', {}).get('monthly_cap_usd'), (int, float)):
        problems.append('budget.monthly_cap_usd must be a number')
    for name, feature in features.items():
        for row in feature.get('rows', []):
            if rows is not None and row not in rows:
                problems.append('feature %s names roadmap row %s, which does not exist' % (name, row))
        if feature.get('enforced_by') == 'engine' and not feature.get('option'):
            problems.append('feature %s is enforced by the engine but names no option and owning row' % name)
    seen = set()
    hosts = {}
    for device in document.get('devices', []):
        ident = device.get('id', '?')
        where = 'device %s' % ident
        if ident in seen:
            problems.append('%s is registered twice' % where)
        seen.add(ident)
        for field in ('status', 'kind', 'os', 'cpu', 'gpu', 'executor', 'tier', 'cost_class', 'cost_per_hour_usd',
                      'features', 'accounts', 'route'):
            if field not in device:
                problems.append('%s lacks %s' % (where, field))
        if device.get('status') not in STATUSES:
            problems.append('%s: status %r is not one of %s' % (where, device.get('status'), STATUSES))
        if device.get('kind') not in KINDS:
            problems.append('%s: kind %r is not one of %s' % (where, device.get('kind'), KINDS))
        if device.get('tier') not in TIERS_ABC:
            problems.append('%s: tier must be A, B or C' % where)
        if device.get('executor') not in EXECUTORS:
            problems.append('%s: executor %r is not one of %s' % (where, device.get('executor'), EXECUTORS))
        if device.get('cost_class') not in COST_CLASSES:
            problems.append('%s: cost class %r is not one of %s' % (where, device.get('cost_class'), COST_CLASSES))
        if device.get('kind') in SCREEN_KINDS:
            screen = device.get('screen') or {}
            if not screen.get('size_in') or not screen.get('density_dpi') or not screen.get('refresh_hz'):
                problems.append('%s: a %s cell names its screen size, density and refresh' % (where, device['kind']))
        for level in device.get('cpu', {}).get('levels', []):
            if level not in levels:
                problems.append('%s: CPU level %s is not a registry level' % (where, level))
        for api in device.get('gpu', {}).get('apis', []):
            if api not in apis:
                problems.append('%s: API %s is not a registry API' % (where, api))
        if device.get('gpu', {}).get('vendor') not in vendors:
            problems.append('%s: GPU vendor %r is not a registry vendor' % (where, device.get('gpu', {}).get('vendor')))
        for feature in device.get('features', []):
            if feature not in features:
                problems.append('%s: feature %s is not registered' % (where, feature))
        for feature, row in device.get('planned_features', {}).items():
            if feature not in device.get('features', []):
                problems.append('%s: planned feature %s is not one of its features' % (where, feature))
            if rows is not None and row not in rows:
                problems.append('%s: planned feature %s waits on row %s, which does not exist' % (where, feature, row))
        for account in device.get('accounts', []):
            if account not in accounts:
                problems.append('%s: account %s is not registered' % (where, account))
        for preset in device.get('lane_presets', []):
            if presets is not None and preset not in presets:
                problems.append('%s: lane preset %s is not a configure preset' % (where, preset))
            if preset not in tiers.get('presets', {}):
                problems.append('%s: lane preset %s has no owner in ci-tiers.json' % (where, preset))
        console = device.get('console_profile')
        if console:
            if console.get('profile') not in profiles:
                problems.append('%s: console profile %s is not registered' % (where, console.get('profile')))
            if console.get('preset') not in tiers.get('presets', {}):
                problems.append('%s: console preset %s has no owner in ci-tiers.json' % (where, console.get('preset')))
        if device.get('kind') == 'console' and device.get('status') == 'active' and not device.get('pack'):
            problems.append('%s: a console cell runs only through its private platform pack' % where)
        if device.get('executor') == 'gce_vm':
            config = device.get('executor_config', {})
            for field in ('machine_type', 'image_family', 'base_image', 'disk_gb', 'max_run_hours'):
                if field not in config:
                    problems.append('%s: gce_vm config lacks %s' % (where, field))
        for job in device.get('ci_jobs', []):
            hosts.setdefault(job, []).append(ident)
    for job in tiers.get('jobs', {}):
        owners = hosts.get(job, [])
        if len(owners) != 1:
            problems.append('ci-tiers job %s must be hosted by exactly one device, not %s' % (job, owners or 'none'))
    for job in hosts:
        if job not in tiers.get('jobs', {}):
            problems.append('device job %s is not a ci-tiers job' % job)
    problems += coverage_problems(document)
    return problems


def coverage_problems(document):
    devices = document.get('devices', [])
    problems = []

    def platform_key(device):
        name = device['os']['name']
        if name.startswith('Windows'):
            name = 'Windows'
        if name == 'Android':
            name = 'Android ' + ('emulator' if device['kind'] == 'emulator' else device['kind'])
        return name, device['cpu']['arch']

    platforms = {platform_key(d) for d in devices}
    for wanted in REQUIRED_COVERAGE['platforms']:
        if tuple(wanted) not in platforms:
            problems.append('coverage: no cell for %s on %s' % wanted)
    browsers = {b for d in devices for b in d.get('browsers', [])}
    for browser in REQUIRED_COVERAGE['browsers']:
        if browser not in browsers:
            problems.append('coverage: no browser cell for %s' % browser)
    levels = {lvl for d in devices for lvl in d['cpu']['levels']}
    for level in REQUIRED_COVERAGE['levels']:
        if level not in levels:
            problems.append('coverage: no cell runs CPU level %s' % level)
    vendors = {d['gpu']['vendor'] for d in devices}
    for vendor in REQUIRED_COVERAGE['vendors']:
        if vendor not in vendors:
            problems.append('coverage: no cell has a %s GPU' % vendor)
    apis = {a for d in devices for a in d['gpu']['apis']}
    for api in REQUIRED_COVERAGE['apis']:
        if api not in apis:
            problems.append('coverage: no cell exercises %s' % api)
    consoles = ' '.join(d['id'] + ' ' + d['os']['name'] for d in devices if d['kind'] == 'console')
    for console in REQUIRED_COVERAGE['consoles']:
        if console.lower().replace(' ', '-') not in consoles.lower().replace(' ', '-'):
            problems.append('coverage: no console cell for %s' % console)
    return problems


def devices_by_id(document):
    return {device['id']: device for device in document['devices']}


def job_hosts(document):
    return {job: device['id'] for device in document['devices'] for job in device.get('ci_jobs', [])}


MODULE_LISTS = ('EXECUTABLES', 'DEPENDS', 'PACKAGES', 'TESTS', 'TEST_DEPENDS', 'TEST_PACKAGES')


def parse_modules(cmake_text):
    """The `crd_module()` registry of the root CMakeLists.txt: {name: {source, depends, tests, test_depends}}."""
    text = re.sub(r'#[^\n]*', '', cmake_text)
    modules = {}
    for body in re.findall(r'\bcrd_module\(([^()]*)\)', text):
        tokens = body.split()
        if not tokens:
            continue
        entry = {'source': tokens[0], 'depends': [], 'tests': [], 'test_depends': []}
        name = tokens[0].rsplit('/', 1)[-1]
        current = None
        position = 1
        while position < len(tokens):
            token = tokens[position]
            if token == 'NAME':
                name = tokens[position + 1]
                position += 2
                current = None
                continue
            if token == 'HOST':
                current = None
            elif token in MODULE_LISTS:
                current = token.lower()
            elif current in entry:
                entry[current].append(token)
            position += 1
        modules[name] = entry
    return modules


def path_owners(path, modules):
    """Modules whose source or test directory holds `path` (a test directory may serve several modules)."""
    owners = set()
    for name, entry in modules.items():
        if path == entry['source'] or path.startswith(entry['source'] + '/'):
            owners.add(name)
        for test in entry['tests']:
            prefix = 'tests/' + test
            if path.startswith(prefix + '/'):
                owners.add(name)
    return owners


def dependency_closure(seeds, modules, with_tests=True):
    pending = list(seeds)
    seen = set()
    while pending:
        name = pending.pop()
        if name in seen or name not in modules:
            continue
        seen.add(name)
        pending += modules[name]['depends']
        if with_tests and name in seeds:
            pending += modules[name]['test_depends']
    return seen


def affected_modules(paths, modules, tiers):
    """(selection, reasons): the modules whose code or tests a change can reach, or None when the change is broader
    than the module graph (a build-system, asset, root or unowned path), so lanes build everything."""
    reverse = {name: set() for name in modules}
    for name, entry in modules.items():
        for dependency in entry['depends'] + entry['test_depends']:
            reverse.setdefault(dependency, set()).add(name)
    seeds = set()
    reasons = []
    for path, cls, _tier in ci_tier.classify(paths, tiers['classes']):
        if cls == 'documentation':
            continue
        if cls != 'source':
            reasons.append('%s: %s path' % (path, cls or 'unclassified'))
            continue
        owners = path_owners(path, modules)
        if not owners:
            reasons.append('%s: no crd_module() owns it' % path)
        seeds |= owners
    if reasons:
        return None, sorted(reasons)
    selection = set(seeds)
    pending = list(seeds)
    while pending:
        for user in reverse.get(pending.pop(), ()):
            if user not in selection:
                selection.add(user)
                pending.append(user)
    return sorted(selection), []


# --------------------------------------------------------------------------------------------- content keys

def tree_entries(revision='HEAD', root=ROOT):
    """[(path, blob id)] of a revision; content addressed, so a key never depends on timestamps or checkouts."""
    completed = subprocess.run(['git', '-C', str(root), 'ls-tree', '-r', '-z', '--full-tree', revision],
                               capture_output=True, check=True)
    entries = []
    for record in completed.stdout.decode('utf-8').split('\0'):
        if not record:
            continue
        meta, path = record.split('\t', 1)
        entries.append((path, meta.split()[2]))
    return entries


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False)


def input_key(entries, tiers, kind, extra, modules=None, selection=None, include_docs=False):
    """SHA-256 over the blobs a step consumes plus the step identity. Documentation never enters a build key; a
    narrowed lane hashes only its module closure and every path no module owns (build system, scripts, assets)."""
    owned_prefixes = None
    if selection is not None and modules is not None:
        closure = dependency_closure(selection, modules)
        owned_prefixes = []
        allowed = []
        for name, entry in modules.items():
            prefixes = [entry['source'] + '/'] + ['tests/%s/' % t for t in entry['tests']]
            owned_prefixes += prefixes
            if name in closure:
                allowed += prefixes
    classes = {path: cls for path, cls, _ in ci_tier.classify([p for p, _ in entries], tiers['classes'])}
    digest = hashlib.sha256()
    digest.update(canonical({'kind': kind, 'extra': extra, 'selection': selection}).encode('utf-8'))
    for path, blob in sorted(entries):
        if not include_docs and classes.get(path) == 'documentation':
            continue
        if owned_prefixes is not None and any(path.startswith(p) for p in owned_prefixes):
            if not any(path.startswith(p) for p in allowed):
                continue
        digest.update(('%s\0%s\n' % (path, blob)).encode('utf-8'))
    return digest.hexdigest()


def device_fingerprint(device, store=None):
    """The device entry plus the exact baked image of its family: a rebaked image changes every key on it."""
    family = device.get('executor_config', {}).get('image_family')
    image = store.get_json('images/%s.json' % family) if store and family else None
    return {'device': device, 'image': (image or {}).get('image')}


# --------------------------------------------------------------------------------------------- result store

class LocalStore:
    """The result store on a local or mounted directory (an owned machine, tests)."""

    def __init__(self, root):
        self.root = Path(root)

    def _path(self, name):
        path = (self.root / name).resolve()
        if not path.is_relative_to(self.root.resolve()):
            raise ValueError('store path escapes the store: %s' % name)
        return path

    def get(self, name):
        path = self._path(name)
        return path.read_bytes() if path.is_file() else None

    def put(self, name, data):
        path = self._path(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        partial = path.with_name(path.name + '.partial')
        partial.write_bytes(data)
        os.replace(partial, path)

    def list(self, prefix):
        base = self._path(prefix) if prefix else self.root
        if not base.exists():
            return []
        if base.is_file():
            return [prefix]
        return sorted(p.relative_to(self.root).as_posix() for p in base.rglob('*')
                      if p.is_file() and not p.name.endswith('.partial'))

    def get_json(self, name):
        data = self.get(name)
        return json.loads(data.decode('utf-8')) if data is not None else None

    def put_json(self, name, value):
        self.put(name, (json.dumps(value, indent=1, sort_keys=True) + '\n').encode('utf-8'))


class GcsStore(LocalStore):
    """The result store on a Cloud Storage bucket through the JSON API with the machine's own credentials."""

    def __init__(self, bucket, prefix='', token=None, http=None):
        from executors import gce_vm
        self.bucket = bucket
        self.prefix = prefix.strip('/') + '/' if prefix.strip('/') else ''
        self.token = token or gce_vm.access_token
        self.http = http or gce_vm.http_request

    def get(self, name):
        url = 'https://storage.googleapis.com/storage/v1/b/%s/o/%s?alt=media' % (
            self.bucket, urllib.parse.quote(self.prefix + name, safe=''))
        status, body = self.http('GET', url, token=self.token(), raw=True, allow=(404,))
        return None if status == 404 else body

    def put(self, name, data):
        url = 'https://storage.googleapis.com/upload/storage/v1/b/%s/o?uploadType=media&name=%s' % (
            self.bucket, urllib.parse.quote(self.prefix + name, safe=''))
        self.http('POST', url, token=self.token(), data=data, content_type='application/octet-stream')

    def list(self, prefix):
        names = []
        page = None
        while True:
            query = {'prefix': self.prefix + prefix, 'fields': 'items(name),nextPageToken'}
            if page:
                query['pageToken'] = page
            url = 'https://storage.googleapis.com/storage/v1/b/%s/o?%s' % (self.bucket, urllib.parse.urlencode(query))
            _, listing = self.http('GET', url, token=self.token())
            names += [item['name'][len(self.prefix):] for item in listing.get('items', [])]
            page = listing.get('nextPageToken')
            if not page:
                return sorted(names)


def open_store(url=None):
    url = url or os.environ.get('CERID_CI_STORE') or str(ROOT / 'build/ci-store')
    if url.startswith('gs://'):
        bucket, _, prefix = url[5:].partition('/')
        return GcsStore(bucket, prefix)
    if url.startswith('file://'):
        url = urllib.parse.unquote(urllib.parse.urlparse(url).path.lstrip('/') if os.name == 'nt'
                                   else urllib.parse.urlparse(url).path)
    return LocalStore(url)


def cached_pass(store, key):
    entry = store.get_json('cache/%s.json' % key) if store else None
    return entry if entry and entry.get('outcome') == 'pass' else None


# --------------------------------------------------------------------------------------------- budget

def month_of(when=None):
    return (when or dt.datetime.now(dt.timezone.utc)).strftime('%Y-%m')


def month_spend(store, month):
    """Recorded machine time of the month in USD: one ledger entry per launched machine (the dispatcher's truth),
    never an estimate."""
    total = 0.0
    for name in store.list('ledger/%s/' % month):
        entry = store.get_json(name) or {}
        total += float(entry.get('usd', 0.0))
    return round(total, 4)


def gcp_budget_stop(store, month):
    """True when the last Cloud Billing budget notification of the month reports spend at or above the budget."""
    note = store.get_json('budget/gcp-%s.json' % month) if store else None
    if not note:
        return False
    return float(note.get('costAmount', 0.0)) >= float(note.get('budgetAmount', 0.0)) > 0.0


def estimate_minutes(store, step_id, default):
    record = store.get_json('durations/%s.json' % step_id) if store else None
    samples = (record or {}).get('minutes', [])
    if not samples:
        return float(default)
    ordered = sorted(samples[-5:])
    return float(ordered[len(ordered) // 2])


def apply_budget(steps, cap, spent, stop=False):
    """Mark paid steps that would pass the cap as `budget`, cheapest priority first; free steps always run. A
    budget cell is never reported as a pass."""
    remaining = cap - spent
    for step in sorted((s for s in steps if s['state'] == 'run'), key=lambda s: (s['priority'], s['order'])):
        cost = step['est_usd']
        if cost <= 0.0:
            continue
        if stop:
            step['state'] = 'budget'
            step['reason'] = 'budget: the GCP budget stop is in force'
        elif cost > remaining:
            step['state'] = 'budget'
            step['reason'] = 'budget: %.2f of %.2f USD spent this month' % (cap - remaining, cap)
        else:
            remaining -= cost
    return round(cap - remaining, 4)


# --------------------------------------------------------------------------------------------- planning

def preset_features(preset, job, tiers):
    features = ['build']
    if job != 'clang-tidy':
        features.append('unit')
    if job == 'clang-tidy':
        features.append('static-analysis')
    if 'asan' in preset or 'tsan' in preset:
        features.append('sanitizers')
    for step, spec in tiers.get('preset_steps', {}).items():
        presets = spec.get('presets') or [spec.get('preset')]
        if preset in presets:
            features.append({'Test at the sse2 level': 'cpu-levels', 'Install pinned Intel SDE': 'cpu-levels',
                             'Seeded detector failures': 'seeded-faults',
                             'GenMC weak-memory model': 'weak-memory'}.get(step, 'unit'))
    if 'arm64' in preset:
        features.append('cpu-levels')
    if job in ('windows', 'clang-cl', 'windows-shipping', 'windows-clang-cl-shipping'):
        features.append('dx12')
    if job in ('linux-gcc', 'linux-gcc-shipping', 'windows', 'windows-shipping'):
        features.append('vulkan')
    return sorted(set(features))


def full_scope_presets(tiers):
    """Presets carrying a preset step that needs the whole tree (seeded detector seeds, the GenMC model): such a lane
    is never narrowed, so a source push keeps exactly the hosted obligations."""
    full = set()
    for step, spec in tiers.get('preset_steps', {}).items():
        if step in ('Seeded detector failures', 'GenMC weak-memory model'):
            full.update(spec.get('presets') or [spec.get('preset')])
    return full


def job_of(preset, tiers):
    entry = tiers['presets'][preset]
    return entry.get('job')


def agents_for(device, queues):
    queue = queues['owned'] if device['executor'] == 'owned_agent' else queues['cloud']
    return {'queue': queue, 'device': device['id']}


def account_present(account, spec, environ, secrets=None):
    """Whether an account-backed input exists, by name only; a value is never read or printed."""
    kind = spec.get('kind')
    if kind == 'env':
        return bool(environ.get(spec['name']))
    if kind == 'secret':
        if spec.get('env_file') and environ.get(spec['env_file']) and Path(environ[spec['env_file']]).is_file():
            return True
        return bool(secrets and spec['name'] in secrets)
    if kind == 'agent':
        return bool(environ.get('CERID_CI_OWNED_AGENTS')) or bool(secrets and 'owned-agents' in secrets)
    if kind == 'program':
        return False
    return False


def missing_accounts(device, registry, environ, secrets=None):
    accounts = registry.get('accounts', {})
    return [name for name in device.get('accounts', [])
            if not accounts.get(name, {}).get('optional') and not account_present(name, accounts.get(name, {}),
                                                                                   environ, secrets)]


def step_cost(device, minutes):
    return round(float(device.get('cost_per_hour_usd', 0.0)) * minutes / 60.0, 4)


def make_step(order, key, label, kind, device, command, features, priority, minutes, store, key_hash,
              preset=None, job=None, selection=None, timeout=None, fresh=False):
    est = estimate_minutes(store, key, minutes)
    step = {'order': order, 'key': key, 'label': label, 'kind': kind, 'device': device['id'], 'preset': preset,
            'job': job, 'command': command, 'features': features, 'priority': priority, 'est_minutes': est,
            'est_usd': step_cost(device, est), 'input_key': key_hash, 'selection': selection, 'state': 'run',
            'reason': '', 'timeout': timeout or int(device.get('executor_config', {}).get('max_run_hours', 6) * 60),
            'tier': device['tier']}
    hit = None if fresh else cached_pass(store, key_hash)
    if hit:
        step['state'] = 'cached'
        step['reason'] = ('cached: passed in build %s' % hit.get('build', '?'))[:SKIP_LIMIT]
        step['cached_from'] = hit
    return step


def plan(event, base='', head='HEAD', requested=None, paths=None, registry=None, tiers=None, modules=None,
         entries=None, store=None, environ=None, secrets=None, fresh=False, month=None):
    """The build plan: tier, selection, every step with its cache and budget state, and every waiting cell."""
    environ = os.environ if environ is None else environ
    registry = registry or load_registry()
    tiers = tiers or ci_tier.load_tiers(TIERS)
    modules = modules if modules is not None else parse_modules((ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'))
    entries = entries if entries is not None else tree_entries(head)
    store = store or open_store()
    month = month or month_of()
    record = ci_tier.resolve(tiers, event, base, head, requested, paths)
    devices = devices_by_id(registry)
    hosts = job_hosts(registry)
    queues = registry['queues']
    defaults = registry['budget']['default_minutes']
    selection, narrow_reasons = (None, ['%s tier' % record['tier']])
    if record['tier'] == 'change':
        changed = [path for path, _, _ in record['classified']] or (paths or [])
        selection, narrow_reasons = affected_modules(changed, modules, tiers)
    full_presets = full_scope_presets(tiers)
    steps = []
    cells = []

    def runnable(device):
        if device['status'] != 'active':
            return 'waiting: %s' % device['status']
        missing = missing_accounts(device, registry, environ, secrets)
        if missing:
            return 'waiting: account %s' % ', '.join(missing)
        return None

    order = 0
    for device in registry['devices']:
        host = device.get('repository_host')
        if not host:
            continue
        order += 1
        key = 'repository-%s' % host
        hashed = input_key(entries, tiers, 'repository', {'host': host, **device_fingerprint(device, store)},
                           include_docs=True)
        why = runnable(device)
        if why:
            cells.append({'device': device['id'], 'features': ['repository'], 'state': 'waiting', 'reason': why})
            continue
        steps.append(make_step(order, key, ':broom: Repository checks (%s)' % host, 'repository', device,
                               'python ci/pipeline.py run repository', ['repository'], 0,
                               defaults['repository'], store, hashed, fresh=fresh))
    for job, presets in record['jobs'].items():
        device = devices[hosts[job]]
        why = runnable(device)
        for preset in presets:
            order += 1
            narrowed = None if preset in full_presets else selection
            hashed = input_key(entries, tiers, 'lane', {'preset': preset, **device_fingerprint(device, store)},
                               modules=modules, selection=narrowed)
            features = preset_features(preset, job, tiers)
            if why:
                cells.append({'device': device['id'], 'preset': preset, 'features': features, 'state': 'waiting',
                              'reason': why})
                continue
            command = 'python ci/pipeline.py lane --preset %s --device %s' % (preset, device['id'])
            if narrowed is not None:
                command += ' --modules %s' % ';'.join(narrowed)
            tier_priority = 1 if tiers['presets'][preset]['tier'] == 'change' else 2
            steps.append(make_step(order, 'lane-%s' % preset, ':hammer: %s' % preset, 'lane', device, command,
                                   features, tier_priority, defaults['lane'], store, hashed, preset=preset, job=job,
                                   selection=narrowed, fresh=fresh))
    complete = record['tier'] == 'complete'
    for device in registry['devices']:
        why = runnable(device)
        planned = device.get('planned_features', {})
        for feature, row in planned.items():
            cells.append({'device': device['id'], 'features': [feature], 'state': 'waiting',
                          'reason': 'waiting: row %s' % row})
        extra = []
        for preset in device.get('lane_presets', []):
            extra.append(('lane-%s@%s' % (preset, device['id']), ':gpu: %s on %s' % (preset, device['id']), 'lane',
                          'python ci/pipeline.py lane --preset %s --device %s' % (preset, device['id']),
                          [f for f in device['features'] if f not in planned], preset))
        console = device.get('console_profile')
        if console:
            extra.append(('console-%s' % device['id'], ':video_game: console profile %s' % console['profile'],
                          'console-profile', 'python ci/pipeline.py lane --preset %s --device %s --console %s' % (
                              console['preset'], device['id'], console['profile']),
                          [f for f in device['features']
                           if registry['features'][f].get('enforced_by') != 'engine' and f not in planned],
                          console['preset']))
            for feature in device['features']:
                spec = registry['features'][feature]
                if spec.get('enforced_by') == 'engine':
                    cells.append({'device': device['id'], 'features': [feature], 'state': 'waiting',
                                  'reason': 'waiting: %s' % spec['option']})
        for feature, command in device.get('device_commands', {}).items():
            if feature in planned:
                continue
            extra.append(('device-%s-%s' % (device['id'], feature), ':satellite: %s on %s' % (feature, device['id']),
                          'device', command + ' --device %s' % device['id'], [feature], None))
        for key, label, kind, command, features, preset in extra:
            if why or not complete:
                cells.append({'device': device['id'], 'preset': preset, 'features': features, 'state': 'waiting',
                              'reason': why or 'waiting: complete tier'})
                continue
            order += 1
            hashed = input_key(entries, tiers, kind, {'key': key, **device_fingerprint(device, store)})
            priority = 3 if device['tier'] == 'C' else 2
            steps.append(make_step(order, key, label, kind, device, command, features, priority,
                                   defaults.get(kind, defaults['device']), store, hashed, preset=preset,
                                   job=job_of(preset, tiers) if preset else None, fresh=fresh))
        if device['status'] != 'active' and not planned and not device.get('lane_presets') \
                and not device.get('console_profile'):
            cells.append({'device': device['id'], 'features': device['features'], 'state': 'waiting',
                          'reason': 'waiting: %s' % device['status']})
    cap = float(registry['budget']['monthly_cap_usd'])
    spent = month_spend(store, month)
    stop = gcp_budget_stop(store, month)
    committed = apply_budget(steps, cap, spent, stop)
    for step in steps:
        step['agents'] = agents_for(devices[step['device']], queues)
    return {'schema': PLAN_SCHEMA, 'event': event, 'base': base, 'head': head, 'tier': record['tier'],
            'reason': record['reason'], 'selection': selection, 'narrowing': narrow_reasons,
            'budget': {'cap_usd': cap, 'spent_usd': spent, 'planned_usd': round(committed - spent, 4),
                       'gcp_stop': stop, 'month': month},
            'steps': steps, 'cells': cells}


def buildkite_pipeline(planned, queues):
    """The Buildkite pipeline document (JSON, which `buildkite-agent pipeline upload` accepts as YAML)."""
    steps = []
    keys = []
    for step in planned['steps']:
        entry = {'label': step['label'], 'key': step['key'], 'command': step['command'], 'agents': step['agents'],
                 'timeout_in_minutes': step['timeout'], 'retry': RETRY,
                 'env': {'CERID_CI_STEP': step['key'], 'CERID_CI_DEVICE': step['device'],
                         'CERID_CI_INPUT_KEY': step['input_key'], 'CERID_CI_TIER': planned['tier'],
                         'CERID_CI_FEATURES': ','.join(step['features'])},
                 'artifact_paths': ['build/*/evidence/**/*', 'build/**/Testing/Temporary/*.log',
                                    'build/**/Testing/Temporary/*.xml']}
        if step['state'] in ('cached', 'budget'):
            entry['skip'] = step['reason'][:SKIP_LIMIT]
        steps.append(entry)
        keys.append(step['key'])
    launcher = {'queue': queues['launcher']}
    steps.append({'label': ':white_check_mark: Required result', 'key': 'required', 'agents': launcher,
                  'command': 'python ci/pipeline.py conclude --plan ci-plan.json',
                  'depends_on': [{'step': key, 'allow_failure': True} for key in keys], 'allow_dependency_failure': True})
    steps.append({'label': ':bar_chart: QUAL dashboard', 'key': 'dashboard', 'agents': launcher,
                  'command': 'python ci/dashboard/generate.py publish --plan ci-plan.json',
                  'depends_on': [{'step': 'required', 'allow_failure': True}], 'allow_dependency_failure': True})
    return {'env': {'PYTHONUTF8': '1', 'CRD_PERF_BUDGET_SOFT': '1', 'CRD_PLATFORM_HEADLESS': '1'}, 'steps': steps}


# --------------------------------------------------------------------------------------------- Buildkite context

def buildkite_event(environ):
    """(event, base, requested tier) of a Buildkite build in ci-tier.py's vocabulary."""
    source = environ.get('BUILDKITE_SOURCE', '')
    requested = environ.get('CERID_CI_TIER') or None
    if source == 'schedule':
        return 'schedule', '', None
    if requested and source in ('ui', 'api', 'trigger_job'):
        return 'workflow_dispatch', '', requested
    pull = environ.get('BUILDKITE_PULL_REQUEST', 'false')
    if pull not in ('', 'false'):
        base_branch = environ.get('BUILDKITE_PULL_REQUEST_BASE_BRANCH') or 'main'
        return 'pull_request', 'origin/' + base_branch, None
    return 'push', environ.get('CERID_CI_BASE', ''), None


def push_base(store, branch):
    """The head of the newest earlier build on the branch whose plan succeeded (ci-tier.py base's rule without a
    hosting API): a replaced or cancelled build can never drop its changes from the next tier decision."""
    record = store.get_json('heads/%s.json' % branch.replace('/', '__')) if store else None
    return (record or {}).get('head', '')


# --------------------------------------------------------------------------------------------- lanes

class Runner:
    """Executes lane commands; tests substitute a recorder."""

    def __init__(self, environ=None, dry=False, log=print):
        self.environ = dict(os.environ if environ is None else environ)
        self.dry = dry
        self.log = log
        self.commands = []

    def __call__(self, argv, check=True, stdout=None, shell=False, env=None):
        merged = dict(self.environ)
        merged.update(env or {})
        self.commands.append({'argv': argv, 'env': env or {}})
        self.log('+ ' + (argv if isinstance(argv, str) else ' '.join(shlex.quote(str(a)) for a in argv)))
        if self.dry:
            return 0
        with (open(stdout, 'w', encoding='utf-8') if stdout else open(os.devnull, 'w')) as sink:
            completed = subprocess.run(argv, env=merged, shell=shell, cwd=str(ROOT),
                                       stdout=sink if stdout else None, check=False)
        if check and completed.returncode != 0:
            raise LaneFailure(argv, completed.returncode)
        return completed.returncode


class LaneFailure(RuntimeError):
    def __init__(self, argv, code):
        super().__init__('%s exited %d' % (argv if isinstance(argv, str) else ' '.join(map(str, argv)), code))
        self.code = code


def apply_env_file(path, environ):
    """Merge a $GITHUB_ENV-format file (NAME=value, or NAME<<DELIM ... DELIM) written by a repository helper."""
    path = Path(path)
    if not path.is_file():
        return
    lines = path.read_text(encoding='utf-8-sig').splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        index += 1
        if '<<' in line and '=' not in line.split('<<', 1)[0]:
            name, delimiter = line.split('<<', 1)
            value = []
            while index < len(lines) and lines[index] != delimiter:
                value.append(lines[index])
                index += 1
            index += 1
            environ[name] = '\n'.join(value)
        elif '=' in line:
            name, value = line.split('=', 1)
            environ[name] = value
    path.write_text('', encoding='utf-8')


def apply_path_file(path, environ):
    path = Path(path)
    if not path.is_file():
        return
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if line.strip():
            environ['PATH'] = line.strip() + os.pathsep + environ.get('PATH', '')
    path.write_text('', encoding='utf-8')


def tool_pin(name):
    return load_json(PINS)['tools'][name]


class Lane:
    """One lane: a ci-tiers preset on its device, phase for phase the hosted job it replaces (parity table in
    docs/design/ci-platform.md, held by scripts/test-ci-platform.py)."""

    def __init__(self, preset, device, tiers=None, run=None, temp=None, modules=None, console=None, registry=None,
                 store=None):
        self.preset = preset
        self.device = device
        self.tiers = tiers or ci_tier.load_tiers(TIERS)
        self.job = self.tiers['presets'][preset]['job']
        self.run = run or Runner()
        self.env = self.run.environ
        self.temp = Path(temp or tempfile.mkdtemp(prefix='cerid-lane-'))
        self.modules = modules
        self.console = console
        self.registry = registry
        self.store = store
        self.windows = self.job.startswith('windows') or self.job in ('clang-tidy', 'clang-cl')
        self.build_dir = 'build/win-vs-debug' if self.job == 'windows-native' else 'build/' + preset
        self.env_file = self.temp / 'env.txt'
        self.path_file = self.temp / 'path.txt'
        self.status = 'success'
        self.test_preset = 'win-vs-debug' if self.job == 'windows-native' else preset
        self.env.setdefault('CPM_SOURCE_CACHE', str(ROOT / '.cpm-cache'))
        self.env['CRD_PLATFORM_HEADLESS'] = '1'
        self.env['CRD_PERF_BUDGET_SOFT'] = '1'
        self.env['PYTHONUTF8'] = '1'

    # Conditions -----------------------------------------------------------------------------------------------
    def cached(self):
        return self.preset in CACHED_PRESETS

    def cold(self):
        return self.preset in COLD_PRESETS

    def preset_step(self, name):
        spec = self.tiers.get('preset_steps', {}).get(name, {})
        return self.preset in (spec.get('presets') or [spec.get('preset')])

    def python(self):
        return [sys.executable]

    def helper(self, script, *args):
        return self.python() + ['scripts/' + script] + [str(a) for a in args]

    def merge(self):
        apply_env_file(self.env_file, self.env)
        apply_path_file(self.path_file, self.env)

    # Phases -----------------------------------------------------------------------------------------------------
    def checkout(self):
        commit = self.env.get('BUILDKITE_COMMIT')
        self.run(['git', 'rev-parse', '--verify', 'HEAD'])
        if commit and not self.run.dry:
            head = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                                  check=True).stdout.strip()
            if not head.startswith(commit) and commit != 'HEAD':
                raise LaneFailure(['checkout'], 1)

    def python_version(self):
        if sys.version_info < (3, 12):
            raise LaneFailure(['python', '>=3.12'], 1)
        self.run(self.python() + ['--version'])

    def reclaim_disk(self):
        self.run(['df', '-h', '/'], check=False)

    def ninja(self):
        self.run(self.helper('install-ninja.py', '--destination', self.temp / 'ninja', '--github-path',
                             self.path_file))
        self.merge()

    def msvc(self):
        args = ['--arch', 'arm64'] if self.job == 'windows-arm64' else []
        self.run(self.helper('msvc-dev-env.py', *args, '--github-env', self.env_file))
        self.merge()

    def adapters(self):
        self.run(['powershell', '-NoProfile', '-Command',
                  'Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, PNPDeviceID | '
                  'Format-List | Out-File -Encoding utf8 -FilePath "%s"' % (self.temp / 'adapters.txt')])

    def cpm_cache(self):
        if self.cold():
            return
        key = 'cpm-%s-%s-%s' % ('Windows' if self.windows else 'Linux', self.preset, files_hash(
            ['CMakeLists.txt', 'CMakePresets.json', 'cmake/CPM.cmake', 'cmake/CrdPins.cmake', 'cmake/pins.json']))
        self.cpm_key = key
        if self.store is not None and not self.run.dry:
            restore_tree(self.store, 'blobs/cpm/%s.tar' % key, Path(self.env['CPM_SOURCE_CACHE']))

    def cpm_save(self):
        if self.cold() or self.store is None or self.run.dry or not hasattr(self, 'cpm_key'):
            return
        save_tree(self.store, 'blobs/cpm/%s.tar' % self.cpm_key, Path(self.env['CPM_SOURCE_CACHE']))

    def vulkan_sdk(self):
        pin = tool_pin('vulkan-sdk-windows')
        root = Path(pin.get('install_root', 'C:/VulkanSDK/' + pin['version']))
        if self.run.dry or not root.is_dir() or self.cold():
            self.run(self.helper('install-vulkan-sdk.py', '--download-dir', self.temp))
        self.env['VULKAN_SDK'] = str(root)
        self.env['PATH'] = str(root / 'Bin') + os.pathsep + self.env.get('PATH', '')

    def cold_before(self):
        if not self.cold():
            return
        self.run(self.helper('check-cold-acquisition.py', '--before', self.env['CPM_SOURCE_CACHE']))
        if self.windows:
            self.run(self.helper('install-warp.py', '--destination', self.temp / 'warp-cold'))

    def sccache(self):
        if not self.cached():
            return
        self.run(self.helper('install-sccache.py', '--destination', self.temp / 'sccache'))
        self.sccache_exe = self.temp / 'sccache' / ('sccache.exe' if self.windows else 'sccache')
        settings = {'SCCACHE_IDLE_TIMEOUT': '0', 'SCCACHE_IGNORE_SERVER_IO_ERROR': '1'}
        bucket = self.env.get('CERID_CI_SCCACHE_BUCKET')
        if bucket:
            settings.update({'SCCACHE_GCS_BUCKET': bucket, 'SCCACHE_GCS_RW_MODE': 'READ_WRITE',
                             'SCCACHE_GCS_KEY_PREFIX': '%s-%s' % (self.preset, self.device['executor_config'].get(
                                 'image_family', self.device['id']))})
        self.env.update(settings)
        if self.run([str(self.sccache_exe), '--start-server'], check=False) != 0:
            for name in ('SCCACHE_GCS_BUCKET', 'SCCACHE_GCS_RW_MODE', 'SCCACHE_GCS_KEY_PREFIX'):
                self.env.pop(name, None)
            self.run.log('sccache did not start on its bucket; the lane builds through an empty local cache')

    def sde(self):
        if not self.preset_step('Install pinned Intel SDE'):
            return
        self.run(self.helper('install-sde.py', '--destination', self.temp / 'sde', '--download-dir', self.temp,
                             '--github-env', self.env_file))
        self.merge()
        if not self.windows:
            self.run(['sudo', 'sysctl', '-w', 'kernel.yama.ptrace_scope=0'])

    def apt(self):
        packages = APT_PACKAGES['arm64' if self.job == 'linux-arm64' else 'x64']
        missing = [] if self.run.dry else [p for p in packages if subprocess.run(
            ['dpkg-query', '-W', p], capture_output=True, check=False).returncode != 0]
        if missing or self.run.dry:
            self.run(['sudo', 'apt-get', 'update', '-q'])
            self.run(['sudo', 'apt-get', 'install', '-y', *packages])

    def apt_census(self):
        packages = APT_PACKAGES['arm64' if self.job == 'linux-arm64' else 'x64']
        self.run(['dpkg-query', '-W', '-f=${Package} ${Version}\\n', *packages], stdout=self.temp / 'packages.txt')

    def vulkan_headers(self):
        self.run(self.helper('install-vulkan-headers.py', '--destination', '/tmp/vulkan-sdk', '--download-dir',
                             self.temp / 'vulkan-headers'))
        self.env['VULKAN_SDK'] = '/tmp/vulkan-sdk'

    def vulkan_validation(self):
        target = self.temp / 'cerid-vulkan-validation'
        self.run(self.helper('install-vulkan-validation.py', '--destination', target))
        self.env['VK_LAYER_PATH'] = str(target / 'share/vulkan/explicit_layer.d')
        self.env['LD_LIBRARY_PATH'] = str(target / 'lib') + (':' + self.env['LD_LIBRARY_PATH']
                                                             if self.env.get('LD_LIBRARY_PATH') else '')

    def clang_tidy(self):
        self.run(self.helper('install-clang-tidy.py', '--destination', self.temp / 'clang-tidy', '--download-dir',
                             self.temp, '--github-env', self.env_file))
        self.merge()
        if not self.run.dry:
            version = subprocess.run([self.env['CRD_CI_TIDY'], '--version'], capture_output=True, text=True,
                                     check=False).stdout
            if not re.search(r'\b%s\b' % re.escape(tool_pin('llvm')['version']), version):
                raise LaneFailure(['clang-tidy --version'], 1)

    def llvm_path(self):
        candidates = sorted(Path('C:/Program Files/Microsoft Visual Studio').glob('*/*/VC/Tools/Llvm/x64/bin'))
        candidates.append(Path('C:/Program Files/LLVM/bin'))
        found = next((c for c in candidates if (c / 'clang-cl.exe').is_file()), None)
        if found is None:
            which = shutil.which('clang-cl')
            found = Path(which).parent if which else None
        if found is None and not self.run.dry:
            raise LaneFailure(['clang-cl not found'], 1)
        if found:
            self.env['PATH'] = str(found) + os.pathsep + self.env.get('PATH', '')

    def configure(self):
        if self.job == 'windows-native':
            self.run(self.helper('project-sync.py', 'configure', '--build', self.build_dir, '--preset', self.preset))
            return
        command = ['cmake', '--preset', self.preset]
        if self.job == 'clang-tidy':
            command += ['-DCRD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON', '-DCLANG_TIDY_EXE=%s' % self.env.get('CRD_CI_TIDY', '')]
        if self.cached() and getattr(self, 'sccache_exe', None):
            command += ['-DCRD_COMPILER_LAUNCHER=%s' % self.sccache_exe.as_posix(), '-DCRD_ENABLE_PCH=OFF']
        if self.modules:
            command += ['-DCRD_MODULES=%s' % ';'.join(self.modules)]
        self.run(command)

    def cold_after(self):
        if not self.cold():
            return
        self.run(self.helper('check-cold-acquisition.py', '--after', self.env['CPM_SOURCE_CACHE'], '--build',
                             self.build_dir, '--fetch-unselected', self.temp / 'cold-unselected'))

    def build(self):
        if self.job == 'windows-native':
            self.run(['cmake', '--build', '--preset', 'win-vs-debug'])
        else:
            self.run(['cmake', '--build', '--preset', self.preset, '--', '-k', '0'])

    def sccache_stats(self):
        if not self.cached() or not hasattr(self, 'sccache_exe'):
            return
        exe = str(self.sccache_exe)
        self.run([exe, '--show-stats', '--stats-format', 'json'], check=False, stdout=self.temp / 'sccache.json')
        self.run([exe, '--show-stats'], check=False)
        self.run([exe, '--stop-server'], check=False)

    def inventory(self):
        args = ['--config', 'Debug'] if self.job == 'windows-native' else []
        self.run(self.helper('ci-evidence.py', 'inventory', '--build-dir', self.build_dir, *args))

    def junit(self, name='ctest-junit.xml'):
        return str(ROOT / self.build_dir / 'Testing/Temporary' / name)

    def ctest(self, *args):
        return self.console_wrap(['ctest', '--preset', self.test_preset, *args])

    def test(self):
        probe = 0
        if self.job in ('windows', 'clang-cl', 'windows-shipping', 'windows-clang-cl-shipping'):
            probe = self.run(self.ctest('-R', DX12_PROBE, '--verbose', '--timeout', '30', '--no-tests=error'),
                             check=False)
        junit = self.junit()
        selection = ['-R', NATIVE_TESTS] if self.job == 'windows-native' else []
        excluded = self.cached_tests()
        if excluded:
            selection += ['--exclude-from-file', str(excluded)]
        code = self.run(self.helper('ci-evidence.py', 'watch', '--build-dir', self.build_dir, '--',
                                    *self.ctest(*selection, '--output-on-failure', '--no-tests=error',
                                                '--output-junit', junit)), check=False)
        if probe:
            raise LaneFailure(['dx12 probe'], probe)
        self.run(self.helper('check-registered-failures.py', '--lane', self.preset, '--junit', junit, '--ctest-exit',
                             code))
        self.record_tests(junit)

    def test_sse2(self):
        if not self.preset_step('Test at the sse2 level'):
            return
        exe = ROOT / self.build_dir / 'tests/math' / ('crd-math-tests.exe' if self.windows else 'crd-math-tests')
        applied = self.run([str(exe), '[cpu-level-applied]'], check=False, env={'CRD_CPU_LEVEL': 'sse2'})
        logs = ROOT / self.build_dir / 'Testing/Temporary'
        if not self.run.dry:
            shutil.copyfile(logs / 'LastTest.log', logs / 'LastTest-lane.log')
        junit = self.junit('ctest-junit-at-sse2.xml')
        code = self.run(['ctest', '--preset', self.preset + '-at-sse2', '--output-on-failure', '--no-tests=error',
                         '--output-junit', junit], check=False)
        if applied:
            raise LaneFailure(['[cpu-level-applied]'], applied)
        self.run(self.helper('check-registered-failures.py', '--lane', self.preset + '-at-sse2', '--junit', junit,
                             '--ctest-exit', code))

    def seeded_faults(self):
        if not self.preset_step('Seeded detector failures'):
            return
        self.run(self.helper('diag-seeded-faults.py', '--build-dir', self.build_dir, '--preset', self.preset,
                             '--trace-dir', self.build_dir + '/evidence/diag-seeded-faults'))

    def genmc(self):
        if not self.preset_step('GenMC weak-memory model'):
            return
        self.run(['sudo', 'apt-get', 'install', '-y', *APT_PACKAGES['genmc']])
        self.run(['dpkg-query', '-W', '-f=${Package} ${Version}\\n', *GENMC_CENSUS],
                 stdout=self.temp / 'packages-genmc.txt')
        if not self.run.dry and (self.temp / 'packages-genmc.txt').is_file():
            with open(self.temp / 'packages.txt', 'a', encoding='utf-8') as census:
                census.write((self.temp / 'packages-genmc.txt').read_text(encoding='utf-8'))
        llvm = '' if self.run.dry else subprocess.run(['dpkg-query', '-W', '-f=${Version}', 'llvm-20-dev'],
                                                       capture_output=True, text=True, check=False).stdout.strip()
        key = 'genmc-Linux-%s-%s' % (llvm, files_hash(['cmake/pins.json', 'scripts/install-genmc.py']))
        if self.store is not None and not self.run.dry:
            restore_tree(self.store, 'blobs/genmc/%s.tar' % key, self.temp / 'genmc')
        self.run(self.helper('install-genmc.py', '--destination', self.temp / 'genmc', '--work-dir',
                             self.temp / 'genmc-work'))
        if self.store is not None and not self.run.dry:
            save_tree(self.store, 'blobs/genmc/%s.tar' % key, self.temp / 'genmc')
        evidence = self.build_dir + '/evidence/genmc'
        self.run(self.helper('check-genmc-models.py', '--genmc', self.temp / 'genmc/bin/genmc', '--json',
                             evidence + '/genmc.json', '--log-dir', evidence))

    def bundle(self):
        args = ['--preset', self.preset, '--build-dir', self.build_dir, '--outcome', self.status,
                '--output', self.build_dir + '/evidence', '--summary', self.temp / 'summary.md']
        if self.windows and self.job != 'windows-arm64':
            args += ['--adapters', self.temp / 'adapters.txt']
        if not self.windows:
            args += ['--packages', self.temp / 'packages.txt']
        if self.cached():
            args += ['--sccache-stats', self.temp / 'sccache.json']
        self.run(self.helper('ci-evidence.py', 'bundle', *args), check=False)
        if self.env.get('BUILDKITE') and (self.run.dry or (self.temp / 'summary.md').is_file()):
            self.run(['buildkite-agent', 'annotate', '--context', self.preset, '--style',
                      'success' if self.status == 'success' else 'error', '--append'], check=False)

    def upload(self):
        if self.env.get('BUILDKITE'):
            self.run(['buildkite-agent', 'artifact', 'upload',
                      'build/*/evidence/**/*;build/**/Testing/Temporary/*.log;build/**/Testing/Temporary/*.xml'],
                     check=False)

    # Per-test cache --------------------------------------------------------------------------------------------
    def test_keys(self):
        """{test name: key} from the CTest inventory: the test executable's bytes, its arguments and properties, the
        device and the runtime inputs no binary carries (assets, the register), so an unchanged binary on an
        unchanged device is never rerun."""
        inventory = ROOT / self.build_dir / 'Testing/inventory.json'
        if not inventory.is_file():
            return {}
        return test_keys(load_json(inventory), self.device, runtime_inputs_key())

    def cached_tests(self):
        if self.store is None or self.env.get('CERID_CI_TIER') == 'complete' or self.env.get('CERID_CI_FRESH'):
            return None
        names = sorted(name for name, key in self.test_keys().items()
                       if (self.store.get_json('tests/%s.json' % key) or {}).get('outcome') == 'pass')
        if not names:
            return None
        path = self.temp / 'cached-tests.txt'
        path.write_text('\n'.join(names) + '\n', encoding='utf-8')
        self.cached_test_count = len(names)
        return path

    def record_tests(self, junit):
        if self.store is None or self.run.dry or not Path(junit).is_file():
            return
        keys = self.test_keys()
        for name, outcome in junit_outcomes(junit).items():
            if outcome == 'pass' and name in keys:
                self.store.put_json('tests/%s.json' % keys[name], {'outcome': 'pass', 'test': name,
                                                                    'build': self.env.get('BUILDKITE_BUILD_NUMBER')})

    # Console profile --------------------------------------------------------------------------------------------
    def console_wrap(self, command):
        if not self.console:
            return command
        profile = self.registry['console_profiles'][self.console]
        return console_command(command, profile, self.windows, ROOT / self.build_dir, self.temp)

    def console_checks(self):
        """Positive controls: the constraints are in force for the test process, or the lane fails."""
        if not self.console:
            return
        profile = self.registry['console_profiles'][self.console]
        if self.windows:
            self.run(console_prepare_windows(ROOT / self.build_dir, self.temp))
        for probe in CONSOLE_PROBES:
            self.run(self.console_wrap(self.python() + [str(CI / 'pipeline.py'), 'console-probe', probe,
                                                         '--profile', self.console, '--build-dir',
                                                         str(ROOT / self.build_dir)]))
        self.run.log('console profile %s enforced: %s' % (self.console, profile['title']))

    def result(self, outcome, started):
        record = {'schema': RESULT_SCHEMA, 'step': self.env.get('CERID_CI_STEP', 'lane-' + self.preset),
                  'device': self.device['id'], 'preset': self.preset, 'job': self.job, 'outcome': outcome,
                  'input_key': self.env.get('CERID_CI_INPUT_KEY'), 'build': self.env.get('BUILDKITE_BUILD_NUMBER'),
                  'commit': self.env.get('BUILDKITE_COMMIT'), 'log': self.env.get('BUILDKITE_BUILD_URL', '') + (
                      '#' + self.env['BUILDKITE_JOB_ID'] if self.env.get('BUILDKITE_JOB_ID') else ''),
                  'minutes': round((time.time() - started) / 60.0, 2), 'console': self.console,
                  'features': (self.env.get('CERID_CI_FEATURES') or '').split(',') if self.env.get(
                      'CERID_CI_FEATURES') else None, 'cached_tests': getattr(self, 'cached_test_count', 0),
                  'finished': dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds')}
        record_result(self.store, record)
        return record

    PHASES = {
        'windows': ['checkout', 'python_version', 'ninja', 'msvc', 'adapters', 'cpm_cache', 'vulkan_sdk',
                    'cold_before', 'sccache', 'sde', 'configure', 'cold_after', 'build', 'sccache_stats', 'inventory',
                    'console_checks', 'test', 'test_sse2', 'seeded_faults', 'bundle', 'upload'],
        'clang-tidy': ['checkout', 'python_version', 'ninja', 'msvc', 'adapters', 'clang_tidy', 'cpm_cache',
                       'vulkan_sdk', 'configure', 'build', 'bundle', 'upload'],
        'clang-cl': ['checkout', 'python_version', 'ninja', 'msvc', 'adapters', 'cpm_cache', 'llvm_path', 'vulkan_sdk',
                     'sccache', 'configure', 'build', 'sccache_stats', 'inventory', 'test', 'bundle', 'upload'],
        'linux-gcc': ['checkout', 'reclaim_disk', 'python_version', 'ninja', 'apt', 'apt_census', 'vulkan_headers',
                      'vulkan_validation', 'cpm_cache', 'cold_before', 'sccache', 'sde', 'configure', 'cold_after',
                      'build', 'sccache_stats', 'inventory', 'console_checks', 'test', 'test_sse2', 'seeded_faults',
                      'genmc', 'bundle', 'upload'],
        'windows-shipping': ['checkout', 'python_version', 'ninja', 'msvc', 'adapters', 'cpm_cache', 'vulkan_sdk',
                             'sccache', 'configure', 'build', 'sccache_stats', 'inventory', 'console_checks', 'test',
                             'bundle', 'upload'],
        'windows-clang-cl-shipping': ['checkout', 'python_version', 'ninja', 'msvc', 'adapters', 'cpm_cache',
                                      'llvm_path', 'vulkan_sdk', 'sccache', 'configure', 'build', 'sccache_stats',
                                      'inventory', 'test', 'bundle', 'upload'],
        'linux-gcc-shipping': ['checkout', 'reclaim_disk', 'python_version', 'ninja', 'apt', 'apt_census',
                               'vulkan_headers', 'vulkan_validation', 'cpm_cache', 'sccache', 'configure', 'build',
                               'sccache_stats', 'inventory', 'console_checks', 'test', 'bundle', 'upload'],
        'windows-native': ['checkout', 'python_version', 'ninja', 'adapters', 'cpm_cache', 'vulkan_sdk', 'configure',
                           'build', 'inventory', 'test', 'bundle', 'upload'],
        'linux-arm64': ['checkout', 'python_version', 'apt', 'apt_census', 'cpm_cache', 'configure', 'build',
                        'inventory', 'test', 'bundle', 'upload'],
        'windows-arm64': ['checkout', 'python_version', 'ninja', 'msvc', 'cpm_cache', 'configure', 'build',
                          'inventory', 'test', 'bundle', 'upload'],
    }
    ALWAYS = ('sccache_stats', 'bundle', 'upload')
    UNLESS_CANCELLED = ('genmc',)

    def execute(self):
        started = time.time()
        failure = None
        for phase in self.PHASES[self.job]:
            if failure is not None and phase not in self.ALWAYS + self.UNLESS_CANCELLED:
                continue
            try:
                getattr(self, phase)()
            except LaneFailure as error:
                self.run.log('phase %s failed: %s' % (phase, error))
                if failure is None:
                    failure = error
                    self.status = 'failure'
        self.cpm_save()
        self.result('fail' if failure else 'pass', started)
        return failure.code if failure else 0


def files_hash(paths):
    digest = hashlib.sha256()
    for path in paths:
        full = ROOT / path
        digest.update(path.encode('utf-8') + b'\0' + (full.read_bytes() if full.is_file() else b''))
    return digest.hexdigest()[:24]


def runtime_inputs_key(root=ROOT):
    """Tree ids of the inputs a test reads at run time that no binary carries."""
    paths = ['assets', 'docs/third-party-defects.md', 'tests/support', 'CMakePresets.json']
    completed = subprocess.run(['git', '-C', str(root), 'rev-parse'] + ['HEAD:' + p for p in paths],
                               capture_output=True, text=True, check=False)
    return hashlib.sha256(completed.stdout.encode('utf-8')).hexdigest()


def test_keys(inventory, device, runtime_key, read=lambda p: Path(p).read_bytes() if Path(p).is_file() else b''):
    keys = {}
    hashes = {}
    for test in inventory.get('tests', []):
        command = test.get('command') or []
        if not command:
            continue
        exe = command[0]
        if exe not in hashes:
            hashes[exe] = hashlib.sha256(read(exe)).hexdigest()
        properties = {p['name']: p.get('value') for p in test.get('properties', [])
                      if p['name'] in ('ENVIRONMENT', 'WORKING_DIRECTORY', 'TIMEOUT', 'WILL_FAIL', 'LABELS',
                                       'RESOURCE_LOCK', 'FIXTURES_REQUIRED')}
        payload = {'binary': hashes[exe], 'args': command[1:], 'properties': properties, 'device': device['id'],
                   'device_config': device.get('executor_config', {}), 'runtime': runtime_key}
        keys[test['name']] = hashlib.sha256(canonical(payload).encode('utf-8')).hexdigest()
    return keys


def junit_outcomes(path):
    outcomes = {}
    for case in ET.parse(path).getroot().iter('testcase'):
        name = case.get('name')
        if case.find('failure') is not None or case.find('error') is not None:
            outcomes[name] = 'fail'
        elif case.find('skipped') is not None or case.get('status') in ('notrun', 'disabled'):
            outcomes[name] = 'skip'
        else:
            outcomes[name] = 'pass'
    return outcomes


def record_result(store, record):
    """A step's result for the dashboard, its duration sample and, on a pass, the cache entry of its input key."""
    if store is None:
        return
    build = record.get('build') or 'local'
    store.put_json('results/%s/%s.json' % (build, record['step']), record)
    durations = store.get_json('durations/%s.json' % record['step']) or {'minutes': []}
    durations['minutes'] = (durations['minutes'] + [record['minutes']])[-20:]
    store.put_json('durations/%s.json' % record['step'], durations)
    if record['outcome'] == 'pass' and record.get('input_key'):
        store.put_json('cache/%s.json' % record['input_key'], {'outcome': 'pass', 'build': build,
                                                                'step': record['step'], 'log': record.get('log')})


def save_tree(store, name, directory):
    import tarfile
    directory = Path(directory)
    if not directory.is_dir():
        return
    with tempfile.TemporaryDirectory() as scratch:
        archive = Path(scratch) / 'tree.tar'
        with tarfile.open(archive, 'w') as package:
            package.add(directory, arcname='.')
        store.put(name, archive.read_bytes())


def restore_tree(store, name, directory):
    import tarfile
    data = store.get(name)
    if data is None:
        return False
    with tempfile.TemporaryDirectory() as scratch:
        archive = Path(scratch) / 'tree.tar'
        archive.write_bytes(data)
        Path(directory).mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as package:
            package.extractall(directory, filter='data')
    return True


# --------------------------------------------------------------------------------------------- console profile

CONSOLE_PROBES = ('memory-cap', 'cores', 'no-wx', 'filesystem')


def console_command(command, profile, windows, build_dir, temp):
    """Wrap a test command in the console profile's operating-system constraints (ADR-0135 §9). Linux: a transient
    systemd service; Windows: a named Job Object joined through the `jail` trampoline (memory and affinity), with
    Arbitrary Code Guard and the filesystem view prepared by console_prepare_windows."""
    usable = profile['logical_cpus'] - profile['reserved_logical_cpus']
    memory = int(profile['memory_cap_gib'] * 1024 ** 3)
    if windows:
        return [sys.executable, str(CI / 'pipeline.py'), 'jail', '--memory-bytes', str(memory), '--cpus', str(usable),
                '--', *command]
    return ['sudo', 'systemd-run', '--quiet', '--wait', '--pipe', '--collect',
            '--uid=%d' % os.getuid() if hasattr(os, 'getuid') else '--uid=0',
            '--working-directory=%s' % ROOT, '-p', 'MemoryMax=%d' % memory, '-p', 'MemorySwapMax=0',
            '-p', 'AllowedCPUs=0-%d' % (usable - 1), '-p', 'MemoryDenyWriteExecute=yes', '-p', 'NoNewPrivileges=yes',
            '-p', 'ProtectSystem=strict', '-p', 'ProtectHome=read-only', '-p', 'PrivateTmp=yes',
            '-p', 'ReadWritePaths=%s %s' % (build_dir, temp), '--setenv=PATH=%s' % os.environ.get('PATH', ''),
            *command]


def console_prepare_windows(build_dir, temp):
    """PowerShell that turns on Arbitrary Code Guard (no writable executable memory) and the remote and low-label
    image-load blocks for every test executable and the probe interpreter, and labels the test binaries and the build
    tree Low integrity so tests can write only there (the restricted filesystem view)."""
    script = (
        "$exes = Get-ChildItem -Recurse -Filter *.exe -Path '{build}' | Select-Object -ExpandProperty Name -Unique; "
        "$exes += [System.IO.Path]::GetFileName('{python}'); "
        "foreach ($e in $exes) {{ Set-ProcessMitigation -Name $e -Enable DynamicCode, BlockRemoteImageLoads, "
        "BlockLowLabelImageLoads }}; "
        "icacls '{build}' /setintegritylevel '(OI)(CI)low' /T /C /Q | Out-Null; "
        "icacls '{temp}' /setintegritylevel '(OI)(CI)low' /T /C /Q | Out-Null"
    ).format(build=build_dir, temp=temp, python=sys.executable)
    return ['powershell', '-NoProfile', '-NonInteractive', '-Command', script]


def console_probe(name, profile, build_dir):
    """One positive control run inside the constraints; exits 0 only when the constraint held."""
    usable = profile['logical_cpus'] - profile['reserved_logical_cpus']
    if name == 'cores':
        count = len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else windows_affinity_count()
        print('usable logical CPUs: %d (profile %d)' % (count, usable))
        return 0 if count <= usable else 1
    if name == 'memory-cap':
        # The allocation runs in a child: past the cap the job refuses it (MemoryError, Windows) or the cgroup's OOM
        # killer ends the child (Linux); only a child that touched every page past the cap means the cap is absent.
        # A control below the cap must succeed, so a host short of memory cannot pass as an enforced cap.
        cap = int(profile['memory_cap_gib'] * 1024 ** 3)
        verdicts = []
        for target in (cap - 1024 ** 3, cap + 512 * 1024 ** 2):
            code = ('import sys\nb = bytearray(%d)\nfor i in range(0, len(b), 4096):\n    b[i] = 1\n'
                    'sys.exit(42)\n' % target)
            result = subprocess.run([sys.executable, '-c', code], check=False)
            print('allocation of %d bytes (cap %d): exit %d' % (target, cap, result.returncode))
            verdicts.append(result.returncode == 42)
        return 0 if verdicts == [True, False] else 1
    if name == 'no-wx':
        return 0 if not wx_allocation_allowed() else 1
    if name == 'filesystem':
        outside = Path(os.environ.get('SystemRoot', '/usr')) / 'cerid-console-probe.txt'
        inside = Path(build_dir) / 'cerid-console-probe.txt'
        try:
            outside.write_text('probe', encoding='utf-8')
            outside.unlink()
            print('wrote outside the build tree: %s' % outside)
            return 1
        except OSError:
            pass
        inside.write_text('probe', encoding='utf-8')
        inside.unlink()
        return 0
    raise SystemExit('unknown console probe %s' % name)


def windows_affinity_count():
    import ctypes
    process = ctypes.c_size_t()
    system = ctypes.c_size_t()
    kernel32 = ctypes.windll.kernel32
    kernel32.GetProcessAffinityMask(kernel32.GetCurrentProcess(), ctypes.byref(process), ctypes.byref(system))
    return bin(process.value).count('1')


def wx_allocation_allowed():
    """True when this process may map memory both writable and executable."""
    if os.name == 'nt':
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.VirtualAlloc.restype = ctypes.c_void_p
        kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
        address = kernel32.VirtualAlloc(None, 4096, 0x3000, 0x40)  # MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE
        if address:
            kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
            kernel32.VirtualFree(address, 0, 0x8000)
            return True
        return False
    import mmap
    try:
        region = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    except (OSError, PermissionError):
        return False
    region.close()
    return True


def jail(memory_bytes, cpus, command):
    """Run `command` inside a new Job Object with a job-wide memory cap, an affinity of the first `cpus` logical
    processors and kill-on-close; every process the command starts stays in the job (Windows)."""
    import ctypes
    from ctypes import wintypes

    class IoCounters(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in ('r', 'w', 'o', 'rb', 'wb', 'ob')]

    class BasicLimits(ctypes.Structure):
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong), ('PerJobUserTimeLimit', ctypes.c_longlong),
                    ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                    ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                    ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD),
                    ('SchedulingClass', wintypes.DWORD)]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [('BasicLimitInformation', BasicLimits), ('IoInfo', IoCounters),
                    ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                    ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]

    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        raise OSError(ctypes.get_last_error(), 'CreateJobObjectW')
    limits = ExtendedLimits()
    limit_job_memory, limit_affinity, kill_on_close = 0x200, 0x10, 0x2000
    limits.BasicLimitInformation.LimitFlags = limit_job_memory | limit_affinity | kill_on_close
    limits.BasicLimitInformation.Affinity = (1 << cpus) - 1
    limits.JobMemoryLimit = memory_bytes
    if not kernel32.SetInformationJobObject(wintypes.HANDLE(job), 9, ctypes.byref(limits), ctypes.sizeof(limits)):
        raise OSError(ctypes.get_last_error(), 'SetInformationJobObject')
    if not kernel32.AssignProcessToJobObject(wintypes.HANDLE(job), wintypes.HANDLE(kernel32.GetCurrentProcess())):
        raise OSError(ctypes.get_last_error(), 'AssignProcessToJobObject')
    return subprocess.call(command)


# --------------------------------------------------------------------------------------------- preflight, checks

PREFLIGHT_CHECKS = [
    ('Pinned inputs agree with the registry', ['scripts/check-pins.py']),
    ('Tier mapping owns every preset', ['scripts/check-ci-tiers.py']),
    ('Device registry and CI platform contracts', ['scripts/test-ci-platform.py']),
]
REPOSITORY_CHECKS = [
    ('Repository hygiene and layout', ['scripts/check-repository.py'], None),
    ('Documentation and roadmap', ['scripts/check-master-plan.py'], None),
    ('CI tier mapping', ['scripts/check-ci-tiers.py'], None),
    ('Pinned inputs', ['scripts/check-pins.py'], None),
    ('Generated sources carry their provenance and bytes', ['scripts/check-generated.py'], None),
    ('Allman braces on every control statement', ['scripts/check-allman-braces.py'], None),
    ('License manifest follows the registry', ['scripts/gen_license_manifest.py', '--check'], None),
    ('Build tooling regression checks', ['scripts/test-repository-tools.py'], None),
    ('Module registry contracts', ['scripts/test-module-selection.py'], None),
    ('Affected workflow selection contracts', ['scripts/test-dev-workflow.py'], None),
    ('CPU sampling runbook verdicts', ['scripts/test-sample-cpu-wpr.py'], None),
    ('Counter capability verdicts', ['scripts/test-counters-capability.py'], None),
    ('Debugger recipe verdicts', ['scripts/test-debugger-recipes.py'], None),
    ('Reverse debugging verdicts', ['scripts/test-reverse-debug-recipes.py'], None),
    ('Seeded detector failure verdicts', ['scripts/test-diag-seeded-faults.py'], None),
    ('Diagnostic coverage verdicts', ['scripts/test-diag-coverage.py'], None),
    ('Overhead board verdicts', ['scripts/test-diag-overhead.py'], None),
    ('Overhead external sampling verdicts', ['scripts/test-sample-overhead-wpr.py'], None),
    ('Project structure synchronization', ['scripts/test-project-sync.py'], None),
    ('Canonical build profile contracts', ['scripts/test-native-build-profiles.py'], None),
    ('Native MSVC profile compile and execution',
     ['scripts/test-native-build-profiles.py', '--generator', 'Visual Studio 18 2026'], 'windows'),
    ('Native Visual Studio structure round trips',
     ['scripts/test-project-sync-native.py', '--generator', 'Visual Studio 18 2026'], 'windows'),
    ('Linux structure projection and compile', ['scripts/test-project-sync-native.py', '--generator', 'Ninja'],
     'linux'),
    ('Generated CEIR artifacts', ['tools/ceir_opgen/ceir_opgen.py', '--check'], None),
    ('Capability source references', ['tools/ceir_capability_matrix/gen_matrix.py', '--check'], None),
    ('CI platform contracts', ['scripts/test-ci-platform.py'], None),
]


def actionlint(run):
    """Validate the GitHub workflows with the pinned actionlint while they still exist (until CI-4 retires them)."""
    if not (ROOT / '.github/workflows').is_dir() or os.name == 'nt':
        return 0
    temp = Path(tempfile.mkdtemp(prefix='cerid-actionlint-'))
    pin = tool_pin('actionlint')
    code = run([sys.executable, 'scripts/pins.py', 'fetch', 'tools', 'actionlint', '--destination', str(temp)],
               check=False)
    if code == 0:
        code = run(['tar', '-xzf', str(temp / pin['file']), '-C', str(temp), 'actionlint'], check=False)
    if code == 0:
        code = run([str(temp / 'actionlint'), '-shellcheck=', '-pyflakes='], check=False)
    return code


def run_checks(which, run=None, host=None):
    run = run or Runner()
    host = host or ('windows' if os.name == 'nt' else 'linux')
    failures = []
    if which == 'preflight':
        run.log('--- Validate workflows (actionlint)')
        if actionlint(run) != 0:
            failures.append('Validate workflows (actionlint)')
    if which == 'repository':
        temp = Path(tempfile.mkdtemp(prefix='cerid-repo-'))
        run([sys.executable, 'scripts/install-ninja.py', '--destination', str(temp / 'ninja'), '--github-path',
             str(temp / 'path.txt')])
        apply_path_file(temp / 'path.txt', run.environ)
        checks = [(t, a) for t, a, only in REPOSITORY_CHECKS if only in (None, host)]
    else:
        checks = PREFLIGHT_CHECKS
    for title, args in checks:
        run.log('--- %s' % title)
        if run([sys.executable] + args, check=False) != 0:
            failures.append(title)
    for title in failures:
        run.log('FAILED: %s' % title)
    return 1 if failures else 0


def conclude(planned, outcome_of):
    """The required result: every planned step that must pass passed (a cached step passed with its key), a budget
    cell is incomplete, never a pass. Reuses ci-tier.py's conclusion rule."""
    needs = {'preflight': {'result': 'success'}}
    expected = []
    for step in planned['steps']:
        expected.append(step['key'])
        if step['state'] == 'cached':
            needs[step['key']] = {'result': 'success'}
        elif step['state'] == 'budget':
            needs[step['key']] = {'result': 'budget'}
        else:
            outcome = outcome_of(step['key'])
            needs[step['key']] = {'result': {'passed': 'success'}.get(outcome, outcome or 'absent')}
    ok, rows = ci_tier.conclude(needs, expected)
    return ok, rows


def buildkite_outcome(key):
    completed = subprocess.run(['buildkite-agent', 'step', 'get', 'outcome', '--step', key], capture_output=True,
                               text=True, check=False)
    return completed.stdout.strip() if completed.returncode == 0 else 'absent'


# --------------------------------------------------------------------------------------------- parity (CI-4)

LANE_STEP_PHASES = {
    'Checkout': 'checkout', 'Setup Python 3.12': 'python_version', 'Install pinned Ninja 1.11.1': 'ninja',
    'Setup MSVC': 'msvc', 'Record graphics driver identity': 'adapters', 'Cache CPM downloads': 'cpm_cache',
    'Cache Vulkan SDK': 'vulkan_sdk', 'Install Vulkan SDK': 'vulkan_sdk', 'Set Vulkan environment': 'vulkan_sdk',
    'Cold acquisition (empty package cache, pinned WARP)': 'cold_before',
    'Cold acquisition (empty package cache)': 'cold_before', 'Install pinned sccache': 'sccache',
    'Compiler cache service': 'sccache', 'Install pinned Intel SDE': 'sde', 'Configure': 'configure',
    'Configure with the compiler cache': 'configure', 'Cold acquisition (every pinned package acquired)': 'cold_after',
    'Build': 'build', 'Build the Debug sandbox profile': 'build', 'Compiler cache statistics': 'sccache_stats',
    'Test inventory': 'inventory', 'Test': 'test', 'Test at the sse2 level': 'test_sse2',
    'Seeded detector failures': 'seeded_faults', 'Install GenMC toolchain': 'genmc', 'Restore GenMC build': 'genmc',
    'GenMC weak-memory model': 'genmc', 'Evidence bundle and summary': 'bundle', 'Preserve evidence': 'upload',
    'Reclaim runner disk': 'reclaim_disk', 'Install system dependencies': 'apt',
    'Record system package versions': 'apt_census',
    'Install pinned Vulkan headers and SPIRV-Reflect': 'vulkan_headers',
    'Install matching Vulkan validation layer': 'vulkan_validation',
    'Install clang-tidy (LLVM 20.1.8, pinned)': 'clang_tidy', 'Verify clang-tidy version': 'clang_tidy',
    'Add LLVM to PATH': 'llvm_path',
}
FIXED_STEP_MAP = {
    'preflight': {
        'uses actions/checkout': ('plan', 'agent checkout of the build commit'),
        'uses actions/setup-python': ('plan', 'the launcher image\'s Python 3.12'),
        'Validate workflows (actionlint 1.7.12)': ('plan', 'run preflight: Validate workflows (actionlint)'),
        'Pinned inputs agree with the registry': ('plan', 'run preflight: Pinned inputs agree with the registry'),
        'Tier mapping owns every preset': ('plan', 'run preflight: Tier mapping owns every preset'),
        'Choose the comparison base': ('plan', 'push_base(): the newest planned head on the branch (result store)'),
        'Fetch the comparison base': ('plan', 'git fetch of the base before planning'),
        'Resolve the tier': ('plan', 'plan(): ci-tier.py resolve, then the module selection'),
    },
    'repository': {
        'uses actions/checkout': ('repository-<host>', 'agent checkout of the build commit'),
        'uses actions/setup-python': ('repository-<host>', 'the image\'s Python 3.12'),
        'Install pinned Ninja 1.11.1': ('repository-<host>', 'run repository: install-ninja.py'),
    },
    'required': {
        'uses actions/checkout': ('required', 'agent checkout of the build commit'),
        'uses actions/setup-python':('required', 'the launcher image\'s Python 3.12'),
        'Every expected job ran and passed': ('required', 'pipeline.py conclude (ci-tier.py conclude rule)'),
    },
}


def workflow_steps(text):
    """{job: [(label, run text)]} of a workflow in this repository's layout: a step starts at `      - `, its label
    is its name or `uses <action>`."""
    check = load_script('check_ci_tiers', 'scripts/check-ci-tiers.py')
    steps = {}
    for job, block in check.workflow_jobs(text).items():
        current = None
        items = []
        for line in block.splitlines():
            start = re.match(r'^      - (.*)$', line)
            if start:
                current = {'name': None, 'uses': None, 'run': []}
                items.append(current)
                line = '        ' + start.group(1)
            if current is None:
                continue
            field = re.match(r'^        (name|uses|run):\s*(.*)$', line)
            if field:
                key, value = field.groups()
                if key == 'run':
                    current['run'] = [] if value in ('|', '>-', '>') else [value]
                    current['in_run'] = True
                else:
                    current[key] = value.strip().strip('"')
                    current['in_run'] = False
            elif current.get('in_run') and line.startswith('          '):
                current['run'].append(line.strip())
            elif re.match(r'^        [a-z-]+:', line):
                current['in_run'] = False
        labels = []
        for item in items:
            label = item['name'] or 'uses ' + (item['uses'] or '?').split('@', 1)[0]
            labels.append((label, '\n'.join(item['run'])))
        steps[job] = labels
    return steps


def parity_rows(text=None, tiers=None):
    """Every GitHub Actions job and step with the Buildkite step and phase that carries it; problems for any step
    without one."""
    text = text if text is not None else WORKFLOW.read_text(encoding='utf-8')
    tiers = tiers or ci_tier.load_tiers(TIERS)
    rows = []
    problems = []
    checks = {title: args for title, args, _ in REPOSITORY_CHECKS}
    for job, steps in workflow_steps(text).items():
        for label, _ in steps:
            if job in FIXED_STEP_MAP and label in FIXED_STEP_MAP[job]:
                step, phase = FIXED_STEP_MAP[job][label]
                rows.append((job, label, step, phase))
            elif job == 'repository' and label in checks:
                rows.append((job, label, 'repository-<host>', 'run repository: ' + ' '.join(checks[label])))
            elif job in Lane.PHASES and label in LANE_STEP_PHASES:
                phase = LANE_STEP_PHASES[label]
                if phase not in Lane.PHASES[job]:
                    problems.append('%s / %s maps to phase %s, which the %s lane does not run' % (job, label, phase,
                                                                                               job))
                presets = [p for p, e in tiers['presets'].items() if e.get('job') == job]
                rows.append((job, label, 'lane-<preset> (%s)' % ', '.join(presets), 'Lane.%s' % phase))
            else:
                problems.append('%s / %s has no Buildkite step' % (job, label))
    return rows, problems


HOST_CHECK = ROOT / '.github/workflows/host-check.yml'


def experiment_rows(text=None):
    """The dispatch-only host-check experiments: listed so CI-4 cannot forget them; each stays open until a
    Buildkite experiment step carries it (they run inline shell that must first move into a script)."""
    text = text if text is not None else (HOST_CHECK.read_text(encoding='utf-8') if HOST_CHECK.is_file() else '')
    rows = []
    for job, steps in workflow_steps(text).items():
        for label, _ in steps:
            rows.append(('host-check/' + job, label, 'experiment-%s' % job, 'open (CI-4): not yet carried'))
    return rows


def parity_markdown(rows):
    lines = ['| GitHub job | GitHub step | Buildkite step | Carried by |', '|---|---|---|---|']
    for job, label, step, phase in rows:
        lines.append('| `%s` | %s | `%s` | %s |' % (job, label.replace('|', '\\|'), step, phase.replace('|', '\\|')))
    return '\n'.join(lines) + '\n'


# --------------------------------------------------------------------------------------------- GCP budget stop

def pull_budget(store, subscription, http=None, token=None):
    """Drain the Cloud Billing budget notifications (Pub/Sub pull subscription) into the store; the newest cost of
    each month decides gcp_budget_stop(). Returns the notifications read."""
    import base64
    from executors import gce_vm
    http = http or gce_vm.http_request
    token = token or gce_vm.access_token
    _, pulled = http('POST', 'https://pubsub.googleapis.com/v1/%s:pull' % subscription, token=token(),
                     body={'maxMessages': 50})
    notes = []
    acks = []
    for item in pulled.get('receivedMessages', []):
        acks.append(item['ackId'])
        try:
            note = json.loads(base64.b64decode(item['message']['data']).decode('utf-8'))
        except (KeyError, ValueError):
            continue
        month = str(note.get('costIntervalStart', ''))[:7] or month_of()
        current = store.get_json('budget/gcp-%s.json' % month) or {}
        if float(note.get('costAmount', 0.0)) >= float(current.get('costAmount', -1.0)):
            store.put_json('budget/gcp-%s.json' % month, {k: note.get(k) for k in (
                'costAmount', 'budgetAmount', 'alertThresholdExceeded', 'costIntervalStart', 'currencyCode')})
        notes.append(note)
    if acks:
        http('POST', 'https://pubsub.googleapis.com/v1/%s:acknowledge' % subscription, token=token(),
             body={'ackIds': acks})
    return notes


# --------------------------------------------------------------------------------------------- probes

def probe_macos(run=None):
    """The macOS probe: toolchain and Metal device census, then `xcrun metal` compiles of the cooked Metal programs
    when the tree has any (MAC.1 adds them); an empty set is reported, never a pass for missing work."""
    run = run or Runner()
    for argv in (['sw_vers'], ['xcrun', '--show-sdk-version'], ['xcrun', 'metal', '--version'],
                 ['system_profiler', 'SPDisplaysDataType']):
        run(argv)
    sources = sorted(ROOT.glob('assets/**/*.metal'))
    if not sources:
        run.log('no cooked Metal programs in the tree yet (MAC.1); compiles: 0')
        return 0
    out = Path(tempfile.mkdtemp(prefix='cerid-metal-'))
    for source in sources:
        run(['xcrun', 'metal', '-c', str(source), '-o', str(out / (source.stem + '.air'))])
    return 0


# --------------------------------------------------------------------------------------------- dispatcher

STACKS = 'https://agent.buildkite.com/v3/stacks'


class Dispatcher:
    """The launcher's job loop over Buildkite's Stacks API: list scheduled jobs (also the queue's heartbeat), route
    each by its `device=` rule to its executor (acquired jobs ignore agent tags, so routing is ours), refuse paid
    launches past the budget, reserve, issue a one-job acquisition token and launch; reap finished machines."""

    def __init__(self, registry, store, agent_token, http=None, stack='cerid-cloud', environ=None, executors=None,
                 now=None):
        import executors as executor_package
        self.registry = registry
        self.store = store
        self.agent_token = agent_token
        self.http = http or executor_package.gce_vm.http_request
        self.stack = stack
        self.queue = registry['queues']['cloud']
        self.environ = os.environ if environ is None else environ
        self.devices = devices_by_id(registry)
        self.executors = executors or executor_package.registry(self.environ)
        self.now = now or (lambda: dt.datetime.now(dt.timezone.utc))

    def call(self, method, path, body=None):
        _, payload = self.http(method, STACKS + path, token=self.agent_token, scheme='Token', body=body)
        return payload

    def register(self):
        return self.call('POST', '/register', {'key': self.stack, 'type': 'custom', 'queue_key': self.queue,
                                               'metadata': {'launcher': 'ci/pipeline.py dispatch'}})

    def scheduled(self):
        jobs = []
        after = None
        while True:
            query = {'queue_key': self.queue, 'limit': 100}
            if after:
                query['after'] = after
            page = self.call('GET', '/%s/scheduled-jobs?%s' % (self.stack, urllib.parse.urlencode(query)))
            if page.get('cluster_queue', {}).get('dispatch_paused'):
                return []
            jobs += page.get('jobs', [])
            info = page.get('page_info', {})
            if not info.get('has_next_page'):
                return jobs
            after = info.get('end_cursor')

    def route(self, job):
        for rule in job.get('agent_query_rules', []):
            name, _, value = rule.partition('=')
            if name == 'device':
                return self.devices.get(value)
        return None

    def finish(self, job, code, detail):
        self.call('POST', '/%s/jobs/%s/finish' % (self.stack, job['id']), {'exit_status': code, 'detail': detail[:4000]})

    def tick(self):
        month = month_of(self.now())
        subscription = self.environ.get('CERID_CI_BUDGET_SUBSCRIPTION')
        if subscription:
            pull_budget(self.store, subscription, http=self.http)
        cap = float(self.registry['budget']['monthly_cap_usd'])
        stop = gcp_budget_stop(self.store, month)
        spent = month_spend(self.store, month)
        launched = []
        for job in self.scheduled():
            device = self.route(job)
            if device is None or device['executor'] not in self.executors:
                self.finish(job, 1, 'no launchable device for rules %s' % job.get('agent_query_rules'))
                continue
            executor = self.executors[device['executor']]
            hourly = float(device.get('cost_per_hour_usd', 0.0))
            bound = hourly * float(device.get('executor_config', {}).get('max_run_hours', 6))
            if hourly > 0 and (stop or spent + bound > cap):
                self.finish(job, BUDGET_EXIT, 'budget: %.2f of %.2f USD this month%s' % (
                    spent, cap, '; GCP budget stop' if stop else ''))
                continue
            reserved = self.call('PUT', '/%s/scheduled-jobs/batch-reserve' % self.stack,
                                 {'job_uuids': [job['id']], 'reservation_expiry_seconds': 1800})
            if job['id'] not in reserved.get('reserved', []):
                continue
            issued = self.call('POST', '/%s/job-acquisition-tokens' % self.stack,
                               {'job_uuids': [job['id']], 'token_lifetime_seconds': 1800})
            tokens = {t['job_uuid']: t['job_acquisition_token'] for t in issued.get('job_acquisition_tokens', [])}
            if job['id'] not in tokens:
                continue
            try:
                handle = executor.launch(job['id'], device, tokens[job['id']])
            except Exception as error:  # a launch failure ends the job with its reason, never a silent hang
                self.finish(job, 1, 'launch failed on %s: %s' % (device['id'], error))
                continue
            spent += bound
            record = {'job': job['id'], 'device': device['id'], 'executor': device['executor'], 'handle': handle,
                      'started': self.now().isoformat(timespec='seconds'), 'rate': hourly}
            self.store.put_json('machines/%s.json' % job['id'], record)
            launched.append(record)
        self.reap()
        return launched

    def reap(self):
        running = [self.store.get_json(n) for n in self.store.list('machines/')]
        running = [r for r in running if r and not r.get('ended')]
        if not running:
            return []
        states = self.call('POST', '/%s/jobs/get-states' % self.stack,
                           {'job_uuids': [r['job'] for r in running]}).get('states', {})
        ended = []
        for record in running:
            if states.get(record['job']) in ('finished', 'canceled', 'expired', 'timed_out', 'skipped', 'broken'):
                executor = self.executors.get(record['executor'])
                if executor is not None:
                    executor.teardown(record['handle'])
                now = self.now()
                started = dt.datetime.fromisoformat(record['started'])
                hours = max((now - started).total_seconds(), 60.0) / 3600.0
                record['ended'] = now.isoformat(timespec='seconds')
                record['usd'] = round(hours * record['rate'], 4)
                self.store.put_json('machines/%s.json' % record['job'], record)
                self.store.put_json('ledger/%s/%s.json' % (month_of(now), record['job']),
                                    {'device': record['device'], 'hours': round(hours, 4), 'usd': record['usd']})
                ended.append(record)
        return ended


def read_token(environ, name):
    path = environ.get(name)
    if not path or not Path(path).is_file():
        raise SystemExit('%s must name the file holding the token (never the value itself)' % name)
    return Path(path).read_text(encoding='utf-8').strip()


# --------------------------------------------------------------------------------------------- command line

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    p_plan = sub.add_parser('plan', help='print the plan for a revision range')
    p_plan.add_argument('--event', default='push')
    p_plan.add_argument('--base', default='')
    p_plan.add_argument('--head', default='HEAD')
    p_plan.add_argument('--tier', default=None)
    p_plan.add_argument('--fresh', action='store_true', help='ignore the result cache')
    p_plan.add_argument('--buildkite', action='store_true', help='print the Buildkite pipeline instead')
    p_upload = sub.add_parser('upload', help='plan this Buildkite build and upload its pipeline')
    p_upload.add_argument('--fresh', action='store_true')
    p_lane = sub.add_parser('lane', help='run one lane')
    p_lane.add_argument('--preset', required=True)
    p_lane.add_argument('--device', required=True)
    p_lane.add_argument('--modules', help='affected module selection (CRD_MODULES)')
    p_lane.add_argument('--console', help='console profile to enforce')
    p_lane.add_argument('--dry-run', action='store_true')
    p_run = sub.add_parser('run', help='run the preflight or repository checks')
    p_run.add_argument('which', choices=('preflight', 'repository'))
    p_con = sub.add_parser('conclude', help='the required result of a build')
    p_con.add_argument('--plan', required=True)
    p_dis = sub.add_parser('dispatch', help='the launcher loop')
    p_dis.add_argument('--once', action='store_true')
    p_dis.add_argument('--interval', type=float, default=15.0)
    p_probe = sub.add_parser('probe', help='a device probe')
    p_probe.add_argument('which', choices=('macos',))
    p_probe.add_argument('--device')
    p_jail = sub.add_parser('jail', help='run a command in a capped Job Object (Windows console profile)')
    p_jail.add_argument('--memory-bytes', type=int, required=True)
    p_jail.add_argument('--cpus', type=int, required=True)
    p_jail.add_argument('rest', nargs=argparse.REMAINDER)
    p_cp = sub.add_parser('console-probe', help='one console constraint control')
    p_cp.add_argument('probe', choices=CONSOLE_PROBES)
    p_cp.add_argument('--profile', required=True)
    p_cp.add_argument('--build-dir', required=True)
    args = parser.parse_args(argv)

    if args.command == 'plan':
        planned = plan(args.event, args.base, args.head, args.tier, fresh=args.fresh)
        document = buildkite_pipeline(planned, load_registry()['queues']) if args.buildkite else planned
        print(json.dumps(document, indent=1))
        return 0
    if args.command == 'upload':
        if run_checks('preflight') != 0:
            return 1
        store = open_store()
        event, base, requested = buildkite_event(os.environ)
        branch = os.environ.get('BUILDKITE_BRANCH', 'main')
        if event == 'push' and not base:
            base = push_base(store, branch)
        if base.startswith('origin/'):
            subprocess.run(['git', 'fetch', '--no-tags', '--depth=50', 'origin', base[7:]], cwd=str(ROOT), check=False)
            base = subprocess.run(['git', 'merge-base', base, 'HEAD'], cwd=str(ROOT), capture_output=True, text=True,
                                  check=False).stdout.strip()
        elif base:
            subprocess.run(['git', 'fetch', '--no-tags', '--depth=1', 'origin', base], cwd=str(ROOT), check=False)
        planned = plan(event, base, 'HEAD', requested, store=store, fresh=args.fresh or bool(
            os.environ.get('CERID_CI_FRESH')))
        Path('ci-plan.json').write_text(json.dumps(planned, indent=1) + '\n', encoding='utf-8')
        subprocess.run(['buildkite-agent', 'artifact', 'upload', 'ci-plan.json'], check=True)
        pipeline = json.dumps(buildkite_pipeline(planned, load_registry()['queues']))
        subprocess.run(['buildkite-agent', 'pipeline', 'upload'], input=pipeline, text=True, check=True)
        store.put_json('heads/%s.json' % branch.replace('/', '__'), {'head': subprocess.run(
            ['git', 'rev-parse', 'HEAD'], cwd=str(ROOT), capture_output=True, text=True, check=True).stdout.strip(),
            'build': os.environ.get('BUILDKITE_BUILD_NUMBER')})
        return 0
    if args.command == 'lane':
        registry = load_registry()
        device = devices_by_id(registry)[args.device]
        modules = [m for m in (args.modules or '').split(';') if m]
        lane = Lane(args.preset, device, run=Runner(dry=args.dry_run), modules=modules or None, console=args.console,
                    registry=registry, store=None if args.dry_run else open_store())
        return lane.execute()
    if args.command == 'run':
        return run_checks(args.which)
    if args.command == 'conclude':
        if not Path(args.plan).is_file():
            subprocess.run(['buildkite-agent', 'artifact', 'download', args.plan, '.'], check=False)
        planned = load_json(args.plan)
        ok, rows = conclude(planned, buildkite_outcome)
        print(ci_tier.conclusion_markdown(ok, rows, planned['tier']))
        return 0 if ok else 1
    if args.command == 'dispatch':
        registry = load_registry()
        dispatcher = Dispatcher(registry, open_store(), read_token(os.environ, 'CERID_BUILDKITE_AGENT_TOKEN_FILE'))
        dispatcher.register()
        while True:
            for record in dispatcher.tick():
                print('launched %s on %s' % (record['job'], record['device']))
            if args.once:
                return 0
            time.sleep(args.interval)
    if args.command == 'probe':
        return probe_macos()
    if args.command == 'jail':
        rest = args.rest[1:] if args.rest[:1] == ['--'] else args.rest
        return jail(args.memory_bytes, args.cpus, rest)
    if args.command == 'console-probe':
        profile = load_registry()['console_profiles'][args.profile]
        return console_probe(args.probe, profile, args.build_dir)
    return 2


if __name__ == '__main__':
    sys.exit(main())
