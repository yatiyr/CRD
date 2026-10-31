"""The gold image service (CI-3, ADR-0135 §17): golden-image testing in the spirit of Skia Gold.

A lane (or a Firebase Test Lab job) writes the images its renderer tests produced, with a manifest
(`cerid-gold/1`), to `<build dir>/gold/` and uploads them as artifacts. When the manifest arrives the coordinator
records a pending result for the job; once the job is over, `tick` (a background worker beside the coordinator's
loop) reads every image, keys it, stores it by content and compares it:

- **Keys**: test, device, device class, GPU vendor, device kind, API, backend (the renderer), CPU level, feature,
  commit and build. The **grouping** is the test plus the keys the policy (`ci/gold.json`) names for it (`api` by
  default; `device_class` or `device` where a test declares that baselines differ per device class).
- **Content**: an image is its digest (SHA-256 of the size and the RGBA8 pixels, coordinator/image.py), stored once
  under `gold/blobs/`; two encodings of one picture are one image.
- **Expectations** per grouping: each digest is `positive` (an approved baseline), `negative` (a rejected rendering)
  or untriaged (never seen by a person). Baselines are the positive digests: any number, any device of the grouping.
- **Comparison**: a positive digest passes and a negative fails, always. Otherwise a deterministic software renderer
  (lavapipe, WARP, SwiftShader: the device's GPU vendor is `software`) is compared exactly, so any other digest needs
  triage. A real GPU or a phone is compared perceptually against the grouping's newest positives: LDR-FLIP at the
  device's pixels per degree (its screen density at its kind's viewing distance) and SSIM, against the test's
  declared tolerances (mean FLIP, the share of pixels above a FLIP threshold, minimum SSIM); within them it passes
  (and says so), beyond them it needs triage. An image too large for the pixel budget is compared box-downsampled by
  a declared integer factor (the pixels per degree divided by it).
- **Never a pass by default**: an untriaged or not yet compared image marks its QUAL cell `triage`; a negative or an
  unreadable image fails it. The cell turns to pass when a person approves, without a re-run.
- **Triage**: Approve (positive) or Reject (negative) on the web UI's triage page, each a compare-and-swap with the
  state the person saw (a stale click is refused, never applied), each audited; a reject files a finding
  (`gold/findings/`) with the text of the ROADMAP finding row its owner files, and notifies.
"""
from __future__ import annotations

from pathlib import Path
import argparse
import fnmatch
import hashlib
import json
import math
import os
import re
import shutil
import sys
import threading
import time

CI = Path(__file__).resolve().parents[1]
if str(CI) not in sys.path:
    sys.path.insert(0, str(CI))

from coordinator import image, state  # noqa: E402

SCHEMA = 'cerid-gold/1'
POLICY_FILE = CI / 'gold.json'
PREFIX = 'orchestrator/'
LABELS = ('positive', 'negative', 'untriaged')
# expected label -> labels a person may move it to (a stale `expected` is refused)
TRANSITIONS = {'untriaged': ('positive', 'negative'), 'positive': ('negative', 'untriaged'),
               'negative': ('positive', 'untriaged')}
KEYS = ('test', 'device', 'device_class', 'vendor', 'kind', 'api', 'backend', 'cpu_level', 'feature')
TEST_NAME = re.compile(r'^[A-Za-z0-9_.:/+-]{1,200}$')
SAFE_FILE = re.compile(r'^[A-Za-z0-9_.+-][A-Za-z0-9_./+-]{0,200}$')
MAX_POSITIVES = 3
MAX_PER_TICK = 8
INDEX = 500  # results the triage page and the re-comparison look at


class GoldError(ValueError):
    pass


def load_policy(path=POLICY_FILE):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def rules_for(policy, test):
    """The test's rules: the defaults, then every `tests` entry whose pattern matches it, in order."""
    rules = dict(policy['defaults'])
    for entry in policy.get('tests', []):
        if fnmatch.fnmatchcase(test, entry['match']):
            rules.update({k: v for k, v in entry.items() if k != 'match'})
    return rules


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'))


