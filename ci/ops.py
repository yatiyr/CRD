#!/usr/bin/env python3
"""ci/ops.py: the one operator command of the Cerid CI platform (ADR-0135).

  status       every device cell: runnable or why it waits, baked images, machines running, the month's spend
  add-device   add one cell to ci/devices.json, validated with the whole registry; prints an owned agent's setup
  budget       the cap, the month's spend, the GCP budget stop and what a complete run would cost
  accounts     which account-backed lanes are configured, by name only (never a value)
  parity       the CI-4 table: every GitHub Actions job and step and the Buildkite step that carries it
  bake         bake a device's image family (gce_vm) from its base image
"""
from __future__ import annotations

from pathlib import Path
import argparse
import datetime as dt
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
        print('\nOn the machine, buildkite-agent.cfg:\n' + agent.config(entry))
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
    args = parser.parse_args(argv)
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    registry = pipeline().load_registry()
    handlers = {'status': cmd_status, 'budget': cmd_budget, 'accounts': cmd_accounts, 'parity': cmd_parity,
                'add-device': cmd_add_device, 'bake': cmd_bake}
    return handlers[args.command](args, registry, os.environ)


if __name__ == '__main__':
    sys.exit(main())
