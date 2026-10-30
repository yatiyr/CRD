"""Test quality for the Cerid orchestrator: per-test history and duration trends, flaky-test detection, quarantine
with an expiry and a tracked issue, and first-bad-commit assistance (ADR-0135 §16).

Every lane writes one record per run to the result store, `test-runs/<preset>@<device>/<build>.json`: each test's
outcome, duration and test key (the executable's bytes, arguments, properties, device and runtime inputs, the same
key the per-test cache uses). From those records:

- **History**: a test's outcome and duration across the lane's last runs.
- **Flaky**: a test is flaky only on proof: its *same* key (identical binary, arguments, device and inputs) both
  passed and failed. A test that fails on a changed key is a regression, never flaky.
- **Quarantine**: a proven-flaky test is quarantined for a bounded time (default 7 days, at most `max_active` at
  once) with an issue record carrying the evidence and the text of the ROADMAP finding the owner files. A quarantined
  test still runs; its failure is reported (annotation, notification, the Quality page) but does not fail the lane;
  nothing is skipped or hidden, and the quarantine ends by itself.
- **First bad commit**: the tests that passed in the lane's previous run on a branch and fail now open one
  bisection together (one regression usually breaks many tests): the recorded runs between the two narrow the range
  for free, then targeted re-runs of those tests alone on the middle commit (`pipeline.py lane --tests`) narrow it
  further, at most three times, through the normal dispatcher and budget. At most two bisections run at once; the
  rest wait `queued`. The first bad commit is the first one where any of the tests fails.
"""
from __future__ import annotations

import hashlib
import re
import time

RUNS = 'test-runs/'
WINDOW = 30
QUARANTINE_DAYS = 7
MAX_ACTIVE = 10
MAX_RERUNS = 3
MAX_BISECTIONS = 2


def lane_id(preset, device):
    return '%s@%s' % (preset, device)


def ident(*parts):
    return hashlib.sha256('\0'.join(parts).encode('utf-8')).hexdigest()[:32]


def lanes(store):
    return sorted({name[len(RUNS):].split('/', 1)[0] for name in store.list(RUNS) if '/' in name[len(RUNS):]})


def runs(store, lane, window=WINDOW):
    """The lane's last `window` run records, oldest first."""
    names = store.list('%s%s/' % (RUNS, lane))[-window:]
    return [r for r in (store.get_json(n) for n in names) if r]


def history(store, lane, test, window=WINDOW):
    """[{build, commit, branch, outcome, seconds, key}] of one test, oldest first."""
    out = []
    for run in runs(store, lane, window):
        entry = run.get('tests', {}).get(test)
        if entry:
            out.append({'build': run.get('build'), 'commit': run.get('commit'), 'branch': run.get('branch'),
                        'finished': run.get('finished'), **entry})
    return out