def grouping_of(keys, rules):
    grouping = {'test': keys['test']}
    for name in rules.get('group_by', []):
        if name not in KEYS:
            raise GoldError('group_by names an unknown key %r' % name)
        grouping[name] = keys.get(name)
    return grouping


def grouping_id(grouping):
    return hashlib.sha256(canonical(grouping).encode('utf-8')).hexdigest()[:24]


def device_keys(device):
    gpu = device.get('gpu', {})
    vendor = gpu.get('vendor', 'unknown')
    return {'device': device['id'], 'vendor': vendor, 'kind': device.get('kind'),
            'device_class': '%s/%s' % (vendor, device.get('kind'))}


def mode_for(device, rules):
    """exact for a deterministic software renderer, perceptual for real GPUs and phones; a test may declare it."""
    if rules.get('mode') in ('exact', 'perceptual'):
        return rules['mode']
    return 'exact' if device.get('gpu', {}).get('vendor') == 'software' else 'perceptual'


def feature_for(api, device, registry):
    feature = (api or '').lower()
    if feature in registry.get('features', {}) and feature in device.get('features', []):
        return feature
    return 'app-launch' if 'app-launch' in device.get('features', []) else feature or 'app-launch'


# --------------------------------------------------------------------------------------------- the manifest

def parse_manifest(data):
    """The manifest's image list, validated: test names, safe relative file names, string keys."""
    try:
        manifest = json.loads(data.decode('utf-8'))
    except (UnicodeDecodeError, ValueError) as error:
        raise GoldError('the gold manifest is not JSON: %s' % error) from None
    if not isinstance(manifest, dict) or manifest.get('schema') != SCHEMA:
        raise GoldError('the gold manifest must say "schema": "%s"' % SCHEMA)
    images = manifest.get('images')
    if not isinstance(images, list) or not images or len(images) > 2000:
        raise GoldError('the gold manifest lists 1 to 2000 images')
    out = []
    for item in images:
        if not isinstance(item, dict) or not TEST_NAME.match(str(item.get('test', ''))):
            raise GoldError('an image needs a test name matching %s' % TEST_NAME.pattern)
        name = str(item.get('file', ''))
        if not SAFE_FILE.match(name) or '..' in name.split('/'):
            raise GoldError('image file %r refused' % name)
        out.append({'test': item['test'], 'file': name,
                    **{k: str(item[k])[:100] for k in ('api', 'backend', 'feature', 'cpu_level') if item.get(k)}})
    return out


def register_manifest(coordinator, job, entry, data):
    """Called when a job uploads `.../gold/manifest.json`: a pending result for the job's attempt, compared by
    `tick` once the job is over. A bad manifest is recorded as an error (the cell fails), never ignored."""
    base = entry['path'].rsplit('/', 1)[0]
    name = result_name(job['build'], job['id'], job['attempt'], base)
    digest = hashlib.sha256(data).hexdigest()
    existing = coordinator.get(name)
    if existing and existing.get('manifest_sha256') == digest:
        return name  # the same manifest again (the in-job upload, then the end-of-job one)
    try:
        images = parse_manifest(data)
        error = None
    except GoldError as problem:
        images, error = [], str(problem)
    record = {'build': job['build'], 'job': job['id'], 'attempt': job['attempt'], 'key': job['key'],
              'manifest_sha256': digest,
              'device': (job.get('env') or {}).get('CERID_CI_DEVICE'), 'commit': None, 'base': base,
              'cpu_level': (job.get('env') or {}).get('CRD_CPU_LEVEL'), 'status': 'pending', 'error': error,
              'entries': [dict(i, digest=None, state=None) for i in images], 'at': coordinator.clock()}
    try:
        record['commit'] = coordinator.build(job['build'])['commit']
    except Exception:
        record['commit'] = None
    coordinator.update(name, lambda current: record)
    coordinator.update('gold/pending.json', lambda p: {'results': sorted(set((p or {}).get('results', []))
                                                                         | {name})})
    coordinator.update('gold/index.json', lambda index: {'results': ([r for r in (index or {}).get('results', [])
                                                                      if r != name] + [name])[-INDEX:]})
    return name


