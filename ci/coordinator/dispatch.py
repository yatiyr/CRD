"""One-job cloud machines for the Cerid orchestrator (ADR-0135 §2 and §4 as amended by §12).

Every ready job on the cloud queue is routed by its `device` tag to its registry cell and executor, checked against
the budget (the month's ledger plus the worst case of every machine already launched, and the GCP budget stop),
reserved for a machine name and launched. A gce_vm machine needs no credential at all: its agent proves its Google
identity and takes the job reserved for that instance. An ssh_host or firebase_test_lab machine gets one job token
that expires with the job. The reaper ends a machine whose job is finished, retried or canceled, writes its real
cost to the ledger and turns a machine that disappeared under a running job (a preempted spot VM, a lost VM) into an
infrastructure retry on a new machine; the orphan sweep deletes any labelled instance no record knows.
"""
from __future__ import annotations

import datetime as dt
import hashlib

from coordinator import state
from coordinator.scheduler import NotFound

ORPHAN_SECONDS = 900


class Dispatcher:
    def __init__(self, coordinator, registry, executors, environ=None, agent=None, now=None):
        self.c = coordinator
        self.p = coordinator.p
        self.registry = registry
        self.executors = executors
        self.environ = environ or {}
        self.agent = agent or {}
        self.queue = registry['queues']['cloud']
        self.devices = self.p.devices_by_id(registry)
        self.now = now or (lambda: dt.datetime.fromtimestamp(self.c.clock(), dt.timezone.utc))

    def machine_name(self, job):
        return ('cerid-ci-%s-a%d' % (job['id'], job['attempt']))[:62]

    def agent_spec(self, machine, token=None):
        """What a launched machine needs to find and verify its agent: the coordinator's URL, the agent's URL and
        SHA-256 (the startup script refuses any other bytes) and, for non-GCE machines only, its one job token."""
        spec = dict(self.agent, machine=machine)
        if token:
            spec['token'] = token
        return spec

    def running_bound(self):
        """Worst-case cost of every machine still running: they count against the cap before their ledger entry."""
        total = 0.0
        for name in self.c.store.list('machines/'):
            record = self.c.store.get_json(name)
            if record and not record.get('ended'):
                total += float(record.get('bound_usd', 0.0))
        return total

    def tick(self):
        p = self.p
        store = self.c.store
        month = p.month_of(self.now())
        subscription = self.environ.get('CERID_CI_BUDGET_SUBSCRIPTION')
        if subscription:
            try:
                p.pull_budget(store, subscription)
            except Exception as error:  # the ledger still caps the month; the stop is a second, slower bound
                self.c.log('budget_pull_failed', level='warning', error=str(error)[:300])
        cap = float(self.registry['budget']['monthly_cap_usd'])
        stop = p.gcp_budget_stop(store, month)
        spent = p.month_spend(store, month) + self.running_bound()
        self.budget_alerts(month, spent, cap, stop)
        launched = []
        for build, job in self.c.ready_jobs(self.queue):
            device = self.devices.get(job['agents'].get('device'))
            if device is None or device['executor'] not in self.executors or device['executor'] == 'owned_agent':
                self.c.fail_job(job['id'], 'no_device', 'no launchable device for %s' % job['agents'])
                continue
            executor = self.executors[device['executor']]
            hourly = float(device.get('cost_per_hour_usd', 0.0))
            hours = float(device.get('executor_config', {}).get('max_run_hours', 6))
            if job.get('timeout_minutes'):
                hours = min(hours, job['timeout_minutes'] / 60.0 + 0.25)
            bound = round(hourly * hours, 4)
            if hourly > 0 and (stop or spent + bound > cap):
                detail = 'budget: %.2f of %.2f USD this month%s' % (spent, cap, '; GCP budget stop' if stop else '')
                self.c.mark_budget(job['id'], detail)
                # The dashboard shows the cell as budget, never as a failure or a pass.
                store.put_json('results/%s/%s.json' % (build['number'], job['key']), {
                    'schema': p.RESULT_SCHEMA, 'step': job['key'], 'device': device['id'], 'outcome': 'budget',
                    'build': str(build['number']), 'reason': detail,
                    'finished': self.now().isoformat(timespec='seconds')})
                continue
            machine = self.machine_name(job)
            needs_token = device['executor'] != 'gce_vm'
            claimed, token = self.c.reserve(job, machine, issue_token=needs_token)
            if not claimed:
                continue
            self.c.store.put_json('orchestrator/instances/%s.json' % machine, {'job': job['id'],
                                                                              'attempt': claimed['attempt']})
            try:
                handle = executor.launch(machine, device, self.agent_spec(machine, token))
            except Exception as error:  # a stockout or quota refusal is infrastructure: a new attempt, bounded
                self.c.infrastructure(job['id'], 'launch_failed', '%s: %s' % (device['id'], str(error)[:300]),
                                      attempt=claimed['attempt'])
                continue
            spent += bound
            record = {'job': job['id'], 'attempt': claimed['attempt'], 'machine': machine, 'device': device['id'],
                      'executor': device['executor'], 'handle': handle, 'build': build['number'], 'key': job['key'],
                      'started': self.now().isoformat(timespec='seconds'), 'rate': hourly, 'bound_usd': bound,
                      'spot': bool(device.get('executor_config', {}).get('spot', False))}
            store.put_json('machines/%s.json' % machine, record)
            self.c.log('machine_launched', job=job['id'], machine=machine, device=device['id'])
            launched.append(record)
        self.reap()
        return launched

    def budget_alerts(self, month, spent, cap, stop):
        """One notification per month and threshold: 80 % and 100 % of the cap (ledger plus running machines' worst
        case) and the GCP budget stop."""
        for fraction in (0.8, 1.0):
            if cap and spent >= fraction * cap:
                self.c.notify('budget', 'CI spend at %d %% of the %.0f USD cap' % (fraction * 100, cap),
                              '%s: %.2f USD recorded or committed of %.2f USD. Paid launches past the cap end as '
                              'budget.' % (month, spent, cap), key='budget:%s:%s' % (month, fraction))
        if stop:
            self.c.notify('budget', 'GCP budget stop in force', 'The Cloud Billing budget reports the month at its '
                          'amount; no paid machine starts until next month.', key='budget:%s:gcp-stop' % month)

    def reap(self):
        """End machines whose job no longer needs them; turn a vanished machine under a live job into a retry."""
        store = self.c.store
        ended = []
        for name in store.list('machines/'):
            record = store.get_json(name)
            if not record or record.get('ended'):
                continue
            try:
                job = self.c.job(record['job'])
            except NotFound:
                job = None
            executor = self.executors.get(record['executor'])
            done = job is None or job['state'] in state.TERMINAL or job['attempt'] != record['attempt'] \
                or job.get('machine') != record['machine']
            if not done and executor is not None and hasattr(executor, 'alive'):
                try:
                    alive = executor.alive(record['handle'])
                except Exception as error:
                    self.c.log('machine_probe_failed', level='warning', machine=record['machine'], error=str(error))
                    alive = None
                if alive is False:
                    reason = 'vm_lost'
                    if record.get('spot') and hasattr(executor, 'lost_reason'):
                        try:
                            reason = executor.lost_reason(record['handle'])
                        except Exception as error:
                            self.c.log('machine_probe_failed', level='warning', machine=record['machine'],
                                       error=str(error))
                    self.c.infrastructure(job['id'], reason, 'machine %s disappeared' % record['machine'],
                                          attempt=record['attempt'])
                    done = True
            if not done:
                continue
            if executor is not None:
                try:
                    executor.teardown(record['handle'])
                except Exception as error:  # try again next tick; the record stays open until it is gone
                    self.c.log('teardown_failed', level='error', machine=record['machine'], error=str(error)[:300])
                    continue
            now = self.now()
            started = dt.datetime.fromisoformat(record['started'])
            hours = max((now - started).total_seconds(), 60.0) / 3600.0
            record['ended'] = now.isoformat(timespec='seconds')
            record['usd'] = round(hours * record['rate'], 4)
            store.put_json(name, record)
            store.put_json('ledger/%s/%s.json' % (self.p.month_of(now), record['machine']),
                           {'device': record['device'], 'hours': round(hours, 4), 'usd': record['usd'],
                            'job': record['job'], 'build': record.get('build'), 'key': record.get('key')})
            self.c.log('machine_ended', machine=record['machine'], usd=record['usd'])
            ended.append(record)
        return ended

    def sweep_orphans(self):
        """Delete labelled machines no open record knows (a coordinator that died between launch and record)."""
        known = set()
        for name in self.c.store.list('machines/'):
            record = self.c.store.get_json(name)
            if record and not record.get('ended'):
                known.add(record['machine'])
        removed = []
        now = self.c.clock()
        for executor in {id(e): e for e in self.executors.values()}.values():
            if not hasattr(executor, 'machines'):
                continue
            for machine in executor.machines():
                if machine['name'] not in known and now - machine.get('created', now) > ORPHAN_SECONDS:
                    executor.teardown(machine['handle'])
                    removed.append(machine['name'])
                    self.c.log('orphan_deleted', level='warning', machine=machine['name'])
        return removed


def agent_digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
