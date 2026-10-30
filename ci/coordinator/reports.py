"""Reports of the Cerid orchestrator (ADR-0135 §16): cost per build, lane, device and month against the cap with a
forecast, the audit log, and the service-level objectives for queue time and coordinator availability.

SLOs and how they are measured:

- **Queue time**: from the moment a step is ready (its dependencies done) to the moment an agent or a machine takes
  it. Every assignment appends its wait to `orchestrator/metrics/queue/<date>.json`. Targets per queue come from
  `slo.queue_seconds` in ci/coordinator.json (defaults: launcher 120 s, owned 300 s, cloud 900 s, the cloud target
  including a VM's boot); the objective is that 95 % of assignments in the window meet their target.
- **Coordinator availability**: the watchdog marks each minute it ticks in `orchestrator/metrics/ticks/<date>.json`;
  availability is ticked minutes over elapsed minutes in the window (default objective 99.5 % over 30 days).
"""
from __future__ import annotations

import calendar
import time

DEFAULT_SLO = {'queue_seconds': {'cerid-launcher': 120, 'cerid-owned': 300, 'cerid-cloud': 900, 'default': 300},
               'queue_objective': 0.95, 'availability_objective': 0.995, 'window_days': 30}
QUEUE_SAMPLES_PER_DAY = 5000


def day(when):
    return time.strftime('%Y-%m-%d', time.gmtime(when))


def record_queue(coordinator, queue, seconds, now):
    def change(current):
        samples = (current or {}).get('samples', [])
        return {'samples': (samples + [[queue, round(max(seconds, 0.0), 1)]])[-QUEUE_SAMPLES_PER_DAY:]}
    coordinator.update('metrics/queue/%s.json' % day(now), change)


def record_tick(coordinator, now):
    minute = int(now // 60) % 1440

    def change(current):
        minutes = set((current or {}).get('minutes', []))
        if minute in minutes:
            return None
        return {'minutes': sorted(minutes | {minute})}
    coordinator.update('metrics/ticks/%s.json' % day(now), change)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))]


def slo(coordinator, now=None, config=None):
    c = coordinator
    now = c.clock() if now is None else now
    spec = dict(DEFAULT_SLO, **(config or {}))
    targets = dict(DEFAULT_SLO['queue_seconds'], **spec.get('queue_seconds', {}))
    days = [day(now - i * 86400) for i in range(int(spec['window_days']))]
    queues = {}
    for name in days:
        for queue, seconds in (c.get('metrics/queue/%s.json' % name) or {}).get('samples', []):
            queues.setdefault(queue, []).append(seconds)
    queue_rows = []
    for queue, samples in sorted(queues.items()):
        target = targets.get(queue, targets['default'])
        within = sum(1 for s in samples if s <= target) / len(samples)
        queue_rows.append({'queue': queue, 'samples': len(samples), 'p50_s': percentile(samples, 0.5),
                           'p95_s': percentile(samples, 0.95), 'target_s': target, 'within': round(within, 4),
                           'objective': spec['queue_objective'], 'met': within >= spec['queue_objective']})
    ticked = 0
    elapsed = 0
    first_seen = None
    for name in reversed(days):
        minutes = (c.get('metrics/ticks/%s.json' % name) or {}).get('minutes')
        if minutes is None and first_seen is None:
            continue
        start = calendar.timegm(time.strptime(name, '%Y-%m-%d'))
        if first_seen is None:
            first_seen = start + 60 * min(minutes)
            start = first_seen
        end = min(start - start % 86400 + 86400, now)
        elapsed += max(int((end - start) // 60), 0)
        ticked += len(minutes or [])
    availability = (ticked / elapsed) if elapsed else None
    return {'window_days': spec['window_days'], 'queues': queue_rows,
            'availability': None if availability is None else round(min(availability, 1.0), 5),
            'availability_objective': spec['availability_objective'],
            'availability_met': availability is None or availability >= spec['availability_objective'],
            'ticked_minutes': ticked, 'elapsed_minutes': elapsed}


def costs(coordinator, registry, month, now=None):
    """Spend of the month by build, lane (step key) and device, against the cap, with a linear forecast."""
    c = coordinator
    p = c.p
    now = c.clock() if now is None else now
    entries = [e for e in (c.store.get_json(n) for n in c.store.list('ledger/%s/' % month)) if e]
    total = round(sum(float(e.get('usd', 0.0)) for e in entries), 4)

    def group(field):
        out = {}
        for entry in entries:
            name = str(entry.get(field) or '?')
            item = out.setdefault(name, {field: name, 'usd': 0.0, 'hours': 0.0, 'machines': 0})
            item['usd'] = round(item['usd'] + float(entry.get('usd', 0.0)), 4)
            item['hours'] = round(item['hours'] + float(entry.get('hours', 0.0)), 4)
            item['machines'] += 1
        return sorted(out.values(), key=lambda i: -i['usd'])
    running = [r for r in (c.store.get_json(n) for n in c.store.list('machines/')) if r and not r.get('ended')]
    bound = round(sum(float(r.get('bound_usd', 0.0)) for r in running), 4)
    year, mon = (int(x) for x in month.split('-'))
    days_in_month = calendar.monthrange(year, mon)[1]
    start = calendar.timegm((year, mon, 1, 0, 0, 0))
    elapsed_days = max(min((now - start) / 86400.0, days_in_month), 1 / 24.0)
    forecast = round(total / elapsed_days * days_in_month, 2) if month == p.month_of(
        __import__('datetime').datetime.fromtimestamp(now, __import__('datetime').timezone.utc)) else total
    cap = float(registry['budget']['monthly_cap_usd'])
    return {'month': month, 'cap_usd': cap, 'spent_usd': total, 'running': len(running), 'running_bound_usd': bound,
            'forecast_usd': forecast, 'forecast_over_cap': forecast > cap, 'gcp_stop': p.gcp_budget_stop(c.store, month),
            'by_build': group('build'), 'by_lane': group('key'), 'by_device': group('device'),
            'months': [{'month': m, 'usd': p.month_spend(c.store, m)} for m in recent_months(month, 6)]}


def recent_months(month, count):
    year, mon = (int(x) for x in month.split('-'))
    out = []
    for _ in range(count):
        out.append('%04d-%02d' % (year, mon))
        mon -= 1
        if mon == 0:
            year, mon = year - 1, 12
    return list(reversed(out))


def audit_entries(coordinator, days=7, now=None, limit=500):
    c = coordinator
    now = c.clock() if now is None else now
    out = []
    for i in range(days):
        for name in reversed(c.store.list('orchestrator/audit/%s/' % day(now - i * 86400))):
            entry = c.store.get_json(name)
            if entry:
                out.append(entry)
            if len(out) >= limit:
                return out
    return out