def recent_results(coordinator, limit=INDEX):
    """(name, record) of the newest results, oldest first (the index `register_manifest` keeps)."""
    names = (coordinator.get('gold/index.json') or {}).get('results', [])[-limit:]
    return [(n, r) for n, r in ((n, coordinator.get(n)) for n in names) if r]


def result_name(build, job_id, attempt, base):
    return 'gold/results/%010d/%s-%d-%s.json' % (int(build), job_id, int(attempt),
                                               hashlib.sha256(base.encode('utf-8')).hexdigest()[:8])


# --------------------------------------------------------------------------------------------- comparison

def blob_name(digest):
    return PREFIX + 'gold/blobs/%s.png' % digest


def diff_name(reference, test):
    return PREFIX + 'gold/diffs/%s-%s' % (reference, test)


def load_image(store, digest):
    data = store.get(blob_name(digest))
    if data is None:
        raise GoldError('image %s is not in the store' % digest)
    return image.decode_png(data)


def compare(store, reference_digest, test_digest, ppd, rules):
    """Metrics of one pair (cached by digests, rules and viewing): FLIP statistics, SSIM, the scale and whether they
    are within the tolerances; the FLIP heatmap is stored beside them."""
    budget = int(rules.get('max_pixels', 262144))
    pixel_threshold = float(rules.get('flip_pixel', 0.1))
    key = hashlib.sha256(canonical({'ppd': round(ppd, 3), 'budget': budget, 'pixel': pixel_threshold}).encode(
        'utf-8')).hexdigest()[:12]
    cached = store.get_json(diff_name(reference_digest, test_digest) + '-%s.json' % key)
    if cached is None:
        reference = load_image(store, reference_digest)
        test = load_image(store, test_digest)
        if (reference.width, reference.height) != (test.width, test.height):
            cached = {'size_mismatch': True, 'reference_size': [reference.width, reference.height],
                      'test_size': [test.width, test.height]}
        else:
            factor = max(1, math.ceil(math.sqrt(reference.width * reference.height / budget)))
            reference, test = image.downsample(reference, factor), image.downsample(test, factor)
            errors = image.flip(reference, test, ppd / factor)
            cached = dict(image.summarize(errors, pixel_threshold), ssim=image.ssim(reference, test), scale=factor,
                          ppd=round(ppd / factor, 3), size=[reference.width, reference.height])
            store.put(diff_name(reference_digest, test_digest) + '-%s.png' % key, image.encode_png(
                image.heatmap(errors, reference.width, reference.height)))
        cached['diff'] = key
        store.put_json(diff_name(reference_digest, test_digest) + '-%s.json' % key, cached)
    within = (not cached.get('size_mismatch') and cached['flip_mean'] <= float(rules.get('flip_mean', 0.0))
              and cached['flip_area'] <= float(rules.get('flip_area', 0.0))
              and cached['ssim'] >= float(rules.get('ssim_min', 1.0)))
    return dict(cached, within=within)


def expectations(coordinator, gid):
    return coordinator.get('gold/expectations/%s.json' % gid) or {'grouping': None, 'digests': {}}


def label_of(expect, digest):
    return (expect.get('digests', {}).get(digest) or {}).get('label', 'untriaged')


def positives(expect, limit=MAX_POSITIVES):
    marks = [(m.get('at', 0), d) for d, m in expect.get('digests', {}).items() if m.get('label') == 'positive']
    return [d for _, d in sorted(marks, reverse=True)[:limit]]


