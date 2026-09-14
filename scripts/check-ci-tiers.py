#!/usr/bin/env python3
"""Validate the CI tier mapping against the presets and the workflow.

Contract: docs/design/ci-tiers.md. Every visible configure preset has exactly one owner in
`.github/ci-tiers.json`; every owning job reads its preset list from the preflight job, is gated on a non-empty
list, and is a dependency of the required aggregate; the class table ends with a catch-all so every changed path
resolves to a tier; cancellation stays limited to superseded pull-request runs.
"""
from pathlib import Path
import argparse
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TIERS = ROOT / '.github/ci-tiers.json'
DEFAULT_PRESETS = ROOT / 'CMakePresets.json'
DEFAULT_WORKFLOW = ROOT / '.github/workflows/ci.yml'
TIER_ORDER = ('preflight', 'change', 'complete')
PRESET_TIERS = ('change', 'complete', 'diagnostic')
FIXED_JOBS = ('preflight', 'repository', 'required')


def output_key(job):
    """GITHUB_OUTPUT key carrying a job's preset list."""
    return 'presets_' + job.replace('-', '_')


def load_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def visible_presets(presets_document):
    return [p['name'] for p in presets_document['configurePresets'] if not p.get('hidden')]


def workflow_jobs(text):
    """Split the workflow text into {job id: block text}; job ids sit at two-space indentation under `jobs:`."""
    start = text.find('\njobs:\n')
    if start < 0:
        return {}
    body = text[start + len('\njobs:\n'):]
    blocks = {}
    current = None
    for line in body.splitlines(keepends=True):
        match = re.match(r'^  ([A-Za-z0-9_-]+):\s*$', line)
        if match:
            current = match.group(1)
            blocks[current] = ''
        elif current is not None:
            blocks[current] += line
    return blocks


