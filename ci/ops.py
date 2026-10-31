#!/usr/bin/env python3
"""ci/ops.py: the one operator command of the Cerid CI platform (ADR-0135).

Usage: python ci/ops.py [--json] [--no-color] [--store URL] [--config FILE] <noun> <verb> [options]

  ui                         open the web UI through the IAP tunnel (gcloud) in your browser
  status | health           the registry's cells and the spend | the coordinator, store, jobs, agents
  build  list|show|start|cancel          job  show|logs|retry|cancel|reproduce
  agent  list|stop          token create|revoke          queue list
  test   list|history|flaky quarantine list|release      bisect list
  cost   [--month --by]     slo                          audit list [--days]
  notify test               backup create|restore         retention run
  device add|bake|list      accounts | parity            completion bash|zsh|powershell

Every command takes --json (machine-readable output on standard output, nothing else). Human output is coloured on
a terminal and plain otherwise (NO_COLOR and --no-color are honoured). An error says what went wrong and what to do
next, and exits 2 (1 means the command ran and found a problem, such as an unhealthy coordinator). The legacy verbs
`add-device`, `bake`, `budget`, `builds`, `agents`, `queues`, `logs`, `retry`, `cancel`, `agent-token`, `backup` and
`restore` keep working.
"""
from __future__ import annotations

from pathlib import Path
import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / 'ci') not in sys.path:
    sys.path.insert(0, str(ROOT / 'ci'))