def process(coordinator, name, registry, policy, artifact_reader):
    """Ingest and compare one pending result whose job is over. Returns the record."""
    record = coordinator.get(name)
    if record is None or record['status'] == 'done':
        return record
    devices = {d['id']: d for d in registry['devices']}
    device = devices.get(record['device'])
    entries = []
    for entry in record['entries']:
        entry = dict(entry)
        try:
            if device is None:
                raise GoldError('the job names no registered device (CERID_CI_DEVICE)')
            data = artifact_reader(record, '%s/%s' % (record['base'], entry['file']))
            picture = image.decode_png(data)
            entry['digest'] = picture.digest()
            entry['size'] = [picture.width, picture.height]
            if coordinator.store.get(blob_name(entry['digest'])) is None:
                coordinator.store.put(blob_name(entry['digest']), image.encode_png(picture))
            rules = rules_for(policy, entry['test'])
            api = entry.get('api') or (device.get('gpu', {}).get('apis') or [''])[0]
            keys = dict(device_keys(device), test=entry['test'], api=api,
                        backend=entry.get('backend') or device.get('gpu', {}).get('model'),
                        cpu_level=entry.get('cpu_level') or record.get('cpu_level') or 'native',
                        feature=entry.get('feature') or feature_for(api, device, registry))
            grouping = grouping_of(keys, rules)
            gid = grouping_id(grouping)
            coordinator.update('gold/expectations/%s.json' % gid, lambda current, g=grouping: None if current else {
                'grouping': g, 'digests': {}})
            entry.update(keys=keys, grouping=gid, mode=mode_for(device, rules),
                         ppd=round(image.ppd_for(device.get('screen'), device.get('kind'), policy.get('viewing')), 3),
                         tolerance={k: rules[k] for k in ('flip_mean', 'flip_area', 'flip_pixel', 'ssim_min')
                                    if k in rules}, error=None)
            entry['comparison'] = comparison_for(coordinator, entry, rules)
        except (GoldError, image.ImageError, KeyError) as error:
            entry['error'] = str(error)[:300]
        entries.append(entry)

    def done(current):
        return dict(current, entries=entries, status='done', compared_at=coordinator.clock())
    record = coordinator.update(name, done)
    coordinator.update('gold/pending.json', lambda p: {'results': [r for r in (p or {}).get('results', [])
                                                                   if r != name]})
    waiting = [e for e in entries if not e.get('error') and entry_state(e, expectations(
        coordinator, e['grouping'])) == 'untriaged']
    if waiting:
        try:
            branch = coordinator.build(record['build']).get('branch')
        except Exception:
            branch = None
        coordinator.notify('gold', '%d images need triage (build #%s, %s)' % (
            len(waiting), record['build'], record.get('device')), 'Untriaged: %s. Approve or reject them on the '
            'Gold page; until then their QUAL cells read "needs triage", never pass.' % ', '.join(
                sorted({e['test'] for e in waiting})[:20]), 'gold-triage-%s-%s' % (record['build'], record['job']),
            branch=branch)
    return record


def comparison_for(coordinator, entry, rules):
    """The entry against the grouping's newest positives: the closest one (lowest mean FLIP) with its metrics."""
    expect = expectations(coordinator, entry['grouping'])
    candidates = [d for d in positives(expect) if d != entry['digest']]
    if label_of(expect, entry['digest']) != 'untriaged' or not candidates:
        return {'against': None, 'positives': sorted(candidates)}
    best = None
    for digest in candidates:
        metrics = compare(coordinator.store, digest, entry['digest'], entry['ppd'], rules)
        rank = 2.0 if metrics.get('size_mismatch') else metrics['flip_mean']
        if best is None or rank < best[0]:
            best = (rank, digest, metrics)
    _, digest, metrics = best
    return {'against': digest, 'metrics': metrics, 'within': bool(metrics.get('within')) and entry[
        'mode'] == 'perceptual', 'positives': sorted(candidates)}


def entry_state(entry, expect):
    """pass, fail, untriaged, pending or error: derived from the expectations now, so an approval takes effect at
    once. A perceptual pass within tolerance holds only while the baseline it was compared with stays positive."""
    if entry.get('error'):
        return 'error'
    if not entry.get('digest'):
        return 'pending'
    label = label_of(expect, entry['digest'])
    if label == 'positive':
        return 'pass'
    if label == 'negative':
        return 'fail'
    comparison = entry.get('comparison') or {}
    if comparison.get('within') and label_of(expect, comparison.get('against')) == 'positive':
        return 'pass'
    return 'untriaged'


def stale(coordinator, entry):
    """True when the grouping's positives changed since the entry was compared (a new baseline was approved)."""
    if entry.get('error') or not entry.get('digest'):
        return False
    expect = expectations(coordinator, entry['grouping'])
    if label_of(expect, entry['digest']) != 'untriaged':
        return False
    now = sorted(d for d in positives(expect) if d != entry['digest'])
    return now != (entry.get('comparison') or {}).get('positives')