def validate(tiers, presets_document, workflow_text):
    problems = []
    if tiers.get('schema') != 'cerid-ci-tiers/1':
        problems.append('unknown schema %r' % tiers.get('schema'))
    if tuple(tiers.get('tiers', ())) != TIER_ORDER:
        problems.append('tiers must be %s' % list(TIER_ORDER))
    visible = visible_presets(presets_document)
    entries = tiers.get('presets', {})
    jobs = tiers.get('jobs', {})
    for name in visible:
        if name not in entries:
            problems.append('visible preset %s has no tier owner' % name)
    for name, entry in entries.items():
        if name not in visible:
            problems.append('tier entry %s is not a visible configure preset' % name)
        tier = entry.get('tier')
        if tier not in PRESET_TIERS:
            problems.append('preset %s has tier %r; expected one of %s' % (name, tier, list(PRESET_TIERS)))
        if tier == 'diagnostic':
            if 'job' in entry:
                problems.append('diagnostic preset %s must not name a job' % name)
            if re.search(r'--preset\s+%s\b' % re.escape(name), workflow_text):
                problems.append('diagnostic preset %s is invoked by the workflow' % name)
        elif entry.get('job') not in jobs:
            problems.append('preset %s names unknown job %r' % (name, entry.get('job')))
    for job in FIXED_JOBS:
        if job in jobs:
            problems.append('job %s is fixed by the workflow and cannot own presets' % job)
    for job, spec in jobs.items():
        if not spec.get('runner'):
            problems.append('job %s has no runner' % job)
        if not any(entry.get('job') == job for entry in entries.values()):
            problems.append('job %s owns no preset' % job)

    blocks = workflow_jobs(workflow_text)
    for job in FIXED_JOBS:
        if job not in blocks:
            problems.append('workflow lacks the %s job' % job)
    required = blocks.get('required', '')
    needs = re.search(r'^\s+needs:\s*\[([^\]]*)\]', required, re.M)
    required_needs = {item.strip() for item in needs.group(1).split(',')} if needs else set()
    if 'if: always()' not in required:
        problems.append('required job must run with if: always()')
    for job in ('preflight', 'repository'):
        if job not in required_needs:
            problems.append('required job must need %s' % job)
    for job in jobs:
        if job not in required_needs:
            problems.append('required job must need %s' % job)
        block = blocks.get(job)
        if block is None:
            problems.append('workflow lacks job %s' % job)
            continue
        key = output_key(job)
        if not re.search(r'^\s+needs:\s*preflight\s*$', block, re.M):
            problems.append('job %s must need preflight' % job)
        if ("if: needs.preflight.outputs.%s != '[]'" % key) not in block:
            problems.append("job %s must be gated on needs.preflight.outputs.%s != '[]'" % (job, key))
        if ('fromJSON(needs.preflight.outputs.%s)' % key) not in block:
            problems.append('job %s must take its matrix from fromJSON(needs.preflight.outputs.%s)' % (job, key))
        if 'runs-on: %s' % jobs[job]['runner'] not in block:
            problems.append('job %s must run on %s' % (job, jobs[job]['runner']))
    for name, entry in entries.items():
        test = entry.get('test')
        if test and not re.search(r'--preset\s+%s\b' % re.escape(test), blocks.get(entry.get('job', ''), '')):
            problems.append('preset %s declares test preset %s, which its job never invokes' % (name, test))
    preflight = blocks.get('preflight', '')
    for job in jobs:
        if ('%s: ${{ steps.tier.outputs.%s }}' % (output_key(job), output_key(job))) not in preflight:
            problems.append('preflight must publish output %s' % output_key(job))
    for key in ('tier', 'expected'):
        if ('%s: ${{ steps.tier.outputs.%s }}' % (key, key)) not in preflight:
            problems.append('preflight must publish output %s' % key)

    classes = tiers.get('classes', [])
    if not classes:
        problems.append('no path classes')
    for cls in classes:
        if cls.get('tier') not in TIER_ORDER:
            problems.append('class %r has tier %r' % (cls.get('name'), cls.get('tier')))
        if not cls.get('patterns'):
            problems.append('class %r has no patterns' % cls.get('name'))
    if classes and '**' not in classes[-1].get('patterns', []):
        problems.append('the last class must be the catch-all (**)')

    triggers = re.search(r'^on:\n((?:  .*\n|\n)+)', workflow_text, re.M)
    trigger_text = triggers.group(1) if triggers else ''
    for trigger in ('push:', 'pull_request:', 'schedule:', 'workflow_dispatch:'):
        if ('  ' + trigger) not in trigger_text:
            problems.append('workflow must define the %s trigger' % trigger[:-1])
    cancel = re.search(r'^\s+cancel-in-progress:\s*(.*)$', workflow_text, re.M)
    if not re.search(r'^concurrency:\s*$', workflow_text, re.M) or cancel is None:
        problems.append('workflow must declare a concurrency group with cancel-in-progress')
    elif "github.event_name == 'pull_request'" not in cancel.group(1) or cancel.group(1).strip() == 'true':
        problems.append('cancel-in-progress must be limited to pull_request runs')
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--tiers', default=str(DEFAULT_TIERS))
    parser.add_argument('--presets', default=str(DEFAULT_PRESETS))
    parser.add_argument('--workflow', default=str(DEFAULT_WORKFLOW))
    args = parser.parse_args(argv)
    tiers = load_json(args.tiers)
    presets = load_json(args.presets)
    workflow = Path(args.workflow).read_text(encoding='utf-8')
    problems = validate(tiers, presets, workflow)
    for problem in problems:
        print('FAIL: ' + problem)
    if problems:
        return 1
    owned = [n for n, e in tiers['presets'].items() if e['tier'] != 'diagnostic']
    print('Checked %d visible presets: %d owned by %d jobs, %d diagnostic; %d path classes.'
          % (len(visible_presets(presets)), len(owned), len(tiers['jobs']),
             len(tiers['presets']) - len(owned), len(tiers['classes'])))
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
