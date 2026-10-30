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


def ui_auth(config, environ, host):
    spec = config.get('ui', {})
    mode = environ.get('CERID_CI_UI_MODE') or spec.get('mode', 'cloudflare-access')
    allowed = [a.strip() for a in (spec.get('allowed_emails') or '').split(',') if a.strip()]
    from executors import gce_vm

    def fetch(url):
        return gce_vm.http_request('GET', url)[1]
    return auth.UiAuth(mode, audience=spec.get('audience', ''), team=spec.get('team', ''), fetch=fetch,
                       allowed=allowed, loopback=loopback(host))


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
    serve.add_argument('--listen', default=None, help='host:port (default: the configuration, 127.0.0.1:8780)')
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
    listen = args.listen or os.environ.get('CERID_CI_LISTEN') or config.get('listen', '127.0.0.1:8780')
    host, _, port = listen.rpartition(':')
    parts = build(config, os.environ)
    c = parts['coordinator']
    c.ensure_schema()
    web = service.Service(c, ui_auth(config, os.environ, host), config, registry=parts['registry'],
                          identity=identity_verifier(config, os.environ), mirror=parts['mirror'],
                          dashboard=dashboard_renderer(parts['store'], parts['registry']))
    loop = service.Loop(c, parts['dispatcher'], interval=args.interval, poll=lambda: triggers.poll(c),
                        schedules=lambda: triggers.schedules(c))
    loop.start()
    server = service.serve(web, host, int(port))
    c.log('coordinator_started', listen=listen, version=service.VERSION)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        loop.stop.set()
        server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