def tick(coordinator, registry, policy=None, artifact_reader=None, limit=MAX_PER_TICK, recent=100):
    """Process pending results whose job is over, then re-compare the untriaged images of the newest `recent` results
    whose grouping gained a baseline. At most `limit` pending results per call."""
    policy = policy or load_policy()
    reader = artifact_reader or default_reader(coordinator)
    done = []
    for name in list((coordinator.get('gold/pending.json') or {}).get('results', []))[:limit]:
        record = coordinator.get(name)
        if record is None:
            continue
        try:
            job = coordinator.job(record['job'])
        except Exception:
            job = {'state': 'passed', 'attempt': record['attempt']}
        if job['state'] not in state.TERMINAL and job['attempt'] == record['attempt']:
            continue
        done.append(process(coordinator, name, registry, policy, reader))
    for name, record in recent_results(coordinator, recent):
        if record['status'] != 'done' or not any(stale(coordinator, e) for e in record['entries']):
            continue
        entries = []
        for entry in record['entries']:
            if stale(coordinator, entry):
                entry = dict(entry, comparison=comparison_for(coordinator, entry, rules_for(policy, entry['test'])))
            entries.append(entry)
        coordinator.update(name, lambda current, e=entries: dict(current, entries=e))
    return done


def default_reader(coordinator):
    def read(record, path):
        entries = [e for e in coordinator.artifacts(record['build']) if e['job'] == record['job'] and
                   e['attempt'] == record['attempt'] and e['path'] == path]
        if not entries:
            raise GoldError('image %s was not uploaded' % path)
        data = coordinator.store.get(PREFIX + entries[-1]['object'])
        if data is None:
            raise GoldError('image %s was removed by retention' % path)
        return data
    return read


# --------------------------------------------------------------------------------------------- views

def results_of(coordinator, number):
    return [coordinator.store.get_json(n) for n in coordinator.store.list(PREFIX + 'gold/results/%010d/' %
                                                                            int(number))]


def overlay(coordinator, cells):
    """QUAL cells with the gold verdicts of their build: an untriaged or pending image makes a passing cell
    `triage` (never a pass); a negative or unreadable image fails it."""
    cache = {}
    by_build = {}
    for cell in cells:
        if not cell.get('build') or cell['state'] not in ('pass', 'fail', 'cached'):
            continue
        number = str(cell['build'])
        if not number.isdigit():
            continue
        if number not in by_build:
            by_build[number] = [r for r in results_of(coordinator, number) if r]
        counts = {}
        for record in by_build[number]:
            if record.get('device') != cell['device']:
                continue
            if record.get('error'):
                counts['error'] = counts.get('error', 0) + 1
            for entry in record['entries']:
                keys = entry.get('keys') or {}
                if keys and keys.get('feature') != cell['feature']:
                    continue
                gid = entry.get('grouping')
                if gid and gid not in cache:
                    cache[gid] = expectations(coordinator, gid)
                verdict = entry_state(entry, cache.get(gid, {'digests': {}}))
                counts[verdict] = counts.get(verdict, 0) + 1
        if not counts:
            continue
        cell['gold'] = counts
        if counts.get('fail') or counts.get('error'):
            cell['state'] = 'fail'
            cell['reason'] = 'gold: %d rejected or unreadable images' % (counts.get('fail', 0) +
                                                                           counts.get('error', 0))
        elif (counts.get('untriaged') or counts.get('pending')) and cell['state'] != 'fail':
            cell['state'] = 'triage'
            cell['reason'] = 'gold: %d images need triage%s' % (
                counts.get('untriaged', 0), ', %d not yet compared' % counts['pending'] if counts.get(
                    'pending') else '')
    return cells


