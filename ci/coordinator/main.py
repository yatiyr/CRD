#!/usr/bin/env python3
"""Run the Cerid orchestrator's coordinator: `python ci/coordinator/main.py serve` (ADR-0135 §12).

The configuration is `ci/coordinator.json` (tracked, no values that are secret); a key ending in `_env` takes its
value from that environment variable, so the launcher's environment file holds everything machine-specific and
Secret Manager (or a directory of files on an owned machine) holds every secret. Recipe:
docs/recipes/ci-platform-setup.md.
"""
from __future__ import annotations

from pathlib import Path
import argparse
import ipaddress
import json
import os
import sys
import threading

CI = Path(__file__).resolve().parents[1]
if str(CI) not in sys.path:
    sys.path.insert(0, str(CI))

from coordinator import auth, dispatch, scheduler, service, triggers  # noqa: E402

CONFIG = CI / 'coordinator.json'
SCHEMA = 'cerid-ci-coordinator/1'


def resolve(value, environ):
    """Replace every `<key>_env: NAME` with `<key>: $NAME` (dropped when the variable is unset)."""
    if isinstance(value, list):
        return [resolve(v, environ) for v in value]
    if not isinstance(value, dict):
        return value
    out = {key: resolve(item, environ) for key, item in value.items()
           if not (key.endswith('_env') and isinstance(item, str))}
    for key, item in value.items():
        if key.endswith('_env') and isinstance(item, str) and environ.get(item):
            out[key[:-4]] = environ[item]  # the environment overrides a literal default of the same key
    return out


def load_config(path=CONFIG, environ=None):
    environ = os.environ if environ is None else environ
    document = json.loads(Path(path).read_text(encoding='utf-8'))
    if document.get('schema') != SCHEMA:
        raise SystemExit('%s is not a %s document' % (path, SCHEMA))
    config = resolve(document, environ)
    problems = []
    for name, repo in config.get('repositories', {}).items():
        if not repo.get('url'):
            problems.append('repository %s has no url (set %s)' % (
                name, document['repositories'][name].get('url_env', 'its url')))
        if repo.get('checkout', 'direct') not in scheduler.CHECKOUT_STRATEGIES:
            problems.append('repository %s: checkout %r is not one of %s' % (
                name, repo.get('checkout'), ', '.join(scheduler.CHECKOUT_STRATEGIES)))
        status = repo.get('status')
        if status and not status.get('repository'):
            repo.pop('status')  # commit status is optional: without its repository name it is off
    return config, problems


def loopback(host):
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return host == 'localhost'


def secrets_provider(config, environ):
    spec = config.get('secrets', {})
    if spec.get('provider') == 'directory' or environ.get('CERID_CI_SECRETS_DIR'):
        return scheduler.DirectorySecrets(environ.get('CERID_CI_SECRETS_DIR') or spec['directory'])
    project = environ.get('CERID_CI_GCP_PROJECT')
    return scheduler.SecretManager(project) if project else None


def listeners(config, environ, override=None):
    """[(host, port, surfaces)]: CERID_CI_LISTEN (or --listen) is a comma-separated list of host:port, each
    optionally followed by `=ui+agent` style surfaces; the configuration's `listen` list otherwise. The default is
    loopback only."""
    text = override or environ.get('CERID_CI_LISTEN')
    items = [x.strip() for x in text.split(',') if x.strip()] if text else config.get('listen', ['127.0.0.1:8780'])
    if isinstance(items, str):
        items = [items]
    out = []
    for item in items:
        address, _, surfaces = item.partition('=')
        host, _, port = address.rpartition(':')
        chosen = tuple(surfaces.split('+')) if surfaces else ('ui', 'agent')
        unknown = set(chosen) - set(service.SURFACES)
        if unknown:
            raise SystemExit('unknown listener surfaces %s in %s' % (', '.join(sorted(unknown)), item))
        out.append((host.strip('[]'), int(port), chosen))
    return out


