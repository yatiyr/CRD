#!/usr/bin/env python3
"""Resolve the CI tier of a run and conclude the required result.

`resolve` classifies the changed paths between a base and a head revision with the class table in
`.github/ci-tiers.json`, picks the highest tier any class asks for (scheduled and manual runs take the complete tier,
an unknown base takes the complete tier) and writes the expected jobs and every job's preset list as workflow outputs.
`conclude` reads the `needs` context of the required job and fails unless preflight succeeded, every expected job
succeeded and no unexpected job failed. Contract: docs/design/ci-tiers.md.
"""
from pathlib import Path
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TIERS = ROOT / '.github/ci-tiers.json'
TIER_ORDER = ('preflight', 'change', 'complete')
ALWAYS_EXPECTED = ('repository',)


def output_key(job):
    return 'presets_' + job.replace('-', '_')


def load_tiers(path=DEFAULT_TIERS):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def glob_regex(pattern):
    """`**` matches any run of path segments, `*` matches inside one segment, everything else is literal."""
    out = ''
    i = 0
    while i < len(pattern):
        if pattern.startswith('**/', i):
            out += '(?:.*/)?'
            i += 3
        elif pattern.startswith('**', i):
            out += '.*'
            i += 2
        elif pattern[i] == '*':
            out += '[^/]*'
            i += 1
        else:
            out += re.escape(pattern[i])
            i += 1
    return re.compile('^' + out + '$')


def classify(paths, classes):
    """Map each path to the first class whose patterns match it; returns [(path, class name, tier)]."""
    compiled = [(cls['name'], cls['tier'], [glob_regex(p) for p in cls['patterns']]) for cls in classes]
    result = []
    for path in paths:
        normalized = path.replace('\\', '/').strip('/')
        for name, tier, regexes in compiled:
            if any(r.match(normalized) for r in regexes):
                result.append((normalized, name, tier))
                break
        else:
            result.append((normalized, None, 'complete'))
    return result


def changed_paths(base, head, repo=ROOT):
    """Paths changed between two revisions, or None when the comparison is not possible."""
    if not base or not head or re.fullmatch(r'0+', base):
        return None
    try:
        completed = subprocess.run(['git', 'diff', '--name-only', base, head], cwd=str(repo), capture_output=True,
                                   text=True, encoding='utf-8', check=False)
    except OSError:
        return None
    if completed.returncode != 0:
        return None
    return [line for line in completed.stdout.splitlines() if line.strip()]


def selection(tiers, tier):
    """Presets each job runs for a tier, in registration order; every job appears, possibly empty."""
    rank = TIER_ORDER.index(tier)
    jobs = {job: [] for job in tiers['jobs']}
    for name, entry in tiers['presets'].items():
        if entry['tier'] == 'diagnostic':
            continue
        if TIER_ORDER.index(entry['tier']) <= rank:
            jobs[entry['job']].append(name)
    return jobs


def resolve(tiers, event, base=None, head=None, requested=None, paths=None):
    """Return the resolution record for one run."""
    classified = []
    if event in ('schedule',):
        tier, reason = 'complete', 'scheduled run takes the complete tier'
    elif event == 'workflow_dispatch':
        tier = requested or 'complete'
        if tier not in TIER_ORDER[1:]:
            raise SystemExit('workflow_dispatch tier must be one of %s, not %r' % (list(TIER_ORDER[1:]), requested))
        reason = 'manual run requested the %s tier' % tier
    elif event in ('push', 'pull_request'):
        if paths is None:
            paths = changed_paths(base, head)
        if paths is None:
            tier, reason = 'complete', 'base revision %s is unavailable; conservative complete tier' % (base or '(none)')
        elif not paths:
            tier, reason = 'complete', 'no changed paths between %s and %s; conservative complete tier' % (base, head)
        else:
            classified = classify(paths, tiers['classes'])
            tier = max((t for _, _, t in classified), key=TIER_ORDER.index)
            drivers = sorted({name for _, name, t in classified if t == tier and name})
            reason = '%d changed path(s); class(es) %s ask for the %s tier' % (len(paths), ', '.join(drivers) or '(unclassified)', tier)
    else:
        tier, reason = 'complete', 'event %s takes the complete tier' % event
    jobs = selection(tiers, tier)
    expected = list(ALWAYS_EXPECTED) + [job for job, presets in jobs.items() if presets]
    return {'tier': tier, 'reason': reason, 'expected': expected, 'jobs': jobs,
            'classified': classified, 'event': event, 'base': base, 'head': head}