def pipeline():
    if 'ci_pipeline' in sys.modules:
        return sys.modules['ci_pipeline']
    spec = importlib.util.spec_from_file_location('ci_pipeline', ROOT / 'ci/pipeline.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules['ci_pipeline'] = module
    spec.loader.exec_module(module)
    return module


class OpsError(Exception):
    """A failure the operator can act on: what happened and what to do next."""

    def __init__(self, message, hint=''):
        super().__init__(message)
        self.hint = hint


class Out:
    """Human output: colour only on a terminal, never with NO_COLOR or --no-color."""

    CODES = {'ok': '32', 'bad': '31', 'warn': '33', 'dim': '2', 'bold': '1', 'head': '1;36'}
    STATE = {'passed': 'ok', 'pass': 'ok', 'running': 'warn', 'assigned': 'warn', 'scheduled': 'dim',
             'failed': 'bad', 'fail': 'bad', 'timed_out': 'bad', 'expired': 'bad', 'canceled': 'dim',
             'cached': 'ok', 'budget': 'warn', 'idle': 'ok', 'busy': 'warn', 'lost': 'bad', 'stopped': 'dim',
             'configured': 'ok', 'missing': 'bad', 'runnable': 'ok', 'met': 'ok', 'missed': 'bad'}
    MARK = {'ok': '✓', 'bad': '✗', 'warn': '●', 'dim': '○'}

    def __init__(self, color):
        self.color = color

    def paint(self, text, style):
        code = self.CODES.get(style)
        return '\033[%sm%s\033[0m' % (code, text) if self.color and code else str(text)

    def state(self, value):
        style = self.STATE.get(str(value).split(' ')[0], '')
        mark = self.MARK.get(style, ' ') if self.color else ''
        return self.paint(('%s %s' % (mark, value)).strip(), style)

    def table(self, headers, rows):
        """Aligned columns; widths measured on the text without colour codes."""
        import re
        plain = lambda s: re.sub(r'\033\[[0-9;]*m', '', str(s))
        widths = [max([len(plain(h))] + [len(plain(r[i])) for r in rows]) for i, h in enumerate(headers)]
        lines = ['  '.join(self.paint(h.ljust(w), 'head') for h, w in zip(headers, widths)).rstrip()]
        for row in rows:
            lines.append('  '.join(str(cell) + ' ' * (w - len(plain(cell))) for cell, w in zip(row, widths)).rstrip())
        return lines


def use_color(args, stream=None):
    stream = stream or sys.stdout
    return not args.no_color and not os.environ.get('NO_COLOR') and hasattr(stream, 'isatty') and stream.isatty()


def when(stamp):
    return time.strftime('%Y-%m-%d %H:%M:%SZ', time.gmtime(stamp)) if stamp else '-'


def secret_names(environ):
    """Secret Manager names of the CI project when credentials exist; an empty set otherwise (names only)."""
    project = environ.get('CERID_CI_GCP_PROJECT')
    if not project:
        return set()
    try:
        from executors import gce_vm
        return set(gce_vm.secret_names(project))
    except Exception:  # no credentials here: the report says unknown rather than guessing
        return set()


def accounts_report(registry, environ, secrets):
    p = pipeline()
    rows = []
    for name, spec in registry['accounts'].items():
        present = p.account_present(name, spec, environ, secrets)
        users = [d['id'] for d in registry['devices'] if name in d.get('accounts', [])]
        where = spec.get('name')
        if spec.get('env_file'):
            where = '%s (secret) or %s (file path)' % (spec['name'], spec['env_file'])
        elif spec.get('secret'):
            where = '%s with secret %s' % (spec['name'], spec['secret'])
        state = 'configured' if present else ('user program' if spec['kind'] == 'program' else 'missing')
        if spec.get('optional') and not present:
            state = 'missing (optional)'
        rows.append((name, spec['kind'], where, state, len(users), spec.get('recipe', '')))
    return rows


def device_entry(args):
    entry = {
        'id': args.id, 'status': args.status, 'kind': args.kind,
        'os': {'name': args.os, 'version': args.os_version},
        'cpu': {'model': args.cpu_model, 'arch': args.arch, 'levels': args.levels.split(',')},
        'gpu': {'vendor': args.gpu_vendor, 'model': args.gpu_model,
                'apis': [a for a in (args.apis or '').split(',') if a]},
        'executor': args.executor, 'executor_config': json.loads(args.config_json or '{}'), 'tier': args.tier,
        'cost_class': args.cost_class, 'cost_per_hour_usd': args.cost,
        'features': args.features.split(','), 'accounts': [a for a in (args.accounts or '').split(',') if a],
        'route': args.route,
    }
    if args.screen:
        size, dpi, hz, hdr = args.screen.split(',')
        entry['screen'] = {'size_in': float(size), 'density_dpi': int(dpi), 'refresh_hz': int(hz),
                           'hdr': hdr.lower() in ('1', 'true', 'yes')}
    if args.lane_presets:
        entry['lane_presets'] = args.lane_presets.split(',')
    if args.executor == 'owned_agent':
        entry['executor_config'].setdefault('queue', 'cerid-owned')
    return entry


def insert_device(text, entry):
    """Append one device to the registry text, keeping every existing line as written."""
    end = text.rstrip().rfind(']')
    if end < 0:
        raise ValueError('registry has no devices array')
    body = json.dumps(entry, indent=2, ensure_ascii=False)
    body = '\n'.join('    ' + line for line in body.splitlines())
    head = text[:end].rstrip()
    return head + ',\n' + body + '\n  ' + text[end:]


def orchestrator(ctx):
    """The coordinator library on the state store, configured like the running coordinator. Every command works
    beside a running coordinator: all writes are compare-and-swap."""
    from coordinator import main as coordinator_main, scheduler
    config, _ = coordinator_main.load_config(ctx.args.config, ctx.environ)
    store = pipeline().open_store(ctx.args.store)
    log = scheduler.JsonLog(open(os.devnull, 'w'))
    return coordinator_main.make_coordinator(config, ctx.environ, store, log, ctx.registry)


def who(environ):
    import getpass
    try:
        name = getpass.getuser()
    except Exception:
        name = 'unknown'
    return 'ops.py:%s' % name


# --------------------------------------------------------------------------------------------- commands
# Every command returns (exit code, data for --json, human lines).

def cmd_accounts(ctx):
    rows = accounts_report(ctx.registry, ctx.environ, secret_names(ctx.environ))
    data = [{'account': r[0], 'kind': r[1], 'where': r[2], 'state': r[3], 'cells': r[4], 'recipe': r[5]}
            for r in rows]
    lines = ['Account-backed inputs (names only; values are never read). Setup: docs/recipes/ci-platform-setup.md']
    lines += ctx.out.table(['ACCOUNT', 'KIND', 'STATE', 'CELLS', 'WHERE'], [
        (r[0], r[1], ctx.out.state(r[3]), r[4], '%s  [#%s]' % (r[2], r[5])) for r in rows])
    return 0, data, lines


def cmd_status(ctx):
    p = pipeline()
    registry, environ = ctx.registry, ctx.environ
    secrets = secret_names(environ)
    store = p.open_store(ctx.args.store)
    month = p.month_of()
    cells = []
    for device in registry['devices']:
        if device['status'] != 'active':
            why = device['status']
        else:
            missing = p.missing_accounts(device, registry, environ, secrets)
            why = 'runnable' if not missing else 'waiting: account ' + ', '.join(missing)
        image = None
        if device['executor'] == 'gce_vm':
            baked = store.get_json('images/%s.json' % device['executor_config']['image_family'])
            image = baked['image'] if baked else None
        cells.append({'device': device['id'], 'tier': device['tier'], 'executor': device['executor'],
                      'cost_class': device['cost_class'], 'state': why, 'image': image,
                      'needs_bake': device['executor'] == 'gce_vm' and image is None})
    running = [r for r in (store.get_json(n) for n in store.list('machines/')) if r and not r.get('ended')]
    problems = p.validate_registry(registry, p.ci_tier.load_tiers(p.TIERS), rows=set(p.roadmap_ids()))
    data = {'cells': cells, 'machines_running': running, 'month': month, 'spent_usd': p.month_spend(store, month),
            'cap_usd': registry['budget']['monthly_cap_usd'], 'gcp_stop': p.gcp_budget_stop(store, month),
            'registry_problems': problems}
    lines = ['Registry: %d cells (%s)' % (len(cells), ', '.join('%s %d' % (s, sum(
        1 for d in registry['devices'] if d['status'] == s)) for s in p.STATUSES))]
    lines += ctx.out.table(['DEVICE', 'TIER', 'EXECUTOR', 'COST', 'STATE', 'IMAGE'], [
        (c['device'], c['tier'], c['executor'], c['cost_class'], ctx.out.state(c['state']),
         c['image'] or (ctx.out.paint('NOT BAKED: ops.py device bake %s' % c['device'], 'warn') if c['needs_bake']
                        else '')) for c in cells])
    lines.append('Machines running: %d' % len(running))
    lines += ['  %s on %s since %s' % (r['job'], r['device'], r['started']) for r in running]
    lines.append('Spend %s: %.2f of %.2f USD%s' % (month, data['spent_usd'], data['cap_usd'],
                                                   '; GCP budget stop in force' if data['gcp_stop'] else ''))
    lines += [ctx.out.paint('REGISTRY: ' + problem, 'bad') for problem in problems]
    return (1 if problems else 0), data, lines


def complete_run_estimate(p, registry, store):
    defaults = registry['budget']['default_minutes']
    tiers = p.ci_tier.load_tiers(p.TIERS)
    hosts = p.job_hosts(registry)
    devices = p.devices_by_id(registry)
    items = []
    for preset, entry in tiers['presets'].items():
        if entry['tier'] == 'diagnostic':
            continue
        device = devices[hosts[entry['job']]]
        items.append(('lane-%s' % preset, device, p.estimate_minutes(store, 'lane-%s' % preset, defaults['lane'])))
    for device in registry['devices']:
        if device['status'] != 'active':
            continue
        for preset in device.get('lane_presets', []):
            key = 'lane-%s@%s' % (preset, device['id'])
            items.append((key, device, p.estimate_minutes(store, key, defaults['lane'])))
        if device.get('console_profile'):
            key = 'console-%s' % device['id']
            items.append((key, device, p.estimate_minutes(store, key, defaults['console-profile'])))
    return [{'step': k, 'device': d['id'], 'minutes': m, 'usd': p.step_cost(d, m)} for k, d, m in items]


def cmd_cost(ctx):
    """The cap, the month's spend and forecast, the breakdown by build, lane and device, and a complete run."""
    p = pipeline()
    from coordinator import reports
    store = p.open_store(ctx.args.store)
    if getattr(ctx.args, 'pull', False):
        subscription = ctx.environ.get('CERID_CI_BUDGET_SUBSCRIPTION')
        if not subscription:
            raise OpsError('CERID_CI_BUDGET_SUBSCRIPTION is not set',
                           'set it as in the operator runbook, "the budget stop", then run this again')
        p.pull_budget(store, subscription)
    c = orchestrator(ctx)
    month = ctx.args.month or p.month_of()
    data = reports.costs(c, ctx.registry, month)
    data['complete_run'] = complete_run_estimate(p, ctx.registry, store)
    total = round(sum(i['usd'] for i in data['complete_run']), 2)
    data['complete_run_usd'] = total
    by = getattr(ctx.args, 'by', 'lane')
    ratio = data['spent_usd'] / data['cap_usd'] if data['cap_usd'] else 0.0
    style = 'bad' if ratio >= 1 or data['forecast_over_cap'] else ('warn' if ratio >= 0.8 else 'ok')
    lines = [ctx.out.paint('%s: %.2f of %.2f USD spent (%d %%), forecast %.2f USD%s' % (
        month, data['spent_usd'], data['cap_usd'], ratio * 100, data['forecast_usd'],
        ', OVER THE CAP' if data['forecast_over_cap'] else ''), style),
        'Running now: %d machines, worst case %.2f USD; GCP budget stop: %s' % (
            data['running'], data['running_bound_usd'], 'IN FORCE' if data['gcp_stop'] else 'no')]
    rows = data['by_%s' % by]
    lines += ['', 'By %s:' % by] + ctx.out.table([by.upper(), 'MACHINES', 'HOURS', 'USD'], [
        (r[{'build': 'build', 'lane': 'key', 'device': 'device'}[by]], r['machines'], '%.2f' % r['hours'],
         '%.2f' % r['usd']) for r in rows[:30]])
    lines += ['', 'One complete run, estimated before the cache: %.2f USD (runs left this month: %d)' % (
        total, int(max(data['cap_usd'] - data['spent_usd'], 0.0) // total) if total else 0)]
    return 0, data, lines


def cmd_parity(ctx):
    p = pipeline()
    rows, problems = p.parity_rows()
    data = {'workflow': [list(r) for r in rows], 'experiments': [list(r) for r in p.experiment_rows()],
            'problems': problems}
    lines = p.parity_markdown(rows).splitlines() + ['', 'Dispatch-only experiments (host-check.yml):', ''] + \
        p.parity_markdown(p.experiment_rows()).splitlines()
    for problem in problems:
        print('PARITY: ' + problem, file=sys.stderr)
    return (1 if problems else 0), data, lines


def cmd_device_add(ctx):
    p = pipeline()
    entry = device_entry(ctx.args)
    text = p.DEVICES.read_text(encoding='utf-8')
    updated = insert_device(text, entry)
    problems = p.validate_registry(json.loads(updated), p.ci_tier.load_tiers(p.TIERS), rows=set(p.roadmap_ids()))
    if problems:
        return 1, {'added': False, 'problems': problems}, [ctx.out.paint('REFUSED: ' + x, 'bad') for x in problems]
    if not ctx.args.dry_run:
        p.DEVICES.write_text(updated, encoding='utf-8', newline='\n')
    lines = ['%s %s (%s, tier %s)' % ('Would add' if ctx.args.dry_run else 'Added', entry['id'], entry['executor'],
                                      entry['tier'])]
    data = {'added': not ctx.args.dry_run, 'device': entry}
    if entry['executor'] == 'owned_agent':
        from executors import owned_agent
        agent = owned_agent.OwnedAgent(ctx.environ)
        data['agent'] = {'command': agent.command(entry), 'install': agent.install(entry)}
        lines += ['', 'On the machine, the agent:', agent.config(entry)] + ['  - ' + s for s in agent.install(entry)]
    return 0, data, lines


def cmd_device_list(ctx):
    devices = ctx.registry['devices']
    data = [{k: d.get(k) for k in ('id', 'status', 'kind', 'tier', 'executor', 'cost_class')} for d in devices]
    return 0, data, ctx.out.table(['DEVICE', 'STATUS', 'KIND', 'TIER', 'EXECUTOR', 'COST'], [
        (d['id'], d['status'], d['kind'], d['tier'], d['executor'], d['cost_class']) for d in data])


def cmd_device_bake(ctx):
    p = pipeline()
    from executors import gce_vm
    devices = p.devices_by_id(ctx.registry)
    if ctx.args.device not in devices:
        raise OpsError('no device %s' % ctx.args.device, 'list the cells with: python ci/ops.py device list')
    device = devices[ctx.args.device]
    if device['executor'] != 'gce_vm':
        raise OpsError('%s is a %s cell; only gce_vm cells have images' % (device['id'], device['executor']))
    executor = gce_vm.GceVm(ctx.environ)
    packages = list(p.APT_PACKAGES['arm64' if device['cpu']['arch'] == 'aarch64' else 'x64'])
    provision = gce_vm.provision_script(device, packages)
    if ctx.args.dry_run:
        zone, body = executor.bake_body(device, provision)
        return 0, {'zone': zone, 'instance': body}, json.dumps({'zone': zone, 'instance': body}, indent=1).splitlines()
    stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%d%H%M')
    try:
        image = executor.bake(device, provision, stamp, log=lambda line: print(line, file=sys.stderr),
                              sleep=time.sleep, clock=time.time)
    except gce_vm.BakeError as error:
        raise OpsError(str(error), 'look at its serial console in the Compute Engine page, then delete it')
    p.open_store(ctx.args.store).put_json('images/%s.json' % image['family'], {
        'image': image['name'], 'family': image['family'], 'baked': stamp, 'device': device['id']})
    return 0, {'image': image['name'], 'family': image['family']}, ['image %s in family %s' % (
        image['name'], image['family'])]


def cmd_build_list(ctx):
    builds = orchestrator(ctx).recent(ctx.args.limit)
    return 0, builds, ctx.out.table(['BUILD', 'STATE', 'REPOSITORY', 'BRANCH', 'COMMIT', 'MESSAGE'], [
        ('#%d' % b['number'], ctx.out.state(b['state']), b['repository'], (b['branch'] or '')[:24],
         (b['commit'] or '')[:12], (b['message'] or '')[:60]) for b in builds]) or ['No builds yet.']


def cmd_build_show(ctx):
    view = orchestrator(ctx).build_view(ctx.args.number)
    lines = ['Build #%d %s  %s %s %s' % (view['number'], ctx.out.state(view['state']), view['repository'],
                                        view['branch'], view['commit'])]
    lines += ctx.out.table(['STEP', 'STATE', 'ATTEMPT', 'REASON'], [
        (j['key'][:48], ctx.out.state(j['state'] + (' (soft)' if j['soft_failed'] else '')), j['attempt'],
         (j.get('waiting') or j['reason'] or '')[:70]) for j in view['jobs']])
    return (0 if view['state'] in ('passed', 'running', 'scheduled') else 1), view, lines


def cmd_build_start(ctx):
    from coordinator import hosts
    c = orchestrator(ctx)
    repo = c.repository(ctx.args.repository)
    commit = ctx.args.commit or hosts.ls_remote(repo['url'], [ctx.args.branch])[ctx.args.branch]
    env = {}
    if ctx.args.tier:
        env['CERID_CI_TIER'] = ctx.args.tier
    if ctx.args.fresh:
        env['CERID_CI_FRESH'] = '1'
    build, _ = c.create_build(ctx.args.repository, {'event': 'push', 'branch': ctx.args.branch, 'commit': commit,
                                                    'message': ctx.args.message or 'manual build (ci/ops.py)',
                                                    'trusted': True}, 'manual', env=env, created_by=who(ctx.environ))
    return 0, {'build': build['number'], 'commit': commit}, ['build #%d scheduled for %s at %s' % (
        build['number'], ctx.args.branch, commit)]


def cmd_build_cancel(ctx):
    orchestrator(ctx).cancel_build(ctx.args.number, by=who(ctx.environ))
    return 0, {'canceled': ctx.args.number}, ['cancel requested for build #%d' % ctx.args.number]


def cmd_job_show(ctx):
    job = orchestrator(ctx).job(ctx.args.job)
    job.pop('token_hash', None)
    lines = ['%s %s attempt %d  %s' % (job['key'], ctx.out.state(job['state']), job['attempt'], job['reason'] or '')]
    if job.get('repro'):
        lines += ['Reproduce:', '  git checkout %s' % job['repro'].get('commit'), '  %s' % job['command']]
        lines += ['  %s: %s' % (k, v) for k, v in sorted((job['repro'].get('tools') or {}).items())]
    return 0, job, lines


def cmd_job_retry(ctx):
    job = orchestrator(ctx).retry_job(ctx.args.job, by=who(ctx.environ))
    return 0, {'job': job['id'], 'state': job['state'], 'attempt': job['attempt']}, [
        'job %s (%s) is %s, attempt %d' % (job['id'], job['key'], job['state'], job['attempt'])]


def cmd_job_cancel(ctx):
    c = orchestrator(ctx)
    job = c.cancel_job(ctx.args.job, reason='canceled by %s' % who(ctx.environ))
    c.audit(who(ctx.environ), 'job-cancel', 'build %d %s' % (job['build'], job['key']), '')
    return 0, {'job': job['id'], 'state': job['state']}, ['cancel requested for %s' % job['key']]


def cmd_job_logs(ctx):
    c = orchestrator(ctx)
    start = 0
    while True:
        data, start, live, job = c.read_log(ctx.args.job, ctx.args.attempt, start)
        if ctx.args.json:
            return 0, {'text': data.decode('utf-8', 'replace'), 'next': start, 'live': live}, []
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
        if not (ctx.args.follow and live):
            return 0, None, []
        time.sleep(2)


def cmd_job_reproduce(ctx, call=None):
    """Download a failed job's diagnostics (ci/coordinator/diagnostics.py) and its reproducibility record, and print
    the exact local replay: the commit in its own worktree, the lane as CI ran it (the failed tests only), the failed
    tests at the job's CPU level, and each diagnostic's reader. --run runs the worktree and lane steps."""
    from coordinator import diagnostics, scheduler
    call = call or subprocess.call
    c = orchestrator(ctx)
    job = c.job(ctx.args.job)
    entries = diagnostics.diagnostic_entries(c.artifacts(job['build']), job['id'], ctx.args.attempt)
    name = '%d-%s' % (job['build'], ''.join(ch if ch.isalnum() or ch in '-_.' else '_' for ch in job['key'])[:60])
    dest = Path(ctx.args.dest) if ctx.args.dest else ROOT / 'build' / 'ci-repro' / name
    dest.mkdir(parents=True, exist_ok=True)
    files = []
    summary = None
    missing = []
    for entry in entries:
        data = c.store.get(scheduler.PREFIX + entry['object'])
        if data is None:
            missing.append(entry['path'])
            continue
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise OpsError('artifact %s does not match its recorded SHA-256' % entry['path'],
                           'the store copy is damaged; run the job again')
        target = dest / entry['kind'] / Path(entry['path']).name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        if entry['kind'] == 'summary':
            summary = json.loads(data.decode('utf-8'))
        else:
            files.append((target, entry['kind']))
    record = {'job': job['id'], 'build': job['build'], 'key': job['key'], 'attempt': job['attempt'],
              'state': job['state'], 'command': job['command'], 'env': job.get('env', {}), 'repro': job.get('repro')}
    (dest / 'repro.json').write_text(json.dumps(record, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    source = dest / 'src'
    steps = diagnostics.reproduce_steps(job, summary, [(f, k) for f, k in files if k != 'junit'], os.name == 'nt',
                                        checkout=str(source))
    withheld = [i for i in (summary or {}).get('items', []) if i.get('withheld')]
    lines = ['Job %s (%s), build #%d, %s: %d diagnostics in %s' % (job['id'], job['key'], job['build'],
                                                                   ctx.out.state(job['state']), len(files), dest)]
    if not job.get('repro'):
        lines.append('No reproducibility record: the agent did not report one for this attempt.')
    if summary is None:
        lines.append('No diagnostics summary: the job did not fail in a lane, or it ran before diagnostics existed.')
    else:
        lines.append('Failed phase %s at the CPU level %s; failed tests: %s' % (
            summary.get('phase'), summary.get('cpu_level'), ', '.join(summary.get('failed_tests') or []) or 'none'))
    for path in missing:
        lines.append('removed by retention: %s' % path)
    for item in withheld:
        lines.append('withheld on the machine: %s %s (%s)' % (item['kind'], item['source'], item['withheld']))
    for number, (title, command, note) in enumerate(steps, 1):
        lines.append('%d. %s' % (number, title))
        if command:
            lines.append('   %s' % command)
        if note:
            lines.append('   (%s)' % note)
    data = {'job': job['id'], 'dest': str(dest), 'summary': summary, 'files': [
        {'path': str(f), 'kind': k} for f, k in files], 'missing': missing,
        'steps': [{'title': t, 'command': cmd, 'note': n} for t, cmd, n in steps]}
    if not ctx.args.run:
        return 0, data, lines
    commit = (job.get('repro') or {}).get('commit') or (summary or {}).get('commit')
    preset = (summary or {}).get('preset')
    device = (summary or {}).get('device') or job.get('env', {}).get('CERID_CI_DEVICE')
    if not (commit and preset and device):
        raise OpsError('this job has no commit, preset and device to run', 'run the printed commands by hand')
    if not source.exists():
        code = call(['git', '-C', str(ROOT), 'worktree', 'add', '--detach', str(source), commit])
        if code:
            raise OpsError('git worktree add failed (%d)' % code, 'fetch the commit: git fetch origin %s' % commit)
    argv = [sys.executable, 'ci/pipeline.py', 'lane', '--preset', preset, '--device', device]
    pattern = diagnostics.regex_for((summary or {}).get('failed_tests') or [])
    if pattern:
        argv += ['--tests', pattern]
    code = call(argv, cwd=str(source))
    data['exit'] = code
    return code, data, lines + ['the lane exited %d' % code]


def cmd_agent_list(ctx):
    agents = orchestrator(ctx).agents()
    return 0, agents, ctx.out.table(['NAME', 'STATUS', 'QUEUE', 'OS', 'ARCH', 'TAGS', 'LAST SEEN'], [
        (a['name'][:24], ctx.out.state(a['status']), a['queue'], a['os'], a['arch'],
         ','.join('%s=%s' % kv for kv in sorted(a['tags'].items())), when(a.get('last_seen'))) for a in agents]) or [
        'No agents registered yet: python ci/ops.py token create --name <machine> --queue cerid-owned --out <file>']


def cmd_agent_stop(ctx):
    agent = orchestrator(ctx).stop_agent(ctx.args.agent, by=who(ctx.environ))
    return 0, {'stopped': agent['id']}, ['agent %s stopped; it takes no new job' % agent['name']]


def cmd_queue_list(ctx):
    queues = orchestrator(ctx).queues()
    return 0, queues, ctx.out.table(['QUEUE', 'AGENTS', 'SCHEDULED', 'ASSIGNED', 'RUNNING', 'OLDEST WAIT'], [
        (q['queue'], q['agents'], q['scheduled'], q['assigned'], q['running'], '%ds' % q['oldest_wait_s'])
        for q in queues]) or ['No active work.']


def cmd_token_create(ctx):
    if not ctx.args.out:
        raise OpsError('token create needs --out FILE', 'the token is written to that file, never printed')
    out = Path(ctx.args.out)
    if out.exists():
        raise OpsError('%s exists' % out, 'choose a new file; an existing token file is never overwritten')
    tags = dict(part.split('=', 1) for part in (ctx.args.tags or '').split(',') if '=' in part)
    token = orchestrator(ctx).create_registration(ctx.args.name, ctx.args.queue, tags, by=who(ctx.environ))
    descriptor = os.open(str(out), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
        handle.write(token + '\n')
    return 0, {'name': ctx.args.name, 'queue': ctx.args.queue, 'file': str(out)}, [
        'registration token for %s (queue %s) written to %s; move it to the machine, readable only by the agent '
        'user' % (ctx.args.name, ctx.args.queue, out)]


def cmd_token_revoke(ctx):
    count = orchestrator(ctx).revoke_registration(ctx.args.name, by=who(ctx.environ))
    return 0, {'revoked': count}, ['%d registration tokens revoked for %s' % (count, ctx.args.name)]


def health_report(c, now=None):
    """(ok, lines): the state store answers, the coordinator ticks, no job is stuck, no agent is lost silently."""
    now = c.clock() if now is None else now
    lines = []
    ok = True
    try:
        schema = c.get('schema.json')
        lines.append('state store: reachable, schema %s' % (schema or {}).get('schema', 'not initialised'))
    except Exception as error:
        return False, ['state store: UNREACHABLE (%s)' % error]
    beat = c.get('coordinator/heartbeat.json')
    age = now - beat['at'] if beat else None
    if age is None or age > 60:
        ok = False
        lines.append('coordinator: NOT TICKING (last tick %s)' % ('never' if age is None else '%d s ago' % age))
    else:
        lines.append('coordinator: ticking (%d s ago, version %s)' % (age, beat.get('version')))
    stuck = []
    for number in c.active_builds():
        build = c.get(c.build_name(number))
        for job in c.jobs_of(build) if build else []:
            if job['state'] == 'running':
                heartbeat = c.store.get_json('orchestrator/heartbeats/%s.json' % job['id']) or {}
                if now - heartbeat.get('at', job.get('started_at') or now) > 2 * c.limits['lost_after_seconds']:
                    stuck.append('#%d %s' % (number, job['key']))
    if stuck:
        ok = False
        lines.append('jobs: STUCK past the watchdog: %s' % ', '.join(stuck))
    else:
        lines.append('jobs: %d active builds, none stuck' % len(c.active_builds()))
    agents = c.agents()
    lost = [a['name'] for a in agents if a['status'] == 'lost']
    lines.append('agents: %d (%d lost%s)' % (len(agents), len(lost), ': ' + ', '.join(lost) if lost else ''))
    machines = [c.store.get_json(n) for n in c.store.list('machines/')]
    lines.append('machines running: %d' % len([m for m in machines if m and not m.get('ended')]))
    return ok, lines


def cmd_health(ctx):
    ok, lines = health_report(orchestrator(ctx))
    painted = [ctx.out.paint(line, 'bad' if any(w in line for w in ('NOT', 'STUCK', 'UNREACHABLE')) else 'ok')
               for line in lines]
    if not ok:
        painted.append('next: docs/recipes/ci-platform-runbooks.md, "the coordinator is not ticking" and "a stuck '
                       'build"')
    return (0 if ok else 1), {'ok': ok, 'checks': lines}, painted


def cmd_test_list(ctx):
    from coordinator import quality
    c = orchestrator(ctx)
    lanes = [ctx.args.lane] if ctx.args.lane else quality.lanes(c.store)
    data = {lane: quality.summary(c.store, lane) for lane in lanes}
    lines = []
    for lane, tests in data.items():
        lines += ['', ctx.out.paint(lane, 'bold')] + ctx.out.table(['TEST', 'RUNS', 'PASS', 'FAIL', 'LAST', 'MEDIAN',
                                                                     'FLAKY'], [
            (t['test'][:60], t['runs'], t['pass'], t['fail'], ctx.out.state(t['last']), '%.2fs' % t['median_s'],
             ctx.out.paint('flaky', 'warn') if t['flaky'] else '') for t in tests[:ctx.args.limit]])
    return 0, data, lines or ['No test runs recorded yet.']


def cmd_test_history(ctx):
    from coordinator import quality
    history = quality.history(orchestrator(ctx).store, ctx.args.lane, ctx.args.test)
    return 0, history, ctx.out.table(['BUILD', 'COMMIT', 'OUTCOME', 'SECONDS', 'KEY'], [
        (h['build'], (h.get('commit') or '')[:12], ctx.out.state(h['outcome']), h.get('seconds'), (h.get('key') or
                                                                                                  '')[:12])
        for h in history]) or ['No runs of %s on %s.' % (ctx.args.test, ctx.args.lane)]


def cmd_test_flaky(ctx):
    from coordinator import quality
    c = orchestrator(ctx)
    data = {lane: quality.flaky(c.store, lane) for lane in quality.lanes(c.store)}
    rows = [(lane, test, ', '.join(e['passed_in']), ', '.join(e['failed_in'])) for lane, found in data.items()
            for test, e in found.items()]
    return 0, data, ctx.out.table(['LANE', 'TEST', 'PASSED IN', 'FAILED IN'], rows) or [
        'No flaky test: no test key both passed and failed.']


def cmd_quarantine_list(ctx):
    c = orchestrator(ctx)
    records = [r for r in (c.store.get_json(n) for n in c.store.list('orchestrator/quarantine/')) if r]
    now = c.clock()
    return 0, records, ctx.out.table(['LANE', 'TEST', 'STATE', 'UNTIL', 'ISSUE'], [
        (r['lane'], r['test'][:50], ctx.out.state('active' if r.get('active') and r['expires'] > now else 'ended'),
         r['expires_text'], r['issue']) for r in records]) or ['Nothing is quarantined.']


def cmd_quarantine_release(ctx):
    from coordinator import quality
    record = quality.release(orchestrator(ctx), ctx.args.lane, ctx.args.test, who(ctx.environ))
    return 0, record, ['released %s on %s; its failures fail the lane again' % (ctx.args.test, ctx.args.lane)]


def cmd_bisect_list(ctx):
    c = orchestrator(ctx)
    records = [r for r in (c.store.get_json(n) for n in c.store.list('orchestrator/bisect/')) if r]
    return 0, records, ctx.out.table(['LANE', 'TEST', 'STATE', 'GOOD', 'BAD', 'FIRST BAD', 'RE-RUNS'], [
        (r['lane'], ', '.join(r['tests'])[:40], r['state'], (r['good']['commit'] or '')[:10], (r['bad']['commit'] or '')[:10],
         (r.get('first_bad') or '')[:12], len(r['reruns'])) for r in records]) or ['No bisection.']


def cmd_audit_list(ctx):
    from coordinator import reports
    entries = reports.audit_entries(orchestrator(ctx), days=ctx.args.days)
    return 0, entries, ctx.out.table(['WHEN', 'WHO', 'ACTION', 'TARGET', 'DETAIL'], [
        (when(e['at']), e['who'][:30], e['action'], e['target'][:40], e['detail'][:50]) for e in entries]) or [
        'No audited action in the last %d days.' % ctx.args.days]


def cmd_slo(ctx):
    from coordinator import reports
    c = orchestrator(ctx)
    data = reports.slo(c, config=c.config.get('slo'))
    lines = ['Window: %d days' % data['window_days']]
    lines += ctx.out.table(['QUEUE', 'SAMPLES', 'P50', 'P95', 'TARGET', 'WITHIN', 'OBJECTIVE'], [
        (q['queue'], q['samples'], '%ss' % q['p50_s'], '%ss' % q['p95_s'], '%ss' % q['target_s'],
         '%.1f %%' % (q['within'] * 100), ctx.out.state('met' if q['met'] else 'missed')) for q in data['queues']])
    if data['availability'] is None:
        lines.append('Coordinator availability: no tick recorded yet')
    else:
        lines.append('Coordinator availability: %.3f %% (objective %.1f %%) %s' % (
            data['availability'] * 100, data['availability_objective'] * 100,
            ctx.out.state('met' if data['availability_met'] else 'missed')))
    ok = data['availability_met'] and all(q['met'] for q in data['queues'])
    return (0 if ok else 1), data, lines


def cmd_notify_test(ctx):
    c = orchestrator(ctx)
    if not c.config.get('notifications', {}).get('channels'):
        raise OpsError('no notification channel is configured',
                       'add notifications.channels to ci/coordinator.json (email or ntfy), see the runbook')
    sent = c.notify('failure', 'Test notification', 'A test from python ci/ops.py notify test by %s.' %
                    who(ctx.environ), key='test:%f' % time.time())
    return (0 if sent else 1), {'sent': sent}, ['sent through: %s' % (', '.join(sent) or 'nothing (see the log)')]


BACKUP_SKIP = ('blobs/', 'orchestrator/logs/', 'orchestrator/artifacts/', 'orchestrator/heartbeats/')


def backup(store, out, everything=False):
    """A tar.gz of the store's objects with a SHA-256 manifest; caches, logs and artifacts only with `everything`."""
    import io
    import tarfile
    names = [n for n in store.list('') if everything or not n.startswith(BACKUP_SKIP)]
    manifest = {}
    with tarfile.open(out, 'w:gz') as archive:
        for name in names:
            data = store.get(name)
            if data is None:
                continue
            manifest[name] = hashlib.sha256(data).hexdigest()
            info = tarfile.TarInfo('objects/' + name)
            info.size = len(data)
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))
        body = json.dumps({'schema': 'cerid-ci-backup/1', 'objects': manifest}, indent=1, sort_keys=True).encode()
        info = tarfile.TarInfo('manifest.json')
        info.size = len(body)
        archive.addfile(info, io.BytesIO(body))
    return manifest


def restore(store, source, overwrite=False):
    """Write a backup's objects back; every object is checked against the manifest first; an existing object is
    kept unless `overwrite`. Returns (written, kept)."""
    import tarfile
    with tarfile.open(source, 'r:gz') as archive:
        manifest = json.loads(archive.extractfile('manifest.json').read().decode('utf-8'))['objects']
        objects = {}
        for member in archive.getmembers():
            if member.name.startswith('objects/') and member.isfile():
                objects[member.name[len('objects/'):]] = archive.extractfile(member).read()
    for name, digest in manifest.items():
        if hashlib.sha256(objects.get(name, b'')).hexdigest() != digest:
            raise ValueError('backup object %s does not match its manifest' % name)
    written = kept = 0
    for name in sorted(manifest):
        if overwrite:
            store.put(name, objects[name])
            written += 1
        elif store.write_if(name, objects[name], 0) is not None:
            written += 1
        else:
            kept += 1
    return written, kept


def cmd_backup_create(ctx):
    manifest = backup(pipeline().open_store(ctx.args.store), ctx.args.out, ctx.args.all)
    try:
        orchestrator(ctx).audit(who(ctx.environ), 'backup', ctx.args.out, '%d objects' % len(manifest))
    except Exception:  # an audit record never fails a backup
        pass
    return 0, {'objects': len(manifest), 'file': ctx.args.out}, ['%d objects backed up to %s' % (
        len(manifest), ctx.args.out)]


def cmd_backup_restore(ctx):
    written, kept = restore(pipeline().open_store(ctx.args.store), ctx.args.source, ctx.args.overwrite)
    try:
        orchestrator(ctx).audit(who(ctx.environ), 'restore', ctx.args.source, '%d written, %d kept' % (written, kept))
    except Exception:
        pass
    return 0, {'written': written, 'kept': kept}, ['%d objects restored, %d existing objects kept%s' % (
        written, kept, '' if ctx.args.overwrite or not kept else ' (--overwrite replaces them)')]


def cmd_retention_run(ctx):
    c = orchestrator(ctx)
    cursor = c.sweep(limit=ctx.args.limit)
    return 0, {'next_build': cursor, 'logs_days': c.limits['logs_days'],
               'artifacts_days': c.limits['artifacts_days']}, [
        'retention: logs %d days, artifacts %d days; swept up to build #%d' % (
            c.limits['logs_days'], c.limits['artifacts_days'], cursor - 1)]


# --------------------------------------------------------------------------------------------- ui (IAP tunnel)

def gcloud_path(which=shutil.which):
    for name in ('gcloud', 'gcloud.cmd'):
        found = which(name)
        if found:
            return found
    raise OpsError('gcloud is not installed or not on PATH',
                   'install the Google Cloud CLI, run `gcloud auth login`, then run this again')


def free_port(preferred):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        try:
            probe.bind(('127.0.0.1', preferred))
            return preferred
        except OSError:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]


def tunnel_ready(port, opener=urllib.request.urlopen):
    try:
        with opener('http://localhost:%d/healthz' % port, timeout=3) as response:
            return json.loads(response.read().decode('utf-8')).get('ok') is True
    except (OSError, ValueError):
        return False


def cmd_ui(ctx, popen=subprocess.Popen, ready=tunnel_ready, browser=None, sleep=time.sleep, clock=time.time):
    """Open the web UI: an IAP TCP tunnel from this computer to the launcher's port 8780 under your own gcloud
    login (only principals with the IAP-secured Tunnel User role on the launcher get through), then the browser."""
    args = ctx.args
    project = args.project or ctx.environ.get('CERID_CI_GCP_PROJECT') or 'cerid-ci'
    zone = args.zone or ctx.environ.get('CERID_CI_GCP_ZONE') or 'us-central1-a'
    port = free_port(args.port)
    command = [gcloud_path(), 'compute', 'start-iap-tunnel', args.instance, '8780',
               '--local-host-port=localhost:%d' % port, '--zone', zone, '--project', project]
    tunnel = popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    url = 'http://localhost:%d/' % port
    deadline = clock() + args.timeout
    try:
        while not ready(port):
            if tunnel.poll() is not None:
                output = tunnel.stdout.read() if tunnel.stdout else ''
                raise OpsError('the IAP tunnel stopped: %s' % ' '.join(output.split())[-400:],
                               'check that you are signed in (`gcloud auth list`), that your account has the '
                               '"IAP-secured Tunnel User" role on %s, and that the firewall rule cerid-ci-iap allows '
                               '35.235.240.0/20 to TCP 8780 (operator runbook, step "the UI tunnel")' % args.instance)
            if clock() > deadline:
                raise OpsError('the tunnel to %s did not answer in %d s' % (args.instance, args.timeout),
                               'is the coordinator running? `gcloud compute ssh %s --tunnel-through-iap --command '
                               '"systemctl status cerid-ci-coordinator"`' % args.instance)
            sleep(0.5)
        if not args.no_browser:
            (browser or __import__('webbrowser').open)(url)
        print('Cerid CI is open at %s (IAP tunnel to %s in %s). Press Ctrl+C to close it.' % (url, args.instance,
                                                                                              project), flush=True)
        if args.once:
            return 0, {'url': url, 'port': port}, []
        tunnel.wait()
        return 0, {'url': url}, []
    except KeyboardInterrupt:
        return 0, {'url': url}, ['tunnel closed']
    finally:
        if tunnel.poll() is None:
            tunnel.terminate()


# --------------------------------------------------------------------------------------------- completion

def command_tree(parser):
    """{command path: [options]} from the argparse parser, for the completion scripts."""
    tree = {}

    def walk(node, path):
        actions = [a for a in node._actions if a.option_strings]
        tree[' '.join(path)] = sorted(o for a in actions for o in a.option_strings if o.startswith('--'))
        for action in node._actions:
            if isinstance(action, argparse._SubParsersAction):
                for name, child in action.choices.items():
                    if name in LEGACY:
                        continue
                    walk(child, path + [name])
    walk(parser, [])
    return tree


def completion(shell, parser):
    tree = command_tree(parser)
    children = {}
    for path in tree:
        if path:
            head, _, last = path.rpartition(' ')
            children.setdefault(head, []).append(last)
    if shell in ('bash', 'zsh'):
        cases = []
        for path in sorted(tree):
            words = sorted(children.get(path, [])) + tree[path]
            cases.append('    "%s") words="%s" ;;' % (path, ' '.join(words)))
        script = '''# Cerid CI ops.py completion (%s). Install: python ci/ops.py completion %s > ~/.cerid-ops-completion
# and source it from your shell's rc file.
_cerid_ops() {
  local cur path w i
  cur="${COMP_WORDS[COMP_CWORD]}"
  path=""
  for ((i=1; i<COMP_CWORD; i++)); do
    w="${COMP_WORDS[i]}"
    case "$w" in -*) continue ;; esac
    if [ -z "$path" ]; then candidate="$w"; else candidate="$path $w"; fi
    case "$candidate" in
%s
      *) continue ;;
    esac
    path="$candidate"
  done
  case "$path" in
%s
    *) words="" ;;
  esac
  COMPREPLY=( $(compgen -W "$words" -- "$cur") )
}
''' % (shell, shell, '\n'.join('      "%s") ;;' % p for p in sorted(tree) if p), '\n'.join(cases))
        if shell == 'zsh':
            script += 'autoload -U +X bashcompinit && bashcompinit\n'
        return script + 'complete -F _cerid_ops ops.py\ncomplete -F _cerid_ops "python ci/ops.py"\n'
    if shell == 'powershell':
        entries = ',\n'.join("    '%s' = @(%s)" % (path, ', '.join("'%s'" % w for w in sorted(children.get(
            path, [])) + tree[path])) for path in sorted(tree))
        return '''# Cerid CI ops.py completion (PowerShell). Install: python ci/ops.py completion powershell >> $PROFILE
$CeridOpsTree = @{
%s
}
Register-ArgumentCompleter -Native -CommandName python, py -ScriptBlock {
  param($wordToComplete, $commandAst, $cursorPosition)
  $words = @($commandAst.CommandElements | ForEach-Object { $_.ToString() })
  $index = [Array]::FindIndex($words, [Predicate[string]]{ param($w) $w -like '*ops.py' })
  if ($index -lt 0) { return }
  $path = ''
  foreach ($w in $words[($index + 1)..($words.Count - 1)]) {
    if ($w -eq $wordToComplete -or $w.StartsWith('-')) { continue }
    $candidate = if ($path) { "$path $w" } else { $w }
    if ($CeridOpsTree.ContainsKey($candidate)) { $path = $candidate }
  }
  $CeridOpsTree[$path] | Where-Object { $_ -like "$wordToComplete*" } |
    ForEach-Object { [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_) }
}
''' % entries
    raise OpsError('unknown shell %s' % shell, 'choose bash, zsh or powershell')


# --------------------------------------------------------------------------------------------- parser

LEGACY = {'add-device': ('device', 'add'), 'bake': ('device', 'bake'), 'budget': ('cost', None),
          'builds': ('build', 'list'), 'agents': ('agent', 'list'), 'queues': ('queue', 'list'),
          'logs': ('job', 'logs'), 'retry': ('job', 'retry'), 'agent-token': ('token', None),
          'restore': ('backup', 'restore')}

HANDLERS = {
    ('ui', None): cmd_ui, ('status', None): cmd_status, ('health', None): cmd_health,
    ('accounts', None): cmd_accounts, ('parity', None): cmd_parity, ('cost', None): cmd_cost, ('slo', None): cmd_slo,
    ('device', 'add'): cmd_device_add, ('device', 'bake'): cmd_device_bake, ('device', 'list'): cmd_device_list,
    ('build', 'list'): cmd_build_list, ('build', 'show'): cmd_build_show, ('build', 'start'): cmd_build_start,
    ('build', 'cancel'): cmd_build_cancel, ('job', 'show'): cmd_job_show, ('job', 'logs'): cmd_job_logs,
    ('job', 'retry'): cmd_job_retry, ('job', 'cancel'): cmd_job_cancel, ('job', 'reproduce'): cmd_job_reproduce,
    ('agent', 'list'): cmd_agent_list,
    ('agent', 'stop'): cmd_agent_stop, ('queue', 'list'): cmd_queue_list, ('token', 'create'): cmd_token_create,
    ('token', 'revoke'): cmd_token_revoke, ('test', 'list'): cmd_test_list, ('test', 'history'): cmd_test_history,
    ('test', 'flaky'): cmd_test_flaky, ('quarantine', 'list'): cmd_quarantine_list,
    ('quarantine', 'release'): cmd_quarantine_release, ('bisect', 'list'): cmd_bisect_list,
    ('audit', 'list'): cmd_audit_list, ('notify', 'test'): cmd_notify_test, ('backup', 'create'): cmd_backup_create,
    ('backup', 'restore'): cmd_backup_restore, ('retention', 'run'): cmd_retention_run,
}


def add_device_options(parser):
    for flag in ('--id', '--kind', '--os', '--os-version', '--arch', '--cpu-model', '--levels', '--gpu-vendor',
                 '--gpu-model', '--executor', '--tier', '--cost-class', '--features', '--route'):
        parser.add_argument(flag, required=True)
    parser.add_argument('--status', default='active')
    parser.add_argument('--apis')
    parser.add_argument('--accounts')
    parser.add_argument('--cost', type=float, default=0.0)
    parser.add_argument('--config-json', '--executor-config', dest='config_json', help='executor_config as JSON')
    parser.add_argument('--screen', help='size_in,density_dpi,refresh_hz,hdr')
    parser.add_argument('--lane-presets')
    parser.add_argument('--dry-run', action='store_true')


def build_parser():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--json', action='store_true', default=argparse.SUPPRESS,
                        help='machine-readable output on standard output')
    common.add_argument('--no-color', action='store_true', default=argparse.SUPPRESS, help='plain human output')
    parser = argparse.ArgumentParser(prog='ops.py', description=__doc__.splitlines()[0], parents=[common],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog='\n'.join(__doc__.splitlines()[2:16]))
    parser.add_argument('--store', help='result store URL (default: CERID_CI_STORE)')
    parser.add_argument('--config', default=str(ROOT / 'ci/coordinator.json'),
                        help='coordinator configuration (default: ci/coordinator.json)')
    nouns = parser.add_subparsers(dest='noun', metavar='<command>')

    def noun(name, help_text, verbs=None):
        node = nouns.add_parser(name, help=help_text, parents=[common])
        if verbs is None:
            return node, None
        return node, node.add_subparsers(dest='verb', metavar='<verb>', required=True)

    def verb(sub, name, help_text):
        return sub.add_parser(name, help=help_text, parents=[common])

    ui, _ = noun('ui', 'open the web UI through the IAP tunnel')
    ui.add_argument('--port', type=int, default=8780, help='local port (another free one if taken)')
    ui.add_argument('--instance', default='cerid-ci-launcher')
    ui.add_argument('--zone')
    ui.add_argument('--project')
    ui.add_argument('--timeout', type=int, default=60)
    ui.add_argument('--no-browser', action='store_true')
    ui.add_argument('--once', action='store_true', help=argparse.SUPPRESS)
    noun('status', 'every device cell, machines running, the month\'s spend')
    noun('health', 'store, coordinator tick, stuck jobs, lost agents, machines')
    noun('accounts', 'which account-backed inputs exist, by name only')
    noun('parity', 'the CI-4 table: every GitHub Actions step and the orchestrator step carrying it')
    cost, _ = noun('cost', 'spend, forecast and breakdown against the cap')
    cost.add_argument('--month')
    cost.add_argument('--by', choices=('build', 'lane', 'device'), default='lane')
    cost.add_argument('--pull', action='store_true', help='read the GCP budget notifications first')
    noun('slo', 'queue-time and availability objectives')
    _, sub = noun('device', 'registry cells', True)
    add_device_options(verb(sub, 'add', 'add one cell, validated with the whole registry'))
    bake = verb(sub, 'bake', 'bake a gce_vm cell\'s image family')
    bake.add_argument('device')
    bake.add_argument('--dry-run', action='store_true')
    verb(sub, 'list', 'every cell')
    _, sub = noun('build', 'builds', True)
    verb(sub, 'list', 'recent builds').add_argument('--limit', type=int, default=30)
    verb(sub, 'show', 'one build and its steps').add_argument('number', type=int)
    start = verb(sub, 'start', 'start a build')
    start.add_argument('--repository', default='cerid')
    start.add_argument('--branch', default='main')
    start.add_argument('--commit')
    start.add_argument('--message')
    start.add_argument('--tier', choices=('change', 'complete'))
    start.add_argument('--fresh', action='store_true', help='ignore the result cache')
    verb(sub, 'cancel', 'cancel a build').add_argument('number', type=int)
    _, sub = noun('job', 'jobs (steps of builds)', True)
    verb(sub, 'show', 'a job and its reproducibility record').add_argument('job')
    logs = verb(sub, 'logs', 'a job\'s log')
    logs.add_argument('job')
    logs.add_argument('--attempt', type=int)
    logs.add_argument('--follow', action='store_true')
    verb(sub, 'retry', 'run a failed job again').add_argument('job')
    verb(sub, 'cancel', 'cancel one job').add_argument('job')
    repro = verb(sub, 'reproduce', 'download a job\'s diagnostics and print (or run) its exact local replay')
    repro.add_argument('job')
    repro.add_argument('--attempt', type=int, help='an earlier attempt (default: the newest with diagnostics)')
    repro.add_argument('--dest', help='directory for the downloads (default build/ci-repro/<build>-<step>)')
    repro.add_argument('--run', action='store_true', help='check the commit out in DEST/src and run the lane there')
    _, sub = noun('agent', 'agents', True)
    verb(sub, 'list', 'every agent')
    verb(sub, 'stop', 'stop an agent').add_argument('agent')
    _, sub = noun('queue', 'queues', True)
    verb(sub, 'list', 'agents and waiting work per queue')
    _, sub = noun('token', 'registration tokens of owned machines', True)
    create = verb(sub, 'create', 'write a new registration token to a file')
    create.add_argument('--name', required=True)
    create.add_argument('--queue', default='cerid-owned')
    create.add_argument('--tags', help='k=v,... the agent can never override (device=<cell>)')
    create.add_argument('--out', help='file to create (mode 0600); the token is never printed')
    verb(sub, 'revoke', 'revoke a machine\'s tokens').add_argument('--name', required=True)
    _, sub = noun('test', 'per-test history and flaky tests', True)
    tl = verb(sub, 'list', 'tests per lane')
    tl.add_argument('--lane')
    tl.add_argument('--limit', type=int, default=40)
    th = verb(sub, 'history', 'one test across runs')
    th.add_argument('--lane', required=True)
    th.add_argument('--test', required=True)
    verb(sub, 'flaky', 'tests whose identical key both passed and failed')
    _, sub = noun('quarantine', 'quarantined flaky tests', True)
    verb(sub, 'list', 'every quarantine')
    rel = verb(sub, 'release', 'end a quarantine now')
    rel.add_argument('--lane', required=True)
    rel.add_argument('--test', required=True)
    _, sub = noun('bisect', 'first-bad-commit searches', True)
    verb(sub, 'list', 'every bisection')
    _, sub = noun('audit', 'the audit log', True)
    verb(sub, 'list', 'audited actions').add_argument('--days', type=int, default=7)
    _, sub = noun('notify', 'notifications', True)
    verb(sub, 'test', 'send a test notification to every channel')
    _, sub = noun('backup', 'backup and restore of the state', True)
    bc = verb(sub, 'create', 'write a backup')
    bc.add_argument('--out', required=True)
    bc.add_argument('--all', action='store_true', help='include caches, logs and artifacts')
    br = verb(sub, 'restore', 'restore a backup')
    br.add_argument('source')
    br.add_argument('--overwrite', action='store_true')
    _, sub = noun('retention', 'log and artifact retention', True)
    verb(sub, 'run', 'apply the retention now').add_argument('--limit', type=int, default=200)
    comp, _ = noun('completion', 'print a shell-completion script')
    comp.add_argument('shell', choices=('bash', 'zsh', 'powershell'))
    return parser


def legacy(argv):
    """Rewrite the earlier flat verbs to the noun-verb form (add-device -> device add, builds -> build list, ...)."""
    flags = [a for a in argv if a in ('--json', '--no-color')]
    rest = [a for a in argv if a not in ('--json', '--no-color')]
    index = 0
    while index < len(rest) and rest[index].startswith('-'):
        index += 2 if rest[index] in ('--store', '--config') else 1
    if index >= len(rest):
        return list(argv)
    head, name, tail = rest[:index], rest[index], rest[index + 1:]

    def value(flag):
        return tail[tail.index(flag) + 1] if flag in tail and tail.index(flag) + 1 < len(tail) else None
    if name == 'build' and tail and tail[0].startswith('-'):
        if '--number' in tail:
            new = ['build', 'show', value('--number')]
        else:
            new = ['build', 'start'] + tail
    elif name == 'backup' and (not tail or tail[0].startswith('-')):
        new = ['backup', 'create'] + tail
    elif name == 'cancel':
        new = ['build', 'cancel', value('--build')] if '--build' in tail else ['job', 'cancel', value('--job')]
    elif name == 'agents' and '--stop' in tail:
        new = ['agent', 'stop', value('--stop')]
    elif name == 'agent-token' and tail:
        new = ['token'] + tail
    elif name == 'add-device':
        # The old `--config <executor_config JSON>` is now --config-json: the global --config names the coordinator's
        # configuration file.
        new = ['device', 'add'] + ['--config-json' if a == '--config' else a for a in tail]
    elif name in LEGACY:
        target, verb_name = LEGACY[name]
        new = [target] + ([verb_name] if verb_name else []) + tail
    else:
        return list(argv)
    return head + new + flags


def main(argv=None, environ=None):
    argv = legacy(sys.argv[1:] if argv is None else argv)
    environ = os.environ if environ is None else environ
    parser = build_parser()
    args = parser.parse_args(argv)
    args.json = getattr(args, 'json', False)
    args.no_color = getattr(args, 'no_color', False)
    if hasattr(sys.stdout, 'reconfigure'):
        try:
            sys.stdout.reconfigure(encoding='utf-8')
        except ValueError:
            pass
    if not args.noun:
        parser.print_help()
        return 2
    out = Out(use_color(args))
    if args.noun == 'completion':
        sys.stdout.write(completion(args.shell, parser))
        return 0

    class Context:
        pass
    ctx = Context()
    ctx.args, ctx.environ, ctx.out = args, environ, out
    ctx.registry = pipeline().load_registry()
    handler = HANDLERS[(args.noun, getattr(args, 'verb', None))]
    try:
        code, data, lines = handler(ctx)
    except OpsError as error:
        return fail(args, out, str(error), error.hint)
    except Exception as error:  # a known failure with a next step, else the plain error
        message, hint = explain(error)
        return fail(args, out, message, hint)
    if args.json:
        if data is not None:
            print(json.dumps(data, indent=1, sort_keys=True, default=str))
    else:
        for line in lines:
            print(line)
    return code


def explain(error):
    """(message, next step) for the failures an operator meets."""
    name = type(error).__name__
    text = str(error) or name
    if name == 'NotFound':
        return text, 'list what exists: python ci/ops.py build list | job show | agent list'
    if name in ('Refused', 'TransitionError'):
        return text, 'reload the build (python ci/ops.py build show N) and decide again'
    if name == 'AuthError':
        return text, 'the token or identity was refused; see the runbook "an agent cannot connect"'
    if 'no Google Cloud credentials' in text or 'HTTP 401' in text or 'HTTP 403' in text:
        return text, ('sign in with `gcloud auth application-default login`, or set CERID_CI_STORE to a local '
                      'directory for an owned setup')
    if isinstance(error, (FileNotFoundError, PermissionError)):
        return text, 'check the path and that this user may read it'
    if isinstance(error, KeyError):
        return 'missing %s' % text, 'check the arguments with --help'
    return '%s: %s' % (name, text), 'run again with --json for the details, or see docs/recipes/ci-platform-runbooks.md'


def fail(args, out, message, hint):
    if args.json:
        print(json.dumps({'error': message, 'next': hint}))
    else:
        print(out.paint('error: ', 'bad') + message, file=sys.stderr)
        if hint:
            print(out.paint('next:  ', 'warn') + hint, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())
