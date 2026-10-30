#!/usr/bin/env python3
"""ci/ops.py: the one operator command of the Cerid CI platform (ADR-0135).

  status       every device cell: runnable or why it waits, baked images, machines running, the month's spend
  add-device   add one cell to ci/devices.json, validated with the whole registry; prints an owned agent's setup
  budget       the cap, the month's spend, the GCP budget stop and what a complete run would cost
  accounts     which account-backed lanes are configured, by name only (never a value)
  parity       the CI-4 table: every GitHub Actions job and step and the Cerid orchestrator step that carries it
  bake         bake a device's image family (gce_vm) from its base image

The orchestrator (ci/coordinator, ADR-0135 §12), on the same state the running coordinator uses:

  builds       the recent builds                  build        start a build, or show one (build --number N)
  retry        retry a job                        cancel       cancel a build or one job
  agents       the agents (agents --stop ID)      queues       every queue's agents and waiting jobs
  logs         a job's log (logs JOB --follow)    agent-token  create or revoke an owned machine's registration
  health       store, coordinator tick, stuck jobs, lost agents, machines
  backup       the state store to a tar.gz        restore      a backup back into the store
"""
from __future__ import annotations

from pathlib import Path
import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def pipeline():
    if 'ci_pipeline' in sys.modules:
        return sys.modules['ci_pipeline']
    spec = importlib.util.spec_from_file_location('ci_pipeline', ROOT / 'ci/pipeline.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules['ci_pipeline'] = module
    spec.loader.exec_module(module)
    return module


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


def cmd_accounts(args, registry, environ):
    secrets = secret_names(environ)
    rows = accounts_report(registry, environ, secrets)
    print('Account-backed inputs (names only; values are never read here). Setup: docs/recipes/ci-platform-setup.md')
    width = max(len(r[0]) for r in rows)
    for name, kind, where, state, users, recipe in rows:
        print('  %-*s  %-8s  %-18s  cells %-3d  %s  [#%s]' % (width, name, kind, state, users, where, recipe))
    return 0


def cmd_status(args, registry, environ):
    p = pipeline()
    secrets = secret_names(environ)
    store = p.open_store(args.store)
    month = p.month_of()
    print('Registry: %d cells (%s)' % (len(registry['devices']), ', '.join(
        '%s %d' % (s, sum(1 for d in registry['devices'] if d['status'] == s)) for s in p.STATUSES)))
    for device in registry['devices']:
        if device['status'] != 'active':
            why = device['status']
        else:
            missing = p.missing_accounts(device, registry, environ, secrets)
            why = 'runnable' if not missing else 'waiting: account ' + ', '.join(missing)
        image = ''
        if device['executor'] == 'gce_vm':
            baked = store.get_json('images/%s.json' % device['executor_config']['image_family'])
            image = ' image %s' % (baked['image'] if baked else 'NOT BAKED (ci/ops.py bake %s)' % device['id'])
        print('  %-34s %s  %-17s %-11s %s%s' % (device['id'], device['tier'], device['executor'], device['cost_class'],
                                              why, image))
    running = [store.get_json(n) for n in store.list('machines/')]
    running = [r for r in running if r and not r.get('ended')]
    print('Machines running: %d' % len(running))
    for record in running:
        print('  %s on %s since %s' % (record['job'], record['device'], record['started']))
    print('Spend %s: %.2f of %.2f USD%s' % (month, p.month_spend(store, month), registry['budget']['monthly_cap_usd'],
                                           '; GCP budget stop in force' if p.gcp_budget_stop(store, month) else ''))
    problems = p.validate_registry(registry, p.ci_tier.load_tiers(p.TIERS), rows=set(p.roadmap_ids()))
    for problem in problems:
        print('REGISTRY: ' + problem)
    return 1 if problems else 0


def cmd_budget(args, registry, environ):
    p = pipeline()
    store = p.open_store(args.store)
    month = args.month or p.month_of()
    if args.pull:
        subscription = environ.get('CERID_CI_BUDGET_SUBSCRIPTION')
        if not subscription:
            print('CERID_CI_BUDGET_SUBSCRIPTION is not set; see the recipe section on the budget stop')
            return 1
        print('Read %d budget notifications' % len(p.pull_budget(store, subscription)))
    cap = float(registry['budget']['monthly_cap_usd'])
    spent = p.month_spend(store, month)
    print('Cap: %.2f USD per month (pipeline ledger and the GCP budget both enforce it)' % cap)
    print('Spent %s: %.2f USD recorded machine time; GCP budget stop: %s' % (
        month, spent, 'IN FORCE' if p.gcp_budget_stop(store, month) else 'no'))
    note = store.get_json('budget/gcp-%s.json' % month)
    if note:
        print('Last GCP notification: %s of %s %s' % (note.get('costAmount'), note.get('budgetAmount'),
                                                      note.get('currencyCode', '')))
    defaults = registry['budget']['default_minutes']
    total = 0.0
    print('One complete run, estimated (default minutes until the store has durations):')
    tiers = p.ci_tier.load_tiers(p.TIERS)
    hosts = p.job_hosts(registry)
    devices = p.devices_by_id(registry)
    lines = []
    for preset, entry in tiers['presets'].items():
        if entry['tier'] == 'diagnostic':
            continue
        device = devices[hosts[entry['job']]]
        minutes = p.estimate_minutes(store, 'lane-%s' % preset, defaults['lane'])
        lines.append(('lane-%s' % preset, device, minutes))
    for device in registry['devices']:
        if device['status'] != 'active':
            continue
        for preset in device.get('lane_presets', []):
            key = 'lane-%s@%s' % (preset, device['id'])
            lines.append((key, device, p.estimate_minutes(store, key, defaults['lane'])))
        if device.get('console_profile'):
            key = 'console-%s' % device['id']
            lines.append((key, device, p.estimate_minutes(store, key, defaults['console-profile'])))
    for key, device, minutes in lines:
        cost = p.step_cost(device, minutes)
        total += cost
        print('  %-52s %-30s %6.0f min  %6.2f USD' % (key, device['id'], minutes, cost))
    print('Complete run: %.2f USD before the result cache; runs left this month at that price: %d' % (
        total, int(max(cap - spent, 0.0) // total) if total else 0))
    return 0


def cmd_parity(args, registry, environ):
    p = pipeline()
    rows, problems = p.parity_rows()
    print(p.parity_markdown(rows), end='')
    print('\nDispatch-only experiments (host-check.yml):\n')
    print(p.parity_markdown(p.experiment_rows()), end='')
    for problem in problems:
        print('PARITY: ' + problem, file=sys.stderr)
    return 1 if problems else 0


def device_entry(args):
    entry = {
        'id': args.id, 'status': args.status, 'kind': args.kind,
        'os': {'name': args.os, 'version': args.os_version},
        'cpu': {'model': args.cpu_model, 'arch': args.arch, 'levels': args.levels.split(',')},
        'gpu': {'vendor': args.gpu_vendor, 'model': args.gpu_model,
                'apis': [a for a in (args.apis or '').split(',') if a]},
        'executor': args.executor, 'executor_config': json.loads(args.config or '{}'), 'tier': args.tier,
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


def cmd_add_device(args, registry, environ):
    p = pipeline()
    entry = device_entry(args)
    text = p.DEVICES.read_text(encoding='utf-8')
    updated = insert_device(text, entry)
    candidate = json.loads(updated)
    problems = p.validate_registry(candidate, p.ci_tier.load_tiers(p.TIERS), rows=set(p.roadmap_ids()))
    if problems:
        for problem in problems:
            print('REFUSED: ' + problem)
        return 1
    if not args.dry_run:
        p.DEVICES.write_text(updated, encoding='utf-8', newline='\n')
    print('%s %s (%s, tier %s)' % ('Would add' if args.dry_run else 'Added', entry['id'], entry['executor'],
                                   entry['tier']))
    if entry['executor'] == 'owned_agent':
        from executors import owned_agent
        agent = owned_agent.OwnedAgent(environ)
        print('\nOn the machine, the agent:\n' + agent.config(entry))
        for step in agent.install(entry):
            print('  - ' + step)
    return 0


def cmd_bake(args, registry, environ):
    p = pipeline()
    from executors import gce_vm
    device = p.devices_by_id(registry)[args.device]
    if device['executor'] != 'gce_vm':
        print('%s is not a gce_vm cell' % args.device)
        return 1
    executor = gce_vm.GceVm(environ)
    packages = list(p.APT_PACKAGES['arm64' if device['cpu']['arch'] == 'aarch64' else 'x64'])
    script = gce_vm.provision_script(device, packages)
    zone, body = executor.bake_body(device, script)
    if args.dry_run:
        print(json.dumps({'zone': zone, 'instance': body}, indent=1))
        return 0
    base = '%s/projects/%s/zones/%s/instances' % (gce_vm.COMPUTE, executor.project, zone)
    executor.http('POST', base, token=executor.token(), body=body)
    print('baking %s on %s; waiting for it to power off' % (args.device, body['name']))
    deadline = time.time() + 4 * 3600
    while time.time() < deadline:
        _, state = executor.http('GET', '%s/%s' % (base, body['name']), token=executor.token())
        if state.get('status') == 'TERMINATED':
            break
        time.sleep(30)
    else:
        print('bake did not finish in four hours; the instance stays for inspection')
        return 1
    stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%d%H%M')
    image = executor.image_body(device, zone, body['name'], stamp)
    executor.http('POST', '%s/projects/%s/global/images' % (gce_vm.COMPUTE, executor.project), token=executor.token(),
                  body=image)
    executor.http('DELETE', '%s/%s' % (base, body['name']), token=executor.token(), allow=(404,))
    p.open_store(args.store).put_json('images/%s.json' % image['family'], {'image': image['name'],
                                                                           'family': image['family'], 'baked': stamp,
                                                                           'device': device['id']})
    print('image %s in family %s' % (image['name'], image['family']))
    return 0


# --------------------------------------------------------------------------------------------- orchestrator

def orchestrator(args, environ):
    """The coordinator library on the state store, configured like the running coordinator. Every command works
    beside a running coordinator: all writes are compare-and-swap."""
    if str(ROOT / 'ci') not in sys.path:
        sys.path.insert(0, str(ROOT / 'ci'))
    from coordinator import main as coordinator_main, scheduler
    config, _ = coordinator_main.load_config(args.config, environ)
    store = pipeline().open_store(args.store)
    log = scheduler.JsonLog(open(os.devnull, 'w'))
    return scheduler.Coordinator(store, config, secrets=coordinator_main.secrets_provider(config, environ), log=log)


def when(stamp):
    return time.strftime('%Y-%m-%d %H:%M:%SZ', time.gmtime(stamp)) if stamp else '-'


def cmd_builds(args, registry, environ):
    c = orchestrator(args, environ)
    for b in c.recent(args.limit):
        print('#%-6d %-9s %-10s %-24s %s  %s' % (b['number'], b['state'], b['repository'], (b['branch'] or '')[:24],
                                                 (b['commit'] or '')[:12], (b['message'] or '')[:60]))
    return 0


def cmd_build(args, registry, environ):
    c = orchestrator(args, environ)
    if args.number:
        view = c.build_view(args.number)
        print('Build #%d %s  %s %s %s' % (view['number'], view['state'], view['repository'], view['branch'],
                                         view['commit']))
        for job in view['jobs']:
            print('  %-40s %-9s attempt %d  %s' % (job['key'][:40], job['state'] + (' (soft)' if job['soft_failed']
                                                                                   else ''), job['attempt'],
                                                   job.get('waiting') or job['reason'] or ''))
        return 0 if view['state'] in ('passed', 'running', 'scheduled') else 1
    from coordinator import hosts
    repo = c.repository(args.repository)
    commit = args.commit or hosts.ls_remote(repo['url'], [args.branch])[args.branch]
    env = {}
    if args.tier:
        env['CERID_CI_TIER'] = args.tier
    if args.fresh:
        env['CERID_CI_FRESH'] = '1'
    build, _ = c.create_build(args.repository, {'event': 'push', 'branch': args.branch, 'commit': commit,
                                                'message': args.message or 'manual build (ci/ops.py)',
                                                'trusted': True}, 'manual', env=env, created_by='ops.py')
    print('build #%d scheduled for %s at %s' % (build['number'], args.branch, commit))
    return 0


def cmd_retry(args, registry, environ):
    job = orchestrator(args, environ).retry_job(args.job, by='ops.py')
    print('job %s (%s) is %s, attempt %d' % (job['id'], job['key'], job['state'], job['attempt']))
    return 0


def cmd_cancel(args, registry, environ):
    c = orchestrator(args, environ)
    if args.job:
        c.cancel_job(args.job, reason='canceled by ops.py')
    else:
        c.cancel_build(args.build, by='ops.py')
    print('cancel requested')
    return 0


def cmd_agents(args, registry, environ):
    c = orchestrator(args, environ)
    if args.stop:
        c.stop_agent(args.stop)
        print('agent %s stopped' % args.stop)
        return 0
    for agent in c.agents():
        print('%-24s %-8s %-14s %-10s %-8s %s  last %s' % (agent['name'][:24], agent['status'], agent['queue'],
                                                          agent['os'], agent['arch'], ','.join(
                                                              '%s=%s' % kv for kv in sorted(agent['tags'].items())),
                                                          when(agent.get('last_seen'))))
    return 0


def cmd_queues(args, registry, environ):
    for queue in orchestrator(args, environ).queues():
        print('%-16s agents %-3d scheduled %-3d assigned %-3d running %-3d oldest wait %ds' % (
            queue['queue'], queue['agents'], queue['scheduled'], queue['assigned'], queue['running'],
            queue['oldest_wait_s']))
    return 0


def cmd_logs(args, registry, environ):
    c = orchestrator(args, environ)
    start = 0
    while True:
        data, start, live, job = c.read_log(args.job, args.attempt, start)
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
        if not (args.follow and live):
            return 0
        time.sleep(2)


def cmd_agent_token(args, registry, environ):
    c = orchestrator(args, environ)
    if args.action == 'revoke':
        print('%d registration tokens revoked for %s' % (c.revoke_registration(args.name), args.name))
        return 0
    if not args.out:
        print('agent-token create needs --out FILE: the token is written there, never printed')
        return 1
    out = Path(args.out)
    if out.exists():
        print('%s exists; refusing to overwrite a token file' % out)
        return 1
    tags = dict(part.split('=', 1) for part in (args.tags or '').split(',') if '=' in part)
    token = c.create_registration(args.name, args.queue, tags)
    descriptor = os.open(str(out), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
        handle.write(token + '\n')
    print('registration token for %s (queue %s) written to %s; move it to the machine, readable only by the agent '
          'user' % (args.name, args.queue, out))
    return 0


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
    running = [m for m in machines if m and not m.get('ended')]
    lines.append('machines running: %d' % len(running))
    return ok, lines


def cmd_health(args, registry, environ):
    ok, lines = health_report(orchestrator(args, environ))
    for line in lines:
        print(line)
    return 0 if ok else 1


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


def cmd_backup(args, registry, environ):
    manifest = backup(pipeline().open_store(args.store), args.out, args.all)
    print('%d objects backed up to %s' % (len(manifest), args.out))
    return 0


def cmd_restore(args, registry, environ):
    written, kept = restore(pipeline().open_store(args.store), args.source, args.overwrite)
    print('%d objects restored, %d existing objects kept%s' % (written, kept, '' if args.overwrite or not kept else
                                                                ' (--overwrite replaces them)'))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--store', help='result store URL (default: CERID_CI_STORE)')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('status')
    bud = sub.add_parser('budget')
    bud.add_argument('--month')
    bud.add_argument('--pull', action='store_true', help='read the GCP budget notifications first')
    sub.add_parser('accounts')
    sub.add_parser('parity')
    bake = sub.add_parser('bake')
    bake.add_argument('device')
    bake.add_argument('--dry-run', action='store_true')
    add = sub.add_parser('add-device')
    for flag, default in (('--id', None), ('--kind', None), ('--os', None), ('--os-version', None), ('--arch', None),
                          ('--cpu-model', None), ('--levels', None), ('--gpu-vendor', None), ('--gpu-model', None),
                          ('--executor', None), ('--tier', None), ('--cost-class', None), ('--features', None),
                          ('--route', None)):
        add.add_argument(flag, required=True, default=default)
    add.add_argument('--status', default='active')
    add.add_argument('--apis')
    add.add_argument('--accounts')
    add.add_argument('--cost', type=float, default=0.0)
    add.add_argument('--config', help='executor_config as JSON')
    add.add_argument('--screen', help='size_in,density_dpi,refresh_hz,hdr')
    add.add_argument('--lane-presets')
    add.add_argument('--dry-run', action='store_true')
    config = str(ROOT / 'ci/coordinator.json')
    parser.add_argument('--config', default=config, help='coordinator configuration (default ci/coordinator.json)')
    builds = sub.add_parser('builds')
    builds.add_argument('--limit', type=int, default=30)
    bld = sub.add_parser('build')
    bld.add_argument('--number', type=int)
    bld.add_argument('--repository', default='cerid')
    bld.add_argument('--branch', default='main')
    bld.add_argument('--commit')
    bld.add_argument('--message')
    bld.add_argument('--tier', choices=('change', 'complete'))
    bld.add_argument('--fresh', action='store_true')
    retry = sub.add_parser('retry')
    retry.add_argument('job')
    cancel = sub.add_parser('cancel')
    target = cancel.add_mutually_exclusive_group(required=True)
    target.add_argument('--build', type=int)
    target.add_argument('--job')
    agents = sub.add_parser('agents')
    agents.add_argument('--stop')
    sub.add_parser('queues')
    logs = sub.add_parser('logs')
    logs.add_argument('job')
    logs.add_argument('--attempt', type=int)
    logs.add_argument('--follow', action='store_true')
    token = sub.add_parser('agent-token')
    token.add_argument('action', choices=('create', 'revoke'))
    token.add_argument('--name', required=True)
    token.add_argument('--queue', default='cerid-owned')
    token.add_argument('--tags', help='k=v,... the agent can never override (device=<cell>)')
    token.add_argument('--out', help='the file to write the token to (created, mode 0600; never printed)')
    sub.add_parser('health')
    bak = sub.add_parser('backup')
    bak.add_argument('--out', required=True)
    bak.add_argument('--all', action='store_true', help='include caches, logs and artifacts')
    res = sub.add_parser('restore')
    res.add_argument('source')
    res.add_argument('--overwrite', action='store_true')
    args = parser.parse_args(argv)
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    registry = pipeline().load_registry()
    handlers = {'status': cmd_status, 'budget': cmd_budget, 'accounts': cmd_accounts, 'parity': cmd_parity,
                'add-device': cmd_add_device, 'bake': cmd_bake, 'builds': cmd_builds, 'build': cmd_build,
                'retry': cmd_retry, 'cancel': cmd_cancel, 'agents': cmd_agents, 'queues': cmd_queues,
                'logs': cmd_logs, 'agent-token': cmd_agent_token, 'health': cmd_health, 'backup': cmd_backup,
                'restore': cmd_restore}
    return handlers[args.command](args, registry, os.environ)


if __name__ == '__main__':
    sys.exit(main())