def summary(store, lane, window=WINDOW):
    """Per test: runs, passes, failures, the last outcome, median and newest duration, flaky flag."""
    tests = {}
    for run in runs(store, lane, window):
        for name, entry in run.get('tests', {}).items():
            item = tests.setdefault(name, {'test': name, 'runs': 0, 'pass': 0, 'fail': 0, 'durations': [],
                                           'keys': {}, 'last': None})
            item['runs'] += 1
            if entry['outcome'] in ('pass', 'fail'):
                item[entry['outcome']] += 1
            item['durations'].append(float(entry.get('seconds') or 0.0))
            item['keys'].setdefault(entry.get('key'), set()).add(entry['outcome'])
            item['last'] = entry['outcome']
    out = []
    for item in tests.values():
        durations = sorted(item['durations'])
        out.append({'test': item['test'], 'runs': item['runs'], 'pass': item['pass'], 'fail': item['fail'],
                    'last': item['last'], 'median_s': durations[len(durations) // 2] if durations else 0.0,
                    'last_s': item['durations'][-1] if item['durations'] else 0.0,
                    'flaky': any({'pass', 'fail'} <= outcomes for key, outcomes in item['keys'].items() if key)})
    return sorted(out, key=lambda t: (not t['flaky'], -t['fail'], t['test']))


def flaky(store, lane, window=WINDOW):
    """{test: evidence} for every test whose identical key both passed and failed in the window."""
    seen = {}
    for run in runs(store, lane, window):
        for name, entry in run.get('tests', {}).items():
            key = entry.get('key')
            if not key or entry['outcome'] not in ('pass', 'fail'):
                continue
            seen.setdefault((name, key), {}).setdefault(entry['outcome'], []).append(str(run.get('build')))
    found = {}
    for (name, key), outcomes in seen.items():
        if 'pass' in outcomes and 'fail' in outcomes:
            found[name] = {'key': key, 'passed_in': outcomes['pass'][-5:], 'failed_in': outcomes['fail'][-5:]}
    return found


# --------------------------------------------------------------------------------------------- quarantine

def quarantine_name(lane, test):
    return 'quarantine/%s.json' % ident(lane, test)


def active_quarantine(store, lane, now, prefix='orchestrator/'):
    """{test: record} of the lane's unexpired quarantines (read by the lane before it judges its tests)."""
    out = {}
    for name in store.list(prefix + 'quarantine/'):
        record = store.get_json(name)
        if record and record['lane'] == lane and record.get('active', True) and record['expires'] > now:
            out[record['test']] = record
    return out


def issue_text(record):
    return ('Flaky test `%s` on lane `%s`: the same test key (%s) passed in builds %s and failed in builds %s. '
            'Quarantined by the CI from %s to %s (it still runs; its failures are reported, not hidden). Owner: '
            'file this as a finding row in docs/ROADMAP.md, fix the cause, then release the quarantine '
            '(`python ci/ops.py quarantine release --lane %s --test %s`).' % (
                record['test'], record['lane'], record['key'][:12], ', '.join(record['passed_in']),
                ', '.join(record['failed_in']), record['since_text'], record['expires_text'], record['lane'],
                record['test']))


def scan(coordinator, window=WINDOW, days=QUARANTINE_DAYS, max_active=MAX_ACTIVE, stamp=None):
    """Quarantine newly proven flaky tests, expire old quarantines; returns (quarantined, expired) records."""
    c = coordinator
    now = c.clock()
    stamp = stamp or (lambda t: time.strftime('%Y-%m-%d', time.gmtime(t)))
    quarantined, expired = [], []
    active = sum(1 for n in c.store.list('orchestrator/quarantine/')
                 if (c.store.get_json(n) or {}).get('active') and (c.store.get_json(n) or {})['expires'] > now)
    for lane in lanes(c.store):
        for test, evidence in sorted(flaky(c.store, lane, window).items()):
            name = quarantine_name(lane, test)
            current = c.get(name)
            if current and (current['expires'] > now or current.get('key') == evidence['key']):
                continue  # already quarantined, or this exact evidence already served its quarantine
            if active >= max_active:
                c.log('quarantine_limit', level='warning', lane=lane, test=test)
                continue
            issue = 'CI-FLAKY-%s' % ident(lane, test, evidence['key'])[:8]
            record = {'lane': lane, 'test': test, 'key': evidence['key'], 'passed_in': evidence['passed_in'],
                      'failed_in': evidence['failed_in'], 'since': now, 'expires': now + days * 86400,
                      'since_text': stamp(now), 'expires_text': stamp(now + days * 86400), 'issue': issue,
                      'active': True, 'by': 'flaky-detector'}
            if c.update(name, lambda cur: record if not cur or cur['expires'] <= now else None) is not record:
                continue
            c.create('issues/%s.json' % issue, {'id': issue, 'kind': 'flaky', 'state': 'open', 'created': now,
                                                'lane': lane, 'test': test, 'text': issue_text(record)})
            c.audit('flaky-detector', 'quarantine', '%s %s' % (lane, test), 'expires %s' % record['expires_text'])
            c.notify('flaky', 'Flaky test quarantined: %s' % test, issue_text(record), key='flaky:%s' % issue)
            active += 1
            quarantined.append(record)
    for name in c.store.list('orchestrator/quarantine/'):
        record = c.store.get_json(name)
        if record and record.get('active') and record['expires'] <= now:
            c.p.update_json(c.store, name, lambda r: dict(r, active=False, ended=now, end_reason='expired')
                            if r.get('active') else None)
            c.notify('quarantine_expired', 'Quarantine expired: %s' % record['test'],
                     'The quarantine of %s on %s ended; its failures fail the lane again. Issue %s.' % (
                         record['test'], record['lane'], record['issue']), key='expired:%s:%s' % (name, record['since']))
            expired.append(record)
    return quarantined, expired


def release(coordinator, lane, test, by):
    c = coordinator
    name = quarantine_name(lane, test)
    record = c.update(name, lambda r: dict(r, active=False, ended=c.clock(), end_reason='released by %s' % by)
                      if r and r.get('active') else None)
    if not record:
        raise LookupError('no quarantine for %s on %s' % (test, lane))
    c.audit(by, 'quarantine-release', '%s %s' % (lane, test), '')
    return record


def quarantine_view(junit_outcomes, quarantined):
    """(failed, quarantined failures, other failures) of one lane run."""
    failed = sorted(name for name, outcome in junit_outcomes.items() if outcome == 'fail')
    held = [name for name in failed if name in quarantined]
    return failed, held, [name for name in failed if name not in quarantined]


# --------------------------------------------------------------------------------------------- first bad commit

def newly_failing(store, lane):
    """(tests, good run, bad run): the tests that passed in the lane's previous run on the same branch and fail in its
    newest run (one regression usually breaks many tests at once: they are bisected together), or None."""
    recorded = runs(store, lane, WINDOW)
    if len(recorded) < 2:
        return None
    bad = recorded[-1]
    previous = [r for r in recorded[:-1] if r.get('branch') == bad.get('branch') and not r.get('bisect')]
    if not previous or bad.get('bisect'):
        return None
    good = previous[-1]
    tests = sorted(test for test, entry in bad.get('tests', {}).items()
                   if entry['outcome'] == 'fail' and good.get('tests', {}).get(test, {}).get('outcome') == 'pass')
    return (tests, good, bad) if tests else None


ACTIVE_BISECTION = ('narrowing', 'rerunning')
TEST_NAME = re.compile(r'^[A-Za-z0-9_.:/+-]{1,200}$')


def open_bisections(coordinator, flaky_tests=None, max_active=MAX_BISECTIONS):
    """One bisection per lane and newly failing run, carrying all its newly failing (non-flaky) tests; past
    `max_active` running bisections a new one waits `queued`. Returns the records opened."""
    c = coordinator
    opened = []
    for lane in lanes(c.store):
        found = newly_failing(c.store, lane)
        if not found:
            continue
        tests, good, bad = found
        known_flaky = flaky_tests.get(lane, {}) if flaky_tests is not None else flaky(c.store, lane)
        tests = [t for t in tests if t not in known_flaky and TEST_NAME.match(t)]
        if not tests:
            continue
        bisect_id = ident(lane, str(bad.get('build')))
        active = sum(1 for n in c.store.list('orchestrator/bisect/')
                     if (c.store.get_json(n) or {}).get('state') in ACTIVE_BISECTION)
        record = {'id': bisect_id, 'lane': lane, 'tests': tests,
                  'state': 'narrowing' if active < max_active else 'queued',
                  'repository': bad.get('repository', 'cerid'), 'branch': bad.get('branch'),
                  'good': {'build': good.get('build'), 'commit': good.get('commit')},
                  'bad': {'build': bad.get('build'), 'commit': bad.get('commit')}, 'reruns': [],
                  'opened': c.clock(), 'first_bad': None}
        if c.create('bisect/%s.json' % bisect_id, record):
            c.audit('bisector', 'bisect-open', '%s: %d tests' % (lane, len(tests)), 'good %s bad %s%s' % (
                good.get('build'), bad.get('build'), ', queued' if record['state'] == 'queued' else ''))
            opened.append(record)
    return opened


def narrow(record, candidates, outcome_of):
    """Move `good` and `bad` inward using the known outcomes of `candidates` (commits strictly between good and bad,
    oldest first; a test that once fails keeps failing): the newest passing commit becomes good, the oldest failing
    commit after it becomes bad. Returns the commits still unknown between them."""
    inside = list(candidates)
    outcomes = [outcome_of(commit) for commit in inside]
    last_pass = max((i for i, outcome in enumerate(outcomes) if outcome == 'pass'), default=-1)
    if last_pass >= 0:
        record['good']['commit'] = inside[last_pass]
    end = len(inside)
    for i in range(last_pass + 1, len(inside)):
        if outcomes[i] == 'fail':
            record['bad']['commit'] = inside[i]
            end = i
            break
    return inside[last_pass + 1:end]


def describe(record):
    tests = record['tests']
    return tests[0] if len(tests) == 1 else '%s and %d more' % (tests[0], len(tests) - 1)


def advance(coordinator, record, commits_between, outcome_of, start_rerun):
    """One step of a bisection: narrow with every known outcome, then either name the first bad commit (the first
    commit where any of its tests fails), start one targeted re-run on the middle commit, or stop when the re-run
    budget is spent."""
    c = coordinator
    if record['state'] not in ACTIVE_BISECTION:
        return record
    unknown = narrow(record, commits_between(record['good']['commit'], record['bad']['commit']), outcome_of)
    pending = [r for r in record['reruns'] if r.get('outcome') is None]
    if pending:
        return record
    if not unknown:
        record['state'] = 'found'
        record['first_bad'] = record['bad']['commit']
        c.notify('bisect', 'First bad commit for %s' % describe(record),
                 'Lane %s: %s is the first commit where %s fail (last good %s).' % (
                     record['lane'], record['first_bad'], ', '.join(record['tests']), record['good']['commit']),
                 key='bisect:%s' % record['id'])
    elif len(record['reruns']) >= MAX_RERUNS:
        record['state'] = 'stopped'
        record['remaining'] = unknown
    else:
        middle = unknown[len(unknown) // 2]
        build = start_rerun(record, middle)
        record['state'] = 'rerunning'
        record['reruns'].append({'commit': middle, 'build': build, 'outcome': None})
    return record


def regex_escape(name):
    return re.sub(r'([.^$*+?{}()|\[\]\\])', r'\\\1', name)


def rerun_command(lane, tests):
    """The lane command re-running only `tests`: one CTest expression, double-quoted, which both the agent's shells
    (bash and cmd.exe) pass through unchanged for these names (letters, digits and `_.:/+-`)."""
    preset, device = lane.split('@', 1)
    for test in tests:
        if not TEST_NAME.match(test):
            raise ValueError('test name %r cannot be passed through a shell safely' % test)
    return 'python ci/pipeline.py lane --preset %s --device %s --tests "^(%s)$"' % (
        preset, device, '|'.join(regex_escape(t) for t in tests))


def outcomes_at(store, lane, tests, window=200):
    """{commit: 'pass'|'fail'} of a set of tests from every recorded run of the lane (bisection re-runs included):
    fail when any of them failed, pass when all of them passed, unknown otherwise."""
    found = {}
    for run in runs(store, lane, window):
        if not run.get('commit'):
            continue
        entries = [run.get('tests', {}).get(test) for test in tests]
        if any(e and e['outcome'] == 'fail' for e in entries):
            found[run['commit']] = 'fail'
        elif all(e and e['outcome'] == 'pass' for e in entries):
            found.setdefault(run['commit'], 'pass')
    return found


def tick(coordinator, registry, commits_between, max_active=MAX_BISECTIONS):
    """Drive the bisections one step: open new ones, promote queued ones while fewer than `max_active` run, narrow
    with the recorded runs, settle finished re-runs, start the next. `commits_between(repository, good, bad)` lists
    the commits strictly between, oldest first."""
    c = coordinator
    opened = open_bisections(c, max_active=max_active)
    devices = {d['id']: d for d in registry['devices']}
    queues = registry['queues']

    def start(rec, commit):
        preset, device_id = rec['lane'].split('@', 1)
        device = devices.get(device_id, {'executor': 'gce_vm', 'id': device_id})
        queue = queues['owned'] if device.get('executor') == 'owned_agent' else queues['cloud']
        build, _ = c.create_build(
            rec['repository'], {'event': 'push', 'branch': rec['branch'] or 'main', 'commit': commit,
                                'trusted': True, 'message': 'bisect %s on %s' % (describe(rec), rec['lane'])},
            'manual', created_by='bisector', env={'CERID_CI_BISECT': rec['id']},
            document={'steps': [{'key': 'bisect', 'label': 'bisect %s' % describe(rec),
                                 'command': rerun_command(rec['lane'], rec['tests']),
                                 'agents': {'queue': queue, 'device': device_id}, 'priority': -1,
                                 'retry': {'automatic': [{'reason': 'infrastructure', 'limit': 2}]}}]})
        return build['number']

    def step(name, record):
        known = outcomes_at(c.store, record['lane'], record['tests'])
        for rerun in record['reruns']:
            if rerun.get('outcome') is None:
                build = c.get(c.build_name(rerun['build'])) if rerun.get('build') else None
                if rerun['commit'] in known:
                    rerun['outcome'] = known[rerun['commit']]
                elif build is not None and build['state'] in ('passed', 'failed', 'canceled'):
                    rerun['outcome'] = 'error'
        if any(r.get('outcome') == 'error' for r in record['reruns']):
            record['state'] = 'stopped'
            record['reason'] = 'a re-run ended without running the tests (build or infrastructure failure)'
        else:
            advance(c, record, lambda good, bad: commits_between(record['repository'], good, bad),
                    lambda commit: known.get(commit), start)
        c.store.put_json(name, record)

    records = {name: c.store.get_json(name) for name in c.store.list('orchestrator/bisect/')}
    records = {name: record for name, record in records.items() if record}
    for name, record in records.items():
        if record['state'] in ACTIVE_BISECTION:
            step(name, record)
    # Queued bisections start, oldest first, as running ones end (in this same pass).
    active = sum(1 for r in records.values() if r['state'] in ACTIVE_BISECTION)
    for name, record in sorted(records.items(), key=lambda item: item[1].get('opened', 0)):
        if record['state'] == 'queued' and active < max_active:
            record['state'] = 'narrowing'
            active += 1
            step(name, record)
    return opened
