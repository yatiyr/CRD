#!/usr/bin/env python3
"""The QUAL dashboard: devices x features, rendered as one static page (ADR-0135 §7, roadmap row CI-3).

Every cell is pass, fail, cached (an unchanged input key that passed before, linked to that run), waiting (a planned
device, a missing account, an engine option or roadmap row not yet delivered) or budget (a paid cell the monthly cap
stopped). A cell whose rendered images wait for a person's triage (the gold service, ci/coordinator/gold.py) reads
needs triage, and one with a rejected image fails. A budget, waiting or needs-triage cell is never shown as a pass.
The page also carries per-device trends, regressions
(a cell that passed in its previous run and fails now), the month's spend against the cap and the roadmap clauses
that have no evidence yet. Inputs: the result store (or a fixture with the same records), the build plan and the
device registry. `publish` deploys the page to Cloudflare Pages behind Cloudflare Access when that account exists.
"""
from __future__ import annotations

from pathlib import Path
import argparse
import html
import importlib.util
import json
import os
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
FIXTURE = HERE / 'fixtures/results.json'


def load_pipeline():
    if 'ci_pipeline' in sys.modules:
        return sys.modules['ci_pipeline']
    spec = importlib.util.spec_from_file_location('ci_pipeline', ROOT / 'ci/pipeline.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules['ci_pipeline'] = module
    spec.loader.exec_module(module)
    return module


def build_number(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return -1


def results_from_store(store):
    records = []
    for name in store.list('results/'):
        record = store.get_json(name)
        if record:
            records.append(record)
    return records


def record_features(record, registry):
    if record.get('features'):
        return [f for f in record['features'] if f]
    device = next((d for d in registry['devices'] if d['id'] == record['device']), None)
    return list(device['features']) if device else []


def matrix(registry, results, planned=None):
    """{(device, feature): cell} plus per-device history. The newest build wins; the plan adds cached, budget and
    waiting cells for this build; a cell no input names stays absent (not applicable)."""
    cells = {}
    history = {}
    for record in sorted(results, key=lambda r: (build_number(r.get('build')), r.get('finished', ''))):
        outcome = {'pass': 'pass', 'budget': 'budget'}.get(record.get('outcome'), 'fail')
        for feature in record_features(record, registry):
            key = (record['device'], feature)
            previous = cells.get(key)
            cells[key] = {'state': outcome, 'build': record.get('build'), 'log': record.get('log'),
                          'step': record.get('step'), 'previous': previous['state'] if previous else None,
                          'reason': record.get('reason', ''), 'job': record.get('job_id'),
                          'diagnostics': record.get('diagnostics') or [], 'phase': record.get('failed_phase')}
        if outcome != 'budget':
            history.setdefault(record['device'], []).append((build_number(record.get('build')), outcome))
    if planned:
        for step in planned.get('steps', []):
            if step['state'] not in ('cached', 'budget'):
                continue
            for feature in step['features']:
                key = (step['device'], feature)
                previous = cells.get(key)
                cells[key] = {'state': step['state'], 'build': (step.get('cached_from') or {}).get('build'),
                              'log': (step.get('cached_from') or {}).get('log'), 'step': step['key'],
                              'previous': previous['state'] if previous else None, 'reason': step['reason']}
        for item in planned.get('cells', []):
            for feature in item['features']:
                key = (item['device'], feature)
                if key not in cells or cells[key]['state'] in ('waiting',):
                    cells[key] = {'state': 'waiting', 'build': None, 'log': None, 'step': None, 'previous': None,
                                  'reason': item['reason']}
    devices = {d['id']: d for d in registry['devices']}
    for device in registry['devices']:
        if device['status'] != 'active':
            for feature in device['features']:
                cells[(device['id'], feature)] = {'state': 'waiting', 'build': None, 'log': None, 'step': None,
                                                  'previous': None, 'reason': 'waiting: %s' % device['status']}
        for feature, row in device.get('planned_features', {}).items():
            key = (device['id'], feature)
            if cells.get(key, {}).get('state') not in ('pass', 'fail'):
                cells[key] = {'state': 'waiting', 'build': None, 'log': None, 'step': None, 'previous': None,
                              'reason': 'waiting: row %s' % row}
    for key in list(cells):
        if key[0] not in devices:
            del cells[key]
    return cells, history


def regressions(cells):
    return sorted(key for key, cell in cells.items() if cell['state'] == 'fail' and cell['previous'] in ('pass', 'cached'))


def clauses_without_evidence(registry, cells, rows):
    """Roadmap rows named by a feature or a planned feature that no cell has green evidence for yet."""
    evidenced = {}
    for (device, feature), cell in cells.items():
        if cell['state'] in ('pass', 'cached'):
            evidenced.setdefault(feature, []).append(device)
    wanted = {}
    for name, feature in registry['features'].items():
        for row in feature.get('rows', []):
            if not evidenced.get(name):
                wanted.setdefault(row, set()).add(name)
        if feature.get('option') and feature.get('enforced_by') == 'engine':
            for row in feature['rows']:
                wanted.setdefault(row, set()).add(name)
    for device in registry['devices']:
        for feature, row in device.get('planned_features', {}).items():
            wanted.setdefault(row, set()).add(feature)
    listing = []
    for row in sorted(wanted):
        state, text = rows.get(row, ('?', ''))
        listing.append({'row': row, 'state': state, 'features': sorted(wanted[row]), 'text': text})
    return listing


def roadmap_rows(path=ROOT / 'docs/ROADMAP.md'):
    spec = importlib.util.spec_from_file_location('check_master_plan_dash', ROOT / 'scripts/check-master-plan.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    rows = {}
    for name, cells in module.parse_rows(Path(path).read_text(encoding='utf-8')):
        text = cells[3]
        rows[name] = (cells[2], (text[:220] + '…') if len(text) > 220 else text)
    return rows


STYLE = '''
:root { --bg:#f7f7f5; --panel:#ffffff; --ink:#1d1d1b; --muted:#5f5f5a; --line:#dcdcd6;
  --pass:#1f7a3a; --pass-bg:#e3f3e7; --fail:#b3261e; --fail-bg:#fbe4e2; --cached:#2d6a8f; --cached-bg:#e2eff7;
  --waiting:#6b6b66; --waiting-bg:#eeeeea; --budget:#8a5a00; --budget-bg:#fbf0d9; --accent:#3a4ccc;
  --triage:#a3195f; --triage-bg:#fbe5f0; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) { --bg:#141413; --panel:#1d1d1b; --ink:#ecece8;
  --muted:#a3a39c; --line:#33332f; --pass:#7fd49a; --pass-bg:#173322; --fail:#ff9b92; --fail-bg:#3a1a17;
  --cached:#8cc6ea; --cached-bg:#142a38; --waiting:#b0b0a8; --waiting-bg:#262623; --budget:#f2c26b; --budget-bg:#352a12;
  --accent:#9aa6ff; --triage:#ff8cc6; --triage-bg:#3a1229; } }
:root[data-theme="dark"] { --bg:#141413; --panel:#1d1d1b; --ink:#ecece8; --muted:#a3a39c; --line:#33332f;
  --pass:#7fd49a; --pass-bg:#173322; --fail:#ff9b92; --fail-bg:#3a1a17; --cached:#8cc6ea; --cached-bg:#142a38;
  --waiting:#b0b0a8; --waiting-bg:#262623; --budget:#f2c26b; --budget-bg:#352a12; --accent:#9aa6ff;
  --triage:#ff8cc6; --triage-bg:#3a1229; }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--ink); font:14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width:1400px; margin:0 auto; padding:24px 16px 48px; }
h1 { font-size:22px; margin:0 0 4px; } h2 { font-size:16px; margin:28px 0 8px; }
.sub { color:var(--muted); margin:0 0 16px; }
.banner { background:var(--budget-bg); color:var(--budget); border:1px solid var(--line); padding:8px 12px; border-radius:6px; margin-bottom:16px; }
.tiles { display:grid; grid-template-columns:repeat(auto-fit, minmax(150px, 1fr)); gap:8px; }
.tile { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:10px 12px; }
.tile b { display:block; font-size:20px; } .tile span { color:var(--muted); font-size:12px; }
.scroll { overflow-x:auto; background:var(--panel); border:1px solid var(--line); border-radius:8px; }
table { border-collapse:collapse; width:100%; font-size:12px; }
th, td { border-bottom:1px solid var(--line); padding:4px 6px; text-align:left; vertical-align:top; }
thead th { position:sticky; top:0; background:var(--panel); font-weight:600; white-space:nowrap; }
th.dev { white-space:nowrap; } th.dev small { display:block; color:var(--muted); font-weight:400; }
.cell { display:inline-block; min-width:58px; padding:1px 6px; border-radius:4px; font-weight:600; text-decoration:none; }
.pass { color:var(--pass); background:var(--pass-bg); } .fail { color:var(--fail); background:var(--fail-bg); }
.cached { color:var(--cached); background:var(--cached-bg); } .waiting { color:var(--waiting); background:var(--waiting-bg); }
.budget { color:var(--budget); background:var(--budget-bg); } .triage { color:var(--triage); background:var(--triage-bg); }
.na { color:var(--line); }
.meter { height:10px; background:var(--waiting-bg); border-radius:5px; overflow:hidden; margin-top:6px; }
.meter i { display:block; height:100%; background:var(--accent); }
svg.trend { vertical-align:middle; } svg.trend rect.p { fill:var(--pass); } svg.trend rect.f { fill:var(--fail); }
ul.plain { padding-left:18px; } code { font-size:12px; }
'''


def cell_html(cell):
    if cell is None:
        return '<span class="na" aria-label="not applicable">·</span>'
    state = cell['state']
    title = html.escape(cell.get('reason') or ('build %s' % cell['build'] if cell.get('build') else state), quote=True)
    label = {'triage': 'needs triage'}.get(state, state)
    if cell.get('log'):
        return '<a class="cell %s" href="%s" title="%s">%s</a>' % (state, html.escape(cell['log'], quote=True), title,
                                                                   label)
    return '<span class="cell %s" title="%s">%s</span>' % (state, title, label)


def trend_svg(points):
    points = sorted(points)[-12:]
    if not points:
        return ''
    bars = ''.join('<rect class="%s" x="%d" y="0" width="5" height="12"><title>build %s: %s</title></rect>' % (
        'p' if outcome == 'pass' else 'f', i * 7, build, outcome) for i, (build, outcome) in enumerate(points))
    return '<svg class="trend" width="%d" height="12" role="img" aria-label="last %d runs">%s</svg>' % (
        len(points) * 7, len(points), bars)


def render(registry, results, planned=None, rows=None, budget=None, fixture=False, cells_hook=None):
    """(page, document). `cells_hook` takes and returns the cell list ({device, feature, state, ...}) before anything
    is counted or drawn: the coordinator passes the gold verdicts (gold.overlay), so this page and the web UI's QUAL
    page always agree."""
    rows = rows or {}
    cells, history = matrix(registry, results, planned)
    if cells_hook is not None:
        listed = cells_hook([dict(c, device=d, feature=f) for (d, f), c in cells.items()])
        cells = {(c['device'], c['feature']): {k: v for k, v in c.items() if k not in ('device', 'feature')}
                 for c in listed}
    features = list(registry['features'])
    used = [f for f in features if any(k[1] == f for k in cells) or any(f in d['features'] for d in registry['devices'])]
    counts = {state: 0 for state in ('pass', 'fail', 'triage', 'cached', 'waiting', 'budget')}
    for cell in cells.values():
        counts[cell['state']] += 1
    budget = budget or (planned or {}).get('budget') or {}
    cap = float(budget.get('cap_usd', registry['budget']['monthly_cap_usd']))
    spent = float(budget.get('spent_usd', 0.0))
    out = ['<!doctype html><html lang="en"><head><meta charset="utf-8">',
           '<meta name="viewport" content="width=device-width, initial-scale=1">',
           '<title>Cerid QUAL dashboard</title><style>%s</style></head><body><main>' % STYLE,
           '<h1>Cerid QUAL dashboard</h1>',
           '<p class="sub">Devices &times; features from the result store, the build plan and '
           '<code>ci/devices.json</code>. Cached means the same input key passed before; budget, waiting and needs '
           'triage cells are never passes.</p>']
    if fixture:
        out.append('<p class="banner">Fixture data: rendered from <code>ci/dashboard/fixtures/results.json</code>, '
                   'not from a CI run.</p>')
    out.append('<div class="tiles">')
    for state in ('pass', 'fail', 'triage', 'cached', 'waiting', 'budget'):
        out.append('<div class="tile"><b class="%s">%d</b><span>%s cells</span></div>' % (
            state, counts[state], {'triage': 'needs triage'}.get(state, state)))
    ratio = min(spent / cap, 1.0) if cap else 0.0
    out.append('<div class="tile"><b>%.2f / %.0f USD</b><span>spent this month (%s)</span>'
               '<div class="meter"><i style="width:%.1f%%"></i></div></div>' % (spent, cap, html.escape(
                   str(budget.get('month', ''))), ratio * 100))
    out.append('</div>')
    out.append('<h2>Matrix</h2><div class="scroll"><table><thead><tr><th>Device</th><th>Tier</th><th>Trend</th>')
    out += ['<th title="%s">%s</th>' % (html.escape(', '.join(registry['features'][f].get('rows', [])), quote=True),
                                        html.escape(registry['features'][f]['title'])) for f in used]
    out.append('</tr></thead><tbody>')
    for device in registry['devices']:
        gpu = device['gpu']
        out.append('<tr><th class="dev" scope="row">%s<small>%s %s · %s · %s</small></th><td>%s</td><td>%s</td>' % (
            html.escape(device['id']), html.escape(device['os']['name']), html.escape(device['os']['version']),
            html.escape(device['cpu']['arch']), html.escape(gpu['model']), device['tier'],
            trend_svg(history.get(device['id'], []))))
        for feature in used:
            out.append('<td>%s</td>' % cell_html(cells.get((device['id'], feature))))
        out.append('</tr>')
    out.append('</tbody></table></div>')
    regressed = regressions(cells)
    out.append('<h2>Regressions</h2>')
    if regressed:
        out.append('<ul class="plain">' + ''.join('<li><b>%s</b> · %s %s</li>' % (
            html.escape(d), html.escape(registry['features'][f]['title']), cell_html(cells[(d, f)])) for d, f in regressed)
                   + '</ul>')
    else:
        out.append('<p class="sub">None: no cell that passed in its previous run fails now.</p>')
    out.append('<h2>Roadmap clauses without evidence</h2><div class="scroll"><table><thead><tr><th>Row</th>'
               '<th>State</th><th>Waiting features</th><th>Clause</th></tr></thead><tbody>')
    for item in clauses_without_evidence(registry, cells, rows):
        out.append('<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>' % (
            html.escape(item['row']), html.escape(item['state']), html.escape(', '.join(item['features'])),
            html.escape(item['text'])))
    out.append('</tbody></table></div>')
    out.append('<h2>Console constraints waiting on engine rows</h2><ul class="plain">')
    for name, feature in registry['features'].items():
        if feature.get('enforced_by') == 'engine':
            out.append('<li><b>%s</b>: %s (%s)</li>' % (html.escape(feature['title']), html.escape(feature['option']),
                                                        html.escape(', '.join(feature['rows']))))
    out.append('</ul></main></body></html>\n')
    document = {'cells': [{'device': d, 'feature': f, **c} for (d, f), c in sorted(cells.items())],
                'regressions': [{'device': d, 'feature': f} for d, f in regressed],
                'budget': {'cap_usd': cap, 'spent_usd': spent}}
    return ''.join(out), document


def write(out_dir, page, document):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / 'index.html').write_text(page, encoding='utf-8', newline='\n')
    (out_dir / 'matrix.json').write_text(json.dumps(document, indent=1, sort_keys=True) + '\n', encoding='utf-8',
                                         newline='\n')


def publish(site, environ=None, run=subprocess.run):
    """Deploy `site` to Cloudflare Pages with the locked wrangler on the pinned Node. Without the account the page
    stays a build artifact; nothing else is tried."""
    environ = os.environ if environ is None else environ
    token_file = environ.get('CERID_CLOUDFLARE_TOKEN_FILE')
    account = environ.get('CERID_CLOUDFLARE_ACCOUNT_ID')
    project = environ.get('CERID_CI_PAGES_PROJECT', 'cerid-qual')
    if not token_file or not account or not Path(token_file).is_file():
        print('Cloudflare Pages is not configured (CERID_CLOUDFLARE_TOKEN_FILE, CERID_CLOUDFLARE_ACCOUNT_ID); '
              'the page stays a build artifact')
        return 0
    pins = load_script_pins()
    node_pin = pins.entry('tools', 'node-linux-x64')
    work = Path(tempfile.mkdtemp(prefix='cerid-pages-'))
    archive = pins.fetch(node_pin, work)
    with tarfile.open(archive) as package:
        package.extractall(work, filter='data')
    node_root = work / node_pin['member'].split('/')[0]
    env = dict(environ)
    env['PATH'] = str(node_root / 'bin') + os.pathsep + env.get('PATH', '')
    env['CLOUDFLARE_ACCOUNT_ID'] = account
    env['CLOUDFLARE_API_TOKEN'] = Path(token_file).read_text(encoding='utf-8').strip()
    run([str(node_root / 'bin/npm'), 'ci', '--ignore-scripts', '--no-audit', '--no-fund'], cwd=str(HERE), env=env,
        check=True)
    run([str(node_root / 'bin/node'), str(HERE / 'node_modules/wrangler/bin/wrangler.js'), 'pages', 'deploy',
         str(site), '--project-name', project, '--branch', environ.get('CERID_CI_BRANCH', 'main'),
         '--commit-dirty=true'], cwd=str(HERE), env=env, check=True)
    return 0


def load_script_pins():
    spec = importlib.util.spec_from_file_location('pins_dash', ROOT / 'scripts/pins.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    ren = sub.add_parser('render', help='render the page to a directory')
    ren.add_argument('--fixture', nargs='?', const=str(FIXTURE), help='render from fixture records')
    ren.add_argument('--store', help='result store URL (default: CERID_CI_STORE)')
    ren.add_argument('--plan', help='build plan JSON')
    ren.add_argument('--out', required=True)
    pub = sub.add_parser('publish', help='render from the store and deploy to Cloudflare Pages')
    pub.add_argument('--plan')
    pub.add_argument('--out', default=str(ROOT / 'build/ci-dashboard'))
    args = parser.parse_args(argv)
    pipeline = load_pipeline()
    registry = pipeline.load_registry()
    planned = None
    if args.plan:
        if not Path(args.plan).is_file() and os.environ.get('CERID_CI'):
            subprocess.run(pipeline.agent_tool('artifact', 'download', args.plan, args.plan), check=False)
        if Path(args.plan).is_file():
            planned = json.loads(Path(args.plan).read_text(encoding='utf-8'))
    fixture = getattr(args, 'fixture', None)
    budget = None
    if fixture:
        data = json.loads(Path(fixture).read_text(encoding='utf-8'))
        results = data['results']
        planned = planned or data.get('plan')
        budget = data.get('budget')
    else:
        store = pipeline.open_store(getattr(args, 'store', None))
        results = results_from_store(store)
        if planned is None:
            month = pipeline.month_of()
            budget = {'cap_usd': registry['budget']['monthly_cap_usd'], 'spent_usd': pipeline.month_spend(store, month),
                      'month': month}
    page, document = render(registry, results, planned, roadmap_rows(), budget, fixture=bool(fixture))
    write(args.out, page, document)
    print('Rendered %d cells into %s' % (len(document['cells']), args.out))
    if args.command == 'publish':
        return publish(Path(args.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