def write_outputs(record, path):
    lines = ['tier=%s' % record['tier'], 'reason=%s' % record['reason'],
             'expected=%s' % json.dumps(record['expected'])]
    for job, presets in record['jobs'].items():
        lines.append('%s=%s' % (output_key(job), json.dumps(presets)))
    with open(path, 'a', encoding='utf-8') as handle:
        handle.write('\n'.join(lines) + '\n')


def resolution_markdown(record):
    lines = ['### CI tier: %s' % record['tier'], '', record['reason'], '',
             '| Job | Presets |', '|---|---|']
    for job, presets in record['jobs'].items():
        lines.append('| %s | %s |' % (job, ', '.join('`%s`' % p for p in presets) or 'skipped'))
    if record['classified']:
        counts = {}
        for _, name, tier in record['classified']:
            counts.setdefault((name or 'unclassified', tier), []).append(_)
        lines += ['', '| Class | Tier | Paths |', '|---|---|---|']
        for (name, tier), paths in sorted(counts.items()):
            shown = ', '.join('`%s`' % p for p in paths[:6]) + (' …' if len(paths) > 6 else '')
            lines.append('| %s | %s | %d: %s |' % (name, tier, len(paths), shown))
    return '\n'.join(lines) + '\n'


def conclude(needs, expected):
    """Return (ok, rows) for the required aggregate."""
    rows = []
    ok = True
    preflight = needs.get('preflight', {}).get('result')
    if preflight != 'success':
        ok = False
        rows.append(('preflight', preflight or 'absent', 'must succeed'))
    else:
        rows.append(('preflight', 'success', 'ok'))
    if not expected:
        ok = False
        rows.append(('(expected jobs)', 'none', 'preflight published no expectation'))
    for job in expected:
        result = needs.get(job, {}).get('result', 'absent')
        if result != 'success':
            ok = False
        rows.append((job, result, 'expected: ok' if result == 'success' else 'expected but not successful'))
    for job in sorted(needs):
        if job == 'preflight' or job in expected:
            continue
        result = needs[job].get('result')
        if result in ('skipped', 'success'):
            rows.append((job, result, 'not expected'))
        else:
            ok = False
            rows.append((job, result, 'not expected but %s' % result))
    return ok, rows


def conclusion_markdown(ok, rows, tier):
    lines = ['### Required result: %s (tier %s)' % ('PASS' if ok else 'FAIL', tier or '?'), '',
             '| Job | Result | Verdict |', '|---|---|---|']
    lines += ['| %s | %s | %s |' % row for row in rows]
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    res = sub.add_parser('resolve', help='resolve the tier and write workflow outputs')
    res.add_argument('--tiers', default=str(DEFAULT_TIERS))
    res.add_argument('--event', required=True, help='GitHub event name')
    res.add_argument('--base', default='', help='base revision (push: event.before; pull_request: base.sha)')
    res.add_argument('--head', default='HEAD', help='head revision')
    res.add_argument('--tier', default='', help='workflow_dispatch tier input')
    res.add_argument('--changed-files', help='file listing changed paths (instead of git diff)')
    res.add_argument('--output', help='GITHUB_OUTPUT file to append to')
    res.add_argument('--summary', help='GITHUB_STEP_SUMMARY file to append to')
    con = sub.add_parser('conclude', help='evaluate the required aggregate')
    con.add_argument('--needs-env', default='NEEDS', help='environment variable holding toJSON(needs)')
    con.add_argument('--expected-env', default='EXPECTED', help='environment variable holding the expected job list')
    con.add_argument('--summary', help='GITHUB_STEP_SUMMARY file to append to')
    args = parser.parse_args(argv)

    if args.command == 'resolve':
        tiers = load_tiers(args.tiers)
        paths = None
        if args.changed_files:
            paths = [l for l in Path(args.changed_files).read_text(encoding='utf-8').splitlines() if l.strip()]
        record = resolve(tiers, args.event, args.base, args.head, args.tier or None, paths)
        if args.output:
            write_outputs(record, args.output)
        markdown = resolution_markdown(record)
        if args.summary:
            with open(args.summary, 'a', encoding='utf-8') as handle:
                handle.write(markdown)
        print(markdown)
        return 0

    needs = json.loads(os.environ.get(args.needs_env) or '{}')
    expected_raw = os.environ.get(args.expected_env) or ''
    expected = json.loads(expected_raw) if expected_raw else []
    tier = needs.get('preflight', {}).get('outputs', {}).get('tier')
    ok, rows = conclude(needs, expected)
    markdown = conclusion_markdown(ok, rows, tier)
    if args.summary:
        with open(args.summary, 'a', encoding='utf-8') as handle:
            handle.write(markdown)
    print(markdown)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