def triage_queue(coordinator, recent=INDEX, include=('untriaged',)):
    """Untriaged images of the newest results, one item per (grouping, digest) with every occurrence."""
    items = {}
    cache = {}
    for _, record in recent_results(coordinator, recent):
        for entry in record['entries']:
            gid = entry.get('grouping')
            if not gid:
                continue
            if gid not in cache:
                cache[gid] = expectations(coordinator, gid)
            verdict = entry_state(entry, cache[gid])
            if verdict not in include:
                continue
            item = items.setdefault((gid, entry['digest']), {
                'grouping': gid, 'group': cache[gid].get('grouping'), 'digest': entry['digest'],
                'test': entry['test'], 'mode': entry['mode'], 'size': entry.get('size'),
                'label': label_of(cache[gid], entry['digest']), 'state': verdict,
                'comparison': entry.get('comparison'), 'tolerance': entry.get('tolerance'),
                'ppd': entry.get('ppd'), 'occurrences': []})
            item['occurrences'].append({'build': record['build'], 'job': record['job'], 'device': record['device'],
                                        'commit': record.get('commit'), 'keys': entry.get('keys')})
    return sorted(items.values(), key=lambda i: (i['test'], i['digest']))


def compared_text(coordinator, gid, digest):
    for _, record in reversed(recent_results(coordinator)):
        for entry in record['entries']:
            if entry.get('grouping') == gid and entry.get('digest') == digest:
                metrics = (entry.get('comparison') or {}).get('metrics')
                if metrics and not metrics.get('size_mismatch'):
                    return 'with approved image %s at mean FLIP %.4f, SSIM %.4f' % (
                        entry['comparison']['against'][:12], metrics['flip_mean'], metrics['ssim'])
                return 'with no approved image of the same size'
    return 'with nothing'


def finding_text(record):
    return ('Rendering of `%s` rejected in gold triage by %s: image %s on %s (builds %s), compared %s. Owner: file '
            'this as a finding row in docs/ROADMAP.md naming the renderer row, fix the cause, and approve the '
            'corrected image; the rejected image fails every cell that produces it again.' % (
                record['test'], record['by'], record['digest'][:12], ', '.join(record['devices']) or 'no device',
                ', '.join(str(b) for b in record['builds']) or 'none', record['compared']))


def triage(coordinator, gid, digest, expected, label, who, note=''):
    """Move a digest of a grouping from `expected` to `label`, refused when someone moved it first."""
    if label not in LABELS or expected not in LABELS or label not in TRANSITIONS[expected]:
        raise GoldError('a %s image cannot become %s' % (expected, label))
    if not re.match(r'^[0-9a-f]{64}$', str(digest)) or not re.match(r'^[0-9a-f]{24}$', str(gid)):
        raise GoldError('bad grouping or digest')
    from coordinator.scheduler import NotFound, Refused
    now = coordinator.clock()
    seen = {}

    def change(current):
        if current is None:
            raise NotFound('grouping %s' % gid)
        marks = dict(current.get('digests', {}))
        mark = marks.get(digest)
        have = (mark or {}).get('label', 'untriaged')
        if have != expected:
            raise Refused('this image is already %s (by %s): reload to see it' % (have, (mark or {}).get('by')))
        if label == 'untriaged':
            marks.pop(digest, None)
        else:
            marks[digest] = {'label': label, 'by': who, 'at': now, 'note': str(note)[:500]}
        seen['grouping'] = current.get('grouping') or {}
        return dict(current, digests=marks)
    coordinator.update('gold/expectations/%s.json' % gid, change)
    test = seen['grouping'].get('test', '?')
    action = {'positive': 'gold-approve', 'negative': 'gold-reject', 'untriaged': 'gold-reset'}[label]
    coordinator.audit(who, action, '%s %s' % (test, digest[:12]), canonical(seen['grouping'])[:500] + (
        ' note: %s' % note if note else ''), via='ui')
    finding = None
    if label == 'negative':
        occurrences = [o for i in triage_queue(coordinator, include=('untriaged', 'fail', 'pass'))
                       if i['grouping'] == gid and i['digest'] == digest for o in i['occurrences']]
        record = {'id': hashlib.sha256(('%s/%s' % (gid, digest)).encode('utf-8')).hexdigest()[:16], 'test': test,
                  'grouping': seen['grouping'], 'digest': digest, 'by': who, 'at': now, 'note': str(note)[:500],
                  'devices': sorted({o['device'] for o in occurrences if o.get('device')}),
                  'builds': sorted({o['build'] for o in occurrences}),
                  'compared': compared_text(coordinator, gid, digest)}
        record['text'] = finding_text(record)
        coordinator.store.put_json(PREFIX + 'gold/findings/%s.json' % record['id'], record)
        coordinator.notify('gold', 'Gold image rejected: %s' % test, record['text'], 'gold-' + record['id'])
        finding = record
    return {'grouping': gid, 'digest': digest, 'label': label, 'finding': finding}