def ui_auth(config, environ, host):
    spec = config.get('ui', {})
    mode = environ.get('CERID_CI_UI_MODE') or spec.get('mode', 'iap-tunnel')

    def names(value):
        return [a.strip() for a in (value or '').split(',') if a.strip()] if isinstance(value, str) else list(
            value or [])
    allowed = names(spec.get('allowed_emails')) + names(spec.get('allowed_logins'))
    from executors import gce_vm

    def fetch(url):
        return gce_vm.http_request('GET', url)[1]
    return auth.UiAuth(mode, audience=spec.get('audience', ''), team=spec.get('team', ''), fetch=fetch,
                       allowed=allowed, loopback=loopback(host), networks=spec.get('networks'))


def identity_verifier(config, environ):
    account = environ.get('CERID_CI_RUNNER_SERVICE_ACCOUNT')
    project = environ.get('CERID_CI_GCP_PROJECT')
    audience = config.get('internal_url') or config.get('url')
    if not (account and project and audience):
        return None
    from executors import gce_vm
    jwks = auth.Jwks(auth.GOOGLE_CERTS, lambda url: gce_vm.http_request('GET', url)[1])

    def verify(token):
        claims = auth.verify_jwt(token, jwks, audience.rstrip('/'), auth.GOOGLE_ISSUERS, ('RS256',))
        return auth.google_instance(claims, account, project)[0]
    return verify


def build(config, environ, store=None, log=None):
    """Every component of a running coordinator, from the configuration and the environment."""
    p = scheduler.pipeline_module()
    store = store or p.open_store(environ.get('CERID_CI_STORE'))
    from executors import gce_vm

    def http(method, url, body=None, headers=None):
        return gce_vm.http_request(method, url, body=body, headers=headers)
    mirror = None
    if any(r.get('checkout') == 'mirror' for r in config.get('repositories', {}).values()):
        mirror = triggers.Mirror(environ.get('CERID_CI_MIRROR_DIR') or config.get('mirror_dir',
                                                                                   '/var/lib/cerid-ci/mirror'))
    coordinator = scheduler.Coordinator(store, config, secrets=secrets_provider(config, environ), http=http,
                                        log=log, mirror=mirror)
    registry = p.load_registry()
    dispatcher = None
    if environ.get('CERID_CI_GCP_PROJECT'):
        import executors
        data, digest = service.Service(coordinator, None).agent_bytes()
        internal = (config.get('internal_url') or config.get('url', '')).rstrip('/')
        dispatcher = dispatch.Dispatcher(coordinator, registry, executors.registry(environ), environ, agent={
            'coordinator': internal, 'external': config.get('url', '').rstrip('/'),
            'agent_url': internal + '/agent/cerid-agent.py', 'agent_sha256': digest})
    return {'store': store, 'coordinator': coordinator, 'registry': registry, 'dispatcher': dispatcher,
            'mirror': mirror}


def qual_renderer(store, registry):
    """The device x feature matrix as data, for the web UI's own QUAL page."""
    p = scheduler.pipeline_module()
    module = dashboard_module()

    def data():
        month = p.month_of()
        budget = {'cap_usd': registry['budget']['monthly_cap_usd'], 'spent_usd': p.month_spend(store, month),
                  'month': month}
        _, document = module.render(registry, module.results_from_store(store), None, module.roadmap_rows(), budget)
        return {'features': {k: {'title': v['title'], 'rows': v.get('rows', []), 'enforced_by': v.get('enforced_by')}
                             for k, v in registry['features'].items()},
                'devices': [{'id': d['id'], 'kind': d['kind'], 'tier': d['tier'], 'status': d['status'],
                             'os': '%s %s' % (d['os']['name'], d['os']['version']), 'arch': d['cpu']['arch'],
                             'gpu': d['gpu']['model'], 'executor': d['executor'], 'features': d['features']}
                            for d in registry['devices']],
                'cells': document['cells'], 'regressions': document['regressions'], 'budget': document['budget']}
    return data


def dashboard_module():
    import importlib.util
    module = sys.modules.get('ci_dashboard')
    if module is None:
        spec = importlib.util.spec_from_file_location('ci_dashboard', CI / 'dashboard/generate.py')
        module = importlib.util.module_from_spec(spec)
        sys.modules['ci_dashboard'] = module
        spec.loader.exec_module(module)
    return module