def findings(coordinator):
    return [coordinator.store.get_json(n) for n in coordinator.store.list(PREFIX + 'gold/findings/')]


# --------------------------------------------------------------------------------------------- worker

class Worker:
    """`tick` on its own thread, so a long comparison never holds up dispatch."""

    def __init__(self, coordinator, registry, seconds=30.0, policy_file=POLICY_FILE):
        self.c = coordinator
        self.registry = registry
        self.seconds = seconds
        self.policy_file = policy_file
        self.stop = threading.Event()

    def run(self):
        while not self.stop.is_set():
            try:
                tick(self.c, self.registry, load_policy(self.policy_file))
            except Exception as error:
                self.c.log('gold_failed', level='error', error=repr(error)[:300])
            self.stop.wait(self.seconds)

    def start(self):
        thread = threading.Thread(target=self.run, name='gold', daemon=True)
        thread.start()
        return thread


# --------------------------------------------------------------------------------------------- Firebase Test Lab

def ftl_manifest(results, device, out, prefix='app-launch'):
    """A gold manifest for the screenshots of a downloaded Firebase Test Lab results tree: every PNG under an
    `artifacts` directory becomes `<prefix>.<file stem>` on the device's first API (its screen data gives the
    pixels per degree on the coordinator). The PNGs are copied into `out`."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    images = []
    for path in sorted(Path(results).rglob('*.png')):
        if 'artifacts' not in path.parts:
            continue
        stem = re.sub(r'[^A-Za-z0-9_.+-]+', '_', path.stem)[:80]
        name = '%s.png' % stem
        shutil.copyfile(path, out / name)
        images.append({'test': '%s.%s' % (prefix, stem), 'file': name,
                       'api': (device.get('gpu', {}).get('apis') or ['Vulkan'])[0], 'feature': 'app-launch',
                       'backend': device.get('gpu', {}).get('model')})
    manifest = {'schema': SCHEMA, 'device': device['id'], 'screen': device.get('screen'), 'images': images}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8')
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description='Cerid CI gold images')
    sub = parser.add_subparsers(dest='command', required=True)
    ftl = sub.add_parser('ftl-manifest', help='a gold manifest from a downloaded Firebase Test Lab results tree')
    ftl.add_argument('--results', required=True)
    ftl.add_argument('--device', required=True)
    ftl.add_argument('--out', required=True)
    ftl.add_argument('--prefix', default='app-launch')
    compare_cmd = sub.add_parser('compare', help='FLIP and SSIM of two PNG files')
    compare_cmd.add_argument('reference')
    compare_cmd.add_argument('test')
    compare_cmd.add_argument('--ppd', type=float, default=image.DEFAULT_PPD)
    compare_cmd.add_argument('--heatmap')
    args = parser.parse_args(argv)
    if args.command == 'ftl-manifest':
        registry = json.loads((CI / 'devices.json').read_text(encoding='utf-8'))
        device = next((d for d in registry['devices'] if d['id'] == args.device), None)
        if device is None:
            parser.error('no device %s in ci/devices.json' % args.device)
        manifest = ftl_manifest(args.results, device, args.out, args.prefix)
        print('%d screenshots in %s' % (len(manifest['images']), os.path.join(args.out, 'manifest.json')))
        return 0
    reference = image.decode_png(Path(args.reference).read_bytes())
    test = image.decode_png(Path(args.test).read_bytes())
    errors = image.flip(reference, test, args.ppd)
    print(json.dumps(dict(image.summarize(errors, 0.1), ssim=image.ssim(reference, test),
                          identical=reference.digest() == test.digest()), indent=1))
    if args.heatmap:
        Path(args.heatmap).write_bytes(image.encode_png(image.heatmap(errors, reference.width, reference.height)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