def quality_work(coordinator, registry, mirror, config):
    """The periodic test-quality pass: quarantine proven flaky tests, expire quarantines, drive bisections."""
    from coordinator import quality
    spec = config.get('quality', {})

    def commits_between(repository, good, bad):
        if mirror is not None and coordinator.repositories.get(repository, {}).get('checkout') == 'mirror':
            return mirror.commits_between(repository, good, bad)
        built = [b for b in reversed(coordinator.recent(200)) if b['repository'] == repository]
        commits = [b['commit'] for b in built]
        if good in commits and bad in commits:
            inside = commits[commits.index(good) + 1:commits.index(bad)]
            return list(dict.fromkeys(c for c in inside if c not in (good, bad)))
        return []

    def work():
        quality.scan(coordinator, days=spec.get('quarantine_days', quality.QUARANTINE_DAYS),
                     max_active=spec.get('max_quarantined', quality.MAX_ACTIVE))
        if spec.get('bisect', True):
            quality.tick(coordinator, registry, commits_between)
    return work


def dashboard_renderer(store, registry):
    p = scheduler.pipeline_module()
    import importlib.util
    spec = importlib.util.spec_from_file_location('ci_dashboard', CI / 'dashboard/generate.py')
    module = sys.modules.get('ci_dashboard')
    if module is None:
        module = importlib.util.module_from_spec(spec)
        sys.modules['ci_dashboard'] = module
        spec.loader.exec_module(module)

    def render():
        month = p.month_of()
        budget = {'cap_usd': registry['budget']['monthly_cap_usd'], 'spent_usd': p.month_spend(store, month),
                  'month': month}
        page, _ = module.render(registry, module.results_from_store(store), None, module.roadmap_rows(), budget)
        return page
    return render


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    serve = sub.add_parser('serve', help='run the coordinator')
    serve.add_argument('--config', default=str(CONFIG))
    serve.add_argument('--listen', default=None, help='host:port[=ui+agent],... (default: CERID_CI_LISTEN, then '
                       'the configuration, then 127.0.0.1:8780)')
    serve.add_argument('--interval', type=float, default=5.0)
    check = sub.add_parser('check', help='validate the configuration and the environment, print what is missing')
    check.add_argument('--config', default=str(CONFIG))
    args = parser.parse_args(argv)
    config, problems = load_config(args.config)
    if args.command == 'check':
        for problem in problems:
            print('CONFIG: ' + problem)
        print('configuration %s: %d repositories, UI %s' % ('incomplete' if problems else 'complete',
                                                            len(config.get('repositories', {})),
                                                            config.get('ui', {}).get('mode')))
        return 1 if problems else 0
    if problems:
        for problem in problems:
            print('CONFIG: ' + problem, file=sys.stderr)
        return 1
    bound = listeners(config, os.environ, args.listen)
    parts = build(config, os.environ)
    c = parts['coordinator']
    c.ensure_schema()
    ui_hosts = [host for host, _, surfaces in bound if 'ui' in surfaces]
    ui = ui_auth(config, os.environ, ui_hosts[0] if len(ui_hosts) == 1 else '0.0.0.0')
    web = service.Service(c, ui, config, registry=parts['registry'],
                          identity=identity_verifier(config, os.environ), mirror=parts['mirror'],
                          dashboard=dashboard_renderer(parts['store'], parts['registry']),
                          qual_data=qual_renderer(parts['store'], parts['registry']))
    loop = service.Loop(c, parts['dispatcher'], interval=args.interval, poll=lambda: triggers.poll(c),
                        schedules=lambda: triggers.schedules(c),
                        quality=quality_work(c, parts['registry'], parts['mirror'], config))
    loop.start()
    servers = [service.serve(web, host, port, surfaces) for host, port, surfaces in bound]
    for server in servers[1:]:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    c.log('coordinator_started', listen=['%s:%d=%s' % (h, p, '+'.join(s)) for h, p, s in bound],
          version=service.VERSION, ui=ui.mode)
    try:
        servers[0].serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        loop.stop.set()
        for server in servers:
            server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
